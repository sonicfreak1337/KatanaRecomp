#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/function_value_analysis.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

katana::io::ExecutableImage image_with_callee(const std::vector<std::uint8_t>& callee) {
    std::vector<std::uint8_t> bytes(128u, 0x09u);
    const std::vector<std::uint8_t> main{
        0x0Eu,
        0xB0u, // bsr 0x20
        0x09u,
        0x00u, // nop (delay)
        0x2Bu,
        0x40u, // jmp @r0
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(main.begin(), main.end(), bytes.begin());
    std::copy(callee.begin(), callee.end(), bytes.begin() + 0x20u);
    bytes[0x10u] = 0x0Bu;
    bytes[0x11u] = 0x00u;
    bytes[0x12u] = 0x09u;
    bytes[0x13u] = 0x00u;
    bytes[0x14u] = 0x0Bu;
    bytes[0x15u] = 0x00u;
    bytes[0x16u] = 0x09u;
    bytes[0x17u] = 0x00u;
    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".text",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       std::move(bytes)});
    image.add_entry_point(0u);
    return image;
}

katana::io::ExecutableImage classification_image(std::vector<std::uint8_t> bytes) {
    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".text",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       std::move(bytes)});
    image.add_entry_point(0u);
    return image;
}

const katana::analysis::IndirectControlFlowResolution*
site(const katana::analysis::ControlFlowAnalysisResult& analysis, const std::uint32_t address) {
    const auto found = std::find_if(
        analysis.indirect_control_flow.begin(),
        analysis.indirect_control_flow.end(),
        [address](const auto& candidate) { return candidate.instruction_address == address; });
    return found == analysis.indirect_control_flow.end() ? nullptr : &*found;
}

const katana::analysis::FunctionRegisterValueSummary*
summary(const katana::analysis::ControlFlowAnalysisResult& analysis,
        const std::uint32_t function,
        const std::uint8_t reg) {
    const auto owner = std::find_if(
        analysis.function_value_summaries.begin(),
        analysis.function_value_summaries.end(),
        [function](const auto& candidate) { return candidate.function_address == function; });
    if (owner == analysis.function_value_summaries.end()) return nullptr;
    const auto value =
        std::find_if(owner->registers.begin(),
                     owner->registers.end(),
                     [reg](const auto& candidate) { return candidate.register_index == reg; });
    return value == owner->registers.end() ? nullptr : &*value;
}

} // namespace

