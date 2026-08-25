#pragma once

#include "katana/runtime/crash_capsule.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

namespace katana::runtime {

inline constexpr std::string_view closure_witness_v5_schema =
    "katana-closure-witness-v5";
inline constexpr std::string_view closure_witness_v5_version = "5";
inline constexpr std::size_t closure_witness_v5_line_capacity = 64u * 1024u;

struct ClosureWitnessBindingView final {
    std::string_view analysis_artifact_key;
    std::string_view content_identity;
    std::string_view boot_byte_identity;
    std::string_view project_identity;
    std::string_view analysis_contract_identity;
    std::string_view image_analysis_key;
    std::string_view game_project_identity;
    std::string_view native_port_identity;
    std::string_view native_port_artifact_identity;
    std::string_view analysis_implementation_identity;
    std::string_view analysis_cache_implementation_identity;
    std::string_view ir_product_implementation_identity;
    std::string_view codegen_implementation_identity;
    std::uint32_t analyzer_abi = 0u;
    std::uint32_t backend_abi = 0u;
    std::uint32_t analysis_mode = 0u;
    std::uint32_t disc_volume_start_lba = 0u;
    std::uint32_t disc_extent_lba_bias = 0u;
    std::uint64_t runtime_generation = 0u;
};

inline void note_closure_binding_once(
    CrashCapsule& capsule,
    const ClosureWitnessBindingView& view,
    const std::uint64_t runtime_generation,
    const std::uint64_t sequence = 0u) noexcept {
    if (capsule.v5.closure_binding_count != 0u) return;
    CrashCapsuleClosureBinding binding;
    binding.sequence = sequence;
    binding.runtime_generation = runtime_generation;
    binding.analyzer = view.analyzer_abi;
    binding.backend = view.backend_abi;
    binding.mode = view.analysis_mode;
    binding.lba = view.disc_volume_start_lba;
    binding.bias = view.disc_extent_lba_bias;
    binding.key.assign(view.analysis_artifact_key);
    binding.content.assign(view.content_identity);
    binding.boot.assign(view.boot_byte_identity);
    binding.project.assign(view.project_identity);
    binding.analysis_contract.assign(view.analysis_contract_identity);
    binding.image_analysis.assign(view.image_analysis_key);
    binding.game_project.assign(view.game_project_identity);
    binding.native_port.assign(view.native_port_identity);
    binding.native_port_artifact.assign(view.native_port_artifact_identity);
    binding.analysis_impl.assign(view.analysis_implementation_identity);
    binding.analysis_cache_impl.assign(
        view.analysis_cache_implementation_identity);
    binding.ir_product_impl.assign(view.ir_product_implementation_identity);
    binding.codegen_impl.assign(view.codegen_implementation_identity);
    capsule.note_v5_closure_binding(binding);
}

struct ClosureWitnessReferenceView final {
    std::uint32_t address = 0u;
    std::string_view identity;
};

struct ClosureWitnessPointerView final {
    bool address_present = false;
    std::uint32_t address = 0u;
    std::string_view identity;
    std::uint32_t value = 0u;
};

struct ClosureWitnessSlotView final {
    bool slot_present = false;
    std::uint32_t address = 0u;
    std::string_view identity;
};

struct ClosureWitnessFlagsView final {
    bool immutable = false;
    bool bounded = false;
    bool complete = false;
    bool runtime_observation = true;
    bool reproof_required = true;
};

struct ClosureWitnessView final {
    std::string_view kind;
    ClosureWitnessReferenceView source;
    ClosureWitnessReferenceView callsite;
    ClosureWitnessPointerView pointer;
    ClosureWitnessReferenceView target;
    ClosureWitnessReferenceView alias;
    ClosureWitnessSlotView slot;
    ClosureWitnessFlagsView flags;
};

struct ClosureWitnessDocumentView final {
    ClosureWitnessBindingView binding;
    std::span<const ClosureWitnessView> witnesses;
    std::uint64_t drop_count = 0u;
    bool truncated = false;
    bool invalid = false;
};

struct ClosureWitnessSerializedLine final {
    std::array<char, closure_witness_v5_line_capacity> bytes{};
    std::size_t size = 0u;
    bool truncated = false;

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(bytes.data(), size < bytes.size() ? size : bytes.size());
    }
};

