#include "katana/runtime/disc_load_transaction.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::uint64_t guest_address_space_size = 0x1'0000'0000ull;
constexpr std::size_t maximum_aot_descriptors = 1024u;
constexpr std::size_t maximum_aot_coverage_intervals = 256u;
constexpr std::uint64_t maximum_aot_descriptor_bytes = 64u * 1024u * 1024u;

std::vector<std::uint8_t> read_previous_bytes(const Memory& memory,
                                              const DiscLoadRequest& request,
                                              const std::span<const DiscLoadCommittedRange> ranges) {
    std::vector<std::uint8_t> result(request.bytes.size());
    for (const auto& range : ranges) {
        for (std::uint32_t offset = 0u; offset < range.size; ++offset) {
            result[range.source_offset + offset] =
                memory.read_u8(range.target_physical_address + offset);
        }
    }
    return result;
}

void validate_request(const DiscLoadRequest& request) {
    if (request.sequence == 0u || request.bytes.empty() || request.content_identity.empty() ||
        !request.guest_translation_validated)
        throw std::invalid_argument("Disc-Ladetransaktion besitzt keinen vollstaendigen Vertrag.");
    if (request.write_source != CodeWriteSource::Copy &&
        request.write_source != CodeWriteSource::Dma)
        throw std::invalid_argument("Disc-Ladetransaktion besitzt eine ungueltige Writequelle.");
    if (request.bytes.size() >
        guest_address_space_size - static_cast<std::uint64_t>(request.physical_destination))
        throw std::out_of_range("Disc-Ladetransaktion ueberschreitet den Gastadressraum.");
    if (request.bytes.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::out_of_range("Disc-Ladetransaktion ist nicht als 32-Bit-Bereich darstellbar.");
    if (request.source_range.known) {
        if (request.source_range.byte_count != request.bytes.size() ||
            request.source_range.byte_count >
                std::numeric_limits<std::uint64_t>::max() -
                    request.source_range.byte_offset)
            throw std::invalid_argument(
                "Disc-Ladetransaktion besitzt einen ungueltigen logischen Quellbereich.");
    } else if (request.source_range.byte_offset != 0u ||
               request.source_range.byte_count != 0u) {
        throw std::invalid_argument(
            "Unbekannter Disc-Quellbereich darf keine geratenen Grenzen tragen.");
    }
    if (request.byte_identity != disc_load_byte_identity(request.bytes))
        throw std::invalid_argument("Disc-Ladetransaktion besitzt eine falsche Byteidentitaet.");
}

bool opaque_descriptor_id(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 128u) return false;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_';
    });
}

bool overlaps(const std::uint64_t left_begin,
              const std::uint64_t left_end,
              const std::uint64_t right_begin,
              const std::uint64_t right_end) noexcept {
    return left_begin < right_end && right_begin < left_end;
}

template <typename Interval>
void add_coverage(std::vector<Interval>& coverage,
                  const std::uint32_t begin,
                  const std::uint32_t end) {
    if (begin >= end) return;
    coverage.push_back({begin, end});
    std::sort(coverage.begin(), coverage.end(), [](const auto& left, const auto& right) {
        return left.begin < right.begin ||
               (left.begin == right.begin && left.end < right.end);
    });
    std::size_t output = 0u;
    for (const auto interval : coverage) {
        if (output == 0u || coverage[output - 1u].end < interval.begin) {
            coverage[output++] = interval;
        } else {
            coverage[output - 1u].end =
                std::max(coverage[output - 1u].end, interval.end);
        }
    }
    coverage.resize(output);
}

template <typename Interval>
bool coverage_contains(const std::span<const Interval> coverage,
                       const std::uint32_t begin,
                       const std::uint32_t end) noexcept {
    return std::any_of(coverage.begin(), coverage.end(), [&](const auto interval) {
        return interval.begin <= begin && interval.end >= end;
    });
}

} // namespace

