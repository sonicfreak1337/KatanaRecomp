#pragma once

#include "katana/runtime/block_table.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace katana::runtime {

enum class FirmwareSegmentKind : std::uint8_t { Rom, Ram, Flash, Mmio };

struct FirmwareMapping {
    std::string name;
    FirmwareSegmentKind kind = FirmwareSegmentKind::Rom;
    std::uint32_t virtual_start = 0u;
    std::uint32_t physical_start = 0u;
    std::uint32_t size = 0u;
};

struct FirmwareCodeCopy {
    std::uint32_t source_physical = 0u;
    std::uint32_t destination_physical = 0u;
    std::uint32_t size = 0u;
    std::string provenance;
    bool byte_verified = false;
    bool changed_after_copy = false;
};

struct RuntimeFirmwareSymbol {
    std::string name;
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;
    std::string provenance;
};

struct FirmwareTargetResolution {
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;
    std::optional<FirmwareCodeCopy> copy;
    bool statically_proven = false;
    std::string provenance;
};

class FirmwareHandoffMap {
public:
    void map_segment(FirmwareMapping mapping);
    void record_copy(FirmwareCodeCopy copy);
    void mark_copy_changed(std::uint32_t destination_address);
    void install_runtime_symbol(RuntimeFirmwareSymbol symbol);
    [[nodiscard]] FirmwareTargetResolution resolve(std::uint32_t virtual_address) const;
    [[nodiscard]] const std::vector<RuntimeFirmwareSymbol>& runtime_symbols() const noexcept;
    [[nodiscard]] std::size_t canonical_origin_count() const noexcept;

private:
    std::vector<FirmwareMapping> mappings_;
    std::vector<FirmwareCodeCopy> copies_;
    std::vector<RuntimeFirmwareSymbol> symbols_;
};

} // namespace katana::runtime
