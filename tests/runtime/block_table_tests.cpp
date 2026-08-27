#include "katana/runtime/block_table.hpp"
#include "katana/runtime/code_invalidation.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace katana::runtime;

static_assert(canonical_physical_address_inline(0x00000000u) == 0x00000000u &&
              canonical_physical_address_inline(0x1FFFFFFFu) == 0x1FFFFFFFu &&
              canonical_physical_address_inline(0x20000000u) == 0x00000000u &&
              canonical_physical_address_inline(0x7BFFFFFFu) == 0x1BFFFFFFu &&
              canonical_physical_address_inline(0x7C000000u) == 0x7C000000u &&
              canonical_physical_address_inline(0x7FFFFFFFu) == 0x7FFFFFFFu &&
              canonical_physical_address_inline(0x80000000u) == 0x00000000u &&
              canonical_physical_address_inline(0xDFFFFFFFu) == 0x1FFFFFFFu &&
              canonical_physical_address_inline(0xE0000000u) == 0xE0000000u &&
              canonical_physical_address_inline(0xFFFFFFFFu) == 0xFFFFFFFFu);

BlockExit block_a(CpuState&, BlockExecutionContext&) {
    return {};
}
BlockExit block_b(CpuState&, BlockExecutionContext&) {
    return {};
}
BlockExit relocated_block(CpuState& cpu, BlockExecutionContext& context) {
    return make_block_exit(
        cpu,
        context,
        BlockEndKind::Fallthrough,
        {relocate_code_address(0x8C500010u), canonical_physical_address(0x8C500010u)},
        BlockAddress{unrelocate_code_address(0xAC200020u), 0u});
}
BlockExit throwing_block(CpuState&, BlockExecutionContext&) {
    throw std::runtime_error("expected-backend-throw");
}

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const RuntimeBlock& resolved(const RuntimeBlockTable& table,
                             const std::optional<RuntimeBlockHandle> handle) {
    require(handle.has_value(), "Blockhandle fehlt.");
    const auto block = table.resolve(*handle);
    require(block.has_value(), "Blockhandle ist unerwartet stale.");
    return block->get();
}

template <typename Function>
bool throws_with(Function function, const std::string& first, const std::string& second) {
    try {
        function();
    } catch (const std::exception& error) {
        const std::string text = error.what();
        return text.find(first) != std::string::npos && text.find(second) != std::string::npos;
    }
    return false;
}

