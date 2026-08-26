#include "katana/runtime/block_table.hpp"

#include "katana/runtime/cache_control.hpp"
#include "katana/runtime/code_invalidation.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace katana::runtime {
namespace {

constexpr std::uint32_t physical_page_size = 4096u;
std::atomic<std::uint64_t> next_block_table_lifetime{1u};

std::uint64_t allocate_block_table_lifetime() noexcept {
    auto lifetime =
        next_block_table_lifetime.fetch_add(1u, std::memory_order_relaxed);
    if (lifetime == 0u)
        lifetime =
            next_block_table_lifetime.fetch_add(1u, std::memory_order_relaxed);
    return lifetime;
}

void advance_generation(std::uint64_t& generation) noexcept {
    ++generation;
    if (generation == 0u) ++generation;
}

[[nodiscard]] bool direct_write_batch_range(
    const GuestWriteEvent& event,
    std::uint32_t& physical_begin,
    std::uint32_t& physical_page) noexcept {
    constexpr auto address_space_end =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    if ((event.size != 1u && event.size != 2u && event.size != 4u) ||
        event.size > address_space_end - event.address)
        return false;
    physical_begin = canonical_physical_address(event.address);
    const auto physical_end =
        static_cast<std::uint64_t>(physical_begin) + event.size;
    const auto final_virtual =
        event.address + static_cast<std::uint32_t>(event.size - 1u);
    if (physical_end > address_space_end ||
        canonical_physical_address(final_virtual) != physical_end - 1u)
        return false;
    physical_page = physical_begin / physical_page_size;
    return static_cast<std::uint32_t>((physical_end - 1u) /
                                     physical_page_size) ==
           physical_page;
}

class ScopedBlockExceptionGeneration final {
  public:
    ScopedBlockExceptionGeneration(BlockExecutionContext& context,
                                   const std::uint64_t generation) noexcept
        : context_(context), previous_(context.exception_generation_on_entry) {
        context_.exception_generation_on_entry = generation;
    }

    ~ScopedBlockExceptionGeneration() noexcept {
        context_.exception_generation_on_entry = previous_;
    }

    ScopedBlockExceptionGeneration(const ScopedBlockExceptionGeneration&) = delete;
    ScopedBlockExceptionGeneration& operator=(const ScopedBlockExceptionGeneration&) = delete;

  private:
    BlockExecutionContext& context_;
    std::optional<std::uint64_t> previous_;
};

class ScopedActiveBlockProvenance final {
  public:
    ScopedActiveBlockProvenance(CpuState& cpu,
                                const std::uint32_t virtual_start,
                                const std::uint32_t physical_start,
                                const std::uint32_t size) noexcept
        : cpu_(cpu), previous_virtual_start_(cpu.active_block_virtual_start),
          previous_physical_start_(cpu.active_block_physical_start),
          previous_size_(cpu.active_block_size) {
        cpu_.active_block_virtual_start = virtual_start;
        cpu_.active_block_physical_start = physical_start;
        cpu_.active_block_size = size;
    }

    ~ScopedActiveBlockProvenance() noexcept {
        cpu_.active_block_virtual_start = previous_virtual_start_;
        cpu_.active_block_physical_start = previous_physical_start_;
        cpu_.active_block_size = previous_size_;
    }

    ScopedActiveBlockProvenance(const ScopedActiveBlockProvenance&) = delete;
    ScopedActiveBlockProvenance& operator=(const ScopedActiveBlockProvenance&) = delete;

  private:
    CpuState& cpu_;
    std::uint32_t previous_virtual_start_;
    std::uint32_t previous_physical_start_;
    std::uint32_t previous_size_;
};

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

bool compatible_physical_overlap(const RuntimeBlock& left, const RuntimeBlock& right) noexcept {
    const auto overlap_start = std::max(left.virtual_start, right.virtual_start);
    const auto left_physical =
        static_cast<std::uint64_t>(canonical_physical_address(left.physical_origin)) +
        (overlap_start - left.virtual_start);
    const auto right_physical =
        static_cast<std::uint64_t>(canonical_physical_address(right.physical_origin)) +
        (overlap_start - right.virtual_start);
    return left_physical == right_physical;
}

std::uint32_t validation_physical_start(const RuntimeBlock& block) noexcept {
    if (!block.aot_template) return block.physical_origin;
    if (block.aot_template->validation_mode ==
        NativeAotTemplateValidationMode::RuntimeBlock)
        return block.physical_origin;
    return block.physical_origin -
           (block.virtual_start - block.aot_template->mapping.runtime_start);
}

std::uint32_t validation_extent(const RuntimeBlock& block) noexcept {
    if (!block.aot_template) return block.size;
    return block.aot_template->validation_mode ==
                   NativeAotTemplateValidationMode::RuntimeBlock
               ? block.size
               : block.aot_template->validation_extent;
}

std::span<const NativeAotTemplateMutableRange>
validation_mutable_ranges(const RuntimeBlock& block) noexcept {
    return block.aot_template &&
                   block.aot_template->validation_mode ==
                       NativeAotTemplateValidationMode::SourceModule
               ? std::span<const NativeAotTemplateMutableRange>(
                     block.aot_template->mutable_ranges)
               : std::span<const NativeAotTemplateMutableRange>{};
}

} // namespace

