#include "cli/closure_witness_import.hpp"
#include "katana/runtime/closure_witness.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

katana::cli::ClosureWitnessDocument document() {
    using namespace katana::cli;
    ClosureWitnessDocument result;
    result.binding.analysis_artifact_key = "analysis:a";
    result.binding.content_identity = "content:b";
    result.binding.boot_byte_identity = "boot:c";
    result.binding.project_identity = "project:e";
    result.binding.analysis_contract_identity = "contract:f";
    result.binding.image_analysis_key = "image-analysis:g";
    result.binding.game_project_identity = "game:h";
    result.binding.native_port_identity = "port:i";
    result.binding.native_port_artifact_identity = "port-artifact:j";
    result.binding.analysis_implementation_identity = "analysis-impl:k";
    result.binding.analysis_cache_implementation_identity = "cache-impl:l";
    result.binding.ir_product_implementation_identity = "ir-impl:m";
    result.binding.codegen_implementation_identity = "codegen-impl:n";
    result.binding.analyzer_abi = "124";
    result.binding.backend_abi = "24";
    result.binding.analysis_mode = "1";
    result.binding.disc_volume_start_lba = "100";
    result.binding.disc_extent_lba_bias = "200";
    result.binding.runtime_generation = "1";
    ClosureWitness witness;
    witness.kind = "jump-table";
    witness.source = {"10", "source:s"};
    witness.callsite = {"20", "callsite:c"};
    witness.pointer = {false, "", "", "40"};
    witness.target = {"50", "target:t"};
    witness.alias = {"60", "alias:a"};
    witness.slot = {false, "", ""};
    witness.flags = {true, true, true, false, false};
    result.witnesses.push_back(witness);
    return result;
}

} // namespace

