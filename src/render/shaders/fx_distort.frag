#version 440

// The effect library's Distort family (0.2, render/EffectLibrary.h): resampling of a linear premultiplied RGBA16F layer.
// Positions are in the layer's frame (source pixels, through the inverse of its Motion: toFrame), so a distortion follows
// the clip as Premiere's effects do before Motion; a frame position S is read back through the forward map. A source
// position outside the frame is transparent, with one source pixel of coverage at its edge. The blurs work on the canvas.
// mode.z = the op, p0..p7 = LayerEffect::params[0..31]. CPU twins: render/reference/LibraryEffects.cpp.

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

const float PI = 3.14159265358979;

const int OP_CORNER_PIN = 40;
const int OP_LENS_DISTORTION = 41;
const int OP_MAGNIFY = 42;
const int OP_MIRROR = 43;
const int OP_OFFSET = 44;
const int OP_SPHERIZE = 45;
const int OP_TWIRL = 46;
const int OP_WAVE_WARP = 47;
const int OP_TURBULENT_DISPLACE = 48;
const int OP_TRANSFORM = 49;
const int OP_HORIZONTAL_FLIP = 50;
const int OP_VERTICAL_FLIP = 51;
const int OP_EDGE_FEATHER = 52;
const int OP_REPLICATE = 53;
const int OP_BASIC_3D = 54;
const int OP_BEVEL_EDGES = 55;
const int OP_DIRECTIONAL_BLUR = 56;
const int OP_CAMERA_BLUR = 57;

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

vec2 canvasSize() { return p.frame.zw; }
vec2 frameSize() { return p.frame.xy; }

// The pixel's centre in canvas sequence pixels (exact, from gl_FragCoord).
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

vec2 toCanvas(vec2 f)
{
    float a = p.toFrame0.x;
    float c = p.toFrame0.y;
    float e = p.toFrame0.z;
    float b = p.toFrame1.x;
    float d = p.toFrame1.y;
    float g = p.toFrame1.z;
    float det = a * d - b * c;
    vec2 r = f - vec2(e, g);
    return vec2(d * r.x - c * r.y, -b * r.x + a * r.y) / det;
}

vec4 sampleCanvas(vec2 c)
{
    vec2 s = canvasSize();
    if (c.x < 0.0 || c.y < 0.0 || c.x > s.x || c.y > s.y)
        return vec4(0.0);
    vec2 uv = c / s;
    return textureLod(tex0, p.mode.y == 1 ? vec2(uv.x, 1.0 - uv.y) : uv, 0.0);
}

// The layer at frame position `f`, without the frame's edge.
vec4 sampleRaw(vec2 f)
{
    vec2 uv = toCanvas(f) / canvasSize();
    return textureLod(tex0, p.mode.y == 1 ? vec2(uv.x, 1.0 - uv.y) : uv, 0.0);
}

float frameCoverage(vec2 f)
{
    vec2 s = frameSize();
    return clamp(min(min(f.x, s.x - f.x), min(f.y, s.y - f.y)) + 0.5, 0.0, 1.0);
}

// The layer at frame position `f`: transparent outside its frame.
vec4 sampleFrame(vec2 f) { return sampleRaw(f) * frameCoverage(f); }

vec2 rotateCw(vec2 v, float a) { return vec2(v.x * cos(a) - v.y * sin(a), v.x * sin(a) + v.y * cos(a)); }

