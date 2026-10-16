// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The prototype temporal upscaler of NFSMW_MV (gpu/motion_vectors.h): one
// compute pass at the end of the scene, before the game's own
// post-processing reads it. A stand-in for FSR 2 with the same inputs (the
// jittered scene at the render size, its depth, motion vectors, the jitter)
// and the same basic steps, to see how good the motion vectors are:
//   - the current frame at the output pixel from the 3x3 render pixels
//     around it, each a sample at its jittered position (Gaussian weights);
//   - the motion vector of the nearest depth among them (dilated, so edges
//     move with what is in front), from the draws' own vectors, or where no
//     draw wrote one (and in camera mode everywhere) from the depth and the
//     camera's matrices;
//   - the history at the pixel's previous place (Catmull-Rom), clipped to
//     the 3x3 neighbourhood's colour box (YCoCg, mean +- sigma), blended in.
// Not FSR 2's quality (no locks, no reactive mask, no luminance-based
// rejection), but what ghosts here because of a wrong vector ghosts there
// too: the evaluation is of the vectors.
#pragma once

namespace gpu
{
    inline const char* kTaauGlsl = R"(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uColor;     // the scene, render size in its top left corner
layout(set = 0, binding = 1) uniform sampler2D uDepth;
layout(set = 0, binding = 2) uniform sampler2D uVelocity;  // guest NDC, previous - current; x >= 16384: none
layout(set = 0, binding = 3) uniform sampler2D uHistory;   // output size
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D oHistory;
layout(set = 0, binding = 5, rgba8) uniform writeonly image2D oOut;
layout(std430, set = 0, binding = 6) writeonly buffer DepthOut { float oDepth[]; };  // flags bit 6: the output's depth
layout(push_constant) uniform P
{
    mat4 camera;        // previous clip = camera * current clip (guest clip space; columns)
    vec2 jitter;        // render pixels, as the scene was shifted
    vec2 renderSize;    // the scene's render size (host pixels)
    vec2 outSize;       // the output size (host pixels)
    uint flags;         // bits 0-1 mode (0 draws, 1 camera, 2 none), 2 reset, 3 debug vectors, 4 no history clip,
                        // 5 reprojection error (the previous frame moved by the vectors against this one, x4),
                        // 6 depth at the output size into oDepth (below 1x: the game's composite reads it)
    float feedback;     // history weight
} p;

// Lanczos 2 (as FSR 2 reconstructs the current frame), |x| < 2.
float Lanczos2(float x)
{
    x = abs(x);
    if (x < 1e-4)
        return 1.0;
    if (x >= 2.0)
        return 0.0;
    float a = 3.14159265 * x;
    return sin(a) * sin(a * 0.5) / (a * a * 0.5);
}

vec3 ToYCoCg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 FromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Guest NDC of a point given in output UV (guest y points up).
vec2 UvToNdc(vec2 uv) { return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }

// The motion (previous - current, guest NDC) at render pixel `k`.
vec2 MotionAt(ivec2 k, uint mode)
{
    if (mode == 2u)
        return vec2(0.0);
    vec2 v = texelFetch(uVelocity, k, 0).xy;
    if (mode == 0u && v.x < 16384.0)
        return v;
    // The camera's: the pixel's point (without the jitter) through the
    // previous view-projection.
    float z = texelFetch(uDepth, k, 0).r;
    vec2 uv = (vec2(k) + 0.5 - p.jitter) / p.renderSize;
    vec2 ndc = UvToNdc(uv);
    vec4 prev = p.camera * vec4(ndc, z, 1.0);
    if (prev.w <= 1e-6)
        return vec2(0.0);
    return prev.xy / prev.w - ndc;
}

// Catmull-Rom history (5 bilinear taps).
vec4 History(vec2 uv)
{
    vec2 pos = uv * p.outSize;
    vec2 c = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (c - 1.0) / p.outSize, t3 = (c + 2.0) / p.outSize, t12 = (c + w2 / w12) / p.outSize;
    vec4 r = texture(uHistory, vec2(t12.x, t0.y)) * (w12.x * w0.y)
           + texture(uHistory, vec2(t0.x, t12.y)) * (w0.x * w12.y)
           + texture(uHistory, vec2(t12.x, t12.y)) * (w12.x * w12.y)
           + texture(uHistory, vec2(t3.x, t12.y)) * (w3.x * w12.y)
           + texture(uHistory, vec2(t12.x, t3.y)) * (w12.x * w3.y);
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / ws, vec4(0.0));
}

