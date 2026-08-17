#include "katana/runtime/native_port_content.hpp"

#include "katana/runtime/block_abi.hpp"
#include "katana/runtime/native_port_aot_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::array<std::uint32_t, 64u> sha256_round_constants{
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u};

class Sha256 final {
  public:
    void update(const std::span<const std::uint8_t> bytes) {
        if (finalized_ ||
            bytes.size() > std::numeric_limits<std::uint64_t>::max() -
                               total_bytes_)
            throw std::runtime_error("native-port-image-sha256-state");
        total_bytes_ += bytes.size();
        auto* cursor = bytes.data();
        auto remaining = bytes.size();
        if (buffer_size_ != 0u) {
            const auto copied =
                std::min(buffer_.size() - buffer_size_, remaining);
            std::memcpy(buffer_.data() + buffer_size_, cursor, copied);
            buffer_size_ += copied;
            cursor += copied;
            remaining -= copied;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_.data());
                buffer_size_ = 0u;
            }
        }
        while (remaining >= buffer_.size()) {
            transform(cursor);
            cursor += buffer_.size();
            remaining -= buffer_.size();
        }
        if (remaining != 0u) {
            std::memcpy(buffer_.data(), cursor, remaining);
            buffer_size_ = remaining;
        }
    }

    [[nodiscard]] std::string finish() {
        if (finalized_)
            throw std::runtime_error("native-port-image-sha256-finalized");
        finalized_ = true;
        const auto bit_count = total_bytes_ * 8u;
        buffer_[buffer_size_++] = 0x80u;
        if (buffer_size_ > 56u) {
            std::fill(buffer_.begin() +
                          static_cast<std::ptrdiff_t>(buffer_size_),
                      buffer_.end(),
                      std::uint8_t{0u});
            transform(buffer_.data());
            buffer_size_ = 0u;
        }
        std::fill(buffer_.begin() +
                      static_cast<std::ptrdiff_t>(buffer_size_),
                  buffer_.begin() + 56,
                  std::uint8_t{0u});
        for (std::size_t index = 0u; index < 8u; ++index)
            buffer_[63u - index] =
                static_cast<std::uint8_t>(bit_count >> (index * 8u));
        transform(buffer_.data());
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto word : state_) output << std::setw(8) << word;
        return output.str();
    }

  private:
    void transform(const std::uint8_t* const block) noexcept {
        std::array<std::uint32_t, 64u> words{};
        for (std::size_t index = 0u; index < 16u; ++index) {
            const auto offset = index * 4u;
            words[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24u) |
                (static_cast<std::uint32_t>(block[offset + 1u]) << 16u) |
                (static_cast<std::uint32_t>(block[offset + 2u]) << 8u) |
                static_cast<std::uint32_t>(block[offset + 3u]);
        }
        for (std::size_t index = 16u; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15u], 7) ^
                            std::rotr(words[index - 15u], 18) ^
                            (words[index - 15u] >> 3u);
            const auto s1 = std::rotr(words[index - 2u], 17) ^
                            std::rotr(words[index - 2u], 19) ^
                            (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0 +
                           words[index - 7u] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = std::tuple{
            state_[0], state_[1], state_[2], state_[3],
            state_[4], state_[5], state_[6], state_[7]};
        for (std::size_t index = 0u; index < words.size(); ++index) {
            const auto sigma1 =
                std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto first = h + sigma1 + ((e & f) ^ (~e & g)) +
                               sha256_round_constants[index] + words[index];
            const auto sigma0 =
                std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto second = sigma0 + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8u> state_{
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
    std::array<std::uint8_t, 64u> buffer_{};
    std::size_t buffer_size_ = 0u;
    std::uint64_t total_bytes_ = 0u;
    bool finalized_ = false;
};

[[nodiscard]] bool path_is_within(const std::filesystem::path& path,
                                  const std::filesystem::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != "..";
}

[[nodiscard]] bool unsafe_filesystem_link(
    const std::filesystem::path& path,
    const std::filesystem::file_status& status) {
    if (std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
#else
    static_cast<void>(path);
    return false;
#endif
}

void require_safe_existing_path(
    const std::filesystem::path& path,
    const bool final_component_must_be_regular_file) {
    const auto normalized =
        std::filesystem::absolute(path).lexically_normal();
    if (normalized.empty() || normalized.root_path().empty())
        throw NativePortContractError(
            NativePortContractFailure::ContentLoadFailed,
            "content-path-root");
    auto current = normalized.root_path();
    std::size_t component_index = 0u;
    for (const auto& component : normalized.relative_path()) {
        if (component.empty() || component == ".") continue;
        ++component_index;
        if (component == "..")
            throw NativePortContractError(
                NativePortContractFailure::ContentLoadFailed,
                "content-path-parent");
        current /= component;
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(current, status_error);
        const bool final = current == normalized;
        const bool unsafe = unsafe_filesystem_link(current, status);
        const bool expects_regular_file =
            final && final_component_must_be_regular_file;
        const bool wrong_type =
            expects_regular_file
                ? !std::filesystem::is_regular_file(status)
                : !std::filesystem::is_directory(status);
        if (status_error || unsafe || wrong_type) {
            std::ostringstream detail;
            detail << "content-path-component:" << component_index
                   << ":final=" << (final ? 1 : 0)
                   << ":expected="
                   << (expects_regular_file ? "file" : "directory")
                   << ":regular="
                   << (std::filesystem::is_regular_file(status) ? 1 : 0)
                   << ":directory="
                   << (std::filesystem::is_directory(status) ? 1 : 0)
                   << ":symlink="
                   << (std::filesystem::is_symlink(status) ? 1 : 0)
                   << ":unsafe=" << (unsafe ? 1 : 0)
                   << ":error=" << status_error.value();
            throw NativePortContractError(
                NativePortContractFailure::ContentLoadFailed,
                detail.str());
        }
    }
}

[[nodiscard]] std::vector<std::uint8_t> read_bound_image(
    const std::filesystem::path& content_root,
    const NativePortImageBinding& image) {
    const auto absolute_root =
        std::filesystem::absolute(content_root).lexically_normal();
    require_safe_existing_path(absolute_root, false);
    const auto canonical_root = std::filesystem::canonical(absolute_root);
    const auto relative =
        std::filesystem::path(image.content_relative_path).lexically_normal();
    if (relative.empty() || relative.is_absolute() ||
        *relative.begin() == "..")
        throw NativePortContractError(
            NativePortContractFailure::ContentLoadFailed,
            "content-root");
    const auto unresolved_candidate = canonical_root / relative;
    require_safe_existing_path(unresolved_candidate, true);
    const auto candidate = std::filesystem::canonical(unresolved_candidate);
    const auto status = std::filesystem::symlink_status(candidate);
    if (!path_is_within(candidate, canonical_root) ||
        !std::filesystem::is_regular_file(status) ||
        unsafe_filesystem_link(candidate, status))
        throw NativePortContractError(
            NativePortContractFailure::ContentLoadFailed,
            "content-image-path");
    const auto file_size = std::filesystem::file_size(candidate);
    if (image.file_offset > file_size ||
        image.byte_size > file_size - image.file_offset)
        throw NativePortContractError(
            NativePortContractFailure::ContentLoadFailed,
            "content-image-range");
    std::ifstream input(candidate, std::ios::binary);
    if (!input || image.file_offset >
                      static_cast<std::uint64_t>(
                          std::numeric_limits<std::streamoff>::max()))
        throw NativePortContractError(
            NativePortContractFailure::ContentLoadFailed,
            "content-image-open");
    input.seekg(static_cast<std::streamoff>(image.file_offset));
    std::vector<std::uint8_t> bytes(image.byte_size);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() !=
                      static_cast<std::streamsize>(bytes.size()))
        throw NativePortContractError(
            NativePortContractFailure::ContentLoadFailed,
            "content-image-read");
    require_safe_existing_path(unresolved_candidate, true);
    Sha256 hash;
    hash.update(bytes);
    if (std::string("sha256:") + hash.finish() != image.byte_identity)
        throw NativePortContractError(
            NativePortContractFailure::ContentLoadFailed,
            "content-image-identity");
    return bytes;
}

[[nodiscard]] std::string sha256_identity(
    const std::span<const std::uint8_t> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return std::string("sha256:") + hash.finish();
}

[[nodiscard]] bool valid_sha256_identity(
    const std::string_view identity) noexcept {
    constexpr std::string_view prefix{"sha256:"};
    return identity.size() == prefix.size() + 64u &&
           identity.starts_with(prefix) &&
           std::all_of(
               identity.begin() +
                   static_cast<std::ptrdiff_t>(prefix.size()),
               identity.end(), [](const char character) {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               });
}

[[nodiscard]] std::optional<std::span<const std::uint8_t>>
native_port_direct_bytes(const CpuState& cpu,
                         const std::uint32_t address,
                         const std::uint32_t size) noexcept {
    const auto segment = address >> 29u;
    // NativePortMemory has no MMU service. Its bounded main-RAM backing is
    // therefore directly addressable through No-MMU P0 as well as the P1/P2
    // aliases. P3 and P4 still require translation or hardware semantics and
    // must never be treated as ordinary title RAM here.
    if (size == 0u || segment >= 6u ||
        (segment < 4u && (cpu.mmucr & 1u) != 0u))
        return std::nullopt;
    const auto guard = cpu.memory.direct_linear_memory_guard(false);
    if (!guard) return std::nullopt;
    const auto physical = canonical_physical_address(address);
    if (physical < guard.physical_base) return std::nullopt;
    const auto relative = physical - guard.physical_base;
    if (relative >= guard.physical_span ||
        size > guard.physical_span - relative)
        return std::nullopt;
    const auto backing_offset = relative & guard.backing_mask;
    if (size > static_cast<std::uint64_t>(guard.backing_mask) + 1u -
                   backing_offset)
        return std::nullopt;
    return std::span<const std::uint8_t>(
        guard.read_bytes + backing_offset, size);
}

[[nodiscard]] std::uint32_t canonical_native_port_runtime_alias(
    const std::uint32_t address) noexcept {
    // The native no-MMU product admits P0, P1 and P2 as aliases of the same
    // bounded RAM. Runtime-image lifecycle, loaded-module identity and
    // dispatcher binding must therefore compare one canonical virtual alias;
    // otherwise a P0/P2 request can miss an active P1 image backed by the
    // exact same bytes. P3/P4 remain distinct and are rejected by the direct
    // byte proof rather than being disguised as RAM.
    if ((address >> 29u) >= 6u) return address;
    return canonical_physical_address(address) | 0x80000000u;
}

void validate_no_live_native_port_continuation(
    const CpuState& cpu,
    const std::uint32_t canonical_begin,
    const std::uint64_t canonical_end,
    const std::string_view detail) {
    const auto point_inside = [&](const std::uint32_t address) {
        // A zero PC/PR/active-instruction value denotes an inactive point in
        // CpuState.  Do not canonicalize that sentinel into the P1 window.
        if (address == 0u) return false;
        const auto canonical = canonical_native_port_runtime_alias(address);
        return canonical >= canonical_begin && canonical < canonical_end;
    };
    if (point_inside(cpu.pc) || point_inside(cpu.pr) ||
        point_inside(cpu.active_instruction_pc))
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation, detail);

    if (cpu.active_block_size == 0u) return;
    const auto active_begin = canonical_native_port_runtime_alias(
        cpu.active_block_virtual_start);
    const auto active_end =
        static_cast<std::uint64_t>(active_begin) + cpu.active_block_size;
    if (active_end > 0x1'0000'0000ull ||
        (active_begin < canonical_end && canonical_begin < active_end))
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation, detail);
}

