#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/native_aot_profile.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

katana::ir::Instruction instruction(const std::uint32_t address,
                                    const katana::ir::Operation operation) {
    katana::ir::Instruction result;
    result.source_address = address;
    result.original_opcode = operation == katana::ir::Operation::Return ? 0x000Bu : 0x0009u;
    result.original_operation = operation;
    result.operation = operation;
    result.widths = katana::ir::operation_operand_widths(operation);
    result.status_effects = katana::ir::instruction_status_effects(operation);
    result.memory_effects = katana::ir::instruction_memory_effects(operation);
    result.accumulator_effects = katana::ir::operation_accumulator_effects(operation);
    return result;
}

katana::ir::Function make_function() {
    constexpr std::uint32_t entry = 0x8C010000u;
    auto owner = instruction(entry, katana::ir::Operation::Return);
    owner.delay_slot = {katana::ir::DelaySlotRole::Owner, entry + 2u};
    auto slot = instruction(entry + 2u, katana::ir::Operation::Nop);
    slot.delay_slot = {katana::ir::DelaySlotRole::Slot, entry};
    katana::ir::BasicBlock block;
    block.start_address = entry;
    block.instructions = {owner, slot};
    katana::ir::Function function;
    function.entry_address = entry;
    function.blocks = {block};
    return function;
}

katana::ir::Function make_observed_function() {
    constexpr std::uint32_t entry = 0x8C010000u;
    auto normal = instruction(entry, katana::ir::Operation::Nop);
    auto owner = instruction(entry + 2u, katana::ir::Operation::Return);
    owner.delay_slot = {katana::ir::DelaySlotRole::Owner, entry + 4u};
    auto slot = instruction(entry + 4u, katana::ir::Operation::Nop);
    slot.delay_slot = {katana::ir::DelaySlotRole::Slot, entry + 2u};
    katana::ir::BasicBlock block;
    block.start_address = entry;
    block.instructions = {normal, owner, slot};
    katana::ir::Function function;
    function.entry_address = entry;
    function.blocks = {block};
    return function;
}

bool same_optimization_options(const katana::ir::OptimizationOptions& left,
                               const katana::ir::OptimizationOptions& right) {
    return left.enabled == right.enabled && left.constant_folding == right.constant_folding &&
           left.copy_propagation == right.copy_propagation &&
           left.dead_code_elimination == right.dead_code_elimination &&
           left.cfg_simplification == right.cfg_simplification &&
           left.load_store_simplification == right.load_store_simplification &&
           left.capture_dumps == right.capture_dumps;
}

} // namespace

