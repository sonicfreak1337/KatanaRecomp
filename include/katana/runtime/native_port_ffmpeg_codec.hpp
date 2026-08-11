#pragma once

#include "katana/runtime/native_port_codec.hpp"

namespace katana::runtime {

// Built-in, process-local FFmpeg provider. The linked FFmpeg configuration is
// checked at runtime and rejected when GPL or nonfree components are enabled.
[[nodiscard]] const NativePortCodecProvider& native_port_ffmpeg_codec_provider() noexcept;

} // namespace katana::runtime
