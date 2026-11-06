#version 440

// Output pass: linear premultiplied canvas -> the role's output transform (display or export encoding).
// Mirrors render::linearToOutput().

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform OutputParams {
    mat4 cornerToClip;
    vec4 uvXform;
    ivec4 mode;
    mat4 toOutput; // I.9: working -> Rec.2020 (PQ, HLG); identity before a LUT; Rec.2020 -> Rec.709 (tone map)
    vec4 cm;       // I.9: x = reference white (nits; a LUT: nits of display light 1.0), y = peak (nits), z = HLG system
                   // gamma, w = outLut size
    mat4 conformIn; // 0.6 (#72) SDR Conform: working -> Rec.2020
    vec4 conform0;  // x = paper white / target peak, y = the source peak in those units, z = operator, w = gamut mapping
    vec4 conform1;  // x = legal range, y = target peak (nits), z = a split preview's before kind (-1: none), w = the split
    vec4 vr0;       // 0.7 (VR / 360 video), the viewer: yaw, pitch, roll (degrees), tan(fov / 2)
    vec4 vr1;       // x = the view's aspect, y = the canvas' projection (0: no viewer, 1: 360, 2: 180), z = its stereo
                    // layout (0 mono, 1 top-bottom, 2 side-by-side), w = the eye
} p;

layout(binding = 1) uniform sampler2D canvasTex;
layout(binding = 2) uniform sampler3D outLut; // I.9 (a 1x1x1 dummy when unused)

// mode.x
const int OUT_REC709 = 0;
const int OUT_SRGB = 1;
const int OUT_PQ = 3;  // I.9, managed pipelines
const int OUT_HLG = 4;
const int OUT_LUT = 5;      // mode.z: the finish (0 as is, 1 BT.1886, 2 PQ, 3 HLG)
const int OUT_TONEMAP = 6;  // the tone-mapped SDR preview of HDR (color::toneMapToSdr)
const int OUT_REC709_MATRIX = 7; // 0.2, Wide Gamut (No Tone Mapping): toOutput (ACEScg -> Rec.709), then BT.709, clipped
const int OUT_CONFORM = 8;       // 0.6 (#72): SDR Conform (color::sdrConformPixel)
// mode.z of OUT_TONEMAP, OUT_REC709_MATRIX and OUT_CONFORM (0.6): 0 = the BT.709 curve, 1 = a 2.6 power (P3-D65); of
// OUT_LUT: its finish (4 = a 2.6 power). mode.w (OUT_CONFORM, opaque exports that report): the flags in the alpha.
// mode.y
const int ALPHA_OPAQUE = 0;
const int ALPHA_STRAIGHT = 1;

float encode(float l, int t)
{
    l = clamp(l, 0.0, 1.0);
    if (t == OUT_SRGB)
        return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    if (t == OUT_REC709) {
        const float a = 1.09929682680944;
        const float b = 0.018053968510807;
        return l < b ? 4.5 * l : a * pow(l, 0.45) - (a - 1.0);
    }
    return l;
}

vec3 encode3(vec3 l, int t)
{
    return vec3(encode(l.r, t), encode(l.g, t), encode(l.b, t));
}

// Colour management (I.9, ColorPipeline v2; mirrors color::runOutputPlan): PQ and HLG from the working space in nits
// (toOutput: working -> Rec.2020, cm.x: the reference white in nits, cm.y: the HLG peak), or a baked output transform
// (the ACES output transforms) indexed by the log2 shaper of the working value (tetrahedral, like
// color::BakedLut::sample), then the display's encoding.
float nitsToPq(float nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float pqToNits(float code)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float e = pow(clamp(code, 0.0, 1.0), 1.0 / m2);
    return 10000.0 * pow(max(e - c1, 0.0) / (c2 - c3 * e), 1.0 / m1);
}

// 0.6: an SDR target's encoding of linear light, clamped: the BT.709 OETF, or a 2.6 power (P3-D65).
vec3 encodeSdr(vec3 l, int curve)
{
    if (curve == 1)
        return pow(clamp(l, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.6));
    return encode3(l, OUT_REC709);
}

