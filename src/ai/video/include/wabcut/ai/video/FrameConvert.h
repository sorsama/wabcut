#pragma once

// Decoded frames <-> the runner's pictures (0.7, AI video effects). A model sees the R'G'B' code values the display would
// show, so a YUV frame is converted with ITS matrix and range (BT.601 / 709 / 2020, limited or full), chroma upsampled
// bilinearly from its siting; the processed picture goes back to the same matrix and range as YUV 4:4:4 at 10 bits (with an
// alpha plane when the media has one), so the colour pipeline reads it exactly as it reads the original and the grade sees
// the same colour space. RGB sources (RGBA8) stay RGBA8. 8-bit sources are processed at 16 bits and stored at 10.

#include <wabcut/ai/video/Picture.h>
#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Error.h>

#include <expected>
#include <memory>

namespace wabcut::ai::video {

/// The frame's colour (and alpha) as a picture. Errors: Unsupported (a native GPU payload or an unknown pixel format),
/// InvalidArgument.
std::expected<Picture, Error> pictureOf(const media::FrameHandle& frame);
/// A stored (processed) frame as a picture. Errors: Unsupported, InvalidArgument.
std::expected<Picture, Error> pictureOf(const StoredFrame& stored);

/// The picture as the planes to store, described like `like` (its matrix, range, colour tags, sample aspect, time base,
/// alpha mode); timing is `like`'s, the runner overwrites it. Errors: Unsupported (a pixel format of `like` it cannot
/// stand for), InvalidArgument.
std::expected<StoredFrame, Error> storedFrameOf(const Picture& picture, const media::FrameHandle& like);

/// A media frame that shows a stored frame: the planes are the stored frame's own, kept alive by the frame. `like` supplies
/// what the cache does not store (the display orientation, the source identity).
media::FrameHandle frameOf(std::shared_ptr<const StoredFrame> stored, const media::FrameHandle* like = nullptr);

} // namespace wabcut::ai::video
