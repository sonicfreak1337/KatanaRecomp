#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/indirect_dispatch.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::string_view runtime_write_module_id_prefix = "guest-runtime-write-";
constexpr std::string_view runtime_write_module_source_identity = "guest-runtime-write-v1";
constexpr std::uint64_t executable_address_space_size = 0x1'0000'0000ull;

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

void accumulate(std::uint64_t& value, const std::uint64_t amount) noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - value)
        value = std::numeric_limits<std::uint64_t>::max();
    else
        value += amount;
}

std::uint64_t next_module_incarnation_generation(
    const std::span<const ExecutableModule> modules,
    const std::string_view id,
    const std::uint64_t initial_generation) {
    std::uint64_t latest_generation = 0u;
    for (const auto& module : modules) {
        if (module.id == id)
            latest_generation = std::max(latest_generation, module.generation);
    }
    if (latest_generation == 0u) return initial_generation;
    if (latest_generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "Ausfuehrbare Modulinkarnation hat ihre Generation ausgeschoepft.");
    }
    return latest_generation + 1u;
}

std::uint32_t read_u32_le(const std::span<const std::uint8_t> bytes,
                          const std::size_t offset) noexcept {
    std::uint32_t value = 0u;
    for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
        value |= static_cast<std::uint32_t>(bytes[offset + byte]) << (byte * 8u);
    return value;
}

void validate_relocations(const ExecutableModule& module,
                          const std::span<const ExecutableModuleRelocation> relocations) {
    std::vector<std::uint32_t> offsets;
    offsets.reserve(relocations.size());
    for (const auto& relocation : relocations) {
        if (relocation.type != executable_module_relocation_module_base32)
            throw std::invalid_argument("Ausfuehrbares Modul besitzt einen unbekannten "
                                        "Relocationtyp.");
        if (relocation.offset > module.bytes.size() ||
            sizeof(std::uint32_t) > module.bytes.size() - relocation.offset)
            throw std::invalid_argument("Modulrelocation liegt ausserhalb der Modulbytes.");
        offsets.push_back(relocation.offset);
    }
    std::sort(offsets.begin(), offsets.end());
    for (std::size_t index = 1u; index < offsets.size(); ++index) {
        if (static_cast<std::uint64_t>(offsets[index]) <
            static_cast<std::uint64_t>(offsets[index - 1u]) + sizeof(std::uint32_t))
            throw std::invalid_argument("Modulrelocationen ueberlappen sich.");
    }
}

std::uint8_t relocated_module_byte(const ExecutableModule& module,
                                   const std::size_t offset) noexcept {
    for (const auto& relocation : module.relocations) {
        if (offset < relocation.offset ||
            offset >= static_cast<std::size_t>(relocation.offset) + sizeof(std::uint32_t))
            continue;
        const auto source = read_u32_le(module.bytes, relocation.offset);
        const auto relocated =
            source + module.guest_start + static_cast<std::uint32_t>(relocation.addend);
        return static_cast<std::uint8_t>(relocated >> ((offset - relocation.offset) * 8u));
    }
    return module.bytes[offset];
}

std::optional<std::size_t> module_offset(const ExecutableModule& module,
                                         const std::uint32_t address,
                                         const std::size_t width) noexcept {
    const auto module_begin = canonical_physical_address(module.guest_start);
    const auto begin = canonical_physical_address(address);
    const auto end = static_cast<std::uint64_t>(begin) + width;
    const auto module_end = static_cast<std::uint64_t>(module_begin) + module.bytes.size();
    if (width == 0u || begin < module_begin || end < begin || end > module_end) return std::nullopt;
    return static_cast<std::size_t>(begin - module_begin);
}

bool canonical_range_is_linear(const std::uint32_t address, const std::size_t size) noexcept {
    if (size == 0u ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u -
                   address)
        return false;
    const auto physical_begin = canonical_physical_address(address);
    const auto final_address =
        address + static_cast<std::uint32_t>(static_cast<std::uint64_t>(size) - 1u);
    const auto expected_physical_end =
        static_cast<std::uint64_t>(physical_begin) + size - 1u;
    return expected_physical_end <= std::numeric_limits<std::uint32_t>::max() &&
           canonical_physical_address(final_address) == expected_physical_end;
}

std::optional<std::pair<std::uint32_t, std::uint64_t>>
canonical_load_range(const std::uint32_t address, const std::size_t size) noexcept {
    if (!canonical_range_is_linear(address, size)) return std::nullopt;
    const auto begin = canonical_physical_address(address);
    return std::pair{begin, static_cast<std::uint64_t>(begin) + size};
}

void normalize_active_extents(ExecutableModule& module) {
    if (module.active_extents.empty())
        module.active_extents.push_back(
            {0u, static_cast<std::uint32_t>(module.bytes.size())});
    std::sort(module.active_extents.begin(),
              module.active_extents.end(),
              [](const auto& left, const auto& right) { return left.offset < right.offset; });
    std::vector<ExecutableModuleActiveExtent> normalized;
    normalized.reserve(module.active_extents.size());
    for (const auto extent : module.active_extents) {
        if (extent.size == 0u || extent.offset > module.bytes.size() ||
            extent.size > module.bytes.size() - extent.offset)
            throw std::invalid_argument("Aktiver Modulbereich liegt ausserhalb der Modulbytes.");
        if (!normalized.empty()) {
            const auto previous_end = static_cast<std::uint64_t>(normalized.back().offset) +
                                      normalized.back().size;
            if (extent.offset < previous_end)
                throw std::invalid_argument("Aktive Modulbereiche ueberlappen sich.");
            if (extent.offset == previous_end) {
                normalized.back().size += extent.size;
                continue;
            }
        }
        normalized.push_back(extent);
    }
    module.active_extents = std::move(normalized);
}

void validate_and_normalize_module(ExecutableModule& module) {
    if (module.id.empty() || module.source_identity.empty() || module.bytes.empty() ||
        module.bytes.size() > std::numeric_limits<std::uint32_t>::max() ||
        (module.guest_start & 1u) != 0u || module.generation == 0u ||
        module.relocation_generation == 0u) {
        throw std::invalid_argument(
            "Ausfuehrbares Modul besitzt keine stabile Identitaet oder Bytes.");
    }
    if (module.end_address() >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::out_of_range("Ausfuehrbares Modul ueberschreitet den Gastadressraum.");
    }
    if (!canonical_range_is_linear(module.guest_start, module.bytes.size()))
        throw std::out_of_range(
            "Ausfuehrbares Modul ueberschreitet eine nichtlineare SH-4-Adressaliasgrenze.");
    normalize_active_extents(module);
    for (const auto extent : module.active_extents) {
        const auto extent_address = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(module.guest_start) + extent.offset);
        if (!canonical_range_is_linear(extent_address, extent.size))
            throw std::out_of_range(
                "Aktiver Modulbereich ueberschreitet eine SH-4-Adressaliasgrenze.");
    }
    validate_relocations(module, module.relocations);
    for (const auto& range : module.range_roles) {
        if (range.size == 0u || range.offset > module.bytes.size() ||
            range.size > module.bytes.size() - range.offset)
            throw std::invalid_argument("Modulbereichsrolle liegt ausserhalb der Modulbytes.");
    }
    module.active = true;
}

bool physical_extents_overlap(const ExecutableModule& left,
                              const ExecutableModule& right) noexcept {
    const auto left_base = canonical_physical_address(left.guest_start);
    const auto right_base = canonical_physical_address(right.guest_start);
    for (const auto left_extent : left.active_extents) {
        const auto left_begin = static_cast<std::uint64_t>(left_base) + left_extent.offset;
        const auto left_end = left_begin + left_extent.size;
        for (const auto right_extent : right.active_extents) {
            const auto right_begin = static_cast<std::uint64_t>(right_base) + right_extent.offset;
            const auto right_end = right_begin + right_extent.size;
            if (left_begin < right_end && right_begin < left_end) return true;
        }
    }
    return false;
}

bool physical_extents_overlap(const ExecutableModule& left,
                              const std::uint32_t right_guest_start,
                              const std::span<const ExecutableModuleActiveExtent> right_extents)
    noexcept {
    const auto left_base = canonical_physical_address(left.guest_start);
    const auto right_base = canonical_physical_address(right_guest_start);
    for (const auto left_extent : left.active_extents) {
        const auto left_begin = static_cast<std::uint64_t>(left_base) + left_extent.offset;
        const auto left_end = left_begin + left_extent.size;
        for (const auto right_extent : right_extents) {
            const auto right_begin =
                static_cast<std::uint64_t>(right_base) + right_extent.offset;
            const auto right_end = right_begin + right_extent.size;
            if (left_begin < right_end && right_begin < left_end) return true;
        }
    }
    return false;
}

bool subtract_physical_range(std::vector<ExecutableModuleActiveExtent>& extents,
                             const std::uint32_t module_guest_start,
                             const std::uint32_t physical_begin,
                             const std::uint64_t physical_end) {
    const auto module_base = canonical_physical_address(module_guest_start);
    std::vector<ExecutableModuleActiveExtent> remaining;
    remaining.reserve(extents.size() + 1u);
    for (const auto extent : extents) {
        const auto extent_begin = static_cast<std::uint64_t>(module_base) + extent.offset;
        const auto extent_end = extent_begin + extent.size;
        if (physical_begin >= extent_end || extent_begin >= physical_end) {
            remaining.push_back(extent);
            continue;
        }
        const auto overlap_begin = std::max<std::uint64_t>(physical_begin, extent_begin);
        const auto overlap_end = std::min(physical_end, extent_end);
        if (extent_begin < overlap_begin)
            remaining.push_back({extent.offset,
                                 static_cast<std::uint32_t>(overlap_begin - extent_begin)});
        if (overlap_end < extent_end)
            remaining.push_back(
                {static_cast<std::uint32_t>(overlap_end - module_base),
                 static_cast<std::uint32_t>(extent_end - overlap_end)});
    }
    if (remaining == extents) return false;
    extents = std::move(remaining);
    return true;
}