const char* disc_load_route_name(const DiscLoadRoute route) noexcept {
    switch (route) {
    case DiscLoadRoute::BiosPio:
        return "bios-pio";
    case DiscLoadRoute::BiosDma:
        return "bios-dma";
    case DiscLoadRoute::BiosPioStream:
        return "bios-pio-stream";
    case DiscLoadRoute::BiosDmaStream:
        return "bios-dma-stream";
    case DiscLoadRoute::TaskfileDma:
        return "taskfile-dma";
    case DiscLoadRoute::SystemBootstrap:
        return "system-bootstrap";
    }
    return "unknown";
}

std::uint64_t claim_disc_load_sequence(std::uint64_t& next_sequence) {
    if (next_sequence == 0u ||
        next_sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Disc-Load-Sequenz ist ausgeschoepft.");
    const auto claimed = next_sequence;
    ++next_sequence;
    return claimed;
}

std::string disc_load_byte_identity(const std::span<const std::uint8_t> bytes) {
    return "sha256:" + katana::io::sha256_bytes(std::string_view(
                           reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string disc_load_source_identity(const std::string_view content_identity,
                                      const std::string_view byte_identity) {
    std::string material = "katana-disc-load-source-v1;";
    material += std::to_string(content_identity.size());
    material.push_back(':');
    material.append(content_identity);
    material.push_back(';');
    material += std::to_string(byte_identity.size());
    material.push_back(':');
    material.append(byte_identity);
    return "disc-load-v1:" + katana::io::sha256_bytes(material);
}

void validate_disc_load_commit(const DiscLoadRequest& request, const DiscLoadCommit& commit) {
    if (commit.sequence != request.sequence || commit.route != request.route ||
        commit.guest_destination != request.guest_destination ||
        commit.physical_destination != request.physical_destination ||
        commit.write_source != request.write_source ||
        commit.content_identity != request.content_identity ||
        commit.byte_identity != request.byte_identity ||
        commit.source_range != request.source_range ||
        commit.committed_bytes != request.bytes.size() || commit.ranges.empty())
        throw std::logic_error("Disc-Ladetransaktion lieferte einen fremden Commitbeleg.");
    std::uint64_t cursor = 0u;
    for (const auto& range : commit.ranges) {
        if (cursor > request.bytes.size() || range.size == 0u ||
            range.source_offset != cursor ||
            range.size > request.bytes.size() - static_cast<std::size_t>(cursor) ||
            range.byte_identity != disc_load_byte_identity(
                                       request.bytes.subspan(range.source_offset, range.size)) ||
            range.source_range_known != request.source_range.known ||
            (range.source_range_known &&
             range.source_byte_offset != request.source_range.byte_offset + cursor) ||
            (!range.source_range_known && range.source_byte_offset != 0u) ||
            (range.module_activated && !range.executable_backing) ||
            (range.module_activated && range.exact_module_retained))
            throw std::logic_error("Disc-Ladetransaktion besitzt ungueltige Commitbereiche.");
        cursor += range.size;
    }
    if (cursor != request.bytes.size())
        throw std::logic_error("Disc-Ladetransaktion hat nicht alle Bytes committed.");
}

ExecutableDiscLoadTransactionCoordinator::ExecutableDiscLoadTransactionCoordinator(
    Memory& memory,
    ExecutableModuleCatalog& modules,
    RuntimeBlockTable& blocks,
    ExecutableCodeTracker& tracker,
    DiscLoadRangeResolver range_resolver,
    DiscLoadAdmissionObserver admission_observer)
    : memory_(memory), modules_(modules), blocks_(blocks), tracker_(tracker),
      range_resolver_(std::move(range_resolver)),
      admission_observer_(std::move(admission_observer)) {
    if (!range_resolver_)
        throw std::invalid_argument("Disc-Ladetransaktion braucht einen Zielbereichsresolver.");
}

void ExecutableDiscLoadTransactionCoordinator::set_aot_module_descriptors(
    const std::span<const DiscLoadAotModuleDescriptor> descriptors) {
    if (aot_descriptors_configured_ || transaction_started_ || pending_write_.active)
        throw std::logic_error(
            "Disc-AOT-Deskriptoren muessen genau einmal vor dem ersten Load gesetzt werden.");
    if (descriptors.size() > maximum_aot_descriptors)
        throw std::out_of_range("Disc-AOT-Deskriptorregistry ueberschreitet ihr Limit.");

    std::vector<AotAssembly> assemblies;
    assemblies.reserve(descriptors.size());
    std::uint64_t total_bytes = 0u;
    for (const auto& descriptor : descriptors) {
        if (!opaque_descriptor_id(descriptor.opaque_id) ||
            descriptor.content_identity.empty() || descriptor.byte_identity.empty() ||
            descriptor.byte_size == 0u ||
            descriptor.byte_size > maximum_aot_descriptor_bytes ||
            descriptor.byte_size >
                std::numeric_limits<std::uint64_t>::max() -
                    descriptor.source_byte_offset)
            throw std::invalid_argument("Disc-AOT-Deskriptor besitzt ungueltige Metadaten.");
        if (total_bytes > 256u * 1024u * 1024u - descriptor.byte_size)
            throw std::out_of_range("Disc-AOT-Deskriptorbytes ueberschreiten ihr Gesamtlimit.");
        total_bytes += descriptor.byte_size;
        for (const auto& existing : assemblies) {
            if (existing.descriptor.opaque_id == descriptor.opaque_id)
                throw std::invalid_argument("Disc-AOT-Deskriptor-ID ist nicht eindeutig.");
            if (existing.descriptor.content_identity == descriptor.content_identity &&
                overlaps(existing.descriptor.source_byte_offset,
                         existing.descriptor.source_byte_offset +
                             existing.descriptor.byte_size,
                         descriptor.source_byte_offset,
                         descriptor.source_byte_offset + descriptor.byte_size))
                throw std::invalid_argument(
                    "Disc-AOT-Deskriptoren ueberlappen im selben Contentvertrag.");
        }
        AotAssembly assembly;
        assembly.descriptor = descriptor;
        assembly.coverage.reserve(maximum_aot_coverage_intervals);
        assemblies.push_back(std::move(assembly));
    }
    aot_assemblies_ = std::move(assemblies);
    aot_descriptors_configured_ = true;
}

DiscLoadCommit
ExecutableDiscLoadTransactionCoordinator::execute(const DiscLoadRequest& request) {
    validate_request(request);
    transaction_started_ = true;
    if (pending_write_.active)
        throw std::logic_error("Verschachtelte Disc-Ladetransaktion ist nicht erlaubt.");

    auto ranges = range_resolver_(request.physical_destination, request.bytes.size());
    if (ranges.empty())
        throw std::out_of_range("Disc-Ladetransaktion besitzt keinen committed Zielbereich.");
    std::uint64_t cursor = 0u;
    bool any_bytes_changed = false;
    std::vector<bool> changed;
    std::vector<bool> retained;
    std::vector<std::vector<std::uint8_t>> changed_masks;
    changed.reserve(ranges.size());
    retained.reserve(ranges.size());
    changed_masks.reserve(ranges.size());
    std::vector<ExecutableModule> pending_modules;
    pending_modules.reserve(ranges.size());
    std::vector<std::pair<std::uint32_t, std::size_t>> catalog_invalidations;
    catalog_invalidations.reserve(ranges.size() + aot_assemblies_.size());

    for (std::size_t index = 0u; index < ranges.size(); ++index) {
        auto& range = ranges[index];
        if (cursor > request.bytes.size() || range.size == 0u ||
            range.source_offset != cursor ||
            range.size > request.bytes.size() - static_cast<std::size_t>(cursor) ||
            static_cast<std::uint64_t>(range.target_physical_address) + range.size >
                guest_address_space_size ||
            static_cast<std::uint64_t>(range.backing_physical_address) + range.size >
                guest_address_space_size ||
            !memory_.is_writable_linear_range(range.target_physical_address, range.size))
            throw std::out_of_range(
                "Disc-Ladetransaktion zielt nicht vollstaendig auf lineares schreibbares RAM.");
        const auto expected_target = canonical_physical_address(
            request.physical_destination + static_cast<std::uint32_t>(cursor));
        if (range.target_physical_address != expected_target ||
            canonical_physical_address(range.backing_physical_address) !=
                range.backing_physical_address)
            throw std::invalid_argument(
                "Disc-Ladetransaktion besitzt eine inkonsistente Aliasabbildung.");
        range.byte_identity =
            disc_load_byte_identity(request.bytes.subspan(range.source_offset, range.size));
        range.source_range_known = request.source_range.known;
        range.source_byte_offset =
            request.source_range.known ? request.source_range.byte_offset + cursor : 0u;

        std::vector<std::uint8_t> changed_mask(range.size, 0u);
        bool physical_bytes_changed = false;
        for (std::uint32_t offset = 0u; offset < range.size; ++offset) {
            const auto differs =
                memory_.read_u8(range.target_physical_address + offset) !=
                request.bytes[range.source_offset + offset];
            changed_mask[offset] = static_cast<std::uint8_t>(differs);
            physical_bytes_changed = physical_bytes_changed || differs;
        }
        any_bytes_changed = any_bytes_changed || physical_bytes_changed;
        changed.push_back(physical_bytes_changed);
        changed_masks.push_back(std::move(changed_mask));
        retained.push_back(false);
        cursor += range.size;
    }
    if (cursor != request.bytes.size())
        throw std::out_of_range("Disc-Ladetransaktion deckt den Payload nicht vollstaendig ab.");

    struct RegisteredSegment {
        std::uint32_t begin = 0u;
        std::uint32_t end = 0u;
        std::size_t assembly_index = 0u;
        bool exact_full_module_retained = false;
        bool full_module_activated = false;
    };
    struct PreparedAotUpdate {
        std::size_t assembly_index = 0u;
        std::optional<std::uint32_t> target_base;
        std::vector<AotCoverageInterval> coverage;
    };
    std::vector<std::vector<RegisteredSegment>> registered_segments(ranges.size());
    std::vector<PreparedAotUpdate> aot_updates;
    aot_updates.reserve(aot_assemblies_.size());

    const auto request_source_begin = request.source_range.byte_offset;
    const auto request_source_end =
        request.source_range.known
            ? request.source_range.byte_offset + request.source_range.byte_count
            : 0u;
    for (std::size_t assembly_index = 0u; assembly_index < aot_assemblies_.size();
         ++assembly_index) {
        const auto& current = aot_assemblies_[assembly_index];
        const auto& descriptor = current.descriptor;
        const auto descriptor_begin = descriptor.source_byte_offset;
        const auto descriptor_end = descriptor_begin + descriptor.byte_size;
        auto next_target = current.target_base;
        auto next_coverage = current.coverage;
        bool affected = false;
        bool matching_source_segment = false;

        if (request.source_range.known &&
            overlaps(request_source_begin,
                     request_source_end,
                     descriptor_begin,
                     descriptor_end)) {
            affected = true;
            if (request.content_identity != descriptor.content_identity) {
                next_target.reset();
                next_coverage.clear();
            } else {
                for (std::size_t range_index = 0u; range_index < ranges.size();
                     ++range_index) {
                    const auto& range = ranges[range_index];
                    if (!range.executable_backing) continue;
                    const auto range_source_begin =
                        request_source_begin + range.source_offset;
                    const auto range_source_end = range_source_begin + range.size;
                    const auto intersection_begin =
                        std::max(range_source_begin, descriptor_begin);
                    const auto intersection_end =
                        std::min(range_source_end, descriptor_end);
                    if (intersection_begin >= intersection_end) continue;
                    matching_source_segment = true;
                    const auto range_relative_begin = static_cast<std::uint32_t>(
                        intersection_begin - range_source_begin);
                    const auto range_relative_end = static_cast<std::uint32_t>(
                        intersection_end - range_source_begin);
                    registered_segments[range_index].push_back(
                        {range_relative_begin,
                         range_relative_end,
                         assembly_index,
                         false,
                         false});

                    const auto file_relative_begin =
                        static_cast<std::uint32_t>(intersection_begin - descriptor_begin);
                    const auto backing_intersection =
                        static_cast<std::uint64_t>(range.backing_physical_address) +
                        range_relative_begin;
                    if (backing_intersection < file_relative_begin) {
                        next_target.reset();
                        next_coverage.clear();
                        continue;
                    }
                    const auto candidate_base = backing_intersection - file_relative_begin;
                    if (candidate_base > std::numeric_limits<std::uint32_t>::max() ||
                        candidate_base + descriptor.byte_size > guest_address_space_size) {
                        next_target.reset();
                        next_coverage.clear();
                        continue;
                    }
                    const auto candidate = static_cast<std::uint32_t>(candidate_base);
                    if (next_target && *next_target != candidate) {
                        next_coverage.clear();
                        next_target.reset();
                    }
                    if (!next_target) next_target = candidate;

                    const auto changed_overlap = std::any_of(
                        changed_masks[range_index].begin() + range_relative_begin,
                        changed_masks[range_index].begin() + range_relative_end,
                        [](const auto value) { return value != 0u; });
                    const auto file_relative_end =
                        static_cast<std::uint32_t>(intersection_end - descriptor_begin);
                    if (changed_overlap &&
                        coverage_contains(std::span<const AotCoverageInterval>(next_coverage),
                                          file_relative_begin,
                                          file_relative_end))
                        next_coverage.clear();
                    add_coverage(next_coverage, file_relative_begin, file_relative_end);
                    if (next_coverage.size() > maximum_aot_coverage_intervals) {
                        next_target.reset();
                        next_coverage.clear();
                    }
                }
            }
        }

        if (next_target) {
            const auto target_begin = static_cast<std::uint64_t>(*next_target);
            const auto target_end = target_begin + descriptor.byte_size;
            for (std::size_t range_index = 0u; range_index < ranges.size(); ++range_index) {
                const auto& range = ranges[range_index];
                const auto range_begin =
                    static_cast<std::uint64_t>(range.backing_physical_address);
                const auto range_end = range_begin + range.size;
                const auto corresponds = std::any_of(
                    registered_segments[range_index].begin(),
                    registered_segments[range_index].end(),
                    [&](const auto& segment) {
                        return segment.assembly_index == assembly_index;
                    });
                if (overlaps(target_begin, target_end, range_begin, range_end) &&
                    !corresponds) {
                    affected = true;
                    next_target.reset();
                    next_coverage.clear();
                    break;
                }
            }
        }

        const auto complete =
            matching_source_segment && next_target && next_coverage.size() == 1u &&
            next_coverage.front().begin == 0u &&
            next_coverage.front().end == descriptor.byte_size;
        bool exact_full_module = false;
        bool activated_full_module = false;
        if (complete &&
            memory_.is_writable_linear_range(*next_target, descriptor.byte_size)) {
            std::vector<std::uint8_t> full_bytes(descriptor.byte_size);
            for (std::uint32_t offset = 0u; offset < descriptor.byte_size; ++offset)
                full_bytes[offset] = memory_.read_u8(*next_target + offset);
            for (std::size_t range_index = 0u; range_index < ranges.size(); ++range_index) {
                const auto& range = ranges[range_index];
                const auto range_source_begin =
                    request_source_begin + range.source_offset;
                const auto range_source_end = range_source_begin + range.size;
                const auto intersection_begin =
                    std::max(range_source_begin, descriptor_begin);
                const auto intersection_end = std::min(range_source_end, descriptor_end);
                if (intersection_begin >= intersection_end) continue;
                const auto file_offset =
                    static_cast<std::size_t>(intersection_begin - descriptor_begin);
                const auto request_offset =
                    static_cast<std::size_t>(intersection_begin - request_source_begin);
                std::copy(request.bytes.begin() + static_cast<std::ptrdiff_t>(request_offset),
                          request.bytes.begin() + static_cast<std::ptrdiff_t>(
                                                      request_offset +
                                                      intersection_end -
                                                      intersection_begin),
                          full_bytes.begin() + static_cast<std::ptrdiff_t>(file_offset));
            }
            if (disc_load_byte_identity(full_bytes) != descriptor.byte_identity) {
                next_target.reset();
                next_coverage.clear();
            } else {
                const auto* existing =
                    modules_.resolve(*next_target, descriptor.byte_size);
                exact_full_module =
                    existing != nullptr &&
                    existing->content_identity == descriptor.content_identity &&
                    existing->byte_identity == descriptor.byte_identity &&
                    existing->active_extents ==
                        std::vector<ExecutableModuleActiveExtent>{
                            {0u, descriptor.byte_size}} &&
                    existing->bytes == full_bytes;
                if (!exact_full_module) {
                    ExecutableModule module;
                    module.id = "disc-loaded-aot-" + descriptor.opaque_id + "-" +
                                std::to_string(*next_target);
                    module.content_identity = descriptor.content_identity;
                    module.byte_identity = descriptor.byte_identity;
                    module.source_identity = disc_load_source_identity(
                        module.content_identity, module.byte_identity);
                    module.guest_start = *next_target;
                    module.bytes = std::move(full_bytes);
                    module.active_extents.push_back({0u, descriptor.byte_size});
                    module.kind = ExecutableModuleKind::Overlay;
                    module.executable_permission = true;
                    module.control_transfer_promotion_allowed = false;
                    module.range_roles.push_back(
                        {0u,
                         descriptor.byte_size,
                         ExecutableStorageRole::RuntimeMaterializable});
                    pending_modules.push_back(std::move(module));
                    catalog_invalidations.emplace_back(*next_target,
                                                       descriptor.byte_size);
                    activated_full_module = true;
                }
            }
        }
        for (auto& segments : registered_segments) {
            for (auto& segment : segments) {
                if (segment.assembly_index != assembly_index) continue;
                segment.exact_full_module_retained = exact_full_module;
                segment.full_module_activated = activated_full_module;
            }
        }
        if (affected)
            aot_updates.push_back(
                {assembly_index, next_target, std::move(next_coverage)});
    }

    std::vector<bool> range_module_activated(ranges.size(), false);
    for (std::size_t range_index = 0u; range_index < ranges.size(); ++range_index) {
        const auto& range = ranges[range_index];
        if (!range.executable_backing) continue;
        if ((range.backing_physical_address & 1u) != 0u)
            throw std::invalid_argument(
                "Ausfuehrbarer Disc-Ladebereich ist nicht SH-4-ausgerichtet.");

        std::vector<std::uint32_t> cuts{0u, range.size};
        for (const auto& segment : registered_segments[range_index]) {
            cuts.push_back(segment.begin);
            cuts.push_back(segment.end);
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        bool all_segments_retained = true;
        for (std::size_t cut = 1u; cut < cuts.size(); ++cut) {
            const auto begin = cuts[cut - 1u];
            const auto end = cuts[cut];
            if (begin == end) continue;
            const auto registered = std::find_if(
                registered_segments[range_index].begin(),
                registered_segments[range_index].end(),
                [&](const auto& segment) {
                    return segment.begin <= begin && segment.end >= end;
                });
            if (registered != registered_segments[range_index].end()) {
                range_module_activated[range_index] =
                    range_module_activated[range_index] ||
                    registered->full_module_activated;
                if (registered->exact_full_module_retained) continue;
                all_segments_retained = false;
                catalog_invalidations.emplace_back(
                    range.backing_physical_address + begin, end - begin);
                if (registered->full_module_activated) continue;

                // A partial registered file is visible only as a typed AOT candidate. The
                // product materializer routes it to the native-template binder, whose exact
                // full-file size/hash contract reports MissingAot; no interpreter fallback is
                // authorized by this module.
                const auto segment_size = end - begin;
                const auto source_offset = range.source_offset + begin;
                const auto segment_bytes =
                    request.bytes.subspan(source_offset, segment_size);
                ExecutableModule module;
                module.id = "disc-pending-aot-" +
                            aot_assemblies_[registered->assembly_index]
                                .descriptor.opaque_id +
                            "-" + std::to_string(request.sequence) + "-" +
                            std::to_string(range_index) + "-" +
                            std::to_string(cut - 1u);
                module.content_identity = request.content_identity;
                module.byte_identity = disc_load_byte_identity(segment_bytes);
                module.source_identity = disc_load_source_identity(
                    module.content_identity, module.byte_identity);
                module.guest_start = range.backing_physical_address + begin;
                module.bytes.assign(segment_bytes.begin(), segment_bytes.end());
                module.kind = ExecutableModuleKind::Overlay;
                module.executable_permission = true;
                module.control_transfer_promotion_allowed = false;
                module.range_roles.push_back(
                    {0u, segment_size, ExecutableStorageRole::RuntimeMaterializable});
                pending_modules.push_back(std::move(module));
                range_module_activated[range_index] = true;
                continue;
            }

            const auto segment_size = end - begin;
            const auto source_offset = range.source_offset + begin;
            const auto segment_bytes = request.bytes.subspan(source_offset, segment_size);
            const auto segment_identity = disc_load_byte_identity(segment_bytes);
            const auto segment_address = range.backing_physical_address + begin;
            const auto* existing = modules_.resolve(segment_address, segment_size);
            const auto exact_module =
                existing != nullptr &&
                existing->content_identity == request.content_identity &&
                existing->byte_identity == segment_identity &&
                modules_.validate_bytes_at(memory_,
                                           segment_address,
                                           range.target_physical_address + begin,
                                           segment_size);
            if (exact_module) continue;
            all_segments_retained = false;
            ExecutableModule module;
            module.id = "gdrom-load-" + std::to_string(request.sequence) + "-" +
                        std::to_string(range_index) + "-" + std::to_string(cut - 1u);
            module.content_identity = request.content_identity;
            module.byte_identity = segment_identity;
            module.source_identity =
                disc_load_source_identity(module.content_identity, module.byte_identity);
            module.guest_start = segment_address;
            module.bytes.assign(segment_bytes.begin(), segment_bytes.end());
            module.kind = ExecutableModuleKind::Overlay;
            module.executable_permission = false;
            module.control_transfer_promotion_allowed = true;
            module.range_roles.push_back(
                {0u, segment_size, ExecutableStorageRole::ProvenData});
            pending_modules.push_back(std::move(module));
            catalog_invalidations.emplace_back(segment_address, segment_size);
            range_module_activated[range_index] = true;
        }
        retained[range_index] = all_segments_retained;
    }

    const auto previous_bytes = read_previous_bytes(memory_, request, ranges);
    std::optional<ExecutableModuleCatalog::PreparedDiscLoadCatalog> catalog_plan;
    struct PreparedCodeCommit {
        ExecutableCodeTracker::PreparedDiscLoadWrite tracker;
        RuntimeBlockTable::PreparedDiscLoadInvalidation blocks;
    };
    std::vector<PreparedCodeCommit> code_plans;
    code_plans.reserve(ranges.size());
    try {
        if (!catalog_invalidations.empty())
            catalog_plan.emplace(modules_.prepare_disc_load_catalog(
                std::move(pending_modules), catalog_invalidations));
        for (std::size_t index = 0u; index < ranges.size(); ++index) {
            const auto& range = ranges[index];
            const auto semantic_change =
                changed[index] || (range.executable_backing && !retained[index]);
            if (!semantic_change ||
                !tracker_.tracks_address(range.backing_physical_address, range.size))
                continue;
            auto tracker_plan = tracker_.prepare_disc_load_write(
                range.backing_physical_address, range.size, request.write_source);
            try {
                auto block_plan = blocks_.prepare_disc_load_invalidation(
                    range.backing_physical_address, range.size);
                code_plans.push_back(
                    {std::move(tracker_plan), std::move(block_plan)});
            } catch (...) {
                tracker_.cancel_disc_load_write(tracker_plan);
                throw;
            }
        }
        // The callback is the final fail-capable admission boundary. All transient catalog/index
        // reservations exist, but every one is still exactly cancellable and no guest byte or
        // executable state has been committed.
        if (admission_observer_) admission_observer_(request, ranges);
    } catch (...) {
        for (auto& plan : code_plans) tracker_.cancel_disc_load_write(plan.tracker);
        if (catalog_plan) modules_.cancel_disc_load_catalog(*catalog_plan);
        throw;
    }

    std::size_t committed_ranges = 0u;
    for (; committed_ranges < ranges.size(); ++committed_ranges) {
        const auto& range = ranges[committed_ranges];
        pending_write_ = {
            range.target_physical_address,
            range.size,
            request.write_source,
            true,
        };
        const auto written = memory_.commit_prevalidated_linear_transaction_bytes(
            range.target_physical_address,
            request.bytes.subspan(range.source_offset, range.size),
            changed_masks[committed_ranges],
            request.write_source);
        pending_write_ = {};
        if (written) continue;

        while (committed_ranges != 0u) {
            --committed_ranges;
            const auto& rollback = ranges[committed_ranges];
            pending_write_ = {
                rollback.target_physical_address,
                rollback.size,
                request.write_source,
                true,
            };
            static_cast<void>(memory_.commit_prevalidated_linear_transaction_bytes(
                rollback.target_physical_address,
                std::span<const std::uint8_t>(previous_bytes)
                    .subspan(rollback.source_offset, rollback.size),
                changed_masks[committed_ranges],
                request.write_source));
            pending_write_ = {};
        }
        for (auto& plan : code_plans) tracker_.cancel_disc_load_write(plan.tracker);
        if (catalog_plan) modules_.cancel_disc_load_catalog(*catalog_plan);
        throw std::logic_error(
            "Vorvalidierter linearer Disc-Ladebereich verlor vor dem Commit sein Backing.");
    }

    if (catalog_plan) modules_.commit_disc_load_catalog(std::move(*catalog_plan));
    for (auto& plan : code_plans) {
        tracker_.commit_disc_load_write(std::move(plan.tracker));
        static_cast<void>(
            blocks_.commit_disc_load_invalidation(std::move(plan.blocks)));
    }
    for (auto& update : aot_updates) {
        auto& assembly = aot_assemblies_[update.assembly_index];
        assembly.target_base = update.target_base;
        assembly.coverage.swap(update.coverage);
    }

    bool identity_rebound = false;
    for (std::size_t index = 0u; index < ranges.size(); ++index) {
        auto& range = ranges[index];
        if (!range.executable_backing || retained[index]) {
            range.exact_module_retained = retained[index];
            continue;
        }
        range.module_activated = range_module_activated[index];
        identity_rebound =
            identity_rebound || (range.module_activated && !changed[index]);
    }

    DiscLoadCommit commit;
    commit.sequence = request.sequence;
    commit.route = request.route;
    commit.guest_destination = request.guest_destination;
    commit.physical_destination = request.physical_destination;
    commit.write_source = request.write_source;
    commit.content_identity = request.content_identity;
    commit.byte_identity = request.byte_identity;
    commit.source_range = request.source_range;
    commit.committed_bytes = request.bytes.size();
    commit.ranges = std::move(ranges);
    commit.bytes_changed = any_bytes_changed;
    commit.identity_rebound = identity_rebound;
    validate_disc_load_commit(request, commit);
    return commit;
}

bool ExecutableDiscLoadTransactionCoordinator::consume_guest_write(
    const GuestWriteEvent& event) noexcept {
    if (!pending_write_.active || event.source != pending_write_.source ||
        canonical_physical_address(event.address) != pending_write_.physical_address ||
        event.size != pending_write_.size)
        return false;
    pending_write_ = {};
    return true;
}

bool ExecutableDiscLoadTransactionCoordinator::transaction_active() const noexcept {
    return pending_write_.active;
}

} // namespace katana::runtime