// Product code may expose a bounded set of statically identity-bound probe
// candidates, but no candidate is observed unless a diagnostic run explicitly
// selects its exact callsite.  The runtime state is intentionally allocation
// free and one-shot: after the first successful return from an armed indirect
// dispatch, that callsite stops writing to the capsule.  Unarmed product runs
// therefore pay no ring-write or identity-lookup cost.
struct ClosureProbeEligibleSiteView final {
    std::uint32_t owner = 0u;
    std::uint32_t owner_size = 0u;
    std::uint32_t callsite = 0u;
    std::string_view owner_identity;
};

struct ClosureProbePlanEntry final {
    std::uint32_t owner = 0u;
    std::uint32_t owner_size = 0u;
    std::uint32_t callsite = 0u;
    std::uint64_t generation = 0u;
    CrashCapsuleToken owner_identity;
    bool observed = false;
};

class ClosureProbePlanState final {
  public:
    void reset() noexcept {
        entries_ = {};
        count_ = 0u;
        pending_ = 0u;
    }

    [[nodiscard]] bool has_pending() const noexcept { return pending_ != 0u; }
    [[nodiscard]] bool has_pending(
        const std::uint32_t callsite) const noexcept {
        if (pending_ == 0u) return false;
        for (std::size_t index = 0u; index < count_; ++index) {
            const auto& entry = entries_[index];
            if (entry.callsite == callsite && !entry.observed) return true;
        }
        return false;
    }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    // Selection is all-or-nothing.  An unknown, ambiguous, duplicate or
    // over-capacity request arms nothing and marks the diagnostic capsule
    // invalid; it can never become closure evidence by partial acceptance.
    [[nodiscard]] bool configure(
        const std::span<const ClosureProbeEligibleSiteView> eligible,
        const std::span<const std::uint32_t> selected_callsites,
        CrashCapsule* const capsule) noexcept {
        reset();
        if (selected_callsites.empty()) return true;
        if (capsule == nullptr ||
            selected_callsites.size() > entries_.size()) {
            if (capsule != nullptr) capsule->note_v5_invalid();
            return false;
        }

        std::array<ClosureProbePlanEntry,
                   crash_capsule_closure_probe_plan_capacity> prepared{};
        std::size_t prepared_count = 0u;
        for (const auto callsite : selected_callsites) {
            if (callsite == 0u) {
                capsule->note_v5_invalid();
                return false;
            }
            for (std::size_t index = 0u; index < prepared_count; ++index) {
                if (prepared[index].callsite == callsite) {
                    capsule->note_v5_invalid();
                    return false;
                }
            }
            const ClosureProbeEligibleSiteView* match = nullptr;
            for (const auto& candidate : eligible) {
                if (candidate.callsite != callsite) continue;
                if (match != nullptr) {
                    capsule->note_v5_invalid();
                    return false;
                }
                match = &candidate;
            }
            if (match == nullptr || match->owner == 0u ||
                match->owner_size == 0u ||
                match->owner_identity.empty()) {
                capsule->note_v5_invalid();
                return false;
            }
            auto& entry = prepared[prepared_count++];
            entry.owner = match->owner;
            entry.owner_size = match->owner_size;
            entry.callsite = match->callsite;
            entry.generation = 0u;
            entry.owner_identity.assign(match->owner_identity);
            if (entry.owner_identity.flag_bits() !=
                CrashCapsuleTokenFlagNone) {
                capsule->note_v5_invalid();
                return false;
            }
        }

        entries_ = prepared;
        count_ = static_cast<std::uint32_t>(prepared_count);
        pending_ = count_;
        return true;
    }

