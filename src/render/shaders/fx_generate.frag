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
// 0.6: CPU twins in LibraryGenerate.cpp; Lightning and Scribble read their strokes (LibraryGeometry.h packStrokes)
// from tex1, Paint Bucket runs in stages (mode.w, Compositor applyLibrary).
const int OP_COLORAMA = 115;
const int OP_FRACTAL_NOISE = 116;
const int OP_CELL_PATTERN = 117;
const int OP_LIGHTNING = 118;
const int OP_SCRIBBLE = 119;
const int OP_PAINT_BUCKET = 120;
const int OP_DEPTH_BLUR = 121; // 0.7: AI Refocus
const int OP_SKIN_SMOOTHING = 122; // 0.7: Skin Smoothing (CPU twin: render/reference/LibraryRetouch.cpp)
const int OP_FACE_RETOUCH = 123;   // 0.7: AI Face Retouch (tex1: R skin, G mouth, B eyes as linear coverage)

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

// ---- 0.6: Colorama ----

vec3 hls(vec3 e)
{
    float mx = max(e.r, max(e.g, e.b));
    float mn = min(e.r, min(e.g, e.b));
    float l = 0.5 * (mx + mn);
    float d = mx - mn;
    if (d <= 0.0)
        return vec3(0.0, l, 0.0);
    float s = d / max(1.0 - abs(2.0 * l - 1.0), 1e-6);
    float h;
    if (mx == e.r)
        h = (e.g - e.b) / d;
    else if (mx == e.g)
        h = (e.b - e.r) / d + 2.0;
    else
        h = (e.r - e.g) / d + 4.0;
    return vec3(fract(h / 6.0), l, clamp(s, 0.0, 1.0));
}

// Use Preset Palette 1..7, five encoded colours each at 0, 1/4, 1/2, 3/4, 1.
const vec3 PALETTES[35] = vec3[35](
    vec3(0.0, 0.0, 0.0), vec3(0.25, 0.25, 0.25), vec3(0.5, 0.5, 0.5), vec3(0.75, 0.75, 0.75), vec3(1.0, 1.0, 1.0),
    vec3(0.0, 0.0, 0.35), vec3(0.0, 0.45, 0.9), vec3(0.2, 0.85, 0.45), vec3(1.0, 0.85, 0.1), vec3(1.0, 1.0, 1.0),
    vec3(0.1, 0.05, 0.0), vec3(0.5, 0.3, 0.05), vec3(0.9, 0.65, 0.2), vec3(1.0, 0.85, 0.5), vec3(1.0, 1.0, 0.9),
    vec3(0.0, 0.0, 0.0), vec3(0.5, 0.0, 0.0), vec3(1.0, 0.3, 0.0), vec3(1.0, 0.8, 0.1), vec3(1.0, 1.0, 0.8),
    vec3(0.0, 0.0, 0.1), vec3(0.0, 0.2, 0.5), vec3(0.3, 0.6, 0.9), vec3(0.7, 0.9, 1.0), vec3(1.0, 1.0, 1.0),
    vec3(1.0, 1.0, 1.0), vec3(0.75, 0.75, 0.75), vec3(0.5, 0.5, 0.5), vec3(0.25, 0.25, 0.25), vec3(0.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0), vec3(0.8, 1.0, 0.0), vec3(0.0, 1.0, 0.6), vec3(0.3, 0.0, 1.0), vec3(1.0, 0.0, 0.0));

vec3 coloramaAt(float t)
{
    float pos[5];
    vec3 col[5];
    int n = 5;
    int palette = clamp(int(floor(q[3] + 0.5)), 0, 7);
    if (palette == 0) {
        n = clamp(int(floor(q[4] + 0.5)), 2, 5);
        for (int k = 0; k < 5; ++k) {
            int o = 5 + 4 * k;
            pos[k] = q[o];
            col[k] = clamp(vec3(q[o + 1], q[o + 2], q[o + 3]), 0.0, 1.0);
        }
        for (int i = 1; i < 5; ++i) {
            if (i >= n)
                break;
            for (int j = i; j > 0; --j) {
                if (!(pos[j] < pos[j - 1]))
                    break;
                float tp = pos[j];
                pos[j] = pos[j - 1];
                pos[j - 1] = tp;
                vec3 tc = col[j];
                col[j] = col[j - 1];
                col[j - 1] = tc;
            }
        }
    } else {
        for (int k = 0; k < 5; ++k) {
            pos[k] = 0.25 * float(k);
            col[k] = PALETTES[(palette - 1) * 5 + k];
        }
    }
    if (t <= pos[0])
        return col[0];
    for (int k = 0; k < 4; ++k) {
        if (k + 1 >= n)
            break;
        float p0 = pos[k];
        float p1 = pos[k + 1];
        if (t <= p1) {
            float u = (t - p0) / max(p1 - p0, 1e-6);
            if (q[25] < 0.5)
                return u < 0.5 ? col[k] : col[k + 1];
            return mix(col[k], col[k + 1], u);
        }
    }
    return col[n - 1];
}

