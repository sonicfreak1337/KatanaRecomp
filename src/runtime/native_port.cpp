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

[[nodiscard]] bool valid_bootstrap_write_policy(
    const NativePortBootstrapWritePolicy policy) noexcept {
    switch (policy) {
    case NativePortBootstrapWritePolicy::WritableDataOnly:
    case NativePortBootstrapWritePolicy::
        IdentityBoundImmutableMaterialization:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_bootstrap_time_policy(
    const NativePortBootstrapTimePolicy policy) noexcept {
    return policy == NativePortBootstrapTimePolicy::NativeHostEpoch;
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
    if (!valid_native_port_hook_result(result) ||
        (binding.original_policy !=
             NativePortHookOriginalPolicy::MayContinueOriginal &&
         result.action == NativePortHookAction::ContinueOriginal))
        return false;
    // A required whole-function replacement may discharge the original
    // function's transitive hardware closure.  An unconstrained jump would
    // reopen that closure at an undeclared AOT target, so closure-closing
    // replacements are terminal: Return or a typed Abort.  MayContinueOriginal
    // hooks do not discharge closure and retain the generic Jump action.
    if (binding.kind == NativePortHookKind::FunctionEntry &&
        binding.original_policy ==
            NativePortHookOriginalPolicy::ReplacesOriginal &&
        native_port_hook_closes_product_contract(binding.requirement) &&
        result.action == NativePortHookAction::Jump)
        return false;
    if (binding.kind != NativePortHookKind::Instruction)
        return true;
    // An instruction hook proves and displaces exactly one architectural
    // instruction, including its mandatory delay slot when covered_size is
    // four. It may not masquerade as a whole-function replacement by
    // returning to PR or jumping over an unproved body. A successful
    // replacement resumes at the immediate architectural fallthrough; Abort
    // remains a typed bring-up failure.
    if (result.action == NativePortHookAction::Return) return false;
    if (result.action == NativePortHookAction::Jump)
        return static_cast<std::uint64_t>(binding.guest_address) +
                   binding.covered_size <=
                   std::numeric_limits<std::uint32_t>::max() &&
               result.target ==
                   binding.guest_address + binding.covered_size;
    return true;
}

void NativePortContext::bind_acceptance(
    const NativePortAcceptanceBinding& binding) noexcept {
    acceptance_milestone_id_ = binding.milestone_id;
    acceptance_witness_hook_guest_address_ =
        binding.witness_hook_guest_address;
    active_hook_guest_address_ = 0u;
    acceptance_presented_frame_baseline_ =
        host != nullptr ? host->presented_frames() : 0u;
    acceptance_reached_ = false;
}

std::uint32_t NativePortContext::begin_hook_dispatch(
    const std::uint32_t guest_address) noexcept {
    const auto previous = active_hook_guest_address_;
    active_hook_guest_address_ = guest_address;
    return previous;
}

void NativePortContext::end_hook_dispatch(
    const std::uint32_t previous_guest_address) noexcept {
    active_hook_guest_address_ = previous_guest_address;
}

bool NativePortContext::report_acceptance(
    const std::string_view milestone_id) noexcept {
    if (acceptance_reached_ || bootstrap_phase !=
            NativePortBootstrapPhase::Completed || host == nullptr ||
        milestone_id != acceptance_milestone_id_ ||
        active_hook_guest_address_ == 0u ||
        active_hook_guest_address_ !=
            acceptance_witness_hook_guest_address_ ||
        host->presented_frames() <= acceptance_presented_frame_baseline_)
        return false;
    acceptance_reached_ = true;
    return true;
}

bool NativePortContext::acceptance_reached() const noexcept {
    return acceptance_reached_;
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
    constexpr std::uint32_t maximum_native_frame_rate_hz = 1'000u;
    if (definition.frame_timing.simulation_rate_hz == 0u ||
        definition.frame_timing.simulation_rate_hz >
            maximum_native_frame_rate_hz ||
        definition.frame_timing.default_presentation_rate_hz <
            definition.frame_timing.simulation_rate_hz ||
        definition.frame_timing.default_presentation_rate_hz >
            definition.frame_timing.maximum_presentation_rate_hz ||
        definition.frame_timing.maximum_presentation_rate_hz >
            maximum_native_frame_rate_hz)
        invalid_definition("frame-timing");
    if ((definition.bootstrap.entry_point & 1u) != 0u ||
        definition.bootstrap.entry_point == 0u ||
        (definition.bootstrap.post_entry_point & 1u) != 0u ||
        definition.bootstrap.post_entry_point == 0u ||
        definition.bootstrap.post_aot_roots.empty() ||
        !NativePortCpuControl::valid_initial_value(
            definition.bootstrap.cache_control_value) ||
        !valid_bootstrap_time_policy(
            definition.bootstrap.time_policy) ||
        !valid_native_port_link_symbol(definition.bootstrap.symbol) ||
        !valid_native_port_sha256_identity(
            definition.bootstrap.post_cpu_state_identity))
        invalid_definition("bootstrap");
    std::set<std::uint32_t> post_aot_roots;
    for (const auto root : definition.bootstrap.post_aot_roots) {
        if ((root & 1u) != 0u || root == 0u ||
            !post_aot_roots.insert(root).second)
            invalid_definition("post-aot-root");
    }
    std::set<std::uint32_t> continuation_resumes;
    for (const auto& continuation :
         definition.bootstrap.post_aot_continuations) {
        if ((continuation.function_entry & 1u) != 0u ||
            continuation.function_entry == 0u ||
            (continuation.resume_address & 1u) != 0u ||
            continuation.resume_address == 0u ||
            !post_aot_roots.contains(continuation.function_entry) ||
            !continuation_resumes.insert(
                continuation.resume_address).second)
            invalid_definition("post-aot-continuation");
    }
    if (!post_aot_roots.contains(
            definition.bootstrap.post_entry_point) &&
        !continuation_resumes.contains(
            definition.bootstrap.post_entry_point))
        invalid_definition("post-entry-control-flow-root-missing");
    if (!valid_identifier_component(definition.acceptance.milestone_id) ||
        (definition.acceptance.witness_hook_guest_address & 1u) != 0u ||
        definition.acceptance.witness_hook_guest_address == 0u)
        invalid_definition("acceptance-milestone");
    std::set<std::string_view> checkpoint_runtime_image_ids;
    for (const auto image_id : definition.checkpoint_runtime_image_ids) {
        if (!valid_identifier_component(image_id) ||
            !checkpoint_runtime_image_ids.insert(image_id).second)
            invalid_definition("checkpoint-runtime-image-id");
    }

    std::set<std::tuple<std::uint64_t, std::uint64_t>>
        bootstrap_write_ranges;
    for (const auto& write : definition.bootstrap.writes) {
        const auto physical = canonical_physical_address(write.guest_address);
        const auto relative =
            physical >= native_port_main_memory_physical_base
                ? physical - native_port_main_memory_physical_base
                : native_port_main_memory_physical_span;
        const auto backing_offset =
            relative < native_port_main_memory_physical_span
                ? relative & (native_port_main_memory_backing_size - 1u)
                : native_port_main_memory_backing_size;
        if (write.byte_size == 0u ||
            backing_offset >= native_port_main_memory_backing_size ||
            write.byte_size >
                native_port_main_memory_backing_size - backing_offset ||
            write.byte_size >
                native_port_main_memory_physical_span - relative ||
            !valid_native_port_sha256_identity(
                write.pre_write_identity) ||
            !valid_native_port_sha256_identity(
                write.post_write_identity) ||
            !valid_bootstrap_write_policy(write.policy))
            invalid_definition("bootstrap-write-binding");
        const auto range = std::tuple{
            static_cast<std::uint64_t>(backing_offset),
            static_cast<std::uint64_t>(backing_offset) + write.byte_size};
        for (const auto& existing : bootstrap_write_ranges) {
            if (std::get<0>(range) < std::get<1>(existing) &&
                std::get<0>(existing) < std::get<1>(range))
                invalid_definition("overlapping-bootstrap-write-bindings");
        }
        bootstrap_write_ranges.insert(range);
    }

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
    if (!range_inside_image(definition.bootstrap.post_entry_point, 2u))
        invalid_definition("post-entry-outside-images");
    for (const auto root : definition.bootstrap.post_aot_roots) {
        if (!range_inside_image(root, 2u))
            invalid_definition("post-aot-root-outside-images");
    }
    for (const auto& continuation :
         definition.bootstrap.post_aot_continuations) {
        if (!range_inside_image(continuation.function_entry, 2u) ||
            !range_inside_image(continuation.resume_address, 2u))
            invalid_definition("post-aot-continuation-outside-images");
    }

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
            native_port_hook_is_executable(hook.requirement) ||
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
            hook.covered_size != 2u && hook.covered_size != 4u)
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
    const auto acceptance_hook = std::find_if(
        definition.hooks.begin(), definition.hooks.end(),
        [&](const auto& hook) {
            return hook.guest_address ==
                   definition.acceptance.witness_hook_guest_address;
        });
    if (acceptance_hook == definition.hooks.end() ||
        !native_port_hook_closes_product_contract(
            acceptance_hook->requirement))
        invalid_definition("acceptance-witness-hook");

    std::set<std::uint32_t> resolved_instructions;
    for (const auto& resolution : definition.hardware_resolutions) {
        if ((resolution.instruction_address & 1u) != 0u ||
            !range_inside_image(resolution.instruction_address, 2u) ||
            !resolved_instructions.insert(
                resolution.instruction_address).second)
            invalid_definition("hardware-resolution");
        const auto hook = std::find_if(
            definition.hooks.begin(),
            definition.hooks.end(),
            [&](const auto& candidate) {
                return candidate.guest_address ==
                       resolution.hook_guest_address;
            });
        // Runtime validation can prove only structural containment. Export
        // admission separately proves the exact function boundary and rejects
        // every external, guarded, seeded or resume entry into the covered
        // interior before it may certify an interior hardware instruction.
        if (hook == definition.hooks.end() ||
            !native_port_hook_closes_product_contract(
                hook->requirement) ||
            hook->original_policy !=
                NativePortHookOriginalPolicy::ReplacesOriginal ||
            resolution.instruction_address < hook->guest_address ||
            static_cast<std::uint64_t>(resolution.instruction_address) + 2u >
                static_cast<std::uint64_t>(hook->guest_address) +
                    hook->covered_size)
            invalid_definition("hook-hardware-resolution");
    }
}

} // namespace katana::runtime
