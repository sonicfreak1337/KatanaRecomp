#pragma once

#include "katana/codegen/native_aot_profile.hpp"
#include "katana/runtime/block_abi.hpp"
#include "katana/testing/sh4_sst.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace katana::testing::sh4_sst {

inline constexpr std::uint8_t normal_code_slot_count = 4u;
inline constexpr std::uint8_t maximum_external_code_slots = 4u;
inline constexpr std::uint8_t maximum_code_slot_count =
    normal_code_slot_count + maximum_external_code_slots;
inline constexpr std::uint8_t invalid_code_slot = 0xFFu;

enum class FetchTransition : std::uint8_t {
    Repeated,
    Sequential,
    DirectTarget,
    DelayedDirectTarget,
    DynamicOrOther,
};

// A code form deliberately excludes absolute corpus addresses. It contains every
// property which can affect decoded/lowered/generated semantics, while equal external
// targets are represented by the same first-occurrence slot.
struct SstCodeForm {
    std::array<std::uint16_t, sh4_sst_opcode_count> opcodes{};
    std::array<std::uint8_t, sh4_sst_cycle_count> fetch_slots{};
    std::array<FetchTransition, sh4_sst_cycle_count - 1u> transitions{};
    std::array<std::uint8_t, maximum_external_code_slots> external_pc_mod4{
        invalid_code_slot,
        invalid_code_slot,
        invalid_code_slot,
        invalid_code_slot,
    };
    std::uint32_t profile_version = katana::codegen::native_aot_emission_profile_version;
    std::uint8_t initial_pc_mod4 = 0u;
    std::uint8_t normal_pc_wrap_mask = 0u;
    std::uint8_t external_slot_count = 0u;
    bool fpscr_pr = false;
    bool fpscr_sz = false;

    [[nodiscard]] bool operator==(const SstCodeForm&) const = default;
};

struct SstCodeSlotBinding {
    std::uint8_t slot = invalid_code_slot;
    std::uint16_t opcode = 0u;
    std::uint32_t canonical_address = 0u;
    std::uint32_t runtime_address = 0u;

    [[nodiscard]] bool operator==(const SstCodeSlotBinding&) const = default;
};

struct SstCodeRelocationRecipe {
    std::uint32_t source = 0u;
    std::uint8_t anchor_slot = invalid_code_slot;
    std::uint32_t delta = 0u;

    [[nodiscard]] bool operator==(const SstCodeRelocationRecipe&) const = default;
};

[[nodiscard]] SstCodeForm make_sst_code_form(const SstTestCase& test);
[[nodiscard]] std::string sst_code_form_key(const SstCodeForm& form);
[[nodiscard]] std::string sst_code_form_key(const SstTestCase& test);

[[nodiscard]] std::optional<std::uint32_t> canonical_address_for_sst_slot(const SstCodeForm& form,
                                                                          std::uint8_t slot);

// Product CFG construction needs independent proven entries whenever one
// physical instruction can be both an owner's delay slot and a normal target.
// This includes a statically visible self-slot target even when the concrete
// four-cycle reference trace does not take that branch.
[[nodiscard]] bool sst_code_form_requires_contextual_delay_slot_entries(const SstCodeForm& form);

[[nodiscard]] std::optional<std::uint32_t> runtime_address_for_sst_slot(const SstTestCase& test,
                                                                        std::uint8_t slot);

[[nodiscard]] std::vector<SstCodeSlotBinding> sst_code_slot_bindings(const SstTestCase& test,
                                                                     const SstCodeForm& form);

// Build-time-only recipes materialize every address constant emitted for a code
// form. Runtime relocation is data-driven: runtime_start = slot[anchor_slot] + delta.
[[nodiscard]] std::vector<SstCodeRelocationRecipe>
sst_code_relocation_recipes(const SstCodeForm& form);

// One-byte mappings are intentional. Generated code relocates exact instruction,
// fallthrough, direct-target, PR-base, and PC-relative constants. Unit mappings avoid
// ambiguous overlaps while covering every exact constant emitted for the four-step form.
[[nodiscard]] std::vector<runtime::CodeAddressMapping>
sst_code_address_mappings(const SstTestCase& test, const SstCodeForm& form);

[[nodiscard]] const char* fetch_transition_name(FetchTransition transition) noexcept;

} // namespace katana::testing::sh4_sst
