#include "katana/codegen/port_export.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Callable>
void require_invalid(Callable&& callable,
                     const std::string_view message) {
    try {
        callable();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

} // namespace

int main() {
    try {
        using katana::codegen::resolve_port_codegen_jobs;
        require(
            resolve_port_codegen_jobs(100u, 24u, "12", std::nullopt) ==
                12u,
            "Primary-only Codegenbudget wurde nicht autoritativ.");
        require(
            resolve_port_codegen_jobs(100u, 24u, std::nullopt, "8") ==
                8u,
            "Legacy-only Codegencap wurde nicht angewendet.");
        require(
            resolve_port_codegen_jobs(100u, 24u, "20", "7") == 7u,
            "Primary und Legacy wurden nicht konservativ vereinigt.");
        require(
            resolve_port_codegen_jobs(0u, 24u, "20", "7") == 0u,
            "Null Partitionen erzeugen angebliche Worker.");
        require_invalid(
            [&] {
                static_cast<void>(resolve_port_codegen_jobs(
                    1u,
                    24u,
                    "184467440737095516160",
                    std::nullopt));
            },
            "Codegenbudget-Overflow wurde akzeptiert.");
        require_invalid(
            [&] {
                static_cast<void>(resolve_port_codegen_jobs(
                    1u, 24u, "257", std::nullopt));
            },
            "Codegenbudget oberhalb des Prozesslimits wurde akzeptiert.");
        std::cout << "Port-Codegen-Jobvertrag erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
