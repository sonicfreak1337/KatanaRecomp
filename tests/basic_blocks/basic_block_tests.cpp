#include "katana/analysis/basic_blocks.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <array>
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

} // namespace

int main() {
    constexpr std::array<std::uint8_t, 20> bytes = {0x02, 0x89, 0x01, 0xE0, 0x03, 0xA0, 0x09,
                                                    0x00, 0x01, 0x70, 0x0B, 0x00, 0x09, 0x00,
                                                    0x09, 0x00, 0x0B, 0x00, 0x09, 0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    const auto blocks = katana::analysis::build_basic_blocks(lines);

    require(blocks.size() == 4, "Es wurden nicht genau vier Basic Blocks erkannt.");

    require(blocks[0].start_address == 0x8C010000u, "Block 0 besitzt eine falsche Startadresse.");
    require(blocks[0].end_address == 0x8C010000u, "Block 0 besitzt eine falsche Endadresse.");
    require(blocks[0].successors.size() == 2, "Block 0 muss zwei Nachfolger besitzen.");
    require(blocks[0].successors[0] == 0x8C010002u, "Der Fallthrough von Block 0 ist falsch.");
    require(blocks[0].successors[1] == 0x8C010008u, "Das Sprungziel von Block 0 ist falsch.");

    require(blocks[1].start_address == 0x8C010002u, "Block 1 besitzt eine falsche Startadresse.");
    require(blocks[1].end_address == 0x8C010006u, "Block 1 muss BRA und Delay Slot enthalten.");
    require(blocks[1].lines.size() == 3, "Block 1 besitzt die falsche Instruktionsanzahl.");
    require(blocks[1].successors.size() == 1, "Block 1 muss genau einen Nachfolger besitzen.");
    require(blocks[1].successors[0] == 0x8C01000Eu, "Das BRA-Ziel von Block 1 ist falsch.");

    require(blocks[2].start_address == 0x8C010008u, "Block 2 besitzt eine falsche Startadresse.");
    require(blocks[2].end_address == 0x8C01000Cu, "Block 2 muss RTS und Delay Slot enthalten.");
    require(blocks[2].successors.empty(),
            "Ein RTS-Block darf keinen direkten Nachfolger besitzen.");

    require(blocks[3].start_address == 0x8C01000Eu, "Block 3 besitzt eine falsche Startadresse.");
    require(blocks[3].end_address == 0x8C010012u, "Block 3 muss RTS und Delay Slot enthalten.");
    require(blocks[3].successors.empty(), "Der letzte RTS-Block darf keinen Nachfolger besitzen.");

    auto gap_lines = katana::sh4::disassemble(
        std::array<std::uint8_t, 10u>{
            0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u},
        0u);
    gap_lines.erase(gap_lines.begin() + 1);
    const auto gap_blocks = katana::analysis::build_basic_blocks(gap_lines);
    require(gap_blocks.size() == 3u && gap_blocks[0].start_address == 0u &&
                gap_blocks[0].successors.empty(),
            "Eine Adressluecke wurde als normaler Fallthrough interpretiert.");

    auto call_gap_lines = katana::sh4::disassemble(
        std::array<std::uint8_t, 12u>{
            0x02u, 0xB0u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u},
        0u);
    call_gap_lines.erase(call_gap_lines.begin() + 2);
    const auto call_gap_blocks = katana::analysis::build_basic_blocks(call_gap_lines);
    require(!call_gap_blocks.empty() && call_gap_blocks.front().end_address == 2u &&
                call_gap_blocks.front().successors.empty(),
            "Ein BSR mit Luecke nach dem Delay Slot erzeugte einen falschen Rueckkehrfallthrough.");

    const auto indirect_lines = katana::sh4::disassemble(
        std::array<std::uint8_t, 12u>{
            0x2Bu, 0x41u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u},
        0u);
    const std::array partial_edges{
        katana::analysis::ResolvedControlFlowEdge{
            0u,
            4u,
            katana::analysis::ResolvedControlFlowKind::Jump,
            false,
            katana::analysis::ControlFlowEvidence::ProvenComplete,
            {katana::analysis::AnalysisEvidenceOrigin::LocalValue}},
        katana::analysis::ResolvedControlFlowEdge{
            0u,
            8u,
            katana::analysis::ResolvedControlFlowKind::Jump,
            true,
            katana::analysis::ControlFlowEvidence::GuardedPartial,
            {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot}},
    };
    const auto partial_blocks = katana::analysis::build_basic_blocks(indirect_lines, partial_edges);
    require(partial_blocks.front().successors == std::vector<std::uint32_t>({4u, 8u}) &&
                partial_blocks.front().has_indirect_successor,
            "Eine einzelne vollstaendige Kante entfernte den partiellen Site-Default.");

    for (std::uint32_t mask = 0u; mask < 8u; ++mask) {
        std::array<std::uint8_t, 10u> cfg_bytes{
            0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u};
        constexpr std::array<std::uint8_t, 3u> displacements{1u, 0u, 0xFFu};
        for (std::size_t node = 0u; node < 3u; ++node) {
            if ((mask & (1u << node)) == 0u) continue;
            cfg_bytes[node * 2u] = displacements[node];
            cfg_bytes[node * 2u + 1u] = 0x89u;
        }
        const auto cfg_lines = katana::sh4::disassemble(cfg_bytes, 0u);
        const auto cfg_blocks = katana::analysis::build_basic_blocks(cfg_lines);
        for (const auto& block : cfg_blocks) {
            const auto control_address = block.lines.back().is_delay_slot && block.lines.size() > 1u
                                             ? block.lines[block.lines.size() - 2u].address
                                             : block.lines.back().address;
            std::vector<std::uint32_t> expected;
            if (control_address < 6u) {
                const auto node = control_address / 2u;
                if ((mask & (1u << node)) != 0u) expected.push_back(6u);
                const auto fallthrough = control_address + 2u;
                if (std::any_of(cfg_blocks.begin(), cfg_blocks.end(), [&](const auto& candidate) {
                        return candidate.start_address == fallthrough;
                    }))
                    expected.push_back(fallthrough);
            }
            std::sort(expected.begin(), expected.end());
            expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
            require(block.successors == expected,
                    "Exhaustive kleine Conditional-CFG weicht vom Referenzgraphen ab.");
        }
    }

    std::cout << "Alle Basic-Block- und CFG-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
