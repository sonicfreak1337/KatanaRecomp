#include "katana/runtime/dynamic_interpreter.hpp"

#include "katana/runtime/exception.hpp"
#include "katana/runtime/fpu.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace katana::runtime {
namespace {
using Kind = sh4::InstructionKind;

std::uint32_t special_read(const CpuState& cpu, const sh4::SpecialRegister reg) {
    using Register = sh4::SpecialRegister;
    switch (reg) {
    case Register::Mach: return cpu.mach;
    case Register::Macl: return cpu.macl;
    case Register::Pr: return cpu.pr;
    case Register::Fpul: return cpu.fpul;
    case Register::Fpscr: return cpu.read_fpscr();
    case Register::Sr: return cpu.read_sr();
    case Register::Gbr: return cpu.gbr;
    case Register::Vbr: return cpu.vbr;
    case Register::Ssr: return cpu.ssr;
    case Register::Spc: return cpu.spc;
    case Register::Sgr: return cpu.sgr;
    case Register::Dbr: return cpu.dbr;
    case Register::Bank0: case Register::Bank1: case Register::Bank2: case Register::Bank3:
    case Register::Bank4: case Register::Bank5: case Register::Bank6: case Register::Bank7:
        return cpu.r_bank[static_cast<std::size_t>(reg) - static_cast<std::size_t>(Register::Bank0)];
    case Register::None: break;
    }
    throw std::invalid_argument("Dynamischer SH-4-Pfad liest ein unbekanntes Spezialregister.");
}

void special_write(CpuState& cpu, const sh4::SpecialRegister reg, const std::uint32_t value) {
    using Register = sh4::SpecialRegister;
    switch (reg) {
    case Register::Mach: cpu.mach = value; return;
    case Register::Macl: cpu.macl = value; return;
    case Register::Pr: cpu.pr = value; return;
    case Register::Fpul: cpu.fpul = value; return;
    case Register::Fpscr: cpu.write_fpscr(value); return;
    case Register::Sr: cpu.write_sr(value); return;
    case Register::Gbr: cpu.gbr = value; return;
    case Register::Vbr: cpu.vbr = value; return;
    case Register::Ssr: cpu.ssr = value; return;
    case Register::Spc: cpu.spc = value; return;
    case Register::Sgr: cpu.sgr = value; return;
    case Register::Dbr: cpu.dbr = value; return;
    case Register::Bank0: case Register::Bank1: case Register::Bank2: case Register::Bank3:
    case Register::Bank4: case Register::Bank5: case Register::Bank6: case Register::Bank7:
        cpu.r_bank[static_cast<std::size_t>(reg) - static_cast<std::size_t>(Register::Bank0)] = value;
        return;
    case Register::None: break;
    }
    throw std::invalid_argument("Dynamischer SH-4-Pfad schreibt ein unbekanntes Spezialregister.");
}

std::uint32_t add_displacement(const std::uint32_t base, const std::int32_t displacement) noexcept {
    return base + static_cast<std::uint32_t>(displacement);
}

void write_t(CpuState& cpu, const bool value) noexcept {
    cpu.t = value;
}

struct StepResult {
    bool control_flow = false;
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    std::uint64_t cycles = 1u;
    std::uint64_t instructions = 1u;
};

bool is_fpu_instruction(const sh4::DecodedInstruction& instruction) noexcept {
    switch (instruction.kind) {
    case Kind::FmovRegister:
    case Kind::FmovLoad:
    case Kind::FmovLoadPostIncrement:
    case Kind::FmovLoadR0Indexed:
    case Kind::FmovStore:
    case Kind::FmovStorePreDecrement:
    case Kind::FmovStoreR0Indexed:
    case Kind::Fldi0:
    case Kind::Fldi1:
    case Kind::Flds:
    case Kind::Fsts:
    case Kind::Fabs:
    case Kind::Fadd:
    case Kind::FcmpEqual:
    case Kind::FcmpGreater:
    case Kind::Fdiv:
    case Kind::FloatFromFpul:
    case Kind::Fmac:
    case Kind::Fmul:
    case Kind::Fneg:
    case Kind::Fsqrt:
    case Kind::Fsrra:
    case Kind::Fsca:
    case Kind::Fipr:
    case Kind::Ftrv:
    case Kind::Fsub:
    case Kind::Ftrc:
    case Kind::FcnvDoubleToSingle:
    case Kind::FcnvSingleToDouble:
    case Kind::Frchg:
    case Kind::Fschg:
        return true;
    case Kind::StoreSpecialRegister:
    case Kind::StoreSpecialRegisterPreDecrement:
    case Kind::LoadSpecialRegister:
    case Kind::LoadSpecialRegisterPostIncrement:
        return instruction.special_register == sh4::SpecialRegister::Fpul ||
               instruction.special_register == sh4::SpecialRegister::Fpscr;
    default:
        return false;
    }
}

StepResult execute_one(CpuState& cpu,
                       PlatformServices& services,
                       const std::uint32_t pc,
                       const std::optional<std::uint32_t> delay_owner = std::nullopt) {
    const auto opcode = guest_read_u16(cpu, pc);
    ++cpu.retired_guest_instructions;
    const auto instruction = sh4::decode(opcode);
    if (!instruction.is_known()) {
        raise_illegal_instruction(cpu, pc, delay_owner);
        return {true, BlockEndKind::Exception, 1u};
    }
    if (instruction.is_privileged && !cpu.privileged_mode()) {
        raise_illegal_instruction(cpu, pc, delay_owner);
        return {true, BlockEndKind::Exception, 1u};
    }
    if (delay_owner && instruction.changes_control_flow()) {
        raise_illegal_instruction(cpu, pc, delay_owner);
        return {true, BlockEndKind::Exception, 1u};
    }
    if (cpu.fpu_disabled() && is_fpu_instruction(instruction)) {
        raise_fpu_disabled(cpu, pc, delay_owner);
        return {true, BlockEndKind::Exception, 1u};
    }
    const auto n = instruction.destination_register;
    const auto m = instruction.source_register;
    const auto next = pc + 2u;
    cpu.pc = next;
    const auto guest_origin =
        cpu.memory.has_guest_memory_access_sink()
            ? GuestInstructionOrigin{unrelocate_code_address(pc), pc, true}
            : GuestInstructionOrigin{};
    const auto branch_target = [&]() {
        const auto target = sh4::calculate_direct_branch_target(instruction, pc);
        if (!target) throw std::logic_error("Direkter SH-4-Branch besitzt kein Ziel.");
        return *target;
    };
    const auto delay_then = [&](const std::uint32_t target, const BlockEndKind kind) {
        StepResult slot;
        try {
            slot = execute_one(cpu, services, next, pc);
        } catch (const MemoryAccessError& error) {
            enter_memory_exception(cpu, error, next, pc);
            slot = {true, BlockEndKind::Exception, 1u, 1u};
        }
        if (slot.end_kind == BlockEndKind::Exception)
            return StepResult{true,
                              BlockEndKind::Exception,
                              1u + slot.cycles,
                              1u + slot.instructions};
        cpu.pc = target;
        return StepResult{true, kind, 1u + slot.cycles, 1u + slot.instructions};
    };
    const auto call_delay_then = [&](const std::uint32_t target) {
        const auto old_pr = cpu.pr;
        cpu.pr = pc + 4u;
        const auto result = delay_then(target, BlockEndKind::Call);
        if (result.end_kind == BlockEndKind::Exception) cpu.pr = old_pr;
        return result;
    };

    switch (instruction.kind) {
    case Kind::Nop: return {};
    case Kind::MovImmediate: cpu.r[n] = static_cast<std::uint32_t>(instruction.immediate); return {};
    case Kind::AddImmediate: cpu.r[n] += static_cast<std::uint32_t>(instruction.immediate); return {};
    case Kind::MovRegister: cpu.r[n] = cpu.r[m]; return {};
    case Kind::AddRegister: cpu.r[n] += cpu.r[m]; return {};
    case Kind::SubRegister: cpu.r[n] -= cpu.r[m]; return {};
    case Kind::NegateRegister: cpu.r[n] = 0u - cpu.r[m]; return {};
    case Kind::NotRegister: cpu.r[n] = ~cpu.r[m]; return {};
    case Kind::AddWithCarry: {
        const auto old = cpu.r[n]; const auto sum = old + cpu.r[m];
        const auto result = sum + (cpu.t ? 1u : 0u);
        write_t(cpu, sum < old || result < sum); cpu.r[n] = result; return {};
    }
    case Kind::SubWithCarry: {
        const auto old = cpu.r[n]; const auto sub = old - cpu.r[m];
        const auto result = sub - (cpu.t ? 1u : 0u);
        write_t(cpu, old < cpu.r[m] || sub < (cpu.t ? 1u : 0u)); cpu.r[n] = result; return {};
    }
    case Kind::AddWithOverflow: {
        const auto a = static_cast<std::int32_t>(cpu.r[n]);
        const auto b = static_cast<std::int32_t>(cpu.r[m]);
        const auto result = static_cast<std::uint32_t>(cpu.r[n] + cpu.r[m]);
        const auto r = static_cast<std::int32_t>(result);
        write_t(cpu, (a >= 0 && b >= 0 && r < 0) || (a < 0 && b < 0 && r >= 0));
        cpu.r[n] = result; return {};
    }
    case Kind::SubWithOverflow: {
        const auto a = static_cast<std::int32_t>(cpu.r[n]);
        const auto b = static_cast<std::int32_t>(cpu.r[m]);
        const auto result = static_cast<std::uint32_t>(cpu.r[n] - cpu.r[m]);
        const auto r = static_cast<std::int32_t>(result);
        write_t(cpu, (a >= 0 && b < 0 && r < 0) || (a < 0 && b >= 0 && r >= 0));
        cpu.r[n] = result; return {};
    }
    case Kind::NegateWithCarry: {
        const auto temporary = 0u - cpu.r[m];
        const auto result = temporary - (cpu.t ? 1u : 0u);
        write_t(cpu, cpu.r[m] != 0u || result > temporary); cpu.r[n] = result; return {};
    }
    case Kind::ExtendUnsignedByte: cpu.r[n] = cpu.r[m] & 0xFFu; return {};
    case Kind::ExtendUnsignedWord: cpu.r[n] = cpu.r[m] & 0xFFFFu; return {};
    case Kind::ExtendSignedByte: cpu.r[n] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(cpu.r[m]))); return {};
    case Kind::ExtendSignedWord: cpu.r[n] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(cpu.r[m]))); return {};
    case Kind::SwapBytes: cpu.r[n] = (cpu.r[m] & 0xFFFF0000u) | ((cpu.r[m] & 0xFFu) << 8u) | ((cpu.r[m] >> 8u) & 0xFFu); return {};
    case Kind::SwapWords: cpu.r[n] = std::rotl(cpu.r[m], 16); return {};
    case Kind::ExtractMiddle: cpu.r[n] = (cpu.r[n] >> 16u) | (cpu.r[m] << 16u); return {};
    case Kind::DecrementAndTest: --cpu.r[n]; write_t(cpu, cpu.r[n] == 0u); return {};
    case Kind::MoveT: cpu.r[n] = cpu.t ? 1u : 0u; return {};
    case Kind::ShiftLogicalLeftOne: write_t(cpu, (cpu.r[n] >> 31u) != 0u); cpu.r[n] <<= 1u; return {};
    case Kind::ShiftLogicalRightOne: write_t(cpu, (cpu.r[n] & 1u) != 0u); cpu.r[n] >>= 1u; return {};
    case Kind::ShiftArithmeticLeftOne: write_t(cpu, (cpu.r[n] >> 31u) != 0u); cpu.r[n] <<= 1u; return {};
    case Kind::ShiftArithmeticRightOne: write_t(cpu, (cpu.r[n] & 1u) != 0u); cpu.r[n] = static_cast<std::uint32_t>(static_cast<std::int32_t>(cpu.r[n]) >> 1u); return {};
    case Kind::ShiftLogicalLeftTwo: cpu.r[n] <<= 2u; return {};
    case Kind::ShiftLogicalLeftEight: cpu.r[n] <<= 8u; return {};
    case Kind::ShiftLogicalLeftSixteen: cpu.r[n] <<= 16u; return {};
    case Kind::ShiftLogicalRightTwo: cpu.r[n] >>= 2u; return {};
    case Kind::ShiftLogicalRightEight: cpu.r[n] >>= 8u; return {};
    case Kind::ShiftLogicalRightSixteen: cpu.r[n] >>= 16u; return {};
    case Kind::RotateLeft: { const auto bit = cpu.r[n] >> 31u; cpu.r[n] = (cpu.r[n] << 1u) | bit; write_t(cpu, bit != 0u); return {}; }
    case Kind::RotateRight: { const auto bit = cpu.r[n] & 1u; cpu.r[n] = (cpu.r[n] >> 1u) | (bit << 31u); write_t(cpu, bit != 0u); return {}; }
    case Kind::RotateLeftThroughT: { const auto bit = cpu.r[n] >> 31u; cpu.r[n] = (cpu.r[n] << 1u) | (cpu.t ? 1u : 0u); write_t(cpu, bit != 0u); return {}; }
    case Kind::RotateRightThroughT: { const auto bit = cpu.r[n] & 1u; cpu.r[n] = (cpu.r[n] >> 1u) | (cpu.t ? 0x80000000u : 0u); write_t(cpu, bit != 0u); return {}; }
    case Kind::ShiftLogicalDynamic:
    case Kind::ShiftArithmeticDynamic: {
        const auto count = static_cast<std::int32_t>(cpu.r[m]);
        if (count >= 0) cpu.r[n] = (count & 0x1Fu) == 0u ? cpu.r[n] : cpu.r[n] << (count & 0x1Fu);
        else {
            const auto shift = ((~static_cast<std::uint32_t>(count)) & 0x1Fu) + 1u;
            if (shift == 32u) {
                cpu.r[n] = instruction.kind == Kind::ShiftArithmeticDynamic &&
                                   static_cast<std::int32_t>(cpu.r[n]) < 0
                               ? 0xFFFFFFFFu
                               : 0u;
            } else {
                cpu.r[n] = instruction.kind == Kind::ShiftArithmeticDynamic
                               ? static_cast<std::uint32_t>(
                                     static_cast<std::int32_t>(cpu.r[n]) >> shift)
                               : cpu.r[n] >> shift;
            }
        }
        return {};
    }
    case Kind::MultiplyLong: cpu.macl = cpu.r[n] * cpu.r[m]; return {};
    case Kind::MultiplySignedWord: cpu.macl = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(cpu.r[n])) * static_cast<std::int32_t>(static_cast<std::int16_t>(cpu.r[m]))); return {};
    case Kind::MultiplyUnsignedWord: cpu.macl = (cpu.r[n] & 0xFFFFu) * (cpu.r[m] & 0xFFFFu); return {};
    case Kind::DoubleMultiplySignedLong: { const auto value = static_cast<std::int64_t>(static_cast<std::int32_t>(cpu.r[n])) * static_cast<std::int64_t>(static_cast<std::int32_t>(cpu.r[m])); cpu.macl = static_cast<std::uint32_t>(value); cpu.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) >> 32u); return {}; }
    case Kind::DoubleMultiplyUnsignedLong: { const auto value = static_cast<std::uint64_t>(cpu.r[n]) * cpu.r[m]; cpu.macl = static_cast<std::uint32_t>(value); cpu.mach = static_cast<std::uint32_t>(value >> 32u); return {}; }
    case Kind::MultiplyAccumulateWord:
    case Kind::MultiplyAccumulateLong: {
        const auto bytes = instruction.kind == Kind::MultiplyAccumulateWord ? 2u : 4u;
        const auto destination_address = cpu.r[n];
        const auto source_address = cpu.r[m] + (m == n ? bytes : 0u);
        const auto destination = bytes == 2u
                                     ? static_cast<std::int64_t>(
                                           guest_read_s16_at(
                                               cpu, guest_origin, destination_address))
                                     : static_cast<std::int64_t>(
                                           static_cast<std::int32_t>(guest_read_u32_at(
                                               cpu, guest_origin, destination_address)));
        const auto source = bytes == 2u
                                ? static_cast<std::int64_t>(
                                      guest_read_s16_at(cpu, guest_origin, source_address))
                                : static_cast<std::int64_t>(
                                      static_cast<std::int32_t>(
                                          guest_read_u32_at(cpu, guest_origin, source_address)));
        cpu.r[n] += bytes;
        cpu.r[m] += bytes;
        const auto product = source * destination;
        if (cpu.s && bytes == 2u) {
            const auto accumulator = static_cast<std::int64_t>(static_cast<std::int32_t>(cpu.macl));
            const auto value = std::clamp<std::int64_t>(
                accumulator + product, std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::max());
            cpu.macl = static_cast<std::uint32_t>(value);
        } else if (cpu.s) {
            const auto raw = (static_cast<std::uint64_t>(cpu.mach & 0xFFFFu) << 32u) | cpu.macl;
            const auto accumulator = (raw & 0x0000800000000000ull) != 0u
                                         ? static_cast<std::int64_t>(raw) - 0x0001000000000000ll
                                         : static_cast<std::int64_t>(raw);
            const auto value = std::clamp<std::int64_t>(
                accumulator + product, -0x0000800000000000ll, 0x00007FFFFFFFFFFFll);
            const auto bits = static_cast<std::uint64_t>(value) & 0x0000FFFFFFFFFFFFull;
            cpu.mach = static_cast<std::uint32_t>((bits >> 32u) & 0xFFFFu);
            cpu.macl = static_cast<std::uint32_t>(bits);
        } else {
            const auto accumulator = (static_cast<std::uint64_t>(cpu.mach) << 32u) | cpu.macl;
            const auto value = accumulator + static_cast<std::uint64_t>(product);
            cpu.mach = static_cast<std::uint32_t>(value >> 32u);
            cpu.macl = static_cast<std::uint32_t>(value);
        }
        return {};
    }
    case Kind::ClearMac: cpu.mach = cpu.macl = 0u; return {};
    case Kind::DivideInitializeUnsigned: cpu.m = cpu.q = cpu.t = false; return {};
    case Kind::DivideInitializeSigned: cpu.q = (cpu.r[n] >> 31u) != 0u; cpu.m = (cpu.r[m] >> 31u) != 0u; cpu.t = cpu.q != cpu.m; return {};
    case Kind::DivideStep: {
        const auto old_q = cpu.q;
        cpu.q = (cpu.r[n] >> 31u) != 0u;
        const auto divisor = cpu.r[m];
        cpu.r[n] = (cpu.r[n] << 1u) | (cpu.t ? 1u : 0u);
        const auto shifted = cpu.r[n];
        if (!old_q) {
            if (!cpu.m) {
                cpu.r[n] = shifted - divisor;
                const auto borrow = cpu.r[n] > shifted;
                cpu.q = cpu.q != borrow;
            } else {
                cpu.r[n] = shifted + divisor;
                const auto carry = cpu.r[n] < shifted;
                cpu.q = (!cpu.q) != carry;
            }
        } else {
            if (!cpu.m) {
                cpu.r[n] = shifted + divisor;
                const auto carry = cpu.r[n] < shifted;
                cpu.q = cpu.q != carry;
            } else {
                cpu.r[n] = shifted - divisor;
                const auto borrow = cpu.r[n] > shifted;
                cpu.q = (!cpu.q) != borrow;
            }
        }
        cpu.t = cpu.q == cpu.m;
        return {};
    }
    case Kind::AndRegister: cpu.r[n] &= cpu.r[m]; return {};
    case Kind::OrRegister: cpu.r[n] |= cpu.r[m]; return {};
    case Kind::XorRegister: cpu.r[n] ^= cpu.r[m]; return {};
    case Kind::AndImmediate: cpu.r[0] &= static_cast<std::uint32_t>(instruction.immediate); return {};
    case Kind::OrImmediate: cpu.r[0] |= static_cast<std::uint32_t>(instruction.immediate); return {};
    case Kind::XorImmediate: cpu.r[0] ^= static_cast<std::uint32_t>(instruction.immediate); return {};
    case Kind::ClearS: cpu.s = false; return {};
    case Kind::SetS: cpu.s = true; return {};
    case Kind::ClearT: cpu.t = false; return {};
    case Kind::SetT: cpu.t = true; return {};
    case Kind::CompareEqualImmediate: write_t(cpu, cpu.r[0] == static_cast<std::uint32_t>(instruction.immediate)); return {};
    case Kind::CompareEqualRegister: write_t(cpu, cpu.r[n] == cpu.r[m]); return {};
    case Kind::CompareHigherOrSame: write_t(cpu, cpu.r[n] >= cpu.r[m]); return {};
    case Kind::CompareGreaterOrEqual: write_t(cpu, static_cast<std::int32_t>(cpu.r[n]) >= static_cast<std::int32_t>(cpu.r[m])); return {};
    case Kind::CompareHigher: write_t(cpu, cpu.r[n] > cpu.r[m]); return {};
    case Kind::CompareGreaterThan: write_t(cpu, static_cast<std::int32_t>(cpu.r[n]) > static_cast<std::int32_t>(cpu.r[m])); return {};
    case Kind::ComparePositiveOrZero: write_t(cpu, static_cast<std::int32_t>(cpu.r[n]) >= 0); return {};
    case Kind::ComparePositive: write_t(cpu, static_cast<std::int32_t>(cpu.r[n]) > 0); return {};
    case Kind::CompareString: { const auto value = cpu.r[n] ^ cpu.r[m]; write_t(cpu, (value & 0xFFu) == 0u || (value & 0xFF00u) == 0u || (value & 0xFF0000u) == 0u || (value & 0xFF000000u) == 0u); return {}; }
    case Kind::TestImmediate: write_t(cpu, (cpu.r[0] & static_cast<std::uint32_t>(instruction.immediate)) == 0u); return {};
    case Kind::TestRegister: write_t(cpu, (cpu.r[n] & cpu.r[m]) == 0u); return {};
    case Kind::TestByteImmediate: {
        const auto address = cpu.gbr + cpu.r[0];
        write_t(cpu,
                (guest_read_u8_at(cpu, guest_origin, address) &
                 static_cast<std::uint8_t>(instruction.immediate)) == 0u);
        return {};
    }
    case Kind::AndByteImmediate: {
        const auto address = cpu.gbr + cpu.r[0];
        guest_write_u8_at(
            cpu,
            guest_origin,
            address,
            static_cast<std::uint8_t>(
                guest_read_u8_at(cpu, guest_origin, address) & instruction.immediate));
        return {};
    }
    case Kind::XorByteImmediate: {
        const auto address = cpu.gbr + cpu.r[0];
        guest_write_u8_at(
            cpu,
            guest_origin,
            address,
            static_cast<std::uint8_t>(
                guest_read_u8_at(cpu, guest_origin, address) ^ instruction.immediate));
        return {};
    }
    case Kind::OrByteImmediate: {
        const auto address = cpu.gbr + cpu.r[0];
        guest_write_u8_at(
            cpu,
            guest_origin,
            address,
            static_cast<std::uint8_t>(
                guest_read_u8_at(cpu, guest_origin, address) | instruction.immediate));
        return {};
    }
    case Kind::TestAndSetByte: {
        const auto value = guest_read_u8_at(cpu, guest_origin, cpu.r[m]);
        guest_write_u8_at(
            cpu, guest_origin, cpu.r[m], static_cast<std::uint8_t>(value | 0x80u));
        write_t(cpu, value == 0u);
        return {};
    }
    case Kind::MovByteStore:
        guest_write_u8_at(
            cpu, guest_origin, cpu.r[n], static_cast<std::uint8_t>(cpu.r[m]));
        return {};
    case Kind::MovWordStore:
        guest_write_u16_at(
            cpu, guest_origin, cpu.r[n], static_cast<std::uint16_t>(cpu.r[m]));
        return {};
    case Kind::MovLongStore:
        guest_write_u32_at(cpu, guest_origin, cpu.r[n], cpu.r[m]);
        return {};
    case Kind::MovByteLoad:
        cpu.r[n] =
            static_cast<std::uint32_t>(guest_read_s8_at(cpu, guest_origin, cpu.r[m]));
        return {};
    case Kind::MovWordLoad:
        cpu.r[n] =
            static_cast<std::uint32_t>(guest_read_s16_at(cpu, guest_origin, cpu.r[m]));
        return {};
    case Kind::MovLongLoad:
        cpu.r[n] = guest_read_u32_at(cpu, guest_origin, cpu.r[m]);
        return {};
    case Kind::MovByteStorePreDecrement: {
        const auto value = static_cast<std::uint8_t>(cpu.r[m]);
        const auto address = cpu.r[n] - 1u;
        guest_write_u8_at(cpu, guest_origin, address, value);
        cpu.r[n] = address;
        return {};
    }
    case Kind::MovWordStorePreDecrement: {
        const auto value = static_cast<std::uint16_t>(cpu.r[m]);
        const auto address = cpu.r[n] - 2u;
        guest_write_u16_at(cpu, guest_origin, address, value);
        cpu.r[n] = address;
        return {};
    }
    case Kind::MovLongStorePreDecrement: {
        const auto value = cpu.r[m];
        const auto address = cpu.r[n] - 4u;
        guest_write_u32_at(cpu, guest_origin, address, value);
        cpu.r[n] = address;
        return {};
    }
    case Kind::MovByteLoadPostIncrement: {
        const auto address = cpu.r[m];
        cpu.r[n] =
            static_cast<std::uint32_t>(guest_read_s8_at(cpu, guest_origin, address));
        if (n != m) cpu.r[m] += 1u;
        return {};
    }
    case Kind::MovWordLoadPostIncrement: {
        const auto address = cpu.r[m];
        cpu.r[n] =
            static_cast<std::uint32_t>(guest_read_s16_at(cpu, guest_origin, address));
        if (n != m) cpu.r[m] += 2u;
        return {};
    }
    case Kind::MovLongLoadPostIncrement: {
        const auto address = cpu.r[m];
        cpu.r[n] = guest_read_u32_at(cpu, guest_origin, address);
        if (n != m) cpu.r[m] += 4u;
        return {};
    }
    case Kind::MovByteStoreDisplacement:
        guest_write_u8_at(cpu,
                          guest_origin,
                          add_displacement(cpu.r[n], instruction.displacement),
                          static_cast<std::uint8_t>(cpu.r[m]));
        return {};
    case Kind::MovWordStoreDisplacement:
        guest_write_u16_at(cpu,
                           guest_origin,
                           add_displacement(cpu.r[n], instruction.displacement),
                           static_cast<std::uint16_t>(cpu.r[m]));
        return {};
    case Kind::MovLongStoreDisplacement:
        guest_write_u32_at(cpu,
                           guest_origin,
                           add_displacement(cpu.r[n], instruction.displacement),
                           cpu.r[m]);
        return {};
    case Kind::MovByteLoadDisplacement:
        cpu.r[n] = static_cast<std::uint32_t>(guest_read_s8_at(
            cpu, guest_origin, add_displacement(cpu.r[m], instruction.displacement)));
        return {};
    case Kind::MovWordLoadDisplacement:
        cpu.r[n] = static_cast<std::uint32_t>(guest_read_s16_at(
            cpu, guest_origin, add_displacement(cpu.r[m], instruction.displacement)));
        return {};
    case Kind::MovLongLoadDisplacement:
        cpu.r[n] = guest_read_u32_at(
            cpu, guest_origin, add_displacement(cpu.r[m], instruction.displacement));
        return {};
    case Kind::MovByteStoreR0Indexed:
        guest_write_u8_at(cpu,
                          guest_origin,
                          cpu.r[n] + cpu.r[0],
                          static_cast<std::uint8_t>(cpu.r[m]));
        return {};
    case Kind::MovWordStoreR0Indexed:
        guest_write_u16_at(cpu,
                           guest_origin,
                           cpu.r[n] + cpu.r[0],
                           static_cast<std::uint16_t>(cpu.r[m]));
        return {};
    case Kind::MovLongStoreR0Indexed:
        guest_write_u32_at(cpu, guest_origin, cpu.r[n] + cpu.r[0], cpu.r[m]);
        return {};
    case Kind::MovByteLoadR0Indexed:
        cpu.r[n] = static_cast<std::uint32_t>(
            guest_read_s8_at(cpu, guest_origin, cpu.r[m] + cpu.r[0]));
        return {};
    case Kind::MovWordLoadR0Indexed:
        cpu.r[n] = static_cast<std::uint32_t>(
            guest_read_s16_at(cpu, guest_origin, cpu.r[m] + cpu.r[0]));
        return {};
    case Kind::MovLongLoadR0Indexed:
        cpu.r[n] = guest_read_u32_at(cpu, guest_origin, cpu.r[m] + cpu.r[0]);
        return {};
    case Kind::MovByteStoreGbrDisplacement:
        guest_write_u8_at(cpu,
                          guest_origin,
                          add_displacement(cpu.gbr, instruction.displacement),
                          static_cast<std::uint8_t>(cpu.r[0]));
        return {};
    case Kind::MovWordStoreGbrDisplacement:
        guest_write_u16_at(cpu,
                           guest_origin,
                           add_displacement(cpu.gbr, instruction.displacement),
                           static_cast<std::uint16_t>(cpu.r[0]));
        return {};
    case Kind::MovLongStoreGbrDisplacement:
        guest_write_u32_at(cpu,
                           guest_origin,
                           add_displacement(cpu.gbr, instruction.displacement),
                           cpu.r[0]);
        return {};
    case Kind::MovByteLoadGbrDisplacement:
        cpu.r[0] = static_cast<std::uint32_t>(guest_read_s8_at(
            cpu, guest_origin, add_displacement(cpu.gbr, instruction.displacement)));
        return {};
    case Kind::MovWordLoadGbrDisplacement:
        cpu.r[0] = static_cast<std::uint32_t>(guest_read_s16_at(
            cpu, guest_origin, add_displacement(cpu.gbr, instruction.displacement)));
        return {};
    case Kind::MovLongLoadGbrDisplacement:
        cpu.r[0] = guest_read_u32_at(
            cpu, guest_origin, add_displacement(cpu.gbr, instruction.displacement));
        return {};
    case Kind::MovWordLoadPcRelative:
        cpu.r[n] = static_cast<std::uint32_t>(guest_read_s16_at(
            cpu, guest_origin, add_displacement(pc + 4u, instruction.displacement)));
        return {};
    case Kind::MovLongLoadPcRelative:
        cpu.r[n] = guest_read_u32_at(
            cpu,
            guest_origin,
            add_displacement((pc + 4u) & ~3u, instruction.displacement));
        return {};
    case Kind::MoveAddressPcRelative: cpu.r[0] = add_displacement((pc + 4u) & ~3u, instruction.displacement); return {};
    case Kind::StoreSpecialRegister: cpu.r[n] = special_read(cpu, instruction.special_register); return {};
    case Kind::StoreSpecialRegisterPreDecrement: {
        const auto value = special_read(cpu, instruction.special_register);
        const auto address = cpu.r[n] - 4u;
        guest_write_u32_at(cpu, guest_origin, address, value);
        cpu.r[n] = address;
        return {};
    }
    case Kind::LoadSpecialRegister: special_write(cpu, instruction.special_register, cpu.r[m]); return {};
    case Kind::LoadSpecialRegisterPostIncrement: {
        const auto value = guest_read_u32_at(cpu, guest_origin, cpu.r[m]);
        cpu.r[m] += 4u;
        special_write(cpu, instruction.special_register, value);
        return {};
    }
    case Kind::TrapAlways: raise_trapa(cpu, static_cast<std::uint8_t>(instruction.immediate), pc); return {true, BlockEndKind::Exception, 1u};
    case Kind::ReturnFromException: {
        const auto target = cpu.spc;
        return_from_exception(cpu);
        StepResult slot;
        try {
            slot = execute_one(cpu, services, next, pc);
        } catch (const MemoryAccessError& error) {
            enter_memory_exception(cpu, error, next, pc);
            slot = {true, BlockEndKind::Exception, 1u, 1u};
        }
        if (slot.end_kind == BlockEndKind::Exception)
            return {true,
                    BlockEndKind::Exception,
                    1u + slot.cycles,
                    1u + slot.instructions};
        cpu.pc = target;
        return {true,
                BlockEndKind::ExceptionReturn,
                1u + slot.cycles,
                1u + slot.instructions};
    }
    case Kind::Sleep: cpu.sleeping = true; return {true, BlockEndKind::Sleep, 1u};
    case Kind::LoadTlb: load_tlb(cpu); return {};
    case Kind::Prefetch:
        static_cast<void>(services.prefetch(cpu, guest_origin, cpu.r[m]));
        return {};
    case Kind::Ocbi: static_cast<void>(maintain_coherent_operand_cache(OperandCacheOperation::Invalidate, cpu.r[m])); return {};
    case Kind::Ocbp: static_cast<void>(maintain_coherent_operand_cache(OperandCacheOperation::Purge, cpu.r[m])); return {};
    case Kind::Ocbwb: static_cast<void>(maintain_coherent_operand_cache(OperandCacheOperation::WriteBack, cpu.r[m])); return {};
    case Kind::MovcaLong:
        guest_write_u32_at(
            cpu, guest_origin, cpu.r[n], cpu.r[0], CodeWriteSource::StoreQueue);
        return {};
    case Kind::Bra: return delay_then(branch_target(), BlockEndKind::StaticBranch);
    case Kind::Bsr: return call_delay_then(branch_target());
    case Kind::Braf: return delay_then(pc + 4u + cpu.r[instruction.branch_register], BlockEndKind::DynamicBranch);
    case Kind::Bsrf: return call_delay_then(pc + 4u + cpu.r[instruction.branch_register]);
    case Kind::Bt: if (cpu.t) { cpu.pc = branch_target(); return {true, BlockEndKind::StaticBranch, 1u}; } return {};
    case Kind::Bf: if (!cpu.t) { cpu.pc = branch_target(); return {true, BlockEndKind::StaticBranch, 1u}; } return {};
    case Kind::BtS: return delay_then(cpu.t ? branch_target() : pc + 4u, BlockEndKind::ConditionalBranch);
    case Kind::BfS: return delay_then(!cpu.t ? branch_target() : pc + 4u, BlockEndKind::ConditionalBranch);
    case Kind::Jmp: return delay_then(cpu.r[instruction.branch_register], BlockEndKind::DynamicBranch);
    case Kind::Jsr: return call_delay_then(cpu.r[instruction.branch_register]);
    case Kind::Rts: return delay_then(cpu.pr, BlockEndKind::Return);
    case Kind::FmovRegister: if (cpu.fpu_transfer_pair()) write_fpu_pair_bits(cpu, n, read_fpu_pair_bits(cpu, m)); else cpu.fr[n] = cpu.fr[m]; return {};
    case Kind::FmovLoad:
        if (cpu.fpu_transfer_pair()) {
            const auto low = guest_read_u32_at(cpu, guest_origin, cpu.r[m]);
            const auto high = guest_read_u32_at(cpu, guest_origin, cpu.r[m] + 4u);
            write_fpu_pair_bits(
                cpu, n, (static_cast<std::uint64_t>(high) << 32u) | low);
        } else {
            cpu.fr[n] = guest_read_u32_at(cpu, guest_origin, cpu.r[m]);
        }
        return {};
    case Kind::FmovLoadPostIncrement: {
        const auto address = cpu.r[m];
        if (cpu.fpu_transfer_pair()) {
            const auto low = guest_read_u32_at(cpu, guest_origin, address);
            const auto high = guest_read_u32_at(cpu, guest_origin, address + 4u);
            write_fpu_pair_bits(
                cpu, n, (static_cast<std::uint64_t>(high) << 32u) | low);
            cpu.r[m] += 8u;
        } else {
            cpu.fr[n] = guest_read_u32_at(cpu, guest_origin, address);
            cpu.r[m] += 4u;
        }
        return {};
    }
    case Kind::FmovLoadR0Indexed: {
        const auto address = cpu.r[m] + cpu.r[0];
        if (cpu.fpu_transfer_pair()) {
            const auto low = guest_read_u32_at(cpu, guest_origin, address);
            const auto high = guest_read_u32_at(cpu, guest_origin, address + 4u);
            write_fpu_pair_bits(
                cpu, n, (static_cast<std::uint64_t>(high) << 32u) | low);
        } else {
            cpu.fr[n] = guest_read_u32_at(cpu, guest_origin, address);
        }
        return {};
    }
    case Kind::FmovStore:
        if (cpu.fpu_transfer_pair()) {
            const auto bits = read_fpu_pair_bits(cpu, m);
            guest_write_u32_at(cpu,
                               guest_origin,
                               cpu.r[n],
                               static_cast<std::uint32_t>(bits),
                               CodeWriteSource::Fpu);
            guest_write_u32_at(cpu,
                               guest_origin,
                               cpu.r[n] + 4u,
                               static_cast<std::uint32_t>(bits >> 32u),
                               CodeWriteSource::Fpu);
        } else {
            guest_write_u32_at(
                cpu, guest_origin, cpu.r[n], cpu.fr[m], CodeWriteSource::Fpu);
        }
        return {};
    case Kind::FmovStorePreDecrement: {
        const auto size = cpu.fpu_transfer_pair() ? 8u : 4u;
        const auto address = cpu.r[n] - size;
        if (size == 8u) {
            const auto bits = read_fpu_pair_bits(cpu, m);
            guest_write_u32_at(cpu,
                               guest_origin,
                               address,
                               static_cast<std::uint32_t>(bits),
                               CodeWriteSource::Fpu);
            guest_write_u32_at(cpu,
                               guest_origin,
                               address + 4u,
                               static_cast<std::uint32_t>(bits >> 32u),
                               CodeWriteSource::Fpu);
        } else {
            const auto bits = cpu.fr[m];
            guest_write_u32_at(
                cpu, guest_origin, address, bits, CodeWriteSource::Fpu);
        }
        cpu.r[n] = address;
        return {};
    }
    case Kind::FmovStoreR0Indexed: {
        const auto address = cpu.r[n] + cpu.r[0];
        if (cpu.fpu_transfer_pair()) {
            const auto bits = read_fpu_pair_bits(cpu, m);
            guest_write_u32_at(cpu,
                               guest_origin,
                               address,
                               static_cast<std::uint32_t>(bits),
                               CodeWriteSource::Fpu);
            guest_write_u32_at(cpu,
                               guest_origin,
                               address + 4u,
                               static_cast<std::uint32_t>(bits >> 32u),
                               CodeWriteSource::Fpu);
        } else {
            guest_write_u32_at(
                cpu, guest_origin, address, cpu.fr[m], CodeWriteSource::Fpu);
        }
        return {};
    }
    case Kind::Fldi0: write_fr_single(cpu, n, 0.0f); return {};
    case Kind::Fldi1: write_fr_single(cpu, n, 1.0f); return {};
    case Kind::Flds: cpu.fpul = cpu.fr[m]; return {};
    case Kind::Fsts: cpu.fr[n] = cpu.fpul; return {};
    case Kind::Fabs: fpu_absolute(cpu, n); return {};
    case Kind::Fadd: fpu_binary(cpu, FpuBinaryOperation::Add, m, n); return {};
    case Kind::Fsub: fpu_binary(cpu, FpuBinaryOperation::Subtract, m, n); return {};
    case Kind::Fmul: fpu_binary(cpu, FpuBinaryOperation::Multiply, m, n); return {};
    case Kind::Fdiv: fpu_binary(cpu, FpuBinaryOperation::Divide, m, n); return {};
    case Kind::FcmpEqual: fpu_compare_equal(cpu, m, n); return {};
    case Kind::FcmpGreater: fpu_compare_greater(cpu, m, n); return {};
    case Kind::FloatFromFpul: fpu_float_from_fpul(cpu, n); return {};
    case Kind::Fmac: fpu_multiply_accumulate(cpu, m, n); return {};
    case Kind::Fneg: fpu_negate(cpu, n); return {};
    case Kind::Fsqrt: fpu_square_root(cpu, n); return {};
    case Kind::Fsrra: fpu_reciprocal_square_root(cpu, n); return {};
    case Kind::Fsca: fpu_sine_cosine(cpu, n); return {};
    case Kind::Fipr: fpu_inner_product(cpu, m, n); return {};
    case Kind::Ftrv: fpu_transform_vector(cpu, n); return {};
    case Kind::Ftrc: fpu_truncate_to_fpul(cpu, n); return {};
    case Kind::FcnvDoubleToSingle: fpu_convert_double_to_single(cpu, m); return {};
    case Kind::FcnvSingleToDouble: fpu_convert_single_to_double(cpu, n); return {};
    case Kind::Frchg: cpu.toggle_fpu_register_bank(); return {};
    case Kind::Fschg: cpu.write_fpscr(cpu.read_fpscr() ^ fpscr_sz_mask); return {};
    case Kind::Unknown:
        raise_illegal_instruction(cpu, pc, delay_owner);
        return {true, BlockEndKind::Exception, 1u};
    }
    raise_illegal_instruction(cpu, pc, delay_owner);
    return {true, BlockEndKind::Exception, 1u};
}
} // namespace

DynamicInterpreterResult execute_dynamic_sh4_block(CpuState& cpu,
                                                    PlatformServices& services,
                                                    const std::uint64_t maximum_instructions) {
    if (maximum_instructions == 0u)
        throw std::invalid_argument("Dynamischer SH-4-Block braucht ein Instruktionsbudget.");
    DynamicInterpreterResult result;
    result.start_pc = cpu.pc;
    for (; result.instructions < maximum_instructions;) {
        const auto instruction_pc = cpu.pc;
        StepResult step;
        try {
            step = execute_one(cpu, services, instruction_pc);
        } catch (const MemoryAccessError& error) {
            enter_memory_exception(cpu, error, instruction_pc);
            step = {true, BlockEndKind::Exception, 1u};
        }
        result.instructions += step.instructions;
        result.guest_cycles += step.cycles;
        const auto bytes = static_cast<std::uint64_t>(instruction_pc - result.start_pc) + 2u;
        result.byte_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, 0xFFFFFFFFu));
        if (step.control_flow) {
            result.end_kind = step.end_kind;
            return result;
        }
    }
    result.end_kind = BlockEndKind::DynamicBranch;
    return result;
}

} // namespace katana::runtime
