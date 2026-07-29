#include "katana/platform/dreamcast.hpp"
#include "katana/runtime/bios_abi.hpp"
#include "katana/runtime/game_project.hpp"
#include "katana/runtime/game_project_artifact.hpp"
#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

katana::runtime::GameProjectHookResult mutate_pc_and_continue(
    katana::runtime::CpuState& cpu,
    katana::runtime::PlatformServices*,
    void*) noexcept {
    cpu.pc += 2u;
    return {};
}
} // namespace

int main() {
    using namespace katana;
    io::ExecutableImage image;
    io::ImageSegment segment;
    segment.name = ".text";
    segment.virtual_address = 0x8C010000u;
    segment.memory_size = 8u;
    segment.bytes = {0x09u, 0x00u, 0x0Bu, 0x00u};
    segment.permissions = {true, false, true};
    image.add_segment(std::move(segment));
    image.add_entry_point(0x8C010000u);

    runtime::CpuState cpu;
    cpu.memory = runtime::Memory(0u);
    const auto result = platform::boot_homebrew(cpu, image);
    require(cpu.pc == 0x8C010000u && cpu.r[15] == 0x8D000000u,
            "Direktboot setzt PC oder Stack nicht deterministisch.");
    require(cpu.memory.read_u32(0x8C010000u) == 0x000B0009u &&
                cpu.memory.read_u32(0x8C010004u) == 0u,
            "Direktboot kopiert Datei- oder BSS-Anteil falsch.");
    require(result.loaded_segments == 1u && result.loaded_bytes == 8u &&
                result.log.front() == "firmware=direct-homebrew" &&
                result.log.back() == "boot=ready",
            "Minimales Plattformlogging ist unvollstaendig.");

    runtime::CpuState hle_cpu;
    hle_cpu.memory = runtime::Memory(0u);
    platform::DreamcastBootConfig hle;
    hle.firmware_mode = platform::FirmwareMode::HleBiosAbi;
    const auto hle_result = platform::boot_homebrew(hle_cpu, image, hle);
    require(hle_result.runtime_blocks && hle_result.firmware_handoff &&
                hle_result.runtime_blocks->size() == 7u &&
                hle_result.firmware_handoff->runtime_symbols().size() == 13u &&
                hle_cpu.memory.read_u32(0x8C0000B0u) ==
                    katana::runtime::hle_bios_abi_vectors()[0].handler_address &&
                hle_result.log.front() == "firmware=hle-bios-abi" &&
                hle_result.log[hle_result.log.size() - 2u] == "bios-abi=installed",
            "Produktiver HLE-Boot installiert BIOS-ABI, Blocktabelle oder Handoff nicht.");

    runtime::CpuState unsupported;
    unsupported.memory = runtime::Memory(0u);
    platform::DreamcastBootConfig lle;
    lle.firmware_mode = platform::FirmwareMode::LleFirmware;
    require(throws<std::invalid_argument>(
                [&] { static_cast<void>(platform::boot_homebrew(unsupported, image, lle)); }),
            "Nicht unterstuetzter LLE-Modus scheitert nicht sichtbar.");

    io::ExecutableImage empty;
    runtime::CpuState missing;
    missing.memory = runtime::Memory(0u);
    require(throws<std::invalid_argument>(
                [&] { static_cast<void>(platform::boot_homebrew(missing, empty)); }),
            "Fehlende Segmente oder Einstiegspunkte werden nicht abgewiesen.");

    io::ExecutableImage invalid;
    io::ImageSegment valid_segment;
    valid_segment.name = ".text";
    valid_segment.virtual_address = 0x8C030000u;
    valid_segment.memory_size = 2u;
    valid_segment.bytes = {0x09u, 0u};
    valid_segment.permissions = {true, false, true};
    invalid.add_segment(std::move(valid_segment));
    io::ImageSegment invalid_segment;
    invalid_segment.name = ".bad";
    invalid_segment.virtual_address = 0x01000000u;
    invalid_segment.memory_size = 1u;
    invalid_segment.bytes = {0u};
    invalid.add_segment(std::move(invalid_segment));
    invalid.add_entry_point(0x8C030000u);
    runtime::CpuState atomic;
    atomic.memory = runtime::Memory(0u);
    require(throws<std::invalid_argument>(
                [&] { static_cast<void>(platform::boot_homebrew(atomic, invalid)); }) &&
                atomic.memory.region_count() == 0u,
            "Fehlerhafte spaetere Segmente hinterlassen einen halben Plattformboot.");

    const auto raw_path = std::filesystem::temp_directory_path() / "katana-v026-homebrew.bin";
    {
        std::ofstream out(raw_path, std::ios::binary | std::ios::trunc);
        const char bytes[2] = {'\x09', '\x00'};
        out.write(bytes, 2);
    }
    runtime::CpuState raw_cpu;
    raw_cpu.memory = runtime::Memory(0u);
    io::RawBinaryLoadOptions raw_options;
    raw_options.base_address = 0x8C020000u;
    raw_options.entry_point = 0x8C020000u;
    const auto raw_result = platform::boot_raw_homebrew(raw_cpu, raw_path, raw_options);
    std::filesystem::remove(raw_path);
    require(raw_result.entry_point == 0x8C020000u &&
                raw_cpu.memory.read_u16(0x8C020000u) == 0x0009u,
            "Raw-Homebrew erreicht den Plattformboot nicht.");

    const std::array project_functions{
        runtime::GameProjectFunctionBoundary{
            0x8C010000u, 8u, "identity_bound_entry"}};
    const std::array project_overrides{
        runtime::GameProjectFunctionOverride{
            0x8C010000u,
            &mutate_pc_and_continue,
            nullptr,
            nullptr,
            runtime::GameProjectFunctionOverrideStrength::Required}};
    runtime::GameProjectDefinition game_project;
    game_project.project_id = "hook-contract-regression";
    game_project.project_version = "1";
    game_project.identity = {
        "synthetic-content",
        "BOOT.BIN",
        "sha256:0000000000000000000000000000000000000000000000000000000000000000"};
    game_project.function_boundaries = project_functions;
    game_project.function_overrides = project_overrides;
    const runtime::GameProjectBindings bindings(game_project);
    runtime::CpuState hook_cpu;
    hook_cpu.pc = 0x8C010000u;
    const auto invalid_continue = bindings.invoke_function_override(
        hook_cpu.pc, hook_cpu, nullptr);
    require(invalid_continue.status ==
                runtime::GameProjectHookDispatchStatus::Invalid,
            "Ein Continue-Hook darf den bereits validierten Gast-PC nicht "
            "unter dem Dispatcher veraendern.");

    const std::array<std::uint8_t, 8u> runtime_image_bytes{
        0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u};
    const std::array<std::uint32_t, 2u> runtime_image_entries{0u, 4u};
    const auto runtime_image_hash =
        std::string("sha256:") + io::sha256_bytes(std::string_view(
            reinterpret_cast<const char*>(runtime_image_bytes.data()),
            runtime_image_bytes.size()));
    const std::array runtime_images{
        runtime::GameProjectRuntimeImage{
            "linked-runtime-image",
            runtime_image_hash,
            0x8C1E5300u,
            0x8C900000u,
            runtime_image_bytes,
            runtime_image_entries}};
    runtime::GameProjectDefinition artifact_project;
    artifact_project.project_id = "runtime-image-artifact-regression";
    artifact_project.project_version = "1";
    artifact_project.identity = game_project.identity;
    artifact_project.runtime_images = runtime_images;
    const auto definition_identity =
        runtime::game_project_definition_identity(artifact_project);
    const auto artifact_path =
        std::filesystem::temp_directory_path() /
        "katana-v4-runtime-image.katana-game-project";
    const auto artifact =
        runtime::GameProjectArtifact::write(artifact_path, artifact_project);
    const auto& roundtrip_images = artifact->definition().runtime_images;
    require(roundtrip_images.size() == 1u &&
                roundtrip_images.front().image_id ==
                    runtime_images.front().image_id &&
                roundtrip_images.front().byte_identity ==
                    runtime_image_hash &&
                roundtrip_images.front().source_start == 0x8C1E5300u &&
                roundtrip_images.front().runtime_start == 0x8C900000u &&
                std::equal(
                    roundtrip_images.front().bytes.begin(),
                    roundtrip_images.front().bytes.end(),
                    runtime_image_bytes.begin(),
                    runtime_image_bytes.end()) &&
                std::equal(
                    roundtrip_images.front().entry_offsets.begin(),
                    roundtrip_images.front().entry_offsets.end(),
                    runtime_image_entries.begin(),
                    runtime_image_entries.end()) &&
                runtime::game_project_definition_identity(
                    artifact->definition()) == definition_identity,
            "Runtime-Image verliert Bytes, Entries oder v4-Identitaet beim "
            "Artifact-Roundtrip.");
    std::filesystem::remove(artifact_path);

    auto mismatched_runtime_image = runtime_images.front();
    mismatched_runtime_image.byte_identity =
        "sha256:0000000000000000000000000000000000000000000000000000000000000000";
    const std::array mismatched_runtime_images{mismatched_runtime_image};
    artifact_project.runtime_images = mismatched_runtime_images;
    require(throws<std::invalid_argument>([&] {
                runtime::validate_game_project_definition(
                    artifact_project);
            }),
            "Runtime-Image mit falscher Byteidentitaet wird akzeptiert.");

    std::cout << "BIOS-freier Dreamcast-Homebrew-Boot erfolgreich.\n";
}
