#include <wabcut/model/AudioChannelOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <memory>

namespace wabcut::edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

const AudioChannelMap* effectiveChannelMap(const Clip& clip, const MediaItem* media) {
    if (const AudioChannelMap* own = clip.channelMap()) {
        return own;
    }
    return media != nullptr && media->channels ? &*media->channels : nullptr;
}

EditOutcome setClipChannels(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                            std::optional<AudioChannelMap> map) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (map) {
        if (const auto problem = channelMapProblem(*map)) {
            return std::unexpected(invalidArgument(*problem));
        }
    }
    const auto selection = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    SequenceEditor ed(state, **seq);
    for (const ClipRef& ref : *selection) {
        const Clip& current = ed.track(ref.track).clips[ref.index];
        if (current.stream.kind != StreamKind::Audio || current.isNested() || current.hasUnknownSource()) {
            return std::unexpected(invalidArgument("clip " + std::to_string(ref.clip.value) +
                                                   " is not an audio clip of a media item: only those have channels"));
        }
        if (current.content().channels == map) {
            continue;
        }
        Clip& c = ed.mutableClip(ref.track, ref.index);
        ClipContent content = c.content();
        content.channels = map;
        c.setContent(std::move(content));
    }
    return ed.finish();
}

std::expected<ProjectState, Error> setMediaChannels(const ProjectState& state, MediaId media, std::optional<AudioChannelMap> map) {
    if (map) {
        if (const auto problem = channelMapProblem(*map)) {
            return std::unexpected(invalidArgument(*problem));
        }
    }
    ProjectState next = state;
    const auto it = std::lower_bound(next.media.begin(), next.media.end(), media,
                                     [](const MediaPtr& m, MediaId value) { return m->id < value; });
    if (it == next.media.end() || (*it)->id != media) {
        return std::unexpected(notFound("media " + std::to_string(media.value) + " does not exist"));
    }
    if ((*it)->info.audio.empty()) {
        return std::unexpected(invalidArgument("media " + std::to_string(media.value) + " has no audio"));
    }
    if ((*it)->channels == map) {
        return next;
    }
    MediaItem copy = **it;
    copy.channels = std::move(map);
    *it = std::make_shared<const MediaItem>(std::move(copy));
    return next;
}

EditOutcome setMasterFormat(const ProjectState& state, SequenceId sequence, ChannelFormat format) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (format != ChannelFormat::Stereo && format != ChannelFormat::Surround51) {
        return std::unexpected(invalidArgument("the master is stereo or 5.1, not " + std::string(toString(format))));
    }
    SequenceEditor ed(state, **seq);
    if ((*seq)->masterFormat != format) {
        ed.header().masterFormat = format;
    }
    return ed.finish();
}

EditOutcome splitClipToMono(const ProjectState& state, SequenceId sequence, ClipId clipId, std::span<const int> sourceChannels,
                            std::span<const TrackId> targets) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (sourceChannels.empty() || sourceChannels.size() != targets.size() + 1) {
        return std::unexpected(invalidArgument("split to mono needs one source channel per clip: the clip's and one per target track"));
    }
    for (const int s : sourceChannels) {
        if (s < 0 || s >= kMaxSourceChannels) {
            return std::unexpected(invalidArgument("source channel " + std::to_string(s + 1) + " is out of range"));
        }
    }
    const ClipId only[] = {clipId};
    const auto selection = detail::resolveClips(**seq, only, LinkMode::IgnoreLinks);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    const ClipRef home = selection->front();
    SequenceEditor ed(state, **seq);
    const Clip original = ed.track(home.track).clips[home.index];
    if (original.stream.kind != StreamKind::Audio || original.isNested() || original.hasUnknownSource()) {
        return std::unexpected(invalidArgument("clip " + std::to_string(clipId.value) + " is not an audio clip of a media item"));
    }
    for (std::size_t k = 0; k < targets.size(); ++k) {
        const Track* t = (*seq)->findTrack(targets[k]);
        if (t == nullptr || t->kind != TrackKind::Audio) {
            return std::unexpected(invalidArgument("split to mono puts its clips on audio tracks"));
        }
        if (t->locked) {
            return std::unexpected(invalidArgument("track " + detail::trackLabel(**seq, t->id) + " is locked"));
        }
        if (t->id == home.track || std::find(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(k), t->id) !=
                                       targets.begin() + static_cast<std::ptrdiff_t>(k)) {
            return std::unexpected(invalidArgument("each split channel needs a track of its own"));
        }
        for (const Clip& other : t->clips) {
            if (other.start() < original.end() && original.start() < other.end()) {
                return std::unexpected(invalidArgument("track " + detail::trackLabel(**seq, t->id) +
                                                       " is not free where the clip is"));
            }
        }
    }
    const LinkGroupId group = original.linkGroup.isValid() ? original.linkGroup : ed.newLinkGroup();
    {
        Clip& c = ed.mutableClip(home.track, home.index);
        ClipContent content = c.content();
        content.channels = AudioChannelMap::mono(sourceChannels[0]);
        c.setContent(std::move(content));
        c.linkGroup = group;
    }
    for (std::size_t k = 0; k < targets.size(); ++k) {
        Clip copy = original;
        copy.id = ed.newClipId();
        copy.linkGroup = group;
        ClipContent content = original.content();
        content.channels = AudioChannelMap::mono(sourceChannels[k + 1]);
        copy.setContent(std::move(content));
        ed.placeNewClip(targets[k], std::move(copy), false);
    }
    ed.touchGroup(group);
    return ed.finish();
}

} // namespace wabcut::edit
