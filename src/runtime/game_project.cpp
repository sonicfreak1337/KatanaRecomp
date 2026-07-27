#include "katana/runtime/game_project.hpp"
#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::uint64_t guest_address_space_size =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;

std::atomic<const GameProjectBindings*> active_bindings = nullptr;

bool valid_code_address(const std::uint32_t address) noexcept {
    return address != 0u && (address & 1u) == 0u;
}

bool valid_range(const std::uint32_t start,
                 const std::uint64_t size,
                 const std::uint32_t alignment) noexcept {
    return size != 0u && alignment != 0u &&
           (start & (alignment - 1u)) == 0u &&
           size <= guest_address_space_size - start;
}

std::uint32_t table_entry_width(const GameProjectTableEncoding encoding) {
    switch (encoding) {
    case GameProjectTableEncoding::Absolute32:
    case GameProjectTableEncoding::SignedRelative32:
        return 4u;
    case GameProjectTableEncoding::SignedRelative16:
        return 2u;
    }
    throw std::invalid_argument("game-project-jump-table-encoding-invalid");
}

void validate_control_transfer_kind(
    const GameProjectControlTransferKind transfer) {
    switch (transfer) {
    case GameProjectControlTransferKind::Jump:
    case GameProjectControlTransferKind::Call:
        return;
    }
    throw std::invalid_argument("game-project-control-transfer-kind-invalid");
}

template <typename Element, typename Projection>
void require_strictly_sorted(const std::span<const Element> elements,
                             Projection projection,
                             const char* error) {
    for (std::size_t index = 1u; index < elements.size(); ++index) {
        if (projection(elements[index - 1u]) >= projection(elements[index]))
            throw std::invalid_argument(error);
    }
}

const GameProjectFunctionBoundary*
find_function(const std::span<const GameProjectFunctionBoundary> functions,
              const std::uint32_t address) noexcept {
    const auto after = std::upper_bound(
        functions.begin(),
        functions.end(),
        address,
        [](const auto value, const auto& function) {
            return value < function.start;
        });
    if (after == functions.begin()) return nullptr;
    const auto& candidate = *std::prev(after);
    const auto offset = static_cast<std::uint64_t>(address) - candidate.start;
    return offset < candidate.size ? &candidate : nullptr;
}

template <typename Element, typename Projection>
const Element* find_exact(const std::span<const Element> elements,
                          const std::uint32_t address,
                          Projection projection) noexcept {
    const auto found = std::lower_bound(
        elements.begin(),
        elements.end(),
        address,
        [&](const auto& element, const auto value) {
            return projection(element) < value;
        });
    return found != elements.end() && projection(*found) == address
               ? &*found
               : nullptr;
}

void validate_identity_binding(const GameProjectIdentityBinding& identity) {
    if (identity.content_identity.empty())
        throw std::invalid_argument("game-project-content-identity-empty");
    if (identity.boot_file_name.empty())
        throw std::invalid_argument("game-project-boot-file-name-empty");
    if (identity.boot_byte_identity.empty())
        throw std::invalid_argument("game-project-boot-byte-identity-empty");
    if (!valid_game_project_sha256_identity(identity.boot_byte_identity))
        throw std::invalid_argument("game-project-boot-byte-identity-invalid");
}

template <typename Element, typename StartProjection, typename SizeProjection>
const Element* find_containing(const std::span<const Element> elements,
                               const std::uint32_t address,
                               const std::size_t size,
                               StartProjection start_projection,
                               SizeProjection size_projection) noexcept {
    if (size == 0u ||
        size > guest_address_space_size - static_cast<std::uint64_t>(address))
        return nullptr;
    const auto after = std::upper_bound(
        elements.begin(),
        elements.end(),
        address,
        [&](const auto value, const auto& element) {
            return value < start_projection(element);
        });
    if (after == elements.begin()) return nullptr;
    const auto& candidate = *std::prev(after);
    const auto candidate_start =
        static_cast<std::uint64_t>(start_projection(candidate));
    const auto candidate_end =
        candidate_start + static_cast<std::uint64_t>(size_projection(candidate));
    const auto requested_end =
        static_cast<std::uint64_t>(address) + size;
    return address >= candidate_start && requested_end <= candidate_end
               ? &candidate
               : nullptr;
}

