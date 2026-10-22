// Scale to Frame Size, Set to Frame Size / Fill Frame and default media scaling (P.2, E5): FrameScaling.h.

#include <wabcut/model/FrameScaling.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace wabcut {

namespace {

constexpr std::array<std::pair<MediaScaling, std::string_view>, 4> kScalingNames{{
    {MediaScaling::None, "none"},
    {MediaScaling::ScaleToFrame, "scaleToFrameSize"},
    {MediaScaling::SetToFrame, "setToFrameSize"},
    {MediaScaling::SetToFill, "setToFillFrame"},
}};

} // namespace

std::string_view toString(MediaScaling scaling) {
    for (const auto& [value, name] : kScalingNames) {
        if (value == scaling) {
            return name;
        }
    }
    return "none";
}

std::optional<MediaScaling> mediaScalingFromString(std::string_view text) {
    for (const auto& [value, name] : kScalingNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<PictureSize> mediaPictureSize(const MediaItem& item, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= item.info.video.size()) {
        return std::nullopt;
    }
    VideoStreamInfo stream = item.info.video[static_cast<std::size_t>(index)];
    if (item.interpretation && item.interpretation->pixelAspect) {
        stream.sampleAspect = *item.interpretation->pixelAspect; // Interpret Footage's conformed pixel aspect
    }
    return streamPictureSize(stream);
}

std::optional<PictureSize> streamPictureSize(const VideoStreamInfo& v) {
    if (v.width <= 0 || v.height <= 0) {
        return std::nullopt;
    }
    const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
    const double w = static_cast<double>(v.width) * (std::isfinite(par) && par > 0.0 ? par : 1.0);
    const double h = static_cast<double>(v.height);
    const int quarter = ((v.rotationDegrees / 90) % 4 + 4) % 4;
    return quarter % 2 == 1 ? PictureSize{h, w} : PictureSize{w, h};
}

std::optional<PictureSize> clipPictureSize(const ProjectState& state, const Clip& clip) {
    if (clip.stream.kind != StreamKind::Video) {
        return std::nullopt;
    }
    if (clip.isNested()) {
        const Sequence* nested = state.findSequence(clip.sequence);
        if (!nested || nested->width <= 0 || nested->height <= 0) {
            return std::nullopt;
        }
        return PictureSize{static_cast<double>(nested->width), static_cast<double>(nested->height)};
    }
    const MediaItem* item = state.findMedia(clip.media);
    return item ? mediaPictureSize(*item, clip.stream.index) : std::nullopt;
}

double frameFitScale(PictureSize picture, std::int32_t width, std::int32_t height, bool fill) {
    if (!(picture.width > 0.0) || !(picture.height > 0.0) || width <= 0 || height <= 0) {
        return 1.0;
    }
    const double sx = static_cast<double>(width) / picture.width;
    const double sy = static_cast<double>(height) / picture.height;
    const double s = fill ? std::max(sx, sy) : std::min(sx, sy);
    return std::isfinite(s) && s > 0.0 ? s : 1.0;
}

double scaleToFrameFactor(const ProjectState& state, const Clip& clip, std::int32_t width, std::int32_t height) {
    if (!clip.content().scaleToFrame) {
        return 1.0;
    }
    return scaleToFrameFactor(clip, clipPictureSize(state, clip), width, height);
}

double scaleToFrameFactor(const Clip& clip, std::optional<PictureSize> picture, std::int32_t width, std::int32_t height) {
    if (!clip.content().scaleToFrame || !picture) {
        return 1.0;
    }
    return frameFitScale(*picture, width, height, false);
}

} // namespace wabcut

