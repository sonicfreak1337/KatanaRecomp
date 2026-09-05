#include "katana/runtime/native_port_audio_command_queue.hpp"
#include "katana/runtime/native_port_audio_engine.hpp"
#include "katana/runtime/native_port_movie.hpp"
#include "katana/runtime/native_port_sound_bank.hpp"
#include "katana/runtime/native_port_telemetry.hpp"

#include "../../src/runtime/native_port_audio_execution_domain.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

using namespace katana::runtime;

template <typename T>
concept HasTargetFeedFrames = requires(T value) { value.target_feed_frames; };

template <typename T>
concept HasMaximumRenderBlocksPerPump =
    requires(T value) { value.maximum_render_blocks_per_pump; };

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << std::endl;
        std::terminate();
    }
}

NativePortAudioCommandQueueConfig test_config(
    const std::uint32_t commands = 4u,
    const std::uint32_t payload_bytes = 64u,
    const std::uint32_t ack_slots = 4u) {
    NativePortAudioCommandQueueConfig config;
    config.maximum_commands = commands;
    config.maximum_payload_bytes = payload_bytes;
    config.maximum_ack_slots = ack_slots;
    config.enabled = true;
    return config;
}

NativePortAudioPodCommand command(
    const std::uint64_t frame,
    const std::uint64_t guest_sequence,
    const std::uint32_t payload_size = 0u,
    const std::uint32_t ack_slot =
        native_port_audio_command_queue_invalid_ack_slot,
    const NativePortAudioCommandTarget target =
        NativePortAudioCommandTarget::AudioEngine,
    const std::uint16_t opcode = 1u,
    const std::uint32_t target_slot = 0u,
    const std::uint32_t target_generation = 0u) {
    NativePortAudioPodCommand value;
    value.stamp = {frame, guest_sequence};
    value.target = target;
    value.opcode = opcode;
    value.payload_size = payload_size;
    value.ack_slot = ack_slot;
    value.target_slot = target_slot;
    value.target_generation = target_generation;
    return value;
}

std::uint64_t publish(
    NativePortAudioCommandQueue& queue,
    const NativePortAudioPodCommand& value,
    const std::span<const std::byte> payload = {}) {
    auto lease = queue.try_begin_produce(value);
    require(lease.has_value(), "Producer-Lease fehlt.");
    require(lease->copy_payload(payload), "Payload konnte nicht kopiert werden.");
    require(lease->command_sequence() == 0u,
            "Nicht publizierte Lease konsumiert eine Command-Sequenz.");
    require(lease->publish(), "Command konnte nicht publiziert werden.");
    return lease->command_sequence();
}

void test_pod_contract_and_defaults() {
    static_assert(native_port_audio_engine_contract_version == 7u);
    static_assert(native_port_sound_bank_contract_version == 9u);
    static_assert(native_port_sound_effect_kernel_contract_version == 1u);
    static_assert(std::is_standard_layout_v<NativePortSoundEffectKernel>);
    static_assert(std::is_trivially_copyable_v<NativePortSoundEffectKernel>);
    static_assert(std::is_standard_layout_v<NativePortSoundEffectKernelProvider>);
    static_assert(
        std::is_trivially_copyable_v<NativePortSoundEffectKernelProvider>);
    static_assert(!HasTargetFeedFrames<NativePortSoundBankConfig>);
    static_assert(
        !HasMaximumRenderBlocksPerPump<NativePortSoundBankConfig>);
    static_assert(std::is_standard_layout_v<NativePortAudioPodCommand>);
    static_assert(std::is_trivially_copyable_v<NativePortAudioPodCommand>);
    static_assert(std::is_standard_layout_v<NativePortAudioCommandAckResult>);
    static_assert(std::is_trivially_copyable_v<NativePortAudioCommandAckResult>);
    static_assert(std::is_standard_layout_v<NativePortAudioCommandAck>);
    static_assert(std::is_trivially_copyable_v<NativePortAudioCommandAck>);
    static_assert(native_port_audio_command_queue_max_ack_result_bytes == 512u);
    static_assert(static_cast<std::size_t>(
                      NativePortTelemetryStage::AudioDecode) == 6u);
    static_assert(static_cast<std::size_t>(
                      NativePortTelemetryStage::AudioMix) == 7u);
    static_assert(static_cast<std::size_t>(
                      NativePortTelemetryStage::GpuTime) == 8u);
    static_assert(static_cast<std::size_t>(
                      NativePortTelemetryStage::AudioServiceTotal) == 9u);
    static_assert(native_port_telemetry_stage_count == 10u);
    static_assert(native_port_telemetry_schema_version == 2u);

    NativePortAudioCommandQueue queue;
    require(queue.mode() == NativePortAudioCommandQueueMode::DedicatedThread,
            "Default-Queue ist nicht DedicatedThread.");
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortAudioCommandQueueLifecycle::Running &&
                snapshot.next_command_sequence == 1u &&
                snapshot.producer_thread_identity == 0u &&
                snapshot.consumer_thread_identity == 0u,
            "Default-Queue publiziert keinen ungebundenen Anfangszustand.");
    require(native_port_audio_serial_reference_requested("1") &&
                !native_port_audio_serial_reference_requested("0") &&
                !native_port_audio_serial_reference_requested("true"),
            "Serial-Reference-Predicate ist nicht exakt.");
}

void test_disabled_queue() {
    auto config = test_config();
    config.enabled = false;
    NativePortAudioCommandQueue queue(config);
    require(queue.snapshot().lifecycle ==
                NativePortAudioCommandQueueLifecycle::Disabled,
            "Deaktivierte Audio-Queue ist nicht Disabled.");
    require(queue.snapshot().producer_thread_identity == 0u &&
                queue.snapshot().consumer_thread_identity == 0u &&
                !queue.try_begin_produce(command(0u, 0u)).has_value() &&
                !queue.try_begin_consume().has_value(),
            "Deaktivierte Audio-Queue bindet Leases.");
    queue.request_shutdown();
    require(queue.snapshot().lifecycle ==
                NativePortAudioCommandQueueLifecycle::Disabled,
            "Shutdown veraendert Disabled nicht.");
}

