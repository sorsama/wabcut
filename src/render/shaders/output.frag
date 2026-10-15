#version 440

// Output pass: linear premultiplied canvas -> the role's output transform (display or export encoding).
// Mirrors render::linearToOutput().

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform OutputParams {
    mat4 cornerToClip;
    vec4 uvXform;
    ivec4 mode;
} p;

layout(binding = 1) uniform sampler2D canvasTex;

// mode.x
const int OUT_REC709 = 0;
const int OUT_SRGB = 1;
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

void main()
{
    vec4 c = texture(canvasTex, v_uv); // linear, premultiplied
    int t = p.mode.x;
    if (p.mode.y == ALPHA_OPAQUE) {
        fragColor = vec4(encode3(c.rgb, t), 1.0); // over black
    } else {
        float a = clamp(c.a, 0.0, 1.0);
        vec3 straight = a > 0.0 ? c.rgb / a : vec3(0.0);
        vec3 e = encode3(straight, t);
        fragColor = p.mode.y == ALPHA_STRAIGHT ? vec4(e, a) : vec4(e * a, a);
    }
}
