#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/observation_restore_policy.hpp"
#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace katana::runtime {

class DreamcastSystemBusControl;

inline constexpr std::uint32_t pvr_register_physical_base = 0x005F8000u;
// The normal register bank is followed by the 128-entry fog table at 0x200 and
// the 1024-entry palette RAM at 0x1000. They are part of the same PVR MMIO
// aperture and must share the register-file state used by the renderer.
inline constexpr std::size_t pvr_register_size = 0x2000u;
inline constexpr std::uint32_t pvr_id = 0x17FD11DBu;
inline constexpr std::uint32_t pvr_revision = 0x00000011u;

namespace pvr_register {
inline constexpr std::uint32_t Id = 0x000u;
inline constexpr std::uint32_t Revision = 0x004u;
inline constexpr std::uint32_t SoftReset = 0x008u;
inline constexpr std::uint32_t StartRender = 0x014u;
inline constexpr std::uint32_t ParameterBase = 0x020u;
inline constexpr std::uint32_t RegionBase = 0x02Cu;
inline constexpr std::uint32_t BorderColor = 0x040u;
inline constexpr std::uint32_t FramebufferReadControl = 0x044u;
inline constexpr std::uint32_t FramebufferWriteControl = 0x048u;
inline constexpr std::uint32_t FramebufferRenderModulo = 0x04Cu;
inline constexpr std::uint32_t FramebufferReadSof1 = 0x050u;
inline constexpr std::uint32_t FramebufferReadSof2 = 0x054u;
inline constexpr std::uint32_t FramebufferReadSize = 0x05Cu;
inline constexpr std::uint32_t FramebufferWriteSof1 = 0x060u;
inline constexpr std::uint32_t FramebufferWriteSof2 = 0x064u;
inline constexpr std::uint32_t FramebufferXClip = 0x068u;
inline constexpr std::uint32_t FramebufferYClip = 0x06Cu;
inline constexpr std::uint32_t ShadingScale = 0x074u;
inline constexpr std::uint32_t CullingValue = 0x078u;
inline constexpr std::uint32_t ParameterConfig = 0x07Cu;
inline constexpr std::uint32_t HalfOffset = 0x080u;
inline constexpr std::uint32_t PerpendicularValue = 0x084u;
inline constexpr std::uint32_t BackgroundPlaneDepth = 0x088u;
inline constexpr std::uint32_t BackgroundPlaneConfig = 0x08Cu;
inline constexpr std::uint32_t IspFeedConfig = 0x098u;
inline constexpr std::uint32_t SdramRefresh = 0x0A0u;
inline constexpr std::uint32_t SdramArbitration = 0x0A4u;
inline constexpr std::uint32_t SdramConfig = 0x0A8u;
inline constexpr std::uint32_t FogTableColor = 0x0B0u;
inline constexpr std::uint32_t FogVertexColor = 0x0B4u;
inline constexpr std::uint32_t FogDensity = 0x0B8u;
inline constexpr std::uint32_t ColorClampMaximum = 0x0BCu;
inline constexpr std::uint32_t ColorClampMinimum = 0x0C0u;
inline constexpr std::uint32_t SpgTriggerPosition = 0x0C4u;
inline constexpr std::uint32_t SpgHblankInterrupt = 0x0C8u;
inline constexpr std::uint32_t SpgVblankInterrupt = 0x0CCu;
inline constexpr std::uint32_t SpgControl = 0x0D0u;
inline constexpr std::uint32_t SpgHblank = 0x0D4u;
inline constexpr std::uint32_t SpgLoad = 0x0D8u;
inline constexpr std::uint32_t SpgVblank = 0x0DCu;
inline constexpr std::uint32_t SpgWidth = 0x0E0u;
inline constexpr std::uint32_t VideoControl = 0x0E8u;
inline constexpr std::uint32_t VideoStartX = 0x0ECu;
inline constexpr std::uint32_t VideoStartY = 0x0F0u;
inline constexpr std::uint32_t ScalerControl = 0x0F4u;
inline constexpr std::uint32_t TextureModulo = 0x0E4u;
inline constexpr std::uint32_t PaletteConfig = 0x108u;
inline constexpr std::uint32_t SpgStatus = 0x10Cu;
inline constexpr std::uint32_t FramebufferBurstControl = 0x110u;
inline constexpr std::uint32_t FramebufferCurrentReadStart = 0x114u;
inline constexpr std::uint32_t YCoefficient = 0x118u;
inline constexpr std::uint32_t PunchThroughAlphaReference = 0x11Cu;
inline constexpr std::uint32_t TaOpbStart = 0x124u;
inline constexpr std::uint32_t TaVertexBufferStart = 0x128u;
inline constexpr std::uint32_t TaOpbEnd = 0x12Cu;
inline constexpr std::uint32_t TaVertexBufferEnd = 0x130u;
inline constexpr std::uint32_t TaOpbPosition = 0x134u;
inline constexpr std::uint32_t TaVertexBufferPosition = 0x138u;
inline constexpr std::uint32_t TaGlobalTileClip = 0x13Cu;
inline constexpr std::uint32_t TaAllocationControl = 0x140u;
inline constexpr std::uint32_t TaInit = 0x144u;
inline constexpr std::uint32_t YuvAddress = 0x148u;
inline constexpr std::uint32_t YuvConfig = 0x14Cu;
inline constexpr std::uint32_t YuvStatus = 0x150u;
inline constexpr std::uint32_t TaListContinue = 0x160u;
inline constexpr std::uint32_t TaNextOpbInit = 0x164u;
inline constexpr std::uint32_t TaObjectListBase = TaOpbStart;
inline constexpr std::uint32_t TaIspBase = TaVertexBufferStart;
inline constexpr std::uint32_t TaObjectListLimit = TaOpbEnd;
inline constexpr std::uint32_t TaIspLimit = TaVertexBufferEnd;
inline constexpr std::uint32_t TaNextOpb = TaOpbPosition;
inline constexpr std::uint32_t TaIspCurrent = TaVertexBufferPosition;
inline constexpr std::uint32_t FogTableBase = 0x200u;
inline constexpr std::uint32_t PaletteTableBase = 0x1000u;
} // namespace pvr_register

