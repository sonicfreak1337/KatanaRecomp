#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

struct PendingMapping {
    std::string name;
    std::uint32_t base_address = 0u;
    std::shared_ptr<MemoryDevice> device;
    MemoryRegionAccess access = MemoryRegionAccess::ReadWrite;
};

class Vram32BitMemoryDevice final : public MemoryDevice {
  public:
    explicit Vram32BitMemoryDevice(std::shared_ptr<LinearMemoryDevice> backing)
        : backing_(std::move(backing)) {
        if (!backing_ || backing_->size() != dreamcast_vram_size) {
            throw std::invalid_argument("Der 32-Bit-VRAM-Pfad braucht ein 8-MiB-VRAM-Backing.");
        }
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return backing_->size();
    }

    [[nodiscard]] std::uint8_t read_u8(const std::uint32_t offset) const override {
        return backing_->read_u8(dreamcast_vram_32bit_to_linear_offset(offset));
    }

    void write_u8(const std::uint32_t offset, const std::uint8_t value) override {
        backing_->write_u8(dreamcast_vram_32bit_to_linear_offset(offset), value);
    }

  private:
    std::shared_ptr<LinearMemoryDevice> backing_;
};

std::uint32_t main_ram_alias_base(const std::uint32_t area_base, const std::size_t mirror_index) {
    return area_base + static_cast<std::uint32_t>(mirror_index * dreamcast_main_ram_size);
}

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
    return output.str();
}

bool overlaps(const std::uint32_t left_base,
              const std::size_t left_size,
              const std::uint32_t right_base,
              const std::size_t right_size) {
    const std::uint64_t left_start = left_base;
    const std::uint64_t left_end = left_start + left_size;
    const std::uint64_t right_start = right_base;
    const std::uint64_t right_end = right_start + right_size;

    return left_start < right_end && right_start < left_end;
}

void require_free_mappings(const Memory& memory, const std::vector<PendingMapping>& mappings) {
    for (std::size_t index = 0u; index < mappings.size(); ++index) {
        const auto& candidate = mappings[index];

        for (std::size_t region_index = 0u; region_index < memory.region_count(); ++region_index) {
            const auto& existing = memory.region(region_index);
            if (overlaps(candidate.base_address,
                         candidate.device->size(),
                         existing.base_address,
                         existing.size)) {
                throw std::invalid_argument("Dreamcast-Speicherregion '" + candidate.name +
                                            "' bei " + hex_address(candidate.base_address) +
                                            " kollidiert mit Region '" + existing.name + "'.");
            }
        }

        for (std::size_t earlier = 0u; earlier < index; ++earlier) {
            const auto& previous = mappings[earlier];
            if (overlaps(candidate.base_address,
                         candidate.device->size(),
                         previous.base_address,
                         previous.device->size())) {
                throw std::logic_error("Dreamcast-Speicherabbildungen ueberlappen intern.");
            }
        }
    }
}

void map_all(Memory& memory, std::vector<PendingMapping> mappings) {
    require_free_mappings(memory, mappings);
    for (auto& mapping : mappings) {
        memory.map_region(std::move(mapping.name),
                          mapping.base_address,
                          std::move(mapping.device),
                          mapping.access);
    }
}

std::shared_ptr<LinearMemoryDevice> make_firmware_device(const std::size_t expected_size,
                                                         const std::span<const std::uint8_t> image,
                                                         const std::string& description) {
    if (!image.empty() && image.size() != expected_size) {
        throw std::invalid_argument(description + " muss leer oder exakt " +
                                    std::to_string(expected_size) + " Byte gross sein.");
    }

    auto device = std::make_shared<LinearMemoryDevice>(expected_size);
    const auto fill_value = static_cast<std::uint8_t>(0xFFu);

    for (std::size_t index = 0u; index < expected_size; ++index) {
        const auto value = image.empty() ? fill_value : image[index];
        device->write_u8(static_cast<std::uint32_t>(index), value);
    }

    return device;
}