RuntimeBlockTable::RuntimeBlockTable() noexcept
    : dispatch_lifetime_(allocate_block_table_lifetime()) {}

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

bool direct_p1_p2_block_binding_contiguous(
    const std::uint32_t virtual_start,
    const std::uint32_t physical_origin,
    const std::uint32_t size) noexcept {
    const auto segment = virtual_start >> 29u;
    if ((segment != 4u && segment != 5u) ||
        (virtual_start & 1u) != 0u || (physical_origin & 1u) != 0u ||
        size < 2u || (size & 1u) != 0u ||
        physical_origin != canonical_physical_address(physical_origin) ||
        canonical_physical_address(virtual_start) != physical_origin)
        return false;

    const auto last_offset = static_cast<std::uint64_t>(size) - 2u;
    const auto virtual_last =
        static_cast<std::uint64_t>(virtual_start) + last_offset;
    const auto physical_last =
        static_cast<std::uint64_t>(physical_origin) + last_offset;
    return virtual_last <= std::numeric_limits<std::uint32_t>::max() &&
           physical_last <= std::numeric_limits<std::uint32_t>::max() &&
           canonical_physical_address(
               static_cast<std::uint32_t>(virtual_last)) == physical_last;
}

bool native_aot_mutable_ranges_valid(
    const std::span<const NativeAotTemplateMutableRange> ranges,
    const std::uint32_t extent) noexcept {
    std::uint64_t previous_end = 0u;
    for (const auto range : ranges) {
        const auto end = static_cast<std::uint64_t>(range.offset) + range.size;
        if (range.size == 0u || range.offset < previous_end || end > extent) return false;
        previous_end = end;
    }
    return true;
}

bool native_aot_offset_is_mutable(
    const std::span<const NativeAotTemplateMutableRange> ranges,
    const std::uint32_t offset) noexcept {
    for (const auto range : ranges) {
        if (offset < range.offset) return false;
        if (offset - range.offset < range.size) return true;
    }
    return false;
}

bool native_aot_write_overlaps_immutable(
    const std::uint32_t tracked_start,
    const std::uint32_t tracked_extent,
    const std::span<const NativeAotTemplateMutableRange> mutable_ranges,
    const std::uint32_t write_start,
    const std::size_t write_size) noexcept {
    if (tracked_extent == 0u || write_size == 0u) return false;
    constexpr auto address_space_end =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    if (tracked_extent > address_space_end - tracked_start ||
        write_size > address_space_end - write_start)
        return true;
    const auto tracked_end = static_cast<std::uint64_t>(tracked_start) + tracked_extent;
    const auto write_end = static_cast<std::uint64_t>(write_start) + write_size;
    const auto overlap_begin =
        std::max<std::uint64_t>(tracked_start, static_cast<std::uint64_t>(write_start));
    const auto overlap_end = std::min(tracked_end, write_end);
    if (overlap_begin >= overlap_end) return false;
    if (!native_aot_mutable_ranges_valid(mutable_ranges, tracked_extent)) return true;

    auto cursor = overlap_begin - tracked_start;
    const auto end_offset = overlap_end - tracked_start;
    for (const auto range : mutable_ranges) {
        if (cursor >= end_offset) return false;
        const auto range_end = static_cast<std::uint64_t>(range.offset) + range.size;
        if (range_end <= cursor) continue;
        if (range.offset > cursor) return true;
        cursor = std::min(end_offset, range_end);
    }
    return cursor < end_offset;
}

std::string stable_runtime_block_identity(const RuntimeBlock& block) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << "v" << std::setw(8) << block.virtual_start << "-p"
        << std::setw(8) << block.physical_origin << std::dec << "-s" << block.size << "-e"
        << static_cast<unsigned>(block.end_kind) << "-a" << block.variant.address_space_generation
        << "-m" << block.variant.mmu_generation << "-w" << block.variant.watchpoint_generation
        << "-f" << block.variant.fpscr_mode << "-r" << block.variant.runtime_generation;
    if (block.static_variant_policy != StaticVariantPolicy::Exact)
        out << "-vp" << static_cast<unsigned>(block.static_variant_policy);
    out << "-" << block.provenance;
    if (block.aot_template) {
        const auto& contract = *block.aot_template;
        out << std::hex << "-ts" << std::setw(8) << contract.mapping.source_start << "-tr"
            << std::setw(8) << contract.mapping.runtime_start << std::dec << "-te"
            << contract.mapping.extent << "-tv" << contract.validation_extent << "-tmv"
            << static_cast<unsigned>(contract.validation_mode);
        for (const auto range : contract.mutable_ranges)
            out << "-tm" << range.offset << "x" << range.size;
    }
    return out.str();
}

