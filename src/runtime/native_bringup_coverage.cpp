#include "katana/runtime/native_bringup_dispatch.hpp"

#include "katana/runtime/block_abi.hpp"
#include "katana/runtime/native_port_content.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

namespace katana::runtime {
namespace {

using CoverageSourceKey = std::tuple<std::uint8_t, std::uint32_t>;
using CoverageEntryKey = std::tuple<std::uint32_t, std::string_view>;

[[nodiscard]] std::uint32_t canonical_coverage_runtime_alias(
    const std::uint32_t address) noexcept {
    // Match the native no-MMU binder's P0/P1/P2 alias contract. P3/P4 remain
    // distinct and fail the exact loaded-module identity checks.
    if ((address >> 29u) >= 6u) return address;
    return canonical_physical_address(address) | 0x80000000u;
}

bool canonical_sha256(const std::string_view value) noexcept {
    constexpr std::string_view prefix = "sha256:";
    return value.starts_with(prefix) && value.size() == prefix.size() + 64u &&
           std::all_of(
               value.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
               value.end(),
               [](const char digit) {
                   return (digit >= '0' && digit <= '9') ||
                          (digit >= 'a' && digit <= 'f');
               });
}

std::optional<std::uint8_t> transfer_rank(
    const NativeBringupTransferKind transfer) noexcept {
    switch (transfer) {
    case NativeBringupTransferKind::CallRegister:
        return std::uint8_t{0u};
    case NativeBringupTransferKind::TailJumpRegister:
        return std::uint8_t{1u};
    }
    return std::nullopt;
}

std::optional<CoverageSourceKey> coverage_source_key(
    const NativeBringupCoverageSourceTransfer& source) noexcept {
    const auto rank = transfer_rank(source.transfer_kind);
    if (!rank) return std::nullopt;
    return CoverageSourceKey{*rank, source.callsite};
}

std::optional<CoverageSourceKey> coverage_source_key(
    const NativeBringupCoveragePreflightRequest& request) noexcept {
    const auto rank = transfer_rank(request.transfer_kind);
    if (!rank) return std::nullopt;
    return CoverageSourceKey{*rank, request.callsite};
}

std::optional<CoverageEntryKey> coverage_entry_key(
    const NativeBringupCoverageEntry& entry) noexcept {
    const auto target = static_cast<std::uint64_t>(entry.runtime_start) +
                        entry.module_relative_offset;
    if (target > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return CoverageEntryKey{
        canonical_coverage_runtime_alias(
            static_cast<std::uint32_t>(target)),
        entry.module_identity};
}

CoverageEntryKey coverage_entry_key(
    const NativePortLoadedAotEntryView& entry) noexcept {
    return CoverageEntryKey{
        canonical_coverage_runtime_alias(
            entry.runtime_start + entry.source_offset),
        entry.module_sha256};
}

bool execution_matches(
    const RuntimeBlockTable& table,
    const std::optional<ValidatedBlockExecution>& execution,
    const NativeBringupDispatchStaticAotBinding& binding,
    const BlockVariantKey& variant) noexcept {
    return execution.has_value() && static_cast<bool>(execution->block) &&
           execution->function != nullptr &&
           execution->virtual_start == binding.block.virtual_address &&
           execution->physical_origin == binding.block.physical_address &&
           execution->size == binding.size && execution->variant == variant &&
           execution->end_kind == binding.end_kind &&
           !execution->runtime_registered &&
           execution->provenance == binding.block_code_identity &&
           execution->generation_guard.kind ==
               BlockDispatchGenerationGuardKind::StaticAot &&
           execution->generation_guard_reusable &&
           table.static_dispatch_generation_guard_current(
               execution->generation_guard);
}

bool current_static_target_execution(
    const RuntimeBlockTable& table,
    const std::optional<ValidatedBlockExecution>& execution,
    const std::uint32_t target,
    const BlockVariantKey& variant) noexcept {
    const auto canonical_target = canonical_coverage_runtime_alias(target);
    return execution.has_value() && static_cast<bool>(execution->block) &&
           execution->function != nullptr &&
           execution->virtual_start == canonical_target &&
           execution->physical_origin ==
               canonical_physical_address(canonical_target) &&
           execution->size >= 2u && execution->variant == variant &&
           !execution->runtime_registered &&
           canonical_sha256(execution->provenance) &&
           execution->generation_guard.kind ==
               BlockDispatchGenerationGuardKind::StaticAot &&
           execution->generation_guard_reusable &&
           table.static_dispatch_generation_guard_current(
               execution->generation_guard);
}

bool loaded_entry_matches(
    const NativePortLoadedAotEntryView& current,
    const NativeBringupCoverageEntry& expected) noexcept {
    return current.module_sha256 == expected.module_identity &&
           current.block_sha256 == expected.target.block_code_identity &&
           current.source_start == expected.source_start &&
           current.runtime_start == expected.runtime_start &&
           current.module_size == expected.module_size &&
           current.source_offset == expected.module_relative_offset &&
           current.block_size == expected.target.size &&
           current.lifecycle_generation != 0u;
}

bool loaded_entry_matches(
    const NativePortLoadedAotEntryView& current,
    const NativePortLoadedAotEntryView& expected) noexcept {
    return current.active &&
           current.module_sha256 == expected.module_sha256 &&
           current.block_sha256 == expected.block_sha256 &&
           current.source_start == expected.source_start &&
           current.runtime_start == expected.runtime_start &&
           current.module_size == expected.module_size &&
           current.source_offset == expected.source_offset &&
           current.block_size == expected.block_size &&
           current.lifecycle_generation == expected.lifecycle_generation &&
           current.lifecycle_generation != 0u;
}

bool loaded_target_execution_matches(
    const RuntimeBlockTable& table,
    const std::optional<ValidatedBlockExecution>& execution,
    const NativePortLoadedAotEntryView& entry,
    const BlockVariantKey& variant) noexcept {
    const auto source_target =
        static_cast<std::uint64_t>(entry.source_start) + entry.source_offset;
    if (source_target > std::numeric_limits<std::uint32_t>::max())
        return false;
    const auto source_address = static_cast<std::uint32_t>(source_target);
    return current_static_target_execution(
               table, execution, source_address, variant) &&
           execution->virtual_start == source_address &&
           execution->size == entry.block_size &&
           execution->provenance == entry.block_sha256;
}

bool runtime_image_target_execution_matches(
    const RuntimeBlockTable& table,
    const std::optional<ValidatedBlockExecution>& execution,
    const NativePortRuntimeImageActiveEntryView& entry,
    const BlockVariantKey& variant) noexcept {
    const auto source_target =
        static_cast<std::uint64_t>(entry.source_start) + entry.source_offset;
    if (source_target > std::numeric_limits<std::uint32_t>::max())
        return false;
    const auto source_address = static_cast<std::uint32_t>(source_target);
    return current_static_target_execution(
               table, execution, source_address, variant) &&
           execution->virtual_start == source_address &&
           execution->size == entry.block_size &&
           execution->provenance == entry.block_sha256 &&
           entry.lifecycle_generation != 0u;
}

bool loaded_source_matches(
    const NativePortLoadedAotEntryView& current,
    const NativeBringupCoverageSourceTransfer& expected) noexcept {
    return current.active &&
           current.module_sha256 == expected.source_module_identity &&
           current.block_sha256 == expected.source.block_code_identity &&
           current.runtime_start == expected.source_runtime_start &&
           current.module_size == expected.source_module_size &&
           current.source_offset == expected.source_module_offset &&
           current.block_size == expected.source.size &&
           static_cast<std::uint64_t>(current.source_start) +
                   current.source_offset ==
               expected.source.block.virtual_address &&
           current.lifecycle_generation != 0u;
}

bool runtime_image_source_matches(
    const NativePortRuntimeImageActiveEntryView& current,
    const NativeBringupCoverageSourceTransfer& expected) noexcept {
    return current.image_id == expected.source_image_id &&
           current.image_sha256 == expected.source_module_identity &&
           current.block_sha256 == expected.source.block_code_identity &&
           current.runtime_start == expected.source_runtime_start &&
           current.image_size == expected.source_module_size &&
           current.source_offset == expected.source_module_offset &&
           current.block_size == expected.source.size &&
           static_cast<std::uint64_t>(current.source_start) +
                   current.source_offset ==
               expected.source.block.virtual_address &&
           current.lifecycle_generation != 0u;
}

struct CoverageSourceAuthority final {
    NativeBringupCoverageSourceKind kind =
        NativeBringupCoverageSourceKind::StaticAot;
    std::string_view module_identity;
    std::string_view block_identity;
    std::uint32_t runtime_start = 0u;
    std::uint32_t module_offset = 0u;
    std::uint64_t lifecycle_generation = 0u;
};

bool source_transfer_shape_matches(
    const NativeBringupCoveragePreflightRequest& request,
    const std::uint32_t source_start,
    const std::uint32_t source_size,
    const std::optional<BlockEndKind> end_kind = std::nullopt) noexcept {
    const bool call = request.transfer_kind ==
                      NativeBringupTransferKind::CallRegister;
    const auto expected_end =
        call ? BlockEndKind::Call : BlockEndKind::DynamicBranch;
    const auto source_end =
        static_cast<std::uint64_t>(source_start) + source_size;
    const auto callsite_end =
        static_cast<std::uint64_t>(request.callsite) + 4u;
    return (request.source.virtual_address & 1u) == 0u &&
           request.source.physical_address ==
               canonical_physical_address(request.source.virtual_address) &&
           request.source.virtual_address == source_start && source_size >= 4u &&
           source_end <= 0x1'0000'0000ull && callsite_end == source_end &&
           (!end_kind || *end_kind == expected_end) &&
           (call ? request.continuation == callsite_end
                 : request.continuation == 0u);
}

std::optional<CoverageSourceAuthority> active_coverage_source_authority(
    const RuntimeBlockTable& table,
    NativePortRuntimeImageBindings& runtime_images,
    NativePortLoadedAotBinder& binder,
    const NativeBringupCoverageDispatchContext& context,
    const NativeBringupCoveragePreflightRequest& request) {
    const auto runtime_source =
        relocate_code_address(request.source.virtual_address);
    const auto runtime_image_source =
        runtime_images.active_entry_for_address(runtime_source);
    if (runtime_image_source.has_value()) {
        const auto source_address =
            static_cast<std::uint64_t>(runtime_image_source->source_start) +
            runtime_image_source->source_offset;
        if (source_address != request.source.virtual_address ||
            !source_transfer_shape_matches(
                request, static_cast<std::uint32_t>(source_address),
                runtime_image_source->block_size))
            return std::nullopt;
        return CoverageSourceAuthority{
            NativeBringupCoverageSourceKind::RuntimeImage,
            runtime_image_source->image_sha256,
            runtime_image_source->block_sha256,
            runtime_image_source->runtime_start,
            runtime_image_source->source_offset,
            runtime_image_source->lifecycle_generation};
    }
    const auto active_module =
        binder.active_module_for_address(runtime_source);
    if (active_module.has_value()) {
        const auto source_offset =
            static_cast<std::uint64_t>(runtime_source) -
            active_module->runtime_start;
        const auto source_address =
            static_cast<std::uint64_t>(active_module->source_start) +
            source_offset;
        if (source_offset > active_module->byte_size ||
            source_address != request.source.virtual_address)
            return std::nullopt;
        const auto active_source =
            binder.active_entry_for_address(runtime_source);
        if (!active_source.has_value() || !active_source->active ||
            active_source->module_sha256 != active_module->sha256 ||
            active_source->source_start != active_module->source_start ||
            active_source->runtime_start != active_module->runtime_start ||
            active_source->module_size != active_module->byte_size ||
            active_source->lifecycle_generation !=
                active_module->lifecycle_generation ||
            !source_transfer_shape_matches(
                request,
                active_source->source_start + active_source->source_offset,
                active_source->block_size))
            return std::nullopt;
        return CoverageSourceAuthority{
            NativeBringupCoverageSourceKind::LoadedAot,
            active_source->module_sha256,
            active_source->block_sha256,
            active_source->runtime_start,
            active_source->source_offset,
            active_source->lifecycle_generation};
    }

    const auto source_execution = table.lookup_static_aot(
        request.source.physical_address,
        request.source.virtual_address,
        request.variant);
    if (!current_static_target_execution(
            table, source_execution, request.source.virtual_address,
            request.variant) ||
        !source_transfer_shape_matches(
            request, source_execution->virtual_start, source_execution->size,
            source_execution->end_kind))
        return std::nullopt;
    return CoverageSourceAuthority{
        NativeBringupCoverageSourceKind::StaticAot,
        context.pack.identity.aot_pack_identity,
        source_execution->provenance,
        source_execution->virtual_start,
        0u,
        0u};
}

inline constexpr std::size_t coverage_preflight_cache_capacity = 256u;
static_assert((coverage_preflight_cache_capacity &
               (coverage_preflight_cache_capacity - 1u)) == 0u);

struct CoveragePreflightCacheEntry final {
    bool valid = false;
    const RuntimeBlockTable* table = nullptr;
    const NativePortRuntimeImageBindings* runtime_images = nullptr;
    const NativePortLoadedAotBinder* binder = nullptr;
    const NativeBringupCoverageDispatchContext* context = nullptr;
    std::uint64_t table_lifetime = 0u;
    std::uint64_t table_generation = 0u;
    NativePortRuntimeImageDispatchStamp runtime_image_stamp;
    NativePortLoadedAotDispatchStamp binder_stamp;
    NativeBringupCoveragePreflightRequest request;
    NativeBringupCoveragePreflightResult result;
    bool has_observation = false;
    NativeBringupCoverageObservation observation;
    NativeBringupCoverageObservations::EventIndex observation_index =
        NativeBringupCoverageObservations::invalid_event_index;
};

thread_local std::array<CoveragePreflightCacheEntry,
                        coverage_preflight_cache_capacity>
    coverage_preflight_cache;

std::size_t coverage_preflight_cache_index(
    const NativeBringupCoveragePreflightRequest& request) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&](const std::uint64_t value) {
        hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6u) +
                (hash >> 2u);
    };
    mix(static_cast<std::uint8_t>(request.transfer_kind));
    mix(request.callsite);
    mix(request.target);
    mix(request.continuation);
    mix(request.source.virtual_address);
    mix(request.source.physical_address);
    mix(request.variant.address_space_generation);
    mix(request.variant.mmu_generation);
    mix(request.variant.watchpoint_generation);
    mix(request.variant.fpscr_mode);
    mix(request.variant.runtime_generation);
    mix(static_cast<std::uint8_t>(request.target_hook));
    return static_cast<std::size_t>(hash) &
           (coverage_preflight_cache_capacity - 1u);
}