enum class DreamcastVideoMode : std::uint8_t {
    NtscNonInterlaced,
    NtscInterlaced,
    PalNonInterlaced,
    PalInterlaced,
    Vga,
};

struct PvrTiming {
    std::uint64_t render_latency = 2'000u;
    std::uint64_t guest_clock_hz = 200'000'000u;
    std::uint64_t pixel_clock_hz = 27'000'000u;
};

inline constexpr std::uint32_t dreamcast_pvr_vblank_in_event_channel = 1u;
inline constexpr std::uint32_t dreamcast_pvr_vblank_out_event_channel = 2u;
inline constexpr std::uint32_t dreamcast_pvr_hblank_event_channel = 3u;
inline constexpr std::uint64_t dreamcast_pvr_scan_event_token_v1 = 1u;

enum class PvrRenderResult : std::uint8_t { Success, Failed };
enum class PvrRenderError : std::uint8_t {
    InvalidTaState,
    InvalidConfiguration,
    MemoryRange,
    UnsupportedFeature,
    InternalLifecycle
};
enum class PvrRenderStartError : std::uint8_t {
    Busy,
    CaptureFailed,
    SchedulerFailure,
    GenerationExhausted,
};

class PvrRenderJobError final : public std::runtime_error {
  public:
    PvrRenderJobError(PvrRenderError error,
                      std::string ta_packet_class,
                      std::string detail);
    [[nodiscard]] PvrRenderError error() const noexcept;
    [[nodiscard]] const std::string& ta_packet_class() const noexcept;

  private:
    PvrRenderError error_;
    std::string ta_packet_class_;
};

struct PvrRenderFailure {
    std::uint64_t request = 0u;
    std::uint64_t generation = 0u;
    PvrRenderError error = PvrRenderError::InternalLifecycle;
    std::string ta_packet_class;
    std::uint64_t register_digest = 0u;
    std::uint64_t guest_cycle = 0u;
    std::string detail;

    [[nodiscard]] bool operator==(const PvrRenderFailure&) const = default;
};

class PvrRenderFailed final : public std::runtime_error {
  public:
    explicit PvrRenderFailed(PvrRenderFailure failure);
    [[nodiscard]] const PvrRenderFailure& failure() const noexcept;

  private:
    PvrRenderFailure failure_;
};

struct PvrRegisterSnapshot {
    // Exact guest-visible register aperture. Dynamic read-only registers are materialized at the
    // snapshot cycle, while ordinary, fog-table and palette entries retain their stored values.
    std::array<std::uint32_t, pvr_register_size / 4u> registers{};
    std::uint32_t framebuffer_read_control = 0u;
    std::uint32_t framebuffer_read_size = 0u;
    std::uint32_t framebuffer_read_sof1 = 0u;
    std::uint32_t framebuffer_read_sof2 = 0u;
    std::uint32_t framebuffer_write_control = 0u;
    std::uint32_t framebuffer_write_sof1 = 0u;
    std::uint32_t framebuffer_write_sof2 = 0u;
    std::uint32_t video_control = 0u;
    std::uint64_t render_requests = 0u;
    std::uint64_t render_completions = 0u;
    std::uint64_t render_failures = 0u;
    std::uint64_t render_overruns = 0u;
    std::uint64_t vblank_in = 0u;
    std::uint64_t vblank_out = 0u;
    std::uint64_t hblank = 0u;
    std::uint64_t resets = 0u;
    std::vector<SchedulerEventId> render_event_ids;
    std::optional<SchedulerEventId> vblank_in_event;
    std::optional<SchedulerEventId> vblank_out_event;
    std::optional<SchedulerEventId> hblank_event;
    // SchedulerEventIds are process-local and are never imported. Portable
    // handoffs use these flags plus the typed scheduler-event contract.
    bool vblank_in_event_rehydration_pending = false;
    bool vblank_out_event_rehydration_pending = false;
    bool hblank_event_rehydration_pending = false;
    std::optional<std::uint32_t> hblank_event_line;
    std::uint64_t scan_frame_cycles = 0u;
    std::uint64_t scan_epoch_cycle = 0u;
    PvrTiming timing{};
    bool in_vblank = false;
    std::uint32_t field = 0u;
    // Zero is the terminal exhausted state; generations never wrap or repeat.
    std::uint64_t next_render_generation = 1u;
    std::uint64_t active_render_request = 0u;
    std::uint64_t active_render_generation = 0u;
    std::uint64_t active_render_start_cycle = 0u;
    std::uint64_t active_render_payload_digest = 0u;
    std::optional<PvrRenderStartError> last_render_start_error;
    std::optional<PvrRenderFailure> last_render_failure;

    [[nodiscard]] std::uint32_t read(const std::uint32_t offset) const {
        if ((offset & 3u) != 0u || offset >= pvr_register_size)
            throw std::out_of_range("PVR-Snapshotregister liegt ausserhalb des Aperturvertrags.");
        return registers[offset / 4u];
    }
};

