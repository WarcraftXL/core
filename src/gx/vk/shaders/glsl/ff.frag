#version 450
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// D3D9's fixed-function pixel processing for stages 0 and 1, as the engine sets them (its
// s_colorOp/s_colorArgs). Adapted from the legacy mod's ff.frag. CURRENT starts as the diffuse
// colour; a stage with a texture combines its texel with CURRENT by its colour op (rgb) and its
// alpha op (a), saturated; a stage without one ends the cascade (D3D's DISABLE, ops -1). Then the
// alpha test, then the vertex fog on rgb.
//
//   0 Mod       MODULATE (TEXTURE, CURRENT)           t * c
//   1 Mod2x     MODULATE2X                            2 * t * c
//   2 Add       ADD                                   t + c
//   3 PassThru  SELECTARG2                            c
//   4 Decal     BLENDCURRENTALPHA (CURRENT, TEXTURE)  c * c.a + t * (1 - c.a)
//   5 Fade      BLENDDIFFUSEALPHA (TEXTURE, CURRENT)  t * d.a + c * (1 - d.a)

#extension GL_GOOGLE_include_directive : require
#include "engine.glsl"
#include "interface.frag.glsl"

/// A stage's 2D coordinate: divided by its last counted component when its transform is PROJECTED.
vec2 StageUV(vec4 t, int flags)
{
    if ((flags & 0x100) != 0)
    {
        int   count = flags & 0xFF;
        float w     = count == 3 ? t.z : (count == 4 ? t.w : 0.0);
        if (w != 0.0) return t.xy / w;
    }
    return t.xy;
}

vec4 Op(int op, vec4 t, vec4 c, float diffuseAlpha)
{
    if (op == 0) return t * c;
    if (op == 1) return 2.0 * t * c;
    if (op == 2) return t + c;
    if (op == 3) return c;
    if (op == 4) return c * c.a + t * (1.0 - c.a);
    if (op == 5) return t * diffuseAlpha + c * (1.0 - diffuseAlpha);
    return c;
}

void main()
{
    DrawData d   = DrawData(pc.data);
    vec4     cur = vColor;
    if (d.ops.x >= 0)
    {
        vec4 t = Sample0(StageUV(vTex0, d.texgen.y));
        cur    = clamp(vec4(Op(d.ops.x, t, cur, vColor.a).rgb, Op(d.ops.y, t, cur, vColor.a).a), 0.0, 1.0);
        if (d.ops.z >= 0)
        {
            vec4 t1 = Sample1(StageUV(vTex1, d.texgen.w));
            cur     = clamp(vec4(Op(d.ops.z, t1, cur, vColor.a).rgb, Op(d.ops.w, t1, cur, vColor.a).a), 0.0, 1.0);
        }
    }
    if (AlphaRejected(cur.a)) discard;
    cur.rgb  = mix(d.fogColor.rgb, cur.rgb, vFog);
    outColor = cur;
}
