#include <wabcut/model/ChangeSet.h>

#include <wabcut/model/ProjectState.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace wabcut {

const TrackChange* SequenceChange::track(TrackId id) const {
    for (const TrackChange& t : tracks) {
        if (t.track == id) {
            return &t;
        }
    }
    return nullptr;
}

std::vector<TrackId> SequenceChange::affectedTracks() const {
    std::vector<TrackId> out;
    out.reserve(tracks.size());
    for (const TrackChange& t : tracks) {
        out.push_back(t.track);
    }
    return out;
}

bool SequenceChange::empty() const {
    return tracks.empty() && addedClips.empty() && removedClips.empty() && modifiedClips.empty() && !settingsChanged &&
           !marksChanged && !markersChanged && !extentChanged && !trackListChanged && !captionsChanged &&
           !beatGridChanged;
}

const SequenceChange* ChangeSet::sequence(SequenceId id) const {
    for (const SequenceChange& s : sequences) {
        if (s.sequence == id) {
            return &s;
        }
    }
    return nullptr;
}

bool ChangeSet::empty() const {
    return sequences.empty() && addedSequences.empty() && removedSequences.empty() && addedMedia.empty() &&
           removedMedia.empty() && modifiedMedia.empty() && !binsChanged && !aiLockChanged && !assetsChanged &&
           !redactionsChanged && !speakersChanged && !transcriptEditsChanged && !captionPresetsChanged &&
           !reviewNotesChanged && !brandKitChanged && !templatesChanged;
}

namespace {

/// How far ahead the clip walk looks to re-synchronise after an inserted or removed clip.
constexpr std::size_t kResyncWindow = 8;

PropertyRoles compareClips(const Clip& a, const Clip& b) {
    PropertyRoles roles;
    if (a.timelineRange != b.timelineRange) {
        roles |= PropertyRole::Timing;
    }
    if (a.sourceIn != b.sourceIn || a.media != b.media || a.stream != b.stream || a.speed != b.speed ||
        a.reversed != b.reversed || a.maintainPitch != b.maintainPitch || a.sequence != b.sequence ||
        a.angles != b.angles) {
        roles |= PropertyRole::Source;
    }
    if (a.enabled != b.enabled) {
        roles |= PropertyRole::Enabled;
    }
    if (a.linkGroup != b.linkGroup) {
        roles |= PropertyRole::Link;
    }
    if (!a.sharesContentWith(b)) {
        if (a.colorIndex != b.colorIndex || a.name() != b.name()) {
            roles |= PropertyRole::Label;
        }
        roles |= a.properties().differences(b.properties());
        if (a.blendMode() != b.blendMode()) {
            roles |= PropertyRole::Opacity; // C.15: the blend mode is the Opacity section's
        }
        if (a.effects() != b.effects()) {
            roles |= PropertyRole::Effects;
        }
        if (a.content().timeRemap != b.content().timeRemap || a.content().unknownSource != b.content().unknownSource) {
            roles |= PropertyRole::Source; // time remapping changes the source shown
        }
        if (a.content().channels != b.content().channels) {
            roles |= PropertyRole::Audio; // A.9: the channel map changes what the clip sounds like
        }
        const Graphic* ga = a.graphic();
        const Graphic* gb = b.graphic();
        if (!(ga == gb || (ga && gb && *ga == *gb))) {
            roles |= PropertyRole::Source; // C.13: the graphic is the picture the clip shows
        }
    } else if (a.colorIndex != b.colorIndex) {
        roles |= PropertyRole::Label;
    }
    return roles;
}

/// k in 1..kResyncWindow when the clip k steps after `it` (at position pos, of `limit`) has `id`; else 0.
std::size_t distanceAhead(ClipList::const_iterator it, std::size_t pos, std::size_t limit, ClipId id) {
    for (std::size_t k = 1; k <= kResyncWindow && pos + k < limit; ++k) {
        ++it;
        if (it->id == id) {
            return k;
        }
    }
    return 0;
}

struct Pending {
    ClipId id;
    const Clip* clip = nullptr;
    std::size_t trackSlot = 0; // index into the builder's track list
};

/// Accumulates one sequence's change while walking its tracks.
class SequenceDiff {
public:
    explicit SequenceDiff(SequenceChange& out) : m_out(out) {}

