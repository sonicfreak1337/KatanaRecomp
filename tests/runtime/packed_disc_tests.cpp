#include "katana/io/input_provenance.hpp"
#include "katana/runtime/disc_install.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/iso9660.hpp"
#include "katana/runtime/packed_disc.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
constexpr std::size_t sector_size = 2048u;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function> void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

struct Fixture final {
    std::filesystem::path root = std::filesystem::current_path() / "katana-packed-disc-fixture";
    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "input" / "tracks");
        std::filesystem::create_directories(root / "output");
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void both32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
        bytes[offset + 4u + index] = static_cast<std::uint8_t>(value >> ((3u - index) * 8u));
    }
}

std::size_t record(std::vector<std::uint8_t>& bytes,
                   const std::size_t offset,
                   const std::uint32_t lba,
                   const std::uint32_t size,
                   const std::string& name,
                   const bool directory) {
    const auto length =
        static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    bytes[offset] = length;
    both32(bytes, offset + 2u, lba);
    both32(bytes, offset + 10u, size);
    bytes[offset + 25u] = directory ? 2u : 0u;
    bytes[offset + 28u] = 1u;
    bytes[offset + 31u] = 1u;
    bytes[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}

std::vector<std::uint8_t> data_track() {
    std::vector<std::uint8_t> bytes(24u * sector_size);
    const std::string hardware = "SEGA SEGAKATANA ";
    const std::string boot_file = "BOOT.BIN        ";
    std::copy(hardware.begin(), hardware.end(), bytes.begin());
    std::copy(boot_file.begin(), boot_file.end(), bytes.begin() + 0x60u);
    const auto pvd = 16u * sector_size;
    bytes[pvd] = 1u;
    std::copy_n("CD001", 5u, bytes.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    bytes[pvd + 6u] = 1u;
    record(bytes, pvd + 156u, 20u, sector_size, std::string(1u, '\0'), true);
    auto directory = 20u * sector_size;
    directory += record(bytes, directory, 20u, sector_size, std::string(1u, '\0'), true);
    directory += record(bytes, directory, 20u, sector_size, std::string(1u, '\1'), true);
    record(bytes, directory, 21u, 4u, "BOOT.BIN;1", false);
    bytes[21u * sector_size] = 0x0Bu;
    bytes[21u * sector_size + 1u] = 0x00u;
    bytes[21u * sector_size + 2u] = 0x09u;
    bytes[21u * sector_size + 3u] = 0x00u;
    return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path);

void put_u32(std::vector<std::uint8_t>& bytes,
             const std::size_t offset,
             const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
}

std::uint64_t get_u64(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    std::uint64_t value = 0u;
    for (std::size_t index = 0u; index < 8u; ++index)
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8u);
    return value;
}

void rewrite_metadata_hash(std::vector<std::uint8_t>& bytes) {
    constexpr std::size_t hash_offset = 144u;
    const auto metadata_size = static_cast<std::size_t>(get_u64(bytes, 48u));
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(hash_offset), 32u, std::uint8_t{0u});
    const auto hash_text = katana::io::sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), metadata_size));
    for (std::size_t index = 0u; index < 32u; ++index) {
        const auto hex = [](const char value) -> std::uint8_t {
            return value <= '9' ? static_cast<std::uint8_t>(value - '0')
                                : static_cast<std::uint8_t>(value - 'a' + 10);
        };
        bytes[hash_offset + index] = static_cast<std::uint8_t>((hex(hash_text[index * 2u]) << 4u) |
                                                               hex(hash_text[index * 2u + 1u]));
    }
}

void write_semantically_mutated_pack(const std::filesystem::path& source,
                                     const std::filesystem::path& destination,
                                     const std::size_t field_offset,
                                     const std::uint32_t value) {
    auto bytes = read_bytes(source);
    put_u32(bytes, field_offset, value);
    rewrite_metadata_hash(bytes);
    write_bytes(destination, bytes);
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string hash(const std::filesystem::path& path) {
    return katana::io::capture_input_provenance("synthetic", path).sha256;
}
} // namespace

