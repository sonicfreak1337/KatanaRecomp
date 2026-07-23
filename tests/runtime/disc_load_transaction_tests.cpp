#include "katana/runtime/disc_load_transaction.hpp"

#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/gdrom_controller.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace katana::runtime;

void require(const bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<std::uint8_t>
read_bytes(const Memory& memory, const std::uint32_t address, const std::size_t size) {
    std::vector<std::uint8_t> result(size);
    for (std::size_t index = 0u; index < size; ++index)
        result[index] = memory.read_u8(address + static_cast<std::uint32_t>(index));
    return result;
}

BlockExit test_block(CpuState&, BlockExecutionContext&) {
    BlockExit exit;
    exit.kind = BlockEndKind::Return;
    return exit;
}

DiscLoadRequest request(const std::uint64_t sequence,
                        const std::uint32_t guest,
                        const std::uint32_t physical,
                        const std::span<const std::uint8_t> bytes,
                        std::string content_identity,
                        const std::uint64_t source_offset,
                        const bool known_source = true) {
    DiscLoadRequest result;
    result.sequence = sequence;
    result.route = DiscLoadRoute::BiosDma;
    result.guest_destination = guest;
    result.physical_destination = physical;
    result.write_source = CodeWriteSource::Dma;
    result.content_identity = std::move(content_identity);
    result.byte_identity = disc_load_byte_identity(bytes);
    result.source_range =
        known_source ? DiscLoadSourceRange{true, source_offset, bytes.size()}
                     : DiscLoadSourceRange{};
    result.bytes = bytes;
    result.guest_translation_validated = true;
    return result;
}

DiscLoadRangeResolver linear_resolver(const bool executable = true) {
    return [=](const std::uint32_t physical, const std::size_t size) {
        DiscLoadCommittedRange range;
        range.target_physical_address = physical;
        range.backing_physical_address = physical;
        range.size = static_cast<std::uint32_t>(size);
        range.executable_backing = executable;
        return std::vector<DiscLoadCommittedRange>{range};
    };
}

struct TransactionFixture {
    Memory memory{0u};
    std::shared_ptr<LinearMemoryDevice> ram =
        std::make_shared<LinearMemoryDevice>(0x10000u);
    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    std::unique_ptr<ExecutableDiscLoadTransactionCoordinator> coordinator;

    explicit TransactionFixture(DiscLoadAdmissionObserver observer = {},
                                DiscLoadRangeResolver resolver = linear_resolver()) {
        memory.map_region("transaction-test-ram", 0x0C000000u, ram);
        blocks.bind_code_tracker(&tracker);
        coordinator = std::make_unique<ExecutableDiscLoadTransactionCoordinator>(
            memory,
            modules,
            blocks,
            tracker,
            std::move(resolver),
            std::move(observer));
        memory.set_guest_write_observer([this](const GuestWriteEvent& event) {
            static_cast<void>(coordinator->consume_guest_write(event));
        });
    }
};

void identity_alias_and_atomic_rejection_regression() {
    constexpr std::uint32_t physical = 0x0C001000u;
    const std::array<std::uint8_t, 8u> bytes{0x09u, 0x00u, 0x0Bu, 0x00u,
                                             0x09u, 0x00u, 0x0Bu, 0x00u};
    TransactionFixture fixture;
    const auto first = fixture.coordinator->execute(
        request(1u, 0x8C001000u, physical, bytes, "sha256:content-a", 0x4000u));
    const auto* first_module = fixture.modules.resolve(physical, bytes.size());
    require(first.bytes_changed && first.ranges.size() == 1u &&
                first.ranges.front().module_activated && first_module != nullptr &&
                first_module->content_identity == "sha256:content-a" &&
                first_module->byte_identity == disc_load_byte_identity(bytes),
            "Erster Disc-Load publiziert Bytes und zweifache Identitaet nicht atomar.");

    const auto module_generation = first_module->generation;
    const auto second = fixture.coordinator->execute(
        request(2u, 0xAC001000u, physical, bytes, "sha256:content-a", 0x4000u));
    const auto* retained = fixture.modules.resolve(physical, bytes.size());
    require(!second.bytes_changed && second.ranges.front().exact_module_retained &&
                !second.ranges.front().module_activated && retained != nullptr &&
                retained->generation == module_generation,
            "P1/P2-Alias behaelt das exakt identische physische Modul nicht bei.");

    const auto rebound = fixture.coordinator->execute(
        request(3u, 0x8C001000u, physical, bytes, "sha256:content-b", 0x4000u));
    require(!rebound.bytes_changed && rebound.identity_rebound &&
                rebound.ranges.front().module_activated &&
                fixture.modules.resolve(physical, bytes.size())->content_identity ==
                    "sha256:content-b",
            "Bytegleiche Fremdidentitaet wird ohne Provenienz-Rebind beibehalten.");

    bool reject = true;
    TransactionFixture rejected(
        [&](const DiscLoadRequest&, const std::span<const DiscLoadCommittedRange>) {
            if (reject) throw std::runtime_error("synthetic-admission-rejection");
        });
    ExecutableModule tombstone;
    tombstone.id = "zero-tombstone";
    tombstone.source_identity = "sha256:zero-tombstone";
    tombstone.guest_start = physical;
    tombstone.bytes.assign(8u, 0u);
    rejected.modules.publish(std::move(tombstone));
    rejected.modules.unload("zero-tombstone", rejected.blocks, rejected.tracker);
    const auto module_before = rejected.modules.snapshot();
    const auto block_before = rejected.blocks.snapshot();
    const auto tracker_before = rejected.tracker.snapshot();
    const auto memory_before = read_bytes(rejected.memory, physical, bytes.size());
    bool threw = false;
    try {
        static_cast<void>(rejected.coordinator->execute(
            request(1u, 0x8C001000u, physical, bytes, "sha256:content-a", 0x4000u)));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw && read_bytes(rejected.memory, physical, bytes.size()) == memory_before &&
                rejected.modules.snapshot() == module_before &&
                rejected.blocks.snapshot() == block_before &&
                rejected.tracker.snapshot() == tracker_before,
            "Admission-Rollback veraendert RAM, Katalog, Zero-Tombstones oder Codezustand.");
}

void latent_aot_multiread_regression() {
    constexpr std::uint32_t target = 0x0C002000u;
    constexpr std::uint64_t source = 0x12000u;
    const std::array<std::uint8_t, 8u> file{0x09u, 0x00u, 0x0Bu, 0x00u,
                                            0x09u, 0x00u, 0x0Bu, 0x00u};
    TransactionFixture fixture;
    fixture.coordinator->set_aot_module_descriptors(
        std::array{DiscLoadAotModuleDescriptor{"latent_fixture",
                                                "sha256:recipe-content",
                                                source,
                                                static_cast<std::uint32_t>(file.size()),
                                                disc_load_byte_identity(file)}});

    const auto first = fixture.coordinator->execute(request(
        1u,
        0x8C002000u,
        target,
        std::span<const std::uint8_t>(file).first<4u>(),
        "sha256:recipe-content",
        source));
    const auto* pending = fixture.modules.resolve(target, 4u);
    require(first.ranges.front().module_activated && pending != nullptr &&
                pending->bytes.size() == 4u && pending->executable_permission &&
                !pending->control_transfer_promotion_allowed &&
                pending->active_extents ==
                    std::vector<ExecutableModuleActiveExtent>{{0u, 4u}} &&
                fixture.modules.resolve(target, file.size()) == nullptr,
            "AOT-Teilread wird nicht als begrenzter MissingAot-Kandidat gehalten.");

    const auto second = fixture.coordinator->execute(request(
        2u,
        0x8C002004u,
        target + 4u,
        std::span<const std::uint8_t>(file).last<4u>(),
        "sha256:recipe-content",
        source + 4u));
    const auto* complete = fixture.modules.resolve(target, file.size());
    require(second.ranges.front().module_activated && complete != nullptr &&
                complete->bytes == std::vector<std::uint8_t>(file.begin(), file.end()) &&
                complete->content_identity == "sha256:recipe-content" &&
                complete->byte_identity == disc_load_byte_identity(file) &&
                complete->active_extents ==
                    std::vector<ExecutableModuleActiveExtent>{
                        {0u, static_cast<std::uint32_t>(file.size())}},
            "Zusammenhaengende Multi-Read-Coverage aktiviert kein voll gehashtes Dateimodul.");

    const auto generation = complete->generation;
    const auto alias_reload = fixture.coordinator->execute(request(
        3u,
        0xAC002000u,
        target,
        file,
        "sha256:recipe-content",
        source));
    require(alias_reload.ranges.front().exact_module_retained &&
                fixture.modules.resolve(target, file.size())->generation == generation,
            "Vollstaendiges AOT-Modul wird bei bytegleichem Aliasreload ersetzt.");

    TransactionFixture wrong_identity;
    wrong_identity.coordinator->set_aot_module_descriptors(
        std::array{DiscLoadAotModuleDescriptor{"latent_fixture",
                                                "sha256:recipe-content",
                                                source,
                                                static_cast<std::uint32_t>(file.size()),
                                                disc_load_byte_identity(file)}});
    static_cast<void>(wrong_identity.coordinator->execute(request(
        1u,
        0x8C002000u,
        target,
        std::span<const std::uint8_t>(file).first<4u>(),
        "sha256:recipe-content",
        source)));
    static_cast<void>(wrong_identity.coordinator->execute(request(
        2u,
        0x8C002004u,
        target + 4u,
        std::span<const std::uint8_t>(file).last<4u>(),
        "sha256:other-content",
        source + 4u)));
    require(wrong_identity.modules.resolve(target, file.size()) == nullptr,
            "Fremde Contentidentitaet vervollstaendigt eine alte AOT-Coverage.");
}

void split_range_and_nonmain_invalidation_regression() {
    Memory memory(0u);
    static_cast<void>(map_dreamcast_main_ram(memory));
    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    ExecutableDiscLoadTransactionCoordinator split(
        memory,
        modules,
        blocks,
        tracker,
        [](const std::uint32_t physical, const std::size_t size) {
            require(physical == 0x0CFFFFFCu && size == 8u,
                    "Splitfixture erhielt unerwarteten Zielbereich.");
            DiscLoadCommittedRange tail;
            tail.target_physical_address = 0x0CFFFFFCu;
            tail.backing_physical_address = 0x0CFFFFFCu;
            tail.size = 4u;
            tail.executable_backing = true;
            DiscLoadCommittedRange head;
            head.target_physical_address = 0x0D000000u;
            head.backing_physical_address = 0x0C000000u;
            head.source_offset = 4u;
            head.size = 4u;
            head.executable_backing = true;
            return std::vector<DiscLoadCommittedRange>{tail, head};
        });
    memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
        static_cast<void>(split.consume_guest_write(event));
    });
    const std::array<std::uint8_t, 8u> wrapped{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    const auto commit = split.execute(
        request(1u, 0x8CFFFFFCu, 0x0CFFFFFCu, wrapped, "sha256:split", 0u));
    require(commit.ranges.size() == 2u &&
                read_bytes(memory, 0x0CFFFFFCu, 4u) ==
                    std::vector<std::uint8_t>(wrapped.begin(), wrapped.begin() + 4) &&
                read_bytes(memory, 0x0D000000u, 4u) ==
                    std::vector<std::uint8_t>(wrapped.begin() + 4, wrapped.end()),
            "RAM-Mirrorgrenze wird nicht als zwei atomare Backingextents committed.");

    TransactionFixture nonmain({}, linear_resolver(false));
    constexpr std::uint32_t tracked = 0x0C003000u;
    static_cast<void>(nonmain.tracker.register_block(
        {"tracked-nonmain", tracked, 4u, "synthetic", {}, ExecutableBlockOrigin::RuntimeWrite}));
    const auto handle = nonmain.blocks.register_runtime(
        {tracked, tracked, 4u, BlockEndKind::Return, {}, test_block, "tracked-nonmain", true});
    const std::array<std::uint8_t, 4u> payload{9u, 0u, 11u, 0u};
    static_cast<void>(nonmain.coordinator->execute(
        request(1u, tracked, tracked, payload, "sha256:nonmain", 0u, false)));
    require(!nonmain.tracker.valid("tracked-nonmain") && !nonmain.blocks.active(handle) &&
                nonmain.modules.resolve(tracked, payload.size()) == nullptr,
            "Getracktes Nicht-Main-RAM behaelt Blockzustand nach Disc-DMA.");
}

