#include "katana/platform/dreamcast_disc.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/io/input_output_error.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace katana::platform {
namespace {

constexpr std::string_view dreamcast_boot_executable_artifact_magic =
    "KATANA-BOOT-EXECUTABLE";

std::string trimmed_ascii(const std::span<const std::uint8_t> bytes,
                          const std::size_t offset,
                          const std::size_t length) {
    if (offset > bytes.size() || length > bytes.size() - offset) {
        throw std::invalid_argument("Dreamcast-Bootmetadaten sind abgeschnitten.");
    }
    std::string result(reinterpret_cast<const char*>(bytes.data() + offset), length);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\0')) {
        result.pop_back();
    }
    return result;
}

bool safe_iso_file_name(const std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find('/') == std::string_view::npos &&
           value.find('\\') == std::string_view::npos && value.find(':') == std::string_view::npos;
}

bool canonical_sha256(const std::string_view value) noexcept {
    if (value.size() != 64u) return false;
    for (const auto character : value)
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f')))
            return false;
    return true;
}

template <typename Integer>
Integer parse_decimal(const std::string_view value,
                      const std::string_view field) {
    Integer result{};
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), result, 10);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size())
        throw std::runtime_error(
            "Boot-Executable-Artefakt besitzt ein ungueltiges Feld: " +
            std::string(field));
    return result;
}

std::vector<std::string> artifact_fields(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> result;
    for (std::string value; input >> value;)
        result.push_back(std::move(value));
    return result;
}

std::string hex_encode(const std::string_view value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2u);
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(digits[byte >> 4u]);
        result.push_back(digits[byte & 0x0Fu]);
    }
    return result;
}

std::string hex_decode(const std::string_view value) {
    const auto digit = [](const char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        return -1;
    };
    if ((value.size() & 1u) != 0u)
        throw std::runtime_error(
            "Boot-Executable-Artefakt besitzt einen ungueltigen Bootdateinamen.");
    std::string result;
    result.reserve(value.size() / 2u);
    for (std::size_t offset = 0u; offset < value.size(); offset += 2u) {
        const auto high = digit(value[offset]);
        const auto low = digit(value[offset + 1u]);
        if (high < 0 || low < 0)
            throw std::runtime_error(
                "Boot-Executable-Artefakt besitzt einen ungueltigen Bootdateinamen.");
        result.push_back(static_cast<char>((high << 4u) | low));
    }
    if (!safe_iso_file_name(result))
        throw std::runtime_error(
            "Boot-Executable-Artefakt besitzt einen unsicheren Bootdateinamen.");
    return result;
}

std::string boot_sha256(const std::span<const std::uint8_t> bytes) {
    return io::sha256_bytes(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void require_regular_nonsymlink_file(const std::filesystem::path& path,
                                     const std::string_view description) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        throw std::runtime_error(std::string(description) +
                                 " ist keine sichere regulaere Datei.");
}

void write_immutable_file(const std::filesystem::path& path,
                          const std::span<const std::uint8_t> bytes) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    const bool missing =
        status_error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found;
    if (!missing && !status_error) {
        if (!std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status))
            throw std::runtime_error(
                "Boot-Executable-Artefaktziel ist keine regulaere Datei.");
        if (io::read_binary_file(path) !=
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()))
            throw std::runtime_error(
                "Ein bestehendes Boot-Executable-Artefakt weicht von der eigenen Disc ab.");
        return;
    }
    if (!missing)
        throw std::runtime_error(
            "Boot-Executable-Artefaktziel konnte nicht sicher geprueft werden.");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw io::InputOutputError(
            "Boot-Executable-Artefakt konnte nicht angelegt werden.");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output)
        throw io::InputOutputError(
            "Boot-Executable-Artefakt konnte nicht vollstaendig geschrieben werden.");
}

void write_immutable_file(const std::filesystem::path& path,
                          const std::string_view content) {
    write_immutable_file(
        path,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(content.data()),
            content.size()));
}

void require_valid_boot_artifact(const DreamcastBootExecutableArtifact& artifact) {
    if (artifact.version != dreamcast_boot_executable_artifact_version ||
        artifact.entry_address != dreamcast_disc_boot_address ||
        artifact.boot_file.empty() ||
        artifact.boot_file.size() >
            runtime::dreamcast_main_ram_size -
                static_cast<std::size_t>(
                    dreamcast_disc_boot_address - 0x8C000000u) ||
        artifact.metadata.hardware_id != "SEGA SEGAKATANA" ||
        !safe_iso_file_name(artifact.metadata.boot_file_name) ||
        !canonical_sha256(artifact.project_identity) ||
        !canonical_sha256(artifact.boot_sha256) ||
        !canonical_sha256(artifact.install_recipe.content_identity) ||
        artifact.boot_sha256 != boot_sha256(artifact.boot_file) ||
        artifact.install_recipe.job_generation != artifact.project_identity ||
        artifact.install_recipe.content_identity.empty() ||
        artifact.install_recipe.boot_sha256 != artifact.boot_sha256)
        throw std::invalid_argument(
            "Boot-Executable-Artefakt verletzt seinen Hash-/Adressvertrag.");
}

} // namespace

