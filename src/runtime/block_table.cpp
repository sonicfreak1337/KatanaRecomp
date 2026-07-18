#include "katana/runtime/block_table.hpp"

#include "katana/runtime/code_invalidation.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace katana::runtime {
namespace {

constexpr std::uint32_t physical_page_size = 4096u;

auto order_key(const RuntimeBlock& block) {
    return std::tie(block.virtual_start,
                    block.variant.address_space_generation,
                    block.variant.mmu_generation,
                    block.variant.watchpoint_generation,
                    block.variant.fpscr_mode,
                    block.variant.runtime_generation,
                    block.physical_origin,
                    block.provenance);
}

bool ranges_overlap(const std::uint32_t left_start,
                    const std::uint32_t left_size,
                    const std::uint32_t right_start,
                    const std::uint32_t right_size) noexcept {
    const auto left_end = static_cast<std::uint64_t>(left_start) + left_size;
    const auto right_end = static_cast<std::uint64_t>(right_start) + right_size;
    return left_start < right_end && right_start < left_end;
}

} // namespace

std::uint32_t canonical_physical_address(const std::uint32_t address) noexcept {
    return address < 0xE0000000u ? address & 0x1FFFFFFFu : address;
}

std::string stable_runtime_block_identity(const RuntimeBlock& block) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << "v" << std::setw(8) << block.virtual_start << "-p"
        << std::setw(8) << block.physical_origin << std::dec << "-s" << block.size << "-e"
        << static_cast<unsigned>(block.end_kind) << "-a" << block.variant.address_space_generation
        << "-m" << block.variant.mmu_generation << "-w" << block.variant.watchpoint_generation
        << "-f" << block.variant.fpscr_mode << "-r" << block.variant.runtime_generation << "-"
        << block.provenance;
    return out.str();
}

RuntimeBlockHandle RuntimeBlockTable::register_static(RuntimeBlock block) {
    if (static_sealed_) {
        throw std::logic_error("Statische Blockregistry ist bereits versiegelt.");
    }
    return insert(std::move(block), false);
}

std::vector<RuntimeBlockHandle>
RuntimeBlockTable::register_static_bulk(std::vector<RuntimeBlock> blocks) {
    if (static_sealed_) {
        throw std::logic_error("Statische Blockregistry ist bereits versiegelt.");
    }
    std::sort(blocks.begin(), blocks.end(), [](const auto& left, const auto& right) {
        return order_key(left) < order_key(right);
    });
    std::vector<RuntimeBlockHandle> handles;
    handles.reserve(blocks.size());
    for (auto& block : blocks) handles.push_back(insert(std::move(block), false));
    static_sealed_ = true;
    return handles;
}

void RuntimeBlockTable::seal_static() noexcept {
    static_sealed_ = true;
}

RuntimeBlockHandle RuntimeBlockTable::register_runtime(RuntimeBlock block) {
    static_sealed_ = true;
    return insert(std::move(block), true);
}

RuntimeBlockHandle RuntimeBlockTable::insert(RuntimeBlock block,
                                             const bool runtime_registered) {
    if (block.size == 0u || block.function == nullptr || block.provenance.empty()) {
        throw std::invalid_argument(
            "Blockeintrag benoetigt Groesse, Backendfunktion und Provenienz.");
    }
    const auto virtual_end = static_cast<std::uint64_t>(block.virtual_start) + block.size;
    if (virtual_end > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Blockbereich laeuft ueber den 32-Bit-Gastadressraum hinaus: " +
                                block.provenance);
    }
    block.physical_origin = canonical_physical_address(block.physical_origin);
    const auto physical_end = static_cast<std::uint64_t>(block.physical_origin) + block.size;
    if (physical_end > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Physischer Blockbereich laeuft ueber 32 Bit hinaus: " +
                                block.provenance);
    }
    block.runtime_registered = runtime_registered;
    const auto identity = stable_runtime_block_identity(block);

    if (const auto known = identities_.find(identity); known != identities_.end()) {
        auto& record = records_.at(known->second);
        if (record.active) {
            throw std::invalid_argument("Blockidentitaet ist bereits aktiv: " + identity);
        }
        if (!runtime_registered || record.static_block) {
            throw std::logic_error("Nur dynamische Runtimebloecke koennen reaktiviert werden: " +
                                   identity);
        }
        if (overlaps_active_virtual(block, known->second)) {
            throw std::invalid_argument("Reaktivierter Block ueberlappt einen aktiven Block: " +
                                        block.provenance);
        }
        record.block = std::move(block);
        record.active = true;
        index_active(known->second, record);
        ++active_count_;
        return {known->second, record.generation};
    }

    if (overlaps_active_virtual(block)) {
        throw std::invalid_argument("Doppelter oder ueberlappender virtueller Block: " +
                                    block.provenance);
    }
    const auto id = next_id_++;
    auto [record_it, inserted] = records_.emplace(
        id, Record{std::move(block), identity, 1u, true, !runtime_registered});
    if (!inserted) throw std::logic_error("Runtime-Block-ID konnte nicht angelegt werden.");
    identities_.emplace(identity, id);
    index_active(id, record_it->second);
    ++active_count_;
    return {id, record_it->second.generation};
}