// ---- 0.6: Fractal Noise and Cell Pattern ----

float latticeValue(int x, int y, int z, uint seed)
{
    return 2.0 * hash01(uint(x) + 2654435761u * uint(y), uint(z) + seed * 1013u, 11u) - 1.0;
}

float catmullRom(float p0, float p1, float p2, float p3, float t)
{
    return 0.5 * (2.0 * p1 + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t
                  + (3.0 * p1 - p0 - 3.0 * p2 + p3) * t * t * t);
}

// One layer of noise at lattice depth z by Noise Type (Block, Linear, Soft Linear, Spline).
float noisePlane(vec2 v, int z, int type, uint seed)
{
    vec2 i = floor(v);
    vec2 f = v - i;
    int x = int(i.x);
    int y = int(i.y);
    if (type == 0)
        return latticeValue(x, y, z, seed);
    if (type == 3) {
        float rows[4];
        for (int j = 0; j < 4; ++j)
            rows[j] = catmullRom(latticeValue(x - 1, y + j - 1, z, seed), latticeValue(x, y + j - 1, z, seed),
                                 latticeValue(x + 1, y + j - 1, z, seed), latticeValue(x + 2, y + j - 1, z, seed), f.x);
        return clamp(catmullRom(rows[0], rows[1], rows[2], rows[3], f.y), -1.0, 1.0);
    }
    vec2 w = type == 1 ? f : f * f * (3.0 - 2.0 * f);
    return mix(mix(latticeValue(x, y, z, seed), latticeValue(x + 1, y, z, seed), w.x),
               mix(latticeValue(x, y + 1, z, seed), latticeValue(x + 1, y + 1, z, seed), w.x), w.y);
}

float noiseTyped(vec2 v, float vz, int type, int period, uint seed)
{
    float iz = floor(vz);
    float fz = vz - iz;
    int z0 = int(iz);
    int z1 = z0 + 1;
    if (period > 0) {
        z0 = z0 - period * int(floor(float(z0) / float(period)));
        z1 = z1 - period * int(floor(float(z1) / float(period)));
    }
    float wz = fz * fz * (3.0 - 2.0 * fz);
    return mix(noisePlane(v, z0, type, seed), noisePlane(v, z1, type, seed), wz);
}

// Contrast, Overflow (Clip, Soft Clamp, Wrap Back) and Invert of a noise value.
float shapeValue(float v, float contrast, float brightness, int overflow, bool invert)
{
    v = (v - 0.5) * contrast / 100.0 + 0.5 + brightness / 100.0;
    if (overflow == 1) {
        v = 1.0 / (1.0 + exp(-4.0 * (v - 0.5)));
    } else if (overflow == 2) {
        float w = v - 2.0 * floor(v / 2.0);
        v = w <= 1.0 ? w : 2.0 - w;
    } else {
        v = clamp(v, 0.0, 1.0);
    }
    return invert ? 1.0 - v : v;
}

vec4 drawGrey(int mode, vec4 dst, float v, float opacity)
{
    float g = eotf(clamp(v, 0.0, 1.0));
    float k = clamp(opacity, 0.0, 1.0);
    vec4 src = vec4(vec3(g * k), k);
    return mode == 0 ? src : blendOver(mode, dst, src);
}

float fractalNoiseAt(vec2 f, vec2 s)
{
    float cell = max(q[7], 1.0);
    float theta = radians(q[6]);
    vec2 pp = f - vec2(q[8], q[9]) * s;
    vec2 v = vec2(cos(theta) * pp.x + sin(theta) * pp.y, -sin(theta) * pp.x + cos(theta) * pp.y) / cell;
    int period = q[15] > 0.5 ? max(1, int(floor(q[16] + 0.5))) : 0;
    float z = q[14] / 360.0;
    if (period > 0)
        z = z - float(period) * floor(z / float(period));
    int fractal = clamp(int(floor(q[0] + 0.5)), 0, 3);
    int type = clamp(int(floor(q[1] + 0.5)), 0, 3);
    float complexity = clamp(q[10], 1.0, 20.0);
    int octaves = int(floor(complexity));
    float last = complexity - float(octaves);
    float influence = clamp(q[11], 0.0, 1.0);
    float subScale = 1.0 / clamp(q[12], 0.25, 1.0);
    float subRotation = radians(q[13]);
    uint seed = uint(clamp(floor(q[17] + 0.5), 0.0, 1000000.0));
    float sum = 0.0;
    float norm = 0.0;
    float amp = 1.0;
    float freq = 1.0;
    for (int o = 0; o < 21; ++o) {
        float weight = o < octaves ? 1.0 : (o == octaves ? last : 0.0);
        if (weight <= 0.0)
            break;
        float r = subRotation * float(o);
        vec2 ov = vec2(cos(r) * v.x + sin(r) * v.y, -sin(r) * v.x + cos(r) * v.y) * freq + vec2(7.31, 3.17) * float(o);
        if (fractal == 2)
            ov += vec2(0.37, 0.21) * float(o) * z;
        float n = noiseTyped(ov, z, type, period, seed + uint(o) * 101u);
        if (fractal == 1)
            n = abs(n);
        else if (fractal == 3)
            n = n * n;
        sum += weight * amp * n;
        norm += weight * amp;
        amp *= influence;
        freq *= subScale;
    }
    float value = norm > 0.0 ? sum / norm : 0.0;
    float v01 = fractal == 1 || fractal == 3 ? value : 0.5 + 0.5 * value;
    return shapeValue(v01, q[3], q[4], clamp(int(floor(q[5] + 0.5)), 0, 2), q[2] > 0.5);
}

