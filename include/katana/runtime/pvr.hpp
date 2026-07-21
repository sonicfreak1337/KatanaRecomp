#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

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

class PvrRegisterFile final {
  public:
    explicit PvrRegisterFile(EventScheduler& scheduler,
                             PvrTiming timing = {},
                             std::function<void()> render_observer = {},
                             std::function<void(bool)> vblank_observer = {});
    ~PvrRegisterFile();
    PvrRegisterFile(const PvrRegisterFile&) = delete;
    PvrRegisterFile& operator=(const PvrRegisterFile&) = delete;
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t render_request_count() const noexcept;
    [[nodiscard]] std::uint64_t render_completion_count() const noexcept;
    [[nodiscard]] std::uint64_t reset_count() const noexcept;
    [[nodiscard]] std::uint64_t vblank_in_count() const noexcept;
    [[nodiscard]] std::uint64_t vblank_out_count() const noexcept;
    [[nodiscard]] std::uint64_t hblank_count() const noexcept;
    [[nodiscard]] bool in_vblank() const noexcept;
    [[nodiscard]] std::uint32_t field() const noexcept;
    void set_render_observer(std::function<void()> observer);
    void set_vblank_observer(std::function<void(bool)> observer);
    void set_hblank_observer(std::function<void()> observer);
    void set_ta_reset_observer(std::function<void()> observer);
    void set_ta_continue_observer(std::function<void()> observer);
    void record_ta_packet(std::uint32_t bytes);

  private:
    [[nodiscard]] static std::size_t index(std::uint32_t offset);
    void complete_render(SchedulerEventId event_id);
    void initialize_register_defaults() noexcept;
    void reschedule_scanout();
    void schedule_scan_event(std::uint32_t line, bool entering);
    void handle_scan_event(SchedulerEventId event_id, bool entering);
    void schedule_hblank_event(std::uint32_t line);
    void handle_hblank_event(SchedulerEventId event_id, std::uint32_t line);
    void cancel_scan_events() noexcept;
    void handle_scheduler_reset() noexcept;
    EventScheduler& scheduler_;
    PvrTiming timing_;
    std::array<std::uint32_t, pvr_register_size / 4u> registers_{};
    std::uint64_t render_requests_ = 0u;
    std::uint64_t render_completions_ = 0u;
    std::uint64_t resets_ = 0u;
    SchedulerResetObserverId reset_observer_ = 0u;
    SchedulerLifetimeToken scheduler_lifetime_;
    std::set<SchedulerEventId> render_events_;
    std::function<void()> render_observer_;
    std::function<void(bool)> vblank_observer_;
    std::function<void()> hblank_observer_;
    std::function<void()> ta_reset_observer_;
    std::function<void()> ta_continue_observer_;
    std::optional<SchedulerEventId> vblank_in_event_;
    std::optional<SchedulerEventId> vblank_out_event_;
    std::optional<SchedulerEventId> hblank_event_;
    std::uint64_t vblank_in_count_ = 0u;
    std::uint64_t vblank_out_count_ = 0u;
    std::uint64_t hblank_count_ = 0u;
    std::uint64_t scan_frame_cycles_ = 0u;
    std::uint64_t scan_epoch_cycle_ = 0u;
    bool in_vblank_ = false;
    std::uint32_t field_ = 0u;
};

void configure_dreamcast_video(PvrRegisterFile& registers, DreamcastVideoMode mode);

enum class PvrFramebufferFormat : std::uint8_t { Rgb565, Argb1555, Rgb888, Rgb0888 };

struct PvrScanoutDescriptor {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t source_width = 0u;
    std::uint32_t source_height = 0u;
    std::uint32_t stride_bytes = 0u;
    std::size_t base_offset = 0u;
    std::size_t second_base_offset = 0u;
    PvrFramebufferFormat format = PvrFramebufferFormat::Rgb565;
    bool line_double = false;
    bool interlaced = false;
    bool horizontal_scale = false;
    std::uint16_t vertical_scale_factor = 0x0400u;
};

[[nodiscard]] std::optional<PvrScanoutDescriptor>
decode_pvr_scanout(const PvrRegisterFile& registers, std::size_t vram_size);

