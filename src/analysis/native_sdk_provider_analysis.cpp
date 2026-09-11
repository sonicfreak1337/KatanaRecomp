#include "katana/analysis/native_sdk_provider_analysis.hpp"

#include "static_callback_inventory.hpp"
#include "global_entry_predecessor_index.hpp"
#include "jump_table_analysis_internal.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace katana::analysis {
namespace {

using Kind = katana::sh4::InstructionKind;
using Line = katana::sh4::DisassemblyLine;

constexpr std::size_t maximum_provider_instructions = 256u;
constexpr std::size_t maximum_provider_candidates = 1'024u;
constexpr std::size_t maximum_resource_seed_lookback = 48u;
constexpr std::size_t maximum_resource_consumer_instructions = 1'024u;
constexpr std::size_t maximum_resource_evidence_sites = 256u;
constexpr std::uint32_t maximum_resource_descriptor_stride = 4'096u;

[[nodiscard]] bool instruction(
    const Line& line,
    const Kind kind,
    const std::optional<std::uint32_t> destination = std::nullopt,
    const std::optional<std::uint32_t> source = std::nullopt,
    const std::optional<std::int32_t> immediate = std::nullopt,
    const std::optional<std::int32_t> displacement = std::nullopt) noexcept {
    return line.instruction.kind == kind &&
           (!destination.has_value() ||
            line.instruction.destination_register == *destination) &&
           (!source.has_value() ||
            line.instruction.source_register == *source) &&
           (!immediate.has_value() ||
            line.instruction.immediate == *immediate) &&
           (!displacement.has_value() ||
            line.instruction.displacement == *displacement);
}

[[nodiscard]] bool branch_register(
    const Line& line,
    const Kind kind,
    const std::uint8_t reg) noexcept {
    return line.instruction.kind == kind &&
           line.instruction.branch_register == reg;
}

[[nodiscard]] bool contiguous(
    const std::span<const Line> lines) noexcept {
    if (lines.empty()) return false;
    for (std::size_t index = 1u; index < lines.size(); ++index) {
        if (lines[index].address != lines.front().address + index * 2u)
            return false;
    }
    return true;
}

[[nodiscard]] bool bounded_function_frame(
    const std::span<const Line> lines) noexcept {
    if (lines.size() < 8u ||
        lines.size() > maximum_provider_instructions ||
        !contiguous(lines) ||
        !instruction(lines.front(), Kind::MovLongStorePreDecrement, 15u, 14u) ||
        !instruction(lines[lines.size() - 2u], Kind::Rts) ||
        !lines.back().is_delay_slot)
        return false;

    const auto saves_pr = std::any_of(
        lines.begin(), lines.begin() + std::min<std::size_t>(16u, lines.size()),
        [](const auto& line) {
            return line.instruction.kind ==
                       Kind::StoreSpecialRegisterPreDecrement &&
                   line.instruction.special_register ==
                       katana::sh4::SpecialRegister::Pr;
        });
    const auto restores_pr = std::any_of(
        lines.end() - std::min<std::size_t>(16u, lines.size()), lines.end(),
        [](const auto& line) {
            return line.instruction.kind ==
                       Kind::LoadSpecialRegisterPostIncrement &&
                   line.instruction.special_register ==
                       katana::sh4::SpecialRegister::Pr;
        });
    return saves_pr && restores_pr;
}

[[nodiscard]] bool named_texture_archive_load_shape(
    const std::span<const Line> lines) noexcept {
    if (!bounded_function_frame(lines)) return false;

    const auto saves_texlist_argument = std::any_of(
        lines.begin(), lines.end(), [](const auto& line) {
            return instruction(line, Kind::MovLongStore, 15u, 5u);
        });
    const auto indirect_calls = std::count_if(
        lines.begin(), lines.end(), [](const auto& line) {
            return line.instruction.kind == Kind::Jsr;
        });
    const auto direct_calls = std::count_if(
        lines.begin(), lines.end(), [](const auto& line) {
            return line.instruction.kind == Kind::Bsr;
        });
    if (!saves_texlist_argument || indirect_calls == 0u || direct_calls == 0u)
        return false;

    // NJS_TEXNAME is a 12-byte record.  The loader copies the materialized
    // archive record's attr (+4) and resource reference (+8) into each guest
    // texname, advancing one record per bounded texlist count. The reference
    // is classified separately below: it must not be presumed scalar merely
    // because the loader copies it as one word.
    constexpr std::size_t loop_size = 12u;
    for (std::size_t index = 0u; index + loop_size <= lines.size(); ++index) {
        const auto loop = lines.subspan(index, loop_size);
        if (!instruction(loop[0], Kind::MovRegister, 7u, 4u) ||
            !instruction(loop[1], Kind::AddRegister, 7u, 14u) ||
            !instruction(loop[2], Kind::MovLongLoadDisplacement, 2u, 7u,
                         std::nullopt, 8) ||
            !instruction(loop[3], Kind::MovRegister, 0u, 4u) ||
            !instruction(loop[4], Kind::AddImmediate, 5u, std::nullopt, 1) ||
            !instruction(loop[5], Kind::AddRegister, 0u, 1u) ||
            !instruction(loop[6], Kind::CompareGreaterOrEqual, 5u, 6u) ||
            !instruction(loop[7], Kind::MovLongStoreDisplacement, 0u, 2u,
                         std::nullopt, 8) ||
            !instruction(loop[8], Kind::MovLongLoadDisplacement, 3u, 7u,
                         std::nullopt, 4) ||
            !instruction(loop[9], Kind::MovLongStoreDisplacement, 0u, 3u,
                         std::nullopt, 4) ||
            loop[10].instruction.kind != Kind::BfS ||
            loop[10].target_address != loop[0].address ||
            !instruction(loop[11], Kind::AddImmediate, 4u, std::nullopt, 12) ||
            !loop[11].is_delay_slot)
            continue;

        const auto preheader = lines.first(index);
        const auto loads_count = std::any_of(
            preheader.begin(), preheader.end(), [](const auto& line) {
                return instruction(line, Kind::MovLongLoadDisplacement,
                                   6u, 6u, std::nullopt, 4);
            });
        const auto loads_entries = std::any_of(
            preheader.begin(), preheader.end(), [](const auto& line) {
                return instruction(line, Kind::MovLongLoad, 1u, 1u);
            });
        if (loads_count && loads_entries) return true;
    }
    return false;
}

[[nodiscard]] bool texture_archive_release_shape(
    const std::span<const Line> lines) noexcept {
    if (!bounded_function_frame(lines) || lines.size() < 24u ||
        !instruction(lines[1], Kind::MovLongStorePreDecrement, 15u, 13u))
        return false;
    const auto captures_texlist = std::any_of(
        lines.begin(), lines.begin() + std::min<std::size_t>(8u, lines.size()),
        [](const auto& line) {
            return instruction(line, Kind::MovRegister, 13u, 4u);
        });
    const auto initializes_success = std::any_of(
        lines.begin(), lines.end(), [](const auto& line) {
            return instruction(line, Kind::MovImmediate, 12u, std::nullopt, 1);
        });
    const auto initializes_failure = std::any_of(
        lines.begin(), lines.end(), [](const auto& line) {
            return instruction(line, Kind::MovImmediate, 11u, std::nullopt, -1);
        });
    if (!captures_texlist || !initializes_success || !initializes_failure)
        return false;

    constexpr std::size_t loop_size = 17u;
    for (std::size_t index = 0u; index + loop_size <= lines.size(); ++index) {
        const auto loop = lines.subspan(index, loop_size);
        if (!instruction(loop[0], Kind::MovRegister, 4u, 14u) ||
            !instruction(loop[1], Kind::ShiftLogicalLeftOne, 4u) ||
            !instruction(loop[2], Kind::MovRegister, 3u, 14u) ||
            !instruction(loop[3], Kind::MovLongLoad, 2u, 13u) ||
            !instruction(loop[4], Kind::AddRegister, 4u, 3u) ||
            !instruction(loop[5], Kind::ShiftLogicalLeftTwo, 4u) ||
            !instruction(loop[6], Kind::AddRegister, 4u, 2u) ||
            !branch_register(loop[7], Kind::Jsr, 10u) ||
            !instruction(loop[8], Kind::MovLongLoadDisplacement, 4u, 4u,
                         std::nullopt, 8) ||
            !loop[8].is_delay_slot ||
            !instruction(loop[9], Kind::ComparePositiveOrZero, 0u) ||
            loop[10].instruction.kind != Kind::Bt ||
            !instruction(loop[11], Kind::MovRegister, 12u, 11u) ||
            !instruction(loop[12], Kind::MovLongStore, 9u, 14u) ||
            !instruction(loop[13], Kind::AddImmediate, 14u, std::nullopt, 1) ||
            !instruction(loop[14], Kind::MovLongLoadDisplacement, 2u, 13u,
                         std::nullopt, 4) ||
            !instruction(loop[15], Kind::CompareHigherOrSame, 14u, 2u) ||
            loop[16].instruction.kind != Kind::Bf ||
            loop[16].target_address != loop[0].address)
            continue;

        const auto returns_status = std::any_of(
            loop.end(), lines.end(), [](const auto& line) {
                return instruction(line, Kind::MovRegister, 0u, 12u);
            });
        if (returns_status) return true;
    }
    return false;
}

[[nodiscard]] bool synchronous_content_range_read_shape(
    const std::span<const Line> lines) noexcept {
    if (!bounded_function_frame(lines) || lines.size() < 60u)
        return false;

    // Bounded SDK content reads preserve five callee-saved registers, retain
    // the request in r10 and spill the requested unit count and destination
    // across two service/poll loops.  This is intentionally a structural ABI
    // signature; no title address, literal target or device register appears
    // in the recognizer.
    constexpr std::array<std::uint32_t, 5u> saved_registers{
        14u, 13u, 12u, 11u, 10u};
    for (std::size_t index = 0u; index < saved_registers.size(); ++index) {
        if (!instruction(lines[index], Kind::MovLongStorePreDecrement,
                         15u, saved_registers[index]))
            return false;
    }
    if (!instruction(lines[10u], Kind::MovRegister, 10u, 4u))
        return false;

    const auto spills_count = std::any_of(
        lines.begin(), lines.begin() + std::min<std::size_t>(20u, lines.size()),
        [](const auto& line) {
            return instruction(line, Kind::MovLongStore, 15u, 5u);
        });
    const auto spills_destination = std::any_of(
        lines.begin(), lines.begin() + std::min<std::size_t>(20u, lines.size()),
        [](const auto& line) {
            return instruction(line, Kind::MovLongStoreDisplacement,
                               15u, 6u, std::nullopt, 4);
        });
    if (!spills_count || !spills_destination) return false;

    const auto indirect_service_calls = std::count_if(
        lines.begin(), lines.end(), [](const auto& line) {
            return line.instruction.kind == Kind::Jsr;
        });
    const auto direct_service_calls = std::count_if(
        lines.begin(), lines.end(), [](const auto& line) {
            return line.instruction.kind == Kind::Bsr;
        });
    if (indirect_service_calls < 4u || direct_service_calls < 5u)
        return false;

    const auto completion_poll = std::any_of(
        lines.begin(), lines.end(), [](const auto& line) {
            return instruction(line, Kind::CompareEqualImmediate,
                               0u, std::nullopt, 3);
        });
    const auto positive_submission = std::any_of(
        lines.begin(), lines.end(), [](const auto& line) {
            return line.instruction.kind == Kind::ComparePositive;
        });
    const auto backward_retry_edges = std::count_if(
        lines.begin(), lines.end(), [](const auto& line) {
            return (line.instruction.kind == Kind::Bf ||
                    line.instruction.kind == Kind::BfS) &&
                   line.target_address.has_value() &&
                   *line.target_address < line.address;
        });
    const auto bounded_retry_counters = std::count_if(
        lines.begin(), lines.end(), [](const auto& line) {
            return line.instruction.kind == Kind::CompareGreaterOrEqual;
        });
    const auto success_tail = std::any_of(
        lines.end() - std::min<std::size_t>(16u, lines.size()),
        lines.end(), [](const auto& line) {
            return line.instruction.kind == Kind::Bra &&
                   line.target_address.has_value() &&
                   *line.target_address > line.address;
        });
    return completion_poll && positive_submission &&
           backward_retry_edges >= 2u && bounded_retry_counters >= 2u &&
           success_tail;
}

[[nodiscard]] std::optional<std::uint32_t> pc_relative_u32(
    const katana::io::ExecutableImage& image,
    const Line& line) noexcept {
    if (line.instruction.kind != Kind::MovLongLoadPcRelative)
        return std::nullopt;
    const auto literal = ((line.address + 4u) & ~3u) +
                         static_cast<std::uint32_t>(
                             line.instruction.displacement);
    const auto resolved = image.resolve_segment_address(literal, 4u);
    if (!resolved.has_value()) return std::nullopt;
    const auto* segment = image.find_segment(*resolved, 4u);
    if (segment == nullptr) return std::nullopt;
    const auto offset = segment->byte_offset(*resolved);
    if (!offset.has_value() || *offset + 4u > segment->bytes.size())
        return std::nullopt;
    const auto* bytes = segment->bytes.data() + *offset;
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

[[nodiscard]] bool sound_bank_chunk_registration_shape(
    const katana::io::ExecutableImage& image,
    const std::span<const Line> lines) noexcept {
    if (!bounded_function_frame(lines) || lines.size() < 160u ||
        !instruction(lines[1], Kind::MovLongStorePreDecrement, 15u, 13u) ||
        !instruction(lines[2], Kind::MovLongStorePreDecrement, 15u, 12u) ||
        !instruction(lines[3], Kind::MovLongStorePreDecrement, 15u, 11u))
        return false;

    const auto spills_destination_pair =
        std::any_of(lines.begin(), lines.begin() + 20u,
                    [](const auto& line) {
                        return instruction(line, Kind::MovLongStore,
                                           15u, 6u);
                    }) &&
        std::any_of(lines.begin(), lines.begin() + 20u,
                    [](const auto& line) {
                        return instruction(line,
                                           Kind::MovLongStoreDisplacement,
                                           15u, 7u, std::nullopt, 4);
                    });
    const auto sign_extends_bank =
        std::any_of(lines.begin(), lines.end(), [](const auto& line) {
            return instruction(line, Kind::ExtendSignedByte,
                               std::nullopt, 5u);
        });
    if (!spills_destination_pair || !sign_extends_bank) return false;

    // The SDK owner discriminates the semantic Manatee unit families before
    // deriving their bounded bank records.  These are public format FourCCs,
    // not title addresses or content bytes.
    constexpr std::array<std::uint32_t, 7u> unit_magics{
        0x42534D53u, // SMSB
        0x42504D53u, // SMPB
        0x42534F53u, // SOSB
        0x52535053u, // SPSR
        0x42504653u, // SFPB
        0x424F4653u, // SFOB
        0x57504653u, // SFPW
    };
    std::set<std::uint32_t> observed_magics;
    for (const auto& line : lines) {
        const auto value = pc_relative_u32(image, line);
        if (value.has_value() &&
            std::ranges::find(unit_magics, *value) != unit_magics.end())
            observed_magics.insert(*value);
    }
    if (observed_magics.size() != unit_magics.size()) return false;

    const auto transfer_calls = std::count_if(
        lines.begin(), lines.end(), [](const auto& line) {
            return branch_register(line, Kind::Jsr, 3u);
        });
    if (transfer_calls != 2u) return false;

    // After the displaced sound-RAM publication, the owner mirrors the two
    // caller-provided offsets into ordinary title RAM. A native provider must
    // retain these observable stores even though no AICA aperture survives.
    for (std::size_t index = 0u; index + 4u <= lines.size(); ++index) {
        const auto tail = lines.subspan(index, 4u);
        if (instruction(tail[0], Kind::MovLongLoad, 2u, 15u) &&
            instruction(tail[1], Kind::MovLongStore, 14u, 2u) &&
            instruction(tail[2], Kind::MovLongLoadDisplacement,
                        3u, 15u, std::nullopt, 4) &&
            instruction(tail[3], Kind::MovLongStoreDisplacement,
                        14u, 3u, std::nullopt, 4))
            return true;
    }
    return false;
}

[[nodiscard]] bool sound_frame_service_shape(
    const std::span<const Line> lines) noexcept {
    if (!bounded_function_frame(lines) || lines.size() < 80u ||
        lines.size() > 112u)
        return false;

    // A high-level SDK sound server owns one complete title tick. It keeps
    // the seven callee-saved working registers, brackets the fixed services
    // with a shared timer/delta pair, then visits a bounded array of active
    // sound records. This is deliberately a pure SH-4 control/data shape:
    // no title address, SDK symbol, device aperture or literal target is
    // part of the provider proof.
    constexpr std::array<std::uint32_t, 7u> saved_registers{
        14u, 13u, 12u, 11u, 10u, 9u, 8u};
    for (std::size_t index = 0u; index < saved_registers.size(); ++index) {
        if (!instruction(lines[index], Kind::MovLongStorePreDecrement,
                         15u, saved_registers[index]))
            return false;
    }
    if (lines[7u].instruction.kind !=
            Kind::StoreSpecialRegisterPreDecrement ||
        lines[7u].instruction.special_register !=
            katana::sh4::SpecialRegister::Pr)
        return false;

    const auto calls = [&](const std::uint8_t reg) {
        return std::count_if(lines.begin(), lines.end(), [&](const auto& line) {
            return branch_register(line, Kind::Jsr, reg);
        });
    };
    if (calls(9u) != 6u || calls(8u) != 6u || calls(3u) != 4u ||
        calls(10u) != 1u)
        return false;

    const auto loads_shared_service = [&](const std::uint32_t reg) {
        return std::count_if(lines.begin(), lines.end(), [&](const auto& line) {
            return instruction(line, Kind::MovLongLoadPcRelative, reg);
        });
    };
    if (loads_shared_service(9u) != 1u || loads_shared_service(8u) != 1u ||
        loads_shared_service(10u) != 1u)
        return false;

    const auto duration_publications = std::count_if(
        lines.begin(), lines.end(), [](const auto& line) {
            return line.instruction.kind == Kind::MovLongStore &&
                   line.instruction.source_register == 0u;
        });
    if (duration_publications < 6u) return false;

    constexpr std::size_t loop_size = 9u;
    for (std::size_t index = 0u; index + loop_size <= lines.size(); ++index) {
        const auto loop = lines.subspan(index, loop_size);
        if (!instruction(loop[0], Kind::MovRegister, 4u, 12u) ||
            !instruction(loop[1], Kind::MovByteLoad, 0u, 4u) ||
            !instruction(loop[2], Kind::CompareEqualImmediate,
                         0u, std::nullopt, 1) ||
            loop[3].instruction.kind != Kind::Bf ||
            !loop[3].target_address.has_value() ||
            *loop[3].target_address != loop[6].address ||
            !branch_register(loop[4], Kind::Jsr, 10u) ||
            loop[5].instruction.kind != Kind::Nop ||
            !loop[5].is_delay_slot ||
            !instruction(loop[6], Kind::AddImmediate,
                         12u, std::nullopt, 116) ||
            !instruction(loop[7], Kind::CompareHigherOrSame, 12u, 11u) ||
            loop[8].instruction.kind != Kind::Bf ||
            !loop[8].target_address.has_value() ||
            *loop[8].target_address != loop[0].address)
            continue;
        return true;
    }
    return false;
}

[[nodiscard]] std::optional<NativeSdkProviderFamily> provider_family(
    const katana::io::ExecutableImage& image,
    const std::span<const Line> lines) noexcept {
    if (named_texture_archive_load_shape(lines))
        return NativeSdkProviderFamily::NamedTextureArchiveLoad;
    if (texture_archive_release_shape(lines))
        return NativeSdkProviderFamily::TextureArchiveRelease;
    if (synchronous_content_range_read_shape(lines))
        return NativeSdkProviderFamily::SynchronousContentRangeRead;
    if (sound_bank_chunk_registration_shape(image, lines))
        return NativeSdkProviderFamily::SoundBankChunkRegistration;
    if (sound_frame_service_shape(lines))
        return NativeSdkProviderFamily::SoundFrameService;
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> provider_evidence(
    const NativeSdkProviderFamily family) {
    switch (family) {
    case NativeSdkProviderFamily::NamedTextureArchiveLoad:
        return {"bounded-texlist-count-loop",
                "texname-record-stride-12",
                "texname-attr-offset-4",
                "texname-resource-reference-offset-8",
                "r4-name-r5-texlist-call-shape"};
    case NativeSdkProviderFamily::TextureArchiveRelease:
        return {"bounded-texlist-count-loop",
                "texname-record-stride-12",
                "texname-resource-reference-offset-8",
                "aggregate-release-status"};
    case NativeSdkProviderFamily::SynchronousContentRangeRead:
        return {"request-count-and-destination-spill",
                "bounded-submit-retry-loop",
                "bounded-completion-poll-loop",
                "completion-status-three",
                "completed-unit-count-return"};
    case NativeSdkProviderFamily::SoundBankChunkRegistration:
        return {"seven-manatee-unit-fourccs",
                "signed-bounded-bank-index",
                "paired-sound-resource-publication",
                "ordinary-title-ram-offset-mirror",
                "aggregate-sdk-status-return"};
    case NativeSdkProviderFamily::SoundFrameService:
        return {"seven-callee-saved-working-registers",
                "shared-timer-delta-service-brackets",
                "four-fixed-sound-services",
                "bounded-active-record-loop-stride-116",
                "ordinary-title-ram-duration-publication"};
    }
    return {};
}

[[nodiscard]] bool line_range_is_contiguous(
    const std::span<const Line> lines,
    const std::size_t begin,
    const std::size_t end) noexcept {
    if (begin >= end || end > lines.size()) return false;
    for (std::size_t index = begin + 1u; index < end; ++index) {
        if (lines[index].address != lines[index - 1u].address + 2u)
            return false;
    }
    return true;
}

[[nodiscard]] bool selected_record_stride_12(
    const std::span<const Line> lines,
    const std::size_t field_load_index) noexcept {
    if (field_load_index >= lines.size()) return false;
    const auto& field_load = lines[field_load_index];
    if (!instruction(field_load, Kind::MovLongLoadDisplacement,
                     std::nullopt, std::nullopt, std::nullopt, 8))
        return false;
    const auto record = field_load.instruction.source_register;
    auto begin = field_load_index > maximum_resource_seed_lookback
                     ? field_load_index - maximum_resource_seed_lookback
                     : 0u;
    if (!line_range_is_contiguous(lines, begin, field_load_index + 1u))
        return false;
    // Never borrow a stride-building sequence from an adjacent owner. The
    // delay slot belongs to the preceding RTS, so the next eligible scan
    // position begins two instructions later.
    for (std::size_t index = begin; index + 1u < field_load_index; ++index) {
        if (instruction(lines[index], Kind::Rts) &&
            lines[index + 1u].is_delay_slot)
            begin = index + 2u;
    }

    // Canonical SH-4 12-byte record selection:
    //   tmp=index; shll tmp; aux=index; add aux,tmp; shll2 tmp;
    //   base=*table; add base,tmp; value=*(tmp+8)
    // Interleaved independent instructions are allowed, but every defining
    // step and register relation is exact and bounded.
    for (std::size_t move = begin; move < field_load_index; ++move) {
        if (!instruction(lines[move], Kind::MovRegister, record)) continue;
        const auto index_register = lines[move].instruction.source_register;
        auto shll = move + 1u;
        while (shll < field_load_index &&
               !instruction(lines[shll], Kind::ShiftLogicalLeftOne, record))
            ++shll;
        if (shll == field_load_index) continue;

        for (std::size_t aux_move = shll + 1u;
             aux_move < field_load_index; ++aux_move) {
            if (!instruction(lines[aux_move], Kind::MovRegister,
                             std::nullopt, index_register))
                continue;
            const auto auxiliary =
                lines[aux_move].instruction.destination_register;
            auto add_index = aux_move + 1u;
            while (add_index < field_load_index &&
                   !instruction(lines[add_index], Kind::AddRegister,
                                record, auxiliary))
                ++add_index;
            if (add_index == field_load_index) continue;
            auto shll2 = add_index + 1u;
            while (shll2 < field_load_index &&
                   !instruction(lines[shll2], Kind::ShiftLogicalLeftTwo,
                                record))
                ++shll2;
            if (shll2 == field_load_index) continue;

            for (std::size_t base_load = shll2 + 1u;
                 base_load < field_load_index; ++base_load) {
                const auto kind = lines[base_load].instruction.kind;
                if (kind != Kind::MovLongLoad &&
                    kind != Kind::MovLongLoadDisplacement &&
                    kind != Kind::MovLongLoadPcRelative)
                    continue;
                const auto base =
                    lines[base_load].instruction.destination_register;
                const auto adds_base = std::any_of(
                    lines.begin() + static_cast<std::ptrdiff_t>(base_load + 1u),
                    lines.begin() + static_cast<std::ptrdiff_t>(field_load_index),
                    [&](const auto& line) {
                        return instruction(line, Kind::AddRegister, record,
                                           base);
                    });
                if (adds_base) return true;
            }
        }
    }
    return false;
}

[[nodiscard]] std::set<std::uint32_t> indexed_record_strides(
    const std::span<const Line> lines) {
    std::set<std::uint32_t> result;
    // Compute the coefficient instead of matching a literal stride. This
    // recognizes index*4*4, +index, *4 as 68 while retaining the exact SH-4
    // dataflow proof and naturally generalizing to other SDK record widths.
    for (std::size_t index = 0u; index + 8u <= lines.size(); ++index) {
        const auto sequence = lines.subspan(index, 8u);
        if (!contiguous(sequence) ||
            !instruction(sequence[0], Kind::MovRegister))
            continue;
        const auto temporary =
            sequence[0].instruction.destination_register;
        const auto source = sequence[0].instruction.source_register;
        if (!instruction(sequence[1], Kind::ShiftLogicalLeftTwo, temporary) ||
            !instruction(sequence[2], Kind::ShiftLogicalLeftTwo, temporary) ||
            !instruction(sequence[3], Kind::MovRegister, std::nullopt,
                         source))
            continue;
        const auto auxiliary =
            sequence[3].instruction.destination_register;
        if (!instruction(sequence[4], Kind::AddRegister, temporary,
                         auxiliary) ||
            !instruction(sequence[5], Kind::ShiftLogicalLeftTwo, temporary) ||
            !instruction(sequence[6], Kind::AddRegister, temporary) ||
            !instruction(sequence[7], Kind::MovLongLoad, std::nullopt,
                         temporary))
            continue;
        constexpr std::uint32_t coefficient = ((1u << 2u) << 2u) + 1u;
        constexpr std::uint32_t stride = coefficient << 2u;
        static_assert(stride <= maximum_resource_descriptor_stride);
        result.insert(stride);
    }
    return result;
}

[[nodiscard]] bool writes_general_register(const Kind kind) noexcept {
    switch (kind) {
    case Kind::MovImmediate:
    case Kind::MovRegister:
    case Kind::AddImmediate:
    case Kind::AddRegister:
    case Kind::SubRegister:
    case Kind::NegateRegister:
    case Kind::NotRegister:
    case Kind::AddWithCarry:
    case Kind::AddWithOverflow:
    case Kind::SubWithCarry:
    case Kind::SubWithOverflow:
    case Kind::NegateWithCarry:
    case Kind::ExtendUnsignedByte:
    case Kind::ExtendUnsignedWord:
    case Kind::ExtendSignedByte:
    case Kind::ExtendSignedWord:
    case Kind::SwapBytes:
    case Kind::SwapWords:
    case Kind::ExtractMiddle:
    case Kind::MoveT:
    case Kind::ShiftLogicalLeftOne:
    case Kind::ShiftLogicalRightOne:
    case Kind::ShiftArithmeticLeftOne:
    case Kind::ShiftArithmeticRightOne:
    case Kind::ShiftLogicalLeftTwo:
    case Kind::ShiftLogicalLeftEight:
    case Kind::ShiftLogicalLeftSixteen:
    case Kind::ShiftLogicalRightTwo:
    case Kind::ShiftLogicalRightEight:
    case Kind::ShiftLogicalRightSixteen:
    case Kind::RotateLeft:
    case Kind::RotateRight:
    case Kind::RotateLeftThroughT:
    case Kind::RotateRightThroughT:
    case Kind::ShiftArithmeticDynamic:
    case Kind::ShiftLogicalDynamic:
    case Kind::AndRegister:
    case Kind::OrRegister:
    case Kind::XorRegister:
    case Kind::AndImmediate:
    case Kind::OrImmediate:
    case Kind::XorImmediate:
    case Kind::MovByteLoad:
    case Kind::MovWordLoad:
    case Kind::MovLongLoad:
    case Kind::MovByteLoadPostIncrement:
    case Kind::MovWordLoadPostIncrement:
    case Kind::MovLongLoadPostIncrement:
    case Kind::MovByteLoadDisplacement:
    case Kind::MovWordLoadDisplacement:
    case Kind::MovLongLoadDisplacement:
    case Kind::MovByteLoadR0Indexed:
    case Kind::MovWordLoadR0Indexed:
    case Kind::MovLongLoadR0Indexed:
    case Kind::MovByteLoadGbrDisplacement:
    case Kind::MovWordLoadGbrDisplacement:
    case Kind::MovLongLoadGbrDisplacement:
    case Kind::MovWordLoadPcRelative:
    case Kind::MovLongLoadPcRelative:
    case Kind::MoveAddressPcRelative:
    case Kind::StoreSpecialRegister:
        return true;
    default:
        return false;
    }
}

struct ResourceAccessInventory final {
    std::set<std::uint32_t> reads;
    std::set<std::uint32_t> writes;
    std::set<std::uint32_t> sites;
    std::uint32_t minimum_bytes = 0u;
};

void record_resource_access(ResourceAccessInventory& inventory,
                            const std::int64_t offset,
                            const std::uint32_t width,
                            const std::uint32_t site,
                            const bool write) {
    if (offset < 0 ||
        static_cast<std::uint64_t>(offset) + width >
            maximum_resource_descriptor_stride)
        return;
    const auto bounded = static_cast<std::uint32_t>(offset);
    (write ? inventory.writes : inventory.reads).insert(bounded);
    if (inventory.sites.size() < maximum_resource_evidence_sites)
        inventory.sites.insert(site);
    inventory.minimum_bytes = std::max(
        inventory.minimum_bytes, bounded + width);
}

void trace_resource_pointer(
    ResourceAccessInventory& inventory,
    const std::span<const Line> lines,
    const std::size_t seed_index) {
    std::array<std::optional<std::int64_t>, 16u> pointer_offsets{};
    const auto seed_register =
        lines[seed_index].instruction.destination_register;
    pointer_offsets[seed_register] = 0;
    if (inventory.sites.size() < maximum_resource_evidence_sites)
        inventory.sites.insert(lines[seed_index].address);

    bool call_clobber_after_delay = false;
    bool stop_after_delay = false;
    const auto end = std::min(lines.size(),
                              seed_index + 1u +
                                  maximum_resource_consumer_instructions);
    for (std::size_t index = seed_index + 1u; index < end; ++index) {
        if (lines[index].address != lines[index - 1u].address + 2u) break;
        const auto& line = lines[index];
        const auto& decoded = line.instruction;

        const auto read_access = [&](const std::uint8_t base,
                                     const std::int32_t displacement,
                                     const std::uint32_t width) {
            if (pointer_offsets[base].has_value())
                record_resource_access(
                    inventory, *pointer_offsets[base] + displacement,
                    width, line.address, false);
        };
        const auto write_access = [&](const std::uint8_t base,
                                      const std::int32_t displacement,
                                      const std::uint32_t width) {
            if (pointer_offsets[base].has_value())
                record_resource_access(
                    inventory, *pointer_offsets[base] + displacement,
                    width, line.address, true);
        };

        switch (decoded.kind) {
        case Kind::MovRegister:
            pointer_offsets[decoded.destination_register] =
                pointer_offsets[decoded.source_register];
            break;
        case Kind::AddImmediate:
            if (pointer_offsets[decoded.destination_register].has_value())
                *pointer_offsets[decoded.destination_register] +=
                    decoded.immediate;
            break;
        case Kind::MovByteLoad:
            read_access(decoded.source_register, 0, 1u);
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovWordLoad:
            read_access(decoded.source_register, 0, 2u);
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovLongLoad:
            read_access(decoded.source_register, 0, 4u);
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovByteLoadDisplacement:
            read_access(decoded.source_register, decoded.displacement, 1u);
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovWordLoadDisplacement:
            read_access(decoded.source_register, decoded.displacement, 2u);
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovLongLoadDisplacement:
            read_access(decoded.source_register, decoded.displacement, 4u);
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovByteLoadPostIncrement:
            read_access(decoded.source_register, 0, 1u);
            if (decoded.source_register != decoded.destination_register &&
                pointer_offsets[decoded.source_register].has_value())
                ++*pointer_offsets[decoded.source_register];
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovWordLoadPostIncrement:
            read_access(decoded.source_register, 0, 2u);
            if (decoded.source_register != decoded.destination_register &&
                pointer_offsets[decoded.source_register].has_value())
                *pointer_offsets[decoded.source_register] += 2;
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovLongLoadPostIncrement:
            read_access(decoded.source_register, 0, 4u);
            if (decoded.source_register != decoded.destination_register &&
                pointer_offsets[decoded.source_register].has_value())
                *pointer_offsets[decoded.source_register] += 4;
            pointer_offsets[decoded.destination_register].reset();
            break;
        case Kind::MovByteStore:
            write_access(decoded.destination_register, 0, 1u);
            break;
        case Kind::MovWordStore:
            write_access(decoded.destination_register, 0, 2u);
            break;
        case Kind::MovLongStore:
            write_access(decoded.destination_register, 0, 4u);
            break;
        case Kind::MovByteStoreDisplacement:
            write_access(decoded.destination_register, decoded.displacement,
                         1u);
            break;
        case Kind::MovWordStoreDisplacement:
            write_access(decoded.destination_register, decoded.displacement,
                         2u);
            break;
        case Kind::MovLongStoreDisplacement:
            write_access(decoded.destination_register, decoded.displacement,
                         4u);
            break;
        case Kind::MovByteStorePreDecrement:
            if (pointer_offsets[decoded.destination_register].has_value())
                --*pointer_offsets[decoded.destination_register];
            write_access(decoded.destination_register, 0, 1u);
            break;
        case Kind::MovWordStorePreDecrement:
            if (pointer_offsets[decoded.destination_register].has_value())
                *pointer_offsets[decoded.destination_register] -= 2;
            write_access(decoded.destination_register, 0, 2u);
            break;
        case Kind::MovLongStorePreDecrement:
            if (pointer_offsets[decoded.destination_register].has_value())
                *pointer_offsets[decoded.destination_register] -= 4;
            write_access(decoded.destination_register, 0, 4u);
            break;
        case Kind::AddRegister:
            // Pointer + non-constant register is no longer an exact bounded
            // descriptor offset. Pointer + pointer is equally unsupported.
            if (pointer_offsets[decoded.destination_register].has_value() ||
                pointer_offsets[decoded.source_register].has_value())
                pointer_offsets[decoded.destination_register].reset();
            break;
        default:
            if (writes_general_register(decoded.kind))
                pointer_offsets[decoded.destination_register].reset();
            break;
        }

        if (call_clobber_after_delay) {
            for (std::size_t reg = 0u; reg <= 7u; ++reg)
                pointer_offsets[reg].reset();
            call_clobber_after_delay = false;
        }
        if (stop_after_delay) break;
        if (decoded.kind == Kind::Jsr || decoded.kind == Kind::Bsr ||
            decoded.kind == Kind::Bsrf)
            call_clobber_after_delay = true;
        if (decoded.kind == Kind::Rts) stop_after_delay = true;
    }
}

[[nodiscard]] std::optional<NativeSdkResourceReferenceContract>
discover_texture_resource_reference(
    const std::span<const Line> lines) {
    ResourceAccessInventory inventory;
    for (std::size_t index = 0u; index < lines.size(); ++index) {
        if (!selected_record_stride_12(lines, index)) continue;
        trace_resource_pointer(inventory, lines, index);
    }
    if (inventory.reads.empty() && inventory.writes.empty())
        return std::nullopt;

    const auto strides = indexed_record_strides(lines);
    const auto stride = std::find_if(
        strides.begin(), strides.end(), [&](const auto value) {
            return value >= inventory.minimum_bytes;
        });
    if (stride == strides.end()) return std::nullopt;

    NativeSdkResourceReferenceContract contract;
    contract.owner_record_stride = 12u;
    contract.reference_field_offset = 8u;
    contract.descriptor_stride = *stride;
    contract.minimum_descriptor_bytes = inventory.minimum_bytes;
    contract.observed_read_offsets.assign(inventory.reads.begin(),
                                          inventory.reads.end());
    contract.observed_write_offsets.assign(inventory.writes.begin(),
                                           inventory.writes.end());
    contract.evidence_sites.assign(inventory.sites.begin(),
                                   inventory.sites.end());
    return contract;
}

void bind_resource_reference(
    std::vector<NativeSdkProviderCandidate>& candidates,
    const std::span<const Line> lines) {
    const auto resource = discover_texture_resource_reference(lines);
    if (!resource.has_value()) return;
    for (auto& candidate : candidates) {
        switch (candidate.family) {
        case NativeSdkProviderFamily::NamedTextureArchiveLoad:
        case NativeSdkProviderFamily::TextureArchiveRelease:
            candidate.resource_reference = resource;
            candidate.evidence.push_back(
                "texname-reference-is-guest-descriptor-pointer");
            break;
        case NativeSdkProviderFamily::SynchronousContentRangeRead:
        case NativeSdkProviderFamily::SoundBankChunkRegistration:
        case NativeSdkProviderFamily::SoundFrameService:
            break;
        }
    }
}

[[nodiscard]] std::optional<std::string> code_identity(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address,
    const std::uint32_t size) {
    const auto resolved = image.resolve_segment_address(address, size);
    if (!resolved.has_value()) return std::nullopt;
    const auto* segment = image.find_segment(*resolved, size);
    if (segment == nullptr) return std::nullopt;
    const auto offset = segment->byte_offset(*resolved);
    if (!offset.has_value() ||
        *offset > segment->bytes.size() ||
        size > segment->bytes.size() - *offset)
        return std::nullopt;
    const auto* data = reinterpret_cast<const char*>(
        segment->bytes.data() + *offset);
    return "sha256:" + katana::io::sha256_bytes(
        std::string_view(data, size));
}

void append_candidate(
    std::vector<NativeSdkProviderCandidate>& result,
    const katana::io::ExecutableImage& image,
    const std::span<const Line> lines) {
    const auto family = provider_family(image, lines);
    if (!family.has_value()) return;
    const auto byte_size = lines.size() * 2u;
    if (byte_size > std::numeric_limits<std::uint32_t>::max()) return;
    const auto identity = code_identity(
        image, lines.front().address,
        static_cast<std::uint32_t>(byte_size));
    if (!identity.has_value()) return;
    result.push_back(
        {*family,
         lines.front().address,
         static_cast<std::uint32_t>(byte_size),
         *identity,
         NativeSdkProviderBoundaryProof::StructuralPrologueReturn,
         std::nullopt,
         provider_evidence(*family)});
    if (result.size() > maximum_provider_candidates)
        throw std::runtime_error(
            "Native SDK provider candidate budget exceeded.");
}

// Exact register/stack flow of a small staged transform wrapper. The concrete
// buffer, all callees and literal-pool offsets are derived from the source;
// there are no title addresses, file names or SDK symbol guesses here.
[[nodiscard]] std::optional<NativeStagedTransformCandidate>
staged_transform_candidate(
    const katana::io::ExecutableImage& image,
    const std::span<const Line> lines) {
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 24u> shape{{
        {0xffffu, 0x2fe6u}, {0xffffu, 0x4f22u}, // preserve r14/PR
        {0xffffu, 0x7ffcu}, {0xffffu, 0x2f52u}, // spill original r5
        {0xff00u, 0xd500u}, {0xf000u, 0xb000u}, {0xffffu, 0x0009u},
        {0xffffu, 0x6e03u}, {0xffffu, 0x4e11u}, {0xffffu, 0x8b03u},
        {0xff00u, 0xd300u}, {0xff00u, 0xd400u},
        {0xffffu, 0x430bu}, {0xffffu, 0x65f2u}, // restore r5 in delay slot
        {0xff00u, 0xd200u}, {0xff00u, 0xd300u},
        {0xffffu, 0x223bu}, {0xffffu, 0x420bu}, {0xffffu, 0x0009u},
        {0xffffu, 0x60e3u}, {0xffffu, 0x7f04u},
        {0xffffu, 0x4f26u}, {0xffffu, 0x000bu}, {0xffffu, 0x6ef6u}
    }};
    if (lines.size() != shape.size() || !contiguous(lines) ||
        (lines.front().address & 1u) != 0u ||
        lines.front().address > std::numeric_limits<std::uint32_t>::max() - 48u)
        return std::nullopt;
    for (std::size_t i = 0u; i < shape.size(); ++i) {
        if ((lines[i].opcode & shape[i].first) != shape[i].second)
            return std::nullopt;
        const auto resolved = image.resolve_segment_address(lines[i].address, 2u);
        if (!resolved.has_value()) return std::nullopt;
        const auto* segment = image.find_segment(*resolved, 2u);
        if (segment == nullptr || !segment->permissions.executable)
            return std::nullopt;
        const auto offset = segment->byte_offset(*resolved);
        if (!offset.has_value() || *offset > segment->bytes.size() ||
            segment->bytes.size() - *offset < 2u ||
            katana::io::read_u16_le(segment->bytes, *offset) != lines[i].opcode)
            return std::nullopt;
    }
    const auto identity = code_identity(image, lines.front().address, 48u);
    if (!identity.has_value()) return std::nullopt;
    NativeStagedTransformCandidate result;
    result.entry_address = lines.front().address;
    result.covered_size = 48u;
    result.code_identity = *identity;
    for (const auto index : {4u, 10u, 11u, 14u, 15u}) {
        const auto& line = lines[index];
        const auto expanded = (static_cast<std::uint64_t>(line.address) + 4u) & ~3ull;
        const auto address = expanded + (line.opcode & 0xffu) * 4u;
        if (address > std::numeric_limits<std::uint32_t>::max() - 3u)
            return std::nullopt;
        const auto literal_address = static_cast<std::uint32_t>(address);
        const auto resolved = image.resolve_segment_address(literal_address, 4u);
        if (!resolved.has_value()) return std::nullopt;
        const auto* segment = image.find_segment(*resolved, 4u);
        if (segment == nullptr || !segment->permissions.readable)
            return std::nullopt;
        const auto offset = segment->byte_offset(*resolved);
        const auto literal_identity = code_identity(image, literal_address, 4u);
        if (!offset.has_value() || *offset > segment->bytes.size() ||
            segment->bytes.size() - *offset < 4u || !literal_identity.has_value())
            return std::nullopt;
        const auto value = static_cast<std::uint32_t>(
            katana::io::read_u16_le(segment->bytes, *offset)) |
            (static_cast<std::uint32_t>(
                katana::io::read_u16_le(segment->bytes, *offset + 2u)) << 16u);
        result.literals.push_back({line.address, literal_address, value, *literal_identity});
    }
    // Require exact staging-address equality. Alias equivalence, buffer
    // mutability and content extents are separate contracts, not guesses.
    if (result.literals[0u].value == 0u ||
        result.literals[0u].value != result.literals[2u].value)
        return std::nullopt;
    result.staging_buffer_address = result.literals[0u].value;
    result.stage_call_address = lines[5u].address;
    const auto call = katana::sh4::decode(lines[5u].opcode);
    const auto target = katana::sh4::calculate_direct_branch_target(
        call, lines[5u].address);
    if (!target.has_value()) return std::nullopt;
    result.stage_target_address = *target;
    result.transform_call_address = lines[12u].address;
    result.transform_target_address = result.literals[1u].value;
    result.finalizer_call_address = lines[17u].address;
    result.finalizer_target_address =
        result.literals[3u].value | result.literals[4u].value;
    if (result.transform_target_address == 0u ||
        (result.transform_target_address & 1u) != 0u ||
        result.finalizer_target_address == 0u ||
        (result.finalizer_target_address & 1u) != 0u)
        return std::nullopt;
    return result;
}

void canonicalize(std::vector<NativeSdkProviderCandidate>& result) {
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.entry_address, left.family, left.covered_size} <
               std::tuple{right.entry_address, right.family, right.covered_size};
    });
    result.erase(
        std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.entry_address == right.entry_address &&
                   left.family == right.family &&
                   left.covered_size == right.covered_size;
        }),
        result.end());
}