struct BootstrapBackingInterval final {
    std::uint32_t begin = 0u;
    std::uint32_t end = 0u;
    std::uint8_t immutable_kind_mask = 0u;
    NativePortBootstrapWritePolicy write_policy =
        NativePortBootstrapWritePolicy::WritableDataOnly;
};

[[nodiscard]] BootstrapBackingInterval bootstrap_backing_interval(
    const std::uint32_t address,
    const std::uint32_t size,
    const NativePortContractFailure failure,
    const std::string_view detail) {
    const auto physical = canonical_physical_address(address);
    if (size == 0u || physical < native_port_main_memory_physical_base ||
        physical >= native_port_main_memory_physical_base +
                        native_port_main_memory_physical_span)
        throw NativePortContractError(failure, detail);
    const auto relative = physical - native_port_main_memory_physical_base;
    const auto begin = relative & (native_port_main_memory_backing_size - 1u);
    if (size > native_port_main_memory_backing_size - begin ||
        size > native_port_main_memory_physical_span - relative)
        throw NativePortContractError(failure, detail);
    return {begin, begin + size};
}

void append_bootstrap_immutable_backing_intervals(
    std::vector<BootstrapBackingInterval>& intervals,
    const NativePortImmutableRange& range) {
    if (range.byte_size == 0u)
        throw NativePortContractError(
            NativePortContractFailure::InvalidDefinition,
            "bootstrap-immutable-range");

    // Native AOT may contain identity-bound source mappings that deliberately
    // live outside the ordinary 64-MiB main-memory window.  They still belong
    // to the runtime immutable-write guard, but they cannot be changed through
    // the 16-MiB bootstrap backing snapshot and therefore have no interval in
    // this transition check.
    const auto physical_begin = static_cast<std::uint64_t>(
        canonical_physical_address(range.physical_address));
    const auto physical_end = physical_begin + range.byte_size;
    if (physical_end > 0x1'0000'0000ull)
        throw NativePortContractError(
            NativePortContractFailure::InvalidDefinition,
            "bootstrap-immutable-range");

    constexpr auto main_begin = static_cast<std::uint64_t>(
        native_port_main_memory_physical_base);
    constexpr auto main_end = main_begin +
                              native_port_main_memory_physical_span;
    auto overlap_begin = std::max(physical_begin, main_begin);
    const auto overlap_end = std::min(physical_end, main_end);
    if (overlap_begin >= overlap_end) return;

    // The 64-MiB physical window contains four aliases of one 16-MiB
    // backing. Split at alias boundaries so every changed backing byte is
    // protected even if a future executable range spans such a boundary.
    while (overlap_begin < overlap_end) {
        const auto relative = overlap_begin - main_begin;
        const auto backing_begin = static_cast<std::uint32_t>(
            relative & (native_port_main_memory_backing_size - 1u));
        const auto alias_remaining =
            native_port_main_memory_backing_size - backing_begin;
        const auto extent = static_cast<std::uint32_t>(std::min(
            overlap_end - overlap_begin,
            static_cast<std::uint64_t>(alias_remaining)));
        intervals.push_back(
            {backing_begin, backing_begin + extent, range.kind_mask});
        overlap_begin += extent;
    }
}

} // namespace

