#include "katana/runtime/native_aot_template.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/runtime/block_guards.hpp"

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
constexpr std::uint32_t maximum_template_extent = 4u * 1024u * 1024u;
constexpr std::uint32_t maximum_fixed_template_extent =
    16u * 1024u * 1024u;
constexpr std::size_t maximum_patch_slots = 4096u;
constexpr std::size_t maximum_patch_targets = 65536u;
constexpr std::size_t maximum_mutable_ranges = 4096u;
constexpr std::size_t maximum_block_identities = 65536u;
constexpr std::string_view runtime_write_module_id_prefix = "guest-runtime-write-";

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

bool valid_sha256(const std::string_view value) noexcept {
    const auto digest =
        value.starts_with("sha256:") ? value.substr(std::string_view{"sha256:"}.size())
                                    : value;
    if (digest.size() != 64u) return false;
    return std::all_of(digest.begin(), digest.end(), [](const char value) {
        return (value >= '0' && value <= '9') ||
               (value >= 'a' && value <= 'f') ||
               (value >= 'A' && value <= 'F');
    });
}

bool block_identities_valid(
    const std::span<const NativeAotTemplateBlockIdentity> identities,
    const std::uint32_t extent) noexcept {
    if (identities.empty() || identities.size() > maximum_block_identities)
        return false;
    std::uint64_t prior_end = 0u;
    for (const auto& identity : identities) {
        const auto end =
            static_cast<std::uint64_t>(identity.source_offset) + identity.size;
        if ((identity.source_offset & 1u) != 0u || identity.size < 2u ||
            (identity.size & 1u) != 0u || end > extent ||
            identity.source_offset < prior_end || !valid_sha256(identity.sha256))
            return false;
        prior_end = end;
    }
    return true;
}

bool sha256_matches(const std::string_view expected,
                    const std::string_view bytes) {
    const auto actual = katana::io::sha256_bytes(bytes);
    return expected == actual ||
           (expected.starts_with("sha256:") &&
            expected.substr(std::string_view{"sha256:"}.size()) == actual);
}

struct MatchingTemplate {
    const NativeAotTemplate* definition = nullptr;
    CodeAddressMapping mapping;
    std::size_t definition_index = 0u;
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
    switch (failure) {
    case NativeAotTemplateBindFailure::MissingAot:
        result.candidate.rejection_failure = MaterializationFailure::MissingAot;
        break;
    case NativeAotTemplateBindFailure::RuntimeContentIdentityMismatch:
    case NativeAotTemplateBindFailure::RuntimeBytesMismatch:
        result.candidate.rejection_failure = MaterializationFailure::ByteIdentityMismatch;
        break;
    default:
        result.candidate.rejection_failure = MaterializationFailure::AotTemplateMismatch;
        break;
    }
    return result;
}

NativeAotTemplateBindResult reject_anonymous_runtime_without_fixed_contract() {
    auto result = reject(NativeAotTemplateBindFailure::NoMatchingDestination);
    result.candidate.rejection_failure = MaterializationFailure::MissingAot;
    return result;
}

bool linear_instruction_mapping(const CpuState& cpu,
                                const std::uint32_t virtual_start,
                                const std::uint32_t physical_start,
                                const std::uint32_t extent) noexcept {
    if (extent == 0u ||
        static_cast<std::uint64_t>(virtual_start) + extent > guest_address_space_extent ||
        static_cast<std::uint64_t>(physical_start) + extent > guest_address_space_extent)
        return false;
    const auto translated_at = [&](const std::uint32_t offset) {
        std::uint32_t translated = 0u;
        try {
            translated =
                cpu.address_space
                    ? cpu.address_space
                          ->inspect_translation(virtual_start + offset,
                                                TranslationAccess::Instruction,
                                                cpu.privileged_mode())
                          .physical_address
                    : canonical_physical_address(virtual_start + offset);
        } catch (const TranslationError&) {
            return false;
        }
        return translated == physical_start + offset;
    };
    if (!translated_at(0u) || !translated_at(extent - 2u)) return false;
    constexpr std::uint64_t smallest_tlb_page = 1024u;
    const auto virtual_end = static_cast<std::uint64_t>(virtual_start) + extent;
    auto boundary =
        (static_cast<std::uint64_t>(virtual_start) / smallest_tlb_page + 1u) *
        smallest_tlb_page;
    while (boundary < virtual_end) {
        const auto offset = static_cast<std::uint32_t>(boundary - virtual_start);
        if (!translated_at(offset) || (offset >= 2u && !translated_at(offset - 2u)))
            return false;
        boundary += smallest_tlb_page;
    }
    return true;
}

} // namespace