    // Returns false for every unarmed/already-recorded callsite and performs
    // no capsule writes in that case.  Runtime observations remain explicitly
    // incomplete and require the normal immutable CFG/owner reproof.
    [[nodiscard]] bool note_successful_dispatch(
        const std::uint32_t callsite,
        const std::uint32_t target,
        const std::uint32_t alias,
        const std::uint32_t continuation,
        const std::uint64_t generation,
        const std::string_view target_identity,
        const std::uint64_t sequence,
        CrashCapsule& capsule) noexcept {
        if (pending_ == 0u) return false;
        ClosureProbePlanEntry* match = nullptr;
        for (std::size_t index = 0u; index < count_; ++index) {
            auto& entry = entries_[index];
            if (entry.callsite == callsite && !entry.observed) {
                match = &entry;
                break;
            }
        }
        if (match == nullptr) return false;

        if (generation == 0u || target == 0u || target_identity.empty()) {
            capsule.note_v5_invalid();
            return false;
        }
        CrashCapsuleToken checked_target_identity;
        checked_target_identity.assign(target_identity);
        if (checked_target_identity.flag_bits() !=
            CrashCapsuleTokenFlagNone) {
            capsule.note_v5_invalid();
            return false;
        }
        if (match->generation == 0u) {
            match->generation = generation;
            CrashCapsuleClosureProbePlan plan;
            plan.sequence = sequence;
            plan.generation = generation;
            plan.kind = static_cast<std::uint32_t>(
                CrashCapsuleV5WitnessKind::IndirectDispatch);
            plan.state = 1u;
            plan.source = match->owner;
            plan.callsite = match->callsite;
            plan.witness_limit = 1u;
            plan.identity = match->owner_identity;
            capsule.note_v5_closure_probe_plan(plan);
        } else if (match->generation != generation) {
            capsule.note_v5_invalid();
            return false;
        }

        CrashCapsuleClosureWitness witness;
        witness.sequence = sequence;
        witness.generation = generation;
        witness.kind = static_cast<std::uint32_t>(
            CrashCapsuleV5WitnessKind::IndirectDispatch);
        witness.source = match->owner;
        witness.callsite = match->callsite;
        witness.pointer = alias;
        witness.pointer_present = 1u;
        witness.pointer_value = alias;
        witness.target = target;
        witness.alias = alias;
        witness.continuation = continuation;
        witness.runtime_observation = 1u;
        witness.reproof_required = 1u;
        witness.source_identity = match->owner_identity;
        witness.target_identity = checked_target_identity;
        capsule.note_v5_closure_witness(witness);

        CrashCapsulePointerProvenance pointer;
        pointer.sequence = sequence;
        pointer.generation = generation;
        pointer.pointer_value = alias;
        pointer.source = match->owner;
        pointer.callsite = match->callsite;
        pointer.pointer = alias;
        pointer.pointer_present = 1u;
        pointer.target = target;
        pointer.alias = alias;
        pointer.source_identity = match->owner_identity;
        pointer.target_identity = checked_target_identity;
        pointer.pointer_identity = checked_target_identity;
        capsule.note_v5_pointer_provenance(pointer);

        CrashCapsuleDispatchWitness dispatch;
        dispatch.sequence = sequence;
        dispatch.generation = generation;
        dispatch.kind = static_cast<std::uint32_t>(
            CrashCapsuleV5WitnessKind::IndirectDispatch);
        dispatch.source = match->owner;
        dispatch.callsite = match->callsite;
        dispatch.target = target;
        dispatch.alias = alias;
        dispatch.continuation = continuation;
        dispatch.source_identity = match->owner_identity;
        dispatch.target_identity = checked_target_identity;
        capsule.note_v5_dispatch_witness(dispatch);

        CrashCapsuleGuestAotCall call;
        call.sequence = sequence;
        call.generation = generation;
        call.caller = match->owner;
        call.callsite = match->callsite;
        call.callee = target;
        call.continuation = continuation;
        call.source = match->owner;
        call.target = target;
        call.caller_identity = match->owner_identity;
        call.callee_identity = checked_target_identity;
        capsule.note_v5_guest_aot_call(call);

        match->observed = true;
        --pending_;
        return true;
    }