void test_fifo_equal_stamp_payload_ack() {
    NativePortAudioCommandQueue queue(test_config());
    const std::array<std::array<std::byte, 3u>, 3u> payloads{
        std::array<std::byte, 3u>{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}},
        std::array<std::byte, 3u>{std::byte{0x20}, std::byte{0x21}, std::byte{0x22}},
        std::array<std::byte, 3u>{std::byte{0x30}, std::byte{0x31}, std::byte{0x32}},
    };
    std::array<std::uint64_t, 3u> sequences{};
    for (std::size_t index = 0u; index < sequences.size(); ++index) {
        sequences[index] = publish(
            queue,
            command(7u, static_cast<std::uint64_t>(index), 3u,
                    static_cast<std::uint32_t>(index),
                    index == 0u ? NativePortAudioCommandTarget::Movie
                                : NativePortAudioCommandTarget::AudioEngine,
                    1u,
                    static_cast<std::uint32_t>(index + 10u), 42u),
            payloads[index]);
        require(sequences[index] == index + 1u,
                "Interne Command-Sequenz ist nicht stabil monoton.");
    }

    for (std::size_t index = 0u; index < sequences.size(); ++index) {
        auto lease = queue.try_begin_consume();
        require(lease.has_value(), "Consumer-Lease fehlt im FIFO.");
        require(lease->command().command_sequence == sequences[index] &&
                    lease->command().target ==
                        (index == 0u ? NativePortAudioCommandTarget::Movie
                                     : NativePortAudioCommandTarget::AudioEngine) &&
                    lease->command().stamp == NativePortAudioCommandStamp{7u,
                                                                            index} &&
                    lease->command().target_slot == index + 10u &&
                    lease->command().target_generation == 42u &&
                    lease->payload().size() == payloads[index].size() &&
                    std::memcmp(lease->payload().data(), payloads[index].data(),
                                payloads[index].size()) == 0,
                "FIFO, Equal-Stamp oder Payload ist veraendert.");
        NativePortAudioCommandAckResult result;
        result.status = NativePortAudioCommandAckStatus::Completed;
        result.result_size = 2u;
        result.bytes[0] = std::byte{static_cast<unsigned char>(index)};
        result.bytes[1] = std::byte{0x7Fu};
        require(lease->complete(result), "Completion-Ack wurde abgewiesen.");
    }

    for (std::size_t index = 0u; index < sequences.size(); ++index) {
        const auto ack = queue.try_read_ack(
            static_cast<std::uint32_t>(index), sequences[index]);
        require(ack.has_value() &&
                    ack->status == NativePortAudioCommandAckStatus::Completed &&
                    ack->command_sequence == sequences[index] &&
                    ack->stamp == NativePortAudioCommandStamp{7u, index} &&
                    ack->result_size == 2u &&
                    ack->bytes[0] == std::byte{static_cast<unsigned char>(index)} &&
                    ack->bytes[1] == std::byte{0x7Fu},
                "Ack ist nicht vollstaendig vor dem Release publiziert.");
    }
    queue.request_shutdown();
    require(!queue.try_begin_consume().has_value() &&
                queue.snapshot().lifecycle ==
                    NativePortAudioCommandQueueLifecycle::Stopped,
            "FIFO-Queue drainte nicht deterministisch.");
    const auto snapshot = queue.snapshot();
    require(snapshot.submitted_commands == 3u &&
                snapshot.completed_commands == 3u &&
                snapshot.cancelled_commands == 0u &&
                snapshot.failed_commands == 0u &&
                snapshot.submitted_payload_bytes == 9u &&
                snapshot.completed_payload_bytes == 9u &&
                snapshot.queued_commands == 0u &&
                snapshot.queued_payload_bytes == 0u &&
                snapshot.published_acks == 3u && snapshot.consumed_acks == 3u,
            "FIFO-/Exact-once-Snapshot ist inkonsistent.");
}

void test_abort_does_not_consume_command_sequence() {
    NativePortAudioCommandQueue queue(test_config(2u, 16u, 2u));
    const std::array<std::byte, 2u> discarded{
        std::byte{0xA1}, std::byte{0xA2}};
    auto aborted = queue.try_begin_produce(command(0u, 0u, 2u));
    require(aborted.has_value() && aborted->command_sequence() == 0u &&
                aborted->copy_payload(discarded),
            "Abort-Fixture erhielt keine unpublizierte Lease.");
    aborted->abort();
    require(queue.snapshot().next_command_sequence == 1u &&
                queue.snapshot().submitted_commands == 0u,
            "Abort konsumierte die interne Command-Sequenz.");

    const std::array<std::byte, 2u> retained{
        std::byte{0xB1}, std::byte{0xB2}};
    auto replacement = queue.try_begin_produce(command(0u, 0u, 2u));
    require(replacement.has_value() && replacement->command_sequence() == 0u &&
                replacement->copy_payload(retained) && replacement->publish() &&
                replacement->command_sequence() == 1u,
            "Republish erhielt nach Abort nicht exakt Sequenz 1.");
    auto consumer = queue.try_begin_consume();
    require(consumer.has_value() && consumer->command().command_sequence == 1u &&
                consumer->payload()[0] == std::byte{0xB1} &&
                consumer->payload()[1] == std::byte{0xB2} && consumer->complete(),
            "Republish-Payload oder Sequenz ist nicht erhalten.");
    queue.request_shutdown();
    require(!queue.try_begin_consume().has_value() &&
                queue.snapshot().lifecycle ==
                    NativePortAudioCommandQueueLifecycle::Stopped,
            "Abort-Fixture drainte nicht deterministisch.");
}

void expect_stamp_failure(
    const NativePortAudioCommandQueueFailure expected,
    const NativePortAudioPodCommand first,
    const NativePortAudioPodCommand invalid) {
    NativePortAudioCommandQueue queue(test_config());
    publish(queue, first);
    auto first_lease = queue.try_begin_consume();
    require(first_lease.has_value() && first_lease->complete(),
            "Stamp-Fixture konnte ersten Command nicht abschliessen.");
    bool threw = false;
    try {
        static_cast<void>(queue.try_begin_produce(invalid));
    } catch (const NativePortAudioCommandQueueError& error) {
        threw = true;
        require(error.failure() == expected,
                "Stamp-Fehler hat den falschen Typ.");
    }
    require(threw, "Ungueltiger Stamp wurde nicht typisiert abgewiesen.");
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortAudioCommandQueueLifecycle::Failed &&
                snapshot.first_error == expected,
            "Stamp-Fehler wurde nicht terminal publiziert.");
}

