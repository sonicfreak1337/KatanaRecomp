#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/persistent_storage.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace katana::runtime {

inline constexpr std::size_t dreamcast_main_ram_size = 0x01000000u;
inline constexpr std::size_t dreamcast_main_ram_mirrors_per_area = 4u;
inline constexpr std::size_t dreamcast_vram_size = 0x00800000u;
inline constexpr std::size_t dreamcast_aica_ram_size = 0x00200000u;
inline constexpr std::size_t dreamcast_bios_size = 0x00200000u;
inline constexpr std::size_t dreamcast_flash_size = 0x00020000u;
inline constexpr std::uint32_t dreamcast_vram_bank_size = 0x00400000u;
inline constexpr std::uint32_t dreamcast_bios_physical_base = 0x00000000u;
inline constexpr std::uint32_t dreamcast_flash_physical_base = 0x00200000u;
inline constexpr std::size_t dreamcast_flash_sector_size = 0x00001000u;
inline constexpr std::uint32_t dreamcast_flash_unlock_address_1 = 0x00005555u;
inline constexpr std::uint32_t dreamcast_flash_unlock_address_2 = 0x00002AAAu;

class FlashMemoryDevice final : public MemoryDevice {
  public:
    explicit FlashMemoryDevice(std::span<const std::uint8_t> image = {});
    explicit FlashMemoryDevice(std::shared_ptr<PersistentImage> image);
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::uint8_t read_u8(std::uint32_t offset) const override;
    void write_u8(std::uint32_t offset, std::uint8_t value) override;
    void reset_command_state() noexcept;
    void set_write_protected(bool protected_state) noexcept;
    [[nodiscard]] bool write_protected() const noexcept;
    [[nodiscard]] std::uint8_t source_byte(std::uint32_t offset) const;
    void save_working_copy();
    [[nodiscard]] bool working_copy_dirty() const noexcept;
    [[nodiscard]] bool persistent_working_copy() const noexcept;

  private:
    enum class CommandState : std::uint8_t {
        ReadArray,
        Unlock2,
        Command,
        Program,
        EraseUnlock1,
        EraseUnlock2,
        EraseConfirm
    };
    void check(std::uint32_t offset) const;
    [[nodiscard]] std::uint8_t working_byte(std::uint32_t offset) const;
    void set_working_byte(std::uint32_t offset, std::uint8_t value);
    [[noreturn]] void fail(const char* message);
    std::vector<std::uint8_t> source_;
    std::vector<std::uint8_t> working_;
    std::shared_ptr<PersistentImage> persistent_image_;
    CommandState state_ = CommandState::ReadArray;
    bool write_protected_ = false;
};

[[nodiscard]] constexpr std::uint32_t
dreamcast_vram_32bit_to_linear_offset(const std::uint32_t offset) noexcept {
    constexpr std::uint32_t bytes_per_word = 4u;

    const auto bank = offset / dreamcast_vram_bank_size;
    const auto offset_in_bank = offset % dreamcast_vram_bank_size;
    const auto word_in_bank = offset_in_bank / bytes_per_word;
    const auto byte_in_word = offset_in_bank % bytes_per_word;

    return ((word_in_bank * 2u + bank) * bytes_per_word) + byte_in_word;
}

inline constexpr std::array<std::uint32_t, 7> dreamcast_direct_segment_bases = {
    0x00000000u, 0x20000000u, 0x40000000u, 0x60000000u, 0x80000000u, 0xA0000000u, 0xC0000000u};

inline constexpr std::array<std::uint32_t, 7> dreamcast_main_ram_area_bases = {
    0x0C000000u, 0x2C000000u, 0x4C000000u, 0x6C000000u, 0x8C000000u, 0xAC000000u, 0xCC000000u};

inline constexpr std::array<std::uint32_t, 4> dreamcast_vram_64bit_physical_bases = {
    0x04000000u, 0x04800000u, 0x06000000u, 0x06800000u};

inline constexpr std::array<std::uint32_t, 4> dreamcast_vram_32bit_physical_bases = {
    0x05000000u, 0x05800000u, 0x07000000u, 0x07800000u};

inline constexpr std::array<std::uint32_t, 4> dreamcast_aica_ram_physical_bases = {
    0x00800000u, 0x00A00000u, 0x00C00000u, 0x00E00000u};

inline constexpr std::size_t dreamcast_main_ram_alias_count =
    dreamcast_main_ram_area_bases.size() * dreamcast_main_ram_mirrors_per_area;
inline constexpr std::size_t dreamcast_vram_64bit_alias_count =
    dreamcast_direct_segment_bases.size() * dreamcast_vram_64bit_physical_bases.size();
inline constexpr std::size_t dreamcast_vram_32bit_alias_count =
    dreamcast_direct_segment_bases.size() * dreamcast_vram_32bit_physical_bases.size();
inline constexpr std::size_t dreamcast_vram_alias_count =
    dreamcast_vram_64bit_alias_count + dreamcast_vram_32bit_alias_count;
inline constexpr std::size_t dreamcast_aica_ram_alias_count =
    dreamcast_direct_segment_bases.size() * dreamcast_aica_ram_physical_bases.size();
inline constexpr std::size_t dreamcast_bios_alias_count = dreamcast_direct_segment_bases.size();
inline constexpr std::size_t dreamcast_flash_alias_count = dreamcast_direct_segment_bases.size();

[[nodiscard]] std::shared_ptr<LinearMemoryDevice> map_dreamcast_main_ram(Memory& memory);

[[nodiscard]] std::shared_ptr<LinearMemoryDevice> map_dreamcast_vram(Memory& memory);

[[nodiscard]] std::shared_ptr<LinearMemoryDevice> map_dreamcast_aica_ram(Memory& memory);

[[nodiscard]] std::shared_ptr<LinearMemoryDevice>
map_dreamcast_bios(Memory& memory, std::span<const std::uint8_t> image = {});

[[nodiscard]] std::shared_ptr<LinearMemoryDevice>
map_dreamcast_flash(Memory& memory, std::span<const std::uint8_t> image = {});

[[nodiscard]] std::shared_ptr<FlashMemoryDevice>
map_dreamcast_command_flash(Memory& memory, std::span<const std::uint8_t> image = {});

[[nodiscard]] std::shared_ptr<FlashMemoryDevice>
map_dreamcast_command_flash(Memory& memory, std::shared_ptr<PersistentImage> image);

} // namespace katana::runtime