bool physical_range_overlaps_active_extents(const ExecutableModule& module,
                                            const std::uint32_t physical_begin,
                                            const std::uint64_t physical_end) noexcept {
    if (!module.active) return false;
    const auto module_base = canonical_physical_address(module.guest_start);
    return std::any_of(module.active_extents.begin(),
                       module.active_extents.end(),
                       [&](const auto extent) {
                           const auto extent_begin =
                               static_cast<std::uint64_t>(module_base) + extent.offset;
                           const auto extent_end = extent_begin + extent.size;
                           return physical_begin < extent_end && extent_begin < physical_end;
                       });
}

bool subtract_physical_range(ExecutableModule& module,
                             const std::uint32_t physical_begin,
                             const std::uint64_t physical_end) {
    if (!physical_range_overlaps_active_extents(module, physical_begin, physical_end))
        return false;
    const auto module_base = canonical_physical_address(module.guest_start);
    std::vector<ExecutableModuleActiveExtent> remaining;
    remaining.reserve(module.active_extents.size() + 1u);
    for (const auto extent : module.active_extents) {
        const auto extent_begin = static_cast<std::uint64_t>(module_base) + extent.offset;
        const auto extent_end = extent_begin + extent.size;
        if (physical_begin >= extent_end || extent_begin >= physical_end) {
            remaining.push_back(extent);
            continue;
        }
        const auto overlap_begin = std::max<std::uint64_t>(physical_begin, extent_begin);
        const auto overlap_end = std::min(physical_end, extent_end);
        if (extent_begin < overlap_begin)
            remaining.push_back({extent.offset,
                                 static_cast<std::uint32_t>(overlap_begin - extent_begin)});
        if (overlap_end < extent_end)
            remaining.push_back(
                {static_cast<std::uint32_t>(overlap_end - module_base),
                 static_cast<std::uint32_t>(extent_end - overlap_end)});
    }
    const auto changed = remaining != module.active_extents;
    module.active_extents = std::move(remaining);
    module.active = !module.active_extents.empty();
    return changed;
}

std::uint64_t invalidate_active_module_extents(const ExecutableModule& module,
                                               RuntimeBlockTable& blocks,
                                               ExecutableCodeTracker& tracker) {
    const auto module_base = canonical_physical_address(module.guest_start);
    std::uint64_t invalidated_blocks = 0u;
    std::uint64_t invalidated_tracked_blocks = 0u;
    for (const auto extent : module.active_extents) {
        const auto extent_address = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(module_base) + extent.offset);
        accumulate(invalidated_blocks,
                   blocks.erase_overlapping_physical(extent_address, extent.size));
        const auto tracked =
            tracker.observe_write(extent_address, extent.size, CodeWriteSource::Copy, true);
        accumulate(invalidated_tracked_blocks, tracked.invalidated_blocks.size());
    }
    return std::max(invalidated_blocks, invalidated_tracked_blocks);
}

} // namespace

std::uint64_t ExecutableModule::end_address() const noexcept {
    return static_cast<std::uint64_t>(guest_start) + bytes.size();
}

bool ExecutableModule::contains(const std::uint32_t address,
                                const std::size_t width) const noexcept {
    if (!active) return false;
    const auto offset = module_offset(*this, address, width);
    if (!offset) return false;
    const auto end = static_cast<std::uint64_t>(*offset) + width;
    return std::any_of(active_extents.begin(), active_extents.end(), [&](const auto extent) {
        const auto extent_end = static_cast<std::uint64_t>(extent.offset) + extent.size;
        return *offset >= extent.offset && end <= extent_end;
    });
}

std::size_t
ExecutableModule::active_extent_remaining(const std::uint32_t address) const noexcept {
    if (!active) return 0u;
    const auto offset = module_offset(*this, address, 1u);
    if (!offset) return 0u;
    for (const auto extent : active_extents) {
        const auto extent_end = static_cast<std::uint64_t>(extent.offset) + extent.size;
        if (*offset >= extent.offset && *offset < extent_end)
            return static_cast<std::size_t>(extent_end - *offset);
    }
    return 0u;
}

bool ExecutableModule::materializable(const std::uint32_t address,
                                      const std::size_t width) const noexcept {
    if (!executable_permission || !contains(address, width)) return false;
    const auto begin = *module_offset(*this, address, width);
    const auto end = begin + width;
    for (const auto& range : range_roles) {
        if (range.role == ExecutableStorageRole::RuntimeMaterializable || range.size == 0u)
            continue;
        const auto range_end = static_cast<std::uint64_t>(range.offset) + range.size;
        if (begin < range_end && range.offset < end) return false;
    }
    return true;
}

void ExecutableLoadWriteTracker::observe(const GuestWriteEvent& event) noexcept {
    if (event.source != CodeWriteSource::Copy && event.source != CodeWriteSource::Dma) {
        reset();
        return;
    }
    const auto range = canonical_load_range(event.address, event.size);
    if (!range) {
        reset();
        return;
    }
    const auto [begin, end] = *range;
    if (pending_ && source_ == event.source && physical_end_ == begin) {
        physical_end_ = end;
        bytes_changed_ = bytes_changed_ || event.bytes_changed;
        return;
    }
    physical_begin_ = begin;
    physical_end_ = end;
    source_ = event.source;
    bytes_changed_ = event.bytes_changed;
    pending_ = true;
}

LoadedRangeWriteObservation ExecutableLoadWriteTracker::consume(const std::uint32_t address,
                                                                const std::size_t size) noexcept {
    const auto range = canonical_load_range(address, size);
    if (!range || !pending_ || range->first != physical_begin_ ||
        range->second != physical_end_) {
        reset();
        return LoadedRangeWriteObservation::UnobservedChanged;
    }
    const auto result = bytes_changed_ ? LoadedRangeWriteObservation::ObservedChanged
                                       : LoadedRangeWriteObservation::ObservedByteIdentical;
    reset();
    return result;
}

void ExecutableLoadWriteTracker::reset() noexcept {
    physical_begin_ = 0u;
    physical_end_ = 0u;
    source_ = CodeWriteSource::Copy;
    bytes_changed_ = false;
    pending_ = false;
}

void ExecutableModuleCatalog::index_active_extents(const ExecutableModule& module) {
    if (!module.active) return;
    const auto module_base = canonical_physical_address(module.guest_start);
    // First ensure every key exists. If allocation fails, only zero-count tombstones may have
    // been added and the live index cannot acquire a false-positive or false-negative count.
    for (const auto extent : module.active_extents) {
        const auto extent_begin = static_cast<std::uint64_t>(module_base) + extent.offset;
        const auto extent_end = extent_begin + extent.size;
        const auto first_page = extent_begin / runtime_write_page_size * runtime_write_page_size;
        const auto last_page =
            (extent_end - 1u) / runtime_write_page_size * runtime_write_page_size;
        for (auto page = first_page;; page += runtime_write_page_size) {
            static_cast<void>(active_extent_page_refcounts_.try_emplace(
                static_cast<std::uint32_t>(page), 0u));
            if (page == last_page) break;
        }
    }
    for (const auto extent : module.active_extents) {
        const auto extent_begin = static_cast<std::uint64_t>(module_base) + extent.offset;
        const auto extent_end = extent_begin + extent.size;
        const auto first_page = extent_begin / runtime_write_page_size * runtime_write_page_size;
        const auto last_page =
            (extent_end - 1u) / runtime_write_page_size * runtime_write_page_size;
        for (auto page = first_page;; page += runtime_write_page_size) {
            auto& references =
                active_extent_page_refcounts_.at(static_cast<std::uint32_t>(page));
            if (references != std::numeric_limits<std::uint64_t>::max()) ++references;
            if (page == last_page) break;
        }
    }
}

void ExecutableModuleCatalog::unindex_active_extents(const ExecutableModule& module) noexcept {
    if (!module.active) return;
    unindex_active_extents(module.guest_start, module.active_extents);
}

void ExecutableModuleCatalog::reserve_active_extent_index(const ExecutableModule& module) {
    reserve_active_extent_index(module.guest_start, module.active_extents);
}

void ExecutableModuleCatalog::reserve_active_extent_index(
    const std::uint32_t guest_start,
    const std::span<const ExecutableModuleActiveExtent> extents,
    std::vector<std::uint32_t>* const inserted_pages) {
    const auto module_base = canonical_physical_address(guest_start);
    for (const auto extent : extents) {
        const auto extent_begin = static_cast<std::uint64_t>(module_base) + extent.offset;
        const auto extent_end = extent_begin + extent.size;
        const auto first_page = extent_begin / runtime_write_page_size * runtime_write_page_size;
        const auto last_page =
            (extent_end - 1u) / runtime_write_page_size * runtime_write_page_size;
        for (auto page = first_page;; page += runtime_write_page_size) {
            const auto page32 = static_cast<std::uint32_t>(page);
            const auto [entry, inserted] =
                active_extent_page_refcounts_.try_emplace(page32, 0u);
            static_cast<void>(entry);
            if (inserted && inserted_pages != nullptr) {
                try {
                    inserted_pages->push_back(page32);
                } catch (...) {
                    active_extent_page_refcounts_.erase(page32);
                    throw;
                }
            }
            if (page == last_page) break;
        }
    }
}

