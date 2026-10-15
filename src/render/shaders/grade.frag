#version 440

// The colour grade (wabcut.color.grade) on a canvas-size intermediate: linear Rec.709, premultiplied, RGBA16F in and
// out. Every formula mirrors the CPU reference in src/render/color/src/Grade.cpp (wabcut::color::gradeMain and
// friends) - change both together; tests/render/test_grade.cpp compares them stage by stage. One pass per mode:
//   0 MAIN       stages 1-9; also the secondary (no refine), vignette and mask view when `mode.z & F_FINAL`
//   1 MATTE      the HSL qualifier of tex0 (MAIN's output) -> (m, m, m, 1)
//   2 THRESHOLD  the denoise's soft threshold of tex0 (the blurred matte)
//   3 APPLY      the secondary of tex0 (MAIN's output) by the matte tex1, sharpened against tex2, then vignette
//   4 MIX        before/after: tex1 (the layer before the grade) left of the split, tex0 right of it

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pos;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform GradeParams {
    mat4 cornerToClip;
    vec4 texel;     // xy = 1 / texture size, zw = size
    vec4 toFrame0;  // canvas sequence px -> layer frame px (vignette)
    vec4 toFrame1;
    vec4 frame;     // xy = layer frame size, zw = canvas size
    ivec4 mode;     // x = pass, y = 1 when stored bottom-up, z = flags, w = mask view (0 off, 1 grey, 2 on black, 3 on white)
    ivec4 lut;      // x = input LUT (bit 0 1D, bit 1 3D), y = look LUT (same), z = 1 trilinear, w = unused
    ivec4 lutSize;  // x = input 3D size, y = input 1D length, z = look 3D size, w = look 1D length
    vec4 in1dMin;
    vec4 in1dMax;
    vec4 in3dMin;
    vec4 in3dMax;
    vec4 look1dMin;
    vec4 look1dMax;
    vec4 look3dMin;
    vec4 look3dMax;
    mat4 wb;        // white balance (3x3 in the upper left)
    mat4 hslWb;
    vec4 basic;     // x = exposure gain, y = contrast power, z = saturation (%), w = vibrance (%)
    vec4 toneY;     // tone curve y at 0, 0.25, 0.75, 1 (y at 0.5 is 0.5)
    vec4 toneM0;    // tangents at 0, 0.25, 0.5, 0.75
    vec4 toneM1;    // x = tangent at 1
    vec4 creative;  // x = look intensity (%), y = faded film (0..1), z = saturation (%), w = vibrance (%)
    vec4 shadowTint;    // rgb = chroma * amount * 0.25, w = tint balance (-1..1)
    vec4 highlightTint; // rgb = chroma * amount * 0.25
    vec4 cdlSlope;  // rgb, w = CDL saturation
    vec4 cdlOffset;
    vec4 cdlPower;
    vec4 hslHue;    // centre, width, softness (degrees), w = invert
    vec4 hslSat;    // low, high, softness (percent)
    vec4 hslLuma;   // low, high, softness (percent)
    vec4 hslCorr;   // x = contrast factor, y = sharpen amount, z = saturation factor, w = unused
    vec4 hslOffset; // rgb
    vec4 vignette;  // amount, midpoint, roundness, feather
    vec4 split;     // the before side: dot(vec3(v_pos, 1), split.xyz) < 0 (top level: x < position; a nested canvas:
                    // the same screen line pulled back through its placement, I.9)
} p;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;
layout(binding = 3) uniform sampler2D tex2;
layout(binding = 4) uniform sampler2D curves;  // rows: 0 RGB+master, 1 hue curves, 2 luma/sat vs sat, 3 input 1D, 4 look 1D
layout(binding = 5) uniform sampler3D lutIn;
layout(binding = 6) uniform sampler3D lutLook;

const int PASS_MAIN = 0;
const int PASS_MATTE = 1;
const int PASS_THRESHOLD = 2;
const int PASS_APPLY = 3;
const int PASS_MIX = 4;

