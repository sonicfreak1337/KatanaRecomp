#include "katana/runtime/wait_loop_trace.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace katana::runtime {
namespace {

void increment_saturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

bool valid_access_event(const GuestMemoryAccessEvent& event) noexcept {
    constexpr std::uint64_t guest_address_space_size = 0x100000000ull;
    if (event.size == 0u ||
        event.size > guest_address_space_size -
                         static_cast<std::uint64_t>(event.physical_address) ||
        event.linear_byte_count > event.linear_byte_offsets.size())
        return false;
    if (event.linear_backing == nullptr)
        return event.linear_byte_count == 0u && event.linear_size == 0u;
    if (event.linear_byte_count == 0u) return false;
    const auto backing_size = event.linear_backing->size();
    for (std::uint8_t index = 0u; index < event.linear_byte_count; ++index) {
        if (event.linear_byte_offsets[index] >= backing_size) return false;
    }
    if (!event.linear_contiguous) return event.linear_size == 0u;
    return event.linear_size != 0u && event.linear_offset <= backing_size &&
           event.linear_size <= backing_size - event.linear_offset;
}

const char* evidence_name(const RuntimeWaitLoopEvidence evidence) noexcept {
    switch (evidence) {
    case RuntimeWaitLoopEvidence::ProvenGuard:
        return "proven-guard";
    case RuntimeWaitLoopEvidence::UnresolvedGuard:
        return "unresolved-guard";
    case RuntimeWaitLoopEvidence::ConservativeCandidate:
        return "conservative-candidate";
    }
    return "unknown";
}

const char* origin_name(const GuestMemoryAccessOrigin origin) noexcept {
    switch (origin) {
    case GuestMemoryAccessOrigin::Memory:
        return "memory";
    case GuestMemoryAccessOrigin::PvrRender:
        return "pvr-render";
    case GuestMemoryAccessOrigin::PvrYuv:
        return "pvr-yuv";
    }
    return "unknown";
}

const char* write_source_name(const CodeWriteSource source) noexcept {
    switch (source) {
    case CodeWriteSource::Cpu:
        return "cpu";
    case CodeWriteSource::Fpu:
        return "fpu";
    case CodeWriteSource::Dma:
        return "dma";
    case CodeWriteSource::StoreQueue:
        return "store-queue";
    case CodeWriteSource::Copy:
        return "copy";
    case CodeWriteSource::Fallback:
        return "fallback";
    }
    return "unknown";
}

const char* writer_link_kind_name(
    const RuntimeWaitLoopWriterLinkKind kind) noexcept {
    switch (kind) {
    case RuntimeWaitLoopWriterLinkKind::None:
        return "none";
    case RuntimeWaitLoopWriterLinkKind::ExactBackingBytes:
        return "exact-backing-bytes";
    case RuntimeWaitLoopWriterLinkKind::PhysicalRangeCandidate:
        return "physical-range-candidate";
    }
    return "unknown";
}

} // namespace

RuntimeWaitLoopTraceRecorder::RuntimeWaitLoopTraceRecorder(
    const std::span<const RuntimeWaitLoopDescriptor> descriptors,
    const RuntimeWaitLoopTraceConfig config)
    : config_(config), descriptors_(descriptors.begin(), descriptors.end()) {
    descriptor_indices_by_read_site_.resize(descriptors_.size());
    for (std::size_t index = 0u; index < descriptors_.size(); ++index)
        descriptor_indices_by_read_site_[index] = index;
    std::sort(descriptor_indices_by_read_site_.begin(),
              descriptor_indices_by_read_site_.end(),
              [&](const std::size_t left, const std::size_t right) {
                  const auto left_site = descriptors_[left].read_site;
                  const auto right_site = descriptors_[right].read_site;
                  return left_site == right_site ? left < right : left_site < right_site;
              });
    locations_.reserve(config_.location_capacity);
    location_indices_by_backing_.reserve(config_.location_capacity);
    non_linear_location_indices_.reserve(config_.location_capacity);
    value_runs_.reserve(config_.value_run_capacity);
    writers_.reserve(config_.writer_capacity);
}

