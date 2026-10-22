#version 440

// Video transitions (0.2: Premiere's library plus the I.2 dissolve, dips and wipe) on canvas-size intermediates:
// linear Rec.709, premultiplied alpha, RGBA16F. tex0 = A (outgoing), tex1 = B (incoming); either may be transparent (a
// one-sided transition). Every formula has a CPU twin in render/reference/Transitions.cpp (the oracle the GPU tests
// compare against): change both together.
//
// Patterns are signed fields f in sequence pixels, B where f > 0: the edge's coverage is clamp(f / aa + 0.5, 0, 1) and a
// border of borderWidth px lies on A's side (-borderWidth < f <= 0). Reverse plays the pattern backwards (f = -f(1 - t)),
// and every pattern field is shifted by m (2t - 1), m = aa / 2 + borderWidth, so nothing of B shows at t = 0 and all of
// it at t = 1 whatever the softness and border.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pos;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform FxParams {
    mat4 cornerToClip;
    vec4 texel;    // xy = 1 / texture size, zw = texture size (canvas texture pixels)
    vec4 p0;       // progress, kind, wipe angle (radians), wipe feather (fraction)
    vec4 p1;       // rgb = colour (dips, light leak, flash; linear), w = preview divisor
    vec4 p2;       // border width (px), anti-aliasing width (px), reverse, direction
    vec4 toFrame0; // unused
    vec4 toFrame1; // unused
    vec4 frame;    // zw = canvas size (sequence px)
    ivec4 mode;    // y = 1 when textures are stored bottom-up
    vec4 p3;       // rgb = border colour (linear), w = seed
    vec4 p4;       // center (fractions), custom 0, custom 1
    vec4 p5;       // custom 2, custom 3
    vec4 p6;
    vec4 p7;
} p;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;

const float PI = 3.14159265358979;

// render::TransitionKind
const int K_CROSS_DISSOLVE = 0;
const int K_DIP_TO_COLOR = 1;
const int K_WIPE = 2;
const int K_ADDITIVE_DISSOLVE = 3;
const int K_NON_ADDITIVE_DISSOLVE = 4;
const int K_IRIS_BOX = 5;
const int K_IRIS_CROSS = 6;
const int K_IRIS_DIAMOND = 7;
const int K_IRIS_ROUND = 8;
const int K_PAGE_PEEL = 9;
const int K_PAGE_TURN = 10;
const int K_BAND_SLIDE = 11;
const int K_CENTER_SPLIT = 12;
const int K_PUSH = 13;
const int K_SLIDE = 14;
const int K_SPLIT = 15;
const int K_WHIP = 16;
const int K_BAND_WIPE = 17;
const int K_BARN_DOORS = 18;
const int K_CHECKER_WIPE = 19;
const int K_CHECKER_BOARD = 20;
const int K_CLOCK_WIPE = 21;
const int K_GRADIENT_WIPE = 22;
const int K_INSET = 23;
const int K_PAINT_SPLATTER = 24;
const int K_PINWHEEL = 25;
const int K_RADIAL_WIPE = 26;
const int K_RANDOM_BLOCKS = 27;
const int K_RANDOM_WIPE = 28;
const int K_SPIRAL_BOXES = 29;
const int K_VENETIAN_BLINDS = 30;
const int K_WEDGE_WIPE = 31;
const int K_ZIG_ZAG_BLOCKS = 32;
const int K_CROSS_ZOOM = 33;
const int K_ZOOM_BLUR = 34;
const int K_LIGHT_LEAK = 35;
const int K_FLASH = 36;
const int K_GLITCH = 37;

vec2 canvasSize() { return p.frame.zw; }

// The pixel's centre in sequence pixels (y down): exact pixel centres from gl_FragCoord.
vec2 pixelPos()
{
    vec2 fc = gl_FragCoord.xy;
    if (p.mode.y == 1)
        fc.y = p.texel.w - fc.y;
    return fc * p.frame.zw / p.texel.zw;
}

vec2 uvOf(vec2 px)
{
    vec2 uv = px / canvasSize();
    return p.mode.y == 1 ? vec2(uv.x, 1.0 - uv.y) : uv;
}

vec4 sampleA(vec2 px) { return textureLod(tex0, uvOf(px), 0.0); }
vec4 sampleB(vec2 px) { return textureLod(tex1, uvOf(px), 0.0); }

