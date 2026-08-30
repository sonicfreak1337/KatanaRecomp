#pragma once

#include "katana/abi_contract.hpp"
#include "katana/runtime/native_port_semantics.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace katana::runtime {

struct CpuState;
struct CrashCapsule;
class NativePortGraphicsDevice;
class NativePortPlatformServices;
class NativePortTextureRegistry;
class NativePortCpuControl;
class NativePortRuntimeImageBindings;
class NativePortLoadedAotBinder;
class NativePortTelemetry;
class NativePortTelemetryWriter;

inline constexpr std::uint32_t native_port_profile_contract_version =
    abi_contract::native_port_profile_contract_version;
inline constexpr std::uint32_t native_port_definition_contract_version = 13u;

struct NativePortLinkContract final {
    std::uint32_t version = native_port_profile_contract_version;
    bool allows_guest_cpu_interpreter = false;
    bool allows_legacy_device_runtime = false;
    bool allows_software_pvr = false;
    bool allows_ta_packet_renderer = false;
    bool allows_aica_command_translation = false;
};

[[nodiscard]] const NativePortLinkContract&
native_port_link_contract() noexcept;

// This contract deliberately contains no Dreamcast boot, device, scheduler,
// PlatformServices, PVR, AICA or firmware types.  A title project supplies
// identity-bound static intent at export time; generated code then links its
// native adapter symbols directly.
enum class NativePortHookKind : std::uint8_t {
    FunctionEntry,
    Instruction
};

enum class NativePortHookRequirement : std::uint8_t {
    Required,
    // Executable only in the explicitly incomplete bring-up product. It may
    // observe or stop at a boundary, but can never discharge hardware closure
    // or witness product acceptance.
    BringUpProbe,
    DiagnosticOnly
};

[[nodiscard]] constexpr bool native_port_hook_is_executable(
    const NativePortHookRequirement requirement) noexcept {
    return requirement == NativePortHookRequirement::Required ||
           requirement == NativePortHookRequirement::BringUpProbe;
}

[[nodiscard]] constexpr bool native_port_hook_closes_product_contract(
    const NativePortHookRequirement requirement) noexcept {
    return requirement == NativePortHookRequirement::Required;
}

enum class NativePortHookOriginalPolicy : std::uint8_t {
    ReplacesOriginal,
    MayContinueOriginal
};

// Identifies the immutable byte authority for a hook boundary.  Static-image
// hooks are contained by NativePortDefinition::images.  A latent AOT module
// is instead discovered from the current disc, transformed and hash-bound by
// the analyzer before its native body exists; it must therefore name that
// module identity explicitly rather than inventing a static image mapping.
enum class NativePortHookCodeSource : std::uint8_t {
    StaticImage,
    LatentAotModule
};

enum class NativePortHookAction : std::uint8_t {
    ContinueOriginal,
    Jump,
    Return,
    Abort
};

struct NativePortHookResult final {
    NativePortHookAction action = NativePortHookAction::ContinueOriginal;
    std::uint32_t target = 0u;
    std::uint32_t error_code = 0u;
};

struct NativePortExecutableIdentity final {
    std::string_view content_identity;
    std::string_view executable_name;
    std::string_view executable_byte_identity;
};

// Exact source bytes are owned by the title/content installer, never by the
// generic Katana repository.  A binding describes how verified bytes enter
// ordinary guest memory before the statically recompiled entry is called.
struct NativePortImageBinding final {
    std::string_view image_id;
    std::string_view content_relative_path;
    std::string_view byte_identity;
    std::uint64_t file_offset = 0u;
    std::uint32_t guest_address = 0u;
    std::uint32_t byte_size = 0u;
    bool writable = true;
};

struct NativePortHookBinding final {
    std::uint32_t guest_address = 0u;
    // Instruction hooks cover one architectural instruction: two bytes for
    // an ordinary SH-4 instruction, or four bytes when its mandatory delay
    // slot is part of the atomic replacement boundary.
    std::uint32_t covered_size = 0u;
    NativePortHookKind kind = NativePortHookKind::FunctionEntry;
    NativePortHookRequirement requirement =
        NativePortHookRequirement::Required;
    NativePortHookOriginalPolicy original_policy =
        NativePortHookOriginalPolicy::ReplacesOriginal;
    std::string_view symbol;
    // SHA-256 over exactly [guest_address, guest_address + covered_size).
    std::string_view code_identity;
    // SHA-256 of the native implementation component which exports `symbol`.
    // It is a separate authority domain from the guest code identity and the
    // semantic declaration.  Hooks without an authoritative semantic
    // contract may leave it empty; a closing contract may not.
    std::string_view provider_implementation_identity;
    NativePortHookCodeSource code_source =
        NativePortHookCodeSource::StaticImage;
    // Empty for StaticImage.  LatentAotModule binds the complete transformed
    // module identity; export admission separately proves that the exact
    // hook range and code_identity belong to one function in that module.
    std::string_view code_source_identity;
};