// color::toneMapToSdr: BT.2390 EETF on the luminance (PQ domain) from [0, peak] into [0, reference white], the colour
// scaled along, Rec.2020 -> Rec.709 (toOutput), then BT.709 encoded (clamped).
vec3 toneMapSdr(vec3 c)
{
    c = max(c, vec3(0.0));
    float white = p.cm.x;
    float peak = p.cm.y;
    float y = dot(c, vec3(0.2627, 0.6780, 0.0593));
    float ratio = 1.0;
    if (y > 0.0 && peak > white) {
        float nits = y * white;
        float pqPeak = nitsToPq(peak);
        float e1 = min(nitsToPq(nits) / pqPeak, 1.0);
        float maxLum = nitsToPq(white) / pqPeak;
        float ks = 1.5 * maxLum - 0.5;
        float e2 = e1;
        if (e1 >= ks && ks < 1.0) {
            float t = (e1 - ks) / (1.0 - ks);
            float t2 = t * t;
            float t3 = t2 * t;
            e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxLum;
        }
        ratio = nits > peak ? white / nits : pqToNits(e2 * pqPeak) / nits;
    }
    return encodeSdr(mat3(p.toOutput) * (c * ratio), p.mode.z);
}

float hlgOetf(float e)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    e = max(e, 0.0);
    return e <= 1.0 / 12.0 ? sqrt(3.0 * e) : a * log(12.0 * e - b) + c;
}

vec3 displayNitsToHlg(vec3 nits)
{
    float peak = p.cm.y;
    float gamma = p.cm.z;
    float yd = max(dot(nits, vec3(0.2627, 0.6780, 0.0593)), 0.0);
    float k = yd > 0.0 ? pow(yd / peak, (1.0 - gamma) / gamma) / peak : 0.0;
    return min(vec3(hlgOetf(nits.r * k), hlgOetf(nits.g * k), hlgOetf(nits.b * k)), vec3(1.0)); // above the peak: the top
}

float shaper(float l) // color::shaperOf: 19 stops of log2 from 2^-12 to 128
{
    l = clamp(l, 0.0, 128.0); // color::kShaperMaxLinear
    return clamp((log2(l + 1.0 / 4096.0) + 12.0) / 19.0, 0.0, 1.0);
}

vec3 outputLut(vec3 x)
{
    int n = int(p.cm.w);
    float last = float(n - 1);
    ivec3 i0;
    vec3 f;
    for (int c = 0; c < 3; ++c) {
        float t = shaper(x[c]) * last;
        i0[c] = min(int(floor(t)), n - 2);
        f[c] = t - float(i0[c]);
    }
    float fr = f.x, fg = f.y, fb = f.z;
    ivec3 da;
    ivec3 db;
    float w0, wa, wb, w1;
    if (fr > fg) {
        if (fg > fb) {
            da = ivec3(1, 0, 0); db = ivec3(1, 1, 0);
            w0 = 1.0 - fr; wa = fr - fg; wb = fg - fb; w1 = fb;
        } else if (fr > fb) {
            da = ivec3(1, 0, 0); db = ivec3(1, 0, 1);
            w0 = 1.0 - fr; wa = fr - fb; wb = fb - fg; w1 = fg;
        } else {
            da = ivec3(0, 0, 1); db = ivec3(1, 0, 1);
            w0 = 1.0 - fb; wa = fb - fr; wb = fr - fg; w1 = fg;
        }
    } else {
        if (fb > fg) {
            da = ivec3(0, 0, 1); db = ivec3(0, 1, 1);
            w0 = 1.0 - fb; wa = fb - fg; wb = fg - fr; w1 = fr;
        } else if (fb > fr) {
            da = ivec3(0, 1, 0); db = ivec3(0, 1, 1);
            w0 = 1.0 - fg; wa = fg - fb; wb = fb - fr; w1 = fr;
        } else {
            da = ivec3(0, 1, 0); db = ivec3(1, 1, 0);
            w0 = 1.0 - fg; wa = fg - fr; wb = fr - fb; w1 = fb;
        }
    }
    return w0 * texelFetch(outLut, i0, 0).rgb + wa * texelFetch(outLut, i0 + da, 0).rgb + wb * texelFetch(outLut, i0 + db, 0).rgb +
           w1 * texelFetch(outLut, i0 + ivec3(1, 1, 1), 0).rgb;
}

vec3 encodeManaged(vec3 l, int t)
{
    if (t == OUT_TONEMAP)
        return toneMapSdr(l);
    if (t == OUT_REC709_MATRIX)
        return encodeSdr(mat3(p.toOutput) * l, p.mode.z);
    vec3 nits;
    if (t == OUT_LUT) {
        vec3 light = outputLut(mat3(p.toOutput) * l); // linear display light
        if (p.mode.z == 0)
            return light;
        if (p.mode.z == 1)
            return pow(clamp(light, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.4));
        if (p.mode.z == 4) // 0.6: P3-D65 (a 2.6 power)
            return pow(clamp(light, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.6));
        nits = max(light, vec3(0.0)) * p.cm.x;
        if (p.mode.z == 2)
            return vec3(nitsToPq(nits.r), nitsToPq(nits.g), nitsToPq(nits.b));
        return displayNitsToHlg(nits);
    }
    nits = max(mat3(p.toOutput) * l, vec3(0.0)) * p.cm.x;
    if (t == OUT_PQ)
        return vec3(nitsToPq(nits.r), nitsToPq(nits.g), nitsToPq(nits.b));
    return displayNitsToHlg(nits);
}