// mode.z flags
const int F_WB = 1;
const int F_EXPOSURE = 2;
const int F_CONTRAST = 4;
const int F_TONE = 8;
const int F_SATURATION = 16;
const int F_LOOK = 32;
const int F_FADED = 64;
const int F_TINTS = 128;
const int F_CREATIVE_SAT = 256;
const int F_CURVES_RGB = 512;
const int F_CURVES_HUE = 1024;
const int F_WHEELS = 2048;
const int F_CDL_SAT = 4096;
const int F_SECONDARY = 8192;   // the HSL secondary runs (MAIN with F_FINAL, or APPLY)
const int F_VIGNETTE = 16384;
const int F_FINAL = 32768;      // MAIN also does the secondary (without refine), mask view and vignette
const int F_INPUT_LUT = 65536;
const int F_BASIC = 131072;     // the Basic stage runs at all
const int F_CREATIVE = 262144;  // the Creative stage runs at all
const int F_HSL_WB = 524288;
const int F_HSL_CONTRAST = 1048576;
const int F_HSL_SHARPEN = 2097152;
const int F_HSL_SAT = 4194304;
const int F_CURVES = 8388608;

const int CURVE_SIZE = 1024;
const float REC709_A = 1.09929682680944;
const float REC709_B = 0.018053968510807;
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

bool has(int flag) { return (p.mode.z & flag) != 0; }

float encode1(float l) { return l < REC709_B ? 4.5 * l : REC709_A * pow(l, 0.45) - (REC709_A - 1.0); }
float decode1(float v) { return v < 4.5 * REC709_B ? v / 4.5 : pow((v + (REC709_A - 1.0)) / REC709_A, 1.0 / 0.45); }
vec3 encode709(vec3 l) { return vec3(encode1(l.r), encode1(l.g), encode1(l.b)); }
vec3 decode709(vec3 v) { return vec3(decode1(v.r), decode1(v.g), decode1(v.b)); }