NativePortMemory::NativePortMemory(
    const std::uint32_t initial_cache_control_value)
    : main_memory_(
          std::make_shared<LinearMemoryDevice>(
              native_port_main_memory_backing_size)),
      cpu_control_(map_native_port_cpu_control(
          cpu_.memory,
          initial_cache_control_value)) {
    constexpr auto mirror_count =
        native_port_main_memory_physical_span /
        native_port_main_memory_backing_size;
    for (std::uint32_t mirror = 0u; mirror < mirror_count; ++mirror) {
        std::ostringstream name;
        name << "native-port-main-memory-" << mirror;
        cpu_.memory.map_region(
            name.str(),
            native_port_main_memory_physical_base +
                mirror * native_port_main_memory_backing_size,
            main_memory_);
    }
    cpu_.memory.bind_direct_linear_alias_window(
        native_port_main_memory_physical_base,
        native_port_main_memory_physical_span,
        *main_memory_);
}

CpuState& NativePortMemory::cpu() noexcept { return cpu_; }

const CpuState& NativePortMemory::cpu() const noexcept { return cpu_; }

NativePortCpuControl& NativePortMemory::cpu_control() noexcept {
    return *cpu_control_;
}

const NativePortCpuControl& NativePortMemory::cpu_control() const noexcept {
    return *cpu_control_;
}

void NativePortMemory::load_verified_images(
    const std::filesystem::path& content_root,
    const std::span<const NativePortImageBinding> images) {
    auto destination = main_memory_->writable_bytes();
    for (const auto& image : images) {
        const auto physical = canonical_physical_address(image.guest_address);
        if (physical < native_port_main_memory_physical_base ||
            physical >= native_port_main_memory_physical_base +
                            native_port_main_memory_physical_span)
            throw NativePortContractError(
                NativePortContractFailure::InvalidDefinition,
                "image-outside-native-main-memory");
        const auto offset =
            (physical - native_port_main_memory_physical_base) &
            (native_port_main_memory_backing_size - 1u);
        if (image.byte_size > destination.size() - offset)
            throw NativePortContractError(
                NativePortContractFailure::InvalidDefinition,
                "image-crosses-native-main-memory-mirror");
        const auto bytes = read_bound_image(content_root, image);
        std::copy(bytes.begin(), bytes.end(), destination.begin() + offset);
    }
}

struct NativePortRuntimeImageBindings::Impl final {
    struct ActiveBinding final {
        std::size_t image_index = 0u;
        std::unique_ptr<ScopedCodeAddressMapping> mapping;
    };

    CpuState& cpu;
    std::span<const NativePortRuntimeImageView> images;
    NativePortImmutableWriteGuard& immutable_guard;
    std::vector<ActiveBinding> active;
};

