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
//            and moved on the screen by the pixels its bytes say (a shadow's)
//   DrawPs   the vertex's colour, a line's edges faded over its softness
//   SurfacePs a ground shape's inside: the picture (its frame from the shape's plane), the feather
//            towards its border and the fade from its centre, as gx/shapes/Geometry.cpp's Shade
//
// Constants: c240 to c245 for the vertex program, above every register the game's vertex programs
// read (c235), so that none of the game's constants is touched; c200 to c205 for SurfacePs, above
// what the game's pixel programs read (as wxl.highlight's).

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

float4 surface[6]     : register(c200); ///< SurfacePs: gx/shapes/Geometry.hpp, SurfaceConstants
sampler2D picture     : register(s0);

struct VsOut
{
    float4 position : POSITION;
    float4 color    : COLOR0;
    float3 line     : TEXCOORD0; ///< a line's: pixels across from its middle, half its drawn width, its softness
    float3 plane    : TEXCOORD1; ///< a surface's: its plane's x and y, a polygon's distance to its border
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
    const float on    = fmod(input.data.w, 2.0);
    const float cap   = fmod(floor(input.data.w * 0.5), 2.0);
    const float soft  = floor(input.data.w * 0.25);
    float       down  = floor(input.data.x * 0.25);
    float       right = floor(input.data.y * 0.5);
    down  = down >= 32.0 ? down - 64.0 : down;
    right = right >= 32.0 ? right - 64.0 : right;
    float4      a    = Clip(Lifted(input.position, on));
    float4      b    = Clip(Lifted(input.other, on));
    const float side = fmod(input.data.x, 4.0) - 1.0;
    float3      line = float3(0.0, 1.0, 0.0);
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
        const float  along = 1.0 - 2.0 * fmod(input.data.y, 2.0);
        const float2 n     = float2(-d.y, d.x) * along;
        const float  halfWidth = input.data.z * screen.z + 2.0 * soft * screen.z;
        const float2 offset    = (n * side - d * cap) * halfWidth;
        a.xy += offset / halfScreen * wa;
        line = float3(side * halfWidth, halfWidth, soft * 4.0 * screen.z);
    }
    // A shadow's move on the screen, in half pixels at 1080 lines (y down).
    a.xy += float2(right, -down) * 8.0 * screen.z / (0.5 * screen.xy) * a.w;
    VsOut o;
    o.position = a;
    o.color    = input.color;
    o.line     = line;
    o.plane    = input.other;
    return o;
}

float4 DrawPs(float4 color : COLOR0, float3 line : TEXCOORD0) : COLOR0
{
    const float cover = line.z > 0.0 ? saturate((line.y - abs(line.x)) / line.z) : 1.0;
    return float4(color.rgb, color.a * cover);
}

float Smooth(float t)
{
    return t * t * (3.0 - 2.0 * t);
}

float4 SurfacePs(float4 color : COLOR0, float3 plane : TEXCOORD1) : COLOR0
{
    const float x = plane.x;
    const float y = plane.y;
    const float r = sqrt(x * x + y * y);
    float       d;
    if (surface[3].x < 0.5)
    {
        d = surface[3].y - r;
        if (surface[3].z > 0.0) d = min(d, r - surface[3].z);
        if (surface[4].x < 3.14159265)
        {
            const float diff = surface[4].x - atan2(abs(y), x);
            d = min(d, diff < 1.57079633 ? r * sin(diff) : r);
        }
    }
    else if (surface[3].x < 1.5)
        d = min(surface[4].y - abs(x), surface[4].z - abs(y));
    else
        d = plane.z;
    const float edge = surface[3].w > 0.0 ? Smooth(saturate(d / surface[3].w)) : 1.0;
    float       fade = 1.0;
    if (surface[4].w < 1.5) fade = 1.0 - Smooth(saturate((r * surface[5].x - surface[4].w) / max(1.0 - surface[4].w, 0.0001)));
    // The derivatives first, where every pixel of the quad runs.
    const float2 uv    = float2(dot(float3(x, y, 1.0), surface[0].xyz), dot(float3(x, y, 1.0), surface[1].xyz));
    const float2 scale = surface[2].xy;
    const float2 gx    = ddx(uv * scale);
    const float2 gy    = ddy(uv * scale);
    float4       texel = float4(1.0, 1.0, 1.0, 1.0);
    if (surface[2].w > 0.5)
    {
        const float2 at = surface[2].z > 0.5 ? frac(uv) * scale : saturate(uv) * scale;
        texel = tex2Dgrad(picture, at, gx, gy);
    }
    const float  a = color.a * texel.a * edge * fade;
    const float3 c = color.rgb * texel.rgb;
    return surface[5].y > 0.5 ? float4(1.0 + (c - 1.0) * a, a) : float4(c, a);
}