void ExecutableModuleCatalog::unindex_active_extents(
    const std::uint32_t guest_start,
    const std::span<const ExecutableModuleActiveExtent> extents) noexcept {
    const auto module_base = canonical_physical_address(guest_start);
    for (const auto extent : extents) {
        const auto extent_begin = static_cast<std::uint64_t>(module_base) + extent.offset;
        const auto extent_end = extent_begin + extent.size;
        const auto first_page = extent_begin / runtime_write_page_size * runtime_write_page_size;
        const auto last_page =
            (extent_end - 1u) / runtime_write_page_size * runtime_write_page_size;
        for (auto page = first_page;; page += runtime_write_page_size) {
            const auto found =
                active_extent_page_refcounts_.find(static_cast<std::uint32_t>(page));
            if (found != active_extent_page_refcounts_.end() && found->second != 0u)
                --found->second;
            if (page == last_page) break;
        }
    }
}

bool ExecutableModuleCatalog::active_extent_index_may_overlap(
    const std::uint32_t physical_begin,
    const std::uint64_t physical_end) const noexcept {
    if (physical_end <= physical_begin) return false;
    const auto first_page = physical_begin / runtime_write_page_size * runtime_write_page_size;
    const auto last_page = static_cast<std::uint32_t>(
        (physical_end - 1u) / runtime_write_page_size * runtime_write_page_size);
    for (auto page = first_page;; page += runtime_write_page_size) {
        const auto found = active_extent_page_refcounts_.find(page);
        if (found != active_extent_page_refcounts_.end() && found->second != 0u) return true;
        if (page == last_page) break;
    }
    return false;
}

void ExecutableModuleCatalog::publish(ExecutableModule module) {
    validate_and_normalize_module(module);
    if (find(module.id) != nullptr)
        throw std::invalid_argument("Ausfuehrbare Modulidentitaet ist bereits aktiv.");
    for (const auto& existing : modules_) {
        if (existing.active && physical_extents_overlap(module, existing))
            throw std::invalid_argument("Ausfuehrbare Module ueberlappen sich.");
    }
    module.generation =
        next_module_incarnation_generation(modules_, module.id, module.generation);
    modules_.reserve(modules_.size() + 1u);
    reserve_active_extent_index(module);
    index_active_extents(module);
    modules_.push_back(std::move(module));
    increment(metrics_.loads);
}

ExecutableModuleCatalog::PreparedDiscLoadCatalog
ExecutableModuleCatalog::prepare_disc_load_catalog(
    std::vector<ExecutableModule> modules,
    const std::span<const std::pair<std::uint32_t, std::size_t>> invalidated_ranges) {
    if (invalidated_ranges.empty())
        throw std::invalid_argument("Disc-Ladetransaktion besitzt keine Invalidierungsbereiche.");
    for (const auto [address, size] : invalidated_ranges) {
        const auto begin = canonical_physical_address(address);
        if (size == 0u ||
            size > executable_address_space_size - static_cast<std::uint64_t>(begin))
            throw std::out_of_range(
                "Disc-Ladetransaktion invalidiert ausserhalb des Gastadressraums.");
    }
    for (auto& module : modules)
        validate_and_normalize_module(module);
    for (std::size_t index = 0u; index < modules.size(); ++index) {
        for (std::size_t previous = 0u; previous < index; ++previous) {
            if (modules[index].id == modules[previous].id)
                throw std::invalid_argument(
                    "Disc-Ladetransaktion verwendet eine Modulidentitaet mehrfach.");
            if (physical_extents_overlap(modules[index], modules[previous]))
                throw std::invalid_argument(
                    "Disc-Ladetransaktion besitzt ueberlappende Modulbereiche.");
        }
        modules[index].generation =
            next_module_incarnation_generation(modules_, modules[index].id, modules[index].generation);
    }

    PreparedDiscLoadCatalog plan;
    plan.modules = std::move(modules);
    plan.invalidated_ranges.assign(invalidated_ranges.begin(), invalidated_ranges.end());
    plan.updates.reserve(modules_.size());
    for (std::size_t index = 0u; index < modules_.size(); ++index) {
        const auto& existing = modules_[index];
        if (!existing.active) continue;
        auto remaining = existing.active_extents;
        bool changed = false;
        for (const auto [address, size] : invalidated_ranges) {
            const auto begin = canonical_physical_address(address);
            const auto end = static_cast<std::uint64_t>(begin) + size;
            changed =
                subtract_physical_range(remaining, existing.guest_start, begin, end) || changed;
        }
        if (changed) {
            const auto remains_active = !remaining.empty();
            plan.updates.push_back({index, std::move(remaining), remains_active});
        }
    }
    for (const auto& incoming : plan.modules) {
        for (std::size_t index = 0u; index < modules_.size(); ++index) {
            const auto& existing = modules_[index];
            if (!existing.active) continue;
            const auto update = std::find_if(
                plan.updates.begin(), plan.updates.end(), [&](const auto& candidate) {
                    return candidate.module_index == index;
                });
            if (existing.id == incoming.id &&
                (update == plan.updates.end() || update->remains_active))
                throw std::invalid_argument(
                    "Disc-Ladetransaktion laesst dieselbe Modulidentitaet aktiv.");
            const auto extents =
                update == plan.updates.end()
                    ? std::span<const ExecutableModuleActiveExtent>(existing.active_extents)
                    : std::span<const ExecutableModuleActiveExtent>(update->remaining_extents);
            if (!extents.empty() &&
                physical_extents_overlap(incoming, existing.guest_start, extents))
                throw std::invalid_argument(
                    "Disc-Ladetransaktion laesst einen ueberlappenden Altbereich aktiv.");
        }
    }

    modules_.reserve(modules_.size() + plan.modules.size());
    try {
        for (const auto& module : plan.modules)
            reserve_active_extent_index(
                module.guest_start, module.active_extents, &plan.inserted_index_pages);
        for (const auto& update : plan.updates)
            reserve_active_extent_index(
                modules_[update.module_index].guest_start,
                update.remaining_extents,
                &plan.inserted_index_pages);
    } catch (...) {
        cancel_disc_load_catalog(plan);
        throw;
    }
    return plan;
}

void ExecutableModuleCatalog::cancel_disc_load_catalog(
    PreparedDiscLoadCatalog& plan) noexcept {
    for (const auto page : plan.inserted_index_pages) {
        const auto found = active_extent_page_refcounts_.find(page);
        if (found != active_extent_page_refcounts_.end() && found->second == 0u)
            active_extent_page_refcounts_.erase(found);
    }
    plan.inserted_index_pages.clear();
}

void ExecutableModuleCatalog::commit_disc_load_catalog(
    PreparedDiscLoadCatalog plan) noexcept {
    const auto index_noalloc = [&](const ExecutableModule& module) noexcept {
        if (!module.active) return;
        const auto module_base = canonical_physical_address(module.guest_start);
        for (const auto extent : module.active_extents) {
            const auto extent_begin = static_cast<std::uint64_t>(module_base) + extent.offset;
            const auto extent_end = extent_begin + extent.size;
            const auto first_page =
                extent_begin / runtime_write_page_size * runtime_write_page_size;
            const auto last_page =
                (extent_end - 1u) / runtime_write_page_size * runtime_write_page_size;
            for (auto page = first_page;; page += runtime_write_page_size) {
                const auto found =
                    active_extent_page_refcounts_.find(static_cast<std::uint32_t>(page));
                if (found == active_extent_page_refcounts_.end()) std::terminate();
                if (found->second != std::numeric_limits<std::uint64_t>::max())
                    ++found->second;
                if (page == last_page) break;
            }
        }
    };
    for (auto& update : plan.updates) {
        auto& module = modules_[update.module_index];
        const auto was_active = module.active;
        unindex_active_extents(module);
        module.active_extents.swap(update.remaining_extents);
        module.active = update.remains_active;
        index_noalloc(module);
        if (was_active && !module.active) increment(metrics_.unloads);
    }
    for (const auto [address, size] : plan.invalidated_ranges) {
        const auto physical_begin = canonical_physical_address(address);
        const auto physical_end = static_cast<std::uint64_t>(physical_begin) + size;
        std::uint64_t cursor = physical_begin;
        while (cursor < physical_end) {
            const auto page =
                static_cast<std::uint32_t>(cursor) & ~(runtime_write_page_size - 1u);
            const auto page_end = std::min<std::uint64_t>(
                physical_end, static_cast<std::uint64_t>(page) + runtime_write_page_size);
            const auto found = runtime_write_pages_.find(page);
            if (found != runtime_write_pages_.end()) {
                auto offset = static_cast<std::uint32_t>(cursor - page);
                const auto end_offset = static_cast<std::uint32_t>(page_end - page);
                while (offset < end_offset) {
                    const auto word = offset / 64u;
                    const auto first_bit = offset % 64u;
                    const auto word_end =
                        std::min<std::uint32_t>(end_offset, (word + 1u) * 64u);
                    const auto last_bit = word_end - word * 64u;
                    auto mask = std::numeric_limits<std::uint64_t>::max() << first_bit;
                    if (last_bit < 64u)
                        mask &= (std::uint64_t{1u} << last_bit) - 1u;
                    found->second.written[word] &= ~mask;
                    offset = word_end;
                }
                if (std::all_of(found->second.written.begin(),
                                found->second.written.end(),
                                [](const auto word) { return word == 0u; }))
                    runtime_write_pages_.erase(found);
            }
            cursor = page_end;
        }
        increment(metrics_.write_index_scans);
    }
    for (auto& module : plan.modules) {
        index_noalloc(module);
        modules_.push_back(std::move(module));
        increment(metrics_.loads);
    }
}

