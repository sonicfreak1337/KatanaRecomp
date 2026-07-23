#pragma once

#include "katana/runtime/disc.hpp"
#include "katana/runtime/disc_load_transaction.hpp"
#include "katana/runtime/memory.hpp"
#include "katana/runtime/runtime.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

class DreamcastG1BusController;
struct G1DmaFault;

inline constexpr std::uint32_t gdrom_register_physical_base = 0x005F7000u;
inline constexpr std::size_t gdrom_register_size = 0x100u;

struct GdRomProductStatus {
    std::uint8_t ata_status = 0u;
    std::uint8_t interrupt_reason = 0u;
    std::size_t pio_bytes_available = 0u;
    std::size_t bios_requests = 0u;
    std::uint64_t completed_commands = 0u;
    std::uint64_t completed_dma = 0u;
    std::uint64_t committed_load_transactions = 0u;
    std::uint64_t failed_load_transactions = 0u;
    std::array<std::uint32_t, 4u> sector_mode{};
    std::uint32_t dma_callback = 0u;
    std::uint32_t dma_callback_argument = 0u;
    std::uint32_t pio_callback = 0u;
    std::uint32_t pio_callback_argument = 0u;
    std::uint64_t stream_bytes_remaining = 0u;
    std::uint32_t transfer_bytes_remaining = 0u;
    std::size_t pending_guest_callbacks = 0u;
};

enum class GdRomBiosRequestState : std::uint8_t {
    None,
    Queued,
    Processing,
    Complete,
    Streaming,
    Error,
    Aborted
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

enum class GdRomBiosTransferKind : std::uint8_t { None, Dma, Pio };

enum class DiscLoadExecutionPolicy : std::uint8_t {
    RequireAtomicExecutor,
    StandaloneTestMode
};

struct GdRomGuestCallback {
    GdRomBiosTransferKind kind = GdRomBiosTransferKind::None;
    std::uint32_t address = 0u;
    std::uint32_t argument = 0u;
    std::uint32_t request_id = 0u;
};

struct GdRomBiosRequestSnapshot {
    std::uint32_t id = 0u;
    std::uint32_t command = 0u;
    std::array<std::uint32_t, 4u> parameters{};
    std::uint64_t async_id = 0u;
    std::uint32_t destination = 0u;
    CodeWriteSource write_source = CodeWriteSource::Copy;
    GdRomBiosRequestState state = GdRomBiosRequestState::Queued;
    std::array<std::uint32_t, 4u> status{};
    GdRomResponse response;
    bool streaming_dma = false;
    std::uint32_t stream_lba = 0u;
    std::uint32_t stream_sector_count = 0u;
    std::uint64_t stream_total_bytes = 0u;
    std::uint64_t stream_consumed_bytes = 0u;
    std::uint32_t cached_stream_sector = 0u;
    std::vector<std::uint8_t> stream_sector_cache;
    GdRomBiosTransferKind transfer_kind = GdRomBiosTransferKind::None;
    std::uint32_t transfer_destination = 0u;
    std::uint32_t transfer_size = 0u;
    std::uint32_t transfer_transferred = 0u;
    bool transfer_active = false;
};

struct DreamcastGdRomSnapshot {
    GdRomAsyncReaderSnapshot reader;
    std::vector<std::uint8_t> packet;
    std::vector<std::uint8_t> data;
    std::size_t data_cursor = 0u;
    std::uint32_t taskfile_phase_remaining = 0u;
    std::uint32_t taskfile_host_byte_limit = 0u;
    std::uint8_t taskfile_phase = 0u;
    std::uint8_t drive_owner = 0u;
    bool command_irq_asserted = false;
    bool command_irq_reassert_pending = false;
    bool taskfile_command_failed = false;
    bool clear_sense_after_data = false;
    std::uint8_t set_mode_offset = 0u;
    std::array<std::uint8_t, 32u> drive_mode{};
    std::uint8_t sense_key = 0u;
    std::uint8_t sense_asc = 0u;
    std::uint8_t sense_ascq = 0u;
    std::uint8_t status = 0u;
    std::uint8_t error = 0u;
    std::uint8_t interrupt_reason = 0u;
    std::uint8_t features = 0u;
    std::uint8_t sector_count_register = 0u;
    std::uint8_t sector_number = 0u;
    std::uint8_t drive_select = 0u;
    std::uint16_t byte_count = 0u;
    std::uint32_t current_fad = 0u;
    bool expecting_packet = false;
    std::vector<GdRomBiosRequestSnapshot> bios_requests;
    std::uint32_t next_bios_request = 0u;
    GdRomBiosRequestStatus last_bios_request;
    std::vector<GdRomBiosCallEvent> bios_call_events;
    std::uint64_t next_bios_call_sequence = 0u;
    std::uint64_t dropped_bios_call_events = 0u;
    std::uint64_t completed_commands = 0u;
    std::uint64_t completed_dma = 0u;
    std::uint64_t next_load_transaction = 0u;
    std::uint64_t committed_load_transactions = 0u;
    std::uint64_t failed_load_transactions = 0u;
    std::array<std::uint32_t, 4u> sector_mode{};
    std::uint32_t dma_callback = 0u;
    std::uint32_t dma_callback_argument = 0u;
    std::uint32_t pio_callback = 0u;
    std::uint32_t pio_callback_argument = 0u;
    bool dma_completion_pending = false;
    bool pio_completion_pending = false;
    std::uint32_t dma_completion_request = 0u;
    std::uint32_t pio_completion_request = 0u;
    std::vector<GdRomGuestCallback> pending_guest_callbacks;
    std::optional<SchedulerEventId> packet_event;
    bool g1_bus_bound = false;
};

class DreamcastGdRomController final {
  public:
    // The address passed to the observer is the contiguous physical range actually committed to
    // Memory, never an untranslated guest pointer.
    using ModuleLoadObserver = std::function<void(std::uint32_t, std::span<const std::uint8_t>,
                                                    std::string_view)>;
    DreamcastGdRomController(Memory& memory,
                             EventScheduler& scheduler,
                             GdRomDrive drive,
                             std::function<void(std::uint64_t)> completion_observer = {},
                             ModuleLoadObserver module_load_observer = {},
                             std::function<void()> command_ack_observer = {},
                             DiscLoadTransactionExecutor load_transaction_executor = {},
                             std::string content_identity = {},
                             DiscLoadExecutionPolicy load_execution_policy =
                                 DiscLoadExecutionPolicy::RequireAtomicExecutor);
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
    void bind_g1_bus(DreamcastG1BusController* g1_bus) noexcept;
    void handle_g1_dma_fault(const G1DmaFault& fault) noexcept;
    [[nodiscard]] std::optional<GdRomGuestCallback> take_pending_guest_callback();
    [[nodiscard]] DreamcastGdRomSnapshot snapshot() const;
    void reset() noexcept;

