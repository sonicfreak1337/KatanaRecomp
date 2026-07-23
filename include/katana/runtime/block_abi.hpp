#pragma once

#include "katana/build_contract.hpp"
#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace katana::runtime {

inline constexpr std::uint32_t block_abi_version = build_contract::block_abi_version;

struct BlockAddress {
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;

    [[nodiscard]] bool operator==(const BlockAddress&) const noexcept = default;
};

// Describes a byte-preserving native AOT template that was copied from one
// guest virtual range to another.  The mapping is intentionally expressed in
// virtual addresses: P1/P2 aliases must remain distinct even when they resolve
// to the same physical bytes.
struct CodeAddressMapping {
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t extent = 0u;

    [[nodiscard]] bool operator==(const CodeAddressMapping&) const noexcept = default;
};

void validate_code_address_mapping(const CodeAddressMapping& mapping);

// Applies only the innermost active mapping that contains the address.  An
// address outside every active range is returned unchanged.  The two helpers
// perform exactly one translation step, which keeps overlapping aliases and
// nested mappings deterministic.
[[nodiscard]] std::uint32_t relocate_code_address(std::uint32_t source_address) noexcept;
[[nodiscard]] std::uint32_t unrelocate_code_address(std::uint32_t runtime_address) noexcept;

class ScopedCodeAddressMapping final {
  public:
    explicit ScopedCodeAddressMapping(CodeAddressMapping mapping);
    ~ScopedCodeAddressMapping() noexcept;

    ScopedCodeAddressMapping(const ScopedCodeAddressMapping&) = delete;
    ScopedCodeAddressMapping& operator=(const ScopedCodeAddressMapping&) = delete;
    ScopedCodeAddressMapping(ScopedCodeAddressMapping&&) = delete;
    ScopedCodeAddressMapping& operator=(ScopedCodeAddressMapping&&) = delete;

  private:
    std::uint64_t token_ = 0u;
};

enum class BlockEndKind : std::uint8_t {
    Fallthrough,
    StaticBranch,
    ConditionalBranch,
    DynamicBranch,
    Call,
    Return,
    ExceptionReturn,
    Sleep,
    Exception,
    InterruptSafepoint
};

enum class BlockSyncPoint : std::uint8_t { Entry, Exit, BackendBoundary, FallbackBoundary };

struct BlockExecutionContext {
    std::uint64_t scheduler_cycle = 0u;
    std::size_t scheduler_event_budget = 0u;
    std::optional<std::uint32_t> delay_slot_owner_pc;
    BlockSyncPoint sync_point = BlockSyncPoint::Entry;
};

struct BlockEntry {
    BlockAddress address;
    std::uint32_t required_runtime_abi = abi_version;
    std::uint32_t required_block_abi = block_abi_version;
};

struct BlockExit {
    BlockEndKind kind = BlockEndKind::Fallthrough;
    BlockAddress source;
    std::optional<BlockAddress> target;
    std::uint64_t scheduler_cycle = 0u;
    std::size_t scheduler_event_budget = 0u;
    ExceptionCause exception_cause = ExceptionCause::None;
    bool in_delay_slot = false;
    std::uint32_t exception_owner_pc = 0u;
};

void validate_block_entry(const CpuState& cpu,
                          const BlockExecutionContext& context,
                          const BlockEntry& entry);

[[nodiscard]] BlockExit make_block_exit(const CpuState& cpu,
                                        BlockExecutionContext& context,
                                        BlockEndKind kind,
                                        BlockAddress source,
                                        std::optional<BlockAddress> target = std::nullopt);

[[nodiscard]] std::uint32_t effective_exception_pc(const CpuState& cpu,
                                                   const BlockExecutionContext& context) noexcept;

[[nodiscard]] std::string stable_block_identity(const BlockAddress& address);

} // namespace katana::runtime
