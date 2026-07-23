#include "katana/runtime/firmware_handoff.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace katana::runtime {
namespace {
bool contains(const std::uint32_t start,
              const std::uint32_t size,
              const std::uint32_t address) noexcept {
    return address >= start &&
           static_cast<std::uint64_t>(address) < static_cast<std::uint64_t>(start) + size;
}
void validate_range(const std::uint32_t start, const std::uint32_t size, const char* what) {
    if (size == 0u ||
        static_cast<std::uint64_t>(start) + size >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::invalid_argument(std::string(what) +
                                    " besitzt einen ungueltigen 32-Bit-Bereich.");
    }
}
} // namespace

void FirmwareHandoffMap::map_segment(FirmwareMapping mapping) {
    if (mapping.name.empty()) {
        throw std::invalid_argument("Firmwaresegment braucht einen Namen.");
    }
    validate_range(mapping.virtual_start, mapping.size, "Firmwaresegment");
    validate_range(mapping.physical_start, mapping.size, "Firmwaresegment");
    mapping.physical_start = canonical_physical_address(mapping.physical_start);
    const auto duplicate = std::find_if(mappings_.begin(), mappings_.end(), [&](const auto& value) {
        return value.virtual_start == mapping.virtual_start && value.size == mapping.size;
    });
    if (duplicate != mappings_.end()) {
        throw std::invalid_argument("Doppelte virtuelle Firmwareabbildung.");
    }
    mappings_.push_back(std::move(mapping));
}

void FirmwareHandoffMap::record_copy(FirmwareCodeCopy copy) {
    if (copy.provenance.empty()) {
        throw std::invalid_argument("Codekopie braucht Provenienz.");
    }
    validate_range(copy.source_physical, copy.size, "Codekopie");
    validate_range(copy.destination_physical, copy.size, "Codekopie");
    copy.source_physical = canonical_physical_address(copy.source_physical);
    copy.destination_physical = canonical_physical_address(copy.destination_physical);
    copies_.push_back(std::move(copy));
}

void FirmwareHandoffMap::mark_copy_changed(const std::uint32_t destination_address) {
    const auto canonical = canonical_physical_address(destination_address);
    bool found = false;
    for (auto& copy : copies_) {
        if (contains(copy.destination_physical, copy.size, canonical)) {
            copy.changed_after_copy = true;
            found = true;
        }
    }
    if (!found) {
        throw std::out_of_range("Adresse gehoert zu keiner bekannten Firmware-Codekopie.");
    }
}

void FirmwareHandoffMap::install_runtime_symbol(RuntimeFirmwareSymbol symbol) {
    if (symbol.name.empty() || symbol.provenance.empty()) {
        throw std::invalid_argument("Dynamischer BIOS-ABI-Vektor braucht Name und Provenienz.");
    }
    symbol.physical_address = canonical_physical_address(symbol.physical_address);
    const auto duplicate = std::find_if(symbols_.begin(), symbols_.end(), [&](const auto& value) {
        return value.virtual_address == symbol.virtual_address || value.name == symbol.name;
    });
    if (duplicate != symbols_.end())
        throw std::invalid_argument("Doppelter dynamischer BIOS-ABI-Vektor.");
    symbols_.push_back(std::move(symbol));
}

FirmwareTargetResolution FirmwareHandoffMap::resolve(const std::uint32_t virtual_address) const {
    const auto physical = canonical_physical_address(virtual_address);
    for (const auto& copy : copies_) {
        if (contains(copy.destination_physical, copy.size, physical)) {
            return {virtual_address,
                    physical,
                    copy,
                    copy.byte_verified && !copy.changed_after_copy,
                    copy.provenance};
        }
    }
    const auto symbol = std::find_if(symbols_.begin(), symbols_.end(), [&](const auto& value) {
        return value.virtual_address == virtual_address || value.physical_address == physical;
    });
    if (symbol != symbols_.end())
        return {virtual_address, physical, std::nullopt, true, symbol->provenance};
    const auto mapping = std::find_if(mappings_.begin(), mappings_.end(), [&](const auto& value) {
        return contains(value.virtual_start, value.size, virtual_address) ||
               contains(value.physical_start, value.size, physical);
    });
    if (mapping == mappings_.end()) {
        throw std::out_of_range("Firmwareziel liegt in keinem bekannten Segment.");
    }
    return {virtual_address,
            physical,
            std::nullopt,
            mapping->kind == FirmwareSegmentKind::Rom,
            mapping->name};
}

const std::vector<RuntimeFirmwareSymbol>& FirmwareHandoffMap::runtime_symbols() const noexcept {
    return symbols_;
}

const std::vector<FirmwareMapping>& FirmwareHandoffMap::mappings() const noexcept {
    return mappings_;
}

const std::vector<FirmwareCodeCopy>& FirmwareHandoffMap::copies() const noexcept {
    return copies_;
}

std::size_t FirmwareHandoffMap::canonical_origin_count() const noexcept {
    std::set<std::pair<std::uint32_t, std::uint32_t>> origins;
    for (const auto& mapping : mappings_) {
        origins.emplace(mapping.physical_start, mapping.size);
    }
    return origins.size();
}

FirmwareHandoffSnapshot FirmwareHandoffMap::snapshot() const {
    return {
        mappings_,
        copies_,
        symbols_,
        canonical_origin_count(),
    };
}

const char* firmware_segment_kind_name(const FirmwareSegmentKind value) noexcept {
    switch (value) {
    case FirmwareSegmentKind::Rom:
        return "rom";
    case FirmwareSegmentKind::Ram:
        return "ram";
    case FirmwareSegmentKind::Flash:
        return "flash";
    case FirmwareSegmentKind::Mmio:
        return "mmio";
    }
    return "unknown";
}

} // namespace katana::runtime
