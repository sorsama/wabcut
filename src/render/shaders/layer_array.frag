#version 440

// Layer pass, zero-copy variant: the planes are texture-array views of a decoder surface (D3D11 NV12/P010 slice),
// sampled at layer 0. Same colour math as layer.frag (semi-planar layouts only, no alpha).

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
    vec4 cm; // I.9 (gaming): x = HLG system gamma - 1, z = an HDR capture's reference white, w = its peak (nits)
} p;

layout(binding = 1) uniform sampler2DArray plane0; // luma view (R8/R16)
layout(binding = 2) uniform sampler2DArray plane1; // chroma view (RG8/RG16)

// mode.y
const int TF_BT709 = 0;
const int TF_SRGB = 1;
const int TF_LINEAR = 2;
const int TF_GAMMA22 = 3;
const int TF_GAMMA28 = 4;
const int TF_PQ_SDR = 8;  // I.9 (gaming): a PQ capture tone-mapped into SDR (captureToSdr; toWorking: Rec.2020 -> 709)
const int TF_HLG_SDR = 9; // I.9 (gaming): an HLG capture, the same (hlgToDisplay with cm.x for a 1000-nit display)
// mode.z
const int ALPHA_NONE = 0;
const int ALPHA_PREMULTIPLIED = 2;

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

// BT.2100 PQ and HLG (I.9, gaming: HDR captures), mirroring color/Hdr.h.
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

void main()
{
    vec2 cuv = v_uv * p.chromaXform.xy + p.chromaXform.zw;
    vec4 s = vec4(0.0, 0.0, 0.0, 1.0);
    s.x = texture(plane0, vec3(v_uv, 0.0)).r;
    s.yz = texture(plane1, vec3(cuv, 0.0)).rg;

    float a = p.mode.z == ALPHA_NONE ? 1.0 : clamp(s.w, 0.0, 1.0);
    vec3 rgb = clamp((p.sampleToRgb * vec4(s.xyz, 1.0)).rgb, 0.0, 1.0);
    if (p.mode.z == ALPHA_PREMULTIPLIED)
        rgb = a > 0.0 ? clamp(rgb / a, 0.0, 1.0) : vec3(0.0); // un-premultiply before linearizing
    int tf = p.mode.y;
    vec3 lin;
    if (tf == TF_PQ_SDR)
        lin = captureToSdr(vec3(pqToLinear(rgb.r), pqToLinear(rgb.g), pqToLinear(rgb.b)) * 10000.0);
    else if (tf == TF_HLG_SDR)
        lin = captureToSdr(hlgToDisplay(rgb) * p.cm.w);
    else
        lin = mat3(p.toWorking) * vec3(toLinear(rgb.r, tf), toLinear(rgb.g, tf), toLinear(rgb.b, tf));

    // Analytic edge coverage: distance to the frame edge in canvas pixels.
    vec2 d = min(v_src, p.srcRect.xy - v_src);
    vec2 w = max(fwidth(v_src), vec2(1e-6));
    vec2 cov = clamp(d / w + 0.5, 0.0, 1.0);
    float k = a * cov.x * cov.y * p.params.x;
    fragColor = vec4(max(lin, vec3(0.0)) * k, k);
}
