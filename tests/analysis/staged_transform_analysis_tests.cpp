#include "katana/analysis/native_sdk_provider_analysis.hpp"
#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void word(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1u) = static_cast<std::uint8_t>(value >> 8u);
}
void longword(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0u; i < 4u; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8u * i));
}
std::vector<std::uint8_t> fixture(std::uint32_t base) {
    std::vector<std::uint8_t> bytes(96u);
    constexpr std::array<std::uint16_t, 24u> body{
        0x2fe6,0x4f22,0x7ffc,0x2f52,0xd50d,0xb079,0x0009,0x6e03,
        0x4e11,0x8b03,0xd30b,0xd40c,0x430b,0x65f2,0xd20b,0xd30c,
        0x223b,0x420b,0x0009,0x60e3,0x7f04,0x4f26,0x000b,0x6ef6};
    for (std::size_t i=0u;i<body.size();++i) word(bytes,2u*i,body[i]);
    longword(bytes,64u,0x0cc00000u);
    longword(bytes,68u,base+0x300u);
    longword(bytes,72u,0x0cc00000u);
    longword(bytes,76u,0x0c123456u);
    longword(bytes,80u,0x80000000u);
    return bytes;
}
katana::io::ExecutableImage image(std::vector<std::uint8_t> bytes,
    std::uint32_t base=0x8c101000u, bool executable=true) {
    katana::io::ExecutableImage result;
    const auto size=bytes.size();
    result.add_segment({"fixture",base,0u,size,katana::io::SegmentKind::Mixed,
        {true,true,executable},std::move(bytes),katana::io::ImageSourceKind::RawBinary,
        katana::io::ImageLoadPhase::Initial,"synthetic-source"});
    return result;
}

// Assemble a synthetic PRS token machine with selectable register roles and
// an external literal pool. Neither the test address nor register allocation
// comes from a game image.
std::vector<std::uint8_t> prs_fixture(bool renamed = false) {
    std::array<unsigned, 16u> r{};
    for (unsigned i=0u;i<r.size();++i) r[i]=i;
    if (renamed) {
        std::swap(r[1],r[2]); std::swap(r[3],r[6]);
        std::swap(r[4],r[5]); std::swap(r[8],r[11]); std::swap(r[9],r[12]);
    }
    std::vector<std::uint8_t> bytes(180u,0u);
    const auto unary=[&](unsigned i,unsigned opcode,unsigned n) {
        word(bytes,2u*i,static_cast<std::uint16_t>(opcode | (r[n]<<8u)));
    };
    const auto binary=[&](unsigned i,unsigned opcode,unsigned n,unsigned m) {
        word(bytes,2u*i,static_cast<std::uint16_t>(opcode | (r[n]<<8u) | (r[m]<<4u)));
    };
    const auto imm=[&](unsigned i,unsigned opcode,unsigned n,unsigned value) {
        word(bytes,2u*i,static_cast<std::uint16_t>(opcode | (r[n]<<8u) | value));
    };
    const auto jump=[&](unsigned i,unsigned opcode,unsigned target) {
        const int displacement=static_cast<int>(target)-static_cast<int>(i)-2;
        const unsigned mask=opcode==0xa000u ? 0xfffu : 0xffu;
        word(bytes,2u*i,static_cast<std::uint16_t>(opcode | (static_cast<unsigned>(displacement)&mask)));
    };
    const auto bit=[&](unsigned i) {
        unary(i,0x4010u,7);jump(i+1,0x8f00u,i+6);unary(i+2,0x4001u,6);
        binary(i+3,0x6004u,6,4);imm(i+4,0xe000u,7,8);unary(i+5,0x4001u,6);
    };
    binary(0,0x2006u,15,9);binary(1,0x2006u,15,8);
    imm(2,0x9000u,9,(160u-8u)/2u);imm(3,0x9000u,8,(162u-10u)/2u);
    binary(4,0x6004u,6,4);imm(5,0xe000u,7,9);jump(6,0xa000u,13);
    binary(7,0x6003u,3,5);word(bytes,16u,0xaaaau);word(bytes,18u,0xbbbbu);
    binary(10,0x6004u,1,4);binary(11,0x2000u,5,1);imm(12,0x7000u,5,1);
    bit(13);jump(19,0x8900u,10);bit(20);jump(26,0x8900u,53);
    imm(27,0xe000u,0,0);bit(28);unary(34,0x4024u,0);bit(35);
    binary(41,0x6004u,2,4);unary(42,0x4024u,0);binary(43,0x200bu,2,9);
    imm(44,0x7000u,0,2);binary(45,0x300cu,2,5);binary(46,0x6004u,1,2);
    unary(47,0x4010u,0);binary(48,0x2000u,5,1);jump(49,0x8f00u,46);
    imm(50,0x7000u,5,1);jump(51,0xa000u,13);word(bytes,104u,0x0009u);
    binary(53,0x6004u,0,4);binary(54,0x6004u,1,4);binary(55,0x600cu,2,0);
    unary(56,0x4018u,1);binary(57,0x200bu,2,1);binary(58,0x2008u,2,2);
    jump(59,0x8900u,71);unary(60,0x4009u,2);unary(61,0x4001u,2);
    word(bytes,124u,0xc907u);binary(63,0x2008u,0,0);jump(64,0x8f00u,44);
    binary(65,0x200bu,2,8);binary(66,0x6004u,0,4);binary(67,0x300cu,2,5);
    binary(68,0x600cu,0,0);jump(69,0xa000u,46);imm(70,0x7000u,0,1);
    binary(71,0x6006u,8,15);binary(72,0x6006u,9,15);binary(73,0x6003u,0,5);
    word(bytes,148u,0x000bu);binary(75,0x3008u,0,3);
    word(bytes,160u,0xff00u);word(bytes,162u,0xe000u);
    return bytes;
}

