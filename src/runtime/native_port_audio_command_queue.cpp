#include "katana/runtime/native_port_audio_command_queue.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace katana::runtime {
namespace {

std::atomic<std::uint64_t> next_audio_queue_thread_token{1u};

[[nodiscard]] std::uint64_t audio_queue_thread_token() noexcept {
    static thread_local const auto token = [] {
        const auto value = next_audio_queue_thread_token.fetch_add(
            1u, std::memory_order_relaxed);
        return value == 0u
                   ? next_audio_queue_thread_token.fetch_add(
                         1u, std::memory_order_relaxed)
                   : value;
    }();
    return token;
}

[[nodiscard]] const char* failure_name(
    const NativePortAudioCommandQueueFailure failure) noexcept {
    switch (failure) {
    case NativePortAudioCommandQueueFailure::None:
        return "none";
    case NativePortAudioCommandQueueFailure::InvalidConfig:
        return "invalid-config";
    case NativePortAudioCommandQueueFailure::Disabled:
        return "disabled";
    case NativePortAudioCommandQueueFailure::ProducerThreadViolation:
        return "producer-thread";
    case NativePortAudioCommandQueueFailure::ConsumerThreadViolation:
        return "consumer-thread";
    case NativePortAudioCommandQueueFailure::ProducerLeaseOverlap:
        return "producer-lease-overlap";
    case NativePortAudioCommandQueueFailure::ConsumerLeaseOverlap:
        return "consumer-lease-overlap";
    case NativePortAudioCommandQueueFailure::ProducerLeaseAbandoned:
        return "producer-lease-abandoned";
    case NativePortAudioCommandQueueFailure::ConsumerLeaseAbandoned:
        return "consumer-lease-abandoned";
    case NativePortAudioCommandQueueFailure::StampInitialSequence:
        return "stamp-initial-sequence";
    case NativePortAudioCommandQueueFailure::StampRegression:
        return "stamp-regression";
    case NativePortAudioCommandQueueFailure::StampGap:
        return "stamp-gap";
    case NativePortAudioCommandQueueFailure::StampOverflow:
        return "stamp-overflow";
    case NativePortAudioCommandQueueFailure::FrameRegression:
        return "frame-regression";
    case NativePortAudioCommandQueueFailure::CommandSequenceExhausted:
        return "command-sequence-exhausted";
    case NativePortAudioCommandQueueFailure::PayloadOverflow:
        return "payload-overflow";
    case NativePortAudioCommandQueueFailure::PayloadReservationOverflow:
        return "payload-reservation-overflow";
    case NativePortAudioCommandQueueFailure::InvalidAckSlot:
        return "invalid-ack-slot";
    case NativePortAudioCommandQueueFailure::AckSlotBusy:
        return "ack-slot-busy";
    case NativePortAudioCommandQueueFailure::AckResultOverflow:
        return "ack-result-overflow";
    case NativePortAudioCommandQueueFailure::InvalidAckStatus:
        return "invalid-ack-status";
    case NativePortAudioCommandQueueFailure::WorkerFailure:
        return "worker-failure";
    case NativePortAudioCommandQueueFailure::QueueClosed:
        return "queue-closed";
    case NativePortAudioCommandQueueFailure::QueueFailed:
        return "queue-failed";
    case NativePortAudioCommandQueueFailure::ShutdownWithOutstandingLease:
        return "shutdown-with-outstanding-lease";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool valid_target(
    const NativePortAudioCommandTarget target) noexcept {
    return static_cast<std::uint8_t>(target) <=
           static_cast<std::uint8_t>(NativePortAudioCommandTarget::Lifecycle);
}

[[nodiscard]] constexpr bool valid_ack_status(
    const NativePortAudioCommandAckStatus status) noexcept {
    return status == NativePortAudioCommandAckStatus::Completed ||
           status == NativePortAudioCommandAckStatus::Cancelled ||
           status == NativePortAudioCommandAckStatus::Failed;
}

} // namespace

NativePortAudioCommandQueueError::NativePortAudioCommandQueueError(
    const NativePortAudioCommandQueueFailure failure,
    const std::uint64_t command_sequence)
    : std::runtime_error(
          std::string("native-port-audio-command-queue-") +
          failure_name(failure) + ":" + std::to_string(command_sequence)),
      failure_(failure), command_sequence_(command_sequence) {}

NativePortAudioCommandQueueFailure
NativePortAudioCommandQueueError::failure() const noexcept {
    return failure_;
}

std::uint64_t NativePortAudioCommandQueueError::command_sequence()
    const noexcept {
    return command_sequence_;
}

struct NativePortAudioCommandQueue::CommandSlot final {
    NativePortAudioPodCommand command{};
    std::uint64_t payload_begin = 0u;
    std::uint64_t payload_reserved_size = 0u;
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(NativePortAudioCommandQueue::SlotState::Free)};
};

struct NativePortAudioCommandQueue::AckSlot final {
    NativePortAudioCommandAck value{};
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(NativePortAudioCommandQueue::AckSlotState::Free)};
};

NativePortAudioCommandProducerLease::NativePortAudioCommandProducerLease(
    NativePortAudioCommandQueue& queue,
    const std::uint32_t slot,
    const std::uint64_t command_sequence) noexcept
    : queue_(&queue), slot_(slot), command_sequence_(command_sequence),
      active_(true) {}

NativePortAudioCommandProducerLease::~NativePortAudioCommandProducerLease() {
    reset();
}

NativePortAudioCommandProducerLease::NativePortAudioCommandProducerLease(
    NativePortAudioCommandProducerLease&& other) noexcept
    : queue_(other.queue_), slot_(other.slot_),
      command_sequence_(other.command_sequence_), active_(other.active_) {
    other.queue_ = nullptr;
    other.active_ = false;
}

NativePortAudioCommandProducerLease&
NativePortAudioCommandProducerLease::operator=(
    NativePortAudioCommandProducerLease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    queue_ = other.queue_;
    slot_ = other.slot_;
    command_sequence_ = other.command_sequence_;
    active_ = other.active_;
    other.queue_ = nullptr;
    other.active_ = false;
    return *this;
}