NativeAotTemplateBinder::NativeAotTemplateBinder(
    CpuState& cpu,
    const ExecutableModuleCatalog& modules,
    const RuntimeBlockTable& blocks,
    const std::span<const NativeAotTemplate> templates,
    const RuntimeBlockTable* const fixed_source_blocks)
    : cpu_(cpu), modules_(modules), blocks_(blocks), templates_(templates),
      fixed_source_blocks_(fixed_source_blocks) {
    fixed_block_identities_valid_.reserve(templates_.size());
    for (const auto& definition : templates_) {
        fixed_block_identities_valid_.push_back(
            definition.destination !=
                    NativeAotTemplateDestination::FixedAddress ||
                block_identities_valid(
                    definition.block_identities, definition.extent));
    }
}

BlockMaterializationProbe
NativeAotTemplateBinder::fixed_block_materialization_probe(
    const std::uint32_t target,
    const std::uint32_t physical_origin) const noexcept {
    BlockMaterializationProbe result;
    std::size_t matching_ranges = 0u;
    for (std::size_t definition_index = 0u;
         definition_index < templates_.size();
         ++definition_index) {
        const auto& definition = templates_[definition_index];
        if (definition.destination !=
                NativeAotTemplateDestination::FixedAddress ||
            definition.extent == 0u ||
            !contains(target,
                      definition.fixed_runtime_start,
                      definition.extent))
            continue;
        ++matching_ranges;
        if (matching_ranges != 1u ||
            (target & 1u) != 0u ||
            physical_origin != canonical_physical_address(target) ||
            !fixed_block_identities_valid_[definition_index] ||
            definition.extent > maximum_fixed_template_extent ||
            (definition.source_start & 3u) != 0u ||
            (definition.fixed_runtime_start & 3u) != 0u ||
            (definition.extent & 3u) != 0u ||
            !definition.source_module_id.empty() ||
            !definition.expected_source_identity.empty() ||
            definition.destination_vbr_delta != 0 ||
            !definition.expected_runtime_content_identity.empty() ||
            !definition.expected_runtime_byte_identity.empty() ||
            !definition.patches.empty() ||
            !definition.mutable_ranges.empty() ||
            definition.validation_mode !=
                NativeAotTemplateValidationMode::RuntimeBlock ||
            !direct_mapped_alias_range(
                definition.source_start, definition.extent) ||
            !direct_mapped_alias_range(
                definition.fixed_runtime_start, definition.extent) ||
            !linear_physical_range(
                definition.source_start, definition.extent)) {
            result.kind = BlockMaterializationProbeKind::Rejected;
            result.rejection_failure =
                MaterializationFailure::AotTemplateMismatch;
            continue;
        }

        const auto offset = target - definition.fixed_runtime_start;
        const auto identity = std::lower_bound(
            definition.block_identities.begin(),
            definition.block_identities.end(),
            offset,
            [](const auto& candidate, const std::uint32_t value) {
                return candidate.source_offset < value;
            });
        if (identity == definition.block_identities.end() ||
            identity->source_offset != offset) {
            result.kind = BlockMaterializationProbeKind::Rejected;
            result.rejection_failure = MaterializationFailure::MissingAot;
            continue;
        }
        if (fixed_source_blocks_ == nullptr) {
            result.kind = BlockMaterializationProbeKind::Rejected;
            result.rejection_failure = MaterializationFailure::MissingAot;
            continue;
        }
        const auto source_address = definition.source_start + offset;
        const auto source_handle =
            fixed_source_blocks_->lookup(source_address, {});
        if (!source_handle.has_value()) {
            result.kind = BlockMaterializationProbeKind::Rejected;
            result.rejection_failure = MaterializationFailure::MissingAot;
            continue;
        }
        const auto source_block =
            fixed_source_blocks_->resolve(*source_handle);
        if (!source_block.has_value() ||
            source_block->get().runtime_registered ||
            source_block->get().aot_template.has_value() ||
            source_block->get().function == nullptr ||
            source_block->get().physical_origin !=
                canonical_physical_address(source_address) ||
            source_block->get().size != identity->size) {
            result.kind = BlockMaterializationProbeKind::Rejected;
            result.rejection_failure = MaterializationFailure::MissingAot;
            continue;
        }
        result.kind = BlockMaterializationProbeKind::IdentityBound;
        result.required_bytes = identity->size;
        result.rejection_failure = MaterializationFailure::None;
    }
    if (matching_ranges > 1u) {
        result.kind = BlockMaterializationProbeKind::Rejected;
        result.required_bytes = 0u;
        result.rejection_failure =
            MaterializationFailure::AotTemplateMismatch;
    }
    return result;
}

