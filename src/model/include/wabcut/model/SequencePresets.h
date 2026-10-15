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
    std::string_view id;    ///< stable: "1080p60", "1440p144", "2160p240", "shorts", ...
    std::string_view name;  ///< "1080p 60 fps (YouTube)"
    std::string_view group; ///< "Landscape 16:9" or "Vertical 9:16"
    std::int32_t width = 0;
    std::int32_t height = 0;
    Rate frameRate;
};

/// Every preset: landscape 1080p, 1440p and 2160p at 30, 60, 120, 144 and 240 fps (and 59.94), then vertical
/// 1080x1920 at 60 ("shorts") and 30.
std::span<const SequencePreset> sequencePresets();
const SequencePreset* findSequencePreset(std::string_view id);

/// A new sequence's settings from a preset (48 kHz audio, no drop-frame display; `name` empty: the preset's name).
edit::SequenceSettings sequenceSettingsFor(const SequencePreset& preset, std::string name = {});

} // namespace wabcut