BlockExit
execute_runtime_block(const RuntimeBlock& block, CpuState& cpu, BlockExecutionContext& context) {
    if (block.function == nullptr) {
        throw std::invalid_argument("Runtimeblock besitzt keine ausfuehrbare Backendfunktion.");
    }
    const ScopedBlockExceptionGeneration exception_generation(context, cpu.exception_generation);
    const ScopedActiveBlockProvenance active_block(
        cpu, block.virtual_start, block.physical_origin, block.size);
    if (!block.aot_template) return block.function(cpu, context);
    const ScopedCodeAddressMapping mapping(block.aot_template->mapping);
    return block.function(cpu, context);
}

BlockExit execute_runtime_block(const ValidatedBlockExecution& block,
                                CpuState& cpu,
                                BlockExecutionContext& context) {
    if (block.function == nullptr) {
        throw std::invalid_argument("Validierter Runtimeblock besitzt keine Backendfunktion.");
    }
    const ScopedBlockExceptionGeneration exception_generation(context, cpu.exception_generation);
    const ScopedActiveBlockProvenance active_block(
        cpu, block.virtual_start, block.physical_origin, block.size);
    if (block.aot_template == nullptr) return block.function(cpu, context);
    const ScopedCodeAddressMapping mapping(block.aot_template->mapping);
    return block.function(cpu, context);
}

RuntimeBlockHandle RuntimeBlockTable::register_static(RuntimeBlock block) {
    if (static_sealed_) {
        throw std::logic_error("Statische Blockregistry ist bereits versiegelt.");
    }
    return insert(std::move(block), false);
}

std::optional<RuntimeBlockHandle>
RuntimeBlockTable::register_static_variant(const std::uint32_t virtual_address,
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
    // A synthesized MMU/contextual variant is exact to that virtual mapping;
    // it must not inherit a source block's P1/P2-only direct policy.
    variant.static_variant_policy = StaticVariantPolicy::Exact;
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
    seal_static();
    return handles;
}

std::vector<RuntimeBlockHandle>
RuntimeBlockTable::register_static_contextual_bulk(std::vector<RuntimeBlock> blocks) {
    if (static_sealed_) {
        throw std::logic_error("Statische Blockregistry ist bereits versiegelt.");
    }
    // Validate only address-neighbouring entries of the same variant. Product
    // ports contain tens of thousands of static blocks, while contextual
    // overlaps are sparse resume/entry aliases; a full quadratic scan would
    // turn a correctness feature into a startup stall.
    std::vector<const RuntimeBlock*> validation_blocks;
    validation_blocks.reserve(active_count_ + blocks.size());
    for (const auto& [id, record] : records_) {
        static_cast<void>(id);
        if (!record.active) continue;
        if (!record.static_block || record.block.runtime_registered)
            throw std::logic_error(
                "Kontextuelle statische Registry enthaelt bereits einen Runtimeblock.");
        validation_blocks.push_back(&record.block);
    }
    for (const auto& block : blocks) validation_blocks.push_back(&block);
    std::sort(validation_blocks.begin(),
              validation_blocks.end(),
              [](const auto* left, const auto* right) {
                  return std::tie(left->variant.address_space_generation,
                                  left->variant.mmu_generation,
                                  left->variant.watchpoint_generation,
                                  left->variant.fpscr_mode,
                                  left->variant.runtime_generation,
                                  left->virtual_start,
                                  left->physical_origin,
                                  left->provenance) <
                         std::tie(right->variant.address_space_generation,
                                  right->variant.mmu_generation,
                                  right->variant.watchpoint_generation,
                                  right->variant.fpscr_mode,
                                  right->variant.runtime_generation,
                                  right->virtual_start,
                                  right->physical_origin,
                                  right->provenance);
    });
    bool has_contextual_overlap = false;
    for (std::size_t left = 0u; left < validation_blocks.size(); ++left) {
        for (std::size_t right = left + 1u;
             right < validation_blocks.size();
             ++right) {
            const auto& left_block = *validation_blocks[left];
            const auto& right_block = *validation_blocks[right];
            if (left_block.variant != right_block.variant) break;
            const auto left_end =
                static_cast<std::uint64_t>(left_block.virtual_start) +
                left_block.size;
            if (right_block.virtual_start >= left_end) break;
            if (!ranges_overlap(left_block.virtual_start,
                                left_block.size,
                                right_block.virtual_start,
                                right_block.size))
                continue;
            has_contextual_overlap = true;
            if (left_block.virtual_start == right_block.virtual_start ||
                !compatible_physical_overlap(left_block, right_block)) {
                throw std::invalid_argument(
                    "Kontextuelle Blockueberlappung ist nicht eindeutig abbildbar: " +
                    left_block.provenance + " <-> " + right_block.provenance);
            }
        }
    }
    std::sort(blocks.begin(), blocks.end(), [](const auto& left, const auto& right) {
        return order_key(left) < order_key(right);
    });
    std::vector<RuntimeBlockHandle> handles;
    handles.reserve(blocks.size());
    contextual_virtual_overlaps_ = has_contextual_overlap;
    for (auto& block : blocks)
        handles.push_back(insert(std::move(block), false, true));
    seal_static();
    return handles;
}

