#include "katana/ir/ir.hpp"

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

}

int main() {
    using katana::ir::Operation;
    using katana::ir::SpecialRegister;
    using katana::ir::StatusRegisterBit;
    using katana::ir::contains_status_bit;
    using katana::ir::instruction_status_effects;

    const auto carry = instruction_status_effects(Operation::AddWithCarry);
    require(
        contains_status_bit(carry.reads, StatusRegisterBit::T)
            && contains_status_bit(carry.writes, StatusRegisterBit::T),
        "ADDC muss T explizit lesen und schreiben."
    );

    const auto comparison = instruction_status_effects(Operation::CompareGreaterThan);
    require(
        comparison.reads == StatusRegisterBit::None
            && contains_status_bit(comparison.writes, StatusRegisterBit::T),
        "Vergleich muss T ausschliesslich schreiben."
    );

    const auto divide = instruction_status_effects(Operation::DivideStep);
    require(
        contains_status_bit(divide.reads, StatusRegisterBit::T)
            && contains_status_bit(divide.reads, StatusRegisterBit::Q)
            && contains_status_bit(divide.reads, StatusRegisterBit::M)
            && contains_status_bit(divide.writes, StatusRegisterBit::T)
            && contains_status_bit(divide.writes, StatusRegisterBit::Q)
            && !contains_status_bit(divide.writes, StatusRegisterBit::M),
        "DIV1 besitzt unvollstaendige Statusregistereffekte."
    );

    const auto stc_sr = instruction_status_effects(
        Operation::StoreSpecialRegister,
        SpecialRegister::Sr
    );
    const auto stc_gbr = instruction_status_effects(
        Operation::StoreSpecialRegister,
        SpecialRegister::Gbr
    );
    require(
        contains_status_bit(stc_sr.reads, StatusRegisterBit::Full)
            && stc_gbr.reads == StatusRegisterBit::None,
        "STC muss SR- und Nicht-SR-Transfers unterscheiden."
    );

    const auto load = instruction_status_effects(Operation::LoadLong);
    require(
        load.reads == StatusRegisterBit::None
            && load.writes == StatusRegisterBit::None,
        "Statusneutrale Operation besitzt erfundene Effekte."
    );

    std::cout << "KR-1902 Statusregistereffekte erfolgreich.\n";
    return EXIT_SUCCESS;
}
