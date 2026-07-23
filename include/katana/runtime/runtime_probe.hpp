#pragma once

#include "katana/runtime/aica.hpp"
#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/cache_control.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/dma.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/firmware_handoff.hpp"
#include "katana/runtime/gdrom_controller.hpp"
#include "katana/runtime/holly_dma.hpp"
#include "katana/runtime/interrupt.hpp"
#include "katana/runtime/io_port.hpp"
#include "katana/runtime/maple.hpp"
#include "katana/runtime/maple_mmio.hpp"
#include "katana/runtime/platform_interrupt.hpp"
#include "katana/runtime/pvr.hpp"
#include "katana/runtime/runtime.hpp"
#include "katana/runtime/scheduler.hpp"
#include "katana/runtime/scif.hpp"
#include "katana/runtime/system_asic.hpp"
#include "katana/runtime/timers.hpp"
#include "katana/runtime/store_queue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

struct DreamcastRuntimeState;
class SystemReplayLog;

inline constexpr std::uint32_t runtime_probe_schema_version = 1u;
inline constexpr std::uint64_t runtime_probe_device_schema_version = 1u;
inline constexpr std::string_view runtime_probe_hash_contract = "fnv1a64-le-v1";
inline constexpr std::uint32_t runtime_probe_fault_report_version = 1u;
inline constexpr std::size_t runtime_probe_replay_coverage_class_count = 12u;
inline constexpr std::string_view runtime_probe_fault_line_prefix =
    "KATANA_RUNTIME_PROBE_FAULT ";
inline constexpr std::uint64_t runtime_probe_fnv1a64_offset_basis =
    14695981039346656037ull;
inline constexpr std::uint64_t runtime_probe_fnv1a64_prime = 1099511628211ull;

class RuntimeProbeBudgetReached final : public std::runtime_error {
  public:
    explicit RuntimeProbeBudgetReached(
        std::optional<std::uint64_t> final_guest_cycle = std::nullopt);

    [[nodiscard]] std::optional<std::uint64_t> final_guest_cycle() const noexcept;

  private:
    std::optional<std::uint64_t> final_guest_cycle_;
};

class RuntimeProbeFnv1a64LeV1 final {
  public:
    void append_u8(std::uint8_t value) noexcept;
    void append_u16(std::uint16_t value) noexcept;
    void append_u32(std::uint32_t value) noexcept;
    void append_u64(std::uint64_t value) noexcept;
    void append_bool(bool value) noexcept;
    void append_bytes(std::span<const std::uint8_t> bytes) noexcept;

    [[nodiscard]] std::uint64_t value() const noexcept;

  private:
    std::uint64_t value_ = runtime_probe_fnv1a64_offset_basis;
};

struct RuntimeProbeTlbEntry {
    std::uint32_t pteh = 0u;
    std::uint32_t ptel = 0u;
    std::uint32_t ptea = 0u;

    [[nodiscard]] bool operator==(const RuntimeProbeTlbEntry&) const = default;
};

struct RuntimeProbeCpuSnapshot {
    std::array<std::uint32_t, general_register_count> r{};
    std::array<std::uint32_t, banked_register_count> r_bank{};
    std::array<std::uint32_t, fpu_register_count> fr{};
    std::array<std::uint32_t, fpu_register_count> xf{};
    std::uint32_t pc = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t gbr = 0u;
    std::uint32_t vbr = 0u;
    std::uint32_t ssr = 0u;
    std::uint32_t spc = 0u;
    std::uint32_t sgr = 0u;
    std::uint32_t dbr = 0u;
    std::uint32_t tra = 0u;
    std::uint32_t tea = 0u;
    std::uint32_t expevt = 0u;
    std::uint32_t intevt = 0u;
    std::uint32_t pteh = 0u;
    std::uint32_t ptel = 0u;
    std::uint32_t ptea = 0u;
    std::uint32_t ttb = 0u;
    std::uint32_t mmucr = 0u;
    std::array<RuntimeProbeTlbEntry, 64u> utlb{};
    std::uint64_t tlb_load_count = 0u;
    std::uint32_t mach = 0u;
    std::uint32_t macl = 0u;
    std::uint32_t fpul = 0u;
    std::uint32_t fpscr = 0u;
    std::uint32_t sr = 0u;
    bool t = false;
    bool s = false;
    bool q = false;
    bool m = false;
    bool trap_pending = false;
    ExceptionCause last_exception_cause = ExceptionCause::None;
    bool exception_in_delay_slot = false;
    bool sleeping = false;
    std::uint32_t last_prefetch_address = 0u;
    std::uint64_t prefetch_count = 0u;
    std::uint64_t retired_guest_instructions = 0u;
    bool last_prefetch_was_store_queue = false;

