#include "katana/codegen/backend.hpp"

#include "katana/ir/verifier.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace katana::codegen {

std::string BackendEmission::joined_text() const {
    std::string result;
    result.reserve(declarations.size() + functions.size() + metadata.size());
    result += declarations;
    result += functions;
    result += metadata;
    return result;
}

BackendEmission generate_program(
    const Backend& backend,
    const BackendRequest& request
) {
    if (backend.name().empty()) {
        throw std::invalid_argument("Codegen-Backend besitzt keinen Namen.");
    }
    if (backend.interface_abi_version() != request.requirements.interface_abi_version) {
        throw std::invalid_argument(
            "Codegen-Backend " + std::string(backend.name()) +
            " besitzt Interface-ABI " +
            std::to_string(backend.interface_abi_version()) +
            ", erforderlich ist " +
            std::to_string(request.requirements.interface_abi_version) + "."
        );
    }
    if (backend.runtime_abi_version() != request.requirements.runtime_abi_version) {
        throw std::invalid_argument(
            "Codegen-Backend " + std::string(backend.name()) +
            " erwartet Runtime-ABI " +
            std::to_string(backend.runtime_abi_version()) +
            ", angefordert ist " +
            std::to_string(request.requirements.runtime_abi_version) + "."
        );
    }
    const auto missing_capabilities =
        request.requirements.capabilities & ~backend.capabilities();
    if (missing_capabilities != 0u) {
        throw std::invalid_argument(
            "Codegen-Backend " + std::string(backend.name()) +
            " besitzt nicht alle erforderlichen Faehigkeiten; fehlende Maske " +
            std::to_string(missing_capabilities) + "."
        );
    }
    if (request.functions.empty()) {
        throw std::invalid_argument("Es wurden keine IR-Funktionen uebergeben.");
    }

    katana::ir::require_valid_program(request.functions);
    const auto entry = std::ranges::find(
        request.functions,
        request.entry_address,
        &katana::ir::Function::entry_address
    );
    if (entry == request.functions.end()) {
        throw std::invalid_argument(
            "Die Einstiegsfunktion ist im IR-Programm nicht vorhanden."
        );
    }

    auto emission = backend.emit(request);
    if (emission.declarations.empty() || emission.functions.empty()) {
        throw std::runtime_error(
            "Codegen-Backend lieferte keinen Deklarations- oder Funktionsabschnitt."
        );
    }
    return emission;
}

} // namespace katana::codegen
