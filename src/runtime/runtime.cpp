#include "katana/runtime/runtime.hpp"

#include <stdexcept>
#include <utility>

namespace katana::runtime {

std::uint32_t CpuState::read_sr() const noexcept {
    return (sr & ~(sr_m_mask | sr_q_mask | sr_s_mask | sr_t_mask)) | (m ? sr_m_mask : 0u) |
           (q ? sr_q_mask : 0u) | (s ? sr_s_mask : 0u) | (t ? sr_t_mask : 0u);
}

void CpuState::write_sr(const std::uint32_t value) noexcept {
    const std::uint32_t masked = value & sr_writable_mask;
    const bool old_rb = (sr & sr_rb_mask) != 0u;
    const bool new_rb = (masked & sr_rb_mask) != 0u;
    if (old_rb != new_rb) {
        for (std::size_t index = 0u; index < r_bank.size(); ++index) {
            std::swap(r[index], r_bank[index]);
        }
    }
    sr = masked;
    m = (masked & sr_m_mask) != 0u;
    q = (masked & sr_q_mask) != 0u;
    s = (masked & sr_s_mask) != 0u;
    t = (masked & sr_t_mask) != 0u;
}

std::uint32_t CpuState::read_fpscr() const noexcept {
    return fpscr;
}

void CpuState::write_fpscr(const std::uint32_t value) noexcept {
    const std::uint32_t masked = value & fpscr_writable_mask;
    const bool old_fr = (fpscr & fpscr_fr_mask) != 0u;
    const bool new_fr = (masked & fpscr_fr_mask) != 0u;
    if (old_fr != new_fr) {
        fr.swap(xf);
    }
    fpscr = masked;
}

void CpuState::toggle_fpu_register_bank() noexcept {
    write_fpscr(read_fpscr() ^ fpscr_fr_mask);
}

bool CpuState::fpu_register_bank_selected() const noexcept {
    return (read_fpscr() & fpscr_fr_mask) != 0u;
}

bool CpuState::fpu_double_precision() const noexcept {
    return (read_fpscr() & fpscr_pr_mask) != 0u;
}

bool CpuState::fpu_transfer_pair() const noexcept {
    return (read_fpscr() & fpscr_sz_mask) != 0u;
}

bool CpuState::fpu_flush_denormals() const noexcept {
    return (read_fpscr() & fpscr_dn_mask) != 0u;
}

std::uint8_t CpuState::interrupt_mask() const noexcept {
    return static_cast<std::uint8_t>((read_sr() & sr_interrupt_mask) >> 4u);
}

void CpuState::set_interrupt_mask(const std::uint8_t level) noexcept {
    const std::uint32_t clamped = level > 15u ? 15u : level;
    write_sr((read_sr() & ~sr_interrupt_mask) | (clamped << 4u));
}

bool CpuState::interrupts_blocked() const noexcept {
    return (read_sr() & sr_bl_mask) != 0u;
}

bool CpuState::privileged_mode() const noexcept {
    return (read_sr() & sr_md_mask) != 0u;
}

bool CpuState::register_bank_selected() const noexcept {
    return (read_sr() & sr_rb_mask) != 0u;
}

bool CpuState::fpu_disabled() const noexcept {
    return (read_sr() & sr_fd_mask) != 0u;
}

void reset_cpu(CpuState& cpu, const ResetState& state) noexcept {
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
    cpu.tea = 0u;
    cpu.expevt = 0u;
    cpu.intevt = 0u;
    cpu.mach = 0u;
    cpu.macl = 0u;
    cpu.fpul = 0u;
    cpu.fpscr = 0u;
    cpu.write_fpscr(state.fpscr);

    cpu.sr = 0u;
    cpu.t = false;
    cpu.s = false;
    cpu.q = false;
    cpu.m = false;
    cpu.trap_pending = false;
    cpu.last_exception_cause = ExceptionCause::None;
    cpu.exception_in_delay_slot = false;
    cpu.sleeping = false;
    cpu.last_prefetch_address = 0u;
    cpu.prefetch_count = 0u;
    cpu.last_prefetch_was_store_queue = false;

    cpu.r[15] = state.stack_pointer;
    cpu.write_sr(state.status_register);
}

void prefetch(CpuState& cpu, const std::uint32_t address) noexcept {
    cpu.last_prefetch_address = address;
    ++cpu.prefetch_count;
    cpu.last_prefetch_was_store_queue = address >= 0xE0000000u && address <= 0xE3FFFFFFu;
}

[[noreturn]] void unresolved_call(CpuState& cpu, const std::uint32_t target) {
    cpu.pc = target;
    throw std::runtime_error("Nicht aufgeloester Aufruf");
}

[[noreturn]] void unresolved_jump(CpuState& cpu, const std::uint32_t target) {
    cpu.pc = target;
    throw std::runtime_error("Nicht aufgeloester Sprung");
}

} // namespace katana::runtime
