#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <string_view>
#include <type_traits>

namespace katana::runtime {

inline constexpr std::uint32_t crash_capsule_contract_version = 1u;
// Version 1 remains the wire contract used by existing generated callers.  The v2
// extension is additive and is emitted only through the bounded v2 serializer below.
inline constexpr std::uint32_t crash_capsule_v2_contract_version = 2u;
inline constexpr std::size_t crash_capsule_event_capacity = 16u;
inline constexpr std::size_t crash_capsule_token_capacity = 128u;
inline constexpr std::size_t crash_capsule_v2_line_capacity = 8192u;
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
        size = static_cast<std::uint8_t>(count);
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
    std::uint8_t size = 0u;
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

struct CrashCapsuleSerializedLine {
    std::array<char, crash_capsule_v2_line_capacity> bytes{};
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
static_assert(std::is_standard_layout_v<CrashCapsuleSerializedLine>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleSerializedLine>);
static_assert(std::is_standard_layout_v<CrashCapsule>);
static_assert(std::is_trivially_copyable_v<CrashCapsule>);

namespace crash_capsule_detail {

struct LineWriter final {
    CrashCapsuleSerializedLine& line;

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

inline void append_field(LineWriter& writer,
                         const std::string_view name,
                         const std::uint64_t value) noexcept {
    writer.append(",\"");
    writer.append(name);
    writer.append("\":");
    writer.append_integer(value);
}

inline void append_field(LineWriter& writer,
                         const std::string_view name,
                         const std::uint32_t value) noexcept {
    append_field(writer, name, static_cast<std::uint64_t>(value));
}

inline void append_token_field(LineWriter& writer,
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

} // namespace katana::runtime
