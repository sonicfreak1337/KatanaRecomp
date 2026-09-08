#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/ir/ir.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace katana::analysis::detail {

// A direct-call return may be the receiver later passed to a callback, but
// this diagnostic does not prove allocation, persistence, or an executable
// target set.  Origins are exact callsite/callee pairs.  Every result remains
// candidate inventory only and is intentionally outside authoritative analysis.
struct StaticReturnedReceiverOrigin final {
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t callee_address = 0u;

    bool operator==(const StaticReturnedReceiverOrigin&) const = default;
};

struct StaticReturnedReceiverContract final {
    std::uint32_t function_address = 0u;
    StaticReturnedReceiverOrigin origin;
    bool optional_null = false;
    bool complete = false;
    bool candidate_only = true;
    bool strict_eligible = false;
    bool execution_eligible = false;

    bool operator==(const StaticReturnedReceiverContract&) const = default;
};

struct StaticReturnedReceiverFieldCandidate final {
    katana::analysis::StaticCallbackFieldSinkContract field;
    StaticReturnedReceiverOrigin origin;
    bool optional_null = false;
    bool complete = false;
    bool candidate_only = true;
    bool strict_eligible = false;
    bool execution_eligible = false;

    bool operator==(const StaticReturnedReceiverFieldCandidate&) const =
        default;
};

// One deterministic, bounded status record is emitted for every distinct
// function that the diagnostic actually examines.  The record is descriptive
// only: it never participates in callback discovery, root closure, strict
// admission, or executable dispatch.
enum class StaticReturnedReceiverFunctionOutcome : std::uint8_t {
    Recognized,
    NoReturnOrigin,
    Incomplete,
    Budget,
};

enum class StaticReturnedReceiverRejectionReason : std::uint8_t {
    None,
    DuplicateBlock,
    MissingEntryBlock,
    EmptyBlock,
    BlockStartMismatch,
    InstructionAddressGap,
    DecodedInstructionMissing,
    DecodedInstructionUnknown,
    DelaySlotMismatch,
    SuccessorMissing,
    WorkBudget,
    MissingIncomingState,
    InvalidCall,
    UnresolvedCallTarget,
    UnknownCallAbi,
    InvalidDelaySlot,
    IndirectTailcall,
    InvalidReturn,
    InvalidBranch,
    UnsupportedInstruction,
    InvalidContinuation,
    UnreachedBlock,
    NoReturn,
    NullOnlyReturn,
    ReturnValueUnknown,
    ReturnOriginConflict,
    IncompleteAnalysis,
};

struct StaticReturnedReceiverFunctionDiagnostic final {
    std::uint32_t function_address = 0u;
    bool present = true;
    std::size_t instruction_count = 0u;
    std::size_t work_items = 0u;
    StaticReturnedReceiverFunctionOutcome outcome =
        StaticReturnedReceiverFunctionOutcome::Incomplete;
    StaticReturnedReceiverRejectionReason rejection_reason =
        StaticReturnedReceiverRejectionReason::None;
    std::uint32_t rejection_block_address = 0u;
    std::uint32_t rejection_instruction_address = 0u;

    bool operator==(const StaticReturnedReceiverFunctionDiagnostic&) const =
        default;
};

struct StaticReturnedReceiverInventory final {
    std::vector<StaticReturnedReceiverContract> returned_receivers;
    std::vector<StaticReturnedReceiverFieldCandidate> field_candidates;
    std::vector<StaticReturnedReceiverFunctionDiagnostic>
        function_diagnostics;
    std::size_t functions_examined = 0u;
    std::size_t instructions_examined = 0u;
    std::size_t work_items = 0u;
    std::size_t work_budget = 0u;
    std::size_t incomplete_functions = 0u;
    bool truncated = false;

    bool operator==(const StaticReturnedReceiverInventory&) const = default;
};

// Finds only complete, finite return summaries whose reachable returns are
// either null or the same exact direct-call origin.  Field candidates require
// an existing field sink contract and an actual receiver argument carrying
// that origin at the contract's callsite.  This fail-closed diagnostic never
// promotes a root, closure, strict contract, or runtime execution target.
[[nodiscard]] StaticReturnedReceiverInventory
discover_static_returned_receiver_contracts(
    const katana::io::ExecutableImage& image,
    std::span<const katana::ir::Function> program,
    std::span<const katana::sh4::DisassemblyLine> decoded_lines,
    std::span<const katana::analysis::StaticCallbackFieldSinkContract>
        field_sink_contracts,
    std::size_t maximum_work_items = 16u * 1024u * 1024u);

} // namespace katana::analysis::detail
