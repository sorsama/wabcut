#include "SequenceEditor.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Nesting.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <tuple>
#include <unordered_set>

namespace wabcut::edit::detail {

Error invalidArgument(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }
Error notFound(std::string message) { return {ErrorCode::NotFound, std::move(message)}; }

std::expected<const Sequence*, Error> findSequence(const ProjectState& state, SequenceId sequence) {
    if (const Sequence* seq = state.findSequence(sequence)) {
        return seq;
    }
    return std::unexpected(notFound("sequence " + std::to_string(sequence.value) + " does not exist"));
}

std::string trackLabel(const Sequence& sequence, TrackId track) {
    if (const auto pos = sequence.trackPosition(track)) {
        return (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1);
    }
    if (const auto index = sequence.submixIndex(track)) {
        return "Submix " + std::to_string(*index + 1);
    }
    return "track " + std::to_string(track.value);
}

std::expected<std::vector<ClipRef>, Error> resolveClips(const Sequence& sequence, std::span<const ClipId> clips,
                                                        LinkMode links) {
    const std::unordered_set<ClipId> wanted(clips.begin(), clips.end());
    struct Found {
        std::size_t trackOrdinal;
        ClipRef ref;
    };
    std::vector<Found> found;
    std::unordered_set<LinkGroupId> groups;
    std::unordered_set<ClipId> seen;

    std::size_t ordinal = 0;
    std::optional<Error> error;
    sequence.forEachTrack([&](const Track& track) {
        std::size_t i = 0;
        for (const Clip& clip : track.clips) {
            if (wanted.contains(clip.id)) {
                if (track.locked && !error) {
                    error = invalidArgument("clip " + std::to_string(clip.id.value) + " is on locked track " +
                                            trackLabel(sequence, track.id));
                }
                found.push_back({ordinal, {track.id, clip.id, i}});
                seen.insert(clip.id);
                if (clip.linkGroup.isValid()) {
                    groups.insert(clip.linkGroup);
                }
            }
            ++i;
        }
        ++ordinal;
    });
    if (error) {
        return std::unexpected(std::move(*error));
    }
    if (seen.size() != wanted.size()) {
        for (const ClipId id : clips) {
            if (!seen.contains(id)) {
                return std::unexpected(notFound("clip " + std::to_string(id.value) + " is not in this sequence"));
            }
        }
    }

    if (links == LinkMode::FollowLinks && !groups.empty()) {
        ordinal = 0;
        sequence.forEachTrack([&](const Track& track) {
            if (!track.locked) {
                std::size_t i = 0;
                for (const Clip& clip : track.clips) {
                    if (clip.linkGroup.isValid() && groups.contains(clip.linkGroup) && !seen.contains(clip.id)) {
                        found.push_back({ordinal, {track.id, clip.id, i}});
                        seen.insert(clip.id);
                    }
                    ++i;
                }
            }
            ++ordinal;
        });
    }

    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return std::tie(a.trackOrdinal, a.ref.index) < std::tie(b.trackOrdinal, b.ref.index);
    });
    std::vector<ClipRef> out;
    out.reserve(found.size());
    for (const auto& f : found) {
        out.push_back(f.ref);
    }
    return out;
}

std::expected<std::vector<ClipRef>, Error> edgeParticipants(const Sequence& sequence, ClipId clipId, Edge edge,
                                                            LinkMode links) {
    const auto location = sequence.findClip(clipId);
    if (!location) {
        return std::unexpected(notFound("clip " + std::to_string(clipId.value) + " is not in this sequence"));
    }
    const Track& home = *location->track;
    if (home.locked) {
        return std::unexpected(invalidArgument("clip " + std::to_string(clipId.value) + " is on locked track " +
                                               trackLabel(sequence, home.id)));
    }
    const Clip& clip = location->clip();
    std::vector<ClipRef> out{{home.id, clip.id, location->index}};
    if (links == LinkMode::IgnoreLinks || !clip.linkGroup.isValid()) {
        return out;
    }
    const Ticks edgeTime = edge == Edge::Head ? clip.start() : clip.end();
    sequence.forEachTrack([&](const Track& track) {
        if (track.locked || track.id == home.id) {
            return;
        }
        std::optional<std::size_t> best;
        ClipId bestId;
        std::int64_t bestDistance = 0;
        std::size_t i = 0;
        for (const Clip& c : track.clips) {
            if (c.linkGroup == clip.linkGroup) {
                const Ticks t = edge == Edge::Head ? c.start() : c.end();
                const std::int64_t distance = t > edgeTime ? (t - edgeTime).value : (edgeTime - t).value;
                if (!best || distance < bestDistance) {
                    best = i;
                    bestId = c.id;
                    bestDistance = distance;
                }
            }
            ++i;
        }
        if (best) {
            out.push_back({track.id, bestId, *best});
        }
    });
    return out;
}

