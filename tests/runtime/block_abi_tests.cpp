#include "katana/runtime/block_abi.hpp"

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
bool throws_invalid(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

using katana::runtime::BlockAddress;
using katana::runtime::BlockEndKind;

katana::runtime::BlockExit backend_a(
    katana::runtime::CpuState& cpu,
    katana::runtime::BlockExecutionContext& context
) {
    cpu.write_sr(0x600000F1u);
    cpu.write_fpscr(0x002C0001u);
    cpu.r[0] = 0x01020304u;
    cpu.r_bank[7] = 0x11223344u;
    cpu.fr[3] = 0x7FC00000u;
    cpu.xf[15] = 0x3F800000u;
    cpu.pr = 0x8C001234u;
    cpu.ssr = 0x400000F0u;
    cpu.spc = 0x8C002000u;
    cpu.sleeping = true;
    cpu.trap_pending = true;
    cpu.last_exception_cause = katana::runtime::ExceptionCause::AddressErrorRead;
    context.scheduler_cycle = 987654321u;
    context.scheduler_event_budget = 17u;
    return katana::runtime::make_block_exit(
        cpu,
        context,
        BlockEndKind::StaticBranch,
        {0x8C000100u, 0x0C000100u},
        BlockAddress{0x8C000200u, 0x0C000200u}
    );
}

void backend_b(
    const katana::runtime::CpuState& cpu,
    const katana::runtime::BlockExecutionContext& context
) {
    require(
        cpu.r[0] == 0x01020304u && cpu.r_bank[7] == 0x11223344u &&
            cpu.fr[3] == 0x7FC00000u && cpu.xf[15] == 0x3F800000u &&
            cpu.pr == 0x8C001234u && cpu.read_sr() == 0x600000F1u &&
            cpu.read_fpscr() == 0x002C0001u && cpu.ssr == 0x400000F0u &&
            cpu.spc == 0x8C002000u && cpu.sleeping && cpu.trap_pending &&
            cpu.last_exception_cause == katana::runtime::ExceptionCause::AddressErrorRead &&
            context.scheduler_cycle == 987654321u && context.scheduler_event_budget == 17u,
        "Backendwechsel verliert beobachtbaren CPU- oder Schedulerzustand."
    );
}

} // namespace

int main() {
    using namespace katana::runtime;

    CpuState cpu;
    cpu.pc = 0x8C000100u;
    BlockExecutionContext context;
    const BlockEntry entry{{0x8C000100u, 0x0C000100u}};
    validate_block_entry(cpu, context, entry);

    const auto first_exit = backend_a(cpu, context);
    backend_b(cpu, context);
    require(
        first_exit.source.virtual_address != first_exit.source.physical_address &&
            stable_block_identity(first_exit.source) == "v8C000100-p0C000100",
        "Virtuelle und physische Gastidentitaet sind nicht stabil getrennt."
    );

    constexpr std::array target_kinds = {
        BlockEndKind::Fallthrough,
        BlockEndKind::StaticBranch,
        BlockEndKind::ConditionalBranch,
        BlockEndKind::DynamicBranch,
        BlockEndKind::Call,
        BlockEndKind::Return
    };
    for (const auto kind : target_kinds) {
        context.sync_point = BlockSyncPoint::BackendBoundary;
        const auto exit = make_block_exit(
            cpu, context, kind, entry.address, BlockAddress{0x8C000300u, 0x0C000300u}
        );
        require(exit.kind == kind && exit.target.has_value(), "Blockendtyp verliert sein Ziel.");
    }
    constexpr std::array terminal_kinds = {
        BlockEndKind::Exception,
        BlockEndKind::InterruptSafepoint
    };
    for (const auto kind : terminal_kinds) {
        context.sync_point = BlockSyncPoint::FallbackBoundary;
        const auto exit = make_block_exit(cpu, context, kind, entry.address);
        require(exit.kind == kind, "Zieloser Blockendtyp wird nicht uebertragen.");
    }

    cpu.pc = 0x8C000102u;
    context.delay_slot_owner_pc = 0x8C000100u;
    context.sync_point = BlockSyncPoint::BackendBoundary;
    const auto delayed = make_block_exit(cpu, context, BlockEndKind::Exception, entry.address);
    require(
        delayed.in_delay_slot && delayed.exception_owner_pc == 0x8C000100u,
        "Delay-Slot-Ausnahme meldet Slot-PC statt Owner-PC."
    );

    context.sync_point = BlockSyncPoint::Entry;
    require(
        throws_invalid([&] { validate_block_entry(cpu, context, entry); }) &&
            throws_invalid([&] {
                static_cast<void>(make_block_exit(
                    cpu, context, BlockEndKind::Call, entry.address
                ));
            }),
        "Unsynchronisierter PC oder fehlendes Blockziel wird akzeptiert."
    );

    std::cout << "KR-3204 Block-ABI und Zustandsuebergaben erfolgreich.\n";
    return 0;
}