    [[nodiscard]] bool operator==(const RuntimeProbeCpuSnapshot&) const = default;
};

struct RuntimeProbeSchedulerSnapshot {
    std::uint64_t current_cycle = 0u;
    std::optional<std::uint64_t> next_event_cycle;
    std::uint64_t pending_event_count = 0u;
    SchedulerEventId next_event_id = 0u;
    SchedulerResetObserverId next_reset_observer_id = 0u;
    std::uint64_t processed_event_count = 0u;
    std::uint64_t reset_generation = 0u;
    std::optional<std::uint64_t> guest_cycle_budget;
    bool advance_in_progress = false;
    std::vector<SchedulerPendingEventSnapshot> pending_events;
    std::vector<SchedulerResetObserverId> reset_observer_ids;

    [[nodiscard]] bool operator==(const RuntimeProbeSchedulerSnapshot&) const = default;
};

struct RuntimeProbeReplaySnapshot {
    std::uint32_t replay_schema_version = 0u;
    std::uint64_t event_count = 0u;
    std::uint64_t dropped_events = 0u;
    std::uint64_t event_hash = 0u;
    std::uint64_t ordering_digest = 0u;
    std::uint32_t enabled_coverage = 0u;
    std::uint32_t observed_coverage = 0u;
    std::uint32_t required_coverage = 0u;
    std::array<std::uint64_t, runtime_probe_replay_coverage_class_count>
        event_counts{};
    bool coverage_complete = false;
    bool complete = false;
    bool sealed = false;
    std::optional<std::uint64_t> final_guest_state_hash;

    [[nodiscard]] bool operator==(const RuntimeProbeReplaySnapshot&) const = default;
};

enum class RuntimeProbeMemoryRegion : std::uint32_t {
    Unknown = 0u,
    MainRam = 1u,
    VideoRam = 2u,
    AicaRam = 3u,
    Flash = 4u,
    Vmu = 5u,
    Custom = 0x80000000u,
};

struct RuntimeProbeMemoryRange {
    RuntimeProbeMemoryRegion region = RuntimeProbeMemoryRegion::Unknown;
    std::uint32_t offset = 0u;
    std::span<const std::uint8_t> bytes;
};

enum class RuntimeProbeDeviceKind : std::uint32_t {
    Unknown = 0u,
    Pvr = 1u,
    SystemBus = 2u,
    Aica = 3u,
    GdRom = 4u,
    Maple = 5u,
    Dmac = 6u,
    InterruptController = 7u,
    StoreQueue = 8u,
    HollyDma = 9u,
    Tmu = 10u,
    Rtc = 11u,
    Renderer = 12u,
    HostAudio = 13u,
    Scif = 14u,
    SystemAsic = 15u,
    PvrTa = 16u,
    PvrTaAperture = 17u,
    PvrYuv = 18u,
    IoPort = 19u,
    AicaRtc = 20u,
    AicaExecution = 21u,
    MapleController = 22u,
    RtcClock = 23u,
    InterruptRouter = 24u,
    InterruptRegisters = 25u,
    CacheControl = 26u,
    AddressSpace = 27u,
    BlockTable = 28u,
    CodeTracker = 29u,
    ModuleCatalog = 30u,
    FirmwareHandoff = 31u,
    Flash = 32u,
    Vmu = 33u,
    Custom = 0x80000000u,
};

struct RuntimeProbeDeviceSchema {
    RuntimeProbeDeviceKind kind = RuntimeProbeDeviceKind::Unknown;
    std::uint32_t instance = 0u;
    std::uint32_t field_count = 0u;
};

