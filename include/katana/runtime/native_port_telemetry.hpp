#pragma once

#include "katana/runtime/native_port.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace katana::runtime {

// Native product performance telemetry is deliberately a small aggregate
// surface.  It records broad host boundaries, never individual SH-4
// instructions or graphics draws. AOT and simulation are separate buckets:
// generated guest dispatch is timed as AOT, while bootstrap/dispatch setup
// orchestration is timed as Simulation. They are non-overlapping and never
// become a trace stream.
enum class NativePortTelemetryStage : std::uint8_t {
    Aot = 0u,
    Simulation,
    Provider,
    RenderPacketBuild,
    RenderSubmit,
    PresentWait,
    AudioDecode,
    AudioMix,
    GpuTime,
    Count,
};

inline constexpr std::size_t native_port_telemetry_stage_count =
    static_cast<std::size_t>(NativePortTelemetryStage::Count);
inline constexpr std::uint32_t native_port_telemetry_schema_version = 1u;
inline constexpr std::size_t native_port_telemetry_maximum_json_bytes =
    4096u;

[[nodiscard]] constexpr std::string_view native_port_telemetry_stage_name(
    const NativePortTelemetryStage stage) noexcept {
    switch (stage) {
    case NativePortTelemetryStage::Aot:
        return "aot";
    case NativePortTelemetryStage::Simulation:
        return "simulation";
    case NativePortTelemetryStage::Provider:
        return "provider";
    case NativePortTelemetryStage::RenderPacketBuild:
        return "render_packet_build";
    case NativePortTelemetryStage::RenderSubmit:
        return "render_submit";
    case NativePortTelemetryStage::PresentWait:
        return "present_wait";
    case NativePortTelemetryStage::AudioDecode:
        return "audio_decode";
    case NativePortTelemetryStage::AudioMix:
        return "audio_mix";
    case NativePortTelemetryStage::GpuTime:
        return "gpu_time";
    case NativePortTelemetryStage::Count:
        break;
    }
    return "unknown";
}

struct NativePortTelemetryStageSnapshot final {
    // A stage is explicitly marked unavailable until a producer supplies a
    // measurement. This keeps unsupported GPU/audio instrumentation distinct
    // from a measured zero rather than manufacturing nominal values.
    bool available = false;
    // Number of completed timed regions (or explicit count events).
    std::uint64_t calls = 0u;
    // Wall-clock nanoseconds spent in the aggregate region.
    std::uint64_t time_ns = 0u;
    // Optional work units (frames, packets, samples, ...).  The unit is
    // chosen by the caller and is intentionally not interpreted here.
    std::uint64_t items = 0u;
};

struct NativePortTelemetrySnapshot final {
    std::uint32_t schema = native_port_telemetry_schema_version;
    // Incremented when a writer publishes a window.  Counters themselves are
    // cumulative, making snapshots useful even when a low-frequency publish
    // interval is not configured.
    std::uint64_t publication = 0u;
    std::uint64_t frames = 0u;
    std::array<NativePortTelemetryStageSnapshot,
               native_port_telemetry_stage_count>
        stages{};
    std::uint64_t render_queue_depth = 0u;
    std::uint64_t render_queue_high_watermark = 0u;
    std::uint64_t audio_queue_depth = 0u;
    std::uint64_t audio_queue_high_watermark = 0u;
    bool render_queue_available = false;
    bool audio_queue_available = false;

    // Serialization is only intended for the terminal/low-frequency
    // publication path.  The fixed field set is bounded well below
    // native_port_telemetry_maximum_json_bytes; no hotpath event formats
    // strings or allocates.
    [[nodiscard]] std::string serialize_json() const;
};

class NativePortTelemetry;

// One owner-thread accumulator.  Product simulation, render and audio
// threads each keep one writer and flush it at a frame/window boundary.  The
// normal sample path updates plain local integers; atomics are touched only
// when the writer is flushed.  The owner must outlive every writer it creates.
class NativePortTelemetryWriter final {
  public:
    NativePortTelemetryWriter() noexcept = default;
    explicit NativePortTelemetryWriter(NativePortTelemetry* owner) noexcept;
    ~NativePortTelemetryWriter() noexcept;

    NativePortTelemetryWriter(const NativePortTelemetryWriter&) = delete;
    NativePortTelemetryWriter& operator=(const NativePortTelemetryWriter&) = delete;
    NativePortTelemetryWriter(NativePortTelemetryWriter&& other) noexcept;
    NativePortTelemetryWriter& operator=(NativePortTelemetryWriter&& other) noexcept;

    // Record one already measured region.  This is the allocation-free,
    // lock-free hotpath operation; callers generally use scoped() below.
    void add(NativePortTelemetryStage stage,
             std::uint64_t time_ns,
             std::uint64_t items = 0u) noexcept;
    void mark_available(NativePortTelemetryStage stage) noexcept;
    void count(NativePortTelemetryStage stage,
               std::uint64_t items = 1u) noexcept;
    void frame(std::uint64_t count = 1u) noexcept;
    void flush() noexcept;

    [[nodiscard]] bool bound() const noexcept { return owner_ != nullptr; }

  private:
    friend class NativePortTelemetry;
    friend class NativePortTelemetryTimer;