bool RuntimeBlockTable::overlaps_active_virtual(const RuntimeBlock& block,
                                                const std::uint64_t ignored_id) const noexcept {
    const VariantAddressKey key{block.variant, block.virtual_start};
    const auto next = active_virtual_ranges_.lower_bound(key);
    if (next != active_virtual_ranges_.end() && next->first.variant == block.variant &&
        next->second != ignored_id) {
        const auto& candidate = records_.at(next->second).block;
        if (ranges_overlap(block.virtual_start,
                           block.size,
                           candidate.virtual_start,
                           candidate.size))
            return true;
    }
    if (next != active_virtual_ranges_.begin()) {
        const auto previous = std::prev(next);
        if (previous->first.variant == block.variant && previous->second != ignored_id) {
            const auto& candidate = records_.at(previous->second).block;
            if (ranges_overlap(block.virtual_start,
                               block.size,
                               candidate.virtual_start,
                               candidate.size))
                return true;
        }
    }
    return false;
}

void RuntimeBlockTable::index_active(const std::uint64_t id, const Record& record) {
    const auto virtual_key = VariantAddressKey{record.block.variant, record.block.virtual_start};
    active_virtual_ranges_.emplace(virtual_key, id);
    const auto physical_key = PhysicalLookupKey{
        record.block.variant, record.block.physical_origin, record.block.virtual_start};
    auto& virtual_index = record.static_block ? static_virtual_index_ : dynamic_virtual_index_;
    auto& physical_index = record.static_block ? static_physical_index_ : dynamic_physical_index_;
    auto& alias_index = record.static_block ? static_alias_index_ : dynamic_alias_index_;
    virtual_index.emplace(virtual_key, id);
    physical_index.emplace(physical_key, id);
    alias_index[record.block.physical_origin].insert(id);

    const auto first_page = record.block.physical_origin / physical_page_size;
    const auto last_byte = static_cast<std::uint64_t>(record.block.physical_origin) +
                           record.block.size - 1u;
    const auto last_page = static_cast<std::uint32_t>(last_byte / physical_page_size);
    for (auto page = first_page;; ++page) {
        active_physical_pages_[page].insert(id);
        if (page == last_page) break;
    }
}

bool RuntimeBlockTable::dispatchable(const Record& record) const noexcept {
    return record.active &&
           (code_tracker_ == nullptr || code_tracker_->dispatchable(record.identity));
}

std::optional<RuntimeBlockHandle>
RuntimeBlockTable::lookup_index(const VirtualIndex& index,
                                const std::uint32_t virtual_address,
                                const BlockVariantKey& variant) const noexcept {
    const auto found = index.find({variant, virtual_address});
    if (found == index.end()) return std::nullopt;
    const auto record = records_.find(found->second);
    if (record == records_.end() || !dispatchable(record->second)) return std::nullopt;
    return RuntimeBlockHandle{record->first, record->second.generation};
}

std::optional<RuntimeBlockHandle>
RuntimeBlockTable::lookup(const std::uint32_t virtual_address,
                          const BlockVariantKey& variant) const noexcept {
    if (const auto dynamic = lookup_index(dynamic_virtual_index_, virtual_address, variant))
        return dynamic;
    return lookup_index(static_virtual_index_, virtual_address, variant);
}

std::optional<RuntimeBlockHandle>
RuntimeBlockTable::lookup_physical_index(const PhysicalIndex& index,
                                         const std::uint32_t physical_address,
                                         const BlockVariantKey& variant) const noexcept {
    const auto canonical = canonical_physical_address(physical_address);
    auto found = index.lower_bound({variant, canonical, 0u});
    while (found != index.end() && found->first.variant == variant &&
           found->first.physical == canonical) {
        const auto record = records_.find(found->second);
        if (record != records_.end() && dispatchable(record->second))
            return RuntimeBlockHandle{record->first, record->second.generation};
        ++found;
    }
    return std::nullopt;
}

std::optional<RuntimeBlockHandle>
RuntimeBlockTable::lookup_physical(const std::uint32_t physical_address,
                                   const BlockVariantKey& variant) const noexcept {
    const auto static_block =
        lookup_physical_index(static_physical_index_, physical_address, variant);
    const auto dynamic_block =
        lookup_physical_index(dynamic_physical_index_, physical_address, variant);
    if (!static_block) return dynamic_block;
    if (!dynamic_block) return static_block;
    const auto static_record = resolve(*static_block);
    const auto dynamic_record = resolve(*dynamic_block);
    if (!static_record) return dynamic_block;
    if (!dynamic_record) return static_block;
    return order_key(static_record->get()) < order_key(dynamic_record->get()) ? static_block
                                                                             : dynamic_block;
}