GameProjectHookDispatchResult invoke_hook(
    CpuState& cpu,
    PlatformServices* const services,
    const GameProjectNativeHook callback,
    const GameProjectHookCondition condition,
    void* const user_context) noexcept {
    if (callback == nullptr)
        return {GameProjectHookDispatchStatus::Disabled,
                GameProjectHookApplication::Continue,
                0u};
    if (condition != nullptr && !condition(cpu, services, user_context))
        return {GameProjectHookDispatchStatus::Disabled,
                GameProjectHookApplication::Continue,
                0u};
    const auto pc_before = cpu.pc;
    const auto result = callback(cpu, services, user_context);
    const auto application = apply_game_project_hook_result(cpu, result);
    if (application == GameProjectHookApplication::Invalid)
        return {GameProjectHookDispatchStatus::Invalid, application, 0u};
    if (application == GameProjectHookApplication::Continue &&
        cpu.pc != pc_before)
        return {GameProjectHookDispatchStatus::Invalid, application, 0u};
    return {GameProjectHookDispatchStatus::Applied,
            application,
            result.error_code};
}

} // namespace

GameProjectHookContractError::GameProjectHookContractError(
    const GameProjectHookContractFailure failure)
    : std::runtime_error([failure] {
          switch (failure) {
          case GameProjectHookContractFailure::InvalidResult:
              return "game-project-hook-invalid-result";
          case GameProjectHookContractFailure::
              ContinueChangedProgramCounter:
              return "game-project-hook-continue-changed-pc";
          case GameProjectHookContractFailure::StaleBlockExecution:
              return "game-project-hook-stale-block-execution";
          }
          return "game-project-hook-contract-invalid";
      }()),
      failure_(failure) {}

GameProjectHookContractFailure
GameProjectHookContractError::failure() const noexcept {
    return failure_;
}

bool valid_game_project_hook_result(
    const GameProjectHookResult& result) noexcept {
    switch (result.action) {
    case GameProjectHookAction::Continue:
        return result.target == 0u && result.error_code == 0u;
    case GameProjectHookAction::Jump:
        return valid_code_address(result.target) && result.error_code == 0u;
    case GameProjectHookAction::Return:
        return result.target == 0u && result.error_code == 0u;
    case GameProjectHookAction::Abort:
        return result.target == 0u && result.error_code != 0u;
    }
    return false;
}

bool valid_game_project_sha256_identity(
    const std::string_view identity) noexcept {
    constexpr std::string_view prefix = "sha256:";
    if (identity.size() != prefix.size() + 64u ||
        !identity.starts_with(prefix))
        return false;
    return std::all_of(
        identity.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
        identity.end(),
        [](const char value) {
            return std::isxdigit(static_cast<unsigned char>(value)) != 0;
        });
}

bool game_project_code_identity_matches(
    const GameProjectCodeIdentity& identity,
    const std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != identity.size ||
        !valid_game_project_sha256_identity(identity.byte_identity))
        return false;
    try {
        const auto digest = katana::io::sha256_bytes(std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        return identity.byte_identity ==
               std::string("sha256:") + digest;
    } catch (...) {
        return false;
    }
}

bool game_project_function_override_enabled(
    const GameProjectFunctionOverride& function_override,
    const CpuState& cpu,
    const PlatformServices* services) noexcept {
    return function_override.callback != nullptr &&
           (function_override.condition == nullptr ||
            function_override.condition(
                cpu, services, function_override.user_context));
}

bool game_project_mid_function_hook_enabled(
    const GameProjectMidFunctionHook& hook,
    const CpuState& cpu,
    const PlatformServices* services) noexcept {
    return hook.callback != nullptr &&
           (hook.condition == nullptr ||
            hook.condition(cpu, services, hook.user_context));
}