// Hardware instructions may be discharged only by a required, complete
// native replacement hook. Ordinary dynamic RAM accesses use the generated
// range guard and typed failure path; they are not a declarative proof.
struct NativePortHardwareResolution final {
    std::uint32_t instruction_address = 0u;
    std::uint32_t hook_guest_address = 0u;
};

enum class NativePortBootstrapWritePolicy : std::uint8_t {
    // Ordinary title data may change, but executable/read-only bytes remain
    // bound to the verified images used by static code generation.
    WritableDataOnly,
    // A checkpoint may intentionally materialize identity-bound executable or
    // read-only bytes. Both the complete pre- and post-range identities are
    // mandatory, so this is an explicit transition rather than an unguarded
    // code patch.
    IdentityBoundImmutableMaterialization,
};

// A title bootstrap may materialize only these ordinary-RAM ranges. Complete
// pre- and post-bootstrap identities bind the transition. Every changed byte
// outside the declared ranges is rejected; executable/read-only changes also
// require the explicit identity-bound policy.
struct NativePortBootstrapWriteBinding final {
    std::uint32_t guest_address = 0u;
    std::uint32_t byte_size = 0u;
    std::string_view pre_write_identity;
    std::string_view post_write_identity;
    NativePortBootstrapWritePolicy policy =
        NativePortBootstrapWritePolicy::WritableDataOnly;
};

// A checkpoint may resume in the middle of one or more still-live caller
// frames.  These are typed control-flow roots, not synthetic functions:
// `function_entry` owns the complete statically analyzed body while
// `resume_address` is an externally reachable block/architectural resume
// within that body.  Private title tooling may derive the pairs from a
// verified checkpoint stack or disassembly; the generic exporter proves the
// relationship against its own post-image CFG before admitting it.
struct NativePortAotContinuationBinding final {
    std::uint32_t function_entry = 0u;
    std::uint32_t resume_address = 0u;
};

enum class NativePortBootstrapTimePolicy : std::uint8_t {
    // The checkpoint has no emulated scheduler/device epoch. Every retained
    // title time boundary must be supplied by a native provider from a fresh
    // monotonic host epoch before post-entry dispatch begins.
    NativeHostEpoch,
};

struct NativePortBootstrap final {
    std::uint32_t entry_point = 0u;
    std::uint32_t stack_pointer = 0u;
    std::uint32_t vector_base = 0u;
    std::uint32_t status_register = 0u;
    std::uint32_t fpscr = 0u;
    // Persistent guest-visible SH-4 CCR configuration at the native entry.
    // Invalidation command bits are forbidden here because they self-clear;
    // the product maps only this CPU-control register, never cache arrays.
    std::uint32_t cache_control_value = 0u;
    // The title bootstrap must leave PC at this exact post-checkpoint entry.
    // Static analysis/codegen uses only the post-AOT roots below, never the
    // loader's pre-checkpoint entry points.  `post_aot_roots` contains true
    // function entries; mid-function checkpoint PCs and suspended caller
    // returns belong in `post_aot_continuations`.
    std::uint32_t post_entry_point = 0u;
    std::span<const std::uint32_t> post_aot_roots;
    std::span<const NativePortAotContinuationBinding>
        post_aot_continuations;
    NativePortBootstrapTimePolicy time_policy =
        NativePortBootstrapTimePolicy::NativeHostEpoch;
    // Required title-owned symbol called after verified image mapping and
    // before the first recompiled game entry. It may finish the initial
    // identity-bound RAM image and native title state while runtime write
    // guards are deliberately inactive. Generated code snapshots the complete
    // native RAM backing and validates declared post identities before the
    // runtime guard is installed. AOT bridges and guest execution are
    // unavailable until it returns successfully; it must not boot firmware or
    // construct guest devices.
    std::string_view symbol;
    std::string_view post_cpu_state_identity;
    std::span<const NativePortBootstrapWriteBinding> writes;
};

// The generic runtime never embeds a title address. The private title adapter
// binds one stable milestone name and marks it reached only at that exact
// native gameplay boundary (for example, its main menu).
struct NativePortAcceptanceBinding final {
    std::string_view milestone_id;
    // Only this identity-bound private hook may report the milestone. The
    // runtime additionally requires a successfully completed native GPU
    // presentation after bootstrap before accepting the witness.
    std::uint32_t witness_hook_guest_address = 0u;
};

