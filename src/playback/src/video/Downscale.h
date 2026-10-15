#pragma once

// Downscale (private): reduced-resolution preview decoding. FFmpeg's `lowres` only exists for a few legacy
// decoders (none of H.264, HEVC, ProRes, DNxHR), so preview frames are box-filtered right after decoding instead:
// the frame caches, uploads and composites then work on a half or a quarter of the pixels.

#include <wabcut/media/FrameHandle.h>

namespace wabcut::playback::detail {

/// `frame` box-filtered down by `divisor` (2 or 4) in each dimension: every CPU-plane layout (planar YUV with or
/// without alpha, semi-planar NV12/P010, RGBA8), sizes rounded up (odd sizes keep their last row/column). Display
/// size and pixel aspect follow (the displayed shape stays exact); timing, colour and identity are kept. Native GPU
/// payloads, invalid frames and divisors other than 2 and 4 come back unchanged.
media::FrameHandle downscaleFrame(const media::FrameHandle& frame, int divisor);

/// The box-filter divisor (1, 2 or 4) that brings a frame `frameHeight` pixels high, of a stream `streamHeight`
/// pixels high, down to about streamHeight / `targetDivisor` (never below it): 1 when it already is that small.
int extraDivisor(int streamHeight, int frameHeight, int targetDivisor);

} // namespace wabcut::playback::detail
