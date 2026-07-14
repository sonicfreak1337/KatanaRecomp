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
    using katana::ir::OperandWidth;
    using katana::ir::Operation;
    using katana::ir::operation_operand_widths;

    const auto immediate = operation_operand_widths(Operation::MovImmediate);
    require(
        immediate.result == OperandWidth::Bits32
            && immediate.input == OperandWidth::None
            && immediate.immediate == OperandWidth::Bits8,
        "MOV Immediate besitzt keine explizite Register- und Immediate-Breite."
    );

    const auto extension = operation_operand_widths(Operation::ExtendSignedByte);
    require(
        extension.result == OperandWidth::Bits32
            && extension.input == OperandWidth::Bits8,
        "Byte-Erweiterung unterscheidet Quell- und Zielbreite nicht."
    );

    const auto load = operation_operand_widths(Operation::LoadByteSigned);
    require(
        load.result == OperandWidth::Bits32
            && load.input == OperandWidth::None
            && load.memory == OperandWidth::Bits8
            && load.address == OperandWidth::Bits32,
        "Byte-Load besitzt keine getrennte Speicher- und Registerbreite."
    );

    const auto displacement = operation_operand_widths(Operation::StoreLongDisplacement);
    require(
        displacement.result == OperandWidth::None
            && displacement.input == OperandWidth::Bits32
            && displacement.displacement == OperandWidth::Bits4
            && displacement.memory == OperandWidth::Bits32
            && displacement.address == OperandWidth::Bits32,
        "Register-Displacement-Store besitzt falsche Breiten."
    );

    const auto comparison = operation_operand_widths(Operation::CompareGreaterThan);
    require(
        comparison.result == OperandWidth::Bit1
            && comparison.input == OperandWidth::Bits32,
        "Vergleich trennt boolesche Ergebnis- und Eingabebreite nicht."
    );

    const auto branch = operation_operand_widths(Operation::Branch);
    require(
        branch.displacement == OperandWidth::Bits12
            && branch.address == OperandWidth::Bits32,
        "Direkter Branch besitzt keine explizite Displacement- oder Adressbreite."
    );

    const auto conditional = operation_operand_widths(Operation::BranchIfTrue);
    require(
        conditional.input == OperandWidth::Bit1
            && conditional.displacement == OperandWidth::Bits8,
        "Bedingter Branch besitzt falsche T- oder Displacement-Breite."
    );

    const auto multiply = operation_operand_widths(Operation::DoubleMultiplySignedLong);
    require(
        multiply.result == OperandWidth::Bits64
            && multiply.input == OperandWidth::Bits32,
        "Doppelte Multiplikation bildet ihre Ergebnisbreite nicht ab."
    );

    const auto nop = operation_operand_widths(Operation::Nop);
    require(
        nop.result == OperandWidth::None
            && nop.input == OperandWidth::None
            && nop.memory == OperandWidth::None,
        "Operandlose Operation besitzt erfundene Breiten."
    );

    require(
        katana::ir::operand_width_name(OperandWidth::Bits32) == "i32"
            && katana::ir::operand_width_name(OperandWidth::None) == "none",
        "Textnamen der Operandbreiten sind instabil."
    );

    std::cout << "KR-1901 Explizite Operandbreiten erfolgreich.\n";
    return EXIT_SUCCESS;
}
