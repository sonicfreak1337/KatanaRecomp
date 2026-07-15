#include "katana/runtime/maple.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) { std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<typename E, typename F> bool throws(F&& f) { try { f(); } catch (const E&) { return true; } return false; }
}

int main() {
    using namespace katana::runtime;
    std::vector<std::uint8_t> source(vmu_storage_size, 0xFFu);
    source[0] = 0x12u;
    auto vmu = std::make_shared<MapleVmuDevice>(source);
    MapleBus bus;
    bus.attach(0u, 1u, vmu);

    const auto info = bus.exchange(0u, 1u, {MapleCommand::DeviceRequest, {}});
    require(info.code == MapleResponseCode::DeviceInfo && info.payload[0] == 0x02000000u,
        "VMU meldet die Memory-Function nicht.");
    const auto initial = bus.exchange(0u, 1u, {MapleCommand::BlockRead, {0u}});
    require(initial.payload.size() == 130u && (initial.payload[2] & 0xFFu) == 0x12u,
        "VMU-Blocklesen ist nicht vollstaendig oder Little Endian.");

    std::vector<std::uint32_t> write_payload(1u + vmu_block_size / 4u, 0xA5A5A5A5u);
    write_payload[0] = 1u;
    const auto ack = bus.exchange(0u, 1u, {MapleCommand::BlockWrite, write_payload});
    require(ack.code == MapleResponseCode::Ack && vmu->read_byte(vmu_block_size) == 0xA5u,
        "VMU-Blockschreiben wird nicht bestaetigt oder gespeichert.");
    require(source[vmu_block_size] == 0xFFu && vmu->source_byte(vmu_block_size) == 0xFFu,
        "VMU veraendert ihr Quellabbild statt einer Arbeitskopie.");

    vmu->set_write_protected(true);
    require(throws<std::runtime_error>([&] { static_cast<void>(bus.exchange(0u, 1u, {MapleCommand::BlockWrite, write_payload})); }),
        "VMU-Schreibschutz wird ignoriert.");
    require(throws<std::out_of_range>([&] { static_cast<void>(bus.exchange(0u, 1u, {MapleCommand::BlockRead, {256u}})); }),
        "VMU akzeptiert einen Block ausserhalb ihres Speichers.");
    require(throws<std::invalid_argument>([] { static_cast<void>(MapleVmuDevice(std::vector<std::uint8_t>(16u))); }),
        "VMU akzeptiert ein falsch grosses Quellabbild.");

    std::cout << "KR-2703 VMU-Minimum erfolgreich.\n";
}
