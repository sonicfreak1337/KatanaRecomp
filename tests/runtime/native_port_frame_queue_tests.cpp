#include "katana/runtime/native_port_frame_queue.hpp"
#include "native_port_graphics_command_stream.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using katana::runtime::NativePortFrameCommand;
using katana::runtime::NativePortFrameQueue;
using katana::runtime::NativePortFrameQueueConfig;
using katana::runtime::NativePortFrameQueueError;
using katana::runtime::NativePortFrameQueueLifecycle;
using katana::runtime::NativePortFrameReadLease;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] NativePortFrameQueueConfig test_config(
    const std::uint32_t command_capacity = 4u,
    const std::uint32_t payload_capacity = 256u) {
    NativePortFrameQueueConfig config;
    config.maximum_commands_per_frame = command_capacity;
    config.maximum_payload_bytes_per_frame = payload_capacity;
    config.enabled = true;
    return config;
}

struct TestPayload final {
    std::uint64_t sequence = 0u;
    std::uint64_t value = 0u;

    [[nodiscard]] bool operator==(const TestPayload&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<TestPayload>);
static_assert(std::is_standard_layout_v<TestPayload>);

struct alignas(64) AlignedPayload final {
    std::array<std::uint64_t, 8u> words{};
};

static_assert(std::is_trivially_copyable_v<AlignedPayload>);
static_assert(std::is_standard_layout_v<AlignedPayload>);

[[nodiscard]] std::optional<TestPayload> decode_payload(
    const NativePortFrameReadLease& lease,
    const NativePortFrameCommand& command) {
    const auto bytes = lease.command_payload(command);
    if (bytes.size() != sizeof(TestPayload)) return std::nullopt;
    TestPayload payload;
    std::memcpy(&payload, bytes.data(), sizeof(payload));
    return payload;
}

void require_failure(const NativePortFrameQueue& queue,
                     const NativePortFrameQueueError error,
                     const std::uint64_t sequence) {
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortFrameQueueLifecycle::Failed,
            "Queuefehler wurde nicht terminal publiziert.");
    require(snapshot.first_error == error,
            "Queuefehler verlor seinen typisierten Fehlercode.");
    require(snapshot.first_error_sequence == sequence,
            "Queuefehler verlor seine Frame-Sequenz.");
    require(snapshot.completed_frames <= snapshot.submitted_frames,
            "Fehlersnapshot verletzt completed <= submitted.");
}

void test_disabled_contract() {
    NativePortFrameQueueConfig config = test_config();
    config.enabled = false;
    NativePortFrameQueue queue(config);
    require(!queue.enabled(), "Explizit deaktivierte Queue ist aktiv.");
    require(queue.snapshot().lifecycle ==
                NativePortFrameQueueLifecycle::Disabled,
            "Explizit deaktivierte Queue verlor Disabled-Lifecycle.");
    require(queue.snapshot().producer_thread_identity == 0u &&
                queue.snapshot().consumer_thread_identity == 0u,
            "Deaktivierte Queue band eine Threaddomaene.");
    require(!queue.try_begin_produce().has_value() &&
                !queue.try_begin_consume().has_value(),
            "Deaktivierte Queue vergibt Leases.");
    queue.request_shutdown();
    require(queue.snapshot().lifecycle ==
                NativePortFrameQueueLifecycle::Disabled,
            "Shutdown veraendert den deaktivierten Referenzpfad.");
}

void test_fifo_backpressure_and_arena_fences() {
    NativePortFrameQueue queue(test_config());
    const auto unbound_snapshot = queue.snapshot();
    require(unbound_snapshot.producer_thread_identity == 0u &&
                unbound_snapshot.consumer_thread_identity == 0u &&
                unbound_snapshot.producer_queue_position == 0u &&
                unbound_snapshot.consumer_queue_position == 0u &&
                unbound_snapshot.next_producer_sequence == 1u &&
                unbound_snapshot.next_consumer_sequence == 1u,
            "Aktive Queue meldet vor dem ersten Lease eine Domainidentitaet.");
    const std::array serial_reference{
        TestPayload{1u, 0x1111u},
        TestPayload{2u, 0x2222u},
        TestPayload{3u, 0x3333u},
    };

    std::uint64_t bound_producer_identity = 0u;
    for (std::size_t index = 0u; index < 2u; ++index) {
        auto lease = queue.try_begin_produce();
        require(lease.has_value(), "Freie Queue verweigert Producer-Lease.");
        const auto bound_snapshot = queue.snapshot();
        require(bound_snapshot.producer_thread_identity != 0u &&
                    bound_snapshot.consumer_thread_identity == 0u &&
                    bound_snapshot.observation_epoch >
                        unbound_snapshot.observation_epoch,
                "Erfolgreicher Producer-Lease band seine Domain nicht.");
        if (index == 0u)
            bound_producer_identity =
                bound_snapshot.producer_thread_identity;
        else
            require(bound_snapshot.producer_thread_identity ==
                        bound_producer_identity,
                    "Producer-Domainidentitaet wechselte innerhalb der Queue.");
        require(lease->sequence() == serial_reference[index].sequence,
                "Producer-Sequenz ist nicht monoton.");
        require(lease->append_pod_command(
                    0x100u + static_cast<std::uint32_t>(index),
                    serial_reference[index],
                    0xA0u + static_cast<std::uint32_t>(index)) &&
                    lease->publish(),
                "Frame konnte nicht atomar publiziert werden.");
    }
    require(!queue.try_begin_produce().has_value(),
            "Depth-2-Queue akzeptiert einen dritten ungepufferten Frame.");

    std::array<TestPayload, 3u> observed{};
    std::array<const std::byte*, 3u> arena_addresses{};
    std::string consumer_failure;
    std::thread consumer([&] {
        for (std::size_t index = 0u; index < observed.size(); ++index) {
            auto lease = queue.wait_begin_consume();
            if (!lease.has_value()) {
                consumer_failure = "Consumer verlor einen publizierten Frame.";
                return;
            }
            const auto commands = lease->commands();
            if (commands.size() != 1u ||
                commands.front().kind !=
                    0x100u + static_cast<std::uint32_t>(index) ||
                commands.front().flags !=
                    0xA0u + static_cast<std::uint32_t>(index)) {
                consumer_failure = "Command-FIFO oder Metadaten sind falsch.";
                lease->fail(NativePortFrameQueueError::SequenceViolation);
                return;
            }
            const auto payload = decode_payload(*lease, commands.front());
            if (!payload.has_value()) {
                consumer_failure = "Offset-Payload ist unlesbar.";
                lease->fail(NativePortFrameQueueError::InvalidCommandRange);
                return;
            }
            observed[index] = *payload;
            arena_addresses[index] = lease->payload().data();
            if (!lease->complete()) {
                consumer_failure = "Completion-Fence wurde abgewiesen.";
                return;
            }
        }
        if (queue.wait_begin_consume().has_value())
            consumer_failure = "Shutdown lieferte einen Phantomframe.";
    });

    auto third = queue.wait_begin_produce();
    require(third.has_value() && third->sequence() == 3u,
            "Bounded Backpressure gab Arena 0 nicht nach Completion frei.");
    require(third->append_pod_command(
                0x102u, serial_reference[2], 0xA2u) && third->publish(),
            "Wiederverwendete Arena konnte Frame 3 nicht publizieren.");
    queue.request_shutdown();
    consumer.join();

    require(consumer_failure.empty(), consumer_failure);
    require(observed == serial_reference,
            "Queue-Reihenfolge weicht von der seriellen Referenz ab.");
    require(arena_addresses[0] != nullptr &&
                arena_addresses[0] != arena_addresses[1] &&
                arena_addresses[0] == arena_addresses[2],
            "Zwei owning Arenen wurden nicht erst nach Completion recycelt.");
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortFrameQueueLifecycle::Stopped &&
                snapshot.first_error == NativePortFrameQueueError::None &&
                snapshot.producer_thread_identity != 0u &&
                snapshot.consumer_thread_identity != 0u &&
                snapshot.producer_thread_identity !=
                    snapshot.consumer_thread_identity &&
                snapshot.submitted_frames == 3u &&
                snapshot.completed_frames == 3u &&
                snapshot.producer_queue_position == 3u &&
                snapshot.consumer_queue_position == 3u &&
                snapshot.next_producer_sequence == 4u &&
                snapshot.next_consumer_sequence == 4u &&
                snapshot.last_submitted_sequence == 3u &&
                snapshot.last_completed_sequence == 3u &&
                snapshot.queue_full_rejections >= 1u,
            "FIFO-/Backpressure-Snapshot ist inkonsistent.");
}