std::vector<PendingMapping> make_direct_mappings(const std::string& name_prefix,
                                                 const std::uint32_t physical_base,
                                                 const std::shared_ptr<MemoryDevice>& device,
                                                 const MemoryRegionAccess access) {
    std::vector<PendingMapping> mappings;
    mappings.reserve(dreamcast_direct_segment_bases.size());

    for (const auto segment_base : dreamcast_direct_segment_bases) {
        const auto base = segment_base + physical_base;
        mappings.push_back(PendingMapping{name_prefix + hex_address(base), base, device, access});
    }

    return mappings;
}

} // namespace

FlashMemoryDevice::FlashMemoryDevice(const std::span<const std::uint8_t> image) {
    if (!image.empty() && image.size() != dreamcast_flash_size) {
        throw std::invalid_argument("Das Dreamcast-Flash-Abbild besitzt nicht exakt 128 KiB.");
    }
    source_.assign(dreamcast_flash_size, 0xFFu);
    if (!image.empty()) {
        source_.assign(image.begin(), image.end());
    }
    working_ = source_;
}

std::size_t FlashMemoryDevice::size() const noexcept {
    return working_.size();
}

void FlashMemoryDevice::check(const std::uint32_t offset) const {
    if (offset >= working_.size()) {
        throw std::out_of_range("Flash-Zugriff ausserhalb des Abbilds.");
    }
}

std::uint8_t FlashMemoryDevice::read_u8(const std::uint32_t offset) const {
    check(offset);
    return working_[offset];
}

[[noreturn]] void FlashMemoryDevice::fail(const char* message) {
    state_ = CommandState::ReadArray;
    throw std::runtime_error(message);
}

void FlashMemoryDevice::write_u8(const std::uint32_t offset, const std::uint8_t value) {
    check(offset);
    if (value == 0xF0u && state_ != CommandState::Program) {
        reset_command_state();
        return;
    }
    switch (state_) {
    case CommandState::ReadArray:
        if (offset == dreamcast_flash_unlock_address_1 && value == 0xAAu) {
            state_ = CommandState::Unlock2;
            return;
        }
        fail("Flash-Schreibzugriff ohne Unlock-Sequenz.");
    case CommandState::Unlock2:
        if (offset == dreamcast_flash_unlock_address_2 && value == 0x55u) {
            state_ = CommandState::Command;
            return;
        }
        fail("Ungueltiger zweiter Flash-Unlock-Schritt.");
    case CommandState::Command:
        if (offset != dreamcast_flash_unlock_address_1) {
            fail("Flash-Kommando an falscher Adresse.");
        }
        if (value == 0xA0u) {
            state_ = CommandState::Program;
            return;
        }
        if (value == 0x80u) {
            state_ = CommandState::EraseUnlock1;
            return;
        }
        fail("Nicht unterstuetztes Flash-Herstellerkommando.");
    case CommandState::Program:
        if (write_protected_) {
            fail("Flash ist schreibgeschuetzt.");
        }
        working_[offset] &= value;
        state_ = CommandState::ReadArray;
        return;
    case CommandState::EraseUnlock1:
        if (offset == dreamcast_flash_unlock_address_1 && value == 0xAAu) {
            state_ = CommandState::EraseUnlock2;
            return;
        }
        fail("Ungueltiger Flash-Erase-Unlock-Schritt.");
    case CommandState::EraseUnlock2:
        if (offset == dreamcast_flash_unlock_address_2 && value == 0x55u) {
            state_ = CommandState::EraseConfirm;
            return;
        }
        fail("Ungueltiger zweiter Flash-Erase-Unlock-Schritt.");
    case CommandState::EraseConfirm:
        if (write_protected_) {
            fail("Flash ist schreibgeschuetzt.");
        }
        if (value != 0x30u) {
            fail("Nicht unterstuetztes Flash-Erase-Kommando.");
        }
        {
            const auto start =
                static_cast<std::size_t>(offset) & ~(dreamcast_flash_sector_size - 1u);
            std::fill_n(working_.begin() + static_cast<std::ptrdiff_t>(start),
                        dreamcast_flash_sector_size,
                        static_cast<std::uint8_t>(0xFFu));
        }
        state_ = CommandState::ReadArray;
        return;
    }
    fail("Ungueltiger Flash-Zustand.");
}

