#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace katana {

inline constexpr std::string_view progress_event_schema = "katana-progress-v1";
inline constexpr std::uint32_t progress_event_schema_version = 1u;

enum class ProgressOperation : std::uint8_t {
    InputProvenance,
    GdiOpen,
    GdiTrackHash,
    PackedDiscOpen,
    PackedDiscContentIdentity,
    PackedDiscWrite,
    PackedDiscVerify,
    DiscInstall,
    DiscInstallSourceVerify,
    DiscLoad,
    BootImage,
    ProgramValidation,
    ControlFlowAnalysis,
    FunctionValueAnalysis,
    CandidateResolution,
    LatentAotAnalysis,
    IrGeneration,
    IrOptimization,
    SourceGeneration,
    MetadataGeneration,
    ArtifactWrite,
    Configure,
    HostRuntimeBuild,
    Compilation,
    Linking,
    Packaging,
    RuntimeStartup,
    PortBuild,
    ControlFlowRound,
    CandidateContractIteration,
};

enum class ProgressState : std::uint8_t {
    Started,
    Running,
    Heartbeat,
    Completed,
    Cached,
    Skipped,
    Failed,
};

enum class ProgressUnit : std::uint8_t {
    None,
    Bytes,
    Tracks,
    Sectors,
    Chunks,
    Files,
    Functions,
    Modules,
    Partitions,
    TranslationUnits,
    Steps,
};

struct ProgressCounterSnapshot final {
    std::optional<std::uint64_t> iteration;
    std::optional<std::uint64_t> pass;
    std::optional<std::uint64_t> active_workers;
    std::optional<std::uint64_t> evaluation_requests;
    std::optional<std::uint64_t> active_evaluation_requests;
    std::optional<std::uint64_t> evaluation_request_nanoseconds;
    std::optional<std::uint64_t> maximum_evaluation_request_nanoseconds;
    std::optional<std::uint64_t> cache_key_builds;
    std::optional<std::uint64_t> active_cache_key_builds;
    std::optional<std::uint64_t> cache_key_build_nanoseconds;
    std::optional<std::uint64_t> maximum_cache_key_build_nanoseconds;
    std::optional<std::uint64_t> cache_waits;
    std::optional<std::uint64_t> active_cache_waits;
    std::optional<std::uint64_t> cache_wait_nanoseconds;
    std::optional<std::uint64_t> maximum_cache_wait_nanoseconds;
    std::optional<std::uint64_t> cache_replays;
    std::optional<std::uint64_t> active_cache_replays;
    std::optional<std::uint64_t> cache_replay_nanoseconds;
    std::optional<std::uint64_t> maximum_cache_replay_nanoseconds;
    std::optional<std::uint64_t> physical_evaluations;
    std::optional<std::uint64_t> active_physical_evaluations;
    std::optional<std::uint64_t> physical_evaluation_nanoseconds;
    std::optional<std::uint64_t>
        maximum_physical_evaluation_nanoseconds;
    std::optional<std::uint64_t> cache_commits;
    std::optional<std::uint64_t> active_cache_commits;
    std::optional<std::uint64_t> cache_commit_nanoseconds;
    std::optional<std::uint64_t> maximum_cache_commit_nanoseconds;
    std::optional<std::uint64_t> queued_work;
    std::optional<std::uint64_t> discovered;
    std::optional<std::uint64_t> started;
    std::optional<std::uint64_t> requeued;
    std::optional<std::uint64_t> cache_hits;
    std::optional<std::uint64_t> cache_misses;
    std::optional<std::uint64_t> planned_work;
    std::optional<std::uint64_t> ready_work;
    std::optional<std::uint64_t> committed_work;
    std::optional<std::uint64_t> configured_workers;
    std::optional<std::uint64_t> added_work;
    std::optional<bool> growing_workset;
    std::optional<std::uint64_t> head_of_line_index;
    std::optional<std::uint64_t> head_of_line_elapsed_milliseconds;
    std::optional<std::uint64_t> ready_ahead;
    std::optional<std::uint64_t> cache_lookups;
    std::optional<std::uint64_t> cache_ready_hits;
    std::optional<std::uint64_t> cache_in_flight_coalesces;
    // Additional physical evaluations after a logical cache hit could not
    // replay the exact inventory payload. This is deliberately independent
    // from the hit/miss accounting identity.
    std::optional<std::uint64_t> cache_replay_fallback_recomputes;
    // Physical evaluations intentionally bypassing the cache for opt-in
    // analyzer diagnostics. Product performance runs require this to be zero.
    std::optional<std::uint64_t> cache_diagnostic_bypass_evaluations;
    std::optional<std::uint64_t> cache_evictions;
    std::optional<std::uint64_t> cache_entries;
    // Deterministic retained-payload admission budget; never process RSS.
    std::optional<std::uint64_t> cache_retained_payload_bytes;
    std::optional<std::uint64_t> cache_miss_cold;
    std::optional<std::uint64_t> cache_miss_evicted;
    std::optional<std::uint64_t> cache_miss_oversize_or_no_exact_replay;
    std::optional<std::uint64_t> cache_miss_function_shape_changed;
    std::optional<std::uint64_t> cache_miss_projected_ingress_changed;
    std::optional<std::uint64_t> cache_miss_summary_dependency_changed;
    std::optional<std::uint64_t> cache_miss_abi_contract_changed;
    std::optional<std::uint64_t> cache_miss_resolution_lens_changed;
    std::optional<std::uint64_t> cache_miss_inventory_sink_changed;
    std::optional<std::uint64_t> cache_miss_isolation_partition_changed;
    std::optional<std::uint64_t> cache_miss_contextual_summary_changed;
    std::optional<std::uint64_t> cache_miss_tail_ingress_changed;
};