float cellPatternAt(vec2 f, vec2 s)
{
    float size = max(q[5], 2.0);
    vec2 u = (f - vec2(q[6], q[7]) * s) / size;
    vec2 i = floor(u);
    float disperse = clamp(q[4], 0.0, 1.5);
    float evolution = radians(q[9]);
    bool cycle = q[10] > 0.5;
    float cycleSpeed = 1.0 / float(max(1, int(floor(q[11] + 0.5))));
    uint seed = uint(clamp(floor(q[12] + 0.5), 0.0, 1000000.0));
    float f1 = 1e9;
    float f2 = 1e9;
    float nearHash = 0.0;
    float nearStatic = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = int(i.x) + dx;
            int ny = int(i.y) + dy;
            uint key = uint(nx) + 2654435761u * uint(ny);
            float h1 = hash01(key, seed * 1013u + 5u, 21u);
            float h2 = hash01(key, seed * 1013u + 6u, 21u);
            float h3 = hash01(key, seed * 1013u + 7u, 21u);
            float h4 = hash01(key, seed * 1013u + 8u, 21u);
            float a = 2.0 * PI * h3 + evolution * (cycle ? cycleSpeed : 0.75 + 0.5 * h4);
            vec2 feature = vec2(float(nx), float(ny)) + 0.5 + disperse * (vec2(h1, h2) - 0.5)
                         + 0.25 * disperse * vec2(cos(a), sin(a));
            float d = length(u - feature);
            if (d < f1) {
                f2 = f1;
                f1 = d;
                nearHash = h3;
                nearStatic = h4;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    float edge = f2 - f1;
    float r = f1 / max(0.5 * (f1 + f2), 1e-6);
    int pattern = clamp(int(floor(q[0] + 0.5)), 0, 6);
    float v;
    if (pattern == 0)
        v = sqrt(max(0.0, 1.0 - r * r));
    else if (pattern == 1)
        v = clamp(edge, 0.0, 1.0);
    else if (pattern == 2)
        v = 0.5 + 0.5 * sin(2.0 * PI * nearHash + evolution);
    else if (pattern == 3)
        v = nearStatic;
    else if (pattern == 4)
        v = nearStatic * (0.4 + 0.6 * clamp(edge * 4.0, 0.0, 1.0));
    else if (pattern == 5)
        v = clamp(2.0 * edge / max(f2, 1e-6), 0.0, 1.0);
    else
        v = 1.0 - clamp(edge * 3.0, 0.0, 1.0);
    if (q[8] > 0.0)
        v *= smoothstep(0.0, 0.5 * q[8], edge);
    return shapeValue(v, q[2], 0.0, clamp(int(floor(q[3] + 0.5)), 0, 2), q[1] > 0.5);
}

// ---- 0.6: Lightning and Scribble: the strokes in tex1 (LibraryGeometry.h packStrokes) ----

vec4 dataTexel(int i)
{
    return texelFetch(tex1, ivec2(i % 1024, i / 1024), 0);
}

// {inside (core or line coverage), outside (glow)} at frame position f: only the segments of f's grid cell.
vec2 strokeShade(vec2 f)
{
    vec4 h0 = dataTexel(0);
    int count = int(h0.x + 0.5);
    if (count <= 0)
        return vec2(0.0);
    int gw = int(h0.y + 0.5);
    int gh = int(h0.z + 0.5);
    int firstIndex = int(h0.w + 0.5);
    vec4 h1 = dataTexel(1);
    vec4 h2 = dataTexel(2);
    int segmentBase = int(h2.x + 0.5);
    int kind = int(h2.y + 0.5);
    float core = h2.z;
    ivec2 cell = ivec2(floor((f - h1.xy) / h1.zw));
    if (cell.x < 0 || cell.y < 0 || cell.x >= gw || cell.y >= gh)
        return vec2(0.0);
    vec4 entry = dataTexel(3 + cell.y * gw + cell.x);
    int start = int(entry.x + 0.5);
    int n = int(entry.y + 0.5);
    const float CUT = 0.011108996538242306; // exp(-4.5): the glow at 3 sigma, subtracted so it ends at 0
    float inside = 0.0;
    float outside = 0.0;
    for (int k = 0; k < n; ++k) {
        int idx = start + k;
        vec4 group = dataTexel(firstIndex + idx / 4);
        int c = idx % 4;
        float which = c == 0 ? group.x : (c == 1 ? group.y : (c == 2 ? group.z : group.w));
        int segment = int(which + 0.5);
        vec4 ab = dataTexel(segmentBase + 2 * segment);
        vec4 ww = dataTexel(segmentBase + 2 * segment + 1);
        vec2 v = ab.zw - ab.xy;
        float len2 = dot(v, v);
        float t = len2 > 0.0 ? clamp(dot(f - ab.xy, v) / len2, 0.0, 1.0) : 0.0;
        float d = length(f - (ab.xy + t * v));
        float w = ww.x + (ww.y - ww.x) * t;
        if (kind == 0) {
            inside = max(inside, clamp(0.5 * w - d + 0.5, 0.0, 1.0));
            continue;
        }
        if (core > 0.0 && w > 0.0)
            inside = max(inside, clamp(0.5 * w * core - d + 0.5, 0.0, 1.0));
        float sigma = 0.5 * w;
        if (sigma > 0.0 && d < 3.0 * sigma)
            outside = max(outside, (exp(-d * d / (2.0 * sigma * sigma)) - CUT) / (1.0 - CUT));
    }
    return vec2(inside, outside);
}

// ---- 0.6: Paint Bucket ----

const ivec2 BUCKET_DIRS[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));