float srgbToLinear(float v)
{
    v = clamp(v, 0.0, 1.0);
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

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

// ---- Corner Pin: the unit square onto the quad (Heckbert), inverted ----

// The square-to-quad matrix rows (a b c / d e f / g h 1) for the four corners in frame pixels.
void squareToQuad(vec2 p0, vec2 p1, vec2 p2, vec2 p3, out vec3 r0, out vec3 r1, out vec3 r2)
{
    vec2 d1 = p1 - p2;
    vec2 d2 = p3 - p2;
    vec2 d3 = p0 - p1 + p2 - p3;
    if (d3.x == 0.0 && d3.y == 0.0) {
        r0 = vec3(p1.x - p0.x, p2.x - p1.x, p0.x);
        r1 = vec3(p1.y - p0.y, p2.y - p1.y, p0.y);
        r2 = vec3(0.0, 0.0, 1.0);
        return;
    }
    float den = d1.x * d2.y - d2.x * d1.y;
    float g = (d3.x * d2.y - d2.x * d3.y) / den;
    float h = (d1.x * d3.y - d3.x * d1.y) / den;
    r0 = vec3(p1.x - p0.x + g * p1.x, p3.x - p0.x + h * p3.x, p0.x);
    r1 = vec3(p1.y - p0.y + g * p1.y, p3.y - p0.y + h * p3.y, p0.y);
    r2 = vec3(g, h, 1.0);
}

// (u, v, w): the square position of a frame point through the adjugate of the square-to-quad matrix (w < 0: behind).
vec3 quadToSquare(vec3 r0, vec3 r1, vec3 r2, vec2 f)
{
    float a = r0.x, b = r0.y, c = r0.z;
    float d = r1.x, e = r1.y, ff = r1.z;
    float g = r2.x, h = r2.y;
    vec3 u = vec3(e - ff * h, c * h - b, b * ff - c * e);
    vec3 v = vec3(ff * g - d, a - c * g, c * d - a * ff);
    vec3 w = vec3(d * h - e * g, b * g - a * h, a * e - b * d);
    vec3 x = vec3(f, 1.0);
    float ww = dot(w, x);
    return vec3(dot(u, x) / ww, dot(v, x) / ww, ww);
}

vec4 cornerPin(vec2 f)
{
    vec2 s = frameSize();
    vec3 r0, r1, r2;
    // Corners: upper left (u, v) = (0, 0), upper right (1, 0), lower right (1, 1), lower left (0, 1).
    squareToQuad(vec2(q[0], q[1]) * s, vec2(q[2], q[3]) * s, vec2(q[6], q[7]) * s, vec2(q[4], q[5]) * s, r0, r1, r2);
    vec3 uv = quadToSquare(r0, r1, r2, f);
    vec3 ux = quadToSquare(r0, r1, r2, f + vec2(1.0, 0.0));
    vec3 uy = quadToSquare(r0, r1, r2, f + vec2(0.0, 1.0));
    if (uv.z <= 0.0)
        return vec4(0.0);
    // Distance to the square's edges in output pixels (finite differences of u and v).
    float gu = max(length(vec2(ux.x - uv.x, uy.x - uv.x)), 1e-6);
    float gv = max(length(vec2(ux.y - uv.y, uy.y - uv.y)), 1e-6);
    float edge = min(min(uv.x, 1.0 - uv.x) / gu, min(uv.y, 1.0 - uv.y) / gv);
    return sampleRaw(uv.xy * s) * clamp(edge + 0.5, 0.0, 1.0);
}

// ---- Magnify's blending ----

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

// `src` blended over `dst` (premultiplied) with Magnify's Blending Mode: None replaces, Add adds, the rest are W3C
// source-over with the mode's B(Cb, Cs) on clamped un-premultiplied colours.
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

// ---- Waves and noise ----

float waveOf(int type, float arg)
{
    float t = fract(arg / (2.0 * PI));
    if (type == 1)
        return t < 0.5 ? 1.0 : -1.0;
    if (type == 2)
        return t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t;
    if (type == 3)
        return 2.0 * t - 1.0;
    if (type == 4)
        return t < 0.5 ? sqrt(max(0.0, 1.0 - (4.0 * t - 1.0) * (4.0 * t - 1.0)))
                       : -sqrt(max(0.0, 1.0 - (4.0 * t - 3.0) * (4.0 * t - 3.0)));
    if (type == 5)
        return 2.0 * sqrt(max(0.0, 1.0 - (2.0 * t - 1.0) * (2.0 * t - 1.0))) - 1.0;
    if (type == 6)
        return 1.0 - 2.0 * sqrt(max(0.0, 1.0 - (2.0 * t - 1.0) * (2.0 * t - 1.0)));
    if (type == 7 || type == 8) {
        int k = int(floor(arg / (2.0 * PI)));
        float a = 2.0 * hash01(uint(k), 7u, 0u) - 1.0;
        if (type == 7)
            return a;
        float b = 2.0 * hash01(uint(k + 1), 7u, 0u) - 1.0;
        return mix(a, b, t * t * (3.0 - 2.0 * t));
    }
    return sin(arg);
}

// How much of a warp a frame point gets under a pinning: 0 at the pinned edges (or centre), 1 from a tenth of the
// frame's smaller side away. `set`: 0 Wave Warp's choices, 1 Turbulent Displace's.
float pinWeight(int pin, int set, vec2 f)
{
    vec2 s = frameSize();
    float ramp = 0.1 * min(s.x, s.y);
    float l = f.x;
    float r = s.x - f.x;
    float t = f.y;
    float b = s.y - f.y;
    float d = 1e9;
    if (set == 0) {
        if (pin == 1) d = min(min(l, r), min(t, b));
        else if (pin == 2) d = length(f - 0.5 * s);
        else if (pin == 3) d = l;
        else if (pin == 4) d = t;
        else if (pin == 5) d = r;
        else if (pin == 6) d = b;
        else if (pin == 7) d = min(t, b);
        else if (pin == 8) d = min(l, r);
    } else {
        if (pin == 1) d = min(min(l, r), min(t, b));
        else if (pin == 2) d = min(t, b);
        else if (pin == 3) d = min(l, r);
        else if (pin == 4) d = l;
        else if (pin == 5) d = t;
        else if (pin == 6) d = r;
        else if (pin == 7) d = b;
    }
    return clamp(d / ramp, 0.0, 1.0);
}

// Value noise in 3D: lattice values hash01 * 2 - 1, smooth (3t^2 - 2t^3) trilinear interpolation; z wraps every
// `period` cells when period > 0.
float valueAt(int x, int y, int z, uint seed)
{
    return 2.0 * hash01(uint(x) + 2654435761u * uint(y), uint(z) + seed * 1013u, 11u) - 1.0;
}

float noise3(vec3 v, int period, uint seed)
{
    vec3 i = floor(v);
    vec3 f = v - i;
    vec3 w = f * f * (3.0 - 2.0 * f);
    int x = int(i.x);
    int y = int(i.y);
    int z0 = int(i.z);
    int z1 = z0 + 1;
    if (period > 0) {
        z0 = z0 - period * int(floor(float(z0) / float(period)));
        z1 = z1 - period * int(floor(float(z1) / float(period)));
    }
    float a = mix(mix(valueAt(x, y, z0, seed), valueAt(x + 1, y, z0, seed), w.x),
                  mix(valueAt(x, y + 1, z0, seed), valueAt(x + 1, y + 1, z0, seed), w.x), w.y);
    float b = mix(mix(valueAt(x, y, z1, seed), valueAt(x + 1, y, z1, seed), w.x),
                  mix(valueAt(x, y + 1, z1, seed), valueAt(x + 1, y + 1, z1, seed), w.x), w.y);
    return mix(a, b, w.z);
}

// Fractal sum: floor(complexity) octaves (and the next at its fraction), each half as strong and twice as fine.
float fbm(vec2 v, float z, float complexity, int period, uint seed)
{
    float sum = 0.0;
    float norm = 0.0;
    float amp = 1.0;
    float freq = 1.0;
    int octaves = int(floor(complexity));
    float last = complexity - float(octaves);
    for (int o = 0; o < 11; ++o) {
        float weight = o < octaves ? 1.0 : (o == octaves ? last : 0.0);
        if (weight <= 0.0)
            break;
        sum += weight * amp * noise3(vec3(v * freq, z), period, seed + uint(o) * 101u);
        norm += weight * amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

// ---- Transform (the effect) ----

// The source frame position of output `f` for anchor A, position P, scales, skew (tangent), skew axis and rotation.
vec2 transformInverse(vec2 f, vec2 anchor, vec2 position, vec2 scale, float skew, float axis, float rotation)
{
    vec2 v = rotateCw(f - position, -rotation);
    v = rotateCw(v, -axis);
    v.x -= skew * v.y;
    v = rotateCw(v, axis);
    return anchor + v / scale;
}

vec4 transformAt(vec2 f, float k)
{
    vec2 s = frameSize();
    // The shutter's start (params 0..9) and, when set, its end (params 14..22), mixed by k.
    bool end = q[23] > 0.5;
    vec2 anchor = vec2(q[0], q[1]);
    vec2 position = vec2(q[2], q[3]);
    float sh = q[5];
    float sw = q[4] > 0.5 ? q[5] : q[6];
    float skew = q[7];
    float axis = q[8];
    float rotation = q[9];
    if (end) {
        anchor = mix(anchor, vec2(q[14], q[15]), k);
        position = mix(position, vec2(q[16], q[17]), k);
        sh = mix(sh, q[18], k);
        sw = mix(sw, q[4] > 0.5 ? q[18] : q[19], k);
        skew = mix(skew, q[20], k);
        axis = mix(axis, q[21], k);
        rotation = mix(rotation, q[22], k);
    }
    vec2 scale = vec2(sw, sh) / 100.0;
    if (abs(scale.x) < 1e-6 || abs(scale.y) < 1e-6)
        return vec4(0.0);
    vec2 src = transformInverse(f, anchor * s, position * s, scale, tan(radians(skew)), radians(axis), radians(rotation));
    return sampleFrame(src);
}

// ---- Basic 3D ----

vec3 rotX(vec3 v, float a) { return vec3(v.x, v.y * cos(a) - v.z * sin(a), v.y * sin(a) + v.z * cos(a)); }
vec3 rotY(vec3 v, float a) { return vec3(v.x * cos(a) + v.z * sin(a), v.y, -v.x * sin(a) + v.z * cos(a)); }

vec4 basic3d(vec2 f)
{
    vec2 s = frameSize();
    vec2 c = 0.5 * s;
    float focal = 1.6 * max(s.x, s.y);
    float swivel = radians(q[0]);
    float tilt = radians(q[1]);
    vec3 ex = rotY(rotX(vec3(1.0, 0.0, 0.0), tilt), swivel);
    vec3 ey = rotY(rotX(vec3(0.0, 1.0, 0.0), tilt), swivel);
    vec3 n = rotY(rotX(vec3(0.0, 0.0, 1.0), tilt), swivel);
    vec3 p0 = vec3(c, q[2] / 100.0 * focal);
    vec3 cam = vec3(c, -focal);
    vec3 d = vec3(f - c, focal);
    float den = dot(d, n);
    if (abs(den) < 1e-6)
        return vec4(0.0);
    float t = dot(p0 - cam, n) / den;
    if (t <= 0.0)
        return vec4(0.0);
    vec3 x = cam + t * d;
    vec2 src = c + vec2(dot(x - p0, ex), dot(x - p0, ey));
    vec4 color = sampleFrame(src);
    if (q[3] > 0.5) {
        // A light above, behind and to the left of the viewer; a highlight where it reflects towards the eye.
        vec3 nn = dot(n, d) > 0.0 ? -n : n;
        vec3 toLight = normalize(vec3(-1.0, -1.0, -1.0));
        vec3 toEye = normalize(cam - x);
        float spec = pow(max(0.0, dot(reflect(-toLight, nn), toEye)), 40.0);
        color.rgb += vec3(0.8 * spec) * color.a;
    }
    return color;
}

void main()
{
    loadParams();
    int op = p.mode.z;
    vec2 cpos = canvasPos();
    vec2 f = toFrame(cpos);
    vec2 s = frameSize();
    vec4 c = textureLod(tex0, v_uv, 0.0);
    float div = p.frame.z / p.texel.z; // sequence pixels per canvas texel
    if (op == OP_CORNER_PIN) {
        fragColor = cornerPin(f);
    } else if (op == OP_LENS_DISTORTION) {
        vec2 center = 0.5 * s * vec2(1.0 + q[2] / 100.0, 1.0 + q[1] / 100.0);
        float radius = 0.5 * length(s);
        vec2 u = (f - center) / radius;
        float r2 = dot(u, u);
        vec2 su = u * (1.0 - q[0] / 100.0 * 0.6 * r2) + vec2(q[4], q[3]) / 100.0 * 0.25 * r2;
        vec2 src = center + su * radius;
        vec4 fill = q[5] > 0.5 ? vec4(0.0) : vec4(srgbToLinear(q[6]), srgbToLinear(q[7]), srgbToLinear(q[8]), 1.0);
        fragColor = mix(fill, sampleRaw(src), frameCoverage(src));
    } else if (op == OP_MAGNIFY) {
        float m = max(q[3] / 100.0, 0.01);
        int link = int(q[4] + 0.5);
        float size = q[5] * (link >= 1 ? m : 1.0);
        float feather = q[6] * (link >= 2 ? m : 1.0);
        vec2 center = vec2(q[1], q[2]) * s;
        vec2 d = f - center;
        float dist = q[0] < 0.5 ? length(d) : max(abs(d.x), abs(d.y));
        float cov = feather > 0.0 ? clamp((size - dist) / feather, 0.0, 1.0) : clamp(size - dist + 0.5, 0.0, 1.0);
        vec4 lens = blendOver(int(q[9] + 0.5), c, sampleFrame(center + d / m));
        fragColor = mix(c, lens, cov * q[7]);
    } else if (op == OP_MIRROR) {
        vec2 center = vec2(q[0], q[1]) * s;
        float a = radians(q[2]);
        vec2 n = vec2(cos(a), sin(a));
        float d = dot(f - center, n);
        fragColor = d > 0.0 ? sampleFrame(f - 2.0 * d * n) : sampleFrame(f);
    } else if (op == OP_OFFSET) {
        vec2 shift = (vec2(q[0], q[1]) - 0.5) * s;
        vec2 src = mod(f - shift, s);
        fragColor = mix(sampleRaw(src) * frameCoverage(f), c, q[2]);
    } else if (op == OP_SPHERIZE) {
        vec2 center = vec2(q[1], q[2]) * s;
        vec2 d = f - center;
        float r = q[0] > 0.0 ? length(d) / q[0] : 1.0;
        vec2 src = f;
        if (r < 1.0 && r > 0.0)
            src = center + d * (asin(r) / (r * 0.5 * PI));
        else if (r == 0.0)
            src = center;
        fragColor = sampleFrame(src);
    } else if (op == OP_TWIRL) {
        float radius = q[1] / 100.0 * max(s.x, s.y);
        vec2 center = vec2(q[2], q[3]) * s;
        vec2 d = f - center;
        float len = length(d);
        vec2 src = f;
        if (len < radius) {
            float k = 1.0 - len / radius;
            src = center + rotateCw(d, -radians(q[0]) * k * k);
        }
        fragColor = sampleFrame(src);
    } else if (op == OP_WAVE_WARP) {
        float a = radians(q[3]);
        vec2 dir = vec2(sin(a), -cos(a));
        vec2 perp = vec2(-dir.y, dir.x);
        float arg = dot(f, dir) / max(q[2], 1.0) * 2.0 * PI - q[4] * q[31] * 2.0 * PI + radians(q[6]);
        vec2 disp = perp * q[1] * waveOf(int(q[0] + 0.5), arg) * pinWeight(int(q[5] + 0.5), 0, f);
        fragColor = sampleFrame(f - disp);
    } else if (op == OP_TURBULENT_DISPLACE) {
        int type = int(q[0] + 0.5);
        float amount = q[1];
        float size = max(q[2], 1.0);
        vec2 v = (f - (vec2(q[3], q[4]) - 0.5) * s) / size;
        float z = q[6] / 360.0;
        int period = q[7] > 0.5 ? max(1, int(q[8] + 0.5)) : 0;
        if (period > 0)
            z = mod(z, float(period));
        uint seed = uint(max(0.0, q[9]));
        float complexity = (type >= 3 && type <= 5) ? 1.0 : q[5];
        float n1 = fbm(v, z, complexity, period, seed);
        float n2 = fbm(v + vec2(31.7, 17.3), z + 5.1, complexity, period, seed);
        vec2 center = 0.5 * s;
        vec2 src;
        if (type == 1 || type == 4)
            src = center + (f - center) * (1.0 + 0.002 * amount * n1);
        else if (type == 2 || type == 5)
            src = center + rotateCw(f - center, 0.005 * amount * n1);
        else if (type == 6)
            src = f + vec2(0.0, amount * n2);
        else if (type == 7)
            src = f + vec2(amount * n1, 0.0);
        else if (type == 8)
            src = f + vec2(amount * n1);
        else
            src = f + amount * vec2(n1, n2);
        src = f + (src - f) * pinWeight(int(q[10] + 0.5), 1, f);
        fragColor = sampleFrame(src);
    } else if (op == OP_TRANSFORM) {
        vec4 acc = transformAt(f, 0.0);
        if (q[23] > 0.5 && q[11] < 0.5 && q[12] > 0.0) {
            // Motion blur over the shutter: 16 positions from its start to its end.
            acc = vec4(0.0);
            for (int i = 0; i < 16; ++i)
                acc += transformAt(f, float(i) / 15.0);
            acc /= 16.0;
        }
        fragColor = acc * q[10];
    } else if (op == OP_HORIZONTAL_FLIP) {
        fragColor = sampleFrame(vec2(s.x - f.x, f.y));
    } else if (op == OP_VERTICAL_FLIP) {
        fragColor = sampleFrame(vec2(f.x, s.y - f.y));
    } else if (op == OP_EDGE_FEATHER) {
        float w = q[0] / 100.0 * 0.5 * min(s.x, s.y);
        float m = w > 0.0 ? clamp(min(min(f.x, s.x - f.x), min(f.y, s.y - f.y)) / w, 0.0, 1.0) : 1.0;
        fragColor = c * m;
    } else if (op == OP_REPLICATE) {
        float n = clamp(floor(q[0] + 0.5), 2.0, 16.0);
        vec2 src = fract(f / s * n) * s;
        fragColor = sampleRaw(src) * frameCoverage(f);
    } else if (op == OP_BASIC_3D) {
        fragColor = basic3d(f);
    } else if (op == OP_BEVEL_EDGES) {
        float band = q[0] * 0.5 * min(s.x, s.y);
        float l = f.x;
        float r = s.x - f.x;
        float t = f.y;
        float b = s.y - f.y;
        float dmin = min(min(l, r), min(t, b));
        fragColor = c;
        if (dmin < band) {
            vec2 n = dmin == l ? vec2(-1.0, 0.0) : dmin == r ? vec2(1.0, 0.0) : dmin == t ? vec2(0.0, -1.0) : vec2(0.0, 1.0);
            float a = radians(q[1]);
            float shade = dot(n, vec2(sin(a), -cos(a))) * q[5];
            vec3 light = vec3(srgbToLinear(q[2]), srgbToLinear(q[3]), srgbToLinear(q[4])) * c.a;
            fragColor.rgb = shade > 0.0 ? c.rgb + (light - c.rgb) * shade : c.rgb * (1.0 + shade);
        }
    } else if (op == OP_DIRECTIONAL_BLUR) {
        float a = radians(q[0]);
        vec2 dir = vec2(sin(a), -cos(a));
        float len = max(q[1], 0.0);
        if (len < 0.5 * div) {
            fragColor = c;
        } else {
            int taps = int(clamp(ceil(len / div) + 1.0, 2.0, 97.0));
            vec4 acc = vec4(0.0);
            for (int i = 0; i < 97; ++i) {
                if (i >= taps)
                    break;
                acc += sampleCanvas(cpos + dir * ((float(i) / float(taps - 1) - 0.5) * len));
            }
            fragColor = acc / float(taps);
        }
    } else if (op == OP_CAMERA_BLUR) {
        float radius = q[0] / 100.0 * 40.0;
        if (radius < 0.5 * div) {
            fragColor = c;
        } else {
            vec4 acc = vec4(0.0);
            for (int i = 0; i < 64; ++i) {
                float r = radius * sqrt((float(i) + 0.5) / 64.0);
                float a = float(i) * 2.399963229728653;
                acc += sampleCanvas(cpos + r * vec2(cos(a), sin(a)));
            }
            fragColor = acc / 64.0;
        }
    } else {
        fragColor = c;
    }
}
