#version 440

// Effect passes on canvas-size intermediates: linear Rec.709, premultiplied alpha, RGBA16F. One shader, one pass per
// mode (mode.x). Every formula here has a CPU twin in tests/render/EffectsReference.cpp, which the golden-image tests
// (tests/render/test_effects.cpp) compare against: change both together. Transitions are fx_transition.frag (0.2); the
// effect library's families are fx_*.frag, with the same uniform block.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pos;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform FxParams {
    mat4 cornerToClip;
    vec4 texel;    // xy = 1 / texture size, zw = texture size (canvas texture pixels)
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 toFrame0; // canvas sequence px -> layer frame px: x' = dot(xyz, (x, y, 1))
    vec4 toFrame1; //                                       y' = dot(xyz, (x, y, 1))
    vec4 frame;    // xy = layer frame size (source px), zw = canvas size (sequence px)
    ivec4 mode;    // x = mode, y = 1 when textures are stored bottom-up
    vec4 p3;       // 0.2: more parameters (the family shaders, fx_*.frag)
    vec4 p4;
    vec4 p5;
    vec4 p6;
    vec4 p7;
} p;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;

const int MODE_COPY = 0;
const int MODE_CROP = 1;
const int MODE_BLUR = 2;
const int MODE_CORRECTION = 3;
const int MODE_VIGNETTE = 4;
const int MODE_SHARPEN = 5;
const int MODE_SHADOW = 6;
const int MODE_OVER = 7;
const int MODE_BORDER = 9; // 8 was the transition mix: fx_transition.frag since 0.2
const int MODE_KEY_MATTE = 10;
const int MODE_KEY_CHOKE = 11;
const int MODE_KEY_APPLY = 12;
const int MODE_BLEND = 13;
const int MODE_TRACK_MATTE = 14;

const int MAX_TAPS = 96;

vec4 fetch0(vec2 uv, float lod, bool repeatEdges)
{
    if (!repeatEdges && (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0))
        return vec4(0.0);
    return textureLod(tex0, uv, lod);
}

// Canvas-space offset (y down) in texture coordinates.
vec2 toUv(vec2 canvasTexels)
{
    vec2 d = canvasTexels * p.texel.xy;
    return p.mode.y == 1 ? vec2(d.x, -d.y) : d;
}

// Separable Gaussian: p0.xy = direction (canvas texels), p0.z = sigma (texels at lod 0), p0.w = lod,
// p1.x = repeat edges.
vec4 blur()
{
    float lod = p.p0.w;
    float stepScale = exp2(lod);
    float sigma = p.p0.z / stepScale;
    bool repeatEdges = p.p1.x > 0.5;
    if (sigma < 0.05)
        return fetch0(v_uv, 0.0, true);
    int taps = min(int(ceil(3.0 * sigma)), MAX_TAPS);
    vec2 du = toUv(p.p0.xy * stepScale);
    vec4 acc = fetch0(v_uv, lod, repeatEdges);
    float total = 1.0;
    for (int i = 1; i <= MAX_TAPS; ++i) {
        if (i > taps)
            break;
        float x = float(i);
        float w = exp(-0.5 * x * x / (sigma * sigma));
        acc += w * (fetch0(v_uv + x * du, lod, repeatEdges) + fetch0(v_uv - x * du, lod, repeatEdges));
        total += 2.0 * w;
    }
    return acc / total;
}

vec2 framePoint()
{
    vec3 px = vec3(v_pos * p.frame.zw, 1.0);
    return vec2(dot(p.toFrame0.xyz, px), dot(p.toFrame1.xyz, px));
}

// p0 = left, top, right, bottom (fractions), p1.x = feather (sequence px), p1.y/p1.z = sequence px per frame
// px along x/y, p1.w = sequence px per canvas texel (preview divisor).
vec4 crop(vec4 c)
{
    vec2 src = framePoint();
    vec2 lo = p.p0.xy * p.frame.xy;
    vec2 hi = p.frame.xy - p.p0.zw * p.frame.xy;
    vec2 inside = min(src - lo, hi - src) * p.p1.yz; // sequence px
    float d = min(inside.x, inside.y);
    float mask = p.p1.x > 0.0 ? clamp(d / p.p1.x, 0.0, 1.0) : clamp(d / p.p1.w + 0.5, 0.0, 1.0);
    return c * mask;
}