struct ProgressEvent final {
    ProgressOperation operation = ProgressOperation::InputProvenance;
    ProgressState state = ProgressState::Started;
    ProgressUnit unit = ProgressUnit::None;
    std::uint64_t sequence = 0u;
    std::uint64_t elapsed_milliseconds = 0u;
    std::uint64_t scope_id = 0u;
    std::optional<std::uint64_t> parent_scope_id;
    std::uint64_t completed = 0u;
    std::optional<std::uint64_t> total;
    ProgressCounterSnapshot counters;
    std::string label;
    std::uint64_t scope_elapsed_milliseconds = 0u;
    // Cumulative for the reporter. Once nonzero, later records make the
    // incomplete observation stream explicit instead of silently sampling it.
    std::uint64_t dropped_observations = 0u;
    bool telemetry_complete = true;
};

using ProgressCallback = std::function<void(const ProgressEvent&)>;

namespace detail {
class ProgressCore;
struct ProgressDeliveryState;
} // namespace detail

class ProgressScope;

class ProgressReporter final {
  public:
    ProgressReporter() noexcept = default;
    explicit ProgressReporter(
        ProgressCallback callback,
        std::chrono::milliseconds minimum_update_interval = std::chrono::milliseconds(100),
        std::chrono::milliseconds heartbeat_interval = std::chrono::seconds(1));

    [[nodiscard]] bool enabled() const noexcept;
    // Exact cumulative loss snapshot for bridges which discover an incomplete
    // stream only after the final callback. A failed seal can legitimately
    // have zero drops when the sole cause is an active scope.
    [[nodiscard]] std::uint64_t dropped_observations() const noexcept;
    // Marks loss discovered by an adapter outside ProgressCore (for example a
    // legacy observer which threw). The loss is sticky and makes sealing fail
    // closed without affecting the observed product work.
    void record_observation_loss(
        std::uint64_t amount = 1u) const noexcept;
    // Waits until every progress callback admitted before this call has
    // returned. Callbacks admitted later do not extend this fence. Calling
    // flush from the callback itself cannot wait and returns false instead.
    [[nodiscard]] bool flush() const noexcept;
    // Atomically closes the reporter to new event admissions, waits for all
    // producers which entered before the seal and all of their admitted
    // callbacks, and verifies that no scope remains active and no observation
    // was discarded. The seal is permanent and idempotent. A false result is
    // sticky: callers must fail closed instead of publishing a successful
    // terminal telemetry record.
    [[nodiscard]] bool seal_and_flush() const noexcept;
    [[nodiscard]] ProgressScope begin(ProgressOperation operation,
                                      ProgressUnit unit = ProgressUnit::None,
                                      std::optional<std::uint64_t> total = std::nullopt,
                                      std::string label = {}) const;

