#include "katana/codegen/cpp_emitter.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

katana::ir::Instruction instruction(
    const std::uint32_t address,
    const katana::ir::Operation operation
) {
    katana::ir::Instruction result;
    result.source_address = address;
    result.original_opcode = operation == katana::ir::Operation::Return
        ? 0x000Bu
        : 0x0009u;
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

} // namespace

int main() {
    constexpr std::uint32_t entry = 0x8C010000u;
    const std::array functions = {make_function()};
    const katana::codegen::CppBackend backend;
    const auto emission = katana::codegen::generate_program(
        backend,
        {functions, entry}
    );

    require(backend.name() == "cpp", "C++-Backend besitzt keine stabile Identitaet.");
    require(
        emission.declarations.find("#include \"katana/runtime/runtime.hpp\"") !=
                std::string::npos &&
            emission.declarations.find(
                "#include \"katana/runtime/platform_services.hpp\""
            ) != std::string::npos &&
            emission.declarations.find("static void fn_8C010000") != std::string::npos &&
            emission.declarations.find("katana/platform/") == std::string::npos &&
            emission.declarations.find("katana/runtime/pvr") == std::string::npos &&
            emission.declarations.find("katana/runtime/aica") == std::string::npos &&
            emission.declarations.find("katana/runtime/maple") == std::string::npos &&
            emission.declarations.find("katana/runtime/gdi") == std::string::npos &&
            emission.declarations.find("void run(CpuState& cpu) {") == std::string::npos,
        "C++-Backend trennt Deklarationen und Funktionskoerper nicht."
    );
    require(
            emission.functions.find("static void fn_8C010000(CpuState& cpu) {") !=
                std::string::npos &&
            emission.functions.find("void run(CpuState& cpu) {") != std::string::npos &&
            emission.functions.find("void run(CpuState& cpu, PlatformServices& services)") !=
                std::string::npos,
        "C++-Backend emittiert die Funktionskoerper nicht im Funktionsabschnitt."
    );
    require(
        emission.metadata.find("generated_entry_address = 0x8C010000u") !=
                std::string::npos &&
            emission.metadata.find("} // namespace katana_generated") != std::string::npos,
        "C++-Backend emittiert keine getrennten stabilen Metadaten."
    );
    require(
        emission.joined_text() == katana::codegen::emit_cpp_program(functions, entry),
        "Kompatibilitaetsfunktion umgeht oder veraendert das C++-Backend."
    );

    std::cout << "KR-3202 C++-Backend-Migration erfolgreich.\n";
    return 0;
}
