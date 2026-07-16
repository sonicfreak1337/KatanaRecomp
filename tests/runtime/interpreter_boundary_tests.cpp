#include "katana/runtime/interpreter_boundary.hpp"

#include <iostream>
#include <stdexcept>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        EventScheduler scheduler;
        SchedulerSafepoints safepoints(scheduler, 8u, 4u);
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
                    tracker.valid("fallback-runtime-op") && tracker.block_count() == 1u,
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

        cpu.last_exception_cause = ExceptionCause::None;
        cpu.vbr = 0x8C000000u;
        const auto fault =
            boundary.execute(cpu, {"memory-fault", 0x8C002002u, 2u, 0x8C002004u, 1u, 0x8C002000u});
        require(
            fault.exception && cpu.last_exception_cause == ExceptionCause::BusErrorRead &&
                cpu.spc == 0x8C002000u && cpu.expevt == event_address_error_read,
            "Fallback-Speicherfehler besitzt nicht Ursache und Owner-PC des generierten Pfads.");

        cpu.last_exception_cause = ExceptionCause::None;
        const auto illegal = boundary.execute(
            cpu, {"unknown-opcode", 0x8C003002u, 0xFFFFu, 0x8C003004u, 1u, 0x8C003000u});
        require(illegal.exception &&
                    cpu.last_exception_cause == ExceptionCause::SlotIllegalInstruction &&
                    cpu.spc == 0x8C003000u && boundary.count("unknown-opcode") == 1u,
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
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