  private:
    friend class ProgressScope;
    ProgressReporter(std::shared_ptr<detail::ProgressCore> core,
                     std::optional<std::uint64_t> parent_scope_id) noexcept;

    std::shared_ptr<detail::ProgressCore> core_;
    std::optional<std::uint64_t> parent_scope_id_;
};

// Thread-safety contract:
// - After construction, calls on one address-stable ProgressScope may run
//   concurrently; updates, reads and exactly one effective terminal transition
//   are serialized internally.
// - Moving, move-assigning or destroying that same object while any thread can
//   still enter or execute one of its methods is not supported. The owner must
//   externally quiesce all users before changing the object's lifetime/address.
// This follows the C++ object-lifetime boundary explicitly; the internal mutex
// protects scope state, not an object whose lifetime has already ended.
class ProgressScope final {
  public:
    ProgressScope() noexcept = default;
    ProgressScope(const ProgressScope&) = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;
    ProgressScope(ProgressScope&& other) noexcept;
    ProgressScope& operator=(ProgressScope&& other) noexcept;
    ~ProgressScope();

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] std::uint64_t completed() const;
    [[nodiscard]] std::optional<std::uint64_t> total() const;
    [[nodiscard]] ProgressReporter child_reporter() const;

    void update(std::uint64_t completed);
    void update(std::uint64_t completed, ProgressCounterSnapshot counters);
    void update(ProgressCounterSnapshot counters);
    void advance(std::uint64_t amount);
    void heartbeat(ProgressCounterSnapshot counters = {});
    void complete();
    void complete(std::uint64_t completed);
    void cached();
    void skipped();
    void fail() noexcept;

  private:
    friend class ProgressReporter;
    struct PreparedEmission final {
        std::shared_ptr<detail::ProgressCore> core;
        std::shared_ptr<detail::ProgressDeliveryState> delivery_state;
        ProgressEvent event;
        std::uint64_t scope_revision = 0u;
    };

    ProgressScope(std::shared_ptr<detail::ProgressCore> core,
                  ProgressOperation operation,
                  ProgressUnit unit,
                  std::optional<std::uint64_t> total,
                  std::string label,
                  std::optional<std::uint64_t> parent_scope_id);

    [[nodiscard]] std::optional<PreparedEmission> prepare_emission_locked(ProgressState state,
                                                                          bool force) noexcept;
    static void deliver(PreparedEmission emission) noexcept;
    void abandon() noexcept;
    [[nodiscard]] std::unique_lock<std::recursive_mutex> lock_scope() const;

    std::shared_ptr<detail::ProgressCore> core_;
    mutable std::shared_ptr<std::recursive_mutex> scope_mutex_;
    std::shared_ptr<detail::ProgressDeliveryState> delivery_state_;
    ProgressOperation operation_ = ProgressOperation::InputProvenance;
    ProgressUnit unit_ = ProgressUnit::None;
    std::optional<std::uint64_t> total_;
    std::string label_;
    std::optional<std::uint64_t> parent_scope_id_;
    std::uint64_t scope_id_ = 0u;
    std::uint64_t completed_ = 0u;
    ProgressCounterSnapshot counters_;
    std::chrono::steady_clock::time_point scope_started_{};
    std::chrono::steady_clock::time_point last_emission_{};
    std::uint64_t emission_revision_ = 0u;
    bool start_reservation_pending_ = false;
    bool scope_registered_ = false;
    bool terminal_ = false;
};

[[nodiscard]] std::string_view progress_operation_name(ProgressOperation operation) noexcept;
[[nodiscard]] std::string_view progress_state_name(ProgressState state) noexcept;
[[nodiscard]] std::string_view progress_unit_name(ProgressUnit unit) noexcept;
[[nodiscard]] bool
progress_cache_accounting_valid(const ProgressCounterSnapshot& counters) noexcept;
[[nodiscard]] bool
progress_activity_accounting_valid(const ProgressCounterSnapshot& counters) noexcept;
[[nodiscard]] bool progress_event_telemetry_complete(const ProgressEvent& event) noexcept;
[[nodiscard]] std::string format_progress_event_json(const ProgressEvent& event);
[[nodiscard]] std::string format_progress_event_human(const ProgressEvent& event);

} // namespace katana