GameProjectHookApplication
apply_game_project_hook_result(CpuState& cpu,
                               const GameProjectHookResult& result) noexcept {
    if (!valid_game_project_hook_result(result))
        return GameProjectHookApplication::Invalid;
    switch (result.action) {
    case GameProjectHookAction::Continue:
        return GameProjectHookApplication::Continue;
    case GameProjectHookAction::Jump:
        cpu.pc = result.target;
        return GameProjectHookApplication::ControlTransfer;
    case GameProjectHookAction::Return:
        if (!valid_code_address(cpu.pr))
            return GameProjectHookApplication::Invalid;
        cpu.pc = cpu.pr;
        return GameProjectHookApplication::ControlTransfer;
    case GameProjectHookAction::Abort:
        return GameProjectHookApplication::Abort;
    }
    return GameProjectHookApplication::Invalid;
}

void validate_game_project_definition(
    const GameProjectDefinition& definition) {
    if (definition.contract_version != game_project_contract_version)
        throw std::invalid_argument("game-project-contract-unsupported");
    if (definition.project_id.empty())
        throw std::invalid_argument("game-project-id-empty");
    if (definition.project_version.empty())
        throw std::invalid_argument("game-project-version-empty");
    validate_identity_binding(definition.identity);

    require_strictly_sorted(
        definition.function_boundaries,
        [](const auto& value) { return value.start; },
        "game-project-functions-not-strictly-sorted");
    std::uint64_t previous_function_end = 0u;
    for (const auto& function : definition.function_boundaries) {
        if (!valid_code_address(function.start) ||
            !valid_range(function.start, function.size, 2u) ||
            (function.size & 1u) != 0u)
            throw std::invalid_argument("game-project-function-range-invalid");
        if (function.start < previous_function_end)
            throw std::invalid_argument("game-project-functions-overlap");
        previous_function_end =
            static_cast<std::uint64_t>(function.start) + function.size;
    }

    require_strictly_sorted(
        definition.jump_tables,
        [](const auto& value) { return value.dispatch_address; },
        "game-project-jump-tables-not-strictly-sorted");
    for (const auto& table : definition.jump_tables) {
        validate_control_transfer_kind(table.transfer);
        const auto width = table_entry_width(table.encoding);
        if (!valid_code_address(table.dispatch_address) ||
            table.entry_count == 0u || table.entry_stride < width ||
            (table.entry_stride % width) != 0u ||
            (table.table_address & (width - 1u)) != 0u)
            throw std::invalid_argument("game-project-jump-table-invalid");
        const auto extent =
            static_cast<std::uint64_t>(table.entry_count - 1u) *
                table.entry_stride +
            width;
        if (!valid_range(table.table_address, extent, width))
            throw std::invalid_argument("game-project-jump-table-range-invalid");
        if (table.encoding == GameProjectTableEncoding::Absolute32) {
            if (table.relative_base != 0u)
                throw std::invalid_argument(
                    "game-project-absolute-jump-table-has-relative-base");
        } else if (!valid_code_address(table.relative_base)) {
            throw std::invalid_argument(
                "game-project-relative-jump-table-base-invalid");
        }
    }

    require_strictly_sorted(
        definition.callback_tables,
        [](const auto& value) { return value.table_address; },
        "game-project-callback-tables-not-strictly-sorted");
    for (const auto& table : definition.callback_tables) {
        validate_control_transfer_kind(table.transfer);
        if (table.entry_count == 0u ||
            table.entry_stride < sizeof(std::uint32_t) ||
            table.pointer_offset > table.entry_stride - sizeof(std::uint32_t) ||
            (table.table_address & 3u) != 0u ||
            (table.entry_stride & 3u) != 0u ||
            (table.pointer_offset & 3u) != 0u)
            throw std::invalid_argument("game-project-callback-table-invalid");
        const auto extent =
            static_cast<std::uint64_t>(table.entry_count - 1u) *
                table.entry_stride +
            table.pointer_offset + sizeof(std::uint32_t);
        if (!valid_range(table.table_address, extent, 4u))
            throw std::invalid_argument("game-project-callback-table-range-invalid");
    }

    for (const auto& definition_template : definition.runtime_code_templates) {
        switch (definition_template.destination) {
        case NativeAotTemplateDestination::VbrRelative:
        case NativeAotTemplateDestination::LoadedModule:
            break;
        default:
            throw std::invalid_argument(
                "game-project-runtime-template-destination-invalid");
        }
        if (definition_template.source_module_id.empty() ||
            definition_template.expected_source_identity.empty() ||
            definition_template.extent == 0u ||
            (definition_template.source_start & 3u) != 0u ||
            (definition_template.extent & 3u) != 0u ||
            !valid_range(
                definition_template.source_start,
                definition_template.extent,
                4u))
            throw std::invalid_argument(
                "game-project-runtime-template-invalid");
        if (definition_template.destination ==
                NativeAotTemplateDestination::LoadedModule &&
            (definition_template.expected_runtime_content_identity.empty() ||
             definition_template.expected_runtime_byte_identity.empty()))
            throw std::invalid_argument(
                "game-project-runtime-template-identity-empty");
        if (!valid_game_project_sha256_identity(
                definition_template.expected_source_identity) ||
            (definition_template.destination ==
                 NativeAotTemplateDestination::LoadedModule &&
             !valid_game_project_sha256_identity(
                 definition_template.expected_runtime_byte_identity)))
            throw std::invalid_argument(
                "game-project-runtime-template-byte-identity-invalid");

        std::uint64_t previous_patch_end = 0u;
        for (const auto& patch : definition_template.patches) {
            const auto patch_end =
                static_cast<std::uint64_t>(patch.source_offset) +
                sizeof(std::uint32_t);
            if ((patch.source_offset & 3u) != 0u ||
                patch.source_offset < previous_patch_end ||
                patch_end > definition_template.extent ||
                patch.allowed_targets.empty())
                throw std::invalid_argument(
                    "game-project-runtime-template-patch-invalid");
            previous_patch_end = patch_end;
            for (std::size_t index = 0u;
                 index < patch.allowed_targets.size();
                 ++index) {
                const auto& target = patch.allowed_targets[index];
                if (!valid_code_address(target.live_value) ||
                    !valid_code_address(target.block_address) ||
                    (index != 0u &&
                     patch.allowed_targets[index - 1u].live_value >=
                         target.live_value))
                    throw std::invalid_argument(
                        "game-project-runtime-template-patch-target-invalid");
            }
        }
        std::uint64_t previous_mutable_end = 0u;
        for (const auto& range : definition_template.mutable_ranges) {
            const auto range_end =
                static_cast<std::uint64_t>(range.offset) + range.size;
            if (range.size == 0u || (range.offset & 3u) != 0u ||
                (range.size & 3u) != 0u ||
                range.offset < previous_mutable_end ||
                range_end > definition_template.extent)
                throw std::invalid_argument(
                    "game-project-runtime-template-mutable-range-invalid");
            previous_mutable_end = range_end;
            if (std::any_of(
                    definition_template.patches.begin(),
                    definition_template.patches.end(),
                    [&](const auto& patch) {
                        const auto patch_end =
                            static_cast<std::uint64_t>(patch.source_offset) +
                            sizeof(std::uint32_t);
                        return range.offset < patch_end &&
                               patch.source_offset < range_end;
                    }))
                throw std::invalid_argument(
                    "game-project-runtime-template-mutable-patch-overlap");
        }
    }

    require_strictly_sorted(
        definition.function_overrides,
        [](const auto& value) { return value.function_address; },
        "game-project-function-overrides-not-strictly-sorted");
    for (const auto& function_override : definition.function_overrides) {
        if (!valid_code_address(function_override.function_address) ||
            find_exact(
                definition.function_boundaries,
                function_override.function_address,
                [](const auto& value) { return value.start; }) == nullptr)
            throw std::invalid_argument(
                "game-project-function-override-boundary-missing");
        switch (function_override.strength) {
        case GameProjectFunctionOverrideStrength::Weak:
            if (function_override.callback == nullptr &&
                (function_override.condition != nullptr ||
                 function_override.user_context != nullptr))
                throw std::invalid_argument(
                    "game-project-empty-weak-function-override-has-state");
            break;
        case GameProjectFunctionOverrideStrength::Required:
            if (function_override.callback != nullptr) break;
            throw std::invalid_argument(
                "game-project-required-function-override-empty");
        default:
            throw std::invalid_argument(
                "game-project-function-override-strength-invalid");
        }
    }

    require_strictly_sorted(
        definition.mid_function_hooks,
        [](const auto& value) { return value.instruction_address; },
        "game-project-mid-hooks-not-strictly-sorted");
    for (const auto& hook : definition.mid_function_hooks) {
        if (!valid_code_address(hook.instruction_address) ||
            find_function(
                definition.function_boundaries,
                hook.instruction_address) == nullptr)
            throw std::invalid_argument("game-project-mid-hook-invalid");
        switch (hook.strength) {
        case GameProjectHookStrength::Weak:
            if (hook.callback == nullptr &&
                (hook.condition != nullptr || hook.user_context != nullptr))
                throw std::invalid_argument(
                    "game-project-empty-weak-mid-hook-has-state");
            break;
        case GameProjectHookStrength::Required:
            if (hook.callback != nullptr) break;
            throw std::invalid_argument("game-project-required-mid-hook-empty");
        default:
            throw std::invalid_argument("game-project-mid-hook-strength-invalid");
        }
    }

    require_strictly_sorted(
        definition.symbols,
        [](const auto& value) { return value.address; },
        "game-project-symbols-not-strictly-sorted");
    for (const auto& symbol : definition.symbols) {
        if (symbol.address == 0u || symbol.name.empty() ||
            (symbol.size != 0u &&
             symbol.size >
                 guest_address_space_size -
                     static_cast<std::uint64_t>(symbol.address)))
            throw std::invalid_argument("game-project-symbol-invalid");
        switch (symbol.kind) {
        case GameProjectSymbolKind::Unknown:
        case GameProjectSymbolKind::Function:
        case GameProjectSymbolKind::Object:
            break;
        default:
            throw std::invalid_argument("game-project-symbol-kind-invalid");
        }
    }

    require_strictly_sorted(
        definition.code_identities,
        [](const auto& value) { return value.address; },
        "game-project-code-identities-not-strictly-sorted");
    std::uint64_t previous_identity_end = 0u;
    for (const auto& identity : definition.code_identities) {
        if (!valid_code_address(identity.address) ||
            !valid_range(identity.address, identity.size, 2u) ||
            !valid_game_project_sha256_identity(identity.byte_identity) ||
            identity.address < previous_identity_end)
            throw std::invalid_argument("game-project-code-identity-invalid");
        previous_identity_end =
            static_cast<std::uint64_t>(identity.address) + identity.size;
    }

    if (definition.boot_config.has_value()) {
        const auto& config = *definition.boot_config;
        if (config.boot_path != DreamcastRuntimeBootPath::DirectBootExecutable)
            throw std::invalid_argument(
                "game-project-boot-config-is-not-direct-boot");
        if (config.post_bios_cpu_state.contract_version !=
            dreamcast_post_bios_cpu_contract_version)
            throw std::invalid_argument(
                "game-project-post-bios-contract-unsupported");
        if (config.post_bios_platform_contract_version !=
            dreamcast_post_bios_platform_contract_version)
            throw std::invalid_argument(
                "game-project-post-bios-platform-contract-unsupported");
        const auto& configured_identity = config.executable_identity;
        if ((!configured_identity.content_identity.empty() &&
             configured_identity.content_identity !=
                 definition.identity.content_identity) ||
            (!configured_identity.boot_file_name.empty() &&
             configured_identity.boot_file_name !=
                 definition.identity.boot_file_name) ||
            (!configured_identity.boot_byte_identity.empty() &&
             configured_identity.boot_byte_identity !=
                 definition.identity.boot_byte_identity))
            throw std::invalid_argument(
                "game-project-boot-config-identity-conflict");
    }
}