bool NativePortAudioCommandProducerLease::valid() const noexcept {
    return active_ && queue_ != nullptr;
}

std::uint64_t NativePortAudioCommandProducerLease::command_sequence()
    const noexcept {
    return command_sequence_;
}

const NativePortAudioPodCommand&
NativePortAudioCommandProducerLease::command() const noexcept {
    static const NativePortAudioPodCommand empty{};
    return valid() ? queue_->slot_command(slot_) : empty;
}

std::span<std::byte> NativePortAudioCommandProducerLease::payload() noexcept {
    return valid() ? queue_->writable_payload(slot_)
                   : std::span<std::byte>{};
}

bool NativePortAudioCommandProducerLease::copy_payload(
    const std::span<const std::byte> bytes) noexcept {
    if (!valid()) return false;
    const auto destination = queue_->writable_payload(slot_);
    if (destination.size() != bytes.size()) return false;
    if (!bytes.empty()) {
        if (bytes.data() == nullptr) return false;
        std::memcpy(destination.data(), bytes.data(), bytes.size_bytes());
    }
    return true;
}

bool NativePortAudioCommandProducerLease::publish() noexcept {
    if (!valid()) return false;
    const auto result = queue_->publish_producer(slot_, command_sequence_);
    if (result) {
        command_sequence_ = queue_->slot_command(slot_).command_sequence;
        queue_ = nullptr;
        active_ = false;
    }
    return result;
}

void NativePortAudioCommandProducerLease::abort() noexcept {
    reset();
}

void NativePortAudioCommandProducerLease::reset() noexcept {
    if (!valid()) return;
    queue_->abort_producer(slot_, command_sequence_);
    queue_ = nullptr;
    active_ = false;
}

NativePortAudioCommandConsumerLease::NativePortAudioCommandConsumerLease(
    NativePortAudioCommandQueue& queue,
    const std::uint32_t slot,
    const std::uint64_t command_sequence,
    const bool cancelled) noexcept
    : queue_(&queue), slot_(slot), command_sequence_(command_sequence),
      cancelled_(cancelled), active_(true) {}

NativePortAudioCommandConsumerLease::~NativePortAudioCommandConsumerLease() {
    reset();
}

NativePortAudioCommandConsumerLease::NativePortAudioCommandConsumerLease(
    NativePortAudioCommandConsumerLease&& other) noexcept
    : queue_(other.queue_), slot_(other.slot_),
      command_sequence_(other.command_sequence_), cancelled_(other.cancelled_),
      active_(other.active_) {
    other.queue_ = nullptr;
    other.cancelled_ = false;
    other.active_ = false;
}

NativePortAudioCommandConsumerLease&
NativePortAudioCommandConsumerLease::operator=(
    NativePortAudioCommandConsumerLease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    queue_ = other.queue_;
    slot_ = other.slot_;
    command_sequence_ = other.command_sequence_;
    cancelled_ = other.cancelled_;
    active_ = other.active_;
    other.queue_ = nullptr;
    other.cancelled_ = false;
    other.active_ = false;
    return *this;
}

bool NativePortAudioCommandConsumerLease::valid() const noexcept {
    return active_ && queue_ != nullptr;
}

bool NativePortAudioCommandConsumerLease::cancelled() const noexcept {
    return valid() && cancelled_;
}

const NativePortAudioPodCommand&
NativePortAudioCommandConsumerLease::command() const noexcept {
    static const NativePortAudioPodCommand empty{};
    return valid() ? queue_->slot_command(slot_) : empty;
}

std::span<const std::byte>
NativePortAudioCommandConsumerLease::payload() const noexcept {
    return valid() ? queue_->readable_payload(slot_)
                   : std::span<const std::byte>{};
}

bool NativePortAudioCommandConsumerLease::complete(
    const NativePortAudioCommandAckResult& result) noexcept {
    if (!valid()) return false;
    if (cancelled_) {
        const auto cancelled = queue_->finish_consumer_cancelled(
            slot_, command_sequence_, NativePortAudioCommandAckStatus::Cancelled,
            result.error_code);
        if (cancelled) {
            queue_ = nullptr;
            cancelled_ = false;
            active_ = false;
        }
        return cancelled;
    }
    const auto completed = queue_->finish_consumer(
        slot_, command_sequence_, result);
    if (completed) {
        queue_ = nullptr;
        cancelled_ = false;
        active_ = false;
    }
    return completed;
}

bool NativePortAudioCommandConsumerLease::cancel(
    const std::uint32_t error_code) noexcept {
    if (!valid()) return false;
    const auto completed = queue_->finish_consumer_cancelled(
        slot_, command_sequence_, NativePortAudioCommandAckStatus::Cancelled,
        error_code);
    if (completed) {
        queue_ = nullptr;
        cancelled_ = false;
        active_ = false;
    }
    return completed;
}

bool NativePortAudioCommandConsumerLease::fail(
    const std::uint32_t error_code) noexcept {
    if (!valid()) return false;
    const auto completed = queue_->finish_consumer_cancelled(
        slot_, command_sequence_, NativePortAudioCommandAckStatus::Failed,
        error_code);
    if (completed) {
        queue_ = nullptr;
        cancelled_ = false;
        active_ = false;
    }
    return completed;
}

void NativePortAudioCommandConsumerLease::reset() noexcept {
    if (!valid()) return;
    queue_->abandon_consumer(slot_, command_sequence_);
    queue_ = nullptr;
    cancelled_ = false;
    active_ = false;
}