// Title simulation cadence is part of the native product contract rather
// than a Dreamcast device clock.  Presentation may run faster by repeating a
// completed GPU frame, but it can never advance title state more frequently
// than simulation_rate_hz.
struct NativePortFrameTimingBinding final {
    std::uint32_t simulation_rate_hz = 60u;
    std::uint32_t default_presentation_rate_hz = 60u;
    std::uint32_t maximum_presentation_rate_hz = 1'000u;
};

// A native product has exactly one live host-controller owner. Generic ports
// retain the Maple device projection. Titles which replace their complete SDK
// peripheral update family instead consume NativePortPlatformServices input
// directly; the generated Maple device then remains attached but neutral so
// one physical edge cannot be published through both contracts.
enum class NativePortInputOwnership : std::uint8_t {
    MapleDevice,
    NativeTitleProjection,
};

struct NativePortDefinition final {
    std::uint32_t contract_version =
        native_port_definition_contract_version;
    std::string_view project_id;
    std::string_view project_version;
    NativePortExecutableIdentity executable;
    NativePortBootstrap bootstrap;
    NativePortAcceptanceBinding acceptance;
    // Checkpoint-resident runtime images are title-owned snapshots whose
    // source addresses differ from their live fixed-address destinations.
    // This contract deliberately does not model later overlay lifecycles:
    // those require a separate identity-bound native load/unload provider and
    // must never be installed as global mappings by this bootstrap profile.
    std::span<const std::string_view> checkpoint_runtime_image_ids;
    std::span<const NativePortImageBinding> images;
    std::span<const NativePortHookBinding> hooks;
    std::span<const NativePortHardwareResolution> hardware_resolutions;
    NativePortFrameTimingBinding frame_timing{};
    // Static provider semantics are optional in the legacy definition shape,
    // but authoritative entries are validated as required replacements.  The
    // field is trailing so existing generated aggregate initializers remain
    // source-compatible and simply carry an empty span until the exporter
    // emits semantic contracts.
    std::span<const NativePortProviderSemanticContract>
        provider_semantic_contracts;
    NativePortProviderSemanticCoverage provider_semantic_coverage =
        NativePortProviderSemanticCoverage::DeclaredOnly;
    NativePortInputOwnership input_ownership =
        NativePortInputOwnership::MapleDevice;
};

enum class NativePortLifecycleState : std::uint8_t {
    Running,
    Paused,
    Shutdown
};

// Native host time and frame presentation are explicit title boundaries.
// They are intentionally unrelated to Dreamcast CPU MHz, ASIC interrupts or
// VBlank register emulation.
class NativePortHostServices {
  public:
    virtual ~NativePortHostServices() = default;

    [[nodiscard]] virtual std::uint64_t monotonic_time_nanoseconds()
        const noexcept = 0;
    [[nodiscard]] virtual NativePortLifecycleState poll_lifecycle() = 0;
    // Synchronize a title-level frame boundary with the native simulation
    // cadence.  This is the semantic replacement for SDK busy-waits on a
    // guest scanline/vblank register: the product waits on host monotonic
    // time and never constructs a video-status device or register value.
    virtual void synchronize_simulation_boundary() = 0;
    virtual void begin_frame(std::uint64_t frame_index) = 0;
    // Complete one title simulation frame. If the title did not open a new
    // GPU frame, the desktop host repeats the last completed image instead of
    // manufacturing and presenting an empty clear frame.
    virtual void present_frame(std::uint64_t frame_index) = 0;
    [[nodiscard]] virtual std::uint64_t presented_frames()
        const noexcept = 0;
};

class NativePortContext;

enum class NativePortImmutableRangeKind : std::uint8_t {
    Executable = 1u << 0u,
    ReadOnlyImage = 1u << 1u,
};

[[nodiscard]] constexpr std::uint8_t native_port_immutable_range_mask(
    const NativePortImmutableRangeKind kind) noexcept {
    return static_cast<std::uint8_t>(kind);
}

struct NativePortImmutableRange final {
    std::uint32_t physical_address = 0u;
    std::uint32_t byte_size = 0u;
    std::uint8_t kind_mask = 0u;

    [[nodiscard]] bool operator==(
        const NativePortImmutableRange&) const = default;
};

// The generated product owns these two bridges. Native title hooks can invoke
// the exact displaced original entry or re-enter an identity-bound external
// callback root which participated in static analysis, without a runtime
// address lookup, decoder or interpreter fallback. Arbitrary AOT entries are
// deliberately not callback capabilities.
using NativePortAotEntryBridge = NativePortHookResult (*)(
    NativePortContext& context,
    std::uint32_t guest_address) noexcept;

