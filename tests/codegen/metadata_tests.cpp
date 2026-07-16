#include "katana/codegen/metadata.hpp"
#include "katana/codegen/partition.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

katana::ir::Function function(const std::uint32_t entry) {
    katana::ir::Function value;
    value.entry_address = entry;
    katana::ir::BasicBlock block;
    block.start_address = entry;
    katana::ir::Instruction instruction;
    instruction.source_address = entry;
    instruction.original_opcode = 0x0009u;
    instruction.original_operation = katana::ir::Operation::Nop;
    instruction.operation = katana::ir::Operation::Nop;
    block.instructions.push_back(instruction);
    value.blocks.push_back(std::move(block));
    return value;
}

std::vector<std::array<std::uint64_t, 4u>> partition_summary(
    const std::vector<katana::ir::Function>& functions
) {
    const auto partitions = katana::codegen::partition_translation_units(
        functions, {128u, 4096u}
    );
    std::vector<std::array<std::uint64_t, 4u>> result;
    for (const auto& partition : partitions) {
        result.push_back({
            partition.first_entry_address,
            partition.last_entry_address,
            partition.function_indices.size(),
            partition.instruction_count
        });
    }
    return result;
}

std::map<std::string, std::string> snapshot(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) { continue; }
        std::ifstream input(entry.path(), std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        result.emplace(entry.path().lexically_relative(root).generic_string(), content.str());
    }
    return result;
}

struct Fixture {
    std::filesystem::path root = std::filesystem::current_path() / "katana-metadata-fixture";
    Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

} // namespace

int main() {
    using namespace katana::codegen;
    using katana::runtime::BlockAddress;
    using katana::runtime::BlockEndKind;

    std::vector<katana::ir::Function> functions;
    std::vector<BlockMetadata> blocks;
    functions.reserve(10'000u);
    blocks.reserve(10'000u);
    for (std::uint32_t index = 0u; index < 10'000u; ++index) {
        const auto virtual_address = 0x8C000000u + index * 2u;
        const auto physical_address = 0x0C000000u + index * 2u;
        functions.push_back(function(virtual_address));
        blocks.push_back({
            {virtual_address, physical_address},
            "main-executable",
            static_cast<std::uint64_t>(index) * 2u,
            2u,
            BlockProvenance::ImageSegment,
            {0x0009u},
            1u,
            index + 1u == 10'000u ? BlockEndKind::Return : BlockEndKind::Fallthrough,
            index + 1u == 10'000u
                ? std::vector<BlockAddress>{}
                : std::vector<BlockAddress>{{virtual_address + 2u, physical_address + 2u}},
            block_state_guard(BlockStateGuard::Mmu) |
                block_state_guard(BlockStateGuard::Fpscr)
        });
    }
    const auto first_partitioning = partition_summary(functions);
    std::reverse(functions.begin(), functions.end());
    const auto second_partitioning = partition_summary(functions);
    require(
        first_partitioning == second_partitioning && first_partitioning.size() == 79u,
        "10.000-Block-Projekt wird nicht reproduzierbar partitioniert."
    );

    const auto metadata = serialize_block_metadata(blocks, "cpp", 1u);
    std::reverse(blocks.begin(), blocks.end());
    require(
        metadata == serialize_block_metadata(blocks, "cpp", 1u) &&
            metadata.find("\"schema_version\":1") != std::string::npos &&
            metadata.find("\"physical_address\":\"0x0C000000\"") != std::string::npos &&
            metadata.find("main-executable") != std::string::npos,
        "Blockmetadaten sind nicht kanonisch, versioniert oder adressvollstaendig."
    );

    Fixture fixture;
    auto artifacts = make_separated_codegen_artifacts(
        {{"unit-a.cpp", "int a() { return 1; }\n"}},
        std::string("\x01\x02", 2u),
        "{\"symbols\":[]}\n",
        metadata
    );
    static_cast<void>(write_codegen_project(fixture.root / "serial", artifacts, {1u}));
    static_cast<void>(write_codegen_project(fixture.root / "parallel", artifacts, {8u}));
    const auto serial = snapshot(fixture.root / "serial");
    require(
        serial == snapshot(fixture.root / "parallel") &&
            serial.contains("code/unit-a.cpp") && serial.contains("data/constants.bin") &&
            serial.contains("symbols/symbols.json") && serial.contains("metadata/blocks.json"),
        "Serielle/parallele Metadaten oder getrennte Artefaktklassen unterscheiden sich."
    );
    std::ostringstream host_pointer;
    host_pointer << static_cast<const void*>(blocks.data());
    require(
        metadata.find(fixture.root.string()) == std::string::npos &&
            metadata.find(host_pointer.str()) == std::string::npos,
        "Metadaten enthalten lokalen Pfad oder Hostadresse."
    );

    CodegenCache cache(fixture.root / "cache");
    const CodegenCacheInputs base{
        "input", "ir", "optimization", "cpp", 1u, 8u,
        "manifest", "overrides", 2u, 1u
    };
    const auto base_key = make_codegen_cache_key(base);
    cache.store(base_key, "metadata/blocks.json", metadata);
    std::vector<CodegenCacheInputs> variants(10u, base);
    variants[0].input_hash = "input-2";
    variants[1].ir_hash = "ir-2";
    variants[2].configuration_hash = "optimization-2";
    variants[3].backend_name = "cpp-2";
    variants[4].backend_abi = 2u;
    variants[5].runtime_abi = 9u;
    variants[6].manifest_hash = "manifest-2";
    variants[7].overrides_hash = "overrides-2";
    variants[8].ir_version = 3u;
    variants[9].optimization_version = 2u;
    for (const auto& variant : variants) {
        const auto key = make_codegen_cache_key(variant);
        require(
            key != base_key && !cache.load(key, "metadata/blocks.json") &&
                cache.load(base_key, "metadata/blocks.json").has_value(),
            "Cache-Schluesselkomponente invalidiert nicht gezielt das betroffene Artefakt."
        );
    }

    blocks.front().source_segment = "C:\\private\\disc";
    bool rejected = false;
    try {
        static_cast<void>(serialize_block_metadata(blocks, "cpp", 1u));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Lokaler Pfad wird als Quellsegment akzeptiert.");

    std::cout << "KR-3305 deterministische Blockmetadaten erfolgreich.\n";
    return 0;
}
