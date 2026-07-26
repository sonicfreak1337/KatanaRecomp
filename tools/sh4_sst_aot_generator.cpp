#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/native_aot_profile.hpp"
#include "katana/codegen/partition.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/runtime/abi.hpp"
#include "katana/runtime/block_abi.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/isa_coverage.hpp"
#include "katana/testing/sh4_sst.hpp"
#include "katana/testing/sh4_sst_codegen.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace analysis = katana::analysis;
namespace codegen = katana::codegen;
namespace io = katana::io;
namespace ir = katana::ir;
namespace runtime = katana::runtime;
namespace sh4 = katana::sh4;
namespace sst = katana::testing;
namespace sst_codegen = katana::testing::sh4_sst;

enum class CorpusScope : std::uint8_t { Smoke, Full };

struct Options {
    std::filesystem::path corpus_root;
    std::filesystem::path output_directory;
    CorpusScope scope = CorpusScope::Smoke;
    std::size_t shard_count = 0u;
};

struct SelectedCase {
    std::string filename;
    std::uint32_t case_index = 0u;
    std::string form_key;
};

struct BlockBinding {
    std::uint32_t address = 0u;
    std::uint32_t size = 0u;
    std::string wrapper;
};

struct FormRecord {
    sst_codegen::SstCodeForm form;
    sst::SstTestCase representative;
    std::string representative_filename;
    std::uint32_t representative_case_index = 0u;
    std::string key;
    std::string digest;
    std::string family;
    std::uint16_t tested_opcode = 0u;
    bool supported = false;
    bool tested_has_delay_slot = false;
    sst::ResultClassification unsupported_classification = sst::ResultClassification::Pass;
    std::string unsupported_reason;
    std::size_t vector_count = 0u;
    std::size_t generated_functions = 0u;
    std::size_t partitions = 0u;
    std::vector<BlockBinding> blocks;
    std::string generated_source;
};

struct CoverageIndex {
    sh4::IsaCoverageReport report;
    std::map<sh4::InstructionKind, const sh4::IsaCoverageEntry*> by_kind;

    CoverageIndex() : report(sh4::build_isa_coverage_report()) {
        for (const auto& entry : report.instructions)
            by_kind.emplace(entry.kind, &entry);
    }

    [[nodiscard]] const sh4::IsaCoverageEntry*
    find(const sh4::InstructionKind kind) const noexcept {
        const auto found = by_kind.find(kind);
        return found == by_kind.end() ? nullptr : found->second;
    }
};

constexpr std::array<std::string_view, 13u> smoke_files{
    "0000mmmm00100011_sz0_pr0.json.bin",
    "0010nnnnmmmm0010_sz0_pr0.json.bin",
    "0010nnnnmmmm0110_sz0_pr0.json.bin",
    "0010nnnnmmmm1001_sz0_pr0.json.bin",
    "0011nnnnmmmm1100_sz0_pr0.json.bin",
    "0100mmmm00001011_sz0_pr0.json.bin",
    "0100mmmm00101011_sz0_pr0.json.bin",
    "0110nnnnmmmm0010_sz0_pr0.json.bin",
    "0110nnnnmmmm0110_sz0_pr0.json.bin",
    "0111nnnniiiiiiii_sz0_pr0.json.bin",
    "10001001dddddddd_sz0_pr0.json.bin",
    "10001101dddddddd_sz0_pr0.json.bin",
    "1010dddddddddddd_sz0_pr0.json.bin",
};

constexpr std::array<std::uint32_t, 5u> smoke_case_indices{0u, 127u, 255u, 383u, 499u};

[[noreturn]] void usage_error(const std::string& message) {
    throw std::invalid_argument(message +
                                "\nUsage: katana-sh4-sst-aot-generator --corpus-root PATH "
                                "--scope smoke|full --output-dir PATH --shard-count N");
}

std::string option_value(const int argc, char** argv, int& index, const std::string_view name) {
    const std::string_view argument(argv[index]);
    const auto prefix = std::string(name) + "=";
    if (argument.starts_with(prefix)) return std::string(argument.substr(prefix.size()));
    if (argument != name) return {};
    if (index + 1 >= argc) usage_error(std::string(name) + " requires a value");
    return argv[++index];
}

std::size_t parse_positive_size(const std::string& text, const std::string_view field) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](const unsigned char character) {
            return std::isdigit(character) != 0;
        }))
        usage_error(std::string(field) + " must be a positive decimal integer");
    std::size_t consumed = 0u;
    const auto value = std::stoull(text, &consumed, 10);
    if (consumed != text.size() || value == 0u || value > std::numeric_limits<std::size_t>::max())
        usage_error(std::string(field) + " is outside the supported range");
    return static_cast<std::size_t>(value);
}

