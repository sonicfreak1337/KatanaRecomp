#include "katana/testing/sh4_sst_codegen.hpp"

#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace katana::testing::sh4_sst {
namespace {

constexpr std::uint32_t canonical_normal_base = 0x10000000u;
constexpr std::uint32_t canonical_external_base = 0x20000000u;
constexpr std::uint32_t canonical_external_stride = 0x00010000u;

struct DirectControl {
    bool direct = false;
    bool delayed = false;
    std::uint32_t target = 0u;
};

std::int32_t sign_extend(const std::uint32_t value, const unsigned bits) noexcept {
    const auto sign = std::uint32_t{1u} << (bits - 1u);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

DirectControl direct_control(const std::uint16_t opcode, const std::uint32_t address) noexcept {
    if ((opcode & 0xF000u) == 0xA000u || (opcode & 0xF000u) == 0xB000u) {
        const auto displacement =
            static_cast<std::uint32_t>(sign_extend(opcode & 0x0FFFu, 12u) * 2);
        return {true, true, address + 4u + displacement};
    }
    const auto high = opcode & 0xFF00u;
    if (high == 0x8900u || high == 0x8B00u || high == 0x8D00u || high == 0x8F00u) {
        const auto displacement = static_cast<std::uint32_t>(sign_extend(opcode & 0x00FFu, 8u) * 2);
        return {true, high == 0x8D00u || high == 0x8F00u, address + 4u + displacement};
    }
    return {};
}

std::optional<std::uint32_t> pc_relative_effective_address(const std::uint16_t opcode,
                                                           const std::uint32_t address) noexcept {
    if ((opcode & 0xF000u) == 0x9000u)
        return address + 4u + static_cast<std::uint32_t>(opcode & 0x00FFu) * 2u;
    if ((opcode & 0xF000u) == 0xD000u || (opcode & 0xFF00u) == 0xC700u)
        return (address & 0xFFFFFFFCu) + 4u + static_cast<std::uint32_t>(opcode & 0x00FFu) * 4u;
    return std::nullopt;
}

std::uint32_t normal_runtime_address(const SstTestCase& test, const std::uint8_t slot) noexcept {
    return test.initial.pc + static_cast<std::uint32_t>(slot) * 2u;
}

std::optional<std::uint8_t> normal_slot_for_runtime_address(const SstTestCase& test,
                                                            const std::uint32_t address) noexcept {
    for (std::uint8_t slot = 0u; slot < normal_code_slot_count; ++slot) {
        if (normal_runtime_address(test, slot) == address) return slot;
    }
    return std::nullopt;
}

std::array<std::optional<std::uint32_t>, maximum_code_slot_count>
canonical_slot_addresses(const SstCodeForm& form) {
    if (form.external_slot_count > maximum_external_code_slots)
        throw SstHarnessInvalid("SST code form contains too many external slots");
    if (form.initial_pc_mod4 > 3u)
        throw SstHarnessInvalid("SST code form has an invalid initial PC alignment");

    std::array<std::optional<std::uint32_t>, maximum_code_slot_count> result{};
    for (std::uint8_t slot = 0u; slot < normal_code_slot_count; ++slot) {
        result[slot] =
            canonical_normal_base + form.initial_pc_mod4 + static_cast<std::uint32_t>(slot) * 2u;
    }

    const auto assign = [&](const std::uint8_t slot, const std::uint32_t value, bool& changed) {
        if (slot >= maximum_code_slot_count)
            throw SstHarnessInvalid("SST code form references an invalid code slot");
        if (result[slot] && *result[slot] != value)
            throw SstHarnessInvalid(
                "SST code form has contradictory canonical-address constraints");
        if (!result[slot]) {
            result[slot] = value;
            changed = true;
        }
    };

    const auto propagate = [&] {
        bool any = false;
        for (;;) {
            bool changed = false;
            for (std::size_t index = 0u; index < form.transitions.size(); ++index) {
                const auto current_slot = form.fetch_slots[index];
                const auto next_slot = form.fetch_slots[index + 1u];
                if (current_slot >= maximum_code_slot_count || next_slot >= maximum_code_slot_count)
                    throw SstHarnessInvalid("SST code form references an invalid fetch slot");

                switch (form.transitions[index]) {
                case FetchTransition::Repeated:
                    if (current_slot != next_slot)
                        throw SstHarnessInvalid(
                            "Repeated SST transition changes normalized code slot");
                    break;
                case FetchTransition::Sequential:
                    if (result[current_slot])
                        assign(next_slot, *result[current_slot] + 2u, changed);
                    if (result[next_slot]) assign(current_slot, *result[next_slot] - 2u, changed);
                    break;
                case FetchTransition::DirectTarget:
                    if (result[current_slot]) {
                        const auto control = direct_control(
                            form.opcodes[current_slot < normal_code_slot_count ? current_slot : 4u],
                            *result[current_slot]);
                        if (!control.direct || control.delayed)
                            throw SstHarnessInvalid(
                                "SST direct-target transition has no matching branch opcode");
                        assign(next_slot, control.target, changed);
                    }
                    break;
                case FetchTransition::DelayedDirectTarget:
                    if (index == 0u)
                        throw SstHarnessInvalid(
                            "SST delayed-target transition has no owner instruction");
                    {
                        const auto owner_slot = form.fetch_slots[index - 1u];
                        if (owner_slot >= maximum_code_slot_count)
                            throw SstHarnessInvalid(
                                "SST delayed-target owner references an invalid slot");
                        if (result[owner_slot]) {
                            const auto control = direct_control(
                                form.opcodes[owner_slot < normal_code_slot_count ? owner_slot : 4u],
                                *result[owner_slot]);
                            if (!control.direct || !control.delayed)
                                throw SstHarnessInvalid(
                                    "SST delayed-target transition has no delayed branch owner");
                            assign(next_slot, control.target, changed);
                        }
                    }
                    break;
                case FetchTransition::DynamicOrOther:
                    break;
                }
            }
            any = any || changed;
            if (!changed) break;
        }
        return any;
    };

    static_cast<void>(propagate());
    for (std::uint8_t external = 0u; external < form.external_slot_count; ++external) {
        const auto slot = static_cast<std::uint8_t>(normal_code_slot_count + external);
        if (result[slot]) continue;
        const auto alignment = form.external_pc_mod4[external];
        if (alignment > 3u)
            throw SstHarnessInvalid("SST external code slot has invalid PC alignment");
        bool assigned = false;
        assign(slot,
               canonical_external_base +
                   static_cast<std::uint32_t>(external) * canonical_external_stride + alignment,
               assigned);
        static_cast<void>(propagate());
    }

    std::map<std::uint32_t, std::uint8_t> owners;
    for (std::uint8_t slot = 0u; slot < normal_code_slot_count + form.external_slot_count; ++slot) {
        if (!result[slot])
            throw SstHarnessInvalid("SST code form left a canonical slot unresolved");
        const auto [found, inserted] = owners.emplace(*result[slot], slot);
        if (!inserted && found->second != slot)
            throw SstHarnessInvalid(
                "SST code form aliases different slots to one canonical address");
    }
    return result;
}

std::array<std::optional<std::uint32_t>, maximum_external_code_slots>
external_runtime_addresses(const SstTestCase& test) {
    std::array<std::optional<std::uint32_t>, maximum_external_code_slots> result{};
    std::size_t count = 0u;
    for (const auto& cycle : test.cycles) {
        if (!cycle.has_fetch())
            throw SstHarnessInvalid("SST code form requires a fetch in every cycle");
        if (normal_slot_for_runtime_address(test, cycle.fetch_address)) continue;
        const auto found = std::find(result.begin(), result.begin() + count, cycle.fetch_address);
        if (found != result.begin() + count) continue;
        if (count == result.size())
            throw SstHarnessInvalid("SST vector contains more than four external code targets");
        result[count++] = cycle.fetch_address;
    }
    return result;
}

std::uint16_t opcode_for_slot(const SstCodeForm& form, const std::uint8_t slot) {
    if (slot < normal_code_slot_count) return form.opcodes[slot];
    if (slot < normal_code_slot_count + form.external_slot_count) return form.opcodes[4u];
    throw SstHarnessInvalid("SST code form requested opcode for an invalid slot");
}

void append_mapping(std::map<std::uint32_t, std::uint32_t>& source_to_runtime,
                    std::map<std::uint32_t, std::uint32_t>& runtime_to_source,
                    const std::uint32_t source,
                    const std::uint32_t runtime) {
    const auto [source_entry, source_inserted] = source_to_runtime.emplace(source, runtime);
    if (!source_inserted && source_entry->second != runtime)
        throw SstHarnessInvalid(
            "SST code form maps one canonical constant to different runtime addresses");
    const auto [runtime_entry, runtime_inserted] = runtime_to_source.emplace(runtime, source);
    if (!runtime_inserted && runtime_entry->second != source)
        throw SstHarnessInvalid(
            "SST code form maps one runtime constant to different canonical addresses");
}

} // namespace

const char* fetch_transition_name(const FetchTransition transition) noexcept {
    switch (transition) {
    case FetchTransition::Repeated:
        return "repeat";
    case FetchTransition::Sequential:
        return "sequential";
    case FetchTransition::DirectTarget:
        return "direct";
    case FetchTransition::DelayedDirectTarget:
        return "delayed-direct";
    case FetchTransition::DynamicOrOther:
        return "dynamic-or-other";
    }
    return "invalid";
}

SstCodeForm make_sst_code_form(const SstTestCase& test) {
    SstCodeForm form;
    form.opcodes = test.opcodes;
    form.initial_pc_mod4 = static_cast<std::uint8_t>(test.initial.pc & 3u);
    form.fpscr_pr = (test.initial.fpscr & runtime::fpscr_pr_mask) != 0u;
    form.fpscr_sz = (test.initial.fpscr & runtime::fpscr_sz_mask) != 0u;
    for (std::uint8_t slot = 0u; slot < normal_code_slot_count; ++slot) {
        const auto wide =
            static_cast<std::uint64_t>(test.initial.pc) + static_cast<std::uint64_t>(slot) * 2u;
        if (wide > std::numeric_limits<std::uint32_t>::max())
            form.normal_pc_wrap_mask |= static_cast<std::uint8_t>(1u << slot);
    }

    const auto external = external_runtime_addresses(test);
    std::uint8_t external_count = 0u;
    while (external_count < external.size() && external[external_count])
        ++external_count;
    form.external_slot_count = external_count;
    for (std::uint8_t index = 0u; index < external_count; ++index)
        form.external_pc_mod4[index] = static_cast<std::uint8_t>(*external[index] & 3u);

    for (std::size_t index = 0u; index < test.cycles.size(); ++index) {
        const auto address = test.cycles[index].fetch_address;
        if (const auto normal = normal_slot_for_runtime_address(test, address)) {
            form.fetch_slots[index] = *normal;
            continue;
        }
        const auto found = std::find(external.begin(), external.begin() + external_count, address);
        if (found == external.begin() + external_count)
            throw SstHarnessInvalid("SST external fetch address lost during normalization");
        form.fetch_slots[index] = static_cast<std::uint8_t>(normal_code_slot_count +
                                                            std::distance(external.begin(), found));
    }

    for (std::size_t index = 0u; index < form.transitions.size(); ++index) {
        const auto current_address = test.cycles[index].fetch_address;
        const auto next_address = test.cycles[index + 1u].fetch_address;
        if (form.fetch_slots[index] == form.fetch_slots[index + 1u]) {
            form.transitions[index] = FetchTransition::Repeated;
            continue;
        }
        if (next_address == current_address + 2u) {
            form.transitions[index] = FetchTransition::Sequential;
            continue;
        }

        const auto current_opcode = opcode_for_slot(form, form.fetch_slots[index]);
        const auto current_control = direct_control(current_opcode, current_address);
        if (current_control.direct && !current_control.delayed &&
            current_control.target == next_address) {
            form.transitions[index] = FetchTransition::DirectTarget;
            continue;
        }
        if (index != 0u) {
            const auto owner_address = test.cycles[index - 1u].fetch_address;
            const auto owner_opcode = opcode_for_slot(form, form.fetch_slots[index - 1u]);
            const auto owner_control = direct_control(owner_opcode, owner_address);
            if (owner_control.direct && owner_control.delayed &&
                owner_control.target == next_address) {
                form.transitions[index] = FetchTransition::DelayedDirectTarget;
                continue;
            }
        }
        form.transitions[index] = FetchTransition::DynamicOrOther;
    }

    // This also validates that the normalized control-flow constraints are
    // self-consistent before the key enters the generator registry.
    static_cast<void>(canonical_slot_addresses(form));
    return form;
}

std::string sst_code_form_key(const SstCodeForm& form) {
    std::ostringstream output;
    output << "sh4-sst-form-v1:p" << form.profile_version << ":op=";
    for (std::size_t index = 0u; index < form.opcodes.size(); ++index) {
        if (index != 0u) output << '.';
        output << std::hex << std::nouppercase << std::setw(4) << std::setfill('0')
               << form.opcodes[index];
    }
    output << std::dec << ":fetch=";
    for (const auto slot : form.fetch_slots)
        output << static_cast<unsigned>(slot);
    output << ":flow=";
    for (const auto transition : form.transitions)
        output << static_cast<unsigned>(transition);
    output << ":pc4=" << static_cast<unsigned>(form.initial_pc_mod4)
           << ":wrap=" << static_cast<unsigned>(form.normal_pc_wrap_mask)
           << ":ext=" << static_cast<unsigned>(form.external_slot_count) << '.';
    for (std::uint8_t index = 0u; index < form.external_slot_count; ++index) {
        if (index != 0u) output << '.';
        output << static_cast<unsigned>(form.external_pc_mod4[index]);
    }
    output << ":pr=" << (form.fpscr_pr ? '1' : '0') << ":sz=" << (form.fpscr_sz ? '1' : '0');
    return output.str();
}

std::string sst_code_form_key(const SstTestCase& test) {
    return sst_code_form_key(make_sst_code_form(test));
}

std::optional<std::uint32_t> canonical_address_for_sst_slot(const SstCodeForm& form,
                                                            const std::uint8_t slot) {
    if (slot >= normal_code_slot_count + form.external_slot_count) return std::nullopt;
    return canonical_slot_addresses(form)[slot];
}

bool sst_code_form_requires_contextual_delay_slot_entries(const SstCodeForm& form) {
    const auto canonical = canonical_slot_addresses(form);
    for (std::size_t index = 0u; index + 1u < form.fetch_slots.size(); ++index) {
        const auto owner_slot = form.fetch_slots[index];
        const auto delay_slot = form.fetch_slots[index + 1u];
        if (owner_slot >= normal_code_slot_count + form.external_slot_count ||
            delay_slot >= normal_code_slot_count + form.external_slot_count)
            throw SstHarnessInvalid("SST contextual-delay scan references an invalid code slot");

        const auto opcode = opcode_for_slot(form, owner_slot);
        const auto decoded = katana::sh4::decode(opcode);
        if (!decoded.has_delay_slot) continue;

        const bool observed_normal_reentry =
            index + 2u < form.fetch_slots.size() && delay_slot == form.fetch_slots[index + 2u];
        const auto control = direct_control(opcode, *canonical[owner_slot]);
        const bool static_normal_entry =
            control.direct && control.delayed && control.target == *canonical[delay_slot];
        if (observed_normal_reentry || static_normal_entry) return true;
    }
    return false;
}

std::optional<std::uint32_t> runtime_address_for_sst_slot(const SstTestCase& test,
                                                          const std::uint8_t slot) {
    if (slot < normal_code_slot_count) return normal_runtime_address(test, slot);
    const auto external_index = static_cast<std::size_t>(slot - normal_code_slot_count);
    const auto external = external_runtime_addresses(test);
    if (external_index >= external.size()) return std::nullopt;
    return external[external_index];
}

std::vector<SstCodeSlotBinding> sst_code_slot_bindings(const SstTestCase& test,
                                                       const SstCodeForm& form) {
    if (make_sst_code_form(test) != form)
        throw SstHarnessInvalid("SST vector does not match the requested code form");
    const auto canonical = canonical_slot_addresses(form);
    std::vector<SstCodeSlotBinding> result;
    result.reserve(normal_code_slot_count + form.external_slot_count);
    for (std::uint8_t slot = 0u; slot < normal_code_slot_count + form.external_slot_count; ++slot) {
        const auto runtime = runtime_address_for_sst_slot(test, slot);
        if (!canonical[slot] || !runtime)
            throw SstHarnessInvalid("SST code-slot binding is incomplete");
        result.push_back({slot, opcode_for_slot(form, slot), *canonical[slot], *runtime});
    }
    return result;
}

std::vector<SstCodeRelocationRecipe> sst_code_relocation_recipes(const SstCodeForm& form) {
    const auto canonical = canonical_slot_addresses(form);
    std::vector<SstCodeRelocationRecipe> result;
    result.reserve(static_cast<std::size_t>(normal_code_slot_count + form.external_slot_count) *
                   5u);

    const auto append = [&](const std::uint32_t source,
                            const std::uint8_t anchor_slot,
                            const std::uint32_t delta) {
        if (anchor_slot >= normal_code_slot_count + form.external_slot_count ||
            !canonical[anchor_slot])
            throw SstHarnessInvalid("SST relocation recipe references an invalid canonical slot");
        if (*canonical[anchor_slot] + delta != source)
            throw SstHarnessInvalid(
                "SST relocation recipe is inconsistent with its canonical anchor");
        const SstCodeRelocationRecipe recipe{source, anchor_slot, delta};
        if (std::find(result.begin(), result.end(), recipe) == result.end())
            result.push_back(recipe);
    };

    for (std::uint8_t slot = 0u; slot < normal_code_slot_count + form.external_slot_count; ++slot) {
        const auto address = *canonical[slot];
        const auto opcode = opcode_for_slot(form, slot);
        append(address, slot, 0u);
        append(address + 2u, slot, 2u);
        append(address + 4u, slot, 4u);

        const auto control = direct_control(opcode, address);
        if (control.direct) append(control.target, slot, control.target - address);

        const auto effective = pc_relative_effective_address(opcode, address);
        if (effective) append(*effective, slot, *effective - address);
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.source != right.source) return left.source < right.source;
        if (left.anchor_slot != right.anchor_slot) return left.anchor_slot < right.anchor_slot;
        return left.delta < right.delta;
    });
    return result;
}

std::vector<runtime::CodeAddressMapping> sst_code_address_mappings(const SstTestCase& test,
                                                                   const SstCodeForm& form) {
    std::map<std::uint32_t, std::uint32_t> source_to_runtime;
    std::map<std::uint32_t, std::uint32_t> runtime_to_source;
    const auto bindings = sst_code_slot_bindings(test, form);
    for (const auto& recipe : sst_code_relocation_recipes(form)) {
        const auto anchor =
            std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
                return binding.slot == recipe.anchor_slot;
            });
        if (anchor == bindings.end())
            throw SstHarnessInvalid("SST relocation recipe has no concrete runtime anchor");
        append_mapping(source_to_runtime,
                       runtime_to_source,
                       recipe.source,
                       anchor->runtime_address + recipe.delta);
    }

    std::vector<runtime::CodeAddressMapping> result;
    result.reserve(source_to_runtime.size());
    for (const auto& [source, runtime_address] : source_to_runtime)
        result.push_back({source, runtime_address, 1u});
    return result;
}

} // namespace katana::testing::sh4_sst
