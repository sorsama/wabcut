#version 440

// Mask passes (I.6, render/Mask.h). The CPU twin is render::maskCoverageCpu (src/render/src/Mask.cpp): change both
// together (tests/render/test_masks.cpp compares them).
//
//   SDF      target = a signed-distance texture over the frame plus a margin (`domain`, normalised frame coordinates):
//            the distance (frame heights, + inside by the even-odd rule) to a polygon whose segments are in `segs`
//            (one texel per segment: x0, y0, x1, y1 in aspect space, i.e. x times the frame aspect).
//   COMPOSE  target = the coverage over the frame: up to 4 shapes per pass, each its distance texture sampled through
//            the shape's inverse transform, then feather (smoothstep), expansion, invert, opacity and the
//            add / subtract / intersect combination; mode.w = 0 start empty, 1 start full, 2 continue from `cov`.
//   MIX      canvas space: tex0 (the effect's or redaction's result) mixed into tex1 (its input) by the coverage.
//   ALPHA    canvas space: tex0 times the coverage (opacity masks).
//   MOSAIC   canvas space: tex0 averaged over cells of style.x canvas texels (its mip at log2(cell)).
//   SOLID    canvas space: the colour style (linear, premultiplied, opaque).

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pos;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform MaskParams {
    mat4 cornerToClip;
    vec4 texel;       // target: 1 / size, size
    vec4 toFrame0;    // canvas sequence px -> layer frame px (MIX, ALPHA)
    vec4 toFrame1;
    vec4 frame;       // xy = layer frame size (px), zw = canvas size (sequence px)
    ivec4 mode;       // x = mode, y = 1 when textures are stored bottom-up, z = segments / shapes, w = start
    vec4 domain;      // this target's domain (SDF, COMPOSE): origin, size (normalised frame coordinates)
    vec4 sdfDomain;   // the distance textures' domain (COMPOSE)
    vec4 style;       // MOSAIC: x = cell (canvas texels), y = lod; SOLID: the colour
    vec4 aspect;      // x = frame aspect (w / h), y = one coverage texel (frame heights)
    vec4 affine0[4];  // COMPOSE: per shape, frame -> shape: (a, b, c), w = distance scale
    vec4 affine1[4];  //          (d, e, f)
    vec4 edge[4];     //          feather, expansion, opacity, inverted
    ivec4 combine[4]; //          x = 0 add, 1 subtract, 2 intersect
} u;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;
layout(binding = 3) uniform sampler2D cov;
layout(binding = 4) uniform sampler2D sdf0;
layout(binding = 5) uniform sampler2D sdf1;
layout(binding = 6) uniform sampler2D sdf2;
layout(binding = 7) uniform sampler2D sdf3;
layout(binding = 8) uniform sampler2D segs;

const int MODE_SDF = 0;
const int MODE_COMPOSE = 1;
const int MODE_MIX = 2;
const int MODE_ALPHA = 3;
const int MODE_MOSAIC = 4;
const int MODE_SOLID = 5;

// A top-left-origin texture coordinate as the texture stores it.
vec2 stored(vec2 topDown)
{
    return u.mode.y == 1 ? vec2(topDown.x, 1.0 - topDown.y) : topDown;
}

float signedDistance(vec2 p)
{
    vec2 q = vec2(p.x * u.aspect.x, p.y);
    float best = 1e30;
    bool inside = false;
    int n = u.mode.z;
    for (int i = 0; i < n; ++i) {
        vec4 s = texelFetch(segs, ivec2(i, 0), 0);
        vec2 a = s.xy;
        vec2 d = s.zw - s.xy;
        float len2 = dot(d, d);
        float t = len2 > 0.0 ? clamp(dot(q - a, d) / len2, 0.0, 1.0) : 0.0;
        vec2 e = q - (a + t * d);
        best = min(best, dot(e, e));
        if ((s.y > q.y) != (s.w > q.y) && q.x < s.x + (q.y - s.y) * d.x / d.y)
            inside = !inside;
    }
    float dist = sqrt(best);
    return inside ? dist : -dist;
}

float sdfAt(int k, vec2 q)
{
    vec2 uv = stored((q - u.sdfDomain.xy) / u.sdfDomain.zw);
    if (k == 0)
        return texture(sdf0, uv).r;
    if (k == 1)
        return texture(sdf1, uv).r;
    if (k == 2)
        return texture(sdf2, uv).r;
    return texture(sdf3, uv).r;
}

float compose()
{
    vec2 p = u.domain.xy + v_pos * u.domain.zw;
    float m = u.mode.w == 2 ? texture(cov, v_uv).r : float(u.mode.w);
    float aa = u.aspect.y;
    for (int k = 0; k < 4; ++k) {
        if (k >= u.mode.z)
            break;
        vec2 q = vec2(dot(u.affine0[k].xyz, vec3(p, 1.0)), dot(u.affine1[k].xyz, vec3(p, 1.0)));
        // affine1.w flags a shape that covers nothing (kMaskEmptyDistance): no distance texture is read for it.
        float d = u.affine1[k].w > 0.5 ? -1.0e4 : sdfAt(k, q) * u.affine0[k].w + u.edge[k].y;
        float f = u.edge[k].x;
        float a = f > aa ? smoothstep(-0.5 * f, 0.5 * f, d) : clamp(0.5 + d / aa, 0.0, 1.0);
        a = u.edge[k].w > 0.5 ? 1.0 - a : a;
        a *= clamp(u.edge[k].z, 0.0, 1.0);
        int c = u.combine[k].x;
        if (c == 1)
            m *= 1.0 - a;
        else if (c == 2)
            m *= a;
        else
            m = m + a - m * a;
    }
    return m;
}

// The coverage at this canvas point (zero outside the layer's frame).
float coverageHere()
{
    vec3 px = vec3(v_pos * u.frame.zw, 1.0);
    vec2 f = vec2(dot(u.toFrame0.xyz, px), dot(u.toFrame1.xyz, px)) / u.frame.xy;
    if (f.x < 0.0 || f.y < 0.0 || f.x > 1.0 || f.y > 1.0)
        return 0.0;
    return texture(cov, stored(f)).r;
}

void main()
{
    int mode = u.mode.x;
    if (mode == MODE_SDF) {
        float d = signedDistance(u.domain.xy + v_pos * u.domain.zw);
        fragColor = vec4(d, d, d, 1.0);
    } else if (mode == MODE_COMPOSE) {
        float m = compose();
        fragColor = vec4(m, m, m, 1.0);
    } else if (mode == MODE_MIX) {
        fragColor = mix(texture(tex1, v_uv), texture(tex0, v_uv), coverageHere());
    } else if (mode == MODE_ALPHA) {
        fragColor = texture(tex0, v_uv) * coverageHere();
    } else if (mode == MODE_MOSAIC) {
        float cell = max(u.style.x, 1.0);
        vec2 texelPos = v_pos * u.texel.zw;
        vec2 centre = (floor(texelPos / cell) + 0.5) * cell;
        fragColor = textureLod(tex0, stored(centre * u.texel.xy), u.style.y);
    } else {
        fragColor = u.style;
    }
}