void test_stamp_contract() {
    {
        NativePortAudioCommandQueue queue(test_config());
        bool threw = false;
        try {
            static_cast<void>(queue.try_begin_produce(command(0u, 1u)));
        } catch (const NativePortAudioCommandQueueError& error) {
            threw = true;
            require(error.failure() ==
                        NativePortAudioCommandQueueFailure::StampInitialSequence,
                    "Nicht-null Startsequenz ist nicht typisiert.");
        }
        require(threw, "Nicht-null Startsequenz wurde akzeptiert.");
    }
    expect_stamp_failure(NativePortAudioCommandQueueFailure::StampRegression,
                         command(4u, 0u), command(4u, 0u));
    expect_stamp_failure(NativePortAudioCommandQueueFailure::StampGap,
                         command(4u, 0u), command(4u, 2u));
    expect_stamp_failure(NativePortAudioCommandQueueFailure::FrameRegression,
                         command(4u, 0u), command(3u, 1u));
}

void test_payload_overflow_and_copy_contract() {
    NativePortAudioCommandQueue queue(test_config(2u, 4u, 2u));
    bool threw = false;
    try {
        static_cast<void>(queue.try_begin_produce(command(0u, 0u, 5u)));
    } catch (const NativePortAudioCommandQueueError& error) {
        threw = true;
        require(error.failure() ==
                    NativePortAudioCommandQueueFailure::PayloadOverflow,
                "Payload-Ueberlauf ist nicht typisiert.");
    }
    require(threw && queue.snapshot().lifecycle ==
                         NativePortAudioCommandQueueLifecycle::Failed,
            "Payload-Ueberlauf ist nicht terminal.");

    NativePortAudioCommandQueue valid_queue(test_config(2u, 4u, 2u));
    std::array<std::byte, 2u> bytes{std::byte{1u}, std::byte{2u}};
    auto lease = valid_queue.try_begin_produce(command(0u, 0u, 2u));
    require(lease.has_value() && lease->copy_payload(bytes),
            "Gueltige Payload wurde nicht akzeptiert.");
    std::array<std::byte, 1u> wrong_size{std::byte{3u}};
    require(!lease->copy_payload(wrong_size),
            "Falsche Payload-Groesse wurde akzeptiert.");
    require(lease->publish(), "Gueltige Payload konnte nicht publiziert werden.");
    auto consumer = valid_queue.try_begin_consume();
    require(consumer.has_value() && consumer->payload().size() == 2u &&
                consumer->payload()[0] == std::byte{1u} &&
                consumer->payload()[1] == std::byte{2u} && consumer->complete(),
            "Gueltige Payload ist beim Consumer veraendert.");
}

void test_payload_alignment_wrap_and_accounting() {
    NativePortAudioCommandQueue queue(test_config(4u, 33u, 4u));
    const std::array<std::byte, 3u> odd{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    struct alignas(std::max_align_t) TypedPayload final {
        std::array<std::int16_t, 4u> samples{};
        std::uint32_t marker = 0u;
    };
    static_assert(std::is_trivially_copyable_v<TypedPayload>);
    const TypedPayload typed{{-32768, -1, 0, 32767}, 0x77665544u};
    publish(queue, command(0u, 0u, static_cast<std::uint32_t>(odd.size())), odd);
    publish(queue,
            command(0u, 1u, static_cast<std::uint32_t>(sizeof(typed))),
            std::as_bytes(std::span(&typed, 1u)));

    auto first = queue.try_begin_consume();
    require(first.has_value() &&
                reinterpret_cast<std::uintptr_t>(first->payload().data()) %
                        native_port_audio_command_queue_payload_alignment ==
                    0u &&
                first->complete(),
            "Erstes Queuepayload ist nicht max_align_t-ausgerichtet.");
    auto second = queue.try_begin_consume();
    require(second.has_value() &&
                reinterpret_cast<std::uintptr_t>(second->payload().data()) %
                        native_port_audio_command_queue_payload_alignment ==
                    0u &&
                second->payload().size() == sizeof(TypedPayload) &&
                reinterpret_cast<std::uintptr_t>(second->payload().data()) %
                        alignof(TypedPayload) ==
                    0u &&
                reinterpret_cast<const TypedPayload*>(
                    second->payload().data())->samples == typed.samples &&
                reinterpret_cast<const TypedPayload*>(
                    second->payload().data())->marker == typed.marker &&
                second->complete(),
            "int16/POD-View verlor Alignment oder Inhalt nach Padding.");
    require(queue.snapshot().queued_payload_bytes == 0u,
            "Consumer zaehlte Alignment-Padding doppelt.");

    // A drained ring at a non-zero physical offset must still accept one
    // maximum-sized aligned payload by rebasing only the empty suffix.
    std::array<std::byte, 33u> full{};
    full.front() = std::byte{0xA5};
    full.back() = std::byte{0x5A};
    publish(queue, command(1u, 2u, static_cast<std::uint32_t>(full.size())),
            full);
    auto wrapped = queue.try_begin_consume();
    require(wrapped.has_value() &&
                reinterpret_cast<std::uintptr_t>(wrapped->payload().data()) %
                        native_port_audio_command_queue_payload_alignment ==
                    0u &&
                wrapped->payload().front() == std::byte{0xA5} &&
                wrapped->payload().back() == std::byte{0x5A} &&
                wrapped->complete() &&
                queue.snapshot().queued_payload_bytes == 0u,
            "Drained Ring konnte Maximalpayload nicht ausgerichtet rebasen.");
}

void test_terminal_worker_failure_and_cancel() {
    NativePortAudioCommandQueue queue(test_config());
    const auto sequence = publish(queue, command(0u, 0u, 0u, 0u));
    queue.fail_terminal(NativePortAudioCommandQueueFailure::WorkerFailure,
                        sequence);
    auto lease = queue.try_begin_consume();
    require(lease.has_value() && lease->cancelled(),
            "Terminal Queue verwirft den Cancel-Record oder markiert ihn nicht.");
    require(lease->complete(),
            "Terminal Cancel-Record wurde nicht exact einmal abgeschlossen.");
    const auto ack = queue.try_read_ack(0u, sequence);
    require(ack.has_value() &&
                ack->status == NativePortAudioCommandAckStatus::Cancelled,
            "Terminal WorkerFailure erzeugte keinen Cancel-Ack.");
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortAudioCommandQueueLifecycle::Failed &&
                snapshot.first_error ==
                    NativePortAudioCommandQueueFailure::WorkerFailure &&
                snapshot.cancelled_commands == 1u &&
                snapshot.published_acks == 1u && snapshot.consumed_acks == 1u,
            "WorkerFailure-Snapshot verletzt Exact-once.");

    NativePortAudioCommandQueue command_queue(test_config());
    const auto failed_sequence = publish(command_queue, command(0u, 0u, 0u, 0u));
    auto failed_lease = command_queue.try_begin_consume();
    require(failed_lease.has_value() && failed_lease->fail(0x44u),
            "Per-Command Failed-Ack wurde abgewiesen.");
    const auto failed_ack = command_queue.try_read_ack(0u, failed_sequence);
    require(failed_ack.has_value() &&
                failed_ack->status == NativePortAudioCommandAckStatus::Failed &&
                failed_ack->error_code == 0x44u &&
                command_queue.snapshot().failed_commands == 1u,
            "Per-Command Failed-Ack ist nicht gebunden.");
}