std::string game_project_definition_identity(
    const GameProjectDefinition& definition) {
    validate_game_project_definition(definition);
    std::ostringstream material;
    material << "game-project-v" << definition.contract_version << ':'
             << definition.project_id << ':' << definition.project_version
             << ':' << definition.identity.content_identity << ':'
             << definition.identity.boot_file_name << ':'
             << definition.identity.boot_byte_identity << ';';
    for (const auto& function : definition.function_boundaries)
        material << "function:" << function.start << ':' << function.size << ':'
                 << function.symbol << ';';
    for (const auto& table : definition.jump_tables)
        material << "jump-table:" << table.dispatch_address << ':'
                 << table.table_address << ':' << table.entry_count << ':'
                 << table.entry_stride << ':' << table.relative_base << ':'
                 << static_cast<unsigned>(table.encoding) << ':'
                 << static_cast<unsigned>(table.transfer) << ';';
    for (const auto& table : definition.callback_tables)
        material << "callback-table:" << table.table_address << ':'
                 << table.entry_count << ':' << table.entry_stride << ':'
                 << table.pointer_offset << ':'
                 << static_cast<unsigned>(table.transfer) << ';';
    for (const auto& native_template : definition.runtime_code_templates) {
        material << "runtime-template:" << native_template.source_module_id
                 << ':' << native_template.expected_source_identity << ':'
                 << native_template.source_start << ':' << native_template.extent
                 << ':' << native_template.destination_vbr_delta << ':'
                 << static_cast<unsigned>(native_template.destination) << ':'
                 << native_template.expected_runtime_content_identity << ':'
                 << native_template.expected_runtime_byte_identity << ';';
        for (const auto& patch : native_template.patches) {
            material << "patch:" << patch.source_offset << ':';
            for (const auto& target : patch.allowed_targets)
                material << target.live_value << '>' << target.block_address
                         << ',';
            material << ';';
        }
        for (const auto& range : native_template.mutable_ranges)
            material << "mutable:" << range.offset << ':' << range.size << ';';
    }
    for (const auto& function : definition.function_overrides)
        material << "override:" << function.function_address << ':'
                 << static_cast<unsigned>(function.strength) << ':'
                 << (function.callback != nullptr) << ':'
                 << (function.condition != nullptr) << ';';
    for (const auto& hook : definition.mid_function_hooks)
        material << "mid-hook:" << hook.instruction_address << ':'
                 << static_cast<unsigned>(hook.strength) << ':'
                 << (hook.callback != nullptr) << ':'
                 << (hook.condition != nullptr) << ';';
    for (const auto& symbol : definition.symbols)
        material << "symbol:" << symbol.address << ':' << symbol.size << ':'
                 << symbol.name << ':' << static_cast<unsigned>(symbol.kind)
                 << ';';
    for (const auto& identity : definition.code_identities)
        material << "code-identity:" << identity.address << ':'
                 << identity.size << ':' << identity.byte_identity << ';';
    if (definition.boot_config.has_value()) {
        const auto& config = *definition.boot_config;
        material << "direct-boot:" << static_cast<unsigned>(config.firmware_mode)
                 << ':' << static_cast<unsigned>(config.boot_path) << ':'
                 << config.post_bios_platform_contract_version << ':'
                 << config.post_bios_cpu_state.contract_version << ':'
                 << config.post_bios_cpu_state.entry_point << ':'
                 << config.post_bios_cpu_state.stack_pointer << ':'
                 << config.post_bios_cpu_state.vector_base << ':'
                 << config.post_bios_cpu_state.status << ':'
                 << config.post_bios_cpu_state.fpscr << ':'
                 << config.post_bios_cpu_state.gbr << ':'
                 << config.post_bios_cpu_state.ssr << ':'
                 << config.post_bios_cpu_state.spc << ':'
                 << config.post_bios_cpu_state.sgr << ':'
                 << config.post_bios_cpu_state.dbr << ':'
                 << config.post_bios_cpu_state.pr << ';';
    }
    return "sha256:" + katana::io::sha256_bytes(material.str());
}