// The fill point's texel in tex0's coordinates (q[29], q[30]: its canvas texel from the top; q[31] = 1 when inside).
ivec2 bucketSeed()
{
    int sy = int(q[30] + 0.5);
    return ivec2(int(q[29] + 0.5), p.mode.y == 1 ? int(p.texel.w + 0.5) - 1 - sy : sy);
}

bool bucketInside(ivec2 t)
{
    return t.x >= 0 && t.y >= 0 && t.x < int(p.texel.z + 0.5) && t.y < int(p.texel.w + 0.5);
}

bool bucketSimilar(vec4 c, vec4 seedColor)
{
    if (q[31] < 0.5)
        return false;
    vec3 e = encoded(c);
    vec3 es = encoded(seedColor);
    float a = clamp(c.a, 0.0, 1.0);
    float as = clamp(seedColor.a, 0.0, 1.0);
    vec3 dd = abs(e - es);
    float dc = max(dd.r, max(dd.g, dd.b));
    int sel = clamp(int(floor(q[2] + 0.5)), 0, 4);
    float d = sel == 0 ? max(dc, abs(a - as)) : (sel == 1 ? dc : (sel == 2 ? a : (sel == 3 ? 1.0 - a : abs(a - as))));
    return d * 255.0 <= q[3];
}

// ---- 0.7: Skin Smoothing and AI Face Retouch ------------------------------------------------------------------------------
// Both work on the straight colour encoded with the BT.709 curve and write back only the change (retouchDelta), so a pixel
// they leave alone stays bit-exact (also above 1.0). The CPU twin, render/reference/LibraryRetouch.cpp, has the same
// constants, tap layouts and formulas; docs/effects.md describes the controls.
const float RETOUCH_GOLDEN = 2.39996323;
const float RETOUCH_RANGE_SIGMA = 0.08; // the smoothing's colour tolerance (distance in encoded RGB)

// How skin-toned an encoded colour is, 0..1: the angle of (Cb, Cr) (BT.709) near `tone` degrees, enough chroma (not grey, not
// a saturated primary) and a luma that is neither black nor white.
float skinTone(vec3 e, float tone, float halfWidth, float soft)
{
    float y = luma(e);
    float cb = (e.b - y) / 1.8556;
    float cr = (e.r - y) / 1.5748;
    float chroma = length(vec2(cb, cr));
    float angle = degrees(atan(cr, cb));
    if (angle < 0.0)
        angle += 360.0;
    float d = abs(angle - tone);
    d = min(d, 360.0 - d);
    float edge = mix(3.0, 20.0, soft);
    float wHue = 1.0 - smoothstep(max(halfWidth - edge, 0.0), halfWidth + edge, d);
    float wChroma = smoothstep(0.015, 0.045, chroma) * (1.0 - smoothstep(0.16, 0.26, chroma));
    float wLuma = smoothstep(0.08, 0.2, y) * (1.0 - smoothstep(0.93, 0.995, y));
    return wHue * wChroma * wLuma;
}

// Applies the change from encoded `from` to `to` to the layer's premultiplied linear pixel.
vec4 retouchDelta(vec4 in0, vec3 from, vec3 to)
{
    if (to == from)
        return in0;
    return vec4(in0.rgb + (eotf3(to) - eotf3(from)) * in0.a, in0.a);
}

