// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The translator of the client's DX9 programs, against D3D9's own formulas. Programs are assembled
// here token by token (vs_1_1 to vs_3_0, ps_1_1 to ps_3_0), translated, drawn on the machine's
// driver into one RGBA32F pixel, and the pixel compared with what a reference interpreter of
// D3D9's instruction set computes on the CPU from the same tokens: every arithmetic instruction
// with its swizzles, source modifiers, write masks, saturation and shifts, relative addressing
// through a0 and aL (a register the program defines itself read that way included), loops, reps,
// branches and breaks, texkill, the alpha test, D3D9's fog after a pixel program below ps_3_0, the
// user clip plane, ps 1.x's constant range, texture reads (2D, cube, projected, biased, explicit
// level). The pure half checks what the translator refuses, the translation cache and its pack,
// and spirv-val over every program assembled here.

#include "gx/vk/frame/Frame.hpp"
#include "gx/vk/resources/Bindless.hpp"
#include "gx/vk/resources/Resources.hpp"
#include "gx/vk/shaders/Cache.hpp"
#include "gx/vk/shaders/Dx9.hpp"
#include "gx/vk/shaders/Interface.hpp"
#include "gx/vk/shaders/Translate.hpp"

#include "Asm.hpp"
#include "shader/Ps3.hpp"
#include "Check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#if defined(WXL_VK_SPIRV_VAL)
#include <spirv-tools/libspirv.hpp>
#endif

namespace wxl::vk::test
{
    namespace
    {
        using namespace wxl::vk::shaders;
        using namespace wxl::vk::test::dx9asm;

        constexpr float kFltMax = 3.402823466e38f;


        // --- D3D9's instruction set on the CPU --------------------------------------------------------

        struct V4
        {
            float v[4] = {0, 0, 0, 0};
        };

        /// What a program reads and writes, interpreted.
        struct Machine
        {
            const dx9::Program* p = nullptr;
            const float*        consts = nullptr; ///< the engine's 256 rows
            V4                  inputs[16];
            V4                  temps[32];
            V4                  textures[8]; ///< ps 1.x t#
            int32_t             a0[4] = {};
            std::vector<int32_t> al;
            V4                  out[12]{{{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}},
                                        {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}};
            V4                  out_pos, out_fog{{1, 0, 0, 0}}, out_color[2]{{{1, 1, 1, 1}}, {{0, 0, 0, 0}}};
            V4                  out_tex[8]{{{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}}, {{0, 0, 0, 1}},
                                           {{0, 0, 0, 1}}, {{0, 0, 0, 1}}};
            V4                  oc[4];
            bool                killed = false;
            std::function<V4(uint32_t stage, V4 coord, int mode)> sample;

            float Clamp(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }

            V4 ConstRow(int32_t index)
            {
                for (const auto& d : p->definitions)
                    if (d.type == Reg::Const && static_cast<int32_t>(d.index) == index)
                    {
                        V4 r;
                        std::memcpy(r.v, d.bits, 16);
                        return r;
                    }
                V4 r;
                index = std::min(std::max(index, 0), 255);
                std::memcpy(r.v, consts + index * 4, 16);
                if (p->pixel && p->major == 1)
                    for (float& x : r.v) x = Clamp(x, -1.0f, 1.0f);
                return r;
            }

            V4 Fetch(const dx9::Operand& s)
            {
                switch (s.type)
                {
                case Reg::Temp: return temps[s.index];
                case Reg::Input: return inputs[s.index];
                case Reg::Const:
                {
                    int32_t index = static_cast<int32_t>(s.index);
                    if (s.relative) index += s.rel_type == Reg::Loop ? al.back() : a0[s.rel_component];
                    return ConstRow(index);
                }
                case Reg::Address: return p->pixel && p->major == 1 && p->minor < 4 ? textures[s.index] : inputs[8 + s.index];
                default: return V4{};
                }
            }

            V4 Source(const dx9::Operand& s)
            {
                const V4 raw = Fetch(s);
                V4       r;
                for (int c = 0; c < 4; ++c) r.v[c] = raw.v[s.Component(c)];
                for (float& x : r.v)
                {
                    switch (s.modifier)
                    {
                    case SrcMod::Neg: x = -x; break;
                    case SrcMod::Bias: x = x - 0.5f; break;
                    case SrcMod::BiasNeg: x = 0.5f - x; break;
                    case SrcMod::Sign: x = 2 * x - 1; break;
                    case SrcMod::SignNeg: x = 1 - 2 * x; break;
                    case SrcMod::Comp: x = 1 - x; break;
                    case SrcMod::X2: x = 2 * x; break;
                    case SrcMod::X2Neg: x = -2 * x; break;
                    case SrcMod::Abs: x = std::fabs(x); break;
                    case SrcMod::AbsNeg: x = -std::fabs(x); break;
                    default: break;
                    }
                }
                return r;
            }

            void Write(const dx9::Operand& d, V4 value)
            {
                for (float& x : value.v)
                {
                    if (d.shift) x = d.shift > 0 ? x * static_cast<float>(1 << d.shift) : x / static_cast<float>(1 << -d.shift);
                    if (d.saturate) x = Clamp(x, 0, 1);
                }
                V4* target = nullptr;
                switch (d.type)
                {
                case Reg::Temp: target = &temps[d.index]; break;
                case Reg::Address:
                    if (!p->pixel)
                    {
                        for (int c = 0; c < 4; ++c)
                            if (d.mask & (1 << c)) a0[c] = static_cast<int32_t>(p->major < 2 ? std::floor(value.v[c]) : std::round(value.v[c]));
                        return;
                    }
                    target = &textures[d.index];
                    break;
                case Reg::RastOut: target = d.index == 0 ? &out_pos : &out_fog; break;
                case Reg::AttrOut: target = &out_color[d.index]; break;
                case Reg::Output: target = p->major >= 3 ? &out[d.index] : &out_tex[d.index]; break;
                case Reg::ColorOut: target = &oc[d.index]; break;
                default: return;
                }
                for (int c = 0; c < 4; ++c)
                    if (d.mask & (1 << c)) target->v[c] = value.v[c];
            }

            static V4 Splat(float x) { return V4{{x, x, x, x}}; }

