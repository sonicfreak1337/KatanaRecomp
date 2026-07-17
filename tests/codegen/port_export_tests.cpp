#include "katana/codegen/port_export.hpp"
#include "katana/runtime/dreamcast_boot.hpp"

#include <algorithm>
#include <array>
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
    record(bytes, directory, data_lba + 21u, 24u, "BOOT.BIN;1", false);
    constexpr std::array<std::uint8_t, 24u> program = {
        0x0Au, 0xE0u, // mov #10,r0
        0x03u, 0x00u, // bsrf r0 -> 0x8C010010
        0x07u, 0xE2u, // delay slot: mov #7,r2
        0x0Bu, 0x00u, // caller rts
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // padding nop
        0x09u, 0x00u, // padding nop
        0x09u, 0x00u, // padding nop
        0x05u, 0xE1u, // callee: mov #5,r1
        0xFFu, 0x71u, // add #-1,r1
        0x0Bu, 0x00u, // callee rts
        0x09u, 0x00u  // delay-slot nop
    };
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
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

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int run_test(const int argc, char* argv[]) {
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
    const auto runtime_boot = katana::runtime::load_dreamcast_runtime_boot(gdi);
    katana::runtime::CpuState runtime_cpu;
    const auto runtime_state =
        katana::runtime::initialize_dreamcast_runtime(runtime_cpu, runtime_boot);
    require(runtime_state.loaded_boot_bytes == 24u && runtime_cpu.pc == 0x8C010000u &&
                runtime_cpu.r[15] == 0x8D000000u &&
                runtime_cpu.memory.read_u16(0x8C010000u) == 0xE00Au &&
                runtime_state.runtime_blocks && runtime_state.runtime_blocks->size() == 0u &&
                runtime_state.system_asic && runtime_state.interrupt_router,
            "Eigenstaendiger GDI-Boot initialisiert Bootimage, CPU oder Speicher nicht.");
    katana::runtime::CpuState hle_runtime_cpu;
    const auto hle_runtime_state = katana::runtime::initialize_dreamcast_runtime(
        hle_runtime_cpu, runtime_boot, katana::runtime::DreamcastRuntimeFirmwareMode::HleBiosAbi);
    require(hle_runtime_cpu.memory.read_u32(0x8C0000B0u) == 0x8C000100u &&
                hle_runtime_state.runtime_blocks->size() == 6u,
            "Produktiver GDI-HLE-Runtimepfad installiert die BIOS-ABI nicht.");
    hle_runtime_state.pvr_registers->write(katana::runtime::pvr_register::StartRender, 1u);
    auto input = std::make_shared<katana::runtime::ReplayInputBackend>(
        std::vector<katana::runtime::ControllerState>{{}});
    hle_runtime_state.maple->attach(
        0u, 0u, std::make_shared<katana::runtime::MapleControllerDevice>(input));
    static_cast<void>(hle_runtime_state.maple->exchange(
        0u, 0u, {katana::runtime::MapleCommand::GetCondition, {}}));
    static_cast<void>(
        hle_runtime_state.gdrom->submit({katana::runtime::GdRomCommand::TestUnitReady}));
    hle_runtime_state.gdrom->advance_to(1'000u);
    static_cast<void>(hle_runtime_state.scheduler->advance_to(1'000u, 1u));
    hle_runtime_state.aica->interrupts().set_enabled(1u);
    hle_runtime_state.aica->interrupts().request(1u);
    require(
        hle_runtime_state.system_asic->events().size() == 4u,
        "Produktive PVR-, Maple-, GD-ROM- und AICA-Ereignisse erreichen das System-ASIC nicht.");
    const auto output = fixture.root / "port";
    const PortExportOptions options{"synthetic_game", "0.37.0-dev", {1u, 4096u}};

    const auto first = export_dreamcast_port_project(gdi, output, options);
    const auto generated_before = snapshot(output / "generated");
    const auto unit =
        std::find_if(generated_before.begin(), generated_before.end(), [](const auto& entry) {
            return entry.first.starts_with("code/unit-00000-") && entry.first.ends_with(".cpp");
        });
    require(first.functions == 2u && first.partitions == 2u && first.checkpoints.size() == 6u &&
                first.checkpoints.back() == "port-project-written",
            "Synthetische GDI durchlaeuft den Portexport nicht vollstaendig.");
    require(unit != generated_before.end(),
            "Portexport besitzt keine deterministische Translation Unit.");
    std::size_t entry_metadata_count = 0u;
    for (const auto& [path, content] : generated_before) {
        if (path.starts_with("code/unit-") && path.ends_with(".cpp")) {
            require(content.find("generated_entry_address = 0x8C010000u") != std::string::npos,
                    "Portpartition besitzt einen abweichenden globalen Programmeinstieg.");
            ++entry_metadata_count;
        }
    }
    require(entry_metadata_count == 2u,
            "Mehrteiliger Portexport erzeugt nicht exakt zwei Translation Units.");
    for (const auto& path : {"include/katana_port.hpp",
                             "code/runtime-dispatch.cpp",
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
                generated_before.at("code/runtime-dispatch.cpp").find("dispatch_indirect") !=
                    std::string::npos &&
                std::filesystem::exists(output / "CMakeLists.txt") &&
                std::filesystem::exists(output / "src" / "main.cpp") &&
                read_text(output / "src" / "main.cpp").find("load_dreamcast_runtime_boot") !=
                    std::string::npos &&
                read_text(output / "src" / "main.cpp").find("create_native_video_output") !=
                    std::string::npos &&
                read_text(output / "src" / "main.cpp").find("framebuffer.capture") !=
                    std::string::npos &&
                read_text(output / "src" / "main.cpp").find("HostRuntimeSession") !=
                    std::string::npos &&
                read_text(output / "src" / "main.cpp").find("audio_hash") != std::string::npos,
            "Portprojekt besitzt keinen ausfuehrbaren GDI-/Runtimevertrag.");
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

    auto protected_options = options;
    protected_options.forbidden_source_root = fixture.root;
    require_failure<std::invalid_argument>(
        [&] {
            static_cast<void>(export_dreamcast_port_project(
                gdi, fixture.root / "generated-commercial-port", protected_options));
        },
        "Portausgabe innerhalb des geschuetzten Quellbaums wurde akzeptiert.");
    const auto link = std::filesystem::temp_directory_path() / "katana-port-parent-link";
    std::error_code link_error;
    std::filesystem::remove(link, link_error);
    link_error.clear();
    std::filesystem::create_directory_symlink(fixture.root, link, link_error);
    if (!link_error) {
        require_failure<std::invalid_argument>(
            [&] {
                static_cast<void>(export_dreamcast_port_project(
                    gdi, link / "through-parent-link", protected_options));
            },
            "Symlink-Elternpfad umgeht den geschuetzten Quellbaum.");
        std::filesystem::remove(link, link_error);
    }

    auto incomplete_track = boot_track();
    incomplete_track[payload_offset(21u)] = 0x09u;
    incomplete_track[payload_offset(21u, 1u)] = 0x00u;
    write_binary(fixture.root / "disc" / "high.bin", incomplete_track);
    const auto incomplete_output = fixture.root / "incomplete-port";
    require_failure<std::runtime_error>(
        [&] { static_cast<void>(export_dreamcast_port_project(gdi, incomplete_output, options)); },
        "Portexport akzeptiert ungeloesten indirekten Kontrollfluss.");
    require(!std::filesystem::exists(incomplete_output),
            "Abgelehnter unvollstaendiger Portexport hinterlaesst Artefakte.");

    std::cout << "KR-3507/KR-4502/KR-4507 reproduzierbarer Port-Projektexport erfolgreich.\n";
    return EXIT_SUCCESS;
}

int main(const int argc, char* argv[]) {
    try {
        return run_test(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
