#include "katana/codegen/port_export.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t raw_sector_size = 2352u;
constexpr std::size_t payload_size = 2048u;
constexpr std::uint32_t data_lba = 100u;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception = std::exception, typename Function>
void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

struct Fixture final {
    std::filesystem::path root = std::filesystem::current_path() / "katana-port-export-fixture";

    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "disc");
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void both32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
        bytes[offset + 4u + index] = static_cast<std::uint8_t>(value >> ((3u - index) * 8u));
    }
}

std::size_t record(std::vector<std::uint8_t>& bytes,
                   const std::size_t offset,
                   const std::uint32_t lba,
                   const std::uint32_t size,
                   const std::string& name,
                   const bool directory) {
    const auto length =
        static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    bytes[offset] = length;
    both32(bytes, offset + 2u, lba);
    both32(bytes, offset + 10u, size);
    bytes[offset + 25u] = directory ? 2u : 0u;
    bytes[offset + 28u] = 1u;
    bytes[offset + 31u] = 1u;
    bytes[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}

std::size_t payload_offset(const std::size_t sector, const std::size_t byte = 0u) {
    return sector * raw_sector_size + 16u + byte;
}

std::vector<std::uint8_t> boot_track() {
    std::vector<std::uint8_t> bytes(22u * raw_sector_size);
    for (std::size_t sector = 0u; sector < 22u; ++sector) {
        bytes[sector * raw_sector_size + 15u] = 1u;
    }
    const std::string hardware = "SEGA SEGAKATANA ";
    const std::string boot_file = "BOOT.BIN        ";
    std::copy(hardware.begin(),
              hardware.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u)));
    std::copy(boot_file.begin(),
              boot_file.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u, 0x60u)));

    const auto pvd = payload_offset(16u);
    bytes[pvd] = 1u;
    std::copy_n("CD001", 5u, bytes.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    bytes[pvd + 6u] = 1u;
    record(bytes, pvd + 156u, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    record(bytes, directory, data_lba + 21u, 4u, "BOOT.BIN;1", false);
    bytes[payload_offset(21u)] = 0x0Bu; // rts
    bytes[payload_offset(21u, 1u)] = 0x00u;
    bytes[payload_offset(21u, 2u)] = 0x09u; // delay-slot nop
    bytes[payload_offset(21u, 3u)] = 0x00u;
    return bytes;
}

void write_binary(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void write_fixture(const std::filesystem::path& directory) {
    write_binary(directory / "low.bin", std::vector<std::uint8_t>(24u * raw_sector_size));
    write_binary(directory / "audio.raw", std::vector<std::uint8_t>(raw_sector_size));
    write_binary(directory / "high.bin", boot_track());
    std::ofstream descriptor(directory / "disc.gdi", std::ios::trunc);
    descriptor << "3\n"
               << "1 0 4 2352 low.bin 0\n"
               << "2 30 0 2352 audio.raw 0\n"
               << "3 100 4 2352 high.bin 0\n";
}

std::map<std::string, std::string> snapshot(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream input(entry.path(), std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        result.emplace(entry.path().lexically_relative(root).generic_string(), content.str());
    }
    return result;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc == 3 && std::string(argv[1]) == "--write-fixture") {
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        write_fixture(directory);
        return EXIT_SUCCESS;
    }
    require(argc == 1, "Unerwartete Argumente fuer den Portexporttest.");
    using namespace katana::codegen;
    Fixture fixture;
    write_fixture(fixture.root / "disc");
    const auto gdi = fixture.root / "disc" / "disc.gdi";
    const auto output = fixture.root / "port";
    const PortExportOptions options{"synthetic_game", "0.34.0-dev", {128u, 4096u}};

    const auto first = export_dreamcast_port_project(gdi, output, options);
    const auto generated_before = snapshot(output / "generated");
    const auto unit =
        std::find_if(generated_before.begin(), generated_before.end(), [](const auto& entry) {
            return entry.first.starts_with("code/unit-00000-") && entry.first.ends_with(".cpp");
        });
    require(first.functions == 1u && first.partitions == 1u && first.checkpoints.size() == 6u &&
                first.checkpoints.back() == "port-project-written",
            "Synthetische GDI durchlaeuft den Portexport nicht vollstaendig.");
    require(unit != generated_before.end(),
            "Portexport besitzt keine deterministische Translation Unit.");
    for (const auto& path : {"include/katana_port.hpp",
                             "metadata/port-project.json",
                             "metadata/provenance.json",
                             "metadata/source-map.json",
                             "metadata/cfg.json",
                             "metadata/cfg.dot",
                             "metadata/callgraph.json",
                             "metadata/callgraph.dot",
                             "katana-port.cmake"}) {
        require(generated_before.contains(path),
                "Portexport verliert Artefakt: " + std::string(path));
    }
    require(generated_before.at("katana-port.cmake").find("add_executable(synthetic_game") !=
                    std::string::npos &&
                generated_before.at("katana-port.cmake").find("katana_runtime") !=
                    std::string::npos &&
                std::filesystem::exists(output / "CMakeLists.txt") &&
                std::filesystem::exists(output / "src" / "main.cpp"),
            "Portprojekt besitzt kein ausfuehrbares Hosttarget oder keinen Runtimevertrag.");
    std::string portable_content;
    for (const auto& [path, content] : generated_before) {
        static_cast<void>(path);
        portable_content += content;
    }
    require(portable_content.find(fixture.root.string()) == std::string::npos &&
                portable_content.find("disc.gdi") == std::string::npos &&
                portable_content.find("high.bin") == std::string::npos,
            "Portartefakte enthalten absolute oder private Disc-/Trackpfade.");

    {
        std::ofstream user(output / "src" / "notes.txt", std::ios::trunc);
        user << "keep-user-file\n";
    }
    const auto second = export_dreamcast_port_project(gdi, output, options);
    require(generated_before == snapshot(output / "generated") && second.removed_files == 0u,
            "Identische Portregenerierung ist nicht bytegleich.");
    std::ifstream user(output / "src" / "notes.txt");
    std::ostringstream user_content;
    user_content << user.rdbuf();
    require(user_content.str() == "keep-user-file\n",
            "Portregenerierung hat eine handgeschriebene Nutzerdatei veraendert.");

    auto low = std::vector<std::uint8_t>(24u * raw_sector_size);
    low.front() = 0xA5u;
    write_binary(fixture.root / "disc" / "low.bin", low);
    const auto provenance_before = generated_before.at("metadata/provenance.json");
    static_cast<void>(export_dreamcast_port_project(gdi, output, options));
    const auto changed = snapshot(output / "generated");
    require(changed.at("metadata/provenance.json") != provenance_before &&
                changed.at(unit->first) == unit->second &&
                std::filesystem::exists(output / "src" / "notes.txt"),
            "Geaenderte Eingabe invalidiert Provenienz nicht gezielt oder loescht Nutzerdateien.");

    auto invalid_options = options;
    invalid_options.target_name = "../invalid";
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(export_dreamcast_port_project(gdi, output, invalid_options)); },
        "Unportabler Port-Zielname wurde akzeptiert.");

    std::cout << "KR-3507 reproduzierbarer Port-Projektexport erfolgreich.\n";
    return EXIT_SUCCESS;
}
