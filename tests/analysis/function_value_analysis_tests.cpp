#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/function_value_analysis.hpp"

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
    require(multi_text.find("interprocedural-return-set; r0; callees=") != std::string::npos &&
                multi_json.find("\"targets\":[\"0x00000010\",\"0x00000014\"]") !=
                    std::string::npos &&
                multi_json.find("\"function_value_summaries\":[") != std::string::npos,
            "Mehrziel- oder Summary-Evidenz fehlt im Text-/JSON-Bericht.");

    const auto conflicting_image = image_with_callee({// bt 0x28; mov #0x10,r0; rts; nop; rts; nop
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
                recursive_site->reason == "dynamic-return-value",
            "Rekursive Summary ohne stabilen Return wurde als Zielbeweis verwendet.");

    const auto missing_return_image = image_with_callee({0x09u, 0x00u});
    const auto missing_return = katana::analysis::analyze_control_flow(missing_return_image);
    const auto* missing_return_summary = summary(missing_return, 0x20u, 0u);
    require(missing_return_summary != nullptr && !missing_return_summary->complete &&
                missing_return_summary->reason == "no-return" &&
                site(missing_return, 4u)->reason == "dynamic-return-value",
            "Callee ohne Return wurde nicht konservativ als unbekannt klassifiziert.");

    auto abi_less_image = unique_image;
    abi_less_image.set_guest_call_abi(katana::io::GuestCallAbi::Unknown);
    const auto abi_less = katana::analysis::analyze_control_flow(abi_less_image);
    const auto* abi_less_site = site(abi_less, 4u);
    require(abi_less.function_value_summaries.empty() && abi_less_site != nullptr &&
                abi_less_site->status == katana::analysis::ResolutionStatus::Unresolved,
            "ABI-loses Image erhielt eine SH-C-Return-Summary.");

    const auto parameter =
        katana::analysis::analyze_control_flow(classification_image({0x2Bu, 0x44u, 0x09u, 0x00u}));
    require(site(parameter, 0u)->reason == "dynamic-parameter",
            "Offener Parameter-Call wurde nicht getrennt klassifiziert.");
    const auto stack = katana::analysis::analyze_control_flow(
        classification_image({0xF2u, 0x61u, 0x2Bu, 0x41u, 0x09u, 0x00u}));
    require(site(stack, 2u)->reason == "dynamic-stack-target",
            "Offenes Stackziel wurde nicht getrennt klassifiziert.");
    const auto unbounded = katana::analysis::analyze_control_flow(
        classification_image({0x22u, 0x61u, 0x2Bu, 0x41u, 0x09u, 0x00u}));
    require(site(unbounded, 2u)->reason == "dynamic-unbounded-memory",
            "Unbeschraenkter Speicherzeiger wurde nicht getrennt klassifiziert.");
    const auto vtable = katana::analysis::analyze_control_flow(
        classification_image({0x42u, 0x61u, 0x12u, 0x62u, 0x2Bu, 0x42u, 0x09u, 0x00u}));
    require(site(vtable, 4u)->reason == "dynamic-vtable-target",
            "Offenes VTable-Ziel wurde nicht getrennt klassifiziert.");

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
    const auto guarded_join_edge = std::find_if(
        guarded_join.resolved_edges.begin(), guarded_join.resolved_edges.end(), [](const auto& edge) {
            return edge.instruction_address == 0x0Cu && edge.target_address == 0x30u;
        });
    require(guarded_join_site != nullptr &&
                guarded_join_site->status == katana::analysis::ResolutionStatus::Guarded &&
                guarded_join_site->target == 0x30u &&
                guarded_join_site->reason == "guarded-function-memory" &&
                guarded_join_edge != guarded_join.resolved_edges.end() &&
                guarded_join_edge->guarded,
            "CFG-Join oder SH-C-Call verlor den dynamisch bewachten Speicherkandidaten.");

    std::vector<std::uint8_t> parameter_candidate_bytes(0x60u, 0x09u);
    const std::array<std::uint8_t, 10u> parameter_caller{
        0x40u, 0xE4u, // mov #0x40,r4
        0x0Du, 0xB0u, // bsr 0x20
        0x09u, 0x00u, // nop (delay)
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u  // nop (delay)
    };
    const std::array<std::uint8_t, 10u> parameter_callee{
        0x42u, 0x61u, // mov.l @r4,r1
        0x0Bu, 0x41u, // jsr @r1
        0x09u, 0x00u, // nop (delay)
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u  // nop (delay)
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
                parameter_candidate_site->status == katana::analysis::ResolutionStatus::Guarded &&
                parameter_candidate_site->target == 0x50u &&
                parameter_candidate_site->reason == "guarded-function-memory" &&
                parameter_candidate_site->evidence_call_sites ==
                    std::vector<std::uint32_t>{0x02u},
            "Direkter Call propagierte seinen Parameterkandidaten nicht sicher zum Callee.");

    std::vector<std::uint8_t> indirect_parameter_bytes(0x60u, 0x09u);
    const std::array<std::uint8_t, 12u> indirect_parameter_caller{
        0x40u, 0xE4u, // mov #0x40,r4
        0x03u, 0xDCu, // mov.l @(0x10,pc),r12
        0x0Bu, 0x4Cu, // jsr @r12
        0x09u, 0x00u, // nop (delay)
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u  // nop (delay)
    };
    std::copy(indirect_parameter_caller.begin(),
              indirect_parameter_caller.end(),
              indirect_parameter_bytes.begin());
    std::copy(parameter_callee.begin(),
              parameter_callee.end(),
              indirect_parameter_bytes.begin() + 0x20u);
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
                indirect_parameter_site->status ==
                    katana::analysis::ResolutionStatus::Guarded &&
                indirect_parameter_site->target == 0x50u &&
                indirect_parameter_site->evidence_call_sites ==
                    std::vector<std::uint32_t>{0x04u},
            "Bewachter indirekter Call propagierte seinen Parameterkandidaten nicht zum Callee.");

    std::vector<std::uint8_t> finite_index_bytes(0x38u, 0x09u);
    const std::array<std::uint8_t, 16u> finite_index_code{
        0x29u, 0x00u, // movt r0 -> {0,1}
        0x08u, 0x40u, // shll2 r0 -> {0,4}
        0x02u, 0xD1u, // mov.l @(0x10,pc),r1
        0x1Eu, 0x02u, // mov.l @(r0,r1),r2
        0x0Bu, 0x42u, // jsr @r2
        0x09u, 0x00u, // nop (delay)
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u  // nop (delay)
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
                finite_index_site->status == katana::analysis::ResolutionStatus::Guarded &&
                finite_index_site->targets ==
                    std::vector<std::uint32_t>({0x30u, 0x34u}) &&
                finite_index_site->reason == "guarded-function-memory",
            "Endlicher MOVT-/Shift-/Indexpfad wurde nicht als bewachte Zielmenge erhalten.");

    std::cout << "KR-4713 interprozedurale Zielwertsummaries erfolgreich.\n";
    return EXIT_SUCCESS;
}
