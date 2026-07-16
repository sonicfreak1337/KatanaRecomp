#include "katana/runtime/controlled_fallback.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main() {
    try {
        CpuState cpu;
        BlockExecutionContext context{77u, 9u, std::nullopt, BlockSyncPoint::Entry};
        const ControlledFallbackRequest opcode{
            FallbackReason::UnknownOpcode, 0x8C001000u, std::uint16_t{0xFFFFu}, std::nullopt, 0x8C001002u
        };
        ControlledFallback aborting(FallbackPolicy::Abort);
        bool aborted = false;
        try { static_cast<void>(aborting.enter(cpu, context, opcode)); }
        catch (const ControlledFallbackError& error) {
            const std::string text = error.what();
            aborted = text.find("8c001000") != std::string::npos && text.find("ffff") != std::string::npos;
        }
        require(aborted && aborting.count(FallbackReason::UnknownOpcode) == 1u,
            "Deaktivierter Fallback bricht nicht sichtbar und gezaehlt ab.");

        ControlledFallback diagnose(FallbackPolicy::Diagnose);
        const auto stopped = diagnose.enter(cpu, context, {
            FallbackReason::UnresolvedControlFlow, 0x8C002000u, std::nullopt, 0x8C00F000u, 0x8C002000u
        });
        require(!stopped.resumed && stopped.diagnosed && cpu.pc == 0x8C002000u,
            "Diagnosepfad setzte Ausfuehrung still fort.");

        ControlledFallback interpreter(FallbackPolicy::Interpreter,
            [](CpuState& state, BlockExecutionContext& boundary, const ControlledFallbackRequest& request) {
                state.r[0] += 3u;
                state.memory.write_u8(0x20u, 0x5Au);
                boundary.scheduler_cycle += 2u;
                return request.block_boundary;
            });
        cpu.r[0] = 4u;
        const auto resumed = interpreter.enter(cpu, context, opcode);
        require(resumed.resumed && cpu.r[0] == 7u && cpu.memory.read_u8(0x20u) == 0x5Au &&
                cpu.pc == 0x8C001002u && resumed.scheduler_cycle == 79u &&
                context.sync_point == BlockSyncPoint::FallbackBoundary,
            "Interpretergrenze synchronisiert CPU, Speicher, Scheduler oder PC nicht.");

        ControlledFallback wrong_boundary(FallbackPolicy::UserHook,
            [](CpuState&, BlockExecutionContext&, const ControlledFallbackRequest& request) {
                return request.block_boundary + 2u;
            });
        bool rejected = false;
        try { static_cast<void>(wrong_boundary.enter(cpu, context, opcode)); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Nutzerhook durfte ausserhalb der Blockgrenze fortsetzen.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