void ExecutableModuleCatalog::publish_loaded_range(ExecutableModule module,
                                                   RuntimeBlockTable& blocks,
                                                   ExecutableCodeTracker& tracker,
                                                   const LoadedRangeWriteObservation observation) {
    validate_and_normalize_module(module);
    const auto physical = canonical_physical_address(module.guest_start);
    if (observation == LoadedRangeWriteObservation::ObservedByteIdentical) {
        struct IdenticalPhysicalOverlap {
            std::uint32_t begin = 0u;
            std::uint64_t end = 0u;
        };
        std::vector<IdenticalPhysicalOverlap> identical_overlaps;
        const auto incoming_extents = module.active_extents;
        for (const auto& existing : modules_) {
            if (!existing.active) continue;
            const auto existing_base = canonical_physical_address(existing.guest_start);
            for (const auto incoming_extent : incoming_extents) {
                const auto incoming_begin =
                    static_cast<std::uint64_t>(physical) + incoming_extent.offset;
                const auto incoming_end = incoming_begin + incoming_extent.size;
                for (const auto existing_extent : existing.active_extents) {
                    const auto existing_begin =
                        static_cast<std::uint64_t>(existing_base) + existing_extent.offset;
                    const auto existing_end = existing_begin + existing_extent.size;
                    const auto overlap_begin = std::max(incoming_begin, existing_begin);
                    const auto overlap_end = std::min(incoming_end, existing_end);
                    if (overlap_begin >= overlap_end) continue;

                    const auto incoming_offset =
                        static_cast<std::size_t>(overlap_begin - physical);
                    const auto existing_offset =
                        static_cast<std::size_t>(overlap_begin - existing_base);
                    const auto overlap_size =
                        static_cast<std::size_t>(overlap_end - overlap_begin);
                    for (std::size_t current = 0u; current < overlap_size; ++current) {
                        if (relocated_module_byte(existing, existing_offset + current) !=
                            relocated_module_byte(module, incoming_offset + current))
                            throw std::invalid_argument(
                                "Byte-identischer Load widerspricht aktiven Modulbytes.");
                    }
                    identical_overlaps.push_back(
                        {static_cast<std::uint32_t>(overlap_begin), overlap_end});
                }
            }
        }
        for (const auto overlap : identical_overlaps)
            static_cast<void>(subtract_physical_range(module, overlap.begin, overlap.end));
        if (!module.active) return;
    }
    for (const auto& existing : modules_) {
        if (existing.active && existing.id == module.id) {
            throw std::invalid_argument("Ausfuehrbare Modulidentitaet ist bereits aktiv.");
        }
    }
    module.generation =
        next_module_incarnation_generation(modules_, module.id, module.generation);

    // All deterministic rejection paths are complete before blocks, tracker, provenance or
    // catalog state are touched. Reserving here also gives the catalog commit a no-allocation
    // path after invalidation has begun.
    modules_.reserve(modules_.size() + 1u);
    reserve_active_extent_index(module);
    if (observation == LoadedRangeWriteObservation::UnobservedChanged) {
        const auto invalidated = blocks.erase_overlapping_physical(physical, module.bytes.size());
        const auto tracked =
            tracker.observe_write(physical, module.bytes.size(), CodeWriteSource::Copy, true);
        metrics_.invalidated_blocks += std::max(invalidated, tracked.invalidated_blocks.size());
        record_runtime_write(module.guest_start,
                             module.bytes.size(),
                             CodeWriteSource::Copy,
                             true);
    }
    index_active_extents(module);
    modules_.push_back(std::move(module));
    increment(metrics_.loads);
}

void ExecutableModuleCatalog::unload(const std::string_view id,
                                     RuntimeBlockTable& blocks,
                                     ExecutableCodeTracker& tracker) {
    const auto module = std::find_if(modules_.begin(), modules_.end(), [&](const auto& candidate) {
        return candidate.active && candidate.id == id;
    });
    if (module == modules_.end()) throw std::invalid_argument("Unbekanntes aktives Modul.");
    accumulate(metrics_.invalidated_blocks,
               invalidate_active_module_extents(*module, blocks, tracker));
    unindex_active_extents(*module);
    module->active = false;
    module->active_extents.clear();
    increment(metrics_.unloads);
}

void ExecutableModuleCatalog::replace(ExecutableModule module,
                                       RuntimeBlockTable& blocks,
                                       ExecutableCodeTracker& tracker) {
    const auto previous = std::find_if(modules_.begin(), modules_.end(), [&](const auto& candidate) {
        return candidate.active && candidate.id == module.id;
    });
    if (previous == modules_.end())
        throw std::invalid_argument("Zu ersetzendes Modul ist nicht aktiv.");
    module.generation =
        next_module_incarnation_generation(modules_, module.id, module.generation);
    validate_and_normalize_module(module);
    for (auto existing = modules_.begin(); existing != modules_.end(); ++existing) {
        if (existing != previous && existing->active && physical_extents_overlap(module, *existing))
            throw std::invalid_argument("Ersatzmodul ueberlappt ein anderes aktives Modul.");
    }

    // Every deterministic rejection and every allocation is complete before the old module,
    // runtime blocks or code tracker are mutated. The index reservation inserts only zero-count
    // pages and is therefore harmless even if allocation itself fails.
    modules_.reserve(modules_.size() + 1u);
    reserve_active_extent_index(module);
    const auto active_previous =
        std::find_if(modules_.begin(), modules_.end(), [&](const auto& candidate) {
            return candidate.active && candidate.id == module.id;
        });
    if (active_previous == modules_.end())
        throw std::logic_error("Vorvalidiertes Ersatzmodul verlor sein aktives Quellmodul.");
    accumulate(metrics_.invalidated_blocks,
               invalidate_active_module_extents(*active_previous, blocks, tracker));
    unindex_active_extents(*active_previous);
    active_previous->active = false;
    active_previous->active_extents.clear();
    index_active_extents(module);
    modules_.push_back(std::move(module));
    increment(metrics_.unloads);
    increment(metrics_.loads);
    increment(metrics_.replacements);
}

void ExecutableModuleCatalog::update_relocations(
    const std::string_view id,
    std::vector<ExecutableModuleRelocation> relocations,
    RuntimeBlockTable& blocks,
    ExecutableCodeTracker& tracker) {
    auto module = std::find_if(modules_.begin(), modules_.end(), [&](const auto& candidate) {
        return candidate.active && candidate.id == id;
    });
    if (module == modules_.end()) throw std::invalid_argument("Unbekanntes aktives Modul.");
    if (module->relocation_generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "Ausfuehrbare Modulrelocation hat ihre Generation ausgeschoepft.");
    }
    validate_relocations(*module, relocations);
    accumulate(metrics_.invalidated_blocks,
               invalidate_active_module_extents(*module, blocks, tracker));
    module->relocations = std::move(relocations);
    ++module->relocation_generation;
}

const ExecutableModule* ExecutableModuleCatalog::resolve(const std::uint32_t address,
                                                         const std::size_t width) const noexcept {
    const auto found = std::find_if(modules_.begin(), modules_.end(), [&](const auto& module) {
        return module.contains(address, width);
    });
    return found == modules_.end() ? nullptr : &*found;
}

const ExecutableModule* ExecutableModuleCatalog::find(const std::string_view id) const noexcept {
    const auto found = std::find_if(modules_.begin(), modules_.end(), [&](const auto& module) {
        return module.active && module.id == id;
    });
    return found == modules_.end() ? nullptr : &*found;
}

bool ExecutableModuleCatalog::authorize_control_transfer(const std::uint32_t address,
                                                         const std::uint32_t maximum_bytes) {
    if ((address & 1u) != 0u || maximum_bytes < 2u) return false;
    auto found = std::find_if(modules_.begin(), modules_.end(), [&](const auto& module) {
        return module.contains(address, 2u);
    });
    if (found == modules_.end()) return false;
    if (!found->active) return false;
    if (!found->control_transfer_promotion_allowed)
        return found->executable_permission && found->materializable(address, 2u);
    const auto begin = address >= found->guest_start &&
                               static_cast<std::uint64_t>(address) < found->end_address()
                           ? address - found->guest_start
                           : canonical_physical_address(address) -
                                 canonical_physical_address(found->guest_start);
    const auto end = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        found->bytes.size(), static_cast<std::uint64_t>(begin) + maximum_bytes));
    std::vector<std::pair<std::uint32_t, std::uint32_t>> executable{{begin, end}};
    for (const auto& range : found->range_roles) {
        if (range.role == ExecutableStorageRole::RuntimeMaterializable && range.size != 0u)
            executable.emplace_back(range.offset, range.offset + range.size);
    }
    std::sort(executable.begin(), executable.end());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
    for (const auto interval : executable) {
        if (merged.empty() || interval.first > merged.back().second)
            merged.push_back(interval);
        else
            merged.back().second = std::max(merged.back().second, interval.second);
    }
    std::vector<ExecutableModuleRangeRole> committed_roles;
    committed_roles.reserve(merged.size() * 2u + 1u);
    std::uint32_t cursor = 0u;
    for (const auto interval : merged) {
        if (cursor < interval.first)
            committed_roles.push_back(
                {cursor, interval.first - cursor, ExecutableStorageRole::ProvenData});
        committed_roles.push_back({interval.first,
                                   interval.second - interval.first,
                                   ExecutableStorageRole::RuntimeMaterializable});
        cursor = interval.second;
    }
    if (cursor < found->bytes.size())
        committed_roles.push_back({cursor,
                                   static_cast<std::uint32_t>(found->bytes.size() - cursor),
                                   ExecutableStorageRole::ProvenData});
    found->range_roles.swap(committed_roles);
    found->executable_permission = true;
    return true;
}

