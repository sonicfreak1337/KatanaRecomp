#include "katana/phase9/performance.hpp"

#include "katana/codegen/backend.hpp"
#include "katana/io/json_report.hpp"
#include "katana/runtime/abi.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace katana::phase9 {
namespace {

const char* profiling_mode_name(const ProfilingMode mode) noexcept {
    switch (mode) {
    case ProfilingMode::Disabled:
        return "disabled";
    case ProfilingMode::Exact:
        return "exact";
    case ProfilingMode::Sampled:
        return "sampled";
    }
    return "unknown";
}

const char* fallback_reason_name(const ProfileFallbackReason reason) noexcept {
    switch (reason) {
    case ProfileFallbackReason::UnknownInstruction:
        return "unknown-instruction";
    case ProfileFallbackReason::UnknownIndirectTarget:
        return "unknown-indirect-target";
    case ProfileFallbackReason::UnsupportedRuntimeState:
        return "unsupported-runtime-state";
    case ProfileFallbackReason::InterpreterBoundary:
        return "interpreter-boundary";
    }
    return "unknown";
}

const char* guard_outcome_name(const GuardOutcome outcome) noexcept {
    switch (outcome) {
    case GuardOutcome::Hit:
        return "hit";
    case GuardOutcome::Disabled:
        return "disabled";
    case GuardOutcome::Region:
        return "region";
    case GuardOutcome::Alignment:
        return "alignment";
    case GuardOutcome::Permission:
        return "permission";
    case GuardOutcome::Mmu:
        return "mmu";
    case GuardOutcome::Alias:
        return "alias";
    case GuardOutcome::Watchpoint:
        return "watchpoint";
    case GuardOutcome::Mmio:
        return "mmio";
    case GuardOutcome::AddressSpace:
        return "address-space";
    case GuardOutcome::CodeGeneration:
        return "code-generation";
    }
    return "unknown";
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

} // namespace

bool ProfileEdge::operator<(const ProfileEdge& other) const noexcept {
    return source < other.source || (source == other.source && target < other.target);
}

ExecutionProfiler::ExecutionProfiler(const ProfilingMode mode,
                                     ExecutionProfileIdentity identity,
                                     const std::uint64_t sample_period)
    : sample_period_(sample_period) {
    if (sample_period == 0u) {
        throw std::invalid_argument("Profiling-Sampleperiode darf nicht null sein.");
    }
    snapshot_.mode = mode;
    snapshot_.identity = std::move(identity);
}

bool ExecutionProfiler::selected() noexcept {
    const auto current = event_index_++;
    return snapshot_.mode == ProfilingMode::Exact ||
           (snapshot_.mode == ProfilingMode::Sampled && current % sample_period_ == 0u);
}

void ExecutionProfiler::record_block(const std::uint32_t address) {
    if (selected()) ++snapshot_.blocks[address];
}

void ExecutionProfiler::record_edge(const std::uint32_t source, const std::uint32_t target) {
    if (selected()) ++snapshot_.edges[{source, target}];
}

void ExecutionProfiler::record_indirect_call(const std::uint32_t callsite) {
    if (selected()) ++snapshot_.indirect_callsites[callsite];
}

void ExecutionProfiler::record_fallback(const ProfileFallbackReason reason) {
    if (selected()) ++snapshot_.fallbacks[fallback_reason_name(reason)];
}

void ExecutionProfiler::record_invalidation(const std::uint32_t physical_page) {
    if (selected()) ++snapshot_.invalidations[physical_page];
}

void ExecutionProfiler::record_guard(const GuardOutcome outcome) {
    if (selected()) ++snapshot_.guards[outcome];
}

const ProfileSnapshot& ExecutionProfiler::snapshot() const noexcept {
    return snapshot_;
}

void require_profile_identity(const ProfileSnapshot& profile,
                              const ExecutionProfileIdentity& expected) {
    const auto valid_hash =
        expected.input_sha256.size() == 64u &&
        std::all_of(expected.input_sha256.begin(),
                    expected.input_sha256.end(),
                    [](const unsigned char character) { return std::isxdigit(character) != 0; });
    if (profile.identity != expected || !valid_hash ||
        expected.runtime_abi != katana::runtime::abi_version ||
        expected.backend_abi != katana::codegen::backend_interface_abi_version) {
        throw std::invalid_argument("Ausfuehrungsprofil passt nicht zu Eingabe oder ABI.");
    }
}

std::vector<std::pair<std::uint32_t, std::uint64_t>> hot_blocks(const ProfileSnapshot& profile) {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> result(profile.blocks.begin(),
                                                                profile.blocks.end());
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.second > right.second ||
               (left.second == right.second && left.first < right.first);
    });
    return result;
}

