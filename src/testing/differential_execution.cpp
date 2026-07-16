#include "katana/testing/differential_execution.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace katana::testing {
namespace {

std::size_t path_index(const DifferentialExecutionPath path) {
    switch (path) {
        case DifferentialExecutionPath::IrReference: return 0u;
        case DifferentialExecutionPath::GeneratedCpp: return 1u;
        case DifferentialExecutionPath::InterpreterFallback: return 2u;
    }
    throw std::invalid_argument("Unbekannter Differential-Ausfuehrungsweg.");
}

std::string indexed_path(const std::string_view prefix, const std::size_t index) {
    return std::string(prefix) + "[" + std::to_string(index) + "]";
}

std::string address_path(const std::uint32_t address) {
    std::ostringstream output;
    output << "memory[0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << address << ']';
    return output.str();
}

void add(
    DifferentialCheckpoint& checkpoint,
    std::string path,
    const std::uint64_t value
) {
    checkpoint.state.push_back({std::move(path), value});
}

DifferentialCheckpoint canonicalize(DifferentialCheckpoint checkpoint) {
    for (const auto& value : checkpoint.state) {
        if (value.path.empty()) {
            throw std::invalid_argument("Differential-Checkpoint besitzt einen leeren Zustandspfad.");
        }
    }
    std::sort(
        checkpoint.state.begin(),
        checkpoint.state.end(),
        [](const auto& left, const auto& right) { return left.path < right.path; }
    );
    for (std::size_t index = 1u; index < checkpoint.state.size(); ++index) {
        if (checkpoint.state[index - 1u].path == checkpoint.state[index].path) {
            throw std::invalid_argument(
                "Differential-Checkpoint besitzt den Zustandspfad doppelt: " +
                checkpoint.state[index].path
            );
        }
    }
    return checkpoint;
}

std::string value_text(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << value;
    return output.str();
}

std::optional<DifferentialDifference> compare_traces(
    const DifferentialTrace& expected,
    const DifferentialTrace& actual
) {
    const auto common = std::min(expected.checkpoints.size(), actual.checkpoints.size());
    for (std::size_t checkpoint_index = 0u; checkpoint_index < common; ++checkpoint_index) {
        const auto& left = expected.checkpoints[checkpoint_index];
        const auto& right = actual.checkpoints[checkpoint_index];
        const auto guest_pc = left.guest_pc;
        if (left.guest_pc != right.guest_pc) {
            return DifferentialDifference{
                checkpoint_index,
                guest_pc,
                expected.path,
                actual.path,
                "checkpoint.guest_pc",
                value_text(left.guest_pc),
                value_text(right.guest_pc)
            };
        }
        std::size_t left_index = 0u;
        std::size_t right_index = 0u;
        while (left_index < left.state.size() || right_index < right.state.size()) {
            if (right_index == right.state.size() ||
                (left_index < left.state.size() &&
                    left.state[left_index].path < right.state[right_index].path)) {
                return DifferentialDifference{
                    checkpoint_index,
                    guest_pc,
                    expected.path,
                    actual.path,
                    left.state[left_index].path,
                    value_text(left.state[left_index].value),
                    "<missing>"
                };
            }
            if (left_index == left.state.size() ||
                right.state[right_index].path < left.state[left_index].path) {
                return DifferentialDifference{
                    checkpoint_index,
                    guest_pc,
                    expected.path,
                    actual.path,
                    right.state[right_index].path,
                    "<missing>",
                    value_text(right.state[right_index].value)
                };
            }
            if (left.state[left_index].value != right.state[right_index].value) {
                return DifferentialDifference{
                    checkpoint_index,
                    guest_pc,
                    expected.path,
                    actual.path,
                    left.state[left_index].path,
                    value_text(left.state[left_index].value),
                    value_text(right.state[right_index].value)
                };
            }
            ++left_index;
            ++right_index;
        }
    }
    if (expected.checkpoints.size() != actual.checkpoints.size()) {
        const auto guest_pc = common == 0u ? 0u : expected.checkpoints[common - 1u].guest_pc;
        return DifferentialDifference{
            common,
            guest_pc,
            expected.path,
            actual.path,
            "checkpoints.count",
            std::to_string(expected.checkpoints.size()),
            std::to_string(actual.checkpoints.size())
        };
    }
    return std::nullopt;
}

std::string json_escape(const std::string_view text) {
    std::ostringstream output;
    for (const auto character : text) {
        switch (character) {
            case '\\': output << "\\\\"; break;
            case '"': output << "\\\""; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default: output << character; break;
        }
    }
    return output.str();
}

}

bool DifferentialReport::matches() const noexcept {
    return !first_difference.has_value();
}