void main()
{
    ivec2 o = ivec2(gl_GlobalInvocationID.xy);
    if (o.x >= int(p.outSize.x) || o.y >= int(p.outSize.y))
        return;
    uint mode = p.flags & 3u;
    vec2 uv = (vec2(o) + 0.5) / p.outSize;
    // The render pixel whose (jittered) sample is nearest.
    vec2 rp = uv * p.renderSize + p.jitter;
    ivec2 rc = ivec2(floor(rp));
    ivec2 rmax = ivec2(ceil(p.renderSize)) - 1;
    if ((p.flags & 64u) != 0u)
        oDepth[o.y * int(p.outSize.x) + o.x] = texelFetch(uDepth, clamp(rc, ivec2(0), rmax), 0).r;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    vec4 m1 = vec4(0.0), m2 = vec4(0.0);
    vec4 cmin = vec4(1e9), cmax = vec4(-1e9);
    float wnear = 0.0;
    float nearest = 2.0;
    ivec2 nearestAt = clamp(rc, ivec2(0), rmax);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            ivec2 k = clamp(rc + ivec2(x, y), ivec2(0), rmax);
            vec4 c = texelFetch(uColor, k, 0);
            vec4 cy = vec4(ToYCoCg(c.rgb), c.a);
            // Distance from the output pixel's centre to the sample, in render pixels.
            vec2 d = vec2(k) + 0.5 - rp;
            float w = Lanczos2(d.x) * Lanczos2(d.y);
            sum += c * w;
            wsum += w;
            wnear = max(wnear, w);
            cmin = min(cmin, c);
            cmax = max(cmax, c);
            m1 += cy;
            m2 += cy * cy;
            float z = texelFetch(uDepth, k, 0).r;
            if (z < nearest)
            {
                nearest = z;
                nearestAt = k;
            }
        }
    // (Lanczos rings: kept inside the samples' range.)
    vec4 current = clamp(sum / max(wsum, 1e-5), cmin, cmax);
    vec2 motion = MotionAt(nearestAt, mode);
    if ((p.flags & 8u) != 0u)
    {
        // Debug: the vectors (pixels x 8, red right, green down; blue: none from a draw).
        vec2 v = texelFetch(uVelocity, nearestAt, 0).xy;
        vec2 px = vec2(motion.x, -motion.y) * 0.5 * p.outSize;
        vec4 dbg = vec4(0.5 + px * (8.0 / 255.0), v.x >= 16384.0 ? 1.0 : 0.0, 1.0);
        imageStore(oOut, o, dbg);
        imageStore(oHistory, o, current);
        return;
    }
    vec2 prevUv = uv + vec2(motion.x, -motion.y) * 0.5;
    bool reset = (p.flags & 4u) != 0u || any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0)));
    if ((p.flags & 32u) != 0u)
    {
        // The vectors' own error: last frame's picture (kept as drawn, not
        // accumulated) at the pixel's previous place against this frame's
        // pixel. Where the previous place is off screen: none (black).
        vec4 c = texelFetch(uColor, clamp(rc, ivec2(0), rmax), 0);
        vec4 e = reset ? vec4(0.0) : abs(History(prevUv) - c);
        imageStore(oHistory, o, c);
        imageStore(oOut, o, vec4(clamp(e.rgb * 4.0, 0.0, 1.0), 1.0));
        return;
    }
    vec4 result = current;
    if (!reset)
    {
        vec4 h = History(prevUv);
        vec4 hy = vec4(ToYCoCg(h.rgb), h.a);
        if ((p.flags & 16u) == 0u)
        {
            vec4 mean = m1 / 9.0;
            vec4 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec4(0.0)));
            vec4 lo = mean - sigma * 1.25, hi = mean + sigma * 1.25;
            // Clip towards the box's centre (not a per-channel clamp).
            vec4 dir = hy - mean;
            vec4 ext = (hi - lo) * 0.5 + 1e-4;
            vec4 t = abs(dir) / ext;
            float m = max(max(t.x, t.y), max(t.z, t.w));
            if (m > 1.0)
                hy = mean + dir / m;
        }
        vec4 hc = vec4(FromYCoCg(hy.rgb), hy.a);
        // The current frame's weight: more where a sample lies near the
        // pixel (an upscale: one between samples learns slower) and where
        // the picture moves fast (each resample of the history blurs it).
        float speed = length(vec2(motion.x, -motion.y) * 0.5 * p.outSize);
        float a = p.feedback <= 0.0 ? 1.0 : clamp((1.0 - p.feedback) * (0.5 + wnear) + speed * 0.01, 0.03, 0.6);
        result = mix(hc, current, a);
    }
    result = clamp(result, vec4(0.0), vec4(1.0));
    imageStore(oHistory, o, result);
    imageStore(oOut, o, result);
}
)";
}
