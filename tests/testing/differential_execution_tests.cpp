#include "katana/runtime/exception.hpp"
#include "katana/runtime/fpu.hpp"
#include "katana/runtime/interpreter_boundary.hpp"
#include "katana/testing/differential_execution.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using katana::testing::DifferentialCheckpoint;
using katana::testing::DifferentialExecutionPath;
using katana::testing::DifferentialMemoryByte;
using katana::testing::DifferentialMmioObservation;
using katana::testing::DifferentialProgram;
using katana::testing::DifferentialRunner;
using katana::testing::DifferentialTrace;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void initialize_program_state(katana::runtime::CpuState& cpu, const DifferentialProgram& program) {
    cpu.pc = program.entry_pc;
    cpu.r[0] = program.corpus == "store-queues" ? 0xE0000000u : 0u;
    cpu.r[1] = program.corpus == "bus-errors" ? 0x00200000u : 0x20u;
    cpu.r[2] = 0xAABBCCDDu;
    cpu.memory.write_u32(0x20u, 0x11223344u);
    katana::runtime::write_fr_single(cpu, 0u, 1.5F);
}

void execute_reference_instruction(katana::runtime::CpuState& cpu, const std::uint16_t opcode) {
    switch (opcode) {
    case 0xE001u:
        cpu.r[0] = 1u;
        return;
    case 0x7001u:
        ++cpu.r[0];
        return;
    case 0x2102u:
        cpu.memory.write_u32(cpu.r[1], cpu.r[0]);
        return;
    case 0x6012u:
        cpu.r[0] = cpu.memory.read_u32(cpu.r[1]);
        return;
    case 0x2122u:
        cpu.memory.write_u32(cpu.r[1], cpu.r[2]);
        return;
    case 0x0083u:
        katana::runtime::prefetch(cpu, cpu.r[0]);
        return;
    case 0xF000u:
        katana::runtime::fpu_binary(cpu, katana::runtime::FpuBinaryOperation::Add, 0u, 0u);
        return;
    case 0xF08Du:
        katana::runtime::write_fr_single(cpu, 0u, 0.0F);
        return;
    case 0xA000u:
    case 0x0009u:
        return;
    default:
        throw std::runtime_error("IR-Referenz erhielt unbekannten Testopcode.");
    }
}

bool execute_fallback_instruction(katana::runtime::CpuState& cpu,
                                  const katana::runtime::InterpreterRequest& request) {
    try {
        execute_reference_instruction(cpu, request.opcode);
    } catch (const std::runtime_error&) {
        throw;
    } catch (...) {
        return false;
    }
    cpu.pc = request.exit_boundary;
    return true;
}