DreamcastBootMetadata parse_dreamcast_boot_metadata(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 0x70u) {
        throw std::invalid_argument("Dreamcast-Bootmetadaten sind abgeschnitten.");
    }
    DreamcastBootMetadata result{trimmed_ascii(bytes, 0x00u, 16u),
                                 trimmed_ascii(bytes, 0x60u, 16u)};
    if (result.hardware_id != "SEGA SEGAKATANA") {
        throw std::invalid_argument("Dreamcast-Hardwarekennung ist ungueltig.");
    }
    if (!safe_iso_file_name(result.boot_file_name)) {
        throw std::invalid_argument("Dreamcast-Bootdateiname ist ungueltig.");
    }
    return result;
}

DreamcastDiscBoot load_dreamcast_gdi_boot(
    const std::filesystem::path& descriptor_path,
    const ProgressReporter& progress) {
    auto boot = runtime::load_dreamcast_runtime_boot(
        descriptor_path,
        progress);
    auto source = std::dynamic_pointer_cast<runtime::GdiDiscSource>(boot.source);
    if (!source) throw std::logic_error("GDI-Boot besitzt keine GDI-DiscSource.");
    return {std::move(source),
            {std::move(boot.hardware_id), std::move(boot.boot_file_name)},
            std::move(boot.system_bootstrap),
            std::move(boot.boot_file),
            boot.data_track_lba,
            boot.extent_lba_bias,
            boot.validated_tracks,
            boot.repeated_bootstrap_reads_match,
            boot.repeated_reads_match};
}

io::ExecutableImage make_dreamcast_disc_executable(const DreamcastDiscBoot& disc) {
    return make_dreamcast_disc_executable(disc, DreamcastDiscExecutionPath::DirectBootFile);
}

