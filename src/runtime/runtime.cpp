#include "katana/runtime/runtime.hpp"

#include <stdexcept>
#include <utility>

namespace katana::runtime {

std::uint32_t CpuState::read_sr() const noexcept {
    return (sr & ~0x00000303u) |
        (m ? 0x00000200u : 0u) |
        (q ? 0x00000100u : 0u) |
        (s ? 0x00000002u : 0u) |
        (t ? 0x00000001u : 0u);
}

void CpuState::write_sr(const std::uint32_t value) noexcept {
    const std::uint32_t masked = value & 0x700083F3u;
    const bool old_rb = (sr & 0x20000000u) != 0u;
    const bool new_rb = (masked & 0x20000000u) != 0u;
    if (old_rb != new_rb) {
        for (std::size_t index = 0u; index < r_bank.size(); ++index) {
            std::swap(r[index], r_bank[index]);
        }
    }
    sr = masked;
    m = (masked & 0x00000200u) != 0u;
    q = (masked & 0x00000100u) != 0u;
    s = (masked & 0x00000002u) != 0u;
    t = (masked & 0x00000001u) != 0u;
}

void reset_cpu(
    CpuState& cpu,
    const ResetState& state
) noexcept {
    cpu.r.fill(0u);
    cpu.r_bank.fill(0u);
    cpu.fr.fill(0u);
    cpu.xf.fill(0u);

    cpu.pc = state.program_counter;
    cpu.pr = 0u;
    cpu.gbr = 0u;
    cpu.vbr = state.vector_base;
    cpu.ssr = 0u;
    cpu.spc = 0u;
    cpu.sgr = 0u;
    cpu.dbr = 0u;
    cpu.tra = 0u;
    cpu.expevt = 0u;
    cpu.intevt = 0u;
    cpu.mach = 0u;
    cpu.macl = 0u;
    cpu.fpul = 0u;
    cpu.fpscr = state.fpscr;

    cpu.sr = 0u;
    cpu.t = false;
    cpu.s = false;
    cpu.q = false;
    cpu.m = false;
    cpu.trap_pending = false;
    cpu.sleeping = false;

    cpu.r[15] = state.stack_pointer;
    cpu.write_sr(state.status_register);
}

[[noreturn]] void unresolved_call(
    CpuState&,
    const std::uint32_t
) {
    throw std::runtime_error("Nicht aufgeloester Aufruf");
}

[[noreturn]] void unresolved_jump(
    CpuState&,
    const std::uint32_t
) {
    throw std::runtime_error("Nicht aufgeloester Sprung");
}

} // namespace katana::runtime