void partial_catalog_index_and_contract_regression() {
    constexpr std::uint32_t base = 0x0C001000u;
    TransactionFixture fixture;
    ExecutableModule original;
    original.id = "three-page-module";
    original.source_identity = "sha256:three-page-module";
    original.content_identity = "sha256:old-content";
    original.byte_identity = "sha256:old-bytes";
    original.guest_start = base;
    original.bytes.assign(0x3000u, 0x11u);
    fixture.memory.write_bytes(base, original.bytes, CodeWriteSource::Copy);
    fixture.modules.publish(original);

    std::vector<std::uint8_t> middle(0x1000u, 0x22u);
    static_cast<void>(fixture.coordinator->execute(
        request(1u,
                0x8C002000u,
                base + 0x1000u,
                middle,
                "sha256:new-content",
                0x9000u)));
    const auto* left = fixture.modules.resolve(base, 2u);
    const auto* right = fixture.modules.resolve(base + 0x2000u, 2u);
    require(left != nullptr && right != nullptr && left->id == "three-page-module" &&
                right->id == "three-page-module" &&
                fixture.modules.resolve(base + 0x1000u, 2u)->id !=
                    "three-page-module",
            "Partieller Mittelseitenersatz verliert Restextents im Katalogindex.");

    std::uint64_t next = 7u;
    require(claim_disc_load_sequence(next) == 7u && next == 8u,
            "Disc-Load-Sequenz vergibt keinen monotonen Wert.");
    next = std::numeric_limits<std::uint64_t>::max();
    bool overflow = false;
    try {
        static_cast<void>(claim_disc_load_sequence(next));
    } catch (const std::overflow_error&) {
        overflow = true;
    }
    require(overflow && next == std::numeric_limits<std::uint64_t>::max(),
            "Disc-Load-Sequenz wrappt oder mutiert vor ihrer Ueberlaufablehnung.");

    Memory controller_memory(0u);
    controller_memory.map_region(
        "controller-contract-ram", 0x0C000000u, std::make_shared<LinearMemoryDevice>(0x1000u));
    EventScheduler scheduler;
    const std::array<std::uint8_t, 2048u> sector{};
    auto source = std::make_shared<MemoryDiscSource>(sector, "synthetic-policy-disc");
    bool missing_executor = false;
    try {
        DreamcastGdRomController rejected(controller_memory, scheduler, GdRomDrive(source));
    } catch (const std::invalid_argument&) {
        missing_executor = true;
    }
    require(missing_executor,
            "Produktiver GD-ROM-Controller akzeptiert stillen Directwrite ohne Executor.");

    DreamcastGdRomController standalone(
        controller_memory,
        scheduler,
        GdRomDrive(source),
        {},
        {},
        {},
        {},
        {},
        DiscLoadExecutionPolicy::StandaloneTestMode);
    static_cast<void>(standalone.status());

    bool missing_identity = false;
    try {
        DreamcastGdRomController rejected(
            controller_memory,
            scheduler,
            GdRomDrive(source),
            {},
            {},
            {},
            [](const DiscLoadRequest&) { return DiscLoadCommit{}; });
    } catch (const std::invalid_argument&) {
        missing_identity = true;
    }
    require(missing_identity,
            "Produktiver GD-ROM-Controller akzeptiert leere Recipe-Contentidentitaet.");
}

} // namespace

int main() {
    try {
        identity_alias_and_atomic_rejection_regression();
        latent_aot_multiread_regression();
        split_range_and_nonmain_invalidation_regression();
        partial_catalog_index_and_contract_regression();
        std::cout << "disc load transaction tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
