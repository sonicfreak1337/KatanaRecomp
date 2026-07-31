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
    std::optional<std::uint64_t> queued_work;
    std::optional<std::uint64_t> discovered;
    std::optional<std::uint64_t> started;
    std::optional<std::uint64_t> requeued;
    std::optional<std::uint64_t> cache_hits;
    std::optional<std::uint64_t> cache_misses;
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
};

using ProgressCallback = std::function<void(const ProgressEvent&)>;

namespace detail {
class ProgressCore;
struct ProgressDeliveryState;
}

class ProgressScope;

class ProgressReporter final {
  public:
    ProgressReporter() noexcept = default;
    explicit ProgressReporter(
        ProgressCallback callback,
        std::chrono::milliseconds minimum_update_interval = std::chrono::milliseconds(100),
        std::chrono::milliseconds heartbeat_interval = std::chrono::seconds(1));

    [[nodiscard]] bool enabled() const noexcept;
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

    [[nodiscard]] std::optional<PreparedEmission>
    prepare_emission_locked(ProgressState state, bool force) noexcept;
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
    std::chrono::steady_clock::time_point last_emission_{};
    std::uint64_t emission_revision_ = 0u;
    bool terminal_ = false;
};

[[nodiscard]] std::string_view progress_operation_name(ProgressOperation operation) noexcept;
[[nodiscard]] std::string_view progress_state_name(ProgressState state) noexcept;
[[nodiscard]] std::string_view progress_unit_name(ProgressUnit unit) noexcept;

} // namespace katana
