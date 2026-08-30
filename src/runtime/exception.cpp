#include "katana/runtime/exception.hpp"
#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/block_table.hpp"

#include <array>
#include <limits>
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
    ExceptionMetadata{ExceptionCause::FpuException,
                      event_fpu_exception,
                      general_exception_vector,
                      false},
};

static_assert(exception_table.size() == static_cast<std::size_t>(ExceptionCause::FpuException) + 1u);

std::uint32_t instruction_physical_pc(const CpuState& cpu,
                                      const std::uint32_t instruction_pc) noexcept {
    if (instruction_pc == cpu.active_instruction_pc)
        return cpu.active_instruction_physical_pc;
    const auto offset = instruction_pc - cpu.active_block_virtual_start;
    if (cpu.active_block_size != 0u && offset < cpu.active_block_size)
        return cpu.active_block_physical_start + offset;
    return canonical_physical_address(instruction_pc);
}

std::uint64_t next_exception_generation(const std::uint64_t current) noexcept {
    return current == std::numeric_limits<std::uint64_t>::max() ? 1u : current + 1u;
}

bool exception_updates_pteh(const ExceptionCause cause) noexcept {
    switch (cause) {
    case ExceptionCause::AddressErrorRead:
    case ExceptionCause::AddressErrorWrite:
    case ExceptionCause::TlbMissRead:
    case ExceptionCause::TlbMissWrite:
    case ExceptionCause::InitialPageWrite:
    case ExceptionCause::TlbProtectionRead:
    case ExceptionCause::TlbProtectionWrite:
    case ExceptionCause::TlbMultipleHit:
        return true;
    default:
        return false;
    }
}

MemoryFaultProvenance make_memory_fault_provenance(
    const MemoryAccessError& error,
    const std::uint32_t instruction_pc,
    const std::optional<std::uint32_t> instruction_opcode) noexcept {
    const auto origin = error.instruction();
    MemoryFaultProvenance provenance;
    provenance.valid = true;
    provenance.source_pc = origin.valid && origin.source_pc != 0u
                               ? origin.source_pc
                               : instruction_pc;
    provenance.runtime_pc = origin.valid && origin.runtime_pc != 0u
                                ? origin.runtime_pc
                                : instruction_pc;
    provenance.instruction_valid = provenance.source_pc != 0u &&
                                    provenance.runtime_pc != 0u;
    provenance.address = error.address();
    provenance.operation = error.operation();
    provenance.width = error.width();
    provenance.access_valid = true;
    if (instruction_opcode.has_value()) {
        provenance.opcode = *instruction_opcode;
        provenance.opcode_valid = *instruction_opcode != 0u;
    }
    return provenance;
}

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

void request_manual_reset(CpuState& cpu, const ManualResetRequest& request) noexcept {
    cpu.last_memory_fault_provenance = {};
    const auto generation = next_exception_generation(cpu.exception_generation);
    const auto attempted = cpu.attempted_guest_instructions;
    const auto retired = cpu.retired_guest_instructions;
    const auto total_cycles = cpu.total_guest_cycles;
    const auto pending_cycles = cpu.pending_guest_cycles;
    const auto physical_instruction_pc =
        instruction_physical_pc(cpu, request.instruction_pc);
    const auto fault_pteh =
        exception_updates_pteh(request.cause) && request.fault_address.has_value()
            ? std::optional<std::uint32_t>{
                  (cpu.pteh & 0x000000FFu) |
                  (*request.fault_address & 0xFFFFFC00u)}
            : std::nullopt;
    const auto sink = cpu.manual_reset_sink;
    if (sink) {
        sink.callback(sink.context, cpu, request);
    } else {
        reset_cpu(cpu,
                  ResetState{tlb_multiple_hit_reset_vector,
                             0u,
                             0u,
                             sr_md_mask | sr_rb_mask | sr_bl_mask | sr_interrupt_mask,
                             0u});
    }

    cpu.exception_generation = generation;
    cpu.attempted_guest_instructions = attempted;
    cpu.retired_guest_instructions = retired;
    cpu.total_guest_cycles = total_cycles;
    cpu.pending_guest_cycles = pending_cycles;
    cpu.last_exception_generation = generation;
    cpu.last_exception_cause = request.cause;
    cpu.last_exception_instruction_pc = request.instruction_pc;
    cpu.last_exception_instruction_physical_pc = physical_instruction_pc;
    cpu.last_exception_owner_pc = request.owner_pc;
    cpu.exception_in_delay_slot = request.in_delay_slot;
    cpu.expevt = request.event_code;
    if (request.fault_address.has_value()) cpu.tea = *request.fault_address;
    if (fault_pteh.has_value()) {
        cpu.pteh = *fault_pteh;
        if (cpu.address_space) cpu.address_space->write_pteh(cpu.pteh);
    }
    cpu.pc = tlb_multiple_hit_reset_vector;
    cpu.write_sr(sr_md_mask | sr_rb_mask | sr_bl_mask | sr_interrupt_mask);
    cpu.trap_pending = false;
    cpu.sleeping = false;
}

