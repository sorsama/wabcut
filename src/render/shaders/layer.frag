#version 440

// Layer pass: samples the frame's planes, converts to linear Rec.709 with premultiplied alpha and blends it
// "over" the canvas (pipeline blend: ONE, ONE_MINUS_SRC_ALPHA). Mirrors render::referencePixel().

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_src;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform LayerParams {
    mat4 cornerToClip;
    mat4 sampleToRgb;
    mat4 toWorking;
    vec4 chromaXform;
    vec4 srcRect;
    vec4 params;
    ivec4 mode;
    vec4 cm; // I.9: x = HLG system gamma - 1, y = inLut size, z = a capture's reference white, w = its peak (nits)
} p;

layout(binding = 1) uniform sampler2D plane0;
layout(binding = 2) uniform sampler2D plane1;
layout(binding = 3) uniform sampler2D plane2;
layout(binding = 4) uniform sampler2D plane3;
layout(binding = 5) uniform sampler3D inLut; // I.9 (a 1x1x1 dummy when unused)

// mode.x
const int LAYOUT_SEMI_PLANAR = 0;
const int LAYOUT_PLANAR3 = 1;
const int LAYOUT_PLANAR4 = 2;
// mode.y
const int TF_BT709 = 0;
const int TF_SRGB = 1;
const int TF_LINEAR = 2;
const int TF_GAMMA22 = 3;
const int TF_GAMMA28 = 4;
const int TF_PQ = 5;  // I.9, managed pipelines only: toWorking holds the scale (nits / 10000 -> working)
const int TF_HLG = 6; // I.9: hlgToDisplay, then toWorking (with the peak and scale)
const int TF_LUT = 7; // I.9: inLut (code values -> working), toWorking unused
const int TF_PQ_SDR = 8;  // I.9 (gaming): a PQ capture tone-mapped into SDR (captureToSdr; toWorking: Rec.2020 -> 709)
const int TF_HLG_SDR = 9; // I.9 (gaming): an HLG capture, the same (hlgToDisplay with cm.x for a 1000-nit display)
const int TF_LUT_SDR = 10; // 0.2, Direct Rec.709: camera log - inLut (scene light), tone-mapped as a capture
// mode.z
const int ALPHA_NONE = 0;
const int ALPHA_PREMULTIPLIED = 2;
const int ALPHA_INVERT = 4; // P.2: a flag on the mode (Interpret Footage > Invert Alpha Channel)
// mode.w: 1 = a float source (a nested composite's canvas, I.9): values above 1 are kept, nothing is clamped to [0, 1]

float toLinear(float v, int tf)
{
    if (tf == TF_SRGB)
        return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    if (tf == TF_LINEAR)
        return v;
    if (tf == TF_GAMMA22)
        return pow(v, 2.2);
    if (tf == TF_GAMMA28)
        return pow(v, 2.8);
    const float a = 1.09929682680944;
    const float b = 0.018053968510807;
    return v < 4.5 * b ? v / 4.5 : pow((v + (a - 1.0)) / a, 1.0 / 0.45);
}

// Colour management (I.9, ColorPipeline v2; mirrors color::runInputPlan): BT.2100 PQ and HLG, and a camera log / ACES
// input baked into a 3D LUT indexed by code values (inLut, tetrahedral like color::BakedLut::sample).
float pqToLinear(float v) // nits / 10000
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float e = pow(clamp(v, 0.0, 1.0), 1.0 / m2);
    return pow(max(e - c1, 0.0) / (c2 - c3 * e), 1.0 / m1);
}

float hlgInverseOetf(float v)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    v = max(v, 0.0);
    return v <= 0.5 ? v * v / 3.0 : (exp((v - c) / a) + b) / 12.0;
}

// HLG signal -> display light / peak (the OOTF with system gamma cm.x + 1, luminance in Rec.2020).
vec3 hlgToDisplay(vec3 v)
{
    vec3 e = vec3(hlgInverseOetf(v.r), hlgInverseOetf(v.g), hlgInverseOetf(v.b));
    float ys = dot(e, vec3(0.2627, 0.6780, 0.0593));
    return ys > 0.0 ? e * pow(ys, p.cm.x) : vec3(0.0);
}