vec3 encodeAny(vec3 l, int t)
{
    return t >= OUT_PQ ? encodeManaged(l, t) : encode3(l, t);
}

// ---- 0.6 (#72): SDR Conform (color::sdrConformPixel, color::conformToneMap, color::gamutCompress) ----

float compressDistance(float dist, float lim, float thr)
{
    const float pwr = 1.2;
    if (dist < thr)
        return dist;
    float scl = (lim - thr) / pow(pow((1.0 - thr) / (lim - thr), -pwr) - 1.0, 1.0 / pwr);
    float nd = (dist - thr) / scl;
    return thr + scl * nd / pow(1.0 + pow(nd, pwr), 1.0 / pwr);
}

vec3 gamutCompress(vec3 c)
{
    float ach = max(c.r, max(c.g, c.b));
    if (ach == 0.0)
        return c;
    vec3 dist = (vec3(ach) - c) / abs(ach);
    vec3 cd = vec3(compressDistance(dist.r, 1.147, 0.815), compressDistance(dist.g, 1.264, 0.803),
                   compressDistance(dist.b, 1.312, 0.880));
    return vec3(ach) - cd * abs(ach);
}

float kneeCurve(float v, float knee, float white)
{
    if (v <= knee)
        return v;
    if (v >= white)
        return 1.0;
    float a = 1.0 - knee;
    float t = (v - knee) / a;
    float big = (white - knee) / a;
    return knee + a * t * (1.0 + t / (big * big)) / (1.0 + t);
}

float acesFilmic(float x)
{
    return x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14);
}

// The operator on s (1.0 = the SDR white) with the source peak `peak`: 0 BT.2390 (PQ, `tp` nits of white), 1 ACES, 2
// Reinhard.
float conformToneMap(float s, float peak, int op, float tp)
{
    if (s <= 0.0)
        return 0.0;
    if (op == 1)
        return clamp(acesFilmic(0.6 * s) / acesFilmic(0.6 * max(peak, 1.0)), 0.0, 1.0);
    if (peak <= 1.0 || s >= peak)
        return min(s, 1.0);
    if (op == 2)
        return min(1.0, s * (1.0 + s / (peak * peak)) / (1.0 + s));
    float pqPeak = nitsToPq(peak * tp);
    float e1 = min(nitsToPq(s * tp) / pqPeak, 1.0);
    float maxLum = nitsToPq(tp) / pqPeak;
    float ks = 1.5 * maxLum - 0.5;
    float e2 = e1;
    if (e1 >= ks && ks < 1.0) {
        float t = (e1 - ks) / (1.0 - ks);
        float t2 = t * t;
        float t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxLum;
    }
    return min(pqToNits(e2 * pqPeak) / tp, 1.0);
}

// A working value conformed into the SDR target: the luminance tone-mapped, the target's primaries (toOutput), the gamut
// mapping (0 ACES RGC, 1 saturation, 2 clip), the legal range's soft clip, the curve. flags: 1 brighter than the SDR
// white, 2 outside the target's gamut.
vec3 conform(vec3 working, out int flags)
{
    vec3 c = mat3(p.conformIn) * working;
    float y = max(dot(c, vec3(0.2627, 0.6780, 0.0593)), 0.0);
    float gain = p.conform0.x;
    float s = y * gain;
    flags = s > 1.0 + 1e-6 ? 1 : 0;
    float yt = conformToneMap(s, p.conform0.y, int(p.conform0.z + 0.5), p.conform1.y);
    float k = y > 0.0 ? yt / y : gain;
    vec3 t = mat3(p.toOutput) * (c * k);
    float lo = min(t.r, min(t.g, t.b));
    float hi = max(t.r, max(t.g, t.b));
    if (lo < -1.0 / 1024.0 || hi > 1.0 + 1.0 / 1024.0)
        flags += 2;
    int gamut = int(p.conform0.w + 0.5);
    if (gamut == 0) {
        t = gamutCompress(t);
    } else if (gamut == 1) {
        float f = 1.0;
        if (lo < 0.0)
            f = min(f, yt / (yt - lo));
        if (hi > 1.0) // at the white itself only a grey fits
            f = min(f, yt < 1.0 ? (1.0 - yt) / (hi - yt) : 0.0);
        t = vec3(yt) + (t - vec3(yt)) * f;
    }
    if (p.conform1.x > 0.5) {
        float m = max(t.r, max(t.g, t.b));
        if (m > 0.9)
            t *= kneeCurve(m, 0.9, 1.25) / m;
    }
    return encodeSdr(t, p.mode.z);
}

