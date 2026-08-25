#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <string_view>
#include <type_traits>

namespace katana::runtime {

inline constexpr std::uint32_t crash_capsule_contract_version = 1u;
// Version 1 remains the wire contract used by existing generated callers.  The v2
// extension is additive and is emitted only through the bounded v2 serializer below.
inline constexpr std::uint32_t crash_capsule_v2_contract_version = 2u;
inline constexpr std::uint32_t crash_capsule_v3_contract_version = 3u;
// Version 4 adds bounded executable-lifecycle and lookahead records.  Keeping
// this distinct from v3 lets strict consumers reject unknown keys instead of
// silently accepting a structurally different line under the old contract.
inline constexpr std::uint32_t crash_capsule_v4_contract_version = 4u;
// Version 5 is an additive, bounded witness contract.  It carries the
// provenance needed to review a runtime observation without treating that
// observation as a static closure proof.  The recording structures below are
// deliberately fixed-size PODs so they are safe on a fault path.
inline constexpr std::uint32_t crash_capsule_v5_contract_version = 5u;
inline constexpr std::size_t crash_capsule_event_capacity = 64u;
inline constexpr std::size_t crash_capsule_token_capacity = 512u;
inline constexpr std::size_t crash_capsule_v2_line_capacity = 32768u;
inline constexpr std::size_t crash_capsule_v4_line_capacity = 65536u;
inline constexpr std::size_t crash_capsule_memory_window_capacity = 20u;
inline constexpr std::size_t crash_capsule_memory_window_word_capacity = 16u;
inline constexpr std::size_t crash_capsule_lookahead_capacity = 8u;
inline constexpr std::size_t crash_capsule_module_lifecycle_capacity = 8u;
inline constexpr std::size_t crash_capsule_v5_line_capacity = 131072u;
inline constexpr std::size_t crash_capsule_closure_binding_capacity = 8u;
inline constexpr std::size_t crash_capsule_closure_witness_capacity = 24u;
inline constexpr std::size_t crash_capsule_pointer_provenance_capacity = 32u;
inline constexpr std::size_t crash_capsule_closure_probe_plan_capacity = 8u;
inline constexpr std::size_t crash_capsule_dispatch_witness_capacity = 16u;
inline constexpr std::size_t crash_capsule_loaded_aot_digest_capacity = 8u;
inline constexpr std::size_t crash_capsule_provider_transcript_capacity = 32u;
inline constexpr std::size_t crash_capsule_stall_snapshot_capacity = 8u;
inline constexpr std::size_t crash_capsule_guest_aot_callchain_capacity = 16u;
static_assert((crash_capsule_event_capacity &
               (crash_capsule_event_capacity - 1u)) == 0u);

enum class CrashCapsuleEventKind : std::uint8_t {
    Block = 1u,
    Mmio = 2u,
    Scheduler = 3u,
    Error = 4u,
};

struct CrashCapsuleEvent {
    std::uint64_t guest_cycle = 0u;
    std::uint64_t detail = 0u;
    std::uint32_t pc = 0u;
    std::uint32_t subject = 0u;
    std::uint32_t auxiliary = 0u;
    std::uint16_t code = 0u;
    CrashCapsuleEventKind kind = CrashCapsuleEventKind::Block;
    std::uint8_t flags = 0u;
};

enum CrashCapsuleV2Field : std::uint32_t {
    CrashCapsuleV2FieldNone = 0u,
    CrashCapsuleV2FieldHostException = 1u << 0u,
    CrashCapsuleV2FieldContract = 1u << 1u,
    CrashCapsuleV2FieldCpu = 1u << 2u,
    CrashCapsuleV2FieldDispatch = 1u << 3u,
    CrashCapsuleV2FieldRuntimeModule = 1u << 4u,
    CrashCapsuleV2FieldSourceModule = 1u << 5u,
    CrashCapsuleV2FieldTransition = 1u << 6u,
    CrashCapsuleV2FieldWait = 1u << 7u,
};

enum CrashCapsuleV3Field : std::uint32_t {
    CrashCapsuleV3FieldNone = 0u,
    CrashCapsuleV3FieldCpuState = 1u << 0u,
    CrashCapsuleV3FieldContractDetail = 1u << 1u,
    CrashCapsuleV3FieldLoadedAot = 1u << 2u,
    CrashCapsuleV3FieldInputTrace = 1u << 3u,
    CrashCapsuleV3FieldProductState = 1u << 4u,
    CrashCapsuleV3FieldPlatform = 1u << 5u,
    CrashCapsuleV3FieldMemoryWindows = 1u << 6u,
    CrashCapsuleV3FieldRuntimeModuleLifecycle = 1u << 7u,
    CrashCapsuleV3FieldLookahead = 1u << 8u,
};

enum class CrashCapsuleLookaheadKind : std::uint32_t {
    RuntimeModuleDecode = 1u,
    RuntimeModuleStage = 2u,
    LoadedAotTransfer = 3u,
    AudioRouteSelection = 4u,
    AdxStreamStart = 5u,
    SoundBankCommand = 6u,
};

enum class CrashCapsuleLookaheadState : std::uint32_t {
    Planned = 1u,
    Validated = 2u,
    Committed = 3u,
    Completed = 4u,
    Cancelled = 5u,
};

enum CrashCapsuleTokenFlag : std::uint8_t {
    CrashCapsuleTokenFlagNone = 0u,
    CrashCapsuleTokenFlagTruncated = 1u << 0u,
    CrashCapsuleTokenFlagInvalid = 1u << 1u,
};

// Bounded, path-free text copied from an already validated runtime identity.  The
// v2 capture path never stores arbitrary exception messages or filesystem paths.
struct CrashCapsuleToken {
    void assign(const std::string_view value) noexcept {
        bytes.fill('\0');
        size = 0u;
        flags = CrashCapsuleTokenFlagNone;
        constexpr auto maximum = crash_capsule_token_capacity - 1u;
        const auto count = value.size() < maximum ? value.size() : maximum;
        if (value.size() > maximum) flags |= CrashCapsuleTokenFlagTruncated;
        for (std::size_t index = 0u; index < count; ++index) {
            const auto character = value[index];
            const bool alpha = (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z');
            const bool digit = character >= '0' && character <= '9';
            const bool drive_relative_prefix =
                character == ':' && index == 1u &&
                ((value[0u] >= 'a' && value[0u] <= 'z') ||
                 (value[0u] >= 'A' && value[0u] <= 'Z'));
            const bool namespace_separator =
                character == ':' && !drive_relative_prefix;
            if (!alpha && !digit && character != '_' && character != '-' &&
                character != '.' && !namespace_separator) {
                bytes.fill('\0');
                size = 0u;
                flags = static_cast<std::uint8_t>(flags | CrashCapsuleTokenFlagInvalid);
                return;
            }
            bytes[index] = character;
        }
        size = static_cast<std::uint16_t>(count);
        bytes[size] = '\0';
    }

    // Contract diagnostics are still path-free and JSON-token-safe, but they
    // need a slightly wider grammar than identities: exact native contract
    // failures carry bounded address/value tuples separated by '=', ';' and
    // punctuation. Keep quotes, path separators and controls fail-closed.
    void assign_diagnostic(const std::string_view value) noexcept {
        bytes.fill('\0');
        size = 0u;
        flags = CrashCapsuleTokenFlagNone;
        constexpr auto maximum = crash_capsule_token_capacity - 1u;
        const auto count = value.size() < maximum ? value.size() : maximum;
        if (value.size() > maximum) flags |= CrashCapsuleTokenFlagTruncated;
        for (std::size_t index = 0u; index < count; ++index) {
            const auto character = value[index];
            const bool alpha = (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z');
            const bool digit = character >= '0' && character <= '9';
            const bool punctuation =
                character == '_' || character == '-' || character == '.' ||
                character == ':' || character == '=' || character == ';' ||
                character == ',' || character == '+' || character == '@' ||
                character == '[' || character == ']' || character == '(' ||
                character == ')' || character == '#' || character == '%' ||
                character == ' ';
            if (!alpha && !digit && !punctuation) {
                bytes.fill('\0');
                size = 0u;
                flags = static_cast<std::uint8_t>(
                    flags | CrashCapsuleTokenFlagInvalid);
                return;
            }
            bytes[index] = character;
        }
        size = static_cast<std::uint16_t>(count);
        bytes[size] = '\0';
    }

    [[nodiscard]] std::string_view view() const noexcept {
        const auto bounded_size = size < bytes.size() ? size : bytes.size();
        return std::string_view(bytes.data(), bounded_size);
    }

    [[nodiscard]] std::uint8_t flag_bits() const noexcept {
        return flags;
    }

  private:
    std::array<char, crash_capsule_token_capacity> bytes{};
    std::uint16_t size = 0u;
    std::uint8_t flags = CrashCapsuleTokenFlagNone;
};

// Additive v2 metadata. All values are supplied by a caller that already owns the
// corresponding runtime state; this type performs no address or identity inference.
struct CrashCapsuleV2Fields {
    std::uint32_t present = CrashCapsuleV2FieldNone;
    std::uint32_t host_exception_code = 0u;
    std::uint32_t contract_code = 0u;
    std::uint32_t guest_pc = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t active_callsite = 0u;
    std::uint32_t active_entry = 0u;
    std::uint32_t wait_state = 0u;
    std::uint32_t transition_kind = 0u;
    std::uint32_t transition_target = 0u;
    std::uint64_t runtime_generation = 0u;
    std::uint64_t source_generation = 0u;
    std::uint64_t source_relocation_generation = 0u;
    std::uint64_t transition_sequence = 0u;
    std::uint64_t scheduler_cycle = 0u;
    std::uint64_t pending_event_count = 0u;
    std::uint64_t next_event_cycle = 0u;
    CrashCapsuleToken host_exception_type;
    CrashCapsuleToken contract_type;
    CrashCapsuleToken runtime_module_identity;
    CrashCapsuleToken source_module_identity;
    CrashCapsuleToken transition_identity;
    std::uint8_t host_exception_latched = 0u;
    std::uint8_t reserved[3u]{};

    void note_host_exception(const std::uint32_t code,
                             const std::string_view type) noexcept {
        if (host_exception_latched != 0u) return;
        host_exception_code = code;
        host_exception_type.assign(type);
        host_exception_latched = 1u;
        present |= CrashCapsuleV2FieldHostException;
    }

    void note_contract(const std::uint32_t code,
                       const std::string_view type) noexcept {
        contract_code = code;
        contract_type.assign(type);
        present |= CrashCapsuleV2FieldContract;
    }

    void note_cpu(const std::uint32_t pc, const std::uint32_t procedure_register) noexcept {
        guest_pc = pc;
        pr = procedure_register;
        present |= CrashCapsuleV2FieldCpu;
    }

    void note_dispatch(const std::uint32_t callsite, const std::uint32_t entry) noexcept {
        active_callsite = callsite;
        active_entry = entry;
        present |= CrashCapsuleV2FieldDispatch;
    }

    void note_runtime_module(const std::string_view identity,
                             const std::uint64_t generation) noexcept {
        runtime_module_identity.assign(identity);
        runtime_generation = generation;
        present |= CrashCapsuleV2FieldRuntimeModule;
    }

    void note_source_module(const std::string_view identity,
                            const std::uint64_t generation,
                            const std::uint64_t relocation_generation = 0u) noexcept {
        source_module_identity.assign(identity);
        source_generation = generation;
        source_relocation_generation = relocation_generation;
        present |= CrashCapsuleV2FieldSourceModule;
    }

    void note_transition(const std::uint32_t kind,
                         const std::uint64_t sequence,
                         const std::uint32_t target,
                         const std::string_view identity = {}) noexcept {
        transition_kind = kind;
        transition_sequence = sequence;
        transition_target = target;
        transition_identity.assign(identity);
        present |= CrashCapsuleV2FieldTransition;
    }

    void note_wait(const std::uint32_t state,
                   const std::uint64_t cycle,
                   const std::uint64_t pending_events,
                   const std::uint64_t next_cycle = 0u) noexcept {
        wait_state = state;
        scheduler_cycle = cycle;
        pending_event_count = pending_events;
        next_event_cycle = next_cycle;
        present |= CrashCapsuleV2FieldWait;
    }
};

struct CrashCapsuleMemoryWindow {
    std::uint32_t guest_focus = 0u;
    std::uint32_t guest_base = 0u;
    std::uint32_t physical_base = 0u;
    std::uint32_t source_mask = 0u;
    std::uint32_t valid_word_mask = 0u;
    std::array<std::uint32_t, crash_capsule_memory_window_word_capacity> words{};
};

// Bounded intent captured before a risky runtime transition begins.  It is a
// diagnostic look-ahead, not authority to execute the target: the consumer
// still has to validate the ordinary image, generation and block contracts.
struct CrashCapsuleLookahead final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint32_t kind = 0u;
    std::uint32_t state = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    std::uint32_t continuation = 0u;
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t source_offset = 0u;
    std::uint32_t byte_size = 0u;
    std::array<std::uint32_t, 4u> arguments{};
    CrashCapsuleToken identity;
};

struct CrashCapsuleRuntimeModuleLifecycle final {
    std::uint64_t sequence = 0u;
    std::uint64_t lifecycle_generation = 0u;
    std::uint64_t retirement_generation = 0u;
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t module_size = 0u;
    std::uint32_t state = 0u;
    CrashCapsuleToken identity;
};

// Additive v3 flight-recorder state. Hot paths write only fixed-width values;
// text remains bounded and path-free. Contract-detail hashes cover only the
// bounded, sanitized token so arbitrary exception text is never retained even
// indirectly by the capsule path.
struct CrashCapsuleV3Fields {
    std::uint32_t present = CrashCapsuleV3FieldNone;
    std::array<std::uint32_t, 16u> gpr{};
    std::uint32_t sr = 0u;
    std::uint32_t gbr = 0u;
    std::uint32_t vbr = 0u;
    std::uint32_t mach = 0u;
    std::uint32_t macl = 0u;
    std::uint32_t fpul = 0u;
    std::uint32_t fpscr = 0u;
    std::uint32_t active_instruction_physical = 0u;
    std::uint32_t active_block_physical = 0u;
    std::uint32_t active_block_size = 0u;
    std::uint32_t last_exception_instruction = 0u;
    std::uint32_t last_exception_physical = 0u;
    std::uint32_t last_exception_owner = 0u;
    std::uint32_t loaded_aot_target = 0u;
    std::uint32_t loaded_aot_runtime_start = 0u;
    std::uint32_t loaded_aot_source_start = 0u;
    std::uint32_t loaded_aot_source_offset = 0u;
    std::uint32_t loaded_aot_module_size = 0u;
    std::uint32_t loaded_aot_block_size = 0u;
    std::uint32_t runtime_module_source_start = 0u;
    std::uint32_t runtime_module_runtime_start = 0u;
    std::uint32_t runtime_module_size = 0u;
    // 1 = staged, 2 = active, 3 = retired. Zero means unavailable.
    std::uint32_t runtime_module_lifecycle_state = 0u;
    std::uint32_t stop_reason = 0u;
    std::uint32_t bootstrap_phase = 0u;
    std::uint32_t input_trace_mode = 0u;
    std::uint32_t cpu_flags = 0u;
    std::uint64_t attempted_guest_instructions = 0u;
    std::uint64_t retired_guest_instructions = 0u;
    std::uint64_t total_guest_cycles = 0u;
    std::uint64_t pending_guest_cycles = 0u;
    std::uint64_t exception_generation = 0u;
    std::uint64_t loaded_aot_expected_generation = 0u;
    std::uint64_t loaded_aot_current_generation = 0u;
    std::uint64_t runtime_module_lifecycle_generation = 0u;
    std::uint64_t runtime_module_retirement_generation = 0u;
    std::uint64_t frame_index = 0u;
    std::uint64_t input_polls = 0u;
    std::uint64_t input_connection_generation = 0u;
    std::uint64_t presented_frames = 0u;
    std::uint64_t contract_detail_hash = 0u;
    std::uint64_t input_trace_hash = 0u;
    CrashCapsuleToken contract_detail;
    CrashCapsuleToken loaded_aot_identity;
    CrashCapsuleToken runtime_module_lifecycle_identity;
    CrashCapsuleToken input_trace;
    CrashCapsuleToken input_identity;
    std::array<CrashCapsuleMemoryWindow,
               crash_capsule_memory_window_capacity> memory_windows{};
    std::uint32_t memory_window_count = 0u;
    std::array<CrashCapsuleLookahead,
               crash_capsule_lookahead_capacity> lookahead{};
    std::uint64_t lookahead_sequence = 0u;
    std::uint32_t lookahead_count = 0u;
    std::uint32_t next_lookahead = 0u;
    std::array<CrashCapsuleRuntimeModuleLifecycle,
               crash_capsule_module_lifecycle_capacity>
        runtime_module_lifecycles{};
    std::uint64_t runtime_module_lifecycle_sequence = 0u;
    std::uint32_t runtime_module_lifecycle_count = 0u;
    std::uint32_t next_runtime_module_lifecycle = 0u;