int main() {
    using namespace katana::runtime;
    Fixture fixture;
    const auto input = fixture.root / "input";
    const auto audio_path = input / "tracks" / "audio.raw";
    const auto data_path = input / "tracks" / "data.bin";
    std::vector<std::uint8_t> audio(2u * 2352u);
    for (std::size_t index = 0u; index < audio.size(); ++index)
        audio[index] = static_cast<std::uint8_t>((index * 17u) & 0xFFu);
    write_bytes(audio_path, audio);
    write_bytes(data_path, data_track());
    const auto gdi_path = input / "disc.gdi";
    {
        std::ofstream descriptor(gdi_path, std::ios::trunc);
        descriptor << "2\n1 0 0 2352 tracks/audio.raw 0\n"
                      "2 10 4 2048 tracks/data.bin 0\n";
    }
    const auto gdi_hash_before = hash(gdi_path);
    const auto audio_hash_before = hash(audio_path);
    const auto data_hash_before = hash(data_path);
    auto gdi = GdiDiscSource::open(gdi_path);
    const auto pack_path = fixture.root / "output" / "game.katana-disc";
    const auto generation_a = std::string(64u, 'a');
    const auto info = write_packed_disc(*gdi, pack_path, generation_a);
    auto packed = PackedDiscSource::open(pack_path);
    packed->verify_all_chunks();
    require(info.tracks.size() == 2u && info.packed_sectors == 26u &&
                info.tracks[0].type == GdiTrackType::Audio &&
                info.tracks[1].type == GdiTrackType::Data && info.tracks[0].session == 1u &&
                info.tracks[1].session == 2u,
            "Mehrtrack-, Audio-, Datentrack- oder Sessionmetadaten fehlen im Pack.");
    require(gdi->read(10u * sector_size, 24u * sector_size) ==
                packed->read(10u * sector_size, 24u * sector_size),
            "GDI- und PackedDiscSource-Datensektoren unterscheiden sich.");
    require(gdi->read_raw_sector(1u, 1u) == packed->read_raw_sector(1u, 1u) &&
                gdi->read_raw_sectors(2u, 3u, 8u) == packed->read_raw_sectors(2u, 3u, 8u),
            "Raw-, Audio- oder gebuendelte Sektorreads unterscheiden sich.");
    require_failure([&] { static_cast<void>(packed->read(2u * sector_size, sector_size)); },
                    "Eine LBA-Luecke wurde als normaler Datensektor behandelt.");
    {
        Iso9660Filesystem gdi_iso(gdi, sector_size, 10u);
        Iso9660Filesystem packed_iso(packed, sector_size, 10u);
        require(gdi_iso.read_file("/BOOT.BIN") == packed_iso.read_file("/BOOT.BIN"),
                "ISO9660 liefert ueber PackedDiscSource abweichende Dateien.");
    }
    const auto pack_hash = hash(pack_path);
    const auto raw_pack = read_bytes(pack_path);
    const std::string pack_text(raw_pack.begin(), raw_pack.end());
    require(pack_text.find(fixture.root.string()) == std::string::npos &&
                pack_text.find("disc.gdi") == std::string::npos &&
                pack_text.find("audio.raw") == std::string::npos &&
                pack_text.find("data.bin") == std::string::npos,
            "Disc-Pack enthaelt Hostpfade oder urspruengliche Dateinamen.");
    const auto manifest = format_packed_disc_manifest(info, pack_hash);
    require(manifest.find(fixture.root.string()) == std::string::npos &&
                manifest.find("disc.gdi") == std::string::npos,
            "Disc-Pack-Manifest enthaelt Hostpfade oder Eingabenamen.");
    const auto recipe = make_disc_install_recipe(*gdi, generation_a, std::string(64u, 'c'));
    const auto recipe_path = fixture.root / "output" / "game.katana-install";
    {
        std::ofstream output(recipe_path, std::ios::binary | std::ios::trunc);
        output << format_disc_install_recipe(recipe);
    }
    const auto parsed_recipe = parse_disc_install_recipe(recipe_path);
    verify_disc_install_source(parsed_recipe, *gdi);
    const auto installed_path = fixture.root / "output" / "installed.katana-disc";
    const auto installed = install_disc_content(parsed_recipe, gdi_path, installed_path);
    require(installed.content_identity == info.content_identity &&
                hash(installed_path) == hash(pack_path) && read_bytes(recipe_path).size() < 4096u,
            "Generische Recipe installiert keinen identischen lokalen Contentcache.");
    auto wrong_recipe = parsed_recipe;
    wrong_recipe.tracks[0].lba += 1u;
    require_failure([&] { verify_disc_install_source(wrong_recipe, *gdi); },
                    "Semantisch falsche Trackgeometrie wurde vom Installer akzeptiert.");
    wrong_recipe = parsed_recipe;
    wrong_recipe.tracks[0].sha256 = std::string(64u, '0');
    require_failure([&] { verify_disc_install_source(wrong_recipe, *gdi); },
                    "Falscher Trackhash wurde vom Installer akzeptiert.");
    const auto recipe_text = format_disc_install_recipe(parsed_recipe);
    require(recipe_text.find(fixture.root.string()) == std::string::npos &&
                recipe_text.find("disc.gdi") == std::string::npos &&
                recipe_text.find("audio.raw") == std::string::npos,
            "Distributions-Recipe enthaelt Hostpfade oder Quelldateinamen.");

    const auto deterministic_path = fixture.root / "output" / "second.katana-disc";
    const auto deterministic = write_packed_disc(*gdi, deterministic_path, generation_a);
    require(hash(deterministic_path) == pack_hash &&
                deterministic.content_identity == info.content_identity,
            "Deterministischer Doppelbuild erzeugt einen abweichenden Disc-Pack.");
    const auto rejected_identity_path = fixture.root / "output" / "wrong-root.katana-disc";
    require_failure(
        [&] {
            static_cast<void>(write_packed_disc(
                *gdi, rejected_identity_path, generation_a, std::string(64u, '0')));
        },
        "Beim Schreiben neu hergeleitete Discbytes wurden nicht gegen die Recipe gebunden.");
    require(!std::filesystem::exists(rejected_identity_path) &&
                !std::filesystem::exists(
                    std::filesystem::path(rejected_identity_path.string() + ".katana-stage")),
            "Eine abweichende Content-Root wurde teilweise veroeffentlicht.");
    const auto generation_b_path = fixture.root / "output" / "new-generation.katana-disc";
    const auto generation_b = write_packed_disc(*gdi, generation_b_path, std::string(64u, 'b'));
    require(generation_b.content_identity == info.content_identity &&
                generation_b.job_generation != info.job_generation &&
                hash(generation_b_path) != pack_hash,
            "Jobgeneration bindet alten und neuen Disc-Pack nicht auseinander.");

    const auto corrupted = fixture.root / "output" / "corrupted.katana-disc";
    std::filesystem::copy_file(pack_path, corrupted);
    {
        std::fstream stream(corrupted, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(-1, std::ios::end);
        char value = 0;
        stream.read(&value, 1);
        value ^= 0x5Au;
        stream.seekp(-1, std::ios::end);
        stream.write(&value, 1);
    }
    require_failure(
        [&] {
            const auto source = PackedDiscSource::open(corrupted);
            source->verify_all_chunks();
        },
        "Ein beschaedigter Disc-Pack-Chunk wurde nicht erkannt.");
    const auto missing = fixture.root / "output" / "missing-chunk.katana-disc";
    std::filesystem::copy_file(pack_path, missing);
    std::filesystem::resize_file(missing, std::filesystem::file_size(missing) - 1u);
    require_failure([&] { static_cast<void>(PackedDiscSource::open(missing)); },
                    "Ein fehlender oder abgeschnittener Chunk wurde nicht erkannt.");
    constexpr std::size_t first_track = 192u;
    const auto unknown_payload = fixture.root / "output" / "unknown-payload.katana-disc";
    write_semantically_mutated_pack(pack_path, unknown_payload, first_track + 16u, 99u);
    require_failure(
        [&] { static_cast<void>(PackedDiscSource::open(unknown_payload)); },
        "Ein unbekannter Payload-Enumwert wurde trotz gueltigem Metadatenhash akzeptiert.");
    const auto tiny_sector = fixture.root / "output" / "tiny-sector.katana-disc";
    write_semantically_mutated_pack(pack_path, tiny_sector, first_track + 12u, 1u);
    require_failure([&] { static_cast<void>(PackedDiscSource::open(tiny_sector)); },
                    "Ein zu kleiner Sektor wurde trotz gueltigem Metadatenhash akzeptiert.");
    constexpr std::size_t second_track = first_track + 112u;
    const auto bad_offset = fixture.root / "output" / "bad-offset.katana-disc";
    write_semantically_mutated_pack(pack_path, bad_offset, second_track + 20u, 1u);
    require_failure([&] { static_cast<void>(PackedDiscSource::open(bad_offset)); },
                    "Ein falscher Payload-Offset wurde trotz gueltigem Metadatenhash akzeptiert.");
    const auto bad_combination = fixture.root / "output" / "bad-combination.katana-disc";
    write_semantically_mutated_pack(pack_path, bad_combination, second_track + 16u, 2u);
    require_failure([&] { static_cast<void>(PackedDiscSource::open(bad_combination)); },
                    "Eine ungueltige Sektorgroessen-/Payloadkombination wurde akzeptiert.");
    const auto blocked = fixture.root / "output" / "blocked.katana-disc";
    std::filesystem::create_directory(blocked);
    require_failure([&] { static_cast<void>(write_packed_disc(*gdi, blocked, generation_a)); },
                    "Unveroeffentlichbarer Pack wurde als Erfolg gemeldet.");
    require(std::filesystem::is_directory(blocked) &&
                !std::filesystem::exists(std::filesystem::path(blocked.string() + ".katana-stage")),
            "Atomarer Publish ersetzte das alte Ziel oder hinterliess Staging.");
    require(hash(gdi_path) == gdi_hash_before && hash(audio_path) == audio_hash_before &&
                hash(data_path) == data_hash_before,
            "Disc-Pack-Export hat GDI oder Trackdateien veraendert.");

    const auto portable_root = fixture.root / "portable-port";
    std::filesystem::create_directories(portable_root / "content");
    const auto portable_pack = portable_root / "content" / "game.katana-disc";
    std::filesystem::copy_file(pack_path, portable_pack);
    const auto read_only_hash = hash(portable_pack);
    packed.reset();
    gdi.reset();
    std::filesystem::remove_all(input);
    auto moved = PackedDiscSource::open(portable_pack);
    moved->verify_all_chunks();
    const auto boot =
        load_dreamcast_runtime_boot(moved,
                                    moved->primary_data_lba(),
                                    moved->info().tracks.size(),
                                    moved->info().content_identity);
    require(
        boot.boot_file == std::vector<std::uint8_t>({0x0Bu, 0x00u, 0x09u, 0x00u}) &&
            boot.repeated_reads_match && hash(portable_pack) == read_only_hash &&
            moved->chunk_cache_size() <= moved->chunk_cache_capacity(),
        "Verschobener synthetischer Port bootet nicht ohne Original-GDI oder veraendert Content.");

    std::cout << "Portable PackedDiscSource-Differenz- und Integritaetstests erfolgreich.\n";
    return EXIT_SUCCESS;
}