NativePortRuntimeImageBindings::NativePortRuntimeImageBindings(
    CpuState& cpu,
    const std::span<const NativePortRuntimeImageView> images,
    NativePortImmutableWriteGuard& immutable_guard)
    : impl_(std::make_unique<Impl>(
          Impl{cpu, images, immutable_guard, {}})) {
    constexpr std::uint32_t maximum_image_bytes = 4u * 1024u * 1024u;
    for (std::size_t image_index = 0u;
         image_index < images.size(); ++image_index) {
        const auto& image = images[image_index];
        const auto source_end =
            static_cast<std::uint64_t>(image.source_start) + image.byte_size;
        const auto runtime_end =
            static_cast<std::uint64_t>(image.runtime_start) + image.byte_size;
        if (image.image_id.empty() || image.image_id.size() > 128u ||
            (image.source_start & 3u) != 0u ||
            (image.runtime_start & 3u) != 0u || image.byte_size < 2u ||
            (image.byte_size & 1u) != 0u ||
            image.byte_size > maximum_image_bytes ||
            source_end > 0x1'0000'0000ull ||
            runtime_end > 0x1'0000'0000ull ||
            !valid_sha256_identity(image.sha256) ||
            image.block_identities.empty())
            throw NativePortContractError(
                NativePortContractFailure::InvalidDefinition,
                "runtime-image-definition");
        for (std::size_t previous = 0u; previous < image_index; ++previous) {
            const auto& other = images[previous];
            const auto other_source_end =
                static_cast<std::uint64_t>(other.source_start) +
                other.byte_size;
            if (image.image_id == other.image_id ||
                (image.source_start < other_source_end &&
                 other.source_start < source_end))
                throw NativePortContractError(
                    NativePortContractFailure::InvalidDefinition,
                    "runtime-image-identity-overlap");
        }
        std::optional<std::uint32_t> previous_offset;
        for (const auto& block : image.block_identities) {
            const auto block_end =
                static_cast<std::uint64_t>(block.source_offset) +
                block.byte_size;
            if ((block.source_offset & 1u) != 0u || block.byte_size < 2u ||
                (block.byte_size & 1u) != 0u ||
                block_end > image.byte_size ||
                (previous_offset.has_value() &&
                 block.source_offset <= *previous_offset) ||
                !valid_sha256_identity(block.sha256))
                throw NativePortContractError(
                    NativePortContractFailure::InvalidDefinition,
                    "runtime-image-block-definition");
            previous_offset = block.source_offset;
        }
    }
    impl_->active.reserve(images.size());
}

NativePortRuntimeImageBindings::~NativePortRuntimeImageBindings() noexcept {
    if (!impl_) return;
    // Keep the immutable guard reusable when this lifecycle owner ends before
    // the guard itself. Normal deactivation is strict; destruction can only
    // perform best-effort cleanup because it must not throw.
    for (auto active = impl_->active.rbegin();
         active != impl_->active.rend(); ++active) {
        const auto& image = impl_->images[active->image_index];
        for (const auto& block : image.block_identities) {
            try {
                impl_->immutable_guard.remove_runtime_executable_range(
                    image.runtime_start + block.source_offset,
                    block.byte_size);
            } catch (...) {
            }
        }
    }
}

void NativePortRuntimeImageBindings::activate(
    const std::string_view image_id) {
    const auto found = std::find_if(
        impl_->images.begin(), impl_->images.end(),
        [image_id](const auto& image) { return image.image_id == image_id; });
    if (found == impl_->images.end())
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "runtime-image-activate-unknown");
    const auto image_index =
        static_cast<std::size_t>(found - impl_->images.begin());
    if (std::any_of(
            impl_->active.begin(), impl_->active.end(),
            [image_index](const auto& active) {
                return active.image_index == image_index;
            }))
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "runtime-image-activate-duplicate");

    const auto canonical_runtime_start =
        canonical_native_port_runtime_alias(found->runtime_start);
    const auto runtime_end =
        static_cast<std::uint64_t>(canonical_runtime_start) +
        found->byte_size;
    for (const auto& active : impl_->active) {
        const auto& other = impl_->images[active.image_index];
        const auto other_runtime_start =
            canonical_native_port_runtime_alias(other.runtime_start);
        const auto other_end =
            static_cast<std::uint64_t>(other_runtime_start) +
            other.byte_size;
        if (canonical_runtime_start < other_end &&
            other_runtime_start < runtime_end)
            throw NativePortContractError(
                NativePortContractFailure::AotContractViolation,
                "runtime-image-active-range-overlap");
    }
    const auto image_bytes = native_port_direct_bytes(
        impl_->cpu, found->runtime_start, found->byte_size);
    if (!image_bytes.has_value())
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "runtime-image-range-unavailable");
    for (const auto& block : found->block_identities) {
        const auto bytes = native_port_direct_bytes(
            impl_->cpu, found->runtime_start + block.source_offset,
            block.byte_size);
        if (!bytes.has_value() || sha256_identity(*bytes) != block.sha256)
            throw NativePortContractError(
                NativePortContractFailure::AotContractViolation,
                "runtime-image-block-identity-mismatch");
    }
    auto mapping = std::make_unique<ScopedCodeAddressMapping>(
        CodeAddressMapping{found->source_start, canonical_runtime_start,
                           found->byte_size});
    std::size_t registered = 0u;
    try {
        for (const auto& block : found->block_identities) {
            impl_->immutable_guard.add_runtime_executable_range(
                found->runtime_start + block.source_offset,
                block.byte_size);
            ++registered;
        }
    } catch (...) {
        while (registered != 0u) {
            --registered;
            const auto& block = found->block_identities[registered];
            impl_->immutable_guard.remove_runtime_executable_range(
                found->runtime_start + block.source_offset,
                block.byte_size);
        }
        throw;
    }
    impl_->active.push_back({image_index, std::move(mapping)});
}