    static constexpr std::uint64_t hash_text(
        const std::string_view value) noexcept {
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto character : value) {
            hash ^= static_cast<std::uint8_t>(character);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    void note_cpu_state(
        const std::array<std::uint32_t, 16u>& registers,
        const std::uint32_t status_register,
        const bool t,
        const bool sleeping,
        const bool trap_pending,
        const std::uint32_t global_base,
        const std::uint32_t vector_base,
        const std::uint32_t multiply_high,
        const std::uint32_t multiply_low,
        const std::uint32_t floating_communication,
        const std::uint32_t floating_status,
        const std::uint32_t instruction_physical,
        const std::uint32_t block_physical,
        const std::uint32_t block_size,
        const std::uint64_t attempted,
        const std::uint64_t retired,
        const std::uint64_t total_cycles,
        const std::uint64_t pending_cycles,
        const std::uint64_t exception_epoch,
        const std::uint32_t exception_instruction,
        const std::uint32_t exception_physical,
        const std::uint32_t exception_owner) noexcept {
        gpr = registers;
        sr = status_register;
        gbr = global_base;
        vbr = vector_base;
        mach = multiply_high;
        macl = multiply_low;
        fpul = floating_communication;
        fpscr = floating_status;
        active_instruction_physical = instruction_physical;
        active_block_physical = block_physical;
        active_block_size = block_size;
        attempted_guest_instructions = attempted;
        retired_guest_instructions = retired;
        total_guest_cycles = total_cycles;
        pending_guest_cycles = pending_cycles;
        exception_generation = exception_epoch;
        last_exception_instruction = exception_instruction;
        last_exception_physical = exception_physical;
        last_exception_owner = exception_owner;
        cpu_flags = (t ? 1u : 0u) | (sleeping ? 1u << 1u : 0u) |
                    (trap_pending ? 1u << 2u : 0u);
        present |= CrashCapsuleV3FieldCpuState;
    }

    void note_contract_detail(const std::string_view detail) noexcept {
        contract_detail.assign_diagnostic(detail);
        contract_detail_hash = hash_text(contract_detail.view());
        present |= CrashCapsuleV3FieldContractDetail;
    }

    void note_loaded_aot(
        const std::uint32_t target,
        const std::uint32_t runtime_start,
        const std::uint32_t source_start,
        const std::uint32_t source_offset,
        const std::uint32_t module_size,
        const std::uint32_t block_size,
        const std::uint64_t expected_generation,
        const std::uint64_t current_generation,
        const std::string_view identity) noexcept {
        loaded_aot_target = target;
        loaded_aot_runtime_start = runtime_start;
        loaded_aot_source_start = source_start;
        loaded_aot_source_offset = source_offset;
        loaded_aot_module_size = module_size;
        loaded_aot_block_size = block_size;
        loaded_aot_expected_generation = expected_generation;
        loaded_aot_current_generation = current_generation;
        loaded_aot_identity.assign(identity);
        present |= CrashCapsuleV3FieldLoadedAot;
    }

    void note_runtime_module_lifecycle(
        const std::string_view identity,
        const std::uint32_t source_start,
        const std::uint32_t runtime_start,
        const std::uint32_t module_size,
        const std::uint64_t lifecycle_generation,
        const std::uint64_t retirement_generation,
        const std::uint32_t lifecycle_state) noexcept {
        runtime_module_lifecycle_identity.assign(identity);
        runtime_module_source_start = source_start;
        runtime_module_runtime_start = runtime_start;
        runtime_module_size = module_size;
        runtime_module_lifecycle_generation = lifecycle_generation;
        runtime_module_retirement_generation = retirement_generation;
        runtime_module_lifecycle_state = lifecycle_state;
        if (runtime_module_lifecycle_sequence ==
            std::numeric_limits<std::uint64_t>::max()) {
            for (auto& item : runtime_module_lifecycles) item = {};
            runtime_module_lifecycle_sequence = 0u;
            runtime_module_lifecycle_count = 0u;
            next_runtime_module_lifecycle = 0u;
        }
        auto& item =
            runtime_module_lifecycles[next_runtime_module_lifecycle];
        item = {};
        item.sequence = ++runtime_module_lifecycle_sequence;
        item.lifecycle_generation = lifecycle_generation;
        item.retirement_generation = retirement_generation;
        item.source_start = source_start;
        item.runtime_start = runtime_start;
        item.module_size = module_size;
        item.state = lifecycle_state;
        item.identity.assign(identity);
        next_runtime_module_lifecycle =
            (next_runtime_module_lifecycle + 1u) %
            crash_capsule_module_lifecycle_capacity;
        if (runtime_module_lifecycle_count <
            crash_capsule_module_lifecycle_capacity)
            ++runtime_module_lifecycle_count;
        present |= CrashCapsuleV3FieldRuntimeModuleLifecycle;
    }

    [[nodiscard]] std::uint64_t begin_lookahead(
        const CrashCapsuleLookaheadKind kind,
        const std::uint32_t callsite,
        const std::uint32_t target,
        const std::uint32_t continuation,
        const std::uint32_t source_start,
        const std::uint32_t runtime_start,
        const std::uint32_t source_offset,
        const std::uint32_t byte_size,
        const std::uint64_t generation,
        const std::string_view identity,
        const std::array<std::uint32_t, 4u> arguments = {}) noexcept {
        if (lookahead_sequence == std::numeric_limits<std::uint64_t>::max()) {
            for (auto& item : lookahead) item = {};
            lookahead_sequence = 0u;
            lookahead_count = 0u;
            next_lookahead = 0u;
        }
        const auto sequence = ++lookahead_sequence;
        auto& item = lookahead[next_lookahead];
        item = {};
        item.sequence = sequence;
        item.generation = generation;
        item.kind = static_cast<std::uint32_t>(kind);
        item.state = static_cast<std::uint32_t>(
            CrashCapsuleLookaheadState::Planned);
        item.callsite = callsite;
        item.target = target;
        item.continuation = continuation;
        item.source_start = source_start;
        item.runtime_start = runtime_start;
        item.source_offset = source_offset;
        item.byte_size = byte_size;
        item.arguments = arguments;
        item.identity.assign(identity);
        next_lookahead =
            (next_lookahead + 1u) % crash_capsule_lookahead_capacity;
        if (lookahead_count < crash_capsule_lookahead_capacity)
            ++lookahead_count;
        present |= CrashCapsuleV3FieldLookahead;
        return sequence;
    }

    void update_lookahead(
        const std::uint64_t sequence,
        const CrashCapsuleLookaheadState state) noexcept {
        if (sequence == 0u) return;
        for (auto& item : lookahead) {
            if (item.sequence != sequence) continue;
            const auto current = static_cast<CrashCapsuleLookaheadState>(
                item.state);
            if (current == CrashCapsuleLookaheadState::Completed ||
                current == CrashCapsuleLookaheadState::Cancelled)
                return;
            if (state == CrashCapsuleLookaheadState::Cancelled) {
                item.state = static_cast<std::uint32_t>(state);
                return;
            }
            if (state != CrashCapsuleLookaheadState::Validated &&
                state != CrashCapsuleLookaheadState::Committed &&
                state != CrashCapsuleLookaheadState::Completed)
                return;
            const auto next_value = static_cast<std::uint32_t>(state);
            if (state == CrashCapsuleLookaheadState::Planned ||
                next_value <= item.state)
                return;
            item.state = next_value;
            return;
        }
    }

    void note_input_trace(const std::string_view trace,
                          const std::string_view identity,
                          const std::uint32_t mode) noexcept {
        input_trace.assign(trace);
        input_identity.assign(identity);
        input_trace_hash = hash_text(trace);
        input_trace_mode = mode;
        present |= CrashCapsuleV3FieldInputTrace;
    }

    void note_product_state(const std::uint32_t stop,
                            const std::uint32_t phase,
                            const std::uint64_t frame,
                            const std::uint64_t presented) noexcept {
        stop_reason = stop;
        bootstrap_phase = phase;
        frame_index = frame;
        presented_frames = presented;
        present |= CrashCapsuleV3FieldProductState;
    }

    void note_platform(const std::uint64_t polls,
                       const std::uint64_t connection_generation) noexcept {
        input_polls = polls;
        input_connection_generation = connection_generation;
        present |= CrashCapsuleV3FieldPlatform;
    }

    void reset_memory_windows() noexcept {
        for (auto& window : memory_windows) window = {};
        memory_window_count = 0u;
        present &= ~CrashCapsuleV3FieldMemoryWindows;
    }

    void note_memory_window(
        const std::uint32_t guest_focus,
        const std::uint32_t guest_base,
        const std::uint32_t physical_base,
        const std::uint32_t source_mask,
        const std::uint32_t valid_word_mask,
        const std::array<std::uint32_t,
                         crash_capsule_memory_window_word_capacity>& words) noexcept {
        if (source_mask == 0u || valid_word_mask == 0u) return;
        const auto count = memory_window_count < memory_windows.size()
            ? static_cast<std::size_t>(memory_window_count)
            : memory_windows.size();
        for (std::size_t index = 0u; index < count; ++index) {
            auto& existing = memory_windows[index];
            if (existing.guest_focus != guest_focus ||
                existing.guest_base != guest_base ||
                existing.physical_base != physical_base)
                continue;
            existing.source_mask |= source_mask;
            for (std::size_t word = 0u; word < words.size(); ++word) {
                const auto bit = 1u << word;
                if ((valid_word_mask & bit) != 0u)
                    existing.words[word] = words[word];
            }
            existing.valid_word_mask |= valid_word_mask;
            present |= CrashCapsuleV3FieldMemoryWindows;
            return;
        }
        if (count == memory_windows.size()) return;
        auto& window = memory_windows[count];
        window.guest_focus = guest_focus;
        window.guest_base = guest_base;
        window.physical_base = physical_base;
        window.source_mask = source_mask;
        window.valid_word_mask = valid_word_mask;
        window.words = words;
        memory_window_count = static_cast<std::uint32_t>(count + 1u);
        present |= CrashCapsuleV3FieldMemoryWindows;
    }
};

enum CrashCapsuleV5Field : std::uint64_t {
    CrashCapsuleV5FieldNone = 0u,
    CrashCapsuleV5FieldClosureBinding = 1ull << 0u,
    CrashCapsuleV5FieldClosureWitness = 1ull << 1u,
    CrashCapsuleV5FieldPointerProvenance = 1ull << 2u,
    CrashCapsuleV5FieldProbePlan = 1ull << 3u,
    CrashCapsuleV5FieldDispatchWitness = 1ull << 4u,
    CrashCapsuleV5FieldLoadedAotDigest = 1ull << 5u,
    CrashCapsuleV5FieldProviderTranscript = 1ull << 6u,
    CrashCapsuleV5FieldStallSnapshot = 1ull << 7u,
    CrashCapsuleV5FieldGuestAotCallchain = 1ull << 8u,
};

enum CrashCapsuleV5RecordingFlag : std::uint32_t {
    CrashCapsuleV5RecordingFlagNone = 0u,
    CrashCapsuleV5RecordingFlagClosureBindingDropped = 1u << 0u,
    CrashCapsuleV5RecordingFlagClosureWitnessDropped = 1u << 1u,
    CrashCapsuleV5RecordingFlagPointerProvenanceDropped = 1u << 2u,
    CrashCapsuleV5RecordingFlagProbePlanDropped = 1u << 3u,
    CrashCapsuleV5RecordingFlagDispatchWitnessDropped = 1u << 4u,
    CrashCapsuleV5RecordingFlagLoadedAotDigestDropped = 1u << 5u,
    CrashCapsuleV5RecordingFlagProviderTranscriptDropped = 1u << 6u,
    CrashCapsuleV5RecordingFlagStallSnapshotDropped = 1u << 7u,
    CrashCapsuleV5RecordingFlagGuestAotCallchainDropped = 1u << 8u,
    CrashCapsuleV5RecordingFlagInvalid = 1u << 30u,
    // This bit is set whenever a bounded ring had to discard an item.  A
    // consumer must not use a v5 record with this bit as a complete witness.
    CrashCapsuleV5RecordingFlagTruncated = 1u << 31u,
};

enum class CrashCapsuleV5WitnessKind : std::uint32_t {
    Unknown = 0u,
    StaticCallback = 1u,
    IndirectDispatch = 2u,
    JumpTable = 3u,
    HardwareAccess = 4u,
    LoadedAot = 5u,
    MissingStaticEntry = 6u,
    HostDeadline = 7u,
};

// Every address-like role is intentionally represented separately.  In
// particular, a source address is never silently reused as a callsite or a
// target, and a pointer observation is never an ownership assertion.
struct CrashCapsuleClosureBinding final {
    std::uint64_t sequence = 0u;
    std::uint64_t runtime_generation = 0u;
    std::uint32_t analyzer = 0u;
    std::uint32_t backend = 0u;
    std::uint32_t mode = 0u;
    std::uint32_t lba = 0u;
    std::uint32_t bias = 0u;
    std::uint32_t flags = 0u;
    std::uint32_t status = 0u;
    CrashCapsuleToken key;
    CrashCapsuleToken content;
    CrashCapsuleToken boot;
    CrashCapsuleToken project;
    CrashCapsuleToken analysis_contract;
    CrashCapsuleToken image_analysis;
    CrashCapsuleToken game_project;
    CrashCapsuleToken native_port;
    CrashCapsuleToken native_port_artifact;
    CrashCapsuleToken analysis_impl;
    CrashCapsuleToken analysis_cache_impl;
    CrashCapsuleToken ir_product_impl;
    CrashCapsuleToken codegen_impl;
};

struct CrashCapsuleClosureWitness final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint64_t source_digest = 0u;
    std::uint64_t target_digest = 0u;
    std::uint64_t table_digest = 0u;
    std::uint64_t pointer_value = 0u;
    std::uint32_t kind = 0u;
    std::uint32_t source = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t slot = 0u;
    std::uint32_t pointer = 0u;
    std::uint32_t pointer_present = 0u;
    std::uint32_t target = 0u;
    std::uint32_t alias = 0u;
    std::uint32_t slot_present = 0u;
    std::uint32_t continuation = 0u;
    std::uint32_t flags = 0u;
    std::uint32_t status = 0u;
    std::uint32_t immutable = 0u;
    std::uint32_t bounded = 0u;
    std::uint32_t complete = 0u;
    std::uint32_t runtime_observation = 0u;
    std::uint32_t reproof_required = 1u;
    CrashCapsuleToken source_identity;
    CrashCapsuleToken target_identity;
    CrashCapsuleToken table_identity;
};

struct CrashCapsulePointerProvenance final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint64_t pointer_value = 0u;
    std::uint64_t source_digest = 0u;
    std::uint64_t target_digest = 0u;
    std::uint32_t source = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t slot = 0u;
    std::uint32_t pointer = 0u;
    std::uint32_t pointer_present = 0u;
    std::uint32_t target = 0u;
    std::uint32_t alias = 0u;
    std::uint32_t slot_present = 0u;
    std::uint32_t flags = 0u;
    std::uint32_t status = 0u;
    CrashCapsuleToken source_identity;
    CrashCapsuleToken target_identity;
    CrashCapsuleToken pointer_identity;
};

struct CrashCapsuleClosureProbePlan final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint32_t kind = 0u;
    std::uint32_t state = 0u;
    std::uint32_t source = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t slot = 0u;
    std::uint32_t pointer = 0u;
    std::uint32_t target = 0u;
    std::uint32_t alias = 0u;
    std::uint32_t witness_limit = 0u;
    std::uint32_t flags = 0u;
    CrashCapsuleToken identity;
};

