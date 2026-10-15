#include <wabcut/editor/SourceEdit.h>

#include <wabcut/model/Nesting.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace wabcut::editor {

namespace {

/// Tracks of one kind to fill, starting at `start` (its position in `tracks`), skipping locked ones.
std::vector<TrackId> destinations(const std::vector<TrackPtr>& tracks, std::size_t start, std::size_t count) {
    std::vector<TrackId> out;
    for (std::size_t i = start; i < tracks.size() && out.size() < count; ++i) {
        if (!tracks[i]->locked) {
            out.push_back(tracks[i]->id);
        }
    }
    return out;
}

/// Index of the first track to fill: the one under the pointer, else the first targeted one, else 0.
std::size_t firstTrack(const std::vector<TrackPtr>& tracks, TrackId pointer) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (pointer && tracks[i]->id == pointer) {
            return i;
        }
    }
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i]->targeted) {
            return i;
        }
    }
    return 0;
}

Error invalid(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message)};
}

/// The track under the pointer: its kind and its place among the tracks of that kind.
struct PointerTrack {
    StreamKind kind = StreamKind::Video;
    std::size_t index = 0;
};
std::optional<PointerTrack> pointerTrack(const Sequence& sequence, TrackId track) {
    if (!track) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < sequence.videoTracks.size(); ++i) {
        if (sequence.videoTracks[i]->id == track) {
            return PointerTrack{StreamKind::Video, i};
        }
    }
    for (std::size_t i = 0; i < sequence.audioTracks.size(); ++i) {
        if (sequence.audioTracks[i]->id == track) {
            return PointerTrack{StreamKind::Audio, i};
        }
    }
    return std::nullopt;
}

/// Calls `fn` for every clip of `track` that lies (partly) in `range`.
template <typename Fn>
void forClipsIn(const Track& track, TimeRange range, Fn&& fn) {
    const std::size_t first = track.firstClipAtOrAfter(range.start);
    for (std::size_t i = first > 0 ? first - 1 : 0; i < track.clips.size(); ++i) {
        const Clip& clip = track.clips[i];
        if (clip.start() >= range.end()) {
            break;
        }
        if (clip.timelineRange.overlaps(range)) {
            fn(clip);
        }
    }
}

/// Whether a stream of the drop may go to `track` without cutting anything the drop did not land on: it has nothing
/// in `landing`, or only clips linked to clips there on the track under the pointer (the drop replaces those).
bool freeFor(const Track& track, std::optional<TimeRange> landing, const Track* pointer) {
    if (!landing) {
        return true;
    }
    std::vector<LinkGroupId> replaced;
    if (pointer) {
        forClipsIn(*pointer, *landing, [&](const Clip& clip) {
            if (clip.linkGroup) {
                replaced.push_back(clip.linkGroup);
            }
        });
    }
    bool free = true;
    forClipsIn(track, *landing, [&](const Clip& clip) {
        if (!clip.linkGroup || std::ranges::find(replaced, clip.linkGroup) == replaced.end()) {
            free = false;
        }
    });
    return free;
}

/// `count` tracks of one kind for streams that follow the pointer's track: the track at `from` and the ones after it,
/// then the ones before it, each unlocked, not in `taken` and free in `landing`; when too few are free, unlocked tracks
/// in the same order make up the rest (Premiere's patching then overwrites them).
std::vector<TrackId> followingTracks(const std::vector<TrackPtr>& tracks, std::size_t from, std::size_t count,
                                     std::optional<TimeRange> landing, const Track* pointer,
                                     const std::vector<TrackId>& taken = {}) {
    std::vector<TrackId> out;
    const auto usable = [&](const Track& t) {
        return !t.locked && std::ranges::find(taken, t.id) == taken.end() && std::ranges::find(out, t.id) == out.end();
    };
    std::vector<std::size_t> order;
    const std::size_t start = std::min(from, tracks.size());
    for (std::size_t i = start; i < tracks.size(); ++i) {
        order.push_back(i);
    }
    for (std::size_t i = start; i-- > 0;) {
        order.push_back(i);
    }
    for (const std::size_t i : order) {
        if (out.size() < count && usable(*tracks[i]) && freeFor(*tracks[i], landing, pointer)) {
            out.push_back(tracks[i]->id);
        }
    }
    for (const std::size_t i : order) {
        if (out.size() < count && usable(*tracks[i])) {
            out.push_back(tracks[i]->id);
        }
    }
    return out;
}