void prs_contract_checks() {
    using katana::analysis::recognize_native_prs_transform;
    constexpr std::uint32_t base=0x8c231002u;
    const auto bytes=prs_fixture();
    const auto found=recognize_native_prs_transform(image(bytes,base),base);
    require(found.has_value(),"complete PRS token machine recognized");
    require(found->source_register==4u && found->destination_register==5u &&
        found->preserved_gpr_mask==0xff00u,"PRS ABI effects derived from all register uses");
    require(found->covered_size==152u && found->literals.size()==2u &&
        found->literals[0].literal_address==base+160u,"external word masks identity-bound");
    const auto moved=recognize_native_prs_transform(image(bytes,base+0x12000u),base+0x12000u);
    require(moved && moved->code_identity==found->code_identity && moved->literals!=found->literals,
        "PRS recognition is independent of code and pool addresses");
    const auto renamed=recognize_native_prs_transform(image(prs_fixture(true),base),base);
    require(renamed && renamed->source_register==5u && renamed->destination_register==4u &&
        renamed->preserved_gpr_mask==0xff00u,"PRS register allocation may change");

    for (unsigned i=0u;i<76u;++i) {
        if (i==8u || i==9u) continue; // unreachable literal island
        auto changed=bytes;word(changed,2u*i,0xffffu);
        require(!recognize_native_prs_transform(image(changed,base),base),
            "no unchecked instruction, delay slot, branch or return in PRS proof");
    }
    for (const auto [offset,value] : std::array<std::pair<unsigned,unsigned>,7u>{{
            {160u,0xff80u},{162u,0xc000u},{30u,0x4600u},{98u,0x8ffau},
            {130u,0x229bu},{148u,0x0009u},{8u,0x6654u}}}) {
        auto changed=bytes;word(changed,offset,static_cast<std::uint16_t>(value));
        require(!recognize_native_prs_transform(image(changed,base),base),
            "wrong offset masks, control direction, copy loop, return or alias reject");
    }
    auto changed=bytes;changed.resize(152u);
    require(!recognize_native_prs_transform(image(changed,base),base),"missing external masks reject");
    require(!recognize_native_prs_transform(image(bytes,base,false),base),"nonexecuting PRS-shaped data rejects");
    require(!recognize_native_prs_transform(image(bytes,base),base+1u),"unaligned PRS entry rejects");
    require(!recognize_native_prs_transform(image(bytes,base),0xfffffff0u),"PRS address overflow rejects");
    changed=bytes;word(changed,16u,0xffffu);word(changed,18u,0x000bu);
    const auto island=recognize_native_prs_transform(image(changed,base),base);
    require(island && island->code_identity!=found->code_identity,
        "skipped literal island is not decoded as reachable code but stays identity-bound");
}
void conditional_file_call_checks() {
    using katana::analysis::discover_native_conditional_file_placements;
    constexpr std::uint32_t base = 0x8c101000u;
    const auto make_bytes = [&] {
        auto bytes = fixture(base); bytes.resize(0x700u, 0xffu);
        const auto prs = prs_fixture();
        std::copy(prs.begin(), prs.end(), bytes.begin() + 0x300u);
        word(bytes, 0x100u, 0x000bu); word(bytes, 0x102u, 0x0009u);
        longword(bytes, 0x580u, 0x0c900000u);
        longword(bytes, 0x584u, base + 0x600u);
        longword(bytes, 0x588u, base);
        const std::string name = "FIELD.PRS";
        std::copy(name.begin(), name.end(), bytes.begin() + 0x600u);
        bytes[0x600u + name.size()] = 0u;
        return bytes;
    };
    const auto analyze = [&](const std::vector<std::uint8_t>& bytes,
                             std::size_t caller_size, bool table_edge = false) {
        katana::analysis::ControlFlowAnalysisResult analysis;
        for (const auto [offset, size] : std::array<std::pair<std::size_t, std::size_t>,4u>{{
                {0u,48u}, {0x100u,4u}, {0x300u,152u}, {0x500u,caller_size}}}) {
            auto lines = katana::sh4::disassemble(std::span<const std::uint8_t>(bytes).subspan(offset,size),
                base + static_cast<std::uint32_t>(offset));
            analysis.recursive.instructions.insert(analysis.recursive.instructions.end(),lines.begin(),lines.end());
            katana::analysis::FunctionCandidate fn;
            fn.address = base + static_cast<std::uint32_t>(offset);
            fn.size = static_cast<std::uint32_t>(size);
            analysis.recursive.functions.push_back(fn);
        }
        if (table_edge) analysis.resolved_edges.push_back({base+0x502u,base+0x510u,
            katana::analysis::ResolvedControlFlowKind::Jump,true,
            katana::analysis::ControlFlowEvidence::GuardedPartial});
        return analysis;
    };
    auto bytes = make_bytes();
    word(bytes,0x500u,0xdc1fu); // destination in callee-save r12
    word(bytes,0x502u,0xd420u); // filename literal at 584
    word(bytes,0x504u,0xbd7cu); // bsr wrapper
    word(bytes,0x506u,0x65c3u); // argument written in delay slot
    word(bytes,0x508u,0x000bu); word(bytes,0x50au,0x0009u);
    const auto original_analysis = analyze(bytes,12u);
    const auto direct = discover_native_conditional_file_placements(image(bytes,base),original_analysis);
    require(direct.size()==1u && direct[0].file_name=="FIELD.PRS" &&
        direct[0].possible_destination_address==0x0c900000u &&
        direct[0].source_contract_identity.starts_with("sha256:"),
        "direct source call binds filename and post-delay argument");
    auto changed=bytes; word(changed,0x506u,0xe400u);
    require(discover_native_conditional_file_placements(image(changed,base),original_analysis).empty(),
        "stale caller bytes reject");
    require(discover_native_conditional_file_placements(image(changed,base),analyze(changed,12u)).empty(),
        "delay-slot overwrite invalidates filename argument");
    require(discover_native_conditional_file_placements(image(bytes,base),analyze(bytes,6u)).empty(),
        "missing physical delay slot rejects");
    changed=bytes; changed[0x600u]='X';
    const auto renamed=discover_native_conditional_file_placements(image(changed,base),original_analysis);
    require(renamed.size()==1u && renamed[0].file_name=="XIELD.PRS" &&
        renamed[0].source_contract_identity!=direct[0].source_contract_identity,
        "external filename bytes participate in source identity");
    changed=bytes;word(changed,0x506u,0x65f2u);
    require(discover_native_conditional_file_placements(image(changed,base),analyze(changed,12u)).empty(),
        "stack-derived call argument is not source-register evidence");

    bytes=make_bytes();
    word(bytes,0x500u,0xd51fu);word(bytes,0x502u,0xd420u);
    word(bytes,0x504u,0xd320u);word(bytes,0x506u,0x430bu);
    word(bytes,0x508u,0xe300u);word(bytes,0x50au,0x000bu);word(bytes,0x50cu,0x0009u);
    require(discover_native_conditional_file_placements(image(bytes,base),analyze(bytes,14u)).size()==1u,
        "JSR target is captured before overwriting its register in the delay slot");

    bytes=make_bytes();
    word(bytes,0x500u,0xdc1fu);word(bytes,0x502u,0x0023u);word(bytes,0x504u,0x0009u);
    for (std::size_t offset=0x506u;offset<0x510u;offset+=2u) word(bytes,offset,0x0009u);
    word(bytes,0x510u,0xd41cu);word(bytes,0x512u,0xbd75u);word(bytes,0x514u,0x65c3u);
    word(bytes,0x516u,0x000bu);word(bytes,0x518u,0x0009u);
    require(discover_native_conditional_file_placements(image(bytes,base),analyze(bytes,26u,true)).size()==1u,
        "resolved table edge reaches file case while preserving pre-dispatch destination");
    require(discover_native_conditional_file_placements(image(bytes,base),analyze(bytes,26u,false)).empty(),
        "absent table evidence does not invent disconnected file case");

    // A source-bound switch can supply candidate paths even while its
    // writable table is deliberately absent from executable resolved_edges.
    bytes=make_bytes();
    word(bytes,0x500u,0xdc1fu);word(bytes,0x502u,0xe102u);
    word(bytes,0x504u,0x3212u);word(bytes,0x506u,0x8923u);
    word(bytes,0x508u,0x4200u);word(bytes,0x50au,0x6323u);
    word(bytes,0x50cu,0xc704u);word(bytes,0x50eu,0x043du);
    word(bytes,0x510u,0x0423u);word(bytes,0x512u,0x0009u);
    word(bytes,0x520u,0x004cu);word(bytes,0x522u,0x003cu);
    word(bytes,0x550u,0x000bu);word(bytes,0x552u,0x0009u);
    word(bytes,0x560u,0xd408u);word(bytes,0x562u,0xbd4du);
    word(bytes,0x564u,0x65c3u);word(bytes,0x566u,0x000bu);word(bytes,0x568u,0x0009u);
    auto switch_analysis=analyze(bytes,0x6au);
    std::erase_if(switch_analysis.recursive.instructions,[](const auto& line) {
        return (line.address>=base+0x514u && line.address<base+0x550u) ||
               (line.address>=base+0x554u && line.address<base+0x560u);
    });
    auto snapshot_image=image(bytes,base);
    snapshot_image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    require(discover_native_conditional_file_placements(image(bytes,base),switch_analysis).empty(),
        "writable source without a snapshot contract cannot supply table paths");
    require(switch_analysis.resolved_edges.empty() &&
        discover_native_conditional_file_placements(snapshot_image,switch_analysis).size()==1u &&
        switch_analysis.resolved_edges.empty(),
        "conditional source-table flow must not promote candidate edges into executable CFA");
    auto alternate_entry=switch_analysis;
    alternate_entry.recursive.seed_contract.push_back({base+0x50eu});
    require(discover_native_conditional_file_placements(snapshot_image,alternate_entry).empty(),
        "an alternate source entry invalidates local table producer dominance");
    auto alternate_edge=switch_analysis;
    katana::analysis::IndirectControlFlowResolution incoming;
    incoming.instruction_address=base+0x100u;
    incoming.analysis_candidates.push_back(base+0x50eu);
    alternate_edge.indirect_control_flow.push_back(incoming);
    require(discover_native_conditional_file_placements(snapshot_image,alternate_edge).empty(),
        "an indirect candidate ingress invalidates local table producer dominance");
}
} // namespace