GuestMemoryAccessSink RuntimeWaitLoopTraceRecorder::sink() noexcept {
    return GuestMemoryAccessSink{this, &RuntimeWaitLoopTraceRecorder::callback};
}

void RuntimeWaitLoopTraceRecorder::callback(
    void* const context,
    const GuestMemoryAccessEvent& event) noexcept {
    static_cast<RuntimeWaitLoopTraceRecorder*>(context)->observe(event);
}

void RuntimeWaitLoopTraceRecorder::observe(const GuestMemoryAccessEvent& event) noexcept {
    increment_saturating(counters_.access_events);
    const auto access_sequence = next_sequence_;
    if (next_sequence_ != std::numeric_limits<std::uint64_t>::max()) ++next_sequence_;

    if (!valid_access_event(event)) {
        increment_saturating(counters_.invalid_access_events);
        return;
    }
    if (event.operation == MemoryAccessOperation::Read) {
        observe_read(event, access_sequence);
    } else {
        observe_write(event, access_sequence);
    }
}

std::span<const RuntimeWaitLoopDescriptor>
RuntimeWaitLoopTraceRecorder::descriptors() const noexcept {
    return descriptors_;
}

std::span<const RuntimeWaitLoopValueRun>
RuntimeWaitLoopTraceRecorder::value_runs() const noexcept {
    return value_runs_;
}

std::span<const RuntimeWaitLoopWriterEvent>
RuntimeWaitLoopTraceRecorder::writers() const noexcept {
    return writers_;
}

const RuntimeWaitLoopTraceCounters& RuntimeWaitLoopTraceRecorder::counters() const noexcept {
    return counters_;
}

bool RuntimeWaitLoopTraceRecorder::complete() const noexcept {
    return counters_.invalid_access_events == 0u &&
           counters_.dropped_value_runs == 0u && counters_.dropped_writer_events == 0u &&
           counters_.dropped_locations == 0u;
}

RuntimeWaitLoopTraceRecorder::ActiveLocation*
RuntimeWaitLoopTraceRecorder::find_or_create_location(
    const std::size_t descriptor_index,
    const GuestMemoryAccessEvent& event,
    const std::uint64_t) noexcept {
    for (auto& location : locations_) {
        if (location.descriptor_index != descriptor_index ||
            location.backing != event.linear_backing ||
            location.width != event.width) {
            continue;
        }
        if (location.backing == nullptr) {
            if (location.physical_address == event.physical_address &&
                location.size == event.size)
                return &location;
            continue;
        }
        if (location.byte_count != event.linear_byte_count) continue;
        bool same = true;
        for (std::uint8_t index = 0u; index < location.byte_count; ++index) {
            if (location.byte_offsets[index] != event.linear_byte_offsets[index]) {
                same = false;
                break;
            }
        }
        if (same) return &location;
    }

    if ((event.linear_backing != nullptr &&
         (event.linear_byte_count == 0u ||
          event.linear_byte_count > event.linear_byte_offsets.size())) ||
        (event.linear_backing == nullptr && event.size == 0u) ||
        locations_.size() >= config_.location_capacity) {
        increment_saturating(counters_.dropped_locations);
        return nullptr;
    }

    ActiveLocation location;
    location.descriptor_index = descriptor_index;
    location.backing = event.linear_backing;
    location.byte_offsets = event.linear_byte_offsets;
    location.byte_count = event.linear_byte_count;
    location.physical_address = event.physical_address;
    location.size = event.size;
    location.width = event.width;
    bool location_appended = false;
    try {
        locations_.push_back(location);
        location_appended = true;
        const auto location_index = locations_.size() - 1u;
        if (event.linear_backing != nullptr)
            location_indices_by_backing_[event.linear_backing].push_back(location_index);
        else
            non_linear_location_indices_.push_back(location_index);
    } catch (...) {
        if (location_appended) locations_.pop_back();
        increment_saturating(counters_.dropped_locations);
        return nullptr;
    }
    return &locations_.back();
}

