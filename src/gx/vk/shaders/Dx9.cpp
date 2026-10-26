// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's shader records decoded.

#include "gx/vk/shaders/Dx9.hpp"

#include <algorithm>
#include <cstdio>

namespace wxl::vk::shaders::dx9
{
    namespace
    {
        bool Fail(std::string& why, const char* format, unsigned a = 0, unsigned b = 0)
        {
            char text[160];
            std::snprintf(text, sizeof text, format, a, b);
            why = text;
            return false;
        }

        Reg TypeOf(uint32_t token) { return static_cast<Reg>(((token >> 28) & 0x7) | ((token >> 8) & 0x18)); }

        /// The operands (destination included) of an opcode below SM2, whose tokens do not say their
        /// length; -1 for one the decoder does not know there.
        int OperandsSm1(uint16_t op, bool pixel, uint8_t minor)
        {
            switch (op)
            {
            case kNop: return 0;
            case kMov: case kRcp: case kRsq: case kExp: case kLog: case kLit: case kFrc: case kExpP: case kLogP: return 2;
            case kAdd: case kSub: case kMul: case kDp3: case kDp4: case kMin: case kMax: case kSlt: case kSge: case kDst:
            case kM4x4: case kM4x3: case kM3x4: case kM3x3: case kM3x2: return 3;
            case kMad: case kLrp: return 4;
            case kDcl: return 2;
            case kDef: return 5;
            default: break;
            }
            if (!pixel) return -1;
            switch (op)
            {
            case kTexCoord: return minor >= 4 ? 2 : 1;
            case kTex: return minor >= 4 ? 2 : 1;
            case kTexKill: return 1;
            case kTexBem: case kTexBemL: case kTexReg2Ar: case kTexReg2Gb: case kTexM3x2Pad: case kTexM3x2Tex: case kTexM3x3Pad:
            case kTexM3x3Tex: case kTexM3x3VSpec: case kTexReg2Rgb: case kTexDp3Tex: case kTexM3x2Depth: case kTexDp3: case kTexM3x3: return 2;
            case kTexM3x3Spec: case kBem: return 3;
            case kTexDepth: return 1;
            case kCnd: case kCmp: return 4;
            default: return -1;
            }
        }

        /// Opcodes whose first operand is not a destination.
        bool NoDestination(uint16_t op)
        {
            switch (op)
            {
            case kNop: case kCall: case kCallNz: case kLoop: case kRet: case kEndLoop: case kLabel: case kRep: case kEndRep:
            case kIf: case kIfc: case kElse: case kEndIf: case kBreak: case kBreakc: case kBreakP: return true;
            default: return false;
            }
        }

        /// The opcodes this decoder reads in a program of @p pixel's kind.
        bool Known(uint16_t op, bool pixel, uint8_t major)
        {
            switch (op)
            {
            case kNop: case kMov: case kAdd: case kSub: case kMad: case kMul: case kRcp: case kRsq: case kDp3: case kDp4:
            case kMin: case kMax: case kSlt: case kSge: case kExp: case kLog: case kLrp: case kFrc: case kM4x4: case kM4x3:
            case kM3x4: case kM3x3: case kM3x2: case kLoop: case kEndLoop: case kDcl: case kPow: case kAbs: case kNrm:
            case kSinCos: case kRep: case kEndRep: case kIf: case kIfc: case kElse: case kEndIf: case kBreak: case kBreakc:
            case kDefB: case kDefI: case kDef: case kExpP: case kLogP: case kCrs: case kSgn: case kDst: case kLit: return true;
            case kRet: return true; // only as the program's last instruction (the translator checks)
            case kMova: return !pixel;
            case kTexCoord: case kTex: case kTexKill: case kCmp: case kDp2Add: case kDsx: case kDsy: case kTexLdl: case kCnd:
                return pixel || op == kTexLdl;
            case kTexBem: return pixel && major == 1;
            default: return false;
            }
        }

        struct Cursor
        {
            const uint32_t* tokens;
            size_t          at;
            size_t          end;
        };

        bool ReadRelative(Cursor& c, Operand& o, uint8_t major, std::string& why)
        {
            if (major < 2)
            {
                // vs 1.1: a0.x, implied.
                o.rel_type      = Reg::Address;
                o.rel_index     = 0;
                o.rel_component = 0;
                return true;
            }
            if (c.at >= c.end) return Fail(why, "a relative operand without its address token");
            const uint32_t t = c.tokens[c.at++];
            o.rel_type       = TypeOf(t);
            o.rel_index      = t & 0x7FF;
            o.rel_component  = static_cast<uint8_t>((t >> 16) & 3);
            if (o.rel_type != Reg::Address && o.rel_type != Reg::Loop) return Fail(why, "a relative address through register type %u", static_cast<unsigned>(o.rel_type));
            return true;
        }

