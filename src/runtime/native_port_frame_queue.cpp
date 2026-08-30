#include "katana/runtime/native_port_frame_queue.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace katana::runtime {
namespace {

std::atomic<std::uint64_t> next_frame_queue_thread_token{1u};

[[nodiscard]] std::uint64_t frame_queue_thread_token() noexcept {
    static thread_local const auto token = [] {
        const auto value = next_frame_queue_thread_token.fetch_add(
            1u, std::memory_order_relaxed);
        return value == 0u ? next_frame_queue_thread_token.fetch_add(
                                  1u, std::memory_order_relaxed)
                           : value;
    }();
    return token;
}

[[nodiscard]] constexpr bool valid_alignment(
    const std::uint32_t alignment) noexcept {
    return alignment != 0u &&
           alignment <= native_port_frame_payload_max_alignment &&
           (alignment & (alignment - 1u)) == 0u;
}

[[nodiscard]] constexpr std::uint64_t saturating_increment(
    const std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max()
               ? value
               : value + 1u;
}

} // namespace

bool native_port_render_thread_enabled() noexcept {
    static const bool enabled = [] {
#if defined(_WIN32)
        constexpr auto variable = "KATANA_PORT_DISABLE_RENDER_THREAD";
        std::size_t required = 0u;
        if (::getenv_s(&required, nullptr, 0u, variable) != 0 ||
            required != 2u)
            return true;
        char value[2]{};
        if (::getenv_s(&required, value, sizeof(value), variable) != 0)
            return true;
        return !native_port_render_thread_kill_switch_disables(value);
#else
        const auto* const value = std::getenv(
            "KATANA_PORT_DISABLE_RENDER_THREAD");
        return value == nullptr ||
               !native_port_render_thread_kill_switch_disables(value);
#endif
    }();
    return enabled;
}

void NativePortFrameQueue::PayloadDeleter::operator()(
    std::byte* const payload) const noexcept {
    ::operator delete[](payload, std::align_val_t{
                                    native_port_frame_payload_max_alignment});
}

NativePortFrameWriteLease::NativePortFrameWriteLease(
    NativePortFrameQueue& queue,
    const std::size_t arena,
    const std::uint64_t sequence) noexcept
    : queue_(&queue), arena_(arena), sequence_(sequence),
      uncaught_exceptions_(std::uncaught_exceptions()), active_(true) {}

NativePortFrameWriteLease::~NativePortFrameWriteLease() {
    reset();
}

NativePortFrameWriteLease::NativePortFrameWriteLease(
    NativePortFrameWriteLease&& other) noexcept
    : queue_(other.queue_), arena_(other.arena_), sequence_(other.sequence_),
      uncaught_exceptions_(other.uncaught_exceptions_), active_(other.active_) {
    other.queue_ = nullptr;
    other.active_ = false;
}

NativePortFrameWriteLease& NativePortFrameWriteLease::operator=(
    NativePortFrameWriteLease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    queue_ = other.queue_;
    arena_ = other.arena_;
    sequence_ = other.sequence_;
    uncaught_exceptions_ = other.uncaught_exceptions_;
    active_ = other.active_;
    other.queue_ = nullptr;
    other.active_ = false;
    return *this;
}

bool NativePortFrameWriteLease::valid() const noexcept {
    return active_ && queue_ != nullptr;
}

std::uint64_t NativePortFrameWriteLease::sequence() const noexcept {
    return sequence_;
}

bool NativePortFrameWriteLease::ensure_owner_thread() const noexcept {
    if (!valid()) return false;
    if (queue_->is_producer_thread()) return true;
    queue_->reject_foreign_write(arena_, sequence_);
    return false;
}

std::optional<std::uint32_t> NativePortFrameWriteLease::append_payload(
    const std::span<const std::byte> bytes,
    const std::uint32_t alignment) noexcept {
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return std::nullopt;
    }
    auto result = queue_->append_payload(
        arena_, sequence_, bytes, alignment);
    if (!result.has_value() &&
        queue_->snapshot().lifecycle ==
            NativePortFrameQueueLifecycle::Failed) {
        active_ = false;
        queue_ = nullptr;
    }
    return result;
}

