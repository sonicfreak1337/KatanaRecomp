#include "katana/runtime/native_port_content.hpp"

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

} // namespace

NativePortMemory::NativePortMemory()
    : main_memory_(
          std::make_shared<LinearMemoryDevice>(
              native_port_main_memory_backing_size)) {
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

} // namespace katana::runtime
