#pragma once

#include "katana/runtime/abi.hpp"
#include "katana/runtime/runtime.hpp"
#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace katana::runtime {

enum class DreamcastConsoleProfile : std::uint8_t;
struct DreamcastRuntimeState;
struct RuntimeAddressSpaceSnapshot;
class ValidatedGameEntryHandoff;

inline constexpr std::uint32_t game_entry_handoff_schema_version = 3u;
inline constexpr std::uint32_t game_entry_platform_state_contract_version = 2u;

// Content identity is the representation-independent, unprefixed lower-case
// SHA-256 used by the disc installer. Byte and descriptor identities use the
// explicit "sha256:" prefix.
struct GameEntryExecutableIdentity {
    std::string content_identity;
    std::string boot_file_name;
    std::string boot_byte_identity;

    [[nodiscard]] bool operator==(const GameEntryExecutableIdentity&) const = default;
};

struct GameEntryHandoffBinding {
    std::uint32_t schema_version = game_entry_handoff_schema_version;
    std::uint32_t required_runtime_abi = abi_version;
    std::uint32_t required_platform_state_contract =
        game_entry_platform_state_contract_version;
    GameEntryExecutableIdentity executable;
    DreamcastConsoleProfile console_profile{};
    std::string descriptor_identity;

    [[nodiscard]] bool operator==(const GameEntryHandoffBinding&) const = default;
};

enum class GameEntryTransferKind : std::uint8_t {
    JumpPreservingPr,
    CallWithReturnPr,
};

struct GameEntryControlTransfer {
    GameEntryTransferKind kind = GameEntryTransferKind::JumpPreservingPr;
    std::uint32_t entry_pc = 0u;
    std::uint32_t exact_pr = 0u;

    [[nodiscard]] bool operator==(const GameEntryControlTransfer&) const = default;
};

struct GameEntryTlbEntry {
    std::uint32_t pteh = 0u;
    std::uint32_t ptel = 0u;
    std::uint32_t ptea = 0u;

    [[nodiscard]] bool operator==(const GameEntryTlbEntry&) const = default;
};

struct GameEntryMmuState {
    std::uint32_t pteh = 0u;
    std::uint32_t ptel = 0u;
    std::uint32_t ptea = 0u;
    std::uint32_t ttb = 0u;
    std::uint32_t mmucr = 0u;
    std::array<GameEntryTlbEntry, 64u> utlb{};

    [[nodiscard]] bool operator==(const GameEntryMmuState&) const = default;
};

struct GameEntryExceptionState {
    bool trap_pending = false;
    ExceptionCause last_cause = ExceptionCause::None;
    bool in_delay_slot = false;
    std::uint32_t last_instruction_pc = 0u;
    std::uint32_t last_instruction_physical_pc = 0u;
    std::uint32_t last_owner_pc = 0u;
    bool sleeping = false;

    [[nodiscard]] bool operator==(const GameEntryExceptionState&) const = default;
};

// Register banks use physical bank numbering, independent of SR.RB/FPSCR.FR.
// This avoids importing CpuState's active/inactive array representation.
struct GameEntryCpuState {
    std::array<std::uint32_t, 8u> gpr_bank0{};
    std::array<std::uint32_t, 8u> gpr_bank1{};
    std::array<std::uint32_t, 8u> r8_to_r15{};
    std::array<std::uint32_t, 16u> fpr_bank0{};
    std::array<std::uint32_t, 16u> fpr_bank1{};
    std::uint32_t pc = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t sr = 0u;
    std::uint32_t fpscr = 0u;
    std::uint32_t gbr = 0u;
    std::uint32_t vbr = 0u;
    std::uint32_t dbr = 0u;
    std::uint32_t ssr = 0u;
    std::uint32_t spc = 0u;
    std::uint32_t sgr = 0u;
    std::uint32_t mach = 0u;
    std::uint32_t macl = 0u;
    std::uint32_t fpul = 0u;
    std::uint32_t tra = 0u;
    std::uint32_t tea = 0u;
    std::uint32_t expevt = 0u;
    std::uint32_t intevt = 0u;
    GameEntryMmuState mmu;
    GameEntryExceptionState exception;

