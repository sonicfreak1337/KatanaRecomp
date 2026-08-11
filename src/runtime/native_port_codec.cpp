#include "katana/runtime/native_port_codec.hpp"

#include <algorithm>
#include <iterator>
namespace katana::runtime {

bool valid_native_port_codec_provider(const NativePortCodecProvider& provider) noexcept {
    if (provider.contract_version != native_port_codec_provider_contract_version ||
        provider.structure_size < native_port_codec_provider_structure_size ||
        provider.open == nullptr || provider.read_next == nullptr || provider.close == nullptr)
        return false;
    const auto end =
        std::find(std::begin(provider.provider_name), std::end(provider.provider_name), '\0');
    return end != std::begin(provider.provider_name) && end != std::end(provider.provider_name) &&
           std::all_of(std::begin(provider.provider_name), end, [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '-' ||
                      character == '_' || character == '.';
           });
}

} // namespace katana::runtime
