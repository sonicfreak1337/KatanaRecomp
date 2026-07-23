#include "katana/runtime/code_invalidation.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace katana::runtime {
namespace {
bool overlaps(const std::uint32_t left,
              const std::uint64_t left_size,
              const std::uint32_t right,
              const std::uint64_t right_size) noexcept {
    return left < static_cast<std::uint64_t>(right) + right_size &&
           right < static_cast<std::uint64_t>(left) + left_size;
}
} // namespace

ExecutableCodeTracker::ExecutableCodeTracker(const std::size_t provenance_capacity)
    : provenance_capacity_(provenance_capacity) {
    if (provenance_capacity_ == 0u) {
        throw std::invalid_argument(
            "Codeinvalidierungsprovenienz braucht eine positive Kapazitaet.");
    }
    invalidation_events_.reserve(provenance_capacity_);
}

BlockRegistrationResult ExecutableCodeTracker::register_block(ExecutableBlockRegistration block) {
    if (block.identity.empty() || block.provenance.empty() || block.size == 0u) {
        throw std::invalid_argument(
            "Ausfuehrbarer Block benoetigt Identitaet, Groesse und Provenienz.");
    }
    block.physical_start = canonical_physical_address(block.physical_start);
    if (static_cast<std::uint64_t>(block.physical_start) + block.size >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Ausfuehrbarer Block laeuft ueber den Adressraum hinaus.");
    }
    const auto known = identity_index_.find(block.identity);
    if (known != identity_index_.end()) {
        auto& duplicate = blocks_[known->second];
        if (duplicate.block.physical_start != block.physical_start ||
            duplicate.block.size != block.size || duplicate.block.provenance != block.provenance ||
            duplicate.block.origin != block.origin) {
            throw std::invalid_argument(
                "Blockidentitaet darf Adresse, Groesse oder Provenienz nicht wechseln.");
        }
        duplicate.block.incoming_links.insert(block.incoming_links.begin(),
                                              block.incoming_links.end());
        if (duplicate.valid) {
            return BlockRegistrationResult::AlreadyValid;
        }
        duplicate.valid = true;
        return BlockRegistrationResult::Reactivated;
    }
    blocks_.push_back({std::move(block), true});
    const auto index = blocks_.size() - 1u;
    identity_index_.emplace(blocks_[index].block.identity, index);
    const auto first_page = blocks_[index].block.physical_start / page_size * page_size;
    const auto final_address =
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(blocks_[index].block.physical_start) +
                                   blocks_[index].block.size - 1u);
    const auto last_page = final_address / page_size * page_size;
    for (auto page = first_page;; page += page_size) {
        page_blocks_[page].push_back(index);
        if (page == last_page) break;
    }
    return BlockRegistrationResult::Inserted;
}

bool ExecutableCodeTracker::retire_block(const std::string& identity) noexcept {
    const auto known = identity_index_.find(identity);
    if (known == identity_index_.end()) return false;
    auto& tracked = blocks_[known->second];
    if (!tracked.valid) return false;
    tracked.valid = false;
    return true;
}

