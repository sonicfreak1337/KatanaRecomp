#include "katana/runtime/exception.hpp"

namespace katana::runtime {

void enter_exception(
    CpuState& cpu,
    const ExceptionRequest& request
) noexcept {
    const std::uint32_t saved_sr = cpu.read_sr();
    cpu.ssr = saved_sr;
    cpu.spc = request.return_pc;
    cpu.sgr = cpu.r[15];

    if (request.fault_address.has_value()) {
        cpu.tea = *request.fault_address;
    }

    if (request.interrupt) {
        cpu.intevt = request.event_code;
    } else {
        cpu.expevt = request.event_code;
    }

    cpu.last_exception_cause = request.cause;
    cpu.exception_in_delay_slot = request.in_delay_slot;
    cpu.trap_pending = true;
    cpu.sleeping = false;
    cpu.write_sr(saved_sr | sr_md_mask | sr_rb_mask | sr_bl_mask);
    cpu.pc = cpu.vbr + request.vector_offset;
}

void raise_trapa(
    CpuState& cpu,
    const std::uint8_t immediate,
    const std::uint32_t instruction_pc
) noexcept {
    cpu.tra = static_cast<std::uint32_t>(immediate) << 2u;
    enter_exception(
        cpu,
        ExceptionRequest{
            ExceptionCause::Trap,
            event_trapa,
            general_exception_vector,
            instruction_pc + 2u
        }
    );
}

void raise_illegal_instruction(
    CpuState& cpu,
    const std::uint32_t instruction_pc,
    const std::optional<std::uint32_t> delay_slot_owner
) noexcept {
    const bool in_delay_slot = delay_slot_owner.has_value();
    enter_exception(
        cpu,
        ExceptionRequest{
            in_delay_slot
                ? ExceptionCause::SlotIllegalInstruction
                : ExceptionCause::IllegalInstruction,
            in_delay_slot
                ? event_slot_illegal_instruction
                : event_illegal_instruction,
            general_exception_vector,
            delay_slot_owner.value_or(instruction_pc),
            std::nullopt,
            false,
            in_delay_slot
        }
    );
}

void raise_fpu_disabled(
    CpuState& cpu,
    const std::uint32_t instruction_pc,
    const std::optional<std::uint32_t> delay_slot_owner
) noexcept {
    const bool in_delay_slot = delay_slot_owner.has_value();
    enter_exception(
        cpu,
        ExceptionRequest{
            in_delay_slot
                ? ExceptionCause::SlotFpuDisabled
                : ExceptionCause::FpuDisabled,
            in_delay_slot
                ? event_slot_fpu_disabled
                : event_fpu_disabled,
            general_exception_vector,
            delay_slot_owner.value_or(instruction_pc),
            std::nullopt,
            false,
            in_delay_slot
        }
    );
}

void enter_memory_exception(
    CpuState& cpu,
    const MemoryAccessError& error,
    const std::uint32_t instruction_pc,
    const std::optional<std::uint32_t> delay_slot_owner
) noexcept {
    const bool write = error.operation() == MemoryAccessOperation::Write;
    const bool address_error =
        error.reason() == MemoryAccessErrorReason::Misaligned;
    const bool in_delay_slot = delay_slot_owner.has_value();

    ExceptionCause cause;
    if (address_error) {
        cause = write
            ? ExceptionCause::AddressErrorWrite
            : ExceptionCause::AddressErrorRead;
    } else {
        cause = write
            ? ExceptionCause::BusErrorWrite
            : ExceptionCause::BusErrorRead;
    }

    enter_exception(
        cpu,
        ExceptionRequest{
            cause,
            write ? event_address_error_write : event_address_error_read,
            general_exception_vector,
            delay_slot_owner.value_or(instruction_pc),
            error.address(),
            false,
            in_delay_slot
        }
    );
}

void return_from_exception(CpuState& cpu) noexcept {
    const std::uint32_t return_pc = cpu.spc;
    const std::uint32_t return_sr = cpu.ssr;
    cpu.write_sr(return_sr);
    cpu.pc = return_pc;
    cpu.trap_pending = false;
    cpu.exception_in_delay_slot = false;
}

} // namespace katana::runtime
