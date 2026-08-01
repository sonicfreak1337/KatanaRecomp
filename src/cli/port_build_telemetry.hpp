#pragma once

#include "katana/progress.hpp"

#include <chrono>
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

inline constexpr std::string_view port_build_telemetry_stream_schema =
    "katana-port-build-telemetry";
inline constexpr std::uint32_t port_build_telemetry_stream_schema_version = 1u;
inline constexpr std::string_view port_build_manifest_schema =
    "katana-port-build-manifest";
inline constexpr std::string_view
    port_build_resolved_environment_schema =
        "katana-port-build-resolved-environment";
inline constexpr std::string_view port_build_host_command_schema =
    "katana-port-build-host-command";
inline constexpr std::string_view port_build_progress_schema =
    "katana-port-build-progress";
inline constexpr std::string_view port_build_phase_timings_schema =
    "katana-port-phase-timings";
inline constexpr std::string_view port_build_resource_schema =
    "katana-port-build-resource";
inline constexpr std::string_view port_build_terminal_schema =
    "katana-port-build-terminal";

enum class PortBuildTerminalOutcome : std::uint8_t {
    Completed,
    Failed,
    Cancelled,
    Abandoned,
};

struct PortBuildTelemetryOptions final {
    // No writer thread, host inspection or resource sampling is started when
    // this is empty. Telemetry therefore stays observational and opt-in.
    std::optional<std::filesystem::path> jsonl_path;

    // These are labels, not paths or arbitrary command lines. The compiler
    // which built this telemetry observer is recorded separately from the
    // resolved child toolchain and must never be presented as that toolchain.
    std::string build_profile;
    std::string job_kind = "port-build";

    // GPU reporting is intentionally identity/capability telemetry only.
    // KR-4974 does not select or execute a GPU compute backend.
    std::string gpu_identity;
    std::string gpu_backend;
    std::optional<std::uint64_t> gpu_memory_bytes;

    // The asynchronous writer has a hard queue and line-size bound. Records
    // rejected by either bound are counted and make the terminal completeness
    // flag false without affecting the observed port build.
    std::size_t maximum_pending_records = 2'048u;
    std::size_t maximum_record_bytes = 64u * 1'024u;
    std::chrono::milliseconds resource_sample_interval =
        std::chrono::seconds(1);

    // The compile budget is resolved before any child process is admitted.
    // requested records caller intent; effective is the hard upper bound
    // actually forwarded to Configure, the build tool and compiler.
    std::size_t host_compile_jobs_requested = 1u;
    std::size_t host_compile_jobs_effective = 1u;

    // A requested JSONL stream is complete only after the post-configure,
    // CMake-resolved child environment has been bound exactly once.
    bool require_resolved_environment = true;
    bool require_phase_timings = false;
};

// Returns the persistent sibling lock which exclusively owns a normalized
// telemetry target for one recorder lifetime. Callers which protect input,
// workspace or publish paths must apply the same alias checks to this path as
// to jsonl_path itself. The lock file is deliberately retained after use;
// ownership is represented solely by its held operating-system lock.
[[nodiscard]] std::filesystem::path
port_build_telemetry_writer_lock_path(
    const std::filesystem::path& jsonl_path);

struct PortBuildTelemetryStatus final {
    bool enabled = false;
    bool terminal_emitted = false;
    bool telemetry_complete = false;
    bool io_failed = false;
    std::uint64_t written_records = 0u;
    std::uint64_t lost_records = 0u;
    std::uint64_t upstream_dropped_observations = 0u;
    bool resolved_environment_recorded = false;
};

struct PortBuildResolvedPlatform final {
    std::string filesystem_type = "unknown";
    std::string filesystem_quality = "unsupported";
    std::string storage_type = "unknown";
    std::string storage_quality = "unsupported";
    std::string energy_profile = "unknown";
    std::string energy_quality = "unsupported";
};

struct PortBuildResolvedEnvironment final {
    // All identities are privacy-safe product/tool family labels, never
    // executable paths. Versions come from CMake's successful compiler and
    // linker identification, not from the parent CLI binary.
    std::string compiler_identity;
    std::string compiler_version;
    std::string compiler_quality = "cmake-identified";
    std::string linker_identity;
    std::string linker_version;
    std::string linker_quality = "cmake-identified";
    std::string cmake_version;
    std::string generator_identity;
    std::string generator_version;
    std::string generator_version_quality =
        "cmake-generator-implementation";
    std::string cache_launcher_identity = "none";
    std::string cache_launcher_quality = "cmake-cache-resolved";

    std::size_t analysis_jobs = 0u;
    std::size_t codegen_jobs = 0u;
    std::size_t host_compile_jobs_requested = 0u;
    std::size_t host_compile_jobs_effective = 0u;
    std::size_t runtime_jobs = 0u;

    PortBuildResolvedPlatform platform;
};

struct PortBuildHostCommandObservation final {
    // Privacy-safe stage label only; command lines and executable paths are
    // deliberately absent from this critical record.
    std::string stage;
    std::optional<int> host_exit_code;
    bool timed_out = false;
    bool interrupted = false;
    std::optional<int> forwarded_signal;
    bool process_tree_quiescent = false;
    std::string process_tree_scope = "unsupported";
    bool process_tree_query_complete = false;
};

struct PortBuildPhaseTimingSample final {
    std::string phase;
    std::uint64_t duration_ms = 0u;
    bool parallel = false;
};

// Inspects only categorical platform properties for the volume containing
// build_path. The path itself never becomes part of the returned value or the
// telemetry stream. Unsupported platform probes remain explicit and honest.
[[nodiscard]] PortBuildResolvedPlatform
inspect_port_build_resolved_platform(
    const std::filesystem::path& build_path) noexcept;