void test_first_error_publication_gate() {
    NativePortAudioCommandQueue queue(test_config(2u, 16u, 2u));
    auto producer = queue.try_begin_produce(command(0u, 0u));
    require(producer.has_value(), "Error-Gate-Fixture erhielt keine Producer-Lease.");

    std::atomic<bool> start{false};
    std::atomic<unsigned> completed_threads{0u};
    std::atomic<bool> foreign_producer_rejected{false};
    std::thread worker([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while (completed_threads.load(std::memory_order_acquire) == 0u)
            std::this_thread::yield();
        queue.fail_terminal(NativePortAudioCommandQueueFailure::WorkerFailure,
                            77u);
        completed_threads.fetch_add(1u, std::memory_order_release);
    });
    std::thread foreign_producer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            static_cast<void>(
                queue.try_begin_produce(command(0u, 1u)));
        } catch (const NativePortAudioCommandQueueError& error) {
            foreign_producer_rejected.store(
                error.failure() ==
                    NativePortAudioCommandQueueFailure::ProducerThreadViolation,
                std::memory_order_release);
        }
        completed_threads.fetch_add(1u, std::memory_order_release);
    });
    start.store(true, std::memory_order_release);
    bool observed_terminal = false;
    while (completed_threads.load(std::memory_order_acquire) != 2u) {
        const auto snapshot = queue.snapshot();
        if (snapshot.lifecycle == NativePortAudioCommandQueueLifecycle::Failed) {
            observed_terminal = true;
            require(snapshot.first_error !=
                        NativePortAudioCommandQueueFailure::None,
                    "Failed-Lifecycle war vor First-Error-Payload sichtbar.");
        }
        std::this_thread::yield();
    }
    worker.join();
    foreign_producer.join();
    producer->abort();
    const auto snapshot = queue.snapshot();
    require(observed_terminal && foreign_producer_rejected.load(std::memory_order_acquire) &&
                snapshot.lifecycle == NativePortAudioCommandQueueLifecycle::Failed &&
                snapshot.first_error != NativePortAudioCommandQueueFailure::None,
            "Concurrent First-Error-Gate oder Producer-Domain ist inkonsistent.");
}

void test_backpressure_and_thread_domains() {
    NativePortAudioCommandQueue queue(test_config(1u, 8u, 1u));
    const std::array<std::byte, 1u> first_payload{std::byte{0x01}};
    publish(queue, command(0u, 0u, 1u), first_payload);
    std::atomic<bool> consumer_ok{true};
    std::thread consumer([&] {
        try {
            auto first = queue.wait_begin_consume();
            if (!first.has_value() || first->payload().size() != 1u ||
                !first->complete()) {
                consumer_ok.store(false, std::memory_order_release);
                return;
            }
            auto second = queue.wait_begin_consume();
            if (!second.has_value() ||
                second->command().stamp.guest_sequence != 1u ||
                !second->complete()) {
                consumer_ok.store(false, std::memory_order_release);
                return;
            }
            queue.request_shutdown();
            if (queue.wait_begin_consume().has_value())
                consumer_ok.store(false, std::memory_order_release);
        } catch (...) {
            consumer_ok.store(false, std::memory_order_release);
        }
    });
    std::optional<NativePortAudioCommandProducerLease> second;
    try {
        second = queue.wait_begin_produce(command(0u, 1u, 0u));
    } catch (const NativePortAudioCommandQueueError& error) {
        std::cerr << "backpressure producer failure="
                  << static_cast<int>(error.failure()) << '\n';
        consumer_ok.store(false, std::memory_order_release);
    }
    if (second.has_value()) {
        const auto published = second->publish();
        require(published, "Backpressure gab den Producer-Lease nicht frei.");
    } else
        queue.request_shutdown();
    consumer.join();
    require(consumer_ok.load(std::memory_order_acquire),
            "Backpressure-Consumer verlor FIFO/Drain.");
    const auto snapshot = queue.snapshot();
    require(snapshot.producer_thread_identity != 0u &&
                snapshot.consumer_thread_identity != 0u &&
                snapshot.producer_thread_identity !=
                    snapshot.consumer_thread_identity &&
                snapshot.queue_full_waits >= 1u &&
                snapshot.submitted_commands == 2u &&
                snapshot.completed_commands == 2u,
            "Backpressure-/Thread-Snapshot ist falsch.");

    NativePortAudioCommandQueue producer_queue(test_config());
    publish(producer_queue, command(0u, 0u));
    std::atomic<bool> producer_violation{false};
    std::thread foreign_producer([&] {
        try {
            static_cast<void>(producer_queue.try_begin_produce(command(0u, 1u)));
        } catch (const NativePortAudioCommandQueueError& error) {
            producer_violation.store(
                error.failure() ==
                    NativePortAudioCommandQueueFailure::ProducerThreadViolation,
                std::memory_order_release);
        }
    });
    foreign_producer.join();
    auto cancelled = producer_queue.try_begin_consume();
    require(producer_violation.load(std::memory_order_acquire) &&
                cancelled.has_value() && cancelled->cancel(0x11u) &&
                producer_queue.snapshot().first_error ==
                    NativePortAudioCommandQueueFailure::ProducerThreadViolation,
            "Producer-Thread-Gate ist nicht fail-closed.");

    NativePortAudioCommandQueue consumer_queue(test_config());
    publish(consumer_queue, command(0u, 0u));
    auto owner_lease = consumer_queue.try_begin_consume();
    require(owner_lease.has_value(), "Consumer-Owner wurde nicht gebunden.");
    std::atomic<bool> consumer_violation{false};
    std::thread foreign_consumer([&] {
        try {
            static_cast<void>(consumer_queue.try_begin_consume());
        } catch (const NativePortAudioCommandQueueError& error) {
            consumer_violation.store(
                error.failure() ==
                    NativePortAudioCommandQueueFailure::ConsumerThreadViolation,
                std::memory_order_release);
        }
    });
    foreign_consumer.join();
    require(consumer_violation.load(std::memory_order_acquire) &&
                owner_lease->cancel(0x22u) &&
                consumer_queue.snapshot().first_error ==
                    NativePortAudioCommandQueueFailure::ConsumerThreadViolation,
            "Consumer-Thread-Gate ist nicht fail-closed.");
}

