#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/indirect_dispatch.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
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

} // namespace

std::uint64_t ExecutableModule::end_address() const noexcept {
    return static_cast<std::uint64_t>(guest_start) + bytes.size();
}

bool ExecutableModule::contains(const std::uint32_t address,
                                const std::size_t width) const noexcept {
    if (!active || width == 0u) return false;
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = begin + width;
    return begin >= guest_start && end >= begin && end <= end_address();
}

bool ExecutableModule::materializable(const std::uint32_t address,
                                      const std::size_t width) const noexcept {
    if (!executable_permission || !contains(address, width)) return false;
    const auto begin = static_cast<std::uint64_t>(address - guest_start);
    const auto end = begin + width;
    for (const auto& range : range_roles) {
        if (range.role == ExecutableStorageRole::RuntimeMaterializable || range.size == 0u)
            continue;
        const auto range_end = static_cast<std::uint64_t>(range.offset) + range.size;
        if (begin < range_end && range.offset < end) return false;
    }
    return true;
}

void ExecutableModuleCatalog::publish(ExecutableModule module) {
    if (module.id.empty() || module.source_identity.empty() || module.bytes.empty() ||
        (module.guest_start & 1u) != 0u || module.generation == 0u ||
        module.relocation_generation == 0u) {
        throw std::invalid_argument(
            "Ausfuehrbares Modul besitzt keine stabile Identitaet oder Bytes.");
    }
    if (module.end_address() >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::out_of_range("Ausfuehrbares Modul ueberschreitet den Gastadressraum.");
    }
    if (find(module.id) != nullptr)
        throw std::invalid_argument("Ausfuehrbare Modulidentitaet ist bereits aktiv.");
    for (const auto& existing : modules_) {
        if (existing.active && module.guest_start < existing.end_address() &&
            existing.guest_start < module.end_address())
            throw std::invalid_argument("Ausfuehrbare Module ueberlappen sich.");
    }
    validate_relocations(module, module.relocations);
    for (const auto& range : module.range_roles) {
        if (range.size == 0u || range.offset > module.bytes.size() ||
            range.size > module.bytes.size() - range.offset)
            throw std::invalid_argument("Modulbereichsrolle liegt ausserhalb der Modulbytes.");
    }
    module.active = true;
    modules_.push_back(std::move(module));
    increment(metrics_.loads);
}

void ExecutableModuleCatalog::publish_loaded_range(ExecutableModule module,
                                                   RuntimeBlockTable& blocks,
                                                   ExecutableCodeTracker& tracker) {
    if (module.bytes.empty()) throw std::invalid_argument("Geladener Modulbereich ist leer.");
    const auto end = module.end_address();
    for (auto& existing : modules_) {
        if (!existing.active || module.guest_start >= existing.end_address() ||
            existing.guest_start >= end)
            continue;
        const auto invalidated =
            blocks.erase_overlapping_physical(existing.guest_start, existing.bytes.size());
        const auto tracked = tracker.observe_write(
            existing.guest_start, existing.bytes.size(), CodeWriteSource::Copy, true);
        metrics_.invalidated_blocks += std::max(invalidated, tracked.invalidated_blocks.size());
        existing.active = false;
        increment(metrics_.unloads);
    }
    publish(std::move(module));
}

void ExecutableModuleCatalog::unload(const std::string_view id,
                                     RuntimeBlockTable& blocks,
                                     ExecutableCodeTracker& tracker) {
    const auto module = std::find_if(modules_.begin(), modules_.end(), [&](const auto& candidate) {
        return candidate.active && candidate.id == id;
    });
    if (module == modules_.end()) throw std::invalid_argument("Unbekanntes aktives Modul.");
    const auto invalidated =
        blocks.erase_overlapping_physical(module->guest_start, module->bytes.size());
    const auto tracked = tracker.observe_write(
        module->guest_start, module->bytes.size(), CodeWriteSource::Copy, true);
    metrics_.invalidated_blocks += std::max(invalidated, tracked.invalidated_blocks.size());
    module->active = false;
    increment(metrics_.unloads);
}