std::size_t NativePortRuntimeImageBindings::deactivate_runtime_range(
    std::uint32_t runtime_start,
    const std::size_t byte_size) {
    validate_deactivate_runtime_range(runtime_start, byte_size);
    runtime_start = canonical_native_port_runtime_alias(runtime_start);
    const auto requested_end =
        static_cast<std::uint64_t>(runtime_start) + byte_size;
    std::vector<std::size_t> selected;
    for (std::size_t active_index = 0u;
         active_index < impl_->active.size(); ++active_index) {
        const auto& image =
            impl_->images[impl_->active[active_index].image_index];
        const auto image_runtime_start =
            canonical_native_port_runtime_alias(image.runtime_start);
        const auto image_end =
            static_cast<std::uint64_t>(image_runtime_start) +
            image.byte_size;
        if (runtime_start >= image_end ||
            image_runtime_start >= requested_end)
            continue;
        selected.push_back(active_index);
    }
    for (auto selected_index = selected.rbegin();
         selected_index != selected.rend(); ++selected_index) {
        const auto active_index = *selected_index;
        const auto& image =
            impl_->images[impl_->active[active_index].image_index];
        for (const auto& block : image.block_identities)
            impl_->immutable_guard.remove_runtime_executable_range(
                image.runtime_start + block.source_offset,
                block.byte_size);
        impl_->active.erase(
            impl_->active.begin() + static_cast<std::ptrdiff_t>(active_index));
    }
    return selected.size();
}

void NativePortRuntimeImageBindings::validate_deactivate_runtime_range(
    std::uint32_t runtime_start,
    const std::size_t byte_size) const {
    runtime_start = canonical_native_port_runtime_alias(runtime_start);
    if (byte_size == 0u ||
        byte_size > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(runtime_start) + byte_size >
            0x1'0000'0000ull)
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "runtime-image-deactivate-range");
    const auto requested_end =
        static_cast<std::uint64_t>(runtime_start) + byte_size;
    for (const auto& active : impl_->active) {
        const auto& image = impl_->images[active.image_index];
        const auto image_runtime_start =
            canonical_native_port_runtime_alias(image.runtime_start);
        const auto image_end =
            static_cast<std::uint64_t>(image_runtime_start) +
            image.byte_size;
        if (runtime_start >= image_end || image_runtime_start >= requested_end)
            continue;
        if (runtime_start > image_runtime_start || requested_end < image_end)
            throw NativePortContractError(
                NativePortContractFailure::AotContractViolation,
                "runtime-image-deactivate-partial");
        validate_no_live_native_port_continuation(
            impl_->cpu, image_runtime_start, image_end,
            "runtime-image-deactivate-live-continuation");
    }
}

bool NativePortRuntimeImageBindings::active(
    const std::string_view image_id) const noexcept {
    return std::any_of(
        impl_->active.begin(), impl_->active.end(),
        [&](const auto& active) {
            return impl_->images[active.image_index].image_id == image_id;
        });
}

struct NativePortLoadedAotBinder::Impl final {
    struct ActiveBinding final {
        std::size_t module_index = 0u;
        std::uint32_t runtime_start = 0u;
        std::uint64_t code_generation = 0u;
        std::unique_ptr<ScopedCodeAddressMapping> mapping;
    };

    CpuState& cpu;
    std::span<const NativePortLoadedAotModuleView> modules;
    NativePortImmutableWriteGuard& immutable_guard;
    std::vector<ActiveBinding> active;
};

NativePortLoadedAotBinder::NativePortLoadedAotBinder(
    CpuState& cpu,
    const std::span<const NativePortLoadedAotModuleView> modules,
    NativePortImmutableWriteGuard& immutable_guard)
    : impl_(std::make_unique<Impl>(
          Impl{cpu, modules, immutable_guard, {}})) {
    constexpr std::uint32_t maximum_module_bytes = 4u * 1024u * 1024u;
    for (std::size_t module_index = 0u;
         module_index < modules.size(); ++module_index) {
        const auto& module = modules[module_index];
        const auto source_end =
            static_cast<std::uint64_t>(module.source_start) +
            module.byte_size;
        if ((module.source_start & 3u) != 0u || module.byte_size < 2u ||
            module.byte_size > maximum_module_bytes ||
            source_end > 0x1'0000'0000ull ||
            !valid_sha256_identity(module.sha256) ||
            module.block_identities.empty())
            throw NativePortContractError(
                NativePortContractFailure::InvalidDefinition,
                "loaded-aot-module-definition");
        for (std::size_t previous = 0u;
             previous < module_index; ++previous) {
            const auto& other = modules[previous];
            const auto other_end =
                static_cast<std::uint64_t>(other.source_start) +
                other.byte_size;
            if (module.source_start < other_end &&
                other.source_start < source_end)
                throw NativePortContractError(
                    NativePortContractFailure::InvalidDefinition,
                    "loaded-aot-module-source-overlap");
        }
        std::optional<std::uint32_t> previous_offset;
        for (const auto& block : module.block_identities) {
            const auto block_end =
                static_cast<std::uint64_t>(block.source_offset) +
                block.byte_size;
            if ((block.source_offset & 1u) != 0u ||
                block.byte_size < 2u || (block.byte_size & 1u) != 0u ||
                block_end > module.byte_size ||
                (previous_offset.has_value() &&
                 block.source_offset <= *previous_offset) ||
                !valid_sha256_identity(block.sha256))
                throw NativePortContractError(
                    NativePortContractFailure::InvalidDefinition,
                    "loaded-aot-block-definition");
            previous_offset = block.source_offset;
        }
    }
    impl_->active.reserve(modules.size());
}

NativePortLoadedAotBinder::~NativePortLoadedAotBinder() noexcept {
    if (!impl_) return;
    // Destruction ends the generated dispatch lifetime. Remove the dynamic
    // executable ranges before their mappings disappear so an owning guard
    // never retains phantom loaded-code protection.
    for (auto active = impl_->active.rbegin();
         active != impl_->active.rend(); ++active) {
        const auto& module = impl_->modules[active->module_index];
        for (const auto& block : module.block_identities) {
            try {
                impl_->immutable_guard.remove_runtime_executable_range(
                    active->runtime_start + block.source_offset,
                    block.byte_size);
            } catch (...) {
                // Destructors cannot recover or report a second exception.
                // Normal runtime retirement remains strict and throwing.
            }
        }
    }
}

