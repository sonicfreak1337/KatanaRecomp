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
    std::vector<std::uint8_t> bytes(0x198u, 0u);
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
    write_u32(bytes, 32u, 0xD0u);
    write_u16(bytes, 40u, 52u);
    write_u16(bytes, 42u, 32u);
    write_u16(bytes, 44u, 2u);
    write_u16(bytes, 46u, 40u);
    write_u16(bytes, 48u, 4u);

    write_u32(bytes, 52u, 1u);
    write_u32(bytes, 56u, 0x80u);
    write_u32(bytes, 60u, 0x8C010000u);
    write_u32(bytes, 68u, 4u);
    write_u32(bytes, 72u, 4u);
    write_u32(bytes, 76u, 5u);
    write_u32(bytes, 84u, 1u);
    write_u32(bytes, 88u, 0x84u);
    write_u32(bytes, 92u, 0x8C020000u);
    write_u32(bytes, 100u, 12u);
    write_u32(bytes, 104u, 12u);
    write_u32(bytes, 108u, 6u);

    bytes[0x80u] = 0x09u;
    bytes[0x81u] = 0x00u;
    bytes[0x82u] = 0x0Bu;
    bytes[0x83u] = 0x00u;
    bytes[0x84u] = 0xAAu;
    bytes[0x85u] = 0x55u;
    write_u32(bytes, 0x84u, 4u);
    write_u32(bytes, 0x88u, 0u);
    write_u32(bytes, 0x8Cu, 0x12345678u);

    const std::string names("\0start\0value\0", 13u);
    for (std::size_t index = 0; index < names.size(); ++index) {
        bytes[0x90u + index] = static_cast<std::uint8_t>(names[index]);
    }
    write_u32(bytes, 0xB0u, 1u);
    write_u32(bytes, 0xB4u, 0x8C010000u);
    write_u32(bytes, 0xB8u, 4u);
    bytes[0xBCu] = 0x12u;
    write_u16(bytes, 0xBEu, 1u);
    write_u32(bytes, 0xC0u, 7u);
    write_u32(bytes, 0xC4u, 0x8C020000u);
    write_u32(bytes, 0xC8u, 2u);
    bytes[0xCCu] = 0x11u;
    write_u16(bytes, 0xCEu, 2u);

    write_u32(bytes, 0xFCu, 3u);
    write_u32(bytes, 0x108u, 0x90u);
    write_u32(bytes, 0x10Cu, 13u);
    write_u32(bytes, 0x124u, 2u);
    write_u32(bytes, 0x130u, 0xA0u);
    write_u32(bytes, 0x134u, 48u);
    write_u32(bytes, 0x138u, 1u);
    write_u32(bytes, 0x144u, 16u);
    write_u32(bytes, 0x14Cu, 9u);
    write_u32(bytes, 0x158u, 0x170u);
    write_u32(bytes, 0x15Cu, 24u);
    write_u32(bytes, 0x160u, 2u);
    write_u32(bytes, 0x16Cu, 8u);
    write_u32(bytes, 0x170u, 0x8C020000u);
    write_u32(bytes, 0x174u, (2u << 8u) | 1u);
    write_u32(bytes, 0x178u, 0x8C020004u);
    write_u32(bytes, 0x17Cu, (1u << 8u) | 2u);
    write_u32(bytes, 0x180u, 0x8C020008u);
    write_u32(bytes, 0x184u, (2u << 8u) | 0xFFu);
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
    require(data.virtual_address == 0x8C020000u && data.memory_size == 12u && data.bytes.size() == 12u, "Data- oder Zero-Fill-Groesse ist falsch.");
    require(data.kind == SegmentKind::Data && data.permissions.readable && data.permissions.writable && !data.permissions.executable, "PF_R/PF_W wurden falsch abgebildet.");
    require(image.symbols().size() == 2u, "ELF-Symboltabelle wurde nicht vollstaendig geladen.");
    const auto* start = image.find_symbol("start");
    require(start != nullptr && start->address == 0x8C010000u && start->size == 4u, "ELF-Funktionssymbol ist falsch.");
    require(start->kind == SymbolKind::Function && start->binding == SymbolBinding::Global, "ELF-Symbolinfo wurde falsch dekodiert.");
    require(image.find_symbol("value")->kind == SymbolKind::Object, "ELF-Objektsymbol ist falsch klassifiziert.");
    require(image.relocations().size() == 3u, "ELF-Relocations wurden nicht vollstaendig geladen.");
    const auto& relocation = image.relocations()[0];
    require(relocation.address == 0x8C020000u && relocation.kind == RelocationKind::Absolute32, "R_SH_DIR32 wurde falsch klassifiziert.");
    require(relocation.symbol_name == "value" && relocation.addend == 4, "Relocation-Symbol oder Addend ist falsch.");
    require(relocation.applied_value == 0x8C020004u, "R_SH_DIR32 wurde nicht angewendet.");
    require(image.read_u32_le(0x8C020000u) == 0x8C020004u, "Relocationsergebnis fehlt in den Segmentdaten.");
    const auto& relative = image.relocations()[1];
    require(relative.kind == RelocationKind::PcRelative32 && relative.applied_value == 0xFFFEFFFCu, "R_SH_REL32 wurde falsch angewendet.");
    const auto& unsupported = image.relocations()[2];
    require(unsupported.kind == RelocationKind::Unsupported && !unsupported.applied_value.has_value(), "Unbekannte Relocation wurde faelschlich angewendet.");
    require(image.read_u32_le(0x8C020008u) == 0x12345678u, "Unbekannte Relocation hat Segmentdaten veraendert.");

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
