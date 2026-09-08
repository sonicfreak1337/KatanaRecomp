#include "katana/runtime/native_port_content.hpp"
#include "katana/runtime/native_port_aot_runtime.hpp"
#include "katana/runtime/native_port_texture_asset.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void test_non_vq_twiddled_mipmap_layout() {
    using namespace katana::runtime;

    // PVRT data-format 0x02 uses the compact SDK file layout rather than the
    // raw VRAM/0x12 OtherMipPoint layout.  A 16x16 RGB565 chain has 684
    // semantic bytes; a containing PVRT chunk may add separately validated
    // trailing alignment (EFF_REGULAR carries four such bytes).  Use distinct
    // solid levels to pin the compact offsets without depending on private
    // Sonic content at CTest time.
    constexpr std::size_t encoded_bytes = 684u;
    std::vector<std::uint8_t> encoded(encoded_bytes, 0u);
    const auto write_rgb565 = [&](const std::size_t offset,
                                  const std::uint16_t value) {
        require(offset + 1u < encoded.size(),
                "Twiddled-Mipmap-Test schreibt ausserhalb des Payloads.");
        encoded[offset] = static_cast<std::uint8_t>(value & 0xFFu);
        encoded[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto morton_index = [](const std::uint32_t x,
                                 const std::uint32_t y,
                                 const std::uint32_t dimension) {
        std::uint32_t result = 0u;
        for (std::uint32_t bit = 0u; (1u << bit) < dimension; ++bit) {
            result |= ((y >> bit) & 1u) << (bit * 2u);
            result |= ((x >> bit) & 1u) << (bit * 2u + 1u);
        }
        return result;
    };
    const auto fill_level = [&](const std::size_t offset,
                                const std::uint32_t dimension,
                                const std::uint16_t value) {
        for (std::uint32_t y = 0u; y < dimension; ++y) {
            for (std::uint32_t x = 0u; x < dimension; ++x) {
                write_rgb565(
                    offset + static_cast<std::size_t>(morton_index(
                                 x, y, dimension)) * 2u,
                    value);
            }
        }
    };

    write_rgb565(2u, 0xF800u);    // 1x1
    fill_level(4u, 2u, 0x07E0u);  // 2x2
    fill_level(12u, 4u, 0x001Fu); // 4x4
    fill_level(44u, 8u, 0xFFFFu); // 8x8
    fill_level(172u, 16u, 0xFFE0u); // 16x16

    const auto texture = decode_native_port_texture_surface(
        encoded, {16u, 16u}, NativePortTextureAssetPixelFormat::Rgb565,
        NativePortTextureAssetDataFormat::SquareTwiddledMipmaps);
    require(texture.rgba8.size() == 16u * 16u * 4u &&
                texture.lower_mip_levels.size() == 4u,
            "Nicht-VQ-Twiddled-Mipmapkette verliert Top-Level oder Mips.");
    require(texture.rgba8[0] == 0xFFu && texture.rgba8[1] == 0xFFu &&
                texture.rgba8[2] == 0u && texture.rgba8[3] == 0xFFu,
            "Twiddled-Top-Level startet nicht am physischen 16x16-Offset.");
    constexpr std::array<std::array<std::uint8_t, 4u>, 4u> expected{
        std::array<std::uint8_t, 4u>{0xFFu, 0xFFu, 0xFFu, 0xFFu},
        std::array<std::uint8_t, 4u>{0u, 0u, 0xFFu, 0xFFu},
        std::array<std::uint8_t, 4u>{0u, 0xFFu, 0u, 0xFFu},
        std::array<std::uint8_t, 4u>{0xFFu, 0u, 0u, 0xFFu}};
    constexpr std::array<std::uint32_t, 4u> expected_dimensions{
        8u, 4u, 2u, 1u};
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        const auto& level = texture.lower_mip_levels[index];
        require(level.extent.width == expected_dimensions[index] &&
                    level.extent.height == expected_dimensions[index] &&
                    std::equal(level.rgba8.begin(), level.rgba8.begin() + 4,
                               expected[index].begin()),
                "Nicht-VQ-Twiddled-Mip-Offset oder 1x1-Selektor ist falsch.");
    }
}