    [[nodiscard]] bool operator==(const GameEntryCpuState&) const = default;
};

// CpuState keeps the currently selected SH-4 GPR/FPU banks in r/fr and the
// inactive banks in r_bank/xf. The handoff contract instead uses physical bank
// numbers, so capture/apply remain independent of the current SR.RB/FPSCR.FR.
[[nodiscard]] GameEntryCpuState
capture_game_entry_cpu_state(const CpuState& cpu);
void apply_game_entry_cpu_state(CpuState& cpu,
                                const GameEntryCpuState& state);

class PreparedGameEntryCpuRestore final {
  public:
    PreparedGameEntryCpuRestore(
        const PreparedGameEntryCpuRestore&) = delete;
    PreparedGameEntryCpuRestore& operator=(
        const PreparedGameEntryCpuRestore&) = delete;
    PreparedGameEntryCpuRestore(
        PreparedGameEntryCpuRestore&&) noexcept;
    PreparedGameEntryCpuRestore& operator=(
        PreparedGameEntryCpuRestore&&) noexcept;
    ~PreparedGameEntryCpuRestore();

  private:
    friend PreparedGameEntryCpuRestore
    prepare_game_entry_cpu_restore(
        const CpuState&,
        const GameEntryCpuState&,
        std::shared_ptr<RuntimeAddressSpace>);
    friend void validate_prepared_game_entry_cpu_mmu(
        const PreparedGameEntryCpuRestore&,
        const RuntimeAddressSpaceSnapshot&);
    friend void commit_prepared_game_entry_cpu_restore(
        CpuState&,
        PreparedGameEntryCpuRestore) noexcept;
    struct Data;
    PreparedGameEntryCpuRestore();
    std::unique_ptr<Data> data_;
};

[[nodiscard]] PreparedGameEntryCpuRestore
prepare_game_entry_cpu_restore(
    const CpuState& cpu,
    const GameEntryCpuState& state,
    std::shared_ptr<RuntimeAddressSpace> target_address_space);
void validate_prepared_game_entry_cpu_mmu(
    const PreparedGameEntryCpuRestore& prepared,
    const RuntimeAddressSpaceSnapshot& expected);
void commit_prepared_game_entry_cpu_restore(
    CpuState& cpu,
    PreparedGameEntryCpuRestore prepared) noexcept;

class PreparedGameEntryCpuMemoryHandoff final {
  public:
    PreparedGameEntryCpuMemoryHandoff(
        const PreparedGameEntryCpuMemoryHandoff&) = delete;
    PreparedGameEntryCpuMemoryHandoff& operator=(
        const PreparedGameEntryCpuMemoryHandoff&) = delete;
    PreparedGameEntryCpuMemoryHandoff(
        PreparedGameEntryCpuMemoryHandoff&&) noexcept;
    PreparedGameEntryCpuMemoryHandoff& operator=(
        PreparedGameEntryCpuMemoryHandoff&&) noexcept;
    ~PreparedGameEntryCpuMemoryHandoff();

    [[nodiscard]] std::size_t memory_operation_count() const noexcept;
    [[nodiscard]] std::uint64_t memory_byte_count() const noexcept;
    [[nodiscard]] std::span<const GuestWriteEvent>
    memory_guest_write_events() const noexcept;
    void suppress_memory_guest_write_observer() noexcept;

  private:
    friend PreparedGameEntryCpuMemoryHandoff
    prepare_validated_game_entry_cpu_memory_handoff(
        CpuState&,
        DreamcastRuntimeState&,
        const ValidatedGameEntryHandoff&);
    friend void commit_prepared_game_entry_memory_handoff(
        CpuState&,
        PreparedGameEntryCpuMemoryHandoff&) noexcept;
    friend void bind_prepared_game_entry_cpu_mmu(
        PreparedGameEntryCpuMemoryHandoff&,
        RuntimeAddressSpaceSnapshot);
    friend void commit_prepared_game_entry_cpu_handoff(
        CpuState&,
        PreparedGameEntryCpuMemoryHandoff) noexcept;
    struct Data;
    PreparedGameEntryCpuMemoryHandoff();
    std::unique_ptr<Data> data_;
};