class PvrRegisterFile final {
  public:
    using RenderJob = std::function<PvrRenderResult()>;
    using RenderJobCommit = std::function<void()>;
    struct PreparedRenderJob {
        RenderJob execute;
        // Published exactly once after the scheduler event and frozen job are registered.
        // Product factories stage mutable capture state until this non-throwing commit.
        RenderJobCommit commit;
        // Deterministic TA-frame/register payload digest. Request, generation and start cycle
        // remain explicit FrozenRenderJob/Snapshot fields rather than being folded into it.
        std::uint64_t payload_digest = 0u;
    };
    using RenderJobFactory =
        std::function<PreparedRenderJob(const PvrRegisterSnapshot&,
                                        std::uint64_t,
                                        std::uint64_t,
                                        std::uint64_t)>;
    explicit PvrRegisterFile(EventScheduler& scheduler,
                             PvrTiming timing = {},
                             std::function<void()> render_observer = {},
                             std::function<void(bool)> vblank_observer = {});
    ~PvrRegisterFile();
    PvrRegisterFile(const PvrRegisterFile&) = delete;
    PvrRegisterFile& operator=(const PvrRegisterFile&) = delete;
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void reset();
    [[nodiscard]] std::uint64_t render_request_count() const noexcept;
    [[nodiscard]] std::uint64_t render_completion_count() const noexcept;
    [[nodiscard]] std::uint64_t render_failure_count() const noexcept;
    [[nodiscard]] const std::optional<PvrRenderFailure>&
    last_render_failure() const noexcept;
    [[nodiscard]] std::uint64_t render_overrun_count() const noexcept;
    [[nodiscard]] std::uint64_t reset_count() const noexcept;
    [[nodiscard]] std::uint64_t vblank_in_count() const noexcept;
    [[nodiscard]] std::uint64_t vblank_out_count() const noexcept;
    [[nodiscard]] std::uint64_t hblank_count() const noexcept;
    [[nodiscard]] bool in_vblank() const noexcept;
    [[nodiscard]] std::uint32_t field() const noexcept;
    [[nodiscard]] PvrRegisterSnapshot snapshot() const;
    void validate_state_restore(const PvrRegisterSnapshot& state) const;
    // Passive restore never imports scheduler IDs, invokes observers, starts a
    // render job or publishes an interrupt. Scan events remain inert until
    // their typed scheduler records are rehydrated.
    void restore_state_passive(PvrRegisterSnapshot state);
    void commit_validated_state_restore(
        PvrRegisterSnapshot state) noexcept;
    [[nodiscard]] SchedulerEventId rehydrate_scheduled_event(
        std::uint64_t guest_cycle,
        std::uint32_t channel,
        std::uint64_t token);
    [[nodiscard]] SchedulerCallback
    make_rehydrated_scheduled_event_callback(
        std::uint32_t channel,
        std::uint64_t token,
        std::optional<std::uint32_t> hblank_line);
    void commit_rehydrated_scheduled_event(
        SchedulerEventId event_id,
        std::uint32_t channel,
        std::uint64_t token) noexcept;
    [[nodiscard]] bool event_rehydration_pending(
        std::uint32_t channel) const noexcept;
    void set_render_observer(std::function<void()> observer);
    void set_render_result_observer(std::function<PvrRenderResult()> observer);
    void set_render_job_factory(RenderJobFactory factory);
    void set_render_overrun_observer(std::function<void()> observer);
    void set_vblank_observer(std::function<void(bool)> observer);
    void set_hblank_observer(std::function<void()> observer);
    void set_ta_reset_observer(std::function<void()> observer);
    void set_ta_continue_observer(std::function<void()> observer);
    void record_ta_packet(std::uint32_t bytes);

  private:
    struct FrozenRenderJob {
        std::uint64_t request = 0u;
        std::uint64_t generation = 0u;
        std::uint64_t start_cycle = 0u;
        std::uint64_t payload_digest = 0u;
        RenderJob execute;
        bool published = false;
    };

    [[nodiscard]] static std::size_t index(std::uint32_t offset);
    void complete_render(SchedulerEventId event_id);
    void initialize_register_defaults() noexcept;
    void reschedule_scanout(bool derive_current_vblank = true);
    void schedule_scan_event(std::uint32_t line, bool entering);
    void handle_scan_event(SchedulerEventId event_id, bool entering);
    void schedule_hblank_event(std::uint32_t line);
    void handle_hblank_event(SchedulerEventId event_id, std::uint32_t line);
    void cancel_scan_events() noexcept;
    void handle_scheduler_reset();
    EventScheduler& scheduler_;
    PvrTiming timing_;
    std::array<std::uint32_t, pvr_register_size / 4u> registers_{};
    std::uint64_t render_requests_ = 0u;
    std::uint64_t render_completions_ = 0u;
    std::uint64_t render_failures_ = 0u;
    std::uint64_t render_overruns_ = 0u;
    std::uint64_t next_render_generation_ = 1u;
    std::optional<PvrRenderStartError> last_render_start_error_;
    std::optional<PvrRenderFailure> last_render_failure_;
    std::uint64_t resets_ = 0u;
    SchedulerResetObserverId reset_observer_ = 0u;
    SchedulerLifetimeToken scheduler_lifetime_;
    std::map<SchedulerEventId, FrozenRenderJob> render_jobs_;
    std::function<PvrRenderResult()> render_result_observer_;
    RenderJobFactory render_job_factory_;
    std::function<void()> render_overrun_observer_;
    std::function<void(bool)> vblank_observer_;
    std::function<void()> hblank_observer_;
    std::function<void()> ta_reset_observer_;
    std::function<void()> ta_continue_observer_;
    std::optional<SchedulerEventId> vblank_in_event_;
    std::optional<SchedulerEventId> vblank_out_event_;
    std::optional<SchedulerEventId> hblank_event_;
    bool vblank_in_event_rehydration_pending_ = false;
    bool vblank_out_event_rehydration_pending_ = false;
    bool hblank_event_rehydration_pending_ = false;
    std::optional<std::uint32_t> hblank_event_line_;
    std::uint64_t vblank_in_count_ = 0u;
    std::uint64_t vblank_out_count_ = 0u;
    std::uint64_t hblank_count_ = 0u;
    std::uint64_t scan_frame_cycles_ = 0u;
    std::uint64_t scan_epoch_cycle_ = 0u;
    bool in_vblank_ = false;
    std::uint32_t field_ = 0u;
};

void configure_dreamcast_video(PvrRegisterFile& registers, DreamcastVideoMode mode);

enum class PvrFramebufferFormat : std::uint8_t { Rgb565, Rgb0555, Rgb888, Rgb0888 };

