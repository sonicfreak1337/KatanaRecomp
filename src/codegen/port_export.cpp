#include "katana/codegen/port_export.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/hardware_audit.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cache.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/latent_aot_registry.hpp"
#include "katana/codegen/naming.hpp"
#include "katana/codegen/native_aot_profile.hpp"
#include "katana/codegen/project.hpp"
#include "katana/codegen/source_map.hpp"
#include "katana/io/input_output_error.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/runtime/abi.hpp"
#include "katana/runtime/disc_install.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/guest_program_range.hpp"
#include "katana/runtime/packed_disc.hpp"
#include "katana/runtime/platform_services.hpp"
#include "katana/runtime/wait_loop_trace.hpp"
#include "katana/sh4/instruction_timing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <compare>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace katana::codegen {
namespace {

void report_progress(const PortExportOptions& options, const std::string_view phase) {
    if (options.progress_callback != nullptr) options.progress_callback(phase);
}

bool valid_target_name(const std::string_view value) noexcept {
    if (value.empty() || !std::isalpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    const bool valid = std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
    });
    return valid && value != "katana-recomp" && value != "katana_runtime" &&
           value != "katana_core" && value != "katana_generated" && value != "all" &&
           value != "clean" && value != "install" && value != "test" && value != "help" &&
           value != "rebuild_cache";
}

constexpr std::string_view port_namespace = "katana_port_generated";

std::string_view console_profile_enumerator(const std::string_view profile) {
    if (profile == "japan-ntsc") return "JapanNtsc";
    if (profile == "north-america-ntsc") return "NorthAmericaNtsc";
    if (profile == "europe-pal") return "EuropePal";
    if (profile == "vga") return "Vga";
    throw std::invalid_argument("Unbekanntes Dreamcast-Konsolenprofil.");
}

std::size_t port_codegen_jobs(const std::size_t partition_count) {
    auto requested = static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency()));
    std::optional<std::string> configured;
#ifdef _WIN32
    char* value = nullptr;
    std::size_t value_size = 0u;
    if (_dupenv_s(&value, &value_size, "KATANA_PORT_CODEGEN_JOBS") == 0 && value != nullptr)
        configured = value;
    std::free(value);
#else
    if (const auto* value = std::getenv("KATANA_PORT_CODEGEN_JOBS");
        value != nullptr && *value != '\0')
        configured = value;
#endif
    if (configured && !configured->empty()) {
        std::size_t parsed = 0u;
        const auto jobs = std::stoull(*configured, &parsed, 10);
        if (parsed != configured->size() || jobs == 0u)
            throw std::invalid_argument("KATANA_PORT_CODEGEN_JOBS ist ungueltig.");
        requested = static_cast<std::size_t>(jobs);
    }
    return std::min(partition_count, requested);
}

std::vector<katana::ir::Function>
select_functions(const std::span<const katana::ir::Function> program,
                 const TranslationUnitPartition& partition) {
    std::vector<katana::ir::Function> selected;
    selected.reserve(partition.function_indices.size());
    for (const auto index : partition.function_indices) {
        if (index >= program.size()) {
            throw std::out_of_range("Portpartition verweist auf eine fehlende Funktion.");
        }
        selected.push_back(program[index]);
    }
    std::sort(selected.begin(), selected.end(), [](const auto& left, const auto& right) {
        return left.entry_address < right.entry_address;
    });
    return selected;
}

std::string game_project_export_identity(
    const katana::runtime::GameProjectDefinition& definition) {
    return katana::runtime::game_project_definition_identity(definition);
}

bool game_project_requires_external_runtime_definition(
    const katana::runtime::GameProjectDefinition& definition) noexcept {
    return !definition.function_overrides.empty() ||
           !definition.mid_function_hooks.empty();
}

katana::runtime::GameProjectDefinition game_project_runtime_definition(
    const katana::runtime::GameProjectDefinition& definition) {
    if (game_project_requires_external_runtime_definition(definition))
        return definition;

    auto runtime_definition = definition;
    runtime_definition.function_boundaries = {};
    runtime_definition.jump_tables = {};
    runtime_definition.callback_tables = {};
    runtime_definition.runtime_code_templates = {};
    runtime_definition.function_overrides = {};
    runtime_definition.mid_function_hooks = {};
    runtime_definition.symbols = {};
    runtime_definition.code_identities = {};
    return runtime_definition;
}

std::string game_project_runtime_identity(
    const katana::runtime::GameProjectDefinition& definition) {
    return katana::runtime::game_project_definition_identity(
        game_project_runtime_definition(definition));
}

void add_game_project_symbol(katana::io::ExecutableImage& image,
                             katana::io::ImageSymbol symbol) {
    if (const auto* existing = image.find_symbol(symbol.name);
        existing != nullptr) {
        if (existing->address != symbol.address ||
            existing->size != symbol.size ||
            existing->kind != symbol.kind)
            throw std::invalid_argument(
                "Game-Project-Symbol kollidiert mit dem Executable-Image.");
        return;
    }
    image.add_symbol(std::move(symbol));
}

void apply_game_project_symbols(
    katana::io::ExecutableImage& image,
    const katana::runtime::GameProjectDefinition& definition) {
    for (const auto& function : definition.function_boundaries) {
        if (function.symbol.empty()) continue;
        add_game_project_symbol(
            image,
            {std::string(function.symbol),
             function.start,
             function.size,
             katana::io::SymbolKind::Function,
             katana::io::SymbolBinding::Weak});
    }
    for (const auto& symbol : definition.symbols) {
        const auto kind = [&] {
            switch (symbol.kind) {
            case katana::runtime::GameProjectSymbolKind::Function:
                return katana::io::SymbolKind::Function;
            case katana::runtime::GameProjectSymbolKind::Object:
                return katana::io::SymbolKind::Object;
            case katana::runtime::GameProjectSymbolKind::Unknown:
                return katana::io::SymbolKind::Unknown;
            }
            return katana::io::SymbolKind::Unknown;
        }();
        add_game_project_symbol(
            image,
            {std::string(symbol.name),
             symbol.address,
             symbol.size,
             kind,
             katana::io::SymbolBinding::Weak});
    }
}

std::uint32_t read_game_project_pointer(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address) {
    const auto resolved = image.resolve_segment_address(address, 4u);
    if (!resolved.has_value())
        throw std::invalid_argument(
            "Game-Project-Callbacktabelle liegt nicht im Executable-Image.");
    return image.read_u32_le(*resolved);
}

katana::analysis::AnalysisOverrides game_project_analysis_overrides(
    const katana::runtime::GameProjectDefinition& definition,
    const katana::io::ExecutableImage& image) {
    katana::analysis::AnalysisOverrides overrides;
    overrides.mode = katana::analysis::AnalysisDirectiveMode::Override;
    overrides.source_path = "external-game-project";
    for (const auto& function : definition.function_boundaries)
        overrides.functions.push_back(
            {function.start, 0u, function.size});
    for (const auto& hook : definition.mid_function_hooks)
        overrides.functions.push_back({hook.instruction_address, 0u});
    for (const auto& symbol : definition.symbols) {
        if (symbol.kind == katana::runtime::GameProjectSymbolKind::Function)
            overrides.functions.push_back({symbol.address, 0u});
    }
    for (const auto& table : definition.callback_tables) {
        for (std::uint32_t index = 0u; index < table.entry_count; ++index) {
            const auto address64 =
                static_cast<std::uint64_t>(table.table_address) +
                static_cast<std::uint64_t>(index) * table.entry_stride +
                table.pointer_offset;
            if (address64 > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument(
                    "Game-Project-Callbacktabelle laeuft ueber.");
            const auto target = read_game_project_pointer(
                image, static_cast<std::uint32_t>(address64));
            if (target != 0u) overrides.functions.push_back({target, 0u});
        }
    }
    std::sort(
        overrides.functions.begin(),
        overrides.functions.end(),
        [](const auto& left, const auto& right) {
            if (left.address != right.address)
                return left.address < right.address;
            // Preserve an exact external boundary when a hook, symbol or
            // callback also contributes the same entry-only seed.
            return left.size > right.size;
        });
    overrides.functions.erase(
        std::unique(
            overrides.functions.begin(),
            overrides.functions.end(),
            [](const auto& left, const auto& right) {
                return left.address == right.address;
            }),
        overrides.functions.end());
    for (const auto& table : definition.jump_tables) {
        const auto encoding = [&] {
            switch (table.encoding) {
            case katana::runtime::GameProjectTableEncoding::Absolute32:
                return katana::analysis::JumpTableOverrideEncoding::Absolute32;
            case katana::runtime::GameProjectTableEncoding::SignedRelative16:
                return katana::analysis::JumpTableOverrideEncoding::
                    SignedRelative16;
            case katana::runtime::GameProjectTableEncoding::SignedRelative32:
                return katana::analysis::JumpTableOverrideEncoding::
                    SignedRelative32;
            }
            return katana::analysis::JumpTableOverrideEncoding::Absolute32;
        }();
        const auto transfer =
            table.transfer ==
                    katana::runtime::GameProjectControlTransferKind::Call
                ? katana::analysis::JumpTableOverrideTransfer::Call
                : katana::analysis::JumpTableOverrideTransfer::Jump;
        overrides.jump_tables.push_back(
            {table.dispatch_address,
             table.table_address,
             table.entry_count,
             0u,
             table.entry_stride,
             table.relative_base,
             encoding,
             transfer});
    }
    return overrides;
}

std::span<const std::uint8_t> game_project_image_bytes(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address,
    const std::size_t size) {
    const auto resolved = image.resolve_segment_address(address, size);
    const auto* segment =
        resolved.has_value() ? image.find_segment(*resolved, size) : nullptr;
    const auto offset =
        segment != nullptr ? segment->byte_offset(*resolved) : std::nullopt;
    if (segment == nullptr || !offset.has_value() ||
        *offset > segment->bytes.size() ||
        size > segment->bytes.size() - *offset)
        throw std::invalid_argument(
            "Game-Project-Range besitzt keine exportierbaren Imagebytes.");
    return std::span<const std::uint8_t>(
        segment->bytes.data() + *offset, size);
}

void validate_game_project_image_contract(
    const katana::runtime::GameProjectDefinition& definition,
    const katana::io::ExecutableImage& image) {
    for (const auto& function : definition.function_boundaries) {
        const auto resolved =
            image.resolve_segment_address(function.start, function.size);
        const auto* segment =
            resolved.has_value()
                ? image.find_segment(*resolved, function.size)
                : nullptr;
        if (segment == nullptr || !segment->permissions.executable)
            throw std::invalid_argument(
                "Game-Project-Funktionsgrenze liegt nicht vollstaendig in "
                "ausfuehrbarem Imagecode.");
    }
    for (const auto& table : definition.jump_tables) {
        const auto width =
            table.encoding ==
                    katana::runtime::GameProjectTableEncoding::
                        SignedRelative16
                ? 2u
                : 4u;
        const auto size =
            static_cast<std::size_t>(table.entry_count - 1u) *
                table.entry_stride +
            width;
        const auto resolved =
            image.resolve_segment_address(table.table_address, size);
        const auto* segment =
            resolved.has_value()
                ? image.find_segment(*resolved, size)
                : nullptr;
        if (segment == nullptr || !segment->permissions.readable ||
            segment->permissions.writable)
            throw std::invalid_argument(
                "Game-Project-Jump-Table ist nicht unveraenderlich im "
                "Executable-Image gebunden.");
    }
    for (const auto& table : definition.callback_tables) {
        const auto size =
            static_cast<std::size_t>(table.entry_count - 1u) *
                table.entry_stride +
            table.pointer_offset + sizeof(std::uint32_t);
        const auto resolved =
            image.resolve_segment_address(table.table_address, size);
        const auto* segment =
            resolved.has_value()
                ? image.find_segment(*resolved, size)
                : nullptr;
        if (segment == nullptr || !segment->permissions.readable)
            throw std::invalid_argument(
                "Game-Project-Callbacktabelle liegt nicht lesbar im "
                "Executable-Image.");
    }
    for (const auto& identity : definition.code_identities) {
        const auto bytes =
            game_project_image_bytes(image, identity.address, identity.size);
        if (!katana::runtime::game_project_code_identity_matches(
                identity, bytes))
            throw std::invalid_argument(
                "Game-Project-Codeidentitaet stimmt nicht mit dem "
                "Executable-Image ueberein.");
    }
}

std::string game_project_metadata(
    const katana::runtime::GameProjectDefinition& definition) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-game-project-v3\",\"contract_version\":"
           << definition.contract_version << ",\"project_id\":"
           << katana::io::quote_json(definition.project_id)
           << ",\"project_version\":"
           << katana::io::quote_json(definition.project_version)
           << ",\"definition_identity\":"
           << katana::io::quote_json(
                  game_project_export_identity(definition))
           << ",\"content_identity\":"
           << katana::io::quote_json(definition.identity.content_identity)
           << ",\"boot_file_name\":"
           << katana::io::quote_json(definition.identity.boot_file_name)
           << ",\"boot_byte_identity\":"
           << katana::io::quote_json(
                   definition.identity.boot_byte_identity)
           << ",\"required_product_milestone\":"
           << katana::io::quote_json(
                  katana::runtime::required_product_milestone_name(
                      definition.required_product_milestone))
           << ",\"direct_boot_configured\":"
           << (definition.boot_config.has_value() ? "true" : "false")
           << ",\"game_entry_handoff\":";
    if (definition.game_entry_handoff.has_value()) {
        const auto& binding = *definition.game_entry_handoff;
        output
            << "{\"schema_version\":" << binding.schema_version
            << ",\"required_runtime_abi\":"
            << binding.required_runtime_abi
            << ",\"required_platform_state_contract\":"
            << binding.required_platform_state_contract
            << ",\"content_identity\":"
            << katana::io::quote_json(
                   binding.executable.content_identity)
            << ",\"boot_file_name\":"
            << katana::io::quote_json(
                   binding.executable.boot_file_name)
            << ",\"boot_byte_identity\":"
            << katana::io::quote_json(
                   binding.executable.boot_byte_identity)
            << ",\"console_profile\":"
            << katana::io::quote_json(
                   katana::runtime::dreamcast_console_profile_name(
                       binding.console_profile))
            << ",\"descriptor_identity\":"
            << katana::io::quote_json(binding.descriptor_identity)
            << ",\"required_completeness\":\"complete-platform\"}";
    } else {
        output << "null";
    }
    output
           << ",\"function_boundaries\":[";
    for (std::size_t index = 0u;
         index < definition.function_boundaries.size();
         ++index) {
        if (index != 0u) output << ',';
        const auto& function = definition.function_boundaries[index];
        output << "{\"start\":" << function.start << ",\"size\":"
               << function.size << ",\"symbol\":"
               << katana::io::quote_json(function.symbol) << '}';
    }
    output << "],\"jump_tables\":[";
    for (std::size_t index = 0u; index < definition.jump_tables.size();
         ++index) {
        if (index != 0u) output << ',';
        const auto& table = definition.jump_tables[index];
        output << "{\"dispatch\":" << table.dispatch_address
               << ",\"table\":" << table.table_address
               << ",\"entries\":" << table.entry_count
               << ",\"stride\":" << table.entry_stride
               << ",\"relative_base\":" << table.relative_base
               << ",\"encoding\":"
               << static_cast<unsigned>(table.encoding)
               << ",\"transfer\":"
               << static_cast<unsigned>(table.transfer) << '}';
    }
    output << "],\"callback_tables\":[";
    for (std::size_t index = 0u;
         index < definition.callback_tables.size();
         ++index) {
        if (index != 0u) output << ',';
        const auto& table = definition.callback_tables[index];
        output << "{\"table\":" << table.table_address
               << ",\"entries\":" << table.entry_count
               << ",\"stride\":" << table.entry_stride
               << ",\"pointer_offset\":" << table.pointer_offset
               << ",\"transfer\":"
               << static_cast<unsigned>(table.transfer) << '}';
    }
    output << "],\"runtime_code_templates\":"
           << definition.runtime_code_templates.size()
           << ",\"function_overrides\":"
           << definition.function_overrides.size()
           << ",\"mid_function_hooks\":"
           << definition.mid_function_hooks.size() << ",\"symbols\":"
           << definition.symbols.size() << ",\"code_identities\":"
           << definition.code_identities.size() << "}\n";
    return output.str();
}

bool has_proven_linear_ram_access(const katana::ir::Instruction& instruction) noexcept {
    return instruction.memory_effects.access != katana::ir::MemoryAccessKind::None &&
           instruction.memory_effects.region == katana::ir::MemoryRegionKind::NormalRam;
}

bool equivalent_ir_instruction(const katana::ir::Instruction& left,
                               const katana::ir::Instruction& right) {
    return left.source_address == right.source_address &&
           left.original_opcode == right.original_opcode &&
           left.original_operation == right.original_operation &&
           left.operation == right.operation && left.widths == right.widths &&
           left.status_effects == right.status_effects &&
           left.memory_effects == right.memory_effects &&
           left.accumulator_effects == right.accumulator_effects &&
           left.destination_register == right.destination_register &&
           left.source_register == right.source_register &&
           left.branch_register == right.branch_register &&
           left.immediate == right.immediate &&
           left.displacement == right.displacement &&
           left.special_register == right.special_register &&
           left.effective_address == right.effective_address &&
           left.target_address == right.target_address &&
           left.resolved_targets == right.resolved_targets &&
           left.forwarded_value_register == right.forwarded_value_register &&
           left.dynamic_target_class == right.dynamic_target_class &&
           left.delay_slot == right.delay_slot &&
           left.is_privileged == right.is_privileged &&
           left.branch_register_relative == right.branch_register_relative;
}

bool equivalent_ir_block(const katana::ir::BasicBlock& left,
                         const katana::ir::BasicBlock& right) {
    return left.start_address == right.start_address &&
           left.successors == right.successors &&
           left.has_indirect_successor == right.has_indirect_successor &&
           left.instructions.size() == right.instructions.size() &&
           std::equal(left.instructions.begin(),
                      left.instructions.end(),
                      right.instructions.begin(),
                      equivalent_ir_instruction);
}

std::string guarded_aot_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << address;
    return output.str();
}

void require_guarded_aot_program_entries(
    const std::span<const katana::ir::Function> program,
    const std::span<const katana::analysis::GuardedAotEntry> entries,
    const std::string_view phase) {
    std::unordered_map<std::uint32_t, const katana::ir::BasicBlock*>
        canonical_blocks;
    std::unordered_set<std::uint32_t> divergent_blocks;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            const auto [existing, inserted] =
                canonical_blocks.emplace(block.start_address, &block);
            if (!inserted &&
                !equivalent_ir_block(*existing->second, block))
                divergent_blocks.insert(block.start_address);
        }
    }

    std::unordered_set<std::uint32_t> seen_entries;
    seen_entries.reserve(entries.size());
    for (const auto& entry : entries) {
        if (!seen_entries.insert(entry.guest_address).second)
            throw std::runtime_error(
                "Guarded-AOT-Vertrag enthaelt einen doppelten Einstieg bei " +
                guarded_aot_address(entry.guest_address) + " (" +
                std::string(phase) + ").");
        const auto found = canonical_blocks.find(entry.guest_address);
        if (found == canonical_blocks.end())
            throw std::runtime_error(
                "Guarded-AOT-Einstieg besitzt keinen eigenstaendigen "
                "IR-Blockstart bei " +
                guarded_aot_address(entry.guest_address) + " (" +
                std::string(phase) + ").");
        if (divergent_blocks.contains(entry.guest_address))
            throw std::runtime_error(
                "IR-Basic-Block besitzt abweichende Funktionsbesitzer.");
        if (entry.shared_body_address == entry.guest_address) continue;
        if (!canonical_blocks.contains(entry.shared_body_address))
            throw std::runtime_error(
                "Guarded-AOT-Einstieg verlor seinen emittierbaren Shared "
                "Body bei " +
                guarded_aot_address(entry.guest_address) + " -> " +
                guarded_aot_address(entry.shared_body_address) + " (" +
                std::string(phase) + ").");
        if (divergent_blocks.contains(entry.shared_body_address))
            throw std::runtime_error(
                "IR-Basic-Block besitzt abweichende Funktionsbesitzer.");
    }
}

bool requires_architectural_safepoint_entry(
    const katana::ir::Instruction& instruction) noexcept {
    using Operation = katana::ir::Operation;
    using Register = katana::ir::SpecialRegister;

    if (instruction.delay_slot.role == katana::ir::DelaySlotRole::Slot)
        return false;
    if ((instruction.operation == Operation::LoadSpecialRegister ||
         instruction.operation == Operation::LoadSpecialRegisterPostIncrement) &&
        (instruction.special_register == Register::Sr ||
         instruction.special_register == Register::Fpscr))
        return true;

    return instruction.operation == Operation::LoadTlb ||
           instruction.operation == Operation::Frchg ||
           instruction.operation == Operation::Fschg;
}

void require_architectural_safepoint_program_entries(
    const std::span<const katana::ir::Function> program,
    const std::string_view phase) {
    std::unordered_set<std::uint32_t> block_entries;
    for (const auto& function : program)
        for (const auto& block : function.blocks)
            block_entries.insert(block.start_address);

    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (!requires_architectural_safepoint_entry(instruction))
                    continue;
                if (instruction.source_address >
                    std::numeric_limits<std::uint32_t>::max() - 2u)
                    throw std::runtime_error(
                        "Architektonische Safepoint-Fortsetzung laeuft "
                        "ueber.");
                const auto continuation = instruction.source_address + 2u;
                if (!block_entries.contains(continuation))
                    throw std::runtime_error(
                        "Architektonische Safepoint-Fortsetzung besitzt "
                        "keinen eigenstaendigen emittierten IR-Blockstart: " +
                        guarded_aot_address(instruction.source_address) +
                        " -> " + guarded_aot_address(continuation) + " (" +
                        std::string(phase) + ").");
            }
        }
    }
}

struct CountedLoopBatchProof {
    struct Descriptor {
        std::uint32_t guard_address = 0u;
        std::uint32_t increment_address = 0u;
        std::uint32_t increment_size = 0u;
        std::uint32_t limit_address = 0u;
        std::uint32_t first_counter_read_instruction_address = 0u;
        std::uint32_t store_instruction_address = 0u;
        std::uint32_t pre_store_instruction_address = 0u;
        std::int32_t counter_displacement = 0;
        std::uint8_t counter_base_register = 0u;
        std::uint8_t limit_register = 0u;
        std::uint8_t compare_register = 0u;
        std::uint8_t increment_register = 0u;
        std::uint8_t step = 0u;
        std::uint8_t limit_width = 0u;
        std::uint8_t guard_instruction_count = 0u;
        std::uint8_t round_instruction_count = 0u;
        std::uint8_t prefix_guest_cycles = 0u;
        std::uint8_t first_counter_read_guest_cycles = 0u;
        std::uint8_t store_guest_cycles = 0u;
        bool signed_word_limit = false;
        std::uint64_t guard_guest_cycles = 0u;
        std::uint64_t round_guest_cycles = 0u;
    } descriptor;
};

struct MmioWaitLoopBatchProof {
    struct Descriptor {
        std::uint32_t loop_header = 0u;
        std::uint32_t read_site = 0u;
        std::uint32_t pointer_literal_address = 0u;
        std::uint32_t mmio_guest_address = 0u;
        std::uint32_t mmio_physical_address = 0u;
        std::uint32_t backedge_instruction_address = 0u;
        std::uint8_t pointer_register = 0u;
        std::uint8_t value_register = 0u;
        std::uint8_t mask_register = 0xFFu;
        std::uint8_t round_instruction_count = 0u;
        std::uint32_t test_mask = 0u;
        bool pointer_from_register = false;
        bool branch_on_true = false;
        std::uint64_t round_guest_cycles = 0u;
        std::uint64_t pre_read_guest_cycles = 0u;
        std::uint8_t read_guest_cycles = 0u;
        std::uint8_t test_guest_cycles = 0u;
        std::uint8_t branch_guest_cycles = 0u;
    } descriptor;
};

struct MemoryFillLoopBatchProof {
    struct Descriptor {
        std::uint32_t guard_address = 0u;
        std::uint32_t body_address = 0u;
        std::uint32_t exit_address = 0u;
        std::uint32_t store_instruction_address = 0u;
        std::uint32_t increment_instruction_address = 0u;
        std::uint32_t limit_load_instruction_address = 0u;
        std::uint32_t compare_instruction_address = 0u;
        std::uint32_t branch_instruction_address = 0u;
        std::uint32_t guard_size = 0u;
        std::uint32_t body_size = 0u;
        std::uint8_t cursor_register = 0u;
        std::uint8_t fill_register = 0u;
        std::uint8_t limit_register = 0u;
        std::uint8_t limit_pointer_register = 0u;
        std::uint8_t store_width = 0u;
        std::uint8_t step = 0u;
        std::uint8_t round_instruction_count = 0u;
        std::uint8_t guard_instruction_count = 0u;
        std::uint8_t store_guest_cycles = 0u;
        std::uint64_t guard_guest_cycles = 0u;
        std::uint64_t round_guest_cycles = 0u;
    } descriptor;
};

struct CompositeCallbackBatchProof {
    enum class Kind : std::uint8_t {
        MemoryCopy,
        FlagPollEqualImmediate,
    };

    struct Descriptor {
        Kind kind = Kind::MemoryCopy;
        std::uint32_t call_block_address = 0u;
        std::uint32_t call_instruction_address = 0u;
        std::uint32_t continuation_address = 0u;
        std::uint32_t exit_address = 0u;
        std::uint32_t kernel_address = 0u;
        std::uint32_t kernel_return_address = 0u;
        std::uint32_t source_load_instruction_address = 0u;
        std::uint32_t target_store_instruction_address = 0u;
        std::uint32_t outer_branch_instruction_address = 0u;
        std::uint32_t call_block_size = 0u;
        std::uint32_t continuation_size = 0u;
        std::uint32_t kernel_size = 0u;
        std::uint32_t kernel_return_size = 0u;
        std::uint8_t callback_register = 0u;
        std::uint8_t destination_register = 0u;
        std::uint8_t count_register = 0u;
        std::uint8_t limit_register = 0u;
        std::uint8_t flag_base_register = 0u;
        std::uint8_t flag_value_register = 0u;
        std::int32_t flag_displacement = 0;
        std::uint32_t flag_expected_value = 0u;
        std::uint8_t current_round_instruction_count = 0u;
        std::uint8_t subsequent_round_instruction_count = 0u;
        std::uint64_t call_block_guest_cycles = 0u;
        std::uint64_t continuation_guest_cycles = 0u;
        std::uint64_t kernel_guest_cycles = 0u;
        std::uint64_t kernel_return_guest_cycles = 0u;
        std::uint64_t current_round_guest_cycles = 0u;
        std::uint64_t subsequent_round_guest_cycles = 0u;
    } descriptor;
};

std::vector<MemoryFillLoopBatchProof>
memory_fill_loop_batch_proofs(const std::span<const katana::ir::Function> program) {
    using Operation = katana::ir::Operation;
    struct BlockOwner {
        const katana::ir::Function* function = nullptr;
        const katana::ir::BasicBlock* block = nullptr;
    };

    std::unordered_map<std::uint32_t, BlockOwner> blocks;
    std::unordered_set<std::uint32_t> duplicate_block_addresses;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> predecessors;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            if (!blocks.emplace(block.start_address, BlockOwner{&function, &block}).second)
                duplicate_block_addresses.insert(block.start_address);
            for (const auto successor : block.successors)
                predecessors[successor].push_back(block.start_address);
        }
    }

    const auto exact_contiguous_block = [](const katana::ir::BasicBlock& block,
                                           const std::size_t expected_instruction_count) {
        if (block.instructions.size() != expected_instruction_count ||
            expected_instruction_count == 0u ||
            expected_instruction_count > std::numeric_limits<std::uint32_t>::max() / 2u)
            return false;
        const auto byte_size = static_cast<std::uint64_t>(expected_instruction_count) * 2u;
        if (static_cast<std::uint64_t>(block.start_address) + byte_size > 0x1'0000'0000ull)
            return false;
        for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
            if (block.instructions[index].source_address !=
                    static_cast<std::uint64_t>(block.start_address) + index * 2u ||
                block.instructions[index].delay_slot.role !=
                    katana::ir::DelaySlotRole::None)
                return false;
        }
        return true;
    };
    const auto exact_successors = [](const katana::ir::BasicBlock& block,
                                     std::initializer_list<std::uint32_t> expected) {
        auto actual = block.successors;
        auto wanted = std::vector<std::uint32_t>(expected);
        std::sort(actual.begin(), actual.end());
        std::sort(wanted.begin(), wanted.end());
        return actual == wanted;
    };

    std::vector<MemoryFillLoopBatchProof> result;
    for (const auto& function : program) {
        for (const auto& guard : function.blocks) {
            if (duplicate_block_addresses.contains(guard.start_address) ||
                !exact_contiguous_block(guard, 3u) || guard.has_indirect_successor)
                continue;
            const auto& limit_load = guard.instructions[0];
            const auto& compare = guard.instructions[1];
            const auto& branch = guard.instructions[2];
            if (limit_load.operation != Operation::LoadLong ||
                limit_load.forwarded_value_register.has_value() ||
                limit_load.memory_effects.access != katana::ir::MemoryAccessKind::Read ||
                limit_load.memory_effects.width != katana::ir::OperandWidth::Bits32 ||
                limit_load.memory_effects.access_count != 1u ||
                compare.operation != Operation::CompareHigherOrSame ||
                branch.operation != Operation::BranchIfFalse ||
                !branch.target_address.has_value())
                continue;

            const auto body_found = blocks.find(*branch.target_address);
            if (body_found == blocks.end() ||
                duplicate_block_addresses.contains(*branch.target_address) ||
                body_found->second.function != &function)
                continue;
            const auto& body = *body_found->second.block;
            if (!exact_contiguous_block(body, 2u) || body.has_indirect_successor)
                continue;
            const auto& store = body.instructions[0];
            const auto& increment = body.instructions[1];
            if (store.operation != Operation::StoreByte ||
                store.memory_effects.access != katana::ir::MemoryAccessKind::Write ||
                store.memory_effects.width != katana::ir::OperandWidth::Bits8 ||
                store.memory_effects.access_count != 1u ||
                increment.operation != Operation::AddImmediate || increment.immediate != 1 ||
                increment.destination_register != store.destination_register ||
                compare.destination_register != store.destination_register ||
                compare.source_register != limit_load.destination_register)
                continue;

            if (branch.source_address > std::numeric_limits<std::uint32_t>::max() - 2u)
                continue;
            const auto exit_address = branch.source_address + 2u;
            if (!exact_successors(guard, {body.start_address, exit_address}) ||
                !exact_successors(body, {guard.start_address}))
                continue;
            const auto incoming = predecessors.find(body.start_address);
            if (incoming == predecessors.end() || incoming->second.size() != 1u ||
                incoming->second.front() != guard.start_address)
                continue;

            const std::array registers{store.destination_register,
                                       store.source_register,
                                       limit_load.destination_register,
                                       limit_load.source_register};
            auto unique_registers = registers;
            std::sort(unique_registers.begin(), unique_registers.end());
            if (std::adjacent_find(unique_registers.begin(), unique_registers.end()) !=
                unique_registers.end())
                continue;

            std::uint64_t guard_cycles = 0u;
            std::uint64_t body_cycles = 0u;
            for (const auto& instruction : guard.instructions)
                guard_cycles +=
                    katana::sh4::instruction_timing(instruction.original_opcode).guest_cycles;
            for (const auto& instruction : body.instructions)
                body_cycles +=
                    katana::sh4::instruction_timing(instruction.original_opcode).guest_cycles;
            const auto store_timing = katana::sh4::instruction_timing(store.original_opcode);
            const auto limit_load_timing =
                katana::sh4::instruction_timing(limit_load.original_opcode);
            constexpr auto byte_max =
                static_cast<std::uint64_t>(std::numeric_limits<std::uint8_t>::max());
            const auto round_cycles = guard_cycles + body_cycles;
            const auto round_instructions = guard.instructions.size() + body.instructions.size();
            if (!store_timing.requires_cycle_flush || !limit_load_timing.requires_cycle_flush ||
                store_timing.guest_cycles == 0u || store_timing.guest_cycles > byte_max ||
                guard_cycles == 0u || guard_cycles >= round_cycles ||
                round_cycles > 4096u || round_instructions > byte_max ||
                guard.instructions.size() > byte_max)
                continue;

            MemoryFillLoopBatchProof::Descriptor descriptor;
            descriptor.guard_address = guard.start_address;
            descriptor.body_address = body.start_address;
            descriptor.exit_address = exit_address;
            descriptor.store_instruction_address = store.source_address;
            descriptor.increment_instruction_address = increment.source_address;
            descriptor.limit_load_instruction_address = limit_load.source_address;
            descriptor.compare_instruction_address = compare.source_address;
            descriptor.branch_instruction_address = branch.source_address;
            descriptor.guard_size =
                static_cast<std::uint32_t>(guard.instructions.size() * 2u);
            descriptor.body_size =
                static_cast<std::uint32_t>(body.instructions.size() * 2u);
            descriptor.cursor_register = store.destination_register;
            descriptor.fill_register = store.source_register;
            descriptor.limit_register = limit_load.destination_register;
            descriptor.limit_pointer_register = limit_load.source_register;
            descriptor.store_width = 1u;
            descriptor.step = 1u;
            descriptor.round_instruction_count =
                static_cast<std::uint8_t>(round_instructions);
            descriptor.guard_instruction_count =
                static_cast<std::uint8_t>(guard.instructions.size());
            descriptor.store_guest_cycles =
                static_cast<std::uint8_t>(store_timing.guest_cycles);
            descriptor.guard_guest_cycles = guard_cycles;
            descriptor.round_guest_cycles = round_cycles;
            result.push_back({descriptor});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.descriptor.guard_address, left.descriptor.body_address} <
               std::tuple{right.descriptor.guard_address, right.descriptor.body_address};
    });
    return result;
}

std::vector<CompositeCallbackBatchProof> composite_callback_batch_proofs(
    const std::span<const katana::ir::Function> program,
    const std::span<const katana::analysis::IndirectControlFlowResolution>
        indirect_control_flow,
    const std::span<const katana::analysis::FunctionCandidate> function_candidates) {
    using Operation = katana::ir::Operation;
    struct BlockOwner {
        const katana::ir::Function* function = nullptr;
        const katana::ir::BasicBlock* block = nullptr;
    };
    struct KernelProof {
        const katana::ir::Function* function = nullptr;
        const katana::ir::BasicBlock* body = nullptr;
        const katana::ir::BasicBlock* return_block = nullptr;
        std::uint64_t guest_cycles = 0u;
    };
    struct ReturnCallbackProof {
        const katana::ir::BasicBlock* block = nullptr;
        std::uint64_t guest_cycles = 0u;
    };

    std::unordered_map<std::uint32_t, BlockOwner> blocks;
    std::unordered_map<std::uint32_t, std::vector<BlockOwner>> block_owners;
    std::unordered_set<std::uint32_t> duplicate_block_addresses;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            block_owners[block.start_address].push_back(BlockOwner{&function, &block});
            if (!blocks.emplace(block.start_address, BlockOwner{&function, &block}).second)
                duplicate_block_addresses.insert(block.start_address);
        }
    }

    const auto exact_contiguous_block = [](const katana::ir::BasicBlock& block,
                                           const std::size_t expected_instruction_count) {
        if (block.instructions.size() != expected_instruction_count ||
            expected_instruction_count == 0u ||
            expected_instruction_count > std::numeric_limits<std::uint32_t>::max() / 2u)
            return false;
        const auto byte_size = static_cast<std::uint64_t>(expected_instruction_count) * 2u;
        if (static_cast<std::uint64_t>(block.start_address) + byte_size > 0x1'0000'0000ull)
            return false;
        for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
            if (block.instructions[index].source_address !=
                static_cast<std::uint64_t>(block.start_address) + index * 2u)
                return false;
        }
        return true;
    };
    const auto exact_successors = [](const katana::ir::BasicBlock& block,
                                     std::initializer_list<std::uint32_t> expected) {
        auto actual = block.successors;
        auto wanted = std::vector<std::uint32_t>(expected);
        std::sort(actual.begin(), actual.end());
        std::sort(wanted.begin(), wanted.end());
        return actual == wanted;
    };
    const auto exact_delay_roles =
        [](const katana::ir::BasicBlock& block,
           const std::initializer_list<katana::ir::DelaySlotRole> expected) {
            if (block.instructions.size() != expected.size()) return false;
            return std::equal(
                block.instructions.begin(),
                block.instructions.end(),
                expected.begin(),
                [](const auto& instruction, const auto role) {
                    return instruction.delay_slot.role == role;
                });
        };
    const auto block_guest_cycles =
        [](const katana::ir::BasicBlock& block) -> std::optional<std::uint64_t> {
            std::uint64_t cycles = 0u;
            for (const auto& instruction : block.instructions) {
                const auto timing =
                    katana::sh4::instruction_timing(instruction.original_opcode);
                if (timing.guest_cycles == 0u ||
                    cycles > std::numeric_limits<std::uint64_t>::max() -
                                 timing.guest_cycles)
                    return std::nullopt;
                cycles += timing.guest_cycles;
            }
            return cycles;
        };
    std::unordered_set<std::uint32_t> provenance_strong_candidates;
    for (const auto& candidate : function_candidates) {
        const bool strong = std::any_of(
            candidate.origins.begin(), candidate.origins.end(), [](const auto origin) {
                using Origin = katana::analysis::FunctionOrigin;
                return origin == Origin::GuardedSnapshot ||
                       origin == Origin::RuntimeCopy ||
                       origin == Origin::StoredCodeAddress;
            });
        if (strong &&
            (candidate.evidence ==
                 katana::analysis::ControlFlowEvidence::GuardedPartial ||
             candidate.evidence ==
                 katana::analysis::ControlFlowEvidence::RuntimeOnly))
            provenance_strong_candidates.insert(candidate.address);
    }

    std::unordered_map<std::uint32_t, KernelProof> kernels;
    for (const auto& [kernel_address, owners] : block_owners) {
        if (!provenance_strong_candidates.contains(kernel_address) || owners.empty())
            continue;
        const auto& canonical_owner = owners.front();
        const auto& function = *canonical_owner.function;
        const auto& body = *canonical_owner.block;
        if (std::any_of(owners.begin(), owners.end(), [&](const auto& owner) {
                return !equivalent_ir_block(body, *owner.block);
            }))
            continue;
        {
            if (
                !exact_contiguous_block(body, 6u) || body.has_indirect_successor ||
                !exact_delay_roles(body,
                                   {katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::Owner,
                                    katana::ir::DelaySlotRole::Slot}))
                continue;
            const auto& shift = body.instructions[0];
            const auto& load = body.instructions[1];
            const auto& decrement = body.instructions[2];
            const auto& store = body.instructions[3];
            const auto& branch = body.instructions[4];
            const auto& delay_increment = body.instructions[5];
            if (shift.operation != Operation::ShiftLogicalRightTwo ||
                shift.destination_register != 6u ||
                load.operation != Operation::LoadLongPostIncrement ||
                load.destination_register != 0u || load.source_register != 5u ||
                load.forwarded_value_register.has_value() ||
                load.memory_effects.access != katana::ir::MemoryAccessKind::Read ||
                load.memory_effects.width != katana::ir::OperandWidth::Bits32 ||
                load.memory_effects.access_count != 1u ||
                load.memory_effects.region ==
                    katana::ir::MemoryRegionKind::Volatile ||
                decrement.operation != Operation::DecrementAndTest ||
                decrement.destination_register != 6u ||
                store.operation != Operation::StoreLong ||
                store.destination_register != 4u || store.source_register != 0u ||
                store.memory_effects.access != katana::ir::MemoryAccessKind::Write ||
                store.memory_effects.width != katana::ir::OperandWidth::Bits32 ||
                store.memory_effects.access_count != 1u ||
                store.memory_effects.region ==
                    katana::ir::MemoryRegionKind::Volatile ||
                branch.operation != Operation::BranchIfFalse ||
                !branch.target_address.has_value() ||
                *branch.target_address != body.start_address ||
                delay_increment.operation != Operation::AddImmediate ||
                delay_increment.destination_register != 4u ||
                delay_increment.immediate != 4)
                continue;

            if (body.start_address >
                std::numeric_limits<std::uint32_t>::max() - 12u)
                continue;
            const auto return_address = body.start_address + 12u;
            if (!exact_successors(body, {body.start_address, return_address}))
                continue;
            const auto returned = block_owners.find(return_address);
            if (returned == block_owners.end() || returned->second.empty())
                continue;
            const auto& return_block = *returned->second.front().block;
            if (std::any_of(returned->second.begin(),
                            returned->second.end(),
                            [&](const auto& owner) {
                                return !equivalent_ir_block(return_block, *owner.block);
                            }))
                continue;
            const auto same_owner_functions = [&] {
                std::vector<const katana::ir::Function*> body_functions;
                std::vector<const katana::ir::Function*> return_functions;
                body_functions.reserve(owners.size());
                return_functions.reserve(returned->second.size());
                for (const auto& owner : owners)
                    body_functions.push_back(owner.function);
                for (const auto& owner : returned->second)
                    return_functions.push_back(owner.function);
                std::sort(body_functions.begin(), body_functions.end());
                std::sort(return_functions.begin(), return_functions.end());
                return body_functions == return_functions;
            };
            if (!same_owner_functions())
                continue;
            if (!exact_contiguous_block(return_block, 2u) ||
                return_block.has_indirect_successor ||
                !exact_successors(return_block, {}) ||
                !exact_delay_roles(return_block,
                                   {katana::ir::DelaySlotRole::Owner,
                                    katana::ir::DelaySlotRole::Slot}) ||
                return_block.instructions[0].operation != Operation::Return ||
                return_block.instructions[1].operation != Operation::Nop)
                continue;
            const auto body_cycles = block_guest_cycles(body);
            const auto return_cycles = block_guest_cycles(return_block);
            if (!body_cycles || !return_cycles ||
                *body_cycles > std::numeric_limits<std::uint64_t>::max() - *return_cycles)
                continue;
            kernels.emplace(body.start_address,
                            KernelProof{&function,
                                        &body,
                                        &return_block,
                                        *body_cycles + *return_cycles});
        }
    }

    std::unordered_map<std::uint32_t, ReturnCallbackProof> return_callbacks;
    for (const auto& [callback_address, owners] : block_owners) {
        if (owners.empty())
            continue;
        const auto& callback = *owners.front().block;
        if (std::any_of(owners.begin(), owners.end(), [&](const auto& owner) {
                return !equivalent_ir_block(callback, *owner.block);
            }) ||
            !exact_contiguous_block(callback, 2u) ||
            callback.has_indirect_successor ||
            !exact_successors(callback, {}) ||
            !exact_delay_roles(callback,
                               {katana::ir::DelaySlotRole::Owner,
                                katana::ir::DelaySlotRole::Slot}) ||
            callback.instructions[0].operation != Operation::Return ||
            callback.instructions[1].operation != Operation::Nop)
            continue;
        const auto cycles = block_guest_cycles(callback);
        if (!cycles || *cycles == 0u || *cycles > 4096u)
            continue;
        return_callbacks.emplace(
            callback_address, ReturnCallbackProof{&callback, *cycles});
    }

    std::unordered_map<std::uint32_t,
                       const katana::analysis::IndirectControlFlowResolution*>
        resolution_by_callsite;
    for (const auto& resolution : indirect_control_flow) {
        if (resolution.kind != katana::analysis::IndirectControlFlowKind::Call ||
            resolution.evidence != katana::analysis::ControlFlowEvidence::RuntimeOnly ||
            resolution.target.has_value() || !resolution.targets.empty() ||
            resolution.analysis_candidates.empty())
            continue;
        resolution_by_callsite.emplace(resolution.instruction_address, &resolution);
    }

    struct SingletonCallResolution {
        const katana::analysis::IndirectControlFlowResolution* resolution = nullptr;
        std::uint32_t candidate = 0u;
    };
    std::unordered_map<std::uint32_t, SingletonCallResolution>
        singleton_resolution_by_callsite;
    std::unordered_set<std::uint32_t> duplicate_singleton_callsites;
    for (const auto& resolution : indirect_control_flow) {
        if (resolution.kind != katana::analysis::IndirectControlFlowKind::Call)
            continue;
        auto candidates = resolution.targets;
        if (resolution.target)
            candidates.push_back(*resolution.target);
        candidates.insert(candidates.end(),
                          resolution.analysis_candidates.begin(),
                          resolution.analysis_candidates.end());
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());
        if (candidates.size() != 1u)
            continue;
        if (!singleton_resolution_by_callsite
                 .emplace(resolution.instruction_address,
                          SingletonCallResolution{&resolution, candidates.front()})
                 .second)
            duplicate_singleton_callsites.insert(resolution.instruction_address);
    }
    for (const auto callsite : duplicate_singleton_callsites)
        singleton_resolution_by_callsite.erase(callsite);

    std::vector<CompositeCallbackBatchProof> result;
    for (const auto& function : program) {
        for (const auto& call_block : function.blocks) {
            if (duplicate_block_addresses.contains(call_block.start_address) ||
                !exact_contiguous_block(call_block, 4u) ||
                !call_block.has_indirect_successor ||
                !exact_delay_roles(call_block,
                                   {katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::Owner,
                                    katana::ir::DelaySlotRole::Slot}))
                continue;
            const auto& source_setup = call_block.instructions[0];
            const auto& length_setup = call_block.instructions[1];
            const auto& call = call_block.instructions[2];
            const auto& destination_setup = call_block.instructions[3];
            if (source_setup.operation != Operation::MovRegister ||
                source_setup.source_register != 15u ||
                source_setup.destination_register != 5u ||
                length_setup.operation != Operation::MovImmediate ||
                length_setup.destination_register != 6u ||
                length_setup.immediate != 4 ||
                call.operation != Operation::CallRegister ||
                call.dynamic_target_class != katana::ir::DynamicTargetClass::RuntimeOnly ||
                call.target_address.has_value() || !call.resolved_targets.empty() ||
                destination_setup.operation != Operation::MovRegister ||
                destination_setup.destination_register != 4u)
                continue;
            const auto resolution = resolution_by_callsite.find(call.source_address);
            if (resolution == resolution_by_callsite.end() ||
                resolution->second->register_index != call.branch_register)
                continue;
            if (call.source_address >
                std::numeric_limits<std::uint32_t>::max() - 4u)
                continue;
            const auto continuation_address = call.source_address + 4u;
            if (!exact_successors(call_block, {continuation_address}))
                continue;
            const auto continuation_found = blocks.find(continuation_address);
            if (continuation_found == blocks.end() ||
                duplicate_block_addresses.contains(continuation_address) ||
                continuation_found->second.function != &function)
                continue;
            const auto& continuation = *continuation_found->second.block;
            if (!exact_contiguous_block(continuation, 4u) ||
                continuation.has_indirect_successor ||
                !exact_delay_roles(continuation,
                                   {katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::Owner,
                                    katana::ir::DelaySlotRole::Slot}))
                continue;
            const auto& count_increment = continuation.instructions[0];
            const auto& compare = continuation.instructions[1];
            const auto& branch = continuation.instructions[2];
            const auto& destination_increment = continuation.instructions[3];
            if (count_increment.operation != Operation::AddImmediate ||
                count_increment.immediate != 1 ||
                compare.operation != Operation::CompareGreaterOrEqual ||
                compare.destination_register != count_increment.destination_register ||
                branch.operation != Operation::BranchIfFalse ||
                !branch.target_address.has_value() ||
                *branch.target_address != call_block.start_address ||
                destination_increment.operation != Operation::AddImmediate ||
                destination_increment.destination_register !=
                    destination_setup.source_register ||
                destination_increment.immediate != 4)
                continue;
            if (continuation_address >
                std::numeric_limits<std::uint32_t>::max() - 8u)
                continue;
            const auto exit_address = continuation_address + 8u;
            if (!exact_successors(
                    continuation, {call_block.start_address, exit_address}))
                continue;

            const std::array variable_registers{
                call.branch_register,
                destination_setup.source_register,
                count_increment.destination_register,
                compare.source_register,
            };
            auto sorted_registers = variable_registers;
            std::sort(sorted_registers.begin(), sorted_registers.end());
            if (std::adjacent_find(sorted_registers.begin(), sorted_registers.end()) !=
                    sorted_registers.end() ||
                std::any_of(variable_registers.begin(),
                            variable_registers.end(),
                            [](const auto reg) {
                                return reg == 0u || reg == 4u || reg == 5u ||
                                       reg == 6u || reg == 15u;
                            }))
                continue;

            const auto continuation_cycles = block_guest_cycles(continuation);
            const auto call_cycles = block_guest_cycles(call_block);
            if (!continuation_cycles || !call_cycles)
                continue;
            constexpr auto byte_max =
                static_cast<std::uint64_t>(std::numeric_limits<std::uint8_t>::max());
            for (const auto candidate : resolution->second->analysis_candidates) {
                const auto kernel = kernels.find(candidate);
                if (kernel == kernels.end())
                    continue;
                const auto current_cycles =
                    kernel->second.guest_cycles + *continuation_cycles;
                if (current_cycles < kernel->second.guest_cycles ||
                    current_cycles >
                        std::numeric_limits<std::uint64_t>::max() - *call_cycles)
                    continue;
                const auto subsequent_cycles = current_cycles + *call_cycles;
                const auto current_instructions =
                    kernel->second.body->instructions.size() +
                    kernel->second.return_block->instructions.size() +
                    continuation.instructions.size();
                const auto subsequent_instructions =
                    current_instructions + call_block.instructions.size();
                if (current_cycles == 0u || subsequent_cycles == 0u ||
                    current_cycles > 4096u || subsequent_cycles > 4096u ||
                    current_instructions > byte_max ||
                    subsequent_instructions > byte_max)
                    continue;

                CompositeCallbackBatchProof::Descriptor descriptor;
                descriptor.call_block_address = call_block.start_address;
                descriptor.call_instruction_address = call.source_address;
                descriptor.continuation_address = continuation_address;
                descriptor.exit_address = exit_address;
                descriptor.kernel_address = kernel->second.body->start_address;
                descriptor.kernel_return_address =
                    kernel->second.return_block->start_address;
                descriptor.source_load_instruction_address =
                    kernel->second.body->instructions[1].source_address;
                descriptor.target_store_instruction_address =
                    kernel->second.body->instructions[3].source_address;
                descriptor.outer_branch_instruction_address = branch.source_address;
                descriptor.call_block_size =
                    static_cast<std::uint32_t>(call_block.instructions.size() * 2u);
                descriptor.continuation_size =
                    static_cast<std::uint32_t>(continuation.instructions.size() * 2u);
                descriptor.kernel_size = static_cast<std::uint32_t>(
                    kernel->second.body->instructions.size() * 2u);
                descriptor.kernel_return_size = static_cast<std::uint32_t>(
                    kernel->second.return_block->instructions.size() * 2u);
                descriptor.callback_register = call.branch_register;
                descriptor.destination_register =
                    destination_setup.source_register;
                descriptor.count_register =
                    count_increment.destination_register;
                descriptor.limit_register = compare.source_register;
                descriptor.current_round_instruction_count =
                    static_cast<std::uint8_t>(current_instructions);
                descriptor.subsequent_round_instruction_count =
                    static_cast<std::uint8_t>(subsequent_instructions);
                descriptor.call_block_guest_cycles = *call_cycles;
                descriptor.continuation_guest_cycles = *continuation_cycles;
                descriptor.kernel_guest_cycles =
                    *block_guest_cycles(*kernel->second.body);
                descriptor.kernel_return_guest_cycles =
                    *block_guest_cycles(*kernel->second.return_block);
                descriptor.current_round_guest_cycles = current_cycles;
                descriptor.subsequent_round_guest_cycles = subsequent_cycles;
                result.push_back({descriptor});
            }
        }
    }

    for (const auto& function : program) {
        for (const auto& call_block : function.blocks) {
            if (duplicate_block_addresses.contains(call_block.start_address) ||
                !exact_contiguous_block(call_block, 2u) ||
                !call_block.has_indirect_successor ||
                !exact_delay_roles(call_block,
                                   {katana::ir::DelaySlotRole::Owner,
                                    katana::ir::DelaySlotRole::Slot}))
                continue;
            const auto& call = call_block.instructions[0];
            const auto& delay = call_block.instructions[1];
            if (call.operation != Operation::CallRegister ||
                delay.operation != Operation::Nop)
                continue;
            const auto resolution =
                singleton_resolution_by_callsite.find(call.source_address);
            if (resolution == singleton_resolution_by_callsite.end() ||
                resolution->second.resolution->register_index !=
                    call.branch_register)
                continue;
            const auto callback =
                return_callbacks.find(resolution->second.candidate);
            if (callback == return_callbacks.end())
                continue;
            if (call.source_address >
                std::numeric_limits<std::uint32_t>::max() - 4u)
                continue;
            const auto continuation_address = call.source_address + 4u;
            if (!exact_successors(call_block, {continuation_address}))
                continue;
            const auto continuation_found = blocks.find(continuation_address);
            if (continuation_found == blocks.end() ||
                duplicate_block_addresses.contains(continuation_address) ||
                continuation_found->second.function != &function)
                continue;
            const auto& continuation = *continuation_found->second.block;
            if (!exact_contiguous_block(continuation, 3u) ||
                continuation.has_indirect_successor ||
                !exact_delay_roles(continuation,
                                   {katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::None,
                                    katana::ir::DelaySlotRole::None}))
                continue;
            const auto& load = continuation.instructions[0];
            const auto& compare = continuation.instructions[1];
            const auto& branch = continuation.instructions[2];
            if (load.operation != Operation::LoadLongDisplacement ||
                load.forwarded_value_register.has_value() ||
                load.memory_effects.access != katana::ir::MemoryAccessKind::Read ||
                load.memory_effects.width != katana::ir::OperandWidth::Bits32 ||
                load.memory_effects.access_count != 1u ||
                load.memory_effects.region ==
                    katana::ir::MemoryRegionKind::Volatile ||
                load.displacement < 0 ||
                compare.operation != Operation::CompareEqualImmediate ||
                compare.destination_register != load.destination_register ||
                load.destination_register != 0u ||
                branch.operation != Operation::BranchIfFalse ||
                !branch.target_address.has_value() ||
                *branch.target_address != call_block.start_address)
                continue;
            if (continuation_address >
                std::numeric_limits<std::uint32_t>::max() - 6u)
                continue;
            const auto exit_address = continuation_address + 6u;
            if (!exact_successors(
                    continuation, {call_block.start_address, exit_address}))
                continue;

            const auto call_cycles = block_guest_cycles(call_block);
            const auto continuation_cycles = block_guest_cycles(continuation);
            if (!call_cycles || !continuation_cycles ||
                callback->second.guest_cycles >
                    std::numeric_limits<std::uint64_t>::max() -
                        *continuation_cycles)
                continue;
            const auto current_cycles =
                callback->second.guest_cycles + *continuation_cycles;
            if (current_cycles >
                std::numeric_limits<std::uint64_t>::max() - *call_cycles)
                continue;
            const auto subsequent_cycles = current_cycles + *call_cycles;
            const auto current_instructions =
                callback->second.block->instructions.size() +
                continuation.instructions.size();
            const auto subsequent_instructions =
                current_instructions + call_block.instructions.size();
            constexpr auto byte_max =
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint8_t>::max());
            if (current_cycles == 0u || subsequent_cycles == 0u ||
                current_cycles > 4096u || subsequent_cycles > 4096u ||
                current_instructions > byte_max ||
                subsequent_instructions > byte_max)
                continue;

            CompositeCallbackBatchProof::Descriptor descriptor;
            descriptor.kind =
                CompositeCallbackBatchProof::Kind::FlagPollEqualImmediate;
            descriptor.call_block_address = call_block.start_address;
            descriptor.call_instruction_address = call.source_address;
            descriptor.continuation_address = continuation_address;
            descriptor.exit_address = exit_address;
            descriptor.kernel_address = callback->second.block->start_address;
            descriptor.source_load_instruction_address = load.source_address;
            descriptor.outer_branch_instruction_address = branch.source_address;
            descriptor.call_block_size =
                static_cast<std::uint32_t>(call_block.instructions.size() * 2u);
            descriptor.continuation_size =
                static_cast<std::uint32_t>(continuation.instructions.size() * 2u);
            descriptor.kernel_size = static_cast<std::uint32_t>(
                callback->second.block->instructions.size() * 2u);
            descriptor.callback_register = call.branch_register;
            descriptor.flag_base_register = load.source_register;
            descriptor.flag_value_register = load.destination_register;
            descriptor.flag_displacement = load.displacement;
            descriptor.flag_expected_value =
                static_cast<std::uint32_t>(compare.immediate);
            descriptor.current_round_instruction_count =
                static_cast<std::uint8_t>(current_instructions);
            descriptor.subsequent_round_instruction_count =
                static_cast<std::uint8_t>(subsequent_instructions);
            descriptor.call_block_guest_cycles = *call_cycles;
            descriptor.continuation_guest_cycles = *continuation_cycles;
            descriptor.kernel_guest_cycles = callback->second.guest_cycles;
            descriptor.current_round_guest_cycles = current_cycles;
            descriptor.subsequent_round_guest_cycles = subsequent_cycles;
            result.push_back({descriptor});
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.descriptor.call_instruction_address,
                          left.descriptor.kernel_address} <
               std::tuple{right.descriptor.call_instruction_address,
                          right.descriptor.kernel_address};
    });
    result.erase(std::unique(result.begin(),
                             result.end(),
                             [](const auto& left, const auto& right) {
                                 return left.descriptor.call_instruction_address ==
                                            right.descriptor.call_instruction_address &&
                                        left.descriptor.kernel_address ==
                                            right.descriptor.kernel_address;
                             }),
                 result.end());
    return result;
}

std::vector<CountedLoopBatchProof>
counted_loop_batch_proofs(const std::span<const katana::ir::Function> program) {
    using Operation = katana::ir::Operation;
    struct BlockOwner {
        const katana::ir::Function* function = nullptr;
        const katana::ir::BasicBlock* block = nullptr;
    };

    std::unordered_map<std::uint32_t, BlockOwner> blocks;
    std::unordered_set<std::uint32_t> duplicate_block_addresses;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> predecessors;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            if (!blocks.emplace(block.start_address, BlockOwner{&function, &block}).second)
                duplicate_block_addresses.insert(block.start_address);
            for (const auto successor : block.successors)
                predecessors[successor].push_back(block.start_address);
        }
    }

    const auto no_delay_slots = [](const katana::ir::BasicBlock& block) {
        return std::all_of(block.instructions.begin(), block.instructions.end(), [](const auto& i) {
            return i.delay_slot.role == katana::ir::DelaySlotRole::None;
        });
    };
    const auto exact_successors = [](const katana::ir::BasicBlock& block,
                                     std::initializer_list<std::uint32_t> expected) {
        auto actual = block.successors;
        auto wanted = std::vector<std::uint32_t>(expected);
        std::sort(actual.begin(), actual.end());
        std::sort(wanted.begin(), wanted.end());
        return actual == wanted;
    };
    const auto exact_contiguous_block = [](const katana::ir::BasicBlock& block,
                                           const std::size_t expected_instruction_count) {
        if (block.instructions.size() != expected_instruction_count ||
            expected_instruction_count == 0u ||
            expected_instruction_count > std::numeric_limits<std::uint32_t>::max() / 2u)
            return false;
        const auto byte_size = static_cast<std::uint64_t>(expected_instruction_count) * 2u;
        if (static_cast<std::uint64_t>(block.start_address) + byte_size > 0x1'0000'0000ull)
            return false;
        for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
            const auto expected_address =
                static_cast<std::uint64_t>(block.start_address) + index * 2u;
            if (block.instructions[index].source_address != expected_address) return false;
        }
        return true;
    };

    std::vector<CountedLoopBatchProof> result;
    for (const auto& function : program) {
        for (const auto& guard : function.blocks) {
            if (duplicate_block_addresses.contains(guard.start_address) ||
                !exact_contiguous_block(guard, 4u) || guard.has_indirect_successor ||
                !no_delay_slots(guard))
                continue;
            const auto& limit_load = guard.instructions[0];
            const auto& counter_load = guard.instructions[1];
            const auto& compare = guard.instructions[2];
            const auto& branch = guard.instructions[3];
            const bool signed_word_limit =
                limit_load.operation == Operation::LoadWordSignedPcRelative;
            if ((!signed_word_limit && limit_load.operation != Operation::LoadLongPcRelative) ||
                !limit_load.effective_address.has_value() ||
                limit_load.memory_effects.access != katana::ir::MemoryAccessKind::Read ||
                limit_load.memory_effects.width !=
                    (signed_word_limit ? katana::ir::OperandWidth::Bits16
                                       : katana::ir::OperandWidth::Bits32) ||
                limit_load.memory_effects.access_count != 1u ||
                limit_load.memory_effects.region != katana::ir::MemoryRegionKind::NormalRam ||
                counter_load.operation != Operation::LoadLongDisplacement ||
                counter_load.forwarded_value_register.has_value() ||
                counter_load.memory_effects.access != katana::ir::MemoryAccessKind::Read ||
                counter_load.memory_effects.width != katana::ir::OperandWidth::Bits32 ||
                counter_load.memory_effects.access_count != 1u ||
                counter_load.memory_effects.region == katana::ir::MemoryRegionKind::NormalRam ||
                compare.operation != Operation::CompareGreaterOrEqual ||
                compare.destination_register != counter_load.destination_register ||
                compare.source_register != limit_load.destination_register ||
                branch.operation != Operation::BranchIfFalse || !branch.target_address.has_value())
                continue;

            const auto increment_found = blocks.find(*branch.target_address);
            if (increment_found == blocks.end() ||
                duplicate_block_addresses.contains(*branch.target_address) ||
                increment_found->second.function != &function)
                continue;
            const auto& increment = *increment_found->second.block;
            if (!exact_contiguous_block(increment, 3u) || increment.has_indirect_successor ||
                !no_delay_slots(increment))
                continue;
            const auto& increment_load = increment.instructions[0];
            const auto& add = increment.instructions[1];
            const auto& store = increment.instructions[2];
            if (increment_load.operation != Operation::LoadLongDisplacement ||
                increment_load.forwarded_value_register.has_value() ||
                increment_load.memory_effects.access != katana::ir::MemoryAccessKind::Read ||
                increment_load.memory_effects.width != katana::ir::OperandWidth::Bits32 ||
                increment_load.memory_effects.access_count != 1u ||
                increment_load.memory_effects.region ==
                    katana::ir::MemoryRegionKind::NormalRam ||
                add.operation != Operation::AddImmediate || add.immediate <= 0 ||
                add.immediate > 127 ||
                add.destination_register != increment_load.destination_register ||
                store.operation != Operation::StoreLongDisplacement ||
                store.memory_effects.access != katana::ir::MemoryAccessKind::Write ||
                store.memory_effects.width != katana::ir::OperandWidth::Bits32 ||
                store.memory_effects.access_count != 1u ||
                store.memory_effects.region == katana::ir::MemoryRegionKind::NormalRam ||
                store.destination_register != increment_load.source_register ||
                store.source_register != increment_load.destination_register ||
                store.displacement != increment_load.displacement ||
                counter_load.source_register != increment_load.source_register ||
                counter_load.displacement != increment_load.displacement)
                continue;

            if (branch.source_address > std::numeric_limits<std::uint32_t>::max() - 2u) continue;
            const auto exit_address = branch.source_address + 2u;
            if (!exact_successors(guard, {increment.start_address, exit_address}) ||
                !exact_successors(increment, {guard.start_address}))
                continue;
            const auto incoming = predecessors.find(increment.start_address);
            if (incoming == predecessors.end() || incoming->second.size() != 1u ||
                incoming->second.front() != guard.start_address)
                continue;

            const auto base_register = increment_load.source_register;
            const auto limit_register = limit_load.destination_register;
            const auto compare_register = counter_load.destination_register;
            const auto increment_register = increment_load.destination_register;
            const std::array registers{
                base_register, limit_register, compare_register, increment_register};
            auto unique_registers = registers;
            std::sort(unique_registers.begin(), unique_registers.end());
            if (std::adjacent_find(unique_registers.begin(), unique_registers.end()) !=
                unique_registers.end())
                continue;

            std::uint64_t guard_cycles = 0u;
            std::uint64_t increment_cycles = 0u;
            for (const auto& instruction : guard.instructions)
                guard_cycles +=
                    katana::sh4::instruction_timing(instruction.original_opcode).guest_cycles;
            for (const auto& instruction : increment.instructions)
                increment_cycles +=
                    katana::sh4::instruction_timing(instruction.original_opcode).guest_cycles;
            const auto limit_timing =
                katana::sh4::instruction_timing(limit_load.original_opcode);
            const auto first_counter_read_timing =
                katana::sh4::instruction_timing(counter_load.original_opcode);
            const auto increment_counter_read_timing =
                katana::sh4::instruction_timing(increment_load.original_opcode);
            const auto store_timing =
                katana::sh4::instruction_timing(store.original_opcode);
            const auto round_cycles = guard_cycles + increment_cycles;
            const auto round_instruction_count =
                guard.instructions.size() + increment.instructions.size();
            constexpr auto byte_max =
                static_cast<std::uint64_t>(std::numeric_limits<std::uint8_t>::max());
            const auto increment_size =
                static_cast<std::uint32_t>(increment.instructions.size() * 2u);
            if (!limit_timing.requires_cycle_flush ||
                !first_counter_read_timing.requires_cycle_flush ||
                !increment_counter_read_timing.requires_cycle_flush ||
                !store_timing.requires_cycle_flush ||
                limit_timing.guest_cycles == 0u ||
                limit_timing.guest_cycles > byte_max ||
                first_counter_read_timing.guest_cycles == 0u ||
                first_counter_read_timing.guest_cycles > byte_max ||
                store_timing.guest_cycles == 0u ||
                store_timing.guest_cycles > byte_max ||
                guard_cycles <=
                    limit_timing.guest_cycles +
                        first_counter_read_timing.guest_cycles ||
                guard_cycles >= round_cycles || round_cycles > 4096u ||
                increment_cycles <= store_timing.guest_cycles ||
                round_instruction_count == 0u || round_instruction_count > byte_max ||
                guard.instructions.size() > byte_max ||
                increment_size == 0u)
                continue;

            CountedLoopBatchProof::Descriptor descriptor;
            descriptor.guard_address = guard.start_address;
            descriptor.increment_address = increment.start_address;
            descriptor.increment_size = increment_size;
            descriptor.limit_address = *limit_load.effective_address;
            descriptor.first_counter_read_instruction_address =
                counter_load.source_address;
            descriptor.store_instruction_address = store.source_address;
            descriptor.pre_store_instruction_address = add.source_address;
            descriptor.counter_displacement = increment_load.displacement;
            descriptor.counter_base_register = base_register;
            descriptor.limit_register = limit_register;
            descriptor.compare_register = compare_register;
            descriptor.increment_register = increment_register;
            descriptor.step = static_cast<std::uint8_t>(add.immediate);
            descriptor.limit_width = signed_word_limit ? 2u : 4u;
            descriptor.guard_instruction_count =
                static_cast<std::uint8_t>(guard.instructions.size());
            descriptor.round_instruction_count = static_cast<std::uint8_t>(round_instruction_count);
            descriptor.prefix_guest_cycles =
                static_cast<std::uint8_t>(limit_timing.guest_cycles);
            descriptor.first_counter_read_guest_cycles =
                static_cast<std::uint8_t>(
                    first_counter_read_timing.guest_cycles);
            descriptor.store_guest_cycles =
                static_cast<std::uint8_t>(store_timing.guest_cycles);
            descriptor.signed_word_limit = signed_word_limit;
            descriptor.guard_guest_cycles = guard_cycles;
            descriptor.round_guest_cycles = round_cycles;
            result.push_back({descriptor});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.descriptor.guard_address < right.descriptor.guard_address;
    });
    return result;
}

std::vector<MmioWaitLoopBatchProof> mmio_wait_loop_batch_proofs(
    const std::span<const katana::ir::Function> program,
    const katana::analysis::DreamcastHardwareAudit& audit) {
    using AccessKind = katana::analysis::HardwareAccessKind;
    using Classification = katana::analysis::HardwareLoopClassification;
    using Operation = katana::ir::Operation;
    using Region = katana::analysis::DreamcastHardwareRegion;

    std::unordered_map<std::uint32_t, const katana::ir::BasicBlock*> blocks;
    std::unordered_set<std::uint32_t> duplicate_block_addresses;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            if (!blocks.emplace(block.start_address, &block).second)
                duplicate_block_addresses.insert(block.start_address);
        }
    }

    std::vector<MmioWaitLoopBatchProof> result;
    for (const auto& loop : audit.loops) {
        const bool statically_resolved_pointer =
            loop.classification == Classification::MmioPoll;
        const bool runtime_resolved_pointer =
            loop.classification == Classification::Unknown &&
            loop.unresolved_guard_access;
        if ((!statically_resolved_pointer && !runtime_resolved_pointer) ||
            loop.header_address != loop.latch_address ||
            loop.block_addresses != std::vector<std::uint32_t>{loop.header_address} ||
            duplicate_block_addresses.contains(loop.header_address))
            continue;
        const auto block_found = blocks.find(loop.header_address);
        if (block_found == blocks.end()) continue;
        const auto& block = *block_found->second;
        if (block.start_address > std::numeric_limits<std::uint32_t>::max() - 8u) continue;
        const bool pointer_from_register =
            runtime_resolved_pointer && block.instructions.size() == 3u;
        if ((!pointer_from_register &&
             (!statically_resolved_pointer || block.instructions.size() != 4u)) ||
            block.has_indirect_successor)
            continue;
        bool contiguous = true;
        for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
            contiguous =
                contiguous &&
                block.instructions[index].source_address ==
                    static_cast<std::uint32_t>(block.start_address + index * 2u) &&
                block.instructions[index].delay_slot.role ==
                    katana::ir::DelaySlotRole::None;
        }
        if (!contiguous) continue;

        const auto read_index = pointer_from_register ? std::size_t{0u} : std::size_t{1u};
        const auto test_index = read_index + 1u;
        const auto branch_index = test_index + 1u;
        const auto* pointer_load =
            pointer_from_register ? nullptr : &block.instructions.front();
        const auto& mmio_read = block.instructions[read_index];
        const auto& test = block.instructions[test_index];
        const auto& branch = block.instructions[branch_index];
        const auto block_size =
            static_cast<std::uint32_t>(block.instructions.size() * 2u);
        const auto exit_address =
            static_cast<std::uint32_t>(block.start_address + block_size);
        auto successors = block.successors;
        std::sort(successors.begin(), successors.end());
        auto expected_successors =
            std::vector<std::uint32_t>{block.start_address, exit_address};
        std::sort(expected_successors.begin(), expected_successors.end());
        const bool immediate_mask = test.operation == Operation::TestImmediate;
        const bool register_mask = test.operation == Operation::TestRegister;
        if (mmio_read.operation != Operation::LoadLong ||
            mmio_read.memory_effects.access != katana::ir::MemoryAccessKind::Read ||
            mmio_read.memory_effects.width != katana::ir::OperandWidth::Bits32 ||
            mmio_read.memory_effects.access_count != 1u ||
            (!pointer_from_register &&
             mmio_read.memory_effects.region == katana::ir::MemoryRegionKind::NormalRam) ||
            (!immediate_mask && !register_mask) ||
            test.destination_register != mmio_read.destination_register ||
            (immediate_mask && (test.immediate <= 0 || test.immediate > 0xFF)) ||
            (register_mask &&
             (test.source_register == mmio_read.destination_register ||
              test.source_register == mmio_read.source_register)) ||
            (branch.operation != Operation::BranchIfTrue &&
             branch.operation != Operation::BranchIfFalse) ||
            !branch.target_address || *branch.target_address != block.start_address ||
            successors != expected_successors)
            continue;

        if (pointer_from_register) {
            if (mmio_read.source_register == mmio_read.destination_register ||
                loop.unresolved_guard_read_instruction_addresses !=
                    std::vector<std::uint32_t>{mmio_read.source_address} ||
                !loop.accesses.empty())
                continue;
        } else if (pointer_load->operation != Operation::LoadLongPcRelative ||
                   !pointer_load->effective_address ||
                   pointer_load->memory_effects.access !=
                       katana::ir::MemoryAccessKind::Read ||
                   pointer_load->memory_effects.width !=
                       katana::ir::OperandWidth::Bits32 ||
                   pointer_load->memory_effects.access_count != 1u ||
                   pointer_load->memory_effects.region !=
                       katana::ir::MemoryRegionKind::NormalRam ||
                   mmio_read.source_register != pointer_load->destination_register) {
            continue;
        }

        const katana::analysis::HardwareLoopAccessEvidence* proven_read = nullptr;
        bool invalid_access = false;
        if (!pointer_from_register) {
            for (const auto& access : loop.accesses) {
                if (access.instruction_address == mmio_read.source_address) {
                    if (proven_read != nullptr || !access.guards_loop ||
                        access.kind != AccessKind::Read || access.width != 4u ||
                        access.region != Region::SystemAsic || !access.aperture_mapped ||
                        access.canonical_address !=
                            katana::runtime::system_asic_physical_base ||
                        access.runtime_support !=
                            katana::analysis::HardwareRuntimeSupport::Implemented) {
                        invalid_access = true;
                        break;
                    }
                    proven_read = &access;
                } else if (access.instruction_address != pointer_load->source_address ||
                           access.kind != AccessKind::Read || access.width != 4u ||
                           !access.linear_memory) {
                    invalid_access = true;
                    break;
                }
            }
            if (invalid_access || proven_read == nullptr) continue;
        }

        if (!pointer_from_register) {
            const auto literal_physical = katana::runtime::canonical_physical_address(
                *pointer_load->effective_address);
            if (literal_physical <
                    katana::runtime::dreamcast_main_ram_area_bases.front() ||
                literal_physical >
                    katana::runtime::dreamcast_main_ram_area_bases.front() +
                        katana::runtime::dreamcast_main_ram_size - 4u ||
                katana::runtime::canonical_physical_address(
                    proven_read->guest_address) != proven_read->canonical_address)
                continue;
        }

        std::uint64_t round_guest_cycles = 0u;
        for (const auto& instruction : block.instructions)
            round_guest_cycles +=
                katana::sh4::instruction_timing(instruction.original_opcode).guest_cycles;
        const auto pre_read_guest_cycles = pointer_from_register
            ? std::uint64_t{0u}
            : katana::sh4::instruction_timing(pointer_load->original_opcode).guest_cycles;
        const auto read_guest_cycles =
            katana::sh4::instruction_timing(mmio_read.original_opcode).guest_cycles;
        const auto test_guest_cycles =
            katana::sh4::instruction_timing(test.original_opcode).guest_cycles;
        const auto branch_guest_cycles =
            katana::sh4::instruction_timing(branch.original_opcode).guest_cycles;
        constexpr auto byte_max =
            static_cast<std::uint64_t>(std::numeric_limits<std::uint8_t>::max());
        if (round_guest_cycles == 0u || round_guest_cycles > 4096u ||
            (!pointer_from_register &&
             (pre_read_guest_cycles == 0u ||
              pre_read_guest_cycles >= round_guest_cycles)) ||
            read_guest_cycles == 0u || read_guest_cycles > byte_max ||
            test_guest_cycles == 0u || test_guest_cycles > byte_max ||
            branch_guest_cycles == 0u || branch_guest_cycles > byte_max ||
            !katana::sh4::instruction_timing(mmio_read.original_opcode).requires_cycle_flush)
            continue;

        MmioWaitLoopBatchProof::Descriptor descriptor;
        descriptor.loop_header = block.start_address;
        descriptor.read_site = mmio_read.source_address;
        descriptor.pointer_literal_address =
            pointer_from_register ? 0u : *pointer_load->effective_address;
        descriptor.mmio_guest_address =
            pointer_from_register ? 0u : proven_read->guest_address;
        descriptor.mmio_physical_address = pointer_from_register
            ? katana::runtime::system_asic_physical_base
            : proven_read->canonical_address;
        descriptor.backedge_instruction_address = branch.source_address;
        descriptor.pointer_register = pointer_from_register
            ? mmio_read.source_register
            : pointer_load->destination_register;
        descriptor.value_register = mmio_read.destination_register;
        descriptor.mask_register = register_mask ? test.source_register : 0xFFu;
        descriptor.round_instruction_count =
            static_cast<std::uint8_t>(block.instructions.size());
        descriptor.test_mask =
            immediate_mask ? static_cast<std::uint32_t>(test.immediate) : 0u;
        descriptor.pointer_from_register = pointer_from_register;
        descriptor.branch_on_true = branch.operation == Operation::BranchIfTrue;
        descriptor.round_guest_cycles = round_guest_cycles;
        descriptor.pre_read_guest_cycles = pre_read_guest_cycles;
        descriptor.read_guest_cycles = static_cast<std::uint8_t>(read_guest_cycles);
        descriptor.test_guest_cycles = static_cast<std::uint8_t>(test_guest_cycles);
        descriptor.branch_guest_cycles = static_cast<std::uint8_t>(branch_guest_cycles);
        result.push_back({descriptor});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.descriptor.loop_header < right.descriptor.loop_header;
    });
    return result;
}

std::string generated_header(const std::string& entry_namespace) {
    return "#pragma once\n\n"
           "#include \"katana/runtime/block_table.hpp\"\n"
           "#include \"katana/runtime/crash_capsule.hpp\"\n"
           "#include \"katana/runtime/disc.hpp\"\n"
           "#include \"katana/runtime/disc_load_transaction.hpp\"\n"
           "#include \"katana/runtime/executable_modules.hpp\"\n"
           "#include \"katana/runtime/platform_services.hpp\"\n"
           "#include \"katana/runtime/runtime.hpp\"\n"
           "#include \"katana/runtime/system_replay.hpp\"\n"
           "#include <array>\n#include <cstddef>\n#include <cstdint>\n"
           "#include <memory>\n#include <string>\n"
           "#include <string_view>\n\n"
           "namespace " +
           entry_namespace +
           " {\n"
           "void run(katana::runtime::CpuState& cpu);\n"
           "enum class StaticAotEscapeReason : std::uint8_t {\n"
           "    ProgramEntry,\n"
           "    CallUnknown,\n"
           "    TargetNotNativeEntrySafe,\n"
           "    TimingNotDeferrable,\n"
           "    SchedulerDue,\n"
           "    InterruptAcceptable,\n"
           "    MmioOrArchitectureBoundary,\n"
           "    Hook,\n"
           "    VariantOrGeneration,\n"
           "    NativeCallDepth,\n"
           "    PartitionOrSymbol,\n"
           "    GuestCycleQuantum,\n"
           "    GuestCycleBudget,\n"
           "    DynamicTarget,\n"
           "    ReturnBoundary,\n"
           "    Sleep,\n"
           "    ProductFastpath,\n"
           "    Unclassified,\n"
           "    Count,\n"
           "};\n"
           "inline constexpr std::size_t static_aot_escape_reason_count =\n"
           "    static_cast<std::size_t>(StaticAotEscapeReason::Count);\n"
           "struct StaticAotEscapeSiteCounter {\n"
           "    std::uint64_t site_id = 0u;\n"
           "    std::uint32_t guest_address = 0u;\n"
           "    StaticAotEscapeReason reason = StaticAotEscapeReason::Unclassified;\n"
           "    std::uint64_t count = 0u;\n"
           "};\n"
           "struct RuntimeRunResult {\n"
           "    std::uint64_t indirect_dispatches = 0u;\n"
           "    std::uint64_t runtime_dispatch_hits = 0u;\n"
           "    std::uint64_t runtime_dispatch_misses = 0u;\n"
           "    std::uint64_t runtime_dispatch_fallbacks = 0u;\n"
           "    std::uint64_t runtime_only_dispatch_hits = 0u;\n"
           "    std::uint64_t runtime_only_dispatch_misses = 0u;\n"
           "    std::uint64_t runtime_only_dispatch_fallbacks = 0u;\n"
           "    std::uint64_t runtime_only_sites = 0u;\n"
           "    std::uint64_t runtime_only_dispatch_share_ppm = 0u;\n"
           "    std::string runtime_only_profile_json;\n"
           "    std::uint32_t runtime_dispatch_first_error = 0u;\n"
           "    std::uint32_t final_pc = 0u;\n"
           "    std::uint64_t scheduler_cycle = 0u;\n"
           "    std::uint64_t central_dispatches = 0u;\n"
           "    bool guest_cycle_budget_reached = false;\n"
           "    std::uint32_t guest_cycle_contract = "
           "katana::runtime::guest_cycle_contract_version;\n"
           "    std::array<std::uint64_t, static_aot_escape_reason_count>\n"
           "        static_aot_escape_counts{};\n"
           "    std::array<StaticAotEscapeSiteCounter, 16u>\n"
           "        static_aot_escape_top_sites{};\n"
           "    std::uint32_t static_aot_escape_top_site_count = 0u;\n"
           "    std::uint64_t static_aot_classified_dispatches = 0u;\n"
           "};\n"
           "struct RuntimeMaterializationStatus {\n"
           "    std::uint64_t requests = 0u;\n"
           "    std::uint64_t cache_hits = 0u;\n"
           "    std::uint64_t materializations = 0u;\n"
           "    std::uint64_t interpreter_materializations = 0u;\n"
           "    std::uint64_t misses = 0u;\n"
           "    std::uint64_t budget_failures = 0u;\n"
           "    std::uint64_t retained_validation_bytes = 0u;\n"
           "    std::uint64_t peak_retained_validation_bytes = 0u;\n"
           "    std::uint64_t reclaimed_validation_bytes = 0u;\n"
           "    std::uint32_t first_failure = 0u;\n"
           "    std::uint32_t first_failure_target = 0u;\n"
           "};\n"
           "struct CountedLoopBatchDescriptor {\n"
           "    std::uint32_t guard_address = 0u;\n"
           "    std::uint32_t increment_address = 0u;\n"
           "    std::uint32_t increment_size = 0u;\n"
           "    std::uint32_t limit_address = 0u;\n"
           "    std::uint32_t first_counter_read_instruction_address = 0u;\n"
           "    std::uint32_t store_instruction_address = 0u;\n"
           "    std::uint32_t pre_store_instruction_address = 0u;\n"
           "    std::int32_t counter_displacement = 0;\n"
           "    std::uint8_t counter_base_register = 0u;\n"
           "    std::uint8_t limit_register = 0u;\n"
           "    std::uint8_t compare_register = 0u;\n"
           "    std::uint8_t increment_register = 0u;\n"
           "    std::uint8_t step = 0u;\n"
           "    std::uint8_t limit_width = 0u;\n"
           "    std::uint8_t guard_instruction_count = 0u;\n"
           "    std::uint8_t round_instruction_count = 0u;\n"
           "    std::uint8_t prefix_guest_cycles = 0u;\n"
           "    std::uint8_t first_counter_read_guest_cycles = 0u;\n"
           "    std::uint8_t store_guest_cycles = 0u;\n"
           "    bool signed_word_limit = false;\n"
           "    std::uint64_t guard_guest_cycles = 0u;\n"
           "    std::uint64_t round_guest_cycles = 0u;\n"
           "    std::string_view guard_provenance;\n"
           "    std::string_view increment_provenance;\n"
           "};\n"
           "struct MmioWaitLoopBatchDescriptor {\n"
           "    std::uint32_t loop_header = 0u;\n"
           "    std::uint32_t read_site = 0u;\n"
           "    std::uint32_t pointer_literal_address = 0u;\n"
           "    std::uint32_t mmio_guest_address = 0u;\n"
           "    std::uint32_t mmio_physical_address = 0u;\n"
           "    std::uint32_t backedge_instruction_address = 0u;\n"
           "    std::uint8_t pointer_register = 0u;\n"
           "    std::uint8_t value_register = 0u;\n"
           "    std::uint8_t mask_register = 0xFFu;\n"
           "    std::uint8_t round_instruction_count = 0u;\n"
           "    std::uint32_t test_mask = 0u;\n"
           "    bool pointer_from_register = false;\n"
           "    bool branch_on_true = false;\n"
           "    std::uint64_t round_guest_cycles = 0u;\n"
           "    std::uint64_t pre_read_guest_cycles = 0u;\n"
           "    std::uint8_t read_guest_cycles = 0u;\n"
           "    std::uint8_t test_guest_cycles = 0u;\n"
           "    std::uint8_t branch_guest_cycles = 0u;\n"
           "    std::string_view block_provenance;\n"
           "};\n"
           "struct MemoryFillLoopBatchDescriptor {\n"
           "    std::uint32_t guard_address = 0u;\n"
           "    std::uint32_t body_address = 0u;\n"
           "    std::uint32_t exit_address = 0u;\n"
           "    std::uint32_t store_instruction_address = 0u;\n"
           "    std::uint32_t increment_instruction_address = 0u;\n"
           "    std::uint32_t limit_load_instruction_address = 0u;\n"
           "    std::uint32_t compare_instruction_address = 0u;\n"
           "    std::uint32_t branch_instruction_address = 0u;\n"
           "    std::uint32_t guard_size = 0u;\n"
           "    std::uint32_t body_size = 0u;\n"
           "    std::uint8_t cursor_register = 0u;\n"
           "    std::uint8_t fill_register = 0u;\n"
           "    std::uint8_t limit_register = 0u;\n"
           "    std::uint8_t limit_pointer_register = 0u;\n"
           "    std::uint8_t store_width = 0u;\n"
           "    std::uint8_t step = 0u;\n"
           "    std::uint8_t round_instruction_count = 0u;\n"
           "    std::uint8_t guard_instruction_count = 0u;\n"
           "    std::uint8_t store_guest_cycles = 0u;\n"
           "    std::uint64_t guard_guest_cycles = 0u;\n"
           "    std::uint64_t round_guest_cycles = 0u;\n"
           "    std::string_view guard_provenance;\n"
           "    std::string_view body_provenance;\n"
           "};\n"
           "enum class CompositeCallbackBatchKind : std::uint8_t {\n"
           "    MemoryCopy,\n"
           "    FlagPollEqualImmediate,\n"
           "};\n"
           "struct CompositeCallbackBatchDescriptor {\n"
           "    CompositeCallbackBatchKind kind =\n"
           "        CompositeCallbackBatchKind::MemoryCopy;\n"
           "    std::uint32_t call_block_address = 0u;\n"
           "    std::uint32_t call_instruction_address = 0u;\n"
           "    std::uint32_t continuation_address = 0u;\n"
           "    std::uint32_t exit_address = 0u;\n"
           "    std::uint32_t kernel_address = 0u;\n"
           "    std::uint32_t kernel_return_address = 0u;\n"
           "    std::uint32_t source_load_instruction_address = 0u;\n"
           "    std::uint32_t target_store_instruction_address = 0u;\n"
           "    std::uint32_t outer_branch_instruction_address = 0u;\n"
           "    std::uint32_t call_block_size = 0u;\n"
           "    std::uint32_t continuation_size = 0u;\n"
           "    std::uint32_t kernel_size = 0u;\n"
           "    std::uint32_t kernel_return_size = 0u;\n"
           "    std::uint8_t callback_register = 0u;\n"
           "    std::uint8_t destination_register = 0u;\n"
           "    std::uint8_t count_register = 0u;\n"
           "    std::uint8_t limit_register = 0u;\n"
           "    std::uint8_t flag_base_register = 0u;\n"
           "    std::uint8_t flag_value_register = 0u;\n"
           "    std::int32_t flag_displacement = 0;\n"
           "    std::uint32_t flag_expected_value = 0u;\n"
           "    std::uint8_t current_round_instruction_count = 0u;\n"
           "    std::uint8_t subsequent_round_instruction_count = 0u;\n"
           "    std::uint64_t call_block_guest_cycles = 0u;\n"
           "    std::uint64_t continuation_guest_cycles = 0u;\n"
           "    std::uint64_t kernel_guest_cycles = 0u;\n"
           "    std::uint64_t kernel_return_guest_cycles = 0u;\n"
           "    std::uint64_t current_round_guest_cycles = 0u;\n"
           "    std::uint64_t subsequent_round_guest_cycles = 0u;\n"
           "    std::string_view call_block_provenance;\n"
           "    std::string_view continuation_provenance;\n"
           "    std::string_view kernel_provenance;\n"
           "    std::string_view kernel_return_provenance;\n"
           "};\n"
           "bool try_product_counted_loop_batch(\n"
           "    katana::runtime::CpuState&, katana::runtime::PlatformServices&,\n"
           "    const katana::runtime::ValidatedBlockExecution&,\n"
           "    const CountedLoopBatchDescriptor&);\n"
           "bool try_product_mmio_wait_loop_batch(\n"
           "    katana::runtime::CpuState&, katana::runtime::PlatformServices&,\n"
           "    const katana::runtime::ValidatedBlockExecution&,\n"
           "    const MmioWaitLoopBatchDescriptor&);\n"
           "bool try_product_memory_fill_loop_batch(\n"
           "    katana::runtime::CpuState&, katana::runtime::PlatformServices&,\n"
           "    const katana::runtime::ValidatedBlockExecution&,\n"
           "    const MemoryFillLoopBatchDescriptor&);\n"
           "bool try_product_composite_callback_batch(\n"
           "    katana::runtime::CpuState&, katana::runtime::PlatformServices&,\n"
           "    const katana::runtime::ValidatedBlockExecution&,\n"
           "    const CompositeCallbackBatchDescriptor&);\n"
           "const RuntimeMaterializationStatus& runtime_materialization_status() noexcept;\n"
           "std::uint64_t runtime_central_dispatch_count() noexcept;\n"
           "void register_latent_aot_modules(\n"
           "    std::string_view content_identity,\n"
           "    katana::runtime::ExecutableDiscLoadTransactionCoordinator& transactions);\n"
           "RuntimeRunResult run_runtime(katana::runtime::CpuState& cpu,\n"
           "                             katana::runtime::PlatformServices& services,\n"
           "                             katana::runtime::RuntimeBlockTable& table,\n"
           "                             katana::runtime::SystemReplayObservationSession& "
           "observations,\n"
           "                             katana::runtime::CrashCapsule& crash_capsule);\n"
           "}\n";
}

std::vector<katana::runtime::RuntimeWaitLoopDescriptor>
runtime_wait_loop_descriptors(const katana::analysis::DreamcastHardwareAudit& audit) {
    using Classification = katana::analysis::HardwareLoopClassification;
    using Evidence = katana::runtime::RuntimeWaitLoopEvidence;
    using Kind = katana::analysis::HardwareAccessKind;
    using Descriptor = katana::runtime::RuntimeWaitLoopDescriptor;

    std::vector<Descriptor> descriptors;
    for (const auto& loop : audit.loops) {
        if (loop.classification == Classification::Counter) continue;

        const auto add = [&](const std::uint32_t read_site, const Evidence evidence) {
            descriptors.push_back({loop.header_address, loop.latch_address, read_site, evidence});
        };
        for (const auto& access : loop.accesses) {
            if (access.kind == Kind::Read && access.guards_loop)
                add(access.instruction_address, Evidence::ProvenGuard);
        }
        for (const auto read_site : loop.unresolved_guard_read_instruction_addresses)
            add(read_site, Evidence::UnresolvedGuard);

        const bool conservative_candidates = loop.classification == Classification::RamPoll ||
                                             loop.classification == Classification::MmioPoll ||
                                             loop.classification == Classification::Mixed ||
                                             loop.classification == Classification::Unknown;
        if (!conservative_candidates) continue;
        for (const auto& access : loop.accesses) {
            if (access.kind == Kind::Read && !access.guards_loop)
                add(access.instruction_address, Evidence::ConservativeCandidate);
        }
    }

    const auto key = [](const Descriptor& descriptor) {
        return std::tuple{descriptor.loop_header,
                          descriptor.loop_latch,
                          descriptor.read_site,
                          static_cast<std::uint8_t>(descriptor.evidence)};
    };
    std::sort(descriptors.begin(), descriptors.end(), [&](const auto& left, const auto& right) {
        return key(left) < key(right);
    });
    descriptors.erase(std::unique(descriptors.begin(),
                                  descriptors.end(),
                                  [](const auto& left, const auto& right) {
                                      return left.loop_header == right.loop_header &&
                                             left.loop_latch == right.loop_latch &&
                                             left.read_site == right.read_site;
                                  }),
                      descriptors.end());
    return descriptors;
}

std::string runtime_wait_loop_descriptor_contract(
    const std::span<const katana::runtime::RuntimeWaitLoopDescriptor> descriptors) {
    const auto evidence_enumerator = [](const katana::runtime::RuntimeWaitLoopEvidence evidence) {
        using Evidence = katana::runtime::RuntimeWaitLoopEvidence;
        switch (evidence) {
        case Evidence::ProvenGuard:
            return "ProvenGuard";
        case Evidence::UnresolvedGuard:
            return "UnresolvedGuard";
        case Evidence::ConservativeCandidate:
            return "ConservativeCandidate";
        }
        throw std::logic_error("Unbekannte Wait-Loop-Evidenz.");
    };
    const auto address = [](const std::uint32_t value) {
        std::ostringstream output;
        output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value
               << 'u';
        return output.str();
    };

    std::ostringstream output;
    output << "constexpr std::array<katana::runtime::RuntimeWaitLoopDescriptor, "
           << descriptors.size() << "u> runtime_wait_loop_descriptors{{\n";
    for (const auto& descriptor : descriptors) {
        output << "    {" << address(descriptor.loop_header) << ", "
               << address(descriptor.loop_latch) << ", " << address(descriptor.read_site)
               << ", katana::runtime::RuntimeWaitLoopEvidence::"
               << evidence_enumerator(descriptor.evidence) << "},\n";
    }
    output << "}};\n";
    return output.str();
}

std::string handwritten_main(
    const std::string& entry_namespace,
    const bool hle_bios_abi,
    const bool direct_boot_executable,
    const bool diagnostic_partial,
    const std::span<const katana::io::InputProvenance> inputs,
    const std::span<const katana::runtime::RuntimeWaitLoopDescriptor> wait_loop_descriptors,
    const std::string_view project_identity,
    const std::string_view expected_content_identity,
    const std::string_view expected_boot_file_name,
    const std::string_view expected_boot_sha256,
    const std::string_view console_profile,
    const std::uint32_t boot_address,
    const std::size_t boot_size,
    const katana::runtime::DreamcastRuntimeBootConfig*
        game_project_boot_config,
    const katana::runtime::GameProjectDefinition* game_project) {
    std::ostringstream identity_contract;
    identity_contract
        << "struct ExpectedInput { std::string_view role; std::string_view sha256; };\n"
        << "constexpr ExpectedInput expected_inputs[]{\n";
    for (const auto& input : inputs) {
        if (input.role != "gdi-descriptor" && !input.role.starts_with("gdi-track-")) continue;
        identity_contract << "    {" << katana::io::quote_json(input.role) << ", "
                          << katana::io::quote_json(input.sha256) << "},\n";
    }
    identity_contract << "};\n"
                      << "constexpr bool diagnostic_partial_port = "
                      << (diagnostic_partial ? "true" : "false") << ";\n"
                      << "constexpr std::string_view expected_project_identity = "
                      << katana::io::quote_json(project_identity) << ";\n"
                      << "constexpr std::string_view expected_content_identity = "
                      << katana::io::quote_json(expected_content_identity) << ";\n"
                      << "constexpr std::string_view expected_boot_file_name = "
                      << katana::io::quote_json(expected_boot_file_name) << ";\n"
                      << "constexpr std::string_view expected_boot_sha256 = "
                      << katana::io::quote_json(expected_boot_sha256) << ";\n"
                      << "constexpr katana::runtime::GuestProgramRange "
                         "expected_guest_program_range{"
                      << boot_address << "u, " << boot_size << "u};\n"
                      << runtime_wait_loop_descriptor_contract(wait_loop_descriptors);
    const auto* game_entry_handoff_binding =
        game_project != nullptr &&
                game_project->game_entry_handoff.has_value()
            ? &*game_project->game_entry_handoff
            : nullptr;
    identity_contract
        << "constexpr bool expected_game_entry_handoff = "
        << (game_entry_handoff_binding != nullptr ? "true" : "false")
        << ";\n"
        << "constexpr std::string_view "
           "expected_game_project_runtime_definition_identity = "
        << katana::io::quote_json(
               game_project != nullptr
                   ? game_project_runtime_identity(*game_project)
                   : std::string{})
        << ";\n"
         << "constexpr std::string_view "
            "expected_game_project_export_definition_identity = "
        << katana::io::quote_json(
               game_project != nullptr
                   ? game_project_export_identity(*game_project)
                   : std::string{})
        << ";\n"
        << "constexpr auto required_product_milestone =\n"
           "    katana::runtime::RequiredProductMilestone::"
        << katana::runtime::required_product_milestone_name(
               game_project != nullptr
                   ? game_project->required_product_milestone
                   : katana::runtime::RequiredProductMilestone::
                         FirstVisibleGameFrame)
        << ";\n";
    if (game_entry_handoff_binding != nullptr) {
        const auto& binding = *game_entry_handoff_binding;
        identity_contract
            << "katana::runtime::GameEntryHandoffBinding "
               "expected_game_entry_handoff_binding() {\n"
               "    katana::runtime::GameEntryHandoffBinding binding;\n"
               "    binding.schema_version = "
            << binding.schema_version
            << "u;\n"
               "    binding.required_runtime_abi = "
            << binding.required_runtime_abi
            << "u;\n"
               "    binding.required_platform_state_contract = "
            << binding.required_platform_state_contract
            << "u;\n"
               "    binding.executable.content_identity = "
            << katana::io::quote_json(
                   binding.executable.content_identity)
            << ";\n"
               "    binding.executable.boot_file_name = "
            << katana::io::quote_json(
                   binding.executable.boot_file_name)
            << ";\n"
               "    binding.executable.boot_byte_identity = "
            << katana::io::quote_json(
                   binding.executable.boot_byte_identity)
            << ";\n"
               "    binding.console_profile = "
               "katana::runtime::DreamcastConsoleProfile::"
            << console_profile_enumerator(console_profile)
            << ";\n"
               "    binding.descriptor_identity = "
            << katana::io::quote_json(binding.descriptor_identity)
            << ";\n"
               "    return binding;\n"
               "}\n";
    }
    std::ostringstream boot_configuration;
    boot_configuration
        << "        katana::runtime::DreamcastRuntimeBootConfig "
           "runtime_boot_config;\n";
    if (game_project_boot_config != nullptr) {
        const auto& config = *game_project_boot_config;
        boot_configuration
            << "        runtime_boot_config.firmware_mode =\n"
               "            katana::runtime::DreamcastRuntimeFirmwareMode::"
            << (config.firmware_mode ==
                        katana::runtime::DreamcastRuntimeFirmwareMode::
                            HleBiosAbi
                    ? "HleBiosAbi"
                    : "Direct")
            << ";\n"
               "        runtime_boot_config.boot_path =\n"
               "            katana::runtime::DreamcastRuntimeBootPath::"
            << (config.boot_path ==
                        katana::runtime::DreamcastRuntimeBootPath::
                            DirectBootExecutable
                    ? "DirectBootExecutable"
                    : "NativeDiscBoot")
            << ";\n"
               "        runtime_boot_config.post_bios_platform_contract_version = "
            << config.post_bios_platform_contract_version
            << "u;\n"
               "        runtime_boot_config.post_bios_cpu_state = {\n"
            << "            "
            << config.post_bios_cpu_state.contract_version << "u,\n"
            << "            " << config.post_bios_cpu_state.entry_point
            << "u,\n"
            << "            " << config.post_bios_cpu_state.stack_pointer
            << "u,\n"
            << "            " << config.post_bios_cpu_state.vector_base
            << "u,\n"
            << "            " << config.post_bios_cpu_state.status << "u,\n"
            << "            " << config.post_bios_cpu_state.fpscr << "u,\n"
            << "            " << config.post_bios_cpu_state.gbr << "u,\n"
            << "            " << config.post_bios_cpu_state.ssr << "u,\n"
            << "            " << config.post_bios_cpu_state.spc << "u,\n"
            << "            " << config.post_bios_cpu_state.sgr << "u,\n"
            << "            " << config.post_bios_cpu_state.dbr << "u,\n"
            << "            " << config.post_bios_cpu_state.pr << "u};\n";
    } else {
        boot_configuration
            << "        runtime_boot_config.firmware_mode =\n"
               "            katana::runtime::DreamcastRuntimeFirmwareMode::"
            << (hle_bios_abi ? "HleBiosAbi" : "Direct")
            << ";\n"
               "        runtime_boot_config.boot_path =\n"
               "            katana::runtime::DreamcastRuntimeBootPath::"
            << (direct_boot_executable || !hle_bios_abi
                    ? "DirectBootExecutable"
                    : "NativeDiscBoot")
            << ";\n";
    }
    const bool local_game_entry_project_supported =
        game_project != nullptr &&
        game_project->game_entry_handoff.has_value() &&
        !game_project_requires_external_runtime_definition(*game_project);
    std::ostringstream product_game_entry_handoff;
    if (game_entry_handoff_binding != nullptr) {
        product_game_entry_handoff
            << "        if (runtime_boot_config.boot_path !=\n"
               "                katana::runtime::DreamcastRuntimeBootPath::"
               "DirectBootExecutable)\n"
               "            throw std::runtime_error(\n"
               "                \"game-entry-handoff-product-requires-direct-boot\");\n"
               "        std::shared_ptr<\n"
               "            katana::runtime::GameEntryHandoffArtifact>\n"
               "            local_product_handoff_artifact;\n"
               "        std::optional<\n"
               "            katana::runtime::GameProjectRegistration>\n"
               "            local_game_project_registration;\n"
               "        const auto* product_handoff_value =\n"
               "            std::getenv(\"KATANA_GAME_ENTRY_HANDOFF_PRODUCT\");\n"
               "        const bool product_handoff_requested =\n"
               "            product_handoff_value != nullptr &&\n"
               "            *product_handoff_value != '\\0';\n"
               "        const auto* registered_game_project =\n"
               "            katana::runtime::active_game_project_bindings();\n";
        if (local_game_entry_project_supported) {
            const auto& definition = *game_project;
            product_game_entry_handoff
                << "        if (registered_game_project == nullptr) {\n"
                   "            if (!product_handoff_requested)\n"
                   "                throw std::runtime_error(\n"
                   "                    \"game-entry-handoff-product-artifact-required\");\n"
                   "            local_product_handoff_artifact =\n"
                   "                katana::runtime::GameEntryHandoffArtifact::load(\n"
                   "                    std::filesystem::path(product_handoff_value));\n"
                   "            const auto expected_handoff_binding =\n"
                   "                expected_game_entry_handoff_binding();\n"
                   "            if (local_product_handoff_artifact->descriptor().binding !=\n"
                   "                    expected_handoff_binding ||\n"
                   "                local_product_handoff_artifact->descriptor().completeness !=\n"
                   "                    katana::runtime::GameEntryHandoffCompleteness::\n"
                   "                        CompletePlatform)\n"
                   "                throw std::runtime_error(\n"
                   "                    \"game-entry-handoff-product-artifact-mismatch\");\n"
                   "            katana::runtime::GameProjectDefinition local_definition;\n"
                   "            local_definition.contract_version = "
                << definition.contract_version
                << "u;\n"
                   "            local_definition.project_id = "
                << katana::io::quote_json(definition.project_id)
                << ";\n"
                   "            local_definition.project_version = "
                << katana::io::quote_json(definition.project_version)
                << ";\n"
                   "            local_definition.identity = {\n"
                   "                "
                << katana::io::quote_json(
                       definition.identity.content_identity)
                << ",\n"
                   "                "
                << katana::io::quote_json(
                       definition.identity.boot_file_name)
                << ",\n"
                   "                "
                << katana::io::quote_json(
                       definition.identity.boot_byte_identity)
                 << "};\n"
                 << "            local_definition.required_product_milestone =\n"
                    "                required_product_milestone;\n"
                 << (definition.boot_config.has_value()
                        ? "            local_definition.boot_config = "
                          "runtime_boot_config;\n"
                        : "")
                << "            local_definition.game_entry_handoff =\n"
                   "                expected_handoff_binding;\n"
                   "            if (katana::runtime::game_project_definition_identity(\n"
                   "                    local_definition) !=\n"
                   "                expected_game_project_runtime_definition_identity)\n"
                   "                throw std::runtime_error(\n"
                   "                    \"game-entry-handoff-local-project-identity-mismatch\");\n"
                   "            katana::runtime::GameProjectRuntimeProviders providers;\n"
                   "            providers.game_entry_handoff =\n"
                   "                local_product_handoff_artifact->provider();\n"
                   "            local_game_project_registration.emplace(\n"
                   "                std::move(local_definition), std::move(providers));\n"
                   "            registered_game_project =\n"
                   "                &local_game_project_registration->bindings();\n"
                   "        } else if (product_handoff_requested) {\n"
                   "            throw std::runtime_error(\n"
                   "                \"game-entry-handoff-product-registration-conflict\");\n"
                   "        }\n";
        } else {
            product_game_entry_handoff
                << "        if (registered_game_project == nullptr)\n"
                   "            throw std::runtime_error(\n"
                   "                \"game-entry-handoff-game-project-not-registered\");\n"
                   "        if (product_handoff_requested)\n"
                   "            throw std::runtime_error(\n"
                   "                \"game-entry-handoff-product-artifact-not-supported\");\n";
        }
        product_game_entry_handoff
            <<
               "        const auto registered_game_project_identity =\n"
               "            katana::runtime::game_project_definition_identity(\n"
               "                registered_game_project->definition());\n"
               "        if (registered_game_project_identity !=\n"
               "                expected_game_project_runtime_definition_identity &&\n"
               "            registered_game_project_identity !=\n"
               "                expected_game_project_export_definition_identity)\n"
               "            throw std::runtime_error(\n"
               "                \"game-entry-handoff-game-project-identity-mismatch\");\n"
               "        const auto expected_handoff_binding =\n"
               "            expected_game_entry_handoff_binding();\n"
               "        const auto& registered_definition =\n"
               "            registered_game_project->definition();\n"
               "        if (!registered_definition.game_entry_handoff.has_value() ||\n"
               "            *registered_definition.game_entry_handoff !=\n"
               "                expected_handoff_binding)\n"
               "            throw std::runtime_error(\n"
               "                \"game-entry-handoff-binding-mismatch\");\n"
               "        const std::array allowed_game_entry_ranges{\n"
               "            katana::runtime::GameEntryCodeRange{\n"
               "                expected_guest_program_range.guest_start,\n"
               "                expected_guest_program_range.byte_size}};\n"
               "        katana::runtime::GameEntryHandoffRequest "
               "product_handoff_request;\n"
               "        product_handoff_request.expected_binding =\n"
               "            expected_handoff_binding;\n"
               "        product_handoff_request.allowed_entry_ranges =\n"
               "            allowed_game_entry_ranges;\n"
               "        product_handoff_request.memory_layout = {\n"
               "            static_cast<std::uint32_t>(state.main_ram->size()),\n"
               "            static_cast<std::uint32_t>(state.vram->size()),\n"
               "            static_cast<std::uint32_t>(state.aica_ram->size())};\n"
               "        product_handoff_request.required_devices =\n"
               "            katana::runtime::"
               "dreamcast_game_entry_required_devices_v2;\n"
               "        product_handoff_request.required_completeness =\n"
               "            katana::runtime::GameEntryHandoffCompleteness::"
               "CompletePlatform;\n"
               "        auto validated_product_handoff =\n"
               "            katana::runtime::validate_and_stage_game_entry_handoff(\n"
               "                product_handoff_request,\n"
               "                registered_game_project->"
               "game_entry_handoff_provider());\n"
               "        const auto applied_product_handoff =\n"
                "            katana::runtime::\n"
                "                apply_validated_game_entry_complete_platform_handoff(\n"
                "                    cpu, state, validated_product_handoff,\n"
                "                    katana::runtime::\n"
                "                        GameEntryCompletePlatformRestoreProfile::\n"
                "                            ProductHandoff);\n"
               "        std::cout\n"
               "            << \"KATANA_GAME_ENTRY_HANDOFF_APPLY status=complete-platform-applied\"\n"
               "            << \" operations=\"\n"
               "            << applied_product_handoff.memory_operations_applied\n"
               "            << \" bytes=\"\n"
               "            << applied_product_handoff.memory_bytes_applied\n"
               "            << \" devices=\"\n"
               "            << applied_product_handoff.devices_applied\n"
               "            << \" events=\"\n"
               "            << applied_product_handoff.scheduler_events_rehydrated\n"
               "            << \" source_guest_cycle=\"\n"
               "            << validated_product_handoff.scheduler().current_cycle\n"
               "            << \" runtime_guest_cycle=\"\n"
               "            << state.scheduler->current_cycle()\n"
               "            << \" platform_state=complete diagnostic=0\\n\";\n";
    }
    return "#include \"katana_port.hpp\"\n"
           "#include \"katana/runtime/block_guards.hpp\"\n"
           "#include \"katana/runtime/crash_capsule.hpp\"\n"
           "#include \"katana/runtime/dreamcast_boot.hpp\"\n"
           "#include \"katana/runtime/disc_install.hpp\"\n"
           "#include \"katana/runtime/exception.hpp\"\n"
           "#include \"katana/runtime/game_entry_handoff_artifact.hpp\"\n"
           "#include \"katana/runtime/game_project.hpp\"\n"
           "#include \"katana/runtime/host_input.hpp\"\n"
           "#include \"katana/runtime/host_runtime.hpp\"\n"
           "#include \"katana/runtime/host_video.hpp\"\n"
           "#include \"katana/runtime/guest_program_range.hpp\"\n"
           "#include \"katana/runtime/indirect_dispatch.hpp\"\n"
           "#include \"katana/runtime/packed_disc.hpp\"\n"
           "#include \"katana/runtime/runtime_probe.hpp\"\n"
           "#include \"katana/runtime/scheduler.hpp\"\n"
           "#include \"katana/runtime/scheduler_safepoint.hpp\"\n"
           "#include \"katana/runtime/system_replay.hpp\"\n"
           "#include \"katana/runtime/wait_loop_trace.hpp\"\n"
           "#include \"katana/io/input_provenance.hpp\"\n"
           "#include <algorithm>\n#include <array>\n#include <chrono>\n#include "
           "<cstdlib>\n#include "
           "<exception>\n#include <filesystem>\n#include <functional>\n#include "
           "<iostream>\n#include <limits>\n#include <memory>\n"
           "#include <optional>\n#include <span>\n#include <string>\n#include <string_view>\n"
           "#include <system_error>\n#include <thread>\n#include <unordered_map>\n#include "
           "<unordered_set>\n#include <vector>\n\n"
           "namespace {\n" +
           identity_contract.str() +
           "constexpr std::uint64_t product_gate_guest_cycle_checkpoint = "
           "600'000'000u;\n"
           "bool deterministic_runtime_probe_requested() {\n"
           "    const auto* value = std::getenv(\"KATANA_RUNTIME_PROBE\");\n"
           "    if (value == nullptr) return false;\n"
           "    if (std::string_view(value) == \"deterministic-v1\") return true;\n"
           "    throw std::invalid_argument(\"runtime-probe-profile-invalid\");\n"
           "}\n"
           "void validate_runtime_probe_environment(bool enabled) {\n"
           "    if (!enabled) return;\n"
           "    const auto* diagnostics = std::getenv(\"KATANA_PORT_DIAGNOSTICS\");\n"
           "    if (diagnostics == nullptr ||\n"
           "        (std::string_view(diagnostics) != \"0\" &&\n"
           "         std::string_view(diagnostics) != \"1\"))\n"
           "        throw std::invalid_argument(\"runtime-probe-diagnostics-invalid\");\n"
           "    if (!katana::runtime::guest_cycle_budget_from_environment().has_value())\n"
           "        throw std::invalid_argument(\"runtime-probe-budget-required\");\n"
           "    constexpr std::array forbidden{\n"
           "        \"KATANA_PORT_WAIT_LOOP_TRACE\", \"KATANA_PORT_DIAGNOSTICS_FULL\",\n"
           "        \"KATANA_PORT_PROGRESS_INTERVAL\", \"KATANA_PORT_LIFECYCLE_TEST\",\n"
           "        \"KATANA_PORT_BLOCK_LIMIT\", \"KATANA_PORT_IGNORE_FOCUS\",\n"
           "        \"KATANA_PORT_CONTROLLER_TEST\",\n"
           "        \"KATANA_PORT_MEMORY_PROBES\",\n"
           "        \"KATANA_GAME_ENTRY_HANDOFF_CAPTURE\",\n"
           "        \"KATANA_GAME_ENTRY_HANDOFF_APPLY_DIAGNOSTIC\"};\n"
           "    for (const auto* name : forbidden)\n"
           "        if (std::getenv(name) != nullptr)\n"
           "            throw std::invalid_argument(\"runtime-probe-environment-conflict\");\n"
           "}\n"
           "void validate_product_gate_environment(\n"
           "        const std::optional<std::uint64_t> guest_cycle_budget) {\n"
           "    if (guest_cycle_budget != product_gate_guest_cycle_checkpoint) return;\n"
           "    constexpr std::array forbidden{\n"
           "        \"KATANA_RUNTIME_PROBE\", \"KATANA_PORT_DIAGNOSTICS\",\n"
           "        \"KATANA_PORT_DIAGNOSTICS_FULL\", \"KATANA_PORT_WAIT_LOOP_TRACE\",\n"
           "        \"KATANA_PORT_PROGRESS_INTERVAL\", \"KATANA_PORT_LIFECYCLE_TEST\",\n"
           "        \"KATANA_PORT_BLOCK_LIMIT\", \"KATANA_PORT_IGNORE_FOCUS\",\n"
           "        \"KATANA_PORT_CONTROLLER_TEST\", \"KATANA_PORT_MEMORY_PROBES\",\n"
           "        \"KATANA_PORT_COUNTED_LOOP_TRACE\",\n"
           "        \"KATANA_PORT_COMPOSITE_CALLBACK_TRACE\",\n"
           "        \"KATANA_GAME_ENTRY_PROBE\",\n"
           "        \"KATANA_GAME_ENTRY_HANDOFF_CAPTURE\",\n"
           "        \"KATANA_GAME_ENTRY_HANDOFF_APPLY_DIAGNOSTIC\"};\n"
           "    for (const auto* name : forbidden)\n"
           "        if (std::getenv(name) != nullptr)\n"
           "            throw std::invalid_argument(\"product-gate-environment-conflict\");\n"
           "}\n"
           "void record_runtime_probe_event(\n"
           "        katana::runtime::SystemReplayLog& replay,\n"
           "        const katana::runtime::EventScheduler& scheduler,\n"
           "        katana::runtime::SystemReplayEventKind kind,\n"
           "        std::string code, std::uint64_t detail = 0u,\n"
           "        std::uint64_t auxiliary = 0u) noexcept {\n"
           "    katana::runtime::SystemReplayEvent event{\n"
           "        0u, scheduler.current_cycle(), kind, std::move(code),\n"
           "        std::nullopt, std::nullopt, detail, auxiliary, false,\n"
           "        scheduler.reset_generation()};\n"
           "    static_cast<void>(replay.try_record(std::move(event)));\n"
           "}\n"
           "class RuntimeProbeMmioTraceSession final {\n"
           "  public:\n"
           "    RuntimeProbeMmioTraceSession(\n"
           "            katana::runtime::Memory& memory,\n"
           "            katana::runtime::SystemReplayLog& replay,\n"
           "            const katana::runtime::EventScheduler& scheduler)\n"
           "        : memory_(&memory) {\n"
           "        if (memory_->has_mmio_trace_handler())\n"
           "            throw std::logic_error(\"runtime-probe-mmio-observer-conflict\");\n"
           "        memory_->set_mmio_trace_handler(\n"
           "            katana::runtime::system_replay_mmio_observer(\n"
           "            replay, [&scheduler] { return scheduler.current_cycle(); },\n"
           "            \"guest-mmio\", [&scheduler] { return scheduler.reset_generation(); }));\n"
           "        try {\n"
           "            replay.enable_coverage(static_cast<\n"
           "                katana::runtime::SystemReplayCoverageMask>(\n"
           "                    katana::runtime::SystemReplayCoverage::Mmio));\n"
           "        } catch (...) {\n"
           "            memory_->clear_mmio_trace_handler();\n"
           "            throw;\n"
           "        }\n"
           "    }\n"
           "    RuntimeProbeMmioTraceSession(const RuntimeProbeMmioTraceSession&) = delete;\n"
           "    RuntimeProbeMmioTraceSession& operator=(const RuntimeProbeMmioTraceSession&) = "
           "delete;\n"
           "    ~RuntimeProbeMmioTraceSession() noexcept { finish(); }\n"
           "    void finish() noexcept {\n"
           "        if (memory_ == nullptr) return;\n"
           "        memory_->clear_mmio_trace_handler();\n"
           "        memory_ = nullptr;\n"
           "    }\n"
           "  private:\n"
           "    katana::runtime::Memory* memory_ = nullptr;\n"
           "};\n"
           "class RuntimeWaitLoopTraceSession final {\n"
           "  public:\n"
           "    RuntimeWaitLoopTraceSession(katana::runtime::Memory& memory, bool enabled)\n"
           "        : memory_(&memory) {\n"
           "        if (!enabled || runtime_wait_loop_descriptors.empty()) return;\n"
           "        std::cerr << \"KATANA_WAIT_LOOP_TRACE_NOTICE local-only; contains raw "
           "guest-memory values; do not share without review\\n\";\n"
           "        recorder_.emplace(runtime_wait_loop_descriptors);\n"
           "        memory_->set_guest_memory_access_sink(recorder_->sink());\n"
           "    }\n"
           "    RuntimeWaitLoopTraceSession(const RuntimeWaitLoopTraceSession&) = delete;\n"
           "    RuntimeWaitLoopTraceSession& operator=(const RuntimeWaitLoopTraceSession&) = "
           "delete;\n"
           "    ~RuntimeWaitLoopTraceSession() noexcept { finish(); }\n"
           "    void finish() noexcept {\n"
           "        if (!recorder_) return;\n"
           "        memory_->clear_guest_memory_access_sink();\n"
           "        if (reported_) return;\n"
           "        reported_ = true;\n"
           "        try {\n"
           "            std::cerr << \"KATANA_WAIT_LOOP_TRACE \"\n"
           "                      << recorder_->serialize_json() << '\\n';\n"
           "        } catch (...) {\n"
           "            std::cerr << \"KATANA_WAIT_LOOP_TRACE "
           "{\\\"schema\\\":\\\"katana.runtime-wait-loop-trace\\\","
           "\\\"trace_version\\\":1,\\\"complete\\\":false,"
           "\\\"contains_raw_guest_values\\\":true,"
           "\\\"serialization_error\\\":true}\\n\";\n"
           "        }\n"
           "    }\n"
           "  private:\n"
           "    katana::runtime::Memory* memory_ = nullptr;\n"
           "    std::optional<katana::runtime::RuntimeWaitLoopTraceRecorder> recorder_;\n"
           "    bool reported_ = false;\n"
           "};\n"
           "void verify_boot_identity(\n"
           "        const katana::runtime::DreamcastRuntimeBootImage& boot) {\n"
           "    const std::string_view boot_bytes(\n"
           "        reinterpret_cast<const char*>(boot.boot_file.data()), boot.boot_file.size());\n"
           "    if ((!expected_boot_file_name.empty() &&\n"
           "         boot.boot_file_name != expected_boot_file_name) ||\n"
           "        katana::io::sha256_bytes(boot_bytes) != expected_boot_sha256)\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "void verify_source_identity(const std::filesystem::path& source,\n"
           "                            const katana::runtime::DreamcastRuntimeBootImage& boot) {\n"
           "    std::vector<katana::io::InputProvenance> actual;\n"
           "    actual.push_back(katana::io::capture_input_provenance(\"gdi-descriptor\", "
           "source));\n"
           "    const auto gdi = std::dynamic_pointer_cast<katana::runtime::GdiDiscSource>(\n"
           "        boot.source);\n"
           "    if (!gdi) throw std::runtime_error(\"source-identity-mismatch\");\n"
           "    for (const auto& track : gdi->descriptor().tracks)\n"
           "        actual.push_back(katana::io::capture_input_provenance(\n"
           "            \"gdi-track-\" + std::to_string(track.number), track.resolved_path));\n"
           "    if (actual.size() != std::size(expected_inputs))\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "    for (std::size_t index = 0u; index < actual.size(); ++index)\n"
           "        if (actual[index].role != expected_inputs[index].role ||\n"
           "            actual[index].sha256 != expected_inputs[index].sha256)\n"
           "            throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "void verify_pack_identity(const katana::runtime::PackedDiscSource& source) {\n"
           "    if (source.info().job_generation != expected_project_identity ||\n"
           "        source.info().content_identity != expected_content_identity)\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "void verify_recipe_identity(const katana::runtime::DiscInstallRecipe& recipe) {\n"
           "    if (recipe.job_generation != expected_project_identity ||\n"
           "        recipe.content_identity != expected_content_identity ||\n"
           "        recipe.boot_sha256 != expected_boot_sha256)\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "class ScopedCpuActiveBlockProvenance final {\n"
           "  public:\n"
           "    ScopedCpuActiveBlockProvenance(\n"
           "            katana::runtime::CpuState& cpu,\n"
           "            const std::uint32_t active_virtual_start,\n"
           "            const std::uint32_t active_physical_start,\n"
           "            const std::uint32_t active_size) noexcept\n"
           "        : cpu_(cpu), previous_virtual_start_(cpu.active_block_virtual_start),\n"
           "          previous_physical_start_(cpu.active_block_physical_start),\n"
           "          previous_size_(cpu.active_block_size) {\n"
           "        cpu_.active_block_virtual_start = active_virtual_start;\n"
           "        cpu_.active_block_physical_start = active_physical_start;\n"
           "        cpu_.active_block_size = active_size;\n"
           "    }\n"
           "    ~ScopedCpuActiveBlockProvenance() noexcept {\n"
           "        cpu_.active_block_virtual_start = previous_virtual_start_;\n"
           "        cpu_.active_block_physical_start = previous_physical_start_;\n"
           "        cpu_.active_block_size = previous_size_;\n"
           "    }\n"
           "    ScopedCpuActiveBlockProvenance(\n"
           "        const ScopedCpuActiveBlockProvenance&) = delete;\n"
           "    ScopedCpuActiveBlockProvenance& operator=(\n"
           "        const ScopedCpuActiveBlockProvenance&) = delete;\n"
           "  private:\n"
           "    katana::runtime::CpuState& cpu_;\n"
           "    std::uint32_t previous_virtual_start_ = 0u;\n"
           "    std::uint32_t previous_physical_start_ = 0u;\n"
           "    std::uint32_t previous_size_ = 0u;\n"
           "};\n"
           "struct ProvenMemoryTranslation {\n"
           "    std::uint32_t physical_address = 0u;\n"
           "    std::uint32_t raw_physical_address = 0u;\n"
           "    std::uint64_t mmu_generation = 0u;\n"
           "    std::uint8_t utlb_slot = 0xFFu;\n"
           "    bool no_mmu_fastpath = false;\n"
           "};\n"
           "bool counted_loop_batch_rejected(std::string_view stage);\n"
           "std::optional<ProvenMemoryTranslation> prove_contiguous_translation(\n"
           "        katana::runtime::CpuState& cpu,\n"
           "        const katana::runtime::DreamcastRuntimeState& state,\n"
           "        const std::uint32_t address, const std::size_t size,\n"
           "        const katana::runtime::TranslationAccess translation_access) {\n"
           "    if (!state.address_space || !cpu.address_space ||\n"
           "        cpu.address_space.get() != state.address_space.get() || size == 0u ||\n"
           "        size > 0x1'0000'0000ull - address) {\n"
           "        static_cast<void>(counted_loop_batch_rejected(\"translation-input\"));\n"
           "        return std::nullopt;\n"
           "    }\n"
           "    try {\n"
           "        const auto first = state.address_space->inspect_translation(\n"
           "            address, translation_access, cpu.privileged_mode());\n"
           "        const auto last_address = static_cast<std::uint32_t>(\n"
           "            static_cast<std::uint64_t>(address) + size - 1u);\n"
           "        const auto last = state.address_space->inspect_translation(\n"
           "            last_address, translation_access, cpu.privileged_mode());\n"
           "        const auto physical = katana::runtime::canonical_physical_address(\n"
           "            first.physical_address);\n"
           "        if (first.virtual_address != address || last.virtual_address != last_address) {\n"
           "            static_cast<void>(counted_loop_batch_rejected(\"translation-virtual\"));\n"
           "            return std::nullopt;\n"
           "        }\n"
           "        if (first.mmu_generation != last.mmu_generation) {\n"
           "            static_cast<void>(counted_loop_batch_rejected(\"translation-generation\"));\n"
           "            return std::nullopt;\n"
           "        }\n"
           "        if (first.no_mmu_fastpath != last.no_mmu_fastpath) {\n"
           "            static_cast<void>(counted_loop_batch_rejected(\"translation-path\"));\n"
           "            return std::nullopt;\n"
           "        }\n"
           "        if (first.utlb_slot != last.utlb_slot) {\n"
           "            static_cast<void>(counted_loop_batch_rejected(\"translation-slot\"));\n"
           "            return std::nullopt;\n"
           "        }\n"
           "        if (size > 0x1'0000'0000ull - physical ||\n"
           "            katana::runtime::canonical_physical_address(\n"
           "                last.physical_address) != physical + size - 1u) {\n"
           "            static_cast<void>(counted_loop_batch_rejected(\"translation-contiguous\"));\n"
           "            return std::nullopt;\n"
           "        }\n"
           "        return ProvenMemoryTranslation{\n"
           "            physical, first.physical_address, first.mmu_generation, first.utlb_slot,\n"
           "            first.no_mmu_fastpath};\n"
           "    } catch (...) {\n"
           "        static_cast<void>(counted_loop_batch_rejected(\"translation-exception\"));\n"
           "        return std::nullopt;\n"
           "    }\n"
           "}\n"
           "std::optional<ProvenMemoryTranslation> prove_main_ram_translation(\n"
           "        katana::runtime::CpuState& cpu,\n"
           "        const katana::runtime::DreamcastRuntimeState& state,\n"
           "        const std::uint32_t address, const std::size_t size,\n"
           "        const katana::runtime::TranslationAccess translation_access) {\n"
           "    const auto translated = prove_contiguous_translation(\n"
           "        cpu, state, address, size, translation_access);\n"
           "    if (!translated || !state.main_ram ||\n"
           "        !cpu.memory.maps_device(\n"
           "            translated->physical_address, size, state.main_ram.get(), false)) {\n"
           "        static_cast<void>(counted_loop_batch_rejected(\"ram-device\"));\n"
           "        return std::nullopt;\n"
           "    }\n"
           "    if (translation_access == katana::runtime::TranslationAccess::Write\n"
           "            ? !cpu.memory.is_writable_linear_range(\n"
           "                  translated->physical_address, size, false)\n"
           "            : !cpu.memory.is_readable_linear_range(\n"
           "                  translated->physical_address, size, false)) {\n"
           "        static_cast<void>(counted_loop_batch_rejected(\"ram-linear\"));\n"
           "        return std::nullopt;\n"
           "    }\n"
           "    return translated;\n"
           "}\n"
           "std::optional<std::uint32_t> dreamcast_main_ram_backing_offset(\n"
           "        const std::uint32_t address, const std::size_t size) noexcept {\n"
           "    if (size == 0u || size > katana::runtime::dreamcast_main_ram_size ||\n"
           "        size > 0x1'0000'0000ull - address)\n"
           "        return std::nullopt;\n"
           "    std::optional<std::uint32_t> result;\n"
           "    const auto access_end = static_cast<std::uint64_t>(address) + size;\n"
           "    for (const auto area_base :\n"
           "         katana::runtime::dreamcast_main_ram_area_bases) {\n"
           "        for (std::size_t mirror = 0u;\n"
           "             mirror < katana::runtime::dreamcast_main_ram_mirrors_per_area;\n"
           "             ++mirror) {\n"
           "            const auto alias_base = static_cast<std::uint64_t>(area_base) +\n"
           "                mirror * katana::runtime::dreamcast_main_ram_size;\n"
           "            const auto alias_end =\n"
           "                alias_base + katana::runtime::dreamcast_main_ram_size;\n"
           "            if (address < alias_base || access_end > alias_end) continue;\n"
           "            const auto candidate = static_cast<std::uint32_t>(\n"
           "                static_cast<std::uint64_t>(address) - alias_base);\n"
           "            if (candidate > katana::runtime::dreamcast_main_ram_size - size ||\n"
           "                (result && *result != candidate))\n"
           "                return std::nullopt;\n"
           "            result = candidate;\n"
           "        }\n"
           "    }\n"
           "    return result;\n"
           "}\n"
           "bool dreamcast_main_ram_backing_is_executable(\n"
           "        const katana::runtime::ExecutableCodeTracker& tracker,\n"
           "        const std::uint32_t backing_offset,\n"
           "        const std::size_t size) noexcept {\n"
           "    if (size == 0u || size > katana::runtime::dreamcast_main_ram_size ||\n"
           "        backing_offset > katana::runtime::dreamcast_main_ram_size - size)\n"
           "        return true;\n"
           "    for (const auto area_base :\n"
           "         katana::runtime::dreamcast_main_ram_area_bases) {\n"
           "        for (std::size_t mirror = 0u;\n"
           "             mirror < katana::runtime::dreamcast_main_ram_mirrors_per_area;\n"
           "             ++mirror) {\n"
           "            const auto alias = static_cast<std::uint64_t>(area_base) +\n"
           "                mirror * katana::runtime::dreamcast_main_ram_size +\n"
           "                backing_offset;\n"
           "            if (alias > std::numeric_limits<std::uint32_t>::max() ||\n"
           "                tracker.tracks_address(static_cast<std::uint32_t>(alias), size))\n"
           "                return true;\n"
           "        }\n"
           "    }\n"
           "    return false;\n"
           "}\n"
           "std::optional<ProvenMemoryTranslation> prove_on_chip_ram_translation(\n"
           "        katana::runtime::CpuState& cpu,\n"
           "        const katana::runtime::DreamcastRuntimeState& state,\n"
           "        const std::uint32_t address, const std::size_t size,\n"
           "        const katana::runtime::TranslationAccess translation_access) {\n"
           "    const auto translated = prove_contiguous_translation(\n"
           "        cpu, state, address, size, translation_access);\n"
           "    const auto device = state.cache_control\n"
           "        ? state.cache_control->on_chip_ram_device() : nullptr;\n"
           "    if (!translated || !state.cache_control || !device ||\n"
           "        (state.cache_control->value() &\n"
           "         katana::runtime::Sh4CacheControl::operand_ram_enable) == 0u ||\n"
           "        !translated->no_mmu_fastpath || translated->utlb_slot != 0xFFu ||\n"
           "        translated->raw_physical_address != address ||\n"
           "        translated->physical_address != address ||\n"
           "        translated->physical_address < katana::runtime::sh4_on_chip_ram_address ||\n"
           "        size > katana::runtime::sh4_on_chip_ram_aperture_size ||\n"
           "        translated->physical_address - katana::runtime::sh4_on_chip_ram_address >\n"
           "            katana::runtime::sh4_on_chip_ram_aperture_size - size ||\n"
           "        !cpu.memory.maps_device(\n"
           "            translated->physical_address, size, device.get(), false)) {\n"
           "        static_cast<void>(counted_loop_batch_rejected(\"ocram-proof\"));\n"
           "        return std::nullopt;\n"
           "    }\n"
           "    return translated;\n"
           "}\n"
           "bool counted_loop_batch_rejected(const std::string_view stage) {\n"
           "    static const bool enabled = [] {\n"
           "        const auto* value = std::getenv(\"KATANA_PORT_COUNTED_LOOP_TRACE\");\n"
           "        return value != nullptr && std::string_view(value) == \"1\";\n"
           "    }();\n"
           "    if (enabled) {\n"
           "        static std::unordered_set<std::string> reported;\n"
           "        if (reported.insert(std::string(stage)).second)\n"
           "            std::cerr << \"KATANA_COUNTED_LOOP_REJECT stage=\" << stage << '\\n';\n"
           "    }\n"
           "    return false;\n"
           "}\n"
           "void trace_counted_loop_batch_admission(const std::uint64_t admitted) {\n"
           "    static const bool enabled = [] {\n"
           "        const auto* value = std::getenv(\"KATANA_PORT_COUNTED_LOOP_TRACE\");\n"
           "        return value != nullptr && std::string_view(value) == \"1\";\n"
           "    }();\n"
           "    static bool reported = false;\n"
           "    if (enabled && !reported) {\n"
           "        reported = true;\n"
           "        std::cerr << \"KATANA_COUNTED_LOOP_ADMIT iterations=\" << admitted << '\\n';\n"
           "    }\n"
           "}\n"
           "void trace_memory_fill_batch_admission(const std::uint64_t admitted) {\n"
           "    static const bool enabled = [] {\n"
           "        const auto* value = std::getenv(\"KATANA_PORT_MEMORY_FILL_TRACE\");\n"
           "        return value != nullptr && std::string_view(value) == \"1\";\n"
           "    }();\n"
           "    static bool reported = false;\n"
           "    if (enabled && !reported) {\n"
           "        reported = true;\n"
           "        std::cerr << \"KATANA_MEMORY_FILL_ADMIT iterations=\" << admitted << '\\n';\n"
           "    }\n"
           "}\n"
           "bool composite_callback_batch_rejected(const std::string_view stage) {\n"
           "    static const bool enabled = [] {\n"
           "        const auto* value = std::getenv(\n"
           "            \"KATANA_PORT_COMPOSITE_CALLBACK_TRACE\");\n"
           "        return value != nullptr && std::string_view(value) == \"1\";\n"
           "    }();\n"
           "    if (enabled) {\n"
           "        static std::unordered_set<std::string> reported;\n"
           "        if (reported.insert(std::string(stage)).second)\n"
           "            std::cerr << \"KATANA_COMPOSITE_CALLBACK_REJECT stage=\"\n"
           "                      << stage << '\\n';\n"
           "    }\n"
           "    return false;\n"
           "}\n"
           "void trace_composite_callback_batch_admission(\n"
           "        const std::uint64_t admitted) {\n"
           "    static const bool enabled = [] {\n"
           "        const auto* value = std::getenv(\n"
           "            \"KATANA_PORT_COMPOSITE_CALLBACK_TRACE\");\n"
           "        return value != nullptr && std::string_view(value) == \"1\";\n"
           "    }();\n"
           "    static bool reported = false;\n"
           "    if (enabled && !reported) {\n"
           "        reported = true;\n"
           "        std::cerr << \"KATANA_COMPOSITE_CALLBACK_ADMIT iterations=\"\n"
           "                  << admitted << '\\n';\n"
           "    }\n"
           "}\n"
           "struct ProductTerminalTelemetry {\n"
           "    std::uint64_t restored_guest_cycle = 0u;\n"
           "    std::uint64_t start_guest_cycle = 0u;\n"
           "    std::uint64_t final_guest_cycle = 0u;\n"
           "    std::uint64_t requested_post_entry_cycles = 0u;\n"
           "    std::uint64_t target_guest_cycle = 0u;\n"
           "    std::uint64_t central_dispatch_baseline = 0u;\n"
           "    std::uint32_t milestone_bits = 0u;\n"
           "    double post_entry_host_seconds = 0.0;\n"
           "    bool guest_program_started = false;\n"
           "    bool product_budget_arm_failed = false;\n"
           "    bool terminal_summary_emitted = false;\n"
           "};\n"
           "class PortPlatformServices final : public katana::runtime::PlatformServices {\n"
           "  private:\n"
           "    enum class AtomicBatchCommitKind : std::uint8_t {\n"
           "        None, CountedLoop, MemoryFill, CompositeCallback\n"
           "    };\n"
           "    enum class FlagPollBatchRejectionStage : std::uint8_t {\n"
           "        Descriptor,\n"
           "        RuntimeState,\n"
           "        NotAtCleanSafepoint,\n"
           "        InterruptPending,\n"
           "        LiveTarget,\n"
           "        CandidateProvenance,\n"
           "        RegisteredBlocks,\n"
           "        InstructionMapping,\n"
           "        EntryRegisters,\n"
           "        Alignment,\n"
           "        RamTranslation,\n"
           "        RamBacking,\n"
           "        SchedulerEventDue,\n"
           "        SchedulerBudgetEmpty,\n"
           "        EmptyBatch,\n"
           "        AccountingOverflow,\n"
           "        CpuCounterOverflow,\n"
           "        MemoryAccounting,\n"
           "        Count,\n"
           "    };\n"
           "    struct FlagPollBatchCounters {\n"
           "        std::uint64_t attempts = 0u;\n"
           "        std::uint64_t admissions = 0u;\n"
           "        std::array<std::uint64_t,\n"
           "                   static_cast<std::size_t>(\n"
           "                       FlagPollBatchRejectionStage::Count)> rejections{};\n"
           "        std::uint64_t batched_rounds = 0u;\n"
           "        std::uint64_t batched_guest_cycles = 0u;\n"
           "    };\n"
           "    struct MmioWaitLoopBatchCounters {\n"
           "        std::uint64_t attempts = 0u;\n"
           "        std::uint64_t admissions = 0u;\n"
           "        std::uint64_t batched_rounds = 0u;\n"
           "        std::uint64_t batched_guest_cycles = 0u;\n"
           "    };\n"
           "    struct StaticAotChainGuard {\n"
           "        katana::runtime::BlockVariantKey variant{};\n"
           "        std::uint64_t runtime_dispatch_generation = 0u;\n"
           "        std::uint64_t code_generation = 0u;\n"
           "        std::uint64_t scheduler_reset_generation = 0u;\n"
           "        std::uint64_t chain_pending_cycle_limit = 0u;\n"
           "        std::uint64_t defer_pending_cycle_limit = 0u;\n"
           "        std::uint64_t router_epoch = 0u;\n"
           "        std::uint64_t controller_epoch = 0u;\n"
           "        std::uint64_t pending_mask = 0u;\n"
           "        std::uint32_t scheduling_sr = 0u;\n"
           "        std::uint8_t highest_pending_level = 0u;\n"
           "        katana::runtime::ExecutableChainRejectionReason\n"
           "            cycle_limit_rejection =\n"
           "                katana::runtime::ExecutableChainRejectionReason::CycleQuantum;\n"
           "        bool interrupt_acceptable = true;\n"
           "        bool valid = false;\n"
           "    };\n"
           "  public:\n"
           "    PortPlatformServices(katana::runtime::CpuState& cpu,\n"
           "                         const katana::runtime::DreamcastRuntimeState& state,\n"
           "                         std::function<katana::runtime::PlatformLifecycleState()> "
           "lifecycle_poll, std::function<void()> guest_frame_poll,\n"
           "                         bool eager_host_poll = false,\n"
           "                         bool runtime_probe_mode = false,\n"
           "                         katana::runtime::SystemReplayLog* replay_log = nullptr,\n"
           "                         std::function<void()> product_entry_evidence_callback = {},\n"
           "                         std::function<void()> game_entry_callback = {},\n"
           "                         std::optional<std::uint64_t>\n"
           "                             requested_post_entry_cycles = {},\n"
           "                         ProductTerminalTelemetry*\n"
           "                             product_terminal_telemetry = nullptr)\n"
           "        : cpu_(cpu), state_(state), lifecycle_poll_(std::move(lifecycle_poll)),\n"
           "          guest_frame_poll_(std::move(guest_frame_poll)),\n"
           "          product_entry_evidence_callback_(\n"
           "              std::move(product_entry_evidence_callback)),\n"
           "          game_entry_callback_(std::move(game_entry_callback)),\n"
           "          replay_log_(replay_log),\n"
           "          replay_observations_(replay_log, state.scheduler.get()),\n"
           "          requested_post_entry_cycles_(requested_post_entry_cycles),\n"
           "          restored_guest_cycle_(state.scheduler->current_cycle()),\n"
           "          product_terminal_telemetry_(product_terminal_telemetry),\n"
           "          eager_host_poll_(eager_host_poll),\n"
           "          runtime_probe_mode_(runtime_probe_mode),\n"
           "          local_block_chaining_enabled_([] {\n"
           "              const auto* limit = std::getenv(\"KATANA_PORT_BLOCK_LIMIT\");\n"
           "              return limit == nullptr || *limit == '\\0';\n"
           "          }()) {\n"
           "        if (runtime_probe_mode_ && replay_log_ != nullptr) {\n"
           "            replay_log_->enable_coverage(\n"
           "                static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
           "                    katana::runtime::SystemReplayCoverage::CpuSafepoint) |\n"
           "                static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
           "                    katana::runtime::SystemReplayCoverage::AcceptedInterrupt) |\n"
           "                static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
           "                    katana::runtime::SystemReplayCoverage::Dma));\n"
           "        }\n"
           "        counted_loop_batching_enabled_ = !diagnostic_partial_port &&\n"
           "            !runtime_probe_mode_ && replay_log_ == nullptr;\n"
           "        constexpr std::array counted_loop_debug_environment{\n"
           "            \"KATANA_PORT_BLOCK_LIMIT\", \"KATANA_PORT_PROGRESS_INTERVAL\",\n"
           "            \"KATANA_RUNTIME_PROBE\", \"KATANA_PORT_WAIT_LOOP_TRACE\",\n"
           "            \"KATANA_PORT_LIFECYCLE_TEST\"};\n"
           "        for (const auto* name : counted_loop_debug_environment)\n"
           "            if (const auto* value = std::getenv(name);\n"
           "                value != nullptr && *value != '\\0')\n"
           "                counted_loop_batching_enabled_ = false;\n"
           "#if defined(KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST) || \\\n"
           "    defined(KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST)\n"
           "        if (const auto* scalar =\n"
           "                std::getenv(\"KATANA_PORT_TEST_DISABLE_COUNTED_LOOP\");\n"
           "            scalar != nullptr && std::string_view(scalar) == \"1\")\n"
           "            counted_loop_batching_enabled_ = false;\n"
           "#endif\n"
           "        mmio_wait_loop_batching_enabled_ = !diagnostic_partial_port &&\n"
           "            !runtime_probe_mode_ && replay_log_ == nullptr;\n"
           "        constexpr std::array mmio_wait_loop_debug_environment{\n"
           "            \"KATANA_PORT_BLOCK_LIMIT\", \"KATANA_PORT_PROGRESS_INTERVAL\",\n"
           "            \"KATANA_RUNTIME_PROBE\", \"KATANA_PORT_WAIT_LOOP_TRACE\",\n"
           "            \"KATANA_PORT_COUNTED_LOOP_TRACE\",\n"
           "            \"KATANA_PORT_LIFECYCLE_TEST\"};\n"
           "        for (const auto* name : mmio_wait_loop_debug_environment)\n"
           "            if (const auto* value = std::getenv(name);\n"
           "                value != nullptr && *value != '\\0')\n"
           "                mmio_wait_loop_batching_enabled_ = false;\n"
           "        memory_fill_loop_batching_enabled_ = !diagnostic_partial_port &&\n"
           "            !runtime_probe_mode_ && replay_log_ == nullptr;\n"
           "        constexpr std::array memory_fill_loop_debug_environment{\n"
           "            \"KATANA_PORT_BLOCK_LIMIT\", \"KATANA_PORT_PROGRESS_INTERVAL\",\n"
           "            \"KATANA_RUNTIME_PROBE\",\n"
           "            \"KATANA_PORT_WAIT_LOOP_TRACE\",\n"
           "            \"KATANA_PORT_LIFECYCLE_TEST\"};\n"
           "        for (const auto* name : memory_fill_loop_debug_environment)\n"
           "            if (const auto* value = std::getenv(name);\n"
           "                value != nullptr && *value != '\\0')\n"
           "                memory_fill_loop_batching_enabled_ = false;\n"
           "#if defined(KATANA_INTERNAL_MEMORY_FILL_DIFFERENTIAL_TEST)\n"
           "        if (const auto* scalar =\n"
           "                std::getenv(\"KATANA_PORT_TEST_DISABLE_MEMORY_FILL\");\n"
           "            scalar != nullptr && std::string_view(scalar) == \"1\")\n"
           "            memory_fill_loop_batching_enabled_ = false;\n"
           "#endif\n"
           "        composite_callback_batching_enabled_ = !diagnostic_partial_port &&\n"
           "            !runtime_probe_mode_ && replay_log_ == nullptr;\n"
           "        constexpr std::array composite_callback_debug_environment{\n"
           "            \"KATANA_PORT_BLOCK_LIMIT\", \"KATANA_PORT_PROGRESS_INTERVAL\",\n"
           "            \"KATANA_RUNTIME_PROBE\", \"KATANA_PORT_WAIT_LOOP_TRACE\",\n"
           "            \"KATANA_PORT_LIFECYCLE_TEST\"};\n"
           "        for (const auto* name : composite_callback_debug_environment)\n"
           "            if (const auto* value = std::getenv(name);\n"
           "                value != nullptr && *value != '\\0')\n"
           "                composite_callback_batching_enabled_ = false;\n"
           "#if defined(KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST)\n"
           "        if (const auto* scalar = std::getenv(\n"
           "                \"KATANA_PORT_TEST_DISABLE_COMPOSITE_CALLBACK\");\n"
           "            scalar != nullptr && std::string_view(scalar) == \"1\")\n"
           "            composite_callback_batching_enabled_ = false;\n"
           "#endif\n"
           "        cpu_.memory.attach_crash_capsule(crash_capsule_);\n"
           "        state_.scheduler->attach_crash_capsule(crash_capsule_);\n"
           "        if (product_terminal_telemetry_ != nullptr) {\n"
           "            product_terminal_telemetry_->restored_guest_cycle =\n"
           "                restored_guest_cycle_;\n"
           "            product_terminal_telemetry_->final_guest_cycle =\n"
           "                restored_guest_cycle_;\n"
           "            product_terminal_telemetry_->requested_post_entry_cycles =\n"
           "                requested_post_entry_cycles_.value_or(0u);\n"
           "        }\n"
           "    }\n"
           "    ~PortPlatformServices() override {\n"
           "        if (product_terminal_telemetry_ != nullptr) {\n"
           "            product_terminal_telemetry_->restored_guest_cycle =\n"
           "                restored_guest_cycle_;\n"
           "            product_terminal_telemetry_->start_guest_cycle =\n"
           "                guest_program_entry_cycle_;\n"
           "            product_terminal_telemetry_->final_guest_cycle =\n"
           "                state_.scheduler ? state_.scheduler->current_cycle()\n"
           "                                 : restored_guest_cycle_;\n"
           "            product_terminal_telemetry_->requested_post_entry_cycles =\n"
           "                requested_post_entry_cycles_.value_or(0u);\n"
           "            product_terminal_telemetry_->target_guest_cycle =\n"
           "                product_target_guest_cycle_.value_or(0u);\n"
           "            product_terminal_telemetry_->product_budget_arm_failed =\n"
           "                product_budget_arm_failed_;\n"
           "            product_terminal_telemetry_->guest_program_started =\n"
           "                guest_program_dispatched_;\n"
           "            product_terminal_telemetry_->milestone_bits |=\n"
           "                (guest_program_dispatched_ ? 1u << 0u : 0u) |\n"
           "                (guest_program_progressed_ ? 1u << 1u : 0u);\n"
           "            if (post_entry_host_started_) {\n"
           "                product_terminal_telemetry_->post_entry_host_seconds =\n"
           "                    std::chrono::duration<double>(\n"
           "                        std::chrono::steady_clock::now() -\n"
           "                        *post_entry_host_started_).count();\n"
           "            }\n"
           "        }\n"
           "        cpu_.memory.detach_crash_capsule(crash_capsule_);\n"
           "        if (state_.scheduler)\n"
           "            state_.scheduler->detach_crash_capsule(crash_capsule_);\n"
           "    }\n"
           "    katana::runtime::CrashCapsule& crash_capsule() noexcept {\n"
           "        return crash_capsule_;\n"
           "    }\n"
           "    std::string_view name() const noexcept override { return \"dreamcast-port\"; }\n"
           "    std::uint32_t abi_version() const noexcept override {\n"
           "        return katana::runtime::platform_services_abi_version;\n"
           "    }\n"
           "    std::uint32_t guest_cycle_contract() const noexcept override {\n"
           "        return katana::runtime::guest_cycle_contract_version;\n"
           "    }\n"
           "    katana::runtime::PlatformCapabilities capabilities() const noexcept override {\n"
           "        return katana::runtime::core_platform_capabilities;\n"
           "    }\n"
           "    void read_memory(std::uint32_t address, std::span<std::uint8_t> output) override "
           "{\n"
           "        for (auto& byte : output) byte = "
           "katana::runtime::guest_read_u8(cpu_, address++);\n"
           "    }\n"
           "    void write_memory(std::uint32_t address, std::span<const std::uint8_t> input) "
           "override {\n"
           "        for (const auto byte : input) katana::runtime::guest_write_u8(\n"
           "            cpu_, address++, byte, katana::runtime::CodeWriteSource::Fallback);\n"
           "    }\n"
           "    std::uint64_t scheduler_cycle() const noexcept override {\n"
           "        return state_.scheduler->current_cycle();\n"
           "    }\n"
           "    std::optional<std::uint64_t> next_scheduler_event_cycle() const noexcept override "
           "{\n"
           "        return state_.scheduler->next_event_cycle();\n"
           "    }\n"
           "    katana::runtime::PlatformSchedulerResult\n"
           "    consume_guest_cycles(std::uint64_t cycles, std::size_t budget) override {\n"
           "#if defined(KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST)\n"
           "        if (internal_batch_commit_abort_requested(\n"
           "                active_atomic_batch_commit_))\n"
           "            throw katana::runtime::PlatformShutdownRequested();\n"
           "#endif\n"
           "        if (!runtime_probe_mode_) {\n"
           "            for (;;) {\n"
           "                const auto lifecycle = poll_host_lifecycle();\n"
           "                if (lifecycle == katana::runtime::PlatformLifecycleState::Shutdown)\n"
           "                    throw katana::runtime::PlatformShutdownRequested();\n"
           "                if (lifecycle == katana::runtime::PlatformLifecycleState::Running)\n"
           "                    break;\n"
           "                std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "            }\n"
           "        }\n"
           "        const auto before = state_.scheduler->current_cycle();\n"
           "        const auto result = state_.scheduler->advance_by(cycles, budget);\n"
           "        if (result.processed_events != 0u) interrupt_sources_dirty_ = true;\n"
           "        if (replay_log_ != nullptr) {\n"
           "            const auto requested = cycles >\n"
           "                    std::numeric_limits<std::uint64_t>::max() - before\n"
           "                ? std::numeric_limits<std::uint64_t>::max() : before + cycles;\n"
           "            const auto jitter = requested > result.guest_cycle\n"
           "                ? requested - result.guest_cycle : result.guest_cycle - requested;\n"
           "            auto event = katana::runtime::make_safepoint_replay_event({\n"
           "                katana::runtime::SafepointKind::BlockEnd,\n"
           "                katana::runtime::ExecutionOrigin::Backend,\n"
           "                requested, result.guest_cycle, jitter, result.processed_events,\n"
           "                false,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::EventBudgetExhausted,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::GuestCycleBudgetExhausted});\n"
           "            event.time_epoch = state_.scheduler->reset_generation();\n"
           "            static_cast<void>(replay_log_->try_record(std::move(event)));\n"
           "        }\n"
           "        if (runtime_probe_mode_) {\n"
           "            const auto remaining = state_.scheduler->remaining_guest_cycles();\n"
           "            if (remaining && *remaining == 0u)\n"
           "                throw katana::runtime::RuntimeProbeBudgetReached(result.guest_cycle);\n"
           "        }\n"
           "        if (!runtime_probe_mode_) {\n"
           "            if (guest_frame_poll_ &&\n"
           "                (result.guest_cycle < last_frame_poll_guest_cycle_ ||\n"
           "                 result.guest_cycle >= next_frame_poll_guest_cycle_)) {\n"
           "                guest_frame_poll_();\n"
           "                last_frame_poll_guest_cycle_ = result.guest_cycle;\n"
           "                next_frame_poll_guest_cycle_ = result.guest_cycle >\n"
           "                        std::numeric_limits<std::uint64_t>::max() -\n"
           "                            host_poll_guest_cycle_quantum\n"
           "                    ? std::numeric_limits<std::uint64_t>::max()\n"
           "                    : result.guest_cycle + host_poll_guest_cycle_quantum;\n"
           "            }\n"
           "        }\n"
           "        return {result.guest_cycle, result.processed_events,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::EventBudgetExhausted,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::GuestCycleBudgetExhausted};\n"
           "    }\n"
           "    void synchronize_interrupt_sources_if_needed() const {\n"
           "        const auto router_epoch = state_.interrupt_router->source_epoch();\n"
           "        if (!interrupt_sources_dirty_ &&\n"
           "            router_epoch == synchronized_interrupt_router_epoch_) return;\n"
           "        static_cast<void>(state_.interrupt_router->synchronize());\n"
           "        synchronized_interrupt_router_epoch_ =\n"
           "            state_.interrupt_router->source_epoch();\n"
           "        interrupt_sources_dirty_ = false;\n"
           "    }\n"
           "    std::optional<katana::runtime::PlatformInterruptRequest> poll_interrupt() override "
           "{\n"
           "        synchronize_interrupt_sources_if_needed();\n"
           "        if (!state_.interrupt_controller->can_accept(cpu_) ||\n"
           "            !state_.interrupt_router->accept_cached(cpu_)) return std::nullopt;\n"
           "        // Accepting removes the controller entry. A level-sensitive source\n"
           "        // must be mirrored again after RTE/BL or IMASK permits it.\n"
           "        interrupt_sources_dirty_ = true;\n"
           "        if (replay_log_ != nullptr)\n"
           "            record_runtime_probe_event(\n"
           "                *replay_log_, *state_.scheduler,\n"
           "                katana::runtime::SystemReplayEventKind::Interrupt,\n"
           "                \"interrupt-accepted\", cpu_.intevt);\n"
           "        return katana::runtime::PlatformInterruptRequest{0u, 0u, cpu_.intevt};\n"
           "    }\n"
           "    katana::runtime::PlatformDmaResult\n"
           "    start_dma(const katana::runtime::PlatformDmaRequest& request) override {\n"
           "        if (request.length == 0u) return {};\n"
           "        if (replay_log_ != nullptr)\n"
           "            record_runtime_probe_event(\n"
           "                *replay_log_, *state_.scheduler,\n"
           "                katana::runtime::SystemReplayEventKind::Dma,\n"
           "                \"platform-dma-start\", request.length,\n"
           "                (static_cast<std::uint64_t>(request.source) << 32u) |\n"
           "                    request.destination);\n"
           "        state_.dmac->start_byte_transfer(\n"
           "            0u, request.source, request.destination, request.length, true);\n"
           "        return {0u, false};\n"
           "    }\n"
           "    katana::runtime::PlatformFallbackResult controlled_fallback(\n"
           "        katana::runtime::CpuState&, const katana::runtime::PlatformFallbackRequest&) "
           "override {\n"
           "        ++fallback_count_;\n"
           "        replay_observations_.observe_controlled_fallback();\n"
           "        return {};\n"
           "    }\n"
           "    bool prefetch(katana::runtime::CpuState& cpu,\n"
           "                  katana::runtime::GuestInstructionOrigin instruction,\n"
           "                  std::uint32_t address) override {\n"
           "        katana::runtime::prefetch(cpu, address);\n"
           "        return state_.store_queues->prefetch(\n"
           "            address,\n"
           "            instruction,\n"
           "            cpu.retired_guest_instructions,\n"
           "            cpu.attempted_guest_instructions);\n"
           "    }\n"
           "    katana::runtime::PlatformLifecycleState poll_host_lifecycle() override {\n"
           "        if (product_budget_arm_failed_)\n"
           "            throw std::runtime_error(\n"
           "                \"product-post-entry-cycle-budget-invalid\");\n"
           "        if (runtime_probe_mode_) {\n"
           "            const auto remaining = state_.scheduler->remaining_guest_cycles();\n"
           "            if (remaining && *remaining == 0u)\n"
           "                throw katana::runtime::RuntimeProbeBudgetReached(\n"
           "                    state_.scheduler->current_cycle());\n"
           "            return katana::runtime::PlatformLifecycleState::Running;\n"
           "        }\n"
           "        if (!lifecycle_poll_)\n"
           "            return katana::runtime::PlatformLifecycleState::Running;\n"
           "        const auto guest_cycle = state_.scheduler->current_cycle();\n"
           "        if (eager_host_poll_ || cached_lifecycle_ !=\n"
           "                katana::runtime::PlatformLifecycleState::Running ||\n"
           "            guest_cycle < last_lifecycle_poll_guest_cycle_ ||\n"
           "            guest_cycle >= next_lifecycle_poll_guest_cycle_) {\n"
           "            cached_lifecycle_ = lifecycle_poll_();\n"
           "            last_lifecycle_poll_guest_cycle_ = guest_cycle;\n"
           "            next_lifecycle_poll_guest_cycle_ = guest_cycle >\n"
           "                    std::numeric_limits<std::uint64_t>::max() -\n"
           "                        host_poll_guest_cycle_quantum\n"
           "                ? std::numeric_limits<std::uint64_t>::max()\n"
           "                : guest_cycle + host_poll_guest_cycle_quantum;\n"
           "        }\n"
           "        return cached_lifecycle_;\n"
           "    }\n"
           "    void observe_guest_checkpoint(std::uint32_t address) noexcept override {\n"
           "        observe_guest_block_completion(address, 0u, false, false);\n"
           "    }\n"
           "    void observe_guest_block_completion(\n"
           "            std::uint32_t address, std::uint64_t retired_delta,\n"
           "            bool new_exception, bool exception_exit) noexcept override {\n"
           "        ++executed_blocks_;\n"
           "        if (guest_program_progressed_) return;\n"
           "        if (!guest_program_range_matcher_.contains_instruction(\n"
           "                cpu_, address)) return;\n"
           "        if (!guest_program_dispatched_) note_guest_program_entry();\n"
           "        if (retired_delta == 0u || new_exception ||\n"
           "            exception_exit) return;\n"
           "        guest_program_progressed_ = true;\n"
           "        guest_program_progress_cycle_ = state_.scheduler->current_cycle();\n"
           "        observe_runtime_checkpoint(\n"
           "            katana::runtime::SystemReplayCheckpointKind::GuestProgramEntered,\n"
           "            katana::runtime::RuntimeProbeCheckpoint::GuestProgramEntered);\n"
           "    }\n"
           "    void observe_runtime_started() noexcept {\n"
           "        observe_runtime_checkpoint(\n"
           "            katana::runtime::SystemReplayCheckpointKind::RuntimeStarted,\n"
           "            katana::runtime::RuntimeProbeCheckpoint::RuntimeStarted);\n"
           "    }\n"
           "    void observe_first_guest_frame() noexcept {\n"
           "        observe_runtime_checkpoint(\n"
           "            katana::runtime::SystemReplayCheckpointKind::FirstGuestFrame,\n"
           "            katana::runtime::RuntimeProbeCheckpoint::FirstGuestFrame);\n"
           "    }\n"
           "    katana::runtime::SystemReplayObservationSession&\n"
           "    observation_session() noexcept { return replay_observations_; }\n"
           "    void emit_runtime_fault(\n"
           "            katana::runtime::RuntimeProbeTermination termination) noexcept {\n"
           "        if (fault_emitted_) return;\n"
           "        fault_emitted_ = true;\n"
           "        static_cast<void>(probe_observations_.latch_fault(\n"
           "            termination, katana::runtime::capture_runtime_probe_cpu(cpu_)));\n"
           "        try {\n"
           "            std::cout << katana::runtime::runtime_probe_fault_line_prefix\n"
           "                      << "
           "katana::runtime::serialize_runtime_probe_fault_envelope_json(\n"
           "                             probe_observations_.fault_envelope(termination))\n"
           "                      << std::endl;\n"
           "        } catch (...) {\n"
           "            if (replay_log_ != nullptr) replay_log_->note_dropped_event();\n"
           "        }\n"
           "    }\n"
           "    bool guest_checkpoint_reached() const noexcept {\n"
           "        return guest_program_progressed_;\n"
           "    }\n"
           "    bool guest_program_dispatched() const noexcept {\n"
           "        return guest_program_dispatched_;\n"
           "    }\n"
           "    bool guest_program_progressed() const noexcept {\n"
           "        return guest_program_progressed_;\n"
           "    }\n"
           "    std::uint64_t guest_program_entry_cycle() const noexcept {\n"
           "        return guest_program_entry_cycle_;\n"
           "    }\n"
           "    std::uint64_t guest_program_progress_cycle() const noexcept {\n"
           "        return guest_program_progress_cycle_;\n"
           "    }\n"
           "    std::uint64_t restored_guest_cycle() const noexcept {\n"
           "        return restored_guest_cycle_;\n"
           "    }\n"
           "    std::optional<std::uint64_t>\n"
           "    requested_post_entry_cycles() const noexcept {\n"
           "        return requested_post_entry_cycles_;\n"
           "    }\n"
           "    std::optional<std::uint64_t>\n"
           "    product_target_guest_cycle() const noexcept {\n"
           "        return product_target_guest_cycle_;\n"
           "    }\n"
           "    bool product_budget_arm_failed() const noexcept {\n"
           "        return product_budget_arm_failed_;\n"
           "    }\n"
           "    double post_entry_host_seconds() const noexcept {\n"
           "        if (!post_entry_host_started_) return 0.0;\n"
           "        return std::chrono::duration<double>(\n"
           "            std::chrono::steady_clock::now() -\n"
           "            *post_entry_host_started_).count();\n"
           "    }\n"
           "    std::uint64_t executed_blocks() const noexcept { return executed_blocks_; }\n"
           "    bool product_entry_evidence_failed() const noexcept {\n"
           "        return product_entry_evidence_failed_;\n"
           "    }\n"
           "    std::uint64_t fallback_count() const noexcept { return fallback_count_; }\n"
           "    void report_flag_poll_batch_statistics(std::ostream& output) const {\n"
           "        const auto rejected = [&](const FlagPollBatchRejectionStage stage) {\n"
           "            return flag_poll_batch_counters_.rejections[\n"
           "                static_cast<std::size_t>(stage)];\n"
           "        };\n"
           "        output << \"KATANA_FLAG_POLL_BATCH_STATS attempts=\"\n"
           "               << flag_poll_batch_counters_.attempts\n"
           "               << \" admissions=\" << flag_poll_batch_counters_.admissions\n"
           "               << \" batched_rounds=\" << flag_poll_batch_counters_.batched_rounds\n"
           "               << \" batched_guest_cycles=\"\n"
           "               << flag_poll_batch_counters_.batched_guest_cycles\n"
           "               << \" reject_poll_descriptor=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::Descriptor)\n"
           "               << \" reject_poll_runtime_state=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::RuntimeState)\n"
           "               << \" reject_poll_not_at_clean_safepoint=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::NotAtCleanSafepoint)\n"
           "               << \" reject_poll_interrupt_pending=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::InterruptPending)\n"
           "               << \" reject_poll_live_target=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::LiveTarget)\n"
           "               << \" reject_poll_candidate_provenance=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::CandidateProvenance)\n"
           "               << \" reject_poll_registered_blocks=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::RegisteredBlocks)\n"
           "               << \" reject_poll_instruction_mapping=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::InstructionMapping)\n"
           "               << \" reject_poll_entry_registers=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::EntryRegisters)\n"
           "               << \" reject_poll_alignment=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::Alignment)\n"
           "               << \" reject_poll_ram_translation=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::RamTranslation)\n"
           "               << \" reject_poll_ram_backing=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::RamBacking)\n"
           "               << \" reject_poll_scheduler_event_due=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::SchedulerEventDue)\n"
           "               << \" reject_poll_scheduler_budget_empty=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::SchedulerBudgetEmpty)\n"
           "               << \" reject_poll_empty_batch=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::EmptyBatch)\n"
           "               << \" reject_poll_accounting_overflow=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::AccountingOverflow)\n"
           "               << \" reject_poll_cpu_counter_overflow=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::CpuCounterOverflow)\n"
           "               << \" reject_poll_memory_accounting=\"\n"
           "               << rejected(FlagPollBatchRejectionStage::MemoryAccounting)\n"
           "               << '\\n';\n"
           "    }\n"
           "    void report_mmio_wait_loop_batch_statistics(std::ostream& output) const {\n"
           "        output << \"KATANA_MMIO_WAIT_BATCH_STATS attempts=\"\n"
           "               << mmio_wait_loop_batch_counters_.attempts\n"
           "               << \" admissions=\" << mmio_wait_loop_batch_counters_.admissions\n"
           "               << \" batched_rounds=\"\n"
           "               << mmio_wait_loop_batch_counters_.batched_rounds\n"
           "               << \" batched_guest_cycles=\"\n"
           "               << mmio_wait_loop_batch_counters_.batched_guest_cycles\n"
           "               << '\\n';\n"
           "    }\n"
           "    void register_executable_block(\n"
           "            std::uint32_t address, std::uint32_t physical_origin,\n"
           "            std::uint32_t size, std::string_view identity,\n"
           "            katana::runtime::ExecutableBlockTimingClass timing_class,\n"
           "            std::uint64_t maximum_guest_cycles) override {\n"
           "        static_cast<void>(state_.code_tracker->register_block(\n"
           "            {std::string(identity), "
           "katana::runtime::canonical_physical_address(physical_origin),\n"
           "             size, \"generated-port\", {},\n"
           "             katana::runtime::ExecutableBlockOrigin::ImageSegment}));\n"
           "        const auto [registered, inserted] =\n"
           "            executable_blocks_.insert_or_assign(\n"
           "                address, ExecutableBlockRegistration{\n"
           "            std::string(identity), "
           "katana::runtime::canonical_physical_address(physical_origin),\n"
           "            size, timing_class, maximum_guest_cycles});\n"
           "        static_cast<void>(inserted);\n"
           "        registered->second.validated_code_generation =\n"
           "            state_.code_tracker->invalidation_count();\n"
           "    }\n"
           "    void allow_executable_block_chaining(std::uint32_t address) override {\n"
           "        const auto found = executable_blocks_.find(address);\n"
           "        if (found == executable_blocks_.end()) return;\n"
           "        found->second.chainable = true;\n"
           "        const auto physical = katana::runtime::canonical_physical_address(address);\n"
           "        if ((physical & 1u) != 0u) return;\n"
           "        const auto page_index = physical / executable_chain_page_size;\n"
           "        if (page_index >= executable_chain_pages_.size()) return;\n"
           "        auto& page = executable_chain_pages_[page_index];\n"
           "        if (!page) page = std::make_unique<ExecutableChainPage>();\n"
           "        page->entries[(physical % executable_chain_page_size) / 2u] =\n"
           "            &found->second;\n"
           "    }\n"
           "    void begin_executable_block(\n"
           "            std::uint32_t virtual_start, std::uint32_t physical_start,\n"
           "            std::uint32_t size,\n"
           "            const katana::runtime::BlockVariantKey& variant,\n"
           "            bool runtime_registered) noexcept override {\n"
           "        last_executable_chain_rejection_ =\n"
           "            katana::runtime::ExecutableChainRejectionReason::None;\n"
           "        if (!game_entry_callback_emitted_ && game_entry_callback_ &&\n"
           "            virtual_start == expected_guest_program_range.guest_start &&\n"
           "            guest_program_range_matcher_.contains_instruction(\n"
           "                cpu_, virtual_start)) {\n"
           "            game_entry_callback_emitted_ = true;\n"
           "            try {\n"
           "                game_entry_callback_();\n"
           "            } catch (...) {\n"
           "                game_entry_callback_failed_ = true;\n"
           "                std::cerr << \"KATANA_GAME_ENTRY_CALLBACK status=failed\\n\";\n"
           "            }\n"
           "        }\n"
           "        if (!guest_program_dispatched_ &&\n"
           "            guest_program_range_matcher_.contains_instruction(\n"
           "                cpu_, virtual_start))\n"
           "            note_guest_program_entry();\n"
           "        if (game_entry_probe_enabled_ && !game_entry_probe_emitted_ &&\n"
           "            guest_program_range_matcher_.contains_instruction(\n"
           "                cpu_, virtual_start)) {\n"
           "            game_entry_probe_emitted_ = true;\n"
           "            try {\n"
           "                const auto cpu_probe =\n"
           "                    katana::runtime::capture_runtime_probe_cpu(cpu_);\n"
           "                const auto scheduler_probe =\n"
           "                    katana::runtime::capture_runtime_probe_scheduler(\n"
           "                        *state_.scheduler);\n"
           "                const std::array main_ram_probe{\n"
           "                    katana::runtime::RuntimeProbeMemoryRange{\n"
           "                        katana::runtime::RuntimeProbeMemoryRegion::MainRam,\n"
           "                        0u, state_.main_ram->bytes()}};\n"
           "                const std::array vram_probe{\n"
           "                    katana::runtime::RuntimeProbeMemoryRange{\n"
           "                        katana::runtime::RuntimeProbeMemoryRegion::VideoRam,\n"
           "                        0u, state_.vram->bytes()}};\n"
           "                const std::array aica_ram_probe{\n"
           "                    katana::runtime::RuntimeProbeMemoryRange{\n"
           "                        katana::runtime::RuntimeProbeMemoryRegion::AicaRam,\n"
           "                        0u, state_.aica_ram->bytes()}};\n"
           "                const auto dreamcast_probe =\n"
           "                    katana::runtime::capture_runtime_probe_dreamcast(\n"
           "                        state_, 0u, 0u, 0u);\n"
           "                std::cout << \"KATANA_GAME_ENTRY_STATE status=captured\"\n"
           "                          << \" guest_cycle=\"\n"
           "                          << state_.scheduler->current_cycle()\n"
           "                          << \" runtime_cpu_hash=\"\n"
           "                          << katana::runtime::hash_runtime_probe_cpu(\n"
           "                                 cpu_probe)\n"
           "                          << \" runtime_scheduler_hash=\"\n"
           "                          << katana::runtime::hash_runtime_probe_scheduler(\n"
           "                                 scheduler_probe)\n"
           "                          << \" main_ram_hash=\"\n"
           "                          << katana::runtime::hash_runtime_probe_memory(\n"
           "                                 main_ram_probe)\n"
           "                          << \" vram_hash=\"\n"
           "                          << katana::runtime::hash_runtime_probe_memory(\n"
           "                                 vram_probe)\n"
           "                          << \" aica_ram_hash=\"\n"
           "                          << katana::runtime::hash_runtime_probe_memory(\n"
           "                                 aica_ram_probe)\n"
           "                          << \" runtime_device_hash=\"\n"
           "                          << katana::runtime::hash_runtime_probe_devices(\n"
           "                                 dreamcast_probe.devices)\n"
           "                          << \" pc=\" << cpu_.pc\n"
           "                          << \" pr=\" << cpu_.pr\n"
           "                          << \" sr=\" << cpu_.read_sr()\n"
           "                          << \" fpscr=\" << cpu_.read_fpscr()\n"
           "                          << \" vbr=\" << cpu_.vbr\n"
           "                          << \" gbr=\" << cpu_.gbr\n"
           "                          << \" r15=\" << cpu_.r[15]\n"
           "                          << \" sgr=\" << cpu_.sgr\n"
           "                          << \" spc=\" << cpu_.spc\n"
           "                          << \" ssr=\" << cpu_.ssr\n"
           "                          << \" expevt=\" << cpu_.expevt\n"
           "                          << \" intevt=\" << cpu_.intevt\n"
           "                          << \" mmucr=\" << cpu_.mmucr\n"
           "                          << \" pending_cycles=\"\n"
           "                          << cpu_.pending_guest_cycles;\n"
           "                for (std::size_t index = 0u; index < cpu_.r.size(); ++index)\n"
           "                    std::cout << \" r\" << index << '=' << cpu_.r[index];\n"
           "                std::cout << '\\n';\n"
           "            } catch (...) {\n"
           "                std::cout << \"KATANA_GAME_ENTRY_STATE status=capture-failed\\n\";\n"
           "            }\n"
           "        }\n"
           "        active_block_variant_ = variant;\n"
           "        active_block_registration_ = nullptr;\n"
           "        if (!runtime_registered) {\n"
           "            const auto exact = executable_blocks_.find(virtual_start);\n"
           "            if (exact != executable_blocks_.end() &&\n"
           "                exact->second.physical_origin ==\n"
           "                    katana::runtime::canonical_physical_address(physical_start) &&\n"
           "                exact->second.size == size)\n"
           "                active_block_registration_ = &exact->second;\n"
           "        }\n"
           "        prepare_static_aot_chain_guard(\n"
           "            virtual_start, physical_start, runtime_registered);\n"
           "    }\n"
           "    bool can_chain_executable_block(std::uint32_t address) const noexcept override {\n"
           "        using Rejection = "
           "katana::runtime::ExecutableChainRejectionReason;\n"
           "        last_executable_chain_rejection_ = Rejection::None;\n"
           "        const auto reject = [&](const Rejection reason) noexcept {\n"
           "            last_executable_chain_rejection_ = reason;\n"
           "            return false;\n"
           "        };\n"
           "        if (runtime_probe_mode_ || replay_log_ != nullptr)\n"
           "            return reject(Rejection::DiagnosticMode);\n"
           "        if (!local_block_chaining_enabled_)\n"
           "            return reject(Rejection::ChainingDisabled);\n"
           "        if (!active_block_variant_ ||\n"
           "            !state_.address_space || !state_.scheduler ||\n"
           "            !state_.interrupt_controller || !state_.interrupt_router ||\n"
           "            !state_.code_tracker)\n"
           "            return reject(Rejection::MissingRuntimeContract);\n"
           "        const auto pending_guest_cycles = cpu_.pending_guest_cycles;\n"
           "        if (pending_guest_cycles == 0u)\n"
           "            return reject(Rejection::NoPendingGuestCycles);\n"
           "        if (!guest_program_progressed_ &&\n"
           "            guest_program_range_matcher_.contains_instruction(cpu_, address))\n"
           "            return reject(Rejection::GameEntryBarrier);\n"
           "        const auto target_segment = address >> 29u;\n"
           "        const bool direct_p1_p2_target = cpu_.privileged_mode() &&\n"
           "            (target_segment == 4u || target_segment == 5u);\n"
           "        if (direct_p1_p2_target) {\n"
           "            const auto physical =\n"
           "                katana::runtime::canonical_physical_address(address);\n"
           "            const ExecutableBlockRegistration* registration = nullptr;\n"
           "            const auto page_index = physical / executable_chain_page_size;\n"
           "            if (page_index < executable_chain_pages_.size()) {\n"
           "                const auto& page = executable_chain_pages_[page_index];\n"
           "                if (page)\n"
           "                    registration = page->entries[\n"
           "                        (physical % executable_chain_page_size) / 2u];\n"
           "            }\n"
           "            if (registration == nullptr)\n"
           "                return reject(Rejection::TargetNotRegistered);\n"
           "            if (!registration->chainable)\n"
           "                return reject(Rejection::TargetNotNativeEntrySafe);\n"
           "            if (registration->timing_class !=\n"
           "                     katana::runtime::ExecutableBlockTimingClass::PureCpu &&\n"
           "                 registration->timing_class !=\n"
           "                     katana::runtime::ExecutableBlockTimingClass::LinearRamOnly)\n"
           "                return reject(Rejection::TimingNotDeferrable);\n"
           "            const auto guard_rejection =\n"
           "                static_aot_chain_guard_rejection(address);\n"
           "            if (guard_rejection != Rejection::None)\n"
           "                return reject(guard_rejection);\n"
           "            if (physical != registration->physical_origin)\n"
           "                return reject(Rejection::AddressTranslation);\n"
           "            const auto& guard = active_static_aot_chain_guard_;\n"
           "            if (registration->validated_code_generation !=\n"
           "                    guard.code_generation &&\n"
           "                !state_.code_tracker->revalidate_dispatchable(\n"
           "                    registration->identity,\n"
           "                    registration->validated_code_generation))\n"
           "                return reject(Rejection::CodeGeneration);\n"
           "            if (registration->maximum_guest_cycles >\n"
           "                    guard.chain_pending_cycle_limit ||\n"
           "                pending_guest_cycles > guard.chain_pending_cycle_limit -\n"
           "                    registration->maximum_guest_cycles)\n"
           "                return reject(guard.cycle_limit_rejection);\n"
           "            cpu_.active_block_virtual_start = address;\n"
           "            cpu_.active_block_physical_start = registration->physical_origin;\n"
           "            cpu_.active_block_size = registration->size;\n"
           "            return true;\n"
           "        }\n"
           "\n"
           "        const auto found = executable_blocks_.find(address);\n"
           "        if (found == executable_blocks_.end())\n"
           "            return reject(Rejection::TargetNotRegistered);\n"
           "        const auto* registration = &found->second;\n"
           "        if (!registration->chainable)\n"
           "            return reject(Rejection::TargetNotNativeEntrySafe);\n"
           "        if (registration->timing_class !=\n"
           "                 katana::runtime::ExecutableBlockTimingClass::PureCpu &&\n"
           "             registration->timing_class !=\n"
           "                 katana::runtime::ExecutableBlockTimingClass::LinearRamOnly)\n"
           "            return reject(Rejection::TimingNotDeferrable);\n"
           "        if (registration->maximum_guest_cycles > "
           "local_block_chain_guest_cycle_budget ||\n"
           "            pending_guest_cycles > local_block_chain_guest_cycle_budget -\n"
           "                registration->maximum_guest_cycles)\n"
           "            return reject(Rejection::CycleQuantum);\n"
           "        const auto current_cycle = state_.scheduler->current_cycle();\n"
           "        const auto prospective_guest_cycles =\n"
           "            pending_guest_cycles + registration->maximum_guest_cycles;\n"
           "        if (const auto remaining = state_.scheduler->remaining_guest_cycles();\n"
           "            remaining && prospective_guest_cycles > *remaining)\n"
           "            return reject(Rejection::GuestCycleBudget);\n"
           "        if (prospective_guest_cycles >\n"
           "            std::numeric_limits<std::uint64_t>::max() - current_cycle)\n"
           "            return reject(Rejection::CycleQuantum);\n"
           "        if (const auto event = state_.scheduler->next_event_cycle(); event &&\n"
           "            *event <= current_cycle + prospective_guest_cycles)\n"
           "            return reject(Rejection::SchedulerDue);\n"
           "        if (!cpu_.interrupts_blocked() && cpu_.interrupt_mask() != 15u) {\n"
           "            try {\n"
           "                synchronize_interrupt_sources_if_needed();\n"
           "                if (state_.interrupt_controller->can_accept(cpu_))\n"
           "                    return reject(Rejection::InterruptAcceptable);\n"
           "            } catch (...) {\n"
           "                return reject(Rejection::InterruptAcceptable);\n"
           "            }\n"
           "        }\n"
           "        try {\n"
           "            const auto inspected = state_.address_space->inspect_translation(\n"
           "                address, katana::runtime::TranslationAccess::Instruction,\n"
           "                cpu_.privileged_mode());\n"
           "            if (katana::runtime::canonical_physical_address(\n"
           "                    inspected.physical_address) != registration->physical_origin)\n"
           "                return reject(Rejection::AddressTranslation);\n"
           "            if (!state_.address_space->prove_instruction_mapping(\n"
           "                    address, registration->physical_origin, registration->size,\n"
           "                    cpu_.privileged_mode()))\n"
           "                return reject(Rejection::AddressTranslation);\n"
           "            const auto current = katana::runtime::block_variant_key(\n"
           "                state_.address_space->guard_for(\n"
           "                    address, cpu_.read_fpscr(), cpu_.privileged_mode()),\n"
           "                active_block_variant_->runtime_generation);\n"
           "            if (current != *active_block_variant_)\n"
           "                return reject(Rejection::VariantOrGeneration);\n"
           "            const auto fetched_physical =\n"
           "                katana::runtime::translate_guest_address(\n"
           "                    cpu_, address,\n"
           "                    katana::runtime::MemoryAccessOperation::Read,\n"
           "                    katana::runtime::MemoryAccessWidth::Halfword, true);\n"
           "            if (katana::runtime::canonical_physical_address(fetched_physical) !=\n"
           "                registration->physical_origin)\n"
           "                return reject(Rejection::AddressTranslation);\n"
           "        } catch (...) {\n"
           "            return reject(Rejection::AddressTranslation);\n"
           "        }\n"
           "        if (!state_.code_tracker->revalidate_dispatchable(\n"
           "                registration->identity,\n"
           "                registration->validated_code_generation))\n"
           "            return reject(Rejection::CodeGeneration);\n"
           "        cpu_.active_block_virtual_start = address;\n"
           "        cpu_.active_block_physical_start = registration->physical_origin;\n"
           "        cpu_.active_block_size = registration->size;\n"
           "        return true;\n"
           "    }\n"
           "    katana::runtime::ExecutableChainRejectionReason\n"
           "    last_executable_chain_rejection() const noexcept override {\n"
           "        return last_executable_chain_rejection_;\n"
           "    }\n"
           "    bool can_defer_guest_block_completion() const noexcept override {\n"
           "        if (!local_block_chaining_enabled_ || runtime_probe_mode_ ||\n"
           "            replay_log_ != nullptr || active_block_registration_ == nullptr ||\n"
           "            !active_block_variant_ || !state_.address_space ||\n"
           "            !state_.scheduler || !state_.interrupt_controller ||\n"
           "            !state_.interrupt_router || !state_.code_tracker)\n"
           "            return false;\n"
           "        // The first executable instruction is a real architectural\n"
           "        // handoff boundary. Commit the predecessor's deferred time and\n"
           "        // service any eligible interrupt before entry-state capture or\n"
           "        // DirectBoot parity checks observe the target.\n"
           "        if (!guest_program_progressed_ &&\n"
           "            guest_program_range_matcher_.contains_instruction(\n"
           "                cpu_, cpu_.pc))\n"
           "            return false;\n"
           "        const auto& block = *active_block_registration_;\n"
           "        if (block.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::PureCpu &&\n"
           "            block.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::LinearRamOnly)\n"
           "            return false;\n"
           "        if (cpu_.memory.watchpoint_count() != 0u ||\n"
           "            cpu_.memory.has_trace_handler() ||\n"
           "            cpu_.memory.has_mmio_trace_handler() ||\n"
           "            cpu_.memory.mmio_access_tracking_enabled() ||\n"
           "            cpu_.memory.has_guest_memory_access_sink())\n"
           "            return false;\n"
           "        const auto pending = cpu_.pending_guest_cycles;\n"
           "        if (pending == 0u)\n"
           "            return false;\n"
           "        const auto target_segment = cpu_.pc >> 29u;\n"
           "        const bool direct_p1_p2_target = cpu_.privileged_mode() &&\n"
           "            (target_segment == 4u || target_segment == 5u);\n"
           "        if (direct_p1_p2_target) {\n"
           "            if (static_aot_chain_guard_rejection(cpu_.pc) !=\n"
           "                    katana::runtime::ExecutableChainRejectionReason::None)\n"
           "                return false;\n"
           "            return pending <=\n"
           "                active_static_aot_chain_guard_.defer_pending_cycle_limit;\n"
           "        }\n"
           "        return false;\n"
           "    }\n"
           "    bool try_composite_callback_flag_poll_batch(\n"
           "            const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "            const " +
           entry_namespace +
           "::CompositeCallbackBatchDescriptor& descriptor) {\n"
           "        constexpr std::uint64_t quantum = 131'072u;\n"
           "        ++flag_poll_batch_counters_.attempts;\n"
           "        const auto reject = [&](const FlagPollBatchRejectionStage stage) {\n"
           "            ++flag_poll_batch_counters_.rejections[\n"
           "                static_cast<std::size_t>(stage)];\n"
           "            return false;\n"
           "        };\n"
           "        const auto runtime_state_allows_batch = [&] {\n"
           "            return cpu_.memory.watchpoint_count() == 0u &&\n"
           "                !cpu_.memory.has_trace_handler() &&\n"
           "                !cpu_.memory.has_mmio_trace_handler() &&\n"
           "                !cpu_.memory.mmio_access_tracking_enabled() &&\n"
           "                !cpu_.memory.has_guest_memory_access_sink() &&\n"
           "                cpu_.memory.lookup_mode() ==\n"
           "                    katana::runtime::MemoryLookupMode::Indexed &&\n"
           "                state_.main_ram &&\n"
           "                state_.main_ram->size() ==\n"
           "                    katana::runtime::dreamcast_main_ram_size &&\n"
           "                state_.address_space && cpu_.address_space &&\n"
           "                cpu_.address_space.get() == state_.address_space.get() &&\n"
           "                state_.address_space->mode() ==\n"
           "                    katana::runtime::AddressTranslationMode::NoMmu &&\n"
           "                state_.scheduler && state_.interrupt_controller &&\n"
           "                state_.interrupt_router && state_.code_tracker &&\n"
           "                state_.runtime_blocks && state_.module_catalog &&\n"
           "                state_.disc_load_transactions &&\n"
           "                !state_.disc_load_transactions->transaction_active() &&\n"
           "                active_block_variant_.has_value();\n"
           "        };\n"
           "        if (!composite_callback_batching_enabled_ ||\n"
           "            descriptor.kind != " +
           entry_namespace +
           "::CompositeCallbackBatchKind::FlagPollEqualImmediate ||\n"
           "            descriptor.call_instruction_address !=\n"
           "                descriptor.call_block_address ||\n"
           "            descriptor.continuation_address !=\n"
           "                descriptor.call_instruction_address + 4u ||\n"
           "            descriptor.exit_address != descriptor.continuation_address + 6u ||\n"
           "            descriptor.kernel_return_address != 0u ||\n"
           "            descriptor.source_load_instruction_address !=\n"
           "                descriptor.continuation_address ||\n"
           "            descriptor.target_store_instruction_address != 0u ||\n"
           "            descriptor.outer_branch_instruction_address !=\n"
           "                descriptor.continuation_address + 4u ||\n"
           "            descriptor.call_block_size != 4u ||\n"
           "            descriptor.continuation_size != 6u ||\n"
           "            descriptor.kernel_size != 4u ||\n"
           "            descriptor.kernel_return_size != 0u ||\n"
           "            descriptor.current_round_instruction_count != 5u ||\n"
           "            descriptor.subsequent_round_instruction_count != 7u ||\n"
           "            descriptor.call_block_guest_cycles == 0u ||\n"
           "            descriptor.continuation_guest_cycles == 0u ||\n"
           "            descriptor.kernel_guest_cycles == 0u ||\n"
           "            descriptor.kernel_return_guest_cycles != 0u ||\n"
           "            descriptor.current_round_guest_cycles !=\n"
           "                descriptor.continuation_guest_cycles +\n"
           "                    descriptor.kernel_guest_cycles ||\n"
           "            descriptor.subsequent_round_guest_cycles !=\n"
           "                descriptor.current_round_guest_cycles +\n"
           "                    descriptor.call_block_guest_cycles ||\n"
           "            descriptor.current_round_guest_cycles > quantum ||\n"
           "            descriptor.subsequent_round_guest_cycles > quantum ||\n"
           "            descriptor.callback_register >= cpu_.r.size() ||\n"
           "            descriptor.flag_base_register >= cpu_.r.size() ||\n"
           "            descriptor.flag_value_register != 0u ||\n"
           "            descriptor.flag_displacement < 0 ||\n"
           "            descriptor.flag_displacement > 60 ||\n"
           "            (descriptor.flag_displacement & 3) != 0)\n"
           "            return reject(FlagPollBatchRejectionStage::Descriptor);\n"
           "        if (!runtime_state_allows_batch())\n"
           "            return reject(FlagPollBatchRejectionStage::RuntimeState);\n"
           "        if (cpu_.pending_guest_cycles != 0u)\n"
           "            return reject(FlagPollBatchRejectionStage::NotAtCleanSafepoint);\n"
           "        if (!cpu_.interrupts_blocked() && cpu_.interrupt_mask() != 15u) {\n"
           "            synchronize_interrupt_sources_if_needed();\n"
           "            if (state_.interrupt_controller->can_accept(cpu_))\n"
           "                return reject(FlagPollBatchRejectionStage::InterruptPending);\n"
           "        }\n"
           "\n"
           "        const auto provenance_matches = [](const std::string_view actual,\n"
           "                                                  const std::string_view expected) {\n"
           "            if (actual == expected || actual.ends_with(expected)) return true;\n"
           "            constexpr std::string_view mmu_suffix = \"-mmu-variant\";\n"
           "            if (actual.size() < expected.size() + mmu_suffix.size() ||\n"
           "                !actual.ends_with(\"-mmu-variant\")) return false;\n"
           "            const auto expected_offset =\n"
           "                actual.size() - mmu_suffix.size() - expected.size();\n"
           "            return\n"
           "                (actual.size() == expected.size() + mmu_suffix.size() &&\n"
           "                 actual.starts_with(expected)) ||\n"
           "                actual.substr(expected_offset, expected.size()) == expected;\n"
           "        };\n"
           "        const auto live_target = cpu_.r[descriptor.callback_register];\n"
           "        if (cpu_.pc != live_target || selected_block.size != descriptor.kernel_size ||\n"
           "            selected_block.end_kind != katana::runtime::BlockEndKind::Return)\n"
           "            return reject(FlagPollBatchRejectionStage::LiveTarget);\n"
           "        const bool static_kernel =\n"
           "            !selected_block.runtime_registered && !selected_block.aot_template &&\n"
           "            live_target == descriptor.kernel_address &&\n"
           "            selected_block.virtual_start == descriptor.kernel_address &&\n"
           "            selected_block.physical_origin ==\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.kernel_address) &&\n"
           "            provenance_matches(selected_block.provenance,\n"
           "                               descriptor.kernel_provenance);\n"
           "        bool mapped_kernel = false;\n"
           "        if (selected_block.aot_template &&\n"
           "            selected_block.virtual_start == live_target &&\n"
           "            selected_block.physical_origin ==\n"
           "                katana::runtime::canonical_physical_address(live_target)) {\n"
           "            const auto& contract = *selected_block.aot_template;\n"
           "            const auto& mapping = contract.mapping;\n"
           "            if (live_target >= mapping.runtime_start) {\n"
           "                const auto offset = static_cast<std::uint64_t>(live_target) -\n"
           "                    mapping.runtime_start;\n"
           "                const auto source = static_cast<std::uint64_t>(\n"
           "                    mapping.source_start) + offset;\n"
           "                const auto callback_end = offset + descriptor.kernel_size;\n"
           "                const bool mutable_overlap = std::any_of(\n"
           "                    contract.mutable_ranges.begin(),\n"
           "                    contract.mutable_ranges.end(), [&](const auto& range) {\n"
           "                        const auto range_end =\n"
           "                            static_cast<std::uint64_t>(range.offset) + range.size;\n"
           "                        return offset < range_end && range.offset < callback_end;\n"
           "                    });\n"
           "                mapped_kernel = source == descriptor.kernel_address &&\n"
           "                    callback_end <= mapping.extent &&\n"
           "                    contract.validation_extent >= mapping.extent &&\n"
           "                    !mutable_overlap;\n"
           "            }\n"
           "        }\n"
           "        if (!static_kernel && !mapped_kernel)\n"
           "            return reject(FlagPollBatchRejectionStage::CandidateProvenance);\n"
           "        const ScopedCpuActiveBlockProvenance active_block_provenance(\n"
           "            cpu_, selected_block.virtual_start,\n"
           "            selected_block.physical_origin, selected_block.size);\n"
           "\n"
           "        const auto call_block =\n"
           "            executable_blocks_.find(descriptor.call_block_address);\n"
           "        const auto continuation =\n"
           "            executable_blocks_.find(descriptor.continuation_address);\n"
           "        const auto kernel =\n"
           "            executable_blocks_.find(descriptor.kernel_address);\n"
           "        if (call_block == executable_blocks_.end() ||\n"
           "            continuation == executable_blocks_.end() ||\n"
           "            kernel == executable_blocks_.end() ||\n"
           "            call_block->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.call_block_address) ||\n"
           "            continuation->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.continuation_address) ||\n"
           "            kernel->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.kernel_address) ||\n"
           "            call_block->second.size != descriptor.call_block_size ||\n"
           "            continuation->second.size != descriptor.continuation_size ||\n"
           "            kernel->second.size != descriptor.kernel_size ||\n"
           "            call_block->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::PureCpu ||\n"
           "            continuation->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush ||\n"
           "            kernel->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::PureCpu ||\n"
           "            call_block->second.maximum_guest_cycles !=\n"
           "                descriptor.call_block_guest_cycles ||\n"
           "            continuation->second.maximum_guest_cycles !=\n"
           "                descriptor.continuation_guest_cycles ||\n"
           "            kernel->second.maximum_guest_cycles !=\n"
           "                descriptor.kernel_guest_cycles ||\n"
           "            !provenance_matches(call_block->second.identity,\n"
           "                               descriptor.call_block_provenance) ||\n"
           "            !provenance_matches(continuation->second.identity,\n"
           "                               descriptor.continuation_provenance) ||\n"
           "            !provenance_matches(kernel->second.identity,\n"
           "                               descriptor.kernel_provenance) ||\n"
           "            !state_.code_tracker->dispatchable(call_block->second.identity) ||\n"
           "            !state_.code_tracker->dispatchable(continuation->second.identity) ||\n"
           "            !state_.code_tracker->dispatchable(kernel->second.identity))\n"
           "            return reject(FlagPollBatchRejectionStage::RegisteredBlocks);\n"
           "        const auto proves_direct_instruction_range = [&]\n"
           "                (const std::uint32_t address,\n"
           "                 const std::uint32_t physical_origin,\n"
           "                 const std::uint32_t size) {\n"
           "            if (size < 2u || (size & 1u) != 0u ||\n"
           "                size > 0x1'0000'0000ull - address ||\n"
           "                size > 0x1'0000'0000ull - physical_origin)\n"
           "                return false;\n"
           "            const auto last_address = static_cast<std::uint32_t>(\n"
           "                static_cast<std::uint64_t>(address) + size - 2u);\n"
           "            const auto last_physical = static_cast<std::uint32_t>(\n"
           "                static_cast<std::uint64_t>(physical_origin) + size - 2u);\n"
           "            try {\n"
           "                if (state_.address_space->instruction_translation_path(\n"
           "                        address, cpu_.privileged_mode()) !=\n"
           "                        katana::runtime::InstructionTranslationPath::Direct ||\n"
           "                    state_.address_space->instruction_translation_path(\n"
           "                        last_address, cpu_.privileged_mode()) !=\n"
           "                        katana::runtime::InstructionTranslationPath::Direct)\n"
           "                    return false;\n"
           "                const auto first = state_.address_space->inspect_translation(\n"
           "                    address, katana::runtime::TranslationAccess::Instruction,\n"
           "                    cpu_.privileged_mode());\n"
           "                const auto last = state_.address_space->inspect_translation(\n"
           "                    last_address,\n"
           "                    katana::runtime::TranslationAccess::Instruction,\n"
           "                    cpu_.privileged_mode());\n"
           "                if (katana::runtime::canonical_physical_address(\n"
           "                        first.physical_address) != physical_origin ||\n"
           "                    katana::runtime::canonical_physical_address(\n"
           "                        last.physical_address) != last_physical ||\n"
           "                    !first.no_mmu_fastpath || first.utlb_slot != 0xFFu ||\n"
           "                    !last.no_mmu_fastpath || last.utlb_slot != 0xFFu ||\n"
           "                    first.mmu_generation != last.mmu_generation ||\n"
           "                    !state_.address_space->prove_instruction_mapping(\n"
           "                        address, physical_origin, size,\n"
           "                        cpu_.privileged_mode()))\n"
           "                    return false;\n"
           "                const auto variant = katana::runtime::block_variant_key(\n"
           "                    state_.address_space->guard_for(\n"
           "                        address, cpu_.read_fpscr(), cpu_.privileged_mode()),\n"
           "                    active_block_variant_->runtime_generation);\n"
           "                return variant == *active_block_variant_;\n"
           "            } catch (...) {\n"
           "                return false;\n"
           "            }\n"
           "        };\n"
           "        if (!proves_direct_instruction_range(\n"
           "                descriptor.call_block_address,\n"
           "                call_block->second.physical_origin,\n"
           "                call_block->second.size) ||\n"
           "            !proves_direct_instruction_range(\n"
           "                live_target, selected_block.physical_origin,\n"
           "                selected_block.size) ||\n"
           "            !proves_direct_instruction_range(\n"
           "                descriptor.continuation_address,\n"
           "                continuation->second.physical_origin,\n"
           "                continuation->second.size))\n"
           "            return reject(FlagPollBatchRejectionStage::InstructionMapping);\n"
           "        if (cpu_.pr != descriptor.continuation_address)\n"
           "            return reject(FlagPollBatchRejectionStage::EntryRegisters);\n"
           "\n"
           "        const auto flag_address = static_cast<std::uint32_t>(\n"
           "            cpu_.r[descriptor.flag_base_register] +\n"
           "            static_cast<std::uint32_t>(descriptor.flag_displacement));\n"
           "        if ((flag_address & 3u) != 0u)\n"
           "            return reject(FlagPollBatchRejectionStage::Alignment);\n"
           "        const auto flag_read = prove_main_ram_translation(\n"
           "            cpu_, state_, flag_address, 4u,\n"
           "            katana::runtime::TranslationAccess::Read);\n"
           "        if (!flag_read || !flag_read->no_mmu_fastpath ||\n"
           "            flag_read->utlb_slot != 0xFFu ||\n"
           "            flag_read->mmu_generation != active_block_variant_->mmu_generation)\n"
           "            return reject(FlagPollBatchRejectionStage::RamTranslation);\n"
           "        const auto flag_backing = dreamcast_main_ram_backing_offset(\n"
           "            flag_read->physical_address, 4u);\n"
           "        if (!flag_backing)\n"
           "            return reject(FlagPollBatchRejectionStage::RamBacking);\n"
           "\n"
           "        const auto rounds_for_cycle_budget = [&](const std::uint64_t budget) {\n"
           "            return budget < descriptor.current_round_guest_cycles\n"
           "                ? std::uint64_t{0u}\n"
           "                : std::uint64_t{1u} +\n"
           "                    (budget - descriptor.current_round_guest_cycles) /\n"
           "                        descriptor.subsequent_round_guest_cycles;\n"
           "        };\n"
           "        auto admitted = rounds_for_cycle_budget(quantum);\n"
           "        const auto scheduler_cycle = state_.scheduler->current_cycle();\n"
           "        if (const auto event = state_.scheduler->next_event_cycle()) {\n"
           "            if (*event <= scheduler_cycle)\n"
           "                return reject(FlagPollBatchRejectionStage::SchedulerEventDue);\n"
           "            admitted = std::min(\n"
           "                admitted, rounds_for_cycle_budget(*event - scheduler_cycle - 1u));\n"
           "        }\n"
           "        if (const auto remaining = state_.scheduler->remaining_guest_cycles()) {\n"
           "            if (*remaining == 0u)\n"
           "                return reject(FlagPollBatchRejectionStage::SchedulerBudgetEmpty);\n"
           "            admitted = std::min(\n"
           "                admitted, rounds_for_cycle_budget(*remaining - 1u));\n"
           "        }\n"
           "        if (admitted == 0u)\n"
           "            return reject(FlagPollBatchRejectionStage::EmptyBatch);\n"
           "        const auto flag_value = state_.main_ram->read_u32(*flag_backing);\n"
           "        const bool complete = flag_value == descriptor.flag_expected_value;\n"
           "        if (complete) admitted = 1u;\n"
           "        const auto subsequent_rounds = admitted - 1u;\n"
           "        if (subsequent_rounds >\n"
           "                (std::numeric_limits<std::uint64_t>::max() -\n"
           "                 descriptor.current_round_guest_cycles) /\n"
           "                    descriptor.subsequent_round_guest_cycles ||\n"
           "            subsequent_rounds >\n"
           "                (std::numeric_limits<std::uint64_t>::max() -\n"
           "                 descriptor.current_round_instruction_count) /\n"
           "                    descriptor.subsequent_round_instruction_count)\n"
           "            return reject(FlagPollBatchRejectionStage::AccountingOverflow);\n"
           "        const auto batch_guest_cycles =\n"
           "            descriptor.current_round_guest_cycles +\n"
           "            subsequent_rounds * descriptor.subsequent_round_guest_cycles;\n"
           "        const auto batch_instructions =\n"
           "            static_cast<std::uint64_t>(\n"
           "                descriptor.current_round_instruction_count) +\n"
           "            subsequent_rounds * descriptor.subsequent_round_instruction_count;\n"
           "        if (batch_guest_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() - scheduler_cycle ||\n"
           "            batch_guest_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.total_guest_cycles ||\n"
           "            batch_guest_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.pending_guest_cycles ||\n"
           "            batch_instructions >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.attempted_guest_instructions ||\n"
           "            batch_instructions >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.retired_guest_instructions)\n"
           "            return reject(FlagPollBatchRejectionStage::CpuCounterOverflow);\n"
           "        if (!cpu_.memory.account_prevalidated_unobserved_accesses(\n"
           "                admitted, admitted))\n"
           "            return reject(FlagPollBatchRejectionStage::MemoryAccounting);\n"
           "\n"
           "        cpu_.r[descriptor.flag_value_register] = flag_value;\n"
           "        cpu_.pr = descriptor.continuation_address;\n"
           "        cpu_.t = complete;\n"
           "        cpu_.pc = complete ? descriptor.exit_address\n"
           "                           : descriptor.call_block_address;\n"
           "        cpu_.active_instruction_pc = descriptor.outer_branch_instruction_address;\n"
           "        cpu_.active_instruction_physical_pc =\n"
           "            continuation->second.physical_origin + 4u;\n"
           "        cpu_.attempted_guest_instructions += batch_instructions;\n"
           "        cpu_.retired_guest_instructions += batch_instructions;\n"
           "        cpu_.pending_guest_cycles += batch_guest_cycles;\n"
           "        ++flag_poll_batch_counters_.admissions;\n"
           "        flag_poll_batch_counters_.batched_rounds += admitted;\n"
           "        flag_poll_batch_counters_.batched_guest_cycles += batch_guest_cycles;\n"
           "        return true;\n"
           "    }\n"
           "    bool try_composite_callback_batch(\n"
           "            const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "            const " +
           entry_namespace +
           "::CompositeCallbackBatchDescriptor& descriptor) {\n"
           "        constexpr std::uint64_t quantum = 131'072u;\n"
           "        const auto reject = [](const std::string_view stage) {\n"
           "            return composite_callback_batch_rejected(stage);\n"
           "        };\n"
           "        const auto runtime_state_allows_batch = [&] {\n"
           "            return (cpu_.interrupts_blocked() || cpu_.interrupt_mask() == 15u) &&\n"
           "                cpu_.memory.watchpoint_count() == 0u &&\n"
           "                !cpu_.memory.has_trace_handler() &&\n"
           "                !cpu_.memory.has_mmio_trace_handler() &&\n"
           "                !cpu_.memory.mmio_access_tracking_enabled() &&\n"
           "                !cpu_.memory.has_guest_memory_access_sink() &&\n"
           "                cpu_.memory.guest_write_observer_allows_prevalidated_linear_writes() &&\n"
           "                cpu_.memory.lookup_mode() ==\n"
           "                    katana::runtime::MemoryLookupMode::Indexed &&\n"
           "                state_.main_ram &&\n"
           "                state_.main_ram->size() ==\n"
           "                    katana::runtime::dreamcast_main_ram_size &&\n"
           "                state_.address_space && cpu_.address_space &&\n"
           "                cpu_.address_space.get() == state_.address_space.get() &&\n"
           "                state_.address_space->mode() ==\n"
           "                    katana::runtime::AddressTranslationMode::NoMmu &&\n"
           "                state_.scheduler && state_.code_tracker &&\n"
           "                state_.runtime_blocks && state_.module_catalog &&\n"
           "                state_.disc_load_transactions &&\n"
           "                !state_.disc_load_transactions->transaction_active() &&\n"
           "                active_block_variant_.has_value();\n"
           "        };\n"
           "        if (descriptor.kind == " +
           entry_namespace +
           "::CompositeCallbackBatchKind::FlagPollEqualImmediate)\n"
           "            return try_composite_callback_flag_poll_batch(\n"
           "                selected_block, descriptor);\n"
           "        if (!composite_callback_batching_enabled_ ||\n"
           "            descriptor.kind != " +
           entry_namespace +
           "::CompositeCallbackBatchKind::MemoryCopy ||\n"
           "            descriptor.call_instruction_address !=\n"
           "                descriptor.call_block_address + 4u ||\n"
           "            descriptor.continuation_address !=\n"
           "                descriptor.call_instruction_address + 4u ||\n"
           "            descriptor.exit_address != descriptor.continuation_address + 8u ||\n"
           "            descriptor.kernel_return_address != descriptor.kernel_address + 12u ||\n"
           "            descriptor.source_load_instruction_address !=\n"
           "                descriptor.kernel_address + 2u ||\n"
           "            descriptor.target_store_instruction_address !=\n"
           "                descriptor.kernel_address + 6u ||\n"
           "            descriptor.outer_branch_instruction_address !=\n"
           "                descriptor.continuation_address + 4u ||\n"
           "            descriptor.call_block_size != 8u ||\n"
           "            descriptor.continuation_size != 8u ||\n"
           "            descriptor.kernel_size != 12u ||\n"
           "            descriptor.kernel_return_size != 4u ||\n"
           "            descriptor.current_round_instruction_count != 12u ||\n"
           "            descriptor.subsequent_round_instruction_count != 16u ||\n"
           "            descriptor.call_block_guest_cycles == 0u ||\n"
           "            descriptor.continuation_guest_cycles == 0u ||\n"
           "            descriptor.kernel_guest_cycles == 0u ||\n"
           "            descriptor.kernel_return_guest_cycles == 0u ||\n"
           "            descriptor.current_round_guest_cycles !=\n"
           "                descriptor.continuation_guest_cycles +\n"
           "                    descriptor.kernel_guest_cycles +\n"
           "                    descriptor.kernel_return_guest_cycles ||\n"
           "            descriptor.subsequent_round_guest_cycles !=\n"
           "                descriptor.current_round_guest_cycles +\n"
           "                    descriptor.call_block_guest_cycles ||\n"
           "            descriptor.current_round_guest_cycles > quantum ||\n"
           "            descriptor.subsequent_round_guest_cycles > quantum ||\n"
           "            descriptor.callback_register >= cpu_.r.size() ||\n"
           "            descriptor.destination_register >= cpu_.r.size() ||\n"
           "            descriptor.count_register >= cpu_.r.size() ||\n"
           "            descriptor.limit_register >= cpu_.r.size())\n"
           "            return reject(\"descriptor\");\n"
           "        const std::array variable_registers{\n"
           "            descriptor.callback_register, descriptor.destination_register,\n"
           "            descriptor.count_register, descriptor.limit_register};\n"
           "        auto sorted_registers = variable_registers;\n"
           "        std::sort(sorted_registers.begin(), sorted_registers.end());\n"
           "        if (std::adjacent_find(sorted_registers.begin(),\n"
           "                               sorted_registers.end()) !=\n"
           "                sorted_registers.end() ||\n"
           "            std::any_of(variable_registers.begin(),\n"
           "                        variable_registers.end(), [](const auto reg) {\n"
           "                            return reg == 0u || reg == 4u || reg == 5u ||\n"
           "                                   reg == 6u || reg == 15u;\n"
           "                        }))\n"
           "            return reject(\"register-contract\");\n"
           "        if (!runtime_state_allows_batch())\n"
           "            return reject(\"runtime-state\");\n"
           "        if (cpu_.pending_guest_cycles != 0u)\n"
           "            katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "        if (cpu_.pending_guest_cycles != 0u ||\n"
           "            !runtime_state_allows_batch())\n"
           "            return reject(\"preflush\");\n"
           "\n"
           "        const auto provenance_matches = [](const std::string_view actual,\n"
           "                                                  const std::string_view expected) {\n"
           "            if (actual == expected || actual.ends_with(expected)) return true;\n"
           "            constexpr std::string_view mmu_suffix = \"-mmu-variant\";\n"
           "            if (actual.size() < expected.size() + mmu_suffix.size() ||\n"
           "                !actual.ends_with(\"-mmu-variant\")) return false;\n"
           "            const auto expected_offset =\n"
           "                actual.size() - mmu_suffix.size() - expected.size();\n"
           "            return\n"
           "                (actual.size() == expected.size() + mmu_suffix.size() &&\n"
           "                 actual.starts_with(expected)) ||\n"
           "                actual.substr(expected_offset, expected.size()) == expected;\n"
           "        };\n"
           "        const auto live_target = cpu_.r[descriptor.callback_register];\n"
           "        if (cpu_.pc != live_target || selected_block.size != descriptor.kernel_size ||\n"
           "            selected_block.end_kind !=\n"
           "                katana::runtime::BlockEndKind::ConditionalBranch)\n"
           "            return reject(\"live-target\");\n"
           "        const bool static_kernel =\n"
           "            !selected_block.runtime_registered && !selected_block.aot_template &&\n"
           "            live_target == descriptor.kernel_address &&\n"
           "            selected_block.virtual_start == descriptor.kernel_address &&\n"
           "            selected_block.physical_origin ==\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.kernel_address) &&\n"
           "            provenance_matches(selected_block.provenance,\n"
           "                               descriptor.kernel_provenance);\n"
           "        bool mapped_kernel = false;\n"
           "        if (selected_block.aot_template &&\n"
           "            selected_block.virtual_start == live_target &&\n"
           "            selected_block.physical_origin ==\n"
           "                katana::runtime::canonical_physical_address(live_target)) {\n"
           "            const auto& contract = *selected_block.aot_template;\n"
           "            const auto& mapping = contract.mapping;\n"
           "            if (live_target >= mapping.runtime_start) {\n"
           "                const auto offset = static_cast<std::uint64_t>(live_target) -\n"
           "                    mapping.runtime_start;\n"
           "                const auto source = static_cast<std::uint64_t>(\n"
           "                    mapping.source_start) + offset;\n"
           "                const auto callback_end = offset + descriptor.kernel_size +\n"
           "                    descriptor.kernel_return_size;\n"
           "                const bool mutable_overlap = std::any_of(\n"
           "                    contract.mutable_ranges.begin(),\n"
           "                    contract.mutable_ranges.end(), [&](const auto& range) {\n"
           "                        const auto range_end =\n"
           "                            static_cast<std::uint64_t>(range.offset) + range.size;\n"
           "                        return offset < range_end && range.offset < callback_end;\n"
           "                    });\n"
           "                mapped_kernel = source == descriptor.kernel_address &&\n"
           "                    callback_end <= mapping.extent &&\n"
           "                    contract.validation_extent >= mapping.extent &&\n"
           "                    !mutable_overlap;\n"
           "            }\n"
           "        }\n"
           "        if (!static_kernel && !mapped_kernel)\n"
           "            return reject(\"candidate-provenance\");\n"
           "        const ScopedCpuActiveBlockProvenance active_block_provenance(\n"
           "            cpu_, selected_block.virtual_start,\n"
           "            selected_block.physical_origin, selected_block.size);\n"
           "\n"
           "        const auto call_block =\n"
           "            executable_blocks_.find(descriptor.call_block_address);\n"
           "        const auto continuation =\n"
           "            executable_blocks_.find(descriptor.continuation_address);\n"
           "        const auto kernel =\n"
           "            executable_blocks_.find(descriptor.kernel_address);\n"
           "        const auto kernel_return =\n"
           "            executable_blocks_.find(descriptor.kernel_return_address);\n"
           "        if (call_block == executable_blocks_.end() ||\n"
           "            continuation == executable_blocks_.end() ||\n"
           "            kernel == executable_blocks_.end() ||\n"
           "            kernel_return == executable_blocks_.end() ||\n"
           "            call_block->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.call_block_address) ||\n"
           "            continuation->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.continuation_address) ||\n"
           "            kernel->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.kernel_address) ||\n"
           "            kernel_return->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.kernel_return_address) ||\n"
           "            call_block->second.size != descriptor.call_block_size ||\n"
           "            continuation->second.size != descriptor.continuation_size ||\n"
           "            kernel->second.size != descriptor.kernel_size ||\n"
           "            kernel_return->second.size != descriptor.kernel_return_size ||\n"
           "            call_block->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::PureCpu ||\n"
           "            continuation->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::PureCpu ||\n"
           "            (kernel->second.timing_class !=\n"
           "                 katana::runtime::ExecutableBlockTimingClass::LinearRamOnly &&\n"
           "             kernel->second.timing_class !=\n"
           "                 katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush) ||\n"
           "            kernel_return->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::PureCpu ||\n"
           "            call_block->second.maximum_guest_cycles !=\n"
           "                descriptor.call_block_guest_cycles ||\n"
           "            continuation->second.maximum_guest_cycles !=\n"
           "                descriptor.continuation_guest_cycles ||\n"
           "            kernel->second.maximum_guest_cycles !=\n"
           "                descriptor.kernel_guest_cycles ||\n"
           "            kernel_return->second.maximum_guest_cycles !=\n"
           "                descriptor.kernel_return_guest_cycles ||\n"
           "            !provenance_matches(call_block->second.identity,\n"
           "                               descriptor.call_block_provenance) ||\n"
           "            !provenance_matches(continuation->second.identity,\n"
           "                               descriptor.continuation_provenance) ||\n"
           "            !provenance_matches(kernel->second.identity,\n"
           "                               descriptor.kernel_provenance) ||\n"
           "            !provenance_matches(kernel_return->second.identity,\n"
           "                               descriptor.kernel_return_provenance) ||\n"
           "            !state_.code_tracker->dispatchable(call_block->second.identity) ||\n"
           "            !state_.code_tracker->dispatchable(continuation->second.identity) ||\n"
           "            !state_.code_tracker->dispatchable(kernel->second.identity) ||\n"
           "            !state_.code_tracker->dispatchable(kernel_return->second.identity))\n"
           "            return reject(\"registered-blocks\");\n"
           "        const auto proves_instruction_block = [&](const std::uint32_t address,\n"
           "                                                    const auto& registration) {\n"
           "            try {\n"
           "                if (state_.address_space->instruction_translation_path(\n"
           "                        address, cpu_.privileged_mode()) !=\n"
           "                    katana::runtime::InstructionTranslationPath::Direct)\n"
           "                    return false;\n"
           "                const auto inspected = state_.address_space->inspect_translation(\n"
           "                    address, katana::runtime::TranslationAccess::Instruction,\n"
           "                    cpu_.privileged_mode());\n"
           "                if (katana::runtime::canonical_physical_address(\n"
           "                        inspected.physical_address) !=\n"
           "                        registration.physical_origin ||\n"
           "                    !inspected.no_mmu_fastpath || inspected.utlb_slot != 0xFFu ||\n"
           "                    !state_.address_space->prove_instruction_mapping(\n"
           "                        address, registration.physical_origin, registration.size,\n"
           "                        cpu_.privileged_mode()))\n"
           "                    return false;\n"
           "                const auto variant = katana::runtime::block_variant_key(\n"
           "                    state_.address_space->guard_for(\n"
           "                        address, cpu_.read_fpscr(), cpu_.privileged_mode()),\n"
           "                    active_block_variant_->runtime_generation);\n"
           "                return variant == *active_block_variant_;\n"
           "            } catch (...) {\n"
           "                return false;\n"
           "            }\n"
           "        };\n"
           "        if (!proves_instruction_block(descriptor.call_block_address,\n"
           "                                      call_block->second) ||\n"
           "            !proves_instruction_block(descriptor.continuation_address,\n"
           "                                      continuation->second) ||\n"
           "            !proves_instruction_block(descriptor.kernel_address,\n"
           "                                      kernel->second) ||\n"
           "            !proves_instruction_block(descriptor.kernel_return_address,\n"
           "                                      kernel_return->second))\n"
           "            return reject(\"instruction-mapping\");\n"
           "\n"
           "        if (cpu_.r[5u] != cpu_.r[15u] || cpu_.r[6u] != 4u ||\n"
           "            cpu_.r[4u] != cpu_.r[descriptor.destination_register] ||\n"
           "            cpu_.pr != descriptor.continuation_address)\n"
           "            return reject(\"entry-registers\");\n"
           "        const auto signed_count = static_cast<std::int64_t>(\n"
           "            static_cast<std::int32_t>(cpu_.r[descriptor.count_register]));\n"
           "        const auto signed_limit = static_cast<std::int64_t>(\n"
           "            static_cast<std::int32_t>(cpu_.r[descriptor.limit_register]));\n"
           "        if (signed_count >= signed_limit)\n"
           "            return reject(\"signed-loop-range\");\n"
           "        auto admitted = static_cast<std::uint64_t>(signed_limit - signed_count);\n"
           "        const auto rounds_for_cycle_budget = [&](const std::uint64_t budget) {\n"
           "            return budget < descriptor.current_round_guest_cycles\n"
           "                ? std::uint64_t{0u}\n"
           "                : std::uint64_t{1u} +\n"
           "                    (budget - descriptor.current_round_guest_cycles) /\n"
           "                        descriptor.subsequent_round_guest_cycles;\n"
           "        };\n"
           "        admitted = std::min(admitted, rounds_for_cycle_budget(quantum));\n"
           "        const auto scheduler_cycle = state_.scheduler->current_cycle();\n"
           "        if (const auto event = state_.scheduler->next_event_cycle()) {\n"
           "            if (*event <= scheduler_cycle)\n"
           "                return reject(\"scheduler-event-due\");\n"
           "            admitted = std::min(\n"
           "                admitted, rounds_for_cycle_budget(*event - scheduler_cycle - 1u));\n"
           "        }\n"
           "        if (const auto remaining = state_.scheduler->remaining_guest_cycles()) {\n"
           "            if (*remaining == 0u)\n"
           "                return reject(\"scheduler-budget-empty\");\n"
           "            admitted = std::min(\n"
           "                admitted, rounds_for_cycle_budget(*remaining - 1u));\n"
           "        }\n"
           "        if (admitted == 0u ||\n"
           "            admitted > std::numeric_limits<std::size_t>::max())\n"
           "            return reject(\"empty-batch\");\n"
           "        const auto word_count = static_cast<std::size_t>(admitted);\n"
           "        if (word_count > std::numeric_limits<std::size_t>::max() / 4u ||\n"
           "            word_count > std::numeric_limits<std::size_t>::max() / 2u)\n"
           "            return reject(\"target-size-overflow\");\n"
           "        const auto target_size = word_count * 4u;\n"
           "        const auto source_address = cpu_.r[15u];\n"
           "        const auto target_address = cpu_.r[4u];\n"
           "        if ((source_address & 3u) != 0u || (target_address & 3u) != 0u)\n"
           "            return reject(\"alignment\");\n"
           "        const auto source_read = prove_main_ram_translation(\n"
           "            cpu_, state_, source_address, 4u,\n"
           "            katana::runtime::TranslationAccess::Read);\n"
           "        const auto target_write = prove_main_ram_translation(\n"
           "            cpu_, state_, target_address, target_size,\n"
           "            katana::runtime::TranslationAccess::Write);\n"
           "        if (!source_read || !target_write ||\n"
           "            !source_read->no_mmu_fastpath || !target_write->no_mmu_fastpath ||\n"
           "            source_read->utlb_slot != 0xFFu ||\n"
           "            target_write->utlb_slot != 0xFFu)\n"
           "            return reject(\"ram-translation\");\n"
           "        const auto source_backing = dreamcast_main_ram_backing_offset(\n"
           "            source_read->physical_address, 4u);\n"
           "        const auto target_backing = dreamcast_main_ram_backing_offset(\n"
           "            target_write->physical_address, target_size);\n"
           "        if (!source_backing || !target_backing ||\n"
           "            (*target_backing < *source_backing + 4u &&\n"
           "             *source_backing < *target_backing + target_size) ||\n"
           "            dreamcast_main_ram_backing_is_executable(\n"
           "                *state_.code_tracker, *target_backing, target_size))\n"
           "            return reject(\"ram-alias-or-code-overlap\");\n"
           "        const auto target_physical = static_cast<std::uint32_t>(\n"
           "            katana::runtime::dreamcast_main_ram_area_bases.front() +\n"
           "            *target_backing);\n"
           "        if (state_.runtime_blocks->may_overlap_active_physical(\n"
           "                target_physical, target_size))\n"
           "            return reject(\"runtime-code-overlap\");\n"
           "\n"
           "        const auto subsequent_rounds = admitted - 1u;\n"
           "        if (subsequent_rounds >\n"
           "                (std::numeric_limits<std::uint64_t>::max() -\n"
           "                 descriptor.current_round_guest_cycles) /\n"
           "                    descriptor.subsequent_round_guest_cycles ||\n"
           "            subsequent_rounds >\n"
           "                (std::numeric_limits<std::uint64_t>::max() -\n"
           "                 descriptor.current_round_instruction_count) /\n"
           "                    descriptor.subsequent_round_instruction_count)\n"
           "            return reject(\"accounting-overflow\");\n"
           "        const auto batch_guest_cycles =\n"
           "            descriptor.current_round_guest_cycles +\n"
           "            subsequent_rounds * descriptor.subsequent_round_guest_cycles;\n"
           "        const auto batch_instructions =\n"
           "            static_cast<std::uint64_t>(\n"
           "                descriptor.current_round_instruction_count) +\n"
           "            subsequent_rounds * descriptor.subsequent_round_instruction_count;\n"
           "        if (batch_guest_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() - scheduler_cycle ||\n"
           "            batch_guest_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.total_guest_cycles ||\n"
           "            batch_instructions >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.attempted_guest_instructions ||\n"
           "            batch_instructions >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.retired_guest_instructions)\n"
           "            return reject(\"cpu-counter-overflow\");\n"
           "        const auto pattern = state_.main_ram->read_u32(*source_backing);\n"
           "        auto prepared_pattern =\n"
           "            cpu_.memory.prepare_prevalidated_linear_u32_pattern(\n"
           "                target_write->physical_address, word_count, pattern,\n"
           "                katana::runtime::CodeWriteSource::Cpu, word_count,\n"
           "                word_count * 2u);\n"
           "        if (!prepared_pattern)\n"
           "            return reject(\"memory-prepare\");\n"
           "        accept_batch_guest_cycles_before_commit(\n"
           "            batch_guest_cycles, AtomicBatchCommitKind::CompositeCallback);\n"
           "        cpu_.memory.commit_prepared_linear_u32_pattern(\n"
           "            std::move(*prepared_pattern));\n"
           "\n"
           "        const auto final_count = static_cast<std::uint32_t>(\n"
           "            cpu_.r[descriptor.count_register] + admitted);\n"
           "        const auto final_destination = static_cast<std::uint32_t>(\n"
           "            static_cast<std::uint64_t>(target_address) + target_size);\n"
           "        const bool complete = admitted ==\n"
           "            static_cast<std::uint64_t>(signed_limit - signed_count);\n"
           "        cpu_.r[0u] = pattern;\n"
           "        cpu_.r[4u] = final_destination;\n"
           "        cpu_.r[5u] = source_address + 4u;\n"
           "        cpu_.r[6u] = 0u;\n"
           "        cpu_.r[descriptor.destination_register] = final_destination;\n"
           "        cpu_.r[descriptor.count_register] = final_count;\n"
           "        cpu_.pr = descriptor.continuation_address;\n"
           "        cpu_.t = complete;\n"
           "        cpu_.pc = complete ? descriptor.exit_address\n"
           "                           : descriptor.call_block_address;\n"
           "        cpu_.active_instruction_pc = descriptor.outer_branch_instruction_address;\n"
           "        cpu_.active_instruction_physical_pc =\n"
           "            continuation->second.physical_origin + 4u;\n"
           "        cpu_.attempted_guest_instructions += batch_instructions;\n"
           "        cpu_.retired_guest_instructions += batch_instructions;\n"
           "        trace_composite_callback_batch_admission(admitted);\n"
           "        return true;\n"
           "    }\n"
           "    bool try_memory_fill_loop_batch(\n"
           "            const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "            const " +
           entry_namespace +
           "::MemoryFillLoopBatchDescriptor& descriptor) {\n"
           "        constexpr std::uint64_t quantum = 131'072u;\n"
           "        const auto body_guest_cycles = descriptor.round_guest_cycles >\n"
           "                descriptor.guard_guest_cycles\n"
           "            ? descriptor.round_guest_cycles - descriptor.guard_guest_cycles\n"
           "            : 0u;\n"
           "        const auto runtime_state_allows_batch = [&] {\n"
           "            return (cpu_.interrupts_blocked() || cpu_.interrupt_mask() == 15u) &&\n"
           "                cpu_.memory.watchpoint_count() == 0u &&\n"
           "                !cpu_.memory.has_trace_handler() &&\n"
           "                !cpu_.memory.has_mmio_trace_handler() &&\n"
           "                !cpu_.memory.mmio_access_tracking_enabled() &&\n"
           "                !cpu_.memory.has_guest_memory_access_sink() &&\n"
           "                cpu_.memory.guest_write_observer_allows_prevalidated_linear_writes() &&\n"
           "                cpu_.memory.lookup_mode() ==\n"
           "                    katana::runtime::MemoryLookupMode::Indexed &&\n"
           "                state_.main_ram &&\n"
           "                state_.main_ram->size() ==\n"
           "                    katana::runtime::dreamcast_main_ram_size &&\n"
           "                state_.address_space && cpu_.address_space &&\n"
           "                cpu_.address_space.get() == state_.address_space.get() &&\n"
           "                state_.scheduler && state_.code_tracker &&\n"
           "                state_.runtime_blocks && state_.module_catalog &&\n"
           "                state_.disc_load_transactions &&\n"
           "                !state_.disc_load_transactions->transaction_active() &&\n"
           "                active_block_variant_.has_value();\n"
           "        };\n"
           "        if (!memory_fill_loop_batching_enabled_ ||\n"
           "            descriptor.body_address != descriptor.store_instruction_address ||\n"
           "            descriptor.increment_instruction_address !=\n"
           "                descriptor.body_address + 2u ||\n"
           "            descriptor.limit_load_instruction_address !=\n"
           "                descriptor.guard_address ||\n"
           "            descriptor.compare_instruction_address !=\n"
           "                descriptor.guard_address + 2u ||\n"
           "            descriptor.branch_instruction_address !=\n"
           "                descriptor.guard_address + 4u ||\n"
           "            descriptor.exit_address != descriptor.guard_address + 6u ||\n"
           "            descriptor.guard_size != 6u || descriptor.body_size != 4u ||\n"
           "            descriptor.store_width != 1u || descriptor.step != 1u ||\n"
           "            descriptor.round_instruction_count != 5u ||\n"
           "            descriptor.guard_instruction_count != 3u ||\n"
           "            descriptor.store_guest_cycles == 0u ||\n"
           "            descriptor.guard_guest_cycles == 0u ||\n"
           "            descriptor.guard_guest_cycles >= descriptor.round_guest_cycles ||\n"
           "            descriptor.round_guest_cycles > quantum ||\n"
           "            body_guest_cycles < descriptor.store_guest_cycles ||\n"
           "            descriptor.cursor_register >= cpu_.r.size() ||\n"
           "            descriptor.fill_register >= cpu_.r.size() ||\n"
           "            descriptor.limit_register >= cpu_.r.size() ||\n"
           "            descriptor.limit_pointer_register >= cpu_.r.size())\n"
           "            return false;\n"
           "        const bool entry_is_guard =\n"
           "            selected_block.virtual_start == descriptor.guard_address;\n"
           "        const bool entry_is_body =\n"
           "            selected_block.virtual_start == descriptor.body_address;\n"
           "        if (entry_is_guard == entry_is_body || selected_block.runtime_registered ||\n"
           "            selected_block.aot_template)\n"
           "            return false;\n"
           "        const auto provenance_matches = [](const std::string_view actual,\n"
           "                                                  const std::string_view expected) {\n"
           "            return actual == expected ||\n"
           "                (actual.size() == expected.size() + 12u &&\n"
           "                 actual.starts_with(expected) &&\n"
           "                 actual.ends_with(\"-mmu-variant\"));\n"
           "        };\n"
           "        if (entry_is_guard) {\n"
           "            if (selected_block.physical_origin !=\n"
           "                    katana::runtime::canonical_physical_address(\n"
           "                        descriptor.guard_address) ||\n"
           "                selected_block.size != descriptor.guard_size ||\n"
           "                selected_block.end_kind !=\n"
           "                    katana::runtime::BlockEndKind::ConditionalBranch ||\n"
           "                !provenance_matches(\n"
           "                    selected_block.provenance, descriptor.guard_provenance))\n"
           "                return false;\n"
           "        } else if (selected_block.physical_origin !=\n"
           "                       katana::runtime::canonical_physical_address(\n"
           "                           descriptor.body_address) ||\n"
           "                   selected_block.size != descriptor.body_size ||\n"
           "                   selected_block.end_kind !=\n"
           "                       katana::runtime::BlockEndKind::Fallthrough ||\n"
           "                   !provenance_matches(\n"
           "                       selected_block.provenance, descriptor.body_provenance)) {\n"
           "            return false;\n"
           "        }\n"
           "        const ScopedCpuActiveBlockProvenance active_block_provenance(\n"
           "            cpu_, selected_block.virtual_start,\n"
           "            selected_block.physical_origin, selected_block.size);\n"
           "        if (!runtime_state_allows_batch()) return false;\n"
           "        if (cpu_.pending_guest_cycles != 0u)\n"
           "            katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "        if (cpu_.pending_guest_cycles != 0u ||\n"
           "            !runtime_state_allows_batch())\n"
           "            return false;\n"
           "        const auto guard = executable_blocks_.find(descriptor.guard_address);\n"
           "        const auto body = executable_blocks_.find(descriptor.body_address);\n"
           "        if (guard == executable_blocks_.end() ||\n"
           "            body == executable_blocks_.end() ||\n"
           "            guard->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.guard_address) ||\n"
           "            body->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.body_address) ||\n"
           "            guard->second.size != descriptor.guard_size ||\n"
           "            body->second.size != descriptor.body_size ||\n"
           "            guard->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush ||\n"
           "            body->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush ||\n"
           "            guard->second.maximum_guest_cycles !=\n"
           "                descriptor.guard_guest_cycles ||\n"
           "            body->second.maximum_guest_cycles != body_guest_cycles ||\n"
           "            !state_.code_tracker->dispatchable(guard->second.identity) ||\n"
           "            !state_.code_tracker->dispatchable(body->second.identity))\n"
           "            return false;\n"
           "        const auto proves_instruction_block = [&](const std::uint32_t address,\n"
           "                                                    const auto& registration) {\n"
           "            try {\n"
           "                if (state_.address_space->instruction_translation_path(\n"
           "                        address, cpu_.privileged_mode()) !=\n"
           "                    katana::runtime::InstructionTranslationPath::Direct)\n"
           "                    return false;\n"
           "                const auto inspected = state_.address_space->inspect_translation(\n"
           "                    address, katana::runtime::TranslationAccess::Instruction,\n"
           "                    cpu_.privileged_mode());\n"
           "                if (katana::runtime::canonical_physical_address(\n"
           "                        inspected.physical_address) !=\n"
           "                        registration.physical_origin ||\n"
           "                    !state_.address_space->prove_instruction_mapping(\n"
           "                        address, registration.physical_origin, registration.size,\n"
           "                        cpu_.privileged_mode()))\n"
           "                    return false;\n"
           "                const auto variant = katana::runtime::block_variant_key(\n"
           "                    state_.address_space->guard_for(\n"
           "                        address, cpu_.read_fpscr(), cpu_.privileged_mode()),\n"
           "                    active_block_variant_->runtime_generation);\n"
           "                return variant == *active_block_variant_;\n"
           "            } catch (...) {\n"
           "                return false;\n"
           "            }\n"
           "        };\n"
           "        if (!proves_instruction_block(descriptor.guard_address, guard->second) ||\n"
           "            !proves_instruction_block(descriptor.body_address, body->second))\n"
           "            return false;\n"
           "        const auto limit_pointer = cpu_.r[descriptor.limit_pointer_register];\n"
           "        if ((limit_pointer & 3u) != 0u) return false;\n"
           "        const auto limit_read = prove_main_ram_translation(\n"
           "            cpu_, state_, limit_pointer, 4u,\n"
           "            katana::runtime::TranslationAccess::Read);\n"
           "        if (!limit_read || !limit_read->no_mmu_fastpath ||\n"
           "            limit_read->utlb_slot != 0xFFu)\n"
           "            return false;\n"
           "        const auto limit_backing = dreamcast_main_ram_backing_offset(\n"
           "            limit_read->physical_address, 4u);\n"
           "        if (!limit_backing) return false;\n"
           "        const auto limit = state_.main_ram->read_u32(*limit_backing);\n"
           "        const auto cursor = cpu_.r[descriptor.cursor_register];\n"
           "        if (cursor >= limit) return false;\n"
           "        const auto distance = static_cast<std::uint64_t>(limit) - cursor;\n"
           "        const auto prefix_guest_cycles =\n"
           "            entry_is_guard ? descriptor.guard_guest_cycles : 0u;\n"
           "        if (prefix_guest_cycles >= quantum) return false;\n"
           "        auto admitted = std::min(\n"
           "            distance,\n"
           "            (quantum - prefix_guest_cycles) /\n"
           "                descriptor.round_guest_cycles);\n"
           "        const auto scheduler_cycle = state_.scheduler->current_cycle();\n"
           "        if (const auto event = state_.scheduler->next_event_cycle()) {\n"
           "            if (*event <= scheduler_cycle) return false;\n"
           "            const auto available = *event - scheduler_cycle - 1u;\n"
           "            if (prefix_guest_cycles > available) return false;\n"
           "            admitted = std::min(\n"
           "                admitted,\n"
           "                (available - prefix_guest_cycles) /\n"
           "                              descriptor.round_guest_cycles);\n"
           "        }\n"
           "        if (const auto remaining = state_.scheduler->remaining_guest_cycles()) {\n"
           "            if (*remaining <= prefix_guest_cycles) return false;\n"
           "            admitted = std::min(\n"
           "                admitted,\n"
           "                (*remaining - prefix_guest_cycles - 1u) /\n"
           "                              descriptor.round_guest_cycles);\n"
           "        }\n"
           "        if (admitted == 0u || admitted >\n"
           "                std::numeric_limits<std::size_t>::max())\n"
           "            return false;\n"
           "        const auto fill_size = static_cast<std::size_t>(admitted);\n"
           "        if (entry_is_guard &&\n"
           "            fill_size == std::numeric_limits<std::size_t>::max())\n"
           "            return false;\n"
           "        const auto synthesized_limit_reads =\n"
           "            fill_size + (entry_is_guard ? 1u : 0u);\n"
           "        if (fill_size >\n"
           "                (std::numeric_limits<std::size_t>::max() -\n"
           "                 (entry_is_guard ? 1u : 0u)) /\n"
           "                    2u)\n"
           "            return false;\n"
           "        const auto synthesized_indexed_region_hits =\n"
           "            fill_size * 2u + (entry_is_guard ? 1u : 0u);\n"
           "        const auto target_write = prove_main_ram_translation(\n"
           "            cpu_, state_, cursor, fill_size,\n"
           "            katana::runtime::TranslationAccess::Write);\n"
           "        if (!target_write || !target_write->no_mmu_fastpath ||\n"
           "            target_write->utlb_slot != 0xFFu)\n"
           "            return false;\n"
           "        const auto target_backing = dreamcast_main_ram_backing_offset(\n"
           "            target_write->physical_address, fill_size);\n"
           "        if (!target_backing ||\n"
           "            (*target_backing < *limit_backing + 4u &&\n"
           "             *limit_backing < *target_backing + fill_size) ||\n"
           "            dreamcast_main_ram_backing_is_executable(\n"
           "                *state_.code_tracker, *target_backing, fill_size))\n"
           "            return false;\n"
           "        const auto backing_physical = static_cast<std::uint32_t>(\n"
           "            katana::runtime::dreamcast_main_ram_area_bases.front() +\n"
           "            *target_backing);\n"
           "        if (state_.runtime_blocks->may_overlap_active_physical(\n"
           "                backing_physical, fill_size))\n"
           "            return false;\n"
           "        if (admitted > std::numeric_limits<std::uint64_t>::max() /\n"
           "                           descriptor.round_guest_cycles ||\n"
           "            admitted > std::numeric_limits<std::uint64_t>::max() /\n"
           "                           descriptor.round_instruction_count)\n"
           "            return false;\n"
           "        const auto rounds_guest_cycles =\n"
           "            admitted * descriptor.round_guest_cycles;\n"
           "        const auto batch_guest_cycles =\n"
           "            prefix_guest_cycles + rounds_guest_cycles;\n"
           "        const auto batch_instructions =\n"
           "            admitted * descriptor.round_instruction_count +\n"
           "            (entry_is_guard ? descriptor.guard_instruction_count : 0u);\n"
           "        if (batch_guest_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() - scheduler_cycle ||\n"
           "            batch_guest_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.total_guest_cycles ||\n"
           "            batch_instructions >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.attempted_guest_instructions ||\n"
           "            batch_instructions >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.retired_guest_instructions)\n"
           "            return false;\n"
           "        const auto fill_value =\n"
           "            static_cast<std::uint8_t>(cpu_.r[descriptor.fill_register]);\n"
           "        auto prepared_fill = cpu_.memory.prepare_prevalidated_linear_fill(\n"
           "            target_write->physical_address, fill_size, fill_value,\n"
           "            katana::runtime::CodeWriteSource::Cpu,\n"
           "            synthesized_limit_reads,\n"
           "            synthesized_indexed_region_hits);\n"
           "        if (!prepared_fill)\n"
           "            return false;\n"
           "        accept_batch_guest_cycles_before_commit(\n"
           "            batch_guest_cycles, AtomicBatchCommitKind::MemoryFill);\n"
           "        cpu_.memory.commit_prepared_linear_fill(std::move(*prepared_fill));\n"
           "        const auto final_cursor =\n"
           "            static_cast<std::uint32_t>(cursor + admitted);\n"
           "        const bool complete = admitted == distance;\n"
           "        cpu_.r[descriptor.cursor_register] = final_cursor;\n"
           "        cpu_.r[descriptor.limit_register] = limit;\n"
           "        cpu_.t = complete;\n"
           "        cpu_.pc = complete ? descriptor.exit_address : descriptor.body_address;\n"
           "        cpu_.active_instruction_pc = descriptor.branch_instruction_address;\n"
           "        cpu_.active_instruction_physical_pc =\n"
           "            guard->second.physical_origin + 4u;\n"
           "        cpu_.attempted_guest_instructions += batch_instructions;\n"
           "        cpu_.retired_guest_instructions += batch_instructions;\n"
           "        trace_memory_fill_batch_admission(admitted);\n"
           "        return true;\n"
           "    }\n"
           "    bool try_mmio_wait_loop_batch(\n"
           "            const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "            const " +
           entry_namespace +
           "::MmioWaitLoopBatchDescriptor& descriptor) {\n"
           "        constexpr std::uint64_t quantum = 131'072u;\n"
           "        ++mmio_wait_loop_batch_counters_.attempts;\n"
           "        const bool pointer_from_register = descriptor.pointer_from_register;\n"
           "        const bool mask_from_register =\n"
           "            descriptor.mask_register < cpu_.r.size();\n"
           "        const auto block_size = pointer_from_register ? 6u : 8u;\n"
           "        const auto read_offset = pointer_from_register ? 0u : 2u;\n"
           "        const auto test_offset = read_offset + 2u;\n"
           "        const auto branch_offset = test_offset + 2u;\n"
           "        const auto runtime_state_allows_batch = [&] {\n"
           "            return cpu_.memory.watchpoint_count() == 0u &&\n"
           "                !cpu_.memory.has_trace_handler() &&\n"
           "                !cpu_.memory.has_mmio_trace_handler() &&\n"
           "                !cpu_.memory.mmio_access_tracking_enabled() &&\n"
           "                !cpu_.memory.has_guest_memory_access_sink() &&\n"
           "                cpu_.memory.lookup_mode() ==\n"
           "                    katana::runtime::MemoryLookupMode::Indexed &&\n"
           "                state_.main_ram && state_.system_asic_device &&\n"
           "                state_.main_ram->size() ==\n"
           "                    katana::runtime::dreamcast_main_ram_size &&\n"
           "                state_.system_asic && state_.address_space &&\n"
           "                cpu_.address_space &&\n"
           "                cpu_.address_space.get() == state_.address_space.get() &&\n"
           "                state_.scheduler && state_.interrupt_controller &&\n"
           "                state_.interrupt_router && state_.code_tracker &&\n"
           "                state_.disc_load_transactions &&\n"
           "                !state_.disc_load_transactions->transaction_active() &&\n"
           "                active_block_variant_.has_value();\n"
           "        };\n"
           "        if (!mmio_wait_loop_batching_enabled_ ||\n"
           "            descriptor.mmio_physical_address !=\n"
           "                katana::runtime::system_asic_physical_base ||\n"
           "            descriptor.read_site != descriptor.loop_header + read_offset ||\n"
           "            descriptor.backedge_instruction_address !=\n"
           "                descriptor.loop_header + branch_offset ||\n"
           "            descriptor.pointer_register >= cpu_.r.size() ||\n"
           "            descriptor.value_register >= cpu_.r.size() ||\n"
           "            (mask_from_register\n"
           "                 ? descriptor.test_mask != 0u ||\n"
           "                       descriptor.mask_register == descriptor.pointer_register ||\n"
           "                       descriptor.mask_register == descriptor.value_register\n"
           "                 : descriptor.mask_register != 0xFFu ||\n"
           "                       descriptor.test_mask == 0u) ||\n"
           "            (pointer_from_register\n"
           "                 ? descriptor.pointer_literal_address != 0u ||\n"
           "                       descriptor.mmio_guest_address != 0u ||\n"
           "                       descriptor.pointer_register == descriptor.value_register ||\n"
           "                       descriptor.round_instruction_count != 3u ||\n"
           "                       descriptor.pre_read_guest_cycles != 0u\n"
           "                 : descriptor.round_instruction_count != 4u ||\n"
           "                       descriptor.pre_read_guest_cycles == 0u ||\n"
           "                       descriptor.pre_read_guest_cycles >=\n"
           "                           descriptor.round_guest_cycles) ||\n"
           "            descriptor.round_guest_cycles == 0u ||\n"
           "            descriptor.round_guest_cycles > quantum ||\n"
           "            descriptor.read_guest_cycles == 0u ||\n"
           "            descriptor.test_guest_cycles == 0u ||\n"
           "            descriptor.branch_guest_cycles == 0u ||\n"
           "            descriptor.pre_read_guest_cycles +\n"
           "                    descriptor.read_guest_cycles +\n"
           "                    descriptor.test_guest_cycles +\n"
           "                    descriptor.branch_guest_cycles !=\n"
           "                descriptor.round_guest_cycles)\n"
           "            return false;\n"
           "        if (selected_block.runtime_registered || selected_block.aot_template ||\n"
           "            selected_block.virtual_start != descriptor.loop_header ||\n"
           "            selected_block.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.loop_header) ||\n"
           "            selected_block.size != block_size ||\n"
           "            selected_block.end_kind !=\n"
           "                katana::runtime::BlockEndKind::ConditionalBranch ||\n"
           "            (selected_block.provenance != descriptor.block_provenance &&\n"
           "             (selected_block.provenance.size() !=\n"
           "                      descriptor.block_provenance.size() + 12u ||\n"
           "              !selected_block.provenance.starts_with(\n"
           "                  descriptor.block_provenance) ||\n"
           "              !selected_block.provenance.ends_with(\"-mmu-variant\"))))\n"
           "            return false;\n"
           "        if (!runtime_state_allows_batch()) return false;\n"
           "        if (cpu_.pending_guest_cycles != 0u) return false;\n"
           "        if (pointer_from_register &&\n"
           "            katana::runtime::canonical_physical_address(\n"
           "                cpu_.r[descriptor.pointer_register]) !=\n"
           "                descriptor.mmio_physical_address) return false;\n"
           "        const auto has_acceptable_pending_interrupt = [&] {\n"
           "            if (cpu_.interrupts_blocked() || cpu_.interrupt_mask() == 15u)\n"
           "                return false;\n"
           "            synchronize_interrupt_sources_if_needed();\n"
           "            return state_.interrupt_controller->can_accept(cpu_);\n"
           "        };\n"
           "        if (has_acceptable_pending_interrupt()) return false;\n"
           "        const auto selected_physical_origin = selected_block.physical_origin;\n"
           "        const auto proves_instruction_block = [&](const auto& registration) {\n"
           "            try {\n"
           "                if (state_.address_space->instruction_translation_path(\n"
           "                        descriptor.loop_header, cpu_.privileged_mode()) !=\n"
           "                    katana::runtime::InstructionTranslationPath::Direct)\n"
           "                    return false;\n"
           "                const auto inspected = state_.address_space->inspect_translation(\n"
           "                    descriptor.loop_header,\n"
           "                    katana::runtime::TranslationAccess::Instruction,\n"
           "                    cpu_.privileged_mode());\n"
           "                if (katana::runtime::canonical_physical_address(\n"
           "                        inspected.physical_address) !=\n"
           "                        registration.physical_origin ||\n"
           "                    !state_.address_space->prove_instruction_mapping(\n"
           "                        descriptor.loop_header, registration.physical_origin,\n"
           "                        registration.size, cpu_.privileged_mode()))\n"
           "                    return false;\n"
           "                const auto variant = katana::runtime::block_variant_key(\n"
           "                    state_.address_space->guard_for(\n"
           "                        descriptor.loop_header, cpu_.read_fpscr(),\n"
           "                        cpu_.privileged_mode()),\n"
           "                    active_block_variant_->runtime_generation);\n"
           "                return variant == *active_block_variant_;\n"
           "            } catch (...) {\n"
           "                return false;\n"
           "            }\n"
           "        };\n"
           "        const auto registration =\n"
           "            executable_blocks_.find(descriptor.loop_header);\n"
           "        if (registration == executable_blocks_.end() ||\n"
           "            registration->second.physical_origin !=\n"
           "                selected_block.physical_origin ||\n"
           "            registration->second.size != selected_block.size ||\n"
           "            registration->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush ||\n"
           "            registration->second.maximum_guest_cycles !=\n"
           "                descriptor.round_guest_cycles ||\n"
           "            !state_.code_tracker->dispatchable(\n"
           "                registration->second.identity) ||\n"
           "            !proves_instruction_block(registration->second))\n"
           "            return false;\n"
           "        const auto main_ram_base =\n"
           "            katana::runtime::dreamcast_main_ram_area_bases.front();\n"
           "        std::optional<ProvenMemoryTranslation> literal;\n"
           "        auto proven_literal_value = pointer_from_register\n"
           "            ? cpu_.r[descriptor.pointer_register] : 0u;\n"
           "        if (!pointer_from_register) {\n"
           "            literal = prove_main_ram_translation(\n"
           "                cpu_, state_, descriptor.pointer_literal_address, 4u,\n"
           "                katana::runtime::TranslationAccess::Read);\n"
           "            if (!literal || !literal->no_mmu_fastpath ||\n"
           "                literal->utlb_slot != 0xFFu ||\n"
           "                literal->physical_address < main_ram_base ||\n"
           "                literal->physical_address >\n"
           "                    main_ram_base +\n"
           "                        katana::runtime::dreamcast_main_ram_size - 4u)\n"
           "                return false;\n"
           "            proven_literal_value = state_.main_ram->read_u32(\n"
           "                literal->physical_address - main_ram_base);\n"
           "            if (proven_literal_value != descriptor.mmio_guest_address)\n"
           "                return false;\n"
           "        }\n"
           "        const auto mmio = prove_contiguous_translation(\n"
           "            cpu_, state_, proven_literal_value, 4u,\n"
           "            katana::runtime::TranslationAccess::Read);\n"
           "        if (!mmio || !mmio->no_mmu_fastpath || mmio->utlb_slot != 0xFFu ||\n"
           "            mmio->physical_address != descriptor.mmio_physical_address ||\n"
           "            !cpu_.memory.maps_device(\n"
           "                mmio->physical_address, 4u,\n"
           "                state_.system_asic_device.get(), false))\n"
           "            return false;\n"
           "        auto literal_value = proven_literal_value;\n"
           "        if (!pointer_from_register) {\n"
           "            const katana::runtime::GuestInstructionAttempt pointer_attempt(\n"
           "                cpu_, descriptor.loop_header,\n"
           "                descriptor.pre_read_guest_cycles);\n"
           "            try {\n"
           "                const katana::runtime::GuestInstructionOrigin pointer_origin{\n"
           "                    descriptor.loop_header, selected_physical_origin, true};\n"
           "                literal_value = katana::runtime::guest_read_u32_at(\n"
           "                    cpu_, pointer_origin, descriptor.pointer_literal_address);\n"
           "                cpu_.r[descriptor.pointer_register] = literal_value;\n"
           "            } catch (const katana::runtime::MemoryAccessError& error) {\n"
           "                katana::runtime::enter_memory_exception(\n"
           "                    cpu_, error, descriptor.loop_header);\n"
           "                katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "                return true;\n"
           "            }\n"
           "        }\n"
           "        katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "        const auto test_mask = mask_from_register\n"
           "            ? cpu_.r[descriptor.mask_register] : descriptor.test_mask;\n"
           "        bool post_flush_batch_contract =\n"
           "            cpu_.pending_guest_cycles == 0u && runtime_state_allows_batch() &&\n"
           "            !has_acceptable_pending_interrupt() &&\n"
           "            literal_value == proven_literal_value && test_mask != 0u &&\n"
           "            (!pointer_from_register ||\n"
           "             cpu_.r[descriptor.pointer_register] == proven_literal_value) &&\n"
           "            (!mask_from_register ||\n"
           "             cpu_.r[descriptor.mask_register] == test_mask);\n"
           "        if (post_flush_batch_contract) {\n"
           "            const auto post_registration =\n"
           "                executable_blocks_.find(descriptor.loop_header);\n"
           "            post_flush_batch_contract =\n"
           "                post_registration != executable_blocks_.end() &&\n"
           "                post_registration->second.physical_origin ==\n"
           "                    selected_physical_origin &&\n"
           "                post_registration->second.size == block_size &&\n"
           "                post_registration->second.timing_class ==\n"
           "                    katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush &&\n"
           "                post_registration->second.maximum_guest_cycles ==\n"
           "                    descriptor.round_guest_cycles &&\n"
           "                state_.code_tracker->dispatchable(\n"
           "                    post_registration->second.identity) &&\n"
           "                proves_instruction_block(post_registration->second);\n"
           "        }\n"
           "        if (post_flush_batch_contract && !pointer_from_register) {\n"
           "            const auto post_literal = prove_main_ram_translation(\n"
           "                cpu_, state_, descriptor.pointer_literal_address, 4u,\n"
           "                katana::runtime::TranslationAccess::Read);\n"
           "            post_flush_batch_contract = post_literal &&\n"
           "                post_literal->no_mmu_fastpath &&\n"
           "                post_literal->utlb_slot == 0xFFu &&\n"
           "                post_literal->physical_address == literal->physical_address &&\n"
           "                state_.main_ram->read_u32(\n"
           "                    post_literal->physical_address - main_ram_base) ==\n"
           "                    descriptor.mmio_guest_address;\n"
           "        }\n"
           "        if (post_flush_batch_contract) {\n"
           "            const auto post_mmio = prove_contiguous_translation(\n"
           "                cpu_, state_, literal_value, 4u,\n"
           "                katana::runtime::TranslationAccess::Read);\n"
           "            post_flush_batch_contract = post_mmio &&\n"
           "                post_mmio->no_mmu_fastpath &&\n"
           "                post_mmio->utlb_slot == 0xFFu &&\n"
           "                post_mmio->physical_address ==\n"
           "                    descriptor.mmio_physical_address &&\n"
           "                cpu_.memory.maps_device(\n"
           "                    post_mmio->physical_address, 4u,\n"
           "                    state_.system_asic_device.get(), false);\n"
           "        }\n"
           "        std::uint32_t mmio_value = 0u;\n"
           "        {\n"
           "            const katana::runtime::GuestInstructionAttempt read_attempt(\n"
           "                cpu_, descriptor.read_site, descriptor.read_guest_cycles);\n"
           "            try {\n"
           "                const auto read_origin =\n"
           "                    cpu_.memory.has_guest_memory_access_sink()\n"
           "                    ? katana::runtime::GuestInstructionOrigin{\n"
           "                          descriptor.read_site,\n"
           "                          selected_physical_origin + read_offset, true}\n"
           "                    : katana::runtime::GuestInstructionOrigin{};\n"
           "                mmio_value = katana::runtime::guest_read_u32_at(\n"
           "                    cpu_, read_origin, literal_value);\n"
           "                cpu_.r[descriptor.value_register] = mmio_value;\n"
           "            } catch (const katana::runtime::MemoryAccessError& error) {\n"
           "                katana::runtime::enter_memory_exception(\n"
           "                    cpu_, error, descriptor.read_site);\n"
           "                katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "                return true;\n"
           "            }\n"
           "        }\n"
           "        const bool test_result =\n"
           "            (mmio_value & test_mask) == 0u;\n"
           "        const auto finish_scalar_round = [&] {\n"
           "            {\n"
           "                const katana::runtime::GuestInstructionAttempt test_attempt(\n"
           "                    cpu_, descriptor.loop_header + test_offset,\n"
           "                    descriptor.test_guest_cycles);\n"
           "                cpu_.t = test_result;\n"
           "            }\n"
           "            {\n"
           "                const katana::runtime::GuestInstructionAttempt branch_attempt(\n"
           "                    cpu_, descriptor.backedge_instruction_address,\n"
           "                    descriptor.branch_guest_cycles);\n"
           "                cpu_.pc = test_result == descriptor.branch_on_true\n"
           "                    ? descriptor.loop_header : descriptor.loop_header + block_size;\n"
           "            }\n"
           "            katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "            return true;\n"
           "        };\n"
           "        if (!post_flush_batch_contract ||\n"
           "            test_result != descriptor.branch_on_true ||\n"
           "            cpu_.pending_guest_cycles != descriptor.read_guest_cycles)\n"
           "            return finish_scalar_round();\n"
           "        const auto first_round_tail_guest_cycles =\n"
           "            descriptor.round_guest_cycles - descriptor.pre_read_guest_cycles;\n"
           "        if (first_round_tail_guest_cycles > quantum)\n"
           "            return finish_scalar_round();\n"
           "        auto admitted = quantum / descriptor.round_guest_cycles;\n"
           "        const auto scheduler_cycle = state_.scheduler->current_cycle();\n"
           "        if (const auto event = state_.scheduler->next_event_cycle()) {\n"
           "            if (*event <= scheduler_cycle)\n"
           "                return finish_scalar_round();\n"
           "            const auto available = *event - scheduler_cycle - 1u;\n"
           "            if (available < first_round_tail_guest_cycles)\n"
           "                return finish_scalar_round();\n"
           "            admitted = std::min(\n"
           "                admitted, 1u +\n"
           "                    (available - first_round_tail_guest_cycles) /\n"
           "                        descriptor.round_guest_cycles);\n"
           "        }\n"
           "        if (const auto remaining = state_.scheduler->remaining_guest_cycles()) {\n"
           "            if (*remaining <= first_round_tail_guest_cycles)\n"
           "                return finish_scalar_round();\n"
           "            admitted = std::min(\n"
           "                admitted, 1u +\n"
           "                    (*remaining - first_round_tail_guest_cycles - 1u) /\n"
           "                        descriptor.round_guest_cycles);\n"
           "        }\n"
           "        if (admitted > std::numeric_limits<std::uint64_t>::max() /\n"
           "                           descriptor.round_guest_cycles ||\n"
           "            admitted > std::numeric_limits<std::uint64_t>::max() /\n"
           "                           descriptor.round_instruction_count)\n"
           "            return finish_scalar_round();\n"
           "        const auto batch_cycles =\n"
           "            admitted * descriptor.round_guest_cycles;\n"
           "        const auto batch_instructions =\n"
           "            admitted * descriptor.round_instruction_count;\n"
           "        const auto already_attempted_instructions =\n"
           "            pointer_from_register ? 1u : 2u;\n"
           "        if (batch_cycles < descriptor.pre_read_guest_cycles ||\n"
           "            batch_instructions < already_attempted_instructions)\n"
           "            return finish_scalar_round();\n"
           "        const auto remaining_batch_cycles =\n"
           "            batch_cycles - descriptor.pre_read_guest_cycles;\n"
           "        const auto remaining_batch_instructions =\n"
           "            batch_instructions - already_attempted_instructions;\n"
           "        if (remaining_batch_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.total_guest_cycles ||\n"
           "            remaining_batch_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() - scheduler_cycle ||\n"
           "            remaining_batch_instructions >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.attempted_guest_instructions ||\n"
           "            remaining_batch_instructions >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.retired_guest_instructions)\n"
           "            return finish_scalar_round();\n"
           "        const auto additional_rounds = admitted - 1u;\n"
           "        const auto memory_accesses_per_additional_round =\n"
           "            pointer_from_register ? 1u : 2u;\n"
           "        if (additional_rounds >\n"
           "                std::numeric_limits<std::uint64_t>::max() /\n"
           "                    memory_accesses_per_additional_round)\n"
           "            return finish_scalar_round();\n"
           "        const auto synthetic_memory_accesses =\n"
           "            additional_rounds * memory_accesses_per_additional_round;\n"
           "        if (!cpu_.memory.account_prevalidated_unobserved_accesses(\n"
           "                synthetic_memory_accesses, synthetic_memory_accesses))\n"
           "            return finish_scalar_round();\n"
           "        cpu_.t = test_result;\n"
           "        cpu_.pc = descriptor.loop_header;\n"
           "        cpu_.attempted_guest_instructions += remaining_batch_instructions;\n"
           "        cpu_.retired_guest_instructions += remaining_batch_instructions;\n"
           "        cpu_.active_instruction_pc =\n"
           "            descriptor.backedge_instruction_address;\n"
           "        cpu_.active_instruction_physical_pc =\n"
           "            selected_physical_origin + branch_offset;\n"
           "        cpu_.pending_guest_cycles +=\n"
           "            remaining_batch_cycles - descriptor.read_guest_cycles;\n"
           "        katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "        ++mmio_wait_loop_batch_counters_.admissions;\n"
           "        mmio_wait_loop_batch_counters_.batched_rounds += admitted;\n"
           "        mmio_wait_loop_batch_counters_.batched_guest_cycles += batch_cycles;\n"
           "        return true;\n"
           "    }\n"
           "    bool try_counted_loop_batch(\n"
           "            const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "            const " +
           entry_namespace +
           "::CountedLoopBatchDescriptor& descriptor) {\n"
           "        constexpr std::uint64_t quantum = 131'072u;\n"
           "        const auto increment_guest_cycles =\n"
           "            descriptor.round_guest_cycles - descriptor.guard_guest_cycles;\n"
           "        if (!counted_loop_batching_enabled_ ||\n"
           "            descriptor.increment_size != 6u || descriptor.step == 0u ||\n"
           "            descriptor.limit_width !=\n"
           "                (descriptor.signed_word_limit ? 2u : 4u) ||\n"
           "            descriptor.guard_instruction_count != 4u ||\n"
           "            descriptor.round_instruction_count != 7u ||\n"
           "            descriptor.prefix_guest_cycles == 0u ||\n"
           "            descriptor.first_counter_read_guest_cycles == 0u ||\n"
           "            descriptor.store_guest_cycles == 0u ||\n"
           "            descriptor.guard_guest_cycles <=\n"
           "                descriptor.prefix_guest_cycles +\n"
           "                    descriptor.first_counter_read_guest_cycles ||\n"
           "            descriptor.guard_guest_cycles >= descriptor.round_guest_cycles ||\n"
           "            descriptor.round_guest_cycles > quantum ||\n"
           "            increment_guest_cycles <= descriptor.store_guest_cycles ||\n"
           "            descriptor.guard_address >\n"
           "                std::numeric_limits<std::uint32_t>::max() - 8u ||\n"
           "            descriptor.increment_address >\n"
           "                std::numeric_limits<std::uint32_t>::max() - 6u ||\n"
           "            descriptor.first_counter_read_instruction_address !=\n"
           "                descriptor.guard_address + 2u ||\n"
           "            descriptor.pre_store_instruction_address !=\n"
           "                descriptor.increment_address + 2u ||\n"
           "            descriptor.store_instruction_address !=\n"
           "                descriptor.increment_address + 4u ||\n"
           "            descriptor.counter_base_register >= cpu_.r.size() ||\n"
           "            descriptor.limit_register >= cpu_.r.size() ||\n"
           "            descriptor.compare_register >= cpu_.r.size() ||\n"
           "            descriptor.increment_register >= cpu_.r.size())\n"
           "            return counted_loop_batch_rejected(\"entry-contract\");\n"
           "        if (selected_block.runtime_registered || selected_block.aot_template ||\n"
           "            selected_block.virtual_start != descriptor.guard_address ||\n"
           "            selected_block.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.guard_address) ||\n"
           "            selected_block.size != 8u ||\n"
           "            selected_block.end_kind !=\n"
           "                katana::runtime::BlockEndKind::ConditionalBranch ||\n"
           "            !counted_loop_provenance_matches(\n"
           "                selected_block.provenance, descriptor.guard_provenance))\n"
           "            return counted_loop_batch_rejected(\"selected-block\");\n"
           "        const auto selected_physical_origin = selected_block.physical_origin;\n"
           "        const ScopedCpuActiveBlockProvenance active_block_provenance(\n"
           "            cpu_, descriptor.guard_address, selected_physical_origin, 8u);\n"
           "        const auto initial_contract = prove_counted_loop_contract(\n"
           "            descriptor, selected_physical_origin, increment_guest_cycles);\n"
           "        if (!initial_contract)\n"
           "            return counted_loop_batch_rejected(\"initial-proof\");\n"
           "        const auto guard_physical = selected_physical_origin;\n"
           "        cpu_.active_block_virtual_start = descriptor.guard_address;\n"
           "        cpu_.active_block_physical_start = guard_physical;\n"
           "        cpu_.active_block_size = 8u;\n"
           "        std::uint32_t limit_bits = 0u;\n"
           "        {\n"
           "            const katana::runtime::GuestInstructionAttempt limit_attempt(\n"
           "                cpu_, descriptor.guard_address,\n"
           "                descriptor.prefix_guest_cycles);\n"
           "            try {\n"
           "                const katana::runtime::GuestInstructionOrigin limit_origin{\n"
           "                    descriptor.guard_address, guard_physical, true};\n"
           "                limit_bits = descriptor.signed_word_limit\n"
           "                    ? static_cast<std::uint32_t>(\n"
           "                          katana::runtime::guest_read_s16_at(\n"
           "                              cpu_, limit_origin, descriptor.limit_address))\n"
           "                    : katana::runtime::guest_read_u32_at(\n"
           "                          cpu_, limit_origin, descriptor.limit_address);\n"
           "                cpu_.r[descriptor.limit_register] = limit_bits;\n"
           "            } catch (const katana::runtime::MemoryAccessError& error) {\n"
           "                katana::runtime::enter_memory_exception(\n"
           "                    cpu_, error, descriptor.guard_address);\n"
           "                katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "                return true;\n"
           "            }\n"
           "        }\n"
           "        katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "        const auto post_flush_contract = prove_counted_loop_contract(\n"
           "            descriptor, selected_physical_origin, increment_guest_cycles);\n"
           "        const bool post_flush_batch_contract =\n"
           "            post_flush_contract && same_counted_loop_contract(\n"
           "                *post_flush_contract, *initial_contract);\n"
           "        const bool post_flush_limit_unchanged =\n"
           "            post_flush_contract &&\n"
           "            (descriptor.signed_word_limit\n"
           "                 ? state_.main_ram->read_u16(\n"
           "                       post_flush_contract->limit_backing_offset) ==\n"
           "                       static_cast<std::uint16_t>(limit_bits)\n"
           "                 : state_.main_ram->read_u32(\n"
           "                       post_flush_contract->limit_backing_offset) == limit_bits);\n"
           "        const auto staged_counter_address = static_cast<std::uint32_t>(\n"
           "            cpu_.r[descriptor.counter_base_register] +\n"
           "            static_cast<std::uint32_t>(descriptor.counter_displacement));\n"
           "        std::uint32_t current_bits = 0u;\n"
           "        {\n"
           "            const katana::runtime::GuestInstructionAttempt "
           "first_counter_read_attempt(\n"
           "                cpu_, descriptor.first_counter_read_instruction_address,\n"
           "                descriptor.first_counter_read_guest_cycles);\n"
           "            try {\n"
           "                const katana::runtime::GuestInstructionOrigin counter_origin{\n"
           "                    descriptor.first_counter_read_instruction_address,\n"
           "                    guard_physical + 2u, true};\n"
           "                current_bits = katana::runtime::guest_read_u32_at(\n"
           "                    cpu_, counter_origin, staged_counter_address);\n"
           "                cpu_.r[descriptor.compare_register] = current_bits;\n"
           "            } catch (const katana::runtime::MemoryAccessError& error) {\n"
           "                katana::runtime::enter_memory_exception(\n"
           "                    cpu_, error,\n"
           "                    descriptor.first_counter_read_instruction_address);\n"
           "                katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "                return true;\n"
           "            }\n"
           "        }\n"
           "        const auto current = static_cast<std::int64_t>(\n"
           "            static_cast<std::int32_t>(current_bits));\n"
           "        const auto limit = static_cast<std::int64_t>(\n"
           "            static_cast<std::int32_t>(limit_bits));\n"
           "        const auto finish_scalar_guard = [&] {\n"
           "            const bool exit_loop = current >= limit;\n"
           "            cpu_.t = exit_loop;\n"
           "            cpu_.pc = exit_loop\n"
           "                ? descriptor.guard_address + 8u\n"
           "                : descriptor.increment_address;\n"
           "            cpu_.active_block_virtual_start = descriptor.guard_address;\n"
           "            cpu_.active_block_physical_start = guard_physical;\n"
           "            cpu_.active_block_size = 8u;\n"
           "            cpu_.active_instruction_pc = descriptor.guard_address + 6u;\n"
           "            cpu_.active_instruction_physical_pc = guard_physical + 6u;\n"
           "            const auto tail_instructions =\n"
           "                descriptor.guard_instruction_count - 2u;\n"
           "            const auto tail_guest_cycles = descriptor.guard_guest_cycles -\n"
           "                descriptor.prefix_guest_cycles -\n"
           "                descriptor.first_counter_read_guest_cycles;\n"
           "            cpu_.attempted_guest_instructions += tail_instructions;\n"
           "            cpu_.retired_guest_instructions += tail_instructions;\n"
           "            cpu_.pending_guest_cycles += tail_guest_cycles;\n"
           "            katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "            return true;\n"
           "        };\n"
           "        if (!post_flush_batch_contract || !post_flush_limit_unchanged ||\n"
           "            staged_counter_address != initial_contract->counter_address ||\n"
           "            cpu_.pending_guest_cycles !=\n"
           "                descriptor.first_counter_read_guest_cycles ||\n"
           "            current >= limit)\n"
           "            return finish_scalar_guard();\n"
           "        const auto distance = static_cast<std::uint64_t>(limit - current);\n"
           "        const auto iterations =\n"
           "            (distance + descriptor.step - 1u) / descriptor.step;\n"
           "        const auto iterations_before_signed_overflow =\n"
           "            static_cast<std::uint64_t>(\n"
           "                (static_cast<std::int64_t>(\n"
           "                     std::numeric_limits<std::int32_t>::max()) - current) /\n"
           "                descriptor.step);\n"
           "        if (iterations == 0u || iterations > iterations_before_signed_overflow)\n"
           "            return finish_scalar_guard();\n"
           "        auto admitted = std::min(\n"
           "            iterations,\n"
           "            quantum / descriptor.round_guest_cycles);\n"
           "        const auto first_round_tail_guest_cycles =\n"
           "            descriptor.round_guest_cycles - descriptor.prefix_guest_cycles;\n"
           "        const auto scheduler_cycle = state_.scheduler->current_cycle();\n"
           "        if (const auto event = state_.scheduler->next_event_cycle()) {\n"
           "            if (*event <= scheduler_cycle)\n"
           "                return finish_scalar_guard();\n"
           "            const auto available = *event - scheduler_cycle - 1u;\n"
           "            if (available < first_round_tail_guest_cycles)\n"
           "                return finish_scalar_guard();\n"
           "            admitted = std::min(\n"
           "                admitted,\n"
           "                1u + (available - first_round_tail_guest_cycles) /\n"
           "                    descriptor.round_guest_cycles);\n"
           "        }\n"
           "        if (const auto remaining = state_.scheduler->remaining_guest_cycles()) {\n"
           "            if (*remaining <= first_round_tail_guest_cycles)\n"
           "                return finish_scalar_guard();\n"
           "            admitted = std::min(\n"
           "                admitted,\n"
           "                1u + (*remaining - first_round_tail_guest_cycles - 1u) /\n"
           "                    descriptor.round_guest_cycles);\n"
           "        }\n"
           "        if (admitted == 0u)\n"
           "            return finish_scalar_guard();\n"
           "        if (admitted > std::numeric_limits<std::uint64_t>::max() /\n"
           "                           descriptor.round_guest_cycles ||\n"
           "            admitted > std::numeric_limits<std::uint64_t>::max() /\n"
           "                           descriptor.round_instruction_count ||\n"
           "            admitted >\n"
           "                (std::numeric_limits<std::uint64_t>::max() - 2u) / 3u)\n"
           "            return finish_scalar_guard();\n"
           "        const auto batch_cycles = admitted * descriptor.round_guest_cycles;\n"
           "        const auto batch_instructions =\n"
           "            admitted * descriptor.round_instruction_count;\n"
           "        if (batch_cycles < descriptor.prefix_guest_cycles +\n"
           "                               descriptor.first_counter_read_guest_cycles +\n"
           "                               descriptor.store_guest_cycles ||\n"
           "            batch_instructions < 3u)\n"
           "            return finish_scalar_guard();\n"
           "        const auto remaining_batch_cycles =\n"
           "            batch_cycles - descriptor.prefix_guest_cycles;\n"
           "        const auto remaining_batch_instructions = batch_instructions - 3u;\n"
           "        const auto instructions_through_final_store =\n"
           "            remaining_batch_instructions + 1u;\n"
           "        if (remaining_batch_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.total_guest_cycles ||\n"
           "            remaining_batch_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() - scheduler_cycle ||\n"
           "            instructions_through_final_store >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.attempted_guest_instructions ||\n"
           "            instructions_through_final_store >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.retired_guest_instructions)\n"
           "            return finish_scalar_guard();\n"
           "        const auto final_counter = static_cast<std::uint32_t>(\n"
           "            current + static_cast<std::int64_t>(admitted * descriptor.step));\n"
           "        const auto previous_counter = static_cast<std::uint32_t>(\n"
           "            current + static_cast<std::int64_t>((admitted - 1u) * descriptor.step));\n"
           "        const auto store_contract = prove_counted_loop_contract(\n"
           "            descriptor, selected_physical_origin, increment_guest_cycles);\n"
           "        if (!store_contract || !same_counted_loop_contract(\n"
           "                *store_contract, *initial_contract) ||\n"
           "            store_contract->counter_address != staged_counter_address)\n"
           "            return finish_scalar_guard();\n"
           "        const auto synthetic_memory_accesses = admitted * 3u - 2u;\n"
           "        const auto first_sequence_value = static_cast<std::uint32_t>(\n"
           "            current + static_cast<std::int64_t>(descriptor.step));\n"
           "        auto prepared_sequence =\n"
           "            cpu_.memory.prepare_prevalidated_repeated_u32_sequence(\n"
           "                store_contract->counter_physical,\n"
           "                static_cast<std::size_t>(admitted), first_sequence_value,\n"
           "                descriptor.step, katana::runtime::CodeWriteSource::Cpu,\n"
           "                static_cast<std::size_t>(synthetic_memory_accesses),\n"
           "                static_cast<std::size_t>(\n"
           "                    synthetic_memory_accesses + admitted));\n"
           "        if (!prepared_sequence) {\n"
           "            static_cast<void>(counted_loop_batch_rejected(\"memory-prepare\"));\n"
           "            return finish_scalar_guard();\n"
           "        }\n"
           "        const auto staged_batch_cycles =\n"
           "            remaining_batch_cycles - cpu_.pending_guest_cycles;\n"
           "        accept_batch_guest_cycles_before_commit(\n"
           "            staged_batch_cycles, AtomicBatchCommitKind::CountedLoop);\n"
           "        cpu_.memory.commit_prepared_repeated_u32_sequence(\n"
           "            std::move(*prepared_sequence));\n"
           "        cpu_.r[descriptor.limit_register] = limit_bits;\n"
           "        cpu_.r[descriptor.compare_register] = previous_counter;\n"
           "        cpu_.r[descriptor.increment_register] = final_counter;\n"
           "        cpu_.t = false;\n"
           "        cpu_.pc = descriptor.guard_address;\n"
           "        cpu_.active_block_virtual_start = descriptor.increment_address;\n"
           "        cpu_.active_block_physical_start = store_contract->increment_physical;\n"
           "        cpu_.active_block_size = descriptor.increment_size;\n"
           "        cpu_.attempted_guest_instructions += instructions_through_final_store;\n"
           "        cpu_.retired_guest_instructions += instructions_through_final_store;\n"
           "        cpu_.active_instruction_pc = descriptor.store_instruction_address;\n"
           "        cpu_.active_instruction_physical_pc = store_contract->store_physical;\n"
           "        cpu_.pc = descriptor.guard_address;\n"
           "        trace_counted_loop_batch_admission(admitted);\n"
           "        return true;\n"
           "    }\n"
           "    katana::runtime::ExecutableCodeTracker* executable_code_tracker() noexcept "
           "override {\n"
           "        return state_.code_tracker.get();\n"
           "    }\n"
           "    katana::runtime::ExecutableModuleCatalog* executable_module_catalog() noexcept "
           "override {\n"
           "        return state_.module_catalog.get();\n"
           "    }\n"
           "  private:\n"
           "    void prepare_static_aot_chain_guard(\n"
           "            const std::uint32_t virtual_start,\n"
           "            const std::uint32_t physical_start,\n"
           "            const bool runtime_registered) noexcept {\n"
           "        active_static_aot_chain_guard_ = {};\n"
           "        if (runtime_registered || runtime_probe_mode_ || replay_log_ != nullptr ||\n"
           "            !local_block_chaining_enabled_ || !active_block_variant_ ||\n"
           "            active_block_registration_ == nullptr ||\n"
           "            !active_block_registration_->chainable ||\n"
           "            !state_.address_space || !cpu_.address_space ||\n"
           "            cpu_.address_space.get() != state_.address_space.get() ||\n"
           "            !state_.scheduler || !state_.interrupt_controller ||\n"
           "            !state_.interrupt_router || !state_.runtime_blocks ||\n"
           "            !state_.code_tracker || product_budget_arm_failed_)\n"
           "            return;\n"
           "        const auto& block = *active_block_registration_;\n"
           "        if ((block.timing_class !=\n"
           "                 katana::runtime::ExecutableBlockTimingClass::PureCpu &&\n"
           "             block.timing_class !=\n"
           "                 katana::runtime::ExecutableBlockTimingClass::LinearRamOnly) ||\n"
           "            block.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(physical_start) ||\n"
           "            katana::runtime::canonical_physical_address(virtual_start) !=\n"
           "                block.physical_origin ||\n"
           "            !state_.address_space->direct_p1_p2_dispatch_guard_current(\n"
           "                virtual_start, cpu_.read_fpscr(), cpu_.privileged_mode(),\n"
           "                *active_block_variant_,\n"
           "                active_block_variant_->runtime_generation))\n"
           "            return;\n"
           "        try {\n"
           "            synchronize_interrupt_sources_if_needed();\n"
           "        } catch (...) {\n"
           "            return;\n"
           "        }\n"
           "\n"
           "        StaticAotChainGuard prepared;\n"
           "        prepared.variant = *active_block_variant_;\n"
           "        prepared.runtime_dispatch_generation =\n"
           "            state_.runtime_blocks->dispatch_generation();\n"
           "        prepared.code_generation =\n"
           "            state_.code_tracker->invalidation_count();\n"
           "        prepared.scheduler_reset_generation =\n"
           "            state_.scheduler->reset_generation();\n"
           "        prepared.scheduling_sr = cpu_.read_sr() &\n"
           "            (katana::runtime::sr_interrupt_mask |\n"
           "             katana::runtime::sr_fd_mask | katana::runtime::sr_bl_mask |\n"
           "             katana::runtime::sr_rb_mask | katana::runtime::sr_md_mask);\n"
           "\n"
           "        prepared.chain_pending_cycle_limit =\n"
           "            local_block_chain_guest_cycle_budget;\n"
           "        prepared.defer_pending_cycle_limit =\n"
           "            local_block_chain_guest_cycle_budget - 1u;\n"
           "        const auto tighten_chain_limit = [\n"
           "                &](const std::uint64_t allowance,\n"
           "                    const katana::runtime::ExecutableChainRejectionReason reason) {\n"
           "            if (allowance >= prepared.chain_pending_cycle_limit) return;\n"
           "            prepared.chain_pending_cycle_limit = allowance;\n"
           "            prepared.cycle_limit_rejection = reason;\n"
           "        };\n"
           "        const auto tighten_defer_limit = [\n"
           "                &](const std::uint64_t allowance) {\n"
           "            prepared.defer_pending_cycle_limit = std::min(\n"
           "                prepared.defer_pending_cycle_limit, allowance);\n"
           "        };\n"
           "        const auto current_cycle = state_.scheduler->current_cycle();\n"
           "        const auto cycle_capacity =\n"
           "            std::numeric_limits<std::uint64_t>::max() - current_cycle;\n"
           "        tighten_chain_limit(\n"
           "            cycle_capacity,\n"
           "            katana::runtime::ExecutableChainRejectionReason::CycleQuantum);\n"
           "        tighten_defer_limit(cycle_capacity);\n"
           "        if (const auto remaining = state_.scheduler->remaining_guest_cycles()) {\n"
           "            tighten_chain_limit(\n"
           "                *remaining,\n"
           "                katana::runtime::ExecutableChainRejectionReason::GuestCycleBudget);\n"
           "            tighten_defer_limit(*remaining == 0u ? 0u : *remaining - 1u);\n"
           "        }\n"
           "        if (product_target_guest_cycle_) {\n"
           "            const auto allowance = *product_target_guest_cycle_ > current_cycle\n"
           "                ? *product_target_guest_cycle_ - current_cycle : 0u;\n"
           "            tighten_chain_limit(\n"
           "                allowance,\n"
           "                katana::runtime::ExecutableChainRejectionReason::GuestCycleBudget);\n"
           "            tighten_defer_limit(allowance == 0u ? 0u : allowance - 1u);\n"
           "        }\n"
           "        if (const auto event = state_.scheduler->next_event_cycle()) {\n"
           "            const auto allowance = *event > current_cycle\n"
           "                ? *event - current_cycle - 1u : 0u;\n"
           "            tighten_chain_limit(\n"
           "                allowance,\n"
           "                katana::runtime::ExecutableChainRejectionReason::SchedulerDue);\n"
           "            tighten_defer_limit(allowance);\n"
           "        }\n"
           "\n"
           "        prepared.router_epoch = state_.interrupt_router->source_epoch();\n"
           "        prepared.controller_epoch =\n"
           "            state_.interrupt_controller->interrupt_epoch();\n"
           "        prepared.pending_mask =\n"
           "            state_.interrupt_controller->pending_mask();\n"
           "        prepared.highest_pending_level =\n"
           "            state_.interrupt_controller->highest_pending_level();\n"
           "        prepared.interrupt_acceptable =\n"
           "            state_.interrupt_controller->can_accept(cpu_);\n"
           "        prepared.valid = true;\n"
           "        active_static_aot_chain_guard_ = std::move(prepared);\n"
           "    }\n"
           "    katana::runtime::ExecutableChainRejectionReason\n"
           "    static_aot_chain_guard_rejection(\n"
           "            const std::uint32_t address) const noexcept {\n"
           "        using Rejection =\n"
           "            katana::runtime::ExecutableChainRejectionReason;\n"
           "        const auto& guard = active_static_aot_chain_guard_;\n"
           "        if (!guard.valid || !state_.address_space || !cpu_.address_space ||\n"
           "            cpu_.address_space.get() != state_.address_space.get() ||\n"
           "            !state_.scheduler || !state_.runtime_blocks ||\n"
           "            !state_.code_tracker || !state_.interrupt_controller ||\n"
           "            !state_.interrupt_router)\n"
           "            return Rejection::VariantOrGeneration;\n"
           "        if (state_.scheduler->reset_generation() !=\n"
           "                guard.scheduler_reset_generation)\n"
           "            return Rejection::SchedulerDue;\n"
           "        if (guard.interrupt_acceptable || interrupt_sources_dirty_ ||\n"
           "            state_.interrupt_router->source_epoch() != guard.router_epoch ||\n"
           "            state_.interrupt_controller->interrupt_epoch() !=\n"
           "                guard.controller_epoch ||\n"
           "            state_.interrupt_controller->pending_mask() != guard.pending_mask ||\n"
           "            state_.interrupt_controller->highest_pending_level() !=\n"
           "                guard.highest_pending_level)\n"
           "            return Rejection::InterruptAcceptable;\n"
           "        const auto scheduling_sr = cpu_.read_sr() &\n"
           "            (katana::runtime::sr_interrupt_mask |\n"
           "             katana::runtime::sr_fd_mask | katana::runtime::sr_bl_mask |\n"
           "             katana::runtime::sr_rb_mask | katana::runtime::sr_md_mask);\n"
           "        if (scheduling_sr != guard.scheduling_sr ||\n"
           "            state_.runtime_blocks->dispatch_generation() !=\n"
           "                guard.runtime_dispatch_generation)\n"
           "            return Rejection::VariantOrGeneration;\n"
           "        if (state_.code_tracker->invalidation_count() !=\n"
           "                guard.code_generation)\n"
           "            return Rejection::CodeGeneration;\n"
           "        if (!state_.address_space->direct_p1_p2_dispatch_guard_current(\n"
           "                address, cpu_.read_fpscr(), cpu_.privileged_mode(),\n"
           "                guard.variant, guard.variant.runtime_generation))\n"
           "            return Rejection::VariantOrGeneration;\n"
           "        return Rejection::None;\n"
           "    }\n"
           "    static const char* atomic_batch_commit_kind_name(\n"
           "            const AtomicBatchCommitKind kind) noexcept {\n"
           "        switch (kind) {\n"
           "        case AtomicBatchCommitKind::CountedLoop: return \"counted-loop\";\n"
           "        case AtomicBatchCommitKind::MemoryFill: return \"memory-fill\";\n"
           "        case AtomicBatchCommitKind::CompositeCallback:\n"
           "            return \"composite-callback\";\n"
           "        case AtomicBatchCommitKind::None: break;\n"
           "        }\n"
           "        return \"none\";\n"
           "    }\n"
           "    static bool internal_batch_commit_abort_requested(\n"
           "            const AtomicBatchCommitKind kind) noexcept {\n"
           "#if defined(KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST)\n"
           "        const auto* requested =\n"
           "            std::getenv(\"KATANA_PORT_TEST_BATCH_COMMIT_ABORT\");\n"
           "        return requested != nullptr && kind != AtomicBatchCommitKind::None &&\n"
           "            std::string_view(requested) == atomic_batch_commit_kind_name(kind);\n"
           "#else\n"
           "        static_cast<void>(kind);\n"
           "        return false;\n"
           "#endif\n"
           "    }\n"
           "    void accept_batch_guest_cycles_before_commit(\n"
           "            const std::uint64_t staged_guest_cycles,\n"
           "            const AtomicBatchCommitKind kind) {\n"
           "        if (kind == AtomicBatchCommitKind::None || staged_guest_cycles == 0u ||\n"
           "            staged_guest_cycles >\n"
           "                std::numeric_limits<std::uint64_t>::max() -\n"
           "                    cpu_.pending_guest_cycles)\n"
           "            throw std::runtime_error(\"Ungueltige atomare Batch-Gastzeit.\");\n"
           "        const auto pending_before = cpu_.pending_guest_cycles;\n"
           "        const auto total_before = cpu_.total_guest_cycles;\n"
           "        const auto previous_kind = active_atomic_batch_commit_;\n"
           "        if (previous_kind != AtomicBatchCommitKind::None)\n"
           "            throw std::runtime_error(\"Verschachtelte atomare Batch-Gastzeit.\");\n"
           "#if defined(KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST)\n"
           "        const bool verify_abort = internal_batch_commit_abort_requested(kind);\n"
           "        const auto cpu_hash_before = verify_abort\n"
           "            ? katana::runtime::hash_runtime_probe_cpu(\n"
           "                  katana::runtime::capture_runtime_probe_cpu(cpu_))\n"
           "            : 0u;\n"
           "        const auto scheduler_hash_before = verify_abort\n"
           "            ? katana::runtime::hash_runtime_probe_scheduler(\n"
           "                  katana::runtime::capture_runtime_probe_scheduler(\n"
           "                      *state_.scheduler))\n"
           "            : 0u;\n"
           "        const auto memory_hash = [&] {\n"
           "            if (!verify_abort) return std::uint64_t{0u};\n"
           "            const std::array ranges{\n"
           "                katana::runtime::RuntimeProbeMemoryRange{\n"
           "                    katana::runtime::RuntimeProbeMemoryRegion::MainRam, 0u,\n"
           "                    state_.main_ram->bytes()}};\n"
           "            return katana::runtime::hash_runtime_probe_memory(ranges);\n"
           "        };\n"
           "        const auto memory_hash_before = memory_hash();\n"
           "        const auto code_hash = [&] {\n"
           "            if (!verify_abort) return std::uint64_t{0u};\n"
           "            const std::array snapshots{\n"
           "                katana::runtime::make_runtime_probe_device_snapshot(\n"
           "                    state_.code_tracker->snapshot())};\n"
           "            return katana::runtime::hash_runtime_probe_devices(snapshots);\n"
           "        };\n"
           "        const auto module_hash = [&] {\n"
           "            if (!verify_abort) return std::uint64_t{0u};\n"
           "            const std::array snapshots{\n"
           "                katana::runtime::make_runtime_probe_device_snapshot(\n"
           "                    state_.module_catalog->snapshot())};\n"
           "            return katana::runtime::hash_runtime_probe_devices(snapshots);\n"
           "        };\n"
           "        const auto code_hash_before = code_hash();\n"
           "        const auto module_hash_before = module_hash();\n"
           "        const auto memory_counters_before = cpu_.memory.performance_counters();\n"
           "#endif\n"
           "        cpu_.pending_guest_cycles += staged_guest_cycles;\n"
           "        active_atomic_batch_commit_ = kind;\n"
           "        const auto restore_unaccepted_time = [&]() noexcept {\n"
           "            const auto delivered = cpu_.total_guest_cycles >= total_before\n"
           "                ? cpu_.total_guest_cycles - total_before : 0u;\n"
           "            cpu_.pending_guest_cycles = delivered < pending_before\n"
           "                ? pending_before - delivered : 0u;\n"
           "            active_atomic_batch_commit_ = previous_kind;\n"
           "        };\n"
           "        try {\n"
           "            katana::runtime::flush_pending_guest_cycles(cpu_, *this);\n"
           "        } catch (const katana::runtime::PlatformShutdownRequested&) {\n"
           "            restore_unaccepted_time();\n"
           "#if defined(KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST)\n"
           "            if (verify_abort) {\n"
           "                const auto& counters = cpu_.memory.performance_counters();\n"
           "                const bool unchanged =\n"
           "                    cpu_hash_before == katana::runtime::hash_runtime_probe_cpu(\n"
           "                        katana::runtime::capture_runtime_probe_cpu(cpu_)) &&\n"
           "                    scheduler_hash_before ==\n"
           "                        katana::runtime::hash_runtime_probe_scheduler(\n"
           "                            katana::runtime::capture_runtime_probe_scheduler(\n"
           "                                *state_.scheduler)) &&\n"
           "                    memory_hash_before == memory_hash() &&\n"
           "                    code_hash_before == code_hash() &&\n"
           "                    module_hash_before == module_hash() &&\n"
           "                    memory_counters_before.indexed_region_hits ==\n"
           "                        counters.indexed_region_hits &&\n"
           "                    memory_counters_before.reference_region_probes ==\n"
           "                        counters.reference_region_probes &&\n"
           "                    memory_counters_before.unobserved_accesses ==\n"
           "                        counters.unobserved_accesses &&\n"
           "                    memory_counters_before.observed_accesses ==\n"
           "                        counters.observed_accesses;\n"
           "                if (!unchanged)\n"
           "                    throw std::runtime_error(\n"
           "                        \"Batch-Lifecycle-Abbruch veraenderte Gastzustand.\");\n"
           "                std::cerr << \"KATANA_BATCH_COMMIT_ABORT_CLEAN kind=\"\n"
           "                          << atomic_batch_commit_kind_name(kind) << '\\n';\n"
           "            }\n"
           "#endif\n"
           "            throw;\n"
           "        } catch (...) {\n"
           "            restore_unaccepted_time();\n"
           "            throw;\n"
           "        }\n"
           "        active_atomic_batch_commit_ = previous_kind;\n"
           "        if (cpu_.pending_guest_cycles != 0u)\n"
           "            throw std::runtime_error(\n"
           "                \"Atomare Batch-Gastzeit wurde nicht vollstaendig angenommen.\");\n"
           "    }\n"
           "    void note_guest_program_entry() noexcept {\n"
           "        if (guest_program_dispatched_) return;\n"
           "        guest_program_dispatched_ = true;\n"
           "        guest_program_entry_cycle_ = state_.scheduler->current_cycle();\n"
           "        post_entry_host_started_ = std::chrono::steady_clock::now();\n"
           "        if (product_entry_evidence_callback_) {\n"
           "            try {\n"
           "                product_entry_evidence_callback_();\n"
           "            } catch (...) {\n"
           "                product_entry_evidence_failed_ = true;\n"
           "            }\n"
           "        }\n"
           "        if (!requested_post_entry_cycles_) return;\n"
           "        product_target_guest_cycle_ =\n"
           "            state_.scheduler->\n"
           "                try_set_guest_cycle_budget_after_current_cycle(\n"
           "                    *requested_post_entry_cycles_);\n"
           "        product_budget_arm_failed_ =\n"
           "            !product_target_guest_cycle_.has_value();\n"
           "    }\n"
           "    void observe_runtime_checkpoint(\n"
           "            katana::runtime::SystemReplayCheckpointKind replay_checkpoint,\n"
           "            katana::runtime::RuntimeProbeCheckpoint probe_checkpoint) noexcept {\n"
           "        try {\n"
           "            if (!probe_observations_.observe_checkpoint(\n"
           "                    probe_checkpoint,\n"
           "                    katana::runtime::capture_runtime_probe_cpu(cpu_))) return;\n"
           "            if (!replay_observations_.observe_guest_checkpoint(replay_checkpoint))\n"
           "                return;\n"
           "            if (runtime_probe_mode_)\n"
           "                std::cout << katana::runtime::runtime_probe_checkpoint_line_prefix\n"
           "                          << replay_observations_.serialize_checkpoint_json()\n"
           "                          << std::endl;\n"
           "        } catch (...) {\n"
           "            if (replay_log_ != nullptr) replay_log_->note_dropped_event();\n"
           "        }\n"
           "    }\n"
           "    katana::runtime::CpuState& cpu_;\n"
           "    const katana::runtime::DreamcastRuntimeState& state_;\n"
           "    const katana::runtime::GuestProgramRangeMatcher "
           "guest_program_range_matcher_{expected_guest_program_range};\n"
           "    std::function<katana::runtime::PlatformLifecycleState()> lifecycle_poll_;\n"
           "    std::function<void()> guest_frame_poll_;\n"
           "    std::function<void()> product_entry_evidence_callback_;\n"
           "    std::function<void()> game_entry_callback_;\n"
           "    katana::runtime::SystemReplayLog* replay_log_ = nullptr;\n"
           "    katana::runtime::SystemReplayObservationSession replay_observations_;\n"
           "    katana::runtime::RuntimeProbeObservationState probe_observations_;\n"
           "    static constexpr std::uint64_t host_poll_guest_cycle_quantum = 200'000u;\n"
           "    std::uint64_t next_lifecycle_poll_guest_cycle_ = 0u;\n"
           "    std::uint64_t last_lifecycle_poll_guest_cycle_ = 0u;\n"
           "    std::uint64_t next_frame_poll_guest_cycle_ = 0u;\n"
           "    std::uint64_t last_frame_poll_guest_cycle_ = 0u;\n"
           "    mutable bool interrupt_sources_dirty_ = true;\n"
           "    mutable std::uint64_t synchronized_interrupt_router_epoch_ = 0u;\n"
           "    mutable katana::runtime::ExecutableChainRejectionReason\n"
           "        last_executable_chain_rejection_ =\n"
           "            katana::runtime::ExecutableChainRejectionReason::None;\n"
           "    katana::runtime::PlatformLifecycleState cached_lifecycle_ =\n"
           "        katana::runtime::PlatformLifecycleState::Running;\n"
           "    AtomicBatchCommitKind active_atomic_batch_commit_ =\n"
           "        AtomicBatchCommitKind::None;\n"
           "    std::uint64_t executed_blocks_ = 0u;\n"
           "    std::uint64_t fallback_count_ = 0u;\n"
           "    struct ExecutableBlockRegistration {\n"
           "        std::string identity;\n"
           "        std::uint32_t physical_origin = 0u;\n"
           "        std::uint32_t size = 0u;\n"
           "        katana::runtime::ExecutableBlockTimingClass timing_class =\n"
           "            katana::runtime::ExecutableBlockTimingClass::NeverChain;\n"
           "        std::uint64_t maximum_guest_cycles = 0u;\n"
           "        mutable std::uint64_t validated_code_generation = 0u;\n"
           "        bool chainable = false;\n"
           "    };\n"
           "    static constexpr std::uint32_t executable_chain_page_size = 4'096u;\n"
           "    static constexpr std::size_t executable_chain_halfwords_per_page =\n"
           "        executable_chain_page_size / 2u;\n"
           "    static constexpr std::size_t executable_chain_page_count =\n"
           "        0x2000'0000u / executable_chain_page_size;\n"
           "    struct ExecutableChainPage {\n"
           "        std::array<ExecutableBlockRegistration*,\n"
           "                   executable_chain_halfwords_per_page> entries{};\n"
           "    };\n"
           "    struct CountedLoopContract {\n"
           "        std::uint32_t counter_address = 0u;\n"
           "        std::uint32_t counter_physical = 0u;\n"
           "        std::uint32_t counter_backing_offset = 0u;\n"
           "        std::uint32_t limit_physical = 0u;\n"
           "        std::uint32_t limit_backing_offset = 0u;\n"
           "        std::uint32_t increment_physical = 0u;\n"
           "        std::uint32_t pre_store_physical = 0u;\n"
           "        std::uint32_t store_physical = 0u;\n"
           "        bool counter_is_on_chip_ram = false;\n"
           "    };\n"
           "    static bool counted_loop_provenance_matches(\n"
           "            const std::string_view actual,\n"
           "            const std::string_view expected) noexcept {\n"
           "        return actual == expected ||\n"
           "            (actual.size() == expected.size() + 12u &&\n"
           "             actual.starts_with(expected) &&\n"
           "             actual.ends_with(\"-mmu-variant\"));\n"
           "    }\n"
           "    static bool counted_loop_identity_matches_provenance(\n"
           "            std::string_view identity,\n"
           "            const std::string_view provenance) noexcept {\n"
           "        if (identity.ends_with(provenance)) return true;\n"
           "        constexpr std::string_view mmu_variant_suffix = \"-mmu-variant\";\n"
           "        if (!identity.ends_with(mmu_variant_suffix)) return false;\n"
           "        identity.remove_suffix(mmu_variant_suffix.size());\n"
           "        return identity.ends_with(provenance);\n"
           "    }\n"
           "    static bool same_counted_loop_contract(\n"
           "            const CountedLoopContract& left,\n"
           "            const CountedLoopContract& right) noexcept {\n"
           "        return left.counter_address == right.counter_address &&\n"
           "            left.counter_physical == right.counter_physical &&\n"
           "            left.counter_backing_offset == right.counter_backing_offset &&\n"
           "            left.limit_physical == right.limit_physical &&\n"
           "            left.limit_backing_offset == right.limit_backing_offset &&\n"
           "            left.increment_physical == right.increment_physical &&\n"
           "            left.pre_store_physical == right.pre_store_physical &&\n"
           "            left.store_physical == right.store_physical &&\n"
           "            left.counter_is_on_chip_ram == right.counter_is_on_chip_ram;\n"
           "    }\n"
           "    static bool counted_loop_direct_p1_p2_range(\n"
           "            const std::uint32_t address,\n"
           "            const std::size_t size) noexcept {\n"
           "        if (size == 0u || size > 0x1'0000'0000ull - address)\n"
           "            return false;\n"
           "        const auto first_segment = address >> 29u;\n"
           "        if (first_segment != 4u && first_segment != 5u)\n"
           "            return false;\n"
           "        const auto last_address = static_cast<std::uint32_t>(\n"
           "            static_cast<std::uint64_t>(address) + size - 1u);\n"
           "        return (last_address >> 29u) == first_segment;\n"
           "    }\n"
           "    bool counted_loop_range_has_no_mmu_side_effects(\n"
           "            const std::uint32_t address,\n"
           "            const std::size_t size) const noexcept {\n"
           "        if (!state_.address_space) return false;\n"
           "        if (state_.address_space->mode() ==\n"
           "                katana::runtime::AddressTranslationMode::NoMmu)\n"
           "            return true;\n"
           "        if (state_.address_space->mode() ==\n"
           "                katana::runtime::AddressTranslationMode::Mmu &&\n"
           "            counted_loop_direct_p1_p2_range(address, size))\n"
           "            return true;\n"
           "        return counted_loop_batch_rejected(\"active-mmu-nondirect-range\");\n"
           "    }\n"
           "    bool runtime_state_allows_counted_loop_batch() const noexcept {\n"
           "        return (cpu_.interrupts_blocked() || cpu_.interrupt_mask() == 15u) &&\n"
           "            cpu_.memory.watchpoint_count() == 0u &&\n"
           "            !cpu_.memory.has_trace_handler() &&\n"
           "            !cpu_.memory.has_mmio_trace_handler() &&\n"
           "            !cpu_.memory.mmio_access_tracking_enabled() &&\n"
           "            !cpu_.memory.has_guest_memory_access_sink() &&\n"
           "            cpu_.memory.guest_write_observer_allows_prevalidated_linear_writes() &&\n"
           "            cpu_.memory.lookup_mode() ==\n"
           "                katana::runtime::MemoryLookupMode::Indexed &&\n"
           "            state_.main_ram && state_.cache_control &&\n"
           "            state_.main_ram->size() ==\n"
           "                katana::runtime::dreamcast_main_ram_size &&\n"
           "            state_.address_space && cpu_.address_space &&\n"
           "            cpu_.address_space.get() == state_.address_space.get() &&\n"
           "            (state_.address_space->mode() ==\n"
           "                 katana::runtime::AddressTranslationMode::NoMmu ||\n"
           "             state_.address_space->mode() ==\n"
           "                 katana::runtime::AddressTranslationMode::Mmu) &&\n"
           "            state_.scheduler && state_.code_tracker &&\n"
           "            state_.runtime_blocks && state_.module_catalog &&\n"
           "            state_.disc_load_transactions &&\n"
           "            !state_.disc_load_transactions->transaction_active() &&\n"
           "            active_block_variant_.has_value();\n"
           "    }\n"
           "    bool proves_counted_instruction_block(\n"
           "            const std::uint32_t address,\n"
           "            const ExecutableBlockRegistration& registration) const noexcept {\n"
           "        if (!active_block_variant_) return false;\n"
           "        if (!counted_loop_range_has_no_mmu_side_effects(\n"
           "                address, registration.size))\n"
           "            return false;\n"
           "        try {\n"
           "            if (state_.address_space->instruction_translation_path(\n"
           "                    address, cpu_.privileged_mode()) !=\n"
           "                katana::runtime::InstructionTranslationPath::Direct)\n"
           "                return false;\n"
           "            const auto inspected = state_.address_space->inspect_translation(\n"
           "                address, katana::runtime::TranslationAccess::Instruction,\n"
           "                cpu_.privileged_mode());\n"
           "            if (katana::runtime::canonical_physical_address(\n"
           "                    inspected.physical_address) !=\n"
           "                    registration.physical_origin ||\n"
           "                !state_.address_space->prove_instruction_mapping(\n"
           "                    address, registration.physical_origin, registration.size,\n"
           "                    cpu_.privileged_mode()))\n"
           "                return false;\n"
           "            const auto variant = katana::runtime::block_variant_key(\n"
           "                state_.address_space->guard_for(\n"
           "                    address, cpu_.read_fpscr(), cpu_.privileged_mode()),\n"
           "                active_block_variant_->runtime_generation);\n"
           "            return variant == *active_block_variant_;\n"
           "        } catch (...) {\n"
           "            return false;\n"
           "        }\n"
           "    }\n"
           "    bool counted_loop_counter_aliases_are_inert(\n"
           "            const bool counter_is_on_chip_ram,\n"
           "            const std::uint32_t counter_address,\n"
           "            const std::uint32_t counter_physical,\n"
           "            const std::uint32_t counter_backing_offset) const noexcept {\n"
           "        if (counter_is_on_chip_ram) {\n"
           "            if (state_.runtime_blocks->may_overlap_active_physical(\n"
           "                    counter_physical, 4u) ||\n"
           "                state_.module_catalog->may_overlap_active_extent(\n"
           "                    counter_physical, 4u))\n"
           "                return false;\n"
           "            for (std::uint32_t offset = 0u; offset < 4u; ++offset)\n"
           "                if (state_.module_catalog->resolve(\n"
           "                        counter_address + offset, 1u) != nullptr ||\n"
           "                    state_.module_catalog->resolve(\n"
           "                        counter_physical + offset, 1u) != nullptr)\n"
           "                    return false;\n"
           "            return true;\n"
           "        }\n"
           "        if (counter_backing_offset >\n"
           "                katana::runtime::dreamcast_main_ram_size - 4u)\n"
           "            return false;\n"
           "        for (const auto area_base :\n"
           "             katana::runtime::dreamcast_main_ram_area_bases) {\n"
           "            for (std::size_t mirror = 0u;\n"
           "                 mirror < katana::runtime::dreamcast_main_ram_mirrors_per_area;\n"
           "                 ++mirror) {\n"
           "                const auto alias = static_cast<std::uint64_t>(area_base) +\n"
           "                    mirror * katana::runtime::dreamcast_main_ram_size +\n"
           "                    counter_backing_offset;\n"
           "                if (alias >\n"
           "                        std::numeric_limits<std::uint32_t>::max() - 3u)\n"
           "                    return false;\n"
           "                const auto alias_address = static_cast<std::uint32_t>(alias);\n"
           "                if (state_.runtime_blocks->may_overlap_active_physical(\n"
           "                        alias_address, 4u) ||\n"
           "                    state_.module_catalog->may_overlap_active_extent(\n"
           "                        alias_address, 4u))\n"
           "                    return false;\n"
           "                for (std::uint32_t offset = 0u; offset < 4u; ++offset)\n"
           "                    if (state_.module_catalog->resolve(\n"
           "                            alias_address + offset, 1u) != nullptr)\n"
           "                        return false;\n"
           "            }\n"
           "        }\n"
           "        return true;\n"
           "    }\n"
           "    std::optional<CountedLoopContract> prove_counted_loop_contract(\n"
           "            const " +
           entry_namespace +
           "::CountedLoopBatchDescriptor& descriptor,\n"
           "            const std::uint32_t selected_physical_origin,\n"
           "            const std::uint64_t increment_guest_cycles) {\n"
           "        if (!runtime_state_allows_counted_loop_batch())\n"
           "            return std::nullopt;\n"
           "        const auto guard = executable_blocks_.find(descriptor.guard_address);\n"
           "        const auto increment =\n"
           "            executable_blocks_.find(descriptor.increment_address);\n"
           "        if (guard == executable_blocks_.end() ||\n"
           "            increment == executable_blocks_.end() ||\n"
           "            guard->second.physical_origin != selected_physical_origin ||\n"
           "            guard->second.size != 8u ||\n"
           "            guard->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush ||\n"
           "            guard->second.maximum_guest_cycles !=\n"
           "                descriptor.guard_guest_cycles ||\n"
           "            increment->second.physical_origin !=\n"
           "                katana::runtime::canonical_physical_address(\n"
           "                    descriptor.increment_address) ||\n"
           "            increment->second.size != descriptor.increment_size ||\n"
           "            increment->second.timing_class !=\n"
           "                katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush ||\n"
           "            increment->second.maximum_guest_cycles != increment_guest_cycles ||\n"
           "            !counted_loop_identity_matches_provenance(\n"
           "                guard->second.identity, descriptor.guard_provenance) ||\n"
           "            !counted_loop_identity_matches_provenance(\n"
           "                increment->second.identity, descriptor.increment_provenance) ||\n"
           "            !state_.code_tracker->dispatchable(guard->second.identity) ||\n"
           "            !state_.code_tracker->dispatchable(increment->second.identity) ||\n"
           "            !proves_counted_instruction_block(\n"
           "                descriptor.guard_address, guard->second) ||\n"
           "            !proves_counted_instruction_block(\n"
           "                descriptor.increment_address, increment->second))\n"
           "            return std::nullopt;\n"
           "        if (descriptor.limit_width !=\n"
           "                (descriptor.signed_word_limit ? 2u : 4u))\n"
           "            return std::nullopt;\n"
           "        const auto counter_address_sum =\n"
           "            static_cast<std::int64_t>(\n"
           "                cpu_.r[descriptor.counter_base_register]) +\n"
           "            static_cast<std::int64_t>(descriptor.counter_displacement);\n"
           "        if (state_.address_space->mode() ==\n"
           "                katana::runtime::AddressTranslationMode::Mmu &&\n"
           "            (counter_address_sum < 0 ||\n"
           "             counter_address_sum >\n"
           "                 std::numeric_limits<std::uint32_t>::max())) {\n"
           "            static_cast<void>(counted_loop_batch_rejected(\n"
           "                \"active-mmu-address-overflow\"));\n"
           "            return std::nullopt;\n"
           "        }\n"
           "        const auto counter_address = static_cast<std::uint32_t>(\n"
           "            cpu_.r[descriptor.counter_base_register] +\n"
           "            static_cast<std::uint32_t>(descriptor.counter_displacement));\n"
           "        const auto limit_size =\n"
           "            static_cast<std::size_t>(descriptor.limit_width);\n"
           "        if ((counter_address & 3u) != 0u ||\n"
           "            descriptor.limit_address % descriptor.limit_width != 0u)\n"
           "            return std::nullopt;\n"
           "        if (!counted_loop_range_has_no_mmu_side_effects(\n"
           "                counter_address, 4u) ||\n"
           "            !counted_loop_range_has_no_mmu_side_effects(\n"
           "                descriptor.limit_address, limit_size))\n"
           "            return std::nullopt;\n"
           "        const bool counter_is_on_chip_ram =\n"
           "            (counter_address & 0xFC000000u) ==\n"
           "                katana::runtime::sh4_on_chip_ram_address;\n"
           "        const auto counter_read = counter_is_on_chip_ram\n"
           "            ? prove_on_chip_ram_translation(\n"
           "                  cpu_, state_, counter_address, 4u,\n"
           "                  katana::runtime::TranslationAccess::Read)\n"
           "            : prove_main_ram_translation(\n"
           "                  cpu_, state_, counter_address, 4u,\n"
           "                  katana::runtime::TranslationAccess::Read);\n"
           "        const auto counter_write = counter_is_on_chip_ram\n"
           "            ? prove_on_chip_ram_translation(\n"
           "                  cpu_, state_, counter_address, 4u,\n"
           "                  katana::runtime::TranslationAccess::Write)\n"
           "            : prove_main_ram_translation(\n"
           "                  cpu_, state_, counter_address, 4u,\n"
           "                  katana::runtime::TranslationAccess::Write);\n"
           "        const auto limit_read = prove_main_ram_translation(\n"
           "            cpu_, state_, descriptor.limit_address, limit_size,\n"
           "            katana::runtime::TranslationAccess::Read);\n"
           "        if (!counter_read || !counter_write || !limit_read ||\n"
           "            !counter_read->no_mmu_fastpath ||\n"
           "            !counter_write->no_mmu_fastpath ||\n"
           "            !limit_read->no_mmu_fastpath ||\n"
           "            counter_read->utlb_slot != 0xFFu ||\n"
           "            counter_write->utlb_slot != 0xFFu ||\n"
           "            limit_read->utlb_slot != 0xFFu ||\n"
           "            counter_read->physical_address !=\n"
           "                counter_write->physical_address ||\n"
           "            counter_read->mmu_generation !=\n"
           "                counter_write->mmu_generation)\n"
           "            return std::nullopt;\n"
           "        const auto counter_physical = counter_write->physical_address;\n"
           "        const auto counter_backing = counter_is_on_chip_ram\n"
           "            ? std::optional<std::uint32_t>{counter_physical -\n"
           "                  katana::runtime::sh4_on_chip_ram_address}\n"
           "            : dreamcast_main_ram_backing_offset(counter_physical, 4u);\n"
           "        const auto limit_backing = dreamcast_main_ram_backing_offset(\n"
           "            limit_read->physical_address, limit_size);\n"
           "        const bool counter_limit_overlap = !counter_is_on_chip_ram &&\n"
           "            counter_backing && limit_backing &&\n"
           "            *counter_backing < *limit_backing + limit_size &&\n"
           "            *limit_backing < *counter_backing + 4u;\n"
           "        if (!counter_backing || !limit_backing || counter_limit_overlap ||\n"
           "            (counter_is_on_chip_ram\n"
           "                 ? state_.code_tracker->tracks_address(counter_physical, 4u)\n"
           "                 : dreamcast_main_ram_backing_is_executable(\n"
           "                       *state_.code_tracker, *counter_backing, 4u)) ||\n"
           "            !counted_loop_counter_aliases_are_inert(\n"
           "                counter_is_on_chip_ram, counter_address, counter_physical,\n"
           "                *counter_backing))\n"
           "            return std::nullopt;\n"
           "        return CountedLoopContract{\n"
           "            counter_address, counter_physical, *counter_backing,\n"
           "            limit_read->physical_address, *limit_backing,\n"
           "            increment->second.physical_origin,\n"
           "            increment->second.physical_origin + 2u,\n"
           "            increment->second.physical_origin + 4u,\n"
           "            counter_is_on_chip_ram};\n"
           "    }\n"
           "    static constexpr std::uint64_t local_block_chain_guest_cycle_budget = 4'096u;\n"
           "    std::unordered_map<std::uint32_t, ExecutableBlockRegistration> "
           "executable_blocks_;\n"
           "    std::vector<std::unique_ptr<ExecutableChainPage>> executable_chain_pages_{\n"
           "        executable_chain_page_count};\n"
           "    std::optional<katana::runtime::BlockVariantKey> active_block_variant_;\n"
           "    const ExecutableBlockRegistration* active_block_registration_ = nullptr;\n"
           "    StaticAotChainGuard active_static_aot_chain_guard_;\n"
           "    FlagPollBatchCounters flag_poll_batch_counters_;\n"
           "    MmioWaitLoopBatchCounters mmio_wait_loop_batch_counters_;\n"
           "    katana::runtime::CrashCapsule crash_capsule_;\n"
           "    bool guest_program_dispatched_ = false;\n"
           "    bool guest_program_progressed_ = false;\n"
           "    std::uint64_t guest_program_entry_cycle_ = 0u;\n"
           "    std::uint64_t guest_program_progress_cycle_ = 0u;\n"
           "    std::optional<std::uint64_t> requested_post_entry_cycles_;\n"
           "    std::optional<std::uint64_t> product_target_guest_cycle_;\n"
           "    std::optional<std::chrono::steady_clock::time_point>\n"
           "        post_entry_host_started_;\n"
           "    std::uint64_t restored_guest_cycle_ = 0u;\n"
           "    ProductTerminalTelemetry* product_terminal_telemetry_ = nullptr;\n"
           "    bool product_budget_arm_failed_ = false;\n"
           "    bool eager_host_poll_ = false;\n"
           "    bool runtime_probe_mode_ = false;\n"
           "    bool local_block_chaining_enabled_ = false;\n"
           "    bool counted_loop_batching_enabled_ = false;\n"
           "    bool mmio_wait_loop_batching_enabled_ = false;\n"
           "    bool memory_fill_loop_batching_enabled_ = false;\n"
           "    bool composite_callback_batching_enabled_ = false;\n"
           "    bool game_entry_probe_enabled_ = [] {\n"
           "        const auto* value = std::getenv(\"KATANA_GAME_ENTRY_PROBE\");\n"
           "        return value != nullptr && std::string_view(value) == \"1\";\n"
           "    }();\n"
           "    bool game_entry_probe_emitted_ = false;\n"
           "    bool game_entry_callback_emitted_ = false;\n"
           "    bool game_entry_callback_failed_ = false;\n"
           "    bool product_entry_evidence_failed_ = false;\n"
           "    bool fault_emitted_ = false;\n"
           "};\n\n"
           "int run_deterministic_runtime_probe(\n"
           "        katana::runtime::CpuState& cpu,\n"
           "        const katana::runtime::DreamcastRuntimeState& state,\n"
           "        bool diagnostics_enabled,\n"
           "        RuntimeWaitLoopTraceSession& wait_loop_trace) {\n"
           "    katana::runtime::SystemReplayLog replay({\n"
           "        katana::runtime::SystemReplayConfig::default_capacity, false,\n"
           "        katana::runtime::SystemReplayProfile::DeterministicV1,\n"
           "        katana::runtime::SystemReplayStorageMode::DigestStream});\n"
           "    state.scheduler->attach_replay_log(replay);\n"
           "    replay.enable_coverage(static_cast<\n"
           "        katana::runtime::SystemReplayCoverageMask>(\n"
           "            katana::runtime::SystemReplayCoverage::SchedulerCallback));\n"
           "    auto input = std::make_shared<katana::runtime::InjectedHostInput>();\n"
           "    state.maple->attach(0u, 0u,\n"
           "        std::make_shared<katana::runtime::MapleControllerDevice>(input));\n"
           "    katana::runtime::RecordingHostAudioOutput audio;\n"
           "    PortPlatformServices services(\n"
           "        cpu, state, {}, {}, false, true, &replay);\n"
           "    katana::runtime::DreamcastMediaClock media_clock(\n"
           "        *state.scheduler, {},\n"
           "        [&](const katana::runtime::VideoTick& tick) {\n"
           "            record_runtime_probe_event(\n"
           "                replay, *state.scheduler,\n"
           "                katana::runtime::SystemReplayEventKind::Video,\n"
           "                \"video-tick\", tick.guest_cycle, tick.frame_index);\n"
           "        },\n"
           "        [&](const katana::runtime::AudioTick& tick) {\n"
           "            audio.submit(state.aica_registers->render_audio(\n"
           "                             tick.frame_count, 44'100u),\n"
           "                         44'100u);\n"
           "            record_runtime_probe_event(\n"
           "                replay, *state.scheduler,\n"
           "                katana::runtime::SystemReplayEventKind::Audio,\n"
           "                \"audio-tick\", tick.frame_count, tick.buffer_index);\n"
           "        });\n"
           "    replay.enable_coverage(\n"
           "        static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
           "            katana::runtime::SystemReplayCoverage::Video) |\n"
           "        static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
           "            katana::runtime::SystemReplayCoverage::Audio));\n"
           "    RuntimeProbeMmioTraceSession mmio_trace(cpu.memory, replay, *state.scheduler);\n"
           "    replay.enable_coverage(static_cast<\n"
           "        katana::runtime::SystemReplayCoverageMask>(\n"
           "            katana::runtime::SystemReplayCoverage::Input));\n"
           "    services.observe_runtime_started();\n"
           "    input->inject(1u, state.scheduler->current_cycle(), {});\n"
           "    replay.inject({\n"
           "        0u, state.scheduler->current_cycle(),\n"
           "        katana::runtime::SystemReplayEventKind::ExternalInput,\n"
           "        \"neutral-controller-input\", std::nullopt, std::nullopt,\n"
           "        0u, 0u, false, state.scheduler->reset_generation()});\n"
           "    media_clock.start();\n"
           "    auto termination = katana::runtime::RuntimeProbeTermination::Completed;\n"
           "    try {\n"
           "        static_cast<void>(" +
           entry_namespace +
           "::run_runtime(cpu, services, *state.runtime_blocks,\n"
           "                      services.observation_session(),\n"
           "                      services.crash_capsule()));\n"
           "    } catch (const katana::runtime::RuntimeProbeBudgetReached& reached) {\n"
           "        if (!reached.final_guest_cycle().has_value() ||\n"
           "            *reached.final_guest_cycle() != state.scheduler->current_cycle()) {\n"
           "            media_clock.stop();\n"
           "            throw std::runtime_error(\"runtime-probe-budget-cycle-mismatch\");\n"
           "        }\n"
           "        termination = katana::runtime::RuntimeProbeTermination::BudgetReached;\n"
           "    } catch (const katana::runtime::IndirectDispatchError&) {\n"
           "        media_clock.stop();\n"
           "        services.emit_runtime_fault(\n"
           "            katana::runtime::RuntimeProbeTermination::DispatchMiss);\n"
           "        return 1;\n"
           "    } catch (const std::exception&) {\n"
           "        media_clock.stop();\n"
           "        services.emit_runtime_fault(\n"
           "            katana::runtime::RuntimeProbeTermination::Failed);\n"
           "        return 1;\n"
           "    } catch (...) {\n"
           "        media_clock.stop();\n"
           "        services.emit_runtime_fault(\n"
           "            katana::runtime::RuntimeProbeTermination::Failed);\n"
           "        return 1;\n"
           "    }\n"
           "    media_clock.stop();\n"
           "    mmio_trace.finish();\n"
           "    wait_loop_trace.finish();\n"
           "    const auto dreamcast = katana::runtime::capture_runtime_probe_dreamcast(\n"
           "        state, audio.submitted_buffers(), audio.submitted_frames(),\n"
           "        audio.deterministic_hash());\n"
           "    const std::array memory{\n"
           "        katana::runtime::RuntimeProbeMemoryRange{\n"
           "            katana::runtime::RuntimeProbeMemoryRegion::MainRam, 0u,\n"
           "            state.main_ram->bytes()},\n"
           "        katana::runtime::RuntimeProbeMemoryRange{\n"
           "            katana::runtime::RuntimeProbeMemoryRegion::VideoRam, 0u,\n"
           "            state.vram->bytes()},\n"
           "        katana::runtime::RuntimeProbeMemoryRange{\n"
           "            katana::runtime::RuntimeProbeMemoryRegion::AicaRam, 0u,\n"
           "            state.aica_ram->bytes()}};\n"
           "    const std::array persistent{\n"
           "        katana::runtime::RuntimeProbeMemoryRange{\n"
           "            katana::runtime::RuntimeProbeMemoryRegion::Flash, 0u,\n"
           "            std::span<const std::uint8_t>(dreamcast.flash)},\n"
           "        katana::runtime::RuntimeProbeMemoryRange{\n"
           "            katana::runtime::RuntimeProbeMemoryRegion::Vmu, 0u,\n"
           "            std::span<const std::uint8_t>(dreamcast.vmu)}};\n"
           "    const auto cpu_snapshot = katana::runtime::capture_runtime_probe_cpu(cpu);\n"
           "    const auto scheduler_snapshot =\n"
           "        katana::runtime::capture_runtime_probe_scheduler(*state.scheduler);\n"
           "    auto replay_snapshot = katana::runtime::capture_runtime_probe_replay(replay);\n"
           "    const auto provisional = katana::runtime::make_runtime_probe_report(\n"
           "        cpu_snapshot, scheduler_snapshot, memory, persistent,\n"
           "        dreamcast.devices, replay_snapshot);\n"
           "    replay.seal(provisional.hashes.guest_state);\n"
           "    replay_snapshot = katana::runtime::capture_runtime_probe_replay(replay);\n"
           "    auto report = katana::runtime::make_runtime_probe_report(\n"
           "        cpu_snapshot, scheduler_snapshot, memory, persistent,\n"
           "        dreamcast.devices, replay_snapshot);\n"
           "    report.termination = termination;\n"
           "    report.diagnostics_enabled = diagnostics_enabled;\n"
           "    if (report.status != katana::runtime::RuntimeProbeStatus::Complete)\n"
           "        throw std::runtime_error(\"runtime-probe-incomplete\");\n"
           "    if (termination != katana::runtime::RuntimeProbeTermination::BudgetReached)\n"
           "        throw std::runtime_error(\"runtime-probe-budget-not-reached\");\n"
           "    if (!report.guest_cycle_budget.has_value() ||\n"
           "        report.guest_cycle != *report.guest_cycle_budget)\n"
           "        throw std::runtime_error(\"runtime-probe-budget-not-exact\");\n"
           "    std::cout << \"KATANA_RUNTIME_PROBE \"\n"
           "              << katana::runtime::serialize_runtime_probe_report_json(report)\n"
           "              << '\\n';\n"
           "    return 0;\n"
           "}\n\n"
           "std::string redact_source(std::string message, const std::filesystem::path& source) {\n"
           "    std::error_code path_error;\n"
           "    const auto absolute = std::filesystem::weakly_canonical(source, path_error);\n"
           "    if (path_error || absolute.empty()) return message;\n"
           "    std::vector<std::string> values;\n"
           "    const auto add = [&values](const std::filesystem::path& path) {\n"
           "        const auto value = path.lexically_normal().string();\n"
           "        if (value.size() < 4u || path == path.root_path() || path == \".\" || "
           "path == \"..\") return;\n"
           "        if (std::find(values.begin(), values.end(), value) == values.end()) "
           "values.push_back(value);\n"
           "    };\n"
           "    add(absolute);\n"
           "    add(absolute.parent_path());\n"
           "    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {\n"
           "        return left.size() > right.size();\n"
           "    });\n"
           "    for (const auto& value : values) {\n"
           "        for (auto offset = message.find(value); offset != std::string::npos;\n"
           "             offset = message.find(value, offset + 12u)) {\n"
           "            message.replace(offset, value.size(), \"<gdi-source>\");\n"
           "        }\n"
           "    }\n"
           "    return message;\n"
           "}\n"
           "} // namespace\n\n"
           "namespace " +
           entry_namespace +
           " {\n"
           "bool try_product_counted_loop_batch(\n"
           "        katana::runtime::CpuState&,\n"
           "        katana::runtime::PlatformServices& services,\n"
           "        const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "        const CountedLoopBatchDescriptor& descriptor) {\n"
           "    auto* const port = dynamic_cast<PortPlatformServices*>(&services);\n"
           "    return port != nullptr &&\n"
           "           port->try_counted_loop_batch(selected_block, descriptor);\n"
           "}\n"
           "bool try_product_mmio_wait_loop_batch(\n"
           "        katana::runtime::CpuState&,\n"
           "        katana::runtime::PlatformServices& services,\n"
           "        const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "        const MmioWaitLoopBatchDescriptor& descriptor) {\n"
           "    auto* const port = dynamic_cast<PortPlatformServices*>(&services);\n"
           "    return port != nullptr &&\n"
           "           port->try_mmio_wait_loop_batch(selected_block, descriptor);\n"
           "}\n"
           "bool try_product_memory_fill_loop_batch(\n"
           "        katana::runtime::CpuState&,\n"
           "        katana::runtime::PlatformServices& services,\n"
           "        const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "        const MemoryFillLoopBatchDescriptor& descriptor) {\n"
           "    auto* const port = dynamic_cast<PortPlatformServices*>(&services);\n"
           "    return port != nullptr &&\n"
           "           port->try_memory_fill_loop_batch(selected_block, descriptor);\n"
           "}\n"
           "bool try_product_composite_callback_batch(\n"
           "        katana::runtime::CpuState&,\n"
           "        katana::runtime::PlatformServices& services,\n"
           "        const katana::runtime::ValidatedBlockExecution& selected_block,\n"
           "        const CompositeCallbackBatchDescriptor& descriptor) {\n"
           "    auto* const port = dynamic_cast<PortPlatformServices*>(&services);\n"
           "    return port != nullptr && port->try_composite_callback_batch(\n"
           "        selected_block, descriptor);\n"
           "}\n"
           "} // namespace " +
           entry_namespace +
           "\n\n"
           "int main(const int argc, const char* const* argv) {\n"
           "    std::filesystem::path source;\n"
           "    ProductTerminalTelemetry terminal_telemetry;\n"
           "    auto emit_terminal_failure = [&]("
           "std::string_view first_problem,\n"
           "                                     std::uint32_t callsite = 0u,\n"
           "                                     std::uint32_t target = 0u,\n"
           "                                     std::uint32_t dispatch_error = 0u) {\n"
           "        if (terminal_telemetry.terminal_summary_emitted) return;\n"
           "        terminal_telemetry.terminal_summary_emitted = true;\n"
           "        const auto& materialization = " +
           entry_namespace +
           "::runtime_materialization_status();\n"
           "        const auto central_dispatches = " +
           entry_namespace +
           "::runtime_central_dispatch_count();\n"
           "        const auto post_entry_dispatches =\n"
           "            central_dispatches >= terminal_telemetry.central_dispatch_baseline\n"
           "                ? central_dispatches -\n"
           "                      terminal_telemetry.central_dispatch_baseline\n"
           "                : 0u;\n"
           "        const auto executed_post_entry_cycles =\n"
           "            terminal_telemetry.guest_program_started &&\n"
           "                    terminal_telemetry.final_guest_cycle >=\n"
           "                        terminal_telemetry.start_guest_cycle\n"
           "                ? terminal_telemetry.final_guest_cycle -\n"
           "                      terminal_telemetry.start_guest_cycle\n"
           "                : 0u;\n"
           "        const auto post_entry_guest_mhz =\n"
           "            terminal_telemetry.post_entry_host_seconds > 0.0\n"
           "                ? static_cast<double>(executed_post_entry_cycles) /\n"
           "                      terminal_telemetry.post_entry_host_seconds / 1'000'000.0\n"
           "                : 0.0;\n"
           "        std::cerr << \"KATANA_BRINGUP_RUN status=error\"\n"
           "                  << \" restored_guest_cycle=\"\n"
           "                  << terminal_telemetry.restored_guest_cycle\n"
           "                  << \" start_guest_cycle=\"\n"
           "                  << terminal_telemetry.start_guest_cycle\n"
           "                  << \" final_guest_cycle=\"\n"
           "                  << terminal_telemetry.final_guest_cycle\n"
           "                  << \" requested_post_entry_cycles=\"\n"
           "                  << terminal_telemetry.requested_post_entry_cycles\n"
           "                  << \" executed_post_entry_cycles=\"\n"
           "                  << executed_post_entry_cycles\n"
           "                  << \" target_guest_cycle=\"\n"
           "                  << terminal_telemetry.target_guest_cycle\n"
           "                  << \" host_seconds=\"\n"
           "                  << terminal_telemetry.post_entry_host_seconds\n"
           "                  << \" post_entry_guest_mhz=\" << post_entry_guest_mhz\n"
           "                  << \" milestone_bits=\"\n"
           "                  << terminal_telemetry.milestone_bits\n"
           "                  << \" post_entry_central_dispatches=\"\n"
           "                  << post_entry_dispatches\n"
           "                  << \" callsite=\" << callsite\n"
           "                  << \" target=\" << target\n"
           "                  << \" dispatch_error=\" << dispatch_error\n"
           "                  << \" materializer_failure=\"\n"
           "                  << materialization.first_failure\n"
           "                  << \" materializer_target=\"\n"
           "                  << materialization.first_failure_target\n"
           "                  << \" product_budget_arm_failed=\"\n"
           "                  << terminal_telemetry.product_budget_arm_failed\n"
           "                  << \" first_problem=\" << first_problem << '\\n';\n"
           "    };\n"
           "    try {\n"
           "        std::error_code executable_error;\n"
           "        auto executable = std::filesystem::weakly_canonical(argv[0], "
           "executable_error);\n"
           "        if (executable_error || executable.empty())\n"
           "            executable = std::filesystem::absolute(argv[0]);\n"
           "        const auto port_root = executable.parent_path();\n"
           "        const bool deterministic_runtime_probe =\n"
           "            deterministic_runtime_probe_requested();\n"
           "        validate_runtime_probe_environment(deterministic_runtime_probe);\n"
           "        if (expected_game_entry_handoff && deterministic_runtime_probe)\n"
           "            throw std::invalid_argument(\n"
           "                \"game-entry-handoff-runtime-probe-conflict\");\n"
           "        const auto configured_guest_cycle_budget =\n"
           "            katana::runtime::guest_cycle_budget_from_environment();\n"
           "        terminal_telemetry.requested_post_entry_cycles =\n"
           "            configured_guest_cycle_budget.value_or(0u);\n"
           "        validate_product_gate_environment(configured_guest_cycle_budget);\n"
           "        bool gdi_debug = false;\n"
           "        bool install_disc = false;\n"
           "        const auto recipe_path = port_root / \"content\" / \"game.katana-install\";\n"
           "        if (argc == 1) {\n"
           "            source = port_root / \"user-data\" / \"content\" / \"game.katana-disc\";\n"
           "        } else if (argc == 3 && std::string_view(argv[1]) == \"--content\") {\n"
           "            source = argv[2];\n"
           "        } else if (argc == 3 && std::string_view(argv[1]) == \"--install-disc\") {\n"
           "            source = argv[2]; install_disc = true;\n"
           "        } else if (argc == 3 && std::string_view(argv[1]) == \"--gdi-debug\") {\n"
           "            source = argv[2]; gdi_debug = true;\n"
           "        } else {\n"
           "            std::cerr << \"Aufruf: game [--install-disc <eigene.gdi>] "
           "[--content <lokaler-cache>] [--gdi-debug <disc.gdi>]\\n\";\n"
           "            return 2;\n"
           "        }\n"
           "        if (install_disc) {\n"
           "            try {\n"
           "                const auto recipe = "
           "katana::runtime::parse_disc_install_recipe(recipe_path);\n"
           "                verify_recipe_identity(recipe);\n"
           "                const auto destination = port_root / \"user-data\" / \"content\" / "
           "\"game.katana-disc\";\n"
           "                const auto info = katana::runtime::install_disc_content(recipe, "
           "source, destination);\n"
           "                auto packed = katana::runtime::PackedDiscSource::open(destination);\n"
           "                verify_pack_identity(*packed);\n"
           "                const auto boot = katana::runtime::load_dreamcast_runtime_boot(\n"
           "                    packed, packed->primary_data_lba(), "
           "packed->info().tracks.size(), packed->info().content_identity);\n"
           "                verify_boot_identity(boot);\n"
           "                std::cout << \"KATANA_DISC_INSTALL_OK tracks=\" << info.tracks.size()\n"
           "                          << \" sectors=\" << info.packed_sectors << '\\n';\n"
           "                return 0;\n"
           "            } catch (const std::exception&) {\n"
           "                throw std::runtime_error(\"source-identity-mismatch\");\n"
           "            }\n"
           "        }\n"
           "        katana::runtime::DreamcastRuntimeBootImage boot;\n"
           "        try {\n"
           "            if (gdi_debug) {\n"
           "                auto gdi = katana::runtime::GdiDiscSource::open(source);\n"
           "                boot = katana::runtime::load_dreamcast_runtime_boot(\n"
           "                    gdi, gdi->primary_data_lba(), "
           "gdi->descriptor().tracks.size(), std::string(expected_content_identity));\n"
           "                verify_source_identity(source, boot);\n"
           "            } else {\n"
           "                auto packed = katana::runtime::PackedDiscSource::open(source);\n"
           "                verify_pack_identity(*packed);\n"
           "                boot = katana::runtime::load_dreamcast_runtime_boot(\n"
           "                    packed, packed->primary_data_lba(), "
           "packed->info().tracks.size(), packed->info().content_identity);\n"
           "            }\n"
           "            verify_boot_identity(boot);\n"
           "        } catch (const std::exception&) { throw "
           "std::runtime_error(\"source-identity-mismatch\"); }\n"
           "        katana::runtime::DreamcastMutableStorageConfig storage_config;\n"
           "        storage_config.project_identity = expected_project_identity;\n"
           "        storage_config.storage_root = port_root / \"user-data\";\n"
           "        constexpr auto console_profile =\n"
           "            katana::runtime::DreamcastConsoleProfile::" +
           std::string(console_profile_enumerator(console_profile)) +
           ";\n"
           "        storage_config.region =\n"
           "            katana::runtime::dreamcast_region_for_console_profile(console_profile);\n"
           "        auto mutable_storage = katana::runtime::DreamcastMutableStorage::open(\n"
           "            std::move(storage_config));\n"
           "        katana::runtime::CpuState cpu;\n"
           + boot_configuration.str() +
           "        runtime_boot_config.executable_identity = {\n"
           "            std::string(expected_content_identity),\n"
           "            std::string(expected_boot_file_name),\n"
           "            \"sha256:\" + std::string(expected_boot_sha256)};\n"
           "        auto state = katana::runtime::initialize_dreamcast_runtime(\n"
           "            cpu, boot, runtime_boot_config,\n"
           "            mutable_storage, console_profile);\n"
           "        terminal_telemetry.restored_guest_cycle =\n"
           "            state.scheduler->current_cycle();\n"
           "        terminal_telemetry.final_guest_cycle =\n"
           "            state.scheduler->current_cycle();\n"
           "        if (deterministic_runtime_probe && configured_guest_cycle_budget)\n"
           "            state.scheduler->set_guest_cycle_budget(\n"
           "                *configured_guest_cycle_budget);\n"
           "        const auto* handoff_capture_value =\n"
           "            std::getenv(\"KATANA_GAME_ENTRY_HANDOFF_CAPTURE\");\n"
           "        const auto* handoff_apply_value =\n"
           "            std::getenv(\"KATANA_GAME_ENTRY_HANDOFF_APPLY_DIAGNOSTIC\");\n"
           "        const bool handoff_capture_requested = handoff_capture_value != nullptr &&\n"
           "            *handoff_capture_value != '\\0';\n"
           "        const bool handoff_apply_requested = handoff_apply_value != nullptr &&\n"
           "            *handoff_apply_value != '\\0';\n"
           "        if (expected_game_entry_handoff && handoff_apply_requested)\n"
           "            throw std::invalid_argument(\n"
           "                \"game-entry-handoff-product-diagnostic-conflict\");\n"
           "        if (handoff_capture_requested && handoff_apply_requested)\n"
           "            throw std::invalid_argument(\n"
           "                \"game-entry-handoff-capture-apply-conflict\");\n"
           "        std::optional<katana::runtime::GameEntryMemorySnapshot>\n"
           "            game_entry_initial_memory;\n"
           "        std::optional<std::filesystem::path> game_entry_capture_path;\n"
           "        bool game_entry_capture_attempted = false;\n"
           "        bool game_entry_capture_succeeded = false;\n"
           "        if (handoff_capture_requested) {\n"
           "            if (runtime_boot_config.boot_path !=\n"
           "                    katana::runtime::DreamcastRuntimeBootPath::NativeDiscBoot)\n"
           "                throw std::invalid_argument(\n"
           "                    \"game-entry-handoff-capture-requires-native-disc-boot\");\n"
           "            game_entry_capture_path =\n"
           "                std::filesystem::path(handoff_capture_value);\n"
           "            game_entry_initial_memory =\n"
           "                katana::runtime::capture_game_entry_memory_snapshot(state);\n"
           "        }\n"
           "        if (handoff_apply_requested) {\n"
           "            if (runtime_boot_config.boot_path !=\n"
           "                    katana::runtime::DreamcastRuntimeBootPath::DirectBootExecutable)\n"
           "                throw std::invalid_argument(\n"
           "                    \"game-entry-handoff-apply-requires-direct-boot\");\n"
           "            const auto artifact =\n"
           "                katana::runtime::GameEntryHandoffArtifact::load(\n"
           "                    std::filesystem::path(handoff_apply_value));\n"
           "            const auto& binding = artifact->descriptor().binding;\n"
           "            if (binding.schema_version !=\n"
           "                    katana::runtime::game_entry_handoff_schema_version ||\n"
           "                binding.required_runtime_abi != katana::runtime::abi_version ||\n"
           "                binding.required_platform_state_contract !=\n"
           "                    katana::runtime::game_entry_platform_state_contract_version ||\n"
           "                binding.executable.content_identity != expected_content_identity ||\n"
           "                binding.executable.boot_file_name != expected_boot_file_name ||\n"
           "                binding.executable.boot_byte_identity !=\n"
           "                    \"sha256:\" + std::string(expected_boot_sha256) ||\n"
           "                binding.console_profile != console_profile)\n"
           "                throw std::runtime_error(\n"
           "                    \"game-entry-handoff-executable-identity-mismatch\");\n"
           "            const std::array allowed_entry_ranges{\n"
           "                katana::runtime::GameEntryCodeRange{\n"
           "                    expected_guest_program_range.guest_start,\n"
           "                    expected_guest_program_range.byte_size}};\n"
           "            katana::runtime::GameEntryHandoffRequest request;\n"
           "            request.expected_binding = binding;\n"
           "            request.allowed_entry_ranges = allowed_entry_ranges;\n"
           "            request.memory_layout = {\n"
           "                static_cast<std::uint32_t>(state.main_ram->size()),\n"
           "                static_cast<std::uint32_t>(state.vram->size()),\n"
           "                static_cast<std::uint32_t>(state.aica_ram->size())};\n"
           "            const bool complete_platform_artifact =\n"
           "                artifact->descriptor().completeness ==\n"
           "                katana::runtime::GameEntryHandoffCompleteness::\n"
           "                    CompletePlatform;\n"
           "            request.required_devices = complete_platform_artifact\n"
           "                ? std::span<const katana::runtime::GameEntryDeviceRequirement>(\n"
           "                    katana::runtime::dreamcast_game_entry_required_devices_v2)\n"
           "                : std::span<const katana::runtime::GameEntryDeviceRequirement>{};\n"
           "            request.required_completeness =\n"
           "                artifact->descriptor().completeness;\n"
           "            auto validated =\n"
           "                katana::runtime::validate_and_stage_game_entry_handoff(\n"
           "                    request, artifact->provider());\n"
           "            if (complete_platform_artifact) {\n"
           "                const auto applied =\n"
           "                    katana::runtime::\n"
           "                        apply_validated_game_entry_complete_platform_handoff(\n"
           "                            cpu, state, validated,\n"
           "                            katana::runtime::\n"
           "                                GameEntryCompletePlatformRestoreProfile::\n"
           "                                    DiagnosticLossless);\n"
           "                std::cout\n"
           "                    << \"KATANA_GAME_ENTRY_HANDOFF_APPLY status=complete-platform-applied\"\n"
           "                    << \" operations=\" << applied.memory_operations_applied\n"
           "                    << \" bytes=\" << applied.memory_bytes_applied\n"
           "                    << \" devices=\" << applied.devices_applied\n"
           "                    << \" events=\" << applied.scheduler_events_rehydrated\n"
           "                    << \" source_guest_cycle=\"\n"
           "                    << validated.scheduler().current_cycle\n"
           "                    << \" runtime_guest_cycle=\"\n"
           "                    << state.scheduler->current_cycle()\n"
           "                    << \" platform_state=complete diagnostic=1\\n\";\n"
           "            } else {\n"
           "                const auto applied =\n"
           "                    katana::runtime::\n"
           "                        apply_validated_game_entry_cpu_memory_handoff(\n"
           "                            cpu, state, validated);\n"
           "                if (!applied.incomplete_handoff ||\n"
           "                    applied.complete_platform_state_applied())\n"
           "                    throw std::runtime_error(\n"
           "                        \"diagnostic-game-entry-handoff-unexpectedly-complete\");\n"
           "                std::cout\n"
           "                    << \"KATANA_GAME_ENTRY_HANDOFF_APPLY status=cpu-memory-applied\"\n"
           "                    << \" operations=\" << applied.memory_operations_applied\n"
           "                    << \" bytes=\" << applied.memory_bytes_applied\n"
           "                    << \" source_guest_cycle=\"\n"
           "                    << validated.scheduler().current_cycle\n"
           "                    << \" runtime_guest_cycle=\"\n"
           "                    << state.scheduler->current_cycle()\n"
           "                    << \" platform_state=pending diagnostic=1\\n\";\n"
           "            }\n"
           "        }\n"
           "#if defined(KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST) || \\\n"
           "    defined(KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST) || \\\n"
           "    defined(KATANA_INTERNAL_MEMORY_FILL_DIFFERENTIAL_TEST)\n"
           "        if (const auto* active_mmu =\n"
           "                std::getenv(\"KATANA_PORT_TEST_ACTIVE_MMU\");\n"
           "            active_mmu != nullptr && std::string_view(active_mmu) == \"1\") {\n"
           "            state.mmu_control->write(0x10u, 1u);\n"
           "            constexpr std::uint32_t test_tlb_flags = 0x00000174u;\n"
           "            cpu.pteh = 0x00100000u;\n"
           "            cpu.ptel = 0x0C100000u | test_tlb_flags;\n"
           "            cpu.ptea = 0u;\n"
           "            katana::runtime::load_tlb(cpu);\n"
           "            static_cast<void>(katana::runtime::translate_guest_address(\n"
           "                cpu, 0x00100000u,\n"
           "                katana::runtime::MemoryAccessOperation::Read,\n"
           "                katana::runtime::MemoryAccessWidth::Halfword, true));\n"
           "            if (const auto* mapped_counter =\n"
           "                    std::getenv(\"KATANA_PORT_TEST_MAPPED_COUNTER_SEGMENT\");\n"
           "                mapped_counter != nullptr) {\n"
           "                const auto mapped_counter_segment =\n"
           "                    std::string_view(mapped_counter);\n"
           "                const auto counter_virtual_base =\n"
           "                    mapped_counter_segment == \"p0\" ? 0x00200000u :\n"
           "                    mapped_counter_segment == \"p3\" ? 0xC0200000u : 0u;\n"
           "                if (counter_virtual_base == 0u)\n"
           "                    throw std::invalid_argument(\n"
           "                        \"counted-loop-test-mapped-segment-invalid\");\n"
           "                cpu.pteh = counter_virtual_base;\n"
           "                cpu.ptel = 0x0D000000u | test_tlb_flags;\n"
           "                cpu.ptea = 0u;\n"
           "                katana::runtime::load_tlb(cpu);\n"
           "                cpu.r[15] = counter_virtual_base;\n"
           "                static_cast<void>(katana::runtime::translate_guest_address(\n"
           "                    cpu, 0x00100000u,\n"
           "                    katana::runtime::MemoryAccessOperation::Read,\n"
           "                    katana::runtime::MemoryAccessWidth::Halfword, true));\n"
           "            }\n"
           "        }\n"
           "#endif\n"
           "        if (!gdi_debug) {\n"
           "            if (!state.disc_load_transactions)\n"
           "                throw std::runtime_error(\"disc-load-transactions-missing\");\n"
           "            " +
           entry_namespace +
           "::register_latent_aot_modules(\n"
           "                boot.content_identity, *state.disc_load_transactions);\n"
           "        }\n"
           "        const auto* diagnostics_value = std::getenv(\"KATANA_PORT_DIAGNOSTICS\");\n"
           "        const bool detailed_diagnostics = diagnostics_value != nullptr &&\n"
           "            std::string_view(diagnostics_value) == \"1\";\n"
           "        cpu.memory.set_mmio_access_tracking(detailed_diagnostics);\n"
           "        const auto* wait_loop_trace_value =\n"
           "            std::getenv(\"KATANA_PORT_WAIT_LOOP_TRACE\");\n"
           "        const bool wait_loop_trace_enabled = wait_loop_trace_value != nullptr &&\n"
           "            std::string_view(wait_loop_trace_value) == \"1\";\n"
           "        RuntimeWaitLoopTraceSession wait_loop_trace(\n"
           "            cpu.memory, wait_loop_trace_enabled);\n"
           "        if (deterministic_runtime_probe)\n"
           "            return run_deterministic_runtime_probe(\n"
           "                cpu, state, detailed_diagnostics, wait_loop_trace);\n"
           "        auto controller_input =\n"
           "            std::make_shared<katana::runtime::ControllerInputTimeline>();\n"
           "        state.maple->attach(0u, 0u,\n"
           "            std::make_shared<katana::runtime::MapleControllerDevice>(\n"
           "                controller_input));\n"
           + product_game_entry_handoff.str() +
           "        auto lifecycle_input =\n"
           "            std::make_shared<katana::runtime::InjectedHostInput>();\n"
           "        class ControllerContractGamepadSource final\n"
           "            : public katana::runtime::HostGamepadSource {\n"
           "          public:\n"
           "            [[nodiscard]] std::vector<katana::runtime::HostControllerSample>\n"
           "            poll() override {\n"
           "                ++poll_count_;\n"
           "                if (poll_count_ == 4u) return {};\n"
           "                katana::runtime::HostControllerSample sample;\n"
           "                sample.device_id = 1u;\n"
           "                sample.kind = katana::runtime::HostControllerKind::XInput;\n"
           "                sample.connected = true;\n"
           "                if (poll_count_ <= 2u ||\n"
           "                    (poll_count_ > 4u && (poll_count_ & 1u) == 0u)) {\n"
           "                    sample.buttons = katana::runtime::host_controller_button(\n"
           "                        katana::runtime::HostControllerButton::South);\n"
           "                    sample.left_x = std::numeric_limits<std::int16_t>::max();\n"
           "                    sample.left_trigger =\n"
           "                        std::numeric_limits<std::uint16_t>::max();\n"
           "                } else {\n"
           "                    sample.buttons = katana::runtime::host_controller_button(\n"
           "                        katana::runtime::HostControllerButton::East);\n"
           "                    sample.right_x = std::numeric_limits<std::int16_t>::max();\n"
           "                    sample.right_trigger =\n"
           "                        std::numeric_limits<std::uint16_t>::max();\n"
           "                }\n"
           "                last_sample_ = sample;\n"
           "                return {sample};\n"
           "            }\n"
           "            [[nodiscard]] std::size_t poll_count() const noexcept {\n"
           "                return poll_count_;\n"
           "            }\n"
           "            [[nodiscard]] katana::runtime::ControllerState expected_state() const {\n"
           "                return poll_count_ == 4u\n"
           "                    ? katana::runtime::ControllerState{}\n"
           "                    : katana::runtime::normalize_host_controller(last_sample_);\n"
           "            }\n"
           "          private:\n"
           "            std::size_t poll_count_ = 0u;\n"
           "            katana::runtime::HostControllerSample last_sample_{};\n"
           "        };\n"
           "        std::unique_ptr<katana::runtime::HostGamepadSource> gamepad_source;\n"
           "        ControllerContractGamepadSource* controller_contract_source = nullptr;\n"
           "        std::unique_ptr<katana::runtime::NativeVideoOutput> video;\n"
           "        if (katana::runtime::native_video_available()) {\n"
           "            video = katana::runtime::create_native_video_output(\n"
           "                {katana::runtime::native_video_contract_version,\n"
           "                 \"KatanaRecomp Game\", 640u, 480u, true});\n"
           "        }\n"
           "        katana::runtime::GuestFrameEvidenceTracker guest_frame_evidence;\n"
           "        struct ProductEvidenceCounters {\n"
           "            std::uint64_t central_dispatches = 0u;\n"
           "            std::uint64_t executed_blocks = 0u;\n"
           "            std::uint64_t pvr_render_requests = 0u;\n"
           "            std::uint64_t pvr_render_completions = 0u;\n"
           "            std::uint64_t pvr_renderer_frames = 0u;\n"
           "            std::uint64_t pvr_proven_guest_frames = 0u;\n"
           "            std::uint64_t pvr_direct_scanout_frames = 0u;\n"
           "            std::uint64_t pvr_direct_scanout_changed_pixels = 0u;\n"
           "            std::uint64_t pvr_changed_pixels = 0u;\n"
           "            std::uint64_t pvr_ta_packets = 0u;\n"
           "            std::uint64_t pvr_ta_vertices = 0u;\n"
           "            std::uint64_t pvr_ta_frames = 0u;\n"
           "            std::uint64_t pvr_yuv_macroblocks = 0u;\n"
           "            std::uint64_t aica_rendered_buffers = 0u;\n"
           "            std::uint64_t aica_rendered_frames = 0u;\n"
           "            std::uint64_t host_presented_frames = 0u;\n"
           "            std::uint64_t host_audio_submitted_buffers = 0u;\n"
           "            std::uint64_t host_audio_submitted_frames = 0u;\n"
           "        };\n"
           "        ProductEvidenceCounters product_entry_evidence_baseline;\n"
           "        bool product_entry_evidence_ready = false;\n"
           "        bool first_presented_frame_reported = false;\n"
           "        bool first_game_framebuffer_write = false;\n"
           "        bool first_game_ta_frame = false;\n"
           "        bool first_visible_game_frame = false;\n"
           "        bool first_ip_bin_visible_frame = false;\n"
           "        std::uint64_t first_game_framebuffer_write_cycle = 0u;\n"
           "        std::uint64_t first_game_ta_frame_cycle = 0u;\n"
           "        std::uint64_t first_visible_game_frame_cycle = 0u;\n"
           "        std::uint64_t first_ip_bin_visible_frame_cycle = 0u;\n"
           "        std::uint64_t host_sequence = 1u;\n"
           "        PortPlatformServices* runtime_observer = nullptr;\n"
           "        katana::runtime::ControllerState controller;\n"
           "        const auto* lifecycle_test_value = "
           "std::getenv(\"KATANA_PORT_LIFECYCLE_TEST\");\n"
           "        const auto* ignore_focus_value = std::getenv(\"KATANA_PORT_IGNORE_FOCUS\");\n"
           "        const bool ignore_focus = ignore_focus_value != nullptr &&\n"
           "            std::string_view(ignore_focus_value) == \"1\";\n"
           "        const std::string_view lifecycle_test = lifecycle_test_value == nullptr\n"
           "            ? std::string_view{} : std::string_view(lifecycle_test_value);\n"
           "        const auto* controller_test_value =\n"
           "            std::getenv(\"KATANA_PORT_CONTROLLER_TEST\");\n"
           "        const bool controller_contract_test = controller_test_value != nullptr &&\n"
           "            std::string_view(controller_test_value) == \"1\";\n"
           "        if (controller_contract_test) {\n"
           "            auto source = std::make_unique<ControllerContractGamepadSource>();\n"
           "            controller_contract_source = source.get();\n"
           "            gamepad_source = std::move(source);\n"
           "        } else if (lifecycle_test.empty() &&\n"
           "            katana::runtime::native_gamepad_input_available())\n"
           "            gamepad_source = katana::runtime::create_native_gamepad_source();\n"
           "        std::size_t lifecycle_test_step = 0u;\n"
           "        katana::runtime::HostPacer pacer;\n"
           "        std::unique_ptr<katana::runtime::HostAudioOutput> audio =\n"
           "            katana::runtime::native_audio_available()\n"
           "                ? katana::runtime::create_native_audio_output()\n"
           "                : std::make_unique<katana::runtime::RecordingHostAudioOutput>();\n"
           "        const auto capture_product_evidence_counters = [&]() noexcept {\n"
           "            ProductEvidenceCounters current;\n"
           "            current.central_dispatches = " +
           entry_namespace +
           "::runtime_central_dispatch_count();\n"
           "            current.executed_blocks = runtime_observer != nullptr\n"
           "                ? runtime_observer->executed_blocks() : 0u;\n"
           "            current.pvr_render_requests =\n"
           "                state.pvr_registers->render_request_count();\n"
           "            current.pvr_render_completions =\n"
           "                state.pvr_registers->render_completion_count();\n"
           "            const auto& renderer = state.pvr_renderer->metrics();\n"
           "            current.pvr_renderer_frames = renderer.frames;\n"
           "            current.pvr_proven_guest_frames = renderer.proven_guest_frames;\n"
           "            current.pvr_direct_scanout_frames =\n"
           "                renderer.direct_scanout_frames;\n"
           "            current.pvr_direct_scanout_changed_pixels =\n"
           "                renderer.direct_scanout_changed_pixels;\n"
           "            current.pvr_changed_pixels = renderer.changed_pixels;\n"
           "            const auto& ta = state.pvr_ta_fifo->metrics();\n"
           "            current.pvr_ta_packets = ta.packets;\n"
           "            current.pvr_ta_vertices = ta.vertices;\n"
           "            current.pvr_ta_frames = ta.frames;\n"
           "            current.pvr_yuv_macroblocks =\n"
           "                state.pvr_yuv_converter->converted_macroblocks();\n"
           "            current.aica_rendered_buffers =\n"
           "                state.aica_registers->rendered_buffer_count();\n"
           "            current.aica_rendered_frames =\n"
           "                state.aica_registers->rendered_frame_count();\n"
           "            current.host_presented_frames =\n"
           "                video ? video->presented_frames() : 0u;\n"
           "            current.host_audio_submitted_buffers =\n"
           "                audio->submitted_buffers();\n"
           "            current.host_audio_submitted_frames =\n"
           "                audio->submitted_frames();\n"
           "            return current;\n"
           "        };\n"
           "        const auto saturating_counter_delta = [](\n"
           "                const std::uint64_t current,\n"
           "                const std::uint64_t baseline) noexcept {\n"
           "            return current >= baseline ? current - baseline : 0u;\n"
           "        };\n"
           "        const auto capture_post_entry_product_evidence = [&]() noexcept {\n"
           "            ProductEvidenceCounters delta;\n"
           "            if (!product_entry_evidence_ready) return delta;\n"
           "            const auto current = capture_product_evidence_counters();\n"
           "            delta.central_dispatches = saturating_counter_delta(\n"
           "                current.central_dispatches,\n"
           "                product_entry_evidence_baseline.central_dispatches);\n"
           "            delta.executed_blocks = saturating_counter_delta(\n"
           "                current.executed_blocks,\n"
           "                product_entry_evidence_baseline.executed_blocks);\n"
           "            delta.pvr_render_requests = saturating_counter_delta(\n"
           "                current.pvr_render_requests,\n"
           "                product_entry_evidence_baseline.pvr_render_requests);\n"
           "            delta.pvr_render_completions = saturating_counter_delta(\n"
           "                current.pvr_render_completions,\n"
           "                product_entry_evidence_baseline.pvr_render_completions);\n"
           "            delta.pvr_renderer_frames = saturating_counter_delta(\n"
           "                current.pvr_renderer_frames,\n"
           "                product_entry_evidence_baseline.pvr_renderer_frames);\n"
           "            delta.pvr_proven_guest_frames = saturating_counter_delta(\n"
           "                current.pvr_proven_guest_frames,\n"
           "                product_entry_evidence_baseline.pvr_proven_guest_frames);\n"
           "            delta.pvr_direct_scanout_frames = saturating_counter_delta(\n"
           "                current.pvr_direct_scanout_frames,\n"
           "                product_entry_evidence_baseline.pvr_direct_scanout_frames);\n"
           "            delta.pvr_direct_scanout_changed_pixels =\n"
           "                saturating_counter_delta(\n"
           "                    current.pvr_direct_scanout_changed_pixels,\n"
           "                    product_entry_evidence_baseline.\n"
           "                        pvr_direct_scanout_changed_pixels);\n"
           "            delta.pvr_changed_pixels = saturating_counter_delta(\n"
           "                current.pvr_changed_pixels,\n"
           "                product_entry_evidence_baseline.pvr_changed_pixels);\n"
           "            delta.pvr_ta_packets = saturating_counter_delta(\n"
           "                current.pvr_ta_packets,\n"
           "                product_entry_evidence_baseline.pvr_ta_packets);\n"
           "            delta.pvr_ta_vertices = saturating_counter_delta(\n"
           "                current.pvr_ta_vertices,\n"
           "                product_entry_evidence_baseline.pvr_ta_vertices);\n"
           "            delta.pvr_ta_frames = saturating_counter_delta(\n"
           "                current.pvr_ta_frames,\n"
           "                product_entry_evidence_baseline.pvr_ta_frames);\n"
           "            delta.pvr_yuv_macroblocks = saturating_counter_delta(\n"
           "                current.pvr_yuv_macroblocks,\n"
           "                product_entry_evidence_baseline.pvr_yuv_macroblocks);\n"
           "            delta.aica_rendered_buffers = saturating_counter_delta(\n"
           "                current.aica_rendered_buffers,\n"
           "                product_entry_evidence_baseline.aica_rendered_buffers);\n"
           "            delta.aica_rendered_frames = saturating_counter_delta(\n"
           "                current.aica_rendered_frames,\n"
           "                product_entry_evidence_baseline.aica_rendered_frames);\n"
           "            delta.host_presented_frames = saturating_counter_delta(\n"
           "                current.host_presented_frames,\n"
           "                product_entry_evidence_baseline.host_presented_frames);\n"
           "            delta.host_audio_submitted_buffers =\n"
           "                saturating_counter_delta(\n"
           "                    current.host_audio_submitted_buffers,\n"
           "                    product_entry_evidence_baseline.\n"
           "                        host_audio_submitted_buffers);\n"
           "            delta.host_audio_submitted_frames = saturating_counter_delta(\n"
           "                current.host_audio_submitted_frames,\n"
           "                product_entry_evidence_baseline.host_audio_submitted_frames);\n"
           "            return delta;\n"
           "        };\n"
           "        katana::runtime::DreamcastMediaClock media_clock(\n"
           "            *state.scheduler, {},\n"
           "            [&](const katana::runtime::VideoTick& tick) {\n"
           "                pacer.pace(tick.guest_cycle);\n"
           "            },\n"
           "            [&](const katana::runtime::AudioTick& tick) {\n"
           "                audio->submit(state.aica_registers->render_audio(\n"
           "                                  tick.frame_count, 44'100u),\n"
           "                              44'100u);\n"
           "            });\n"
           "        katana::runtime::HostRuntimeSession host(\n"
           "            *state.scheduler, media_clock, lifecycle_input, *audio, &pacer,\n"
           "            [mutable_storage] { mutable_storage->save(); });\n"
           "        host.inject({host_sequence++, state.scheduler->current_cycle(),\n"
           "                     katana::runtime::HostRuntimeEventKind::Resume, {}});\n"
           "        std::uint64_t controller_changes = 0u;\n"
           "        std::uint32_t controller_contract = 0u;\n"
           "        const auto forward_controller_change =\n"
           "            [&](const std::optional<katana::runtime::ControllerInputChange>& change) {\n"
           "                if (!change) return;\n"
           "                ++controller_changes;\n"
           "                host.inject({host_sequence++, change->guest_cycle,\n"
           "                             katana::runtime::HostRuntimeEventKind::Controller,\n"
           "                             change->state});\n"
           "            };\n"
           "        const auto observe_controller_contract =\n"
           "            [&](const katana::runtime::ControllerState& expected,\n"
           "                const std::uint32_t observation) {\n"
           "                const auto response = state.maple->exchange_at(\n"
           "                    0u, 0u, {katana::runtime::MapleCommand::GetCondition, {}},\n"
           "                    state.scheduler->current_cycle());\n"
           "                const auto expected_condition =\n"
           "                    static_cast<std::uint32_t>(static_cast<std::uint16_t>(\n"
           "                        ~expected.pressed_buttons)) |\n"
           "                    (static_cast<std::uint32_t>(expected.right_trigger) << 16u) |\n"
           "                    (static_cast<std::uint32_t>(expected.left_trigger) << 24u);\n"
           "                const auto expected_axes =\n"
           "                    static_cast<std::uint32_t>(expected.joystick_x) |\n"
           "                    (static_cast<std::uint32_t>(expected.joystick_y) << 8u) |\n"
           "                    (static_cast<std::uint32_t>(expected.joystick2_x) << 16u) |\n"
           "                    (static_cast<std::uint32_t>(expected.joystick2_y) << 24u);\n"
           "                if (response.code != katana::runtime::MapleResponseCode::DataTransfer ||\n"
           "                    response.payload.size() != 3u ||\n"
           "                    response.payload[0] != 0x01000000u ||\n"
           "                    response.payload[1] != expected_condition ||\n"
           "                    response.payload[2] != expected_axes)\n"
           "                    throw std::runtime_error(\"controller-contract-maple-mismatch\");\n"
           "                controller_contract |= observation;\n"
           "            };\n"
           "        const auto pump_guest_frame = [&] {\n"
           "            const auto result = katana::runtime::pump_guest_frame_proof(\n"
           "                *state.pvr_renderer, video.get());\n"
           "            const bool game_code_progressed =\n"
           "                product_entry_evidence_ready &&\n"
           "                runtime_observer != nullptr &&\n"
           "                runtime_observer->guest_program_progressed();\n"
           "            const auto evidence = guest_frame_evidence.observe(\n"
           "                result, game_code_progressed);\n"
           "            if (game_code_progressed && !first_game_framebuffer_write &&\n"
           "                evidence.proof_source ==\n"
           "                    katana::runtime::PvrGuestFrameProofSource::DirectFramebuffer &&\n"
           "                evidence.write_generation_first != 0u) {\n"
           "                first_game_framebuffer_write = true;\n"
           "                first_game_framebuffer_write_cycle =\n"
           "                    state.scheduler->current_cycle();\n"
           "                terminal_telemetry.milestone_bits |= 1u << 2u;\n"
           "                std::cout << \"KR_FIRST_GAME_FRAMEBUFFER_WRITE\\n\";\n"
           "            }\n"
           "            if (game_code_progressed && !first_game_ta_frame &&\n"
           "                evidence.proof_source ==\n"
           "                    katana::runtime::PvrGuestFrameProofSource::TaRender) {\n"
           "                first_game_ta_frame = true;\n"
           "                first_game_ta_frame_cycle = state.scheduler->current_cycle();\n"
           "                terminal_telemetry.milestone_bits |= 1u << 3u;\n"
           "                std::cout << \"KR_FIRST_GAME_TA_FRAME\\n\";\n"
           "            }\n"
           "            if (katana::runtime::has_guest_frame_evidence_marker(\n"
           "                    evidence.markers,\n"
           "                    katana::runtime::GuestFrameEvidenceMarker::FirstGuestScanout)) {\n"
           "                std::cout << \"KR_FIRST_GUEST_SCANOUT\\n\";\n"
           "                std::cout << \"KR_FIRST_GUEST_FRAME\\n\";\n"
           "                if (runtime_observer != nullptr)\n"
           "                    runtime_observer->observe_first_guest_frame();\n"
           "            }\n"
           "            if (katana::runtime::has_guest_frame_evidence_marker(\n"
           "                    evidence.markers,\n"
           "                    katana::runtime::GuestFrameEvidenceMarker::FirstTaFrame)) {\n"
           "                std::cout << \"KR_FIRST_TA_FRAME\\n\";\n"
           "            }\n"
           "            if (katana::runtime::has_guest_frame_evidence_marker(\n"
           "                    evidence.markers,\n"
           "                    katana::runtime::GuestFrameEvidenceMarker::"
           "FirstPostBootstrapTaFrame))\n"
           "                std::cout << \"KR_FIRST_POST_BOOTSTRAP_TA_FRAME\\n\";\n"
           "            if (result.frame_presented &&\n"
           "                !first_presented_frame_reported) {\n"
           "                std::cout << \"KR_FIRST_PRESENTED_FRAME\\n\";\n"
           "                first_presented_frame_reported = true;\n"
           "            }\n"
           "            if (result.proven_frame_presented &&\n"
           "                game_code_progressed &&\n"
           "                !first_visible_game_frame) {\n"
           "                first_visible_game_frame = true;\n"
           "                first_visible_game_frame_cycle =\n"
           "                    state.scheduler->current_cycle();\n"
           "                terminal_telemetry.milestone_bits |= 1u << 4u;\n"
           "                std::cout << \"KR_FIRST_VISIBLE_GAME_FRAME\\n\";\n"
           "            } else if (result.proven_frame_presented &&\n"
           "                       (runtime_observer == nullptr ||\n"
           "                        !runtime_observer->guest_program_dispatched()) &&\n"
           "                       runtime_boot_config.boot_path ==\n"
           "                           katana::runtime::DreamcastRuntimeBootPath::NativeDiscBoot &&\n"
           "                       !first_ip_bin_visible_frame) {\n"
           "                first_ip_bin_visible_frame = true;\n"
           "                first_ip_bin_visible_frame_cycle =\n"
           "                    state.scheduler->current_cycle();\n"
           "                terminal_telemetry.milestone_bits |= 1u << 6u;\n"
           "                std::cout << \"KR_FIRST_IP_BIN_VISIBLE_FRAME\\n\";\n"
           "            }\n"
           "        };\n"
           "        const auto pump_host_events = [&] {\n"
           "            if (host.state() == katana::runtime::HostRuntimeState::Shutdown)\n"
           "                return;\n"
           "            if (!lifecycle_test.empty()) {\n"
           "                const auto inject = [&](katana::runtime::HostRuntimeEventKind kind) {\n"
           "                    host.inject({host_sequence++, state.scheduler->current_cycle(),\n"
           "                                 kind, controller});\n"
           "                };\n"
           "                if (lifecycle_test == \"running-close\") {\n"
           "                    if (lifecycle_test_step++ == 0u) {\n"
           "                        controller.pressed_buttons = 0x0004u;\n"
           "                        inject(katana::runtime::HostRuntimeEventKind::Controller);\n"
           "                        controller = {};\n"
           "                        inject(katana::runtime::HostRuntimeEventKind::Controller);\n"
           "                    } else inject(katana::runtime::HostRuntimeEventKind::Shutdown);\n"
           "                } else if (lifecycle_test == \"focus-resume-close\") {\n"
           "                    const auto step = lifecycle_test_step++;\n"
           "                    if (step == 0u) "
           "inject(katana::runtime::HostRuntimeEventKind::FocusLost);\n"
           "                    else if (step == 1u) "
           "inject(katana::runtime::HostRuntimeEventKind::FocusGained);\n"
           "                    else inject(katana::runtime::HostRuntimeEventKind::Shutdown);\n"
           "                } else if (lifecycle_test == \"paused-close\") {\n"
           "                    inject(lifecycle_test_step++ == 0u\n"
           "                        ? katana::runtime::HostRuntimeEventKind::FocusLost\n"
           "                        : katana::runtime::HostRuntimeEventKind::Shutdown);\n"
           "                } else {\n"
           "                    throw std::runtime_error(\"Unbekannter Lifecycle-Testmodus.\");\n"
           "                }\n"
           "                return;\n"
           "            }\n"
           "            if (video) {\n"
           "                video->poll_events();\n"
           "                for (const auto& event : video->drain_events()) {\n"
           "                    if (host.state() == "
           "katana::runtime::HostRuntimeState::Shutdown) break;\n"
           "                    const auto guest_cycle = state.scheduler->current_cycle();\n"
           "                    if (event.kind == "
           "katana::runtime::NativeHostEventKind::FocusGained) {\n"
           "                        if (controller_input->set_focus(true, guest_cycle))\n"
           "                            ++controller_changes;\n"
           "                        host.inject({host_sequence++, guest_cycle,\n"
           "                                     katana::runtime::HostRuntimeEventKind::"
           "FocusGained, {}});\n"
           "                        continue;\n"
           "                    }\n"
           "                    if (event.kind == "
           "katana::runtime::NativeHostEventKind::FocusLost) {\n"
           "                        if (ignore_focus) continue;\n"
           "                        if (controller_input->set_focus(false, guest_cycle))\n"
           "                            ++controller_changes;\n"
           "                        host.inject({host_sequence++, guest_cycle,\n"
           "                                     katana::runtime::HostRuntimeEventKind::"
           "FocusLost, {}});\n"
           "                        continue;\n"
           "                    }\n"
           "                    if (event.kind == katana::runtime::NativeHostEventKind::Close) {\n"
           "                        host.inject({host_sequence++, guest_cycle,\n"
           "                                     katana::runtime::HostRuntimeEventKind::Shutdown,\n"
           "                                     {}});\n"
           "                        continue;\n"
           "                    }\n"
           "                    const auto down =\n"
           "                        event.kind == katana::runtime::NativeHostEventKind::KeyDown;\n"
           "                    std::optional<katana::runtime::KeyboardControllerKey> key;\n"
           "                    switch (event.key) {\n"
           "                    case katana::runtime::NativeHostKey::Start:\n"
           "                        key = katana::runtime::KeyboardControllerKey::Start; break;\n"
           "                    case katana::runtime::NativeHostKey::A:\n"
           "                        key = katana::runtime::KeyboardControllerKey::A; break;\n"
           "                    case katana::runtime::NativeHostKey::B:\n"
           "                        key = katana::runtime::KeyboardControllerKey::B; break;\n"
           "                    case katana::runtime::NativeHostKey::X:\n"
           "                        key = katana::runtime::KeyboardControllerKey::X; break;\n"
           "                    case katana::runtime::NativeHostKey::Y:\n"
           "                        key = katana::runtime::KeyboardControllerKey::Y; break;\n"
           "                    case katana::runtime::NativeHostKey::Up:\n"
           "                        key = katana::runtime::KeyboardControllerKey::DpadUp; break;\n"
           "                    case katana::runtime::NativeHostKey::Down:\n"
           "                        key = katana::runtime::KeyboardControllerKey::DpadDown; break;\n"
           "                    case katana::runtime::NativeHostKey::Left:\n"
           "                        key = katana::runtime::KeyboardControllerKey::DpadLeft; break;\n"
           "                    case katana::runtime::NativeHostKey::Right:\n"
           "                        key = katana::runtime::KeyboardControllerKey::DpadRight; break;\n"
           "                    case katana::runtime::NativeHostKey::Unknown: break;\n"
           "                    }\n"
           "                    if (key)\n"
           "                        forward_controller_change(\n"
           "                            controller_input->keyboard_event(\n"
           "                                *key, down, guest_cycle));\n"
           "                }\n"
           "            }\n"
           "            if (gamepad_source && controller_input->focused() &&\n"
           "                host.state() != katana::runtime::HostRuntimeState::Shutdown) {\n"
           "                const auto poll_gamepad = [&] {\n"
           "                    const auto guest_cycle = state.scheduler->current_cycle();\n"
           "                    const auto change = controller_input->poll(\n"
           "                        *gamepad_source, guest_cycle);\n"
           "                if (controller_contract_test) {\n"
           "                    const auto poll_count = controller_contract_source->poll_count();\n"
           "                    if ((poll_count == 2u) != !change.has_value())\n"
           "                        throw std::runtime_error(\n"
           "                            \"controller-contract-change-dedup-mismatch\");\n"
           "                }\n"
           "                    forward_controller_change(change);\n"
           "                    if (controller_contract_test) {\n"
           "                    const auto poll_count = controller_contract_source->poll_count();\n"
           "                    const auto observation = poll_count == 1u ? 1u :\n"
           "                        poll_count == 2u ? 2u : poll_count == 4u ? 16u : 8u;\n"
           "                    observe_controller_contract(\n"
           "                        controller_contract_source->expected_state(), observation);\n"
           "                    if (poll_count == 2u) {\n"
           "                        const auto reset =\n"
           "                            controller_input->set_focus(false, guest_cycle);\n"
           "                        if (!reset)\n"
           "                            throw std::runtime_error(\n"
           "                                \"controller-contract-focus-reset-missing\");\n"
           "                        forward_controller_change(reset);\n"
           "                        observe_controller_contract(\n"
           "                            katana::runtime::ControllerState{}, 4u);\n"
           "                        if (controller_input->set_focus(true, guest_cycle))\n"
           "                            throw std::runtime_error(\n"
           "                                \"controller-contract-focus-resume-changed-state\");\n"
           "                    }\n"
           "                    }\n"
           "                };\n"
           "                poll_gamepad();\n"
           "                while (controller_contract_test &&\n"
           "                       controller_contract_source->poll_count() < 5u)\n"
           "                    poll_gamepad();\n"
           "            }\n"
           "        };\n"
           "        std::function<void()> game_entry_callback;\n"
           "        if (game_entry_capture_path) {\n"
           "            game_entry_callback = [&]() noexcept {\n"
           "                if (game_entry_capture_attempted) return;\n"
           "                game_entry_capture_attempted = true;\n"
           "                try {\n"
           "                    if (!game_entry_initial_memory ||\n"
           "                        cpu.pending_guest_cycles != 0u ||\n"
           "                        cpu.pc != expected_guest_program_range.guest_start ||\n"
           "                        !katana::runtime::\n"
           "                            guest_program_range_contains_instruction(\n"
           "                                cpu, cpu.pc,\n"
           "                                expected_guest_program_range))\n"
           "                        throw std::runtime_error(\n"
           "                            \"game-entry-capture-not-at-clean-entry\");\n"
           "                    auto delta =\n"
           "                        katana::runtime::capture_game_entry_memory_delta(\n"
           "                            *game_entry_initial_memory, state);\n"
           "                    auto platform =\n"
           "                        katana::runtime::\n"
           "                            capture_complete_game_entry_platform_state(\n"
           "                                state);\n"
           "                    katana::runtime::GameEntryHandoff handoff;\n"
           "                    handoff.binding.executable = {\n"
           "                        std::string(expected_content_identity),\n"
           "                        std::string(expected_boot_file_name),\n"
           "                        \"sha256:\" + std::string(expected_boot_sha256)};\n"
           "                    handoff.binding.console_profile = console_profile;\n"
           "                    handoff.completeness =\n"
           "                        katana::runtime::GameEntryHandoffCompleteness::\n"
           "                            CompletePlatform;\n"
           "                    handoff.transfer = {\n"
           "                        katana::runtime::GameEntryTransferKind::\n"
           "                            JumpPreservingPr,\n"
           "                        cpu.pc, cpu.pr};\n"
           "                    handoff.cpu =\n"
           "                        katana::runtime::capture_game_entry_cpu_state(cpu);\n"
           "                    handoff.memory_operations =\n"
           "                        std::move(delta.operations);\n"
           "                    handoff.devices = std::move(platform.devices);\n"
           "                    handoff.scheduler = std::move(platform.scheduler);\n"
           "                    std::vector<\n"
           "                        katana::runtime::GameEntryHandoffArtifactPayload>\n"
           "                        payloads;\n"
           "                    payloads.reserve(\n"
           "                        delta.payloads.size() + platform.payloads.size());\n"
           "                    for (const auto& payload : delta.payloads) {\n"
           "                        payloads.push_back({\n"
           "                            \"memory-\" + std::to_string(\n"
           "                                payload.memory_operation_index),\n"
           "                            katana::runtime::\n"
           "                                GameEntryHandoffArtifactPayloadTarget::\n"
           "                                    memory_operation(\n"
           "                                        payload.memory_operation_index),\n"
           "                            payload.bytes});\n"
           "                    }\n"
           "                    for (const auto& payload : platform.payloads) {\n"
           "                        payloads.push_back({\n"
           "                            payload.name,\n"
           "                            katana::runtime::\n"
           "                                GameEntryHandoffArtifactPayloadTarget::\n"
           "                                    device_payload(\n"
           "                                        payload.device,\n"
           "                                        payload.field_id),\n"
           "                            payload.bytes});\n"
           "                    }\n"
           "                    const auto artifact =\n"
           "                        katana::runtime::GameEntryHandoffArtifact::write(\n"
           "                            *game_entry_capture_path,\n"
           "                            std::move(handoff), payloads);\n"
           "                    game_entry_capture_succeeded = true;\n"
           "                    std::cout\n"
           "                        << \"KATANA_GAME_ENTRY_HANDOFF_CAPTURE status=ok\"\n"
           "                        << \" operations=\"\n"
           "                        << artifact->descriptor().memory_operations.size()\n"
           "                        << \" changed_bytes=\" << delta.changed_bytes\n"
           "                        << \" guest_cycle=\"\n"
           "                        << state.scheduler->current_cycle()\n"
           "                        << \" descriptor_identity=\"\n"
           "                        << artifact->descriptor().binding.descriptor_identity\n"
           "                        << \" devices=\"\n"
           "                        << artifact->descriptor().devices.size()\n"
           "                        << \" events=\"\n"
           "                        << artifact->descriptor().scheduler.pending_events.size()\n"
           "                        << \" platform_state=complete diagnostic=0\\n\";\n"
           "                } catch (const std::exception& error) {\n"
           "                    std::cerr\n"
           "                        << \"KATANA_GAME_ENTRY_HANDOFF_CAPTURE status=failed\"\n"
           "                        << \" error=\" << error.what() << '\\n';\n"
           "                } catch (...) {\n"
           "                    std::cerr\n"
           "                        << \"KATANA_GAME_ENTRY_HANDOFF_CAPTURE status=failed\"\n"
           "                        << \" error=unknown\\n\";\n"
           "                }\n"
           "            };\n"
           "        }\n"
           "        std::function<void()> product_entry_evidence_callback = [&] {\n"
           "            product_entry_evidence_ready = false;\n"
           "            guest_frame_evidence = {};\n"
           "            state.pvr_renderer->reset_guest_frame_evidence(\n"
           "                state.vram->bytes());\n"
           "            product_entry_evidence_baseline =\n"
           "                capture_product_evidence_counters();\n"
           "            // The central loop has selected, but not executed, the entry\n"
           "            // block when this boundary fires. Keep that first game dispatch\n"
           "            // in the post-entry interval.\n"
           "            if (product_entry_evidence_baseline.central_dispatches != 0u)\n"
           "                --product_entry_evidence_baseline.central_dispatches;\n"
           "            terminal_telemetry.central_dispatch_baseline =\n"
           "                product_entry_evidence_baseline.central_dispatches;\n"
           "            product_entry_evidence_ready = true;\n"
           "        };\n"
           "        PortPlatformServices services(cpu, state, [&] {\n"
           "            pump_host_events();\n"
           "            if (host.state() == katana::runtime::HostRuntimeState::Shutdown)\n"
           "                return katana::runtime::PlatformLifecycleState::Shutdown;\n"
           "            if (host.state() == katana::runtime::HostRuntimeState::Paused)\n"
           "                return katana::runtime::PlatformLifecycleState::Paused;\n"
           "            return katana::runtime::PlatformLifecycleState::Running;\n"
           "        }, pump_guest_frame, !lifecycle_test.empty(), false, nullptr,\n"
           "           std::move(product_entry_evidence_callback),\n"
           "           std::move(game_entry_callback),\n"
           "           configured_guest_cycle_budget, &terminal_telemetry);\n"
           "        runtime_observer = &services;\n"
           "        services.observe_runtime_started();\n"
           "        auto report_progress = [&] {\n"
           "            const auto gdrom_status = state.gdrom->status();\n"
           "            const auto g1_dma = state.holly_dma.g1->state();\n"
           "            const auto pvr_dma = state.holly_dma.pvr->state();\n"
           "            const auto pvr = state.pvr_registers->snapshot();\n"
           "            const auto system_bus = state.system_bus_control->snapshot();\n"
           "            const auto active_irq = state.interrupt_controller->highest_pending();\n"
           "            const auto post_entry =\n"
           "                capture_post_entry_product_evidence();\n"
           "            const auto& materialization = " +
           entry_namespace +
           "::runtime_materialization_status();\n"
           "            std::cerr << \"KATANA_PORT_PROGRESS pc=\" << cpu.pc\n"
           "                      << \" hardware_cycles=\" << state.scheduler->current_cycle()\n"
           "                      << \" post_entry_central_dispatches=\"\n"
           "                      << post_entry.central_dispatches\n"
           "                      << \" post_entry_executed_blocks=\"\n"
           "                      << post_entry.executed_blocks\n"
           "                      << \" retired_guest_instructions=\"\n"
           "                      << cpu.retired_guest_instructions\n"
           "                      << \" exception_cause=\"\n"
           "                      << static_cast<unsigned>(cpu.last_exception_cause)\n"
           "                      << \" expevt=\" << cpu.expevt\n"
           "                      << \" intevt=\" << cpu.intevt\n"
           "                      << \" tea=\" << cpu.tea\n"
           "                      << \" spc=\" << cpu.spc\n"
           "                      << \" ssr=\" << cpu.ssr\n"
           "                      << \" vbr=\" << cpu.vbr;\n"
           "            for (std::size_t register_index = 0u; register_index < cpu.r.size();\n"
           "                 ++register_index)\n"
           "                std::cerr << \" r\" << register_index << '='\n"
           "                          << cpu.r[register_index];\n"
           "            std::cerr << \" pr=\" << cpu.pr\n"
           "                      << \" gbr=\" << cpu.gbr\n"
           "                      << \" sr=\" << cpu.read_sr()\n"
           "                      << \" fpscr=\" << cpu.read_fpscr()\n"
           "                      << \" mach=\" << cpu.mach\n"
           "                      << \" macl=\" << cpu.macl\n"
           "                      << \" fpul=\" << cpu.fpul\n"
           "                      << \" post_entry_host_presented_frames=\"\n"
           "                      << post_entry.host_presented_frames\n"
           "                      << \" ta_transfers=\" << state.store_queue_transfers->size()\n"
           "                      << \" ta_transfers_dropped=\"\n"
           "                      << *state.dropped_store_queue_transfers\n"
           "                      << \" pvr_fb_r_ctrl=\"\n"
           "                      << pvr.framebuffer_read_control\n"
           "                      << \" pvr_fb_r_size=\"\n"
           "                      << pvr.framebuffer_read_size\n"
           "                      << \" pvr_fb_r_sof1=\"\n"
           "                      << pvr.framebuffer_read_sof1\n"
           "                      << \" post_entry_pvr_render_requests=\"\n"
           "                      << post_entry.pvr_render_requests\n"
           "                      << \" post_entry_pvr_render_completions=\"\n"
           "                      << post_entry.pvr_render_completions\n"
           "                      << \" post_entry_pvr_renderer_frames=\"\n"
           "                      << post_entry.pvr_renderer_frames\n"
           "                      << \" post_entry_pvr_proven_guest_frames=\"\n"
           "                      << post_entry.pvr_proven_guest_frames\n"
           "                      << \" post_entry_pvr_direct_scanout_frames=\"\n"
           "                      << post_entry.pvr_direct_scanout_frames\n"
           "                      << \" post_entry_pvr_direct_scanout_changed_pixels=\"\n"
           "                      << post_entry.pvr_direct_scanout_changed_pixels\n"
           "                      << \" post_entry_pvr_changed_pixels=\"\n"
           "                      << post_entry.pvr_changed_pixels\n"
           "                      << \" post_entry_pvr_ta_packets=\"\n"
           "                      << post_entry.pvr_ta_packets\n"
           "                      << \" post_entry_pvr_ta_vertices=\"\n"
           "                      << post_entry.pvr_ta_vertices\n"
           "                      << \" post_entry_pvr_ta_frames=\"\n"
           "                      << post_entry.pvr_ta_frames\n"
           "                      << \" post_entry_pvr_yuv_macroblocks=\"\n"
           "                      << post_entry.pvr_yuv_macroblocks;\n"
           "            std::cerr << \" irq_pending_count=\"\n"
           "                      << state.interrupt_controller->pending_count()\n"
           "                      << \" irq_source=\"\n"
           "                      << (active_irq ? active_irq->source : 0u)\n"
           "                      << \" irq_level=\"\n"
           "                      << (active_irq ? static_cast<unsigned>(active_irq->level) : 0u)\n"
           "                      << \" irq_event=\"\n"
           "                      << (active_irq ? active_irq->event_code : 0u);\n"
           "            std::cerr << \" dmac2_source=\" << state.dmac->source(2u)\n"
           "                      << \" dmac2_destination=\" << state.dmac->destination(2u)\n"
           "                      << \" dmac2_count=\" << state.dmac->count(2u)\n"
           "                      << \" dmac2_control=\" << state.dmac->control(2u)\n"
           "                      << \" dmac_operation=\" << state.dmac->operation()\n"
           "                      << \" channel2_start=\"\n"
           "                      << system_bus.channel2_start\n"
           "                      << \" channel2_length=\"\n"
           "                      << system_bus.channel2_length;\n"
           "            std::cerr << \" g1_dma_active=\" << g1_dma.active\n"
           "                      << \" g1_dma_address=\" << g1_dma.system_address\n"
           "                      << \" g1_dma_remaining=\" << g1_dma.remaining\n"
           "                      << \" pvr_dma_active=\" << pvr_dma.active\n"
           "                      << \" pvr_dma_system_address=\" << pvr_dma.system_address\n"
           "                      << \" pvr_dma_address=\" << pvr_dma.peripheral_address\n"
           "                      << \" pvr_dma_remaining=\" << pvr_dma.remaining;\n"
           "            for (std::size_t channel = 0u; channel < 4u; ++channel) {\n"
           "                const auto& g2 = state.holly_dma.g2->channel_state(channel);\n"
           "                std::cerr << \" g2_\" << channel << \"_active=\" << g2.active\n"
           "                          << \" g2_\" << channel << \"_remaining=\"\n"
           "                          << g2.remaining;\n"
           "            }\n"
           "            std::cerr << \" gdrom_ata_status=\"\n"
           "                      << static_cast<unsigned>(gdrom_status.ata_status)\n"
           "                      << \" gdrom_interrupt_reason=\"\n"
           "                      << static_cast<unsigned>(gdrom_status.interrupt_reason)\n"
           "                      << \" gdrom_pio_bytes=\" << gdrom_status.pio_bytes_available\n"
           "                      << \" gdrom_bios_requests=\" << gdrom_status.bios_requests\n"
           "                      << \" gdrom_commands=\" << gdrom_status.completed_commands\n"
           "                      << \" gdrom_dma=\" << gdrom_status.completed_dma;\n"
           "            std::cerr << \" materializer_requests=\" << materialization.requests\n"
           "                      << \" materializer_cache_hits=\" << materialization.cache_hits\n"
           "                      << \" materializations=\" << materialization.materializations\n"
           "                      << \" interpreter_materializations=\"\n"
           "                      << materialization.interpreter_materializations\n"
           "                      << \" materializer_misses=\" << materialization.misses\n"
           "                      << \" materializer_budget_failures=\"\n"
           "                      << materialization.budget_failures\n"
           "                      << \" retained_validation_bytes=\"\n"
           "                      << materialization.retained_validation_bytes\n"
           "                      << \" peak_retained_validation_bytes=\"\n"
           "                      << materialization.peak_retained_validation_bytes\n"
           "                      << \" reclaimed_validation_bytes=\"\n"
           "                      << materialization.reclaimed_validation_bytes\n"
           "                      << \" materializer_first_failure=\"\n"
           "                      << materialization.first_failure\n"
           "                      << \" materializer_first_target=\"\n"
           "                      << materialization.first_failure_target;\n"
           "            if (const auto mmio = cpu.memory.last_mmio_access(); mmio)\n"
           "                std::cerr << \" last_mmio_operation=\"\n"
           "                          << (mmio->operation == "
           "katana::runtime::MemoryAccessOperation::Read\n"
           "                                  ? \"read\" : \"write\")\n"
           "                          << \" last_mmio_address=\" << mmio->address\n"
           "                          << \" last_mmio_width=\"\n"
           "                          << static_cast<unsigned>(mmio->width)\n"
           "                          << \" last_mmio_value=\" << mmio->value\n"
           "                          << \" last_mmio_region=\" << mmio->region_name;\n"
           "            std::cerr << \" aica_pending=\" << state.aica->interrupts().pending()\n"
           "                      << \" aica_enabled=\" << state.aica->interrupts().enabled()\n"
           "                      << \" aica_active_channels=\"\n"
           "                      << state.aica_registers->active_channel_count()\n"
           "                      << \" post_entry_aica_rendered_buffers=\"\n"
           "                      << post_entry.aica_rendered_buffers\n"
           "                      << \" post_entry_aica_rendered_frames=\"\n"
           "                      << post_entry.aica_rendered_frames\n"
           "                      << \" post_entry_host_audio_submitted_buffers=\"\n"
           "                      << post_entry.host_audio_submitted_buffers\n"
           "                      << \" post_entry_host_audio_submitted_frames=\"\n"
           "                      << post_entry.host_audio_submitted_frames\n"
           "                      << \" aica_rtc=\" << state.aica_rtc->counter()\n"
           "                      << \" aica_rtc_write_enabled=\"\n"
           "                      << state.aica_rtc->write_enabled();\n"
           "            if (const auto& error = state.pvr_renderer->first_error(); error)\n"
           "                std::cerr << \" pvr_first_error=\"\n"
           "                          << katana::runtime::pvr_render_error_name(error->error)\n"
           "                          << \" pvr_error_request=\" << error->render_request\n"
           "                          << \" pvr_error_detail=\" << error->detail;\n"
           "            if (const auto* probes = std::getenv(\"KATANA_PORT_MEMORY_PROBES\");\n"
           "                probes != nullptr && *probes != '\\0') {\n"
           "                const char* cursor = probes;\n"
           "                for (std::size_t index = 0u; index < 16u && *cursor != '\\0'; ++index) "
           "{\n"
           "                    errno = 0; char* end = nullptr;\n"
           "                    const auto parsed = std::strtoull(cursor, &end, 0);\n"
           "                    if (errno != 0 || end == cursor || parsed > 0xFFFFFFFFull ||\n"
           "                        (*end != '\\0' && *end != ',')) {\n"
           "                        std::cerr << \" memory_probe_parse_error=1\"; break;\n"
           "                    }\n"
           "                    const auto address = static_cast<std::uint32_t>(parsed);\n"
           "                    std::cerr << \" memory_probe_address=\" << address;\n"
           "                    try {\n"
           "                        const std::array<const katana::runtime::MemoryDevice*, 3u>\n"
           "                            permitted{state.main_ram.get(), state.vram.get(),\n"
           "                                      state.aica_ram.get()};\n"
           "                        std::cerr << \" memory_probe_value=\"\n"
           "                                  << katana::runtime::peek_guest_u32(\n"
           "                                         cpu, address, permitted);\n"
           "                    }\n"
           "                    catch (...) { std::cerr << \" memory_probe_error=1\"; }\n"
           "                    if (*end == '\\0') break; cursor = end + 1;\n"
           "                }\n"
           "            }\n"
           "            std::cerr << '\\n';\n"
           "            services.report_flag_poll_batch_statistics(std::cerr);\n"
           "            services.report_mmio_wait_loop_batch_statistics(std::cerr);\n"
           "        };\n"
           "        " +
           entry_namespace +
           "::RuntimeRunResult result;\n"
           "        try {\n"
           "            result = " +
           entry_namespace +
           "::run_runtime(cpu, services, *state.runtime_blocks,\n"
           "                          services.observation_session(),\n"
           "                          services.crash_capsule());\n"
           "        } catch (const katana::runtime::PlatformLifecycleExit&) {\n"
           "            report_progress();\n"
           "            if (state.gdrom)\n"
           "                std::cerr << \"KATANA_GDROM_BIOS_EVENTS \"\n"
           "                          << state.gdrom->format_bios_call_events_json() << '\\n';\n"
           "            host.shutdown();\n"
           "            host.require_clean_shutdown();\n"
           "            throw;\n"
           "        } catch (...) {\n"
           "            report_progress();\n"
           "            throw;\n"
           "        }\n"
           "        const auto post_entry_host_seconds =\n"
           "            services.post_entry_host_seconds();\n"
           "        const auto post_entry_evidence =\n"
           "            capture_post_entry_product_evidence();\n"
           "        if (services.product_budget_arm_failed())\n"
           "            throw std::runtime_error(\n"
           "                \"product-post-entry-cycle-budget-invalid\");\n"
           "        if (game_entry_capture_path &&\n"
           "            (!game_entry_capture_attempted ||\n"
           "             !game_entry_capture_succeeded))\n"
           "            throw std::runtime_error(\n"
           "                \"game-entry-handoff-capture-incomplete\");\n"
           "        if (result.guest_cycle_budget_reached) report_progress();\n"
           "        if (host.state() == katana::runtime::HostRuntimeState::Shutdown) {\n"
           "            if (const auto* final_progress =\n"
           "                    std::getenv(\"KATANA_PORT_FINAL_PROGRESS\");\n"
           "                final_progress != nullptr && *final_progress != '\\0')\n"
           "                report_progress();\n"
           "            host.require_clean_shutdown();\n"
           "            if (state.scheduler->pending_event_count() != 0u)\n"
           "                throw std::runtime_error(\"Host-Shutdown hinterliess "
           "Schedulerereignisse.\");\n"
           "            std::cout << \"KR_HOST_SHUTDOWN guest_dispatch_stopped=1 host_events=\"\n"
           "                      << host.processed_events() << \" input_events=\"\n"
           "                      << lifecycle_input->injected_events()\n"
           "                      << \" controller_changes=\"\n"
           "                      << controller_changes\n"
           "                      << \" controller_contract=\"\n"
           "                      << controller_contract << '\\n';\n"
           "        }\n"
           "        if (services.guest_program_dispatched()) {\n"
           "            std::cout << \"KR_BOOT_EXECUTABLE_ENTRY guest_cycle=\"\n"
           "                      << services.guest_program_entry_cycle() << '\\n';\n"
           "            std::cout << \"KR_GUEST_PROGRAM_DISPATCHED\\n\";\n"
           "        }\n"
           "        if (services.guest_program_progressed())\n"
           "            std::cout << \"KR_GAME_CODE_PROGRESSED guest_cycle=\"\n"
           "                      << services.guest_program_progress_cycle() << '\\n';\n"
           "        if (result.static_aot_classified_dispatches != "
           "result.central_dispatches)\n"
           "            throw std::runtime_error(\n"
           "                \"static-aot-escape-classification-mismatch\");\n"
           "        const auto static_aot_escape_count = [&](const auto reason) {\n"
           "            return result.static_aot_escape_counts[\n"
           "                static_cast<std::size_t>(reason)];\n"
           "        };\n"
           "        using StaticEscape = " +
           entry_namespace +
           "::StaticAotEscapeReason;\n"
           "        std::cout << \"KATANA_STATIC_AOT_ESCAPE_STATS whole_run_classified=\"\n"
           "                  << result.static_aot_classified_dispatches\n"
           "                  << \" whole_run_central_dispatches=\"\n"
           "                  << result.central_dispatches\n"
           "                  << \" program_entry=\"\n"
           "                  << static_aot_escape_count(StaticEscape::ProgramEntry)\n"
           "                  << \" call_unknown=\"\n"
           "                  << static_aot_escape_count(StaticEscape::CallUnknown)\n"
           "                  << \" target_not_native_entry_safe=\"\n"
           "                  << static_aot_escape_count(\n"
           "                         StaticEscape::TargetNotNativeEntrySafe)\n"
           "                  << \" timing_not_deferrable=\"\n"
           "                  << static_aot_escape_count(\n"
           "                         StaticEscape::TimingNotDeferrable)\n"
           "                  << \" scheduler_due=\"\n"
           "                  << static_aot_escape_count(StaticEscape::SchedulerDue)\n"
           "                  << \" interrupt_acceptable=\"\n"
           "                  << static_aot_escape_count(\n"
           "                         StaticEscape::InterruptAcceptable)\n"
           "                  << \" mmio_or_architecture=\"\n"
           "                  << static_aot_escape_count(\n"
           "                         StaticEscape::MmioOrArchitectureBoundary)\n"
           "                  << \" hook=\"\n"
           "                  << static_aot_escape_count(StaticEscape::Hook)\n"
           "                  << \" variant_or_generation=\"\n"
           "                  << static_aot_escape_count(\n"
           "                         StaticEscape::VariantOrGeneration)\n"
           "                  << \" native_call_depth=\"\n"
           "                  << static_aot_escape_count(StaticEscape::NativeCallDepth)\n"
           "                  << \" partition_or_symbol=\"\n"
           "                  << static_aot_escape_count(\n"
           "                         StaticEscape::PartitionOrSymbol)\n"
           "                  << \" guest_cycle_quantum=\"\n"
           "                  << static_aot_escape_count(\n"
           "                         StaticEscape::GuestCycleQuantum)\n"
           "                  << \" guest_cycle_budget=\"\n"
           "                  << static_aot_escape_count(\n"
           "                         StaticEscape::GuestCycleBudget)\n"
           "                  << \" dynamic_target=\"\n"
           "                  << static_aot_escape_count(StaticEscape::DynamicTarget)\n"
           "                  << \" return_boundary=\"\n"
           "                  << static_aot_escape_count(StaticEscape::ReturnBoundary)\n"
           "                  << \" sleep=\"\n"
           "                  << static_aot_escape_count(StaticEscape::Sleep)\n"
           "                  << \" product_fastpath=\"\n"
           "                  << static_aot_escape_count(StaticEscape::ProductFastpath)\n"
           "                  << \" unclassified=\"\n"
           "                  << static_aot_escape_count(StaticEscape::Unclassified);\n"
           "        for (std::size_t index = 0u;\n"
           "             index < result.static_aot_escape_top_site_count; ++index) {\n"
           "            const auto& site = result.static_aot_escape_top_sites[index];\n"
           "            std::cout << \" top\" << index << \"_site_id=\" << site.site_id\n"
           "                      << \" top\" << index << \"_guest_address=\"\n"
           "                      << site.guest_address\n"
           "                      << \" top\" << index << \"_reason=\"\n"
           "                      << static_cast<unsigned>(site.reason)\n"
           "                      << \" top\" << index << \"_count=\" << site.count;\n"
           "        }\n"
           "        std::cout << '\\n';\n"
           "        const std::uint64_t silent_failures =\n"
           "            (state.loaded_boot_bytes == 0u ? 1u : 0u) +\n"
           "            (result.scheduler_cycle == 0u ? 1u : 0u) +\n"
           "            (cpu.trap_pending ? 1u : 0u) +\n"
           "            (cpu.last_exception_cause != katana::runtime::ExceptionCause::None ? 1u : "
           "0u) +\n"
           "            services.fallback_count();\n"
           "        int product_exit_code = 1;\n"
           "        {\n"
           "            const auto& gate_materialization = " +
           entry_namespace +
           "::runtime_materialization_status();\n"
           "            const bool required_milestone_reached = [&] {\n"
           "                switch (required_product_milestone) {\n"
           "                case katana::runtime::RequiredProductMilestone::\n"
           "                        BootExecutableEntry:\n"
           "                    return services.guest_program_dispatched();\n"
           "                case katana::runtime::RequiredProductMilestone::\n"
           "                        GameCodeProgressed:\n"
           "                    return services.guest_program_progressed();\n"
           "                case katana::runtime::RequiredProductMilestone::\n"
           "                        FirstGameFramebufferWrite:\n"
           "                    return first_game_framebuffer_write;\n"
           "                case katana::runtime::RequiredProductMilestone::FirstTaFrame:\n"
           "                    return first_game_ta_frame;\n"
           "                case katana::runtime::RequiredProductMilestone::\n"
           "                        FirstVisibleGameFrame:\n"
           "                    return first_visible_game_frame;\n"
           "                }\n"
           "                return false;\n"
           "            }();\n"
           "            const bool terminal_problem =\n"
           "                gate_materialization.first_failure != 0u ||\n"
           "                result.runtime_dispatch_first_error != 0u ||\n"
           "                services.crash_capsule().first_error_latched != 0u ||\n"
           "                state.pvr_renderer->first_error().has_value() ||\n"
           "                services.product_entry_evidence_failed() ||\n"
           "                silent_failures != 0u;\n"
           "            const auto* first_problem = gate_materialization.first_failure != 0u\n"
           "                ? \"aot-materialization\"\n"
           "                : result.runtime_dispatch_first_error != 0u\n"
           "                    ? \"runtime-dispatch\"\n"
           "                    : services.crash_capsule().first_error_latched != 0u\n"
           "                        ? \"runtime\"\n"
           "                    : state.pvr_renderer->first_error().has_value()\n"
           "                        ? \"pvr\"\n"
           "                    : services.product_entry_evidence_failed()\n"
           "                        ? \"product-entry-evidence\"\n"
           "                    : silent_failures != 0u\n"
           "                        ? \"runtime-contract\"\n"
           "                    : !required_milestone_reached\n"
           "                        ? \"required-milestone-not-reached\" : \"none\";\n"
           "            const auto* highest_milestone = first_visible_game_frame\n"
           "                ? \"FirstVisibleGameFrame\"\n"
           "                : first_game_ta_frame ? \"FirstTaFrame\"\n"
           "                : first_game_framebuffer_write ? \"FirstGameFramebufferWrite\"\n"
           "                : services.guest_program_progressed() ? \"GameCodeProgressed\"\n"
           "                : services.guest_program_dispatched() ? \"BootExecutableEntry\"\n"
           "                : first_ip_bin_visible_frame ? \"IpBinVisibleFrame\" : \"None\";\n"
           "            const auto* visible_screen = first_visible_game_frame\n"
           "                ? \"game-frame\"\n"
           "                : first_ip_bin_visible_frame ? \"ip-bin-frame\" : \"none\";\n"
           "            const auto* boot_path_name = runtime_boot_config.boot_path ==\n"
           "                    katana::runtime::DreamcastRuntimeBootPath::DirectBootExecutable\n"
           "                ? \"DirectBootExecutable\" : \"NativeDiscBoot\";\n"
           "            const std::uint32_t milestone_bits =\n"
           "                (services.guest_program_dispatched() ? 1u << 0u : 0u) |\n"
           "                (services.guest_program_progressed() ? 1u << 1u : 0u) |\n"
           "                (first_game_framebuffer_write ? 1u << 2u : 0u) |\n"
           "                (first_game_ta_frame ? 1u << 3u : 0u) |\n"
           "                (first_visible_game_frame ? 1u << 4u : 0u) |\n"
           "                (first_ip_bin_visible_frame ? 1u << 6u : 0u);\n"
           "            const auto requested_post_entry_cycles =\n"
           "                services.requested_post_entry_cycles().value_or(0u);\n"
           "            const bool product_budget_requested =\n"
           "                services.requested_post_entry_cycles().has_value();\n"
           "            const auto target_guest_cycle =\n"
           "                services.product_target_guest_cycle().value_or(0u);\n"
           "            const auto executed_post_entry_cycles =\n"
           "                services.guest_program_dispatched() &&\n"
           "                        result.scheduler_cycle >=\n"
           "                            services.guest_program_entry_cycle()\n"
           "                    ? result.scheduler_cycle -\n"
           "                          services.guest_program_entry_cycle()\n"
           "                    : 0u;\n"
           "            const auto post_entry_guest_mhz =\n"
           "                post_entry_host_seconds > 0.0\n"
           "                ? static_cast<double>(executed_post_entry_cycles) /\n"
           "                    post_entry_host_seconds / 1'000'000.0\n"
           "                : 0.0;\n"
           "            const bool comparable_product_gate =\n"
           "                requested_post_entry_cycles ==\n"
           "                    product_gate_guest_cycle_checkpoint &&\n"
           "                executed_post_entry_cycles ==\n"
           "                    product_gate_guest_cycle_checkpoint;\n"
           "            const bool requested_budget_complete =\n"
           "                product_budget_requested &&\n"
           "                requested_post_entry_cycles ==\n"
           "                    executed_post_entry_cycles;\n"
           "            const bool successful_product_run =\n"
           "                required_milestone_reached &&\n"
           "                (!product_budget_requested ||\n"
           "                 (requested_budget_complete &&\n"
           "                  comparable_product_gate));\n"
           "            const auto* product_status = terminal_problem\n"
           "                ? \"error\"\n"
           "                : successful_product_run\n"
           "                    ? \"required-milestone-reached\"\n"
           "                : product_budget_requested &&\n"
           "                        required_milestone_reached &&\n"
           "                        !requested_budget_complete\n"
           "                    ? \"early-exit-before-requested-budget\"\n"
           "                : product_budget_requested &&\n"
           "                        required_milestone_reached &&\n"
           "                        requested_budget_complete &&\n"
           "                        !comparable_product_gate\n"
           "                    ? \"non-comparable-product-budget\"\n"
           "                : result.guest_cycle_budget_reached\n"
           "                    ? \"guest-cycle-budget-reached-milestone-missed\"\n"
           "                    : \"early-exit-before-required-milestone\";\n"
           "            terminal_telemetry.terminal_summary_emitted = true;\n"
           "            std::cout << (comparable_product_gate\n"
           "                              ? \"KATANA_PRODUCT_GATE\"\n"
           "                              : \"KATANA_BRINGUP_RUN\")\n"
           "                      << \" status=\" << product_status\n"
           "                      << \" restored_guest_cycle=\"\n"
           "                      << services.restored_guest_cycle()\n"
           "                      << \" start_guest_cycle=\"\n"
           "                      << services.guest_program_entry_cycle()\n"
           "                      << \" target_guest_cycle=\" << target_guest_cycle\n"
           "                      << \" final_guest_cycle=\" << result.scheduler_cycle\n"
           "                      << \" requested_post_entry_cycles=\"\n"
           "                      << requested_post_entry_cycles\n"
           "                      << \" executed_post_entry_cycles=\"\n"
           "                      << executed_post_entry_cycles\n"
           "                      << \" host_seconds=\" << post_entry_host_seconds\n"
           "                      << \" post_entry_guest_mhz=\"\n"
           "                      << post_entry_guest_mhz\n"
           "                      << \" post_entry_central_dispatches=\"\n"
           "                      << post_entry_evidence.central_dispatches\n"
           "                      << \" post_entry_executed_blocks=\"\n"
           "                      << post_entry_evidence.executed_blocks\n"
           "                      << \" post_entry_pvr_render_requests=\"\n"
           "                      << post_entry_evidence.pvr_render_requests\n"
           "                      << \" post_entry_pvr_render_completions=\"\n"
           "                      << post_entry_evidence.pvr_render_completions\n"
           "                      << \" post_entry_pvr_renderer_frames=\"\n"
           "                      << post_entry_evidence.pvr_renderer_frames\n"
           "                      << \" post_entry_pvr_proven_guest_frames=\"\n"
           "                      << post_entry_evidence.pvr_proven_guest_frames\n"
           "                      << \" post_entry_pvr_direct_scanout_frames=\"\n"
           "                      << post_entry_evidence.pvr_direct_scanout_frames\n"
           "                      << \" post_entry_pvr_direct_scanout_changed_pixels=\"\n"
           "                      << post_entry_evidence.\n"
           "                             pvr_direct_scanout_changed_pixels\n"
           "                      << \" post_entry_pvr_changed_pixels=\"\n"
           "                      << post_entry_evidence.pvr_changed_pixels\n"
           "                      << \" post_entry_pvr_ta_packets=\"\n"
           "                      << post_entry_evidence.pvr_ta_packets\n"
           "                      << \" post_entry_pvr_ta_vertices=\"\n"
           "                      << post_entry_evidence.pvr_ta_vertices\n"
           "                      << \" post_entry_pvr_ta_frames=\"\n"
           "                      << post_entry_evidence.pvr_ta_frames\n"
           "                      << \" post_entry_pvr_yuv_macroblocks=\"\n"
           "                      << post_entry_evidence.pvr_yuv_macroblocks\n"
           "                      << \" post_entry_aica_rendered_buffers=\"\n"
           "                      << post_entry_evidence.aica_rendered_buffers\n"
           "                      << \" post_entry_aica_rendered_frames=\"\n"
           "                      << post_entry_evidence.aica_rendered_frames\n"
           "                      << \" post_entry_host_presented_frames=\"\n"
           "                      << post_entry_evidence.host_presented_frames\n"
           "                      << \" post_entry_host_audio_submitted_buffers=\"\n"
           "                      << post_entry_evidence.host_audio_submitted_buffers\n"
           "                      << \" post_entry_host_audio_submitted_frames=\"\n"
           "                      << post_entry_evidence.host_audio_submitted_frames\n"
           "                      << \" post_entry_evidence_ready=\"\n"
           "                      << static_cast<unsigned>(\n"
           "                             product_entry_evidence_ready)\n"
           "                      << \" post_entry_evidence_failed=\"\n"
           "                      << static_cast<unsigned>(\n"
           "                             services.product_entry_evidence_failed())\n"
           "                      << \" boot_path=\" << boot_path_name\n"
           "                      << \" milestone_bits=\" << milestone_bits\n"
           "                      << \" required_milestone=\"\n"
           "                      << katana::runtime::required_product_milestone_name(\n"
           "                             required_product_milestone)\n"
           "                      << \" required_milestone_reached=\"\n"
           "                      << static_cast<unsigned>(required_milestone_reached)\n"
           "                      << \" highest_milestone=\" << highest_milestone\n"
           "                      << \" visible_screen=\" << visible_screen\n"
           "                      << \" boot_executable_entry_cycle=\"\n"
           "                      << services.guest_program_entry_cycle()\n"
           "                      << \" game_code_progress_cycle=\"\n"
           "                      << services.guest_program_progress_cycle()\n"
           "                      << \" first_game_framebuffer_write_cycle=\"\n"
           "                      << first_game_framebuffer_write_cycle\n"
           "                      << \" first_ta_frame_cycle=\"\n"
           "                      << first_game_ta_frame_cycle\n"
           "                      << \" first_visible_game_frame_cycle=\"\n"
           "                      << first_visible_game_frame_cycle\n"
           "                      << \" first_ip_bin_visible_frame_cycle=\"\n"
           "                      << first_ip_bin_visible_frame_cycle\n"
           "                      << \" first_problem=\" << first_problem << '\\n';\n"
           "            product_exit_code = terminal_problem\n"
           "                ? 1 : successful_product_run\n"
           "                    ? 0 : result.guest_cycle_budget_reached ? 3 : 1;\n"
           "        }\n"
           "        if (silent_failures != 0u) {\n"
           "            throw std::runtime_error(\"Runtime-Einstieg besitzt keinen "
           "Dispatchnachweis.\");\n"
           "        }\n"
           "        const auto audio_buffers = audio->submitted_buffers();\n"
           "        const auto audio_hash = audio->deterministic_hash();\n"
           "        const auto input_events = lifecycle_input->injected_events();\n"
           "        const auto controller_samples = controller_input->sampled_frames();\n"
           "#if defined(KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST) || \\\n"
           "    defined(KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST) || \\\n"
           "    defined(KATANA_INTERNAL_MEMORY_FILL_DIFFERENTIAL_TEST)\n"
           "        const auto* counted_loop_differential =\n"
           "            std::getenv(\"KATANA_PORT_COUNTED_LOOP_DIFFERENTIAL_TEST\");\n"
           "        const auto* composite_callback_differential =\n"
           "            std::getenv(\"KATANA_PORT_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST\");\n"
           "        const auto* memory_fill_differential =\n"
           "            std::getenv(\"KATANA_PORT_MEMORY_FILL_DIFFERENTIAL_TEST\");\n"
           "        const bool report_counted_loop_state =\n"
           "            counted_loop_differential != nullptr &&\n"
           "            std::string_view(counted_loop_differential) == \"1\";\n"
           "        const bool report_composite_callback_state =\n"
           "            composite_callback_differential != nullptr &&\n"
           "            std::string_view(composite_callback_differential) == \"1\";\n"
           "        const bool report_memory_fill_state =\n"
           "            memory_fill_differential != nullptr &&\n"
           "            std::string_view(memory_fill_differential) == \"1\";\n"
           "        if (report_counted_loop_state || report_composite_callback_state ||\n"
           "            report_memory_fill_state) {\n"
           "            const auto cpu_probe =\n"
           "                katana::runtime::capture_runtime_probe_cpu(cpu);\n"
           "            const auto scheduler_probe =\n"
           "                katana::runtime::capture_runtime_probe_scheduler(*state.scheduler);\n"
           "            const std::array memory_probe{\n"
           "                katana::runtime::RuntimeProbeMemoryRange{\n"
           "                    katana::runtime::RuntimeProbeMemoryRegion::MainRam, 0u,\n"
           "                    state.main_ram->bytes()},\n"
           "                katana::runtime::RuntimeProbeMemoryRange{\n"
           "                    katana::runtime::RuntimeProbeMemoryRegion::VideoRam, 0u,\n"
           "                    state.vram->bytes()},\n"
           "                katana::runtime::RuntimeProbeMemoryRange{\n"
           "                    katana::runtime::RuntimeProbeMemoryRegion::AicaRam, 0u,\n"
           "                    state.aica_ram->bytes()}};\n"
           "            const auto dreamcast_probe =\n"
           "                katana::runtime::capture_runtime_probe_dreamcast(\n"
           "                    state, audio_buffers, audio->submitted_frames(), audio_hash);\n"
           "            const auto find_device = [&](const auto kind) {\n"
           "                const auto found = std::find_if(\n"
           "                    dreamcast_probe.devices.begin(), dreamcast_probe.devices.end(),\n"
           "                    [kind](const auto& device) { return device.kind == kind; });\n"
           "                if (found == dreamcast_probe.devices.end())\n"
           "                    throw std::runtime_error(\n"
           "                        \"counted-loop-differential-device-missing\");\n"
           "                return *found;\n"
           "            };\n"
           "            const std::array address_space_probe{find_device(\n"
           "                katana::runtime::RuntimeProbeDeviceKind::AddressSpace)};\n"
           "            const std::array code_tracker_probe{find_device(\n"
           "                katana::runtime::RuntimeProbeDeviceKind::CodeTracker)};\n"
           "            const std::array module_probe{find_device(\n"
           "                katana::runtime::RuntimeProbeDeviceKind::ModuleCatalog)};\n"
           "            const auto& memory_counters = cpu.memory.performance_counters();\n"
           "            if (report_memory_fill_state)\n"
           "                std::cout << \"KATANA_MEMORY_FILL_STATE cpu=\";\n"
           "            else if (report_composite_callback_state)\n"
           "                std::cout << \"KATANA_COMPOSITE_CALLBACK_STATE cpu=\";\n"
           "            else\n"
           "                std::cout << \"KATANA_COUNTED_LOOP_STATE cpu=\";\n"
           "            std::cout\n"
           "                      << katana::runtime::hash_runtime_probe_cpu(cpu_probe)\n"
           "                      << \" scheduler=\"\n"
           "                      << katana::runtime::hash_runtime_probe_scheduler(\n"
           "                             scheduler_probe)\n"
           "                      << \" memory=\"\n"
           "                      << katana::runtime::hash_runtime_probe_memory(memory_probe)\n"
           "                      << \" address_space=\"\n"
           "                      << katana::runtime::hash_runtime_probe_devices(\n"
           "                             address_space_probe);\n"
           "            std::cout\n"
           "                      << \" code_provenance=\"\n"
           "                      << katana::runtime::hash_runtime_probe_devices(\n"
           "                             code_tracker_probe)\n"
           "                      << \" module_provenance=\"\n"
           "                      << katana::runtime::hash_runtime_probe_devices(module_probe)\n"
           "                      << \" attempted=\" << cpu.attempted_guest_instructions\n"
           "                      << \" retired=\" << cpu.retired_guest_instructions\n"
           "                      << \" guest_cycles=\" << cpu.total_guest_cycles\n"
           "                      << \" scheduler_cycle=\" << state.scheduler->current_cycle()\n"
           "                      << \" t=\" << static_cast<unsigned>(cpu.t)\n"
           "                      << \" mmucr=\" << cpu.mmucr\n"
           "                      << \" mode=\"\n"
           "                      << static_cast<unsigned>(state.address_space->mode())\n"
           "                      << \" indexed_hits=\"\n"
           "                      << memory_counters.indexed_region_hits\n"
           "                      << \" reference_probes=\"\n"
           "                      << memory_counters.reference_region_probes\n"
           "                      << \" unobserved=\" << memory_counters.unobserved_accesses\n"
           "                      << \" observed=\" << memory_counters.observed_accesses\n"
           "                      << \" write_observer=\"\n"
           "                      << static_cast<unsigned>(\n"
           "                             cpu.memory.has_guest_write_observer())\n"
           "                      << \" stable_write_observer=\"\n"
           "                      << static_cast<unsigned>(\n"
           "                             cpu.memory."
           "guest_write_observer_allows_prevalidated_linear_writes())\n"
           "                      << \" pc=\" << cpu.pc << \" pr=\" << cpu.pr\n"
           "                      << \" sr=\" << cpu.read_sr()\n"
           "                      << \" pending_cycles=\" << cpu.pending_guest_cycles\n"
           "                      << \" active_instruction=\" << cpu.active_instruction_pc\n"
           "                      << \" active_instruction_physical=\"\n"
           "                      << cpu.active_instruction_physical_pc\n"
           "                      << \" active_block=\" << cpu.active_block_virtual_start\n"
           "                      << \" active_block_physical=\"\n"
           "                      << cpu.active_block_physical_start\n"
           "                      << \" active_block_size=\" << cpu.active_block_size\n"
           "                      << \" exception_generation=\" << cpu.exception_generation\n"
           "                      << \" last_exception_generation=\"\n"
           "                      << cpu.last_exception_generation\n"
           "                      << \" prefetch_count=\" << cpu.prefetch_count\n"
           "                      << \" last_prefetch=\" << cpu.last_prefetch_address\n"
           "                      << \" sleeping=\" << static_cast<unsigned>(cpu.sleeping)\n"
           "                      << \" store_queue_prefetch=\"\n"
           "                      << static_cast<unsigned>(cpu.last_prefetch_was_store_queue)\n"
           "                      << \" indirect_dispatches=\" << result.indirect_dispatches\n"
           "                      << \" runtime_dispatch_hits=\"\n"
           "                      << result.runtime_dispatch_hits\n"
           "                      << \" runtime_dispatch_misses=\"\n"
           "                      << result.runtime_dispatch_misses\n"
           "                      << \" runtime_dispatch_fallbacks=\"\n"
           "                      << result.runtime_dispatch_fallbacks\n"
           "                      << \" runtime_only_dispatch_hits=\"\n"
           "                      << result.runtime_only_dispatch_hits\n"
           "                      << \" runtime_only_dispatch_misses=\"\n"
           "                      << result.runtime_only_dispatch_misses\n"
           "                      << \" runtime_only_dispatch_fallbacks=\"\n"
           "                      << result.runtime_only_dispatch_fallbacks;\n"
           "            for (std::size_t index = 0u; index < cpu.r.size(); ++index)\n"
           "                std::cout << \" r\" << index << '=' << cpu.r[index];\n"
           "            std::cout << '\\n';\n"
           "        }\n"
           "#endif\n"
           "        host.shutdown();\n"
           "        host.require_clean_shutdown();\n"
           "        if (state.scheduler->pending_event_count() != 0u)\n"
           "            throw std::runtime_error(\"Host-Shutdown hinterliess "
           "Schedulerereignisse.\");\n"
           "        if (diagnostic_partial_port) {\n"
           "            std::cout << \"KR_DIAGNOSTIC_PARTIAL_RUNTIME_REACHED guest_cycles=\"\n"
           "                      << result.scheduler_cycle << \" executed_blocks=\"\n"
           "                      << services.executed_blocks() << '\\n';\n"
           "            return 3;\n"
           "        }\n"
           "        if (services.guest_program_progressed())\n"
           "            std::cout << \"KR_GUEST_PROGRAM_PROGRESSED\\n\";\n"
           "        if (services.guest_program_dispatched())\n"
           "            std::cout << \"KR_GUEST_PROGRAM_ENTERED\\n\";\n"
           "        std::cout << \"KATANA_RUNTIME_METRICS silent_failures=\"\n"
           "                  << silent_failures << \" guest_cycles=\"\n"
           "                  << result.scheduler_cycle << \" indirect_dispatches=\"\n"
           "                  << result.indirect_dispatches\n"
           "                  << \" runtime_dispatch_hits=\"\n"
           "                  << result.runtime_dispatch_hits\n"
           "                  << \" runtime_dispatch_misses=\"\n"
           "                  << result.runtime_dispatch_misses\n"
           "                  << \" runtime_dispatch_fallbacks=\"\n"
           "                  << result.runtime_dispatch_fallbacks\n"
           "                  << \" runtime_only_dispatch_hits=\"\n"
           "                  << result.runtime_only_dispatch_hits\n"
           "                  << \" runtime_only_dispatch_misses=\"\n"
           "                  << result.runtime_only_dispatch_misses\n"
           "                  << \" runtime_only_dispatch_fallbacks=\"\n"
           "                  << result.runtime_only_dispatch_fallbacks\n"
           "                  << \" runtime_only_sites=\" << result.runtime_only_sites\n"
           "                  << \" runtime_only_dispatch_share_ppm=\"\n"
           "                  << result.runtime_only_dispatch_share_ppm\n"
           "                  << \" runtime_dispatch_first_error=\"\n"
           "                  << result.runtime_dispatch_first_error\n"
           "                  << \" post_entry_host_presented_frames=\"\n"
           "                  << post_entry_evidence.host_presented_frames\n"
           "                  << \" post_entry_host_audio_submitted_buffers=\"\n"
           "                  << post_entry_evidence.host_audio_submitted_buffers\n"
           "                  << \" post_entry_host_audio_submitted_frames=\"\n"
           "                  << post_entry_evidence.host_audio_submitted_frames\n"
           "                  << \" audio_hash=\" << audio_hash\n"
           "                  << \" input_events=\" << input_events\n"
           "                  << \" controller_changes=\" << controller_changes\n"
           "                  << \" controller_samples=\" << controller_samples\n"
           "                  << \" controller_contract=\" << controller_contract\n"
           "                  << \" post_entry_central_dispatches=\"\n"
           "                  << post_entry_evidence.central_dispatches\n"
           "                  << \" post_entry_executed_blocks=\"\n"
           "                  << post_entry_evidence.executed_blocks\n"
           "                  << \" guest_cycle_contract=\" << result.guest_cycle_contract\n"
           "                  << \" guest_checkpoint=1 fallback_count=\"\n"
           "                  << services.fallback_count() << '\\n';\n"
           "        std::cout << \"KR_GENERATED_RUNTIME_STARTED boot_bytes=\"\n"
           "                  << state.loaded_boot_bytes << \" indirect_dispatches=\"\n"
           "                  << result.indirect_dispatches << \" final_pc=\" << result.final_pc "
           "<< '\\n';\n"
           "        return product_exit_code;\n"
           "    } catch (const katana::runtime::PlatformLifecycleExit& exit) {\n"
           "        const auto& evidence = exit.evidence();\n"
           "        const auto* reason = exit.reason() ==\n"
           "                katana::runtime::PlatformLifecycleExitReason::Reset ? \"reset\"\n"
           "            : exit.reason() ==\n"
           "                katana::runtime::PlatformLifecycleExitReason::BiosMenu\n"
           "                ? \"bios-menu\" : \"cd-menu\";\n"
           "        std::cerr << \"KATANA_PLATFORM_LIFECYCLE_EXIT {\\\"reason\\\":\\\"\"\n"
           "                  << reason << \"\\\",\\\"guest_cycle\\\":\"\n"
           "                  << evidence.guest_cycle << \" ,\\\"callsite\\\":\"\n"
           "                  << evidence.callsite << \" ,\\\"return_address\\\":\"\n"
           "                  << evidence.return_address << \" ,\\\"registers\\\":[\";\n"
           "        for (std::size_t index = 0u; index < evidence.registers.size(); ++index) {\n"
           "            if (index != 0u) std::cerr << ',';\n"
           "            std::cerr << evidence.registers[index];\n"
           "        }\n"
           "        std::cerr << \"],\\\"last_gdrom_request\\\":\"\n"
           "                  << evidence.last_gdrom_request\n"
           "                  << \" ,\\\"last_gdrom_command\\\":\"\n"
           "                  << evidence.last_gdrom_command\n"
           "                  << \" ,\\\"last_gdrom_state\\\":\"\n"
           "                  << evidence.last_gdrom_state\n"
           "                  << \" ,\\\"last_gdrom_status\\\":[\";\n"
           "        for (std::size_t index = 0u; index < evidence.last_gdrom_status.size();\n"
           "             ++index) {\n"
           "            if (index != 0u) std::cerr << ',';\n"
           "            std::cerr << evidence.last_gdrom_status[index];\n"
           "        }\n"
           "        std::cerr << \"]}\\n\";\n"
           "        emit_terminal_failure(\"platform-lifecycle\", evidence.callsite,\n"
           "                              evidence.return_address);\n"
           "        return 1;\n"
           "    } catch (const katana::runtime::HostPacingException& error) {\n"
           "        std::cerr << \"KATANA_HOST_PACING_ERROR \" << error.serialize_json() << "
           "'\\n';\n"
           "        emit_terminal_failure(\"host-pacing\");\n"
           "        return 1;\n"
           "    } catch (const katana::runtime::StoreQueuePrefetchRejected& error) {\n"
           "        const auto& fault = error.fault();\n"
           "        std::cerr << \"KATANA_STORE_QUEUE_PREFETCH_REJECTED reason=\"\n"
           "                  << static_cast<unsigned>(fault.reason)\n"
           "                  << \" source=\" << fault.source_address\n"
           "                  << \" target=\" << fault.target_address\n"
           "                  << \" source_pc=\" << fault.instruction.source_pc\n"
           "                  << \" runtime_pc=\" << fault.instruction.runtime_pc\n"
           "                  << \" packet_class=\" << fault.packet_class << '\\n';\n"
           "        emit_terminal_failure(\"store-queue-prefetch\",\n"
           "                              fault.instruction.source_pc,\n"
           "                              fault.target_address);\n"
           "        return 1;\n"
           "    } catch (const katana::runtime::PvrRenderFailed& error) {\n"
           "        const auto& failure = error.failure();\n"
           "        std::cerr << \"KATANA_PVR_RENDER_FAILED request=\" << failure.request\n"
           "                  << \" generation=\" << failure.generation\n"
           "                  << \" error=\"\n"
           "                  << katana::runtime::pvr_render_error_name(failure.error)\n"
           "                  << \" ta_packet_class=\" << failure.ta_packet_class\n"
           "                  << \" register_digest=\" << failure.register_digest\n"
           "                  << \" guest_cycle=\" << failure.guest_cycle << '\\n';\n"
           "        emit_terminal_failure(\"pvr-render\");\n"
           "        return 1;\n"
           "    } catch (const katana::runtime::IndirectDispatchError& error) {\n"
           "        std::cerr << \"KATANA_RUNTIME_DISPATCH_ERROR \"\n"
           "                  << error.metrics_json() << '\\n';\n"
           "        std::cerr << \"Portlauf fehlgeschlagen: \"\n"
           "                  << redact_source(error.what(), source) << '\\n';\n"
           "        emit_terminal_failure(\n"
           "            \"runtime-dispatch\", error.callsite(), error.target(),\n"
           "            static_cast<std::uint32_t>(error.error()));\n"
           "        return 1;\n"
           "    } catch (const std::exception& error) {\n"
           "        std::cerr << \"Portlauf fehlgeschlagen: \"\n"
           "                  << redact_source(error.what(), source) << '\\n';\n"
           "        emit_terminal_failure(\"runtime-exception\");\n"
           "        return 1;\n"
           "    } catch (...) {\n"
           "        std::cerr << \"Portlauf fehlgeschlagen: unknown-runtime-error\\n\";\n"
           "        emit_terminal_failure(\"unknown-runtime-error\");\n"
           "        return 1;\n"
           "    }\n"
           "}\n";
}

std::vector<ProjectArtifact> runtime_dispatch_artifacts(
    const std::string& entry_namespace,
    const std::span<const katana::ir::Function> program,
    const bool diagnostic_interpreter,
    const std::span<const katana::analysis::RuntimeCodeCopy> runtime_code_copies,
    const std::span<const katana::analysis::IndirectControlFlowResolution>
        indirect_control_flow,
    const std::span<const katana::analysis::FunctionCandidate> function_candidates,
    const std::span<const katana::analysis::GuardedAotEntry>
        guarded_aot_entries,
    const katana::io::ExecutableImage& image,
    const std::uint32_t boot_address,
    const std::size_t boot_size,
    const std::span<const PreparedLatentAotModule> latent_modules,
    const std::span<const MmioWaitLoopBatchProof> mmio_wait_loops,
    const katana::runtime::GameProjectDefinition* const external_game_project,
    const std::string_view expected_content_identity) {
    const auto external_native_templates =
        external_game_project != nullptr
            ? external_game_project->runtime_code_templates
            : std::span<const katana::runtime::NativeAotTemplate>{};
    const bool external_game_project_hooks =
        external_game_project != nullptr &&
        (!external_game_project->function_overrides.empty() ||
         !external_game_project->mid_function_hooks.empty());
    const bool external_game_project_handoff =
        external_game_project != nullptr &&
        external_game_project->game_entry_handoff.has_value();
    const bool external_game_project_runtime_bindings =
        external_game_project_hooks || external_game_project_handoff;
    const bool required_game_project_runtime_bindings =
        external_game_project != nullptr &&
        (external_game_project_handoff ||
         std::any_of(
             external_game_project->function_overrides.begin(),
             external_game_project->function_overrides.end(),
             [](const auto& function) {
                 return function.strength ==
                        katana::runtime::
                            GameProjectFunctionOverrideStrength::Required;
             }) ||
         std::any_of(
             external_game_project->mid_function_hooks.begin(),
             external_game_project->mid_function_hooks.end(),
             [](const auto& hook) {
                 return hook.strength ==
                        katana::runtime::GameProjectHookStrength::Required;
             }));
    const auto external_game_project_runtime_identity =
        external_game_project != nullptr
            ? game_project_runtime_identity(*external_game_project)
            : std::string{};
    const auto external_game_project_export_identity =
        external_game_project != nullptr
            ? game_project_export_identity(*external_game_project)
            : std::string{};
    std::unordered_set<std::uint32_t> external_hook_entries;
    if (external_game_project != nullptr) {
        for (const auto& function :
             external_game_project->function_overrides)
            external_hook_entries.insert(function.function_address);
        for (const auto& hook : external_game_project->mid_function_hooks)
            external_hook_entries.insert(hook.instruction_address);
    }
    const auto counted_loops = counted_loop_batch_proofs(program);
    const auto memory_fill_loops = memory_fill_loop_batch_proofs(program);
    const auto composite_callback_batches = composite_callback_batch_proofs(
        program, indirect_control_flow, function_candidates);
    std::vector<const CompositeCallbackBatchProof*> composite_callback_emission;
    composite_callback_emission.reserve(composite_callback_batches.size());
    for (const auto& proof : composite_callback_batches)
        composite_callback_emission.push_back(&proof);
    std::stable_sort(
        composite_callback_emission.begin(),
        composite_callback_emission.end(),
        [](const auto* left, const auto* right) {
            return left->descriptor.call_instruction_address <
                   right->descriptor.call_instruction_address;
        });
    enum class StaticFastpathBindingKind : std::uint8_t {
        CompositeCallback,
        MemoryFill,
        MmioWait,
        CountedLoop
    };
    struct StaticFastpathBindingEmission {
        StaticFastpathBindingKind kind;
        std::size_t descriptor_index = 0u;

        [[nodiscard]] bool
        operator==(const StaticFastpathBindingEmission&) const noexcept = default;
    };
    std::map<std::uint32_t, std::vector<StaticFastpathBindingEmission>>
        static_fastpath_bindings;
    const auto add_static_fastpath_binding =
        [&](const std::uint32_t address,
            const StaticFastpathBindingEmission binding) {
            auto& bindings = static_fastpath_bindings[address];
            if (std::find(bindings.begin(), bindings.end(), binding) ==
                bindings.end())
                bindings.push_back(binding);
        };
    if (!diagnostic_interpreter) {
        for (std::size_t index = 0u;
             index < composite_callback_emission.size();
             ++index)
            add_static_fastpath_binding(
                composite_callback_emission[index]
                    ->descriptor.kernel_address,
                {StaticFastpathBindingKind::CompositeCallback, index});
        for (std::size_t index = 0u; index < memory_fill_loops.size();
             ++index) {
            add_static_fastpath_binding(
                memory_fill_loops[index].descriptor.guard_address,
                {StaticFastpathBindingKind::MemoryFill, index});
            add_static_fastpath_binding(
                memory_fill_loops[index].descriptor.body_address,
                {StaticFastpathBindingKind::MemoryFill, index});
        }
        for (std::size_t index = 0u; index < mmio_wait_loops.size();
             ++index)
            add_static_fastpath_binding(
                mmio_wait_loops[index].descriptor.loop_header,
                {StaticFastpathBindingKind::MmioWait, index});
        for (std::size_t index = 0u; index < counted_loops.size();
             ++index)
            add_static_fastpath_binding(
                counted_loops[index].descriptor.guard_address,
                {StaticFastpathBindingKind::CountedLoop, index});
    }
    const auto symbol = [](const std::uint32_t address) {
        constexpr std::array digits{
            '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
        std::string result(8u, '0');
        for (std::size_t index = 0u; index < result.size(); ++index)
            result[result.size() - index - 1u] = digits[(address >> (index * 4u)) & 0xFu];
        return result;
    };
    const auto end_kind = [](const katana::ir::BasicBlock& block) {
        using O = katana::ir::Operation;
        const katana::ir::Instruction* terminal = nullptr;
        for (const auto& instruction : block.instructions) {
            if (instruction.delay_slot.role != katana::ir::DelaySlotRole::Slot)
                terminal = &instruction;
        }
        if (terminal == nullptr) return "Fallthrough";
        switch (terminal->operation) {
        case O::Branch:
            return "StaticBranch";
        case O::BranchIfTrue:
        case O::BranchIfFalse:
            return "ConditionalBranch";
        case O::JumpRegister:
            return "DynamicBranch";
        case O::Call:
        case O::CallRegister:
            return "Call";
        case O::Return:
            return "Return";
        case O::ReturnFromException:
            return "ExceptionReturn";
        case O::Sleep:
            return "Sleep";
        case O::TrapAlways:
            return "Exception";
        default:
            return "Fallthrough";
        }
    };
    struct NativeTemplateEmission {
        struct PatchTarget {
            std::uint32_t live_value = 0u;
            std::uint32_t block_address = 0u;

            auto operator<=>(const PatchTarget&) const = default;
        };
        struct MutableRange {
            std::uint32_t offset = 0u;
            std::uint32_t size = 0u;

            auto operator<=>(const MutableRange&) const = default;
        };

        std::string_view module_id;
        std::string expected_source_identity;
        std::uint32_t source_start = 0u;
        std::uint32_t extent = 0u;
        std::int32_t destination_vbr_delta = 0;
        std::map<std::uint32_t, std::vector<PatchTarget>> patch_targets;
        std::vector<MutableRange> mutable_ranges;
    };
    const auto range_contains = [](const std::uint32_t outer_start,
                                   const std::uint64_t outer_size,
                                   const std::uint32_t inner_start,
                                   const std::uint32_t inner_size) {
        const auto outer_end = static_cast<std::uint64_t>(outer_start) + outer_size;
        const auto inner_end = static_cast<std::uint64_t>(inner_start) + inner_size;
        return inner_size != 0u && outer_end <= 0x1'0000'0000ull && inner_end <= 0x1'0000'0000ull &&
               inner_start >= outer_start && inner_end <= outer_end;
    };
    struct ProgramBlockProof {
        std::uint32_t start = 0u;
        std::uint64_t end = 0u;
    };
    std::vector<ProgramBlockProof> program_block_proofs;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            auto end = static_cast<std::uint64_t>(block.start_address) + 2u;
            for (const auto& instruction : block.instructions)
                end = std::max(
                    end, static_cast<std::uint64_t>(instruction.source_address) + 2u);
            program_block_proofs.push_back({block.start_address, end});
        }
    }
    std::vector<NativeTemplateEmission> native_templates;
    for (const auto& copy : runtime_code_copies) {
        if (!copy.mutable_range_analysis_complete)
            throw std::runtime_error(
                "Runtime-Codecopy enthaelt einen selbstmodifizierenden Slot ohne "
                "vollstaendigen Write-before-read-Beweis.");
        if (copy.source_byte_count == 0u ||
            static_cast<std::uint64_t>(copy.source_begin) + copy.source_byte_count >
                0x1'0000'0000ull ||
            copy.source_end_inclusive !=
                copy.source_begin + copy.source_byte_count - sizeof(std::uint32_t))
            throw std::runtime_error("Runtime-Codecopy besitzt ungueltige Quellgrenzen.");
        std::string_view module_id;
        std::uint32_t module_start = 0u;
        std::size_t module_size = 0u;
        if (range_contains(katana::runtime::dreamcast_system_bootstrap_address,
                           katana::runtime::dreamcast_system_bootstrap_size,
                           copy.source_begin,
                           copy.source_byte_count)) {
            module_id = katana::runtime::dreamcast_initial_disc_bootstrap_module_id;
            module_start = katana::runtime::dreamcast_system_bootstrap_address;
            module_size = katana::runtime::dreamcast_system_bootstrap_size;
        } else if (range_contains(
                       boot_address, boot_size, copy.source_begin, copy.source_byte_count)) {
            module_id = katana::runtime::dreamcast_initial_boot_executable_module_id;
            module_start = boot_address;
            module_size = boot_size;
        } else {
            throw std::runtime_error(
                "Runtime-Codecopy liegt ausserhalb lokal gebundener Disc-Bootmodule.");
        }
        const auto* module_segment = image.find_segment(module_start, module_size);
        if (module_segment == nullptr) {
            throw std::runtime_error(
                "Runtime-Codecopy besitzt kein vollstaendiges lokales Quellsegment.");
        }
        const auto module_offset = module_segment->byte_offset(module_start);
        if (!module_offset.has_value() || *module_offset > module_segment->bytes.size() ||
            module_size > module_segment->bytes.size() - *module_offset) {
            throw std::runtime_error(
                "Runtime-Codecopy-Quellsegment besitzt keine vollstaendigen Modulbytes.");
        }
        const auto expected_source_identity =
            "sha256:" +
            katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(module_segment->bytes.data() + *module_offset),
                module_size));
        auto existing = std::find_if(
            native_templates.begin(), native_templates.end(), [&](const auto& candidate) {
                return candidate.module_id == module_id &&
                       candidate.expected_source_identity == expected_source_identity &&
                       candidate.source_start == copy.source_begin &&
                       candidate.extent == copy.source_byte_count &&
                       candidate.destination_vbr_delta == copy.destination_vbr_delta;
            });
        if (existing == native_templates.end()) {
            native_templates.push_back({module_id,
                                        expected_source_identity,
                                        copy.source_begin,
                                        copy.source_byte_count,
                                        copy.destination_vbr_delta,
                                        {},
                                        {}});
            existing = std::prev(native_templates.end());
        }
        for (const auto& patch : copy.patch_candidates) {
            if (patch.slot_address < copy.source_begin ||
                static_cast<std::uint64_t>(patch.slot_address) + sizeof(std::uint32_t) >
                    static_cast<std::uint64_t>(copy.source_begin) + copy.source_byte_count)
                throw std::runtime_error(
                    "Runtime-Codecopy-Patchslot liegt ausserhalb der Vorlage.");
            existing->patch_targets[patch.slot_address - copy.source_begin].push_back(
                {patch.live_value, patch.target_address});
        }
        for (const auto& range : copy.mutable_ranges) {
            if (!range_contains(copy.source_begin,
                                copy.source_byte_count,
                                range.slot_address,
                                range.size) ||
                !range_contains(copy.source_begin,
                                copy.source_byte_count,
                                range.store_instruction_address,
                                2u) ||
                !range_contains(copy.source_begin,
                                copy.source_byte_count,
                                range.load_instruction_address,
                                2u))
                throw std::runtime_error(
                    "Bewiesener Runtime-Codecopy-Mutable-Range liegt ausserhalb der Vorlage.");
            const auto source_block = std::find_if(
                program_block_proofs.begin(),
                program_block_proofs.end(),
                [&](const auto proof) { return proof.start == copy.source_begin; });
            const auto range_end =
                static_cast<std::uint64_t>(range.slot_address) + range.size;
            if (source_block == program_block_proofs.end() ||
                (source_block->start < range_end &&
                 range.slot_address < source_block->end))
                throw std::runtime_error(
                    "Runtime-Codecopy-Mutable-Range ueberlappt den nativen Entryblock.");
            if (std::any_of(program_block_proofs.begin(),
                            program_block_proofs.end(),
                            [&](const auto proof) {
                                return proof.start > copy.source_begin &&
                                       proof.start <= range.load_instruction_address;
                            }))
                throw std::runtime_error(
                    "Runtime-Codecopy-Scratchslot besitzt einen alternativen nativen "
                    "Blockentry vor dem bewiesenen Overwrite/Restore.");
            existing->mutable_ranges.push_back(
                {range.slot_address - copy.source_begin, range.size});
        }
    }
    for (auto& native_template : native_templates) {
        for (auto& [offset, targets] : native_template.patch_targets) {
            static_cast<void>(offset);
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        }
        std::sort(native_template.mutable_ranges.begin(),
                  native_template.mutable_ranges.end());
        native_template.mutable_ranges.erase(
            std::unique(native_template.mutable_ranges.begin(),
                        native_template.mutable_ranges.end()),
            native_template.mutable_ranges.end());
        std::uint64_t previous_end = 0u;
        for (const auto range : native_template.mutable_ranges) {
            const auto end = static_cast<std::uint64_t>(range.offset) + range.size;
            if (range.size == 0u || range.offset < previous_end ||
                end > native_template.extent)
                throw std::runtime_error(
                    "Runtime-Codecopy-Mutable-Ranges sind leer, ueberlappend oder ausserhalb.");
            previous_end = end;
            for (const auto& [patch_offset, targets] :
                 native_template.patch_targets) {
                static_cast<void>(targets);
                const auto patch_end =
                    static_cast<std::uint64_t>(patch_offset) + sizeof(std::uint32_t);
                if (range.offset < patch_end && patch_offset < end)
                    throw std::runtime_error(
                        "Runtime-Codecopy-Mutable-Range ueberlappt einen Patchslot.");
            }
        }
    }
    struct DispatchBlock {
        std::uint32_t owner;
        std::uint32_t address;
        std::uint32_t size;
        const char* end_kind;
        const char* timing_class;
        std::uint64_t maximum_guest_cycles;
    };
    std::size_t block_count = 0u;
    for (const auto& function : program)
        block_count += function.blocks.size();
    std::unordered_map<std::uint32_t, const katana::ir::BasicBlock*> emitted_blocks;
    emitted_blocks.reserve(block_count);
    std::vector<DispatchBlock> dispatch_blocks;
    dispatch_blocks.reserve(block_count);
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            const auto [existing, inserted] =
                emitted_blocks.emplace(block.start_address, &block);
            if (!inserted) {
                if (!equivalent_ir_block(*existing->second, block))
                    throw std::runtime_error(
                        "IR-Basic-Block besitzt abweichende Funktionsbesitzer.");
                continue;
            }
            std::uint32_t end = block.start_address + 2u;
            auto timing_class = katana::runtime::ExecutableBlockTimingClass::PureCpu;
            std::uint64_t maximum_guest_cycles = 0u;
            std::unordered_set<std::uint32_t> timed_instructions;
            for (const auto& instruction : block.instructions) {
                end = std::max(end, instruction.source_address + 2u);
                if (!timed_instructions.insert(instruction.source_address).second) continue;
                const auto timing = katana::sh4::instruction_timing(instruction.original_opcode);
                maximum_guest_cycles += timing.guest_cycles;
                if (timing.timing_class == katana::sh4::InstructionTimingClass::DeviceBusBoundary) {
                    timing_class = katana::runtime::ExecutableBlockTimingClass::NeverChain;
                } else if (timing.requires_cycle_flush &&
                           timing_class !=
                               katana::runtime::ExecutableBlockTimingClass::NeverChain) {
                    if (has_proven_linear_ram_access(instruction)) {
                        if (timing_class == katana::runtime::ExecutableBlockTimingClass::PureCpu)
                            timing_class =
                                katana::runtime::ExecutableBlockTimingClass::LinearRamOnly;
                    } else {
                        timing_class =
                            katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush;
                    }
                }
            }
            const auto timing_class_name = [&] {
                switch (timing_class) {
                case katana::runtime::ExecutableBlockTimingClass::PureCpu:
                    return "PureCpu";
                case katana::runtime::ExecutableBlockTimingClass::LinearRamOnly:
                    return "LinearRamOnly";
                case katana::runtime::ExecutableBlockTimingClass::RequiresCycleFlush:
                    return "RequiresCycleFlush";
                case katana::runtime::ExecutableBlockTimingClass::NeverChain:
                    return "NeverChain";
                }
                return "NeverChain";
            }();
            dispatch_blocks.push_back(
                {function.entry_address,
                 block.start_address,
                 end - block.start_address,
                 end_kind(block),
                 timing_class_name,
                 std::max<std::uint64_t>(1u, maximum_guest_cycles)});
        }
    }
    std::sort(dispatch_blocks.begin(),
              dispatch_blocks.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
    if (dispatch_blocks.empty())
        throw std::runtime_error("Runtime-Dispatch besitzt keine generierten Bloecke.");
    require_guarded_aot_program_entries(
        program, guarded_aot_entries, "runtime-static-registry");
    for (const auto& native_template : native_templates) {
        if (!emitted_blocks.contains(native_template.source_start))
            throw std::runtime_error("Runtime-Codecopy besitzt keinen generierten AOT-Quellblock.");
        for (const auto& [offset, targets] : native_template.patch_targets) {
            static_cast<void>(offset);
            if (std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
                    return !emitted_blocks.contains(target.block_address);
                }))
                throw std::runtime_error(
                    "Runtime-Codecopy-Patchziel besitzt keinen generierten AOT-Block.");
        }
    }
    for (const auto& native_template : external_native_templates) {
        if (!emitted_blocks.contains(native_template.source_start))
            throw std::runtime_error(
                "Externes Runtimecode-Template besitzt keinen generierten "
                "AOT-Quellblock.");
        for (const auto& patch : native_template.patches) {
            if (std::any_of(
                    patch.allowed_targets.begin(),
                    patch.allowed_targets.end(),
                    [&](const auto& target) {
                        return !emitted_blocks.contains(target.block_address);
                    }))
                throw std::runtime_error(
                    "Externes Runtimecode-Template verweist auf ein nicht "
                    "generiertes AOT-Patchziel.");
        }
    }
    std::unordered_map<const katana::io::ImageSegment*, std::string>
        guarded_entry_source_identities;
    for (const auto& entry : guarded_aot_entries) {
        if (entry.evidence !=
                katana::analysis::ControlFlowEvidence::GuardedPartial ||
            entry.origins.empty() ||
            (entry.entry_byte_extent != 2u &&
             entry.entry_byte_extent != 4u) ||
            entry.source_sites.empty() ||
            entry.source_identity.size() != 71u ||
            !entry.source_identity.starts_with("sha256:") ||
            entry.entry_byte_identity.size() != 71u ||
            !entry.entry_byte_identity.starts_with("sha256:"))
            throw std::runtime_error(
                "Guarded-AOT-Einstieg besitzt keinen vollstaendigen "
                "Analysevertrag bei 0x" +
                symbol(entry.guest_address) + ".");
        const auto* segment =
            image.find_segment(entry.guest_address, entry.entry_byte_extent);
        const auto byte_offset =
            segment != nullptr
                ? segment->byte_offset(entry.guest_address)
                : std::optional<std::size_t>{};
        const auto source_offset =
            segment != nullptr
                ? segment->source_byte_offset(entry.guest_address)
                : std::optional<std::uint64_t>{};
        if (segment == nullptr || !byte_offset.has_value() ||
            !source_offset.has_value() ||
            *byte_offset > segment->bytes.size() ||
            entry.entry_byte_extent >
                segment->bytes.size() - *byte_offset)
            throw std::runtime_error(
                "Guarded-AOT-Einstieg verlor seine gebundenen Quellbytes bei 0x" +
                symbol(entry.guest_address) + ".");
        auto& source_identity = guarded_entry_source_identities[segment];
        if (source_identity.empty()) {
            source_identity =
                "sha256:" +
                katana::io::sha256_bytes(std::string_view(
                    reinterpret_cast<const char*>(segment->bytes.data()),
                    segment->bytes.size()));
        }
        const auto entry_identity =
            "sha256:" +
            katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(
                    segment->bytes.data() + *byte_offset),
                entry.entry_byte_extent));
        if (source_identity != entry.source_identity ||
            *source_offset != entry.source_byte_offset ||
            entry_identity != entry.entry_byte_identity)
            throw std::runtime_error(
                "Guarded-AOT-Einstieg stimmt nicht mit seiner Byte-/"
                "Modulidentitaet ueberein bei 0x" +
                symbol(entry.guest_address) + ".");
        const bool native_template_entry =
            std::any_of(
                native_templates.begin(),
                native_templates.end(),
                [&](const auto& candidate) {
                    return candidate.source_start == entry.guest_address;
                }) ||
            std::any_of(
                external_native_templates.begin(),
                external_native_templates.end(),
                [&](const auto& candidate) {
                    return candidate.source_start == entry.guest_address;
                });
        if (!emitted_blocks.contains(entry.guest_address) &&
            !native_template_entry)
            throw std::runtime_error(
                "Guarded-AOT-Einstieg besitzt weder statischen Block noch "
                "natives Template bei 0x" +
                symbol(entry.guest_address) + ".");
        // A shared-body hint never aliases the entry. The entry block executes
        // first (including its delay slot) and may only then enter the shared
        // body.
        if (entry.shared_body_address != entry.guest_address &&
            !emitted_blocks.contains(entry.shared_body_address))
            throw std::runtime_error(
                "Guarded-AOT-Einstieg besitzt keinen emittierten Shared Body "
                "bei 0x" +
                symbol(entry.guest_address) + " -> 0x" +
                symbol(entry.shared_body_address) + ".");
    }
    std::set<std::string> latent_ids;
    for (const auto& module : latent_modules) {
        if (module.id.empty() || !latent_ids.insert(module.id).second ||
            module.byte_identity.size() != 71u || !module.byte_identity.starts_with("sha256:") ||
            module.byte_size == 0u || (module.byte_size & 3u) != 0u ||
            static_cast<std::uint64_t>(module.source_address) + module.byte_size >
                0x1'0000'0000ull ||
            module.disc_byte_offset > std::numeric_limits<std::uint64_t>::max() - module.byte_size)
            throw std::runtime_error("Latentes AOT-Modul besitzt ungueltige Exportgrenzen.");
        bool has_block = false;
        for (const auto& function : module.program) {
            for (const auto& block : function.blocks)
                has_block = has_block || emitted_blocks.contains(block.start_address);
        }
        if (!has_block)
            throw std::runtime_error("Latentes AOT-Modul besitzt keinen generierten Block.");
    }

    constexpr std::size_t dispatch_blocks_per_shard = 512u;
    const auto shard_count =
        (dispatch_blocks.size() + dispatch_blocks_per_shard - 1u) / dispatch_blocks_per_shard;
    const auto shard_symbol = [](const std::size_t index) {
        std::ostringstream name;
        name << std::setfill('0') << std::setw(5) << index;
        return name.str();
    };

    std::vector<ProjectArtifact> result;
    result.reserve(shard_count + 2u);
    std::ostringstream internal_header;
    internal_header
        << "#pragma once\n\n"
        << "#include \"katana/runtime/block_abi.hpp\"\n"
        << "#include \"katana/runtime/block_table.hpp\"\n"
        << "#include \"katana/runtime/indirect_dispatch.hpp\"\n"
        << "#include \"katana/runtime/platform_services.hpp\"\n"
        << "#include <cstdint>\n#include <vector>\n\n"
        << "namespace " << entry_namespace << "::runtime_dispatch_detail {\n"
        << "extern thread_local katana::runtime::PlatformServices* active_services;\n"
        << "extern thread_local katana::runtime::BlockAddress active_exit_source;\n"
        << "extern thread_local katana::runtime::BlockEndKind active_exit_kind;\n"
        << "extern thread_local katana::runtime::DynamicDispatchSiteClass "
           "active_exit_site_class;\n"
        << "extern thread_local bool tail_dispatch_completed;\n"
        << "[[nodiscard]] katana::runtime::RuntimeBlockFastpathBinding "
           "static_fastpath_binding(std::uint32_t address) noexcept;\n"
        << "void append_static_block(\n"
        << "    std::vector<katana::runtime::RuntimeBlock>& blocks,\n"
        << "    std::uint32_t address, std::uint32_t size,\n"
        << "    katana::runtime::BlockEndKind end_kind,\n"
        << "    katana::runtime::BackendBlockFunction function, const char* provenance);\n"
        << "void register_executable_block(\n"
        << "    const katana::runtime::RuntimeBlockTable& table,\n"
        << "    katana::runtime::PlatformServices& services,\n"
        << "    std::uint32_t address, std::uint32_t size,\n"
        << "    katana::runtime::ExecutableBlockTimingClass timing_class,\n"
        << "    std::uint64_t maximum_guest_cycles);\n";
    for (std::size_t shard = 0u; shard < shard_count; ++shard) {
        const auto suffix = shard_symbol(shard);
        internal_header << "void append_static_blocks_shard_" << suffix
                        << "(std::vector<katana::runtime::RuntimeBlock>& blocks);\n"
                        << "void register_executable_blocks_shard_" << suffix
                        << "(const katana::runtime::RuntimeBlockTable& table, "
                           "katana::runtime::PlatformServices& services);\n";
    }
    internal_header << "} // namespace " << entry_namespace << "::runtime_dispatch_detail\n";
    result.push_back({"include/runtime-dispatch-internal.hpp", internal_header.str()});

    for (std::size_t shard = 0u; shard < shard_count; ++shard) {
        const auto begin = shard * dispatch_blocks_per_shard;
        const auto end = std::min(begin + dispatch_blocks_per_shard, dispatch_blocks.size());
        const auto suffix = shard_symbol(shard);
        std::vector<std::uint32_t> owners;
        owners.reserve(end - begin);
        for (auto index = begin; index < end; ++index)
            owners.push_back(dispatch_blocks[index].owner);
        std::sort(owners.begin(), owners.end());
        owners.erase(std::unique(owners.begin(), owners.end()), owners.end());

        std::ostringstream shard_output;
        shard_output << "#include \"../include/runtime-dispatch-internal.hpp\"\n"
                     << "\n"
                     << "namespace " << entry_namespace << " {\n";
        for (const auto owner : owners) {
            shard_output
                << "katana::runtime::BlockExit fn_" << symbol(owner)
                << "_runtime_entry(katana::runtime::CpuState&, "
                   "katana::runtime::BlockExecutionContext&);\n";
        }
        shard_output << "namespace runtime_dispatch_detail {\n"
                     << "void append_static_blocks_shard_" << suffix
                     << "(std::vector<katana::runtime::RuntimeBlock>& blocks) {\n";
        for (auto index = begin; index < end; ++index) {
            const auto& block = dispatch_blocks[index];
            const auto address = symbol(block.address);
            shard_output << "    append_static_block(blocks, 0x" << address << "u, " << block.size
                         << "u, katana::runtime::BlockEndKind::" << block.end_kind
                         << ", &fn_" << symbol(block.owner)
                         << "_runtime_entry, \"generated-block-"
                         << address << "\");\n";
        }
        shard_output << "}\n\n"
                     << "void register_executable_blocks_shard_" << suffix
                     << "(const katana::runtime::RuntimeBlockTable& table, "
                        "katana::runtime::PlatformServices& services) {\n";
        for (auto index = begin; index < end; ++index) {
            const auto& block = dispatch_blocks[index];
            const auto address = symbol(block.address);
            shard_output << "    register_executable_block(table, services, 0x" << address << "u, "
                         << block.size
                         << "u, katana::runtime::ExecutableBlockTimingClass::" << block.timing_class
                         << ", " << block.maximum_guest_cycles << "u);\n";
            if ((std::string_view(block.timing_class) == "PureCpu" ||
                 std::string_view(block.timing_class) == "LinearRamOnly") &&
                std::string_view(block.end_kind) != "ExceptionReturn" &&
                std::string_view(block.end_kind) != "Sleep" &&
                std::string_view(block.end_kind) != "Exception" &&
                !external_hook_entries.contains(block.address))
                shard_output << "    services.allow_executable_block_chaining(0x" << address
                             << "u);\n";
        }
        shard_output << "}\n"
                     << "} // namespace runtime_dispatch_detail\n"
                     << "} // namespace " << entry_namespace << "\n";
        result.push_back(
            {std::filesystem::path("code") / ("runtime-dispatch-shard-" + suffix + ".cpp"),
             shard_output.str()});
    }

    std::ostringstream output;
    output << "#include \"../include/katana_port.hpp\"\n"
           << "#include \"../include/runtime-dispatch-internal.hpp\"\n"
           << "#include \"katana/runtime/block_abi.hpp\"\n"
           << "#include \"katana/runtime/block_table.hpp\"\n"
           << "#include \"katana/runtime/crash_capsule.hpp\"\n"
           << "#include \"katana/runtime/dispatch_diagnostics.hpp\"\n"
           << "#include \"katana/runtime/disc.hpp\"\n"
           << "#include \"katana/runtime/disc_load_transaction.hpp\"\n"
           << "#include \"katana/runtime/dreamcast_boot.hpp\"\n"
           << "#include \"katana/runtime/executable_modules.hpp\"\n"
           << "#include \"katana/runtime/game_project.hpp\"\n"
           << "#include \"katana/runtime/indirect_dispatch.hpp\"\n"
           << "#include \"katana/runtime/runtime_probe.hpp\"\n"
           << "#include \"katana/io/input_provenance.hpp\"\n";
    output << "#include \"katana/runtime/native_aot_template.hpp\"\n";
    if (diagnostic_interpreter)
        output << "#include \"katana/runtime/dynamic_interpreter.hpp\"\n"
               << "#include \"katana/sh4/decoder.hpp\"\n";
    output
        << "#include <array>\n#include <cerrno>\n#include <chrono>\n#include <cstdlib>\n#include "
           "<iostream>\n#include <limits>\n"
           "#include <memory>\n#include <span>\n#include <stdexcept>\n#include <string>\n"
           "#include <string_view>\n#include <thread>\n#include <utility>\n#include <vector>\n\n"
        << "namespace " << entry_namespace << " {\n";
    output << "thread_local RuntimeMaterializationStatus last_materialization_status;\n"
           << "const RuntimeMaterializationStatus& runtime_materialization_status() noexcept {\n"
           << "    return last_materialization_status;\n"
           << "}\n";
    output << "void register_latent_aot_modules(\n"
           << "    const std::string_view content_identity,\n"
           << "    katana::runtime::ExecutableDiscLoadTransactionCoordinator& transactions) {\n";
    if (latent_modules.empty()) {
        output << "    static_cast<void>(content_identity);\n"
               << "    static_cast<void>(transactions);\n"
               << "    return;\n";
    } else {
        output << "    if (content_identity != "
               << katana::io::quote_json(expected_content_identity) << ")\n"
               << "        throw std::runtime_error(\"source-identity-mismatch\");\n"
               << "    std::vector<katana::runtime::DiscLoadAotModuleDescriptor> "
                  "descriptors;\n"
               << "    descriptors.reserve(" << latent_modules.size() << "u);\n";
        for (const auto& module : latent_modules) {
            output << "    {\n"
                   << "        descriptors.push_back({" << katana::io::quote_json(module.id) << ", "
                   << katana::io::quote_json(expected_content_identity) << ", "
                   << module.disc_byte_offset << "ull, " << module.byte_size << "u, "
                   << katana::io::quote_json(module.byte_identity) << "});\n"
                   << "    }\n";
        }
        output << "    transactions.set_aot_module_descriptors(descriptors);\n";
    }
    output << "}\n"
           << "namespace runtime_dispatch_detail {\n"
           << "thread_local katana::runtime::PlatformServices* active_services = nullptr;\n"
           << "thread_local katana::runtime::BlockAddress active_exit_source;\n"
           << "thread_local katana::runtime::BlockEndKind active_exit_kind =\n"
              "    katana::runtime::BlockEndKind::Fallthrough;\n"
           << "thread_local katana::runtime::DynamicDispatchSiteClass active_exit_site_class =\n"
              "    katana::runtime::DynamicDispatchSiteClass::NotDynamic;\n"
           << "thread_local bool tail_dispatch_completed = false;\n"
           << "void register_executable_block(\n"
           << "    const katana::runtime::RuntimeBlockTable& table,\n"
           << "    katana::runtime::PlatformServices& services,\n"
           << "    std::uint32_t address, std::uint32_t size,\n"
           << "    katana::runtime::ExecutableBlockTimingClass timing_class,\n"
           << "    std::uint64_t maximum_guest_cycles) {\n"
           << "    const auto registered_handle = table.lookup(address, {});\n"
           << "    if (!registered_handle) throw std::runtime_error(\"Registrierter Block "
              "fehlt.\");\n"
           << "    const auto registered = table.resolve(*registered_handle);\n"
           << "    if (!registered) throw std::runtime_error(\"Registrierter Block ist "
              "stale.\");\n"
           << "    services.register_executable_block(\n"
           << "        address, registered->get().physical_origin, size,\n"
              "        katana::runtime::stable_runtime_block_identity(registered->get()),\n"
              "        timing_class, maximum_guest_cycles);\n"
           << "}\n"
           << "void append_static_block(\n"
           << "    std::vector<katana::runtime::RuntimeBlock>& blocks,\n"
           << "    std::uint32_t address, std::uint32_t size,\n"
           << "    katana::runtime::BlockEndKind end_kind,\n"
           << "    katana::runtime::BackendBlockFunction function, const char* provenance) {\n"
           << "    katana::runtime::RuntimeBlock block;\n"
           << "    block.virtual_start = address;\n"
           << "    block.physical_origin = "
              "katana::runtime::canonical_physical_address(address);\n"
           << "    block.size = size; block.end_kind = end_kind; block.function = function;\n"
           << "    block.static_variant_policy = katana::runtime::StaticVariantPolicy::"
              "DirectP1P2RuntimeStateAgnostic;\n"
           << "    block.provenance = provenance;\n"
           << "    block.fastpath = static_fastpath_binding(address);\n"
           << "    blocks.emplace_back(std::move(block));\n"
           << "}\n"
           << "} // namespace runtime_dispatch_detail\n"
           << "using runtime_dispatch_detail::active_exit_kind;\n"
           << "using runtime_dispatch_detail::active_exit_site_class;\n"
           << "using runtime_dispatch_detail::active_exit_source;\n"
           << "using runtime_dispatch_detail::active_services;\n"
           << "using runtime_dispatch_detail::tail_dispatch_completed;\n"
           << "namespace {\n"
           << "thread_local katana::runtime::RuntimeBlockTable* active_table = nullptr;\n"
           << "thread_local katana::runtime::BlockExecutionContext* active_context = nullptr;\n"
           << "thread_local katana::runtime::DispatchDiagnosticRecorder* active_diagnostics = "
              "nullptr;\n"
           << "thread_local katana::runtime::IndirectDispatchMetrics* active_dispatch_metrics = "
              "nullptr;\n"
           << "thread_local katana::runtime::CrashCapsule* active_crash_capsule = nullptr;\n"
           << "thread_local katana::runtime::SystemReplayObservationSession* "
              "active_observations = nullptr;\n"
           << "thread_local katana::runtime::DemandBlockMaterializer* active_materializer = "
              "nullptr;\n"
           << "thread_local const katana::runtime::GameProjectBindings* "
              "active_game_project = nullptr;\n"
           << "thread_local std::uint64_t executed_dispatch_blocks = 0u;\n"
           << "thread_local std::array<std::uint64_t, static_aot_escape_reason_count>\n"
              "    static_aot_escape_counts{};\n"
           << "thread_local std::vector<\n"
              "    std::array<std::uint64_t, static_aot_escape_reason_count>>\n"
              "    static_aot_escape_site_counts;\n"
           << "thread_local std::vector<std::uint32_t> static_aot_escape_site_addresses;\n"
           << "thread_local std::uint64_t static_aot_classified_dispatches = 0u;\n"
           << "void record_static_aot_escape(const StaticAotEscapeReason reason,\n"
              "                              const std::uint64_t site_id,\n"
              "                              const std::uint32_t guest_address) noexcept {\n"
           << "    const auto reason_index = static_cast<std::size_t>(reason);\n"
           << "    if (reason_index >= static_aot_escape_reason_count) return;\n"
           << "    ++static_aot_escape_counts[reason_index];\n"
           << "    ++static_aot_classified_dispatches;\n"
           << "    if (site_id >= static_aot_escape_site_counts.size()) return;\n"
           << "    ++static_aot_escape_site_counts[static_cast<std::size_t>(site_id)]"
              "[reason_index];\n"
           << "    static_aot_escape_site_addresses[static_cast<std::size_t>(site_id)] =\n"
              "        guest_address;\n"
           << "}\n"
           << "StaticAotEscapeReason classify_chain_rejection(\n"
              "        const katana::runtime::ExecutableChainRejectionReason reason) noexcept {\n"
           << "    using Rejection = "
              "katana::runtime::ExecutableChainRejectionReason;\n"
           << "    switch (reason) {\n"
           << "    case Rejection::TargetNotNativeEntrySafe:\n"
              "        return StaticAotEscapeReason::TargetNotNativeEntrySafe;\n"
           << "    case Rejection::TimingNotDeferrable:\n"
              "        return StaticAotEscapeReason::TimingNotDeferrable;\n"
           << "    case Rejection::SchedulerDue:\n"
              "        return StaticAotEscapeReason::SchedulerDue;\n"
           << "    case Rejection::InterruptAcceptable:\n"
              "        return StaticAotEscapeReason::InterruptAcceptable;\n"
           << "    case Rejection::VariantOrGeneration:\n"
           << "    case Rejection::CodeGeneration:\n"
              "        return StaticAotEscapeReason::VariantOrGeneration;\n"
           << "    case Rejection::CycleQuantum:\n"
              "        return StaticAotEscapeReason::GuestCycleQuantum;\n"
           << "    case Rejection::GuestCycleBudget:\n"
              "        return StaticAotEscapeReason::GuestCycleBudget;\n"
           << "    case Rejection::NoPendingGuestCycles:\n"
              "        return StaticAotEscapeReason::MmioOrArchitectureBoundary;\n"
           << "    case Rejection::AddressTranslation:\n"
              "        return StaticAotEscapeReason::VariantOrGeneration;\n"
           << "    case Rejection::TargetNotRegistered:\n"
           << "    case Rejection::GameEntryBarrier:\n"
           << "    case Rejection::ChainingDisabled:\n"
           << "    case Rejection::MissingRuntimeContract:\n"
           << "    case Rejection::DiagnosticMode:\n"
              "        return StaticAotEscapeReason::PartitionOrSymbol;\n"
           << "    case Rejection::None:\n"
           << "        return StaticAotEscapeReason::Unclassified;\n"
           << "    }\n"
           << "    return StaticAotEscapeReason::Unclassified;\n"
           << "}\n"
           << "bool detailed_dispatch_diagnostics_enabled() noexcept {\n"
           << "    static const bool enabled = [] {\n"
           << "        const auto enabled_by_one = [](const char* name) {\n"
           << "            const auto* value = std::getenv(name);\n"
           << "            return value != nullptr && std::string_view(value) == \"1\";\n"
           << "        };\n"
           << "        const auto* runtime_probe = std::getenv(\"KATANA_RUNTIME_PROBE\");\n"
           << "        return enabled_by_one(\"KATANA_PORT_DIAGNOSTICS\") ||\n"
           << "               enabled_by_one(\"KATANA_PORT_DIAGNOSTICS_FULL\") ||\n"
           << "               (runtime_probe != nullptr && *runtime_probe != '\\0');\n"
           << "    }();\n"
           << "    return " << (diagnostic_interpreter ? "true" : "false")
           << " || enabled;\n"
           << "}\n"
           << "enum class DispatchChainBoundary { NestedCall, ProgramRoot };\n"
           << "void dispatch_chain(katana::runtime::CpuState&, std::uint32_t, "
              "katana::runtime::IndirectDispatchKind, "
              "katana::runtime::RuntimeDispatchClass, bool, DispatchChainBoundary);\n"
           << "class ServiceScope {\n"
           << "  public:\n"
           << "    ServiceScope(katana::runtime::PlatformServices& services,\n"
              "                 katana::runtime::RuntimeBlockTable& table,\n"
              "                 katana::runtime::BlockExecutionContext& context,\n"
              "                 katana::runtime::DispatchDiagnosticRecorder& diagnostics,\n"
              "                 katana::runtime::IndirectDispatchMetrics& dispatch_metrics,\n"
              "                 katana::runtime::CrashCapsule& crash_capsule,\n"
              "                 katana::runtime::SystemReplayObservationSession& observations,\n"
              "                 katana::runtime::DemandBlockMaterializer& materializer,\n"
              "                 const katana::runtime::GameProjectBindings* game_project) {\n"
           << "        if (active_services != nullptr) throw std::runtime_error(\"Runtime-Dispatch "
              "ist nicht reentrant.\");\n"
           << "        active_services = &services; active_table = &table;\n"
              "        active_context = &context;\n"
              "        active_diagnostics = detailed_dispatch_diagnostics_enabled()\n"
              "                                 ? &diagnostics : nullptr;\n"
              "        active_dispatch_metrics = &dispatch_metrics;\n"
              "        active_crash_capsule = &crash_capsule;\n"
              "        active_observations = &observations;\n"
              "        active_materializer = &materializer;\n"
              "        active_game_project = game_project;\n"
              "        executed_dispatch_blocks = 0u;\n"
           << "        static_aot_escape_counts.fill(0u);\n"
           << "        static_aot_classified_dispatches = 0u;\n"
           << "        static_aot_escape_site_counts.assign("
           << (emitted_blocks.size() + 1u)
           << "u, {});\n"
           << "        static_aot_escape_site_addresses.assign("
           << (emitted_blocks.size() + 1u)
           << "u, 0u);\n"
           << "    }\n"
           << "    ~ServiceScope() { active_services = nullptr; active_table = nullptr;\n"
              "        active_context = nullptr; active_diagnostics = nullptr;\n"
              "        active_dispatch_metrics = nullptr; active_crash_capsule = nullptr;\n"
              "        active_observations = nullptr;\n"
              "        active_materializer = nullptr; active_game_project = nullptr; }\n"
           << "};\n";
    if (diagnostic_interpreter)
        output << "katana::runtime::BlockExit dispatch_dynamic_interpreter(\n"
                  "        katana::runtime::CpuState& cpu,\n"
                  "        katana::runtime::BlockExecutionContext& context) {\n"
                  "    if (active_services == nullptr)\n"
                  "        throw std::runtime_error(\"Dynamischer SH-4-Pfad ohne "
                  "Plattformdienste.\");\n"
                  "    const auto source = cpu.pc;\n"
                  "    const auto interpreted =\n"
                  "        katana::runtime::execute_dynamic_sh4_block(cpu, *active_services, 1u);\n"
                  "    return katana::runtime::make_block_exit(\n"
                  "        cpu, context, interpreted.end_kind,\n"
                  "        {source, katana::runtime::canonical_physical_address(source)},\n"
                  "        katana::runtime::BlockAddress{\n"
                  "            cpu.pc, katana::runtime::canonical_physical_address(cpu.pc)});\n"
                  "}\n";
    const auto emitted_composite_callback_batch_count =
        diagnostic_interpreter ? std::size_t{0u} : composite_callback_batches.size();
    output << "constexpr std::array<CompositeCallbackBatchDescriptor, "
           << emitted_composite_callback_batch_count
           << "u> composite_callback_batch_descriptors{{\n";
    if (!diagnostic_interpreter) {
        for (const auto* proof : composite_callback_emission) {
            const auto& descriptor = proof->descriptor;
            const auto kind =
                descriptor.kind ==
                        CompositeCallbackBatchProof::Kind::FlagPollEqualImmediate
                    ? "CompositeCallbackBatchKind::FlagPollEqualImmediate"
                    : "CompositeCallbackBatchKind::MemoryCopy";
            output << "    {" << kind << ", 0x"
                   << symbol(descriptor.call_block_address) << "u, 0x"
                   << symbol(descriptor.call_instruction_address) << "u, 0x"
                   << symbol(descriptor.continuation_address) << "u, 0x"
                   << symbol(descriptor.exit_address) << "u, 0x"
                   << symbol(descriptor.kernel_address) << "u, 0x"
                   << symbol(descriptor.kernel_return_address) << "u, 0x"
                   << symbol(descriptor.source_load_instruction_address) << "u, 0x"
                   << symbol(descriptor.target_store_instruction_address) << "u, 0x"
                   << symbol(descriptor.outer_branch_instruction_address) << "u, "
                   << descriptor.call_block_size << "u, "
                   << descriptor.continuation_size << "u, "
                   << descriptor.kernel_size << "u, "
                   << descriptor.kernel_return_size << "u, "
                   << static_cast<unsigned>(descriptor.callback_register) << "u, "
                   << static_cast<unsigned>(descriptor.destination_register) << "u, "
                   << static_cast<unsigned>(descriptor.count_register) << "u, "
                   << static_cast<unsigned>(descriptor.limit_register) << "u, "
                   << static_cast<unsigned>(descriptor.flag_base_register) << "u, "
                   << static_cast<unsigned>(descriptor.flag_value_register) << "u, "
                   << descriptor.flag_displacement << ", "
                   << descriptor.flag_expected_value << "u, "
                   << static_cast<unsigned>(
                          descriptor.current_round_instruction_count)
                   << "u, "
                   << static_cast<unsigned>(
                          descriptor.subsequent_round_instruction_count)
                   << "u, " << descriptor.call_block_guest_cycles << "u, "
                   << descriptor.continuation_guest_cycles << "u, "
                   << descriptor.kernel_guest_cycles << "u, "
                   << descriptor.kernel_return_guest_cycles << "u, "
                   << descriptor.current_round_guest_cycles << "u, "
                   << descriptor.subsequent_round_guest_cycles
                   << "u, \"generated-block-"
                   << symbol(descriptor.call_block_address)
                   << "\", \"generated-block-"
                   << symbol(descriptor.continuation_address)
                   << "\", \"generated-block-"
                   << symbol(descriptor.kernel_address)
                   << "\", ";
            if (descriptor.kind == CompositeCallbackBatchProof::Kind::MemoryCopy)
                output << "\"generated-block-"
                       << symbol(descriptor.kernel_return_address) << "\"";
            else
                output << "\"\"";
            output << "},\n";
        }
    }
    output << "}};\n";
    const auto emitted_memory_fill_loop_count =
        diagnostic_interpreter ? std::size_t{0u} : memory_fill_loops.size();
    output << "constexpr std::array<MemoryFillLoopBatchDescriptor, "
           << emitted_memory_fill_loop_count
           << "u> memory_fill_loop_batch_descriptors{{\n";
    if (!diagnostic_interpreter) {
        for (const auto& proof : memory_fill_loops) {
            const auto& descriptor = proof.descriptor;
            output << "    {0x" << symbol(descriptor.guard_address) << "u, 0x"
                   << symbol(descriptor.body_address) << "u, 0x"
                   << symbol(descriptor.exit_address) << "u, 0x"
                   << symbol(descriptor.store_instruction_address) << "u, 0x"
                   << symbol(descriptor.increment_instruction_address) << "u, 0x"
                   << symbol(descriptor.limit_load_instruction_address) << "u, 0x"
                   << symbol(descriptor.compare_instruction_address) << "u, 0x"
                   << symbol(descriptor.branch_instruction_address) << "u, "
                   << descriptor.guard_size << "u, " << descriptor.body_size << "u, "
                   << static_cast<unsigned>(descriptor.cursor_register) << "u, "
                   << static_cast<unsigned>(descriptor.fill_register) << "u, "
                   << static_cast<unsigned>(descriptor.limit_register) << "u, "
                   << static_cast<unsigned>(descriptor.limit_pointer_register) << "u, "
                   << static_cast<unsigned>(descriptor.store_width) << "u, "
                   << static_cast<unsigned>(descriptor.step) << "u, "
                   << static_cast<unsigned>(descriptor.round_instruction_count) << "u, "
                   << static_cast<unsigned>(descriptor.guard_instruction_count) << "u, "
                   << static_cast<unsigned>(descriptor.store_guest_cycles) << "u, "
                   << descriptor.guard_guest_cycles << "u, "
                   << descriptor.round_guest_cycles << "u, \"generated-block-"
                   << symbol(descriptor.guard_address) << "\", \"generated-block-"
                   << symbol(descriptor.body_address) << "\"},\n";
        }
    }
    output << "}};\n";
    const auto emitted_mmio_wait_loop_count =
        diagnostic_interpreter ? std::size_t{0u} : mmio_wait_loops.size();
    output << "constexpr std::array<MmioWaitLoopBatchDescriptor, "
           << emitted_mmio_wait_loop_count
           << "u> mmio_wait_loop_batch_descriptors{{\n";
    if (!diagnostic_interpreter) {
        for (const auto& proof : mmio_wait_loops) {
            const auto& descriptor = proof.descriptor;
            output << "    {0x" << symbol(descriptor.loop_header) << "u, 0x"
                   << symbol(descriptor.read_site) << "u, 0x"
                   << symbol(descriptor.pointer_literal_address) << "u, 0x"
                   << symbol(descriptor.mmio_guest_address) << "u, 0x"
                   << symbol(descriptor.mmio_physical_address) << "u, 0x"
                   << symbol(descriptor.backedge_instruction_address) << "u, "
                   << static_cast<unsigned>(descriptor.pointer_register) << "u, "
                   << static_cast<unsigned>(descriptor.value_register) << "u, "
                   << static_cast<unsigned>(descriptor.mask_register) << "u, "
                   << static_cast<unsigned>(descriptor.round_instruction_count) << "u, "
                   << descriptor.test_mask << "u, "
                   << (descriptor.pointer_from_register ? "true" : "false") << ", "
                   << (descriptor.branch_on_true ? "true" : "false") << ", "
                   << descriptor.round_guest_cycles << "u, "
                   << descriptor.pre_read_guest_cycles << "u, "
                   << static_cast<unsigned>(descriptor.read_guest_cycles) << "u, "
                   << static_cast<unsigned>(descriptor.test_guest_cycles) << "u, "
                   << static_cast<unsigned>(descriptor.branch_guest_cycles)
                   << "u, \"generated-block-"
                   << symbol(descriptor.loop_header) << "\"},\n";
        }
    }
    output << "}};\n";
    const auto emitted_counted_loop_count =
        diagnostic_interpreter ? std::size_t{0u} : counted_loops.size();
    output << "constexpr std::array<CountedLoopBatchDescriptor, " << emitted_counted_loop_count
           << "u> counted_loop_batch_descriptors{{\n";
    if (!diagnostic_interpreter) {
        for (const auto& proof : counted_loops) {
            const auto& descriptor = proof.descriptor;
            output << "    {0x" << symbol(descriptor.guard_address) << "u, 0x"
                   << symbol(descriptor.increment_address) << "u, " << descriptor.increment_size
                   << "u, 0x" << symbol(descriptor.limit_address) << "u, 0x"
                   << symbol(descriptor.first_counter_read_instruction_address) << "u, 0x"
                   << symbol(descriptor.store_instruction_address) << "u, 0x"
                   << symbol(descriptor.pre_store_instruction_address) << "u, "
                   << descriptor.counter_displacement << ", "
                   << static_cast<unsigned>(descriptor.counter_base_register) << "u, "
                   << static_cast<unsigned>(descriptor.limit_register) << "u, "
                   << static_cast<unsigned>(descriptor.compare_register) << "u, "
                   << static_cast<unsigned>(descriptor.increment_register) << "u, "
                   << static_cast<unsigned>(descriptor.step) << "u, "
                   << static_cast<unsigned>(descriptor.limit_width) << "u, "
                   << static_cast<unsigned>(descriptor.guard_instruction_count) << "u, "
                   << static_cast<unsigned>(descriptor.round_instruction_count) << "u, "
                   << static_cast<unsigned>(descriptor.prefix_guest_cycles) << "u, "
                   << static_cast<unsigned>(descriptor.first_counter_read_guest_cycles)
                   << "u, "
                   << static_cast<unsigned>(descriptor.store_guest_cycles) << "u, "
                   << (descriptor.signed_word_limit ? "true" : "false") << ", "
                   << descriptor.guard_guest_cycles << "u, "
                   << descriptor.round_guest_cycles << "u, \"generated-block-"
                   << symbol(descriptor.guard_address) << "\", \"generated-block-"
                   << symbol(descriptor.increment_address) << "\"},\n";
        }
    }
    output << "}};\n";
    output
        << "std::uint64_t configured_block_budget() {\n"
           "    static const auto budget = [] {\n"
           "        const auto* text = std::getenv(\"KATANA_PORT_BLOCK_LIMIT\");\n"
           "        if (text == nullptr || *text == '\\0')\n"
           "            return std::numeric_limits<std::uint64_t>::max();\n"
           "        errno = 0;\n"
           "        char* end = nullptr;\n"
           "        const auto value = std::strtoull(text, &end, 10);\n"
           "        if (errno != 0 || end == text || *end != '\\0' || value == 0u)\n"
           "            throw std::runtime_error(\"KATANA_PORT_BLOCK_LIMIT ist ungueltig.\");\n"
           "        return static_cast<std::uint64_t>(value);\n"
           "    }();\n"
           "    return budget;\n"
           "}\n\n"
           "std::uint64_t configured_progress_interval() {\n"
           "    static const auto interval = [] {\n"
           "        const auto* text = std::getenv(\"KATANA_PORT_PROGRESS_INTERVAL\");\n"
           "        if (text == nullptr || *text == '\\0') return std::uint64_t{0u};\n"
           "        errno = 0; char* end = nullptr;\n"
           "        const auto value = std::strtoull(text, &end, 10);\n"
           "        if (errno != 0 || end == text || *end != '\\0')\n"
           "            throw std::runtime_error(\"KATANA_PORT_PROGRESS_INTERVAL ist "
           "ungueltig.\");\n"
           "        return static_cast<std::uint64_t>(value);\n"
           "    }();\n"
           "    return interval;\n"
           "}\n\n"
           "void dispatch_chain(katana::runtime::CpuState& cpu, std::uint32_t target,\n"
           "                    katana::runtime::IndirectDispatchKind kind,\n"
           "                    katana::runtime::RuntimeDispatchClass dispatch_class,\n"
           "                    bool diagnostic, DispatchChainBoundary boundary) {\n"
           "    const auto block_budget = configured_block_budget();\n"
           "    const auto program_return_sentinel = cpu.pr;\n"
           "    katana::runtime::BlockVariantKey dispatch_variant{};\n"
           "    std::uint32_t dispatch_callsite = cpu.pc;\n"
           "    katana::runtime::BlockAddress dispatch_source{cpu.pc,\n"
           "        katana::runtime::canonical_physical_address(cpu.pc)};\n"
           "    auto dispatch_origin =\n"
           "        dispatch_class == katana::runtime::RuntimeDispatchClass::RuntimeOnly\n"
           "            ? katana::runtime::DispatchResolutionOrigin::RuntimeOnly\n"
           "            : katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "    auto pending_escape_reason = boundary == DispatchChainBoundary::ProgramRoot\n"
           "        ? StaticAotEscapeReason::ProgramEntry\n"
           "        : kind == katana::runtime::IndirectDispatchKind::Call\n"
           "            ? StaticAotEscapeReason::CallUnknown\n"
           "            : StaticAotEscapeReason::DynamicTarget;\n"
           "    std::uint64_t pending_escape_site_id = 0u;\n"
           "    std::uint32_t pending_escape_address = dispatch_callsite;\n"
           "    for (;;) {\n"
           "        const auto lifecycle = active_services->poll_host_lifecycle();\n"
           "        if (lifecycle == katana::runtime::PlatformLifecycleState::Shutdown)\n"
           "            throw katana::runtime::PlatformShutdownRequested();\n"
           "        if (lifecycle == katana::runtime::PlatformLifecycleState::Paused) {\n"
           "            std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "            continue;\n"
           "        }\n"
           "        if (executed_dispatch_blocks >= block_budget)\n"
           "            throw std::runtime_error(\"KATANA_PORT_BLOCK_LIMIT erreicht.\");\n"
           "        ++executed_dispatch_blocks;\n"
           "        record_static_aot_escape(\n"
           "            pending_escape_reason, pending_escape_site_id,\n"
           "            pending_escape_address);\n"
           "        const auto progress_interval = configured_progress_interval();\n"
           "        if (progress_interval != 0u &&\n"
           "            executed_dispatch_blocks % progress_interval == 0u)\n"
           "            std::cerr << \"KATANA_PORT_BLOCK_PROGRESS blocks=\"\n"
           "                      << executed_dispatch_blocks << \" pc=\" << cpu.pc\n"
           "                      << \" guest_cycles=\" << active_services->scheduler_cycle()\n"
           "                      << '\\n';\n"
           "        if (cpu.sleeping) {\n"
           "            if (active_services->poll_interrupt().has_value()) {\n"
           "                target = cpu.pc;\n"
           "                kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "                dispatch_class = "
           "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "                dispatch_origin = "
           "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "                diagnostic = false;\n"
           "            } else {\n"
           "                const auto next_event = "
           "active_services->next_scheduler_event_cycle();\n"
           "                if (!next_event) throw std::runtime_error(\"SLEEP besitzt kein "
           "Wakeup-Ereignis.\");\n"
           "                const auto scheduler = active_services->consume_guest_cycles(\n"
           "                    *next_event - active_services->scheduler_cycle(), 1024u);\n"
           "            if (scheduler.budget_exhausted)\n"
           "                throw std::runtime_error(\"Schedulerbudget erschoepft\");\n"
           "                if (scheduler.guest_cycle_budget_exhausted)\n"
           "                    throw katana::runtime::GuestCycleBudgetReached(\n"
           "                        scheduler.guest_cycle);\n"
           "                if (!active_services->poll_interrupt().has_value()) {\n"
           "                    pending_escape_reason = "
           "StaticAotEscapeReason::SchedulerDue;\n"
           "                    pending_escape_site_id = 0u;\n"
           "                    pending_escape_address = cpu.pc;\n"
           "                    continue;\n"
           "                }\n"
           "                target = cpu.pc;\n"
           "                kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "                dispatch_class = "
           "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "                dispatch_origin = "
           "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "                diagnostic = false;\n"
           "            }\n"
           "        }\n"
           "        const auto selected = [&] {\n"
           "            try {\n"
           "                auto result = katana::runtime::dispatch_indirect(\n"
           "                    cpu, *active_table,\n"
           "                    {kind, dispatch_callsite, target, cpu.pr, dispatch_source,\n"
           "                     dispatch_variant,\n"
           "                     dispatch_origin,\n"
           "                     diagnostic ? active_diagnostics : nullptr, dispatch_class,\n"
           "                     active_dispatch_metrics, active_materializer});\n"
           "                active_observations->observe_block_dispatch_hit(\n"
           "                    dispatch_class, result.materialized);\n"
           "                return result;\n"
           "            } catch (const katana::runtime::IndirectDispatchError& error) {\n"
           "                active_crash_capsule->note_first_error(\n"
           "                    static_cast<std::uint32_t>(error.error()),\n"
           "                    error.callsite(), error.target());\n"
           "                active_observations->observe_block_dispatch_miss(\n"
           "                    *active_dispatch_metrics);\n"
           "                throw;\n"
           "            }\n"
           "        }();\n"
           "        const auto& selected_block = selected.execution;\n"
           "        const auto selected_escape_site_id = selected_block.block.id;\n"
           "        active_crash_capsule->note_block(\n"
           "            selected.diagnostic_target, selected_block.virtual_start,\n"
           "            active_context->scheduler_cycle);\n"
           "        dispatch_variant = selected_block.variant;\n"
           "        if (selected_block.function == nullptr)\n"
           "            throw std::runtime_error(\"Runtime-Dispatchziel besitzt keinen generierten "
           "Block.\");\n";
    if (external_game_project_hooks) {
        output
            << "        if (active_game_project != nullptr) {\n"
               "            const auto* function_hook =\n"
               "                active_game_project->function_override(\n"
               "                    selected_block.virtual_start);\n"
               "            const auto* mid_hook =\n"
               "                active_game_project->mid_function_hook(\n"
               "                    selected_block.virtual_start);\n"
               "            if ((function_hook != nullptr && "
               "function_hook->callback != nullptr) ||\n"
               "                (mid_hook != nullptr && mid_hook->callback != "
               "nullptr)) {\n"
               "                katana::runtime::flush_pending_guest_cycles(\n"
               "                    cpu, *active_services);\n"
               "                const auto apply_hook = [&](const auto& "
               "result) {\n"
               "                    if (result.status == "
               "katana::runtime::GameProjectHookDispatchStatus::Invalid)\n"
               "                        throw "
               "katana::runtime::GameProjectHookContractError(\n"
               "                            katana::runtime::"
               "GameProjectHookContractFailure::InvalidResult);\n"
               "                    if (result.status != "
               "katana::runtime::GameProjectHookDispatchStatus::Applied)\n"
               "                        return false;\n"
               "                    if (result.application == "
               "katana::runtime::GameProjectHookApplication::Abort)\n"
               "                        throw std::runtime_error(\n"
               "                            \"Externes Game-Project brach mit "
               "Fehlercode \" +\n"
               "                            std::to_string(result.error_code) "
               "+ \".\");\n"
               "                    if (result.application != "
               "katana::runtime::GameProjectHookApplication::ControlTransfer)\n"
               "                        return false;\n"
               "                    target = cpu.pc;\n"
               "                    dispatch_callsite = "
               "selected_block.virtual_start;\n"
               "                    dispatch_source = "
               "{selected_block.virtual_start,\n"
               "                        selected_block.physical_origin};\n"
               "                    kind = "
               "katana::runtime::IndirectDispatchKind::TailJump;\n"
               "                    dispatch_class = "
               "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
               "                    dispatch_origin = "
               "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
               "                    diagnostic = false;\n"
               "                    return true;\n"
               "                };\n"
               "                bool transferred = false;\n"
               "                if (function_hook != nullptr)\n"
               "                    transferred = apply_hook(\n"
               "                        active_game_project->"
               "invoke_function_override(\n"
               "                            selected_block.virtual_start, cpu, "
               "active_services));\n"
               "                if (!transferred && mid_hook != nullptr)\n"
               "                    transferred = apply_hook(\n"
               "                        active_game_project->"
               "invoke_mid_function_hook(\n"
               "                            selected_block.virtual_start, cpu, "
               "active_services));\n"
               "                if (!transferred) {\n"
               "                    if (cpu.pc != "
               "selected_block.virtual_start)\n"
               "                        throw "
               "katana::runtime::GameProjectHookContractError(\n"
               "                            katana::runtime::"
               "GameProjectHookContractFailure::"
               "ContinueChangedProgramCounter);\n"
               "                    bool generation_current =\n"
               "                        "
               "!selected_block.generation_guard_reusable;\n"
               "                    if "
               "(selected_block.generation_guard.kind ==\n"
               "                        katana::runtime::"
               "BlockDispatchGenerationGuardKind::StaticAot)\n"
               "                        generation_current = "
               "active_table->\n"
               "                            "
               "static_dispatch_generation_guard_current(\n"
               "                                "
               "selected_block.generation_guard);\n"
               "                    else if "
               "(selected_block.generation_guard.kind ==\n"
               "                             katana::runtime::"
               "BlockDispatchGenerationGuardKind::Materializer)\n"
               "                        generation_current = "
               "active_materializer->\n"
               "                            "
               "dispatch_generation_guard_current(\n"
               "                                cpu, "
               "selected_block.generation_guard);\n"
               "                    const auto refreshed_block =\n"
               "                        "
               "active_table->resolve(selected_block.block);\n"
               "                    if (!generation_current || "
               "!refreshed_block ||\n"
               "                        refreshed_block->get().function != "
               "selected_block.function ||\n"
               "                        refreshed_block->get().virtual_start != "
               "selected_block.virtual_start ||\n"
               "                        refreshed_block->get().physical_origin "
               "!= selected_block.physical_origin ||\n"
               "                        refreshed_block->get().size != "
               "selected_block.size ||\n"
               "                        refreshed_block->get().variant != "
               "selected_block.variant ||\n"
               "                        refreshed_block->get().end_kind != "
               "selected_block.end_kind ||\n"
               "                        "
               "refreshed_block->get().runtime_registered !=\n"
               "                            "
               "selected_block.runtime_registered)\n"
               "                        throw "
               "katana::runtime::GameProjectHookContractError(\n"
               "                            katana::runtime::"
               "GameProjectHookContractFailure::StaleBlockExecution);\n"
               "                }\n"
               "                if (transferred) {\n"
               "                    pending_escape_reason = "
               "StaticAotEscapeReason::Hook;\n"
               "                    pending_escape_site_id = "
               "selected_escape_site_id;\n"
               "                    pending_escape_address = "
               "selected_block.virtual_start;\n"
               "                    continue;\n"
               "                }\n"
               "            }\n"
               "        }\n";
    }
    output <<
           "        active_services->begin_executable_block(\n"
           "            selected_block.virtual_start,\n"
           "            selected_block.physical_origin,\n"
           "            selected_block.size,\n"
           "            selected_block.variant,\n"
           "            selected_block.runtime_registered);\n"
           "        const auto finalize_product_fastpath = [\n"
           "                &](const std::uint64_t fastpath_retired_before,\n"
           "                    const std::uint64_t fastpath_exception_generation_before,\n"
           "                    const katana::runtime::BlockAddress nominal_source,\n"
           "                    const katana::runtime::BlockEndKind nominal_kind) {\n"
           "            if (cpu.retired_guest_instructions < fastpath_retired_before)\n"
           "                throw std::runtime_error(\n"
           "                    \"Fastpath-Gastinstruktionszaehler lief rueckwaerts.\");\n"
           "            const auto fastpath_retired =\n"
           "                cpu.retired_guest_instructions - fastpath_retired_before;\n"
           "            const bool fastpath_new_exception =\n"
           "                cpu.exception_generation != fastpath_exception_generation_before;\n"
           "            const auto fastpath_completion =\n"
           "                katana::runtime::finalize_guest_block(\n"
           "                    cpu, *active_services,\n"
           "                    active_context->scheduler_event_budget,\n"
           "                    selected.diagnostic_target, fastpath_retired,\n"
           "                    fastpath_new_exception, fastpath_new_exception);\n"
           "            active_context->scheduler_cycle =\n"
           "                fastpath_completion.scheduler.guest_cycle;\n"
           "            const auto fastpath_source = fastpath_new_exception\n"
           "                ? katana::runtime::BlockAddress{\n"
           "                      cpu.last_exception_instruction_pc,\n"
           "                      cpu.last_exception_instruction_physical_pc}\n"
           "                : nominal_source;\n"
           "            active_exit_source = fastpath_source;\n"
           "            active_exit_kind = fastpath_new_exception\n"
           "                ? katana::runtime::BlockEndKind::Exception\n"
           "                : fastpath_completion.interrupt.has_value()\n"
           "                    ? katana::runtime::BlockEndKind::InterruptSafepoint\n"
           "                    : nominal_kind;\n"
           "            active_exit_site_class =\n"
           "                katana::runtime::DynamicDispatchSiteClass::NotDynamic;\n"
           "            if (fastpath_new_exception)\n"
           "                active_observations->observe_guest_exception(\n"
           "                    cpu.last_exception_cause);\n"
           "            return fastpath_source;\n"
           "        };\n"
           "        switch (selected_block.fastpath.kind) {\n"
           "        case katana::runtime::RuntimeBlockFastpathKind::CompositeCallback: {\n"
           "            const auto& composite_callback = *static_cast<const\n"
           "                CompositeCallbackBatchDescriptor*>(\n"
           "                    selected_block.fastpath.descriptor);\n"
           "            if (kind != katana::runtime::IndirectDispatchKind::Call ||\n"
           "                dispatch_callsite !=\n"
           "                    composite_callback.call_instruction_address)\n"
           "                break;\n"
           "            const auto fastpath_retired_before =\n"
           "                cpu.retired_guest_instructions;\n"
           "            const auto fastpath_exception_generation_before =\n"
           "                cpu.exception_generation;\n"
           "            if (try_product_composite_callback_batch(\n"
           "                    cpu, *active_services, selected_block,\n"
           "                    composite_callback)) {\n"
           "                const auto fastpath_source = finalize_product_fastpath(\n"
           "                    fastpath_retired_before,\n"
           "                    fastpath_exception_generation_before,\n"
           "                    {composite_callback.outer_branch_instruction_address,\n"
           "                     katana::runtime::canonical_physical_address(\n"
           "                         composite_callback.outer_branch_instruction_address)},\n"
           "                    katana::runtime::BlockEndKind::ConditionalBranch);\n"
           "                target = cpu.pc;\n"
           "                dispatch_callsite = fastpath_source.virtual_address;\n"
           "                dispatch_source = fastpath_source;\n"
           "                kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "                dispatch_class =\n"
           "                    katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "                dispatch_origin =\n"
           "                    katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "                diagnostic = false;\n"
           "                pending_escape_reason = "
           "StaticAotEscapeReason::ProductFastpath;\n"
           "                pending_escape_site_id = selected_escape_site_id;\n"
           "                pending_escape_address = selected_block.virtual_start;\n"
           "                continue;\n"
           "            }\n"
           "            break;\n"
           "        }\n"
           "        case katana::runtime::RuntimeBlockFastpathKind::MemoryFill: {\n"
           "            const auto& memory_fill_loop = *static_cast<const\n"
           "                MemoryFillLoopBatchDescriptor*>(\n"
           "                    selected_block.fastpath.descriptor);\n"
           "            const auto fastpath_retired_before =\n"
           "                cpu.retired_guest_instructions;\n"
           "            const auto fastpath_exception_generation_before =\n"
           "                cpu.exception_generation;\n"
           "            if (try_product_memory_fill_loop_batch(\n"
           "                    cpu, *active_services, selected_block, "
           "memory_fill_loop)) {\n"
           "                const auto fastpath_source = finalize_product_fastpath(\n"
           "                    fastpath_retired_before,\n"
           "                    fastpath_exception_generation_before,\n"
           "                    {memory_fill_loop.branch_instruction_address,\n"
           "                     katana::runtime::canonical_physical_address(\n"
           "                         memory_fill_loop.branch_instruction_address)},\n"
           "                    katana::runtime::BlockEndKind::ConditionalBranch);\n"
           "                target = cpu.pc;\n"
           "                dispatch_callsite = fastpath_source.virtual_address;\n"
           "                dispatch_source = fastpath_source;\n"
           "                kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "                dispatch_class =\n"
           "                    katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "                dispatch_origin =\n"
           "                    katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "                diagnostic = false;\n"
           "                pending_escape_reason = "
           "StaticAotEscapeReason::ProductFastpath;\n"
           "                pending_escape_site_id = selected_escape_site_id;\n"
           "                pending_escape_address = selected_block.virtual_start;\n"
           "                continue;\n"
           "            }\n"
           "            break;\n"
           "        }\n"
           "        case katana::runtime::RuntimeBlockFastpathKind::MmioWait: {\n"
           "            const auto& mmio_wait_loop = *static_cast<const\n"
           "                MmioWaitLoopBatchDescriptor*>(\n"
           "                    selected_block.fastpath.descriptor);\n"
           "            const auto fastpath_retired_before =\n"
           "                cpu.retired_guest_instructions;\n"
           "            const auto fastpath_exception_generation_before =\n"
           "                cpu.exception_generation;\n"
           "            if (try_product_mmio_wait_loop_batch(\n"
           "                    cpu, *active_services, selected_block, "
           "mmio_wait_loop)) {\n"
           "                const auto fastpath_source = finalize_product_fastpath(\n"
           "                    fastpath_retired_before,\n"
           "                    fastpath_exception_generation_before,\n"
           "                    {mmio_wait_loop.backedge_instruction_address,\n"
           "                     katana::runtime::canonical_physical_address(\n"
           "                         mmio_wait_loop.backedge_instruction_address)},\n"
           "                    katana::runtime::BlockEndKind::ConditionalBranch);\n"
           "                target = cpu.pc;\n"
           "                dispatch_callsite = fastpath_source.virtual_address;\n"
           "                dispatch_source = fastpath_source;\n"
           "                kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "                dispatch_class =\n"
           "                    katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "                dispatch_origin =\n"
           "                    katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "                diagnostic = false;\n"
           "                pending_escape_reason = "
           "StaticAotEscapeReason::ProductFastpath;\n"
           "                pending_escape_site_id = selected_escape_site_id;\n"
           "                pending_escape_address = selected_block.virtual_start;\n"
           "                continue;\n"
           "            }\n"
           "            break;\n"
           "        }\n"
           "        case katana::runtime::RuntimeBlockFastpathKind::CountedLoop: {\n"
           "            const auto& counted_loop = *static_cast<const\n"
           "                CountedLoopBatchDescriptor*>(\n"
           "                    selected_block.fastpath.descriptor);\n"
           "            const auto fastpath_retired_before =\n"
           "                cpu.retired_guest_instructions;\n"
           "            const auto fastpath_exception_generation_before =\n"
           "                cpu.exception_generation;\n"
           "            if (try_product_counted_loop_batch(\n"
           "                    cpu, *active_services, selected_block, "
           "counted_loop)) {\n"
           "                const auto counted_fastpath_kind =\n"
           "                    cpu.active_instruction_pc ==\n"
           "                            counted_loop.store_instruction_address\n"
           "                        ? katana::runtime::BlockEndKind::Fallthrough\n"
           "                        : katana::runtime::BlockEndKind::ConditionalBranch;\n"
           "                const auto fastpath_source = finalize_product_fastpath(\n"
           "                    fastpath_retired_before,\n"
           "                    fastpath_exception_generation_before,\n"
           "                    {cpu.active_instruction_pc,\n"
           "                     cpu.active_instruction_physical_pc},\n"
           "                    counted_fastpath_kind);\n"
           "                target = cpu.pc;\n"
           "                dispatch_callsite = fastpath_source.virtual_address;\n"
           "                dispatch_source = fastpath_source;\n"
           "                kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "                dispatch_class =\n"
           "                    katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "                dispatch_origin =\n"
           "                    katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "                diagnostic = false;\n"
           "                pending_escape_reason = "
           "StaticAotEscapeReason::ProductFastpath;\n"
           "                pending_escape_site_id = selected_escape_site_id;\n"
           "                pending_escape_address = selected_block.virtual_start;\n"
           "                continue;\n"
           "            }\n"
           "            break;\n"
           "        }\n"
           "        case katana::runtime::RuntimeBlockFastpathKind::None:\n"
           "            break;\n"
           "        }\n"
           "        const auto retired_before = cpu.retired_guest_instructions;\n"
           "        const auto exception_generation_before = cpu.exception_generation;\n"
           "        active_exit_source = {selected.diagnostic_target, selected.physical_target};\n"
           "        active_exit_site_class = "
           "katana::runtime::DynamicDispatchSiteClass::NotDynamic;\n"
           "        auto exit = katana::runtime::execute_runtime_block(\n"
           "            selected_block, cpu, *active_context);\n"
           "        if (cpu.retired_guest_instructions < retired_before)\n"
           "            throw std::runtime_error(\"Gastinstruktionszaehler lief rueckwaerts.\");\n"
           "        const auto retired = cpu.retired_guest_instructions - retired_before;\n"
           "        const bool new_exception =\n"
           "            cpu.exception_generation != exception_generation_before;\n"
           "        if (new_exception) exit.kind = katana::runtime::BlockEndKind::Exception;\n"
           "        const auto expected_fallthrough = static_cast<std::uint32_t>(\n"
           "            exit.source.virtual_address + 2u);\n"
           "        if (retired != 0u &&\n"
           "            exit.kind == katana::runtime::BlockEndKind::Fallthrough &&\n"
           "            (!exit.target.has_value() ||\n"
           "             exit.target->virtual_address != expected_fallthrough))\n"
           "            throw std::runtime_error(\n"
           "                \"Generierter Fallthrough verfehlte den exakten Gast-PC: source=\" +\n"
           "                std::to_string(exit.source.virtual_address) + \" expected=\" +\n"
           "                std::to_string(expected_fallthrough) + \" actual=\" +\n"
           "                std::to_string(cpu.pc));\n"
           "        const bool deferrable_exit =\n"
           "            exit.kind == katana::runtime::BlockEndKind::Fallthrough ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::StaticBranch ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::ConditionalBranch ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::DynamicBranch;\n"
           "        katana::runtime::PlatformBlockCompletion completion;\n"
           "        if (!new_exception && deferrable_exit &&\n"
           "            active_services->can_defer_guest_block_completion()) {\n"
           "            active_services->observe_guest_block_completion(\n"
           "                selected.diagnostic_target, retired, false, false);\n"
           "            completion.scheduler.guest_cycle = active_services->scheduler_cycle();\n"
           "        } else {\n"
           "            completion = katana::runtime::finalize_guest_block(\n"
           "                cpu, *active_services, active_context->scheduler_event_budget,\n"
           "                selected.diagnostic_target, retired, new_exception,\n"
           "                exit.kind == katana::runtime::BlockEndKind::Exception);\n"
           "        }\n"
           "        active_context->scheduler_cycle = completion.scheduler.guest_cycle;\n"
           "        if (exit.kind == katana::runtime::BlockEndKind::Exception)\n"
           "            active_observations->observe_guest_exception(cpu.last_exception_cause);\n"
           "        if (completion.interrupt.has_value())\n"
           "            exit.kind = katana::runtime::BlockEndKind::InterruptSafepoint;\n"
           "        if (exit.kind == katana::runtime::BlockEndKind::Return) {\n"
           "            if (boundary == DispatchChainBoundary::NestedCall ||\n"
           "                cpu.pc == program_return_sentinel)\n"
           "                return;\n"
           "            // RTS latches PR before its delay slot. Follow the resulting guest PC;\n"
           "            // only a nested host-call boundary or the root sentinel ends a chain.\n"
           "            target = cpu.pc;\n"
           "            dispatch_callsite = exit.source.virtual_address;\n"
           "            dispatch_source = exit.source;\n"
           "            kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "            dispatch_class = "
           "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "            dispatch_origin = "
           "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "            diagnostic = false;\n"
           "            pending_escape_reason = "
           "StaticAotEscapeReason::ReturnBoundary;\n"
           "            pending_escape_site_id = selected_escape_site_id;\n"
           "            pending_escape_address = exit.source.virtual_address;\n"
           "            continue;\n"
           "        }\n"
           "        if (exit.kind == katana::runtime::BlockEndKind::Exception ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::ExceptionReturn ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::InterruptSafepoint ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::Sleep) {\n"
           "            target = cpu.pc;\n"
           "            dispatch_callsite = exit.source.virtual_address;\n"
           "            dispatch_source = exit.source;\n"
           "            kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "            dispatch_class = "
           "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "            dispatch_origin = "
           "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "            diagnostic = false;\n"
           "            pending_escape_reason =\n"
           "                exit.kind == katana::runtime::BlockEndKind::InterruptSafepoint\n"
           "                    ? StaticAotEscapeReason::InterruptAcceptable\n"
           "                    : exit.kind == katana::runtime::BlockEndKind::Sleep\n"
           "                        ? StaticAotEscapeReason::Sleep\n"
           "                        : StaticAotEscapeReason::"
           "MmioOrArchitectureBoundary;\n"
           "            pending_escape_site_id = selected_escape_site_id;\n"
           "            pending_escape_address = exit.source.virtual_address;\n"
           "            continue;\n"
           "        }\n"
           "        const auto continuation =\n"
           "            katana::runtime::make_indirect_dispatch_continuation(\n"
           "                exit, active_exit_site_class);\n"
           "        target = cpu.pc;\n"
           "        kind = continuation.kind;\n"
           "        dispatch_callsite = continuation.callsite;\n"
           "        dispatch_source = continuation.source;\n"
           "        dispatch_origin = continuation.resolution_origin;\n"
           "        dispatch_class = continuation.dispatch_class;\n"
           "        diagnostic = continuation.record_diagnostics;\n"
           "        const auto chain_rejection =\n"
           "            active_services->last_executable_chain_rejection();\n"
           "        if (chain_rejection !=\n"
           "                katana::runtime::ExecutableChainRejectionReason::None) {\n"
           "            pending_escape_reason = classify_chain_rejection(chain_rejection);\n"
           "        } else if (active_exit_site_class !=\n"
           "                       katana::runtime::DynamicDispatchSiteClass::NotDynamic) {\n"
           "            pending_escape_reason = continuation.kind ==\n"
           "                    katana::runtime::IndirectDispatchKind::Call\n"
           "                ? StaticAotEscapeReason::CallUnknown\n"
           "                : StaticAotEscapeReason::DynamicTarget;\n"
           "        } else if (exit.kind == katana::runtime::BlockEndKind::Call) {\n"
           "            pending_escape_reason = StaticAotEscapeReason::NativeCallDepth;\n"
           "        } else if (exit.kind == "
           "katana::runtime::BlockEndKind::DynamicBranch) {\n"
           "            pending_escape_reason = StaticAotEscapeReason::DynamicTarget;\n"
           "        } else if (exit.kind == katana::runtime::BlockEndKind::Return) {\n"
           "            pending_escape_reason = StaticAotEscapeReason::ReturnBoundary;\n"
           "        } else {\n"
           "            pending_escape_reason = StaticAotEscapeReason::PartitionOrSymbol;\n"
           "        }\n"
           "        pending_escape_site_id = selected_escape_site_id;\n"
           "        pending_escape_address = exit.source.virtual_address;\n"
           "    }\n"
           "    throw std::runtime_error(\"Runtime-Blockbudget erschoepft.\");\n"
           "}\n"
           "} // namespace\n\n";
    output << "namespace runtime_dispatch_detail {\n"
              "katana::runtime::RuntimeBlockFastpathBinding "
              "static_fastpath_binding(\n"
              "        const std::uint32_t address) noexcept {\n"
              "    switch (address) {\n";
    for (const auto& [address, bindings] : static_fastpath_bindings) {
        // A block can carry exactly one immutable descriptor. In particular,
        // multiple composite callsite/target contracts sharing a target remain
        // conservative instead of choosing a descriptor by array order.
        if (bindings.size() != 1u)
            continue;
        const auto binding = bindings.front();
        output << "    case 0x" << symbol(address) << "u: return {\n";
        switch (binding.kind) {
        case StaticFastpathBindingKind::CompositeCallback:
            output
                << "        katana::runtime::RuntimeBlockFastpathKind::"
                   "CompositeCallback,\n"
                << "        &composite_callback_batch_descriptors["
                << binding.descriptor_index << "u]};\n";
            break;
        case StaticFastpathBindingKind::MemoryFill:
            output
                << "        katana::runtime::RuntimeBlockFastpathKind::"
                   "MemoryFill,\n"
                << "        &memory_fill_loop_batch_descriptors["
                << binding.descriptor_index << "u]};\n";
            break;
        case StaticFastpathBindingKind::MmioWait:
            output
                << "        katana::runtime::RuntimeBlockFastpathKind::"
                   "MmioWait,\n"
                << "        &mmio_wait_loop_batch_descriptors["
                << binding.descriptor_index << "u]};\n";
            break;
        case StaticFastpathBindingKind::CountedLoop:
            output
                << "        katana::runtime::RuntimeBlockFastpathKind::"
                   "CountedLoop,\n"
                << "        &counted_loop_batch_descriptors["
                << binding.descriptor_index << "u]};\n";
            break;
        }
    }
    output << "    default: return {};\n"
              "    }\n"
              "}\n"
              "} // namespace runtime_dispatch_detail\n\n";
    output << "std::uint64_t runtime_central_dispatch_count() noexcept {\n"
           "    return executed_dispatch_blocks;\n"
           "}\n\n"
           "void static_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call,\n"
              "        katana::runtime::RuntimeDispatchClass::GuardedFallback, false,\n"
              "        DispatchChainBoundary::NestedCall);\n"
              "}\n"
              "void resolved_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call,\n"
              "        katana::runtime::RuntimeDispatchClass::GuardedFallback, true,\n"
              "        DispatchChainBoundary::NestedCall);\n"
              "}\n"
              "void guarded_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call,\n"
              "        katana::runtime::RuntimeDispatchClass::GuardedFallback, true,\n"
              "        DispatchChainBoundary::NestedCall);\n"
              "}\n"
              "void guarded_jump(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::TailJump,\n"
              "        katana::runtime::RuntimeDispatchClass::GuardedFallback, true,\n"
              "        DispatchChainBoundary::NestedCall);\n"
              "    tail_dispatch_completed = true;\n"
              "}\n"
              "void runtime_only_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call,\n"
              "        katana::runtime::RuntimeDispatchClass::RuntimeOnly, true,\n"
              "        DispatchChainBoundary::NestedCall);\n"
              "}\n"
              "void runtime_only_jump(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::TailJump,\n"
              "        katana::runtime::RuntimeDispatchClass::RuntimeOnly, true,\n"
              "        DispatchChainBoundary::NestedCall);\n"
              "    tail_dispatch_completed = true;\n"
              "}\n"
              "void unresolved_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    katana::runtime::unresolved_call(cpu, target);\n"
              "}\n"
              "void unresolved_jump(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    katana::runtime::unresolved_jump(cpu, target);\n"
              "}\n\n"
           << "RuntimeRunResult run_runtime(katana::runtime::CpuState& cpu,\n"
           << "                             katana::runtime::PlatformServices& services,\n"
           << "                             katana::runtime::RuntimeBlockTable& table,\n"
           << "                             katana::runtime::SystemReplayObservationSession& "
              "observations,\n"
           << "                             katana::runtime::CrashCapsule& crash_capsule) {\n"
           << "    katana::runtime::validate_platform_services(services);\n";
    if (external_game_project_runtime_bindings) {
        output
            << "    const auto* registered_game_project =\n"
               "        katana::runtime::active_game_project_bindings();\n";
        if (required_game_project_runtime_bindings)
            output
                << "    if (registered_game_project == nullptr)\n"
                   "        throw std::runtime_error(\"Erforderliches externes "
                   "Game-Project ist nicht registriert.\");\n";
        output
            << "    if (registered_game_project != nullptr) {\n"
               "        const auto registered_game_project_identity =\n"
               "            katana::runtime::game_project_definition_identity(\n"
               "                registered_game_project->definition());\n"
               "        if (registered_game_project_identity != "
            << katana::io::quote_json(
                   external_game_project_runtime_identity)
            << " &&\n"
               "            registered_game_project_identity != "
            << katana::io::quote_json(
                   external_game_project_export_identity)
            << ")\n"
               "            throw std::runtime_error(\"Registriertes externes "
               "Game-Project besitzt die falsche Identitaet.\");\n"
               "    }\n";
    } else {
        output << "    const katana::runtime::GameProjectBindings* "
                  "registered_game_project = nullptr;\n";
    }
    output << "    table.bind_code_tracker(\n"
           << "        services.executable_code_tracker(),\n"
           << "        katana::runtime::StaticAotInvalidationContract::Coordinated);\n";
    output << "    std::vector<katana::runtime::RuntimeBlock> static_blocks;\n";
    output << "    static_blocks.reserve(" << emitted_blocks.size() << "u);\n";
    for (std::size_t shard = 0u; shard < shard_count; ++shard)
        output << "    runtime_dispatch_detail::append_static_blocks_shard_" << shard_symbol(shard)
               << "(static_blocks);\n";
    output << "    static_cast<void>(table.register_static_bulk(std::move(static_blocks)));\n";
    for (std::size_t shard = 0u; shard < shard_count; ++shard)
        output << "    runtime_dispatch_detail::register_executable_blocks_shard_"
               << shard_symbol(shard) << "(table, services);\n";
    output << "    katana::runtime::DispatchDiagnosticRecorder diagnostics;\n"
           << "    katana::runtime::IndirectDispatchMetrics dispatch_metrics;\n"
           << "    dispatch_metrics.set_site_details_enabled(\n"
           << "        detailed_dispatch_diagnostics_enabled());\n"
           << "    katana::runtime::BlockExecutionContext context;\n"
           << "    context.scheduler_cycle = services.scheduler_cycle();\n"
           << "    context.scheduler_event_budget = 1024u;\n"
           << "    auto* modules = services.executable_module_catalog();\n"
           << "    if (modules == nullptr)\n"
           << "        throw std::runtime_error(\"Produktpfad besitzt keinen Modul-Catalog.\");\n"
           << "    const std::vector<katana::runtime::NativeAotTemplate> native_aot_templates{\n";
    for (const auto& native_template : native_templates) {
        output << "        {std::string(katana::runtime::";
        if (native_template.module_id ==
            katana::runtime::dreamcast_initial_disc_bootstrap_module_id) {
            output << "dreamcast_initial_disc_bootstrap_module_id";
        } else if (native_template.module_id ==
                   katana::runtime::dreamcast_initial_boot_executable_module_id) {
            output << "dreamcast_initial_boot_executable_module_id";
        } else {
            throw std::logic_error("Unbekannte lokale AOT-Quellmodulkennung.");
        }
        output << "), \"" << native_template.expected_source_identity << "\", 0x"
               << symbol(native_template.source_start) << "u, " << native_template.extent << "u, "
               << native_template.destination_vbr_delta << ", {";
        for (const auto& [offset, targets] : native_template.patch_targets) {
            output << "{" << offset << "u, {";
            for (std::size_t index = 0u; index < targets.size(); ++index) {
                if (index != 0u) output << ',';
                output << "{0x" << symbol(targets[index].live_value) << "u,0x"
                       << symbol(targets[index].block_address) << "u}";
            }
            output << "}},";
        }
        output << "}, katana::runtime::NativeAotTemplateDestination::VbrRelative, {}, {}, {";
        for (const auto range : native_template.mutable_ranges)
            output << "{" << range.offset << "u," << range.size << "u},";
        output << "}},\n";
    }
    for (const auto& module : latent_modules) {
        output << "        {" << katana::io::quote_json(module.id) << ", "
               << katana::io::quote_json(module.byte_identity) << ", 0x"
               << symbol(module.source_address) << "u, " << module.byte_size
               << "u, 0, {}, "
                  "katana::runtime::NativeAotTemplateDestination::LoadedModule, "
               << katana::io::quote_json(expected_content_identity) << ", "
               << katana::io::quote_json(module.byte_identity) << "},\n";
    }
    for (const auto& native_template : external_native_templates) {
        output << "        {"
               << katana::io::quote_json(native_template.source_module_id)
               << ", "
               << katana::io::quote_json(
                      native_template.expected_source_identity)
               << ", 0x" << symbol(native_template.source_start) << "u, "
               << native_template.extent << "u, "
               << native_template.destination_vbr_delta << ", {";
        for (const auto& patch : native_template.patches) {
            output << "{" << patch.source_offset << "u,{";
            for (const auto& target : patch.allowed_targets)
                output << "{0x" << symbol(target.live_value) << "u,0x"
                       << symbol(target.block_address) << "u},";
            output << "}},";
        }
        output << "}, katana::runtime::NativeAotTemplateDestination::"
               << (native_template.destination ==
                           katana::runtime::NativeAotTemplateDestination::
                               VbrRelative
                       ? "VbrRelative"
                       : "LoadedModule")
               << ", "
               << katana::io::quote_json(
                      native_template.expected_runtime_content_identity)
               << ", "
               << katana::io::quote_json(
                      native_template.expected_runtime_byte_identity)
               << ", {";
        for (const auto& range : native_template.mutable_ranges)
            output << "{" << range.offset << "u," << range.size << "u},";
        output << "}},\n";
    }
    output << "    };\n"
           << "    katana::runtime::NativeAotTemplateBinder native_aot_binder(\n"
           << "        cpu, *modules, table, native_aot_templates);\n"
           << "    katana::runtime::BlockMaterializationPolicy materialization_policy;\n"
           << "    materialization_policy.max_blocks = 65536u;\n"
           << "    materialization_policy.max_bytes = 64u * 1024u * 1024u;\n"
           << "    materialization_policy.max_materializations_per_run = 65536u;\n"
           << "    if (const auto* probe = std::getenv(\"KATANA_RUNTIME_PROBE\");\n"
              "        probe != nullptr && std::string_view(probe) == \"deterministic-v1\")\n"
              "        materialization_policy.deterministic_no_host_time = true;\n";
    if (diagnostic_interpreter) {
        output
            << "    materialization_policy.enabled = true;\n"
            << "    katana::runtime::DemandBlockMaterializer materializer(\n"
            << "        *modules, table, services.executable_code_tracker(), "
               "materialization_policy,\n"
            << "        [&native_aot_binder](const std::uint32_t target,\n"
            << "           const std::uint32_t physical_origin,\n"
            << "           const std::span<const std::uint8_t> bytes,\n"
            << "           const katana::runtime::BlockVariantKey& variant) {\n"
            << "            auto native = native_aot_binder.bind(\n"
            << "                target, physical_origin, bytes, variant);\n"
            << "            if (native) return std::move(native.candidate);\n"
            << "            katana::runtime::MaterializedBlockCandidate candidate;\n"
            << "            if (bytes.size() < 2u) return candidate;\n"
            << "            const auto opcode = static_cast<std::uint16_t>(bytes[0]) |\n"
            << "                static_cast<std::uint16_t>(bytes[1] << 8u);\n"
            << "            const auto decoded = katana::sh4::decode(opcode);\n"
            << "            if (!decoded.is_known()) return candidate;\n"
            << "            std::size_t size = 2u;\n"
            << "            candidate.instructions = 1u;\n"
            << "            if (decoded.has_delay_slot) {\n"
            << "                if (bytes.size() < 4u) return candidate;\n"
            << "                const auto slot_opcode = static_cast<std::uint16_t>(bytes[2]) |\n"
            << "                    static_cast<std::uint16_t>(bytes[3] << 8u);\n"
            << "                const auto slot = katana::sh4::decode(slot_opcode);\n"
            << "                if (!slot.is_known() || slot.changes_control_flow())\n"
            << "                    return candidate;\n"
            << "                size = 4u; ++candidate.instructions;\n"
            << "            }\n"
            << "            candidate.block = {target,\n"
            << "                katana::runtime::canonical_physical_address(target),\n"
            << "                static_cast<std::uint32_t>(size),\n"
            << "                katana::runtime::BlockEndKind::DynamicBranch, variant,\n"
            << "                &dispatch_dynamic_interpreter, \"runtime-sh4-interpreter\"};\n"
            << "            candidate.decode_candidate_validated = true;\n"
            << "            candidate.interpreter_backed = true;\n"
            << "            candidate.bounded_analysis_complete = false;\n"
            << "            candidate.ir_verified = false;\n"
            << "            candidate.code_generated = false;\n"
            << "            candidate.guest_cycles = candidate.instructions;\n"
            << "            return candidate;\n"
            << "        });\n";
    } else {
        output << "    // Product ports execute only statically generated native/AOT blocks.\n"
               << "    // Runtime copies bind only to analysis-proven, pre-generated native code.\n"
               << "    materialization_policy.enabled = "
               << (native_templates.empty() && latent_modules.empty() ? "false" : "true") << ";\n"
               << "    katana::runtime::DemandBlockMaterializer materializer(\n"
               << "        *modules, table, services.executable_code_tracker(),\n"
               << "        materialization_policy,\n"
               << "        [&native_aot_binder](const std::uint32_t target,\n"
               << "           const std::uint32_t physical_origin,\n"
               << "           const std::span<const std::uint8_t> bytes,\n"
               << "           const katana::runtime::BlockVariantKey& variant) {\n"
               << "            return std::move(\n"
               << "                native_aot_binder.bind(\n"
               << "                    target, physical_origin, bytes, variant).candidate);\n"
               << "        });\n";
    }
    output << "    last_materialization_status = {};\n"
           << "    const auto capture_materialization_status = [&] {\n"
           << "        materializer.reconcile_inactive_origins(&dispatch_metrics);\n"
           << "        const auto& metrics = materializer.metrics();\n"
           << "        last_materialization_status = {metrics.requests, metrics.cache_hits,\n"
           << "            metrics.materializations, metrics.interpreter_materializations,\n"
           << "            metrics.misses, metrics.budget_failures,\n"
           << "            metrics.retained_validation_bytes,\n"
           << "            metrics.peak_retained_validation_bytes,\n"
           << "            metrics.reclaimed_validation_bytes,\n"
           << "            static_cast<std::uint32_t>(metrics.first_failure),\n"
           << "            metrics.first_failure_target};\n"
           << "    };\n"
           << "    ServiceScope scope(\n"
           << "        services, table, context, diagnostics, dispatch_metrics,\n"
           << "        crash_capsule, observations, materializer, registered_game_project);\n"
           << "    bool guest_cycle_budget_reached = false;\n"
           << "    try {\n"
           << "        if (cpu.pc == 0u || (cpu.pc & 1u) != 0u)\n"
           << "            throw std::runtime_error(\"runtime-entry-pc-invalid\");\n"
           << "        dispatch_chain(cpu, cpu.pc,\n"
           << "        katana::runtime::IndirectDispatchKind::TailJump,\n"
           << "        katana::runtime::RuntimeDispatchClass::GuardedFallback, false,\n"
           << "        DispatchChainBoundary::ProgramRoot);\n"
           << "    } catch (const katana::runtime::PlatformShutdownRequested&) {\n"
           << "        // A host close request is a controlled end of native guest dispatch.\n"
           << "    } catch (const katana::runtime::RuntimeProbeBudgetReached&) {\n"
           << "        capture_materialization_status();\n"
           << "        throw;\n"
           << "    } catch (const katana::runtime::GuestCycleBudgetReached& reached) {\n"
           << "        if (reached.final_guest_cycle() != services.scheduler_cycle())\n"
           << "            throw std::runtime_error(\"guest-cycle-budget-cycle-mismatch\");\n"
           << "        guest_cycle_budget_reached = true;\n"
           << "    } catch (...) {\n"
           << "        capture_materialization_status();\n"
           << "        crash_capsule.note_first_error(0xFFFFFFFFu, cpu.pc, cpu.pc);\n"
           << "        std::cerr << \"KATANA_CRASH_CAPSULE version=\"\n"
           << "                  << katana::runtime::crash_capsule_contract_version\n"
           << "                  << \" last_pc=\" << crash_capsule.last_pc\n"
           << "                  << \" last_block=\" << crash_capsule.last_block\n"
           << "                  << \" last_mmio_address=\"\n"
           << "                  << crash_capsule.last_mmio_address\n"
           << "                  << \" last_mmio_value=\" << crash_capsule.last_mmio_value\n"
           << "                  << \" last_scheduler_cycle=\"\n"
           << "                  << crash_capsule.last_scheduler_cycle\n"
           << "                  << \" last_scheduler_event_id=\"\n"
           << "                  << crash_capsule.last_scheduler_event_id\n"
           << "                  << \" last_scheduler_event_kind=\"\n"
           << "                  << crash_capsule.last_scheduler_event_kind\n"
           << "                  << \" first_error=\" << crash_capsule.first_error_code\n"
           << "                  << \" first_error_pc=\" << crash_capsule.first_error_pc\n"
           << "                  << \" first_error_target=\"\n"
           << "                  << crash_capsule.first_error_target\n"
           << "                  << \" ring_events=\" << crash_capsule.event_count << '\\n';\n"
           << "        if (const auto* value = std::getenv(\"KATANA_PORT_DIAGNOSTICS\");\n"
           << "            value != nullptr && std::string_view(value) == \"1\")\n"
           << "            std::cerr << \"KATANA_RUNTIME_DISPATCH_DIAGNOSTICS \"\n"
           << "                      << diagnostics.serialize_hotspots_json() << '\\n';\n"
           << "        if (const auto* value = std::getenv(\"KATANA_PORT_DIAGNOSTICS_FULL\");\n"
           << "            value != nullptr && std::string_view(value) == \"1\")\n"
           << "            std::cerr << \"KATANA_RUNTIME_DISPATCH_EVENTS \"\n"
           << "                      << diagnostics.serialize_json() << '\\n';\n"
           << "        throw;\n"
           << "    }\n"
           << "    capture_materialization_status();\n"
           << "    std::array<StaticAotEscapeSiteCounter, 16u> "
              "static_aot_escape_top_sites{};\n"
           << "    std::uint32_t static_aot_escape_top_site_count = 0u;\n"
           << "    const auto insert_static_aot_escape_site = [\n"
              "            &](const StaticAotEscapeSiteCounter candidate) {\n"
           << "        if (candidate.count == 0u) return;\n"
           << "        auto count = static_cast<std::size_t>(\n"
              "            static_aot_escape_top_site_count);\n"
           << "        if (count < static_aot_escape_top_sites.size()) {\n"
           << "            static_aot_escape_top_sites[count] = candidate;\n"
           << "            ++static_aot_escape_top_site_count;\n"
           << "            ++count;\n"
           << "        } else {\n"
           << "            const auto& last = static_aot_escape_top_sites.back();\n"
           << "            if (candidate.count < last.count ||\n"
           << "                (candidate.count == last.count &&\n"
           << "                 candidate.site_id >= last.site_id)) return;\n"
           << "            static_aot_escape_top_sites.back() = candidate;\n"
           << "        }\n"
           << "        auto index = std::min<std::size_t>(\n"
              "            count, static_aot_escape_top_sites.size()) - 1u;\n"
           << "        while (index != 0u) {\n"
           << "            const auto& current = static_aot_escape_top_sites[index];\n"
           << "            const auto& previous = static_aot_escape_top_sites[index - 1u];\n"
           << "            if (current.count < previous.count ||\n"
           << "                (current.count == previous.count &&\n"
           << "                 current.site_id >= previous.site_id)) break;\n"
           << "            std::swap(static_aot_escape_top_sites[index],\n"
              "                      static_aot_escape_top_sites[index - 1u]);\n"
           << "            --index;\n"
           << "        }\n"
           << "    };\n"
           << "    for (std::size_t site = 0u;\n"
              "         site < static_aot_escape_site_counts.size(); ++site) {\n"
           << "        for (std::size_t reason = 0u;\n"
              "             reason < static_aot_escape_reason_count; ++reason) {\n"
           << "            insert_static_aot_escape_site({\n"
           << "                static_cast<std::uint64_t>(site),\n"
           << "                static_aot_escape_site_addresses[site],\n"
           << "                static_cast<StaticAotEscapeReason>(reason),\n"
           << "                static_aot_escape_site_counts[site][reason]});\n"
           << "        }\n"
           << "    }\n"
           << "    const auto first_error = dispatch_metrics.first_error();\n"
           << "    return {diagnostics.total_occurrences(), dispatch_metrics.hits(),\n"
           << "        dispatch_metrics.misses(), dispatch_metrics.fallbacks(),\n"
           << "        dispatch_metrics.runtime_only_hits(),\n"
           << "        dispatch_metrics.runtime_only_misses(),\n"
           << "        dispatch_metrics.runtime_only_fallbacks(),\n"
           << "        dispatch_metrics.runtime_only_site_count(),\n"
           << "        dispatch_metrics.runtime_only_dispatch_share_ppm(),\n"
           << "        dispatch_metrics.serialize_json(true),\n"
           << "        static_cast<std::uint32_t>(first_error ? first_error->error\n"
           << "            : katana::runtime::DispatchDiagnosticError::None),\n"
           << "        cpu.pc, services.scheduler_cycle(), executed_dispatch_blocks,\n"
           << "        guest_cycle_budget_reached,\n"
           << "        katana::runtime::guest_cycle_contract_version,\n"
           << "        static_aot_escape_counts,\n"
           << "        static_aot_escape_top_sites,\n"
           << "        static_aot_escape_top_site_count,\n"
           << "        static_aot_classified_dispatches};\n"
           << "}\n"
           << "} // namespace " << entry_namespace << "\n";
    result.push_back({"code/runtime-dispatch.cpp", output.str()});
    return result;
}

std::string root_cmake(const bool diagnostic_partial) {
    return "cmake_minimum_required(VERSION 3.25)\n"
           "project(KatanaPort LANGUAGES CXX)\n"
           "set(KATANA_RUNTIME_ROOT \"\" CACHE PATH \"KatanaRecomp source root\")\n"
           "set(KATANA_RUNTIME_PREFIX \"\" CACHE PATH "
           "\"Installed KatanaRecomp runtime package prefix\")\n"
           "set(KATANA_PORT_DIAGNOSTIC_RUNTIME " +
           std::string(diagnostic_partial ? "ON" : "OFF") +
           ")\n"
           "set(KATANA_PORT_BUILD_PROFILE \"bringup\" CACHE STRING "
           "\"Port build profile: bringup or gate\")\n"
           "set_property(CACHE KATANA_PORT_BUILD_PROFILE PROPERTY STRINGS bringup gate)\n"
           "if(NOT KATANA_PORT_BUILD_PROFILE STREQUAL \"bringup\" AND\n"
           "   NOT KATANA_PORT_BUILD_PROFILE STREQUAL \"gate\")\n"
           "  message(FATAL_ERROR \"KATANA_PORT_BUILD_PROFILE must be bringup or gate\")\n"
           "endif()\n"
           "if(KATANA_RUNTIME_ROOT STREQUAL \"\" AND NOT \"$ENV{KATANA_RUNTIME_ROOT}\" "
              "STREQUAL \"\")\n"
           "  file(TO_CMAKE_PATH \"$ENV{KATANA_RUNTIME_ROOT}\" KATANA_RUNTIME_ROOT)\n"
           "endif()\n"
           "if(KATANA_RUNTIME_PREFIX STREQUAL \"\" AND "
           "NOT \"$ENV{KATANA_RUNTIME_PREFIX}\" STREQUAL \"\")\n"
           "  file(TO_CMAKE_PATH \"$ENV{KATANA_RUNTIME_PREFIX}\" "
           "KATANA_RUNTIME_PREFIX)\n"
           "endif()\n"
           "if(NOT \"${CMAKE_LINKER_TYPE}\" STREQUAL \"\" AND "
           "CMAKE_VERSION VERSION_LESS 3.29)\n"
           "  message(FATAL_ERROR \"CMAKE_LINKER_TYPE requires CMake 3.29 or newer\")\n"
           "endif()\n"
           "if(MSVC)\n"
           "  if(KATANA_PORT_BUILD_PROFILE STREQUAL \"bringup\")\n"
           "    if(CMAKE_LINKER_TYPE STREQUAL \"LLD\")\n"
           "      add_link_options(\"$<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>\")\n"
           "    else()\n"
           "      add_link_options(\"$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL>\"\n"
           "                       \"$<$<CONFIG:RelWithDebInfo>:/DEBUG:FASTLINK>\")\n"
           "    endif()\n"
           "  else()\n"
           "    if(CMAKE_LINKER_TYPE STREQUAL \"LLD\")\n"
           "      add_link_options(\"$<$<CONFIG:RelWithDebInfo>:/OPT:REF>\"\n"
           "                       \"$<$<CONFIG:RelWithDebInfo>:/OPT:ICF>\")\n"
           "    else()\n"
           "      add_link_options(\"$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>\"\n"
           "                       \"$<$<CONFIG:RelWithDebInfo>:/OPT:REF>\"\n"
           "                       \"$<$<CONFIG:RelWithDebInfo>:/OPT:ICF>\")\n"
           "    endif()\n"
           "  endif()\n"
           "endif()\n"
           "if(KATANA_PORT_DIAGNOSTIC_RUNTIME)\n"
           "  set(KATANA_PORT_NAMESPACED_RUNTIME_TARGET KatanaRecomp::runtime)\n"
           "  set(KATANA_PORT_SOURCE_RUNTIME_TARGET katana_runtime)\n"
           "else()\n"
           "  set(KATANA_PORT_NAMESPACED_RUNTIME_TARGET KatanaRecomp::runtime_core)\n"
           "  set(KATANA_PORT_SOURCE_RUNTIME_TARGET katana_runtime_core)\n"
           "endif()\n"
           "if(NOT KATANA_RUNTIME_PREFIX STREQUAL \"\")\n"
           "  find_package(KatanaRecomp CONFIG REQUIRED "
           "PATHS \"${KATANA_RUNTIME_PREFIX}\" NO_DEFAULT_PATH)\n"
           "  if(TARGET \"${KATANA_PORT_NAMESPACED_RUNTIME_TARGET}\")\n"
           "    set(KATANA_PORT_RUNTIME_TARGET \"${KATANA_PORT_NAMESPACED_RUNTIME_TARGET}\")\n"
           "  else()\n"
           "    message(FATAL_ERROR \"Installed KatanaRecomp package has no requested "
           "runtime target\")\n"
           "  endif()\n"
           "elseif(NOT KATANA_RUNTIME_ROOT STREQUAL \"\")\n"
           "  add_subdirectory(\"${KATANA_RUNTIME_ROOT}\" "
           "\"${CMAKE_BINARY_DIR}/katana-runtime\" EXCLUDE_FROM_ALL)\n"
           "  if(TARGET \"${KATANA_PORT_SOURCE_RUNTIME_TARGET}\")\n"
           "    set(KATANA_PORT_RUNTIME_TARGET \"${KATANA_PORT_SOURCE_RUNTIME_TARGET}\")\n"
           "  elseif(TARGET \"${KATANA_PORT_NAMESPACED_RUNTIME_TARGET}\")\n"
           "    set(KATANA_PORT_RUNTIME_TARGET \"${KATANA_PORT_NAMESPACED_RUNTIME_TARGET}\")\n"
           "  else()\n"
           "    message(FATAL_ERROR \"KatanaRecomp source fallback has no requested runtime "
           "target\")\n"
           "  endif()\n"
           "elseif(TARGET \"${KATANA_PORT_NAMESPACED_RUNTIME_TARGET}\")\n"
           "  set(KATANA_PORT_RUNTIME_TARGET \"${KATANA_PORT_NAMESPACED_RUNTIME_TARGET}\")\n"
           "elseif(TARGET \"${KATANA_PORT_SOURCE_RUNTIME_TARGET}\")\n"
           "  set(KATANA_PORT_RUNTIME_TARGET \"${KATANA_PORT_SOURCE_RUNTIME_TARGET}\")\n"
           "else()\n"
           "  find_package(KatanaRecomp CONFIG QUIET)\n"
           "  if(TARGET \"${KATANA_PORT_NAMESPACED_RUNTIME_TARGET}\")\n"
           "    set(KATANA_PORT_RUNTIME_TARGET \"${KATANA_PORT_NAMESPACED_RUNTIME_TARGET}\")\n"
           "  elseif(TARGET \"${KATANA_PORT_SOURCE_RUNTIME_TARGET}\")\n"
           "    set(KATANA_PORT_RUNTIME_TARGET \"${KATANA_PORT_SOURCE_RUNTIME_TARGET}\")\n"
           "  else()\n"
           "    message(FATAL_ERROR \"Find KatanaRecomp or set KATANA_RUNTIME_ROOT to the "
           "compatible source tree\")\n"
           "  endif()\n"
           "endif()\n"
           "add_subdirectory(generated)\n"
           "include(\"${CMAKE_CURRENT_SOURCE_DIR}/generated/katana-port.cmake\")\n";
}

std::string product_gate_runner() {
    return "param(\n"
           "    [Parameter(Mandatory = $true)][string]$Executable,\n"
           "    [ValidateRange(1, 3600)][int]$WatchdogSeconds = 120\n"
           ")\n"
           "$ErrorActionPreference = 'Stop'\n"
           "$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path\n"
           "$startInfo = [System.Diagnostics.ProcessStartInfo]::new()\n"
           "$startInfo.FileName = $resolvedExecutable\n"
           "$startInfo.UseShellExecute = $false\n"
           "$startInfo.CreateNoWindow = $true\n"
           "$startInfo.RedirectStandardOutput = $true\n"
           "$startInfo.RedirectStandardError = $true\n"
           "$startInfo.EnvironmentVariables['KATANA_GUEST_CYCLE_BUDGET'] = "
           "'600000000'\n"
           "$process = [System.Diagnostics.Process]::Start($startInfo)\n"
           "$standardOutput = $process.StandardOutput.ReadToEndAsync()\n"
           "$standardError = $process.StandardError.ReadToEndAsync()\n"
           "$timedOut = -not $process.WaitForExit($WatchdogSeconds * 1000)\n"
           "if ($timedOut) {\n"
           "    try {\n"
           "        if (-not $process.HasExited) { $process.Kill() }\n"
           "    } catch [System.InvalidOperationException] {\n"
           "        # Das Kind endete zwischen WaitForExit und Kill.\n"
           "    }\n"
           "    $process.WaitForExit()\n"
           "}\n"
           "[Console]::Out.Write($standardOutput.GetAwaiter().GetResult())\n"
           "[Console]::Error.Write($standardError.GetAwaiter().GetResult())\n"
           "if ($timedOut) {\n"
           "    [Console]::Error.WriteLine("
           "'KATANA_PRODUCT_GATE status=host-watchdog-hang')\n"
           "    exit 124\n"
           "}\n"
           "exit $process.ExitCode\n";
}

std::string port_cmake(const std::string& target_name) {
    return "add_executable(" + target_name +
           " \"${CMAKE_CURRENT_LIST_DIR}/../src/main.cpp\")\n"
           "target_compile_features(" +
           target_name +
           " PRIVATE cxx_std_20)\n"
           "option(KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST "
           "\"Enable internal generated-port counted-loop differential hooks\" OFF)\n"
           "if(KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST)\n"
           "  target_compile_definitions(" +
           target_name +
           " PRIVATE KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST=1)\n"
           "endif()\n"
           "option(KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST "
           "\"Enable internal generated-port composite-callback differential hooks\" OFF)\n"
           "if(KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST)\n"
           "  target_compile_definitions(" +
           target_name +
           " PRIVATE KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST=1)\n"
           "endif()\n"
           "option(KATANA_INTERNAL_MEMORY_FILL_DIFFERENTIAL_TEST "
           "\"Enable internal generated-port memory-fill differential hooks\" OFF)\n"
           "if(KATANA_INTERNAL_MEMORY_FILL_DIFFERENTIAL_TEST)\n"
           "  target_compile_definitions(" +
           target_name +
           " PRIVATE KATANA_INTERNAL_MEMORY_FILL_DIFFERENTIAL_TEST=1)\n"
           "endif()\n"
           "option(KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST "
           "\"Enable internal generated-port atomic batch lifecycle hook\" OFF)\n"
           "if(KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST)\n"
           "  target_compile_definitions(" +
           target_name +
           " PRIVATE KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST=1)\n"
           "endif()\n"
           "target_include_directories(" +
           target_name +
           " PRIVATE \"${CMAKE_CURRENT_LIST_DIR}/include\")\n"
           "target_link_libraries(" +
           target_name + " PRIVATE katana_generated ${KATANA_PORT_RUNTIME_TARGET})\n"
           "if(KATANA_PORT_BUILD_PROFILE STREQUAL \"gate\")\n"
           "  include(CheckIPOSupported)\n"
           "  check_ipo_supported(RESULT KATANA_PORT_IPO_SUPPORTED "
           "OUTPUT KATANA_PORT_IPO_ERROR)\n"
           "  if(NOT KATANA_PORT_IPO_SUPPORTED)\n"
           "    message(FATAL_ERROR \"Gate build requires IPO: ${KATANA_PORT_IPO_ERROR}\")\n"
           "  endif()\n"
           "  set_property(TARGET katana_generated PROPERTY "
           "INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)\n"
           "  set_property(TARGET " +
           target_name +
           " PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)\n"
           "endif()\n";
}

std::string
port_metadata(const PortExportOptions& options,
              const std::size_t function_count,
              const std::span<const TranslationUnitPartition> partitions,
              const std::uint32_t entry_address,
              const std::size_t boot_size,
              const bool direct_boot_executable,
              const std::string_view project_identity,
              const katana::analysis::ControlFlowAnalysisResult& analysis,
              const std::size_t latent_aot_module_count) {
    const auto& indirect = analysis.indirect_control_flow;
    const auto count = [&indirect](const auto status) {
        return std::count_if(indirect.begin(), indirect.end(), [status](const auto& resolution) {
            return katana::analysis::control_flow_report_status(resolution) == status;
        });
    };
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-port-project", "port-project");
    output << ",\"contract_version\":" << port_project_contract_version
           << ",\"target_name\":" << katana::io::quote_json(options.target_name)
           << ",\"console_profile\":" << katana::io::quote_json(options.console_profile)
           << ",\"diagnostic_partial\":" << (options.diagnostic_partial ? "true" : "false")
           << ",\"execution_profile\":"
           << katana::io::quote_json(options.diagnostic_partial ? "diagnostic-interpreter"
                                                                : "native-aot-product")
           << ",\"runtime_interpreter_enabled\":" << (options.diagnostic_partial ? "true" : "false")
           << ",\"unbound_code_policy\":"
           << katana::io::quote_json(options.diagnostic_partial ? "diagnostic-interpreter"
                                                                : "typed-materialization-error")
           << ",\"runtime_abi\":" << katana::runtime::abi_version
           << ",\"backend_abi\":" << backend_interface_abi_version
           << ",\"execution_coverage_contract\":"
           << katana::io::quote_json(options.diagnostic_partial ? "diagnostic-validated-demand-v1"
                                                                : "native-aot-or-typed-abort-v1")
           << ",\"dispatch_paths_without_validation\":0"
           << ",\"project_identity\":" << katana::io::quote_json(project_identity)
           << ",\"entry_address\":" << entry_address << ",\"boot_size\":" << boot_size
           << ",\"boot_path\":"
           << katana::io::quote_json(direct_boot_executable
                                         ? "direct-boot-executable"
                                         : "native-disc-boot")
           << ",\"function_count\":" << function_count
           << ",\"latent_aot_modules\":" << latent_aot_module_count << ",\"resolved_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::Resolved)
           << ",\"guarded_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::GuardedComplete) +
                  count(katana::analysis::ControlFlowReportStatus::GuardedPartial)
           << ",\"guarded_complete_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::GuardedComplete)
           << ",\"guarded_partial_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::GuardedPartial)
           << ",\"runtime_only_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::RuntimeOnly)
           << ",\"unresolved_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::Unresolved)
           << ",\"function_summary_iterations\":"
           << analysis.function_summary_iterations
           << ",\"function_iteration_budget\":"
           << analysis.function_iteration_budget
           << ",\"function_budget_exhausted\":"
           << (analysis.function_budget_exhausted ? "true" : "false")
           << ",\"raw_stored_code_inventory_candidates\":"
           << analysis.raw_stored_code_inventory_candidates
           << ",\"raw_stored_code_inventory_budget\":"
           << analysis.raw_stored_code_inventory_budget
           << ",\"raw_stored_code_inventory_truncated\":"
           << (analysis.raw_stored_code_inventory_truncated ? "true"
                                                            : "false")
           << ",\"guarded_code_inventory_candidates\":"
           << analysis.guarded_code_inventory_candidates
           << ",\"guarded_code_inventory_budget\":"
           << analysis.guarded_code_inventory_budget
           << ",\"guarded_code_shape_validation_work\":"
           << analysis.guarded_code_shape_validation_work
           << ",\"guarded_code_shape_validation_work_budget\":"
           << analysis.guarded_code_shape_validation_work_budget
           << ",\"guarded_code_shape_budget_exceeded_candidates\":"
           << analysis.guarded_code_shape_budget_exceeded_candidates
           << ",\"candidate_inventory_truncated\":"
           << (analysis.candidate_inventory_truncated ? "true" : "false")
           << ",\"returned_table_scan_truncated\":"
           << (analysis.returned_table_scan_truncated ? "true" : "false")
           << ",\"partitions\":[";
    for (std::size_t index = 0u; index < partitions.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& partition = partitions[index];
        output << "{\"index\":" << partition.index
               << ",\"functions\":" << partition.function_indices.size()
               << ",\"instructions\":" << partition.instruction_count
               << ",\"first_entry\":" << partition.first_entry_address
               << ",\"last_entry\":" << partition.last_entry_address << '}';
    }
    output << "]}";
    return output.str();
}

void write_port_file(const std::filesystem::path& root,
                     const std::filesystem::path& relative,
                     const std::string_view content,
                     const bool replace_existing = false) {
    auto candidate = root;
    for (const auto& component : relative) {
        candidate /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::runtime_error("Port-Bootstrappfad enthaelt einen symbolischen Link.");
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("Port-Bootstrappfad konnte nicht geprueft werden.");
        }
    }
    const auto path = root / relative;
    if (std::filesystem::exists(path) && !replace_existing) return;
    std::filesystem::create_directories(path.parent_path());
    if (std::filesystem::is_regular_file(path) &&
        std::filesystem::file_size(path) == content.size()) {
        std::ifstream existing(path, std::ios::binary);
        std::string current(content.size(), '\0');
        existing.read(current.data(), static_cast<std::streamsize>(current.size()));
        if (existing && current == content) return;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw katana::io::InputOutputError("Port-Bootstrapdatei konnte nicht geoeffnet werden.");
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output)
        throw katana::io::InputOutputError("Port-Bootstrapdatei konnte nicht geschrieben werden.");
}

bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::filesystem::path resolve_existing_parents(std::filesystem::path path) {
    std::vector<std::filesystem::path> missing;
    std::error_code error;
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        if (error) throw std::runtime_error("Port-Ausgabepfad konnte nicht geprueft werden.");
        missing.push_back(path.filename());
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    auto resolved = std::filesystem::canonical(path);
    for (auto iterator = missing.rbegin(); iterator != missing.rend(); ++iterator)
        resolved /= *iterator;
    return resolved.lexically_normal();
}

struct DiscExportContext {
    std::shared_ptr<katana::runtime::GdiDiscSource> source;
    katana::runtime::DiscInstallRecipe recipe;
    std::string boot_sha256;
};

std::string prepared_boot_sha256(const PreparedPortProgram& prepared) {
    const auto* boot_segment =
        prepared.image.find_segment(prepared.boot_address, prepared.boot_size);
    if (boot_segment == nullptr)
        throw std::runtime_error(
            "Portvertrag kann das analysierte Bootprogramm nicht binden.");
    const auto boot_offset =
        boot_segment->byte_offset(prepared.boot_address);
    if (!boot_offset || *boot_offset > boot_segment->bytes.size() ||
        prepared.boot_size > boot_segment->bytes.size() - *boot_offset)
        throw std::runtime_error(
            "Portvertrag findet keine vollstaendigen Bootbytes.");
    return katana::io::sha256_bytes(std::string_view(
        reinterpret_cast<const char*>(
            boot_segment->bytes.data() + *boot_offset),
        prepared.boot_size));
}

DiscExportContext prepare_disc_export_context(
    const PreparedPortProgram& prepared,
    const std::shared_ptr<katana::runtime::GdiDiscSource>& validated_disc_source = {}) {
    const auto descriptor_input =
        std::find_if(prepared.inputs.begin(), prepared.inputs.end(), [](const auto& input) {
            return input.role == "gdi-descriptor";
        });
    if (descriptor_input == prepared.inputs.end() || descriptor_input->local_path.empty())
        throw std::invalid_argument(
            "Portexport besitzt keine lokale GDI-Eingabe fuer die Installations-Recipe.");
    auto disc_source = validated_disc_source;
    if (!disc_source)
        disc_source = katana::runtime::GdiDiscSource::open(descriptor_input->local_path);
    const auto& disc_descriptor = disc_source->descriptor();
    if (disc_descriptor.sha256 != descriptor_input->sha256)
        throw std::runtime_error("GDI wurde vor dem Recipe-Export veraendert.");
    for (const auto& track : disc_descriptor.tracks) {
        const auto role = "gdi-track-" + std::to_string(track.number);
        const auto expected = std::find_if(prepared.inputs.begin(),
                                           prepared.inputs.end(),
                                           [&](const auto& input) { return input.role == role; });
        if (expected == prepared.inputs.end() || expected->sha256 != track.sha256)
            throw std::runtime_error("GDI-Track wurde vor dem Recipe-Export veraendert.");
    }
    auto boot_sha256 = prepared_boot_sha256(prepared);
    auto recipe = katana::runtime::make_disc_install_recipe(
        *disc_source, std::string(prepared.project_identity), boot_sha256);
    return {std::move(disc_source), std::move(recipe), std::move(boot_sha256)};
}

DiscExportContext prepare_disc_export_context(
    const PreparedPortProgram& prepared,
    const katana::runtime::DiscInstallRecipe& validated_recipe) {
    auto boot_sha256 = prepared_boot_sha256(prepared);
    if (validated_recipe.job_generation != prepared.project_identity ||
        validated_recipe.boot_sha256 != boot_sha256)
        throw std::runtime_error(
            "Privates Boot-Executable und Disc-Recipe besitzen keine gemeinsame Identitaet.");
    return {{}, validated_recipe, std::move(boot_sha256)};
}

std::vector<LatentAotOccupiedRange>
latent_aot_occupied_ranges(const PreparedPortProgram& prepared) {
    std::vector<LatentAotOccupiedRange> result;
    result.reserve(prepared.image.segments().size() + prepared.program.size() * 2u);
    for (const auto& segment : prepared.image.segments()) {
        if (segment.memory_size != 0u)
            result.push_back({segment.virtual_address, segment.memory_size});
    }
    for (const auto& function : prepared.program) {
        result.push_back({function.entry_address, 2u});
        for (const auto& block : function.blocks) {
            std::uint64_t end = static_cast<std::uint64_t>(block.start_address) + 2u;
            for (const auto& instruction : block.instructions)
                end = std::max(end, static_cast<std::uint64_t>(instruction.source_address) + 2u);
            if (end > 0x1'0000'0000ull || end <= block.start_address)
                throw std::runtime_error(
                    "Vorbereitetes IR besitzt keine lineare belegte Adressrange.");
            result.push_back({block.start_address, end - block.start_address});
        }
    }
    return result;
}

} // namespace

void preserve_local_port_user_data(const std::filesystem::path& previous_root,
                                   const std::filesystem::path& published_root) {
    const auto previous = previous_root / "user-data";
    std::error_code status_error;
    const auto previous_status = std::filesystem::symlink_status(previous, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        previous_status.type() == std::filesystem::file_type::not_found)
        return;
    if (status_error)
        throw std::filesystem::filesystem_error(
            "Lokale Portdaten konnten nicht geprueft werden.", previous, status_error);
    if (!std::filesystem::is_directory(previous_status) ||
        std::filesystem::is_symlink(previous_status))
        throw std::runtime_error("Lokale Portdaten sind kein sicherer regulaerer Ordner.");

    const auto published = published_root / "user-data";
    if (std::filesystem::exists(published)) {
        const auto published_status = std::filesystem::symlink_status(published);
        if (!std::filesystem::is_directory(published_status) ||
            std::filesystem::is_symlink(published_status))
            throw std::runtime_error("Frisch publizierte lokale Daten sind kein sicherer Ordner.");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(published)) {
            const auto status = entry.symlink_status();
            if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
                throw std::runtime_error(
                    "Frisch publizierter Port besitzt unerwartet lokale Datendateien.");
        }
    }
    std::filesystem::remove_all(published);
    std::filesystem::rename(previous, published);
}

static PortExportResult export_dreamcast_port_project_impl(
    const PreparedPortProgram& prepared,
    const std::filesystem::path& output_root,
    const PortExportOptions& options,
    const std::shared_ptr<katana::runtime::GdiDiscSource>& validated_disc_source,
    const katana::runtime::DiscInstallRecipe* const validated_recipe = nullptr) {
    report_progress(options, "program-validation");
    if (output_root.empty() || !valid_target_name(options.target_name) ||
        options.tool_version.empty() || prepared.entry_address == 0u ||
        prepared.boot_address == 0u || prepared.boot_size == 0u || prepared.program.empty()) {
        throw std::invalid_argument(
            "Portexport braucht vorbereitetes IR, Einstieg, Bootprogramm, Ausgabe, "
            "Zielkennung und Werkzeugversion.");
    }
    if (prepared.boot_size > std::numeric_limits<std::uint32_t>::max() ||
        !katana::runtime::valid_guest_program_range(
            {prepared.boot_address, static_cast<std::uint32_t>(prepared.boot_size)}))
        throw std::invalid_argument(
            "Bootprogramm besitzt keine lineare ausfuehrbare Gastprogramm-Range.");
    static_cast<void>(console_profile_enumerator(options.console_profile));
    const auto blocking_diagnostics =
        std::count_if(prepared.analysis.recursive.diagnostics.begin(),
                      prepared.analysis.recursive.diagnostics.end(),
                      katana::analysis::analysis_diagnostic_blocks_codegen);
    if (!options.diagnostic_partial && blocking_diagnostics != 0u) {
        throw std::runtime_error("Portanalyse enthaelt unbekannte Instruktionen.");
    }
    if (!options.diagnostic_partial &&
        !prepared.analysis.guarded_aot_entry_rejections.empty()) {
        std::ostringstream reason;
        reason << "Portanalyse kann akzeptierte Guarded-AOT-Einstiege "
                  "nicht nativ materialisieren:";
        constexpr std::size_t reported_rejections = 8u;
        const auto count = std::min(
            reported_rejections,
            prepared.analysis.guarded_aot_entry_rejections.size());
        for (std::size_t index = 0u; index < count; ++index) {
            const auto& rejection =
                prepared.analysis.guarded_aot_entry_rejections[index];
            reason << (index == 0u ? " " : ", ")
                   << guarded_aot_address(rejection.guest_address)
                   << " ("
                   << katana::analysis::
                          guarded_aot_entry_rejection_reason_name(
                              rejection.reason)
                   << ')';
        }
        if (count <
            prepared.analysis.guarded_aot_entry_rejections.size()) {
            reason << ", ... ("
                   << prepared.analysis.guarded_aot_entry_rejections.size()
                   << " insgesamt)";
        }
        throw std::runtime_error(reason.str());
    }
    const auto incomplete = std::count_if(
        prepared.analysis.indirect_control_flow.begin(),
        prepared.analysis.indirect_control_flow.end(),
        [](const auto& resolution) {
            const auto status = katana::analysis::control_flow_report_status(resolution);
            return status == katana::analysis::ControlFlowReportStatus::GuardedPartial ||
                   status == katana::analysis::ControlFlowReportStatus::Unresolved;
        });
    if (!options.diagnostic_partial && incomplete != 0u) {
        throw std::runtime_error("Portanalyse ist unvollstaendig: " + std::to_string(incomplete) +
                                 " partielle oder ungeloeste Kontrollflussstellen.");
    }
    if (!options.diagnostic_partial &&
        !katana::analysis::guarded_aot_inventory_complete(
            prepared.analysis)) {
        std::ostringstream reason;
        reason << "Portanalyse besitzt kein vollstaendiges Guarded-AOT-Inventar:"
               << " function_budget_exhausted="
               << prepared.analysis.function_budget_exhausted
               << " raw_stored_candidates="
               << prepared.analysis.raw_stored_code_inventory_candidates
               << '/'
               << prepared.analysis.raw_stored_code_inventory_budget
               << " raw_stored_truncated="
               << prepared.analysis.raw_stored_code_inventory_truncated
               << " candidate_inventory_truncated="
               << prepared.analysis.candidate_inventory_truncated
               << " returned_table_scan_truncated="
               << prepared.analysis.returned_table_scan_truncated
               << " candidates="
               << prepared.analysis.guarded_code_inventory_candidates
               << '/' << prepared.analysis.guarded_code_inventory_budget
               << " shape_budget_exceeded="
               << prepared.analysis
                      .guarded_code_shape_budget_exceeded_candidates
               << " entry_rejections="
               << prepared.analysis.guarded_aot_entry_rejections.size();
        throw std::runtime_error(reason.str());
    }
    katana::ir::require_valid_program(prepared.program);
    require_guarded_aot_program_entries(
        prepared.program,
        prepared.analysis.guarded_aot_entries,
        "prepared-ir");
    std::optional<DiscExportContext> disc_context;
    if (validated_recipe != nullptr)
        disc_context.emplace(
            prepare_disc_export_context(prepared, *validated_recipe));
    LatentAotDiscovery latent_aot;
    if (prepared.discover_latent_aot) {
        if (validated_recipe != nullptr)
            throw std::invalid_argument(
                "Boot-Executable-Artefakt kann keine Discmodule analysieren.");
        report_progress(options, "latent-aot-source-validation");
        disc_context.emplace(
            prepare_disc_export_context(prepared, validated_disc_source));
        const std::array excluded_identities{"sha256:" + disc_context->boot_sha256};
        const auto occupied = latent_aot_occupied_ranges(prepared);
        report_progress(options, "latent-aot-discovery");
        latent_aot = discover_latent_aot_modules(disc_context->source,
                                                 prepared.disc_volume_start_lba,
                                                 prepared.disc_extent_lba_bias,
                                                 excluded_identities,
                                                 {},
                                                 occupied);
    }
    if (options.game_project != nullptr) {
        report_progress(options, "game-project-validation");
        const auto& game_project = *options.game_project;
        katana::runtime::validate_game_project_definition(game_project);
        if (game_project.game_entry_handoff.has_value()) {
            if (!prepared.direct_boot_executable)
                throw std::invalid_argument(
                    "Game-Entry-Handoff darf nur in einen "
                    "DirectBootExecutable-Produktport exportiert werden.");
            if (katana::runtime::dreamcast_console_profile_name(
                    game_project.game_entry_handoff->console_profile) !=
                options.console_profile)
                throw std::invalid_argument(
                    "Game-Entry-Handoff-Konsolenprofil passt nicht zum "
                    "Produktport.");
        }
        validate_game_project_image_contract(game_project, prepared.image);
        if (!disc_context.has_value())
            throw std::invalid_argument(
                "Game-Project-Export braucht eine validierte Disc- oder "
                "Boot-Artefaktidentitaet.");
        if (game_project.identity.content_identity !=
            disc_context->recipe.content_identity)
            throw std::invalid_argument(
                "Game-Project-Contentidentitaet passt nicht zur "
                "Installationsquelle.");
        const auto* boot_segment =
            prepared.image.find_segment(prepared.boot_address, prepared.boot_size);
        if (boot_segment == nullptr ||
            boot_segment->local_source_name !=
                game_project.identity.boot_file_name)
            throw std::invalid_argument(
                "Game-Project-Bootdateiidentitaet passt nicht zum "
                "Executable-Image.");
        const auto boot_bytes = game_project_image_bytes(
            prepared.image, prepared.boot_address, prepared.boot_size);
        const auto boot_byte_identity =
            std::string("sha256:") +
            katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(boot_bytes.data()),
                boot_bytes.size()));
        if (boot_byte_identity !=
            game_project.identity.boot_byte_identity)
            throw std::invalid_argument(
                "Game-Project-Bootbyteidentitaet passt nicht zum "
                "Executable-Image.");
        if (game_project.boot_config.has_value()) {
            if (prepared.direct_boot_executable)
                katana::runtime::validate_dreamcast_post_bios_cpu_state(
                    game_project.boot_config->post_bios_cpu_state,
                    prepared.boot_size);
        }
        const auto has_function =
            [&](const std::uint32_t address,
                const std::uint32_t size = 0u) {
            return std::any_of(
                prepared.analysis.recursive.functions.begin(),
                prepared.analysis.recursive.functions.end(),
                [&](const auto& candidate) {
                    return candidate.address == address &&
                           (size == 0u || candidate.size == size);
                });
        };
        for (const auto& function : game_project.function_boundaries) {
            if (!has_function(function.start, function.size))
                throw std::invalid_argument(
                    "Game-Project-Funktionsgrenze fehlt mit ihrer exakten "
                    "Groesse in der vorbereiteten Analyse.");
        }
        for (const auto& hook : game_project.mid_function_hooks) {
            if (!has_function(hook.instruction_address))
                throw std::invalid_argument(
                    "Game-Project-Mid-Function-Hook besitzt keinen "
                    "exportierten nativen Entry.");
        }
        for (const auto& table : game_project.jump_tables) {
            const auto analyzed = std::find_if(
                prepared.analysis.jump_tables.begin(),
                prepared.analysis.jump_tables.end(),
                [&](const auto& candidate) {
                    return candidate.dispatch_address ==
                               table.dispatch_address &&
                           candidate.table_address ==
                               table.table_address &&
                           candidate.requested_entries ==
                               table.entry_count &&
                           candidate.resolved;
                });
            if (analyzed == prepared.analysis.jump_tables.end())
                throw std::invalid_argument(
                    "Game-Project-Jump-Table ist in der vorbereiteten Analyse "
                    "nicht vollstaendig aufgeloest.");
        }
    }
    std::vector<katana::ir::Function> emitted_program(prepared.program.begin(),
                                                      prepared.program.end());
    for (const auto& module : latent_aot.modules)
        emitted_program.insert(emitted_program.end(), module.program.begin(), module.program.end());
    annotate_proven_linear_ram_accesses(emitted_program);
    katana::ir::require_valid_program(emitted_program);
    require_guarded_aot_program_entries(
        emitted_program,
        prepared.analysis.guarded_aot_entries,
        "final-emitted-ir");
    require_architectural_safepoint_program_entries(
        emitted_program, "final-emitted-ir");
    const auto hardware_audit =
        katana::analysis::audit_dreamcast_hardware(prepared.image, prepared.analysis);
    const auto wait_loop_descriptors = runtime_wait_loop_descriptors(hardware_audit);
    const auto mmio_wait_loops =
        mmio_wait_loop_batch_proofs(emitted_program, hardware_audit);
    report_progress(options, "partition-codegen");
    const auto partitions = partition_translation_units(emitted_program, options.partition_options);
    if (partitions.empty()) throw std::runtime_error("Portcodegen erzeugte keine Partition.");

    std::unique_ptr<CodegenCache> partition_cache;
    if (!options.codegen_cache_root.empty())
        partition_cache = std::make_unique<CodegenCache>(options.codegen_cache_root);
    std::atomic_size_t partition_cache_hits = 0u;
    std::atomic_size_t partition_cache_misses = 0u;
    std::unordered_set<std::uint32_t> emitted_function_entries;
    emitted_function_entries.reserve(emitted_program.size());
    std::unordered_map<std::uint32_t, std::uint32_t> emitted_block_owners;
    for (const auto& function : emitted_program) {
        emitted_function_entries.insert(function.entry_address);
        for (const auto& block : function.blocks)
            emitted_block_owners.emplace(block.start_address,
                                         function.entry_address);
    }
    std::unordered_set<std::uint32_t> external_hook_entries;
    if (options.game_project != nullptr) {
        for (const auto& function :
             options.game_project->function_overrides)
            external_hook_entries.insert(function.function_address);
        for (const auto& hook : options.game_project->mid_function_hooks)
            external_hook_entries.insert(hook.instruction_address);
    }

    // Runtime-only analysis candidates are not promoted to static CFG edges:
    // writable callback slots remain authoritative. A singleton candidate can
    // nevertheless be entered natively after the generated callsite compares
    // the live target and the runtime chaining guard validates the AOT entry.
    std::unordered_map<std::uint32_t, std::uint32_t> guarded_native_call_targets;
    std::unordered_set<std::uint32_t> ambiguous_guarded_native_callsites;
    for (const auto& resolution : prepared.analysis.indirect_control_flow) {
        if (resolution.kind != katana::analysis::IndirectControlFlowKind::Call)
            continue;
        auto candidates = resolution.analysis_candidates;
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());
        if (candidates.size() != 1u ||
            !emitted_function_entries.contains(candidates.front()))
            continue;
        const auto [found, inserted] = guarded_native_call_targets.emplace(
            resolution.instruction_address, candidates.front());
        if (!inserted && found->second != candidates.front())
            ambiguous_guarded_native_callsites.insert(
                resolution.instruction_address);
    }
    for (const auto callsite : ambiguous_guarded_native_callsites)
        guarded_native_call_targets.erase(callsite);
    const auto partition_configuration_hash = katana::io::sha256_bytes(
        std::string("cpp-port-partition-v") +
        std::to_string(port_partition_emission_schema_version) + ':' +
        std::string(port_namespace) + ':' +
        std::to_string(prepared.entry_address) + ':' +
        std::to_string(options.partition_options.maximum_functions) + ':' +
        std::to_string(options.partition_options.maximum_instructions) + ':' +
        std::to_string(native_aot_emission_profile_version) + ':' +
        std::to_string(options.diagnostic_partial) + ':' + options.console_profile);
    const auto partition_manifest_hash = katana::io::sha256_bytes(
        options.target_name + ':' + std::to_string(port_project_contract_version));
    const auto partition_overrides_hash =
        options.game_project != nullptr
            ? katana::io::sha256_bytes(
                  game_project_export_identity(*options.game_project))
            : katana::io::sha256_bytes(
                  "no-external-game-project-overrides-v2");
    std::vector<ProjectArtifact> artifacts;
    artifacts.reserve(partitions.size() + 9u);
    const auto emit_partition = [&](const TranslationUnitPartition& partition) {
        auto functions = select_functions(emitted_program, partition);
        std::unordered_set<std::uint32_t> local_function_entries;
        local_function_entries.reserve(functions.size());
        for (const auto& function : functions)
            local_function_entries.insert(function.entry_address);
        std::vector<std::uint32_t> external_callees;
        std::vector<GuardedNativeCallTarget> partition_guarded_native_calls;
        std::vector<NativeAotBlockOwnerEntry> partition_native_block_owners;
        std::vector<std::uint32_t> partition_architectural_boundaries;
        for (const auto& function : functions) {
            for (const auto callee : function.direct_callees) {
                if (!local_function_entries.contains(callee) &&
                    emitted_function_entries.contains(callee))
                    external_callees.push_back(callee);
            }
            for (const auto& block : function.blocks) {
                if (external_hook_entries.contains(block.start_address))
                    partition_architectural_boundaries.push_back(
                        block.start_address);
                for (const auto& instruction : block.instructions) {
                    for (const auto target : instruction.resolved_targets) {
                        const auto owner = emitted_block_owners.find(target);
                        if (owner == emitted_block_owners.end())
                            continue;
                        partition_native_block_owners.push_back(
                            {target, owner->second});
                        if (!local_function_entries.contains(owner->second))
                            external_callees.push_back(owner->second);
                    }
                    if (instruction.operation != katana::ir::Operation::CallRegister)
                        continue;
                    const auto guarded =
                        guarded_native_call_targets.find(instruction.source_address);
                    if (guarded == guarded_native_call_targets.end())
                        continue;
                    partition_guarded_native_calls.push_back(
                        {instruction.source_address, guarded->second});
                    if (!local_function_entries.contains(guarded->second))
                        external_callees.push_back(guarded->second);
                }
            }
        }
        std::sort(external_callees.begin(), external_callees.end());
        external_callees.erase(
            std::unique(external_callees.begin(), external_callees.end()),
            external_callees.end());
        for (const auto callee : external_callees) {
            if (external_hook_entries.contains(callee))
                partition_architectural_boundaries.push_back(callee);
        }
        std::sort(partition_guarded_native_calls.begin(),
                  partition_guarded_native_calls.end(),
                  [](const auto& left, const auto& right) {
                      return std::tie(left.callsite, left.target) <
                             std::tie(right.callsite, right.target);
                  });
        partition_guarded_native_calls.erase(
            std::unique(partition_guarded_native_calls.begin(),
                        partition_guarded_native_calls.end(),
                        [](const auto& left, const auto& right) {
                            return left.callsite == right.callsite &&
                                   left.target == right.target;
                        }),
            partition_guarded_native_calls.end());
        std::sort(partition_native_block_owners.begin(),
                  partition_native_block_owners.end(),
                  [](const auto& left, const auto& right) {
                      return std::tie(left.block_address, left.owner_entry) <
                             std::tie(right.block_address, right.owner_entry);
                  });
        partition_native_block_owners.erase(
            std::unique(partition_native_block_owners.begin(),
                        partition_native_block_owners.end(),
                        [](const auto& left, const auto& right) {
                            return left.block_address == right.block_address &&
                                   left.owner_entry == right.owner_entry;
                        }),
            partition_native_block_owners.end());
        std::sort(partition_architectural_boundaries.begin(),
                  partition_architectural_boundaries.end());
        partition_architectural_boundaries.erase(
            std::unique(partition_architectural_boundaries.begin(),
                        partition_architectural_boundaries.end()),
            partition_architectural_boundaries.end());
        const auto contains_program_entry =
            std::any_of(functions.begin(), functions.end(), [&prepared](const auto& function) {
                return function.entry_address == prepared.entry_address;
            });
        std::ostringstream partition_linkage_identity;
        partition_linkage_identity << "entry=" << contains_program_entry << ';';
        for (const auto callee : external_callees)
            partition_linkage_identity << "external=" << callee << ';';
        for (const auto& candidate : partition_guarded_native_calls)
            partition_linkage_identity << "guarded=" << candidate.callsite << ':'
                                       << candidate.target << ';';
        for (const auto& owner : partition_native_block_owners)
            partition_linkage_identity << "native-owner="
                                       << owner.block_address << ':'
                                       << owner.owner_entry << ';';
        for (const auto boundary : partition_architectural_boundaries)
            partition_linkage_identity << "architectural-boundary="
                                       << boundary << ';';
        const auto partition_emission_hash = katana::io::sha256_bytes(
            partition_configuration_hash + ':' + partition_linkage_identity.str());
        const auto translation_unit_name =
            deterministic_translation_unit_name(partition, emitted_program);
        const auto relative_path =
            std::filesystem::path("code") / translation_unit_name;
        std::string cache_key;
        const auto cache_artifact_name =
            (std::filesystem::path("partition") / translation_unit_name).generic_string();
        if (partition_cache) {
            cache_key = make_codegen_cache_key(
                {std::string(prepared.project_identity),
                 partition.content_sha256,
                 partition_emission_hash,
                 "cpp-port",
                 backend_interface_abi_version,
                 katana::runtime::abi_version,
                 partition_manifest_hash,
                 partition_overrides_hash,
                 2u,
                 native_aot_emission_profile_version,
                 options.tool_version});
            if (auto cached = partition_cache->load(cache_key, cache_artifact_name)) {
                partition_cache_hits.fetch_add(1u, std::memory_order_relaxed);
                return ProjectArtifact{relative_path, std::move(*cached)};
            }
        }
        // Declare only proven cross-partition callees. A complete global
        // declaration table in every unit would make source size quadratic.
        NativeAotBackendRequestOptions request_options;
        request_options.symbol_namespace = port_namespace;
        request_options.emit_run_functions = false;
        request_options.metadata_entry_address = prepared.entry_address;
        auto request = make_native_aot_backend_request(NativeAotEmissionProfile::Product,
                                                       functions,
                                                       functions.front().entry_address,
                                                       request_options);
        request.known_function_entries = external_callees;
        request.guarded_native_call_targets = partition_guarded_native_calls;
        request.native_block_owner_entries =
            partition_native_block_owners;
        request.architectural_boundary_entries =
            partition_architectural_boundaries;
        auto content = emit_cpp_port_translation_unit(request).joined_text();
        if (partition_cache) {
            partition_cache->store(cache_key, cache_artifact_name, content);
            partition_cache_misses.fetch_add(1u, std::memory_order_relaxed);
        }
        return ProjectArtifact{relative_path, std::move(content)};
    };
    const auto codegen_jobs = port_codegen_jobs(partitions.size());
    std::vector<std::optional<ProjectArtifact>> generated(partitions.size());
    std::atomic_size_t next_partition = 0u;
    std::vector<std::future<void>> workers;
    workers.reserve(codegen_jobs);
    for (std::size_t worker = 0u; worker < codegen_jobs; ++worker) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (;;) {
                const auto index = next_partition.fetch_add(1u, std::memory_order_relaxed);
                if (index >= partitions.size()) return;
                generated[index] = emit_partition(partitions[index]);
            }
        }));
    }
    for (auto& worker : workers)
        worker.get();
    for (auto& artifact : generated)
        artifacts.push_back(std::move(*artifact));
    report_progress(options, "metadata");
    const auto entry_partition =
        std::find_if(partitions.begin(), partitions.end(), [&](const auto& partition) {
            return std::any_of(partition.function_indices.begin(),
                               partition.function_indices.end(),
                               [&](const auto index) {
                                   return emitted_program[index].entry_address ==
                                          prepared.entry_address;
                               });
        });
    if (entry_partition == partitions.end()) {
        throw std::runtime_error("Portcodegen besitzt keine Einstiegspartition.");
    }
    const auto entry_namespace = std::string(port_namespace);
    std::string source_map_json;
    std::string control_flow_graph_json;
    std::string control_flow_graph_dot;
    std::string call_graph_json;
    std::string call_graph_dot;
    bool metadata_cache_hit = false;
    std::string metadata_cache_key;
    if (partition_cache) {
        std::ostringstream metadata_ir_identity;
        metadata_ir_identity << "port-metadata-v"
                             << port_metadata_cache_schema_version << ':';
        for (const auto& partition : partitions)
            metadata_ir_identity << partition.content_sha256 << ';';
        for (const auto& input : prepared.inputs)
            metadata_ir_identity << input.role << ':' << input.size << ':'
                                 << input.sha256 << ';';
        for (const auto& module : latent_aot.modules)
            metadata_ir_identity << module.id << ':' << module.byte_identity << ':'
                                 << module.source_address << ':' << module.byte_size << ':'
                                 << module.disc_byte_offset << ';';
        metadata_cache_key = make_codegen_cache_key(
            {std::string(prepared.project_identity),
             katana::io::sha256_bytes(metadata_ir_identity.str()),
             katana::io::sha256_bytes(
                 options.target_name + ':' + options.console_profile + ':' +
                 std::to_string(prepared.entry_address) + ':' +
                 std::to_string(prepared.direct_boot_executable) + ':' +
                 std::to_string(port_metadata_cache_schema_version)),
             "port-metadata",
             backend_interface_abi_version,
             katana::runtime::abi_version,
             partition_manifest_hash,
             partition_overrides_hash,
             2u,
             native_aot_emission_profile_version,
             options.tool_version});
        auto cached_source_map =
            partition_cache->load(metadata_cache_key, "source-map.json");
        auto cached_cfg_json =
            partition_cache->load(metadata_cache_key, "cfg.json");
        auto cached_cfg_dot =
            partition_cache->load(metadata_cache_key, "cfg.dot");
        auto cached_callgraph_json =
            partition_cache->load(metadata_cache_key, "callgraph.json");
        auto cached_callgraph_dot =
            partition_cache->load(metadata_cache_key, "callgraph.dot");
        if (cached_source_map && cached_cfg_json && cached_cfg_dot &&
            cached_callgraph_json && cached_callgraph_dot) {
            source_map_json = std::move(*cached_source_map);
            control_flow_graph_json = std::move(*cached_cfg_json);
            control_flow_graph_dot = std::move(*cached_cfg_dot);
            call_graph_json = std::move(*cached_callgraph_json);
            call_graph_dot = std::move(*cached_callgraph_dot);
            metadata_cache_hit = true;
        }
    }
    if (!metadata_cache_hit) {
        auto source_map_image = prepared.image;
        if (options.game_project != nullptr)
            apply_game_project_symbols(
                source_map_image, *options.game_project);
        for (const auto& module : latent_aot.modules) {
            katana::io::ImageSegment segment{
                "latent-aot-module",
                module.source_address,
                module.disc_byte_offset,
                module.byte_size,
                katana::io::SegmentKind::Mixed,
                {true, false, true},
                // The validated disc span remains addressable to the source map without
                // retaining retail bytes or allocating a same-sized zero buffer.
                {}};
            segment.source_kind = katana::io::ImageSourceKind::DiscModule;
            segment.load_phase = katana::io::ImageLoadPhase::RuntimeModule;
            segment.latent_source_size = module.byte_size;
            source_map_image.add_segment(std::move(segment));
        }
        source_map_json =
            serialize_address_source_map(build_address_source_map(source_map_image, artifacts));
        const auto control_flow_graph =
            katana::analysis::build_control_flow_graph(prepared.analysis);
        const auto call_graph =
            katana::analysis::build_call_graph(prepared.analysis);
        control_flow_graph_json =
            katana::analysis::serialize_analysis_graph_json(control_flow_graph);
        control_flow_graph_dot =
            katana::analysis::serialize_analysis_graph_dot(control_flow_graph);
        call_graph_json = katana::analysis::serialize_analysis_graph_json(call_graph);
        call_graph_dot = katana::analysis::serialize_analysis_graph_dot(call_graph);
        if (partition_cache) {
            partition_cache->store(metadata_cache_key, "source-map.json", source_map_json);
            partition_cache->store(metadata_cache_key, "cfg.json", control_flow_graph_json);
            partition_cache->store(metadata_cache_key, "cfg.dot", control_flow_graph_dot);
            partition_cache->store(metadata_cache_key, "callgraph.json", call_graph_json);
            partition_cache->store(metadata_cache_key, "callgraph.dot", call_graph_dot);
        }
    }
    katana::io::BuildProvenance provenance;
    provenance.tool_version = options.tool_version;
    provenance.manifest_version = port_project_contract_version;
    provenance.manifest_sha256 = katana::io::sha256_bytes(
        options.target_name + ":" + std::to_string(port_project_contract_version));
    provenance.ir_version = 2u;
    provenance.runtime_abi = katana::runtime::abi_version;
    provenance.backend_name = "cpp";
    provenance.backend_abi = backend_interface_abi_version;
    provenance.inputs.assign(prepared.inputs.begin(), prepared.inputs.end());

    artifacts.push_back({"include/katana_port.hpp", generated_header(entry_namespace)});
    auto dispatch_artifacts = runtime_dispatch_artifacts(
        entry_namespace,
        emitted_program,
        options.diagnostic_partial,
        prepared.analysis.runtime_code_copies.copies,
        prepared.analysis.indirect_control_flow,
        prepared.analysis.recursive.functions,
        prepared.analysis.guarded_aot_entries,
        prepared.image,
        prepared.boot_address,
        prepared.boot_size,
        latent_aot.modules,
        mmio_wait_loops,
        options.game_project,
        disc_context ? std::string_view(disc_context->recipe.content_identity)
                     : std::string_view{});
    for (auto& artifact : dispatch_artifacts)
        artifacts.push_back(std::move(artifact));
    artifacts.push_back({"katana-port.cmake", port_cmake(options.target_name)});
    artifacts.push_back({"metadata/port-project.json",
                         port_metadata(options,
                                       emitted_program.size(),
                                       partitions,
                                       prepared.entry_address,
                                       prepared.boot_size,
                                       prepared.direct_boot_executable,
                                       prepared.project_identity,
                                       prepared.analysis,
                                       latent_aot.modules.size())});
    if (options.game_project != nullptr)
        artifacts.push_back(
            {"metadata/game-project.json",
             game_project_metadata(*options.game_project)});
    artifacts.push_back(
        {"metadata/provenance.json", katana::io::format_build_provenance_json(provenance)});
    artifacts.push_back({"metadata/source-map.json", std::move(source_map_json)});
    artifacts.push_back({"metadata/cfg.json", std::move(control_flow_graph_json)});
    artifacts.push_back({"metadata/cfg.dot", std::move(control_flow_graph_dot)});
    artifacts.push_back({"metadata/callgraph.json", std::move(call_graph_json)});
    artifacts.push_back({"metadata/callgraph.dot", std::move(call_graph_dot)});

    const auto absolute_root = std::filesystem::absolute(output_root).lexically_normal();
    const auto resolved_root = resolve_existing_parents(absolute_root);
    if (!options.forbidden_source_root.empty()) {
        const auto source_root = std::filesystem::canonical(options.forbidden_source_root);
        if (path_is_within(resolved_root, source_root)) {
            throw std::invalid_argument(
                "Port-Ausgabe muss ausserhalb des KatanaRecomp-Quellbaums liegen.");
        }
    }
    std::error_code root_error;
    const auto root_status = std::filesystem::symlink_status(absolute_root, root_error);
    if (!root_error && std::filesystem::is_symlink(root_status)) {
        throw std::runtime_error("Port-Ausgabeziel darf kein symbolischer Link sein.");
    }
    if (root_error && root_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("Port-Ausgabeziel konnte nicht geprueft werden.");
    }
    std::filesystem::create_directories(absolute_root);
    const auto canonical_root = std::filesystem::canonical(absolute_root);
    if (!options.forbidden_source_root.empty() &&
        path_is_within(canonical_root, std::filesystem::canonical(options.forbidden_source_root))) {
        throw std::invalid_argument("Kanonische Port-Ausgabe liegt im KatanaRecomp-Quellbaum.");
    }
    report_progress(options, "disc-recipe");
    if (!disc_context) {
        disc_context.emplace(
            prepare_disc_export_context(prepared, validated_disc_source));
    }
    const auto& recipe = disc_context->recipe;
    const auto& boot_sha256 = disc_context->boot_sha256;
    const auto recipe_path = canonical_root / "content" / "game.katana-install";
    write_port_file(canonical_root,
                    "content/game.katana-install",
                    katana::runtime::format_disc_install_recipe(recipe),
                    true);
    std::filesystem::create_directories(canonical_root / "runtime");
    std::filesystem::create_directories(canonical_root / "user-data" / "content");
    report_progress(options, "artifact-write");
    const auto write = write_codegen_project(
        canonical_root / "generated", std::move(artifacts), ProjectWriteOptions{codegen_jobs});
    write_port_file(
        canonical_root, "CMakeLists.txt", root_cmake(options.diagnostic_partial), true);
    write_port_file(
        canonical_root, "run-product-gate.ps1", product_gate_runner(), true);
    write_port_file(canonical_root,
                    ".gitignore",
                    "/build/\n/build-*/\n/user-data/\n/content/*.katana-disc\n*.katana-disc\n",
                    true);
    write_port_file(
        canonical_root,
        "INSTALL_ORIGINAL_DISC.txt",
        "ORIGINAL DISC REQUIRED - DISTRIBUTABLE PORT\n\n"
        "This package contains native AOT code and a hash/layout recipe, but no retail disc "
        "sectors.\nRun: game.exe --install-disc <path-to-your-own-disc.gdi>\n\n"
        "The installer validates descriptor, region-bearing boot data, complete track layout "
        "and SHA-256 identities before creating user-data/content/game.katana-disc locally.\n"
        "That local cache contains complete retail data and must never be redistributed.\n"
        "The original GDI and tracks are opened read-only and are never modified or deleted.\n",
        true);
    const auto* exported_boot_segment =
        prepared.image.find_segment(prepared.boot_address, prepared.boot_size);
    if (exported_boot_segment == nullptr ||
        (prepared.direct_boot_executable &&
         exported_boot_segment->local_source_name.empty()))
        throw std::runtime_error(
            "DirectBootExecutable braucht einen gebundenen Bootdateinamen.");
    write_port_file(canonical_root,
                    "src/main.cpp",
                    handwritten_main(entry_namespace,
                                     prepared.hle_bios_abi,
                                     prepared.direct_boot_executable,
                                     options.diagnostic_partial,
                                     prepared.inputs,
                                     wait_loop_descriptors,
                                     recipe.job_generation,
                                     recipe.content_identity,
                                     exported_boot_segment->local_source_name,
                                     boot_sha256,
                                     options.console_profile,
                                     prepared.boot_address,
                                     prepared.boot_size,
                                     prepared.direct_boot_executable &&
                                             options.game_project != nullptr &&
                                             options.game_project->boot_config
                                                 .has_value()
                                         ? &*options.game_project->boot_config
                                         : nullptr,
                                     options.game_project),
                    true);

    PortExportResult result;
    result.output_root = canonical_root;
    result.functions = emitted_program.size();
    result.partitions = partitions.size();
    result.generated_files = write.written_files.size();
    result.removed_files = write.removed_files.size();
    result.codegen_cache_hits = partition_cache_hits.load(std::memory_order_relaxed);
    result.codegen_cache_misses = partition_cache_misses.load(std::memory_order_relaxed);
    result.metadata_cache_hit = metadata_cache_hit;
    result.disc_install_recipe = recipe_path;
    result.job_generation = recipe.job_generation;
    result.content_identity = recipe.content_identity;
    result.disc_tracks = recipe.tracks.size();
    result.checkpoints = {validated_recipe != nullptr
                              ? "boot-executable-artifact-validated"
                              : "gdi-validated",
                          "disc-recipe-written",
                          "retail-content-excluded",
                          "boot-image-loaded",
                          "analysis-complete",
                          "ir-lowered",
                          "partitioned-codegen-complete",
                          "port-project-written"};
    if (partition_cache)
        result.checkpoints.push_back(result.codegen_cache_misses == 0u
                                         ? "partition-codegen-cache-hit"
                                         : "partition-codegen-cache-populated");
    if (!latent_aot.modules.empty())
        result.checkpoints.insert(result.checkpoints.begin() + 2u, "latent-aot-registry-written");
    if (options.game_project != nullptr)
        result.checkpoints.insert(
            result.checkpoints.begin() + 2u,
            "external-game-project-identity-validated");
    return result;
}

PortExportResult export_dreamcast_port_project(const PreparedPortProgram& prepared,
                                               const std::filesystem::path& output_root,
                                               const PortExportOptions& options) {
    return export_dreamcast_port_project_impl(prepared, output_root, options, {});
}

PortExportResult export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                                               const std::filesystem::path& output_root,
                                               const PortExportOptions& options) {
    if (gdi_path.empty()) {
        throw std::invalid_argument("Portexport braucht eine GDI-Quelle.");
    }
    report_progress(options, "disc-load");
    const auto disc = katana::platform::load_dreamcast_gdi_boot(gdi_path);
    return export_dreamcast_port_project(disc, output_root, options);
}

PortExportResult export_dreamcast_port_project(
    const katana::platform::DreamcastDiscBoot& disc,
    const std::filesystem::path& output_root,
    const PortExportOptions& options) {
    if (!disc.source || disc.system_bootstrap.empty() ||
        disc.boot_file.empty())
        throw std::invalid_argument(
            "Portexport braucht eine vollstaendig geladene GDI-Quelle.");
    report_progress(options, "boot-image");
    auto image = katana::platform::make_dreamcast_disc_executable(
        disc, katana::platform::DreamcastDiscExecutionPath::NativeSystemBootstrap);
    std::optional<katana::analysis::AnalysisOverrides>
        game_project_overrides;
    if (options.game_project != nullptr) {
        katana::runtime::validate_game_project_definition(
            *options.game_project);
        validate_game_project_image_contract(*options.game_project, image);
        apply_game_project_symbols(image, *options.game_project);
        game_project_overrides =
            game_project_analysis_overrides(*options.game_project, image);
    }
    report_progress(options, "control-flow-analysis");
    const auto analysis_started = std::chrono::steady_clock::now();
    const auto analysis = katana::analysis::analyze_control_flow(
        image,
        game_project_overrides ? &*game_project_overrides : nullptr,
        [&options,
         analysis_started](const katana::analysis::ControlFlowAnalysisProgress& progress) {
            if (options.progress_callback == nullptr) return;
            const bool sampled_iteration = progress.iteration <= 16u ||
                                           (progress.iteration != 0u &&
                                            (progress.iteration & (progress.iteration - 1u)) == 0u);
            if (!sampled_iteration && progress.phase != "fixpoint-complete" &&
                progress.phase != "complete")
                return;
            std::ostringstream marker;
            marker << "control-flow-" << progress.phase << "-i" << progress.iteration << "-s"
                   << progress.seeds << "-n" << progress.instructions << "-c" << progress.contexts
                   << "-r" << progress.resolutions << "-ms"
                   << std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - analysis_started)
                          .count();
            const auto text = marker.str();
            report_progress(options, text);
        });
    report_progress(options, "ir-lowering");
    const auto architectural_safepoints =
        katana::ir::architectural_safepoint_block_leaders(analysis);
    auto program =
        katana::ir::lower_program(analysis, architectural_safepoints);
    report_progress(options, "ir-optimization");
    const auto& emission_contract = native_aot_emission_contract(NativeAotEmissionProfile::Product);
    static_cast<void>(
        katana::ir::optimize_program(program, emission_contract.optimization_options));
    report_progress(options, "input-provenance");
    std::vector<katana::io::InputProvenance> inputs;
    const auto& descriptor = disc.source->descriptor();
    inputs.push_back(
        {"gdi-descriptor", descriptor.size, descriptor.sha256, descriptor.resolved_path});
    for (const auto& track : disc.source->descriptor().tracks) {
        inputs.push_back({"gdi-track-" + std::to_string(track.number),
                          track.file_offset + track.sector_count * track.sector_size,
                          track.sha256,
                          track.resolved_path});
    }
    const auto project_identity =
        katana::platform::dreamcast_disc_project_identity(disc);
    return export_dreamcast_port_project_impl(
        {image,
         analysis,
         program,
         inputs,
         katana::platform::dreamcast_system_bootstrap_entry_address,
         katana::platform::dreamcast_disc_boot_address,
         disc.boot_file.size(),
         project_identity,
         true,
         true,
         disc.data_track_lba,
         disc.extent_lba_bias},
        output_root,
        options,
        disc.source);
}

PortExportResult export_dreamcast_port_project_from_boot_artifact(
    const std::filesystem::path& artifact_manifest_path,
    const std::filesystem::path& output_root,
    const PortExportOptions& options) {
    if (output_root.empty())
        throw std::invalid_argument(
            "Boot-Executable-Portexport braucht ein Ausgabeziel.");
    report_progress(options, "boot-artifact-load");
    const auto artifact =
        katana::platform::load_dreamcast_boot_executable_artifact(
            artifact_manifest_path);
    const auto artifact_root = artifact.manifest_path.parent_path();
    const auto resolved_output =
        resolve_existing_parents(
            std::filesystem::absolute(output_root).lexically_normal());
    if (path_is_within(resolved_output, artifact_root) ||
        path_is_within(artifact_root, resolved_output))
        throw std::invalid_argument(
            "Port-Ausgabe und privates Boot-Executable-Artefakt duerfen sich nicht ueberlappen.");

    report_progress(options, "boot-image");
    auto image =
        katana::platform::make_dreamcast_boot_executable(artifact);
    std::optional<katana::analysis::AnalysisOverrides>
        game_project_overrides;
    if (options.game_project != nullptr) {
        katana::runtime::validate_game_project_definition(
            *options.game_project);
        validate_game_project_image_contract(*options.game_project, image);
        apply_game_project_symbols(image, *options.game_project);
        game_project_overrides =
            game_project_analysis_overrides(*options.game_project, image);
    }
    report_progress(options, "control-flow-analysis");
    const auto analysis_started = std::chrono::steady_clock::now();
    const auto analysis = katana::analysis::analyze_control_flow(
        image,
        game_project_overrides ? &*game_project_overrides : nullptr,
        [&options,
         analysis_started](
            const katana::analysis::ControlFlowAnalysisProgress& progress) {
            if (options.progress_callback == nullptr) return;
            const bool sampled_iteration =
                progress.iteration <= 16u ||
                (progress.iteration != 0u &&
                 (progress.iteration & (progress.iteration - 1u)) == 0u);
            if (!sampled_iteration &&
                progress.phase != "fixpoint-complete" &&
                progress.phase != "complete")
                return;
            std::ostringstream marker;
            marker << "control-flow-" << progress.phase << "-i"
                   << progress.iteration << "-s" << progress.seeds << "-n"
                   << progress.instructions << "-c" << progress.contexts
                   << "-r" << progress.resolutions << "-ms"
                   << std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() -
                          analysis_started)
                          .count();
            const auto text = marker.str();
            report_progress(options, text);
        });
    report_progress(options, "ir-lowering");
    const auto architectural_safepoints =
        katana::ir::architectural_safepoint_block_leaders(analysis);
    auto program =
        katana::ir::lower_program(analysis, architectural_safepoints);
    report_progress(options, "ir-optimization");
    const auto& emission_contract =
        native_aot_emission_contract(NativeAotEmissionProfile::Product);
    static_cast<void>(katana::ir::optimize_program(
        program, emission_contract.optimization_options));

    report_progress(options, "input-provenance");
    std::vector<katana::io::InputProvenance> inputs;
    inputs.reserve(artifact.install_recipe.tracks.size() + 4u);
    inputs.push_back({"gdi-descriptor",
                      0u,
                      artifact.install_recipe.descriptor_sha256,
                      {}});
    for (const auto& track : artifact.install_recipe.tracks)
        inputs.push_back(
            {"gdi-track-" + std::to_string(track.number),
             track.file_offset +
                 track.sector_count *
                     static_cast<std::uint64_t>(track.sector_size),
             track.sha256,
             {}});
    inputs.push_back(katana::io::capture_input_provenance(
        "boot-executable-manifest", artifact.manifest_path));
    inputs.push_back(katana::io::capture_input_provenance(
        "boot-executable-private", artifact.executable_path));
    inputs.push_back(katana::io::capture_input_provenance(
        "disc-install-recipe", artifact.install_recipe_path));

    return export_dreamcast_port_project_impl(
        {image,
         analysis,
         program,
         inputs,
         artifact.entry_address,
         katana::platform::dreamcast_disc_boot_address,
         artifact.boot_file.size(),
         artifact.project_identity,
         true,
         false,
         0u,
         0u,
         true},
        output_root,
        options,
        {},
        &artifact.install_recipe);
}

} // namespace katana::codegen
