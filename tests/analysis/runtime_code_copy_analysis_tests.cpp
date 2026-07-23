#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/analysis/runtime_code_copy_analysis.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t base_address = 0x00001000u;
constexpr std::uint32_t copy_setup = 0x00001006u;
constexpr std::uint32_t copy_loop = 0x00001010u;
constexpr std::uint32_t source_begin = 0x00001080u;
constexpr std::uint32_t source_end_inclusive = 0x00001088u;
constexpr std::uint32_t patch_slot = 0x00001088u;
constexpr std::uint32_t patch_target = 0x00001100u;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void write_u16(std::vector<std::uint8_t>& bytes,
               const std::uint32_t address,
               const std::uint16_t value) {
    const auto offset = static_cast<std::size_t>(address - base_address);
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void write_u32(std::vector<std::uint8_t>& bytes,
               const std::uint32_t address,
               const std::uint32_t value) {
    const auto offset = static_cast<std::size_t>(address - base_address);
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
    bytes[offset + 2u] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
    bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
}

katana::io::ExecutableImage copy_image(const bool valid_compare = true,
                                       const std::uint32_t candidate = patch_target) {
    std::vector<std::uint8_t> bytes(0x120u, 0u);

    // A finite pre-copy patch: both the slot address and written handler value come from PC
    // relative 32-bit literals in this straight-line block.
    write_u16(bytes, 0x1000u, 0xD00Fu); // mov.l @(0x3c,pc),r0 -> [0x1040]
    write_u16(bytes, 0x1002u, 0xD110u); // mov.l @(0x40,pc),r1 -> [0x1044]
    write_u16(bytes, 0x1004u, 0x2012u); // mov.l r1,@r0

    // Bounded VBR-relative longword copy with an inclusive source-end comparison.
    write_u16(bytes, 0x1006u, 0x0222u);                           // stc vbr,r2
    write_u16(bytes, 0x1008u, 0xD30Fu);                           // mov.l @(0x3c,pc),r3 -> [0x1048]
    write_u16(bytes, 0x100Au, 0x9418u);                           // mov.w @(0x30,pc),r4 -> [0x103e]
    write_u16(bytes, 0x100Cu, 0xA003u);                           // bra 0x1016
    write_u16(bytes, 0x100Eu, 0x324Cu);                           // add r4,r2 (delay slot)
    write_u16(bytes, 0x1010u, 0x6536u);                           // mov.l @r3+,r5
    write_u16(bytes, 0x1012u, 0x2252u);                           // mov.l r5,@r2
    write_u16(bytes, 0x1014u, 0x7204u);                           // add #4,r2
    write_u16(bytes, 0x1016u, 0xD60Du);                           // mov.l @(0x34,pc),r6 -> [0x104c]
    write_u16(bytes, 0x1018u, valid_compare ? 0x3366u : 0x3360u); // cmp/hi or cmp/eq
    write_u16(bytes, 0x101Au, 0x8BF9u);                           // bf 0x1010
    write_u16(bytes, 0x101Cu, 0x000Bu);                           // rts
    write_u16(bytes, 0x101Eu, 0x0009u);                           // nop

    write_u16(bytes, 0x103Eu, 0xFA00u); // signed VBR delta (-0x600)
    write_u32(bytes, 0x1040u, patch_slot);
    write_u32(bytes, 0x1044u, candidate);
    write_u32(bytes, 0x1048u, source_begin);
    write_u32(bytes, 0x104Cu, source_end_inclusive);

    // The copied template references 0x1088 as a long literal.  Recursive decoding reaches the
    // RTS delay slot at 0x1084 but never classifies the patched literal itself as an instruction.
    write_u16(bytes, 0x1080u, 0xD701u); // mov.l @(4,pc),r7 -> [0x1088]
    write_u16(bytes, 0x1082u, 0x000Bu); // rts
    write_u16(bytes, 0x1084u, 0x0009u); // nop
    write_u32(bytes, patch_slot, 0u);
    write_u16(bytes, patch_target, 0x000Bu);      // rts
    write_u16(bytes, patch_target + 2u, 0x0009u); // nop

    katana::io::ExecutableImage image;
    image.add_segment({".mixed",
                       base_address,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       std::move(bytes)});
    image.add_entry_point(base_address);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_initial_snapshot_entry(base_address);
    return image;
}

const katana::analysis::FunctionCandidate*
find_function(const katana::analysis::ControlFlowAnalysisResult& analysis,
              const std::uint32_t address) {
    const auto found =
        std::find_if(analysis.recursive.functions.begin(),
                     analysis.recursive.functions.end(),
                     [address](const auto& candidate) { return candidate.address == address; });
    return found == analysis.recursive.functions.end() ? nullptr : &*found;
}

bool has_runtime_copy_origin(const katana::analysis::FunctionCandidate* candidate) {
    return candidate != nullptr &&
           std::find(candidate->origins.begin(),
                     candidate->origins.end(),
                     katana::analysis::FunctionOrigin::RuntimeCopy) != candidate->origins.end();
}

} // namespace

