#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

class DiscSource {
public:
    virtual ~DiscSource() = default;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual const std::string& identity() const noexcept = 0;
    virtual void read(std::uint64_t offset, std::span<std::uint8_t> destination) const = 0;
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset, std::size_t length) const;
};

class MemoryDiscSource final : public DiscSource {
public:
    using DiscSource::read;
    MemoryDiscSource(std::span<const std::uint8_t> bytes, std::string identity);
    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] const std::string& identity() const noexcept override;
    void read(std::uint64_t offset, std::span<std::uint8_t> destination) const override;
private:
    std::vector<std::uint8_t> bytes_;
    std::string identity_;
};

class FileDiscSource final : public DiscSource {
public:
    using DiscSource::read;
    FileDiscSource(std::filesystem::path path, std::string identity);
    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] const std::string& identity() const noexcept override;
    void read(std::uint64_t offset, std::span<std::uint8_t> destination) const override;
private:
    std::filesystem::path path_;
    std::string identity_;
    std::uint64_t size_ = 0u;
};

enum class GdRomCommand : std::uint8_t {
    TestUnitReady,
    GetStatus,
    GetCapacity,
    ReadSectors
};

enum class GdRomStatus : std::uint8_t {
    Good,
    NoMedia,
    InvalidCommand,
    InvalidField,
    OutOfRange
};

struct GdRomRequest {
    GdRomCommand command = GdRomCommand::TestUnitReady;
    std::uint32_t lba = 0u;
    std::uint32_t sector_count = 0u;
};

struct GdRomResponse {
    GdRomStatus status = GdRomStatus::Good;
    std::vector<std::uint8_t> data;
    std::uint32_t transferred_sectors = 0u;
};

class GdRomDrive final {
public:
    explicit GdRomDrive(std::shared_ptr<const DiscSource> source, std::uint32_t sector_size = 2048u);
    [[nodiscard]] GdRomResponse execute(const GdRomRequest& request) const;
    [[nodiscard]] std::uint32_t sector_size() const noexcept;
private:
    std::shared_ptr<const DiscSource> source_;
    std::uint32_t sector_size_ = 2048u;
};

} // namespace katana::runtime
