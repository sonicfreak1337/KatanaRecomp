#include "katana/runtime/maple.hpp"

#include <stdexcept>
#include <utility>

namespace katana::runtime {

std::size_t MapleBus::slot(const std::uint8_t port, const std::uint8_t unit) {
    if (port >= maple_port_count || unit >= maple_units_per_port) {
        throw std::out_of_range("Maple-Port oder -Unit liegt ausserhalb des Busses.");
    }
    return static_cast<std::size_t>(port) * maple_units_per_port + unit;
}

void MapleBus::attach(
    const std::uint8_t port,
    const std::uint8_t unit,
    std::shared_ptr<MapleDevice> device
) {
    if (!device) {
        throw std::invalid_argument("Ein Maple-Geraet darf nicht null sein.");
    }
    auto& target = devices_[slot(port, unit)];
    if (target) {
        throw std::invalid_argument("Maple-Port und -Unit sind bereits belegt.");
    }
    target = std::move(device);
}

bool MapleBus::attached(const std::uint8_t port, const std::uint8_t unit) const {
    return static_cast<bool>(devices_[slot(port, unit)]);
}

MapleResponse MapleBus::exchange(
    const std::uint8_t port,
    const std::uint8_t unit,
    const MapleRequest& request
) {
    auto& device = devices_[slot(port, unit)];
    if (!device) {
        throw std::runtime_error("Kein Maple-Geraet an der angeforderten Adresse.");
    }
    auto response = device->transact(request);
    history_.push_back(MapleTransactionRecord{
        next_sequence_++, port, unit, request.command, response.code
    });
    return response;
}

std::span<const MapleTransactionRecord> MapleBus::history() const noexcept {
    return history_;
}

} // namespace katana::runtime
