#include "katana/runtime/guest_buffer.hpp"

#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/runtime.hpp"

#include <algorithm>

namespace katana::runtime {
namespace {

constexpr std::size_t minimum_sh4_page_size = 1024u;
constexpr std::uint64_t guest_address_space_size = std::uint64_t{1u} << 32u;

std::uint32_t translate_bound_address(const GuestAddressSpaceBinding& binding,
                                      const std::uint32_t address,
                                      const GuestBufferAccess access) {
    if (!binding.address_space) {
        RuntimeAddressSpace no_mmu_address_space;
        return no_mmu_address_space
            .translate(address,
                       access == GuestBufferAccess::Write
                           ? TranslationAccess::Write
                           : TranslationAccess::Read,
                       binding.privileged)
            .physical_address;
    }
    return binding.address_space
        ->translate(address,
                    access == GuestBufferAccess::Write
                        ? TranslationAccess::Write
                        : TranslationAccess::Read,
                    binding.privileged)
        .physical_address;
}

std::optional<GuestLinearBuffer>
resolve_guest_buffer(const GuestAddressSpaceBinding& binding,
                     Memory& memory,
                     const std::uint32_t guest_address,
                     const std::size_t size,
                     const std::size_t alignment,
                     const GuestBufferAccess access) noexcept {
    if (alignment == 0u || guest_address % alignment != 0u ||
        size > guest_address_space_size - guest_address)
        return std::nullopt;
    if (size == 0u)
        return GuestLinearBuffer{guest_address, 0u, 0u, alignment, access};

    try {
        const auto first =
            translate_bound_address(binding, guest_address, access);
        auto offset = std::min<std::size_t>(
            size, minimum_sh4_page_size -
                      (guest_address & (minimum_sh4_page_size - 1u)));
        while (offset < size) {
            const auto translated = translate_bound_address(
                binding,
                guest_address + static_cast<std::uint32_t>(offset),
                access);
            if (static_cast<std::uint64_t>(translated) !=
                static_cast<std::uint64_t>(first) + offset)
                return std::nullopt;
            offset += std::min<std::size_t>(minimum_sh4_page_size, size - offset);
        }
        if (static_cast<std::uint64_t>(first) + size > guest_address_space_size)
            return std::nullopt;
        const auto linear = access == GuestBufferAccess::Write
                                ? memory.is_writable_linear_range(first, size)
                                : memory.is_readable_linear_range(first, size);
        if (!linear) return std::nullopt;
        return GuestLinearBuffer{guest_address, first, size, alignment, access};
    } catch (...) {
        return std::nullopt;
    }
}

bool same_current_mapping(const GuestAddressSpaceBinding& binding,
                          Memory& memory,
                          const GuestLinearBuffer& buffer) noexcept {
    const auto current =
        resolve_guest_buffer(binding,
                             memory,
                             buffer.guest_address,
                             buffer.size,
                             buffer.alignment,
                             buffer.access);
    return current && current->physical_address == buffer.physical_address;
}

} // namespace

GuestAddressSpaceBinding bind_guest_address_space(const CpuState& cpu) noexcept {
    return {cpu.address_space, cpu.privileged_mode()};
}

std::optional<GuestLinearBuffer>
resolve_guest_read_buffer(const GuestAddressSpaceBinding& binding,
                          Memory& memory,
                          const std::uint32_t guest_address,
                          const std::size_t size,
                          const std::size_t alignment) noexcept {
    return resolve_guest_buffer(
        binding, memory, guest_address, size, alignment, GuestBufferAccess::Read);
}

std::optional<GuestLinearBuffer>
resolve_guest_write_buffer(const GuestAddressSpaceBinding& binding,
                           Memory& memory,
                           const std::uint32_t guest_address,
                           const std::size_t size,
                           const std::size_t alignment) noexcept {
    return resolve_guest_buffer(
        binding, memory, guest_address, size, alignment, GuestBufferAccess::Write);
}

bool read_guest_buffer(const GuestAddressSpaceBinding& binding,
                       Memory& memory,
                       const GuestLinearBuffer& buffer,
                       const std::span<std::uint8_t> destination) noexcept {
    if (buffer.access != GuestBufferAccess::Read ||
        destination.size() != buffer.size)
        return false;
    if (destination.empty()) return true;
    if (!same_current_mapping(binding, memory, buffer)) return false;
    try {
        for (std::size_t index = 0u; index < destination.size(); ++index)
            destination[index] = memory.read_u8(
                buffer.physical_address + static_cast<std::uint32_t>(index));
        return true;
    } catch (...) {
        return false;
    }
}

bool commit_guest_write_buffer(const GuestAddressSpaceBinding& binding,
                               Memory& memory,
                               const GuestLinearBuffer& buffer,
                               const std::span<const std::uint8_t> bytes,
                               const CodeWriteSource source) noexcept {
    if (buffer.access != GuestBufferAccess::Write || bytes.size() != buffer.size)
        return false;
    if (bytes.empty()) return true;
    if (!same_current_mapping(binding, memory, buffer)) return false;
    return memory.commit_linear_transaction_bytes(
        buffer.physical_address, bytes, source);
}

std::optional<GuestLinearBuffer>
resolve_guest_read_buffer(CpuState& cpu,
                          const std::uint32_t guest_address,
                          const std::size_t size,
                          const std::size_t alignment) noexcept {
    return resolve_guest_read_buffer(
        bind_guest_address_space(cpu), cpu.memory, guest_address, size, alignment);
}

std::optional<GuestLinearBuffer>
resolve_guest_write_buffer(CpuState& cpu,
                           const std::uint32_t guest_address,
                           const std::size_t size,
                           const std::size_t alignment) noexcept {
    return resolve_guest_write_buffer(
        bind_guest_address_space(cpu), cpu.memory, guest_address, size, alignment);
}

bool read_guest_buffer(CpuState& cpu,
                       const GuestLinearBuffer& buffer,
                       const std::span<std::uint8_t> destination) noexcept {
    return read_guest_buffer(
        bind_guest_address_space(cpu), cpu.memory, buffer, destination);
}

bool commit_guest_write_buffer(CpuState& cpu,
                               const GuestLinearBuffer& buffer,
                               const std::span<const std::uint8_t> bytes,
                               const CodeWriteSource source) noexcept {
    return commit_guest_write_buffer(
        bind_guest_address_space(cpu), cpu.memory, buffer, bytes, source);
}

} // namespace katana::runtime
