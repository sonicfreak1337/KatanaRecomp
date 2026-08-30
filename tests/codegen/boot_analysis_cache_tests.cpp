#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/hardware_audit.hpp"
#include "katana/codegen/boot_analysis_cache.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t image_base = 0x8C010000u;

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

katana::io::ExecutableImage make_image() {
    katana::io::ExecutableImage image("boot-analysis-cache-test.bin");
    katana::io::ImageSegment segment;
    segment.name = "test-code";
    segment.virtual_address = image_base;
    segment.file_offset = 0u;
    segment.memory_size = 4u;
    segment.kind = katana::io::SegmentKind::Code;
    segment.permissions = {true, true, true};
    // jmp @r0; nop
    segment.bytes = {0x2Bu, 0x40u, 0x09u, 0x00u};
    segment.source_kind = katana::io::ImageSourceKind::RawBinary;
    segment.local_source_name = "boot-analysis-cache-test.bin";
    image.add_segment(std::move(segment));
    image.add_entry_point(image_base);
    return image;
}

katana::codegen::PreparedBootAnalysisArtifact make_artifact(
    const katana::io::ExecutableImage& image) {
    katana::analysis::ControlFlowAnalysisResult analysis;
    analysis.recursive.instructions = katana::sh4::disassemble(image);
    require(analysis.recursive.instructions.size() == 2u,
            "Testbild wurde nicht vollstaendig disassembliert.");
    analysis.recursive.functions.push_back(
        {image_base,
         katana::analysis::AnalysisConfidence::Certain,
         katana::analysis::ControlFlowEvidence::ProvenComplete,
         {katana::analysis::FunctionOrigin::EntryPoint},
         4u});

    katana::analysis::IndirectControlFlowResolution indirect;
    indirect.instruction_address = image_base;
    indirect.kind = katana::analysis::IndirectControlFlowKind::Jump;
    indirect.register_index = 0u;
    indirect.status = katana::analysis::ResolutionStatus::Unresolved;
    indirect.evidence = katana::analysis::ControlFlowEvidence::RuntimeOnly;
    indirect.origin_class =
        katana::analysis::IndirectControlFlowOriginClass::RuntimePointer;
    indirect.evidence_origins = {
        katana::analysis::AnalysisEvidenceOrigin::RuntimeClassification};
    indirect.reason = "runtime-register";
    indirect.value_source = "r0";
    indirect.instruction_kind =
        analysis.recursive.instructions.front().instruction.kind;
    analysis.indirect_control_flow.push_back(std::move(indirect));

    katana::analysis::GuardedAotEntry guarded;
    guarded.guest_address = image_base;
    guarded.shared_body_address = image_base;
    guarded.evidence =
        katana::analysis::ControlFlowEvidence::GuardedPartial;
    guarded.origins = {
        katana::analysis::GuardedAotEntryOrigin::StoredCodeAddress};
    guarded.source_sites = {image_base};
    const auto& bytes = image.segments().front().bytes;
    guarded.source_identity =
        "sha256:" +
        katana::io::sha256_bytes(std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    guarded.source_byte_offset = 0u;
    guarded.entry_byte_extent = 4u;
    guarded.entry_byte_identity = guarded.source_identity;
    analysis.guarded_aot_entries.push_back(std::move(guarded));
    analysis.guarded_code_inventory_candidates = 1u;
    analysis.guarded_code_inventory_budget = 1'024u;

    katana::analysis::JumpTableAnalysis cached_table;
    cached_table.dispatch_address = image_base;
    cached_table.table_address = image_base;
    cached_table.requested_entries = 1u;
    cached_table.resolved = true;
    cached_table.aot_candidates_only = true;
    cached_table.evidence =
        katana::analysis::ControlFlowEvidence::GuardedPartial;
    cached_table.authority =
        katana::analysis::JumpTableAuthority::SnapshotCandidate;
    cached_table.entries.push_back(
        {0u, image_base, image_base, true, "synthetic-codec-target"});
    cached_table.reason = "synthetic-codec-authority";
    analysis.jump_tables.push_back(std::move(cached_table));

    auto program = katana::ir::lower_program(analysis);
    katana::codegen::PreparedBootAnalysisArtifact artifact;
    artifact.control_flow_graph =
        katana::analysis::build_control_flow_graph(analysis);
    artifact.call_graph =
        katana::analysis::build_call_graph(analysis);
    artifact.hardware_loops =
        katana::analysis::audit_dreamcast_hardware(image, analysis).loops;
    artifact.analysis = std::move(analysis);
    artifact.lowered_program = std::move(program);
    require(katana::codegen::boot_analysis_artifact_cacheable(artifact),
            "Synthetisches Boot-Analysecache-Artefakt ist nicht cachebar.");
    return artifact;
}

} // namespace

