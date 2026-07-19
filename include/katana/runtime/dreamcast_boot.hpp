#pragma once

#include "katana/runtime/aica.hpp"
#include "katana/runtime/bios_abi.hpp"
#include "katana/runtime/cache_control.hpp"
#include "katana/runtime/disc.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/gdi.hpp"
#include "katana/runtime/io_port.hpp"
#include "katana/runtime/maple.hpp"
#include "katana/runtime/packed_disc.hpp"
#include "katana/runtime/platform_interrupt.hpp"
#include "katana/runtime/pvr.hpp"
#include "katana/runtime/runtime.hpp"
#include "katana/runtime/store_queue.hpp"
#include "katana/runtime/system_asic.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t dreamcast_disc_boot_address = 0x8C010000u;
inline constexpr std::uint32_t dreamcast_direct_boot_stack = 0x8D000000u;
inline constexpr std::uint32_t dreamcast_disc_boot_status = sr_md_mask | sr_interrupt_mask;
inline constexpr std::uint16_t dreamcast_composite_port_a_input = 0x0300u;
enum class DreamcastRuntimeFirmwareMode : std::uint8_t { Direct, HleBiosAbi };

struct DreamcastMutableStorageConfig {
    std::string project_identity;
    std::filesystem::path storage_root;
    std::optional<std::filesystem::path> flash_source;
    std::optional<std::filesystem::path> vmu_source;
};

class DreamcastMutableStorage final {
  public:
    [[nodiscard]] static std::shared_ptr<DreamcastMutableStorage>
    open(DreamcastMutableStorageConfig config);
    [[nodiscard]] const std::shared_ptr<PersistentImage>& flash_image() const noexcept;
    [[nodiscard]] const std::shared_ptr<PersistentImage>& vmu_image() const noexcept;
    void save();
    [[nodiscard]] std::string serialize_status_json() const;

  private:
    DreamcastMutableStorage(std::shared_ptr<PersistentImage> flash,
                            std::shared_ptr<PersistentImage> vmu);
    std::shared_ptr<PersistentImage> flash_;
    std::shared_ptr<PersistentImage> vmu_;
};

[[nodiscard]] std::filesystem::path default_dreamcast_user_data_root();

struct DreamcastRuntimeBootImage {
    std::shared_ptr<DiscSource> source;
    std::string hardware_id;
    std::string boot_file_name;
    std::vector<std::uint8_t> boot_file;
    std::uint32_t data_track_lba = 0u;
    std::uint32_t extent_lba_bias = 0u;
    std::size_t validated_tracks = 0u;
    bool repeated_reads_match = false;
};

struct DreamcastRuntimeState {
    std::shared_ptr<LinearMemoryDevice> main_ram;
    std::shared_ptr<LinearMemoryDevice> vram;
    std::shared_ptr<LinearMemoryDevice> aica_ram;
    std::shared_ptr<FlashMemoryDevice> flash;
    std::shared_ptr<MapleVmuDevice> vmu;
    std::shared_ptr<DreamcastMutableStorage> mutable_storage;
    std::shared_ptr<EventScheduler> scheduler;
    std::shared_ptr<Sh4RtcClockDomain> rtc_clock;
    std::shared_ptr<Sh4Tmu> tmu;
    std::shared_ptr<Sh4Rtc> rtc;
    std::shared_ptr<Sh4Dmac> dmac;
    std::shared_ptr<InterruptController> interrupt_controller;
    std::shared_ptr<PlatformInterruptRouter> interrupt_router;
    std::shared_ptr<DreamcastSystemAsic> system_asic;
    std::shared_ptr<PvrRegisterFile> pvr_registers;
    std::shared_ptr<AicaRegisterFile> aica_registers;
    std::shared_ptr<MapleBus> maple;
    std::shared_ptr<GdRomAsyncReader> gdrom;
    std::shared_ptr<AicaExecutionController> aica;
    std::shared_ptr<RuntimeBlockTable> runtime_blocks;
    std::shared_ptr<ExecutableCodeTracker> code_tracker;
    std::shared_ptr<Sh4StoreQueues> store_queues;
    std::shared_ptr<Sh4CacheControl> cache_control;
    std::shared_ptr<Sh4IoPort> io_ports;
    std::shared_ptr<std::vector<StoreQueueTransfer>> store_queue_transfers;
    std::shared_ptr<std::uint64_t> dropped_store_queue_transfers;
    std::shared_ptr<FirmwareHandoffMap> firmware_handoff;
    std::size_t loaded_boot_bytes = 0u;
};

[[nodiscard]] DreamcastRuntimeBootImage
load_dreamcast_runtime_boot(const std::filesystem::path& descriptor_path);

[[nodiscard]] DreamcastRuntimeBootImage load_dreamcast_runtime_boot(
    std::shared_ptr<DiscSource> source, std::uint32_t data_track_lba, std::size_t validated_tracks);

[[nodiscard]] DreamcastRuntimeBootImage
load_dreamcast_runtime_boot_from_pack(const std::filesystem::path& pack_path);

[[nodiscard]] DreamcastRuntimeState initialize_dreamcast_runtime(
    CpuState& cpu,
    const DreamcastRuntimeBootImage& boot,
    DreamcastRuntimeFirmwareMode firmware_mode = DreamcastRuntimeFirmwareMode::Direct,
    std::shared_ptr<DreamcastMutableStorage> mutable_storage = {});

} // namespace katana::runtime
