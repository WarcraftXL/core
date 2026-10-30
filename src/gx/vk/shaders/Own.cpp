// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The present pass's two shaders. What each computes, in GLSL terms:
//
//   vertex:   layout(push_constant) uniform Rect { vec4 rect; };   // x0, y0, x1, y1 in clip space
//             uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
//             gl_Position = vec4(rect.xy + uv * (rect.zw - rect.xy), 0.0, 1.0);
//
//   fragment: vec4 c = texture(source, uv);
//             vec3 x  = clamp(c.rgb, 0.0, 1.0) * 255.0;
//             vec3 lo = floor(x), hi = min(lo + 1.0, 255.0), t = x - lo;
//             out = vec4(mix(ramp[lo].rgb, ramp[hi].rgb, t) per channel, c.a);
//
//   the cursor's fragment adds, after the sample: if (c.a < 224.0 / 255.0) discard;

#include "gx/vk/shaders/Own.hpp"

#include "gx/vk/shaders/Spirv.hpp"

#include <spirv/unified1/GLSL.std.450.h>

namespace wxl::vk::shaders
{
    std::vector<uint32_t> PresentVertex()
    {
        spirv::Module m(SpvExecutionModelVertex);
        const spirv::Id i32  = m.Int();
        const spirv::Id f32  = m.Float();
        const spirv::Id vec2 = m.Vector(f32, 2);
        const spirv::Id vec4 = m.Vector(f32, 4);

        const spirv::Id block = m.Struct({vec4});
        m.MemberDecorate(block, 0, SpvDecorationOffset, {0});
        m.Decorate(block, SpvDecorationBlock);
        const spirv::Id rect  = m.Variable(m.Pointer(SpvStorageClassPushConstant, block), SpvStorageClassPushConstant);
        m.Name(rect, "rect");
        const spirv::Id index = m.Variable(m.Pointer(SpvStorageClassInput, i32), SpvStorageClassInput);
        const spirv::Id pos   = m.Variable(m.Pointer(SpvStorageClassOutput, vec4), SpvStorageClassOutput);
        const spirv::Id uv    = m.Variable(m.Pointer(SpvStorageClassOutput, vec2), SpvStorageClassOutput);
        m.Decorate(index, SpvDecorationBuiltIn, {SpvBuiltInVertexIndex});
        m.Decorate(pos, SpvDecorationBuiltIn, {SpvBuiltInPosition});
        m.Decorate(uv, SpvDecorationLocation, {0});
        m.Name(uv, "uv");

        const spirv::Id one  = m.ConstInt(1);
        const spirv::Id two  = m.ConstInt(2);
        const spirv::Id f0   = m.ConstFloat(0.0f);
        const spirv::Id f1   = m.ConstFloat(1.0f);

        m.BeginMain({index, pos, uv});
        const spirv::Id i      = m.Op(SpvOpLoad, i32, {index});
        const spirv::Id shifted = m.Op(SpvOpShiftLeftLogical, i32, {i, one});
        const spirv::Id xi     = m.Op(SpvOpBitwiseAnd, i32, {shifted, two});
        const spirv::Id yi     = m.Op(SpvOpBitwiseAnd, i32, {i, two});
        const spirv::Id x      = m.Op(SpvOpConvertSToF, f32, {xi});
        const spirv::Id y      = m.Op(SpvOpConvertSToF, f32, {yi});
        m.OpVoid(SpvOpStore, {uv, m.Op(SpvOpCompositeConstruct, vec2, {x, y})});
        const spirv::Id r   = m.Op(SpvOpLoad, vec4, {m.Op(SpvOpAccessChain, m.Pointer(SpvStorageClassPushConstant, vec4), {rect, m.ConstInt(0)})});
        const spirv::Id x0  = m.Op(SpvOpCompositeExtract, f32, {r, 0});
        const spirv::Id y0  = m.Op(SpvOpCompositeExtract, f32, {r, 1});
        const spirv::Id x1  = m.Op(SpvOpCompositeExtract, f32, {r, 2});
        const spirv::Id y1  = m.Op(SpvOpCompositeExtract, f32, {r, 3});
        const spirv::Id px  = m.Op(SpvOpFAdd, f32, {x0, m.Op(SpvOpFMul, f32, {x, m.Op(SpvOpFSub, f32, {x1, x0})})});
        const spirv::Id py  = m.Op(SpvOpFAdd, f32, {y0, m.Op(SpvOpFMul, f32, {y, m.Op(SpvOpFSub, f32, {y1, y0})})});
        m.OpVoid(SpvOpStore, {pos, m.Op(SpvOpCompositeConstruct, vec4, {px, py, f0, f1})});
        m.EndMain();
        return m.Words();
    }