/// What a drop brings: a picture or not, how many audio streams, and (A.10) the recording whose audio goes by name.
struct DropStreams {
    bool video = false;
    std::size_t audio = 0;
    const MediaInfo* byName = nullptr;
};

std::vector<edit::SourcePatch> patchesFor(const Sequence& sequence, const DropStreams& streams, TrackId track,
                                          std::optional<TimeRange> landing) {
    std::vector<edit::SourcePatch> patches;
    const std::optional<PointerTrack> pointer = pointerTrack(sequence, track);
    if (!pointer) {
        // Outside the tracks: the targeted tracks (else V1 and A1, A2, ...), Premiere's default patching.
        if (streams.video) {
            const auto video = destinations(sequence.videoTracks, firstTrack(sequence.videoTracks, track), 1);
            if (!video.empty()) {
                patches.push_back({{StreamKind::Video, 0}, video.front()});
            }
        }
        const std::size_t firstAudio = firstTrack(sequence.audioTracks, track);
        if (streams.byName) {
            for (edit::SourcePatch& p : edit::audioPatchesByName(sequence, *streams.byName, firstAudio)) {
                patches.push_back(std::move(p));
            }
            return patches;
        }
        const auto audio = destinations(sequence.audioTracks, firstAudio, streams.audio);
        for (std::size_t i = 0; i < audio.size(); ++i) {
            patches.push_back({{StreamKind::Audio, static_cast<std::int32_t>(i)}, audio[i]});
        }
        return patches;
    }

    const bool onVideo = pointer->kind == StreamKind::Video;
    const Track& under = onVideo ? *sequence.videoTracks[pointer->index] : *sequence.audioTracks[pointer->index];
    if (streams.video) {
        // The track under the pointer (the next unlocked one up when it is locked), else the matching video track.
        const auto video = onVideo ? destinations(sequence.videoTracks, pointer->index, 1)
                                   : followingTracks(sequence.videoTracks, pointer->index, 1, landing, &under);
        if (!video.empty()) {
            patches.push_back({{StreamKind::Video, 0}, video.front()});
        }
    }
    if (streams.byName) {
        // A.10: by name; the untitled streams from the track under the pointer, or the matching audio track.
        const std::size_t first = std::min(pointer->index, sequence.audioTracks.size());
        for (edit::SourcePatch& p : edit::audioPatchesByName(sequence, *streams.byName, first)) {
            patches.push_back(std::move(p));
        }
        return patches;
    }
    const std::size_t count = streams.audio;
    std::vector<TrackId> audio;
    if (count > 0 && !onVideo) {
        // The first stream on the track under the pointer, the others where they cut nothing.
        audio = destinations(sequence.audioTracks, pointer->index, 1);
        const auto rest = followingTracks(sequence.audioTracks, pointer->index + 1, count - audio.size(), landing,
                                          &under, audio);
        audio.insert(audio.end(), rest.begin(), rest.end());
    } else if (count > 0) {
        audio = followingTracks(sequence.audioTracks, pointer->index, count, landing, &under);
    }
    for (std::size_t i = 0; i < audio.size(); ++i) {
        patches.push_back({{StreamKind::Audio, static_cast<std::int32_t>(i)}, audio[i]});
    }
    return patches;
}