NativeAotTemplateBindResult NativeAotTemplateBinder::bind(
    const std::uint32_t target,
    const std::uint32_t physical_origin,
    const std::span<const std::uint8_t> target_suffix,
    const BlockVariantKey& variant) const {
    std::optional<MatchingTemplate> match;
    const auto* loaded_module = modules_.resolve(physical_origin, 2u);
    bool has_loaded_module_definition = false;
    bool loaded_content_identity_matched = false;
    bool loaded_byte_identity_matched = false;
    bool loaded_byte_identity_mismatched = false;
    for (std::size_t definition_index = 0u;
         definition_index < templates_.size();
         ++definition_index) {
        const auto& definition = templates_[definition_index];
        if (definition.extent == 0u) continue;
        CodeAddressMapping mapping;
        if (definition.destination == NativeAotTemplateDestination::VbrRelative) {
            mapping = {definition.source_start,
                       add_signed_wrapping(cpu_.vbr, definition.destination_vbr_delta),
                       definition.extent};
        } else if (definition.destination ==
                   NativeAotTemplateDestination::LoadedModule) {
            has_loaded_module_definition = true;
            if (loaded_module == nullptr ||
                loaded_module->content_identity != definition.expected_runtime_content_identity)
                continue;
            loaded_content_identity_matched = true;
            if (loaded_module->bytes.size() != definition.extent ||
                loaded_module->active_extents.size() != 1u ||
                loaded_module->active_extents.front() !=
                    ExecutableModuleActiveExtent{0u, definition.extent})
                continue;
            if (loaded_module->byte_identity != definition.expected_runtime_byte_identity) {
                loaded_byte_identity_mismatched = true;
                continue;
            }
            loaded_byte_identity_matched = true;
            const auto offset = module_offset(*loaded_module, physical_origin, 2u);
            if (!offset.has_value() || target < *offset) continue;
            mapping = {definition.source_start, target - *offset, definition.extent};
        } else {
            mapping = {definition.source_start,
                       definition.fixed_runtime_start,
                       definition.extent};
        }
        if (!contains(target, mapping.runtime_start, mapping.extent)) continue;
        if (match.has_value())
            return reject(NativeAotTemplateBindFailure::AmbiguousDestination);
        match =
            MatchingTemplate{&definition, mapping, definition_index};
    }
    const auto anonymous_runtime_module =
        loaded_module != nullptr &&
        loaded_module->id.starts_with(runtime_write_module_id_prefix);
    if (!match.has_value() && anonymous_runtime_module)
        return reject_anonymous_runtime_without_fixed_contract();
    if (!match.has_value() && has_loaded_module_definition && loaded_module != nullptr) {
        if (!loaded_content_identity_matched)
            return reject(NativeAotTemplateBindFailure::RuntimeContentIdentityMismatch);
        if (!loaded_byte_identity_matched && loaded_byte_identity_mismatched)
            return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);
        return reject(NativeAotTemplateBindFailure::MissingAot);
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
    const auto vbr_relative =
        definition.destination == NativeAotTemplateDestination::VbrRelative;
    const auto loaded_destination =
        definition.destination == NativeAotTemplateDestination::LoadedModule;
    const auto fixed_destination =
        definition.destination == NativeAotTemplateDestination::FixedAddress;
    const auto source_module_definition_valid =
        (fixed_destination &&
         definition.source_module_id.empty() &&
         definition.expected_source_identity.empty()) ||
        (!fixed_destination && !definition.source_module_id.empty() &&
         !definition.expected_source_identity.empty());
    const auto trailing_destination_contract_valid =
        fixed_destination
            ? definition.destination_vbr_delta == 0 &&
                  definition.expected_runtime_content_identity.empty() &&
                  definition.expected_runtime_byte_identity.empty() &&
                  definition.patches.empty() && definition.mutable_ranges.empty() &&
                  fixed_block_identities_valid_[
                      match->definition_index] &&
                  definition.validation_mode ==
                      NativeAotTemplateValidationMode::RuntimeBlock
            : definition.fixed_runtime_start == 0u &&
                  definition.block_identities.empty() &&
                  definition.validation_mode ==
                      NativeAotTemplateValidationMode::SourceModule;
    if ((target & 1u) != 0u || (physical_origin & 1u) != 0u ||
        (definition.source_start & 3u) != 0u || (mapping.runtime_start & 3u) != 0u ||
        (definition.extent & 3u) != 0u ||
        definition.extent >
            (fixed_destination ? maximum_fixed_template_extent
                               : maximum_template_extent) ||
        !source_module_definition_valid || !trailing_destination_contract_valid ||
        !direct_mapped_alias_range(definition.source_start, definition.extent) ||
        (fixed_destination &&
         !direct_mapped_alias_range(mapping.runtime_start, mapping.extent)) ||
        (vbr_relative &&
         !direct_mapped_alias_range(mapping.runtime_start, mapping.extent)) ||
        !linear_physical_range(definition.source_start, definition.extent) ||
        (vbr_relative &&
         (!linear_physical_range(mapping.runtime_start, mapping.extent) ||
          physical_origin != canonical_physical_address(target))) ||
        (loaded_destination &&
         !linear_instruction_mapping(cpu_,
                                     mapping.runtime_start,
                                     canonical_physical_address(loaded_module->guest_start),
                                     mapping.extent)) ||
        (loaded_destination &&
         (definition.destination_vbr_delta != 0 ||
          definition.expected_runtime_content_identity.empty() ||
          definition.expected_runtime_byte_identity.empty() ||
          definition.expected_source_identity != definition.expected_runtime_byte_identity ||
          !definition.patches.empty() || !definition.mutable_ranges.empty())) ||
        definition.patches.size() > maximum_patch_slots ||
        definition.mutable_ranges.size() > maximum_mutable_ranges ||
        !native_aot_mutable_ranges_valid(definition.mutable_ranges, definition.extent))
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
    for (const auto* patch : ordered_patches) {
        const auto patch_end =
            static_cast<std::uint64_t>(patch->source_offset) + sizeof(std::uint32_t);
        if (std::any_of(definition.mutable_ranges.begin(),
                        definition.mutable_ranges.end(),
                        [&](const auto range) {
                            const auto range_end =
                                static_cast<std::uint64_t>(range.offset) + range.size;
                            return patch->source_offset < range_end &&
                                   range.offset < patch_end;
                        }))
            return reject(NativeAotTemplateBindFailure::InvalidDefinition);
    }

    const ExecutableModule* source_module = nullptr;
    std::optional<std::uint32_t> source_module_offset;
    if (vbr_relative) {
        source_module = modules_.find(definition.source_module_id);
        if (source_module == nullptr)
            return reject(NativeAotTemplateBindFailure::SourceModuleMissing);
        if (source_module->source_identity != definition.expected_source_identity)
            return reject(NativeAotTemplateBindFailure::SourceIdentityMismatch);
        if (!source_module->relocations.empty())
            return reject(NativeAotTemplateBindFailure::InvalidDefinition);
        source_module_offset =
            module_offset(*source_module, definition.source_start, definition.extent);
        if (!source_module_offset.has_value())
            return reject(NativeAotTemplateBindFailure::SourceIdentityMismatch);
    }

    const auto block_offset = target - mapping.runtime_start;
    if (physical_origin < block_offset)
        return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);
    const auto runtime_physical = physical_origin - block_offset;
    if ((runtime_physical & 3u) != 0u ||
        static_cast<std::uint64_t>(runtime_physical) + definition.extent >
            guest_address_space_extent)
        return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);
    if (!fixed_destination &&
        !cpu_.memory.contains(runtime_physical, definition.extent))
        return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);
    if (fixed_destination &&
        (physical_origin != canonical_physical_address(target) ||
         runtime_physical !=
             canonical_physical_address(mapping.runtime_start)))
        return reject(NativeAotTemplateBindFailure::InvalidDefinition);

    if (vbr_relative) {
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
                if (require_original_identity &&
                    native_aot_offset_is_mutable(definition.mutable_ranges, current))
                    continue;
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
    }

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
    if (fixed_destination && fixed_source_blocks_ == nullptr)
        return reject(NativeAotTemplateBindFailure::MissingAot);
    const auto& source_blocks =
        fixed_destination ? *fixed_source_blocks_ : blocks_;
    const auto source_handle = source_blocks.lookup(source_address, {});
    const auto destination_requires_precompiled_entry =
        loaded_destination || fixed_destination;
    if (!source_handle.has_value())
        return reject(destination_requires_precompiled_entry
                          ? NativeAotTemplateBindFailure::MissingAot
                          : NativeAotTemplateBindFailure::SourceBlockMissing);
    const auto source_block = source_blocks.resolve(*source_handle);
    if (!source_block.has_value() || source_block->get().runtime_registered ||
        source_block->get().aot_template.has_value() || source_block->get().function == nullptr ||
        source_block->get().physical_origin != canonical_physical_address(source_address) ||
        source_block->get().size < 2u || (source_block->get().size & 1u) != 0u ||
        source_block->get().size > definition.extent - offset ||
        source_block->get().size > target_suffix.size())
        return reject(destination_requires_precompiled_entry
                          ? NativeAotTemplateBindFailure::MissingAot
                          : NativeAotTemplateBindFailure::SourceBlockMissing);
    std::optional<std::uint32_t> loaded_block_offset;
    if (loaded_destination) {
        loaded_block_offset =
            module_offset(*loaded_module, physical_origin, source_block->get().size);
        if (!loaded_block_offset.has_value() ||
            !loaded_module->materializable(physical_origin, source_block->get().size))
            return reject(NativeAotTemplateBindFailure::MissingAot);
    } else if (vbr_relative &&
               !source_module->materializable(source_address,
                                              source_block->get().size)) {
        return reject(NativeAotTemplateBindFailure::SourceBlockMissing);
    }
    const auto source_block_end = static_cast<std::uint64_t>(offset) + source_block->get().size;
    if (std::any_of(ordered_patches.begin(), ordered_patches.end(), [&](const auto* patch) {
            const auto patch_end = static_cast<std::uint64_t>(patch->source_offset) +
                                   sizeof(std::uint32_t);
            return offset < patch_end && patch->source_offset < source_block_end;
        }))
        return reject(NativeAotTemplateBindFailure::SourceBlockMissing);
    if (std::any_of(definition.mutable_ranges.begin(),
                    definition.mutable_ranges.end(),
                    [&](const auto range) {
                        const auto range_end =
                            static_cast<std::uint64_t>(range.offset) + range.size;
                        return offset < range_end && range.offset < source_block_end;
                    }))
        return reject(NativeAotTemplateBindFailure::SourceBlockMissing);
    for (std::uint32_t byte = 0u; byte < source_block->get().size; ++byte) {
        if (target_suffix[byte] != cpu_.memory.read_u8(runtime_physical + offset + byte) ||
            (loaded_block_offset.has_value() &&
             target_suffix[byte] != loaded_module->bytes[*loaded_block_offset + byte]))
            return reject(NativeAotTemplateBindFailure::RuntimeBytesMismatch);
    }
    if (fixed_destination) {
        const auto identity = std::lower_bound(
            definition.block_identities.begin(),
            definition.block_identities.end(),
            offset,
            [](const auto& candidate, const std::uint32_t value) {
                return candidate.source_offset < value;
            });
        if (identity == definition.block_identities.end() ||
            identity->source_offset != offset ||
            identity->size != source_block->get().size)
            return reject(NativeAotTemplateBindFailure::MissingAot);
        std::string runtime_bytes;
        runtime_bytes.resize(source_block->get().size);
        for (std::uint32_t byte = 0u; byte < source_block->get().size; ++byte) {
            runtime_bytes[byte] = static_cast<char>(
                cpu_.memory.read_u8(physical_origin + byte));
        }
        if (!sha256_matches(identity->sha256, runtime_bytes))
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
    candidate.block.provenance =
        "native-aot-template:" +
        (definition.source_module_id.empty()
             ? std::string{"fixed-address"}
             : definition.source_module_id);
    candidate.block.aot_template =
        RuntimeAotTemplateContract{
            mapping,
            definition.validation_mode == NativeAotTemplateValidationMode::RuntimeBlock
                ? source_block->get().size
                : definition.extent,
            definition.mutable_ranges,
            definition.validation_mode};
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
    case NativeAotTemplateBindFailure::RuntimeContentIdentityMismatch:
        return "runtime-content-identity-mismatch";
    case NativeAotTemplateBindFailure::MissingAot:
        return "missing-aot";
    }
    return "unknown";
}

} // namespace katana::runtime
