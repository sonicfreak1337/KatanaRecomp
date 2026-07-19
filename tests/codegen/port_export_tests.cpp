#include "katana/codegen/port_export.hpp"
#include "katana/ir/lower.hpp"
#include "katana/platform/dreamcast_disc.hpp"
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

std::vector<std::uint8_t> boot_track(const bool immediate_trap = false) {
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
    constexpr std::array<std::uint8_t, 24u> normal_program = {
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
    constexpr std::array<std::uint8_t, 24u> trap_program = {
        0x00u, 0xC3u, // trapa #0
        0x0Bu, 0x00u, // unreachable rts
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u,
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u};
    const auto& program = immediate_trap ? trap_program : normal_program;
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

void write_fixture(const std::filesystem::path& directory, const bool immediate_trap = false) {
    write_binary(directory / "low.bin", std::vector<std::uint8_t>(24u * raw_sector_size));
    write_binary(directory / "audio.raw", std::vector<std::uint8_t>(raw_sector_size));
    write_binary(directory / "high.bin", boot_track(immediate_trap));
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
    if (argc == 3 && (std::string(argv[1]) == "--write-fixture" ||
                      std::string(argv[1]) == "--write-trap-fixture")) {
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        write_fixture(directory, std::string(argv[1]) == "--write-trap-fixture");
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
                runtime_cpu.read_sr() == katana::runtime::dreamcast_disc_boot_status &&
                runtime_cpu.privileged_mode() && runtime_cpu.interrupt_mask() == 15u &&
                runtime_cpu.memory.read_u16(0x8C010000u) == 0xE00Au &&
                runtime_state.runtime_blocks && runtime_state.runtime_blocks->size() == 0u &&
                runtime_state.system_asic && runtime_state.interrupt_router &&
                runtime_state.cache_control &&
                runtime_state.io_ports &&
                runtime_cpu.memory.read_u16(katana::runtime::sh4_port_data_a_address) ==
                    katana::runtime::dreamcast_composite_port_a_input &&
                runtime_cpu.memory.read_u32(katana::runtime::sh4_cache_control_address) == 0u,
            "Eigenstaendiger GDI-Boot initialisiert Bootimage, privilegierten CPU-Handoff oder "
            "Speicher nicht.");
    runtime_cpu.memory.write_u32(katana::runtime::sh4_cache_control_address,
                                 katana::runtime::Sh4CacheControl::instruction_invalidate);
    require(runtime_state.cache_control->instruction_invalidation_count() == 1u &&
                runtime_cpu.memory.read_u32(katana::runtime::sh4_cache_control_address) == 0u,
            "Produktiver GDI-Boot bindet das SH-4-Cache-Control-Register nicht ein.");
    static_cast<void>(runtime_state.code_tracker->register_block(
        {"sq-code",
         0x0C000000u,
         32u,
         "synthetic",
         {},
         katana::runtime::ExecutableBlockOrigin::ImageSegment}));
    runtime_cpu.memory.write_u32(0xFF000038u, 0x0Cu);
    for (std::uint32_t offset = 0u; offset < 32u; offset += 4u)
        runtime_cpu.memory.write_u32(0xE0000000u + offset, 0x03020100u + offset * 0x01010101u);
    require(runtime_state.store_queues->prefetch(0xE0000000u) &&
                runtime_cpu.memory.read_u32(0x0C000000u) == 0x03020100u &&
                runtime_state.code_tracker->invalidation_count() == 1u,
            "Produktive Store Queue uebertraegt keine 32 Byte nach RAM oder invalidiert Code.");
    runtime_cpu.memory.write_u32(0xFF00003Cu, 0x10u);
    runtime_cpu.memory.write_u32(0xE2000020u, 0x88776655u);
    require(runtime_state.store_queues->prefetch(0xE2000020u) &&
                runtime_state.store_queue_transfers->back().target ==
                    katana::runtime::StoreQueueTarget::TileAccelerator &&
                runtime_state.store_queue_transfers->back().bytes[0] == 0x55u,
            "Produktive Store Queue verliert QACR-basierten TA-Transfer.");
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
    static_cast<void>(hle_runtime_state.scheduler->advance_to(2'000u, 3u));
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
    require(first.functions == 2u && first.partitions == 2u && first.checkpoints.size() == 8u &&
                first.checkpoints.back() == "port-project-written",
            "Synthetische GDI durchlaeuft den Portexport nicht vollstaendig.");
    require(std::filesystem::exists(output / "content" / "game.katana-disc") &&
                std::filesystem::exists(output / "content" / "game.katana-disc.json") &&
                read_text(output / ".gitignore").find("/content/*.katana-disc") !=
                    std::string::npos &&
                read_text(output / "LOCAL_CONTENT_NOTICE.txt")
                        .find("DO NOT DISTRIBUTE") != std::string::npos &&
                read_text(output / "LOCAL_CONTENT_NOTICE.txt")
                        .find("original disc") != std::string::npos &&
                first.packed_sectors == 47u && first.packed_disc_bytes != 0u,
            "Portexport kennzeichnet oder schuetzt den lokalen Disc-Pack nicht vollstaendig.");
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
    require(
        generated_before.at("katana-port.cmake").find("add_executable(synthetic_game") !=
                std::string::npos &&
            generated_before.at("katana-port.cmake").find("katana_runtime") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("dispatch_indirect") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"execution_coverage_contract\":\"validated-demand-v1\"") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"dispatch_paths_without_validation\":0") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("generated-block-8C010000") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("register_executable_block(table, services, 0x8C010000u") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("append_static_block(static_blocks, 0x8C010000u") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("static_blocks.push_back({") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("if (const auto registered_handle = table.lookup(0x8C010000u") ==
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("6u, katana::runtime::BlockEndKind::Call") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("SLEEP besitzt kein Wakeup-Ereignis") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("Schedulerbudget erschoepft") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("Gastzyklusbudget erschoepft") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("Runtime-Blockbudget erschoepft") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("KATANA_PORT_BLOCK_LIMIT") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("poll_host_lifecycle") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("PlatformShutdownRequested") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("blocks < 1000000u") ==
                std::string::npos &&
            generated_before.at("include/katana_port.hpp").find("runtime_only_profile_json") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("runtime_only_dispatch_share_ppm") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("serialize_json(true)") !=
                std::string::npos &&
            std::filesystem::exists(output / "CMakeLists.txt") &&
            read_text(output / "CMakeLists.txt").find("katana_core") == std::string::npos &&
            std::filesystem::exists(output / "src" / "main.cpp") &&
            read_text(output / "src" / "main.cpp").find("load_dreamcast_runtime_boot") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("source.info().content_identity != expected_content_identity") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("verify_boot_identity(boot)") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("create_native_video_output") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pump_host_events") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_HOST_SHUTDOWN") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("framebuffer.capture") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("decode_pvr_scanout") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KATANA_PORT_PROGRESS") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("exception_cause=") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.expevt") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.spc") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("framebuffer.configure(640u") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("if (pump_video) pump_video(tick)") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("video->present") <
                read_text(output / "src" / "main.cpp").find("run_runtime(cpu, services)") &&
            read_text(output / "src" / "main.cpp").find("HostRuntimeSession") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("DreamcastMutableStorage::open") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("HostPacer") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("mutable_storage->save") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KATANA_HOST_PACING_ERROR") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("audio_hash") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("source-identity-mismatch") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("weakly_canonical") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("source.parent_path().string()") ==
                std::string::npos,
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

    const auto guarded_disc = katana::platform::load_dreamcast_gdi_boot(gdi);
    auto guarded_image = katana::platform::make_dreamcast_disc_executable(guarded_disc);
    require(guarded_image.segments().size() == 1u &&
                guarded_image.segments()[0].kind == katana::io::SegmentKind::Mixed &&
                guarded_image.segments()[0].source_kind ==
                    katana::io::ImageSourceKind::DiscBootFile &&
                guarded_image.segments()[0].bytes.size() == guarded_image.segments()[0].memory_size,
            "GDI-Loader markiert die Bootdatei pauschal als Code oder erfindet Zero-Fill.");
    auto guarded_analysis = katana::analysis::analyze_control_flow(guarded_image);
    require(!guarded_analysis.indirect_control_flow.empty() &&
                !guarded_analysis.resolved_edges.empty(),
            "Guarded-Portfixture besitzt keine indirekte Kandidatenkante.");
    guarded_analysis.indirect_control_flow.front().status =
        katana::analysis::ResolutionStatus::Guarded;
    guarded_analysis.indirect_control_flow.front().evidence =
        katana::analysis::ControlFlowEvidence::GuardedComplete;
    guarded_analysis.resolved_edges.front().guarded = true;
    auto guarded_program = katana::ir::lower_program(guarded_analysis);
    std::vector<katana::io::InputProvenance> guarded_inputs;
    guarded_inputs.push_back(katana::io::capture_input_provenance("gdi-descriptor", gdi));
    for (const auto& track : guarded_disc.source->descriptor().tracks)
        guarded_inputs.push_back(katana::io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path));
    const auto guarded_output = fixture.root / "guarded-port";
    const auto guarded_export =
        export_dreamcast_port_project({guarded_image,
                                       guarded_analysis,
                                       guarded_program,
                                       guarded_inputs,
                                       katana::platform::dreamcast_disc_boot_address,
                                       katana::platform::dreamcast_disc_boot_address,
                                       guarded_disc.boot_file.size(),
                                       "guarded-fixture"},
                                      guarded_output,
                                      options);
    const auto guarded_sources = snapshot(guarded_output / "generated");
    std::string guarded_text;
    for (const auto& [path, content] : guarded_sources) {
        if (path.starts_with("code/unit-")) guarded_text += content;
    }
    const auto& guarded_metadata = guarded_sources.at("metadata/port-project.json");
    require(guarded_export.functions != 0u && guarded_text.find("default:") != std::string::npos &&
                guarded_text.find("unresolved_call") != std::string::npos &&
                guarded_metadata.find("\"guarded_control_flow\":1") != std::string::npos &&
                guarded_metadata.find("\"guarded_complete_control_flow\":1") != std::string::npos &&
                guarded_metadata.find("\"guarded_partial_control_flow\":0") != std::string::npos &&
                guarded_metadata.find("\"unresolved_control_flow\":0") != std::string::npos,
            "Guarded-Kandidaten erreichen Portcodegen oder dynamischen Default nicht.");

    auto runtime_only_analysis = guarded_analysis;
    auto& runtime_only_resolution = runtime_only_analysis.indirect_control_flow.front();
    runtime_only_resolution.status = katana::analysis::ResolutionStatus::Unresolved;
    runtime_only_resolution.evidence = katana::analysis::ControlFlowEvidence::RuntimeOnly;
    runtime_only_resolution.target.reset();
    runtime_only_resolution.targets.clear();
    runtime_only_resolution.analysis_candidates.clear();
    runtime_only_resolution.reason = "synthetic-runtime-contract";
    runtime_only_analysis.resolved_edges.clear();
    const auto runtime_only_program = katana::ir::lower_program(runtime_only_analysis);
    const auto runtime_only_output = fixture.root / "runtime-only-port";
    static_cast<void>(export_dreamcast_port_project({guarded_image,
                                                     runtime_only_analysis,
                                                     runtime_only_program,
                                                     guarded_inputs,
                                                     katana::platform::dreamcast_disc_boot_address,
                                                     katana::platform::dreamcast_disc_boot_address,
                                                     guarded_disc.boot_file.size(),
                                                     "runtime-only-fixture"},
                                                    runtime_only_output,
                                                    options));
    const auto runtime_only_sources = snapshot(runtime_only_output / "generated");
    std::string runtime_only_text;
    for (const auto& [path, content] : runtime_only_sources)
        if (path.starts_with("code/unit-")) runtime_only_text += content;
    require(runtime_only_text.find("runtime_only_jump") != std::string::npos &&
                runtime_only_sources.at("metadata/port-project.json")
                        .find("\"runtime_only_control_flow\":1") != std::string::npos &&
                runtime_only_sources.at("metadata/port-project.json")
                        .find("\"unresolved_control_flow\":0") != std::string::npos,
            "Portexport verliert den validierenden Runtime-only-Vertrag.");

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
    static_cast<void>(export_dreamcast_port_project(gdi, incomplete_output, options));
    const auto inferred_runtime_sources = snapshot(incomplete_output / "generated");
    std::string inferred_runtime_text;
    for (const auto& [path, content] : inferred_runtime_sources)
        if (path.starts_with("code/unit-")) inferred_runtime_text += content;
    require(inferred_runtime_text.find("runtime_only_jump") != std::string::npos &&
                inferred_runtime_sources.at("metadata/port-project.json")
                        .find("\"runtime_only_control_flow\":1") != std::string::npos &&
                inferred_runtime_sources.at("metadata/port-project.json")
                        .find("\"unresolved_control_flow\":0") != std::string::npos,
            "Allgemeiner Runtimezeiger erreicht den validierenden Portvertrag nicht.");

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
