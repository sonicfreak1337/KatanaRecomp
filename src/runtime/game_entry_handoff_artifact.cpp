#include "katana/runtime/game_entry_handoff_artifact.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/runtime/dreamcast_boot.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::array<std::uint8_t, 8u> artifact_magic{
    'K', 'A', 'T', 'G', 'H', 'F', '1', '\n'};
constexpr std::uint32_t artifact_header_size = 192u;
constexpr std::uint32_t artifact_payload_entry_size = 168u;
constexpr std::uint32_t artifact_maximum_descriptor_size =
    16u * 1024u * 1024u;
constexpr std::uint32_t artifact_maximum_payload_count = 32'768u;
constexpr std::uint32_t artifact_payload_name_size = 64u;
constexpr std::size_t artifact_sha_text_size = 64u;
constexpr std::size_t artifact_sha_offset = 64u;
constexpr std::size_t artifact_descriptor_sha_offset =
    artifact_sha_offset + artifact_sha_text_size;
constexpr std::string_view sha256_prefix = "sha256:";
constexpr std::string_view zero_sha256 =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view zero_prefixed_sha256 =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[noreturn]] void artifact_error(const std::string_view message) {
    throw std::runtime_error(std::string(message));
}

bool lower_hex(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

bool valid_unprefixed_sha256(const std::string_view value) noexcept {
    return value.size() == artifact_sha_text_size && lower_hex(value);
}

bool stable_boot_file_name(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 255u || value == "." || value == "..")
        return false;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x21u && byte <= 0x7Eu && character != '/' &&
               character != '\\';
    });
}

bool stable_payload_name(const std::string_view value) noexcept {
    if (value.empty() || value.size() > artifact_payload_name_size ||
        value == "." || value == "..")
        return false;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') ||
               character == '-' || character == '_' || character == '.';
    });
}

std::string hash_bytes(const std::span<const std::uint8_t> bytes) {
    const auto view =
        bytes.empty()
            ? std::string_view{}
            : std::string_view(reinterpret_cast<const char*>(bytes.data()),
                               bytes.size());
    return io::sha256_bytes(view);
}

std::uint64_t checked_add(const std::uint64_t left,
                          const std::uint64_t right) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        artifact_error("Game-entry handoff artifact size overflow.");
    return left + right;
}

std::uint64_t checked_multiply(const std::uint64_t left,
                               const std::uint64_t right) {
    if (left != 0u &&
        right > std::numeric_limits<std::uint64_t>::max() / left)
        artifact_error("Game-entry handoff artifact size overflow.");
    return left * right;
}

std::uint32_t checked_count(const std::size_t value,
                            const std::uint32_t maximum) {
    if (value > maximum)
        artifact_error("Game-entry handoff artifact count exceeds its limit.");
    return static_cast<std::uint32_t>(value);
}

class BinaryWriter final {
  public:
    void u8(const std::uint8_t value) {
        bytes_.push_back(value);
    }