// p0 = exposure (stops), contrast (-100..100), saturation (0..200), temperature (-100..100); p1.x = tint.
vec4 correction(vec4 c)
{
    if (c.a <= 0.0)
        return c;
    vec3 rgb = max(c.rgb / c.a, vec3(0.0));
    rgb *= exp2(p.p0.x);
    float t = p.p0.w / 100.0;
    float g = p.p1.x / 100.0;
    rgb *= vec3(1.0 + 0.25 * t, 1.0, 1.0 - 0.25 * t) * vec3(1.0 + 0.1 * g, 1.0 - 0.2 * g, 1.0 + 0.1 * g);
    const float pivot = 0.18;
    float k = 1.0 + p.p0.y / 100.0;
    rgb = pivot * pow(max(rgb, vec3(0.0)) / pivot, vec3(k));
    float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = max(vec3(y) + (rgb - vec3(y)) * (p.p0.z / 100.0), vec3(0.0));
    return vec4(rgb * c.a, c.a);
}

// p0 = amount (stops at the edge), midpoint, roundness, feather (0..100).
vec4 vignette(vec4 c)
{
    vec2 q = framePoint() / p.frame.xy * 2.0 - 1.0;
    float roundness = p.p0.z / 100.0;
    float aspect = p.frame.x / max(p.frame.y, 1.0);
    q.x *= mix(1.0, aspect, max(roundness, 0.0));
    float n = 2.0 + 6.0 * max(-roundness, 0.0);
    vec2 a = abs(q);
    float d = pow(pow(a.x, n) + pow(a.y, n), 1.0 / n);
    float mid = 0.3 + 0.9 * p.p0.y / 100.0;
    float width = max(0.01, p.p0.w / 100.0);
    float v = smoothstep(mid - 0.5 * width, mid + 0.5 * width, d);
    return vec4(c.rgb * exp2(p.p0.x * v), c.a);
}

// ---- Ultra Key (C.15 wabcut.key.ultra): CPU twin render/reference/UltraKey.cpp, the formulas in UltraKey.h ----

// The BT.709 OETF and its inverse on [0, 1] (render/reference Image.cpp toEncoded / toLinear).
float encode709(float l)
{
    l = clamp(l, 0.0, 1.0);
    return l < 0.018053968510807 ? 4.5 * l : 1.09929682680944 * pow(l, 0.45) - 0.09929682680944;
}

float decode709(float v)
{
    v = clamp(v, 0.0, 1.0);
    return v < 4.5 * 0.018053968510807 ? v / 4.5 : pow((v + 0.09929682680944) / 1.09929682680944, 1.0 / 0.45);
}

// The straight encoded colour of a linear premultiplied pixel (0 where it is transparent).
vec3 keyEncoded(vec4 c)
{
    float a = clamp(c.a, 0.0, 1.0);
    if (a <= 0.0)
        return vec3(0.0);
    return vec3(encode709(c.r / a), encode709(c.g / a), encode709(c.b / a));
}

