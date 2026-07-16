#pragma once

#include "katana/runtime/block_dispatch.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace katana::phase9 {

inline constexpr std::uint32_t execution_profile_schema_version = 1u;

enum class ProfilingMode : std::uint8_t { Disabled, Exact, Sampled };
enum class GuardOutcome : std::uint8_t {
    Hit,
    Disabled,
    Region,
    Alignment,
    Permission,
    Mmu,
    Alias,
    Watchpoint,
    Mmio,
    AddressSpace,
    CodeGeneration
};

struct ExecutionProfileIdentity {
    std::string input_sha256;
    std::uint32_t runtime_abi = 0u;
    std::uint32_t backend_abi = 0u;

    [[nodiscard]] bool operator==(const ExecutionProfileIdentity&) const = default;
};

struct ProfileEdge {
    std::uint32_t source = 0u;
    std::uint32_t target = 0u;

    [[nodiscard]] bool operator<(const ProfileEdge& other) const noexcept;
};

struct ProfileSnapshot {
    ExecutionProfileIdentity identity;
    ProfilingMode mode = ProfilingMode::Disabled;
    std::map<std::uint32_t, std::uint64_t> blocks;
    std::map<ProfileEdge, std::uint64_t> edges;
    std::map<std::uint32_t, std::uint64_t> indirect_callsites;
    std::map<std::string, std::uint64_t> fallbacks;
    std::map<std::uint32_t, std::uint64_t> invalidations;
    std::map<GuardOutcome, std::uint64_t> guards;
};

class ExecutionProfiler final {
  public:
    ExecutionProfiler(ProfilingMode mode,
                      ExecutionProfileIdentity identity,
                      std::uint64_t sample_period = 1u);
    void record_block(std::uint32_t address);
    void record_edge(std::uint32_t source, std::uint32_t target);
    void record_indirect_call(std::uint32_t callsite);
    void record_fallback(std::string reason);
    void record_invalidation(std::uint32_t physical_page);
    void record_guard(GuardOutcome outcome);
    [[nodiscard]] const ProfileSnapshot& snapshot() const noexcept;

  private:
    [[nodiscard]] bool selected() noexcept;
    ProfileSnapshot snapshot_;
    std::uint64_t sample_period_ = 1u;
    std::uint64_t event_index_ = 0u;
};

void require_profile_identity(const ProfileSnapshot& profile,
                              const ExecutionProfileIdentity& expected);
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint64_t>>
hot_blocks(const ProfileSnapshot& profile);
[[nodiscard]] std::vector<std::pair<ProfileEdge, std::uint64_t>>
hot_edges(const ProfileSnapshot& profile);
[[nodiscard]] std::string format_execution_profile_json(const ProfileSnapshot& profile);

struct FastMemoryGuard {
    bool enabled = false;
    bool linear_ram = false;
    bool aligned = false;
    bool writable = false;
    bool mmu_disabled = false;
    bool alias_stable = false;
    bool watchpoints_absent = false;
    bool mmio_absent = false;
    std::uint64_t address_space_generation = 0u;
    std::uint64_t expected_address_space_generation = 0u;
    std::uint64_t code_generation = 0u;
    std::uint64_t expected_code_generation = 0u;
};

[[nodiscard]] GuardOutcome evaluate_fast_memory_guard(const FastMemoryGuard& guard,
                                                      bool write) noexcept;

class GuardedMemoryFastpath final {
  public:
    GuardedMemoryFastpath(runtime::Memory& memory,
                          std::shared_ptr<runtime::LinearMemoryDevice> linear,
                          std::uint32_t base_address,
                          runtime::ExecutableCodeTracker* code_tracker = nullptr,
                          ExecutionProfiler* profiler = nullptr);
    [[nodiscard]] std::uint32_t read_u32(std::uint32_t address, const FastMemoryGuard& guard);
    void write_u32(std::uint32_t address, std::uint32_t value, const FastMemoryGuard& guard);
    [[nodiscard]] std::uint64_t hits() const noexcept;
    [[nodiscard]] std::uint64_t misses() const noexcept;

  private:
    [[nodiscard]] std::size_t offset(std::uint32_t address) const;
    runtime::Memory& memory_;
    std::shared_ptr<runtime::LinearMemoryDevice> linear_;
    std::uint32_t base_address_ = 0u;
    runtime::ExecutableCodeTracker* code_tracker_ = nullptr;
    ExecutionProfiler* profiler_ = nullptr;
    std::uint64_t hits_ = 0u;
    std::uint64_t misses_ = 0u;
};

struct InlineCacheEntry {
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    runtime::BlockVariantKey variant;
    std::uint64_t block_generation = 0u;
    std::string block_identity;
};

class MonomorphicDispatchCache final {
  public:
    [[nodiscard]] runtime::IndirectDispatchResult
    dispatch(runtime::CpuState& cpu,
             const runtime::RuntimeBlockTable& table,
             const runtime::IndirectDispatchRequest& request,
             std::uint64_t block_generation,
             ExecutionProfiler* profiler = nullptr);
    void invalidate(const std::string& block_identity) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::uint64_t hits() const noexcept;
    [[nodiscard]] std::uint64_t misses() const noexcept;
    [[nodiscard]] const std::optional<InlineCacheEntry>& entry() const noexcept;

  private:
    std::optional<InlineCacheEntry> entry_;
    std::uint64_t hits_ = 0u;
    std::uint64_t misses_ = 0u;
};

struct InlineDecision {
    bool inline_call = false;
    std::size_t estimated_instructions = 0u;
    std::string reason;
};

[[nodiscard]] InlineDecision decide_inline(std::uint64_t call_count,
                                           std::size_t callee_instructions,
                                           bool recursive,
                                           std::size_t remaining_code_budget) noexcept;

} // namespace katana::phase9
