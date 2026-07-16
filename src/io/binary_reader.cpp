#include "katana/io/binary_reader.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace katana::io {

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);

    if (!input) {
        throw std::runtime_error("Binärdatei konnte nicht geöffnet werden: " + path.string());
    }

    const auto end_position = input.tellg();

    if (end_position < 0) {
        throw std::runtime_error("Größe der Binärdatei konnte nicht bestimmt werden.");
    }

    const auto size = static_cast<std::size_t>(end_position);

    std::vector<std::uint8_t> bytes(size);

    input.seekg(0, std::ios::beg);

    if (size > 0) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));

        if (!input) {
            throw std::runtime_error("Binärdatei konnte nicht vollständig gelesen werden.");
        }
    }

    return bytes;
}

std::uint16_t read_u16_le(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset >= bytes.size() || bytes.size() - offset < 2) {
        throw std::out_of_range("Für einen 16-Bit-Wert sind nicht genügend Bytes vorhanden.");
    }

    const auto low = static_cast<std::uint16_t>(bytes[offset]);
    const auto high = static_cast<std::uint16_t>(bytes[offset + 1]);

    return static_cast<std::uint16_t>(low | (high << 8u));
}

} // namespace katana::io
