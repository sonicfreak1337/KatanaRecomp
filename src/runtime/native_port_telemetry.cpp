#include "katana/runtime/native_port_telemetry.hpp"

#include <chrono>
#include <limits>

namespace katana::runtime {
namespace {

void saturating_add(std::uint64_t& destination,
                    const std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - destination)
        destination = std::numeric_limits<std::uint64_t>::max();
    else
        destination += value;
}

void saturating_atomic_add(std::atomic<std::uint64_t>& destination,
                           const std::uint64_t value) noexcept {
    if (value == 0u) return;
    auto current = destination.load(std::memory_order_relaxed);
    for (;;) {
        const auto next =
            value > std::numeric_limits<std::uint64_t>::max() - current
                ? std::numeric_limits<std::uint64_t>::max()
                : current + value;
        if (destination.compare_exchange_weak(
                current, next, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return;
    }
}

void saturating_atomic_add_release(
    std::atomic<std::uint64_t>& destination,
    const std::uint64_t value) noexcept {
    if (value == 0u) return;
    auto current = destination.load(std::memory_order_relaxed);
    for (;;) {
        const auto next =
            value > std::numeric_limits<std::uint64_t>::max() - current
                ? std::numeric_limits<std::uint64_t>::max()
                : current + value;
        if (destination.compare_exchange_weak(
                current, next, std::memory_order_release,
                std::memory_order_relaxed))
            return;
    }
}

void append_u64(std::string& output, const std::uint64_t value) {
    output.append(std::to_string(value));
}

} // namespace

std::string NativePortTelemetrySnapshot::serialize_json() const {
    std::string output;
    output.reserve(native_port_telemetry_maximum_json_bytes);
    output += "{\"schema\":";
    append_u64(output, schema);
    output += ",\"publication\":";
    append_u64(output, publication);
    output += ",\"frames\":";
    append_u64(output, frames);
    output += ",\"stages\":{";
    for (std::size_t index = 0u;
         index < native_port_telemetry_stage_count; ++index) {
        if (index != 0u) output.push_back(',');
        output.push_back('"');
        output += native_port_telemetry_stage_name(
            static_cast<NativePortTelemetryStage>(index));
        output += "\":{\"available\":";
        output += stages[index].available ? "true" : "false";
        output += ",\"calls\":";
        append_u64(output, stages[index].calls);
        output += ",\"time_ns\":";
        append_u64(output, stages[index].time_ns);
        output += ",\"items\":";
        append_u64(output, stages[index].items);
        output += '}';
    }
    output += "},\"queues\":{\"render\":{\"depth\":";
    append_u64(output, render_queue_depth);
    output += ",\"high_watermark\":";
    append_u64(output, render_queue_high_watermark);
    output += ",\"available\":";
    output += render_queue_available ? "true" : "false";
    output += "},\"audio\":{\"depth\":";
    append_u64(output, audio_queue_depth);
    output += ",\"high_watermark\":";
    append_u64(output, audio_queue_high_watermark);
    output += ",\"available\":";
    output += audio_queue_available ? "true" : "false";
    output += "}}}";
    return output;
}

NativePortTelemetryWriter::NativePortTelemetryWriter(
    NativePortTelemetry* const owner) noexcept
    : owner_(owner) {}

NativePortTelemetryWriter::~NativePortTelemetryWriter() noexcept {
    flush();
}

NativePortTelemetryWriter::NativePortTelemetryWriter(
    NativePortTelemetryWriter&& other) noexcept
    : owner_(other.owner_), local_stages_(other.local_stages_),
      local_frames_(other.local_frames_) {
    other.owner_ = nullptr;
    other.local_stages_ = {};
    other.local_frames_ = 0u;
}

NativePortTelemetryWriter& NativePortTelemetryWriter::operator=(
    NativePortTelemetryWriter&& other) noexcept {
    if (this == &other) return *this;
    flush();
    owner_ = other.owner_;
    local_stages_ = other.local_stages_;
    local_frames_ = other.local_frames_;
    other.owner_ = nullptr;
    other.local_stages_ = {};
    other.local_frames_ = 0u;
    return *this;
}

void NativePortTelemetryWriter::add(
    const NativePortTelemetryStage stage,
    const std::uint64_t time_ns,
    const std::uint64_t items) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= native_port_telemetry_stage_count) return;
    auto& counters = local_stages_[index];
    counters.available = true;
    saturating_add(counters.calls, 1u);
    saturating_add(counters.time_ns, time_ns);
    saturating_add(counters.items, items);
}