EditResult unchanged(const ProjectState& state) {
    EditResult result;
    result.state = state;
    return result;
}

std::vector<TrackId> unlockedTargets(const Sequence& sequence) {
    std::vector<TrackId> out;
    sequence.forEachTrack([&](const Track& t) {
        if (t.targeted && !t.locked) {
            out.push_back(t.id);
        }
    });
    return out;
}

// --- SequenceEditor -----------------------------------------------------------------------------------

SequenceEditor::SequenceEditor(const ProjectState& state, const Sequence& sequence)
    : m_state(state), m_original(sequence), m_seq(sequence), m_ids(state.ids) {}

std::vector<TrackId> SequenceEditor::trackIds() const {
    std::vector<TrackId> out;
    out.reserve(m_seq.trackCount());
    m_seq.forEachTrack([&](const Track& t) { out.push_back(t.id); });
    return out;
}

TrackPtr& SequenceEditor::slot(TrackId id) {
    for (auto* list : {&m_seq.videoTracks, &m_seq.audioTracks, &m_seq.submixTracks}) {
        for (auto& t : *list) {
            if (t->id == id) {
                return t;
            }
        }
    }
    assert(false && "unknown track");
    return m_seq.videoTracks.front();
}

const TrackPtr& SequenceEditor::slot(TrackId id) const {
    return const_cast<SequenceEditor*>(this)->slot(id);
}

const Track& SequenceEditor::track(TrackId id) const { return *slot(id); }

Track& SequenceEditor::mutableTrack(TrackId id) {
    for (const auto& [clonedId, clone] : m_cloned) {
        if (clonedId == id) {
            return *clone;
        }
    }
    // The copy shares every clip chunk with the original; ClipList copies a chunk only when it changes.
    TrackPtr& s = slot(id);
    auto copy = std::make_shared<Track>(*s);
    Track* raw = copy.get();
    s = std::move(copy);
    m_cloned.emplace_back(id, raw);
    return *raw;
}

SourceBounds SequenceEditor::sourceBounds(const Clip& clip) const {
    // Media: the stream's media, or a restricted subclip's range (M5k). Nested sequences: their duration (C.8).
    if (!clip.isNested() && !clip.hasUnknownSource()) {
        if (const MediaItem* media = m_state.findMedia(clip.media)) {
            return media->sourceBounds(clip.stream);
        }
    }
    return SourceBounds{Ticks{}, clipSourceExtent(m_state, clip)};
}

std::optional<Ticks> SequenceEditor::maxDuration(const Clip& clip) const {
    const SourceBounds bounds = sourceBounds(clip);
    if (const auto room = clip.tailRoom(bounds.start, bounds.end)) {
        return clip.duration() + *room;
    }
    return std::nullopt;
}

std::optional<Ticks> SequenceEditor::headRoom(const Clip& clip) const {
    const SourceBounds bounds = sourceBounds(clip);
    return clip.headRoom(bounds.start, bounds.end);
}

std::optional<std::size_t> SequenceEditor::splitAt(TrackId trackId, Ticks at) {
    const auto index = track(trackId).clipIndexAt(at);
    if (!index || track(trackId).clips[*index].start() == at) {
        return std::nullopt;
    }
    Track& t = mutableTrack(trackId);
    Clip& left = t.clips.mutableAt(*index);
    // Both pieces share the content block: name, parameters, keyframe curves (media time, so they evaluate
    // seamlessly across the cut) and effects.
    Clip right = left.splitOff(at);
    right.id = newClipId();
    m_created.push_back(right.id);
    if (right.linkGroup.isValid()) {
        m_splits.push_back({right.linkGroup, at, right.id});
        touchGroup(right.linkGroup);
    }
    // A transition at the clip's Out point now belongs to the right-hand piece.
    for (Transition& tr : t.transitions) {
        if (tr.outgoing == left.id) {
            tr.outgoing = right.id;
        }
    }
    t.clips.insert(*index + 1, std::move(right));
    return *index + 1;
}

