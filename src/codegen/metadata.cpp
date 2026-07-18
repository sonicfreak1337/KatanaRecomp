#include "katana/codegen/metadata.hpp"

#include "katana/runtime/abi.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace katana::codegen {
namespace {

bool semantic_identifier(const std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_' ||
               character == '.';
    });
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string hex16(const std::uint16_t value) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(4) << value;
    return output.str();
}

std::string_view provenance_name(const BlockProvenance value) {
    switch (value) {
    case BlockProvenance::ImageSegment:
        return "image-segment";
    case BlockProvenance::RomToRamCopy:
        return "rom-to-ram-copy";
    case BlockProvenance::FallbackDecode:
        return "fallback-decode";
    case BlockProvenance::RuntimeWrite:
        return "runtime-write";
    }
    throw std::invalid_argument("Unbekannte Blockprovenienz.");
}

std::string_view end_name(const katana::runtime::BlockEndKind value) {
    using katana::runtime::BlockEndKind;
    switch (value) {
    case BlockEndKind::Fallthrough:
        return "fallthrough";
    case BlockEndKind::StaticBranch:
        return "static-branch";
    case BlockEndKind::ConditionalBranch:
        return "conditional-branch";
    case BlockEndKind::DynamicBranch:
        return "dynamic-branch";
    case BlockEndKind::Call:
        return "call";
    case BlockEndKind::Return:
        return "return";
    case BlockEndKind::ExceptionReturn:
        return "exception-return";
    case BlockEndKind::Sleep:
        return "sleep";
    case BlockEndKind::Exception:
        return "exception";
    case BlockEndKind::InterruptSafepoint:
        return "interrupt-safepoint";
    }
    throw std::invalid_argument("Unbekannter Blockendtyp.");
}

} // namespace

std::string serialize_block_metadata(const std::span<const BlockMetadata> blocks,
                                     const std::string_view backend_name,
                                     const std::uint32_t backend_abi) {
    if (!semantic_identifier(backend_name) || backend_abi == 0u) {
        throw std::invalid_argument("Blockmetadaten brauchen Backendname und ABI.");
    }
    std::vector<BlockMetadata> ordered(blocks.begin(), blocks.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.address.virtual_address != right.address.virtual_address) {
            return left.address.virtual_address < right.address.virtual_address;
        }
        return left.address.physical_address < right.address.physical_address;
    });
    for (std::size_t index = 0u; index < ordered.size(); ++index) {
        auto& block = ordered[index];
        if (!semantic_identifier(block.source_segment) || block.byte_size == 0u ||
            block.guest_opcodes.empty() ||
            block.byte_size != block.guest_opcodes.size() * sizeof(std::uint16_t) ||
            block.estimated_guest_cycles == 0u) {
            throw std::invalid_argument(
                "Blockmetadaten enthalten unvollstaendige Groessen oder Provenienz.");
        }
        if (index != 0u && ordered[index - 1u].address == block.address) {
            throw std::invalid_argument("Blockmetadaten enthalten eine doppelte Gastidentitaet.");
        }
        std::sort(block.direct_successors.begin(),
                  block.direct_successors.end(),
                  [](const auto& left, const auto& right) {
                      if (left.virtual_address != right.virtual_address) {
                          return left.virtual_address < right.virtual_address;
                      }
                      return left.physical_address < right.physical_address;
                  });
    }

    std::ostringstream output;
    output << "{\"schema_version\":" << block_metadata_schema_version
           << ",\"runtime_abi\":" << katana::runtime::abi_version << ",\"backend\":\""
           << backend_name << "\",\"backend_abi\":" << backend_abi << ",\"blocks\":[";
    for (std::size_t index = 0u; index < ordered.size(); ++index) {
        const auto& block = ordered[index];
        if (index != 0u) {
            output << ',';
        }
        output << "{\"virtual_address\":\"" << hex32(block.address.virtual_address)
               << "\",\"physical_address\":\"" << hex32(block.address.physical_address)
               << "\",\"source_segment\":\"" << block.source_segment
               << "\",\"source_byte_offset\":" << block.source_byte_offset
               << ",\"byte_size\":" << block.byte_size << ",\"provenance\":\""
               << provenance_name(block.provenance) << "\",\"guest_opcodes\":[";
        for (std::size_t opcode = 0u; opcode < block.guest_opcodes.size(); ++opcode) {
            if (opcode != 0u) {
                output << ',';
            }
            output << '"' << hex16(block.guest_opcodes[opcode]) << '"';
        }
        output << "],\"estimated_guest_cycles\":" << block.estimated_guest_cycles
               << ",\"end_kind\":\"" << end_name(block.end_kind) << "\",\"direct_successors\":[";
        for (std::size_t successor = 0u; successor < block.direct_successors.size(); ++successor) {
            if (successor != 0u) {
                output << ',';
            }
            output << "{\"virtual_address\":\""
                   << hex32(block.direct_successors[successor].virtual_address)
                   << "\",\"physical_address\":\""
                   << hex32(block.direct_successors[successor].physical_address) << "\"}";
        }
        output << "],\"state_guards\":" << block.state_guards << '}';
    }
    output << "]}\n";
    return output.str();
}

std::vector<ProjectArtifact>
make_separated_codegen_artifacts(std::vector<ProjectArtifact> code_units,
                                 std::string constants,
                                 std::string symbols,
                                 std::string runtime_metadata) {
    for (auto& unit : code_units) {
        unit.relative_path = std::filesystem::path("code") / unit.relative_path.filename();
    }
    code_units.push_back({"data/constants.bin", std::move(constants)});
    code_units.push_back({"symbols/symbols.json", std::move(symbols)});
    code_units.push_back({"metadata/blocks.json", std::move(runtime_metadata)});
    return code_units;
}

} // namespace katana::codegen
