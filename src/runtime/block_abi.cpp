#include "katana/runtime/block_abi.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace katana::runtime {
namespace {

struct ActiveCodeAddressMapping {
    std::uint64_t token = 0u;
    CodeAddressMapping mapping;
};

thread_local std::vector<ActiveCodeAddressMapping> active_code_address_mappings;
thread_local std::uint64_t next_code_address_mapping_token = 1u;

constexpr std::uint64_t guest_address_space_extent =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;

bool contains(const std::uint32_t address,
              const std::uint32_t start,
              const std::uint32_t extent) noexcept {
    return static_cast<std::uint64_t>(address) >= start &&
           static_cast<std::uint64_t>(address) < static_cast<std::uint64_t>(start) + extent;
}

std::uint64_t allocate_mapping_token() noexcept {
    auto token = next_code_address_mapping_token++;
    if (token == 0u) token = next_code_address_mapping_token++;
    return token;
}

bool requires_target(const BlockEndKind kind) noexcept {
    switch (kind) {
    case BlockEndKind::Fallthrough:
    case BlockEndKind::StaticBranch:
    case BlockEndKind::ConditionalBranch:
    case BlockEndKind::DynamicBranch:
    case BlockEndKind::Call:
    case BlockEndKind::Return:
    case BlockEndKind::ExceptionReturn:
        return true;
    case BlockEndKind::Sleep:
    case BlockEndKind::Exception:
    case BlockEndKind::InterruptSafepoint:
        return false;
    }
    return false;
}

} // namespace

void validate_code_address_mapping(const CodeAddressMapping& mapping) {
    if (mapping.extent == 0u) {
        throw std::invalid_argument("AOT-Codeadressabbildung benoetigt eine Ausdehnung.");
    }
    if (static_cast<std::uint64_t>(mapping.source_start) + mapping.extent >
        guest_address_space_extent) {
        throw std::length_error(
            "AOT-Codeadressabbildung laeuft am Quellbereich ueber 32 Bit hinaus.");
    }
    if (static_cast<std::uint64_t>(mapping.runtime_start) + mapping.extent >
        guest_address_space_extent) {
        throw std::length_error(
            "AOT-Codeadressabbildung laeuft am Runtimebereich ueber 32 Bit hinaus.");
    }
}

std::uint32_t relocate_code_address(const std::uint32_t source_address) noexcept {
    for (auto mapping = active_code_address_mappings.rbegin();
         mapping != active_code_address_mappings.rend();
         ++mapping) {
        if (!contains(source_address,
                      mapping->mapping.source_start,
                      mapping->mapping.extent))
            continue;
        const auto offset = source_address - mapping->mapping.source_start;
        return mapping->mapping.runtime_start + offset;
    }
    return source_address;
}

std::uint32_t unrelocate_code_address(const std::uint32_t runtime_address) noexcept {
    for (auto mapping = active_code_address_mappings.rbegin();
         mapping != active_code_address_mappings.rend();
         ++mapping) {
        if (!contains(runtime_address,
                      mapping->mapping.runtime_start,
                      mapping->mapping.extent))
            continue;
        const auto offset = runtime_address - mapping->mapping.runtime_start;
        return mapping->mapping.source_start + offset;
    }
    return runtime_address;
}

ScopedCodeAddressMapping::ScopedCodeAddressMapping(const CodeAddressMapping mapping) {
    validate_code_address_mapping(mapping);
    token_ = allocate_mapping_token();
    active_code_address_mappings.push_back({token_, mapping});
}

ScopedCodeAddressMapping::~ScopedCodeAddressMapping() noexcept {
    const auto found = std::find_if(active_code_address_mappings.rbegin(),
                                    active_code_address_mappings.rend(),
                                    [this](const auto& entry) { return entry.token == token_; });
    if (found == active_code_address_mappings.rend()) return;
    active_code_address_mappings.erase(std::next(found).base());
}

void validate_block_entry(const CpuState& cpu,
                          const BlockExecutionContext& context,
                          const BlockEntry& entry) {
    if (entry.required_runtime_abi != abi_version) {
        throw std::invalid_argument("Blockeintritt fordert Runtime-ABI " +
                                    std::to_string(entry.required_runtime_abi) +
                                    ", vorhanden ist " + std::to_string(abi_version) + '.');
    }
    if (entry.required_block_abi != block_abi_version) {
        throw std::invalid_argument("Blockeintritt fordert Block-ABI " +
                                    std::to_string(entry.required_block_abi) + ", vorhanden ist " +
                                    std::to_string(block_abi_version) + '.');
    }
    if (context.sync_point != BlockSyncPoint::Entry &&
        context.sync_point != BlockSyncPoint::BackendBoundary &&
        context.sync_point != BlockSyncPoint::FallbackBoundary) {
        throw std::invalid_argument(
            "Blockeintritt liegt nicht an einem synchronisierten Grenzpunkt.");
    }
    if (cpu.pc != entry.address.virtual_address) {
        throw std::invalid_argument("Blockeintritt stimmt nicht mit dem Gast-PC ueberein.");
    }
}

BlockExit make_block_exit(const CpuState& cpu,
                          BlockExecutionContext& context,
                          const BlockEndKind kind,
                          const BlockAddress source,
                          const std::optional<BlockAddress> target) {
    if (requires_target(kind) && !target) {
        throw std::invalid_argument("Der typisierte Blockaustritt benoetigt eine Gastzieladresse.");
    }
    context.sync_point = BlockSyncPoint::Exit;
    return {kind,
            source,
            target,
            context.scheduler_cycle,
            context.scheduler_event_budget,
            cpu.last_exception_cause,
            context.delay_slot_owner_pc.has_value(),
            effective_exception_pc(cpu, context)};
}

std::uint32_t effective_exception_pc(const CpuState& cpu,
                                     const BlockExecutionContext& context) noexcept {
    return context.delay_slot_owner_pc.value_or(cpu.pc);
}

std::string stable_block_identity(const BlockAddress& address) {
    std::ostringstream stream;
    stream << "v" << std::uppercase << std::hex << std::setfill('0') << std::setw(8)
           << address.virtual_address << "-p" << std::setw(8) << address.physical_address;
    return stream.str();
}

} // namespace katana::runtime
