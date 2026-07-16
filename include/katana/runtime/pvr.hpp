#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t pvr_register_physical_base = 0x005F8000u;
inline constexpr std::size_t pvr_register_size = 0x200u;
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
inline constexpr std::uint32_t FramebufferReadSof1 = 0x050u;
inline constexpr std::uint32_t FramebufferReadSize = 0x05Cu;
inline constexpr std::uint32_t VideoControl = 0x0E8u;
} // namespace pvr_register

class PvrRegisterFile final {
  public:
    explicit PvrRegisterFile(std::function<void()> render_observer = {});
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t render_request_count() const noexcept;
    [[nodiscard]] std::uint64_t reset_count() const noexcept;

  private:
    [[nodiscard]] static std::size_t index(std::uint32_t offset);
    std::array<std::uint32_t, pvr_register_size / 4u> registers_{};
    std::uint64_t render_requests_ = 0u;
    std::uint64_t resets_ = 0u;
    std::function<void()> render_observer_;
};

enum class PvrFramebufferFormat : std::uint8_t { Rgb565, Argb1555, Rgb888 };

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
                   PvrFramebufferFormat format);
    [[nodiscard]] PvrFrame capture(std::span<const std::uint8_t> vram,
                                   std::size_t base_offset = 0u);
    [[nodiscard]] std::uint64_t presented_frames() const noexcept;

  private:
    std::uint32_t width_ = 0u;
    std::uint32_t height_ = 0u;
    std::uint32_t stride_ = 0u;
    PvrFramebufferFormat format_ = PvrFramebufferFormat::Rgb565;
    std::uint64_t presented_frames_ = 0u;
};

enum class PvrListType : std::uint8_t { Opaque, PunchThrough, Translucent };

struct PvrVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t argb = 0xFFFFFFFFu;
};

struct PvrPrimitive {
    PvrListType list = PvrListType::Opaque;
    std::vector<PvrVertex> vertices;
};

struct PvrTaFrame {
    std::vector<PvrPrimitive> primitives;
};

class TileAccelerator final {
  public:
    void begin_list(PvrListType type);
    void submit_vertex(const PvrVertex& vertex, bool end_of_strip);
    void end_list();
    [[nodiscard]] PvrTaFrame finish_frame();
    [[nodiscard]] bool list_open() const noexcept;

  private:
    [[nodiscard]] static std::uint8_t list_rank(PvrListType type) noexcept;
    std::vector<PvrPrimitive> primitives_;
    std::vector<PvrVertex> current_strip_;
    PvrListType current_list_ = PvrListType::Opaque;
    std::uint8_t highest_list_rank_ = 0u;
    bool frame_has_list_ = false;
    bool list_open_ = false;
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
map_pvr_registers(Memory& memory, std::function<void()> render_observer = {});

} // namespace katana::runtime