int main() {
    const auto unique_image =
        image_with_callee({0x10u, 0xE0u, 0x0Bu, 0x00u, 0x09u, 0x00u}); // mov #0x10,r0; rts; nop
    const auto unique = katana::analysis::analyze_control_flow(unique_image);
    const auto* unique_site = site(unique, 4u);
    require(unique_site != nullptr &&
                unique_site->status == katana::analysis::ResolutionStatus::Resolved &&
                unique_site->target == 0x10u &&
                unique_site->targets == std::vector<std::uint32_t>{0x10u} &&
                unique_site->evidence_call_sites == std::vector<std::uint32_t>{0u} &&
                unique_site->evidence_callees == std::vector<std::uint32_t>{0x20u},
            "Eindeutiger R0-Return wurde nicht mit Callsite-/Callee-Evidenz aufgeloest.");
    const auto* unique_summary = summary(unique, 0x20u, 0u);
    require(unique_summary != nullptr && unique_summary->complete &&
                unique_summary->values == std::vector<std::uint32_t>{0x10u} &&
                unique_summary->reason == "constant-return",
            "Eindeutige Funktionssummary fehlt oder ist nicht vollstaendig.");
    const auto* preserved = summary(unique, 0x20u, 8u);
    require(preserved != nullptr && preserved->abi_preserved,
            "SH-C-Erhalt von R8 wurde in der Funktionssummary nicht ausgewiesen.");

    std::vector<std::uint8_t> guarded_callee_bytes(0x26u, 0x09u);
    const std::array<std::uint8_t, 10u> guarded_caller{
        0x10u,
        0xE4u, // mov #0x10,r4
        0x0Bu,
        0x41u, // jsr @r1
        0x09u,
        0x00u, // nop (delay)
        0x2Bu,
        0x40u, // jmp @r0
        0x09u,
        0x00u // nop (delay)
    };
    const std::array<std::uint8_t, 6u> guarded_callee{
        0x43u,
        0x60u, // mov r4,r0
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(guarded_caller.begin(), guarded_caller.end(), guarded_callee_bytes.begin());
    std::copy(guarded_callee.begin(), guarded_callee.end(), guarded_callee_bytes.begin() + 0x20u);
    katana::io::ExecutableImage guarded_callee_image;
    guarded_callee_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    guarded_callee_image.add_segment({".text",
                                      0u,
                                      0u,
                                      guarded_callee_bytes.size(),
                                      katana::io::SegmentKind::Code,
                                      {true, false, true},
                                      guarded_callee_bytes});
    guarded_callee_image.add_entry_point(0u);
    const auto guarded_callee_lines = katana::sh4::disassemble(guarded_callee_bytes, 0u);
    const std::array<std::uint32_t, 1u> guarded_function_entries{0u};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> guarded_call_edges{{
        {2u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedComplete,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot}},
    }};
    const auto guarded_values = katana::analysis::analyze_function_values(
        guarded_callee_image, guarded_callee_lines, guarded_function_entries, guarded_call_edges);
    const auto guarded_callee_summary =
        std::find_if(guarded_values.summaries.begin(),
                     guarded_values.summaries.end(),
                     [](const auto& candidate) { return candidate.function_address == 0x20u; });
    require(guarded_callee_summary != guarded_values.summaries.end(),
            "Guarded-complete-Callkante legte ihren exklusiv erreichbaren Callee nicht an.");
    const auto guarded_r0 =
        std::find_if(guarded_callee_summary->registers.begin(),
                     guarded_callee_summary->registers.end(),
                     [](const auto& candidate) { return candidate.register_index == 0u; });
    const auto guarded_return =
        std::find_if(guarded_values.resolutions.begin(),
                     guarded_values.resolutions.end(),
                     [](const auto& candidate) { return candidate.instruction_address == 6u; });
    require(guarded_r0 != guarded_callee_summary->registers.end() && guarded_r0->complete &&
                guarded_r0->guarded && guarded_r0->values == std::vector<std::uint32_t>{0x10u} &&
                guarded_return != guarded_values.resolutions.end() && guarded_return->complete &&
                guarded_return->guarded &&
                guarded_return->evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedComplete &&
                guarded_return->targets == std::vector<std::uint32_t>{0x10u} &&
                guarded_return->call_sites == std::vector<std::uint32_t>{2u} &&
                guarded_return->callees == std::vector<std::uint32_t>{0x20u},
            "Guarded-complete-Callee verlor Ingressguard oder R0-Return-Summary.");

    [] {
        const auto multi_image =
            image_with_callee({// bt 0x28; mov #0x10,r0; rts; nop; mov #0x14,r0; rts; nop
                               0x02u,
                               0x89u,
                               0x10u,
                               0xE0u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u,
                               0x14u,
                               0xE0u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u});
        const auto multi = katana::analysis::analyze_control_flow(multi_image);
        const auto* multi_site = site(multi, 4u);
        require(multi_site != nullptr &&
                    multi_site->status == katana::analysis::ResolutionStatus::Resolved &&
                    !multi_site->target.has_value() &&
                    multi_site->targets == std::vector<std::uint32_t>({0x10u, 0x14u}) &&
                    multi_site->reason == "interprocedural-return-set",
                "Mehrwertige Return-Summary wurde nicht als endliche Zielmenge aufgeloest.");
        require(std::count_if(multi.resolved_edges.begin(),
                              multi.resolved_edges.end(),
                              [](const auto& edge) { return edge.instruction_address == 4u; }) == 2,
                "Mehrwertige Return-Summary erzeugte nicht genau zwei CFG-Kanten.");
        const auto multi_text = katana::analysis::format_indirect_control_flow_report(
            multi.indirect_control_flow, multi.jump_tables, multi.symbolic_addresses);
        const auto multi_json = katana::analysis::format_control_flow_analysis_json(multi);
        require(
            multi_text.find("interprocedural-return-set; evidence=proven-complete; r0; callees=") !=
                    std::string::npos &&
                multi_json.find("\"targets\":[\"0x00000010\",\"0x00000014\"]") !=
                    std::string::npos &&
                multi_json.find("\"function_value_summaries\":[") != std::string::npos,
            "Mehrziel- oder Summary-Evidenz fehlt im Text-/JSON-Bericht.");

        const auto conflicting_image =
            image_with_callee({// bt 0x28; mov #0x10,r0; rts; nop; rts; nop
                               0x02u,
                               0x89u,
                               0x10u,
                               0xE0u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u});
        const auto conflicting = katana::analysis::analyze_control_flow(conflicting_image);
        const auto* conflicting_site = site(conflicting, 4u);
        require(conflicting_site != nullptr &&
                    conflicting_site->status == katana::analysis::ResolutionStatus::Unresolved &&
                    conflicting_site->reason == "dynamic-return-value",
                "Widerspruechlicher Return-Pfad wurde nicht sichtbar dynamisch gelassen.");
        const auto* conflicting_summary = summary(conflicting, 0x20u, 0u);
        require(conflicting_summary != nullptr && !conflicting_summary->complete &&
                    conflicting_summary->reason == "return-path-unknown",
                "Widerspruechliche Return-Summary wurde faelschlich als vollstaendig markiert.");

        const auto recursive_image = image_with_callee({// bsr 0x20; nop; rts; nop
                                                        0xFEu,
                                                        0xBFu,
                                                        0x09u,
                                                        0x00u,
                                                        0x0Bu,
                                                        0x00u,
                                                        0x09u,
                                                        0x00u});
        const auto recursive = katana::analysis::analyze_control_flow(recursive_image);
        const auto* recursive_site = site(recursive, 4u);
        require(recursive_site != nullptr &&
                    recursive_site->status == katana::analysis::ResolutionStatus::Unresolved &&
                    recursive_site->reason == "dynamic-return-value" &&
                    recursive_site->origin_class ==
                        katana::analysis::IndirectControlFlowOriginClass::Callback,
                "Rekursive Summary ohne stabilen Return wurde als Zielbeweis verwendet.");

        const auto missing_return_image = image_with_callee({0x09u, 0x00u});
        const auto missing_return = katana::analysis::analyze_control_flow(missing_return_image);
        const auto* missing_return_summary = summary(missing_return, 0x20u, 0u);
        require(missing_return_summary != nullptr && !missing_return_summary->complete &&
                    missing_return_summary->reason == "no-return" &&
                    site(missing_return, 4u)->reason == "dynamic-return-value",
                "Callee ohne Return wurde nicht konservativ als unbekannt klassifiziert.");
    }();

    auto abi_less_image = unique_image;
    abi_less_image.set_guest_call_abi(katana::io::GuestCallAbi::Unknown);
    const auto abi_less = katana::analysis::analyze_control_flow(abi_less_image);
    const auto* abi_less_site = site(abi_less, 4u);
    require(abi_less.function_value_summaries.empty() && abi_less_site != nullptr &&
                abi_less_site->status == katana::analysis::ResolutionStatus::Unresolved,
            "ABI-loses Image erhielt eine SH-C-Return-Summary.");

    const auto parameter =
        katana::analysis::analyze_control_flow(classification_image({0x2Bu, 0x44u, 0x09u, 0x00u}));
    require(site(parameter, 0u)->reason == "dynamic-parameter" &&
                site(parameter, 0u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Parameter,
            "Offener Parameter-Call wurde nicht getrennt klassifiziert.");
    const auto stack = katana::analysis::analyze_control_flow(
        classification_image({0xF2u, 0x61u, 0x2Bu, 0x41u, 0x09u, 0x00u}));
    require(site(stack, 2u)->reason == "dynamic-stack-target" &&
                site(stack, 2u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Stack,
            "Offenes Stackziel wurde nicht getrennt klassifiziert.");
    const auto unbounded = katana::analysis::analyze_control_flow(
        classification_image({0x22u, 0x61u, 0x2Bu, 0x41u, 0x09u, 0x00u}));
    require(site(unbounded, 2u)->reason == "dynamic-unbounded-memory" &&
                site(unbounded, 2u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::UnboundedMemory,
            "Unbeschraenkter Speicherzeiger wurde nicht getrennt klassifiziert.");
    const auto vtable = katana::analysis::analyze_control_flow(
        classification_image({0x42u, 0x61u, 0x12u, 0x62u, 0x2Bu, 0x42u, 0x09u, 0x00u}));
    require(site(vtable, 4u)->reason == "dynamic-vtable-target" &&
                site(vtable, 4u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::ObjectVTable,
            "Offenes VTable-Ziel wurde nicht getrennt klassifiziert.");
    const auto runtime_pointer =
        katana::analysis::analyze_control_flow(classification_image({0x2Bu, 0x41u, 0x09u, 0x00u}));
    require(site(runtime_pointer, 0u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::RuntimePointer &&
                site(runtime_pointer, 0u)->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                katana::analysis::control_flow_report_status(*site(runtime_pointer, 0u)) ==
                    katana::analysis::ControlFlowReportStatus::RuntimeOnly &&
                site(runtime_pointer, 0u)->reason ==
                    "dynamic-runtime-pointer-register-value-unknown" &&
                site(runtime_pointer, 0u)->evidence_origins ==
                    std::vector{katana::analysis::AnalysisEvidenceOrigin::RuntimeClassification},
            "Allgemeiner unbekannter Zeiger besitzt keinen validierten Runtimevertrag: " +
                std::to_string(static_cast<int>(site(runtime_pointer, 0u)->origin_class)) + "/" +
                std::to_string(static_cast<int>(site(runtime_pointer, 0u)->evidence)) + "/" +
                std::to_string(static_cast<int>(
                    katana::analysis::control_flow_report_status(*site(runtime_pointer, 0u)))) +
                "/" + site(runtime_pointer, 0u)->reason + "/" +
                std::to_string(site(runtime_pointer, 0u)->evidence_origins.size()));
    const auto runtime_pointer_json =
        katana::analysis::format_control_flow_analysis_json(runtime_pointer);
    require(runtime_pointer_json.find("\"instruction_form\":\"Jmp\"") != std::string::npos &&
                runtime_pointer_json.find("\"definition_complete\":false") != std::string::npos &&
                runtime_pointer_json.find("\"preceding_call\":false") != std::string::npos,
            "Der Sitebericht verliert Instruktionsform oder Definitionsprovenienz.");

    std::vector<std::uint8_t> indexed_slice_bytes(24u, 0u);
    for (std::size_t index = 0u; index < indexed_slice_bytes.size(); index += 2u)
        indexed_slice_bytes[index] = 0x09u; // nop
    const std::array<std::uint8_t, 12u> joined_slice{
        0x22u, 0x61u, // mov.l @r2,r1
        0x01u, 0x89u, // bt 0x8
        0x09u, 0x00u, // nop
        0x09u, 0x00u, // nop
        0x2Bu, 0x41u, // jmp @r1
        0x09u, 0x00u  // nop (delay)
    };
    const std::array<std::uint8_t, 6u> disjoint_slice{
        0xF2u, 0x63u, // mov.l @r15,r3
        0x2Bu, 0x43u, // jmp @r3
        0x09u, 0x00u  // nop (delay)
    };
    std::copy(joined_slice.begin(), joined_slice.end(), indexed_slice_bytes.begin());
    std::copy(disjoint_slice.begin(),
              disjoint_slice.end(),
              indexed_slice_bytes.begin() + 16u);
    katana::io::ExecutableImage indexed_slice_image;
    indexed_slice_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    indexed_slice_image.add_segment({".text",
                                     0u,
                                     0u,
                                     indexed_slice_bytes.size(),
                                     katana::io::SegmentKind::Code,
                                     {true, false, true},
                                     std::move(indexed_slice_bytes)});
    indexed_slice_image.add_entry_point(0u);
    indexed_slice_image.add_entry_point(16u);
    const auto indexed_slices = katana::analysis::analyze_control_flow(indexed_slice_image);
    const auto* joined_site = site(indexed_slices, 8u);
    const auto* disjoint_site = site(indexed_slices, 18u);
    require(joined_site != nullptr && joined_site->definition_complete &&
                joined_site->definition_sites == std::vector<std::uint32_t>{0u} &&
                joined_site->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::UnboundedMemory,
            "Writer-Slice-Index verliert die gemeinsame Definition am CFG-Join.");
    require(disjoint_site != nullptr && disjoint_site->definition_complete &&
                disjoint_site->definition_sites == std::vector<std::uint32_t>{16u} &&
                disjoint_site->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Stack,
            "Writer-Slice-Index verwechselt Definitionen getrennter Entry-Bloecke.");

    std::vector<std::uint8_t> guarded_join_bytes(0x40u, 0x09u);
    const std::array<std::uint8_t, 20u> guarded_join_code{
        0x05u, 0xDCu, // mov.l @(0x18,pc),r12
        0x01u, 0x89u, // bt 0x8
        0x09u, 0x00u, // nop
        0x09u, 0x00u, // nop
        0x0Au, 0xB0u, // bsr 0x20
        0x09u, 0x00u, // nop (delay)
        0x0Bu, 0x4Cu, // jsr @r12
        0x09u, 0x00u, // nop (delay)
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u  // nop (delay)
    };
    std::copy(guarded_join_code.begin(), guarded_join_code.end(), guarded_join_bytes.begin());
    guarded_join_bytes[0x18u] = 0x30u;
    guarded_join_bytes[0x19u] = 0x00u;
    guarded_join_bytes[0x1Au] = 0x00u;
    guarded_join_bytes[0x1Bu] = 0x00u;
    guarded_join_bytes[0x20u] = 0x0Bu;
    guarded_join_bytes[0x22u] = 0x09u;
    guarded_join_bytes[0x30u] = 0x0Bu;
    guarded_join_bytes[0x32u] = 0x09u;
    katana::io::ExecutableImage guarded_join_image;
    guarded_join_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    guarded_join_image.add_segment({".rwx",
                                    0u,
                                    0u,
                                    guarded_join_bytes.size(),
                                    katana::io::SegmentKind::Code,
                                    {true, true, true},
                                    std::move(guarded_join_bytes)});
    guarded_join_image.add_entry_point(0u);
    const auto guarded_join = katana::analysis::analyze_control_flow(guarded_join_image);
    const auto* guarded_join_site = site(guarded_join, 0x0Cu);
    const auto guarded_join_edge =
        std::find_if(guarded_join.resolved_edges.begin(),
                     guarded_join.resolved_edges.end(),
                     [](const auto& edge) {
                         return edge.instruction_address == 0x0Cu && edge.target_address == 0x30u;
                     });
    require(guarded_join_site != nullptr &&
                guarded_join_site->evidence == katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                !guarded_join_site->target.has_value() && guarded_join_site->targets.empty() &&
                guarded_join_site->analysis_candidates == std::vector<std::uint32_t>{0x30u} &&
                guarded_join_site->reason == "runtime-contract-function-memory" &&
                guarded_join_edge == guarded_join.resolved_edges.end(),
            "CFG-Join fror einen veraenderlichen Speicherkandidaten statisch ein.");
    const auto guarded_join_ir = katana::ir::lower_program(guarded_join);
    const katana::ir::Instruction* guarded_join_ir_site = nullptr;
    for (const auto& function : guarded_join_ir)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (instruction.source_address == 0x0Cu) guarded_join_ir_site = &instruction;
    require(guarded_join_ir_site != nullptr &&
                guarded_join_ir_site->dynamic_target_class ==
                    katana::ir::DynamicTargetClass::RuntimeOnly &&
                guarded_join_ir_site->resolved_targets.empty() &&
                katana::ir::verify_program(guarded_join_ir).empty(),
            "Veraenderlicher Funktionsspeicher erreicht nicht kandidatenfrei den Runtimevertrag.");

    std::vector<std::uint8_t> parameter_candidate_bytes(0x60u, 0x09u);
    const std::array<std::uint8_t, 10u> parameter_caller{
        0x40u,
        0xE4u, // mov #0x40,r4
        0x0Du,
        0xB0u, // bsr 0x20
        0x09u,
        0x00u, // nop (delay)
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    const std::array<std::uint8_t, 10u> parameter_callee{
        0x42u,
        0x61u, // mov.l @r4,r1
        0x0Bu,
        0x41u, // jsr @r1
        0x09u,
        0x00u, // nop (delay)
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(parameter_caller.begin(), parameter_caller.end(), parameter_candidate_bytes.begin());
    std::copy(parameter_callee.begin(),
              parameter_callee.end(),
              parameter_candidate_bytes.begin() + 0x20u);
    parameter_candidate_bytes[0x40u] = 0x50u;
    parameter_candidate_bytes[0x41u] = 0x00u;
    parameter_candidate_bytes[0x42u] = 0x00u;
    parameter_candidate_bytes[0x43u] = 0x00u;
    parameter_candidate_bytes[0x50u] = 0x0Bu;
    parameter_candidate_bytes[0x51u] = 0x00u;
    parameter_candidate_bytes[0x52u] = 0x09u;
    parameter_candidate_bytes[0x53u] = 0x00u;
    katana::io::ExecutableImage parameter_candidate_image;
    parameter_candidate_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    parameter_candidate_image.add_segment({".rwx",
                                           0u,
                                           0u,
                                           parameter_candidate_bytes.size(),
                                           katana::io::SegmentKind::Code,
                                           {true, true, true},
                                           std::move(parameter_candidate_bytes)});
    parameter_candidate_image.add_entry_point(0u);
    const auto parameter_candidate =
        katana::analysis::analyze_control_flow(parameter_candidate_image);
    const auto* parameter_candidate_site = site(parameter_candidate, 0x22u);
    require(parameter_candidate_site != nullptr &&
                parameter_candidate_site->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                parameter_candidate_site->analysis_candidates ==
                    std::vector<std::uint32_t>{0x50u} &&
                parameter_candidate_site->reason == "runtime-contract-function-memory" &&
                parameter_candidate_site->evidence_call_sites == std::vector<std::uint32_t>{0x02u},
            "Direkter Call propagierte seinen Parameterkandidaten nicht sicher zum Callee.");

    auto unknown_caller_image = parameter_candidate_image;
    unknown_caller_image.add_entry_point(0x20u);
    const auto unknown_caller = katana::analysis::analyze_control_flow(unknown_caller_image);
    const auto* unknown_caller_site = site(unknown_caller, 0x22u);
    require(
        unknown_caller_site != nullptr &&
            !katana::analysis::control_flow_evidence_complete(unknown_caller_site->evidence) &&
            (unknown_caller_site->evidence == katana::analysis::ControlFlowEvidence::RuntimeOnly ||
             unknown_caller_site->evidence == katana::analysis::ControlFlowEvidence::Unresolved),
        "Ein unbekannter zusaetzlicher Caller wurde durch einen bekannten Caller geheilt.");

    std::vector<std::uint8_t> indirect_parameter_bytes(0x60u, 0x09u);
    const std::array<std::uint8_t, 12u> indirect_parameter_caller{
        0x40u,
        0xE4u, // mov #0x40,r4
        0x03u,
        0xDCu, // mov.l @(0x10,pc),r12
        0x0Bu,
        0x4Cu, // jsr @r12
        0x09u,
        0x00u, // nop (delay)
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(indirect_parameter_caller.begin(),
              indirect_parameter_caller.end(),
              indirect_parameter_bytes.begin());
    std::copy(
        parameter_callee.begin(), parameter_callee.end(), indirect_parameter_bytes.begin() + 0x20u);
    indirect_parameter_bytes[0x10u] = 0x20u;
    indirect_parameter_bytes[0x11u] = 0x00u;
    indirect_parameter_bytes[0x12u] = 0x00u;
    indirect_parameter_bytes[0x13u] = 0x00u;
    indirect_parameter_bytes[0x40u] = 0x50u;
    indirect_parameter_bytes[0x41u] = 0x00u;
    indirect_parameter_bytes[0x42u] = 0x00u;
    indirect_parameter_bytes[0x43u] = 0x00u;
    indirect_parameter_bytes[0x50u] = 0x0Bu;
    indirect_parameter_bytes[0x51u] = 0x00u;
    indirect_parameter_bytes[0x52u] = 0x09u;
    indirect_parameter_bytes[0x53u] = 0x00u;
    katana::io::ExecutableImage indirect_parameter_image;
    indirect_parameter_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    indirect_parameter_image.add_segment({".rwx",
                                          0u,
                                          0u,
                                          indirect_parameter_bytes.size(),
                                          katana::io::SegmentKind::Code,
                                          {true, true, true},
                                          std::move(indirect_parameter_bytes)});
    indirect_parameter_image.add_entry_point(0u);
    const auto indirect_parameter =
        katana::analysis::analyze_control_flow(indirect_parameter_image);
    const auto* indirect_parameter_site = site(indirect_parameter, 0x22u);
    require(indirect_parameter_site != nullptr &&
                indirect_parameter_site->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                indirect_parameter_site->analysis_candidates == std::vector<std::uint32_t>{0x50u} &&
                indirect_parameter_site->evidence_call_sites == std::vector<std::uint32_t>{0x04u},
            "Bewachter indirekter Call propagierte seinen Parameterkandidaten nicht zum Callee.");

    std::vector<std::uint8_t> finite_index_bytes(0x38u, 0x09u);
    const std::array<std::uint8_t, 16u> finite_index_code{
        0x29u,
        0x00u, // movt r0 -> {0,1}
        0x08u,
        0x40u, // shll2 r0 -> {0,4}
        0x02u,
        0xD1u, // mov.l @(0x10,pc),r1
        0x1Eu,
        0x02u, // mov.l @(r0,r1),r2
        0x0Bu,
        0x42u, // jsr @r2
        0x09u,
        0x00u, // nop (delay)
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(finite_index_code.begin(), finite_index_code.end(), finite_index_bytes.begin());
    finite_index_bytes[0x10u] = 0x18u;
    finite_index_bytes[0x11u] = 0x00u;
    finite_index_bytes[0x12u] = 0x00u;
    finite_index_bytes[0x13u] = 0x00u;
    finite_index_bytes[0x18u] = 0x30u;
    finite_index_bytes[0x19u] = 0x00u;
    finite_index_bytes[0x1Au] = 0x00u;
    finite_index_bytes[0x1Bu] = 0x00u;
    finite_index_bytes[0x1Cu] = 0x34u;
    finite_index_bytes[0x1Du] = 0x00u;
    finite_index_bytes[0x1Eu] = 0x00u;
    finite_index_bytes[0x1Fu] = 0x00u;
    finite_index_bytes[0x30u] = 0x0Bu;
    finite_index_bytes[0x31u] = 0x00u;
    finite_index_bytes[0x32u] = 0x09u;
    finite_index_bytes[0x33u] = 0x00u;
    finite_index_bytes[0x34u] = 0x0Bu;
    finite_index_bytes[0x35u] = 0x00u;
    finite_index_bytes[0x36u] = 0x09u;
    finite_index_bytes[0x37u] = 0x00u;
    katana::io::ExecutableImage finite_index_image;
    finite_index_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    finite_index_image.add_segment({".rwx",
                                    0u,
                                    0u,
                                    finite_index_bytes.size(),
                                    katana::io::SegmentKind::Code,
                                    {true, true, true},
                                    std::move(finite_index_bytes)});
    finite_index_image.add_entry_point(0u);
    const auto finite_index = katana::analysis::analyze_control_flow(finite_index_image);
    const auto* finite_index_site = site(finite_index, 0x08u);
    require(finite_index_site != nullptr &&
                finite_index_site->evidence == katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                finite_index_site->targets.empty() &&
                finite_index_site->analysis_candidates ==
                    std::vector<std::uint32_t>({0x30u, 0x34u}) &&
                finite_index_site->reason == "runtime-contract-function-memory",
            "Endlicher, veraenderlicher Indexpfad wurde als vollstaendige Zielmenge eingefroren.");
    require(
        finite_index.function_scc_count != 0u && finite_index.function_summary_iterations != 0u &&
            finite_index.instruction_arena != nullptr &&
            finite_index.instruction_arena->size() == finite_index.recursive.instructions.size() &&
            !finite_index.block_spans.empty() && finite_index.evidence_ids.size() != 0u,
        "SCC-Summaries, immutable Arena, Blockspans oder Evidence-Interning fehlen.");

    [] {
        std::vector<std::uint8_t> stack_spill_bytes(0x24u, 0x09u);
        const std::array<std::uint8_t, 14u> stack_spill_code{
            0x03u,
            0xD8u, // mov.l @(0x10,pc),r8
            0x86u,
            0x2Fu, // mov.l r8,@-r15
            0xF6u,
            0x6Du, // mov.l @r15+,r13
            0x0Bu,
            0x4Du, // jsr @r13
            0x09u,
            0x00u, // nop (delay)
            0x0Bu,
            0x00u, // rts
            0x09u,
            0x00u // nop (delay)
        };
        std::copy(stack_spill_code.begin(), stack_spill_code.end(), stack_spill_bytes.begin());
        stack_spill_bytes[0x10u] = 0x20u;
        stack_spill_bytes[0x11u] = 0x00u;
        stack_spill_bytes[0x12u] = 0x00u;
        stack_spill_bytes[0x13u] = 0x00u;
        stack_spill_bytes[0x20u] = 0x0Bu;
        stack_spill_bytes[0x21u] = 0x00u;
        stack_spill_bytes[0x22u] = 0x09u;
        stack_spill_bytes[0x23u] = 0x00u;
        const auto stack_spill = katana::analysis::analyze_control_flow(
            classification_image(std::move(stack_spill_bytes)));
        const auto* stack_spill_site = site(stack_spill, 0x06u);
        if (stack_spill_site == nullptr) {
            require(false, "Stackspill/Reload-Callsite fehlt.");
            return;
        }
        require(stack_spill_site->target == 0x20u,
                "Fester Stackspill/Reload verliert sein R13-Ziel.");
        require(stack_spill_site->status == katana::analysis::ResolutionStatus::Resolved,
                "Fester Stackspill/Reload verliert seinen vollstaendigen Beweis: Status " +
                    std::to_string(static_cast<int>(stack_spill_site->status)) + ", Evidenz " +
                    std::to_string(static_cast<int>(stack_spill_site->evidence)) + ", Grund " +
                    stack_spill_site->reason + ".");
    }();

    [] {
        const auto object_image = [](const bool invalidate_with_byte,
                                     const bool invalidate_with_prefetch) {
            std::vector<std::uint8_t> text(0x34u, 0x09u);
            text[0x00u] = 0x07u;
            text[0x01u] = 0xD4u; // mov.l @(0x20,pc),r4 -> Objekt 0x40
            text[0x02u] = 0x08u;
            text[0x03u] = 0xD1u; // mov.l @(0x24,pc),r1 -> Callback 0x30
            text[0x04u] = 0x12u;
            text[0x05u] = 0x24u; // mov.l r1,@r4
            std::size_t cursor = 0x06u;
            if (invalidate_with_byte) {
                text[cursor++] = 0x00u;
                text[cursor++] = 0x24u; // mov.b r0,@r4 ueberlappt das Feld
            } else if (invalidate_with_prefetch) {
                text[cursor++] = 0x83u;
                text[cursor++] = 0x04u; // pref @r4 invalidiert unbekannte Mutation
            }
            const auto load_address = cursor;
            text[cursor++] = 0x42u;
            text[cursor++] = 0x62u; // mov.l @r4,r2
            const auto call_address = cursor;
            text[cursor++] = 0x0Bu;
            text[cursor++] = 0x42u; // jsr @r2
            text[cursor++] = 0x09u;
            text[cursor++] = 0x00u;
            text[cursor++] = 0x0Bu;
            text[cursor++] = 0x00u;
            text[cursor++] = 0x09u;
            text[cursor++] = 0x00u;
            text[0x20u] = 0x40u;
            text[0x21u] = 0x00u;
            text[0x22u] = 0x00u;
            text[0x23u] = 0x00u;
            text[0x24u] = 0x30u;
            text[0x25u] = 0x00u;
            text[0x26u] = 0x00u;
            text[0x27u] = 0x00u;
            text[0x30u] = 0x0Bu;
            text[0x31u] = 0x00u;
            text[0x32u] = 0x09u;
            text[0x33u] = 0x00u;
            katana::io::ExecutableImage image;
            image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
            image.add_segment({".text",
                               0u,
                               0u,
                               text.size(),
                               katana::io::SegmentKind::Code,
                               {true, false, true},
                               std::move(text)});
            image.add_segment({".object",
                               0x40u,
                               0x34u,
                               4u,
                               katana::io::SegmentKind::Data,
                               {true, true, false},
                               std::vector<std::uint8_t>(4u)});
            image.add_entry_point(0u);
            return std::pair{std::move(image),
                             std::pair{static_cast<std::uint32_t>(load_address),
                                       static_cast<std::uint32_t>(call_address)}};
        };
        auto [dominant_object_image, dominant_addresses] = object_image(false, false);
        const auto dominant_object = katana::analysis::analyze_control_flow(dominant_object_image);
        const auto* dominant_site = site(dominant_object, dominant_addresses.second);
        require(dominant_site != nullptr && dominant_site->target == 0x30u &&
                    dominant_site->status == katana::analysis::ResolutionStatus::Guarded &&
                    dominant_site->evidence ==
                        katana::analysis::ControlFlowEvidence::GuardedComplete &&
                    dominant_site->origin_class ==
                        katana::analysis::IndirectControlFlowOriginClass::ObjectVTable &&
                    dominant_object.function_summary_iterations <=
                        dominant_object.function_iteration_budget &&
                    !dominant_object.function_budget_exhausted,
                "Dominanter Objektfeldstore erzeugt keine begrenzte vollstaendige Zielmenge.");
        auto [overlap_image, overlap_addresses] = object_image(true, false);
        const auto overlap = katana::analysis::analyze_control_flow(overlap_image);
        require(site(overlap, overlap_addresses.second) != nullptr &&
                    !katana::analysis::control_flow_evidence_complete(
                        site(overlap, overlap_addresses.second)->evidence),
                "Ueberlappender Teilstore laesst einen stale Objektfeldbeweis bestehen.");
        auto [prefetch_image, prefetch_addresses] = object_image(false, true);
        const auto prefetch = katana::analysis::analyze_control_flow(prefetch_image);
        require(site(prefetch, prefetch_addresses.second) != nullptr &&
                    !katana::analysis::control_flow_evidence_complete(
                        site(prefetch, prefetch_addresses.second)->evidence),
                "PREF laesst einen stale Objektfeldbeweis bestehen.");
    }();

    std::cout << "KR-4713 interprozedurale Zielwertsummaries erfolgreich.\n";
    return EXIT_SUCCESS;
}
