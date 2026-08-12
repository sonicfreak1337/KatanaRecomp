#pragma once

#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/game_entry_handoff.hpp"
#include "katana/runtime/native_aot_template.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace katana::runtime {

class PlatformServices;

inline constexpr std::uint32_t game_project_contract_version = 7u;

enum class RequiredProductMilestone : std::uint8_t {
    BootExecutableEntry,
    GameCodeProgressed,
    FirstGameFramebufferWrite,
    FirstTaFrame,
    FirstVisibleGameFrame,
    MainMenuPresented
};

enum class GameProjectControlTransferKind : std::uint8_t {
    Jump,
    Call
};

enum class GameProjectTableEncoding : std::uint8_t {
    Absolute32,
    SignedRelative16,
    SignedRelative32
};

struct GameProjectFunctionBoundary {
    std::uint32_t start = 0u;
    std::uint32_t size = 0u;
    std::string_view symbol;
    // Empty binds metadata to the immutable base image. Runtime/overlay
    // metadata names its owning GameProjectRuntimeImage explicitly.
    std::string_view image_id;
};

struct GameProjectJumpTable {
    std::uint32_t dispatch_address = 0u;
    std::uint32_t table_address = 0u;
    std::uint32_t entry_count = 0u;
    std::uint32_t entry_stride = 0u;
    std::uint32_t relative_base = 0u;
    GameProjectTableEncoding encoding = GameProjectTableEncoding::Absolute32;
    GameProjectControlTransferKind transfer = GameProjectControlTransferKind::Jump;
    std::string_view image_id;
};

struct GameProjectCallbackTable {
    std::uint32_t table_address = 0u;
    std::uint32_t entry_count = 0u;
    std::uint32_t entry_stride = sizeof(std::uint32_t);
    std::uint32_t pointer_offset = 0u;
    GameProjectControlTransferKind transfer = GameProjectControlTransferKind::Call;
    std::string_view image_id;
};

enum class GameProjectFunctionOverrideStrength : std::uint8_t {
    // A null weak callback means that the generated AOT function remains active.
    Weak,
    Required
};

using GameProjectHookStrength = GameProjectFunctionOverrideStrength;

enum class GameProjectHookAction : std::uint8_t {
    Continue,
    Jump,
    Return,
    Abort
};

struct GameProjectHookResult {
    GameProjectHookAction action = GameProjectHookAction::Continue;
    std::uint32_t target = 0u;
    std::uint32_t error_code = 0u;
};

using GameProjectHookCondition =
    bool (*)(const CpuState&, const PlatformServices*, const void*) noexcept;
using GameProjectNativeHook =
    GameProjectHookResult (*)(CpuState&, PlatformServices*, void*) noexcept;

struct GameProjectFunctionOverride {
    std::uint32_t function_address = 0u;
    GameProjectNativeHook callback = nullptr;
    GameProjectHookCondition condition = nullptr;
    void* user_context = nullptr;
    GameProjectFunctionOverrideStrength strength =
        GameProjectFunctionOverrideStrength::Weak;
};

struct GameProjectMidFunctionHook {
    std::uint32_t instruction_address = 0u;
    GameProjectNativeHook callback = nullptr;
    GameProjectHookCondition condition = nullptr;
    void* user_context = nullptr;
    GameProjectHookStrength strength = GameProjectHookStrength::Required;
};

enum class GameProjectSymbolKind : std::uint8_t {
    Unknown,
    Function,
    Object
};

struct GameProjectSymbol {
    std::uint32_t address = 0u;
    std::string_view name;
    std::uint32_t size = 0u;
    GameProjectSymbolKind kind = GameProjectSymbolKind::Unknown;
};

// Optional fine-grained identity proof for code outside the immutable boot
// executable (for example an overlay or a runtime-loaded module). The identity
// is SHA-256 over exactly [address, address + size). The exporter validates
// these ranges before accepting title-owned hooks or metadata.
struct GameProjectCodeIdentity {
    std::uint32_t address = 0u;
    std::uint32_t size = 0u;
    std::string_view byte_identity;
    std::string_view image_id;
};