[[nodiscard]] PreparedGameEntryCpuMemoryHandoff
prepare_validated_game_entry_cpu_memory_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    const ValidatedGameEntryHandoff& handoff);
void commit_prepared_game_entry_memory_handoff(
    CpuState& cpu,
    PreparedGameEntryCpuMemoryHandoff& prepared) noexcept;
void bind_prepared_game_entry_cpu_mmu(
    PreparedGameEntryCpuMemoryHandoff& prepared,
    RuntimeAddressSpaceSnapshot expected);
void commit_prepared_game_entry_cpu_handoff(
    CpuState& cpu,
    PreparedGameEntryCpuMemoryHandoff prepared) noexcept;

struct GameEntryCodeRange {
    std::uint32_t start = 0u;
    std::uint32_t size = 0u;

    [[nodiscard]] bool operator==(const GameEntryCodeRange&) const = default;
};

enum class GameEntryMemoryRegion : std::uint8_t {
    MainRam,
    Vram,
    AicaRam,
};

struct GameEntryMemoryLayout {
    std::uint32_t main_ram_size = 0u;
    std::uint32_t vram_size = 0u;
    std::uint32_t aica_ram_size = 0u;

    [[nodiscard]] bool operator==(const GameEntryMemoryLayout&) const = default;
};

struct GameEntryPrivateSliceReference {
    // Opaque content identities only. Providers decide where local data lives.
    std::string artifact_identity;
    std::uint64_t artifact_offset = 0u;
    std::uint32_t size = 0u;
    std::string byte_identity;

    [[nodiscard]] bool operator==(const GameEntryPrivateSliceReference&) const = default;
};

enum class GameEntryMemoryOperationKind : std::uint8_t {
    Fill,
    CopyPrivateSlice,
};

struct GameEntryMemoryOperation {
    GameEntryMemoryRegion region = GameEntryMemoryRegion::MainRam;
    std::uint32_t offset = 0u;
    std::uint32_t size = 0u;
    GameEntryMemoryOperationKind kind = GameEntryMemoryOperationKind::Fill;
    std::uint8_t fill_value = 0u;
    GameEntryPrivateSliceReference private_slice;
    std::string expected_before_identity;
    std::string expected_after_identity;
    bool executable = false;

    [[nodiscard]] bool operator==(const GameEntryMemoryOperation&) const = default;
};

enum class GameEntryDeviceKind : std::uint16_t {
    Pvr = 1u,
    GdRom = 2u,
    G1 = 3u,
    Sh4Dmac = 4u,
    Aica = 5u,
    Maple = 6u,
    SystemBus = 7u,
    SystemAsic = 8u,
    InterruptController = 9u,
    InterruptRouter = 10u,
    InterruptRegisters = 11u,
    Mmu = 12u,
    Cache = 13u,
    StoreQueues = 14u,
    IoPorts = 15u,
    HollyG2Dma = 16u,
    HollyPvrDma = 17u,
    Sh4Tmu = 18u,
    Sh4RtcClock = 19u,
    Sh4Rtc = 20u,
    Sh4Scif = 21u,
    Flash = 22u,
};

struct GameEntryDeviceKey {
    GameEntryDeviceKind kind = GameEntryDeviceKind::Pvr;
    std::uint16_t instance = 0u;

    [[nodiscard]] bool operator==(const GameEntryDeviceKey&) const = default;
};

struct GameEntryDeviceScalar {
    std::uint32_t field_id = 0u;
    std::uint64_t value = 0u;

    [[nodiscard]] bool operator==(const GameEntryDeviceScalar&) const = default;
};

struct GameEntryDevicePayload {
    std::uint32_t field_id = 0u;
    GameEntryPrivateSliceReference private_slice;

    [[nodiscard]] bool operator==(const GameEntryDevicePayload&) const = default;
};

// Each device adapter owns the meaning of its monotonically numbered fields.
// State contract versions are per device kind and remain independent from the
// outer handoff schema.
struct GameEntryDeviceState {
    GameEntryDeviceKey key;
    std::uint32_t state_contract_version = 1u;
    std::vector<GameEntryDeviceScalar> scalars;
    std::vector<GameEntryDevicePayload> payloads;

