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
#include <type_traits>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

constexpr std::uint64_t guest_address_space_size =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
constexpr std::size_t maximum_runtime_image_size =
    16u * 1024u * 1024u;
constexpr std::size_t maximum_runtime_image_total_size =
    64u * 1024u * 1024u;
constexpr std::size_t maximum_runtime_image_entries = 65'536u;

std::atomic<const GameProjectBindings*> active_bindings = nullptr;

// Game-project identities are cache, AOT and runtime trust boundaries. Keep
// their preimage binary, typed and length-delimited so arbitrary project,
// symbol and image names cannot alias a different field sequence.
class GameProjectIdentityMaterial final {
  public:
    void u8(const std::uint8_t value) {
        bytes_.push_back(static_cast<char>(value));
    }

    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void i32(const std::int32_t value) {
        u32(static_cast<std::uint32_t>(value));
    }

    void boolean(const bool value) {
        u8(value ? 1u : 0u);
    }

    template <typename Enum>
    void enumeration(const Enum value) {
        static_assert(std::is_enum_v<Enum>);
        u32(static_cast<std::uint32_t>(value));
    }

    void count(const std::size_t value) {
        u64(static_cast<std::uint64_t>(value));
    }

    void text(const std::string_view value) {
        count(value.size());
        bytes_.append(value);
    }

    [[nodiscard]] std::string finish() && {
        return std::move(bytes_);
    }

  private:
    std::string bytes_;
};

bool valid_code_address(const std::uint32_t address) noexcept {
    return address != 0u && (address & 1u) == 0u;
}

bool valid_project_identity_component(
    const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128u &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_' ||
                      character == '.';
           });
}

bool valid_range(const std::uint32_t start,
                 const std::uint64_t size,
                 const std::uint32_t alignment) noexcept {
    return size != 0u && alignment != 0u &&
           (start & (alignment - 1u)) == 0u &&
           size <= guest_address_space_size - start;
}

bool direct_mapped_range(const std::uint32_t start,
                         const std::size_t size) noexcept {
    if (size == 0u ||
        size > guest_address_space_size - static_cast<std::uint64_t>(start))
        return false;
    const auto final =
        start + static_cast<std::uint32_t>(size - 1u);
    const auto segment = start & 0xE0000000u;
    return (segment == 0x80000000u || segment == 0xA0000000u) &&
           (final & 0xE0000000u) == segment;
}

std::uint32_t direct_mapped_physical(
    const std::uint32_t address) noexcept {
    return address & 0x1FFFFFFFu;
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

bool valid_game_entry_boot_file_name(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 255u || value == "." || value == "..")
        return false;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x21u && byte <= 0x7Eu && character != '/' &&
               character != '\\';
    });
}

bool valid_game_entry_console_profile(
    const DreamcastConsoleProfile profile) noexcept {
    switch (profile) {
    case DreamcastConsoleProfile::JapanNtsc:
    case DreamcastConsoleProfile::NorthAmericaNtsc:
    case DreamcastConsoleProfile::EuropePal:
    case DreamcastConsoleProfile::Vga:
        return true;
    }
    return false;
}

void validate_game_entry_binding(
    const GameProjectDefinition& definition,
    const GameEntryHandoffBinding& binding) {
    if (binding.schema_version != game_entry_handoff_schema_version)
        throw std::invalid_argument(
            "game-project-game-entry-schema-unsupported");
    if (binding.required_runtime_abi != abi_version)
        throw std::invalid_argument(
            "game-project-game-entry-runtime-abi-unsupported");
    if (binding.required_platform_state_contract !=
        game_entry_platform_state_contract_version)
        throw std::invalid_argument(
            "game-project-game-entry-platform-contract-unsupported");
    if (!valid_game_entry_content_identity(
            binding.executable.content_identity))
        throw std::invalid_argument(
            "game-project-game-entry-content-identity-invalid");
    if (!valid_game_entry_boot_file_name(
            binding.executable.boot_file_name))
        throw std::invalid_argument(
            "game-project-game-entry-boot-file-name-invalid");
    if (!valid_game_entry_sha256_identity(
            binding.executable.boot_byte_identity))
        throw std::invalid_argument(
            "game-project-game-entry-boot-byte-identity-invalid");
    if (!valid_game_entry_console_profile(binding.console_profile))
        throw std::invalid_argument(
            "game-project-game-entry-console-profile-invalid");
    if (!valid_game_entry_sha256_identity(binding.descriptor_identity))
        throw std::invalid_argument(
            "game-project-game-entry-descriptor-identity-invalid");
    if (std::string_view(binding.executable.content_identity) !=
            definition.identity.content_identity ||
        std::string_view(binding.executable.boot_file_name) !=
            definition.identity.boot_file_name ||
        std::string_view(binding.executable.boot_byte_identity) !=
            definition.identity.boot_byte_identity)
        throw std::invalid_argument(
            "game-project-game-entry-executable-identity-mismatch");
}

