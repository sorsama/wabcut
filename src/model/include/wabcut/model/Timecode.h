#pragma once

// SMPTE timecode formatting and parsing.
//
// Labels count frames at the nominal integer rate (24 for 23.976, 30 for 29.97, 60 for 59.94). Drop-frame
// (29.97 and 59.94, and other x/1001 multiples of 30) skips labels to stay in step with the clock: at
// the start of every minute except each tenth minute, frame numbers 0-1 (29.97) or 0-3 (59.94) are not
// used. Drop-frame labels use ';' before the frames field: "00:01:00;02".

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace wabcut {

/// Nominal integer frames per second used for labels (round(num/den)), e.g. 30 for 30000/1001.
std::int64_t timecodeBase(Rate rate);

/// Labels dropped per minute in drop-frame mode, or 0 when `rate` has no drop-frame variant.
std::int64_t dropFramesPerMinute(Rate rate);

inline bool supportsDropFrame(Rate rate) { return dropFramesPerMinute(rate) > 0; }

/// "HH:MM:SS:FF" (or "HH:MM:SS;FF" for drop-frame) of the frame index. Negative frames get a leading '-'.
/// `dropFrame` is ignored for rates without a drop-frame variant. Hours are not wrapped at 24.
std::string framesToTimecode(std::int64_t frame, Rate rate, bool dropFrame);

/// Timecode of the frame that contains `t`.
std::string ticksToTimecode(Ticks t, Rate rate, bool dropFrame);

/// Parses a timecode into a frame index.
/// Accepts "HH:MM:SS:FF" with any of ':' ';' '.' ',' as separators, fewer fields counted from the right
/// ("SS:FF", "MM:SS:FF"), a leading '-' or '+', and bare digits packed in pairs from the right like
/// Premiere's timecode fields ("110" = 00:00:01:10). Fields other than the leading one must be in
/// range, and drop-frame labels that do not exist (e.g. "00:01:00;00") are rejected.
std::expected<std::int64_t, Error> parseTimecodeFrames(std::string_view text, Rate rate, bool dropFrame);

/// Parses a timecode into the tick position of the start of that frame.
std::expected<Ticks, Error> parseTimecode(std::string_view text, Rate rate, bool dropFrame);

} // namespace wabcut
