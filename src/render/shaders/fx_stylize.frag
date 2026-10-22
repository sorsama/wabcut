#version 440

// The effect library's Stylize family (0.2, render/EffectLibrary.h): neighbourhood operations on a linear premultiplied
// RGBA16F layer, in canvas space. Some ops are several passes (Compositor applyLibrary): tex1 is then a blurred copy or
// a matte, and mode.w says which pass this is. mode.z = the op, p0..p7 = LayerEffect::params[0..31]. CPU twins:
// render/reference/LibraryStylize.cpp.

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
    ivec4 mode;    // y = textures stored bottom-up, z = the op, w = the pass
    vec4 p3;
    vec4 p4;
    vec4 p5;
    vec4 p6;
    vec4 p7;
} p;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;

const float PI = 3.14159265358979;

const int OP_UNSHARP_MASK = 70;
const int OP_EMBOSS = 71;
const int OP_COLOR_EMBOSS = 72;
const int OP_FIND_EDGES = 73;
const int OP_MEDIAN = 74;
const int OP_DUST_SCRATCHES = 75;
const int OP_ALPHA_GLOW = 76;
const int OP_BEVEL_ALPHA = 77;
const int OP_RADIAL_SHADOW = 78;
const int OP_ROUGHEN_EDGES = 79;
const int OP_COLOR_KEY = 80;
const int OP_CONVOLUTION_KERNEL = 81;

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

vec2 canvasPos()
{
    vec2 fc = gl_FragCoord.xy;
    if (p.mode.y == 1)
        fc.y = p.texel.w - fc.y;
    return fc * p.frame.zw / p.texel.zw;
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

vec2 uvOf(vec2 c)
{
    vec2 uv = c / canvasSize();
    return p.mode.y == 1 ? vec2(uv.x, 1.0 - uv.y) : uv;
}

// tex0 / tex1 at canvas position `c`, transparent outside the canvas.
vec4 at0(vec2 c)
{
    vec2 s = canvasSize();
    if (c.x < 0.0 || c.y < 0.0 || c.x > s.x || c.y > s.y)
        return vec4(0.0);
    return textureLod(tex0, uvOf(c), 0.0);
}

vec4 at1(vec2 c)
{
    vec2 s = canvasSize();
    if (c.x < 0.0 || c.y < 0.0 || c.x > s.x || c.y > s.y)
        return vec4(0.0);
    return textureLod(tex1, uvOf(c), 0.0);
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

vec3 encoded(vec4 c)
{
    float a = clamp(c.a, 0.0, 1.0);
    return a > 0.0 ? vec3(oetf(c.r / a), oetf(c.g / a), oetf(c.b / a)) : vec3(0.0);
}

vec4 premultiplied(vec3 e, float a) { return vec4(eotf(e.r), eotf(e.g), eotf(e.b), 1.0) * a; }

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

// Value noise and its fractal sum (fx_distort.frag's).
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

// The median of a 3 x 3 grid `step` canvas pixels apart, by luma (the sample of the middle luma; ties keep the first).
vec4 medianOf(vec2 c, float step)
{
    vec4 s[9];
    float l[9];
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            vec4 v = at0(c + vec2(float(i - 1), float(j - 1)) * step);
            s[j * 3 + i] = v;
            l[j * 3 + i] = luma(encoded(v));
        }
    }
    vec4 result = s[4];
    for (int k = 0; k < 9; ++k) {
        int below = 0;
        int equalBefore = 0;
        for (int m = 0; m < 9; ++m) {
            if (l[m] < l[k])
                ++below;
            else if (l[m] == l[k] && m < k)
                ++equalBefore;
        }
        if (below + equalBefore == 4)
            result = s[k];
    }
    return result;
}