void RuntimeWaitLoopTraceRecorder::observe_read(
    const GuestMemoryAccessEvent& event,
    const std::uint64_t access_sequence) noexcept {
    if (!event.instruction.valid || !event.scalar_value_valid) {
        increment_saturating(counters_.ignored_access_events);
        return;
    }

    const auto first = std::lower_bound(
        descriptor_indices_by_read_site_.begin(),
        descriptor_indices_by_read_site_.end(),
        event.instruction.source_pc,
        [&](const std::size_t descriptor_index, const std::uint32_t read_site) {
            return descriptors_[descriptor_index].read_site < read_site;
        });
    const auto last = std::upper_bound(
        first,
        descriptor_indices_by_read_site_.end(),
        event.instruction.source_pc,
        [&](const std::uint32_t read_site, const std::size_t descriptor_index) {
            return read_site < descriptors_[descriptor_index].read_site;
        });
    if (first == last) {
        increment_saturating(counters_.ignored_access_events);
        return;
    }
    for (auto current = first; current != last; ++current) {
        const auto descriptor_index = *current;
        auto* const location =
            find_or_create_location(descriptor_index, event, access_sequence);
        if (location == nullptr) continue;

        const auto previous_read_sequence = location->last_read_access_sequence;
        if (location->has_value && location->current_value == event.value) {
            if (location->current_run_recorded) {
                auto& run = value_runs_[location->current_run_index];
                increment_saturating(run.samples);
                run.last_access_sequence = access_sequence;
                run.last_retired_guest_instructions = event.retired_guest_instructions;
            }
            location->last_read_access_sequence = access_sequence;
            continue;
        }

        location->has_value = true;
        location->current_value = event.value;
        location->last_read_access_sequence = access_sequence;
        location->current_run_recorded = false;
        if (value_runs_.size() >= config_.value_run_capacity) {
            increment_saturating(counters_.dropped_value_runs);
            continue;
        }

        RuntimeWaitLoopValueRun run;
        run.sequence = access_sequence;
        run.first_access_sequence = access_sequence;
        run.last_access_sequence = access_sequence;
        run.samples = 1u;
        run.descriptor_index = descriptor_index;
        run.instruction = event.instruction;
        run.virtual_address = event.virtual_address;
        run.physical_address = event.physical_address;
        run.width = event.width;
        run.value = event.value;
        run.first_retired_guest_instructions = event.retired_guest_instructions;
        run.last_retired_guest_instructions = event.retired_guest_instructions;
        if (location->has_last_writer &&
            writers_[location->last_writer_index].last_access_sequence >
                previous_read_sequence) {
            run.writer_linked = true;
            run.writer_sequence = writers_[location->last_writer_index].sequence;
            run.writer_link_kind =
                location->backing != nullptr
                    ? RuntimeWaitLoopWriterLinkKind::ExactBackingBytes
                    : RuntimeWaitLoopWriterLinkKind::PhysicalRangeCandidate;
        }
        value_runs_.push_back(run);
        location->current_run_index = value_runs_.size() - 1u;
        location->current_run_recorded = true;
    }
}

RuntimeWaitLoopTraceRecorder::WriterProjection
RuntimeWaitLoopTraceRecorder::writer_projection(
    const GuestMemoryAccessEvent& event) noexcept {
    WriterProjection projection;
    projection.backing = event.linear_backing;
    projection.byte_offsets = event.linear_byte_offsets;
    projection.byte_count = event.linear_byte_count;
    projection.contiguous = event.linear_contiguous && event.linear_size != 0u;
    projection.contiguous_offset = event.linear_offset;
    projection.contiguous_size = event.linear_size;
    projection.physical_address = event.physical_address;
    projection.size = event.size;
    return projection;
}

