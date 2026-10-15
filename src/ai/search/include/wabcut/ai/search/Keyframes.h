#pragma once

// Shot-aware keyframes for visual search: the frames of a video that get an embedding. Every shot of the C.5 shot
// detection (ShotAnalysis) gets one keyframe in its middle; a shot longer than `interval` is cut into equal stretches
// of at most `interval` and gets one keyframe in the middle of each, so a long take still finds what happens in it.
// A search hit is the stretch its keyframe stands for.

#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <vector>

namespace wabcut::ai::search {

struct KeyframeOptions {
    Ticks interval = Ticks::fromSeconds(5); ///< longest stretch one keyframe stands for
    std::size_t maxKeyframes = 20'000;     ///< per video (a 28-hour stream at 5 s); the interval grows past it
};

struct Keyframe {
    Ticks time;      ///< source time of the frame to embed
    TimeRange range; ///< the stretch it stands for (a search hit's source range)
    TimeRange shot;  ///< the whole shot
};

/// Keyframes of [shots.start, shots.end) cut at shots.cuts. Empty for an empty stretch.
std::vector<Keyframe> planKeyframes(const analysis::ShotAnalysis& shots, const KeyframeOptions& options = {});

} // namespace wabcut::ai::search