void SequenceEditor::clearRange(TrackId trackId, TimeRange range) {
    if (range.isEmpty()) {
        return;
    }
    const Ticks a = range.start;
    const Ticks b = range.end();
    // A clip spanning the whole range is split at its end first: the left part then gets its tail
    // trimmed (keeping the clip's id) and the right part is a new clip.
    if (const auto i = track(trackId).clipIndexAt(a)) {
        const Clip& c = track(trackId).clips[*i];
        if (c.start() < a && c.end() > b) {
            splitAt(trackId, b);
        }
    }
    const std::size_t first = track(trackId).firstClipAtOrAfter(a);
    if (first > 0 && track(trackId).clips[first - 1].end() > a) {
        Clip& c = mutableClip(trackId, first - 1); // crosses the range start: trim its tail
        c.moveTail((a - c.start()) - c.duration());
    }
    std::size_t last = track(trackId).firstClipAtOrAfter(b);
    if (last > first && track(trackId).clips[last - 1].end() > b) {
        Clip& c = mutableClip(trackId, last - 1); // crosses the range end: trim its head, keep its id
        c.moveHead(b - c.start());
        --last;
    }
    if (first == last) {
        return;
    }
    Track& mt = mutableTrack(trackId);
    auto it = mt.clips.iteratorAt(first);
    for (std::size_t i = first; i < last; ++i, ++it) {
        m_removed.push_back(it->id);
        touchGroup(it->linkGroup);
    }
    mt.clips.erase(first, last);
}

void SequenceEditor::rippleRemove(TrackId trackId, TimeRange range) {
    if (range.isEmpty()) {
        return;
    }
    clearRange(trackId, range);
    shiftFrom(trackId, range.end(), -range.duration);
}

void SequenceEditor::rippleInsert(TrackId trackId, Ticks at, Ticks duration) {
    if (duration.value <= 0) {
        return;
    }
    splitAt(trackId, at);
    shiftFrom(trackId, at, duration);
}

void SequenceEditor::shiftFrom(TrackId trackId, Ticks from, Ticks delta) {
    if (delta.value == 0) {
        return;
    }
    const std::size_t first = track(trackId).firstClipAtOrAfter(from);
    const std::size_t size = track(trackId).clips.size();
    if (first == size) {
        return;
    }
    mutableTrack(trackId).clips.mutateRange(first, size, [&](Clip& c) { c.timelineRange.start += delta; });
}

template <class Map>
void SequenceEditor::mapCaptions(Map&& map) {
    if (m_seq.captionTracks.empty()) {
        return;
    }
    std::vector<CaptionTrack> tracks = m_seq.captionTracks;
    for (CaptionTrack& t : tracks) {
        std::vector<CaptionCue> cues;
        cues.reserve(t.cues.size() + 1);
        for (const CaptionCue& cue : t.cues) {
            for (auto [start, end] : map(cue.start, cue.end())) {
                if (end > start) {
                    cues.push_back(CaptionCue{start, end - start, cue.text, cue.speaker});
                }
            }
        }
        std::stable_sort(cues.begin(), cues.end(), [](const CaptionCue& a, const CaptionCue& b) { return a.start < b.start; });
        if (!t.style.allowOverlap) {
            for (std::size_t i = 0; i + 1 < cues.size(); ++i) {
                if (cues[i].end() > cues[i + 1].start) {
                    cues[i].duration = cues[i + 1].start - cues[i].start;
                }
            }
            std::erase_if(cues, [](const CaptionCue& c) { return c.duration.value <= 0; });
        }
        t.cues = std::move(cues);
    }
    if (tracks != m_seq.captionTracks) {
        header().captionTracks = std::move(tracks);
    }
}