void NativePortTelemetryWriter::mark_available(
    const NativePortTelemetryStage stage) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= native_port_telemetry_stage_count) return;
    local_stages_[index].available = true;
}

void NativePortTelemetryWriter::count(
    const NativePortTelemetryStage stage,
    const std::uint64_t items) noexcept {
    add(stage, 0u, items);
}

void NativePortTelemetryWriter::frame(const std::uint64_t count_value) noexcept {
    saturating_add(local_frames_, count_value);
}

void NativePortTelemetryWriter::flush() noexcept {
    if (owner_ == nullptr) return;
    for (std::size_t index = 0u;
         index < native_port_telemetry_stage_count; ++index) {
        auto& local = local_stages_[index];
        if (!local.available && local.calls == 0u &&
            local.time_ns == 0u && local.items == 0u)
            continue;
        auto& destination = owner_->stages_[index];
        saturating_atomic_add(destination.calls, local.calls);
        saturating_atomic_add(destination.time_ns, local.time_ns);
        saturating_atomic_add(destination.items, local.items);
        if (local.available)
            destination.available.store(true, std::memory_order_release);
        local = {};
    }
    saturating_atomic_add(owner_->frames_, local_frames_);
    local_frames_ = 0u;
}

NativePortTelemetryTimer::NativePortTelemetryTimer(
    NativePortTelemetryWriter& writer,
    const NativePortTelemetryStage stage) noexcept
    : writer_(&writer), stage_(stage), started_ns_(native_port_telemetry_now_ns()) {}

NativePortTelemetryTimer::~NativePortTelemetryTimer() noexcept {
    stop();
}

NativePortTelemetryTimer::NativePortTelemetryTimer(
    NativePortTelemetryTimer&& other) noexcept
    : writer_(other.writer_), stage_(other.stage_),
      started_ns_(other.started_ns_) {
    other.writer_ = nullptr;
    other.started_ns_ = 0u;
}

NativePortTelemetryTimer& NativePortTelemetryTimer::operator=(
    NativePortTelemetryTimer&& other) noexcept {
    if (this == &other) return *this;
    stop();
    writer_ = other.writer_;
    stage_ = other.stage_;
    started_ns_ = other.started_ns_;
    other.writer_ = nullptr;
    other.started_ns_ = 0u;
    return *this;
}

void NativePortTelemetryTimer::stop() noexcept {
    if (writer_ == nullptr) return;
    const auto now = native_port_telemetry_now_ns();
    const auto elapsed = now >= started_ns_ ? now - started_ns_ : 0u;
    writer_->add(stage_, elapsed);
    writer_ = nullptr;
    started_ns_ = 0u;
}

NativePortTelemetry::NativePortTelemetry() noexcept = default;

NativePortTelemetryWriter NativePortTelemetry::make_writer() noexcept {
    return NativePortTelemetryWriter(this);
}

void NativePortTelemetry::publish(NativePortTelemetryWriter& writer) noexcept {
    writer.flush();
    saturating_atomic_add_release(publication_, 1u);
}

