#include "katana/runtime/block_abi.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace katana::runtime {
namespace {

bool requires_target(const BlockEndKind kind) noexcept {
    switch (kind) {
    case BlockEndKind::Fallthrough:
    case BlockEndKind::StaticBranch:
    case BlockEndKind::ConditionalBranch:
    case BlockEndKind::DynamicBranch:
    case BlockEndKind::Call:
    case BlockEndKind::Return:
        return true;
    case BlockEndKind::Exception:
    case BlockEndKind::InterruptSafepoint:
        return false;
    }
    return false;
}

} // namespace

void validate_block_entry(
    const CpuState& cpu,
    const BlockExecutionContext& context,
    const BlockEntry& entry
) {
    if (entry.required_runtime_abi != abi_version) {
        throw std::invalid_argument("Blockeintritt fordert Runtime-ABI " +
            std::to_string(entry.required_runtime_abi) + ", vorhanden ist " +
            std::to_string(abi_version) + '.');
    }
    if (entry.required_block_abi != block_abi_version) {
        throw std::invalid_argument("Blockeintritt fordert Block-ABI " +
            std::to_string(entry.required_block_abi) + ", vorhanden ist " +
            std::to_string(block_abi_version) + '.');
    }
    if (context.sync_point != BlockSyncPoint::Entry &&
        context.sync_point != BlockSyncPoint::BackendBoundary &&
        context.sync_point != BlockSyncPoint::FallbackBoundary) {
        throw std::invalid_argument("Blockeintritt liegt nicht an einem synchronisierten Grenzpunkt.");
    }
    if (cpu.pc != entry.address.virtual_address) {
        throw std::invalid_argument("Blockeintritt stimmt nicht mit dem Gast-PC ueberein.");
    }
}

BlockExit make_block_exit(
    const CpuState& cpu,
    BlockExecutionContext& context,
    const BlockEndKind kind,
    const BlockAddress source,
    const std::optional<BlockAddress> target
) {
    if (requires_target(kind) && !target) {
        throw std::invalid_argument("Der typisierte Blockaustritt benoetigt eine Gastzieladresse.");
    }
    context.sync_point = BlockSyncPoint::Exit;
    return {
        kind,
        source,
        target,
        context.scheduler_cycle,
        context.scheduler_event_budget,
        cpu.last_exception_cause,
        context.delay_slot_owner_pc.has_value(),
        effective_exception_pc(cpu, context)
    };
}

std::uint32_t effective_exception_pc(
    const CpuState& cpu,
    const BlockExecutionContext& context
) noexcept {
    return context.delay_slot_owner_pc.value_or(cpu.pc);
}

std::string stable_block_identity(const BlockAddress& address) {
    std::ostringstream stream;
    stream << "v" << std::uppercase << std::hex << std::setfill('0')
           << std::setw(8) << address.virtual_address << "-p"
           << std::setw(8) << address.physical_address;
    return stream.str();
}

} // namespace katana::runtime
