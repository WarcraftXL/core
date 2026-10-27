// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw on a Direct3D 9 device: the programs its shapes are drawn with, compiled when the
// runtime is built (tools/shaders, d3dcompiler) into the runtime's own bytecode. The Vulkan
// renderer draws the same vertices with the same arithmetic (gx/vk/render/glsl/draw.vert), and the
// CPU's copy of it (gx/shapes/Geometry.cpp, Project) is what the tests hold both to.
//
//   DrawVs   a vertex of the stream (gx/shapes/Geometry.hpp, Vertex): a triangle's corner where it
//            is, a line's corner moved across the segment on the screen by half the line's width
//            (and along it, past an end that has a cap), the segment first cut at the near plane;
//            a ground shape's vertices pulled towards the eye, which leaves them where they are on
//            the screen and in front of the ground they lie on
//   DrawPs   the vertex's colour
//
// Constants: c240 to c245, above every register the game's vertex programs read (c235), so that
// none of the game's constants is touched.

float4 viewColumns[4] : register(c240); ///< the view-projection's columns: clip = dot([p 1], column)
float4 screen         : register(c244); ///< the viewport's width and height in pixels, half a pixel's
                                        ///< width at 1080 lines in pixels / 8, 0
float4 lift           : register(c245); ///< the pull towards the eye: a fraction of the distance, yards

struct VsIn
{
    float3 position : POSITION0;  ///< from the eye
    float3 other    : TEXCOORD0;  ///< a line's other end
    float4 color    : COLOR0;
    float4 data     : TEXCOORD1;  ///< side (0, 1, 2), end (0, 1), width in 1/8 px, flags (1 lift, 2 cap)
};

struct VsOut
{
    float4 position : POSITION;
    float4 color    : COLOR0;
};

float4 Clip(float3 p)
{
    const float4 v = float4(p, 1.0);
    return float4(dot(v, viewColumns[0]), dot(v, viewColumns[1]), dot(v, viewColumns[2]), dot(v, viewColumns[3]));
}

float3 Lifted(float3 p, float on)
{
    const float d    = length(p);
    const float pull = min(on * (lift.x * d + lift.y), 0.5 * d);
    return p * (1.0 - pull / max(d, 0.0001));
}

VsOut DrawVs(VsIn input)
{
    const float on  = fmod(input.data.w, 2.0);
    const float cap = floor(input.data.w * 0.5);
    float4      a   = Clip(Lifted(input.position, on));
    float4      b   = Clip(Lifted(input.other, on));
    const float side = input.data.x - 1.0;
    if (side != 0.0)
    {
        const float4 a0 = a;
        const float4 b0 = b;
        if (a0.z < 0.0) a = lerp(a0, b0, saturate(a0.z / (a0.z - b0.z)));
        if (b0.z < 0.0) b = lerp(b0, a0, saturate(b0.z / (b0.z - a0.z)));
        const float2 halfScreen = 0.5 * screen.xy;
        const float  wa = max(a.w, 0.000001);
        const float  wb = max(b.w, 0.000001);
        const float2 sa = a.xy / wa * halfScreen;
        const float2 sb = b.xy / wb * halfScreen;
        float2       d  = sb - sa;
        const float  len = length(d);
        d = len > 0.0001 ? d / len : float2(1.0, 0.0);
        const float  along = 1.0 - 2.0 * input.data.y;
        const float2 n     = float2(-d.y, d.x) * along;
        const float  halfWidth = input.data.z * screen.z;
        const float2 offset    = (n * side - d * cap) * halfWidth;
        a.xy += offset / halfScreen * wa;
    }
    VsOut o;
    o.position = a;
    o.color    = input.color;
    return o;
}

float4 DrawPs(float4 color : COLOR0) : COLOR0
{
    return color;
}
