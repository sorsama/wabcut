#version 440

// The effect library's Generate family (0.2, render/EffectLibrary.h): pictures drawn over or instead of a linear
// premultiplied RGBA16F layer (gradients, shapes, a lens flare), the Transition bin's effects (the layer wiped away by
// Transition Completion), and the effects that read a second picture (tex1, LayerEffect::matte: echoes of the clip, text,
// a difference layer, a matte image; mode.w = 1 when tex1 holds it). Positions are in the layer's frame (through toFrame).
// mode.z = the op, p0..p7 = LayerEffect::params[0..31]. CPU twins: render/reference/LibraryGenerate.cpp.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pos;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform FxParams {
    mat4 cornerToClip;
    vec4 texel;    // xy = 1 / texture size, zw = texture size
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 toFrame0; // canvas sequence px -> layer frame px
    vec4 toFrame1;
    vec4 frame;    // xy = layer frame size, zw = canvas size (sequence px)
    ivec4 mode;    // y = textures stored bottom-up, z = the op, w = 1: tex1 holds the second picture
    vec4 p3;
    vec4 p4;
    vec4 p5;
    vec4 p6;
    vec4 p7;
} p;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;

const float PI = 3.14159265358979;

const int OP_FOUR_COLOR_GRADIENT = 100;
const int OP_RAMP = 101;
const int OP_CIRCLE = 102;
const int OP_GRID = 103;
const int OP_CHECKERBOARD = 104;
const int OP_LENS_FLARE = 105;
const int OP_BLOCK_DISSOLVE = 106;
const int OP_GRADIENT_WIPE = 107;
const int OP_LINEAR_WIPE = 108;
const int OP_RADIAL_WIPE = 109;
const int OP_VENETIAN_BLINDS = 110;
const int OP_ECHO = 111;
const int OP_TEXT_OVERLAY = 112;
const int OP_DIFFERENCE_MATTE = 113;
const int OP_IMAGE_MATTE_KEY = 114;

float q[32];

void loadParams()
{
    q[0] = p.p0.x; q[1] = p.p0.y; q[2] = p.p0.z; q[3] = p.p0.w;
    q[4] = p.p1.x; q[5] = p.p1.y; q[6] = p.p1.z; q[7] = p.p1.w;
    q[8] = p.p2.x; q[9] = p.p2.y; q[10] = p.p2.z; q[11] = p.p2.w;
    q[12] = p.p3.x; q[13] = p.p3.y; q[14] = p.p3.z; q[15] = p.p3.w;
    q[16] = p.p4.x; q[17] = p.p4.y; q[18] = p.p4.z; q[19] = p.p4.w;
    q[20] = p.p5.x; q[21] = p.p5.y; q[22] = p.p5.z; q[23] = p.p5.w;
    q[24] = p.p6.x; q[25] = p.p6.y; q[26] = p.p6.z; q[27] = p.p6.w;
    q[28] = p.p7.x; q[29] = p.p7.y; q[30] = p.p7.z; q[31] = p.p7.w;
}

vec2 canvasPos()
{
    vec2 fc = gl_FragCoord.xy;
    if (p.mode.y == 1)
        fc.y = p.texel.w - fc.y;
    return fc * p.frame.zw / p.texel.zw;
}

vec2 toFrame(vec2 c)
{
    vec3 v = vec3(c, 1.0);
    return vec2(dot(p.toFrame0.xyz, v), dot(p.toFrame1.xyz, v));
}

const float A709 = 1.09929682680944;
const float B709 = 0.018053968510807;

float oetf(float l)
{
    l = clamp(l, 0.0, 1.0);
    return l < B709 ? 4.5 * l : A709 * pow(l, 0.45) - (A709 - 1.0);
}

float eotf(float v)
{
    v = clamp(v, 0.0, 1.0);
    return v < 4.5 * B709 ? v / 4.5 : pow((v + A709 - 1.0) / A709, 1.0 / 0.45);
}

vec3 eotf3(vec3 v) { return vec3(eotf(v.r), eotf(v.g), eotf(v.b)); }

vec3 encoded(vec4 c)
{
    float a = clamp(c.a, 0.0, 1.0);
    return a > 0.0 ? vec3(oetf(c.r / a), oetf(c.g / a), oetf(c.b / a)) : vec3(0.0);
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float srgbToLinear(float v)
{
    v = clamp(v, 0.0, 1.0);
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

vec3 srgbToLinear3(vec3 c) { return vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)); }