void test_rollback_isolation() {
    NativePortFrameQueue queue(test_config());
    {
        auto lease = queue.try_begin_produce();
        require(lease.has_value(), "Rollback-Fixture erhielt keine Lease.");
        const TestPayload discarded{1u, 0xDEADu};
        require(lease->append_pod_command(0x201u, discarded),
                "Rollback-Fixture konnte Payload nicht schreiben.");
        lease->abort();
    }
    const auto aborted_snapshot = queue.snapshot();
    require(aborted_snapshot.producer_queue_position == 0u &&
                aborted_snapshot.consumer_queue_position == 0u &&
                aborted_snapshot.next_producer_sequence == 1u &&
                aborted_snapshot.next_consumer_sequence == 1u,
            "Abort verschob die sichtbare Queue-Sequenz.");

    const TestPayload replacement{1u, 0xBEEFu};
    auto replacement_lease = queue.try_begin_produce();
    require(replacement_lease.has_value() &&
                replacement_lease->sequence() == 1u &&
                replacement_lease->append_pod_command(0x202u, replacement) &&
                replacement_lease->publish(),
            "Abort isolierte die nicht publizierten Bytes nicht.");

    std::optional<TestPayload> observed;
    std::thread consumer([&] {
        auto lease = queue.wait_begin_consume();
        if (!lease.has_value() || lease->commands().size() != 1u) return;
        observed = decode_payload(*lease, lease->commands().front());
        static_cast<void>(lease->complete());
    });
    consumer.join();
    queue.request_shutdown();

    require(observed.has_value() && *observed == replacement,
            "Consumer sah verworfene statt atomar publizierte Framedaten.");
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortFrameQueueLifecycle::Stopped &&
                snapshot.submitted_frames == 1u &&
                snapshot.completed_frames == 1u &&
                snapshot.producer_queue_position == 1u &&
                snapshot.consumer_queue_position == 1u &&
                snapshot.next_producer_sequence == 2u &&
                snapshot.next_consumer_sequence == 2u,
            "Abort wurde als sichtbarer Frame gezaehlt.");
}

void test_payload_base_alignment() {
    NativePortFrameQueue queue(test_config(1u, 128u));
    AlignedPayload payload;
    payload.words.front() = 0x12345678u;
    auto write = queue.try_begin_produce();
    require(write.has_value() &&
                write->append_pod_command(0x301u, payload) &&
                write->publish(),
            "64-Byte-Alignment-Fixture publizierte nicht.");

    bool aligned = false;
    std::thread consumer([&] {
        auto read = queue.wait_begin_consume();
        if (!read.has_value() || read->commands().size() != 1u) return;
        const auto bytes = read->command_payload(read->commands().front());
        aligned = bytes.size() == sizeof(AlignedPayload) &&
                  reinterpret_cast<std::uintptr_t>(bytes.data()) %
                          alignof(AlignedPayload) ==
                      0u;
        static_cast<void>(read->complete());
    });
    consumer.join();
    queue.request_shutdown();
    require(aligned,
            "Offset-Arena richtet zugesagte 64-Byte-Payload nicht aus.");
}