        bool ReadDestination(Cursor& c, Operand& o, uint8_t major, std::string& why)
        {
            if (c.at >= c.end) return Fail(why, "an instruction without its destination");
            const uint32_t t = c.tokens[c.at++];
            o.type           = TypeOf(t);
            o.index          = t & 0x7FF;
            o.mask           = static_cast<uint8_t>((t >> 16) & 0xF);
            const uint32_t m = (t >> 20) & 0xF;
            o.saturate       = (m & 1) != 0;
            o.partial        = (m & 2) != 0;
            o.centroid       = (m & 4) != 0;
            const int32_t s  = static_cast<int32_t>((t >> 24) & 0xF);
            o.shift          = static_cast<int8_t>(s >= 8 ? s - 16 : s);
            if (t & (1u << 13))
            {
                o.relative = true;
                if (!ReadRelative(c, o, major, why)) return false;
            }
            return true;
        }

        bool ReadSource(Cursor& c, Operand& o, uint8_t major, std::string& why)
        {
            if (c.at >= c.end) return Fail(why, "an instruction without its source");
            const uint32_t t = c.tokens[c.at++];
            o.type           = TypeOf(t);
            o.index          = t & 0x7FF;
            o.swizzle        = static_cast<uint8_t>((t >> 16) & 0xFF);
            o.modifier       = static_cast<SrcMod>((t >> 24) & 0xF);
            if (t & (1u << 13))
            {
                o.relative = true;
                if (!ReadRelative(c, o, major, why)) return false;
            }
            return true;
        }

        /// The bounds of each register type in a program of this kind and version; false for a type it has none of.
        bool Bound(const Program& p, Reg type, bool destination, uint32_t& limit)
        {
            switch (type)
            {
            case Reg::Temp: limit = 32; return true;
            case Reg::Input: limit = p.pixel ? (p.major >= 3 ? 10 : 2) : 16; return true;
            case Reg::Const: limit = 256; return true;
            case Reg::Address: limit = p.pixel ? 8 : 1; return true;
            case Reg::RastOut: limit = 3; return !p.pixel && destination;
            case Reg::AttrOut: limit = 2; return !p.pixel && destination;
            case Reg::Output: limit = p.major >= 3 ? 12 : 8; return !p.pixel && destination;
            case Reg::ConstInt: limit = 16; return !destination;
            case Reg::ColorOut: limit = 4; return p.pixel && destination;
            case Reg::ConstBool: limit = 16; return !destination;
            case Reg::Loop: limit = 1; return !destination;
            case Reg::Sampler: limit = 16; return !destination;
            default: return false;
            }
        }

        bool CheckOperand(const Program& p, const Operand& o, bool destination, std::string& why)
        {
            uint32_t limit = 0;
            if (!Bound(p, o.type, destination, limit))
                return Fail(why, destination ? "a destination of register type %u" : "a source of register type %u",
                            static_cast<unsigned>(o.type));
            if (o.index >= limit) return Fail(why, "register %u of type %u out of range", o.index, static_cast<unsigned>(o.type));
            if (o.relative && o.type != Reg::Const && !(o.type == Reg::Input && p.pixel && p.major >= 3))
                return Fail(why, "relative addressing of register type %u", static_cast<unsigned>(o.type));
            if (destination && o.relative) return Fail(why, "a destination addressed relatively");
            if (!destination && static_cast<uint8_t>(o.modifier) > static_cast<uint8_t>(SrcMod::Not))
                return Fail(why, "source modifier %u", static_cast<unsigned>(o.modifier));
            return true;
        }
    }

    std::string ProfileName(const Program& program)
    {
        char text[16];
        std::snprintf(text, sizeof text, "%s_%u_%u", program.pixel ? "ps" : "vs", program.major, program.minor);
        return text;
    }