bool same_coverage_preflight_request(
    const NativeBringupCoveragePreflightRequest& left,
    const NativeBringupCoveragePreflightRequest& right) noexcept {
    return left.transfer_kind == right.transfer_kind &&
           left.callsite == right.callsite && left.target == right.target &&
           left.continuation == right.continuation &&
           left.source == right.source && left.variant == right.variant &&
           left.target_hook == right.target_hook;
}

std::optional<NativeBringupCoveragePreflightResult>
replay_cached_coverage_preflight(
    const RuntimeBlockTable& table,
    NativePortRuntimeImageBindings& runtime_images,
    NativePortLoadedAotBinder& binder,
    const NativeBringupCoverageDispatchContext& context,
    const NativeBringupCoveragePreflightRequest& request) noexcept {
    auto& cached = coverage_preflight_cache[
        coverage_preflight_cache_index(request)];
    if (!cached.valid || cached.table != &table ||
        cached.runtime_images != &runtime_images ||
        cached.binder != &binder ||
        cached.context != &context ||
        cached.table_lifetime != table.dispatch_lifetime() ||
        cached.table_generation != table.dispatch_generation() ||
        cached.runtime_image_stamp != runtime_images.dispatch_stamp() ||
        cached.binder_stamp != binder.dispatch_stamp() ||
        !same_coverage_preflight_request(cached.request, request) ||
        !context.validated_view_current(table, runtime_images, binder))
        return std::nullopt;

    if (cached.has_observation &&
        !context.observations.record_cached(
            cached.observation_index, cached.observation))
        cached.observation_index =
            context.observations.record(cached.observation);
    auto result = cached.result;
    result.cache_hit = true;
    return result;
}