            /// Runs the instructions [begin, end); false at a break (the loop's end).
            bool Run(size_t& at, size_t end)
            {
                const auto& code = p->code;
                while (at < end)
                {
                    const dx9::Instruction& ins = code[at];
                    auto s = [&](int i) { return Source(ins.src[i]); };
                    auto cmp = [](uint8_t c, float a, float b) {
                        switch (c)
                        {
                        case 1: return a > b;
                        case 2: return a == b;
                        case 3: return a >= b;
                        case 4: return a < b;
                        case 5: return a != b;
                        case 6: return a <= b;
                        default: return false;
                        }
                    };
                    V4 r;
                    bool write = ins.has_dst;
                    switch (ins.op)
                    {
                    case dx9::kMov:
                    case dx9::kMova: r = s(0); break;
                    case dx9::kAdd: { V4 a = s(0), b = s(1); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] + b.v[c]; break; }
                    case dx9::kSub: { V4 a = s(0), b = s(1); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] - b.v[c]; break; }
                    case dx9::kMul: { V4 a = s(0), b = s(1); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] * b.v[c]; break; }
                    case dx9::kMad: { V4 a = s(0), b = s(1), d = s(2); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] * b.v[c] + d.v[c]; break; }
                    case dx9::kMin: { V4 a = s(0), b = s(1); for (int c = 0; c < 4; ++c) r.v[c] = std::min(a.v[c], b.v[c]); break; }
                    case dx9::kMax: { V4 a = s(0), b = s(1); for (int c = 0; c < 4; ++c) r.v[c] = std::max(a.v[c], b.v[c]); break; }
                    case dx9::kSlt: { V4 a = s(0), b = s(1); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] < b.v[c] ? 1.0f : 0.0f; break; }
                    case dx9::kSge: { V4 a = s(0), b = s(1); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] >= b.v[c] ? 1.0f : 0.0f; break; }
                    case dx9::kCmp: { V4 a = s(0), b = s(1), d = s(2); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] >= 0 ? b.v[c] : d.v[c]; break; }
                    case dx9::kCnd: { V4 a = s(0), b = s(1), d = s(2); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] > 0.5f ? b.v[c] : d.v[c]; break; }
                    case dx9::kLrp: { V4 a = s(0), b = s(1), d = s(2); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] * (b.v[c] - d.v[c]) + d.v[c]; break; }
                    case dx9::kFrc: { V4 a = s(0); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] - std::floor(a.v[c]); break; }
                    case dx9::kAbs: { V4 a = s(0); for (int c = 0; c < 4; ++c) r.v[c] = std::fabs(a.v[c]); break; }
                    case dx9::kSgn: { V4 a = s(0); for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] > 0 ? 1.0f : a.v[c] < 0 ? -1.0f : 0.0f; break; }
                    case dx9::kDp3: { V4 a = s(0), b = s(1); r = Splat(a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2]); break; }
                    case dx9::kDp4: { V4 a = s(0), b = s(1); r = Splat(a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3]); break; }
                    case dx9::kDp2Add: { V4 a = s(0), b = s(1), d = s(2); r = Splat(a.v[0] * b.v[0] + a.v[1] * b.v[1] + d.v[0]); break; }
                    case dx9::kRcp: { const float x = s(0).v[0]; r = Splat(x == 0 ? std::copysign(kFltMax, x) : 1.0f / x); break; }
                    case dx9::kRsq: { const float x = std::fabs(s(0).v[0]); r = Splat(x == 0 ? kFltMax : 1.0f / std::sqrt(x)); break; }
                    case dx9::kExp:
                    case dx9::kExpP: r = Splat(std::exp2(s(0).v[0])); break;
                    case dx9::kLog:
                    case dx9::kLogP: { const float x = std::fabs(s(0).v[0]); r = Splat(x == 0 ? -kFltMax : std::log2(x)); break; }
                    case dx9::kPow: r = Splat(std::pow(std::fabs(s(0).v[0]), s(1).v[0])); break;
                    case dx9::kNrm:
                    {
                        V4 a = s(0);
                        const float l = a.v[0] * a.v[0] + a.v[1] * a.v[1] + a.v[2] * a.v[2];
                        const float f = l == 0 ? kFltMax : 1.0f / std::sqrt(l);
                        for (int c = 0; c < 4; ++c) r.v[c] = a.v[c] * f;
                        break;
                    }
                    case dx9::kSinCos: { const float x = s(0).v[0]; r = V4{{std::cos(x), std::sin(x), 0, 0}}; break; }
                    case dx9::kCrs:
                    {
                        V4 a = s(0), b = s(1);
                        r = V4{{a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2], a.v[0] * b.v[1] - a.v[1] * b.v[0], 0}};
                        break;
                    }
                    case dx9::kLit:
                    {
                        V4 a = s(0);
                        const float power = Clamp(a.v[3], -128, 128);
                        r = V4{{1, std::max(a.v[0], 0.0f), a.v[0] > 0 ? std::pow(std::max(a.v[1], 0.0f), power) : 0.0f, 1}};
                        break;
                    }
                    case dx9::kDst: { V4 a = s(0), b = s(1); r = V4{{1, a.v[1] * b.v[1], a.v[2], b.v[3]}}; break; }
                    case dx9::kM4x4:
                    case dx9::kM4x3:
                    case dx9::kM3x4:
                    case dx9::kM3x3:
                    case dx9::kM3x2:
                    {
                        const bool four = ins.op == dx9::kM4x4 || ins.op == dx9::kM4x3;
                        const int  rows = ins.op == dx9::kM4x4 || ins.op == dx9::kM3x4 ? 4 : ins.op == dx9::kM3x2 ? 2 : 3;
                        V4 a = s(0);
                        for (int row = 0; row < rows; ++row)
                        {
                            dx9::Operand m = ins.src[1];
                            m.index += static_cast<uint32_t>(row);
                            V4 b = Source(m);
                            r.v[row] = a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + (four ? a.v[3] * b.v[3] : 0.0f);
                        }
                        break;
                    }
                    case dx9::kTexKill:
                    {
                        dx9::Operand reg = ins.dst;
                        reg.swizzle = kXYZW;
                        const V4 v = Fetch(reg);
                        const uint8_t mask = p->major < 2 ? 7 : ins.dst.mask;
                        for (int c = 0; c < 4; ++c)
                            if ((mask & (1 << c)) && v.v[c] < 0) killed = true;
                        write = false;
                        break;
                    }
                    case dx9::kTex:
                    case dx9::kTexLdl:
                    {
                        if (p->pixel && p->major == 1)
                        {
                            const V4 coord = p->minor >= 4 ? s(0) : inputs[8 + ins.dst.index];
                            r = sample(ins.dst.index, coord, 0);
                        }
                        else
                        {
                            const int mode = ins.op == dx9::kTexLdl ? 3 : ins.control;
                            const V4  texel = sample(ins.src[1].index, s(0), mode);
                            // The sampler's swizzle orders the texel (ps 2.x and up).
                            for (int k = 0; k < 4; ++k) r.v[k] = texel.v[ins.src[1].Component(k)];
                        }
                        break;
                    }
                    case dx9::kIf:
                    case dx9::kIfc:
                    {
                        bool taken;
                        if (ins.op == dx9::kIf)
                        {
                            taken = false;
                            for (const auto& d : p->definitions)
                                if (d.type == Reg::ConstBool && d.index == ins.src[0].index) taken = d.bits[0] != 0;
                            if (ins.src[0].modifier == SrcMod::Not) taken = !taken;
                        }
                        else
                            taken = cmp(ins.control, s(0).v[0], s(1).v[0]);
                        // Find the else and the endif of this if.
                        size_t depth = 0, otherwise = 0, close = at;
                        for (size_t i = at + 1; i < code.size(); ++i)
                        {
                            const uint16_t op = code[i].op;
                            if (op == dx9::kIf || op == dx9::kIfc) ++depth;
                            else if (op == dx9::kEndIf)
                            {
                                if (depth == 0)
                                {
                                    close = i;
                                    break;
                                }
                                --depth;
                            }
                            else if (op == dx9::kElse && depth == 0) otherwise = i;
                        }
                        size_t from = taken ? at + 1 : (otherwise ? otherwise + 1 : close);
                        const size_t to = taken ? (otherwise ? otherwise : close) : close;
                        if (!Run(from, to)) return false;
                        at = close + 1;
                        continue;
                    }
                    case dx9::kLoop:
                    case dx9::kRep:
                    {
                        const dx9::Operand& counter = ins.op == dx9::kLoop ? ins.src[1] : ins.src[0];
                        int32_t v[4] = {};
                        for (const auto& d : p->definitions)
                            if (d.type == Reg::ConstInt && d.index == counter.index) std::memcpy(v, d.bits, 16);
                        size_t depth = 0, close = at;
                        for (size_t i = at + 1; i < code.size(); ++i)
                        {
                            const uint16_t op = code[i].op;
                            if (op == dx9::kLoop || op == dx9::kRep) ++depth;
                            else if (op == dx9::kEndLoop || op == dx9::kEndRep)
                            {
                                if (depth == 0)
                                {
                                    close = i;
                                    break;
                                }
                                --depth;
                            }
                        }
                        al.push_back(v[1]);
                        for (int32_t n = 0; n < v[0]; ++n)
                        {
                            size_t from = at + 1;
                            if (!Run(from, close)) break;
                            al.back() += v[2];
                        }
                        al.pop_back();
                        at = close + 1;
                        continue;
                    }
                    case dx9::kTexCoord:
                        if (p->minor >= 4) r = s(0);
                        else
                        {
                            const V4 t = inputs[8 + ins.dst.index];
                            r = V4{{Clamp(t.v[0], 0, 1), Clamp(t.v[1], 0, 1), Clamp(t.v[2], 0, 1), 1}};
                        }
                        break;
                    case dx9::kBreak: return false;
                    case dx9::kBreakc:
                        if (cmp(ins.control, s(0).v[0], s(1).v[0])) return false;
                        write = false;
                        break;
                    default: write = false; break;
                    }
                    if (write) Write(ins.dst, r);
                    ++at;
                }
                return true;
            }
        };

        // --- The driver's side --------------------------------------------------------------------

        /// One pixel of RGBA32F drawn by a vertex and a pixel program from a single point.
        class Lab
        {
        public:
            bool Create(char* why, uint32_t size)
            {
                device::Options options;
                options.floor.present = false;
                if (!context_.Create(options, why, size) || !allocator_.Create(context_, why, size) || !timeline_.Create(context_, why, size) ||
                    !submitter_.Create(timeline_, why, size))
                    return false;
                resources::ImageInfo black;
                black.format = VK_FORMAT_B8G8R8A8_UNORM;
                black.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                if (!allocator_.CreateImage(black, black_) || !bindless_.Create(context_, 256, black_.view, why, size)) return false;

                const VkPushConstantRange  push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
                const VkDescriptorSetLayout set = bindless_.Layout();
                VkPipelineLayoutCreateInfo info{};
                info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                info.setLayoutCount         = 1;
                info.pSetLayouts            = &set;
                info.pushConstantRangeCount = 1;
                info.pPushConstantRanges    = &push;
                if (vkCreatePipelineLayout(context_.Device(), &info, nullptr, &layout_) != VK_SUCCESS) return false;

                if (!allocator_.CreateImage(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                            target_) ||
                    !allocator_.CreateBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, resources::Access::Readback, readback_) ||
                    !allocator_.CreateBuffer(kVertexInputs * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, resources::Access::Upload, vertex_) ||
                    !allocator_.CreateBuffer(4096, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             resources::Access::Upload, vregs_) ||
                    !allocator_.CreateBuffer(4096, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             resources::Access::Upload, pregs_) ||
                    !allocator_.CreateBuffer(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, resources::Access::Upload, staging_))
                    return false;
                return MakeTextures();
            }

            ~Lab()
            {
                if (!context_.Ready()) return;
                timeline_.WaitFor(timeline_.Last());
                for (VkPipeline p : pipelines_) vkDestroyPipeline(context_.Device(), p, nullptr);
                if (layout_) vkDestroyPipelineLayout(context_.Device(), layout_, nullptr);
                bindless_.Destroy();
                for (resources::Image* i : {&black_, &target_, &tex2d_, &cube_}) allocator_.Destroy(*i);
                for (resources::Buffer* b : {&readback_, &vertex_, &vregs_, &pregs_, &staging_}) allocator_.Destroy(*b);
                submitter_.Destroy();
                timeline_.Destroy();
                allocator_.Destroy();
                context_.Destroy();
            }

            bool Clip() const { return context_.Chosen().features.shaderClipDistance; }

            /// The vertex's attributes, by location.
            V4 attributes[kVertexInputs];
            float vregs[256 * 4] = {};
            float pregs[256 * 4] = {};
            Push  push{};
            /// The 2x2 texture (stage 0, 2D) and the 1x1 cube (stage 1): their texels, as the reference reads them.
            V4 texels[4];
            V4 faces[6];
            /// Stage 1 the cube (as the cases below expect), or the 2D texture like every other stage.
            bool cube1 = true;

            /// Draws with @p vs and @p ps; false when a program does not translate or the pipeline is
            /// not made. @p out is the pixel, (-7, -7, -7, -7) when nothing was drawn.
            bool Run(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps, float out[4], std::string& why)
            {
                Options options;
                options.clip_distance = Clip();
                Translation v, p;
                if (!Translate(vs.data(), vs.size(), options, v, why) || !Translate(ps.data(), ps.size(), options, p, why)) return false;
                Validate(v.words, "a vertex program");
                Validate(p.words, "a pixel program");
                const VkDevice dev = context_.Device();
                VkShaderModule modules[2] = {};
                for (int i = 0; i < 2; ++i)
                {
                    const std::vector<uint32_t>& words = i ? p.words : v.words;
                    VkShaderModuleCreateInfo     info{};
                    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                    info.codeSize = words.size() * 4;
                    info.pCode    = words.data();
                    vkCreateShaderModule(dev, &info, nullptr, &modules[i]);
                }
                VkPipelineShaderStageCreateInfo stages[2]{};
                for (int i = 0; i < 2; ++i)
                {
                    stages[i].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    stages[i].stage  = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
                    stages[i].module = modules[i];
                    stages[i].pName  = "main";
                }
                VkVertexInputAttributeDescription attributes[kVertexInputs]{};
                uint32_t                          count = 0;
                for (uint32_t loc = 0; loc < kVertexInputs; ++loc)
                    if (v.info.inputs >> loc & 1) attributes[count++] = {loc, 0, VK_FORMAT_R32G32B32A32_SFLOAT, loc * 16};
                const VkVertexInputBindingDescription binding{0, kVertexInputs * 16, VK_VERTEX_INPUT_RATE_VERTEX};
                VkPipelineVertexInputStateCreateInfo input{};
                input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                input.vertexBindingDescriptionCount   = 1;
                input.pVertexBindingDescriptions      = &binding;
                input.vertexAttributeDescriptionCount = count;
                input.pVertexAttributeDescriptions    = attributes;
                VkPipelineInputAssemblyStateCreateInfo assembly{};
                assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                VkViewport viewport{0.5f, 0.5f, 1.0f, 1.0f, 0.0f, 1.0f};
                VkRect2D   scissor{{0, 0}, {1, 1}};
                VkPipelineViewportStateCreateInfo vp{};
                vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                vp.viewportCount = 1;
                vp.pViewports    = &viewport;
                vp.scissorCount  = 1;
                vp.pScissors     = &scissor;
                VkPipelineRasterizationStateCreateInfo raster{};
                raster.sType     = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                raster.lineWidth = 1.0f;
                VkPipelineMultisampleStateCreateInfo ms{};
                ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineDepthStencilStateCreateInfo depth{};
                depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                VkPipelineColorBlendAttachmentState attachment{};
                attachment.colorWriteMask = 0xF;
                VkPipelineColorBlendStateCreateInfo blend{};
                blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                blend.attachmentCount = 1;
                blend.pAttachments    = &attachment;
                const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
                VkPipelineRenderingCreateInfo rendering{};
                rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                rendering.colorAttachmentCount    = 1;
                rendering.pColorAttachmentFormats = &format;
                VkGraphicsPipelineCreateInfo info{};
                info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                info.pNext               = &rendering;
                info.stageCount          = 2;
                info.pStages             = stages;
                info.pVertexInputState   = &input;
                info.pInputAssemblyState = &assembly;
                info.pViewportState      = &vp;
                info.pRasterizationState = &raster;
                info.pMultisampleState   = &ms;
                info.pDepthStencilState  = &depth;
                info.pColorBlendState    = &blend;
                info.layout              = layout_;
                VkPipeline pipeline      = VK_NULL_HANDLE;
                const VkResult made      = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
                vkDestroyShaderModule(dev, modules[0], nullptr);
                vkDestroyShaderModule(dev, modules[1], nullptr);
                if (made != VK_SUCCESS)
                {
                    why = "the pipeline was not made";
                    return false;
                }
                pipelines_.push_back(pipeline);

                std::memcpy(vertex_.mapped, attributes_data(), kVertexInputs * 16);
                std::memcpy(vregs_.mapped, vregs, sizeof vregs);
                std::memcpy(pregs_.mapped, pregs, sizeof pregs);
                for (resources::Buffer* b : {&vertex_, &vregs_, &pregs_}) allocator_.Flush(*b, 0, b->size);
                Push pc       = push;
                pc.vertex[0]  = static_cast<uint32_t>(vregs_.address);
                pc.vertex[1]  = static_cast<uint32_t>(vregs_.address >> 32);
                pc.pixel[0]   = static_cast<uint32_t>(pregs_.address);
                pc.pixel[1]   = static_cast<uint32_t>(pregs_.address >> 32);
                pc.point_size = 1.0f;
                resources::SamplerKey nearest;
                nearest.filter = 0;
                pc.stages[0]   = StageWord(slot2d_, bindless_.Sampler(nearest));
                pc.stages[1]   = StageWord(cube1 ? slot_cube_ : slot2d_, bindless_.Sampler(nearest));
                for (uint32_t k = 2; k < kStages; ++k) pc.stages[k] = StageWord(slot2d_, bindless_.Sampler(nearest));

                const VkCommandBuffer cmd = submitter_.Begin();
                Barrier(cmd, target_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                VkRenderingAttachmentInfo color{};
                color.sType                       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                color.imageView                   = target_.view;
                color.imageLayout                 = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp                      = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color.storeOp                     = VK_ATTACHMENT_STORE_OP_STORE;
                color.clearValue.color.float32[0] = color.clearValue.color.float32[1] = color.clearValue.color.float32[2] =
                    color.clearValue.color.float32[3] = -7.0f;
                VkRenderingInfo rinfo{};
                rinfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rinfo.renderArea           = {{0, 0}, {1, 1}};
                rinfo.layerCount           = 1;
                rinfo.colorAttachmentCount = 1;
                rinfo.pColorAttachments    = &color;
                vkCmdBeginRendering(cmd, &rinfo);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                const VkDescriptorSet descriptors = bindless_.Set();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &descriptors, 0, nullptr);
                vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_.buffer, &offset);
                vkCmdDraw(cmd, 1, 1, 0, 0);
                vkCmdEndRendering(cmd);
                Barrier(cmd, target_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent      = {1, 1, 1};
                vkCmdCopyImageToBuffer(cmd, target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
                const uint64_t serial = submitter_.Submit();
                if (!serial || timeline_.WaitFor(serial) != frame::Wait::Done)
                {
                    why = "the draw did not complete";
                    return false;
                }
                allocator_.Invalidate(readback_);
                std::memcpy(out, readback_.mapped, 16);
                return true;
            }

        private:
            const float* attributes_data() const { return &attributes[0].v[0]; }

            static void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t layers = 1)
            {
                VkImageMemoryBarrier2 b{};
                b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b.srcAccessMask    = VK_ACCESS_2_MEMORY_WRITE_BIT;
                b.dstStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b.dstAccessMask    = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                b.oldLayout        = from;
                b.newLayout        = to;
                b.image            = image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
                VkDependencyInfo d{};
                d.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                d.imageMemoryBarrierCount = 1;
                d.pImageMemoryBarriers    = &b;
                vkCmdPipelineBarrier2(cmd, &d);
            }

            bool MakeTextures()
            {
                // A 2x2 2D texture and a 1x1 cube with a colour per face, in RGBA32F so the reference reads them exactly.
                const float t[4][4] = {{0.25f, 0.5f, 0.75f, 1.0f}, {1.0f, 0.0f, 0.5f, 0.25f}, {0.0f, 1.0f, 0.0f, 0.5f}, {0.125f, 0.375f, 0.625f, 0.875f}};
                for (int i = 0; i < 4; ++i) std::memcpy(texels[i].v, t[i], 16);
                for (int f = 0; f < 6; ++f) faces[f] = V4{{0.1f * static_cast<float>(f + 1), 0.05f * static_cast<float>(f), 1.0f, 1.0f}};
                resources::ImageInfo info;
                info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                info.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                info.width = info.height = 2;
                if (!allocator_.CreateImage(info, tex2d_)) return false;
                info.width = info.height = 1;
                info.cube = true;
                if (!allocator_.CreateImage(info, cube_)) return false;
                std::memcpy(staging_.mapped, t, sizeof t);
                std::memcpy(static_cast<uint8_t*>(staging_.mapped) + 64, faces, sizeof faces);
                allocator_.Flush(staging_, 0, 64 + sizeof faces);
                const VkCommandBuffer cmd = submitter_.Begin();
                Barrier(cmd, tex2d_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                Barrier(cmd, cube_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent      = {2, 2, 1};
                vkCmdCopyBufferToImage(cmd, staging_.buffer, tex2d_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                region.bufferOffset     = 64;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6};
                region.imageExtent      = {1, 1, 1};
                vkCmdCopyBufferToImage(cmd, staging_.buffer, cube_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                Barrier(cmd, tex2d_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                Barrier(cmd, cube_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6);
                // The black slot's image, defined too.
                Barrier(cmd, black_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                const uint64_t serial = submitter_.Submit();
                if (!serial || timeline_.WaitFor(serial) != frame::Wait::Done) return false;
                slot2d_   = bindless_.AddImage(tex2d_.view);
                slot_cube_ = bindless_.AddImage(cube_.view);
                return true;
            }

            void Validate(const std::vector<uint32_t>& words, const char* what)
            {
#if defined(WXL_VK_SPIRV_VAL)
                spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
                std::string          message;
                tools.SetMessageConsumer([&message](spv_message_level_t, const char*, const spv_position_t&, const char* text) { message += text; });
                if (!tools.Validate(words))
                {
                    std::printf("  spirv-val on %s: %s\n", what, message.c_str());
                    Check(false, "a translated program passes spirv-val");
                }
#else
                (void)words;
                (void)what;
#endif
            }

            device::Context       context_;
            resources::Allocator  allocator_;
            frame::Timeline       timeline_;
            frame::Submitter      submitter_;
            resources::Bindless   bindless_;
            resources::Image      black_, target_, tex2d_, cube_;
            resources::Buffer     readback_, vertex_, vregs_, pregs_, staging_;
            VkPipelineLayout      layout_ = VK_NULL_HANDLE;
            std::vector<VkPipeline> pipelines_;
            uint32_t              slot2d_ = 0, slot_cube_ = 0;
        };

        // --- The programs around the one under test -----------------------------------------------

        /// vs_3_0 passing the position and texture coordinate 0 on.
        std::vector<uint8_t> PassVs()
        {
            Asm a(false, 3, 0);
            a.Dcl(0, 0, Dst(Reg::Input, 0));
            a.Dcl(5, 0, Dst(Reg::Input, 1));
            a.Dcl(0, 0, Dst(Reg::Output, 0));
            a.Dcl(5, 0, Dst(Reg::Output, 1));
            a.Op(dx9::kMov, {Dst(Reg::Output, 0), Src(Reg::Input, 0)});
            a.Op(dx9::kMov, {Dst(Reg::Output, 1), Src(Reg::Input, 1)});
            return a.Bytes();
        }

        /// ps_3_0 writing texture coordinate @p set.
        std::vector<uint8_t> PassPs3(uint32_t set = 0)
        {
            Asm a(true, 3, 0);
            a.Dcl(5, set, Dst(Reg::Input, 0));
            a.Op(dx9::kMov, {Dst(Reg::ColorOut, 0), Src(Reg::Input, 0)});
            return a.Bytes();
        }

        /// ps_2_0 writing t0.
        std::vector<uint8_t> PassPs2()
        {
            Asm a(true, 2, 0);
            a.Op(dx9::kDcl, {0x80000000u, Dst(Reg::Address, 0)});
            a.Op(dx9::kMov, {Dst(Reg::ColorOut, 0), Src(Reg::Address, 0)});
            return a.Bytes();
        }

        bool Near(const float* a, const V4& b, float tolerance)
        {
            for (int c = 0; c < 4; ++c)
            {
                const float scale = std::max(1.0f, std::fabs(b.v[c]));
                if (!(std::fabs(a[c] - b.v[c]) <= tolerance * scale)) return false;
            }
            return true;
        }

        struct Case
        {
            std::string          name;
            std::vector<uint8_t> vs, ps;
            bool                 pixel_under_test = false;
            float                tolerance = 1e-5f;
        };

        /// What the pair computes on the CPU (the pixel).
        bool Reference(Lab& lab, const Case& c, V4& out, bool& killed)
        {
            dx9::Program vp, pp;
            std::string  why;
            std::vector<uint32_t> vt(c.vs.size() / 4), pt(c.ps.size() / 4);
            std::memcpy(vt.data(), c.vs.data(), c.vs.size());
            std::memcpy(pt.data(), c.ps.data(), c.ps.size());
            if (!dx9::Decode(vt.data(), vt.size(), vp, why) || !dx9::Decode(pt.data(), pt.size(), pp, why)) return false;
            auto sample = [&lab](uint32_t stage, V4 coord, int mode) {
                if (mode == 1)
                    for (int i = 0; i < 3; ++i) coord.v[i] /= coord.v[3];
                if (stage == 1 && lab.cube1)
                {
                    // The cube's face: the major axis of the direction.
                    const float ax = std::fabs(coord.v[0]), ay = std::fabs(coord.v[1]), az = std::fabs(coord.v[2]);
                    int face = ax >= ay && ax >= az ? (coord.v[0] >= 0 ? 0 : 1) : ay >= az ? (coord.v[1] >= 0 ? 2 : 3) : (coord.v[2] >= 0 ? 4 : 5);
                    return lab.faces[face];
                }
                // Nearest of 2x2, clamped.
                const int x = std::clamp(static_cast<int>(std::floor(coord.v[0] * 2.0f)), 0, 1);
                const int y = std::clamp(static_cast<int>(std::floor(coord.v[1] * 2.0f)), 0, 1);
                return lab.texels[y * 2 + x];
            };
            Machine v;
            v.p      = &vp;
            v.consts = lab.vregs;
            v.sample = sample;
            for (const auto& d : vp.declarations)
                if (d.type == Reg::Input)
                {
                    const int loc = InputLocation(d.usage, d.usage_index);
                    if (loc >= 0)
                        for (int k = 0; k < 4; ++k)
                            if (d.mask & (1 << k)) v.inputs[d.index].v[k] = lab.attributes[loc].v[k];
                }
            size_t at = 0;
            v.Run(at, vp.code.size());
            // The varyings by location.
            V4 vary[kVaryings];
            for (uint32_t i = 0; i < kVaryings; ++i) vary[i] = i == kFog ? V4{{1, 0, 0, 0}} : V4{{0, 0, 0, 1}};
            if (vp.major >= 3)
            {
                vary[kColor0] = V4{{1, 1, 1, 1}};
                vary[kColor1] = V4{{0, 0, 0, 0}};
                for (const auto& d : vp.declarations)
                {
                    if (d.type != Reg::Output) continue;
                    const int loc = VaryingLocation(d.usage, d.usage_index);
                    if (loc >= 0)
                        for (int k = 0; k < 4; ++k)
                            if (d.mask & (1 << k)) vary[loc].v[k] = v.out[d.index].v[k];
                }
            }
            else
            {
                for (int k = 0; k < 4; ++k)
                {
                    vary[kColor0].v[k] = std::clamp(v.out_color[0].v[k], 0.0f, 1.0f);
                    vary[kColor1].v[k] = std::clamp(v.out_color[1].v[k], 0.0f, 1.0f);
                }
                for (int t = 0; t < 8; ++t) vary[kTexcoord0 + t] = v.out_tex[t];
                vary[kFog] = v.out_fog;
            }
            Machine p;
            p.p      = &pp;
            p.consts = lab.pregs;
            p.sample = sample;
            if (pp.major >= 3)
            {
                for (const auto& d : pp.declarations)
                    if (d.type == Reg::Input)
                    {
                        const int loc = VaryingLocation(d.usage, d.usage_index);
                        if (loc >= 0)
                            for (int k = 0; k < 4; ++k)
                                if (d.mask & (1 << k)) p.inputs[d.index].v[k] = vary[loc].v[k];
                    }
            }
            else
            {
                p.inputs[0] = vary[kColor0];
                p.inputs[1] = vary[kColor1];
                for (int t = 0; t < 8; ++t) p.inputs[8 + t] = vary[kTexcoord0 + t];
                for (int t = 0; t < 4; ++t) p.textures[t] = vary[kTexcoord0 + t];
            }
            at = 0;
            p.Run(at, pp.code.size());
            V4 color = pp.major == 1 ? p.temps[0] : p.oc[0];
            if (pp.major < 3 && (lab.push.flags & kFlagFog))
            {
                const float f = std::clamp(vary[kFog].v[0], 0.0f, 1.0f);
                const uint32_t fc = lab.push.fog_color;
                const float fog[3] = {((fc >> 16) & 0xFF) / 255.0f, ((fc >> 8) & 0xFF) / 255.0f, (fc & 0xFF) / 255.0f};
                for (int k = 0; k < 3; ++k) color.v[k] = fog[k] + (color.v[k] - fog[k]) * f;
            }
            if (lab.push.alpha_ref > 0 && std::floor(std::clamp(color.v[3], 0.0f, 1.0f) * 255.0f + 0.5f) < lab.push.alpha_ref) p.killed = true;
            out    = color;
            killed = p.killed;
            return true;
        }

        void RunCase(Lab& lab, const Case& c)
        {
            float       gpu[4];
            std::string why;
            const bool  ran = lab.Run(c.vs, c.ps, gpu, why);
            V4          expected;
            bool        killed = false;
            const bool  ref    = Reference(lab, c, expected, killed);
            if (killed) expected = V4{{-7, -7, -7, -7}};
            const bool ok = ran && ref && Near(gpu, expected, c.tolerance);
            if (!ok)
                std::printf("  %s: %s gpu (%g %g %g %g) d3d9 (%g %g %g %g)\n", c.name.c_str(), ran ? "" : why.c_str(), gpu[0], gpu[1], gpu[2],
                            gpu[3], expected.v[0], expected.v[1], expected.v[2], expected.v[3]);
            Check(ok, ("the translation computes D3D9's " + c.name).c_str());
        }

        /// A vs_3_0 program around @p body (writing r0), then o1 = r0 read by a passing pixel program.
        Case VertexCase(const std::string& name, const std::function<void(Asm&)>& body, float tolerance = 1e-5f)
        {
            Asm a(false, 3, 0);
            a.Dcl(0, 0, Dst(Reg::Input, 0));
            a.Dcl(5, 0, Dst(Reg::Input, 1));
            a.Dcl(0, 0, Dst(Reg::Output, 0));
            a.Dcl(5, 0, Dst(Reg::Output, 1));
            a.Op(dx9::kMov, {Dst(Reg::Output, 0), Src(Reg::Input, 0)});
            body(a);
            a.Op(dx9::kMov, {Dst(Reg::Output, 1), Src(Reg::Temp, 0)});
            Case c;
            c.name      = name + " (vs_3_0)";
            c.vs        = a.Bytes();
            c.ps        = PassPs3();
            c.tolerance = tolerance;
            return c;
        }

        /// A ps_3_0 program around @p body (writing r0) under the passing vertex program.
        Case PixelCase(const std::string& name, const std::function<void(Asm&)>& body, float tolerance = 1e-5f)
        {
            Asm a(true, 3, 0);
            a.Dcl(5, 0, Dst(Reg::Input, 0));
            a.DclSampler(2, 0);
            a.DclSampler(3, 1);
            body(a);
            a.Op(dx9::kMov, {Dst(Reg::ColorOut, 0), Src(Reg::Temp, 0)});
            Case c;
            c.name             = name + " (ps_3_0)";
            c.vs               = PassVs();
            c.ps               = a.Bytes();
            c.pixel_under_test = true;
            c.tolerance        = tolerance;
            return c;
        }
    }

    void TranslatePureCases()
    {
        Options     options;
        Translation t;
        std::string why;
        // Refusals: what the translator does not write, the client draws without, as D3D9 refuses.
        {
            Asm a(false, 3, 0);
            a.Op(dx9::kCall, {Src(Reg::Label, 0)});
            const auto bytes = a.Bytes();
            Check(!Translate(bytes.data(), bytes.size(), options, t, why) && !why.empty(), "a call is refused, with its reason");
        }
        {
            const uint32_t junk[3] = {0x12345678u, 0, 0x0000FFFFu};
            Check(!Translate(junk, sizeof junk, options, t, why), "a record that is no shader is refused");
            Check(!Translate(junk, 6, options, t, why), "a record of a partial token is refused");
        }
        {
            Asm a(false, 3, 0);
            a.Op(dx9::kMov, {Dst(Reg::Temp, 0), Src(Reg::Const, 0)});
            std::vector<uint8_t> bytes = a.Bytes();
            bytes.resize(bytes.size() - 4); // no end token
            Check(!Translate(bytes.data(), bytes.size(), options, t, why), "a record without its end is refused");
        }
        {
            Asm a(false, 3, 0);
            a.Dcl(0, 0, Dst(Reg::Output, 0));
            a.Op(dx9::kIfc, {Src(Reg::Const, 0), Src(Reg::Const, 1)}, 1);
            const auto bytes = a.Bytes();
            Check(!Translate(bytes.data(), bytes.size(), options, t, why), "an if left open is refused");
        }
        {
            // The register counts the renderer copies: c0..c5 read, c3 defined; relative: all 256.
            Asm a(false, 3, 0);
            a.Dcl(0, 0, Dst(Reg::Output, 0));
            a.Def(3, 1, 2, 3, 4);
            a.Op(dx9::kAdd, {Dst(Reg::Output, 0), Src(Reg::Const, 5), Src(Reg::Const, 3)});
            auto bytes = a.Bytes();
            Check(Translate(bytes.data(), bytes.size(), options, t, why) && t.info.const_rows == 6 && !t.info.pixel && t.info.major == 3,
                  "a vertex program reports the register rows it reads (c5: 6 rows)");
            Asm b(false, 2, 0);
            b.Op(dx9::kMova, {Dst(Reg::Address, 0, 1), Src(Reg::Const, 0, Swz(0, 0, 0, 0))});
            b.t.push_back(dx9::kMov | 3u << 24);
            b.t.push_back(Dst(Reg::RastOut, 0));
            b.t.push_back(Src(Reg::Const, 4, kXYZW, SrcMod::None, true));
            b.t.push_back(Rel(Reg::Address, 0));
            bytes = b.Bytes();
            Check(Translate(bytes.data(), bytes.size(), options, t, why) && t.info.const_rows == kRegisterRows,
                  "a0-relative reads report the whole register file");
        }

        // The translation cache: memory, its pack on disk, refusals kept.
        const std::filesystem::path pack = std::filesystem::temp_directory_path() / "wxl-vk-test-shaders.pack";
        std::error_code ignored;
        std::filesystem::remove(pack, ignored);
        const auto good = PassVs();
        Asm        badAsm(false, 3, 0);
        badAsm.Op(dx9::kCall, {Src(Reg::Label, 0)});
        const auto bad = badAsm.Bytes();
        {
            Cache cache;
            cache.Open(pack.string(), options);
            Check(cache.Get(good.data(), good.size(), why) != nullptr && !cache.Get(bad.data(), bad.size(), why) && !why.empty(),
                  "the cache translates a record and refuses another");
            const auto again = cache.Get(good.data(), good.size(), why);
            const Cache::Stats s = cache.Counters();
            Check(again && s.translated == 1 && s.refused == 1, "a record asked for again is not translated again");
            cache.Prewarm({PassPs3(), PassPs2()});
            for (int i = 0; i < 200 && (cache.Pending() || cache.Counters().prewarmed < 2); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            Check(cache.Counters().prewarmed == 2, "the worker translates the records it is handed ahead");
        }
        {
            Cache cache;
            cache.Open(pack.string(), options);
            const auto  read = cache.Get(good.data(), good.size(), why);
            std::string no;
            const auto  refused = cache.Get(bad.data(), bad.size(), no);
            const auto  ahead   = cache.Get(PassPs2().data(), PassPs2().size(), why);
            const Cache::Stats s = cache.Counters();
            Translation direct;
            Translate(good.data(), good.size(), options, direct, why);
            Check(read && read->words == direct.words && !read->info.pixel && !refused && !no.empty() && ahead && ahead->info.pixel &&
                      s.translated == 0 && s.from_disk == 3,
                  "a new launch reads the module, the refusal and the prewarmed record back from the pack");
            Options other;
            other.clip_distance = !options.clip_distance;
            Check(!(Cache::KeyOf(good.data(), good.size(), options) == Cache::KeyOf(good.data(), good.size(), other)),
                  "another device's options key another module");
        }
        {
            // A damaged pack starts again rather than reading garbage.
            FILE* f = std::fopen(pack.string().c_str(), "r+b");
            if (f)
            {
                std::fseek(f, 8, SEEK_SET);
                const uint32_t junk = 0xDEADBEEFu;
                std::fwrite(&junk, 4, 1, f);
                std::fclose(f);
            }
            Cache cache;
            cache.Open(pack.string(), options);
            cache.Get(good.data(), good.size(), why);
            Check(cache.Counters().translated == 1 && cache.Counters().from_disk == 0, "a damaged pack is started again");
        }
        std::filesystem::remove(pack, ignored);
    }

    /// wxl.modern-adt's height blend (shader::BlendByHeight) on the Vulkan renderer: the client's
    /// terrain program reduced to its blend, rewritten, translated and drawn on the driver, against
    /// D3D9's semantics (the interpreter above) and against retail's formula worked out apart; a
    /// chunk without heights drawn bit for bit as the program before the rewrite.
    void TerrainHeightBlendCases(Lab& lab)
    {
        namespace O = dx9;
        using wxl::modern_adt::shader::BlendByHeight;
        lab.cube1 = false;
        // Texel centres of the 2x2 texture: layer k's coordinate picks its texel; its height is that texel's alpha.
        const float centre[4][2] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}};
        uint32_t    drawn = 0;
        for (uint32_t layers = 2; layers <= 4; ++layers)
        {
            // The vertex program: texture coordinate k from c(10 + k).
            Asm v(false, 3, 0);
            v.Dcl(0, 0, Dst(Reg::Input, 0));
            v.Dcl(0, 0, Dst(Reg::Output, 0));
            for (uint32_t k = 0; k <= layers; ++k) v.Dcl(5, k, Dst(Reg::Output, 1 + k));
            v.Op(O::kMov, {Dst(Reg::Output, 0), Src(Reg::Input, 0)});
            for (uint32_t k = 0; k <= layers; ++k) v.Op(O::kMov, {Dst(Reg::Output, 1 + k), Src(Reg::Const, 10 + k)});
            // The pixel program: Terrain1's blend, layer k on sampler k at v(k), the alphas on sampler L.
            Asm p(true, 3, 0);
            for (uint32_t k = 0; k <= layers; ++k) p.Dcl(5, k, Dst(Reg::Input, k));
            for (uint32_t k = 0; k <= layers; ++k) p.DclSampler(2, k);
            for (uint32_t k = 0; k <= layers; ++k) p.Op(O::kTex, {Dst(Reg::Temp, k), Src(Reg::Input, k), Src(Reg::Sampler, k)});
            uint32_t acc = 0;
            for (uint32_t k = 1; k < layers; ++k)
            {
                p.Op(O::kLrp, {Dst(Reg::Temp, 10 + k), Src(Reg::Temp, layers, (k - 1) * 0x55u), Src(Reg::Temp, k), Src(Reg::Temp, acc)});
                acc = 10 + k;
            }
            p.Op(O::kMov, {Dst(Reg::ColorOut, 0), Src(Reg::Temp, acc)});
            const std::vector<uint8_t> stock = p.Bytes();
            std::vector<uint32_t>      words;
            wxl::modern_adt::shader::Found found;
            std::string                    why;
            if (!BlendByHeight(stock.data(), stock.size(), words, found, why))
            {
                Check(false, ("the height blend rewrites a terrain program: " + why).c_str());
                continue;
            }
            std::vector<uint8_t> blended(words.size() * 4);
            std::memcpy(blended.data(), words.data(), blended.size());
            for (uint32_t pick = 0; pick < 8; ++pick)
            {
                // Which texel each layer and the alphas read, and which layers carry a height.
                uint32_t texel[5];
                for (uint32_t k = 0; k <= layers; ++k) texel[k] = (pick + k * 3) % 4;
                std::memset(lab.vregs, 0, sizeof lab.vregs);
                std::memset(lab.pregs, 0, sizeof lab.pregs);
                for (uint32_t k = 0; k <= layers; ++k)
                {
                    lab.vregs[(10 + k) * 4 + 0] = centre[texel[k]][0];
                    lab.vregs[(10 + k) * 4 + 1] = centre[texel[k]][1];
                    lab.vregs[(10 + k) * 4 + 3] = 1.0f;
                }
                float params[4][4] = {};
                const bool heights = pick % 4 != 0;
                for (uint32_t k = 0; k < layers; ++k)
                    if (heights && (pick + k) % 3 != 0)
                    {
                        params[k][0] = 2.0f + static_cast<float>(pick + k);
                        params[k][1] = pick % 2 ? 1.0f : 0.5f;
                        params[k][2] = 1.0f;
                    }
                std::memcpy(&lab.pregs[32 * 4], params, sizeof params);
                Case k;
                k.name = "the height blend of " + std::to_string(layers) + " layers, pick " + std::to_string(pick);
                k.vs   = v.Bytes();
                k.ps   = blended;
                k.tolerance = 2e-5f;
                RunCase(lab, k);
                ++drawn;
                // Retail's formula, worked out apart from the texels.
                float gpu[4], before[4];
                std::string w;
                const bool ran = lab.Run(k.vs, blended, gpu, w) && lab.Run(k.vs, stock, before, w);
                const V4& alphas = lab.texels[texel[layers]];
                float wgt[4] = {}, rem = 1.0f, pct[4] = {}, top = 0.0f, sum = 0.0f, want[4] = {};
                for (int j = static_cast<int>(layers) - 1; j >= 1; --j)
                {
                    wgt[j] = alphas.v[j - 1] * rem;
                    rem *= 1.0f - alphas.v[j - 1];
                }
                wgt[0] = rem;
                for (uint32_t j = 0; j < layers; ++j)
                {
                    const float h = lab.texels[texel[j]].v[3];
                    pct[j] = wgt[j] * (params[j][2] != 0.0f ? h * params[j][0] + params[j][1] : 1.0f);
                    top    = std::max(top, pct[j]);
                }
                for (uint32_t j = 0; j < layers; ++j)
                {
                    if (heights) pct[j] *= 1.0f - std::clamp(top - pct[j], 0.0f, 1.0f);
                    sum += pct[j];
                }
                for (uint32_t j = 0; j < layers; ++j)
                    for (int c = 0; c < 4; ++c) want[c] += (heights ? pct[j] / std::max(sum, 1e-6f) : wgt[j]) * lab.texels[texel[j]].v[c];
                float worst = 0.0f;
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(gpu[c] - want[c]));
                Check(ran && worst < 2e-5f, ("the Vulkan renderer draws retail's height blend: " + k.name).c_str());
                if (!heights)
                    Check(ran && std::memcmp(gpu, before, sizeof gpu) == 0,
                          ("a chunk without heights draws bit for bit as the client's program: " + k.name).c_str());
            }
        }
        std::memset(lab.vregs, 0, sizeof lab.vregs);
        std::memset(lab.pregs, 0, sizeof lab.pregs);
        lab.cube1 = true;
        std::printf("  height blend: %u terrain draws on the driver\n", drawn);
    }

    bool TranslateLiveCases()
    {
        Lab  lab;
        char why[256] = {};
        if (!lab.Create(why, sizeof why))
        {
            std::printf("  translator lab: %s\n", why);
            return false;
        }
        // The vertex: at the pixel's centre, a texture coordinate, a colour; the registers.
        lab.attributes[0] = V4{{0.0f, 0.0f, 0.5f, 1.0f}};
        lab.attributes[6] = V4{{0.3f, 0.8f, -0.25f, 2.0f}};
        lab.attributes[4] = V4{{0.2f, 0.4f, 0.6f, 0.8f}};
        const float rows[8][4] = {{1.5f, -2.0f, 0.25f, 4.0f},   {-0.5f, 3.0f, 2.0f, -1.0f}, {0.75f, 0.5f, -3.0f, 0.0f},
                                  {2.0f, 0.0f, -1.0f, 0.125f}, {0.0f, 0.0f, 0.0f, 0.0f},   {10.0f, 20.0f, 30.0f, 40.0f},
                                  {0.1f, 0.2f, 0.3f, 0.4f},     {7.0f, 8.0f, 9.0f, 1.0f}};
        for (int r = 0; r < 8; ++r)
        {
            std::memcpy(lab.vregs + r * 4, rows[r], 16);
            std::memcpy(lab.pregs + r * 4, rows[r], 16);
        }
        for (int r = 8; r < 64; ++r)
            for (int k = 0; k < 4; ++k) lab.vregs[r * 4 + k] = lab.pregs[r * 4 + k] = static_cast<float>(r) + 0.25f * static_cast<float>(k);

        using O = dx9::Opcode;
        auto c = [](uint32_t i, uint32_t swz = kXYZW, SrcMod mod = SrcMod::None) { return Src(Reg::Const, i, swz, mod); };
        auto r = [](uint32_t i, uint32_t swz = kXYZW, SrcMod mod = SrcMod::None) { return Src(Reg::Temp, i, swz, mod); };
        auto d = [](uint32_t i, uint32_t mask = 0xF, bool sat = false, int shift = 0) { return Dst(Reg::Temp, i, mask, sat, shift); };

        std::vector<Case> cases;
        // Every arithmetic instruction, in a vertex and in a pixel program.
        struct Arith
        {
            const char* name;
            uint16_t    op;
            int         sources;
            float       tolerance;
        };
        static const Arith kArith[] = {
            {"mov", O::kMov, 1, 1e-6f},  {"add", O::kAdd, 2, 1e-6f},   {"sub", O::kSub, 2, 1e-6f},   {"mul", O::kMul, 2, 1e-6f},
            {"mad", O::kMad, 3, 1e-5f},  {"min", O::kMin, 2, 1e-6f},   {"max", O::kMax, 2, 1e-6f},   {"slt", O::kSlt, 2, 1e-6f},
            {"sge", O::kSge, 2, 1e-6f},  {"dp3", O::kDp3, 2, 1e-5f},   {"dp4", O::kDp4, 2, 1e-5f},   {"rcp", O::kRcp, 1, 1e-5f},
            {"rsq", O::kRsq, 1, 1e-4f},  {"exp", O::kExp, 1, 1e-4f},   {"log", O::kLog, 1, 1e-4f},   {"frc", O::kFrc, 1, 1e-6f},
            {"abs", O::kAbs, 1, 1e-6f},  {"nrm", O::kNrm, 1, 1e-4f},   {"pow", O::kPow, 2, 1e-3f},   {"lrp", O::kLrp, 3, 1e-5f},
            {"crs", O::kCrs, 2, 1e-5f},  {"sgn", O::kSgn, 1, 1e-6f},   {"lit", O::kLit, 1, 1e-3f},   {"dst", O::kDst, 2, 1e-6f},
            {"cmp", O::kCmp, 3, 1e-6f},  {"dp2add", O::kDp2Add, 3, 1e-5f},
        };
        for (const Arith& a : kArith)
        {
            const bool pixelOnly = a.op == O::kCmp || a.op == O::kDp2Add;
            const bool vertexOnly = a.op == O::kLit || a.op == O::kDst || a.op == O::kSgn || a.op == O::kCrs;
            auto body = [&a, c](Asm& as) {
                std::vector<uint32_t> ops = {Dst(Reg::Temp, 0)};
                const uint32_t        srcs[3] = {c(0, Swz(0, 2, 1, 3)), c(1, kXYZW, SrcMod::Neg), c(2, Swz(3, 3, 1, 0), SrcMod::Abs)};
                for (int s = 0; s < a.sources; ++s) ops.push_back(srcs[s]);
                as.t.push_back(a.op | static_cast<uint32_t>(ops.size()) << 24);
                as.t.insert(as.t.end(), ops.begin(), ops.end());
            };
            if (!pixelOnly) cases.push_back(VertexCase(a.name, body, a.tolerance));
            if (!vertexOnly) cases.push_back(PixelCase(a.name, body, a.tolerance));
        }
        // Zeros where D3D9 keeps the largest float: rcp, rsq and log of 0, and 0 * rcp(0).
        cases.push_back(VertexCase("rcp, rsq, log of 0 and 0 * rcp(0)", [&](Asm& a) {
            a.Op(O::kRcp, {d(0, 1), c(4, Swz(0, 0, 0, 0))});
            a.Op(O::kRsq, {d(0, 2), c(4, Swz(1, 1, 1, 1))});
            a.Op(O::kLog, {d(0, 4), c(4, Swz(2, 2, 2, 2))});
            a.Op(O::kMul, {d(0, 8), r(0, Swz(0, 0, 0, 0)), c(4)});
        }));
        // Source modifiers, write masks, saturation.
        static const SrcMod kMods[] = {SrcMod::Bias, SrcMod::BiasNeg, SrcMod::Sign, SrcMod::SignNeg, SrcMod::Comp, SrcMod::X2, SrcMod::X2Neg, SrcMod::AbsNeg};
        for (SrcMod mod : kMods)
        {
            char name[48];
            std::snprintf(name, sizeof name, "source modifier %u", static_cast<unsigned>(mod));
            cases.push_back(PixelCase(name, [&, mod](Asm& a) { a.Op(O::kMov, {d(0), c(6, kXYZW, mod)}); }));
        }
        cases.push_back(VertexCase("a write mask (.xz) and saturation", [&](Asm& a) {
            a.Op(O::kMov, {d(0), c(3)});
            a.Op(O::kAdd, {d(0, 5, true), c(0), c(1)});
        }));
        cases.push_back(VertexCase("sincos into .xy", [&](Asm& a) {
            a.Op(O::kMov, {d(0), c(6)});
            a.Op(O::kSinCos, {d(0, 3), c(0, Swz(2, 2, 2, 2))});
        }, 1e-5f));
        cases.push_back(VertexCase("m4x4, m4x3, m3x3, m3x2 against c8..", [&](Asm& a) {
            a.Op(O::kM4x4, {d(0), c(0), c(8)});
            a.Op(O::kM4x3, {d(1), c(1), c(12)});
            a.Op(O::kM3x3, {d(2), c(2), c(16)});
            a.Op(O::kM3x2, {d(3), c(3), c(20)});
            a.Op(O::kAdd, {d(0), r(0), r(1)});
            a.Op(O::kAdd, {d(0), r(0), r(2)});
            a.Op(O::kAdd, {d(0), r(0), r(3)});
        }, 1e-5f));
        cases.push_back(VertexCase("a vertex input with a swizzle", [&](Asm& a) { a.Op(O::kMul, {d(0), Src(Reg::Input, 1, Swz(3, 2, 1, 0)), c(0)}); }));

        // Constants the program sets itself, and relative addressing.
        cases.push_back(VertexCase("def overriding the engine's register", [&](Asm& a) {
            a.Def(5, 1.0f, -2.0f, 3.5f, 0.0f);
            a.Op(O::kAdd, {d(0), c(5), c(6)});
        }));
        cases.push_back(VertexCase("mova and c[a0.x + 8]", [&](Asm& a) {
            a.Op(O::kMova, {Dst(Reg::Address, 0, 1), c(7, Swz(3, 3, 3, 3))}); // a0.x = round(1.0) = 1
            a.t.push_back(O::kAdd | 4u << 24);
            a.t.push_back(d(0));
            a.t.push_back(Src(Reg::Const, 8, kXYZW, SrcMod::None, true));
            a.t.push_back(Rel(Reg::Address, 0));
            a.t.push_back(c(0));
        }));
        cases.push_back(VertexCase("a loop over aL summing c[aL + 10], one of them defined", [&](Asm& a) {
            a.DefI(0, 4, 2, 3, 0); // 4 iterations, aL = 2, 5, 8, 11
            a.Def(15, 100.0f, 0.0f, 0.0f, 0.0f);
            a.Op(O::kMov, {d(0), c(4)});
            a.t.push_back(O::kLoop | 2u << 24);
            a.t.push_back(Src(Reg::Loop, 0));
            a.t.push_back(Src(Reg::ConstInt, 0));
            a.t.push_back(O::kAdd | 4u << 24);
            a.t.push_back(d(0));
            a.t.push_back(r(0));
            a.t.push_back(Src(Reg::Const, 10, kXYZW, SrcMod::None, true));
            a.t.push_back(Rel(Reg::Loop, 0));
            a.Op(O::kEndLoop, {});
        }));
        cases.push_back(VertexCase("rep with an if, an else and a breakc", [&](Asm& a) {
            a.DefI(1, 6, 0, 0, 0);
            a.Def(9, 1.0f, 2.0f, 0.5f, 7.0f);
            a.Op(O::kMov, {d(0), c(4)});
            a.Op(O::kRep, {Src(Reg::ConstInt, 1)});
            a.Op(O::kAdd, {d(0, 1), r(0, Swz(0, 0, 0, 0)), c(9, Swz(0, 0, 0, 0))});
            a.Op(O::kIfc, {r(0, Swz(0, 0, 0, 0)), c(9, Swz(1, 1, 1, 1))}, 1); // if r0.x > 2
            a.Op(O::kAdd, {d(0, 2), r(0, Swz(1, 1, 1, 1)), c(9, Swz(2, 2, 2, 2))});
            a.Op(O::kElse, {});
            a.Op(O::kAdd, {d(0, 4), r(0, Swz(2, 2, 2, 2)), c(9, Swz(0, 0, 0, 0))});
            a.Op(O::kEndIf, {});
            a.Op(O::kBreakc, {r(0, Swz(0, 0, 0, 0)), c(9, Swz(3, 3, 3, 3))}, 3); // break if r0.x >= 7
            a.Op(O::kEndRep, {});
        }));
        cases.push_back(VertexCase("if on a defined bool, and its negation", [&](Asm& a) {
            a.DefB(2, true);
            a.Op(O::kMov, {d(0), c(4)});
            a.Op(O::kIf, {Src(Reg::ConstBool, 2)});
            a.Op(O::kMov, {d(0, 1), c(0)});
            a.Op(O::kEndIf, {});
            a.Op(O::kIf, {Src(Reg::ConstBool, 2, kXYZW, SrcMod::Not)});
            a.Op(O::kMov, {d(0, 2), c(1)});
            a.Op(O::kEndIf, {});
            a.Op(O::kIf, {Src(Reg::ConstBool, 3)}); // undefined: false
            a.Op(O::kMov, {d(0, 4), c(2)});
            a.Op(O::kEndIf, {});
        }));

        // Textures.
        cases.push_back(PixelCase("texld of a 2D texture at the coordinate", [&](Asm& a) {
            a.Op(O::kTex, {d(0), Src(Reg::Input, 0), Src(Reg::Sampler, 0)});
        }));
        cases.push_back(PixelCase("texldl, an explicit level, with a sampler swizzle", [&](Asm& a) {
            a.Def(9, 0.75f, 0.25f, 0.0f, 0.0f);
            a.Op(O::kTexLdl, {d(0), c(9), Src(Reg::Sampler, 0, Swz(2, 1, 0, 3))});
        }));
        cases.push_back(PixelCase("texldp, divided by w", [&](Asm& a) {
            a.Def(9, 1.5f, 0.5f, 0.0f, 2.0f);
            a.Op(O::kTex, {d(0), c(9), Src(Reg::Sampler, 0)}, 1);
        }));
        cases.push_back(PixelCase("texld of a cube along -y", [&](Asm& a) {
            a.Def(9, 0.1f, -1.0f, 0.2f, 0.0f);
            a.Op(O::kTex, {d(0), c(9), Src(Reg::Sampler, 1)});
        }));
        cases.push_back(PixelCase("texkill on a negative component", [&](Asm& a) {
            a.Op(O::kMov, {d(0), c(0)});
            a.Op(O::kTexKill, {Dst(Reg::Input, 0, 0x7)}); // the coordinate's z is negative
        }));
        cases.push_back(PixelCase("texkill on positive components", [&](Asm& a) {
            a.Op(O::kMov, {d(0), c(0)});
            a.Op(O::kTexKill, {Dst(Reg::Input, 0, 0x3)});
        }));

        // vs_1_1 and vs_2_0: oT, oD saturated, oFog, a0 floored; ps 1.x and 2.0.
        {
            Asm v(false, 1, 1);
            v.Dcl(0, 0, Dst(Reg::Input, 0));
            v.Dcl(10, 0, Dst(Reg::Input, 3));
            v.Op(O::kMov, {Dst(Reg::RastOut, 0), Src(Reg::Input, 0)});
            v.Op(O::kMov, {Dst(Reg::Address, 0, 1), c(0, Swz(0, 0, 0, 0))}); // floor(1.5) = 1
            v.Op(O::kAdd, {Dst(Reg::Output, 0), Src(Reg::Const, 8, kXYZW, SrcMod::None, true), Src(Reg::Input, 3)});
            v.Op(O::kMul, {Dst(Reg::AttrOut, 0), c(5), c(6)}); // saturated to 1
            Case k;
            k.name = "vs_1_1: a0 floored, oT0, oD0 saturated";
            k.vs   = v.Bytes();
            Asm p(true, 2, 0);
            p.Op(O::kDcl, {0x80000000u, Dst(Reg::Address, 0)});
            p.Op(O::kDcl, {0x80000000u, Dst(Reg::Input, 0)});
            p.Op(O::kAdd, {Dst(Reg::ColorOut, 0), Src(Reg::Address, 0), Src(Reg::Input, 0)});
            k.ps = p.Bytes();
            cases.push_back(k);
        }
        {
            Asm p(true, 1, 1);
            p.Op(O::kTex, {Dst(Reg::Address, 0)});
            p.Op(O::kMul, {Dst(Reg::Temp, 0, 0xF, false, 1), Src(Reg::Address, 0), c(5)}); // _x2, c5 clamped to 1
            Case k;
            k.name = "ps_1_1: tex t0, a constant clamped to 1, a _x2 shift";
            Asm v(false, 2, 0);
            v.Dcl(0, 0, Dst(Reg::Input, 0));
            v.Op(O::kMov, {Dst(Reg::RastOut, 0), Src(Reg::Input, 0)});
            v.Def(30, 0.6f, 0.2f, 0.0f, 1.0f);
            v.Op(O::kMov, {Dst(Reg::Output, 0), Src(Reg::Const, 30)});
            k.vs = v.Bytes();
            k.ps = p.Bytes();
            cases.push_back(k);
        }
        {
            Asm p(true, 1, 4);
            p.Op(O::kTexCoord, {Dst(Reg::Temp, 1, 0x7), Src(Reg::Address, 0)});
            p.t.push_back(dx9::kPhase);
            p.Op(O::kTex, {Dst(Reg::Temp, 0), Src(Reg::Temp, 1)});
            p.Op(O::kLrp, {Dst(Reg::Temp, 0), Src(Reg::Temp, 1, Swz(0, 0, 0, 0)), Src(Reg::Temp, 0), c(6, kXYZW, SrcMod::Comp)});
            Case k;
            k.name = "ps_1_4: texcrd, phase, a dependent texld, lrp with a complemented constant";
            Asm v(false, 2, 0);
            v.Dcl(0, 0, Dst(Reg::Input, 0));
            v.Op(O::kMov, {Dst(Reg::RastOut, 0), Src(Reg::Input, 0)});
            v.Def(30, 0.6f, 0.7f, 0.5f, 1.0f);
            v.Op(O::kMov, {Dst(Reg::Output, 0), Src(Reg::Const, 30)});
            k.vs = v.Bytes();
            k.ps = p.Bytes();
            cases.push_back(k);
        }

        for (const Case& k : cases) RunCase(lab, k);

        // D3D9's fog after a ps_2_0 program, from the vertex program's oFog; none after ps_3_0.
        {
            Asm v(false, 2, 0);
            v.Dcl(0, 0, Dst(Reg::Input, 0));
            v.Op(O::kMov, {Dst(Reg::RastOut, 0), Src(Reg::Input, 0)});
            v.Def(30, 0.25f, 0.0f, 0.0f, 0.0f);
            v.Op(O::kMov, {Dst(Reg::RastOut, 1, 1), Src(Reg::Const, 30)});
            v.Def(31, 0.8f, 0.6f, 0.4f, 0.5f);
            v.Op(O::kMov, {Dst(Reg::Output, 0), Src(Reg::Const, 31)});
            lab.push.flags     = kFlagFog;
            lab.push.fog_color = 0xFF0080FFu;
            Case k;
            k.name = "D3D9's fog after ps_2_0 (a quarter of the colour, three quarters fog)";
            k.vs   = v.Bytes();
            k.ps   = PassPs2();
            k.tolerance = 1e-5f;
            RunCase(lab, k);
            Case n = VertexCase("no fog after ps_3_0", [&](Asm& a) { a.Op(O::kMov, {d(0), c(6)}); });
            RunCase(lab, n);
            lab.push.flags     = 0;
            lab.push.fog_color = 0;
        }
        // The alpha test: an alpha below the reference discards; at it, passes.
        {
            lab.push.alpha_ref = 128.0f;
            RunCase(lab, PixelCase("the alpha test discarding 0.4 below 128", [&](Asm& a) {
                a.Def(9, 1.0f, 1.0f, 1.0f, 0.4f);
                a.Op(O::kMov, {d(0), c(9)});
            }));
            RunCase(lab, PixelCase("the alpha test passing 128/255", [&](Asm& a) {
                a.Def(9, 1.0f, 1.0f, 1.0f, 128.0f / 255.0f);
                a.Op(O::kMov, {d(0), c(9)});
            }));
            lab.push.alpha_ref = 0.0f;
        }
        // The user clip plane, against the clip-space position.
        if (lab.Clip())
        {
            float       out[4];
            std::string w;
            lab.push.flags        = kFlagClip;
            lab.push.clip_plane[0] = 0.0f;
            lab.push.clip_plane[2] = -1.0f; // the position's z is 0.5: clipped
            const Case k = VertexCase("clip", [&](Asm& a) { a.Op(O::kMov, {d(0), c(6)}); });
            const bool clipped = lab.Run(k.vs, k.ps, out, w) && out[0] == -7.0f;
            lab.push.clip_plane[2] = 1.0f;
            const bool kept = lab.Run(k.vs, k.ps, out, w) && std::fabs(out[0] - 0.1f) < 1e-6f;
            Check(clipped && kept, "the user clip plane clips a vertex behind it and keeps one in front");
            lab.push.flags = 0;
            std::memset(lab.push.clip_plane, 0, sizeof lab.push.clip_plane);
        }
        TerrainHeightBlendCases(lab);
        std::printf("  translator: %zu programs against D3D9's formulas\n", cases.size() + 6);
        return true;
    }
}