void RuntimeBlockTable::seal_static() {
    if (static_sealed_) return;
    rebuild_static_aot_index();
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
    const bool seal_after_registration = !static_sealed_;
    const bool contextual_aot_entry = block.aot_template.has_value();
    bool has_contextual_overlap = false;
    if (contextual_aot_entry) {
        auto candidate =
            active_virtual_ranges_.lower_bound({block.variant, 0u});
        for (;
             candidate != active_virtual_ranges_.end() &&
             candidate->first.variant == block.variant;
             ++candidate) {
            const auto& active = records_.at(candidate->second).block;
            if (!ranges_overlap(block.virtual_start,
                                block.size,
                                active.virtual_start,
                                active.size))
                continue;
            has_contextual_overlap = true;
            if (block.virtual_start == active.virtual_start ||
                !compatible_physical_overlap(block, active)) {
                throw std::invalid_argument(
                    "Kontextuelle Runtime-AOT-Ueberlappung ist nicht "
                    "eindeutig abbildbar: " +
                    active.provenance + " <-> " + block.provenance);
            }
        }
    }
    contextual_virtual_overlaps_ =
        contextual_virtual_overlaps_ || has_contextual_overlap;
    const auto handle =
        insert(std::move(block), true, contextual_aot_entry);
    if (seal_after_registration) seal_static();
    return handle;
}

