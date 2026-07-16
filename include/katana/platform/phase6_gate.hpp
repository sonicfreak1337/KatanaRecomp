#pragma once

#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace katana::platform {

using Phase6BlockExecutor = void (*)(runtime::CpuState& cpu);

struct Phase6GateReport {
    std::string checkpoint = "SA_PHASE6_NOT_REACHED";
    bool gdi_loaded = false;
    std::size_t tracks_validated = 0u;
    bool iso9660_mounted = false;
    bool boot_metadata_read = false;
    bool boot_file_loaded = false;
    bool repeated_reads_match = false;
    bool main_executable_entered = false;
    std::uint64_t executed_blocks = 0u;
    std::uint64_t guest_cycles = 0u;
    std::uint64_t scheduler_events = 0u;
    std::uint64_t gdrom_completions = 0u;
    std::uint64_t tmu_events = 0u;
    std::uint64_t dma_events = 0u;
    std::uint64_t interrupts_delivered = 0u;
    std::uint64_t cache_invalidations = 0u;
    std::uint64_t indirect_dispatches = 0u;
    std::uint64_t fallbacks = 0u;
    std::uint64_t silent_failures = 0u;
    std::uint64_t pvr_frames = 0u;
    std::uint64_t audio_sample_frames = 0u;
    std::uint64_t maple_transactions = 0u;
    std::uint32_t last_guest_pc = 0u;
    std::uint64_t scheduler_cycle = 0u;
    std::size_t scheduler_pending_events = 0u;
};

[[nodiscard]] Phase6GateReport run_phase6_gate(const std::filesystem::path& descriptor_path,
                                               Phase6BlockExecutor execute_block,
                                               std::size_t block_instruction_count,
                                               std::uint64_t guest_cycle_budget = 64u);

[[nodiscard]] std::string serialize_phase6_gate_report(const Phase6GateReport& report);

void write_phase6_gate_report(const Phase6GateReport& report,
                              const std::filesystem::path& output_path);

} // namespace katana::platform
