#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/function_value_analysis.hpp"

#include <algorithm>
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

    std::cout << "KR-4713 interprozedurale Zielwertsummaries erfolgreich.\n";
    return EXIT_SUCCESS;
}
