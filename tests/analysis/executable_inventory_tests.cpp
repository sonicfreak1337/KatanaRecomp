#include "katana/analysis/code_address.hpp"
#include "katana/analysis/executable_inventory.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::size_t index(const katana::analysis::ExecutableByteClass value) {
    return static_cast<std::size_t>(value);
}

std::size_t index(const katana::analysis::PrecompileClass value) {
    return static_cast<std::size_t>(value);
}

std::size_t index(const katana::analysis::MixedRangeRole value) {
    return static_cast<std::size_t>(value);
}

std::size_t index(const katana::analysis::RangeProofClass value) {
    return static_cast<std::size_t>(value);
}

} // namespace

int main() {
    using namespace katana;
    std::vector<std::uint8_t> bytes(128u, 0x00u);
    std::fill(bytes.begin(), bytes.begin() + 96, std::uint8_t{0x5a});
    bytes[0] = 0x09u;
    bytes[1] = 0x00u;
    bytes[2] = 0x09u;
    bytes[3] = 0x00u;
    bytes[4] = 0x09u;
    bytes[5] = 0x00u;
    std::fill(bytes.begin() + 32, bytes.begin() + 48, std::uint8_t{0x00});
    const auto write_u32 = [&](const std::size_t offset, const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
    };
    write_u32(64u, 0x1000u);
    write_u32(68u, 0x1002u);
    write_u32(72u, 0x1004u);

    io::ExecutableImage image("private/source/path/disc.bin");
    io::ImageSegment segment{
        "boot", 0x1000u, 0u, bytes.size(), io::SegmentKind::Code, {true, true, true}, bytes};
    segment.source_kind = io::ImageSourceKind::DiscBootFile;
    segment.kind = io::SegmentKind::Mixed;
    segment.local_source_name = "disc.bin";
    image.add_segment(std::move(segment));
    image.add_entry_point(0x1004u);
    require(analysis::validate_committed_code_address(image, 0x1000u).valid(),
            "Mixed Bootsegment verweigert einen bewiesenen Decodekandidaten.");
    image.add_symbol({"object", 0x100cu, 4u, io::SymbolKind::Object, io::SymbolBinding::Local});

    analysis::ControlFlowAnalysisResult control_flow;
    control_flow.recursive.instructions.push_back(
        {0x1000u, 0x0009u, sh4::DecodedInstruction{0x0009u, sh4::InstructionKind::Nop}});
    control_flow.recursive.instructions.push_back(
        {0x1002u, 0x0009u, sh4::DecodedInstruction{0x0009u, sh4::InstructionKind::Nop}});
    sh4::DecodedInstruction mova{0xc70eu, sh4::InstructionKind::MoveAddressPcRelative};
    mova.displacement = 0x38;
    control_flow.recursive.instructions.push_back({0x1004u, 0xc70eu, mova});
    control_flow.recursive.proven_instruction_addresses.push_back(0x1000u);
    control_flow.recursive.guarded_candidate_instruction_addresses.push_back(0x1002u);
    analysis::JumpTableAnalysis table;
    table.encoding = analysis::JumpTableEncoding::Absolute32;
    table.entries.push_back({0u, 0x1010u, 0x1000u, true, "synthetic"});
    control_flow.jump_tables.push_back(table);

    const auto inventory = analysis::build_executable_byte_inventory(image, control_flow);
    require(
        inventory.committed_executable_bytes == 128u &&
            inventory.byte_counts[index(analysis::ExecutableByteClass::ProvenReachableCode)] ==
                4u &&
            inventory.byte_counts[index(analysis::ExecutableByteClass::RuntimeDiscoveredCode)] ==
                2u &&
            inventory.byte_counts[index(analysis::ExecutableByteClass::EmbeddedData)] == 4u &&
            inventory.byte_counts[index(analysis::ExecutableByteClass::JumpTable)] == 4u &&
            inventory.byte_counts[index(analysis::ExecutableByteClass::PointerTable)] == 12u &&
            inventory.byte_counts[index(analysis::ExecutableByteClass::Padding)] == 48u &&
            inventory.byte_counts[index(analysis::ExecutableByteClass::UnknownExecutable)] == 54u,
        "Byteinventar klassifiziert synthetische Bereiche nicht disjunkt und exakt.");
    require(inventory.precompile_counts[index(analysis::PrecompileClass::InitiallyReachable)] ==
                    6u &&
                inventory.precompile_counts[index(analysis::PrecompileClass::NeverExecutedData)] ==
                    52u &&
                inventory.precompile_counts[index(analysis::PrecompileClass::Unknown)] == 70u &&
                inventory.role_counts[index(analysis::MixedRangeRole::PointerTable)] == 12u &&
                inventory.proof_counts[index(analysis::RangeProofClass::Candidate)] == 16u,
            "Byteinventar trennt Vorabkompilierung und nie ausgefuehrte Daten nicht.");

    const auto internal_padding =
        std::find_if(inventory.ranges.begin(), inventory.ranges.end(), [](const auto& range) {
            return range.address == 0x1020u;
        });
    const auto trailing_padding =
        std::find_if(inventory.ranges.begin(), inventory.ranges.end(), [](const auto& range) {
            return range.address == 0x1060u;
        });
    require(internal_padding != inventory.ranges.end() &&
                internal_padding->proof == analysis::RangeProofClass::Candidate &&
                internal_padding->precompile_class == analysis::PrecompileClass::Unknown &&
                trailing_padding != inventory.ranges.end() &&
                trailing_padding->proof == analysis::RangeProofClass::Proven &&
                trailing_padding->precompile_class == analysis::PrecompileClass::NeverExecutedData,
            "Inneres Fuellmaterial darf nicht wie bewiesenes, ausgerichtetes Endpadding gelten.");
    const auto random_data =
        std::find_if(inventory.ranges.begin(), inventory.ranges.end(), [](const auto& range) {
            return range.address <= 0x1018u && 0x1018u < range.address + range.size;
        });
    require(random_data != inventory.ranges.end() &&
                random_data->role == analysis::MixedRangeRole::Unknown,
            "Zufaellig decodierbare Daten wurden ohne Kontrollflussevidenz als Code eingestuft.");

    const auto public_json = analysis::format_executable_inventory_json(image, inventory, false);
    const auto local_json = analysis::format_executable_inventory_json(image, inventory, true);
    require(public_json.find("disc.bin") == std::string::npos &&
                public_json.find("\"address\"") == std::string::npos &&
                public_json.find("\"source_kind\":\"disc_boot_file\"") != std::string::npos &&
                public_json.find("\"zero_fill_bytes\":0") != std::string::npos &&
                public_json.find("\"entropy_bucket\"") != std::string::npos &&
                public_json.find("\"decode_density_bucket\"") != std::string::npos &&
                public_json.find("\"classified_bytes\"") != std::string::npos &&
                local_json.find("disc.bin") != std::string::npos &&
                local_json.find("\"ranges\"") != std::string::npos &&
                local_json.find("\"pages\"") != std::string::npos &&
                local_json.find("\"file_offset\"") != std::string::npos &&
                local_json.find("\"static_references\"") != std::string::npos &&
                local_json.find("\"reason\"") != std::string::npos,
            "Adressfreier Bericht und lokale Details sind nicht sauber getrennt.");

    std::cout << "KR-4704 executable byte inventory regression passed.\n";
    return EXIT_SUCCESS;
}
