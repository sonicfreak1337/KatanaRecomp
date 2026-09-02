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

} // namespace

int main(const int argc, char** const argv) {
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
    test_non_vq_twiddled_mipmap_layout();
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
        katana::runtime::NativePortLoadedAotBinder binder(
            cpu, modules, module_guard, module_ledger);
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
        require(staged_entry.has_value() && !staged_entry->active &&
                    staged_entry->module_sha256 == identity &&
                    staged_entry->block_sha256 == identity &&
                    staged_entry->source_start == source_start &&
                    staged_entry->runtime_start == runtime_start &&
                    staged_entry->module_size == bytes.size() &&
                    staged_entry->source_offset == 0u &&
                    staged_entry->block_size == bytes.size() &&
                    staged_entry->lifecycle_generation == lifecycle &&
                    !binder.active_entry_for_address(runtime_start).has_value(),
                "Read-only AOT-Preflight verlor staged Modul-, Block- oder "
                "Lifecycle-Identitaet.");
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
        require(active.has_value() && active->sha256 == identity &&
                    active->source_start == source_start &&
                    active->runtime_start == runtime_start &&
                    active->byte_size == bytes.size() &&
                    active->lifecycle_generation == lifecycle &&
                    active_entry.has_value() && active_entry->active &&
                    active_entry->module_sha256 == identity &&
                    active_entry->block_sha256 == identity &&
                    active_entry->lifecycle_generation == lifecycle,
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

    // An ordinary guest store may retire one exact dynamic executable owner
    // after the store has completed. A disjoint active module remains valid
    // at the new observer generation, and the retired placement can later be
    // staged and identity-checked again.
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
        const std::array blocks{
            katana::runtime::NativePortLoadedAotBlockIdentityView{
                0u, static_cast<std::uint32_t>(bytes.size()), identity}};
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
                    binder.validate_bound_entry(second_runtime_start),
                "Post-Store-Retirement schloss nicht genau einen dynamischen "
                "Owner oder invalidierte einen disjunkten Survivor.");
        const auto surviving = binder.active_module_for_address(
            second_runtime_start);
        require(surviving.has_value() &&
                    surviving->lifecycle_generation == second_lifecycle,
                "Generation-Rebase veraenderte den Lifecycle des Survivors.");

        cpu.memory.write_bytes(
            0x0C920000u, bytes,
            katana::runtime::CodeWriteSource::Copy);
        const auto replacement_lifecycle = binder.stage_runtime_module(
            {identity, first_source_start, first_runtime_start,
             static_cast<std::uint32_t>(bytes.size())});
        require(replacement_lifecycle > initial_owner_lifecycle &&
                    replacement_lifecycle > second_lifecycle &&
                    binder.bind_entry(first_runtime_start) &&
                    binder.validate_bound_entry(first_runtime_start),
                "Retired dynamischer Owner konnte nicht identity-bound "
                "reacquired werden.");

        cpu.pr = first_runtime_start + 2u;
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
        bindings.activate("closure-runtime-image");
        const auto exact = bindings.active_entry_for_address(runtime_start);
        require(exact.has_value() && exact->image_id == "closure-runtime-image" &&
                    exact->image_sha256 == identity &&
                    exact->block_sha256 == identity &&
                    exact->source_start == source_start &&
                    exact->runtime_start == runtime_start &&
                    exact->source_offset == 0u &&
                    exact->block_size == bytes.size() &&
                    exact->lifecycle_generation != 0u,
                "Aktives Runtime-Image verlor exakte Entry-/Blockidentitaet "
                "oder Generation.");
        require(!bindings.active_entry_for_address(runtime_start + 2u).has_value(),
                "Runtime-Image-Midblock wurde als exakter Closure-Entry akzeptiert.");
        require(bindings.deactivate_runtime_range(runtime_start, bytes.size()) == 1u &&
                    !bindings.active_entry_for_address(runtime_start).has_value(),
                "Retired Runtime-Image blieb als Closure-Entry aktiv.");
    } catch (const std::exception& error) {
        require(false, std::string("Runtime-Image-Entryidentitaet warf: ") +
                           error.what());
    }

    std::cout << "Native-Port-Executable-Lifecycle erfolgreich.\n";
    return EXIT_SUCCESS;
}
