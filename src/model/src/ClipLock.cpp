// Lock Clip (ClipLock.h).

#include <wabcut/model/ClipLock.h>

#include "SequenceEditor.h"

#include <string_view>
#include <unordered_set>

namespace wabcut::edit {

using detail::SequenceEditor;

std::string lockedClipMessage(const Clip& clip) {
    if (clip.name().empty()) {
        return "clip " + std::to_string(clip.id.value) + " is locked";
    }
    return "clip '" + clip.name() + "' is locked";
}

bool isLockedClipError(const Error& error) {
    constexpr std::string_view tail = " is locked";
    const std::string_view text = error.message;
    return text.starts_with("clip ") && text.ends_with(tail);
}

EditOutcome setClipsLocked(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips, bool locked) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    const std::unordered_set<ClipId> wanted(clips.begin(), clips.end());
    SequenceEditor ed(state, s);
    std::unordered_set<ClipId> seen;
    for (const TrackId trackId : ed.trackIds()) {
        const Track& track = *s.findTrack(trackId); // the input track: indices stay those of the working copy
        for (std::size_t i = 0; i < track.clips.size(); ++i) {
            const Clip& clip = track.clips[i];
            if (!wanted.contains(clip.id)) {
                continue;
            }
            seen.insert(clip.id);
            if (clip.locked != locked) {
                ed.mutableClip(trackId, i).locked = locked;
                if (!locked) {
                    ed.touchGroup(clip.linkGroup); // a clip left alone in its link group leaves it, as unlocking a track does
                }
            }
        }
    }
    if (seen.size() != wanted.size()) {
        for (const ClipId id : clips) {
            if (!seen.contains(id)) {
                return std::unexpected(detail::notFound("clip " + std::to_string(id.value) + " is not in this sequence"));
            }
        }
    }
    return ed.finish();
}

std::optional<Error> lockedClipProblem(const ProjectState& before, const ProjectState& after) {
    for (const SequencePtr& was : before.sequences) {
        if (!was) {
            continue;
        }
        const Sequence* now = after.findSequence(was->id);
        if (!now || now == was.get()) {
            continue; // gone (deleted as a whole) or shared: nothing in it changed
        }
        std::optional<Error> problem;
        was->forEachTrack([&](const Track& track) {
            if (problem) {
                return;
            }
            const Track* nowTrack = now->findTrack(track.id);
            if (nowTrack == &track) {
                return;
            }
            for (const Clip& clip : track.clips) {
                if (!clip.locked) {
                    continue;
                }
                // It must still be there, on the same track, at the same time, as it was; only its flag may differ.
                bool intact = false;
                if (nowTrack) {
                    if (const auto index = nowTrack->clipIndexAt(clip.start())) {
                        Clip same = nowTrack->clips[*index];
                        if (same.id == clip.id) {
                            // Unlocking may dissolve the link group the clip was left alone in (setClipsLocked).
                            const bool unlocked = !same.locked;
                            same.locked = clip.locked;
                            same.media = clip.media; // Consolidate Duplicates and relinking re-point the media, not the edit
                            if (unlocked) {
                                same.linkGroup = clip.linkGroup;
                            }
                            intact = same == clip;
                        }
                    }
                }
                if (!intact) {
                    problem = Error{ErrorCode::InvalidArgument, lockedClipMessage(clip)};
                    return;
                }
            }
        });
        if (problem) {
            return problem;
        }
    }
    return std::nullopt;
}

} // namespace wabcut::edit