void test_capacity_and_range_failures() {
    {
        NativePortFrameQueue queue(test_config(1u, 8u));
        auto lease = queue.try_begin_produce();
        require(lease.has_value(), "Payload-Overflow-Fixture ohne Lease.");
        const std::array<std::byte, 9u> bytes{};
        require(!lease->append_payload(bytes).has_value(),
                "Payload-Overflow wurde akzeptiert.");
        require_failure(queue,
                        NativePortFrameQueueError::PayloadCapacityExceeded,
                        1u);
    }
    {
        NativePortFrameQueue queue(test_config(1u, 8u));
        auto lease = queue.try_begin_produce();
        require(lease.has_value() &&
                    lease->append_command_reference(1u, 0u, 0u) &&
                    !lease->append_command_reference(2u, 0u, 0u),
                "Command-Capacity wurde nicht fail-closed erzwungen.");
        require_failure(queue,
                        NativePortFrameQueueError::CommandCapacityExceeded,
                        1u);
    }
    {
        NativePortFrameQueue queue(test_config(1u, 8u));
        auto lease = queue.try_begin_produce();
        require(lease.has_value(), "Alignment-Fixture ohne Lease.");
        const std::array<std::byte, 1u> bytes{};
        require(!lease->append_payload(bytes, 3u).has_value(),
                "Nicht-potenzzweier Alignment wurde akzeptiert.");
        require_failure(queue,
                        NativePortFrameQueueError::InvalidPayloadAlignment,
                        1u);
    }
    {
        NativePortFrameQueue queue(test_config(1u, 8u));
        auto lease = queue.try_begin_produce();
        require(lease.has_value() &&
                    !lease->append_command_reference(1u, 1u, 0u),
                "Command ausserhalb der Payload wurde akzeptiert.");
        require_failure(queue,
                        NativePortFrameQueueError::InvalidCommandRange,
                        1u);
    }
}

void test_exception_and_abandonment_mailbox() {
    {
        NativePortFrameQueue queue(test_config());
        try {
            auto lease = queue.try_begin_produce();
            require(lease.has_value(), "Producer-Exception-Fixture ohne Lease.");
            throw std::runtime_error("producer");
        } catch (const std::runtime_error&) {
        }
        require_failure(
            queue, NativePortFrameQueueError::ProducerException, 1u);
    }
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        const TestPayload payload{1u, 1u};
        require(write.has_value() && write->append_pod_command(1u, payload) &&
                    write->publish(),
                "Consumer-Exception-Fixture publizierte nicht.");
        std::thread consumer([&] {
            try {
                auto lease = queue.wait_begin_consume();
                if (!lease.has_value()) return;
                throw std::runtime_error("consumer");
            } catch (const std::runtime_error&) {
            }
        });
        consumer.join();
        require_failure(
            queue, NativePortFrameQueueError::ConsumerException, 1u);
        require(queue.snapshot().completed_frames == 0u,
                "Consumer-Exception markierte einen Frame als abgeschlossen.");
    }
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        const TestPayload payload{1u, 1u};
        require(write.has_value() && write->append_pod_command(1u, payload) &&
                    write->publish(),
                "Abandonment-Fixture publizierte nicht.");
        std::thread consumer([&] {
            auto lease = queue.wait_begin_consume();
            static_cast<void>(lease);
        });
        consumer.join();
        require_failure(queue,
                        NativePortFrameQueueError::ConsumerLeaseAbandoned,
                        1u);
        require(queue.snapshot().completed_frames == 0u,
                "Abandoned Lease markierte einen Frame als abgeschlossen.");
    }
}

void test_shutdown_and_thread_ownership() {
    {
        NativePortFrameQueue queue(test_config());
        std::atomic<bool> entered{false};
        bool received_frame = true;
        std::thread consumer([&] {
            entered.store(true, std::memory_order_release);
            received_frame = queue.wait_begin_consume().has_value();
        });
        while (!entered.load(std::memory_order_acquire))
            std::this_thread::yield();
        queue.request_shutdown();
        consumer.join();
        require(!received_frame &&
                    queue.snapshot().lifecycle ==
                        NativePortFrameQueueLifecycle::Stopped,
                "Shutdown weckte leeren Consumer nicht deterministisch.");
    }
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        require(write.has_value(), "Shutdown-Rollback-Fixture ohne Lease.");
        queue.request_shutdown();
        require(!write->publish(),
                "Shutdown publizierte einen nur teilweise gebauten Frame.");
        const auto snapshot = queue.snapshot();
        require(snapshot.lifecycle == NativePortFrameQueueLifecycle::Stopped &&
                    snapshot.first_error == NativePortFrameQueueError::None &&
                    snapshot.submitted_frames == 0u,
                "Shutdown-Rollback ist nicht unsichtbar und fail-closed.");
    }
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        require(write.has_value(), "Ownership-Fixture ohne erste Lease.");
        const auto owner_identity =
            queue.snapshot().producer_thread_identity;
        require(owner_identity != 0u,
                "Producer-Owner erhielt keine Domainidentitaet.");
        write->abort();
        std::thread foreign_producer([&] {
            static_cast<void>(queue.try_begin_produce());
        });
        foreign_producer.join();
        require_failure(queue,
                        NativePortFrameQueueError::ProducerThreadViolation,
                        0u);
        const auto rejected_snapshot = queue.snapshot();
        require(rejected_snapshot.producer_thread_identity == owner_identity &&
                    rejected_snapshot.consumer_thread_identity == 0u,
                "ProducerThreadViolation schrieb die Ownerdomain um.");
    }
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        const TestPayload payload{1u, 1u};
        require(write.has_value() && write->append_pod_command(1u, payload) &&
                    write->publish(),
                "Consumer-Ownership-Fixture publizierte nicht.");
        std::thread first_consumer([&] {
            auto read = queue.wait_begin_consume();
            if (read.has_value()) static_cast<void>(read->complete());
        });
        first_consumer.join();
        const auto owner_snapshot = queue.snapshot();
        require(owner_snapshot.producer_thread_identity != 0u &&
                    owner_snapshot.consumer_thread_identity != 0u &&
                    owner_snapshot.producer_thread_identity !=
                        owner_snapshot.consumer_thread_identity,
                "Consumer-Ownerdomain wurde nicht disjunkt gebunden.");
        std::thread foreign_consumer([&] {
            static_cast<void>(queue.try_begin_consume());
        });
        foreign_consumer.join();
        require_failure(queue,
                        NativePortFrameQueueError::ConsumerThreadViolation,
                        0u);
        const auto rejected_snapshot = queue.snapshot();
        require(rejected_snapshot.producer_thread_identity ==
                    owner_snapshot.producer_thread_identity &&
                    rejected_snapshot.consumer_thread_identity ==
                        owner_snapshot.consumer_thread_identity,
                "ConsumerThreadViolation schrieb eine Ownerdomain um.");
    }
    {
        NativePortFrameQueue queue(test_config());
        auto first = queue.try_begin_produce();
        require(first.has_value() && !queue.try_begin_produce().has_value(),
                "Producer-Lease-Reentry wurde akzeptiert.");
        first->abort();
        require_failure(queue,
                        NativePortFrameQueueError::ProducerLeaseOverlap,
                        0u);
    }
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        const TestPayload payload{1u, 1u};
        require(write.has_value() && write->append_pod_command(1u, payload) &&
                    write->publish(),
                "Consumer-Reentry-Fixture publizierte nicht.");
        std::thread consumer([&] {
            auto first = queue.wait_begin_consume();
            if (!first.has_value()) return;
            static_cast<void>(queue.try_begin_consume());
            first->fail(NativePortFrameQueueError::ConsumerLeaseOverlap);
        });
        consumer.join();
        require_failure(queue,
                        NativePortFrameQueueError::ConsumerLeaseOverlap,
                        0u);
    }
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        require(write.has_value(), "Thread-Domain-Fixture ohne Producer.");
        const auto producer_identity =
            queue.snapshot().producer_thread_identity;
        write->abort();
        require(!queue.try_begin_consume().has_value(),
                "Gleicher Thread wurde Producer und Consumer.");
        require_failure(
            queue, NativePortFrameQueueError::ThreadDomainOverlap, 0u);
        const auto rejected_snapshot = queue.snapshot();
        require(rejected_snapshot.producer_thread_identity ==
                    producer_identity &&
                    rejected_snapshot.consumer_thread_identity == 0u,
                "ThreadDomainOverlap band dieselbe Domain als Consumer.");
    }
    {
        NativePortFrameQueue queue(test_config());
        queue.report_producer_error(
            NativePortFrameQueueError::ProducerException, 7u);
        queue.report_consumer_error(
            NativePortFrameQueueError::ConsumerException, 8u);
        require_failure(
            queue, NativePortFrameQueueError::ProducerException, 7u);
    }
}

