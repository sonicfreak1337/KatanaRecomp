#include "katana/runtime/block_table.hpp"

#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/cache_control.hpp"

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

std::uint32_t validation_physical_start(const RuntimeBlock& block) noexcept {
    if (!block.aot_template) return block.physical_origin;
    return block.physical_origin -
           (block.virtual_start - block.aot_template->mapping.runtime_start);
}

std::uint32_t validation_extent(const RuntimeBlock& block) noexcept {
    return block.aot_template ? block.aot_template->validation_extent : block.size;
}

} // namespace

std::size_t
RuntimeBlockTable::VariantAddressHash::operator()(const VariantAddressKey& key) const noexcept {
    auto seed = static_cast<std::size_t>(key.address);
    const auto mix = [&seed](auto value) {
        seed ^= std::hash<decltype(value)>{}(value) + static_cast<std::size_t>(0x9E3779B9u) +
                (seed << 6u) + (seed >> 2u);
    };
    mix(key.variant.address_space_generation);
    mix(key.variant.mmu_generation);
    mix(key.variant.watchpoint_generation);
    mix(key.variant.fpscr_mode);
    mix(key.variant.runtime_generation);
    return seed;
}

std::uint32_t canonical_physical_address(const std::uint32_t address) noexcept {
    if ((address & 0xFC000000u) == sh4_on_chip_ram_address) return address;
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
    if (block.aot_template) {
        const auto& contract = *block.aot_template;
        out << std::hex << "-ts" << std::setw(8) << contract.mapping.source_start << "-tr"
            << std::setw(8) << contract.mapping.runtime_start << std::dec << "-te"
            << contract.mapping.extent << "-tv" << contract.validation_extent;
    }
    return out.str();
}

BlockExit execute_runtime_block(const RuntimeBlock& block,
                                CpuState& cpu,
                                BlockExecutionContext& context) {
    if (block.function == nullptr) {
        throw std::invalid_argument("Runtimeblock besitzt keine ausfuehrbare Backendfunktion.");
    }
    if (!block.aot_template) return block.function(cpu, context);
    const ScopedCodeAddressMapping mapping(block.aot_template->mapping);
    return block.function(cpu, context);
}

RuntimeBlockHandle RuntimeBlockTable::register_static(RuntimeBlock block) {
    if (static_sealed_) {
        throw std::logic_error("Statische Blockregistry ist bereits versiegelt.");
    }
    return insert(std::move(block), false);
}

std::optional<RuntimeBlockHandle> RuntimeBlockTable::register_static_variant(
    const std::uint32_t virtual_address,
    const std::uint32_t physical_address,
    const BlockVariantKey& source_variant,
    const BlockVariantKey& target_variant) {
    if (const auto existing = lookup(virtual_address, target_variant)) return existing;
    auto source = lookup(virtual_address, source_variant);
    if (!source) source = lookup_physical(physical_address, source_variant);
    if (!source) return std::nullopt;
    const auto resolved = resolve(*source);
    if (!resolved || resolved->get().runtime_registered ||
        resolved->get().physical_origin != canonical_physical_address(physical_address))
        return std::nullopt;
    auto variant = resolved->get();
    variant.virtual_start = virtual_address;
    variant.physical_origin = canonical_physical_address(physical_address);
    variant.variant = target_variant;
    variant.provenance += "-mmu-variant";
    return insert(std::move(variant), false);
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
    for (auto& block : blocks)
        handles.push_back(insert(std::move(block), false));
    static_sealed_ = true;
    return handles;
}

void RuntimeBlockTable::seal_static() noexcept {
    static_sealed_ = true;
}

RuntimeBlockHandle RuntimeBlockTable::register_bootstrap_static(RuntimeBlock block) {
    if (static_sealed_) {
        throw std::logic_error(
            "Statischer Bootstrapblock muss vor der statischen Registry installiert werden.");
    }
    return insert(std::move(block), false);
}

RuntimeBlockHandle RuntimeBlockTable::register_runtime(RuntimeBlock block) {
    const auto handle = insert(std::move(block), true);
    static_sealed_ = true;
    return handle;
}

