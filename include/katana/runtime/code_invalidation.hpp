#pragma once

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace katana::runtime {

enum class ExecutableBlockOrigin : std::uint8_t {
    ImageSegment,
    RomRamCopy,
    FallbackDecode,
    RuntimeWrite
};
enum class BlockRegistrationResult : std::uint8_t {
    Inserted,
    AlreadyValid,
    Reactivated,
};

struct ExecutableBlockRegistration {
    std::string identity;
    std::uint32_t physical_start = 0u;
    std::uint32_t size = 0u;
    std::string provenance;
    std::set<std::string> incoming_links;
    ExecutableBlockOrigin origin = ExecutableBlockOrigin::ImageSegment;
};

struct CodeInvalidationResult {
    std::vector<std::string> invalidated_blocks;
    std::vector<std::string> unlinked_sources;
    std::vector<std::uint32_t> changed_pages;
    CodeWriteSource source = CodeWriteSource::Cpu;
    bool byte_identical = false;
};

struct CodeInvalidationPage {
    std::uint32_t physical_page = 0u;
    std::uint64_t generation = 0u;
};

struct CodeInvalidationEvent {
    std::uint64_t sequence = 0u;
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;
    std::size_t size = 0u;
    CodeWriteSource source = CodeWriteSource::Cpu;
    bool byte_identical = false;
    std::vector<CodeInvalidationPage> pages;
    std::vector<std::string> invalidated_blocks;
    std::vector<std::string> unlinked_sources;
};

struct TrackedExecutableBlock {
    ExecutableBlockRegistration block;
    bool valid = true;
};

enum class CodeInvalidationLookupMode : std::uint8_t { PageIndex, ReferenceScan };

struct CodeInvalidationPerformanceCounters {
    std::uint64_t indexed_candidates = 0u;
    std::uint64_t reference_candidates = 0u;
};

class ExecutableCodeTracker {
  public:
    static constexpr std::uint32_t page_size = 4096u;
    static constexpr std::size_t default_provenance_capacity = 1024u;

    explicit ExecutableCodeTracker(
        std::size_t provenance_capacity = default_provenance_capacity);

    [[nodiscard]] BlockRegistrationResult register_block(ExecutableBlockRegistration block);
    [[nodiscard]] CodeInvalidationResult observe_write(std::uint32_t address,
                                                       std::size_t size,
                                                       CodeWriteSource source,
                                                       bool bytes_changed = true);
    [[nodiscard]] bool valid(const std::string& identity) const;
    [[nodiscard]] bool dispatchable(const std::string& identity) const noexcept;
    [[nodiscard]] std::uint64_t page_generation(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint64_t invalidation_count() const noexcept;
    [[nodiscard]] std::size_t block_count() const noexcept;
    [[nodiscard]] std::size_t incoming_link_count(const std::string& identity) const;
    [[nodiscard]] const std::map<std::uint32_t, std::uint64_t>& hotspots() const noexcept;
    [[nodiscard]] const std::vector<TrackedExecutableBlock>& blocks() const noexcept;
    [[nodiscard]] const std::vector<CodeInvalidationEvent>& invalidation_events() const noexcept;
    [[nodiscard]] std::uint64_t dropped_provenance_events() const noexcept;
    [[nodiscard]] std::size_t provenance_capacity() const noexcept;
    [[nodiscard]] CodeInvalidationLookupMode lookup_mode() const noexcept;
    void set_lookup_mode(CodeInvalidationLookupMode mode) noexcept;
    [[nodiscard]] const CodeInvalidationPerformanceCounters& performance_counters() const noexcept;
    void reset_performance_counters() noexcept;

  private:
    void record_invalidation_event(std::uint32_t virtual_address,
                                   std::uint32_t physical_address,
                                   std::size_t size,
                                   const CodeInvalidationResult& result) noexcept;
    std::vector<TrackedExecutableBlock> blocks_;
    std::unordered_map<std::string, std::size_t> identity_index_;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> page_blocks_;
    std::map<std::uint32_t, std::uint64_t> generations_;
    std::map<std::uint32_t, std::uint64_t> hotspots_;
    std::uint64_t invalidation_count_ = 0u;
    std::vector<CodeInvalidationEvent> invalidation_events_;
    std::size_t provenance_capacity_ = default_provenance_capacity;
    std::uint64_t next_provenance_sequence_ = 0u;
    std::uint64_t dropped_provenance_events_ = 0u;
    CodeInvalidationLookupMode lookup_mode_ = CodeInvalidationLookupMode::PageIndex;
    CodeInvalidationPerformanceCounters performance_counters_;
};

[[nodiscard]] const char* code_write_source_name(CodeWriteSource value) noexcept;
[[nodiscard]] const char* executable_block_origin_name(ExecutableBlockOrigin value) noexcept;

} // namespace katana::runtime