void NativePortTelemetry::observe_render_queue_depth(
    const std::uint64_t depth) noexcept {
    render_queue_depth_.store(depth, std::memory_order_relaxed);
    auto current = render_queue_high_watermark_.load(std::memory_order_relaxed);
    while (current < depth &&
           !render_queue_high_watermark_.compare_exchange_weak(
               current, depth, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    render_queue_available_.store(true, std::memory_order_release);
}

void NativePortTelemetry::observe_audio_queue_depth(
    const std::uint64_t depth) noexcept {
    audio_queue_depth_.store(depth, std::memory_order_relaxed);
    auto current = audio_queue_high_watermark_.load(std::memory_order_relaxed);
    while (current < depth &&
           !audio_queue_high_watermark_.compare_exchange_weak(
               current, depth, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    audio_queue_available_.store(true, std::memory_order_release);
}

void NativePortTelemetry::mark_stage_available(
    const NativePortTelemetryStage stage) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= native_port_telemetry_stage_count) return;
    stages_[index].available.store(true, std::memory_order_release);
}

NativePortTelemetrySnapshot NativePortTelemetry::snapshot() const noexcept {
    NativePortTelemetrySnapshot result;
    result.publication = publication_.load(std::memory_order_acquire);
    result.frames = frames_.load(std::memory_order_relaxed);
    for (std::size_t index = 0u;
         index < native_port_telemetry_stage_count; ++index) {
        result.stages[index].available =
            stages_[index].available.load(std::memory_order_acquire);
        result.stages[index].calls =
            stages_[index].calls.load(std::memory_order_relaxed);
        result.stages[index].time_ns =
            stages_[index].time_ns.load(std::memory_order_relaxed);
        result.stages[index].items =
            stages_[index].items.load(std::memory_order_relaxed);
    }
    result.render_queue_available =
        render_queue_available_.load(std::memory_order_acquire);
    result.render_queue_depth =
        render_queue_depth_.load(std::memory_order_relaxed);
    result.render_queue_high_watermark =
        render_queue_high_watermark_.load(std::memory_order_relaxed);
    result.audio_queue_available =
        audio_queue_available_.load(std::memory_order_acquire);
    result.audio_queue_depth =
        audio_queue_depth_.load(std::memory_order_relaxed);
    result.audio_queue_high_watermark =
        audio_queue_high_watermark_.load(std::memory_order_relaxed);
    return result;
}

NativePortTelemetrySnapshot NativePortTelemetry::snapshot(
    NativePortTelemetryWriter& writer) noexcept {
    writer.flush();
    return static_cast<const NativePortTelemetry&>(*this).snapshot();
}

NativePortTelemetryHostProxy::NativePortTelemetryHostProxy(
    NativePortHostServices& host,
    NativePortTelemetryWriter& writer) noexcept
    : host_(&host), writer_(&writer) {}

std::uint64_t NativePortTelemetryHostProxy::monotonic_time_nanoseconds()
    const noexcept {
    return host_->monotonic_time_nanoseconds();
}

NativePortLifecycleState NativePortTelemetryHostProxy::poll_lifecycle() {
    return host_->poll_lifecycle();
}

std::optional<NativePortDevelopmentStateRequest>
NativePortTelemetryHostProxy::take_development_state_request() {
    return host_->take_development_state_request();
}

void NativePortTelemetryHostProxy::synchronize_simulation_boundary() {
    host_->synchronize_simulation_boundary();
}

void NativePortTelemetryHostProxy::begin_frame(
    const std::uint64_t frame_index) {
    // A frame's packet build spans all title draw submissions. Keep the
    // timer alive until present_frame rather than measuring only begin_frame.
    render_packet_timer_.reset();
    render_packet_timer_.emplace(
        *writer_, NativePortTelemetryStage::RenderPacketBuild);
    try {
        host_->begin_frame(frame_index);
        writer_->frame();
    } catch (...) {
        // Publish the failed interval before the proxy unwinds so a
        // terminal performance snapshot still contains this attempt.
        render_packet_timer_.reset();
        throw;
    }
}

void NativePortTelemetryHostProxy::present_frame(
    const std::uint64_t frame_index) {
    render_packet_timer_.reset();
    host_->present_frame(frame_index);
}

std::uint64_t NativePortTelemetryHostProxy::presented_frames()
    const noexcept {
    return host_->presented_frames();
}

std::uint64_t native_port_telemetry_now_ns() noexcept {
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    return count < 0 ? 0u : static_cast<std::uint64_t>(count);
}

NativePortTelemetryTimer scoped_native_port_telemetry(
    NativePortTelemetryWriter& writer,
    const NativePortTelemetryStage stage) noexcept {
    return NativePortTelemetryTimer(writer, stage);
}

} // namespace katana::runtime
