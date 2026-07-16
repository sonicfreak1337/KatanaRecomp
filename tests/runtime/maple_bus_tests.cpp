#include "katana/runtime/maple.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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
class EchoDevice final : public katana::runtime::MapleDevice {
  public:
    katana::runtime::MapleResponse transact(const katana::runtime::MapleRequest& request) override {
        return {katana::runtime::MapleResponseCode::DataTransfer, request.payload};
    }
};
} // namespace

int main() {
    using namespace katana::runtime;
    MapleBus bus;
    bus.attach(0u, 0u, std::make_shared<EchoDevice>());
    bus.attach(3u, 5u, std::make_shared<EchoDevice>());
    require(bus.attached(0u, 0u) && bus.attached(3u, 5u),
            "Maple-Randadressen sind nicht belegbar.");
    require(
        throws<std::invalid_argument>([&] { bus.attach(0u, 0u, std::make_shared<EchoDevice>()); }),
        "Doppelte Maple-Adresse wird nicht abgewiesen.");
    require(throws<std::out_of_range>([&] { static_cast<void>(bus.attached(4u, 0u)); }),
            "Ungueltiger Maple-Port wird nicht abgewiesen.");
    require(throws<std::runtime_error>([&] { static_cast<void>(bus.exchange(1u, 0u, {})); }),
            "Fehlendes Maple-Geraet erzeugt keinen sichtbaren Fehler.");

    const auto first = bus.exchange(0u, 0u, {MapleCommand::GetCondition, {1u, 2u}});
    const auto second = bus.exchange(3u, 5u, {MapleCommand::BlockRead, {3u}});
    require(first.payload == std::vector<std::uint32_t>({1u, 2u}) && second.payload[0] == 3u,
            "Maple-Payload geht auf dem Bus verloren.");
    require(bus.history().size() == 2u && bus.history()[0].sequence == 1u &&
                bus.history()[1].sequence == 2u && bus.history()[1].port == 3u,
            "Maple-Transaktionshistorie ist nicht deterministisch.");

    std::cout << "KR-2701 Maple-Bus erfolgreich.\n";
}