void test_abandon_and_serial_mode() {
    auto config = test_config();
    config.mode = NativePortAudioCommandQueueMode::SerialReference;
    NativePortAudioCommandQueue queue(config);
    publish(queue, command(0u, 0u));
    {
        auto lease = queue.try_begin_consume();
        require(lease.has_value(), "Abandon-Fixture erhielt keine Lease.");
    }
    const auto snapshot = queue.snapshot();
    require(snapshot.lifecycle == NativePortAudioCommandQueueLifecycle::Failed &&
                snapshot.first_error ==
                    NativePortAudioCommandQueueFailure::ConsumerLeaseAbandoned &&
                snapshot.failed_commands == 1u,
            "Abandonment wurde nicht terminal/fail-closed verbucht.");
    require(queue.mode() == NativePortAudioCommandQueueMode::SerialReference,
            "SerialReference-Modus wurde nicht als Konfiguration erhalten.");
}

struct DomainTargetFixture final {
    NativePortAudioExecutionDomain* domain = nullptr;
    NativePortTelemetry* telemetry_to_unbind = nullptr;
    std::unique_ptr<std::uint32_t> core;
    std::uint32_t executions = 0u;
    std::uint32_t cleanups = 0u;
    bool all_executions_on_consumer = true;
    bool telemetry_unbind_result = false;

    static void execute(
        void* const object,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        NativePortAudioCommandAckResult& result) noexcept {
        auto& self = *static_cast<DomainTargetFixture*>(object);
        ++self.executions;
        self.all_executions_on_consumer =
            self.all_executions_on_consumer && self.domain != nullptr &&
            self.domain->on_audio_thread();
        result = {};
        if (!payload.empty()) {
            result.status = NativePortAudioCommandAckStatus::Failed;
            result.error_code = 1u;
            return;
        }
        switch (opcode) {
        case 1u:
            if (self.core != nullptr) {
                result.status = NativePortAudioCommandAckStatus::Failed;
                result.error_code = 2u;
                return;
            }
            self.core = std::make_unique<std::uint32_t>(0xA11D10u);
            break;
        case 2u:
            if (self.core == nullptr) {
                result.status = NativePortAudioCommandAckStatus::Failed;
                result.error_code = 3u;
                return;
            }
            ++*self.core;
            break;
        case 3u:
            self.core.reset();
            break;
        case 4u:
            self.telemetry_unbind_result =
                self.domain != nullptr &&
                self.domain->unbind_telemetry(self.telemetry_to_unbind);
            if (!self.telemetry_unbind_result) {
                result.status = NativePortAudioCommandAckStatus::Failed;
                result.error_code = 5u;
                return;
            }
            break;
        default:
            result.status = NativePortAudioCommandAckStatus::Failed;
            result.error_code = 4u;
            return;
        }
        result.status = NativePortAudioCommandAckStatus::Completed;
    }

    static void cleanup(void* const object) noexcept {
        auto& self = *static_cast<DomainTargetFixture*>(object);
        ++self.cleanups;
        self.core.reset();
    }
};

struct CompletionServiceFixture final {
    NativePortAudioExecutionDomain* domain = nullptr;
    std::mutex mutex;
    std::condition_variable event;
    bool service_after_control = false;
    bool service_on_consumer = false;
    std::uint32_t pending_blocks = 0u;
    std::uint32_t drained_blocks = 0u;
    std::uint32_t controls = 0u;
    std::uint32_t services = 0u;

    static void execute(
        void* const object,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        NativePortAudioCommandAckResult& result) noexcept {
        auto& self = *static_cast<CompletionServiceFixture*>(object);
        result = {};
        if (!payload.empty()) {
            result.status = NativePortAudioCommandAckStatus::Failed;
            result.error_code = 1u;
            return;
        }
        if (opcode == 1u) {
            std::lock_guard lock(self.mutex);
            self.pending_blocks = 8u;
        } else if (opcode == 2u) {
            std::lock_guard lock(self.mutex);
            ++self.controls;
        } else {
            result.status = NativePortAudioCommandAckStatus::Failed;
            result.error_code = 2u;
            return;
        }
        result.status = NativePortAudioCommandAckStatus::Completed;
    }

    static std::uint32_t service(void* const object) noexcept {
        auto& self = *static_cast<CompletionServiceFixture*>(object);
        std::lock_guard lock(self.mutex);
        self.service_after_control = self.controls == 1u;
        self.service_on_consumer =
            self.domain != nullptr && self.domain->on_audio_thread();
        self.drained_blocks += self.pending_blocks;
        self.pending_blocks = 0u;
        ++self.services;
        self.event.notify_all();
        return 0u;
    }
};