int main() {
    using namespace katana::cli;
    const auto source = document();
    require(!closure_witness_is_admissible(source),
            "wire witness flags must never self-admit closure");
    const auto serialized = serialize_closure_witness_v5(source);
    const auto imported = import_closure_witness_v5(serialized);
    require(imported.parsed && imported.valid && !imported.closure_admitted &&
                imported.reproof_required,
            "serialized v5 witness must round-trip fail-closed");
    require(serialized.find("slot_address") == std::string::npos &&
                serialized.find("\"address_present\":false") != std::string::npos &&
                serialized.find("slot_present") != std::string::npos,
            "v5 must use explicit pointer/slot presence instead of legacy fields");

    auto runtime = source;
    runtime.witnesses.front().flags.runtime_observation = true;
    runtime.witnesses.front().flags.reproof_required = true;
    require(!closure_witness_is_admissible(runtime), "runtime observation must not close");

    auto dropped = source;
    dropped.drop_count = 1u;
    const auto dropped_json = serialize_closure_witness_v5(dropped);
    const auto dropped_result = import_closure_witness_v5(dropped_json);
    require(dropped_result.parsed && !dropped_result.valid &&
                !dropped_result.closure_admitted &&
                !dropped_result.document.witnesses.front().flags.complete,
            "drops must fail closed and clear completeness");

    auto unknown = serialized;
    const auto insertion = unknown.rfind('}');
    unknown.insert(insertion, ",\"slot_address\":null");
    const auto unknown_result = import_closure_witness_v5(unknown);
    require(!unknown_result.parsed && !unknown_result.closure_admitted,
            "unknown legacy fields must be rejected strictly");

    auto claimed = serialized;
    const auto marker = claimed.find("\"closure_admitted\":false");
    require(marker != std::string::npos, "serialized admission marker missing");
    claimed.replace(marker, std::string("\"closure_admitted\":false").size(),
                    "\"closure_admitted\":true");
    const auto claimed_result = import_closure_witness_v5(claimed);
    require(!claimed_result.parsed && !claimed_result.closure_admitted,
            "inconsistent admission claim must be rejected");

    auto noncanonical = serialized;
    const auto numeric = noncanonical.find("\"runtime_generation\":\"1\"");
    require(numeric != std::string::npos, "serialized runtime generation missing");
    noncanonical.replace(numeric, std::string("\"runtime_generation\":\"1\"").size(),
                         "\"runtime_generation\":\"01\"");
    require(!import_closure_witness_v5(noncanonical).parsed,
            "non-canonical decimal identity must be rejected");

    const katana::runtime::ClosureWitnessBindingView runtime_binding{
        "analysis:a", "content:b", "boot:c", "project:e", "contract:f",
        "image-analysis:g", "game:h", "port:i", "port-artifact:j",
        "analysis-impl:k", "cache-impl:l", "ir-impl:m", "codegen-impl:n",
        124u, 24u, 1u, 100u, 200u, 7u};
    const std::array runtime_witnesses{
        katana::runtime::ClosureWitnessView{
            "indirect-dispatch",
            {0x1000u, "sha256:runtime-source"},
            {0x1010u, "sha256:runtime-callsite"},
            {false, 0u, {}, 0xA0003000u},
            {0x3000u, "sha256:runtime-target"},
            {0xA0003000u, "sha256:runtime-alias"},
            {false, 0u, {}},
            {false, false, false, true, true}}};
    const auto runtime_line = katana::runtime::serialize_closure_witness_v5(
        {runtime_binding, runtime_witnesses, 0u, false, false});
    require(!runtime_line.truncated,
            "runtime/product Closure-Witness-Sidecar wurde unerwartet gekappt");
    const auto runtime_import = import_closure_witness_v5(runtime_line.view());
    require(runtime_import.parsed && runtime_import.valid &&
                runtime_import.reproof_required &&
                !runtime_import.closure_admitted &&
                runtime_import.document.binding.runtime_generation == "7" &&
                runtime_import.document.witnesses.size() == 1u &&
                runtime_import.document.witnesses.front().callsite.address ==
                    "4112" &&
                runtime_import.document.witnesses.front().pointer.value ==
                    "2684366848",
            "runtime/product Sidecar und strikter CLI-v5-Importer sprechen nicht"
            " dasselbe Wire-Format.");

    // End-to-end retention contract: an explicitly armed successful indirect
    // dispatch remains first in the bounded ring when a later hardware fault
    // terminates the run, and the product-side serializer preserves that
    // chronology for the strict CLI importer. Runtime evidence stays
    // incomplete and requires immutable static reproof.
    static katana::runtime::CrashCapsule timeline_capsule;
    constexpr std::array timeline_candidates{
        katana::runtime::ClosureProbeEligibleSiteView{
            0x1000u, 0x100u, 0x1010u, "sha256:timeline-owner"}};
    constexpr std::array timeline_selection{0x1010u};
    katana::runtime::ClosureProbePlanState timeline_plan;
    require(timeline_plan.configure(
                timeline_candidates, timeline_selection, &timeline_capsule) &&
                timeline_capsule.v5.closure_probe_plan_count == 0u &&
                timeline_plan.note_successful_dispatch(
                    0x1010u, 0x3000u, 0xA0003000u, 0x1014u, 9u,
                    "sha256:timeline-target", 40u, timeline_capsule),
            "plan-bound successful dispatch could not be retained");

    katana::runtime::CrashCapsuleClosureWitness terminal_fault;
    terminal_fault.sequence = 41u;
    terminal_fault.generation = 9u;
    terminal_fault.kind = static_cast<std::uint32_t>(
        katana::runtime::CrashCapsuleV5WitnessKind::HardwareAccess);
    terminal_fault.source = 0x5000u;
    terminal_fault.callsite = 0x5050u;
    terminal_fault.pointer_value = 0xFF000038u;
    terminal_fault.target = 0xFF000038u;
    terminal_fault.alias = 0xFF000038u;
    terminal_fault.runtime_observation = 1u;
    terminal_fault.reproof_required = 1u;
    terminal_fault.source_identity.assign("sha256:timeline-fault-source");
    terminal_fault.target_identity.assign("sha256:timeline-fault-target");
    timeline_capsule.note_v5_closure_witness(terminal_fault);
    require(timeline_capsule.v5.closure_witness_count == 2u &&
                timeline_capsule.v5.closure_witnesses[0].sequence == 40u &&
                timeline_capsule.v5.closure_witnesses[1].sequence == 41u &&
                timeline_capsule.v5.closure_witnesses[0].callsite == 0x1010u &&
                timeline_capsule.v5.closure_witnesses[1].callsite == 0x5050u,
            "later hardware fault did not retain chronological dispatch evidence");

    const auto witness_view = [](const auto& value,
                                 const std::string_view kind) {
        return katana::runtime::ClosureWitnessView{
            kind,
            {value.source, value.source_identity.view()},
            {value.callsite, value.source_identity.view()},
            {value.pointer_present != 0u, value.pointer,
             value.target_identity.view(),
             static_cast<std::uint32_t>(value.pointer_value)},
            {value.target, value.target_identity.view()},
            {value.alias, value.target_identity.view()},
            {value.slot_present != 0u, value.slot,
             value.table_identity.view()},
            {false, false, false,
             value.runtime_observation != 0u,
             value.reproof_required != 0u}};
    };
    const std::array timeline_views{
        witness_view(timeline_capsule.v5.closure_witnesses[0],
                     "indirect-dispatch"),
        witness_view(timeline_capsule.v5.closure_witnesses[1],
                     "hardware-access")};
    const auto timeline_line = katana::runtime::serialize_closure_witness_v5(
        {runtime_binding, timeline_views, 0u, false, false});
    const auto timeline_import = import_closure_witness_v5(timeline_line.view());
    require(!timeline_line.truncated && timeline_import.parsed &&
                timeline_import.valid && timeline_import.reproof_required &&
                !timeline_import.closure_admitted &&
                timeline_import.document.witnesses.size() == 2u &&
                timeline_import.document.witnesses[0].kind ==
                    "indirect-dispatch" &&
                timeline_import.document.witnesses[0].callsite.address ==
                    "4112" &&
                timeline_import.document.witnesses[1].kind ==
                    "hardware-access" &&
                timeline_import.document.witnesses[1].callsite.address ==
                    "20560" &&
                !timeline_import.document.witnesses[0].flags.complete &&
                !timeline_import.document.witnesses[1].flags.complete,
            "dispatch-to-fault chronology did not survive v5 sidecar import");

    std::cout << "Closure-Witness-v5 strict import tests passed.\n";
}