struct PvrFrame {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::vector<std::uint8_t> rgba;
};

class PvrFramebuffer final {
  public:
    void configure(std::uint32_t width,
                   std::uint32_t height,
                   std::uint32_t stride_bytes,
                   PvrFramebufferFormat format,
                   bool line_double = false,
                   bool interlaced = false,
                   std::uint32_t source_width = 0u,
                   std::uint32_t source_height = 0u);
    [[nodiscard]] PvrFrame capture(std::span<const std::uint8_t> vram,
                                   std::size_t base_offset = 0u,
                                   std::optional<std::size_t> second_base_offset = std::nullopt);
    [[nodiscard]] std::uint64_t presented_frames() const noexcept;

  private:
    std::uint32_t width_ = 0u;
    std::uint32_t height_ = 0u;
    std::uint32_t source_width_ = 0u;
    std::uint32_t source_height_ = 0u;
    std::uint32_t stride_ = 0u;
    PvrFramebufferFormat format_ = PvrFramebufferFormat::Rgb565;
    bool line_double_ = false;
    bool interlaced_ = false;
    std::uint64_t presented_frames_ = 0u;
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

class TileAccelerator final {
  public:
    void begin_list(PvrListType type);
    void set_material(PvrMaterial material);
    void submit_vertex(const PvrVertex& vertex, bool end_of_strip);
    void end_list();
    [[nodiscard]] PvrTaFrame finish_frame();
    [[nodiscard]] bool list_open() const noexcept;

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

struct PvrTaMetrics {
    std::uint64_t packets = 0u;
    std::uint64_t polygon_headers = 0u;
    std::uint64_t vertices = 0u;
    std::uint64_t list_completions = 0u;
    std::uint64_t frames = 0u;
    std::uint64_t continuations = 0u;
};

class PvrTaFifo final {
  public:
    explicit PvrTaFifo(std::function<void(PvrListType)> list_observer = {});
    void submit(std::span<const std::uint8_t> packet);
    [[nodiscard]] PvrTaFrame finish_frame();
    [[nodiscard]] const PvrTaMetrics& metrics() const noexcept;
    void continue_list();
    void reset() noexcept;

  private:
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
};

class PvrTaFifoMemoryDevice final : public MemoryDevice {
  public:
    static constexpr std::size_t aperture_size = 0x00800000u;
    explicit PvrTaFifoMemoryDevice(std::shared_ptr<PvrTaFifo> fifo,
                                   std::shared_ptr<PvrRegisterFile> registers = {});
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::uint8_t read_u8(std::uint32_t offset) const override;
    void write_u8(std::uint32_t offset, std::uint8_t value) override;

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
    [[nodiscard]] std::uint64_t converted_macroblocks() const noexcept;

  private:
    void refresh_configuration();
    void convert_macroblock();
    std::shared_ptr<PvrRegisterFile> registers_;
    std::shared_ptr<LinearMemoryDevice> vram_;
    std::function<void()> completion_observer_;
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
};

enum class PvrRenderError : std::uint8_t {
    InvalidTaState,
    InvalidConfiguration,
    MemoryRange,
    UnsupportedFeature,
    InternalLifecycle
};

struct PvrRenderFirstError {
    PvrRenderError error = PvrRenderError::InvalidConfiguration;
    std::uint64_t render_request = 0u;
    std::string detail;
};

[[nodiscard]] const char* pvr_render_error_name(PvrRenderError error) noexcept;

class PvrSoftwareRenderer final {
  public:
    void render(const PvrTaFrame& frame,
                const PvrRegisterFile& registers,
                LinearMemoryDevice& vram);
    [[nodiscard]] const PvrSoftwareRenderMetrics& metrics() const noexcept;
    void record_error(PvrRenderError error, std::uint64_t render_request, std::string detail);
    [[nodiscard]] const std::optional<PvrRenderFirstError>& first_error() const noexcept;

  private:
    PvrSoftwareRenderMetrics metrics_;
    std::optional<PvrRenderFirstError> first_error_;
};

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