// Mirrors a position into the frame (zoom-outs show the picture reflected beyond its edges).
vec2 mirrorPos(vec2 px)
{
    vec2 s = canvasSize();
    vec2 q = mod(px, 2.0 * s);
    return vec2(q.x > s.x ? 2.0 * s.x - q.x : q.x, q.y > s.y ? 2.0 * s.y - q.y : q.y);
}

float coverage(float f) { return clamp(f / p.p2.y + 0.5, 0.0, 1.0); }

// A and B mixed by a pattern field, with the border on A's side.
vec4 patternMix(vec4 a, vec4 b, float f)
{
    float inB = coverage(f);
    float bw = p.p2.x;
    if (bw <= 0.0)
        return mix(a, b, inB);
    float inBorder = coverage(f + bw) - inB;
    return a * (1.0 - inB - inBorder) + b * inB + vec4(p.p3.rgb, 1.0) * inBorder;
}

// ---- Hashes (exact 32-bit integer maths, as the CPU twin) ----

uint hashU(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A value in (0, 1) for (i, j) and the transition's seed: ((hash % 1024) + 0.5) / 1024, exact in float.
float hash01(int i, int j, int salt)
{
    uint h = hashU(uint(i) + hashU(uint(j) + hashU(uint(int(p.p3.w + 0.5)) + uint(salt) * 0x9e3779b9u)));
    return (float(h & 1023u) + 0.5) / 1024.0;
}

// ---- Angles ----

// The clockwise angle (radians, -PI..PI) from direction u to vector d on screen (y down).
float cwAngle(vec2 u, vec2 d)
{
    float y = u.x * d.y - u.y * d.x;
    float x = dot(u, d);
    return x == 0.0 && y == 0.0 ? 0.0 : atan(y, x);
}

// Unit vector of a "From North|From East|From South|From West" choice (where a sweep starts).
vec2 fromDirection(int dir)
{
    if (dir == 1)
        return vec2(1.0, 0.0);
    if (dir == 2)
        return vec2(0.0, 1.0);
    if (dir == 3)
        return vec2(-1.0, 0.0);
    return vec2(0.0, -1.0);
}

// Travel vector of a "West to East|North to South|East to West|South to North" choice (4..7: the diagonals
// NW to SE, NE to SW, SE to NW, SW to NE; unit length on each axis).
vec2 travel(int dir)
{
    if (dir == 1)
        return vec2(0.0, 1.0);
    if (dir == 2)
        return vec2(-1.0, 0.0);
    if (dir == 3)
        return vec2(0.0, -1.0);
    if (dir == 4)
        return vec2(1.0, 1.0);
    if (dir == 5)
        return vec2(-1.0, 1.0);
    if (dir == 6)
        return vec2(-1.0, -1.0);
    if (dir == 7)
        return vec2(1.0, -1.0);
    return vec2(1.0, 0.0);
}

// A corner of "From Northwest|From Northeast|From Southeast|From Southwest".
vec2 cornerOf(int dir)
{
    vec2 s = canvasSize();
    if (dir == 1)
        return vec2(s.x, 0.0);
    if (dir == 2)
        return s;
    if (dir == 3)
        return vec2(0.0, s.y);
    return vec2(0.0);
}

// ---- Pattern fields at progress t (B where > 0) ----

float irisField(int kind, vec2 pos, float t)
{
    vec2 s = canvasSize();
    vec2 c = p.p4.xy * s;
    vec2 d = abs(pos - c);
    vec2 h = max(c, s - c); // half sizes reaching the farthest edges
    if (kind == K_IRIS_ROUND) {
        vec2 far = max(c, s - c);
        return t * length(far) - length(pos - c);
    }
    if (kind == K_IRIS_BOX)
        return min(t * h.x - d.x, t * h.y - d.y);
    if (kind == K_IRIS_DIAMOND) {
        vec2 ab = 2.0 * h;
        float n = 1.0 / sqrt(1.0 / (ab.x * ab.x) + 1.0 / (ab.y * ab.y));
        return (t - d.x / ab.x - d.y / ab.y) * n;
    }
    // Iris Cross: two arms, each as long as the frame by mid-transition and as thick by its end.
    float reach = min(1.0, 2.0 * t);
    float horizontal = min(reach * h.x - d.x, t * h.y - d.y);
    float vertical = min(reach * h.y - d.y, t * h.x - d.x);
    return max(horizontal, vertical);
}

// An angular sector [0, sweep) clockwise from the ray `start` around `c`, as a field (radius times angle).
float sectorField(vec2 pos, vec2 c, vec2 start, float sweep)
{
    vec2 d = pos - c;
    float r = length(d);
    float a = cwAngle(start, d); // -PI..PI
    float f = sweep <= PI ? min(a, sweep - a) : max(a, sweep - 2.0 * PI - a);
    return r * f;
}

float bandIndexField(vec2 pos, float t, int bands, bool vertical)
{
    vec2 s = canvasSize();
    float along = vertical ? pos.y : pos.x;
    float across = vertical ? pos.x : pos.y;
    float length_ = vertical ? s.y : s.x;
    float size = (vertical ? s.x : s.y) / float(bands);
    int band = int(floor(across / size));
    return (band % 2) == 0 ? t * length_ - along : along - (1.0 - t) * length_;
}

// A cell's wipe: `q` cell lengths of progress through a cell whose local coordinate along the wipe is `u` (0..1).
float cellWipe(float q, float u, float cellLength) { return (q - u) * cellLength; }

// Local coordinate (0..1) along a travel direction inside a cell, and the cell's length along it.
vec2 cellLocal(vec2 pos, vec2 cell, int dir)
{
    vec2 local = fract(pos / cell);
    if (dir == 1)
        return vec2(local.y, cell.y);
    if (dir == 2)
        return vec2(1.0 - local.x, cell.x);
    if (dir == 3)
        return vec2(1.0 - local.y, cell.y);
    return vec2(local.x, cell.x);
}

float checkerField(int kind, vec2 pos, float t)
{
    vec2 s = canvasSize();
    vec2 n = max(vec2(1.0), floor(p.p4.zw + 0.5));
    vec2 cell = s / n;
    ivec2 ij = ivec2(floor(pos / cell));
    bool odd = ((ij.x + ij.y) % 2) == 1;
    vec2 lc = cellLocal(pos, cell, int(p.p2.w + 0.5));
    float q;
    if (kind == K_CHECKER_WIPE)
        q = odd ? 1.5 * t - 0.5 : 1.5 * t;
    else
        q = odd ? clamp(2.0 * t - 1.0, 0.0, 1.0) : clamp(2.0 * t, 0.0, 1.0);
    return cellWipe(q, lc.x, lc.y);
}

// Spiral order of cell (i, j) in an nx x ny grid (clockwise from the top-left, outside in) and its travel direction.
ivec2 spiralOrder(int i, int j, int nx, int ny)
{
    int k = min(min(i, j), min(nx - 1 - i, ny - 1 - j));
    int w = nx - 2 * k;
    int h = ny - 2 * k;
    int before = nx * ny - w * h;
    if (j == k)
        return ivec2(before + (i - k), 0);
    if (i == nx - 1 - k)
        return ivec2(before + (w - 1) + (j - k), 1);
    if (j == ny - 1 - k)
        return ivec2(before + (w - 1) + (h - 1) + (nx - 1 - k - i), 2);
    return ivec2(before + 2 * (w - 1) + (h - 1) + (ny - 1 - k - j), 3);
}

float blocksField(int kind, vec2 pos, float t)
{
    vec2 s = canvasSize();
    vec2 n = max(vec2(1.0), floor(p.p4.zw + 0.5));
    int dir = int(p.p2.w + 0.5);
    if (kind == K_RANDOM_WIPE) {
        float rows = 18.0;
        n = vec2(max(1.0, floor(rows * s.x / s.y + 0.5)), rows);
    }
    vec2 cell = s / n;
    ivec2 ij = clamp(ivec2(floor(pos / cell)), ivec2(0), ivec2(n) - ivec2(1));
    int nx = int(n.x);
    int ny = int(n.y);
    if (kind == K_RANDOM_BLOCKS)
        return t > hash01(ij.x, ij.y, 1) ? 1e4 : -1e4;
    if (kind == K_RANDOM_WIPE) {
        const float spread = 4.0;
        int pos_ = dir == 1 ? ij.y : dir == 2 ? nx - 1 - ij.x : dir == 3 ? ny - 1 - ij.y : ij.x;
        float count = (dir == 1 || dir == 3) ? n.y : n.x;
        float reveal = (float(pos_) + spread * hash01(ij.x, ij.y, 2)) / (count + spread);
        return t > reveal ? 1e4 : -1e4;
    }
    ivec2 order; // x = index, y = travel direction (0 +x, 1 +y, 2 -x, 3 -y)
    if (kind == K_SPIRAL_BOXES) {
        order = spiralOrder(ij.x, ij.y, nx, ny);
    } else { // Zig-Zag Blocks: rows (or columns) back and forth
        bool columns = dir == 1 || dir == 3;
        int line = columns ? ij.x : ij.y;
        int along = columns ? ij.y : ij.x;
        int length_ = columns ? ny : nx;
        bool forward = (line % 2 == 0) == (dir == 0 || dir == 1);
        int at = forward ? along : length_ - 1 - along;
        int travelDir = columns ? (forward ? 1 : 3) : (forward ? 0 : 2);
        order = ivec2(line * length_ + at, travelDir);
    }
    vec2 lc = cellLocal(pos, cell, order.y);
    float q = t * float(nx * ny) - float(order.x);
    return cellWipe(q, lc.x, lc.y);
}

float splatterField(vec2 pos, float t)
{
    vec2 s = canvasSize();
    float diag = length(s);
    float f = -1e4;
    for (int i = 0; i < 28; ++i) {
        vec2 c = vec2(hash01(i, 0, 3), hash01(i, 1, 3)) * s;
        float rho = (0.12 + 0.2 * hash01(i, 2, 3)) * diag;
        float start = 0.6 * hash01(i, 3, 3);
        float grow = clamp((t - start) / (1.0 - start), 0.0, 1.0);
        vec2 d = pos - c;
        float wobble = 1.0 + 0.18 * sin(7.0 * cwAngle(vec2(1.0, 0.0), d) + 6.2831853 * hash01(i, 4, 3));
        f = max(f, grow * rho * wobble - length(d));
    }
    float flood = clamp((t - 0.8) / 0.2, 0.0, 1.0);
    return max(f, (2.0 * flood - 1.0) * diag);
}

float patternField(int kind, vec2 pos, float t)
{
    vec2 s = canvasSize();
    int dir = int(p.p2.w + 0.5);
    if (kind == K_IRIS_BOX || kind == K_IRIS_CROSS || kind == K_IRIS_DIAMOND || kind == K_IRIS_ROUND)
        return irisField(kind, pos, t);
    if (kind == K_CLOCK_WIPE)
        return sectorField(pos, 0.5 * s, fromDirection(dir), t * 2.0 * PI);
    if (kind == K_PINWHEEL) {
        int wedges = max(1, int(p.p4.z + 0.5));
        float w = 2.0 * PI / float(wedges);
        vec2 c = p.p4.xy * s;
        vec2 d = pos - c;
        float a = cwAngle(vec2(0.0, -1.0), d);
        float local = mod(a, w);
        float sweep = t * w;
        float f = local < sweep ? min(local, sweep - local) : -min(local - sweep, w - local);
        return length(d) * f;
    }
    if (kind == K_WEDGE_WIPE) {
        vec2 d = pos - 0.5 * s;
        return length(d) * (t * PI - abs(cwAngle(fromDirection(dir), d)));
    }
    if (kind == K_RADIAL_WIPE) {
        vec2 corner = cornerOf(dir);
        vec2 start = dir == 1 ? vec2(0.0, 1.0) : dir == 2 ? vec2(-1.0, 0.0) : dir == 3 ? vec2(0.0, -1.0) : vec2(1.0, 0.0);
        vec2 d = pos - corner;
        return length(d) * (t * 0.5 * PI - cwAngle(start, d));
    }
    if (kind == K_INSET) {
        vec2 corner = cornerOf(dir);
        vec2 d = abs(pos - corner);
        return min(t * s.x - d.x, t * s.y - d.y);
    }
    if (kind == K_BARN_DOORS)
        return dir == 1 ? t * 0.5 * s.y - abs(pos.y - 0.5 * s.y) : t * 0.5 * s.x - abs(pos.x - 0.5 * s.x);
    if (kind == K_BAND_WIPE)
        return bandIndexField(pos, t, max(1, int(p.p4.z + 0.5)), dir == 1);
    if (kind == K_VENETIAN_BLINDS) {
        int bands = max(1, int(p.p4.z + 0.5));
        bool vertical = dir == 0 || dir == 2; // West to East / East to West: vertical slats
        float size = (vertical ? s.x : s.y) / float(bands);
        float along = vertical ? pos.x : pos.y;
        float local = along - floor(along / size) * size;
        if (dir == 2 || dir == 3)
            local = size - local;
        return t * size - local;
    }
    if (kind == K_CHECKER_WIPE || kind == K_CHECKER_BOARD)
        return checkerField(kind, pos, t);
    if (kind == K_RANDOM_BLOCKS || kind == K_RANDOM_WIPE || kind == K_SPIRAL_BOXES || kind == K_ZIG_ZAG_BLOCKS)
        return blocksField(kind, pos, t);
    if (kind == K_PAINT_SPLATTER)
        return splatterField(pos, t);
    return 0.0;
}

// ---- Moving pictures ----

// Whip: B pushes A out along `v` (unit axis); the picture at `pos` after `offset` px of travel (B behind its leading
// edge, `offset` from the side it enters).
vec4 pushAt(vec2 pos, vec2 v, float offset)
{
    vec2 s = canvasSize();
    float len = abs(v.x) > 0.5 ? s.x : s.y;
    float along = dot(pos, v) + (v.x + v.y < 0.0 ? len : 0.0); // 0 at the entry side
    return offset > along ? sampleB(pos - v * offset + v * len) : sampleA(pos - v * offset);
}

vec4 slideMix(vec2 pos, float t)
{
    vec2 s = canvasSize();
    int dir = int(p.p2.w + 0.5);
    vec2 v = travel(dir);
    bool reverse = p.p2.z > 0.5;
    // Slide: B's frame moves in from behind its entry edges; Reverse: A's frame moves out the same way, over B.
    vec2 o = reverse ? t * v * s : (t - 1.0) * v * s;
    vec2 q = pos - o;
    float inside = min(min(q.x, s.x - q.x), min(q.y, s.y - q.y));
    if (reverse)
        return patternMix(sampleA(q), sampleB(pos), -inside + (0.5 * p.p2.y + p.p2.x) * (2.0 * t - 1.0));
    return patternMix(sampleA(pos), sampleB(q), inside + (0.5 * p.p2.y + p.p2.x) * (2.0 * t - 1.0));
}

vec4 bandSlideMix(vec2 pos, float t)
{
    vec2 s = canvasSize();
    bool vertical = int(p.p2.w + 0.5) == 1;
    int bands = max(1, int(p.p4.z + 0.5));
    float size = (vertical ? s.x : s.y) / float(bands);
    int band = int(floor((vertical ? pos.x : pos.y) / size));
    float length_ = vertical ? s.y : s.x;
    float along = vertical ? pos.y : pos.x;
    bool fromStart = band % 2 == 0;
    float shift = fromStart ? (1.0 - t) * length_ : -(1.0 - t) * length_;
    float f = fromStart ? t * length_ - along : along - (1.0 - t) * length_;
    vec2 q = vertical ? vec2(pos.x, pos.y + shift) : vec2(pos.x + shift, pos.y);
    return patternMix(sampleA(pos), sampleB(q), f + (0.5 * p.p2.y + p.p2.x) * (2.0 * t - 1.0));
}

vec4 splitMix(int kind, vec2 pos, float t)
{
    vec2 s = canvasSize();
    vec2 h = 0.5 * s;
    vec2 side = vec2(pos.x < h.x ? -1.0 : 1.0, pos.y < h.y ? -1.0 : 1.0);
    vec2 shift;
    float insideA;
    if (kind == K_CENTER_SPLIT) {
        shift = side * t * h;
        vec2 q = pos - shift;
        vec2 in2 = vec2(side.x < 0.0 ? h.x - q.x : q.x - h.x, side.y < 0.0 ? h.y - q.y : q.y - h.y);
        insideA = min(in2.x, in2.y);
    } else if (int(p.p2.w + 0.5) == 1) { // Split, vertical: top and bottom halves
        shift = vec2(0.0, side.y * t * h.y);
        float qy = pos.y - shift.y;
        insideA = side.y < 0.0 ? h.y - qy : qy - h.y;
    } else {
        shift = vec2(side.x * t * h.x, 0.0);
        float qx = pos.x - shift.x;
        insideA = side.x < 0.0 ? h.x - qx : qx - h.x;
    }
    return patternMix(sampleA(pos - shift), sampleB(pos), -insideA + (0.5 * p.p2.y + p.p2.x) * (2.0 * t - 1.0));
}

vec4 whipMix(vec2 pos, float t)
{
    int dir = int(p.p2.w + 0.5);
    vec2 v = travel(dir);
    vec2 s = canvasSize();
    float len = abs(v.x) > 0.5 ? s.x : s.y;
    float e = t * t * (3.0 - 2.0 * t);
    float speed = 6.0 * t * (1.0 - t);
    float blur = p.p4.z / 100.0 * speed * len * 0.25;
    const int taps = 24;
    vec4 acc = vec4(0.0);
    for (int i = 0; i < taps; ++i) {
        float k = (float(i) + 0.5) / float(taps) - 0.5;
        acc += pushAt(pos + v * (k * blur), v, e * len);
    }
    return acc / float(taps);
}

// Zooms: the picture scaled by `scale` about `c`, blurred radially over `blur` (a fraction of the scale).
vec4 zoomSample(bool fromA, vec2 pos, vec2 c, float scale, float blur, bool mirror)
{
    const int taps = 16;
    vec4 acc = vec4(0.0);
    for (int i = 0; i < taps; ++i) {
        float k = 1.0 + blur * float(i) / float(taps - 1);
        vec2 q = c + (pos - c) / (scale * k);
        if (mirror)
            q = mirrorPos(q);
        acc += fromA ? sampleA(q) : sampleB(q);
    }
    return acc / float(taps);
}

vec4 zoomMix(int kind, vec2 pos, float t)
{
    vec2 c = p.p4.xy * canvasSize();
    bool out_ = kind == K_ZOOM_BLUR && int(p.p2.w + 0.5) == 1;
    float strength = kind == K_CROSS_ZOOM ? 1.0 : p.p4.z / 50.0;
    float w = smoothstep(0.45, 0.55, t);
    vec4 a = vec4(0.0);
    vec4 b = vec4(0.0);
    // A zooms towards the cut, B continues from the same zoom back to rest (z = 1 at the cut, 0 at the ends).
    if (w < 1.0) {
        float z = 2.0 * t;
        float scale = out_ ? 1.0 / (1.0 + 3.0 * z * z) : 1.0 + 3.0 * z * z;
        a = zoomSample(true, pos, c, scale, 0.15 * strength * z, out_);
    }
    if (w > 0.0) {
        float z = 2.0 * (1.0 - t);
        float scale = out_ ? 1.0 / (1.0 + 3.0 * z * z) : 1.0 + 3.0 * z * z;
        b = zoomSample(false, pos, c, scale, 0.15 * strength * z, out_);
    }
    return mix(a, b, w);
}

// Page Peel / Page Turn: the page (A) lifts from a corner; behind the fold B shows, the flap folds back over A.
vec4 pageMix(int kind, vec2 pos, float t)
{
    vec2 s = canvasSize();
    int dir = int(p.p2.w + 0.5);
    bool reverse = p.p2.z > 0.5;
    float tt = reverse ? 1.0 - t : t;
    vec2 k = cornerOf(dir);
    vec2 n = normalize(2.0 * k - s); // towards the lifted corner
    float extent = length(s);
    float margin = 0.5 * p.p2.y;
    float c = tt * (extent + 2.0 * margin) - margin; // the fold, from just before the corner to just past the far one
    float tau = dot(k - pos, n); // 0 at the corner, `extent` at the opposite one
    vec2 mirrored = pos + 2.0 * (tau - c) * n;
    float tauS = 2.0 * c - tau;
    vec4 page = reverse ? sampleB(pos) : sampleA(pos);
    vec4 under = reverse ? sampleA(pos) : sampleB(pos);
    vec4 back = reverse ? sampleB(mirrored) : sampleA(mirrored);
    float edge = min(min(mirrored.x, s.x - mirrored.x), min(mirrored.y, s.y - mirrored.y));
    float flap = coverage(tau - c) * coverage(edge) * coverage(tauS);
    vec4 flapColor;
    if (kind == K_PAGE_TURN) {
        flapColor = vec4(back.rgb * 0.8, back.a);
    } else {
        float curl = 0.55 + 0.45 * clamp((tau - c) / (0.06 * extent), 0.0, 1.0);
        flapColor = vec4(vec3(0.72) * curl * back.a, back.a);
        under = vec4(under.rgb * (1.0 - 0.45 * (1.0 - tt) * exp(-max(c - tau, 0.0) / (0.03 * extent))), under.a);
    }
    vec4 top = mix(page, flapColor, flap);
    return mix(top, under, coverage(c - tau));
}

vec4 glitchMix(vec2 pos, float t)
{
    vec2 s = canvasSize();
    float sine = max(0.0, sin(PI * t));
    float g = p.p4.z / 100.0 * sine * sine;
    int step_ = int(floor(t * 24.0));
    int band = int(floor(pos.y / (s.y / 32.0)));
    float dx = 0.0;
    if (hash01(band, step_, 5) < 0.35 * g * 2.0)
        dx = (hash01(band, step_, 6) - 0.5) * 0.2 * s.x * g;
    ivec2 cell = ivec2(floor(pos / (s / vec2(16.0, 9.0))));
    bool flicker = hash01(cell.x + 16 * cell.y, step_, 7) < 0.15 * g;
    bool useB = (t >= 0.5) != flicker;
    float split = p.p4.w * g;
    vec2 q = vec2(pos.x + dx, pos.y);
    vec4 r = useB ? sampleB(q + vec2(split, 0.0)) : sampleA(q + vec2(split, 0.0));
    vec4 c = useB ? sampleB(q) : sampleA(q);
    vec4 b = useB ? sampleB(q - vec2(split, 0.0)) : sampleA(q - vec2(split, 0.0));
    return vec4(r.r, c.g, b.b, c.a);
}

vec4 lightLeakMix(vec2 pos, float t, vec4 a, vec4 b)
{
    vec2 s = canvasSize();
    vec4 base = mix(a, b, smoothstep(0.35, 0.65, t));
    float sine = max(0.0, sin(PI * t));
    float intensity = p.p4.z * sine * sqrt(sine);
    vec3 leak = vec3(0.0);
    for (int k = 0; k < 3; ++k) {
        float fk = float(k);
        vec2 c = vec2(0.15 + 0.7 * fract(fk * 0.37 + 0.8 * t), 0.2 + 0.6 * fract(fk * 0.61 + 0.3)) * s;
        float sigma = (0.3 + 0.1 * fk) * s.y;
        vec2 d = pos - c;
        vec3 tint = k == 0 ? vec3(1.0) : (k == 1 ? vec3(1.0, 0.8, 0.6) : vec3(1.0, 0.9, 0.8));
        leak += p.p1.rgb * tint * exp(-dot(d, d) / (2.0 * sigma * sigma));
    }
    return vec4(base.rgb + intensity * leak * base.a, base.a);
}

vec4 flashMix(float t, vec4 a, vec4 b)
{
    vec4 base = t < 0.5 ? a : b;
    float bump = 1.0 - abs(2.0 * t - 1.0);
    float f = clamp(p.p4.z * bump * bump, 0.0, 1.0);
    vec4 bright = vec4(base.rgb * (1.0 + 3.0 * f), base.a);
    return mix(bright, vec4(p.p1.rgb, 1.0), f * f);
}

vec4 gradientMix(vec2 pos, float t, vec4 a, vec4 b)
{
    vec2 s = canvasSize();
    int type = int(p.p4.z + 0.5);
    float g;
    if (type == 1)
        g = pos.y / s.y;
    else if (type == 2)
        g = length(pos - 0.5 * s) / length(0.5 * s);
    else if (type == 3)
        g = 0.5 * (pos.x / s.x + pos.y / s.y);
    else
        g = pos.x / s.x;
    bool reverse = p.p2.z > 0.5;
    float tt = reverse ? 1.0 - t : t;
    float softness = p.p4.w / 100.0;
    float m;
    if (softness > 0.0) {
        m = clamp((tt * (1.0 + softness) - g) / softness, 0.0, 1.0);
    } else {
        float length_ = type == 1 ? s.y : (type == 2 ? length(0.5 * s) : s.x);
        float aa = p.p2.y;
        m = clamp(((tt - g) * length_ + 0.5 * aa * (2.0 * tt - 1.0)) / aa + 0.5, 0.0, 1.0);
    }
    if (reverse)
        m = 1.0 - m;
    return mix(a, b, m);
}

void main()
{
    float t = clamp(p.p0.x, 0.0, 1.0);
    int kind = int(p.p0.y + 0.5);
    vec4 a = textureLod(tex0, v_uv, 0.0);
    vec4 b = textureLod(tex1, v_uv, 0.0);
    vec2 pos = pixelPos();
    float wa = min(1.0, 2.0 * (1.0 - t));
    float wb = min(1.0, 2.0 * t);
    if (kind == K_CROSS_DISSOLVE) {
        fragColor = mix(a, b, t);
    } else if (kind == K_DIP_TO_COLOR) {
        vec4 color = vec4(p.p1.rgb, 1.0);
        fragColor = t < 0.5 ? mix(a, color, 2.0 * t) : mix(color, b, 2.0 * t - 1.0);
    } else if (kind == K_ADDITIVE_DISSOLVE) {
        fragColor = vec4(a.rgb * wa + b.rgb * wb, min(1.0, a.a * wa + b.a * wb));
    } else if (kind == K_NON_ADDITIVE_DISSOLVE) {
        fragColor = max(a * wa, b * wb);
    } else if (kind == K_WIPE) {
        // A straight edge at an angle (I.2), with Premiere's border, reverse and anti-aliasing (0.2).
        bool reverse = p.p2.z > 0.5;
        float tt = reverse ? 1.0 - t : t;
        vec2 dirv = vec2(cos(p.p0.z), sin(p.p0.z));
        vec2 size = canvasSize();
        float s0 = min(0.0, dirv.x * size.x) + min(0.0, dirv.y * size.y);
        float s1 = max(0.0, dirv.x * size.x) + max(0.0, dirv.y * size.y);
        float len = s1 - s0;
        float w = p.p0.w * len;
        float e = s0 - 0.5 * w + tt * (len + w);
        float f = e - dot(pos, dirv);
        if (reverse)
            f = -f;
        if (w > 0.0) {
            f += p.p2.x * (2.0 * t - 1.0); // the border clears the frame at the ends
            float m = clamp(f / w + 0.5, 0.0, 1.0);
            float bw = p.p2.x;
            float inBorder = bw > 0.0 ? clamp((f + bw) / w + 0.5, 0.0, 1.0) - m : 0.0;
            fragColor = a * (1.0 - m - inBorder) + b * m + vec4(p.p3.rgb, 1.0) * inBorder;
        } else {
            fragColor = patternMix(a, b, f + (0.5 * p.p2.y + p.p2.x) * (2.0 * t - 1.0));
        }
    } else if (kind == K_PUSH) {
        // Reverse pushes the other way.
        int dir = int(p.p2.w + 0.5);
        if (p.p2.z > 0.5)
            dir = (dir + 2) % 4;
        vec2 v = travel(dir);
        vec2 s = canvasSize();
        float len = abs(v.x) > 0.5 ? s.x : s.y;
        float shift = t * len;
        float f = shift - (dot(pos, v) + (v.x + v.y < 0.0 ? len : 0.0)) + 0.5 * p.p2.y * (2.0 * t - 1.0);
        fragColor = mix(sampleA(pos - v * shift), sampleB(pos - v * shift + v * len), coverage(f));
    } else if (kind == K_SLIDE) {
        fragColor = slideMix(pos, t);
    } else if (kind == K_BAND_SLIDE) {
        fragColor = bandSlideMix(pos, t);
    } else if (kind == K_CENTER_SPLIT || kind == K_SPLIT) {
        fragColor = splitMix(kind, pos, t);
    } else if (kind == K_WHIP) {
        fragColor = whipMix(pos, t);
    } else if (kind == K_CROSS_ZOOM || kind == K_ZOOM_BLUR) {
        fragColor = zoomMix(kind, pos, t);
    } else if (kind == K_PAGE_PEEL || kind == K_PAGE_TURN) {
        fragColor = pageMix(kind, pos, t);
    } else if (kind == K_GLITCH) {
        fragColor = glitchMix(pos, t);
    } else if (kind == K_LIGHT_LEAK) {
        fragColor = lightLeakMix(pos, t, a, b);
    } else if (kind == K_FLASH) {
        fragColor = flashMix(t, a, b);
    } else if (kind == K_GRADIENT_WIPE) {
        fragColor = gradientMix(pos, t, a, b);
    } else {
        // The pattern wipes: a field at progress t (reversed: -f(1 - t)), shifted so the ends are exact.
        bool reverse = p.p2.z > 0.5;
        float f = patternField(kind, pos, reverse ? 1.0 - t : t);
        if (reverse)
            f = -f;
        float margin = 0.5 * p.p2.y + p.p2.x;
        fragColor = patternMix(a, b, f + margin * (2.0 * t - 1.0));
    }
}