int main() {
    constexpr std::uint32_t entry = 0x8C010000u;
    const std::array functions = {make_function()};
    const katana::codegen::CppBackend backend;
    const auto emission = katana::codegen::generate_program(backend, {functions, entry});

    require(backend.name() == "cpp", "C++-Backend besitzt keine stabile Identitaet.");
    require(emission.declarations.find(
                "#include \"katana/runtime/aot_runtime_abi.hpp\"") != std::string::npos &&
                emission.declarations.find("static void fn_8C010000") != std::string::npos &&
                emission.declarations.find("katana/platform/") == std::string::npos &&
                emission.declarations.find("katana/runtime/pvr") == std::string::npos &&
                emission.declarations.find("katana/runtime/aica") == std::string::npos &&
                emission.declarations.find("katana/runtime/maple") == std::string::npos &&
                emission.declarations.find("katana/runtime/gdi") == std::string::npos &&
                emission.declarations.find("void run(CpuState& cpu) {") == std::string::npos,
            "C++-Backend trennt Deklarationen und Funktionskoerper nicht.");
    require(
        emission.functions.find(
            "static void fn_8C010000_with_services(CpuState& cpu, PlatformServices* services) {") !=
                std::string::npos &&
            emission.functions.find("void run(CpuState& cpu) {") != std::string::npos &&
            emission.functions.find("void run(CpuState& cpu, PlatformServices& services)") !=
                std::string::npos,
        "C++-Backend emittiert die Funktionskoerper nicht im Funktionsabschnitt.");
    require(emission.metadata.find("generated_entry_address = 0x8C010000u") != std::string::npos &&
                emission.metadata.find("} // namespace katana_generated") != std::string::npos,
            "C++-Backend emittiert keine getrennten stabilen Metadaten.");
    require(emission.joined_text() == katana::codegen::emit_cpp_program(functions, entry),
            "Kompatibilitaetsfunktion umgeht oder veraendert das C++-Backend.");

    using katana::codegen::NativeAotEmissionProfile;
    const auto& product_contract =
        katana::codegen::native_aot_emission_contract(NativeAotEmissionProfile::Product);
    const auto& conformance_contract = katana::codegen::native_aot_emission_contract(
        NativeAotEmissionProfile::ExternalConformance);
    require(
        product_contract.version == katana::codegen::native_aot_emission_profile_version &&
            conformance_contract.version == katana::codegen::native_aot_emission_profile_version &&
            katana::codegen::native_aot_emission_profile_name(NativeAotEmissionProfile::Product) ==
                std::string_view("product") &&
            katana::codegen::native_aot_emission_profile_name(
                NativeAotEmissionProfile::ExternalConformance) ==
                std::string_view("external-conformance"),
        "Native-AOT-Profile besitzen keine stabile Version oder Identitaet.");
    require(same_optimization_options(product_contract.optimization_options,
                                      conformance_contract.optimization_options) &&
                product_contract.partition_options.maximum_functions ==
                    conformance_contract.partition_options.maximum_functions &&
                product_contract.partition_options.maximum_instructions ==
                    conformance_contract.partition_options.maximum_instructions &&
                product_contract.external_function_linkage &&
                conformance_contract.external_function_linkage &&
                product_contract.single_block_execution &&
                conformance_contract.single_block_execution &&
                product_contract.external_dynamic_dispatch &&
                conformance_contract.external_dynamic_dispatch &&
                product_contract.guarded_local_block_chaining &&
                conformance_contract.guarded_local_block_chaining,
            "Produkt- und Konformitaetsprofil verwenden nicht denselben AOT-Vertrag.");

    const std::array observed_functions = {make_observed_function()};
    katana::codegen::NativeAotBackendRequestOptions product_options;
    product_options.symbol_namespace = "katana_product_profile";
    product_options.emit_run_functions = false;
    product_options.metadata_entry_address = entry;
    const auto product_request = katana::codegen::make_native_aot_backend_request(
        NativeAotEmissionProfile::Product, observed_functions, entry, product_options);
    const auto product_emission = backend.emit(product_request);
    require(product_request.external_function_linkage && product_request.single_block_execution &&
                product_request.external_dynamic_dispatch &&
                product_request.guarded_local_block_chaining &&
                !product_request.external_instruction_observer &&
                product_emission.joined_text().find("note_instruction_entry") ==
                    std::string::npos &&
                product_emission.functions.find(
                    "cpu.attempted_guest_instructions += 1u;") != std::string::npos,
            "Produktprofil emittiert nicht den produktiven AOT-Vertrag hookfrei.");

    katana::codegen::NativeAotBackendRequestOptions conformance_options;
    conformance_options.symbol_namespace = "katana_external_conformance";
    conformance_options.emit_run_functions = false;
    conformance_options.metadata_entry_address = entry;
    conformance_options.external_instruction_observer = true;
    const auto conformance_request = katana::codegen::make_native_aot_backend_request(
        NativeAotEmissionProfile::ExternalConformance,
        observed_functions,
        entry,
        conformance_options);
    const auto conformance_emission = backend.emit(conformance_request);
    require(conformance_request.external_function_linkage ==
                    product_request.external_function_linkage &&
                conformance_request.single_block_execution ==
                    product_request.single_block_execution &&
                conformance_request.external_dynamic_dispatch ==
                    product_request.external_dynamic_dispatch &&
                conformance_request.guarded_local_block_chaining ==
                    product_request.guarded_local_block_chaining &&
                conformance_request.external_instruction_observer &&
                conformance_emission.declarations.find(
                    "void note_instruction_entry(std::uint32_t address, "
                    "bool in_delay_slot) noexcept;") != std::string::npos,
            "Konformitaetsprofil behaelt den Produktvertrag nicht mit externem Beobachter bei.");

    const auto normal_hook = conformance_emission.functions.find(
        "note_instruction_entry(katana::runtime::relocate_code_address(0x8C010000u), false);");
    const auto normal_attempt = conformance_emission.functions.find(
        "ExplicitGuestInstructionAttempt guest_instruction_attempt", normal_hook);
    const auto owner_hook = conformance_emission.functions.find(
        "note_instruction_entry(katana::runtime::relocate_code_address(0x8C010002u), false);");
    const auto terminal_attempt =
        conformance_emission.functions.find(
            "ExplicitGuestInstructionAttempt terminal_instruction_attempt");
    const auto slot_hook = conformance_emission.functions.find(
        "note_instruction_entry(katana::runtime::relocate_code_address(0x8C010004u), true);");
    const auto slot_attempt = conformance_emission.functions.find(
        "ExplicitGuestInstructionAttempt guest_instruction_attempt", slot_hook);
    require(normal_hook != std::string::npos && normal_attempt != std::string::npos &&
                owner_hook != std::string::npos && terminal_attempt != std::string::npos &&
                slot_hook != std::string::npos && slot_attempt != std::string::npos &&
                normal_hook < normal_attempt && normal_attempt < owner_hook &&
                owner_hook < terminal_attempt && terminal_attempt < slot_hook &&
                slot_hook < slot_attempt &&
                conformance_emission.functions.find("cpu.attempted_guest_instructions +=") ==
                    std::string::npos,
            "Externer Beobachter liegt nicht vor jeder exakten Instruktionsbuchhaltung oder "
            "laesst Regionenbatching zu.");

    bool product_observer_rejected = false;
    try {
        auto invalid_options = product_options;
        invalid_options.external_instruction_observer = true;
        static_cast<void>(katana::codegen::make_native_aot_backend_request(
            NativeAotEmissionProfile::Product, observed_functions, entry, invalid_options));
    } catch (const std::invalid_argument&) {
        product_observer_rejected = true;
    }
    require(product_observer_rejected,
            "Produktprofil akzeptiert faelschlich externe Instruktionsbeobachtung.");

    auto direct_ram = instruction(0x8C020000u, katana::ir::Operation::LoadLongPcRelative);
    direct_ram.effective_address = 0x8C000000u;
    auto outside_ram = instruction(0x8C020002u, katana::ir::Operation::LoadLongPcRelative);
    outside_ram.effective_address = 0x8BFFFFFCu;
    katana::ir::BasicBlock memory_block;
    memory_block.start_address = direct_ram.source_address;
    memory_block.instructions = {direct_ram, outside_ram};
    katana::ir::Function memory_function;
    memory_function.entry_address = memory_block.start_address;
    memory_function.blocks = {memory_block};
    std::vector memory_program = {memory_function};
    katana::codegen::annotate_proven_linear_ram_accesses(memory_program);
    require(memory_program.front().blocks.front().instructions[0].memory_effects.region ==
                    katana::ir::MemoryRegionKind::NormalRam &&
                memory_program.front().blocks.front().instructions[1].memory_effects.region ==
                    katana::ir::MemoryRegionKind::Unknown,
            "Gemeinsame AOT-RAM-Annotation klassifiziert direkte Zugriffe nicht konservativ.");

    std::cout << "KR-3202 C++-Backend-Migration erfolgreich.\n";
    return 0;
}