bool RuntimeWaitLoopTraceRecorder::overlaps(
    const ActiveLocation& location,
    const WriterProjection& writer) noexcept {
    if (location.backing == nullptr || writer.backing == nullptr) {
        if (location.backing != nullptr || writer.backing != nullptr ||
            location.size == 0u || writer.size == 0u)
            return false;
        const std::uint64_t read_start = location.physical_address;
        const std::uint64_t read_end = read_start + location.size;
        const std::uint64_t write_start = writer.physical_address;
        const std::uint64_t write_end = write_start + writer.size;
        return read_start < write_end && write_start < read_end;
    }
    if (location.backing != writer.backing) return false;
    const auto read_count =
        std::min<std::size_t>(location.byte_count, location.byte_offsets.size());
    const auto write_count =
        std::min<std::size_t>(writer.byte_count, writer.byte_offsets.size());
    for (std::size_t read_index = 0u; read_index < read_count; ++read_index) {
        const auto read_offset = location.byte_offsets[read_index];
        if (writer.contiguous) {
            const std::uint64_t start = writer.contiguous_offset;
            const std::uint64_t end = start + writer.contiguous_size;
            if (read_offset >= start && read_offset < end) return true;
            continue;
        }
        for (std::size_t write_index = 0u; write_index < write_count; ++write_index) {
            if (read_offset == writer.byte_offsets[write_index]) return true;
        }
    }
    return false;
}

bool RuntimeWaitLoopTraceRecorder::same_writer(
    const RuntimeWaitLoopWriterEvent& left,
    const GuestMemoryAccessEvent& right) noexcept {
    return left.access_origin == right.access_origin &&
           left.instruction.source_pc == right.instruction.source_pc &&
           left.instruction.runtime_pc == right.instruction.runtime_pc &&
           left.instruction.valid == right.instruction.valid &&
           left.virtual_address == right.virtual_address &&
           left.physical_address == right.physical_address && left.size == right.size &&
           left.width == right.width && left.value == right.value &&
           left.source == right.write_source &&
           left.scalar_value_valid == right.scalar_value_valid &&
           left.bytes_changed == right.bytes_changed;
}

void RuntimeWaitLoopTraceRecorder::observe_write(
    const GuestMemoryAccessEvent& event,
    const std::uint64_t access_sequence) noexcept {
    if (!event.bytes_changed) {
        increment_saturating(counters_.ignored_access_events);
        return;
    }
    const auto projection = writer_projection(event);
    const std::vector<std::size_t>* candidate_indices = nullptr;
    if (projection.backing != nullptr) {
        const auto found = location_indices_by_backing_.find(projection.backing);
        if (found != location_indices_by_backing_.end())
            candidate_indices = &found->second;
    } else {
        candidate_indices = &non_linear_location_indices_;
    }
    if (candidate_indices == nullptr || candidate_indices->empty()) {
        increment_saturating(counters_.ignored_access_events);
        return;
    }
    bool relevant = false;
    for (const auto location_index : *candidate_indices) {
        if (overlaps(locations_[location_index], projection)) {
            relevant = true;
            break;
        }
    }
    if (!relevant) {
        increment_saturating(counters_.ignored_access_events);
        return;
    }

    std::size_t writer_index = 0u;
    if (!writers_.empty() && same_writer(writers_.back(), event) &&
        writers_.back().last_access_sequence + 1u == access_sequence) {
        writer_index = writers_.size() - 1u;
        writers_.back().last_access_sequence = access_sequence;
        increment_saturating(writers_.back().writes);
        writers_.back().retired_guest_instructions = event.retired_guest_instructions;
    } else {
        if (writers_.size() >= config_.writer_capacity) {
            increment_saturating(counters_.dropped_writer_events);
            for (const auto location_index : *candidate_indices) {
                auto& location = locations_[location_index];
                if (overlaps(location, projection)) location.has_last_writer = false;
            }
            return;
        }
        RuntimeWaitLoopWriterEvent writer;
        writer.sequence = access_sequence;
        writer.last_access_sequence = access_sequence;
        writer.writes = 1u;
        writer.access_origin = event.access_origin;
        writer.instruction = event.instruction;
        writer.virtual_address = event.virtual_address;
        writer.physical_address = event.physical_address;
        writer.size = event.size;
        writer.width = event.width;
        writer.value = event.value;
        writer.source = event.write_source;
        writer.scalar_value_valid = event.scalar_value_valid;
        writer.bytes_changed = event.bytes_changed;
        writer.retired_guest_instructions = event.retired_guest_instructions;
        writers_.push_back(writer);
        writer_index = writers_.size() - 1u;
    }
    for (const auto location_index : *candidate_indices) {
        auto& location = locations_[location_index];
        if (!overlaps(location, projection)) continue;
        location.last_writer_index = writer_index;
        location.has_last_writer = true;
    }
}

