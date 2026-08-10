#include "katana/runtime/dreamcast_memory.hpp"

#include "katana/runtime/system_asic.hpp"

#include <algorithm>
#include <cassert>
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

    [[nodiscard]] std::uint16_t read_u16(const std::uint32_t offset) const override {
        return backing_->read_u16(dreamcast_vram_32bit_to_linear_offset(offset));
    }

    [[nodiscard]] std::uint32_t read_u32(const std::uint32_t offset) const override {
        return backing_->read_u32(dreamcast_vram_32bit_to_linear_offset(offset));
    }

    void write_u8(const std::uint32_t offset, const std::uint8_t value) override {
        backing_->write_u8(dreamcast_vram_32bit_to_linear_offset(offset), value);
    }

    void write_u16(const std::uint32_t offset, const std::uint16_t value) override {
        backing_->write_u16(dreamcast_vram_32bit_to_linear_offset(offset), value);
    }

    void write_u32(const std::uint32_t offset, const std::uint32_t value) override {
        backing_->write_u32(dreamcast_vram_32bit_to_linear_offset(offset), value);
    }

    [[nodiscard]] LinearMemoryProjection
    linear_projection(const std::uint32_t offset,
                      const MemoryAccessWidth width) const noexcept override {
        const auto byte_count = static_cast<std::uint8_t>(width);
        if (offset > size() || byte_count > size() - offset) return {};

        LinearMemoryProjection projection;
        projection.backing = backing_.get();
        projection.byte_count = byte_count;
        projection.contiguous = true;
        for (std::uint8_t index = 0u; index < byte_count; ++index) {
            projection.byte_offsets[index] =
                dreamcast_vram_32bit_to_linear_offset(offset + index);
            if (index != 0u &&
                projection.byte_offsets[index] != projection.byte_offsets[index - 1u] + 1u) {
                projection.contiguous = false;
            }
        }
        return projection;
    }

  private:
    std::shared_ptr<LinearMemoryDevice> backing_;
};

enum class TaVramPath : std::uint8_t { Path0, Path1 };

class TaVramMemoryDevice final : public MemoryDevice {
  public:
    TaVramMemoryDevice(std::shared_ptr<LinearMemoryDevice> backing,
                       std::shared_ptr<DreamcastSystemBusControl> system_bus_control,
                       const TaVramPath path)
        : backing_(std::move(backing)),
          system_bus_control_(std::move(system_bus_control)),
          path_(path) {
        if (!backing_ || backing_->size() != dreamcast_vram_size) {
            throw std::invalid_argument(
                "Der Area-4-VRAM-Pfad braucht ein 8-MiB-VRAM-Backing.");
        }
        if (!system_bus_control_) {
            throw std::invalid_argument(
                "Der Area-4-VRAM-Pfad braucht die Systembus-LMMODE-Register.");
        }
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return backing_->size();
    }

    [[nodiscard]] std::uint8_t read_u8(const std::uint32_t) const override {
        throw std::runtime_error(
            "Der Area-4-TA-VRAM-Pfad ist ein schreibbarer Hardwarepfad.");
    }

    void validate_write(const std::uint32_t offset,
                        const std::size_t size,
                        const CodeWriteSource source) const override {
        const auto admitted_origin =
            source == CodeWriteSource::Dma ||
            source == CodeWriteSource::StoreQueue;
        if (!admitted_origin || size != 32u || (offset & 31u) != 0u) {
            throw std::invalid_argument(
                "Der Area-4-TA-VRAM-Pfad akzeptiert nur ausgerichtete "
                "32-Byte-DMA- oder Store-Queue-Schreibpakete.");
        }
    }

    void write_u8(const std::uint32_t offset, const std::uint8_t value) override {
        backing_->write_u8(backing_offset(offset), value);
    }

    [[nodiscard]] LinearMemoryProjection
    linear_projection(const std::uint32_t offset,
                      const MemoryAccessWidth width) const noexcept override {
        const auto byte_count = static_cast<std::uint8_t>(width);
        if (offset > size() || byte_count > size() - offset) return {};

        const auto projected = uses_32bit_path();
        LinearMemoryProjection projection;
        projection.backing = backing_.get();
        projection.byte_count = byte_count;
        projection.contiguous = true;
        for (std::uint8_t index = 0u; index < byte_count; ++index) {
            projection.byte_offsets[index] =
                projected
                    ? dreamcast_vram_32bit_to_linear_offset(offset + index)
                    : offset + index;
            if (index != 0u &&
                projection.byte_offsets[index] !=
                    projection.byte_offsets[index - 1u] + 1u) {
                projection.contiguous = false;
            }
        }
        return projection;
    }

