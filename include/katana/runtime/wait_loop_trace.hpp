#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t runtime_wait_loop_trace_version = 1u;

enum class RuntimeWaitLoopEvidence : std::uint8_t {
    ProvenGuard,
    UnresolvedGuard,
    ConservativeCandidate
};

enum class RuntimeWaitLoopWriterLinkKind : std::uint8_t {
    None,
    ExactBackingBytes,
    PhysicalRangeCandidate
};

struct RuntimeWaitLoopDescriptor {
    std::uint32_t loop_header = 0u;
    std::uint32_t loop_latch = 0u;
    std::uint32_t read_site = 0u;
    RuntimeWaitLoopEvidence evidence = RuntimeWaitLoopEvidence::ProvenGuard;
};

struct RuntimeWaitLoopTraceConfig {
    std::size_t value_run_capacity = 4096u;
    std::size_t writer_capacity = 4096u;
    std::size_t location_capacity = 256u;
};

struct RuntimeWaitLoopValueRun {
    std::uint64_t sequence = 0u;
    std::uint64_t first_access_sequence = 0u;
    std::uint64_t last_access_sequence = 0u;
    std::uint64_t samples = 0u;
    std::size_t descriptor_index = 0u;
    GuestInstructionOrigin instruction;
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;
    MemoryAccessWidth width = MemoryAccessWidth::Byte;
    std::uint32_t value = 0u;
    std::uint64_t first_retired_guest_instructions = 0u;
    std::uint64_t last_retired_guest_instructions = 0u;
    std::uint64_t writer_sequence = 0u;
    RuntimeWaitLoopWriterLinkKind writer_link_kind =
        RuntimeWaitLoopWriterLinkKind::None;
    bool writer_linked = false;
};

struct RuntimeWaitLoopWriterEvent {
    std::uint64_t sequence = 0u;
    std::uint64_t last_access_sequence = 0u;
    std::uint64_t writes = 0u;
    GuestMemoryAccessOrigin access_origin = GuestMemoryAccessOrigin::Memory;
    GuestInstructionOrigin instruction;
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;
    std::size_t size = 0u;
    MemoryAccessWidth width = MemoryAccessWidth::Byte;
    std::uint32_t value = 0u;
    CodeWriteSource source = CodeWriteSource::Cpu;
    bool scalar_value_valid = false;
    bool bytes_changed = false;
    std::uint64_t retired_guest_instructions = 0u;
};

struct RuntimeWaitLoopTraceCounters {
    std::uint64_t access_events = 0u;
    std::uint64_t ignored_access_events = 0u;
    std::uint64_t invalid_access_events = 0u;
    std::uint64_t dropped_value_runs = 0u;
    std::uint64_t dropped_writer_events = 0u;
    std::uint64_t dropped_locations = 0u;
};

class RuntimeWaitLoopTraceRecorder final {
  public:
    explicit RuntimeWaitLoopTraceRecorder(
        std::span<const RuntimeWaitLoopDescriptor> descriptors,
        RuntimeWaitLoopTraceConfig config = {});

    [[nodiscard]] GuestMemoryAccessSink sink() noexcept;
    void observe(const GuestMemoryAccessEvent& event) noexcept;

    [[nodiscard]] std::span<const RuntimeWaitLoopDescriptor> descriptors() const noexcept;
    [[nodiscard]] std::span<const RuntimeWaitLoopValueRun> value_runs() const noexcept;
    [[nodiscard]] std::span<const RuntimeWaitLoopWriterEvent> writers() const noexcept;
    [[nodiscard]] const RuntimeWaitLoopTraceCounters& counters() const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::string serialize_json() const;

  private:
    struct ActiveLocation {
        std::size_t descriptor_index = 0u;
        const LinearMemoryDevice* backing = nullptr;
        std::array<std::uint32_t, 4u> byte_offsets{};
        std::uint8_t byte_count = 0u;
        std::uint32_t physical_address = 0u;
        std::size_t size = 0u;
        MemoryAccessWidth width = MemoryAccessWidth::Byte;
        std::uint32_t current_value = 0u;
        std::size_t current_run_index = 0u;
        std::size_t last_writer_index = 0u;
        std::uint64_t last_read_access_sequence = 0u;
        bool has_value = false;
        bool current_run_recorded = false;
        bool has_last_writer = false;
    };

    struct WriterProjection {
        const LinearMemoryDevice* backing = nullptr;
        std::array<std::uint32_t, 4u> byte_offsets{};
        std::uint32_t contiguous_offset = 0u;
        std::size_t contiguous_size = 0u;
        std::uint32_t physical_address = 0u;
        std::size_t size = 0u;
        std::uint8_t byte_count = 0u;
        bool contiguous = false;
    };

    static void callback(void* context, const GuestMemoryAccessEvent& event) noexcept;
    [[nodiscard]] ActiveLocation*
    find_or_create_location(std::size_t descriptor_index,
                            const GuestMemoryAccessEvent& event,
                            std::uint64_t access_sequence) noexcept;
    void observe_read(const GuestMemoryAccessEvent& event,
                      std::uint64_t access_sequence) noexcept;
    void observe_write(const GuestMemoryAccessEvent& event,
                       std::uint64_t access_sequence) noexcept;
    [[nodiscard]] static bool overlaps(const ActiveLocation& location,
                                       const WriterProjection& writer) noexcept;
    [[nodiscard]] static WriterProjection
    writer_projection(const GuestMemoryAccessEvent& event) noexcept;
    [[nodiscard]] static bool same_writer(const RuntimeWaitLoopWriterEvent& left,
                                          const GuestMemoryAccessEvent& right) noexcept;

    RuntimeWaitLoopTraceConfig config_;
    std::vector<RuntimeWaitLoopDescriptor> descriptors_;
    std::vector<std::size_t> descriptor_indices_by_read_site_;
    std::vector<ActiveLocation> locations_;
    std::unordered_map<const LinearMemoryDevice*, std::vector<std::size_t>>
        location_indices_by_backing_;
    std::vector<std::size_t> non_linear_location_indices_;
    std::vector<RuntimeWaitLoopValueRun> value_runs_;
    std::vector<RuntimeWaitLoopWriterEvent> writers_;
    RuntimeWaitLoopTraceCounters counters_;
    std::uint64_t next_sequence_ = 1u;
};

} // namespace katana::runtime