void ExecutableModuleCatalog::replace(ExecutableModule module,
                                      RuntimeBlockTable& blocks,
                                      ExecutableCodeTracker& tracker) {
    const auto* previous = find(module.id);
    if (previous == nullptr) throw std::invalid_argument("Zu ersetzendes Modul ist nicht aktiv.");
    const auto generation = previous->generation;
    unload(module.id, blocks, tracker);
    module.generation =
        generation == std::numeric_limits<std::uint64_t>::max() ? generation : generation + 1u;
    publish(std::move(module));
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
    validate_relocations(*module, relocations);
    const auto invalidated =
        blocks.erase_overlapping_physical(module->guest_start, module->bytes.size());
    const auto tracked = tracker.observe_write(
        module->guest_start, module->bytes.size(), CodeWriteSource::Copy, true);
    metrics_.invalidated_blocks += std::max(invalidated, tracked.invalidated_blocks.size());
    module->relocations = std::move(relocations);
    if (module->relocation_generation != std::numeric_limits<std::uint64_t>::max())
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
    if (found->executable_permission && found->materializable(address, 2u)) return true;
    if (!found->active || !found->control_transfer_promotion_allowed) return false;
    const auto begin = address - found->guest_start;
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
    found->range_roles.clear();
    std::uint32_t cursor = 0u;
    for (const auto interval : merged) {
        if (cursor < interval.first)
            found->range_roles.push_back(
                {cursor, interval.first - cursor, ExecutableStorageRole::ProvenData});
        found->range_roles.push_back({interval.first,
                                      interval.second - interval.first,
                                      ExecutableStorageRole::RuntimeMaterializable});
        cursor = interval.second;
    }
    if (cursor < found->bytes.size())
        found->range_roles.push_back({cursor,
                                      static_cast<std::uint32_t>(found->bytes.size() - cursor),
                                      ExecutableStorageRole::ProvenData});
    found->executable_permission = true;
    return true;
}

void ExecutableModuleCatalog::record_runtime_write(const std::uint32_t address,
                                                   const std::size_t size,
                                                   const bool bytes_changed) {
    if (!bytes_changed || size == 0u ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u - address)
        return;
    const auto physical_begin = canonical_physical_address(address);
    const auto physical_end = static_cast<std::uint64_t>(physical_begin) + size;
    if (physical_end > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u)
        return;

    for (auto& module : modules_) {
        if (!module.active || !module.source_identity.starts_with("guest-runtime-write-v1"))
            continue;
        const auto module_begin = canonical_physical_address(module.guest_start);
        const auto module_end = static_cast<std::uint64_t>(module_begin) + module.bytes.size();
        if (physical_begin < module_end && module_begin < physical_end) {
            module.active = false;
            increment(metrics_.unloads);
        }
    }

    std::uint64_t cursor = physical_begin;
    while (cursor < physical_end) {
        const auto page = static_cast<std::uint32_t>(cursor) & ~(runtime_write_page_size - 1u);
        const auto page_end = std::min<std::uint64_t>(
            physical_end, static_cast<std::uint64_t>(page) + runtime_write_page_size);
        auto& written = runtime_write_pages_[page].written;
        auto offset = static_cast<std::uint32_t>(cursor - page);
        const auto end_offset = static_cast<std::uint32_t>(page_end - page);
        while (offset < end_offset) {
            const auto word = offset / 64u;
            const auto first_bit = offset % 64u;
            const auto word_end = std::min<std::uint32_t>(end_offset, (word + 1u) * 64u);
            const auto last_bit = word_end - word * 64u;
            auto mask = std::numeric_limits<std::uint64_t>::max() << first_bit;
            if (last_bit < 64u) mask &= (std::uint64_t{1u} << last_bit) - 1u;
            written[word] |= mask;
            offset = word_end;
        }
        cursor = page_end;
    }
}

