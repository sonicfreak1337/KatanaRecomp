#include "katana/runtime/maple_mmio.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
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
                controller->transferred_word_count() == 6u,
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
    require(throws([&] { memory.write_u32(0x005F6C18u, 1u); }) &&
                controller->state() == MapleDmaState::Failed &&
                controller->error() == MapleDmaError::ProtectedRange &&
                scheduler.pending_event_count() == 0u,
            "Erste Adresse hinter dem Maple-Schutzfenster wird nicht strukturiert abgelehnt.");

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
                completions == completions_before_failed_commit &&
                scheduler.pending_event_count() == 0u,
            "Maple-Completionfehler schreibt partiell oder tritt aus dem Schedulercallback aus.");

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
    require(throws([&] { memory.write_u32(0x005F6C18u, 1u); }),
            "Unbekanntes Maple-DMA-Deskriptormuster wurde akzeptiert.");

    memory.write_u32(table, 0x80000000u);
    memory.write_u32(table + 4u, 0x0C002000u);
    memory.write_u32(table + 8u, 0x01002009u);
    require(throws([&] { memory.write_u32(0x005F6C18u, 1u); }),
            "Widerspruechliche Maple-Frame-/Deskriptorlaenge wurde akzeptiert.");

    std::cout << "Dreamcast-Maple-MMIO und echter DMA-Responsepfad erfolgreich.\n";
}
