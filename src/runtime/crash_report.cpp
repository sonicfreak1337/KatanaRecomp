#include "katana/runtime/crash_report.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {

bool stable_token(const std::string_view value, const bool empty_allowed = false) noexcept {
    if (value.empty()) return empty_allowed;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' || character == '_' ||
               character == '-';
    });
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

const char* block_end_name(const BlockEndKind kind) noexcept {
    switch (kind) {
    case BlockEndKind::Fallthrough:
        return "fallthrough";
    case BlockEndKind::StaticBranch:
        return "static-branch";
    case BlockEndKind::ConditionalBranch:
        return "conditional-branch";
    case BlockEndKind::DynamicBranch:
        return "dynamic-branch";
    case BlockEndKind::Call:
        return "call";
    case BlockEndKind::Return:
        return "return";
    case BlockEndKind::ExceptionReturn:
        return "exception-return";
    case BlockEndKind::Sleep:
        return "sleep";
    case BlockEndKind::Exception:
        return "exception";
    case BlockEndKind::InterruptSafepoint:
        return "interrupt-safepoint";
    }
    return "unknown";
}

void optional_address(std::ostringstream& output, const std::optional<std::uint32_t> address) {
    address.has_value() ? output << '"' << hex32(*address) << '"' : output << "null";
}

void validate_context(const CrashReportContext& context) {
    if (!stable_token(context.stop_code) || !stable_token(context.block_provenance, true) ||
        !stable_token(context.dispatch_origin, true) ||
        !stable_token(context.dispatch_action, true)) {
        throw std::invalid_argument("Crashbericht enthaelt unportable Diagnosecodes.");
    }
    if (context.block_address.has_value() != context.block_variant.has_value() ||
        (context.block_address.has_value() && context.block_provenance.empty())) {
        throw std::invalid_argument(
            "Crashbericht braucht Blockadresse, -variante und -provenienz gemeinsam.");
    }
    const bool dispatch_present =
        context.dispatch_callsite.has_value() || context.dispatch_target.has_value();
    if (dispatch_present && (context.dispatch_origin.empty() || context.dispatch_action.empty())) {
        throw std::invalid_argument(
            "Crashbericht braucht fuer Dispatchadressen Herkunft und Aktion.");
    }
}

} // namespace

CrashReport capture_crash_report(const CpuState& cpu, CrashReportContext context) {
    validate_context(context);
    if (context.block_address.has_value()) {
        context.block_address->physical_address =
            canonical_physical_address(context.block_address->physical_address);
    }
    if (cpu.exception_in_delay_slot && !context.delay_slot_owner_pc.has_value()) {
        context.delay_slot_owner_pc = cpu.spc;
    }
    CrashReport report;
    report.context = std::move(context);
    report.virtual_pc = cpu.pc;
    report.canonical_pc = report.context.canonical_pc.value_or(canonical_physical_address(cpu.pc));
    report.registers = cpu.r;
    report.banked_registers = cpu.r_bank;
    report.pr = cpu.pr;
    report.sr = cpu.read_sr();
    report.fpscr = cpu.read_fpscr();
    report.spc = cpu.spc;
    report.ssr = cpu.ssr;
    report.sgr = cpu.sgr;
    report.tea = cpu.tea;
    report.expevt = cpu.expevt;
    report.intevt = cpu.intevt;
    report.trap_pending = cpu.trap_pending;
    report.exception_cause = cpu.last_exception_cause;
    report.exception_in_delay_slot = cpu.exception_in_delay_slot;
    return report;
}