RuntimeBlockHandle RuntimeBlockTable::insert(RuntimeBlock block, const bool runtime_registered) {
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
    if (block.aot_template) {
        if (!runtime_registered) {
            throw std::invalid_argument(
                "AOT-Templateabbildungen sind ausschliesslich fuer Runtimebloecke zulaessig: " +
                block.provenance);
        }
        const auto& contract = *block.aot_template;
        validate_code_address_mapping(contract.mapping);
        if (contract.validation_extent == 0u ||
            contract.validation_extent < contract.mapping.extent) {
            throw std::invalid_argument(
                "AOT-Templatevalidierung muss den vollstaendigen Mappingbereich abdecken: " +
                block.provenance);
        }
        const auto mapping_runtime_end =
            static_cast<std::uint64_t>(contract.mapping.runtime_start) + contract.mapping.extent;
        if (block.virtual_start < contract.mapping.runtime_start ||
            virtual_end > mapping_runtime_end) {
            throw std::invalid_argument(
                "Runtimeblock liegt ausserhalb seiner AOT-Templateabbildung: " +
                block.provenance);
        }
        const auto block_offset = block.virtual_start - contract.mapping.runtime_start;
        if (block_offset > block.physical_origin) {
            throw std::invalid_argument(
                "AOT-Templateblock kann seinen physischen Validierungsanfang nicht darstellen: " +
                block.provenance);
        }
        const auto validation_start = block.physical_origin - block_offset;
        const auto source_validation_end =
            static_cast<std::uint64_t>(contract.mapping.source_start) +
            contract.validation_extent;
        const auto runtime_validation_end =
            static_cast<std::uint64_t>(contract.mapping.runtime_start) +
            contract.validation_extent;
        const auto validation_end =
            static_cast<std::uint64_t>(validation_start) + contract.validation_extent;
        if (source_validation_end >
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u ||
            runtime_validation_end >
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u ||
            validation_end >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
            throw std::length_error(
                "AOT-Templatevalidierung laeuft ueber den Gastadressraum hinaus: " +
                block.provenance);
        }
        const auto validation_last_offset = contract.validation_extent - 1u;
        const auto directly_aliased =
            canonical_physical_address(block.virtual_start) == block.physical_origin;
        if (directly_aliased &&
            canonical_physical_address(contract.mapping.runtime_start +
                                       validation_last_offset) !=
                validation_start + validation_last_offset) {
            throw std::invalid_argument(
                "AOT-Templatevalidierung kreuzt eine nicht zusammenhaengende Aliasgrenze: " +
                block.provenance);
        }
        if (static_cast<std::uint64_t>(block_offset) + block.size >
            contract.validation_extent) {
            throw std::invalid_argument(
                "AOT-Templatevalidierung deckt die Runtimeblockbytes nicht ab: " +
                block.provenance);
        }
    }
    const auto physical_end = static_cast<std::uint64_t>(block.physical_origin) + block.size;
    if (physical_end > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Physischer Blockbereich laeuft ueber 32 Bit hinaus: " +
                                block.provenance);
    }
    block.runtime_registered = runtime_registered;
    rejected_generations_.erase({block.variant, block.virtual_start});
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
        if (const auto overlap = overlapping_active_virtual(block, known->second)) {
            throw std::invalid_argument("Reaktivierter Block ueberlappt einen aktiven Block: " +
                                        records_.at(*overlap).block.provenance + " <-> " +
                                        block.provenance);
        }
        record.block = std::move(block);
        record.active = true;
        index_active(known->second, record);
        ++active_count_;
        return {known->second, record.generation};
    }

    if (const auto overlap = overlapping_active_virtual(block)) {
        throw std::invalid_argument("Doppelter oder ueberlappender virtueller Block: " +
                                    records_.at(*overlap).block.provenance + " <-> " +
                                    block.provenance);
    }
    const auto id = next_id_++;
    auto [record_it, inserted] =
        records_.emplace(id, Record{std::move(block), identity, 1u, true, !runtime_registered});
    if (!inserted) throw std::logic_error("Runtime-Block-ID konnte nicht angelegt werden.");
    identities_.emplace(identity, id);
    index_active(id, record_it->second);
    ++active_count_;
    return {id, record_it->second.generation};
}