    std::size_t trackSlot(TrackId id) {
        for (std::size_t i = 0; i < m_tracks.size(); ++i) {
            if (m_tracks[i].track == id) {
                return i;
            }
        }
        m_tracks.push_back(TrackChange{id, {}, false, false, false, false});
        m_streams.emplace_back();
        return m_tracks.size() - 1;
    }

    TrackChange& trackChange(std::size_t slot) { return m_tracks[slot]; }

    void added(std::size_t slot, const Clip& clip) { m_added.push_back({clip.id, &clip, slot}); }
    void removed(std::size_t slot, const Clip& clip) { m_removed.push_back({clip.id, &clip, slot}); }
    void modified(std::size_t slot, const Clip& before, const Clip& after, PropertyRoles roles) {
        m_out.modifiedClips.push_back(after.id);
        m_out.roles |= roles;
        // The walk visits clips in time order, so each stream of ranges arrives sorted and collapses as
        // it goes (a ripple over 10,000 clips ends up as one range per stream).
        appendSorted(m_streams[slot].before, before.timelineRange);
        appendSorted(m_streams[slot].after, after.timelineRange);
    }

    /// Walks the clips of one track that differ between the revisions.
    void diffClips(std::size_t slot, const ClipList& before, const ClipList& after) {
        // Skip the shared chunks at both ends.
        const std::size_t bc = before.chunkCount();
        const std::size_t ac = after.chunkCount();
        std::size_t prefix = 0;
        while (prefix < bc && prefix < ac && before.chunkIdentity(prefix) == after.chunkIdentity(prefix)) {
            ++prefix;
        }
        std::size_t suffix = 0;
        while (suffix < bc - prefix && suffix < ac - prefix &&
               before.chunkIdentity(bc - 1 - suffix) == after.chunkIdentity(ac - 1 - suffix)) {
            ++suffix;
        }
        const std::size_t b0 = prefix < bc ? before.chunkStart(prefix) : before.size();
        const std::size_t b1 = bc - suffix < bc ? before.chunkStart(bc - suffix) : before.size();
        const std::size_t a0 = prefix < ac ? after.chunkStart(prefix) : after.size();
        const std::size_t a1 = ac - suffix < ac ? after.chunkStart(ac - suffix) : after.size();

        auto bi = before.iteratorAt(b0);
        auto ai = after.iteratorAt(a0);
        std::size_t i = b0;
        std::size_t j = a0;
        while (i < b1 && j < a1) {
            const Clip& b = *bi;
            const Clip& a = *ai;
            if (b.id == a.id) {
                if (!(b == a)) {
                    modified(slot, b, a, compareClips(b, a));
                }
                ++bi, ++i, ++ai, ++j;
                continue;
            }
            // Out of step: skip whichever side re-synchronises sooner. Skipped clips are only pending;
            // reconcile() pairs ids that turn up on both sides (moves), so the lists stay exact.
            const std::size_t aheadInAfter = distanceAhead(ai, j, a1, b.id);
            const std::size_t aheadInBefore = distanceAhead(bi, i, b1, a.id);
            if (aheadInBefore != 0 && (aheadInAfter == 0 || aheadInBefore <= aheadInAfter)) {
                for (std::size_t k = 0; k < aheadInBefore; ++k, ++bi, ++i) {
                    removed(slot, *bi);
                }
            } else if (aheadInAfter != 0) {
                for (std::size_t k = 0; k < aheadInAfter; ++k, ++ai, ++j) {
                    added(slot, *ai);
                }
            } else {
                removed(slot, b);
                added(slot, a);
                ++bi, ++i, ++ai, ++j;
            }
        }
        for (; i < b1; ++bi, ++i) {
            removed(slot, *bi);
        }
        for (; j < a1; ++ai, ++j) {
            added(slot, *ai);
        }
    }