struct CrashCapsuleDispatchWitness final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint64_t table_digest = 0u;
    std::uint32_t kind = 0u;
    std::uint32_t source = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t slot = 0u;
    std::uint32_t pointer = 0u;
    std::uint32_t target = 0u;
    std::uint32_t alias = 0u;
    std::uint32_t continuation = 0u;
    std::uint32_t index = 0u;
    std::uint32_t flags = 0u;
    std::uint32_t status = 0u;
    CrashCapsuleToken source_identity;
    CrashCapsuleToken target_identity;
    CrashCapsuleToken table_identity;
};

struct CrashCapsuleLoadedAotDigest final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint64_t source_generation = 0u;
    std::uint64_t module_digest = 0u;
    std::uint64_t block_digest = 0u;
    std::uint32_t target = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t source_start = 0u;
    std::uint32_t source_offset = 0u;
    std::uint32_t module_size = 0u;
    std::uint32_t block_size = 0u;
    std::uint32_t flags = 0u;
    CrashCapsuleToken module_identity;
    CrashCapsuleToken block_identity;
};

struct CrashCapsuleProviderTranscript final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint64_t provider_digest = 0u;
    std::uint64_t address = 0u;
    std::uint64_t value = 0u;
    std::uint64_t result = 0u;
    std::uint64_t state = 0u;
    std::uint32_t provider = 0u;
    std::uint32_t region = 0u;
    std::uint32_t operation = 0u;
    std::uint32_t width = 0u;
    std::uint32_t source = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t slot = 0u;
    std::uint32_t pointer = 0u;
    std::uint32_t target = 0u;
    std::uint32_t alias = 0u;
    std::uint32_t instruction_source_pc = 0u;
    std::uint32_t instruction_runtime_pc = 0u;
    std::uint32_t instruction_opcode = 0u;
    std::uint32_t instruction_valid = 0u;
    std::uint32_t flags = 0u;
    CrashCapsuleToken provider_identity;
    CrashCapsuleToken source_identity;
    CrashCapsuleToken target_identity;
};

struct CrashCapsuleStallSnapshot final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint64_t guest_cycle = 0u;
    std::uint64_t frame = 0u;
    std::uint64_t attempted = 0u;
    std::uint64_t retired = 0u;
    std::uint32_t pc = 0u;
    std::uint32_t owner = 0u;
    std::uint32_t reason = 0u;
    std::uint32_t phase = 0u;
    std::uint32_t controlled = 0u;
    std::uint32_t flags = 0u;
    std::array<std::uint32_t, 16u> gpr{};
    std::uint32_t sr = 0u;
    std::uint32_t gbr = 0u;
    CrashCapsuleToken reason_identity;
};

struct CrashCapsuleGuestAotCall final {
    std::uint64_t sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint64_t module_digest = 0u;
    std::uint32_t depth = 0u;
    std::uint32_t caller = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t callee = 0u;
    std::uint32_t continuation = 0u;
    std::uint32_t source = 0u;
    std::uint32_t target = 0u;
    std::uint32_t flags = 0u;
    CrashCapsuleToken module_identity;
    CrashCapsuleToken caller_identity;
    CrashCapsuleToken callee_identity;
};

using ClosureBinding = CrashCapsuleClosureBinding;
using ClosureWitness = CrashCapsuleClosureWitness;
using PointerProvenance = CrashCapsulePointerProvenance;
using ClosureProbePlan = CrashCapsuleClosureProbePlan;
using DispatchWitness = CrashCapsuleDispatchWitness;
using LoadedAotDigest = CrashCapsuleLoadedAotDigest;
using ProviderTranscript = CrashCapsuleProviderTranscript;
using StallSnapshot = CrashCapsuleStallSnapshot;
using GuestAotCall = CrashCapsuleGuestAotCall;

struct CrashCapsuleV5Fields final {
    std::uint64_t present = CrashCapsuleV5FieldNone;
    std::uint32_t recording_flags = CrashCapsuleV5RecordingFlagNone;
    std::uint32_t reserved = 0u;
    std::array<CrashCapsuleClosureBinding,
               crash_capsule_closure_binding_capacity> closure_bindings{};
    std::array<CrashCapsuleClosureWitness,
               crash_capsule_closure_witness_capacity> closure_witnesses{};
    std::array<CrashCapsulePointerProvenance,
               crash_capsule_pointer_provenance_capacity> pointer_provenance{};
    std::array<CrashCapsuleClosureProbePlan,
               crash_capsule_closure_probe_plan_capacity> closure_probe_plans{};
    std::array<CrashCapsuleDispatchWitness,
               crash_capsule_dispatch_witness_capacity> dispatch_witnesses{};
    std::array<CrashCapsuleLoadedAotDigest,
               crash_capsule_loaded_aot_digest_capacity> loaded_aot_digests{};
    std::array<CrashCapsuleProviderTranscript,
               crash_capsule_provider_transcript_capacity> provider_transcripts{};
    std::array<CrashCapsuleStallSnapshot,
               crash_capsule_stall_snapshot_capacity> stall_snapshots{};
    std::array<CrashCapsuleGuestAotCall,
               crash_capsule_guest_aot_callchain_capacity> guest_aot_callchain{};
    std::uint32_t closure_binding_count = 0u;
    std::uint32_t closure_binding_next = 0u;
    std::uint64_t closure_binding_drops = 0u;
    std::uint32_t closure_witness_count = 0u;
    std::uint32_t closure_witness_next = 0u;
    std::uint64_t closure_witness_drops = 0u;
    std::uint32_t pointer_provenance_count = 0u;
    std::uint32_t pointer_provenance_next = 0u;
    std::uint64_t pointer_provenance_drops = 0u;
    std::uint32_t closure_probe_plan_count = 0u;
    std::uint32_t closure_probe_plan_next = 0u;
    std::uint64_t closure_probe_plan_drops = 0u;
    std::uint32_t dispatch_witness_count = 0u;
    std::uint32_t dispatch_witness_next = 0u;
    std::uint64_t dispatch_witness_drops = 0u;
    std::uint32_t loaded_aot_digest_count = 0u;
    std::uint32_t loaded_aot_digest_next = 0u;
    std::uint64_t loaded_aot_digest_drops = 0u;
    std::uint32_t provider_transcript_count = 0u;
    std::uint32_t provider_transcript_next = 0u;
    std::uint64_t provider_transcript_drops = 0u;
    std::uint32_t stall_snapshot_count = 0u;
    std::uint32_t stall_snapshot_next = 0u;
    std::uint64_t stall_snapshot_drops = 0u;
    std::uint32_t guest_aot_callchain_count = 0u;
    std::uint32_t guest_aot_callchain_next = 0u;
    std::uint64_t guest_aot_callchain_drops = 0u;

