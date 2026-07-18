#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t persistent_image_contract_version = 1u;

enum class PersistentImageRecovery : std::uint8_t {
    CreatedFromSource,
    LoadedPrimary,
    RestoredRecovery
};

struct PersistentImageConfig {
    std::string kind;
    std::optional<std::filesystem::path> source_path;
    std::filesystem::path working_path;
    std::size_t expected_size = 0u;
    std::uint8_t erased_value = 0xFFu;
};

class PersistentImage final {
  public:
    [[nodiscard]] static std::shared_ptr<PersistentImage> open(PersistentImageConfig config);
    PersistentImage(const PersistentImage&) = delete;
    PersistentImage& operator=(const PersistentImage&) = delete;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint8_t read_byte(std::size_t offset) const;
    [[nodiscard]] std::uint8_t source_byte(std::size_t offset) const;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    void write_byte(std::size_t offset, std::uint8_t value);
    void write(std::size_t offset, std::span<const std::uint8_t> bytes);
    void save();
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] PersistentImageRecovery recovery() const noexcept;
    [[nodiscard]] std::uint64_t save_count() const noexcept;
    [[nodiscard]] std::string serialize_status_json() const;

  private:
    explicit PersistentImage(PersistentImageConfig config);
    void load();
    void publish(bool preserve_primary);
    void verify_source_unchanged() const;

    PersistentImageConfig config_;
    std::filesystem::path recovery_path_;
    std::vector<std::uint8_t> source_;
    std::vector<std::uint8_t> working_;
    std::string source_sha256_;
    PersistentImageRecovery recovery_ = PersistentImageRecovery::CreatedFromSource;
    std::uint64_t save_count_ = 0u;
    bool dirty_ = false;
};

[[nodiscard]] const char* persistent_image_recovery_name(PersistentImageRecovery value) noexcept;

} // namespace katana::runtime
