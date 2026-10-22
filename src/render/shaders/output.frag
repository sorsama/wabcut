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
    return encode3(mat3(p.toOutput) * (c * ratio), OUT_REC709);
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
        return encode3(mat3(p.toOutput) * l, OUT_REC709);
    vec3 nits;
    if (t == OUT_LUT) {
        vec3 light = outputLut(mat3(p.toOutput) * l); // linear display light
        if (p.mode.z == 0)
            return light;
        if (p.mode.z == 1)
            return pow(clamp(light, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.4));
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

void main()
{
    vec4 c = texture(canvasTex, v_uv); // linear, premultiplied
    int t = p.mode.x;
    if (p.mode.y == ALPHA_OPAQUE) {
        fragColor = vec4(encodeAny(c.rgb, t), 1.0); // over black
    } else {
        float a = clamp(c.a, 0.0, 1.0);
        vec3 straight = a > 0.0 ? c.rgb / a : vec3(0.0);
        vec3 e = encodeAny(straight, t);
        fragColor = p.mode.y == ALPHA_STRAIGHT ? vec4(e, a) : vec4(e * a, a);
    }
}