bool NativePortLoadedAotBinder::validate_bound_entry(
    const std::uint32_t target) const {
    const auto runtime_target =
        canonical_native_port_runtime_alias(target);
    const Impl::ActiveBinding* match = nullptr;
    for (const auto& active : impl_->active) {
        const auto& module = impl_->modules[active.module_index];
        const auto active_runtime_start =
            canonical_native_port_runtime_alias(active.runtime_start);
        if (runtime_target < active_runtime_start ||
            static_cast<std::uint64_t>(runtime_target) >=
                static_cast<std::uint64_t>(active_runtime_start) +
                    module.byte_size)
            continue;
        if (match != nullptr)
            throw NativePortContractError(
                NativePortContractFailure::AotContractViolation,
                "loaded-aot-active-mapping-ambiguous");
        match = &active;
    }
    if (match == nullptr) return false;

    // Binding verifies the complete immutable AOT identity once and registers
    // every emitted block with the write observer. Re-hashing a block on every
    // dispatch turns a static overlay into an O(code-size) runtime hot path.
    // The observer generation is the authoritative O(1) proof that no bound
    // executable byte changed since activation.
    if (impl_->immutable_guard.write_detected() ||
        match->code_generation != impl_->immutable_guard.generation())
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "loaded-aot-entry-code-generation");

    const auto& module = impl_->modules[match->module_index];
    const auto match_runtime_start =
        canonical_native_port_runtime_alias(match->runtime_start);
    const auto offset = runtime_target - match_runtime_start;
    const auto block = std::lower_bound(
        module.block_identities.begin(), module.block_identities.end(),
        offset, [](const auto& candidate, const std::uint32_t value) {
            return candidate.source_offset < value;
        });
    if (block == module.block_identities.end() ||
        block->source_offset != offset) {
        std::ostringstream detail;
        detail << "loaded-aot-entry-identity-missing:target=0x" << std::hex
               << runtime_target << ";runtime-start=0x"
               << match_runtime_start << ";source-start=0x"
               << module.source_start << ";offset=0x" << offset
               << ";pc=0x" << impl_->cpu.pc << ";pr=0x" << impl_->cpu.pr
               << ";active-instruction=0x"
               << impl_->cpu.active_instruction_pc
               << ";active-block=0x"
               << impl_->cpu.active_block_virtual_start;
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            detail.str());
    }
    return true;
}

bool NativePortLoadedAotBinder::bind_entry(
    const std::uint32_t target) {
    if (validate_bound_entry(target)) return true;

    const auto runtime_target =
        canonical_native_port_runtime_alias(target);

    struct Candidate final {
        std::size_t module_index = 0u;
        std::uint32_t runtime_start = 0u;
    };
    std::optional<Candidate> match;
    for (std::size_t module_index = 0u;
         module_index < impl_->modules.size(); ++module_index) {
        const auto& module = impl_->modules[module_index];
        for (const auto& block : module.block_identities) {
            if (runtime_target < block.source_offset) continue;
            const auto runtime_start = runtime_target - block.source_offset;
            if ((runtime_start & 3u) != 0u ||
                static_cast<std::uint64_t>(runtime_start) +
                        module.byte_size >
                    0x1'0000'0000ull)
                continue;
            const auto block_bytes = native_port_direct_bytes(
                impl_->cpu, target, block.byte_size);
            if (!block_bytes.has_value() ||
                sha256_identity(*block_bytes) != block.sha256)
                continue;
            const auto module_bytes = native_port_direct_bytes(
                impl_->cpu, runtime_start, module.byte_size);
            if (!module_bytes.has_value())
                continue;
            // Loaded title modules commonly mix executable blocks with mutable
            // data.  Prefer the whole decoded-image identity while it is still
            // intact, but admit a later binding only when every block emitted
            // as AOT code still has its exact export-time identity.  This keeps
            // data initialization from invalidating safe static code without
            // weakening the executable closure.
            if (sha256_identity(*module_bytes) != module.sha256) {
                const auto code_identity_matches = std::all_of(
                    module.block_identities.begin(),
                    module.block_identities.end(), [&](const auto& candidate) {
                        const auto bytes = native_port_direct_bytes(
                            impl_->cpu,
                            runtime_start + candidate.source_offset,
                            candidate.byte_size);
                        return bytes.has_value() &&
                               sha256_identity(*bytes) == candidate.sha256;
                    });
                if (!code_identity_matches) continue;
            }
            const Candidate candidate{module_index, runtime_start};
            if (match.has_value() &&
                (match->module_index != candidate.module_index ||
                 match->runtime_start != candidate.runtime_start))
                throw NativePortContractError(
                    NativePortContractFailure::AotContractViolation,
                    "loaded-aot-runtime-identity-ambiguous");
            match = candidate;
        }
    }
    if (!match.has_value()) return false;

    const auto& module = impl_->modules[match->module_index];
    for (const auto& active : impl_->active) {
        const auto& active_module = impl_->modules[active.module_index];
        const auto active_runtime_start =
            canonical_native_port_runtime_alias(active.runtime_start);
        const auto active_end =
            static_cast<std::uint64_t>(active_runtime_start) +
            active_module.byte_size;
        const auto candidate_end =
            static_cast<std::uint64_t>(match->runtime_start) +
            module.byte_size;
        if (match->runtime_start < active_end &&
            active_runtime_start < candidate_end)
            throw NativePortContractError(
                NativePortContractFailure::AotContractViolation,
                "loaded-aot-runtime-range-overlap");
    }
    auto mapping = std::make_unique<ScopedCodeAddressMapping>(
        CodeAddressMapping{module.source_start, match->runtime_start,
                           module.byte_size});
    if (unrelocate_code_address(runtime_target) !=
        module.source_start + (runtime_target - match->runtime_start))
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "loaded-aot-runtime-mapping");
    std::size_t registered = 0u;
    try {
        for (const auto& block : module.block_identities) {
            impl_->immutable_guard.add_runtime_executable_range(
                match->runtime_start + block.source_offset,
                block.byte_size);
            ++registered;
        }
    } catch (...) {
        while (registered != 0u) {
            --registered;
            const auto& block = module.block_identities[registered];
            impl_->immutable_guard.remove_runtime_executable_range(
                match->runtime_start + block.source_offset,
                block.byte_size);
        }
        throw;
    }
    impl_->active.push_back(
        {match->module_index, match->runtime_start,
         impl_->immutable_guard.generation(), std::move(mapping)});
    return true;
}