void SequenceEditor::captionsInsert(Ticks at, Ticks duration, std::optional<Ticks> until) {
    if (duration.value <= 0) {
        return;
    }
    mapCaptions([&](Ticks start, Ticks end) {
        std::vector<std::pair<Ticks, Ticks>> out;
        const auto clip = [&](std::pair<Ticks, Ticks> span) {
            if (until && span.second > *until) {
                span.second = *until;
            }
            return span;
        };
        if (until && start >= *until) {
            out.emplace_back(start, end);
        } else if (start >= at) {
            out.push_back(clip({start + duration, end + duration}));
        } else if (end > at) {
            out.emplace_back(start, at); // cut through, like a clip on a sync-locked track
            out.push_back(clip({at + duration, end + duration}));
        } else {
            out.emplace_back(start, end);
        }
        return out;
    });
}

void SequenceEditor::captionsRemove(TimeRange range, std::optional<Ticks> until) {
    if (range.duration.value <= 0) {
        return;
    }
    const auto map = [&](Ticks t) {
        if (until && t >= *until) {
            return t;
        }
        if (t <= range.start) {
            return t;
        }
        return t < range.end() ? range.start : t - range.duration;
    };
    mapCaptions([&](Ticks start, Ticks end) { return std::vector<std::pair<Ticks, Ticks>>{{map(start), map(end)}}; });
}

void SequenceEditor::captionsStretch(TimeRange range, Ticks duration, bool ripple) {
    if (range.duration.value <= 0 || duration == range.duration) {
        return;
    }
    const auto map = [&](Ticks t) {
        if (t <= range.start) {
            return t;
        }
        if (t < range.end()) {
            return range.start +
                   Ticks{mulDiv((t - range.start).value, duration.value, range.duration.value, Rounding::Nearest)
                             .value_or(0)};
        }
        return ripple ? t + (duration - range.duration) : t;
    };
    mapCaptions([&](Ticks start, Ticks end) { return std::vector<std::pair<Ticks, Ticks>>{{map(start), map(end)}}; });
}

void SequenceEditor::removeClip(TrackId trackId, ClipId clipId) {
    const auto index = track(trackId).indexOf(clipId);
    assert(index);
    Track& t = mutableTrack(trackId);
    m_removed.push_back(clipId);
    touchGroup(t.clips[*index].linkGroup);
    t.clips.erase(*index, *index + 1);
}

std::vector<Clip> SequenceEditor::extract(std::span<const ClipRef> refs, bool reportRemoved) {
    std::vector<Clip> out;
    out.reserve(refs.size());
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < refs.size();) {
        std::size_t j = i;
        indices.clear();
        while (j < refs.size() && refs[j].track == refs[i].track) {
            assert(j == i || refs[j - 1].index < refs[j].index);
            indices.push_back(refs[j].index);
            ++j;
        }
        std::vector<Clip> taken = mutableTrack(refs[i].track).clips.extract(indices);
        for (Clip& clip : taken) {
            if (reportRemoved) {
                m_removed.push_back(clip.id);
                touchGroup(clip.linkGroup);
            }
            out.push_back(std::move(clip));
        }
        i = j;
    }
    return out;
}

void SequenceEditor::eraseTrack(TrackId trackId) {
    for (const Clip& clip : track(trackId).clips) {
        m_removed.push_back(clip.id);
        touchGroup(clip.linkGroup);
    }
    std::erase_if(m_cloned, [&](const auto& entry) { return entry.first == trackId; });
    for (auto* list : {&m_seq.videoTracks, &m_seq.audioTracks, &m_seq.submixTracks}) {
        std::erase_if(*list, [&](const TrackPtr& t) { return t->id == trackId; });
    }
    m_headerChanged = true;
}

TrackId SequenceEditor::addSubmixTrack(ChannelFormat format, std::string name) {
    auto added = std::make_shared<Track>();
    added->id = m_ids.allocate<TrackId>();
    added->kind = TrackKind::Audio;
    added->name = std::move(name);
    added->format = format;
    added->syncLocked = false; // holds no clips: ripples have nothing to move on it
    const TrackId id = added->id;
    m_cloned.emplace_back(id, added.get());
    m_seq.submixTracks.push_back(std::move(added));
    m_createdTracks.push_back(id);
    m_headerChanged = true;
    return id;
}