inline constexpr std::array<RuntimeProbeDeviceSchema, 35u>
    runtime_probe_deterministic_v1_device_schemas = {{
        {RuntimeProbeDeviceKind::Pvr, 0u, 32u},
        {RuntimeProbeDeviceKind::SystemBus, 0u, 23u},
        {RuntimeProbeDeviceKind::Aica, 0u, 8u},
        {RuntimeProbeDeviceKind::GdRom, 0u, 71u},
        {RuntimeProbeDeviceKind::Maple, 0u, 28u},
        {RuntimeProbeDeviceKind::Dmac, 0u, 53u},
        {RuntimeProbeDeviceKind::InterruptController, 0u, 3u},
        {RuntimeProbeDeviceKind::StoreQueue, 0u, 18u},
        {RuntimeProbeDeviceKind::HollyDma, 0u, 42u},
        {RuntimeProbeDeviceKind::HollyDma, 1u, 33u},
        {RuntimeProbeDeviceKind::HollyDma, 2u, 86u},
        {RuntimeProbeDeviceKind::Tmu, 0u, 39u},
        {RuntimeProbeDeviceKind::Rtc, 0u, 34u},
        {RuntimeProbeDeviceKind::Renderer, 0u, 38u},
        {RuntimeProbeDeviceKind::HostAudio, 0u, 4u},
        {RuntimeProbeDeviceKind::Scif, 0u, 17u},
        {RuntimeProbeDeviceKind::SystemAsic, 0u, 21u},
        {RuntimeProbeDeviceKind::PvrTa, 0u, 135u},
        {RuntimeProbeDeviceKind::PvrTaAperture, 0u, 6u},
        {RuntimeProbeDeviceKind::PvrYuv, 0u, 8u},
        {RuntimeProbeDeviceKind::IoPort, 0u, 10u},
        {RuntimeProbeDeviceKind::AicaRtc, 0u, 9u},
        {RuntimeProbeDeviceKind::AicaExecution, 0u, 21u},
        {RuntimeProbeDeviceKind::MapleController, 0u, 19u},
        {RuntimeProbeDeviceKind::RtcClock, 0u, 6u},
        {RuntimeProbeDeviceKind::InterruptRouter, 0u, 14u},
        {RuntimeProbeDeviceKind::InterruptRegisters, 0u, 6u},
        {RuntimeProbeDeviceKind::CacheControl, 0u, 13u},
        {RuntimeProbeDeviceKind::AddressSpace, 0u, 9u},
        {RuntimeProbeDeviceKind::BlockTable, 0u, 12u},
        {RuntimeProbeDeviceKind::CodeTracker, 0u, 16u},
        {RuntimeProbeDeviceKind::ModuleCatalog, 0u, 14u},
        {RuntimeProbeDeviceKind::FirmwareHandoff, 0u, 8u},
        {RuntimeProbeDeviceKind::Flash, 0u, 6u},
        {RuntimeProbeDeviceKind::Vmu, 0u, 5u},
    }};

struct RuntimeProbeDeviceField {
    std::uint32_t id = 0u;
    std::uint64_t value = 0u;

    [[nodiscard]] bool operator==(const RuntimeProbeDeviceField&) const = default;
};

struct RuntimeProbeDeviceSnapshot {
    RuntimeProbeDeviceKind kind = RuntimeProbeDeviceKind::Unknown;
    std::uint32_t instance = 0u;
    std::vector<RuntimeProbeDeviceField> fields;

    [[nodiscard]] bool operator==(const RuntimeProbeDeviceSnapshot&) const = default;
};

struct RuntimeProbeDreamcastSnapshot {
    std::vector<std::uint8_t> flash;
    std::vector<std::uint8_t> vmu;
    std::vector<RuntimeProbeDeviceSnapshot> devices;
};

struct RuntimeProbeHashes {
    std::uint64_t cpu = 0u;
    std::uint64_t scheduler = 0u;
    std::uint64_t memory = 0u;
    std::uint64_t persistent = 0u;
    std::uint64_t devices = 0u;
    std::uint64_t replay = 0u;
    std::uint64_t guest_state = 0u;
    std::uint64_t combined = 0u;