// The output of a linear working value: SDR Conform (its split preview's before side: the setup's own SDR output,
// through Rec.2020 and the same toOutput), or the transform. One encodeAny call (F3P, 0.6): shader compilers inline
// every call, and each copy of the output transforms costs D3D's compiler about 0.2 s at the shader's first use.
vec3 encodeOutput(vec3 l, int t, out int flags)
{
    flags = 0;
    if (t == OUT_CONFORM) {
        if (!(p.conform1.z >= 0.0 && v_uv.x < p.conform1.w))
            return conform(l, flags);
        l = mat3(p.conformIn) * l; // the split preview's before side
        t = int(p.conform1.z + 0.5);
    }
    return encodeAny(l, t);
}

// ---- 0.7 (VR / 360 video): the viewer (vr::viewUv in model/Vr.h is its CPU twin) ----
// The canvas is an equirectangular sphere; every pixel of the view looks along a ray from the middle of it: x to the right,
// y up, the plane at distance 1, turned by yaw * pitch * roll (degrees).

const float PI = 3.14159265358979;

vec4 vrSample()
{
    float yaw = radians(p.vr0.x);
    float pitch = radians(p.vr0.y);
    float roll = radians(p.vr0.z);
    float tanH = p.vr0.w;
    vec3 ray = vec3((v_uv.x - 0.5) * 2.0 * tanH, (0.5 - v_uv.y) * 2.0 * tanH / max(p.vr1.x, 1e-4), 1.0);
    float cy = cos(yaw);
    float sy = sin(yaw);
    float cp = cos(pitch);
    float sp = sin(pitch);
    float cr = cos(roll);
    float sr = sin(roll);
    // GLSL matrices are column-major: transpose(mat3(row0, row1, row2)) is the matrix with those rows.
    mat3 rot = transpose(mat3(cy, 0.0, sy, 0.0, 1.0, 0.0, -sy, 0.0, cy)) *
               transpose(mat3(1.0, 0.0, 0.0, 0.0, cp, sp, 0.0, -sp, cp)) *
               transpose(mat3(cr, sr, 0.0, -sr, cr, 0.0, 0.0, 0.0, 1.0));
    vec3 d = normalize(rot * ray);
    float lon = atan(d.x, d.z);
    float lat = asin(clamp(d.y, -1.0, 1.0));
    bool half180 = p.vr1.y > 1.5;
    vec2 e = vec2(0.0, 0.5 - lat / PI);
    if (half180) {
        e.x = lon / PI + 0.5;
        if (abs(lon) > PI * 0.5)
            return vec4(0.0);
    } else {
        e.x = lon / (2.0 * PI) + 0.5;
        e.x -= floor(e.x);
    }
    int stereo = int(p.vr1.z + 0.5);
    vec4 rect = vec4(0.0, 0.0, 1.0, 1.0);
    if (stereo == 1)
        rect = p.vr1.w < 0.5 ? vec4(0.0, 0.0, 1.0, 0.5) : vec4(0.0, 0.5, 1.0, 1.0);
    else if (stereo == 2)
        rect = p.vr1.w < 0.5 ? vec4(0.0, 0.0, 0.5, 1.0) : vec4(0.5, 0.0, 1.0, 1.0);
    return textureLod(canvasTex, rect.xy + e * (rect.zw - rect.xy), 0.0);
}

void main()
{
    vec4 c = p.vr1.y > 0.5 ? vrSample() : texture(canvasTex, v_uv); // linear, premultiplied
    // One encodeOutput call (F3P, 0.6: see encodeOutput): opaque encodes the premultiplied colour over black, the
    // alpha modes the straight colour.
    bool opaque = p.mode.y == ALPHA_OPAQUE;
    float a = clamp(c.a, 0.0, 1.0);
    vec3 straight = opaque ? c.rgb : (a > 0.0 ? c.rgb / a : vec3(0.0));
    int flags = 0;
    vec3 e = encodeOutput(straight, p.mode.x, flags);
    if (opaque) {
        // 0.6: an export that reports SDR Conform carries the flags in the alpha it ignores (render::conformFlagsOf).
        fragColor = vec4(e, p.mode.w == 1 ? 1.0 - float(flags) / 3.0 : 1.0);
    } else {
        fragColor = p.mode.y == ALPHA_STRAIGHT ? vec4(e, a) : vec4(e * a, a);
    }
}