  private:
    [[nodiscard]] bool uses_32bit_path() const noexcept {
        return path_ == TaVramPath::Path0
                   ? system_bus_control_->texture_memory_mode0_uses_32bit_path()
                   : system_bus_control_->texture_memory_mode1_uses_32bit_path();
    }

    [[nodiscard]] std::uint32_t backing_offset(const std::uint32_t offset) const noexcept {
        return uses_32bit_path()
                   ? dreamcast_vram_32bit_to_linear_offset(offset)
                   : offset;
    }

    std::shared_ptr<LinearMemoryDevice> backing_;
    std::shared_ptr<DreamcastSystemBusControl> system_bus_control_;
    TaVramPath path_ = TaVramPath::Path0;
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

FlashMemoryDevice::FlashMemoryDevice(std::shared_ptr<PersistentImage> image)
    : persistent_image_(std::move(image)) {
    if (!persistent_image_ || persistent_image_->size() != dreamcast_flash_size)
        throw std::invalid_argument("Persistente Flash-Arbeitskopie besitzt nicht exakt 128 KiB.");
}

std::size_t FlashMemoryDevice::size() const noexcept {
    return persistent_image_ ? persistent_image_->size() : working_.size();
}

void FlashMemoryDevice::check(const std::uint32_t offset) const {
    if (offset >= size()) {
        throw std::out_of_range("Flash-Zugriff ausserhalb des Abbilds.");
    }
}

std::uint8_t FlashMemoryDevice::read_u8(const std::uint32_t offset) const {
    check(offset);
    return working_byte(offset);
}

std::uint8_t FlashMemoryDevice::working_byte(const std::uint32_t offset) const {
    return persistent_image_ ? persistent_image_->read_byte(offset) : working_[offset];
}

void FlashMemoryDevice::set_working_byte(const std::uint32_t offset, const std::uint8_t value) {
    if (persistent_image_)
        persistent_image_->write_byte(offset, value);
    else
        working_[offset] = value;
}

[[noreturn]] void FlashMemoryDevice::fail(const char* message) {
    state_ = FlashCommandState::ReadArray;
    throw std::runtime_error(message);
}

void FlashMemoryDevice::write_u8(const std::uint32_t offset, const std::uint8_t value) {
    check(offset);
    if (value == 0xF0u && state_ != FlashCommandState::Program) {
        reset_command_state();
        return;
    }
    switch (state_) {
    case FlashCommandState::ReadArray:
        if (offset == dreamcast_flash_unlock_address_1 && value == 0xAAu) {
            state_ = FlashCommandState::Unlock2;
            return;
        }
        fail("Flash-Schreibzugriff ohne Unlock-Sequenz.");
    case FlashCommandState::Unlock2:
        if (offset == dreamcast_flash_unlock_address_2 && value == 0x55u) {
            state_ = FlashCommandState::Command;
            return;
        }
        fail("Ungueltiger zweiter Flash-Unlock-Schritt.");
    case FlashCommandState::Command:
        if (offset != dreamcast_flash_unlock_address_1) {
            fail("Flash-Kommando an falscher Adresse.");
        }
        if (value == 0xA0u) {
            state_ = FlashCommandState::Program;
            return;
        }
        if (value == 0x80u) {
            state_ = FlashCommandState::EraseUnlock1;
            return;
        }
        fail("Nicht unterstuetztes Flash-Herstellerkommando.");
    case FlashCommandState::Program:
        if (write_protected_) {
            fail("Flash ist schreibgeschuetzt.");
        }
        set_working_byte(offset, static_cast<std::uint8_t>(working_byte(offset) & value));
        state_ = FlashCommandState::ReadArray;
        return;
    case FlashCommandState::EraseUnlock1:
        if (offset == dreamcast_flash_unlock_address_1 && value == 0xAAu) {
            state_ = FlashCommandState::EraseUnlock2;
            return;
        }
        fail("Ungueltiger Flash-Erase-Unlock-Schritt.");
    case FlashCommandState::EraseUnlock2:
        if (offset == dreamcast_flash_unlock_address_2 && value == 0x55u) {
            state_ = FlashCommandState::EraseConfirm;
            return;
        }
        fail("Ungueltiger zweiter Flash-Erase-Unlock-Schritt.");
    case FlashCommandState::EraseConfirm:
        if (write_protected_) {
            fail("Flash ist schreibgeschuetzt.");
        }
        if (value != 0x30u) {
            fail("Nicht unterstuetztes Flash-Erase-Kommando.");
        }
        {
            const auto start =
                static_cast<std::size_t>(offset) & ~(dreamcast_flash_sector_size - 1u);
            if (persistent_image_) {
                const std::vector<std::uint8_t> erased(dreamcast_flash_sector_size, 0xFFu);
                persistent_image_->write(start, erased);
            } else {
                std::fill_n(working_.begin() + static_cast<std::ptrdiff_t>(start),
                            dreamcast_flash_sector_size,
                            static_cast<std::uint8_t>(0xFFu));
            }
        }
        state_ = FlashCommandState::ReadArray;
        return;
    }
    fail("Ungueltiger Flash-Zustand.");
}

void FlashMemoryDevice::reset_command_state() noexcept {
    state_ = FlashCommandState::ReadArray;
}
void FlashMemoryDevice::set_write_protected(const bool value) noexcept {
    write_protected_ = value;
}
bool FlashMemoryDevice::write_protected() const noexcept {
    return write_protected_;
}
std::uint8_t FlashMemoryDevice::source_byte(const std::uint32_t offset) const {
    check(offset);
    return persistent_image_ ? persistent_image_->source_byte(offset) : source_[offset];
}
void FlashMemoryDevice::save_working_copy() {
    if (!persistent_image_) throw std::logic_error("Flash besitzt keine persistente Arbeitskopie.");
    persistent_image_->save();
}
bool FlashMemoryDevice::working_copy_dirty() const noexcept {
    return persistent_image_ && persistent_image_->dirty();
}
bool FlashMemoryDevice::persistent_working_copy() const noexcept {
    return persistent_image_ != nullptr;
}

FlashMemorySnapshot FlashMemoryDevice::snapshot() const {
    std::vector<std::uint8_t> source(size());
    std::vector<std::uint8_t> working(size());
    for (std::size_t offset = 0u; offset < size(); ++offset) {
        source[offset] = persistent_image_
                             ? persistent_image_->source_byte(offset)
                             : source_[offset];
        working[offset] = persistent_image_
                              ? persistent_image_->read_byte(offset)
                              : working_[offset];
    }
    return {
        size(),
        state_,
        write_protected_,
        working_copy_dirty(),
        persistent_working_copy(),
        std::move(source),
        std::move(working),
    };
}

void FlashMemoryDevice::validate_state_restore(
    const FlashMemorySnapshot& state) const {
    static_cast<void>(prepare_state_restore(
        state, PersistenceHandoffPolicy::DiagnosticLossless));
}

PreparedFlashMemoryRestore FlashMemoryDevice::prepare_state_restore(
    const FlashMemorySnapshot& state,
    const PersistenceHandoffPolicy policy) const {
    const auto valid_command_state =
        state.command_state == FlashCommandState::ReadArray ||
        state.command_state == FlashCommandState::Unlock2 ||
        state.command_state == FlashCommandState::Command ||
        state.command_state == FlashCommandState::Program ||
        state.command_state == FlashCommandState::EraseUnlock1 ||
        state.command_state == FlashCommandState::EraseUnlock2 ||
        state.command_state == FlashCommandState::EraseConfirm;
    if (!valid_command_state || state.size != size() ||
        state.size != dreamcast_flash_size ||
        state.source_bytes.size() != state.size ||
        state.working_bytes.size() != state.size ||
        (!state.persistent_working_copy && state.working_copy_dirty))
        throw std::invalid_argument(
            "Flash-Handoff besitzt einen inkompatiblen Zustandsvertrag.");

    PreparedFlashMemoryRestore prepared;
    prepared.owner_ = this;
    prepared.command_state_ = state.command_state;
    switch (policy) {
    case PersistenceHandoffPolicy::DiagnosticLossless:
        if (state.persistent_working_copy != persistent_working_copy())
            throw std::invalid_argument(
                "Flash-Handoff und Runtime besitzen unterschiedliche Persistenzvertraege.");
        prepared.write_protected_ = state.write_protected;
        prepared.replace_write_protection_ = true;
        prepared.replace_working_copy_ = true;
        break;
    case PersistenceHandoffPolicy::ProductPreserveTarget:
        // Installed bytes, dirty bookkeeping and host write protection remain
        // authoritative. Only guest-visible command progress is transferred.
        break;
    default:
        throw std::invalid_argument("Unbekannte Persistenz-Handoff-Policy.");
    }

    if (persistent_image_) {
        prepared.persistent_.emplace(
            persistent_image_->prepare_working_copy_restore(
                state.source_bytes,
                state.working_bytes,
                state.working_copy_dirty,
                policy));
    } else if (!std::equal(
                   state.source_bytes.begin(),
                   state.source_bytes.end(),
                   source_.begin())) {
        throw std::invalid_argument(
            "Flash-Handoff passt nicht zur installierten Quellidentitaet.");
    } else if (prepared.replace_working_copy_) {
        prepared.working_ = state.working_bytes;
    }
    return prepared;
}

void FlashMemoryDevice::commit_prepared_state_restore(
    PreparedFlashMemoryRestore prepared) noexcept {
    assert(prepared.owner_ == this);
    if (prepared.persistent_) {
        persistent_image_->commit_prepared_working_copy_restore(
            std::move(*prepared.persistent_));
    } else if (prepared.replace_working_copy_) {
        working_.swap(prepared.working_);
    }
    state_ = prepared.command_state_;
    if (prepared.replace_write_protection_)
        write_protected_ = prepared.write_protected_;
}

void FlashMemoryDevice::restore_state_passive(
    const FlashMemorySnapshot& state) {
    auto prepared = prepare_state_restore(
        state, PersistenceHandoffPolicy::DiagnosticLossless);
    commit_prepared_state_restore(std::move(prepared));
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
    constexpr auto physical_main_ram_base = dreamcast_main_ram_area_bases.front();
    constexpr auto physical_main_ram_span =
        static_cast<std::uint32_t>(dreamcast_main_ram_size *
                                   dreamcast_main_ram_mirrors_per_area);
    memory.bind_direct_linear_alias_window(
        physical_main_ram_base, physical_main_ram_span, *main_ram);
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

void map_dreamcast_ta_vram_aliases(Memory& memory,
                                    const std::shared_ptr<LinearMemoryDevice>& vram,
                                    const std::shared_ptr<DreamcastSystemBusControl>&
                                        system_bus_control) {
    if (!vram || vram->size() != dreamcast_vram_size) {
        throw std::invalid_argument("Die TA-VRAM-Aliase brauchen das gemeinsame 8-MiB-VRAM.");
    }
    if (!system_bus_control) {
        throw std::invalid_argument(
            "Die TA-VRAM-Aliase brauchen die Systembus-LMMODE-Register.");
    }
    auto path0 = std::make_shared<TaVramMemoryDevice>(
        vram, system_bus_control, TaVramPath::Path0);
    auto path1 = std::make_shared<TaVramMemoryDevice>(
        vram, system_bus_control, TaVramPath::Path1);
    std::vector<PendingMapping> mappings;
    mappings.reserve(dreamcast_direct_segment_bases.size() * 4u);
    for (const auto segment_base : dreamcast_direct_segment_bases) {
        const auto path0_base = segment_base + 0x11000000u;
        const auto path0_mirror = segment_base + 0x11800000u;
        const auto path1_base = segment_base + 0x13000000u;
        const auto path1_mirror = segment_base + 0x13800000u;
        mappings.push_back(PendingMapping{
            "dreamcast-ta-vram-path0-" + hex_address(path0_base), path0_base, path0});
        mappings.push_back(PendingMapping{
            "dreamcast-ta-vram-path0-" + hex_address(path0_mirror), path0_mirror, path0});
        mappings.push_back(PendingMapping{
            "dreamcast-ta-vram-path1-" + hex_address(path1_base), path1_base, path1});
        mappings.push_back(PendingMapping{
            "dreamcast-ta-vram-path1-" + hex_address(path1_mirror), path1_mirror, path1});
    }
    map_all(memory, std::move(mappings));
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

std::shared_ptr<FlashMemoryDevice>
map_dreamcast_command_flash(Memory& memory, std::shared_ptr<PersistentImage> image) {
    auto flash = std::make_shared<FlashMemoryDevice>(std::move(image));
    auto mappings = make_direct_mappings("dreamcast-command-flash-",
                                         dreamcast_flash_physical_base,
                                         flash,
                                         MemoryRegionAccess::ReadWrite);
    map_all(memory, std::move(mappings));
    return flash;
}

} // namespace katana::runtime