    NativePortTelemetry* owner_ = nullptr;
    std::array<NativePortTelemetryStageSnapshot,
               native_port_telemetry_stage_count>
        local_stages_{};
    std::uint64_t local_frames_ = 0u;
};

// Timers are intended for broad boundaries such as one simulation frame,
// one provider call, one render submission or one audio pump.  They must not
// be placed around individual guest instructions or individual draw calls.
class NativePortTelemetryTimer final {
  public:
    NativePortTelemetryTimer(NativePortTelemetryWriter& writer,
                             NativePortTelemetryStage stage) noexcept;
    ~NativePortTelemetryTimer() noexcept;

    NativePortTelemetryTimer(const NativePortTelemetryTimer&) = delete;
    NativePortTelemetryTimer& operator=(const NativePortTelemetryTimer&) = delete;
    NativePortTelemetryTimer(NativePortTelemetryTimer&& other) noexcept;
    NativePortTelemetryTimer& operator=(NativePortTelemetryTimer&& other) noexcept;

    void stop() noexcept;

  private:
    NativePortTelemetryWriter* writer_ = nullptr;
    NativePortTelemetryStage stage_ = NativePortTelemetryStage::Aot;
    std::uint64_t started_ns_ = 0u;
};

class NativePortTelemetry final {
  public:
    NativePortTelemetry() noexcept;
    ~NativePortTelemetry() = default;

    NativePortTelemetry(const NativePortTelemetry&) = delete;
    NativePortTelemetry& operator=(const NativePortTelemetry&) = delete;

    [[nodiscard]] NativePortTelemetryWriter make_writer() noexcept;

    // Flushes one owner's local accumulator and publishes a new aggregate
    // window.  No output or allocation occurs here.
    void publish(NativePortTelemetryWriter& writer) noexcept;

    // Queue gauges are shared directly by producer/consumer threads.  The
    // current value and a monotone high-water mark are both retained.
    void observe_render_queue_depth(std::uint64_t depth) noexcept;
    void observe_audio_queue_depth(std::uint64_t depth) noexcept;
    void mark_stage_available(NativePortTelemetryStage stage) noexcept;

    [[nodiscard]] NativePortTelemetrySnapshot snapshot() const noexcept;
    [[nodiscard]] NativePortTelemetrySnapshot
    snapshot(NativePortTelemetryWriter& writer) noexcept;

  private:
    friend class NativePortTelemetryWriter;

    struct AtomicStage final {
        std::atomic<bool> available{false};
        std::atomic<std::uint64_t> calls{0u};
        std::atomic<std::uint64_t> time_ns{0u};
        std::atomic<std::uint64_t> items{0u};
    };

    std::array<AtomicStage, native_port_telemetry_stage_count> stages_{};
    std::atomic<std::uint64_t> frames_{0u};
    std::atomic<std::uint64_t> publication_{0u};
    std::atomic<std::uint64_t> render_queue_depth_{0u};
    std::atomic<std::uint64_t> render_queue_high_watermark_{0u};
    std::atomic<std::uint64_t> audio_queue_depth_{0u};
    std::atomic<std::uint64_t> audio_queue_high_watermark_{0u};
    std::atomic<bool> render_queue_available_{false};
    std::atomic<bool> audio_queue_available_{false};
};

// Simulation-thread host facade used by generated products.  It preserves the
// existing NativePortHostServices contract while timing only the simulation
// side of the broad render-packet construction window: begin_frame starts it
// and present_frame closes it after the title's draws. Render-submit and
// present-wait timings belong to the backend owner and are intentionally not
// inferred from this facade.
// The wrapped host remains the owner of the window/backend.
class NativePortTelemetryHostProxy final : public NativePortHostServices {
  public:
    NativePortTelemetryHostProxy(NativePortHostServices& host,
                                 NativePortTelemetryWriter& writer) noexcept;
    ~NativePortTelemetryHostProxy() override = default;

    [[nodiscard]] std::uint64_t monotonic_time_nanoseconds()
        const noexcept override;
    [[nodiscard]] NativePortLifecycleState poll_lifecycle() override;
    void synchronize_simulation_boundary() override;
    void begin_frame(std::uint64_t frame_index) override;
    void present_frame(std::uint64_t frame_index) override;
    [[nodiscard]] std::uint64_t presented_frames()
        const noexcept override;

  private:
    NativePortHostServices* host_ = nullptr;
    NativePortTelemetryWriter* writer_ = nullptr;
    // The title's frame interval between begin_frame and present_frame is
    // the packet-build boundary. Keeping this timer alive across the interval
    // avoids measuring only the cheap begin call while drawing happens later.
    std::optional<NativePortTelemetryTimer> render_packet_timer_;
};

// Steady-clock sampling is kept in the runtime implementation so product
// hotpaths only pay for it at the explicitly selected aggregate boundaries.
[[nodiscard]] std::uint64_t native_port_telemetry_now_ns() noexcept;

[[nodiscard]] NativePortTelemetryTimer scoped_native_port_telemetry(
    NativePortTelemetryWriter& writer,
    NativePortTelemetryStage stage) noexcept;

} // namespace katana::runtime