TrackId SequenceEditor::addTrack(TrackKind kind, std::string name, TrackRole role) {
    // The same track ProjectOps' addTrack makes (Track's defaults: sync-locked, untargeted, unity fader), with a role.
    auto added = std::make_shared<Track>();
    added->id = m_ids.allocate<TrackId>();
    added->kind = kind;
    added->name = std::move(name);
    added->role = kind == TrackKind::Audio ? role : TrackRole::None;
    const TrackId id = added->id;
    // The new track is this edit's own: later changes write it directly, like a cloned track.
    m_cloned.emplace_back(id, added.get());
    (kind == TrackKind::Video ? m_seq.videoTracks : m_seq.audioTracks).push_back(std::move(added));
    m_createdTracks.push_back(id);
    m_headerChanged = true;
    return id;
}

void SequenceEditor::placeClip(TrackId trackId, Clip clip) {
    Track& t = mutableTrack(trackId);
    const std::size_t at = t.clips.lowerBoundStart(clip.start());
    assert(at == 0 || t.clips[at - 1].end() <= clip.start());
    assert(at == t.clips.size() || clip.end() <= t.clips[at].start());
    t.clips.insert(at, std::move(clip));
}

void SequenceEditor::placeNewClip(TrackId trackId, Clip clip, bool fromSource) {
    m_created.push_back(clip.id);
    if (fromSource) {
        m_placed.push_back(clip.id);
    }
    placeClip(trackId, std::move(clip));
}

void SequenceEditor::relinkSplitPieces(std::vector<ClipId>& removedSorted) {
    // Right pieces cut at the same time out of the same link group stay linked to each other.
    std::vector<SplitRecord> live;
    for (const auto& s : m_splits) {
        if (!std::binary_search(removedSorted.begin(), removedSorted.end(), s.right)) {
            live.push_back(s);
        }
    }
    std::sort(live.begin(), live.end(), [](const SplitRecord& a, const SplitRecord& b) {
        return std::tie(a.group, a.cut, a.right) < std::tie(b.group, b.cut, b.right);
    });
    std::vector<std::pair<ClipId, LinkGroupId>> assignment;
    for (std::size_t i = 0; i < live.size();) {
        std::size_t j = i;
        while (j < live.size() && live[j].group == live[i].group && live[j].cut == live[i].cut) {
            ++j;
        }
        const LinkGroupId group = j - i >= 2 ? newLinkGroup() : LinkGroupId{};
        for (std::size_t k = i; k < j; ++k) {
            assignment.emplace_back(live[k].right, group);
        }
        i = j;
    }
    if (assignment.empty()) {
        return;
    }
    std::sort(assignment.begin(), assignment.end());
    // Split pieces only live on cloned tracks. Find them read-only, then write just those clips.
    for (const auto& entry : m_cloned) {
        Track& t = *entry.second;
        std::vector<std::pair<std::size_t, LinkGroupId>> writes;
        std::size_t index = 0;
        for (const Clip& clip : t.clips) {
            const auto it = std::lower_bound(assignment.begin(), assignment.end(), clip.id,
                                             [](const auto& e, ClipId v) { return e.first < v; });
            if (it != assignment.end() && it->first == clip.id) {
                writes.emplace_back(index, it->second);
            }
            ++index;
        }
        for (const auto& [i, group] : writes) {
            t.clips.mutableAt(i).linkGroup = group;
        }
    }
}

void SequenceEditor::dissolveSingletonGroups() {
    std::sort(m_touchedGroups.begin(), m_touchedGroups.end());
    m_touchedGroups.erase(std::unique(m_touchedGroups.begin(), m_touchedGroups.end()), m_touchedGroups.end());
    if (m_touchedGroups.empty()) {
        return;
    }
    struct Seen {
        int count = 0;
        TrackId track;
        std::size_t index = 0;
    };
    std::vector<Seen> seen(m_touchedGroups.size());
    m_seq.forEachTrack([&](const Track& t) {
        std::size_t index = 0;
        for (const Clip& clip : t.clips) {
            if (clip.linkGroup.isValid()) {
                const auto it = std::lower_bound(m_touchedGroups.begin(), m_touchedGroups.end(), clip.linkGroup);
                if (it != m_touchedGroups.end() && *it == clip.linkGroup) {
                    Seen& s = seen[static_cast<std::size_t>(it - m_touchedGroups.begin())];
                    ++s.count;
                    s.track = t.id;
                    s.index = index;
                }
            }
            ++index;
        }
    });
    for (const Seen& s : seen) {
        // A clip left alone in its group on a locked track keeps the group: locked tracks never change.
        // The group dissolves when that track is unlocked (setTrackState touches its groups).
        if (s.count == 1 && !track(s.track).locked) {
            mutableClip(s.track, s.index).linkGroup = {};
        }
    }
}

