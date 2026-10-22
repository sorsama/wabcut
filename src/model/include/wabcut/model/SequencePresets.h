#pragma once

// Sequence presets (C.15, gaming): the frame sizes and rates gaming videos are made at - YouTube landscape at 1080p, 1440p
// and 4K from 30 to 240 fps, and vertical 9:16 for Shorts / TikTok / Reels. Every rate is exact in ticks
// (Rate::isExact: one frame is a whole number of ticks), so frame n of any preset is exactly n x the frame duration
// however long the video.

#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace wabcut {

struct SequencePreset {
    std::string_view id;    ///< stable: "1080p60", "1440p144", "2160p240", "shorts", "1080p29.97df", "square1080p30", ...
    std::string_view name;  ///< "1080p 60 fps (YouTube)"
    std::string_view group; ///< "Landscape 16:9", "Vertical 9:16" or "Square 1:1"
    std::int32_t width = 0;
    std::int32_t height = 0;
    Rate frameRate;
    /// P.2 (E5): shows drop-frame timecode (29.97 and 59.94 only).
    bool dropFrame = false;
};

/// Every preset (P.2 extends C.15's gaming list with Premiere's broadcast and film rates): landscape 720p, 1080p, 1440p
/// and UHD 2160p at 23.976, 24, 25, 29.97 (drop-frame and non-drop), 30, 50, 59.94 (drop and non-drop), 60, and 1080p
/// to 2160p also at 120, 144 and 240 fps; vertical 1080x1920 for Shorts / TikTok / Reels ("shorts" at 60, "shorts30");
/// square 1080x1080.
std::span<const SequencePreset> sequencePresets();
const SequencePreset* findSequencePreset(std::string_view id);

/// A new sequence's settings from a preset (48 kHz audio, the preset's drop-frame display; `name` empty: the preset's
/// name).
edit::SequenceSettings sequenceSettingsFor(const SequencePreset& preset, std::string name = {});

} // namespace wabcut