    std::vector<uint32_t> PresentFragment()
    {
        spirv::Module m(SpvExecutionModelFragment);
        const spirv::Id i32   = m.Int();
        const spirv::Id f32   = m.Float();
        const spirv::Id vec2  = m.Vector(f32, 2);
        const spirv::Id vec3  = m.Vector(f32, 3);
        const spirv::Id vec4  = m.Vector(f32, 4);
        const spirv::Id ivec3 = m.Vector(i32, 3);

        const spirv::Id image   = m.Image2D(f32);
        const spirv::Id sampled = m.SampledImage(image);
        const spirv::Id rows    = m.Array(vec4, kGammaRows, 16);
        const spirv::Id ramp    = m.Struct({rows});
        m.MemberDecorate(ramp, 0, SpvDecorationOffset, {0});
        m.Decorate(ramp, SpvDecorationBlock);

        const spirv::Id uv     = m.Variable(m.Pointer(SpvStorageClassInput, vec2), SpvStorageClassInput);
        const spirv::Id color  = m.Variable(m.Pointer(SpvStorageClassOutput, vec4), SpvStorageClassOutput);
        const spirv::Id source = m.Variable(m.Pointer(SpvStorageClassUniformConstant, sampled), SpvStorageClassUniformConstant);
        const spirv::Id gamma  = m.Variable(m.Pointer(SpvStorageClassUniform, ramp), SpvStorageClassUniform);
        m.Decorate(uv, SpvDecorationLocation, {0});
        m.Decorate(color, SpvDecorationLocation, {0});
        m.Decorate(source, SpvDecorationDescriptorSet, {0});
        m.Decorate(source, SpvDecorationBinding, {0});
        m.Decorate(gamma, SpvDecorationDescriptorSet, {0});
        m.Decorate(gamma, SpvDecorationBinding, {1});
        m.Name(source, "source");
        m.Name(gamma, "gamma");
        const spirv::Id rowPointer = m.Pointer(SpvStorageClassUniform, vec4);

        const spirv::Id zero  = m.ConstFloat(0.0f);
        const spirv::Id one   = m.ConstFloat(1.0f);
        const spirv::Id top   = m.ConstFloat(255.0f);
        const spirv::Id zero3 = m.ConstComposite(vec3, {zero, zero, zero});
        const spirv::Id one3  = m.ConstComposite(vec3, {one, one, one});
        const spirv::Id top3  = m.ConstComposite(vec3, {top, top, top});
        const spirv::Id first = m.ConstInt(0);

        m.BeginMain({uv, color});
        m.ExecutionMode(SpvExecutionModeOriginUpperLeft);
        const spirv::Id texture = m.Op(SpvOpLoad, sampled, {source});
        const spirv::Id at      = m.Op(SpvOpLoad, vec2, {uv});
        const spirv::Id c       = m.Op(SpvOpImageSampleImplicitLod, vec4, {texture, at});
        const spirv::Id rgb     = m.Op(SpvOpVectorShuffle, vec3, {c, c, 0, 1, 2});
        const spirv::Id clamped = m.Ext(vec3, GLSLstd450FClamp, {rgb, zero3, one3});
        const spirv::Id x       = m.Op(SpvOpVectorTimesScalar, vec3, {clamped, top});
        const spirv::Id lo      = m.Ext(vec3, GLSLstd450Floor, {x});
        const spirv::Id hi      = m.Ext(vec3, GLSLstd450FMin, {m.Op(SpvOpFAdd, vec3, {lo, one3}), top3});
        const spirv::Id t       = m.Op(SpvOpFSub, vec3, {x, lo});
        const spirv::Id loIndex = m.Op(SpvOpConvertFToS, ivec3, {lo});
        const spirv::Id hiIndex = m.Op(SpvOpConvertFToS, ivec3, {hi});

        spirv::Id channel[3];
        for (uint32_t k = 0; k < 3; ++k)
        {
            const spirv::Id a    = m.Op(SpvOpCompositeExtract, i32, {loIndex, k});
            const spirv::Id b    = m.Op(SpvOpCompositeExtract, i32, {hiIndex, k});
            const spirv::Id rowA = m.Op(SpvOpLoad, vec4, {m.Op(SpvOpAccessChain, rowPointer, {gamma, first, a})});
            const spirv::Id rowB = m.Op(SpvOpLoad, vec4, {m.Op(SpvOpAccessChain, rowPointer, {gamma, first, b})});
            const spirv::Id va   = m.Op(SpvOpCompositeExtract, f32, {rowA, k});
            const spirv::Id vb   = m.Op(SpvOpCompositeExtract, f32, {rowB, k});
            const spirv::Id tk   = m.Op(SpvOpCompositeExtract, f32, {t, k});
            channel[k]           = m.Ext(f32, GLSLstd450FMix, {va, vb, tk});
        }
        const spirv::Id alpha = m.Op(SpvOpCompositeExtract, f32, {c, 3});
        m.OpVoid(SpvOpStore, {color, m.Op(SpvOpCompositeConstruct, vec4, {channel[0], channel[1], channel[2], alpha})});
        m.EndMain();
        return m.Words();
    }
}
