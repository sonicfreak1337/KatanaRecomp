#include "katana/runtime/persistent_storage.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::array<std::uint8_t, 8u> container_magic{
    'K', 'A', 'T', 'W', 'C', 'P', '1', '\n'};
constexpr std::size_t sha256_text_size = 64u;
constexpr std::size_t fixed_header_size = container_magic.size() + 4u + 4u + 8u +
                                          sha256_text_size + sha256_text_size;

bool stable_kind(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 32u &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '-';
           });
}

std::string hash_bytes(const std::span<const std::uint8_t> bytes) {
    return katana::io::sha256_bytes(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void append_u32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    for (std::size_t byte = 0u; byte < 4u; ++byte)
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
}

void append_u64(std::vector<std::uint8_t>& output, const std::uint64_t value) {
    for (std::size_t byte = 0u; byte < 8u; ++byte)
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
}

std::uint32_t read_u32(const std::span<const std::uint8_t> input, const std::size_t offset) {
    if (offset > input.size() || 4u > input.size() - offset)
        throw std::runtime_error("Arbeitskopienheader ist abgeschnitten.");
    std::uint32_t value = 0u;
    for (std::size_t byte = 0u; byte < 4u; ++byte)
        value |= static_cast<std::uint32_t>(input[offset + byte]) << (byte * 8u);
    return value;
}

std::uint64_t read_u64(const std::span<const std::uint8_t> input, const std::size_t offset) {
    if (offset > input.size() || 8u > input.size() - offset)
        throw std::runtime_error("Arbeitskopienheader ist abgeschnitten.");
    std::uint64_t value = 0u;
    for (std::size_t byte = 0u; byte < 8u; ++byte)
        value |= static_cast<std::uint64_t>(input[offset + byte]) << (byte * 8u);
    return value;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path,
                                    const std::size_t expected_maximum) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Arbeitskopie konnte nicht geoeffnet werden.");
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > expected_maximum)
        throw std::runtime_error("Arbeitskopie besitzt eine ungueltige Groesse.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("Arbeitskopie konnte nicht gelesen werden.");
    return bytes;
}

