#version 440

// Effect / transition passes: a full-canvas quad over an intermediate (canvas-size, linear premultiplied RGBA16F).

layout(location = 0) in vec2 corner; // unit square, (0,0) = canvas top-left

layout(location = 0) out vec2 v_uv;  // texture coordinate of the same canvas point in the inputs
layout(location = 1) out vec2 v_pos; // canvas position, 0..1 with a top-left origin

layout(std140, binding = 0) uniform FxParams {
    mat4 cornerToClip;
    vec4 texel;
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 toFrame0;
    vec4 toFrame1;
    vec4 frame;
    ivec4 mode; // y: 1 = textures are stored bottom-up (QRhi::isYUpInFramebuffer)
} p;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    v_pos = corner;
    v_uv = p.mode.y == 1 ? vec2(corner.x, 1.0 - corner.y) : corner;
    gl_Position = p.cornerToClip * vec4(corner, 0.0, 1.0);
}