std::vector<std::pair<ProfileEdge, std::uint64_t>> hot_edges(const ProfileSnapshot& profile) {
    std::vector<std::pair<ProfileEdge, std::uint64_t>> result(profile.edges.begin(),
                                                              profile.edges.end());
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) return left.second > right.second;
        if (left.first.source != right.first.source) {
            return left.first.source < right.first.source;
        }
        return left.first.target < right.first.target;
    });
    return result;
}

std::string format_execution_profile_json(const ProfileSnapshot& profile) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-execution-profile\",\"version\":"
           << execution_profile_schema_version << ",\"mode\":\""
           << profiling_mode_name(profile.mode) << "\",\"identity\":{\"input_sha256\":"
           << katana::io::quote_json(profile.identity.input_sha256)
           << ",\"runtime_abi\":" << profile.identity.runtime_abi
           << ",\"backend_abi\":" << profile.identity.backend_abi << "},\"hot_blocks\":[";
    const auto blocks = hot_blocks(profile);
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (index != 0u) output << ',';
        output << "{\"address\":\"" << hex32(blocks[index].first)
               << "\",\"count\":" << blocks[index].second << '}';
    }
    output << "],\"hot_edges\":[";
    const auto edges = hot_edges(profile);
    for (std::size_t index = 0u; index < edges.size(); ++index) {
        if (index != 0u) output << ',';
        output << "{\"source\":\"" << hex32(edges[index].first.source) << "\",\"target\":\""
               << hex32(edges[index].first.target) << "\",\"count\":" << edges[index].second << '}';
    }
    output << "],\"indirect_callsites\":[";
    std::size_t callsite_index = 0u;
    for (const auto& [callsite, count] : profile.indirect_callsites) {
        if (callsite_index++ != 0u) output << ',';
        output << "{\"callsite\":\"" << hex32(callsite) << "\",\"count\":" << count << '}';
    }
    output << "],\"fallbacks\":[";
    std::size_t fallback_index = 0u;
    for (const auto& [reason, count] : profile.fallbacks) {
        if (fallback_index++ != 0u) output << ',';
        output << "{\"reason\":" << katana::io::quote_json(reason) << ",\"count\":" << count << '}';
    }
    output << "],\"invalidations\":[";
    std::size_t invalidation_index = 0u;
    for (const auto& [page, count] : profile.invalidations) {
        if (invalidation_index++ != 0u) output << ',';
        output << "{\"physical_page\":\"" << hex32(page) << "\",\"count\":" << count << '}';
    }
    output << "],\"guards\":[";
    std::size_t guard_index = 0u;
    for (const auto& [outcome, count] : profile.guards) {
        if (guard_index++ != 0u) output << ',';
        output << "{\"outcome\":\"" << guard_outcome_name(outcome) << "\",\"count\":" << count
               << '}';
    }
    output << "]}\n";
    return output.str();
}

