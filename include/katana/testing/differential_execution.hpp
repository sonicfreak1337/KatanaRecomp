#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/runtime.hpp"
#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace katana::testing {

enum class DifferentialExecutionPath : std::uint8_t {
    IrReference,
    GeneratedCpp,
    InterpreterFallback
};

struct DifferentialProgram {
    std::string identity;
    std::string corpus;
    std::uint64_t seed = 0u;
    std::uint32_t entry_pc = 0u;
    std::vector<std::uint16_t> opcodes;
};

struct DifferentialStateValue {
    std::string path;
    std::uint64_t value = 0u;

    bool operator==(const DifferentialStateValue&) const = default;
};

struct DifferentialCheckpoint {
    std::uint32_t guest_pc = 0u;
    std::vector<DifferentialStateValue> state;
};

struct DifferentialTrace {
    DifferentialExecutionPath path = DifferentialExecutionPath::IrReference;
    std::vector<DifferentialCheckpoint> checkpoints;
};

struct DifferentialMemoryByte {
    std::uint32_t address = 0u;
    std::uint8_t value = 0u;
};

struct DifferentialMmioObservation {
    runtime::MemoryAccessOperation operation = runtime::MemoryAccessOperation::Read;
    std::uint32_t address = 0u;
    runtime::MemoryAccessWidth width = runtime::MemoryAccessWidth::Byte;
    std::uint32_t value = 0u;
};

using DifferentialRun = std::function<DifferentialTrace(const DifferentialProgram&)>;

struct DifferentialRunner {
    DifferentialExecutionPath path = DifferentialExecutionPath::IrReference;
    DifferentialRun run;
};

struct DifferentialDifference {
    std::size_t checkpoint_index = 0u;
    std::uint32_t guest_pc = 0u;
    DifferentialExecutionPath expected_path = DifferentialExecutionPath::IrReference;
    DifferentialExecutionPath actual_path = DifferentialExecutionPath::IrReference;
    std::string state_path;
    std::string expected;
    std::string actual;
};

struct DifferentialReport {
    DifferentialProgram program;
    std::array<DifferentialTrace, 3u> traces;
    std::optional<DifferentialDifference> first_difference;

    [[nodiscard]] bool matches() const noexcept;
};

class DifferentialMismatch final : public std::runtime_error {
  public:
    explicit DifferentialMismatch(DifferentialDifference difference);

    [[nodiscard]] const DifferentialDifference& difference() const noexcept;

  private:
    DifferentialDifference difference_;
};

[[nodiscard]] const char* differential_execution_path_name(DifferentialExecutionPath path) noexcept;

[[nodiscard]] DifferentialCheckpoint
make_runtime_checkpoint(const runtime::CpuState& cpu,
                        std::uint32_t guest_pc,
                        std::span<const DifferentialMemoryByte> memory = {},
                        std::span<const DifferentialMmioObservation> mmio = {},
                        const runtime::EventScheduler* scheduler = nullptr);

[[nodiscard]] DifferentialReport
run_differential_execution(const DifferentialProgram& program,
                           std::span<const DifferentialRunner> runners);

void require_differential_match(const DifferentialReport& report);

[[nodiscard]] std::string format_differential_counterexample_json(const DifferentialReport& report);

[[nodiscard]] std::vector<DifferentialProgram> default_differential_corpus();
[[nodiscard]] std::vector<DifferentialProgram> differential_regression_programs();
void run_generated_differential_program(std::size_t index, runtime::CpuState& cpu);

} // namespace katana::testing
