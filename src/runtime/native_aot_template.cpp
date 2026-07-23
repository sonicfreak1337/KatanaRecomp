#include "katana/runtime/native_aot_template.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::uint64_t guest_address_space_extent =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
constexpr std::uint32_t maximum_template_extent = 1024u * 1024u;
constexpr std::size_t maximum_patch_slots = 4096u;
constexpr std::size_t maximum_patch_targets = 65536u;

bool direct_mapped_alias_range(const std::uint32_t address,
                               const std::uint32_t extent) noexcept {
    if (extent == 0u) return false;
    const auto end = static_cast<std::uint64_t>(address) + extent - 1u;
    if (end > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto segment = address & 0xE0000000u;
    return (segment == 0x80000000u || segment == 0xA0000000u) &&
           (static_cast<std::uint32_t>(end) & 0xE0000000u) == segment;
}

bool direct_mapped_alias(const std::uint32_t address) noexcept {
    return direct_mapped_alias_range(address, 1u);
}

struct MatchingTemplate {
    const NativeAotTemplate* definition = nullptr;
    CodeAddressMapping mapping;
};

std::uint32_t add_signed_wrapping(const std::uint32_t base, const std::int32_t delta) noexcept {
    return base + static_cast<std::uint32_t>(delta);
}

bool contains(const std::uint32_t address,
              const std::uint32_t start,
              const std::uint32_t extent) noexcept {
    return static_cast<std::uint64_t>(address) >= start &&
           static_cast<std::uint64_t>(address) < static_cast<std::uint64_t>(start) + extent;
}

bool linear_physical_range(const std::uint32_t address, const std::uint32_t extent) noexcept {
    if (extent == 0u || static_cast<std::uint64_t>(address) + extent > guest_address_space_extent)
        return false;
    const auto physical = canonical_physical_address(address);
    const auto final = address + extent - 1u;
    return static_cast<std::uint64_t>(physical) + extent - 1u <=
               std::numeric_limits<std::uint32_t>::max() &&
           canonical_physical_address(final) == physical + extent - 1u;
}

std::optional<std::uint32_t> module_offset(const ExecutableModule& module,
                                           const std::uint32_t source_start,
                                           const std::uint32_t extent) noexcept {
    if (!linear_physical_range(module.guest_start,
                               static_cast<std::uint32_t>(module.bytes.size())) ||
        !linear_physical_range(source_start, extent))
        return std::nullopt;
    const auto module_start = canonical_physical_address(module.guest_start);
    const auto source = canonical_physical_address(source_start);
    const auto module_end = static_cast<std::uint64_t>(module_start) + module.bytes.size();
    const auto source_end = static_cast<std::uint64_t>(source) + extent;
    if (source < module_start || source_end > module_end) return std::nullopt;
    return source - module_start;
}

std::uint32_t read_u32(const Memory& memory, const std::uint32_t address) {
    std::uint32_t result = 0u;
    for (std::uint32_t byte = 0u; byte < sizeof(result); ++byte)
        result |= static_cast<std::uint32_t>(memory.read_u8(address + byte)) << (byte * 8u);
    return result;
}

NativeAotTemplateBindResult reject(const NativeAotTemplateBindFailure failure) {
    NativeAotTemplateBindResult result;
    result.failure = failure;
    result.candidate.rejection_failure = MaterializationFailure::AotTemplateMismatch;
    return result;
}

} // namespace

NativeAotTemplateBinder::NativeAotTemplateBinder(
    CpuState& cpu,
    const ExecutableModuleCatalog& modules,
    const RuntimeBlockTable& blocks,
    const std::span<const NativeAotTemplate> templates) noexcept
    : cpu_(cpu), modules_(modules), blocks_(blocks), templates_(templates) {}

