#pragma once

#include "katana/runtime/scheduler.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

enum class DiscTrackKind : std::uint8_t { Audio, Data };

struct DiscTrackLayout {
    std::uint32_t number = 0u;
    std::uint32_t lba = 0u;
    DiscTrackKind kind = DiscTrackKind::Data;
    std::uint32_t sector_size = 2048u;
    std::uint64_t sector_count = 0u;
    std::uint32_t session = 0u;
};

class DiscSource {
  public:
    virtual ~DiscSource() = default;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual const std::string& identity() const noexcept = 0;
    [[nodiscard]] virtual std::vector<DiscTrackLayout> layout() const;
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
    [[nodiscard]] std::uint64_t read_operations() const noexcept;
    [[nodiscard]] std::uint64_t bytes_read() const noexcept;
    [[nodiscard]] std::uint64_t open_operations() const noexcept;

  private:
    std::filesystem::path path_;
    std::string identity_;
    std::uint64_t size_ = 0u;
    mutable std::ifstream stream_;
    mutable std::mutex stream_mutex_;
    mutable std::atomic<std::uint64_t> read_operations_{0u};
    mutable std::atomic<std::uint64_t> bytes_read_{0u};
};

enum class GdRomCommand : std::uint8_t { TestUnitReady, GetStatus, GetCapacity, ReadSectors };

enum class GdRomStatus : std::uint8_t {
    Good,
    NoMedia,
    InvalidCommand,
    InvalidField,
    OutOfRange,
    Aborted
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
    explicit GdRomDrive(std::shared_ptr<const DiscSource> source,
                        std::uint32_t sector_size = 2048u);
    [[nodiscard]] GdRomResponse execute(const GdRomRequest& request) const;
    [[nodiscard]] std::uint32_t sector_size() const noexcept;
    [[nodiscard]] std::uint64_t sector_count() const noexcept;
    [[nodiscard]] const std::vector<DiscTrackLayout>& layout() const noexcept;
    [[nodiscard]] const std::string& identity() const noexcept;

  private:
    std::shared_ptr<const DiscSource> source_;
    std::uint32_t sector_size_ = 2048u;
    std::vector<DiscTrackLayout> layout_;
};

struct GdRomTiming {
    std::uint64_t command_latency = 1000u;
    std::uint64_t cycles_per_sector = 500u;
};

struct GdRomAsyncCompletion {
    std::uint64_t request_id = 0u;
    std::uint64_t ready_cycle = 0u;
    GdRomResponse response;
};

struct GdRomAsyncPendingSnapshot {
    std::uint64_t request_id = 0u;
    std::uint64_t ready_cycle = 0u;
    GdRomRequest request;
    SchedulerEventId event_id = 0u;
};

struct GdRomAsyncReaderSnapshot {
    std::uint64_t scheduler_cycle = 0u;
    GdRomTiming timing{};
    std::uint64_t next_request_id = 0u;
    std::vector<GdRomAsyncPendingSnapshot> pending;
    std::vector<GdRomAsyncCompletion> completed;
};

class GdRomAsyncReader final {
  public:
    explicit GdRomAsyncReader(EventScheduler& scheduler,
                              GdRomDrive drive,
                              GdRomTiming timing = {},
                              std::function<void(std::uint64_t)> completion_observer = {});
    ~GdRomAsyncReader();
    GdRomAsyncReader(const GdRomAsyncReader&) = delete;
    GdRomAsyncReader& operator=(const GdRomAsyncReader&) = delete;
    [[nodiscard]] std::uint64_t submit(const GdRomRequest& request);
    [[nodiscard]] bool cancel(std::uint64_t request_id) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::optional<GdRomAsyncCompletion> take_completed();
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::uint64_t current_cycle() const noexcept;
    [[nodiscard]] GdRomAsyncReaderSnapshot snapshot() const;

  private:
    struct Pending {
        std::uint64_t request_id = 0u;
        std::uint64_t ready_cycle = 0u;
        GdRomRequest request;
        SchedulerEventId event_id = 0u;
    };
    void complete(std::uint64_t request_id, std::uint64_t cycle);
    void handle_scheduler_reset() noexcept;
    EventScheduler& scheduler_;
    SchedulerLifetimeToken scheduler_lifetime_;
    GdRomDrive drive_;
    GdRomTiming timing_;
    std::uint64_t next_request_id_ = 1u;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::vector<Pending> pending_;
    std::vector<GdRomAsyncCompletion> completed_;
    std::function<void(std::uint64_t)> completion_observer_;
};

} // namespace katana::runtime