    [[nodiscard]] bool operator==(const GameEntryDeviceState&) const = default;
};

struct GameEntryDeviceRequirement {
    GameEntryDeviceKey key;
    std::uint32_t state_contract_version = 1u;

    [[nodiscard]] bool operator==(const GameEntryDeviceRequirement&) const = default;
};

inline constexpr std::array<GameEntryDeviceRequirement, 22u>
    dreamcast_game_entry_required_devices_v2{{
        {{GameEntryDeviceKind::Pvr, 0u}, 1u},
        {{GameEntryDeviceKind::GdRom, 0u}, 1u},
        {{GameEntryDeviceKind::G1, 0u}, 1u},
        {{GameEntryDeviceKind::Sh4Dmac, 0u}, 1u},
        {{GameEntryDeviceKind::Aica, 0u}, 1u},
        {{GameEntryDeviceKind::Maple, 0u}, 1u},
        {{GameEntryDeviceKind::SystemBus, 0u}, 1u},
        {{GameEntryDeviceKind::SystemAsic, 0u}, 1u},
        {{GameEntryDeviceKind::InterruptController, 0u}, 1u},
        {{GameEntryDeviceKind::InterruptRouter, 0u}, 1u},
        {{GameEntryDeviceKind::InterruptRegisters, 0u}, 1u},
        {{GameEntryDeviceKind::Mmu, 0u}, 1u},
        {{GameEntryDeviceKind::Cache, 0u}, 1u},
        {{GameEntryDeviceKind::StoreQueues, 0u}, 1u},
        {{GameEntryDeviceKind::IoPorts, 0u}, 1u},
        {{GameEntryDeviceKind::HollyG2Dma, 0u}, 1u},
        {{GameEntryDeviceKind::HollyPvrDma, 0u}, 1u},
        {{GameEntryDeviceKind::Sh4Tmu, 0u}, 1u},
        {{GameEntryDeviceKind::Sh4RtcClock, 0u}, 1u},
        {{GameEntryDeviceKind::Sh4Rtc, 0u}, 1u},
        {{GameEntryDeviceKind::Sh4Scif, 0u}, 1u},
        {{GameEntryDeviceKind::Flash, 0u}, 1u},
    }};

struct GameEntryScheduledEvent {
    std::uint64_t guest_cycle = 0u;
    SchedulerEventKind kind = SchedulerEventKind::Unknown;
    GameEntryDeviceKey owner;
    std::uint32_t channel = 0u;
    std::uint64_t token = 0u;

    [[nodiscard]] bool operator==(const GameEntryScheduledEvent&) const = default;
};

struct GameEntrySchedulerState {
    std::uint64_t current_cycle = 0u;
    std::vector<GameEntryScheduledEvent> pending_events;

    [[nodiscard]] bool operator==(const GameEntrySchedulerState&) const = default;
};

enum class GameEntryHandoffCompleteness : std::uint8_t {
    CompletePlatform,
    CpuMemoryDiagnostic,
};

struct GameEntryHandoff {
    GameEntryHandoffBinding binding;
    GameEntryHandoffCompleteness completeness =
        GameEntryHandoffCompleteness::CompletePlatform;
    GameEntryControlTransfer transfer;
    GameEntryCpuState cpu;
    std::vector<GameEntryMemoryOperation> memory_operations;
    std::vector<GameEntryDeviceState> devices;
    GameEntrySchedulerState scheduler;
    // Digest of the normative state expected after a future application step.
    std::string expected_semantic_state_identity;

    [[nodiscard]] bool operator==(const GameEntryHandoff&) const = default;
};

struct GameEntryHandoffLimits {
    std::size_t maximum_memory_operations = 16'384u;
    std::size_t maximum_device_states = 128u;
    std::size_t maximum_device_scalars = 65'536u;
    std::size_t maximum_device_payloads = 16'384u;
    std::size_t maximum_scheduler_events = 16'384u;
    std::uint64_t maximum_staged_bytes = 64u * 1024u * 1024u;
};