void ExecutableModuleCatalog::record_runtime_write(const std::uint32_t address,
                                                   const std::size_t size,
                                                   const CodeWriteSource source,
                                                   const bool bytes_changed) {
    if (size == 0u ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u - address)
        return;
    const auto establishes_provenance =
        source == CodeWriteSource::Cpu || source == CodeWriteSource::Fpu ||
        source == CodeWriteSource::StoreQueue || source == CodeWriteSource::Fallback;
    if (!bytes_changed && !establishes_provenance) return;
    const auto physical_begin = canonical_physical_address(address);
    const auto physical_end = static_cast<std::uint64_t>(physical_begin) + size;
    if (physical_end > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u)
        return;

    if (bytes_changed) {
        const auto scan_modules = !canonical_range_is_linear(address, size) ||
                                  active_extent_index_may_overlap(physical_begin, physical_end);
        if (scan_modules) {
            increment(metrics_.write_index_scans);
            for (auto& module : modules_) {
                if (!physical_range_overlaps_active_extents(module, physical_begin, physical_end))
                    continue;
                const auto was_active = module.active;
                const auto previous_extents = module.active_extents;
                const auto changed = subtract_physical_range(module, physical_begin, physical_end);
                if (changed) {
                    unindex_active_extents(module.guest_start, previous_extents);
                    if (module.active) index_active_extents(module);
                }
                if (changed && was_active && !module.active) increment(metrics_.unloads);
            }
        } else {
            increment(metrics_.write_index_rejections);
        }
    }

    const auto update_provenance = [&](const bool set) {
        std::uint64_t cursor = physical_begin;
        while (cursor < physical_end) {
            const auto page = static_cast<std::uint32_t>(cursor) & ~(runtime_write_page_size - 1u);
            const auto page_end = std::min<std::uint64_t>(
                physical_end, static_cast<std::uint64_t>(page) + runtime_write_page_size);
            auto found = runtime_write_pages_.find(page);
            if (set && found == runtime_write_pages_.end())
                found = runtime_write_pages_.emplace(page, RuntimeWritePage{}).first;
            if (found != runtime_write_pages_.end()) {
                auto offset = static_cast<std::uint32_t>(cursor - page);
                const auto end_offset = static_cast<std::uint32_t>(page_end - page);
                while (offset < end_offset) {
                    const auto word = offset / 64u;
                    const auto first_bit = offset % 64u;
                    const auto word_end = std::min<std::uint32_t>(end_offset, (word + 1u) * 64u);
                    const auto last_bit = word_end - word * 64u;
                    auto mask = std::numeric_limits<std::uint64_t>::max() << first_bit;
                    if (last_bit < 64u) mask &= (std::uint64_t{1u} << last_bit) - 1u;
                    if (set)
                        found->second.written[word] |= mask;
                    else
                        found->second.written[word] &= ~mask;
                    offset = word_end;
                }
                if (!set &&
                    std::all_of(found->second.written.begin(),
                                found->second.written.end(),
                                [](const auto word) { return word == 0u; }))
                    runtime_write_pages_.erase(found);
            }
            cursor = page_end;
        }
    };
    if (bytes_changed) update_provenance(false);
    if (establishes_provenance) update_provenance(true);
}

bool ExecutableModuleCatalog::promote_runtime_write(const Memory& memory,
                                                    const std::uint32_t address,
                                                    const std::uint32_t maximum_bytes) {
    if ((address & 1u) != 0u || maximum_bytes < 2u) return false;
    const auto was_written = [this](const std::uint32_t candidate) {
        const auto physical = canonical_physical_address(candidate);
        const auto page = physical & ~(runtime_write_page_size - 1u);
        const auto found = runtime_write_pages_.find(page);
        if (found == runtime_write_pages_.end()) return false;
        const auto offset = physical - page;
        return (found->second.written[offset / 64u] & (std::uint64_t{1u} << (offset % 64u))) != 0u;
    };
    if (!was_written(address) || !was_written(address + 1u)) return false;

    const auto physical_address = canonical_physical_address(address);
    const auto resolved = resolve(address, 2u);
    if (resolved != nullptr) {
        auto found = std::find_if(modules_.begin(), modules_.end(), [&](const auto& module) {
            return &module == resolved;
        });
        if (found == modules_.end() ||
            !found->id.starts_with(runtime_write_module_id_prefix) ||
            found->source_identity != runtime_write_module_source_identity ||
            found->kind != ExecutableModuleKind::Overlay ||
            !found->control_transfer_promotion_allowed || !found->relocations.empty())
            return false;

        const auto offset = module_offset(*found, address, 2u);
        if (!offset) return false;
        const auto tail = std::find_if(found->active_extents.begin(),
                                       found->active_extents.end(),
                                       [&](const auto extent) {
                                           return *offset >= extent.offset &&
                                                  *offset + 2u <=
                                                      static_cast<std::uint64_t>(extent.offset) +
                                                          extent.size &&
                                                  static_cast<std::uint64_t>(extent.offset) +
                                                          extent.size ==
                                                      found->bytes.size();
                                       });
        if (tail == found->active_extents.end()) return false;

        const auto module_physical = canonical_physical_address(found->guest_start);
        const auto current_end =
            static_cast<std::uint64_t>(module_physical) + found->bytes.size();
        auto extension_end = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(physical_address) + maximum_bytes,
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u);
        for (const auto& other : modules_) {
            if (!other.active || &other == &*found) continue;
            const auto other_base = canonical_physical_address(other.guest_start);
            for (const auto extent : other.active_extents) {
                const auto extent_begin =
                    static_cast<std::uint64_t>(other_base) + extent.offset;
                const auto extent_end = extent_begin + extent.size;
                if (extent_end <= current_end || extent_begin >= extension_end) continue;
                extension_end = extent_begin <= current_end ? current_end : extent_begin;
            }
        }
        if (extension_end <= current_end) return false;

        std::vector<std::uint8_t> extension;
        while (current_end + extension.size() < extension_end) {
            const auto logical =
                static_cast<std::uint64_t>(found->guest_start) + found->bytes.size() +
                extension.size();
            if (logical > std::numeric_limits<std::uint32_t>::max()) break;
            const auto candidate = static_cast<std::uint32_t>(logical);
            if (canonical_physical_address(candidate) != current_end + extension.size() ||
                !memory.contains(candidate, 1u) || !was_written(candidate))
                break;
            extension.push_back(memory.read_u8(candidate));
        }
        if ((extension.size() & 1u) != 0u) extension.pop_back();
        if (extension.empty()) return false;

        auto expanded = *found;
        const auto previous_size = static_cast<std::uint32_t>(expanded.bytes.size());
        expanded.bytes.insert(expanded.bytes.end(), extension.begin(), extension.end());
        expanded.active_extents.back().size += static_cast<std::uint32_t>(extension.size());
        expanded.range_roles.push_back({previous_size,
                                        static_cast<std::uint32_t>(extension.size()),
                                        ExecutableStorageRole::ProvenData});
        validate_and_normalize_module(expanded);
        for (const auto& other : modules_) {
            if (other.active && &other != &*found && physical_extents_overlap(expanded, other))
                throw std::logic_error(
                    "Runtimewrite-Erweiterung kollidiert nach Vorvalidierung.");
        }
        reserve_active_extent_index(expanded);
        unindex_active_extents(*found);
        *found = std::move(expanded);
        index_active_extents(*found);
        return true;
    }

    auto snapshot_limit = maximum_bytes;
    for (const auto& existing : modules_) {
        if (!existing.active) continue;
        const auto existing_base = canonical_physical_address(existing.guest_start);
        for (const auto extent : existing.active_extents) {
            const auto extent_start = static_cast<std::uint64_t>(existing_base) + extent.offset;
            if (extent_start <= physical_address) continue;
            const auto distance = extent_start - physical_address;
            snapshot_limit = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(snapshot_limit, distance));
        }
    }

    std::uint32_t snapshot_size = 0u;
    while (snapshot_size < snapshot_limit && snapshot_size <=
                                                    std::numeric_limits<std::uint32_t>::max() -
                                                        address &&
           memory.contains(address + snapshot_size, 1u) &&
           was_written(address + snapshot_size))
        ++snapshot_size;
    snapshot_size &= ~1u;
    if (snapshot_size < 2u) return false;

    ExecutableModule module;
    module.id = std::string(runtime_write_module_id_prefix) +
                std::to_string(next_runtime_write_module_++);
    module.source_identity = runtime_write_module_source_identity;
    module.guest_start = address;
    module.bytes.resize(snapshot_size);
    for (std::uint32_t offset = 0u; offset < snapshot_size; ++offset)
        module.bytes[offset] = memory.read_u8(address + offset);
    module.kind = ExecutableModuleKind::Overlay;
    module.executable_permission = false;
    module.control_transfer_promotion_allowed = true;
    module.range_roles.push_back({0u, snapshot_size, ExecutableStorageRole::ProvenData});
    publish(std::move(module));
    return true;
}

bool ExecutableModuleCatalog::validate_bytes(const Memory& memory,
                                             const std::uint32_t address,
                                             const std::size_t width) const {
    return validate_bytes_at(memory, address, address, width);
}

bool ExecutableModuleCatalog::validate_bytes_at(const Memory& memory,
                                                const std::uint32_t module_address,
                                                const std::uint32_t memory_address,
                                                const std::size_t width) const {
    const auto* module = resolve(module_address, width);
    if (module == nullptr || !memory.contains(memory_address, width)) return false;
    const auto offset = module_address >= module->guest_start &&
                                static_cast<std::uint64_t>(module_address) < module->end_address()
                            ? static_cast<std::size_t>(module_address - module->guest_start)
                            : static_cast<std::size_t>(
                                  canonical_physical_address(module_address) -
                                  canonical_physical_address(module->guest_start));
    for (std::size_t current = 0u; current < width; ++current)
        if (memory.read_u8(memory_address + static_cast<std::uint32_t>(current)) !=
            relocated_module_byte(*module, offset + current))
            return false;
    return true;
}

const ExecutableModuleMetrics& ExecutableModuleCatalog::metrics() const noexcept {
    return metrics_;
}

