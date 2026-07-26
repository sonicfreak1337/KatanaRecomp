#pragma once

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/platform_services.hpp"
#include "katana/testing/sh4_sst.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace katana::testing::sh4_sst {

struct NativeInstructionObservation {
    std::uint32_t guest_pc = 0u;
    bool delay_slot = false;

    [[nodiscard]] bool operator==(const NativeInstructionObservation&) const = default;
};

struct NativeExecutionTrace {
    std::vector<NativeInstructionObservation> instructions;
    std::vector<std::uint32_t> block_entries;
    bool allocation_failed = false;

    [[nodiscard]] bool complete() const noexcept {
        return !allocation_failed;
    }
};

class AotDispatchError final : public std::runtime_error {
  public:
    AotDispatchError(std::uint32_t target, bool call);

    [[nodiscard]] std::uint32_t target() const noexcept;
    [[nodiscard]] bool call() const noexcept;

  private:
    std::uint32_t target_ = 0u;
    bool call_ = false;
};

struct GeneratedBlockDescriptor {
    std::uint32_t canonical_address = 0u;
    std::uint32_t size = 0u;
    runtime::BackendBlockFunction function = nullptr;
};

struct GeneratedCanonicalSlotDescriptor {
    std::uint8_t slot = 0u;
    std::uint32_t canonical_address = 0u;

    [[nodiscard]] bool operator==(const GeneratedCanonicalSlotDescriptor&) const = default;
};

struct GeneratedRelocationRecipe {
    std::uint32_t source = 0u;
    std::uint8_t anchor_slot = 0u;
    std::uint32_t delta = 0u;

    [[nodiscard]] bool operator==(const GeneratedRelocationRecipe&) const = default;
};

struct GeneratedFormDescriptor {
    std::string_view key;
    std::string_view family;
    std::uint16_t opcode = 0u;
    std::array<std::uint8_t, sh4_sst_cycle_count> fetch_slots{};
    std::span<const GeneratedCanonicalSlotDescriptor> canonical_slots;
    std::span<const GeneratedRelocationRecipe> relocation_recipes;
    bool supported = false;
    bool tested_has_delay_slot = false;
    ResultClassification unsupported_classification = ResultClassification::Pass;
    std::string_view unsupported_reason;
    std::span<const GeneratedBlockDescriptor> blocks;
    std::size_t vector_count = 0u;
    std::size_t generated_functions = 0u;
    std::size_t product_partitions = 0u;
};

struct GeneratedCaseDescriptor {
    std::string_view filename;
    std::uint32_t case_index = 0u;
    std::string_view form_key;
};

void begin_native_execution_trace(NativeExecutionTrace& trace,
                                  runtime::PlatformServices& services) noexcept;
void end_native_execution_trace() noexcept;
void note_native_instruction(std::uint32_t guest_pc, bool delay_slot) noexcept;
void note_native_block(std::uint32_t guest_pc) noexcept;
[[nodiscard]] runtime::PlatformServices* active_native_services() noexcept;

[[noreturn]] void reject_native_dispatch(runtime::CpuState& cpu, std::uint32_t target, bool call);

[[nodiscard]] std::span<const GeneratedFormDescriptor> generated_forms() noexcept;
[[nodiscard]] const GeneratedFormDescriptor* find_generated_form(std::string_view key) noexcept;
[[nodiscard]] std::span<const GeneratedCaseDescriptor> generated_cases() noexcept;
[[nodiscard]] std::span<const std::uint16_t> represented_external_opcodes() noexcept;
[[nodiscard]] std::span<const std::uint16_t> unrepresented_katana_opcodes() noexcept;
[[nodiscard]] std::string_view generated_corpus_scope() noexcept;
[[nodiscard]] std::size_t generated_vector_count() noexcept;

} // namespace katana::testing::sh4_sst
