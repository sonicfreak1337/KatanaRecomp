#include "katana/codegen/latent_aot_analysis_cache.hpp"

#include "katana/io/input_provenance.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t envelope_header_bytes = 88u;
constexpr std::size_t payload_size_offset = 48u;
constexpr std::size_t payload_sha_offset = 56u;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint8_t hex_nibble(const char value) {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    throw std::runtime_error("Ungueltiger Testhash.");
}

void write_u16(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1u) = static_cast<std::uint8_t>(value >> 8u);
}

void write_u32(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint32_t value) {
    for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
        bytes.at(offset + byte) =
            static_cast<std::uint8_t>(value >> (byte * 8u));
}

void write_u64(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint64_t value) {
    for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
        bytes.at(offset + byte) =
            static_cast<std::uint8_t>(value >> (byte * 8u));
}

void refresh_payload_sha(std::vector<std::uint8_t>& artifact) {
    require(
        artifact.size() >= envelope_header_bytes,
        "Testartefakt besitzt keinen vollstaendigen Envelope.");
    const auto payload = std::string_view(
        reinterpret_cast<const char*>(
            artifact.data() + envelope_header_bytes),
        artifact.size() - envelope_header_bytes);
    const auto digest = katana::io::sha256_bytes(payload);
    require(digest.size() == 64u, "Test-SHA-256 war nicht kanonisch.");
    for (std::size_t index = 0u; index < 32u; ++index) {
        artifact.at(payload_sha_offset + index) =
            static_cast<std::uint8_t>(
                static_cast<std::uint8_t>(
                    hex_nibble(digest[index * 2u]) << 4u) |
                hex_nibble(digest[index * 2u + 1u]));
    }
}

katana::codegen::LatentAotAnalysisCacheKeyInputs base_key_inputs() {
    katana::codegen::LatentAotAnalysisCacheKeyInputs inputs;
    inputs.byte_sha256 = std::string(64u, 'a');
    inputs.byte_size = 16u;
    inputs.entry_offsets = {0u};
    inputs.exact_candidate = true;
    inputs.source_address = 0x88000000u;
    inputs.maximum_entry_scan_instructions = 1'024u;
    inputs.maximum_native_instructions = 32'768u;
    inputs.maximum_blocks = 8'192u;
    inputs.maximum_functions = 2'048u;
    inputs.maximum_analysis_iterations = 64u;
    inputs.maximum_analysis_contexts = 65'536u;
    inputs.analyzer_abi = 19u;
    return inputs;
}

std::vector<katana::ir::Function> rich_program() {
    katana::ir::Instruction instruction;
    instruction.source_address = 0x88000000u;
    instruction.original_opcode = 0x402Bu;
    instruction.original_operation = katana::ir::Operation::CallRegister;
    instruction.operation = katana::ir::Operation::CallRegister;
    instruction.widths = {
        katana::ir::OperandWidth::Bits32,
        katana::ir::OperandWidth::Bits32,
        katana::ir::OperandWidth::Bits8,
        katana::ir::OperandWidth::Bits12,
        katana::ir::OperandWidth::Bits32,
        katana::ir::OperandWidth::Bits32};
    instruction.status_effects.reads =
        static_cast<katana::ir::StatusRegisterBit>(3u);
    instruction.status_effects.writes =
        katana::ir::StatusRegisterBit::Full;
    instruction.memory_effects = {
        katana::ir::MemoryAccessKind::Read,
        katana::ir::OperandWidth::Bits32,
        2u,
        katana::ir::AddressUpdateKind::PostIncrement,
        1u,
        katana::ir::MemoryRegionKind::NormalRam};
    instruction.accumulator_effects = {
        katana::ir::AccumulatorRegister::Mach,
        katana::ir::AccumulatorRegister::Macl,
        static_cast<katana::ir::AccumulatorRegister>(3u),
        katana::ir::AccumulatorRegister::None};
    instruction.destination_register = 1u;
    instruction.source_register = 2u;
    instruction.branch_register = 3u;
    instruction.immediate = -123'456;
    instruction.displacement = 0x1234;
    instruction.special_register = katana::ir::SpecialRegister::Pr;
    instruction.effective_address = 0x8800000Cu;
    instruction.target_address = 0x88000008u;
    instruction.resolved_targets = {0x88000008u, 0x8800000Cu};
    instruction.forwarded_value_register = 4u;
    instruction.dynamic_target_class =
        katana::ir::DynamicTargetClass::GuardedComplete;
    instruction.delay_slot.role = katana::ir::DelaySlotRole::Slot;
    instruction.delay_slot.counterpart_address = 0x87FFFFFEu;
    instruction.is_privileged = true;
    instruction.branch_register_relative = true;

    katana::ir::BasicBlock block;
    block.start_address = 0x88000000u;
    block.instructions = {instruction};
    block.successors = {0x88000008u, 0x8800000Cu};
    block.has_indirect_successor = true;

    katana::ir::Function function;
    function.entry_address = 0x88000000u;
    function.blocks = {block};
    function.direct_callees = {0x88000008u};
    function.indirect_call_sites = {0x88000000u};
    return {function};
}

