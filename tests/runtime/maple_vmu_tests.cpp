#include "katana/runtime/maple.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace katana::runtime;
    std::vector<std::uint8_t> source(vmu_storage_size, 0xFFu);
    source[0] = 0x12u;
    auto vmu = std::make_shared<MapleVmuDevice>(source);
    MapleBus bus;
    bus.attach(0u, 1u, vmu);
    const auto location = [](const std::uint16_t block,
                             const std::uint8_t phase = 0u,
                             const std::uint8_t partition = 0u) {
        return static_cast<std::uint32_t>(partition) |
               (static_cast<std::uint32_t>(phase) << 8u) |
               (static_cast<std::uint32_t>(block & 0xFFu) << 24u) |
               (static_cast<std::uint32_t>(block >> 8u) << 16u);
    };
    const auto write_phase = [&](const std::uint16_t block,
                                 const std::uint8_t phase,
                                 const std::uint32_t pattern) {
        std::vector<std::uint32_t> payload(
            2u + 128u / sizeof(std::uint32_t), pattern);
        payload[0] = vmu_memory_function;
        payload[1] = location(block, phase);
        return payload;
    };

    const auto info = bus.exchange(0u, 1u, {MapleCommand::DeviceRequest, {}});
    require(info.code == MapleResponseCode::DeviceInfo && info.payload.size() == 28u &&
                info.payload[0] == vmu_memory_function && info.payload[1] == 0x00410F00u,
            "VMU meldet keinen vollstaendigen 28-Wort-Device-Info-Vertrag.");
    const auto media = bus.exchange(
        0u,
        1u,
        {MapleCommand::GetMemoryInformation, {vmu_memory_function, location(0u)}});
    require(
        media.code == MapleResponseCode::DataTransfer &&
            media.payload ==
                std::vector<std::uint32_t>({
                    vmu_memory_function,
                    0x000000FFu,
                    0x00FE00FFu,
                    0x00FD0001u,
                    0x0000000Du,
                    0x001F00C8u,
                    0x00800000u,
                }),
        "VMU meldet kein Standardlayout mit 256 Bloecken und 200 Datenbloecken.");
    const auto wrong_function = bus.exchange(
        0u,
        1u,
        {MapleCommand::GetMemoryInformation, {0x01000000u, location(0u)}});
    const auto malformed = bus.exchange(
        0u,
        1u,
        {MapleCommand::GetMemoryInformation, {vmu_memory_function}});
    const auto unknown = bus.exchange(
        0u,
        1u,
        {static_cast<MapleCommand>(0x7Fu), {}});
    require(wrong_function.code == MapleResponseCode::FunctionNotSupported &&
                malformed.code == MapleResponseCode::FileError &&
                malformed.payload == std::vector<std::uint32_t>({0x10000000u}) &&
                unknown.code == MapleResponseCode::UnknownCommand,
            "VMU trennt unbekannte Funktion, fehlerhaften Request und Command nicht.");

    const auto initial = bus.exchange(
        0u,
        1u,
        {MapleCommand::BlockRead, {vmu_memory_function, location(0u)}});
    require(initial.code == MapleResponseCode::DataTransfer &&
                initial.payload.size() == 130u &&
                initial.payload[0] == vmu_memory_function &&
                initial.payload[1] == location(0u) &&
                (initial.payload[2] & 0xFFu) == 0x12u,
            "VMU-Blocklesen ist nicht vollstaendig oder Little Endian.");

    require(
        bus.exchange(
               0u,
               1u,
               {MapleCommand::BlockWrite, write_phase(1u, 0u, 0xA0A0A0A0u)})
                    .code == MapleResponseCode::Ack &&
            bus.exchange(
               0u,
               1u,
               {MapleCommand::BlockWrite, write_phase(1u, 1u, 0xA1A1A1A1u)})
                    .code == MapleResponseCode::Ack &&
            vmu->read_byte(vmu_block_size) == 0xFFu,
        "VMU schreibt Phasendaten vor dem Block-Sync sichtbar.");
    const auto staged = vmu->state_snapshot();
    require(staged.pending_write && staged.pending_write->block == 1u &&
                staged.pending_write->next_phase == 2u,
            "VMU-Snapshot verliert eine ausstehende Schreibsequenz.");

    MapleVmuDevice product_restored(source);
    require(product_restored
                    .transact(
                        {MapleCommand::BlockWrite, write_phase(2u, 0u, 0xBBBBBBBBu)})
                    .code == MapleResponseCode::Ack,
            "VMU-Produkttest kann keine unabhaengige Zielphase vormerken.");
    auto product_restore = product_restored.prepare_state_restore(
        staged, PersistenceHandoffPolicy::ProductPreserveTarget);
    product_restored.commit_prepared_state_restore(std::move(product_restore));
    const auto product_state = product_restored.state_snapshot();
    require(product_state.pending_write == staged.pending_write &&
                product_restored.read_byte(vmu_block_size) == 0xFFu,
            "Produkt-Handoff verliert Gast-Schreibfortschritt oder ersetzt Savebytes.");
    product_restored.set_write_protected(true);
    require(throws<std::invalid_argument>([&] {
                static_cast<void>(product_restored.prepare_state_restore(
                    staged, PersistenceHandoffPolicy::ProductPreserveTarget));
            }),
            "Produkt-Handoff uebernimmt eine Gast-Schreibphase trotz Hostschutz.");

    MapleVmuDevice restored(source);
    restored.restore_state(staged);
    require(
        restored.transact(
                    {MapleCommand::BlockWrite, write_phase(1u, 2u, 0xA2A2A2A2u)})
                    .code == MapleResponseCode::Ack &&
            restored.transact(
                        {MapleCommand::BlockWrite, write_phase(1u, 3u, 0xA3A3A3A3u)})
                    .code == MapleResponseCode::Ack &&
            restored.read_byte(vmu_block_size) == 0xFFu &&
            restored.transact(
                        {MapleCommand::BlockSync,
                         {vmu_memory_function, location(1u, 4u)}})
                    .code == MapleResponseCode::Ack &&
            restored.read_byte(vmu_block_size) == 0xA0u &&
            restored.read_byte(vmu_block_size + 128u) == 0xA1u &&
            restored.read_byte(vmu_block_size + 256u) == 0xA2u &&
            restored.read_byte(vmu_block_size + 384u) == 0xA3u,
        "VMU restauriert, synchronisiert oder committed vier Schreibphasen nicht.");
    require(source[vmu_block_size] == 0xFFu && vmu->source_byte(vmu_block_size) == 0xFFu,
            "VMU veraendert ihr Quellabbild statt einer Arbeitskopie.");

    const auto first_phase =
        restored.transact({MapleCommand::BlockWrite, write_phase(2u, 0u, 0xB0B0B0B0u)});
    const auto skipped_phase =
        restored.transact({MapleCommand::BlockWrite, write_phase(2u, 2u, 0xB2B2B2B2u)});
    const auto rejected_sync = restored.transact(
        {MapleCommand::BlockSync, {vmu_memory_function, location(2u, 4u)}});
    require(first_phase.code == MapleResponseCode::Ack &&
                skipped_phase.code == MapleResponseCode::FileError &&
                skipped_phase.payload == std::vector<std::uint32_t>({0x02000000u}) &&
                rejected_sync.code == MapleResponseCode::FileError &&
                restored.read_byte(vmu_block_size * 2u) == 0xFFu,
            "VMU committed eine lueckenhafte oder fehlgeschlagene Schreibsequenz.");

    vmu->set_write_protected(true);
    const auto protected_snapshot = vmu->snapshot();
    require(protected_snapshot.size == vmu_storage_size &&
                protected_snapshot.write_protected &&
                !protected_snapshot.persistent_working_copy,
            "VMU-Snapshot verliert Speicherprofil oder Schreibschutz.");
    require(bus.exchange(
                    0u,
                    1u,
                    {MapleCommand::BlockWrite, write_phase(3u, 0u, 0xCCCCCCCCu)})
                    .code == MapleResponseCode::FileError,
            "VMU-Schreibschutz wird ignoriert.");
    const auto invalid_block = bus.exchange(
        0u,
        1u,
        {MapleCommand::BlockRead, {vmu_memory_function, location(256u)}});
    require(invalid_block.code == MapleResponseCode::FileError &&
                invalid_block.payload == std::vector<std::uint32_t>({0x04000000u}),
            "VMU akzeptiert einen Block ausserhalb ihres Speichers.");
    require(throws<std::invalid_argument>(
                [] { static_cast<void>(MapleVmuDevice(std::vector<std::uint8_t>(16u))); }),
            "VMU akzeptiert ein falsch grosses Quellabbild.");

    std::cout << "KR-2703 VMU-Minimum erfolgreich.\n";
}
