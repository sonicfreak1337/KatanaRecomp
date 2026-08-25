#pragma once

#include "host_build_tool.hpp"
#include "katana/progress.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace katana::cli {

struct HostBuildProgressPlan final {
    std::optional<std::uint64_t> translation_units;
    std::uint64_t archive_steps = 1u;
    // Maximum physical launcher passes accepted for one logical link. An
    // up-to-date build may legitimately consume fewer passes; an excess is
    // still rejected fail-closed by the observer.
    std::uint64_t link_steps = 1u;
    std::size_t configured_workers = 1u;
};

struct HostBuildProgressSnapshot final {
    std::uint64_t compile_started = 0u;
    std::uint64_t compile_committed = 0u;
    std::uint64_t compile_failed = 0u;
    std::uint64_t archive_started = 0u;
    std::uint64_t archive_committed = 0u;
    std::uint64_t archive_failed = 0u;
    std::uint64_t link_started = 0u;
    std::uint64_t link_committed = 0u;
    std::uint64_t link_failed = 0u;
    bool observation_complete = true;
};

// Terminal progress may only classify build-graph edges which did not invoke
// a tool as cache/up-to-date work when the caller supplies the complete proof
// from the successfully supervised, instrumented target build.
struct HostBuildCompletionProof final {
    bool bound_build_graph_succeeded = false;
    bool process_tree_quiescent = false;
    bool linked_artifact_verified = false;
    bool uninvoked_plan_edges_up_to_date_verified = false;
    bool zero_tool_invocations_artifact_byte_identical = false;
};

// Deterministic observer race barriers used by the focused contract tests.
// Product callers leave this empty.
struct HostBuildProgressObserverHooks final {
    std::function<void(const std::filesystem::path&)>
        before_event_open;
};

// Reads only the versioned, per-invocation event files emitted by the compiler
// and linker launchers. Unknown files/formats make observation incomplete;
// they never manufacture work counts from arbitrary generator text.
class HostBuildProgressObserver final {
  public:
    HostBuildProgressObserver(
        std::filesystem::path event_root,
        HostBuildProgressPlan plan,
        const katana::ProgressReporter& progress,
        HostBuildProgressObserverHooks hooks = {});
    ~HostBuildProgressObserver();

    HostBuildProgressObserver(const HostBuildProgressObserver&) = delete;
    HostBuildProgressObserver& operator=(
        const HostBuildProgressObserver&) = delete;
    HostBuildProgressObserver(HostBuildProgressObserver&&) noexcept;
    HostBuildProgressObserver& operator=(
        HostBuildProgressObserver&&) noexcept;

    void poll() noexcept;
    [[nodiscard]] bool finish_success(
        const HostBuildCompletionProof& proof) noexcept;
    void fail() noexcept;
    [[nodiscard]] HostBuildProgressSnapshot snapshot() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::cli