NativeBringupCoveragePreflightResult remember_coverage_preflight(
    const RuntimeBlockTable& table,
    NativePortRuntimeImageBindings& runtime_images,
    NativePortLoadedAotBinder& binder,
    const NativeBringupCoverageDispatchContext& context,
    const NativeBringupCoveragePreflightRequest& request,
    NativeBringupCoveragePreflightResult result,
    const NativeBringupCoverageObservation* const observation) noexcept {
    auto& cached = coverage_preflight_cache[
        coverage_preflight_cache_index(request)];
    cached.valid = false;
    cached.table = &table;
    cached.runtime_images = &runtime_images;
    cached.binder = &binder;
    cached.context = &context;
    cached.table_lifetime = table.dispatch_lifetime();
    cached.table_generation = table.dispatch_generation();
    cached.runtime_image_stamp = runtime_images.dispatch_stamp();
    cached.binder_stamp = binder.dispatch_stamp();
    cached.request = request;
    result.cache_hit = false;
    cached.result = result;
    cached.has_observation = observation != nullptr;
    cached.observation_index =
        NativeBringupCoverageObservations::invalid_event_index;
    if (observation != nullptr) {
        cached.observation = *observation;
        cached.observation_index =
            context.observations.record(cached.observation);
    }
    cached.valid = true;
    return result;
}

