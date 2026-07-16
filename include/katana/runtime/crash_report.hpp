#pragma once

#include "katana/runtime/block_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace katana::runtime {

inline constexpr std::uint32_t crash_report_schema_version = 1u;

struct CrashReportContext {
    std::string stop_code;
    std::optional<std::uint32_t> canonical_pc;
    std::optional<BlockAddress> block_address;
    std::optional<BlockVariantKey> block_variant;
    std::string block_provenance;
    BlockEndKind block_end_kind = BlockEndKind::Exception;
    std::optional<std::uint32_t> delay_slot_owner_pc;
    std::uint64_t scheduler_cycle = 0u;
    std::size_t scheduler_pending_events = 0u;
    std::optional<std::uint32_t> dispatch_callsite;
    std::optional<std::uint32_t> dispatch_target;
    std::string dispatch_origin;
    std::string dispatch_action;
};

struct CrashReport {
    CrashReportContext context;
    std::uint32_t virtual_pc = 0u;
    std::uint32_t canonical_pc = 0u;
    std::array<std::uint32_t, general_register_count> registers{};
    std::array<std::uint32_t, banked_register_count> banked_registers{};
    std::uint32_t pr = 0u;
    std::uint32_t sr = 0u;
    std::uint32_t fpscr = 0u;
    std::uint32_t spc = 0u;
    std::uint32_t ssr = 0u;
    std::uint32_t sgr = 0u;
    std::uint32_t tea = 0u;
    std::uint32_t expevt = 0u;
    std::uint32_t intevt = 0u;
    bool trap_pending = false;
    ExceptionCause exception_cause = ExceptionCause::None;
    bool exception_in_delay_slot = false;
};

[[nodiscard]] CrashReport capture_crash_report(const CpuState& cpu, CrashReportContext context);
[[nodiscard]] std::string serialize_crash_report(const CrashReport& report);
[[nodiscard]] const char* exception_cause_name(ExceptionCause cause) noexcept;

} // namespace katana::runtime