namespace wabcut::edit {

namespace {

/// Scale factors are stored with six significant digits after the point (Premiere shows Scale with one decimal), so a
/// saved project reads back the same value.
double roundedScale(double s) { return std::round(s * 1e6) / 1e6; }

/// The video clips among `clips` (no linked partners), or an error naming why none can change.
std::expected<std::vector<detail::ClipRef>, Error> videoClips(const Sequence& seq, std::span<const ClipId> clips) {
    auto refs = detail::resolveClips(seq, clips, LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    std::vector<detail::ClipRef> out;
    for (const detail::ClipRef& ref : *refs) {
        const Track* t = seq.findTrack(ref.track);
        if (t && t->kind == TrackKind::Video) {
            out.push_back(ref);
        }
    }
    if (out.empty()) {
        return std::unexpected(detail::invalidArgument("select a video clip: only video clips are scaled to the frame"));
    }
    return out;
}

} // namespace

EditOutcome setScaleToFrameSize(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                                bool enabled) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = videoClips(**seq, clips);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    detail::SequenceEditor ed(state, **seq);
    for (const detail::ClipRef& ref : *refs) {
        const Clip& current = ed.track(ref.track).clips[ref.index];
        if (current.content().scaleToFrame == enabled) {
            continue;
        }
        Clip& c = ed.mutableClip(ref.track, ref.index);
        ClipContent content = c.content();
        content.scaleToFrame = enabled;
        c.setContent(std::move(content));
    }
    return ed.finish();
}

EditOutcome setToFrameSize(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, FrameFit fit) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto refs = videoClips(**seq, clips);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    detail::SequenceEditor ed(state, **seq);
    for (const detail::ClipRef& ref : *refs) {
        const Clip& current = ed.track(ref.track).clips[ref.index];
        const auto picture = clipPictureSize(state, current);
        if (!picture) {
            continue; // nothing known to fit
        }
        const double scale = roundedScale(frameFitScale(*picture, (*seq)->width, (*seq)->height, fit == FrameFit::Fill));
        const ParamValue wanted(scale);
        if (!current.content().scaleToFrame && current.properties()[ParamId::Scale] == wanted) {
            continue;
        }
        Clip& c = ed.mutableClip(ref.track, ref.index);
        ClipContent content = c.content();
        content.scaleToFrame = false;
        content.properties[ParamId::Scale] = wanted;
        c.setContent(std::move(content));
    }
    return ed.finish();
}

std::expected<ProjectState, Error> setMediaScaling(const ProjectState& state, std::span<const MediaId> media,
                                                   MediaScaling scaling) {
    ProjectState next = state;
    for (const MediaId id : media) {
        const auto it = std::lower_bound(next.media.begin(), next.media.end(), id,
                                         [](const MediaPtr& m, MediaId value) { return m->id < value; });
        if (it == next.media.end() || (*it)->id != id) {
            return std::unexpected(detail::notFound("media " + std::to_string(id.value) + " does not exist"));
        }
        if ((*it)->scaling == scaling) {
            continue;
        }
        auto item = std::make_shared<MediaItem>(**it);
        item->scaling = scaling;
        *it = std::move(item);
    }
    return next;
}

std::shared_ptr<const ClipContent> scaledClipContent(const MediaItem& item, StreamRef stream, const Sequence& sequence,
                                                     std::shared_ptr<const ClipContent> base) {
    if (item.scaling == MediaScaling::None || stream.kind != StreamKind::Video) {
        return base;
    }
    const auto picture = mediaPictureSize(item, stream.index);
    if (!picture) {
        return base;
    }
    ClipContent content = base ? *base : ClipContent{};
    switch (item.scaling) {
    case MediaScaling::None:
        return base;
    case MediaScaling::ScaleToFrame:
        content.scaleToFrame = true;
        break;
    case MediaScaling::SetToFrame:
    case MediaScaling::SetToFill: {
        const double scale = roundedScale(
            frameFitScale(*picture, sequence.width, sequence.height, item.scaling == MediaScaling::SetToFill));
        if (scale == 1.0) {
            return base; // the picture already matches the frame
        }
        content.properties[ParamId::Scale] = ParamValue(scale);
        break;
    }
    }
    return std::make_shared<const ClipContent>(std::move(content));
}

} // namespace wabcut::edit