void test_flycast_texture_memory_contracts() {
    using namespace katana::runtime;
    using Format = NativePortTextureMemoryPixelFormat;
    using Storage = NativePortTextureMemoryStorage;
    const auto hash = [](const std::string_view hex) {
        NativePortTexturePayloadSha256 result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            unsigned byte = 0u;
            const auto parsed = std::from_chars(
                hex.data() + i * 2u, hex.data() + i * 2u + 2u, byte, 16);
            require(parsed.ec == std::errc{}, "Invalid fixture palette SHA");
            result[i] = static_cast<std::uint8_t>(byte);
        }
        return result;
    };
    NativePortTextureMemoryLayout layout;
    layout.extent = {8u, 8u};
    layout.storage = Storage::Twiddled;
    layout.pixel_format = Format::Palette4;
    layout.mipmapped = true;
    std::array<NativePortTexturePaletteColor, 16u> palette{};
    for (std::uint32_t i = 0u; i < palette.size(); ++i)
        palette[i] = {static_cast<std::uint8_t>(i * 17u),
                      static_cast<std::uint8_t>(i * 17u),
                      static_cast<std::uint8_t>(i * 17u), 255u};
    layout.palette_rgba8 = palette;
    layout.palette_rgba8_sha256 =
        hash("a0d6d56263b270be4e975f86f8ac6c4e81b3e34f9804055891951d234c813aae");
    // Flycast's PAL4 converter aliases writes at the 2x2 mip: the final
    // logical pixels use nibbles 0,2,8,10 relative to that level, including
    // bytes of the next authored level. Pin those visible results safely.
    std::vector<std::uint8_t> pal4(44u);
    for (std::size_t i = 0; i < pal4.size(); ++i)
        pal4[i] = static_cast<std::uint8_t>(
            ((i * 2u) & 15u) | (((i * 2u + 1u) & 15u) << 4u));
    const auto decoded = decode_native_port_texture_memory_surface(pal4, layout);
    require(decoded.lower_mip_levels.size() == 3u, "Raw PAL4 mip count");
    const auto& tiny = decoded.lower_mip_levels[1u].rgba8;
    require(tiny[0] == 4u * 17u && tiny[4] == 6u * 17u &&
                tiny[8] == 12u * 17u && tiny[12] == 14u * 17u &&
                decoded.lower_mip_levels[2u].rgba8[0] == 2u * 17u,
            "Raw PAL4 tiny mip differs from final Flycast pixels");

    layout.vector_quantized = true;
    layout.codebook_entries = 256u;
    std::vector<std::uint8_t> pal4_vq(2'058u, 0u);
    std::copy_n(pal4.begin(), 8u, pal4_vq.begin());
    const auto vq = decode_native_port_texture_memory_surface(pal4_vq, layout);
    const auto& vq_tiny = vq.lower_mip_levels[1u].rgba8;
    require(vq_tiny[0] == 0u && vq_tiny[4] == 2u * 17u &&
                vq_tiny[8] == 8u * 17u && vq_tiny[12] == 10u * 17u &&
                vq.lower_mip_levels[2u].rgba8[0] == 10u * 17u,
            "Raw PAL4 VQ tiny-level selector changed");
    auto changed = pal4_vq;
    changed[100u] = 1u; // An unused codebook byte still belongs to this source.
    const auto rebound = decode_native_port_texture_memory_surface(changed, layout);
    require(rebound.rgba8 == vq.rgba8 &&
                rebound.decoded_rgba8_sha256 != vq.decoded_rgba8_sha256,
            "Unused codebook source bytes escaped payload identity");

    layout = {};
    layout.extent = {8u, 8u};
    layout.storage = Storage::Linear;
    layout.pixel_format = Format::Rgb565;
    layout.vector_quantized = true;
    layout.codebook_entries = 256u;
    std::vector<std::uint8_t> linear_vq(2'064u, 0u);
    constexpr std::array<std::uint16_t, 4u> colors{
        0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        linear_vq[i * 2u] = static_cast<std::uint8_t>(colors[i]);
        linear_vq[i * 2u + 1u] = static_cast<std::uint8_t>(colors[i] >> 8u);
    }
    const auto planar = decode_native_port_texture_memory_surface(linear_vq, layout);
    require(planar.rgba8[0] == 255u && planar.rgba8[5] == 255u &&
                planar.rgba8[10] == 255u && planar.rgba8[12] == 255u,
            "Planar VQ lost its 4x1 block or packed-stride default");

    layout = {};
    layout.extent = {8u, 8u};
    layout.pixel_format = Format::Yuv422;
    std::vector<std::uint8_t> yuv(128u);
    for (std::size_t i = 0; i < yuv.size(); i += 4u) {
        yuv[i] = yuv[i + 2u] = 128u;
        yuv[i + 1u] = 10u;
        yuv[i + 3u] = 20u;
    }
    const auto gray = decode_native_port_texture_memory_surface(yuv, layout);
    require(gray.rgba8[0] == 10u && gray.rgba8[1] == 10u &&
                gray.rgba8[4] == 20u && gray.rgba8[7] == 255u,
            "YUV422 pair ordering or neutral chroma changed");
    layout.pixel_format = Format::Bump;
    for (std::size_t i = 0; i < yuv.size(); i += 2u) {
        yuv[i] = 0x34u; yuv[i + 1u] = 0x12u;
    }
    const auto bump = decode_native_port_texture_memory_surface(yuv, layout);
    require(bump.rgba8[0] == 0x22u && bump.rgba8[1] == 0x33u &&
                bump.rgba8[2] == 0x44u && bump.rgba8[3] == 0x11u,
            "Bump angles lost ARGB4444 channel order");
    layout.stride_bytes = 32u;
    bool rejected_stride = false;
    try {
        static_cast<void>(decode_native_port_texture_memory_surface(yuv, layout));
    } catch (const NativePortTextureAssetError& error) {
        rejected_stride = error.failure() == NativePortTextureAssetFailure::InvalidStride;
    }
    require(rejected_stride, "TCW stride admitted a half hardware pixel group");
}

class ChainGuardHost final
    : public katana::runtime::NativePortHostServices {
  public:
    [[nodiscard]] std::uint64_t monotonic_time_nanoseconds()
        const noexcept override {
        return 1u;
    }

    [[nodiscard]] katana::runtime::NativePortLifecycleState
    poll_lifecycle() override {
        return katana::runtime::NativePortLifecycleState::Running;
    }

    void synchronize_simulation_boundary() override {}
    void begin_frame(std::uint64_t) override {}
    void present_frame(std::uint64_t) override {}

    [[nodiscard]] std::uint64_t presented_frames()
        const noexcept override {
        return 0u;
    }
};

[[nodiscard]] bool chain_guard_static_entry(
    const std::uint32_t address) noexcept {
    return address == 0x8C010000u;
}

struct ImmutableGuardBenchmarkInput final {
    std::vector<katana::runtime::NativePortImmutableRange> ranges;
    std::array<std::vector<std::uint32_t>, 3u> queries;
};

[[nodiscard]] ImmutableGuardBenchmarkInput
make_immutable_guard_benchmark_input() {
    constexpr std::size_t page_size = 4096u;
    constexpr std::size_t page_count =
        katana::runtime::native_port_main_memory_backing_size / page_size;
    constexpr std::size_t occupied_page_count = 369u;
    constexpr std::size_t range_count = 4750u;
    constexpr std::size_t query_count = 1u << 18u;

    ImmutableGuardBenchmarkInput input;
    input.ranges.reserve(range_count);
    std::array<bool, page_count> occupied_pages{};
    std::vector<std::uint32_t> occupied_page_indices;
    occupied_page_indices.reserve(occupied_page_count);
    for (std::size_t ordinal = 0u; ordinal < occupied_page_count;
         ++ordinal) {
        const auto page = static_cast<std::uint32_t>(
            (ordinal * 4051u + 137u) & (page_count - 1u));
        occupied_pages[page] = true;
        occupied_page_indices.push_back(page);
        const auto ranges_on_page = 12u + (ordinal < 322u ? 1u : 0u);
        for (std::size_t range = 0u; range < ranges_on_page; ++range) {
            input.ranges.push_back({
                katana::runtime::native_port_main_memory_physical_base +
                    page * static_cast<std::uint32_t>(page_size) +
                    16u + static_cast<std::uint32_t>(range * 16u),
                4u,
                katana::runtime::native_port_immutable_range_mask(
                    katana::runtime::NativePortImmutableRangeKind::Executable)});
        }
    }
    std::sort(input.ranges.begin(), input.ranges.end(),
              [](const auto& left, const auto& right) {
                  return left.physical_address < right.physical_address;
              });

    std::vector<std::uint32_t> empty_page_indices;
    empty_page_indices.reserve(page_count - occupied_page_count);
    for (std::uint32_t page = 0u; page < page_count; ++page) {
        if (!occupied_pages[page]) empty_page_indices.push_back(page);
    }
    for (auto& queries : input.queries) queries.reserve(query_count);
    for (std::size_t index = 0u; index < query_count; ++index) {
        const auto empty_page = empty_page_indices[
            (index * 2654435761u) % empty_page_indices.size()];
        input.queries[0].push_back(
            katana::runtime::native_port_main_memory_physical_base +
            empty_page * static_cast<std::uint32_t>(page_size) +
            static_cast<std::uint32_t>((index * 4u) & (page_size - 4u)));

        const auto occupied_page = occupied_page_indices[
            (index * 2246822519u) % occupied_page_indices.size()];
        const auto occupied_offset =
            (index & 1u) == 0u ? 16u : 2048u;
        input.queries[1].push_back(
            katana::runtime::native_port_main_memory_physical_base +
            occupied_page * static_cast<std::uint32_t>(page_size) +
            occupied_offset);

        const bool use_empty_page = index % 100u < 91u;
        input.queries[2].push_back(
            use_empty_page ? input.queries[0].back()
                           : input.queries[1].back());
    }
    require(input.ranges.size() == range_count,
            "Benchmark bildet die repraesentative Range-Anzahl nicht ab.");
    return input;
}

void run_immutable_guard_benchmark(const std::uint64_t calls) {
    const auto input = make_immutable_guard_benchmark_input();
    katana::runtime::NativePortImmutableWriteGuard guard(input.ranges);
    constexpr std::array<std::string_view, 3u> names{
        "empty", "occupied", "mixed"};
    for (std::size_t mix = 0u; mix < input.queries.size(); ++mix) {
        const auto& queries = input.queries[mix];
        std::uint64_t warmup_checksum = 0u;
        for (const auto address : queries)
            warmup_checksum += guard.tracks_address(address, 4u) ? 1u : 0u;

        std::uint64_t checksum = 0u;
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t call = 0u; call < calls; ++call) {
            const auto address = queries[
                static_cast<std::size_t>(call) & (queries.size() - 1u)];
            checksum += guard.tracks_address(address, 4u) ? 1u : 0u;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count();
        std::cout << "KATANA_IMMUTABLE_PAGE_REJECT_BENCHMARK mix="
                  << names[mix] << " calls=" << calls << " ns=" << elapsed
                  << " checksum=" << checksum
                  << " warmup_checksum=" << warmup_checksum << '\n';
    }
}

// Regression for a host deadline arriving inside Sonic's native frame. This
// pins the generic request/accept boundary without private content or a GPU.
void test_host_stop_rendezvous() {
    using namespace katana::runtime;
    NativePortContext immediate;
    require(!request_native_port_host_stop(immediate, NativePortStopReason::HookAbort) &&
                immediate.pending_host_stop_reason == NativePortStopReason::None &&
                immediate.stop_reason == NativePortStopReason::None,
            "Non-host reason entered host-stop rendezvous.");
    require(request_native_port_host_stop(immediate, NativePortStopReason::HostRequested) &&
                immediate.stop_reason == NativePortStopReason::HostRequested,
            "Unbound provider did not preserve immediate host shutdown.");
    NativePortContext deadline;
    require(request_native_port_host_stop(deadline, NativePortStopReason::HostDeadline),
            "Unbound provider did not accept deadline.");

    struct FrameOwner { bool open = true; bool service_active = false; unsigned checks = 0u; } owner;
    NativePortContext deferred;
    deferred.host_deadline_nanoseconds = 1234u;
    deferred.title_state = &owner;
    deferred.host_stop_ready = [](NativePortContext& context, NativePortStopReason reason) noexcept {
        auto& state = *static_cast<FrameOwner*>(context.title_state);
        ++state.checks;
        return reason == NativePortStopReason::HostDeadline &&
               !state.open && !state.service_active;
    };
    require(!try_accept_native_port_host_stop(deferred) && owner.checks == 0u,
            "Readiness callback ran without a pending request.");
    require(!request_native_port_host_stop(deferred, NativePortStopReason::HostDeadline) &&
                !request_native_port_host_stop(deferred, NativePortStopReason::HostRequested) &&
                deferred.pending_host_stop_reason == NativePortStopReason::HostDeadline &&
                deferred.stop_reason == NativePortStopReason::None,
            "Open frame stopped or second request replaced the first.");
    require(!try_accept_native_port_host_stop(deferred),
            "Nested dispatch polling bypassed frame ownership.");
    owner.open = false; // The provider's ordinary completed-frame boundary.
    owner.service_active = true;
    require(!try_accept_native_port_host_stop(deferred),
            "Nested completed frame unwound an unfinished host-owned AOT service.");
    owner.service_active = false;
    require(try_accept_native_port_host_stop(deferred) &&
                deferred.stop_reason == NativePortStopReason::HostDeadline &&
                deferred.host_deadline_nanoseconds == 1234u,
            "Completed frame did not accept the original immutable deadline.");
    const auto accepted_checks = owner.checks;
    require(request_native_port_host_stop(deferred, NativePortStopReason::HostRequested) &&
                deferred.stop_reason == NativePortStopReason::HostDeadline &&
                owner.checks == accepted_checks,
            "Accepted host stop was replaced or consulted the provider again.");

    NativePortContext failure;
    failure.stop_reason = NativePortStopReason::AotContractViolation;
    failure.host_stop_ready = [](NativePortContext&, NativePortStopReason) noexcept -> bool {
        std::abort(); // Real errors must not consult a shutdown provider.
    };
    require(!request_native_port_host_stop(failure, NativePortStopReason::HostDeadline) &&
                !try_accept_native_port_host_stop(failure) &&
                failure.stop_reason == NativePortStopReason::AotContractViolation,
            "Host request overwrote an existing contract failure.");
    NativePortContext callback_failure;
    callback_failure.host_stop_ready = [](NativePortContext& context, NativePortStopReason) noexcept {
        context.stop_reason = NativePortStopReason::HookAbort;
        return true;
    };
    require(!request_native_port_host_stop(callback_failure, NativePortStopReason::HostRequested) &&
                callback_failure.stop_reason == NativePortStopReason::HookAbort,
            "Readiness-time failure lost priority to host shutdown.");
}

void test_aot_services_host_stop_rendezvous() {
    using namespace katana::runtime;
    struct Host final : NativePortHostServices {
        std::uint64_t now = 1u;
        NativePortLifecycleState lifecycle = NativePortLifecycleState::Running;
        bool frame_open = true;
        bool service_active = false;
        std::uint64_t presents = 0u;
        std::uint64_t monotonic_time_nanoseconds() const noexcept override { return now; }
        NativePortLifecycleState poll_lifecycle() override { return lifecycle; }
        void synchronize_simulation_boundary() override {}
        void begin_frame(std::uint64_t) override { frame_open = true; }
        void present_frame(std::uint64_t) override { frame_open = false; ++presents; }
        std::uint64_t presented_frames() const noexcept override { return presents; }
    };
    struct Fixture {
        CpuState cpu;
        Host host;
        NativePortContext context;
        std::array<NativePortImmutableRange, 1u> ranges{{
            {0x0C010000u, 2u,
             native_port_immutable_range_mask(NativePortImmutableRangeKind::Executable)}}};
        NativePortImmutableWriteGuard guard{ranges};
        std::optional<NativePortAotServices> services;
        Fixture(std::uint64_t deadline = 0u,
                NativePortLifecycleState lifecycle = NativePortLifecycleState::Running,
                bool bind_provider = true,
                NativePortStopReason error = NativePortStopReason::None) {
            host.lifecycle = lifecycle;
            context.cpu = &cpu;
            context.host = &host;
            context.host_deadline_nanoseconds = deadline;
            context.stop_reason = error;
            if (bind_provider)
                context.host_stop_ready = [](NativePortContext& ctx, NativePortStopReason) noexcept {
                    const auto& owner = *static_cast<Host*>(ctx.host);
                    return !owner.frame_open && !owner.service_active;
                };
            services.emplace(context, chain_guard_static_entry, guard);
        }
        NativePortBlockCompletion complete(std::uint64_t cycles = 1u, bool exception = false) {
            cpu.pending_guest_cycles += cycles;
            return finalize_guest_block(cpu, *services, 1u, 0u, 0u, exception);
        }
        void present() {
            host.present_frame(context.frame_index++);
            static_cast<void>(try_accept_native_port_host_stop(context));
        }
    };
    {
        Fixture f(1u);
        require(f.context.stop_reason == NativePortStopReason::None &&
                    f.context.pending_host_stop_reason == NativePortStopReason::HostDeadline &&
                    !f.services->poll_interrupt().has_value() &&
                    f.services->can_chain_executable_block(0x8C010000u),
                "AOT constructor bypassed the open-frame deadline rendezvous.");
        for (unsigned i = 0u; i < 3u; ++i) {
            const auto result = f.complete();
            require(!result.interrupt.has_value() && result.scheduler.processed_boundaries == 0u &&
                        f.context.stop_reason == NativePortStopReason::None,
                    "AOT completed-block deadline unwound an open frame.");
        }
        require(f.cpu.total_guest_cycles == 3u && f.cpu.pending_guest_cycles == 0u,
                "Deferred host stop changed guest-cycle accounting.");
        f.host.service_active = true;
        f.present();
        require(!f.complete().interrupt.has_value(),
                "AOT deadline unwound an unfinished native service after present.");
        f.host.service_active = false;
        require(try_accept_native_port_host_stop(f.context), "Natural service boundary did not accept stop.");
        require(f.services->poll_interrupt() == NativePortStopReason::HostDeadline &&
                    f.complete(0u).interrupt == NativePortStopReason::HostDeadline &&
                    !f.services->can_chain_executable_block(0x8C010000u) &&
                    f.context.host_deadline_nanoseconds == 1u,
                "Accepted frame stop required another cycle poll or changed deadline.");
        require(!f.complete(0u, true).interrupt.has_value(),
                "Host stop overrode a new guest exception at block completion.");
        f.host.lifecycle = NativePortLifecycleState::Running;
        require(f.services->consume_guest_cycles(0u, 1024u).processed_boundaries == 1u,
                "Accepted stop was absent from effective boundary accounting.");
    }
    {
        Fixture f(10u);
        f.host.now = 10u;
        require(!f.complete().interrupt.has_value() &&
                    f.context.pending_host_stop_reason == NativePortStopReason::HostDeadline,
                "Budgeted AOT deadline failed to request deferred shutdown.");
        f.host.lifecycle = NativePortLifecycleState::Shutdown;
        require(!f.complete().interrupt.has_value(), "Second host request unwound pending frame.");
        f.present();
        require(f.services->poll_interrupt() == NativePortStopReason::HostDeadline,
                "Shutdown replaced the first pending deadline.");
    }
    {
        Fixture f(10u);
        f.host.lifecycle = NativePortLifecycleState::Shutdown;
        require(!f.complete().interrupt.has_value() &&
                    f.context.pending_host_stop_reason == NativePortStopReason::HostRequested,
                "AOT lifecycle bypassed the open-frame rendezvous.");
        f.host.now = 10u;
        f.host.lifecycle = NativePortLifecycleState::Paused;
        require(!f.complete().interrupt.has_value(), "Pending pause published an AOT unwind signal.");
        // Also exercise acceptance by the real budgeted runtime refresh,
        // without a direct helper call from the completed-frame fixture.
        f.host.present_frame(f.context.frame_index++);
        require(f.complete().interrupt == NativePortStopReason::HostRequested &&
                    f.services->poll_interrupt() == NativePortStopReason::HostRequested,
                "Later deadline replaced the first lifecycle stop.");
    }
    {
        Fixture f(0u, NativePortLifecycleState::Paused);
        require(f.services->poll_interrupt() == NativePortStopReason::None &&
                    !f.services->can_chain_executable_block(0x8C010000u),
                "Normal pause lost its nonterminal engaged-None yield.");
        f.host.lifecycle = NativePortLifecycleState::Running;
        require(!f.complete().interrupt.has_value(), "Resume retained a stale pause boundary.");
        f.host.lifecycle = NativePortLifecycleState::Paused;
        require(f.complete().interrupt == NativePortStopReason::None, "Pause was not sampled.");
        require(!request_native_port_host_stop(f.context, NativePortStopReason::HostRequested),
                "Open paused frame accepted stop early.");
        require(!f.complete(0u).interrupt.has_value() &&
                    f.services->can_chain_executable_block(0x8C010000u) &&
                    f.services->consume_guest_cycles(0u, 1024u).processed_boundaries == 0u &&
                    !f.complete().interrupt.has_value(),
                "Cached pause overrode pending stop before or after the poll budget.");
        f.present();
        require(f.complete(0u).interrupt == NativePortStopReason::HostRequested,
                "Paused frame completion failed to publish accepted stop.");
    }
    {
        Fixture f(1u, NativePortLifecycleState::Shutdown, true,
                  NativePortStopReason::AotContractViolation);
        require(f.context.stop_reason == NativePortStopReason::AotContractViolation &&
                    f.services->poll_interrupt() == NativePortStopReason::AotContractViolation &&
                    !f.services->can_chain_executable_block(0x8C010000u) &&
                    f.complete().interrupt == NativePortStopReason::AotContractViolation,
                "AOT host polling replaced or hid a preexisting real error.");
    }
    {
        Fixture f(1u);
        f.context.stop_reason = NativePortStopReason::HookAbort;
        require(f.complete(0u).interrupt == NativePortStopReason::HookAbort &&
                    f.complete().interrupt == NativePortStopReason::HookAbort &&
                    !f.services->can_chain_executable_block(0x8C010000u),
                "Failure during pending shutdown lost priority.");
    }
    {
        Fixture f(1u, NativePortLifecycleState::Running, false);
        require(f.services->poll_interrupt() == NativePortStopReason::HostDeadline,
                "Unbound AOT provider did not preserve immediate deadline acceptance.");
        f.context.stop_reason = NativePortStopReason::AotContractViolation;
        require(f.complete(0u).interrupt == NativePortStopReason::AotContractViolation,
                "Cached accepted host stop masked a subsequent real failure.");
    }
    {
        Fixture f(0u, NativePortLifecycleState::Shutdown, false);
        require(f.services->poll_interrupt() == NativePortStopReason::HostRequested,
                "Unbound AOT provider did not preserve immediate lifecycle shutdown.");
    }
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--host-stop-only") {
        test_host_stop_rendezvous();
        test_aot_services_host_stop_rendezvous();
        std::cout << "Native host-stop rendezvous passed.\n";
        return EXIT_SUCCESS;
    }
    if (argc == 3 &&
        std::string_view(argv[1]) == "--benchmark-immutable-page-reject") {
        std::uint64_t calls = 0u;
        const auto end = argv[2] + std::char_traits<char>::length(argv[2]);
        const auto parsed = std::from_chars(argv[2], end, calls);
        require(parsed.ec == std::errc{} && parsed.ptr == end && calls != 0u,
                "Benchmark-Aufrufzahl ist ungueltig.");
        run_immutable_guard_benchmark(calls);
        return EXIT_SUCCESS;
    }
    require(argc == 1, "Unbekannte Native-Port-Content-Testoption.");
    test_host_stop_rendezvous();
    test_aot_services_host_stop_rendezvous();
    test_non_vq_twiddled_mipmap_layout();
    test_flycast_texture_memory_contracts();
    const std::array immutable_ranges{
        katana::runtime::NativePortImmutableRange{
            0x0C000000u, 2u,
            katana::runtime::native_port_immutable_range_mask(
                katana::runtime::NativePortImmutableRangeKind::Executable)}};
    katana::runtime::NativePortImmutableWriteGuard immutable_guard(
        immutable_ranges);

    constexpr std::uint32_t guard_page_size = 4096u;
    const std::array page_index_ranges{
        katana::runtime::NativePortImmutableRange{
            katana::runtime::native_port_main_memory_physical_base +
                guard_page_size - 2u,
            4u,
            katana::runtime::native_port_immutable_range_mask(
                katana::runtime::NativePortImmutableRangeKind::Executable)},
        katana::runtime::NativePortImmutableRange{
            katana::runtime::native_port_main_memory_physical_base +
                3u * guard_page_size + 16u,
            4u,
            katana::runtime::native_port_immutable_range_mask(
                katana::runtime::NativePortImmutableRangeKind::ReadOnlyImage)},
        katana::runtime::NativePortImmutableRange{
            0x1F000000u, 4u,
            katana::runtime::native_port_immutable_range_mask(
                katana::runtime::NativePortImmutableRangeKind::Executable)}};
    katana::runtime::NativePortImmutableWriteGuard page_index_guard(
        page_index_ranges);
    require(
        page_index_guard.tracks_address(
            katana::runtime::native_port_main_memory_physical_base +
                guard_page_size - 4u,
            8u) &&
            page_index_guard.tracks_address(0x8C000FFEu, 4u) &&
            !page_index_guard.tracks_address(
                katana::runtime::native_port_main_memory_physical_base +
                    2u * guard_page_size + 32u,
                4u) &&
            !page_index_guard.tracks_address(
                katana::runtime::native_port_main_memory_physical_base +
                    3u * guard_page_size + 2048u,
                4u) &&
            page_index_guard.tracks_address(
                katana::runtime::native_port_main_memory_physical_base +
                    3u * guard_page_size + 16u,
                4u) &&
            page_index_guard.tracks_address(0x1F000000u, 4u),
        "Negativer Page-Reject verlor Seitenrand, Alias, belegte Seite "
        "oder Baseline ausserhalb des Main-RAM-Backings.");

    // Sonic's AOT stores overwhelmingly hit empty RAM pages. The early
    // rejection must remain exact for every direct segment and RAM mirror,
    // including the last byte before a backing discontinuity.
    for (const auto segment : {0x00000000u, 0x80000000u, 0xA0000000u}) {
        for (const auto mirror : {0x0C000000u, 0x0D000000u, 0x0E000000u, 0x0F000000u}) {
            const auto base = segment | mirror;
            require(!page_index_guard.tracks_address(base + 0x2FFCu, 4u) &&
                        page_index_guard.tracks_address(base + 0x2FFCu, 24u) &&
                        !page_index_guard.tracks_address(base + 0xFFFFFCu, 4u) &&
                        page_index_guard.tracks_address(base + 0xFFFFFCu, 8u),
                    "Empty-page early rejection crossed an occupied page or RAM mirror.");
        }
    }

    {
        katana::runtime::CpuState chain_cpu;
        ChainGuardHost chain_host;
        katana::runtime::NativePortContext chain_context;
        chain_context.cpu = &chain_cpu;
        chain_context.host = &chain_host;
        katana::runtime::NativePortImmutableWriteGuard chain_guard(
            immutable_ranges);
        katana::runtime::NativePortAotServices chain_services(
            chain_context, chain_guard_static_entry, chain_guard);
        require(chain_services.aot_contract_valid() &&
                    chain_services.can_chain_executable_block(0x8C010000u) &&
                    !chain_services.can_chain_executable_block(0x8C010002u),
                "Vollstaendiger AOT-Observer-Vertrag laesst keinen exakten "
                "statischen Chain-Entry zu.");
        chain_cpu.memory.clear_guest_write_batch_observer();
        require(!chain_services.aot_contract_valid() &&
                    !chain_services.can_chain_executable_block(0x8C010000u),
                "Retired Batch-Observer stoppt AOT-Chaining nicht fail-closed.");
        chain_cpu.memory.clear_guest_write_observer();
    }

    immutable_guard.reserve_additional_runtime_executable_ranges(3u);
    immutable_guard.add_runtime_executable_range(0x8C900000u, 0x100u);
    immutable_guard.add_runtime_executable_range(0x8C900200u, 0x80u);
    immutable_guard.validate_runtime_executable_range_present(
        0x0C900000u, 0x100u);
    require(immutable_guard.tracks_address(0x0C900000u, 0x100u) &&
                immutable_guard.tracks_address(0x8C900200u, 0x80u),
            "Verzoegerter Guard-Rebuild verlor registrierte Ranges.");
    immutable_guard.remove_runtime_executable_range_committed(
        0x0C900000u, 0x100u);
    require(!immutable_guard.tracks_address(0x8C900000u, 0x100u) &&
                immutable_guard.tracks_address(0x0C900200u, 0x80u),
            "Guard-Retirement verlor verbleibende Range oder blieb aktiv.");
    immutable_guard.observe_write(
        {0x8C900200u, 2u, katana::runtime::CodeWriteSource::Cpu, true});
    require(immutable_guard.write_detected() &&
                immutable_guard.first_write_address() == 0x0C900200u,
            "Noexcept Write-Observer materialisierte Dirty-Guard nicht.");
    immutable_guard.remove_runtime_executable_range_committed(
        0x0C900200u, 0x80u);
    require(!immutable_guard.tracks_address(0x8C900200u, 2u),
            "Letztes Runtime-Range-Retirement liess den Page-Index stale.");

    {
        katana::runtime::NativePortImmutableWriteGuard multiple_write_guard(
            immutable_ranges);
        multiple_write_guard.reserve_additional_runtime_executable_ranges(2u);
        multiple_write_guard.add_runtime_executable_range(
            0x0C900000u, 4u);
        multiple_write_guard.add_runtime_executable_range(
            0x0C910000u, 4u);
        multiple_write_guard.observe_write(
            {0x0C900000u, 2u,
             katana::runtime::CodeWriteSource::Cpu, true});
        multiple_write_guard.observe_write(
            {0x0C910000u, 2u,
             katana::runtime::CodeWriteSource::Cpu, true});
        require(
            multiple_write_guard.first_write_kind_mask() !=
                katana::runtime::native_port_immutable_range_mask(
                    katana::runtime::NativePortImmutableRangeKind::Executable),
            "Mehrere Executable-Stores wurden als einzelner Ownerbeweis "
            "beibehalten.");
    }

    katana::runtime::NativePortExecutableLifecycleLedger lifecycle_ledger(2u);
    const auto first_lifecycle = lifecycle_ledger.acquire(0x8C900000u, 0x1000u);
    require(first_lifecycle != 0u,
            "Der gemeinsame Executable-Lifecycle vergibt keine Generation.");

    bool overlap_failed = false;
    try {
        static_cast<void>(
            lifecycle_ledger.acquire(0x0C900800u, 0x1000u));
    } catch (const katana::runtime::NativePortContractError&) {
        overlap_failed = true;
    }
    require(overlap_failed,
            "Aliasierende Runtime-Image/AOT-Ranges werden nicht gemeinsam abgewiesen.");

    const auto retirement = lifecycle_ledger.release(first_lifecycle);
    const auto replacement = lifecycle_ledger.acquire(0x0C900000u, 0x1000u);
    require(retirement > first_lifecycle && replacement > retirement,
            "Executable-Aktivierung und Retirement sind nicht monoton generationiert.");
    static_cast<void>(lifecycle_ledger.release(replacement));

    try {
        constexpr std::uint32_t source_start = 0x80800000u;
        constexpr std::uint32_t runtime_start = 0x8C900000u;
        constexpr std::string_view identity =
            "sha256:7af85194466a76bee16168ca8152d4560bd9bec17ade2525f267ed49a54f36a9";
        const std::array<std::uint8_t, 4u> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
        const std::array source_bindings{
            katana::runtime::NativePortLoadedAotSourceBindingView{
                katana::runtime::NativePortLoadedAotSourceTransform::SegaPrs,
                identity, 0u, static_cast<std::uint32_t>(bytes.size()), 0u}};
        const std::array blocks{
            katana::runtime::NativePortLoadedAotBlockIdentityView{
                0u, static_cast<std::uint32_t>(bytes.size()), identity}};
        const std::array modules{
            katana::runtime::NativePortLoadedAotModuleView{
                source_start, static_cast<std::uint32_t>(bytes.size()),
                identity, source_bindings, blocks}};
        katana::runtime::NativePortMemory memory;
        auto& cpu = memory.cpu();
        cpu.memory.write_bytes(
            0x0C900000u, bytes,
            katana::runtime::CodeWriteSource::Copy);
        katana::runtime::NativePortImmutableWriteGuard module_guard(
            immutable_ranges);
        katana::runtime::NativePortExecutableLifecycleLedger module_ledger(1u);
        constexpr std::string_view pack_identity =
            "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
        const auto module_universe_identity =
            katana::runtime::native_port_loaded_aot_module_universe_identity(
                modules);
        katana::runtime::NativePortLoadedAotBinder binder(
            cpu, modules, module_guard, module_ledger,
            module_universe_identity, pack_identity);
        const auto initial_dispatch_stamp = binder.dispatch_stamp();
        require(module_universe_identity.starts_with("sha256:") &&
                    module_universe_identity.size() == 71u &&
                    binder.module_universe_identity() ==
                        module_universe_identity &&
                    binder.aot_pack_identity() == pack_identity &&
                    initial_dispatch_stamp.module_universe_identity ==
                        module_universe_identity &&
                    initial_dispatch_stamp.aot_pack_identity == pack_identity,
                "Loaded-AOT-Binder verlor die kanonische Moduluniversums- "
                "oder AOT-Pack-Identitaet in seinem Dispatch-Stamp.");
        const auto unplaced = binder.resolve_prs_module_source(
            identity, 0u, bytes.size());
        require(unplaced.has_value() && unplaced->sha256 == identity &&
                    unplaced->source_start == source_start &&
                    unplaced->runtime_start == 0u &&
                    unplaced->byte_size == bytes.size() &&
                    !binder.resolve_prs_module_source(
                               identity, bytes.size(), bytes.size())
                         .has_value(),
                "Exactes PRS-Modul ohne statische Placement-Authority wurde "
                "nicht fuer die authentifizierte Loader-Platzierung erhalten.");
        auto loader_bound = *unplaced;
        loader_bound.runtime_start = runtime_start;
        const auto lifecycle = binder.stage_runtime_module(loader_bound);
        const auto staged_entry =
            binder.preflight_entry_for_address(runtime_start);
        const auto staged_inventory =
            binder.modules_for_development_state();
        require(staged_entry.has_value() && !staged_entry->active &&
                    staged_entry->module_sha256 == identity &&
                    staged_entry->block_sha256 == identity &&
                    staged_entry->source_start == source_start &&
                    staged_entry->runtime_start == runtime_start &&
                    staged_entry->module_size == bytes.size() &&
                    staged_entry->source_offset == 0u &&
                    staged_entry->block_size == bytes.size() &&
                    staged_entry->lifecycle_generation == lifecycle &&
                    staged_inventory.size() == 1u &&
                    staged_inventory.front().sha256 == identity &&
                    staged_inventory.front().activation_entry ==
                        runtime_start &&
                    staged_inventory.front().lifecycle_generation ==
                        lifecycle &&
                    !staged_inventory.front().active &&
                    !binder.active_entry_for_address(runtime_start).has_value(),
                "Read-only AOT-Preflight verlor staged Modul-, Block- oder "
                "Lifecycle-Identitaet.");
        binder.validate_development_state_module(loader_bound, runtime_start);
        require(binder.bind_entry(runtime_start),
                "Geladenes AOT-Testmodul wurde nicht aktiviert.");
        require(binder.validate_bound_entry(runtime_start) &&
                    binder.validate_bound_entry(0x0C900000u),
                "Geladenes AOT-Entry-Bitmap verlor Exact- oder Alias-Entry.");
        bool midblock_failed = false;
        try {
            static_cast<void>(
                binder.validate_bound_entry(runtime_start + 2u));
        } catch (const katana::runtime::NativePortContractError&) {
            midblock_failed = true;
        }
        require(midblock_failed,
                "Geladenes AOT-Entry-Bitmap akzeptierte Midblock-Ziel.");
        const auto active = binder.active_module_for_address(
            0x0C900002u);
        const auto active_entry =
            binder.active_entry_for_address(0x0C900000u);
        const auto active_inventory =
            binder.modules_for_development_state();
        require(active.has_value() && active->sha256 == identity &&
                    active->source_start == source_start &&
                    active->runtime_start == runtime_start &&
                    active->byte_size == bytes.size() &&
                    active->lifecycle_generation == lifecycle &&
                    active_entry.has_value() && active_entry->active &&
                    active_entry->module_sha256 == identity &&
                    active_entry->block_sha256 == identity &&
                    active_entry->lifecycle_generation == lifecycle &&
                    active_inventory.size() == 1u &&
                    active_inventory.front().active &&
                    active_inventory.front().activation_entry ==
                        runtime_start,
                "Aktive Modulidentitaet verliert Alias, Range oder Generation.");
        require(!binder.active_module_for_address(0x8C800000u).has_value(),
                "Ungebundene Adresse wurde einem geladenen Modul zugeordnet.");
        module_guard.observe_write(
            {runtime_start, 2u, katana::runtime::CodeWriteSource::Cpu, true});
        bool generation_failed = false;
        try {
            static_cast<void>(binder.validate_bound_entry(runtime_start));
        } catch (const katana::runtime::NativePortContractError&) {
            generation_failed = true;
        }
        require(generation_failed,
                "Geladenes AOT-Entry-Bitmap umging Guard-Invalidierung.");
        require(binder.deactivate_runtime_range(runtime_start, bytes.size()) ==
                        1u &&
                    !binder.validate_bound_entry(runtime_start) &&
                    !binder.preflight_entry_for_address(runtime_start)
                         .has_value() &&
                    !binder.active_entry_for_address(runtime_start)
                         .has_value(),
                "Retired geladenes AOT-Entry blieb dispatchbar.");
    } catch (const std::exception& error) {
        require(false, std::string("Aktive Modulidentitaet warf: ") +
                           error.what());
    }

    // An ordinary guest store quarantines only the exact generated block
    // ranges it overlaps after the store has completed. Unchanged blocks in
    // the same lifecycle and a disjoint active module remain valid at the new
    // observer generation. Restoring an exact block identity can reacquire
    // its already generated entry without retiring or restaging the module.
    try {
        constexpr std::uint32_t first_source_start = 0x80820000u;
        constexpr std::uint32_t second_source_start = 0x80830000u;
        constexpr std::uint32_t first_runtime_start = 0x8C920000u;
        constexpr std::uint32_t second_runtime_start = 0x8C930000u;
        constexpr std::string_view identity =
            "sha256:7af85194466a76bee16168ca8152d4560bd9bec17ade2525f267ed49a54f36a9";
        const std::array<std::uint8_t, 4u> bytes{
            0x09u, 0x00u, 0x0Bu, 0x00u};
        const std::array first_source_bindings{
            katana::runtime::NativePortLoadedAotSourceBindingView{
                katana::runtime::NativePortLoadedAotSourceTransform::Identity,
                identity, 0u, static_cast<std::uint32_t>(bytes.size()), 0u}};
        const std::array second_source_bindings{
            katana::runtime::NativePortLoadedAotSourceBindingView{
                katana::runtime::NativePortLoadedAotSourceTransform::Identity,
                identity, 4u, static_cast<std::uint32_t>(bytes.size()), 0u}};
        constexpr std::string_view first_block_identity =
            "sha256:a2c4aed1cf757cd9a509734a267ffc7b1166b55f4c8f9c3e3550c56e743328fc";
        constexpr std::string_view second_block_identity =
            "sha256:cbe2268747c9c8072c7f9926f2288f270637dc55bb9d14d3368361d5e47d25be";
        const std::array blocks{
            katana::runtime::NativePortLoadedAotBlockIdentityView{
                0u, 2u, first_block_identity},
            katana::runtime::NativePortLoadedAotBlockIdentityView{
                2u, 2u, second_block_identity}};
        const std::array modules{
            katana::runtime::NativePortLoadedAotModuleView{
                first_source_start, static_cast<std::uint32_t>(bytes.size()),
                identity, first_source_bindings, blocks},
            katana::runtime::NativePortLoadedAotModuleView{
                second_source_start, static_cast<std::uint32_t>(bytes.size()),
                identity, second_source_bindings, blocks}};
        const std::array<katana::runtime::NativePortRuntimeImageView, 0u>
            no_images{};
        katana::runtime::NativePortMemory memory;
        auto& cpu = memory.cpu();
        cpu.memory.write_bytes(
            0x0C920000u, bytes,
            katana::runtime::CodeWriteSource::Copy);
        cpu.memory.write_bytes(
            0x0C930000u, bytes,
            katana::runtime::CodeWriteSource::Copy);
        katana::runtime::NativePortImmutableWriteGuard module_guard(
            immutable_ranges);
        katana::runtime::NativePortExecutableLifecycleLedger module_ledger(2u);
        katana::runtime::NativePortRuntimeImageBindings runtime_images(
            cpu, no_images, module_guard, module_ledger);
        katana::runtime::NativePortLoadedAotBinder binder(
            cpu, modules, module_guard, module_ledger);
        const auto initial_owner_lifecycle = binder.stage_runtime_module(
            {identity, first_source_start, first_runtime_start,
             static_cast<std::uint32_t>(bytes.size())});
        const auto second_lifecycle = binder.stage_runtime_module(
            {identity, second_source_start, second_runtime_start,
             static_cast<std::uint32_t>(bytes.size())});
        require(binder.bind_entry(first_runtime_start) &&
                    binder.bind_entry(second_runtime_start),
                "Dynamische Executable-Write-Fixture aktiviert ihre Owner nicht.");

        ChainGuardHost host;
        katana::runtime::NativePortContext context;
        context.cpu = &cpu;
        context.host = &host;
        context.runtime_images = &runtime_images;
        context.loaded_aot = &binder;
        katana::runtime::NativePortAotServices services(
            context, chain_guard_static_entry, module_guard);
        cpu.memory.write_u16(
            0x0C920000u, 0u,
            katana::runtime::CodeWriteSource::Cpu);
        require(services.immutable_write_detected() &&
                    services.reconcile_runtime_executable_write() &&
                    !services.immutable_write_detected() &&
                    !binder.validate_bound_entry(first_runtime_start) &&
                    binder.validate_bound_entry(first_runtime_start + 2u) &&
                    binder.validate_bound_entry(second_runtime_start),
                "Post-Store-Reconciliation quarantinierte nicht exakt den "
                "betroffenen Block oder invalidierte Survivors.");
        const auto same_owner = binder.active_module_for_address(
            first_runtime_start);
        const auto surviving = binder.active_module_for_address(
            second_runtime_start);
        require(same_owner.has_value() &&
                    same_owner->lifecycle_generation ==
                        initial_owner_lifecycle &&
                    surviving.has_value() &&
                    surviving->lifecycle_generation == second_lifecycle,
                "Blockquarantaene pensionierte einen Modul-Lifecycle.");

        cpu.memory.write_bytes(
            0x0C920000u, std::span(bytes).first(2u),
            katana::runtime::CodeWriteSource::Copy);
        require(binder.bind_entry(first_runtime_start) &&
                    binder.validate_bound_entry(first_runtime_start) &&
                    binder.active_module_for_address(first_runtime_start)
                            ->lifecycle_generation ==
                        initial_owner_lifecycle,
                "Restaurierter Block wurde nicht im bestehenden Lifecycle "
                "identity-bound reaktiviert.");

        cpu.pr = first_runtime_start;
        cpu.memory.write_u16(
            0x0C920000u, 0u,
            katana::runtime::CodeWriteSource::Cpu);
        require(services.immutable_write_detected() &&
                    !services.reconcile_runtime_executable_write() &&
                    services.immutable_write_detected(),
                "Live Continuation wurde nach Executable-Write nicht "
                "fail-closed gehalten.");
        cpu.pr = 0u;
    } catch (const std::exception& error) {
        require(false, std::string("Executable-Write-Reconciliation warf: ") +
                           error.what());
    }

    // A fixed immutable executable range is never converted into a dynamic
    // lifecycle retirement, even when empty runtime owners are bound.
    try {
        constexpr std::uint32_t fixed_address = 0x0C940000u;
        const std::array fixed_ranges{
            katana::runtime::NativePortImmutableRange{
                fixed_address, 4u,
                katana::runtime::native_port_immutable_range_mask(
                    katana::runtime::NativePortImmutableRangeKind::Executable)}};
        const std::array<katana::runtime::NativePortRuntimeImageView, 0u>
            no_images{};
        const std::array<katana::runtime::NativePortLoadedAotModuleView, 0u>
            no_modules{};
        katana::runtime::NativePortMemory memory;
        auto& cpu = memory.cpu();
        cpu.memory.write_u16(
            fixed_address, 0x0009u,
            katana::runtime::CodeWriteSource::Copy);
        katana::runtime::NativePortImmutableWriteGuard fixed_guard(
            fixed_ranges);
        katana::runtime::NativePortExecutableLifecycleLedger fixed_ledger(0u);
        katana::runtime::NativePortRuntimeImageBindings runtime_images(
            cpu, no_images, fixed_guard, fixed_ledger);
        katana::runtime::NativePortLoadedAotBinder binder(
            cpu, no_modules, fixed_guard, fixed_ledger);
        ChainGuardHost host;
        katana::runtime::NativePortContext context;
        context.cpu = &cpu;
        context.host = &host;
        context.runtime_images = &runtime_images;
        context.loaded_aot = &binder;
        katana::runtime::NativePortAotServices services(
            context, chain_guard_static_entry, fixed_guard);
        cpu.memory.write_u16(
            fixed_address, 0u,
            katana::runtime::CodeWriteSource::Cpu);
        require(services.immutable_write_detected() &&
                    !services.reconcile_runtime_executable_write() &&
                    services.immutable_write_detected(),
                "Fixed immutable Executable-Write wurde als dynamischer "
                "Lifecycle akzeptiert.");
    } catch (const std::exception& error) {
        require(false, std::string("Fixed-Write-Negativtest warf: ") +
                           error.what());
    }

    // Closure probes may retain a fixed runtime-image dispatch only at an
    // exact generated block boundary of the currently active immutable
    // generation. Merely landing inside the image or the block is not an
    // identity proof, and retirement removes the entry immediately.
    try {
        constexpr std::uint32_t source_start = 0x80810000u;
        constexpr std::uint32_t runtime_start = 0x8C910000u;
        constexpr std::string_view identity =
            "sha256:7af85194466a76bee16168ca8152d4560bd9bec17ade2525f267ed49a54f36a9";
        const std::array<std::uint8_t, 4u> bytes{
            0x09u, 0x00u, 0x0Bu, 0x00u};
        const std::array blocks{
            katana::runtime::NativePortLoadedAotBlockIdentityView{
                0u, static_cast<std::uint32_t>(bytes.size()), identity}};
        const std::array images{
            katana::runtime::NativePortRuntimeImageView{
                "closure-runtime-image", source_start, runtime_start,
                static_cast<std::uint32_t>(bytes.size()), identity, blocks}};
        katana::runtime::NativePortMemory memory;
        auto& cpu = memory.cpu();
        cpu.memory.write_bytes(
            0x0C910000u, bytes, katana::runtime::CodeWriteSource::Copy);
        katana::runtime::NativePortImmutableWriteGuard image_guard(
            immutable_ranges);
        katana::runtime::NativePortExecutableLifecycleLedger image_ledger(1u);
        katana::runtime::NativePortRuntimeImageBindings bindings(
            cpu, images, image_guard, image_ledger);
        const auto inactive_stamp = bindings.dispatch_stamp();
        // Sonic quickload must validate the saved owner before the unrelated
        // live level is retired. Prove exact aliases against saved RAM while
        // this image is inactive and its live bytes deliberately differ.
        const auto saved_image = katana::runtime::capture_native_port_main_memory(cpu);
        cpu.memory.write_u16(0x0C910000u, 0u, katana::runtime::CodeWriteSource::Copy);
        for (const auto entry : {0x0C910000u, 0x8C910000u, 0xAC910000u})
            static_cast<void>(bindings.validate_development_state_image(
                "closure-runtime-image", entry, saved_image));
        bool saved_midblock_rejected = false;
        try {
            static_cast<void>(bindings.validate_development_state_image(
                "closure-runtime-image", runtime_start + 2u, saved_image));
        } catch (const katana::runtime::NativePortContractError&) {
            saved_midblock_rejected = true;
        }
        require(saved_midblock_rejected && bindings.dispatch_stamp() == inactive_stamp &&
                    bindings.active_images_for_development_state().empty() &&
                    cpu.memory.read_u16(0x0C910000u) == 0u,
                "Saved-entry preflight used live bytes, accepted a midblock, or changed ownership.");
        cpu.memory.write_bytes(0x0C910000u, bytes, katana::runtime::CodeWriteSource::Copy);
        require(bindings.recognizes_image("closure-runtime-image") &&
                    !bindings.recognizes_image("unknown-runtime-image"),
                "Runtime-Image-Entwicklungszustand verliert seine feste "
                "Image-Identitaet.");
        bindings.activate("closure-runtime-image");
        const auto active_stamp = bindings.dispatch_stamp();
        const auto active_inventory =
            bindings.active_images_for_development_state();
        const auto exact = bindings.active_entry_for_address(runtime_start);
        require(exact.has_value() && exact->image_id == "closure-runtime-image" &&
                    exact->image_sha256 == identity &&
                    exact->block_sha256 == identity &&
                    exact->source_start == source_start &&
                    exact->runtime_start == runtime_start &&
                    exact->source_offset == 0u &&
                    exact->block_size == bytes.size() &&
                    exact->lifecycle_generation != 0u &&
                    active_inventory.size() == 1u &&
                    active_inventory.front().image_id ==
                        "closure-runtime-image" &&
                    active_inventory.front().runtime_start == runtime_start &&
                    active_inventory.front().byte_size == bytes.size() &&
                    active_inventory.front().lifecycle_generation ==
                        exact->lifecycle_generation,
                "Aktives Runtime-Image verlor exakte Entry-/Blockidentitaet "
                "oder Generation.");
        require(!bindings.active_entry_for_address(runtime_start + 2u).has_value(),
                "Runtime-Image-Midblock wurde als exakter Closure-Entry akzeptiert.");
        require(active_stamp != inactive_stamp &&
                    bindings.deactivate_runtime_range(
                        runtime_start, bytes.size()) == 1u &&
                    bindings.dispatch_stamp() != active_stamp &&
                    !bindings.active_entry_for_address(runtime_start)
                         .has_value(),
                "Retired Runtime-Image blieb als Closure-Entry aktiv.");
    } catch (const std::exception& error) {
        require(false, std::string("Runtime-Image-Entryidentitaet warf: ") +
                           error.what());
    }

    // Development-state restore validates every fixed immutable byte before
    // publishing the all-RAM copy. Mutable bytes may move backwards; a stale
    // executable byte rejects the complete transaction without partial RAM.
    // Sonic r276 exposed the missing production write observer here: it denies
    // raw mutable pointers even after dynamic executable owners retire.
    try {
        katana::runtime::NativePortMemory memory;
        auto& cpu = memory.cpu();
        constexpr std::uint32_t immutable_address = 0x0C000100u;
        constexpr std::uint32_t mutable_address = 0x0C000200u;
        cpu.memory.write_u8(
            immutable_address, 0x5Au,
            katana::runtime::CodeWriteSource::Copy);
        cpu.memory.write_u8(
            mutable_address, 0x11u,
            katana::runtime::CodeWriteSource::Copy);
        auto saved = katana::runtime::capture_native_port_main_memory(cpu);
        saved[0x200u] = 0x77u;
        saved[0xFEu] = 0x33u;
        saved[0x102u] = 0x44u;
        const std::array fixed{
            katana::runtime::NativePortImmutableRange{
                immutable_address, 1u,
                katana::runtime::native_port_immutable_range_mask(
                    katana::runtime::
                        NativePortImmutableRangeKind::ReadOnlyImage)},
            // Unsorted, overlapping and mirrored ranges share one backing.
            katana::runtime::NativePortImmutableRange{
                0x0D000100u, 2u,
                katana::runtime::native_port_immutable_range_mask(
                    katana::runtime::NativePortImmutableRangeKind::Executable)},
            katana::runtime::NativePortImmutableRange{
                0x0C0000FFu, 3u,
                katana::runtime::native_port_immutable_range_mask(
                    katana::runtime::NativePortImmutableRangeKind::Executable)}};
        // The production guard consumes a sorted canonical union. The restore
        // metadata above deliberately describes the same backing via aliases.
        const std::array guard_ranges{
            katana::runtime::NativePortImmutableRange{
                0x0C0000FFu, 3u,
                katana::runtime::native_port_immutable_range_mask(
                    katana::runtime::NativePortImmutableRangeKind::ReadOnlyImage)}};
        katana::runtime::NativePortImmutableWriteGuard restore_guard(guard_ranges);
        std::size_t observed_writes = 0u;
        cpu.memory.set_guest_write_observer(
            [&](const katana::runtime::GuestWriteEvent& event) {
                ++observed_writes;
                restore_guard.observe_write(event);
            }, katana::runtime::GuestWriteObserverContract::StableForPrevalidatedLinearWrites);
        require(cpu.memory.direct_linear_memory_guard(true).write_bytes == nullptr,
                "Sonic restore test must retain the production write observer.");
        const auto before_preflight = katana::runtime::capture_native_port_main_memory(cpu);
        katana::runtime::validate_native_port_main_memory_for_development_state(
            cpu, saved, fixed);
        require(observed_writes == 0u &&
                    katana::runtime::capture_native_port_main_memory(cpu) == before_preflight,
                "Sonic snapshot preflight modified live RAM or notified a write observer.");
        katana::runtime::restore_native_port_main_memory_for_development_state(
            cpu, saved, fixed);
        const auto restored =
            katana::runtime::capture_native_port_main_memory(cpu);
        require(restored == saved && observed_writes > 0u && !restore_guard.write_detected(),
                "Sonic restore lost RAM bytes, bypassed its observer, or wrote fixed code.");
        const auto initial_writes = observed_writes;
        katana::runtime::restore_native_port_main_memory_for_development_state(cpu, saved, fixed);
        require(observed_writes == initial_writes && !restore_guard.write_detected(),
                "Unchanged restored RAM must not publish spurious writes.");
        auto invalid = restored;
        invalid[0x100u] = 0xA5u;
        bool preflight_rejected = false;
        try {
            katana::runtime::validate_native_port_main_memory_for_development_state(
                cpu, invalid, fixed);
        } catch (const katana::runtime::NativePortContractError&) {
            preflight_rejected = true;
        }
        require(preflight_rejected && observed_writes == initial_writes &&
                    katana::runtime::capture_native_port_main_memory(cpu) == restored,
                "Sonic snapshot preflight did not reject immutable drift without mutation.");
        bool rejected = false;
        try {
            katana::runtime::
                restore_native_port_main_memory_for_development_state(
                    cpu, invalid, fixed);
        } catch (const katana::runtime::NativePortContractError&) {
            rejected = true;
        }
        require(rejected &&
                    katana::runtime::capture_native_port_main_memory(cpu) ==
                        restored && observed_writes == initial_writes && !restore_guard.write_detected(),
                "Development-State-Restore akzeptierte Immutable-Drift oder "
                "schrieb RAM teilweise.");
    } catch (const std::exception& error) {
        require(false, std::string("Development-State-RAM-Restore warf: ") +
                           error.what());
    }

    std::cout << "Native-Port-Executable-Lifecycle erfolgreich.\n";
    return EXIT_SUCCESS;
}
