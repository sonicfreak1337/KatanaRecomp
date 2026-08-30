#include "katana/runtime/block_abi.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <optional>
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

struct CodeAddressLookupCacheEntry final {
    std::uint64_t begin = 0u;
    std::uint64_t end = 0u;
    std::int64_t delta = 0;
};

constexpr std::size_t code_address_lookup_cache_size = 64u;
static_assert((code_address_lookup_cache_size &
               (code_address_lookup_cache_size - 1u)) == 0u);
thread_local std::array<CodeAddressLookupCacheEntry,
                        code_address_lookup_cache_size>
    relocate_code_address_cache;
thread_local std::array<CodeAddressLookupCacheEntry,
                        code_address_lookup_cache_size>
    unrelocate_code_address_cache;
thread_local CodeAddressLookupCacheEntry relocate_code_address_recent;
thread_local CodeAddressLookupCacheEntry unrelocate_code_address_recent;

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

void invalidate_code_address_lookup_cache() noexcept {
    // Mapping changes are rare. An empty interval is sufficient to invalidate
    // an entry, avoiding both a generation load on every lookup and needless
    // writes to the cached range/delta fields.
    for (auto& cached : relocate_code_address_cache) cached.end = 0u;
    for (auto& cached : unrelocate_code_address_cache) cached.end = 0u;
    relocate_code_address_recent.end = 0u;
    unrelocate_code_address_recent.end = 0u;
}

#if defined(_MSC_VER)
#define KATANA_BLOCK_ABI_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define KATANA_BLOCK_ABI_NOINLINE __attribute__((noinline))
#else
#define KATANA_BLOCK_ABI_NOINLINE
#endif

template <bool Reverse>
KATANA_BLOCK_ABI_NOINLINE std::uint32_t
lookup_code_address_slow(const std::uint32_t address) noexcept {
    auto& cache = Reverse ? unrelocate_code_address_cache
                          : relocate_code_address_cache;
    auto& recent = Reverse ? unrelocate_code_address_recent
                           : relocate_code_address_recent;
    const auto cache_index =
        ((static_cast<std::size_t>(address) >> 12u) ^
         (static_cast<std::size_t>(address) >> 20u)) &
        (code_address_lookup_cache_size - 1u);
    auto& cached = cache[cache_index];
    if (static_cast<std::uint64_t>(address) >= cached.begin &&
        static_cast<std::uint64_t>(address) < cached.end) {
        recent = cached;
        return static_cast<std::uint32_t>(
            static_cast<std::int64_t>(address) + cached.delta);
    }

    std::uint64_t begin = 0u;
    std::uint64_t end = guest_address_space_extent;
    std::int64_t delta = 0;
    std::optional<std::size_t> selected;
    for (std::size_t index = active_code_address_mappings.size();
         index != 0u; --index) {
        const auto& mapping =
            active_code_address_mappings[index - 1u].mapping;
        const auto start = Reverse ? mapping.runtime_start
                                   : mapping.source_start;
        if (!contains(address, start, mapping.extent)) continue;
        selected = index - 1u;
        begin = start;
        end = static_cast<std::uint64_t>(start) + mapping.extent;
        delta = Reverse
                    ? static_cast<std::int64_t>(mapping.source_start) -
                          mapping.runtime_start
                    : static_cast<std::int64_t>(mapping.runtime_start) -
                          mapping.source_start;
        break;
    }

    const auto constrain_gap = [&](const CodeAddressMapping& mapping) {
        const auto start = Reverse ? mapping.runtime_start
                                   : mapping.source_start;
        const auto mapping_end =
            static_cast<std::uint64_t>(start) + mapping.extent;
        if (mapping_end <= address)
            begin = std::max(begin, mapping_end);
        else if (start > address)
            end = std::min(end, static_cast<std::uint64_t>(start));
    };
    if (selected.has_value()) {
        // A newer overlapping mapping has priority. Shrink the cached interval
        // around this address so a neighboring address can never reuse the
        // older result across such an overlap.
        for (std::size_t index = *selected + 1u;
             index < active_code_address_mappings.size(); ++index)
            constrain_gap(active_code_address_mappings[index].mapping);
    } else {
        for (const auto& mapping : active_code_address_mappings)
            constrain_gap(mapping.mapping);
    }
    cached = {begin, end, delta};
    recent = cached;
    return static_cast<std::uint32_t>(
        static_cast<std::int64_t>(address) + delta);
}

