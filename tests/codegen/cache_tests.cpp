#include "katana/codegen/cache.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Fixture {
    std::filesystem::path path = std::filesystem::current_path() / "katana-codegen-cache-fixture";
    std::filesystem::path external =
        std::filesystem::current_path() /
        "katana-codegen-cache-external-fixture";
    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        error.clear();
        std::filesystem::remove_all(external, error);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        error.clear();
        std::filesystem::remove_all(external, error);
    }
};

std::optional<std::filesystem::path> find_regular_file_containing(
    const std::filesystem::path& root,
    const std::string_view needle) {
    if (!std::filesystem::exists(root))
        return std::nullopt;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream input(entry.path(), std::ios::binary);
        const std::string content{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        if (!input.bad() &&
            content.find(needle) != std::string::npos)
            return entry.path();
    }
    return std::nullopt;
}

} // namespace

int main() {
    using namespace katana::codegen;
    Fixture fixture;
    CodegenCache cache(fixture.path);
    CodegenCacheInputs inputs{"input-a",
                              "ir-a",
                              "opt-a",
                              "cpp",
                              1u,
                              8u,
                              "manifest-a",
                              "overrides-a",
                               2u,
                               1u,
                               "0.34.0-dev",
                               "exporter-fixture-a"};
    const auto key = make_codegen_cache_key(inputs);
    require(key.starts_with("cg-v5-") && key.size() == 6u + 64u,
            "Codegen-Cache verwendet keinen kanonischen SHA-256-Schluessel.");
    require(!cache.load(key, "unit.cpp"), "Leerer Cache meldet einen Treffer.");
    cache.store(key, "unit.cpp", "generated-a\n");
    require(cache.load(key, "unit.cpp") == std::optional<std::string>("generated-a\n"),
            "Gespeichertes Artefakt ist kein bytegleicher Cachetreffer.");
    CodegenCache nested_cache(
        fixture.path / "missing-parent" / "cache-root");
    nested_cache.store_bounded(
        key, "nested.bin", "nested", 8u);
    require(
        nested_cache.load_bounded(
            key, "nested.bin", 8u) ==
            std::optional<std::string>{"nested"},
        "Begrenzter Cache legte fehlende sichere Eltern seines "
        "Stammverzeichnisses nicht an.");
    require(
        cache.load_bounded(key, "unit.cpp", 12u) ==
            std::optional<std::string>("generated-a\n"),
        "Begrenzter Cache-Read verlor ein exakt passendes Artefakt.");
    require(
        !cache.load_bounded(key, "unit.cpp", 11u),
        "Begrenzter Cache-Read las ein Artefakt oberhalb des Bytebudgets.");
    bool rejected_zero_budget = false;
    try {
        static_cast<void>(
            cache.load_bounded(key, "unit.cpp", 0u));
    } catch (const std::invalid_argument&) {
        rejected_zero_budget = true;
    }
    require(
        rejected_zero_budget,
        "Begrenzter Cache-Read akzeptierte ein Nullbudget.");
    cache.store_bounded(
        key, "bounded.bin", "old", 8u);
    const auto bounded_old =
        cache.load_bounded(key, "bounded.bin", 8u);
    require(
        bounded_old == std::optional<std::string>{"old"} &&
            cache.erase_bounded_if_matches(
                key, "bounded.bin", *bounded_old, 8u),
        "Begrenzte Cache-Reparatur entfernte ihr exakt gelesenes "
        "Artefakt nicht.");
    cache.store_bounded(
        key, "bounded.bin", "repaired", 8u);
    require(
        cache.load_bounded(key, "bounded.bin", 8u) ==
            std::optional<std::string>{"repaired"},
        "Begrenzter Cache-Publish reparierte kein entferntes Artefakt.");
    cache.store_integrity_bounded(
        key, "integrity.bin", "trusted-payload", 32u);
    require(
        cache.load_integrity_bounded(
            key, "integrity.bin", 32u) ==
            std::optional<std::string>{"trusted-payload"},
        "Integritaetsgebundener Cache verlor einen gueltigen Payload.");
    const auto integrity_path =
        find_regular_file_containing(
            cache.root(), "trusted-payload");
    require(
        integrity_path.has_value(),
        "Integritaetsgebundene Cache-Fixture fand ihr Artefakt nicht.");
    {
        std::fstream artifact(
            *integrity_path,
            std::ios::binary | std::ios::in | std::ios::out);
        artifact.seekg(-1, std::ios::end);
        char last = 0;
        artifact.read(&last, 1);
        last ^= 0x01;
        artifact.seekp(-1, std::ios::end);
        artifact.write(&last, 1);
    }
    require(
        !cache.load_integrity_bounded(
            key, "integrity.bin", 32u),
        "Payload-Bitrot wurde als integritaetsgebundener Cachetreffer "
        "akzeptiert.");
    cache.store_integrity_bounded(
        key, "integrity.bin", "trusted-payload", 32u);
    require(
        cache.load_integrity_bounded(
            key, "integrity.bin", 32u) ==
            std::optional<std::string>{"trusted-payload"},
        "Integritaetsgebundener Cache reparierte ein exakt erkanntes "
        "kaputtes Artefakt nicht.");
    cache.store_integrity_bounded(
        key, "bound-a.bin", "payload-a", 32u);
    cache.store_integrity_bounded(
        key, "bound-b.bin", "payload-b", 32u);
    const auto bound_a = cache.root() / key / "bound-a.bin";
    const auto bound_b = cache.root() / key / "bound-b.bin";
    const auto bound_swap = cache.root() / key / "bound-swap.tmp";
    std::filesystem::rename(bound_a, bound_swap);
    std::filesystem::rename(bound_b, bound_a);
    std::filesystem::rename(bound_swap, bound_b);
    require(
        !cache.load_integrity_bounded(
            key, "bound-a.bin", 32u) &&
            !cache.load_integrity_bounded(
                key, "bound-b.bin", 32u),
        "Integritaetsenvelope akzeptierte zwischen Artefaktnamen "
        "vertauschte Payloads.");
    cache.store_bounded(
        key, "legacy-raw.bin", "legacy-payload", 32u);
    require(
        !cache.load_integrity_bounded(
            key, "legacy-raw.bin", 32u),
        "Ungepruefter Legacy-Payload wurde als integritaetsgebunden "
        "akzeptiert.");
    cache.store_integrity_bounded(
        key, "legacy-raw.bin", "verified-payload", 32u);
    require(
        cache.load_integrity_bounded(
            key, "legacy-raw.bin", 32u) ==
            std::optional<std::string>{"verified-payload"},
        "Integritaetsgebundener Cache ersetzte ein ungebundenes "
        "Legacy-Artefakt nicht sicher.");
    cache.store(key, "oversized.bin", "oversized");
    cache.store_bounded(
        key, "oversized.bin", "new", 3u);
    require(
        cache.load_bounded(key, "oversized.bin", 3u) ==
            std::optional<std::string>{"new"},
        "Begrenzter Cache-Publish reparierte ein sicher erkanntes "
        "uebergrosses Artefakt nicht.");

    std::filesystem::create_directories(fixture.path);
    std::filesystem::create_directories(fixture.external);
    {
        std::ofstream external(
            fixture.external / "artifact.bin",
            std::ios::binary | std::ios::trunc);
        external << "external";
    }
    std::error_code link_error;
    std::filesystem::create_directory_symlink(
        fixture.external, fixture.path / "linked-key", link_error);
    if (!link_error) {
        require(
            !cache.load_bounded("linked-key", "artifact.bin", 8u) &&
                !cache.erase_bounded_if_matches(
                    "linked-key", "artifact.bin", "external", 8u),
            "Begrenzter Cache folgte einem verlinkten Key-Ordner beim "
            "Lesen oder Entfernen.");
        bool rejected_link_publish = false;
        try {
            cache.store_bounded(
                "linked-key", "artifact.bin", "replaced", 8u);
        } catch (const std::runtime_error&) {
            rejected_link_publish = true;
        }
        std::ifstream external(
            fixture.external / "artifact.bin", std::ios::binary);
        std::string external_content;
        external >> external_content;
        require(
            rejected_link_publish && external_content == "external",
            "Begrenzter Cache schrieb durch einen verlinkten Key-Ordner "
            "oder veraenderte das externe Ziel.");

        const auto linked_artifact =
            cache.root() / key / "linked-artifact.bin";
        link_error.clear();
        std::filesystem::create_symlink(
            fixture.external / "artifact.bin",
            linked_artifact,
            link_error);
        if (!link_error) {
            require(
                !cache.load_bounded(
                    key, "linked-artifact.bin", 8u) &&
                    !cache.erase_bounded_if_matches(
                        key,
                        "linked-artifact.bin",
                        "external",
                        8u),
                "Begrenzter Cache folgte einem verlinkten Artefakt "
                "beim Lesen oder Entfernen.");
            bool rejected_artifact_link_publish = false;
            try {
                cache.store_bounded(
                    key,
                    "linked-artifact.bin",
                    "replaced",
                    8u);
            } catch (const std::runtime_error&) {
                rejected_artifact_link_publish = true;
            }
            std::ifstream linked_external(
                fixture.external / "artifact.bin",
                std::ios::binary);
            std::string linked_external_content;
            linked_external >> linked_external_content;
            require(
                rejected_artifact_link_publish &&
                    linked_external_content == "external",
                "Begrenzter Cache schrieb durch einen "
                "Artefaktlink oder veraenderte dessen externes Ziel.");
        }

        const auto linked_root = fixture.path / "linked-root";
        std::filesystem::create_directories(
            fixture.external / "key");
        {
            std::ofstream external_root_artifact(
                fixture.external / "key" / "artifact.bin",
                std::ios::binary | std::ios::trunc);
            external_root_artifact << "external";
        }
        link_error.clear();
        std::filesystem::create_directory_symlink(
            fixture.external, linked_root, link_error);
        if (!link_error) {
            katana::codegen::CodegenCache linked_root_cache(
                linked_root);
            require(
                !linked_root_cache.load_bounded(
                    "key", "artifact.bin", 8u),
                "Begrenzter Cache folgte einem verlinkten Cache-Stamm.");
            bool rejected_linked_root_publish = false;
            try {
                linked_root_cache.store_bounded(
                    "key", "artifact.bin", "replaced", 8u);
            } catch (const std::runtime_error&) {
                rejected_linked_root_publish = true;
            }
            require(
                rejected_linked_root_publish,
                "Begrenzter Cache publizierte durch einen verlinkten "
                "Cache-Stamm.");
        }
    }

    const auto original_time = std::filesystem::last_write_time(cache.root() / key / "unit.cpp");
    cache.store(key, "unit.cpp", "generated-a\n");
    require(std::filesystem::last_write_time(cache.root() / key / "unit.cpp") == original_time,
            "Bytegleicher Cachetreffer wird unnoetig neu geschrieben.");

    inputs.ir_hash = "ir-b";
    const auto changed_key = make_codegen_cache_key(inputs);
    require(changed_key != key && !cache.load(changed_key, "unit.cpp"),
            "IR-Aenderung invalidiert das betroffene Artefakt nicht.");

    const auto changed_tool = [&] {
        auto value = inputs;
        value.ir_hash = "ir-a";
        value.tool_version = "0.35.0-dev";
        return make_codegen_cache_key(value);
    }();
    require(changed_tool != key, "Werkzeugversion invalidiert den Cache nicht.");
    const auto changed_implementation = [&] {
        auto value = inputs;
        value.ir_hash = "ir-a";
        value.implementation_identity = "exporter-fixture-b";
        return make_codegen_cache_key(value);
    }();
    require(changed_implementation != key,
            "Exakte Exporterimplementierung invalidiert den Cache nicht.");

    const auto long_cache_root =
        fixture.path /
        "long-root-component-0123456789abcdef0123456789abcdef0123456789";
    CodegenCache long_path_cache(long_cache_root);
    const std::string long_artifact_name(
        120u, 'a');
    long_path_cache.store(key, long_artifact_name, "long-path-content\n");
    require(
        long_path_cache.load(key, long_artifact_name) ==
            std::optional<std::string>("long-path-content\n"),
        "Kompaktes physisches Cachelayout verliert lange Windows-Portpfade.");

    bool rejected = false;
    try {
        static_cast<void>(cache.load(key, "../unit.cpp"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Cache erlaubt Pfadausbruch ueber Artefaktnamen.");

    std::vector<std::thread> publishers;
    std::mutex publisher_failure_mutex;
    std::string publisher_failure;
    for (std::size_t index = 0u; index < 8u; ++index) {
        publishers.emplace_back([&] {
            try {
                cache.store(
                    key,
                    "concurrent.cpp",
                    "stable-content\n");
            } catch (const std::exception& error) {
                const std::lock_guard lock(
                    publisher_failure_mutex);
                if (publisher_failure.empty())
                    publisher_failure = error.what();
            }
        });
    }
    for (auto& publisher : publishers)
        publisher.join();
    require(publisher_failure.empty(),
            "Paralleler atomarer Publish warf eine Ausnahme: " +
                publisher_failure);
    require(cache.load(key, "concurrent.cpp") == std::optional<std::string>("stable-content\n") &&
                std::none_of(std::filesystem::directory_iterator(cache.root() / key),
                             std::filesystem::directory_iterator{},
                             [](const auto& entry) {
                                 return entry.path().filename().string().starts_with(".publish-");
                             }),
            "Paralleler atomarer Publish hinterlaesst Teilstand oder Stagingdaten.");

    std::cout << "KR-3303 inkrementeller Codegen-Cache erfolgreich.\n";
    return 0;
}
