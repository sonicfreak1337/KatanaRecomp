#include "katana/runtime/maple_mmio.hpp"
#include "katana/runtime/host_input.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename F> bool throws(F&& function) {
    try {
        function();
    } catch (...) {
        return true;
    }
    return false;
}

class TimelineGamepadSource final : public katana::runtime::HostGamepadSource {
  public:
    [[nodiscard]] std::vector<katana::runtime::HostControllerSample> poll() override {
        return samples;
    }

    std::vector<katana::runtime::HostControllerSample> samples;
};
} // namespace

int main() {
    using namespace katana::runtime;
    Memory memory(0u);
    auto ram = std::make_shared<LinearMemoryDevice>(16u * 1024u * 1024u);
    memory.map_region("test-main-ram", 0x0C000000u, ram);
    EventScheduler scheduler;
    auto maple = std::make_shared<MapleBus>();
    ControllerState state;
    state.pressed_buttons = static_cast<std::uint16_t>(ControllerButton::A);
    auto input =
        std::make_shared<ReplayInputBackend>(std::vector<ControllerState>(16u, state));
    maple->attach(0u, 0u, std::make_shared<MapleControllerDevice>(input));
    std::uint64_t completions = 0u;
    const auto controller = map_dreamcast_maple_controller(
        memory, scheduler, maple, MapleDmaTiming{10u}, [&] { ++completions; });

    constexpr std::uint32_t table = 0x0C001000u;
    constexpr std::uint32_t response = 0x0C002000u;
    constexpr std::uint32_t request_header = 0x01002009u;
    memory.write_u32(table, 0x80000001u);
    memory.write_u32(table + 4u, response);
    memory.write_u32(table + 8u, request_header);
    memory.write_u32(table + 12u, 0x01000000u);

    memory.write_u32(0xA05F6C04u, table);
    memory.write_u32(0x805F6C10u, 0u);
    memory.write_u32(0x005F6C14u, 1u);
    memory.write_u32(0xA05F6C18u, 1u);
    require(memory.read_u32(0x005F6C18u) == 1u && scheduler.pending_event_count() == 1u,
            "Maple-DMA-Start plant keinen sichtbaren asynchronen Transfer.");
    static_cast<void>(scheduler.advance_by(100u, 1u));
    require(memory.read_u32(0x005F6C18u) == 0u && completions == 1u &&
                controller->completed_dma_count() == 1u &&
                controller->transferred_word_count() == 6u &&
                controller->event_publication_state() ==
                    MapleDmaEventPublicationState::Published &&
                controller->event_publication_error() ==
                    MapleDmaEventPublicationError::None &&
                controller->event_publication_failure_count() == 0u,
            "Maple-DMA schliesst nicht einmalig mit Zaehlern und Status ab.");
    require(memory.read_u32(response) == 0x03200008u &&
                memory.read_u32(response + 4u) == 0x01000000u &&
                (memory.read_u32(response + 8u) &
                 static_cast<std::uint16_t>(ControllerButton::A)) == 0u,
            "Maple-DMA schreibt keinen korrekt adressierten Controller-Responseframe.");

    constexpr std::uint32_t second_response = 0x0C102000u;
    memory.write_u32(table, 0x00000001u);
    memory.write_u32(table + 4u, response);
    memory.write_u32(table + 8u, request_header);
    memory.write_u32(table + 12u, 0x01000000u);
    memory.write_u32(table + 16u, 0x80000001u);
    memory.write_u32(table + 20u, second_response);
    memory.write_u32(table + 24u, request_header);
    memory.write_u32(table + 28u, 0x01000000u);
    for (std::uint32_t index = 0u; index < 4u; ++index) {
        memory.write_u32(response + index * 4u, 0xABABABABu);
        memory.write_u32(second_response + index * 4u, 0xCDCDCDCDu);
    }
    std::uint32_t multi_response_notifications = 0u;
    bool first_notification_saw_complete_batch = false;
    memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
        if (event.address != response && event.address != second_response) return;
        if (multi_response_notifications++ == 0u) {
            first_notification_saw_complete_batch =
                memory.read_u32(response) == 0x03200008u &&
                memory.read_u32(second_response) == 0x03200008u;
        }
    });
    const auto completions_before_multi_response = completions;
    memory.write_u32(0x005F6C04u, table);
    memory.write_u32(0x005F6C10u, 0u);
    memory.write_u32(0x005F6C14u, 1u);
    memory.write_u32(0x005F6C18u, 1u);
    // Two request/response pairs transfer twelve words at ten cycles per word.
    static_cast<void>(scheduler.advance_by(120u, 1u));
    memory.clear_guest_write_observer();
    require(controller->state() == MapleDmaState::Completed &&
                completions == completions_before_multi_response + 1u &&
                memory.read_u32(response) == 0x03200008u &&
                memory.read_u32(second_response) == 0x03200008u &&
                controller->transferred_word_count() == 18u &&
                multi_response_notifications == 2u &&
                first_notification_saw_complete_batch,
            "Maple-Mehrantwort-DMA macht zwischen Antwortbeobachtern einen Teilzustand sichtbar.");

    const std::vector<std::uint8_t> admitted_bytes{0x10u, 0x20u, 0x30u, 0x40u};
    const std::vector<std::uint8_t> rejected_bytes{0x50u, 0x60u, 0x70u, 0x80u};
    memory.write_u32(response, 0xEEEEEEEEu);
    const std::vector<LinearMemoryTransactionWrite> rejected_batch{
        {response, admitted_bytes},
        {0x01000000u, rejected_bytes},
    };
    require(!memory.commit_linear_transaction_batch(
                rejected_batch, CodeWriteSource::Dma) &&
                memory.read_u32(response) == 0xEEEEEEEEu,
            "Abgelehnte Mehrbereichstransaktion macht eine Maple-Teilantwort sichtbar.");
    memory.set_lookup_mode(MemoryLookupMode::Reference);
    const std::vector<LinearMemoryTransactionWrite> reference_lookup_batch{
        {response, admitted_bytes},
        {second_response, rejected_bytes},
    };
    require(memory.commit_linear_transaction_batch(
                reference_lookup_batch, CodeWriteSource::Dma) &&
                memory.read_u32(response) == 0x40302010u &&
                memory.read_u32(second_response) == 0x80706050u,
            "Atomarer Maple-Mehrbereichscommit haengt faelschlich vom Memory-Index ab.");
    memory.set_lookup_mode(MemoryLookupMode::Indexed);

    memory.write_u32(table, 0x80000001u);
    memory.write_u32(table + 4u, response);
    memory.write_u32(table + 8u, request_header);
    memory.write_u32(table + 12u, 0x01000000u);

    memory.write_u32(0x005F6C80u, 0xFFFFFFFFu);
    memory.write_u32(0x005F6CE8u, 0u);
    require(memory.read_u32(0xA05F6C80u) == 0xFFFF130Fu && memory.read_u32(0x805F6CE8u) == 0u &&
                memory.read_u32(0x005F6C84u) == 0u,
            "Maple-System-, MSB- oder Statusregister besitzen falsche Semantik.");
    memory.write_u32(0x005F6C8Cu, 0x12340000u);
    memory.write_u32(0x005F6C8Cu, 0x61557F00u);
    require(throws([&] { static_cast<void>(memory.read_u32(0x005F6C8Cu)); }) &&
                throws([&] { memory.write_u32(0x005F6C84u, 1u); }) &&
                throws([&] { static_cast<void>(memory.read_u32(0x005F6C00u)); }) &&
                throws([&] { memory.write_u32(0x005F6C00u, 1u); }) &&
                throws([&] { static_cast<void>(memory.read_u16(0x005F6C04u)); }),
            "Maple-MMIO-Zugriffsrechte oder Breitenvertrag sind offen.");

    constexpr std::uint32_t protected_last_start = 0x0C0FFFE0u;
    constexpr std::uint32_t protected_first_invalid = 0x0C100000u;
    memory.write_u32(0x005F6CE8u, 1u);
    memory.write_u32(protected_last_start, 0x80000200u);
    memory.write_u32(protected_last_start + 4u, 0u);
    memory.write_u32(protected_first_invalid, 0x80000200u);
    memory.write_u32(protected_first_invalid + 4u, 0u);
    memory.write_u32(0x005F6C8Cu, 0x61554040u);
    memory.write_u32(0x005F6C04u, protected_last_start);
    memory.write_u32(0x005F6C10u, 0u);
    memory.write_u32(0x005F6C14u, 1u);
    memory.write_u32(0x005F6C18u, 1u);
    require(controller->state() == MapleDmaState::Active &&
                scheduler.pending_event_count() == 1u,
            "Letzte gueltige Maple-Schutzfenster-Startadresse wird abgelehnt.");
    static_cast<void>(scheduler.advance_by(10u, 1u));
    require(controller->state() == MapleDmaState::Completed,
            "Transfer an der letzten gueltigen Maple-Schutzfensteradresse schliesst nicht ab.");
    memory.write_u32(0x005F6C04u, protected_first_invalid);
    const auto completions_before_software_fault = completions;
    require(!throws([&] { memory.write_u32(0x005F6C18u, 1u); }) &&
                controller->state() == MapleDmaState::Failed &&
                controller->error() == MapleDmaError::ProtectedRange &&
                controller->error_address() == protected_first_invalid &&
                !controller->hard_trigger_failed() &&
                controller->event_publication_state() ==
                    MapleDmaEventPublicationState::Published &&
                completions == completions_before_software_fault + 1u &&
                scheduler.pending_event_count() == 0u,
            "Softwaregetriggerter Maple-Gastfehler erreicht Geraete-/ASIC-Zustand nicht "
            "ohne Hostexception.");

    memory.write_u32(0x005F6C14u, 0u);
    memory.write_u32(0x005F6C10u, 1u);
    memory.write_u32(0x005F6C14u, 1u);
    memory.write_u32(0x005F6C18u, 1u);
    const auto completions_before_hardware_fault = completions;
    require(controller->state() == MapleDmaState::Armed &&
                !throws([&] { controller->hardware_trigger(); }) &&
                controller->state() == MapleDmaState::Failed &&
                controller->error() == MapleDmaError::ProtectedRange &&
                controller->error_address() == protected_first_invalid &&
                !controller->hard_trigger_failed() &&
                completions == completions_before_hardware_fault + 1u &&
                scheduler.pending_event_count() == 0u,
            "Hardwaregetriggerter Maple-Gastfehler divergiert vom Softwaretriggervertrag.");

    memory.write_u32(0x005F6C14u, 0u);
    memory.write_u32(0x005F6C8Cu, 0x61557F00u);
    memory.write_u32(table, 0x80000001u);
    memory.write_u32(table + 4u, response);
    memory.write_u32(table + 8u, request_header);
    memory.write_u32(table + 12u, 0x01000000u);
    memory.write_u32(0x005F6C04u, table);
    memory.write_u32(0x005F6C10u, 1u);
    memory.write_u32(0x005F6C14u, 1u);
    memory.write_u32(0x005F6C18u, 1u);
    require(controller->state() == MapleDmaState::Armed &&
                memory.read_u32(0x005F6C18u) == 1u &&
                scheduler.pending_event_count() == 0u,
            "Maple-Hardwaretriggermodus startet bereits beim Software-Startwrite.");
    controller->hardware_trigger();
    require(controller->state() == MapleDmaState::Active &&
                scheduler.pending_event_count() == 1u,
            "GeArmed Maple-DMA startet beim Hardwaretrigger nicht.");
    static_cast<void>(scheduler.advance_by(100u, 1u));
    require(controller->state() == MapleDmaState::Completed &&
                controller->error() == MapleDmaError::None,
            "Hardwaregetriggerte Maple-DMA erreicht keinen erfolgreichen Abschluss.");

    for (std::uint32_t index = 0u; index < 4u; ++index)
        memory.write_u32(response + index * 4u, 0xCCCCCCCCu);
    memory.write_u32(0x005F6C10u, 0u);
    memory.write_u32(0x005F6C18u, 1u);
    const auto completions_before_failed_commit = completions;
    memory.write_u32(0x005F6C8Cu, 0x61554141u);
    static_cast<void>(scheduler.advance_by(100u, 1u));
    bool failed_response_untouched = true;
    for (std::uint32_t index = 0u; index < 4u; ++index)
        failed_response_untouched &=
            memory.read_u32(response + index * 4u) == 0xCCCCCCCCu;
    require(controller->state() == MapleDmaState::Failed &&
                controller->error() == MapleDmaError::ResponseRange &&
                controller->error_address() == response &&
                failed_response_untouched &&
                completions == completions_before_failed_commit + 1u &&
                scheduler.pending_event_count() == 0u,
            "Maple-Completionfehler schreibt partiell, verliert das ASIC-Ereignis oder tritt "
            "aus dem Schedulercallback aus.");

    memory.write_u32(0x005F6C14u, 0u);
    memory.write_u32(0x005F6C8Cu, 0x61557F00u);
    memory.write_u32(0x005F6C04u, table);
    memory.write_u32(0x005F6C10u, 1u);
    memory.write_u32(0x005F6C14u, 1u);
    memory.write_u32(0x005F6C18u, 1u);
    require(controller->state() == MapleDmaState::Armed,
            "Maple-Resettest kann keinen wartenden Hardwaretransfer armen.");
    controller->reset();
    require(controller->state() == MapleDmaState::Disabled &&
                memory.read_u32(0x005F6C18u) == 0u &&
                scheduler.pending_event_count() == 0u,
            "Maple-Reset entfernt einen geArmed Transfer nicht.");

    for (std::uint32_t index = 0u; index < 4u; ++index)
        memory.write_u32(response + index * 4u, 0xEEEEEEEEu);
    memory.write_u32(0x005F6C04u, table);
    memory.write_u32(0x005F6C10u, 0u);
    memory.write_u32(0x005F6C14u, 1u);
    memory.write_u32(0x005F6C18u, 1u);
    const auto completions_before_abort = completions;
    require(controller->state() == MapleDmaState::Active &&
                scheduler.pending_event_count() == 1u,
            "Maple-Aborttest kann keinen aktiven Transfer starten.");
    memory.write_u32(0x005F6C14u, 0u);
    static_cast<void>(scheduler.advance_by(100u, 1u));
    bool aborted_response_untouched = true;
    for (std::uint32_t index = 0u; index < 4u; ++index)
        aborted_response_untouched &=
            memory.read_u32(response + index * 4u) == 0xEEEEEEEEu;
    require(controller->state() == MapleDmaState::Disabled &&
                scheduler.pending_event_count() == 0u &&
                completions == completions_before_abort &&
                aborted_response_untouched,
            "Maple-Abort laesst ein Ereignis oder einen spaeten Responsewrite aktiv.");

    memory.write_u32(table, 0x80000100u);
    memory.write_u32(0x005F6C04u, table);
    memory.write_u32(0x005F6C14u, 1u);
    const auto completions_before_unsupported_descriptor = completions;
    require(!throws([&] { memory.write_u32(0x005F6C18u, 1u); }) &&
                controller->state() == MapleDmaState::Failed &&
                controller->error() == MapleDmaError::UnsupportedDescriptor &&
                controller->error_address() == table &&
                completions == completions_before_unsupported_descriptor + 1u,
            "Unbekanntes Maple-DMA-Deskriptormuster erreicht den Gastfehlerpfad nicht.");

    memory.write_u32(table, 0x80000000u);
    memory.write_u32(table + 4u, 0x0C002000u);
    memory.write_u32(table + 8u, 0x01002009u);
    const auto completions_before_invalid_descriptor = completions;
    require(!throws([&] { memory.write_u32(0x005F6C18u, 1u); }) &&
                controller->state() == MapleDmaState::Failed &&
                controller->error() == MapleDmaError::InvalidDescriptor &&
                controller->error_address() == table &&
                completions == completions_before_invalid_descriptor + 1u,
            "Widerspruechliche Maple-Frame-/Deskriptorlaenge erreicht den Gastfehlerpfad nicht.");

    {
        Memory publication_memory(0u);
        auto publication_ram =
            std::make_shared<LinearMemoryDevice>(16u * 1024u * 1024u);
        publication_memory.map_region(
            "publication-main-ram", 0x0C000000u, publication_ram);
        EventScheduler publication_scheduler;
        auto publication_maple = std::make_shared<MapleBus>();
        auto publication_input = std::make_shared<ReplayInputBackend>(
            std::vector<ControllerState>(4u, ControllerState{}));
        publication_maple->attach(
            0u, 0u, std::make_shared<MapleControllerDevice>(publication_input));

        std::uint64_t publication_calls = 0u;
        bool observer_saw_publishing = true;
        std::shared_ptr<DreamcastMapleController> publication_controller;
        publication_controller = map_dreamcast_maple_controller(
            publication_memory,
            publication_scheduler,
            publication_maple,
            MapleDmaTiming{10u},
            [&] {
                ++publication_calls;
                observer_saw_publishing &=
                    publication_controller->event_publication_state() ==
                    MapleDmaEventPublicationState::Publishing;
                throw std::runtime_error(
                    "synthetischer Maple-ASIC-Publikationsfehler");
            });

        constexpr std::uint32_t publication_table = 0x0C003000u;
        constexpr std::uint32_t publication_response = 0x0C004000u;
        publication_memory.write_u32(publication_table, 0x80000001u);
        publication_memory.write_u32(
            publication_table + 4u, publication_response);
        publication_memory.write_u32(
            publication_table + 8u, request_header);
        publication_memory.write_u32(
            publication_table + 12u, 0x01000000u);
        publication_memory.write_u32(
            0x005F6C04u, publication_table);
        publication_memory.write_u32(0x005F6C10u, 0u);
        publication_memory.write_u32(0x005F6C14u, 1u);
        publication_memory.write_u32(0x005F6C18u, 1u);
        const auto successful_publication_escaped = throws([&] {
            static_cast<void>(
                publication_scheduler.advance_by(100u, 1u));
        });
        const auto committed_snapshot = publication_controller->snapshot();
        require(!successful_publication_escaped &&
                    publication_memory.read_u32(publication_response) ==
                        0x03200008u &&
                    committed_snapshot.state == MapleDmaState::Completed &&
                    committed_snapshot.error == MapleDmaError::None &&
                    committed_snapshot.completed_dma_count == 1u &&
                    committed_snapshot.failed_dma_count == 0u &&
                    committed_snapshot.event_publication_state ==
                        MapleDmaEventPublicationState::Failed &&
                    committed_snapshot.event_publication_error ==
                        MapleDmaEventPublicationError::ObserverException &&
                    committed_snapshot.event_publication_failure_count == 1u &&
                    publication_calls == 1u && observer_saw_publishing,
                "Maple-ASIC-Publikationsfehler klassifiziert einen atomar "
                "commiteten DMA nachtraeglich als Transferfehler.");

        publication_memory.write_u32(
            publication_table, 0x80000100u);
        const auto guest_fault_escaped = throws([&] {
            publication_memory.write_u32(0x005F6C18u, 1u);
        });
        const auto failed_snapshot = publication_controller->snapshot();
        require(!guest_fault_escaped &&
                    failed_snapshot.state == MapleDmaState::Failed &&
                    failed_snapshot.error ==
                        MapleDmaError::UnsupportedDescriptor &&
                    failed_snapshot.error_address == publication_table &&
                    failed_snapshot.completed_dma_count == 1u &&
                    failed_snapshot.failed_dma_count == 1u &&
                    failed_snapshot.event_publication_state ==
                        MapleDmaEventPublicationState::Failed &&
                    failed_snapshot.event_publication_error ==
                        MapleDmaEventPublicationError::ObserverException &&
                    failed_snapshot.event_publication_failure_count == 2u &&
                    publication_calls == 2u && observer_saw_publishing,
                "Maple-Gastfehler verliert bei fehlschlagender "
                "ASIC-Publikation seine eigene typisierte Ursache.");
    }

    {
        Memory timeline_memory(0u);
        timeline_memory.map_region(
            "timeline-main-ram",
            0x0C000000u,
            std::make_shared<LinearMemoryDevice>(16u * 1024u * 1024u));
        EventScheduler timeline_scheduler;
        auto timeline_bus = std::make_shared<MapleBus>();
        auto timeline_input = std::make_shared<ControllerInputTimeline>(
            ControllerNormalizationConfig{0u, 0u, 0u});
        TimelineGamepadSource timeline_source;
        HostControllerSample first_sample;
        first_sample.device_id = 1u;
        first_sample.kind = HostControllerKind::XInput;
        first_sample.buttons =
            host_controller_button(HostControllerButton::South);
        first_sample.left_x = 32'767;
        first_sample.connected = true;
        timeline_source.samples = {first_sample};
        require(timeline_input->poll(timeline_source, 0u).has_value(),
                "Maple-MMIO-Timeline nimmt den ersten Hostzustand nicht an.");
        timeline_bus->attach(
            0u, 0u, std::make_shared<MapleControllerDevice>(timeline_input));
        const auto timeline_controller = map_dreamcast_maple_controller(
            timeline_memory,
            timeline_scheduler,
            timeline_bus,
            MapleDmaTiming{10u});

        constexpr std::uint32_t timeline_table = 0x0C005000u;
        constexpr std::uint32_t timeline_first_response = 0x0C006000u;
        constexpr std::uint32_t timeline_second_response = 0x0C007000u;
        const auto write_timeline_request = [&](const std::uint32_t destination) {
            timeline_memory.write_u32(timeline_table, 0x80000001u);
            timeline_memory.write_u32(timeline_table + 4u, destination);
            timeline_memory.write_u32(timeline_table + 8u, request_header);
            timeline_memory.write_u32(timeline_table + 12u, 0x01000000u);
            timeline_memory.write_u32(0x005F6C04u, timeline_table);
            timeline_memory.write_u32(0x005F6C10u, 0u);
            timeline_memory.write_u32(0x005F6C14u, 1u);
            timeline_memory.write_u32(0x005F6C18u, 1u);
        };

        write_timeline_request(timeline_first_response);
        static_cast<void>(timeline_scheduler.advance_to(5u, 1u));
        auto second_sample = first_sample;
        second_sample.buttons =
            host_controller_button(HostControllerButton::East);
        second_sample.left_x = std::numeric_limits<std::int16_t>::min();
        timeline_source.samples = {second_sample};
        require(timeline_input->poll(timeline_source, 5u).has_value(),
                "Hostzustandswechsel waehrend Maple-DMA fehlt in der Timeline.");
        static_cast<void>(timeline_scheduler.advance_to(60u, 1u));
        const auto first_condition =
            timeline_memory.read_u32(timeline_first_response + 8u);
        const auto first_axes =
            timeline_memory.read_u32(timeline_first_response + 12u);
        require(
            (first_condition &
             static_cast<std::uint16_t>(ControllerButton::A)) == 0u &&
                (first_condition &
                 static_cast<std::uint16_t>(ControllerButton::B)) != 0u &&
                (first_axes & 0xFFu) == 0xFFu,
            "Maple-DMA verwendet einen erst nach dem Transaktionszyklus sichtbaren Zustand.");

        write_timeline_request(timeline_second_response);
        static_cast<void>(timeline_scheduler.advance_to(120u, 1u));
        const auto second_condition =
            timeline_memory.read_u32(timeline_second_response + 8u);
        const auto second_axes =
            timeline_memory.read_u32(timeline_second_response + 12u);
        require(
            (second_condition &
             static_cast<std::uint16_t>(ControllerButton::A)) != 0u &&
                (second_condition &
                 static_cast<std::uint16_t>(ControllerButton::B)) == 0u &&
                (second_axes & 0xFFu) == 0u &&
                timeline_controller->completed_dma_count() == 2u &&
                timeline_input->sampled_frames() == 2u,
            "Naechste Maple-DMA-Transaktion sieht nicht den letzten gastzeitgebundenen Zustand.");
    }

    maple->attach(0u, 1u, std::make_shared<MapleVmuDevice>());
    auto persistence_bound = snapshot_dreamcast_maple_state(*maple, *controller);
    persistence_bound.controller.completion_event.reset();
    persistence_bound.controller.completion_event_rehydration_pending = false;
    persistence_bound.controller.pending_responses = {
        {response, {0x01200008u, 0x02000000u, 0xA5A5A5A5u}}};
    persistence_bound.controller.enabled = 1u;
    persistence_bound.controller.active = 1u;
    persistence_bound.controller.state = MapleDmaState::Active;
    persistence_bound.controller.error = MapleDmaError::None;
    persistence_bound.controller.error_address.reset();
    persistence_bound.controller.address_protect = 0x4040u;
    require(
        throws([&] {
            static_cast<void>(prepare_dreamcast_maple_state_restore(
                *maple,
                *controller,
                persistence_bound,
                PersistenceHandoffPolicy::ProductPreserveTarget));
        }),
        "Produkt-Handoff akzeptiert eine ausstehende Maple-Antwort, deren "
        "Abhaengigkeit von Capture-VMU-Bytes nicht rekonstruierbar ist.");

    std::cout << "Dreamcast-Maple-MMIO und echter DMA-Responsepfad erfolgreich.\n";
}