void test_transported_lease_rejection_and_error_publication() {
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        require(write.has_value(),
                "Transportierte Producer-Lease wurde nicht vergeben.");
        const auto owner_snapshot = queue.snapshot();
        std::atomic<bool> rejected{false};
        std::thread foreign_writer(
            [lease = std::move(*write), &rejected]() mutable {
                const TestPayload payload{1u, 1u};
                rejected.store(
                    !lease.append_pod_command(1u, payload),
                    std::memory_order_release);
            });
        foreign_writer.join();
        require(rejected.load(std::memory_order_acquire),
                "Fremdthread schrieb durch eine transportierte Producer-Lease.");
        require_failure(queue,
                        NativePortFrameQueueError::ProducerThreadViolation,
                        1u);
        const auto rejected_snapshot = queue.snapshot();
        require(rejected_snapshot.producer_thread_identity ==
                    owner_snapshot.producer_thread_identity &&
                    rejected_snapshot.consumer_thread_identity == 0u &&
                    rejected_snapshot.submitted_frames == 0u,
                "Transportierte Producer-Lease schrieb Domain oder Position um.");
    }
    {
        NativePortFrameQueue queue(test_config());
        auto write = queue.try_begin_produce();
        const TestPayload payload{1u, 1u};
        require(write.has_value() && write->append_pod_command(1u, payload) &&
                    write->publish(),
                "Transportierte Consumer-Lease-Fixture publizierte nicht.");

        std::optional<NativePortFrameReadLease> transported;
        std::thread consumer([&] {
            transported = queue.wait_begin_consume();
        });
        consumer.join();
        require(transported.has_value(),
                "Consumer-Owner erhielt keine transportierbare Lease.");
        const auto owner_snapshot = queue.snapshot();
        require(owner_snapshot.producer_thread_identity != 0u &&
                    owner_snapshot.consumer_thread_identity != 0u &&
                    owner_snapshot.producer_thread_identity !=
                        owner_snapshot.consumer_thread_identity,
                "Consumer-Lease band keine disjunkte Ownerdomain.");
        require(transported->commands().empty(),
                "Fremdthread las durch eine transportierte Consumer-Lease.");
        require_failure(queue,
                        NativePortFrameQueueError::ConsumerThreadViolation,
                        1u);
        const auto rejected_snapshot = queue.snapshot();
        require(rejected_snapshot.producer_thread_identity ==
                    owner_snapshot.producer_thread_identity &&
                    rejected_snapshot.consumer_thread_identity ==
                        owner_snapshot.consumer_thread_identity &&
                    rejected_snapshot.completed_frames == 0u,
                "Transportierte Consumer-Lease schrieb Domain oder Position um.");
    }

    for (std::uint32_t iteration = 0u; iteration < 128u; ++iteration) {
        NativePortFrameQueue queue(test_config(1u, 1u));
        std::thread producer([&] {
            queue.report_producer_error(
                NativePortFrameQueueError::ProducerException, 7u);
        });
        std::thread consumer([&] {
            queue.report_consumer_error(
                NativePortFrameQueueError::ConsumerException, 8u);
        });
        producer.join();
        consumer.join();
        const auto snapshot = queue.snapshot();
        const auto producer_won =
            snapshot.first_error ==
                NativePortFrameQueueError::ProducerException &&
            snapshot.first_error_sequence == 7u;
        const auto consumer_won =
            snapshot.first_error ==
                NativePortFrameQueueError::ConsumerException &&
            snapshot.first_error_sequence == 8u;
        require(snapshot.lifecycle == NativePortFrameQueueLifecycle::Failed &&
                    (producer_won || consumer_won),
                "Konkurrierender First-Error wurde unfertig publiziert.");
    }
}