    template <typename Record, std::size_t Capacity>
    static void push_ring(
        std::array<Record, Capacity>& ring,
        std::uint32_t& count,
        std::uint32_t& next,
        std::uint64_t& drops,
        std::uint32_t& recording_flags,
        const std::uint32_t dropped_flag,
        const std::uint64_t present_flag,
        const Record& record,
        std::uint64_t& present) noexcept {
        const auto bounded_next = next < Capacity ? next : 0u;
        if (next >= Capacity || count > Capacity)
            recording_flags |= CrashCapsuleV5RecordingFlagInvalid;
        ring[bounded_next] = record;
        next = (bounded_next + 1u) % static_cast<std::uint32_t>(Capacity);
        if (count < Capacity) {
            ++count;
        } else {
            ++drops;
            recording_flags |= dropped_flag | CrashCapsuleV5RecordingFlagTruncated;
        }
        present |= present_flag;
    }

    static bool token_invalid(const CrashCapsuleToken& token) noexcept {
        // A truncated identity is no longer the exact identity supplied by
        // the producer and therefore cannot remain positive v5 evidence.
        return token.flag_bits() != CrashCapsuleTokenFlagNone;
    }

    void note_closure_binding(const CrashCapsuleClosureBinding& record) noexcept {
        if (token_invalid(record.key) || token_invalid(record.content) ||
            token_invalid(record.boot) || token_invalid(record.project) ||
            token_invalid(record.analysis_contract) ||
            token_invalid(record.image_analysis) || token_invalid(record.game_project) ||
            token_invalid(record.native_port) || token_invalid(record.native_port_artifact) ||
            token_invalid(record.analysis_impl) || token_invalid(record.analysis_cache_impl) ||
            token_invalid(record.ir_product_impl) || token_invalid(record.codegen_impl))
            mark_invalid();
        push_ring(closure_bindings, closure_binding_count, closure_binding_next,
                  closure_binding_drops, recording_flags,
                  CrashCapsuleV5RecordingFlagClosureBindingDropped,
                  CrashCapsuleV5FieldClosureBinding, record, present);
    }
    void note_closure_witness(const CrashCapsuleClosureWitness& record) noexcept {
        if (token_invalid(record.source_identity) || token_invalid(record.target_identity) ||
            token_invalid(record.table_identity))
            mark_invalid();
        push_ring(closure_witnesses, closure_witness_count, closure_witness_next,
                  closure_witness_drops, recording_flags,
                  CrashCapsuleV5RecordingFlagClosureWitnessDropped,
                  CrashCapsuleV5FieldClosureWitness, record, present);
    }
    void note_pointer_provenance(const CrashCapsulePointerProvenance& record) noexcept {
        if (token_invalid(record.source_identity) || token_invalid(record.target_identity) ||
            token_invalid(record.pointer_identity))
            mark_invalid();
        push_ring(pointer_provenance, pointer_provenance_count, pointer_provenance_next,
                  pointer_provenance_drops, recording_flags,
                  CrashCapsuleV5RecordingFlagPointerProvenanceDropped,
                  CrashCapsuleV5FieldPointerProvenance, record, present);
    }
    void note_closure_probe_plan(const CrashCapsuleClosureProbePlan& record) noexcept {
        if (token_invalid(record.identity)) mark_invalid();
        push_ring(closure_probe_plans, closure_probe_plan_count, closure_probe_plan_next,
                  closure_probe_plan_drops, recording_flags,
                  CrashCapsuleV5RecordingFlagProbePlanDropped,
                  CrashCapsuleV5FieldProbePlan, record, present);
    }
    void note_dispatch_witness(const CrashCapsuleDispatchWitness& record) noexcept {
        if (token_invalid(record.source_identity) || token_invalid(record.target_identity) ||
            token_invalid(record.table_identity))
            mark_invalid();
        push_ring(dispatch_witnesses, dispatch_witness_count, dispatch_witness_next,
                  dispatch_witness_drops, recording_flags,
                  CrashCapsuleV5RecordingFlagDispatchWitnessDropped,
                  CrashCapsuleV5FieldDispatchWitness, record, present);
    }
    void note_loaded_aot_digest(const CrashCapsuleLoadedAotDigest& record) noexcept {
        if (token_invalid(record.module_identity) || token_invalid(record.block_identity))
            mark_invalid();
        push_ring(loaded_aot_digests, loaded_aot_digest_count, loaded_aot_digest_next,
                  loaded_aot_digest_drops, recording_flags,
                  CrashCapsuleV5RecordingFlagLoadedAotDigestDropped,
                  CrashCapsuleV5FieldLoadedAotDigest, record, present);
    }
    void note_provider_transcript(const CrashCapsuleProviderTranscript& record) noexcept {
        if (token_invalid(record.provider_identity) || token_invalid(record.source_identity) ||
            token_invalid(record.target_identity))
            mark_invalid();
        push_ring(provider_transcripts, provider_transcript_count,
                  provider_transcript_next, provider_transcript_drops,
                  recording_flags,
                  CrashCapsuleV5RecordingFlagProviderTranscriptDropped,
                  CrashCapsuleV5FieldProviderTranscript, record, present);
    }
    void note_stall_snapshot(const CrashCapsuleStallSnapshot& record) noexcept {
        if (token_invalid(record.reason_identity)) mark_invalid();
        push_ring(stall_snapshots, stall_snapshot_count, stall_snapshot_next,
                  stall_snapshot_drops, recording_flags,
                  CrashCapsuleV5RecordingFlagStallSnapshotDropped,
                  CrashCapsuleV5FieldStallSnapshot, record, present);
    }
    void note_guest_aot_call(const CrashCapsuleGuestAotCall& record) noexcept {
        if (token_invalid(record.module_identity) || token_invalid(record.caller_identity) ||
            token_invalid(record.callee_identity))
            mark_invalid();
        push_ring(guest_aot_callchain, guest_aot_callchain_count,
                  guest_aot_callchain_next, guest_aot_callchain_drops,
                  recording_flags,
                  CrashCapsuleV5RecordingFlagGuestAotCallchainDropped,
                  CrashCapsuleV5FieldGuestAotCallchain, record, present);
    }

    [[nodiscard]] bool truncated() const noexcept {
        return (recording_flags & CrashCapsuleV5RecordingFlagTruncated) != 0u;
    }

    void mark_invalid() noexcept {
        recording_flags |= CrashCapsuleV5RecordingFlagInvalid;
    }

    [[nodiscard]] bool invalid() const noexcept {
        return (recording_flags & CrashCapsuleV5RecordingFlagInvalid) != 0u;
    }

    [[nodiscard]] std::uint64_t drop_count() const noexcept {
        return closure_binding_drops + closure_witness_drops +
               pointer_provenance_drops + closure_probe_plan_drops +
               dispatch_witness_drops + loaded_aot_digest_drops +
               provider_transcript_drops + stall_snapshot_drops +
               guest_aot_callchain_drops;
    }
};

struct CrashCapsuleSerializedLine {
    std::array<char, crash_capsule_v4_line_capacity> bytes{};
    std::uint32_t size = 0u;
    bool truncated = false;

    [[nodiscard]] std::string_view view() const noexcept {
        const auto bounded_size = size < bytes.size() ? size : bytes.size();
        return std::string_view(bytes.data(), bounded_size);
    }
};

struct CrashCapsuleV5SerializedLine {
    std::array<char, crash_capsule_v5_line_capacity> bytes{};
    std::uint32_t size = 0u;
    bool truncated = false;

    [[nodiscard]] std::string_view view() const noexcept {
        const auto bounded_size = size < bytes.size() ? size : bytes.size();
        return std::string_view(bytes.data(), bounded_size);
    }
};

// Always-on product fault context. The complete state is fixed-size POD: recording performs
// no allocation, string formatting, locking, map lookup, or callback construction.
struct CrashCapsule {
    std::uint64_t observed_guest_cycle = 0u;
    std::uint64_t last_scheduler_cycle = 0u;
    std::uint64_t last_scheduler_event_id = 0u;
    std::uint32_t last_pc = 0u;
    std::uint32_t last_block = 0u;
    std::uint32_t last_mmio_address = 0u;
    std::uint32_t last_mmio_value = 0u;
    std::uint32_t last_scheduler_event_kind = 0u;
    std::uint32_t first_error_code = 0u;
    std::uint32_t first_error_pc = 0u;
    std::uint32_t first_error_target = 0u;
    std::uint32_t next_event = 0u;
    std::uint32_t event_count = 0u;
    std::uint8_t last_mmio_width = 0u;
    std::uint8_t last_mmio_operation = 0u;
    std::uint8_t first_error_latched = 0u;
    std::uint8_t reserved = 0u;
    std::array<CrashCapsuleEvent, crash_capsule_event_capacity> events{};
    CrashCapsuleV2Fields v2{};
    CrashCapsuleV3Fields v3{};
    CrashCapsuleV5Fields v5{};

    void note_block(const std::uint32_t pc,
                    const std::uint32_t block,
                    const std::uint64_t guest_cycle) noexcept {
        observed_guest_cycle = guest_cycle;
        last_pc = pc;
        last_block = block;
        push({guest_cycle, 0u, pc, block, 0u, 0u, CrashCapsuleEventKind::Block, 0u});
    }

    void note_mmio(const std::uint8_t operation,
                   const std::uint8_t width,
                   const std::uint32_t address,
                   const std::uint32_t value) noexcept {
        last_mmio_operation = operation;
        last_mmio_width = width;
        last_mmio_address = address;
        last_mmio_value = value;
        push({observed_guest_cycle,
              value,
              last_pc,
              address,
              0u,
              width,
              CrashCapsuleEventKind::Mmio,
              operation});
    }

    void note_scheduler(const std::uint64_t guest_cycle,
                        const std::uint64_t event_id,
                        const std::uint32_t event_kind) noexcept {
        observed_guest_cycle = guest_cycle;
        last_scheduler_cycle = guest_cycle;
        last_scheduler_event_id = event_id;
        last_scheduler_event_kind = event_kind;
        push({guest_cycle,
              event_id,
              last_pc,
              event_kind,
              0u,
              0u,
              CrashCapsuleEventKind::Scheduler,
              0u});
    }

    void note_first_error(const std::uint32_t error_code,
                          const std::uint32_t pc,
                          const std::uint32_t target) noexcept {
        if (first_error_latched != 0u) return;
        first_error_latched = 1u;
        first_error_code = error_code;
        first_error_pc = pc;
        first_error_target = target;
        push({observed_guest_cycle,
              target,
              pc,
              error_code,
              0u,
              0u,
              CrashCapsuleEventKind::Error,
              0u});
    }

    void note_v2_host_exception(const std::uint32_t code,
                                const std::string_view type) noexcept {
        v2.note_host_exception(code, type);
    }

    void note_v2_contract(const std::uint32_t code,
                          const std::string_view type) noexcept {
        v2.note_contract(code, type);
    }

    void note_v2_cpu(const std::uint32_t pc, const std::uint32_t procedure_register) noexcept {
        v2.note_cpu(pc, procedure_register);
    }

    void note_v2_dispatch(const std::uint32_t callsite, const std::uint32_t entry) noexcept {
        v2.note_dispatch(callsite, entry);
    }

    void note_v2_runtime_module(const std::string_view identity,
                                const std::uint64_t generation) noexcept {
        v2.note_runtime_module(identity, generation);
    }

    void note_v2_source_module(const std::string_view identity,
                               const std::uint64_t generation,
                               const std::uint64_t relocation_generation = 0u) noexcept {
        v2.note_source_module(identity, generation, relocation_generation);
    }

    void note_v2_transition(const std::uint32_t kind,
                            const std::uint64_t sequence,
                            const std::uint32_t target,
                            const std::string_view identity = {}) noexcept {
        v2.note_transition(kind, sequence, target, identity);
    }

