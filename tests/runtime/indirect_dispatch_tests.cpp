#include "katana/runtime/indirect_dispatch.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace katana::runtime;

namespace {
BlockExit block(CpuState&, BlockExecutionContext&) { return {}; }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main() {
    try {
        RuntimeBlockTable table;
        const BlockVariantKey variant{1u, 0u, 0u, 0u, 0u};
        table.register_static({0x8C001000u, 0x0C001000u, 4u, BlockEndKind::Return, variant, block, "compiled", false});
        CpuState cpu;
        cpu.pr = 0xDEADBEEFu;
        const BlockAddress source{0x8C000100u, 0x0C000100u};

        const auto call = dispatch_indirect(cpu, table, {
            IndirectDispatchKind::Call, 0x8C000102u, 0xAC001000u, 0x8C000106u, source, variant
        });
        require(call.block != nullptr && call.alias_lookup && call.physical_target == 0x0C001000u,
            "P2-Alias erreichte den physischen P1-Block nicht.");
        require(cpu.pc == 0xAC001000u && cpu.pr == 0x8C000106u,
            "Call bewahrt PC-/PR-Semantik nicht.");

        const auto jump = dispatch_indirect(cpu, table, {
            IndirectDispatchKind::TailJump, 0x8C000200u, 0x8C001000u, 0u, source, variant
        });
        require(!jump.alias_lookup && cpu.pc == 0x8C001000u && cpu.pr == 0x8C000106u,
            "Tail-Jump veraendert PR oder verfehlt den exakten Lookup.");

        cpu.pr = 0xAC001000u;
        const auto returned = dispatch_indirect(cpu, table, {
            IndirectDispatchKind::Return, 0x8C000300u, 0x11111111u, 0u, source, variant
        });
        require(returned.alias_lookup && cpu.pc == cpu.pr, "Return verwendet nicht PR als dynamisches Ziel.");

        bool failed = false;
        try {
            static_cast<void>(dispatch_indirect(cpu, table, {
                IndirectDispatchKind::TailJump, 0x8C000400u, 0x8C999000u, 0u, source, variant
            }));
        } catch (const IndirectDispatchError& error) {
            const std::string text = error.what();
            failed = text.find("8c000400") != std::string::npos &&
                text.find("8c999000") != std::string::npos && text.find("source=") != std::string::npos;
        }
        require(failed, "Unbekanntes Ziel wurde nicht mit Callsite, Ziel und Quelle abgelehnt.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