void test_stress_ordering() {
    constexpr std::uint64_t frame_count = 50'000u;
    NativePortFrameQueue queue(test_config(1u, sizeof(TestPayload)));
    std::string consumer_failure;
    std::thread consumer([&] {
        for (std::uint64_t sequence = 1u; sequence <= frame_count; ++sequence) {
            auto lease = queue.wait_begin_consume();
            if (!lease.has_value() || lease->sequence() != sequence ||
                lease->commands().size() != 1u) {
                consumer_failure = "Stress-FIFO verlor Frame oder Sequenz.";
                return;
            }
            const auto payload = decode_payload(
                *lease, lease->commands().front());
            if (!payload.has_value() || payload->sequence != sequence ||
                payload->value != ~sequence) {
                consumer_failure = "Stress-FIFO sah gerissene Payloaddaten.";
                lease->fail(NativePortFrameQueueError::SequenceViolation);
                return;
            }
            if (!lease->complete()) {
                consumer_failure = "Stress-FIFO verlor Completion-Fence.";
                return;
            }
        }
        if (queue.wait_begin_consume().has_value())
            consumer_failure = "Stress-Shutdown erzeugte Phantomframe.";
    });

    for (std::uint64_t sequence = 1u; sequence <= frame_count; ++sequence) {
        auto lease = queue.wait_begin_produce();
        require(lease.has_value() && lease->sequence() == sequence,
                "Stress-Producer verlor Backpressure-Sequenz.");
        const TestPayload payload{sequence, ~sequence};
        require(lease->append_pod_command(0x501u, payload) &&
                    lease->publish(),
                "Stress-Producer konnte Frame nicht publizieren.");
    }
    queue.request_shutdown();
    consumer.join();

    require(consumer_failure.empty(), consumer_failure);
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortFrameQueueLifecycle::Stopped &&
                snapshot.first_error == NativePortFrameQueueError::None &&
                snapshot.submitted_frames == frame_count &&
                snapshot.completed_frames == frame_count,
            "Stress-Snapshot verletzt FIFO-/Fence-Vertrag.");
}