/// C.8: a sequence dropped as a source: a nested clip of it, or a multicam clip of a multicam source.
edit::EditOutcome dropSequence(const ProjectState& state, const Sequence& sequence, const MediaDrop& drop) {
    const Sequence* source = state.findSequence(drop.sourceSequence);
    if (!source) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    if (source->id == sequence.id) {
        return std::unexpected(invalid("a sequence cannot be nested in itself"));
    }
    if (sequenceContains(state, source->id, sequence.id)) {
        return std::unexpected(invalid("\"" + source->name + "\" already plays this sequence, so it cannot go in it"));
    }
    edit::SourceClip clip;
    clip.sequence = source->id;
    clip.patches = patchesForDrop(sequence, *source, drop.track);
    if (clip.patches.empty()) {
        return std::unexpected(invalid("the sequence has no picture or sound that fits an unlocked track"));
    }
    clip.sourceRange = drop.sourceRange.value_or(TimeRange{Ticks{}, source->duration});
    if (clip.sourceRange.isEmpty()) {
        return std::unexpected(invalid("the sequence is empty"));
    }
    const Ticks at = snapToFrame(Ticks{std::max<std::int64_t>(0, drop.at.value)}, sequence.frameRate);
    // As for media: once it is known where the clips land, the streams that follow the pointer's track avoid tracks
    // with other clips there.
    clip.patches = patchesForDrop(sequence, *source, drop.track, TimeRange{at, clip.sourceRange.duration});
    return drop.mode == DropMode::Overwrite ? edit::overwriteEdit(state, sequence.id, clip, at)
                                            : edit::insertEdit(state, sequence.id, clip, at);
}

} // namespace

std::vector<edit::SourcePatch> patchesForDrop(const Sequence& sequence, const MediaItem& media, TrackId track,
                                              std::optional<TimeRange> landing) {
    const DropStreams streams{.video = media.info.hasVideo(),
                              .audio = media.info.audio.size(),
                              .byName = edit::routesAudioByName(media.info) ? &media.info : nullptr};
    return patchesFor(sequence, streams, track, landing);
}

std::vector<edit::SourcePatch> patchesForDrop(const Sequence& sequence, const Sequence& source, TrackId track,
                                              std::optional<TimeRange> landing) {
    const DropStreams streams{.video = sequenceHasStream(source, {StreamKind::Video, 0}),
                              .audio = sequenceHasStream(source, {StreamKind::Audio, 0}) ? std::size_t{1} : 0};
    return patchesFor(sequence, streams, track, landing);
}

edit::EditOutcome dropMedia(EditSession& session, const MediaDrop& drop) {
    const char* label = drop.mode == DropMode::Overwrite ? "Overwrite" : "Insert";
    return session.apply(label, [&](const ProjectState& state) -> edit::EditOutcome {
        const Sequence* sequence = state.findSequence(drop.sequence);
        if (sequence && drop.sourceSequence) {
            return dropSequence(state, *sequence, drop);
        }
        const MediaItem* media = state.findMedia(drop.media);
        if (!sequence || !media) {
            return std::unexpected(Error{ErrorCode::NotFound, "no such sequence or media item"});
        }
        edit::SourceClip source;
        source.media = media->id;
        source.patches = patchesForDrop(*sequence, *media, drop.track);
        if (source.patches.empty()) {
            return std::unexpected(invalid("the media has no stream that fits an unlocked track"));
        }
        if (drop.sourceRange) {
            source.sourceRange = *drop.sourceRange;
        } else {
            std::optional<Ticks> extent;
            for (const edit::SourcePatch& p : source.patches) {
                if (const auto e = media->sourceExtent(p.source)) {
                    extent = extent ? std::min(*extent, *e) : *e;
                }
            }
            source.sourceRange = {Ticks{}, extent.value_or(edit::kDefaultStillDuration)};
        }
        if (source.sourceRange.isEmpty()) {
            return std::unexpected(invalid("the media is empty"));
        }
        const Ticks at = snapToFrame(Ticks{std::max<std::int64_t>(0, drop.at.value)}, sequence->frameRate);
        // Now that it is known where the clips land, the streams that follow the pointer's track avoid tracks with
        // other clips there.
        source.patches = patchesForDrop(*sequence, *media, drop.track, TimeRange{at, source.sourceRange.duration});
        return drop.mode == DropMode::Overwrite ? edit::overwriteEdit(state, sequence->id, source, at)
                                                : edit::insertEdit(state, sequence->id, source, at);
    });
}

} // namespace wabcut::editor
