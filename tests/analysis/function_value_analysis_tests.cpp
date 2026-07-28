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
#include <set>
#include <string>
#include <utility>
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

katana::io::ExecutableImage returned_table_load_image(
    const std::vector<std::uint16_t>& setup_opcodes,
    const std::uint16_t load_opcode,
    const std::vector<std::uint32_t>& returned_addresses,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& table_slots) {
    require(!returned_addresses.empty() && returned_addresses.size() <= 8u,
            "Returned-Table-Testfixture erhielt keine begrenzte Rueckgabemenge.");
    std::vector<std::uint8_t> bytes(0x800u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    const auto put_return = [&](const std::size_t address,
                                const std::uint32_t value) {
        require(value <= 0x7Fu,
                "Returned-Table-Testfixture braucht positive MOV-Immediate-Werte.");
        put_u16(address,
                static_cast<std::uint16_t>(0xE000u | value)); // mov #value,r0
        put_u16(address + 2u, 0x000Bu);                       // rts
        put_u16(address + 4u, 0x0009u);                       // nop (delay)
    };

    put_u16(0x00u, 0xB00Eu); // bsr 0x20
    put_u16(0x02u, 0x0009u); // nop (delay)
    auto cursor = 0x04u;
    for (const auto opcode : setup_opcodes) {
        put_u16(cursor, opcode);
        cursor += 2u;
    }
    require(cursor + 6u <= 0x20u,
            "Returned-Table-Testfixture ueberlappt den Accessor.");
    put_u16(cursor, load_opcode);
    put_u16(cursor + 2u, 0x000Bu); // rts
    put_u16(cursor + 4u, 0x0009u); // nop (delay)

    const auto branch_count = returned_addresses.size() - 1u;
    const auto default_return = 0x20u + branch_count * 2u;
    for (std::size_t index = 0u; index < branch_count; ++index) {
        const auto branch_address = 0x20u + index * 2u;
        const auto target_address = default_return + (index + 1u) * 6u;
        const auto displacement =
            (target_address - (branch_address + 4u)) / 2u;
        require(displacement <= 0x7Fu,
                "Returned-Table-Testfixture ueberschritt den BT-Bereich.");
        put_u16(branch_address,
                static_cast<std::uint16_t>(0x8900u | displacement));
        put_return(target_address, returned_addresses[index]);
    }
    put_return(default_return, returned_addresses.back());

    for (const auto [address, value] : table_slots) {
        require(address <= bytes.size() - 4u,
                "Returned-Table-Testslot liegt ausserhalb des Images.");
        put_u32(address, value);
    }
    std::set<std::uint32_t> handlers;
    for (const auto& [address, value] : table_slots) {
        static_cast<void>(address);
        if (value >= 0xC0u && value <= bytes.size() - 4u)
            handlers.insert(value);
    }
    for (const auto handler : handlers) {
        put_u16(handler, 0x000Bu);
        put_u16(handler + 2u, 0x0009u);
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_initial_snapshot_entry(0u);
    image.add_segment({".returned-table-load",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       std::move(bytes),
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-returned-table-load"});
    image.add_entry_point(0u);
    return image;
}

katana::analysis::FunctionValueAnalysisResult
returned_table_values(const katana::io::ExecutableImage& image) {
    const auto lines =
        katana::sh4::disassemble(image.segments().front().bytes, 0u);
    constexpr std::array<std::uint32_t, 2u> function_entries{0u, 0x20u};
    return katana::analysis::analyze_function_values(
        image, lines, function_entries);
}

const katana::analysis::ReturnedCodeAddressTableCandidate*
returned_table_candidate(
    const katana::analysis::FunctionValueAnalysisResult& analysis,
    const std::uint32_t table_address) {
    const auto found = std::find_if(
        analysis.guarded_code_inventory.returned_code_address_tables.begin(),
        analysis.guarded_code_inventory.returned_code_address_tables.end(),
        [table_address](const auto& candidate) {
            return candidate.table_address == table_address;
        });
    return found == analysis.guarded_code_inventory.returned_code_address_tables.end()
               ? nullptr
               : &*found;
}

katana::analysis::FunctionValueAnalysisResult
incomplete_return_family_values() {
    std::vector<std::uint8_t> bytes(0x60u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0xD105u); // mov.l @(0x18,pc),r1 -> known accessor 0x20
    put_u16(0x02u, 0x410Bu); // jsr @r1 (incomplete family)
    put_u16(0x04u, 0x0009u); // nop (delay)
    put_u16(0x06u, 0x6803u); // mov r0,r8 (preserve semantic provenance)
    put_u16(0x08u, 0x6C03u); // mov r0,r12
    put_u16(0x0Au, 0x63C2u); // mov.l @r12,r3
    put_u16(0x0Cu, 0x430Bu); // jsr @r3
    put_u16(0x0Eu, 0x0009u); // nop (delay)
    put_u16(0x10u, 0x000Bu); // rts
    put_u16(0x12u, 0x0009u); // nop (delay)
    put_u32(0x18u, 0x20u);
    put_u16(0x20u, 0xE040u); // known candidate returns non-stack table 0x40
    put_u16(0x22u, 0x000Bu);
    put_u16(0x24u, 0x0009u);
    put_u32(0x40u, 0x50u);
    put_u16(0x50u, 0x000Bu); // callback
    put_u16(0x52u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.add_segment({".incomplete-return-family",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes,
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-incomplete-return-family"});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<std::uint32_t, 2u> function_entries{0u, 0x20u};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x02u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot}},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, function_entries, edges);
}

katana::analysis::FunctionValueAnalysisResult
shifted_stack_alias_values(const bool isolated_harvest) {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0x7FD4u); // add #-44,r15
    put_u16(0x02u, 0x64F3u); // mov r15,r4
    put_u16(0x04u, 0xE560u); // mov #0x60,r5 (callback)
    put_u16(0x06u, 0xB01Bu); // bsr 0x40
    put_u16(0x08u, 0x0009u); // nop (delay)
    put_u16(0x0Au, 0x7F2Cu); // add #44,r15
    put_u16(0x0Cu, 0x000Bu);
    put_u16(0x0Eu, 0x0009u);
    put_u16(0x40u, 0x2452u); // mov.l r5,@r4
    put_u16(0x42u, 0x2F62u); // mov.l r6,@r15 (same rebased slot)
    put_u16(0x44u, 0x6542u); // mov.l @r4,r5
    put_u16(0x46u, 0xE220u); // mov #0x20,r2
    put_u16(0x48u, 0x2252u); // mov.l r5,@r2
    put_u16(0x4Au, 0x000Bu);
    put_u16(0x4Cu, 0x0009u);
    put_u16(0x60u, 0x000Bu);
    put_u16(0x62u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".shifted-stack-alias",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes,
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-shifted-stack-alias"});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    if (isolated_harvest) {
        image.add_entry_point(0x40u);
        constexpr std::array<std::uint32_t, 2u> function_entries{0u, 0x40u};
        return katana::analysis::analyze_function_values(
            image, lines, function_entries);
    }
    constexpr std::array<std::uint32_t, 1u> function_entries{0u};
    return katana::analysis::analyze_function_values(
        image, lines, function_entries);
}

katana::analysis::FunctionValueAnalysisResult
guarded_inventory_budget_values(const std::size_t candidate_count) {
    constexpr std::size_t record_size = 0x20u;
    constexpr std::uint32_t handler_base = 0x1'0000u;
    std::vector<std::uint8_t> bytes(
        handler_base + candidate_count * 4u, 0x09u);
    std::vector<std::uint32_t> function_entries;
    function_entries.reserve(candidate_count);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    for (std::size_t index = 0u; index < candidate_count; ++index) {
        const auto caller = index * record_size;
        const auto callback =
            handler_base + static_cast<std::uint32_t>(index * 4u);
        function_entries.push_back(static_cast<std::uint32_t>(caller));
        put_u16(caller + 0x00u, 0xD405u); // mov.l @(0x18,pc),r4
        put_u16(caller + 0x02u, 0xB003u); // bsr local registrar +0x0c
        put_u16(caller + 0x04u, 0x0009u);
        put_u16(caller + 0x06u, 0x000Bu);
        put_u16(caller + 0x08u, 0x0009u);
        put_u16(caller + 0x0Cu, 0xE220u); // mov #0x20,r2
        put_u16(caller + 0x0Eu, 0x2242u); // mov.l r4,@r2
        put_u16(caller + 0x10u, 0x000Bu);
        put_u16(caller + 0x12u, 0x0009u);
        put_u32(caller + 0x18u, callback);
        put_u16(callback, 0x000Bu);
        put_u16(callback + 2u, 0x0009u);
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".guarded-inventory-budget",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes,
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-guarded-inventory-budget"});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    return katana::analysis::analyze_function_values(
        image, lines, function_entries);
}

bool has_stored_code_address(
    const katana::analysis::FunctionValueAnalysisResult& analysis,
    const std::uint32_t target) {
    return std::any_of(
        analysis.guarded_code_inventory.stored_code_addresses.begin(),
        analysis.guarded_code_inventory.stored_code_addresses.end(),
        [target](const auto& candidate) {
            return candidate.target_address == target;
        });
}

katana::analysis::FunctionValueAnalysisResult
conditional_shared_tail_values() {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0xE470u); // mov #0x70,r4 (callback)
    put_u16(0x02u, 0xB00Du); // bsr 0x20
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x08u, 0x0009u);
    put_u16(0x20u, 0x412Bu); // jmp @r1 (candidate-only tail)
    put_u16(0x22u, 0x0009u);
    put_u16(0x40u, 0x890Eu); // bt 0x60 (unknown condition)
    put_u16(0x42u, 0x000Bu); // internal path without a store
    put_u16(0x44u, 0x0009u);
    put_u16(0x60u, 0x2742u); // external shared tail: mov.l r4,@r7
    put_u16(0x62u, 0x000Bu);
    put_u16(0x64u, 0x0009u);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".conditional-shared-tail",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u> boundaries{{
        {0x00u, 0x0Au},
        {0x20u, 0x04u},
        {0x60u, 0x06u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x20u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
object_field_tail_values(const bool overwrite_callback_from_object) {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0xE460u); // mov #0x60,r4 (object)
    put_u16(0x02u, 0xE570u); // mov #0x70,r5 (real callback argument)
    put_u16(0x04u, 0xB00Cu); // bsr 0x20
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0x000Bu);
    put_u16(0x0Au, 0x0009u);
    put_u16(0x20u, 0xE004u); // mov #4,r0
    put_u16(0x22u,
            overwrite_callback_from_object
                ? 0x054Eu // mov.l @(r0,r4),r5
                : 0x0009u);
    put_u16(0x24u, 0x462Bu); // jmp @r6 (candidate-only tail)
    put_u16(0x26u, 0x0009u);
    put_u16(0x40u, 0x2752u); // mov.l r5,@r7 (unknown object)
    put_u16(0x42u, 0x000Bu);
    put_u16(0x44u, 0x0009u);
    put_u16(0x60u, 0x000Bu); // object address is also decode-valid
    put_u16(0x62u, 0x0009u);
    put_u32(0x64u, 0x70u);   // ordinary field, coincidentally code-like
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".object-field-tail",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x0Cu},
        {0x20u, 0x08u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x24u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
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

    {
        std::vector<std::uint8_t> bytes(0x26u, 0x09u);
        bytes[0x00u] = 0x2Bu;
        bytes[0x01u] = 0x41u; // jmp @r1
        bytes[0x02u] = 0x09u;
        bytes[0x03u] = 0x00u;
        bytes[0x20u] = 0x2Au;
        bytes[0x21u] = 0xE0u; // mov #42,r0
        bytes[0x22u] = 0x0Bu;
        bytes[0x23u] = 0x00u;
        bytes[0x24u] = 0x09u;
        bytes[0x25u] = 0x00u;
        auto image = classification_image(bytes);
        const auto lines = katana::sh4::disassemble(bytes, 0u);
        constexpr std::array<std::uint32_t, 1u> function_entries{0u};
        const std::array<katana::analysis::ResolvedControlFlowEdge, 2u> edges{{
            {0x00u,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Jump,
             true,
             katana::analysis::ControlFlowEvidence::GuardedComplete,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot}},
            {0x00u,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
        }};
        const auto values = katana::analysis::analyze_function_values(
            image, lines, function_entries, edges);
        const auto owner =
            std::find_if(values.summaries.begin(),
                         values.summaries.end(),
                         [](const auto& candidate) {
                             return candidate.function_address == 0u;
                         });
        require(owner != values.summaries.end(),
                "Candidate-Carrier entfernte die Owner-Funktionssummary.");
        const auto r0 =
            std::find_if(owner->registers.begin(),
                         owner->registers.end(),
                         [](const auto& candidate) {
                             return candidate.register_index == 0u;
                         });
        require(r0 != owner->registers.end() && r0->complete &&
                    r0->values == std::vector<std::uint32_t>{42u},
                "Candidate-Carrier entfernte die reale Jump-Kante mit "
                "identischem Callsite-/Zielpaar.");
    }

    {
        const auto conditional = conditional_shared_tail_values();
        const auto stored =
            std::find_if(
                conditional.guarded_code_inventory.stored_code_addresses.begin(),
                conditional.guarded_code_inventory.stored_code_addresses.end(),
                [](const auto& candidate) {
                    return candidate.target_address == 0x70u;
                });
        require(stored !=
                        conditional.guarded_code_inventory.stored_code_addresses.end() &&
                    stored->guarded &&
                    !conditional.guarded_code_inventory
                         .candidate_inventory_truncated,
                "Bedingter externer Shared-Tail verlor seinen "
                "Callbackstore oder meldete ein falsches vollstaendiges Inventar.");
    }

    {
        const auto direct_argument = object_field_tail_values(false);
        const auto loaded_field = object_field_tail_values(true);
        require(has_stored_code_address(direct_argument, 0x70u) &&
                    !has_stored_code_address(loaded_field, 0x70u),
                "Objektadress-Provenienz wurde weiterhin als Provenienz "
                "des geladenen Feldinhalts behandelt.");
    }

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

    const auto guarded_return_table_image = [] {
        std::vector<std::uint8_t> bytes(0x60u, 0x09u);
        bytes[0x00u] = 0x05u;
        bytes[0x01u] = 0xD1u; // mov.l @(0x18,pc),r1 -> accessor 0x20
        bytes[0x02u] = 0x0Bu;
        bytes[0x03u] = 0x41u; // jsr @r1
        bytes[0x04u] = 0x09u;
        bytes[0x05u] = 0x00u; // nop (delay)
        bytes[0x06u] = 0x03u;
        bytes[0x07u] = 0x6Cu; // mov r0,r12
        bytes[0x08u] = 0xC2u;
        bytes[0x09u] = 0x63u; // mov.l @r12,r3
        bytes[0x0Au] = 0x0Bu;
        bytes[0x0Bu] = 0x43u; // jsr @r3
        bytes[0x0Cu] = 0x09u;
        bytes[0x0Du] = 0x00u; // nop (delay)
        bytes[0x0Eu] = 0x0Bu;
        bytes[0x0Fu] = 0x00u; // rts
        bytes[0x10u] = 0x09u;
        bytes[0x11u] = 0x00u; // nop (delay)
        bytes[0x18u] = 0x20u;
        bytes[0x19u] = 0x00u;
        bytes[0x1Au] = 0x00u;
        bytes[0x1Bu] = 0x00u;
        bytes[0x20u] = 0x02u;
        bytes[0x21u] = 0xD0u; // mov.l @(0x2c,pc),r0 -> table 0x40
        bytes[0x22u] = 0x0Bu;
        bytes[0x23u] = 0x00u; // rts
        bytes[0x24u] = 0x09u;
        bytes[0x25u] = 0x00u; // nop (delay)
        bytes[0x2Cu] = 0x40u;
        bytes[0x2Du] = 0x00u;
        bytes[0x2Eu] = 0x00u;
        bytes[0x2Fu] = 0x00u;
        bytes[0x40u] = 0x50u;
        bytes[0x41u] = 0x00u;
        bytes[0x42u] = 0x00u;
        bytes[0x43u] = 0x00u;
        bytes[0x44u] = 0x54u;
        bytes[0x45u] = 0x00u;
        bytes[0x46u] = 0x00u;
        bytes[0x47u] = 0x00u;
        bytes[0x48u] = 0x58u;
        bytes[0x49u] = 0x00u;
        bytes[0x4Au] = 0x00u;
        bytes[0x4Bu] = 0x00u;
        bytes[0x50u] = 0x0Bu;
        bytes[0x51u] = 0x00u; // handler: rts
        bytes[0x52u] = 0x09u;
        bytes[0x53u] = 0x00u; // nop (delay)
        bytes[0x54u] = 0x0Bu;
        bytes[0x55u] = 0x00u; // sibling handler: rts
        bytes[0x56u] = 0x09u;
        bytes[0x57u] = 0x00u; // nop (delay)
        bytes[0x58u] = 0x0Bu;
        bytes[0x59u] = 0x00u; // sibling handler: rts
        bytes[0x5Au] = 0x09u;
        bytes[0x5Bu] = 0x00u; // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.set_initial_snapshot_entry(0x58u);
        image.add_segment({".guarded-return-table",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-guarded-return-table"});
        image.add_entry_point(0u);
        return image;
    }();
    const auto guarded_return_table =
        katana::analysis::analyze_control_flow(guarded_return_table_image);
    const auto* guarded_table_summary = summary(guarded_return_table, 0x20u, 0u);
    const auto* guarded_accessor_dispatch = site(guarded_return_table, 0x02u);
    const auto* guarded_table_dispatch = site(guarded_return_table, 0x0Au);
    constexpr std::array guarded_family{0x50u, 0x54u, 0x58u};
    require(guarded_table_summary != nullptr && !guarded_table_summary->complete &&
                guarded_table_summary->guarded &&
                guarded_table_summary->values == std::vector<std::uint32_t>{0x40u} &&
                guarded_table_summary->reason == "constant-return-candidate",
            "Endliche Guarded-Partial-Rueckgabe ging in der Funktionssummary verloren.");
    require(guarded_accessor_dispatch != nullptr &&
                guarded_accessor_dispatch->status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                guarded_accessor_dispatch->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                guarded_accessor_dispatch->targets.empty() &&
                guarded_accessor_dispatch->analysis_candidates ==
                    std::vector<std::uint32_t>{0x20u},
            "Indirekter Guarded-Accessor verlor seinen reinen Runtime-Kandidaten.");
    require(guarded_table_dispatch != nullptr &&
                guarded_table_dispatch->status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                guarded_table_dispatch->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                guarded_table_dispatch->targets.empty() &&
                guarded_table_dispatch->analysis_candidates ==
                    std::vector<std::uint32_t>{0x50u} &&
                guarded_table_dispatch->reason == "runtime-contract-function-memory",
            "Guarded-Return-Tabellenload verlor seinen reinen Runtime-Kandidaten.");
    require(std::all_of(guarded_family.begin(),
                        guarded_family.end(),
                        [&](const auto address) {
                            return std::any_of(
                                guarded_return_table.recursive.functions.begin(),
                                guarded_return_table.recursive.functions.end(),
                                [address](const auto& function) {
                                    return function.address == address;
                                });
                        }),
            "Guarded-Return-Tabellenfamilie erreichte das AOT-Inventar nicht vollstaendig.");
    require(std::none_of(guarded_return_table.resolved_edges.begin(),
                         guarded_return_table.resolved_edges.end(),
                         [](const auto& edge) {
                             return (edge.instruction_address == 0x02u &&
                                     edge.target_address == 0x20u) ||
                                    (edge.instruction_address == 0x0Au &&
                                     edge.target_address == 0x50u);
                         }),
            "Guarded-Return-Kandidaten wurden faelschlich zu autoritativen CFG-Kanten.");
    const auto guarded_sibling =
        std::find_if(guarded_return_table.recursive.functions.begin(),
                     guarded_return_table.recursive.functions.end(),
                     [](const auto& function) { return function.address == 0x54u; });
    require(guarded_sibling != guarded_return_table.recursive.functions.end() &&
                guarded_sibling->origins ==
                    std::vector{katana::analysis::FunctionOrigin::GuardedSnapshot},
            "Nicht direkt geladener Tabellenbruder verlor seine Guarded-Snapshot-Herkunft.");

    auto short_return_table_image = guarded_return_table_image;
    short_return_table_image.write_u32_le(0x48u, 1u);
    const auto short_return_table =
        katana::analysis::analyze_control_flow(short_return_table_image);
    require(std::any_of(short_return_table.recursive.functions.begin(),
                        short_return_table.recursive.functions.end(),
                        [](const auto& function) {
                            return function.address == 0x54u &&
                                   function.origins ==
                                       std::vector{katana::analysis::FunctionOrigin::
                                                       GuardedSnapshot};
                        }) &&
                std::none_of(short_return_table.recursive.functions.begin(),
                             short_return_table.recursive.functions.end(),
                             [](const auto& function) {
                                 return function.address == 0x58u;
                             }),
            "Bewiesene Zweiereintragstabelle wurde nicht begrenzt inventarisiert.");

    auto single_return_table_image = short_return_table_image;
    single_return_table_image.write_u32_le(0x44u, 1u);
    const auto single_return_table =
        katana::analysis::analyze_control_flow(single_return_table_image);
    require(std::any_of(single_return_table.recursive.functions.begin(),
                        single_return_table.recursive.functions.end(),
                        [](const auto& function) {
                            return function.address == 0x50u &&
                                   std::find(
                                       function.origins.begin(),
                                       function.origins.end(),
                                       katana::analysis::FunctionOrigin::
                                           GuardedSnapshot) !=
                                       function.origins.end();
                        }) &&
                std::none_of(single_return_table.recursive.functions.begin(),
                             single_return_table.recursive.functions.end(),
                             [](const auto& function) {
                                 return function.address == 0x54u;
                             }),
            "Konkret geladener einzelner Callbackslot erreichte das bewachte AOT-Inventar "
            "nicht.");

    {
        const auto incomplete_family = incomplete_return_family_values();
        const auto accessor = std::find_if(
            incomplete_family.summaries.begin(),
            incomplete_family.summaries.end(),
            [](const auto& candidate) {
                return candidate.function_address == 0x20u;
            });
        require(accessor != incomplete_family.summaries.end(),
                "Incomplete Callee-Familie verlor den bekannten Accessor.");
        const auto accessor_return =
            std::find_if(accessor->registers.begin(),
                         accessor->registers.end(),
                         [](const auto& candidate) {
                             return candidate.register_index == 0u;
                         });
        require(accessor_return != accessor->registers.end() &&
                    accessor_return->values ==
                        std::vector<std::uint32_t>{0x40u} &&
                    !accessor_return->may_alias_stack,
                "Bekannter Accessor verlor seinen Non-Stack-Return.");
        const auto owner = std::find_if(
            incomplete_family.summaries.begin(),
            incomplete_family.summaries.end(),
            [](const auto& candidate) {
                return candidate.function_address == 0u;
            });
        require(owner != incomplete_family.summaries.end(),
                "Incomplete Callee-Familie verlor ihre Caller-Summary.");
        const auto returned =
            std::find_if(owner->registers.begin(),
                         owner->registers.end(),
                         [](const auto& candidate) {
                             return candidate.register_index == 8u;
                         });
        const auto* table =
            returned_table_candidate(incomplete_family, 0x40u);
        const auto dispatch = std::find_if(
            incomplete_family.resolutions.begin(),
            incomplete_family.resolutions.end(),
            [](const auto& candidate) {
                return candidate.instruction_address == 0x0Cu;
            });
        require(returned != owner->registers.end() &&
                    returned->abi_preserved && returned->may_alias_stack,
                "Incomplete Callee-Familie verlor ihr semantisches "
                "Stack-May-Alias am bekannten Non-Stack-Return (may_alias=" +
                    std::to_string(returned->may_alias_stack) +
                    ", abi_preserved=" +
                    std::to_string(returned->abi_preserved) +
                    ").");
        require(owner->memory_values.empty(),
                "Inventory-only Non-Stack-Provenienz erzeugte einen normalen "
                "Memory-Summary-Beweis.");
        require(table != nullptr &&
                    table->target_addresses ==
                        std::vector<std::uint32_t>{0x50u},
                "Separate Inventory-Provenienz verlor den bekannten "
                "Guarded-Table-Seed (tables=" +
                    std::to_string(
                        incomplete_family.guarded_code_inventory
                            .returned_code_address_tables.size()) +
                    ").");
        require(dispatch != incomplete_family.resolutions.end() &&
                    dispatch->guarded && !dispatch->complete &&
                    dispatch->evidence ==
                        katana::analysis::ControlFlowEvidence::GuardedPartial,
                "Inventory-only Non-Stack-Provenienz erzeugte einen normalen "
                "CFG-Beweis.");
    }

    for (const auto isolated_harvest : {false, true}) {
        const auto shifted_stack =
            shifted_stack_alias_values(isolated_harvest);
        require(
            std::none_of(
                shifted_stack.guarded_code_inventory.stored_code_addresses.begin(),
                shifted_stack.guarded_code_inventory.stored_code_addresses.end(),
                [](const auto& candidate) {
                    return candidate.target_address == 0x60u;
                }),
            isolated_harvest
                ? "Isolated Store Harvest lud nach verschobenem Caller-SP einen "
                  "ueberschriebenen Stackslot als Callback."
                : "Candidate-Input-Merge lud nach verschobenem Caller-SP einen "
                  "ueberschriebenen Stackslot als Callback.");
    }

    [] {
        constexpr auto mov_r0_r12 = std::uint16_t{0x6C03u};
        constexpr auto nop = std::uint16_t{0x0009u};
        constexpr auto movt_r0 = std::uint16_t{0x0029u};
        constexpr auto shll2_r0 = std::uint16_t{0x4008u};
        constexpr auto mov_l_at_r12_r3 = std::uint16_t{0x63C2u};
        constexpr auto mov_l_at_r12_post_r3 = std::uint16_t{0x63C6u};
        constexpr auto mov_l_at_4_r12_r3 = std::uint16_t{0x53C1u};
        constexpr auto mov_l_at_r0_r12_r3 = std::uint16_t{0x03CEu};
        constexpr auto mov_l_at_r0_r0_r3 = std::uint16_t{0x030Eu};

        const auto direct = returned_table_values(returned_table_load_image(
            {mov_r0_r12, nop}, mov_l_at_r12_r3, {0x70u}, {{0x70u, 0xC0u}}));
        const auto* direct_table = returned_table_candidate(direct, 0x70u);
        require(direct_table != nullptr &&
                    direct_table->target_addresses ==
                        std::vector<std::uint32_t>{0xC0u} &&
                    direct_table->load_instruction_addresses ==
                        std::vector<std::uint32_t>{0x08u} &&
                    direct_table->evidence_call_sites ==
                        std::vector<std::uint32_t>{0x00u} &&
                    direct_table->evidence_callees ==
                        std::vector<std::uint32_t>{0x20u},
                "MOV.L @Rm verlor den einzelnen provenance-starken Callbackslot.");

        auto direct_with_unknown_ingress_image = returned_table_load_image(
            {mov_r0_r12, nop}, mov_l_at_r12_r3, {0x70u}, {{0x70u, 0xC0u}});
        direct_with_unknown_ingress_image.add_entry_point(0x20u);
        const auto direct_with_unknown_ingress =
            returned_table_values(direct_with_unknown_ingress_image);
        const auto* preserved_direct_table =
            returned_table_candidate(direct_with_unknown_ingress, 0x70u);
        require(preserved_direct_table != nullptr &&
                    preserved_direct_table->target_addresses ==
                        std::vector<std::uint32_t>{0xC0u} &&
                    preserved_direct_table->evidence_call_sites ==
                        std::vector<std::uint32_t>{0x00u},
                "Unbekannter zusaetzlicher Callee-Ingress reduzierte den vorhandenen "
                "Returned-Table-Bestand.");

        const auto post_increment = returned_table_values(
            returned_table_load_image({mov_r0_r12, nop},
                                      mov_l_at_r12_post_r3,
                                      {0x70u},
                                      {{0x70u, 0xC0u}, {0x74u, 0xC4u}}));
        const auto* post_increment_table =
            returned_table_candidate(post_increment, 0x70u);
        require(post_increment_table != nullptr &&
                    post_increment_table->target_addresses ==
                        std::vector<std::uint32_t>({0xC0u, 0xC4u}),
                "MOV.L @Rm+ verwendete nicht den alten Basiswert fuer die "
                "Zweierslottabelle.");

        const auto displaced = returned_table_values(returned_table_load_image(
            {mov_r0_r12, nop},
            mov_l_at_4_r12_r3,
            {0x6Cu},
            {{0x70u, 0xC0u}}));
        require(returned_table_candidate(displaced, 0x70u) != nullptr &&
                    returned_table_candidate(displaced, 0x6Cu) == nullptr,
                "MOV.L @(disp,Rm) addierte den skalierten Decoder-Displacement nicht "
                "exakt.");

        const auto indexed = returned_table_values(returned_table_load_image(
            {mov_r0_r12, movt_r0, shll2_r0},
            mov_l_at_r0_r12_r3,
            {0x60u, 0x70u},
            {{0x60u, 0xC0u},
             {0x64u, 0xC4u},
             {0x70u, 0xC8u},
             {0x74u, 0xCCu}}));
        for (const auto address : {0x60u, 0x64u, 0x70u, 0x74u}) {
            const auto* table = returned_table_candidate(indexed, address);
            require(table != nullptr &&
                        table->evidence_call_sites ==
                            std::vector<std::uint32_t>{0x00u} &&
                        table->evidence_callees ==
                            std::vector<std::uint32_t>{0x20u},
                    "MOV.L @(R0,Rm) verlor eine endliche kartesische "
                    "Effektivadresse.");
        }

        const auto same_register = returned_table_values(
            returned_table_load_image({nop, nop},
                                      mov_l_at_r0_r0_r3,
                                      {0x38u, 0x3Cu},
                                      {{0x70u, 0xC0u},
                                       {0x74u, 0xC4u},
                                       {0x78u, 0xC8u}}));
        for (const auto address : {0x70u, 0x78u}) {
            require(returned_table_candidate(same_register, address) != nullptr,
                    "MOV.L @(R0,R0) verlor eine korrelierte Selbstsumme.");
        }
        require(returned_table_candidate(same_register, 0x74u) == nullptr,
                "MOV.L @(R0,R0) erfand die unmoegliche kartesische Summe x+y.");

        const auto sparse = returned_table_values(returned_table_load_image(
            {mov_r0_r12, nop},
            mov_l_at_r12_r3,
            {0x70u},
            {{0x70u, 0u},
             {0x74u, 0xC0u},
             {0x78u, 0xC4u},
             {0x7Cu, 1u},
             {0x80u, 0xC8u}}));
        const auto* sparse_table = returned_table_candidate(sparse, 0x70u);
        require(sparse_table != nullptr &&
                    sparse_table->target_addresses ==
                        std::vector<std::uint32_t>({0xC0u, 0xC4u, 0xC8u}),
                "Eng begrenzte Null-/Reservierungsslots kappten die "
                "provenance-starke Callbacktabelle.");

        const auto excessive_gap = returned_table_values(
            returned_table_load_image({mov_r0_r12, nop},
                                      mov_l_at_r12_r3,
                                      {0x70u},
                                      {{0x70u, 0u},
                                       {0x74u, 1u},
                                       {0x78u, 3u},
                                       {0x7Cu, 0xC0u}}));
        require(excessive_gap.guarded_code_inventory.returned_code_address_tables.empty(),
                "Drei aufeinanderfolgende unbelegte Slots wurden ueber das enge "
                "Callbackfenster hinweg geraten.");

        std::vector<std::pair<std::uint32_t, std::uint32_t>> twelve_slots;
        std::vector<std::uint32_t> twelve_targets;
        for (std::uint32_t index = 0u; index < 12u; ++index) {
            const auto target = 0x400u + index * 4u;
            twelve_slots.emplace_back(0x70u + index * 4u, target);
            twelve_targets.push_back(target);
        }
        const auto twelve_entry_table = returned_table_values(
            returned_table_load_image(
                {mov_r0_r12, nop}, mov_l_at_r12_r3, {0x70u}, twelve_slots));
        const auto* twelve_entry_candidate =
            returned_table_candidate(twelve_entry_table, 0x70u);
        require(twelve_entry_candidate != nullptr &&
                    twelve_entry_candidate->target_addresses == twelve_targets &&
                    !twelve_entry_candidate->scan_truncated,
                "Returned-Callbacktabellen blieben auf acht Slots begrenzt.");

        std::vector<std::pair<std::uint32_t, std::uint32_t>> sixty_five_slots;
        for (std::uint32_t index = 0u; index < 65u; ++index)
            sixty_five_slots.emplace_back(0x70u + index * 4u, 0x400u + index * 4u);
        const auto bounded_table = returned_table_values(
            returned_table_load_image(
                {mov_r0_r12, nop}, mov_l_at_r12_r3, {0x70u}, sixty_five_slots));
        const auto* bounded_candidate = returned_table_candidate(bounded_table, 0x70u);
        require(bounded_candidate != nullptr &&
                    bounded_candidate->target_addresses.size() == 64u &&
                    bounded_candidate->scan_truncated &&
                    bounded_table.guarded_code_inventory.table_scan_truncated,
                "Begrenzter Returned-Tabellenscan meldete seine Truncation nicht "
                "maschinenlesbar.");

        const std::vector<std::uint32_t> too_many_returned_bases{
            0x60u, 0x64u, 0x68u, 0x6Cu, 0x70u, 0x74u, 0x78u, 0x7Cu};
        const auto excessive_cartesian = returned_table_values(
            returned_table_load_image({mov_r0_r12, movt_r0, shll2_r0},
                                      mov_l_at_r0_r12_r3,
                                      too_many_returned_bases,
                                      {{0x60u, 0xC0u}}));
        require(
            excessive_cartesian.guarded_code_inventory.returned_code_address_tables.empty(),
                "Kartesische Effektivadressen ueberschritten das bestehende "
                "Achtkandidatenlimit.");

        std::vector<std::uint8_t> local_bytes(0xE0u, 0x09u);
        const auto put_local_u16 = [&local_bytes](const std::size_t offset,
                                                  const std::uint16_t value) {
            local_bytes[offset] = static_cast<std::uint8_t>(value);
            local_bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_local_u32 = [&local_bytes](const std::size_t offset,
                                                  const std::uint32_t value) {
            local_bytes[offset] = static_cast<std::uint8_t>(value);
            local_bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            local_bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            local_bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        put_local_u16(0x00u, 0xEC70u); // mov #0x70,r12
        put_local_u16(0x02u, mov_l_at_r12_r3);
        put_local_u16(0x04u, 0x000Bu);
        put_local_u16(0x06u, nop);
        put_local_u32(0x70u, 0xC0u);
        put_local_u16(0xC0u, 0x000Bu);
        put_local_u16(0xC2u, nop);
        katana::io::ExecutableImage local_image;
        local_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        local_image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        local_image.add_segment({".local-table",
                                 0u,
                                 0u,
                                 local_bytes.size(),
                                 katana::io::SegmentKind::Mixed,
                                 {true, true, true},
                                 std::move(local_bytes),
                                 katana::io::ImageSourceKind::DiscBootFile,
                                 katana::io::ImageLoadPhase::Initial,
                                 "synthetic-local-table"});
        local_image.add_entry_point(0u);
        const auto local_lines =
            katana::sh4::disassemble(local_image.segments().front().bytes, 0u);
        constexpr std::array<std::uint32_t, 1u> local_entries{0u};
        const auto local_values = katana::analysis::analyze_function_values(
            local_image, local_lines, local_entries);
        require(local_values.guarded_code_inventory.returned_code_address_tables.empty(),
                "Lokaler Tabellenzeiger ohne Call-/Return-Provenienz wurde als "
                "Returned-Callbacktabelle akzeptiert.");
    }();

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
                multi_json.find("\"function_value_summaries\":[") != std::string::npos &&
                multi_json.find("\"candidate_inventory_truncated\":false") !=
                    std::string::npos &&
                multi_json.find("\"returned_table_scan_truncated\":false") !=
                    std::string::npos,
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
    require(unknown_caller_site != nullptr &&
                !katana::analysis::control_flow_evidence_complete(
                    unknown_caller_site->evidence) &&
                !unknown_caller_site->target.has_value() &&
                unknown_caller_site->targets.empty() &&
                unknown_caller_site->analysis_candidates.empty() &&
                std::none_of(unknown_caller.resolved_edges.begin(),
                             unknown_caller.resolved_edges.end(),
                             [](const auto& edge) {
                                 return edge.instruction_address == 0x22u &&
                                        edge.target_address == 0x50u;
                             }),
            "Partielle Callerwerte veraenderten trotz unbekanntem Ingress "
            "globale Resolutions oder CFG-Kanten.");

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

    {
        constexpr std::size_t inventory_budget = 1'024u;
        constexpr std::uint32_t handler_base = 0x1'0000u;
        const auto exact_budget =
            guarded_inventory_budget_values(inventory_budget);
        require(
            exact_budget.guarded_code_inventory.candidate_budget ==
                    inventory_budget &&
                exact_budget.guarded_code_inventory.candidate_count ==
                    inventory_budget &&
                !exact_budget.guarded_code_inventory
                     .candidate_inventory_truncated &&
                exact_budget.guarded_code_inventory.stored_code_addresses
                        .size() == inventory_budget &&
                std::all_of(
                    exact_budget.guarded_code_inventory.stored_code_addresses
                        .begin(),
                    exact_budget.guarded_code_inventory.stored_code_addresses
                        .end(),
                    [](const auto& candidate) { return candidate.guarded; }),
            "Exakt 1.024 eindeutige Guarded-Code-Ziele verletzten Budget, "
            "Zaehler oder Guard-Vertrag.");

        const auto over_budget =
            guarded_inventory_budget_values(inventory_budget + 1u);
        const auto& retained =
            over_budget.guarded_code_inventory.stored_code_addresses;
        const auto deterministic_prefix =
            retained.size() == inventory_budget &&
            std::all_of(
                retained.begin(),
                retained.end(),
                [&](const auto& candidate) {
                    const auto index = static_cast<std::size_t>(
                        &candidate - retained.data());
                    return candidate.target_address ==
                           handler_base +
                               static_cast<std::uint32_t>(index * 4u);
                });
        require(
            over_budget.guarded_code_inventory.candidate_budget ==
                    inventory_budget &&
                over_budget.guarded_code_inventory.candidate_count ==
                    inventory_budget &&
                over_budget.guarded_code_inventory
                    .candidate_inventory_truncated &&
                deterministic_prefix &&
                std::none_of(
                    retained.begin(),
                    retained.end(),
                    [](const auto& candidate) {
                        return candidate.target_address ==
                               handler_base +
                                   static_cast<std::uint32_t>(
                                       inventory_budget * 4u);
                    }),
            "Das 1.025. eindeutige Guarded-Code-Ziel wurde nicht waehrend "
            "der Sammlung mit korrekter Truncation und stabilem Prefix "
            "abgewiesen.");
    }

    std::cout << "KR-4713 interprozedurale Zielwertsummaries erfolgreich.\n";
    return EXIT_SUCCESS;
}
