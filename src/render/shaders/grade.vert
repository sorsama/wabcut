#version 440

// Colour grade passes (grade.frag): a full-canvas quad over an intermediate, like fx.vert, reading the prefix of the
// grade's own uniform block (cornerToClip ... mode). The block is declared whole, member for member as in grade.frag:
// OpenGL links the stages of a program only when a uniform block they share is declared the same in each.

layout(location = 0) in vec2 corner; // unit square, (0,0) = canvas top-left

layout(location = 0) out vec2 v_uv;  // texture coordinate of the same canvas point in the inputs
layout(location = 1) out vec2 v_pos; // canvas position, 0..1 with a top-left origin

layout(std140, binding = 0) uniform GradeParams {
    mat4 cornerToClip;
    vec4 texel;
    vec4 toFrame0;
    vec4 toFrame1;
    vec4 frame;
    ivec4 mode; // y: 1 = textures are stored bottom-up (QRhi::isYUpInFramebuffer)
    ivec4 lut;  // the rest: fragment stage only (grade.frag)
    ivec4 lutSize;
    vec4 in1dMin;
    vec4 in1dMax;
    vec4 in3dMin;
    vec4 in3dMax;
    vec4 look1dMin;
    vec4 look1dMax;
    vec4 look3dMin;
    vec4 look3dMax;
    mat4 wb;
    mat4 hslWb;
    vec4 basic;
    vec4 toneY;
    vec4 toneM0;
    vec4 toneM1;
    vec4 creative;
    vec4 shadowTint;
    vec4 highlightTint;
    vec4 cdlSlope;
    vec4 cdlOffset;
    vec4 cdlPower;
    vec4 hslHue;
    vec4 hslSat;
    vec4 hslLuma;
    vec4 hslCorr;
    vec4 hslOffset;
    vec4 vignette;
    vec4 split;
} p;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    v_pos = corner;
    v_uv = p.mode.y == 1 ? vec2(corner.x, 1.0 - corner.y) : corner;
    gl_Position = p.cornerToClip * vec4(corner, 0.0, 1.0);
}