io::ExecutableImage make_dreamcast_disc_executable(
    const DreamcastDiscBoot& disc, const DreamcastDiscExecutionPath execution_path) {
    if (disc.system_bootstrap.size() != runtime::dreamcast_system_bootstrap_size ||
        disc.boot_file.empty()) {
        throw std::invalid_argument("Dreamcast-Bootdatei ist leer.");
    }
    io::ExecutableImage image;
    image.set_guest_call_abi(io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_address_model(io::ImageAddressModel::Sh4DirectMapped);
    io::ImageSegment bootstrap_segment{".disc-bootstrap",
                                       dreamcast_system_bootstrap_address,
                                       0u,
                                       disc.system_bootstrap.size(),
                                       io::SegmentKind::Mixed,
                                       {true, true, true},
                                       disc.system_bootstrap};
    bootstrap_segment.source_kind = io::ImageSourceKind::DiscBootFile;
    bootstrap_segment.load_phase = io::ImageLoadPhase::Initial;
    bootstrap_segment.local_source_name = "disc-system-bootstrap";
    image.add_segment(std::move(bootstrap_segment));
    io::ImageSegment boot_segment{".text",
                                  dreamcast_disc_boot_address,
                                  0u,
                                  disc.boot_file.size(),
                                  io::SegmentKind::Mixed,
                                  {true, true, true},
                                  disc.boot_file};
    boot_segment.source_kind = io::ImageSourceKind::DiscBootFile;
    boot_segment.load_phase = io::ImageLoadPhase::Initial;
    boot_segment.local_source_name = disc.metadata.boot_file_name;
    image.add_segment(std::move(boot_segment));
    if (execution_path == DreamcastDiscExecutionPath::NativeSystemBootstrap) {
        image.add_entry_point(dreamcast_system_bootstrap_entry_address);
        image.add_entry_point(dreamcast_disc_boot_address);
        image.set_initial_snapshot_entry(dreamcast_system_bootstrap_entry_address);
    } else {
        image.add_entry_point(dreamcast_disc_boot_address);
        image.set_initial_snapshot_entry(dreamcast_disc_boot_address);
    }
    return image;
}

std::string dreamcast_disc_project_identity(const DreamcastDiscBoot& disc) {
    if (!disc.source)
        throw std::invalid_argument(
            "Dreamcast-Disc besitzt keine Quelle fuer die Projektidentitaet.");
    const auto& descriptor = disc.source->descriptor();
    std::ostringstream material;
    material << "gdi-descriptor:" << descriptor.size << ':'
             << descriptor.sha256 << '\n';
    for (const auto& track : descriptor.tracks)
        material << "gdi-track-" << track.number << ':'
                 << track.file_offset +
                        track.sector_count *
                            static_cast<std::uint64_t>(track.sector_size)
                 << ':' << track.sha256 << '\n';
    return io::sha256_bytes(material.str());
}

DreamcastBootExecutableArtifact
extract_dreamcast_boot_executable_artifact(
    const std::filesystem::path& descriptor_path,
    const std::filesystem::path& artifact_root) {
    if (descriptor_path.empty() || artifact_root.empty())
        throw std::invalid_argument(
            "Boot-Executable-Extraktion braucht GDI und privaten Artefaktpfad.");
    const auto disc = load_dreamcast_gdi_boot(descriptor_path);
    if (!disc.repeated_bootstrap_reads_match || !disc.repeated_reads_match)
        throw std::runtime_error(
            "Dreamcast-Disc lieferte keine wiederholbar identischen Bootdaten.");
    const auto project_identity = dreamcast_disc_project_identity(disc);
    const auto executable_sha256 = boot_sha256(disc.boot_file);
    const auto recipe = runtime::make_disc_install_recipe(
        *disc.source, project_identity, executable_sha256);

    const auto root = std::filesystem::absolute(artifact_root).lexically_normal();
    std::error_code root_error;
    const auto root_status = std::filesystem::symlink_status(root, root_error);
    const bool root_missing =
        root_error == std::errc::no_such_file_or_directory ||
        root_status.type() == std::filesystem::file_type::not_found;
    if (!root_missing && !root_error) {
        if (!std::filesystem::is_directory(root_status) ||
            std::filesystem::is_symlink(root_status))
            throw std::runtime_error(
                "Boot-Executable-Artefaktpfad ist kein sicherer regulaerer Ordner.");
    } else if (root_missing) {
        std::filesystem::create_directories(root);
    } else {
        throw std::runtime_error(
            "Boot-Executable-Artefaktpfad konnte nicht sicher geprueft werden.");
    }

    const auto executable_path =
        root / std::string(dreamcast_boot_executable_file_name);
    const auto recipe_path =
        root / std::string(dreamcast_boot_executable_recipe_name);
    const auto manifest_path =
        root / std::string(dreamcast_boot_executable_manifest_name);
    write_immutable_file(
        executable_path, std::span<const std::uint8_t>(disc.boot_file));
    write_immutable_file(
        recipe_path, runtime::format_disc_install_recipe(recipe));

    std::ostringstream manifest;
    manifest << dreamcast_boot_executable_artifact_magic << ' '
             << dreamcast_boot_executable_artifact_version << '\n'
             << "project " << project_identity << '\n'
             << "content " << recipe.content_identity << '\n'
             << "boot-name-hex "
             << hex_encode(disc.metadata.boot_file_name) << '\n'
             << "entry " << dreamcast_disc_boot_address << '\n'
             << "size " << disc.boot_file.size() << '\n'
             << "boot-sha256 " << executable_sha256 << '\n'
             << "executable " << dreamcast_boot_executable_file_name << '\n'
             << "recipe " << dreamcast_boot_executable_recipe_name << '\n';
    write_immutable_file(manifest_path, manifest.str());
    return load_dreamcast_boot_executable_artifact(manifest_path);
}

DreamcastBootExecutableArtifact
load_dreamcast_boot_executable_artifact(
    const std::filesystem::path& manifest_path) {
    if (manifest_path.empty())
        throw std::invalid_argument(
            "Boot-Executable-Artefaktmanifest fehlt.");
    require_regular_nonsymlink_file(
        manifest_path, "Boot-Executable-Artefaktmanifest");
    const auto canonical_manifest = std::filesystem::canonical(manifest_path);
    if (canonical_manifest.filename().string() !=
        dreamcast_boot_executable_manifest_name)
        throw std::runtime_error(
            "Boot-Executable-Artefaktmanifest besitzt keinen stabilen Dateinamen.");

    std::ifstream input(canonical_manifest, std::ios::binary);
    if (!input)
        throw io::InputOutputError(
            "Boot-Executable-Artefaktmanifest konnte nicht gelesen werden.");
    std::string line;
    if (!std::getline(input, line))
        throw std::runtime_error(
            "Boot-Executable-Artefaktmanifest ist leer.");
    auto values = artifact_fields(line);
    if (values.size() != 2u ||
        values[0] != dreamcast_boot_executable_artifact_magic)
        throw std::runtime_error(
            "Boot-Executable-Artefaktmanifest besitzt keinen gueltigen Header.");
    const auto version =
        parse_decimal<std::uint32_t>(values[1], "Version");
    const auto read_pair = [&](const std::string_view key) {
        if (!std::getline(input, line))
            throw std::runtime_error(
                "Boot-Executable-Artefaktmanifest ist abgeschnitten.");
        const auto pair = artifact_fields(line);
        if (pair.size() != 2u || pair[0] != key)
            throw std::runtime_error(
                "Boot-Executable-Artefaktmanifest besitzt eine falsche Feldreihenfolge.");
        return pair[1];
    };
    const auto project_identity = read_pair("project");
    const auto content_identity = read_pair("content");
    const auto boot_file_name =
        hex_decode(read_pair("boot-name-hex"));
    const auto entry_address =
        parse_decimal<std::uint32_t>(read_pair("entry"), "Einstieg");
    const auto executable_size =
        parse_decimal<std::uint64_t>(read_pair("size"), "Dateigroesse");
    const auto executable_sha256 = read_pair("boot-sha256");
    if (read_pair("executable") !=
            dreamcast_boot_executable_file_name ||
        read_pair("recipe") != dreamcast_boot_executable_recipe_name)
        throw std::runtime_error(
            "Boot-Executable-Artefaktmanifest besitzt einen unsicheren Dateivertrag.");
    if (std::getline(input, line) && !line.empty())
        throw std::runtime_error(
            "Boot-Executable-Artefaktmanifest besitzt unerwartete Zusatzdaten.");

    const auto root = canonical_manifest.parent_path();
    const auto executable_path =
        root / std::string(dreamcast_boot_executable_file_name);
    const auto recipe_path =
        root / std::string(dreamcast_boot_executable_recipe_name);
    require_regular_nonsymlink_file(
        executable_path, "Private Dreamcast-Boot-Executable");
    require_regular_nonsymlink_file(
        recipe_path, "Dreamcast-Disc-Installations-Recipe");
    auto bytes = io::read_binary_file(executable_path);
    auto recipe = runtime::parse_disc_install_recipe(recipe_path);
    if (version != dreamcast_boot_executable_artifact_version ||
        executable_size != bytes.size() ||
        !canonical_sha256(project_identity) ||
        !canonical_sha256(content_identity) ||
        !canonical_sha256(executable_sha256) ||
        executable_sha256 != boot_sha256(bytes) ||
        recipe.job_generation != project_identity ||
        recipe.content_identity != content_identity ||
        recipe.boot_sha256 != executable_sha256)
        throw std::runtime_error(
            "Boot-Executable-Artefaktmanifest stimmt nicht mit Bytes oder Recipe ueberein.");

    DreamcastBootExecutableArtifact artifact;
    artifact.version = version;
    artifact.manifest_path = canonical_manifest;
    artifact.executable_path = executable_path;
    artifact.install_recipe_path = recipe_path;
    artifact.metadata = {"SEGA SEGAKATANA", boot_file_name};
    artifact.boot_file = std::move(bytes);
    artifact.install_recipe = std::move(recipe);
    artifact.project_identity = project_identity;
    artifact.boot_sha256 = executable_sha256;
    artifact.entry_address = entry_address;
    require_valid_boot_artifact(artifact);
    return artifact;
}

io::ExecutableImage make_dreamcast_boot_executable(
    const DreamcastBootExecutableArtifact& artifact) {
    require_valid_boot_artifact(artifact);
    io::ExecutableImage image(artifact.executable_path);
    image.set_guest_call_abi(io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_address_model(io::ImageAddressModel::Sh4DirectMapped);
    io::ImageSegment boot_segment{".text",
                                  artifact.entry_address,
                                  0u,
                                  artifact.boot_file.size(),
                                  io::SegmentKind::Mixed,
                                  {true, true, true},
                                  artifact.boot_file};
    boot_segment.source_kind = io::ImageSourceKind::DiscBootFile;
    boot_segment.load_phase = io::ImageLoadPhase::Initial;
    boot_segment.local_source_name = artifact.metadata.boot_file_name;
    image.add_segment(std::move(boot_segment));
    image.add_entry_point(artifact.entry_address);
    image.set_initial_snapshot_entry(artifact.entry_address);
    return image;
}

} // namespace katana::platform