Options parse_options(const int argc, char** argv) {
    Options result;
    bool have_root = false;
    bool have_scope = false;
    bool have_output = false;
    bool have_shards = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: katana-sh4-sst-aot-generator --corpus-root PATH "
                         "--scope smoke|full --output-dir PATH --shard-count N\n";
            std::exit(EXIT_SUCCESS);
        }
        if (const auto value = option_value(argc, argv, index, "--corpus-root"); !value.empty()) {
            if (have_root) usage_error("--corpus-root was provided more than once");
            result.corpus_root = std::filesystem::path(value);
            have_root = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--output-dir"); !value.empty()) {
            if (have_output) usage_error("--output-dir was provided more than once");
            result.output_directory = std::filesystem::path(value);
            have_output = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--scope"); !value.empty()) {
            if (have_scope) usage_error("--scope was provided more than once");
            if (value == "smoke") {
                result.scope = CorpusScope::Smoke;
            } else if (value == "full") {
                result.scope = CorpusScope::Full;
            } else {
                usage_error("--scope must be smoke or full");
            }
            have_scope = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--shard-count"); !value.empty()) {
            if (have_shards) usage_error("--shard-count was provided more than once");
            result.shard_count = parse_positive_size(value, "--shard-count");
            if (result.shard_count > 1024u) usage_error("--shard-count must not exceed 1024");
            have_shards = true;
            continue;
        }
        usage_error("unknown argument: " + std::string(argument));
    }
    if (!have_root || !have_scope || !have_output || !have_shards)
        usage_error("all four generator options are required");
    return result;
}

const char* scope_name(const CorpusScope scope) noexcept {
    return scope == CorpusScope::Smoke ? "smoke" : "full";
}