template <typename Function> bool throws_any(Function function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    try {
        RuntimeBlockTable static_fast;
        static_fast.bind_code_tracker(
            nullptr, StaticAotInvalidationContract::Coordinated);
        RuntimeBlock static_fast_block{0x8C001000u,
                                       0x0C001000u,
                                       8u,
                                       BlockEndKind::StaticBranch,
                                       {},
                                       block_a,
                                       "static-fast",
                                       false};
        static_fast_block.static_variant_policy =
            StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic;
        static_cast<void>(static_fast.register_static(std::move(static_fast_block)));
        static_fast.seal_static();
        const BlockVariantKey boot_variant{0u, 0u, 0u, 1u, 0u};
        const auto p2_fast =
            static_fast.lookup_static_aot(0x0C001000u, 0xAC001000u, boot_variant);
        require(p2_fast && p2_fast->function == block_a &&
                    p2_fast->virtual_start == 0x8C001000u &&
                    p2_fast->variant == boot_variant &&
                    static_fast.static_dispatch_generation_guard_current(
                        p2_fast->generation_guard),
                "Variantneutraler Static-AOT-Tier verliert Bootvariante oder P2-Alias.");
        require(!static_fast.lookup_static_aot(
                    0x0C001000u, 0xAC002000u, boot_variant),
                "Static-AOT-Tier akzeptiert ein virtuell/physisch widerspruechliches Ziel.");
        static_cast<void>(static_fast.register_runtime(
            {0xAC001100u,
             0x0C001100u,
             4u,
             BlockEndKind::Return,
             boot_variant,
             block_b,
             "dynamic-same-page-other-halfword",
             false}));
        require(static_fast.lookup_static_aot(
                    0x0C001000u, 0x8C001000u, boot_variant) &&
                    static_fast.static_dispatch_generation_guard_current(
                        p2_fast->generation_guard),
                "Dynamischer Nachbar sperrt oder invalidiert einen anderen "
                "Static-AOT-Halfword-Eintrag.");
        auto foreign_runtime_variant = boot_variant;
        ++foreign_runtime_variant.runtime_generation;
        static_cast<void>(static_fast.register_runtime(
            {0xAC001000u,
             0x0C001000u,
             4u,
             BlockEndKind::Return,
             foreign_runtime_variant,
             block_b,
             "dynamic-same-halfword-foreign-variant",
             false}));
        const auto refreshed_static = static_fast.lookup_static_aot(
            0x0C001000u, 0x8C001000u, boot_variant);
        require(refreshed_static &&
                    !static_fast.static_dispatch_generation_guard_current(
                        p2_fast->generation_guard),
                "Fremde Dynamic-Variante schattet Static-AOT oder laesst den "
                "alten Eintragsguard bestehen.");
        static_cast<void>(static_fast.register_runtime(
            {0xAC001000u,
             0x0C001000u,
             4u,
             BlockEndKind::Return,
             boot_variant,
             block_b,
             "dynamic-same-halfword-live-variant",
             false}));
        require(!static_fast.lookup_static_aot(
                    0x0C001000u, 0x8C001000u, boot_variant),
                "Passende Dynamic-Variante wird vom Static-AOT-Tier ueberschattet.");

        require(direct_p1_p2_block_binding_contiguous(
                    0x9FFFFFFCu, 0x1FFFFFFCu, 4u) &&
                    !direct_p1_p2_block_binding_contiguous(
                        0x9FFFFFFCu, 0x1FFFFFFCu, 6u),
                "Direct-P1/P2-Bindung prueft nur den Einstieg statt das letzte "
                "Blockhalfword.");
        RuntimeBlockTable alias_boundary;
        RuntimeBlock crossing_alias_boundary{0x9FFFFFFCu,
                                             0x1FFFFFFCu,
                                             6u,
                                             BlockEndKind::Return,
                                             {},
                                             block_a,
                                             "crossing-direct-alias-boundary",
                                             false};
        crossing_alias_boundary.static_variant_policy =
            StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic;
        require(throws_any([&] {
                    static_cast<void>(alias_boundary.register_static(
                        std::move(crossing_alias_boundary)));
                }),
                "Static-AOT-Registrierung akzeptiert einen Block ueber die "
                "P1/P2-Aliasgrenze.");

        RuntimeBlockTable table;
        const BlockVariantKey base{1u, 2u, 3u, 4u, 5u};
        static_cast<void>(table.register_static({0x8C001000u,
                                                 0x0C001000u,
                                                 8u,
                                                 BlockEndKind::StaticBranch,
                                                 base,
                                                 block_a,
                                                 "rom-reset",
                                                 false}));
        static_cast<void>(table.register_runtime({0xAC001000u,
                                                  0x0C001000u,
                                                  8u,
                                                  BlockEndKind::Return,
                                                  base,
                                                  block_b,
                                                  "ram-copy",
                                                  false}));
        require(table.size() == 2u,
                "Statische und dynamische Eintraege teilen nicht dieselbe Tabelle.");
        require(resolved(table, table.lookup(0x8C001000u, base)).function == block_a,
                "Deterministischer Lookup schlug fehl.");
        auto mmu_alias_variant = base;
        ++mmu_alias_variant.mmu_generation;
        const auto mmu_alias =
            table.register_static_variant(0x00123000u, 0x0C001000u, base, mmu_alias_variant);
        require(mmu_alias.has_value() && resolved(table, mmu_alias).virtual_start == 0x00123000u &&
                    resolved(table, mmu_alias).physical_origin == 0x0C001000u &&
                    resolved(table, mmu_alias).variant == mmu_alias_variant &&
                    resolved(table, mmu_alias).function == block_a && table.size() == 3u,
                "AOT-MMU-Variante verliert neue virtuelle Adresse oder physische Herkunft.");
        require(resolved(table, table.lookup(0xAC001000u, base)).runtime_registered,
                "Laufzeitprovenienz ging verloren.");
        const auto direct_handle = table.lookup(0x8C001000u, base);
        table.set_lookup_mode(RuntimeBlockLookupMode::ReferenceTree);
        const auto reference_handle = table.lookup(0x8C001000u, base);
        require(direct_handle == reference_handle && table.lookup_counters().direct_probes != 0u &&
                    table.lookup_counters().reference_probes != 0u,
                "Direkter Dispatchindex und deaktivierbarer Referenzbaum divergieren.");
        table.set_lookup_mode(RuntimeBlockLookupMode::Direct);
        const auto aliases = table.aliases(0xAC001000u);
        require(aliases.size() == 3u &&
                    table.resolve(aliases[0])->get().virtual_start !=
                        table.resolve(aliases[1])->get().virtual_start &&
                    table.resolve(aliases[0])->get().virtual_start !=
                        table.resolve(aliases[2])->get().virtual_start &&
                    table.resolve(aliases[1])->get().virtual_start !=
                        table.resolve(aliases[2])->get().virtual_start,
                "P1-/P2-/MMU-Aliase bewahren ihre virtuellen Diagnosen nicht.");

        RuntimeBlock same_identity = resolved(table, table.lookup(0x8C001000u, base));
        same_identity.function = block_b;
        require(stable_runtime_block_identity(resolved(table, table.lookup(0x8C001000u, base))) ==
                    stable_runtime_block_identity(same_identity),
                "Hostfunktionszeiger beeinflusst die stabile Blockidentitaet.");

        require(throws_with(
                    [&] {
                        static_cast<void>(table.register_runtime({0x8C001004u,
                                                                  0x0C002000u,
                                                                  8u,
                                                                  BlockEndKind::Call,
                                                                  base,
                                                                  block_b,
                                                                  "overlap",
                                                                  false}));
                    },
                    "rom-reset",
                    "overlap"),
                "Ueberlappung nennt nicht beide Provenienzen.");

        RuntimeBlockTable contextual;
        std::vector<RuntimeBlock> contextual_blocks{{0x1000u,
                                                     0x1000u,
                                                     4u,
                                                     BlockEndKind::ConditionalBranch,
                                                     {},
                                                     block_a,
                                                     "delay-owner",
                                                     false},
                                                    {0x1002u,
                                                     0x1002u,
                                                     4u,
                                                     BlockEndKind::Fallthrough,
                                                     {},
                                                     block_b,
                                                     "normal-slot-entry",
                                                     false}};
        const auto contextual_handles =
            contextual.register_static_contextual_bulk(std::move(contextual_blocks));
        require(contextual_handles.size() == 2u && contextual.lookup(0x1000u, {}).has_value() &&
                    contextual.lookup(0x1002u, {}).has_value() &&
                    contextual.erase_overlapping_physical(0x1002u, 1u) == 2u &&
                    !contextual.resolve(contextual_handles[0]) &&
                    !contextual.resolve(contextual_handles[1]),
                "Kontextuelle Delay-Slot-Ueberlappung verliert Dispatch oder Invalidierung.");
        RuntimeBlockTable hidden_contextual_overlap;
        static_cast<void>(hidden_contextual_overlap.register_static_contextual_bulk(
            {{0x3000u,
              0x3000u,
              0x100u,
              BlockEndKind::ConditionalBranch,
              {},
              block_a,
              "wide-context-owner",
              false},
             {0x3080u,
              0x3080u,
              2u,
              BlockEndKind::Fallthrough,
              {},
              block_b,
              "narrow-context-entry",
              false}}));
        require(throws_with(
                    [&] {
                        static_cast<void>(
                            hidden_contextual_overlap.register_runtime({0x3084u,
                                                                        0x4000u,
                                                                        2u,
                                                                        BlockEndKind::Fallthrough,
                                                                        {},
                                                                        block_b,
                                                                        "hidden-runtime-overlap",
                                                                        false}));
                    },
                    "wide-context-owner",
                    "hidden-runtime-overlap"),
                "Regulaere Registrierung uebersah ein verdecktes kontextuelles Intervall.");
        RuntimeBlockTable incompatible_contextual;
        require(throws_with(
                    [&] {
                        static_cast<void>(incompatible_contextual.register_static_contextual_bulk(
                            {{0x2000u,
                              0x2000u,
                              4u,
                              BlockEndKind::ConditionalBranch,
                              {},
                              block_a,
                              "owner-map",
                              false},
                             {0x2002u,
                              0x3000u,
                              2u,
                              BlockEndKind::Fallthrough,
                              {},
                              block_b,
                              "conflicting-map",
                              false}}));
                    },
                    "owner-map",
                    "conflicting-map"),
                "Kontextuelle Ueberlappung akzeptiert widerspruechliche physische Bytes.");

        auto other_variant = base;
        ++other_variant.mmu_generation;
        static_cast<void>(table.register_runtime({0x8C001000u,
                                                  0x0D001000u,
                                                  8u,
                                                  BlockEndKind::Return,
                                                  other_variant,
                                                  block_b,
                                                  "tlb-remap",
                                                  false}));
        require(resolved(table, table.lookup(0x8C001000u, other_variant)).physical_origin ==
                    0x0D001000u,
                "Blockvarianten koennen keine geaenderte physische Herkunft tragen.");
        const auto dynamic_handle = table.lookup(0xAC001000u, base);
        const auto dynamic_block = resolved(table, dynamic_handle);
        const auto identity = stable_runtime_block_identity(dynamic_block);
        require(table.erase_identity(identity) && !table.lookup(0xAC001000u, base).has_value() &&
                    table.size() == 3u && !table.erase_identity(identity),
                "Gezielte Blockinvalidierung entfernt nicht genau die stabile Identitaet.");
        require(!table.resolve(*dynamic_handle), "Erase liess ein altes Handle aktiv.");
        const auto reactivated = table.register_runtime(dynamic_block);
        require(reactivated.id == dynamic_handle->id &&
                    reactivated.generation != dynamic_handle->generation &&
                    table.resolve(reactivated).has_value() && !table.resolve(*dynamic_handle),
                "Dynamische Reaktivierung recycelt Generation oder Record-ID falsch.");
        table.mark_rejected(0xAC00F000u, base);
        const auto table_snapshot = table.snapshot();
        const auto reactivated_record =
            std::find_if(table_snapshot.records.begin(),
                         table_snapshot.records.end(),
                         [&](const auto& record) { return record.handle == reactivated; });
        require(reactivated_record != table_snapshot.records.end() && reactivated_record->active &&
                    reactivated_record->runtime_registered &&
                    table_snapshot.next_id > reactivated.id &&
                    table_snapshot.rejected ==
                        std::vector<RuntimeBlockRejectionSnapshot>{{0xAC00F000u, base, 1u}},
                "Blocktabellen-Snapshot verliert Runtimegeneration oder Rejection-FSM.");

        const auto retained = table.lookup(0x8C001000u, base);
        for (std::uint32_t index = 0u; index < 10'000u; ++index) {
            const auto address = 0x20000000u + index * 4u;
            static_cast<void>(table.register_runtime({address,
                                                      address,
                                                      4u,
                                                      BlockEndKind::Fallthrough,
                                                      {},
                                                      block_a,
                                                      "growth-" + std::to_string(index),
                                                      false}));
        }
        require(retained.has_value() && table.resolve(*retained).has_value() &&
                    table.resolve(*retained)->get().provenance == "rom-reset",
                "Containerwachstum entwertet ein bestaetigtes Blockhandle.");

        RuntimeBlockTable physical_overlap_query;
        require(!physical_overlap_query.may_overlap_active_physical(0x0C123000u, 1u) &&
                    !physical_overlap_query.may_overlap_active_physical(0xFFFFFFFFu, 0u) &&
                    physical_overlap_query.may_overlap_active_physical(0x9FFFFFFFu, 2u) &&
                    physical_overlap_query.may_overlap_active_physical(0xFFFFFFFFu, 2u) &&
                    physical_overlap_query.may_overlap_active_physical(
                        0u, std::numeric_limits<std::size_t>::max()),
                "Active-Physical-Query behandelt leere, Alias- oder Overflowbereiche falsch.");
        const RuntimeBlock physical_overlap_block{0x8C123FFEu,
                                                  0x0C123FFEu,
                                                  4u,
                                                  BlockEndKind::Return,
                                                  {},
                                                  block_a,
                                                  "active-physical-overlap-query",
                                                  false};
        const auto physical_overlap_identity =
            stable_runtime_block_identity(physical_overlap_block);
        static_cast<void>(
            physical_overlap_query.register_runtime(physical_overlap_block));
        require(physical_overlap_query.may_overlap_active_physical(0x0C123FFEu, 1u) &&
                    physical_overlap_query.may_overlap_active_physical(0x8C123FFFu, 2u) &&
                    physical_overlap_query.may_overlap_active_physical(0xAC124000u, 1u) &&
                    !physical_overlap_query.may_overlap_active_physical(0x0C123000u, 1u) &&
                    !physical_overlap_query.may_overlap_active_physical(0x0C124002u, 1u),
                "Active-Physical-Query verliert Aliastreffer oder liefert Seiten-False-Positives.");
        require(physical_overlap_query.erase_identity(physical_overlap_identity) &&
                    !physical_overlap_query.may_overlap_active_physical(0x0C123FFEu, 4u),
                "Active-Physical-Query behaelt einen deaktivierten Block im Seitenindex.");

        RuntimeBlockTable guarded;
        ExecutableCodeTracker tracker;
        RuntimeBlock tracked{0x8C100000u,
                             0x0C100000u,
                             16u,
                             BlockEndKind::Return,
                             {},
                             block_a,
                             "tracker-race",
                             false};
        const auto tracked_identity = stable_runtime_block_identity(tracked);
        static_cast<void>(tracker.register_block(
            {tracked_identity, tracked.physical_origin, tracked.size, tracked.provenance, {}}));
        guarded.bind_code_tracker(&tracker);
        const auto tracked_handle = guarded.register_runtime(tracked);
        require(guarded.lookup(tracked.virtual_start, {}).has_value(),
                "Trackergebundener Block ist vor Invalidierung nicht sichtbar.");
        static_cast<void>(
            tracker.observe_write(tracked.virtual_start + 4u, 1u, CodeWriteSource::Cpu));
        require(!guarded.resolve(tracked_handle) &&
                    !guarded.lookup(tracked.virtual_start, {}).has_value(),
                "Trackerinvalidierung zwischen Lookup und Resolve fuehrt stale Code aus.");

        RuntimeBlockTable guarded_static;
        ExecutableCodeTracker static_tracker;
        RuntimeBlock tracked_static{0x8C101000u,
                                    0x0C101000u,
                                    8u,
                                    BlockEndKind::Return,
                                    {},
                                    block_a,
                                    "static-tracker-race",
                                    false};
        tracked_static.static_variant_policy =
            StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic;
        const auto tracked_static_identity =
            stable_runtime_block_identity(tracked_static);
        static_cast<void>(static_tracker.register_block(
            {tracked_static_identity,
             tracked_static.physical_origin,
             tracked_static.size,
             tracked_static.provenance,
             {}}));
        guarded_static.bind_code_tracker(
            &static_tracker, StaticAotInvalidationContract::Coordinated);
        static_cast<void>(guarded_static.register_static(tracked_static));
        guarded_static.seal_static();
        const auto tracked_static_execution = guarded_static.lookup_static_aot(
            tracked_static.physical_origin,
            tracked_static.virtual_start,
            tracked_static.variant);
        require(tracked_static_execution &&
                    guarded_static.static_dispatch_generation_guard_current(
                        tracked_static_execution->generation_guard),
                "Trackergebundener Static-AOT-Guard ist initial nicht aktiv.");
        auto wrong_static_id = tracked_static_execution->generation_guard;
        ++wrong_static_id.block.id;
        auto wrong_static_generation =
            tracked_static_execution->generation_guard;
        ++wrong_static_generation.block.generation;
        require(!guarded_static.static_dispatch_generation_guard_current(
                    wrong_static_id) &&
                    !guarded_static.static_dispatch_generation_guard_current(
                        wrong_static_generation),
                "Static-AOT-Guard ignoriert Record-ID oder Record-Generation.");
        static_cast<void>(static_tracker.observe_write(
            tracked_static.virtual_start + 2u,
            1u,
            CodeWriteSource::Cpu));
        require(!guarded_static.lookup_static_aot(
                    tracked_static.physical_origin,
                    tracked_static.virtual_start,
                    tracked_static.variant) &&
                    !guarded_static.static_dispatch_generation_guard_current(
                        tracked_static_execution->generation_guard),
                "Trackerinvalidierung nach Static-AOT-Lookup laesst Lookup oder "
                "Generation-Guard dispatchbar.");

        RuntimeBlockTable aot_templates;
        const RuntimeAotTemplateContract template_contract{{0x8C500000u, 0xAC200000u, 0x40u},
                                                           0x80u};
        RuntimeBlock first_template_block{0xAC200010u,
                                          canonical_physical_address(0xAC200010u),
                                          4u,
                                          BlockEndKind::Fallthrough,
                                          {},
                                          block_a,
                                          "native-template-first",
                                          false,
                                          template_contract};
        RuntimeBlock second_template_block{0xAC200020u,
                                           canonical_physical_address(0xAC200020u),
                                           4u,
                                           BlockEndKind::Return,
                                           {},
                                           block_b,
                                           "native-template-second",
                                           false,
                                           template_contract};
        const auto first_template_identity = stable_runtime_block_identity(first_template_block);
        const auto second_template_identity = stable_runtime_block_identity(second_template_block);
        const auto first_template_handle = aot_templates.register_runtime(first_template_block);
        const auto second_template_handle = aot_templates.register_runtime(second_template_block);
        require(first_template_identity != second_template_identity &&
                    first_template_identity.find("-ts8c500000-trac200000-te64-tv128") !=
                        std::string::npos &&
                    aot_templates.resolve(first_template_handle)->get().aot_template ==
                        template_contract &&
                    aot_templates.resolve(second_template_handle)->get().aot_template ==
                        template_contract &&
                    aot_templates.may_overlap_active_physical(
                        canonical_physical_address(template_contract.mapping.runtime_start) +
                            0x70u,
                        1u) &&
                    !aot_templates.may_overlap_active_physical(
                        canonical_physical_address(template_contract.mapping.runtime_start) +
                            0x80u,
                        1u),
                "AOT-Templatevertrag fehlt in stabiler Identitaet oder Runtimeblock.");
        auto executable_template = first_template_block;
        executable_template.function = relocated_block;
        CpuState template_cpu;
        template_cpu.active_block_virtual_start = 0x11110000u;
        template_cpu.active_block_physical_start = 0x02220000u;
        template_cpu.active_block_size = 0x40u;
        BlockExecutionContext template_context;
        const auto template_exit =
            execute_runtime_block(executable_template, template_cpu, template_context);
        require(template_exit.source.virtual_address == 0xAC200010u &&
                    template_exit.source.physical_address == 0x0C200010u &&
                    template_exit.target->virtual_address == 0x8C500020u &&
                    relocate_code_address(0x8C500010u) == 0x8C500010u &&
                    template_cpu.active_block_virtual_start == 0x11110000u &&
                    template_cpu.active_block_physical_start == 0x02220000u &&
                    template_cpu.active_block_size == 0x40u &&
                    !template_context.exception_generation_on_entry.has_value(),
                "Runtimeblockausfuehrung aktiviert oder entfernt AOT-Mapping/physische "
                "Blockprovenienz nicht atomar.");
        auto throwing_template = executable_template;
        throwing_template.function = throwing_block;
        require(throws_with(
                    [&] {
                        static_cast<void>(execute_runtime_block(
                            throwing_template, template_cpu, template_context));
                    },
                    "expected-backend-throw",
                    "") &&
                    template_cpu.active_block_virtual_start == 0x11110000u &&
                    template_cpu.active_block_physical_start == 0x02220000u &&
                    template_cpu.active_block_size == 0x40u &&
                    !template_context.exception_generation_on_entry.has_value(),
                "Runtimeblock-Throw laesst Exceptiongenerationssnapshot oder aktive "
                "Blockprovenienz gesetzt.");
        require(aot_templates.erase_overlapping_physical(
                    canonical_physical_address(template_contract.mapping.runtime_start) + 0x70u,
                    1u) == 2u &&
                    !aot_templates.resolve(first_template_handle) &&
                    !aot_templates.resolve(second_template_handle) &&
                    !aot_templates.may_overlap_active_physical(
                        canonical_physical_address(template_contract.mapping.runtime_start),
                        template_contract.validation_extent),
                "Literalpatch ausserhalb der Blockbytes invalidiert nicht die ganze AOT-Vorlage.");

        RuntimeBlockTable mutable_aot_templates;
        const RuntimeAotTemplateContract mutable_template_contract{
            {0x8C500000u, 0xAC210000u, 0x40u}, 0x40u, {{0x30u, 4u}}};
        auto mutable_first = first_template_block;
        mutable_first.virtual_start = 0xAC210010u;
        mutable_first.physical_origin =
            canonical_physical_address(mutable_first.virtual_start);
        mutable_first.provenance = "native-template-mutable-first";
        mutable_first.aot_template = mutable_template_contract;
        auto mutable_second = second_template_block;
        mutable_second.virtual_start = 0xAC210020u;
        mutable_second.physical_origin =
            canonical_physical_address(mutable_second.virtual_start);
        mutable_second.provenance = "native-template-mutable-second";
        mutable_second.aot_template = mutable_template_contract;
        const auto mutable_first_handle =
            mutable_aot_templates.register_runtime(mutable_first);
        const auto mutable_second_handle =
            mutable_aot_templates.register_runtime(mutable_second);
        const auto mutable_physical_start = canonical_physical_address(
            mutable_template_contract.mapping.runtime_start);
        require(mutable_aot_templates.may_overlap_active_physical(
                    mutable_physical_start + 0x30u, 4u) &&
                    mutable_aot_templates.erase_overlapping_physical(
                        mutable_physical_start + 0x30u, 4u) == 0u &&
                    mutable_aot_templates.resolve(mutable_first_handle).has_value() &&
                    mutable_aot_templates.resolve(mutable_second_handle).has_value() &&
                    mutable_aot_templates.may_overlap_active_physical(
                        mutable_physical_start + 0x30u, 4u),
                "Scratchslot-Write invalidierte RuntimeBlockTable-AOT-Bloecke.");
        require(mutable_aot_templates.erase_overlapping_physical(
                    mutable_physical_start + 0x33u, 2u) == 2u &&
                    !mutable_aot_templates.resolve(mutable_first_handle) &&
                    !mutable_aot_templates.resolve(mutable_second_handle) &&
                    !mutable_aot_templates.may_overlap_active_physical(
                        mutable_physical_start, mutable_template_contract.validation_extent),
                "Scratchslot/Nachbarbyte-Write invalidierte RuntimeBlockTable nicht.");

        RuntimeBlockTable mmu_aot_templates;
        const auto mmu_template_handle = mmu_aot_templates.register_runtime(
            {0x00002010u,
             0x0C000110u,
             4u,
             BlockEndKind::Return,
             {},
             block_a,
             "native-template-mmu",
             false,
             RuntimeAotTemplateContract{{0x8C500000u, 0x00002000u, 0x40u}, 0x40u}});
        require(mmu_aot_templates.resolve(mmu_template_handle).has_value() &&
                    mmu_aot_templates.may_overlap_active_physical(0x0C000130u, 1u) &&
                    mmu_aot_templates.erase_overlapping_physical(0x0C000130u, 1u) == 1u,
                "MMU-AOT-Template leitet die physische Validierung nicht vom Blockursprung ab.");

        RuntimeBlockTable invalid_templates;
        const auto valid_template_block = first_template_block;
        require(throws_any([&] {
                    static_cast<void>(invalid_templates.register_static(valid_template_block));
                }) &&
                    throws_any([&] {
                        auto invalid = valid_template_block;
                        invalid.virtual_start = 0xAC200040u;
                        invalid.physical_origin = canonical_physical_address(invalid.virtual_start);
                        static_cast<void>(invalid_templates.register_runtime(invalid));
                    }) &&
                    throws_any([&] {
                        auto invalid = valid_template_block;
                        invalid.aot_template->validation_extent = 0x20u;
                        static_cast<void>(invalid_templates.register_runtime(invalid));
                    }) &&
                    throws_any([&] {
                        auto invalid = valid_template_block;
                        invalid.aot_template->mutable_ranges = {{0x10u, 4u}};
                        static_cast<void>(invalid_templates.register_runtime(invalid));
                    }) &&
                    throws_any([&] {
                        auto invalid = valid_template_block;
                        invalid.physical_origin = 0x00000008u;
                        static_cast<void>(invalid_templates.register_runtime(invalid));
                    }) &&
                    throws_any([&] {
                        auto invalid = valid_template_block;
                        invalid.virtual_start = 0x9FFFFFF0u;
                        invalid.physical_origin = canonical_physical_address(invalid.virtual_start);
                        invalid.aot_template =
                            RuntimeAotTemplateContract{{0x8D000000u, 0x9FFFFFF0u, 0x10u}, 0x20u};
                        static_cast<void>(invalid_templates.register_runtime(invalid));
                    }),
                "Ungueltiger statischer, ausserhalb liegender oder aliasbrechender AOT-Vertrag "
                "wurde akzeptiert.");

        RuntimeBlockTable rejected;
        require(throws_any([&] {
                    static_cast<void>(rejected.register_runtime(
                        {0x1000u, 0x1000u, 0u, {}, {}, block_a, "zero", false}));
                }) &&
                    throws_any([&] {
                        static_cast<void>(rejected.register_runtime(
                            {0x1000u, 0x1000u, 4u, {}, {}, nullptr, "null", false}));
                    }) &&
                    throws_any([&] {
                        static_cast<void>(rejected.register_runtime(
                            {0x1000u, 0x1000u, 4u, {}, {}, block_a, "", false}));
                    }) &&
                    throws_any([&] {
                        static_cast<void>(rejected.register_runtime(
                            {0xFFFFFFFEu, 0x1000u, 4u, {}, {}, block_a, "overflow", false}));
                    }),
                "Ungueltige Groesse, Funktion, Provenienz oder Adressraumgrenze wurde akzeptiert.");
        static_cast<void>(
            rejected.register_static({0x2000u, 0x2000u, 4u, {}, {}, block_a, "sealed", false}));
        require(stable_runtime_block_identity(resolved(rejected, rejected.lookup(0x2000u, {}))) ==
                    "v00002000-p00002000-s4-e0-a0-m0-w0-f0-r0-sealed",
                "Statische Blockidentitaet wurde durch optionalen AOT-Vertrag veraendert.");
        rejected.seal_static();
        require(throws_any([&] {
                    static_cast<void>(rejected.register_static(
                        {0x3000u, 0x3000u, 4u, {}, {}, block_a, "late-static", false}));
                }),
                "Statische Registrierung nach der Versiegelung wurde akzeptiert.");

        RuntimeBlockTable bulk;
        std::vector<RuntimeBlock> blocks;
        blocks.reserve(100'000u);
        for (std::uint32_t index = 0u; index < 100'000u; ++index) {
            const auto address = 0x1000u + index * 4u;
            blocks.push_back({address,
                              address,
                              4u,
                              BlockEndKind::Fallthrough,
                              {},
                              block_a,
                              "bulk-" + std::to_string(index),
                              false});
        }
        const auto registration_start = std::chrono::steady_clock::now();
        const auto handles = bulk.register_static_bulk(std::move(blocks));
        const auto registration_elapsed = std::chrono::steady_clock::now() - registration_start;
        require(handles.size() == 100'000u && bulk.size() == 100'000u &&
                    bulk.lookup(0x1000u, {}).has_value() &&
                    bulk.lookup(0x1000u + 50'000u * 4u, {}).has_value() &&
                    bulk.lookup(0x1000u + 99'999u * 4u, {}).has_value() &&
                    !bulk.lookup(0x0FFCu, {}).has_value(),
                "100.000-Block-Bulkregistry verliert Treffer oder Misses.");
        const auto lookup_start = std::chrono::steady_clock::now();
        for (std::uint32_t index = 0u; index < 100'000u; ++index) {
            const auto address = 0x1000u + index * 4u;
            const auto by_virtual = bulk.lookup(address, {});
            const auto by_physical = bulk.lookup_physical(address, {});
            require(by_virtual.has_value() && by_physical.has_value() &&
                        by_virtual->id == handles[index].id && by_physical->id == handles[index].id,
                    "Bulkregistry weicht von der unabhaengigen Adressabbildung ab.");
        }
        const auto lookup_elapsed = std::chrono::steady_clock::now() - lookup_start;
        require(registration_elapsed < std::chrono::seconds(30) &&
                    lookup_elapsed < std::chrono::seconds(15),
                "Bulkregistrierung oder 200.000 geordnete Lookups sprengen das Lastbudget.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
