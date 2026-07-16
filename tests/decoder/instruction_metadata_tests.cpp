#include "katana/sh4/decoder.hpp"
#include "katana/sh4/instruction_metadata.hpp"

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

} // namespace

int main() {
    using namespace katana::sh4;

    const auto instructions = instruction_metadata();
    const auto special_registers = special_register_encoding_metadata();

    require(instructions.size() == 146u, "Die normale Metadatentabelle ist unvollstaendig.");
    require(special_registers.size() == 78u, "Die Systemregistertabelle ist unvollstaendig.");

    for (const auto& metadata : instructions) {
        require(metadata.matches(metadata.pattern), "Eine Regel erkennt ihr eigenes Muster nicht.");
        const auto decoded = decode(metadata.pattern);
        require(decoded.kind == metadata.kind,
                "Decoder und Metadaten widersprechen sich bei " + std::string(metadata.name) + ".");
        require(decoded.control_flow == metadata.control_flow,
                "Kontrollflussmetadaten weichen fuer " + std::string(metadata.name) + " ab.");
        require(decoded.has_delay_slot == metadata.has_delay_slot,
                "Delay-Slot-Metadaten weichen fuer " + std::string(metadata.name) + " ab.");
        require(decoded.is_privileged == metadata.is_privileged,
                "Privilegstatus weicht fuer " + std::string(metadata.name) + " ab.");
        require(metadata_for_kind(metadata.kind) == &metadata,
                "Kind-Lookup liefert nicht den kanonischen Eintrag.");
    }

    for (const auto& metadata : special_registers) {
        require(metadata.matches(metadata.pattern),
                "Eine Systemregisterregel erkennt ihr Muster nicht.");
        const auto decoded = decode(metadata.pattern);
        require(decoded.kind == metadata.kind,
                "Systemregister-Kind stimmt nicht mit der Metadatenquelle ueberein.");
        require(decoded.special_register == metadata.special_register,
                "Systemregisteroperand stimmt nicht mit der Metadatenquelle ueberein.");
        require(decoded.is_privileged == metadata.is_privileged,
                "Systemregister-Privilegstatus stimmt nicht mit der Metadatenquelle ueberein.");
    }

    require(metadata_for_kind(InstructionKind::Unknown) == nullptr,
            "Unknown darf keine normale Decoderregel besitzen.");

    std::cout << "KR-1501 Instruktionsmetadaten erfolgreich.\n";
    return EXIT_SUCCESS;
}
