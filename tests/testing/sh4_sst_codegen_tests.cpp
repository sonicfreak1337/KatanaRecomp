#include "katana/testing/sh4_sst_codegen.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace katana::testing;
using namespace katana::testing::sh4_sst;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SstTestCase branch_fixture(const std::uint32_t base) {
    SstTestCase test;
    test.initial.pc = base;
    test.opcodes = {0x0009u, 0x89B1u, 0x311Cu, 0x0009u, 0x322Cu};
    const auto target = base - 152u;
    const std::array addresses{base, base + 2u, target, target + 2u};
    for (std::size_t index = 0u; index < addresses.size(); ++index) {
        test.cycles[index].actions = SstCycle::fetch_action;
        test.cycles[index].fetch_address = addresses[index];
    }
    return test;
}

bool contains_mapping(const std::vector<katana::runtime::CodeAddressMapping>& mappings,
                      const std::uint32_t source,
                      const std::uint32_t runtime) {
    return std::any_of(mappings.begin(), mappings.end(), [&](const auto& mapping) {
        return mapping.source_start == source && mapping.runtime_start == runtime &&
               mapping.extent == 1u;
    });
}

bool contains_recipe(const std::vector<SstCodeRelocationRecipe>& recipes,
                     const std::uint32_t source,
                     const std::uint8_t anchor_slot,
                     const std::uint32_t delta) {
    return std::find(recipes.begin(),
                     recipes.end(),
                     SstCodeRelocationRecipe{source, anchor_slot, delta}) != recipes.end();
}

} // namespace

int main() {
    try {
        const auto first_test = branch_fixture(0x10001000u);
        const auto second_test = branch_fixture(0x80002000u);
        const auto first = make_sst_code_form(first_test);
        const auto second = make_sst_code_form(second_test);
        require(first == second && sst_code_form_key(first) == sst_code_form_key(second),
                "absolute addresses leaked into the deduplicated SST form");
        require(first.fetch_slots == std::array<std::uint8_t, 4u>{0u, 1u, 4u, 5u} &&
                    first.transitions == std::array{FetchTransition::Sequential,
                                                    FetchTransition::DirectTarget,
                                                    FetchTransition::Sequential},
                "external direct target was not normalized by control-flow shape");

        const auto canonical_target = canonical_address_for_sst_slot(first, 4u);
        const auto canonical_after_target = canonical_address_for_sst_slot(first, 5u);
        require(canonical_target && canonical_after_target &&
                    *canonical_after_target == *canonical_target + 2u,
                "sequential catch-all targets lost their canonical relation");
        const auto canonical_branch_owner = *canonical_address_for_sst_slot(first, 1u);
        const auto branch_recipes = sst_code_relocation_recipes(first);
        require(
            contains_recipe(
                branch_recipes, *canonical_target, 1u, *canonical_target - canonical_branch_owner),
            "direct target was not materialized as an anchored relocation recipe");
        const auto mappings = sst_code_address_mappings(first_test, first);
        require(contains_mapping(mappings, *canonical_target, first_test.cycles[2].fetch_address),
                "canonical branch target has no runtime mapping");

        auto mode_variant = first_test;
        mode_variant.initial.fpscr |= katana::runtime::fpscr_pr_mask;
        require(sst_code_form_key(mode_variant) != sst_code_form_key(first_test),
                "FPSCR.PR is missing from the code-form key");

        auto pc_relative = first_test;
        pc_relative.opcodes = {0x0009u, 0xD001u, 0x311Cu, 0x0009u, 0x322Cu};
        for (std::size_t index = 0u; index < pc_relative.cycles.size(); ++index)
            pc_relative.cycles[index].fetch_address =
                pc_relative.initial.pc + static_cast<std::uint32_t>(index * 2u);
        const auto pc_relative_form = make_sst_code_form(pc_relative);
        const auto pc_relative_mappings = sst_code_address_mappings(pc_relative, pc_relative_form);
        const auto canonical_owner = *canonical_address_for_sst_slot(pc_relative_form, 1u);
        const auto runtime_owner = pc_relative.initial.pc + 2u;
        const auto canonical_effective = (canonical_owner & 0xFFFFFFFCu) + 8u;
        const auto runtime_effective = (runtime_owner & 0xFFFFFFFCu) + 8u;
        const auto pc_relative_recipes = sst_code_relocation_recipes(pc_relative_form);
        require(contains_recipe(pc_relative_recipes,
                                canonical_effective,
                                1u,
                                canonical_effective - canonical_owner),
                "PC-relative constant was not materialized as a relocation recipe");
        require(contains_mapping(pc_relative_mappings, canonical_effective, runtime_effective),
                "PC-relative effective address has no relocation mapping");

        auto odd_reference = branch_fixture(0x10001001u);
        const auto odd_form = make_sst_code_form(odd_reference);
        require(odd_form.initial_pc_mod4 == 1u,
                "unaligned upstream PC could not be represented for N/A reporting");

        std::cout << "SH-4 SST deterministic code-form tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
