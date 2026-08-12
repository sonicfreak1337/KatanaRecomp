#include "katana/io/executable_image.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace katana::io {
namespace {

constexpr std::uint64_t kAddressSpaceSize = 0x100000000ull;
constexpr std::uint64_t kSh4DirectWindowSize = 0x20000000ull;
constexpr std::uint32_t kSh4DirectAddressLimit = 0xE0000000u;
std::atomic_uint64_t next_executable_image_identity{1u};

[[nodiscard]] std::uint64_t
allocate_executable_image_identity() noexcept {
    auto identity = next_executable_image_identity.fetch_add(
        1u, std::memory_order_relaxed);
    if (identity == 0u) {
        identity = next_executable_image_identity.fetch_add(
            1u, std::memory_order_relaxed);
    }
    return identity;
}

bool ranges_overlap(const std::uint64_t first_begin,
                    const std::uint64_t first_end,
                    const std::uint64_t second_begin,
                    const std::uint64_t second_end) noexcept {
    return first_begin < second_end && second_begin < first_end;
}

struct Sh4PhysicalRange {
    std::uint64_t begin = 0u;
    std::uint64_t end = 0u;
};

std::optional<Sh4PhysicalRange>
sh4_physical_range(const std::uint32_t start,
                   const std::uint64_t size) {
    if (size == 0u || start >= kSh4DirectAddressLimit)
        return std::nullopt;
    const auto virtual_end = static_cast<std::uint64_t>(start) + size;
    if (virtual_end > kSh4DirectAddressLimit ||
        ((start >> 29u) !=
         (static_cast<std::uint32_t>(virtual_end - 1u) >> 29u)))
        throw std::invalid_argument(
            "SH-4-Direktbereich kreuzt eine physisch nichtlineare "
            "Adressfenstergrenze.");
    const auto physical_begin =
        static_cast<std::uint64_t>(start & 0x1FFFFFFFu);
    if (physical_begin + size > kSh4DirectWindowSize)
        throw std::invalid_argument(
            "SH-4-Direktbereich laeuft ueber den physischen Adressraum.");
    return Sh4PhysicalRange{physical_begin, physical_begin + size};
}

void require_sh4_unique_segment(
    const ImageSegment& candidate,
    const std::span<const ImageSegment> segments,
    const std::span<const ImageAddressAlias> aliases) {
    const auto physical =
        sh4_physical_range(candidate.virtual_address,
                           candidate.memory_size);
    if (!physical.has_value())
        return;
    for (const auto& existing : segments) {
        const auto other =
            sh4_physical_range(existing.virtual_address,
                               existing.memory_size);
        if (other.has_value() &&
            ranges_overlap(
                physical->begin, physical->end, other->begin, other->end))
            throw std::invalid_argument(
                "SH-4-Direktsegmente besitzen mehrdeutige physische "
                "Bereiche.");
    }
    for (const auto& alias : aliases) {
        const auto runtime =
            sh4_physical_range(alias.runtime_start, alias.size);
        if (runtime.has_value() &&
            ranges_overlap(
                physical->begin, physical->end, runtime->begin, runtime->end))
            throw std::invalid_argument(
                "SH-4-Direktsegment ueberschattet einen "
                "Runtime-Adressalias.");
    }
}

void require_sh4_unique_alias(
    const ImageAddressAlias& candidate,
    const std::span<const ImageSegment> segments,
    const std::span<const ImageAddressAlias> aliases) {
    static_cast<void>(
        sh4_physical_range(candidate.source_start, candidate.size));
    const auto runtime =
        sh4_physical_range(candidate.runtime_start, candidate.size);
    if (!runtime.has_value())
        return;
    for (const auto& segment : segments) {
        const auto physical =
            sh4_physical_range(segment.virtual_address,
                               segment.memory_size);
        if (physical.has_value() &&
            ranges_overlap(
                runtime->begin, runtime->end, physical->begin, physical->end))
            throw std::invalid_argument(
                "Runtime-Adressalias ueberschattet ein vorhandenes "
                "SH-4-Direktsegment.");
    }
    for (const auto& alias : aliases) {
        const auto other =
            sh4_physical_range(alias.runtime_start, alias.size);
        if (other.has_value() &&
            ranges_overlap(
                runtime->begin, runtime->end, other->begin, other->end))
            throw std::invalid_argument(
                "Runtime-Adressaliase besitzen mehrdeutige physische "
                "Bereiche.");
    }
}

void require_sh4_unique_layout(
    const std::span<const ImageSegment> segments,
    const std::span<const ImageAddressAlias> aliases) {
    for (std::size_t index = 0u; index < segments.size(); ++index)
        require_sh4_unique_segment(
            segments[index], segments.subspan(0u, index), aliases);
    for (std::size_t index = 0u; index < aliases.size(); ++index)
        require_sh4_unique_alias(
            aliases[index], segments, aliases.subspan(0u, index));
}

} // namespace