void test_completion_wake_drains_pending_without_new_command() {
    NativePortAudioExecutionDomainConfig config;
    config.command_queue = test_config(8u, 64u, 4u);
    config.command_queue.mode =
        NativePortAudioCommandQueueMode::DedicatedThread;
    auto domain = NativePortAudioExecutionDomain::acquire(config);
    CompletionServiceFixture fixture;
    fixture.domain = domain.get();
    const auto handle = domain->register_target(
        NativePortAudioExecutionDomainTarget::HostOutput, &fixture,
        &CompletionServiceFixture::execute, nullptr,
        &CompletionServiceFixture::service);
    require(handle.has_value(),
            "Completion-Service-Fixture konnte Ziel nicht registrieren.");

    const auto burst = domain->dispatch_async(*handle, 1u, {}, 0u);
    require(burst.accepted() && burst.command_sequence == 1u,
            "Endlicher Pending-Burst wurde nicht publiziert.");
    const auto control = domain->dispatch_async(*handle, 2u, {}, 0u);
    require(control.accepted() && control.command_sequence == 2u,
            "Control hinter Pending-Burst wurde nicht publiziert.");
    domain->request_consumer_service();
    {
        std::unique_lock lock(fixture.mutex);
        require(fixture.event.wait_for(
                    lock, std::chrono::seconds(2),
                    [&fixture] { return fixture.services == 1u; }),
                "Completion-Wake servicierte Pending ohne Folgecommand nicht.");
        require(fixture.pending_blocks == 0u &&
                    fixture.drained_blocks == 8u &&
                    fixture.controls == 1u &&
                    fixture.service_after_control &&
                    fixture.service_on_consumer,
                "Completion-Retry verlor Tail, blockierte Control oder lief fremd.");
    }
    const auto snapshot = domain->snapshot();
    require(snapshot.queue.submitted_commands == 2u &&
                snapshot.queue.completed_commands == 2u &&
                snapshot.next_guest_sequence == 2u &&
                domain->unregister_target(*handle, &fixture),
            "Completion-Wake konsumierte eine Sequenz oder verlor Lifecycle.");
    domain->shutdown();
}

void test_resultless_async_controls_preserve_sync_fence_order() {
    NativePortAudioExecutionDomainConfig config;
    config.command_queue = test_config(8u, 64u, 4u);
    config.command_queue.mode =
        NativePortAudioCommandQueueMode::SerialReference;
    auto domain = NativePortAudioExecutionDomain::acquire(config);
    DomainTargetFixture fixture;
    fixture.domain = domain.get();
    const auto handle = domain->register_target(
        NativePortAudioExecutionDomainTarget::AudioEngine, &fixture,
        &DomainTargetFixture::execute, &DomainTargetFixture::cleanup);
    require(handle.has_value() &&
                domain->dispatch_sync(*handle, 1u, {}, 5u).completed(),
            "Async-Control-Fixture konnte nicht konstruiert werden.");

    const auto first = domain->dispatch_async(*handle, 2u, {}, 5u);
    const auto second = domain->dispatch_async(*handle, 2u, {}, 5u);
    require(first.accepted() && second.accepted() &&
                first.command_sequence == 2u &&
                second.command_sequence == 3u && fixture.executions == 1u,
            "Resultlose Controls blockierten oder liefen vor dem Consumer.");

    // A later synchronous lifecycle/result boundary is the ordering fence for
    // every already accepted resultless control on the same domain queue.
    const auto fence = domain->dispatch_sync(*handle, 2u, {}, 5u);
    const auto snapshot = domain->snapshot();
    require(fence.completed() && fence.command_sequence == 4u &&
                fence.stamp == NativePortAudioCommandStamp{5u, 3u} &&
                fixture.executions == 4u && fixture.core != nullptr &&
                *fixture.core == 0xA11D13u &&
                fixture.all_executions_on_consumer &&
                snapshot.queue.submitted_commands == 4u &&
                snapshot.queue.completed_commands == 4u &&
                snapshot.queue.failed_commands == 0u &&
                snapshot.next_guest_sequence == 4u,
            "Sync-Fence verlor Reihenfolge oder Completion der Async-Controls.");

    require(domain->dispatch_sync(*handle, 3u, {}, 6u).completed() &&
                domain->unregister_target(*handle, &fixture),
            "Async-Control-Fixture konnte nicht synchron retiren.");
    domain->shutdown();
}

void test_serial_domain_top_level_and_cleanup() {
    NativePortAudioExecutionDomainConfig config;
    config.command_queue = test_config(8u, 64u, 4u);
    config.command_queue.mode =
        NativePortAudioCommandQueueMode::SerialReference;
    auto domain = NativePortAudioExecutionDomain::acquire(config);

    DomainTargetFixture first;
    first.domain = domain.get();
    const auto first_handle = domain->register_target(
        NativePortAudioExecutionDomainTarget::AudioEngine, &first,
        &DomainTargetFixture::execute, &DomainTargetFixture::cleanup);
    require(first_handle.has_value(),
            "Serial-Domain konnte das erste Ziel nicht registrieren.");

    const auto constructed = domain->dispatch_sync(
        *first_handle, 1u, {}, 7u);
    require(constructed.completed() && !constructed.inline_execution &&
                constructed.command_sequence == 1u &&
                constructed.stamp == NativePortAudioCommandStamp{7u, 0u} &&
                !domain->on_audio_thread(),
            "Serial Construct war kein geordnetes Top-Level-Kommando.");
    const auto second = domain->dispatch_sync(
        *first_handle, 2u, {}, 7u,
        0u, NativePortAudioExecutionDomainStage::AudioMix);
    require(second.completed() && !second.inline_execution &&
                second.command_sequence == 2u &&
                second.stamp == NativePortAudioCommandStamp{7u, 1u} &&
                first.executions == 2u &&
                first.all_executions_on_consumer && !domain->on_audio_thread(),
            "Serial-Folgekommando lief ausserhalb der Queue oder inline.");

    const auto destroyed = domain->dispatch_sync(*first_handle, 3u, {}, 8u);
    require(destroyed.completed() &&
                domain->unregister_target(*first_handle, &first) &&
                first.cleanups == 0u && first.core == nullptr,
            "Normaler Destroy hat Cleanup nicht sauber disarmt.");

    DomainTargetFixture terminal;
    terminal.domain = domain.get();
    const auto terminal_handle = domain->register_target(
        NativePortAudioExecutionDomainTarget::AudioEngine, &terminal,
        &DomainTargetFixture::execute, &DomainTargetFixture::cleanup);
    require(terminal_handle.has_value() &&
                terminal_handle->slot == first_handle->slot &&
                terminal_handle->generation > first_handle->generation,
            "Freier Audio-Slot erhielt keine strikt neue Generation.");
    const auto reused_before = domain->snapshot();
    const auto& reused_target =
        reused_before.targets[terminal_handle->slot];
    require(!domain->unregister_target(*first_handle, &first) &&
                reused_target.registered &&
                reused_target.target_generation ==
                    terminal_handle->generation &&
                reused_target.active_dispatches == 0u,
            "Ein stale Handle beanspruchte die wiedereroeffnete Generation "
            "oder der Closed-Bit-Zaehler leakte in den Snapshot.");
    require(domain->dispatch_sync(*terminal_handle, 1u, {}, 8u).completed(),
            "Terminal-Fixture konnte Core nicht konstruieren.");
    domain->shutdown();
    domain->shutdown();
    const auto snapshot = domain->snapshot();
    const auto& target_snapshot = snapshot.targets[terminal_handle->slot];
    require(terminal.cleanups == 1u && terminal.core == nullptr &&
                !target_snapshot.registered &&
                target_snapshot.target_generation ==
                    terminal_handle->generation,
            "Terminales Consumer-Cleanup war nicht exact-once/monoton.");
}