ExecutableModuleCatalogSnapshot ExecutableModuleCatalog::snapshot() const {
    ExecutableModuleCatalogSnapshot result;
    result.modules = modules_;
    result.runtime_write_pages.reserve(runtime_write_pages_.size());
    for (const auto& [physical_page, page] : runtime_write_pages_)
        result.runtime_write_pages.push_back({physical_page, page.written});
    result.active_extent_page_refcounts.reserve(active_extent_page_refcounts_.size());
    for (const auto& [physical_page, refcount] : active_extent_page_refcounts_)
        result.active_extent_page_refcounts.push_back({physical_page, refcount});
    result.next_runtime_write_module = next_runtime_write_module_;
    result.metrics = metrics_;
    return result;
}

DemandBlockMaterializer::DemandBlockMaterializer(ExecutableModuleCatalog& modules,
                                                 RuntimeBlockTable& blocks,
                                                 ExecutableCodeTracker* tracker,
                                                 BlockMaterializationPolicy policy,
                                                 BlockMaterializeCallback callback)
    : modules_(modules), blocks_(blocks), tracker_(tracker), policy_(policy),
      callback_(std::move(callback)) {
    if (policy_.enabled &&
        (!callback_ || policy_.max_blocks == 0u || policy_.max_bytes == 0u ||
         policy_.max_guest_cycles == 0u || policy_.max_instructions == 0u ||
         policy_.max_recursive_seeds == 0u ||
         (!policy_.deterministic_no_host_time && policy_.max_analysis_time_ms == 0u) ||
         policy_.max_memory_bytes == 0u || policy_.max_materializations_per_run == 0u ||
         policy_.max_repeated_misses_per_target == 0u))
        throw std::invalid_argument("Aktive Blockmaterialisierung braucht Callback und Budgets.");
}

void DemandBlockMaterializer::fail(const MaterializationFailure failure,
                                   const std::uint32_t target) noexcept {
    last_failure_ = failure;
    increment(metrics_.misses);
    increment(misses_by_target_[target]);
    if (metrics_.first_failure == MaterializationFailure::None) {
        metrics_.first_failure = failure;
        metrics_.first_failure_target = target;
    }
    if (failure == MaterializationFailure::BudgetExhausted ||
        failure == MaterializationFailure::RepeatedMissLimit)
        increment(metrics_.budget_failures);
    if (failure == MaterializationFailure::GenerationMismatch ||
        failure == MaterializationFailure::ModuleUnloaded ||
        failure == MaterializationFailure::RelocationMismatch)
        increment(metrics_.generation_revalidation_failures);
    if (failure == MaterializationFailure::ByteIdentityMismatch ||
        failure == MaterializationFailure::AotTemplateMismatch)
        increment(metrics_.byte_identity_failures);
    events_.push_back({next_event_sequence_++, target, failure, false});
}

void DemandBlockMaterializer::record_success(const std::uint32_t target) noexcept {
    last_failure_ = MaterializationFailure::None;
    misses_by_target_.erase(target);
    events_.push_back({next_event_sequence_++, target, MaterializationFailure::None, true});
}

