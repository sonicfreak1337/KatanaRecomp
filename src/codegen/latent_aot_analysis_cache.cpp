#include "katana/codegen/latent_aot_analysis_cache.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace katana::codegen {
namespace {

constexpr std::array<std::uint8_t, 8u> cache_magic{
    'K', 'L', 'A', 'T', 'A', 'A', 'C', '1'};
constexpr std::array<std::uint8_t, 8u> key_magic{
    'K', 'L', 'A', 'T', 'A', 'A', 'K', '1'};
constexpr std::size_t sha256_byte_count = 32u;
constexpr std::size_t cache_header_bytes =
    cache_magic.size() + sizeof(std::uint32_t) + sizeof(std::uint8_t) + 3u +
    sha256_byte_count + sizeof(std::uint64_t) + sha256_byte_count;
constexpr std::size_t maximum_implementation_id_bytes = 128u;
constexpr std::size_t maximum_key_material_bytes =
    maximum_latent_aot_analysis_cache_entries * sizeof(std::uint32_t) +
    1'024u;
constexpr IrProgramCacheLimits latent_ir_limits{
    maximum_latent_aot_analysis_cache_artifact_bytes - cache_header_bytes,
    maximum_latent_aot_analysis_cache_functions,
    maximum_latent_aot_analysis_cache_blocks,
    maximum_latent_aot_analysis_cache_instructions,
    maximum_latent_aot_analysis_cache_successors,
    maximum_latent_aot_analysis_cache_targets,
    maximum_latent_aot_analysis_cache_callsites,
    maximum_latent_aot_analysis_cache_parser_depth,
    128u * 1024u * 1024u};

enum class PayloadKind : std::uint8_t {
    Positive = 1u,
    Negative = 2u,
};

class CodecError final : public std::runtime_error {
public:
    CodecError() : std::runtime_error("invalid latent AOT analysis cache") {}
};

[[nodiscard]] std::uint8_t hex_nibble(const char value) {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    throw CodecError();
}

[[nodiscard]] bool lowercase_sha256(const std::string_view value) noexcept {
    return value.size() == sha256_byte_count * 2u &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] std::array<std::uint8_t, sha256_byte_count>
decode_sha256(const std::string_view value) {
    if (!lowercase_sha256(value)) throw CodecError();
    std::array<std::uint8_t, sha256_byte_count> result{};
    for (std::size_t index = 0u; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(hex_nibble(value[index * 2u]) << 4u) |
            hex_nibble(value[index * 2u + 1u]));
    }
    return result;
}

[[nodiscard]] bool stable_implementation_id(
    const std::string_view value) noexcept {
    return !value.empty() && value.size() <= maximum_implementation_id_bytes &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_' || character == '.';
           });
}

class Writer {
public:
    explicit Writer(const std::size_t maximum_bytes)
        : maximum_bytes_(maximum_bytes) {}

    void reserve(const std::size_t bytes) {
        if (bytes > maximum_bytes_) throw CodecError();
        bytes_.reserve(bytes);
    }

    void u8(const std::uint8_t value) {
        require_space(1u);
        bytes_.push_back(value);
    }