// The command stream is intentionally tested at the queue boundary.  The
// test keeps the producer-owned source objects alive, mutates them only after
// publish, and lets a distinct consumer thread inspect the read-lease views.
void test_graphics_command_stream_roundtrip() {
    using namespace katana::runtime;
    using CommandKind = NativePortGraphicsCommandKind;

    NativePortTextureConfig texture_config;
    texture_config.extent = {2u, 2u};
    texture_config.format = NativePortTextureFormat::Rgba8Unorm;
    texture_config.mip_levels = 1u;
    std::array<std::byte, 16u> texture_pixels{};
    for (std::size_t index = 0u; index < texture_pixels.size(); ++index)
        texture_pixels[index] = static_cast<std::byte>(index + 1u);
    NativePortImageView texture_image;
    texture_image.extent = texture_config.extent;
    texture_image.format = texture_config.format;
    texture_image.stride_bytes = 8u;
    texture_image.pixels = texture_pixels;

    std::array<NativePortVertex, 3u> vertices{};
    vertices[0].position = {-1.0f, -1.0f, 0.0f};
    vertices[1].position = {1.0f, -1.0f, 0.0f};
    vertices[2].position = {0.0f, 1.0f, 0.0f};
    std::array<std::uint32_t, 3u> indices{0u, 1u, 2u};
    NativePortMeshConfig mesh_config;
    mesh_config.vertices = vertices;
    mesh_config.indices = indices;
    mesh_config.topology = NativePortPrimitiveTopology::TriangleList;
    mesh_config.shading = NativePortShadingMode::Smooth;

    NativePortDrawPacket draw_packet;
    draw_packet.vertices = vertices;
    draw_packet.indices = indices;
    draw_packet.batch.identity = 0x101u;
    draw_packet.batch.submission_order = 1u;
    draw_packet.batch.semantic = NativePortDrawBatchClass::Scene3D;
    draw_packet.topology = NativePortPrimitiveTopology::TriangleList;

    NativePortImageView present_image = texture_image;
    present_image.format = NativePortTextureFormat::Bgra8Unorm;
    std::array<std::byte, 16u> present_pixels{};
    for (std::size_t index = 0u; index < present_pixels.size(); ++index)
        present_pixels[index] = static_cast<std::byte>(0xC0u + index);
    present_image.pixels = present_pixels;

    NativePortFrameQueue queue(test_config(32u, 64u * 1024u));
    auto write = queue.try_begin_produce();
    require(write.has_value(), "Graphics-Codec erhielt keine Write-Lease.");
    NativePortGraphicsCommandWriter writer(*write);
    const NativePortTextureHandle texture_handle{0x1111u};
    const NativePortMeshHandle mesh_handle{0x2222u};
    require(writer.show() && writer.poll_events(),
            "Graphics-Codec konnte Basisbefehle nicht schreiben.");
    // Empty initial mips are a valid create operation (deferred upload).
    require(writer.create_texture(texture_handle, texture_config,
                                  std::span<const NativePortImageView>{}),
            "Graphics-Codec akzeptierte leere optionale Mips nicht.");
    require(writer.update_texture(texture_handle, texture_image) &&
                writer.destroy_texture(texture_handle) &&
                writer.create_mesh(mesh_handle, mesh_config) &&
                writer.destroy_mesh(mesh_handle) && writer.begin_frame() &&
                writer.draw(draw_packet) && writer.flush_type2_translucency() &&
                writer.present() && writer.repeat_present() &&
                writer.present_image(present_image) && writer.shutdown(),
            "Graphics-Codec konnte nicht jede Befehlsfamilie schreiben.");
    require(writer.publish(), "Graphics-Codec publizierte den Frame nicht.");

    // Prove that no producer pointer or source span escaped into the arena.
    texture_pixels[0] = static_cast<std::byte>(0xEEu);
    present_pixels[0] = static_cast<std::byte>(0xEFu);
    vertices[0].position[0] = 99.0f;
    indices[0] = 2u;
    queue.request_shutdown();

    std::string consumer_failure;
    std::thread consumer([&] {
        auto read = queue.wait_begin_consume();
        if (!read.has_value()) {
            consumer_failure = "Graphics-Codec verlor den publizierten Frame.";
            return;
        }
        NativePortGraphicsCommandReader reader(*read);
        if (!reader.valid() || reader.size() != 14u) {
            consumer_failure = "Graphics-Codec validierte den Roundtrip nicht.";
            read->fail(NativePortFrameQueueError::InvalidCommandRange);
            return;
        }
        static constexpr std::array expected_kinds{
            CommandKind::Show,
            CommandKind::PollEvents,
            CommandKind::CreateTexture,
            CommandKind::UpdateTexture,
            CommandKind::DestroyTexture,
            CommandKind::CreateMesh,
            CommandKind::DestroyMesh,
            CommandKind::BeginFrame,
            CommandKind::Draw,
            CommandKind::FlushType2,
            CommandKind::Present,
            CommandKind::RepeatPresent,
            CommandKind::PresentImage,
            CommandKind::Shutdown,
        };
        for (std::size_t ordinal = 0u; ordinal < expected_kinds.size();
             ++ordinal) {
            const auto command = reader.next();
            if (!command.has_value() || command->ordinal != ordinal ||
                command->kind != expected_kinds[ordinal]) {
                consumer_failure = "Graphics-Codec veraenderte FIFO/Ordinal.";
                read->fail(NativePortFrameQueueError::SequenceViolation);
                return;
            }
            switch (command->kind) {
            case CommandKind::CreateTexture: {
                const auto& value = std::get<NativePortGraphicsCreateTextureView>(
                    command->payload);
                if (value.texture != texture_handle ||
                    value.config.extent != texture_config.extent ||
                    value.initial_mip_levels.size() != 0u) {
                    consumer_failure = "CreateTexture-View ist nicht stabil.";
                    read->fail(NativePortFrameQueueError::InvalidCommandRange);
                    return;
                }
                break;
            }
            case CommandKind::UpdateTexture: {
                const auto& value = std::get<NativePortGraphicsUpdateTextureView>(
                    command->payload);
                const auto image = value.mip_levels.at(0u);
                if (value.texture != texture_handle || !image.has_value() ||
                    image->pixels.size() != texture_pixels.size() ||
                    image->pixels[0] != static_cast<std::byte>(1u)) {
                    consumer_failure = "UpdateTexture verlor den Deep-Copy.";
                    read->fail(NativePortFrameQueueError::InvalidCommandRange);
                    return;
                }
                break;
            }
            case CommandKind::DestroyTexture:
                if (std::get<NativePortGraphicsDestroyTextureView>(
                        command->payload)
                        .texture != texture_handle) {
                    consumer_failure = "DestroyTexture-Handle wurde veraendert.";
                    read->fail(NativePortFrameQueueError::InvalidCommandRange);
                    return;
                }
                break;
            case CommandKind::CreateMesh: {
                const auto& value = std::get<NativePortGraphicsCreateMeshView>(
                    command->payload);
                if (value.mesh != mesh_handle || value.vertices.size() != 3u ||
                    value.indices.size() != 3u ||
                    value.vertices[0].position[0] != -1.0f ||
                    value.indices[0] != 0u) {
                    consumer_failure = "CreateMesh verlor den Deep-Copy.";
                    read->fail(NativePortFrameQueueError::InvalidCommandRange);
                    return;
                }
                break;
            }
            case CommandKind::DestroyMesh:
                if (std::get<NativePortGraphicsDestroyMeshView>(command->payload)
                        .mesh != mesh_handle) {
                    consumer_failure = "DestroyMesh-Handle wurde veraendert.";
                    read->fail(NativePortFrameQueueError::InvalidCommandRange);
                    return;
                }
                break;
            case CommandKind::Draw: {
                const auto& value =
                    std::get<NativePortGraphicsDrawView>(command->payload);
                if (value.packet.vertices.size() != 3u ||
                    value.packet.indices.size() != 3u ||
                    value.packet.vertices[0].position[0] != -1.0f ||
                    value.packet.indices[0] != 0u ||
                    value.packet.batch.identity != 0x101u) {
                    consumer_failure = "Draw verlor Geometrie oder State.";
                    read->fail(NativePortFrameQueueError::InvalidCommandRange);
                    return;
                }
                break;
            }
            case CommandKind::PresentImage: {
                const auto& value = std::get<NativePortGraphicsPresentImageView>(
                    command->payload);
                if (value.image.pixels.size() != present_pixels.size() ||
                    value.image.pixels[0] != static_cast<std::byte>(0xC0u) ||
                    value.viewport != NativePortViewportTarget::Game ||
                    value.fit != NativePortImageFit::Contain) {
                    consumer_failure = "PresentImage verlor den Deep-Copy.";
                    read->fail(NativePortFrameQueueError::InvalidCommandRange);
                    return;
                }
                break;
            }
            default:
                break;
            }
        }
        if (reader.position() != expected_kinds.size() ||
            reader.next().has_value() || !read->complete())
            consumer_failure = "Graphics-Codec hat Ende oder Fence verletzt.";
    });
    consumer.join();
    require(consumer_failure.empty(), consumer_failure);
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortFrameQueueLifecycle::Stopped &&
                snapshot.submitted_frames == 1u &&
                snapshot.completed_frames == 1u,
            "Graphics-Codec Roundtrip beendete Queue nicht FIFO-sicher.");

    // Capacity failure is rejected before a command reference can become
    // visible.  The queue is terminal, and no partial frame is consumable.
    {
        NativePortFrameQueue small_queue(test_config(
            8u, static_cast<std::uint32_t>(sizeof(NativePortGraphicsDrawPayload) +
                                            8u)));
        auto small_write = small_queue.try_begin_produce();
        require(small_write.has_value(), "Capacity-Fixture erhielt keine Lease.");
        NativePortGraphicsCommandWriter small_writer(*small_write);
        require(!small_writer.draw(draw_packet) && !small_writer.publish() &&
                    small_writer.error() ==
                        NativePortGraphicsCommandEncodeError::CapacityExceeded,
                "Graphics-Codec verschluckte Arena-Capacity-Overflow.");
        const auto small_snapshot = small_queue.snapshot();
        require(small_snapshot.submitted_frames == 0u &&
                    !small_queue.try_begin_consume().has_value(),
                "Capacity-Overflow publizierte Partial-Commands.");
    }
}

