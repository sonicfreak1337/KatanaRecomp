#include "katana/io/raw_binary_loader.hpp"

#include "katana/io/binary_reader.hpp"

#include <stdexcept>
#include <utility>

namespace katana::io {

ExecutableImage load_raw_binary(const std::filesystem::path& path,
                                const RawBinaryLoadOptions& options) {
    auto bytes = read_binary_file(path);
    if (bytes.empty()) {
        throw std::runtime_error("Raw-Binaerdatei ist leer: " + path.string() + " (Offset 0).");
    }

    ExecutableImage image(path);
    ImageSegment segment{options.segment_name,
                         options.base_address,
                         0u,
                         bytes.size(),
                         options.segment_kind,
                         options.permissions,
                         std::move(bytes)};
    segment.source_kind = ImageSourceKind::RawBinary;
    segment.local_source_name = path.filename().string();
    image.add_segment(std::move(segment));
    if (options.entry_point.has_value()) {
        image.add_entry_point(*options.entry_point);
    }
    return image;
}

} // namespace katana::io