bool NativePortFrameWriteLease::append_command_reference(
    const std::uint32_t kind,
    const std::uint32_t payload_offset,
    const std::uint32_t payload_size,
    const std::uint32_t flags) noexcept {
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return false;
    }
    const auto result = queue_->append_command_reference(
        arena_, sequence_, kind, payload_offset, payload_size, flags);
    if (!result && queue_->snapshot().lifecycle ==
                       NativePortFrameQueueLifecycle::Failed) {
        active_ = false;
        queue_ = nullptr;
    }
    return result;
}

bool NativePortFrameWriteLease::append_command(
    const std::uint32_t kind,
    const std::span<const std::byte> payload,
    const std::uint32_t alignment,
    const std::uint32_t flags) noexcept {
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return false;
    }
    const auto offset = append_payload(payload, alignment);
    if (!offset.has_value()) return false;
    return append_command_reference(
        kind,
        *offset,
        static_cast<std::uint32_t>(payload.size()),
        flags);
}

bool NativePortFrameWriteLease::publish() noexcept {
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return false;
    }
    const auto result = queue_->publish_write(arena_, sequence_);
    active_ = false;
    queue_ = nullptr;
    return result;
}

void NativePortFrameWriteLease::abort() noexcept {
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return;
    }
    queue_->abort_write(arena_, sequence_);
    active_ = false;
    queue_ = nullptr;
}

void NativePortFrameWriteLease::fail(
    const NativePortFrameQueueError error) noexcept {
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return;
    }
    queue_->fail_write(arena_, sequence_, error);
    active_ = false;
    queue_ = nullptr;
}

void NativePortFrameWriteLease::reset() noexcept {
    if (!valid()) return;
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return;
    }
    if (std::uncaught_exceptions() > uncaught_exceptions_)
        queue_->fail_write(
            arena_, sequence_, NativePortFrameQueueError::ProducerException);
    else
        queue_->abort_write(arena_, sequence_);
    active_ = false;
    queue_ = nullptr;
}

NativePortFrameReadLease::NativePortFrameReadLease(
    NativePortFrameQueue& queue,
    const std::size_t arena,
    const std::uint64_t sequence) noexcept
    : queue_(&queue), arena_(arena), sequence_(sequence),
      uncaught_exceptions_(std::uncaught_exceptions()), active_(true) {}

NativePortFrameReadLease::~NativePortFrameReadLease() {
    reset();
}

NativePortFrameReadLease::NativePortFrameReadLease(
    NativePortFrameReadLease&& other) noexcept
    : queue_(other.queue_), arena_(other.arena_), sequence_(other.sequence_),
      uncaught_exceptions_(other.uncaught_exceptions_), active_(other.active_) {
    other.queue_ = nullptr;
    other.active_ = false;
}

NativePortFrameReadLease& NativePortFrameReadLease::operator=(
    NativePortFrameReadLease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    queue_ = other.queue_;
    arena_ = other.arena_;
    sequence_ = other.sequence_;
    uncaught_exceptions_ = other.uncaught_exceptions_;
    active_ = other.active_;
    other.queue_ = nullptr;
    other.active_ = false;
    return *this;
}

bool NativePortFrameReadLease::valid() const noexcept {
    return active_ && queue_ != nullptr;
}

std::uint64_t NativePortFrameReadLease::sequence() const noexcept {
    return sequence_;
}

bool NativePortFrameReadLease::ensure_owner_thread() const noexcept {
    if (!valid()) return false;
    if (queue_->is_consumer_thread()) return true;
    queue_->reject_foreign_read(arena_, sequence_);
    return false;
}

std::span<const NativePortFrameCommand>
NativePortFrameReadLease::commands() const noexcept {
    return ensure_owner_thread()
               ? queue_->commands(arena_, sequence_)
               : std::span<const NativePortFrameCommand>{};
}

std::span<const std::byte> NativePortFrameReadLease::payload()
    const noexcept {
    return ensure_owner_thread() ? queue_->payload(arena_, sequence_)
                                 : std::span<const std::byte>{};
}

std::span<const std::byte> NativePortFrameReadLease::command_payload(
    const NativePortFrameCommand& command) const noexcept {
    const auto bytes = payload();
    if (command.payload_offset > bytes.size() ||
        command.payload_size > bytes.size() - command.payload_offset)
        return {};
    return bytes.subspan(command.payload_offset, command.payload_size);
}

bool NativePortFrameReadLease::complete() noexcept {
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return false;
    }
    const auto result = queue_->complete_read(arena_, sequence_);
    active_ = false;
    queue_ = nullptr;
    return result;
}