int main() {
    try {
        const auto image = make_image();
        auto artifact = make_artifact(image);
        const auto key = katana::codegen::make_boot_analysis_cache_key(
            image, nullptr, "test-contract", std::string(64u, 'a'));
        katana::analysis::AnalysisOverrides external_entry_overrides;
        external_entry_overrides.external_entry_hints.push_back(
            {image_base + 2u, 0u});
        const auto external_entry_key =
            katana::codegen::make_boot_analysis_cache_key(
                image,
                &external_entry_overrides,
                "test-contract",
                std::string(64u, 'a'));
        require(
            external_entry_key != key,
            "Ein nicht-rootender externer Entry fehlte in der "
            "Bootcache-Quellbindung.");
        auto additional_root_image = image;
        additional_root_image.add_entry_point(image_base + 2u);
        const auto additional_root_key =
            katana::codegen::make_boot_analysis_cache_key(
                additional_root_image,
                nullptr,
                "test-contract",
                std::string(64u, 'a'));
        require(
            additional_root_key != key,
            "Eine Function-/StaticEntry-Rooterweiterung fehlte in der "
            "Bootcache-Quellbindung.");
        auto generation_bound_image = image;
        generation_bound_image.add_immutable_range(
            {image_base,
             4u,
             "sha256:" +
                 katana::io::sha256_bytes(std::string_view(
                     reinterpret_cast<const char*>(
                         image.segments().front().bytes.data()),
                     image.segments().front().bytes.size())),
             0u});
        const auto generation_bound_key =
            katana::codegen::make_boot_analysis_cache_key(
                generation_bound_image,
                nullptr,
                "test-contract",
                std::string(64u, 'a'));
        require(
            generation_bound_key != key,
            "Eine identity-/generationgebundene Moduleinstanz fehlte in "
            "der Bootcache-Quellbindung.");
        auto rebound_generation_image = generation_bound_image;
        rebound_generation_image.write_u32_le(
            image_base,
            rebound_generation_image.read_u32_le(image_base));
        rebound_generation_image.add_immutable_range(
            {image_base,
             4u,
             generation_bound_image.immutable_ranges().front().identity,
             0u});
        const auto rebound_generation_key =
            katana::codegen::make_boot_analysis_cache_key(
                rebound_generation_image,
                nullptr,
                "test-contract",
                std::string(64u, 'a'));
        require(
            rebound_generation_key != generation_bound_key,
            "Eine neue Immutable-Range-Generation erbte den Bootcache der "
            "vorherigen Proofepoche.");

        artifact.analysis.guarded_code_inventory_walk
            .forwarded_store_evaluation_cache_hits = 7u;
        artifact.analysis.guarded_code_inventory_walk
            .forwarded_store_evaluation_cache_misses = 11u;
        const auto canonical =
            katana::codegen::serialize_boot_analysis_cache(key, artifact);
        artifact.analysis.guarded_code_inventory_walk
            .forwarded_store_evaluation_cache_hits = 701u;
        artifact.analysis.guarded_code_inventory_walk
            .forwarded_store_evaluation_cache_misses = 1'101u;
        require(
            katana::codegen::serialize_boot_analysis_cache(key, artifact) ==
                canonical,
            "Scheduling-lokale Inventory-Cachezaehler veraendern den "
            "kanonischen Bootcache.");
        {
            auto canonical_parsed =
                katana::codegen::parse_boot_analysis_cache(
                    key, canonical);
            require(
                canonical_parsed.state ==
                        katana::codegen::BootAnalysisCacheState::Hit &&
                    canonical_parsed.artifact.analysis.jump_tables.size() ==
                        1u &&
                    canonical_parsed.artifact.analysis.jump_tables.front()
                            .authority ==
                        katana::analysis::JumpTableAuthority::
                            SnapshotCandidate &&
                    !canonical_parsed.artifact.analysis
                         .guarded_code_inventory_walk
                         .inventory_tail_target_unresolved &&
                    !katana::codegen::
                         validate_boot_analysis_cache_source_binding(
                             canonical_parsed.artifact, image),
                "Das vollstaendige Tail-Target-Diagnosebit rundete nicht "
                "durch den Bootcache oder ein positiver Bootcache wurde "
                "trotz fehlendem Vollstaendigkeitsbeweis als Produktinput "
                "zugelassen.");
        }
        auto unresolved_tail = artifact;
        unresolved_tail.analysis.guarded_code_inventory_walk
            .inventory_tail_target_unresolved = true;
        require(
            !katana::codegen::boot_analysis_artifact_cacheable(
                unresolved_tail),
            "Ein ungebundenes Inventory-Tail-Target wurde als "
            "vollstaendiges Bootcache-Artefakt zugelassen.");

        auto previous_schema = canonical;
        constexpr std::size_t schema_offset = 8u;
        const auto stale_schema =
            katana::codegen::boot_analysis_cache_schema_version - 1u;
        require(
            previous_schema.size() >= schema_offset + 4u,
            "Bootcache-Testartefakt besitzt keinen vollstaendigen Header.");
        for (std::size_t index = 0u; index < 4u; ++index) {
            previous_schema[schema_offset + index] =
                static_cast<std::uint8_t>(
                    stale_schema >> (index * 8u));
        }
        require(
            katana::codegen::parse_boot_analysis_cache(
                key, previous_schema)
                    .state ==
                katana::codegen::BootAnalysisCacheState::Miss,
            "Ein Bootcache des vorherigen Schemas wurde nicht als Miss "
            "behandelt.");

        const auto checkpoint =
            katana::codegen::serialize_boot_analysis_checkpoint(
                key, artifact);
        auto previous_checkpoint_schema = checkpoint;
        const auto stale_checkpoint_schema =
            katana::codegen::boot_analysis_checkpoint_schema_version - 1u;
        require(
            previous_checkpoint_schema.size() >= schema_offset + 4u,
            "Bootcheckpoint-Testartefakt besitzt keinen vollstaendigen "
            "Header.");
        for (std::size_t index = 0u; index < 4u; ++index) {
            previous_checkpoint_schema[schema_offset + index] =
                static_cast<std::uint8_t>(
                    stale_checkpoint_schema >> (index * 8u));
        }
        require(
            katana::codegen::parse_boot_analysis_checkpoint(
                key, previous_checkpoint_schema)
                    .state ==
                katana::codegen::BootAnalysisCacheState::Miss,
            "Ein Bootcheckpoint des vorherigen Schemas wurde nicht als "
            "Miss behandelt.");

        // Simulate a checksum-consistent subset forgery. All retained
        // instructions still match the current bytes, so byte rebinding alone
        // cannot prove that the omitted callback/function never existed.
        auto subset = artifact;
        subset.lowered_program.front().blocks.front().instructions.pop_back();
        const auto hostile =
            katana::codegen::serialize_boot_analysis_cache(key, subset);
        auto parsed =
            katana::codegen::parse_boot_analysis_cache(key, hostile);
        require(parsed.state ==
                    katana::codegen::BootAnalysisCacheState::Hit,
                "Checksum-konsistentes Testartefakt wurde nicht geparst.");
        require(
            !katana::codegen::validate_boot_analysis_cache_source_binding(
                parsed.artifact, image),
            "Checksum-konsistenter Bootcache-Subset-Forge wurde nicht "
            "fail-closed abgelehnt.");

        auto wrong_image = image;
        wrong_image.write_u32_le(image_base, 0x00090009u);
        const auto wrong_key =
            katana::codegen::make_boot_analysis_cache_key(
                wrong_image,
                nullptr,
                "test-contract",
                std::string(64u, 'a'));
        require(
            wrong_key != key &&
                katana::codegen::parse_boot_analysis_cache(
                    wrong_key, hostile)
                        .state ==
                    katana::codegen::BootAnalysisCacheState::Miss,
            "Cache-Key blieb an veraenderte Quellbytes gebunden.");
        require(
            katana::codegen::parse_boot_analysis_cache(
                additional_root_key, canonical)
                    .state ==
                katana::codegen::BootAnalysisCacheState::Miss &&
                katana::codegen::parse_boot_analysis_cache(
                    generation_bound_key, canonical)
                    .state ==
                katana::codegen::BootAnalysisCacheState::Miss &&
                katana::codegen::parse_boot_analysis_cache(
                    rebound_generation_key, canonical)
                    .state ==
                katana::codegen::BootAnalysisCacheState::Miss,
            "Ein alter Bootcache ueberlebte ein neues Entryuniversum oder "
            "eine neue identity-/generationgebundene Moduleinstanz.");

        std::cout
            << "Boot-Analysecache-Rebinding-Regressions bestanden.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
