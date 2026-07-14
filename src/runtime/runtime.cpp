#include "katana/runtime/runtime.hpp"

#include <stdexcept>
#include <utility>

namespace katana::runtime {

Memory::Memory(const std::size_t size)
    : bytes_(size, 0u) {}

std::size_t Memory::size() const noexcept {
    return bytes_.size();
}

std::uint8_t Memory::read_u8(const std::uint32_t address) const {
    check(address, 1u);
    return bytes_[static_cast<std::size_t>(address)];
}

std::uint16_t Memory::read_u16(const std::uint32_t address) const {
    check(address, 2u);
    const auto offset = static_cast<std::size_t>(address);
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes_[offset]) |
        (static_cast<std::uint16_t>(bytes_[offset + 1u]) << 8u)
    );
}

std::uint32_t Memory::read_u32(const std::uint32_t address) const {
    check(address, 4u);
    const auto offset = static_cast<std::size_t>(address);
    return
        static_cast<std::uint32_t>(bytes_[offset]) |
        (static_cast<std::uint32_t>(bytes_[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes_[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes_[offset + 3u]) << 24u);
}

std::uint32_t Memory::read_s8(const std::uint32_t address) const {
    const auto value = read_u8(address);
    return (value & 0x80u) != 0u
        ? 0xFFFFFF00u | static_cast<std::uint32_t>(value)
        : static_cast<std::uint32_t>(value);
}

std::uint32_t Memory::read_s16(const std::uint32_t address) const {
    const auto value = read_u16(address);
    return (value & 0x8000u) != 0u
        ? 0xFFFF0000u | static_cast<std::uint32_t>(value)
        : static_cast<std::uint32_t>(value);
}

void Memory::write_u8(
    const std::uint32_t address,
    const std::uint8_t value
) {
    check(address, 1u);
    bytes_[static_cast<std::size_t>(address)] = value;
}

void Memory::write_u16(
    const std::uint32_t address,
    const std::uint16_t value
) {
    check(address, 2u);
    const auto offset = static_cast<std::size_t>(address);
    bytes_[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes_[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
}

void Memory::write_u32(
    const std::uint32_t address,
    const std::uint32_t value
) {
    check(address, 4u);
    const auto offset = static_cast<std::size_t>(address);
    bytes_[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes_[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
    bytes_[offset + 2u] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
    bytes_[offset + 3u] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
}

void Memory::check(
    const std::uint32_t address,
    const std::size_t width
) const {
    const auto offset = static_cast<std::size_t>(address);
    if (offset > bytes_.size() || width > bytes_.size() - offset) {
        throw std::out_of_range(
            "Speicherzugriff ausserhalb des Runtime-Speichers"
        );
    }
}

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

}