void main()
{
    loadParams();
    int op = p.mode.z;
    int pass = p.mode.w;
    vec2 c = canvasPos();
    vec4 in0 = textureLod(tex0, v_uv, 0.0);
    float div = p.frame.z / p.texel.z;
    if (op == OP_UNSHARP_MASK) {
        // tex1 = the layer blurred (sigma = Radius); sharpened where the difference passes Threshold.
        vec4 b = textureLod(tex1, v_uv, 0.0);
        vec3 d = in0.rgb - b.rgb;
        float m = max(abs(d.r), max(abs(d.g), abs(d.b)));
        float t = q[2] / 255.0;
        float w = q[2] <= 0.0 ? 1.0 : smoothstep(t - 0.5 / 255.0, t + 0.5 / 255.0, m);
        fragColor = vec4(max(in0.rgb + q[0] / 100.0 * d * w, vec3(0.0)), in0.a);
    } else if (op == OP_EMBOSS || op == OP_COLOR_EMBOSS) {
        float a = radians(q[0]);
        vec2 d = vec2(sin(a), -cos(a)) * max(q[1], 0.0);
        float relief = luma(encoded(at0(c + d))) - luma(encoded(at0(c - d)));
        float v = 0.5 + q[2] / 100.0 * 0.5 * relief;
        vec3 e = encoded(in0);
        vec3 r = op == OP_EMBOSS ? vec3(clamp(v, 0.0, 1.0)) : clamp(e + vec3(v - 0.5), 0.0, 1.0);
        fragColor = premultiplied(mix(r, e, q[3]), clamp(in0.a, 0.0, 1.0));
    } else if (op == OP_FIND_EDGES) {
        // Sobel on the encoded colour, one canvas texel apart; dark lines on white (Invert: bright on black).
        vec2 t = vec2(div);
        vec3 s00 = encoded(at0(c + vec2(-t.x, -t.y)));
        vec3 s10 = encoded(at0(c + vec2(0.0, -t.y)));
        vec3 s20 = encoded(at0(c + vec2(t.x, -t.y)));
        vec3 s01 = encoded(at0(c + vec2(-t.x, 0.0)));
        vec3 s21 = encoded(at0(c + vec2(t.x, 0.0)));
        vec3 s02 = encoded(at0(c + vec2(-t.x, t.y)));
        vec3 s12 = encoded(at0(c + vec2(0.0, t.y)));
        vec3 s22 = encoded(at0(c + vec2(t.x, t.y)));
        vec3 gx = (s20 + 2.0 * s21 + s22) - (s00 + 2.0 * s01 + s02);
        vec3 gy = (s02 + 2.0 * s12 + s22) - (s00 + 2.0 * s10 + s20);
        vec3 mag = clamp(sqrt(gx * gx + gy * gy) * 0.25, 0.0, 1.0);
        vec3 r = q[0] > 0.5 ? mag : 1.0 - mag;
        vec3 e = encoded(in0);
        fragColor = premultiplied(mix(r, e, q[1]), clamp(in0.a, 0.0, 1.0));
    } else if (op == OP_CONVOLUTION_KERNEL) {
        // Premiere's 3 x 3 kernel (M11 top left) on the encoded colour, one canvas texel apart: sum M x neighbour /
        // Scale + Offset / 255; Process Alpha convolves the alpha too.
        float scale = max(q[10], 1.0);
        vec3 sum = vec3(0.0);
        float alpha = 0.0;
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                vec4 s = at0(c + vec2(float(i - 1), float(j - 1)) * div);
                float m = q[j * 3 + i];
                sum += m * encoded(s);
                alpha += m * s.a;
            }
        }
        vec3 r = clamp(sum / scale + q[9] / 255.0, 0.0, 1.0);
        float a = q[11] > 0.5 ? clamp(alpha / scale + q[9] / 255.0, 0.0, 1.0) : clamp(in0.a, 0.0, 1.0);
        fragColor = premultiplied(r, a);
    } else if (op == OP_MEDIAN || op == OP_DUST_SCRATCHES) {
        float radius = op == OP_MEDIAN ? q[0] : q[0];
        bool alpha = op == OP_MEDIAN ? q[1] > 0.5 : q[2] > 0.5;
        if (radius < 0.5) {
            fragColor = in0;
            return;
        }
        vec4 med = medianOf(c, max(radius, div));
        if (op == OP_DUST_SCRATCHES) {
            vec3 e = encoded(in0);
            vec3 em = encoded(med);
            float diff = max(abs(e.r - em.r), max(abs(e.g - em.g), abs(e.b - em.b)));
            if (diff * 255.0 <= q[1])
                med = in0;
        }
        fragColor = alpha ? med : premultiplied(encoded(med), clamp(in0.a, 0.0, 1.0));
    } else if (op == OP_ALPHA_GLOW) {
        // tex1 = the layer blurred by Glow: its alpha, coloured, under the layer at Brightness.
        float g = clamp(textureLod(tex1, v_uv, 0.0).a, 0.0, 1.0);
        float k = q[9] > 0.5 ? g : (g > 0.004 ? 1.0 : 0.0);
        vec3 start = srgbToLinear3(vec3(q[2], q[3], q[4]));
        vec3 end = srgbToLinear3(vec3(q[5], q[6], q[7]));
        vec3 color = q[8] > 0.5 ? mix(end, start, g) : start;
        float a = k * clamp(q[1] / 255.0, 0.0, 1.0);
        fragColor = in0 + vec4(color * a, a) * (1.0 - in0.a);
    } else if (op == OP_BEVEL_ALPHA) {
        // tex1 = the layer blurred by Edge Thickness: the slope of its alpha lights and shades the edges.
        float t = div;
        float gx = (at1(c + vec2(t, 0.0)).a - at1(c - vec2(t, 0.0)).a) * 0.5;
        float gy = (at1(c + vec2(0.0, t)).a - at1(c - vec2(0.0, t)).a) * 0.5;
        float a = radians(q[1]);
        vec2 toLight = vec2(sin(a), -cos(a));
        float shade = clamp(-dot(vec2(gx, gy), toLight) * max(q[0], 1.0) / div * 2.0, -1.0, 1.0) * q[5];
        vec3 light = srgbToLinear3(vec3(q[2], q[3], q[4])) * in0.a;
        fragColor = vec4(shade > 0.0 ? in0.rgb + (light - in0.rgb) * shade : in0.rgb * (1.0 + shade), in0.a);
    } else if (op == OP_RADIAL_SHADOW) {
        if (pass == 0) {
            // The shadow: the layer's alpha projected from the light (Projection Distance scales it away from it).
            vec2 light = toCanvas(vec2(q[4], q[5]) * p.frame.xy);
            float k = 1.0 + max(q[6], 0.0) / 100.0;
            vec4 src = at0(light + (c - light) / k);
            vec3 shadow = srgbToLinear3(vec3(q[0], q[1], q[2]));
            if (q[8] > 0.5 && src.a > 0.0) // Glass Edge: the layer's colour shows through
                shadow = mix(shadow, src.rgb / src.a, clamp(q[9], 0.0, 1.0));
            float sa = src.a * q[3];
            fragColor = vec4(shadow * sa, sa);
        } else {
            // tex1 = the (softened) shadow: under the layer, or alone with Shadow Only.
            vec4 shadow = textureLod(tex1, v_uv, 0.0);
            fragColor = q[10] > 0.5 ? shadow : in0 + shadow * (1.0 - in0.a);
        }
    } else if (op == OP_ROUGHEN_EDGES) {
        // tex1 = the layer's alpha softened by Border: its 0.5 level, pushed by fractal noise, is the new edge.
        float ab = clamp(textureLod(tex1, v_uv, 0.0).a, 0.0, 1.0);
        int type = int(q[0] + 0.5);
        float scale = max(q[7], 1.0);
        float stretch = exp2(q[8] * 0.25);
        vec2 v = (c - (vec2(q[9], q[10]) - 0.5) * canvasSize()) / scale * vec2(1.0 / stretch, stretch);
        int period = q[13] > 0.5 ? max(1, int(q[14] + 0.5)) : 0;
        float z = q[12] / 360.0;
        if (period > 0)
            z = mod(z, float(period));
        float n = fbm(v, z, q[11], period, uint(max(0.0, q[15])));
        float sharp = max(q[5], 0.0);
        if (type == 2 || type == 3) // Cut, Spiky: hard edges
            sharp += 6.0;
        float w = 0.5 / (1.0 + 2.0 * sharp);
        float level = ab + n * q[6] * 0.5;
        float m = smoothstep(0.5 - w, 0.5 + w, level);
        vec4 rough = in0 * m;
        bool colored = type == 1 || type == 5 || type == 7;
        if (colored && in0.a > 0.0) {
            float edge = 1.0 - clamp((level - 0.5) * 4.0, 0.0, 1.0);
            vec3 edgeColor = srgbToLinear3(vec3(q[1], q[2], q[3]));
            rough.rgb = mix(rough.rgb, edgeColor * rough.a, edge);
        }
        fragColor = rough;
    } else if (op == OP_COLOR_KEY) {
        if (pass == 0) {
            // The matte: transparent where every channel is within Color Tolerance (of 255) of the key colour.
            vec3 e = encoded(in0);
            vec3 k = vec3(q[0], q[1], q[2]);
            vec3 d = abs(e - k) * 255.0;
            float m = smoothstep(q[3] - 0.5, q[3] + 0.5, max(d.r, max(d.g, d.b)));
            fragColor = vec4(m);
        } else if (pass == 1) {
            // Edge Thin: the matte eroded (positive) or dilated (negative) by a disc of |Edge Thin| pixels.
            float r = abs(q[4]) / div;
            int reach = int(floor(r));
            bool erode = q[4] > 0.0;
            float v = textureLod(tex0, v_uv, 0.0).a;
            for (int j = -6; j <= 6; ++j) {
                for (int i = -6; i <= 6; ++i) {
                    if (abs(i) > reach || abs(j) > reach || float(i * i + j * j) > r * r)
                        continue;
                    vec2 o = vec2(float(i), float(j)) * div;
                    float s = at0(c + o).a;
                    v = erode ? min(v, s) : max(v, s);
                }
            }
            fragColor = vec4(v);
        } else {
            // tex1 = the matte (thinned, feathered): the layer times it.
            fragColor = in0 * clamp(textureLod(tex1, v_uv, 0.0).a, 0.0, 1.0);
        }
    } else {
        fragColor = in0;
    }
}