struct PvrScanoutDescriptor {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t source_width = 0u;
    std::uint32_t source_height = 0u;
    std::uint32_t stride_bytes = 0u;
    std::size_t base_offset = 0u;
    std::size_t second_base_offset = 0u;
    PvrFramebufferFormat format = PvrFramebufferFormat::Rgb565;
    std::uint8_t concat = 0u;
    bool line_double = false;
    bool interlaced = false;
    bool weave_fields = false;
    bool horizontal_scale = false;
    std::uint16_t vertical_scale_factor = 0x0400u;
    bool video_blank = false;
    std::array<std::uint8_t, 4u> border_rgba{0u, 0u, 0u, 0xFFu};
};

[[nodiscard]] std::optional<PvrScanoutDescriptor>
decode_pvr_scanout(const PvrRegisterFile& registers, std::size_t vram_size);

struct PvrFrame {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::vector<std::uint8_t> rgba;
};

// Captures the scanout represented by the current register/VRAM state without
// mutating renderer evidence queues. This is used for exact product-boundary
// baselines rather than reusing an older host-presented frame.
[[nodiscard]] std::optional<PvrFrame>
capture_pvr_scanout_frame(const PvrRegisterFile& registers,
                          std::span<const std::uint8_t> vram);

class PvrFramebuffer final {
  public:
    void configure(std::uint32_t width,
                   std::uint32_t height,
                   std::uint32_t stride_bytes,
                   PvrFramebufferFormat format,
                   bool line_double = false,
                   bool interlaced = false,
                   std::uint32_t source_width = 0u,
                   std::uint32_t source_height = 0u,
                   std::uint8_t concat = 0u,
                   bool logical_32bit_vram = false);
    [[nodiscard]] PvrFrame capture(std::span<const std::uint8_t> vram,
                                   std::size_t base_offset = 0u,
                                   std::optional<std::size_t> second_base_offset = std::nullopt,
                                   std::optional<std::array<std::uint8_t, 4u>> solid_color =
                                       std::nullopt);
    [[nodiscard]] std::uint64_t presented_frames() const noexcept;
    [[nodiscard]] std::size_t capture_job_capacity() const noexcept;
    [[nodiscard]] std::size_t last_capture_job_count() const noexcept;

  private:
    std::uint32_t width_ = 0u;
    std::uint32_t height_ = 0u;
    std::uint32_t source_width_ = 0u;
    std::uint32_t source_height_ = 0u;
    std::uint32_t stride_ = 0u;
    PvrFramebufferFormat format_ = PvrFramebufferFormat::Rgb565;
    std::uint8_t concat_ = 0u;
    bool line_double_ = false;
    bool interlaced_ = false;
    bool logical_32bit_vram_ = false;
    std::uint64_t presented_frames_ = 0u;
    std::size_t last_capture_jobs_ = 1u;
};

enum class PvrListType : std::uint8_t {
    Opaque = 0u,
    OpaqueModifier = 1u,
    Translucent = 2u,
    TranslucentModifier = 3u,
    PunchThrough = 4u
};

struct PvrVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t argb = 0xFFFFFFFFu;
    std::uint32_t oargb = 0u;
    float volume_u = 0.0f;
    float volume_v = 0.0f;
    std::uint32_t volume_argb = 0xFFFFFFFFu;
    std::uint32_t volume_oargb = 0u;
};

struct PvrMaterial {
    bool gouraud = true;
    bool textured = false;
    bool texture_twiddled = true;
    bool texture_vq = false;
    bool texture_mipmapped = false;
    bool texture_x32_stride = false;
    bool texture_alpha_disabled = false;
    bool vertex_alpha_enabled = false;
    bool offset_color_enabled = false;
    bool color_clamp_enabled = false;
    bool texture_supersampling = false;
    bool shadow_enabled = false;
    bool blend_destination_accumulation = false;
    bool blend_source_accumulation = false;
    bool clamp_u = false;
    bool clamp_v = false;
    bool flip_u = false;
    bool flip_v = false;
    bool depth_write = true;
    std::uint8_t depth_compare = 7u;
    std::uint8_t culling = 0u;
    std::uint8_t texture_format = 0u;
    std::uint8_t texture_shading = 1u;
    std::uint8_t texture_filter = 0u;
    std::uint8_t texture_mipmap_bias = 0u;
    std::uint8_t fog_mode = 2u;
    std::uint8_t source_blend = 1u;
    std::uint8_t destination_blend = 0u;
    std::uint8_t palette_bank = 0u;
    std::uint8_t user_clip_mode = 0u;
    std::uint16_t user_clip_start_x = 0u;
    std::uint16_t user_clip_start_y = 0u;
    std::uint16_t user_clip_end_x = 0u;
    std::uint16_t user_clip_end_y = 0u;
    std::uint32_t texture_width = 0u;
    std::uint32_t texture_height = 0u;
    std::uint32_t texture_base = 0u;
    std::uint32_t texture_stride_width = 0u;
    std::shared_ptr<PvrMaterial> volume_material;
};

struct PvrPrimitive {
    PvrListType list = PvrListType::Opaque;
    std::vector<PvrVertex> vertices;
    PvrMaterial material;
};

struct PvrModifierVolume {
    PvrListType list = PvrListType::OpaqueModifier;
    std::vector<std::array<PvrVertex, 3u>> triangles;
    std::uint8_t depth_mode = 0u;
    std::uint8_t culling = 0u;
    std::uint8_t user_clip_mode = 0u;
    std::uint16_t user_clip_start_x = 0u;
    std::uint16_t user_clip_start_y = 0u;
    std::uint16_t user_clip_end_x = 0u;
    std::uint16_t user_clip_end_y = 0u;
    bool volume_last = false;
};

struct PvrTaFrame {
    std::vector<PvrPrimitive> primitives;
    std::vector<PvrModifierVolume> modifier_volumes;
};

struct TileAcceleratorSnapshot {
    std::vector<PvrPrimitive> primitives;
    std::vector<PvrVertex> current_strip;
    PvrListType current_list = PvrListType::Opaque;
    PvrMaterial current_material;
    std::uint8_t highest_list_rank = 0u;
    bool frame_has_list = false;
    bool list_open = false;
};