NativePortAudioCommandQueue::NativePortAudioCommandQueue(
    const NativePortAudioCommandQueueConfig& config)
    : config_(config) {
    if (!config_.enabled) {
        lifecycle_.store(NativePortAudioCommandQueueLifecycle::Disabled,
                         std::memory_order_release);
        return;
    }
    if (config_.maximum_commands == 0u ||
        config_.maximum_commands > 1'048'576u ||
        config_.maximum_payload_bytes == 0u ||
        config_.maximum_payload_bytes > (1u << 30u) ||
        config_.maximum_ack_slots == 0u ||
        config_.maximum_ack_slots > 1'048'576u)
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::InvalidConfig);
    try {
        commands_ = std::make_unique<CommandSlot[]>(config_.maximum_commands);
        payload_ = std::make_unique<std::byte[]>(config_.maximum_payload_bytes);
        acks_ = std::make_unique<AckSlot[]>(config_.maximum_ack_slots);
    } catch (...) {
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::InvalidConfig);
    }
    lifecycle_.store(NativePortAudioCommandQueueLifecycle::Running,
                     std::memory_order_release);
}

NativePortAudioCommandQueue::~NativePortAudioCommandQueue() {
    if (!config_.enabled) return;
    const auto producer = producer_position_.load(std::memory_order_acquire);
    const auto consumer = consumer_position_.load(std::memory_order_acquire);
    if (producer != consumer || producer_lease_active_.load(std::memory_order_acquire) ||
        consumer_lease_active_.load(std::memory_order_acquire)) {
        publish_error(
            NativePortAudioCommandQueueFailure::ShutdownWithOutstandingLease,
            0u);
    }
}

NativePortAudioCommandQueueMode
NativePortAudioCommandQueue::mode() const noexcept {
    return config_.mode;
}

std::optional<NativePortAudioCommandProducerLease>
NativePortAudioCommandQueue::try_begin_produce(
    const NativePortAudioPodCommand& command) {
    return try_begin_produce_impl(command);
}

std::optional<NativePortAudioCommandProducerLease>
NativePortAudioCommandQueue::wait_begin_produce(
    const NativePortAudioPodCommand& command) {
    for (;;) {
        // Observe the shared epoch before inspecting queue state. If the
        // consumer frees space after this load, its epoch increment either
        // makes wait() return immediately or wakes it. Loading the epoch only
        // after the failed state check can miss that sole transition and
        // sleep until an unrelated later command.
        const auto observed = event_epoch_.load(std::memory_order_acquire);
        if (auto lease = try_begin_produce_impl(command); lease.has_value())
            return lease;
        const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
        if (lifecycle == NativePortAudioCommandQueueLifecycle::Failed)
            throw NativePortAudioCommandQueueError(
                NativePortAudioCommandQueueFailure::QueueFailed);
        if (lifecycle == NativePortAudioCommandQueueLifecycle::Disabled)
            return std::nullopt;
        if (lifecycle == NativePortAudioCommandQueueLifecycle::Draining ||
            lifecycle == NativePortAudioCommandQueueLifecycle::Stopped)
            throw NativePortAudioCommandQueueError(
                NativePortAudioCommandQueueFailure::QueueClosed);
        queue_full_waits_.fetch_add(1u, std::memory_order_relaxed);
        event_epoch_.wait(observed, std::memory_order_acquire);
    }
}

std::optional<NativePortAudioCommandConsumerLease>
NativePortAudioCommandQueue::try_begin_consume() {
    return try_begin_consume_impl();
}

std::optional<NativePortAudioCommandConsumerLease>
NativePortAudioCommandQueue::wait_begin_consume() {
    for (;;) {
        // Pair the state inspection with an epoch captured first. A producer
        // publication between those operations must not become an already-
        // consumed notification followed by an unbounded wait.
        const auto observed = event_epoch_.load(std::memory_order_acquire);
        if (auto lease = try_begin_consume_impl(); lease.has_value())
            return lease;
        const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
        if (lifecycle == NativePortAudioCommandQueueLifecycle::Disabled ||
            lifecycle == NativePortAudioCommandQueueLifecycle::Stopped)
            return std::nullopt;
        if (lifecycle == NativePortAudioCommandQueueLifecycle::Failed &&
            consumer_position_.load(std::memory_order_acquire) >=
                producer_position_.load(std::memory_order_acquire))
            return std::nullopt;
        event_epoch_.wait(observed, std::memory_order_acquire);
    }
}

std::optional<NativePortAudioCommandAck>
NativePortAudioCommandQueue::try_read_ack(
    const std::uint32_t ack_slot,
    const std::uint64_t command_sequence) {
    return read_ack(ack_slot, command_sequence, false);
}

std::optional<NativePortAudioCommandAck>
NativePortAudioCommandQueue::wait_read_ack(
    const std::uint32_t ack_slot,
    const std::uint64_t command_sequence) {
    return read_ack(ack_slot, command_sequence, true);
}

