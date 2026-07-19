#include "katana/sh4/instruction_metadata.hpp"

#include <array>

namespace katana::sh4 {
namespace {

using F = OperandFormat;
using C = ControlFlowKind;

#define KATANA_RULE(kind, mask, pattern, format)                                                   \
    InstructionMetadata {                                                                          \
        InstructionKind::kind, #kind, mask, pattern, F::format, C::None, false, false              \
    }
#define KATANA_FLOW(kind, mask, pattern, format, flow, delay, privileged)                          \
    InstructionMetadata {                                                                          \
        InstructionKind::kind, #kind, mask, pattern, F::format, C::flow, delay, privileged         \
    }

constexpr std::array kInstructionMetadata = {
    KATANA_RULE(Nop, 0xFFFFu, 0x0009u, None),
    KATANA_FLOW(Rts, 0xFFFFu, 0x000Bu, None, Return, true, false),
    KATANA_FLOW(ReturnFromException, 0xFFFFu, 0x002Bu, None, ExceptionReturn, true, true),
    KATANA_FLOW(Sleep, 0xFFFFu, 0x001Bu, None, Halt, false, true),
    KATANA_FLOW(LoadTlb, 0xFFFFu, 0x0038u, None, None, false, true),
    KATANA_RULE(Prefetch, 0xF0FFu, 0x0083u, RegisterIndirectN),
    KATANA_RULE(Ocbi, 0xF0FFu, 0x0093u, RegisterIndirectN),
    KATANA_RULE(Ocbp, 0xF0FFu, 0x00A3u, RegisterIndirectN),
    KATANA_RULE(Ocbwb, 0xF0FFu, 0x00B3u, RegisterIndirectN),
    KATANA_RULE(MovcaLong, 0xF0FFu, 0x00C3u, RegisterIndirectN),
    KATANA_FLOW(TrapAlways, 0xFF00u, 0xC300u, Immediate8Register0, Trap, false, false),
    KATANA_FLOW(Bra, 0xF000u, 0xA000u, Displacement12, UnconditionalBranch, true, false),
    KATANA_FLOW(Bsr, 0xF000u, 0xB000u, Displacement12, Call, true, false),
    KATANA_FLOW(Braf, 0xF0FFu, 0x0023u, RegisterN, IndirectBranch, true, false),
    KATANA_FLOW(Bsrf, 0xF0FFu, 0x0003u, RegisterN, IndirectCall, true, false),
    KATANA_FLOW(Bt, 0xFF00u, 0x8900u, Displacement8, ConditionalBranch, false, false),
    KATANA_FLOW(Bf, 0xFF00u, 0x8B00u, Displacement8, ConditionalBranch, false, false),
    KATANA_FLOW(BtS, 0xFF00u, 0x8D00u, Displacement8, ConditionalBranch, true, false),
    KATANA_FLOW(BfS, 0xFF00u, 0x8F00u, Displacement8, ConditionalBranch, true, false),
    KATANA_FLOW(Jmp, 0xF0FFu, 0x402Bu, RegisterIndirectN, IndirectBranch, true, false),
    KATANA_FLOW(Jsr, 0xF0FFu, 0x400Bu, RegisterIndirectN, IndirectCall, true, false),

    KATANA_RULE(FmovRegister, 0xF00Fu, 0xF00Cu, RegisterMRegisterN),
    KATANA_RULE(FmovLoad, 0xF00Fu, 0xF008u, RegisterMRegisterN),
    KATANA_RULE(FmovLoadPostIncrement, 0xF00Fu, 0xF009u, RegisterMRegisterN),
    KATANA_RULE(FmovLoadR0Indexed, 0xF00Fu, 0xF006u, RegisterMRegisterN),
    KATANA_RULE(FmovStore, 0xF00Fu, 0xF00Au, RegisterMRegisterN),
    KATANA_RULE(FmovStorePreDecrement, 0xF00Fu, 0xF00Bu, RegisterMRegisterN),
    KATANA_RULE(FmovStoreR0Indexed, 0xF00Fu, 0xF007u, RegisterMRegisterN),
    KATANA_RULE(Fldi0, 0xF0FFu, 0xF08Du, RegisterN),
    KATANA_RULE(Fldi1, 0xF0FFu, 0xF09Du, RegisterN),
    KATANA_RULE(Flds, 0xF0FFu, 0xF01Du, RegisterN),
    KATANA_RULE(Fsts, 0xF0FFu, 0xF00Du, RegisterN),
    KATANA_RULE(Fabs, 0xF0FFu, 0xF05Du, RegisterN),
    KATANA_RULE(Fadd, 0xF00Fu, 0xF000u, RegisterMRegisterN),
    KATANA_RULE(FcmpEqual, 0xF00Fu, 0xF004u, RegisterMRegisterN),
    KATANA_RULE(FcmpGreater, 0xF00Fu, 0xF005u, RegisterMRegisterN),
    KATANA_RULE(Fdiv, 0xF00Fu, 0xF003u, RegisterMRegisterN),
    KATANA_RULE(FloatFromFpul, 0xF0FFu, 0xF02Du, RegisterN),
    KATANA_RULE(Fmac, 0xF00Fu, 0xF00Eu, RegisterMRegisterN),
    KATANA_RULE(Fmul, 0xF00Fu, 0xF002u, RegisterMRegisterN),
    KATANA_RULE(Fneg, 0xF0FFu, 0xF04Du, RegisterN),
    KATANA_RULE(Fsqrt, 0xF0FFu, 0xF06Du, RegisterN),
    KATANA_RULE(Fsrra, 0xF0FFu, 0xF07Du, RegisterN),
    KATANA_RULE(Fsca, 0xF1FFu, 0xF0FDu, RegisterN),
    KATANA_RULE(Fipr, 0xF0FFu, 0xF0EDu, RegisterMRegisterN),
    KATANA_RULE(Ftrv, 0xF3FFu, 0xF1FDu, RegisterN),
    KATANA_RULE(Fsub, 0xF00Fu, 0xF001u, RegisterMRegisterN),
    KATANA_RULE(Ftrc, 0xF0FFu, 0xF03Du, RegisterN),
    KATANA_RULE(FcnvDoubleToSingle, 0xF1FFu, 0xF0BDu, RegisterN),
    KATANA_RULE(FcnvSingleToDouble, 0xF1FFu, 0xF0ADu, RegisterN),
    KATANA_RULE(Frchg, 0xFFFFu, 0xFBFDu, None),
    KATANA_RULE(Fschg, 0xFFFFu, 0xF3FDu, None),

    KATANA_RULE(SubRegister, 0xF00Fu, 0x3008u, RegisterMRegisterN),
    KATANA_RULE(NegateRegister, 0xF00Fu, 0x600Bu, RegisterMRegisterN),
    KATANA_RULE(NotRegister, 0xF00Fu, 0x6007u, RegisterMRegisterN),
    KATANA_RULE(AddWithCarry, 0xF00Fu, 0x300Eu, RegisterMRegisterN),
    KATANA_RULE(AddWithOverflow, 0xF00Fu, 0x300Fu, RegisterMRegisterN),
    KATANA_RULE(SubWithCarry, 0xF00Fu, 0x300Au, RegisterMRegisterN),
    KATANA_RULE(SubWithOverflow, 0xF00Fu, 0x300Bu, RegisterMRegisterN),
    KATANA_RULE(NegateWithCarry, 0xF00Fu, 0x600Au, RegisterMRegisterN),
    KATANA_RULE(ExtendUnsignedByte, 0xF00Fu, 0x600Cu, RegisterMRegisterN),
    KATANA_RULE(ExtendUnsignedWord, 0xF00Fu, 0x600Du, RegisterMRegisterN),
    KATANA_RULE(ExtendSignedByte, 0xF00Fu, 0x600Eu, RegisterMRegisterN),
    KATANA_RULE(ExtendSignedWord, 0xF00Fu, 0x600Fu, RegisterMRegisterN),
    KATANA_RULE(SwapBytes, 0xF00Fu, 0x6008u, RegisterMRegisterN),
    KATANA_RULE(SwapWords, 0xF00Fu, 0x6009u, RegisterMRegisterN),
    KATANA_RULE(ExtractMiddle, 0xF00Fu, 0x200Du, RegisterMRegisterN),
    KATANA_RULE(DecrementAndTest, 0xF0FFu, 0x4010u, RegisterN),
    KATANA_RULE(MoveT, 0xF0FFu, 0x0029u, RegisterN),
    KATANA_RULE(ShiftLogicalLeftOne, 0xF0FFu, 0x4000u, RegisterN),
    KATANA_RULE(ShiftLogicalRightOne, 0xF0FFu, 0x4001u, RegisterN),
    KATANA_RULE(ShiftArithmeticLeftOne, 0xF0FFu, 0x4020u, RegisterN),
    KATANA_RULE(ShiftArithmeticRightOne, 0xF0FFu, 0x4021u, RegisterN),
    KATANA_RULE(ShiftLogicalLeftTwo, 0xF0FFu, 0x4008u, RegisterN),
    KATANA_RULE(ShiftLogicalLeftEight, 0xF0FFu, 0x4018u, RegisterN),
    KATANA_RULE(ShiftLogicalLeftSixteen, 0xF0FFu, 0x4028u, RegisterN),
    KATANA_RULE(ShiftLogicalRightTwo, 0xF0FFu, 0x4009u, RegisterN),
    KATANA_RULE(ShiftLogicalRightEight, 0xF0FFu, 0x4019u, RegisterN),
    KATANA_RULE(ShiftLogicalRightSixteen, 0xF0FFu, 0x4029u, RegisterN),
    KATANA_RULE(RotateLeft, 0xF0FFu, 0x4004u, RegisterN),
    KATANA_RULE(RotateRight, 0xF0FFu, 0x4005u, RegisterN),
    KATANA_RULE(RotateLeftThroughT, 0xF0FFu, 0x4024u, RegisterN),
    KATANA_RULE(RotateRightThroughT, 0xF0FFu, 0x4025u, RegisterN),
    KATANA_RULE(ShiftArithmeticDynamic, 0xF00Fu, 0x400Cu, RegisterMRegisterN),
    KATANA_RULE(ShiftLogicalDynamic, 0xF00Fu, 0x400Du, RegisterMRegisterN),

    KATANA_RULE(DivideInitializeUnsigned, 0xFFFFu, 0x0019u, None),
    KATANA_RULE(ClearMac, 0xFFFFu, 0x0028u, None),
    KATANA_RULE(DivideInitializeSigned, 0xF00Fu, 0x2007u, RegisterMRegisterN),
    KATANA_RULE(DivideStep, 0xF00Fu, 0x3004u, RegisterMRegisterN),
    KATANA_RULE(MultiplyAccumulateWord, 0xF00Fu, 0x400Fu, RegisterMRegisterN),
    KATANA_RULE(MultiplyAccumulateLong, 0xF00Fu, 0x000Fu, RegisterMRegisterN),
    KATANA_RULE(DoubleMultiplySignedLong, 0xF00Fu, 0x300Du, RegisterMRegisterN),
    KATANA_RULE(DoubleMultiplyUnsignedLong, 0xF00Fu, 0x3005u, RegisterMRegisterN),
    KATANA_RULE(MultiplyLong, 0xF00Fu, 0x0007u, RegisterMRegisterN),
    KATANA_RULE(MultiplySignedWord, 0xF00Fu, 0x200Fu, RegisterMRegisterN),
    KATANA_RULE(MultiplyUnsignedWord, 0xF00Fu, 0x200Eu, RegisterMRegisterN),

    KATANA_RULE(AndRegister, 0xF00Fu, 0x2009u, RegisterMRegisterN),
    KATANA_RULE(XorRegister, 0xF00Fu, 0x200Au, RegisterMRegisterN),
    KATANA_RULE(OrRegister, 0xF00Fu, 0x200Bu, RegisterMRegisterN),
    KATANA_RULE(AndImmediate, 0xFF00u, 0xC900u, Immediate8Register0),
    KATANA_RULE(XorImmediate, 0xFF00u, 0xCA00u, Immediate8Register0),
    KATANA_RULE(OrImmediate, 0xFF00u, 0xCB00u, Immediate8Register0),
    KATANA_RULE(ClearS, 0xFFFFu, 0x0048u, None),
    KATANA_RULE(SetS, 0xFFFFu, 0x0058u, None),
    KATANA_RULE(ClearT, 0xFFFFu, 0x0008u, None),
    KATANA_RULE(SetT, 0xFFFFu, 0x0018u, None),
    KATANA_RULE(CompareEqualImmediate, 0xFF00u, 0x8800u, Immediate8Register0),
    KATANA_RULE(CompareEqualRegister, 0xF00Fu, 0x3000u, RegisterMRegisterN),
    KATANA_RULE(CompareHigherOrSame, 0xF00Fu, 0x3002u, RegisterMRegisterN),
    KATANA_RULE(CompareGreaterOrEqual, 0xF00Fu, 0x3003u, RegisterMRegisterN),
    KATANA_RULE(CompareHigher, 0xF00Fu, 0x3006u, RegisterMRegisterN),
    KATANA_RULE(CompareGreaterThan, 0xF00Fu, 0x3007u, RegisterMRegisterN),
    KATANA_RULE(ComparePositiveOrZero, 0xF0FFu, 0x4011u, RegisterN),
    KATANA_RULE(ComparePositive, 0xF0FFu, 0x4015u, RegisterN),
    KATANA_RULE(CompareString, 0xF00Fu, 0x200Cu, RegisterMRegisterN),
    KATANA_RULE(TestImmediate, 0xFF00u, 0xC800u, Immediate8Register0),
    KATANA_RULE(TestRegister, 0xF00Fu, 0x2008u, RegisterMRegisterN),
    KATANA_RULE(TestByteImmediate, 0xFF00u, 0xCC00u, Immediate8Register0),
    KATANA_RULE(AndByteImmediate, 0xFF00u, 0xCD00u, Immediate8Register0),
    KATANA_RULE(XorByteImmediate, 0xFF00u, 0xCE00u, Immediate8Register0),
    KATANA_RULE(OrByteImmediate, 0xFF00u, 0xCF00u, Immediate8Register0),
    KATANA_RULE(TestAndSetByte, 0xF0FFu, 0x401Bu, RegisterIndirectN),

    KATANA_RULE(MovByteStorePreDecrement, 0xF00Fu, 0x2004u, RegisterMRegisterN),
    KATANA_RULE(MovWordStorePreDecrement, 0xF00Fu, 0x2005u, RegisterMRegisterN),
    KATANA_RULE(MovLongStorePreDecrement, 0xF00Fu, 0x2006u, RegisterMRegisterN),
    KATANA_RULE(MovByteLoadPostIncrement, 0xF00Fu, 0x6004u, RegisterMRegisterN),
    KATANA_RULE(MovWordLoadPostIncrement, 0xF00Fu, 0x6005u, RegisterMRegisterN),
    KATANA_RULE(MovLongLoadPostIncrement, 0xF00Fu, 0x6006u, RegisterMRegisterN),
    KATANA_RULE(MovByteStoreDisplacement, 0xFF00u, 0x8000u, Displacement4),
    KATANA_RULE(MovWordStoreDisplacement, 0xFF00u, 0x8100u, Displacement4),
    KATANA_RULE(MovLongStoreDisplacement, 0xF000u, 0x1000u, Displacement4),
    KATANA_RULE(MovByteLoadDisplacement, 0xFF00u, 0x8400u, Displacement4),
    KATANA_RULE(MovWordLoadDisplacement, 0xFF00u, 0x8500u, Displacement4),
    KATANA_RULE(MovLongLoadDisplacement, 0xF000u, 0x5000u, Displacement4),
    KATANA_RULE(MovByteStoreR0Indexed, 0xF00Fu, 0x0004u, R0Indexed),
    KATANA_RULE(MovWordStoreR0Indexed, 0xF00Fu, 0x0005u, R0Indexed),
    KATANA_RULE(MovLongStoreR0Indexed, 0xF00Fu, 0x0006u, R0Indexed),
    KATANA_RULE(MovByteLoadR0Indexed, 0xF00Fu, 0x000Cu, R0Indexed),
    KATANA_RULE(MovWordLoadR0Indexed, 0xF00Fu, 0x000Du, R0Indexed),
    KATANA_RULE(MovLongLoadR0Indexed, 0xF00Fu, 0x000Eu, R0Indexed),
    KATANA_RULE(MovByteStoreGbrDisplacement, 0xFF00u, 0xC000u, GbrDisplacement8),
    KATANA_RULE(MovWordStoreGbrDisplacement, 0xFF00u, 0xC100u, GbrDisplacement8),
    KATANA_RULE(MovLongStoreGbrDisplacement, 0xFF00u, 0xC200u, GbrDisplacement8),
    KATANA_RULE(MovByteLoadGbrDisplacement, 0xFF00u, 0xC400u, GbrDisplacement8),
    KATANA_RULE(MovWordLoadGbrDisplacement, 0xFF00u, 0xC500u, GbrDisplacement8),
    KATANA_RULE(MovLongLoadGbrDisplacement, 0xFF00u, 0xC600u, GbrDisplacement8),
    KATANA_RULE(MovWordLoadPcRelative, 0xF000u, 0x9000u, PcDisplacement8),
    KATANA_RULE(MovLongLoadPcRelative, 0xF000u, 0xD000u, PcDisplacement8),
    KATANA_RULE(MoveAddressPcRelative, 0xFF00u, 0xC700u, PcDisplacement8),
    KATANA_RULE(MovByteStore, 0xF00Fu, 0x2000u, RegisterMRegisterN),
    KATANA_RULE(MovWordStore, 0xF00Fu, 0x2001u, RegisterMRegisterN),
    KATANA_RULE(MovLongStore, 0xF00Fu, 0x2002u, RegisterMRegisterN),
    KATANA_RULE(MovByteLoad, 0xF00Fu, 0x6000u, RegisterMRegisterN),
    KATANA_RULE(MovWordLoad, 0xF00Fu, 0x6001u, RegisterMRegisterN),
    KATANA_RULE(MovLongLoad, 0xF00Fu, 0x6002u, RegisterMRegisterN),
    KATANA_RULE(MovImmediate, 0xF000u, 0xE000u, Immediate8RegisterN),
    KATANA_RULE(AddImmediate, 0xF000u, 0x7000u, Immediate8RegisterN),
    KATANA_RULE(MovRegister, 0xF00Fu, 0x6003u, RegisterMRegisterN),
    KATANA_RULE(AddRegister, 0xF00Fu, 0x300Cu, RegisterMRegisterN)};

#undef KATANA_FLOW
#undef KATANA_RULE

#define KATANA_SPECIAL(kind, mnemonic, pattern, reg, reg_name, memory, source, privileged)         \
    SpecialRegisterEncodingMetadata {                                                              \
        InstructionKind::kind, mnemonic, 0xF0FFu, pattern, SpecialRegister::reg, reg_name, memory, \
            source, privileged                                                                     \
    }

constexpr std::array kSpecialRegisterMetadata = {
    KATANA_SPECIAL(StoreSpecialRegister, "sts", 0x000Au, Mach, "mach", false, false, false),
    KATANA_SPECIAL(StoreSpecialRegister, "sts", 0x001Au, Macl, "macl", false, false, false),
    KATANA_SPECIAL(StoreSpecialRegister, "sts", 0x002Au, Pr, "pr", false, false, false),
    KATANA_SPECIAL(StoreSpecialRegister, "sts", 0x005Au, Fpul, "fpul", false, false, false),
    KATANA_SPECIAL(StoreSpecialRegister, "sts", 0x006Au, Fpscr, "fpscr", false, false, false),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "sts.l", 0x4002u, Mach, "mach", true, false, false),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "sts.l", 0x4012u, Macl, "macl", true, false, false),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "sts.l", 0x4022u, Pr, "pr", true, false, false),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "sts.l", 0x4052u, Fpul, "fpul", true, false, false),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "sts.l", 0x4062u, Fpscr, "fpscr", true, false, false),
    KATANA_SPECIAL(LoadSpecialRegister, "lds", 0x400Au, Mach, "mach", false, true, false),
    KATANA_SPECIAL(LoadSpecialRegister, "lds", 0x401Au, Macl, "macl", false, true, false),
    KATANA_SPECIAL(LoadSpecialRegister, "lds", 0x402Au, Pr, "pr", false, true, false),
    KATANA_SPECIAL(LoadSpecialRegister, "lds", 0x405Au, Fpul, "fpul", false, true, false),
    KATANA_SPECIAL(LoadSpecialRegister, "lds", 0x406Au, Fpscr, "fpscr", false, true, false),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "lds.l", 0x4006u, Mach, "mach", true, true, false),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "lds.l", 0x4016u, Macl, "macl", true, true, false),
    KATANA_SPECIAL(LoadSpecialRegisterPostIncrement, "lds.l", 0x4026u, Pr, "pr", true, true, false),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "lds.l", 0x4056u, Fpul, "fpul", true, true, false),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "lds.l", 0x4066u, Fpscr, "fpscr", true, true, false),

    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x0002u, Sr, "sr", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x0012u, Gbr, "gbr", false, false, false),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x0022u, Vbr, "vbr", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x0032u, Ssr, "ssr", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x0042u, Spc, "spc", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x003Au, Sgr, "sgr", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x00FAu, Dbr, "dbr", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegisterPreDecrement, "stc.l", 0x4003u, Sr, "sr", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x4013u, Gbr, "gbr", true, false, false),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x4023u, Vbr, "vbr", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x4033u, Ssr, "ssr", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x4043u, Spc, "spc", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x4032u, Sgr, "sgr", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x40F2u, Dbr, "dbr", true, false, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x400Eu, Sr, "sr", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x401Eu, Gbr, "gbr", false, true, false),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x402Eu, Vbr, "vbr", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x403Eu, Ssr, "ssr", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x404Eu, Spc, "spc", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x40FAu, Dbr, "dbr", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegisterPostIncrement, "ldc.l", 0x4007u, Sr, "sr", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x4017u, Gbr, "gbr", true, true, false),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x4027u, Vbr, "vbr", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x4037u, Ssr, "ssr", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x4047u, Spc, "spc", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x40F6u, Dbr, "dbr", true, true, true),

    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x0082u, Bank0, "r0_bank", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x0092u, Bank1, "r1_bank", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x00A2u, Bank2, "r2_bank", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x00B2u, Bank3, "r3_bank", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x00C2u, Bank4, "r4_bank", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x00D2u, Bank5, "r5_bank", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x00E2u, Bank6, "r6_bank", false, false, true),
    KATANA_SPECIAL(StoreSpecialRegister, "stc", 0x00F2u, Bank7, "r7_bank", false, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x4083u, Bank0, "r0_bank", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x4093u, Bank1, "r1_bank", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x40A3u, Bank2, "r2_bank", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x40B3u, Bank3, "r3_bank", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x40C3u, Bank4, "r4_bank", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x40D3u, Bank5, "r5_bank", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x40E3u, Bank6, "r6_bank", true, false, true),
    KATANA_SPECIAL(
        StoreSpecialRegisterPreDecrement, "stc.l", 0x40F3u, Bank7, "r7_bank", true, false, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x408Eu, Bank0, "r0_bank", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x409Eu, Bank1, "r1_bank", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x40AEu, Bank2, "r2_bank", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x40BEu, Bank3, "r3_bank", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x40CEu, Bank4, "r4_bank", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x40DEu, Bank5, "r5_bank", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x40EEu, Bank6, "r6_bank", false, true, true),
    KATANA_SPECIAL(LoadSpecialRegister, "ldc", 0x40FEu, Bank7, "r7_bank", false, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x4087u, Bank0, "r0_bank", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x4097u, Bank1, "r1_bank", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x40A7u, Bank2, "r2_bank", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x40B7u, Bank3, "r3_bank", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x40C7u, Bank4, "r4_bank", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x40D7u, Bank5, "r5_bank", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x40E7u, Bank6, "r6_bank", true, true, true),
    KATANA_SPECIAL(
        LoadSpecialRegisterPostIncrement, "ldc.l", 0x40F7u, Bank7, "r7_bank", true, true, true)};

#undef KATANA_SPECIAL

} // namespace

std::span<const InstructionMetadata> instruction_metadata() noexcept {
    return kInstructionMetadata;
}

std::span<const SpecialRegisterEncodingMetadata> special_register_encoding_metadata() noexcept {
    return kSpecialRegisterMetadata;
}

const InstructionMetadata* metadata_for_kind(const InstructionKind kind) noexcept {
    for (const auto& metadata : kInstructionMetadata) {
        if (metadata.kind == kind) {
            return &metadata;
        }
    }
    return nullptr;
}

} // namespace katana::sh4