struct GameEntryHandoffRequest {
    GameEntryHandoffBinding expected_binding;
    std::span<const GameEntryCodeRange> allowed_entry_ranges;
    GameEntryMemoryLayout memory_layout;
    std::span<const GameEntryDeviceRequirement> required_devices =
        dreamcast_game_entry_required_devices_v2;
    GameEntryHandoffLimits limits;
    GameEntryHandoffCompleteness required_completeness =
        GameEntryHandoffCompleteness::CompletePlatform;
};

using DescribeGameEntryHandoff =
    const GameEntryHandoff* (*)(void* context,
                               const GameEntryHandoffRequest& request) noexcept;
using ReadGameEntryPrivateSlice =
    bool (*)(void* context,
             const GameEntryPrivateSliceReference& reference,
             std::span<std::uint8_t> destination) noexcept;

struct GameEntryHandoffProvider {
    void* context = nullptr;
    DescribeGameEntryHandoff describe = nullptr;
    ReadGameEntryPrivateSlice read_private_slice = nullptr;
};

enum class GameEntryHandoffFailure : std::uint8_t {
    ProviderUnavailable,
    DescriptionUnavailable,
    BindingInvalid,
    BindingMismatch,
    DescriptorIdentityMismatch,
    CpuStateInvalid,
    MemoryLayoutInvalid,
    MemoryOperationInvalid,
    DeviceStateInvalid,
    SchedulerStateInvalid,
    PrivateSliceInvalid,
    PrivateSliceReadFailed,
    PrivateSliceIdentityMismatch,
    LimitExceeded,
    RuntimeStateInvalid,
    MemoryBeforeIdentityMismatch,
    MemoryAfterIdentityMismatch,
    CpuMemoryApplyFailed,
    CompletenessMismatch,
    DeviceStateApplyFailed,
    SchedulerStateApplyFailed,
    SemanticStateMismatch,
};

class GameEntryHandoffError final : public std::runtime_error {
  public:
    explicit GameEntryHandoffError(GameEntryHandoffFailure failure);

    [[nodiscard]] GameEntryHandoffFailure failure() const noexcept;

  private:
    GameEntryHandoffFailure failure_;
};

struct ValidatedGameEntryMemoryOperation {
    GameEntryMemoryOperation operation;
    std::vector<std::uint8_t> bytes;
};

struct ValidatedGameEntryDevicePayload {
    GameEntryDevicePayload payload;
    std::vector<std::uint8_t> bytes;
};

struct ValidatedGameEntryDeviceState {
    GameEntryDeviceKey key;
    std::uint32_t state_contract_version = 0u;
    std::vector<GameEntryDeviceScalar> scalars;
    std::vector<ValidatedGameEntryDevicePayload> payloads;
};

class ValidatedGameEntryHandoff final {
  public:
    ValidatedGameEntryHandoff(const ValidatedGameEntryHandoff&) = delete;
    ValidatedGameEntryHandoff&
    operator=(const ValidatedGameEntryHandoff&) = delete;
    ValidatedGameEntryHandoff(ValidatedGameEntryHandoff&&) noexcept = default;
    ValidatedGameEntryHandoff&
    operator=(ValidatedGameEntryHandoff&&) noexcept = default;
    ~ValidatedGameEntryHandoff() = default;

    [[nodiscard]] const GameEntryHandoffBinding& binding() const noexcept;
    [[nodiscard]] GameEntryHandoffCompleteness completeness() const noexcept;
    [[nodiscard]] const GameEntryControlTransfer& transfer() const noexcept;
    [[nodiscard]] const GameEntryCpuState& cpu() const noexcept;
    [[nodiscard]] std::span<const ValidatedGameEntryMemoryOperation>
    memory_operations() const noexcept;
    [[nodiscard]] std::span<const ValidatedGameEntryDeviceState>
    devices() const noexcept;
    [[nodiscard]] const GameEntrySchedulerState& scheduler() const noexcept;
    [[nodiscard]] const std::string&
    expected_semantic_state_identity() const noexcept;

  private:
    ValidatedGameEntryHandoff() = default;
    friend ValidatedGameEntryHandoff validate_and_stage_game_entry_handoff(
        const GameEntryHandoffRequest& request,
        const GameEntryHandoffProvider& provider);