RuntimeBlockHandle RuntimeBlockTable::insert(RuntimeBlock block,
                                             const bool runtime_registered,
                                             const bool allow_contextual_overlap) {
    if (block.size == 0u || block.function == nullptr || block.provenance.empty()) {
        throw std::invalid_argument(
            "Blockeintrag benoetigt Groesse, Backendfunktion und Provenienz.");
    }
    if ((block.fastpath.kind == RuntimeBlockFastpathKind::None) !=
        (block.fastpath.descriptor == nullptr)) {
        throw std::invalid_argument(
            "Fastpathbindung benoetigt konsistent Typ und Deskriptor: " +
            block.provenance);
    }
    const auto virtual_end = static_cast<std::uint64_t>(block.virtual_start) + block.size;
    if (virtual_end > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::length_error("Blockbereich laeuft ueber den 32-Bit-Gastadressraum hinaus: " +
                                block.provenance);
    }
    block.physical_origin = canonical_physical_address(block.physical_origin);
    if (!runtime_registered &&
        block.static_variant_policy ==
            StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic &&
        !direct_p1_p2_block_binding_contiguous(block.virtual_start,
                                               block.physical_origin,
                                               block.size)) {
        throw std::invalid_argument(
            "Direkte Static-AOT-Bindung kreuzt eine Aliasgrenze: " +
            block.provenance);
    }
    if (block.aot_template) {
        if (!runtime_registered) {
            throw std::invalid_argument(
                "AOT-Templateabbildungen sind ausschliesslich fuer Runtimebloecke zulaessig: " +
                block.provenance);
        }
        const auto& contract = *block.aot_template;
        validate_code_address_mapping(contract.mapping);
        const auto source_module_validation =
            contract.validation_mode ==
            NativeAotTemplateValidationMode::SourceModule;
        if (contract.validation_extent == 0u ||
            (source_module_validation &&
             contract.validation_extent < contract.mapping.extent) ||
            (!source_module_validation &&
             (contract.validation_extent != block.size ||
              !contract.mutable_ranges.empty())) ||
            !native_aot_mutable_ranges_valid(contract.mutable_ranges,
                                             contract.validation_extent)) {
            throw std::invalid_argument(
                "AOT-Templatevalidierung besitzt keinen konsistenten Proofbereich: " +
                block.provenance);
        }
        const auto mapping_runtime_end =
            static_cast<std::uint64_t>(contract.mapping.runtime_start) + contract.mapping.extent;
        if (block.virtual_start < contract.mapping.runtime_start ||
            virtual_end > mapping_runtime_end) {
            throw std::invalid_argument(
                "Runtimeblock liegt ausserhalb seiner AOT-Templateabbildung: " + block.provenance);
        }
        const auto block_offset = block.virtual_start - contract.mapping.runtime_start;
        if (block_offset > block.physical_origin) {
            throw std::invalid_argument(
                "AOT-Templateblock kann seinen physischen Validierungsanfang nicht darstellen: " +
                block.provenance);
        }
        const auto validation_start =
            source_module_validation ? block.physical_origin - block_offset
                                     : block.physical_origin;
        const auto source_validation_start =
            static_cast<std::uint64_t>(contract.mapping.source_start) +
            (source_module_validation ? 0u : block_offset);
        const auto runtime_validation_start =
            static_cast<std::uint64_t>(
                source_module_validation ? contract.mapping.runtime_start
                                         : block.virtual_start);
        const auto source_validation_end =
            source_validation_start + contract.validation_extent;
        const auto runtime_validation_end =
            runtime_validation_start + contract.validation_extent;
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
            canonical_physical_address(
                static_cast<std::uint32_t>(runtime_validation_start) +
                validation_last_offset) !=
                validation_start + validation_last_offset) {
            throw std::invalid_argument(
                "AOT-Templatevalidierung kreuzt eine nicht zusammenhaengende Aliasgrenze: " +
                block.provenance);
        }
        if (source_module_validation &&
            static_cast<std::uint64_t>(block_offset) + block.size >
                contract.validation_extent) {
            throw std::invalid_argument(
                "AOT-Templatevalidierung deckt die Runtimeblockbytes nicht ab: " +
                block.provenance);
        }
        const auto block_end_offset = static_cast<std::uint64_t>(block_offset) + block.size;
        if (std::any_of(contract.mutable_ranges.begin(),
                        contract.mutable_ranges.end(),
                        [&](const auto range) {
                            const auto range_end =
                                static_cast<std::uint64_t>(range.offset) + range.size;
                            return block_offset < range_end && range.offset < block_end_offset;
                        })) {
            throw std::invalid_argument(
                "AOT-Mutable-Range ueberlappt ausfuehrbare Runtimeblockbytes: " +
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
        if (!allow_contextual_overlap) {
            if (const auto overlap = overlapping_active_virtual(block, known->second)) {
                throw std::invalid_argument("Reaktivierter Block ueberlappt einen aktiven Block: " +
                                            records_.at(*overlap).block.provenance + " <-> " +
                                            block.provenance);
            }
        } else if (const auto same_start =
                       active_virtual_ranges_.find({block.variant, block.virtual_start});
                   same_start != active_virtual_ranges_.end() &&
                   same_start->second != known->second) {
            throw std::invalid_argument("Reaktivierter Block ueberlappt einen aktiven Block: " +
                                        records_.at(same_start->second).block.provenance + " <-> " +
                                        block.provenance);
        }
        record.block = std::move(block);
        record.active = true;
        index_active(known->second, record);
        ++active_count_;
        return {known->second, record.generation};
    }

    if (!allow_contextual_overlap) {
        if (const auto overlap = overlapping_active_virtual(block)) {
            throw std::invalid_argument("Doppelter oder ueberlappender virtueller Block: " +
                                        records_.at(*overlap).block.provenance + " <-> " +
                                        block.provenance);
        }
    } else if (const auto same_start =
                   active_virtual_ranges_.find({block.variant, block.virtual_start});
               same_start != active_virtual_ranges_.end()) {
        throw std::invalid_argument("Doppelter kontextueller Blockeinstieg: " +
                                    records_.at(same_start->second).block.provenance + " <-> " +
                                    block.provenance);
    }
    const auto id = next_id_++;
    auto [record_it, inserted] =
        records_.emplace(
            id,
            Record{std::move(block),
                   identity,
                   1u,
                   0u,
                   true,
                   !runtime_registered});
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
    if (contextual_virtual_overlaps_) {
        auto candidate = active_virtual_ranges_.lower_bound({block.variant, 0u});
        for (;
             candidate != active_virtual_ranges_.end() && candidate->first.variant == block.variant;
             ++candidate) {
            if (candidate->second == ignored_id) continue;
            const auto& active = records_.at(candidate->second).block;
            if (ranges_overlap(block.virtual_start, block.size, active.virtual_start, active.size))
                return candidate->second;
        }
        return std::nullopt;
    }
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
    if (!record.static_block && (record.block.physical_origin & 1u) == 0u) {
        const auto page = record.block.physical_origin / physical_page_size;
        const auto halfword =
            (record.block.physical_origin % physical_page_size) / 2u;
        if (page < static_aot_pages_.size() && static_aot_pages_[page]) {
            auto& static_page = *static_aot_pages_[page];
            auto& shadow = static_page.dynamic_entries[halfword];
            if (!shadow)
                shadow = std::make_unique<std::vector<const Record*>>();
            shadow->push_back(&record);
            const auto entry = static_page.entries[halfword];
            if (entry != 0u &&
                entry != std::numeric_limits<std::uint32_t>::max() &&
                entry <= static_aot_entries_.size())
                advance_generation(static_aot_entries_[entry - 1u].generation);
        }
    }
    // Every active-index mutation invalidates previously captured dispatch
    // descriptors. In particular, a newly materialized dynamic block can take
    // precedence over a static entry on the same page.
    advance_generation(dispatch_generation_);
}

void RuntimeBlockTable::rebuild_static_aot_index() {
    std::vector<std::unique_ptr<StaticAotPage>> pages(static_aot_page_count);
    std::vector<StaticAotEntry> entries;
    entries.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        static_cast<void>(id);
        if (!record.active) continue;
        if (!record.static_block ||
            record.block.static_variant_policy !=
                StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic ||
            !direct_p1_p2_block_binding_contiguous(
                record.block.virtual_start,
                record.block.physical_origin,
                record.block.size))
            continue;
        const auto page_index = record.block.physical_origin / physical_page_size;
        if (page_index >= pages.size()) continue;
        const auto halfword =
            (record.block.physical_origin % physical_page_size) / 2u;
        if (!pages[page_index]) pages[page_index] = std::make_unique<StaticAotPage>();
        auto& page = *pages[page_index];
        auto& slot = page.entries[halfword];
        if (slot == std::numeric_limits<std::uint32_t>::max()) continue;
        if (slot != 0u && entries[slot - 1u].record != &record) {
            slot = std::numeric_limits<std::uint32_t>::max();
            continue;
        }
        if (slot == 0u) {
            entries.push_back({&record, id, 1u});
            slot = static_cast<std::uint32_t>(entries.size());
        }
    }
    for (const auto& [id, record] : records_) {
        static_cast<void>(id);
        if (!record.active || record.static_block ||
            (record.block.physical_origin & 1u) != 0u)
            continue;
        const auto page_index = record.block.physical_origin / physical_page_size;
        if (page_index >= pages.size() || !pages[page_index]) continue;
        const auto halfword =
            (record.block.physical_origin % physical_page_size) / 2u;
        auto& shadow = pages[page_index]->dynamic_entries[halfword];
        if (!shadow)
            shadow = std::make_unique<std::vector<const Record*>>();
        shadow->push_back(&record);
    }
    static_aot_pages_ = std::move(pages);
    static_aot_entries_ = std::move(entries);
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

std::optional<ValidatedBlockExecution>
RuntimeBlockTable::lookup_static_aot(const std::uint32_t physical_address,
                                    const std::uint32_t virtual_address,
                                    const BlockVariantKey& variant) const noexcept {
    if (!static_sealed_ || (physical_address & 1u) != 0u ||
        (virtual_address & 1u) != 0u ||
        ((virtual_address >> 29u) != 4u && (virtual_address >> 29u) != 5u) ||
        static_aot_invalidation_ != StaticAotInvalidationContract::Coordinated ||
        static_aot_pages_.empty())
        return std::nullopt;
    const auto canonical = canonical_physical_address(physical_address);
    if (canonical_physical_address(virtual_address) != canonical)
        return std::nullopt;
    const auto page_index = canonical / physical_page_size;
    if (page_index >= static_aot_pages_.size() ||
        !static_aot_pages_[page_index])
        return std::nullopt;
    const auto halfword = (canonical % physical_page_size) / 2u;
    const auto& page = *static_aot_pages_[page_index];
    if (const auto& shadow = page.dynamic_entries[halfword]; shadow) {
        const auto matching_dynamic =
            std::any_of(shadow->begin(), shadow->end(), [&](const Record* candidate) {
                return candidate != nullptr && dispatchable(*candidate) &&
                       !candidate->static_block &&
                       candidate->block.variant == variant;
            });
        if (matching_dynamic) return std::nullopt;
    }
    const auto entry_index = page.entries[halfword];
    if (entry_index == 0u ||
        entry_index == std::numeric_limits<std::uint32_t>::max() ||
        entry_index > static_aot_entries_.size())
        return std::nullopt;
    const auto& entry = static_aot_entries_[entry_index - 1u];
    const auto authoritative_record = records_.find(entry.id);
    if (entry.id == 0u || authoritative_record == records_.end() ||
        entry.record != &authoritative_record->second)
        return std::nullopt;
    const auto* const record = &authoritative_record->second;
    if (!dispatchable(*record) ||
        !record->static_block || record->block.runtime_registered ||
        record->block.function == nullptr || record->block.size < 2u ||
        record->block.physical_origin != canonical ||
        record->block.static_variant_policy !=
            StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic ||
        !direct_p1_p2_block_binding_contiguous(
            record->block.virtual_start,
            record->block.physical_origin,
            record->block.size) ||
        record->block.variant.runtime_generation != variant.runtime_generation)
        return std::nullopt;

    ValidatedBlockExecution execution;
    execution.block = {entry.id, record->generation};
    execution.function = record->block.function;
    // Native owner functions are keyed by their emitted virtual entry. P1/P2
    // aliases select the same physical implementation, but execution must use
    // the record entry just like the validated slow alias path.
    execution.virtual_start = record->block.virtual_start;
    execution.physical_origin = canonical;
    execution.size = record->block.size;
    execution.variant = variant;
    execution.end_kind = record->block.end_kind;
    execution.runtime_registered = false;
    execution.provenance = record->block.provenance;
    execution.fastpath = record->block.fastpath;
    execution.generation_guard = {
        .block = execution.block,
        .kind = BlockDispatchGenerationGuardKind::StaticAot,
        .table_lifetime = dispatch_lifetime_,
        .table_generation = entry.generation,
        .static_entry_index = entry_index,
        .runtime_registered = false,
    };
    execution.generation_guard_reusable = true;
    return execution;
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

std::uint64_t RuntimeBlockTable::dispatch_lifetime() const noexcept {
    return dispatch_lifetime_;
}

std::uint64_t RuntimeBlockTable::dispatch_generation() const noexcept {
    return dispatch_generation_;
}

bool RuntimeBlockTable::static_aot_dispatch_ready() const noexcept {
    return static_sealed_ &&
           static_aot_invalidation_ ==
               StaticAotInvalidationContract::Coordinated &&
           !static_aot_pages_.empty();
}

bool RuntimeBlockTable::static_dispatch_generation_guard_current(
    const BlockDispatchGenerationGuard& guard) const noexcept {
    if (!static_sealed_ ||
        guard.kind != BlockDispatchGenerationGuardKind::StaticAot ||
        guard.runtime_registered ||
        static_aot_invalidation_ !=
            StaticAotInvalidationContract::Coordinated ||
        guard.table_lifetime != dispatch_lifetime_ ||
        guard.static_entry_index == 0u ||
        guard.static_entry_index > static_aot_entries_.size())
        return false;

    const auto& entry = static_aot_entries_[guard.static_entry_index - 1u];
    if (entry.record == nullptr || entry.id == 0u ||
        guard.block.id != entry.id ||
        guard.table_generation != entry.generation)
        return false;
    const auto record = records_.find(entry.id);
    return record != records_.end() && entry.record == &record->second &&
           guard.block.generation == record->second.generation &&
           dispatchable(record->second) && record->second.static_block &&
           !record->second.block.runtime_registered &&
           record->second.block.function != nullptr &&
           record->second.block.static_variant_policy ==
               StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic &&
           direct_p1_p2_block_binding_contiguous(
               record->second.block.virtual_start,
               record->second.block.physical_origin,
               record->second.block.size);
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
            record.block.static_variant_policy,
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
    if (!record.static_block && (record.block.physical_origin & 1u) == 0u) {
        const auto page = record.block.physical_origin / physical_page_size;
        const auto halfword =
            (record.block.physical_origin % physical_page_size) / 2u;
        if (page < static_aot_pages_.size() && static_aot_pages_[page]) {
            auto& static_page = *static_aot_pages_[page];
            auto& shadow = static_page.dynamic_entries[halfword];
            if (shadow) {
                std::erase(*shadow, &record);
                if (shadow->empty()) shadow.reset();
            }
            const auto entry = static_page.entries[halfword];
            if (entry != 0u &&
                entry != std::numeric_limits<std::uint32_t>::max() &&
                entry <= static_aot_entries_.size())
                advance_generation(static_aot_entries_[entry - 1u].generation);
        }
    }
    if (record.static_block && !static_aot_pages_.empty()) {
        const auto page = record.block.physical_origin / physical_page_size;
        const auto halfword =
            (record.block.physical_origin % physical_page_size) / 2u;
        if (page < static_aot_pages_.size() && static_aot_pages_[page]) {
            const auto entry = static_aot_pages_[page]->entries[halfword];
            if (entry != 0u &&
                entry != std::numeric_limits<std::uint32_t>::max() &&
                entry <= static_aot_entries_.size() &&
                static_aot_entries_[entry - 1u].record == &record) {
                advance_generation(static_aot_entries_[entry - 1u].generation);
                static_aot_pages_[page]->entries[halfword] = 0u;
            }
        }
    }
    record.active = false;
    ++record.generation;
    if (record.generation == 0u) ++record.generation;
    advance_generation(dispatch_generation_);
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

bool RuntimeBlockTable::may_overlap_active_physical(
    const std::uint32_t address,
    const std::size_t size) const noexcept {
    if (size == 0u) return false;
    constexpr auto address_space_end =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    if (size > address_space_end - address) return true;

    const auto canonical = canonical_physical_address(address);
    const auto final_address =
        address + static_cast<std::uint32_t>(static_cast<std::uint64_t>(size) - 1u);
    const auto expected_final = static_cast<std::uint64_t>(canonical) + size - 1u;
    if (expected_final > std::numeric_limits<std::uint32_t>::max() ||
        canonical_physical_address(final_address) != expected_final)
        return true;

    const auto range_end = expected_final + 1u;
    const auto first_page = canonical / physical_page_size;
    const auto last_page = static_cast<std::uint32_t>(expected_final / physical_page_size);
    for (auto page = first_page;; ++page) {
        if (const auto candidates = active_physical_pages_.find(page);
            candidates != active_physical_pages_.end()) {
            for (const auto id : candidates->second) {
                const auto record = records_.find(id);
                if (record == records_.end() || !record->second.active) continue;
                const auto validation_start = validation_physical_start(record->second.block);
                const auto validation_end =
                    static_cast<std::uint64_t>(validation_start) +
                    validation_extent(record->second.block);
                if (canonical < validation_end && validation_start < range_end) return true;
            }
        }
        if (page == last_page) break;
    }
    return false;
}

RuntimeBlockTable::PreparedDiscLoadInvalidation
RuntimeBlockTable::prepare_disc_load_invalidation(const std::uint32_t physical_address,
                                                  const std::size_t size) const {
    PreparedDiscLoadInvalidation plan;
    if (size == 0u) return plan;
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
    plan.ids.reserve(candidates.size());
    for (const auto id : candidates) {
        const auto& record = records_.at(id);
        const auto tracked_start = validation_physical_start(record.block);
        if (record.active &&
            native_aot_write_overlaps_immutable(
                tracked_start,
                validation_extent(record.block),
                validation_mutable_ranges(record.block),
                canonical,
                static_cast<std::size_t>(write_end - canonical)))
            plan.ids.push_back(id);
    }
    return plan;
}

std::size_t
RuntimeBlockTable::commit_disc_load_invalidation(PreparedDiscLoadInvalidation plan) noexcept {
    std::size_t invalidated = 0u;
    for (const auto id : plan.ids) {
        const auto found = records_.find(id);
        if (found == records_.end() || !found->second.active) continue;
        deactivate(id);
        ++invalidated;
    }
    return invalidated;
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
        if (native_aot_write_overlaps_immutable(
                tracked_start,
                validation_extent(block),
                validation_mutable_ranges(block),
                canonical,
                static_cast<std::size_t>(write_end - canonical)))
            invalidated.push_back(id);
    }
    for (const auto id : invalidated)
        deactivate(id);
    return invalidated.size();
}

std::size_t RuntimeBlockTable::erase_overlapping_physical_batch(
    const std::span<const GuestWriteEvent> events) noexcept {
    if (events.size() > direct_linear_write_batch_capacity)
        std::terminate();

    ++write_batch_visit_epoch_;
    if (write_batch_visit_epoch_ == 0u) {
        for (auto& [id, record] : records_) {
            static_cast<void>(id);
            record.write_batch_visit_epoch = 0u;
        }
        write_batch_visit_epoch_ = 1u;
    }
    const auto epoch = write_batch_visit_epoch_;
    std::size_t invalidated = 0u;

    const auto overlaps_any_changed_event =
        [&](const RuntimeBlock& block) noexcept {
            const auto tracked_start = validation_physical_start(block);
            for (const auto& event : events) {
                if (!event.bytes_changed) continue;
                std::uint32_t physical = 0u;
                std::uint32_t page = 0u;
                if (!direct_write_batch_range(event, physical, page))
                    std::terminate();
                if (native_aot_write_overlaps_immutable(
                        tracked_start,
                        validation_extent(block),
                        validation_mutable_ranges(block),
                        physical,
                        event.size))
                    return true;
            }
            return false;
        };

    for (std::size_t event_index = 0u;
         event_index < events.size();
         ++event_index) {
        if (!events[event_index].bytes_changed) continue;
        std::uint32_t physical = 0u;
        std::uint32_t page = 0u;
        if (!direct_write_batch_range(
                events[event_index], physical, page))
            std::terminate();
        bool page_seen = false;
        for (std::size_t previous = 0u;
             previous < event_index;
             ++previous) {
            if (!events[previous].bytes_changed) continue;
            std::uint32_t previous_physical = 0u;
            std::uint32_t previous_page = 0u;
            if (!direct_write_batch_range(
                    events[previous],
                    previous_physical,
                    previous_page))
                std::terminate();
            if (previous_page == page) {
                page_seen = true;
                break;
            }
        }
        if (page_seen) continue;

        auto candidates = active_physical_pages_.find(page);
        if (candidates == active_physical_pages_.end()) continue;
        auto candidate = candidates->second.begin();
        while (candidates != active_physical_pages_.end() &&
               candidate != candidates->second.end()) {
            const auto id = *candidate;
            ++candidate;
            const auto found = records_.find(id);
            if (found == records_.end() || !found->second.active ||
                found->second.write_batch_visit_epoch == epoch)
                continue;
            found->second.write_batch_visit_epoch = epoch;
            if (!overlaps_any_changed_event(found->second.block))
                continue;

            deactivate(id);
            ++invalidated;
            // deactivate() can erase the current page map node. Reacquire it;
            // the per-record epoch prevents duplicate candidate work.
            candidates = active_physical_pages_.find(page);
            if (candidates == active_physical_pages_.end()) break;
            candidate = candidates->second.begin();
        }
    }
    return invalidated;
}

void RuntimeBlockTable::bind_code_tracker(
    const ExecutableCodeTracker* const tracker,
    const StaticAotInvalidationContract static_aot_invalidation) noexcept {
    if (code_tracker_ == tracker &&
        static_aot_invalidation_ == static_aot_invalidation)
        return;
    code_tracker_ = tracker;
    static_aot_invalidation_ = static_aot_invalidation;
    for (auto& entry : static_aot_entries_)
        advance_generation(entry.generation);
    advance_generation(dispatch_generation_);
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
    static_aot_pages_.clear();
    static_aot_entries_.clear();
    write_batch_visit_epoch_ = 0u;
    active_count_ = 0u;
    static_sealed_ = false;
    static_aot_invalidation_ = StaticAotInvalidationContract::Conservative;
    contextual_virtual_overlaps_ = false;
    lookup_counters_ = {};
    rejected_generations_.clear();
    dispatch_lifetime_ = allocate_block_table_lifetime();
    advance_generation(dispatch_generation_);
}

} // namespace katana::runtime