void test_dedicated_domain_telemetry_before_ack() {
    NativePortAudioExecutionDomainConfig config;
    config.command_queue = test_config(8u, 64u, 4u);
    config.command_queue.mode =
        NativePortAudioCommandQueueMode::DedicatedThread;
    auto domain = NativePortAudioExecutionDomain::acquire(config);
    NativePortTelemetry telemetry;
    require(domain->bind_telemetry(&telemetry),
            "Audio-Telemetrie konnte nicht gebunden werden.");

    DomainTargetFixture fixture;
    fixture.domain = domain.get();
    const auto handle = domain->register_target(
        NativePortAudioExecutionDomainTarget::AudioEngine, &fixture,
        &DomainTargetFixture::execute, &DomainTargetFixture::cleanup);
    require(handle.has_value(),
            "Dedicated-Domain konnte Testziel nicht registrieren.");
    const auto publication_before = telemetry.snapshot().publication;
    const auto result = domain->dispatch_sync(
        *handle, 1u, {}, 3u, 0u,
        NativePortAudioExecutionDomainStage::AudioMix);
    const auto telemetry_after = telemetry.snapshot();
    const auto mix_index = static_cast<std::size_t>(
        NativePortTelemetryStage::AudioMix);
    const auto service_total_index = static_cast<std::size_t>(
        NativePortTelemetryStage::AudioServiceTotal);
    const auto domain_after = domain->snapshot();
    require(result.completed() && !result.inline_execution &&
                result.command_sequence == 1u &&
                result.stamp == NativePortAudioCommandStamp{3u, 0u} &&
                telemetry_after.publication > publication_before &&
                telemetry_after.stages[mix_index].available &&
                telemetry_after.stages[mix_index].calls >= 1u &&
                !telemetry_after.stages[service_total_index].available &&
                telemetry_after.stages[service_total_index].calls == 0u &&
                telemetry_after.audio_queue_available &&
                domain_after.producer_thread_identity != 0u &&
                domain_after.consumer_thread_identity != 0u &&
                domain_after.producer_thread_identity !=
                    domain_after.consumer_thread_identity,
            "Dedicated ACK wurde vor Telemetrie/Threadbeweis publiziert.");

    const auto decode_index = static_cast<std::size_t>(
        NativePortTelemetryStage::AudioDecode);
    const auto before_decode = telemetry.snapshot();
    const auto decoded = domain->dispatch_sync(
        *handle, 2u, {}, 3u, 0u,
        NativePortAudioExecutionDomainStage::AudioDecode);
    const auto after_decode = telemetry.snapshot();
    require(decoded.completed() &&
                after_decode.stages[decode_index].available &&
                after_decode.stages[decode_index].calls ==
                    before_decode.stages[decode_index].calls + 1u &&
                after_decode.stages[mix_index].calls ==
                    before_decode.stages[mix_index].calls &&
                after_decode.stages[service_total_index].calls ==
                    before_decode.stages[service_total_index].calls,
            "Reiner Decode-Command verlor seine exklusive Telemetriestufe.");

    const auto before_combined = telemetry.snapshot();
    const auto combined = domain->dispatch_sync(
        *handle, 2u, {}, 3u, 0u,
        NativePortAudioExecutionDomainStage::AudioDecodeAndMix);
    const auto after_combined = telemetry.snapshot();
    const auto combined_json = after_combined.serialize_json();
    constexpr std::string_view service_total_json_name =
        "\"audio_service_total\"";
    require(combined.completed() &&
                after_combined.stages[mix_index].calls ==
                    before_combined.stages[mix_index].calls &&
                after_combined.stages[decode_index].calls ==
                    before_combined.stages[decode_index].calls &&
                after_combined.stages[service_total_index].available &&
                after_combined.stages[service_total_index].calls ==
                    before_combined.stages[service_total_index].calls + 1u &&
                combined_json.find("\"schema\":2") != std::string::npos &&
                combined_json.find(
                    "\"audio_service_total\":{\"available\":true") !=
                    std::string::npos &&
                combined_json.find(service_total_json_name) ==
                    combined_json.rfind(service_total_json_name),
            "Combined Audioexecutor verlor AudioServiceTotal, wurde in eine "
            "schmalere Stufe projiziert oder nicht einmalig serialisiert.");

    require(domain->dispatch_sync(*handle, 3u, {}, 4u).completed() &&
                domain->unregister_target(*handle, &fixture),
            "Dedicated Normal-Destroy konnte Cleanup nicht disarmen.");
    const auto queue_before_unbind = domain->snapshot().queue;
    require(domain->unbind_telemetry(&telemetry),
            "Audio-Telemetrie konnte nicht geloest werden.");
    const auto queue_after_unbind = domain->snapshot().queue;
    require(queue_after_unbind.submitted_commands ==
                    queue_before_unbind.submitted_commands + 1u &&
                queue_after_unbind.completed_commands ==
                    queue_before_unbind.completed_commands + 1u,
            "Letzter Telemetrie-Unbind besitzt keine Worker-ACK-Barriere.");
    domain->shutdown();
    require(fixture.cleanups == 0u && fixture.core == nullptr,
            "Disarmtes Dedicated-Ziel wurde terminal erneut bereinigt.");
}

