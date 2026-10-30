// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight on a Direct3D 9 device: the programs of its passes, compiled when the runtime is
// built (tools/shaders, d3dcompiler) into the runtime's own bytecode.
//
//   MaskPs3, MaskPs2   a highlighted batch drawn again with the game's own vertex program, into the
//                      two mask targets: its colour and coverage, its glow and its outline width.
//                      The 3.0 program pairs with a vs_3_0 vertex program, the 2.0 one with older
//                      ones (Direct3D 9 does not mix 3.0 with earlier models)
//   FullscreenVs       a quad over the whole target, moved by half a pixel to Direct3D 9's centres
//   BlurPs             nine taps of a Gaussian along one direction: the glow, downsampled then blurred
//   CompositePs        the outline found around the mask, over the blurred glow, onto the scene
//
// Constants: the pixel programs read c200 and up (ps_3_0) or c29 and up (ps_2_0), which no program
// of the game reads (its pixel programs stop at c14), and the vertex program c250, above every
// register the game's vertex programs read (c235): the game's constants are never touched by the
// full-screen passes.

// --- The masks ----------------------------------------------------------------------------------

struct MaskOut
{
    float4 color : COLOR0; ///< rgb the colour, a 1: covered
    float4 extra : COLOR1; ///< rgb the glow (colour times strength), a the outline width / 32 px
};

// The batch's first texture and its alpha test, as the game drew it: a texel under the reference
// (hair, a mane, leaves, a cloak's cut) is no part of the silhouette. The reference is below 0 for
// a batch drawn without the test, which then keeps every texel.
sampler2D maskTexture : register(s0);

float4 maskColor3 : register(c200);
float4 maskExtra3 : register(c201);
float4 maskAlpha3 : register(c202); ///< x: the alpha test's reference, 0 to 1, or -1

MaskOut MaskPs3(float2 uv : TEXCOORD0)
{
    clip(tex2D(maskTexture, uv).a - maskAlpha3.x);
    MaskOut o;
    o.color = maskColor3;
    o.extra = maskExtra3;
    return o;
}

float4 maskAlpha2 : register(c29);
float4 maskColor2 : register(c30);
float4 maskExtra2 : register(c31);

MaskOut MaskPs2(float2 uv : TEXCOORD0)
{
    clip(tex2D(maskTexture, uv).a - maskAlpha2.x);
    MaskOut o;
    o.color = maskColor2;
    o.extra = maskExtra2;
    return o;
}

// --- The full-screen quad -----------------------------------------------------------------------

float4 halfPixel : register(c250); ///< xy: -1 / width, 1 / height, in clip space

struct QuadOut
{
    float4 position : POSITION;
    float2 uv       : TEXCOORD0;
};

QuadOut FullscreenVs(float4 position : POSITION, float2 uv : TEXCOORD0)
{
    QuadOut o;
    o.position    = position;
    o.position.xy = position.xy + halfPixel.xy * position.w;
    o.uv          = uv;
    return o;
}

// --- The glow's blur ----------------------------------------------------------------------------

sampler2D blurSource : register(s0);
float4    blurStep   : register(c200); ///< xy: one tap's step in the source's coordinates

float4 BlurPs(float2 uv : TEXCOORD0) : COLOR0
{
    static const float kWeights[5] = {0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162};
    float4 sum = tex2D(blurSource, uv) * kWeights[0];
    [unroll] for (int i = 1; i < 5; ++i)
    {
        const float2 offset = blurStep.xy * i;
        sum += tex2D(blurSource, uv + offset) * kWeights[i];
        sum += tex2D(blurSource, uv - offset) * kWeights[i];
    }
    return sum;
}

// --- The outline and the glow, onto the scene ---------------------------------------------------

sampler2D maskColor : register(s0); ///< the colour mask, resolved: rgb times coverage, a coverage
sampler2D maskExtra : register(s1); ///< the other mask, resolved: rgb the glow, a the width code, times coverage
sampler2D glow      : register(s2); ///< the other mask blurred at half the size: rgb the glow, a above 0 near an object
float4    screen    : register(c200); ///< xy: one pixel in uv; z: the widest outline this frame, in pixels; w: 1 with a glow
float4    glowGain  : register(c201); ///< x: how strong the glow is overall

static const float kWidthCode = 32.0; ///< the mask's width code is the width over this many pixels

// One sample of the search: the covered pixel at `distance` along `direction`, kept when its own
// object's outline reaches this far and it reaches further than the best so far.
void Tap(float2 uv, float2 direction, float distance, inout float best, inout float3 color)
{
    const float2 at       = uv + direction * distance * screen.xy;
    const float4 sample   = tex2Dgrad(maskColor, at, float2(0.0, 0.0), float2(0.0, 0.0));
    const float  coverage = max(sample.a, 0.004);
    const float  width    = tex2Dgrad(maskExtra, at, float2(0.0, 0.0), float2(0.0, 0.0)).a / coverage * kWidthCode;
    const float  cover    = saturate(width - distance + 1.0) * step(0.004, sample.a);
    const float  take     = step(best + 0.0001, cover);
    best  = lerp(best, cover, take);
    color = lerp(color, sample.rgb / coverage, take);
}

// Twelve directions around a pixel, written out: the compilers this is built with do not all
// unroll nested loops.
#define WXL_RING(distance)                                                    \
    Tap(uv, float2(1.0, 0.0), distance, best, color);                         \
    Tap(uv, float2(0.8660254, 0.5), distance, best, color);                   \
    Tap(uv, float2(0.5, 0.8660254), distance, best, color);                   \
    Tap(uv, float2(0.0, 1.0), distance, best, color);                         \
    Tap(uv, float2(-0.5, 0.8660254), distance, best, color);                  \
    Tap(uv, float2(-0.8660254, 0.5), distance, best, color);                  \
    Tap(uv, float2(-1.0, 0.0), distance, best, color);                        \
    Tap(uv, float2(-0.8660254, -0.5), distance, best, color);                 \
    Tap(uv, float2(-0.5, -0.8660254), distance, best, color);                 \
    Tap(uv, float2(0.0, -1.0), distance, best, color);                        \
    Tap(uv, float2(0.5, -0.8660254), distance, best, color);                  \
    Tap(uv, float2(0.8660254, -0.5), distance, best, color);

float4 CompositePs(float2 uv : TEXCOORD0) : COLOR0
{
    const float  inside = tex2D(maskColor, uv).a;
    const float4 near   = tex2D(glow, uv); // rgb the glow; a above 0 near a highlighted object

    // The outline: the nearest covered pixel within its own object's width, searched on three
    // rings out to the widest outline asked this frame. Its end is one pixel soft. Searched only
    // near an object (the blurred mask reaches past the widest outline): elsewhere a pixel costs
    // two reads.
    float  best  = 0.0;
    float3 color = float3(0.0, 0.0, 0.0);
    if (near.a > 0.0)
    {
        WXL_RING(screen.z / 3.0)
        WXL_RING(screen.z * 2.0 / 3.0)
        WXL_RING(screen.z)
    }
    const float outline = best * (1.0 - inside);

    // The glow: light added around the object, never over it.
    const float3 light = near.rgb * glowGain.x * screen.w * (1.0 - inside);

    // Blended as one, inverse source alpha: the outline covers, the glow adds.
    return float4(color * outline + light * (1.0 - outline), outline);
}