float sstep(float e0, float e1, float x)
{
    if (e1 <= e0)
        return x < e0 ? 0.0 : 1.0;
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// HSV of the given values (negatives count as 0): x = hue in turns, y = saturation, z = value.
vec3 toHsv(vec3 c)
{
    c = max(c, vec3(0.0));
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    vec3 o = vec3(0.0, mx > 1e-12 ? min(d / mx, 1.0) : 0.0, mx); // clamped: division may be inexact
    if (d <= 1e-12)
        return o;
    float h;
    if (mx == c.r) {
        h = (c.g - c.b) / d;
        if (h < 0.0)
            h += 6.0;
    } else if (mx == c.g) {
        h = (c.b - c.r) / d + 2.0;
    } else {
        h = (c.r - c.g) / d + 4.0;
    }
    o.x = h / 6.0;
    if (o.x >= 1.0)
        o.x -= 1.0;
    return o;
}

float skinWeight(float h) { return sstep(0.03, 0.07, h) * (1.0 - sstep(0.12, 0.16, h)); }

// Saturation and vibrance about the luma of c; the weights come from `display`.
vec3 saturate(vec3 c, vec3 display, float saturation, float vibrance)
{
    float factor = saturation / 100.0;
    if (vibrance != 0.0) {
        vec3 hsv = toHsv(display);
        float v = vibrance / 100.0 * (1.0 - hsv.y) * (1.0 - hsv.y);
        if (vibrance > 0.0)
            v *= 1.0 - 0.7 * skinWeight(hsv.x);
        factor *= 1.0 + v;
    }
    factor = max(factor, 0.0);
    float y = dot(c, LUMA);
    return vec3(y) + (c - vec3(y)) * factor;
}

// Rotates chroma about the grey axis by `turns`, keeping Rec.709 luma (Rodrigues).
vec3 rotateHue(vec3 c, float turns)
{
    float a = turns * 6.283185307179586;
    float ca = cos(a);
    float sa = sin(a);
    float k = 0.5773502691896258;
    vec3 axis = vec3(k);
    float d = (c.r + c.g + c.b) * k;
    vec3 r = c * ca + cross(axis, c) * sa + axis * d * (1.0 - ca);
    float dy = dot(c, LUMA) - dot(r, LUMA);
    return r + vec3(dy);
}

// Tone curve: the Hermite segments of a monotone cubic through x = 0, 0.25, 0.5, 0.75, 1 (CPU: toneCurve).
float toneCurve(float x)
{
    float ys[5] = float[5](p.toneY.x, p.toneY.y, 0.5, p.toneY.z, p.toneY.w);
    float ms[5] = float[5](p.toneM0.x, p.toneM0.y, p.toneM0.z, p.toneM0.w, p.toneM1.x);
    if (x < 0.0)
        return ys[0] + ms[0] * x;
    if (x > 1.0)
        return ys[4] + ms[4] * (x - 1.0);
    int k = min(int(floor(x * 4.0)), 3);
    float h = 0.25;
    float t = (x - float(k) * 0.25) / h;
    float t2 = t * t;
    float t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * ys[k] + (t3 - 2.0 * t2 + t) * h * ms[k] + (-2.0 * t3 + 3.0 * t2) * ys[k + 1] +
           (t3 - t2) * h * ms[k + 1];
}

// A baked row (CPU: sampleRow): entries at i / (n - 1), or i / n wrapping when periodic.
float row(int r, int c, float x, int n, bool periodic)
{
    float pos;
    int i0;
    int i1;
    if (periodic) {
        pos = (x - floor(x)) * float(n);
        i0 = min(int(floor(pos)), n - 1);
        i1 = (i0 + 1) % n;
    } else {
        pos = clamp(x, 0.0, 1.0) * float(n - 1);
        i0 = min(int(floor(pos)), n - 2);
        i1 = i0 + 1;
    }
    float t = pos - float(i0);
    float a = texelFetch(curves, ivec2(i0, r), 0)[c];
    float b = texelFetch(curves, ivec2(i1, r), 0)[c];
    return a + (b - a) * t;
}

float rgbCurve(int c, float x)
{
    if (x < 0.0)
        return row(0, c, 0.0, CURVE_SIZE, false) + x;
    if (x > 1.0)
        return row(0, c, 1.0, CURVE_SIZE, false) + (x - 1.0);
    return row(0, c, x, CURVE_SIZE, false);
}

vec3 lut1d(int r, int n, vec3 x, vec3 lo, vec3 hi)
{
    vec3 o;
    for (int c = 0; c < 3; ++c) {
        float t = (x[c] - lo[c]) / (hi[c] - lo[c]);
        t = t >= 0.0 ? min(t, 1.0) : 0.0;
        o[c] = row(r, c, t, n, false);
    }
    return o;
}

vec3 corner(int which, ivec3 i)
{
    return which == 0 ? texelFetch(lutIn, i, 0).rgb : texelFetch(lutLook, i, 0).rgb;
}

// A 3D LUT (CPU: apply3d): tetrahedral, or trilinear for comparison.
vec3 lut3d(int which, int n, vec3 x, vec3 lo, vec3 hi)
{
    float last = float(n - 1);
    ivec3 i0;
    vec3 f;
    for (int c = 0; c < 3; ++c) {
        float t = (x[c] - lo[c]) / (hi[c] - lo[c]);
        t = (t >= 0.0 ? min(t, 1.0) : 0.0) * last;
        i0[c] = min(int(floor(t)), n - 2);
        f[c] = t - float(i0[c]);
    }
    if (p.lut.z == 1) {
        vec3 c000 = corner(which, i0);
        vec3 c100 = corner(which, i0 + ivec3(1, 0, 0));
        vec3 c010 = corner(which, i0 + ivec3(0, 1, 0));
        vec3 c110 = corner(which, i0 + ivec3(1, 1, 0));
        vec3 c001 = corner(which, i0 + ivec3(0, 0, 1));
        vec3 c101 = corner(which, i0 + ivec3(1, 0, 1));
        vec3 c011 = corner(which, i0 + ivec3(0, 1, 1));
        vec3 c111 = corner(which, i0 + ivec3(1, 1, 1));
        vec3 x00 = mix(c000, c100, f.x);
        vec3 x10 = mix(c010, c110, f.x);
        vec3 x01 = mix(c001, c101, f.x);
        vec3 x11 = mix(c011, c111, f.x);
        return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
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
    return w0 * corner(which, i0) + wa * corner(which, i0 + da) + wb * corner(which, i0 + db) +
           w1 * corner(which, i0 + ivec3(1, 1, 1));
}

vec3 applyLut(int which, vec3 x)
{
    int kind = which == 0 ? p.lut.x : p.lut.y;
    if ((kind & 1) != 0) {
        x = which == 0 ? lut1d(3, p.lutSize.y, x, p.in1dMin.xyz, p.in1dMax.xyz)
                       : lut1d(4, p.lutSize.w, x, p.look1dMin.xyz, p.look1dMax.xyz);
    }
    if ((kind & 2) != 0) {
        x = which == 0 ? lut3d(0, p.lutSize.x, x, p.in3dMin.xyz, p.in3dMax.xyz)
                       : lut3d(1, p.lutSize.z, x, p.look3dMin.xyz, p.look3dMax.xyz);
    }
    return x;
}

float rangeMask(float x, float lo, float hi, float soft)
{
    if (soft <= 0.0)
        return x >= lo && x <= hi ? 1.0 : 0.0;
    return sstep(lo - soft, lo, x) * (1.0 - sstep(hi, hi + soft, x));
}

// Stages 1-9 (CPU: gradeMain): straight linear in, display (encoded) out.
vec3 gradeMain(vec3 c)
{
    if (has(F_INPUT_LUT))
        c = decode709(applyLut(0, encode709(c)));
    if (has(F_BASIC)) {
        if (has(F_WB))
            c = mat3(p.wb) * c;
        c *= p.basic.x;
        if (has(F_CONTRAST)) {
            for (int i = 0; i < 3; ++i)
                c[i] = c[i] <= 0.0 ? c[i] : 0.18 * pow(c[i] / 0.18, p.basic.y);
        }
        if (has(F_TONE)) {
            vec3 e = encode709(c);
            float y = dot(e, LUMA);
            float d = toneCurve(y) - y;
            c = decode709(e + vec3(d));
        }
        if (has(F_SATURATION))
            c = saturate(c, encode709(c), p.basic.z, p.basic.w);
    }
    vec3 e = encode709(c);
    if (has(F_CREATIVE)) {
        if (has(F_LOOK)) {
            vec3 l = applyLut(1, e);
            e = e + (l - e) * (p.creative.x / 100.0);
        }
        if (has(F_FADED)) {
            vec3 x = clamp(e, 0.0, 1.0);
            e += 0.2 * p.creative.y * (1.0 - x) * (1.0 - x);
        }
        if (has(F_TINTS)) {
            float u = clamp(dot(e, LUMA) + 0.5 * p.shadowTint.w, 0.0, 1.0);
            e += p.shadowTint.rgb * ((1.0 - u) * (1.0 - u)) + p.highlightTint.rgb * (u * u);
        }
        if (has(F_CREATIVE_SAT))
            e = saturate(e, e, p.creative.z, p.creative.w);
    }
    if (has(F_CURVES)) {
        if (has(F_CURVES_RGB)) {
            for (int ch = 0; ch < 3; ++ch) {
                float m = rgbCurve(3, e[ch]);
                e[ch] = rgbCurve(ch, m);
            }
        }
        if (has(F_CURVES_HUE)) {
            vec3 hsv = toHsv(e);
            float y = dot(e, LUMA);
            float hueSat = row(1, 0, hsv.x, CURVE_SIZE, true);
            float hueHue = row(1, 1, hsv.x, CURVE_SIZE, true);
            float hueLuma = row(1, 2, hsv.x, CURVE_SIZE, true);
            float lumaSat = row(2, 0, clamp(y, 0.0, 1.0), CURVE_SIZE, false);
            float satSat = row(2, 1, hsv.y, CURVE_SIZE, false);
            float factor = (2.0 * hueSat) * (2.0 * lumaSat) * (2.0 * satSat);
            vec3 r = rotateHue(e, hueHue - 0.5);
            e = vec3(y) + (r - vec3(y)) * factor;
            e += vec3((hueLuma - 0.5) * hsv.y * 0.5);
        }
    }
    if (has(F_WHEELS)) {
        e = pow(clamp(e * p.cdlSlope.rgb + p.cdlOffset.rgb, 0.0, 1.0), p.cdlPower.rgb);
        if (has(F_CDL_SAT)) {
            float y = dot(e, LUMA);
            e = clamp(vec3(y) + (e - vec3(y)) * p.cdlSlope.w, 0.0, 1.0);
        }
    }
    return e;
}

// The HSL qualifier (CPU: qualifierMatte).
float qualifier(vec3 display)
{
    vec3 hsv = toHsv(display);
    float mh = 1.0;
    if (p.hslHue.y < 360.0) {
        float d = abs(hsv.x * 360.0 - p.hslHue.x);
        d = mod(d, 360.0);
        d = min(d, 360.0 - d);
        float inner = p.hslHue.y / 2.0;
        mh = p.hslHue.z <= 0.0 ? (d <= inner ? 1.0 : 0.0) : 1.0 - sstep(inner, inner + p.hslHue.z, d);
    }
    float s = hsv.y * 100.0;
    float l = clamp(dot(display, LUMA), 0.0, 1.0) * 100.0;
    float m = mh * rangeMask(s, p.hslSat.x, p.hslSat.y, p.hslSat.z) * rangeMask(l, p.hslLuma.x, p.hslLuma.y, p.hslLuma.z);
    return p.hslHue.w > 0.5 ? 1.0 - m : m;
}

// The secondary's correction mixed in by the matte (CPU: applySecondary).
vec3 secondary(vec3 display, float matte, vec3 blurred)
{
    vec3 c = display;
    if (has(F_HSL_WB))
        c = encode709(mat3(p.hslWb) * decode709(c));
    if (has(F_HSL_CONTRAST))
        c = (c - vec3(0.5)) * p.hslCorr.x + vec3(0.5);
    if (has(F_HSL_SHARPEN))
        c = c + (display - blurred) * p.hslCorr.y;
    if (has(F_HSL_SAT)) {
        float y = dot(c, LUMA);
        c = vec3(y) + (c - vec3(y)) * p.hslCorr.z;
    }
    c += p.hslOffset.rgb;
    return mix(display, c, clamp(matte, 0.0, 1.0));
}

vec3 maskView(vec3 display, float matte)
{
    if (p.mode.w == 1)
        return vec3(matte);
    if (p.mode.w == 2)
        return display * matte;
    if (p.mode.w == 3)
        return display * matte + vec3(1.0 - matte);
    return display;
}

// Vignette (CPU: applyVignette; wabcut.vignette's formula).
vec3 applyVignette(vec3 c)
{
    vec3 px = vec3(v_pos * p.frame.zw, 1.0);
    vec2 src = vec2(dot(p.toFrame0.xyz, px), dot(p.toFrame1.xyz, px));
    vec2 q = src / p.frame.xy * 2.0 - 1.0;
    float roundness = p.vignette.z / 100.0;
    float aspect = p.frame.x / max(p.frame.y, 1.0);
    q.x *= 1.0 + (aspect - 1.0) * max(roundness, 0.0);
    float n = 2.0 + 6.0 * max(-roundness, 0.0);
    vec2 a = abs(q);
    float d = pow(pow(a.x, n) + pow(a.y, n), 1.0 / n);
    float mid = 0.3 + 0.9 * p.vignette.y / 100.0;
    float width = max(0.01, p.vignette.w / 100.0);
    float v = sstep(mid - 0.5 * width, mid + 0.5 * width, d);
    return c * exp2(p.vignette.x * v);
}

vec4 straight(vec4 c) { return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0); }

void main()
{
    int pass = p.mode.x;
    vec4 c = textureLod(tex0, v_uv, 0.0);
    if (pass == PASS_MIX) {
        vec4 before = textureLod(tex1, v_uv, 0.0);
        fragColor = dot(vec3(v_pos, 1.0), p.split.xyz) < 0.0 ? before : c;
        return;
    }
    if (pass == PASS_THRESHOLD) {
        float m = sstep(0.25, 0.75, c.r);
        fragColor = vec4(m, m, m, 1.0);
        return;
    }
    if (c.a <= 0.0) {
        fragColor = pass == PASS_MATTE ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(0.0);
        return;
    }
    vec4 s = straight(c);
    if (pass == PASS_MATTE) {
        float m = qualifier(encode709(s.rgb));
        fragColor = vec4(m, m, m, 1.0);
        return;
    }
    vec3 e;
    if (pass == PASS_MAIN) {
        e = gradeMain(s.rgb);
        if (!has(F_FINAL)) {
            fragColor = vec4(decode709(e) * s.a, s.a); // stage 9's output for the secondary's passes
            return;
        }
        if (has(F_SECONDARY)) {
            float m = qualifier(e);
            vec3 graded = secondary(e, m, e);
            e = p.mode.w != 0 ? maskView(e, m) : graded;
        }
    } else { // PASS_APPLY
        e = encode709(s.rgb);
        float m = textureLod(tex1, v_uv, 0.0).r;
        vec3 blurred = encode709(straight(textureLod(tex2, v_uv, 0.0)).rgb);
        vec3 graded = secondary(e, m, blurred);
        e = p.mode.w != 0 ? maskView(e, m) : graded;
    }
    vec3 lin = decode709(e);
    if (has(F_VIGNETTE))
        lin = applyVignette(lin);
    fragColor = vec4(lin * s.a, s.a);
}