void test_graphics_command_stream_fail_closed() {
    using namespace katana::runtime;
    NativePortTextureConfig config;
    config.extent = {1u, 1u};
    std::array<std::byte, 4u> pixels{std::byte{0x11u}, std::byte{0x22u},
                                     std::byte{0x33u}, std::byte{0x44u}};
    NativePortImageView image;
    image.extent = config.extent;
    image.format = config.format;
    image.stride_bytes = 4u;
    image.pixels = pixels;

    // Each mutation is applied only after queue publication.  The reader's
    // constructor must reject the complete frame, including any valid prefix.
    const auto malformed = [&](const std::uint32_t mutation) {
        NativePortFrameQueue queue(test_config(8u, 4096u));
        auto write = queue.try_begin_produce();
        require(write.has_value(), "Malformed-Codec-Fixture ohne Lease.");
        NativePortGraphicsCommandWriter writer(*write);
        require(writer.create_texture(NativePortTextureHandle{0x77u}, config,
                                       std::span<const NativePortImageView>(&image,
                                                                            1u)) &&
                    writer.publish(),
                "Malformed-Codec-Fixture konnte nicht publizieren.");
        queue.request_shutdown();
        std::string failure;
        std::thread consumer([&] {
            auto read = queue.wait_begin_consume();
            if (!read.has_value()) {
                failure = "Malformed-Codec-Frame wurde nicht gelesen.";
                return;
            }
            const auto commands = read->commands();
            const auto payload = read->payload();
            if (commands.size() != 1u || payload.empty()) {
                failure = "Malformed-Codec-Frame hatte falsche Grunddaten.";
                read->fail(NativePortFrameQueueError::InvalidCommandRange);
                return;
            }
            auto& command = const_cast<NativePortFrameCommand&>(commands[0]);
            auto* root = const_cast<NativePortGraphicsCreateTexturePayload*>(
                reinterpret_cast<const NativePortGraphicsCreateTexturePayload*>(
                    payload.data() + command.payload_offset));
            if (mutation == 0u)
                root->mip_count =
                    native_port_graphics_command_stream_max_mip_levels + 1u;
            else if (mutation == 1u)
                root->mip_descriptors_offset = root->header.byte_size - 1u;
            else
                root->mip_descriptors_offset += 1u;
            NativePortGraphicsCommandReader reader(*read);
            if (reader.valid() || reader.size() != 0u || reader.next().has_value())
                failure = "Malformed-Codec-Frame gab einen Teilpraefix preis.";
            if (!read->complete())
                failure = "Malformed-Codec-Frame verlor Completion-Fence.";
        });
        consumer.join();
        require(failure.empty(), failure);
    };
    malformed(0u); // invalid nested count
    malformed(1u); // invalid nested range
    malformed(2u); // invalid nested alignment

    // A malformed second command must hide the preceding valid command too.
    {
        NativePortFrameQueue queue(test_config(8u, 4096u));
        auto write = queue.try_begin_produce();
        require(write.has_value(), "Prefix-Fixture erhielt keine Lease.");
        NativePortGraphicsCommandWriter writer(*write);
        require(writer.show() &&
                    writer.create_texture(NativePortTextureHandle{0x88u}, config,
                                          std::span<const NativePortImageView>(
                                              &image, 1u)) &&
                    writer.publish(),
                "Prefix-Fixture publizierte nicht.");
        queue.request_shutdown();
        std::string failure;
        std::thread consumer([&] {
            auto read = queue.wait_begin_consume();
            if (!read.has_value()) {
                failure = "Prefix-Fixture wurde nicht gelesen.";
                return;
            }
            const auto commands = read->commands();
            auto& second = const_cast<NativePortFrameCommand&>(commands[1]);
            --second.payload_size;
            NativePortGraphicsCommandReader reader(*read);
            if (reader.valid() || reader.size() != 0u || reader.next().has_value())
                failure = "Reader lieferte gueltigen Prefix trotz Fehler.";
            static_cast<void>(read->complete());
        });
        consumer.join();
        require(failure.empty(), failure);
    }
}