    GameEntryHandoffBinding binding_;
    GameEntryHandoffCompleteness completeness_ =
        GameEntryHandoffCompleteness::CompletePlatform;
    GameEntryControlTransfer transfer_;
    GameEntryCpuState cpu_;
    std::vector<ValidatedGameEntryMemoryOperation> memory_operations_;
    std::vector<ValidatedGameEntryDeviceState> devices_;
    GameEntrySchedulerState scheduler_;
    std::string expected_semantic_state_identity_;
};

enum class GameEntryCpuMemoryApplyStatus : std::uint8_t {
    // The complete validated handoff contained no device or scheduler state.
    CpuMemoryAndControlTransferApplied,
    // CPU, memory and the exact PC/PR transfer were applied. Device and/or
    // scheduler state remains intentionally unapplied and must be handled by
    // the caller before treating the complete platform handoff as restored.
    CpuMemoryAndControlTransferAppliedPlatformStatePending,
};

struct GameEntryCpuMemoryApplyResult {
    GameEntryCpuMemoryApplyStatus status =
        GameEntryCpuMemoryApplyStatus::
            CpuMemoryAndControlTransferApplied;
    std::size_t memory_operations_applied = 0u;
    std::uint64_t memory_bytes_applied = 0u;
    bool device_state_pending = false;
    bool scheduler_state_pending = false;
    bool incomplete_handoff = false;

    [[nodiscard]] bool complete_platform_state_applied() const noexcept {
        return status ==
               GameEntryCpuMemoryApplyStatus::
                   CpuMemoryAndControlTransferApplied;
    }
};

[[nodiscard]] bool
valid_game_entry_content_identity(const std::string& identity) noexcept;
[[nodiscard]] bool
valid_game_entry_sha256_identity(const std::string& identity) noexcept;
[[nodiscard]] std::string
game_entry_semantic_state_identity(const GameEntryHandoff& handoff);
[[nodiscard]] std::string
game_entry_handoff_descriptor_identity(const GameEntryHandoff& handoff);

struct GameEntryMemorySnapshot {
    std::vector<std::uint8_t> main_ram;
    std::vector<std::uint8_t> vram;
    std::vector<std::uint8_t> aica_ram;
};

struct GameEntryMemoryDeltaPayload {
    std::uint32_t memory_operation_index = 0u;
    std::vector<std::uint8_t> bytes;
};

struct GameEntryMemoryDelta {
    std::vector<GameEntryMemoryOperation> operations;
    std::vector<GameEntryMemoryDeltaPayload> payloads;
    std::uint64_t changed_bytes = 0u;
};

struct CapturedGameEntryDevicePayload {
    GameEntryDeviceKey device;
    std::uint32_t field_id = 0u;
    std::string name;
    std::vector<std::uint8_t> bytes;
};

struct CapturedGameEntryPlatformState {
    std::vector<GameEntryDeviceState> devices;
    GameEntrySchedulerState scheduler;
    std::vector<CapturedGameEntryDevicePayload> payloads;
};

// Capture helpers are intentionally title-neutral. The snapshot is taken at
// the common initialized-runtime boundary; the delta is formed later at the
// exact executable-entry boundary without embedding private bytes in Katana.
[[nodiscard]] GameEntryMemorySnapshot
capture_game_entry_memory_snapshot(const DreamcastRuntimeState& runtime);
[[nodiscard]] GameEntryMemoryDelta capture_game_entry_memory_delta(
    const GameEntryMemorySnapshot& before,
    const DreamcastRuntimeState& after,
    std::uint32_t page_size = 4096u);
// Captures every guest-visible Dreamcast platform device required by the
// current platform-state contract. Device payload bytes remain owning and
// private; callers bind them into a local artifact.
[[nodiscard]] CapturedGameEntryPlatformState
capture_complete_game_entry_platform_state(
    const DreamcastRuntimeState& runtime);

// The returned plan owns every referenced byte. All structure, identity,
// bounds, completeness and provider reads are validated before it is returned;
// no CpuState, Memory, device or scheduler object is mutated here.
[[nodiscard]] ValidatedGameEntryHandoff
validate_and_stage_game_entry_handoff(
    const GameEntryHandoffRequest& request,
    const GameEntryHandoffProvider& provider);

