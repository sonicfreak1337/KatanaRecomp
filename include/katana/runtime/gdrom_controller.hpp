#pragma once

#include "katana/runtime/disc.hpp"
#include "katana/runtime/memory.hpp"
#include "katana/runtime/runtime.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

enum class GdRomBiosRequestState : std::uint8_t {
    None,
    Queued,
    Processing,
    Complete,
    Streaming,
    Error
};

struct GdRomBiosRequestStatus {
    std::uint32_t id = 0u;
    std::uint32_t command = 0u;
    GdRomBiosRequestState state = GdRomBiosRequestState::None;
    std::array<std::uint32_t, 4u> status{};
};

struct GdRomBiosCallEvent {
    std::uint64_t sequence = 0u;
    std::uint64_t guest_cycle = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t return_address = 0u;
    std::uint32_t selector = 0u;
    std::uint32_t super_selector = 0u;
    std::array<std::uint32_t, 4u> arguments{};
    std::uint32_t request_id = 0u;
    GdRomBiosRequestState state_before = GdRomBiosRequestState::None;
    GdRomBiosRequestState state_after = GdRomBiosRequestState::None;
    std::uint32_t result = 0u;
    std::array<std::uint32_t, 4u> status{};
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
    ~DreamcastGdRomController();
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, MemoryAccessWidth width);
    void write(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width);
    [[nodiscard]] std::uint32_t
    bios_call(CpuState& cpu, std::uint32_t selector, std::uint32_t super_selector);
    [[nodiscard]] bool reload_system_bootstrap(CpuState& cpu);
    void dma_to_memory(std::uint32_t address, std::uint32_t length, std::uint32_t direction);
    [[nodiscard]] GdRomProductStatus status() const noexcept;
    [[nodiscard]] const GdRomBiosRequestStatus& last_bios_request() const noexcept;
    [[nodiscard]] std::span<const GdRomBiosCallEvent> bios_call_events() const noexcept;
    [[nodiscard]] std::string format_bios_call_events_json() const;
    void reset() noexcept;

  private:
    struct BiosRequest {
        std::uint32_t id = 0u;
        std::uint32_t command = 0u;
        std::array<std::uint32_t, 4u> parameters{};
        std::uint64_t async_id = 0u;
        std::uint32_t destination = 0u;
        CodeWriteSource write_source = CodeWriteSource::Copy;
        GdRomBiosRequestState state = GdRomBiosRequestState::Queued;
        std::array<std::uint32_t, 4u> status{};
        GdRomResponse response;
    };
    void execute_packet();
    void schedule_packet();
    void complete_packet(SchedulerEventId event_id, std::uint64_t cycle);
    void publish_data(std::vector<std::uint8_t> data);
    void pump_completions();
    [[nodiscard]] std::vector<std::uint8_t> build_packet_toc(std::uint32_t session) const;
    [[nodiscard]] std::array<std::uint32_t, 102u> build_bios_toc(std::uint32_t area) const;
    void execute_bios_request(CpuState& cpu, BiosRequest& request);
    void submit_bios_read(BiosRequest& request);
    void remember_bios_request(const BiosRequest& request) noexcept;
    [[nodiscard]] const BiosRequest* find_bios_request(std::uint32_t id) const noexcept;
    std::uint32_t finish_bios_call(GdRomBiosCallEvent event, std::uint32_t result);
    [[nodiscard]] static std::uint32_t fad_to_lba(std::uint32_t fad) noexcept;
    Memory& memory_;
    EventScheduler& scheduler_;
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
    std::uint32_t next_bios_request_ = 1u;
    GdRomBiosRequestStatus last_bios_request_;
    std::vector<GdRomBiosCallEvent> bios_call_events_;
    std::uint64_t next_bios_call_sequence_ = 1u;
    std::uint64_t dropped_bios_call_events_ = 0u;
    std::uint64_t completed_commands_ = 0u;
    std::uint64_t completed_dma_ = 0u;
    ModuleLoadObserver module_load_observer_;
    std::function<void(std::uint64_t)> completion_observer_;
    std::optional<SchedulerEventId> packet_event_;
    SchedulerLifetimeToken scheduler_lifetime_;
};

[[nodiscard]] std::shared_ptr<DreamcastGdRomController>
map_dreamcast_gdrom(Memory& memory,
                    EventScheduler& scheduler,
                    GdRomDrive drive,
                    std::function<void(std::uint64_t)> completion_observer = {},
                    DreamcastGdRomController::ModuleLoadObserver module_load_observer = {});

} // namespace katana::runtime