NativeAotTemplateBindResult NativeAotTemplateBinder::bind(
    const std::uint32_t target,
    const std::uint32_t physical_origin,
    const std::span<const std::uint8_t> target_suffix,
    const BlockVariantKey& variant) const {
    std::optional<MatchingTemplate> match;
    for (const auto& definition : templates_) {
        if (definition.extent == 0u) continue;
        const CodeAddressMapping mapping{definition.source_start,
                                         add_signed_wrapping(cpu_.vbr,
                                                             definition.destination_vbr_delta),
                                         definition.extent};
        if (!contains(target, mapping.runtime_start, mapping.extent)) continue;
        if (match.has_value())
            return reject(NativeAotTemplateBindFailure::AmbiguousDestination);
        match = MatchingTemplate{&definition, mapping};
    }
    if (!match.has_value())
        return reject(NativeAotTemplateBindFailure::NoMatchingDestination);

    const auto& definition = *match->definition;
    const auto& mapping = match->mapping;
    try {
        validate_code_address_mapping(mapping);
    } catch (const std::exception&) {
        return reject(NativeAotTemplateBindFailure::InvalidDefinition);
    }
    if ((target & 1u) != 0u || (physical_origin & 1u) != 0u ||
        (definition.source_start & 3u) != 0u || (mapping.runtime_start & 3u) != 0u ||
        (definition.extent & 3u) != 0u || definition.extent > maximum_template_extent ||
        !direct_mapped_alias_range(definition.source_start, definition.extent) ||
        !direct_mapped_alias_range(mapping.runtime_start, mapping.extent) ||
        !linear_physical_range(definition.source_start, definition.extent) ||
        !linear_physical_range(mapping.runtime_start, mapping.extent) ||
        physical_origin != canonical_physical_address(target) ||
        definition.patches.size() > maximum_patch_slots)
        return reject(NativeAotTemplateBindFailure::InvalidDefinition);

    std::vector<const NativeAotTemplatePatch*> ordered_patches;
    ordered_patches.reserve(definition.patches.size());
    std::size_t total_patch_targets = 0u;
    for (const auto& patch : definition.patches) {
        if ((patch.source_offset & 3u) != 0u || patch.source_offset > definition.extent ||
            sizeof(std::uint32_t) > definition.extent - patch.source_offset ||
            patch.allowed_targets.empty())
            return reject(NativeAotTemplateBindFailure::InvalidDefinition);
        if (patch.allowed_targets.size() > maximum_patch_targets - total_patch_targets)
            return reject(NativeAotTemplateBindFailure::InvalidDefinition);
        total_patch_targets += patch.allowed_targets.size();
        std::unordered_map<std::uint32_t, std::uint32_t> live_bindings;
        live_bindings.reserve(patch.allowed_targets.size());
        for (const auto patch_target : patch.allowed_targets) {
            if ((patch_target.live_value & 1u) != 0u ||
                (patch_target.block_address & 1u) != 0u ||
                !direct_mapped_alias(patch_target.live_value) ||
                !direct_mapped_alias(patch_target.block_address) ||
                canonical_physical_address(patch_target.live_value) !=
                    canonical_physical_address(patch_target.block_address))
                return reject(NativeAotTemplateBindFailure::InvalidDefinition);
            const auto [known, inserted] =
                live_bindings.emplace(patch_target.live_value, patch_target.block_address);
            if (!inserted && known->second != patch_target.block_address)
                return reject(NativeAotTemplateBindFailure::InvalidDefinition);
        }
        ordered_patches.push_back(&patch);
    }
    std::sort(ordered_patches.begin(), ordered_patches.end(), [](const auto* left, const auto* right) {
        return left->source_offset < right->source_offset;
    });
    for (std::size_t index = 1u; index < ordered_patches.size(); ++index) {
        if (ordered_patches[index]->source_offset <
            ordered_patches[index - 1u]->source_offset + sizeof(std::uint32_t))
            return reject(NativeAotTemplateBindFailure::InvalidDefinition);
    }

    const auto* source_module = modules_.find(definition.source_module_id);
    if (source_module == nullptr)
        return reject(NativeAotTemplateBindFailure::SourceModuleMissing);
    if (definition.expected_source_identity.empty() ||
        source_module->source_identity != definition.expected_source_identity)
        return reject(NativeAotTemplateBindFailure::SourceIdentityMismatch);
    if (!source_module->relocations.empty())
        return reject(NativeAotTemplateBindFailure::InvalidDefinition);
    const auto source_module_offset =
        module_offset(*source_module, definition.source_start, definition.extent);
    if (!source_module_offset.has_value())
        return reject(NativeAotTemplateBindFailure::SourceIdentityMismatch);

    const auto block_offset = target - mapping.runtime_start;
    if (physical_origin < block_offset)
        return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);
    const auto runtime_physical = physical_origin - block_offset;
    if ((runtime_physical & 3u) != 0u ||
        static_cast<std::uint64_t>(runtime_physical) + definition.extent >
            guest_address_space_extent)
        return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);
    if (!cpu_.memory.contains(runtime_physical, definition.extent))
        return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);

    const auto validate_run = [&](const std::uint32_t offset,
                                  const std::uint32_t size,
                                  const bool require_original_identity)
        -> NativeAotTemplateBindFailure {
        if (size == 0u) return NativeAotTemplateBindFailure::None;
        if (require_original_identity &&
            !source_module->contains(definition.source_start + offset, size))
            return NativeAotTemplateBindFailure::SourceIdentityMismatch;
        for (std::uint32_t byte = 0u; byte < size; ++byte) {
            const auto current = offset + byte;
            const auto runtime_byte = cpu_.memory.read_u8(runtime_physical + current);
            if (require_original_identity &&
                runtime_byte != source_module->bytes[*source_module_offset + current])
                return NativeAotTemplateBindFailure::RuntimeBytesMismatch;
        }
        return NativeAotTemplateBindFailure::None;
    };
    std::uint32_t validation_cursor = 0u;
    for (const auto* patch : ordered_patches) {
        const auto prefix = patch->source_offset - validation_cursor;
        if (const auto failure = validate_run(validation_cursor, prefix, true);
            failure != NativeAotTemplateBindFailure::None)
            return reject(failure);
        if (const auto failure =
                validate_run(patch->source_offset, sizeof(std::uint32_t), false);
            failure != NativeAotTemplateBindFailure::None)
            return reject(failure);
        validation_cursor = patch->source_offset + sizeof(std::uint32_t);
    }
    if (const auto failure =
            validate_run(validation_cursor, definition.extent - validation_cursor, true);
        failure != NativeAotTemplateBindFailure::None)
        return reject(failure);

    for (const auto* patch : ordered_patches) {
        const auto runtime_value = read_u32(cpu_.memory, runtime_physical + patch->source_offset);
        const auto allowed =
            std::find_if(patch->allowed_targets.begin(),
                         patch->allowed_targets.end(),
                         [runtime_value](const auto candidate) {
                             return candidate.live_value == runtime_value;
                         });
        if (allowed == patch->allowed_targets.end())
            return reject(NativeAotTemplateBindFailure::PatchTargetRejected);
        const auto handler = blocks_.lookup(allowed->block_address, {});
        if (!handler.has_value())
            return reject(NativeAotTemplateBindFailure::PatchTargetRejected);
        const auto handler_block = blocks_.resolve(*handler);
        if (!handler_block.has_value() || handler_block->get().runtime_registered ||
            handler_block->get().function == nullptr ||
            handler_block->get().physical_origin !=
                canonical_physical_address(allowed->block_address))
            return reject(NativeAotTemplateBindFailure::PatchTargetRejected);
    }

    const auto offset = block_offset;
    const auto source_address = mapping.source_start + offset;
    const auto source_handle = blocks_.lookup(source_address, {});
    if (!source_handle.has_value())
        return reject(NativeAotTemplateBindFailure::SourceBlockMissing);
    const auto source_block = blocks_.resolve(*source_handle);
    if (!source_block.has_value() || source_block->get().runtime_registered ||
        source_block->get().aot_template.has_value() || source_block->get().function == nullptr ||
        source_block->get().physical_origin != canonical_physical_address(source_address) ||
        source_block->get().size < 2u || (source_block->get().size & 1u) != 0u ||
        source_block->get().size > definition.extent - offset ||
        source_block->get().size > target_suffix.size() ||
        !source_module->materializable(source_address, source_block->get().size))
        return reject(NativeAotTemplateBindFailure::SourceBlockMissing);
    const auto source_block_end = static_cast<std::uint64_t>(offset) + source_block->get().size;
    if (std::any_of(ordered_patches.begin(), ordered_patches.end(), [&](const auto* patch) {
            const auto patch_end = static_cast<std::uint64_t>(patch->source_offset) +
                                   sizeof(std::uint32_t);
            return offset < patch_end && patch->source_offset < source_block_end;
        }))
        return reject(NativeAotTemplateBindFailure::SourceBlockMissing);
    for (std::uint32_t byte = 0u; byte < source_block->get().size; ++byte) {
        if (target_suffix[byte] != cpu_.memory.read_u8(runtime_physical + offset + byte))
            return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);
    }

    NativeAotTemplateBindResult result;
    auto& candidate = result.candidate;
    candidate.block.virtual_start = target;
    candidate.block.physical_origin = physical_origin;
    candidate.block.size = source_block->get().size;
    candidate.block.end_kind = source_block->get().end_kind;
    candidate.block.variant = variant;
    candidate.block.function = source_block->get().function;
    candidate.block.provenance = "native-aot-template:" + definition.source_module_id;
    candidate.block.aot_template = RuntimeAotTemplateContract{mapping, definition.extent};
    candidate.decode_candidate_validated = true;
    candidate.interpreter_backed = false;
    candidate.bounded_analysis_complete = true;
    candidate.ir_verified = true;
    candidate.code_generated = true;
    candidate.instructions = std::max<std::uint64_t>(1u, candidate.block.size / 2u);
    candidate.recursive_seeds = 1u;
    return result;
}

const char* native_aot_template_bind_failure_name(
    const NativeAotTemplateBindFailure failure) noexcept {
    switch (failure) {
    case NativeAotTemplateBindFailure::None:
        return "none";
    case NativeAotTemplateBindFailure::NoMatchingDestination:
        return "no-matching-destination";
    case NativeAotTemplateBindFailure::AmbiguousDestination:
        return "ambiguous-destination";
    case NativeAotTemplateBindFailure::InvalidDefinition:
        return "invalid-definition";
    case NativeAotTemplateBindFailure::SourceModuleMissing:
        return "source-module-missing";
    case NativeAotTemplateBindFailure::SourceIdentityMismatch:
        return "source-identity-mismatch";
    case NativeAotTemplateBindFailure::RuntimeBytesMismatch:
        return "runtime-bytes-mismatch";
    case NativeAotTemplateBindFailure::PatchTargetRejected:
        return "patch-target-rejected";
    case NativeAotTemplateBindFailure::SourceBlockMissing:
        return "source-block-missing";
    }
    return "unknown";
}

} // namespace katana::runtime
