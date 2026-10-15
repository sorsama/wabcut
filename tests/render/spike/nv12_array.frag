#version 440

// NV12 decoder surface slice -> BT.709 limited -> R'G'B' (Rec.709 out = pass-through), as the compositor computes it.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Params {
    mat4 cornerToClip;
    vec4 uvScale;
    vec4 chromaXform;
} p;

layout(binding = 1) uniform sampler2DArray luma;   // R8 view of the NV12 surface, one array slice
layout(binding = 2) uniform sampler2DArray chroma; // R8G8 view of the same slice

void main()
{
    vec2 uv = v_uv * p.uvScale.xy;
    vec2 cuv = v_uv * p.chromaXform.xy + p.chromaXform.zw;
    float y = (texture(luma, vec3(uv, 0.0)).r * 255.0 - 16.0) / 219.0;
    vec2 c = (texture(chroma, vec3(cuv, 0.0)).rg * 255.0 - 128.0) / 224.0;
    vec3 rgb = vec3(y + 1.5748 * c.y, y - 0.187324 * c.x - 0.468124 * c.y, y + 1.8556 * c.x);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
