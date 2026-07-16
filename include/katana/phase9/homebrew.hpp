#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace katana::phase9 {

inline constexpr std::uint32_t homebrew_corpus_schema_version = 1u;
inline constexpr std::uint32_t homebrew_report_schema_version = 1u;

enum class HomebrewArtifactKind : std::uint8_t {
    CpuConformance,
    Console,
    Controller,
    Graphics2d,
    Audio,
    IntegratedGame,
    FirmwareHandoff,
    SchedulerDmaInterrupt,
};

struct HomebrewArtifact {
    std::string id;
    HomebrewArtifactKind kind = HomebrewArtifactKind::CpuConformance;
    std::string origin;
    std::string distribution_status;
    std::vector<std::uint8_t> bytes;
    std::string sha256;
};

struct FirmwareHandoffReport {
    std::uint32_t p2_entry = 0u;
    std::uint32_t p1_entry = 0u;
    std::uint32_t rom_physical = 0u;
    std::uint32_t ram_physical = 0u;
    std::size_t copied_bytes = 0u;
    std::size_t dynamic_vectors = 0u;
    std::uint64_t prefetches = 0u;
    std::uint64_t invalidations = 0u;
    bool store_queue_observed = false;
    bool analysis_complete = false;
    bool generated_cpp_complete = false;
    bool dispatch_complete = false;
};

struct HomebrewHostFrameReport {
    std::string marker = "KR_PHASE9_HOMEBREW_HOST_FRAME";
    std::uint64_t guest_cycles = 0u;
    std::uint64_t frame_intervals = 0u;
    std::uint64_t pvr_frames = 0u;
    std::uint32_t frame_width = 0u;
    std::uint32_t frame_height = 0u;
    std::size_t frame_rgba_bytes = 0u;
    std::uint64_t audio_buffers = 0u;
    std::uint64_t audio_frames = 0u;
    std::uint64_t maple_transactions = 0u;
    std::uint64_t dma_units = 0u;
    std::uint64_t interrupts = 0u;
    std::uint64_t scheduler_jitter = 0u;
    std::uint64_t invalidations = 0u;
    std::uint64_t replay_events = 0u;
    std::uint64_t state_hash = 0u;
    std::uint64_t audio_hash = 0u;
    std::uint64_t frame_hash = 0u;
    std::uint64_t silent_failures = 0u;
    std::string console_output;
};

[[nodiscard]] const char* homebrew_artifact_kind_name(HomebrewArtifactKind kind) noexcept;
[[nodiscard]] std::vector<HomebrewArtifact> build_homebrew_corpus();
void require_valid_homebrew_corpus(const std::vector<HomebrewArtifact>& corpus);
[[nodiscard]] std::string format_homebrew_corpus_json(const std::vector<HomebrewArtifact>& corpus);

[[nodiscard]] FirmwareHandoffReport run_synthetic_firmware_handoff();
[[nodiscard]] HomebrewHostFrameReport run_homebrew_host_frame();
[[nodiscard]] std::string format_homebrew_host_frame_json(const HomebrewHostFrameReport& report);

} // namespace katana::phase9