std::string serialize_crash_report(const CrashReport& report) {
    const auto& context = report.context;
    validate_context(context);
    std::ostringstream output;
    output << "{\"schema\":\"katana-crash-report\",\"report_version\":1"
           << ",\"crash_version\":" << crash_report_schema_version
           << ",\"status\":\"failure\",\"stop_code\":\"" << context.stop_code << '"'
           << ",\"virtual_pc\":\"" << hex32(report.virtual_pc) << '"' << ",\"canonical_pc\":\""
           << hex32(report.canonical_pc) << '"' << ",\"cpu\":{\"r\":[";
    for (std::size_t index = 0u; index < report.registers.size(); ++index) {
        if (index != 0u) output << ',';
        output << '"' << hex32(report.registers[index]) << '"';
    }
    output << "],\"r_bank\":[";
    for (std::size_t index = 0u; index < report.banked_registers.size(); ++index) {
        if (index != 0u) output << ',';
        output << '"' << hex32(report.banked_registers[index]) << '"';
    }
    output << "],\"pr\":\"" << hex32(report.pr) << "\",\"sr\":\"" << hex32(report.sr)
           << "\",\"fpscr\":\"" << hex32(report.fpscr) << "\",\"spc\":\"" << hex32(report.spc)
           << "\",\"ssr\":\"" << hex32(report.ssr) << "\",\"sgr\":\"" << hex32(report.sgr)
           << "\",\"tea\":\"" << hex32(report.tea) << "\",\"expevt\":\"" << hex32(report.expevt)
           << "\",\"intevt\":\"" << hex32(report.intevt)
           << "\",\"trap_pending\":" << (report.trap_pending ? "true" : "false")
           << ",\"exception_cause\":\"" << exception_cause_name(report.exception_cause)
           << "\",\"exception_in_delay_slot\":"
           << (report.exception_in_delay_slot ? "true" : "false") << '}'
           << ",\"delay_slot_owner_pc\":";
    optional_address(output, context.delay_slot_owner_pc);
    output << ",\"block\":";
    if (context.block_address.has_value() && context.block_variant.has_value()) {
        const auto& address = *context.block_address;
        const auto& variant = *context.block_variant;
        output << "{\"virtual_address\":\"" << hex32(address.virtual_address)
               << "\",\"physical_address\":\"" << hex32(address.physical_address)
               << "\",\"provenance\":\"" << context.block_provenance << "\",\"end_kind\":\""
               << block_end_name(context.block_end_kind)
               << "\",\"variant\":{\"address_space_generation\":"
               << variant.address_space_generation
               << ",\"mmu_generation\":" << variant.mmu_generation
               << ",\"watchpoint_generation\":" << variant.watchpoint_generation
               << ",\"fpscr_mode\":" << variant.fpscr_mode
               << ",\"runtime_generation\":" << variant.runtime_generation << "}}";
    } else {
        output << "null";
    }
    output << ",\"scheduler\":{\"cycle\":" << context.scheduler_cycle
           << ",\"pending_events\":" << context.scheduler_pending_events << '}'
           << ",\"dispatch\":{\"callsite\":";
    optional_address(output, context.dispatch_callsite);
    output << ",\"target\":";
    optional_address(output, context.dispatch_target);
    output << ",\"pr\":\"" << hex32(report.pr) << "\",\"origin\":\"" << context.dispatch_origin
           << "\",\"action\":\"" << context.dispatch_action << "\"}}\n";
    return output.str();
}

const char* exception_cause_name(const ExceptionCause cause) noexcept {
    switch (cause) {
    case ExceptionCause::None:
        return "none";
    case ExceptionCause::Trap:
        return "trap";
    case ExceptionCause::IllegalInstruction:
        return "illegal-instruction";
    case ExceptionCause::SlotIllegalInstruction:
        return "slot-illegal-instruction";
    case ExceptionCause::FpuDisabled:
        return "fpu-disabled";
    case ExceptionCause::SlotFpuDisabled:
        return "slot-fpu-disabled";
    case ExceptionCause::AddressErrorRead:
        return "address-error-read";
    case ExceptionCause::AddressErrorWrite:
        return "address-error-write";
    case ExceptionCause::BusErrorRead:
        return "bus-error-read";
    case ExceptionCause::BusErrorWrite:
        return "bus-error-write";
    case ExceptionCause::Interrupt:
        return "interrupt";
    }
    return "unknown";
}

} // namespace katana::runtime
