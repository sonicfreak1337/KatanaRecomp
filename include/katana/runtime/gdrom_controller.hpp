#pragma once

#include "katana/runtime/disc.hpp"
#include "katana/runtime/memory.hpp"
#include "katana/runtime/runtime.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t gdrom_register_physical_base = 0x005F7000u;
inline constexpr std::size_t gdrom_register_size = 0x100u;

struct GdRomProductStatus {
    std::uint8_t ata_status = 0u;
    std::uint8_t interrupt_reason = 0u;
    std::size_t pio_bytes_available = 0u;
    std::size_t bios_requests = 0u;
    std::uint64_t completed_commands = 0u;
    std::uint64_t completed_dma = 0u;
};

class DreamcastGdRomController final {
  public:
    using ModuleLoadObserver = std::function<void(std::uint32_t, std::span<const std::uint8_t>,
                                                   std::string_view)>;
    DreamcastGdRomController(Memory& memory,
                             EventScheduler& scheduler,
                             GdRomDrive drive,
                             std::function<void(std::uint64_t)> completion_observer = {},
                             ModuleLoadObserver module_load_observer = {});
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, MemoryAccessWidth width);
    void write(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width);
    [[nodiscard]] std::uint32_t
    bios_call(CpuState& cpu, std::uint32_t selector, std::uint32_t super_selector);
    void dma_to_memory(std::uint32_t address, std::uint32_t length, std::uint32_t direction);
    [[nodiscard]] GdRomProductStatus status() const noexcept;
    void reset() noexcept;

  private:
    struct BiosRequest {
        std::uint64_t async_id = 0u;
        std::uint32_t destination = 0u;
        CodeWriteSource write_source = CodeWriteSource::Copy;
        bool completed = false;
        GdRomResponse response;
    };
    void execute_packet();
    void publish_data(std::vector<std::uint8_t> data);
    void pump_completions();
    [[nodiscard]] std::vector<std::uint8_t> build_toc(std::uint32_t session) const;
    [[nodiscard]] std::uint64_t submit_bios_read(CpuState& cpu,
                                                 std::uint32_t command,
                                                 std::uint32_t parameters);
    [[nodiscard]] static std::uint32_t fad_to_lba(std::uint32_t fad) noexcept;
    Memory& memory_;
    GdRomDrive drive_;
    GdRomAsyncReader reader_;
    std::vector<std::uint8_t> packet_;
    std::vector<std::uint8_t> data_;
    std::size_t data_cursor_ = 0u;
    std::uint8_t status_ = 0x40u;
    std::uint8_t error_ = 0u;
    std::uint8_t interrupt_reason_ = 0u;
    std::uint16_t byte_count_ = 0u;
    bool expecting_packet_ = false;
    std::map<std::uint64_t, BiosRequest> bios_requests_;
    std::uint64_t next_immediate_request_ = 0x80000000u;
    std::uint64_t completed_commands_ = 0u;
    std::uint64_t completed_dma_ = 0u;
    ModuleLoadObserver module_load_observer_;
};

[[nodiscard]] std::shared_ptr<DreamcastGdRomController>
map_dreamcast_gdrom(Memory& memory,
                    EventScheduler& scheduler,
                    GdRomDrive drive,
                    std::function<void(std::uint64_t)> completion_observer = {},
                    DreamcastGdRomController::ModuleLoadObserver module_load_observer = {});

} // namespace katana::runtime
