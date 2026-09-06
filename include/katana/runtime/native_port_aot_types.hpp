#pragma once

#include <cstdint>

namespace katana::runtime {

class NativePortContext;

enum class NativePortImmutableRangeKind : std::uint8_t {
    Executable = 1u << 0u,
    ReadOnlyImage = 1u << 1u,
};

[[nodiscard]] constexpr std::uint8_t native_port_immutable_range_mask(
    const NativePortImmutableRangeKind kind) noexcept {
    return static_cast<std::uint8_t>(kind);
}

struct NativePortImmutableRange final {
    std::uint32_t physical_address = 0u;
    std::uint32_t byte_size = 0u;
    std::uint8_t kind_mask = 0u;

    [[nodiscard]] bool operator==(
        const NativePortImmutableRange&) const = default;
};

enum class NativePortStopReason : std::uint8_t {
    None,
    HostRequested,
    HostDeadline,
    HookAbort,
    MissingStaticEntry,
    ExecutableCodeWrite,
    ReadOnlyImageWrite,
    GuestExceptionOrSleep,
    AotContractViolation,
    UnresolvedHardwareAccess,
    ForbiddenHardwareOperation,
};

} // namespace katana::runtime
