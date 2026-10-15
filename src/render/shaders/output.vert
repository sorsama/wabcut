#version 440

// Output pass: draws the canvas into the target's viewport (letterboxed by the caller).

layout(location = 0) in vec2 corner;
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform OutputParams {
    mat4 cornerToClip;
    vec4 uvXform; // canvas uv = corner * xy + zw (flips rendered textures on Y-up framebuffers)
    ivec4 mode;   // x = output transform, y = output alpha
} p;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    v_uv = corner * p.uvXform.xy + p.uvXform.zw;
    gl_Position = p.cornerToClip * vec4(corner, 0.0, 1.0);
}