// Identity-bound external runtime-image descriptor. The source range
// describes the linked image address while runtime_start describes the
// direct-mapped guest destination. Retail bytes are supplied privately to the
// exporter and are never owned by this persistent project contract. Runtime
// entry points are offsets into byte_size and never inferred from mutable
// guest memory.
struct GameProjectRuntimeImage {
    std::string_view image_id;
    std::string_view byte_identity;
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t byte_size = 0u;
    std::span<const std::uint32_t> entry_offsets;
};

struct GameProjectIdentityBinding {
    std::string_view content_identity;
    std::string_view boot_file_name;
    std::string_view boot_byte_identity;
};

// The external project owns all arrays and strings referenced by these spans
// for at least as long as GameProjectBindings is used. No title data is copied
// into KatanaRuntime merely by constructing this view.
struct GameProjectDefinition {
    std::uint32_t contract_version = game_project_contract_version;
    std::string_view project_id;
    std::string_view project_version;
    GameProjectIdentityBinding identity;
    RequiredProductMilestone required_product_milestone =
        RequiredProductMilestone::FirstVisibleGameFrame;
    std::span<const GameProjectFunctionBoundary> function_boundaries;
    std::span<const GameProjectJumpTable> jump_tables;
    std::span<const GameProjectCallbackTable> callback_tables;
    std::span<const NativeAotTemplate> runtime_code_templates;
    std::span<const GameProjectFunctionOverride> function_overrides;
    std::span<const GameProjectMidFunctionHook> mid_function_hooks;
    std::span<const GameProjectSymbol> symbols;
    std::span<const GameProjectCodeIdentity> code_identities;
    std::span<const GameProjectRuntimeImage> runtime_images;
    std::optional<DreamcastRuntimeBootConfig> boot_config;
    // Declarative and identity-bound only. The provider that owns any private
    // descriptor or slice bytes is attached separately at runtime.
    std::optional<GameEntryHandoffBinding> game_entry_handoff;
};

struct GameProjectRuntimeProviders {
    GameEntryHandoffProvider game_entry_handoff;
};

enum class GameProjectHookApplication : std::uint8_t {
    Continue,
    ControlTransfer,
    Abort,
    Invalid
};

enum class GameProjectHookDispatchStatus : std::uint8_t {
    NotRegistered,
    NotFound,
    Disabled,
    Applied,
    Invalid
};

struct GameProjectHookDispatchResult {
    GameProjectHookDispatchStatus status =
        GameProjectHookDispatchStatus::NotRegistered;
    GameProjectHookApplication application =
        GameProjectHookApplication::Continue;
    std::uint32_t error_code = 0u;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == GameProjectHookDispatchStatus::Applied;
    }
};

enum class GameProjectHookContractFailure : std::uint8_t {
    InvalidResult,
    ContinueChangedProgramCounter,
    StaleBlockExecution
};

class GameProjectHookContractError final : public std::runtime_error {
  public:
    explicit GameProjectHookContractError(
        GameProjectHookContractFailure failure);

    [[nodiscard]] GameProjectHookContractFailure failure() const noexcept;

  private:
    GameProjectHookContractFailure failure_;
};

[[nodiscard]] bool
valid_game_project_hook_result(const GameProjectHookResult& result) noexcept;
[[nodiscard]] bool
valid_game_project_sha256_identity(std::string_view identity) noexcept;
[[nodiscard]] std::string_view required_product_milestone_name(
    RequiredProductMilestone milestone) noexcept;
