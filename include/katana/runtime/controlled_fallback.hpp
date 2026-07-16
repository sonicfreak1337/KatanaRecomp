#pragma once

#include "katana/runtime/block_abi.hpp"
#include "katana/runtime/dispatch_diagnostics.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace katana::runtime {

enum class FallbackPolicy : std::uint8_t { Abort, Diagnose, Interpreter, UserHook };
enum class FallbackReason : std::uint8_t { UnknownOpcode, UnresolvedControlFlow, DynamicCode };

struct ControlledFallbackRequest {
    FallbackReason reason = FallbackReason::UnknownOpcode;
    std::uint32_t guest_pc = 0u;
    std::optional<std::uint16_t> opcode;
    std::optional<std::uint32_t> target;
    std::uint32_t block_boundary = 0u;
    std::uint64_t guest_instructions = 0u;
};

struct ControlledFallbackResult {
    bool resumed = false;
    bool diagnosed = false;
    std::uint32_t next_guest_pc = 0u;
    std::uint64_t scheduler_cycle = 0u;
    std::string stable_reason;
};

using ControlledFallbackHandler = std::function<std::uint32_t(
    CpuState&, BlockExecutionContext&, const ControlledFallbackRequest&)>;

class ControlledFallbackError final : public std::runtime_error {
  public:
    explicit ControlledFallbackError(const ControlledFallbackRequest& request);
};

class ControlledFallback {
  public:
    explicit ControlledFallback(FallbackPolicy policy,
                                ControlledFallbackHandler handler = {},
                                DispatchDiagnosticRecorder* diagnostics = nullptr);

    [[nodiscard]] ControlledFallbackResult
    enter(CpuState& cpu, BlockExecutionContext& context, const ControlledFallbackRequest& request);
    [[nodiscard]] std::uint64_t count(FallbackReason reason) const noexcept;
    [[nodiscard]] std::uint64_t total_count() const noexcept;

  private:
    FallbackPolicy policy_;
    ControlledFallbackHandler handler_;
    DispatchDiagnosticRecorder* diagnostics_ = nullptr;
    std::array<std::uint64_t, 3u> counts_{};
};

[[nodiscard]] std::string stable_fallback_reason(const ControlledFallbackRequest& request);

} // namespace katana::runtime