std::uint64_t ImageSegment::end_address() const noexcept {
    return static_cast<std::uint64_t>(virtual_address) + memory_size;
}

bool ImageSegment::contains(const std::uint32_t address, const std::size_t width) const noexcept {
    if (width == 0u) {
        return false;
    }
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = begin + static_cast<std::uint64_t>(width);
    return begin >= virtual_address && end >= begin && end <= end_address();
}

std::optional<std::size_t> ImageSegment::byte_offset(const std::uint32_t address) const noexcept {
    if (!contains(address)) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::uint64_t>(address) - virtual_address;
    if (offset >= bytes.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(offset);
}

std::optional<std::uint64_t>
ImageSegment::source_byte_offset(const std::uint32_t address) const noexcept {
    if (!contains(address)) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::uint64_t>(address) - virtual_address;
    const auto source_size =
        latent_source_size == 0u ? static_cast<std::uint64_t>(bytes.size())
                                 : latent_source_size;
    if (offset >= source_size) {
        return std::nullopt;
    }
    return offset;
}

ExecutableImage::ExecutableImage(std::filesystem::path source_path)
    : source_path_(std::move(source_path)),
      analysis_instance_identity_(
          allocate_executable_image_identity()) {}

ExecutableImage::ExecutableImage(
    const ExecutableImage& other)
    : source_path_(other.source_path_),
      segments_(other.segments_),
      entry_points_(other.entry_points_),
      symbols_(other.symbols_),
      relocations_(other.relocations_),
      address_aliases_(other.address_aliases_),
      guest_call_abi_(other.guest_call_abi_),
      initial_snapshot_policy_(other.initial_snapshot_policy_),
      initial_snapshot_entry_(other.initial_snapshot_entry_),
      address_model_(other.address_model_),
      analysis_instance_identity_(
          allocate_executable_image_identity()) {}

ExecutableImage&
ExecutableImage::operator=(const ExecutableImage& other) {
    if (this == &other) return *this;
    ExecutableImage replacement(other);
    source_path_.swap(replacement.source_path_);
    segments_.swap(replacement.segments_);
    entry_points_.swap(replacement.entry_points_);
    symbols_.swap(replacement.symbols_);
    relocations_.swap(replacement.relocations_);
    address_aliases_.swap(replacement.address_aliases_);
    std::swap(guest_call_abi_,
              replacement.guest_call_abi_);
    std::swap(initial_snapshot_policy_,
              replacement.initial_snapshot_policy_);
    std::swap(initial_snapshot_entry_,
              replacement.initial_snapshot_entry_);
    std::swap(address_model_, replacement.address_model_);
    mark_analysis_mutation();
    return *this;
}

ExecutableImage::ExecutableImage(
    ExecutableImage&& other) noexcept
    : source_path_(std::move(other.source_path_)),
      segments_(std::move(other.segments_)),
      entry_points_(std::move(other.entry_points_)),
      symbols_(std::move(other.symbols_)),
      relocations_(std::move(other.relocations_)),
      address_aliases_(std::move(other.address_aliases_)),
      guest_call_abi_(other.guest_call_abi_),
      initial_snapshot_policy_(other.initial_snapshot_policy_),
      initial_snapshot_entry_(other.initial_snapshot_entry_),
      address_model_(other.address_model_),
      analysis_instance_identity_(
          allocate_executable_image_identity()) {
    other.mark_analysis_mutation();
}

ExecutableImage&
ExecutableImage::operator=(ExecutableImage&& other) noexcept {
    if (this == &other) return *this;
    source_path_ = std::move(other.source_path_);
    segments_ = std::move(other.segments_);
    entry_points_ = std::move(other.entry_points_);
    symbols_ = std::move(other.symbols_);
    relocations_ = std::move(other.relocations_);
    address_aliases_ = std::move(other.address_aliases_);
    guest_call_abi_ = other.guest_call_abi_;
    initial_snapshot_policy_ = other.initial_snapshot_policy_;
    initial_snapshot_entry_ = other.initial_snapshot_entry_;
    address_model_ = other.address_model_;
    mark_analysis_mutation();
    other.mark_analysis_mutation();
    return *this;
}

void ExecutableImage::mark_analysis_mutation() noexcept {
    if (analysis_revision_ ==
        std::numeric_limits<std::uint64_t>::max()) {
        analysis_instance_identity_ =
            allocate_executable_image_identity();
        analysis_revision_ = 0u;
        return;
    }
    ++analysis_revision_;
}

void ExecutableImage::add_segment(ImageSegment segment) {
    if (segment.name.empty()) {
        throw std::invalid_argument("Ein Image-Segment braucht einen Namen.");
    }
    if (segment.memory_size == 0u) {
        throw std::invalid_argument("Ein Image-Segment darf nicht leer sein.");
    }
    if (segment.memory_size < segment.bytes.size()) {
        throw std::invalid_argument(
            "Die Speichergroesse eines Segments ist kleiner als seine Dateidaten.");
    }
    if (segment.latent_source_size != 0u &&
        (segment.latent_source_size < segment.bytes.size() ||
         segment.latent_source_size > segment.memory_size)) {
        throw std::invalid_argument(
            "Die latente Quelldatengroesse eines Segments ist ungueltig.");
    }
    if (segment.end_address() > kAddressSpaceSize) {
        throw std::out_of_range("Ein Image-Segment ueberschreitet den 32-Bit-Adressraum.");
    }
    const auto source_size =
        segment.latent_source_size == 0u
            ? static_cast<std::uint64_t>(segment.bytes.size())
            : segment.latent_source_size;
    if (segment.file_offset > std::numeric_limits<std::uint64_t>::max() - source_size) {
        throw std::out_of_range("Dateioffset und Segmentgroesse laufen ueber.");
    }

    for (const auto& existing : segments_) {
        if (ranges_overlap(segment.virtual_address,
                           segment.end_address(),
                           existing.virtual_address,
                           existing.end_address())) {
            throw std::invalid_argument("Image-Segmente ueberlappen sich: " + existing.name +
                                        " und " + segment.name + ".");
        }
    }
    for (const auto& alias : address_aliases_) {
        if (ranges_overlap(segment.virtual_address,
                           segment.end_address(),
                           alias.runtime_start,
                           static_cast<std::uint64_t>(alias.runtime_start) + alias.size)) {
            throw std::invalid_argument(
                "Ein Image-Segment ueberlappt einen Runtime-Adressalias.");
        }
    }
    if (address_model_ == ImageAddressModel::Sh4DirectMapped)
        require_sh4_unique_segment(
            segment, segments_, address_aliases_);

    segments_.push_back(std::move(segment));
    std::sort(segments_.begin(),
              segments_.end(),
              [](const ImageSegment& first, const ImageSegment& second) {
                  return first.virtual_address < second.virtual_address;
              });
    mark_analysis_mutation();
}

void ExecutableImage::add_entry_point(const std::uint32_t address) {
    if (std::find(entry_points_.begin(), entry_points_.end(), address) != entry_points_.end()) {
        return;
    }
    entry_points_.push_back(address);
    std::sort(entry_points_.begin(), entry_points_.end());
    mark_analysis_mutation();
}

void ExecutableImage::replace_entry_points(
    const std::span<const std::uint32_t> addresses) {
    std::vector<std::uint32_t> replacement(addresses.begin(), addresses.end());
    std::sort(replacement.begin(), replacement.end());
    replacement.erase(
        std::unique(replacement.begin(), replacement.end()), replacement.end());
    if (replacement == entry_points_) return;
    entry_points_ = std::move(replacement);
    mark_analysis_mutation();
}

void ExecutableImage::add_symbol(ImageSymbol symbol) {
    if (symbol.name.empty()) {
        throw std::invalid_argument("Ein Image-Symbol braucht einen Namen.");
    }
    if (find_symbol(symbol.name) != nullptr) {
        throw std::invalid_argument("Doppelter Symbolname: " + symbol.name + ".");
    }
    symbols_.push_back(std::move(symbol));
    std::sort(
        symbols_.begin(), symbols_.end(), [](const ImageSymbol& first, const ImageSymbol& second) {
            if (first.address != second.address) {
                return first.address < second.address;
            }
            return first.name < second.name;
        });
    mark_analysis_mutation();
}

void ExecutableImage::add_relocation(ImageRelocation relocation) {
    relocations_.push_back(std::move(relocation));
    std::sort(relocations_.begin(),
              relocations_.end(),
              [](const ImageRelocation& first, const ImageRelocation& second) {
                  if (first.address != second.address) {
                      return first.address < second.address;
                  }
                  return first.raw_type < second.raw_type;
              });
    mark_analysis_mutation();
}

void ExecutableImage::add_address_alias(ImageAddressAlias alias) {
    if (alias.size == 0u) {
        throw std::invalid_argument("Ein Image-Adressalias darf nicht leer sein.");
    }
    const auto source_end = static_cast<std::uint64_t>(alias.source_start) + alias.size;
    const auto runtime_end = static_cast<std::uint64_t>(alias.runtime_start) + alias.size;
    if (source_end > kAddressSpaceSize || runtime_end > kAddressSpaceSize) {
        throw std::out_of_range(
            "Ein Image-Adressalias ueberschreitet den 32-Bit-Adressraum.");
    }

    const auto* source_segment = find_segment(alias.source_start);
    if (source_segment == nullptr ||
        source_end > static_cast<std::uint64_t>(source_segment->virtual_address) +
                         source_segment->bytes.size()) {
        throw std::invalid_argument(
            "Der Quellbereich eines Image-Adressalias ist nicht vollstaendig durch "
            "committete Segmentdaten gedeckt.");
    }

    for (const auto& segment : segments_) {
        if (ranges_overlap(alias.runtime_start,
                           runtime_end,
                           segment.virtual_address,
                           segment.end_address())) {
            throw std::invalid_argument(
                "Der Runtimebereich eines Image-Adressalias ueberlappt ein Image-Segment.");
        }
    }
    for (const auto& existing : address_aliases_) {
        const auto existing_source_end =
            static_cast<std::uint64_t>(existing.source_start) + existing.size;
        const auto existing_runtime_end =
            static_cast<std::uint64_t>(existing.runtime_start) + existing.size;
        if (ranges_overlap(alias.source_start,
                           source_end,
                           existing.source_start,
                           existing_source_end)) {
            throw std::invalid_argument(
                "Quellbereiche von Image-Adressaliasen duerfen sich nicht ueberlappen.");
        }
        if (ranges_overlap(alias.runtime_start,
                           runtime_end,
                           existing.runtime_start,
                           existing_runtime_end)) {
            throw std::invalid_argument(
                "Runtimebereiche von Image-Adressaliasen duerfen sich nicht ueberlappen.");
        }
    }
    if (address_model_ == ImageAddressModel::Sh4DirectMapped)
        require_sh4_unique_alias(
            alias, segments_, address_aliases_);

    address_aliases_.push_back(alias);
    std::sort(address_aliases_.begin(),
              address_aliases_.end(),
              [](const ImageAddressAlias& first, const ImageAddressAlias& second) {
                  if (first.runtime_start != second.runtime_start) {
                      return first.runtime_start < second.runtime_start;
                  }
                  return first.source_start < second.source_start;
              });
    mark_analysis_mutation();
}

void ExecutableImage::set_guest_call_abi(const GuestCallAbi abi) noexcept {
    if (guest_call_abi_ == abi) return;
    guest_call_abi_ = abi;
    mark_analysis_mutation();
}

void ExecutableImage::set_initial_snapshot_policy(const InitialSnapshotPolicy policy) noexcept {
    if (initial_snapshot_policy_ == policy) return;
    initial_snapshot_policy_ = policy;
    mark_analysis_mutation();
}

void ExecutableImage::set_initial_snapshot_entry(const std::uint32_t address) noexcept {
    if (initial_snapshot_entry_ == address) return;
    initial_snapshot_entry_ = address;
    mark_analysis_mutation();
}

void ExecutableImage::set_address_model(const ImageAddressModel model) {
    switch (model) {
    case ImageAddressModel::Exact:
        break;
    case ImageAddressModel::Sh4DirectMapped:
        require_sh4_unique_layout(segments_, address_aliases_);
        break;
    default:
        throw std::invalid_argument("Unbekanntes Executable-Image-Adressmodell.");
    }
    if (address_model_ == model) return;
    address_model_ = model;
    mark_analysis_mutation();
}

const std::filesystem::path& ExecutableImage::source_path() const noexcept {
    return source_path_;
}

std::span<const ImageSegment> ExecutableImage::segments() const noexcept {
    return segments_;
}

std::span<const std::uint32_t> ExecutableImage::entry_points() const noexcept {
    return entry_points_;
}

std::span<const ImageSymbol> ExecutableImage::symbols() const noexcept {
    return symbols_;
}

std::span<const ImageRelocation> ExecutableImage::relocations() const noexcept {
    return relocations_;
}

std::span<const ImageAddressAlias> ExecutableImage::address_aliases() const noexcept {
    return address_aliases_;
}

GuestCallAbi ExecutableImage::guest_call_abi() const noexcept {
    return guest_call_abi_;
}

InitialSnapshotPolicy ExecutableImage::initial_snapshot_policy() const noexcept {
    return initial_snapshot_policy_;
}

std::optional<std::uint32_t> ExecutableImage::initial_snapshot_entry() const noexcept {
    return initial_snapshot_entry_;
}

ImageAddressModel ExecutableImage::address_model() const noexcept {
    return address_model_;
}

std::uint64_t
ExecutableImage::analysis_instance_identity() const noexcept {
    return analysis_instance_identity_;
}

std::uint64_t
ExecutableImage::analysis_revision() const noexcept {
    return analysis_revision_;
}

std::optional<std::uint32_t>
ExecutableImage::resolve_segment_address(const std::uint32_t address,
                                         const std::size_t width) const noexcept {
    if (find_segment(address, width) != nullptr) return address;
    if (width == 0u) return std::nullopt;
    const auto address_end =
        static_cast<std::uint64_t>(address) + static_cast<std::uint64_t>(width);
    if (address_end <= kAddressSpaceSize) {
        for (const auto& alias : address_aliases_) {
            const auto runtime_end =
                static_cast<std::uint64_t>(alias.runtime_start) + alias.size;
            if (address < alias.runtime_start || address_end > runtime_end) continue;
            const auto offset =
                static_cast<std::uint64_t>(address) - alias.runtime_start;
            const auto source =
                static_cast<std::uint64_t>(alias.source_start) + offset;
            if (source > std::numeric_limits<std::uint32_t>::max()) continue;
            const auto candidate = static_cast<std::uint32_t>(source);
            if (find_segment(candidate, width) != nullptr) return candidate;
        }
    }
    if (address_model_ != ImageAddressModel::Sh4DirectMapped || address >= 0xE0000000u)
        return std::nullopt;
    const auto physical = address & 0x1FFFFFFFu;
    for (const auto& segment : segments_) {
        if (segment.virtual_address >= 0xE0000000u) continue;
        const auto segment_physical = segment.virtual_address & 0x1FFFFFFFu;
        if (physical < segment_physical) continue;
        const auto offset = static_cast<std::uint64_t>(physical) - segment_physical;
        if (offset > std::numeric_limits<std::uint32_t>::max()) continue;
        const auto resolved = static_cast<std::uint64_t>(segment.virtual_address) + offset;
        if (resolved > std::numeric_limits<std::uint32_t>::max()) continue;
        const auto candidate = static_cast<std::uint32_t>(resolved);
        if (segment.contains(candidate, width)) return candidate;
    }
    return std::nullopt;
}

const ImageSymbol* ExecutableImage::find_symbol(const std::string_view name) const noexcept {
    for (const auto& symbol : symbols_) {
        if (symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

const ImageSegment* ExecutableImage::find_segment(const std::uint32_t address,
                                                  const std::size_t width) const noexcept {
    for (const auto& segment : segments_) {
        if (segment.contains(address, width)) {
            return &segment;
        }
    }
    return nullptr;
}

std::uint32_t ExecutableImage::read_u32_le(const std::uint32_t address) const {
    const auto* segment = find_segment(address, 4u);
    if (segment == nullptr) {
        throw std::out_of_range("32-Bit-Lesezugriff liegt ausserhalb der Image-Segmente.");
    }
    const auto offset = segment->byte_offset(address);
    if (!offset.has_value() || segment->bytes.size() < 4u || *offset > segment->bytes.size() - 4u) {
        throw std::out_of_range("32-Bit-Lesezugriff liegt ausserhalb der committed Segmentdaten.");
    }
    return static_cast<std::uint32_t>(segment->bytes[*offset]) |
           (static_cast<std::uint32_t>(segment->bytes[*offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(segment->bytes[*offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(segment->bytes[*offset + 3u]) << 24u);
}

void ExecutableImage::write_bytes(
    const std::uint32_t address,
    const std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return;
    for (auto& segment : segments_) {
        if (!segment.contains(address, bytes.size())) continue;
        const auto offset = segment.byte_offset(address);
        if (!offset.has_value() || *offset > segment.bytes.size() ||
            bytes.size() > segment.bytes.size() - *offset) {
            throw std::out_of_range(
                "Byte-Schreibzugriff liegt ausserhalb der committed Segmentdaten.");
        }
        if (!std::equal(bytes.begin(), bytes.end(),
                        segment.bytes.begin() +
                            static_cast<std::ptrdiff_t>(*offset))) {
            std::copy(bytes.begin(), bytes.end(),
                      segment.bytes.begin() +
                          static_cast<std::ptrdiff_t>(*offset));
            mark_analysis_mutation();
        }
        return;
    }
    throw std::out_of_range(
        "Byte-Schreibzugriff liegt ausserhalb der Image-Segmente.");
}

void ExecutableImage::write_u32_le(const std::uint32_t address, const std::uint32_t value) {
    for (auto& segment : segments_) {
        if (!segment.contains(address, 4u)) {
            continue;
        }
        const auto offset = segment.byte_offset(address);
        if (!offset.has_value() || segment.bytes.size() < 4u ||
            *offset > segment.bytes.size() - 4u) {
            throw std::out_of_range(
                "32-Bit-Schreibzugriff liegt ausserhalb der committed Segmentdaten.");
        }
        for (std::size_t index = 0; index < 4u; ++index) {
            segment.bytes[*offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
        }
        mark_analysis_mutation();
        return;
    }
    throw std::out_of_range("32-Bit-Schreibzugriff liegt ausserhalb der Image-Segmente.");
}

const char* segment_kind_name(const SegmentKind kind) noexcept {
    switch (kind) {
    case SegmentKind::Unknown:
        return "unknown";
    case SegmentKind::Code:
        return "code";
    case SegmentKind::Data:
        return "data";
    case SegmentKind::Mixed:
        return "mixed";
    }
    return "unknown";
}

const char* image_source_kind_name(const ImageSourceKind kind) noexcept {
    switch (kind) {
    case ImageSourceKind::Unknown:
        return "unknown";
    case ImageSourceKind::RawBinary:
        return "raw_binary";
    case ImageSourceKind::ElfLoadSegment:
        return "elf_load_segment";
    case ImageSourceKind::DiscBootFile:
        return "disc_boot_file";
    case ImageSourceKind::DiscModule:
        return "disc_module";
    case ImageSourceKind::RuntimeMemory:
        return "runtime_memory";
    }
    return "unknown";
}

const char* image_load_phase_name(const ImageLoadPhase phase) noexcept {
    switch (phase) {
    case ImageLoadPhase::Initial:
        return "initial";
    case ImageLoadPhase::RuntimeModule:
        return "runtime_module";
    case ImageLoadPhase::Overlay:
        return "overlay";
    }
    return "initial";
}

const char* symbol_kind_name(const SymbolKind kind) noexcept {
    switch (kind) {
    case SymbolKind::Unknown:
        return "unknown";
    case SymbolKind::Function:
        return "function";
    case SymbolKind::Object:
        return "object";
    }
    return "unknown";
}

const char* relocation_kind_name(const RelocationKind kind) noexcept {
    switch (kind) {
    case RelocationKind::None:
        return "none";
    case RelocationKind::Absolute32:
        return "absolute32";
    case RelocationKind::PcRelative32:
        return "pc-relative32";
    case RelocationKind::Unsupported:
        return "unsupported";
    }
    return "unsupported";
}

} // namespace katana::io