std::string RuntimeWaitLoopTraceRecorder::serialize_json() const {
    std::ostringstream output;
    output << "{\"schema\":\"katana.runtime-wait-loop-trace\",\"trace_version\":"
           << runtime_wait_loop_trace_version << ",\"complete\":"
           << (complete() ? "true" : "false")
           << ",\"contains_raw_guest_values\":true"
              ",\"writer_scope\":\"since-previous-sample\""
              ",\"counters\":{\"access_events\":"
           << counters_.access_events << ",\"ignored_access_events\":"
           << counters_.ignored_access_events << ",\"invalid_access_events\":"
           << counters_.invalid_access_events << ",\"dropped_value_runs\":"
           << counters_.dropped_value_runs << ",\"dropped_writer_events\":"
           << counters_.dropped_writer_events << ",\"dropped_locations\":"
           << counters_.dropped_locations << "},\"descriptors\":[";
    for (std::size_t index = 0u; index < descriptors_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& descriptor = descriptors_[index];
        output << "{\"loop_header\":" << descriptor.loop_header
               << ",\"loop_latch\":" << descriptor.loop_latch
               << ",\"read_site\":" << descriptor.read_site << ",\"evidence\":\""
               << evidence_name(descriptor.evidence) << "\"}";
    }
    output << "],\"value_runs\":[";
    for (std::size_t index = 0u; index < value_runs_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& run = value_runs_[index];
        output << "{\"sequence\":" << run.sequence
               << ",\"first_access_sequence\":" << run.first_access_sequence
               << ",\"last_access_sequence\":" << run.last_access_sequence
               << ",\"samples\":" << run.samples << ",\"descriptor_index\":"
               << run.descriptor_index << ",\"source_pc\":" << run.instruction.source_pc
               << ",\"runtime_pc\":" << run.instruction.runtime_pc
               << ",\"virtual_address\":" << run.virtual_address
               << ",\"physical_address\":" << run.physical_address
               << ",\"width\":" << static_cast<unsigned>(run.width)
               << ",\"value\":" << run.value
               << ",\"first_retired_guest_instructions\":"
               << run.first_retired_guest_instructions
               << ",\"last_retired_guest_instructions\":"
               << run.last_retired_guest_instructions << ",\"writer_linked\":"
               << (run.writer_linked ? "true" : "false")
               << ",\"writer_link_kind\":\""
               << writer_link_kind_name(run.writer_link_kind)
               << "\",\"writer_sequence\":" << run.writer_sequence << "}";
    }
    output << "],\"writers\":[";
    for (std::size_t index = 0u; index < writers_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& writer = writers_[index];
        output << "{\"sequence\":" << writer.sequence
               << ",\"last_access_sequence\":" << writer.last_access_sequence
               << ",\"writes\":" << writer.writes << ",\"origin\":\""
               << origin_name(writer.access_origin) << "\",\"source\":\""
               << write_source_name(writer.source) << "\",\"source_pc\":"
               << writer.instruction.source_pc << ",\"runtime_pc\":"
               << writer.instruction.runtime_pc << ",\"instruction_valid\":"
               << (writer.instruction.valid ? "true" : "false")
               << ",\"virtual_address\":"
               << writer.virtual_address << ",\"physical_address\":"
               << writer.physical_address << ",\"size\":" << writer.size
               << ",\"width\":" << static_cast<unsigned>(writer.width)
               << ",\"scalar_value_valid\":"
               << (writer.scalar_value_valid ? "true" : "false")
               << ",\"value\":";
        if (writer.scalar_value_valid)
            output << writer.value;
        else
            output << "null";
        output << ",\"bytes_changed\":"
               << (writer.bytes_changed ? "true" : "false")
               << ",\"retired_guest_instructions\":"
               << writer.retired_guest_instructions << "}";
    }
    output << "]}";
    return output.str();
}

} // namespace katana::runtime