std::optional<std::uint64_t>
RuntimeBlockTable::overlapping_active_virtual(const RuntimeBlock& block,
                                              const std::uint64_t ignored_id) const noexcept {
    const VariantAddressKey key{block.variant, block.virtual_start};
    const auto next = active_virtual_ranges_.lower_bound(key);
    if (next != active_virtual_ranges_.end() && next->first.variant == block.variant &&
        next->second != ignored_id) {
        const auto& candidate = records_.at(next->second).block;
        if (ranges_overlap(
                block.virtual_start, block.size, candidate.virtual_start, candidate.size))
            return next->second;
    }
    if (next != active_virtual_ranges_.begin()) {
        const auto previous = std::prev(next);
        if (previous->first.variant == block.variant && previous->second != ignored_id) {
            const auto& candidate = records_.at(previous->second).block;
            if (ranges_overlap(
                    block.virtual_start, block.size, candidate.virtual_start, candidate.size))
                return previous->second;
        }
    }
    return std::nullopt;
}

void RuntimeBlockTable::index_active(const std::uint64_t id, const Record& record) {
    const auto virtual_key = VariantAddressKey{record.block.variant, record.block.virtual_start};
    active_virtual_ranges_.emplace(virtual_key, id);
    const auto physical_key = PhysicalLookupKey{
        record.block.variant, record.block.physical_origin, record.block.virtual_start};
    auto& virtual_index = record.static_block ? static_virtual_index_ : dynamic_virtual_index_;
    auto& direct_virtual_index =
        record.static_block ? static_direct_virtual_index_ : dynamic_direct_virtual_index_;
    auto& physical_index = record.static_block ? static_physical_index_ : dynamic_physical_index_;
    auto& alias_index = record.static_block ? static_alias_index_ : dynamic_alias_index_;
    virtual_index.emplace(virtual_key, id);
    direct_virtual_index.emplace(virtual_key, id);
    physical_index.emplace(physical_key, id);
    alias_index[record.block.physical_origin].insert(id);

    const auto tracked_start = validation_physical_start(record.block);
    const auto first_page = tracked_start / physical_page_size;
    const auto last_byte =
        static_cast<std::uint64_t>(tracked_start) + validation_extent(record.block) - 1u;
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
RuntimeBlockTable::lookup_direct_index(const DirectVirtualIndex& index,
                                       const std::uint32_t virtual_address,
                                       const BlockVariantKey& variant) const noexcept {
    ++lookup_counters_.direct_probes;
    const auto found = index.find({variant, virtual_address});
    if (found == index.end()) return std::nullopt;
    const auto record = records_.find(found->second);
    if (record == records_.end() || !dispatchable(record->second)) return std::nullopt;
    return RuntimeBlockHandle{record->first, record->second.generation};
}

std::optional<RuntimeBlockHandle>
RuntimeBlockTable::lookup(const std::uint32_t virtual_address,
                          const BlockVariantKey& variant) const noexcept {
    if (lookup_mode_ == RuntimeBlockLookupMode::Direct) {
        if (const auto dynamic =
                lookup_direct_index(dynamic_direct_virtual_index_, virtual_address, variant))
            return dynamic;
        return lookup_direct_index(static_direct_virtual_index_, virtual_address, variant);
    }
    lookup_counters_.reference_probes += 2u;
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

RuntimeBlockDispatchStatus
RuntimeBlockTable::dispatch_status(const std::uint32_t virtual_address,
                                   const BlockVariantKey& variant) const noexcept {
    if (const auto handle = lookup(virtual_address, variant)) {
        const auto record = records_.find(handle->id);
        if (record != records_.end())
            return {record->second.static_block ? RuntimeBlockDispatchState::StaticCompiled
                                                : RuntimeBlockDispatchState::RuntimeMaterialized,
                    record->second.generation,
                    handle};
    }
    const auto rejected = rejected_generations_.find({variant, virtual_address});
    return {RuntimeBlockDispatchState::Rejected,
            rejected == rejected_generations_.end() ? 0u : rejected->second,
            std::nullopt};
}

void RuntimeBlockTable::mark_rejected(const std::uint32_t virtual_address,
                                      const BlockVariantKey& variant) const noexcept {
    auto& generation = rejected_generations_[{variant, virtual_address}];
    if (generation != std::numeric_limits<std::uint64_t>::max()) ++generation;
}

std::size_t RuntimeBlockTable::size() const noexcept {
    return active_count_;
}

RuntimeBlockLookupMode RuntimeBlockTable::lookup_mode() const noexcept {
    return lookup_mode_;
}

void RuntimeBlockTable::set_lookup_mode(const RuntimeBlockLookupMode mode) noexcept {
    lookup_mode_ = mode;
}

const RuntimeBlockLookupCounters& RuntimeBlockTable::lookup_counters() const noexcept {
    return lookup_counters_;
}

RuntimeBlockTableSnapshot RuntimeBlockTable::snapshot() const {
    RuntimeBlockTableSnapshot result;
    result.records.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        result.records.push_back({
            {id, record.generation},
            record.block.virtual_start,
            record.block.physical_origin,
            record.block.size,
            record.block.end_kind,
            record.block.variant,
            record.identity,
            record.block.provenance,
            record.block.runtime_registered,
            record.active,
            record.static_block,
            record.block.aot_template,
        });
    }
    result.rejected.reserve(rejected_generations_.size());
    for (const auto& [key, generation] : rejected_generations_) {
        result.rejected.push_back({key.address, key.variant, generation});
    }
    result.next_id = next_id_;
    result.active_count = active_count_;
    result.static_sealed = static_sealed_;
    result.code_tracker_bound = code_tracker_ != nullptr;
    result.lookup_mode = lookup_mode_;
    result.lookup_counters = lookup_counters_;
    return result;
}

void RuntimeBlockTable::reset_lookup_counters() const noexcept {
    lookup_counters_ = {};
}

void RuntimeBlockTable::deactivate(const std::uint64_t id) noexcept {
    const auto found = records_.find(id);
    if (found == records_.end() || !found->second.active) return;
    auto& record = found->second;
    active_virtual_ranges_.erase({record.block.variant, record.block.virtual_start});
    if (!record.static_block) {
        dynamic_virtual_index_.erase({record.block.variant, record.block.virtual_start});
        dynamic_direct_virtual_index_.erase({record.block.variant, record.block.virtual_start});
        dynamic_physical_index_.erase(
            {record.block.variant, record.block.physical_origin, record.block.virtual_start});
    }
    if (!record.static_block) {
        if (auto aliases = dynamic_alias_index_.find(record.block.physical_origin);
            aliases != dynamic_alias_index_.end()) {
            aliases->second.erase(id);
            if (aliases->second.empty()) dynamic_alias_index_.erase(aliases);
        }
    }
    const auto tracked_start = validation_physical_start(record.block);
    const auto first_page = tracked_start / physical_page_size;
    const auto last_byte =
        static_cast<std::uint64_t>(tracked_start) + validation_extent(record.block) - 1u;
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
        const auto tracked_start = validation_physical_start(block);
        const auto tracked_end =
            static_cast<std::uint64_t>(tracked_start) + validation_extent(block);
        if (tracked_start < write_end && canonical < tracked_end) invalidated.push_back(id);
    }
    for (const auto id : invalidated)
        deactivate(id);
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
    static_direct_virtual_index_.clear();
    dynamic_direct_virtual_index_.clear();
    static_physical_index_.clear();
    dynamic_physical_index_.clear();
    static_alias_index_.clear();
    dynamic_alias_index_.clear();
    active_physical_pages_.clear();
    active_count_ = 0u;
    static_sealed_ = false;
    lookup_counters_ = {};
    rejected_generations_.clear();
}

} // namespace katana::runtime
