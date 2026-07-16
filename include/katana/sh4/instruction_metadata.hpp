#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace katana::sh4 {

enum class OperandFormat {
    None,
    RegisterN,
    RegisterMRegisterN,
    Immediate8RegisterN,
    Immediate8Register0,
    Displacement8,
    Displacement12,
    RegisterIndirectN,
    Displacement4,
    Displacement8RegisterN,
    R0Indexed,
    GbrDisplacement8,
    PcDisplacement8,
    SpecialRegisterTransfer
};

struct InstructionMetadata {
    InstructionKind kind;
    std::string_view name;
    std::uint16_t mask;
    std::uint16_t pattern;
    OperandFormat operand_format;
    ControlFlowKind control_flow;
    bool has_delay_slot;
    bool is_privileged;

    [[nodiscard]] constexpr bool matches(const std::uint16_t opcode) const noexcept {
        return (opcode & mask) == pattern;
    }
};

struct SpecialRegisterEncodingMetadata {
    InstructionKind kind;
    std::string_view mnemonic;
    std::uint16_t mask;
    std::uint16_t pattern;
    SpecialRegister special_register;
    std::string_view special_register_name;
    bool memory_form;
    bool register_is_source;
    bool is_privileged;

    [[nodiscard]] constexpr bool matches(const std::uint16_t opcode) const noexcept {
        return (opcode & mask) == pattern;
    }
};

[[nodiscard]] std::span<const InstructionMetadata> instruction_metadata() noexcept;
[[nodiscard]] std::span<const SpecialRegisterEncodingMetadata>
special_register_encoding_metadata() noexcept;

[[nodiscard]] const InstructionMetadata* metadata_for_kind(InstructionKind kind) noexcept;

} // namespace katana::sh4