int main() {
    prs_contract_checks();
    conditional_file_call_checks();
    using katana::analysis::discover_native_staged_transform_candidates;
    constexpr std::uint32_t base=0x8c101000u;
    const auto original=fixture(base);
    const auto source=image(original);
    auto candidates=discover_native_staged_transform_candidates(source);
    require(candidates.size()==1u,"one source-bound staged wrapper");
    const auto& found=candidates.front();
    require(found.covered_size==48u && found.literals.size()==5u,"body and literal extents");
    require(found.stage_target_address==base+0x100u,"direct stage call");
    require(found.transform_target_address==base+0x300u,"independent transform target");
    require(found.finalizer_target_address==0x8c123456u,"finalizer retained as unmodeled call");
    const auto lines=katana::sh4::disassemble(original,base);
    require(discover_native_staged_transform_candidates(source,lines)==candidates,
        "raw-source and analyzed-instruction paths agree");

    constexpr std::uint32_t relocated=0x8c211000u;
    const auto moved=discover_native_staged_transform_candidates(image(fixture(relocated),relocated));
    require(moved.size()==1u && moved[0].stage_target_address==relocated+0x100u &&
        moved[0].transform_target_address==relocated+0x300u,"relocation without title addresses");
    require(moved[0].code_identity==found.code_identity && moved[0].literals!=found.literals,
        "out-of-body literal identity is not hidden by identical wrapper bytes");

    for (const auto [offset,value] : std::array<std::pair<std::size_t,std::uint16_t>,3u>{{
            {18u,std::uint16_t{0x8903u}},{26u,std::uint16_t{0x65e2u}},
            {38u,std::uint16_t{0xe000u}}}}) {
        auto changed=original;word(changed,offset,value);
        require(discover_native_staged_transform_candidates(image(changed)).empty(),
            "changed success branch, output provenance or result rejects");
        require(discover_native_staged_transform_candidates(image(changed),lines).empty(),
            "stale analyzed opcodes cannot bind to changed source");
    }
    auto changed=original;longword(changed,72u,0x0cc00100u);
    require(discover_native_staged_transform_candidates(image(changed)).empty(),
        "different stage and transform input buffers reject");
    changed=original;longword(changed,68u,base+0x400u);
    const auto rebound=discover_native_staged_transform_candidates(image(changed),lines);
    require(rebound.size()==1u && rebound[0].code_identity==found.code_identity &&
        rebound[0]!=found,"changed literal pool creates different source evidence");
    auto missing=lines;missing.erase(missing.begin()+12);
    require(discover_native_staged_transform_candidates(source,missing).empty(),
        "partial CFG cannot synthesize missing wrapper instructions");
    require(discover_native_staged_transform_candidates(image(original,base,false)).empty(),
        "data segment cannot supply executable wrapper evidence");
    changed=original;changed.resize(48u);
    require(discover_native_staged_transform_candidates(image(changed)).empty(),
        "unavailable literal bytes reject");
    require(discover_native_staged_transform_candidates(image(original,base+1u)).empty(),
        "unaligned source is not SH4 code");
    std::cout << "staged transform source/relocation/negative checks passed\n";
}