[[noreturn]] void reject(
    const NativeBringupCoveragePreflightRequest& request,
    const NativeBringupDispatchMiss miss) {
    throw NativeBringupDispatchError(
        {request.transfer_kind,
         request.callsite,
         request.target,
         request.continuation,
         request.source,
         request.variant},
        miss);
}

} // namespace

NativeBringupCoveragePreflightResult
preflight_native_bringup_coverage_dispatch(
    const RuntimeBlockTable& table,
    NativePortRuntimeImageBindings& runtime_images,
    NativePortLoadedAotBinder& binder,
    const NativeBringupCoverageDispatchContext& context,
    const NativeBringupCoveragePreflightRequest& request) {
    if (binder.module_universe_identity() !=
            context.pack.identity.module_universe_identity ||
        binder.aot_pack_identity() !=
            context.pack.identity.aot_pack_identity)
        reject(request, NativeBringupDispatchMiss::InvalidEntry);
    if (const auto cached = replay_cached_coverage_preflight(
            table, runtime_images, binder, context, request);
        cached.has_value())
        return *cached;
    if (!context.validated_view_current(table, runtime_images, binder))
        reject(request, NativeBringupDispatchMiss::InvalidEntry);
    if (request.variant.runtime_generation != context.runtime_generation)
        reject(request, NativeBringupDispatchMiss::GenerationMismatch);

    const auto requested_source_key = coverage_source_key(request);
    if (!requested_source_key)
        reject(request, NativeBringupDispatchMiss::CoverageSourceMissing);
    const auto selected_source = std::lower_bound(
        context.pack.source_transfers.begin(),
        context.pack.source_transfers.end(),
        *requested_source_key,
        [](const NativeBringupCoverageSourceTransfer& source,
           const CoverageSourceKey& key) {
            return *coverage_source_key(source) < key;
        });
    const bool has_source_transfer =
        selected_source != context.pack.source_transfers.end() &&
        *coverage_source_key(*selected_source) == *requested_source_key;
    CoverageSourceAuthority source_authority;
    if (has_source_transfer) {
        if (request.source != selected_source->source.block ||
            request.continuation != selected_source->continuation)
            reject(request, NativeBringupDispatchMiss::SourceIdentityMismatch);

        const auto source_execution = table.lookup_static_aot(
            selected_source->source.block.physical_address,
            selected_source->source.block.virtual_address,
            request.variant);
        if (!execution_matches(
                table, source_execution, selected_source->source,
                request.variant))
            reject(request, NativeBringupDispatchMiss::SourceIdentityMismatch);
        source_authority = {
            selected_source->source_kind,
            selected_source->source_module_identity,
            selected_source->source.block_code_identity,
            selected_source->source_runtime_start,
            selected_source->source_module_offset,
            0u};
        if (selected_source->source_kind ==
            NativeBringupCoverageSourceKind::LoadedAot) {
            const auto runtime_source =
                selected_source->source_runtime_start +
                selected_source->source_module_offset;
            if (relocate_code_address(
                    selected_source->source.block.virtual_address) !=
                runtime_source)
                reject(
                    request,
                    NativeBringupDispatchMiss::LoadedModuleIdentityMismatch);
            const auto active_source =
                binder.active_entry_for_address(runtime_source);
            if (!active_source.has_value())
                reject(request, NativeBringupDispatchMiss::LoadedModuleInactive);
            if (!loaded_source_matches(*active_source, *selected_source))
                reject(
                    request,
                    NativeBringupDispatchMiss::LoadedModuleIdentityMismatch);
            source_authority.lifecycle_generation =
                active_source->lifecycle_generation;
        } else if (selected_source->source_kind ==
                   NativeBringupCoverageSourceKind::RuntimeImage) {
            const auto runtime_source =
                selected_source->source_runtime_start +
                selected_source->source_module_offset;
            const auto active_source =
                runtime_images.active_entry_for_address(runtime_source);
            if (!active_source.has_value())
                reject(request, NativeBringupDispatchMiss::RuntimeImageInactive);
            if (relocate_code_address(
                    selected_source->source.block.virtual_address) !=
                runtime_source)
                reject(
                    request,
                    NativeBringupDispatchMiss::RuntimeImageIdentityMismatch);
            if (!runtime_image_source_matches(
                    *active_source, *selected_source))
                reject(
                    request,
                    NativeBringupDispatchMiss::RuntimeImageIdentityMismatch);
            source_authority.lifecycle_generation =
                active_source->lifecycle_generation;
        }
    } else {
        // Coverage source records are diagnostics/proof accelerators, not
        // compile authority. An unknown edge may originate at any exact block
        // in the sealed Static-AOT universe or in a currently active,
        // identity- and generation-bound RuntimeImage/Loaded-AOT module. This is the
        // deliberate NativeBringup-only separation between code availability
        // and reachability proof.
        const auto active_source = active_coverage_source_authority(
            table, runtime_images, binder, context, request);
        if (!active_source.has_value())
            reject(request, NativeBringupDispatchMiss::CoverageSourceMissing);
        source_authority = *active_source;
    }
    if (request.target_hook ==
        NativeBringupCoveragePreflightRequest::TargetHook::
            ConflictingInstruction)
        reject(request, NativeBringupDispatchMiss::HookReplacementConflict);
    if (request.target_hook ==
        NativeBringupCoveragePreflightRequest::TargetHook::
            CallableFunctionEntry) {
        // The generated dispatcher invokes this exact native FunctionEntry;
        // it does not execute or bind an AOT body for the target. Source
        // identity and generation have already been authenticated above, so
        // return an empty execution result without mutating the Loaded-AOT
        // binder or claiming a CoverageOnly target observation.
        return remember_coverage_preflight(
            table, runtime_images, binder, context, request,
            {{}, {}, request.target,
             canonical_physical_address(request.target), 0u, false},
            nullptr);
    }
    if (request.target_hook !=
        NativeBringupCoveragePreflightRequest::TargetHook::None)
        reject(request, NativeBringupDispatchMiss::InvalidEntry);

    // A complete-disassembly source may legitimately indirect into the sealed
    // primary Static-AOT image. That target has no Loaded-AOT lifecycle entry:
    // requiring one here rejects ordinary main-image function pointers before
    // dispatch. Keep this coverage-only by requiring the exact halfword entry,
    // current Static-AOT generation guard and canonical code identity. Loaded
    // overlays still fall through to the identity/lifecycle transaction below.
    const auto canonical_target =
        canonical_coverage_runtime_alias(request.target);
    const auto static_target = table.lookup_static_aot(
        canonical_physical_address(canonical_target), canonical_target,
        request.variant);
    if (current_static_target_execution(
            table, static_target, canonical_target, request.variant)) {
        const NativeBringupCoverageObservation observation{
            request.transfer_kind,
            request.callsite,
            canonical_target,
            source_authority.kind,
            source_authority.module_identity,
            source_authority.block_identity,
            source_authority.runtime_start,
            source_authority.module_offset,
            source_authority.lifecycle_generation,
            context.pack.identity.aot_pack_identity,
            static_target->provenance,
            static_target->virtual_start,
            0u,
            0u,
            context.pack.identity.aot_pack_generation,
            context.runtime_generation,
            1u};
        return remember_coverage_preflight(
            table, runtime_images, binder, context, request,
            {static_target->block, *static_target, request.target,
             canonical_physical_address(request.target), 0u, false},
            &observation);
    }

    // Fixed runtime images have their own activation/retirement owner. They
    // are neither StaticAOT at the runtime address nor staged LoadedAOT
    // modules. Authenticate the active image tuple, then execute the exact
    // source-space AOT block that its sealed mapping names.
    const auto runtime_image_target =
        runtime_images.active_entry_for_address(canonical_target);
    if (runtime_image_target.has_value()) {
        const auto source_target_wide =
            static_cast<std::uint64_t>(runtime_image_target->source_start) +
            runtime_image_target->source_offset;
        if (source_target_wide > std::numeric_limits<std::uint32_t>::max())
            reject(
                request,
                NativeBringupDispatchMiss::RuntimeImageIdentityMismatch);
        const auto source_target =
            static_cast<std::uint32_t>(source_target_wide);
        const auto target_execution = table.lookup_static_aot(
            canonical_physical_address(source_target), source_target,
            request.variant);
        if (!runtime_image_target_execution_matches(
                table, target_execution, *runtime_image_target,
                request.variant))
            reject(
                request,
                NativeBringupDispatchMiss::RuntimeImageIdentityMismatch);
        const NativeBringupCoverageObservation observation{
            request.transfer_kind,
            request.callsite,
            canonical_target,
            source_authority.kind,
            source_authority.module_identity,
            source_authority.block_identity,
            source_authority.runtime_start,
            source_authority.module_offset,
            source_authority.lifecycle_generation,
            runtime_image_target->image_sha256,
            runtime_image_target->block_sha256,
            runtime_image_target->runtime_start,
            runtime_image_target->source_offset,
            runtime_image_target->lifecycle_generation,
            context.pack.identity.aot_pack_generation,
            context.runtime_generation,
            1u};
        return remember_coverage_preflight(
            table, runtime_images, binder, context, request,
            {target_execution->block, *target_execution, request.target,
             canonical_physical_address(request.target),
             runtime_image_target->lifecycle_generation, false},
            &observation);
    }

    const auto staged_target =
        binder.preflight_entry_for_address(request.target);
    if (!staged_target.has_value())
        reject(request, NativeBringupDispatchMiss::UnmappedTarget);
    const auto requested_entry_key = coverage_entry_key(*staged_target);
    if (std::get<0>(requested_entry_key) !=
        canonical_coverage_runtime_alias(request.target))
        reject(
            request,
            NativeBringupDispatchMiss::LoadedModuleIdentityMismatch);
    const auto selected_entry = std::lower_bound(
        context.pack.entries.begin(),
        context.pack.entries.end(),
        requested_entry_key,
        [](const NativeBringupCoverageEntry& entry,
           const CoverageEntryKey& key) {
            return *coverage_entry_key(entry) < key;
        });
    const bool has_placement_entry =
        selected_entry != context.pack.entries.end() &&
        *coverage_entry_key(*selected_entry) == requested_entry_key;
    if (has_placement_entry &&
        !loaded_entry_matches(*staged_target, *selected_entry))
        reject(
            request,
            NativeBringupDispatchMiss::LoadedModuleIdentityMismatch);

    // The coverage entry list is an optional fixed-placement accelerator, not
    // executable authority. A complete-disassembly module can be compiled
    // before its title loader chooses a destination, in which case export has
    // no honest runtime_start to put in that list. The binder already owns the
    // generated module/block universe and has authenticated the staged bytes,
    // exact loader placement and lifecycle generation above. Do not require a
    // second table populated from observed placements: the generated dispatch
    // resolves the host function after bind_entry installs this exact module
    // mapping.
    std::optional<ValidatedBlockExecution> target_execution;
    RuntimeBlockHandle target_block;
    if (has_placement_entry) {
        target_execution = table.lookup_static_aot(
            selected_entry->target.block.physical_address,
            selected_entry->target.block.virtual_address,
            request.variant);
        if (!loaded_target_execution_matches(
                table, target_execution, *staged_target, request.variant))
            reject(request, NativeBringupDispatchMiss::UnknownCompiledTarget);
        target_block = target_execution->block;
    }

    if (!binder.bind_entry(request.target))
        reject(request, NativeBringupDispatchMiss::UnmappedTarget);
    const auto active_target =
        binder.active_entry_for_address(request.target);
    if (!active_target.has_value() || !active_target->active ||
        active_target->lifecycle_generation !=
            staged_target->lifecycle_generation ||
        !loaded_entry_matches(*active_target, *staged_target))
        reject(
            request,
            NativeBringupDispatchMiss::LoadedModuleIdentityMismatch);

    const NativeBringupCoverageObservation observation{
        request.transfer_kind,
        request.callsite,
        canonical_coverage_runtime_alias(request.target),
        source_authority.kind,
        source_authority.module_identity,
        source_authority.block_identity,
        source_authority.runtime_start,
        source_authority.module_offset,
        source_authority.lifecycle_generation,
        active_target->module_sha256,
        active_target->block_sha256,
        active_target->runtime_start,
        active_target->source_offset,
        active_target->lifecycle_generation,
        context.pack.identity.aot_pack_generation,
        context.runtime_generation,
        1u};

    return remember_coverage_preflight(
        table, runtime_images, binder, context, request,
        {target_block,
         target_execution.value_or(ValidatedBlockExecution{}),
         request.target,
         canonical_physical_address(request.target),
         active_target->lifecycle_generation,
         false},
        &observation);
}

} // namespace katana::runtime
