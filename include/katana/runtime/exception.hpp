#pragma once

#include "katana/runtime/runtime.hpp"

#include <cstdint>
#include <optional>

namespace katana::runtime {

inline constexpr std::uint32_t general_exception_vector = 0x00000100u;
inline constexpr std::uint32_t interrupt_vector = 0x00000600u;

inline constexpr std::uint32_t event_address_error_read = 0x000000E0u;
inline constexpr std::uint32_t event_address_error_write = 0x00000100u;
inline constexpr std::uint32_t event_trapa = 0x00000160u;
inline constexpr std::uint32_t event_illegal_instruction = 0x00000180u;
inline constexpr std::uint32_t event_slot_illegal_instruction = 0x000001A0u;
inline constexpr std::uint32_t event_fpu_disabled = 0x00000800u;
inline constexpr std::uint32_t event_slot_fpu_disabled = 0x00000820u;

struct ExceptionRequest {
    ExceptionCause cause = ExceptionCause::None;
    std::uint32_t event_code = 0u;
    std::uint32_t vector_offset = general_exception_vector;
    std::uint32_t return_pc = 0u;
    std::optional<std::uint32_t> fault_address;
    bool interrupt = false;
    bool in_delay_slot = false;
};

void enter_exception(CpuState& cpu, const ExceptionRequest& request) noexcept;

void raise_trapa(CpuState& cpu, std::uint8_t immediate, std::uint32_t instruction_pc) noexcept;

void raise_illegal_instruction(
    CpuState& cpu,
    std::uint32_t instruction_pc,
    std::optional<std::uint32_t> delay_slot_owner = std::nullopt) noexcept;

void raise_fpu_disabled(CpuState& cpu,
                        std::uint32_t instruction_pc,
                        std::optional<std::uint32_t> delay_slot_owner = std::nullopt) noexcept;

void enter_memory_exception(CpuState& cpu,
                            const MemoryAccessError& error,
                            std::uint32_t instruction_pc,
                            std::optional<std::uint32_t> delay_slot_owner = std::nullopt) noexcept;

void return_from_exception(CpuState& cpu) noexcept;

} // namespace katana::runtime
