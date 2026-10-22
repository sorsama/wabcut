// Time Interpolation (P.2): names, when it applies, and its edit (TimeInterpolation.h, MotionOps.h).

#include <wabcut/model/MotionOps.h>
#include <wabcut/model/TimeInterpolation.h>

#include <wabcut/model/Clip.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/MediaInfo.h>

#include "SequenceEditor.h"

#include <array>
#include <string>
#include <utility>

namespace wabcut {

namespace {

struct Name {
    TimeInterpolation mode;
    std::string_view id;
    std::string_view label;
};

constexpr std::array<Name, 3> kNames{{
    {TimeInterpolation::FrameSampling, "frameSampling", "Frame Sampling"},
    {TimeInterpolation::FrameBlending, "frameBlending", "Frame Blending"},
    {TimeInterpolation::OpticalFlow, "opticalFlow", "Optical Flow"},
}};

} // namespace

std::string_view toString(TimeInterpolation mode) {
    for (const Name& n : kNames) {
        if (n.mode == mode) {
            return n.id;
        }
    }
    return kNames.front().id;
}

std::optional<TimeInterpolation> timeInterpolationFromString(std::string_view text) {
    for (const Name& n : kNames) {
        if (n.id == text) {
            return n.mode;
        }
    }
    return std::nullopt;
}

std::string_view displayName(TimeInterpolation mode) {
    for (const Name& n : kNames) {
        if (n.mode == mode) {
            return n.label;
        }
    }
    return kNames.front().label;
}

bool retimesFrames(const Clip& clip, const MediaItem& item, Rate sequenceRate) {
    if (clip.stream.kind != StreamKind::Video || clip.isNested() || clip.isFreeze() || item.isGenerator() ||
        item.info.isStillImage || clip.stream.index < 0 ||
        clip.stream.index >= static_cast<std::int32_t>(item.info.video.size())) {
        return false;
    }
    if (clip.isRemapped() || clip.reversed) {
        return true;
    }
    // Interpret Footage (EditingOps.h): an item interpreted at another frame rate plays at its conform speed (a 120 fps
    // capture interpreted as 30 fps: 1/4, every frame shown), which is the clip's "100 %"; its frames are then at the
    // interpreted rate.
    const Speed conform = edit::conformSpeed(item);
    const bool unity = conform.isUnity() ? clip.speed.isUnity()
                                         : clip.speed.num > 0 && std::int64_t{clip.speed.num} * conform.den ==
                                                                     std::int64_t{clip.speed.den} * conform.num;
    if (!unity) {
        return true;
    }
    const Rate media = conform.isUnity() ? item.info.video[static_cast<std::size_t>(clip.stream.index)].frameRate
                                         : *item.interpretation->frameRate;
    return media.isValid() && sequenceRate.isValid() && !(media.normalized() == sequenceRate.normalized());
}

namespace edit {

EditOutcome setTimeInterpolation(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                                 TimeInterpolation mode, LinkMode links) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = detail::resolveClips(**seq, clips, links);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    detail::SequenceEditor ed(state, **seq);
    bool video = false;
    for (const detail::ClipRef& ref : *refs) {
        const Clip& c = ed.track(ref.track).clips[ref.index];
        if (c.stream.kind != StreamKind::Video) {
            continue; // an audio partner: Time Interpolation is the picture's
        }
        video = true;
        if (c.content().timeInterpolation == mode) {
            continue;
        }
        Clip& m = ed.mutableClip(ref.track, ref.index);
        ClipContent content = m.content();
        content.timeInterpolation = mode;
        m.setContent(std::move(content));
    }
    if (!video) {
        return std::unexpected(detail::invalidArgument("Time Interpolation applies to video clips; none was given"));
    }
    return ed.finish();
}

} // namespace edit

} // namespace wabcut