std::vector<RuntimeBlockHandle>
RuntimeBlockTable::aliases(const std::uint32_t physical_origin) const {
    std::vector<RuntimeBlockHandle> result;
    const auto canonical = canonical_physical_address(physical_origin);
    const auto append = [&](const AliasIndex& index) {
        const auto found = index.find(canonical);
        if (found == index.end()) return;
        for (const auto id : found->second) {
            const auto record = records_.find(id);
            if (record != records_.end() && dispatchable(record->second))
                result.push_back({id, record->second.generation});
        }
    };
    append(static_alias_index_);
    append(dynamic_alias_index_);
    std::sort(result.begin(), result.end(), [&](const auto left, const auto right) {
        const auto left_block = resolve(left);
        const auto right_block = resolve(right);
        if (!left_block || !right_block) return left < right;
        return order_key(left_block->get()) < order_key(right_block->get());
    });
    return result;
}

std::optional<std::reference_wrapper<const RuntimeBlock>>
RuntimeBlockTable::resolve(const RuntimeBlockHandle handle) const noexcept {
    const auto found = records_.find(handle.id);
    if (found == records_.end() || found->second.generation != handle.generation ||
        !dispatchable(found->second))
        return std::nullopt;
    return std::cref(found->second.block);
}

bool RuntimeBlockTable::active(const RuntimeBlockHandle handle) const noexcept {
    return resolve(handle).has_value();
}

std::size_t RuntimeBlockTable::size() const noexcept {
    return active_count_;
}

void RuntimeBlockTable::deactivate(const std::uint64_t id) noexcept {
    const auto found = records_.find(id);
    if (found == records_.end() || !found->second.active) return;
    auto& record = found->second;
    active_virtual_ranges_.erase({record.block.variant, record.block.virtual_start});
    if (!record.static_block) {
        dynamic_virtual_index_.erase({record.block.variant, record.block.virtual_start});
        dynamic_physical_index_.erase({record.block.variant,
                                       record.block.physical_origin,
                                       record.block.virtual_start});
    }
    if (!record.static_block) {
        if (auto aliases = dynamic_alias_index_.find(record.block.physical_origin);
            aliases != dynamic_alias_index_.end()) {
            aliases->second.erase(id);
            if (aliases->second.empty()) dynamic_alias_index_.erase(aliases);
        }
    }
    const auto first_page = record.block.physical_origin / physical_page_size;
    const auto last_byte = static_cast<std::uint64_t>(record.block.physical_origin) +
                           record.block.size - 1u;
    const auto last_page = static_cast<std::uint32_t>(last_byte / physical_page_size);
    for (auto page = first_page;; ++page) {
        if (auto entries = active_physical_pages_.find(page);
            entries != active_physical_pages_.end()) {
            entries->second.erase(id);
            if (entries->second.empty()) active_physical_pages_.erase(entries);
        }
        if (page == last_page) break;
    }
    record.active = false;
    ++record.generation;
    --active_count_;
}

bool RuntimeBlockTable::erase_identity(const std::string& block_identity) noexcept {
    const auto found = identities_.find(block_identity);
    if (found == identities_.end()) return false;
    const auto record = records_.find(found->second);
    if (record == records_.end() || !record->second.active) return false;
    deactivate(found->second);
    return true;
}

std::size_t RuntimeBlockTable::erase_overlapping_physical(const std::uint32_t physical_address,
                                                          const std::size_t size) noexcept {
    if (size == 0u) return 0u;
    const auto canonical = canonical_physical_address(physical_address);
    constexpr auto address_space_end =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    const auto write_end = size >= address_space_end - canonical
                               ? address_space_end
                               : static_cast<std::uint64_t>(canonical) + size;
    const auto first_page = canonical / physical_page_size;
    const auto last_page = static_cast<std::uint32_t>((write_end - 1u) / physical_page_size);
    std::set<std::uint64_t> candidates;
    for (auto page = first_page;; ++page) {
        if (const auto found = active_physical_pages_.find(page);
            found != active_physical_pages_.end())
            candidates.insert(found->second.begin(), found->second.end());
        if (page == last_page) break;
    }
    std::vector<std::uint64_t> invalidated;
    for (const auto id : candidates) {
        const auto& block = records_.at(id).block;
        const auto block_end = static_cast<std::uint64_t>(block.physical_origin) + block.size;
        if (block.physical_origin < write_end && canonical < block_end)
            invalidated.push_back(id);
    }
    for (const auto id : invalidated) deactivate(id);
    return invalidated.size();
}

void RuntimeBlockTable::bind_code_tracker(const ExecutableCodeTracker* const tracker) noexcept {
    code_tracker_ = tracker;
}

void RuntimeBlockTable::clear() noexcept {
    records_.clear();
    identities_.clear();
    active_virtual_ranges_.clear();
    static_virtual_index_.clear();
    dynamic_virtual_index_.clear();
    static_physical_index_.clear();
    dynamic_physical_index_.clear();
    static_alias_index_.clear();
    dynamic_alias_index_.clear();
    active_physical_pages_.clear();
    active_count_ = 0u;
    static_sealed_ = false;
}

} // namespace katana::runtime