[[nodiscard]] std::vector<Line> decode_window(
    const std::span<const std::uint8_t> bytes,
    const std::size_t start_offset,
    const std::uint32_t base_address) {
    std::vector<Line> lines;
    lines.reserve(maximum_provider_instructions);
    bool delay_slot = false;
    for (std::size_t index = 0u;
         index < maximum_provider_instructions &&
         start_offset + index * 2u + 2u <= bytes.size();
         ++index) {
        const auto expanded = static_cast<std::uint64_t>(base_address) +
                              start_offset + index * 2u;
        if (expanded > std::numeric_limits<std::uint32_t>::max()) break;
        Line line;
        line.address = static_cast<std::uint32_t>(expanded);
        line.opcode = katana::io::read_u16_le(
            bytes, start_offset + index * 2u);
        line.instruction = katana::sh4::decode(line.opcode);
        line.is_delay_slot = delay_slot;
        line.target_address = katana::sh4::calculate_direct_branch_target(
            line.instruction, line.address);
        delay_slot = line.instruction.has_delay_slot;
        lines.push_back(std::move(line));
    }
    return lines;
}

void scan_decoded_window(
    std::vector<NativeSdkProviderCandidate>& result,
    const katana::io::ExecutableImage& image,
    const std::span<const Line> window) {
    for (std::size_t index = 6u; index + 1u < window.size(); ++index) {
        if (!instruction(window[index], Kind::Rts)) continue;
        append_candidate(result, image, window.first(index + 2u));
        // A structural prologue may never claim code beyond its first
        // concrete return.  Continuing the linear scan would concatenate the
        // next adjacent function and let its provider-shaped loop confer a
        // false boundary on the preceding owner.
        break;
    }
}

