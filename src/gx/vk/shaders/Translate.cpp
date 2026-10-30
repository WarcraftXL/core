// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's DX9 programs translated to SPIR-V.

#include "gx/vk/shaders/Translate.hpp"

#include "gx/vk/shaders/Dx9.hpp"
#include "gx/vk/shaders/Interface.hpp"
#include "gx/vk/shaders/Spirv.hpp"

#include <spirv/unified1/GLSL.std.450.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wxl::vk::shaders
{
    namespace
    {
        using spirv::Id;
        using namespace dx9;

        constexpr float kFltMax = 3.402823466e38f;

        /// The push block's members, in the order of shaders::Push.
        enum PushMember : int32_t
        {
            kPushData = 0,
            kPushVertex,
            kPushPixel,
            kPushAlphaRef,
            kPushFlags,
            kPushClipPlane,
            kPushFogColor,
            kPushPointSize,
            kPushPointRange,
            kPushStages,
        };

        class Writer
        {
        public:
            Writer(const Program& program, const Options& options)
                : p_(program), o_(options), m_(program.pixel ? SpvExecutionModelFragment : SpvExecutionModelVertex)
            {
            }

            bool Run(Translation& out, std::string& why);

        private:
            struct Construct
            {
                enum Kind
                {
                    If,
                    Loop,
                } kind = If;
                Id   merge = 0;
                Id   otherwise = 0; ///< an if's else block
                bool else_seen = false;
                Id   next = 0;      ///< a loop's continue block
                Id   counter = 0;   ///< a loop's iteration count (a local int)
                Id   al = 0;        ///< a loop's aL (a local int); 0 for rep
                int32_t step = 0;
            };

            bool Fail(std::string& why, const char* format, unsigned a = 0, unsigned b = 0)
            {
                char text[200];
                std::snprintf(text, sizeof text, format, a, b);
                why = text;
                return false;
            }

            // --- Building blocks ------------------------------------------------------------------
            Id F(float v) { return m_.ConstFloat(v); }
            Id I(int32_t v) { return m_.ConstInt(v); }
            Id U(uint32_t v) { return m_.ConstUint(v); }
            Id V4(float x, float y, float z, float w) { return m_.ConstComposite(vec4_, {F(x), F(y), F(z), F(w)}); }
            Id Splat(Id scalar) { return m_.Op(SpvOpCompositeConstruct, vec4_, {scalar, scalar, scalar, scalar}); }
            Id Comp(Id v, uint32_t i) { return m_.Op(SpvOpCompositeExtract, f32_, {v, i}); }
            Id Ext(uint32_t op, Id type, std::initializer_list<Id> args) { return m_.Ext(type, op, args); }
            Id Add(Id a, Id b) { return m_.Op(SpvOpFAdd, vec4_, {a, b}); }
            Id Mul(Id a, Id b) { return m_.Op(SpvOpFMul, vec4_, {a, b}); }
            Id Shuffle(Id a, Id b, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
            {
                return m_.Op(SpvOpVectorShuffle, vec4_, {a, b, x, y, z, w});
            }
            Id Swizzle(Id v, uint8_t swizzle)
            {
                if (swizzle == 0xE4) return v;
                return Shuffle(v, v, swizzle & 3, (swizzle >> 2) & 3, (swizzle >> 4) & 3, (swizzle >> 6) & 3);
            }
            /// @p old with the components of @p mask taken from @p value.
            Id Merge(Id old, Id value, uint8_t mask)
            {
                if ((mask & 0xF) == 0xF) return value;
                return Shuffle(old, value, (mask & 1) ? 4 : 0, (mask & 2) ? 5 : 1, (mask & 4) ? 6 : 2, (mask & 8) ? 7 : 3);
            }
            Id Load(Id type, Id pointer) { return m_.Op(SpvOpLoad, type, {pointer}); }
            void Store(Id pointer, Id value) { m_.OpVoid(SpvOpStore, {pointer, value}); }
            Id Local(Id type, Id initial)
            {
                const Id var = m_.Local(m_.Pointer(SpvStorageClassFunction, type));
                Store(var, initial);
                return var;
            }
            Id PushMember(PushMember member, Id type)
            {
                return Load(type, m_.Op(SpvOpAccessChain, m_.Pointer(SpvStorageClassPushConstant, type), {push_, I(member)}));
            }
            /// Whether bit @p bit of the push constants' flags is set.
            Id Flag(uint32_t bit)
            {
                const Id masked = m_.Op(SpvOpBitwiseAnd, u32_, {flags_, U(bit)});
                return m_.Op(SpvOpINotEqual, bool_, {masked, U(0)});
            }
            Id Clamp(Id v, float lo, float hi) { return Ext(GLSLstd450NClamp, vec4_, {v, V4(lo, lo, lo, lo), V4(hi, hi, hi, hi)}); }
            Id ClampScalar(Id v, float lo, float hi) { return Ext(GLSLstd450NClamp, f32_, {v, F(lo), F(hi)}); }
            Id Variable(Id type, SpvStorageClass storage)
            {
                const Id var = m_.Variable(m_.Pointer(storage, type), storage);
                m_.AddInterface(var);
                return var;
            }

            // --- The program's registers ----------------------------------------------------------
            Id  Fetch(const Operand& s, std::string& why);
            Id  Source(const Operand& s, std::string& why);
            Id  Modify(Id v, SrcMod modifier);
            Id  FileRow(Id index);
            Id  ConstRead(const Operand& s, std::string& why);
            Id  RelativeIndex(const Operand& s, std::string& why);
            Id  BoneRead(const Operand& s, std::string& why);
            bool Write(const Operand& d, Id value, std::string& why);
            const Definition* Defined(Reg type, uint32_t index) const;
            Id  DefConst(const Definition& d);
            Id  TexcoordInput(uint32_t set);

            // --- Instructions ------------------------------------------------------------------------
            bool Emit(const Instruction& ins, size_t at, std::string& why);
            bool Flow(const Instruction& ins, size_t at, std::string& why);
            bool Texture(const Instruction& ins, std::string& why);
            Id   Sample(uint32_t stage, Id coord, int mode, std::string& why);
            void Kill(Id condition);
            Id   Compare(uint8_t comparison, Id a, Id b);

            bool Prologue(std::string& why);
            bool Epilogue(std::string& why);

            const Program& p_;
            const Options& o_;
            spirv::Module  m_;

            // Types.
            Id f32_ = 0, vec2_ = 0, vec3_ = 0, vec4_ = 0, i32_ = 0, ivec4_ = 0, u32_ = 0, uvec2_ = 0, bool_ = 0, bvec4_ = 0;
            Id fn_vec4_ = 0;
            // Globals.
            Id push_ = 0;
            Id flags_ = 0;
            Id regs_ = 0;          ///< the register file's pointer, loaded at the start
            Id row_pointer_ = 0;   ///< PhysicalStorageBuffer pointer to a vec4
            // A program reading bones: where they are (the palette's address, else the register
            // file's), what a0 + c# becomes there, and the last row a read may take.
            Id palette_pointer_ = 0;
            Id palette_address_ = 0;
            Id palette_offset_ = 0;
            Id palette_last_ = 0;
            Id images2d_ = 0, images_cube_ = 0, samplers_ = 0;
            Id point_coord_ = 0;
            // Registers, as local variables (or values computed once at the start).
            std::vector<Id> temps_;
            Id  a0_ = 0;
            std::vector<Id> textures_; ///< ps 1.1-1.3: t#
            Id  inputs_[16] = {};      ///< the v# values
            Id  varying_in_[kVaryings] = {}; ///< a pixel program's varyings, loaded
            Id  out_pos_ = 0, out_fog_ = 0, out_pts_ = 0;
            Id  out_color_[2] = {};
            Id  out_tex_[8] = {};
            Id  out_reg_[12] = {};     ///< vs 3.0: o#
            Id  out_c_[4] = {};        ///< a pixel program's oC#
            std::vector<Construct> constructs_;
            ProgramInfo info_;
            bool ps1_ = false;
        };

        const Definition* Writer::Defined(Reg type, uint32_t index) const
        {
            for (const Definition& d : p_.definitions)
                if (d.type == type && d.index == index) return &d;
            return nullptr;
        }

        Id Writer::DefConst(const Definition& d)
        {
            float v[4];
            std::memcpy(v, d.bits, sizeof v);
            return V4(v[0], v[1], v[2], v[3]);
        }

        Id Writer::FileRow(Id index)
        {
            const Id pointer = m_.Op(SpvOpAccessChain, row_pointer_, {regs_, I(0), index});
            return m_.Op(SpvOpLoad, vec4_, {pointer, SpvMemoryAccessAlignedMask, 16});
        }

        Id Writer::RelativeIndex(const Operand& s, std::string& why)
        {
            Id base;
            if (s.rel_type == Reg::Loop)
            {
                const Construct* loop = nullptr;
                for (auto it = constructs_.rbegin(); it != constructs_.rend() && !loop; ++it)
                    if (it->kind == Construct::Loop && it->al) loop = &*it;
                if (!loop)
                {
                    Fail(why, "aL read outside a loop");
                    return 0;
                }
                base = Load(i32_, loop->al);
            }
            else
            {
                if (p_.pixel || !a0_)
                {
                    Fail(why, "a0 read where there is none");
                    return 0;
                }
                base = m_.Op(SpvOpCompositeExtract, i32_, {Load(ivec4_, a0_), s.rel_component});
            }
            const Id index = m_.Op(SpvOpIAdd, i32_, {base, I(static_cast<int32_t>(s.index))});
            return Ext(GLSLstd450SClamp, i32_, {index, I(0), I(static_cast<int32_t>(kRegisterRows) - 1)});
        }

        // A bone's row (c31..c33 through a0): row a0 + c# - 31 of the draw's palette when it has
        // one, else the register file's row a0 + c#, as D3D9 reads it.
        Id Writer::BoneRead(const Operand& s, std::string& why)
        {
            if (!a0_)
            {
                Fail(why, "a0 read where there is none");
                return 0;
            }
            const Id base  = m_.Op(SpvOpIAdd, i32_, {m_.Op(SpvOpCompositeExtract, i32_, {Load(ivec4_, a0_), s.rel_component}),
                                                     I(static_cast<int32_t>(s.index))});
            const Id index = Ext(GLSLstd450SClamp, i32_, {m_.Op(SpvOpIAdd, i32_, {base, palette_offset_}), I(0), palette_last_});
            const Id file  = m_.Op(SpvOpBitcast, palette_pointer_, {palette_address_});
            const Id row   = m_.Op(SpvOpAccessChain, row_pointer_, {file, I(0), index});
            Id v = m_.Op(SpvOpLoad, vec4_, {row, SpvMemoryAccessAlignedMask, 16});
            for (const Definition& d : p_.definitions)
            {
                if (d.type != Reg::Const || d.index < s.index) continue;
                const Id hit = m_.Op(SpvOpIEqual, bool_, {base, I(static_cast<int32_t>(d.index))});
                v            = m_.Op(SpvOpSelect, vec4_, {hit, DefConst(d), v});
            }
            return v;
        }

        Id Writer::ConstRead(const Operand& s, std::string& why)
        {
            if (!s.relative)
            {
                if (const Definition* d = Defined(Reg::Const, s.index)) return DefConst(*d);
                Id v = FileRow(I(static_cast<int32_t>(s.index)));
                // ps 1.x reads its constants in [-1, 1].
                if (ps1_) v = Clamp(v, -1.0f, 1.0f);
                return v;
            }
            if (palette_pointer_ && !p_.pixel && s.rel_type == Reg::Address && s.index >= kPaletteBase && s.index < kPaletteBase + 3)
                return BoneRead(s, why);
            const Id index = RelativeIndex(s, why);
            if (!index) return 0;
            Id v = FileRow(index);
            // A register the program sets itself is its own value however it is reached.
            for (const Definition& d : p_.definitions)
            {
                if (d.type != Reg::Const || d.index < s.index) continue;
                const Id hit = m_.Op(SpvOpIEqual, bool_, {index, I(static_cast<int32_t>(d.index))});
                v            = m_.Op(SpvOpSelect, vec4_, {hit, DefConst(d), v});
            }
            return v;
        }

        Id Writer::TexcoordInput(uint32_t set) { return set < 8 ? varying_in_[kTexcoord0 + set] : V4(0, 0, 0, 1); }

        Id Writer::Fetch(const Operand& s, std::string& why)
        {
            switch (s.type)
            {
            case Reg::Temp:
                if (s.index >= temps_.size()) break;
                return Load(vec4_, temps_[s.index]);
            case Reg::Input:
                if (s.relative)
                {
                    Fail(why, "an input register addressed relatively");
                    return 0;
                }
                return inputs_[s.index] ? inputs_[s.index] : V4(0, 0, 0, 0);
            case Reg::Const: return ConstRead(s, why);
            case Reg::Address:
                if (!p_.pixel)
                {
                    if (!a0_) break;
                    return m_.Op(SpvOpConvertSToF, vec4_, {Load(ivec4_, a0_)});
                }
                if (ps1_ && p_.minor < 4)
                {
                    if (s.index >= textures_.size()) break;
                    return Load(vec4_, textures_[s.index]);
                }
                return TexcoordInput(s.index);
            case Reg::ConstInt:
            {
                const Definition* d = Defined(Reg::ConstInt, s.index);
                int32_t           v[4] = {};
                if (d) std::memcpy(v, d->bits, sizeof v);
                return V4(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3]));
            }
            case Reg::Loop:
            {
                for (auto it = constructs_.rbegin(); it != constructs_.rend(); ++it)
                    if (it->kind == Construct::Loop && it->al) return Splat(m_.Op(SpvOpConvertSToF, f32_, {Load(i32_, it->al)}));
                break;
            }
            default: break;
            }
            if (why.empty()) Fail(why, "a source of register type %u, index %u", static_cast<unsigned>(s.type), s.index);
            return 0;
        }

        Id Writer::Modify(Id v, SrcMod modifier)
        {
            switch (modifier)
            {
            case SrcMod::None: return v;
            case SrcMod::Neg: return m_.Op(SpvOpFNegate, vec4_, {v});
            case SrcMod::Bias: return m_.Op(SpvOpFSub, vec4_, {v, V4(0.5f, 0.5f, 0.5f, 0.5f)});
            case SrcMod::BiasNeg: return m_.Op(SpvOpFSub, vec4_, {V4(0.5f, 0.5f, 0.5f, 0.5f), v});
            case SrcMod::Sign: return m_.Op(SpvOpFSub, vec4_, {Mul(v, V4(2, 2, 2, 2)), V4(1, 1, 1, 1)});
            case SrcMod::SignNeg: return m_.Op(SpvOpFSub, vec4_, {V4(1, 1, 1, 1), Mul(v, V4(2, 2, 2, 2))});
            case SrcMod::Comp: return m_.Op(SpvOpFSub, vec4_, {V4(1, 1, 1, 1), v});
            case SrcMod::X2: return Mul(v, V4(2, 2, 2, 2));
            case SrcMod::X2Neg: return Mul(v, V4(-2, -2, -2, -2));
            case SrcMod::Abs: return Ext(GLSLstd450FAbs, vec4_, {v});
            case SrcMod::AbsNeg: return m_.Op(SpvOpFNegate, vec4_, {Ext(GLSLstd450FAbs, vec4_, {v})});
            case SrcMod::Dz:
            case SrcMod::Dw:
            {
                // x and y divided by z (or w); ps 1.4's projective texture reads.
                const Id d = Comp(v, modifier == SrcMod::Dz ? 2 : 3);
                const Id xy = m_.Op(SpvOpFDiv, vec4_, {v, Splat(d)});
                return Shuffle(v, xy, 4, 5, 2, 3);
            }
            default: return v;
            }
        }

        Id Writer::Source(const Operand& s, std::string& why)
        {
            const Id raw = Fetch(s, why);
            if (!raw) return 0;
            return Modify(Swizzle(raw, s.swizzle), s.modifier);
        }

        bool Writer::Write(const Operand& d, Id value, std::string& why)
        {
            if (d.shift)
            {
                const float scale = d.shift > 0 ? static_cast<float>(1 << d.shift) : 1.0f / static_cast<float>(1 << -d.shift);
                value             = Mul(value, V4(scale, scale, scale, scale));
            }
            if (d.saturate) value = Clamp(value, 0.0f, 1.0f);
            Id target = 0;
            switch (d.type)
            {
            case Reg::Temp: target = d.index < temps_.size() ? temps_[d.index] : 0; break;
            case Reg::Address:
                if (!p_.pixel)
                {
                    // vs 1.1's mov floors, mova (vs 2.0 and up) rounds.
                    const Id rounded = Ext(p_.major < 2 ? GLSLstd450Floor : GLSLstd450Round, vec4_, {value});
                    const Id ints    = m_.Op(SpvOpConvertFToS, ivec4_, {rounded});
                    const Id old     = Load(ivec4_, a0_);
                    const uint8_t mk = d.mask;
                    Store(a0_, m_.Op(SpvOpVectorShuffle, ivec4_, {old, ints, (mk & 1u) ? 4u : 0u, (mk & 2u) ? 5u : 1u, (mk & 4u) ? 6u : 2u,
                                                                  (mk & 8u) ? 7u : 3u}));
                    return true;
                }
                target = d.index < textures_.size() ? textures_[d.index] : 0;
                break;
            case Reg::RastOut: target = d.index == 0 ? out_pos_ : d.index == 1 ? out_fog_ : out_pts_; break;
            case Reg::AttrOut: target = d.index < 2 ? out_color_[d.index] : 0; break;
            case Reg::Output: target = p_.major >= 3 ? (d.index < 12 ? out_reg_[d.index] : 0) : (d.index < 8 ? out_tex_[d.index] : 0); break;
            case Reg::ColorOut: target = d.index < 4 ? out_c_[d.index] : 0; break;
            default: break;
            }
            if (!target) return Fail(why, "a destination of register type %u, index %u", static_cast<unsigned>(d.type), d.index);
            Store(target, Merge(Load(vec4_, target), value, d.mask));
            return true;
        }

        Id Writer::Compare(uint8_t comparison, Id a, Id b)
        {
            static const SpvOp kOps[7] = {SpvOpFOrdEqual, SpvOpFOrdGreaterThan, SpvOpFOrdEqual, SpvOpFOrdGreaterThanEqual,
                                          SpvOpFOrdLessThan, SpvOpFUnordNotEqual, SpvOpFOrdLessThanEqual};
            return m_.Op(kOps[comparison < 7 ? comparison : 0], bool_, {a, b});
        }

        void Writer::Kill(Id condition)
        {
            const Id kill = m_.NewId(), after = m_.NewId();
            m_.OpVoid(SpvOpSelectionMerge, {after, SpvSelectionControlMaskNone});
            m_.OpVoid(SpvOpBranchConditional, {condition, kill, after});
            m_.Label(kill);
            m_.OpVoid(SpvOpKill, {});
            m_.Label(after);
        }

        /// mode: 0 implicit, 1 projected (by w), 2 biased (by w), 3 explicit level (w).
        Id Writer::Sample(uint32_t stage, Id coord, int mode, std::string& why)
        {
            if (stage >= kStages)
            {
                Fail(why, "sampler %u", stage);
                return 0;
            }
            const bool cube = (info_.cube >> stage) & 1;
            if (!samplers_)
            {
                const Id type = m_.RuntimeArray(m_.Sampler());
                samplers_     = Variable(type, SpvStorageClassUniformConstant);
                m_.Decorate(samplers_, SpvDecorationDescriptorSet, {0});
                m_.Decorate(samplers_, SpvDecorationBinding, {1});
            }
            Id& images = cube ? images_cube_ : images2d_;
            const Id image_type = m_.Image(f32_, cube ? SpvDimCube : SpvDim2D);
            if (!images)
            {
                images = Variable(m_.RuntimeArray(image_type), SpvStorageClassUniformConstant);
                m_.Decorate(images, SpvDecorationDescriptorSet, {0});
                m_.Decorate(images, SpvDecorationBinding, {0});
            }
            info_.samplers |= 1u << stage;

            const Id word    = Load(u32_, m_.Op(SpvOpAccessChain, m_.Pointer(SpvStorageClassPushConstant, u32_), {push_, I(kPushStages), I(static_cast<int32_t>(stage))}));
            const Id slot    = m_.Op(SpvOpBitwiseAnd, u32_, {word, U(0xFFFF)});
            const Id smp_ix  = m_.Op(SpvOpShiftRightLogical, u32_, {word, U(16)});
            const Id img     = Load(image_type, m_.Op(SpvOpAccessChain, m_.Pointer(SpvStorageClassUniformConstant, image_type), {images, slot}));
            const Id smp     = Load(m_.Sampler(), m_.Op(SpvOpAccessChain, m_.Pointer(SpvStorageClassUniformConstant, m_.Sampler()), {samplers_, smp_ix}));
            const Id sampled = m_.Op(SpvOpSampledImage, m_.SampledImage(image_type), {img, smp});

            Id at = coord;
            if (mode == 1)
            {
                // Divided by w: D3D9's projected read.
                const Id w = Comp(coord, 3);
                at         = m_.Op(SpvOpFDiv, vec4_, {coord, Splat(w)});
            }
            const Id xy = m_.Op(SpvOpVectorShuffle, cube ? vec3_ : vec2_,
                                cube ? std::vector<uint32_t>{at, at, 0, 1, 2} : std::vector<uint32_t>{at, at, 0, 1});
            if (!p_.pixel || mode == 3)
            {
                const Id lod = mode == 3 ? Comp(coord, 3) : F(0.0f);
                return m_.Op(SpvOpImageSampleExplicitLod, vec4_, {sampled, xy, SpvImageOperandsLodMask, lod});
            }
            if (mode == 2) return m_.Op(SpvOpImageSampleImplicitLod, vec4_, {sampled, xy, SpvImageOperandsBiasMask, Comp(coord, 3)});
            return m_.Op(SpvOpImageSampleImplicitLod, vec4_, {sampled, xy});
        }

        bool Writer::Texture(const Instruction& ins, std::string& why)
        {
            if (ps1_)
            {
                const uint32_t stage = ins.dst.index;
                Id             coord;
                if (p_.minor >= 4)
                {
                    // texld r#, t# (or r# in the second phase): stage = the destination.
                    if (ins.src_count < 1) return Fail(why, "a ps 1.4 texld without its coordinate");
                    coord = Source(ins.src[0], why);
                    if (!coord) return false;
                }
                else
                    coord = TexcoordInput(stage);
                const Id texel = Sample(stage, coord, 0, why);
                return texel && Write(ins.dst, texel, why);
            }
            if (ins.src_count < 2 || ins.src[1].type != Reg::Sampler) return Fail(why, "a texld without its sampler");
            const Id coord = Source(ins.src[0], why);
            if (!coord) return false;
            const int mode  = ins.op == kTexLdl ? 3 : ins.control == 1 ? 1 : ins.control == 2 ? 2 : 0;
            const Id  texel = Sample(ins.src[1].index, coord, mode, why);
            if (!texel) return false;
            return Write(ins.dst, Swizzle(texel, ins.src[1].swizzle), why);
        }

        bool Writer::Flow(const Instruction& ins, size_t at, std::string& why)
        {
            switch (ins.op)
            {
            case kIf:
            case kIfc:
            {
                Id condition;
                if (ins.op == kIf)
                {
                    const Operand& b = ins.src[0];
                    if (b.type != Reg::ConstBool) return Fail(why, "an if on register type %u", static_cast<unsigned>(b.type));
                    const Definition* d = Defined(Reg::ConstBool, b.index);
                    bool value = d && d->bits[0] != 0;
                    if (b.modifier == SrcMod::Not) value = !value;
                    condition = m_.ConstBool(value);
                }
                else
                {
                    const Id a = Source(ins.src[0], why), b = Source(ins.src[1], why);
                    if (!a || !b) return false;
                    condition = Compare(ins.control, Comp(a, 0), Comp(b, 0));
                }
                Construct c;
                c.kind      = Construct::If;
                c.merge     = m_.NewId();
                c.otherwise = m_.NewId();
                const Id then = m_.NewId();
                m_.OpVoid(SpvOpSelectionMerge, {c.merge, SpvSelectionControlMaskNone});
                m_.OpVoid(SpvOpBranchConditional, {condition, then, c.otherwise});
                m_.Label(then);
                constructs_.push_back(c);
                return true;
            }
            case kElse:
            {
                if (constructs_.empty() || constructs_.back().kind != Construct::If || constructs_.back().else_seen)
                    return Fail(why, "an else without its if");
                Construct& c = constructs_.back();
                m_.OpVoid(SpvOpBranch, {c.merge});
                m_.Label(c.otherwise);
                c.else_seen = true;
                return true;
            }
            case kEndIf:
            {
                if (constructs_.empty() || constructs_.back().kind != Construct::If) return Fail(why, "an endif without its if");
                const Construct c = constructs_.back();
                constructs_.pop_back();
                m_.OpVoid(SpvOpBranch, {c.merge});
                if (!c.else_seen)
                {
                    m_.Label(c.otherwise);
                    m_.OpVoid(SpvOpBranch, {c.merge});
                }
                m_.Label(c.merge);
                return true;
            }
            case kLoop:
            case kRep:
            {
                const Operand& counter = ins.op == kLoop ? ins.src[1] : ins.src[0];
                if (ins.src_count < (ins.op == kLoop ? 2u : 1u) || counter.type != Reg::ConstInt) return Fail(why, "a loop without its integer register");
                const Definition* d = Defined(Reg::ConstInt, counter.index);
                int32_t           v[4] = {};
                if (d) std::memcpy(v, d->bits, sizeof v);
                Construct c;
                c.kind    = Construct::Loop;
                c.merge   = m_.NewId();
                c.next    = m_.NewId();
                c.step    = v[2];
                c.counter = Local(i32_, I(0));
                if (ins.op == kLoop) c.al = Local(i32_, I(v[1]));
                const Id header = m_.NewId(), body = m_.NewId();
                m_.OpVoid(SpvOpBranch, {header});
                m_.Label(header);
                const Id more = m_.Op(SpvOpSLessThan, bool_, {Load(i32_, c.counter), I(v[0])});
                m_.OpVoid(SpvOpLoopMerge, {c.merge, c.next, SpvLoopControlMaskNone});
                m_.OpVoid(SpvOpBranchConditional, {more, body, c.merge});
                m_.Label(body);
                c.otherwise = header; // the loop's header, branched back to from its continue block
                constructs_.push_back(c);
                return true;
            }
            case kEndLoop:
            case kEndRep:
            {
                if (constructs_.empty() || constructs_.back().kind != Construct::Loop) return Fail(why, "an endloop without its loop");
                const Construct c = constructs_.back();
                constructs_.pop_back();
                m_.OpVoid(SpvOpBranch, {c.next});
                m_.Label(c.next);
                Store(c.counter, m_.Op(SpvOpIAdd, i32_, {Load(i32_, c.counter), I(1)}));
                if (c.al) Store(c.al, m_.Op(SpvOpIAdd, i32_, {Load(i32_, c.al), I(c.step)}));
                m_.OpVoid(SpvOpBranch, {c.otherwise});
                m_.Label(c.merge);
                return true;
            }
            case kBreak:
            case kBreakc:
            {
                const Construct* loop = nullptr;
                for (auto it = constructs_.rbegin(); it != constructs_.rend() && !loop; ++it)
                    if (it->kind == Construct::Loop) loop = &*it;
                if (!loop) return Fail(why, "a break outside a loop");
                if (ins.op == kBreak)
                {
                    m_.OpVoid(SpvOpBranch, {loop->merge});
                    m_.Label(m_.NewId());
                    return true;
                }
                const Id a = Source(ins.src[0], why), b = Source(ins.src[1], why);
                if (!a || !b) return false;
                const Id condition = Compare(ins.control, Comp(a, 0), Comp(b, 0));
                const Id leave = m_.NewId(), after = m_.NewId();
                m_.OpVoid(SpvOpSelectionMerge, {after, SpvSelectionControlMaskNone});
                m_.OpVoid(SpvOpBranchConditional, {condition, leave, after});
                m_.Label(leave);
                m_.OpVoid(SpvOpBranch, {loop->merge});
                m_.Label(after);
                return true;
            }
            case kRet:
                if (at + 1 != p_.code.size() || !constructs_.empty()) return Fail(why, "a ret before the end of the program");
                return true;
            default: return Fail(why, "opcode %u", ins.op);
            }
        }

        bool Writer::Emit(const Instruction& ins, size_t at, std::string& why)
        {
            auto src = [&](uint32_t i) -> Id {
                if (i >= ins.src_count)
                {
                    Fail(why, "opcode %u without its source %u", ins.op, i);
                    return 0;
                }
                return Source(ins.src[i], why);
            };
            auto scalar = [&](Id v) { return Comp(v, 0); };
            auto replicate = [&](Id s) { return Splat(s); };
            Id r = 0;
            switch (ins.op)
            {
            case kNop:
            case kPhase: return true;
            case kMov:
            case kMova:
                r = src(0);
                break;
            case kAdd:
            case kSub:
            case kMul:
            {
                const Id a = src(0), b = src(1);
                if (!a || !b) return false;
                r = m_.Op(ins.op == kAdd ? SpvOpFAdd : ins.op == kSub ? SpvOpFSub : SpvOpFMul, vec4_, {a, b});
                break;
            }
            case kMad:
            {
                const Id a = src(0), b = src(1), c = src(2);
                if (!a || !b || !c) return false;
                r = Add(Mul(a, b), c);
                break;
            }
            case kRcp:
            {
                const Id a = src(0);
                if (!a) return false;
                r = replicate(ClampScalar(m_.Op(SpvOpFDiv, f32_, {F(1.0f), scalar(a)}), -kFltMax, kFltMax));
                break;
            }
            case kRsq:
            {
                const Id a = src(0);
                if (!a) return false;
                r = replicate(Ext(GLSLstd450NMin, f32_, {Ext(GLSLstd450InverseSqrt, f32_, {Ext(GLSLstd450FAbs, f32_, {scalar(a)})}), F(kFltMax)}));
                break;
            }
            case kExp:
            case kExpP:
            {
                const Id a = src(0);
                if (!a) return false;
                r = replicate(Ext(GLSLstd450Exp2, f32_, {scalar(a)}));
                break;
            }
            case kLog:
            case kLogP:
            {
                const Id a = src(0);
                if (!a) return false;
                r = replicate(Ext(GLSLstd450NMax, f32_, {Ext(GLSLstd450Log2, f32_, {Ext(GLSLstd450FAbs, f32_, {scalar(a)})}), F(-kFltMax)}));
                break;
            }
            case kPow:
            {
                const Id a = src(0), b = src(1);
                if (!a || !b) return false;
                const Id base = Ext(GLSLstd450FAbs, f32_, {scalar(a)});
                r = replicate(Ext(GLSLstd450NMin, f32_, {Ext(GLSLstd450Pow, f32_, {base, scalar(b)}), F(kFltMax)}));
                break;
            }
            case kDp3:
            case kDp4:
            {
                const Id a = src(0), b = src(1);
                if (!a || !b) return false;
                if (ins.op == kDp4) r = replicate(m_.Op(SpvOpDot, f32_, {a, b}));
                else
                {
                    const Id a3 = m_.Op(SpvOpVectorShuffle, vec3_, {a, a, 0, 1, 2});
                    const Id b3 = m_.Op(SpvOpVectorShuffle, vec3_, {b, b, 0, 1, 2});
                    r = replicate(m_.Op(SpvOpDot, f32_, {a3, b3}));
                }
                break;
            }
            case kDp2Add:
            {
                const Id a = src(0), b = src(1), c = src(2);
                if (!a || !b || !c) return false;
                const Id a2 = m_.Op(SpvOpVectorShuffle, vec2_, {a, a, 0, 1});
                const Id b2 = m_.Op(SpvOpVectorShuffle, vec2_, {b, b, 0, 1});
                r = replicate(m_.Op(SpvOpFAdd, f32_, {m_.Op(SpvOpDot, f32_, {a2, b2}), scalar(c)}));
                break;
            }
            case kMin:
            case kMax:
            {
                const Id a = src(0), b = src(1);
                if (!a || !b) return false;
                r = Ext(ins.op == kMin ? GLSLstd450FMin : GLSLstd450FMax, vec4_, {a, b});
                break;
            }
            case kSlt:
            case kSge:
            {
                const Id a = src(0), b = src(1);
                if (!a || !b) return false;
                const Id test = m_.Op(ins.op == kSlt ? SpvOpFOrdLessThan : SpvOpFOrdGreaterThanEqual, bvec4_, {a, b});
                r = m_.Op(SpvOpSelect, vec4_, {test, V4(1, 1, 1, 1), V4(0, 0, 0, 0)});
                break;
            }
            case kCmp:
            case kCnd:
            {
                const Id a = src(0), b = src(1), c = src(2);
                if (!a || !b || !c) return false;
                const Id test = ins.op == kCmp ? m_.Op(SpvOpFOrdGreaterThanEqual, bvec4_, {a, V4(0, 0, 0, 0)})
                                               : m_.Op(SpvOpFOrdGreaterThan, bvec4_, {a, V4(0.5f, 0.5f, 0.5f, 0.5f)});
                r = m_.Op(SpvOpSelect, vec4_, {test, b, c});
                break;
            }
            case kLrp:
            {
                const Id a = src(0), b = src(1), c = src(2);
                if (!a || !b || !c) return false;
                r = Add(Mul(a, m_.Op(SpvOpFSub, vec4_, {b, c})), c);
                break;
            }
            case kFrc:
            {
                const Id a = src(0);
                if (!a) return false;
                r = Ext(GLSLstd450Fract, vec4_, {a});
                break;
            }
            case kAbs:
            {
                const Id a = src(0);
                if (!a) return false;
                r = Ext(GLSLstd450FAbs, vec4_, {a});
                break;
            }
            case kSgn:
            {
                const Id a = src(0);
                if (!a) return false;
                r = Ext(GLSLstd450FSign, vec4_, {a});
                break;
            }
            case kNrm:
            {
                const Id a = src(0);
                if (!a) return false;
                const Id a3  = m_.Op(SpvOpVectorShuffle, vec3_, {a, a, 0, 1, 2});
                const Id rsq = Ext(GLSLstd450NMin, f32_, {Ext(GLSLstd450InverseSqrt, f32_, {m_.Op(SpvOpDot, f32_, {a3, a3})}), F(kFltMax)});
                r = Mul(a, Splat(rsq));
                break;
            }
            case kCrs:
            {
                const Id a = src(0), b = src(1);
                if (!a || !b) return false;
                const Id c = Ext(GLSLstd450Cross, vec3_, {m_.Op(SpvOpVectorShuffle, vec3_, {a, a, 0, 1, 2}),
                                                          m_.Op(SpvOpVectorShuffle, vec3_, {b, b, 0, 1, 2})});
                r = m_.Op(SpvOpCompositeConstruct, vec4_, {c, F(0.0f)});
                break;
            }
            case kSinCos:
            {
                const Id a = src(0);
                if (!a) return false;
                // The angle is the source's replicated component; x gets its cosine, y its sine.
                const Id angle = scalar(a);
                r = m_.Op(SpvOpCompositeConstruct, vec4_, {Ext(GLSLstd450Cos, f32_, {angle}), Ext(GLSLstd450Sin, f32_, {angle}), F(0.0f), F(0.0f)});
                break;
            }
            case kLit:
            {
                const Id a = src(0);
                if (!a) return false;
                const Id x = Comp(a, 0), y = Comp(a, 1), w = Comp(a, 3);
                const Id diffuse = Ext(GLSLstd450FMax, f32_, {x, F(0.0f)});
                const Id power   = Ext(GLSLstd450FClamp, f32_, {w, F(-128.0f), F(128.0f)});
                const Id spec    = Ext(GLSLstd450Pow, f32_, {Ext(GLSLstd450FMax, f32_, {y, F(0.0f)}), power});
                const Id lit     = m_.Op(SpvOpFOrdGreaterThan, bool_, {x, F(0.0f)});
                r = m_.Op(SpvOpCompositeConstruct, vec4_, {F(1.0f), diffuse, m_.Op(SpvOpSelect, f32_, {lit, spec, F(0.0f)}), F(1.0f)});
                break;
            }
            case kDst:
            {
                const Id a = src(0), b = src(1);
                if (!a || !b) return false;
                r = m_.Op(SpvOpCompositeConstruct, vec4_, {F(1.0f), m_.Op(SpvOpFMul, f32_, {Comp(a, 1), Comp(b, 1)}), Comp(a, 2), Comp(b, 3)});
                break;
            }
            case kM4x4:
            case kM4x3:
            case kM3x4:
            case kM3x3:
            case kM3x2:
            {
                const Id a = src(0);
                if (!a) return false;
                const bool     four = ins.op == kM4x4 || ins.op == kM4x3;
                const uint32_t rows = ins.op == kM4x4 || ins.op == kM3x4 ? 4 : ins.op == kM3x2 ? 2 : 3;
                Id             dots[4] = {F(0.0f), F(0.0f), F(0.0f), F(0.0f)};
                const Id       a3 = m_.Op(SpvOpVectorShuffle, vec3_, {a, a, 0, 1, 2});
                for (uint32_t row = 0; row < rows; ++row)
                {
                    Operand m = ins.src[1];
                    m.index += row;
                    const Id b = Source(m, why);
                    if (!b) return false;
                    dots[row] = four ? m_.Op(SpvOpDot, f32_, {a, b}) : m_.Op(SpvOpDot, f32_, {a3, m_.Op(SpvOpVectorShuffle, vec3_, {b, b, 0, 1, 2})});
                }
                r = m_.Op(SpvOpCompositeConstruct, vec4_, {dots[0], dots[1], dots[2], dots[3]});
                break;
            }
            case kDsx:
            case kDsy:
            {
                if (!p_.pixel) return Fail(why, "a derivative in a vertex program");
                const Id a = src(0);
                if (!a) return false;
                r = m_.Op(ins.op == kDsx ? SpvOpDPdx : SpvOpDPdy, vec4_, {a});
                break;
            }
            case kTexKill:
            {
                if (!p_.pixel) return Fail(why, "texkill in a vertex program");
                Operand reg = ins.dst;
                reg.swizzle = 0xE4;
                reg.modifier = SrcMod::None;
                const Id v = Fetch(reg, why);
                if (!v) return false;
                // Below ps 2.0, x, y and z; from it, the components the register's mask names.
                const uint8_t mask = p_.major < 2 ? 0x7 : (ins.dst.mask ? ins.dst.mask : 0xF);
                const Id      below = m_.Op(SpvOpFOrdLessThan, bvec4_, {v, V4(0, 0, 0, 0)});
                Id            any = m_.ConstBool(false);
                for (uint32_t c = 0; c < 4; ++c)
                    if (mask & (1u << c)) any = m_.Op(SpvOpLogicalOr, bool_, {any, m_.Op(SpvOpCompositeExtract, bool_, {below, c})});
                Kill(any);
                return true;
            }
            case kTex:
            case kTexLdl: return Texture(ins, why);
            case kTexCoord:
            {
                if (!ps1_) return Fail(why, "texcoord above ps 1.4");
                if (p_.minor >= 4)
                {
                    // texcrd r#, t#: the coordinate itself.
                    const Id a = src(0);
                    return a && Write(ins.dst, a, why);
                }
                // texcoord t#: the coordinate as a colour, saturated, alpha 1.
                const Id t = Clamp(TexcoordInput(ins.dst.index), 0.0f, 1.0f);
                return Write(ins.dst, Shuffle(t, V4(1, 1, 1, 1), 0, 1, 2, 4), why);
            }
            case kTexBem:
            {
                // The bump matrix the engine never sets is zero: the stage's own coordinate.
                const Id texel = Sample(ins.dst.index, TexcoordInput(ins.dst.index), 0, why);
                return texel && Write(ins.dst, texel, why);
            }
            case kIf:
            case kIfc:
            case kElse:
            case kEndIf:
            case kLoop:
            case kEndLoop:
            case kRep:
            case kEndRep:
            case kBreak:
            case kBreakc:
            case kRet: return Flow(ins, at, why);
            default: return Fail(why, "opcode %u", ins.op);
            }
            if (!r) return false;
            if (!ins.has_dst) return Fail(why, "opcode %u without its destination", ins.op);
            return Write(ins.dst, r, why);
        }

        bool Writer::Prologue(std::string& why)
        {
            ps1_ = p_.pixel && p_.major == 1;

            // Locals first: every register the program names.
            const uint32_t temps = std::max<uint32_t>(p_.temps, ps1_ ? (p_.minor >= 4 ? 6u : 2u) : 0u);
            for (uint32_t i = 0; i < temps; ++i) temps_.push_back(Local(vec4_, V4(0, 0, 0, 0)));
            if (!p_.pixel) a0_ = Local(ivec4_, m_.ConstComposite(ivec4_, {I(0), I(0), I(0), I(0)}));

            const Id fv4 = vec4_;
            if (p_.pixel)
            {
                // The varyings this program reads: by dcl (ps 3.0), by register below it.
                bool needs[kVaryings] = {};
                if (p_.major >= 3)
                {
                    for (const Declaration& d : p_.declarations)
                    {
                        if (d.type != Reg::Input) continue;
                        const int loc = VaryingLocation(d.usage, d.usage_index);
                        if (loc < 0) return Fail(why, "a pixel input of meaning %u.%u", d.usage, d.usage_index);
                        needs[loc] = true;
                    }
                }
                else
                {
                    needs[kColor0] = needs[kColor1] = true;
                    for (uint32_t t = 0; t < 8; ++t) needs[kTexcoord0 + t] = true;
                    needs[kFog] = true;
                }
                if (!p_.pixel || p_.major < 3) needs[kFog] = true;
                bool texcoords = false;
                for (uint32_t t = 0; t < 8; ++t) texcoords |= needs[kTexcoord0 + t];
                Id sprite = 0, sprite_coord = 0;
                if (texcoords)
                {
                    point_coord_ = Variable(vec2_, SpvStorageClassInput);
                    m_.Decorate(point_coord_, SpvDecorationBuiltIn, {SpvBuiltInPointCoord});
                    sprite = Flag(kFlagPointSprite);
                    const Id pc = Load(vec2_, point_coord_);
                    sprite_coord = m_.Op(SpvOpCompositeConstruct, fv4, {pc, F(0.0f), F(1.0f)});
                }
                for (uint32_t loc = 0; loc < kVaryings; ++loc)
                {
                    if (!needs[loc]) continue;
                    const Id var = Variable(fv4, SpvStorageClassInput);
                    m_.Decorate(var, SpvDecorationLocation, {loc});
                    Id value = Load(fv4, var);
                    if (loc >= kTexcoord0 && loc < kTexcoord0 + 8) value = m_.Op(SpvOpSelect, fv4, {sprite, sprite_coord, value});
                    varying_in_[loc] = value;
                }
                for (uint32_t loc = 0; loc < kVaryings; ++loc)
                    if (!varying_in_[loc]) varying_in_[loc] = loc == kFog ? V4(1, 0, 0, 0) : V4(0, 0, 0, 1);
                // The v registers.
                if (p_.major >= 3)
                {
                    for (const Declaration& d : p_.declarations)
                    {
                        if (d.type != Reg::Input || d.index >= 16) continue;
                        const Id value  = varying_in_[VaryingLocation(d.usage, d.usage_index)];
                        inputs_[d.index] = inputs_[d.index] ? Merge(inputs_[d.index], value, d.mask) : value;
                    }
                }
                else
                {
                    inputs_[0] = varying_in_[kColor0];
                    inputs_[1] = varying_in_[kColor1];
                }
                if (ps1_ && p_.minor < 4)
                    for (uint32_t t = 0; t < 4; ++t) textures_.push_back(Local(fv4, varying_in_[kTexcoord0 + t]));
                for (uint32_t c = 0; c < 4; ++c) out_c_[c] = Local(fv4, V4(0, 0, 0, 0));
                return true;
            }

            // A vertex program's inputs, by the meaning of their dcl.
            for (const Declaration& d : p_.declarations)
            {
                if (d.type != Reg::Input) continue;
                const int loc = InputLocation(d.usage, d.usage_index);
                if (loc < 0) return Fail(why, "a vertex input of meaning %u.%u", d.usage, d.usage_index);
                if (d.index >= 16) return Fail(why, "vertex input register %u", d.index);
                const Id var = Variable(fv4, SpvStorageClassInput);
                m_.Decorate(var, SpvDecorationLocation, {static_cast<uint32_t>(loc)});
                Id value = Load(fv4, var);
                // A D3DCOLOR the device reads in RGBA order is turned back.
                const Id swap = Flag(1u << (kFlagSwizzleShift + static_cast<uint32_t>(loc)));
                value         = m_.Op(SpvOpSelect, fv4, {swap, Shuffle(value, value, 2, 1, 0, 3), value});
                info_.inputs |= 1u << loc;
                inputs_[d.index] = inputs_[d.index] ? Merge(inputs_[d.index], value, d.mask) : value;
            }
            // The outputs, with what a register nobody writes says.
            out_pos_ = Local(fv4, V4(0, 0, 0, 1));
            out_fog_ = Local(fv4, V4(1, 0, 0, 0));
            out_pts_ = Local(fv4, V4(1, 0, 0, 0));
            out_color_[0] = Local(fv4, V4(1, 1, 1, 1));
            out_color_[1] = Local(fv4, V4(0, 0, 0, 0));
            for (Id& t : out_tex_) t = Local(fv4, V4(0, 0, 0, 1));
            if (p_.major >= 3)
                for (Id& o : out_reg_) o = Local(fv4, V4(0, 0, 0, 1));
            return true;
        }

        bool Writer::Epilogue(std::string& why)
        {
            const Id fv4 = vec4_;
            if (p_.pixel)
            {
                Id color = ps1_ ? Load(fv4, temps_[0]) : Load(fv4, out_c_[0]);
                if (info_.fixed_fog)
                {
                    // D3D9's fog after the program: the fog colour by the fog factor's complement.
                    const Id packed = PushMember(kPushFogColor, u32_);
                    Id       rgb[3];
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        const Id byte = m_.Op(SpvOpBitwiseAnd, u32_, {m_.Op(SpvOpShiftRightLogical, u32_, {packed, U(16 - 8 * c)}), U(0xFF)});
                        rgb[c] = m_.Op(SpvOpFMul, f32_, {m_.Op(SpvOpConvertUToF, f32_, {byte}), F(1.0f / 255.0f)});
                    }
                    const Id fog_color = m_.Op(SpvOpCompositeConstruct, fv4, {rgb[0], rgb[1], rgb[2], F(0.0f)});
                    const Id factor    = ClampScalar(Comp(varying_in_[kFog], 0), 0.0f, 1.0f);
                    const Id fogged    = Ext(GLSLstd450FMix, fv4, {fog_color, color, Splat(factor)});
                    color = m_.Op(SpvOpSelect, fv4, {Flag(kFlagFog), Shuffle(fogged, color, 0, 1, 2, 7), color});
                }
                // D3D9's alpha test, ALPHAFUNC GREATEREQUAL: the alpha as the byte it compares.
                const Id ref   = PushMember(kPushAlphaRef, f32_);
                const Id alpha = Ext(GLSLstd450Floor, f32_, {m_.Op(SpvOpFAdd, f32_, {m_.Op(SpvOpFMul, f32_, {ClampScalar(Comp(color, 3), 0.0f, 1.0f), F(255.0f)}), F(0.5f)})});
                const Id test  = m_.Op(SpvOpLogicalAnd, bool_, {m_.Op(SpvOpFOrdGreaterThan, bool_, {ref, F(0.0f)}), m_.Op(SpvOpFOrdLessThan, bool_, {alpha, ref})});
                Kill(test);
                const Id out = Variable(fv4, SpvStorageClassOutput);
                m_.Decorate(out, SpvDecorationLocation, {0});
                Store(out, color);
                return true;
            }

            // The varyings, every one of them.
            Id varyings[kVaryings];
            for (uint32_t loc = 0; loc < kVaryings; ++loc) varyings[loc] = loc == kFog ? V4(1, 0, 0, 0) : V4(0, 0, 0, 1);
            varyings[kColor0] = V4(1, 1, 1, 1);
            varyings[kColor1] = V4(0, 0, 0, 0);
            Id position = 0, point = 0;
            if (p_.major >= 3)
            {
                for (const Declaration& d : p_.declarations)
                {
                    if (d.type != Reg::Output) continue;
                    if (d.index >= 12) return Fail(why, "output register %u", d.index);
                    const Id value = Load(fv4, out_reg_[d.index]);
                    if (d.usage == 0 && d.usage_index == 0) position = value;
                    else if (d.usage == 4) point = Comp(value, 0);
                    else
                    {
                        const int loc = VaryingLocation(d.usage, d.usage_index);
                        if (loc < 0) return Fail(why, "a vertex output of meaning %u.%u", d.usage, d.usage_index);
                        varyings[loc] = Merge(varyings[loc], value, d.mask);
                    }
                }
                if (!position) return Fail(why, "a vs_3_0 program without a position output");
            }
            else
            {
                position = Load(fv4, out_pos_);
                // D3D9 saturates the colour outputs below vs 3.0.
                varyings[kColor0] = Clamp(Load(fv4, out_color_[0]), 0.0f, 1.0f);
                varyings[kColor1] = Clamp(Load(fv4, out_color_[1]), 0.0f, 1.0f);
                for (uint32_t t = 0; t < 8; ++t) varyings[kTexcoord0 + t] = Load(fv4, out_tex_[t]);
                varyings[kFog] = Load(fv4, out_fog_);
                bool points = false;
                for (const Instruction& ins : p_.code)
                    points |= ins.has_dst && ins.dst.type == Reg::RastOut && ins.dst.index == 2;
                if (points) point = Comp(Load(fv4, out_pts_), 0);
            }
            for (uint32_t loc = 0; loc < kVaryings; ++loc)
            {
                const Id var = Variable(fv4, SpvStorageClassOutput);
                m_.Decorate(var, SpvDecorationLocation, {loc});
                Store(var, varyings[loc]);
            }

            const Id pos = Variable(fv4, SpvStorageClassOutput);
            m_.Decorate(pos, SpvDecorationBuiltIn, {SpvBuiltInPosition});
            // D3D9's clip space has y up; Vulkan's down. z keeps D3D9's [0, w], Vulkan's range.
            Store(pos, m_.Op(SpvOpVectorShuffle, fv4, {position, m_.Op(SpvOpFNegate, fv4, {position}), 0, 5, 2, 3}));

            const Id size = Variable(f32_, SpvStorageClassOutput);
            m_.Decorate(size, SpvDecorationBuiltIn, {SpvBuiltInPointSize});
            // D3D9 clamps a point's size, the program's or POINTSIZE, to POINTSIZE_MIN..POINTSIZE_MAX.
            const Id range = PushMember(kPushPointRange, vec2_);
            Store(size, m_.Ext(f32_, GLSLstd450FClamp, {point ? point : PushMember(kPushPointSize, f32_), Comp(range, 0), Comp(range, 1)}));

            if (o_.clip_distance)
            {
                const Id clip = Variable(m_.PlainArray(f32_, 1), SpvStorageClassOutput);
                m_.Decorate(clip, SpvDecorationBuiltIn, {SpvBuiltInClipDistance});
                const Id distance = m_.Op(SpvOpDot, f32_, {PushMember(kPushClipPlane, fv4), position});
                const Id value    = m_.Op(SpvOpSelect, f32_, {Flag(kFlagClip), distance, F(1.0f)});
                Store(m_.Op(SpvOpAccessChain, m_.Pointer(SpvStorageClassOutput, f32_), {clip, I(0)}), value);
            }
            return true;
        }

        bool Writer::Run(Translation& out, std::string& why)
        {
            m_.SetVersion(spirv::Module::kVersion15);
            m_.Capability(SpvCapabilityPhysicalStorageBufferAddresses);
            m_.Capability(SpvCapabilitySampledImageArrayDynamicIndexing);
            m_.Capability(SpvCapabilityRuntimeDescriptorArray);
            if (!p_.pixel && o_.clip_distance) m_.Capability(SpvCapabilityClipDistance);
            m_.Addressing(SpvAddressingModelPhysicalStorageBuffer64);

            f32_    = m_.Float();
            vec2_   = m_.Vector(f32_, 2);
            vec3_   = m_.Vector(f32_, 3);
            vec4_   = m_.Vector(f32_, 4);
            i32_    = m_.Int();
            ivec4_  = m_.Vector(i32_, 4);
            u32_    = m_.Uint();
            uvec2_  = m_.Vector(u32_, 2);
            bool_   = m_.Bool();
            bvec4_  = m_.Vector(bool_, 4);
            fn_vec4_ = m_.Pointer(SpvStorageClassFunction, vec4_);

            // The push constants (shaders::Push).
            const Id stages = m_.Array(u32_, kStages, 4);
            const Id block  = m_.Struct({uvec2_, uvec2_, uvec2_, f32_, u32_, vec4_, u32_, f32_, vec2_, stages});
            static const uint32_t kOffsets[] = {0, 8, 16, 24, 28, 32, 48, 52, 56, 64};
            for (uint32_t i = 0; i < 10; ++i) m_.MemberDecorate(block, i, SpvDecorationOffset, {kOffsets[i]});
            m_.Decorate(block, SpvDecorationBlock);
            m_.BeginMain({});
            push_ = Variable(block, SpvStorageClassPushConstant);
            m_.Name(push_, "pc");

            info_.pixel      = p_.pixel;
            info_.major      = p_.major;
            info_.minor      = p_.minor;
            info_.const_rows = p_.relative_const ? kRegisterRows : p_.const_rows;
            info_.palette    = !p_.pixel && p_.relative_bones;
            info_.fixed_fog  = p_.pixel && p_.major < 3;
            for (const Declaration& d : p_.declarations)
                if (d.type == Reg::Sampler && d.texture == 3) info_.cube |= 1u << d.index;

            flags_ = PushMember(kPushFlags, u32_);
            if (info_.const_rows)
            {
                // The engine's register file: vec4 rows at the address the draw pushes.
                const Id rows   = m_.Array(vec4_, kRegisterRows, 16);
                const Id file   = m_.Struct({rows});
                m_.MemberDecorate(file, 0, SpvDecorationOffset, {0});
                m_.Decorate(file, SpvDecorationBlock);
                const Id file_pointer = m_.Pointer(SpvStorageClassPhysicalStorageBuffer, file);
                row_pointer_          = m_.Pointer(SpvStorageClassPhysicalStorageBuffer, vec4_);
                const Id address      = PushMember(p_.pixel ? kPushPixel : kPushVertex, uvec2_);
                regs_                 = m_.Op(SpvOpBitcast, file_pointer, {address});
            }
            if (info_.palette)
            {
                // The bones' file: the palette (kPaletteRows rows at most) when the flags count its
                // bones, at the address they name; else the register file, read from c31 as D3D9 reads it.
                const Id rows = m_.Array(vec4_, kPaletteRows, 16);
                const Id file = m_.Struct({rows});
                m_.MemberDecorate(file, 0, SpvDecorationOffset, {0});
                m_.Decorate(file, SpvDecorationBlock);
                palette_pointer_ = m_.Pointer(SpvStorageClassPhysicalStorageBuffer, file);
                if (!row_pointer_) row_pointer_ = m_.Pointer(SpvStorageClassPhysicalStorageBuffer, vec4_);
                const Id bvec2  = m_.Vector(bool_, 2);
                const Id bones  = m_.Op(SpvOpBitwiseAnd, u32_, {m_.Op(SpvOpShiftRightLogical, u32_, {flags_, U(kFlagBonesShift)}), U(kFlagBonesMask)});
                const Id on     = m_.Op(SpvOpINotEqual, bool_, {bones, U(0)});
                const Id pixel  = Flag(kFlagPaletteAtPixel);
                const Id where  = m_.Op(SpvOpSelect, uvec2_, {m_.Op(SpvOpCompositeConstruct, bvec2, {pixel, pixel}), PushMember(kPushPixel, uvec2_),
                                                              PushMember(kPushData, uvec2_)});
                palette_address_ = m_.Op(SpvOpSelect, uvec2_, {m_.Op(SpvOpCompositeConstruct, bvec2, {on, on}), where, PushMember(kPushVertex, uvec2_)});
                const Id last    = m_.Op(SpvOpISub, i32_, {m_.Op(SpvOpIMul, i32_, {m_.Op(SpvOpBitcast, i32_, {bones}), I(3)}), I(1)});
                palette_last_    = m_.Op(SpvOpSelect, i32_, {on, last, I(static_cast<int32_t>(kRegisterRows) - 1)});
                palette_offset_  = m_.Op(SpvOpSelect, i32_, {on, I(-static_cast<int32_t>(kPaletteBase)), I(0)});
            }

            if (!Prologue(why)) return false;
            for (size_t i = 0; i < p_.code.size(); ++i)
                if (!Emit(p_.code[i], i, why)) return false;
            if (!constructs_.empty()) return Fail(why, "a branch or loop left open");
            if (!Epilogue(why)) return false;
            if (p_.pixel) m_.ExecutionMode(SpvExecutionModeOriginUpperLeft);
            m_.EndMain();

            out.words = m_.Words();
            out.info  = info_;
            return true;
        }
    }

    bool Translate(const void* code, size_t size, const Options& options, Translation& out, std::string& why)
    {
        out = Translation{};
        why.clear();
        if (!code || size < 8 || size % 4)
        {
            why = "not a whole number of tokens";
            return false;
        }
        std::vector<uint32_t> tokens(size / 4);
        std::memcpy(tokens.data(), code, size);
        dx9::Program program;
        if (!dx9::Decode(tokens.data(), tokens.size(), program, why)) return false;
        Writer writer(program, options);
        if (!writer.Run(out, why))
        {
            why = dx9::ProfileName(program) + ": " + why;
            return false;
        }
        return true;
    }
}