    void note_v2_wait(const std::uint32_t state,
                      const std::uint64_t cycle,
                      const std::uint64_t pending_events,
                      const std::uint64_t next_cycle = 0u) noexcept {
        v2.note_wait(state, cycle, pending_events, next_cycle);
    }

    void note_v3_cpu_state(
        const std::array<std::uint32_t, 16u>& registers,
        const std::uint32_t status_register,
        const bool t,
        const bool sleeping,
        const bool trap_pending,
        const std::uint32_t global_base,
        const std::uint32_t vector_base,
        const std::uint32_t multiply_high,
        const std::uint32_t multiply_low,
        const std::uint32_t floating_communication,
        const std::uint32_t floating_status,
        const std::uint32_t instruction_physical,
        const std::uint32_t block_physical,
        const std::uint32_t block_size,
        const std::uint64_t attempted,
        const std::uint64_t retired,
        const std::uint64_t total_cycles,
        const std::uint64_t pending_cycles,
        const std::uint64_t exception_epoch,
        const std::uint32_t exception_instruction,
        const std::uint32_t exception_physical,
        const std::uint32_t exception_owner) noexcept {
        v3.note_cpu_state(
            registers, status_register, t, sleeping, trap_pending,
            global_base, vector_base, multiply_high, multiply_low,
            floating_communication, floating_status, instruction_physical,
            block_physical, block_size, attempted, retired, total_cycles,
            pending_cycles, exception_epoch, exception_instruction,
            exception_physical, exception_owner);
    }

    void note_v3_contract_detail(const std::string_view detail) noexcept {
        v3.note_contract_detail(detail);
    }

    void note_v3_loaded_aot(
        const std::uint32_t target,
        const std::uint32_t runtime_start,
        const std::uint32_t source_start,
        const std::uint32_t source_offset,
        const std::uint32_t module_size,
        const std::uint32_t block_size,
        const std::uint64_t expected_generation,
        const std::uint64_t current_generation,
        const std::string_view identity) noexcept {
        v3.note_loaded_aot(
            target, runtime_start, source_start, source_offset, module_size,
            block_size, expected_generation, current_generation, identity);
    }

    void note_v3_runtime_module_lifecycle(
        const std::string_view identity,
        const std::uint32_t source_start,
        const std::uint32_t runtime_start,
        const std::uint32_t module_size,
        const std::uint64_t lifecycle_generation,
        const std::uint64_t retirement_generation,
        const std::uint32_t lifecycle_state) noexcept {
        v3.note_runtime_module_lifecycle(
            identity, source_start, runtime_start, module_size,
            lifecycle_generation, retirement_generation, lifecycle_state);
    }

    [[nodiscard]] std::uint64_t note_v3_lookahead(
        const CrashCapsuleLookaheadKind kind,
        const std::uint32_t callsite,
        const std::uint32_t target,
        const std::uint32_t continuation,
        const std::uint32_t source_start,
        const std::uint32_t runtime_start,
        const std::uint32_t source_offset,
        const std::uint32_t byte_size,
        const std::uint64_t generation,
        const std::string_view identity,
        const std::array<std::uint32_t, 4u> arguments = {}) noexcept {
        return v3.begin_lookahead(
            kind, callsite, target, continuation, source_start, runtime_start,
            source_offset, byte_size, generation, identity, arguments);
    }

    void update_v3_lookahead(
        const std::uint64_t sequence,
        const CrashCapsuleLookaheadState state) noexcept {
        v3.update_lookahead(sequence, state);
    }

    void note_v3_input_trace(const std::string_view trace,
                             const std::string_view identity,
                             const std::uint32_t mode) noexcept {
        v3.note_input_trace(trace, identity, mode);
    }

    void note_v3_product_state(const std::uint32_t stop,
                               const std::uint32_t phase,
                               const std::uint64_t frame,
                               const std::uint64_t presented) noexcept {
        v3.note_product_state(stop, phase, frame, presented);
    }

    void note_v3_platform(const std::uint64_t polls,
                          const std::uint64_t connection_generation) noexcept {
        v3.note_platform(polls, connection_generation);
    }

    void reset_v3_memory_windows() noexcept {
        v3.reset_memory_windows();
    }

    void note_v3_memory_window(
        const std::uint32_t guest_focus,
        const std::uint32_t guest_base,
        const std::uint32_t physical_base,
        const std::uint32_t source_mask,
        const std::uint32_t valid_word_mask,
        const std::array<std::uint32_t,
                         crash_capsule_memory_window_word_capacity>& words) noexcept {
        v3.note_memory_window(
            guest_focus, guest_base, physical_base, source_mask,
            valid_word_mask, words);
    }

    void note_v5_closure_binding(
        const CrashCapsuleClosureBinding& record) noexcept {
        v5.note_closure_binding(record);
    }
    void note_v5_closure_witness(
        const CrashCapsuleClosureWitness& record) noexcept {
        v5.note_closure_witness(record);
    }
    void note_v5_pointer_provenance(
        const CrashCapsulePointerProvenance& record) noexcept {
        v5.note_pointer_provenance(record);
    }
    void note_v5_closure_probe_plan(
        const CrashCapsuleClosureProbePlan& record) noexcept {
        v5.note_closure_probe_plan(record);
    }
    void note_v5_probe_plan(
        const CrashCapsuleClosureProbePlan& record) noexcept {
        note_v5_closure_probe_plan(record);
    }
    void note_v5_dispatch_witness(
        const CrashCapsuleDispatchWitness& record) noexcept {
        v5.note_dispatch_witness(record);
    }
    void note_v5_loaded_aot_digest(
        const CrashCapsuleLoadedAotDigest& record) noexcept {
        v5.note_loaded_aot_digest(record);
    }
    void note_v5_provider_transcript(
        const CrashCapsuleProviderTranscript& record) noexcept {
        v5.note_provider_transcript(record);
    }
    void note_v5_stall_snapshot(
        const CrashCapsuleStallSnapshot& record) noexcept {
        v5.note_stall_snapshot(record);
    }
    void note_v5_guest_aot_call(
        const CrashCapsuleGuestAotCall& record) noexcept {
        v5.note_guest_aot_call(record);
    }
    void note_v5_guest_aot_callchain(
        const CrashCapsuleGuestAotCall& record) noexcept {
        note_v5_guest_aot_call(record);
    }
    void note_v5_invalid() noexcept { v5.mark_invalid(); }

  private:
    void push(const CrashCapsuleEvent event) noexcept {
        events[next_event] = event;
        next_event = (next_event + 1u) &
                     static_cast<std::uint32_t>(crash_capsule_event_capacity - 1u);
        if (event_count < crash_capsule_event_capacity) ++event_count;
    }
};

static_assert(std::is_standard_layout_v<CrashCapsuleEvent>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleEvent>);
static_assert(std::is_standard_layout_v<CrashCapsuleToken>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleToken>);
static_assert(std::is_standard_layout_v<CrashCapsuleV2Fields>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleV2Fields>);
static_assert(std::is_standard_layout_v<CrashCapsuleV3Fields>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleV3Fields>);
static_assert(std::is_standard_layout_v<CrashCapsuleMemoryWindow>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleMemoryWindow>);
static_assert(std::is_standard_layout_v<CrashCapsuleLookahead>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleLookahead>);
static_assert(std::is_standard_layout_v<CrashCapsuleRuntimeModuleLifecycle>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleRuntimeModuleLifecycle>);
static_assert(std::is_standard_layout_v<CrashCapsuleClosureBinding>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleClosureBinding>);
static_assert(std::is_standard_layout_v<CrashCapsuleClosureWitness>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleClosureWitness>);
static_assert(std::is_standard_layout_v<CrashCapsulePointerProvenance>);
static_assert(std::is_trivially_copyable_v<CrashCapsulePointerProvenance>);
static_assert(std::is_standard_layout_v<CrashCapsuleClosureProbePlan>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleClosureProbePlan>);
static_assert(std::is_standard_layout_v<CrashCapsuleDispatchWitness>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleDispatchWitness>);
static_assert(std::is_standard_layout_v<CrashCapsuleLoadedAotDigest>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleLoadedAotDigest>);
static_assert(std::is_standard_layout_v<CrashCapsuleProviderTranscript>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleProviderTranscript>);
static_assert(std::is_standard_layout_v<CrashCapsuleStallSnapshot>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleStallSnapshot>);
static_assert(std::is_standard_layout_v<CrashCapsuleGuestAotCall>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleGuestAotCall>);
static_assert(std::is_standard_layout_v<CrashCapsuleV5Fields>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleV5Fields>);
static_assert(std::is_standard_layout_v<CrashCapsuleSerializedLine>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleSerializedLine>);
static_assert(std::is_standard_layout_v<CrashCapsuleV5SerializedLine>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleV5SerializedLine>);
static_assert(std::is_standard_layout_v<CrashCapsule>);
static_assert(std::is_trivially_copyable_v<CrashCapsule>);

namespace crash_capsule_detail {

template <typename Line>
struct LineWriter final {
    Line& line;

    void append(const std::string_view value) noexcept {
        if (line.truncated) return;
        if (line.size >= line.bytes.size() ||
            value.size() > line.bytes.size() - line.size) {
            line.truncated = true;
            return;
        }
        for (const auto character : value) line.bytes[line.size++] = character;
    }

    void append_char(const char value) noexcept {
        if (line.truncated) return;
        if (line.size >= line.bytes.size()) {
            line.truncated = true;
            return;
        }
        line.bytes[line.size++] = value;
    }

    void append_boolean(const bool value) noexcept {
        append(value ? "true" : "false");
    }

    template <typename Integer>
    void append_integer(const Integer value) noexcept {
        std::array<char, 32u> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (converted.ec != std::errc{}) {
            line.truncated = true;
            return;
        }
        append(std::string_view(buffer.data(),
                                static_cast<std::size_t>(converted.ptr - buffer.data())));
    }

    void append_token(const CrashCapsuleToken& token) noexcept {
        append_char('"');
        append(token.view());
        append_char('"');
    }
};

template <typename Line>
inline void append_field(LineWriter<Line>& writer,
                         const std::string_view name,
                         const std::uint64_t value) noexcept {
    writer.append(",\"");
    writer.append(name);
    writer.append("\":");
    writer.append_integer(value);
}

template <typename Line>
inline void append_first_field(LineWriter<Line>& writer,
                               const std::string_view name,
                               const std::uint64_t value) noexcept {
    writer.append("\"");
    writer.append(name);
    writer.append("\":");
    writer.append_integer(value);
}

template <typename Line>
inline void append_field(LineWriter<Line>& writer,
                         const std::string_view name,
                         const std::uint32_t value) noexcept {
    append_field(writer, name, static_cast<std::uint64_t>(value));
}

template <typename Line>
inline void append_token_field(LineWriter<Line>& writer,
                               const std::string_view name,
                               const CrashCapsuleToken& token) noexcept {
    writer.append(",\"");
    writer.append(name);
    writer.append("\":");
    writer.append_token(token);
    writer.append(",\"");
    writer.append(name);
    writer.append("_flags\":");
    writer.append_integer(static_cast<std::uint32_t>(token.flag_bits()));
}

template <typename Line>
inline void append_v5_roles(LineWriter<Line>& writer,
                            const std::uint32_t source,
                            const std::uint32_t callsite,
                            const std::uint32_t slot,
                            const std::uint32_t pointer,
                            const std::uint32_t target,
                            const std::uint32_t alias,
                            const std::uint32_t slot_present,
                            const std::uint32_t pointer_present = 0u) noexcept {
    append_field(writer, "source", source);
    append_field(writer, "callsite", callsite);
    append_field(writer, "slot", slot);
    append_field(writer, "slot_present", slot_present);
    append_field(writer, "pointer", pointer);
    append_field(writer, "pointer_present", pointer_present);
    append_field(writer, "target", target);
    append_field(writer, "alias", alias);
}

template <typename Line, typename Record, std::size_t Capacity, typename Emit>
inline void append_v5_ring(LineWriter<Line>& writer,
                           const std::string_view name,
                           const std::array<Record, Capacity>& ring,
                           const std::uint32_t count,
                           const std::uint32_t next,
                           Emit&& emit) noexcept {
    writer.append(",\"");
    writer.append(name);
    writer.append("\":[");
    const auto bounded_count = count < Capacity ? static_cast<std::size_t>(count)
                                                 : Capacity;
    const auto bounded_next = next < Capacity ? static_cast<std::size_t>(next) : 0u;
    const auto oldest = bounded_count < Capacity ? 0u : bounded_next;
    for (std::size_t offset = 0u; offset < bounded_count; ++offset) {
        const auto index = (oldest + offset) % Capacity;
        writer.append(offset == 0u ? "{" : ",{");
        emit(writer, ring[index]);
        writer.append("}");
    }
    writer.append("]");
}

} // namespace crash_capsule_detail