std::optional<RuntimeBlockHandle>
DemandBlockMaterializer::try_materialize(CpuState& cpu,
                                         const std::uint32_t target,
                                         const std::uint32_t physical_origin,
                                         const BlockVariantKey& variant,
                                         const std::uint32_t callsite) {
    reconcile_inactive_origins();
    increment(metrics_.requests);
    if (const auto existing = blocks_.lookup(target, variant)) {
        increment(metrics_.cache_hits);
        return existing;
    }
    if (!policy_.enabled) {
        fail(MaterializationFailure::Disabled, target);
        return std::nullopt;
    }
    if ((target & 1u) != 0u) {
        fail(MaterializationFailure::Misaligned, target);
        return std::nullopt;
    }
    const auto containing_origin =
        std::find_if(origins_.begin(), origins_.end(), [&](const auto& origin) {
            if (origin.aot_template) return false;
            const auto begin = static_cast<std::uint64_t>(origin.address);
            const auto end = begin + origin.block_size;
            const auto requested = static_cast<std::uint64_t>(target);
            if (requested < begin || requested + 2u > end ||
                !blocks_.active(origin.handle))
                return false;
            const auto registered = blocks_.resolve(origin.handle);
            return registered && registered->get().variant == variant;
        });
    if (containing_origin != origins_.end()) {
        if (!containing_origin->interpreter_backed) {
            fail(MaterializationFailure::InvalidBlock, target);
            return std::nullopt;
        }
        if (!validate_for_dispatch(
                cpu, containing_origin->handle, target, physical_origin))
            return std::nullopt;
        increment(metrics_.cache_hits);
        last_failure_ = MaterializationFailure::None;
        return containing_origin->handle;
    }
    if (misses_by_target_[target] >= policy_.max_repeated_misses_per_target) {
        fail(MaterializationFailure::RepeatedMissLimit, target);
        return std::nullopt;
    }
    const auto physical_target = canonical_physical_address(physical_origin);
    if (!cpu.memory.contains(physical_target, 2u)) {
        fail(MaterializationFailure::Uncommitted, target);
        return std::nullopt;
    }
    auto* module = modules_.resolve(physical_target, 2u);
    if (module == nullptr) {
        if (!modules_.promote_runtime_write(cpu.memory, physical_target)) {
            fail(MaterializationFailure::UnknownSource, target);
            return std::nullopt;
        }
        module = modules_.resolve(physical_target, 2u);
    } else if (module->id.starts_with(runtime_write_module_id_prefix) &&
               module->source_identity == runtime_write_module_source_identity) {
        static_cast<void>(modules_.promote_runtime_write(cpu.memory, physical_target));
        module = modules_.resolve(physical_target, 2u);
    }
    if (module->control_transfer_promotion_allowed) {
        if (!modules_.authorize_control_transfer(physical_target)) {
            fail(MaterializationFailure::ProvenNonCode, target);
            return std::nullopt;
        }
    } else if (!module->executable_permission) {
        fail(MaterializationFailure::PermissionDenied, target);
        return std::nullopt;
    } else if (!module->materializable(physical_target, 2u)) {
        fail(MaterializationFailure::ProvenNonCode, target);
        return std::nullopt;
    }
    module = modules_.resolve(physical_target, 2u);
    if (metrics_.materializations >= policy_.max_blocks ||
        metrics_.materializations >= policy_.max_materializations_per_run ||
        metrics_.materialized_bytes >= policy_.max_bytes) {
        fail(MaterializationFailure::BudgetExhausted, target);
        return std::nullopt;
    }
    const auto module_id = module->id;
    const auto source_identity = module->source_identity;
    const auto module_generation = module->generation;
    const auto relocation_generation = module->relocation_generation;
    const auto snapshot_size = std::min<std::size_t>(
        module->active_extent_remaining(physical_target),
        static_cast<std::size_t>(policy_.max_bytes - metrics_.materialized_bytes));
    if (snapshot_size < 2u || snapshot_size > policy_.max_memory_bytes) {
        fail(MaterializationFailure::BudgetExhausted, target);
        return std::nullopt;
    }
    std::vector<std::uint8_t> snapshot(snapshot_size);
    for (std::size_t current = 0u; current < snapshot.size(); ++current)
        snapshot[current] =
            cpu.memory.read_u8(physical_target + static_cast<std::uint32_t>(current));
    if (!modules_.validate_bytes_at(
            cpu.memory, physical_target, physical_target, snapshot.size())) {
        fail(MaterializationFailure::ByteIdentityMismatch, target);
        return std::nullopt;
    }
    std::optional<std::chrono::steady_clock::time_point> started;
    if (!policy_.deterministic_no_host_time) {
        started = std::chrono::steady_clock::now();
    }
    auto candidate = callback_(target, physical_target, snapshot, variant);
    std::uint64_t elapsed_ms = 0u;
    if (started) {
        elapsed_ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - *started)
                                           .count());
    }
    if (!candidate.decode_candidate_validated) {
        fail(candidate.rejection_failure == MaterializationFailure::None
                 ? MaterializationFailure::DecodeRejected
                 : candidate.rejection_failure,
             target);
        return std::nullopt;
    }
    if (!candidate.interpreter_backed && !candidate.bounded_analysis_complete) {
        fail(MaterializationFailure::AnalysisIncomplete, target);
        return std::nullopt;
    }
    if (!candidate.interpreter_backed && !candidate.ir_verified) {
        fail(MaterializationFailure::IrVerificationFailed, target);
        return std::nullopt;
    }
    if (!candidate.interpreter_backed && !candidate.code_generated) {
        fail(MaterializationFailure::CodeGenerationFailed, target);
        return std::nullopt;
    }
    if (candidate.guest_cycles > policy_.max_guest_cycles ||
        candidate.instructions > policy_.max_instructions ||
        candidate.recursive_seeds > policy_.max_recursive_seeds ||
        (!policy_.deterministic_no_host_time &&
         std::max(elapsed_ms, candidate.analysis_time_ms) > policy_.max_analysis_time_ms) ||
        candidate.peak_memory_bytes > policy_.max_memory_bytes) {
        fail(MaterializationFailure::BudgetExhausted, target);
        return std::nullopt;
    }
    const auto interpreter_backed = candidate.interpreter_backed;
    auto block = std::move(candidate.block);
    if (block.virtual_start != target || block.size < 2u || block.variant != variant ||
        block.size > snapshot.size() || block.function == nullptr || block.provenance.empty() ||
        block.size > policy_.max_bytes - metrics_.materialized_bytes) {
        fail(MaterializationFailure::InvalidBlock, target);
        return std::nullopt;
    }
    const auto* current_module = modules_.find(module_id);
    if (current_module == nullptr) {
        fail(MaterializationFailure::ModuleUnloaded, target);
        return std::nullopt;
    }
    if (current_module->generation != module_generation) {
        fail(MaterializationFailure::GenerationMismatch, target);
        return std::nullopt;
    }
    if (current_module->relocation_generation != relocation_generation) {
        fail(MaterializationFailure::RelocationMismatch, target);
        return std::nullopt;
    }
    if (!current_module->materializable(physical_target, block.size)) {
        fail(MaterializationFailure::ProvenNonCode, target);
        return std::nullopt;
    }
    for (std::size_t current = 0u; current < block.size; ++current) {
        if (cpu.memory.read_u8(physical_target + static_cast<std::uint32_t>(current)) !=
            snapshot[current]) {
            fail(MaterializationFailure::ByteIdentityMismatch, target);
            return std::nullopt;
        }
    }
    if (!modules_.validate_bytes_at(
            cpu.memory, physical_target, physical_target, block.size)) {
        fail(MaterializationFailure::ByteIdentityMismatch, target);
        return std::nullopt;
    }
    block.physical_origin = physical_target;

    auto validation_physical_address = physical_target;
    auto validation_size = block.size;
    auto validation_module_address = physical_target;
    auto validation_block_module_address = physical_target;
    auto validation_module_id = module_id;
    auto validation_source_identity = source_identity;
    auto validation_module_generation = module_generation;
    auto validation_relocation_generation = relocation_generation;
    auto validation_snapshot =
        std::vector<std::uint8_t>(snapshot.begin(), snapshot.begin() + block.size);
    const auto aot_template = block.aot_template.has_value();
    if (aot_template) {
        if (interpreter_backed) {
            fail(MaterializationFailure::InvalidBlock, target);
            return std::nullopt;
        }
        const auto& contract = *block.aot_template;
        try {
            validate_code_address_mapping(contract.mapping);
        } catch (const std::exception&) {
            fail(MaterializationFailure::InvalidBlock, target);
            return std::nullopt;
        }
        const auto mapping_end =
            static_cast<std::uint64_t>(contract.mapping.runtime_start) + contract.mapping.extent;
        const auto block_end = static_cast<std::uint64_t>(block.virtual_start) + block.size;
        if (contract.validation_extent < contract.mapping.extent ||
            block.virtual_start < contract.mapping.runtime_start || block_end > mapping_end) {
            fail(MaterializationFailure::InvalidBlock, target);
            return std::nullopt;
        }
        const auto block_offset = block.virtual_start - contract.mapping.runtime_start;
        if (block_offset > physical_target ||
            static_cast<std::uint64_t>(contract.mapping.source_start) +
                    contract.validation_extent >
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u ||
            static_cast<std::uint64_t>(contract.mapping.runtime_start) +
                    contract.validation_extent >
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
            fail(MaterializationFailure::InvalidBlock, target);
            return std::nullopt;
        }
        if (contract.validation_extent > policy_.max_memory_bytes) {
            fail(MaterializationFailure::BudgetExhausted, target);
            return std::nullopt;
        }
        validation_physical_address = physical_target - block_offset;
        validation_size = contract.validation_extent;
        validation_module_address = contract.mapping.source_start;
        validation_block_module_address = contract.mapping.source_start + block_offset;
        const auto validation_last_offset = validation_size - 1u;
        if (canonical_physical_address(block.virtual_start) == physical_target &&
            canonical_physical_address(contract.mapping.runtime_start +
                                       validation_last_offset) !=
                validation_physical_address + validation_last_offset) {
            fail(MaterializationFailure::InvalidBlock, target);
            return std::nullopt;
        }
        if (static_cast<std::uint64_t>(validation_physical_address) + validation_size >
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u ||
            !cpu.memory.contains(validation_physical_address, validation_size)) {
            fail(MaterializationFailure::Uncommitted, target);
            return std::nullopt;
        }
        // The binder may explicitly allow patched pointer/literal words inside
        // the template.  Such words are removed from the module's active
        // extents, so resolve only the proven native entry here; the full live
        // range is protected by the snapshot below.
        const auto* source_module =
            modules_.resolve(validation_block_module_address, block.size);
        if (source_module == nullptr ||
            !source_module->materializable(validation_block_module_address, block.size)) {
            fail(MaterializationFailure::AotTemplateMismatch, target);
            return std::nullopt;
        }
        validation_module_id = source_module->id;
        validation_source_identity = source_module->source_identity;
        validation_module_generation = source_module->generation;
        validation_relocation_generation = source_module->relocation_generation;
        validation_snapshot.resize(validation_size);
        for (std::uint32_t current = 0u; current < validation_size; ++current) {
            validation_snapshot[current] =
                cpu.memory.read_u8(validation_physical_address + current);
        }
    }

    block.provenance = "runtime-module:" + validation_module_id + ":g" +
                       std::to_string(validation_module_generation) + ":r" +
                       std::to_string(validation_relocation_generation) + ":" + block.provenance;
    const auto identity = stable_runtime_block_identity(block);
    const auto block_size = block.size;
    const auto provenance = block.provenance;
    std::shared_ptr<const std::vector<std::uint8_t>> validation_proof;
    for (const auto& origin : origins_) {
        if (origin.physical_address == validation_physical_address &&
            origin.size == validation_size &&
            origin.module_address == validation_module_address &&
            origin.module_id == validation_module_id &&
            origin.source_identity == validation_source_identity &&
            origin.module_generation == validation_module_generation &&
            origin.relocation_generation == validation_relocation_generation &&
            origin.aot_template == aot_template && origin.snapshot &&
            *origin.snapshot == validation_snapshot) {
            validation_proof = origin.snapshot;
            break;
        }
    }
    const auto retains_new_proof = !validation_proof;
    if (retains_new_proof) {
        if (metrics_.retained_validation_bytes >= policy_.max_memory_bytes ||
            validation_snapshot.size() >
                policy_.max_memory_bytes - metrics_.retained_validation_bytes) {
            fail(MaterializationFailure::BudgetExhausted, target);
            return std::nullopt;
        }
        validation_proof =
            std::make_shared<const std::vector<std::uint8_t>>(std::move(validation_snapshot));
    }
    MaterializedOrigin pending_origin{
        target,
        validation_physical_address,
        validation_size,
        validation_module_address,
        validation_block_module_address,
        block_size,
        callsite,
        identity,
        validation_module_id,
        validation_source_identity,
        validation_module_generation,
        validation_relocation_generation,
        {},
        std::move(validation_proof),
        interpreter_backed,
        aot_template};
    origins_.reserve(origins_.size() + 1u);
    const auto handle = blocks_.register_runtime(std::move(block));
    pending_origin.handle = handle;
    try {
        origins_.push_back(std::move(pending_origin));
    } catch (...) {
        static_cast<void>(blocks_.erase_identity(identity));
        throw;
    }
    if (!validate_for_dispatch(cpu, handle, target, physical_target)) {
        origins_.pop_back();
        static_cast<void>(blocks_.erase_identity(identity));
        return std::nullopt;
    }
    try {
        if (tracker_ != nullptr) {
            static_cast<void>(tracker_->register_block({identity,
                                                        validation_physical_address,
                                                        validation_size,
                                                        provenance,
                                                        {},
                                                        aot_template
                                                            ? ExecutableBlockOrigin::RomRamCopy
                                                            : ExecutableBlockOrigin::RuntimeWrite}));
        }
    } catch (...) {
        origins_.pop_back();
        static_cast<void>(blocks_.erase_identity(identity));
        throw;
    }
    if (retains_new_proof) {
        accumulate(metrics_.retained_validation_bytes, validation_size);
        metrics_.peak_retained_validation_bytes =
            std::max(metrics_.peak_retained_validation_bytes,
                     metrics_.retained_validation_bytes);
    }
    increment(metrics_.materializations);
    if (interpreter_backed) increment(metrics_.interpreter_materializations);
    const auto materialized_size = block_size;
    metrics_.materialized_bytes += materialized_size;
    record_success(target);
    return handle;
}

const BlockMaterializationMetrics& DemandBlockMaterializer::metrics() const noexcept {
    return metrics_;
}

const std::vector<BlockMaterializationEvent>& DemandBlockMaterializer::events() const noexcept {
    return events_;
}

MaterializationFailure DemandBlockMaterializer::last_failure() const noexcept {
    return last_failure_;
}