void test_async_target_failure_is_sticky_and_cancels_later_commands() {
    NativePortAudioExecutionDomainConfig config;
    config.command_queue = test_config(8u, 128u, 4u);
    config.command_queue.mode =
        NativePortAudioCommandQueueMode::SerialReference;
    auto domain = NativePortAudioExecutionDomain::acquire(config);
    DomainTargetFixture fixture;
    fixture.domain = domain.get();
    const auto handle = domain->register_target(
        NativePortAudioExecutionDomainTarget::AudioEngine, &fixture,
        &DomainTargetFixture::execute, &DomainTargetFixture::cleanup);
    require(handle.has_value(),
            "Async-Fehler-Fixture konnte Ziel nicht registrieren.");

    const auto failed = domain->dispatch_async(*handle, 99u, {}, 12u);
    const auto later = domain->dispatch_async(*handle, 1u, {}, 12u);
    require(failed.accepted() && later.accepted() &&
                failed.command_sequence == 1u &&
                later.command_sequence == 2u,
            "Async-Fixture konnte geordnete Commands nicht publizieren.");
    domain->pump();
    const auto snapshot = domain->snapshot();
    require(fixture.executions == 1u &&
                snapshot.first_error ==
                    NativePortAudioExecutionDomainFailure::TargetExecutionFailed &&
                snapshot.has_async_target_failure &&
                snapshot.async_target_failure.command_sequence == 1u &&
                snapshot.async_target_failure.frame_index == 12u &&
                snapshot.async_target_failure.guest_sequence == 0u &&
                snapshot.async_target_failure.target == handle->target &&
                snapshot.async_target_failure.target_slot == handle->slot &&
                snapshot.async_target_failure.target_generation ==
                    handle->generation &&
                snapshot.async_target_failure.opcode == 99u &&
                snapshot.async_target_failure.target_error_code == 4u &&
                snapshot.queue.failed_commands == 1u &&
                snapshot.queue.cancelled_commands == 1u,
            "Async-Executorfehler wurde nicht sticky/vollstaendig publiziert.");
    const auto rejected = domain->dispatch_async(*handle, 1u, {}, 13u);
    require(rejected.failure ==
                NativePortAudioExecutionDomainFailure::TargetExecutionFailed &&
                rejected.has_target_failure &&
                rejected.target_failure_command_sequence == 1u &&
                rejected.target_failure_frame_index == 12u &&
                rejected.target_failure_guest_sequence == 0u &&
                rejected.target_failure_target == handle->target &&
                rejected.target_failure_slot == handle->slot &&
                rejected.target_failure_generation == handle->generation &&
                rejected.target_failure_opcode == 99u &&
                rejected.target_failure_error_code == 4u,
            "Folgezugriff sah den sticky Async-Executorfehler nicht.");
    domain->shutdown();
    require(fixture.cleanups == 1u,
            "Terminaler Async-Fehler verlor Consumer-Cleanup.");
}

void test_worker_side_last_telemetry_unbind_is_rejected() {
    NativePortAudioExecutionDomainConfig config;
    config.command_queue = test_config(8u, 128u, 4u);
    auto domain = NativePortAudioExecutionDomain::acquire(config);
    NativePortTelemetry telemetry;
    require(domain->bind_telemetry(&telemetry),
            "Worker-Unbind-Fixture konnte Telemetrie nicht binden.");
    DomainTargetFixture fixture;
    fixture.domain = domain.get();
    fixture.telemetry_to_unbind = &telemetry;
    const auto handle = domain->register_target(
        NativePortAudioExecutionDomainTarget::AudioEngine, &fixture,
        &DomainTargetFixture::execute, &DomainTargetFixture::cleanup);
    require(handle.has_value(),
            "Worker-Unbind-Fixture konnte Ziel nicht registrieren.");
    const auto unbound = domain->dispatch_sync(
        *handle, 4u, {}, 1u, 0u,
        NativePortAudioExecutionDomainStage::AudioDecodeAndMix);
    const auto later = domain->dispatch_sync(*handle, 1u, {}, 2u);
    const auto destroyed = domain->dispatch_sync(*handle, 3u, {}, 3u);
    const auto queue_before_producer_unbind = domain->snapshot().queue;
    const bool producer_unbound = domain->unbind_telemetry(&telemetry);
    const auto queue_after_producer_unbind = domain->snapshot().queue;
    require(unbound.has_ack &&
                unbound.ack.status ==
                    NativePortAudioCommandAckStatus::Failed &&
                !fixture.telemetry_unbind_result && later.completed() &&
                destroyed.completed() &&
                domain->unregister_target(*handle, &fixture) &&
                producer_unbound &&
                queue_after_producer_unbind.submitted_commands ==
                    queue_before_producer_unbind.submitted_commands + 1u &&
                queue_after_producer_unbind.completed_commands ==
                    queue_before_producer_unbind.completed_commands + 1u,
            "Aktiver Worker-Unbind veraenderte das Binding oder umging die "
            "Producer-ACK-Lifetime-Barriere.");
    domain->shutdown();
}

void test_movie_target_shares_domain_and_retires() {
    auto domain = NativePortAudioExecutionDomain::acquire();
    const auto registered_movie_targets = [](const auto& snapshot) {
        std::size_t count = 0u;
        for (const auto& target : snapshot.targets) {
            if (target.target == NativePortAudioExecutionDomainTarget::Movie &&
                target.registered)
                ++count;
        }
        return count;
    };

    {
        NativePortMovieSession movie;
        require(registered_movie_targets(domain->snapshot()) == 1u,
                "Movie registrierte sich nicht im gemeinsamen Audio-Domainziel.");
    }

    const auto after = domain->snapshot();
    require(registered_movie_targets(after) == 0u &&
                after.queue.submitted_commands >= 1u &&
                after.queue.completed_commands >= 1u &&
                after.producer_thread_identity != 0u &&
                after.consumer_thread_identity != 0u &&
                after.producer_thread_identity != after.consumer_thread_identity,
            "Movie-Destroy/ACK retirierte das gemeinsame Domainziel nicht sauber.");
    domain->shutdown();
}

} // namespace

int main() {
    test_pod_contract_and_defaults();
    test_disabled_queue();
    test_fifo_equal_stamp_payload_ack();
    test_abort_does_not_consume_command_sequence();
    test_stamp_contract();
    test_payload_overflow_and_copy_contract();
    test_payload_alignment_wrap_and_accounting();
    test_terminal_worker_failure_and_cancel();
    test_first_error_publication_gate();
    test_backpressure_and_thread_domains();
    test_abandon_and_serial_mode();
    test_serial_domain_top_level_and_cleanup();
    test_dedicated_domain_telemetry_before_ack();
    test_async_target_failure_is_sticky_and_cancels_later_commands();
    test_worker_side_last_telemetry_unbind_is_rejected();
    test_completion_wake_drains_pending_without_new_command();
    test_resultless_async_controls_preserve_sync_fence_order();
    test_movie_target_shares_domain_and_retires();
    std::cout << "native_port_audio_command_queue_tests: OK\n";
    return 0;
}