void enter_exception(CpuState& cpu, const ExceptionRequest& request) noexcept {
    cpu.last_memory_fault_provenance = {};
    const auto metadata = exception_metadata(request.cause, request.event_code);
    const std::uint32_t saved_sr = cpu.read_sr();
    const auto instruction_pc =
        request.instruction_pc.value_or(cpu.active_instruction_pc);
    const auto owner_pc = request.delay_slot_owner_pc.value_or(instruction_pc);
    if (!metadata.interrupt && metadata.vector_offset == general_exception_vector &&
        (saved_sr & sr_bl_mask) != 0u) {
        request_manual_reset(
            cpu,
            ManualResetRequest{ManualResetReason::BlockedGeneralException,
                               metadata.cause,
                               metadata.event_code,
                               request.fault_address,
                               instruction_pc,
                               owner_pc,
                               request.in_delay_slot});
        return;
    }

    cpu.exception_generation = next_exception_generation(cpu.exception_generation);
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
    cpu.last_exception_instruction_pc = instruction_pc;
    cpu.last_exception_instruction_physical_pc =
        instruction_physical_pc(cpu, instruction_pc);
    cpu.last_exception_owner_pc = owner_pc;
    cpu.last_exception_generation = cpu.exception_generation;
    cpu.trap_pending = true;
    cpu.sleeping = false;
    cpu.write_sr(saved_sr | sr_md_mask | sr_rb_mask | sr_bl_mask);
    cpu.pc = cpu.vbr + metadata.vector_offset;
}

void raise_trapa(CpuState& cpu,
                 const std::uint8_t immediate,
                 const std::uint32_t instruction_pc) noexcept {
    cpu.tra = static_cast<std::uint32_t>(immediate) << 2u;
    auto request = ExceptionRequest{
        ExceptionCause::Trap, event_trapa, general_exception_vector, instruction_pc + 2u};
    request.instruction_pc = instruction_pc;
    enter_exception(cpu, request);
}

void raise_illegal_instruction(CpuState& cpu,
                               const std::uint32_t instruction_pc,
                               const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    const bool in_delay_slot = delay_slot_owner.has_value();
    auto request =
        ExceptionRequest{in_delay_slot ? ExceptionCause::SlotIllegalInstruction
                                       : ExceptionCause::IllegalInstruction,
                         in_delay_slot ? event_slot_illegal_instruction
                                       : event_illegal_instruction,
                         general_exception_vector,
                         delay_slot_owner.value_or(instruction_pc),
                         std::nullopt,
                         false,
                         in_delay_slot};
    request.instruction_pc = instruction_pc;
    request.delay_slot_owner_pc = delay_slot_owner;
    enter_exception(cpu, request);
}

void raise_fpu_disabled(CpuState& cpu,
                        const std::uint32_t instruction_pc,
                        const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    const bool in_delay_slot = delay_slot_owner.has_value();
    auto request =
        ExceptionRequest{in_delay_slot ? ExceptionCause::SlotFpuDisabled
                                       : ExceptionCause::FpuDisabled,
                         in_delay_slot ? event_slot_fpu_disabled : event_fpu_disabled,
                         general_exception_vector,
                         delay_slot_owner.value_or(instruction_pc),
                         std::nullopt,
                         false,
                         in_delay_slot};
    request.instruction_pc = instruction_pc;
    request.delay_slot_owner_pc = delay_slot_owner;
    enter_exception(cpu, request);
}

void enter_memory_exception_impl(
    CpuState& cpu,
    const MemoryAccessError& error,
    const std::uint32_t instruction_pc,
    const std::optional<std::uint32_t> instruction_opcode,
    const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    const auto provenance = make_memory_fault_provenance(
        error, instruction_pc, instruction_opcode);
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

    if (exception_updates_pteh(metadata.cause))
        cpu.pteh = (cpu.pteh & 0x000000FFu) | (error.address() & 0xFFFFFC00u);

    if (cause == ExceptionCause::TlbMultipleHit) {
        request_manual_reset(
            cpu,
            ManualResetRequest{ManualResetReason::TlbMultipleHit,
                               cause,
                               event_tlb_multiple_hit,
                               error.address(),
                               instruction_pc,
                               delay_slot_owner.value_or(instruction_pc),
                               in_delay_slot});
        cpu.last_memory_fault_provenance = provenance;
        return;
    }

    auto request = ExceptionRequest{cause,
                                    metadata.event_code,
                                    general_exception_vector,
                                    delay_slot_owner.value_or(instruction_pc),
                                    error.address(),
                                    false,
                                    in_delay_slot};
    request.instruction_pc = instruction_pc;
    request.delay_slot_owner_pc = delay_slot_owner;
    enter_exception(cpu, request);
    cpu.last_memory_fault_provenance = provenance;
}

void enter_memory_exception(CpuState& cpu,
                            const MemoryAccessError& error,
                            const std::uint32_t instruction_pc,
                            const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    enter_memory_exception_impl(
        cpu, error, instruction_pc, std::nullopt, delay_slot_owner);
}

void enter_memory_exception_with_provenance(
    CpuState& cpu,
    const MemoryAccessError& error,
    const std::uint32_t instruction_pc,
    const std::uint32_t instruction_opcode,
    const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    enter_memory_exception_impl(
        cpu, error, instruction_pc, instruction_opcode, delay_slot_owner);
}

void record_memory_fault_provenance(
    CpuState& cpu,
    const MemoryAccessError& error,
    const std::uint32_t instruction_pc,
    const std::optional<std::uint32_t> instruction_opcode) noexcept {
    cpu.last_memory_fault_provenance = make_memory_fault_provenance(
        error, instruction_pc, instruction_opcode);
}

void return_from_exception(CpuState& cpu) noexcept {
    const std::uint32_t return_pc = cpu.spc;
    const std::uint32_t return_sr = cpu.ssr;
    cpu.write_sr(return_sr);
    cpu.pc = return_pc;
    cpu.trap_pending = false;
    cpu.last_exception_cause = ExceptionCause::None;
    cpu.exception_in_delay_slot = false;
    cpu.last_memory_fault_provenance = {};
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