DifferentialTrace execute_direct_path(const DifferentialExecutionPath path,
                                      const DifferentialProgram& program,
                                      const bool corrupt_last_register) {
    katana::runtime::CpuState cpu;
    initialize_program_state(cpu, program);
    std::vector<DifferentialMemoryByte> memory = {{0x20u, 0u}, {0x21u, 0u}};
    std::vector<DifferentialMmioObservation> mmio;
    DifferentialTrace trace;
    trace.path = path;
    std::uint64_t exception_outcome = 0u;
    try {
        if (path == DifferentialExecutionPath::IrReference) {
            std::optional<std::uint32_t> delayed_target;
            for (std::size_t index = 0u; index < program.opcodes.size(); ++index) {
                const auto guest_pc = program.entry_pc + static_cast<std::uint32_t>(index * 2u);
                cpu.pc = guest_pc;
                try {
                    execute_reference_instruction(cpu, program.opcodes[index]);
                } catch (const katana::runtime::MemoryAccessError& error) {
                    katana::runtime::enter_memory_exception(
                        cpu, error, program.entry_pc + static_cast<std::uint32_t>(index * 2u));
                    break;
                }
                if (program.opcodes[index] == 0xA000u) {
                    delayed_target = guest_pc + 4u;
                    cpu.pc = guest_pc + 2u;
                } else if (delayed_target) {
                    cpu.pc = *delayed_target;
                    delayed_target.reset();
                } else {
                    cpu.pc = guest_pc + 2u;
                }
            }
        } else {
            const auto programs = katana::testing::differential_regression_programs();
            const auto found =
                std::find_if(programs.begin(), programs.end(), [&](const auto& item) {
                    return item.identity == program.identity;
                });
            require(found != programs.end(), "Generiertes Differentialprogramm fehlt.");
            katana::testing::run_generated_differential_program(
                static_cast<std::size_t>(found - programs.begin()), cpu);
        }
    } catch (const katana::runtime::MemoryAccessError& error) {
        katana::runtime::enter_memory_exception(cpu, error, cpu.pc);
    }
    exception_outcome = cpu.last_exception_cause == katana::runtime::ExceptionCause::None ? 0u : 1u;
    if (corrupt_last_register) ++cpu.r[0];
    if (path == DifferentialExecutionPath::IrReference && exception_outcome == 0u) {
        // The generated fixture appends `rts; nop`; model that real epilogue in the reference.
        cpu.pc = cpu.pr;
    }
    if (exception_outcome == 0u) {
        memory[0].value = cpu.memory.read_u8(0x20u);
        memory[1].value = cpu.memory.read_u8(0x21u);
    }
    auto checkpoint = katana::testing::make_runtime_checkpoint(
        cpu,
        program.entry_pc + static_cast<std::uint32_t>((program.opcodes.size() - 1u) * 2u),
        memory,
        mmio);
    checkpoint.state.push_back({"outcome.exception", exception_outcome});
    trace.checkpoints.push_back(std::move(checkpoint));
    return trace;
}

DifferentialTrace execute_fallback_path(const DifferentialProgram& program) {
    katana::runtime::CpuState cpu;
    initialize_program_state(cpu, program);
    katana::runtime::EventScheduler scheduler;
    katana::runtime::SchedulerSafepoints safepoints(scheduler, 8u, 4u);
    std::vector<DifferentialMemoryByte> memory = {{0x20u, 0u}, {0x21u, 0u}};
    std::vector<DifferentialMmioObservation> mmio;
    katana::runtime::PreciseInterpreterBoundary boundary(
        safepoints, [&](auto& state, const auto& request) {
            return execute_fallback_instruction(state, request);
        });
    DifferentialTrace trace;
    trace.path = DifferentialExecutionPath::InterpreterFallback;
    std::uint64_t exception_outcome = 0u;
    std::optional<std::uint32_t> delayed_target;
    std::optional<std::uint32_t> delay_slot_owner;
    try {
        for (std::size_t index = 0u; index < program.opcodes.size(); ++index) {
            const auto guest_pc = program.entry_pc + static_cast<std::uint32_t>(index * 2u);
            auto exit_pc = guest_pc + 2u;
            if (delayed_target) exit_pc = *delayed_target;
            const auto result = boundary.execute(cpu,
                                                 {"differential-test",
                                                  guest_pc,
                                                  program.opcodes[index],
                                                  exit_pc,
                                                  1u,
                                                  delay_slot_owner});
            if (!result.resumed && program.corpus == "bus-errors") {
                exception_outcome = 1u;
                break;
            }
            if (!result.resumed || result.next_pc != exit_pc) {
                throw std::runtime_error(
                    "Kontrollierter Fallback erreicht seine Instruktionsgrenze nicht: " +
                    program.identity);
            }
            if (program.opcodes[index] == 0xA000u) {
                delayed_target = guest_pc + 4u;
                delay_slot_owner = guest_pc;
            } else {
                delayed_target.reset();
                delay_slot_owner.reset();
            }
        }
    } catch (const katana::runtime::MemoryAccessError&) {
        exception_outcome = 1u;
    }
    if (exception_outcome == 0u) {
        // The generated fixture appends `rts; nop`; execute the same epilogue after fallback.
        cpu.pc = cpu.pr;
    }
    if (exception_outcome == 0u) {
        memory[0].value = cpu.memory.read_u8(0x20u);
        memory[1].value = cpu.memory.read_u8(0x21u);
    }
    auto checkpoint = katana::testing::make_runtime_checkpoint(
        cpu,
        program.entry_pc + static_cast<std::uint32_t>((program.opcodes.size() - 1u) * 2u),
        memory,
        mmio);
    checkpoint.state.push_back({"outcome.exception", exception_outcome});
    trace.checkpoints.push_back(std::move(checkpoint));
    return trace;
}