void NativePortLoadedAotBinder::validate_deactivate_runtime_range(
    std::uint32_t runtime_start,
    const std::size_t byte_size) const {
    runtime_start = canonical_native_port_runtime_alias(runtime_start);
    if (byte_size == 0u ||
        byte_size > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(runtime_start) + byte_size >
            0x1'0000'0000ull)
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "loaded-aot-deactivate-range");
    const auto requested_end =
        static_cast<std::uint64_t>(runtime_start) + byte_size;
    for (const auto& active : impl_->active) {
        const auto& module = impl_->modules[active.module_index];
        const auto module_start =
            canonical_native_port_runtime_alias(active.runtime_start);
        const auto module_end =
            static_cast<std::uint64_t>(module_start) + module.byte_size;
        if (runtime_start >= module_end || module_start >= requested_end)
            continue;
        if (runtime_start > module_start || requested_end < module_end)
            throw NativePortContractError(
                NativePortContractFailure::AotContractViolation,
                "loaded-aot-deactivate-partial");
        validate_no_live_native_port_continuation(
            impl_->cpu, module_start, module_end,
            "loaded-aot-deactivate-live-continuation");
    }
}

std::size_t NativePortLoadedAotBinder::deactivate_runtime_range(
    std::uint32_t runtime_start,
    const std::size_t byte_size) {
    validate_deactivate_runtime_range(runtime_start, byte_size);
    runtime_start = canonical_native_port_runtime_alias(runtime_start);
    const auto requested_end =
        static_cast<std::uint64_t>(runtime_start) + byte_size;
    std::vector<std::size_t> selected;
    for (std::size_t active_index = 0u;
         active_index < impl_->active.size(); ++active_index) {
        const auto& active = impl_->active[active_index];
        const auto& module = impl_->modules[active.module_index];
        const auto module_start =
            canonical_native_port_runtime_alias(active.runtime_start);
        const auto module_end =
            static_cast<std::uint64_t>(module_start) + module.byte_size;
        if (runtime_start < module_end && module_start < requested_end)
            selected.push_back(active_index);
    }
    for (auto selected_index = selected.rbegin();
         selected_index != selected.rend(); ++selected_index) {
        const auto active_index = *selected_index;
        const auto& active = impl_->active[active_index];
        const auto& module = impl_->modules[active.module_index];
        for (const auto& block : module.block_identities)
            impl_->immutable_guard.remove_runtime_executable_range(
                active.runtime_start + block.source_offset,
                block.byte_size);
        impl_->active.erase(
            impl_->active.begin() +
            static_cast<std::ptrdiff_t>(active_index));
    }
    return selected.size();
}

NativePortExecutableRetirement deactivate_native_port_executable_range(
    NativePortContext& context,
    const std::uint32_t runtime_start,
    const std::size_t byte_size) {
    if (context.runtime_images == nullptr || context.loaded_aot == nullptr)
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "native-executable-retirement-unbound");
    // Validate both sides before mutating either. This makes partial-range or
    // live-continuation failures atomic across the fixed-image and latent-AOT
    // lifecycle domains.
    context.runtime_images->validate_deactivate_runtime_range(
        runtime_start, byte_size);
    context.loaded_aot->validate_deactivate_runtime_range(
        runtime_start, byte_size);
    NativePortExecutableRetirement result;
    result.runtime_images =
        context.runtime_images->deactivate_runtime_range(
            runtime_start, byte_size);
    result.loaded_aot_modules =
        context.loaded_aot->deactivate_runtime_range(
            runtime_start, byte_size);
    return result;
}

std::vector<std::uint8_t>
capture_native_port_main_memory(const CpuState& cpu) {
    const auto guard = cpu.memory.direct_linear_memory_guard(false);
    std::uint32_t offset = 0u;
    if (guard.read_bytes == nullptr ||
        !direct_linear_guard_offset(
            guard,
            0x8C000000u,
            native_port_main_memory_backing_size,
            offset))
        throw NativePortContractError(
            NativePortContractFailure::BootstrapFailed,
            "bootstrap-main-memory-snapshot");
    return {guard.read_bytes + offset,
            guard.read_bytes + offset + native_port_main_memory_backing_size};
}