    void finish() {
        reconcile();
        for (std::size_t slot = 0; slot < m_tracks.size(); ++slot) {
            TrackChange& t = m_tracks[slot];
            t.ranges.insert(t.ranges.end(), m_streams[slot].before.begin(), m_streams[slot].before.end());
            t.ranges.insert(t.ranges.end(), m_streams[slot].after.begin(), m_streams[slot].after.end());
            mergeRanges(t.ranges);
            if (!t.ranges.empty() || t.headerChanged || t.transitionsChanged || t.added || t.removed) {
                m_out.tracks.push_back(std::move(t));
            }
        }
    }

private:
    void addRange(std::size_t slot, TimeRange r) {
        if (!r.isEmpty()) {
            m_tracks[slot].ranges.push_back(r);
        }
    }

    static void appendSorted(std::vector<TimeRange>& ranges, TimeRange r) {
        if (r.isEmpty()) {
            return;
        }
        if (!ranges.empty() && r.start >= ranges.back().start && r.start <= ranges.back().end()) {
            ranges.back() = TimeRange::fromStartEnd(ranges.back().start, std::max(ranges.back().end(), r.end()));
        } else {
            ranges.push_back(r);
        }
    }

    static void mergeRanges(std::vector<TimeRange>& ranges) {
        std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
        std::vector<TimeRange> merged;
        for (const TimeRange& r : ranges) {
            if (!merged.empty() && r.start <= merged.back().end()) {
                merged.back() = TimeRange::fromStartEnd(merged.back().start, std::max(merged.back().end(), r.end()));
            } else {
                merged.push_back(r);
            }
        }
        ranges = std::move(merged);
    }

    /// Ids that left one place and appeared in another are moves (or unchanged after a resync slip).
    void reconcile() {
        const auto byId = [](const Pending& a, const Pending& b) { return a.id < b.id; };
        std::sort(m_added.begin(), m_added.end(), byId);
        std::sort(m_removed.begin(), m_removed.end(), byId);
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < m_removed.size() || j < m_added.size()) {
            if (j == m_added.size() || (i < m_removed.size() && m_removed[i].id < m_added[j].id)) {
                m_out.removedClips.push_back(m_removed[i].id);
                addRange(m_removed[i].trackSlot, m_removed[i].clip->timelineRange);
                ++i;
            } else if (i == m_removed.size() || m_added[j].id < m_removed[i].id) {
                m_out.addedClips.push_back(m_added[j].id);
                addRange(m_added[j].trackSlot, m_added[j].clip->timelineRange);
                ++j;
            } else {
                const Pending& b = m_removed[i];
                const Pending& a = m_added[j];
                PropertyRoles roles = compareClips(*b.clip, *a.clip);
                if (b.trackSlot != a.trackSlot) {
                    roles |= PropertyRole::Track;
                }
                if (!roles.empty()) {
                    m_out.modifiedClips.push_back(a.id);
                    m_out.roles |= roles;
                    addRange(b.trackSlot, b.clip->timelineRange);
                    addRange(a.trackSlot, a.clip->timelineRange);
                }
                ++i;
                ++j;
            }
        }
    }

    SequenceChange& m_out;
    struct Streams {
        std::vector<TimeRange> before;
        std::vector<TimeRange> after;
    };
    std::vector<TrackChange> m_tracks;
    std::vector<Streams> m_streams; // parallel to m_tracks
    std::vector<Pending> m_added;
    std::vector<Pending> m_removed;
};

const TrackPtr* findTrackPtr(const Sequence& s, TrackId id) {
    for (const auto* list : {&s.videoTracks, &s.audioTracks}) {
        for (const TrackPtr& t : *list) {
            if (t->id == id) {
                return &t;
            }
        }
    }
    return nullptr;
}

std::vector<TrackId> trackOrder(const Sequence& s) {
    std::vector<TrackId> out;
    s.forEachTrack([&](const Track& t) { out.push_back(t.id); });
    return out;
}

} // namespace