CodeInvalidationResult ExecutableCodeTracker::observe_write(const std::uint32_t address,
                                                            const std::size_t size,
                                                            const CodeWriteSource source,
                                                            const bool bytes_changed) {
    if (size == 0u) {
        throw std::invalid_argument("Code-Schreibbeobachtung braucht eine Groesse.");
    }
    const auto canonical = canonical_physical_address(address);
    if (size > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(canonical) + size >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Code-Schreibbeobachtung laeuft ueber den Adressraum hinaus.");
    }
    CodeInvalidationResult result;
    result.source = source;
    result.byte_identical = !bytes_changed;
    if (!bytes_changed) {
        record_invalidation_event(address, canonical, size, result);
        return result;
    }

    const auto first_page = canonical / page_size * page_size;
    const auto final_address = static_cast<std::uint32_t>(canonical + size - 1u);
    const auto last_page = final_address / page_size * page_size;
    for (std::uint32_t page = first_page;; page += page_size) {
        ++generations_[page];
        ++hotspots_[page];
        result.changed_pages.push_back(page);
        if (page == last_page) {
            break;
        }
    }
    std::vector<std::size_t> candidates;
    if (lookup_mode_ == CodeInvalidationLookupMode::PageIndex) {
        for (auto page = first_page;; page += page_size) {
            if (const auto found = page_blocks_.find(page); found != page_blocks_.end()) {
                candidates.insert(candidates.end(), found->second.begin(), found->second.end());
            }
            if (page == last_page) break;
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        performance_counters_.indexed_candidates += candidates.size();
    } else {
        candidates.resize(blocks_.size());
        for (std::size_t index = 0u; index < candidates.size(); ++index)
            candidates[index] = index;
        performance_counters_.reference_candidates += candidates.size();
    }
    for (const auto index : candidates) {
        auto& tracked = blocks_[index];
        if (tracked.valid &&
            overlaps(tracked.block.physical_start, tracked.block.size, canonical, size)) {
            tracked.valid = false;
            result.invalidated_blocks.push_back(tracked.block.identity);
            result.unlinked_sources.insert(result.unlinked_sources.end(),
                                           tracked.block.incoming_links.begin(),
                                           tracked.block.incoming_links.end());
            ++invalidation_count_;
        }
    }
    std::sort(result.invalidated_blocks.begin(), result.invalidated_blocks.end());
    std::sort(result.unlinked_sources.begin(), result.unlinked_sources.end());
    result.unlinked_sources.erase(
        std::unique(result.unlinked_sources.begin(), result.unlinked_sources.end()),
        result.unlinked_sources.end());
    record_invalidation_event(address, canonical, size, result);
    return result;
}

ExecutableCodeTracker::PreparedDiscLoadWrite
ExecutableCodeTracker::prepare_disc_load_write(const std::uint32_t address,
                                               const std::size_t size,
                                               const CodeWriteSource source) {
    if (size == 0u)
        throw std::invalid_argument("Disc-Codewrite-Admission braucht eine Groesse.");
    const auto canonical = canonical_physical_address(address);
    if (size > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(canonical) + size >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u)
        throw std::length_error("Disc-Codewrite-Admission laeuft ueber den Adressraum.");

    PreparedDiscLoadWrite plan;
    plan.address = address;
    plan.physical_address = canonical;
    plan.size = size;
    plan.source = source;
    plan.result.source = source;
    const auto first_page = canonical / page_size * page_size;
    const auto final_address = static_cast<std::uint32_t>(canonical + size - 1u);
    const auto last_page = final_address / page_size * page_size;
    for (auto page = first_page;; page += page_size) {
        plan.pages.push_back(page);
        plan.result.changed_pages.push_back(page);
        if (page == last_page) break;
    }

    std::vector<std::size_t> candidates;
    plan.indexed_lookup = lookup_mode_ == CodeInvalidationLookupMode::PageIndex;
    if (plan.indexed_lookup) {
        for (const auto page : plan.pages) {
            if (const auto found = page_blocks_.find(page); found != page_blocks_.end())
                candidates.insert(candidates.end(), found->second.begin(), found->second.end());
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    } else {
        candidates.resize(blocks_.size());
        for (std::size_t index = 0u; index < candidates.size(); ++index)
            candidates[index] = index;
    }
    plan.candidate_count = candidates.size();
    for (const auto index : candidates) {
        const auto& tracked = blocks_.at(index);
        if (!tracked.valid ||
            !overlaps(tracked.block.physical_start, tracked.block.size, canonical, size))
            continue;
        plan.block_indices.push_back(index);
        plan.result.invalidated_blocks.push_back(tracked.block.identity);
        plan.result.unlinked_sources.insert(plan.result.unlinked_sources.end(),
                                            tracked.block.incoming_links.begin(),
                                            tracked.block.incoming_links.end());
    }
    std::sort(plan.result.invalidated_blocks.begin(), plan.result.invalidated_blocks.end());
    std::sort(plan.result.unlinked_sources.begin(), plan.result.unlinked_sources.end());
    plan.result.unlinked_sources.erase(
        std::unique(plan.result.unlinked_sources.begin(), plan.result.unlinked_sources.end()),
        plan.result.unlinked_sources.end());

    try {
        for (const auto page : plan.pages) {
            if (!generations_.contains(page)) {
                generations_.emplace(page, 0u);
                plan.inserted_generation_pages.push_back(page);
            }
            if (!hotspots_.contains(page)) {
                hotspots_.emplace(page, 0u);
                plan.inserted_hotspot_pages.push_back(page);
            }
        }
    } catch (...) {
        cancel_disc_load_write(plan);
        throw;
    }
    return plan;
}

void ExecutableCodeTracker::cancel_disc_load_write(PreparedDiscLoadWrite& plan) noexcept {
    for (const auto page : plan.inserted_generation_pages) {
        const auto found = generations_.find(page);
        if (found != generations_.end() && found->second == 0u) generations_.erase(found);
    }
    for (const auto page : plan.inserted_hotspot_pages) {
        const auto found = hotspots_.find(page);
        if (found != hotspots_.end() && found->second == 0u) hotspots_.erase(found);
    }
    plan.inserted_generation_pages.clear();
    plan.inserted_hotspot_pages.clear();
}

void ExecutableCodeTracker::commit_disc_load_write(PreparedDiscLoadWrite plan) noexcept {
    for (const auto page : plan.pages) {
        auto generation = generations_.find(page);
        auto hotspot = hotspots_.find(page);
        if (generation == generations_.end() || hotspot == hotspots_.end()) continue;
        ++generation->second;
        ++hotspot->second;
    }
    for (const auto index : plan.block_indices) {
        if (index >= blocks_.size() || !blocks_[index].valid) continue;
        blocks_[index].valid = false;
        ++invalidation_count_;
    }
    if (plan.indexed_lookup)
        performance_counters_.indexed_candidates += plan.candidate_count;
    else
        performance_counters_.reference_candidates += plan.candidate_count;
    record_invalidation_event(
        plan.address, plan.physical_address, plan.size, plan.result);
}

void ExecutableCodeTracker::record_invalidation_event(
    const std::uint32_t virtual_address,
    const std::uint32_t physical_address,
    const std::size_t size,
    const CodeInvalidationResult& result) noexcept {
    if (next_provenance_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        if (dropped_provenance_events_ != std::numeric_limits<std::uint64_t>::max()) {
            ++dropped_provenance_events_;
        }
        return;
    }
    try {
        CodeInvalidationEvent event;
        event.sequence = next_provenance_sequence_;
        event.virtual_address = virtual_address;
        event.physical_address = physical_address;
        event.size = size;
        event.source = result.source;
        event.byte_identical = result.byte_identical;
        event.invalidated_blocks = result.invalidated_blocks;
        event.unlinked_sources = result.unlinked_sources;
        event.pages.reserve(result.changed_pages.size());
        for (const auto page : result.changed_pages) {
            event.pages.push_back({page, page_generation(page)});
        }
        if (invalidation_events_.size() == provenance_capacity_) {
            invalidation_events_.erase(invalidation_events_.begin());
            if (dropped_provenance_events_ != std::numeric_limits<std::uint64_t>::max()) {
                ++dropped_provenance_events_;
            }
        }
        invalidation_events_.push_back(std::move(event));
    } catch (...) {
        if (dropped_provenance_events_ != std::numeric_limits<std::uint64_t>::max()) {
            ++dropped_provenance_events_;
        }
    }
    ++next_provenance_sequence_;
}

bool ExecutableCodeTracker::valid(const std::string& identity) const {
    const auto found = identity_index_.find(identity);
    if (found == identity_index_.end()) {
        throw std::out_of_range("Unbekannte Blockidentitaet.");
    }
    return blocks_[found->second].valid;
}

bool ExecutableCodeTracker::dispatchable(const std::string& identity) const noexcept {
    const auto found = identity_index_.find(identity);
    return found == identity_index_.end() || blocks_[found->second].valid;
}

bool ExecutableCodeTracker::tracks_address(const std::uint32_t address,
                                           const std::size_t size) const noexcept {
    if (size == 0u) return false;
    const auto canonical = canonical_physical_address(address);
    if (size > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(canonical) + size >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u)
        return false;
    const auto first_page = canonical / page_size * page_size;
    const auto last_address = static_cast<std::uint32_t>(canonical + size - 1u);
    const auto last_page = last_address / page_size * page_size;
    for (auto page = first_page;; page += page_size) {
        if (page_blocks_.contains(page)) return true;
        if (page == last_page) break;
    }
    return false;
}

std::uint64_t ExecutableCodeTracker::page_generation(const std::uint32_t address) const noexcept {
    const auto page = canonical_physical_address(address) / page_size * page_size;
    const auto found = generations_.find(page);
    return found == generations_.end() ? 0u : found->second;
}
std::uint64_t ExecutableCodeTracker::invalidation_count() const noexcept {
    return invalidation_count_;
}
std::size_t ExecutableCodeTracker::block_count() const noexcept {
    return blocks_.size();
}
std::size_t ExecutableCodeTracker::incoming_link_count(const std::string& identity) const {
    const auto found = identity_index_.find(identity);
    if (found == identity_index_.end()) {
        throw std::out_of_range("Unbekannte Blockidentitaet.");
    }
    return blocks_[found->second].block.incoming_links.size();
}
const std::map<std::uint32_t, std::uint64_t>& ExecutableCodeTracker::hotspots() const noexcept {
    return hotspots_;
}

const std::vector<TrackedExecutableBlock>& ExecutableCodeTracker::blocks() const noexcept {
    return blocks_;
}

const std::vector<CodeInvalidationEvent>&
ExecutableCodeTracker::invalidation_events() const noexcept {
    return invalidation_events_;
}

std::uint64_t ExecutableCodeTracker::dropped_provenance_events() const noexcept {
    return dropped_provenance_events_;
}

std::size_t ExecutableCodeTracker::provenance_capacity() const noexcept {
    return provenance_capacity_;
}

CodeInvalidationLookupMode ExecutableCodeTracker::lookup_mode() const noexcept {
    return lookup_mode_;
}

void ExecutableCodeTracker::set_lookup_mode(const CodeInvalidationLookupMode mode) noexcept {
    lookup_mode_ = mode;
}

const CodeInvalidationPerformanceCounters&
ExecutableCodeTracker::performance_counters() const noexcept {
    return performance_counters_;
}

ExecutableCodeTrackerSnapshot ExecutableCodeTracker::snapshot() const {
    ExecutableCodeTrackerSnapshot result;
    result.blocks = blocks_;
    result.page_generations.reserve(generations_.size());
    for (const auto& [page, generation] : generations_)
        result.page_generations.push_back({page, generation});
    result.hotspots.reserve(hotspots_.size());
    for (const auto& [page, count] : hotspots_)
        result.hotspots.push_back({page, count});
    result.invalidation_count = invalidation_count_;
    result.invalidation_events = invalidation_events_;
    result.provenance_capacity = provenance_capacity_;
    result.next_provenance_sequence = next_provenance_sequence_;
    result.dropped_provenance_events = dropped_provenance_events_;
    result.lookup_mode = lookup_mode_;
    result.performance_counters = performance_counters_;
    return result;
}

void ExecutableCodeTracker::reset_performance_counters() noexcept {
    performance_counters_ = {};
}

const char* code_write_source_name(const CodeWriteSource value) noexcept {
    switch (value) {
    case CodeWriteSource::Cpu:
        return "cpu";
    case CodeWriteSource::Fpu:
        return "fpu";
    case CodeWriteSource::Dma:
        return "dma";
    case CodeWriteSource::StoreQueue:
        return "store-queue";
    case CodeWriteSource::Copy:
        return "copy";
    case CodeWriteSource::Fallback:
        return "fallback";
    }
    return "unknown";
}

const char* executable_block_origin_name(const ExecutableBlockOrigin value) noexcept {
    switch (value) {
    case ExecutableBlockOrigin::ImageSegment:
        return "image-segment";
    case ExecutableBlockOrigin::RomRamCopy:
        return "rom-ram-copy";
    case ExecutableBlockOrigin::FallbackDecode:
        return "fallback-decode";
    case ExecutableBlockOrigin::RuntimeWrite:
        return "runtime-write";
    }
    return "unknown";
}

} // namespace katana::runtime