void NativePortFrameReadLease::fail(
    const NativePortFrameQueueError error) noexcept {
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return;
    }
    queue_->fail_read(arena_, sequence_, error);
    active_ = false;
    queue_ = nullptr;
}

void NativePortFrameReadLease::reset() noexcept {
    if (!valid()) return;
    if (!ensure_owner_thread()) {
        active_ = false;
        queue_ = nullptr;
        return;
    }
    queue_->fail_read(
        arena_, sequence_,
        std::uncaught_exceptions() > uncaught_exceptions_
            ? NativePortFrameQueueError::ConsumerException
            : NativePortFrameQueueError::ConsumerLeaseAbandoned);
    active_ = false;
    queue_ = nullptr;
}

NativePortFrameQueue::NativePortFrameQueue(
    const NativePortFrameQueueConfig& config)
    : config_(config) {
    if (config_.maximum_commands_per_frame == 0u ||
        config_.maximum_payload_bytes_per_frame == 0u)
        throw std::invalid_argument("native-port-frame-queue-capacity");

    if (!config_.enabled ||
        (!native_port_render_thread_enabled() &&
         config_.threading_mode !=
             NativePortFrameQueueConfig::ThreadingMode::SerialReference)) {
        lifecycle_.store(
            NativePortFrameQueueLifecycle::Disabled,
            std::memory_order_relaxed);
        return;
    }

    for (auto& arena : arenas_) {
        arena.commands =
            std::make_unique_for_overwrite<NativePortFrameCommand[]>(
                config_.maximum_commands_per_frame);
        arena.payload.reset(static_cast<std::byte*>(::operator new[](
            config_.maximum_payload_bytes_per_frame,
            std::align_val_t{native_port_frame_payload_max_alignment})));
    }
    lifecycle_.store(
        NativePortFrameQueueLifecycle::Running,
        std::memory_order_release);
}

NativePortFrameQueue::~NativePortFrameQueue() {
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    if (lifecycle == NativePortFrameQueueLifecycle::Running ||
        lifecycle == NativePortFrameQueueLifecycle::Draining) {
        if (producer_lease_active_.load(std::memory_order_acquire) ||
            consumer_lease_active_.load(std::memory_order_acquire) ||
            producer_position_.load(std::memory_order_acquire) !=
                consumer_position_.load(std::memory_order_acquire))
            publish_error(
                NativePortFrameQueueError::ShutdownWithOutstandingFrame,
                producer_position_.load(std::memory_order_relaxed));
        else
            request_shutdown();
    }
}

bool NativePortFrameQueue::enabled() const noexcept {
    return lifecycle_.load(std::memory_order_acquire) !=
           NativePortFrameQueueLifecycle::Disabled;
}

