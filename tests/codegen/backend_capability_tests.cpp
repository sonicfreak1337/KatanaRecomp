#include "katana/codegen/cpp_emitter.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function>
std::string failure_message(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
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

    katana::codegen::BackendRequirements supported;
    supported.capabilities =
        katana::codegen::capability(katana::codegen::BackendCapability::RuntimeCpuState) |
        katana::codegen::capability(katana::codegen::BackendCapability::Fpu);
    const auto emission = katana::codegen::generate_program(
        backend,
        {functions, entry, supported}
    );
    require(
        !emission.functions.empty(),
        "Unterstuetzte Backend-Faehigkeiten werden faelschlich abgelehnt."
    );

    auto wrong_interface = supported;
    ++wrong_interface.interface_abi_version;
    const auto interface_error = failure_message([&] {
        static_cast<void>(katana::codegen::generate_program(
            backend,
            {functions, entry, wrong_interface}
        ));
    });
    require(
        interface_error.find("cpp") != std::string::npos &&
            interface_error.find("Interface-ABI") != std::string::npos &&
            interface_error.find("erforderlich") != std::string::npos,
        "Interface-ABI-Fehler nennt Backend oder Soll-/Ist-Vertrag nicht."
    );

    auto wrong_runtime = supported;
    ++wrong_runtime.runtime_abi_version;
    const auto runtime_error = failure_message([&] {
        static_cast<void>(katana::codegen::generate_program(
            backend,
            {functions, entry, wrong_runtime}
        ));
    });
    require(
        runtime_error.find("cpp") != std::string::npos &&
            runtime_error.find("Runtime-ABI") != std::string::npos &&
            runtime_error.find("angefordert") != std::string::npos,
        "Runtime-ABI-Fehler nennt Backend oder Soll-/Ist-Vertrag nicht."
    );

    auto unsupported = supported;
    unsupported.capabilities |= 1ull << 63u;
    const auto capability_error = failure_message([&] {
        static_cast<void>(katana::codegen::generate_program(
            backend,
            {functions, entry, unsupported}
        ));
    });
    require(
        capability_error.find("cpp") != std::string::npos &&
        capability_error.find("fehlende Maske 9223372036854775808") != std::string::npos,
        "Faehigkeitsfehler nennt Backend oder stabile fehlende Maske nicht."
    );

    std::cout << "KR-3203 Backend-ABI und Faehigkeitspruefung erfolgreich.\n";
    return 0;
}