// I.9 (gaming): an HDR capture (PQ / HLG) in an SDR project, tone-mapped into SDR (color::hdrCaptureToSdr ->
// color::toneMapToSdr): BT.2390 EETF on the Rec.2020 luminance from the 1000-nit master (cm.w) into [0, reference white
// (cm.z)], the colour scaled along, Rec.2020 -> Rec.709 (toWorking), clipped to [0, 1].
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

vec3 captureToSdr(vec3 nits)
{
    float white = p.cm.z;
    float peak = p.cm.w;
    vec3 c = max(nits, vec3(0.0)) / white;
    float y = dot(c, vec3(0.2627, 0.6780, 0.0593));
    float ratio = 1.0;
    if (y > 0.0 && peak > white) {
        float lum = y * white;
        float pqPeak = nitsToPq(peak);
        float e1 = min(nitsToPq(lum) / pqPeak, 1.0);
        float maxLum = nitsToPq(white) / pqPeak;
        float ks = 1.5 * maxLum - 0.5;
        float e2 = e1;
        if (e1 >= ks && ks < 1.0) {
            float t = (e1 - ks) / (1.0 - ks);
            float t2 = t * t;
            float t3 = t2 * t;
            e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxLum;
        }
        ratio = lum > peak ? white / lum : pqToLinear(e2 * pqPeak) * 10000.0 / lum;
    }
    return clamp(mat3(p.toWorking) * (c * ratio), 0.0, 1.0);
}

