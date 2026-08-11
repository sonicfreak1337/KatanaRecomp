#pragma once

#include "katana/build_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace katana::runtime {

struct CpuState;

inline constexpr std::uint32_t native_port_profile_contract_version =
    build_contract::native_port_profile_contract_version;
inline constexpr std::uint32_t native_port_definition_contract_version = 1u;

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
    DiagnosticOnly
};

enum class NativePortHookOriginalPolicy : std::uint8_t {
    ReplacesOriginal,
    MayContinueOriginal
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
    std::uint32_t covered_size = 0u;
    NativePortHookKind kind = NativePortHookKind::FunctionEntry;
    NativePortHookRequirement requirement =
        NativePortHookRequirement::Required;
    NativePortHookOriginalPolicy original_policy =
        NativePortHookOriginalPolicy::ReplacesOriginal;
    std::string_view symbol;
    // SHA-256 over exactly [guest_address, guest_address + covered_size).
    std::string_view code_identity;
};

enum class NativePortHardwareResolutionKind : std::uint8_t {
    // The complete effective-address set at this instruction was proven to
    // remain inside an ordinary native memory binding.
    NativeMemory,
    // The instruction is unreachable after the named covering native hook.
    ReplacedByHook
};

enum class NativePortMemoryAccess : std::uint8_t {
    Read = 1u << 0u,
    Write = 1u << 1u,
    Prefetch = 1u << 2u,
};

[[nodiscard]] constexpr std::uint8_t
native_port_memory_access_mask(const NativePortMemoryAccess access) noexcept {
    return static_cast<std::uint8_t>(access);
}

inline constexpr std::uint8_t native_port_memory_width_u8 = 1u << 0u;
inline constexpr std::uint8_t native_port_memory_width_u16 = 1u << 1u;
inline constexpr std::uint8_t native_port_memory_width_u32 = 1u << 2u;
inline constexpr std::uint8_t native_port_memory_width_cache_line_32 =
    1u << 3u;

struct NativePortHardwareResolution final {
    std::uint32_t instruction_address = 0u;
    NativePortHardwareResolutionKind kind =
        NativePortHardwareResolutionKind::NativeMemory;
    std::uint32_t hook_guest_address = 0u;
    // NativeMemory is an explicit, bounded proof over one verified image,
    // access class and width set. It is not a blanket waiver for a dynamic
    // effective address. ReplacedByHook leaves these fields empty/zero.
    std::string_view native_memory_image_id;
    std::uint32_t native_memory_guest_address = 0u;
    std::uint32_t native_memory_byte_size = 0u;
    std::uint8_t native_memory_access_mask = 0u;
    std::uint8_t native_memory_width_mask = 0u;
};

struct NativePortBootstrap final {
    std::uint32_t entry_point = 0u;
    std::uint32_t stack_pointer = 0u;
    std::uint32_t vector_base = 0u;
    std::uint32_t status_register = 0u;
    std::uint32_t fpscr = 0u;
    // Required title-owned symbol called after verified image mapping and
    // before the first recompiled game entry.  It initializes native title
    // state only; it must not boot firmware or construct guest devices.
    std::string_view symbol;
};

struct NativePortDefinition final {
    std::uint32_t contract_version =
        native_port_definition_contract_version;
    std::string_view project_id;
    std::string_view project_version;
    NativePortExecutableIdentity executable;
    NativePortBootstrap bootstrap;
    std::span<const NativePortImageBinding> images;
    std::span<const NativePortHookBinding> hooks;
    std::span<const NativePortHardwareResolution> hardware_resolutions;
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
    virtual void begin_frame(std::uint64_t frame_index) = 0;
    virtual void present_frame(std::uint64_t frame_index) = 0;
};

struct NativePortContext;

// The generated product owns these two bridges. Native title hooks can invoke
// the exact displaced original entry or re-enter statically recompiled game
// code without a runtime address lookup, decoder or interpreter fallback.
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
};

struct NativePortContext final {
    CpuState* cpu = nullptr;
    NativePortHostServices* host = nullptr;
    NativePortAotBridge aot;
    void* title_state = nullptr;
    std::uint64_t frame_index = 0u;
    std::uint64_t host_deadline_nanoseconds = 0u;
    NativePortStopReason stop_reason = NativePortStopReason::None;
};

enum class NativePortContractFailure : std::uint8_t {
    InvalidDefinition,
    InvalidHookResult,
    MissingRequiredHook,
    UnresolvedHardwareAccess,
    BootstrapFailed
};

class NativePortContractError final : public std::runtime_error {
  public:
    NativePortContractError(NativePortContractFailure failure,
                            std::string_view detail);

    [[nodiscard]] NativePortContractFailure failure() const noexcept;

  private:
    NativePortContractFailure failure_;
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
