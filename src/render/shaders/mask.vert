#version 440

// Mask passes (I.6, mask.frag): a full-target quad. v_pos runs 0..1 across the target with a top-left origin; v_uv is
// the texture coordinate of that point in inputs of the target's size.

layout(location = 0) in vec2 corner;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_pos;

layout(std140, binding = 0) uniform MaskParams {
    mat4 cornerToClip;
    vec4 texel;
    vec4 toFrame0;
    vec4 toFrame1;
    vec4 frame;
    ivec4 mode;
    vec4 domain;
    vec4 sdfDomain;
    vec4 style;
    vec4 aspect;
    vec4 affine0[4];
    vec4 affine1[4];
    vec4 edge[4];
    ivec4 combine[4];
} u;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    v_pos = corner;
    v_uv = u.mode.y == 1 ? vec2(corner.x, 1.0 - corner.y) : corner;
    gl_Position = u.cornerToClip * vec4(corner, 0.0, 1.0);
}
