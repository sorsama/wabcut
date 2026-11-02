#pragma once

// Strokes of the 0.6 generators Lightning and Scribble (render/EffectLibrary.h, docs/effects.md): the geometry both the
// compositor and the CPU twins draw, so preview, export and the reference agree to the segment. Pure C++, double
// precision, deterministic for the parameters (the seed and the curve time included).
//
// The compositor packs the strokes into a data texture (packStrokes: RGBA32F, a header, a uniform grid of cells listing
// the segments near each, then the segments) that fx_generate.frag reads as tex1; the CPU twins shade every segment
// directly (strokeShade). Both stop a segment's influence at the same distance (StrokeStyle::reach), so the grid never
// drops a segment that changes a pixel.
//
// Units: frame pixels (the layer's frame; the shader maps canvas positions into it), widths full widths.

#include <wabcut/render/reference/LibraryEffects.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace wabcut::render::reference {

/// One stroke segment from a to b, full width `wa` at a and `wb` at b (frame pixels).
struct StrokeSegment {
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
    double wa = 1.0, wb = 1.0;
};

/// A closed polygon in frame pixels.
using StrokePolygon = LibraryPath;

/// How a stroke list is shaded. Lightning: a bright core of coreWidth x the width (antialiased) inside a Gaussian glow
/// of sigma = width / 2; Scribble: a solid antialiased line of the width.
enum class StrokeKind : std::uint8_t { Line = 0, Glow = 1 };

/// How far beyond a segment (frame pixels, at its widest) its shading reaches: the culling distance of both paths.
double strokeReach(StrokeKind kind, double width, double coreWidth);

/// Lightning's bolt and branches for `q` (EffectLibrary.h order, q[31] = the curve time) on a frame of `width` x `height`.
std::vector<StrokeSegment> lightningStrokes(const LibraryParams& q, double width, double height);

/// Scribble's strokes for `q` (q[31] = the curve time): inside or along `paths` (the effect's masks, frame pixels;
/// Single Mask uses the first), the frame's rectangle when there is none.
std::vector<StrokeSegment> scribbleStrokes(const LibraryParams& q, std::span<const StrokePolygon> paths, double width,
                                           double height);

/// The shading of a stroke list at frame position (x, y): {inside (core or line coverage), outside (glow)}, each 0..1.
std::array<double, 2> strokeShade(std::span<const StrokeSegment> segments, StrokeKind kind, double coreWidth, double x,
                                  double y);

/// The data texture of a stroke list (RGBA32F texels, `width` x `height`, row-major, top row first):
///   texel 0: segment count, grid columns, grid rows, the first index texel
///   texel 1: the grid's origin x, y and its cell width, height (frame pixels)
///   texel 2: the first segment texel, the kind, the core width, the reach
///   texels 3..: per cell (row-major) its first index and count into the index list
///   index list: four segment numbers per texel
///   segments: two texels each, (ax, ay, bx, by) and (wa, wb, 0, 0)
/// An empty list packs a header with no segments.
struct StrokeTexture {
    int width = 0;
    int height = 0;
    std::vector<float> texels; ///< width * height * 4
};
inline constexpr int kStrokeTextureWidth = 1024;
/// At most this many segments are kept (the rest of a list is dropped, the first ones drawn).
inline constexpr std::size_t kMaxStrokeSegments = 8192;
StrokeTexture packStrokes(std::span<const StrokeSegment> segments, StrokeKind kind, double coreWidth);

} // namespace wabcut::render::reference