void NativePortAudioCommandQueue::request_shutdown() noexcept {
    auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    while (lifecycle == NativePortAudioCommandQueueLifecycle::Running) {
        if (lifecycle_.compare_exchange_weak(
                lifecycle, NativePortAudioCommandQueueLifecycle::Draining,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            event_epoch_.fetch_add(1u, std::memory_order_release);
            event_epoch_.notify_all();
            return;
        }
    }
    event_epoch_.fetch_add(1u, std::memory_order_release);
    event_epoch_.notify_all();
}

void NativePortAudioCommandQueue::notify_consumer_event() noexcept {
    event_epoch_.fetch_add(1u, std::memory_order_release);
    event_epoch_.notify_all();
}

std::uint64_t
NativePortAudioCommandQueue::consumer_event_epoch_nonblocking() const noexcept {
    return event_epoch_.load(std::memory_order_acquire);
}

void NativePortAudioCommandQueue::wait_for_consumer_event(
    const std::uint64_t observed_epoch) const noexcept {
    event_epoch_.wait(observed_epoch, std::memory_order_acquire);
}

bool NativePortAudioCommandQueue::consumer_closed_nonblocking() const noexcept {
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    if (lifecycle == NativePortAudioCommandQueueLifecycle::Disabled ||
        lifecycle == NativePortAudioCommandQueueLifecycle::Stopped)
        return true;
    return lifecycle == NativePortAudioCommandQueueLifecycle::Failed &&
           consumer_position_.load(std::memory_order_acquire) >=
               producer_position_.load(std::memory_order_acquire);
}

void NativePortAudioCommandQueue::fail_terminal(
    const NativePortAudioCommandQueueFailure failure,
    const std::uint64_t command_sequence) noexcept {
    if (!config_.enabled || failure == NativePortAudioCommandQueueFailure::None)
        return;
    if (!consumer_thread_allowed()) {
        publish_error(
            NativePortAudioCommandQueueFailure::ConsumerThreadViolation,
            command_sequence);
        return;
    }
    publish_error(failure, command_sequence);
}

NativePortAudioCommandQueueSnapshot
NativePortAudioCommandQueue::snapshot() const noexcept {
    NativePortAudioCommandQueueSnapshot result;
    result.lifecycle = lifecycle_.load(std::memory_order_acquire);
    result.mode = config_.mode;
    if (first_error_published_.load(std::memory_order_acquire)) {
        result.first_error = static_cast<NativePortAudioCommandQueueFailure>(
            first_error_.load(std::memory_order_relaxed));
        result.first_error_command_sequence =
            first_error_command_sequence_.load(std::memory_order_relaxed);
    }
    result.producer_thread_identity =
        producer_thread_identity_.load(std::memory_order_acquire);
    result.consumer_thread_identity =
        consumer_thread_identity_.load(std::memory_order_acquire);
    result.submitted_commands =
        submitted_commands_.load(std::memory_order_acquire);
    result.completed_commands =
        completed_commands_.load(std::memory_order_acquire);
    result.cancelled_commands =
        cancelled_commands_.load(std::memory_order_acquire);
    result.failed_commands = failed_commands_.load(std::memory_order_acquire);
    result.submitted_payload_bytes =
        submitted_payload_bytes_.load(std::memory_order_acquire);
    result.completed_payload_bytes =
        completed_payload_bytes_.load(std::memory_order_acquire);
    result.cancelled_payload_bytes =
        cancelled_payload_bytes_.load(std::memory_order_acquire);
    result.queued_commands = queued_commands_nonblocking();
    const auto producer_payload =
        producer_payload_position_.load(std::memory_order_acquire);
    const auto consumer_payload =
        consumer_payload_position_.load(std::memory_order_acquire);
    result.queued_payload_bytes = producer_payload >= consumer_payload
                                      ? producer_payload - consumer_payload
                                      : 0u;
    result.queue_full_waits =
        queue_full_waits_.load(std::memory_order_acquire);
    result.ack_waits = ack_waits_.load(std::memory_order_acquire);
    result.published_acks =
        published_acks_.load(std::memory_order_acquire);
    result.consumed_acks = consumed_acks_.load(std::memory_order_acquire);
    result.next_command_sequence =
        next_command_sequence_.load(std::memory_order_acquire);
    result.last_submitted_command_sequence =
        last_submitted_command_sequence_.load(std::memory_order_acquire);
    result.last_completed_command_sequence =
        last_completed_command_sequence_.load(std::memory_order_acquire);
    result.has_last_submitted_stamp =
        has_last_submitted_stamp_.load(std::memory_order_acquire);
    result.has_last_completed_stamp =
        has_last_completed_stamp_.load(std::memory_order_acquire);
    result.last_submitted_stamp.frame_index =
        last_submitted_frame_.load(std::memory_order_acquire);
    result.last_submitted_stamp.guest_sequence =
        last_submitted_guest_sequence_.load(std::memory_order_acquire);
    result.last_completed_stamp.frame_index =
        last_completed_frame_.load(std::memory_order_acquire);
    result.last_completed_stamp.guest_sequence =
        last_completed_guest_sequence_.load(std::memory_order_acquire);
    return result;
}

std::uint64_t
NativePortAudioCommandQueue::queued_commands_nonblocking() const noexcept {
    const auto producer_position =
        producer_position_.load(std::memory_order_acquire);
    const auto consumer_position =
        consumer_position_.load(std::memory_order_acquire);
    return producer_position >= consumer_position
               ? producer_position - consumer_position
               : 0u;
}

std::optional<NativePortAudioCommandProducerLease>
NativePortAudioCommandQueue::try_begin_produce_impl(
    const NativePortAudioPodCommand& command) {
    if (!config_.enabled) return std::nullopt;
    if (!producer_thread_allowed()) {
        publish_error(
            NativePortAudioCommandQueueFailure::ProducerThreadViolation, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::ProducerThreadViolation);
    }
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    if (lifecycle == NativePortAudioCommandQueueLifecycle::Failed)
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::QueueFailed);
    if (lifecycle == NativePortAudioCommandQueueLifecycle::Draining ||
        lifecycle == NativePortAudioCommandQueueLifecycle::Stopped)
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::QueueClosed);
    if (lifecycle != NativePortAudioCommandQueueLifecycle::Running)
        return std::nullopt;
    bool producer_lease_expected = false;
    if (!producer_lease_active_.compare_exchange_strong(
            producer_lease_expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        publish_error(
            NativePortAudioCommandQueueFailure::ProducerLeaseOverlap, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::ProducerLeaseOverlap);
    }
    const auto release_lease = [this]() noexcept {
        producer_lease_active_.store(false, std::memory_order_release);
    };
    if (!valid_target(command.target)) {
        release_lease();
        publish_error(NativePortAudioCommandQueueFailure::InvalidConfig, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::InvalidConfig);
    }
    if (command.payload_size > config_.maximum_payload_bytes) {
        release_lease();
        publish_error(NativePortAudioCommandQueueFailure::PayloadOverflow, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::PayloadOverflow);
    }
    if (command.ack_slot != native_port_audio_command_queue_invalid_ack_slot &&
        command.ack_slot >= config_.maximum_ack_slots) {
        release_lease();
        publish_error(NativePortAudioCommandQueueFailure::InvalidAckSlot, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::InvalidAckSlot);
    }
    if (command.command_sequence != 0u || command.payload_offset != 0u) {
        release_lease();
        publish_error(NativePortAudioCommandQueueFailure::InvalidConfig, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::InvalidConfig);
    }
    const auto has_stamp =
        has_last_submitted_stamp_.load(std::memory_order_acquire);
    if (!has_stamp && command.stamp.guest_sequence != 0u) {
        release_lease();
        publish_error(
            NativePortAudioCommandQueueFailure::StampInitialSequence, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::StampInitialSequence);
    }
    if (has_stamp) {
        const auto last_frame =
            last_submitted_frame_.load(std::memory_order_relaxed);
        const auto last_guest_sequence =
            last_submitted_guest_sequence_.load(std::memory_order_relaxed);
        NativePortAudioCommandQueueFailure stamp_failure =
            NativePortAudioCommandQueueFailure::None;
        if (command.stamp.frame_index < last_frame) {
            stamp_failure =
                NativePortAudioCommandQueueFailure::FrameRegression;
        } else if (last_guest_sequence ==
                   std::numeric_limits<std::uint64_t>::max()) {
            stamp_failure = NativePortAudioCommandQueueFailure::StampOverflow;
        } else if (command.stamp.guest_sequence <= last_guest_sequence) {
            stamp_failure =
                NativePortAudioCommandQueueFailure::StampRegression;
        } else if (command.stamp.guest_sequence != last_guest_sequence + 1u) {
            stamp_failure = NativePortAudioCommandQueueFailure::StampGap;
        }
        if (stamp_failure != NativePortAudioCommandQueueFailure::None) {
            release_lease();
            publish_error(stamp_failure, 0u);
            throw NativePortAudioCommandQueueError(stamp_failure);
        }
    }
    const auto producer_position =
        producer_position_.load(std::memory_order_relaxed);
    auto& slot = commands_[producer_position % config_.maximum_commands];
    auto expected_state = static_cast<std::uint8_t>(SlotState::Free);
    if (!slot.state.compare_exchange_strong(
            expected_state, static_cast<std::uint8_t>(SlotState::Writing),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        release_lease();
        return std::nullopt;
    }
    auto producer_payload =
        producer_payload_position_.load(std::memory_order_relaxed);
    auto consumer_payload =
        consumer_payload_position_.load(std::memory_order_acquire);
    if (producer_payload < consumer_payload ||
        producer_payload - consumer_payload > config_.maximum_payload_bytes) {
        slot.state.store(static_cast<std::uint8_t>(SlotState::Free),
                         std::memory_order_release);
        release_lease();
        publish_error(
            NativePortAudioCommandQueueFailure::PayloadReservationOverflow,
            0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::PayloadReservationOverflow);
    }
    const auto payload_size = static_cast<std::uint64_t>(command.payload_size);
    const auto payload_capacity =
        static_cast<std::uint64_t>(config_.maximum_payload_bytes);
    auto offset = producer_payload % payload_capacity;
    auto padding = std::uint64_t{0u};
    if (payload_size != 0u) {
        const auto alignment = static_cast<std::uint64_t>(
            native_port_audio_command_queue_payload_alignment);
        const auto alignment_padding =
            (alignment - (offset % alignment)) % alignment;
        padding = offset + alignment_padding + payload_size <= payload_capacity
                      ? alignment_padding
                      : payload_capacity - offset;

        // A completely drained ring may otherwise lack enough logical room
        // for a maximum-sized payload solely because the previous command
        // ended away from physical offset zero. Rebase both equal cursors to
        // the next ring boundary; no live lease or payload can reference the
        // discarded empty suffix once producer/consumer command positions
        // are equal.
        if (producer_payload == consumer_payload &&
            producer_position ==
                consumer_position_.load(std::memory_order_acquire) &&
            padding + payload_size > payload_capacity) {
            const auto rebase = payload_capacity - offset;
            producer_payload += rebase;
            consumer_payload += rebase;
            producer_payload_position_.store(producer_payload,
                                             std::memory_order_relaxed);
            consumer_payload_position_.store(consumer_payload,
                                             std::memory_order_release);
            padding = 0u;
        }
    }
    const auto reservation_size = padding + payload_size;
    if (reservation_size > config_.maximum_payload_bytes ||
        producer_payload - consumer_payload >
            config_.maximum_payload_bytes - reservation_size) {
        slot.state.store(static_cast<std::uint8_t>(SlotState::Free),
                         std::memory_order_release);
        release_lease();
        return std::nullopt;
    }
    if (command.ack_slot != native_port_audio_command_queue_invalid_ack_slot) {
        auto& ack = acks_[command.ack_slot];
        auto expected_ack = static_cast<std::uint8_t>(AckSlotState::Free);
        if (!ack.state.compare_exchange_strong(
                expected_ack, static_cast<std::uint8_t>(AckSlotState::Pending),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            slot.state.store(static_cast<std::uint8_t>(SlotState::Free),
                             std::memory_order_release);
            release_lease();
            return std::nullopt;
        }
    }
    const auto next_sequence =
        next_command_sequence_.load(std::memory_order_relaxed);
    if (next_sequence == 0u) {
        if (command.ack_slot != native_port_audio_command_queue_invalid_ack_slot)
            acks_[command.ack_slot].state.store(
                static_cast<std::uint8_t>(AckSlotState::Free),
                std::memory_order_release);
        slot.state.store(static_cast<std::uint8_t>(SlotState::Free),
                         std::memory_order_release);
        release_lease();
        publish_error(
            NativePortAudioCommandQueueFailure::CommandSequenceExhausted, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::CommandSequenceExhausted);
    }
    slot.command = command;
    // The sequence is assigned by publish_producer after all producer-side
    // validation has succeeded. An aborted lease therefore cannot consume it.
    slot.command.command_sequence = 0u;
    slot.command.payload_offset = payload_size == 0u
                                      ? 0u
                                      : static_cast<std::uint32_t>(
                                            (producer_payload + padding) %
                                            payload_capacity);
    slot.payload_begin = producer_payload + padding;
    slot.payload_reserved_size = reservation_size;
    if (command.ack_slot != native_port_audio_command_queue_invalid_ack_slot) {
        auto& value = acks_[command.ack_slot].value;
        value = {};
        value.command_sequence = 0u;
        value.stamp = command.stamp;
        value.status = NativePortAudioCommandAckStatus::Pending;
    }
    producer_payload_position_.store(producer_payload + reservation_size,
                                     std::memory_order_release);
    return NativePortAudioCommandProducerLease(*this,
                                               static_cast<std::uint32_t>(
                                                   producer_position %
                                                   config_.maximum_commands),
                                               0u);
}

std::optional<NativePortAudioCommandConsumerLease>
NativePortAudioCommandQueue::try_begin_consume_impl() {
    if (!config_.enabled) return std::nullopt;
    if (!consumer_thread_allowed()) {
        publish_error(
            NativePortAudioCommandQueueFailure::ConsumerThreadViolation, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::ConsumerThreadViolation);
    }
    bool consumer_lease_expected = false;
    if (!consumer_lease_active_.compare_exchange_strong(
            consumer_lease_expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        publish_error(
            NativePortAudioCommandQueueFailure::ConsumerLeaseOverlap, 0u);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::ConsumerLeaseOverlap);
    }
    const auto consumer_position =
        consumer_position_.load(std::memory_order_relaxed);
    auto& slot = commands_[consumer_position % config_.maximum_commands];
    auto state = slot.state.load(std::memory_order_acquire);
    if (state != static_cast<std::uint8_t>(SlotState::Ready) &&
        state != static_cast<std::uint8_t>(SlotState::Cancelled)) {
        consumer_lease_active_.store(false, std::memory_order_release);
        const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
        if (lifecycle == NativePortAudioCommandQueueLifecycle::Draining &&
            consumer_position_.load(std::memory_order_acquire) ==
                producer_position_.load(std::memory_order_acquire)) {
            lifecycle_.store(NativePortAudioCommandQueueLifecycle::Stopped,
                             std::memory_order_release);
            event_epoch_.fetch_add(1u, std::memory_order_release);
            event_epoch_.notify_all();
        }
        return std::nullopt;
    }
    auto expected_state = state;
    if (!slot.state.compare_exchange_strong(
            expected_state, static_cast<std::uint8_t>(SlotState::Reading),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        consumer_lease_active_.store(false, std::memory_order_release);
        return std::nullopt;
    }
    return NativePortAudioCommandConsumerLease(
        *this, static_cast<std::uint32_t>(consumer_position %
                                           config_.maximum_commands),
        slot.command.command_sequence,
        state == static_cast<std::uint8_t>(SlotState::Cancelled));
}

bool NativePortAudioCommandQueue::publish_producer(
    const std::uint32_t slot_index,
    const std::uint64_t command_sequence) noexcept {
    if (!producer_lease_active_.load(std::memory_order_acquire) ||
        slot_index >= config_.maximum_commands)
        return false;
    if (!producer_thread_allowed()) return false;
    auto& slot = commands_[slot_index];
    if (command_sequence != 0u || slot.command.command_sequence != 0u ||
        slot.state.load(std::memory_order_acquire) !=
            static_cast<std::uint8_t>(SlotState::Writing))
        return false;
    const auto next_sequence =
        next_command_sequence_.load(std::memory_order_relaxed);
    if (next_sequence == 0u) return false;
    slot.command.command_sequence = next_sequence;
    if (slot.command.ack_slot != native_port_audio_command_queue_invalid_ack_slot)
        acks_[slot.command.ack_slot].value.command_sequence = next_sequence;
    next_command_sequence_.store(
        next_sequence == std::numeric_limits<std::uint64_t>::max()
            ? 0u
            : next_sequence + 1u,
        std::memory_order_relaxed);
    slot.state.store(static_cast<std::uint8_t>(SlotState::Ready),
                     std::memory_order_release);
    producer_position_.fetch_add(1u, std::memory_order_release);
    submitted_commands_.fetch_add(1u, std::memory_order_relaxed);
    submitted_payload_bytes_.fetch_add(slot.command.payload_size,
                                       std::memory_order_relaxed);
    last_submitted_command_sequence_.store(next_sequence,
                                            std::memory_order_release);
    last_submitted_frame_.store(slot.command.stamp.frame_index,
                                std::memory_order_release);
    last_submitted_guest_sequence_.store(slot.command.stamp.guest_sequence,
                                         std::memory_order_release);
    has_last_submitted_stamp_.store(true, std::memory_order_release);
    producer_lease_active_.store(false, std::memory_order_release);
    event_epoch_.fetch_add(1u, std::memory_order_release);
    event_epoch_.notify_all();
    return true;
}

void NativePortAudioCommandQueue::abort_producer(
    const std::uint32_t slot_index,
    const std::uint64_t command_sequence) noexcept {
    if (!producer_lease_active_.load(std::memory_order_acquire) ||
        slot_index >= config_.maximum_commands)
        return;
    auto& slot = commands_[slot_index];
    if (slot.command.command_sequence != command_sequence) return;
    producer_payload_position_.store(
        slot.payload_begin -
            (slot.payload_reserved_size - slot.command.payload_size),
        std::memory_order_release);
    if (slot.command.ack_slot != native_port_audio_command_queue_invalid_ack_slot)
        acks_[slot.command.ack_slot].state.store(
            static_cast<std::uint8_t>(AckSlotState::Free),
            std::memory_order_release);
    slot.state.store(static_cast<std::uint8_t>(SlotState::Free),
                     std::memory_order_release);
    slot.command = {};
    slot.payload_begin = 0u;
    slot.payload_reserved_size = 0u;
    producer_lease_active_.store(false, std::memory_order_release);
    event_epoch_.fetch_add(1u, std::memory_order_release);
    event_epoch_.notify_all();
}

bool NativePortAudioCommandQueue::finish_consumer(
    const std::uint32_t slot_index,
    const std::uint64_t command_sequence,
    const NativePortAudioCommandAckResult& result) noexcept {
    if (result.result_size > native_port_audio_command_queue_max_ack_result_bytes) {
        publish_error(
            NativePortAudioCommandQueueFailure::AckResultOverflow,
            command_sequence);
        NativePortAudioCommandAckResult failure{};
        failure.status = NativePortAudioCommandAckStatus::Failed;
        failure.error_code = 1u;
        return finalize_consumer(slot_index, command_sequence, failure);
    }
    if (!valid_ack_status(result.status)) {
        publish_error(NativePortAudioCommandQueueFailure::InvalidAckStatus,
                      command_sequence);
        NativePortAudioCommandAckResult failure{};
        failure.status = NativePortAudioCommandAckStatus::Failed;
        failure.error_code = 1u;
        return finalize_consumer(slot_index, command_sequence, failure);
    }
    return finalize_consumer(slot_index, command_sequence, result);
}

bool NativePortAudioCommandQueue::finish_consumer_cancelled(
    const std::uint32_t slot_index,
    const std::uint64_t command_sequence,
    const NativePortAudioCommandAckStatus status,
    const std::uint32_t error_code) noexcept {
    NativePortAudioCommandAckResult result{};
    result.status = status;
    result.error_code = error_code;
    if (!valid_ack_status(status)) {
        publish_error(NativePortAudioCommandQueueFailure::InvalidAckStatus,
                      command_sequence);
        result.status = NativePortAudioCommandAckStatus::Failed;
        result.error_code = 1u;
    }
    return finalize_consumer(slot_index, command_sequence, result);
}

bool NativePortAudioCommandQueue::finalize_consumer(
    const std::uint32_t slot_index,
    const std::uint64_t command_sequence,
    const NativePortAudioCommandAckResult& result) noexcept {
    if (!consumer_lease_active_.load(std::memory_order_acquire) ||
        slot_index >= config_.maximum_commands)
        return false;
    if (!consumer_thread_allowed()) {
        publish_error(
            NativePortAudioCommandQueueFailure::ConsumerThreadViolation,
            command_sequence);
        return false;
    }
    auto& slot = commands_[slot_index];
    if (slot.command.command_sequence != command_sequence ||
        slot.state.load(std::memory_order_acquire) !=
            static_cast<std::uint8_t>(SlotState::Reading))
        return false;
    if (!valid_ack_status(result.status) ||
        result.result_size > native_port_audio_command_queue_max_ack_result_bytes)
        return false;
    if (slot.command.ack_slot != native_port_audio_command_queue_invalid_ack_slot) {
        auto& ack = acks_[slot.command.ack_slot];
        auto& value = ack.value;
        value = {};
        value.command_sequence = command_sequence;
        value.stamp = slot.command.stamp;
        value.status = result.status;
        value.error_code = result.error_code;
        value.result_size = result.result_size;
        if (result.result_size != 0u)
            std::memcpy(value.bytes.data(), result.bytes.data(),
                        result.result_size);
        ack.state.store(
            static_cast<std::uint8_t>(result.status ==
                                              NativePortAudioCommandAckStatus::Completed
                                          ? AckSlotState::Completed
                                          : result.status ==
                                                NativePortAudioCommandAckStatus::Cancelled
                                            ? AckSlotState::Cancelled
                                            : AckSlotState::Failed),
            std::memory_order_release);
        published_acks_.fetch_add(1u, std::memory_order_relaxed);
    }
    if (result.status == NativePortAudioCommandAckStatus::Completed) {
        completed_commands_.fetch_add(1u, std::memory_order_relaxed);
        completed_payload_bytes_.fetch_add(slot.command.payload_size,
                                           std::memory_order_relaxed);
        last_completed_command_sequence_.store(command_sequence,
                                               std::memory_order_release);
        last_completed_frame_.store(slot.command.stamp.frame_index,
                                    std::memory_order_release);
        last_completed_guest_sequence_.store(slot.command.stamp.guest_sequence,
                                             std::memory_order_release);
        has_last_completed_stamp_.store(true, std::memory_order_release);
    } else if (result.status == NativePortAudioCommandAckStatus::Cancelled) {
        cancelled_commands_.fetch_add(1u, std::memory_order_relaxed);
        cancelled_payload_bytes_.fetch_add(slot.command.payload_size,
                                           std::memory_order_relaxed);
    } else {
        failed_commands_.fetch_add(1u, std::memory_order_relaxed);
    }
    // payload_begin already includes any alignment/wrap padding. Adding the
    // whole reservation again would double-count that padding and eventually
    // make a drained ring appear occupied.
    consumer_payload_position_.store(
        slot.payload_begin + slot.command.payload_size,
        std::memory_order_release);
    slot.state.store(static_cast<std::uint8_t>(SlotState::Free),
                     std::memory_order_release);
    consumer_position_.fetch_add(1u, std::memory_order_release);
    consumer_lease_active_.store(false, std::memory_order_release);
    event_epoch_.fetch_add(1u, std::memory_order_release);
    event_epoch_.notify_all();
    return true;
}

void NativePortAudioCommandQueue::abandon_consumer(
    const std::uint32_t slot,
    const std::uint64_t command_sequence) noexcept {
    if (!consumer_lease_active_.load(std::memory_order_acquire)) return;
    publish_error(
        NativePortAudioCommandQueueFailure::ConsumerLeaseAbandoned,
        command_sequence);
    static_cast<void>(finish_consumer_cancelled(
        slot, command_sequence, NativePortAudioCommandAckStatus::Failed, 1u));
}

bool NativePortAudioCommandQueue::producer_thread_allowed() noexcept {
    const auto token = audio_queue_thread_token();
    auto owner = producer_thread_identity_.load(std::memory_order_acquire);
    if (owner == 0u && producer_thread_identity_.compare_exchange_strong(
                           owner, token, std::memory_order_acq_rel,
                           std::memory_order_acquire))
        return true;
    return owner == token;
}

bool NativePortAudioCommandQueue::consumer_thread_allowed() noexcept {
    const auto token = audio_queue_thread_token();
    auto owner = consumer_thread_identity_.load(std::memory_order_acquire);
    if (owner == 0u && consumer_thread_identity_.compare_exchange_strong(
                           owner, token, std::memory_order_acq_rel,
                           std::memory_order_acquire))
        return true;
    return owner == token;
}

void NativePortAudioCommandQueue::publish_error(
    const NativePortAudioCommandQueueFailure failure,
    const std::uint64_t command_sequence) noexcept {
    if (failure == NativePortAudioCommandQueueFailure::None) return;
    bool expected = false;
    if (first_error_claimed_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        first_error_.store(static_cast<std::uint8_t>(failure),
                           std::memory_order_relaxed);
        first_error_command_sequence_.store(command_sequence,
                                            std::memory_order_relaxed);
        // Publish the error payload before making Failed observable. Losers
        // only cancel already-ready slots; they never publish lifecycle state
        // while the winner is still filling the first-error record.
        first_error_published_.store(true, std::memory_order_release);
        lifecycle_.store(NativePortAudioCommandQueueLifecycle::Failed,
                         std::memory_order_release);
    }
    cancel_ready_commands();
    event_epoch_.fetch_add(1u, std::memory_order_release);
    event_epoch_.notify_all();
}

void NativePortAudioCommandQueue::cancel_ready_commands() noexcept {
    if (!config_.enabled || config_.maximum_commands == 0u) return;
    const auto consumer = consumer_position_.load(std::memory_order_acquire);
    const auto producer = producer_position_.load(std::memory_order_acquire);
    if (producer < consumer) return;
    const auto count = std::min<std::uint64_t>(
        producer - consumer, config_.maximum_commands);
    for (std::uint64_t index = 0u; index < count; ++index) {
        auto& slot = commands_[(consumer + index) % config_.maximum_commands];
        auto expected = static_cast<std::uint8_t>(SlotState::Ready);
        static_cast<void>(slot.state.compare_exchange_strong(
            expected, static_cast<std::uint8_t>(SlotState::Cancelled),
            std::memory_order_acq_rel, std::memory_order_acquire));
    }
}

std::span<std::byte> NativePortAudioCommandQueue::writable_payload(
    const std::uint32_t slot_index) noexcept {
    if (!config_.enabled || slot_index >= config_.maximum_commands ||
        payload_ == nullptr)
        return {};
    const auto& slot = commands_[slot_index];
    if (slot.command.payload_size == 0u) return {};
    return {payload_.get() + slot.command.payload_offset,
            slot.command.payload_size};
}

std::span<const std::byte> NativePortAudioCommandQueue::readable_payload(
    const std::uint32_t slot_index) const noexcept {
    if (!config_.enabled || slot_index >= config_.maximum_commands ||
        payload_ == nullptr)
        return {};
    const auto& slot = commands_[slot_index];
    if (slot.command.payload_size == 0u) return {};
    return {payload_.get() + slot.command.payload_offset,
            slot.command.payload_size};
}

const NativePortAudioPodCommand& NativePortAudioCommandQueue::slot_command(
    const std::uint32_t slot_index) const noexcept {
    static const NativePortAudioPodCommand empty{};
    if (!config_.enabled || slot_index >= config_.maximum_commands ||
        commands_ == nullptr)
        return empty;
    return commands_[slot_index].command;
}

std::optional<NativePortAudioCommandAck>
NativePortAudioCommandQueue::read_ack(
    const std::uint32_t ack_slot,
    const std::uint64_t command_sequence,
    const bool wait) {
    if (!config_.enabled) return std::nullopt;
    if (!producer_thread_allowed()) {
        publish_error(
            NativePortAudioCommandQueueFailure::ProducerThreadViolation,
            command_sequence);
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::ProducerThreadViolation);
    }
    if (ack_slot >= config_.maximum_ack_slots)
        throw NativePortAudioCommandQueueError(
            NativePortAudioCommandQueueFailure::InvalidAckSlot);
    auto& ack = acks_[ack_slot];
    for (;;) {
        // Capture the notification epoch before reading the ACK. Completion
        // release-publishes the ACK and then advances this epoch. Therefore a
        // completion concurrent with the check either becomes visible below
        // or changes the value observed by wait(). The previous inverse order
        // could read Pending, then read the post-completion epoch, and sleep
        // forever despite the ACK already being terminal.
        const auto observed = event_epoch_.load(std::memory_order_acquire);
        const auto state = ack.state.load(std::memory_order_acquire);
        const auto terminal = state == static_cast<std::uint8_t>(AckSlotState::Completed) ||
                               state == static_cast<std::uint8_t>(AckSlotState::Cancelled) ||
                               state == static_cast<std::uint8_t>(AckSlotState::Failed);
        if (terminal) {
            const auto value = ack.value;
            if (value.command_sequence != command_sequence) return std::nullopt;
            auto expected = state;
            if (!ack.state.compare_exchange_strong(
                    expected, static_cast<std::uint8_t>(AckSlotState::Free),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            consumed_acks_.fetch_add(1u, std::memory_order_relaxed);
            event_epoch_.fetch_add(1u, std::memory_order_release);
            event_epoch_.notify_all();
            return value;
        }
        if (!wait) return std::nullopt;
        ack_waits_.fetch_add(1u, std::memory_order_relaxed);
        event_epoch_.wait(observed, std::memory_order_acquire);
        const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
        if (lifecycle == NativePortAudioCommandQueueLifecycle::Stopped ||
            (lifecycle == NativePortAudioCommandQueueLifecycle::Failed &&
             consumer_position_.load(std::memory_order_acquire) >=
                 producer_position_.load(std::memory_order_acquire)))
            return std::nullopt;
    }
}

bool native_port_audio_serial_reference_requested(
    const std::string_view value) noexcept {
    return value == "1";
}

bool native_port_audio_serial_reference_requested() noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const auto* const value = std::getenv(
        native_port_audio_serial_reference_environment.data());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return value != nullptr &&
           native_port_audio_serial_reference_requested(value);
}

} // namespace katana::runtime