GuardOutcome evaluate_fast_memory_guard(const FastMemoryGuard& guard, const bool write) noexcept {
    if (!guard.enabled) return GuardOutcome::Disabled;
    if (!guard.linear_ram) return GuardOutcome::Region;
    if (!guard.aligned) return GuardOutcome::Alignment;
    if (write && !guard.writable) return GuardOutcome::Permission;
    if (!guard.mmu_disabled) return GuardOutcome::Mmu;
    if (!guard.alias_stable) return GuardOutcome::Alias;
    if (!guard.watchpoints_absent) return GuardOutcome::Watchpoint;
    if (!guard.mmio_absent) return GuardOutcome::Mmio;
    if (guard.address_space_generation != guard.expected_address_space_generation) {
        return GuardOutcome::AddressSpace;
    }
    if (guard.code_generation != guard.expected_code_generation) {
        return GuardOutcome::CodeGeneration;
    }
    return GuardOutcome::Hit;
}

GuardedMemoryFastpath::GuardedMemoryFastpath(runtime::Memory& memory,
                                             std::shared_ptr<runtime::LinearMemoryDevice> linear,
                                             const std::uint32_t base_address,
                                             runtime::ExecutableCodeTracker* code_tracker,
                                             ExecutionProfiler* profiler)
    : memory_(memory), linear_(std::move(linear)), base_address_(base_address),
      code_tracker_(code_tracker), profiler_(profiler) {
    if (!linear_) throw std::invalid_argument("RAM-Fastpath braucht ein lineares Speichergeraet.");
}

std::size_t GuardedMemoryFastpath::offset(const std::uint32_t address) const {
    if (address < base_address_ ||
        static_cast<std::uint64_t>(address - base_address_) + 4u > linear_->size()) {
        throw std::out_of_range("Fastpath-Adresse liegt ausserhalb des linearen RAMs.");
    }
    return static_cast<std::size_t>(address - base_address_);
}

