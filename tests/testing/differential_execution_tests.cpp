#include "generated_execution_program.cpp"
#include "katana/runtime/interpreter_boundary.hpp"
#include "katana/testing/differential_execution.hpp"

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

void execute_ir_reference(katana::runtime::CpuState& cpu,
                          std::vector<DifferentialMemoryByte>& memory,
                          std::vector<DifferentialMmioObservation>& mmio,
                          const std::uint16_t opcode) {
    switch (opcode) {
    case 0xE001u:
        cpu.r[0] = 1u;
        break;
    case 0x7001u:
        ++cpu.r[0];
        break;
    case 0x2102u:
        memory[0].value = static_cast<std::uint8_t>(cpu.r[0]);
        break;
    case 0xC001u:
        mmio.push_back({katana::runtime::MemoryAccessOperation::Write,
                        0x005F6800u,
                        katana::runtime::MemoryAccessWidth::Word,
                        cpu.r[0]});
        break;
    default:
        throw std::runtime_error("IR-Referenz erhielt unbekannten Testopcode.");
    }
}

void execute_generated_cpp(katana::runtime::CpuState& cpu,
                           std::vector<DifferentialMemoryByte>& memory,
                           std::vector<DifferentialMmioObservation>& mmio,
                           const std::uint16_t opcode) {
    if (opcode == 0xE001u) {
        cpu.r[0] = 1u;
    } else if (opcode == 0x7001u) {
        cpu.r[0] = cpu.r[0] + 1u;
    } else if (opcode == 0x2102u) {
        memory.front().value = static_cast<std::uint8_t>(cpu.r[0] & 0xFFu);
    } else if (opcode == 0xC001u) {
        mmio.emplace_back(DifferentialMmioObservation{katana::runtime::MemoryAccessOperation::Write,
                                                      0x005F6800u,
                                                      katana::runtime::MemoryAccessWidth::Word,
                                                      cpu.r[0]});
    } else {
        throw std::runtime_error("Generated-C++-Adapter erhielt unbekannten Testopcode.");
    }
}

bool execute_fallback_instruction(katana::runtime::CpuState& cpu,
                                  std::vector<DifferentialMemoryByte>& memory,
                                  std::vector<DifferentialMmioObservation>& mmio,
                                  const katana::runtime::InterpreterRequest& request) {
    switch (request.opcode) {
    case 0xE001u:
        cpu.r[0] = 1u;
        break;
    case 0x7001u:
        cpu.r[0] += 1u;
        break;
    case 0x2102u:
        memory.at(0).value = static_cast<std::uint8_t>(cpu.r[0]);
        break;
    case 0xC001u:
        mmio.push_back({katana::runtime::MemoryAccessOperation::Write,
                        0x005F6800u,
                        katana::runtime::MemoryAccessWidth::Word,
                        cpu.r[0]});
        break;
    default:
        return false;
    }
    cpu.pc = request.exit_boundary;
    return true;
}

DifferentialTrace execute_direct_path(const DifferentialExecutionPath path,
                                      const DifferentialProgram& program,
                                      const bool corrupt_last_register) {
    katana::runtime::CpuState cpu;
    cpu.pc = program.entry_pc;
    katana::runtime::EventScheduler scheduler;
    std::vector<DifferentialMemoryByte> memory = {{0x20u, 0u}, {0x21u, 0u}};
    std::vector<DifferentialMmioObservation> mmio;
    DifferentialTrace trace;
    trace.path = path;
    for (std::size_t index = 0u; index < program.opcodes.size(); ++index) {
        if (path == DifferentialExecutionPath::IrReference) {
            execute_ir_reference(cpu, memory, mmio, program.opcodes[index]);
        } else {
            execute_generated_cpp(cpu, memory, mmio, program.opcodes[index]);
        }
        cpu.pc = program.entry_pc + static_cast<std::uint32_t>((index + 1u) * 2u);
        static_cast<void>(scheduler.advance_by(1u, 8u));
        if (corrupt_last_register && index + 1u == program.opcodes.size()) ++cpu.r[0];
        trace.checkpoints.push_back(katana::testing::make_runtime_checkpoint(
            cpu,
            program.entry_pc + static_cast<std::uint32_t>(index * 2u),
            memory,
            mmio,
            &scheduler));
    }
    return trace;
}

DifferentialTrace execute_fallback_path(const DifferentialProgram& program) {
    katana::runtime::CpuState cpu;
    cpu.pc = program.entry_pc;
    katana::runtime::EventScheduler scheduler;
    katana::runtime::SchedulerSafepoints safepoints(scheduler, 8u, 4u);
    std::vector<DifferentialMemoryByte> memory = {{0x20u, 0u}, {0x21u, 0u}};
    std::vector<DifferentialMmioObservation> mmio;
    katana::runtime::PreciseInterpreterBoundary boundary(
        safepoints, [&](auto& state, const auto& request) {
            return execute_fallback_instruction(state, memory, mmio, request);
        });
    DifferentialTrace trace;
    trace.path = DifferentialExecutionPath::InterpreterFallback;
    for (std::size_t index = 0u; index < program.opcodes.size(); ++index) {
        const auto guest_pc = program.entry_pc + static_cast<std::uint32_t>(index * 2u);
        const auto exit_pc = guest_pc + 2u;
        const auto result = boundary.execute(
            cpu, {"differential-test", guest_pc, program.opcodes[index], exit_pc, 1u});
        require(result.resumed && result.next_pc == exit_pc,
                "Kontrollierter Fallback erreicht seine Instruktionsgrenze nicht.");
        trace.checkpoints.push_back(
            katana::testing::make_runtime_checkpoint(cpu, guest_pc, memory, mmio, &scheduler));
    }
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
        katana_generated::CpuState emitted_cpu;
        katana_generated::run(emitted_cpu);
        katana::runtime::CpuState emitted_reference;
        emitted_reference.r[1] = 4u;
        emitted_reference.r[2] = 7u;
        emitted_reference.pc = 0x8C010004u;
        emitted_reference.pr = 0x8C010004u;
        require(emitted_cpu.r[1] == emitted_reference.r[1] &&
                    emitted_cpu.r[2] == emitted_reference.r[2] &&
                    emitted_cpu.pc == emitted_reference.pc &&
                    emitted_cpu.pr == emitted_reference.pr,
                "Echt emittiertes und kompiliertes C++ weicht vom Referenzzustand ab.");

        const DifferentialProgram program{"synthetic-three-paths",
                                          "core",
                                          0x12345678u,
                                          0x8C010000u,
                                          {0xE001u, 0x7001u, 0x2102u, 0xC001u}};
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
                    broken.first_difference->guest_pc == 0x8C010006u,
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
                    "bus-errors", "delay-slots", "fpu-modes", "mmu-translation", "store-queues"},
            "Differentialkorpus deckt die zugesagten semantischen Grenzen nicht ab.");

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