    [[nodiscard]] bool operator==(const RuntimeProbeHashes&) const = default;
};

enum class RuntimeProbeStatus : std::uint8_t {
    Complete,
    Incomplete,
    Failed,
};

enum class RuntimeProbeTermination : std::uint8_t {
    Unknown = 0u,
    Completed = 1u,
    GuestLifecycle = 2u,
    BudgetReached = 3u,
    HostShutdown = 4u,
    Failed = 5u,
    Hang = 6u,
    GuestException = 7u,
    DispatchMiss = 8u,
};

enum class RuntimeProbeCheckpoint : std::uint8_t {
    None = 0u,
    RuntimeStarted = 1u,
    GuestProgramEntered = 2u,
    FirstGuestFrame = 3u,
    GuestInputInteractive = 4u,
    ControlledRetailScene = 5u,
};

struct RuntimeProbeCheckpointObservation {
    RuntimeProbeCheckpoint checkpoint = RuntimeProbeCheckpoint::None;
    RuntimeProbeCpuSnapshot cpu;

    [[nodiscard]] bool
    operator==(const RuntimeProbeCheckpointObservation&) const = default;
};

struct RuntimeProbeFaultObservation {
    RuntimeProbeTermination termination = RuntimeProbeTermination::Unknown;
    RuntimeProbeCpuSnapshot cpu;

    [[nodiscard]] bool operator==(const RuntimeProbeFaultObservation&) const = default;
};

struct RuntimeProbeFaultEnvelope {
    std::uint32_t report_version = runtime_probe_fault_report_version;
    RuntimeProbeTermination termination = RuntimeProbeTermination::Unknown;
    std::optional<RuntimeProbeFaultObservation> first_fault;
    std::optional<RuntimeProbeCheckpointObservation> last_stable_checkpoint;

    [[nodiscard]] bool operator==(const RuntimeProbeFaultEnvelope&) const = default;
};

class RuntimeProbeObservationState final {
  public:
    [[nodiscard]] bool
    observe_checkpoint(RuntimeProbeCheckpoint checkpoint,
                       const RuntimeProbeCpuSnapshot& cpu) noexcept;
    [[nodiscard]] bool
    latch_fault(RuntimeProbeTermination termination,
                const RuntimeProbeCpuSnapshot& cpu) noexcept;

    [[nodiscard]] const std::optional<RuntimeProbeCheckpointObservation>&
    last_stable_checkpoint() const noexcept;
    [[nodiscard]] const std::optional<RuntimeProbeFaultObservation>&
    first_fault() const noexcept;
    [[nodiscard]] RuntimeProbeFaultEnvelope
    fault_envelope(RuntimeProbeTermination termination) const noexcept;

  private:
    std::optional<RuntimeProbeCheckpointObservation> last_stable_checkpoint_;
    std::optional<RuntimeProbeFaultObservation> first_fault_;
};

struct RuntimeProbeReport {
    std::uint32_t schema_version = runtime_probe_schema_version;
    RuntimeProbeStatus status = RuntimeProbeStatus::Incomplete;
    RuntimeProbeTermination termination = RuntimeProbeTermination::Unknown;
    bool diagnostics_enabled = false;
    std::optional<std::uint64_t> guest_cycle_budget;
    std::uint64_t guest_cycle = 0u;
    std::uint64_t retired_guest_instructions = 0u;
    std::uint64_t memory_byte_count = 0u;
    std::uint64_t memory_range_count = 0u;
    std::uint64_t persistent_byte_count = 0u;
    std::uint64_t persistent_range_count = 0u;
    std::uint64_t device_count = 0u;
    std::uint64_t device_field_count = 0u;
    RuntimeProbeReplaySnapshot replay;
    RuntimeProbeHashes hashes;

    [[nodiscard]] bool operator==(const RuntimeProbeReport&) const = default;
};

[[nodiscard]] RuntimeProbeCpuSnapshot
capture_runtime_probe_cpu(const CpuState& cpu) noexcept;
[[nodiscard]] RuntimeProbeSchedulerSnapshot
capture_runtime_probe_scheduler(const EventScheduler& scheduler);
[[nodiscard]] RuntimeProbeReplaySnapshot
capture_runtime_probe_replay(const SystemReplayLog& replay);