struct NativePortAotBridge final {
    NativePortAotEntryBridge invoke_original = nullptr;
    NativePortAotEntryBridge invoke_callback = nullptr;
};

using NativePortHookFunction = NativePortHookResult (*)(
    NativePortContext& context) noexcept;

struct NativePortBootstrapResult final {
    bool ready = false;
    std::uint32_t error_code = 0u;
};

using NativePortBootstrapFunction = NativePortBootstrapResult (*)(
    NativePortContext& context) noexcept;

enum class NativePortStopReason : std::uint8_t {
    None,
    HostRequested,
    HostDeadline,
    HookAbort,
    MissingStaticEntry,
    ExecutableCodeWrite,
    ReadOnlyImageWrite,
    GuestExceptionOrSleep,
    AotContractViolation,
    UnresolvedHardwareAccess,
    ForbiddenHardwareOperation,
};

enum class NativePortBootstrapPhase : std::uint8_t {
    NotStarted,
    Running,
    Completed,
    Failed,
};

class NativePortContext final {
  public:
    CpuState* cpu = nullptr;
    NativePortHostServices* host = nullptr;
    NativePortGraphicsDevice* graphics = nullptr;
    NativePortPlatformServices* platform = nullptr;
    NativePortTextureRegistry* textures = nullptr;
    NativePortCpuControl* cpu_control = nullptr;
    NativePortRuntimeImageBindings* runtime_images = nullptr;
    NativePortLoadedAotBinder* loaded_aot = nullptr;
    CrashCapsule* crash_capsule = nullptr;
    NativePortAotBridge aot;
    void* title_state = nullptr;
    std::uint64_t frame_index = 0u;
    std::uint64_t host_deadline_nanoseconds = 0u;
    NativePortStopReason stop_reason = NativePortStopReason::None;
    NativePortBootstrapPhase bootstrap_phase =
        NativePortBootstrapPhase::NotStarted;

    // Generated product code binds the private acceptance witness only after
    // bootstrap has completed. Title code can report it only while that exact
    // hook is executing and only after a newer native frame was presented.
    void bind_acceptance(const NativePortAcceptanceBinding& binding) noexcept;
    [[nodiscard]] std::uint32_t begin_hook_dispatch(
        std::uint32_t guest_address) noexcept;
    void end_hook_dispatch(std::uint32_t previous_guest_address) noexcept;
    [[nodiscard]] bool report_acceptance(
        std::string_view milestone_id) noexcept;
    [[nodiscard]] bool acceptance_reached() const noexcept;

  private:
    std::string_view acceptance_milestone_id_;
    std::uint32_t acceptance_witness_hook_guest_address_ = 0u;
    std::uint32_t active_hook_guest_address_ = 0u;
    std::uint64_t acceptance_presented_frame_baseline_ = 0u;
    bool acceptance_reached_ = false;

  public:
    // Append-only v25 fields: keep all pre-v25 NativePortContext offsets
    // stable for sealed AOT objects.  These are data-only service pointers;
    // guest state, memory, and AOT capabilities never cross to worker
    // threads through them.
    NativePortTelemetry* telemetry = nullptr;
    NativePortTelemetryWriter* telemetry_writer = nullptr;
};

enum class NativePortContractFailure : std::uint8_t {
    InvalidDefinition,
    InvalidHookResult,
    MissingRequiredHook,
    UnresolvedHardwareAccess,
    BootstrapFailed,
    MissingStaticEntry,
    ForbiddenHardwareOperation,
    ImmutableMemoryWrite,
    HookAborted,
    GuestExceptionOrSleep,
    AotContractViolation,
    ContentLoadFailed
};

class NativePortContractError final : public std::runtime_error {
  public:
    NativePortContractError(NativePortContractFailure failure,
                            std::string_view detail);

    [[nodiscard]] NativePortContractFailure failure() const noexcept;
    [[nodiscard]] std::string_view detail() const noexcept;

  private:
    NativePortContractFailure failure_;
    std::string detail_;
};

[[nodiscard]] bool
valid_native_port_sha256_identity(std::string_view identity) noexcept;
[[nodiscard]] bool
valid_native_port_link_symbol(std::string_view symbol) noexcept;
[[nodiscard]] bool
valid_native_port_hook_result(const NativePortHookResult& result) noexcept;
[[nodiscard]] bool valid_native_port_hook_result(
    const NativePortHookBinding& binding,
    const NativePortHookResult& result) noexcept;
void validate_native_port_definition(const NativePortDefinition& definition);

} // namespace katana::runtime