uint hashU(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash01(uint a, uint b, uint c)
{
    uint h = hashU(a + hashU(b + hashU(c)));
    return float(h >> 8) * (1.0 / 16777216.0);
}

float cwAngle(vec2 u, vec2 d)
{
    float y = u.x * d.y - u.y * d.x;
    float x = dot(u, d);
    return x == 0.0 && y == 0.0 ? 0.0 : atan(y, x);
}

// ---- Blending (Magnify's list, plus Stencil Alpha 9 and Silhouette Alpha 10 for the shapes) ----

float blendChannel(int mode, float b, float s)
{
    if (mode == 3)
        return b * s;
    if (mode == 4)
        return b + s - b * s;
    if (mode == 5)
        return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    if (mode == 6)
        return min(b, s);
    if (mode == 7)
        return max(b, s);
    if (mode == 8)
        return abs(b - s);
    return s;
}

vec4 blendOver(int mode, vec4 dst, vec4 src)
{
    if (mode == 0)
        return src;
    if (mode == 2)
        return vec4(dst.rgb + src.rgb, min(1.0, dst.a + src.a));
    float sa = clamp(src.a, 0.0, 1.0);
    float ba = clamp(dst.a, 0.0, 1.0);
    vec3 cs = sa > 0.0 ? clamp(src.rgb / sa, 0.0, 1.0) : vec3(0.0);
    vec3 cb = ba > 0.0 ? clamp(dst.rgb / ba, 0.0, 1.0) : vec3(0.0);
    vec3 bf = vec3(blendChannel(mode, cb.r, cs.r), blendChannel(mode, cb.g, cs.g), blendChannel(mode, cb.b, cs.b));
    vec3 cr = (1.0 - ba) * cs + ba * bf;
    return vec4(sa * cr + (1.0 - sa) * dst.rgb, sa + ba * (1.0 - sa));
}

// A shape of colour `color` (linear) and coverage k drawn with a blending mode: None draws only the shape, Stencil and
// Silhouette Alpha cut the layer by it.
vec4 drawShape(int mode, vec4 dst, vec3 color, float k)
{
    if (mode == 0)
        return vec4(color * k, k);
    if (mode == 9)
        return dst * k;
    if (mode == 10)
        return dst * (1.0 - k);
    return blendOver(mode, dst, vec4(color * k, k));
}

// The cell size of Grid and Checkerboard: from the corner point, the width, or the width and height.
vec2 cellSize(float sizeFrom, vec2 anchor, vec2 corner, float width, float height)
{
    int from = int(sizeFrom + 0.5);
    vec2 c = from == 0 ? abs(corner - anchor) : (from == 1 ? vec2(width) : vec2(width, height));
    return max(c, vec2(1.0));
}

// ---- Lens Flare ----

vec3 lensFlare(vec2 f, vec2 s, vec2 center, float brightness, int lens)
{
    float diag = length(s);
    vec2 d = f - center;
    float r = length(d);
    float glow = 0.9 * exp(-r * r / (2.0 * (0.05 * diag) * (0.05 * diag))) + 0.3 * exp(-r / (0.12 * diag));
    float streak = 0.4 * exp(-abs(d.y) / (0.004 * diag)) * exp(-abs(d.x) / (0.3 * diag));
    vec3 color = vec3(1.0, 0.92, 0.8) * (glow + streak);
    // Ghosts along the line through the frame's centre, by lens.
    vec2 axis = 0.5 * s - center;
    for (int k = 0; k < 6; ++k) {
        float fk = float(k);
        float t = lens == 1 ? 0.35 + 0.33 * fk : (lens == 2 ? 0.6 + 0.22 * fk : 0.25 + 0.3 * fk);
        float radius = diag * (lens == 2 ? 0.02 + 0.012 * fk : 0.03 + 0.018 * mod(fk * 3.0, 5.0));
        vec3 tint = k % 3 == 0 ? vec3(0.3, 0.6, 1.0) : (k % 3 == 1 ? vec3(0.4, 1.0, 0.5) : vec3(1.0, 0.5, 0.3));
        float g = length(f - (center + axis * t));
        color += tint * 0.12 * (1.0 - smoothstep(0.75 * radius, radius, g));
    }
    return color * brightness / 100.0;
}

// ---- The Transition bin's fields ----

float wipeKeep(float field, float feather, float completion)
{
    // field > 0 where the layer is wiped away; shifted so completion 0 keeps everything and 1 nothing.
    float w = max(feather, 1.0);
    return 1.0 - clamp((field + 0.5 * w * (2.0 * completion - 1.0)) / w + 0.5, 0.0, 1.0);
}

void main()
{
    loadParams();
    int op = p.mode.z;
    vec2 c = canvasPos();
    vec2 f = toFrame(c);
    vec2 s = p.frame.xy;
    vec4 in0 = textureLod(tex0, v_uv, 0.0);
    bool second = p.mode.w == 1;
    uvec2 px = uvec2(gl_FragCoord.xy);
    if (op == OP_FOUR_COLOR_GRADIENT) {
        float diag = length(s);
        float power = 200.0 / max(q[20], 1.0);
        vec3 sum = vec3(0.0);
        float total = 0.0;
        for (int i = 0; i < 4; ++i) {
            int o = 5 * i;
            vec2 point = vec2(q[o], q[o + 1]) * s;
            vec3 color = vec3(q[o + 2], q[o + 3], q[o + 4]);
            float w = 1.0 / (pow(length(f - point) / diag, power) + 1e-6);
            sum += color * w;
            total += w;
        }
        vec3 g = sum / total + (hash01(px.x + 65536u * px.y, 3u, 0u) - 0.5) * q[21] * 0.2;
        vec4 gradient = vec4(eotf3(clamp(g, 0.0, 1.0)), 1.0);
        int mode = int(q[23] + 0.5);
        fragColor = mode == 0 ? mix(in0, gradient, q[22]) : blendOver(mode, in0, gradient * q[22]);
    } else if (op == OP_RAMP) {
        vec2 start = vec2(q[0], q[1]) * s;
        vec2 end = vec2(q[5], q[6]) * s;
        vec2 axis = end - start;
        float t = q[10] > 0.5 ? length(f - start) / max(length(axis), 1e-6) : dot(f - start, axis) / max(dot(axis, axis), 1e-6);
        vec3 color = mix(vec3(q[2], q[3], q[4]), vec3(q[7], q[8], q[9]), clamp(t, 0.0, 1.0));
        color += (hash01(px.x + 65536u * px.y, 5u, 0u) - 0.5) * q[11] / 255.0;
        fragColor = mix(vec4(eotf3(clamp(color, 0.0, 1.0)), 1.0), in0, q[12]);
    } else if (op == OP_CIRCLE) {
        vec2 center = vec2(q[0], q[1]) * s;
        float d = length(f - center);
        float r = q[2];
        int edge = int(q[3] + 0.5);
        float t = q[4];
        float fo = q[5];
        float fi = q[6];
        float inner = edge == 1 ? t : (edge == 2 ? r - t : r - t * r / 100.0);
        if (edge == 4) {
            fo *= r / 100.0;
            fi *= r / 100.0;
        }
        float k = fo > 0.0 ? clamp((r - d) / fo + 0.5, 0.0, 1.0) : clamp(r - d + 0.5, 0.0, 1.0);
        if (edge != 0)
            k *= fi > 0.0 ? clamp((d - inner) / fi + 0.5, 0.0, 1.0) : clamp(d - inner + 0.5, 0.0, 1.0);
        if (q[7] > 0.5)
            k = 1.0 - k;
        fragColor = drawShape(int(q[12] + 0.5), in0, srgbToLinear3(vec3(q[8], q[9], q[10])), k * q[11]);
    } else if (op == OP_GRID) {
        vec2 anchor = vec2(q[0], q[1]) * s;
        vec2 cell = cellSize(q[2], anchor, vec2(q[3], q[4]) * s, q[5], q[6]);
        vec2 local = mod(f - anchor, cell);
        vec2 dist = min(local, cell - local);
        float b = 0.5 * q[7];
        float k = 0.0;
        if (q[7] > 0.0) {
            float kx = clamp((b - dist.x) / max(q[8], 1.0) + 0.5, 0.0, 1.0);
            float ky = clamp((b - dist.y) / max(q[9], 1.0) + 0.5, 0.0, 1.0);
            k = max(kx, ky);
        }
        if (q[10] > 0.5)
            k = 1.0 - k;
        fragColor = drawShape(int(q[15] + 0.5), in0, srgbToLinear3(vec3(q[11], q[12], q[13])), k * q[14]);
    } else if (op == OP_CHECKERBOARD) {
        vec2 anchor = vec2(q[0], q[1]) * s;
        vec2 cell = cellSize(q[2], anchor, vec2(q[3], q[4]) * s, q[5], q[6]);
        vec2 ij = floor((f - anchor) / cell);
        float k = mod(ij.x + ij.y, 2.0) == 0.0 ? 1.0 : 0.0;
        fragColor = drawShape(int(q[13] + 0.5), in0, srgbToLinear3(vec3(q[9], q[10], q[11])), k * q[12]);
    } else if (op == OP_LENS_FLARE) {
        vec3 flare = lensFlare(f, s, vec2(q[0], q[1]) * s, q[2], int(q[3] + 0.5));
        vec4 lit = vec4(in0.rgb + flare, max(in0.a, clamp(max(flare.r, max(flare.g, flare.b)), 0.0, 1.0)));
        fragColor = mix(lit, in0, q[4]);
    } else if (op == OP_BLOCK_DISSOLVE) {
        vec2 cell = max(vec2(q[1], q[2]), vec2(1.0));
        vec2 ij = floor(f / cell);
        float r = hash01(uint(int(ij.x)), uint(int(ij.y)), 9u);
        fragColor = r >= q[0] ? in0 : vec4(0.0);
    } else if (op == OP_GRADIENT_WIPE) {
        // The gradient: the second picture's luma (Gradient Layer), else the layer's own.
        vec4 gsrc = second ? textureLod(tex1, v_uv, 0.0) : in0;
        float g = luma(encoded(gsrc));
        if (q[4] > 0.5)
            g = 1.0 - g;
        float soft = q[1];
        float keep = soft > 0.0 ? clamp((g - q[0] * (1.0 + soft)) / soft + 1.0, 0.0, 1.0) : (g >= q[0] ? 1.0 : 0.0);
        fragColor = in0 * keep;
    } else if (op == OP_LINEAR_WIPE) {
        float a = radians(q[1]);
        vec2 v = vec2(sin(a), -cos(a));
        float s0 = min(0.0, v.x * s.x) + min(0.0, v.y * s.y);
        float s1 = max(0.0, v.x * s.x) + max(0.0, v.y * s.y);
        float field = s0 + q[0] * (s1 - s0) - dot(f, v);
        fragColor = in0 * wipeKeep(field, q[2], q[0]);
    } else if (op == OP_RADIAL_WIPE) {
        float a0 = radians(q[1]);
        vec2 start = vec2(sin(a0), -cos(a0));
        vec2 center = vec2(q[2], q[3]) * s;
        vec2 d = f - center;
        float r = length(d);
        float a = cwAngle(start, d);
        int wipe = int(q[4] + 0.5);
        float field;
        if (wipe == 2) {
            field = r * (q[0] * PI - abs(a));
        } else {
            if (wipe == 1)
                a = -a;
            float sweep = q[0] * 2.0 * PI;
            field = r * (sweep <= PI ? min(a, sweep - a) : max(a, sweep - 2.0 * PI - a));
        }
        fragColor = in0 * wipeKeep(field, q[5], q[0]);
    } else if (op == OP_VENETIAN_BLINDS) {
        float a = radians(q[1]);
        vec2 v = vec2(-sin(a), cos(a));
        float width = max(q[2], 1.0);
        float along = dot(f, v);
        float local = along - floor(along / width) * width;
        fragColor = in0 * wipeKeep(q[0] * width - local, q[3], q[0]);
    } else if (op == OP_ECHO) {
        // tex1 = the echoes (the evaluator composites them by the operator); the current frame at Starting Intensity.
        vec4 now = in0 * q[2];
        vec4 echoes = second ? textureLod(tex1, v_uv, 0.0) : vec4(0.0);
        int eop = int(q[4] + 0.5);
        if (eop == 0 || eop == 6)
            fragColor = vec4(now.rgb + echoes.rgb, min(1.0, now.a + echoes.a));
        else if (eop == 1)
            fragColor = max(now, echoes);
        else if (eop == 2)
            fragColor = min(now, echoes);
        else if (eop == 3)
            fragColor = now + echoes - now * echoes;
        else if (eop == 4)
            fragColor = now + echoes * (1.0 - now.a);
        else
            fragColor = echoes + now * (1.0 - echoes.a);
    } else if (op == OP_TEXT_OVERLAY) {
        vec4 text = second ? textureLod(tex1, v_uv, 0.0) : vec4(0.0);
        fragColor = text + in0 * (1.0 - text.a);
    } else if (op == OP_DIFFERENCE_MATTE && !second) {
        fragColor = in0; // no Difference Layer: nothing keyed
    } else if (op == OP_DIFFERENCE_MATTE) {
        vec4 other = second ? textureLod(tex1, v_uv, 0.0) : vec4(0.0);
        vec3 e = encoded(in0);
        vec3 o = encoded(other);
        float d = max(abs(e.r - o.r), max(abs(e.g - o.g), abs(e.b - o.b)));
        float m = smoothstep(q[3], q[3] + q[4] + 1e-5, d);
        int view = int(q[0] + 0.5);
        fragColor = view == 1 ? in0 : (view == 2 ? vec4(vec3(eotf(m)), 1.0) : in0 * m);
    } else if (op == OP_IMAGE_MATTE_KEY && !second) {
        fragColor = in0; // no matte image chosen: nothing keyed
    } else if (op == OP_IMAGE_MATTE_KEY) {
        vec4 matte = second ? textureLod(tex1, v_uv, 0.0) : vec4(0.0);
        float m = q[0] > 0.5 ? clamp(luma(encoded(matte)) * clamp(matte.a, 0.0, 1.0), 0.0, 1.0) : clamp(matte.a, 0.0, 1.0);
        if (q[1] > 0.5)
            m = 1.0 - m;
        fragColor = in0 * m;
    } else {
        fragColor = in0;
    }
}