std::uint32_t GuardedMemoryFastpath::read_u32(const std::uint32_t address,
                                              const FastMemoryGuard& guard) {
    auto verified = guard;
    verified.linear_ram = memory_.maps_device(address, 4u, linear_.get());
    verified.aligned = (address & 3u) == 0u;
    verified.watchpoints_absent = memory_.watchpoint_count() == 0u && !memory_.has_trace_handler();
    verified.mmio_absent = verified.linear_ram;
    const auto outcome = evaluate_fast_memory_guard(verified, false);
    if (profiler_) profiler_->record_guard(outcome);
    if (outcome != GuardOutcome::Hit) {
        ++misses_;
        return memory_.read_u32(address);
    }
    const auto position = offset(address);
    const auto bytes = linear_->bytes();
    ++hits_;
    return static_cast<std::uint32_t>(bytes[position]) |
           (static_cast<std::uint32_t>(bytes[position + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[position + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[position + 3u]) << 24u);
}

void GuardedMemoryFastpath::write_u32(const std::uint32_t address,
                                      const std::uint32_t value,
                                      const FastMemoryGuard& guard) {
    auto verified = guard;
    verified.linear_ram = memory_.maps_device(address, 4u, linear_.get());
    verified.aligned = (address & 3u) == 0u;
    verified.watchpoints_absent = memory_.watchpoint_count() == 0u && !memory_.has_trace_handler();
    verified.mmio_absent = verified.linear_ram;
    verified.writable = false;
    for (std::size_t index = 0u; index < memory_.region_count(); ++index) {
        const auto& region = memory_.region(index);
        const auto end = static_cast<std::uint64_t>(address) + 4u;
        const auto region_end = static_cast<std::uint64_t>(region.base_address) + region.size;
        if (address >= region.base_address && end <= region_end) {
            verified.writable = region.access == runtime::MemoryRegionAccess::ReadWrite;
            break;
        }
    }
    bool changed = true;
    if (verified.linear_ram) {
        const auto position = offset(address);
        const auto bytes = linear_->bytes();
        const auto previous = static_cast<std::uint32_t>(bytes[position]) |
                              (static_cast<std::uint32_t>(bytes[position + 1u]) << 8u) |
                              (static_cast<std::uint32_t>(bytes[position + 2u]) << 16u) |
                              (static_cast<std::uint32_t>(bytes[position + 3u]) << 24u);
        changed = previous != value;
    }
    const auto outcome = evaluate_fast_memory_guard(verified, true);
    if (profiler_) profiler_->record_guard(outcome);
    if (outcome != GuardOutcome::Hit) {
        ++misses_;
        memory_.write_u32(address, value);
        if (code_tracker_ != nullptr && !memory_.has_guest_write_observer())
            static_cast<void>(
                code_tracker_->observe_write(address, 4u, runtime::CodeWriteSource::Cpu, changed));
        return;
    }
    static_cast<void>(offset(address));
    memory_.write_u32(address, value);
    if (code_tracker_ != nullptr && !memory_.has_guest_write_observer())
        static_cast<void>(
            code_tracker_->observe_write(address, 4u, runtime::CodeWriteSource::Cpu, changed));
    ++hits_;
}

std::uint64_t GuardedMemoryFastpath::hits() const noexcept {
    return hits_;
}

std::uint64_t GuardedMemoryFastpath::misses() const noexcept {
    return misses_;
}

runtime::IndirectDispatchResult
MonomorphicDispatchCache::dispatch(runtime::CpuState& cpu,
                                   const runtime::RuntimeBlockTable& table,
                                   const runtime::IndirectDispatchRequest& request,
                                   const std::uint64_t block_generation,
                                   ExecutionProfiler* profiler) {
    if (profiler) profiler->record_indirect_call(request.callsite);
    const auto target =
        request.kind == runtime::IndirectDispatchKind::Return ? cpu.pr : request.target;
    if (entry_ && entry_->callsite == request.callsite && entry_->target == target &&
        entry_->variant == request.variant && entry_->block_generation == block_generation) {
        const auto handle = table.lookup(target, request.variant);
        const auto block = handle ? table.resolve(*handle) : std::nullopt;
        if (block &&
            runtime::stable_runtime_block_identity(block->get()) == entry_->block_identity) {
            ++hits_;
            cpu.pc = target;
            if (request.kind == runtime::IndirectDispatchKind::Call)
                cpu.pr = request.return_address;
            return {*handle,
                    target,
                    runtime::canonical_physical_address(target),
                    cpu.pc,
                    cpu.pr,
                    block->get().virtual_start != target,
                    "inline-cache"};
        }
    }
    ++misses_;
    auto result = runtime::dispatch_indirect(cpu, table, request);
    if (const auto block = table.resolve(result.block)) {
        entry_ = InlineCacheEntry{request.callsite,
                                  result.diagnostic_target,
                                  request.variant,
                                  block_generation,
                                  runtime::stable_runtime_block_identity(block->get())};
    }
    return result;
}

void MonomorphicDispatchCache::invalidate(const std::string& block_identity) noexcept {
    if (entry_ && entry_->block_identity == block_identity) entry_.reset();
}

void MonomorphicDispatchCache::clear() noexcept {
    entry_.reset();
}

std::uint64_t MonomorphicDispatchCache::hits() const noexcept {
    return hits_;
}

std::uint64_t MonomorphicDispatchCache::misses() const noexcept {
    return misses_;
}

const std::optional<InlineCacheEntry>& MonomorphicDispatchCache::entry() const noexcept {
    return entry_;
}

InlineDecision decide_inline(const std::uint64_t call_count,
                             const std::size_t callee_instructions,
                             const bool recursive,
                             const std::size_t remaining_code_budget) noexcept {
    if (recursive) return {false, callee_instructions, "recursive"};
    if (callee_instructions == 0u) return {false, 0u, "empty"};
    if (callee_instructions > 24u) return {false, callee_instructions, "callee-too-large"};
    if (callee_instructions > remaining_code_budget) {
        return {false, callee_instructions, "code-budget"};
    }
    if (call_count < 8u) return {false, callee_instructions, "cold-callsite"};
    return {true, callee_instructions, "hot-small-callee"};
}

} // namespace katana::phase9