std::string hex8(const std::uint32_t value) {
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

std::string hex4(const std::uint16_t value) {
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setw(4) << std::setfill('0') << value;
    return output.str();
}

std::string shard_suffix(const std::size_t index) {
    std::ostringstream output;
    output << std::setw(5) << std::setfill('0') << index;
    return output.str();
}

std::string quote_cpp(const std::string_view text) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : text) {
        switch (character) {
        case '\\':
            output << "\\\\";
            break;
        case '"':
            output << "\\\"";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20u || character >= 0x7Fu) {
                output << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

void write_if_different(const std::filesystem::path& path, const std::string_view content) {
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            const std::string current((std::istreambuf_iterator<char>(existing)),
                                      std::istreambuf_iterator<char>());
            if (current == content) return;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open generated output: " + path.string());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) throw std::runtime_error("cannot write generated output: " + path.string());
}

std::vector<std::string> selected_filenames(const sst::SstManifest& manifest,
                                            const CorpusScope scope) {
    if (scope == CorpusScope::Smoke) {
        return {smoke_files.begin(), smoke_files.end()};
    }
    std::vector<std::string> result;
    result.reserve(manifest.entries.size());
    for (const auto& entry : manifest.entries)
        result.push_back(entry.filename);
    return result;
}

std::vector<std::uint32_t> selected_indices(const CorpusScope scope) {
    if (scope == CorpusScope::Smoke) return {smoke_case_indices.begin(), smoke_case_indices.end()};
    std::vector<std::uint32_t> result(sst::sh4_sst_corpus_records_per_file);
    for (std::uint32_t index = 0u; index < result.size(); ++index)
        result[index] = index;
    return result;
}

std::uint16_t form_opcode_for_slot(const sst_codegen::SstCodeForm& form, std::uint8_t slot);

std::set<std::uint8_t> compiled_slots_for_form(const sst_codegen::SstCodeForm& form) {
    std::set<std::uint8_t> result{0u, 1u, 2u, 3u};
    for (const auto slot : form.fetch_slots) {
        if (slot >= sst_codegen::normal_code_slot_count) result.insert(slot);
    }
    return result;
}

void classify_form(FormRecord& record, const CoverageIndex& coverage) {
    const auto tested = sh4::decode(record.tested_opcode);
    record.tested_has_delay_slot = tested.has_delay_slot;
    if (const auto* entry = coverage.find(tested.kind)) {
        record.family = entry->family_id;
    } else {
        record.family = "unknown-opcode";
    }

    const auto aligned =
        std::all_of(record.representative.cycles.begin(),
                    record.representative.cycles.end(),
                    [](const auto& cycle) { return (cycle.fetch_address & 1u) == 0u; });
    if (!aligned || (record.representative.initial.pc & 1u) != 0u) {
        record.unsupported_classification =
            sst::ResultClassification::NotApplicableReferenceAlignment;
        record.unsupported_reason = "unaligned reference instruction fetch";
        return;
    }

    const auto last_slot = record.form.fetch_slots.back();
    const auto last_opcode = form_opcode_for_slot(record.form, last_slot);
    if (sh4::decode(last_opcode).has_delay_slot) {
        record.unsupported_classification = sst::ResultClassification::NotApplicableAccessShape;
        record.unsupported_reason = "four-instruction oracle ends at a delay-slot owner";
        return;
    }

    for (const auto slot : compiled_slots_for_form(record.form)) {
        const auto opcode = form_opcode_for_slot(record.form, slot);
        const auto decoded = sh4::decode(opcode);
        const auto* entry = coverage.find(decoded.kind);
        if (!decoded.is_known() || entry == nullptr) {
            record.unsupported_classification =
                sst::ResultClassification::NotApplicableKatanaRestricted;
            record.unsupported_reason = "unknown opcode " + hex4(opcode);
            return;
        }
        if (entry->support != sh4::AlphaIsaSupport::Supported) {
            record.unsupported_classification =
                sst::ResultClassification::NotApplicableKatanaRestricted;
            record.unsupported_reason = "Katana alpha family is " +
                                        std::string(sh4::alpha_isa_support_name(entry->support)) +
                                        ": " + entry->family_id;
            return;
        }
        const auto operation = ir::lowering_operation_for_instruction(decoded.kind);
        if (operation == ir::Operation::Unknown ||
            !codegen::cpp_backend_supports_operation(operation)) {
            record.unsupported_classification =
                sst::ResultClassification::NotApplicableKatanaRestricted;
            record.unsupported_reason = "opcode is not lowerable by the native C++ backend";
            return;
        }
    }
    record.supported = true;
}

std::vector<sst_codegen::SstCodeSlotBinding> canonical_bindings(const FormRecord& record) {
    return sst_codegen::sst_code_slot_bindings(record.representative, record.form);
}

std::uint16_t form_opcode_for_slot(const sst_codegen::SstCodeForm& form, const std::uint8_t slot) {
    return form.opcodes[slot < sst_codegen::normal_code_slot_count ? slot : 4u];
}

io::ExecutableImage make_synthetic_image(const FormRecord& record,
                                         analysis::AnalysisOverrides& overrides) {
    std::map<std::uint32_t, std::uint16_t> code;
    const auto bindings = canonical_bindings(record);
    const std::set<std::uint8_t> observed_slots(record.form.fetch_slots.begin(),
                                                record.form.fetch_slots.end());
    const auto compiled_slots = compiled_slots_for_form(record.form);
    for (const auto& binding : bindings) {
        if (!compiled_slots.contains(binding.slot)) continue;
        const auto [found, inserted] = code.emplace(binding.canonical_address, binding.opcode);
        if (!inserted && found->second != binding.opcode)
            throw std::runtime_error("canonical SST code slots contain conflicting opcodes");
    }

    io::ExecutableImage image("deterministic-sh4-sst-" + record.digest);
    std::size_t segment_index = 0u;
    for (auto cursor = code.begin(); cursor != code.end();) {
        const auto start = cursor->first;
        std::vector<std::uint8_t> bytes;
        auto expected = start;
        while (cursor != code.end() && cursor->first == expected) {
            bytes.push_back(static_cast<std::uint8_t>(cursor->second & 0xFFu));
            bytes.push_back(static_cast<std::uint8_t>(cursor->second >> 8u));
            expected += 2u;
            ++cursor;
        }
        io::ImageSegment segment{
            ".sst.text." + std::to_string(segment_index++),
            start,
            0u,
            bytes.size(),
            io::SegmentKind::Code,
            {true, false, true},
            std::move(bytes),
        };
        segment.source_kind = io::ImageSourceKind::RawBinary;
        image.add_segment(std::move(segment));
    }

    const auto entry =
        sst_codegen::canonical_address_for_sst_slot(record.form, record.form.fetch_slots[0]);
    if (!entry) throw std::runtime_error("SST code form has no canonical entry address");
    image.add_entry_point(*entry);

    overrides.source_path = "generated-sh4-sst-analysis-overrides";
    std::size_t line = 1u;
    for (std::uint8_t slot = 0u; slot < sst_codegen::normal_code_slot_count; ++slot) {
        if (observed_slots.contains(slot)) continue;
        const auto address = sst_codegen::canonical_address_for_sst_slot(record.form, slot);
        if (!address) throw std::runtime_error("normal SST slot has no canonical address");
        overrides.functions.push_back({*address, line++});
    }
    for (std::uint8_t external = 0u; external < record.form.external_slot_count; ++external) {
        const auto address = sst_codegen::canonical_address_for_sst_slot(
            record.form, static_cast<std::uint8_t>(sst_codegen::normal_code_slot_count + external));
        if (!address) throw std::runtime_error("external SST slot has no canonical address");
        overrides.functions.push_back({*address, line++});
    }

    std::map<std::uint32_t, std::set<std::uint32_t>> dynamic_targets;
    for (std::size_t index = 0u; index < record.form.fetch_slots.size(); ++index) {
        const auto slot = record.form.fetch_slots[index];
        const auto opcode = form_opcode_for_slot(record.form, slot);
        const auto decoded = sh4::decode(opcode);
        // A standalone synthetic RTS has no caller context and therefore is
        // deliberately not an overrideable indirect-dispatch site. Its
        // observed continuation is already seeded as a separate function,
        // while the emitted return keeps the live PR authoritative.
        const bool indirect = decoded.control_flow == sh4::ControlFlowKind::IndirectBranch ||
                              decoded.control_flow == sh4::ControlFlowKind::IndirectCall;
        if (!indirect) continue;
        const auto target_index = index + (decoded.has_delay_slot ? 2u : 1u);
        if (target_index >= record.form.fetch_slots.size()) continue;
        const auto owner = sst_codegen::canonical_address_for_sst_slot(record.form, slot);
        const auto target = sst_codegen::canonical_address_for_sst_slot(
            record.form, record.form.fetch_slots[target_index]);
        if (owner && target) dynamic_targets[*owner].insert(*target);
    }
    for (const auto& [owner, targets] : dynamic_targets) {
        // A single static override is evidence only when the four-step form
        // observes one concrete target for this site. Multi-target execution
        // remains a runtime-computed transition, with every target separately
        // seeded above.
        if (targets.size() == 1u) overrides.jumps.push_back({owner, *targets.begin(), line++});
    }
    return image;
}

std::vector<ir::Function>
select_partition_functions(const std::vector<ir::Function>& program,
                           const codegen::TranslationUnitPartition& partition) {
    std::vector<ir::Function> result;
    result.reserve(partition.function_indices.size());
    for (const auto index : partition.function_indices) {
        if (index >= program.size())
            throw std::runtime_error("SST partition references a missing IR function");
        result.push_back(program[index]);
    }
    return result;
}

std::uint32_t block_size(const ir::BasicBlock& block) {
    if (block.instructions.empty())
        throw std::runtime_error("generated SST IR contains an empty basic block");
    std::uint64_t end = block.start_address;
    for (const auto& instruction : block.instructions)
        end = std::max(end, static_cast<std::uint64_t>(instruction.source_address) + 2u);
    if (end <= block.start_address ||
        end - block.start_address > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("generated SST block has an invalid address extent");
    return static_cast<std::uint32_t>(end - block.start_address);
}

void emit_dynamic_dispatch_definitions(std::ostringstream& output,
                                       const std::string& symbol_namespace) {
    output << "#undef static_call\n"
           << "#undef resolved_call\n"
           << "#undef guarded_call\n"
           << "#undef guarded_jump\n"
           << "#undef runtime_only_call\n"
           << "#undef runtime_only_jump\n"
           << "#undef unresolved_call\n"
           << "#undef unresolved_jump\n\n"
           << "namespace " << symbol_namespace << " {\n"
           << "void note_instruction_entry(const std::uint32_t address,\n"
           << "                            const bool in_delay_slot) noexcept {\n"
           << "    ::katana::testing::sh4_sst::note_native_instruction(address, "
              "in_delay_slot);\n"
           << "}\n"
           << "void note_block_entry(const std::uint32_t address) noexcept {\n"
           << "    ::katana::testing::sh4_sst::note_native_block(address);\n"
           << "}\n";
    for (const auto call :
         {"static_call", "resolved_call", "guarded_call", "runtime_only_call", "unresolved_call"}) {
        output << "void " << call << "(CpuState& cpu, const std::uint32_t target) {\n"
               << "    ::katana::testing::sh4_sst::reject_native_dispatch(cpu, target, true);\n"
               << "}\n";
    }
    for (const auto jump : {"guarded_jump", "runtime_only_jump", "unresolved_jump"}) {
        output << "void " << jump << "(CpuState& cpu, const std::uint32_t target) {\n"
               << "    ::katana::testing::sh4_sst::reject_native_dispatch(cpu, target, false);\n"
               << "}\n";
    }
    output << "} // namespace " << symbol_namespace << "\n\n";
}

void emit_wrapper(std::ostringstream& output,
                  const std::string& wrapper,
                  const std::string& symbol_namespace,
                  const std::uint32_t function_entry) {
    output << "namespace katana::testing::sh4_sst::generated_detail {\n"
           << "katana::runtime::BlockExit " << wrapper << "(katana::runtime::CpuState& cpu,\n"
           << "    katana::runtime::BlockExecutionContext& context) {\n"
           << "    auto* const services = active_native_services();\n"
           << "    if (services == nullptr)\n"
           << "        throw std::runtime_error(\"SH-4 SST native services are not active\");\n"
           << "    const katana::runtime::BlockAddress source{\n"
           << "        cpu.pc, katana::runtime::canonical_physical_address(cpu.pc)};\n"
           << "    const auto exception_generation = cpu.exception_generation;\n"
           << "    ::" << symbol_namespace << "::fn_" << hex8(function_entry)
           << "_with_services(cpu, services);\n"
           << "    context.scheduler_cycle = services->scheduler_cycle();\n"
           << "    if (cpu.exception_generation != exception_generation)\n"
           << "        return katana::runtime::make_block_exit(\n"
           << "            cpu, context, katana::runtime::BlockEndKind::Exception, source);\n"
           << "    return katana::runtime::make_block_exit(\n"
           << "        cpu, context, katana::runtime::BlockEndKind::Fallthrough, source,\n"
           << "        katana::runtime::BlockAddress{\n"
           << "            cpu.pc, katana::runtime::canonical_physical_address(cpu.pc)});\n"
           << "}\n"
           << "} // namespace katana::testing::sh4_sst::generated_detail\n\n";
}

void generate_form(FormRecord& record) {
    analysis::AnalysisOverrides overrides;
    auto image = make_synthetic_image(record, overrides);
    const auto analyzed = analysis::analyze_control_flow(image, &overrides);
    for (const auto& diagnostic : analyzed.recursive.diagnostics) {
        if (analysis::analysis_diagnostic_blocks_codegen(diagnostic)) {
            throw std::runtime_error("supported SST form " + record.key +
                                     " produced a blocking decoder/CFG diagnostic at 0x" +
                                     hex8(diagnostic.address));
        }
    }

    auto program = ir::lower_program(analyzed);
    if (program.empty())
        throw std::runtime_error("supported SST form lowered to an empty program: " + record.key);
    const auto& contract = codegen::native_aot_emission_contract(
        codegen::NativeAotEmissionProfile::ExternalConformance);
    static_cast<void>(ir::optimize_program(program, contract.optimization_options));
    ir::require_valid_program(program);
    codegen::annotate_proven_linear_ram_accesses(program);
    ir::require_valid_program(program);
    const auto partitions =
        codegen::partition_translation_units(program, contract.partition_options);
    if (partitions.empty())
        throw std::runtime_error("supported SST form produced no product partitions");

    std::ostringstream source;
    source << "// Deterministic SH-4 SingleStepTests AOT form " << record.digest << "\n"
           << "// key: " << record.key << "\n\n";
    const codegen::CppBackend backend;
    std::map<std::uint32_t, std::string> wrapper_for_function;
    for (const auto& partition : partitions) {
        auto functions = select_partition_functions(program, partition);
        const auto symbol_namespace =
            "katana_sh4_sst_" + record.digest + "_p" + std::to_string(partition.index);
        codegen::NativeAotBackendRequestOptions request_options;
        request_options.symbol_namespace = symbol_namespace;
        request_options.emit_run_functions = false;
        request_options.metadata_entry_address = program.front().entry_address;
        request_options.external_instruction_observer = true;
        const auto request = codegen::make_native_aot_backend_request(
            codegen::NativeAotEmissionProfile::ExternalConformance,
            functions,
            functions.front().entry_address,
            request_options);
        source << backend.emit(request).joined_text() << '\n';
        emit_dynamic_dispatch_definitions(source, symbol_namespace);

        for (const auto& function : functions) {
            const auto wrapper = "execute_" + record.digest + "_" + hex8(function.entry_address);
            const auto [found, inserted] =
                wrapper_for_function.emplace(function.entry_address, wrapper);
            if (!inserted)
                throw std::runtime_error("SST function appears in more than one product partition");
            emit_wrapper(source, found->second, symbol_namespace, function.entry_address);
        }
    }

    std::map<std::uint32_t, BlockBinding> blocks;
    for (const auto& function : program) {
        const auto wrapper = wrapper_for_function.find(function.entry_address);
        if (wrapper == wrapper_for_function.end())
            throw std::runtime_error("SST IR function has no emitted wrapper");
        for (const auto& block : function.blocks) {
            const BlockBinding binding{
                block.start_address,
                block_size(block),
                wrapper->second,
            };
            const auto [found, inserted] = blocks.emplace(binding.address, binding);
            if (!inserted &&
                (found->second.size != binding.size || found->second.wrapper != binding.wrapper))
                throw std::runtime_error(
                    "SST IR has ambiguous basic blocks at one canonical address");
        }
    }
    const auto slot_bindings = canonical_bindings(record);
    for (const auto slot : compiled_slots_for_form(record.form)) {
        const auto binding = std::find_if(slot_bindings.begin(),
                                          slot_bindings.end(),
                                          [slot](const auto& value) { return value.slot == slot; });
        if (binding == slot_bindings.end())
            throw std::runtime_error("compiled SST slot is missing a canonical binding");
        const bool covered = std::any_of(blocks.begin(), blocks.end(), [&](const auto& value) {
            const auto& block = value.second;
            return binding->canonical_address >= block.address &&
                   static_cast<std::uint64_t>(binding->canonical_address) <
                       static_cast<std::uint64_t>(block.address) + block.size;
        });
        if (!covered)
            throw std::runtime_error(
                "supported SST form did not emit a compiled instruction address");
    }

    record.generated_functions = program.size();
    record.partitions = partitions.size();
    for (auto& [address, block] : blocks) {
        static_cast<void>(address);
        record.blocks.push_back(std::move(block));
    }
    record.generated_source = source.str();
}

void append_form_descriptor(std::ostringstream& output,
                            const FormRecord& form,
                            const std::size_t ordinal) {
    const auto block_name = "form_blocks_" + std::to_string(ordinal);
    const auto slot_name = "form_slots_" + std::to_string(ordinal);
    const auto relocation_name = "form_relocations_" + std::to_string(ordinal);
    const auto slots = canonical_bindings(form);
    const auto relocations = sst_codegen::sst_code_relocation_recipes(form.form);
    output << "static constexpr std::array<GeneratedBlockDescriptor, " << form.blocks.size() << "> "
           << block_name << "{{\n";
    for (const auto& block : form.blocks) {
        output << "    {0x" << hex8(block.address) << "u, " << block.size
               << "u, &generated_detail::" << block.wrapper << "},\n";
    }
    output << "}};\n";
    output << "static constexpr std::array<GeneratedCanonicalSlotDescriptor, " << slots.size()
           << "> " << slot_name << "{{\n";
    for (const auto& slot : slots) {
        output << "    {" << static_cast<unsigned>(slot.slot) << "u, 0x"
               << hex8(slot.canonical_address) << "u},\n";
    }
    output << "}};\n";
    output << "static constexpr std::array<GeneratedRelocationRecipe, " << relocations.size()
           << "> " << relocation_name << "{{\n";
    for (const auto& relocation : relocations) {
        output << "    {0x" << hex8(relocation.source) << "u, "
               << static_cast<unsigned>(relocation.anchor_slot) << "u, 0x" << hex8(relocation.delta)
               << "u},\n";
    }
    output << "}};\n";
    output << "static constexpr GeneratedFormDescriptor form_" << ordinal << "{"
           << quote_cpp(form.key) << ", " << quote_cpp(form.family) << ", 0x"
           << hex4(form.tested_opcode) << "u, std::array<std::uint8_t, "
           << form.form.fetch_slots.size() << "u>{";
    for (std::size_t index = 0u; index < form.form.fetch_slots.size(); ++index) {
        if (index != 0u) output << ", ";
        output << static_cast<unsigned>(form.form.fetch_slots[index]) << "u";
    }
    output << "}, std::span<const GeneratedCanonicalSlotDescriptor>{" << slot_name
           << "}, std::span<const GeneratedRelocationRecipe>{" << relocation_name << "}, "
           << (form.supported ? "true" : "false") << ", "
           << (form.tested_has_delay_slot ? "true" : "false") << ", ResultClassification::" <<
        [&] {
            const auto name =
                std::string(sst::result_classification_name(form.unsupported_classification));
            std::string symbol;
            bool uppercase = true;
            for (const auto character : name) {
                if (character == '-') {
                    uppercase = true;
                } else {
                    symbol.push_back(uppercase ? static_cast<char>(std::toupper(
                                                     static_cast<unsigned char>(character)))
                                               : character);
                    uppercase = false;
                }
            }
            return symbol;
        }() << ", "
           << quote_cpp(form.unsupported_reason) << ", std::span<const GeneratedBlockDescriptor>{"
           << block_name << "}, " << form.vector_count << "u, " << form.generated_functions << "u, "
           << form.partitions << "u};\n\n";
}

std::string make_shard_source(const std::size_t shard_index,
                              const std::vector<const FormRecord*>& forms,
                              const std::vector<const SelectedCase*>& cases) {
    std::ostringstream output;
    output << "// Generated by katana-sh4-sst-aot-generator. Do not edit.\n"
           << "#include \"katana/testing/sh4_sst_generated.hpp\"\n"
           << "#include \"katana/runtime/block_abi.hpp\"\n"
           << "#include <array>\n"
           << "#include <cstdint>\n"
           << "#include <stdexcept>\n\n";
    for (const auto* form : forms)
        output << form->generated_source;
    output << "namespace katana::testing::sh4_sst::generated_detail {\n\n";
    for (std::size_t index = 0u; index < forms.size(); ++index)
        append_form_descriptor(output, *forms[index], index);
    output << "static constexpr std::array<GeneratedFormDescriptor, " << forms.size()
           << "> forms{{\n";
    for (std::size_t index = 0u; index < forms.size(); ++index)
        output << "    form_" << index << ",\n";
    output << "}};\n\n"
           << "static constexpr std::array<GeneratedCaseDescriptor, " << cases.size()
           << "> cases{{\n";
    for (const auto* selected : cases) {
        output << "    {" << quote_cpp(selected->filename) << ", " << selected->case_index << "u, "
               << quote_cpp(selected->form_key) << "},\n";
    }
    output << "}};\n\n"
           << "std::span<const GeneratedFormDescriptor> generated_forms_shard_"
           << shard_suffix(shard_index) << "() noexcept { return forms; }\n"
           << "std::span<const GeneratedCaseDescriptor> generated_cases_shard_"
           << shard_suffix(shard_index) << "() noexcept { return cases; }\n"
           << "} // namespace katana::testing::sh4_sst::generated_detail\n";
    return output.str();
}

std::string u16_array(const std::string_view name, const std::vector<std::uint16_t>& values) {
    std::ostringstream output;
    output << "static constexpr std::array<std::uint16_t, " << values.size() << "> " << name
           << "{{\n";
    for (std::size_t index = 0u; index < values.size(); ++index) {
        if (index % 12u == 0u) output << "    ";
        output << "0x" << hex4(values[index]) << "u,";
        if (index % 12u == 11u || index + 1u == values.size()) {
            output << '\n';
        } else {
            output << ' ';
        }
    }
    output << "}};\n\n";
    return output.str();
}

std::string make_registry_source(const Options& options,
                                 const std::vector<std::uint16_t>& represented,
                                 const std::vector<std::uint16_t>& unrepresented) {
    std::ostringstream output;
    output << "// Generated by katana-sh4-sst-aot-generator. Do not edit.\n"
           << "#include \"katana/testing/sh4_sst_generated.hpp\"\n"
           << "#include <algorithm>\n"
           << "#include <array>\n"
           << "#include <vector>\n\n"
           << "namespace katana::testing::sh4_sst::generated_detail {\n";
    for (std::size_t shard = 0u; shard < options.shard_count; ++shard) {
        output << "std::span<const GeneratedFormDescriptor> generated_forms_shard_"
               << shard_suffix(shard) << "() noexcept;\n"
               << "std::span<const GeneratedCaseDescriptor> generated_cases_shard_"
               << shard_suffix(shard) << "() noexcept;\n";
    }
    output << "} // namespace katana::testing::sh4_sst::generated_detail\n\n"
           << "namespace katana::testing::sh4_sst {\nnamespace {\n"
           << "const std::vector<GeneratedFormDescriptor>& all_forms() {\n"
           << "    static const auto result = [] {\n"
           << "        std::vector<GeneratedFormDescriptor> values;\n";
    for (std::size_t shard = 0u; shard < options.shard_count; ++shard) {
        output << "        { const auto shard = generated_detail::generated_forms_shard_"
               << shard_suffix(shard)
               << "(); values.insert(values.end(), shard.begin(), shard.end()); }\n";
    }
    output << "        std::sort(values.begin(), values.end(), [](const auto& left, "
              "const auto& right) { return left.key < right.key; });\n"
           << "        return values;\n"
           << "    }();\n"
           << "    return result;\n"
           << "}\n\n"
           << "const std::vector<GeneratedCaseDescriptor>& all_cases() {\n"
           << "    static const auto result = [] {\n"
           << "        std::vector<GeneratedCaseDescriptor> values;\n";
    for (std::size_t shard = 0u; shard < options.shard_count; ++shard) {
        output << "        { const auto shard = generated_detail::generated_cases_shard_"
               << shard_suffix(shard)
               << "(); values.insert(values.end(), shard.begin(), shard.end()); }\n";
    }
    output << "        std::sort(values.begin(), values.end(), [](const auto& left, "
              "const auto& right) {\n"
           << "            if (left.filename != right.filename) return left.filename < "
              "right.filename;\n"
           << "            return left.case_index < right.case_index;\n"
           << "        });\n"
           << "        return values;\n"
           << "    }();\n"
           << "    return result;\n"
           << "}\n\n"
           << u16_array("represented_opcodes", represented)
           << u16_array("unrepresented_opcodes", unrepresented) << "} // namespace\n\n"
           << "std::span<const GeneratedFormDescriptor> generated_forms() noexcept {\n"
           << "    return all_forms();\n"
           << "}\n\n"
           << "const GeneratedFormDescriptor* find_generated_form("
              "const std::string_view key) noexcept {\n"
           << "    const auto& forms = all_forms();\n"
           << "    const auto found = std::lower_bound(forms.begin(), forms.end(), key,\n"
           << "        [](const auto& form, const auto value) { return form.key < value; });\n"
           << "    return found != forms.end() && found->key == key ? &*found : nullptr;\n"
           << "}\n\n"
           << "std::span<const GeneratedCaseDescriptor> generated_cases() noexcept {\n"
           << "    return all_cases();\n"
           << "}\n\n"
           << "std::span<const std::uint16_t> represented_external_opcodes() noexcept {\n"
           << "    return represented_opcodes;\n"
           << "}\n\n"
           << "std::span<const std::uint16_t> unrepresented_katana_opcodes() noexcept {\n"
           << "    return unrepresented_opcodes;\n"
           << "}\n\n"
           << "std::string_view generated_corpus_scope() noexcept { return "
           << quote_cpp(scope_name(options.scope)) << "; }\n"
           << "std::size_t generated_vector_count() noexcept { return all_cases().size(); }\n"
           << "} // namespace katana::testing::sh4_sst\n";
    return output.str();
}

std::string make_metadata_json(const Options& options,
                               const sst::SstManifest& manifest,
                               const std::vector<std::string>& files,
                               const std::vector<FormRecord>& forms,
                               const std::vector<SelectedCase>& cases,
                               const std::vector<std::string>& shard_hashes,
                               const std::string& registry_hash) {
    const auto supported =
        std::count_if(forms.begin(), forms.end(), [](const auto& form) { return form.supported; });
    std::size_t functions = 0u;
    std::size_t partitions = 0u;
    std::size_t blocks = 0u;
    for (const auto& form : forms) {
        functions += form.generated_functions;
        partitions += form.partitions;
        blocks += form.blocks.size();
    }

    std::ostringstream output;
    output << "{\"schema\":\"katana-sh4-sst-aot-generation\",\"version\":1"
           << ",\"corpus\":{\"commit\":" << io::quote_json(sst::sh4_sst_corpus_commit)
           << ",\"tree\":" << io::quote_json(sst::sh4_sst_corpus_tree)
           << ",\"manifest_sha256\":" << io::quote_json(manifest.sha256) << "}"
           << ",\"scope\":" << io::quote_json(scope_name(options.scope))
           << ",\"profile\":{\"name\":"
           << io::quote_json(codegen::native_aot_emission_profile_name(
                  codegen::NativeAotEmissionProfile::ExternalConformance))
           << ",\"version\":" << codegen::native_aot_emission_profile_version << "}"
           << ",\"abi\":{\"runtime\":" << runtime::abi_version
           << ",\"block\":" << runtime::block_abi_version
           << ",\"backend\":" << codegen::backend_interface_abi_version << "}"
           << ",\"vectors\":" << cases.size() << ",\"code_forms\":{\"total\":" << forms.size()
           << ",\"supported\":" << supported << ",\"unsupported\":" << forms.size() - supported
           << "}"
           << ",\"generated\":{\"functions\":" << functions << ",\"partitions\":" << partitions
           << ",\"blocks\":" << blocks << ",\"shards\":" << options.shard_count << "}"
           << ",\"files\":[";
    for (std::size_t index = 0u; index < files.size(); ++index) {
        if (index != 0u) output << ',';
        output << io::quote_json(files[index]);
    }
    output << "],\"outputs\":{\"registry_sha256\":" << io::quote_json(registry_hash)
           << ",\"shard_sha256\":[";
    for (std::size_t index = 0u; index < shard_hashes.size(); ++index) {
        if (index != 0u) output << ',';
        output << io::quote_json(shard_hashes[index]);
    }
    output << "]},\"unsupported_forms\":[";
    bool first = true;
    for (const auto& form : forms) {
        if (form.supported) continue;
        if (!first) output << ',';
        first = false;
        output << "{\"key\":" << io::quote_json(form.key)
               << ",\"family\":" << io::quote_json(form.family)
               << ",\"opcode\":" << form.tested_opcode << ",\"vectors\":" << form.vector_count
               << ",\"reason\":" << io::quote_json(form.unsupported_reason) << '}';
    }
    output << "]}\n";
    return output.str();
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        std::error_code error;
        if (!std::filesystem::is_directory(options.corpus_root, error) || error)
            throw std::runtime_error("corpus root is not a readable directory: " +
                                     options.corpus_root.string());
        std::filesystem::create_directories(options.output_directory, error);
        if (error)
            throw std::runtime_error("cannot create generator output directory: " +
                                     error.message());

        const auto manifest = sst::calculate_sh4_sst_manifest(options.corpus_root);
        sst::require_pinned_sh4_sst_manifest(manifest);
        const auto files = selected_filenames(manifest, options.scope);
        const auto indices = selected_indices(options.scope);
        const CoverageIndex coverage;

        std::map<std::string, FormRecord> forms_by_key;
        std::vector<SelectedCase> cases;
        cases.reserve(files.size() * indices.size());
        std::set<std::uint16_t> represented_set;
        std::map<std::string, std::string> digest_owners;
        for (const auto& filename : files) {
            const auto parsed = sst::parse_sh4_sst_file(options.corpus_root / filename);
            for (const auto case_index : indices) {
                if (case_index >= parsed.cases.size())
                    throw std::runtime_error("selected SST case index is outside " + filename);
                const auto& test = parsed.cases[case_index];
                const auto form = sst_codegen::make_sst_code_form(test);
                const auto key = sst_codegen::sst_code_form_key(form);
                // Validate every selected vector against the materialized,
                // form-invariant relocation recipes while opcode inspection is
                // still confined to this build-time generator.
                static_cast<void>(sst_codegen::sst_code_address_mappings(test, form));
                auto [found, inserted] = forms_by_key.try_emplace(key);
                if (inserted) {
                    auto& record = found->second;
                    record.form = form;
                    record.representative = test;
                    record.representative_filename = filename;
                    record.representative_case_index = case_index;
                    record.key = key;
                    record.digest = io::sha256_bytes(key).substr(0u, 20u);
                    record.tested_opcode = test.opcodes[1u];
                    const auto [owner, unique] = digest_owners.emplace(record.digest, record.key);
                    if (!unique && owner->second != record.key)
                        throw std::runtime_error("SST form digest collision: " + record.digest);
                    classify_form(record, coverage);
                } else if (found->second.form != form) {
                    throw std::runtime_error(
                        "two unequal SST code forms produced the same canonical key");
                }
                ++found->second.vector_count;
                cases.push_back({filename, case_index, key});
                represented_set.insert(test.opcodes[1u]);
            }
        }

        std::vector<FormRecord> forms;
        forms.reserve(forms_by_key.size());
        for (auto& [key, form] : forms_by_key) {
            static_cast<void>(key);
            forms.push_back(std::move(form));
        }
        for (auto& form : forms) {
            if (!form.supported) continue;
            try {
                generate_form(form);
            } catch (const std::exception& error) {
                throw std::runtime_error("supported SST form " + form.key + " represented by " +
                                         form.representative_filename + " case " +
                                         std::to_string(form.representative_case_index) +
                                         " failed generation: " + error.what());
            }
        }

        std::vector<std::uint16_t> represented(represented_set.begin(), represented_set.end());
        std::vector<std::uint16_t> unrepresented;
        for (std::uint32_t opcode = 0u; opcode <= 0xFFFFu; ++opcode) {
            const auto narrowed = static_cast<std::uint16_t>(opcode);
            if (!sh4::decode(narrowed).is_known() || represented_set.contains(narrowed)) continue;
            unrepresented.push_back(narrowed);
        }

        std::vector<std::vector<const FormRecord*>> forms_by_shard(options.shard_count);
        for (std::size_t index = 0u; index < forms.size(); ++index)
            forms_by_shard[index % options.shard_count].push_back(&forms[index]);
        std::vector<std::vector<const SelectedCase*>> cases_by_shard(options.shard_count);
        for (std::size_t index = 0u; index < cases.size(); ++index)
            cases_by_shard[index % options.shard_count].push_back(&cases[index]);

        std::vector<std::string> shard_hashes;
        shard_hashes.reserve(options.shard_count);
        for (std::size_t shard = 0u; shard < options.shard_count; ++shard) {
            const auto source =
                make_shard_source(shard, forms_by_shard[shard], cases_by_shard[shard]);
            shard_hashes.push_back(io::sha256_bytes(source));
            write_if_different(options.output_directory /
                                   ("sh4_sst_aot_shard_" + shard_suffix(shard) + ".cpp"),
                               source);
        }
        const auto registry = make_registry_source(options, represented, unrepresented);
        const auto registry_hash = io::sha256_bytes(registry);
        write_if_different(options.output_directory / "sh4_sst_aot_registry.cpp", registry);
        const auto metadata =
            make_metadata_json(options, manifest, files, forms, cases, shard_hashes, registry_hash);
        write_if_different(options.output_directory / "sh4_sst_aot_metadata.json", metadata);

        const auto supported = std::count_if(
            forms.begin(), forms.end(), [](const auto& form) { return form.supported; });
        std::cout << "SH-4 SST AOT generation complete: scope=" << scope_name(options.scope)
                  << " vectors=" << cases.size() << " forms=" << forms.size()
                  << " supported_forms=" << supported << " shards=" << options.shard_count << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "SH-4 SST AOT generation failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