int main() {
    auto image = copy_image();
    const auto initial_recursive = katana::analysis::analyze_reachable_code(image);
    const auto direct =
        katana::analysis::analyze_runtime_code_copies(image, initial_recursive.instructions);
    require(direct.copies.size() == 1u,
            "Die synthetische bounded Copy-Schleife wurde nicht erkannt.");
    const auto& initial_copy = direct.copies.front();
    require(initial_copy.setup_address == copy_setup && initial_copy.loop_address == copy_loop &&
                initial_copy.source_begin == source_begin &&
                initial_copy.source_end_inclusive == source_end_inclusive &&
                initial_copy.source_byte_count == 12u &&
                initial_copy.destination_vbr_delta == -0x600 && initial_copy.aot_candidates_only &&
                initial_copy.evidence == katana::analysis::ControlFlowEvidence::GuardedPartial,
            "Copy-Grenzen, VBR-Delta oder guarded AOT-Vertrag sind falsch.");
    require(initial_copy.patch_candidates.empty(),
            "Ein noch nicht decodierter Template-Literalslot wurde voreilig als Patch bewiesen.");

    const auto integrated = katana::analysis::analyze_control_flow(image);
    require(integrated.runtime_code_copies.copies.size() == 1u &&
                integrated.runtime_code_copies.copies.front().patch_candidates.size() == 1u,
            "Der Fixpunkt hat den bewiesenen Template-Patch nicht nachgezogen.");
    const auto& patch = integrated.runtime_code_copies.copies.front().patch_candidates.front();
    require(patch.store_instruction_address == 0x1004u && patch.slot_address == patch_slot &&
                patch.live_value == patch_target &&
                patch.target_address == patch_target,
            "Patchslot oder endliches natives Ziel wurde falsch abgeleitet.");

    const auto* source = find_function(integrated, source_begin);
    const auto* handler = find_function(integrated, patch_target);
    require(has_runtime_copy_origin(source) && has_runtime_copy_origin(handler) &&
                source->evidence == katana::analysis::ControlFlowEvidence::GuardedPartial &&
                handler->evidence == katana::analysis::ControlFlowEvidence::GuardedPartial,
            "Source-Root oder Handlerziel verlor RuntimeCopy-Herkunft/Guard.");
    require(
        std::binary_search(integrated.recursive.guarded_candidate_instruction_addresses.begin(),
                           integrated.recursive.guarded_candidate_instruction_addresses.end(),
                           source_begin) &&
            std::binary_search(integrated.recursive.guarded_candidate_instruction_addresses.begin(),
                               integrated.recursive.guarded_candidate_instruction_addresses.end(),
                               patch_target),
        "RuntimeCopy-Seeds wurden nicht als reine AOT-Kandidaten decodiert.");
    require(std::none_of(integrated.resolved_edges.begin(),
                         integrated.resolved_edges.end(),
                         [](const auto& edge) {
                             return edge.target_address == source_begin ||
                                    edge.target_address == patch_target;
                         }),
            "RuntimeCopy-Kandidaten erzeugten eine erfundene Live-CFG-Kante.");
    require(std::string(katana::analysis::function_origin_name(
                katana::analysis::FunctionOrigin::RuntimeCopy)) == "runtime-copy",
            "RuntimeCopy-Herkunft besitzt keinen stabilen Namen.");

    auto wrong_compare = copy_image(false);
    const auto wrong_recursive = katana::analysis::analyze_reachable_code(wrong_compare);
    require(
        katana::analysis::analyze_runtime_code_copies(wrong_compare, wrong_recursive.instructions)
            .copies.empty(),
        "Eine nicht inklusive cmp/hi-Bound-Schleife wurde als Codecopy akzeptiert.");

    auto invalid_target = copy_image(true, 0xDEADBEEFu);
    const auto invalid_analysis = katana::analysis::analyze_control_flow(invalid_target);
    require(invalid_analysis.runtime_code_copies.copies.size() == 1u &&
                invalid_analysis.runtime_code_copies.copies.front().patch_candidates.empty() &&
                find_function(invalid_analysis, 0xDEADBEEFu) == nullptr,
            "Ein nicht ausführbares Patchziel wurde als nativer Handler geseedet.");

    std::cout << "Runtime-Code-Copy-Analysetests erfolgreich.\n";
    return EXIT_SUCCESS;
}
