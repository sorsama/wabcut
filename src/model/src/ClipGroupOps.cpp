// Clip > Group / Ungroup (ClipGroups.h).

#include <wabcut/model/ClipGroups.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <unordered_set>

namespace wabcut::edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::SequenceEditor;

EditOutcome groupClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto selection = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    if (selection->size() < 2) {
        return std::unexpected(invalidArgument("select at least two clips to group"));
    }
    SequenceEditor ed(state, **seq);
    const ClipGroupId group = ed.newClipGroup();
    for (const ClipRef& ref : *selection) {
        ed.mutableClip(ref.track, ref.index).group = group;
    }
    return ed.finish();
}

EditOutcome ungroupClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    std::unordered_set<ClipGroupId> groups;
    for (const ClipId id : clips) {
        const auto loc = s.findClip(id);
        if (!loc) {
            return std::unexpected(detail::notFound("clip " + std::to_string(id.value) + " is not in the sequence"));
        }
        if (loc->clip().group.isValid()) {
            groups.insert(loc->clip().group);
        }
    }
    if (groups.empty()) {
        return detail::unchanged(state);
    }
    // Every member leaves (also on a locked track: a group is selection metadata, not content).
    SequenceEditor ed(state, s);
    for (const TrackId trackId : ed.trackIds()) {
        const Track& track = *s.findTrack(trackId); // the input track: indices stay those of the working copy
        for (std::size_t i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].group.isValid() && groups.contains(track.clips[i].group)) {
                ed.mutableClip(trackId, i).group = {};
            }
        }
    }
    return ed.finish();
}

std::vector<ClipId> withGroupedClips(const Sequence& sequence, std::span<const ClipId> clips) {
    std::vector<ClipId> out(clips.begin(), clips.end());
    std::unordered_set<ClipId> seen(clips.begin(), clips.end());
    std::unordered_set<ClipGroupId> groups;
    sequence.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.group.isValid() && seen.contains(c.id)) {
                groups.insert(c.group);
            }
        }
    });
    if (!groups.empty()) {
        sequence.forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                if (c.group.isValid() && groups.contains(c.group) && seen.insert(c.id).second) {
                    out.push_back(c.id);
                }
            }
        });
    }
    return withLinkedClips(sequence, out);
}

bool anyGrouped(const Sequence& sequence, std::span<const ClipId> clips) {
    return std::ranges::any_of(clips, [&](ClipId id) {
        const auto loc = sequence.findClip(id);
        return loc && loc->clip().group.isValid();
    });
}

} // namespace wabcut::edit
