#include "katana/codegen/cache.hpp"

#include <cstdlib>
#include <filesystem>
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

struct Fixture {
    std::filesystem::path path = std::filesystem::current_path() / "katana-codegen-cache-fixture";
    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

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
                              "0.34.0-dev"};
    const auto key = make_codegen_cache_key(inputs);
    require(!cache.load(key, "unit.cpp"), "Leerer Cache meldet einen Treffer.");
    cache.store(key, "unit.cpp", "generated-a\n");
    require(cache.load(key, "unit.cpp") == std::optional<std::string>("generated-a\n"),
            "Gespeichertes Artefakt ist kein bytegleicher Cachetreffer.");

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

    bool rejected = false;
    try {
        static_cast<void>(cache.load(key, "../unit.cpp"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Cache erlaubt Pfadausbruch ueber Artefaktnamen.");

    std::cout << "KR-3303 inkrementeller Codegen-Cache erfolgreich.\n";
    return 0;
}