  private:
    std::array<ClosureProbePlanEntry,
               crash_capsule_closure_probe_plan_capacity> entries_{};
    std::uint32_t count_ = 0u;
    std::uint32_t pending_ = 0u;
};

namespace closure_witness_detail {

struct Writer final {
    ClosureWitnessSerializedLine& line;

    void append(const std::string_view value) noexcept {
        if (line.truncated) return;
        if (line.size > line.bytes.size() ||
            value.size() > line.bytes.size() - line.size) {
            line.truncated = true;
            return;
        }
        for (const auto byte : value) line.bytes[line.size++] = byte;
    }

    void character(const char value) noexcept {
        if (line.truncated) return;
        if (line.size == line.bytes.size()) {
            line.truncated = true;
            return;
        }
        line.bytes[line.size++] = value;
    }

    void boolean(const bool value) noexcept { append(value ? "true" : "false"); }

    template <typename Integer>
    void decimal(const Integer value) noexcept {
        std::array<char, 32u> buffer{};
        const auto converted = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(), value, 10);
        if (converted.ec != std::errc{}) {
            line.truncated = true;
            return;
        }
        append(std::string_view(
            buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }

    void quoted(const std::string_view value) noexcept {
        character('"');
        for (const auto raw : value) {
            const auto byte = static_cast<unsigned char>(raw);
            switch (raw) {
            case '"': append("\\\""); break;
            case '\\': append("\\\\"); break;
            case '\b': append("\\b"); break;
            case '\f': append("\\f"); break;
            case '\n': append("\\n"); break;
            case '\r': append("\\r"); break;
            case '\t': append("\\t"); break;
            default:
                if (byte < 0x20u) {
                    line.truncated = true;
                    return;
                }
                character(raw);
                break;
            }
        }
        character('"');
    }

    template <typename Integer>
    void quoted_decimal(const Integer value) noexcept {
        character('"');
        decimal(value);
        character('"');
    }
};

inline void key(Writer& writer, const std::string_view value) noexcept {
    writer.quoted(value);
    writer.character(':');
}

inline void comma_key(Writer& writer, const std::string_view value) noexcept {
    writer.character(',');
    key(writer, value);
}

inline void reference(
    Writer& writer, const ClosureWitnessReferenceView& value) noexcept {
    writer.character('{');
    key(writer, "address");
    writer.quoted_decimal(value.address);
    comma_key(writer, "identity");
    writer.quoted(value.identity);
    writer.character('}');
}

} // namespace closure_witness_detail

[[nodiscard]] inline ClosureWitnessSerializedLine serialize_closure_witness_v5(
    const ClosureWitnessDocumentView& document) noexcept {
    ClosureWitnessSerializedLine result;
    closure_witness_detail::Writer writer{result};
    using namespace closure_witness_detail;

    writer.character('{');
    key(writer, "schema"); writer.quoted(closure_witness_v5_schema);
    comma_key(writer, "version"); writer.quoted(closure_witness_v5_version);
    comma_key(writer, "binding"); writer.character('{');
    key(writer, "analysis_artifact_key");
    writer.quoted(document.binding.analysis_artifact_key);
    comma_key(writer, "content_identity");
    writer.quoted(document.binding.content_identity);
    comma_key(writer, "boot_byte_identity");
    writer.quoted(document.binding.boot_byte_identity);
    comma_key(writer, "project_identity");
    writer.quoted(document.binding.project_identity);
    comma_key(writer, "analysis_contract_identity");
    writer.quoted(document.binding.analysis_contract_identity);
    comma_key(writer, "image_analysis_key");
    writer.quoted(document.binding.image_analysis_key);
    comma_key(writer, "game_project_identity");
    writer.quoted(document.binding.game_project_identity);
    comma_key(writer, "native_port_identity");
    writer.quoted(document.binding.native_port_identity);
    comma_key(writer, "native_port_artifact_identity");
    writer.quoted(document.binding.native_port_artifact_identity);
    comma_key(writer, "analysis_implementation_identity");
    writer.quoted(document.binding.analysis_implementation_identity);
    comma_key(writer, "analysis_cache_implementation_identity");
    writer.quoted(document.binding.analysis_cache_implementation_identity);
    comma_key(writer, "ir_product_implementation_identity");
    writer.quoted(document.binding.ir_product_implementation_identity);
    comma_key(writer, "codegen_implementation_identity");
    writer.quoted(document.binding.codegen_implementation_identity);
    comma_key(writer, "analyzer_abi");
    writer.quoted_decimal(document.binding.analyzer_abi);
    comma_key(writer, "backend_abi");
    writer.quoted_decimal(document.binding.backend_abi);
    comma_key(writer, "analysis_mode");
    writer.quoted_decimal(document.binding.analysis_mode);
    comma_key(writer, "disc_volume_start_lba");
    writer.quoted_decimal(document.binding.disc_volume_start_lba);
    comma_key(writer, "disc_extent_lba_bias");
    writer.quoted_decimal(document.binding.disc_extent_lba_bias);
    comma_key(writer, "runtime_generation");
    writer.quoted_decimal(document.binding.runtime_generation);
    writer.character('}');

    comma_key(writer, "witnesses"); writer.character('[');
    for (std::size_t index = 0u; index < document.witnesses.size(); ++index) {
        if (index != 0u) writer.character(',');
        const auto& witness = document.witnesses[index];
        writer.character('{');
        key(writer, "kind"); writer.quoted(witness.kind);
        comma_key(writer, "source"); reference(writer, witness.source);
        comma_key(writer, "callsite"); reference(writer, witness.callsite);
        comma_key(writer, "pointer"); writer.character('{');
        key(writer, "address_present");
        writer.boolean(witness.pointer.address_present);
        comma_key(writer, "address");
        if (witness.pointer.address_present)
            writer.quoted_decimal(witness.pointer.address);
        else
            writer.quoted({});
        comma_key(writer, "identity");
        writer.quoted(witness.pointer.address_present
                          ? witness.pointer.identity
                          : std::string_view{});
        comma_key(writer, "value"); writer.quoted_decimal(witness.pointer.value);
        writer.character('}');
        comma_key(writer, "target"); reference(writer, witness.target);
        comma_key(writer, "alias"); reference(writer, witness.alias);
        comma_key(writer, "slot"); writer.character('{');
        key(writer, "slot_present"); writer.boolean(witness.slot.slot_present);
        comma_key(writer, "address");
        if (witness.slot.slot_present)
            writer.quoted_decimal(witness.slot.address);
        else
            writer.quoted({});
        comma_key(writer, "identity");
        writer.quoted(witness.slot.slot_present
                          ? witness.slot.identity
                          : std::string_view{});
        writer.character('}');
        comma_key(writer, "flags"); writer.character('{');
        key(writer, "immutable"); writer.boolean(witness.flags.immutable);
        comma_key(writer, "bounded"); writer.boolean(witness.flags.bounded);
        comma_key(writer, "complete");
        writer.boolean(witness.flags.complete && document.drop_count == 0u &&
                       !document.truncated && !document.invalid);
        comma_key(writer, "runtime_observation");
        writer.boolean(witness.flags.runtime_observation);
        comma_key(writer, "reproof_required");
        writer.boolean(witness.flags.reproof_required);
        writer.character('}');
        writer.character('}');
    }
    writer.character(']');
    comma_key(writer, "drop_count"); writer.quoted_decimal(document.drop_count);
    comma_key(writer, "truncated"); writer.boolean(document.truncated);
    comma_key(writer, "invalid"); writer.boolean(document.invalid);
    comma_key(writer, "closure_admitted"); writer.boolean(false);
    writer.character('}');
    return result;
}

} // namespace katana::runtime