[[nodiscard]] std::vector<Line> disassemble_executable_segments(
    const katana::io::ExecutableImage& image) {
    std::vector<Line> result;
    for (const auto& segment : image.segments()) {
        if (!segment.permissions.executable || segment.bytes.empty())
            continue;
        auto lines = katana::sh4::disassemble(segment.bytes,
                                              segment.virtual_address);
        result.insert(result.end(), std::make_move_iterator(lines.begin()),
                      std::make_move_iterator(lines.end()));
    }
    std::sort(result.begin(), result.end(), [](const auto& left,
                                                const auto& right) {
        return left.address < right.address;
    });
    return result;
}

} // namespace

std::optional<NativePrsTransformContract> recognize_native_prs_transform(
    const katana::io::ExecutableImage& image,
    const std::uint32_t entry_address) {
    constexpr std::uint32_t body_bytes = 152u;
    if ((entry_address & 1u) != 0u ||
        entry_address > std::numeric_limits<std::uint32_t>::max() - body_bytes)
        return std::nullopt;
    const auto resolved = image.resolve_segment_address(entry_address, body_bytes);
    if (!resolved.has_value()) return std::nullopt;
    const auto* segment = image.find_segment(*resolved, body_bytes);
    if (segment == nullptr || !segment->permissions.executable ||
        !segment->permissions.readable)
        return std::nullopt;
    const auto offset = segment->byte_offset(*resolved);
    if (!offset.has_value() || *offset > segment->bytes.size() ||
        segment->bytes.size() - *offset < body_bytes)
        return std::nullopt;
    const auto lines = katana::sh4::disassemble(
        std::span<const std::uint8_t>(segment->bytes).subspan(*offset, body_bytes),
        entry_address);
    if (lines.size() != body_bytes / 2u) return std::nullopt;

    // Bind semantic roles, allowing register allocation to change. r0 is
    // fixed by AND-immediate/return and r15 by the ordinary stack contract.
    std::array<std::uint32_t, 16u> reg{};
    reg[0u] = 0u;
    reg[1u] = lines[10u].instruction.destination_register; // copied byte
    reg[2u] = lines[41u].instruction.destination_register; // backward source
    reg[3u] = lines[7u].instruction.destination_register;  // original output
    reg[4u] = lines[4u].instruction.source_register;       // encoded input
    reg[5u] = lines[7u].instruction.source_register;       // decoded output
    reg[6u] = lines[4u].instruction.destination_register;  // control bits
    reg[7u] = lines[5u].instruction.destination_register;  // remaining bits
    reg[8u] = lines[1u].instruction.source_register;       // 13-bit mask
    reg[9u] = lines[0u].instruction.source_register;       // 8-bit mask
    reg[15u] = 15u;
    std::uint16_t used = 1u << 15u;
    for (std::size_t role = 0u; role < 10u; ++role) {
        if (reg[role] >= 15u || (used & (1u << reg[role])) != 0u)
            return std::nullopt;
        used = static_cast<std::uint16_t>(used | (1u << reg[role]));
    }
    const auto op = [&](const std::size_t i, const Kind kind,
                        const std::optional<unsigned> destination = std::nullopt,
                        const std::optional<unsigned> source = std::nullopt,
                        const std::optional<std::int32_t> immediate = std::nullopt) {
        return instruction(lines[i], kind,
            destination ? std::optional{reg[*destination]} : std::nullopt,
            source ? std::optional{reg[*source]} : std::nullopt, immediate);
    };
    const auto branch = [&](const std::size_t i, const Kind kind,
                            const std::size_t target) {
        return op(i, kind) &&
            lines[i].target_address == entry_address + target * 2u;
    };
    // Four inlined bit reads. DT's branch decision precedes its SHLR delay
    // slot; the following BT consumes SHLR's T (the low control bit).
    const auto bit = [&](const std::size_t i) {
        return op(i, Kind::DecrementAndTest, 7u) &&
            branch(i + 1u, Kind::BfS, i + 6u) &&
            op(i + 2u, Kind::ShiftLogicalRightOne, 6u) &&
            op(i + 3u, Kind::MovByteLoadPostIncrement, 6u, 4u) &&
            op(i + 4u, Kind::MovImmediate, 7u, std::nullopt, 8) &&
            op(i + 5u, Kind::ShiftLogicalRightOne, 6u);
    };
    if (!op(0u, Kind::MovLongStorePreDecrement, 15u, 9u) ||
        !op(1u, Kind::MovLongStorePreDecrement, 15u, 8u) ||
        !op(2u, Kind::MovWordLoadPcRelative, 9u) ||
        !op(3u, Kind::MovWordLoadPcRelative, 8u) ||
        !op(4u, Kind::MovByteLoadPostIncrement, 6u, 4u) ||
        !op(5u, Kind::MovImmediate, 7u, std::nullopt, 9) ||
        !branch(6u, Kind::Bra, 13u) ||
        !op(7u, Kind::MovRegister, 3u, 5u) ||
        // Words 8/9 are an unreachable data island, not instructions.
        !op(10u, Kind::MovByteLoadPostIncrement, 1u, 4u) ||
        !op(11u, Kind::MovByteStore, 5u, 1u) ||
        !op(12u, Kind::AddImmediate, 5u, std::nullopt, 1) ||
        !bit(13u) || !branch(19u, Kind::Bt, 10u) ||
        !bit(20u) || !branch(26u, Kind::Bt, 53u) ||
        !op(27u, Kind::MovImmediate, 0u, std::nullopt, 0) ||
        !bit(28u) || !op(34u, Kind::RotateLeftThroughT, 0u) ||
        !bit(35u) || !op(41u, Kind::MovByteLoadPostIncrement, 2u, 4u) ||
        !op(42u, Kind::RotateLeftThroughT, 0u) ||
        !op(43u, Kind::OrRegister, 2u, 9u) ||
        !op(44u, Kind::AddImmediate, 0u, std::nullopt, 2) ||
        !op(45u, Kind::AddRegister, 2u, 5u) ||
        // Forward byte copies deliberately permit overlapping backreferences.
        !op(46u, Kind::MovByteLoadPostIncrement, 1u, 2u) ||
        !op(47u, Kind::DecrementAndTest, 0u) ||
        !op(48u, Kind::MovByteStore, 5u, 1u) ||
        !branch(49u, Kind::BfS, 46u) ||
        !op(50u, Kind::AddImmediate, 5u, std::nullopt, 1) ||
        !branch(51u, Kind::Bra, 13u) || !op(52u, Kind::Nop) ||
        !op(53u, Kind::MovByteLoadPostIncrement, 0u, 4u) ||
        !op(54u, Kind::MovByteLoadPostIncrement, 1u, 4u) ||
        !op(55u, Kind::ExtendUnsignedByte, 2u, 0u) ||
        !op(56u, Kind::ShiftLogicalLeftEight, 1u) ||
        !op(57u, Kind::OrRegister, 2u, 1u) ||
        !op(58u, Kind::TestRegister, 2u, 2u) ||
        !branch(59u, Kind::Bt, 71u) ||
        !op(60u, Kind::ShiftLogicalRightTwo, 2u) ||
        !op(61u, Kind::ShiftLogicalRightOne, 2u) ||
        !op(62u, Kind::AndImmediate, 0u, std::nullopt, 7) ||
        !op(63u, Kind::TestRegister, 0u, 0u) ||
        !branch(64u, Kind::BfS, 44u) ||
        !op(65u, Kind::OrRegister, 2u, 8u) ||
        !op(66u, Kind::MovByteLoadPostIncrement, 0u, 4u) ||
        !op(67u, Kind::AddRegister, 2u, 5u) ||
        !op(68u, Kind::ExtendUnsignedByte, 0u, 0u) ||
        !branch(69u, Kind::Bra, 46u) ||
        !op(70u, Kind::AddImmediate, 0u, std::nullopt, 1) ||
        !op(71u, Kind::MovLongLoadPostIncrement, 8u, 15u) ||
        !op(72u, Kind::MovLongLoadPostIncrement, 9u, 15u) ||
        !op(73u, Kind::MovRegister, 0u, 5u) ||
        !op(74u, Kind::Rts) || !op(75u, Kind::SubRegister, 0u, 3u))
        return std::nullopt;

    NativePrsTransformContract result;
    result.entry_address = entry_address;
    result.covered_size = body_bytes;
    const auto identity = code_identity(image, entry_address, body_bytes);
    if (!identity) return std::nullopt;
    result.code_identity = *identity;
    result.source_register = static_cast<std::uint8_t>(reg[4u]);
    result.destination_register = static_cast<std::uint8_t>(reg[5u]);
    result.preserved_gpr_mask = static_cast<std::uint16_t>(
        (~used & 0xffffu) | (1u << 15u) | (1u << reg[8u]) | (1u << reg[9u]));
    for (const auto [i, expected] :
         {std::pair{2u, 0xff00u}, std::pair{3u, 0xe000u}}) {
        const auto expanded = static_cast<std::uint64_t>(lines[i].address) + 4u +
            static_cast<std::uint32_t>(lines[i].instruction.displacement);
        if (expanded > std::numeric_limits<std::uint32_t>::max() - 1u)
            return std::nullopt;
        const auto address = static_cast<std::uint32_t>(expanded);
        const auto literal_resolved = image.resolve_segment_address(address, 2u);
        if (!literal_resolved) return std::nullopt;
        const auto* literal_segment = image.find_segment(*literal_resolved, 2u);
        if (literal_segment == nullptr || !literal_segment->permissions.readable)
            return std::nullopt;
        const auto literal_offset = literal_segment->byte_offset(*literal_resolved);
        const auto literal_identity = code_identity(image, address, 2u);
        if (!literal_offset || *literal_offset > literal_segment->bytes.size() ||
            literal_segment->bytes.size() - *literal_offset < 2u || !literal_identity)
            return std::nullopt;
        const auto value = katana::io::read_u16_le(literal_segment->bytes, *literal_offset);
        if (value != expected) return std::nullopt;
        result.literals.push_back({lines[i].address, address, value, *literal_identity});
    }
    return result;
}