[[nodiscard]] inline CrashCapsuleSerializedLine
serialize_crash_capsule_v2(const CrashCapsule& capsule) noexcept {
    CrashCapsuleSerializedLine result;
    crash_capsule_detail::LineWriter writer{result};
    const auto& fields = capsule.v2;
    writer.append("{\"schema\":\"katana-crash-capsule\",\"version\":");
    writer.append_integer(crash_capsule_v2_contract_version);
    crash_capsule_detail::append_field(writer, "present", fields.present);
    crash_capsule_detail::append_field(writer, "host_exception_code",
                                        fields.host_exception_code);
    crash_capsule_detail::append_token_field(writer, "host_exception_type",
                                              fields.host_exception_type);
    crash_capsule_detail::append_field(writer, "contract_code", fields.contract_code);
    crash_capsule_detail::append_token_field(writer, "contract_type", fields.contract_type);
    crash_capsule_detail::append_field(writer, "guest_pc", fields.guest_pc);
    crash_capsule_detail::append_field(writer, "pr", fields.pr);
    crash_capsule_detail::append_field(writer, "active_callsite", fields.active_callsite);
    crash_capsule_detail::append_field(writer, "active_entry", fields.active_entry);
    crash_capsule_detail::append_token_field(writer, "runtime_module",
                                              fields.runtime_module_identity);
    crash_capsule_detail::append_field(writer, "runtime_generation",
                                        fields.runtime_generation);
    crash_capsule_detail::append_token_field(writer, "source_module",
                                              fields.source_module_identity);
    crash_capsule_detail::append_field(writer, "source_generation", fields.source_generation);
    crash_capsule_detail::append_field(writer, "source_relocation_generation",
                                        fields.source_relocation_generation);
    crash_capsule_detail::append_field(writer, "transition_kind", fields.transition_kind);
    crash_capsule_detail::append_field(writer, "transition_sequence",
                                        fields.transition_sequence);
    crash_capsule_detail::append_field(writer, "transition_target", fields.transition_target);
    crash_capsule_detail::append_token_field(writer, "transition_identity",
                                              fields.transition_identity);
    crash_capsule_detail::append_field(writer, "wait_state", fields.wait_state);
    crash_capsule_detail::append_field(writer, "scheduler_cycle", fields.scheduler_cycle);
    crash_capsule_detail::append_field(writer, "pending_event_count",
                                        fields.pending_event_count);
    crash_capsule_detail::append_field(writer, "next_event_cycle", fields.next_event_cycle);
    crash_capsule_detail::append_field(writer, "last_pc", capsule.last_pc);
    crash_capsule_detail::append_field(writer, "last_block", capsule.last_block);
    crash_capsule_detail::append_field(writer, "last_scheduler_cycle",
                                        capsule.last_scheduler_cycle);
    crash_capsule_detail::append_field(writer, "last_scheduler_event_id",
                                        capsule.last_scheduler_event_id);
    crash_capsule_detail::append_field(writer, "last_scheduler_event_kind",
                                        capsule.last_scheduler_event_kind);
    crash_capsule_detail::append_field(writer, "first_error_code", capsule.first_error_code);
    crash_capsule_detail::append_field(writer, "first_error_pc", capsule.first_error_pc);
    crash_capsule_detail::append_field(writer, "first_error_target", capsule.first_error_target);
    crash_capsule_detail::append_field(writer, "ring_events", capsule.event_count);
    writer.append(",\"events\":[");
    const auto event_count = capsule.event_count < crash_capsule_event_capacity
        ? static_cast<std::size_t>(capsule.event_count)
        : crash_capsule_event_capacity;
    const auto event_mask = crash_capsule_event_capacity - 1u;
    const auto oldest_event = event_count < crash_capsule_event_capacity
        ? 0u
        : static_cast<std::size_t>(capsule.next_event) & event_mask;
    for (std::size_t offset = 0u; offset < event_count; ++offset) {
        const auto index = (oldest_event + offset) & event_mask;
        const auto& event = capsule.events[index];
        writer.append(offset == 0u ? "{" : ",{");
        writer.append("\"guest_cycle\":");
        writer.append_integer(event.guest_cycle);
        crash_capsule_detail::append_field(writer, "detail", event.detail);
        crash_capsule_detail::append_field(writer, "pc", event.pc);
        crash_capsule_detail::append_field(writer, "subject", event.subject);
        crash_capsule_detail::append_field(writer, "auxiliary", event.auxiliary);
        crash_capsule_detail::append_field(
            writer, "code", static_cast<std::uint32_t>(event.code));
        crash_capsule_detail::append_field(
            writer, "kind", static_cast<std::uint32_t>(event.kind));
        crash_capsule_detail::append_field(
            writer, "flags", static_cast<std::uint32_t>(event.flags));
        writer.append("}");
    }
    writer.append("]");
    writer.append("}");
    return result;
}

