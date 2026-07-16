#include "katana/runtime/code_invalidation.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace katana::runtime {
namespace {
bool overlaps(
    const std::uint32_t left, const std::uint64_t left_size,
    const std::uint32_t right, const std::uint64_t right_size
) noexcept {
    return left < static_cast<std::uint64_t>(right) + right_size &&
        right < static_cast<std::uint64_t>(left) + left_size;
}
}

BlockRegistrationResult ExecutableCodeTracker::register_block(
    ExecutableBlockRegistration block
) {
    if (block.identity.empty() || block.provenance.empty() || block.size == 0u) {
        throw std::invalid_argument("Ausfuehrbarer Block benoetigt Identitaet, Groesse und Provenienz.");
    }
    block.physical_start = canonical_physical_address(block.physical_start);
    if (static_cast<std::uint64_t>(block.physical_start) + block.size >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Ausfuehrbarer Block laeuft ueber den Adressraum hinaus.");
    }
    const auto duplicate = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& value) {
        return value.block.identity == block.identity;
    });
    if (duplicate != blocks_.end()) {
        if (duplicate->block.physical_start != block.physical_start ||
            duplicate->block.size != block.size ||
            duplicate->block.provenance != block.provenance) {
            throw std::invalid_argument(
                "Blockidentitaet darf Adresse, Groesse oder Provenienz nicht wechseln."
            );
        }
        duplicate->block.incoming_links.insert(
            block.incoming_links.begin(),
            block.incoming_links.end()
        );
        if (duplicate->valid) {
            return BlockRegistrationResult::AlreadyValid;
        }
        duplicate->valid = true;
        return BlockRegistrationResult::Reactivated;
    }
    blocks_.push_back({std::move(block), true});
    return BlockRegistrationResult::Inserted;
}

CodeInvalidationResult ExecutableCodeTracker::observe_write(
    const std::uint32_t address,
    const std::size_t size,
    const CodeWriteSource source,
    const bool bytes_changed
) {
    if (size == 0u) { throw std::invalid_argument("Code-Schreibbeobachtung braucht eine Groesse."); }
    const auto canonical = canonical_physical_address(address);
    if (size > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(canonical) + size >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Code-Schreibbeobachtung laeuft ueber den Adressraum hinaus.");
    }
    CodeInvalidationResult result;
    result.source = source;
    result.byte_identical = !bytes_changed;
    if (!bytes_changed) { return result; }

    const auto first_page = canonical / page_size * page_size;
    const auto final_address = static_cast<std::uint32_t>(canonical + size - 1u);
    const auto last_page = final_address / page_size * page_size;
    for (std::uint32_t page = first_page;; page += page_size) {
        ++generations_[page];
        ++hotspots_[page];
        result.changed_pages.push_back(page);
        if (page == last_page) { break; }
    }
    for (auto& tracked : blocks_) {
        if (tracked.valid && overlaps(tracked.block.physical_start, tracked.block.size, canonical, size)) {
            tracked.valid = false;
            result.invalidated_blocks.push_back(tracked.block.identity);
            result.unlinked_sources.insert(
                result.unlinked_sources.end(),
                tracked.block.incoming_links.begin(), tracked.block.incoming_links.end()
            );
            ++invalidation_count_;
        }
    }
    std::sort(result.invalidated_blocks.begin(), result.invalidated_blocks.end());
    std::sort(result.unlinked_sources.begin(), result.unlinked_sources.end());
    result.unlinked_sources.erase(
        std::unique(result.unlinked_sources.begin(), result.unlinked_sources.end()),
        result.unlinked_sources.end()
    );
    return result;
}

bool ExecutableCodeTracker::valid(const std::string& identity) const {
    const auto found = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& value) {
        return value.block.identity == identity;
    });
    if (found == blocks_.end()) { throw std::out_of_range("Unbekannte Blockidentitaet."); }
    return found->valid;
}

std::uint64_t ExecutableCodeTracker::page_generation(const std::uint32_t address) const noexcept {
    const auto page = canonical_physical_address(address) / page_size * page_size;
    const auto found = generations_.find(page);
    return found == generations_.end() ? 0u : found->second;
}
std::uint64_t ExecutableCodeTracker::invalidation_count() const noexcept { return invalidation_count_; }
std::size_t ExecutableCodeTracker::block_count() const noexcept { return blocks_.size(); }
std::size_t ExecutableCodeTracker::incoming_link_count(const std::string& identity) const {
    const auto found = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& value) {
        return value.block.identity == identity;
    });
    if (found == blocks_.end()) { throw std::out_of_range("Unbekannte Blockidentitaet."); }
    return found->block.incoming_links.size();
}
const std::map<std::uint32_t, std::uint64_t>& ExecutableCodeTracker::hotspots() const noexcept { return hotspots_; }

} // namespace katana::runtime
