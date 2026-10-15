#version 440

// Zero-copy spike: full-target quad sampling a decoder surface (texture-array slice) directly.

layout(location = 0) in vec2 corner;
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform Params {
    mat4 cornerToClip;
    vec4 uvScale;     // xy: display size / surface size (decoder surfaces are padded, e.g. 1088 rows)
    vec4 chromaXform; // chroma uv = uv * xy + zw (siting, as the compositor does)
} p;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    v_uv = corner;
    gl_Position = p.cornerToClip * vec4(corner, 0.0, 1.0);
}
