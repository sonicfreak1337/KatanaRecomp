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

BlockExit native_template_block(CpuState&, BlockExecutionContext&) {
    return {};
}

BlockExit native_handler_block(CpuState&, BlockExecutionContext&) {
    return {};
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
        ExecutableModule latent_source_module;
        latent_source_module.id = "latent-source-module";
        latent_source_module.source_identity = latent_byte_identity;
        latent_source_module.guest_start = latent_source;
        latent_source_module.bytes = latent_bytes;
        latent_source_module.writable = false;
        latent_modules.publish(latent_source_module);
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
            latent_source_module.id,
            latent_source_module.source_identity,
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
        require(latent_bound && latent_bound.candidate.block.function == native_template_block &&
                    latent_bound.candidate.block.aot_template ==
                        RuntimeAotTemplateContract{
                            {latent_source,
                             latent_runtime,
                             static_cast<std::uint32_t>(latent_bytes.size())},
                            static_cast<std::uint32_t>(latent_bytes.size())},
                "Byteidentisches geladenes Discmodul wurde nicht an latentes AOT gebunden.");

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
                            static_cast<std::uint32_t>(latent_bytes.size())},
                "Lineares aktives P0-TLB-Mapping wurde nicht an latentes AOT gebunden.");
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
        unknown_bytes_definition[0].expected_runtime_byte_identity =
            "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
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
        holey_modules.publish(latent_source_module);
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

        std::cout << "Native AOT-Templatebindung erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
