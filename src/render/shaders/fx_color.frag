#version 440

// The effect library's Color family (0.2, render/EffectLibrary.h): pixel operations on the straight, BT.709-encoded colour
// of a linear premultiplied RGBA16F layer (the working space's encoding, as Premiere's 8-bit effects see the picture),
// back to linear premultiplied. mode.z = the op (render::LibraryOp), p0..p7 = LayerEffect::params[0..31]. Every op has a
// CPU twin in render/reference/LibraryEffects.cpp: change both together.

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
    ivec4 mode;    // y = textures stored bottom-up, z = the op
    vec4 p3;
    vec4 p4;
    vec4 p5;
    vec4 p6;
    vec4 p7;
} p;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;

// render::LibraryOp
const int OP_BRIGHTNESS_CONTRAST = 1;
const int OP_LEVELS = 2;
const int OP_PROC_AMP = 3;
const int OP_EXTRACT = 4;
const int OP_INVERT = 5;
const int OP_TINT = 6;
const int OP_CHANGE_COLOR = 7;
const int OP_CHANGE_TO_COLOR = 8;
const int OP_LEAVE_COLOR = 9;
const int OP_COLOR_BALANCE = 10;
const int OP_VIDEO_LIMITER = 11;
const int OP_BLACK_WHITE = 12;
const int OP_COLOR_PASS = 13;
const int OP_COLOR_REPLACE = 14;
const int OP_GAMMA_CORRECTION = 15;
const int OP_POSTERIZE = 16;
const int OP_SOLARIZE = 17;
const int OP_THRESHOLD = 18;
const int OP_STROBE_LIGHT = 19;
const int OP_ALPHA_ADJUST = 20;
const int OP_LUMA_KEY = 21;
const int OP_NOISE = 22;

float q[32];

void loadParams()
{
    vec4 v[8] = vec4[8](p.p0, p.p1, p.p2, p.p3, p.p4, p.p5, p.p6, p.p7);
    for (int i = 0; i < 8; ++i) {
        q[4 * i] = v[i].x;
        q[4 * i + 1] = v[i].y;
        q[4 * i + 2] = v[i].z;
        q[4 * i + 3] = v[i].w;
    }
}

// ---- Encoding (render/reference Image.h toEncoded / toLinear; the extended forms above 1 for the Video Limiter) ----

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

float oetfExt(float l) { return l <= 1.0 ? oetf(l) : A709 * pow(l, 0.45) - (A709 - 1.0); }
float eotfExt(float v) { return v <= 1.0 ? eotf(v) : pow((v + A709 - 1.0) / A709, 1.0 / 0.45); }

vec3 oetf3(vec3 l) { return vec3(oetf(l.r), oetf(l.g), oetf(l.b)); }
vec3 eotf3(vec3 v) { return vec3(eotf(v.r), eotf(v.g), eotf(v.b)); }

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// ---- HLS (h in turns 0..1) ----

vec3 toHls(vec3 c)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float l = 0.5 * (mx + mn);
    float d = mx - mn;
    if (d <= 0.0)
        return vec3(0.0, l, 0.0);
    float s = l < 0.5 ? d / (mx + mn) : d / (2.0 - mx - mn);
    float h;
    if (mx == c.r)
        h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    else if (mx == c.g)
        h = (c.b - c.r) / d + 2.0;
    else
        h = (c.r - c.g) / d + 4.0;
    return vec3(h / 6.0, l, s);
}

float hueChannel(float m1, float m2, float h)
{
    h = h - floor(h);
    if (h < 1.0 / 6.0)
        return m1 + (m2 - m1) * 6.0 * h;
    if (h < 0.5)
        return m2;
    if (h < 2.0 / 3.0)
        return m1 + (m2 - m1) * (2.0 / 3.0 - h) * 6.0;
    return m1;
}

vec3 fromHls(vec3 hls)
{
    float l = hls.y;
    float s = hls.z;
    if (s <= 0.0)
        return vec3(l);
    float m2 = l <= 0.5 ? l * (1.0 + s) : l + s - l * s;
    float m1 = 2.0 * l - m2;
    return vec3(hueChannel(m1, m2, hls.x + 1.0 / 3.0), hueChannel(m1, m2, hls.x), hueChannel(m1, m2, hls.x - 1.0 / 3.0));
}

