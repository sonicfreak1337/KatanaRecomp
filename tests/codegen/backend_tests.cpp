#include "katana/codegen/backend.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Function>
bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
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

katana::ir::Function program_function() {
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

class RecordingBackend : public katana::codegen::Backend {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "recording";
    }
    [[nodiscard]] std::uint32_t interface_abi_version() const noexcept override {
        return katana::codegen::backend_interface_abi_version;
    }
    [[nodiscard]] std::uint32_t runtime_abi_version() const noexcept override {
        return katana::runtime::abi_version;
    }
    [[nodiscard]] katana::codegen::BackendCapabilities capabilities() const noexcept override {
        return katana::codegen::capability(
            katana::codegen::BackendCapability::StructuredSections
        );
    }

    [[nodiscard]] katana::codegen::BackendEmission emit(
        const katana::codegen::BackendRequest& request
    ) const override {
        observed_entry = request.entry_address;
        observed_functions = request.functions.size();
        return {"declarations\n", "functions\n", "metadata\n"};
    }

    mutable std::uint32_t observed_entry = 0u;
    mutable std::size_t observed_functions = 0u;
};

class EmptyNameBackend final : public RecordingBackend {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return {}; }
};

class IncompleteBackend final : public RecordingBackend {
public:
    [[nodiscard]] katana::codegen::BackendEmission emit(
        const katana::codegen::BackendRequest&
    ) const override {
        return {"declarations\n", {}, {}};
    }
};

} // namespace

int main() {
    constexpr std::uint32_t entry = 0x8C010000u;
    const std::array functions = {program_function()};
    const katana::codegen::BackendRequest request{functions, entry};
    RecordingBackend backend;
    const auto emission = katana::codegen::generate_program(backend, request);

    require(
        backend.observed_entry == entry && backend.observed_functions == 1u,
        "Backend erhaelt nicht das validierte IR-Programm und den Einstieg."
    );
    require(
        emission.joined_text() == "declarations\nfunctions\nmetadata\n",
        "Backendabschnitte verlieren ihre deterministische Reihenfolge."
    );
    require(
        throws<std::invalid_argument>([&] {
            const katana::codegen::BackendRequest empty{
                std::span<const katana::ir::Function>{},
                entry
            };
            static_cast<void>(katana::codegen::generate_program(
                backend,
                empty
            ));
        }) &&
        throws<std::invalid_argument>([&] {
            static_cast<void>(katana::codegen::generate_program(
                backend,
                {functions, entry + 4u}
            ));
        }) &&
        throws<std::invalid_argument>([&] {
            EmptyNameBackend unnamed;
            static_cast<void>(katana::codegen::generate_program(unnamed, request));
        }),
        "Backendgrenze akzeptiert leere Programme, unbekannte Einstiege oder namenlose Backends."
    );
    require(
        throws<std::runtime_error>([&] {
            IncompleteBackend incomplete;
            static_cast<void>(katana::codegen::generate_program(incomplete, request));
        }),
        "Backendgrenze akzeptiert unvollstaendige Emissionen."
    );

    std::cout << "KR-3201 Modulares Backend-Interface erfolgreich.\n";
    return 0;
}
