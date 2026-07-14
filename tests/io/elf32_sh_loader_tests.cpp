#include "katana/io/elf32_sh_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

void write_u16(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void write_u32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

std::vector<std::uint8_t> valid_elf() {
    std::vector<std::uint8_t> bytes(0x86u, 0u);
    bytes[0] = 0x7Fu;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1u;
    bytes[5] = 1u;
    bytes[6] = 1u;
    write_u16(bytes, 16u, 2u);
    write_u16(bytes, 18u, 42u);
    write_u32(bytes, 20u, 1u);
    write_u32(bytes, 24u, 0x8C010000u);
    write_u32(bytes, 28u, 52u);
    write_u16(bytes, 40u, 52u);
    write_u16(bytes, 42u, 32u);
    write_u16(bytes, 44u, 2u);

    write_u32(bytes, 52u, 1u);
    write_u32(bytes, 56u, 0x80u);
    write_u32(bytes, 60u, 0x8C010000u);
    write_u32(bytes, 68u, 4u);
    write_u32(bytes, 72u, 4u);
    write_u32(bytes, 76u, 5u);
    write_u32(bytes, 84u, 1u);
    write_u32(bytes, 88u, 0x84u);
    write_u32(bytes, 92u, 0x8C020000u);
    write_u32(bytes, 100u, 2u);
    write_u32(bytes, 104u, 8u);
    write_u32(bytes, 108u, 6u);

    bytes[0x80u] = 0x09u;
    bytes[0x81u] = 0x00u;
    bytes[0x82u] = 0x0Bu;
    bytes[0x83u] = 0x00u;
    bytes[0x84u] = 0xAAu;
    bytes[0x85u] = 0x55u;
    return bytes;
}

void save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string load_failure(const std::filesystem::path& path) {
    try {
        static_cast<void>(katana::io::load_elf32_sh(path));
    } catch (const std::exception& error) {
        return error.what();
    }
    require(false, "Eine ungueltige ELF-Datei wurde akzeptiert.");
    return {};
}

}

int main() {
    using namespace katana::io;
    const auto path = std::filesystem::current_path() / "katana-elf32-sh-fixture.elf";
    auto bytes = valid_elf();
    save(path, bytes);

    const auto image = load_elf32_sh(path);
    require(image.segments().size() == 2u, "PT_LOAD-Segmente wurden nicht vollstaendig geladen.");
    require(image.entry_points().size() == 1u && image.entry_points()[0] == 0x8C010000u, "ELF-Einstiegspunkt ist falsch.");
    const auto& text = image.segments()[0];
    const auto& data = image.segments()[1];
    require(text.virtual_address == 0x8C010000u && text.file_offset == 0x80u, "Textlayout ist falsch.");
    require(text.kind == SegmentKind::Code && text.permissions.readable && text.permissions.executable && !text.permissions.writable, "PF_R/PF_X wurden falsch abgebildet.");
    require(text.bytes == std::vector<std::uint8_t>({0x09u, 0x00u, 0x0Bu, 0x00u}), "Textbytes sind falsch.");
    require(data.virtual_address == 0x8C020000u && data.memory_size == 8u && data.bytes.size() == 2u, "Data- oder Zero-Fill-Groesse ist falsch.");
    require(data.kind == SegmentKind::Data && data.permissions.readable && data.permissions.writable && !data.permissions.executable, "PF_R/PF_W wurden falsch abgebildet.");

    bytes = valid_elf();
    bytes[0] = 0u;
    save(path, bytes);
    auto error = load_failure(path);
    require(error.find(path.string()) != std::string::npos && error.find("Offset 0") != std::string::npos, "Magiefehler nennt Pfad und Offset nicht.");

    bytes = valid_elf();
    write_u16(bytes, 18u, 3u);
    save(path, bytes);
    error = load_failure(path);
    require(error.find("Offset 18") != std::string::npos && error.find("EM_SH") != std::string::npos, "Maschinenfehler ist nicht diagnostisch.");

    bytes = valid_elf();
    write_u32(bytes, 68u, 5u);
    write_u32(bytes, 72u, 4u);
    save(path, bytes);
    error = load_failure(path);
    require(error.find("p_filesz") != std::string::npos, "Ungueltige Segmentgroessen wurden nicht erklaert.");

    bytes = valid_elf();
    write_u32(bytes, 56u, 0x1000u);
    save(path, bytes);
    error = load_failure(path);
    require(error.find("Offset 4096") != std::string::npos && error.find("PT_LOAD") != std::string::npos, "Dateibereichsfehler nennt Offset und Ursache nicht.");

    std::filesystem::remove(path);
    std::cout << "KR-1603 ELF32-SH-Loader erfolgreich.\n";
    return EXIT_SUCCESS;
}