std::vector<katana::ir::Function> minimal_program() {
    katana::ir::Instruction instruction;
    instruction.source_address = 0x88000000u;
    instruction.original_opcode = 0x0009u;
    instruction.original_operation = katana::ir::Operation::Nop;
    instruction.operation = katana::ir::Operation::Nop;

    katana::ir::BasicBlock block;
    block.start_address = 0x88000000u;
    block.instructions = {instruction};

    katana::ir::Function function;
    function.entry_address = 0x88000000u;
    function.blocks = {block};
    return {function};
}

void require_key_miss(
    const std::string& expected_key,
    const std::vector<std::uint8_t>& artifact,
    const katana::codegen::LatentAotAnalysisCacheKeyInputs& changed,
    const std::string& dimension) {
    const auto changed_key =
        katana::codegen::make_latent_aot_analysis_cache_key(changed);
    require(changed_key != expected_key,
            "Cache-Key ignorierte Dimension: " + dimension);
    require(katana::codegen::parse_latent_aot_analysis_cache(
                changed_key, artifact)
                .state ==
                katana::codegen::LatentAotAnalysisCacheState::Miss,
            "Fremder Cache-Key wurde nicht als Miss behandelt: " + dimension);
}

} // namespace

int main() {
    try {
        const auto inputs = base_key_inputs();
        const auto key =
            katana::codegen::make_latent_aot_analysis_cache_key(inputs);
        require(key.size() == 64u, "Cache-Key ist kein SHA-256.");

        const auto source_program = rich_program();
        const auto positive =
            katana::codegen::serialize_latent_aot_positive_cache(
                key, source_program);
        const auto parsed_positive =
            katana::codegen::parse_latent_aot_analysis_cache(key, positive);
        require(
            parsed_positive.state ==
                    katana::codegen::LatentAotAnalysisCacheState::Positive &&
                parsed_positive.rejection ==
                    katana::codegen::LatentAotAnalysisRejection::None &&
                parsed_positive.program.size() == 1u &&
                parsed_positive.program.front().blocks.size() == 1u &&
                parsed_positive.program.front()
                        .blocks.front()
                        .instructions.size() == 1u,
            "Positiver Cache-Roundtrip verlor das Programm.");
        const auto& parsed_instruction =
            parsed_positive.program.front().blocks.front().instructions.front();
        require(
            parsed_instruction.original_operation ==
                    katana::ir::Operation::CallRegister &&
                parsed_instruction.operation ==
                    katana::ir::Operation::CallRegister &&
                parsed_instruction.widths == source_program.front()
                                                   .blocks.front()
                                                   .instructions.front()
                                                   .widths &&
                parsed_instruction.status_effects ==
                    source_program.front()
                        .blocks.front()
                        .instructions.front()
                        .status_effects &&
                parsed_instruction.memory_effects ==
                    source_program.front()
                        .blocks.front()
                        .instructions.front()
                        .memory_effects &&
                parsed_instruction.accumulator_effects ==
                    source_program.front()
                        .blocks.front()
                        .instructions.front()
                        .accumulator_effects &&
                parsed_instruction.effective_address ==
                    std::optional<std::uint32_t>{0x8800000Cu} &&
                parsed_instruction.target_address ==
                    std::optional<std::uint32_t>{0x88000008u} &&
                parsed_instruction.resolved_targets.size() == 2u &&
                parsed_instruction.forwarded_value_register ==
                    std::optional<std::uint8_t>{4u} &&
                parsed_instruction.delay_slot.counterpart_address ==
                    std::optional<std::uint32_t>{0x87FFFFFEu} &&
                parsed_instruction.is_privileged &&
                parsed_instruction.branch_register_relative,
            "Positiver Cache-Roundtrip verlor explizite IR-Felder.");
        require(
            katana::codegen::serialize_latent_aot_positive_cache(
                key, parsed_positive.program) == positive,
            "Positiver Cache-Roundtrip war nicht binaer kanonisch.");

        const auto negative =
            katana::codegen::serialize_latent_aot_negative_cache(
                key,
                katana::codegen::LatentAotAnalysisRejection::
                    ControlFlowIncomplete);
        const auto parsed_negative =
            katana::codegen::parse_latent_aot_analysis_cache(key, negative);
        require(
            parsed_negative.state ==
                    katana::codegen::LatentAotAnalysisCacheState::Negative &&
                parsed_negative.rejection ==
                    katana::codegen::LatentAotAnalysisRejection::
                        ControlFlowIncomplete &&
                parsed_negative.program.empty(),
            "Negativer Cache-Roundtrip verlor den typisierten Grund.");

        auto unordered_entries = inputs;
        unordered_entries.entry_offsets = {4u, 0u, 4u};
        auto canonical_entries = inputs;
        canonical_entries.entry_offsets = {0u, 4u};
        require(
            katana::codegen::make_latent_aot_analysis_cache_key(
                unordered_entries) ==
                katana::codegen::make_latent_aot_analysis_cache_key(
                    canonical_entries),
            "Entryoffset-Set wurde nicht kanonisch sortiert und dedupliziert.");

        auto changed = inputs;
        changed.byte_sha256 = std::string(64u, 'b');
        require_key_miss(key, positive, changed, "Byte-SHA-256");
        changed = inputs;
        changed.byte_size = 18u;
        require_key_miss(key, positive, changed, "Bytegroesse");
        changed = inputs;
        changed.entry_offsets = {2u};
        require_key_miss(key, positive, changed, "Entryoffsets");
        changed = inputs;
        changed.exact_candidate = false;
        require_key_miss(key, positive, changed, "Exact-Flag");
        changed = inputs;
        ++changed.source_address;
        require_key_miss(key, positive, changed, "Quelladresse");
        changed = inputs;
        ++changed.address_layout_schema;
        require_key_miss(key, positive, changed, "Adresslayoutschema");
        changed = inputs;
        ++changed.maximum_entry_scan_instructions;
        require_key_miss(key, positive, changed, "Entryscan-Budget");
        changed = inputs;
        ++changed.maximum_native_instructions;
        require_key_miss(key, positive, changed, "Instruktionsbudget");
        changed = inputs;
        ++changed.maximum_blocks;
        require_key_miss(key, positive, changed, "Blockbudget");
        changed = inputs;
        ++changed.maximum_functions;
        require_key_miss(key, positive, changed, "Funktionsbudget");
        changed = inputs;
        ++changed.maximum_analysis_iterations;
        require_key_miss(key, positive, changed, "Iterationsbudget");
        changed = inputs;
        ++changed.maximum_analysis_contexts;
        require_key_miss(key, positive, changed, "Kontextbudget");
        changed = inputs;
        ++changed.analyzer_abi;
        require_key_miss(key, positive, changed, "Analyzer-ABI");
        changed = inputs;
        changed.analyzer_implementation_id += "-changed";
        require_key_miss(key, positive, changed, "Analyzer-Implementation");
        changed = inputs;
        ++changed.ir_schema;
        require_key_miss(key, positive, changed, "IR-Schema");
        changed = inputs;
        ++changed.optimizer_schema;
        require_key_miss(key, positive, changed, "Optimizer-Schema");

        auto corrupt = positive;
        corrupt.back() ^= 0x80u;
        require(katana::codegen::parse_latent_aot_analysis_cache(key, corrupt)
                    .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Corrupt,
                "Payload-Bitkorruption wurde akzeptiert.");
        corrupt = positive;
        corrupt.at(payload_sha_offset) ^= 0x01u;
        require(katana::codegen::parse_latent_aot_analysis_cache(key, corrupt)
                    .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Corrupt,
                "Kaputte Payload-Pruefsumme wurde akzeptiert.");

        for (const auto truncated_size :
             std::array<std::size_t, 3u>{
                 0u, envelope_header_bytes - 1u, positive.size() - 1u}) {
            const std::vector<std::uint8_t> truncated(
                positive.begin(),
                positive.begin() +
                    static_cast<std::ptrdiff_t>(truncated_size));
            require(
                katana::codegen::parse_latent_aot_analysis_cache(
                    key, truncated)
                        .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Corrupt,
                "Trunkiertes Cacheartefakt wurde akzeptiert.");
        }

        const std::vector<std::uint8_t> oversized(
            katana::codegen::
                    maximum_latent_aot_analysis_cache_artifact_bytes +
                1u);
        require(
            katana::codegen::parse_latent_aot_analysis_cache(key, oversized)
                    .state ==
                katana::codegen::LatentAotAnalysisCacheState::Corrupt,
            "Uebergrosses Cacheartefakt wurde akzeptiert.");

        const std::array<std::uint8_t, 4u> allocation_bomb{
            1u, 0u, 0u, 0u};
        const katana::codegen::IrProgramCacheLimits tiny_allocation{
            1'024u,
            1u,
            1u,
            1u,
            1u,
            1u,
            1u,
            8u,
            1u};
        bool allocation_bomb_bounded = false;
        try {
            static_cast<void>(
                katana::codegen::parse_ir_program_cache_payload(
                    allocation_bomb, tiny_allocation));
        } catch (const std::bad_alloc&) {
            throw std::runtime_error(
                "IR-Codec liess einen Containerzaehler bis bad_alloc "
                "amplifizieren.");
        } catch (const std::runtime_error&) {
            allocation_bomb_bounded = true;
        }
        require(
            allocation_bomb_bounded,
            "IR-Codec ignorierte sein kumulatives Allokationsbudget.");

        const auto minimal = minimal_program();
        const auto minimal_artifact =
            katana::codegen::serialize_latent_aot_positive_cache(key, minimal);
        constexpr std::size_t instruction_start =
            envelope_header_bytes + 20u;

        corrupt = minimal_artifact;
        write_u16(corrupt, instruction_start + 8u, 0xFFFFu);
        refresh_payload_sha(corrupt);
        require(katana::codegen::parse_latent_aot_analysis_cache(key, corrupt)
                    .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Corrupt,
                "Ungueltiger IR-Enumwert wurde akzeptiert.");

        corrupt = minimal_artifact;
        write_u32(
            corrupt,
            envelope_header_bytes,
            static_cast<std::uint32_t>(
                katana::codegen::
                    maximum_latent_aot_analysis_cache_functions +
                1u));
        refresh_payload_sha(corrupt);
        require(katana::codegen::parse_latent_aot_analysis_cache(key, corrupt)
                    .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Corrupt,
                "Uebergrosser Containerzaehler wurde akzeptiert.");

        corrupt = minimal_artifact;
        corrupt.at(instruction_start + 40u) = 2u;
        refresh_payload_sha(corrupt);
        require(katana::codegen::parse_latent_aot_analysis_cache(key, corrupt)
                    .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Corrupt,
                "Ungueltiges Optional-Tag wurde akzeptiert.");

        corrupt = minimal_artifact;
        corrupt.at(instruction_start + 50u) = 2u;
        refresh_payload_sha(corrupt);
        require(katana::codegen::parse_latent_aot_analysis_cache(key, corrupt)
                    .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Corrupt,
                "Ungueltiger Boolwert wurde akzeptiert.");

        corrupt = minimal_artifact;
        corrupt.push_back(0u);
        write_u64(
            corrupt,
            payload_size_offset,
            static_cast<std::uint64_t>(
                corrupt.size() - envelope_header_bytes));
        refresh_payload_sha(corrupt);
        require(katana::codegen::parse_latent_aot_analysis_cache(key, corrupt)
                    .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Corrupt,
                "Trailing Payload-Byte wurde akzeptiert.");

        corrupt = positive;
        write_u32(
            corrupt,
            8u,
            katana::codegen::latent_aot_analysis_cache_schema_version + 1u);
        require(katana::codegen::parse_latent_aot_analysis_cache(key, corrupt)
                    .state ==
                    katana::codegen::LatentAotAnalysisCacheState::Miss,
                "Fremdes Cache-Schema wurde nicht als Miss behandelt.");

        std::cout << "Latent-AOT-Analysecache-Codec-Regressions bestanden.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
