#pragma once

#include "katana/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace katana::runtime {

class RuntimeAddressSpace;
struct CpuState;

enum class GuestBufferAccess : std::uint8_t { Read, Write };

struct GuestAddressSpaceBinding {
    std::shared_ptr<RuntimeAddressSpace> address_space;
    bool privileged = true;
};

struct GuestLinearBuffer {
    std::uint32_t guest_address = 0u;
    std::uint32_t physical_address = 0u;
    std::size_t size = 0u;
    std::size_t alignment = 1u;
    GuestBufferAccess access = GuestBufferAccess::Read;

    [[nodiscard]] bool operator==(const GuestLinearBuffer&) const = default;
};

[[nodiscard]] GuestAddressSpaceBinding
bind_guest_address_space(const CpuState& cpu) noexcept;
[[nodiscard]] std::optional<GuestLinearBuffer>
resolve_guest_read_buffer(const GuestAddressSpaceBinding& binding,
                          Memory& memory,
                          std::uint32_t guest_address,
                          std::size_t size,
                          std::size_t alignment = 1u) noexcept;
[[nodiscard]] std::optional<GuestLinearBuffer>
resolve_guest_write_buffer(const GuestAddressSpaceBinding& binding,
                           Memory& memory,
                           std::uint32_t guest_address,
                           std::size_t size,
                           std::size_t alignment = 1u) noexcept;
[[nodiscard]] bool read_guest_buffer(const GuestAddressSpaceBinding& binding,
                                     Memory& memory,
                                     const GuestLinearBuffer& buffer,
                                     std::span<std::uint8_t> destination) noexcept;
[[nodiscard]] bool commit_guest_write_buffer(
    const GuestAddressSpaceBinding& binding,
    Memory& memory,
    const GuestLinearBuffer& buffer,
    std::span<const std::uint8_t> bytes,
    CodeWriteSource source = CodeWriteSource::Copy) noexcept;

[[nodiscard]] std::optional<GuestLinearBuffer>
resolve_guest_read_buffer(CpuState& cpu,
                          std::uint32_t guest_address,
                          std::size_t size,
                          std::size_t alignment = 1u) noexcept;
[[nodiscard]] std::optional<GuestLinearBuffer>
resolve_guest_write_buffer(CpuState& cpu,
                           std::uint32_t guest_address,
                           std::size_t size,
                           std::size_t alignment = 1u) noexcept;
[[nodiscard]] bool read_guest_buffer(CpuState& cpu,
                                     const GuestLinearBuffer& buffer,
                                     std::span<std::uint8_t> destination) noexcept;
[[nodiscard]] bool commit_guest_write_buffer(
    CpuState& cpu,
    const GuestLinearBuffer& buffer,
    std::span<const std::uint8_t> bytes,
    CodeWriteSource source = CodeWriteSource::Copy) noexcept;

} // namespace katana::runtime