void validate_native_port_bootstrap_memory_transition(
    const CpuState& cpu,
    const std::span<const std::uint8_t> before,
    const std::span<const NativePortBootstrapWriteBinding> writes,
    const std::span<const NativePortImmutableRange> immutable_ranges) {
    if (before.size() != native_port_main_memory_backing_size)
        throw NativePortContractError(
            NativePortContractFailure::BootstrapFailed,
            "bootstrap-memory-snapshot-size");
    const auto after = capture_native_port_main_memory(cpu);

    std::vector<BootstrapBackingInterval> writable;
    writable.reserve(writes.size());
    for (const auto& write : writes) {
        auto interval = bootstrap_backing_interval(
            write.guest_address,
            write.byte_size,
            NativePortContractFailure::InvalidDefinition,
            "bootstrap-write-range");
        interval.write_policy = write.policy;
        if (sha256_identity(std::span<const std::uint8_t>(before).subspan(
                interval.begin,
                write.byte_size)) != write.pre_write_identity)
            throw NativePortContractError(
                NativePortContractFailure::BootstrapFailed,
                "bootstrap-pre-memory-identity");
        writable.push_back(interval);
    }
    std::sort(writable.begin(), writable.end(), [](const auto& left,
                                                   const auto& right) {
        return std::tie(left.begin, left.end) <
               std::tie(right.begin, right.end);
    });

    std::vector<BootstrapBackingInterval> immutable;
    immutable.reserve(immutable_ranges.size());
    for (const auto& range : immutable_ranges)
        append_bootstrap_immutable_backing_intervals(immutable, range);
    std::sort(immutable.begin(), immutable.end(), [](const auto& left,
                                                     const auto& right) {
        return std::tie(left.begin, left.end) <
               std::tie(right.begin, right.end);
    });

    std::size_t writable_index = 0u;
    std::size_t next_immutable_index = 0u;
    std::vector<std::size_t> active_immutable;
    for (std::uint32_t offset = 0u;
         offset < native_port_main_memory_backing_size;
         ++offset) {
        if (before[offset] == after[offset]) continue;
        while (writable_index < writable.size() &&
               writable[writable_index].end <= offset)
            ++writable_index;
        if (writable_index == writable.size() ||
            writable[writable_index].begin > offset)
            throw NativePortContractError(
                NativePortContractFailure::BootstrapFailed,
                "bootstrap-changed-undeclared-byte");

        while (next_immutable_index < immutable.size() &&
               immutable[next_immutable_index].begin <= offset) {
            if (immutable[next_immutable_index].end > offset)
                active_immutable.push_back(next_immutable_index);
            ++next_immutable_index;
        }
        std::erase_if(active_immutable, [&](const auto index) {
            return immutable[index].end <= offset;
        });
        std::uint8_t immutable_kind_mask = 0u;
        for (const auto index : active_immutable)
            immutable_kind_mask |= immutable[index].immutable_kind_mask;
        if (immutable_kind_mask != 0u &&
            writable[writable_index].write_policy !=
                NativePortBootstrapWritePolicy::
                    IdentityBoundImmutableMaterialization) {
            std::ostringstream detail;
            detail << "bootstrap-changed-immutable-byte:backing-offset=0x"
                   << std::hex << offset << ";kind=0x"
                   << static_cast<unsigned>(immutable_kind_mask);
            throw NativePortContractError(
                NativePortContractFailure::ImmutableMemoryWrite,
                detail.str());
        }
    }

    for (const auto& write : writes) {
        const auto interval = bootstrap_backing_interval(
            write.guest_address,
            write.byte_size,
            NativePortContractFailure::InvalidDefinition,
            "bootstrap-write-range");
        if (sha256_identity(std::span<const std::uint8_t>(after).subspan(
                interval.begin,
                write.byte_size)) != write.post_write_identity)
            throw NativePortContractError(
                NativePortContractFailure::BootstrapFailed,
                "bootstrap-post-memory-identity");
    }
}

std::string native_port_cpu_state_identity(const CpuState& cpu) {
    if (cpu.manual_reset_sink.context != nullptr ||
        cpu.manual_reset_sink.callback != nullptr || cpu.address_space ||
        cpu.gdrom_services != nullptr || cpu.g1_bus != nullptr)
        throw NativePortContractError(
            NativePortContractFailure::BootstrapFailed,
            "cpu-state-historical-runtime-binding");

    std::vector<std::uint8_t> bytes;
    bytes.reserve(1'024u);
    const auto append_u8 = [&](const std::uint8_t value) {
        bytes.push_back(value);
    };
    const auto append_u32 = [&](const std::uint32_t value) {
        for (std::size_t shift = 0u; shift < 32u; shift += 8u)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto append_u64 = [&](const std::uint64_t value) {
        for (std::size_t shift = 0u; shift < 64u; shift += 8u)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto append_words = [&](const auto& words) {
        for (const auto value : words) append_u32(value);
    };

    append_words(cpu.r);
    append_words(cpu.r_bank);
    append_words(cpu.fr);
    append_words(cpu.xf);
    append_u32(cpu.pc);
    append_u32(cpu.pr);
    append_u32(cpu.gbr);
    append_u32(cpu.vbr);
    append_u32(cpu.ssr);
    append_u32(cpu.spc);
    append_u32(cpu.sgr);
    append_u32(cpu.dbr);
    append_u32(cpu.tra);
    append_u32(cpu.tea);
    append_u32(cpu.expevt);
    append_u32(cpu.intevt);
    append_u32(cpu.pteh);
    append_u32(cpu.ptel);
    append_u32(cpu.ptea);
    append_u32(cpu.ttb);
    append_u32(cpu.mmucr);
    for (const auto& entry : cpu.utlb) {
        append_u32(entry.pteh);
        append_u32(entry.ptel);
        append_u32(entry.ptea);
    }
    append_u64(cpu.tlb_load_count);
    append_u32(cpu.mach);
    append_u32(cpu.macl);
    append_u32(cpu.fpul);
    append_u32(cpu.fpscr);
    append_u32(cpu.sr);
    append_u8(cpu.t ? 1u : 0u);
    append_u8(cpu.s ? 1u : 0u);
    append_u8(cpu.q ? 1u : 0u);
    append_u8(cpu.m ? 1u : 0u);
    append_u8(cpu.trap_pending ? 1u : 0u);
    append_u64(cpu.exception_generation);
    append_u8(static_cast<std::uint8_t>(cpu.last_exception_cause));
    append_u8(cpu.exception_in_delay_slot ? 1u : 0u);
    append_u32(cpu.last_exception_instruction_pc);
    append_u32(cpu.last_exception_instruction_physical_pc);
    append_u32(cpu.last_exception_owner_pc);
    append_u64(cpu.last_exception_generation);
    append_u8(cpu.sleeping ? 1u : 0u);
    append_u32(cpu.last_prefetch_address);
    append_u64(cpu.prefetch_count);
    append_u64(cpu.attempted_guest_instructions);
    append_u64(cpu.retired_guest_instructions);
    append_u64(cpu.total_guest_cycles);
    append_u64(cpu.pending_guest_cycles);
    append_u32(cpu.active_instruction_pc);
    append_u32(cpu.active_instruction_physical_pc);
    append_u32(cpu.active_block_virtual_start);
    append_u32(cpu.active_block_physical_start);
    append_u32(cpu.active_block_size);
    append_u8(cpu.last_prefetch_was_store_queue ? 1u : 0u);
    return sha256_identity(bytes);
}

} // namespace katana::runtime