// The skin weight of the tap at `uv`: from the face map (fromMap) or from its own colour.
float retouchSkinAt(vec2 uv, vec4 tap, vec3 e, bool fromMap, float tone, float halfWidth, float soft)
{
    if (tap.a <= 0.0)
        return 0.0;
    return fromMap ? clamp(textureLod(tex1, uv, 0.0).r, 0.0, 1.0) : skinTone(e, tone, halfWidth, soft);
}

// An edge-preserving smoothing of the skin around the pixel: 24 taps on a Vogel disc of `radius` texels, each weighted by a
// Gaussian of its distance (sigma radius / 2), by how close its colour is to `c0` and by its own skin weight. rgb = the
// smoothed colour (c0 where no skin is around), a = the skin weight averaged over the disc (the pixel counts as a tap).
vec4 smoothSkin(vec3 c0, float s0, float radius, bool fromMap, float tone, float halfWidth, float soft)
{
    vec2 base = v_uv * p.texel.zw;
    vec3 num = c0 * s0;
    float den = s0;
    float skinSum = s0;
    float weightSum = 1.0;
    for (int i = 0; i < 24; ++i) {
        float rr = sqrt((float(i) + 0.5) / 24.0) * radius;
        float th = float(i) * RETOUCH_GOLDEN;
        vec2 cell = clamp(floor(base + vec2(cos(th), sin(th)) * rr), vec2(0.0), p.texel.zw - 1.0);
        vec2 uv = (cell + 0.5) * p.texel.xy;
        vec4 tap = textureLod(tex0, uv, 0.0);
        vec3 e = encoded(tap);
        float s = retouchSkinAt(uv, tap, e, fromMap, tone, halfWidth, soft);
        float ws = exp(-2.0 * (rr / radius) * (rr / radius));
        vec3 dc = e - c0;
        float ws2 = ws * s;
        float wr = ws2 * exp(-dot(dc, dc) / (2.0 * RETOUCH_RANGE_SIGMA * RETOUCH_RANGE_SIGMA));
        num += wr * e;
        den += wr;
        skinSum += ws2;
        weightSum += ws;
    }
    return vec4(den > 1e-6 ? num / den : c0, skinSum / weightSum);
}

// Spot healing: a robust average of the skin around the pixel (16 taps on a disc of `radius` texels; the first average is
// averaged again without the taps far from it, and the pixel is only healed where most of the skin agrees on one colour), and where the pixel and the pixel smoothed over its 4 neighbours both differ
// from it by more than a threshold, but not by a feature's worth, the pixel moves to it, keeping its own fine texture.
vec3 healBlemish(vec3 c0, float strength, float radius)
{
    vec2 base = v_uv * p.texel.zw;
    vec3 colors[16];
    float weights[16];
    vec3 sum1 = vec3(0.0);
    float tot1 = 0.0;
    for (int i = 0; i < 16; ++i) {
        float rr = sqrt((float(i) + 0.5) / 16.0) * radius;
        float th = float(i) * RETOUCH_GOLDEN;
        vec2 cell = clamp(floor(base + vec2(cos(th), sin(th)) * rr), vec2(0.0), p.texel.zw - 1.0);
        vec2 uv = (cell + 0.5) * p.texel.xy;
        vec4 tap = textureLod(tex0, uv, 0.0);
        vec3 e = encoded(tap);
        float w = tap.a > 0.0 ? clamp(textureLod(tex1, uv, 0.0).r, 0.0, 1.0) : 0.0;
        colors[i] = e;
        weights[i] = w;
        sum1 += w * e;
        tot1 += w;
    }
    if (tot1 < 1e-4)
        return c0;
    vec3 mean1 = sum1 / tot1;
    vec3 sum2 = vec3(0.0);
    float tot2 = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec3 d = colors[i] - mean1;
        float w = weights[i] * exp(-dot(d, d) / (2.0 * 0.08 * 0.08));
        sum2 += w * colors[i];
        tot2 += w;
    }
    vec3 mean = tot2 > 1e-4 ? sum2 / tot2 : mean1;
    // How much of the skin round the pixel agrees on one colour: a spot in clean skin leaves most of it, the edge of a feature (a
    // nostril, a shadow) leaves two colours and nothing that is "the skin round it": then nothing is healed.
    float agree = smoothstep(0.30, 0.65, tot2 / tot1);
    vec2 cell0 = floor(base);
    vec3 low = c0;
    for (int k = 0; k < 4; ++k) {
        vec2 off = k == 0 ? vec2(1.0, 0.0) : k == 1 ? vec2(-1.0, 0.0) : k == 2 ? vec2(0.0, 1.0) : vec2(0.0, -1.0);
        vec2 cell = clamp(cell0 + off, vec2(0.0), p.texel.zw - 1.0);
        low += encoded(textureLod(tex0, (cell + 0.5) * p.texel.xy, 0.0));
    }
    low /= 5.0;
    float dev = min(length(c0 - mean), length(low - mean));
    float t0 = mix(0.10, 0.025, strength);
    float b = smoothstep(t0, t0 + 0.06, dev) * (1.0 - smoothstep(0.30, 0.50, dev)) * agree;
    float f = b * clamp(strength * 1.5, 0.0, 1.0);
    return mix(c0, mean + clamp(c0 - low, vec3(-0.04), vec3(0.04)), f);
}