vec3 inputLut(vec3 x)
{
    int n = int(p.cm.y);
    float last = float(n - 1);
    ivec3 i0;
    vec3 f;
    for (int c = 0; c < 3; ++c) {
        float t = clamp(x[c], 0.0, 1.0) * last;
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
    return w0 * texelFetch(inLut, i0, 0).rgb + wa * texelFetch(inLut, i0 + da, 0).rgb + wb * texelFetch(inLut, i0 + db, 0).rgb +
           w1 * texelFetch(inLut, i0 + ivec3(1, 1, 1), 0).rgb;
}

// 0.2 field options (model/FieldOptions.h, CompositeLayer::fieldMode in mode.z bits 3-4): an interlaced frame shown from
// one field - the kept field's rows as they are, the other field's rows the mean of the kept rows above and below
// (Premiere's "interpolates the missing lines from the dominant field") - or every row blended [1 2 1] / 4 with its
// neighbours (Flicker Removal). Rows are the plane's own texels; the result is then filtered like a plain sample.
const int FIELD_KEEP_UPPER = 1;
const int FIELD_KEEP_LOWER = 2;
const int FIELD_BLEND = 3;

vec4 planeRow(sampler2D tex, float u, int row, int height)
{
    row = clamp(row, 0, height - 1);
    return texture(tex, vec2(u, (float(row) + 0.5) / float(height)));
}

vec4 fieldRow(sampler2D tex, float u, int row, int height, int fieldMode)
{
    row = clamp(row, 0, height - 1);
    if (fieldMode == FIELD_BLEND)
        return 0.25 * planeRow(tex, u, row - 1, height) + 0.5 * planeRow(tex, u, row, height) +
               0.25 * planeRow(tex, u, row + 1, height);
    int kept = fieldMode == FIELD_KEEP_UPPER ? 0 : 1;
    if ((row & 1) == kept)
        return planeRow(tex, u, row, height);
    if (row == 0)
        return planeRow(tex, u, 1, height);
    if (row + 1 >= height)
        return planeRow(tex, u, row - 1, height);
    return 0.5 * (planeRow(tex, u, row - 1, height) + planeRow(tex, u, row + 1, height));
}

vec4 samplePlane(sampler2D tex, vec2 uv, int fieldMode)
{
    if (fieldMode == 0)
        return texture(tex, uv);
    int height = textureSize(tex, 0).y;
    float y = uv.y * float(height) - 0.5;
    float r0 = floor(y);
    vec4 a = fieldRow(tex, uv.x, int(r0), height, fieldMode);
    vec4 b = fieldRow(tex, uv.x, int(r0) + 1, height, fieldMode);
    return mix(a, b, y - r0);
}

void main()
{
    vec2 cuv = v_uv * p.chromaXform.xy + p.chromaXform.zw;
    vec4 s = vec4(0.0, 0.0, 0.0, 1.0);
    int planeLayout = p.mode.x;
    int fieldMode = (p.mode.z >> 3) & 3;
    if (planeLayout == LAYOUT_SEMI_PLANAR) {
        s.x = samplePlane(plane0, v_uv, fieldMode).r;
        s.yz = samplePlane(plane1, cuv, fieldMode).rg;
    } else if (planeLayout == LAYOUT_PLANAR3 || planeLayout == LAYOUT_PLANAR4) {
        s.x = samplePlane(plane0, v_uv, fieldMode).r;
        s.y = samplePlane(plane1, cuv, fieldMode).r;
        s.z = samplePlane(plane2, cuv, fieldMode).r;
        if (planeLayout == LAYOUT_PLANAR4)
            s.w = samplePlane(plane3, v_uv, fieldMode).r * p.params.y;
    } else {
        s = samplePlane(plane0, v_uv, fieldMode);
    }

    int alphaMode = p.mode.z & 3; // bit 2: ALPHA_INVERT, bits 3-4: the field mode
    float a = alphaMode == ALPHA_NONE ? 1.0 : clamp(s.w, 0.0, 1.0);
    vec3 rgb = (p.sampleToRgb * vec4(s.xyz, 1.0)).rgb;
    if (p.mode.w == 1) {
        if (alphaMode == ALPHA_PREMULTIPLIED)
            rgb = a > 0.0 ? max(rgb / a, vec3(0.0)) : vec3(0.0);
    } else {
        rgb = clamp(rgb, 0.0, 1.0);
        if (alphaMode == ALPHA_PREMULTIPLIED)
            rgb = a > 0.0 ? clamp(rgb / a, 0.0, 1.0) : vec3(0.0); // un-premultiply before linearizing
    }
    if ((p.mode.z & ALPHA_INVERT) != 0)
        a = 1.0 - a; // P.2: Interpret Footage > Invert Alpha Channel (the colour was un-premultiplied above)
    int tf = p.mode.y;
    vec3 lin;
    if (tf == TF_LUT)
        lin = sinh(inputLut(rgb)); // the texels hold asinh(working) (color::BakedLut::Values::Asinh)
    else if (tf == TF_LUT_SDR)
        lin = captureToSdr(sinh(inputLut(rgb)) * p.cm.z); // scene light (1.0 = white) as nits, then the capture tone map
    else if (tf == TF_PQ_SDR)
        lin = captureToSdr(vec3(pqToLinear(rgb.r), pqToLinear(rgb.g), pqToLinear(rgb.b)) * 10000.0);
    else if (tf == TF_HLG_SDR)
        lin = captureToSdr(hlgToDisplay(rgb) * p.cm.w);
    else if (tf == TF_HLG)
        lin = mat3(p.toWorking) * hlgToDisplay(rgb);
    else if (tf == TF_PQ)
        lin = mat3(p.toWorking) * vec3(pqToLinear(rgb.r), pqToLinear(rgb.g), pqToLinear(rgb.b));
    else
        lin = mat3(p.toWorking) * vec3(toLinear(rgb.r, tf), toLinear(rgb.g, tf), toLinear(rgb.b, tf));

    // Analytic edge coverage: distance to the frame edge in canvas pixels.
    vec2 d = min(v_src, p.srcRect.xy - v_src);
    vec2 w = max(fwidth(v_src), vec2(1e-6));
    vec2 cov = clamp(d / w + 0.5, 0.0, 1.0);
    float k = a * cov.x * cov.y * p.params.x;
    fragColor = vec4(max(lin, vec3(0.0)) * k, k);
}