  private:
    enum class DriveOwner : std::uint8_t { None, Bios, Taskfile };
    enum class TaskfilePhase : std::uint8_t {
        Idle,
        PacketIn,
        Executing,
        DataIn,
        DmaIn,
        DataOut,
    };

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
        bool streaming_dma = false;
        std::uint32_t stream_lba = 0u;
        std::uint32_t stream_sector_count = 0u;
        std::uint64_t stream_total_bytes = 0u;
        std::uint64_t stream_consumed_bytes = 0u;
        std::uint32_t cached_stream_sector = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint8_t> stream_sector_cache;
        GdRomBiosTransferKind transfer_kind = GdRomBiosTransferKind::None;
        std::uint32_t transfer_destination = 0u;
        std::uint32_t transfer_size = 0u;
        std::uint32_t transfer_transferred = 0u;
        bool transfer_active = false;
    };
    void execute_packet();
    void schedule_packet();
    void complete_packet(SchedulerEventId event_id, std::uint64_t cycle);
    void publish_data(std::vector<std::uint8_t> data);
    void publish_dma_data(std::vector<std::uint8_t> data,
                          DiscLoadSourceRange source_range);
    void begin_data_out(std::size_t size, std::uint8_t mode_offset);
    void begin_next_taskfile_data_phase();
    void complete_taskfile_data_phase();
    void finish_taskfile_command();
    void fail_taskfile_command(std::uint8_t sense_key,
                               std::uint8_t asc,
                               std::uint8_t ascq,
                               bool ata_abort);
    void latch_sense(std::uint8_t sense_key,
                     std::uint8_t asc,
                     std::uint8_t ascq,
                     bool ata_abort = false) noexcept;
    void clear_sense() noexcept;
    void raise_command_irq(std::uint64_t cycle);
    void acknowledge_command_irq();
    [[nodiscard]] bool taskfile_blocks_bios() const noexcept;
    void release_bios_owner_if_idle() noexcept;
    void pump_completions();
    [[nodiscard]] std::vector<std::uint8_t> build_packet_toc(std::uint32_t session) const;
    [[nodiscard]] std::array<std::uint32_t, 102u> build_bios_toc(std::uint32_t area) const;
    void execute_bios_request(CpuState& cpu, BiosRequest& request);
    void submit_bios_read(CpuState& cpu, BiosRequest& request);
    void submit_bios_stream(BiosRequest& request);
    [[nodiscard]] std::vector<std::uint8_t> preview_stream_bytes(BiosRequest& request,
                                                                 std::uint32_t length);
    void commit_stream_bytes(BiosRequest& request, std::uint32_t length);
    void finish_stream_transfer(BiosRequest& request);
    [[nodiscard]] BiosRequest* active_stream_transfer(GdRomBiosTransferKind kind) noexcept;
    void queue_stream_callback(std::uint32_t request_id, GdRomBiosTransferKind kind);
    [[nodiscard]] DiscLoadCommit commit_disc_load(DiscLoadRoute route,
                                                  std::uint32_t guest_destination,
                                                  std::uint32_t physical_destination,
                                                  std::span<const std::uint8_t> bytes,
                                                  CodeWriteSource source,
                                                  DiscLoadSourceRange source_range = {});
    void remember_bios_request(const BiosRequest& request) noexcept;
    [[nodiscard]] const BiosRequest* find_bios_request(std::uint32_t id) const noexcept;
    std::uint32_t finish_bios_call(GdRomBiosCallEvent event, std::uint32_t result);
    [[nodiscard]] static std::uint32_t fad_to_lba(std::uint32_t fad) noexcept;
    void reset_transport() noexcept;
    Memory& memory_;
    EventScheduler& scheduler_;
    GdRomDrive drive_;
    GdRomAsyncReader reader_;
    std::vector<std::uint8_t> packet_;
    std::vector<std::uint8_t> data_;
    std::size_t data_cursor_ = 0u;
    DiscLoadSourceRange taskfile_data_source_range_;
    std::uint32_t taskfile_phase_remaining_ = 0u;
    std::uint32_t taskfile_host_byte_limit_ = 65'536u;
    TaskfilePhase taskfile_phase_ = TaskfilePhase::Idle;
    DriveOwner drive_owner_ = DriveOwner::None;
    bool command_irq_asserted_ = false;
    bool command_irq_reassert_pending_ = false;
    bool taskfile_command_failed_ = false;
    bool clear_sense_after_data_ = false;
    std::uint8_t set_mode_offset_ = 0u;
    std::array<std::uint8_t, 32u> drive_mode_{0u,
                                              0u,
                                              0u,
                                              0u,
                                              0u,
                                              0xB4u,
                                              0x19u,
                                              0u,
                                              0u,
                                              0x08u};
    std::uint8_t sense_key_ = 0u;
    std::uint8_t sense_asc_ = 0u;
    std::uint8_t sense_ascq_ = 0u;
    std::uint8_t status_ = 0x40u;
    std::uint8_t error_ = 0u;
    std::uint8_t interrupt_reason_ = 0u;
    std::uint8_t features_ = 0u;
    std::uint8_t sector_count_register_ = 0u;
    std::uint8_t sector_number_ = 0u;
    std::uint8_t drive_select_ = 0u;
    std::uint16_t byte_count_ = 0u;
    std::uint32_t current_fad_ = 150u;
    bool expecting_packet_ = false;
    std::map<std::uint64_t, BiosRequest> bios_requests_;
    std::uint32_t next_bios_request_ = 1u;
    GdRomBiosRequestStatus last_bios_request_;
    std::vector<GdRomBiosCallEvent> bios_call_events_;
    std::uint64_t next_bios_call_sequence_ = 1u;
    std::uint64_t dropped_bios_call_events_ = 0u;
    std::uint64_t completed_commands_ = 0u;
    std::uint64_t completed_dma_ = 0u;
    std::uint64_t next_load_transaction_ = 1u;
    std::uint64_t committed_load_transactions_ = 0u;
    std::uint64_t failed_load_transactions_ = 0u;
    std::array<std::uint32_t, 4u> sector_mode_{0u, 0x2000u, 1024u, 2048u};
    std::uint32_t dma_callback_ = 0u;
    std::uint32_t dma_callback_argument_ = 0u;
    std::uint32_t pio_callback_ = 0u;
    std::uint32_t pio_callback_argument_ = 0u;
    bool dma_completion_pending_ = false;
    bool pio_completion_pending_ = false;
    std::uint32_t dma_completion_request_ = 0u;
    std::uint32_t pio_completion_request_ = 0u;
    std::vector<GdRomGuestCallback> pending_guest_callbacks_;
    DreamcastG1BusController* g1_bus_ = nullptr;
    ModuleLoadObserver module_load_observer_;
    DiscLoadTransactionExecutor load_transaction_executor_;
    std::string content_identity_;
    DiscLoadExecutionPolicy load_execution_policy_ =
        DiscLoadExecutionPolicy::RequireAtomicExecutor;
    std::function<void(std::uint64_t)> completion_observer_;
    std::function<void()> command_ack_observer_;
    std::optional<SchedulerEventId> packet_event_;
    SchedulerLifetimeToken scheduler_lifetime_;
};

[[nodiscard]] std::shared_ptr<DreamcastGdRomController>
map_dreamcast_gdrom(Memory& memory,
                    EventScheduler& scheduler,
                    GdRomDrive drive,
                    std::function<void(std::uint64_t)> completion_observer = {},
                    DreamcastGdRomController::ModuleLoadObserver module_load_observer = {},
                    std::function<void()> command_ack_observer = {},
                    DiscLoadTransactionExecutor load_transaction_executor = {},
                    std::string content_identity = {},
                    DiscLoadExecutionPolicy load_execution_policy =
                        DiscLoadExecutionPolicy::RequireAtomicExecutor);

} // namespace katana::runtime