vec4 skinSmoothing(vec4 in0)
{
    float amount = clamp(q[0] / 100.0, 0.0, 1.0);
    float detail = clamp(q[1] / 100.0, 0.0, 1.0);
    float radius = max(q[2], 0.0) * p.texel.z / p.frame.z;
    float tone = q[3];
    float halfWidth = mix(8.0, 45.0, clamp(q[4] / 100.0, 0.0, 1.0));
    float soft = clamp(q[5] / 100.0, 0.0, 1.0);
    int view = int(floor(q[6] + 0.5));
    if (view != 1 && (amount <= 0.0 || radius < 0.5))
        return in0;
    vec3 c0 = encoded(in0);
    float s0 = in0.a > 0.0 ? skinTone(c0, tone, halfWidth, soft) : 0.0;
    if (view != 1 && s0 <= 0.0)
        return in0; // not skin: not changed, whatever is round it (the taps would only weigh it by s0 = 0)
    vec4 sm = smoothSkin(c0, s0, max(radius, 0.5), false, tone, halfWidth, soft);
    float m = s0 * smoothstep(0.15, 0.75, sm.a);
    if (view == 1)
        return vec4(vec3(eotf(m)), 1.0);
    vec3 result = mix(c0, sm.rgb + detail * (c0 - sm.rgb), amount * m);
    return retouchDelta(in0, c0, result);
}