    void u16(const std::uint16_t value) {
        require_space(sizeof(value));
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u32(const std::uint32_t value) {
        require_space(sizeof(value));
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u64(const std::uint64_t value) {
        require_space(sizeof(value));
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void raw(const std::span<const std::uint8_t> values) {
        require_space(values.size());
        bytes_.insert(bytes_.end(), values.begin(), values.end());
    }

    void text(const std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw CodecError();
        u32(static_cast<std::uint32_t>(value.size()));
        raw(std::span(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }

    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

private:
    void require_space(const std::size_t bytes) const {
        if (bytes > maximum_bytes_ ||
            bytes_.size() > maximum_bytes_ - bytes)
            throw CodecError();
    }

    std::size_t maximum_bytes_;
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes,
                    const std::size_t maximum_depth =
                        maximum_latent_aot_analysis_cache_parser_depth,
                    const std::size_t maximum_allocation_bytes =
                        128u * 1024u * 1024u)
        : bytes_(bytes),
          maximum_depth_(maximum_depth),
          allocation_bytes_remaining_(maximum_allocation_bytes) {
        if (maximum_depth_ == 0u ||
            allocation_bytes_remaining_ == 0u)
            throw CodecError();
    }

    class DepthScope {
    public:
        explicit DepthScope(Reader& reader) : reader_(&reader) {
            reader_->enter();
        }
        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;
        DepthScope(DepthScope&& other) noexcept
            : reader_(std::exchange(other.reader_, nullptr)) {}
        ~DepthScope() {
            if (reader_ != nullptr) reader_->leave();
        }

    private:
        Reader* reader_;
    };

    [[nodiscard]] DepthScope scoped_depth() {
        return DepthScope(*this);
    }

    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[offset_++];
    }

    [[nodiscard]] std::uint16_t u16() {
        require(sizeof(std::uint16_t));
        std::uint16_t value = 0u;
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            value |= static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(bytes_[offset_++]) << (byte * 8u));
        return value;
    }

    [[nodiscard]] std::uint32_t u32() {
        require(sizeof(std::uint32_t));
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            value |= static_cast<std::uint32_t>(bytes_[offset_++])
                     << (byte * 8u);
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        require(sizeof(std::uint64_t));
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            value |= static_cast<std::uint64_t>(bytes_[offset_++])
                     << (byte * 8u);
        return value;
    }

    [[nodiscard]] std::span<const std::uint8_t> raw(
        const std::size_t count) {
        require(count);
        const auto result = bytes_.subspan(offset_, count);
        offset_ += count;
        return result;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return offset_ == bytes_.size();
    }

    template <typename Value>
    void reserve_vector(const std::size_t count,
                        const std::size_t minimum_encoded_bytes = 1u) {
        if (minimum_encoded_bytes == 0u ||
            count > remaining() / minimum_encoded_bytes ||
            count > std::numeric_limits<std::size_t>::max() /
                        sizeof(Value))
            throw CodecError();
        const auto bytes = count * sizeof(Value);
        if (bytes > allocation_bytes_remaining_) throw CodecError();
        allocation_bytes_remaining_ -= bytes;
    }

private:
    void require(const std::size_t count) const {
        if (count > remaining()) throw CodecError();
    }

    void enter() {
        if (depth_ >= maximum_depth_)
            throw CodecError();
        ++depth_;
    }

    void leave() noexcept {
        --depth_;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t maximum_depth_ = 0u;
    std::size_t offset_ = 0u;
    std::size_t depth_ = 0u;
    std::size_t allocation_bytes_remaining_ = 0u;
};

template <typename Enum>
[[nodiscard]] constexpr std::uint8_t enum_u8(const Enum value) noexcept {
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] bool valid_operand_width(const std::uint8_t value) noexcept {
    switch (static_cast<katana::ir::OperandWidth>(value)) {
    case katana::ir::OperandWidth::None:
    case katana::ir::OperandWidth::Bit1:
    case katana::ir::OperandWidth::Bits4:
    case katana::ir::OperandWidth::Bits8:
    case katana::ir::OperandWidth::Bits12:
    case katana::ir::OperandWidth::Bits16:
    case katana::ir::OperandWidth::Bits32:
    case katana::ir::OperandWidth::Bits64:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_status_bits(const std::uint8_t value) noexcept {
    constexpr std::uint8_t allowed =
        enum_u8(katana::ir::StatusRegisterBit::T) |
        enum_u8(katana::ir::StatusRegisterBit::S) |
        enum_u8(katana::ir::StatusRegisterBit::Q) |
        enum_u8(katana::ir::StatusRegisterBit::M) |
        enum_u8(katana::ir::StatusRegisterBit::Full);
    return (value & static_cast<std::uint8_t>(~allowed)) == 0u;
}

[[nodiscard]] bool valid_accumulator_bits(const std::uint8_t value) noexcept {
    constexpr std::uint8_t allowed =
        enum_u8(katana::ir::AccumulatorRegister::Mach) |
        enum_u8(katana::ir::AccumulatorRegister::Macl);
    return (value & static_cast<std::uint8_t>(~allowed)) == 0u;
}

[[nodiscard]] bool valid_operation(
    const std::underlying_type_t<katana::ir::Operation> value) noexcept {
    return value >=
               static_cast<std::underlying_type_t<katana::ir::Operation>>(
                   katana::ir::Operation::Unknown) &&
           value <=
               static_cast<std::underlying_type_t<katana::ir::Operation>>(
                   katana::ir::Operation::Return);
}

[[nodiscard]] bool valid_special_register(
    const std::underlying_type_t<katana::ir::SpecialRegister> value) noexcept {
    return value >=
               static_cast<std::underlying_type_t<
                   katana::ir::SpecialRegister>>(
                   katana::ir::SpecialRegister::None) &&
           value <=
               static_cast<std::underlying_type_t<
                   katana::ir::SpecialRegister>>(
                   katana::ir::SpecialRegister::Bank7);
}

[[nodiscard]] bool valid_rejection(const std::uint8_t value) noexcept {
    return value >= enum_u8(LatentAotAnalysisRejection::NoEntryPoints) &&
           value <= enum_u8(
                        LatentAotAnalysisRejection::AnalysisContextBudgetExceeded);
}

void require_add(std::size_t& total,
                 const std::size_t count,
                 const std::size_t maximum) {
    if (count > maximum || total > maximum - count) throw CodecError();
    total += count;
}

void write_optional_u32(Writer& output,
                        const std::optional<std::uint32_t> value) {
    output.u8(value.has_value() ? 1u : 0u);
    if (value.has_value()) output.u32(*value);
}

void write_optional_u8(Writer& output,
                       const std::optional<std::uint8_t> value) {
    output.u8(value.has_value() ? 1u : 0u);
    if (value.has_value()) output.u8(*value);
}

[[nodiscard]] std::optional<std::uint32_t> read_optional_u32(
    Reader& input) {
    const auto present = input.u8();
    if (present > 1u) throw CodecError();
    if (present == 0u) return std::nullopt;
    return input.u32();
}

[[nodiscard]] std::optional<std::uint8_t> read_optional_u8(Reader& input) {
    const auto present = input.u8();
    if (present > 1u) throw CodecError();
    if (present == 0u) return std::nullopt;
    return input.u8();
}

void write_instruction(Writer& output,
                       const katana::ir::Instruction& instruction,
                       const std::size_t maximum_targets) {
    const auto original_operation = static_cast<
        std::underlying_type_t<katana::ir::Operation>>(
        instruction.original_operation);
    const auto operation =
        static_cast<std::underlying_type_t<katana::ir::Operation>>(
            instruction.operation);
    const auto special_register = static_cast<
        std::underlying_type_t<katana::ir::SpecialRegister>>(
        instruction.special_register);
    if (!valid_operation(original_operation) || !valid_operation(operation) ||
        !valid_operand_width(enum_u8(instruction.widths.result)) ||
        !valid_operand_width(enum_u8(instruction.widths.input)) ||
        !valid_operand_width(enum_u8(instruction.widths.immediate)) ||
        !valid_operand_width(enum_u8(instruction.widths.displacement)) ||
        !valid_operand_width(enum_u8(instruction.widths.memory)) ||
        !valid_operand_width(enum_u8(instruction.widths.address)) ||
        !valid_status_bits(enum_u8(instruction.status_effects.reads)) ||
        !valid_status_bits(enum_u8(instruction.status_effects.writes)) ||
        enum_u8(instruction.memory_effects.access) >
            enum_u8(katana::ir::MemoryAccessKind::Write) ||
        !valid_operand_width(enum_u8(instruction.memory_effects.width)) ||
        enum_u8(instruction.memory_effects.address_update) >
            enum_u8(katana::ir::AddressUpdateKind::PostIncrement) ||
        enum_u8(instruction.memory_effects.region) >
            enum_u8(katana::ir::MemoryRegionKind::Volatile) ||
        !valid_accumulator_bits(
            enum_u8(instruction.accumulator_effects.reads_if_s_clear)) ||
        !valid_accumulator_bits(
            enum_u8(instruction.accumulator_effects.reads_if_s_set)) ||
        !valid_accumulator_bits(
            enum_u8(instruction.accumulator_effects.writes_if_s_clear)) ||
        !valid_accumulator_bits(
            enum_u8(instruction.accumulator_effects.writes_if_s_set)) ||
        instruction.destination_register > 15u ||
        instruction.source_register > 15u ||
        instruction.branch_register > 15u ||
        !valid_special_register(special_register) ||
        (instruction.forwarded_value_register.has_value() &&
         *instruction.forwarded_value_register > 15u) ||
        enum_u8(instruction.dynamic_target_class) >
            enum_u8(katana::ir::DynamicTargetClass::Unresolved) ||
        enum_u8(instruction.delay_slot.role) >
            enum_u8(katana::ir::DelaySlotRole::Slot) ||
        instruction.resolved_targets.size() > maximum_targets)
        throw CodecError();

    output.u32(instruction.source_address);
    output.u16(instruction.original_opcode);
    output.u16(static_cast<std::uint16_t>(original_operation));
    output.u16(static_cast<std::uint16_t>(operation));

    output.u8(enum_u8(instruction.widths.result));
    output.u8(enum_u8(instruction.widths.input));
    output.u8(enum_u8(instruction.widths.immediate));
    output.u8(enum_u8(instruction.widths.displacement));
    output.u8(enum_u8(instruction.widths.memory));
    output.u8(enum_u8(instruction.widths.address));

    output.u8(enum_u8(instruction.status_effects.reads));
    output.u8(enum_u8(instruction.status_effects.writes));

    output.u8(enum_u8(instruction.memory_effects.access));
    output.u8(enum_u8(instruction.memory_effects.width));
    output.u8(instruction.memory_effects.access_count);
    output.u8(enum_u8(instruction.memory_effects.address_update));
    output.u8(instruction.memory_effects.updated_register_count);
    output.u8(enum_u8(instruction.memory_effects.region));

    output.u8(enum_u8(instruction.accumulator_effects.reads_if_s_clear));
    output.u8(enum_u8(instruction.accumulator_effects.reads_if_s_set));
    output.u8(enum_u8(instruction.accumulator_effects.writes_if_s_clear));
    output.u8(enum_u8(instruction.accumulator_effects.writes_if_s_set));

    output.u8(instruction.destination_register);
    output.u8(instruction.source_register);
    output.u8(instruction.branch_register);
    output.u32(std::bit_cast<std::uint32_t>(instruction.immediate));
    output.u32(std::bit_cast<std::uint32_t>(instruction.displacement));
    output.u8(static_cast<std::uint8_t>(special_register));
    write_optional_u32(output, instruction.effective_address);
    write_optional_u32(output, instruction.target_address);
    output.u32(static_cast<std::uint32_t>(
        instruction.resolved_targets.size()));
    for (const auto target : instruction.resolved_targets)
        output.u32(target);
    write_optional_u8(output, instruction.forwarded_value_register);
    output.u8(enum_u8(instruction.dynamic_target_class));
    output.u8(enum_u8(instruction.delay_slot.role));
    write_optional_u32(output, instruction.delay_slot.counterpart_address);
    output.u8(instruction.is_privileged ? 1u : 0u);
    output.u8(instruction.branch_register_relative ? 1u : 0u);
}

[[nodiscard]] katana::ir::Instruction read_instruction(
    Reader& input,
    std::size_t& target_count,
    const std::size_t maximum_targets) {
    katana::ir::Instruction instruction;
    instruction.source_address = input.u32();
    instruction.original_opcode = input.u16();
    const auto original_operation = input.u16();
    const auto operation = input.u16();
    if (!valid_operation(
            static_cast<std::underlying_type_t<katana::ir::Operation>>(
                original_operation)) ||
        !valid_operation(
            static_cast<std::underlying_type_t<katana::ir::Operation>>(
                operation)))
        throw CodecError();
    instruction.original_operation =
        static_cast<katana::ir::Operation>(original_operation);
    instruction.operation = static_cast<katana::ir::Operation>(operation);

    const auto result_width = input.u8();
    const auto input_width = input.u8();
    const auto immediate_width = input.u8();
    const auto displacement_width = input.u8();
    const auto memory_width = input.u8();
    const auto address_width = input.u8();
    if (!valid_operand_width(result_width) ||
        !valid_operand_width(input_width) ||
        !valid_operand_width(immediate_width) ||
        !valid_operand_width(displacement_width) ||
        !valid_operand_width(memory_width) ||
        !valid_operand_width(address_width))
        throw CodecError();
    instruction.widths = {
        static_cast<katana::ir::OperandWidth>(result_width),
        static_cast<katana::ir::OperandWidth>(input_width),
        static_cast<katana::ir::OperandWidth>(immediate_width),
        static_cast<katana::ir::OperandWidth>(displacement_width),
        static_cast<katana::ir::OperandWidth>(memory_width),
        static_cast<katana::ir::OperandWidth>(address_width)};

    const auto status_reads = input.u8();
    const auto status_writes = input.u8();
    if (!valid_status_bits(status_reads) || !valid_status_bits(status_writes))
        throw CodecError();
    instruction.status_effects = {
        static_cast<katana::ir::StatusRegisterBit>(status_reads),
        static_cast<katana::ir::StatusRegisterBit>(status_writes)};

    const auto memory_access = input.u8();
    const auto memory_effect_width = input.u8();
    const auto memory_access_count = input.u8();
    const auto address_update = input.u8();
    const auto updated_register_count = input.u8();
    const auto memory_region = input.u8();
    if (memory_access > enum_u8(katana::ir::MemoryAccessKind::Write) ||
        !valid_operand_width(memory_effect_width) ||
        address_update >
            enum_u8(katana::ir::AddressUpdateKind::PostIncrement) ||
        memory_region > enum_u8(katana::ir::MemoryRegionKind::Volatile))
        throw CodecError();
    instruction.memory_effects = {
        static_cast<katana::ir::MemoryAccessKind>(memory_access),
        static_cast<katana::ir::OperandWidth>(memory_effect_width),
        memory_access_count,
        static_cast<katana::ir::AddressUpdateKind>(address_update),
        updated_register_count,
        static_cast<katana::ir::MemoryRegionKind>(memory_region)};

    const auto accumulator_reads_clear = input.u8();
    const auto accumulator_reads_set = input.u8();
    const auto accumulator_writes_clear = input.u8();
    const auto accumulator_writes_set = input.u8();
    if (!valid_accumulator_bits(accumulator_reads_clear) ||
        !valid_accumulator_bits(accumulator_reads_set) ||
        !valid_accumulator_bits(accumulator_writes_clear) ||
        !valid_accumulator_bits(accumulator_writes_set))
        throw CodecError();
    instruction.accumulator_effects = {
        static_cast<katana::ir::AccumulatorRegister>(
            accumulator_reads_clear),
        static_cast<katana::ir::AccumulatorRegister>(accumulator_reads_set),
        static_cast<katana::ir::AccumulatorRegister>(
            accumulator_writes_clear),
        static_cast<katana::ir::AccumulatorRegister>(
            accumulator_writes_set)};

    instruction.destination_register = input.u8();
    instruction.source_register = input.u8();
    instruction.branch_register = input.u8();
    if (instruction.destination_register > 15u ||
        instruction.source_register > 15u ||
        instruction.branch_register > 15u)
        throw CodecError();
    instruction.immediate = std::bit_cast<std::int32_t>(input.u32());
    instruction.displacement = std::bit_cast<std::int32_t>(input.u32());
    const auto special_register = input.u8();
    if (!valid_special_register(
            static_cast<std::underlying_type_t<
                katana::ir::SpecialRegister>>(special_register)))
        throw CodecError();
    instruction.special_register =
        static_cast<katana::ir::SpecialRegister>(special_register);
    instruction.effective_address = read_optional_u32(input);
    instruction.target_address = read_optional_u32(input);

    const auto resolved_target_count =
        static_cast<std::size_t>(input.u32());
    require_add(target_count,
                resolved_target_count,
                maximum_targets);
    {
        auto depth = input.scoped_depth();
        input.reserve_vector<std::uint32_t>(
            resolved_target_count, sizeof(std::uint32_t));
        instruction.resolved_targets.reserve(resolved_target_count);
        for (std::size_t index = 0u; index < resolved_target_count; ++index)
            instruction.resolved_targets.push_back(input.u32());
    }

    instruction.forwarded_value_register = read_optional_u8(input);
    if (instruction.forwarded_value_register.has_value() &&
        *instruction.forwarded_value_register > 15u)
        throw CodecError();
    const auto dynamic_target_class = input.u8();
    if (dynamic_target_class >
        enum_u8(katana::ir::DynamicTargetClass::Unresolved))
        throw CodecError();
    instruction.dynamic_target_class =
        static_cast<katana::ir::DynamicTargetClass>(dynamic_target_class);
    const auto delay_role = input.u8();
    if (delay_role > enum_u8(katana::ir::DelaySlotRole::Slot))
        throw CodecError();
    instruction.delay_slot.role =
        static_cast<katana::ir::DelaySlotRole>(delay_role);
    instruction.delay_slot.counterpart_address = read_optional_u32(input);
    const auto privileged = input.u8();
    const auto branch_register_relative = input.u8();
    if (privileged > 1u || branch_register_relative > 1u)
        throw CodecError();
    instruction.is_privileged = privileged != 0u;
    instruction.branch_register_relative = branch_register_relative != 0u;
    return instruction;
}

[[nodiscard]] std::vector<std::uint8_t> serialize_program_payload(
    const std::span<const katana::ir::Function> program,
    const IrProgramCacheLimits& limits) {
    if (limits.maximum_payload_bytes < sizeof(std::uint32_t) ||
        limits.maximum_functions == 0u ||
        limits.maximum_blocks == 0u ||
        limits.maximum_instructions == 0u ||
        limits.maximum_successors == 0u ||
        limits.maximum_targets == 0u ||
        limits.maximum_callsites == 0u ||
        limits.maximum_parser_depth == 0u ||
        limits.maximum_allocation_bytes == 0u ||
        program.empty() ||
        program.size() > limits.maximum_functions)
        throw CodecError();
    Writer output(limits.maximum_payload_bytes);
    output.u32(static_cast<std::uint32_t>(program.size()));

    std::size_t block_count = 0u;
    std::size_t instruction_count = 0u;
    std::size_t successor_count = 0u;
    std::size_t target_count = 0u;
    std::size_t callsite_count = 0u;
    for (const auto& function : program) {
        require_add(block_count,
                    function.blocks.size(),
                    limits.maximum_blocks);
        require_add(callsite_count,
                    function.direct_callees.size(),
                    limits.maximum_callsites);
        require_add(callsite_count,
                    function.indirect_call_sites.size(),
                    limits.maximum_callsites);

        output.u32(function.entry_address);
        output.u32(static_cast<std::uint32_t>(function.blocks.size()));
        for (const auto& block : function.blocks) {
            require_add(instruction_count,
                        block.instructions.size(),
                        limits.maximum_instructions);
            require_add(successor_count,
                        block.successors.size(),
                        limits.maximum_successors);
            require_add(target_count,
                        block.guarded_case_ownership_targets.size(),
                        limits.maximum_targets);

            output.u32(block.start_address);
            output.u32(static_cast<std::uint32_t>(
                block.instructions.size()));
            for (const auto& instruction : block.instructions) {
                require_add(target_count,
                            instruction.resolved_targets.size(),
                            limits.maximum_targets);
                write_instruction(output, instruction, limits.maximum_targets);
            }
            output.u32(static_cast<std::uint32_t>(block.successors.size()));
            for (const auto successor : block.successors)
                output.u32(successor);
            output.u32(static_cast<std::uint32_t>(
                block.guarded_case_ownership_targets.size()));
            for (const auto target :
                 block.guarded_case_ownership_targets)
                output.u32(target);
            output.u8(block.has_indirect_successor ? 1u : 0u);
        }
        output.u32(static_cast<std::uint32_t>(
            function.direct_callees.size()));
        for (const auto callee : function.direct_callees)
            output.u32(callee);
        output.u32(static_cast<std::uint32_t>(
            function.indirect_call_sites.size()));
        for (const auto call_site : function.indirect_call_sites)
            output.u32(call_site);
    }
    return std::move(output).finish();
}

[[nodiscard]] std::vector<katana::ir::Function> parse_program_payload(
    Reader& input,
    const IrProgramCacheLimits& limits) {
    const auto function_count = static_cast<std::size_t>(input.u32());
    if (function_count == 0u ||
        function_count > limits.maximum_functions)
        throw CodecError();

    std::size_t block_count = 0u;
    std::size_t instruction_count = 0u;
    std::size_t successor_count = 0u;
    std::size_t target_count = 0u;
    std::size_t callsite_count = 0u;
    std::vector<katana::ir::Function> program;
    input.reserve_vector<katana::ir::Function>(
        function_count, sizeof(std::uint32_t) * 2u);
    program.reserve(function_count);
    auto program_depth = input.scoped_depth();
    for (std::size_t function_index = 0u;
         function_index < function_count;
         ++function_index) {
        katana::ir::Function function;
        function.entry_address = input.u32();
        const auto local_block_count =
            static_cast<std::size_t>(input.u32());
        require_add(block_count,
                    local_block_count,
                    limits.maximum_blocks);
        input.reserve_vector<katana::ir::BasicBlock>(
            local_block_count, sizeof(std::uint32_t) * 2u);
        function.blocks.reserve(local_block_count);
        {
            auto block_depth = input.scoped_depth();
            for (std::size_t block_index = 0u;
                 block_index < local_block_count;
                 ++block_index) {
                katana::ir::BasicBlock block;
                block.start_address = input.u32();
                const auto local_instruction_count =
                    static_cast<std::size_t>(input.u32());
                require_add(
                    instruction_count,
                    local_instruction_count,
                    limits.maximum_instructions);
                input.reserve_vector<katana::ir::Instruction>(
                    local_instruction_count, 32u);
                block.instructions.reserve(local_instruction_count);
                {
                    auto instruction_depth = input.scoped_depth();
                    for (std::size_t instruction_index = 0u;
                         instruction_index < local_instruction_count;
                         ++instruction_index) {
                        block.instructions.push_back(
                            read_instruction(
                                input, target_count, limits.maximum_targets));
                    }
                }

                const auto local_successor_count =
                    static_cast<std::size_t>(input.u32());
                require_add(successor_count,
                            local_successor_count,
                            limits.maximum_successors);
                {
                    auto successor_depth = input.scoped_depth();
                    input.reserve_vector<std::uint32_t>(
                        local_successor_count,
                        sizeof(std::uint32_t));
                    block.successors.reserve(local_successor_count);
                    for (std::size_t successor_index = 0u;
                         successor_index < local_successor_count;
                         ++successor_index)
                        block.successors.push_back(input.u32());
                }
                const auto local_ownership_target_count =
                    static_cast<std::size_t>(input.u32());
                require_add(target_count,
                            local_ownership_target_count,
                            limits.maximum_targets);
                {
                    auto ownership_depth = input.scoped_depth();
                    input.reserve_vector<std::uint32_t>(
                        local_ownership_target_count,
                        sizeof(std::uint32_t));
                    block.guarded_case_ownership_targets.reserve(
                        local_ownership_target_count);
                    for (std::size_t target_index = 0u;
                         target_index < local_ownership_target_count;
                         ++target_index)
                        block.guarded_case_ownership_targets.push_back(
                            input.u32());
                }
                const auto has_indirect_successor = input.u8();
                if (has_indirect_successor > 1u) throw CodecError();
                block.has_indirect_successor =
                    has_indirect_successor != 0u;
                function.blocks.push_back(std::move(block));
            }
        }

        const auto direct_callee_count =
            static_cast<std::size_t>(input.u32());
        require_add(callsite_count,
                    direct_callee_count,
                    limits.maximum_callsites);
        {
            auto callees_depth = input.scoped_depth();
            input.reserve_vector<std::uint32_t>(
                direct_callee_count, sizeof(std::uint32_t));
            function.direct_callees.reserve(direct_callee_count);
            for (std::size_t callee_index = 0u;
                 callee_index < direct_callee_count;
                 ++callee_index)
                function.direct_callees.push_back(input.u32());
        }
        const auto indirect_call_site_count =
            static_cast<std::size_t>(input.u32());
        require_add(callsite_count,
                    indirect_call_site_count,
                    limits.maximum_callsites);
        {
            auto callsites_depth = input.scoped_depth();
            input.reserve_vector<std::uint32_t>(
                indirect_call_site_count,
                sizeof(std::uint32_t));
            function.indirect_call_sites.reserve(indirect_call_site_count);
            for (std::size_t callsite_index = 0u;
                 callsite_index < indirect_call_site_count;
                 ++callsite_index)
                function.indirect_call_sites.push_back(input.u32());
        }
        program.push_back(std::move(function));
    }
    return program;
}

[[nodiscard]] std::vector<std::uint8_t> make_envelope(
    const std::string_view key,
    const PayloadKind kind,
    const std::span<const std::uint8_t> payload) {
    const auto decoded_key = decode_sha256(key);
    if (payload.size() >
            maximum_latent_aot_analysis_cache_artifact_bytes -
                cache_header_bytes)
        throw CodecError();
    const auto payload_view = std::string_view(
        reinterpret_cast<const char*>(payload.data()), payload.size());
    const auto payload_sha =
        decode_sha256(katana::io::sha256_bytes(payload_view));

    Writer output(maximum_latent_aot_analysis_cache_artifact_bytes);
    output.reserve(cache_header_bytes + payload.size());
    output.raw(cache_magic);
    output.u32(latent_aot_analysis_cache_schema_version);
    output.u8(enum_u8(kind));
    output.u8(0u);
    output.u8(0u);
    output.u8(0u);
    output.raw(decoded_key);
    output.u64(static_cast<std::uint64_t>(payload.size()));
    output.raw(payload_sha);
    output.raw(payload);
    return std::move(output).finish();
}

} // namespace

std::vector<std::uint8_t> serialize_ir_program_cache_payload(
    const std::span<const katana::ir::Function> program,
    const IrProgramCacheLimits& limits) {
    return serialize_program_payload(program, limits);
}

std::vector<katana::ir::Function> parse_ir_program_cache_payload(
    const std::span<const std::uint8_t> payload,
    const IrProgramCacheLimits& limits) {
    if (payload.size() > limits.maximum_payload_bytes) throw CodecError();
    Reader input(payload,
                 limits.maximum_parser_depth,
                 limits.maximum_allocation_bytes);
    auto program = parse_program_payload(input, limits);
    if (!input.empty()) throw CodecError();
    return program;
}

std::string make_latent_aot_analysis_cache_key(
    const LatentAotAnalysisCacheKeyInputs& inputs) {
    const auto valid_candidate_size =
        inputs.exact_candidate
            ? inputs.byte_size >= 2u && (inputs.byte_size & 1u) == 0u
            : inputs.byte_size >= 4u && (inputs.byte_size & 3u) == 0u;
    if (!lowercase_sha256(inputs.byte_sha256) || !valid_candidate_size ||
        static_cast<std::uint64_t>(inputs.source_address) +
                inputs.byte_size >
            0x1'0000'0000ull ||
        inputs.entry_offsets.empty() ||
        inputs.entry_offsets.size() >
            maximum_latent_aot_analysis_cache_entries ||
        inputs.address_layout_schema == 0u ||
        inputs.maximum_entry_scan_instructions == 0u ||
        inputs.maximum_native_instructions == 0u ||
        inputs.maximum_blocks == 0u || inputs.maximum_functions == 0u ||
        inputs.maximum_analysis_iterations == 0u ||
        inputs.maximum_analysis_contexts == 0u || inputs.analyzer_abi == 0u ||
        !stable_implementation_id(inputs.analyzer_implementation_id) ||
        inputs.ir_schema == 0u || inputs.optimizer_schema == 0u)
        throw std::invalid_argument(
            "Latent-AOT-Analysecache-Key ist unvollstaendig.");

    auto entry_offsets = inputs.entry_offsets;
    std::sort(entry_offsets.begin(), entry_offsets.end());
    entry_offsets.erase(
        std::unique(entry_offsets.begin(), entry_offsets.end()),
        entry_offsets.end());
    if (entry_offsets.empty() ||
        std::any_of(
            entry_offsets.begin(),
            entry_offsets.end(),
            [&inputs](const std::uint32_t offset) {
                return offset >= inputs.byte_size || (offset & 1u) != 0u;
            }) ||
        (!inputs.exact_candidate &&
         (entry_offsets.size() != 1u || entry_offsets.front() != 0u)))
        throw std::invalid_argument(
            "Latent-AOT-Analysecache-Entries sind ungueltig.");

    Writer canonical(maximum_key_material_bytes);
    canonical.raw(key_magic);
    canonical.u32(latent_aot_analysis_cache_schema_version);
    canonical.raw(decode_sha256(inputs.byte_sha256));
    canonical.u32(inputs.byte_size);
    canonical.u32(static_cast<std::uint32_t>(entry_offsets.size()));
    for (const auto offset : entry_offsets)
        canonical.u32(offset);
    canonical.u8(inputs.exact_candidate ? 1u : 0u);
    canonical.u32(inputs.source_address);
    canonical.u32(inputs.address_layout_schema);
    canonical.u64(
        static_cast<std::uint64_t>(inputs.maximum_entry_scan_instructions));
    canonical.u64(
        static_cast<std::uint64_t>(inputs.maximum_native_instructions));
    canonical.u64(static_cast<std::uint64_t>(inputs.maximum_blocks));
    canonical.u64(static_cast<std::uint64_t>(inputs.maximum_functions));
    canonical.u64(
        static_cast<std::uint64_t>(inputs.maximum_analysis_iterations));
    canonical.u64(
        static_cast<std::uint64_t>(inputs.maximum_analysis_contexts));
    canonical.u32(inputs.analyzer_abi);
    canonical.text(inputs.analyzer_implementation_id);
    canonical.u32(inputs.ir_schema);
    canonical.u32(inputs.optimizer_schema);
    const auto bytes = std::move(canonical).finish();
    return katana::io::sha256_bytes(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::vector<std::uint8_t> serialize_latent_aot_positive_cache(
    const std::string_view key,
    const std::span<const katana::ir::Function> program) {
    const auto payload = serialize_program_payload(program, latent_ir_limits);
    return make_envelope(key, PayloadKind::Positive, payload);
}

std::vector<std::uint8_t> serialize_latent_aot_negative_cache(
    const std::string_view key,
    const LatentAotAnalysisRejection rejection) {
    const auto raw_rejection = enum_u8(rejection);
    if (!valid_rejection(raw_rejection))
        throw std::invalid_argument(
            "Latent-AOT-Analysecache-Ablehnung ist nicht cachebar.");
    const std::array payload{raw_rejection};
    return make_envelope(key, PayloadKind::Negative, payload);
}

LatentAotAnalysisCacheParseResult parse_latent_aot_analysis_cache(
    const std::string_view expected_key,
    const std::span<const std::uint8_t> artifact) {
    LatentAotAnalysisCacheParseResult result;
    if (!lowercase_sha256(expected_key) || artifact.size() < cache_header_bytes ||
        artifact.size() > maximum_latent_aot_analysis_cache_artifact_bytes) {
        result.state = LatentAotAnalysisCacheState::Corrupt;
        return result;
    }

    try {
        Reader input(artifact);
        if (!std::ranges::equal(input.raw(cache_magic.size()), cache_magic)) {
            result.state = LatentAotAnalysisCacheState::Corrupt;
            return result;
        }
        const auto schema = input.u32();
        if (schema != latent_aot_analysis_cache_schema_version) {
            result.state = LatentAotAnalysisCacheState::Miss;
            return result;
        }
        const auto kind = input.u8();
        if (input.u8() != 0u || input.u8() != 0u || input.u8() != 0u)
            throw CodecError();
        const auto embedded_key = input.raw(sha256_byte_count);
        const auto decoded_expected_key = decode_sha256(expected_key);
        if (!std::ranges::equal(embedded_key, decoded_expected_key)) {
            result.state = LatentAotAnalysisCacheState::Miss;
            return result;
        }

        const auto payload_size_u64 = input.u64();
        if (payload_size_u64 >
            maximum_latent_aot_analysis_cache_artifact_bytes -
                cache_header_bytes)
            throw CodecError();
        const auto payload_size =
            static_cast<std::size_t>(payload_size_u64);
        const auto expected_payload_sha = input.raw(sha256_byte_count);
        if (payload_size != input.remaining()) throw CodecError();
        const auto payload = input.raw(payload_size);
        if (!input.empty()) throw CodecError();
        const auto actual_payload_sha = decode_sha256(
            katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(payload.data()),
                payload.size())));
        if (!std::ranges::equal(expected_payload_sha, actual_payload_sha))
            throw CodecError();

        Reader payload_input(
            payload,
            latent_ir_limits.maximum_parser_depth,
            latent_ir_limits.maximum_allocation_bytes);
        if (kind == enum_u8(PayloadKind::Positive)) {
            result.program =
                parse_program_payload(payload_input, latent_ir_limits);
            if (!payload_input.empty()) throw CodecError();
            result.rejection = LatentAotAnalysisRejection::None;
            result.state = LatentAotAnalysisCacheState::Positive;
            return result;
        }
        if (kind == enum_u8(PayloadKind::Negative)) {
            const auto rejection = payload_input.u8();
            if (!valid_rejection(rejection) || !payload_input.empty())
                throw CodecError();
            result.rejection =
                static_cast<LatentAotAnalysisRejection>(rejection);
            result.state = LatentAotAnalysisCacheState::Negative;
            return result;
        }
        throw CodecError();
    } catch (const CodecError&) {
        return {LatentAotAnalysisCacheState::Corrupt,
                {},
                LatentAotAnalysisRejection::None};
    } catch (const std::bad_alloc&) {
        return {LatentAotAnalysisCacheState::Corrupt,
                {},
                LatentAotAnalysisRejection::None};
    } catch (const std::length_error&) {
        return {LatentAotAnalysisCacheState::Corrupt,
                {},
                LatentAotAnalysisRejection::None};
    }
}

} // namespace katana::codegen
