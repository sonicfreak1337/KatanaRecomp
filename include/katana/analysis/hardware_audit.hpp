#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace katana::analysis {

enum class DreamcastHardwareRegion : std::uint8_t {
    SystemBus,
    SystemAsic,
    Maple,
    GdRom,
    G1Dma,
    G2Dma,
    PvrDma,
    Pvr,
    Aica,
    AicaRtc,
    AicaRam,
    TaFifo,
    TaYuv,
    TaVram,
    Vram64,
    Vram32,
    StoreQueue,
    Sh4Mmu,
    Sh4Cache,
    Sh4Qacr,
    Sh4Io,
    Sh4Dmac,
    Sh4Rtc,
    Sh4Intc,
    Sh4Tmu,
    Sh4Scif,
    Sh4P4,
    Unknown
};

enum class HardwareAccessKind : std::uint8_t { Read, Write, Prefetch };

// Ordered from complete product support to a missing mapping. Address summaries retain the
// weakest capability observed across all access widths and directions at that address.
enum class HardwareRuntimeSupport : std::uint8_t {
    Implemented,
    Partial,
    KnownGap,
    Rejected,
    Unmapped
};

struct HardwareAccessReference {
    std::uint32_t instruction_address = 0u;
    std::uint32_t guest_address = 0u;
    std::uint32_t canonical_address = 0u;
    DreamcastHardwareRegion region = DreamcastHardwareRegion::Unknown;
    HardwareAccessKind kind = HardwareAccessKind::Read;
    std::uint8_t width = 0u;
    bool aperture_mapped = false;
    HardwareRuntimeSupport runtime_support = HardwareRuntimeSupport::Unmapped;
    std::string support_reason;
    std::string register_name;
};

struct HardwareAddressSummary {
    std::uint32_t guest_address = 0u;
    std::uint32_t canonical_address = 0u;
    DreamcastHardwareRegion region = DreamcastHardwareRegion::Unknown;
    bool aperture_mapped = false;
    HardwareRuntimeSupport runtime_support = HardwareRuntimeSupport::Unmapped;
    std::string support_reason;
    std::string register_name;
    std::size_t reads = 0u;
    std::size_t writes = 0u;
    std::size_t prefetches = 0u;
    std::vector<std::uint8_t> widths;
    std::vector<std::uint32_t> instruction_addresses;
};

struct HardwareInstructionDiagnostic {
    std::uint32_t address = 0u;
    std::uint16_t opcode = 0u;
    std::string reason;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    std::vector<std::uint32_t> incoming_addresses;
    std::vector<std::uint32_t> delay_slot_owners;
};

struct DreamcastHardwareAudit {
    std::size_t image_bytes = 0u;
    std::size_t reachable_instructions = 0u;
    std::size_t reachable_functions = 0u;
    std::size_t unknown_instructions = 0u;
    std::size_t memory_access_sites = 0u;
    std::size_t resolved_memory_access_sites = 0u;
    std::size_t unresolved_memory_access_sites = 0u;
    std::size_t implemented_addresses = 0u;
    std::size_t partial_addresses = 0u;
    std::size_t known_gap_addresses = 0u;
    std::size_t rejected_addresses = 0u;
    std::size_t unmapped_addresses = 0u;
    std::vector<HardwareInstructionDiagnostic> instruction_diagnostics;
    std::vector<HardwareAccessReference> references;
    std::vector<HardwareAddressSummary> addresses;
};

[[nodiscard]] DreamcastHardwareAudit
audit_dreamcast_hardware(const io::ExecutableImage& image,
                         const ControlFlowAnalysisResult& analysis);
[[nodiscard]] const char* dreamcast_hardware_region_name(DreamcastHardwareRegion region) noexcept;
[[nodiscard]] const char* hardware_access_kind_name(HardwareAccessKind kind) noexcept;
[[nodiscard]] const char* hardware_runtime_support_name(HardwareRuntimeSupport support) noexcept;
[[nodiscard]] std::string format_hardware_audit_text(const DreamcastHardwareAudit& audit);
[[nodiscard]] std::string format_hardware_audit_json(const DreamcastHardwareAudit& audit,
                                                     bool include_accesses = false);

} // namespace katana::analysis
