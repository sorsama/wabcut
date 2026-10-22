#include <wabcut/model/FieldOptions.h>
#include <wabcut/model/FieldOptionsOps.h>

#include "SequenceEditor.h"

#include <wabcut/model/Clip.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <array>
#include <cstdlib>
#include <utility>

namespace wabcut {

namespace {

struct ProcessingName {
    FieldProcessing value;
    std::string_view id;
    std::string_view label;
};

constexpr std::array<ProcessingName, 4> kProcessing{{
    {FieldProcessing::None, "none", "None"},
    {FieldProcessing::InterlaceConsecutiveFrames, "interlaceConsecutiveFrames", "Interlace Consecutive Frames"},
    {FieldProcessing::AlwaysDeinterlace, "alwaysDeinterlace", "Always Deinterlace"},
    {FieldProcessing::FlickerRemoval, "flickerRemoval", "Flicker Removal"},
}};

FieldRender keepFirst(FieldOrder order) {
    return order == FieldOrder::LowerFirst ? FieldRender::KeepLower : FieldRender::KeepUpper;
}

FieldRender keepSecond(FieldOrder order) {
    return order == FieldOrder::LowerFirst ? FieldRender::KeepUpper : FieldRender::KeepLower;
}

} // namespace

std::string_view toString(FieldProcessing processing) {
    for (const ProcessingName& p : kProcessing) {
        if (p.value == processing) {
            return p.id;
        }
    }
    return "none";
}

std::optional<FieldProcessing> fieldProcessingFromString(std::string_view text) {
    for (const ProcessingName& p : kProcessing) {
        if (p.id == text) {
            return p.value;
        }
    }
    return std::nullopt;
}

std::string_view displayName(FieldProcessing processing) {
    for (const ProcessingName& p : kProcessing) {
        if (p.value == processing) {
            return p.label;
        }
    }
    return "None";
}

FieldOrder effectiveFieldOrder(const MediaItem& item, int streamIndex) {
    if (item.isGenerator()) {
        return FieldOrder::Progressive;
    }
    if (item.interpretation && item.interpretation->fieldOrder) {
        return *item.interpretation->fieldOrder;
    }
    if (streamIndex < 0 || streamIndex >= static_cast<int>(item.info.video.size()) || item.info.isStillImage) {
        return FieldOrder::Progressive;
    }
    return item.info.video[static_cast<std::size_t>(streamIndex)].fieldOrder;
}

FieldRendering fieldRenderingOf(const Clip& clip, const MediaItem* item, int streamIndex, const Sequence& sequence) {
    const ClipFieldOptions& options = clip.content().fieldOptions;
    if (options.processing == FieldProcessing::FlickerRemoval) {
        return {FieldRender::Blend, FieldRender::Blend};
    }
    FieldOrder order = item ? effectiveFieldOrder(*item, streamIndex) : FieldOrder::Progressive;
    if (options.reverseFieldDominance) {
        order = order == FieldOrder::UpperFirst   ? FieldOrder::LowerFirst
                : order == FieldOrder::LowerFirst ? FieldOrder::UpperFirst
                                                  : order;
    }
    const bool always = options.processing == FieldProcessing::AlwaysDeinterlace;
    if (order == FieldOrder::Progressive) {
        // Premiere: Always Deinterlace on footage without fields keeps the upper field, or the lower one with Reverse
        // Field Dominance. Without it a progressive picture is shown as it is.
        if (!always) {
            return {};
        }
        const FieldRender kept = options.reverseFieldDominance ? FieldRender::KeepLower : FieldRender::KeepUpper;
        return {kept, kept};
    }
    if (!always && sequence.fields != FieldOrder::Progressive) {
        return {}; // an interlaced sequence keeps the fields (its frames carry both)
    }
    const FieldRender first = keepFirst(order);
    if (always || clip.isFreeze() || !sequence.frameRate.isValid()) {
        return {first, first};
    }
    // Both fields are seen when the clip's source advances by at most half a frame per sequence frame (29.97i in a
    // 59.94p sequence, or slowed down): the first half of each frame shows its first field, the second half the other.
    // The media's frame period decides; a stream without a rate keeps one field.
    const VideoStreamInfo* stream =
        item && streamIndex >= 0 && streamIndex < static_cast<int>(item->info.video.size())
            ? &item->info.video[static_cast<std::size_t>(streamIndex)]
            : nullptr;
    Rate mediaRate = stream ? stream->frameRate : Rate{};
    if (item && item->interpretation && item->interpretation->frameRate) {
        mediaRate = *item->interpretation->frameRate;
    }
    if (!mediaRate.isValid()) {
        return {first, first};
    }
    const Ticks sequenceFrame = framesToTicks(1, sequence.frameRate);
    const Ticks mediaFrame = framesToTicks(1, mediaRate);
    const Ticks step = clip.isRemapped() ? sequenceFrame : toSourceTicks(sequenceFrame, clip.speed);
    if (std::llabs(step.value) * 2 > mediaFrame.value + 2) {
        return {first, first};
    }
    return {first, keepSecond(order)};
}

FieldRender fieldRenderAt(const FieldRendering& rendering, Ticks sourceTime, Ticks framePts, Ticks frameDuration) {
    if (rendering.first == rendering.second || frameDuration.value <= 0) {
        return rendering.first;
    }
    const std::int64_t offset = sourceTime.value - framePts.value;
    if (offset < 0 || offset >= frameDuration.value) {
        return rendering.first;
    }
    return offset * 2 < frameDuration.value ? rendering.first : rendering.second;
}

} // namespace wabcut

// ---- Editing -------------------------------------------------------------------------------------------------

namespace wabcut::edit {

EditOutcome setFieldOptions(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                            const ClipFieldOptions& options) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    auto refs = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    std::vector<detail::ClipRef> video;
    for (const detail::ClipRef& ref : *refs) {
        const Track* t = (*seq)->findTrack(ref.track);
        if (t && t->kind == TrackKind::Video) {
            video.push_back(ref);
        }
    }
    if (video.empty()) {
        return std::unexpected(detail::invalidArgument("select a video clip: Field Options apply to video clips"));
    }
    detail::SequenceEditor ed(state, **seq);
    for (const detail::ClipRef& ref : video) {
        const Clip& current = ed.track(ref.track).clips[ref.index];
        if (current.content().fieldOptions == options) {
            continue;
        }
        Clip& c = ed.mutableClip(ref.track, ref.index);
        ClipContent content = c.content();
        content.fieldOptions = options;
        c.setContent(std::move(content));
    }
    return ed.finish();
}

} // namespace wabcut::edit
