#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace katana::runtime {

inline constexpr std::uint32_t abi_version = 4u;

inline constexpr std::size_t general_register_count = 16u;
inline constexpr std::size_t banked_register_count = 8u;
inline constexpr std::size_t fpu_register_count = 16u;

struct ResetState {
    std::uint32_t program_counter = 0u;
    std::uint32_t stack_pointer = 0u;
    std::uint32_t vector_base = 0u;
    std::uint32_t status_register = 0u;
    std::uint32_t fpscr = 0u;
};

struct CpuState {
    std::array<std::uint32_t, general_register_count> r{};
    std::array<std::uint32_t, banked_register_count> r_bank{};
    std::array<std::uint32_t, fpu_register_count> fr{};
    std::array<std::uint32_t, fpu_register_count> xf{};
    std::uint32_t pc = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t gbr = 0u;
    std::uint32_t vbr = 0u;
    std::uint32_t ssr = 0u;
    std::uint32_t spc = 0u;
    std::uint32_t sgr = 0u;
    std::uint32_t dbr = 0u;
    std::uint32_t tra = 0u;
    std::uint32_t expevt = 0u;
    std::uint32_t intevt = 0u;
    std::uint32_t mach = 0u;
    std::uint32_t macl = 0u;
    std::uint32_t fpul = 0u;
    std::uint32_t fpscr = 0u;
    std::uint32_t sr = 0u;
    bool t = false;
    bool s = false;
    bool q = false;
    bool m = false;
    bool trap_pending = false;
    bool sleeping = false;
    Memory memory{};

    [[nodiscard]] std::uint32_t read_sr() const noexcept;
    void write_sr(std::uint32_t value) noexcept;
};

void reset_cpu(
    CpuState& cpu,
    const ResetState& state = ResetState{}
) noexcept;

[[noreturn]] void unresolved_call(CpuState& cpu, std::uint32_t target);
[[noreturn]] void unresolved_jump(CpuState& cpu, std::uint32_t target);

} // namespace katana::runtime