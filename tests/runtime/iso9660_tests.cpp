#include "katana/runtime/iso9660.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t sector_size = 2048u;
void require(const bool value, const std::string& message) {
    if (!value) { std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<typename E, typename F> bool throws(F&& f) { try { f(); } catch (const E&) { return true; } return false; }
void both32(std::vector<std::uint8_t>& image, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t i = 0u; i < 4u; ++i) {
        image[offset + i] = static_cast<std::uint8_t>(value >> (i * 8u));
        image[offset + 4u + i] = static_cast<std::uint8_t>(value >> ((3u - i) * 8u));
    }
}
std::size_t record(
    std::vector<std::uint8_t>& image,
    const std::size_t offset,
    const std::uint32_t lba,
    const std::uint32_t size,
    const std::string& name,
    const bool directory
) {
    const auto length = static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    image[offset] = length;
    both32(image, offset + 2u, lba);
    both32(image, offset + 10u, size);
    image[offset + 25u] = directory ? 0x02u : 0u;
    image[offset + 28u] = 1u;
    image[offset + 31u] = 1u;
    image[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), image.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}
std::vector<std::uint8_t> make_iso() {
    std::vector<std::uint8_t> image(24u * sector_size);
    const auto pvd = 16u * sector_size;
    image[pvd] = 1u;
    std::copy_n("CD001", 5u, image.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    image[pvd + 6u] = 1u;
    record(image, pvd + 156u, 20u, sector_size, std::string(1u, '\0'), true);
    image[17u * sector_size] = 255u;
    std::copy_n("CD001", 5u, image.begin() + static_cast<std::ptrdiff_t>(17u * sector_size + 1u));
    image[17u * sector_size + 6u] = 1u;

    auto offset = 20u * sector_size;
    offset += record(image, offset, 20u, sector_size, std::string(1u, '\0'), true);
    offset += record(image, offset, 20u, sector_size, std::string(1u, '\1'), true);
    offset += record(image, offset, 21u, 5u, "HELLO.TXT;1", false);
    record(image, offset, 22u, sector_size, "SUBDIR", true);
    std::copy_n("HELLO", 5u, image.begin() + static_cast<std::ptrdiff_t>(21u * sector_size));

    offset = 22u * sector_size;
    offset += record(image, offset, 22u, sector_size, std::string(1u, '\0'), true);
    offset += record(image, offset, 20u, sector_size, std::string(1u, '\1'), true);
    record(image, offset, 23u, 4u, "NEST.BIN;1", false);
    image[23u * sector_size] = 1u;
    image[23u * sector_size + 1u] = 2u;
    image[23u * sector_size + 2u] = 3u;
    image[23u * sector_size + 3u] = 4u;
    return image;
}
}

int main() {
    using namespace katana::runtime;
    auto source = std::make_shared<MemoryDiscSource>(make_iso(), "synthetic:iso9660");
    Iso9660Filesystem filesystem(source);
    const auto root = filesystem.list_directory();
    require(root.size() == 2u && root[0].name == "HELLO.TXT" && !root[0].directory &&
        root[1].name == "SUBDIR" && root[1].directory,
        "ISO9660-Rootverzeichnis oder Versionssuffix-Normalisierung ist falsch.");
    require(filesystem.read_file("/hello.txt") == std::vector<std::uint8_t>({'H','E','L','L','O'}),
        "ISO9660-Datei wird nicht case-insensitive gelesen.");
    require(filesystem.read_file("/SUBDIR/NEST.BIN") == std::vector<std::uint8_t>({1u,2u,3u,4u}),
        "ISO9660-Unterverzeichnis wird nicht aufgeloest.");
    require(throws<std::out_of_range>([&] { static_cast<void>(filesystem.read_file("/MISSING.BIN")); }),
        "Fehlender ISO9660-Pfad wird akzeptiert.");
    auto broken = make_iso();
    broken[16u * sector_size + 1u] = 'X';
    require(throws<std::runtime_error>([&] {
        Iso9660Filesystem invalid(std::make_shared<MemoryDiscSource>(broken, "synthetic:broken-iso"));
    }), "Ungueltiger ISO9660-Descriptor wird akzeptiert.");

    std::cout << "KR-3003 ISO9660 erfolgreich.\n";
}
