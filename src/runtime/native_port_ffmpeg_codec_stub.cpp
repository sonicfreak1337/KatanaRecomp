#include "katana/runtime/native_port_ffmpeg_codec.hpp"

#include <cstring>

namespace katana::runtime {
namespace {

void open_unsupported(void*,
                      const NativePortCodecOpenRequest*,
                      NativePortCodecOpenResult* result) noexcept {
    if (result == nullptr) return;
    *result = {};
    result->failure = NativePortCodecFailure::UnsupportedCodec;
}

void read_unsupported(void*, NativePortCodecReadResult* result) noexcept {
    if (result == nullptr) return;
    *result = {};
    result->failure = NativePortCodecFailure::UnsupportedCodec;
}

void close_unsupported(void*) noexcept {}

} // namespace

const NativePortCodecProvider& native_port_ffmpeg_codec_provider() noexcept {
    static const NativePortCodecProvider provider = [] {
        NativePortCodecProvider value;
        value.structure_size = native_port_codec_provider_structure_size;
        constexpr char name[] = "ffmpeg-unavailable";
        std::memcpy(value.provider_name, name, sizeof(name));
        value.open = &open_unsupported;
        value.read_next = &read_unsupported;
        value.close = &close_unsupported;
        return value;
    }();
    return provider;
}

} // namespace katana::runtime