DifferentialMismatch::DifferentialMismatch(DifferentialDifference difference)
    : std::runtime_error(
        "Differentialabweichung bei Gast-PC " + value_text(difference.guest_pc) +
        " in " + difference.state_path + " zwischen " +
        differential_execution_path_name(difference.expected_path) + " und " +
        differential_execution_path_name(difference.actual_path) + "."
    ),
      difference_(std::move(difference)) {}

const DifferentialDifference& DifferentialMismatch::difference() const noexcept {
    return difference_;
}

const char* differential_execution_path_name(const DifferentialExecutionPath path) noexcept {
    switch (path) {
        case DifferentialExecutionPath::IrReference: return "ir-reference";
        case DifferentialExecutionPath::GeneratedCpp: return "generated-cpp";
        case DifferentialExecutionPath::InterpreterFallback: return "interpreter-fallback";
    }
    return "unknown";
}

DifferentialCheckpoint make_runtime_checkpoint(
    const runtime::CpuState& cpu,
    const std::uint32_t guest_pc,
    const std::span<const DifferentialMemoryByte> memory,
    const std::span<const DifferentialMmioObservation> mmio,
    const runtime::EventScheduler* scheduler
) {
    DifferentialCheckpoint checkpoint;
    checkpoint.guest_pc = guest_pc;
    for (std::size_t index = 0u; index < cpu.r.size(); ++index) {
        add(checkpoint, indexed_path("cpu.r", index), cpu.r[index]);
    }
    for (std::size_t index = 0u; index < cpu.r_bank.size(); ++index) {
        add(checkpoint, indexed_path("cpu.r_bank", index), cpu.r_bank[index]);
    }
    for (std::size_t index = 0u; index < cpu.fr.size(); ++index) {
        add(checkpoint, indexed_path("cpu.fr", index), cpu.fr[index]);
        add(checkpoint, indexed_path("cpu.xf", index), cpu.xf[index]);
    }
    add(checkpoint, "cpu.pc", cpu.pc);
    add(checkpoint, "cpu.pr", cpu.pr);
    add(checkpoint, "cpu.gbr", cpu.gbr);
    add(checkpoint, "cpu.vbr", cpu.vbr);
    add(checkpoint, "cpu.ssr", cpu.ssr);
    add(checkpoint, "cpu.spc", cpu.spc);
    add(checkpoint, "cpu.sgr", cpu.sgr);
    add(checkpoint, "cpu.dbr", cpu.dbr);
    add(checkpoint, "cpu.tra", cpu.tra);
    add(checkpoint, "cpu.tea", cpu.tea);
    add(checkpoint, "cpu.expevt", cpu.expevt);
    add(checkpoint, "cpu.intevt", cpu.intevt);
    add(checkpoint, "cpu.mach", cpu.mach);
    add(checkpoint, "cpu.macl", cpu.macl);
    add(checkpoint, "cpu.fpul", cpu.fpul);
    add(checkpoint, "cpu.fpscr.visible", cpu.read_fpscr());
    add(checkpoint, "cpu.fpscr.raw", cpu.fpscr);
    add(checkpoint, "cpu.sr.visible", cpu.read_sr());
    add(checkpoint, "cpu.sr.raw", cpu.sr);
    add(checkpoint, "cpu.flag.t", cpu.t);
    add(checkpoint, "cpu.flag.s", cpu.s);
    add(checkpoint, "cpu.flag.q", cpu.q);
    add(checkpoint, "cpu.flag.m", cpu.m);
    add(checkpoint, "cpu.trap_pending", cpu.trap_pending);
    add(checkpoint, "cpu.sleeping", cpu.sleeping);
    add(checkpoint, "cpu.prefetch.address", cpu.last_prefetch_address);
    add(checkpoint, "cpu.prefetch.count", cpu.prefetch_count);
    add(checkpoint, "cpu.prefetch.store_queue", cpu.last_prefetch_was_store_queue);
    add(checkpoint, "exception.cause", static_cast<std::uint8_t>(cpu.last_exception_cause));
    add(checkpoint, "exception.delay_slot", cpu.exception_in_delay_slot);

    for (const auto& byte : memory) {
        add(checkpoint, address_path(byte.address), byte.value);
    }
    for (std::size_t index = 0u; index < mmio.size(); ++index) {
        const auto prefix = indexed_path("mmio", index);
        add(checkpoint, prefix + ".operation", static_cast<std::uint8_t>(mmio[index].operation));
        add(checkpoint, prefix + ".address", mmio[index].address);
        add(checkpoint, prefix + ".width", static_cast<std::uint8_t>(mmio[index].width));
        add(checkpoint, prefix + ".value", mmio[index].value);
    }
    if (scheduler != nullptr) {
        add(checkpoint, "scheduler.current_cycle", scheduler->current_cycle());
        add(checkpoint, "scheduler.pending_events", scheduler->pending_event_count());
        add(checkpoint, "scheduler.processed_events", scheduler->processed_event_count());
        add(checkpoint, "scheduler.reset_generation", scheduler->reset_generation());
        add(
            checkpoint,
            "scheduler.next_event_cycle",
            scheduler->next_event_cycle().value_or(std::numeric_limits<std::uint64_t>::max())
        );
    }
    return canonicalize(std::move(checkpoint));
}

