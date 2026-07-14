#include "katana/sh4/disassembler.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <string>

namespace katana::sh4 {

std::vector<DisassemblyLine> disassemble(
    const std::span<const std::uint8_t> bytes,
    const std::uint32_t base_address
) {
    if ((bytes.size() % 2u) != 0u) {
        throw std::invalid_argument(
            "Die BinÃ¤rdatei besitzt eine ungerade Anzahl Bytes."
        );
    }

    std::vector<DisassemblyLine> lines;
    lines.reserve(bytes.size() / 2u);

    bool previous_instruction_has_delay_slot = false;

    for (std::size_t offset = 0; offset < bytes.size(); offset += 2u) {
        const auto expanded_address =
            static_cast<std::uint64_t>(base_address) +
            static_cast<std::uint64_t>(offset);

        if (
            expanded_address >
            std::numeric_limits<std::uint32_t>::max()
        ) {
            throw std::overflow_error(
                "Die berechnete Instruktionsadresse Ã¼berschreitet 32 Bit."
            );
        }

        const auto opcode = katana::io::read_u16_le(bytes, offset);

        DisassemblyLine line;
        line.address = static_cast<std::uint32_t>(expanded_address);
        line.opcode = opcode;
        line.instruction = decode(opcode);
        line.is_delay_slot = previous_instruction_has_delay_slot;
        line.target_address = calculate_direct_branch_target(
            line.instruction,
            line.address
        );

        previous_instruction_has_delay_slot =
            line.instruction.has_delay_slot;

        lines.push_back(line);
    }

    return lines;
}

std::vector<DisassemblyLine> disassemble(const katana::io::ExecutableImage& image) {
    std::vector<DisassemblyLine> lines;
    for (const auto& segment : image.segments()) {
        if (segment.kind != katana::io::SegmentKind::Code
            || !segment.permissions.executable) {
            continue;
        }
        try {
            auto segment_lines = disassemble(segment.bytes, segment.virtual_address);
            lines.insert(
                lines.end(),
                std::make_move_iterator(segment_lines.begin()),
                std::make_move_iterator(segment_lines.end())
            );
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Disassembly von Segment " + segment.name + " fehlgeschlagen: " + error.what()
            );
        }
    }
    return lines;
}

}
