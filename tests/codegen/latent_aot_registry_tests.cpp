#include "katana/codegen/latent_aot_registry.hpp"

#include "katana/analysis/abi.hpp"
#include "katana/codegen/latent_aot_analysis_cache.hpp"
#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

struct AnalysisCacheFixture {
    std::filesystem::path path =
        std::filesystem::current_path() /
        "katana-latent-aot-analysis-cache-fixture";

    AnalysisCacheFixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    ~AnalysisCacheFixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    [[nodiscard]] std::size_t corrupt_all_artifacts() const {
        std::size_t count = 0u;
        if (!std::filesystem::exists(path))
            return count;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file() ||
                entry.path().filename() != "module-analysis.bin")
                continue;
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture konnte nicht korrumpiert werden.");
            output << "corrupt";
            output.close();
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture wurde nicht vollstaendig korrumpiert.");
            ++count;
        }
        return count;
    }

    [[nodiscard]] bool replace_positive_with_source_mismatch(
        const std::string& positive_key) const {
        if (!std::filesystem::exists(path))
            return false;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file() ||
                entry.path().filename() != "module-analysis.bin")
                continue;
            std::ifstream input(entry.path(), std::ios::binary);
            const std::vector<std::uint8_t> artifact{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            if (input.bad() || artifact.empty())
                throw std::runtime_error(
                    "Analysecache-Fixture konnte ein Artefakt nicht lesen.");
            auto parsed =
                katana::codegen::parse_latent_aot_analysis_cache(
                    positive_key, artifact);
            if (parsed.state !=
                katana::codegen::LatentAotAnalysisCacheState::Positive)
                continue;
            auto& instruction =
                parsed.program.front().blocks.front().instructions.front();
            // Keep the graph and IR structurally valid while making its
            // claimed delayed opcode disagree with the exact RTS bytes. BRA
            // retains an Owner role, so the ordinary IR verifier still accepts
            // this checksum-consistent foreign payload.
            instruction.original_opcode = 0xA000u;
            const auto replaced =
                katana::codegen::serialize_latent_aot_positive_cache(
                    positive_key, parsed.program);
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(replaced.data()),
                static_cast<std::streamsize>(replaced.size()));
            output.close();
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture konnte Fremd-IR nicht schreiben.");
            return true;
        }
        return false;
    }
};