// The shortest signed hue difference from a to b (turns, -0.5..0.5).
float hueDelta(float a, float b)
{
    float d = b - a;
    return d - floor(d + 0.5);
}

vec2 chroma(vec3 c)
{
    float y = luma(c);
    return vec2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
}

// Distance of a colour to a key in RGB (0), by hue (1) or by chroma (2), about 0..1.
float colorDistance(vec3 c, vec3 k, int match)
{
    if (match == 1) {
        float d = abs(hueDelta(toHls(c).x, toHls(k).x));
        return 2.0 * d;
    }
    if (match == 2)
        return length(chroma(c) - chroma(k));
    return length(c - k) / 1.7320508;
}

// W3C SetLum with ClipColor (Rec. 709 luma), as the blend modes.
vec3 clipColor(vec3 c)
{
    float l = luma(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0 && l - n > 0.0)
        c = vec3(l) + (c - vec3(l)) * l / (l - n);
    if (x > 1.0 && x - l > 0.0)
        c = vec3(l) + (c - vec3(l)) * (1.0 - l) / (x - l);
    return c;
}

vec3 setLum(vec3 c, float l) { return clipColor(c + vec3(l - luma(c))); }

// ---- Hash (the transitions' integer hash) ----

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

// ---- Ops ----

float levelsOf(float v, float ib, float iw, float ob, float ow, float gamma)
{
    ib /= 255.0;
    iw /= 255.0;
    ob /= 255.0;
    ow /= 255.0;
    float x = iw > ib ? clamp((v - ib) / (iw - ib), 0.0, 1.0) : (v >= ib ? 1.0 : 0.0);
    return ob + (ow - ob) * pow(x, 100.0 / max(gamma, 1.0));
}

vec2 framePoint()
{
    vec3 px = vec3(v_pos * p.frame.zw, 1.0);
    return vec2(dot(p.toFrame0.xyz, px), dot(p.toFrame1.xyz, px));
}

vec3 yiqOf(vec3 c)
{
    return vec3(dot(c, vec3(0.299, 0.587, 0.114)), dot(c, vec3(0.595716, -0.274453, -0.321263)),
                dot(c, vec3(0.211456, -0.522591, 0.311135)));
}

vec3 fromYiq(vec3 y)
{
    return vec3(y.x + 0.9563 * y.y + 0.6210 * y.z, y.x - 0.2721 * y.y - 0.6474 * y.z, y.x - 1.1070 * y.y + 1.7046 * y.z);
}

// The Video Limiter on one encoded value: compressed above the knee towards the level (Compression Before Clipping), or
// clipped at it.
float limitOf(float v, float level, float knee, float k)
{
    if (k > 0.0 && v > knee)
        return knee + (level - knee) * (1.0 - exp(-(v - knee) / (level - knee)));
    return min(v, level);
}

// Strobe Light's operators on 0..1 values.
float strobeOperator(int op, float e, float c)
{
    if (op == 0)
        return c;
    if (op == 1)
        return min(1.0, e + c);
    if (op == 2)
        return max(0.0, e - c);
    if (op == 3)
        return e * c;
    if (op == 4)
        return abs(e - c);
    if (op >= 5 && op <= 7) {
        uint a = uint(clamp(floor(e * 255.0 + 0.5), 0.0, 255.0));
        uint b = uint(clamp(floor(c * 255.0 + 0.5), 0.0, 255.0));
        uint r = op == 5 ? (a & b) : op == 6 ? (a | b) : (a ^ b);
        return float(r) / 255.0;
    }
    if (op == 10)
        return min(e, c);
    if (op == 11)
        return max(e, c);
    if (op == 12)
        return 1.0 - (1.0 - e) * (1.0 - c);
    return e;
}