[[nodiscard]] inline CrashCapsuleSerializedLine
serialize_crash_capsule_v4(const CrashCapsule& capsule) noexcept {
    CrashCapsuleSerializedLine result;
    crash_capsule_detail::LineWriter writer{result};
    const auto& fields = capsule.v2;
    const auto& flight = capsule.v3;
    writer.append("{\"schema\":\"katana-crash-capsule\",\"version\":");
    writer.append_integer(crash_capsule_v4_contract_version);
    crash_capsule_detail::append_field(writer, "present", fields.present);
    crash_capsule_detail::append_field(writer, "present_v3", flight.present);
    crash_capsule_detail::append_field(writer, "host_exception_code",
                                        fields.host_exception_code);
    crash_capsule_detail::append_token_field(writer, "host_exception_type",
                                              fields.host_exception_type);
    crash_capsule_detail::append_field(writer, "contract_code", fields.contract_code);
    crash_capsule_detail::append_token_field(writer, "contract_type", fields.contract_type);
    crash_capsule_detail::append_token_field(writer, "contract_detail",
                                              flight.contract_detail);
    crash_capsule_detail::append_field(writer, "contract_detail_hash",
                                        flight.contract_detail_hash);
    crash_capsule_detail::append_field(writer, "guest_pc", fields.guest_pc);
    crash_capsule_detail::append_field(writer, "pr", fields.pr);
    writer.append(",\"gpr\":[");
    for (std::size_t index = 0u; index < flight.gpr.size(); ++index) {
        if (index != 0u) writer.append_char(',');
        writer.append_integer(flight.gpr[index]);
    }
    writer.append("]");
    crash_capsule_detail::append_field(
        writer, "memory_window_count", flight.memory_window_count);
    writer.append(",\"memory_windows\":[");
    const auto memory_window_count =
        flight.memory_window_count < flight.memory_windows.size()
            ? static_cast<std::size_t>(flight.memory_window_count)
            : flight.memory_windows.size();
    for (std::size_t index = 0u; index < memory_window_count; ++index) {
        const auto& window = flight.memory_windows[index];
        writer.append(index == 0u ? "{" : ",{");
        writer.append("\"guest_focus\":");
        writer.append_integer(window.guest_focus);
        crash_capsule_detail::append_field(writer, "guest_base", window.guest_base);
        crash_capsule_detail::append_field(
            writer, "physical_base", window.physical_base);
        crash_capsule_detail::append_field(writer, "source_mask", window.source_mask);
        crash_capsule_detail::append_field(
            writer, "valid_word_mask", window.valid_word_mask);
        writer.append(",\"words\":[");
        for (std::size_t word = 0u; word < window.words.size(); ++word) {
            if (word != 0u) writer.append_char(',');
            writer.append_integer(window.words[word]);
        }
        writer.append("]}");
    }
    writer.append("]");
    crash_capsule_detail::append_field(writer, "sr", flight.sr);
    crash_capsule_detail::append_field(writer, "cpu_flags", flight.cpu_flags);
    crash_capsule_detail::append_field(writer, "gbr", flight.gbr);
    crash_capsule_detail::append_field(writer, "vbr", flight.vbr);
    crash_capsule_detail::append_field(writer, "mach", flight.mach);
    crash_capsule_detail::append_field(writer, "macl", flight.macl);
    crash_capsule_detail::append_field(writer, "fpul", flight.fpul);
    crash_capsule_detail::append_field(writer, "fpscr", flight.fpscr);
    crash_capsule_detail::append_field(writer, "active_callsite", fields.active_callsite);
    crash_capsule_detail::append_field(writer, "active_entry", fields.active_entry);
    crash_capsule_detail::append_field(writer, "active_instruction_physical",
                                        flight.active_instruction_physical);
    crash_capsule_detail::append_field(writer, "active_block_physical",
                                        flight.active_block_physical);
    crash_capsule_detail::append_field(writer, "active_block_size",
                                        flight.active_block_size);
    crash_capsule_detail::append_field(writer, "attempted_guest_instructions",
                                        flight.attempted_guest_instructions);
    crash_capsule_detail::append_field(writer, "retired_guest_instructions",
                                        flight.retired_guest_instructions);
    crash_capsule_detail::append_field(writer, "total_guest_cycles",
                                        flight.total_guest_cycles);
    crash_capsule_detail::append_field(writer, "pending_guest_cycles",
                                        flight.pending_guest_cycles);
    crash_capsule_detail::append_field(writer, "exception_generation",
                                        flight.exception_generation);
    crash_capsule_detail::append_field(writer, "last_exception_instruction",
                                        flight.last_exception_instruction);
    crash_capsule_detail::append_field(writer, "last_exception_physical",
                                        flight.last_exception_physical);
    crash_capsule_detail::append_field(writer, "last_exception_owner",
                                        flight.last_exception_owner);
    crash_capsule_detail::append_token_field(writer, "runtime_module",
                                              fields.runtime_module_identity);
    crash_capsule_detail::append_field(writer, "runtime_generation",
                                        fields.runtime_generation);
    crash_capsule_detail::append_token_field(writer, "source_module",
                                              fields.source_module_identity);
    crash_capsule_detail::append_field(writer, "source_generation", fields.source_generation);
    crash_capsule_detail::append_field(writer, "source_relocation_generation",
                                        fields.source_relocation_generation);
    crash_capsule_detail::append_token_field(writer, "loaded_aot_identity",
                                              flight.loaded_aot_identity);
    crash_capsule_detail::append_field(writer, "loaded_aot_target",
                                        flight.loaded_aot_target);
    crash_capsule_detail::append_field(writer, "loaded_aot_runtime_start",
                                        flight.loaded_aot_runtime_start);
    crash_capsule_detail::append_field(writer, "loaded_aot_source_start",
                                        flight.loaded_aot_source_start);
    crash_capsule_detail::append_field(writer, "loaded_aot_source_offset",
                                        flight.loaded_aot_source_offset);
    crash_capsule_detail::append_field(writer, "loaded_aot_module_size",
                                        flight.loaded_aot_module_size);
    crash_capsule_detail::append_field(writer, "loaded_aot_block_size",
                                        flight.loaded_aot_block_size);
    crash_capsule_detail::append_field(writer, "loaded_aot_expected_generation",
                                        flight.loaded_aot_expected_generation);
    crash_capsule_detail::append_field(writer, "loaded_aot_current_generation",
                                        flight.loaded_aot_current_generation);
    crash_capsule_detail::append_token_field(
        writer, "runtime_module_lifecycle_identity",
        flight.runtime_module_lifecycle_identity);
    crash_capsule_detail::append_field(
        writer, "runtime_module_source_start",
        flight.runtime_module_source_start);
    crash_capsule_detail::append_field(
        writer, "runtime_module_runtime_start",
        flight.runtime_module_runtime_start);
    crash_capsule_detail::append_field(
        writer, "runtime_module_size", flight.runtime_module_size);
    crash_capsule_detail::append_field(
        writer, "runtime_module_lifecycle_generation",
        flight.runtime_module_lifecycle_generation);
    crash_capsule_detail::append_field(
        writer, "runtime_module_retirement_generation",
        flight.runtime_module_retirement_generation);
    crash_capsule_detail::append_field(
        writer, "runtime_module_lifecycle_state",
        flight.runtime_module_lifecycle_state);
    crash_capsule_detail::append_field(
        writer, "runtime_module_lifecycle_sequence",
        flight.runtime_module_lifecycle_sequence);
    crash_capsule_detail::append_field(
        writer, "runtime_module_lifecycle_count",
        flight.runtime_module_lifecycle_count);
    writer.append(",\"runtime_module_lifecycles\":[");
    const auto lifecycle_count =
        flight.runtime_module_lifecycle_count <
                flight.runtime_module_lifecycles.size()
            ? static_cast<std::size_t>(
                  flight.runtime_module_lifecycle_count)
            : flight.runtime_module_lifecycles.size();
    const auto oldest_lifecycle =
        lifecycle_count < flight.runtime_module_lifecycles.size()
            ? 0u
            : static_cast<std::size_t>(
                  flight.next_runtime_module_lifecycle);
    for (std::size_t offset = 0u; offset < lifecycle_count; ++offset) {
        const auto index =
            (oldest_lifecycle + offset) %
            flight.runtime_module_lifecycles.size();
        const auto& item = flight.runtime_module_lifecycles[index];
        writer.append(offset == 0u ? "{" : ",{");
        writer.append("\"sequence\":");
        writer.append_integer(item.sequence);
        crash_capsule_detail::append_field(
            writer, "lifecycle_generation", item.lifecycle_generation);
        crash_capsule_detail::append_field(
            writer, "retirement_generation", item.retirement_generation);
        crash_capsule_detail::append_field(
            writer, "source_start", item.source_start);
        crash_capsule_detail::append_field(
            writer, "runtime_start", item.runtime_start);
        crash_capsule_detail::append_field(
            writer, "module_size", item.module_size);
        crash_capsule_detail::append_field(writer, "state", item.state);
        crash_capsule_detail::append_token_field(
            writer, "identity", item.identity);
        writer.append("}");
    }
    writer.append("]");
    crash_capsule_detail::append_field(
        writer, "lookahead_sequence", flight.lookahead_sequence);
    crash_capsule_detail::append_field(
        writer, "lookahead_count", flight.lookahead_count);
    writer.append(",\"lookahead\":[");
    const auto lookahead_count =
        flight.lookahead_count < flight.lookahead.size()
            ? static_cast<std::size_t>(flight.lookahead_count)
            : flight.lookahead.size();
    const auto oldest_lookahead =
        lookahead_count < flight.lookahead.size()
            ? 0u
            : static_cast<std::size_t>(flight.next_lookahead);
    for (std::size_t offset = 0u; offset < lookahead_count; ++offset) {
        const auto index =
            (oldest_lookahead + offset) % flight.lookahead.size();
        const auto& item = flight.lookahead[index];
        writer.append(offset == 0u ? "{" : ",{");
        writer.append("\"sequence\":");
        writer.append_integer(item.sequence);
        crash_capsule_detail::append_field(writer, "generation",
                                            item.generation);
        crash_capsule_detail::append_field(writer, "kind", item.kind);
        crash_capsule_detail::append_field(writer, "state", item.state);
        crash_capsule_detail::append_field(writer, "callsite",
                                            item.callsite);
        crash_capsule_detail::append_field(writer, "target", item.target);
        crash_capsule_detail::append_field(writer, "continuation",
                                            item.continuation);
        crash_capsule_detail::append_field(writer, "source_start",
                                            item.source_start);
        crash_capsule_detail::append_field(writer, "runtime_start",
                                            item.runtime_start);
        crash_capsule_detail::append_field(writer, "source_offset",
                                            item.source_offset);
        crash_capsule_detail::append_field(writer, "byte_size",
                                            item.byte_size);
        writer.append(",\"arguments\":[");
        for (std::size_t argument = 0u;
             argument < item.arguments.size(); ++argument) {
            if (argument != 0u) writer.append(",");
            writer.append_integer(item.arguments[argument]);
        }
        writer.append("]");
        crash_capsule_detail::append_token_field(writer, "identity",
                                                  item.identity);
        writer.append("}");
    }
    writer.append("]");
    crash_capsule_detail::append_token_field(writer, "input_trace",
                                              flight.input_trace);
    crash_capsule_detail::append_token_field(writer, "input_identity",
                                              flight.input_identity);
    crash_capsule_detail::append_field(writer, "input_trace_hash",
                                        flight.input_trace_hash);
    crash_capsule_detail::append_field(writer, "input_trace_mode",
                                        flight.input_trace_mode);
    crash_capsule_detail::append_field(writer, "input_polls", flight.input_polls);
    crash_capsule_detail::append_field(writer, "input_connection_generation",
                                        flight.input_connection_generation);
    crash_capsule_detail::append_field(writer, "stop_reason", flight.stop_reason);
    crash_capsule_detail::append_field(writer, "bootstrap_phase",
                                        flight.bootstrap_phase);
    crash_capsule_detail::append_field(writer, "frame_index", flight.frame_index);
    crash_capsule_detail::append_field(writer, "presented_frames",
                                        flight.presented_frames);
    crash_capsule_detail::append_field(writer, "transition_kind", fields.transition_kind);
    crash_capsule_detail::append_field(writer, "transition_sequence",
                                        fields.transition_sequence);
    crash_capsule_detail::append_field(writer, "transition_target", fields.transition_target);
    crash_capsule_detail::append_token_field(writer, "transition_identity",
                                              fields.transition_identity);
    crash_capsule_detail::append_field(writer, "wait_state", fields.wait_state);
    crash_capsule_detail::append_field(writer, "scheduler_cycle", fields.scheduler_cycle);
    crash_capsule_detail::append_field(writer, "pending_event_count",
                                        fields.pending_event_count);
    crash_capsule_detail::append_field(writer, "next_event_cycle", fields.next_event_cycle);
    crash_capsule_detail::append_field(writer, "last_pc", capsule.last_pc);
    crash_capsule_detail::append_field(writer, "last_block", capsule.last_block);
    crash_capsule_detail::append_field(writer, "last_mmio_address",
                                        capsule.last_mmio_address);
    crash_capsule_detail::append_field(writer, "last_mmio_value",
                                        capsule.last_mmio_value);
    crash_capsule_detail::append_field(writer, "last_mmio_width",
                                        static_cast<std::uint32_t>(capsule.last_mmio_width));
    crash_capsule_detail::append_field(writer, "last_mmio_operation",
                                        static_cast<std::uint32_t>(capsule.last_mmio_operation));
    crash_capsule_detail::append_field(writer, "last_scheduler_cycle",
                                        capsule.last_scheduler_cycle);
    crash_capsule_detail::append_field(writer, "last_scheduler_event_id",
                                        capsule.last_scheduler_event_id);
    crash_capsule_detail::append_field(writer, "last_scheduler_event_kind",
                                        capsule.last_scheduler_event_kind);
    crash_capsule_detail::append_field(writer, "first_error_code", capsule.first_error_code);
    crash_capsule_detail::append_field(writer, "first_error_pc", capsule.first_error_pc);
    crash_capsule_detail::append_field(writer, "first_error_target", capsule.first_error_target);
    crash_capsule_detail::append_field(writer, "ring_events", capsule.event_count);
    writer.append(",\"events\":[");
    const auto event_count = capsule.event_count < crash_capsule_event_capacity
        ? static_cast<std::size_t>(capsule.event_count)
        : crash_capsule_event_capacity;
    const auto event_mask = crash_capsule_event_capacity - 1u;
    const auto oldest_event = event_count < crash_capsule_event_capacity
        ? 0u
        : static_cast<std::size_t>(capsule.next_event) & event_mask;
    for (std::size_t offset = 0u; offset < event_count; ++offset) {
        const auto index = (oldest_event + offset) & event_mask;
        const auto& event = capsule.events[index];
        writer.append(offset == 0u ? "{" : ",{");
        writer.append("\"guest_cycle\":");
        writer.append_integer(event.guest_cycle);
        crash_capsule_detail::append_field(writer, "detail", event.detail);
        crash_capsule_detail::append_field(writer, "pc", event.pc);
        crash_capsule_detail::append_field(writer, "subject", event.subject);
        crash_capsule_detail::append_field(writer, "auxiliary", event.auxiliary);
        crash_capsule_detail::append_field(
            writer, "code", static_cast<std::uint32_t>(event.code));
        crash_capsule_detail::append_field(
            writer, "kind", static_cast<std::uint32_t>(event.kind));
        crash_capsule_detail::append_field(
            writer, "flags", static_cast<std::uint32_t>(event.flags));
        writer.append("}");
    }
    writer.append("]}");
    if (!result.truncated) return result;

    // Never publish a partial JSON object.  If a future bounded section grows
    // beyond the fixed wire budget, retain the essential fault identity in a
    // compact, valid v4 record and mark the omitted detail explicitly.
    result = {};
    crash_capsule_detail::LineWriter fallback{result};
    fallback.append("{\"schema\":\"katana-crash-capsule\",\"version\":");
    fallback.append_integer(crash_capsule_v4_contract_version);
    fallback.append(",\"truncated\":true");
    crash_capsule_detail::append_field(
        fallback, "present", capsule.v2.present);
    crash_capsule_detail::append_field(
        fallback, "present_v3", capsule.v3.present);
    crash_capsule_detail::append_field(
        fallback, "contract_code", capsule.v2.contract_code);
    crash_capsule_detail::append_field(
        fallback, "guest_pc", capsule.v2.guest_pc);
    crash_capsule_detail::append_field(fallback, "pr", capsule.v2.pr);
    crash_capsule_detail::append_field(
        fallback, "first_error_code", capsule.first_error_code);
    crash_capsule_detail::append_field(
        fallback, "first_error_pc", capsule.first_error_pc);
    crash_capsule_detail::append_field(
        fallback, "first_error_target", capsule.first_error_target);
    crash_capsule_detail::append_token_field(
        fallback, "runtime_module_lifecycle_identity",
        capsule.v3.runtime_module_lifecycle_identity);
    crash_capsule_detail::append_field(
        fallback, "runtime_module_source_start",
        capsule.v3.runtime_module_source_start);
    crash_capsule_detail::append_field(
        fallback, "runtime_module_runtime_start",
        capsule.v3.runtime_module_runtime_start);
    crash_capsule_detail::append_field(
        fallback, "runtime_module_size",
        capsule.v3.runtime_module_size);
    crash_capsule_detail::append_field(
        fallback, "runtime_module_lifecycle_generation",
        capsule.v3.runtime_module_lifecycle_generation);
    crash_capsule_detail::append_field(
        fallback, "runtime_module_retirement_generation",
        capsule.v3.runtime_module_retirement_generation);
    crash_capsule_detail::append_field(
        fallback, "runtime_module_lifecycle_state",
        capsule.v3.runtime_module_lifecycle_state);
    crash_capsule_detail::append_field(
        fallback, "lookahead_count", capsule.v3.lookahead_count);
    fallback.append("}");
    result.truncated = true;
    return result;
}

