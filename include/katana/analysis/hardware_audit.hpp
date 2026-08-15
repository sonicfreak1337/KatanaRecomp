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
    Sh4OnChipRam,
    Sh4Mmu,
    Sh4Cache,
    Sh4Exception,
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

enum class HardwareLoopClassification : std::uint8_t {
    Counter,
    RamPoll,
    MmioPoll,
    Mixed,
    Unknown
};

enum class HardwareLoopLocalProgressKind : std::uint8_t {
    IntegerInduction,
    AddressPostIncrement,
    PointerTraversal
};

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

inline constexpr std::uint8_t hardware_audit_access_read = 1u << 0u;
inline constexpr std::uint8_t hardware_audit_access_write = 1u << 1u;
inline constexpr std::uint8_t hardware_audit_access_prefetch = 1u << 2u;
inline constexpr std::uint8_t hardware_audit_width_u8 = 1u << 0u;
inline constexpr std::uint8_t hardware_audit_width_u16 = 1u << 1u;
inline constexpr std::uint8_t hardware_audit_width_u32 = 1u << 2u;
inline constexpr std::uint8_t hardware_audit_width_cache_line_32 = 1u << 3u;

struct UnresolvedMemoryInstructionSite final {
    std::uint32_t instruction_address = 0u;
    std::uint8_t access_mask = 0u;
    std::uint8_t width_mask = 0u;
};

struct HardwareLoopAccessEvidence {
    std::uint32_t instruction_address = 0u;
    std::uint32_t guest_address = 0u;
    std::uint32_t canonical_address = 0u;
    DreamcastHardwareRegion region = DreamcastHardwareRegion::Unknown;
    HardwareAccessKind kind = HardwareAccessKind::Read;
    std::uint8_t width = 0u;
    bool linear_memory = false;
    bool aperture_mapped = false;
    HardwareRuntimeSupport runtime_support = HardwareRuntimeSupport::Unmapped;
    bool guards_loop = false;
};

struct HardwareLoopWriteCandidate {
    std::uint32_t instruction_address = 0u;
    std::uint32_t guest_address = 0u;
    std::uint32_t canonical_address = 0u;
    std::uint8_t width = 0u;
};

struct HardwareLoopLocalProgressEvidence {
    std::uint32_t condition_instruction_address = 0u;
    std::uint32_t progress_instruction_address = 0u;
    std::uint8_t register_index = 0u;
    HardwareLoopLocalProgressKind kind =
        HardwareLoopLocalProgressKind::IntegerInduction;
};

struct HardwareNaturalLoop {
    std::uint32_t header_address = 0u;
    std::uint32_t latch_address = 0u;
    std::uint32_t backedge_instruction_address = 0u;
    HardwareLoopClassification classification = HardwareLoopClassification::Unknown;
    bool unresolved_guard_access = false;
    std::vector<std::uint32_t> unresolved_guard_read_instruction_addresses;
    std::vector<std::uint32_t> block_addresses;
    std::vector<std::uint32_t> counter_instruction_addresses;
    // A loop whose exit dependency changes through one of these bounded,
    // loop-local operations is a traversal/counter rather than an external
    // progress wait. Evidence is tied to the controlling condition and the
    // exact induction/pointer instruction; unrelated arithmetic cannot
    // discharge a provider requirement.
    std::vector<HardwareLoopLocalProgressEvidence> local_progress_evidence;
    std::vector<HardwareLoopAccessEvidence> accesses;
    // Resolved image-wide writes overlapping a resolved RAM-poll guard.  They
    // are candidates rather than assumed producers: the native provider still
    // has to prove owner, ABI, ordering and the value transition.
    std::vector<HardwareLoopWriteCandidate> matching_write_candidates;
    bool matching_write_candidates_truncated = false;
};

struct DreamcastHardwareAudit {
    std::string scope = "executable_image";
    std::size_t image_bytes = 0u;
    std::size_t reachable_instructions = 0u;
    std::size_t reachable_functions = 0u;
    std::size_t unknown_instructions = 0u;
    std::size_t memory_access_sites = 0u;
    std::size_t resolved_memory_access_sites = 0u;
    std::size_t unresolved_memory_access_sites = 0u;
    // Exact reachable instruction sites whose complete effective-address set
    // could not be proven. Native-port admission consumes the addresses,
    // rather than trusting only the aggregate count.
    std::vector<UnresolvedMemoryInstructionSite>
        unresolved_memory_instruction_sites;
    std::size_t implemented_addresses = 0u;
    std::size_t partial_addresses = 0u;
    std::size_t known_gap_addresses = 0u;
    std::size_t rejected_addresses = 0u;
    std::size_t unmapped_addresses = 0u;
    std::size_t unresolved_poll_guard_loops = 0u;
    std::vector<HardwareInstructionDiagnostic> instruction_diagnostics;
    std::vector<HardwareAccessReference> references;
    std::vector<HardwareAddressSummary> addresses;
    std::vector<HardwareNaturalLoop> loops;
};

[[nodiscard]] inline bool
is_unresolved_poll_guard_loop(const HardwareNaturalLoop& loop) noexcept {
    if (loop.classification != HardwareLoopClassification::Unknown) return false;
    if (!loop.unresolved_guard_read_instruction_addresses.empty()) return true;
    for (const auto& access : loop.accesses) {
        if (access.kind != HardwareAccessKind::Read) continue;
        if (access.guards_loop) return true;
    }
    return false;
}

[[nodiscard]] inline std::size_t
count_unresolved_poll_guard_loops(const std::vector<HardwareNaturalLoop>& loops) noexcept {
    std::size_t count = 0u;
    for (const auto& loop : loops) {
        if (is_unresolved_poll_guard_loop(loop)) ++count;
    }
    return count;
}

[[nodiscard]] DreamcastHardwareAudit
audit_dreamcast_hardware(const io::ExecutableImage& image,
                         const ControlFlowAnalysisResult& analysis);
[[nodiscard]] const char* dreamcast_hardware_region_name(DreamcastHardwareRegion region) noexcept;
[[nodiscard]] const char* hardware_access_kind_name(HardwareAccessKind kind) noexcept;
[[nodiscard]] const char*
hardware_loop_classification_name(HardwareLoopClassification classification) noexcept;
[[nodiscard]] const char* hardware_loop_local_progress_kind_name(
    HardwareLoopLocalProgressKind kind) noexcept;
[[nodiscard]] const char* hardware_runtime_support_name(HardwareRuntimeSupport support) noexcept;
[[nodiscard]] std::string format_hardware_audit_text(const DreamcastHardwareAudit& audit);
[[nodiscard]] std::string format_hardware_audit_json(const DreamcastHardwareAudit& audit,
                                                     bool include_accesses = false);

} // namespace katana::analysis