float lumaOf(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 toYCbCr(vec3 e)
{
    float y = lumaOf(e);
    return vec3(y, (e.b - y) / 1.8556, (e.r - y) / 1.5748);
}

vec3 fromYCbCr(vec3 q)
{
    float r = q.x + 1.5748 * q.z;
    float b = q.x + 1.8556 * q.y;
    return vec3(r, (q.x - 0.2126 * r - 0.0722 * b) / 0.7152, b);
}

// x = chroma distance to the key (0 at the key, 1 at grey), y = luma difference.
vec2 keyDistance(vec3 e, vec3 key)
{
    vec3 q = toYCbCr(e);
    vec3 k = toYCbCr(key);
    return vec2(length(q.yz - k.yz) / max(length(k.yz), 1e-6), q.x - k.x);
}

// p0 = key colour (encoded), transparency; p1 = highlight, shadow, tolerance, pedestal.
float keyMatte(vec3 e)
{
    vec2 k = keyDistance(e, p.p0.rgb);
    float t = p.p1.z / 100.0 * 0.5;
    float ped = p.p1.w / 100.0 * 0.25;
    float w = 0.25 * (1.0 - p.p0.w / 100.0) + 0.02;
    float chroma = smoothstep(t, t + w, k.x - ped);
    float a = chroma + p.p1.x / 100.0 * max(0.0, k.y) * (1.0 - chroma);
    a = a * (1.0 - p.p1.y / 100.0 * max(0.0, -k.y) * (1.0 - chroma));
    return clamp(a, 0.0, 1.0);
}

// The matte (in .a) eroded by a disc of p0.x texels (p0.y = its floor), inside the picture.
float keyChoke()
{
    float rc = p.p0.x;
    int reach = int(p.p0.y);
    float lowest = 1.0;
    for (int j = 0; j <= 2 * MAX_TAPS; ++j) {
        if (j > 2 * reach)
            break;
        for (int i = 0; i <= 2 * MAX_TAPS; ++i) {
            if (i > 2 * reach)
                break;
            vec2 d = vec2(float(i - reach), float(j - reach));
            vec2 uv = v_uv + toUv(d);
            if (dot(d, d) <= rc * rc && uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0)
                lowest = min(lowest, textureLod(tex0, uv, 0.0).a);
        }
    }
    return lowest;
}

// Spill suppression and colour correction of an encoded colour. p0 = key colour; p1.xy = tolerance, pedestal;
// p2 = desaturate, range, spill, luma; toFrame0 = saturation, hue (radians), luminance, key channel.
vec3 keyColor(vec3 e)
{
    vec2 k = keyDistance(e, p.p0.rgb);
    float start = p.p1.x / 100.0 * 0.5 + p.p1.y / 100.0 * 0.25;
    float s = p.p2.z / 100.0 * (1.0 - smoothstep(start, start + p.p2.y / 100.0 * 0.5 + 0.05, k.x));
    int kc = int(p.toFrame0.w + 0.5);
    vec3 c = e;
    float yOriginal = lumaOf(c);
    float v = kc == 0 ? c.r : (kc == 1 ? c.g : c.b);
    float limit = kc == 0 ? (c.g + c.b) * 0.5 : (kc == 1 ? (c.b + c.r) * 0.5 : (c.r + c.g) * 0.5);
    v = v - s * max(0.0, v - limit);
    if (kc == 0)
        c.r = v;
    else if (kc == 1)
        c.g = v;
    else
        c.b = v;
    float yCut = lumaOf(c);
    c = c + p.p2.x / 100.0 * s * (vec3(yCut) - c);
    c = c + vec3((p.p2.w / 100.0 - 0.5) * s * (yOriginal - yCut));
    vec3 q = toYCbCr(c);
    q.x *= p.toFrame0.z / 100.0;
    float h = p.toFrame0.y;
    float sat = p.toFrame0.x / 100.0;
    vec2 cbcr = vec2(q.y * cos(h) - q.z * sin(h), q.y * sin(h) + q.z * cos(h)) * sat;
    return clamp(fromYCbCr(vec3(q.x, cbcr)), 0.0, 1.0);
}

// tex0 = the layer, tex1 = the cleaned matte (in .a); p0.w = output (0 composite, 1 alpha, 2 colour);
// p1.zw = contrast, mid point.
vec4 keyApply(vec4 c)
{
    float aIn = clamp(c.a, 0.0, 1.0);
    float mid = p.p1.w / 100.0;
    float m = clamp((textureLod(tex1, v_uv, 0.0).a - mid) * (1.0 + p.p1.z / 25.0) + mid, 0.0, 1.0);
    float a = aIn * m;
    int outMode = int(p.p0.w + 0.5);
    if (outMode == 1) {
        float g = decode709(a);
        return vec4(g, g, g, 1.0);
    }
    vec3 k = keyColor(keyEncoded(c));
    vec3 lin = vec3(decode709(k.r), decode709(k.g), decode709(k.b));
    return outMode == 2 ? vec4(lin, 1.0) : vec4(lin * a, a);
}

// ---- Blend modes (C.15, 0.1.2): CPU twin render/reference/Blend.cpp, docs/internal/c15-parity-handoff.md "Blend modes" ----
// The model's BlendMode values (model/Blend.h).
const int BM_NORMAL = 0;
const int BM_MULTIPLY = 1;
const int BM_SCREEN = 2;
const int BM_OVERLAY = 3;
const int BM_ADD = 4;
const int BM_DARKEN = 5;
const int BM_LIGHTEN = 6;
const int BM_DIFFERENCE = 7;
const int BM_DISSOLVE = 8;
const int BM_COLOR_BURN = 9;
const int BM_LINEAR_BURN = 10;
const int BM_DARKER_COLOR = 11;
const int BM_COLOR_DODGE = 12;
const int BM_LIGHTER_COLOR = 13;
const int BM_SOFT_LIGHT = 14;
const int BM_HARD_LIGHT = 15;
const int BM_VIVID_LIGHT = 16;
const int BM_LINEAR_LIGHT = 17;
const int BM_PIN_LIGHT = 18;
const int BM_HARD_MIX = 19;
const int BM_EXCLUSION = 20;
const int BM_SUBTRACT = 21;
const int BM_DIVIDE = 22;
const int BM_HUE = 23;
const int BM_SATURATION = 24;
const int BM_COLOR = 25;
const int BM_LUMINOSITY = 26;

bool blendClampsInputs(int m)
{
    return m == BM_COLOR_BURN || m == BM_LINEAR_BURN || m == BM_SCREEN || m == BM_COLOR_DODGE || m == BM_OVERLAY
        || m == BM_SOFT_LIGHT || m == BM_HARD_LIGHT || m == BM_VIVID_LIGHT || m == BM_LINEAR_LIGHT || m == BM_PIN_LIGHT
        || m == BM_HARD_MIX || m == BM_EXCLUSION || m >= BM_HUE;
}

float screenOf(float b, float s) { return b + s - b * s; }

float colorBurnOf(float b, float s)
{
    if (b >= 1.0)
        return 1.0;
    if (s <= 0.0)
        return 0.0;
    return 1.0 - min(1.0, (1.0 - b) / s);
}

float colorDodgeOf(float b, float s)
{
    if (b <= 0.0)
        return 0.0;
    if (s >= 1.0)
        return 1.0;
    return min(1.0, b / (1.0 - s));
}

float hardLightOf(float b, float s) { return s <= 0.5 ? b * (2.0 * s) : screenOf(b, 2.0 * s - 1.0); }

float separable(int m, float b, float s)
{
    if (m == BM_DARKEN)
        return min(b, s);
    if (m == BM_MULTIPLY)
        return b * s;
    if (m == BM_COLOR_BURN)
        return colorBurnOf(b, s);
    if (m == BM_LINEAR_BURN)
        return max(0.0, b + s - 1.0);
    if (m == BM_LIGHTEN)
        return max(b, s);
    if (m == BM_SCREEN)
        return screenOf(b, s);
    if (m == BM_COLOR_DODGE)
        return colorDodgeOf(b, s);
    if (m == BM_ADD)
        return b + s;
    if (m == BM_OVERLAY)
        return hardLightOf(s, b);
    if (m == BM_SOFT_LIGHT) {
        if (s <= 0.5)
            return b - (1.0 - 2.0 * s) * b * (1.0 - b);
        float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
        return b + (2.0 * s - 1.0) * (d - b);
    }
    if (m == BM_HARD_LIGHT)
        return hardLightOf(b, s);
    if (m == BM_VIVID_LIGHT)
        return s <= 0.5 ? colorBurnOf(b, 2.0 * s) : colorDodgeOf(b, 2.0 * s - 1.0);
    if (m == BM_LINEAR_LIGHT)
        return clamp(b + 2.0 * s - 1.0, 0.0, 1.0);
    if (m == BM_PIN_LIGHT)
        return s <= 0.5 ? min(b, 2.0 * s) : max(b, 2.0 * s - 1.0);
    if (m == BM_HARD_MIX)
        return b + s >= 1.0 ? 1.0 : 0.0;
    if (m == BM_DIFFERENCE)
        return abs(b - s);
    if (m == BM_EXCLUSION)
        return b + s - 2.0 * b * s;
    if (m == BM_SUBTRACT)
        return max(0.0, b - s);
    if (m == BM_DIVIDE) {
        if (s == 0.0)
            return b == 0.0 ? 0.0 : 1.0;
        return min(1.0, b / s);
    }
    return s; // Normal, Dissolve
}

// W3C non-separable helpers (Rec. 709 weights).
vec3 clipColor(vec3 c)
{
    float l = lumaOf(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0 && l - n > 0.0)
        c = vec3(l) + (c - vec3(l)) * l / (l - n);
    if (x > 1.0 && x - l > 0.0)
        c = vec3(l) + (c - vec3(l)) * (1.0 - l) / (x - l);
    return c;
}

vec3 setLum(vec3 c, float l)
{
    float d = l - lumaOf(c);
    return clipColor(c + vec3(d));
}

float satOf(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }

vec3 setSat(vec3 c, float s)
{
    float v[3] = float[3](c.r, c.g, c.b);
    int imax = 0;
    int imin = 0;
    for (int i = 1; i < 3; ++i) {
        if (v[i] > v[imax])
            imax = i;
        if (v[i] < v[imin])
            imin = i;
    }
    float o[3] = float[3](0.0, 0.0, 0.0);
    if (imax == imin)
        return vec3(0.0); // every channel equal: no saturation to keep
    int imid = 3 - imax - imin;
    float range = v[imax] - v[imin];
    o[imid] = (v[imid] - v[imin]) * s / range;
    o[imax] = s;
    return vec3(o[0], o[1], o[2]);
}

vec3 blendFunction(int m, vec3 cb, vec3 cs)
{
    vec3 b = cb;
    vec3 s = cs;
    if (blendClampsInputs(m)) {
        b = clamp(b, 0.0, 1.0);
        s = clamp(s, 0.0, 1.0);
    }
    if (m == BM_DARKER_COLOR)
        return lumaOf(s) < lumaOf(b) ? s : b;
    if (m == BM_LIGHTER_COLOR)
        return lumaOf(s) > lumaOf(b) ? s : b;
    if (m == BM_HUE)
        return setLum(setSat(s, satOf(b)), lumaOf(b));
    if (m == BM_SATURATION)
        return setLum(setSat(b, satOf(s)), lumaOf(b));
    if (m == BM_COLOR)
        return setLum(s, lumaOf(b));
    if (m == BM_LUMINOSITY)
        return setLum(b, lumaOf(s));
    return vec3(separable(m, b.r, s.r), separable(m, b.g, s.g), separable(m, b.b, s.b));
}

// 64-bit unsigned arithmetic on (hi, lo) pairs of 32-bit words, for Dissolve's splitmix64.
uvec2 add64(uvec2 a, uvec2 b)
{
    uint lo = a.y + b.y;
    return uvec2(a.x + b.x + (lo < a.y ? 1u : 0u), lo);
}

uvec2 mul32(uint a, uint b) // the full 64-bit product
{
    uint a0 = a & 0xFFFFu;
    uint a1 = a >> 16;
    uint b0 = b & 0xFFFFu;
    uint b1 = b >> 16;
    uint p00 = a0 * b0;
    uint p01 = a0 * b1;
    uint p10 = a1 * b0;
    uint p11 = a1 * b1;
    uint mid = p01 + p10;
    uint midCarry = mid < p01 ? 0x10000u : 0u;
    uint lo = p00 + (mid << 16);
    uint loCarry = lo < p00 ? 1u : 0u;
    return uvec2(p11 + (mid >> 16) + midCarry + loCarry, lo);
}

uvec2 mul64(uvec2 a, uvec2 b) // the low 64 bits
{
    uvec2 low = mul32(a.y, b.y);
    return uvec2(low.x + a.x * b.y + a.y * b.x, low.y);
}

uvec2 xorShift64(uvec2 a, int k) // a ^ (a >> k), 0 < k < 32
{
    return uvec2(a.x ^ (a.x >> uint(k)), a.y ^ ((a.y >> uint(k)) | (a.x << uint(32 - k))));
}

// splitmix64(seed, n) >> 11 as a value in [0, 1): its top 24 bits (render/reference Blend.cpp dissolveValue).
float dissolveValue(uvec2 seed, uvec2 n)
{
    uvec2 z = add64(seed, mul64(add64(n, uvec2(0u, 1u)), uvec2(0x9E3779B9u, 0x7F4A7C15u)));
    z = mul64(xorShift64(z, 30), uvec2(0xBF58476Du, 0x1CE4E5B9u));
    z = mul64(xorShift64(z, 27), uvec2(0x94D049BBu, 0x133111EBu));
    z = xorShift64(z, 31);
    return float(z.x >> 8) * (1.0 / 16777216.0);
}

// tex0 = the layer (premultiplied, its opacity included), tex1 = the canvas below it. mode.z = the blend mode;
// Dissolve: p1 = the seed (clip id) as four 16-bit words from the high one, p2.xy = the frame's low 32 bits as two.
vec4 blendOver()
{
    vec4 s = textureLod(tex0, v_uv, 0.0);
    vec4 b = textureLod(tex1, v_uv, 0.0);
    float sa = clamp(s.a, 0.0, 1.0);
    float ba = clamp(b.a, 0.0, 1.0);
    vec3 cs = sa > 0.0 ? s.rgb / sa : vec3(0.0);
    vec3 cb = ba > 0.0 ? b.rgb / ba : vec3(0.0);
    int m = p.mode.z;
    if (m == BM_DISSOLVE) {
        uvec2 seed = uvec2((uint(p.p1.x) << 16) | uint(p.p1.y), (uint(p.p1.z) << 16) | uint(p.p1.w));
        uvec2 xy = uvec2(v_pos * p.texel.zw);
        uvec2 n = uvec2((uint(p.p2.x) << 16) | uint(p.p2.y), xy.y * uint(p.texel.z) + xy.x);
        return dissolveValue(seed, n) < sa ? vec4(cs, 1.0) : b;
    }
    vec3 cr = (1.0 - ba) * cs + ba * blendFunction(m, cb, cs);
    return vec4(sa * cr + (1.0 - sa) * b.rgb, sa + ba * (1.0 - sa));
}

void main()
{
    int mode = p.mode.x;
    if (mode == MODE_BLUR) {
        fragColor = blur();
        return;
    }
    vec4 c = textureLod(tex0, v_uv, 0.0);
    if (mode == MODE_CROP) {
        fragColor = crop(c);
    } else if (mode == MODE_CORRECTION) {
        fragColor = correction(c);
    } else if (mode == MODE_VIGNETTE) {
        fragColor = vignette(c);
    } else if (mode == MODE_SHARPEN) {
        // tex0 = original, tex1 = blurred; p0.x = amount (1 = 100 %).
        vec4 b = textureLod(tex1, v_uv, 0.0);
        fragColor = vec4(max(c.rgb + p.p0.x * (c.rgb - b.rgb), vec3(0.0)), c.a);
    } else if (mode == MODE_SHADOW) {
        // The layer's alpha moved by p0.xy (canvas texels), tinted p1.rgb (linear) at opacity p0.z.
        float a = textureLod(tex0, v_uv - toUv(p.p0.xy), 0.0).a;
        vec2 shifted = v_uv - toUv(p.p0.xy);
        if (shifted.x < 0.0 || shifted.y < 0.0 || shifted.x > 1.0 || shifted.y > 1.0)
            a = 0.0;
        float k = a * p.p0.z;
        fragColor = vec4(p.p1.rgb * k, k);
    } else if (mode == MODE_BORDER) {
        // C.15 Border: the layer over its alpha dilated by p0.x canvas texels (a disc: rings at the full and half width,
        // 64 directions each), in p1.rgb (linear) at opacity p0.y. CPU twin: EffectsReference.cpp.
        float r = p.p0.x;
        float a = c.a;
        for (int i = 0; i < 64; ++i) {
            float angle = float(i) * (6.28318530718 / 64.0);
            vec2 d = vec2(cos(angle), sin(angle)) * r;
            a = max(a, textureLod(tex0, v_uv + toUv(d), 0.0).a);
            a = max(a, textureLod(tex0, v_uv + toUv(0.5 * d), 0.0).a);
        }
        float k = clamp(a, 0.0, 1.0) * p.p0.y;
        fragColor = c + vec4(p.p1.rgb * k, k) * (1.0 - c.a);
    } else if (mode == MODE_KEY_MATTE) {
        fragColor = vec4(keyMatte(keyEncoded(c)));
    } else if (mode == MODE_KEY_CHOKE) {
        fragColor = vec4(keyChoke());
    } else if (mode == MODE_KEY_APPLY) {
        fragColor = keyApply(c);
    } else if (mode == MODE_BLEND) {
        fragColor = blendOver();
    } else if (mode == MODE_TRACK_MATTE) {
        // C.15 Track Matte Key: tex1 = the matte track's composite on this canvas (p0.w = 0: none, transparent black);
        // p0 = luma (1) or alpha (0), reverse, the matte track is valid (0: the layer shows nothing).
        vec4 matte = p.p0.w > 0.5 ? textureLod(tex1, v_uv, 0.0) : vec4(0.0);
        float m = p.p0.x > 0.5 ? clamp(lumaOf(matte.rgb), 0.0, 1.0) : clamp(matte.a, 0.0, 1.0);
        if (p.p0.y > 0.5)
            m = 1.0 - m;
        fragColor = p.p0.z > 0.5 ? c * m : vec4(0.0);
    } else if (mode == MODE_OVER) {
        // tex0 over tex1.
        vec4 under = textureLod(tex1, v_uv, 0.0);
        fragColor = c + under * (1.0 - c.a);
    } else {
        fragColor = c;
    }
}