std::vector<std::uint8_t> encode_container(const PersistentImageConfig& config,
                                           const std::string_view source_sha256,
                                           const std::span<const std::uint8_t> payload) {
    const auto payload_sha256 = hash_bytes(payload);
    std::vector<std::uint8_t> output;
    output.reserve(fixed_header_size + config.kind.size() + payload.size());
    output.insert(output.end(), container_magic.begin(), container_magic.end());
    append_u32(output, persistent_image_contract_version);
    append_u32(output, static_cast<std::uint32_t>(config.kind.size()));
    append_u64(output, payload.size());
    output.insert(output.end(), source_sha256.begin(), source_sha256.end());
    output.insert(output.end(), payload_sha256.begin(), payload_sha256.end());
    output.insert(output.end(), config.kind.begin(), config.kind.end());
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

std::vector<std::uint8_t> decode_container(const PersistentImageConfig& config,
                                           const std::string_view source_sha256,
                                           const std::filesystem::path& path) {
    const auto maximum = fixed_header_size + config.kind.size() + config.expected_size;
    const auto bytes = read_file(path, maximum);
    if (bytes.size() < fixed_header_size ||
        !std::equal(container_magic.begin(), container_magic.end(), bytes.begin()))
        throw std::runtime_error("Arbeitskopie besitzt keine gueltige Signatur.");
    const auto version = read_u32(bytes, container_magic.size());
    const auto kind_size = read_u32(bytes, container_magic.size() + 4u);
    const auto payload_size = read_u64(bytes, container_magic.size() + 8u);
    if (version != persistent_image_contract_version || kind_size != config.kind.size() ||
        payload_size != config.expected_size ||
        payload_size > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Arbeitskopienvertrag ist inkompatibel.");
    const auto source_hash_offset = container_magic.size() + 4u + 4u + 8u;
    const auto payload_hash_offset = source_hash_offset + sha256_text_size;
    const auto kind_offset = payload_hash_offset + sha256_text_size;
    const auto payload_offset = kind_offset + kind_size;
    if (payload_offset > bytes.size() || payload_size != bytes.size() - payload_offset)
        throw std::runtime_error("Arbeitskopiennutzdaten sind abgeschnitten.");
    const std::string_view stored_source(
        reinterpret_cast<const char*>(bytes.data() + source_hash_offset), sha256_text_size);
    const std::string_view stored_payload(
        reinterpret_cast<const char*>(bytes.data() + payload_hash_offset), sha256_text_size);
    const std::string_view stored_kind(
        reinterpret_cast<const char*>(bytes.data() + kind_offset), kind_size);
    const auto payload = std::span<const std::uint8_t>(bytes).subspan(payload_offset);
    if (stored_source != source_sha256 || stored_kind != config.kind ||
        stored_payload != hash_bytes(payload))
        throw std::runtime_error("Arbeitskopienidentitaet oder Nutzdatenpruefung ist ungueltig.");
    return std::vector<std::uint8_t>(payload.begin(), payload.end());
}

void durable_write(const std::filesystem::path& path,
                   const std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    auto* file = ::_wfopen(path.c_str(), L"wb");
#else
    auto* file = std::fopen(path.c_str(), "wb");
#endif
    if (file == nullptr) throw std::runtime_error("Temporaere Arbeitskopie konnte nicht angelegt werden.");
    bool success = false;
    try {
        const auto written = bytes.empty() ? 0u : std::fwrite(bytes.data(), 1u, bytes.size(), file);
        if (written != bytes.size() || std::fflush(file) != 0)
            throw std::runtime_error("Temporaere Arbeitskopie konnte nicht geschrieben werden.");
#ifdef _WIN32
        if (::_commit(::_fileno(file)) != 0)
#else
        if (::fsync(::fileno(file)) != 0)
#endif
            throw std::runtime_error("Temporaere Arbeitskopie konnte nicht synchronisiert werden.");
        success = true;
    } catch (...) {
        static_cast<void>(std::fclose(file));
        throw;
    }
    if (std::fclose(file) != 0 || !success)
        throw std::runtime_error("Temporaere Arbeitskopie konnte nicht geschlossen werden.");
}

std::filesystem::path temporary_path(const std::filesystem::path& working) {
    for (std::size_t attempt = 1u; attempt <= 1024u; ++attempt) {
        auto candidate = working;
        candidate += ".tmp-" + std::to_string(attempt);
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    throw std::runtime_error("Kein freier temporaerer Arbeitskopienpfad verfuegbar.");
}

void reject_symlink(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (!error && std::filesystem::is_symlink(status))
        throw std::runtime_error("Arbeitskopie darf kein symbolischer Link sein.");
    if (error && error != std::errc::no_such_file_or_directory)
        throw std::runtime_error("Arbeitskopienpfad konnte nicht geprueft werden.");
}

std::filesystem::path normalized_existing_parent(const std::filesystem::path& path) {
    auto existing = std::filesystem::absolute(path).lexically_normal();
    std::vector<std::filesystem::path> suffix;
    while (!std::filesystem::exists(existing)) {
        if (existing.filename().empty())
            throw std::runtime_error("Arbeitskopienpfad besitzt keinen vorhandenen Elternpfad.");
        suffix.push_back(existing.filename());
        existing = existing.parent_path();
    }
    existing = std::filesystem::canonical(existing);
    for (auto iterator = suffix.rbegin(); iterator != suffix.rend(); ++iterator)
        existing /= *iterator;
    return existing.lexically_normal();
}

} // namespace

std::shared_ptr<PersistentImage> PersistentImage::open(PersistentImageConfig config) {
    auto image = std::shared_ptr<PersistentImage>(new PersistentImage(std::move(config)));
    image->load();
    return image;
}

PersistentImage::PersistentImage(PersistentImageConfig config) : config_(std::move(config)) {
    if (!stable_kind(config_.kind) || config_.working_path.empty() ||
        config_.expected_size == 0u)
        throw std::invalid_argument("Persistente Arbeitskopienkonfiguration ist unvollstaendig.");
    config_.working_path = std::filesystem::absolute(config_.working_path).lexically_normal();
    recovery_path_ = config_.working_path;
    recovery_path_ += ".recovery";
    reject_symlink(config_.working_path);
    reject_symlink(recovery_path_);
    if (config_.source_path) {
        *config_.source_path = std::filesystem::absolute(*config_.source_path).lexically_normal();
        if (normalized_existing_parent(*config_.source_path) ==
                normalized_existing_parent(config_.working_path) ||
            normalized_existing_parent(*config_.source_path) ==
                normalized_existing_parent(recovery_path_))
            throw std::invalid_argument("Quelle und Arbeitskopie muessen getrennte Dateien sein.");
    }
}

void PersistentImage::load() {
    source_.assign(config_.expected_size, config_.erased_value);
    if (config_.source_path) {
        if (!std::filesystem::is_regular_file(*config_.source_path))
            throw std::runtime_error("Arbeitskopienquelle fehlt oder ist keine Datei.");
        source_ = read_file(*config_.source_path, config_.expected_size);
        if (source_.size() != config_.expected_size)
            throw std::runtime_error("Arbeitskopienquelle besitzt eine ungueltige Groesse.");
    }
    source_sha256_ = hash_bytes(source_);
    if (std::filesystem::exists(config_.working_path)) {
        try {
            working_ = decode_container(config_, source_sha256_, config_.working_path);
            recovery_ = PersistentImageRecovery::LoadedPrimary;
            return;
        } catch (...) {
            if (!std::filesystem::exists(recovery_path_)) throw;
        }
    }
    if (std::filesystem::exists(recovery_path_)) {
        working_ = decode_container(config_, source_sha256_, recovery_path_);
        recovery_ = PersistentImageRecovery::RestoredRecovery;
        publish(false);
        return;
    }
    working_ = source_;
    recovery_ = PersistentImageRecovery::CreatedFromSource;
    publish(false);
}

std::size_t PersistentImage::size() const noexcept { return working_.size(); }
std::uint8_t PersistentImage::read_byte(const std::size_t offset) const {
    return working_.at(offset);
}
std::uint8_t PersistentImage::source_byte(const std::size_t offset) const {
    return source_.at(offset);
}
std::span<const std::uint8_t> PersistentImage::bytes() const noexcept { return working_; }

void PersistentImage::write_byte(const std::size_t offset, const std::uint8_t value) {
    if (offset >= working_.size()) throw std::out_of_range("Arbeitskopienwrite ausserhalb des Abbilds.");
    if (working_[offset] == value) return;
    working_[offset] = value;
    dirty_ = true;
}

void PersistentImage::write(const std::size_t offset,
                            const std::span<const std::uint8_t> bytes) {
    if (offset > working_.size() || bytes.size() > working_.size() - offset)
        throw std::out_of_range("Arbeitskopienwrite ausserhalb des Abbilds.");
    if (std::equal(bytes.begin(), bytes.end(), working_.begin() + static_cast<std::ptrdiff_t>(offset)))
        return;
    std::copy(bytes.begin(), bytes.end(), working_.begin() + static_cast<std::ptrdiff_t>(offset));
    dirty_ = true;
}

void PersistentImage::verify_source_unchanged() const {
    if (!config_.source_path) return;
    const auto current = read_file(*config_.source_path, config_.expected_size);
    if (current.size() != config_.expected_size || hash_bytes(current) != source_sha256_)
        throw std::runtime_error("Arbeitskopienquelle wurde waehrend der Sitzung veraendert.");
}

void PersistentImage::publish(const bool preserve_primary) {
    std::filesystem::create_directories(config_.working_path.parent_path());
    const auto temporary = temporary_path(config_.working_path);
    const auto container = encode_container(config_, source_sha256_, working_);
    try {
        durable_write(temporary, container);
        bool moved_primary = false;
        if (preserve_primary && std::filesystem::exists(config_.working_path)) {
            std::error_code remove_error;
            std::filesystem::remove(recovery_path_, remove_error);
            if (remove_error)
                throw std::runtime_error("Alte Recovery-Arbeitskopie konnte nicht entfernt werden.");
            std::filesystem::rename(config_.working_path, recovery_path_);
            moved_primary = true;
        } else if (std::filesystem::exists(config_.working_path)) {
            std::filesystem::remove(config_.working_path);
        }
        try {
            std::filesystem::rename(temporary, config_.working_path);
        } catch (...) {
            if (moved_primary && !std::filesystem::exists(config_.working_path)) {
                std::error_code restore_error;
                std::filesystem::rename(recovery_path_, config_.working_path, restore_error);
            }
            throw;
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void PersistentImage::save() {
    if (!dirty_) return;
    verify_source_unchanged();
    publish(true);
    dirty_ = false;
    if (save_count_ != std::numeric_limits<std::uint64_t>::max()) ++save_count_;
}

bool PersistentImage::dirty() const noexcept { return dirty_; }
PersistentImageRecovery PersistentImage::recovery() const noexcept { return recovery_; }
std::uint64_t PersistentImage::save_count() const noexcept { return save_count_; }

std::string PersistentImage::serialize_status_json() const {
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-persistent-image-v1", "persistent-image");
    output << ",\"contract_version\":" << persistent_image_contract_version
           << ",\"kind\":" << katana::io::quote_json(config_.kind)
           << ",\"size\":" << working_.size() << ",\"dirty\":" << (dirty_ ? "true" : "false")
           << ",\"recovery\":" << katana::io::quote_json(persistent_image_recovery_name(recovery_))
           << ",\"save_count\":" << save_count_ << '}';
    return output.str();
}

const char* persistent_image_recovery_name(const PersistentImageRecovery value) noexcept {
    switch (value) {
    case PersistentImageRecovery::CreatedFromSource:
        return "created-from-source";
    case PersistentImageRecovery::LoadedPrimary:
        return "loaded-primary";
    case PersistentImageRecovery::RestoredRecovery:
        return "restored-recovery";
    }
    return "created-from-source";
}

} // namespace katana::runtime
