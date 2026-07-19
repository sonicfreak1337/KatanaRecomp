#include "katana/runtime/disc_install.hpp"

#include "katana/io/input_provenance.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace katana::runtime {
namespace {
constexpr std::string_view recipe_magic = "KATANA-DISC-INSTALL";

template <typename Integer>
Integer parse_integer(const std::string_view text, const std::string_view field) {
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::runtime_error("Disc-Installations-Recipe besitzt ein ungueltiges Feld: " +
                                 std::string(field));
    return value;
}

void require_sha256(const std::string_view value, const std::string_view field) {
    if (value.size() != 64u)
        throw std::runtime_error("Disc-Installations-Recipe besitzt keinen gueltigen " +
                                 std::string(field) + ".");
    for (const auto character : value)
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F')))
            throw std::runtime_error("Disc-Installations-Recipe besitzt keinen gueltigen " +
                                     std::string(field) + ".");
}

std::vector<std::string> fields(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> result;
    for (std::string value; input >> value;)
        result.push_back(std::move(value));
    return result;
}

void validate_recipe(const DiscInstallRecipe& recipe) {
    if (recipe.version != disc_install_recipe_version || recipe.tracks.empty())
        throw std::runtime_error(
            "Disc-Installations-Recipe besitzt eine unbekannte Version oder keine Tracks.");
    require_sha256(recipe.job_generation, "Jobgeneration");
    require_sha256(recipe.descriptor_sha256, "Descriptor-SHA-256");
    require_sha256(recipe.content_identity, "Content-Identitaet");
    require_sha256(recipe.boot_sha256, "Boot-SHA-256");
    for (std::size_t index = 0u; index < recipe.tracks.size(); ++index) {
        const auto& track = recipe.tracks[index];
        if (track.number != index + 1u || track.sector_count == 0u ||
            (track.type != GdiTrackType::Audio && track.type != GdiTrackType::Data) ||
            (track.sector_size != 2048u && track.sector_size != 2336u &&
             track.sector_size != 2352u && track.sector_size != 2448u))
            throw std::runtime_error(
                "Disc-Installations-Recipe besitzt eine ungueltige Trackgeometrie.");
        require_sha256(track.sha256, "Track-SHA-256");
    }
}
} // namespace

DiscInstallRecipe make_disc_install_recipe(const GdiDiscSource& source,
                                           std::string job_generation,
                                           std::string boot_sha256) {
    try {
        require_sha256(job_generation, "Jobgeneration");
    } catch (const std::runtime_error&) {
        job_generation = katana::io::sha256_bytes(job_generation);
    }
    require_sha256(boot_sha256, "Boot-SHA-256");
    const auto& descriptor = source.descriptor();
    DiscInstallRecipe result;
    result.job_generation = std::move(job_generation);
    result.descriptor_sha256 = descriptor.sha256;
    result.boot_sha256 = std::move(boot_sha256);
    result.tracks.reserve(descriptor.tracks.size());
    for (const auto& source_track : descriptor.tracks) {
        DiscInstallTrack track{source_track.number,
                               source_track.lba,
                               source_track.type,
                               source_track.sector_size,
                               source_track.file_offset,
                               source_track.sector_count,
                               source_track.sha256};
        result.tracks.push_back(std::move(track));
    }
    result.content_identity = gdi_content_identity(descriptor);
    validate_recipe(result);
    return result;
}

std::string format_disc_install_recipe(const DiscInstallRecipe& recipe) {
    validate_recipe(recipe);
    std::ostringstream output;
    output << recipe_magic << ' ' << recipe.version << '\n'
           << "job " << recipe.job_generation << '\n'
           << "descriptor " << recipe.descriptor_sha256 << '\n'
           << "content " << recipe.content_identity << '\n'
           << "boot " << recipe.boot_sha256 << '\n'
           << "tracks " << recipe.tracks.size() << '\n';
    for (const auto& track : recipe.tracks)
        output << "track " << track.number << ' ' << track.lba << ' '
               << static_cast<unsigned>(track.type) << ' ' << track.sector_size << ' '
               << track.file_offset << ' ' << track.sector_count << ' ' << track.sha256 << '\n';
    return output.str();
}

