#include "katana/runtime/native_aot_template.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/runtime/block_guards.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace katana::runtime;

constexpr std::uint32_t fixed_execution_marker = 0xF17EDA07u;

BlockExit native_template_block(CpuState&, BlockExecutionContext&) {
    return {};
}

BlockExit native_handler_block(CpuState&, BlockExecutionContext&) {
    return {};
}

BlockExit fixed_execution_block(CpuState& cpu, BlockExecutionContext&) {
    cpu.r[7] = fixed_execution_marker;
    BlockExit result;
    result.kind = BlockEndKind::Return;
    return result;
}

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void write_u32(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint32_t value) {
    for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
}

} // namespace

int main() {
    try {
        constexpr std::uint32_t source = 0x80001000u;
        constexpr std::uint32_t runtime = 0x80002600u;
        constexpr std::uint32_t handler_live = 0xA0012000u;
        constexpr std::uint32_t handler_block_address = 0x80012000u;
        constexpr std::uint32_t patch_offset = 12u;
        std::vector<std::uint8_t> original{
            0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u,
            0x09u, 0x00u, 0x09u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
        auto live = original;
        write_u32(live, patch_offset, handler_live);

        CpuState cpu;
        cpu.vbr = 0x80002000u;
        const auto source_physical = canonical_physical_address(source);
        const auto runtime_physical = canonical_physical_address(runtime);
        cpu.memory.write_bytes(source_physical, original, CodeWriteSource::Copy);
        ExecutableModuleCatalog modules;
        ExecutableModule module;
        module.id = "synthetic-local-disc-module";
        module.source_identity = "sha256:free-native-template-fixture-v1";
        module.guest_start = source;
        module.bytes = original;
        modules.publish(module);
        modules.record_runtime_write(source + patch_offset,
                                     sizeof(std::uint32_t),
                                     CodeWriteSource::Cpu,
                                     true);
        cpu.memory.write_bytes(source_physical, live, CodeWriteSource::Cpu);
        cpu.memory.write_bytes(runtime_physical, live, CodeWriteSource::Copy);

        RuntimeBlockTable blocks;
        static_cast<void>(blocks.register_static({source,
                                                  canonical_physical_address(source),
                                                  2u,
                                                  BlockEndKind::Fallthrough,
                                                  {},
                                                  native_template_block,
                                                  "synthetic-template-source"}));
        static_cast<void>(blocks.register_static({handler_block_address,
                                                  canonical_physical_address(handler_block_address),
                                                  2u,
                                                  BlockEndKind::Return,
                                                  {},
                                                  native_handler_block,
                                                  "synthetic-template-handler"}));
        const std::array templates{NativeAotTemplate{
            module.id, module.source_identity, source,
            static_cast<std::uint32_t>(live.size()), 0x600,
            {{patch_offset, {{handler_live, handler_block_address}}}}}};
        NativeAotTemplateBinder binder(cpu, modules, blocks, templates);
        const auto bound = binder.bind(
            runtime, runtime_physical, live, BlockVariantKey{1u, 2u, 3u, 4u, 5u});
        require(bound && !bound.candidate.interpreter_backed &&
                    bound.candidate.bounded_analysis_complete && bound.candidate.ir_verified &&
                    bound.candidate.code_generated &&
                    bound.candidate.block.function == native_template_block &&
                    bound.candidate.block.virtual_start == runtime &&
                    bound.candidate.block.variant == BlockVariantKey{1u, 2u, 3u, 4u, 5u} &&
                    bound.candidate.block.aot_template ==
                        RuntimeAotTemplateContract{
                            {source, runtime, static_cast<std::uint32_t>(live.size())},
                            static_cast<std::uint32_t>(live.size())},
                "Bewiesene, gepatchte Runtime-Kopie wurde nicht an nativen AOT-Code gebunden.");

        cpu.memory.write_bytes(source_physical, original, CodeWriteSource::Cpu);
        const auto rebound_after_source_restore =
            binder.bind(runtime, runtime_physical, live, {});
        require(static_cast<bool>(rebound_after_source_restore),
                "Eine gueltige Zielkopie hing faelschlich vom spaeteren Live-Quellpuffer ab.");

        auto mutable_templates = templates;
        mutable_templates[0].mutable_ranges = {{8u, 4u}};
        NativeAotTemplateBinder mutable_binder(cpu, modules, blocks, mutable_templates);
        auto scratch_mutated = live;
        scratch_mutated[8u] ^= 0x5Au;
        scratch_mutated[11u] ^= 0xA5u;
        cpu.memory.write_bytes(runtime_physical, scratch_mutated, CodeWriteSource::Cpu);
        const auto mutable_bound =
            mutable_binder.bind(runtime, runtime_physical, scratch_mutated, {});
        require(mutable_bound &&
                    mutable_bound.candidate.block.aot_template ==
                        RuntimeAotTemplateContract{
                            {source, runtime, static_cast<std::uint32_t>(live.size())},
                            static_cast<std::uint32_t>(live.size()),
                            {{8u, 4u}}},
                "Bewiesene Mutation im Scratchslot wurde nicht gebunden oder nicht propagiert.");

        auto adjacent_mutated = scratch_mutated;
        adjacent_mutated[7u] ^= 0x01u;
        cpu.memory.write_bytes(runtime_physical, adjacent_mutated, CodeWriteSource::Cpu);
        const auto adjacent_rejected =
            mutable_binder.bind(runtime, runtime_physical, adjacent_mutated, {});
        require(!adjacent_rejected &&
                    adjacent_rejected.failure ==
                        NativeAotTemplateBindFailure::RuntimeBytesMismatch,
                "Mutation direkt neben dem Scratchslot wurde akzeptiert.");
        cpu.memory.write_bytes(runtime_physical, live, CodeWriteSource::Copy);

        const auto invalid_mutable_definition = [&](auto ranges) {
            auto definitions = templates;
            definitions[0].mutable_ranges = std::move(ranges);
            NativeAotTemplateBinder invalid_binder(cpu, modules, blocks, definitions);
            const auto rejected_definition =
                invalid_binder.bind(runtime, runtime_physical, live, {});
            return !rejected_definition &&
                   rejected_definition.failure ==
                       NativeAotTemplateBindFailure::InvalidDefinition;
        };
        require(
            invalid_mutable_definition(
                std::vector<NativeAotTemplateMutableRange>{{8u, 0u}}) &&
                invalid_mutable_definition(
                    std::vector<NativeAotTemplateMutableRange>{{15u, 2u}}) &&
                invalid_mutable_definition(
                    std::vector<NativeAotTemplateMutableRange>{{8u, 4u}, {4u, 4u}}) &&
                invalid_mutable_definition(
                    std::vector<NativeAotTemplateMutableRange>{{4u, 8u}, {8u, 4u}}) &&
                invalid_mutable_definition(
                    std::vector<NativeAotTemplateMutableRange>{{patch_offset, 4u}}),
            "Leere, ausserhalb liegende, unsortierte, ueberlappende oder Patchslot-"
            "ueberlappende Mutable-Range wurde akzeptiert.");

        auto executable_overlap_templates = templates;
        executable_overlap_templates[0].mutable_ranges = {{0u, 4u}};
        NativeAotTemplateBinder executable_overlap_binder(
            cpu, modules, blocks, executable_overlap_templates);
        const auto executable_overlap =
            executable_overlap_binder.bind(runtime, runtime_physical, live, {});
        require(!executable_overlap &&
                    executable_overlap.failure ==
                        NativeAotTemplateBindFailure::SourceBlockMissing,
                "Mutable-Range ueber gebundenen Sourceblockbytes wurde akzeptiert.");

        const std::array foreign_alias_templates{NativeAotTemplate{
            module.id, module.source_identity, source,
            static_cast<std::uint32_t>(live.size()), 0x600,
            {{patch_offset, {{handler_live, handler_block_address + 0x1000u}}}}}};
        NativeAotTemplateBinder foreign_alias_binder(
            cpu, modules, blocks, foreign_alias_templates);
        const auto foreign_alias =
            foreign_alias_binder.bind(runtime, runtime_physical, live, {});
        require(!foreign_alias &&
                    foreign_alias.failure == NativeAotTemplateBindFailure::InvalidDefinition,
                "Physisch fremdes Live-/Block-Aliaspaar wurde akzeptiert.");

        cpu.vbr = 0x00002000u;
        const auto translated_runtime_definition = binder.bind(0x00002600u, 0x00002600u, live, {});
        cpu.vbr = 0x80002000u;
        require(!translated_runtime_definition &&
                    translated_runtime_definition.failure ==
                        NativeAotTemplateBindFailure::InvalidDefinition,
                "Ein TLB-abhaengiger P0/P3-Templatebereich wurde ohne Seitenbeweis akzeptiert.");

        const auto wrong_runtime_origin = binder.bind(runtime, runtime_physical + 4u, live, {});
        require(!wrong_runtime_origin &&
                    wrong_runtime_origin.failure ==
                        NativeAotTemplateBindFailure::InvalidDefinition,
                "Ein falscher physischer Runtime-Ursprung wurde akzeptiert.");

        const std::array wrong_identity_templates{NativeAotTemplate{
            module.id, "sha256:wrong-local-source", source,
            static_cast<std::uint32_t>(live.size()), 0x600,
            {{patch_offset, {{handler_live, handler_block_address}}}}}};
        NativeAotTemplateBinder wrong_identity_binder(
            cpu, modules, blocks, wrong_identity_templates);
        const auto wrong_identity =
            wrong_identity_binder.bind(runtime, runtime_physical, live, {});
        require(!wrong_identity &&
                    wrong_identity.failure ==
                        NativeAotTemplateBindFailure::SourceIdentityMismatch,
                "Eine Vorlage mit falscher lokaler Quellidentitaet wurde akzeptiert.");

        RuntimeBlockTable overlapping_blocks;
        static_cast<void>(overlapping_blocks.register_static(
            {source,
             canonical_physical_address(source),
             static_cast<std::uint32_t>(live.size()),
             BlockEndKind::Fallthrough,
             {},
             native_template_block,
             "synthetic-overlapping-source"}));
        static_cast<void>(overlapping_blocks.register_static(
            {handler_block_address,
             canonical_physical_address(handler_block_address),
             2u,
             BlockEndKind::Return,
             {},
             native_handler_block,
             "synthetic-overlapping-handler"}));
        NativeAotTemplateBinder overlapping_binder(
            cpu, modules, overlapping_blocks, templates);
        const auto overlapping =
            overlapping_binder.bind(runtime, runtime_physical, live, {});
        require(!overlapping &&
                    overlapping.failure == NativeAotTemplateBindFailure::SourceBlockMissing,
                "Ein Quellblock, der einen Patchslot ueberlappt, wurde gebunden.");

        auto corrupt = live;
        corrupt[2] ^= 0x01u;
        cpu.memory.write_bytes(runtime_physical, corrupt, CodeWriteSource::Cpu);
        const auto corrupted = binder.bind(runtime, runtime_physical, corrupt, {});
        require(!corrupted &&
                    corrupted.failure == NativeAotTemplateBindFailure::RuntimeBytesMismatch,
                "Manipulierte Instruktionsbytes wurden als native Vorlage akzeptiert.");

        cpu.memory.write_bytes(runtime_physical, live, CodeWriteSource::Copy);
        auto rejected_patch = live;
        write_u32(rejected_patch, patch_offset, 0xA0013000u);
        cpu.memory.write_bytes(source_physical, rejected_patch, CodeWriteSource::Cpu);
        cpu.memory.write_bytes(runtime_physical, rejected_patch, CodeWriteSource::Copy);
        const auto rejected = binder.bind(runtime, runtime_physical, rejected_patch, {});
        require(!rejected &&
                    rejected.failure == NativeAotTemplateBindFailure::PatchTargetRejected,
                "Unbewiesener Patchzeiger wurde als native Vorlage akzeptiert.");

        constexpr std::uint32_t latent_source = 0x88001000u;
        constexpr std::uint32_t latent_runtime = 0x80003000u;
        constexpr std::uint32_t latent_offset = 4u;
        const std::vector<std::uint8_t> latent_bytes{
            0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u,
            0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u};
        const auto latent_byte_identity =
            "sha256:" + katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(latent_bytes.data()),
                            latent_bytes.size()));
        ExecutableModuleCatalog latent_modules;
        ExecutableModule loaded_module;
        loaded_module.id = "disc-load-module";
        loaded_module.source_identity = "disc-load-v1:free-provenance";
        loaded_module.content_identity = "sha256:free-content-v1";
        loaded_module.byte_identity = latent_byte_identity;
        loaded_module.guest_start = canonical_physical_address(latent_runtime);
        loaded_module.bytes = latent_bytes;
        latent_modules.publish(loaded_module);
        cpu.memory.write_bytes(canonical_physical_address(latent_runtime),
                               latent_bytes,
                               CodeWriteSource::Copy);
        RuntimeBlockTable latent_blocks;
        static_cast<void>(latent_blocks.register_static(
            {latent_source + latent_offset,
             canonical_physical_address(latent_source + latent_offset),
             2u,
             BlockEndKind::Return,
             {},
             native_template_block,
             "synthetic-latent-aot-source"}));
        const std::array latent_templates{NativeAotTemplate{
            "latent-source-module",
            latent_byte_identity,
            latent_source,
            static_cast<std::uint32_t>(latent_bytes.size()),
            0,
            {},
            NativeAotTemplateDestination::LoadedModule,
            loaded_module.content_identity,
            latent_byte_identity}};
        NativeAotTemplateBinder latent_binder(
            cpu, latent_modules, latent_blocks, latent_templates);
        const auto latent_bound = latent_binder.bind(
            latent_runtime + latent_offset,
            canonical_physical_address(latent_runtime + latent_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(latent_offset),
            BlockVariantKey{9u, 8u, 7u, 6u, 5u});
        require(latent_modules.find("latent-source-module") == nullptr && latent_bound &&
                    !latent_bound.candidate.interpreter_backed &&
                    latent_bound.candidate.block.function == native_template_block &&
                    latent_bound.candidate.block.aot_template ==
                        RuntimeAotTemplateContract{
                            {latent_source,
                             latent_runtime,
                             static_cast<std::uint32_t>(latent_bytes.size())},
                            static_cast<std::uint32_t>(latent_bytes.size())},
                "Byteidentisches geladenes Discmodul wurde nicht an latentes AOT gebunden.");

        auto mutable_latent_templates = latent_templates;
        mutable_latent_templates[0].mutable_ranges = {{8u, 4u}};
        NativeAotTemplateBinder mutable_latent_binder(
            cpu, latent_modules, latent_blocks, mutable_latent_templates);
        const auto mutable_latent = mutable_latent_binder.bind(
            latent_runtime + latent_offset,
            canonical_physical_address(latent_runtime + latent_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(latent_offset),
            {});
        require(!mutable_latent &&
                    mutable_latent.failure ==
                        NativeAotTemplateBindFailure::InvalidDefinition,
                "LoadedModule-Template akzeptierte VBR-spezifische Mutable-Ranges.");

        auto mismatched_template_identity = latent_templates;
        mismatched_template_identity[0].expected_source_identity =
            "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
        NativeAotTemplateBinder mismatched_template_binder(
            cpu, latent_modules, latent_blocks, mismatched_template_identity);
        const auto mismatched_template = mismatched_template_binder.bind(
            latent_runtime + latent_offset,
            canonical_physical_address(latent_runtime + latent_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(latent_offset),
            {});
        require(!mismatched_template &&
                    mismatched_template.failure ==
                        NativeAotTemplateBindFailure::InvalidDefinition,
                "Latentes AOT akzeptierte einen vom Runtimehash geloesten Templatevertrag.");

        auto mutated_loaded_bytes = latent_bytes;
        mutated_loaded_bytes[latent_offset] ^= 0x01u;
        cpu.memory.write_bytes(canonical_physical_address(latent_runtime),
                               mutated_loaded_bytes,
                               CodeWriteSource::Cpu);
        const auto mutated_loaded = latent_binder.bind(
            latent_runtime + latent_offset,
            canonical_physical_address(latent_runtime + latent_offset),
            std::span<const std::uint8_t>(mutated_loaded_bytes).subspan(latent_offset),
            {});
        require(!mutated_loaded &&
                    mutated_loaded.failure ==
                        NativeAotTemplateBindFailure::RuntimeBytesMismatch,
                "Nach dem Load veraenderte Runtimebytes wurden trotz Metadatenbindung akzeptiert.");
        cpu.memory.write_bytes(canonical_physical_address(latent_runtime),
                               latent_bytes,
                               CodeWriteSource::Copy);

        constexpr std::uint32_t latent_tlb_virtual = 0x00103000u;
        cpu.write_sr(sr_md_mask);
        cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        cpu.address_space->set_mode(AddressTranslationMode::Mmu);
        cpu.address_space->write_mmucr(1u);
        cpu.address_space->ldtlb({latent_tlb_virtual,
                                  canonical_physical_address(latent_runtime),
                                  4096u,
                                  0u,
                                  0u,
                                  true,
                                  true,
                                  true,
                                  true,
                                  true,
                                  true,
                                  false});
        const auto address_space_before_tlb_bind = cpu.address_space->snapshot();
        const auto latent_tlb_bound = latent_binder.bind(
            latent_tlb_virtual + latent_offset,
            canonical_physical_address(latent_runtime + latent_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(latent_offset),
            BlockVariantKey{10u, 9u, 8u, 7u, 6u});
        require(latent_tlb_bound &&
                    latent_tlb_bound.candidate.block.aot_template ==
                        RuntimeAotTemplateContract{
                            {latent_source,
                             latent_tlb_virtual,
                             static_cast<std::uint32_t>(latent_bytes.size())},
                            static_cast<std::uint32_t>(latent_bytes.size())} &&
                    cpu.address_space->snapshot() == address_space_before_tlb_bind,
                "Lineares aktives P0-TLB-Mapping wurde nicht beobachtend an latentes AOT "
                "gebunden.");
        cpu.address_space.reset();

        auto wrong_content_definition = latent_templates;
        wrong_content_definition[0].expected_runtime_content_identity =
            "sha256:other-content";
        NativeAotTemplateBinder wrong_content_binder(
            cpu, latent_modules, latent_blocks, wrong_content_definition);
        const auto wrong_content = wrong_content_binder.bind(
            latent_runtime + latent_offset,
            canonical_physical_address(latent_runtime + latent_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(latent_offset),
            {});
        require(!wrong_content &&
                    wrong_content.failure ==
                        NativeAotTemplateBindFailure::RuntimeContentIdentityMismatch,
                "Latentes AOT wurde ohne exakte Disc-Contentidentitaet aktiviert.");

        auto unknown_bytes_definition = latent_templates;
        unknown_bytes_definition[0].expected_source_identity =
            "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
        unknown_bytes_definition[0].expected_runtime_byte_identity =
            unknown_bytes_definition[0].expected_source_identity;
        NativeAotTemplateBinder unknown_bytes_binder(
            cpu, latent_modules, latent_blocks, unknown_bytes_definition);
        const auto unknown_bytes = unknown_bytes_binder.bind(
            latent_runtime + latent_offset,
            canonical_physical_address(latent_runtime + latent_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(latent_offset),
            {});
        require(!unknown_bytes &&
                    unknown_bytes.failure ==
                        NativeAotTemplateBindFailure::RuntimeBytesMismatch &&
                    unknown_bytes.candidate.rejection_failure ==
                        MaterializationFailure::ByteIdentityMismatch,
                "Vollstaendige Discdatei mit falscher erwarteter Byteidentitaet wurde nicht "
                "als Byte-Identity-Mismatch abgelehnt.");

        ExecutableModuleCatalog holey_modules;
        auto holey_loaded_module = loaded_module;
        holey_loaded_module.id = "holey-disc-load-module";
        holey_loaded_module.active_extents = {{0u, 8u}, {12u, 4u}};
        holey_modules.publish(std::move(holey_loaded_module));
        NativeAotTemplateBinder holey_binder(
            cpu, holey_modules, latent_blocks, latent_templates);
        const auto holey = holey_binder.bind(
            latent_runtime + latent_offset,
            canonical_physical_address(latent_runtime + latent_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(latent_offset),
            {});
        require(!holey && holey.failure == NativeAotTemplateBindFailure::MissingAot,
                "Holey/partiell aktives Discmodul wurde als volle AOT-Vorlage akzeptiert.");

        const auto missing_block = latent_binder.bind(
            latent_runtime + 8u,
            canonical_physical_address(latent_runtime + 8u),
            std::span<const std::uint8_t>(latent_bytes).subspan(8u),
            {});
        require(!missing_block &&
                    missing_block.failure == NativeAotTemplateBindFailure::MissingAot,
                "Nicht vorab kompilierter Moduleinstieg erzeugte keinen Missing-AOT-Fehler.");

        constexpr std::uint32_t fixed_source = 0x88002000u;
        constexpr std::uint32_t fixed_runtime = 0x80004000u;
        constexpr std::uint32_t fixed_offset = 4u;
        cpu.memory.write_bytes(canonical_physical_address(fixed_runtime),
                               latent_bytes,
                               CodeWriteSource::Copy);
        RuntimeBlockTable fixed_blocks;
        static_cast<void>(fixed_blocks.register_static(
            {fixed_source + fixed_offset,
             canonical_physical_address(fixed_source + fixed_offset),
             2u,
             BlockEndKind::Return,
             {},
             native_template_block,
             "synthetic-fixed-aot-source"}));
        const auto fixed_block_identity =
            "sha256:" + katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(
                                latent_bytes.data() + fixed_offset),
                            2u));
        const std::array fixed_templates{NativeAotTemplate{
            {},
            {},
            fixed_source,
            static_cast<std::uint32_t>(latent_bytes.size()),
            0,
            {},
            NativeAotTemplateDestination::FixedAddress,
            {},
            {},
            {},
            fixed_runtime,
            {{fixed_offset, 2u, fixed_block_identity}},
            NativeAotTemplateValidationMode::RuntimeBlock}};
        RuntimeBlockTable fixed_dispatch_blocks;
        fixed_dispatch_blocks.seal_static();
        NativeAotTemplateBinder fixed_binder(
            cpu,
            latent_modules,
            fixed_dispatch_blocks,
            fixed_templates,
            &fixed_blocks);
        const auto fixed_probe =
            fixed_binder.fixed_block_materialization_probe(
                fixed_runtime + fixed_offset,
                canonical_physical_address(
                    fixed_runtime + fixed_offset));
        const auto fixed_hole_probe =
            fixed_binder.fixed_block_materialization_probe(
                fixed_runtime,
                canonical_physical_address(fixed_runtime));
        require(
            fixed_probe.kind ==
                    BlockMaterializationProbeKind::IdentityBound &&
                fixed_probe.required_bytes == 2u &&
                fixed_hole_probe.kind ==
                    BlockMaterializationProbeKind::Rejected &&
                fixed_hole_probe.rejection_failure ==
                    MaterializationFailure::MissingAot,
            "FixedAddress-Preflight verlor die exakte Blockgroesse "
            "oder liess ein ungebundenes Ziel dynamisch durch.");
        const auto fixed_bound = fixed_binder.bind(
            fixed_runtime + fixed_offset,
            canonical_physical_address(fixed_runtime + fixed_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(fixed_offset),
            BlockVariantKey{11u, 12u, 13u, 14u, 15u});
        require(fixed_bound &&
                    fixed_bound.candidate.block.function == native_template_block &&
                    fixed_bound.candidate.block.physical_origin ==
                        canonical_physical_address(fixed_runtime + fixed_offset) &&
                    fixed_bound.candidate.block.aot_template ==
                        RuntimeAotTemplateContract{
                            {fixed_source,
                             fixed_runtime,
                             static_cast<std::uint32_t>(latent_bytes.size())},
                            2u,
                            {},
                            NativeAotTemplateValidationMode::RuntimeBlock},
                "FixedAddress-AOT wurde nicht ohne Modul-/Contentlineage an den exakt "
                "gehashten Runtimeblock gebunden.");
        require(!fixed_dispatch_blocks.lookup(
                    fixed_source + fixed_offset, {}).has_value(),
                "FixedAddress-Quellblock wurde in die Gastdispatch-Tabelle "
                "veroeffentlicht.");
        RuntimeBlockTable fixed_fallback_blocks;
        static_cast<void>(fixed_fallback_blocks.register_static(
            {fixed_source + fixed_offset,
             canonical_physical_address(fixed_source + fixed_offset),
             2u,
             BlockEndKind::Return,
             {},
             native_template_block,
             "synthetic-fixed-fallback-source"}));
        NativeAotTemplateBinder fixed_without_source_catalog(
            cpu,
            latent_modules,
            fixed_fallback_blocks,
            fixed_templates);
        const auto fixed_without_source = fixed_without_source_catalog.bind(
            fixed_runtime + fixed_offset,
            canonical_physical_address(fixed_runtime + fixed_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(fixed_offset),
            {});
        require(
            !fixed_without_source &&
                fixed_without_source.failure ==
                    NativeAotTemplateBindFailure::MissingAot,
            "FixedAddress-AOT fiel ohne separaten Quellkatalog auf die "
            "Gastdispatch-Tabelle zurueck.");

        constexpr std::uint32_t wide_fixed_source =
            0x88004000u;
        constexpr std::uint32_t wide_fixed_runtime =
            0x80006000u;
        constexpr std::uint32_t wide_fixed_size = 132u;
        std::vector<std::uint8_t> wide_fixed_bytes(
            wide_fixed_size, 0u);
        for (std::uint32_t offset = 0u;
             offset < wide_fixed_size;
             offset += 2u)
            wide_fixed_bytes[offset] = 0x09u;
        wide_fixed_bytes[wide_fixed_size - 4u] = 0x0Bu;
        const auto wide_fixed_physical =
            canonical_physical_address(wide_fixed_runtime);
        cpu.memory.write_bytes(wide_fixed_physical,
                               wide_fixed_bytes,
                               CodeWriteSource::Cpu);
        ExecutableModuleCatalog wide_fixed_modules;
        wide_fixed_modules.record_runtime_write(
            wide_fixed_physical,
            wide_fixed_bytes.size(),
            CodeWriteSource::Cpu,
            true);
        RuntimeBlockTable wide_fixed_source_blocks;
        static_cast<void>(wide_fixed_source_blocks.register_static(
            {wide_fixed_source,
             canonical_physical_address(wide_fixed_source),
             wide_fixed_size,
             BlockEndKind::Return,
             {},
             fixed_execution_block,
             "synthetic-wide-fixed-aot-source"}));
        RuntimeBlockTable wide_fixed_dispatch_blocks;
        const auto wide_fixed_identity =
            "sha256:" + katana::io::sha256_bytes(
                            std::string_view(
                                reinterpret_cast<const char*>(
                                    wide_fixed_bytes.data()),
                                wide_fixed_bytes.size()));
        const std::array wide_fixed_templates{NativeAotTemplate{
            {},
            {},
            wide_fixed_source,
            wide_fixed_size,
            0,
            {},
            NativeAotTemplateDestination::FixedAddress,
            {},
            {},
            {},
            wide_fixed_runtime,
            {{0u, wide_fixed_size, wide_fixed_identity}},
            NativeAotTemplateValidationMode::RuntimeBlock}};
        NativeAotTemplateBinder wide_fixed_binder(
            cpu,
            wide_fixed_modules,
            wide_fixed_dispatch_blocks,
            wide_fixed_templates,
            &wide_fixed_source_blocks);
        BlockMaterializationPolicy wide_fixed_policy;
        wide_fixed_policy.enabled = true;
        wide_fixed_policy.max_blocks = 4u;
        wide_fixed_policy.max_bytes = 512u;
        DemandBlockMaterializer wide_fixed_materializer(
            wide_fixed_modules,
            wide_fixed_dispatch_blocks,
            nullptr,
            wide_fixed_policy,
            [&wide_fixed_binder](
                const std::uint32_t target,
                const std::uint32_t physical_origin,
                const std::span<const std::uint8_t> bytes,
                const BlockVariantKey& variant) {
                return std::move(
                    wide_fixed_binder
                        .bind(target,
                              physical_origin,
                              bytes,
                              variant)
                        .candidate);
            },
            [&wide_fixed_binder](
                const std::uint32_t target,
                const std::uint32_t physical_origin,
                const BlockVariantKey&) {
                return wide_fixed_binder
                    .fixed_block_materialization_probe(
                        target, physical_origin);
            });
        auto wide_fixed_corrupt = wide_fixed_bytes;
        wide_fixed_corrupt.front() ^= 0x01u;
        cpu.memory.write_bytes(wide_fixed_physical,
                               wide_fixed_corrupt,
                               CodeWriteSource::Cpu);
        wide_fixed_modules.record_runtime_write(
            wide_fixed_physical,
            wide_fixed_corrupt.size(),
            CodeWriteSource::Cpu,
            true);
        const auto wide_fixed_rejected =
            wide_fixed_materializer.try_materialize(
                cpu,
                wide_fixed_runtime,
                wide_fixed_physical,
                {},
                0x8000000Eu);
        require(
            !wide_fixed_rejected.has_value() &&
                wide_fixed_modules.resolve(
                    wide_fixed_physical, 2u) == nullptr,
            "FixedAddress-Hashfehler veraenderte den "
            "Runtimewrite-Modulkatalog vor der Ablehnung.");
        cpu.memory.write_bytes(wide_fixed_physical,
                               wide_fixed_bytes,
                               CodeWriteSource::Cpu);
        wide_fixed_modules.record_runtime_write(
            wide_fixed_physical,
            wide_fixed_bytes.size(),
            CodeWriteSource::Cpu,
            true);
        const auto wide_fixed_handle =
            wide_fixed_materializer.try_materialize(
                cpu,
                wide_fixed_runtime,
                wide_fixed_physical,
                {},
                0x80000010u);
        const auto wide_fixed_module =
            wide_fixed_modules.resolve(
                wide_fixed_physical, wide_fixed_size);
        require(
            wide_fixed_handle.has_value() &&
                wide_fixed_module != nullptr &&
                wide_fixed_module->bytes.size() ==
                    wide_fixed_size &&
                wide_fixed_dispatch_blocks.active(
                    *wide_fixed_handle) &&
                wide_fixed_materializer.metrics()
                        .materialized_bytes ==
                    wide_fixed_size,
            "Exakt gehashter FixedAddress-AOT-Block ueber 128 "
            "Byte wurde nicht zielgenau materialisiert.");
        const auto wide_fixed_resolved =
            wide_fixed_dispatch_blocks.resolve(*wide_fixed_handle);
        require(
            wide_fixed_resolved.has_value() &&
                wide_fixed_resolved->get().function ==
                    fixed_execution_block,
            "Materialisierter FixedAddress-Block besitzt keinen "
            "ausfuehrbaren nativen Funktionszeiger.");
        cpu.r[7] = 0u;
        BlockExecutionContext wide_fixed_context{};
        const auto wide_fixed_exit =
            wide_fixed_resolved->get().function(
                cpu, wide_fixed_context);
        require(
            cpu.r[7] == fixed_execution_marker &&
                wide_fixed_exit.kind == BlockEndKind::Return,
            "Materialisierter FixedAddress-Block wurde nicht ueber "
            "seinen aufgeloesten nativen Funktionszeiger ausgefuehrt.");

        auto fixed_mutated = latent_bytes;
        fixed_mutated[fixed_offset] ^= 0x01u;
        cpu.memory.write_bytes(canonical_physical_address(fixed_runtime),
                               fixed_mutated,
                               CodeWriteSource::Cpu);
        const auto fixed_hash_rejected = fixed_binder.bind(
            fixed_runtime + fixed_offset,
            canonical_physical_address(fixed_runtime + fixed_offset),
            std::span<const std::uint8_t>(fixed_mutated).subspan(fixed_offset),
            {});
        require(!fixed_hash_rejected &&
                    fixed_hash_rejected.failure ==
                        NativeAotTemplateBindFailure::RuntimeBytesMismatch,
                "FixedAddress-AOT akzeptierte Runtimeblockbytes mit falschem SHA-256.");
        cpu.memory.write_bytes(canonical_physical_address(fixed_runtime),
                               latent_bytes,
                               CodeWriteSource::Copy);

        constexpr std::uint32_t anonymous_runtime = 0x80005000u;
        ExecutableModuleCatalog anonymous_modules;
        ExecutableModule anonymous_module;
        anonymous_module.id = "guest-runtime-write-fixed-contract-fixture";
        anonymous_module.source_identity = "guest-runtime-write-v1";
        anonymous_module.guest_start =
            canonical_physical_address(anonymous_runtime);
        anonymous_module.bytes = latent_bytes;
        anonymous_modules.publish(std::move(anonymous_module));
        cpu.memory.write_bytes(canonical_physical_address(anonymous_runtime),
                               latent_bytes,
                               CodeWriteSource::Copy);
        NativeAotTemplateBinder anonymous_binder(
            cpu, anonymous_modules, latent_blocks, latent_templates);
        const auto anonymous_without_fixed = anonymous_binder.bind(
            anonymous_runtime + latent_offset,
            canonical_physical_address(anonymous_runtime + latent_offset),
            std::span<const std::uint8_t>(latent_bytes).subspan(latent_offset),
            {});
        require(!anonymous_without_fixed &&
                    anonymous_without_fixed.failure ==
                        NativeAotTemplateBindFailure::NoMatchingDestination &&
                    anonymous_without_fixed.candidate.rejection_failure ==
                        MaterializationFailure::MissingAot,
                "Anonymer Runtimewrite ohne FixedAddress-Vertrag wurde als Content-/"
                "Bytemismatch statt Missing-AOT klassifiziert.");

        std::cout << "Native AOT-Templatebindung erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
