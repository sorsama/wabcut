#pragma once

// The library's CPU twins by family (LibraryEffects.cpp dispatches to them).

#include <wabcut/render/reference/LibraryEffects.h>

namespace wabcut::render::reference::detail {

/// The Distort family (40..69): one output pixel of `in` (frame = canvas, identity Motion, divisor 1).
bool isDistort(LibraryEffect effect);
Rgba distortPixel(const Image& in, LibraryEffect op, const LibraryParams& q, int px, int py);

/// The Stylize family (70..99): the whole picture (several passes for some ops).
bool isStylize(LibraryEffect effect);
Image stylize(const Image& in, LibraryEffect op, const LibraryParams& q);

/// The Generate family (100..): the whole picture; `second` is the effect's second picture (null: none), `paths` its
/// masks in frame pixels (Scribble).
bool isGenerate(LibraryEffect effect);
Image generate(const Image& in, LibraryEffect op, const LibraryParams& q, const Image* second,
               std::span<const LibraryPath> paths = {});

/// 0.7: Skin Smoothing and AI Face Retouch (LibraryRetouch.cpp); `second` is the face map (linear R skin, G mouth, B eyes).
Image retouch(const Image& in, LibraryEffect op, const LibraryParams& q, const Image* second);

} // namespace wabcut::render::reference::detail