class TileAccelerator final {
  public:
    void begin_list(PvrListType type);
    void set_material(PvrMaterial material);
    void submit_vertex(const PvrVertex& vertex, bool end_of_strip);
    void end_list();
    [[nodiscard]] PvrTaFrame finish_frame();
    [[nodiscard]] bool list_open() const noexcept;
    [[nodiscard]] TileAcceleratorSnapshot snapshot() const;
    void validate_state_restore(const TileAcceleratorSnapshot& state) const;
    void restore_state_passive(TileAcceleratorSnapshot state);
    void commit_validated_state_restore(
        TileAcceleratorSnapshot state) noexcept;

  private:
    [[nodiscard]] static std::uint8_t list_rank(PvrListType type) noexcept;
    std::vector<PvrPrimitive> primitives_;
    std::vector<PvrVertex> current_strip_;
    PvrListType current_list_ = PvrListType::Opaque;
    PvrMaterial current_material_;
    std::uint8_t highest_list_rank_ = 0u;
    bool frame_has_list_ = false;
    bool list_open_ = false;
};

enum class PvrTaPacketKind : std::uint8_t {
    EndOfList,
    UserClip,
    ObjectListSet,
    ReservedParameter3,
    PolygonHeader,
    SpriteHeader,
    ReservedParameter6,
    Vertex,
    IntensityContinuation,
    SpriteContinuation,
    ExtendedVertexContinuation,
    ModifierVertexContinuation,
    Count,
};

inline constexpr std::size_t pvr_ta_packet_kind_count =
    static_cast<std::size_t>(PvrTaPacketKind::Count);
inline constexpr std::uint64_t pvr_ta_maximum_frame_packets =
    0x00800000u / 32u;

struct PvrTaMetrics {
    std::uint64_t packets = 0u;
    std::array<std::uint64_t, pvr_ta_packet_kind_count> normalized_packets{};
    std::uint64_t polygon_headers = 0u;
    std::uint64_t vertices = 0u;
    std::uint64_t list_completions = 0u;
    std::uint64_t frames = 0u;
    std::uint64_t continuations = 0u;
    std::uint64_t rejected_packets = 0u;
};

enum class PvrTaInputErrorReason : std::uint8_t {
    InvalidPacket,
    UnsupportedPacket,
    InvalidListOrder,
    IncompletePacket,
    BufferOverflow,
};

class PvrTaParserException final : public std::runtime_error {
  public:
    PvrTaParserException(PvrTaInputErrorReason reason, std::string detail)
        : std::runtime_error(std::move(detail)), reason_(reason) {}

    [[nodiscard]] PvrTaInputErrorReason reason() const noexcept { return reason_; }

  private:
    PvrTaInputErrorReason reason_;
};

struct PvrTaInputError {
    PvrTaInputErrorReason reason = PvrTaInputErrorReason::InvalidPacket;
    std::uint64_t packet = 0u;
    std::string detail;

    [[nodiscard]] bool operator==(const PvrTaInputError&) const = default;
};

struct PvrTaSubmitResult {
    bool accepted = false;
    std::optional<PvrTaInputError> error;
};

struct PvrTaFifoSnapshot {
    TileAcceleratorSnapshot accelerator;
    PvrListType active_list = PvrListType::Opaque;
    bool active_textured = false;
    bool active_uv16 = false;
    std::uint8_t active_color_type = 0u;
    bool active_sprite = false;
    bool active_two_volume = false;
    std::uint32_t active_header_argb = 0u;
    std::uint32_t active_header_oargb = 0u;
    std::uint32_t active_volume_header_argb = 0u;
    bool intensity_face_color_valid = false;
    PvrMaterial active_material;
    std::uint16_t user_clip_start_x = 0u;
    std::uint16_t user_clip_start_y = 0u;
    std::uint16_t user_clip_end_x = 0u;
    std::uint16_t user_clip_end_y = 0u;
    std::optional<std::array<std::uint8_t, 32u>> pending_sprite_vertex;
    std::optional<PvrVertex> pending_extended_vertex;
    bool pending_intensity_header = false;
    bool pending_extended_end_of_strip = false;
    std::vector<PvrModifierVolume> modifier_volumes;
    std::optional<std::size_t> active_modifier_volume;
    std::optional<std::array<std::uint8_t, 32u>> pending_modifier_vertex_packet;
    PvrTaMetrics metrics{};
    std::uint64_t frame_packets = 0u;
    std::optional<PvrTaInputError> first_input_error;
};

class PvrTaFifo final {
  public:
    explicit PvrTaFifo(std::function<void(PvrListType)> list_observer = {});
    void submit(std::span<const std::uint8_t> packet);
    [[nodiscard]] PvrTaSubmitResult submit_guest(std::span<const std::uint8_t> packet);
    [[nodiscard]] PvrTaSubmitResult
    reject_guest(PvrTaInputErrorReason reason, std::string detail);
    [[nodiscard]] PvrTaFrame finish_frame();
    [[nodiscard]] const PvrTaMetrics& metrics() const noexcept;
    [[nodiscard]] PvrTaFifoSnapshot snapshot() const;
    void validate_state_restore(const PvrTaFifoSnapshot& state) const;
    void restore_state_passive(PvrTaFifoSnapshot state);
    void commit_validated_state_restore(
        PvrTaFifoSnapshot state) noexcept;
    void continue_list();
    void reset() noexcept;