    void u16(const std::uint16_t value) {
        for (std::size_t byte = 0u; byte < 2u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void boolean(const bool value) {
        u8(static_cast<std::uint8_t>(value ? 1u : 0u));
    }

    template <typename Enum>
    void enumeration(const Enum value) {
        u32(static_cast<std::uint32_t>(value));
    }

    void string(const std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            artifact_error("Game-entry handoff artifact string is too long.");
        u32(static_cast<std::uint32_t>(value.size()));
        raw(value);
    }

    void raw(const std::string_view value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void raw(const std::span<const std::uint8_t> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void zeros(const std::size_t count) {
        bytes_.insert(bytes_.end(), count, 0u);
    }

    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class BinaryReader final {
  public:
    explicit BinaryReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() {
        return take(1u)[0];
    }

    [[nodiscard]] std::uint16_t u16() {
        const auto bytes = take(2u);
        std::uint16_t value = 0u;
        for (std::size_t byte = 0u; byte < 2u; ++byte)
            value |= static_cast<std::uint16_t>(bytes[byte])
                     << (byte * 8u);
        return value;
    }

    [[nodiscard]] std::uint32_t u32() {
        const auto bytes = take(4u);
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            value |= static_cast<std::uint32_t>(bytes[byte])
                     << (byte * 8u);
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        const auto bytes = take(8u);
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            value |= static_cast<std::uint64_t>(bytes[byte])
                     << (byte * 8u);
        return value;
    }

    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            artifact_error("Game-entry handoff artifact boolean is invalid.");
        return value != 0u;
    }

    template <typename Enum>
    [[nodiscard]] Enum enumeration() {
        static_assert(std::is_enum_v<Enum>);
        using Underlying = std::underlying_type_t<Enum>;
        using Unsigned = std::make_unsigned_t<Underlying>;
        const auto value = u32();
        if (value >
            static_cast<std::uint32_t>(
                std::numeric_limits<Unsigned>::max()))
            artifact_error(
                "Game-entry handoff artifact enum encoding is invalid.");
        return static_cast<Enum>(
            static_cast<Underlying>(
                static_cast<Unsigned>(value)));
    }

    [[nodiscard]] std::string string(const std::size_t maximum_size) {
        const auto size = u32();
        if (size > maximum_size)
            artifact_error("Game-entry handoff artifact string is too long.");
        const auto bytes = take(size);
        return std::string(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size());
    }

    [[nodiscard]] std::span<const std::uint8_t> take(
        const std::size_t size) {
        if (position_ > bytes_.size() || size > bytes_.size() - position_)
            artifact_error("Game-entry handoff artifact is truncated.");
        const auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - position_;
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0u;
};

bool empty_slice(const GameEntryPrivateSliceReference& reference) noexcept {
    return reference.artifact_identity.empty() &&
           reference.artifact_offset == 0u && reference.size == 0u &&
           reference.byte_identity.empty();
}

void write_private_slice(BinaryWriter& writer,
                         const GameEntryPrivateSliceReference& reference,
                         const bool canonical_artifact) {
    const auto present = !empty_slice(reference);
    writer.boolean(present);
    if (!present)
        return;
    writer.string(canonical_artifact ? zero_prefixed_sha256
                                     : reference.artifact_identity);
    writer.u64(reference.artifact_offset);
    writer.u32(reference.size);
    writer.string(reference.byte_identity);
}

GameEntryPrivateSliceReference read_private_slice(BinaryReader& reader) {
    if (!reader.boolean())
        return {};
    GameEntryPrivateSliceReference result;
    result.artifact_identity = reader.string(80u);
    result.artifact_offset = reader.u64();
    result.size = reader.u32();
    result.byte_identity = reader.string(80u);
    return result;
}

std::vector<std::uint8_t> serialize_descriptor(
    const GameEntryHandoff& handoff,
    const bool canonical_artifact) {
    BinaryWriter writer;
    writer.u32(handoff.binding.schema_version);
    writer.u32(handoff.binding.required_runtime_abi);
    writer.u32(handoff.binding.required_platform_state_contract);
    writer.string(handoff.binding.executable.content_identity);
    writer.string(handoff.binding.executable.boot_file_name);
    writer.string(handoff.binding.executable.boot_byte_identity);
    writer.enumeration(handoff.binding.console_profile);
    writer.string(canonical_artifact ? zero_prefixed_sha256
                                     : handoff.binding.descriptor_identity);

    writer.enumeration(handoff.completeness);
    writer.enumeration(handoff.transfer.kind);
    writer.u32(handoff.transfer.entry_pc);
    writer.u32(handoff.transfer.exact_pr);

    for (const auto value : handoff.cpu.gpr_bank0)
        writer.u32(value);
    for (const auto value : handoff.cpu.gpr_bank1)
        writer.u32(value);
    for (const auto value : handoff.cpu.r8_to_r15)
        writer.u32(value);
    for (const auto value : handoff.cpu.fpr_bank0)
        writer.u32(value);
    for (const auto value : handoff.cpu.fpr_bank1)
        writer.u32(value);
    writer.u32(handoff.cpu.pc);
    writer.u32(handoff.cpu.pr);
    writer.u32(handoff.cpu.sr);
    writer.u32(handoff.cpu.fpscr);
    writer.u32(handoff.cpu.gbr);
    writer.u32(handoff.cpu.vbr);
    writer.u32(handoff.cpu.dbr);
    writer.u32(handoff.cpu.ssr);
    writer.u32(handoff.cpu.spc);
    writer.u32(handoff.cpu.sgr);
    writer.u32(handoff.cpu.mach);
    writer.u32(handoff.cpu.macl);
    writer.u32(handoff.cpu.fpul);
    writer.u32(handoff.cpu.tra);
    writer.u32(handoff.cpu.tea);
    writer.u32(handoff.cpu.expevt);
    writer.u32(handoff.cpu.intevt);
    writer.u32(handoff.cpu.mmu.pteh);
    writer.u32(handoff.cpu.mmu.ptel);
    writer.u32(handoff.cpu.mmu.ptea);
    writer.u32(handoff.cpu.mmu.ttb);
    writer.u32(handoff.cpu.mmu.mmucr);
    for (const auto& entry : handoff.cpu.mmu.utlb) {
        writer.u32(entry.pteh);
        writer.u32(entry.ptel);
        writer.u32(entry.ptea);
    }
    writer.boolean(handoff.cpu.exception.trap_pending);
    writer.enumeration(handoff.cpu.exception.last_cause);
    writer.boolean(handoff.cpu.exception.in_delay_slot);
    writer.u32(handoff.cpu.exception.last_instruction_pc);
    writer.u32(handoff.cpu.exception.last_instruction_physical_pc);
    writer.u32(handoff.cpu.exception.last_owner_pc);
    writer.boolean(handoff.cpu.exception.sleeping);

    writer.u32(checked_count(handoff.memory_operations.size(), 16'384u));
    for (const auto& operation : handoff.memory_operations) {
        writer.enumeration(operation.region);
        writer.u32(operation.offset);
        writer.u32(operation.size);
        writer.enumeration(operation.kind);
        writer.u8(operation.fill_value);
        write_private_slice(
            writer, operation.private_slice, canonical_artifact);
        writer.string(operation.expected_before_identity);
        writer.string(operation.expected_after_identity);
        writer.boolean(operation.executable);
    }

    writer.u32(checked_count(handoff.devices.size(), 128u));
    for (const auto& device : handoff.devices) {
        writer.enumeration(device.key.kind);
        writer.u16(device.key.instance);
        writer.u32(device.state_contract_version);
        writer.u32(checked_count(device.scalars.size(), 65'536u));
        for (const auto& scalar : device.scalars) {
            writer.u32(scalar.field_id);
            writer.u64(scalar.value);
        }
        writer.u32(checked_count(device.payloads.size(), 16'384u));
        for (const auto& payload : device.payloads) {
            writer.u32(payload.field_id);
            write_private_slice(
                writer, payload.private_slice, canonical_artifact);
        }
    }

    writer.u64(handoff.scheduler.current_cycle);
    writer.u32(
        checked_count(handoff.scheduler.pending_events.size(), 16'384u));
    for (const auto& event : handoff.scheduler.pending_events) {
        writer.u64(event.guest_cycle);
        writer.enumeration(event.kind);
        writer.enumeration(event.owner.kind);
        writer.u16(event.owner.instance);
        writer.u32(event.channel);
        writer.u64(event.token);
    }
    writer.string(handoff.expected_semantic_state_identity);

    auto bytes = std::move(writer).finish();
    if (bytes.empty() || bytes.size() > artifact_maximum_descriptor_size)
        artifact_error(
            "Game-entry handoff descriptor exceeds its hard size limit.");
    return bytes;
}

template <typename Value, std::size_t Size>
void read_u32_array(BinaryReader& reader,
                    std::array<Value, Size>& values) {
    static_assert(std::is_same_v<Value, std::uint32_t>);
    for (auto& value : values)
        value = reader.u32();
}

GameEntryHandoff deserialize_descriptor(
    const std::span<const std::uint8_t> bytes) {
    BinaryReader reader(bytes);
    GameEntryHandoff handoff;
    handoff.binding.schema_version = reader.u32();
    handoff.binding.required_runtime_abi = reader.u32();
    handoff.binding.required_platform_state_contract = reader.u32();
    handoff.binding.executable.content_identity = reader.string(80u);
    handoff.binding.executable.boot_file_name = reader.string(255u);
    handoff.binding.executable.boot_byte_identity = reader.string(80u);
    handoff.binding.console_profile =
        reader.enumeration<DreamcastConsoleProfile>();
    handoff.binding.descriptor_identity = reader.string(80u);

    handoff.completeness =
        reader.enumeration<GameEntryHandoffCompleteness>();
    handoff.transfer.kind = reader.enumeration<GameEntryTransferKind>();
    handoff.transfer.entry_pc = reader.u32();
    handoff.transfer.exact_pr = reader.u32();

    read_u32_array(reader, handoff.cpu.gpr_bank0);
    read_u32_array(reader, handoff.cpu.gpr_bank1);
    read_u32_array(reader, handoff.cpu.r8_to_r15);
    read_u32_array(reader, handoff.cpu.fpr_bank0);
    read_u32_array(reader, handoff.cpu.fpr_bank1);
    handoff.cpu.pc = reader.u32();
    handoff.cpu.pr = reader.u32();
    handoff.cpu.sr = reader.u32();
    handoff.cpu.fpscr = reader.u32();
    handoff.cpu.gbr = reader.u32();
    handoff.cpu.vbr = reader.u32();
    handoff.cpu.dbr = reader.u32();
    handoff.cpu.ssr = reader.u32();
    handoff.cpu.spc = reader.u32();
    handoff.cpu.sgr = reader.u32();
    handoff.cpu.mach = reader.u32();
    handoff.cpu.macl = reader.u32();
    handoff.cpu.fpul = reader.u32();
    handoff.cpu.tra = reader.u32();
    handoff.cpu.tea = reader.u32();
    handoff.cpu.expevt = reader.u32();
    handoff.cpu.intevt = reader.u32();
    handoff.cpu.mmu.pteh = reader.u32();
    handoff.cpu.mmu.ptel = reader.u32();
    handoff.cpu.mmu.ptea = reader.u32();
    handoff.cpu.mmu.ttb = reader.u32();
    handoff.cpu.mmu.mmucr = reader.u32();
    for (auto& entry : handoff.cpu.mmu.utlb) {
        entry.pteh = reader.u32();
        entry.ptel = reader.u32();
        entry.ptea = reader.u32();
    }
    handoff.cpu.exception.trap_pending = reader.boolean();
    handoff.cpu.exception.last_cause =
        reader.enumeration<ExceptionCause>();
    handoff.cpu.exception.in_delay_slot = reader.boolean();
    handoff.cpu.exception.last_instruction_pc = reader.u32();
    handoff.cpu.exception.last_instruction_physical_pc = reader.u32();
    handoff.cpu.exception.last_owner_pc = reader.u32();
    handoff.cpu.exception.sleeping = reader.boolean();

    const auto memory_count = reader.u32();
    if (memory_count > 16'384u)
        artifact_error(
            "Game-entry handoff artifact has too many memory operations.");
    handoff.memory_operations.reserve(memory_count);
    for (std::uint32_t index = 0u; index < memory_count; ++index) {
        GameEntryMemoryOperation operation;
        operation.region = reader.enumeration<GameEntryMemoryRegion>();
        operation.offset = reader.u32();
        operation.size = reader.u32();
        operation.kind =
            reader.enumeration<GameEntryMemoryOperationKind>();
        operation.fill_value = reader.u8();
        operation.private_slice = read_private_slice(reader);
        operation.expected_before_identity = reader.string(80u);
        operation.expected_after_identity = reader.string(80u);
        operation.executable = reader.boolean();
        handoff.memory_operations.push_back(std::move(operation));
    }

    const auto device_count = reader.u32();
    if (device_count > 128u)
        artifact_error(
            "Game-entry handoff artifact has too many device states.");
    handoff.devices.reserve(device_count);
    std::uint64_t total_scalars = 0u;
    std::uint64_t total_payloads = 0u;
    for (std::uint32_t index = 0u; index < device_count; ++index) {
        GameEntryDeviceState device;
        device.key.kind = reader.enumeration<GameEntryDeviceKind>();
        device.key.instance = reader.u16();
        device.state_contract_version = reader.u32();
        const auto scalar_count = reader.u32();
        total_scalars = checked_add(total_scalars, scalar_count);
        if (total_scalars > 65'536u)
            artifact_error(
                "Game-entry handoff artifact has too many device scalars.");
        device.scalars.reserve(scalar_count);
        for (std::uint32_t scalar_index = 0u;
             scalar_index < scalar_count;
             ++scalar_index) {
            device.scalars.push_back({reader.u32(), reader.u64()});
        }
        const auto payload_count = reader.u32();
        total_payloads = checked_add(total_payloads, payload_count);
        if (total_payloads > 16'384u)
            artifact_error(
                "Game-entry handoff artifact has too many device payloads.");
        device.payloads.reserve(payload_count);
        for (std::uint32_t payload_index = 0u;
             payload_index < payload_count;
             ++payload_index) {
            GameEntryDevicePayload payload;
            payload.field_id = reader.u32();
            payload.private_slice = read_private_slice(reader);
            device.payloads.push_back(std::move(payload));
        }
        handoff.devices.push_back(std::move(device));
    }

    handoff.scheduler.current_cycle = reader.u64();
    const auto event_count = reader.u32();
    if (event_count > 16'384u)
        artifact_error(
            "Game-entry handoff artifact has too many scheduler events.");
    handoff.scheduler.pending_events.reserve(event_count);
    for (std::uint32_t index = 0u; index < event_count; ++index) {
        GameEntryScheduledEvent event;
        event.guest_cycle = reader.u64();
        event.kind = reader.enumeration<SchedulerEventKind>();
        event.owner.kind = reader.enumeration<GameEntryDeviceKind>();
        event.owner.instance = reader.u16();
        event.channel = reader.u32();
        event.token = reader.u64();
        handoff.scheduler.pending_events.push_back(event);
    }
    handoff.expected_semantic_state_identity = reader.string(80u);

    if (reader.remaining() != 0u)
        artifact_error(
            "Game-entry handoff descriptor has trailing bytes.");
    return handoff;
}

bool valid_console_profile(const DreamcastConsoleProfile value) noexcept {
    return static_cast<std::uint32_t>(value) <=
           static_cast<std::uint32_t>(DreamcastConsoleProfile::Vga);
}

bool valid_transfer_kind(const GameEntryTransferKind value) noexcept {
    return static_cast<std::uint32_t>(value) <=
           static_cast<std::uint32_t>(
               GameEntryTransferKind::CallWithReturnPr);
}

bool valid_completeness(
    const GameEntryHandoffCompleteness value) noexcept {
    return static_cast<std::uint32_t>(value) <=
           static_cast<std::uint32_t>(
               GameEntryHandoffCompleteness::CpuMemoryDiagnostic);
}

bool valid_exception_cause(const ExceptionCause value) noexcept {
    return static_cast<std::uint32_t>(value) <=
           static_cast<std::uint32_t>(ExceptionCause::Interrupt);
}

bool valid_memory_region(const GameEntryMemoryRegion value) noexcept {
    return static_cast<std::uint32_t>(value) <=
           static_cast<std::uint32_t>(GameEntryMemoryRegion::AicaRam);
}

bool valid_memory_kind(const GameEntryMemoryOperationKind value) noexcept {
    return static_cast<std::uint32_t>(value) <=
           static_cast<std::uint32_t>(
               GameEntryMemoryOperationKind::CopyPrivateSlice);
}

bool valid_device_kind(const GameEntryDeviceKind value) noexcept {
    const auto number = static_cast<std::uint32_t>(value);
    return number >= static_cast<std::uint32_t>(GameEntryDeviceKind::Pvr) &&
           number <=
               static_cast<std::uint32_t>(GameEntryDeviceKind::Flash);
}

bool valid_scheduler_kind(const SchedulerEventKind value) noexcept {
    const auto number = static_cast<std::uint32_t>(value);
    return number >=
               static_cast<std::uint32_t>(SchedulerEventKind::DiscRead) &&
           number <=
               static_cast<std::uint32_t>(SchedulerEventKind::AicaTick) &&
           value != SchedulerEventKind::MediaVideo &&
           value != SchedulerEventKind::MediaAudio;
}

bool valid_private_slice(
    const GameEntryPrivateSliceReference& reference) noexcept {
    return valid_game_entry_sha256_identity(reference.artifact_identity) &&
           valid_game_entry_sha256_identity(reference.byte_identity) &&
           reference.size != 0u &&
           reference.artifact_offset <=
               std::numeric_limits<std::uint64_t>::max() - reference.size;
}

void validate_descriptor_structure(const GameEntryHandoff& handoff) {
    if (handoff.binding.schema_version !=
            game_entry_handoff_schema_version ||
        handoff.binding.required_runtime_abi != abi_version ||
        handoff.binding.required_platform_state_contract !=
            game_entry_platform_state_contract_version ||
        !valid_game_entry_content_identity(
            handoff.binding.executable.content_identity) ||
        !stable_boot_file_name(
            handoff.binding.executable.boot_file_name) ||
        !valid_game_entry_sha256_identity(
            handoff.binding.executable.boot_byte_identity) ||
        !valid_console_profile(handoff.binding.console_profile) ||
        !valid_game_entry_sha256_identity(
            handoff.binding.descriptor_identity) ||
        !valid_completeness(handoff.completeness) ||
        !valid_transfer_kind(handoff.transfer.kind) ||
        !valid_exception_cause(handoff.cpu.exception.last_cause) ||
        !valid_game_entry_sha256_identity(
            handoff.expected_semantic_state_identity))
        artifact_error(
            "Game-entry handoff descriptor contract is invalid.");

    if (game_entry_semantic_state_identity(handoff) !=
            handoff.expected_semantic_state_identity ||
        game_entry_handoff_descriptor_identity(handoff) !=
            handoff.binding.descriptor_identity)
        artifact_error(
            "Game-entry handoff descriptor identity is invalid.");

    std::uint64_t staged_bytes = 0u;
    for (const auto& operation : handoff.memory_operations) {
        if (!valid_memory_region(operation.region) ||
            !valid_memory_kind(operation.kind) || operation.size == 0u ||
            !valid_game_entry_sha256_identity(
                operation.expected_before_identity) ||
            !valid_game_entry_sha256_identity(
                operation.expected_after_identity))
            artifact_error(
                "Game-entry handoff memory operation is invalid.");
        if (operation.kind == GameEntryMemoryOperationKind::Fill) {
            if (!empty_slice(operation.private_slice))
                artifact_error(
                    "Fill operation unexpectedly contains a private slice.");
        } else if (!valid_private_slice(operation.private_slice) ||
                   operation.private_slice.size != operation.size ||
                   operation.private_slice.byte_identity !=
                       operation.expected_after_identity) {
            artifact_error(
                "Copy operation private slice is invalid.");
        }
        staged_bytes = checked_add(staged_bytes, operation.size);
    }

    for (const auto& device : handoff.devices) {
        if (!valid_device_kind(device.key.kind) ||
            device.state_contract_version == 0u)
            artifact_error(
                "Game-entry handoff device state is invalid.");
        for (const auto& payload : device.payloads) {
            if (!valid_private_slice(payload.private_slice))
                artifact_error(
                    "Game-entry handoff device payload is invalid.");
            staged_bytes =
                checked_add(staged_bytes, payload.private_slice.size);
        }
    }
    if (staged_bytes > 64u * 1024u * 1024u)
        artifact_error(
            "Game-entry handoff private payloads exceed their hard limit.");

    for (const auto& event : handoff.scheduler.pending_events) {
        if (!valid_scheduler_kind(event.kind) ||
            !valid_device_kind(event.owner.kind))
            artifact_error(
                "Game-entry handoff scheduler event is invalid.");
    }
}

auto target_key(
    const GameEntryHandoffArtifactPayloadTarget& target) noexcept {
    return std::tuple{
        static_cast<std::uint8_t>(target.kind),
        target.memory_operation_index,
        static_cast<std::uint16_t>(target.device.kind),
        target.device.instance,
        target.device_field_id};
}

bool valid_target_shape(
    const GameEntryHandoffArtifactPayloadTarget& target) noexcept {
    switch (target.kind) {
    case GameEntryHandoffArtifactPayloadTargetKind::MemoryOperation:
        return target.device == GameEntryDeviceKey{} &&
               target.device_field_id == 0u;
    case GameEntryHandoffArtifactPayloadTargetKind::DevicePayload:
        return target.memory_operation_index == 0u &&
               valid_device_kind(target.device.kind);
    }
    return false;
}

GameEntryPrivateSliceReference* resolve_target(
    GameEntryHandoff& descriptor,
    const GameEntryHandoffArtifactPayloadTarget& target) {
    if (!valid_target_shape(target))
        artifact_error("Game-entry handoff payload target is invalid.");
    switch (target.kind) {
    case GameEntryHandoffArtifactPayloadTargetKind::MemoryOperation: {
        if (target.memory_operation_index >=
            descriptor.memory_operations.size())
            artifact_error(
                "Game-entry handoff memory payload target is out of range.");
        auto& operation =
            descriptor.memory_operations[target.memory_operation_index];
        if (operation.kind !=
            GameEntryMemoryOperationKind::CopyPrivateSlice)
            artifact_error(
                "Game-entry handoff payload targets a non-copy operation.");
        return &operation.private_slice;
    }
    case GameEntryHandoffArtifactPayloadTargetKind::DevicePayload:
        for (auto& device : descriptor.devices) {
            if (device.key != target.device)
                continue;
            for (auto& payload : device.payloads) {
                if (payload.field_id == target.device_field_id)
                    return &payload.private_slice;
            }
            break;
        }
        artifact_error(
            "Game-entry handoff device payload target does not exist.");
    }
    artifact_error("Game-entry handoff payload target is invalid.");
}

const GameEntryPrivateSliceReference* resolve_target(
    const GameEntryHandoff& descriptor,
    const GameEntryHandoffArtifactPayloadTarget& target) {
    return resolve_target(
        const_cast<GameEntryHandoff&>(descriptor), target);
}

std::size_t expected_private_slice_count(
    const GameEntryHandoff& descriptor) {
    std::size_t count = 0u;
    for (const auto& operation : descriptor.memory_operations) {
        if (operation.kind ==
            GameEntryMemoryOperationKind::CopyPrivateSlice)
            ++count;
    }
    for (const auto& device : descriptor.devices)
        count = checked_add(count, device.payloads.size());
    return count;
}

struct PayloadEntry {
    std::string name;
    GameEntryHandoffArtifactPayloadTarget target;
    std::uint64_t offset = 0u;
    std::uint32_t size = 0u;
    std::string sha256;
    std::vector<std::uint8_t> bytes;
};

void write_payload_entry(BinaryWriter& writer,
                         const PayloadEntry& entry) {
    writer.enumeration(entry.target.kind);
    writer.u32(entry.target.memory_operation_index);
    writer.enumeration(entry.target.device.kind);
    writer.u32(entry.target.device.instance);
    writer.u32(entry.target.device_field_id);
    writer.u32(static_cast<std::uint32_t>(entry.name.size()));
    writer.u64(entry.offset);
    writer.u64(entry.size);
    writer.raw(entry.sha256);
    writer.raw(entry.name);
    writer.zeros(artifact_payload_name_size - entry.name.size());
}

PayloadEntry read_payload_entry(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != artifact_payload_entry_size)
        artifact_error(
            "Game-entry handoff payload table entry has an invalid size.");
    BinaryReader reader(bytes);
    PayloadEntry entry;
    entry.target.kind =
        reader.enumeration<GameEntryHandoffArtifactPayloadTargetKind>();
    entry.target.memory_operation_index = reader.u32();
    entry.target.device.kind =
        reader.enumeration<GameEntryDeviceKind>();
    const auto instance = reader.u32();
    if (instance > std::numeric_limits<std::uint16_t>::max())
        artifact_error(
            "Game-entry handoff payload device instance is invalid.");
    entry.target.device.instance = static_cast<std::uint16_t>(instance);
    entry.target.device_field_id = reader.u32();
    const auto name_size = reader.u32();
    entry.offset = reader.u64();
    const auto size = reader.u64();
    if (size == 0u || size > std::numeric_limits<std::uint32_t>::max())
        artifact_error(
            "Game-entry handoff payload size is invalid.");
    entry.size = static_cast<std::uint32_t>(size);
    const auto sha = reader.take(artifact_sha_text_size);
    entry.sha256.assign(reinterpret_cast<const char*>(sha.data()),
                        sha.size());
    const auto name_bytes = reader.take(artifact_payload_name_size);
    if (name_size == 0u || name_size > name_bytes.size())
        artifact_error(
            "Game-entry handoff payload name size is invalid.");
    entry.name.assign(
        reinterpret_cast<const char*>(name_bytes.data()), name_size);
    if (!std::all_of(name_bytes.begin() + name_size,
                     name_bytes.end(),
                     [](const std::uint8_t value) { return value == 0u; }) ||
        !stable_payload_name(entry.name) ||
        !valid_unprefixed_sha256(entry.sha256) ||
        !valid_target_shape(entry.target) || reader.remaining() != 0u)
        artifact_error(
            "Game-entry handoff payload table entry is invalid.");
    return entry;
}

std::vector<std::uint8_t> encode_artifact(
    const std::span<const std::uint8_t> descriptor,
    const std::span<const PayloadEntry> entries,
    const std::string_view artifact_sha256,
    const std::string_view descriptor_sha256) {
    if (!valid_unprefixed_sha256(artifact_sha256) ||
        !valid_unprefixed_sha256(descriptor_sha256))
        artifact_error(
            "Game-entry handoff artifact hash field is invalid.");
    const auto table_offset =
        checked_add(artifact_header_size, descriptor.size());
    const auto data_offset = checked_add(
        table_offset,
        checked_multiply(entries.size(), artifact_payload_entry_size));
    std::uint64_t file_size = data_offset;
    for (const auto& entry : entries)
        file_size = checked_add(file_size, entry.bytes.size());
    if (file_size > game_entry_handoff_artifact_maximum_size ||
        file_size > std::numeric_limits<std::size_t>::max())
        artifact_error(
            "Game-entry handoff artifact exceeds its hard size limit.");

    BinaryWriter writer;
    writer.raw(artifact_magic);
    writer.u32(game_entry_handoff_artifact_format_version);
    writer.u32(artifact_header_size);
    writer.u64(file_size);
    writer.u64(artifact_header_size);
    writer.u64(descriptor.size());
    writer.u64(table_offset);
    writer.u32(checked_count(entries.size(),
                             artifact_maximum_payload_count));
    writer.u32(artifact_payload_entry_size);
    writer.u64(data_offset);
    writer.raw(artifact_sha256);
    writer.raw(descriptor_sha256);
    writer.raw(descriptor);
    for (const auto& entry : entries)
        write_payload_entry(writer, entry);
    for (const auto& entry : entries)
        writer.raw(entry.bytes);
    auto result = std::move(writer).finish();
    if (result.size() != file_size)
        artifact_error(
            "Game-entry handoff artifact layout is inconsistent.");
    return result;
}

struct ParsedArtifact {
    std::string artifact_sha256;
    std::string descriptor_sha256;
    std::uint64_t descriptor_offset = 0u;
    std::uint64_t descriptor_size = 0u;
    std::uint64_t payload_data_offset = 0u;
    GameEntryHandoff descriptor;
    std::vector<PayloadEntry> entries;
};

ParsedArtifact parse_artifact(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < artifact_header_size ||
        bytes.size() > game_entry_handoff_artifact_maximum_size)
        artifact_error(
            "Game-entry handoff artifact has an invalid size.");
    BinaryReader reader(bytes.first(artifact_header_size));
    const auto magic = reader.take(artifact_magic.size());
    if (!std::equal(magic.begin(), magic.end(), artifact_magic.begin()))
        artifact_error(
            "Game-entry handoff artifact signature is invalid.");
    const auto version = reader.u32();
    const auto header_size = reader.u32();
    const auto file_size = reader.u64();
    const auto descriptor_offset = reader.u64();
    const auto descriptor_size = reader.u64();
    const auto table_offset = reader.u64();
    const auto payload_count = reader.u32();
    const auto payload_entry_size = reader.u32();
    const auto data_offset = reader.u64();
    const auto artifact_sha = reader.take(artifact_sha_text_size);
    const auto descriptor_sha = reader.take(artifact_sha_text_size);
    if (reader.remaining() != 0u ||
        version != game_entry_handoff_artifact_format_version ||
        header_size != artifact_header_size || file_size != bytes.size() ||
        descriptor_offset != artifact_header_size || descriptor_size == 0u ||
        descriptor_size > artifact_maximum_descriptor_size ||
        table_offset != checked_add(descriptor_offset, descriptor_size) ||
        payload_count > artifact_maximum_payload_count ||
        payload_entry_size != artifact_payload_entry_size ||
        data_offset != checked_add(
                           table_offset,
                           checked_multiply(payload_count,
                                            payload_entry_size)) ||
        data_offset > bytes.size())
        artifact_error(
            "Game-entry handoff artifact header contract is invalid.");

    ParsedArtifact parsed;
    parsed.artifact_sha256.assign(
        reinterpret_cast<const char*>(artifact_sha.data()),
        artifact_sha.size());
    parsed.descriptor_sha256.assign(
        reinterpret_cast<const char*>(descriptor_sha.data()),
        descriptor_sha.size());
    if (!valid_unprefixed_sha256(parsed.artifact_sha256) ||
        !valid_unprefixed_sha256(parsed.descriptor_sha256))
        artifact_error(
            "Game-entry handoff artifact header hash is invalid.");
    parsed.descriptor_offset = descriptor_offset;
    parsed.descriptor_size = descriptor_size;
    parsed.payload_data_offset = data_offset;

    const auto descriptor_bytes = bytes.subspan(
        static_cast<std::size_t>(descriptor_offset),
        static_cast<std::size_t>(descriptor_size));
    if (hash_bytes(descriptor_bytes) != parsed.descriptor_sha256)
        artifact_error(
            "Game-entry handoff descriptor SHA-256 is invalid.");
    parsed.descriptor = deserialize_descriptor(descriptor_bytes);
    validate_descriptor_structure(parsed.descriptor);

    parsed.entries.reserve(payload_count);
    std::set<std::string> names;
    std::uint64_t expected_offset = data_offset;
    for (std::uint32_t index = 0u; index < payload_count; ++index) {
        const auto entry_offset = checked_add(
            table_offset,
            checked_multiply(index, artifact_payload_entry_size));
        auto entry = read_payload_entry(bytes.subspan(
            static_cast<std::size_t>(entry_offset),
            artifact_payload_entry_size));
        if ((index != 0u &&
             target_key(parsed.entries.back().target) >=
                 target_key(entry.target)) ||
            !names.insert(entry.name).second ||
            entry.offset != expected_offset ||
            entry.offset > bytes.size() ||
            entry.size > bytes.size() - entry.offset)
            artifact_error(
                "Game-entry handoff payload table order or bounds are invalid.");
        const auto payload = bytes.subspan(
            static_cast<std::size_t>(entry.offset), entry.size);
        if (hash_bytes(payload) != entry.sha256)
            artifact_error(
                "Game-entry handoff private slice SHA-256 is invalid.");
        expected_offset = checked_add(expected_offset, entry.size);
        parsed.entries.push_back(std::move(entry));
    }
    if (expected_offset != bytes.size() ||
        parsed.entries.size() !=
            expected_private_slice_count(parsed.descriptor))
        artifact_error(
            "Game-entry handoff payload coverage is incomplete.");

    auto canonical = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    std::fill_n(canonical.begin() + artifact_sha_offset,
                artifact_sha_text_size * 2u,
                static_cast<std::uint8_t>('0'));
    const auto canonical_descriptor =
        serialize_descriptor(parsed.descriptor, true);
    if (canonical_descriptor.size() != descriptor_size)
        artifact_error(
            "Game-entry handoff canonical descriptor layout changed.");
    std::copy(canonical_descriptor.begin(),
              canonical_descriptor.end(),
              canonical.begin() +
                  static_cast<std::ptrdiff_t>(descriptor_offset));
    if (hash_bytes(canonical) != parsed.artifact_sha256)
        artifact_error(
            "Game-entry handoff artifact SHA-256 is invalid.");

    const auto artifact_identity =
        std::string(sha256_prefix) + parsed.artifact_sha256;
    for (const auto& entry : parsed.entries) {
        const auto* reference =
            resolve_target(parsed.descriptor, entry.target);
        if (reference->artifact_identity != artifact_identity ||
            reference->artifact_offset != entry.offset ||
            reference->size != entry.size ||
            reference->byte_identity !=
                std::string(sha256_prefix) + entry.sha256)
            artifact_error(
                "Game-entry handoff descriptor payload binding is invalid.");
    }
    return parsed;
}

void require_regular_nonsymlink_file(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        artifact_error(
            "Game-entry handoff artifact must be a regular non-symlink file.");
}

std::filesystem::path canonical_regular_file(
    const std::filesystem::path& path) {
    if (path.empty())
        artifact_error("Game-entry handoff artifact path is empty.");
    require_regular_nonsymlink_file(path);
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error)
        artifact_error(
            "Game-entry handoff artifact path cannot be canonicalized.");
    require_regular_nonsymlink_file(canonical);
    return canonical;
}

std::vector<std::uint8_t> read_artifact_file(
    const std::filesystem::path& path) {
    require_regular_nonsymlink_file(path);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        artifact_error("Game-entry handoff artifact cannot be opened.");
    const auto end = input.tellg();
    if (end < 0 ||
        static_cast<std::uint64_t>(end) >
            game_entry_handoff_artifact_maximum_size)
        artifact_error(
            "Game-entry handoff artifact exceeds its hard size limit.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    if (!input)
        artifact_error("Game-entry handoff artifact cannot be read.");
    require_regular_nonsymlink_file(path);
    std::error_code error;
    const auto size_after = std::filesystem::file_size(path, error);
    if (error || size_after != bytes.size())
        artifact_error(
            "Game-entry handoff artifact changed while it was read.");
    return bytes;
}

void durable_write(const std::filesystem::path& path,
                   const std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    std::FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0)
        file = nullptr;
#else
    auto* file = std::fopen(path.c_str(), "wb");
#endif
    if (file == nullptr)
        artifact_error(
            "Temporary game-entry handoff artifact cannot be created.");
    try {
        const auto written =
            bytes.empty()
                ? 0u
                : std::fwrite(bytes.data(), 1u, bytes.size(), file);
        if (written != bytes.size() || std::fflush(file) != 0)
            artifact_error(
                "Temporary game-entry handoff artifact cannot be written.");
#ifdef _WIN32
        if (::_commit(::_fileno(file)) != 0)
#else
        if (::fsync(::fileno(file)) != 0)
#endif
            artifact_error(
                "Temporary game-entry handoff artifact cannot be synchronized.");
    } catch (...) {
        static_cast<void>(std::fclose(file));
        throw;
    }
    if (std::fclose(file) != 0)
        artifact_error(
            "Temporary game-entry handoff artifact cannot be closed.");
}

std::filesystem::path normalized_destination(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty() ||
        path.filename() == "." || path.filename() == "..")
        artifact_error(
            "Game-entry handoff artifact destination is invalid.");
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(absolute.parent_path());
    std::error_code error;
    const auto parent =
        std::filesystem::canonical(absolute.parent_path(), error);
    if (error)
        artifact_error(
            "Game-entry handoff artifact destination cannot be canonicalized.");
    const auto parent_status =
        std::filesystem::symlink_status(parent, error);
    if (error || !std::filesystem::is_directory(parent_status) ||
        std::filesystem::is_symlink(parent_status))
        artifact_error(
            "Game-entry handoff artifact parent is not a canonical directory.");
    const auto result = parent / absolute.filename();
    const auto status = std::filesystem::symlink_status(result, error);
    const auto missing =
        (!error && status.type() == std::filesystem::file_type::not_found) ||
        error == std::errc::no_such_file_or_directory;
    if (!missing && !error &&
        (std::filesystem::is_symlink(status) ||
         !std::filesystem::is_regular_file(status)))
        artifact_error(
            "Game-entry handoff artifact destination is not a regular file.");
    if (!missing && error)
        artifact_error(
            "Game-entry handoff artifact destination cannot be inspected.");
    return result;
}

std::filesystem::path temporary_path(
    const std::filesystem::path& destination) {
    for (std::size_t attempt = 1u; attempt <= 1024u; ++attempt) {
        auto candidate = destination;
        candidate += ".tmp-" + std::to_string(attempt);
        std::error_code error;
        const auto status =
            std::filesystem::symlink_status(candidate, error);
        if ((!error &&
             status.type() == std::filesystem::file_type::not_found) ||
            error == std::errc::no_such_file_or_directory)
            return candidate;
        if (error)
            artifact_error(
                "Temporary game-entry handoff artifact path cannot be inspected.");
        if (std::filesystem::is_symlink(status))
            artifact_error(
                "Temporary game-entry handoff artifact path is a symlink.");
    }
    artifact_error(
        "No temporary game-entry handoff artifact path is available.");
}

void atomic_replace(const std::filesystem::path& source,
                    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!::MoveFileExW(
            source.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        artifact_error(
            "Game-entry handoff artifact cannot be atomically published.");
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error)
        artifact_error(
            "Game-entry handoff artifact cannot be atomically published.");
#endif
}

} // namespace

std::shared_ptr<GameEntryHandoffArtifact>
GameEntryHandoffArtifact::load_owned(
    const std::filesystem::path& path) {
    const auto canonical = canonical_regular_file(path);
    auto bytes = read_artifact_file(canonical);
    auto parsed = parse_artifact(bytes);

    auto result = std::shared_ptr<GameEntryHandoffArtifact>(
        new GameEntryHandoffArtifact);
    result->canonical_path_ = canonical;
    result->artifact_identity_ =
        std::string(sha256_prefix) + parsed.artifact_sha256;
    result->descriptor_ = std::move(parsed.descriptor);
    result->bytes_ = std::move(bytes);
    result->slices_.reserve(parsed.entries.size());
    for (const auto& entry : parsed.entries) {
        result->slices_.push_back(
            {entry.offset,
             entry.size,
             std::string(sha256_prefix) + entry.sha256});
    }
    return result;
}

std::shared_ptr<GameEntryHandoffArtifact>
GameEntryHandoffArtifact::load(const std::filesystem::path& path) {
    return load_owned(path);
}

std::shared_ptr<GameEntryHandoffArtifact>
GameEntryHandoffArtifact::write(
    const std::filesystem::path& path,
    GameEntryHandoff descriptor,
    const std::span<const GameEntryHandoffArtifactPayload> payloads) {
    if (payloads.size() != expected_private_slice_count(descriptor))
        artifact_error(
            "Game-entry handoff capture does not cover every private slice.");
    if (payloads.size() > artifact_maximum_payload_count)
        artifact_error(
            "Game-entry handoff capture has too many private payloads.");

    std::vector<PayloadEntry> entries;
    entries.reserve(payloads.size());
    std::set<std::string> names;
    std::uint64_t total_payload_size = 0u;
    for (const auto& payload : payloads) {
        if (!stable_payload_name(payload.name) ||
            !valid_target_shape(payload.target) ||
            !names.insert(payload.name).second || payload.bytes.empty() ||
            payload.bytes.size() >
                std::numeric_limits<std::uint32_t>::max())
            artifact_error(
                "Game-entry handoff capture payload is invalid.");
        total_payload_size =
            checked_add(total_payload_size, payload.bytes.size());
        if (total_payload_size > 64u * 1024u * 1024u)
            artifact_error(
                "Game-entry handoff capture payloads exceed their hard limit.");
        PayloadEntry entry;
        entry.name = payload.name;
        entry.target = payload.target;
        entry.size = static_cast<std::uint32_t>(payload.bytes.size());
        entry.bytes.assign(payload.bytes.begin(), payload.bytes.end());
        entry.sha256 = hash_bytes(entry.bytes);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left,
                                                 const auto& right) {
        return target_key(left.target) < target_key(right.target);
    });
    for (std::size_t index = 1u; index < entries.size(); ++index) {
        if (target_key(entries[index - 1u].target) ==
            target_key(entries[index].target))
            artifact_error(
                "Game-entry handoff capture payload target is duplicated.");
    }

    for (auto& entry : entries) {
        auto* reference = resolve_target(descriptor, entry.target);
        reference->artifact_identity =
            std::string(zero_prefixed_sha256);
        reference->artifact_offset = 0u;
        reference->size = entry.size;
        reference->byte_identity =
            std::string(sha256_prefix) + entry.sha256;
        if (entry.target.kind ==
            GameEntryHandoffArtifactPayloadTargetKind::MemoryOperation) {
            auto& operation = descriptor.memory_operations[
                entry.target.memory_operation_index];
            if (operation.size != entry.size)
                artifact_error(
                    "Game-entry handoff memory operation and payload sizes differ.");
            operation.expected_after_identity =
                reference->byte_identity;
        }
    }

    descriptor.binding.descriptor_identity =
        std::string(zero_prefixed_sha256);
    descriptor.expected_semantic_state_identity =
        game_entry_semantic_state_identity(descriptor);
    auto provisional_descriptor =
        serialize_descriptor(descriptor, true);
    auto next_offset = checked_add(
        checked_add(artifact_header_size, provisional_descriptor.size()),
        checked_multiply(entries.size(), artifact_payload_entry_size));
    for (auto& entry : entries) {
        entry.offset = next_offset;
        auto* reference = resolve_target(descriptor, entry.target);
        reference->artifact_offset = entry.offset;
        next_offset = checked_add(next_offset, entry.size);
    }
    if (next_offset > game_entry_handoff_artifact_maximum_size)
        artifact_error(
            "Game-entry handoff artifact exceeds its hard size limit.");

    const auto canonical_descriptor =
        serialize_descriptor(descriptor, true);
    const auto canonical_file = encode_artifact(
        canonical_descriptor,
        entries,
        zero_sha256,
        zero_sha256);
    const auto artifact_sha256 = hash_bytes(canonical_file);
    const auto artifact_identity =
        std::string(sha256_prefix) + artifact_sha256;
    for (auto& entry : entries)
        resolve_target(descriptor, entry.target)->artifact_identity =
            artifact_identity;
    descriptor.expected_semantic_state_identity =
        game_entry_semantic_state_identity(descriptor);
    descriptor.binding.descriptor_identity =
        game_entry_handoff_descriptor_identity(descriptor);
    validate_descriptor_structure(descriptor);

    const auto descriptor_bytes =
        serialize_descriptor(descriptor, false);
    if (descriptor_bytes.size() != canonical_descriptor.size())
        artifact_error(
            "Game-entry handoff descriptor layout changed while binding identities.");
    const auto descriptor_sha256 = hash_bytes(descriptor_bytes);
    const auto artifact_bytes = encode_artifact(
        descriptor_bytes,
        entries,
        artifact_sha256,
        descriptor_sha256);
    static_cast<void>(parse_artifact(artifact_bytes));

    const auto destination = normalized_destination(path);
    const auto temporary = temporary_path(destination);
    try {
        durable_write(temporary, artifact_bytes);
        const auto reread = load_owned(temporary);
        if (reread->artifact_identity() != artifact_identity ||
            reread->descriptor() != descriptor)
            artifact_error(
                "Temporary game-entry handoff artifact validation changed its content.");

        std::error_code status_error;
        const auto destination_status =
            std::filesystem::symlink_status(destination, status_error);
        const auto destination_missing =
            (!status_error &&
             destination_status.type() ==
                 std::filesystem::file_type::not_found) ||
            status_error == std::errc::no_such_file_or_directory;
        if (!destination_missing && !status_error &&
            (std::filesystem::is_symlink(destination_status) ||
             !std::filesystem::is_regular_file(destination_status)))
            artifact_error(
                "Game-entry handoff artifact destination changed before publish.");
        if (!destination_missing && status_error)
            artifact_error(
                "Game-entry handoff artifact destination cannot be rechecked.");
        atomic_replace(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return load_owned(destination);
}

const std::filesystem::path&
GameEntryHandoffArtifact::canonical_path() const noexcept {
    return canonical_path_;
}

const std::string&
GameEntryHandoffArtifact::artifact_identity() const noexcept {
    return artifact_identity_;
}

const GameEntryHandoff&
GameEntryHandoffArtifact::descriptor() const noexcept {
    return descriptor_;
}

std::uint64_t GameEntryHandoffArtifact::file_size() const noexcept {
    return bytes_.size();
}

GameEntryHandoffProvider GameEntryHandoffArtifact::provider() noexcept {
    return {this, describe_callback, read_callback};
}

const GameEntryHandoff*
GameEntryHandoffArtifact::describe_callback(
    void* context,
    const GameEntryHandoffRequest& request) noexcept {
    static_cast<void>(request);
    if (context == nullptr)
        return nullptr;
    return &static_cast<GameEntryHandoffArtifact*>(context)->descriptor_;
}

bool GameEntryHandoffArtifact::read_callback(
    void* context,
    const GameEntryPrivateSliceReference& reference,
    const std::span<std::uint8_t> destination) noexcept {
    try {
        if (context == nullptr)
            return false;
        const auto* artifact =
            static_cast<const GameEntryHandoffArtifact*>(context);
        if (reference.artifact_identity !=
                artifact->artifact_identity_ ||
            destination.size() != reference.size ||
            !valid_game_entry_sha256_identity(reference.byte_identity))
            return false;
        const auto found = std::lower_bound(
            artifact->slices_.begin(),
            artifact->slices_.end(),
            reference.artifact_offset,
            [](const auto& slice, const std::uint64_t offset) {
                return slice.offset < offset;
            });
        if (found == artifact->slices_.end() ||
            found->offset != reference.artifact_offset ||
            found->size != reference.size ||
            found->byte_identity != reference.byte_identity ||
            found->offset > artifact->bytes_.size() ||
            found->size > artifact->bytes_.size() - found->offset)
            return false;
        std::copy_n(
            artifact->bytes_.begin() +
                static_cast<std::ptrdiff_t>(found->offset),
            found->size,
            destination.begin());
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace katana::runtime
