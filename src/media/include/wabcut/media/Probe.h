#pragma once

// Media probing: what is in a file, without decoding it.

#include <wabcut/model/Error.h>
#include <wabcut/model/MediaInfo.h>

#include <expected>
#include <string>

namespace wabcut::media {

/// Reads the container and stream headers of `path` (UTF-8) and fills a MediaInfo.
///
/// - Durations are exact conversions of the container/stream time bases to Ticks.
/// - VideoStreamInfo::frameRate is the exact nominal rate. Rates within 0.02% of a broadcast rate snap to it
///   (e.g. 2997/100 -> 30000/1001). variableFrameRate is derived from the demuxer's sample index when the
///   container has one (MP4/MOV), otherwise from disagreement between FFmpeg's average and real base frame
///   rates.
/// - VideoStreamInfo::color reports the stream's tags as written (Unknown when untagged).
/// - rotationDegrees is the clockwise rotation (0/90/180/270) to apply for display, read from the display
/// matrix.
/// - Embedded cover art (attached pictures) is not listed as a video stream.
///
/// Errors: NotFound (no such file), Unsupported (not a media file FFmpeg can read), IoError.
/// Thread-safe: may be called concurrently from any thread.
std::expected<MediaInfo, Error> probe(const std::string& path);

} // namespace wabcut::media