  private:
    void discard_frame_state() noexcept;
    TileAccelerator accelerator_;
    std::function<void(PvrListType)> list_observer_;
    PvrListType active_list_ = PvrListType::Opaque;
    bool active_textured_ = false;
    bool active_uv16_ = false;
    std::uint8_t active_color_type_ = 0u;
    bool active_sprite_ = false;
    bool active_two_volume_ = false;
    std::uint32_t active_header_argb_ = 0xFFFFFFFFu;
    std::uint32_t active_header_oargb_ = 0u;
    std::uint32_t active_volume_header_argb_ = 0xFFFFFFFFu;
    bool intensity_face_color_valid_ = false;
    PvrMaterial active_material_;
    std::uint16_t user_clip_start_x_ = 0u;
    std::uint16_t user_clip_start_y_ = 0u;
    std::uint16_t user_clip_end_x_ = 0u;
    std::uint16_t user_clip_end_y_ = 0u;
    std::optional<std::array<std::uint8_t, 32u>> pending_sprite_vertex_;
    std::optional<PvrVertex> pending_extended_vertex_;
    bool pending_intensity_header_ = false;
    bool pending_extended_end_of_strip_ = false;
    std::vector<PvrModifierVolume> modifier_volumes_;
    std::optional<std::size_t> active_modifier_volume_;
    std::optional<std::array<std::uint8_t, 32u>> pending_modifier_vertex_packet_;
    PvrTaMetrics metrics_;
    std::uint64_t frame_packets_ = 0u;
    std::optional<PvrTaInputError> first_input_error_;
};

inline constexpr std::size_t pvr_channel2_transfer_unit_size = 32u;

enum class PvrChannel2DestinationKind : std::uint8_t {
    TaFifo,
    YuvConverter,
    DirectTexturePath0,
    DirectTexturePath1,
};

struct PvrChannel2DestinationPlan {
    std::uint32_t initial_address = 0u;
    std::size_t byte_count = 0u;
    std::size_t unit_count = 0u;
    PvrChannel2DestinationKind kind = PvrChannel2DestinationKind::TaFifo;

    [[nodiscard]] bool destination_progresses() const noexcept;
    [[nodiscard]] std::uint32_t destination_for_unit(std::size_t unit) const;
};

[[nodiscard]] PvrChannel2DestinationPlan
plan_pvr_channel2_destination(std::uint32_t destination, std::size_t byte_count);

class PvrTaFifoMemoryDevice final : public MemoryDevice {
  public:
    static constexpr std::size_t aperture_size = 0x00800000u;
    explicit PvrTaFifoMemoryDevice(std::shared_ptr<PvrTaFifo> fifo,
                                   std::shared_ptr<PvrRegisterFile> registers = {});
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::uint8_t read_u8(std::uint32_t offset) const override;
    void write_u8(std::uint32_t offset, std::uint8_t value) override;
    struct Snapshot {
        std::array<std::uint8_t, 32u> packet{};
        std::uint32_t packet_base = 0u;
        std::uint32_t written_mask = 0u;
        bool packet_active = false;

        [[nodiscard]] bool operator==(const Snapshot&) const = default;
    };
    [[nodiscard]] Snapshot snapshot() const noexcept;
    void validate_state_restore(const Snapshot& state) const;
    void restore_state_passive(Snapshot state);
    void commit_validated_state_restore(Snapshot state) noexcept;
    void reset() noexcept;

  private:
    std::shared_ptr<PvrTaFifo> fifo_;
    std::shared_ptr<PvrRegisterFile> registers_;
    std::array<std::uint8_t, 32u> packet_{};
    std::uint32_t packet_base_ = 0u;
    std::uint32_t written_mask_ = 0u;
    bool packet_active_ = false;
};

class PvrYuvConverterMemoryDevice final : public MemoryDevice {
  public:
    static constexpr std::size_t aperture_size = 0x00800000u;
    PvrYuvConverterMemoryDevice(std::shared_ptr<PvrRegisterFile> registers,
                                std::shared_ptr<LinearMemoryDevice> vram,
                                std::function<void()> completion_observer = {});
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::uint8_t read_u8(std::uint32_t offset) const override;
    void write_u8(std::uint32_t offset, std::uint8_t value) override;
    void set_guest_memory_access_memory(Memory* memory) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::uint64_t converted_macroblocks() const noexcept;
    struct Snapshot {
        std::vector<std::uint8_t> input;
        std::uint32_t configuration = 0u;
        std::uint32_t destination = 0u;
        std::uint32_t frame_macroblock = 0u;
        std::uint64_t converted_macroblocks = 0u;
        bool guest_memory_access_bound = false;
    };
    [[nodiscard]] Snapshot snapshot() const;
    void validate_state_restore(const Snapshot& state) const;
    void restore_state_passive(Snapshot state);
    void commit_validated_state_restore(Snapshot state) noexcept;

  private:
    void refresh_configuration();
    void convert_macroblock();
    std::shared_ptr<PvrRegisterFile> registers_;
    std::shared_ptr<LinearMemoryDevice> vram_;
    std::function<void()> completion_observer_;
    Memory* guest_memory_access_memory_ = nullptr;
    std::vector<std::uint8_t> input_;
    std::uint32_t configuration_ = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t destination_ = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t frame_macroblock_ = 0u;
    std::uint64_t converted_macroblocks_ = 0u;
};

struct PvrSoftwareRenderMetrics {
    std::uint64_t frames = 0u;
    std::uint64_t triangles = 0u;
    std::uint64_t pixels = 0u;
    std::uint64_t pixel_writes = 0u;
    std::uint64_t changed_pixels = 0u;
    std::uint64_t proven_guest_frames = 0u;
    std::uint64_t direct_scanout_frames = 0u;
    std::uint64_t direct_scanout_changed_pixels = 0u;
    std::uint64_t last_frame_pixel_writes = 0u;
    std::uint64_t last_frame_changed_pixels = 0u;
    std::uint64_t dropped_render_evidence_generations = 0u;
    std::uint64_t render_evidence_pixels_examined = 0u;
    std::uint64_t render_evidence_range_rejections = 0u;
    std::uint64_t render_evidence_scan_budget_exhaustions = 0u;
};

enum class PvrGuestFrameProofSource : std::uint8_t { TaRender, DirectFramebuffer };

struct PvrChangedPixelEvidence {
    std::uint32_t offset = 0u;
    std::uint32_t packed_value = 0u;
    std::uint32_t changed_byte_mask = 0u;
};

struct PvrRenderGenerationEvidence {
    std::uint64_t generation = 0u;
    std::uint32_t write_base = 0u;
    std::uint32_t stride_bytes = 0u;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint8_t pixel_bytes = 0u;
    bool render_to_texture = false;
    std::uint64_t pixel_writes = 0u;
    std::uint64_t changed_pixels = 0u;
    std::size_t validation_cursor = 0u;
    std::vector<PvrChangedPixelEvidence> changed_pixel_values;
};

