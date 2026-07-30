#include "katana/codegen/latent_aot_registry.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t sector_size = 2048u;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void both32(std::vector<std::uint8_t>& image,
            const std::size_t offset,
            const std::uint32_t value) {
    for (std::size_t byte = 0u; byte < 4u; ++byte) {
        image[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
        image[offset + 4u + byte] =
            static_cast<std::uint8_t>(value >> ((3u - byte) * 8u));
    }
}

std::size_t record(std::vector<std::uint8_t>& image,
                   const std::size_t offset,
                   const std::uint32_t lba,
                   const std::uint32_t size,
                   const std::string& name,
                   const bool directory) {
    const auto length =
        static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    image[offset] = length;
    both32(image, offset + 2u, lba);
    both32(image, offset + 10u, size);
    image[offset + 25u] = directory ? 0x02u : 0u;
    image[offset + 28u] = 1u;
    image[offset + 31u] = 1u;
    image[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(),
              name.end(),
              image.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}

struct FixtureFile {
    std::uint32_t lba = 0u;
    std::string name;
    std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t>
fixture_iso_with_files(const std::vector<FixtureFile>& files) {
    std::size_t image_sectors = 24u;
    for (const auto& file : files) {
        image_sectors = std::max(
            image_sectors,
            static_cast<std::size_t>(file.lba) + 1u);
    }
    std::vector<std::uint8_t> image(image_sectors * sector_size);
    const auto pvd = 16u * sector_size;
    image[pvd] = 1u;
    std::copy_n("CD001", 5u, image.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    image[pvd + 6u] = 1u;
    static_cast<void>(
        record(image, pvd + 156u, 20u, sector_size, std::string(1u, '\0'), true));

    auto directory = 20u * sector_size;
    directory +=
        record(image, directory, 20u, sector_size, std::string(1u, '\0'), true);
    directory +=
        record(image, directory, 20u, sector_size, std::string(1u, '\1'), true);
    for (const auto& file : files) {
        directory += record(image,
                            directory,
                            file.lba,
                            static_cast<std::uint32_t>(file.bytes.size()),
                            file.name,
                            false);
        std::copy(file.bytes.begin(),
                  file.bytes.end(),
                  image.begin() +
                      static_cast<std::ptrdiff_t>(file.lba * sector_size));
    }
    return image;
}

std::vector<std::uint8_t> fixture_iso() {
    const std::vector<std::uint8_t> module_bytes{
        0x0Bu, 0x00u, 0x09u, 0x00u};
    return fixture_iso_with_files(
        {{21u, "MODULE.BIN;1", module_bytes},
         {22u, "COPY.BIN;1", module_bytes},
         {23u, "DATA.DAT;1", {0xFFu, 0xFFu, 0xFFu, 0xFFu}}});
}

std::string byte_identity(const std::vector<std::uint8_t>& bytes) {
    return "sha256:" + katana::io::sha256_bytes(std::string_view(
                           reinterpret_cast<const char*>(bytes.data()),
                           bytes.size()));
}

} // namespace

int main() {
    try {
        constexpr std::uint32_t relocation_base = 0x89000000u;
        katana::ir::Instruction mova;
        mova.source_address = relocation_base;
        mova.operation = katana::ir::Operation::MoveAddressPcRelative;
        mova.effective_address = relocation_base + 12u;
        katana::ir::Instruction branch;
        branch.source_address = relocation_base + 2u;
        branch.operation = katana::ir::Operation::Branch;
        branch.target_address = relocation_base + 8u;
        katana::ir::BasicBlock relocation_block;
        relocation_block.start_address = relocation_base;
        relocation_block.instructions = {mova, branch};
        relocation_block.successors = {relocation_base + 8u};
        katana::ir::Function relocation_function;
        relocation_function.entry_address = relocation_base;
        relocation_function.blocks = {relocation_block};
        relocation_function.direct_callees = {relocation_base + 8u};
        const std::array relocation_program{relocation_function};
        require(katana::codegen::latent_aot_program_is_relocation_closed(
                    relocation_program, relocation_base, 16u),
                "Interne MOVA-/Branchadressen wurden nicht als relocation-closed erkannt.");
        auto external_target = relocation_program;
        external_target[0].blocks[0].instructions[1].target_address =
            relocation_base + 16u;
        require(!katana::codegen::latent_aot_program_is_relocation_closed(
                    external_target, relocation_base, 16u),
                "Externes direktes Sprungziel wurde an synthetischer Basis akzeptiert.");
        auto external_pc_relative = relocation_program;
        external_pc_relative[0].blocks[0].instructions[0].effective_address =
            relocation_base + 16u;
        require(!katana::codegen::latent_aot_program_is_relocation_closed(
                    external_pc_relative, relocation_base, 16u),
                "Externe PC-relative/MOVA-Adresse wurde an synthetischer Basis akzeptiert.");

        auto source = std::make_shared<katana::runtime::MemoryDiscSource>(
            fixture_iso(), "synthetic-latent-aot-disc");
        const auto discovered =
            katana::codegen::discover_latent_aot_modules(source, 0u, 0u);
        require(discovered.examined_files == 3u && discovered.duplicate_files == 1u &&
                    discovered.rejected_files == 1u && discovered.modules.size() == 1u,
                "Deterministische Discdatei-Discovery klassifizierte die Fixture falsch.");
        const auto& module = discovered.modules.front();
        require(module.disc_byte_offset == 21u * sector_size && module.byte_size == 4u &&
                    module.source_address == 0x88000000u && !module.program.empty() &&
                    module.id.starts_with("latent-aot-") &&
                    module.byte_identity.starts_with("sha256:") &&
                    module.id.find("MODULE") == std::string::npos &&
                    module.id.find("COPY") == std::string::npos &&
                    module.id.find("DATA") == std::string::npos,
                "Latente Registry verlor Offset/AOT oder exportierte einen Discdateinamen.");
        require(
            module.block_identities.size() == 1u &&
                module.block_identities.front().source_offset == 0u &&
                module.block_identities.front().size == 4u &&
                module.block_identities.front().sha256 ==
                    module.byte_identity,
            "Latente Registry exportierte nicht die exakte, sortierte "
            "DispatchBlock-Identitaet.");

        const auto repeated =
            katana::codegen::discover_latent_aot_modules(source, 0u, 0u);
        require(repeated.modules.size() == discovered.modules.size() &&
                    repeated.modules.front().id == module.id &&
                    repeated.modules.front().byte_identity == module.byte_identity &&
                    repeated.modules.front().disc_byte_offset == module.disc_byte_offset &&
                    repeated.modules.front().source_address == module.source_address &&
                    repeated.modules.front().program.size() == module.program.size() &&
                    repeated.modules.front().block_identities ==
                        module.block_identities,
                "Latente Registry ist bei identischer Disc nicht deterministisch geordnet.");

        const std::array duplicate_extent_hint{
            katana::codegen::LatentAotEntryHint{
                module.byte_identity, 22u * sector_size, 4u, 0u}};
        const auto exact_duplicate_extent =
            katana::codegen::discover_latent_aot_modules(
                source,
                0u,
                0u,
                {},
                katana::codegen::LatentAotDiscoveryOptions{},
                {},
                duplicate_extent_hint);
        require(
            exact_duplicate_extent.modules.size() == 1u &&
                exact_duplicate_extent.modules.front().disc_byte_offset ==
                    22u * sector_size &&
                exact_duplicate_extent.modules.front().entry_offsets ==
                    std::vector<std::uint32_t>{0u},
            "Byteidentischer erster ISO-Extent konsumierte die exakte Bindung "
            "des zweiten Extents.");

        const std::array ambiguous_duplicate_hints{
            katana::codegen::LatentAotEntryHint{
                module.byte_identity, 21u * sector_size, 4u, 0u},
            katana::codegen::LatentAotEntryHint{
                module.byte_identity, 22u * sector_size, 4u, 0u},
        };
        bool rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    source,
                    0u,
                    0u,
                    {},
                    katana::codegen::LatentAotDiscoveryOptions{},
                    {},
                    ambiguous_duplicate_hints));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-byte-identity-ambiguous";
        }
        require(
            rejected,
            "Byteidentische Exact-Hints an verschiedenen Disc-Extents "
            "erzeugten mehrdeutige Runtime-Templates.");

        const std::vector<std::uint8_t> first_cap_bytes{
            0x0Bu, 0x00u, 0x09u, 0x00u};
        const std::vector<std::uint8_t> hinted_cap_bytes{
            0x0Bu, 0x00u, 0x08u, 0x00u};
        auto cap_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "FIRST.BIN;1", first_cap_bytes},
                     {22u, "HINTED.BIN;1", hinted_cap_bytes}}),
                "synthetic-latent-aot-cap-disc");
        auto cap_options = katana::codegen::LatentAotDiscoveryOptions{};
        cap_options.maximum_candidate_files = 1u;
        const std::array behind_cap_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(hinted_cap_bytes),
                22u * sector_size,
                static_cast<std::uint32_t>(hinted_cap_bytes.size()),
                0u}};
        const auto exact_behind_cap =
            katana::codegen::discover_latent_aot_modules(
                cap_source,
                0u,
                0u,
                {},
                cap_options,
                {},
                behind_cap_hint);
        require(
            std::any_of(
                exact_behind_cap.modules.begin(),
                exact_behind_cap.modules.end(),
                [](const auto& candidate) {
                    return candidate.disc_byte_offset == 22u * sector_size &&
                           candidate.entry_offsets ==
                               std::vector<std::uint32_t>{0u};
                }),
            "Exakter Latent-AOT-Hint hinter dem Heuristik-Kandidatenlimit "
            "blieb ungebunden.");

        const std::vector<std::uint8_t> header_module_bytes{
            0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x0Bu, 0x00u, 0x09u, 0x00u};
        auto header_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "HEADER.BIN;1", header_module_bytes}}),
                "synthetic-latent-aot-header-disc");
        const std::array nonzero_entry_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(header_module_bytes),
                21u * sector_size,
                static_cast<std::uint32_t>(header_module_bytes.size()),
                4u}};
        const auto exact_nonzero_entry =
            katana::codegen::discover_latent_aot_modules(
                header_source,
                0u,
                0u,
                {},
                katana::codegen::LatentAotDiscoveryOptions{},
                {},
                nonzero_entry_hint);
        require(
            exact_nonzero_entry.modules.size() == 1u &&
                exact_nonzero_entry.modules.front().entry_offsets ==
                    std::vector<std::uint32_t>{4u},
            "Exakter Nonzero-Entry wurde durch einen synthetischen Entry 0 "
            "oder dessen Datenheader abgelehnt.");

        const std::vector<std::uint8_t> six_byte_module{
            0x0Bu, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u};
        auto six_byte_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "SIX.BIN;1", six_byte_module}}),
                "synthetic-latent-aot-six-byte-disc");
        const std::array six_byte_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(six_byte_module),
                21u * sector_size,
                static_cast<std::uint32_t>(six_byte_module.size()),
                0u}};
        const auto exact_six_byte =
            katana::codegen::discover_latent_aot_modules(
                six_byte_source,
                0u,
                0u,
                {},
                katana::codegen::LatentAotDiscoveryOptions{},
                {},
                six_byte_hint);
        require(exact_six_byte.modules.size() == 1u &&
                    exact_six_byte.modules.front().byte_size == 6u,
                "Gerader exakter Sechs-Byte-Modulbound wurde wie ein "
                "Vierbyte-Heuristikkandidat abgelehnt.");

        auto exact_file_bounded =
            katana::codegen::LatentAotDiscoveryOptions{};
        exact_file_bounded.maximum_file_bytes = 4u;
        rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    six_byte_source,
                    0u,
                    0u,
                    {},
                    exact_file_bounded,
                    {},
                    six_byte_hint));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-file-budget";
        }
        require(
            rejected,
            "Exact-Hint umging das einzelne Modul-/Binderbudget.");

        auto exact_total_bounded =
            katana::codegen::LatentAotDiscoveryOptions{};
        exact_total_bounded.maximum_file_bytes = 8u;
        exact_total_bounded.maximum_total_file_bytes = 4u;
        rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    six_byte_source,
                    0u,
                    0u,
                    {},
                    exact_total_bounded,
                    {},
                    six_byte_hint));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-total-budget";
        }
        require(
            rejected,
            "Exact-Hint umging das globale Dateilesebudget.");

        const std::array occupied{
            katana::codegen::LatentAotOccupiedRange{0x88000000u, 4096u}};
        const auto collision_free = katana::codegen::discover_latent_aot_modules(
            source,
            0u,
            0u,
            {},
            katana::codegen::LatentAotDiscoveryOptions{},
            occupied);
        require(collision_free.modules.size() == 1u &&
                    collision_free.modules.front().source_address == 0x88001000u,
                "Belegte native Source-Range wurde nicht deterministisch uebersprungen.");

        const std::array excluded{module.byte_identity};
        const auto excluded_result = katana::codegen::discover_latent_aot_modules(
            source, 0u, 0u, excluded);
        require(excluded_result.modules.empty() &&
                    excluded_result.duplicate_files == 2u &&
                    excluded_result.rejected_files == 1u,
                "Ausgeschlossene/duplizierte Byteidentitaet wurde erneut analysiert.");

        auto bounded = katana::codegen::LatentAotDiscoveryOptions{};
        bounded.maximum_native_instructions_per_module = 1u;
        const auto bounded_result = katana::codegen::discover_latent_aot_modules(
            source, 0u, 0u, {}, bounded);
        require(bounded_result.modules.empty() && bounded_result.rejected_files >= 1u,
                "Instruktionsbudget verwarf ein zu grosses natives Modul nicht lokal.");

        auto invalid = katana::codegen::LatentAotDiscoveryOptions{};
        invalid.maximum_workers = 0u;
        rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(source, 0u, 0u, {}, invalid));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Ungueltiges Discovery-Budget wurde akzeptiert.");

        auto directory_bounded = katana::codegen::LatentAotDiscoveryOptions{};
        directory_bounded.maximum_directory_bytes = 1024u;
        directory_bounded.maximum_total_directory_bytes = 1024u;
        rejected = false;
        try {
            static_cast<void>(katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, directory_bounded));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected,
                "Directory wurde trotz vorangestelltem Bytebudget vollstaendig gelesen.");

        std::cout << "Latente native Disc-AOT-Registry erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