DifferentialReport run_differential_execution(
    const DifferentialProgram& program,
    const std::span<const DifferentialRunner> runners
) {
    if (program.identity.empty() || program.corpus.empty() || program.opcodes.empty()) {
        throw std::invalid_argument("Differentialprogramm braucht Identitaet, Korpus und Opcodes.");
    }
    if (runners.size() != 3u) {
        throw std::invalid_argument("Differentialausfuehrung braucht exakt drei Ausfuehrungswege.");
    }
    DifferentialReport report;
    report.program = program;
    std::array<bool, 3u> seen{};
    for (const auto& runner : runners) {
        const auto index = path_index(runner.path);
        if (seen[index] || !runner.run) {
            throw std::invalid_argument("Differentialausfuehrungsweg fehlt oder ist doppelt.");
        }
        seen[index] = true;
        auto trace = runner.run(program);
        if (trace.path != runner.path || trace.checkpoints.empty()) {
            throw std::invalid_argument("Differentialrunner liefert Pfadkonflikt oder keine Checkpoints.");
        }
        for (auto& checkpoint : trace.checkpoints) {
            checkpoint = canonicalize(std::move(checkpoint));
        }
        report.traces[index] = std::move(trace);
    }
    const auto& reference = report.traces[path_index(DifferentialExecutionPath::IrReference)];
    const auto& generated = report.traces[path_index(DifferentialExecutionPath::GeneratedCpp)];
    report.first_difference = compare_traces(reference, generated);
    if (!report.first_difference) {
        const auto& fallback = report.traces[path_index(
            DifferentialExecutionPath::InterpreterFallback
        )];
        report.first_difference = compare_traces(reference, fallback);
    }
    return report;
}

void require_differential_match(const DifferentialReport& report) {
    if (report.first_difference) {
        throw DifferentialMismatch(*report.first_difference);
    }
}

std::string format_differential_counterexample_json(const DifferentialReport& report) {
    std::ostringstream output;
    output << "{\n"
           << "  \"schema\": \"katana-differential-counterexample\",\n"
           << "  \"version\": 1,\n"
           << "  \"identity\": \"" << json_escape(report.program.identity) << "\",\n"
           << "  \"corpus\": \"" << json_escape(report.program.corpus) << "\",\n"
           << "  \"seed\": " << report.program.seed << ",\n"
           << "  \"entry_pc\": \"" << value_text(report.program.entry_pc) << "\",\n"
           << "  \"opcodes\": [";
    for (std::size_t index = 0u; index < report.program.opcodes.size(); ++index) {
        if (index != 0u) output << ", ";
        output << '"' << value_text(report.program.opcodes[index]) << '"';
    }
    output << "],\n  \"matches\": " << (report.matches() ? "true" : "false");
    if (report.first_difference) {
        const auto& difference = *report.first_difference;
        output << ",\n  \"first_difference\": {\n"
               << "    \"checkpoint\": " << difference.checkpoint_index << ",\n"
               << "    \"guest_pc\": \"" << value_text(difference.guest_pc) << "\",\n"
               << "    \"expected_path\": \""
               << differential_execution_path_name(difference.expected_path) << "\",\n"
               << "    \"actual_path\": \""
               << differential_execution_path_name(difference.actual_path) << "\",\n"
               << "    \"state_path\": \"" << json_escape(difference.state_path) << "\",\n"
               << "    \"expected\": \"" << json_escape(difference.expected) << "\",\n"
               << "    \"actual\": \"" << json_escape(difference.actual) << "\"\n"
               << "  }";
    }
    output << "\n}\n";
    return output.str();
}

std::vector<DifferentialProgram> default_differential_corpus() {
    return {
        {"delay-slot-owner", "delay-slots", 0x37070001u, 0x8C010000u, {0xA000u, 0x0009u}},
        {"fpu-mode-boundary", "fpu-modes", 0x37070002u, 0x8C020000u, {0xF000u, 0xF08Du}},
        {"mmu-translation", "mmu-translation", 0x37070003u, 0x8C030000u, {0x6012u, 0x2122u}},
        {"store-queue-pref", "store-queues", 0x37070004u, 0x8C040000u, {0x0083u}},
        {"bus-error-read", "bus-errors", 0x37070005u, 0x8C050000u, {0x6012u}}
    };
}

}
