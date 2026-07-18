#include "katana/runtime/disc.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}
struct TempFile {
    std::filesystem::path path;
    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};
} // namespace

int main() {
    using namespace katana::runtime;
    const std::vector<std::uint8_t> bytes = {0x10u, 0x20u, 0x30u, 0x40u, 0x50u};
    MemoryDiscSource memory(bytes, "synthetic:memory-disc");
    require(memory.size() == 5u && memory.identity() == "synthetic:memory-disc" &&
                memory.read(1u, 3u) == std::vector<std::uint8_t>({0x20u, 0x30u, 0x40u}),
            "Speicher-Discquelle verliert Groesse, Identitaet oder Bereichsdaten.");
    require(throws<std::out_of_range>([&] { static_cast<void>(memory.read(4u, 2u)); }),
            "Speicher-Discquelle akzeptiert einen ueberlaufenden Read.");
    require(throws<std::invalid_argument>([&] { MemoryDiscSource invalid(bytes, ""); }),
            "Discquelle akzeptiert eine leere semantische Identitaet.");

    TempFile fixture{std::filesystem::current_path() / "katana-disc-source-test.bin"};
    {
        std::ofstream output(fixture.path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    FileDiscSource file(fixture.path, "synthetic:file-disc");
    require(file.size() == bytes.size() && file.identity() == "synthetic:file-disc" &&
                file.read(2u, 2u) == std::vector<std::uint8_t>({0x30u, 0x40u}),
            "Datei-Discquelle liest nicht deterministisch oder leakt den Hostpfad als Identitaet.");
    static_cast<void>(file.read(0u, 1u));
    static_cast<void>(file.read(1u, 1u));
    require(file.open_operations() == 1u && file.read_operations() == 3u &&
                file.bytes_read() == 4u,
            "Wiederholte Dateireads oeffnen die read-only Quelle erneut.");
    require(std::filesystem::file_size(fixture.path) == bytes.size(),
            "Read-only-Discquelle hat die Quelldatei veraendert.");
    require(throws<std::invalid_argument>([] {
                FileDiscSource missing("katana-missing-disc-source.bin", "synthetic:missing");
            }),
            "Fehlende Disc-Dateiquelle wird akzeptiert.");

    std::cout << "KR-3001 Disc- und Dateiquellen-Abstraktion erfolgreich.\n";
}