void validate_runtime_providers(
    const GameProjectDefinition& definition,
    const GameProjectRuntimeProviders& providers) {
    const auto& game_entry = providers.game_entry_handoff;
    const bool provider_present =
        game_entry.context != nullptr || game_entry.describe != nullptr ||
        game_entry.read_private_slice != nullptr;
    if (!definition.game_entry_handoff.has_value()) {
        if (provider_present)
            throw std::invalid_argument(
                "game-project-game-entry-provider-without-binding");
        return;
    }
    if (game_entry.describe == nullptr)
        throw std::invalid_argument(
            "game-project-game-entry-provider-unavailable");
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

std::string_view required_product_milestone_name(
    const RequiredProductMilestone milestone) noexcept {
    switch (milestone) {
    case RequiredProductMilestone::BootExecutableEntry:
        return "BootExecutableEntry";
    case RequiredProductMilestone::GameCodeProgressed:
        return "GameCodeProgressed";
    case RequiredProductMilestone::FirstGameFramebufferWrite:
        return "FirstGameFramebufferWrite";
    case RequiredProductMilestone::FirstTaFrame:
        return "FirstTaFrame";
    case RequiredProductMilestone::FirstVisibleGameFrame:
        return "FirstVisibleGameFrame";
    case RequiredProductMilestone::MainMenuPresented:
        return "MainMenuPresented";
    }
    return "Invalid";
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
    if (!valid_project_identity_component(definition.project_id))
        throw std::invalid_argument("game-project-id-invalid");
    if (!valid_project_identity_component(definition.project_version))
        throw std::invalid_argument("game-project-version-invalid");
    validate_identity_binding(definition.identity);
    if (required_product_milestone_name(
            definition.required_product_milestone) == "Invalid")
        throw std::invalid_argument(
            "game-project-required-product-milestone-invalid");
    if (definition.game_entry_handoff.has_value())
        validate_game_entry_binding(
            definition, *definition.game_entry_handoff);

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
            break;
        case NativeAotTemplateDestination::LoadedModule:
            // A LoadedModule template is safe only when a proven runtime
            // module carries its exact template ID. The current GameProject
            // contract has no declarative Disc source binding that can
            // publish that ID, so accepting this definition would create a
            // template that can only fail later as MissingAot.
            throw std::invalid_argument(
                "game-project-loaded-module-template-source-binding-unavailable");
        default:
            throw std::invalid_argument(
                "game-project-runtime-template-destination-invalid");
        }
        // FixedAddress templates are derived by the exporter from
        // GameProjectRuntimeImage. Legacy VBR/Loaded artifact records do not
        // serialize these trailing proof fields and must not silently lose
        // caller-provided semantics during a round trip.
        if (definition_template.fixed_runtime_start != 0u ||
            !definition_template.block_identities.empty() ||
            definition_template.validation_mode !=
                NativeAotTemplateValidationMode::SourceModule)
            throw std::invalid_argument(
                "game-project-runtime-template-fixed-fields-invalid");
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
        if (!valid_game_project_sha256_identity(
                definition_template.expected_source_identity))
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

    struct RuntimeImageRange {
        std::uint32_t physical_start = 0u;
        std::uint64_t physical_end = 0u;
    };
    std::size_t runtime_image_total_size = 0u;
    std::vector<std::string_view> runtime_image_ids;
    std::vector<RuntimeImageRange> runtime_image_source_ranges;
    runtime_image_ids.reserve(definition.runtime_images.size());
    runtime_image_source_ranges.reserve(definition.runtime_images.size());
    for (const auto& image : definition.runtime_images) {
        if (image.image_id.empty() ||
            image.image_id.find('=') != std::string_view::npos)
            throw std::invalid_argument(
                "game-project-runtime-image-id-invalid");
        if (!valid_game_project_sha256_identity(image.byte_identity))
            throw std::invalid_argument(
                "game-project-runtime-image-byte-identity-invalid");
        if (image.byte_size == 0u ||
            image.byte_size > maximum_runtime_image_size ||
            (image.byte_size & 3u) != 0u ||
            (image.source_start & 3u) != 0u ||
            (image.runtime_start & 3u) != 0u ||
            !direct_mapped_range(image.source_start, image.byte_size) ||
            !direct_mapped_range(image.runtime_start, image.byte_size))
            throw std::invalid_argument(
                "game-project-runtime-image-range-invalid");
        if (image.byte_size >
            maximum_runtime_image_total_size - runtime_image_total_size)
            throw std::invalid_argument(
                "game-project-runtime-images-total-size-invalid");
        runtime_image_total_size += image.byte_size;

        std::uint32_t previous_entry = 0u;
        bool first_entry = true;
        if (image.entry_offsets.empty() ||
            image.entry_offsets.size() > maximum_runtime_image_entries)
            throw std::invalid_argument(
                "game-project-runtime-image-entry-count-invalid");
        for (const auto entry : image.entry_offsets) {
            if ((entry & 1u) != 0u || entry >= image.byte_size ||
                (!first_entry && entry <= previous_entry))
                throw std::invalid_argument(
                    "game-project-runtime-image-entry-offset-invalid");
            previous_entry = entry;
            first_entry = false;
        }

        const auto source_physical =
            direct_mapped_physical(image.source_start);
        runtime_image_ids.push_back(image.image_id);
        runtime_image_source_ranges.push_back(
            {source_physical, static_cast<std::uint64_t>(source_physical) +
                                  image.byte_size});
    }
    std::sort(runtime_image_ids.begin(), runtime_image_ids.end());
    if (std::adjacent_find(
            runtime_image_ids.begin(), runtime_image_ids.end()) !=
        runtime_image_ids.end())
        throw std::invalid_argument(
            "game-project-runtime-image-id-duplicate");
    const auto require_metadata_scope =
        [&](const std::string_view image_id,
            const std::uint32_t address,
            const std::uint32_t size,
            const char* const error) {
            if (image_id.empty()) return;
            const auto image = std::find_if(
                definition.runtime_images.begin(),
                definition.runtime_images.end(),
                [&](const auto& candidate) {
                    return candidate.image_id == image_id;
                });
            if (image == definition.runtime_images.end() ||
                address < image->source_start ||
                static_cast<std::uint64_t>(address) + size >
                    static_cast<std::uint64_t>(image->source_start) +
                        image->byte_size)
                throw std::invalid_argument(error);
        };
    for (const auto& function : definition.function_boundaries)
        require_metadata_scope(function.image_id,
                               function.start,
                               function.size,
                               "game-project-function-image-scope-invalid");
    for (const auto& table : definition.jump_tables)
        require_metadata_scope(table.image_id,
                               table.dispatch_address,
                               2u,
                               "game-project-jump-table-image-scope-invalid");
    for (const auto& table : definition.callback_tables)
        require_metadata_scope(table.image_id,
                               table.table_address,
                               sizeof(std::uint32_t),
                               "game-project-callback-table-image-scope-invalid");
    for (const auto& identity : definition.code_identities)
        require_metadata_scope(identity.image_id,
                               identity.address,
                               identity.size,
                               "game-project-code-identity-image-scope-invalid");
    const auto require_nonoverlapping_runtime_image_ranges =
        [](auto& ranges, const char* error) {
            std::sort(
                ranges.begin(),
                ranges.end(),
                [](const auto& left, const auto& right) {
                    return left.physical_start < right.physical_start;
                });
            for (std::size_t index = 1u; index < ranges.size(); ++index) {
                if (ranges[index].physical_start <
                    ranges[index - 1u].physical_end)
                    throw std::invalid_argument(error);
            }
        };
    require_nonoverlapping_runtime_image_ranges(
        runtime_image_source_ranges,
        "game-project-runtime-image-source-ranges-overlap");
    // Runtime destinations belong to mutually exclusive image lifetimes.
    // Their overlap is therefore checked for the selected active set by the
    // exporter, not globally across dormant overlays in the project catalog.

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
            identity.address < previous_identity_end) {
            std::ostringstream error;
            error << "game-project-code-identity-invalid:address=0x"
                  << std::hex << identity.address << ":size=0x"
                  << identity.size << ":previous-end=0x"
                  << previous_identity_end;
            throw std::invalid_argument(error.str());
        }
        previous_identity_end =
            static_cast<std::uint64_t>(identity.address) + identity.size;
    }

    require_strictly_sorted(
        definition.static_entries,
        [](const auto value) { return value; },
        "game-project-static-entries-not-strictly-sorted");
    for (const auto entry : definition.static_entries) {
        const auto* const boundary = find_exact(
            definition.function_boundaries,
            entry,
            [](const auto& value) { return value.start; });
        const auto* const identity = find_exact(
            definition.code_identities,
            entry,
            [](const auto& value) { return value.address; });
        if (boundary == nullptr || identity == nullptr ||
            boundary->size != identity->size ||
            boundary->image_id != identity->image_id)
            throw std::invalid_argument(
                "game-project-static-entry-exact-identity-missing");
    }

    if (definition.game_entry_handoff.has_value() &&
        !definition.boot_config.has_value())
        throw std::invalid_argument(
            "game-project-game-entry-requires-direct-boot");

    if (definition.boot_config.has_value()) {
        const auto& config = *definition.boot_config;
        switch (config.firmware_mode) {
        case DreamcastRuntimeFirmwareMode::Direct:
        case DreamcastRuntimeFirmwareMode::HleBiosAbi:
            break;
        default:
            throw std::invalid_argument(
                "game-project-firmware-mode-invalid");
        }
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
    GameProjectIdentityMaterial material;
    material.text("katana.game-project-definition.identity");
    material.u32(3u);
    material.u32(definition.contract_version);
    material.text(definition.project_id);
    material.text(definition.project_version);
    material.text(definition.identity.content_identity);
    material.text(definition.identity.boot_file_name);
    material.text(definition.identity.boot_byte_identity);
    material.enumeration(definition.required_product_milestone);

    material.count(definition.function_boundaries.size());
    for (const auto& function : definition.function_boundaries) {
        material.u32(function.start);
        material.u32(function.size);
        material.text(function.symbol);
        material.text(function.image_id);
    }

    material.count(definition.jump_tables.size());
    for (const auto& table : definition.jump_tables) {
        material.u32(table.dispatch_address);
        material.u32(table.table_address);
        material.u32(table.entry_count);
        material.u32(table.entry_stride);
        material.u32(table.relative_base);
        material.enumeration(table.encoding);
        material.enumeration(table.transfer);
        material.text(table.image_id);
    }

    material.count(definition.callback_tables.size());
    for (const auto& table : definition.callback_tables) {
        material.u32(table.table_address);
        material.u32(table.entry_count);
        material.u32(table.entry_stride);
        material.u32(table.pointer_offset);
        material.enumeration(table.transfer);
        material.text(table.image_id);
    }

    material.count(definition.runtime_code_templates.size());
    for (const auto& native_template : definition.runtime_code_templates) {
        material.text(native_template.source_module_id);
        material.text(native_template.expected_source_identity);
        material.u32(native_template.source_start);
        material.u32(native_template.extent);
        material.i32(native_template.destination_vbr_delta);
        material.enumeration(native_template.destination);
        material.text(native_template.expected_runtime_content_identity);
        material.text(native_template.expected_runtime_byte_identity);
        material.count(native_template.patches.size());
        for (const auto& patch : native_template.patches) {
            material.u32(patch.source_offset);
            material.count(patch.allowed_targets.size());
            for (const auto& target : patch.allowed_targets) {
                material.u32(target.live_value);
                material.u32(target.block_address);
            }
        }
        material.count(native_template.mutable_ranges.size());
        for (const auto& range : native_template.mutable_ranges) {
            material.u32(range.offset);
            material.u32(range.size);
        }
        material.u32(native_template.fixed_runtime_start);
        material.count(native_template.block_identities.size());
        for (const auto& identity : native_template.block_identities) {
            material.u32(identity.source_offset);
            material.u32(identity.size);
            material.text(identity.sha256);
        }
        material.enumeration(native_template.validation_mode);
    }

    material.count(definition.runtime_images.size());
    for (const auto& image : definition.runtime_images) {
        material.text(image.image_id);
        material.text(image.byte_identity);
        material.u32(image.source_start);
        material.u32(image.runtime_start);
        material.u32(image.byte_size);
        material.count(image.entry_offsets.size());
        for (const auto entry : image.entry_offsets)
            material.u32(entry);
    }

    material.count(definition.function_overrides.size());
    for (const auto& function : definition.function_overrides) {
        material.u32(function.function_address);
        material.enumeration(function.strength);
        material.boolean(function.callback != nullptr);
        material.boolean(function.condition != nullptr);
        material.boolean(function.user_context != nullptr);
    }

    material.count(definition.mid_function_hooks.size());
    for (const auto& hook : definition.mid_function_hooks) {
        material.u32(hook.instruction_address);
        material.enumeration(hook.strength);
        material.boolean(hook.callback != nullptr);
        material.boolean(hook.condition != nullptr);
        material.boolean(hook.user_context != nullptr);
    }

    material.count(definition.symbols.size());
    for (const auto& symbol : definition.symbols) {
        material.u32(symbol.address);
        material.u32(symbol.size);
        material.text(symbol.name);
        material.enumeration(symbol.kind);
    }

    material.count(definition.code_identities.size());
    for (const auto& identity : definition.code_identities) {
        material.u32(identity.address);
        material.u32(identity.size);
        material.text(identity.byte_identity);
        material.text(identity.image_id);
    }

    material.count(definition.static_entries.size());
    for (const auto entry : definition.static_entries)
        material.u32(entry);

    material.boolean(definition.boot_config.has_value());
    if (definition.boot_config.has_value()) {
        const auto& config = *definition.boot_config;
        material.enumeration(config.firmware_mode);
        material.enumeration(config.boot_path);
        material.u32(config.post_bios_platform_contract_version);
        material.u32(config.post_bios_cpu_state.contract_version);
        material.u32(config.post_bios_cpu_state.entry_point);
        material.u32(config.post_bios_cpu_state.stack_pointer);
        material.u32(config.post_bios_cpu_state.vector_base);
        material.u32(config.post_bios_cpu_state.status);
        material.u32(config.post_bios_cpu_state.fpscr);
        material.u32(config.post_bios_cpu_state.gbr);
        material.u32(config.post_bios_cpu_state.ssr);
        material.u32(config.post_bios_cpu_state.spc);
        material.u32(config.post_bios_cpu_state.sgr);
        material.u32(config.post_bios_cpu_state.dbr);
        material.u32(config.post_bios_cpu_state.pr);
        material.text(config.executable_identity.content_identity);
        material.text(config.executable_identity.boot_file_name);
        material.text(config.executable_identity.boot_byte_identity);
    }

    material.boolean(definition.game_entry_handoff.has_value());
    if (definition.game_entry_handoff.has_value()) {
        const auto& binding = *definition.game_entry_handoff;
        material.u32(binding.schema_version);
        material.u32(binding.required_runtime_abi);
        material.u32(binding.required_platform_state_contract);
        material.text(binding.executable.content_identity);
        material.text(binding.executable.boot_file_name);
        material.text(binding.executable.boot_byte_identity);
        material.enumeration(binding.console_profile);
        material.text(binding.descriptor_identity);
    }
    return "sha256:" +
           katana::io::sha256_bytes(std::move(material).finish());
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

GameProjectBindings::GameProjectBindings(
    GameProjectDefinition definition,
    GameProjectRuntimeProviders runtime_providers)
    : definition_(std::move(definition)),
      runtime_providers_(std::move(runtime_providers)) {
    validate_game_project_definition(definition_);
    validate_runtime_providers(definition_, runtime_providers_);
}

const GameProjectDefinition&
GameProjectBindings::definition() const noexcept {
    return definition_;
}

const GameProjectRuntimeProviders&
GameProjectBindings::runtime_providers() const noexcept {
    return runtime_providers_;
}

const GameEntryHandoffProvider&
GameProjectBindings::game_entry_handoff_provider() const noexcept {
    return runtime_providers_.game_entry_handoff;
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
    GameProjectDefinition definition,
    GameProjectRuntimeProviders runtime_providers)
    : bindings_(
          std::move(definition), std::move(runtime_providers)) {
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