// Resolves the authoritative post-configure toolchain from CMake's generated
// state. Executable paths are consumed only to derive a basename-level cache
// launcher identity and are never retained or serialized.
[[nodiscard]] PortBuildResolvedEnvironment
resolve_port_build_cmake_environment(
    const std::filesystem::path& build_path,
    std::size_t analysis_jobs,
    std::size_t codegen_jobs,
    std::size_t host_compile_jobs_requested,
    std::size_t host_compile_jobs_effective,
    std::size_t runtime_jobs);

struct PortBuildGpuResourceSample final {
    // Percent in thousandths (100'000 == 100%). Producers must leave fields
    // empty when the selected platform cannot report them honestly.
    std::optional<std::uint64_t> utilization_percent_milli;
    std::optional<std::uint64_t> memory_bytes_current;
    std::optional<std::uint64_t> memory_bytes_peak;
    std::optional<std::uint64_t> host_to_device_bytes;
    std::optional<std::uint64_t> device_to_host_bytes;
    std::string quality = "unsupported";
};

#ifndef _WIN32
struct PortBuildPosixProcessTreeFinalSample final {
    bool cpu_available = false;
    std::uint64_t user_cpu_ms = 0u;
    std::uint64_t kernel_cpu_ms = 0u;
    bool faults_available = false;
    std::uint64_t page_faults = 0u;
    bool working_set_peak_available = false;
    std::uint64_t working_set_peak_bytes = 0u;
    bool io_blocks_available = false;
    std::uint64_t io_input_blocks = 0u;
    std::uint64_t io_output_blocks = 0u;
};
#endif

class PortBuildTelemetryRecorder final {
  public:
    explicit PortBuildTelemetryRecorder(
        PortBuildTelemetryOptions options = {}) noexcept;
    ~PortBuildTelemetryRecorder() noexcept;

    PortBuildTelemetryRecorder(const PortBuildTelemetryRecorder&) = delete;
    PortBuildTelemetryRecorder& operator=(
        const PortBuildTelemetryRecorder&) = delete;
    PortBuildTelemetryRecorder(PortBuildTelemetryRecorder&&) = delete;
    PortBuildTelemetryRecorder& operator=(
        PortBuildTelemetryRecorder&&) = delete;

    [[nodiscard]] bool enabled() const noexcept;

    // Suitable as the sink of a ProgressReporter. The returned callback must
    // not outlive this recorder.
    [[nodiscard]] ProgressCallback progress_callback() noexcept;
    void observe_progress(const ProgressEvent& event) noexcept;

    // Exactly one post-configure binding is accepted. False means the record
    // was invalid, duplicated, or could not be queued; explicit telemetry will
    // then remain incomplete and fail closed at the terminal contract.
    [[nodiscard]] bool record_resolved_environment(
        PortBuildResolvedEnvironment environment) noexcept;

    // Records the child command's result independently from the eventual CLI
    // exit. In particular, a host timeout (124) must not masquerade as the
    // port CLI's BuildFailure exit (7) in the terminal record.
    [[nodiscard]] bool record_host_command(
        PortBuildHostCommandObservation observation) noexcept;

    // Exactly one critical versioned phase-timing record is accepted before
    // terminal close. Parallel worker samples may overlap the serial total.
    [[nodiscard]] bool record_phase_timings(
        std::uint64_t total_ms,
        std::span<const PortBuildPhaseTimingSample> samples) noexcept;

    // Sticky bridge for loss discovered only by ProgressReporter::
    // seal_and_flush(). It is intentionally independent of a later progress
    // event because a sealed reporter admits no such event.
    // False means the bridge was not admitted before terminal closing or
    // could not be retained; explicit telemetry must then fail closed.
    [[nodiscard]] bool mark_upstream_incomplete(
        std::string_view reason,
        std::uint64_t cumulative_dropped_observations = 0u) noexcept;

    // A forced sample. Progress observations additionally trigger rate-limited
    // samples at resource_sample_interval.
    void sample_resources(std::string_view phase = {}) noexcept;
    void set_gpu_resource_sample(
        PortBuildGpuResourceSample sample) noexcept;

#ifdef _WIN32
    // The recorder never owns or closes the handle. Call clear_process_tree()
    // before closing it so the final cumulative job counters can be retained.
    void register_windows_job(std::uintptr_t native_job_handle) noexcept;
#else
    // The recorder never owns, signals or waits for the process group.
    void register_posix_process_group(std::int64_t process_group) noexcept;
    // Linux supervision can retain descendants which leave the registered
    // process group via setsid()/setpgid(). The recorder samples these PIDs
    // in addition to the group; callers replace the complete current set.
    void update_posix_process_tree_members(
        std::span<const std::int64_t> process_ids) noexcept;
    // Called with wait4()/equivalent cumulative evidence before the process
    // group is cleared. This retains short-lived descendants which may already
    // have disappeared from /proc between periodic samples.
    void record_posix_process_tree_final_sample(
        const PortBuildPosixProcessTreeFinalSample& sample) noexcept;
#endif
    void clear_process_tree() noexcept;

    // Idempotent. This drains the bounded queue, writes the terminal record and
    // flushes it before returning. A build failure can still have complete
    // telemetry; completeness describes the evidence stream, not the outcome.
    void finish(PortBuildTerminalOutcome outcome,
                int exit_code = 0,
                std::string_view terminal_phase = {}) noexcept;

    [[nodiscard]] PortBuildTelemetryStatus status() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view
port_build_terminal_outcome_name(PortBuildTerminalOutcome outcome) noexcept;

} // namespace katana::cli