void main()
{
    loadParams();
    int op = p.mode.z;
    vec4 c = textureLod(tex0, v_uv, 0.0);
    float a = clamp(c.a, 0.0, 1.0);
    vec3 e = a > 0.0 ? oetf3(c.rgb / a) : vec3(0.0);
    vec3 r = e;     // the result's straight encoded colour
    float ra = a;   // and alpha
    if (op == OP_BRIGHTNESS_CONTRAST) {
        float b = q[0] / 200.0;
        float k = clamp(q[1] / 100.0, -1.0, 1.0);
        k = (1.0 + k) / (1.0 - 0.99 * k);
        r = clamp((e + vec3(b) - 0.5) * k + 0.5, 0.0, 1.0);
    } else if (op == OP_LEVELS) {
        // Each channel's own levels, then the RGB levels.
        vec3 v = vec3(levelsOf(e.r, q[5], q[6], q[7], q[8], q[9]), levelsOf(e.g, q[10], q[11], q[12], q[13], q[14]),
                      levelsOf(e.b, q[15], q[16], q[17], q[18], q[19]));
        r = vec3(levelsOf(v.r, q[0], q[1], q[2], q[3], q[4]), levelsOf(v.g, q[0], q[1], q[2], q[3], q[4]),
                 levelsOf(v.b, q[0], q[1], q[2], q[3], q[4]));
    } else if (op == OP_PROC_AMP) {
        bool original = q[4] > 0.5 && framePoint().x >= q[5] * p.frame.x;
        if (!original) {
            float y = luma(e) * q[1] / 100.0 + q[0] / 200.0;
            vec2 cc = chroma(e);
            float h = radians(q[2]);
            vec2 rot = vec2(cc.x * cos(h) - cc.y * sin(h), cc.x * sin(h) + cc.y * cos(h)) * (q[3] / 100.0);
            float red = y + 1.5748 * rot.y;
            float blue = y + 1.8556 * rot.x;
            r = clamp(vec3(red, (y - 0.2126 * red - 0.0722 * blue) / 0.7152, blue), 0.0, 1.0);
        }
    } else if (op == OP_EXTRACT) {
        float y = luma(e) * 255.0;
        float s = q[2] / 100.0 * 64.0;
        float m = s > 0.0 ? clamp((y - q[0] + s) / s, 0.0, 1.0) * clamp((q[1] + s - y) / s, 0.0, 1.0)
                          : (y >= q[0] && y <= q[1] ? 1.0 : 0.0);
        if (q[3] > 0.5)
            m = 1.0 - m;
        r = vec3(m);
    } else if (op == OP_INVERT) {
        int ch = int(q[0] + 0.5);
        vec3 hls = toHls(e);
        vec3 yiq = yiqOf(e);
        float alpha = a;
        if (ch == 0)
            r = 1.0 - e;
        else if (ch == 1)
            r = vec3(1.0 - e.r, e.g, e.b);
        else if (ch == 2)
            r = vec3(e.r, 1.0 - e.g, e.b);
        else if (ch == 3)
            r = vec3(e.r, e.g, 1.0 - e.b);
        else if (ch == 4)
            r = fromHls(vec3(hls.x + 0.5, 1.0 - hls.y, 1.0 - hls.z));
        else if (ch == 5)
            r = fromHls(vec3(hls.x + 0.5, hls.y, hls.z));
        else if (ch == 6)
            r = fromHls(vec3(hls.x, 1.0 - hls.y, hls.z));
        else if (ch == 7)
            r = fromHls(vec3(hls.x, hls.y, 1.0 - hls.z));
        else if (ch == 8)
            r = fromYiq(vec3(1.0 - yiq.x, -yiq.y, -yiq.z));
        else if (ch == 9)
            r = fromYiq(vec3(1.0 - yiq.x, yiq.y, yiq.z));
        else if (ch == 10)
            r = fromYiq(vec3(yiq.x, -yiq.y, yiq.z));
        else if (ch == 11)
            r = fromYiq(vec3(yiq.x, yiq.y, -yiq.z));
        else
            alpha = 1.0 - a;
        r = mix(clamp(r, 0.0, 1.0), e, q[1]);
        ra = mix(alpha, a, q[1]);
    } else if (op == OP_TINT) {
        vec3 tinted = mix(vec3(q[0], q[1], q[2]), vec3(q[3], q[4], q[5]), luma(e));
        r = mix(e, tinted, q[6]);
    } else if (op == OP_CHANGE_COLOR) {
        float d = colorDistance(e, vec3(q[4], q[5], q[6]), int(q[9] + 0.5));
        float m = 1.0 - smoothstep(q[7], q[7] + q[8] + 1e-5, d);
        if (q[10] > 0.5)
            m = 1.0 - m;
        if (q[0] > 0.5) {
            r = vec3(m);
        } else {
            vec3 hls = toHls(e);
            hls.x = hls.x + q[1] / 360.0 * m;
            hls.y = clamp(hls.y + q[2] / 100.0 * m, 0.0, 1.0);
            hls.z = clamp(hls.z + q[3] / 100.0 * m, 0.0, 1.0);
            r = fromHls(hls);
        }
    } else if (op == OP_CHANGE_TO_COLOR) {
        vec3 from = toHls(vec3(q[0], q[1], q[2]));
        vec3 to = toHls(vec3(q[3], q[4], q[5]));
        vec3 hls = toHls(e);
        float soft = 0.5 * q[11] + 1e-5;
        float m = (1.0 - smoothstep(q[8], q[8] + soft, abs(hueDelta(hls.x, from.x))))
                  * (1.0 - smoothstep(q[9], q[9] + soft, abs(hls.y - from.y)))
                  * (1.0 - smoothstep(q[10], q[10] + soft, abs(hls.z - from.z)));
        int change = int(q[6] + 0.5);
        bool useL = change == 1 || change == 3;
        bool useS = change == 2 || change == 3;
        if (q[12] > 0.5) {
            r = vec3(m);
        } else {
            if (q[7] > 0.5) { // transforming: move by From -> To
                hls.x += hueDelta(from.x, to.x) * m;
                if (useL)
                    hls.y = clamp(hls.y + (to.y - from.y) * m, 0.0, 1.0);
                if (useS)
                    hls.z = clamp(hls.z + (to.z - from.z) * m, 0.0, 1.0);
            } else {          // setting: become To's components
                hls.x += hueDelta(hls.x, to.x) * m;
                if (useL)
                    hls.y = mix(hls.y, to.y, m);
                if (useS)
                    hls.z = mix(hls.z, to.z, m);
            }
            r = fromHls(hls);
        }
    } else if (op == OP_LEAVE_COLOR) {
        float d = colorDistance(e, vec3(q[1], q[2], q[3]), int(q[6] + 0.5));
        float keep = 1.0 - smoothstep(q[4], q[4] + q[5] + 1e-5, d);
        r = mix(e, vec3(luma(e)), q[0] * (1.0 - keep));
    } else if (op == OP_COLOR_BALANCE) {
        float l = luma(e);
        float ws = 1.0 - smoothstep(0.0, 0.5, l);
        float wh = smoothstep(0.5, 1.0, l);
        float wm = 1.0 - ws - wh;
        vec3 delta = (ws * vec3(q[0], q[1], q[2]) + wm * vec3(q[3], q[4], q[5]) + wh * vec3(q[6], q[7], q[8])) / 100.0 * 0.3;
        r = clamp(e + delta, 0.0, 1.0);
        if (q[9] > 0.5)
            r = clamp(r + vec3(l - luma(r)), 0.0, 1.0);
    } else if (op == OP_VIDEO_LIMITER) {
        float level = 1.0 + floor(q[0] + 0.5) / 100.0;
        int ci = int(q[1] + 0.5);
        float k = ci == 1 ? 0.03 : ci == 2 ? 0.05 : ci == 3 ? 0.10 : ci == 4 ? 0.20 : 0.0;
        float knee = level * (1.0 - k);
        vec3 lin = a > 0.0 ? max(c.rgb / a, vec3(0.0)) : vec3(0.0);
        vec3 v = vec3(oetfExt(lin.r), oetfExt(lin.g), oetfExt(lin.b));
        vec3 w = vec3(limitOf(v.r, level, knee, k), limitOf(v.g, level, knee, k), limitOf(v.b, level, knee, k));
        bool limited = any(notEqual(w, v));
        vec3 outLin = vec3(eotfExt(w.r), eotfExt(w.g), eotfExt(w.b));
        if (q[2] > 0.5 && limited)
            outLin = eotf3(vec3(q[3], q[4], q[5]));
        fragColor = vec4(outLin * a, a);
        return;
    } else if (op == OP_BLACK_WHITE) {
        r = vec3(luma(e));
    } else if (op == OP_COLOR_PASS) {
        float sim = q[3] / 100.0;
        float keep = 1.0 - smoothstep(sim, sim + 0.004, colorDistance(e, vec3(q[0], q[1], q[2]), 0));
        if (q[4] > 0.5)
            keep = 1.0 - keep;
        r = mix(vec3(luma(e)), e, keep);
    } else if (op == OP_COLOR_REPLACE) {
        float sim = q[6] / 100.0;
        float m = 1.0 - smoothstep(sim, sim + 0.004, colorDistance(e, vec3(q[0], q[1], q[2]), 0));
        vec3 replace = vec3(q[3], q[4], q[5]);
        r = mix(e, q[7] > 0.5 ? replace : clamp(setLum(replace, luma(e)), 0.0, 1.0), m);
    } else if (op == OP_GAMMA_CORRECTION) {
        r = pow(e, vec3(q[0] / 10.0));
    } else if (op == OP_POSTERIZE) {
        float levels = max(2.0, floor(q[0] + 0.5));
        r = min(floor(e * levels), vec3(levels - 1.0)) / (levels - 1.0);
    } else if (op == OP_SOLARIZE) {
        r = mix(e, 1.0 - e, vec3(greaterThan(e, vec3(q[0]))));
    } else if (op == OP_THRESHOLD) {
        r = vec3(luma(e) * 255.0 >= q[0] ? 1.0 : 0.0);
    } else if (op == OP_STROBE_LIGHT) {
        if (q[10] > 0.5) {
            if (q[7] > 0.5) {
                ra = a * q[3]; // transparent while it strobes
            } else {
                int sop = int(q[8] + 0.5);
                vec3 s = vec3(q[0], q[1], q[2]);
                vec3 o;
                if (sop == 8 || sop == 9) { // Lighten / Darken: the lighter or darker whole colour
                    bool sLighter = luma(s) > luma(e);
                    o = (sop == 8) == sLighter ? s : e;
                } else {
                    o = vec3(strobeOperator(sop, e.r, s.r), strobeOperator(sop, e.g, s.g), strobeOperator(sop, e.b, s.b));
                }
                r = mix(o, e, q[3]);
            }
        }
    } else if (op == OP_ALPHA_ADJUST) {
        float na = q[1] > 0.5 ? 1.0 : a;
        if (q[2] > 0.5)
            na = 1.0 - na;
        na *= q[0];
        if (q[3] > 0.5) {
            float g = eotf(na);
            fragColor = vec4(g, g, g, 1.0);
        } else {
            vec3 straight = a > 0.0 ? c.rgb / a : vec3(0.0);
            fragColor = vec4(straight * na, na);
        }
        return;
    } else if (op == OP_LUMA_KEY) {
        float y = luma(e);
        float t = q[0];
        float cut = q[1];
        float k = t == cut ? (y >= t ? 1.0 : 0.0) : clamp((y - cut) / (t - cut), 0.0, 1.0);
        ra = a * k;
    } else if (op == OP_NOISE) {
        uvec2 xy = uvec2(gl_FragCoord.xy);
        uint frame = uint(max(0.0, floor(q[31] * 1000.0 + 0.5)));
        float amount = q[0];
        bool color = q[1] > 0.5;
        uint cell = xy.x + 65536u * xy.y;
        vec3 n = vec3(hash01(cell, frame, 0u), hash01(cell, frame, color ? 1u : 0u), hash01(cell, frame, color ? 2u : 0u));
        vec3 v = e + (n - 0.5) * amount;
        r = q[2] > 0.5 ? clamp(v, 0.0, 1.0) : v - floor(v);
    }
    fragColor = vec4(eotf3(r) * ra, ra);
}