bool DemandBlockMaterializer::validate_for_dispatch(const CpuState& cpu,
                                                    const RuntimeBlockHandle handle,
                                                    const std::uint32_t target,
                                                    const std::optional<std::uint32_t>
                                                        physical_entry) noexcept {
    const auto origin = std::find_if(origins_.begin(), origins_.end(), [&](const auto& candidate) {
        const auto begin = static_cast<std::uint64_t>(candidate.address);
        const auto end = begin + candidate.block_size;
        const auto requested = static_cast<std::uint64_t>(target);
        if (candidate.handle != handle) return false;
        if (!physical_entry)
            return candidate.address == target ||
                   (candidate.interpreter_backed && requested >= begin &&
                    requested + 2u <= end);
        if (candidate.block_module_address < candidate.module_address) return false;
        const auto block_offset =
            candidate.block_module_address - candidate.module_address;
        const auto expected_entry =
            static_cast<std::uint64_t>(candidate.physical_address) + block_offset;
        const auto translated_entry =
            static_cast<std::uint64_t>(canonical_physical_address(*physical_entry));
        return candidate.interpreter_backed
                   ? translated_entry >= expected_entry &&
                         translated_entry + 2u <= expected_entry + candidate.block_size
                   : translated_entry == expected_entry;
    });
    if (origin == origins_.end()) {
        increment(metrics_.dispatch_validation_failures);
        fail(MaterializationFailure::StaleHandle, target);
        return false;
    }
    if (!origin->snapshot || origin->snapshot->size() != origin->size) {
        increment(metrics_.dispatch_validation_failures);
        fail(MaterializationFailure::StaleHandle, target);
        return false;
    }
    const auto* module = modules_.find(origin->module_id);
    if (module == nullptr) {
        increment(metrics_.dispatch_validation_failures);
        fail(MaterializationFailure::ModuleUnloaded, target);
        return false;
    }
    if (module->source_identity != origin->source_identity ||
        module->generation != origin->module_generation) {
        increment(metrics_.dispatch_validation_failures);
        fail(MaterializationFailure::GenerationMismatch, target);
        return false;
    }
    if (module->relocation_generation != origin->relocation_generation) {
        increment(metrics_.dispatch_validation_failures);
        fail(MaterializationFailure::RelocationMismatch, target);
        return false;
    }
    if (!module->materializable(origin->block_module_address, origin->block_size) ||
        !cpu.memory.contains(origin->physical_address, origin->size)) {
        increment(metrics_.dispatch_validation_failures);
        fail(MaterializationFailure::ProvenNonCode, target);
        return false;
    }
    for (std::size_t current = 0u; current < origin->snapshot->size(); ++current) {
        if (cpu.memory.read_u8(origin->physical_address + static_cast<std::uint32_t>(current)) !=
            (*origin->snapshot)[current]) {
            increment(metrics_.dispatch_validation_failures);
            fail(origin->aot_template ? MaterializationFailure::AotTemplateMismatch
                                      : MaterializationFailure::ByteIdentityMismatch,
                 target);
            return false;
        }
    }
    if (!origin->aot_template &&
        !modules_.validate_bytes_at(cpu.memory,
                                    origin->module_address,
                                    origin->physical_address,
                                    origin->size)) {
        increment(metrics_.dispatch_validation_failures);
        fail(origin->aot_template ? MaterializationFailure::AotTemplateMismatch
                                  : MaterializationFailure::ByteIdentityMismatch,
             target);
        return false;
    }
    return true;
}

void DemandBlockMaterializer::reconcile_inactive_origins(
    IndirectDispatchMetrics* const dispatch_metrics) {
    const auto release_proof = [&](const MaterializedOrigin& origin) noexcept {
        if (!origin.snapshot || origin.snapshot.use_count() != 1u) return;
        const auto released = static_cast<std::uint64_t>(origin.snapshot->size());
        metrics_.retained_validation_bytes =
            released >= metrics_.retained_validation_bytes
                ? 0u
                : metrics_.retained_validation_bytes - released;
        accumulate(metrics_.reclaimed_validation_bytes, released);
    };
    for (auto origin = origins_.begin(); origin != origins_.end();) {
        const auto* module = modules_.find(origin->module_id);
        const auto dependency_active =
            module != nullptr && module->source_identity == origin->source_identity &&
            module->generation == origin->module_generation &&
            module->relocation_generation == origin->relocation_generation &&
            module->materializable(origin->block_module_address, origin->block_size);
        if (blocks_.active(origin->handle) && dependency_active) {
            ++origin;
            continue;
        }
        static_cast<void>(blocks_.erase_identity(origin->block_identity));
        if (tracker_ != nullptr)
            static_cast<void>(tracker_->retire_block(origin->block_identity));
        if (dispatch_metrics != nullptr)
            dispatch_metrics->record_invalidation(origin->callsite);
        release_proof(*origin);
        origin = origins_.erase(origin);
    }
}

void DemandBlockMaterializer::record_invalidation(const std::uint32_t address,
                                                  const std::size_t size,
                                                  IndirectDispatchMetrics& dispatch_metrics) {
    constexpr auto address_space_end =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    if (size == 0u) return;
    const auto logical_begin = static_cast<std::uint64_t>(address);
    const auto logical_end =
        size >= address_space_end - logical_begin ? address_space_end : logical_begin + size;
    const auto overlaps_invalidated_range = [&](const MaterializedOrigin& origin) noexcept {
        const auto origin_begin = static_cast<std::uint64_t>(origin.physical_address);
        const auto origin_end = origin_begin + origin.size;
        auto logical = static_cast<std::uint64_t>(address);
        while (logical < logical_end) {
            const auto logical_address = static_cast<std::uint32_t>(logical);
            const auto physical_begin =
                static_cast<std::uint64_t>(canonical_physical_address(logical_address));
            auto next_boundary = address_space_end;
            if (logical < 0xE0000000ull) {
                next_boundary =
                    ((logical / 0x20000000ull) + 1u) * 0x20000000ull;
                next_boundary = std::min<std::uint64_t>(next_boundary, 0xE0000000ull);
            }
            const auto chunk_end = std::min(logical_end, next_boundary);
            const auto physical_end = physical_begin + (chunk_end - logical);
            if (physical_begin < origin_end && origin_begin < physical_end) return true;
            logical = chunk_end;
        }
        return false;
    };
    const auto release_proof = [&](const MaterializedOrigin& origin) noexcept {
        if (!origin.snapshot || origin.snapshot.use_count() != 1u) return;
        const auto released = static_cast<std::uint64_t>(origin.snapshot->size());
        metrics_.retained_validation_bytes =
            released >= metrics_.retained_validation_bytes
                ? 0u
                : metrics_.retained_validation_bytes - released;
        accumulate(metrics_.reclaimed_validation_bytes, released);
    };
    for (auto origin = origins_.begin(); origin != origins_.end();) {
        if (!overlaps_invalidated_range(*origin)) {
            ++origin;
            continue;
        }
        static_cast<void>(blocks_.erase_identity(origin->block_identity));
        if (tracker_ != nullptr)
            static_cast<void>(tracker_->retire_block(origin->block_identity));
        dispatch_metrics.record_invalidation(origin->callsite);
        release_proof(*origin);
        origin = origins_.erase(origin);
    }
}

const char* executable_module_kind_name(const ExecutableModuleKind value) noexcept {
    return value == ExecutableModuleKind::Overlay ? "overlay" : "module";
}

const char* executable_storage_role_name(const ExecutableStorageRole value) noexcept {
    switch (value) {
    case ExecutableStorageRole::RuntimeMaterializable:
        return "runtime-materializable";
    case ExecutableStorageRole::ProvenData:
        return "proven-data";
    case ExecutableStorageRole::ProvenPadding:
        return "proven-padding";
    }
    return "runtime-materializable";
}

const char* materialization_failure_name(const MaterializationFailure value) noexcept {
    switch (value) {
    case MaterializationFailure::None:
        return "none";
    case MaterializationFailure::Disabled:
        return "disabled";
    case MaterializationFailure::Misaligned:
        return "misaligned";
    case MaterializationFailure::Uncommitted:
        return "uncommitted";
    case MaterializationFailure::PermissionDenied:
        return "permission-denied";
    case MaterializationFailure::ProvenNonCode:
        return "proven-non-code";
    case MaterializationFailure::UnknownSource:
        return "unknown-source";
    case MaterializationFailure::BudgetExhausted:
        return "budget-exhausted";
    case MaterializationFailure::RepeatedMissLimit:
        return "repeated-miss-limit";
    case MaterializationFailure::DecodeRejected:
        return "decode-rejected";
    case MaterializationFailure::AnalysisIncomplete:
        return "analysis-incomplete";
    case MaterializationFailure::IrVerificationFailed:
        return "ir-verification-failed";
    case MaterializationFailure::CodeGenerationFailed:
        return "code-generation-failed";
    case MaterializationFailure::ByteIdentityMismatch:
        return "byte-identity-mismatch";
    case MaterializationFailure::AotTemplateMismatch:
        return "aot-template-mismatch";
    case MaterializationFailure::MissingAot:
        return "missing-aot";
    case MaterializationFailure::GenerationMismatch:
        return "generation-mismatch";
    case MaterializationFailure::ModuleUnloaded:
        return "module-unloaded";
    case MaterializationFailure::RelocationMismatch:
        return "relocation-mismatch";
    case MaterializationFailure::StaleHandle:
        return "stale-handle";
    case MaterializationFailure::InvalidBlock:
        return "invalid-block";
    }
    return "invalid-block";
}

std::string
format_block_materialization_metrics_json(const BlockMaterializationMetrics& metrics,
                                          const std::span<const BlockMaterializationEvent> events,
                                          const bool include_local_details) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-materialization-v1\",\"materialization_attempts\":"
           << metrics.requests << ",\"materialization_successes\":" << metrics.materializations
           << ",\"interpreter_materializations\":" << metrics.interpreter_materializations
           << ",\"retained_validation_bytes\":" << metrics.retained_validation_bytes
           << ",\"peak_retained_validation_bytes\":"
           << metrics.peak_retained_validation_bytes
           << ",\"reclaimed_validation_bytes\":" << metrics.reclaimed_validation_bytes
           << ",\"materialization_rejections\":" << metrics.misses
           << ",\"materialization_budget_failures\":" << metrics.budget_failures
           << ",\"generation_revalidation_failures\":" << metrics.generation_revalidation_failures
           << ",\"byte_identity_failures\":" << metrics.byte_identity_failures
           << ",\"dispatch_validation_failures\":" << metrics.dispatch_validation_failures
           << ",\"first_failure\":\"" << materialization_failure_name(metrics.first_failure)
           << "\",\"events\":[";
    if (include_local_details) {
        for (std::size_t index = 0u; index < events.size(); ++index) {
            if (index != 0u) output << ',';
            output << "{\"sequence\":" << events[index].sequence
                   << ",\"target\":" << events[index].target
                   << ",\"success\":" << (events[index].success ? "true" : "false")
                   << ",\"failure\":\"" << materialization_failure_name(events[index].failure)
                   << "\"}";
        }
    }
    output << "]}";
    return output.str();
}

} // namespace katana::runtime
