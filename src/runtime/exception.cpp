#include "katana/runtime/exception.hpp"

#include <array>
#include <memory>
#include <stdexcept>

namespace katana::runtime {
namespace {

constexpr std::array exception_table = {
    ExceptionMetadata{ExceptionCause::None, 0u, general_exception_vector, false},
    ExceptionMetadata{ExceptionCause::Trap, event_trapa, general_exception_vector, false},
    ExceptionMetadata{ExceptionCause::IllegalInstruction,
                      event_illegal_instruction,
                      general_exception_vector,
                      false},
    ExceptionMetadata{ExceptionCause::SlotIllegalInstruction,
                      event_slot_illegal_instruction,
                      general_exception_vector,
                      false},
    ExceptionMetadata{
        ExceptionCause::FpuDisabled, event_fpu_disabled, general_exception_vector, false},
    ExceptionMetadata{
        ExceptionCause::SlotFpuDisabled, event_slot_fpu_disabled, general_exception_vector, false},
    ExceptionMetadata{ExceptionCause::AddressErrorRead,
                      event_address_error_read,
                      general_exception_vector,
                      false},
    ExceptionMetadata{ExceptionCause::AddressErrorWrite,
                      event_address_error_write,
                      general_exception_vector,
                      false},
    ExceptionMetadata{
        ExceptionCause::TlbMissRead, event_tlb_miss_read, tlb_miss_exception_vector, false},
    ExceptionMetadata{
        ExceptionCause::TlbMissWrite, event_tlb_miss_write, tlb_miss_exception_vector, false},
    ExceptionMetadata{ExceptionCause::InitialPageWrite,
                      event_initial_page_write,
                      general_exception_vector,
                      false},
    ExceptionMetadata{ExceptionCause::TlbProtectionRead,
                      event_tlb_protection_read,
                      general_exception_vector,
                      false},
    ExceptionMetadata{ExceptionCause::TlbProtectionWrite,
                      event_tlb_protection_write,
                      general_exception_vector,
                      false},
    ExceptionMetadata{
        ExceptionCause::TlbMultipleHit, event_tlb_multiple_hit, general_exception_vector, false},
    // Host-side bus failures enter the architectural data-address exception class.
    ExceptionMetadata{ExceptionCause::AddressErrorRead,
                      event_address_error_read,
                      general_exception_vector,
                      false},
    ExceptionMetadata{ExceptionCause::AddressErrorWrite,
                      event_address_error_write,
                      general_exception_vector,
                      false},
    ExceptionMetadata{ExceptionCause::Interrupt, 0u, interrupt_vector, true},
};

static_assert(exception_table.size() == static_cast<std::size_t>(ExceptionCause::Interrupt) + 1u);

} // namespace

ExceptionMetadata exception_metadata(const ExceptionCause cause,
                                     const std::uint32_t interrupt_event_code) noexcept {
    const auto index = static_cast<std::size_t>(cause);
    auto metadata = index < exception_table.size() ? exception_table[index] : exception_table[0];
    if (metadata.interrupt) {
        metadata.event_code = interrupt_event_code;
    }
    return metadata;
}

void enter_exception(CpuState& cpu, const ExceptionRequest& request) noexcept {
    const auto metadata = exception_metadata(request.cause, request.event_code);
    const std::uint32_t saved_sr = cpu.read_sr();
    cpu.ssr = saved_sr;
    cpu.spc = request.return_pc;
    cpu.sgr = cpu.r[15];

    if (request.fault_address.has_value()) {
        cpu.tea = *request.fault_address;
    }

    if (metadata.interrupt) {
        cpu.intevt = metadata.event_code;
    } else {
        cpu.expevt = metadata.event_code;
    }

    cpu.last_exception_cause = metadata.cause;
    cpu.exception_in_delay_slot = request.in_delay_slot;
    cpu.trap_pending = true;
    cpu.sleeping = false;
    cpu.write_sr(saved_sr | sr_md_mask | sr_rb_mask | sr_bl_mask);
    cpu.pc = cpu.vbr + metadata.vector_offset;
}

void raise_trapa(CpuState& cpu,
                 const std::uint8_t immediate,
                 const std::uint32_t instruction_pc) noexcept {
    cpu.tra = static_cast<std::uint32_t>(immediate) << 2u;
    enter_exception(
        cpu,
        ExceptionRequest{
            ExceptionCause::Trap, event_trapa, general_exception_vector, instruction_pc + 2u});
}

void raise_illegal_instruction(CpuState& cpu,
                               const std::uint32_t instruction_pc,
                               const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    const bool in_delay_slot = delay_slot_owner.has_value();
    enter_exception(
        cpu,
        ExceptionRequest{in_delay_slot ? ExceptionCause::SlotIllegalInstruction
                                       : ExceptionCause::IllegalInstruction,
                         in_delay_slot ? event_slot_illegal_instruction : event_illegal_instruction,
                         general_exception_vector,
                         delay_slot_owner.value_or(instruction_pc),
                         std::nullopt,
                         false,
                         in_delay_slot});
}

void raise_fpu_disabled(CpuState& cpu,
                        const std::uint32_t instruction_pc,
                        const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    const bool in_delay_slot = delay_slot_owner.has_value();
    enter_exception(cpu,
                    ExceptionRequest{in_delay_slot ? ExceptionCause::SlotFpuDisabled
                                                   : ExceptionCause::FpuDisabled,
                                     in_delay_slot ? event_slot_fpu_disabled : event_fpu_disabled,
                                     general_exception_vector,
                                     delay_slot_owner.value_or(instruction_pc),
                                     std::nullopt,
                                     false,
                                     in_delay_slot});
}

void enter_memory_exception(CpuState& cpu,
                            const MemoryAccessError& error,
                            const std::uint32_t instruction_pc,
                            const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    const bool write = error.operation() == MemoryAccessOperation::Write;
    const bool in_delay_slot = delay_slot_owner.has_value();
    auto cause = write ? ExceptionCause::AddressErrorWrite : ExceptionCause::AddressErrorRead;
    switch (error.reason()) {
    case MemoryAccessErrorReason::TlbMiss:
        cause = write ? ExceptionCause::TlbMissWrite : ExceptionCause::TlbMissRead;
        break;
    case MemoryAccessErrorReason::TlbMultipleHit:
        cause = ExceptionCause::TlbMultipleHit;
        break;
    case MemoryAccessErrorReason::InitialPageWrite:
        cause = ExceptionCause::InitialPageWrite;
        break;
    case MemoryAccessErrorReason::TlbProtection:
        cause = write ? ExceptionCause::TlbProtectionWrite : ExceptionCause::TlbProtectionRead;
        break;
    case MemoryAccessErrorReason::DeviceRejected:
        cause = write ? ExceptionCause::BusErrorWrite : ExceptionCause::BusErrorRead;
        break;
    default:
        break;
    }
    const auto metadata = exception_metadata(cause);

    const bool updates_pteh = cause == ExceptionCause::TlbMissRead ||
                              cause == ExceptionCause::TlbMissWrite ||
                              cause == ExceptionCause::InitialPageWrite ||
                              cause == ExceptionCause::TlbProtectionRead ||
                              cause == ExceptionCause::TlbProtectionWrite ||
                              cause == ExceptionCause::TlbMultipleHit;
    if (updates_pteh)
        cpu.pteh = (cpu.pteh & 0x000003FFu) | (error.address() & 0xFFFFFC00u);

    if (cause == ExceptionCause::TlbMultipleHit) {
        cpu.tea = error.address();
        cpu.expevt = event_tlb_multiple_hit;
        cpu.vbr = 0u;
        cpu.write_sr(sr_md_mask | sr_rb_mask | sr_bl_mask | sr_interrupt_mask);
        cpu.pc = tlb_multiple_hit_reset_vector;
        cpu.last_exception_cause = cause;
        cpu.exception_in_delay_slot = in_delay_slot;
        cpu.trap_pending = true;
        cpu.sleeping = false;
        return;
    }

    enter_exception(cpu,
                    ExceptionRequest{cause,
                                     metadata.event_code,
                                     general_exception_vector,
                                     delay_slot_owner.value_or(instruction_pc),
                                     error.address(),
                                     false,
                                     in_delay_slot});
}

void return_from_exception(CpuState& cpu) noexcept {
    const std::uint32_t return_pc = cpu.spc;
    const std::uint32_t return_sr = cpu.ssr;
    cpu.write_sr(return_sr);
    cpu.pc = return_pc;
    cpu.trap_pending = false;
    cpu.last_exception_cause = ExceptionCause::None;
    cpu.exception_in_delay_slot = false;
}

void map_sh4_exception_event_registers(Memory& memory, CpuState& cpu) {
    constexpr std::uint32_t tra_mask = 0x000003FCu;
    constexpr std::uint32_t event_mask = 0x00000FFFu;
    auto device = std::make_shared<MmioMemoryDevice>(
        3u * sizeof(std::uint32_t),
        [&cpu](const std::uint32_t offset, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word || (offset & 3u) != 0u || offset > 8u)
                throw std::invalid_argument(
                    "SH-4-Exceptionregister erlauben nur ausgerichtete 32-Bit-Zugriffe.");
            if (offset == 0u) return cpu.tra & tra_mask;
            if (offset == 4u) return cpu.expevt & event_mask;
            return cpu.intevt & event_mask;
        },
        [&cpu](
            const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word || (offset & 3u) != 0u || offset > 8u)
                throw std::invalid_argument(
                    "SH-4-Exceptionregister erlauben nur ausgerichtete 32-Bit-Zugriffe.");
            if (offset == 0u)
                cpu.tra = value & tra_mask;
            else if (offset == 4u)
                cpu.expevt = value & event_mask;
            else
                cpu.intevt = value & event_mask;
        });
    memory.map_region("sh4-exception-events-p4", sh4_tra_address, device);
    memory.map_region("sh4-exception-events-area7", sh4_exception_area7_address, std::move(device));
}

} // namespace katana::runtime