std::array<DifferentialRunner, 3u> runners(const bool corrupt_generated = false) {
    return {{{DifferentialExecutionPath::IrReference,
              [](const auto& program) {
                  return execute_direct_path(
                      DifferentialExecutionPath::IrReference, program, false);
              }},
             {DifferentialExecutionPath::GeneratedCpp,
              [=](const auto& program) {
                  return execute_direct_path(
                      DifferentialExecutionPath::GeneratedCpp, program, corrupt_generated);
              }},
             {DifferentialExecutionPath::InterpreterFallback,
              [](const auto& program) { return execute_fallback_path(program); }}}};
}

} // namespace

int main() {
    try {
        const auto programs = katana::testing::differential_regression_programs();
        const auto& program = programs.front();
        const auto matching_runners = runners();
        const auto matching =
            katana::testing::run_differential_execution(program, matching_runners);
        require(matching.matches(), "Gleiche Ausfuehrungswege werden als Abweichung gemeldet.");
        katana::testing::require_differential_match(matching);

        const auto broken_runners = runners(true);
        const auto broken = katana::testing::run_differential_execution(program, broken_runners);
        require(!broken.matches() && broken.first_difference.has_value(),
                "Absichtlich fehlerhaftes generiertes Backend wird nicht erkannt.");
        require(broken.first_difference->actual_path == DifferentialExecutionPath::GeneratedCpp &&
                    broken.first_difference->state_path == "cpu.r[0]" &&
                    broken.first_difference->guest_pc == 0x8C010004u,
                "Erste Abweichung verliert Ausfuehrungsweg, Feld oder Gast-PC.");
        bool mismatch_thrown = false;
        try {
            katana::testing::require_differential_match(broken);
        } catch (const katana::testing::DifferentialMismatch& error) {
            mismatch_thrown = error.difference().state_path == "cpu.r[0]";
        }
        require(mismatch_thrown, "DifferentialMismatch besitzt keine strukturierte Abweichung.");

        const auto counterexample =
            katana::testing::format_differential_counterexample_json(broken);
        require(counterexample.find("katana-differential-counterexample") != std::string::npos &&
                    counterexample.find("generated-cpp") != std::string::npos &&
                    counterexample.find("cpu.r[0]") != std::string::npos &&
                    counterexample.find("C:\\") == std::string::npos,
                "Gegenbeispiel ist unvollstaendig oder enthaelt einen Hostpfad.");

        const auto corpus = katana::testing::default_differential_corpus();
        std::set<std::string> categories;
        for (const auto& item : corpus)
            categories.insert(item.corpus);
        require(
            categories ==
                std::set<std::string>{
                    "bus-errors", "delay-slots", "fpu-modes", "memory-load-store", "store-queues"},
            "Differentialkorpus deckt die zugesagten semantischen Grenzen nicht ab.");
        for (const auto& item : corpus) {
            const auto report = katana::testing::run_differential_execution(item, matching_runners);
            katana::testing::require_differential_match(report);
        }

        auto duplicate_runners = runners();
        duplicate_runners[2].path = DifferentialExecutionPath::GeneratedCpp;
        bool duplicate_rejected = false;
        try {
            static_cast<void>(
                katana::testing::run_differential_execution(program, duplicate_runners));
        } catch (const std::invalid_argument&) {
            duplicate_rejected = true;
        }
        require(duplicate_rejected, "Doppelter Ausfuehrungsweg wird akzeptiert.");

        std::cout << "KR-3707 Differentialausfuehrung erfolgreich.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