[[nodiscard]] bool game_project_code_identity_matches(
    const GameProjectCodeIdentity& identity,
    std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool game_project_function_override_enabled(
    const GameProjectFunctionOverride& function_override,
    const CpuState& cpu,
    const PlatformServices* services) noexcept;
[[nodiscard]] bool game_project_mid_function_hook_enabled(
    const GameProjectMidFunctionHook& hook,
    const CpuState& cpu,
    const PlatformServices* services) noexcept;
[[nodiscard]] GameProjectHookApplication
apply_game_project_hook_result(CpuState& cpu,
                               const GameProjectHookResult& result) noexcept;

void validate_game_project_definition(const GameProjectDefinition& definition);
[[nodiscard]] std::string
game_project_definition_identity(const GameProjectDefinition& definition);
void validate_game_project_boot_identity(
    const GameProjectDefinition& definition,
    const DreamcastRuntimeBootImage& boot);

// Produces a fully identity-bound boot contract. The returned value owns its
// strings and can safely outlive the external definition's string views.
[[nodiscard]] DreamcastRuntimeBootConfig bind_game_project_boot_config(
    const GameProjectDefinition& definition,
    const DreamcastRuntimeBootImage& boot);

class GameProjectBindings final {
  public:
    explicit GameProjectBindings(
        GameProjectDefinition definition,
        GameProjectRuntimeProviders runtime_providers = {});

    [[nodiscard]] const GameProjectDefinition& definition() const noexcept;
    [[nodiscard]] const GameProjectRuntimeProviders&
    runtime_providers() const noexcept;
    [[nodiscard]] const GameEntryHandoffProvider&
    game_entry_handoff_provider() const noexcept;
    [[nodiscard]] const GameProjectFunctionBoundary*
    function_containing(std::uint32_t address) const noexcept;
    [[nodiscard]] const GameProjectJumpTable*
    jump_table(std::uint32_t dispatch_address) const noexcept;
    [[nodiscard]] const GameProjectCallbackTable*
    callback_table(std::uint32_t table_address) const noexcept;
    [[nodiscard]] const GameProjectFunctionOverride*
    function_override(std::uint32_t function_address) const noexcept;
    [[nodiscard]] const GameProjectMidFunctionHook*
    mid_function_hook(std::uint32_t instruction_address) const noexcept;
    [[nodiscard]] const GameProjectSymbol*
    symbol(std::uint32_t address) const noexcept;
    [[nodiscard]] const GameProjectCodeIdentity*
    code_identity_containing(std::uint32_t address,
                             std::size_t size = 1u) const noexcept;

    [[nodiscard]] GameProjectHookDispatchResult
    invoke_function_override(std::uint32_t function_address,
                             CpuState& cpu,
                             PlatformServices* services) const noexcept;
    [[nodiscard]] GameProjectHookDispatchResult
    invoke_mid_function_hook(std::uint32_t instruction_address,
                             CpuState& cpu,
                             PlatformServices* services) const noexcept;

  private:
    GameProjectDefinition definition_;
    GameProjectRuntimeProviders runtime_providers_;
};

// A game-specific project normally owns one static registration in its own
// binary. KatanaRuntime keeps only an atomic non-owning pointer, so no map,
// allocation or title lookup enters the product hot path. Registration is
// exclusive and its definition/storage must outlive all generated execution.
class GameProjectRegistration final {
  public:
    explicit GameProjectRegistration(
        GameProjectDefinition definition,
        GameProjectRuntimeProviders runtime_providers = {});
    ~GameProjectRegistration();

    GameProjectRegistration(const GameProjectRegistration&) = delete;
    GameProjectRegistration& operator=(const GameProjectRegistration&) = delete;
    GameProjectRegistration(GameProjectRegistration&&) = delete;
    GameProjectRegistration& operator=(GameProjectRegistration&&) = delete;

    [[nodiscard]] const GameProjectBindings& bindings() const noexcept;

  private:
    GameProjectBindings bindings_;
};

[[nodiscard]] const GameProjectBindings*
active_game_project_bindings() noexcept;
[[nodiscard]] const GameProjectDefinition*
active_game_project_definition() noexcept;

} // namespace katana::runtime