void validate_game_project_boot_identity(
    const GameProjectDefinition& definition,
    const DreamcastRuntimeBootImage& boot) {
    validate_game_project_definition(definition);
    if (definition.identity.content_identity != boot.content_identity)
        throw std::invalid_argument("game-project-content-identity-mismatch");
    if (definition.identity.boot_file_name != boot.boot_file_name)
        throw std::invalid_argument("game-project-boot-file-name-mismatch");
    if (definition.identity.boot_byte_identity !=
        dreamcast_boot_executable_byte_identity(boot))
        throw std::invalid_argument("game-project-boot-byte-identity-mismatch");
}

DreamcastRuntimeBootConfig bind_game_project_boot_config(
    const GameProjectDefinition& definition,
    const DreamcastRuntimeBootImage& boot) {
    validate_game_project_boot_identity(definition, boot);
    if (!definition.boot_config.has_value())
        throw std::invalid_argument("game-project-direct-boot-not-configured");
    auto config = *definition.boot_config;
    config.executable_identity = {
        std::string(definition.identity.content_identity),
        std::string(definition.identity.boot_file_name),
        std::string(definition.identity.boot_byte_identity)};
    validate_dreamcast_runtime_boot_config(boot, config);
    return config;
}

GameProjectBindings::GameProjectBindings(GameProjectDefinition definition)
    : definition_(std::move(definition)) {
    validate_game_project_definition(definition_);
}