    bool Decode(const uint32_t* tokens, size_t count, Program& out, std::string& why)
    {
        out = Program{};
        if (!tokens || count < 2) return Fail(why, "too short to be a shader");
        const uint32_t version = tokens[0];
        const uint32_t kind    = version >> 16;
        if (kind != 0xFFFE && kind != 0xFFFF) return Fail(why, "not a D3D9 shader (version token %08x)", version);
        out.pixel = kind == 0xFFFF;
        out.major = static_cast<uint8_t>((version >> 8) & 0xFF);
        out.minor = static_cast<uint8_t>(version & 0xFF);
        const bool known = out.pixel ? (out.major == 1 && out.minor >= 1 && out.minor <= 4) || (out.major == 2 && out.minor <= 1) ||
                                           (out.major == 3 && out.minor == 0)
                                     : (out.major == 1 && out.minor == 1) || (out.major == 2 && out.minor <= 1) ||
                                           (out.major == 3 && out.minor == 0);
        if (!known) return Fail(why, "shader model %u.%u", out.major, out.minor);

        bool   ended = false;
        size_t i     = 1;
        while (i < count)
        {
            const uint32_t t  = tokens[i];
            const uint16_t op = static_cast<uint16_t>(t & 0xFFFF);
            if (op == 0xFFFF)
            {
                ended = true;
                break;
            }
            if (op == 0xFFFE)
            {
                i += 1 + ((t >> 16) & 0x7FFF);
                continue;
            }
            if (op == kPhase)
            {
                if (!out.pixel || out.major != 1 || out.minor != 4) return Fail(why, "a phase outside ps_1_4");
                Instruction phase;
                phase.op = kPhase;
                out.code.push_back(phase);
                ++i;
                continue;
            }
            if (t & 0x10000000) return Fail(why, "a predicated instruction (opcode %u)", op);
            if (!Known(op, out.pixel, out.major)) return Fail(why, "opcode %u", op);
            size_t length = 0;
            if (out.major >= 2) length = (t >> 24) & 0xF;
            else
            {
                const int operands = OperandsSm1(op, out.pixel, out.minor);
                if (operands < 0) return Fail(why, "opcode %u below shader model 2", op);
                length = static_cast<size_t>(operands);
            }
            if (i + 1 + length > count) return Fail(why, "an instruction (opcode %u) past the end", op);
            Cursor c{tokens, i + 1, i + 1 + length};

            if (op == kDcl)
            {
                if (length < 2) return Fail(why, "a short dcl");
                const uint32_t info = tokens[c.at++];
                Operand        reg;
                if (!ReadDestination(c, reg, out.major, why)) return false;
                Declaration d;
                d.type        = reg.type;
                d.index       = reg.index;
                d.mask        = reg.mask;
                d.usage       = info & 0x1F;
                d.usage_index = (info >> 16) & 0xF;
                d.texture     = (info >> 27) & 0xF;
                if (d.type == Reg::Sampler && d.texture != 2 && d.texture != 3)
                    return Fail(why, "a sampler of texture type %u", d.texture);
                out.declarations.push_back(d);
                i += 1 + length;
                continue;
            }
            if (op == kDef || op == kDefI || op == kDefB)
            {
                Operand reg;
                if (!ReadDestination(c, reg, out.major, why)) return false;
                Definition d;
                d.type  = reg.type;
                d.index = reg.index;
                const size_t values = op == kDefB ? 1 : 4;
                if (c.end - c.at < values) return Fail(why, "a short def");
                for (size_t v = 0; v < values; ++v) d.bits[v] = tokens[c.at++];
                const Reg want = op == kDef ? Reg::Const : op == kDefI ? Reg::ConstInt : Reg::ConstBool;
                if (d.type != want || d.index >= (want == Reg::Const ? 256u : 16u)) return Fail(why, "a def of register type %u", static_cast<unsigned>(d.type));
                out.definitions.push_back(d);
                i += 1 + length;
                continue;
            }

            Instruction ins;
            ins.op      = op;
            ins.control = static_cast<uint8_t>((t >> 16) & 0xFF);
            ins.coissue = (t & 0x40000000) != 0;
            // texkill names a register in a destination's token; the others with one write it.
            ins.has_dst = !NoDestination(op);
            if (ins.has_dst && length)
            {
                if (!ReadDestination(c, ins.dst, out.major, why)) return false;
                // ps 1.x texture instructions write a t register; texkill tests one.
                if (!CheckOperand(out, ins.dst, op != kTexKill, why))
                {
                    // texkill reads its register: a source's bounds.
                    if (op != kTexKill || !CheckOperand(out, ins.dst, false, why)) return false;
                }
            }
            while (c.at < c.end)
            {
                if (ins.src_count == 4) return Fail(why, "more than four sources (opcode %u)", op);
                Operand& s = ins.src[ins.src_count++];
                if (!ReadSource(c, s, out.major, why)) return false;
                if (!CheckOperand(out, s, false, why)) return false;
            }
            out.code.push_back(ins);
            i += 1 + length;
        }
        if (!ended) return Fail(why, "no end token");

        // What the code reads and writes.
        auto defined = [&out](Reg type, uint32_t index) {
            for (const Definition& d : out.definitions)
                if (d.type == type && d.index == index) return true;
            return false;
        };
        auto note = [&out, &defined](const Operand& o) {
            switch (o.type)
            {
            case Reg::Temp: out.temps = std::max(out.temps, o.index + 1); break;
            case Reg::Const:
                if (o.relative) out.relative_const = true;
                else if (!defined(Reg::Const, o.index)) out.const_rows = std::max(out.const_rows, o.index + 1);
                break;
            case Reg::Address:
                if (out.pixel) out.textures = std::max(out.textures, o.index + 1);
                break;
            case Reg::Output:
                if (!out.pixel && out.major < 3) out.outputs = std::max(out.outputs, o.index + 1);
                break;
            default: break;
            }
        };
        for (const Instruction& ins : out.code)
        {
            if (ins.has_dst) note(ins.dst);
            for (uint32_t s = 0; s < ins.src_count; ++s) note(ins.src[s]);
            if (ins.op == kTex || ins.op == kTexLdl)
            {
                if (out.pixel && out.major == 1) out.sampler_mask |= 1u << ins.dst.index;
                else if (ins.src_count >= 2 && ins.src[1].type == Reg::Sampler) out.sampler_mask |= 1u << ins.src[1].index;
            }
            if (ins.op == kTexBem) out.sampler_mask |= 1u << ins.dst.index;
        }
        return true;
    }
}
