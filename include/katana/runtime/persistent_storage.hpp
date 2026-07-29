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

enum class PersistenceHandoffPolicy : std::uint8_t {
    DiagnosticLossless,
    ProductPreserveTarget
};

struct PersistentImageConfig {
    std::string kind;
    std::optional<std::filesystem::path> source_path;
    std::filesystem::path working_path;
    std::size_t expected_size = 0u;
    std::uint8_t erased_value = 0xFFu;
    // Used only when neither a source nor an existing primary/recovery
    // working copy exists. The source identity remains the erased image.
    std::optional<std::vector<std::uint8_t>> source_less_initial_working_copy;
};

class PersistentImage;

class PreparedPersistentImageRestore final {
  public:
    PreparedPersistentImageRestore(const PreparedPersistentImageRestore&) = delete;
    PreparedPersistentImageRestore&
    operator=(const PreparedPersistentImageRestore&) = delete;
    PreparedPersistentImageRestore(PreparedPersistentImageRestore&&) noexcept = default;
    PreparedPersistentImageRestore&
    operator=(PreparedPersistentImageRestore&&) noexcept = default;

  private:
    friend class PersistentImage;
    PreparedPersistentImageRestore() = default;

    const PersistentImage* owner_ = nullptr;
    std::vector<std::uint8_t> working_;
    bool dirty_ = false;
    bool replace_working_copy_ = false;
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
    // Game-entry imports preserve the installed source identity and replace
    // only the in-memory working copy. No file is published by this method.
    void validate_working_copy_restore(
        std::span<const std::uint8_t> expected_source,
        std::span<const std::uint8_t> working,
        bool dirty) const;
    // Product handoffs bind the installed source but preserve the target
    // process' current working bytes and dirty bookkeeping. DiagnosticLossless
    // remains available for exact same-session capture/restore.
    [[nodiscard]] PreparedPersistentImageRestore prepare_working_copy_restore(
        std::span<const std::uint8_t> expected_source,
        std::span<const std::uint8_t> working,
        bool dirty,
        PersistenceHandoffPolicy policy) const;
    // All validation and allocation happens in prepare_working_copy_restore().
    // The owning image must remain live and unchanged until this noexcept swap.
    void commit_prepared_working_copy_restore(
        PreparedPersistentImageRestore prepared) noexcept;
    void restore_working_copy_passive(
        std::span<const std::uint8_t> expected_source,
        std::span<const std::uint8_t> working,
        bool dirty);
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