SequenceChange diffSequence(const Sequence& before, const Sequence& after) {
    SequenceChange out;
    out.sequence = after.id;
    if (&before == &after) {
        return out;
    }
    out.settingsChanged = before.name != after.name || before.bin != after.bin || !(before.frameRate == after.frameRate) ||
                          before.width != after.width || before.height != after.height ||
                          !(before.audioRate == after.audioRate) || before.dropFrame != after.dropFrame ||
                          before.startTimecode != after.startTimecode ||
                          before.effects != after.effects || before.audioEffects != after.audioEffects ||
                          before.derivedFrom != after.derivedFrom || before.masterVolume != after.masterVolume ||
                          before.masterFormat != after.masterFormat || before.multicam != after.multicam;
    out.marksChanged = before.markIn != after.markIn || before.markOut != after.markOut;
    out.markersChanged = before.markers != after.markers;
    out.captionsChanged = before.captionTracks != after.captionTracks;
    out.beatGridChanged = before.beatGrid != after.beatGrid;
    out.extentChanged = before.duration != after.duration || before.workArea != after.workArea;

    const std::vector<TrackId> beforeOrder = trackOrder(before);
    const std::vector<TrackId> afterOrder = trackOrder(after);
    out.trackListChanged = beforeOrder != afterOrder || before.videoTracks.size() != after.videoTracks.size();

    SequenceDiff d(out);
    std::vector<TrackId> all = beforeOrder;
    for (const TrackId id : afterOrder) {
        if (std::find(all.begin(), all.end(), id) == all.end()) {
            all.push_back(id);
        }
    }
    for (const TrackId id : all) {
        const TrackPtr* b = findTrackPtr(before, id);
        const TrackPtr* a = findTrackPtr(after, id);
        if (b && a && *b == *a) {
            continue;
        }
        const std::size_t slot = d.trackSlot(id);
        if (!b) {
            d.trackChange(slot).added = true;
            for (const Clip& c : (*a)->clips) {
                d.added(slot, c);
            }
            continue;
        }
        if (!a) {
            d.trackChange(slot).removed = true;
            for (const Clip& c : (*b)->clips) {
                d.removed(slot, c);
            }
            continue;
        }
        d.trackChange(slot).headerChanged = !(*b)->sameHeader(**a);
        d.trackChange(slot).transitionsChanged = (*b)->transitions != (*a)->transitions;
        d.diffClips(slot, (*b)->clips, (*a)->clips);
    }
    d.finish();
    return out;
}

ChangeSet diff(const ProjectState& before, const ProjectState& after) {
    ChangeSet out;
    out.binsChanged = before.bins != after.bins;
    out.aiLockChanged = before.aiNoCloud != after.aiNoCloud;
    out.assetsChanged = before.assets != after.assets;
    out.redactionsChanged = before.redactions != after.redactions;
    out.speakersChanged = before.speakers != after.speakers;
    out.transcriptEditsChanged = before.transcriptEdits != after.transcriptEdits;
    out.captionPresetsChanged = before.captionPresets != after.captionPresets;
    out.reviewNotesChanged = before.reviewNotes != after.reviewNotes;
    out.brandKitChanged = before.brandKit != after.brandKit;
    out.templatesChanged = before.templates != after.templates;

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < before.media.size() || j < after.media.size()) {
        if (j == after.media.size() || (i < before.media.size() && before.media[i]->id < after.media[j]->id)) {
            out.removedMedia.push_back(before.media[i++]->id);
        } else if (i == before.media.size() || after.media[j]->id < before.media[i]->id) {
            out.addedMedia.push_back(after.media[j++]->id);
        } else {
            if (before.media[i] != after.media[j] && !(*before.media[i] == *after.media[j])) {
                out.modifiedMedia.push_back(after.media[j]->id);
            }
            ++i;
            ++j;
        }
    }

    i = 0;
    j = 0;
    while (i < before.sequences.size() || j < after.sequences.size()) {
        if (j == after.sequences.size() ||
            (i < before.sequences.size() && before.sequences[i]->id < after.sequences[j]->id)) {
            out.removedSequences.push_back(before.sequences[i++]->id);
        } else if (i == before.sequences.size() || after.sequences[j]->id < before.sequences[i]->id) {
            out.addedSequences.push_back(after.sequences[j++]->id);
        } else {
            if (before.sequences[i] != after.sequences[j]) {
                SequenceChange change = diffSequence(*before.sequences[i], *after.sequences[j]);
                if (!change.empty()) {
                    out.sequences.push_back(std::move(change));
                }
            }
            ++i;
            ++j;
        }
    }
    return out;
}

} // namespace wabcut