const GameProjectDefinition&
GameProjectBindings::definition() const noexcept {
    return definition_;
}

const GameProjectFunctionBoundary*
GameProjectBindings::function_containing(
    const std::uint32_t address) const noexcept {
    return find_function(definition_.function_boundaries, address);
}

const GameProjectJumpTable*
GameProjectBindings::jump_table(
    const std::uint32_t dispatch_address) const noexcept {
    return find_exact(
        definition_.jump_tables,
        dispatch_address,
        [](const auto& value) { return value.dispatch_address; });
}

const GameProjectCallbackTable*
GameProjectBindings::callback_table(
    const std::uint32_t table_address) const noexcept {
    return find_exact(
        definition_.callback_tables,
        table_address,
        [](const auto& value) { return value.table_address; });
}

const GameProjectFunctionOverride*
GameProjectBindings::function_override(
    const std::uint32_t function_address) const noexcept {
    return find_exact(
        definition_.function_overrides,
        function_address,
        [](const auto& value) { return value.function_address; });
}

const GameProjectMidFunctionHook*
GameProjectBindings::mid_function_hook(
    const std::uint32_t instruction_address) const noexcept {
    return find_exact(
        definition_.mid_function_hooks,
        instruction_address,
        [](const auto& value) { return value.instruction_address; });
}