[[nodiscard]] inline CrashCapsuleV5SerializedLine
serialize_crash_capsule_v5(const CrashCapsule& capsule) noexcept {
    CrashCapsuleV5SerializedLine result;
    crash_capsule_detail::LineWriter writer{result};
    const auto& v5 = capsule.v5;
    writer.append("{\"schema\":\"katana-crash-capsule\",\"version\":");
    writer.append_integer(crash_capsule_v5_contract_version);
    crash_capsule_detail::append_field(writer, "present", v5.present);
    crash_capsule_detail::append_field(writer, "recording_flags", v5.recording_flags);
    writer.append(",\"invalid\":");
    writer.append_boolean(v5.invalid());
    writer.append(",\"truncated\":");
    writer.append_boolean(v5.truncated());
    crash_capsule_detail::append_field(writer, "drop_count", v5.drop_count());
    crash_capsule_detail::append_field(writer, "v4_present", capsule.v2.present);
    crash_capsule_detail::append_field(writer, "v3_present", capsule.v3.present);
    crash_capsule_detail::append_field(writer, "contract_code", capsule.v2.contract_code);
    crash_capsule_detail::append_field(writer, "guest_pc", capsule.v2.guest_pc);
    crash_capsule_detail::append_field(writer, "pr", capsule.v2.pr);
    crash_capsule_detail::append_field(writer, "first_error_code", capsule.first_error_code);
    crash_capsule_detail::append_field(writer, "first_error_pc", capsule.first_error_pc);
    crash_capsule_detail::append_field(writer, "first_error_target", capsule.first_error_target);

    crash_capsule_detail::append_field(writer, "closure_binding_count", v5.closure_binding_count);
    crash_capsule_detail::append_field(writer, "closure_binding_drops", v5.closure_binding_drops);
    crash_capsule_detail::append_field(writer, "closure_witness_count", v5.closure_witness_count);
    crash_capsule_detail::append_field(writer, "closure_witness_drops", v5.closure_witness_drops);
    crash_capsule_detail::append_field(writer, "pointer_provenance_count", v5.pointer_provenance_count);
    crash_capsule_detail::append_field(writer, "pointer_provenance_drops", v5.pointer_provenance_drops);
    crash_capsule_detail::append_field(writer, "closure_probe_plan_count", v5.closure_probe_plan_count);
    crash_capsule_detail::append_field(writer, "closure_probe_plan_drops", v5.closure_probe_plan_drops);
    crash_capsule_detail::append_field(writer, "dispatch_witness_count", v5.dispatch_witness_count);
    crash_capsule_detail::append_field(writer, "dispatch_witness_drops", v5.dispatch_witness_drops);
    crash_capsule_detail::append_field(writer, "loaded_aot_digest_count", v5.loaded_aot_digest_count);
    crash_capsule_detail::append_field(writer, "loaded_aot_digest_drops", v5.loaded_aot_digest_drops);
    crash_capsule_detail::append_field(writer, "provider_transcript_count", v5.provider_transcript_count);
    crash_capsule_detail::append_field(writer, "provider_transcript_drops", v5.provider_transcript_drops);
    crash_capsule_detail::append_field(writer, "stall_snapshot_count", v5.stall_snapshot_count);
    crash_capsule_detail::append_field(writer, "stall_snapshot_drops", v5.stall_snapshot_drops);
    crash_capsule_detail::append_field(writer, "guest_aot_callchain_count", v5.guest_aot_callchain_count);
    crash_capsule_detail::append_field(writer, "guest_aot_callchain_drops", v5.guest_aot_callchain_drops);

    crash_capsule_detail::append_v5_ring(
        writer, "closure_bindings", v5.closure_bindings, v5.closure_binding_count,
        v5.closure_binding_next, [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "runtime_generation", item.runtime_generation);
            crash_capsule_detail::append_field(out, "analyzer", item.analyzer);
            crash_capsule_detail::append_field(out, "backend", item.backend);
            crash_capsule_detail::append_field(out, "mode", item.mode);
            crash_capsule_detail::append_field(out, "lba", item.lba);
            crash_capsule_detail::append_field(out, "bias", item.bias);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_field(out, "status", item.status);
            crash_capsule_detail::append_token_field(out, "key", item.key);
            crash_capsule_detail::append_token_field(out, "content", item.content);
            crash_capsule_detail::append_token_field(out, "boot", item.boot);
            crash_capsule_detail::append_token_field(out, "project", item.project);
            crash_capsule_detail::append_token_field(out, "analysis_contract", item.analysis_contract);
            crash_capsule_detail::append_token_field(out, "image_analysis", item.image_analysis);
            crash_capsule_detail::append_token_field(out, "game_project", item.game_project);
            crash_capsule_detail::append_token_field(out, "native_port", item.native_port);
            crash_capsule_detail::append_token_field(out, "native_port_artifact", item.native_port_artifact);
            crash_capsule_detail::append_token_field(out, "analysis_impl", item.analysis_impl);
            crash_capsule_detail::append_token_field(out, "analysis_cache_impl", item.analysis_cache_impl);
            crash_capsule_detail::append_token_field(out, "ir_product_impl", item.ir_product_impl);
            crash_capsule_detail::append_token_field(out, "codegen_impl", item.codegen_impl);
        });
    crash_capsule_detail::append_v5_ring(
        writer, "closure_witnesses", v5.closure_witnesses, v5.closure_witness_count,
        v5.closure_witness_next, [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "generation", item.generation);
            crash_capsule_detail::append_field(out, "kind", item.kind);
            crash_capsule_detail::append_v5_roles(out, item.source, item.callsite,
                                                   item.slot, item.pointer, item.target,
                                                   item.alias, item.slot_present,
                                                   item.pointer_present);
            crash_capsule_detail::append_field(out, "continuation", item.continuation);
            crash_capsule_detail::append_field(out, "pointer_value", item.pointer_value);
            crash_capsule_detail::append_field(out, "source_digest", item.source_digest);
            crash_capsule_detail::append_field(out, "target_digest", item.target_digest);
            crash_capsule_detail::append_field(out, "table_digest", item.table_digest);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_field(out, "status", item.status);
            crash_capsule_detail::append_field(out, "immutable", item.immutable);
            crash_capsule_detail::append_field(out, "bounded", item.bounded);
            crash_capsule_detail::append_field(out, "complete", item.complete);
            crash_capsule_detail::append_field(out, "runtime_observation", item.runtime_observation);
            crash_capsule_detail::append_field(out, "reproof_required", item.reproof_required);
            crash_capsule_detail::append_token_field(out, "source_identity", item.source_identity);
            crash_capsule_detail::append_token_field(out, "target_identity", item.target_identity);
            crash_capsule_detail::append_token_field(out, "table_identity", item.table_identity);
        });
    crash_capsule_detail::append_v5_ring(
        writer, "pointer_provenance", v5.pointer_provenance,
        v5.pointer_provenance_count, v5.pointer_provenance_next,
        [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "generation", item.generation);
            crash_capsule_detail::append_v5_roles(out, item.source, item.callsite,
                                                   item.slot, item.pointer, item.target,
                                                   item.alias, item.slot_present,
                                                   item.pointer_present);
            crash_capsule_detail::append_field(out, "pointer_value", item.pointer_value);
            crash_capsule_detail::append_field(out, "source_digest", item.source_digest);
            crash_capsule_detail::append_field(out, "target_digest", item.target_digest);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_field(out, "status", item.status);
            crash_capsule_detail::append_token_field(out, "source_identity", item.source_identity);
            crash_capsule_detail::append_token_field(out, "target_identity", item.target_identity);
            crash_capsule_detail::append_token_field(out, "pointer_identity", item.pointer_identity);
        });
    crash_capsule_detail::append_v5_ring(
        writer, "closure_probe_plans", v5.closure_probe_plans,
        v5.closure_probe_plan_count, v5.closure_probe_plan_next,
        [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "generation", item.generation);
            crash_capsule_detail::append_field(out, "kind", item.kind);
            crash_capsule_detail::append_field(out, "state", item.state);
            crash_capsule_detail::append_v5_roles(out, item.source, item.callsite,
                                                   item.slot, item.pointer, item.target,
                                                   item.alias, 0u);
            crash_capsule_detail::append_field(out, "witness_limit", item.witness_limit);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_token_field(out, "identity", item.identity);
        });
    crash_capsule_detail::append_v5_ring(
        writer, "dispatch_witnesses", v5.dispatch_witnesses,
        v5.dispatch_witness_count, v5.dispatch_witness_next,
        [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "generation", item.generation);
            crash_capsule_detail::append_field(out, "kind", item.kind);
            crash_capsule_detail::append_v5_roles(out, item.source, item.callsite,
                                                   item.slot, item.pointer, item.target,
                                                   item.alias, 0u);
            crash_capsule_detail::append_field(out, "continuation", item.continuation);
            crash_capsule_detail::append_field(out, "index", item.index);
            crash_capsule_detail::append_field(out, "table_digest", item.table_digest);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_field(out, "status", item.status);
            crash_capsule_detail::append_token_field(out, "source_identity", item.source_identity);
            crash_capsule_detail::append_token_field(out, "target_identity", item.target_identity);
            crash_capsule_detail::append_token_field(out, "table_identity", item.table_identity);
        });
    crash_capsule_detail::append_v5_ring(
        writer, "loaded_aot_digests", v5.loaded_aot_digests,
        v5.loaded_aot_digest_count, v5.loaded_aot_digest_next,
        [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "generation", item.generation);
            crash_capsule_detail::append_field(out, "source_generation", item.source_generation);
            crash_capsule_detail::append_field(out, "target", item.target);
            crash_capsule_detail::append_field(out, "runtime_start", item.runtime_start);
            crash_capsule_detail::append_field(out, "source_start", item.source_start);
            crash_capsule_detail::append_field(out, "source_offset", item.source_offset);
            crash_capsule_detail::append_field(out, "module_size", item.module_size);
            crash_capsule_detail::append_field(out, "block_size", item.block_size);
            crash_capsule_detail::append_field(out, "module_digest", item.module_digest);
            crash_capsule_detail::append_field(out, "block_digest", item.block_digest);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_token_field(out, "module_identity", item.module_identity);
            crash_capsule_detail::append_token_field(out, "block_identity", item.block_identity);
        });
    crash_capsule_detail::append_v5_ring(
        writer, "provider_transcripts", v5.provider_transcripts,
        v5.provider_transcript_count, v5.provider_transcript_next,
        [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "generation", item.generation);
            crash_capsule_detail::append_field(out, "provider", item.provider);
            crash_capsule_detail::append_field(out, "region", item.region);
            crash_capsule_detail::append_field(out, "operation", item.operation);
            crash_capsule_detail::append_field(out, "width", item.width);
            crash_capsule_detail::append_field(out, "address", item.address);
            crash_capsule_detail::append_field(out, "value", item.value);
            crash_capsule_detail::append_field(out, "result", item.result);
            crash_capsule_detail::append_field(out, "state", item.state);
            crash_capsule_detail::append_v5_roles(out, item.source, item.callsite,
                                                   item.slot, item.pointer, item.target,
                                                   item.alias, 0u);
            crash_capsule_detail::append_field(
                out, "instruction_source_pc", item.instruction_source_pc);
            crash_capsule_detail::append_field(
                out, "instruction_runtime_pc", item.instruction_runtime_pc);
            crash_capsule_detail::append_field(
                out, "instruction_opcode", item.instruction_opcode);
            crash_capsule_detail::append_field(
                out, "instruction_valid", item.instruction_valid);
            crash_capsule_detail::append_field(out, "provider_digest", item.provider_digest);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_token_field(out, "provider_identity", item.provider_identity);
            crash_capsule_detail::append_token_field(out, "source_identity", item.source_identity);
            crash_capsule_detail::append_token_field(out, "target_identity", item.target_identity);
        });
    crash_capsule_detail::append_v5_ring(
        writer, "stall_snapshots", v5.stall_snapshots,
        v5.stall_snapshot_count, v5.stall_snapshot_next,
        [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "generation", item.generation);
            crash_capsule_detail::append_field(out, "guest_cycle", item.guest_cycle);
            crash_capsule_detail::append_field(out, "frame", item.frame);
            crash_capsule_detail::append_field(out, "attempted", item.attempted);
            crash_capsule_detail::append_field(out, "retired", item.retired);
            crash_capsule_detail::append_field(out, "pc", item.pc);
            crash_capsule_detail::append_field(out, "owner", item.owner);
            crash_capsule_detail::append_field(out, "reason", item.reason);
            crash_capsule_detail::append_field(out, "phase", item.phase);
            crash_capsule_detail::append_field(out, "controlled", item.controlled);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_field(out, "sr", item.sr);
            crash_capsule_detail::append_field(out, "gbr", item.gbr);
            out.append(",\"gpr\":[");
            for (std::size_t index = 0u; index < item.gpr.size(); ++index) {
                if (index != 0u) out.append_char(',');
                out.append_integer(item.gpr[index]);
            }
            out.append("]");
            crash_capsule_detail::append_token_field(out, "reason_identity", item.reason_identity);
        });
    crash_capsule_detail::append_v5_ring(
        writer, "guest_aot_callchain", v5.guest_aot_callchain,
        v5.guest_aot_callchain_count, v5.guest_aot_callchain_next,
        [](auto& out, const auto& item) {
            crash_capsule_detail::append_first_field(out, "sequence", item.sequence);
            crash_capsule_detail::append_field(out, "generation", item.generation);
            crash_capsule_detail::append_field(out, "module_digest", item.module_digest);
            crash_capsule_detail::append_field(out, "depth", item.depth);
            crash_capsule_detail::append_field(out, "caller", item.caller);
            crash_capsule_detail::append_field(out, "callsite", item.callsite);
            crash_capsule_detail::append_field(out, "callee", item.callee);
            crash_capsule_detail::append_field(out, "continuation", item.continuation);
            crash_capsule_detail::append_field(out, "source", item.source);
            crash_capsule_detail::append_field(out, "target", item.target);
            crash_capsule_detail::append_field(out, "flags", item.flags);
            crash_capsule_detail::append_token_field(out, "module_identity", item.module_identity);
            crash_capsule_detail::append_token_field(out, "caller_identity", item.caller_identity);
            crash_capsule_detail::append_token_field(out, "callee_identity", item.callee_identity);
        });

    writer.append("}");
    if (!result.truncated) return result;

    // A capsule must never publish a syntactically incomplete JSON object.
    // Keep a small, valid v5 record and retain the drop/invalid state so a
    // strict consumer cannot mistake the fallback for complete evidence.
    result = {};
    crash_capsule_detail::LineWriter fallback{result};
    fallback.append("{\"schema\":\"katana-crash-capsule\",\"version\":");
    fallback.append_integer(crash_capsule_v5_contract_version);
    fallback.append(",\"invalid\":true,\"truncated\":true");
    crash_capsule_detail::append_field(fallback, "recording_flags", v5.recording_flags);
    crash_capsule_detail::append_field(fallback, "drop_count", v5.drop_count());
    crash_capsule_detail::append_field(fallback, "contract_code", capsule.v2.contract_code);
    crash_capsule_detail::append_field(fallback, "guest_pc", capsule.v2.guest_pc);
    crash_capsule_detail::append_field(fallback, "pr", capsule.v2.pr);
    crash_capsule_detail::append_field(fallback, "first_error_code", capsule.first_error_code);
    crash_capsule_detail::append_field(fallback, "first_error_pc", capsule.first_error_pc);
    crash_capsule_detail::append_field(fallback, "first_error_target", capsule.first_error_target);
    fallback.append("}");
    result.truncated = true;
    return result;
}

} // namespace katana::runtime
