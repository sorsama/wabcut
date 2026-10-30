#version 440

// Layer pass: draws one frame as a (transformed) quad into the linear RGBA16F canvas.
// The quad is the frame rectangle grown by a one-canvas-pixel fringe so edges can be antialiased analytically.

layout(location = 0) in vec2 corner; // unit square, (0,0) = frame top-left

layout(location = 0) out vec2 v_uv;  // luma-plane texture coordinate
layout(location = 1) out vec2 v_src; // source pixel coordinate (edge coverage)

// The whole block of layer.frag and layer_array.frag, member for member: OpenGL links the stages of a program only when
// a uniform block they share is declared the same in each (Vulkan, D3D and Metal do not care).
layout(std140, binding = 0) uniform LayerParams {
    mat4 cornerToClip; // unit corner -> clip space (fringe, layer transform, canvas projection, QRhi correction)
    mat4 sampleToRgb;  // rgb' = (sampleToRgb * vec4(samples, 1)).rgb
    mat4 toWorking;    // upper 3x3: linear source primaries -> linear Rec.709
    vec4 chromaXform;  // chroma uv = uv * xy + zw (subsampling + siting)
    vec4 srcRect;      // xy = frame size in source pixels, zw = fringe in source pixels
    vec4 params;       // x = opacity, y = alpha sample scale, zw = uv scale (display / texture size)
    ivec4 mode;        // x = plane layout, y = transfer, z = alpha mode
    vec4 cm;           // fragment stage only (layer.frag)
} p;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    vec2 src = corner * (p.srcRect.xy + 2.0 * p.srcRect.zw) - p.srcRect.zw;
    v_src = src;
    v_uv = src / p.srcRect.xy * p.params.zw; // zw: displayed / texture size (padded decoder surfaces)
    gl_Position = p.cornerToClip * vec4(corner, 0.0, 1.0);
}