// Semantic graphics contracts belong to the backend owner thread. The codec
// proves only that the POD/offset representation is complete and bounded, so
// structurally representable invalid input reaches the typed operation reply
// instead of becoming a terminal queue-decode failure.
void test_graphics_command_stream_defers_backend_contracts() {
    using namespace katana::runtime;

    NativePortTextureConfig invalid_texture;
    invalid_texture.extent = {};
    invalid_texture.format = static_cast<NativePortTextureFormat>(0xFFu);
    invalid_texture.mip_levels = 0u;

    NativePortMeshConfig invalid_mesh;
    invalid_mesh.topology =
        static_cast<NativePortPrimitiveTopology>(0xFFu);
    invalid_mesh.shading = static_cast<NativePortShadingMode>(0xFFu);
    invalid_mesh.small_triangle_area_threshold =
        std::numeric_limits<float>::quiet_NaN();

    NativePortFrameConfig invalid_frame;
    invalid_frame.clear_depth = std::numeric_limits<float>::quiet_NaN();

    NativePortDrawPacket invalid_draw;
    invalid_draw.topology =
        static_cast<NativePortPrimitiveTopology>(0xFFu);

    NativePortImageView invalid_image;
    invalid_image.format = static_cast<NativePortTextureFormat>(0xFFu);

    NativePortFrameQueue queue(test_config(8u, 64u * 1024u));
    auto write = queue.try_begin_produce();
    require(write.has_value(), "Semantic-Codec-Fixture erhielt keine Lease.");
    NativePortGraphicsCommandWriter writer(*write);
    require(writer.create_texture(
                {}, invalid_texture, std::span<const NativePortImageView>{}) &&
                writer.update_texture(
                    {}, std::span<const NativePortImageView>{}) &&
                writer.create_mesh({}, invalid_mesh) &&
                writer.begin_frame(invalid_frame) && writer.draw(invalid_draw) &&
                writer.present_image(
                    invalid_image,
                    static_cast<NativePortViewportTarget>(0xFFu),
                    static_cast<NativePortImageFit>(0xFFu)) &&
                writer.publish(),
            "Der Codec verwarf einen strukturell darstellbaren Backendvertrag.");
    queue.request_shutdown();

    std::string consumer_failure;
    std::thread consumer([&] {
        auto read = queue.wait_begin_consume();
        if (!read.has_value()) {
            consumer_failure = "Semantic-Codec-Fixture verlor den Frame.";
            return;
        }
        NativePortGraphicsCommandReader reader(*read);
        if (!reader.valid() || reader.size() != 6u) {
            consumer_failure =
                "Der Reader behandelte Backendsemantik als Wire-Korruption.";
            read->fail(NativePortFrameQueueError::InvalidCommandRange);
            return;
        }
        for (std::uint32_t ordinal = 0u; ordinal < 6u; ++ordinal) {
            const auto command = reader.next();
            if (!command.has_value() || command->ordinal != ordinal) {
                consumer_failure =
                    "Semantic-Codec-Fixture verlor Command-Ordinal/FIFO.";
                read->fail(NativePortFrameQueueError::SequenceViolation);
                return;
            }
        }
        if (reader.next().has_value() || !read->complete())
            consumer_failure =
                "Semantic-Codec-Fixture verlor Ende oder Completion-Fence.";
    });
    consumer.join();
    require(consumer_failure.empty(), consumer_failure);
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortFrameQueueLifecycle::Stopped &&
                snapshot.first_error == NativePortFrameQueueError::None &&
                snapshot.submitted_frames == 1u &&
                snapshot.completed_frames == 1u,
            "Backendsemantik setzte die Transportqueue terminal.");
}

} // namespace

int main(const int argc, char** const argv) {
    static_assert(katana::runtime::native_port_frame_queue_contract_version ==
                  2u);
    static_assert(katana::runtime::native_port_frame_queue_depth == 2u);
    static_assert(std::is_trivially_copyable_v<NativePortFrameCommand>);
    static_assert(std::is_standard_layout_v<NativePortFrameCommand>);
    static_assert(sizeof(NativePortFrameCommand) == 16u);
    static_assert(katana::runtime::native_port_frame_queue_next_sequence(
                      std::numeric_limits<std::uint64_t>::max() - 1u) ==
                  std::numeric_limits<std::uint64_t>::max());
    static_assert(katana::runtime::native_port_frame_queue_next_sequence(
                      std::numeric_limits<std::uint64_t>::max()) == 0u);

    require(katana::runtime::native_port_render_thread_kill_switch_disables(
                "1") &&
                !katana::runtime::native_port_render_thread_kill_switch_disables(
                    "0") &&
                !katana::runtime::native_port_render_thread_kill_switch_disables(
                    "true"),
            "Kill-Switch akzeptiert nicht exakt den Wert 1.");

    if (argc == 2 &&
        std::string_view(argv[1]) == "--kill-switch-disabled") {
        require(!katana::runtime::native_port_render_thread_enabled(),
                "Prozessweiter Renderthread-Kill-Switch blieb wirkungslos.");
        NativePortFrameQueue queue(test_config());
        require(!queue.enabled() &&
                    queue.snapshot().lifecycle ==
                        NativePortFrameQueueLifecycle::Disabled,
                "Kill-Switch allokierte/aktivierte die Queue.");
        auto serial_config = test_config();
        serial_config.threading_mode =
            NativePortFrameQueueConfig::ThreadingMode::SerialReference;
        NativePortFrameQueue serial_queue(serial_config);
        auto serial_write = serial_queue.try_begin_produce();
        require(serial_queue.enabled() && serial_write.has_value() &&
                    serial_write->append_pod_command(0xCAFEu,
                                                     TestPayload{1u, 2u}) &&
                    serial_write->publish(),
                "SerialReference konnte den gemeinsamen Codecpfad nicht "
                "publizieren.");
        auto serial_read = serial_queue.try_begin_consume();
        require(serial_read.has_value() &&
                    serial_read->commands().size() == 1u &&
                    serial_read->complete(),
                "SerialReference konnte denselben Frame nicht konsumieren.");
        const auto serial_snapshot = serial_queue.snapshot();
        require(serial_snapshot.producer_thread_identity != 0u &&
                    serial_snapshot.producer_thread_identity ==
                        serial_snapshot.consumer_thread_identity &&
                    serial_snapshot.submitted_frames == 1u &&
                    serial_snapshot.completed_frames == 1u,
                "SerialReference verlor Same-Domain/FIFO-Semantik.");
        serial_queue.request_shutdown();
        std::cout << "Native-Port-Frame-Queue Kill-Switch erfolgreich.\n";
        return EXIT_SUCCESS;
    }
    require(argc == 1, "Unbekannte Frame-Queue-Testoption.");
    require(katana::runtime::native_port_render_thread_enabled(),
            "Standardtest wurde mit deaktiviertem Renderthread gestartet.");

    test_disabled_contract();
    test_fifo_backpressure_and_arena_fences();
    test_rollback_isolation();
    test_payload_base_alignment();
    test_capacity_and_range_failures();
    test_exception_and_abandonment_mailbox();
    test_shutdown_and_thread_ownership();
    test_transported_lease_rejection_and_error_publication();
    test_stress_ordering();
    test_graphics_command_stream_roundtrip();
    test_graphics_command_stream_fail_closed();
    test_graphics_command_stream_defers_backend_contracts();

    std::cout << "Native-Port-Frame-Queue-Vertrag erfolgreich.\n";
    return EXIT_SUCCESS;
}
