#pragma once

#include "katana/ir/ir.hpp"

#include <cstdint>
#include <vector>

namespace katana::ir {

// The scalar architectural values that native function-level AOT may keep in
// host locals. FPU data registers deliberately remain outside this contract;
// FPUL is scalar SH-4 state and is tracked independently.
enum class TrackedRegister : std::uint8_t {
    R0,
    R1,
    R2,
    R3,
    R4,
    R5,
    R6,
    R7,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
    T,
    Pr,
    Gbr,
    Mach,
    Macl,
    Fpul,
    Count
};

using RegisterMask = std::uint32_t;

[[nodiscard]] constexpr RegisterMask register_bit(const TrackedRegister value) noexcept {
    return RegisterMask{1u} << static_cast<std::uint8_t>(value);
}

[[nodiscard]] constexpr RegisterMask gpr_register_bit(const std::uint8_t index) noexcept {
    return index < 16u ? RegisterMask{1u} << index : RegisterMask{0u};
}

inline constexpr RegisterMask general_register_mask = (RegisterMask{1u} << 16u) - 1u;
inline constexpr RegisterMask scalar_register_mask =
    register_bit(TrackedRegister::T) | register_bit(TrackedRegister::Pr) |
    register_bit(TrackedRegister::Gbr) | register_bit(TrackedRegister::Mach) |
    register_bit(TrackedRegister::Macl) | register_bit(TrackedRegister::Fpul);
inline constexpr RegisterMask tracked_register_mask =
    general_register_mask | scalar_register_mask;

[[nodiscard]] constexpr bool register_mask_contains(const RegisterMask mask,
                                                    const TrackedRegister value) noexcept {
    return (mask & register_bit(value)) != 0u;
}

[[nodiscard]] constexpr std::uint16_t
general_register_subset(const RegisterMask mask) noexcept {
    return static_cast<std::uint16_t>(mask & general_register_mask);
}

// The scalar subset keeps the TrackedRegister T..FPUL ordering and therefore
// maps directly to runtime::NativeAotScalarRegisterMask without coupling the
// analyzer IR to the runtime header.
[[nodiscard]] constexpr std::uint8_t scalar_register_subset(const RegisterMask mask) noexcept {
    return static_cast<std::uint8_t>((mask & scalar_register_mask) >> 16u);
}

struct InstructionRegisterUseDef {
    RegisterMask uses = 0u;
    RegisterMask defs = 0u;

    bool operator==(const InstructionRegisterUseDef&) const = default;
};

// Returns exact use/def information for the scalar architectural register
// subset above. The result is conservative when an instruction is unknown.
[[nodiscard]] InstructionRegisterUseDef
instruction_register_use_def(const Instruction& instruction) noexcept;

struct BlockRegisterLiveness {
    std::uint32_t start_address = 0u;
    RegisterMask uses_before_def = 0u;
    RegisterMask defs = 0u;
    RegisterMask live_in = 0u;
    RegisterMask live_out = 0u;
    // An open successor is outside this Function or is dynamically selected.
    // Its register demand is unknown, so all tracked values are live-out.
    bool has_open_successor = false;
};

struct RegisterLocalizationPlan {
    std::vector<BlockRegisterLiveness> blocks;
    RegisterMask referenced_registers = 0u;
    // Values whose lifetime crosses an instruction boundary. A one-off dead
    // definition is referenced but is intentionally not a native-local
    // candidate.
    RegisterMask candidate_registers = 0u;
    bool closed_control_flow = true;

    [[nodiscard]] const BlockRegisterLiveness*
    find_block(std::uint32_t start_address) const noexcept;

    [[nodiscard]] std::uint16_t general_register_candidates() const noexcept {
        return general_register_subset(candidate_registers);
    }

    [[nodiscard]] std::uint8_t scalar_register_candidates() const noexcept {
        return scalar_register_subset(candidate_registers);
    }
};

// Computes block-local use/def and the standard backwards CFG liveness
// fixed-point. External and indirect successors are deliberately conservative;
// ordinary function returns are closed native exits and are flushed by the
// register-file ownership contract.
[[nodiscard]] RegisterLocalizationPlan
make_register_localization_plan(const Function& function);

} // namespace katana::ir
