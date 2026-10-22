#version 440

// The flow mix (P.2, docs/motion.md): A and B warped toward each other along their motion and blended - Time
// Interpolation's Optical Flow between two frames of a clip, and Morph Cut between the two sides of a jump cut. Canvas-size
// intermediates in, one out: linear Rec.709, premultiplied alpha, RGBA16F. The CPU reference is motion::interpolate
// (src/render/motion/src/Interpolate.cpp), which tests/render/test_flowmix.cpp compares against: change both together.
//
// At t (0 = A, 1 = B), for the canvas point x, with F = motion A -> B and G = B -> A (fractions of the canvas):
//   x0 = x - (1 - t) t F(x) + t^2 G(x),  x1 = x + (1 - t)^2 F(x) - t (1 - t) G(x)
//   e0 = |F(x0) + G(x0 + F(x0))|, e1 = |G(x1) + F(x1 + G(x1))| (motion texels): v = 1 / (1 + e^2)
//   out = (w0 A(x0) + w1 B(x1)) / (w0 + w1), w0 = (1 - t) v0, w1 = t v1 ((1 - t) A(x0) + t B(x1) when both vanish)

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pos;

layout(location = 0) out vec4 fragColor;

// The fx passes' block (shaders/fx.frag): p0.x = t, p0.yz = the motion texture's size; mode.y = 1 when render targets
// are stored bottom-up.
layout(std140, binding = 0) uniform FxParams {
    mat4 cornerToClip;
    vec4 texel;
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 toFrame0;
    vec4 toFrame1;
    vec4 frame;
    ivec4 mode;
} p;

layout(binding = 1) uniform sampler2D tex0;   // A
layout(binding = 2) uniform sampler2D tex1;   // B
layout(binding = 3) uniform sampler2D motion; // RGBA32F, top row first: forward u, v, backward u, v (canvas fractions)

// Canvas fractions (y down) <-> render-target texture coordinates (the same flip both ways).
vec2 toTex(vec2 c)
{
    return p.mode.y == 1 ? vec2(c.x, 1.0 - c.y) : c;
}

// The motion at canvas fractions c: bilinear between texel centres, clamped, with texelFetch (32-bit float textures
// need not filter); motion::sampleFlow's arithmetic.
vec4 motionAt(vec2 c)
{
    vec2 size = p.p0.yz;
    vec2 s = c * size - 0.5;
    vec2 x = vec2(s.x >= 0.0 ? min(s.x, size.x - 1.0) : 0.0, s.y >= 0.0 ? min(s.y, size.y - 1.0) : 0.0);
    ivec2 last = ivec2(size) - 1;
    ivec2 i0 = min(ivec2(x), last);
    ivec2 i1 = min(i0 + 1, last);
    vec2 f = x - vec2(i0);
    vec4 a = texelFetch(motion, i0, 0);
    vec4 b = texelFetch(motion, ivec2(i1.x, i0.y), 0);
    vec4 c0 = texelFetch(motion, ivec2(i0.x, i1.y), 0);
    vec4 d = texelFetch(motion, i1, 0);
    vec4 top = a + (b - a) * f.x;
    vec4 bottom = c0 + (d - c0) * f.x;
    return top + (bottom - top) * f.y;
}

void main()
{
    float t = clamp(p.p0.x, 0.0, 1.0);
    float s = 1.0 - t;
    vec2 x = toTex(v_uv);
    vec4 f = motionAt(x);
    vec2 x0 = x - s * t * f.xy + t * t * f.zw;
    vec2 x1 = x + s * s * f.xy - t * s * f.zw;
    vec4 f0 = motionAt(x0);
    vec4 g0 = motionAt(x0 + f0.xy);
    vec2 e0 = (f0.xy + g0.zw) * p.p0.yz;
    vec4 f1 = motionAt(x1);
    vec4 g1 = motionAt(x1 + f1.zw);
    vec2 e1 = (f1.zw + g1.xy) * p.p0.yz;
    float v0 = 1.0 / (1.0 + dot(e0, e0));
    float v1 = 1.0 / (1.0 + dot(e1, e1));
    float w0 = s * v0;
    float w1 = t * v1;
    float sum = w0 + w1;
    if (sum > 1e-6) {
        w0 /= sum;
        w1 /= sum;
    } else {
        w0 = s;
        w1 = t;
    }
    vec4 a = textureLod(tex0, toTex(x0), 0.0);
    vec4 b = textureLod(tex1, toTex(x1), 0.0);
    fragColor = w0 * a + w1 * b;
}
