#include "katana/runtime/block_dispatch.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace katana::runtime;
namespace {
BlockExit host_block(CpuState&, BlockExecutionContext&) {
    return {};
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        RuntimeBlockTable table;
        const BlockVariantKey variant{};
        for (const auto address : {0x8C001000u, 0x8C002000u, 0x8C003000u}) {
            static_cast<void>(table.register_static({address,
                                   canonical_physical_address(address),
                                   4u,
                                   BlockEndKind::Return,
                                   variant,
                                   host_block,
                                   "block-" + std::to_string(address),
                                   false}));
        }
        CanonicalBlockDispatcher dispatcher(table);
        CpuState cpu;
        BlockExecutionContext context{};
        const BlockAddress source{0x8C000100u, 0x0C000100u};
        const BlockAddress taken{0x8C001000u, 0x0C001000u};
        const BlockAddress fallthrough{0x8C002000u, 0x0C002000u};

        require(
            dispatcher
                    .dispatch(
                        cpu, context, variant, {BlockEndKind::Fallthrough, source, {}, fallthrough})
                    .target_block.has_value(),
            "Fallthrough besitzt keinen Ausfuehrungspfad.");
        require(dispatcher
                        .dispatch(cpu,
                                  context,
                                  variant,
                                  {BlockEndKind::StaticBranch, source, {taken}, std::nullopt})
                        .exit.target == taken,
                "Statischer Block waehlt sein Ziel nicht.");
        require(
            dispatcher.dispatch(cpu,
                                context,
                                variant,
                                {BlockEndKind::ConditionalBranch, source, {taken}, fallthrough},
                                true)
                        .exit.target == taken &&
                dispatcher
                        .dispatch(cpu,
                                  context,
                                  variant,
                                  {BlockEndKind::ConditionalBranch, source, {taken}, fallthrough},
                                  false)
                        .exit.target == fallthrough,
            "Bedingter Block waehlt nicht exakt einen dokumentierten Nachfolger.");

        require(dispatcher
                        .dispatch(cpu,
                                  context,
                                  variant,
                                  {BlockEndKind::DynamicBranch, source, {}, std::nullopt},
                                  false,
                                  0x8C003000u)
                        .target_block.has_value(),
                "Dynamischer Sprung besitzt keinen generischen Lookup.");
        static_cast<void>(dispatcher.dispatch(cpu,
                                              context,
                                              variant,
                                              {BlockEndKind::Call, source, {}, std::nullopt},
                                              false,
                                              0x8C003000u));
        require(cpu.pr == source.virtual_address + 4u, "Dynamischer Call setzt PR falsch.");
        cpu.pr = 0x8C002000u;
        const auto returned = dispatcher.dispatch(
            cpu, context, variant, {BlockEndKind::Return, source, {}, std::nullopt});
        require(returned.exit.kind == BlockEndKind::Return && cpu.pc == 0x8C002000u,
                "Return verwendet PR nicht als eigenen dynamischen Endtyp.");
        require(
            !dispatcher
                    .dispatch(
                        cpu, context, variant, {BlockEndKind::Exception, source, {}, std::nullopt})
                    .target_block &&
                !dispatcher
                     .dispatch(cpu,
                               context,
                               variant,
                               {BlockEndKind::InterruptSafepoint, source, {}, std::nullopt})
                     .target_block,
            "Exception oder Interrupt-Safepoint erfindet ein Ziel.");

        dispatcher.link("caller-b", "target");
        dispatcher.link("caller-a", "target");
        const auto unlinked = dispatcher.unlink_target("target");
        require(unlinked == std::vector<std::string>({"caller-a", "caller-b"}) &&
                    dispatcher.incoming_link_count("target") == 0u,
                "Unlink entfernt eingehende Direktlinks nicht stabil und vollstaendig.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