template <bool Reverse>
std::uint32_t lookup_code_address(const std::uint32_t address) noexcept {
    auto& recent = Reverse ? unrelocate_code_address_recent
                           : relocate_code_address_recent;
    if (static_cast<std::uint64_t>(address) >= recent.begin &&
        static_cast<std::uint64_t>(address) < recent.end)
        return static_cast<std::uint32_t>(
            static_cast<std::int64_t>(address) + recent.delta);
    return lookup_code_address_slow<Reverse>(address);
}

#undef KATANA_BLOCK_ABI_NOINLINE

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
    return lookup_code_address<false>(source_address);
}

std::uint32_t unrelocate_code_address(const std::uint32_t runtime_address) noexcept {
    return lookup_code_address<true>(runtime_address);
}

ScopedCodeAddressMapping::ScopedCodeAddressMapping(const CodeAddressMapping mapping) {
    validate_code_address_mapping(mapping);
    token_ = allocate_mapping_token();
    active_code_address_mappings.push_back({token_, mapping});
    invalidate_code_address_lookup_cache();
}

ScopedCodeAddressMapping::~ScopedCodeAddressMapping() noexcept {
    const auto found = std::find_if(active_code_address_mappings.rbegin(),
                                    active_code_address_mappings.rend(),
                                    [this](const auto& entry) { return entry.token == token_; });
    if (found == active_code_address_mappings.rend()) return;
    active_code_address_mappings.erase(std::next(found).base());
    invalidate_code_address_lookup_cache();
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
    const bool exception_edge =
        kind == BlockEndKind::Exception &&
        context.exception_generation_on_entry.has_value() &&
        cpu.exception_generation != *context.exception_generation_on_entry &&
        cpu.last_exception_generation == cpu.exception_generation &&
        cpu.last_exception_generation != 0u;
    auto actual_source =
        exception_edge
            ? BlockAddress{cpu.last_exception_instruction_pc,
                           cpu.last_exception_instruction_physical_pc}
            : source;
    if (!exception_edge && context.exception_generation_on_entry.has_value()) {
        const auto block_offset =
            actual_source.virtual_address - cpu.active_block_virtual_start;
        if (cpu.active_block_size != 0u && block_offset < cpu.active_block_size) {
            actual_source.physical_address =
                cpu.active_block_physical_start + block_offset;
        }
    }
    return {kind,
            actual_source,
            target,
            context.scheduler_cycle,
            context.scheduler_event_budget,
            exception_edge ? cpu.last_exception_cause : ExceptionCause::None,
            exception_edge ? cpu.exception_in_delay_slot
                           : context.delay_slot_owner_pc.has_value(),
            exception_edge ? cpu.last_exception_owner_pc
                           : effective_exception_pc(cpu, context),
            exception_edge ? cpu.last_exception_instruction_pc : actual_source.virtual_address,
            exception_edge ? cpu.last_exception_generation : 0u};
}

std::uint32_t effective_exception_pc(const CpuState& cpu,
                                     const BlockExecutionContext& context) noexcept {
    if (context.exception_generation_on_entry.has_value() &&
        cpu.exception_generation != *context.exception_generation_on_entry &&
        cpu.last_exception_generation == cpu.exception_generation &&
        cpu.last_exception_generation != 0u)
        return cpu.last_exception_owner_pc;
    return context.delay_slot_owner_pc.value_or(cpu.pc);
}

std::string stable_block_identity(const BlockAddress& address) {
    std::ostringstream stream;
    stream << "v" << std::uppercase << std::hex << std::setfill('0') << std::setw(8)
           << address.virtual_address << "-p" << std::setw(8) << address.physical_address;
    return stream.str();
}

} // namespace katana::runtime
