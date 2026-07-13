#include "katana/sh4/disassembler.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace katana::sh4 {

std::vector<DisassemblyLine> disassemble(
    const std::span<const std::uint8_t> bytes,
    const std::uint32_t base_address
) {
    if ((bytes.size() % 2u) != 0u) {
        throw std::invalid_argument(
            "Die Binärdatei besitzt eine ungerade Anzahl Bytes."
        );
    }

    std::vector<DisassemblyLine> lines;
    lines.reserve(bytes.size() / 2u);

    for (std::size_t offset = 0; offset < bytes.size(); offset += 2u) {
        const auto expanded_address =
            static_cast<std::uint64_t>(base_address) +
            static_cast<std::uint64_t>(offset);

        if (
            expanded_address >
            std::numeric_limits<std::uint32_t>::max()
        ) {
            throw std::overflow_error(
                "Die berechnete Instruktionsadresse überschreitet 32 Bit."
            );
        }

        const auto opcode = katana::io::read_u16_le(bytes, offset);

        DisassemblyLine line;
        line.address = static_cast<std::uint32_t>(expanded_address);
        line.opcode = opcode;
        line.instruction = decode(opcode);

        lines.push_back(std::move(line));
    }

    return lines;
}

}