void SequenceEditor::pruneTransitions() {
    for (const auto& entry : m_cloned) {
        Track& t = *entry.second;
        if (t.transitions.empty()) {
            continue;
        }
        // Drop transitions this edit invalidated (clips gone, no longer adjacent, too short, no handles),
        // then any that now overlap an earlier one.
        std::erase_if(t.transitions, [&](const Transition& tr) {
            return transitionProblem(m_state, t, tr, Validation::Strict).has_value();
        });
        std::sort(t.transitions.begin(), t.transitions.end(), [&](const Transition& x, const Transition& y) {
            return *transitionEditPoint(t, x) < *transitionEditPoint(t, y);
        });
        std::optional<Ticks> previousEnd;
        std::erase_if(t.transitions, [&](const Transition& tr) {
            const Ticks cut = *transitionEditPoint(t, tr);
            if (previousEnd && cut - tr.before < *previousEnd) {
                return true;
            }
            previousEnd = cut + tr.after;
            return false;
        });
    }
}

EditResult SequenceEditor::finish(std::optional<Ticks> playhead, Ticks appliedDelta) {
    EditResult result;
    result.playhead = playhead;
    result.appliedDelta = appliedDelta;
    if (!changed()) {
        result.state = m_state;
        return result;
    }

    std::vector<ClipId> removedSorted = m_removed;
    std::sort(removedSorted.begin(), removedSorted.end());
    if (!m_splits.empty()) {
        relinkSplitPieces(removedSorted);
    }
    dissolveSingletonGroups();
    pruneTransitions();
    // Content may have grown past the explicit duration; ripple removals never shrink it.
    m_seq.duration = std::max(m_seq.duration, m_seq.contentEnd());

    std::vector<ClipId> createdSorted = m_created;
    std::sort(createdSorted.begin(), createdSorted.end());
    const auto isRemoved = [&](ClipId id) { return std::binary_search(removedSorted.begin(), removedSorted.end(), id); };
    const auto isCreated = [&](ClipId id) { return std::binary_search(createdSorted.begin(), createdSorted.end(), id); };
    for (const ClipId id : m_created) {
        if (!isRemoved(id)) {
            result.created.push_back(id);
        }
    }
    for (const ClipId id : m_placed) {
        if (!isRemoved(id)) {
            result.placed.push_back(id);
        }
    }
    for (const ClipId id : m_removed) {
        if (!isCreated(id)) {
            result.removed.push_back(id);
        }
    }
    for (const TransitionId id : m_createdTransitions) {
        const bool kept = std::any_of(m_seq.videoTracks.begin(), m_seq.videoTracks.end(), [&](const TrackPtr& t) {
            return std::any_of(t->transitions.begin(), t->transitions.end(), [&](const Transition& tr) { return tr.id == id; });
        }) || std::any_of(m_seq.audioTracks.begin(), m_seq.audioTracks.end(), [&](const TrackPtr& t) {
            return std::any_of(t->transitions.begin(), t->transitions.end(), [&](const Transition& tr) { return tr.id == id; });
        });
        if (kept) {
            result.createdTransitions.push_back(id);
        }
    }
    result.createdEffects = m_createdEffects;
    for (const TrackId id : m_createdTracks) {
        if (m_seq.findTrack(id) || m_seq.findSubmix(id)) { // not erased again by the same edit
            result.createdTracks.push_back(id);
        }
    }

    auto next = std::make_shared<const Sequence>(std::move(m_seq));
    SequenceChange change = diffSequence(m_original, *next);
    if (!change.empty()) {
        result.changes.sequences.push_back(std::move(change));
    }
    result.state = m_state.withSequence(std::move(next));
    result.state.ids = m_ids;
    return result;
}

} // namespace wabcut::edit::detail