std::vector<NativeStagedTransformCandidate>
discover_native_staged_transform_candidates(
    const katana::io::ExecutableImage& image,
    const std::span<const Line> analyzed_lines) {
    std::vector<NativeStagedTransformCandidate> result;
    for (std::size_t i = 0u; i + 24u <= analyzed_lines.size(); ++i) {
        if (analyzed_lines[i].opcode != 0x2fe6u) continue;
        if (auto candidate = staged_transform_candidate(
                image, analyzed_lines.subspan(i, 24u))) {
            result.push_back(std::move(*candidate));
            if (result.size() > maximum_provider_candidates)
                throw std::runtime_error("Staged transform candidate budget exceeded.");
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.entry_address < b.entry_address;
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<NativeStagedTransformCandidate>
discover_native_staged_transform_candidates(
    const katana::io::ExecutableImage& image) {
    std::vector<NativeStagedTransformCandidate> result;
    for (const auto& segment : image.segments()) {
        if (!segment.permissions.executable) continue;
        for (std::size_t offset = 0u; offset + 48u <= segment.bytes.size(); offset += 2u) {
            if (katana::io::read_u16_le(segment.bytes, offset) != 0x2fe6u)
                continue;
            const auto expanded = static_cast<std::uint64_t>(segment.virtual_address) + offset;
            if (expanded > std::numeric_limits<std::uint32_t>::max() - 48u)
                continue;
            const auto lines = katana::sh4::disassemble(
                std::span<const std::uint8_t>(segment.bytes).subspan(offset, 48u),
                static_cast<std::uint32_t>(expanded));
            if (auto candidate = staged_transform_candidate(image, lines)) {
                result.push_back(std::move(*candidate));
                if (result.size() > maximum_provider_candidates)
                    throw std::runtime_error("Staged transform candidate budget exceeded.");
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.entry_address < b.entry_address;
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<NativeConditionalFilePlacement>
discover_native_conditional_file_placements(
    const katana::io::ExecutableImage& image,
    const ControlFlowAnalysisResult& analysis) {
    std::vector<NativeConditionalFilePlacement> result;
    // Decode only the existing inventory, from current source bytes. The
    // supplied CFG remains positive candidate evidence, never completeness.
    std::vector<Line> lines;
    lines.reserve(analysis.recursive.instructions.size());
    for (const auto& supplied : analysis.recursive.instructions) {
        const auto resolved = image.resolve_segment_address(supplied.address, 2u);
        if (!resolved) return {};
        const auto* segment = image.find_segment(*resolved, 2u);
        if (!segment || !segment->permissions.executable) return {};
        const auto offset = segment->byte_offset(*resolved);
        if (!offset || *offset > segment->bytes.size() ||
            segment->bytes.size() - *offset < 2u) return {};
        const auto opcode = katana::io::read_u16_le(segment->bytes, *offset);
        if (opcode != supplied.opcode ||
            (!lines.empty() && supplied.address <= lines.back().address)) return {};
        Line line;
        line.address = supplied.address;
        line.opcode = opcode;
        line.instruction = katana::sh4::decode(opcode);
        line.target_address = katana::sh4::calculate_direct_branch_target(line.instruction, line.address);
        line.is_delay_slot = !lines.empty() && lines.back().address + 2u == line.address &&
                             lines.back().instruction.has_delay_slot && !lines.back().is_delay_slot;
        lines.push_back(std::move(line));
    }
    std::map<std::uint32_t, NativeStagedTransformCandidate> wrappers;
    std::vector<std::uint32_t> callees;
    for (auto& wrapper : discover_native_staged_transform_candidates(image, lines)) {
        const auto prs = recognize_native_prs_transform(image, wrapper.transform_target_address);
        if (!prs || prs->source_register != 4u || prs->destination_register != 5u ||
            (prs->preserved_gpr_mask & 0xc000u) != 0xc000u) continue;
        callees.push_back(wrapper.entry_address);
        wrappers.emplace(wrapper.entry_address, std::move(wrapper));
    }
    if (callees.empty()) return result;
    std::vector<FunctionBoundary> boundaries;
    std::vector<std::uint32_t> leaders;
    std::vector<std::uint32_t> normal_entries;
    for (const auto& function : analysis.recursive.functions) {
        boundaries.push_back({function.address, function.size});
        leaders.push_back(function.address);
        normal_entries.push_back(function.address);
        if (function.size != 0u && function.address <=
            std::numeric_limits<std::uint32_t>::max() - function.size)
            leaders.push_back(function.address + function.size);
    }
    // Snapshot switch entries are intentionally absent from executable CFG
    // edges. Re-derive their table bytes and producer shape for this private,
    // conditional flow graph only; do not mutate the analyzed program or
    // claim that these are all possible selector values/targets.
    const auto source_address = [&](const std::uint32_t address) {
        return image.resolve_segment_address(address, 2u).value_or(address);
    };
    std::set<std::uint32_t> ingress_boundaries;
    std::set<std::pair<std::uint32_t, std::uint32_t>> ingress_edges;
    const auto boundary = [&](const std::uint32_t address) {
        ingress_boundaries.insert(source_address(address));
    };
    const auto ingress = [&](const std::uint32_t from, const std::uint32_t to) {
        ingress_edges.emplace(source_address(from), source_address(to));
    };
    for (const auto entry : image.entry_points()) boundary(entry);
    for (const auto& function : analysis.recursive.functions) boundary(function.address);
    for (const auto& seed : analysis.recursive.seed_contract) boundary(seed.address);
    for (const auto& entry : analysis.guarded_aot_entries) {
        boundary(entry.guest_address); boundary(entry.shared_body_address);
    }
    for (const auto& line : lines)
        if (line.target_address) ingress(line.address, *line.target_address);
    for (const auto& edge : analysis.resolved_edges)
        ingress(edge.instruction_address, edge.target_address);
    for (const auto& indirect : analysis.indirect_control_flow) {
        if (indirect.target) ingress(indirect.instruction_address, *indirect.target);
        for (const auto target : indirect.targets) ingress(indirect.instruction_address, target);
        for (const auto target : indirect.analysis_candidates) ingress(indirect.instruction_address, target);
    }
    for (const auto& continuation : analysis.static_return_continuations)
        ingress(continuation.instruction_address, continuation.target_address);
    for (const auto& table : analysis.jump_tables)
        for (const auto& entry : table.entries)
            if (entry.accepted) ingress(table.dispatch_address, entry.target);
    std::vector<detail::RelativeJumpTableRecognition> tables;
    for (std::size_t i = 0u; i < lines.size(); ++i) {
        if ((lines[i].instruction.kind != Kind::Braf &&
             lines[i].instruction.kind != Kind::Bsrf) || lines[i].is_delay_slot) continue;
        auto table = detail::recognize_relative_jump_table(image, lines, i);
        if (!table) continue;
        // Even rejected tables can contradict another producer's ingress.
        // Collect every candidate before validating any positive table path.
        for (const auto& entry : table->table.entries)
            if (entry.accepted) ingress(table->table.dispatch_address, entry.target);
        tables.push_back(std::move(*table));
    }
    detail::GlobalEntryPredecessorIndex predecessors;
    for (const auto address : ingress_boundaries) predecessors.add_boundary(address);
    for (const auto& [from, to] : ingress_edges) predecessors.add_edge(from, to);
    auto conditional_edges = analysis.resolved_edges;
    for (const auto& recognition : tables) {
        const auto& table = recognition.table;
        const auto& producer = recognition.producer;
        if (table.dispatch_kind != JumpTableDispatchKind::Jump ||
            (!table.resolved && !table.aot_candidates_only)) continue;
        std::vector<detail::GlobalEntryEdge> internal_edges;
        for (const auto& edge : producer.internal_branch_edges)
            internal_edges.push_back({source_address(edge.source), source_address(edge.target)});
        if (producer.requires_global_ingress_proof &&
            !predecessors.closes(source_address(producer.ingress_seed_address),
                source_address(producer.ingress_load_address), std::span{internal_edges})) continue;
        for (const auto& entry : table.entries)
            if (entry.accepted)
                conditional_edges.push_back({table.dispatch_address, entry.target,
                    ResolvedControlFlowKind::Jump, true, ControlFlowEvidence::GuardedPartial});
    }
    const auto blocks = build_basic_blocks(lines, conditional_edges, leaders, normal_entries);
    const auto functions = discover_functions_from_blocks(blocks, boundaries, conditional_edges);
    const auto calls = detail::discover_source_constant_calls(image, blocks, functions, callees);
    for (const auto& call : calls) {
        if (!call.arguments[0] || !call.arguments[1]) continue;
        const auto wrapper = wrappers.find(call.callee_address);
        if (wrapper == wrappers.end()) continue;
        std::string name;
        bool terminated = false;
        for (std::uint64_t i = 0u; i <= 255u; ++i) {
            const auto address = static_cast<std::uint64_t>(*call.arguments[0]) + i;
            if (address > std::numeric_limits<std::uint32_t>::max()) break;
            const auto resolved = image.resolve_segment_address(static_cast<std::uint32_t>(address), 1u);
            if (!resolved) break;
            const auto* segment = image.find_segment(*resolved, 1u);
            if (!segment || !segment->permissions.readable) break;
            const auto offset = segment->byte_offset(*resolved);
            if (!offset || *offset >= segment->bytes.size()) break;
            const auto c = segment->bytes[*offset];
            if (c == 0u) { terminated = true; break; }
            if (c < 0x20u || c > 0x7eu || c == ':' || c == '*' || c == '?') break;
            name.push_back(static_cast<char>(c));
        }
        if (!terminated || name.empty() || name.front() == '/' || name.front() == '\\' ||
            name.find("..") != std::string::npos) continue;
        result.push_back({call.function_address, call.call_instruction_address, call.callee_address,
            std::move(name), *call.arguments[1], wrapper->second.staging_buffer_address, {}});
    }
    if (result.empty()) return result;
    // Hash once per source view, not once per call or disc candidate. Whole
    // segment bytes also bind out-of-body literal pools and filename NULs.
    katana::io::Sha256Accumulator binding;
    const auto scalar = [&](std::uint64_t value) {
        std::array<char, 8> bytes{};
        for (auto& byte : bytes) { byte = static_cast<char>(value & 255u); value >>= 8u; }
        binding.update(std::string_view(bytes.data(), bytes.size()));
    };
    const auto string = [&](std::string_view value) { scalar(value.size()); binding.update(value); };
    string("conditional-prs-calls-v1:source-CFG:SH-C-callee-save:wrapper-spill-preserved:effects-unproved");
    scalar(static_cast<std::uint64_t>(image.address_model()));
    scalar(static_cast<std::uint64_t>(image.guest_call_abi()));
    scalar(static_cast<std::uint64_t>(image.initial_snapshot_policy()));
    scalar(image.segments().size());
    for (const auto& segment : image.segments()) {
        scalar(segment.virtual_address); scalar(segment.memory_size);
        scalar(static_cast<std::uint64_t>(segment.source_kind));
        scalar(static_cast<std::uint64_t>(segment.load_phase));
        scalar(segment.permissions.readable); scalar(segment.permissions.writable);
        scalar(segment.permissions.executable); string(segment.local_source_name);
        string(std::string_view(reinterpret_cast<const char*>(segment.bytes.data()), segment.bytes.size()));
    }
    scalar(image.address_aliases().size());
    for (const auto& alias : image.address_aliases()) {
        scalar(alias.source_start); scalar(alias.runtime_start); scalar(alias.size);
    }
    scalar(lines.size());
    for (const auto& line : lines) { scalar(line.address); scalar(line.opcode); scalar(line.is_delay_slot); }
    scalar(boundaries.size());
    for (const auto& boundary : boundaries) { scalar(boundary.entry_address); scalar(boundary.size); }
    scalar(conditional_edges.size());
    for (const auto& edge : conditional_edges) {
        scalar(edge.instruction_address); scalar(edge.target_address);
        scalar(static_cast<std::uint64_t>(edge.kind)); scalar(edge.guarded);
        scalar(static_cast<std::uint64_t>(edge.evidence)); scalar(edge.analysis_candidate_carrier);
    }
    scalar(ingress_boundaries.size());
    for (const auto address : ingress_boundaries) scalar(address);
    scalar(ingress_edges.size());
    for (const auto& [from, to] : ingress_edges) { scalar(from); scalar(to); }
    const auto source_identity = binding.finish();
    for (auto& candidate : result) {
        const auto derivation = source_identity + ":" + std::to_string(candidate.function_address) + ":" +
            std::to_string(candidate.call_instruction_address) + ":" + std::to_string(candidate.wrapper_address) +
            ":" + candidate.file_name + ":" + std::to_string(candidate.possible_destination_address) +
            ":" + std::to_string(candidate.staging_buffer_address);
        candidate.source_contract_identity = "sha256:" + katana::io::sha256_bytes(derivation);
    }
    return result;
}

std::string_view native_sdk_provider_family_name(
    const NativeSdkProviderFamily family) noexcept {
    switch (family) {
    case NativeSdkProviderFamily::NamedTextureArchiveLoad:
        return "named-texture-archive-load";
    case NativeSdkProviderFamily::TextureArchiveRelease:
        return "texture-archive-release";
    case NativeSdkProviderFamily::SynchronousContentRangeRead:
        return "synchronous-content-range-read";
    case NativeSdkProviderFamily::SoundBankChunkRegistration:
        return "sound-bank-chunk-registration";
    case NativeSdkProviderFamily::SoundFrameService:
        return "sound-frame-service";
    }
    return "unknown";
}

std::string_view native_sdk_provider_boundary_proof_name(
    const NativeSdkProviderBoundaryProof proof) noexcept {
    switch (proof) {
    case NativeSdkProviderBoundaryProof::StructuralPrologueReturn:
        return "structural-prologue-return";
    }
    return "unknown";
}

std::string_view native_sdk_resource_reference_kind_name(
    const NativeSdkResourceReferenceKind kind) noexcept {
    switch (kind) {
    case NativeSdkResourceReferenceKind::GuestDescriptorPointer:
        return "guest-descriptor-pointer";
    }
    return "unknown";
}

std::vector<NativeSdkProviderCandidate>
discover_native_sdk_provider_candidates(
    const katana::io::ExecutableImage& image) {
    std::vector<NativeSdkProviderCandidate> result;
    for (const auto& segment : image.segments()) {
        if (!segment.permissions.executable || segment.bytes.size() < 16u)
            continue;
        for (std::size_t offset = 0u; offset + 2u <= segment.bytes.size();
             offset += 2u) {
            const auto opcode = katana::io::read_u16_le(segment.bytes, offset);
            if (opcode != 0x2FE6u) continue; // mov.l r14,@-r15
            const auto window = decode_window(
                segment.bytes, offset, segment.virtual_address);
            scan_decoded_window(result, image, window);
        }
    }
    canonicalize(result);
    const auto analyzed_lines = disassemble_executable_segments(image);
    bind_resource_reference(result, analyzed_lines);
    return result;
}

std::vector<NativeSdkProviderCandidate>
discover_native_sdk_provider_candidates(
    const katana::io::ExecutableImage& image,
    const std::span<const Line> analyzed_lines) {
    if (!std::is_sorted(
            analyzed_lines.begin(), analyzed_lines.end(),
            [](const auto& left, const auto& right) {
                return left.address < right.address;
            }))
        throw std::invalid_argument(
            "Native SDK provider analysis requires sorted instructions.");

    std::vector<NativeSdkProviderCandidate> result;
    for (std::size_t start = 0u; start < analyzed_lines.size(); ++start) {
        if (!instruction(analyzed_lines[start],
                         Kind::MovLongStorePreDecrement, 15u, 14u))
            continue;
        const auto count = std::min(
            maximum_provider_instructions,
            analyzed_lines.size() - start);
        scan_decoded_window(
            result, image, analyzed_lines.subspan(start, count));
    }
    canonicalize(result);
    bind_resource_reference(result, analyzed_lines);
    if (std::ranges::any_of(result, [](const auto& candidate) {
            return !candidate.resource_reference.has_value();
        })) {
        // Provider reachability and provider boundaries stay tied to the
        // product analysis.  Resource descriptors are data contracts shared
        // with surviving code, however, so consumers can legitimately sit
        // outside that current root slice.  Complete only the missing data
        // proof from the same identity-bound executable bytes; this never
        // promotes another provider or executable root.
        const auto image_lines = disassemble_executable_segments(image);
        bind_resource_reference(result, image_lines);
        for (auto& candidate : result) {
            if (candidate.resource_reference.has_value())
                candidate.evidence.push_back(
                    "resource-reference-completed-from-identity-bound-image");
        }
    }
    return result;
}

} // namespace katana::analysis
