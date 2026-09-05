#include "katana/runtime/native_bringup_dispatch.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <tuple>

namespace katana::runtime {
namespace {

void increment_saturated(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
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

bool valid_component(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128u &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_' || character == '.';
           });
}

bool range_contains(const std::uint32_t start,
                    const std::uint32_t size,
                    const std::uint32_t value,
                    const std::uint32_t extent) noexcept {
    const auto begin = static_cast<std::uint64_t>(start);
    const auto end = begin + static_cast<std::uint64_t>(size);
    const auto value_begin = static_cast<std::uint64_t>(value);
    const auto value_end = value_begin + static_cast<std::uint64_t>(extent);
    return size != 0u && extent != 0u && end <= 0x1'0000'0000ull &&
           value_begin >= begin && value_end <= end;
}

bool binding_valid(
    const NativeBringupDispatchStaticAotBinding& binding) noexcept {
    return direct_p1_p2_block_binding_contiguous(
               binding.block.virtual_address,
               binding.block.physical_address,
               binding.size) &&
           canonical_sha256(binding.block_code_identity);
}

using DispatchKey =
    std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>;

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

std::optional<DispatchKey> entry_key(
    const NativeBringupDispatchEntry& entry) noexcept {
    const auto rank = transfer_rank(entry.admission.transfer_kind);
    if (!rank) return std::nullopt;
    return DispatchKey{
        *rank, entry.admission.callsite, entry.admission.target};
}

std::optional<DispatchKey> request_key(
    const NativeBringupDispatchPreflightRequest& request) noexcept {
    const auto rank = transfer_rank(request.transfer_kind);
    if (!rank) return std::nullopt;
    return DispatchKey{*rank, request.callsite, request.target};
}

bool entry_execution_admitted(
    const NativeBringupDispatchEntry& entry,
    const NativeBringupDispatchPackIdentity& identity) noexcept {
    const auto& admission = entry.admission;
    const bool proven =
        admission.stage == NativeBringupEvidenceStage::Proven;
    const bool candidate =
        admission.stage == NativeBringupEvidenceStage::Candidate;
    if ((!proven && !candidate) ||
        admission.contract_version !=
            native_bringup_evidence_contract_version ||
        (proven && admission.proposed_promotion !=
                       NativeBringupPromotionType::StaticCompiledTarget) ||
        (candidate && admission.proposed_promotion !=
                           NativeBringupPromotionType::AnalyzerReproof) ||
        !admission.runtime_contract_identity.empty() ||
        (proven && !admission.missing_proof.empty()) ||
        (candidate && (admission.missing_proof.empty() ||
                       admission.static_correlation.empty() ||
                       admission.analyzer_path.empty())))
        return false;

    const auto source_block_end =
        static_cast<std::uint64_t>(admission.source_block) +
        admission.source_block_size;
    const auto callsite_end =
        static_cast<std::uint64_t>(admission.callsite) + 4u;
    const bool call = admission.transfer_kind ==
                      NativeBringupTransferKind::CallRegister;
    const bool jump = admission.transfer_kind ==
                      NativeBringupTransferKind::TailJumpRegister;
    const bool valid_continuation =
        call ? static_cast<std::uint64_t>(admission.continuation) ==
                   callsite_end
             : jump && admission.continuation == 0u;
    const auto expected_source_end =
        call ? BlockEndKind::Call : BlockEndKind::DynamicBranch;

    return canonical_sha256(admission.source_owner_code_identity) &&
           canonical_sha256(admission.source_block_code_identity) &&
           canonical_sha256(admission.callsite_code_identity) &&
           canonical_sha256(admission.target_block_code_identity) &&
           canonical_sha256(admission.target_owner_code_identity) &&
           valid_component(admission.source_image_id) &&
           valid_component(admission.target_image_id) &&
           canonical_sha256(admission.source_module_identity) &&
           canonical_sha256(admission.target_module_identity) &&
           admission.source_generation == identity.aot_pack_generation &&
           admission.target_generation == identity.aot_pack_generation &&
           binding_valid(entry.source) && binding_valid(entry.target) &&
           (admission.source_owner & 1u) == 0u &&
           (admission.source_owner_size & 1u) == 0u &&
           (admission.source_block & 1u) == 0u &&
           admission.source_block_size >= 4u &&
           (admission.source_block_size & 1u) == 0u &&
           (admission.callsite & 1u) == 0u &&
           (admission.target_owner & 1u) == 0u &&
           (admission.target_owner_size & 1u) == 0u &&
           (admission.target & 1u) == 0u &&
           admission.target_block_size >= 2u &&
           (admission.target_block_size & 1u) == 0u &&
           entry.source.block.virtual_address == admission.source_block &&
           entry.source.size == admission.source_block_size &&
           entry.source.end_kind == expected_source_end &&
           entry.source.block_code_identity ==
               admission.source_block_code_identity &&
           entry.target.block.virtual_address == admission.target &&
           entry.target.size == admission.target_block_size &&
           entry.target.block_code_identity ==
               admission.target_block_code_identity &&
           range_contains(admission.source_owner,
                          admission.source_owner_size,
                          admission.source_block,
                          admission.source_block_size) &&
           range_contains(admission.source_block,
                          admission.source_block_size,
                          admission.callsite,
                          4u) &&
           source_block_end <= 0x1'0000'0000ull &&
           callsite_end == source_block_end &&
           range_contains(admission.target_owner,
                          admission.target_owner_size,
                          admission.target,
                          admission.target_block_size) &&
           valid_continuation;
}

bool ordered_static_view(const NativeBringupDispatchPack& pack) noexcept {
    const auto& identity = pack.identity;
    const bool no_authoring_proof =
        identity.authoring_artifact_identity.empty() &&
        pack.allowlist.empty();
    if (pack.allowlist.size() > native_bringup_dispatch_maximum_entries ||
        identity.contract_version !=
            native_bringup_evidence_contract_version ||
        (!no_authoring_proof &&
         !canonical_sha256(identity.authoring_artifact_identity)) ||
        !valid_component(identity.project_id) ||
        !valid_component(identity.project_version) ||
        !canonical_sha256(identity.analysis_identity) ||
        !canonical_sha256(identity.aot_pack_identity) ||
        identity.aot_pack_generation == 0u)
        return false;

    std::optional<DispatchKey> previous;
    for (const auto& entry : pack.allowlist) {
        if (!entry_execution_admitted(entry, identity)) return false;
        const auto key = entry_key(entry);
        if (!key || (previous && !(*previous < *key))) return false;
        previous = key;
    }
    return true;
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

bool ordered_coverage_view(
    const NativeBringupCoverageDispatchPack& pack) noexcept {
    const auto& identity = pack.identity;
    if (identity.contract_version !=
            native_bringup_coverage_contract_version ||
        !canonical_sha256(identity.authority_identity) ||
        !valid_component(identity.project_id) ||
        !valid_component(identity.project_version) ||
        !canonical_sha256(identity.analysis_identity) ||
        !canonical_sha256(identity.aot_pack_identity) ||
        !canonical_sha256(identity.module_universe_identity) ||
        identity.aot_pack_generation == 0u)
        return false;

    // The three spans are offline lookup/diagnostic material. Runtime
    // execution never consumes their size, storage, row contents, ordering or
    // capability claims, so malformed, stale, duplicated or missing metadata
    // cannot veto an otherwise exact current executable entry. The published
    // maximum constants remain generator-side resource budgets only.
    return true;
}

[[noreturn]] void reject(
    const NativeBringupDispatchPreflightRequest& request,
    const NativeBringupDispatchMiss miss) {
    throw NativeBringupDispatchError(request, miss);
}

const char* transfer_name(const NativeBringupTransferKind transfer) noexcept {
    switch (transfer) {
    case NativeBringupTransferKind::CallRegister:
        return "call-register";
    case NativeBringupTransferKind::TailJumpRegister:
        return "tail-jump-register";
    }
    return "invalid";
}

const char* coverage_source_kind_name(
    const NativeBringupCoverageSourceKind kind) noexcept {
    switch (kind) {
    case NativeBringupCoverageSourceKind::StaticAot:
        return "static-aot";
    case NativeBringupCoverageSourceKind::LoadedAot:
        return "loaded-aot";
    case NativeBringupCoverageSourceKind::RuntimeImage:
        return "runtime-image";
    }
    return "invalid";
}

const char* coverage_owner_kind_name(
    const NativeBringupCoverageOwnerKind kind) noexcept {
    switch (kind) {
    case NativeBringupCoverageOwnerKind::NativeFunctionEntry:
        return "native-function-entry";
    case NativeBringupCoverageOwnerKind::PrimaryStatic:
        return "primary-static";
    case NativeBringupCoverageOwnerKind::RuntimeImage:
        return "runtime-image";
    case NativeBringupCoverageOwnerKind::LoadedAot:
        return "loaded-aot";
    }
    return "invalid";
}

std::uint64_t coverage_observation_hash(
    const NativeBringupCoverageObservation& event) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    const auto append_byte = [&](const std::uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    const auto append_number = [&](std::uint64_t value) {
        for (unsigned index = 0u; index < 8u; ++index) {
            append_byte(static_cast<std::uint8_t>(value));
            value >>= 8u;
        }
    };
    const auto append_string = [&](const std::string_view value) {
        append_number(value.size());
        for (const auto character : value)
            append_byte(static_cast<std::uint8_t>(character));
    };
    append_number(static_cast<std::uint8_t>(event.transfer_kind));
    append_number(event.callsite);
    append_number(event.target);
    append_number(static_cast<std::uint8_t>(event.source_kind));
    append_string(event.source_module_identity);
    append_string(event.source_block_identity);
    append_number(event.source_runtime_start);
    append_number(event.source_module_offset);
    append_number(event.source_lifecycle_generation);
    append_string(event.source_image_id);
    append_number(static_cast<std::uint8_t>(event.target_owner_kind));
    append_string(event.target_module_identity);
    append_string(event.target_image_id);
    append_string(event.target_block_identity);
    append_number(event.target_runtime_start);
    append_number(event.target_module_offset);
    append_number(event.target_lifecycle_generation);
    append_number(event.aot_pack_generation);
    append_number(event.runtime_generation);
    return hash;
}

bool same_coverage_observation_key(
    const NativeBringupCoverageObservation& left,
    const NativeBringupCoverageObservation& right) noexcept {
    return std::tie(
               left.transfer_kind,
               left.callsite,
               left.target,
               left.source_kind,
               left.source_module_identity,
               left.source_block_identity,
               left.source_runtime_start,
               left.source_module_offset,
               left.source_lifecycle_generation,
               left.source_image_id,
               left.target_owner_kind,
               left.target_module_identity,
               left.target_image_id,
               left.target_block_identity,
               left.target_runtime_start,
               left.target_module_offset,
               left.target_lifecycle_generation,
               left.aot_pack_generation,
               left.runtime_generation) ==
           std::tie(
               right.transfer_kind,
               right.callsite,
               right.target,
               right.source_kind,
               right.source_module_identity,
               right.source_block_identity,
               right.source_runtime_start,
               right.source_module_offset,
               right.source_lifecycle_generation,
               right.source_image_id,
               right.target_owner_kind,
               right.target_module_identity,
               right.target_image_id,
               right.target_block_identity,
               right.target_runtime_start,
               right.target_module_offset,
               right.target_lifecycle_generation,
               right.aot_pack_generation,
               right.runtime_generation);
}

bool same_coverage_observation_storage_key(
    const NativeBringupCoverageObservation& left,
    const NativeBringupCoverageObservation& right) noexcept {
    const auto same_storage = [](const std::string_view lhs,
                                 const std::string_view rhs) noexcept {
        return lhs.data() == rhs.data() && lhs.size() == rhs.size();
    };
    return left.transfer_kind == right.transfer_kind &&
           left.callsite == right.callsite && left.target == right.target &&
           left.source_kind == right.source_kind &&
           same_storage(left.source_module_identity,
                        right.source_module_identity) &&
           same_storage(left.source_block_identity,
                        right.source_block_identity) &&
           left.source_runtime_start == right.source_runtime_start &&
           left.source_module_offset == right.source_module_offset &&
           left.source_lifecycle_generation ==
               right.source_lifecycle_generation &&
           same_storage(left.source_image_id, right.source_image_id) &&
           left.target_owner_kind == right.target_owner_kind &&
           same_storage(left.target_module_identity,
                        right.target_module_identity) &&
           same_storage(left.target_image_id, right.target_image_id) &&
           same_storage(left.target_block_identity,
                        right.target_block_identity) &&
           left.target_runtime_start == right.target_runtime_start &&
           left.target_module_offset == right.target_module_offset &&
           left.target_lifecycle_generation ==
               right.target_lifecycle_generation &&
           left.aot_pack_generation == right.aot_pack_generation &&
           left.runtime_generation == right.runtime_generation;
}

} // namespace

NativeBringupDispatchContext::NativeBringupDispatchContext(
    const RuntimeBlockTable& validated_table,
    const NativeBringupDispatchPack& validated_pack,
    const std::uint64_t active_runtime_generation,
    NativeBringupDispatchObservations& active_observations) noexcept
    : pack(validated_pack),
      runtime_generation(active_runtime_generation),
      observations(active_observations),
      validated_allowlist_data_(validated_pack.allowlist.data()),
      validated_allowlist_size_(validated_pack.allowlist.size()),
      validated_identity_(validated_pack.identity),
      validated_table_(&validated_table),
      validated_table_lifetime_(validated_table.dispatch_lifetime()),
      validated_table_generation_(validated_table.dispatch_generation()) {}

bool NativeBringupDispatchContext::validated_static_view_current(
    const RuntimeBlockTable& table) const noexcept {
    const auto same_storage = [](const std::string_view current,
                                 const std::string_view validated) noexcept {
        return current.data() == validated.data() &&
               current.size() == validated.size();
    };
    const auto& current = pack.identity;
    return runtime_generation != 0u && table.static_aot_dispatch_ready() &&
           validated_table_ == &table &&
           table.dispatch_lifetime() == validated_table_lifetime_ &&
           table.dispatch_generation() == validated_table_generation_ &&
           pack.allowlist.data() == validated_allowlist_data_ &&
           pack.allowlist.size() == validated_allowlist_size_ &&
           current.contract_version == validated_identity_.contract_version &&
           same_storage(current.authoring_artifact_identity,
                        validated_identity_.authoring_artifact_identity) &&
           same_storage(current.project_id, validated_identity_.project_id) &&
           same_storage(current.project_version,
                        validated_identity_.project_version) &&
           same_storage(current.analysis_identity,
                        validated_identity_.analysis_identity) &&
           same_storage(current.aot_pack_identity,
                        validated_identity_.aot_pack_identity) &&
           current.aot_pack_generation ==
               validated_identity_.aot_pack_generation;
}

NativeBringupDispatchContext make_native_bringup_dispatch_context(
    const RuntimeBlockTable& table,
    const NativeBringupDispatchPack& pack,
    const std::uint64_t runtime_generation,
    NativeBringupDispatchObservations& observations) {
    if (runtime_generation == 0u || !table.static_aot_dispatch_ready() ||
        !ordered_static_view(pack))
        throw std::invalid_argument(
            "Native bring-up dispatch context lacks a sealed executable pack.");

    BlockVariantKey variant;
    variant.runtime_generation = runtime_generation;
    for (const auto& entry : pack.allowlist) {
        const auto source = table.lookup_static_aot(
            entry.source.block.physical_address,
            entry.source.block.virtual_address,
            variant);
        const auto target = table.lookup_static_aot(
            entry.target.block.physical_address,
            entry.target.block.virtual_address,
            variant);
        if (!execution_matches(table, source, entry.source, variant) ||
            !execution_matches(table, target, entry.target, variant))
            throw std::invalid_argument(
                "Native bring-up dispatch pack is not bound to the sealed "
                "Static-AOT table.");
    }
    return NativeBringupDispatchContext{
        table, pack, runtime_generation, observations};
}

NativeBringupDispatchPreflightResult preflight_native_bringup_dispatch(
    const RuntimeBlockTable& table,
    const NativeBringupDispatchContext& context,
    const NativeBringupDispatchPreflightRequest& request) {
    const auto& pack = context.pack;
    if (!context.validated_static_view_current(table))
        reject(request, NativeBringupDispatchMiss::InvalidEntry);

    const auto requested_key = request_key(request);
    if (!requested_key)
        reject(request, NativeBringupDispatchMiss::UnknownCompiledTarget);
    const auto selected = std::lower_bound(
        pack.allowlist.begin(),
        pack.allowlist.end(),
        *requested_key,
        [](const NativeBringupDispatchEntry& entry, const DispatchKey& key) {
            return *entry_key(entry) < key;
        });
    if (selected == pack.allowlist.end() ||
        *entry_key(*selected) != *requested_key)
        reject(request, NativeBringupDispatchMiss::UnknownCompiledTarget);

    if (request.variant.runtime_generation != context.runtime_generation)
        reject(request, NativeBringupDispatchMiss::GenerationMismatch);
    if (request.source != selected->source.block ||
        request.continuation != selected->admission.continuation)
        reject(request, NativeBringupDispatchMiss::SourceIdentityMismatch);

    const auto source_execution = table.lookup_static_aot(
        selected->source.block.physical_address,
        selected->source.block.virtual_address,
        request.variant);
    if (!execution_matches(
            table, source_execution, selected->source, request.variant))
        reject(request, NativeBringupDispatchMiss::SourceIdentityMismatch);

    const auto physical = selected->target.block.physical_address;
    const auto execution =
        table.lookup_static_aot(physical, request.target, request.variant);
    if (!execution_matches(table, execution, selected->target, request.variant))
        reject(request, NativeBringupDispatchMiss::UnknownCompiledTarget);
    return {execution->block,
            *execution,
            selected->admission.target,
            physical};
}

NativeBringupCoverageDispatchContext::NativeBringupCoverageDispatchContext(
    const RuntimeBlockTable& validated_table,
    NativePortRuntimeImageBindings& validated_runtime_images,
    NativePortLoadedAotBinder& validated_binder,
    const NativeBringupCoverageDispatchPack& validated_pack,
    const std::uint64_t active_runtime_generation,
    NativeBringupCoverageObservations& active_observations) noexcept
    : pack(validated_pack),
      runtime_generation(active_runtime_generation),
      observations(active_observations),
      validated_identity_(validated_pack.identity),
      validated_table_(&validated_table),
      validated_runtime_images_(&validated_runtime_images),
      validated_binder_(&validated_binder),
      validated_table_lifetime_(validated_table.dispatch_lifetime()),
      validated_table_generation_(validated_table.dispatch_generation()) {}

bool NativeBringupCoverageDispatchContext::validated_view_current(
    const RuntimeBlockTable& table,
    const NativePortRuntimeImageBindings& runtime_images,
    const NativePortLoadedAotBinder& binder) const noexcept {
    const auto same_storage = [](const std::string_view current,
                                 const std::string_view validated) noexcept {
        return current.data() == validated.data() &&
               current.size() == validated.size();
    };
    const auto& current = pack.identity;
    return runtime_generation != 0u && table.static_aot_dispatch_ready() &&
           validated_table_ == &table &&
           validated_runtime_images_ == &runtime_images &&
           validated_binder_ == &binder &&
           table.dispatch_lifetime() == validated_table_lifetime_ &&
           table.dispatch_generation() == validated_table_generation_ &&
           current.contract_version == validated_identity_.contract_version &&
           same_storage(current.authority_identity,
                        validated_identity_.authority_identity) &&
           same_storage(current.project_id, validated_identity_.project_id) &&
           same_storage(current.project_version,
                        validated_identity_.project_version) &&
           same_storage(current.analysis_identity,
                        validated_identity_.analysis_identity) &&
           same_storage(current.aot_pack_identity,
                        validated_identity_.aot_pack_identity) &&
           same_storage(current.module_universe_identity,
                        validated_identity_.module_universe_identity) &&
           current.aot_pack_generation ==
               validated_identity_.aot_pack_generation;
}

NativeBringupCoverageDispatchContext
make_native_bringup_coverage_dispatch_context(
    const RuntimeBlockTable& table,
    NativePortRuntimeImageBindings& runtime_images,
    NativePortLoadedAotBinder& binder,
    const NativeBringupCoverageDispatchPack& pack,
    const std::uint64_t runtime_generation,
    NativeBringupCoverageObservations& observations) {
    if (runtime_generation == 0u || !table.static_aot_dispatch_ready() ||
        !ordered_coverage_view(pack))
        throw std::invalid_argument(
            "Native bring-up coverage context lacks a sealed executable "
            "pack.");

    // Coverage rows are optional diagnostics and are deliberately not read.
    // The context binds only the top-level pack identities plus the exact
    // table/runtime-image/binder lifetimes; every dispatch resolves its
    // current source and target owner directly from those authorities.
    return NativeBringupCoverageDispatchContext{
        table, runtime_images, binder, pack, runtime_generation,
        observations};
}

NativeBringupDispatchError::NativeBringupDispatchError(
    const NativeBringupDispatchPreflightRequest& request,
    const NativeBringupDispatchMiss miss)
    : std::runtime_error([&] {
          // Error-only, fixed-width guest values; never expose host pointers.
          std::ostringstream detail;
          detail << "Native bring-up preflight rejected: "
                 << native_bringup_dispatch_miss_name(miss)
                 << std::hex << std::setfill('0')
                 << " source=0x" << std::setw(8) << request.source.virtual_address
                 << " source_physical=0x" << std::setw(8)
                 << request.source.physical_address
                 << " callsite=0x" << std::setw(8) << request.callsite
                 << " target=0x" << std::setw(8) << request.target
                 << " continuation=0x" << std::setw(8) << request.continuation
                 << " transfer=0x" << std::setw(2)
                 << static_cast<std::uint32_t>(request.transfer_kind);
          return detail.str();
      }()),
      miss_(miss),
      transfer_kind_(request.transfer_kind),
      callsite_(request.callsite),
      target_(request.target),
      source_(request.source) {}

NativeBringupDispatchMiss NativeBringupDispatchError::miss() const noexcept {
    return miss_;
}

NativeBringupTransferKind
NativeBringupDispatchError::transfer_kind() const noexcept {
    return transfer_kind_;
}

std::uint32_t NativeBringupDispatchError::callsite() const noexcept {
    return callsite_;
}

std::uint32_t NativeBringupDispatchError::target() const noexcept {
    return target_;
}

BlockAddress NativeBringupDispatchError::source() const noexcept {
    return source_;
}

const char* native_bringup_dispatch_miss_name(
    const NativeBringupDispatchMiss value) noexcept {
    switch (value) {
    case NativeBringupDispatchMiss::None:
        return "none";
    case NativeBringupDispatchMiss::UnknownCompiledTarget:
        return "unknown-compiled-target";
    case NativeBringupDispatchMiss::MissingIdentity:
        return "missing-identity";
    case NativeBringupDispatchMiss::SourceIdentityMismatch:
        return "source-identity-mismatch";
    case NativeBringupDispatchMiss::GenerationMismatch:
        return "generation-mismatch";
    case NativeBringupDispatchMiss::InvalidEntry:
        return "invalid-entry";
    case NativeBringupDispatchMiss::UnmappedTarget:
        return "unmapped-target";
    case NativeBringupDispatchMiss::PhysicalIdentityMismatch:
        return "physical-identity-mismatch";
    case NativeBringupDispatchMiss::CoverageSourceMissing:
        return "coverage-source-missing";
    case NativeBringupDispatchMiss::CoverageTargetMissing:
        return "coverage-target-missing";
    case NativeBringupDispatchMiss::LoadedModuleInactive:
        return "loaded-module-inactive";
    case NativeBringupDispatchMiss::LoadedModuleIdentityMismatch:
        return "loaded-module-identity-mismatch";
    case NativeBringupDispatchMiss::RuntimeImageInactive:
        return "runtime-image-inactive";
    case NativeBringupDispatchMiss::RuntimeImageIdentityMismatch:
        return "runtime-image-identity-mismatch";
    case NativeBringupDispatchMiss::HookReplacementConflict:
        return "hook-replacement-conflict";
    case NativeBringupDispatchMiss::AmbiguousTargetOwner:
        return "ambiguous-target-owner";
    case NativeBringupDispatchMiss::TargetCapabilityMismatch:
        return "target-capability-mismatch";
    }
    return "invalid-entry";
}

NativeBringupCoverageObservations::NativeBringupCoverageObservations() {
    static_assert(
        (native_bringup_coverage_observation_capacity &
         (native_bringup_coverage_observation_capacity - 1u)) == 0u);
    events_.reserve(native_bringup_coverage_observation_capacity);
    event_index_.resize(
        native_bringup_coverage_observation_capacity * 2u,
        std::numeric_limits<std::uint32_t>::max());
}

void NativeBringupCoverageObservations::set_recording_enabled(
    const bool enabled) {
    if (enabled == recording_enabled()) return;
    events_.clear();
    total_occurrences_ = 0u;
    dropped_events_ = 0u;
    if (!enabled) {
        event_index_.clear();
        return;
    }
    event_index_.assign(
        native_bringup_coverage_observation_capacity * 2u,
        std::numeric_limits<std::uint32_t>::max());
}

bool NativeBringupCoverageObservations::recording_enabled() const noexcept {
    return !event_index_.empty();
}

NativeBringupCoverageObservations::EventIndex
NativeBringupCoverageObservations::record(
    const NativeBringupCoverageObservation& observation) noexcept {
    if (!recording_enabled()) return invalid_event_index;
    increment_saturated(total_occurrences_);
    const auto empty = std::numeric_limits<std::uint32_t>::max();
    const auto index_mask = event_index_.size() - 1u;
    auto slot = static_cast<std::size_t>(
        coverage_observation_hash(observation)) & index_mask;
    for (std::size_t probe = 0u; probe < event_index_.size(); ++probe) {
        const auto event_index = event_index_[slot];
        if (event_index == empty) {
            if (events_.size() >=
                native_bringup_coverage_observation_capacity) {
                increment_saturated(dropped_events_);
                return invalid_event_index;
            }
            auto recorded = observation;
            recorded.occurrences = 1u;
            const auto inserted =
                static_cast<std::uint32_t>(events_.size());
            events_.push_back(recorded);
            event_index_[slot] = inserted;
            return inserted;
        }
        auto& event = events_[event_index];
        if (same_coverage_observation_key(event, observation)) {
            increment_saturated(event.occurrences);
            return event_index;
        }
        slot = (slot + 1u) & index_mask;
    }
    increment_saturated(dropped_events_);
    return invalid_event_index;
}

bool NativeBringupCoverageObservations::record_cached(
    const EventIndex index,
    const NativeBringupCoverageObservation& observation) noexcept {
    if (!recording_enabled()) return true;
    if (index >= events_.size() ||
        !same_coverage_observation_storage_key(events_[index], observation))
        return false;
    increment_saturated(total_occurrences_);
    increment_saturated(events_[index].occurrences);
    return true;
}

void NativeBringupCoverageObservations::clear() noexcept {
    events_.clear();
    std::fill(
        event_index_.begin(), event_index_.end(),
        std::numeric_limits<std::uint32_t>::max());
    total_occurrences_ = 0u;
    dropped_events_ = 0u;
}

std::uint64_t NativeBringupCoverageObservations::total_occurrences() const
    noexcept {
    return total_occurrences_;
}

std::uint64_t NativeBringupCoverageObservations::dropped_events() const
    noexcept {
    return dropped_events_;
}

std::span<const NativeBringupCoverageObservation>
NativeBringupCoverageObservations::events() const noexcept {
    return {events_.data(), events_.size()};
}

std::string NativeBringupCoverageObservations::serialize_json() const {
    std::ostringstream output;
    output << "{\"schema\":\"katana-native-bringup-coverage-v2\""
           << ",\"report_version\":2"
           << ",\"report_type\":\"native-bringup-coverage\""
           << ",\"non_release\":true,\"coverage_only\":true"
           << ",\"proof\":\"incomplete\",\"static_proof_promotions\":0"
           << ",\"total_occurrences\":" << total_occurrences_
           << ",\"dropped_events\":" << dropped_events_
           << ",\"events\":[";
    for (std::size_t index = 0u; index < events_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& event = events_[index];
        output << "{\"transfer\":\"" << transfer_name(event.transfer_kind)
               << "\",\"callsite\":" << event.callsite
               << ",\"target\":" << event.target
               << ",\"source_kind\":\""
               << coverage_source_kind_name(event.source_kind)
               << "\",\"source_module_identity\":\""
               << event.source_module_identity
               << "\",\"source_block_identity\":\""
               << event.source_block_identity
               << "\",\"source_runtime_start\":"
               << event.source_runtime_start
               << ",\"source_module_offset\":"
               << event.source_module_offset
               << ",\"source_lifecycle_generation\":"
               << event.source_lifecycle_generation
               << ",\"source_image_id\":\""
               << event.source_image_id
               << "\",\"target_owner_kind\":\""
               << coverage_owner_kind_name(event.target_owner_kind)
               << "\",\"target_module_identity\":\""
               << event.target_module_identity
               << "\",\"target_image_id\":\""
               << event.target_image_id
               << "\",\"target_block_identity\":\""
               << event.target_block_identity
               << "\",\"target_runtime_start\":"
               << event.target_runtime_start
               << ",\"target_module_offset\":"
               << event.target_module_offset
               << ",\"target_lifecycle_generation\":"
               << event.target_lifecycle_generation
               << ",\"aot_pack_generation\":"
               << event.aot_pack_generation
               << ",\"runtime_generation\":"
               << event.runtime_generation
               << ",\"occurrences\":" << event.occurrences << '}';
    }
    output << "]}";
    return output.str();
}

void NativeBringupDispatchObservations::record(
    const bool executed,
    const NativeBringupDispatchMiss miss,
    const NativeBringupTransferKind transfer_kind,
    const std::uint32_t callsite,
    const std::uint32_t target,
    const std::uint64_t aot_pack_generation,
    const std::uint64_t runtime_generation) noexcept {
    increment_saturated(total_occurrences_);
    for (std::size_t index = 0u; index < event_count_; ++index) {
        auto& event = events_[index];
        if (event.executed == executed && event.miss == miss &&
            event.transfer_kind == transfer_kind &&
            event.callsite == callsite && event.target == target &&
            event.aot_pack_generation == aot_pack_generation &&
            event.runtime_generation == runtime_generation) {
            increment_saturated(event.occurrences);
            return;
        }
    }
    if (event_count_ == events_.size()) {
        increment_saturated(dropped_events_);
        return;
    }
    events_[event_count_++] = {executed,
                               miss,
                               transfer_kind,
                               callsite,
                               target,
                               aot_pack_generation,
                               runtime_generation,
                               1u};
}

void NativeBringupDispatchObservations::clear() noexcept {
    events_ = {};
    event_count_ = 0u;
    total_occurrences_ = 0u;
    dropped_events_ = 0u;
}

std::uint64_t NativeBringupDispatchObservations::total_occurrences() const
    noexcept {
    return total_occurrences_;
}

std::uint64_t NativeBringupDispatchObservations::dropped_events() const
    noexcept {
    return dropped_events_;
}

std::span<const NativeBringupDispatchObservation>
NativeBringupDispatchObservations::events() const noexcept {
    return {events_.data(), event_count_};
}

std::string NativeBringupDispatchObservations::serialize_json() const {
    std::ostringstream output;
    output << "{\"schema\":\"katana-native-bringup-dispatch-v1\""
           << ",\"report_version\":1,\"report_type\":\"native-bringup-dispatch\""
           << ",\"non_release\":true,\"proof\":\"incomplete\""
           << ",\"static_proof_promotions\":0"
           << ",\"total_occurrences\":" << total_occurrences_
           << ",\"dropped_events\":" << dropped_events_ << ",\"events\":[";
    for (std::size_t index = 0u; index < event_count_; ++index) {
        if (index != 0u) output << ',';
        const auto& event = events_[index];
        output << "{\"executed\":" << (event.executed ? "true" : "false")
               << ",\"miss\":\"" << native_bringup_dispatch_miss_name(event.miss)
               << "\",\"transfer_kind\":\""
               << transfer_name(event.transfer_kind)
               << "\",\"callsite\":\"0x" << std::hex << std::uppercase
               << std::setw(8) << std::setfill('0') << event.callsite
               << "\",\"target\":\"0x" << std::setw(8) << event.target
               << std::dec << "\",\"aot_pack_generation\":"
               << event.aot_pack_generation
               << ",\"runtime_generation\":" << event.runtime_generation
               << ",\"occurrences\":" << event.occurrences << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace katana::runtime