DiscInstallRecipe parse_disc_install_recipe(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("Disc-Installations-Recipe ist nicht lesbar.");
    DiscInstallRecipe recipe;
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error("Disc-Installations-Recipe ist leer.");
    auto values = fields(line);
    if (values.size() != 2u || values[0] != recipe_magic)
        throw std::runtime_error("Disc-Installations-Recipe besitzt keinen gueltigen Header.");
    recipe.version = parse_integer<std::uint32_t>(values[1], "Version");
    const auto read_pair = [&](const std::string_view key) {
        if (!std::getline(input, line))
            throw std::runtime_error("Disc-Installations-Recipe ist abgeschnitten.");
        const auto pair = fields(line);
        if (pair.size() != 2u || pair[0] != key)
            throw std::runtime_error(
                "Disc-Installations-Recipe besitzt eine falsche Feldreihenfolge.");
        return pair[1];
    };
    recipe.job_generation = read_pair("job");
    recipe.descriptor_sha256 = read_pair("descriptor");
    recipe.content_identity = read_pair("content");
    recipe.boot_sha256 = read_pair("boot");
    const auto track_count = parse_integer<std::size_t>(read_pair("tracks"), "Trackanzahl");
    recipe.tracks.reserve(track_count);
    for (std::size_t index = 0u; index < track_count; ++index) {
        if (!std::getline(input, line))
            throw std::runtime_error(
                "Disc-Installations-Recipe ist in der Tracktabelle abgeschnitten.");
        values = fields(line);
        if (values.size() != 8u || values[0] != "track")
            throw std::runtime_error(
                "Disc-Installations-Recipe besitzt einen ungueltigen Trackeintrag.");
        const auto type = parse_integer<std::uint32_t>(values[3], "Tracktyp");
        if (type != 0u && type != 4u)
            throw std::runtime_error(
                "Disc-Installations-Recipe besitzt einen unbekannten Tracktyp.");
        recipe.tracks.push_back({parse_integer<std::uint32_t>(values[1], "Tracknummer"),
                                 parse_integer<std::uint32_t>(values[2], "LBA"),
                                 static_cast<GdiTrackType>(type),
                                 parse_integer<std::uint32_t>(values[4], "Sektorgroesse"),
                                 parse_integer<std::uint64_t>(values[5], "Dateioffset"),
                                 parse_integer<std::uint64_t>(values[6], "Sektoranzahl"),
                                 values[7]});
    }
    if (std::getline(input, line) && !line.empty())
        throw std::runtime_error("Disc-Installations-Recipe besitzt unerwartete Zusatzdaten.");
    validate_recipe(recipe);
    return recipe;
}

void verify_disc_install_source(const DiscInstallRecipe& recipe, const GdiDiscSource& source) {
    validate_recipe(recipe);
    const auto& actual = source.descriptor();
    if (actual.sha256 != recipe.descriptor_sha256 || actual.tracks.size() != recipe.tracks.size())
        throw std::runtime_error(
            "Originaldisc stimmt nicht mit der Installations-Recipe ueberein.");
    for (std::size_t index = 0u; index < actual.tracks.size(); ++index) {
        const auto& left = recipe.tracks[index];
        const auto& right = actual.tracks[index];
        if (left.number != right.number || left.lba != right.lba || left.type != right.type ||
            left.sector_size != right.sector_size || left.file_offset != right.file_offset ||
            left.sector_count != right.sector_count || left.sha256 != right.sha256)
            throw std::runtime_error(
                "Originaldisc-Track stimmt nicht mit der Installations-Recipe ueberein.");
    }
    const auto rebuilt =
        make_disc_install_recipe(source, recipe.job_generation, recipe.boot_sha256);
    if (rebuilt.content_identity != recipe.content_identity)
        throw std::runtime_error("Originaldisc besitzt eine abweichende Content-Identitaet.");
}

PackedDiscInfo install_disc_content(const DiscInstallRecipe& recipe,
                                    const std::filesystem::path& gdi_path,
                                    const std::filesystem::path& destination) {
    const auto source = GdiDiscSource::open(gdi_path);
    verify_disc_install_source(recipe, *source);
    return write_packed_disc(*source, destination, recipe.job_generation);
}

} // namespace katana::runtime
