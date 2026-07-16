#pragma once

#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/dispatch_diagnostics.hpp"
#include "katana/runtime/exception.hpp"
#include "katana/runtime/scheduler_safepoint.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace katana::runtime {

struct InterpreterRequest {
    std::string reason;
    std::uint32_t guest_pc = 0u;
    std::uint16_t opcode = 0u;
    std::uint32_t exit_boundary = 0u;
    std::uint64_t guest_cycles = 1u;
    std::optional<std::uint32_t> delay_slot_owner;
    bool manifest_allowed = true;
    bool dynamic_code = false;
    std::uint32_t dynamic_physical_address = 0u;
    std::uint32_t dynamic_size = 0u;
    std::string provenance;
};

struct InterpreterResult {
    bool resumed = false;
    bool exception = false;
    std::uint32_t next_pc = 0u;
    ExceptionCause exception_cause = ExceptionCause::None;
    SafepointReport safepoint;
};

using InterpreterStep = std::function<bool(CpuState&, const InterpreterRequest&)>;

class PreciseInterpreterBoundary {
  public:
    PreciseInterpreterBoundary(SchedulerSafepoints& safepoints,
                               InterpreterStep step,
                               ExecutableCodeTracker* code_tracker = nullptr,
                               DispatchDiagnosticRecorder* diagnostics = nullptr);
    [[nodiscard]] InterpreterResult execute(CpuState& cpu, const InterpreterRequest& request);
    [[nodiscard]] std::uint64_t count(const std::string& reason) const noexcept;
    [[nodiscard]] const std::map<std::string, std::uint64_t>& counts() const noexcept;

  private:
    SchedulerSafepoints& safepoints_;
    InterpreterStep step_;
    ExecutableCodeTracker* code_tracker_;
    DispatchDiagnosticRecorder* diagnostics_;
    std::map<std::string, std::uint64_t> counts_;
};

} // namespace katana::runtime
