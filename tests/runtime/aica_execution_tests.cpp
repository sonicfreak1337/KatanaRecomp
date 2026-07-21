#include "katana/runtime/aica.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace katana::runtime;
    AicaExecutionController execution;
    require(execution.mode() == AicaArm7Mode::HighLevelAudio &&
                !execution.arm7_executes_instructions(),
            "AICA startet nicht im dokumentierten HLE-Audioprofil.");
    require(throws<std::runtime_error>([&] { execution.set_mode(AicaArm7Mode::LowLevelArm7); }) &&
                execution.mode() == AicaArm7Mode::HighLevelAudio,
            "Nicht implementiertes ARM7-LLE wird still akzeptiert oder veraendert den Modus.");

    execution.timer(0u).configure(254u, 1u, true);
    execution.interrupts().set_enabled(AicaExecutionController::timer_interrupt_base);
    execution.tick(3u);
    require(execution.timer(0u).counter() == 255u && !execution.interrupts().asserted(),
            "AICA-Timer ignoriert Teiler oder loest zu frueh aus.");
    execution.tick(1u);
    require(execution.timer(0u).counter() == 0u && execution.interrupts().asserted() &&
                execution.interrupts().pending() == (1u << 6u),
            "AICA-Timeroverflow erzeugt keinen maskierten Interrupt.");
    execution.interrupts().acknowledge(1u << 6u);
    require(!execution.interrupts().asserted() && execution.interrupts().pending() == 0u,
            "AICA-Timerinterrupt kann nicht quittiert werden.");
    execution.timer(1u).configure(255u, 0u, true);
    execution.tick(1u);
    require(execution.interrupts().pending() == (1u << 7u) && !execution.interrupts().asserted(),
            "Maskierter AICA-Interrupt geht verloren oder wird faelschlich zugestellt.");
    require(throws<std::out_of_range>([&] { static_cast<void>(execution.timer(3u)); }),
            "Ungueltiger AICA-Timerindex wird akzeptiert.");
    require(throws<std::invalid_argument>([] {
                AicaTimer timer;
                timer.configure(0u, 8u, true);
            }),
            "Ungueltiger AICA-Timerteiler wird akzeptiert.");

    std::cout << "KR-2904 ARM7-HLE-Strategie, Timer und Interrupts erfolgreich.\n";
}