std::string analysis_cache_key_for_module(
    const katana::codegen::PreparedLatentAotModule& module,
    const katana::codegen::LatentAotDiscoveryOptions& options) {
    katana::codegen::LatentAotAnalysisCacheKeyInputs inputs;
    inputs.byte_sha256 = module.byte_identity.substr(7u);
    inputs.byte_size = module.byte_size;
    inputs.entry_offsets = module.entry_offsets;
    inputs.exact_candidate = false;
    inputs.source_address = module.source_address;
    inputs.maximum_entry_scan_instructions =
        options.maximum_entry_scan_instructions;
    inputs.maximum_native_instructions =
        options.maximum_native_instructions_per_module;
    inputs.maximum_blocks = options.maximum_blocks_per_module;
    inputs.maximum_functions = options.maximum_functions_per_module;
    inputs.maximum_analysis_iterations =
        options.maximum_analysis_iterations;
    inputs.maximum_analysis_contexts =
        options.maximum_analysis_contexts;
    inputs.analyzer_abi = katana::analysis::abi_version;
    inputs.analyzer_implementation_id =
        std::string(
            katana::codegen::latent_aot_analysis_implementation_id) +
        "-" +
        katana::io::sha256_bytes(
            options.analysis_implementation_identity);
    return katana::codegen::make_latent_aot_analysis_cache_key(inputs);
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
        require(module.source_bindings.size() == 2u &&
                    module.source_bindings[0].disc_byte_offset ==
                        21u * sector_size &&
                    module.source_bindings[1].disc_byte_offset ==
                        22u * sector_size &&
                    module.source_bindings[0].byte_size == 4u &&
                    module.source_bindings[1].byte_size == 4u &&
                    module.source_bindings[0].id.starts_with(
                        "latent-aot-source-") &&
                    module.source_bindings[1].id.starts_with(
                        "latent-aot-source-") &&
                    module.byte_size == 4u &&
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
                    repeated.modules.front().source_bindings ==
                        module.source_bindings &&
                    repeated.modules.front().source_address == module.source_address &&
                    repeated.modules.front().program.size() == module.program.size() &&
                    repeated.modules.front().block_identities ==
                        module.block_identities,
                "Latente Registry ist bei identischer Disc nicht deterministisch geordnet.");

        AnalysisCacheFixture analysis_cache_fixture;
        auto cached_options =
            katana::codegen::LatentAotDiscoveryOptions{};
        cached_options.analysis_cache_root =
            analysis_cache_fixture.path;
        const auto unproven_cache_disabled =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            unproven_cache_disabled.modules.size() == 1u &&
                unproven_cache_disabled.analysis_cache_positive_hits == 0u &&
                unproven_cache_disabled.analysis_cache_negative_hits == 0u &&
                unproven_cache_disabled.analysis_cache_misses == 0u &&
                unproven_cache_disabled.analysis_cache_stores == 0u &&
                !std::filesystem::exists(analysis_cache_fixture.path),
            "Latent-AOT-Analysecache lief ohne beweisbare genaue "
            "Analyzer-/Exporter-Identitaet.");
        cached_options.analysis_implementation_identity =
            "latent-registry-test-implementation-a";
        const auto cache_cold =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            cache_cold.modules.size() == 1u &&
                cache_cold.rejected_files == 1u &&
                cache_cold.analysis_candidate_duration_ms.size() == 2u &&
                cache_cold.analysis_cache_positive_hits == 0u &&
                cache_cold.analysis_cache_negative_hits == 0u &&
                cache_cold.analysis_cache_misses == 2u &&
                cache_cold.analysis_cache_stores == 2u,
            "Kalter Latent-AOT-Analysecache speicherte positive und "
            "deterministisch negative Resultate nicht exakt einmal.");
        const auto cache_warm =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            cache_warm.modules.size() == 1u &&
                cache_warm.analysis_candidate_duration_ms.size() == 2u &&
                cache_warm.modules.front().program.size() ==
                    cache_cold.modules.front().program.size() &&
                cache_warm.modules.front().program.front().entry_address ==
                    cache_cold.modules.front().program.front().entry_address &&
                cache_warm.modules.front().program.front().blocks.size() ==
                    cache_cold.modules.front().program.front().blocks.size() &&
                cache_warm.modules.front().block_identities ==
                    cache_cold.modules.front().block_identities &&
                cache_warm.rejected_files == 1u &&
                cache_warm.analysis_cache_positive_hits == 1u &&
                cache_warm.analysis_cache_negative_hits == 1u &&
                cache_warm.analysis_cache_misses == 0u &&
                cache_warm.analysis_cache_stores == 0u,
            "Warmer Latent-AOT-Analysecache lieferte keinen validierten "
            "positiven und negativen Treffer.");

        const auto positive_cache_key =
            analysis_cache_key_for_module(
                cache_cold.modules.front(), cached_options);
        require(
            analysis_cache_fixture
                .replace_positive_with_source_mismatch(
                    positive_cache_key),
            "Analysecache-Fixture fand keinen positiven Cacheeintrag.");
        const auto cache_source_mismatch =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            cache_source_mismatch.modules.size() == 1u &&
                cache_source_mismatch.rejected_files == 1u &&
                cache_source_mismatch.analysis_cache_positive_hits == 0u &&
                cache_source_mismatch.analysis_cache_negative_hits == 1u &&
                cache_source_mismatch.analysis_cache_misses == 1u &&
                cache_source_mismatch.analysis_cache_corrupt_entries == 1u &&
                cache_source_mismatch.analysis_cache_stores == 1u &&
                cache_source_mismatch.modules.front().block_identities ==
                    cache_cold.modules.front().block_identities,
            "Checksum-konsistentes, formal gueltiges Fremd-IR wurde nicht "
            "an die aktuellen Modulbytes gebunden und neu analysiert.");

        auto changed_implementation_options = cached_options;
        changed_implementation_options.analysis_implementation_identity =
            "latent-registry-test-implementation-b";
        const auto implementation_key_miss =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, changed_implementation_options);
        require(
            implementation_key_miss.modules.size() == 1u &&
                implementation_key_miss.rejected_files == 1u &&
                implementation_key_miss.analysis_cache_positive_hits == 0u &&
                implementation_key_miss.analysis_cache_negative_hits == 0u &&
                implementation_key_miss.analysis_cache_misses == 2u &&
                implementation_key_miss.analysis_cache_stores == 2u,
            "Geaenderte genaue Analyzer-/Exporter-Implementierung "
            "invalidierte positive und negative Cacheeintraege nicht.");

        auto changed_cache_options = cached_options;
        ++changed_cache_options.maximum_analysis_contexts;
        const auto cache_key_miss =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, changed_cache_options);
        require(
            cache_key_miss.modules.size() == 1u &&
                cache_key_miss.rejected_files == 1u &&
                cache_key_miss.analysis_cache_positive_hits == 0u &&
                cache_key_miss.analysis_cache_negative_hits == 0u &&
                cache_key_miss.analysis_cache_misses == 2u &&
                cache_key_miss.analysis_cache_stores == 2u,
            "Analyse-relevantes Budget invalidierte den "
            "Latent-AOT-Analysecache nicht.");

        require(
            analysis_cache_fixture.corrupt_all_artifacts() == 6u,
            "Analysecache-Fixture fand nicht alle positiven/negativen "
            "Artefakte beider Schluessel.");
        const auto cache_corrupt =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            cache_corrupt.modules.size() == 1u &&
                cache_corrupt.modules.front().program.size() ==
                    cache_cold.modules.front().program.size() &&
                cache_corrupt.modules.front().program.front().entry_address ==
                    cache_cold.modules.front().program.front().entry_address &&
                cache_corrupt.rejected_files == 1u &&
                cache_corrupt.analysis_cache_positive_hits == 0u &&
                cache_corrupt.analysis_cache_negative_hits == 0u &&
                cache_corrupt.analysis_cache_misses == 2u &&
                cache_corrupt.analysis_cache_corrupt_entries == 2u &&
                cache_corrupt.analysis_cache_stores == 2u,
            "Korrupter Latent-AOT-Analysecache wurde nicht fail-closed "
            "als Miss neu analysiert und begrenzt repariert.");

        auto exact_only_options =
            katana::codegen::LatentAotDiscoveryOptions{};
        exact_only_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        const std::array duplicate_extent_hint{
            katana::codegen::LatentAotEntryHint{
                module.byte_identity, 22u * sector_size, 4u, 0u}};
        const auto exact_duplicate_extent =
            katana::codegen::discover_latent_aot_modules(
                source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                duplicate_extent_hint);
        require(
            exact_duplicate_extent.modules.size() == 1u &&
                exact_duplicate_extent.modules.front().id == module.id &&
                exact_duplicate_extent.modules.front().source_bindings.size() ==
                    1u &&
                exact_duplicate_extent.modules.front()
                        .source_bindings.front()
                        .disc_byte_offset == 22u * sector_size &&
                exact_duplicate_extent.modules.front().entry_offsets ==
                    std::vector<std::uint32_t>{0u},
            "Byteidentischer erster ISO-Extent konsumierte die exakte Bindung "
            "des zweiten Extents.");

        const std::vector<std::uint8_t> multi_entry_bytes{
            0x0Bu, 0x00u, 0x09u, 0x00u,
            0x0Bu, 0x00u, 0x09u, 0x00u};
        auto multi_entry_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "MULTI_A.BIN;1", multi_entry_bytes},
                     {22u, "MULTI_B.BIN;1", multi_entry_bytes}}),
                "synthetic-latent-aot-multi-entry-disc");
        const std::array multi_extent_hints{
            katana::codegen::LatentAotEntryHint{
                byte_identity(multi_entry_bytes),
                21u * sector_size,
                static_cast<std::uint32_t>(multi_entry_bytes.size()),
                0u},
            katana::codegen::LatentAotEntryHint{
                byte_identity(multi_entry_bytes),
                22u * sector_size,
                static_cast<std::uint32_t>(multi_entry_bytes.size()),
                4u},
        };
        const auto multi_extent =
            katana::codegen::discover_latent_aot_modules(
                multi_entry_source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                multi_extent_hints);
        require(
            multi_extent.examined_files == 2u &&
                multi_extent.modules.size() == 1u &&
                multi_extent.modules.front().source_bindings.size() == 2u &&
                multi_extent.modules.front().source_bindings[0].disc_byte_offset ==
                    21u * sector_size &&
                multi_extent.modules.front().source_bindings[1].disc_byte_offset ==
                    22u * sector_size &&
                multi_extent.modules.front().entry_offsets ==
                    (std::vector<std::uint32_t>{0u, 4u}),
            "Byteidentische Exact-Hints an verschiedenen Disc-Extents "
            "wurden nicht als ein Template mit zwei Source-Bindings und "
            "vereinigtem Entryset gruppiert.");

        bool rejected = false;

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
        cap_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        cap_options.maximum_candidate_files = 0u;
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
            exact_behind_cap.examined_files == 1u &&
                exact_behind_cap.modules.size() == 1u &&
                exact_behind_cap.modules.front().source_bindings.size() == 1u &&
                exact_behind_cap.modules.front()
                        .source_bindings.front()
                        .disc_byte_offset == 22u * sector_size &&
                exact_behind_cap.modules.front().entry_offsets ==
                    std::vector<std::uint32_t>{0u},
            "Exakter Latent-AOT-Hint hinter dem Heuristik-Kandidatenlimit "
            "blieb ungebunden oder loeste eine unangeforderte Heuristik aus.");

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
                exact_only_options,
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
                exact_only_options,
                {},
                six_byte_hint);
        require(exact_six_byte.modules.size() == 1u &&
                    exact_six_byte.modules.front().byte_size == 6u,
                "Gerader exakter Sechs-Byte-Modulbound wurde wie ein "
                "Vierbyte-Heuristikkandidat abgelehnt.");

        auto exact_two_byte_limits = exact_only_options;
        exact_two_byte_limits.maximum_candidate_files = 0u;
        exact_two_byte_limits.maximum_file_bytes = 2u;
        exact_two_byte_limits.maximum_total_file_bytes = 2u;
        const auto empty_exact_only =
            katana::codegen::discover_latent_aot_modules(
                source,
                0u,
                0u,
                {},
                exact_two_byte_limits);
        require(empty_exact_only.examined_files == 0u &&
                    empty_exact_only.modules.empty(),
                "Leerer ExactOnly-Vertrag verlangte faelschlich "
                "Vierbyte-Heuristikbudgets oder untersuchte Dateien.");

        auto exact_file_bounded =
            exact_only_options;
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
            exact_only_options;
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

        auto oversized_runtime_template =
            katana::codegen::LatentAotDiscoveryOptions{};
        oversized_runtime_template.maximum_file_bytes =
            static_cast<std::size_t>(
                katana::runtime::maximum_native_aot_template_extent) +
            1u;
        rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    source,
                    0u,
                    0u,
                    {},
                    oversized_runtime_template));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(
            rejected,
            "Discovery akzeptierte ein Modulbudget oberhalb des "
            "Runtime-Template-Limits.");

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