const GameProjectSymbol*
GameProjectBindings::symbol(const std::uint32_t address) const noexcept {
    return find_exact(
        definition_.symbols,
        address,
        [](const auto& value) { return value.address; });
}

const GameProjectCodeIdentity*
GameProjectBindings::code_identity_containing(
    const std::uint32_t address,
    const std::size_t size) const noexcept {
    return find_containing(
        definition_.code_identities,
        address,
        size,
        [](const auto& value) { return value.address; },
        [](const auto& value) { return value.size; });
}

GameProjectHookDispatchResult
GameProjectBindings::invoke_function_override(
    const std::uint32_t function_address,
    CpuState& cpu,
    PlatformServices* const services) const noexcept {
    const auto* function = function_override(function_address);
    if (function == nullptr)
        return {GameProjectHookDispatchStatus::NotFound,
                GameProjectHookApplication::Continue,
                0u};
    return invoke_hook(cpu,
                       services,
                       function->callback,
                       function->condition,
                       function->user_context);
}

GameProjectHookDispatchResult
GameProjectBindings::invoke_mid_function_hook(
    const std::uint32_t instruction_address,
    CpuState& cpu,
    PlatformServices* const services) const noexcept {
    const auto* hook = mid_function_hook(instruction_address);
    if (hook == nullptr)
        return {GameProjectHookDispatchStatus::NotFound,
                GameProjectHookApplication::Continue,
                0u};
    return invoke_hook(
        cpu, services, hook->callback, hook->condition, hook->user_context);
}

GameProjectRegistration::GameProjectRegistration(
    GameProjectDefinition definition)
    : bindings_(std::move(definition)) {
    const GameProjectBindings* expected = nullptr;
    if (!active_bindings.compare_exchange_strong(
            expected,
            &bindings_,
            std::memory_order_release,
            std::memory_order_relaxed))
        throw std::logic_error("game-project-registration-already-active");
}

GameProjectRegistration::~GameProjectRegistration() {
    const GameProjectBindings* expected = &bindings_;
    static_cast<void>(active_bindings.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_release,
        std::memory_order_relaxed));
}

const GameProjectBindings&
GameProjectRegistration::bindings() const noexcept {
    return bindings_;
}

const GameProjectBindings* active_game_project_bindings() noexcept {
    return active_bindings.load(std::memory_order_acquire);
}

const GameProjectDefinition* active_game_project_definition() noexcept {
    const auto* bindings = active_game_project_bindings();
    return bindings != nullptr ? &bindings->definition() : nullptr;
}

} // namespace katana::runtime