struct PvrGuestFrameProof {
    std::uint64_t render_generation = 0u;
    std::uint64_t changed_pixels = 0u;
    std::uint32_t scanout_field = 0u;
    PvrScanoutDescriptor scanout;
    PvrFrame frame;
    PvrGuestFrameProofSource source = PvrGuestFrameProofSource::TaRender;
    std::uint64_t write_generation_first = 0u;
    std::uint64_t write_generation_last = 0u;
};

struct PvrRenderFirstError {
    PvrRenderError error = PvrRenderError::InvalidConfiguration;
    std::uint64_t render_request = 0u;
    std::string detail;
};

struct PvrSoftwareRendererSnapshot {
    PvrSoftwareRenderMetrics metrics{};
    std::uint64_t next_render_generation = 0u;
    std::uint64_t last_render_generation = 0u;
    std::deque<PvrRenderGenerationEvidence> pending_render_evidence;
    std::size_t pending_render_evidence_bytes = 0u;
    std::uint64_t next_evidence_scan_generation = 0u;
    std::uint64_t next_direct_write_generation = 0u;
    // Compatibility view for existing probe consumers; equals the last generation.
    std::uint64_t pending_direct_write_generation = 0u;
    std::uint64_t pending_direct_first_write_generation = 0u;
    std::uint64_t pending_direct_last_write_generation = 0u;
    std::vector<std::uint64_t> direct_dirty_words;
    std::size_t direct_dirty_byte_count = 0u;
    std::vector<std::uint8_t> direct_vram_shadow;
    bool guest_memory_access_bound = false;
    bool direct_vram_shadow_valid = false;
    std::optional<PvrGuestFrameProof> queued_guest_frame_proof;
    std::optional<PvrRenderFirstError> first_error;
};

[[nodiscard]] const char* pvr_render_error_name(PvrRenderError error) noexcept;

class PvrSoftwareRenderer final {
  public:
    static constexpr std::size_t render_evidence_capacity = 256u;
    static constexpr std::size_t render_evidence_byte_capacity = 64u << 20u;
    static constexpr std::size_t render_evidence_scan_pixel_budget = 2u << 20u;

    void render(const PvrTaFrame& frame,
                const PvrRegisterFile& registers,
                LinearMemoryDevice& vram);
    void render(const PvrTaFrame& frame,
                const PvrRegisterSnapshot& registers,
                LinearMemoryDevice& vram);
    void render(const PvrTaFrame& frame,
                const PvrRegisterSnapshot& registers,
                LinearMemoryDevice& vram,
                std::uint64_t render_generation);
    void set_guest_memory_access_memory(Memory* memory) noexcept;
    void set_texture_memory_mode_control(
        const DreamcastSystemBusControl* system_bus_control) noexcept;
    void observe_vram_write(std::uint32_t address,
                            std::size_t size,
                            bool bytes_changed = true);
    void reset_guest_frame_evidence(std::span<const std::uint8_t> vram = {});
    void observe_vblank_scanout(const PvrRegisterFile& registers,
                                std::span<const std::uint8_t> vram);
    [[nodiscard]] std::optional<PvrGuestFrameProof> take_guest_frame_proof();
    [[nodiscard]] std::optional<PvrFrame> take_scanout_frame();
    // Presentation retries are bounded by the existing one-proof/one-scanout
    // queues. Any subsequently observed scanout wins over a retained proof;
    // byte-identical pixels do not imply the same presentation generation.
    [[nodiscard]] bool retain_unpresented_guest_frame_proof(
        PvrGuestFrameProof proof);
    [[nodiscard]] bool retain_unpresented_scanout_frame(PvrFrame frame);
    [[nodiscard]] const PvrSoftwareRenderMetrics& metrics() const noexcept;
    [[nodiscard]] std::size_t render_job_capacity() const noexcept;
    [[nodiscard]] std::size_t last_render_job_count() const noexcept;
    [[nodiscard]] std::uint64_t last_render_generation() const noexcept;
    [[nodiscard]] std::size_t pending_render_generations() const noexcept;
    [[nodiscard]] std::size_t pending_render_evidence_bytes() const noexcept;
    void record_error(PvrRenderError error, std::uint64_t render_request, std::string detail);
    [[nodiscard]] const std::optional<PvrRenderFirstError>& first_error() const noexcept;
    [[nodiscard]] PvrSoftwareRendererSnapshot snapshot() const;
    void validate_state_restore(const PvrSoftwareRendererSnapshot& state) const;
    void restore_state_passive(PvrSoftwareRendererSnapshot state);
    void commit_validated_state_restore(
        PvrSoftwareRendererSnapshot state) noexcept;

  private:
    PvrSoftwareRenderMetrics metrics_;
    std::uint64_t next_render_generation_ = 1u;
    std::uint64_t last_render_generation_ = 0u;
    std::deque<PvrRenderGenerationEvidence> pending_render_evidence_;
    std::size_t pending_render_evidence_bytes_ = 0u;
    std::uint64_t next_evidence_scan_generation_ = 0u;
    std::uint64_t next_direct_write_generation_ = 1u;
    std::uint64_t pending_direct_first_write_generation_ = 0u;
    std::uint64_t pending_direct_last_write_generation_ = 0u;
    std::vector<std::uint64_t> direct_dirty_words_;
    std::size_t direct_dirty_byte_count_ = 0u;
    std::vector<std::uint8_t> direct_vram_shadow_;
    Memory* guest_memory_access_memory_ = nullptr;
    const DreamcastSystemBusControl* texture_memory_mode_control_ = nullptr;
    bool direct_vram_shadow_valid_ = true;
    std::optional<PvrGuestFrameProof> queued_guest_frame_proof_;
    // Host presentation is deliberately independent from guest-frame evidence.
    // This latest-wins queue is transient host-boundary state and is rebuilt from
    // the restored PVR registers and VRAM at the next VBlank.
    std::optional<PvrFrame> queued_scanout_frame_;
    std::optional<PvrRenderFirstError> first_error_;
    std::size_t last_render_jobs_ = 1u;
};