bool ExecutableModuleCatalog::promote_runtime_write(const Memory& memory,
                                                    const std::uint32_t address,
                                                    const std::uint32_t maximum_bytes) {
    if ((address & 1u) != 0u || maximum_bytes < 2u || resolve(address, 2u) != nullptr) return false;
    const auto was_written = [this](const std::uint32_t candidate) {
        const auto physical = canonical_physical_address(candidate);
        const auto page = physical & ~(runtime_write_page_size - 1u);
        const auto found = runtime_write_pages_.find(page);
        if (found == runtime_write_pages_.end()) return false;
        const auto offset = physical - page;
        return (found->second.written[offset / 64u] & (std::uint64_t{1u} << (offset % 64u))) != 0u;
    };
    if (!was_written(address) || !was_written(address + 1u)) return false;

    std::uint32_t snapshot_size = maximum_bytes;
    while (snapshot_size >= 2u && !memory.contains(address, snapshot_size))
        --snapshot_size;
    if (snapshot_size < 2u) return false;

    ExecutableModule module;
    module.id = "guest-runtime-write-" + std::to_string(next_runtime_write_module_++);
    module.source_identity = "guest-runtime-write-v1";
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
    const auto* module = resolve(address, width);
    if (module == nullptr || !memory.contains(address, width)) return false;
    const auto offset = static_cast<std::size_t>(address - module->guest_start);
    for (std::size_t current = 0u; current < width; ++current)
        if (memory.read_u8(address + static_cast<std::uint32_t>(current)) !=
            relocated_module_byte(*module, offset + current))
            return false;
    return true;
}

