// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's shader records read: Direct3D 9 shader bytecode (the token stream D3D9's
// CreateVertexShader and CreatePixelShader take, what CGxShader::m_code holds) decoded into a list
// of instructions with their operands, and what the program declares (its inputs, outputs and
// samplers, its constants, the registers it reads). Vertex programs vs_1_1 to vs_3_0, pixel
// programs ps_1_1 to ps_3_0: the profiles the client's archives carry (Shaders\Vertex\vs_*,
// Shaders\Pixel\ps_*). Below SM2 the instruction tokens do not say their length; the operand count
// of each opcode does.
//
// A record this decoder cannot read is refused with the reason (an opcode it does not know, a
// register out of range, a malformed token): the client then draws without it, as when D3D9
// refuses a shader. Portable, no device.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wxl::vk::shaders::dx9
{
    /// D3DSHADER_INSTRUCTION_OPCODE_TYPE, the ones the decoder knows.
    enum Opcode : uint16_t
    {
        kNop = 0, kMov = 1, kAdd = 2, kSub = 3, kMad = 4, kMul = 5, kRcp = 6, kRsq = 7, kDp3 = 8, kDp4 = 9,
        kMin = 10, kMax = 11, kSlt = 12, kSge = 13, kExp = 14, kLog = 15, kLit = 16, kDst = 17, kLrp = 18,
        kFrc = 19, kM4x4 = 20, kM4x3 = 21, kM3x4 = 22, kM3x3 = 23, kM3x2 = 24, kCall = 25, kCallNz = 26,
        kLoop = 27, kRet = 28, kEndLoop = 29, kLabel = 30, kDcl = 31, kPow = 32, kCrs = 33, kSgn = 34,
        kAbs = 35, kNrm = 36, kSinCos = 37, kRep = 38, kEndRep = 39, kIf = 40, kIfc = 41, kElse = 42,
        kEndIf = 43, kBreak = 44, kBreakc = 45, kMova = 46, kDefB = 47, kDefI = 48,
        kTexCoord = 64, kTexKill = 65, kTex = 66, kTexBem = 67, kTexBemL = 68, kTexReg2Ar = 69,
        kTexReg2Gb = 70, kTexM3x2Pad = 71, kTexM3x2Tex = 72, kTexM3x3Pad = 73, kTexM3x3Tex = 74,
        kTexM3x3Spec = 76, kTexM3x3VSpec = 77, kExpP = 78, kLogP = 79, kCnd = 80, kDef = 81,
        kTexReg2Rgb = 82, kTexDp3Tex = 83, kTexM3x2Depth = 84, kTexDp3 = 85, kTexM3x3 = 86,
        kTexDepth = 87, kCmp = 88, kBem = 89, kDp2Add = 90, kDsx = 91, kDsy = 92, kTexLdd = 93,
        kSetP = 94, kTexLdl = 95, kBreakP = 96,
        kPhase = 0xFFFD,
    };

    /// D3DSHADER_PARAM_REGISTER_TYPE.
    enum class Reg : uint8_t
    {
        Temp = 0,
        Input = 1,
        Const = 2,
        Address = 3, ///< a0 in a vertex program; t# (Texture) in a pixel program
        RastOut = 4, ///< oPos (0), oFog (1), oPts (2)
        AttrOut = 5, ///< oD0, oD1
        Output = 6,  ///< oT# below vs_3_0, o# in vs_3_0
        ConstInt = 7,
        ColorOut = 8,
        DepthOut = 9,
        Sampler = 10,
        Const2 = 11,
        Const3 = 12,
        Const4 = 13,
        ConstBool = 14,
        Loop = 15,
        TempFloat16 = 16,
        Misc = 17,
        Label = 18,
        Predicate = 19,
    };

    /// D3DSHADER_PARAM_SRCMOD_TYPE.
    enum class SrcMod : uint8_t
    {
        None = 0, Neg = 1, Bias = 2, BiasNeg = 3, Sign = 4, SignNeg = 5, Comp = 6, X2 = 7, X2Neg = 8,
        Dz = 9, Dw = 10, Abs = 11, AbsNeg = 12, Not = 13,
    };

    struct Operand
    {
        Reg      type = Reg::Temp;
        uint32_t index = 0;
        // A destination's.
        uint8_t  mask = 0xF;       ///< write mask, bit 0 = x
        bool     saturate = false;
        bool     partial = false;
        bool     centroid = false;
        int8_t   shift = 0;        ///< ps 1.x: the result times 2^shift
        // A source's.
        uint8_t  swizzle = 0xE4;   ///< two bits per component, x first: 0xE4 is .xyzw
        SrcMod   modifier = SrcMod::None;
        // Relative addressing: the register indexed by a0.<component> or aL.
        bool     relative = false;
        Reg      rel_type = Reg::Address;
        uint32_t rel_index = 0;
        uint8_t  rel_component = 0;

        uint8_t Component(uint32_t i) const { return static_cast<uint8_t>((swizzle >> (2 * i)) & 3); }
    };

    struct Instruction
    {
        uint16_t op = kNop;
        uint8_t  control = 0; ///< ifc/breakc/setp: the comparison (1 gt, 2 eq, 3 ge, 4 lt, 5 ne, 6 le); texld: 1 projected, 2 biased
        bool     coissue = false;
        bool     has_dst = false;
        Operand  dst;
        uint32_t src_count = 0;
        Operand  src[4];
    };

    /// A dcl: an input or output with its meaning, or a sampler with its texture type.
    struct Declaration
    {
        Reg      type = Reg::Input;
        uint32_t index = 0;
        uint8_t  mask = 0xF;
        uint32_t usage = 0;        ///< D3DDECLUSAGE
        uint32_t usage_index = 0;
        uint32_t texture = 0;      ///< a sampler's D3DSAMPLER_TEXTURE_TYPE: 2 2D, 3 cube, 4 volume
    };

    /// A def, defi or defb: a constant the program sets itself.
    struct Definition
    {
        Reg      type = Reg::Const;
        uint32_t index = 0;
        uint32_t bits[4] = {}; ///< floats (def), ints (defi), a bool (defb, bits[0])
    };

    struct Program
    {
        bool     pixel = false;
        uint8_t  major = 0;
        uint8_t  minor = 0;
        std::vector<Instruction> code;
        std::vector<Declaration> declarations;
        std::vector<Definition>  definitions;

        // What the code reads and writes, gathered as it is decoded.
        uint32_t temps = 0;           ///< r0..r(temps-1)
        uint32_t const_rows = 0;      ///< c0..c(const_rows-1) read with a fixed index
        bool     relative_const = false; ///< a c register read through a0 or aL
        uint32_t textures = 0;        ///< ps 1.x/2.0: t0..t(textures-1) read or written
        uint32_t outputs = 0;         ///< vs below 3.0: oT0..oT(outputs-1)
        uint32_t sampler_mask = 0;    ///< the samplers sampled (s# of a texld, or the stage of a ps 1.x tex)
    };

    /// "vs_3_0", "ps_1_4", from a program's version.
    std::string ProfileName(const Program& program);

    /**
     * @brief Decodes @p count tokens of @p tokens (the record's bytes as little-endian words).
     * @return false with @p why when the record is not a program this decoder reads.
     */
    bool Decode(const uint32_t* tokens, size_t count, Program& out, std::string& why);
}
