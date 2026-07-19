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
    const auto* module = modules_.resolve(target, 2u);
    if (module == nullptr) {
        fail(MaterializationFailure::UnknownSource, target);
        return std::nullopt;
    }
    if (!module->executable_permission) {
        fail(MaterializationFailure::PermissionDenied, target);
        return std::nullopt;
    }
    if (!module->materializable(target, 2u)) {
        fail(MaterializationFailure::ProvenNonCode, target);
        return std::nullopt;
    }
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
    if (!candidate.bounded_analysis_complete) {
        fail(MaterializationFailure::AnalysisIncomplete, target);
        return std::nullopt;
    }
    if (!candidate.ir_verified) {
        fail(MaterializationFailure::IrVerificationFailed, target);
        return std::nullopt;
    }
    if (!candidate.code_generated) {
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