const ExecutableModuleMetrics& ExecutableModuleCatalog::metrics() const noexcept {
    return metrics_;
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
         policy_.max_recursive_seeds == 0u || policy_.max_analysis_time_ms == 0u ||
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
    if (failure == MaterializationFailure::ByteIdentityMismatch)
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
                                         const BlockVariantKey& variant,
                                         const std::uint32_t callsite) {
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
    if (misses_by_target_[target] >= policy_.max_repeated_misses_per_target) {
        fail(MaterializationFailure::RepeatedMissLimit, target);
        return std::nullopt;
    }
    if (!cpu.memory.contains(target, 2u)) {
        fail(MaterializationFailure::Uncommitted, target);
        return std::nullopt;
    }
    auto* module = modules_.resolve(target, 2u);
    if (module == nullptr) {
        if (!modules_.promote_runtime_write(cpu.memory, target)) {
            fail(MaterializationFailure::UnknownSource, target);
            return std::nullopt;
        }
        module = modules_.resolve(target, 2u);
    }
    if (!module->executable_permission) {
        if (!module->control_transfer_promotion_allowed) {
            fail(MaterializationFailure::PermissionDenied, target);
            return std::nullopt;
        }
        if (!modules_.authorize_control_transfer(target)) {
            fail(MaterializationFailure::ProvenNonCode, target);
            return std::nullopt;
        }
    } else if (!module->materializable(target, 2u)) {
        fail(MaterializationFailure::ProvenNonCode, target);
        return std::nullopt;
    }
    module = modules_.resolve(target, 2u);
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
    const auto offset = static_cast<std::size_t>(target - module->guest_start);
    const auto snapshot_size = std::min<std::size_t>(
        module->bytes.size() - offset,
        static_cast<std::size_t>(policy_.max_bytes - metrics_.materialized_bytes));
    if (snapshot_size < 2u || snapshot_size > policy_.max_memory_bytes) {
        fail(MaterializationFailure::BudgetExhausted, target);
        return std::nullopt;
    }
    std::vector<std::uint8_t> snapshot(snapshot_size);
    for (std::size_t current = 0u; current < snapshot.size(); ++current)
        snapshot[current] = cpu.memory.read_u8(target + static_cast<std::uint32_t>(current));
    if (!modules_.validate_bytes(cpu.memory, target, snapshot.size())) {
        fail(MaterializationFailure::ByteIdentityMismatch, target);
        return std::nullopt;
    }
    const auto started = std::chrono::steady_clock::now();
    auto candidate = callback_(target, snapshot, variant);
    const auto elapsed_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());
    if (!candidate.decode_candidate_validated) {
        fail(MaterializationFailure::DecodeRejected, target);
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
        std::max(elapsed_ms, candidate.analysis_time_ms) > policy_.max_analysis_time_ms ||
        candidate.peak_memory_bytes > policy_.max_memory_bytes) {
        fail(MaterializationFailure::BudgetExhausted, target);
        return std::nullopt;
    }
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
    if (!current_module->materializable(block.virtual_start, block.size)) {
        fail(MaterializationFailure::ProvenNonCode, target);
        return std::nullopt;
    }
    for (std::size_t current = 0u; current < block.size; ++current) {
        if (cpu.memory.read_u8(target + static_cast<std::uint32_t>(current)) != snapshot[current]) {
            fail(MaterializationFailure::ByteIdentityMismatch, target);
            return std::nullopt;
        }
    }
    if (!modules_.validate_bytes(cpu.memory, block.virtual_start, block.size)) {
        fail(MaterializationFailure::ByteIdentityMismatch, target);
        return std::nullopt;
    }
    block.physical_origin = canonical_physical_address(block.virtual_start);
    block.provenance = "runtime-module:" + module_id + ":g" + std::to_string(module_generation) +
                       ":r" + std::to_string(relocation_generation) + ":" + block.provenance;
    const auto identity = stable_runtime_block_identity(block);
    const auto physical_origin = block.physical_origin;
    const auto block_size = block.size;
    const auto provenance = block.provenance;
    const auto handle = blocks_.register_runtime(std::move(block));
    origins_.push_back(
        {target,
         block_size,
         callsite,
         module_id,
         source_identity,
         module_generation,
         relocation_generation,
         handle,
         std::vector<std::uint8_t>(snapshot.begin(), snapshot.begin() + block_size)});
    if (!validate_for_dispatch(cpu, handle, target)) {
        origins_.pop_back();
        static_cast<void>(blocks_.erase_identity(identity));
        return std::nullopt;
    }
    try {
        if (tracker_ != nullptr) {
            static_cast<void>(tracker_->register_block({identity,
                                                        physical_origin,
                                                        block_size,
                                                        provenance,
                                                        {},
                                                        ExecutableBlockOrigin::RuntimeWrite}));
        }
    } catch (...) {
        origins_.pop_back();
        static_cast<void>(blocks_.erase_identity(identity));
        throw;
    }
    increment(metrics_.materializations);
    if (candidate.interpreter_backed) increment(metrics_.interpreter_materializations);
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
                                                    const std::uint32_t target) noexcept {
    const auto origin = std::find_if(origins_.begin(), origins_.end(), [&](const auto& candidate) {
        return candidate.handle == handle && candidate.address == target;
    });
    if (origin == origins_.end()) {
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
    if (!module->materializable(origin->address, origin->size) ||
        !cpu.memory.contains(origin->address, origin->size)) {
        increment(metrics_.dispatch_validation_failures);
        fail(MaterializationFailure::ProvenNonCode, target);
        return false;
    }
    for (std::size_t current = 0u; current < origin->snapshot.size(); ++current) {
        if (cpu.memory.read_u8(origin->address + static_cast<std::uint32_t>(current)) !=
            origin->snapshot[current]) {
            increment(metrics_.dispatch_validation_failures);
            fail(MaterializationFailure::ByteIdentityMismatch, target);
            return false;
        }
    }
    if (!modules_.validate_bytes(cpu.memory, origin->address, origin->size)) {
        increment(metrics_.dispatch_validation_failures);
        fail(MaterializationFailure::ByteIdentityMismatch, target);
        return false;
    }
    return true;
}

void DemandBlockMaterializer::record_invalidation(const std::uint32_t address,
                                                  const std::size_t size,
                                                  IndirectDispatchMetrics& dispatch_metrics) {
    const auto end = static_cast<std::uint64_t>(address) + size;
    origins_.erase(std::remove_if(origins_.begin(),
                                  origins_.end(),
                                  [&](const auto& origin) {
                                      const auto origin_end =
                                          static_cast<std::uint64_t>(origin.address) + origin.size;
                                      if (address >= origin_end || origin.address >= end)
                                          return false;
                                      dispatch_metrics.record_invalidation(origin.callsite);
                                      return true;
                                  }),
                   origins_.end());
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
