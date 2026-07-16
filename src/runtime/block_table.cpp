#include "katana/runtime/block_table.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace katana::runtime {
namespace {

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

bool ranges_overlap(const RuntimeBlock& left, const RuntimeBlock& right) noexcept {
    const auto left_end = static_cast<std::uint64_t>(left.virtual_start) + left.size;
    const auto right_end = static_cast<std::uint64_t>(right.virtual_start) + right.size;
    return left.virtual_start < right_end && right.virtual_start < left_end;
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

void RuntimeBlockTable::register_static(RuntimeBlock block) {
    insert(std::move(block), false);
}

void RuntimeBlockTable::register_runtime(RuntimeBlock block) {
    insert(std::move(block), true);
}

void RuntimeBlockTable::insert(RuntimeBlock block, const bool runtime_registered) {
    if (block.size == 0u || block.function == nullptr || block.provenance.empty()) {
        throw std::invalid_argument(
            "Blockeintrag benoetigt Groesse, Backendfunktion und Provenienz.");
    }
    if (static_cast<std::uint64_t>(block.virtual_start) + block.size >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Blockbereich laeuft ueber den 32-Bit-Gastadressraum hinaus: " +
                                block.provenance);
    }
    block.physical_origin = canonical_physical_address(block.physical_origin);
    block.runtime_registered = runtime_registered;
    for (const auto& existing : blocks_) {
        if (existing.variant == block.variant && ranges_overlap(existing, block)) {
            throw std::invalid_argument("Doppelter oder ueberlappender virtueller Block: " +
                                        existing.provenance + " <-> " + block.provenance);
        }
    }
    blocks_.push_back(std::move(block));
    std::sort(blocks_.begin(), blocks_.end(), [](const auto& left, const auto& right) {
        return order_key(left) < order_key(right);
    });
}

const RuntimeBlock* RuntimeBlockTable::lookup(const std::uint32_t virtual_address,
                                              const BlockVariantKey& variant) const noexcept {
    const auto found = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& block) {
        return block.virtual_start == virtual_address && block.variant == variant;
    });
    return found == blocks_.end() ? nullptr : &*found;
}

const RuntimeBlock*
RuntimeBlockTable::lookup_physical(const std::uint32_t physical_address,
                                   const BlockVariantKey& variant) const noexcept {
    const auto canonical = canonical_physical_address(physical_address);
    const auto found = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& block) {
        return block.physical_origin == canonical && block.variant == variant;
    });
    return found == blocks_.end() ? nullptr : &*found;
}

std::vector<const RuntimeBlock*>
RuntimeBlockTable::aliases(const std::uint32_t physical_origin) const {
    std::vector<const RuntimeBlock*> result;
    const auto canonical = canonical_physical_address(physical_origin);
    for (const auto& block : blocks_) {
        if (block.physical_origin == canonical) {
            result.push_back(&block);
        }
    }
    return result;
}

std::size_t RuntimeBlockTable::size() const noexcept {
    return blocks_.size();
}

bool RuntimeBlockTable::erase_identity(const std::string& block_identity) noexcept {
    const auto previous_size = blocks_.size();
    std::erase_if(blocks_, [&](const auto& block) {
        return stable_runtime_block_identity(block) == block_identity;
    });
    return blocks_.size() != previous_size;
}

void RuntimeBlockTable::clear() noexcept {
    blocks_.clear();
}

} // namespace katana::runtime