inline constexpr std::uint32_t dreamcast_pvr_state_contract_version = 1u;

struct DreamcastPvrStateSnapshot {
    std::uint32_t contract_version = dreamcast_pvr_state_contract_version;
    PvrRegisterSnapshot registers;
    PvrTaFifoSnapshot ta_fifo;
    PvrTaFifoMemoryDevice::Snapshot ta_aperture;
    PvrYuvConverterMemoryDevice::Snapshot yuv;
    PvrSoftwareRendererSnapshot renderer;
};

class PreparedDreamcastPvrStateRestore final {
  public:
    PreparedDreamcastPvrStateRestore(
        const PreparedDreamcastPvrStateRestore&) = delete;
    PreparedDreamcastPvrStateRestore& operator=(
        const PreparedDreamcastPvrStateRestore&) = delete;
    PreparedDreamcastPvrStateRestore(
        PreparedDreamcastPvrStateRestore&&) noexcept = default;
    PreparedDreamcastPvrStateRestore& operator=(
        PreparedDreamcastPvrStateRestore&&) noexcept = default;

  private:
    PreparedDreamcastPvrStateRestore() = default;
    friend PreparedDreamcastPvrStateRestore
    prepare_dreamcast_pvr_state_restore(
        const PvrRegisterFile&,
        const PvrTaFifo&,
        const PvrTaFifoMemoryDevice&,
        const PvrYuvConverterMemoryDevice&,
        const PvrSoftwareRenderer&,
        DreamcastPvrStateSnapshot);
    friend void commit_dreamcast_pvr_state_restore(
        PvrRegisterFile&,
        PvrTaFifo&,
        PvrTaFifoMemoryDevice&,
        PvrYuvConverterMemoryDevice&,
        PvrSoftwareRenderer&,
        PreparedDreamcastPvrStateRestore) noexcept;

    DreamcastPvrStateSnapshot state_;
};

[[nodiscard]] DreamcastPvrStateSnapshot snapshot_dreamcast_pvr_state(
    const PvrRegisterFile& registers,
    const PvrTaFifo& ta_fifo,
    const PvrTaFifoMemoryDevice& ta_aperture,
    const PvrYuvConverterMemoryDevice& yuv,
    const PvrSoftwareRenderer& renderer);
// Normalizes only run-local host observations in a detached capture. A fresh
// product epoch uses the final target VRAM as its direct-write baseline so
// restored pixels cannot become new frame evidence after the live commit.
void normalize_dreamcast_pvr_observations_for_restore(
    DreamcastPvrStateSnapshot& state,
    ObservationRestorePolicy policy,
    std::span<const std::uint8_t> final_vram = {});
void validate_dreamcast_pvr_state_restore(
    const PvrRegisterFile& registers,
    const PvrTaFifo& ta_fifo,
    const PvrTaFifoMemoryDevice& ta_aperture,
    const PvrYuvConverterMemoryDevice& yuv,
    const PvrSoftwareRenderer& renderer,
    const DreamcastPvrStateSnapshot& state);
[[nodiscard]] PreparedDreamcastPvrStateRestore
prepare_dreamcast_pvr_state_restore(
    const PvrRegisterFile& registers,
    const PvrTaFifo& ta_fifo,
    const PvrTaFifoMemoryDevice& ta_aperture,
    const PvrYuvConverterMemoryDevice& yuv,
    const PvrSoftwareRenderer& renderer,
    DreamcastPvrStateSnapshot state);
void commit_dreamcast_pvr_state_restore(
    PvrRegisterFile& registers,
    PvrTaFifo& ta_fifo,
    PvrTaFifoMemoryDevice& ta_aperture,
    PvrYuvConverterMemoryDevice& yuv,
    PvrSoftwareRenderer& renderer,
    PreparedDreamcastPvrStateRestore prepared) noexcept;
void restore_dreamcast_pvr_state_passive(
    PvrRegisterFile& registers,
    PvrTaFifo& ta_fifo,
    PvrTaFifoMemoryDevice& ta_aperture,
    PvrYuvConverterMemoryDevice& yuv,
    PvrSoftwareRenderer& renderer,
    DreamcastPvrStateSnapshot state);
[[nodiscard]] std::vector<std::uint8_t>
encode_dreamcast_pvr_state(const DreamcastPvrStateSnapshot& state);
[[nodiscard]] DreamcastPvrStateSnapshot
decode_dreamcast_pvr_state(std::span<const std::uint8_t> bytes);

enum class PvrTextureFormat : std::uint8_t { Rgb565, Argb1555, Argb4444 };

struct PvrTexture {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] PvrTexture decode_pvr_texture(std::span<const std::uint8_t> source,
                                            std::uint32_t width,
                                            std::uint32_t height,
                                            PvrTextureFormat format);

class PvrRenderBackend {
  public:
    virtual ~PvrRenderBackend() = default;
    virtual void render(const PvrTaFrame& frame, std::span<const PvrTexture> textures) = 0;
};

class RecordingPvrRenderBackend final : public PvrRenderBackend {
  public:
    void render(const PvrTaFrame& frame, std::span<const PvrTexture> textures) override;
    [[nodiscard]] std::uint64_t submitted_frames() const noexcept;
    [[nodiscard]] const PvrTaFrame& last_frame() const noexcept;
    [[nodiscard]] const std::vector<PvrTexture>& last_textures() const noexcept;

  private:
    std::uint64_t submitted_frames_ = 0u;
    PvrTaFrame last_frame_;
    std::vector<PvrTexture> last_textures_;
};

[[nodiscard]] std::shared_ptr<PvrRegisterFile>
map_pvr_registers(Memory& memory,
                  EventScheduler& scheduler,
                  std::function<void()> render_observer = {},
                  PvrTiming timing = {},
                  std::function<void(bool)> vblank_observer = {});

} // namespace katana::runtime
