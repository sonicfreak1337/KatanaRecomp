#include "katana/runtime/native_port.hpp"
#include "katana/runtime/native_port_content.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <tuple>

namespace katana::runtime {
namespace {

[[nodiscard]] bool valid_identifier_component(
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

[[nodiscard]] bool valid_content_relative_path(
    const std::string_view value) noexcept {
    if (value.empty() || value.size() > 4096u || value.front() == '/' ||
        value.front() == '\\' || value.back() == '/' ||
        value.back() == '\\' || value.find(':') != std::string_view::npos)
        return false;
    std::size_t segment_begin = 0u;
    for (std::size_t index = 0u; index <= value.size(); ++index) {
        if (index != value.size() && value[index] != '/' &&
            value[index] != '\\') {
            const auto byte = static_cast<unsigned char>(value[index]);
            if (byte < 0x20u || byte == 0x7fu) return false;
            continue;
        }
        const auto segment = value.substr(segment_begin, index - segment_begin);
        if (segment.empty() || segment == "." || segment == "..") return false;
        segment_begin = index + 1u;
    }
    return true;
}

[[noreturn]] void invalid_definition(const std::string_view detail) {
    throw NativePortContractError(
        NativePortContractFailure::InvalidDefinition, detail);
}

} // namespace

NativePortContractError::NativePortContractError(
    const NativePortContractFailure failure,
    const std::string_view detail)
    : std::runtime_error(
          "native-port-contract: " + std::string(detail)),
      failure_(failure) {}

NativePortContractFailure NativePortContractError::failure() const noexcept {
    return failure_;
}

bool valid_native_port_sha256_identity(
    const std::string_view identity) noexcept {
    constexpr std::string_view prefix{"sha256:"};
    return identity.size() == prefix.size() + 64u &&
           identity.starts_with(prefix) &&
           std::all_of(
               identity.begin() +
                   static_cast<std::ptrdiff_t>(prefix.size()),
               identity.end(),
               [](const char character) {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               });
}

bool valid_native_port_link_symbol(const std::string_view symbol) noexcept {
    if (symbol.empty() || symbol.size() > 240u) return false;
    const auto identifier_start = [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               character == '_';
    };
    const auto identifier_continue = [&](const char character) {
        return identifier_start(character) ||
               (character >= '0' && character <= '9');
    };
    return identifier_start(symbol.front()) &&
           std::all_of(symbol.begin() + 1, symbol.end(), identifier_continue);
}

bool valid_native_port_hook_result(
    const NativePortHookResult& result) noexcept {
    switch (result.action) {
    case NativePortHookAction::ContinueOriginal:
    case NativePortHookAction::Return:
        return result.target == 0u && result.error_code == 0u;
    case NativePortHookAction::Jump:
        return result.target != 0u && result.error_code == 0u;
    case NativePortHookAction::Abort:
        return result.target == 0u && result.error_code != 0u;
    }
    return false;
}

bool valid_native_port_hook_result(
    const NativePortHookBinding& binding,
    const NativePortHookResult& result) noexcept {
    return valid_native_port_hook_result(result) &&
           (binding.original_policy ==
                    NativePortHookOriginalPolicy::MayContinueOriginal ||
            result.action != NativePortHookAction::ContinueOriginal);
}

void validate_native_port_definition(
    const NativePortDefinition& definition) {
    if (definition.contract_version !=
        native_port_definition_contract_version)
        invalid_definition("definition-version");
    if (!valid_identifier_component(definition.project_id) ||
        !valid_identifier_component(definition.project_version))
        invalid_definition("project-identity");
    if (definition.executable.content_identity.empty() ||
        definition.executable.executable_name.empty() ||
        !valid_native_port_sha256_identity(
            definition.executable.executable_byte_identity))
        invalid_definition("executable-identity");
    if ((definition.bootstrap.entry_point & 1u) != 0u ||
        definition.bootstrap.entry_point == 0u ||
        !valid_native_port_link_symbol(definition.bootstrap.symbol))
        invalid_definition("bootstrap");

    std::set<std::string_view> image_ids;
    std::set<std::tuple<std::uint64_t, std::uint64_t>> image_ranges;
    std::set<std::tuple<std::uint64_t, std::uint64_t>> physical_image_ranges;
    for (const auto& image : definition.images) {
        const auto end =
            static_cast<std::uint64_t>(image.guest_address) +
            image.byte_size;
        const auto physical = canonical_physical_address(image.guest_address);
        const auto physical_end =
            static_cast<std::uint64_t>(physical) + image.byte_size;
        const auto backing_offset =
            physical >= native_port_main_memory_physical_base
                ? (physical - native_port_main_memory_physical_base) &
                      (native_port_main_memory_backing_size - 1u)
                : native_port_main_memory_backing_size;
        if (!valid_identifier_component(image.image_id) ||
            !valid_content_relative_path(image.content_relative_path) ||
            !valid_native_port_sha256_identity(image.byte_identity) ||
            image.byte_size == 0u || end > 0x1'0000'0000ull ||
            physical < native_port_main_memory_physical_base ||
            physical_end >
                static_cast<std::uint64_t>(
                    native_port_main_memory_physical_base) +
                    native_port_main_memory_physical_span ||
            backing_offset >= native_port_main_memory_backing_size ||
            image.byte_size >
                native_port_main_memory_backing_size - backing_offset ||
            image.file_offset >
                std::numeric_limits<std::uint64_t>::max() -
                    image.byte_size ||
            !image_ids.insert(image.image_id).second)
            invalid_definition("image-binding");
        const auto range = std::tuple{
            static_cast<std::uint64_t>(image.guest_address), end};
        for (const auto& existing : image_ranges) {
            if (std::get<0>(range) < std::get<1>(existing) &&
                std::get<0>(existing) < std::get<1>(range))
                invalid_definition("overlapping-image-bindings");
        }
        image_ranges.insert(range);
        const auto physical_range = std::tuple{
            static_cast<std::uint64_t>(backing_offset),
            static_cast<std::uint64_t>(backing_offset) + image.byte_size};
        for (const auto& existing : physical_image_ranges) {
            if (std::get<0>(physical_range) < std::get<1>(existing) &&
                std::get<0>(existing) < std::get<1>(physical_range))
                invalid_definition("aliased-image-bindings");
        }
        physical_image_ranges.insert(physical_range);
    }
    if (definition.images.empty()) invalid_definition("missing-image-binding");
    const auto range_inside_image = [&](const std::uint32_t address,
                                        const std::uint32_t size) {
        const auto begin = static_cast<std::uint64_t>(address);
        const auto end = begin + size;
        return std::any_of(
            definition.images.begin(),
            definition.images.end(),
            [&](const auto& image) {
                const auto image_begin =
                    static_cast<std::uint64_t>(image.guest_address);
                const auto image_end = image_begin + image.byte_size;
                return begin >= image_begin && end <= image_end;
            });
    };
    if (!range_inside_image(definition.bootstrap.entry_point, 2u))
        invalid_definition("bootstrap-outside-images");

    std::set<std::uint32_t> hook_addresses;
    std::set<std::string_view> hook_symbols;
    std::set<std::tuple<std::uint64_t, std::uint64_t>> hook_ranges;
    for (const auto& hook : definition.hooks) {
        const auto end =
            static_cast<std::uint64_t>(hook.guest_address) +
            hook.covered_size;
        const auto valid_kind =
            hook.kind == NativePortHookKind::FunctionEntry ||
            hook.kind == NativePortHookKind::Instruction;
        const auto valid_requirement =
            hook.requirement == NativePortHookRequirement::Required ||
            hook.requirement == NativePortHookRequirement::DiagnosticOnly;
        const auto valid_original_policy =
            hook.original_policy ==
                NativePortHookOriginalPolicy::ReplacesOriginal ||
            hook.original_policy ==
                NativePortHookOriginalPolicy::MayContinueOriginal;
        if ((hook.guest_address & 1u) != 0u ||
            hook.covered_size < 2u ||
            (hook.covered_size & 1u) != 0u ||
            end > 0x1'0000'0000ull ||
            !valid_kind || !valid_requirement ||
            !valid_original_policy ||
            !range_inside_image(hook.guest_address, hook.covered_size) ||
            !valid_native_port_link_symbol(hook.symbol) ||
            !valid_native_port_sha256_identity(hook.code_identity) ||
            !hook_addresses.insert(hook.guest_address).second ||
            hook.symbol == definition.bootstrap.symbol ||
            !hook_symbols.insert(hook.symbol).second)
            invalid_definition("hook-binding");
        if (hook.kind == NativePortHookKind::Instruction &&
            hook.covered_size != 2u)
            invalid_definition("instruction-hook-extent");
        const auto range = std::tuple{
            static_cast<std::uint64_t>(hook.guest_address), end};
        for (const auto& existing : hook_ranges) {
            if (std::get<0>(range) < std::get<1>(existing) &&
                std::get<0>(existing) < std::get<1>(range))
                invalid_definition("overlapping-hook-bindings");
        }
        hook_ranges.insert(range);
    }

    std::set<std::uint32_t> resolved_instructions;
    for (const auto& resolution : definition.hardware_resolutions) {
        const auto valid_kind =
            resolution.kind ==
                NativePortHardwareResolutionKind::NativeMemory ||
            resolution.kind ==
                NativePortHardwareResolutionKind::ReplacedByHook;
        if ((resolution.instruction_address & 1u) != 0u ||
            !valid_kind ||
            !range_inside_image(resolution.instruction_address, 2u) ||
            !resolved_instructions.insert(
                resolution.instruction_address).second)
            invalid_definition("hardware-resolution");
        if (resolution.kind ==
            NativePortHardwareResolutionKind::NativeMemory) {
            constexpr std::uint8_t valid_access_mask =
                native_port_memory_access_mask(NativePortMemoryAccess::Read) |
                native_port_memory_access_mask(NativePortMemoryAccess::Write) |
                native_port_memory_access_mask(
                    NativePortMemoryAccess::Prefetch);
            constexpr std::uint8_t valid_width_mask =
                native_port_memory_width_u8 |
                native_port_memory_width_u16 |
                native_port_memory_width_u32 |
                native_port_memory_width_cache_line_32;
            const auto image = std::find_if(
                definition.images.begin(),
                definition.images.end(),
                [&](const auto& candidate) {
                    return candidate.image_id ==
                           resolution.native_memory_image_id;
                });
            const auto range_end =
                static_cast<std::uint64_t>(
                    resolution.native_memory_guest_address) +
                resolution.native_memory_byte_size;
            const auto image_begin =
                image == definition.images.end()
                    ? 0u
                    : static_cast<std::uint64_t>(image->guest_address);
            const auto image_end =
                image == definition.images.end()
                    ? 0u
                    : image_begin + image->byte_size;
            if (resolution.hook_guest_address != 0u ||
                resolution.native_memory_image_id.empty() ||
                resolution.native_memory_byte_size == 0u ||
                resolution.native_memory_access_mask == 0u ||
                resolution.native_memory_width_mask == 0u ||
                (resolution.native_memory_access_mask &
                 ~valid_access_mask) != 0u ||
                (resolution.native_memory_width_mask &
                 ~valid_width_mask) != 0u ||
                image == definition.images.end() ||
                resolution.native_memory_guest_address < image_begin ||
                range_end > image_end ||
                ((resolution.native_memory_access_mask &
                  native_port_memory_access_mask(
                      NativePortMemoryAccess::Write)) != 0u &&
                 !image->writable))
                invalid_definition("native-memory-resolution-hook");
            continue;
        }
        if (!resolution.native_memory_image_id.empty() ||
            resolution.native_memory_guest_address != 0u ||
            resolution.native_memory_byte_size != 0u ||
            resolution.native_memory_access_mask != 0u ||
            resolution.native_memory_width_mask != 0u)
            invalid_definition("hook-resolution-native-memory-fields");
        const auto hook = std::find_if(
            definition.hooks.begin(),
            definition.hooks.end(),
            [&](const auto& candidate) {
                return candidate.guest_address ==
                       resolution.hook_guest_address;
            });
        if (hook == definition.hooks.end() ||
            hook->requirement != NativePortHookRequirement::Required ||
            hook->original_policy !=
                NativePortHookOriginalPolicy::ReplacesOriginal ||
            // Hooks are dispatched only at their entry address.  A range
            // inclusion would incorrectly certify a direct/mid-block entry
            // to a later instruction as unreachable.
            resolution.instruction_address != hook->guest_address)
            invalid_definition("hook-hardware-resolution");
    }
}

} // namespace katana::runtime