[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrRegisterSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastSystemBusSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastSystemAsicSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const AicaRegisterSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const AicaRtcSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const AicaExecutionController::Snapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastGdRomSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const MapleBusSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastMapleControllerSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4TmuSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4RtcClockDomain::Snapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4RtcSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4ScifSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrTaFifoSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrTaFifoMemoryDevice::Snapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrYuvConverterMemoryDevice::Snapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrSoftwareRendererSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4IoPortSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4DmacSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const InterruptControllerSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PlatformInterruptRouterSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4InterruptRegistersSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4CacheControlSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const RuntimeAddressSpaceSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const RuntimeBlockTableSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const ExecutableCodeTrackerSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const ExecutableModuleCatalogSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const FirmwareHandoffSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4StoreQueueSnapshot& snapshot,
                                   std::span<const StoreQueueTransfer> transfer_history = {},
                                   std::uint64_t dropped_transfers = 0u,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const FlashMemorySnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const MapleVmuSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastG1DmaSnapshot& snapshot,
                                   std::uint32_t instance = 0u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastPvrDmaSnapshot& snapshot,
                                   std::uint32_t instance = 1u);
[[nodiscard]] RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastG2DmaSnapshot& snapshot,
                                   std::uint32_t instance = 2u);
[[nodiscard]] RuntimeProbeDreamcastSnapshot
capture_runtime_probe_dreamcast(const DreamcastRuntimeState& state,
                                std::uint64_t audio_buffers,
                                std::uint64_t audio_frames,
                                std::uint64_t audio_hash);

[[nodiscard]] std::uint64_t
hash_runtime_probe_cpu(const RuntimeProbeCpuSnapshot& snapshot) noexcept;
[[nodiscard]] std::uint64_t
hash_runtime_probe_scheduler(const RuntimeProbeSchedulerSnapshot& snapshot) noexcept;
[[nodiscard]] std::uint64_t
hash_runtime_probe_memory(std::span<const RuntimeProbeMemoryRange> ranges);
[[nodiscard]] std::uint64_t
hash_runtime_probe_persistent(std::span<const RuntimeProbeMemoryRange> ranges);
[[nodiscard]] std::uint64_t
hash_runtime_probe_devices(std::span<const RuntimeProbeDeviceSnapshot> devices);
[[nodiscard]] std::uint64_t
hash_runtime_probe_replay(const RuntimeProbeReplaySnapshot& snapshot) noexcept;
[[nodiscard]] std::uint64_t
combine_runtime_probe_guest_state_hashes(std::uint64_t cpu_hash,
                                         std::uint64_t scheduler_hash,
                                         std::uint64_t memory_hash,
                                         std::uint64_t persistent_hash,
                                         std::uint64_t device_hash) noexcept;
[[nodiscard]] std::uint64_t
combine_runtime_probe_hashes(std::uint64_t guest_state_hash,
                             std::uint64_t replay_hash) noexcept;

void validate_runtime_probe_deterministic_v1(
    const RuntimeProbeSchedulerSnapshot& scheduler,
    std::span<const RuntimeProbeMemoryRange> memory,
    std::span<const RuntimeProbeMemoryRange> persistent,
    std::span<const RuntimeProbeDeviceSnapshot> devices,
    const RuntimeProbeReplaySnapshot& replay);

[[nodiscard]] RuntimeProbeReport
make_runtime_probe_report(const RuntimeProbeCpuSnapshot& cpu,
                          const RuntimeProbeSchedulerSnapshot& scheduler,
                          std::span<const RuntimeProbeMemoryRange> memory,
                          std::span<const RuntimeProbeMemoryRange> persistent,
                          std::span<const RuntimeProbeDeviceSnapshot> devices,
                          const RuntimeProbeReplaySnapshot& replay);
[[nodiscard]] std::string
serialize_runtime_probe_report_json(const RuntimeProbeReport& report);
[[nodiscard]] std::string
serialize_runtime_probe_fault_envelope_json(
    const RuntimeProbeFaultEnvelope& envelope);

} // namespace katana::runtime
