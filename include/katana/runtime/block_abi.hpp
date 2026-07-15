#pragma once

#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace katana::runtime {

inline constexpr std::uint32_t block_abi_version = 1u;

struct BlockAddress {
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;

    [[nodiscard]] bool operator==(const BlockAddress&) const noexcept = default;
};

enum class BlockEndKind : std::uint8_t {
    Fallthrough,
    StaticBranch,
    ConditionalBranch,
    DynamicBranch,
    Call,
    Return,
    Exception,
    InterruptSafepoint
};

enum class BlockSyncPoint : std::uint8_t {
    Entry,
    Exit,
    BackendBoundary,
    FallbackBoundary
};

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

void validate_block_entry(
    const CpuState& cpu,
    const BlockExecutionContext& context,
    const BlockEntry& entry
);

[[nodiscard]] BlockExit make_block_exit(
    const CpuState& cpu,
    BlockExecutionContext& context,
    BlockEndKind kind,
    BlockAddress source,
    std::optional<BlockAddress> target = std::nullopt
);

[[nodiscard]] std::uint32_t effective_exception_pc(
    const CpuState& cpu,
    const BlockExecutionContext& context
) noexcept;

[[nodiscard]] std::string stable_block_identity(const BlockAddress& address);

} // namespace katana::runtime