// Atomically commits every staged MainRam/VRAM/AicaRam operation and then the
// physical-bank CPU state plus exact PC/PR control transfer. All ranges,
// staged-byte hashes and expected-before hashes are checked before mutation.
// Device adapters and scheduler events are never synthesized by this partial
// application API; the nodiscard result reports whether such state remains.
[[nodiscard]] GameEntryCpuMemoryApplyResult
apply_validated_game_entry_cpu_memory_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    const ValidatedGameEntryHandoff& handoff);

// Stable, domain-separated identities of the normative state represented by
// one prepared complete-platform commit. These are computed before the live
// runtime is mutated. Memory identifies the complete set of target spans and
// bytes written by the handoff; the other fields identify the resulting
// subsystem state (with the selected observation/persistence profile applied).
struct GameEntryCompletePlatformExpectedIdentities {
    std::string cpu;
    std::string memory;
    std::string pvr;
    std::string aica;
    std::string maple;
    std::string scheduler;
    std::string irq;

    [[nodiscard]] bool operator==(
        const GameEntryCompletePlatformExpectedIdentities&) const = default;
};

struct GameEntryCompletePlatformApplyResult {
    std::size_t memory_operations_applied = 0u;
    std::uint64_t memory_bytes_applied = 0u;
    std::size_t devices_applied = 0u;
    std::size_t scheduler_events_rehydrated = 0u;
    GameEntryCompletePlatformExpectedIdentities expected_identities;
};

enum class GameEntryCompletePlatformRestoreProfile : std::uint8_t {
    DiagnosticLossless,
    ProductHandoff,
};

class PreparedCompletePlatformHandoff final {
  public:
    PreparedCompletePlatformHandoff(
        const PreparedCompletePlatformHandoff&) = delete;
    PreparedCompletePlatformHandoff& operator=(
        const PreparedCompletePlatformHandoff&) = delete;
    PreparedCompletePlatformHandoff(
        PreparedCompletePlatformHandoff&&) noexcept;
    PreparedCompletePlatformHandoff& operator=(
        PreparedCompletePlatformHandoff&&) noexcept;
    ~PreparedCompletePlatformHandoff();

    [[nodiscard]] const GameEntryCompletePlatformExpectedIdentities&
    expected_identities() const noexcept;

  private:
    friend PreparedCompletePlatformHandoff
    prepare_validated_game_entry_complete_platform_handoff(
        CpuState&,
        DreamcastRuntimeState&,
        const ValidatedGameEntryHandoff&,
        GameEntryCompletePlatformRestoreProfile);
    friend GameEntryCompletePlatformApplyResult
    commit_prepared_game_entry_complete_platform_handoff(
        CpuState&,
        DreamcastRuntimeState&,
        PreparedCompletePlatformHandoff) noexcept;
    struct Data;
    PreparedCompletePlatformHandoff();
    std::unique_ptr<Data> data_;
};

// Decodes and validates all 22 device contracts and their exact typed-event
// bijection before committing. The target runtime must already be fully
// constructed and quiescent. Any incompatibility and every allocation fails
// before the first live mutation. ProductHandoff resets host observations and
// preserves target-owned persistent storage; DiagnosticLossless reproduces
// captured diagnostics and persistence bytes.
[[nodiscard]] PreparedCompletePlatformHandoff
prepare_validated_game_entry_complete_platform_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    const ValidatedGameEntryHandoff& handoff,
    GameEntryCompletePlatformRestoreProfile profile);
[[nodiscard]] GameEntryCompletePlatformApplyResult
commit_prepared_game_entry_complete_platform_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    PreparedCompletePlatformHandoff prepared) noexcept;
[[nodiscard]] GameEntryCompletePlatformApplyResult
apply_validated_game_entry_complete_platform_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    const ValidatedGameEntryHandoff& handoff,
    GameEntryCompletePlatformRestoreProfile profile =
        GameEntryCompletePlatformRestoreProfile::DiagnosticLossless);

} // namespace katana::runtime