void FlashMemoryDevice::reset_command_state() noexcept {
    state_ = CommandState::ReadArray;
}
void FlashMemoryDevice::set_write_protected(const bool value) noexcept {
    write_protected_ = value;
}
bool FlashMemoryDevice::write_protected() const noexcept {
    return write_protected_;
}
std::uint8_t FlashMemoryDevice::source_byte(const std::uint32_t offset) const {
    check(offset);
    return source_[offset];
}

std::shared_ptr<LinearMemoryDevice> map_dreamcast_main_ram(Memory& memory) {
    auto main_ram = std::make_shared<LinearMemoryDevice>(dreamcast_main_ram_size);

    std::vector<PendingMapping> mappings;
    mappings.reserve(dreamcast_main_ram_alias_count);

    for (const auto area_base : dreamcast_main_ram_area_bases) {
        for (std::size_t mirror_index = 0u; mirror_index < dreamcast_main_ram_mirrors_per_area;
             ++mirror_index) {
            const auto base = main_ram_alias_base(area_base, mirror_index);
            mappings.push_back(
                PendingMapping{"dreamcast-main-ram-" + hex_address(base), base, main_ram});
        }
    }

    map_all(memory, std::move(mappings));
    return main_ram;
}

std::shared_ptr<LinearMemoryDevice> map_dreamcast_vram(Memory& memory) {
    auto vram = std::make_shared<LinearMemoryDevice>(dreamcast_vram_size);
    auto path_32bit = std::make_shared<Vram32BitMemoryDevice>(vram);

    std::vector<PendingMapping> mappings;
    mappings.reserve(dreamcast_vram_alias_count);

    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_vram_64bit_physical_bases) {
            const auto base = segment_base + physical_base;
            mappings.push_back(
                PendingMapping{"dreamcast-vram-64bit-" + hex_address(base), base, vram});
        }
        for (const auto physical_base : dreamcast_vram_32bit_physical_bases) {
            const auto base = segment_base + physical_base;
            mappings.push_back(
                PendingMapping{"dreamcast-vram-32bit-" + hex_address(base), base, path_32bit});
        }
    }

    map_all(memory, std::move(mappings));
    return vram;
}

std::shared_ptr<LinearMemoryDevice> map_dreamcast_aica_ram(Memory& memory) {
    auto aica_ram = std::make_shared<LinearMemoryDevice>(dreamcast_aica_ram_size);

    std::vector<PendingMapping> mappings;
    mappings.reserve(dreamcast_aica_ram_alias_count);

    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_aica_ram_physical_bases) {
            const auto base = segment_base + physical_base;
            mappings.push_back(
                PendingMapping{"dreamcast-aica-ram-" + hex_address(base), base, aica_ram});
        }
    }

    map_all(memory, std::move(mappings));
    return aica_ram;
}

std::shared_ptr<LinearMemoryDevice> map_dreamcast_bios(Memory& memory,
                                                       const std::span<const std::uint8_t> image) {
    auto bios = make_firmware_device(dreamcast_bios_size, image, "Das Dreamcast-BIOS-Abbild");

    auto mappings = make_direct_mappings(
        "dreamcast-bios-", dreamcast_bios_physical_base, bios, MemoryRegionAccess::ReadOnly);

    map_all(memory, std::move(mappings));
    return bios;
}

std::shared_ptr<LinearMemoryDevice> map_dreamcast_flash(Memory& memory,
                                                        const std::span<const std::uint8_t> image) {
    auto flash = make_firmware_device(dreamcast_flash_size, image, "Das Dreamcast-Flash-Abbild");

    auto mappings = make_direct_mappings(
        "dreamcast-flash-", dreamcast_flash_physical_base, flash, MemoryRegionAccess::ReadWrite);

    map_all(memory, std::move(mappings));
    return flash;
}

std::shared_ptr<FlashMemoryDevice>
map_dreamcast_command_flash(Memory& memory, const std::span<const std::uint8_t> image) {
    auto flash = std::make_shared<FlashMemoryDevice>(image);
    auto mappings = make_direct_mappings("dreamcast-command-flash-",
                                         dreamcast_flash_physical_base,
                                         flash,
                                         MemoryRegionAccess::ReadWrite);
    map_all(memory, std::move(mappings));
    return flash;
}

} // namespace katana::runtime