vec4 faceRetouch(vec4 in0, bool hasMap)
{
    if (!hasMap)
        return in0; // no face map yet: the picture as it is
    float smoothing = clamp(q[0] / 100.0, 0.0, 1.0);
    float detail = clamp(q[1] / 100.0, 0.0, 1.0);
    float scale = p.texel.z / p.frame.z;
    float radius = max(q[2], 0.0) * scale;
    float blemish = clamp(q[3] / 100.0, 0.0, 1.0);
    float blemishRadius = max(q[4], 0.0) * scale;
    float teeth = clamp(q[5] / 100.0, 0.0, 1.0);
    float eyes = clamp(q[6] / 100.0, 0.0, 1.0);
    int view = int(floor(q[7] + 0.5));
    vec3 map = clamp(textureLod(tex1, v_uv, 0.0).rgb, 0.0, 1.0);
    if (view >= 1 && view <= 3)
        return vec4(vec3(eotf(view == 1 ? map.r : view == 2 ? map.g : map.b)), 1.0);
    bool doSkin = map.r > 0.003 && in0.a > 0.0;
    bool doBlemish = doSkin && blemish > 0.0 && blemishRadius >= 0.5;
    bool doSmooth = doSkin && smoothing > 0.0 && radius >= 0.5;
    bool doTeeth = teeth > 0.0 && map.g > 0.003;
    bool doEyes = eyes > 0.0 && map.b > 0.003;
    if (!doBlemish && !doSmooth && !doTeeth && !doEyes)
        return in0;
    vec3 c0 = encoded(in0);
    vec3 c = c0;
    if (doBlemish)
        c = mix(c, healBlemish(c, blemish, blemishRadius), map.r); // by the pixel's own skin weight: a map's soft tail beyond the edge heals nothing
    if (doSmooth) {
        vec4 sm = smoothSkin(c, map.r, radius, true, 0.0, 0.0, 0.0);
        c = mix(c, sm.rgb + detail * (c - sm.rgb), smoothing * map.r);
    }
    float y = luma(c);
    float mx = max(c.r, max(c.g, c.b));
    float sat = mx > 0.0 ? (mx - min(c.r, min(c.g, c.b))) / mx : 0.0;
    if (doTeeth) {
        // The bright, barely coloured part of the mouth: teeth (the tongue, gums and shadows are darker or redder).
        float k = teeth * map.g * smoothstep(0.30, 0.50, y) * (1.0 - smoothstep(0.35, 0.60, sat));
        vec3 t = mix(c, vec3(y), 0.85 * k);
        c = t + (vec3(1.0) - t) * (0.28 * k);
    }
    if (doEyes) {
        // The whites of the eyes: bright and barely coloured (the iris, pupil, lashes and the red of the veins are left).
        float k = eyes * map.b * smoothstep(0.35, 0.55, y) * (1.0 - smoothstep(0.30, 0.55, sat));
        vec3 t = mix(c, vec3(y), 0.6 * k);
        c = t + (vec3(1.0) - t) * (0.25 * k);
    }
    return retouchDelta(in0, c0, clamp(c, 0.0, 1.0));
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
    } else if (op == OP_COLORAMA) {
        vec3 e = encoded(in0);
        float a = clamp(in0.a, 0.0, 1.0);
        vec3 h = hls(e);
        int src = clamp(int(floor(q[0] + 0.5)), 0, 7);
        float v = src == 0 ? (e.r + e.g + e.b) / 3.0
                : src == 1 ? h.y
                : src == 2 ? h.x
                : src == 3 ? h.z
                : src == 4 ? e.r
                : src == 5 ? e.g
                : src == 6 ? e.b
                           : a;
        float phase = v * max(q[2], 0.1) + q[1] / 360.0;
        float t = fract(phase);
        if (phase > 0.0 && t < 1e-6)
            t = 1.0;
        vec4 mapped = vec4(eotf3(coloramaAt(t)) * a, a);
        int mode = clamp(int(floor(q[27] + 0.5)), 0, 7);
        vec4 blended = mode == 0 ? mapped : blendOver(mode + 1, in0, mapped);
        fragColor = mix(blended, in0, clamp(q[26], 0.0, 1.0));
    } else if (op == OP_FRACTAL_NOISE) {
        fragColor = drawGrey(clamp(int(floor(q[19] + 0.5)), 0, 8), in0, fractalNoiseAt(f, s), q[18]);
    } else if (op == OP_CELL_PATTERN) {
        fragColor = drawGrey(clamp(int(floor(q[14] + 0.5)), 0, 8), in0, cellPatternAt(f, s), q[13]);
    } else if (op == OP_LIGHTNING) {
        vec2 sh = strokeShade(f);
        vec3 outsideColor = srgbToLinear3(vec3(q[20], q[21], q[22]));
        vec3 insideColor = srgbToLinear3(vec3(q[23], q[24], q[25]));
        float ki = sh.x;
        float ko = sh.y * (1.0 - ki);
        vec4 bolt = vec4(insideColor * ki + outsideColor * ko, ki + ko);
        int mode = clamp(int(floor(q[28] + 0.5)), 0, 2);
        fragColor = mode == 0 ? bolt + in0 * (1.0 - bolt.a)
                  : (mode == 1 ? vec4(in0.rgb + bolt.rgb, min(1.0, in0.a + bolt.a)) : in0 + bolt - in0 * bolt);
    } else if (op == OP_SCRIBBLE) {
        float k = strokeShade(f).x * clamp(q[6], 0.0, 1.0);
        vec4 line = vec4(srgbToLinear3(vec3(q[3], q[4], q[5])) * k, k);
        int composite = clamp(int(floor(q[18] + 0.5)), 0, 2);
        fragColor = composite == 0 ? line + in0 * (1.0 - k) : (composite == 1 ? line : in0 * k);
    } else if (op == OP_PAINT_BUCKET) {
        // The stages (Compositor applyLibrary): 0 runs of similar pixels (1 each), 1 run doubling by q[28], 2 the fill
        // point, 3 propagation (tex0 = the fill, tex1 = the runs), 4 the mask (Invert Fill), 5 the fill composited
        // (tex1 = the mask softened by Stroke).
        int stage = p.mode.w;
        ivec2 t0 = ivec2(gl_FragCoord.xy);
        if (stage == 0) {
            vec4 seedColor = texelFetch(tex0, bucketSeed(), 0);
            fragColor = vec4(bucketSimilar(texelFetch(tex0, t0, 0), seedColor) ? 1.0 : 0.0);
        } else if (stage == 1) {
            vec4 r = texelFetch(tex0, t0, 0);
            float st = floor(q[28] + 0.5);
            vec4 runs = r;
            for (int c = 0; c < 4; ++c) {
                if (r[c] >= st) {
                    ivec2 n = t0 + BUCKET_DIRS[c] * int(st);
                    runs[c] = st + (bucketInside(n) ? texelFetch(tex0, n, 0)[c] : 0.0);
                }
            }
            fragColor = runs;
        } else if (stage == 2) {
            float r = texelFetch(tex0, t0, 0).x;
            fragColor = vec4(q[31] > 0.5 && r > 0.0 && t0 == bucketSeed() ? 1.0 : 0.0);
        } else if (stage == 3) {
            float filled = texelFetch(tex0, t0, 0).a;
            vec4 r = texelFetch(tex1, t0, 0);
            if (filled < 0.5 && r.x > 0.0) {
                bool hit = false;
                for (int c = 0; c < 4; ++c) {
                    for (int j = 0; j <= 10; ++j) {
                        int st = 1 << j;
                        if (hit || r[c] <= float(st))
                            break;
                        ivec2 n = t0 + BUCKET_DIRS[c] * st;
                        hit = bucketInside(n) && texelFetch(tex0, n, 0).a > 0.5;
                    }
                }
                filled = hit ? 1.0 : 0.0;
            }
            fragColor = vec4(filled);
        } else if (stage == 4) {
            float m = texelFetch(tex0, t0, 0).a > 0.5 ? 1.0 : 0.0;
            fragColor = vec4(q[11] > 0.5 ? 1.0 - m : m);
        } else if (q[4] > 0.5) {
            // View Threshold: what the fill selects, white on black.
            vec4 seedColor = texelFetch(tex0, bucketSeed(), 0);
            fragColor = bucketSimilar(in0, seedColor) ? vec4(1.0) : vec4(0.0, 0.0, 0.0, 1.0);
        } else {
            float b = clamp(textureLod(tex1, v_uv, 0.0).a, 0.0, 1.0);
            int stroke = clamp(int(floor(q[5] + 0.5)), 0, 4);
            float m = b;
            if (stroke == 2)
                m = smoothstep(0.02, 0.3, b);
            else if (stroke == 3)
                m = smoothstep(0.7, 0.98, b);
            else if (stroke == 4)
                m = smoothstep(0.05, 0.3, b) * (1.0 - smoothstep(0.7, 0.95, b));
            float k = m * clamp(q[12], 0.0, 1.0);
            vec4 fill = vec4(srgbToLinear3(vec3(q[8], q[9], q[10])) * k, k);
            int mode = clamp(int(floor(q[13] + 0.5)), 0, 5);
            fragColor = mode == 0 ? fill + in0 * (1.0 - k)
                      : mode == 1 ? in0 + fill * (1.0 - in0.a)
                      : mode == 2 ? vec4(in0.rgb + fill.rgb, min(1.0, in0.a + fill.a))
                      : mode == 3 ? blendOver(3, in0, fill)
                      : mode == 4 ? blendOver(4, in0, fill)
                                  : fill;
        }
    } else if (op == OP_DEPTH_BLUR) {
        // AI Refocus (0.7). tex1's alpha is the depth map placed like the layer: 0 = nearest .. 1 = farthest. A pixel's circle
        // of confusion grows with its distance from the focus (outside the sharp zone) and the blur gathers 48 Vogel-disc taps,
        // each counted when its own circle reaches this pixel; a sharp nearer pixel is not covered by the blurred farther ones.
        // CPU twin: render/reference/LibraryGenerate.cpp depthBlur (same constants).
        float focus = clamp(q[0] / 100.0, 0.0, 1.0);
        float zone = clamp(q[1], 0.0, 100.0) / 200.0;
        float maxRadius = clamp(q[2], 0.0, 100.0) / 100.0 * 40.0 * (p.texel.w / 1080.0);
        float fore = clamp(q[3], 0.0, 100.0) / 100.0;
        float back = clamp(q[4], 0.0, 100.0) / 100.0;
        int view = int(floor(q[5] + 0.5));
        if (!second) {
            fragColor = in0; // no depth map yet: the picture as it is
        } else {
            float z0 = clamp(textureLod(tex1, v_uv, 0.0).a, 0.0, 1.0);
            float off0 = z0 - focus;
            float c0 = clamp(max(abs(off0) - zone, 0.0) / 0.4, 0.0, 1.0) * (off0 < 0.0 ? fore : back);
            float r0 = c0 * maxRadius;
            if (view == 1) {
                fragColor = vec4(vec3(eotf(z0)), 1.0);
            } else if (view == 2) {
                fragColor = vec4(vec3(eotf(c0)), 1.0);
            } else if (maxRadius < 0.25) {
                fragColor = in0;
            } else {
                vec2 base = v_uv * p.texel.zw;
                vec4 sum = in0;
                float total = 1.0;
                for (int i = 0; i < 48; ++i) {
                    float rr = sqrt((float(i) + 0.5) / 48.0) * maxRadius;
                    float th = float(i) * 2.39996323;
                    vec2 pos = base + vec2(cos(th), sin(th)) * rr;
                    vec2 cell = clamp(floor(pos), vec2(0.0), p.texel.zw - 1.0);
                    vec2 uv = (cell + 0.5) * p.texel.xy;
                    float zs = clamp(textureLod(tex1, uv, 0.0).a, 0.0, 1.0);
                    float offs = zs - focus;
                    float rs = clamp(max(abs(offs) - zone, 0.0) / 0.4, 0.0, 1.0) * (offs < 0.0 ? fore : back) * maxRadius;
                    float cover = clamp(rs - rr + 0.5, 0.0, 1.0);
                    if (zs > z0 + 0.02) {
                        cover *= smoothstep(0.0, 1.0, r0 / max(rs, 1.0));
                    }
                    sum += textureLod(tex0, uv, 0.0) * cover;
                    total += cover;
                }
                fragColor = sum / total;
            }
        }
    } else if (op == OP_SKIN_SMOOTHING) {
        fragColor = skinSmoothing(in0);
    } else if (op == OP_FACE_RETOUCH) {
        fragColor = faceRetouch(in0, second);
    } else {
        fragColor = in0;
    }
}