bool NativePortFrameQueue::bind_producer_thread() noexcept {
    const auto token = frame_queue_thread_token();
    const bool serial_reference =
        config_.threading_mode ==
        NativePortFrameQueueConfig::ThreadingMode::SerialReference;
    if (!serial_reference &&
        consumer_thread_token_.load(std::memory_order_acquire) == token) {
        publish_error(NativePortFrameQueueError::ThreadDomainOverlap, 0u);
        return false;
    }
    auto owner = producer_thread_token_.load(std::memory_order_acquire);
    bool newly_bound = false;
    if (owner == 0u) {
        if (!producer_thread_token_.compare_exchange_strong(
                owner, token, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (owner != token) {
                publish_error(
                    NativePortFrameQueueError::ProducerThreadViolation, 0u);
                return false;
            }
        } else
            newly_bound = true;
    } else if (owner != token) {
        publish_error(
            NativePortFrameQueueError::ProducerThreadViolation, 0u);
        return false;
    }
    if (!serial_reference &&
        consumer_thread_token_.load(std::memory_order_acquire) == token) {
        publish_error(NativePortFrameQueueError::ThreadDomainOverlap, 0u);
        return false;
    }
    if (newly_bound) notify_state_change();
    return true;
}

bool NativePortFrameQueue::bind_consumer_thread() noexcept {
    const auto token = frame_queue_thread_token();
    const bool serial_reference =
        config_.threading_mode ==
        NativePortFrameQueueConfig::ThreadingMode::SerialReference;
    if (!serial_reference &&
        producer_thread_token_.load(std::memory_order_acquire) == token) {
        publish_error(NativePortFrameQueueError::ThreadDomainOverlap, 0u);
        return false;
    }
    auto owner = consumer_thread_token_.load(std::memory_order_acquire);
    bool newly_bound = false;
    if (owner == 0u) {
        if (!consumer_thread_token_.compare_exchange_strong(
                owner, token, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (owner != token) {
                publish_error(
                    NativePortFrameQueueError::ConsumerThreadViolation, 0u);
                return false;
            }
        } else
            newly_bound = true;
    } else if (owner != token) {
        publish_error(
            NativePortFrameQueueError::ConsumerThreadViolation, 0u);
        return false;
    }
    if (!serial_reference &&
        producer_thread_token_.load(std::memory_order_acquire) == token) {
        publish_error(NativePortFrameQueueError::ThreadDomainOverlap, 0u);
        return false;
    }
    if (newly_bound) notify_state_change();
    return true;
}

bool NativePortFrameQueue::is_producer_thread() const noexcept {
    const auto owner =
        producer_thread_token_.load(std::memory_order_acquire);
    return owner != 0u && owner == frame_queue_thread_token();
}

bool NativePortFrameQueue::is_consumer_thread() const noexcept {
    const auto owner =
        consumer_thread_token_.load(std::memory_order_acquire);
    return owner != 0u && owner == frame_queue_thread_token();
}

void NativePortFrameQueue::reject_foreign_write(
    const std::size_t arena_index,
    const std::uint64_t sequence) noexcept {
    if (arena_index < arenas_.size()) {
        auto& arena = arenas_[arena_index];
        if (arena.sequence == sequence &&
            arena.state.load(std::memory_order_acquire) == ArenaState::Writing)
            arena.state.store(ArenaState::Free, std::memory_order_release);
    }
    producer_lease_active_.store(false, std::memory_order_release);
    publish_error(NativePortFrameQueueError::ProducerThreadViolation,
                  sequence);
}

void NativePortFrameQueue::reject_foreign_read(
    const std::size_t arena_index,
    const std::uint64_t sequence) noexcept {
    if (arena_index < arenas_.size()) {
        auto& arena = arenas_[arena_index];
        if (arena.sequence == sequence &&
            arena.state.load(std::memory_order_acquire) == ArenaState::Reading)
            arena.state.store(ArenaState::Free, std::memory_order_release);
    }
    consumer_lease_active_.store(false, std::memory_order_release);
    publish_error(NativePortFrameQueueError::ConsumerThreadViolation,
                  sequence);
}

bool NativePortFrameQueue::has_producer_capacity() const noexcept {
    const auto producer = producer_position_.load(std::memory_order_acquire);
    const auto consumer = consumer_position_.load(std::memory_order_acquire);
    return producer >= consumer &&
           producer - consumer < native_port_frame_queue_depth;
}

bool NativePortFrameQueue::has_consumer_work() const noexcept {
    return consumer_position_.load(std::memory_order_acquire) <
           producer_position_.load(std::memory_order_acquire);
}

void NativePortFrameQueue::notify_state_change() noexcept {
    event_epoch_.fetch_add(1u, std::memory_order_release);
    event_epoch_.notify_all();
}

void NativePortFrameQueue::maybe_publish_stopped() noexcept {
    if (lifecycle_.load(std::memory_order_acquire) !=
            NativePortFrameQueueLifecycle::Draining ||
        producer_lease_active_.load(std::memory_order_acquire) ||
        consumer_lease_active_.load(std::memory_order_acquire) ||
        has_consumer_work())
        return;
    auto expected = NativePortFrameQueueLifecycle::Draining;
    if (lifecycle_.compare_exchange_strong(
            expected, NativePortFrameQueueLifecycle::Stopped,
            std::memory_order_acq_rel, std::memory_order_acquire))
        notify_state_change();
}

void NativePortFrameQueue::publish_error(
    const NativePortFrameQueueError error,
    const std::uint64_t sequence) noexcept {
    if (error == NativePortFrameQueueError::None ||
        lifecycle_.load(std::memory_order_acquire) ==
            NativePortFrameQueueLifecycle::Disabled)
        return;
    bool unclaimed = false;
    if (first_error_claimed_.compare_exchange_strong(
            unclaimed, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        first_error_ = error;
        first_error_sequence_ = sequence;
        lifecycle_.store(
            NativePortFrameQueueLifecycle::Failed,
            std::memory_order_release);
        first_error_ready_.store(true, std::memory_order_release);
        first_error_ready_.notify_all();
        notify_state_change();
        return;
    }
    while (!first_error_ready_.load(std::memory_order_acquire))
        first_error_ready_.wait(false, std::memory_order_acquire);
}

std::optional<NativePortFrameWriteLease>
NativePortFrameQueue::try_begin_produce() noexcept {
    if (lifecycle_.load(std::memory_order_acquire) !=
            NativePortFrameQueueLifecycle::Running ||
        !bind_producer_thread())
        return std::nullopt;
    bool inactive = false;
    if (!producer_lease_active_.compare_exchange_strong(
            inactive, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        publish_error(
            NativePortFrameQueueError::ProducerLeaseOverlap,
            producer_position_.load(std::memory_order_relaxed));
        return std::nullopt;
    }
    if (lifecycle_.load(std::memory_order_acquire) !=
        NativePortFrameQueueLifecycle::Running) {
        producer_lease_active_.store(false, std::memory_order_release);
        maybe_publish_stopped();
        return std::nullopt;
    }
    if (!has_producer_capacity()) {
        producer_lease_active_.store(false, std::memory_order_release);
        auto value = queue_full_rejections_.load(std::memory_order_relaxed);
        while (!queue_full_rejections_.compare_exchange_weak(
            value, saturating_increment(value), std::memory_order_relaxed,
            std::memory_order_relaxed)) {}
        return std::nullopt;
    }

    const auto position =
        producer_position_.load(std::memory_order_relaxed);
    if (position == std::numeric_limits<std::uint64_t>::max()) {
        producer_lease_active_.store(false, std::memory_order_release);
        publish_error(NativePortFrameQueueError::SequenceViolation, position);
        return std::nullopt;
    }
    const auto sequence = native_port_frame_queue_next_sequence(position);
    const auto arena_index = static_cast<std::size_t>(
        position % native_port_frame_queue_depth);
    auto& arena = arenas_[arena_index];
    auto expected = ArenaState::Free;
    if (!arena.state.compare_exchange_strong(
            expected, ArenaState::Writing,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        producer_lease_active_.store(false, std::memory_order_release);
        publish_error(
            NativePortFrameQueueError::SequenceViolation, sequence);
        return std::nullopt;
    }
    arena.command_count = 0u;
    arena.payload_size = 0u;
    arena.sequence = sequence;
    return NativePortFrameWriteLease(
        *this, arena_index, arena.sequence);
}

std::optional<NativePortFrameWriteLease>
NativePortFrameQueue::wait_begin_produce() noexcept {
    for (;;) {
        if (auto lease = try_begin_produce(); lease.has_value()) return lease;
        if (lifecycle_.load(std::memory_order_acquire) !=
            NativePortFrameQueueLifecycle::Running)
            return std::nullopt;
        const auto epoch = event_epoch_.load(std::memory_order_acquire);
        if (has_producer_capacity()) continue;
        event_epoch_.wait(epoch, std::memory_order_acquire);
    }
}

std::optional<NativePortFrameReadLease>
NativePortFrameQueue::try_begin_consume() noexcept {
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    if (lifecycle == NativePortFrameQueueLifecycle::Disabled ||
        lifecycle == NativePortFrameQueueLifecycle::Stopped ||
        lifecycle == NativePortFrameQueueLifecycle::Failed ||
        !bind_consumer_thread())
        return std::nullopt;
    bool inactive = false;
    if (!consumer_lease_active_.compare_exchange_strong(
            inactive, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        publish_error(
            NativePortFrameQueueError::ConsumerLeaseOverlap,
            consumer_position_.load(std::memory_order_relaxed));
        return std::nullopt;
    }
    const auto admitted_lifecycle =
        lifecycle_.load(std::memory_order_acquire);
    if (admitted_lifecycle == NativePortFrameQueueLifecycle::Disabled ||
        admitted_lifecycle == NativePortFrameQueueLifecycle::Stopped ||
        admitted_lifecycle == NativePortFrameQueueLifecycle::Failed) {
        consumer_lease_active_.store(false, std::memory_order_release);
        return std::nullopt;
    }
    if (!has_consumer_work()) {
        consumer_lease_active_.store(false, std::memory_order_release);
        maybe_publish_stopped();
        return std::nullopt;
    }

    const auto position =
        consumer_position_.load(std::memory_order_relaxed);
    const auto sequence = native_port_frame_queue_next_sequence(position);
    const auto arena_index = static_cast<std::size_t>(
        position % native_port_frame_queue_depth);
    auto& arena = arenas_[arena_index];
    auto expected = ArenaState::Ready;
    if (!arena.state.compare_exchange_strong(
            expected, ArenaState::Reading,
            std::memory_order_acq_rel, std::memory_order_acquire) ||
        sequence == 0u || arena.sequence != sequence ||
        arena.command_count > config_.maximum_commands_per_frame ||
        arena.payload_size > config_.maximum_payload_bytes_per_frame) {
        consumer_lease_active_.store(false, std::memory_order_release);
        publish_error(
            NativePortFrameQueueError::SequenceViolation, sequence);
        return std::nullopt;
    }
    for (std::uint32_t index = 0u; index < arena.command_count; ++index) {
        const auto& command = arena.commands[index];
        if (command.payload_offset > arena.payload_size ||
            command.payload_size > arena.payload_size - command.payload_offset) {
            consumer_lease_active_.store(false, std::memory_order_release);
            arena.state.store(ArenaState::Free, std::memory_order_release);
            publish_error(
                NativePortFrameQueueError::InvalidCommandRange,
                arena.sequence);
            return std::nullopt;
        }
    }
    return NativePortFrameReadLease(*this, arena_index, arena.sequence);
}

std::optional<NativePortFrameReadLease>
NativePortFrameQueue::wait_begin_consume() noexcept {
    for (;;) {
        if (auto lease = try_begin_consume(); lease.has_value()) return lease;
        const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
        if (lifecycle == NativePortFrameQueueLifecycle::Disabled ||
            lifecycle == NativePortFrameQueueLifecycle::Stopped ||
            lifecycle == NativePortFrameQueueLifecycle::Failed)
            return std::nullopt;
        const auto epoch = event_epoch_.load(std::memory_order_acquire);
        if (has_consumer_work()) continue;
        maybe_publish_stopped();
        if (lifecycle_.load(std::memory_order_acquire) ==
            NativePortFrameQueueLifecycle::Stopped)
            return std::nullopt;
        event_epoch_.wait(epoch, std::memory_order_acquire);
    }
}

std::optional<std::uint32_t> NativePortFrameQueue::append_payload(
    const std::size_t arena_index,
    const std::uint64_t sequence,
    const std::span<const std::byte> bytes,
    const std::uint32_t alignment) noexcept {
    if (arena_index >= arenas_.size()) return std::nullopt;
    auto& arena = arenas_[arena_index];
    if (!producer_lease_active_.load(std::memory_order_acquire) ||
        arena.state.load(std::memory_order_acquire) != ArenaState::Writing ||
        arena.sequence != sequence) {
        fail_write(
            arena_index, sequence, NativePortFrameQueueError::SequenceViolation);
        return std::nullopt;
    }
    if (!valid_alignment(alignment)) {
        fail_write(
            arena_index, sequence,
            NativePortFrameQueueError::InvalidPayloadAlignment);
        return std::nullopt;
    }
    const auto mask = alignment - 1u;
    if (arena.payload_size > std::numeric_limits<std::uint32_t>::max() - mask) {
        fail_write(
            arena_index, sequence,
            NativePortFrameQueueError::PayloadCapacityExceeded);
        return std::nullopt;
    }
    const auto aligned = (arena.payload_size + mask) & ~mask;
    if (aligned > config_.maximum_payload_bytes_per_frame ||
        bytes.size() >
            config_.maximum_payload_bytes_per_frame - aligned) {
        fail_write(
            arena_index, sequence,
            NativePortFrameQueueError::PayloadCapacityExceeded);
        return std::nullopt;
    }
    if (aligned > arena.payload_size)
        std::memset(arena.payload.get() + arena.payload_size, 0,
                    aligned - arena.payload_size);
    if (!bytes.empty())
        std::memcpy(arena.payload.get() + aligned,
                    bytes.data(), bytes.size());
    arena.payload_size = aligned + static_cast<std::uint32_t>(bytes.size());
    return aligned;
}

bool NativePortFrameQueue::append_command_reference(
    const std::size_t arena_index,
    const std::uint64_t sequence,
    const std::uint32_t kind,
    const std::uint32_t payload_offset,
    const std::uint32_t payload_size,
    const std::uint32_t flags) noexcept {
    if (arena_index >= arenas_.size()) return false;
    auto& arena = arenas_[arena_index];
    if (!producer_lease_active_.load(std::memory_order_acquire) ||
        arena.state.load(std::memory_order_acquire) != ArenaState::Writing ||
        arena.sequence != sequence) {
        fail_write(
            arena_index, sequence, NativePortFrameQueueError::SequenceViolation);
        return false;
    }
    if (arena.command_count >= config_.maximum_commands_per_frame) {
        fail_write(
            arena_index, sequence,
            NativePortFrameQueueError::CommandCapacityExceeded);
        return false;
    }
    if (payload_offset > arena.payload_size ||
        payload_size > arena.payload_size - payload_offset) {
        fail_write(
            arena_index, sequence,
            NativePortFrameQueueError::InvalidCommandRange);
        return false;
    }
    arena.commands[arena.command_count++] = {
        kind, payload_offset, payload_size, flags};
    return true;
}

bool NativePortFrameQueue::publish_write(
    const std::size_t arena_index,
    const std::uint64_t sequence) noexcept {
    if (arena_index >= arenas_.size()) return false;
    auto& arena = arenas_[arena_index];
    const auto position =
        producer_position_.load(std::memory_order_relaxed);
    const auto expected_sequence =
        native_port_frame_queue_next_sequence(position);
    if (!producer_lease_active_.load(std::memory_order_acquire) ||
        lifecycle_.load(std::memory_order_acquire) !=
            NativePortFrameQueueLifecycle::Running ||
        arena.state.load(std::memory_order_acquire) != ArenaState::Writing ||
        expected_sequence == 0u || arena.sequence != sequence ||
        sequence != expected_sequence) {
        abort_write(arena_index, sequence);
        return false;
    }
    arena.state.store(ArenaState::Ready, std::memory_order_release);
    producer_lease_active_.store(false, std::memory_order_release);
    producer_position_.store(expected_sequence, std::memory_order_release);
    notify_state_change();
    return true;
}

void NativePortFrameQueue::abort_write(
    const std::size_t arena_index,
    const std::uint64_t sequence) noexcept {
    if (arena_index >= arenas_.size()) return;
    auto& arena = arenas_[arena_index];
    if (arena.sequence == sequence &&
        arena.state.load(std::memory_order_acquire) == ArenaState::Writing)
        arena.state.store(ArenaState::Free, std::memory_order_release);
    producer_lease_active_.store(false, std::memory_order_release);
    maybe_publish_stopped();
    notify_state_change();
}

void NativePortFrameQueue::fail_write(
    const std::size_t arena_index,
    const std::uint64_t sequence,
    const NativePortFrameQueueError error) noexcept {
    if (arena_index < arenas_.size()) {
        auto& arena = arenas_[arena_index];
        if (arena.sequence == sequence &&
            arena.state.load(std::memory_order_acquire) == ArenaState::Writing)
            arena.state.store(ArenaState::Free, std::memory_order_release);
    }
    producer_lease_active_.store(false, std::memory_order_release);
    publish_error(error == NativePortFrameQueueError::None
                      ? NativePortFrameQueueError::SequenceViolation
                      : error,
                  sequence);
}

std::span<const NativePortFrameCommand> NativePortFrameQueue::commands(
    const std::size_t arena_index,
    const std::uint64_t sequence) noexcept {
    if (arena_index >= arenas_.size()) return {};
    const auto& arena = arenas_[arena_index];
    if (arena.sequence != sequence ||
        arena.state.load(std::memory_order_acquire) != ArenaState::Reading)
        return {};
    return {arena.commands.get(), arena.command_count};
}

std::span<const std::byte> NativePortFrameQueue::payload(
    const std::size_t arena_index,
    const std::uint64_t sequence) noexcept {
    if (arena_index >= arenas_.size()) return {};
    const auto& arena = arenas_[arena_index];
    if (arena.sequence != sequence ||
        arena.state.load(std::memory_order_acquire) != ArenaState::Reading)
        return {};
    return {arena.payload.get(), arena.payload_size};
}

bool NativePortFrameQueue::complete_read(
    const std::size_t arena_index,
    const std::uint64_t sequence) noexcept {
    if (arena_index >= arenas_.size()) return false;
    auto& arena = arenas_[arena_index];
    const auto position =
        consumer_position_.load(std::memory_order_relaxed);
    const auto expected_sequence =
        native_port_frame_queue_next_sequence(position);
    if (!consumer_lease_active_.load(std::memory_order_acquire) ||
        arena.state.load(std::memory_order_acquire) != ArenaState::Reading ||
        expected_sequence == 0u || arena.sequence != sequence ||
        sequence != expected_sequence) {
        fail_read(
            arena_index, sequence, NativePortFrameQueueError::SequenceViolation);
        return false;
    }
    arena.state.store(ArenaState::Free, std::memory_order_release);
    consumer_lease_active_.store(false, std::memory_order_release);
    consumer_position_.store(expected_sequence, std::memory_order_release);
    maybe_publish_stopped();
    notify_state_change();
    return true;
}

void NativePortFrameQueue::fail_read(
    const std::size_t arena_index,
    const std::uint64_t sequence,
    const NativePortFrameQueueError error) noexcept {
    if (arena_index < arenas_.size()) {
        auto& arena = arenas_[arena_index];
        if (arena.sequence == sequence &&
            arena.state.load(std::memory_order_acquire) == ArenaState::Reading)
            arena.state.store(ArenaState::Free, std::memory_order_release);
    }
    consumer_lease_active_.store(false, std::memory_order_release);
    publish_error(error == NativePortFrameQueueError::None
                      ? NativePortFrameQueueError::SequenceViolation
                      : error,
                  sequence);
}

void NativePortFrameQueue::request_shutdown() noexcept {
    auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    while (lifecycle == NativePortFrameQueueLifecycle::Running &&
           !lifecycle_.compare_exchange_weak(
               lifecycle, NativePortFrameQueueLifecycle::Draining,
               std::memory_order_acq_rel, std::memory_order_acquire)) {}
    if (lifecycle == NativePortFrameQueueLifecycle::Disabled ||
        lifecycle == NativePortFrameQueueLifecycle::Stopped ||
        lifecycle == NativePortFrameQueueLifecycle::Failed)
        return;
    maybe_publish_stopped();
    notify_state_change();
}

void NativePortFrameQueue::report_producer_error(
    const NativePortFrameQueueError error,
    const std::uint64_t sequence) noexcept {
    publish_error(error, sequence);
}

void NativePortFrameQueue::report_consumer_error(
    const NativePortFrameQueueError error,
    const std::uint64_t sequence) noexcept {
    publish_error(error, sequence);
}

NativePortFrameQueueSnapshot NativePortFrameQueue::snapshot() const noexcept {
    for (;;) {
        NativePortFrameQueueSnapshot result;
        const auto epoch = event_epoch_.load(std::memory_order_acquire);
        const auto full_rejections =
            queue_full_rejections_.load(std::memory_order_acquire);
        result.observation_epoch = epoch;
        result.lifecycle = lifecycle_.load(std::memory_order_acquire);
        const auto error_claimed =
            first_error_claimed_.load(std::memory_order_acquire);
        if (error_claimed &&
            !first_error_ready_.load(std::memory_order_acquire)) {
            first_error_ready_.wait(false, std::memory_order_acquire);
            continue;
        }
        if (error_claimed) {
            result.first_error = first_error_;
            result.first_error_sequence = first_error_sequence_;
            // The ready acquire pairs with the winning publisher after it has
            // already made Failed visible. Re-read so a snapshot can never
            // expose a completed error mailbox with a pre-error lifecycle.
            result.lifecycle = lifecycle_.load(std::memory_order_acquire);
        }
        result.producer_thread_identity =
            producer_thread_token_.load(std::memory_order_acquire);
        result.consumer_thread_identity =
            consumer_thread_token_.load(std::memory_order_acquire);
        result.consumer_queue_position =
            consumer_position_.load(std::memory_order_acquire);
        result.producer_queue_position =
            producer_position_.load(std::memory_order_acquire);
        result.completed_frames = result.consumer_queue_position;
        result.submitted_frames = result.producer_queue_position;
        result.next_consumer_sequence = native_port_frame_queue_next_sequence(
            result.consumer_queue_position);
        result.next_producer_sequence = native_port_frame_queue_next_sequence(
            result.producer_queue_position);
        result.queue_full_rejections = full_rejections;
        result.last_completed_sequence = result.consumer_queue_position;
        result.last_submitted_sequence = result.producer_queue_position;
        if (event_epoch_.load(std::memory_order_acquire) == epoch &&
            queue_full_rejections_.load(std::memory_order_acquire) ==
                full_rejections &&
            result.completed_frames <= result.submitted_frames)
            return result;
    }
}

} // namespace katana::runtime
