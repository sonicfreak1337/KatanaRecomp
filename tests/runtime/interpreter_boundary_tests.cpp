#include "katana/runtime/interpreter_boundary.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        EventScheduler scheduler;
        SchedulerSafepoints safepoints(scheduler, 12u, 4u);
        ExecutableCodeTracker tracker;
        bool watchpoint = false;
        CpuState cpu;
        static_cast<void>(cpu.memory.add_watchpoint(
            0x20u, 1u, MemoryWatchpointAccess::Write, [&](const auto&) { watchpoint = true; }));
        PreciseInterpreterBoundary boundary(
            safepoints,
            [](CpuState& state, const InterpreterRequest& request) {
                if (request.opcode == 1u) {
                    state.r[0] += 2u;
                    state.memory.write_u8(0x20u, 0x5Au);
                    state.pc = request.exit_boundary;
                    return true;
                }
                if (request.opcode == 2u) {
                    static_cast<void>(state.memory.read_u32(0xFFFF0000u));
                    return true;
                }
                return false;
            },
            &tracker);

        const InterpreterRequest dynamic_request{"unsupported-generated-op",
                                                 0x8C001000u,
                                                 1u,
                                                 0x8C001002u,
                                                 2u,
                                                 std::nullopt,
                                                 true,
                                                 true,
                                                 0x0C001000u,
                                                 2u,
                                                 "runtime-op"};
        const auto normal = boundary.execute(cpu, dynamic_request);
        require(normal.resumed && cpu.r[0] == 2u && cpu.memory.read_u8(0x20u) == 0x5Au &&
                    watchpoint && normal.safepoint.delivered_cycle == 2u &&
                    tracker.valid("fallback-runtime-op") && tracker.block_count() == 1u &&
                    cpu.attempted_guest_instructions == 1u &&
                    cpu.retired_guest_instructions == 1u &&
                    elapsed_guest_cycles(cpu) == 2u,
                "Fallback umgeht CPU-/Speicherzustand, Watchpoint, Scheduler oder Codeprovenienz.");
        const auto repeated = boundary.execute(cpu, dynamic_request);
        require(
            repeated.resumed && cpu.r[0] == 4u && tracker.block_count() == 1u &&
                tracker.invalidation_count() == 0u &&
                boundary.count("unsupported-generated-op") == 2u,
            "Wiederholter dynamischer Fallback dupliziert oder verwirft seine Blockidentitaet.");
        static_cast<void>(tracker.observe_write(0xAC001000u, 1u, CodeWriteSource::Cpu));
        require(!tracker.valid("fallback-runtime-op"),
                "Dynamischer Fallbackcode umgeht Schreibinvalidierung.");
        const auto reactivated = boundary.execute(cpu, dynamic_request);
        require(reactivated.resumed && tracker.valid("fallback-runtime-op") &&
                    tracker.block_count() == 1u && tracker.invalidation_count() == 1u &&
                    boundary.count("unsupported-generated-op") == 3u,
                "Invalidierter dynamischer Fallback wird nicht ohne Trackerduplikat reaktiviert.");

        cpu.vbr = 0x8C000000u;
        enter_exception(cpu,
                        {ExceptionCause::Interrupt,
                         0x00000320u,
                         interrupt_vector,
                         0x8C001006u,
                         std::nullopt,
                         true});
        const auto active_exception_generation = cpu.exception_generation;
        const auto handler_step = boundary.execute(
            cpu, {"handler-step", 0x8C001100u, 1u, 0x8C001102u, 1u});
        require(handler_step.resumed && !handler_step.exception &&
                    handler_step.exception_cause == ExceptionCause::None &&
                    cpu.trap_pending &&
                    cpu.last_exception_cause == ExceptionCause::Interrupt &&
                    cpu.exception_generation == active_exception_generation &&
                    cpu.pc == 0x8C001102u,
                "Interpretergrenze verwechselt einen aktiven Handler mit einer neu "
                "ausgeloesten Exception.");
        return_from_exception(cpu);

        cpu.vbr = 0x8C000000u;
        const auto fault =
            boundary.execute(cpu, {"memory-fault", 0x8C002002u, 2u, 0x8C002004u, 1u, 0x8C002000u});
        require(
            fault.exception && cpu.last_exception_cause == ExceptionCause::AddressErrorRead &&
                cpu.spc == 0x8C002000u && cpu.expevt == event_address_error_read &&
                cpu.attempted_guest_instructions == 5u &&
                cpu.retired_guest_instructions == 4u &&
                elapsed_guest_cycles(cpu) == 8u,
            "Fallback-Speicherfehler besitzt nicht Ursache und Owner-PC des generierten Pfads.");

        return_from_exception(cpu);
        const auto illegal = boundary.execute(
            cpu, {"unknown-opcode", 0x8C003002u, 0xFFFFu, 0x8C003004u, 1u, 0x8C003000u});
        require(illegal.exception &&
                    cpu.last_exception_cause == ExceptionCause::SlotIllegalInstruction &&
                    cpu.spc == 0x8C003000u && boundary.count("unknown-opcode") == 1u &&
                    cpu.attempted_guest_instructions == 6u &&
                    cpu.retired_guest_instructions == 4u &&
                    elapsed_guest_cycles(cpu) == 9u,
                "Illegale Delay-Slot-Instruktion ist nicht praezise oder stabil gezaehlt.");

        bool forbidden = false;
        try {
            static_cast<void>(boundary.execute(
                cpu, {"manifest-denied", 0x8C004000u, 1u, 0x8C004002u, 1u, std::nullopt, false}));
        } catch (const std::runtime_error&) {
            forbidden = true;
        }
        require(forbidden && boundary.count("manifest-denied") == 0u,
                "Manifestverbot wird umgangen oder faelschlich als Eintritt gezaehlt.");

        EventScheduler invalid_scheduler;
        SchedulerSafepoints invalid_safepoints(invalid_scheduler, 12u, 4u);
        PreciseInterpreterBoundary invalid_boundary(
            invalid_safepoints,
            [](CpuState& state, const InterpreterRequest& request) {
                state.pc = request.guest_pc + 4u;
                return true;
            });
        CpuState invalid_cpu;
        bool invalid_boundary_rejected = false;
        try {
            static_cast<void>(invalid_boundary.execute(
                invalid_cpu,
                {"invalid-boundary", 0x8C005000u, 1u, 0x8C005002u, 3u}));
        } catch (const std::runtime_error&) {
            invalid_boundary_rejected = true;
        }
        require(invalid_boundary_rejected &&
                    invalid_cpu.attempted_guest_instructions == 1u &&
                    invalid_cpu.retired_guest_instructions == 1u &&
                    elapsed_guest_cycles(invalid_cpu) == 3u &&
                    invalid_scheduler.current_cycle() == 3u,
                "Erfolgreicher Fallback-Step wird vor Safepoint/Boundaryfehler nicht retired.");

        EventScheduler partial_scheduler;
        static_cast<void>(partial_scheduler.schedule_at(2u, [](auto, auto) {}));
        static_cast<void>(partial_scheduler.schedule_at(4u, [](auto, auto) {}));
        SchedulerSafepoints partial_safepoints(partial_scheduler, 1u, 8u);
        PreciseInterpreterBoundary partial_boundary(
            partial_safepoints,
            [](CpuState& state, const InterpreterRequest& request) {
                state.pc = request.exit_boundary;
                return true;
            });
        CpuState partial_cpu;
        const auto partial = partial_boundary.execute(
            partial_cpu, {"partial-safepoint", 0x8C006000u, 1u, 0x8C006002u, 5u});
        require(partial.safepoint.budget_exhausted &&
                    partial_scheduler.current_cycle() == 2u &&
                    partial_cpu.total_guest_cycles == 2u &&
                    partial_cpu.pending_guest_cycles == 3u &&
                    elapsed_guest_cycles(partial_cpu) == 5u,
                "Partieller Safepoint verbucht nicht nur tatsaechlich gelieferte Gastzeit.");

        EventScheduler throwing_scheduler;
        static_cast<void>(throwing_scheduler.schedule_at(
            2u, [](auto, auto) { throw std::runtime_error("expected-safepoint-throw"); }));
        SchedulerSafepoints throwing_safepoints(throwing_scheduler, 1u, 8u);
        PreciseInterpreterBoundary throwing_boundary(
            throwing_safepoints,
            [](CpuState& state, const InterpreterRequest& request) {
                state.pc = request.exit_boundary;
                return true;
            });
        CpuState throwing_cpu;
        bool safepoint_threw = false;
        try {
            static_cast<void>(throwing_boundary.execute(
                throwing_cpu,
                {"throwing-safepoint", 0x8C007000u, 1u, 0x8C007002u, 5u}));
        } catch (const std::runtime_error& error) {
            safepoint_threw =
                std::string(error.what()).find("expected-safepoint-throw") !=
                std::string::npos;
        }
        require(safepoint_threw && throwing_scheduler.current_cycle() == 2u &&
                    throwing_cpu.total_guest_cycles == 2u &&
                    throwing_cpu.pending_guest_cycles == 3u &&
                    elapsed_guest_cycles(throwing_cpu) == 5u,
                "Safepoint-Throw verliert gelieferten Anteil oder verbucht Restzeit vorzeitig.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
