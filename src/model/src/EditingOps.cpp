// Premiere's editing commands (P.2, E5): EditingOps.h.

#include <wabcut/model/EditingOps.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>

namespace wabcut::edit {

using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

namespace {

MediaPtr* mediaSlot(ProjectState& state, MediaId id) {
    const auto it = std::lower_bound(state.media.begin(), state.media.end(), id,
                                     [](const MediaPtr& m, MediaId value) { return m->id < value; });
    return it != state.media.end() && (*it)->id == id ? &*it : nullptr;
}


/// `clips` with their linked partners (no lock checks: for reading).
std::set<std::uint64_t> withPartners(const Sequence& seq, std::span<const ClipId> clips) {
    std::set<std::uint64_t> out;
    std::set<std::uint64_t> groups;
    seq.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (std::find(clips.begin(), clips.end(), c.id) != clips.end()) {
                out.insert(c.id.value);
                if (c.linkGroup.isValid()) {
                    groups.insert(c.linkGroup.value);
                }
            }
        }
    });
    seq.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.linkGroup.isValid() && groups.contains(c.linkGroup.value)) {
                out.insert(c.id.value);
            }
        }
    });
    return out;
}

/// The stream of `item` a clip of `kind` with stream index `index` plays after a replace: the same index when the item
/// has it, else its first stream of that kind.
std::optional<StreamRef> replacementStream(const MediaItem& item, StreamKind kind, std::int32_t index) {
    const std::size_t count = kind == StreamKind::Video ? item.info.video.size() : item.info.audio.size();
    if (count == 0) {
        return std::nullopt;
    }
    return StreamRef{kind, index >= 0 && static_cast<std::size_t>(index) < count ? index : 0};
}

std::string clipLabel(const Clip& c) { return "clip " + std::to_string(c.id.value) + " ('" + c.name() + "')"; }

} // namespace

// --- Replace Edit -------------------------------------------------------------------------------------------------

std::optional<Ticks> replaceMatchFrameIn(const Clip& clip, Ticks sourcePlayhead, Ticks sequencePlayhead) {
    if (clip.isFreeze()) {
        return sourcePlayhead;
    }
    if (clip.isRemapped()) {
        return std::nullopt;
    }
    const Ticks offset = toSourceTicks(sequencePlayhead - clip.start(), clip.speed);
    if (!clip.reversed) {
        return sourcePlayhead - offset;
    }
    // Reversed: the clip's first frame shows sourceOut - 1; sourceOut = sourcePlayhead + 1 + offset.
    return sourcePlayhead + Ticks{1} + offset - clip.sourceDuration();
}

EditOutcome replaceClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips,
                         const ReplaceSource& source, LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const MediaItem* item = state.findMedia(source.media);
    if (!item) {
        return std::unexpected(notFound("media " + std::to_string(source.media.value) + " does not exist"));
    }
    if (item->isGenerator()) {
        return std::unexpected(invalidArgument("replace with a media file: '" + item->name + "' is generated"));
    }
    if (clips.empty()) {
        return std::unexpected(invalidArgument("choose the clip to replace"));
    }
    const auto refs = detail::resolveClips(**seq, clips, links);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    // Partners keep their sync with the first clip: every source time moves by the same amount.
    const auto anchor = (*seq)->findClip(clips.front());
    if (!anchor) {
        return std::unexpected(notFound("clip " + std::to_string(clips.front().value) + " is not in the sequence"));
    }
    const Ticks shift = source.sourceIn - anchor->clip().sourceIn;
    SequenceEditor ed(state, **seq);
    std::size_t replaced = 0;
    for (const detail::ClipRef& ref : *refs) {
        const Clip& c = ed.track(ref.track).clips[ref.index];
        const auto stream = replacementStream(*item, c.stream.kind, c.stream.index);
        if (!stream) {
            continue; // the item has no stream of this kind: the clip stays
        }
        if (c.isRemapped() || c.isFreeze()) {
            return std::unexpected(invalidArgument(clipLabel(c) + (c.isFreeze() ? " is a frame hold" : " is time-remapped") +
                                                   ": replace it as a clip at a constant speed"));
        }
        const Ticks in = c.sourceIn + shift;
        const SourceBounds bounds = item->sourceBounds(*stream);
        if (in < bounds.start || (bounds.end && in + c.sourceDuration() > *bounds.end)) {
            return std::unexpected(invalidArgument("'" + item->name + "' has too little media to replace " + clipLabel(c) +
                                                   " from there (insufficient media)"));
        }
        Clip& m = ed.mutableClip(ref.track, ref.index);
        m.media = item->id;
        m.sequence = {};
        m.angles = {};
        m.stream = *stream;
        m.sourceIn = in;
        ClipContent content = m.content();
        content.name = item->name;
        content.unknownSource.clear();
        content.graphic.reset();
        content.channels.reset(); // the old source's channel layout
        m.setContent(std::move(content));
        ++replaced;
    }
    if (replaced == 0) {
        return std::unexpected(invalidArgument("'" + item->name + "' has no picture or sound for the selected clips"));
    }
    return ed.finish();
}

// --- Replace Footage, Make Offline ----------------------------------------------------------------------------------

std::expected<ProjectState, Error> replaceFootage(const ProjectState& state, MediaId media, const MediaItem& replacement,
                                                  bool rename) {
    const MediaItem* original = state.findMedia(media);
    if (!original) {
        return std::unexpected(notFound("media " + std::to_string(media.value) + " does not exist"));
    }
    if (original->isGenerator()) {
        return std::unexpected(invalidArgument("'" + original->name + "' is generated: it has no footage to replace"));
    }
    if (replacement.isGenerator() || replacement.locator.path.empty()) {
        return std::unexpected(invalidArgument("replace footage with a media file"));
    }
    ProjectState next = state;
    const auto rewrite = [&](MediaPtr& slot, bool renameIt) {
        auto item = std::make_shared<MediaItem>(*slot);
        item->locator.path = replacement.locator.path;
        item->locator.relativePath = replacement.locator.relativePath;
        item->locator.fingerprint = replacement.locator.fingerprint;
        item->locator.offline = false;
        item->locator.proxy.reset();
        item->info = replacement.info;
        item->audioTakes.clear();
        item->madeOffline = false;
        if (renameIt && !replacement.name.empty()) {
            item->name = replacement.name;
        }
        item->clipDefaults = std::make_shared<const ClipContent>(ClipContent{item->name, {}});
        slot = std::move(item);
    };
    const std::string oldPath = original->locator.path;
    for (MediaPtr& m : next.media) {
        if (m->id == media) {
            rewrite(m, rename);
        } else if (m->subclip && m->subclip->master == media && m->locator.path == oldPath) {
            rewrite(m, false); // its subclips play the same file
        }
    }
    // Every clip of it must still find its stream and its media.
    for (const SequencePtr& s : next.sequences) {
        std::optional<std::string> problem;
        s->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                if (problem || !c.media.isValid()) {
                    continue;
                }
                const MediaItem* item = next.findMedia(c.media);
                if (!item || (item->id != media && !(item->subclip && item->subclip->master == media))) {
                    continue;
                }
                if (!item->hasStream(c.stream)) {
                    problem = "the new file has no " + std::string(c.stream.kind == StreamKind::Video ? "video" : "audio") +
                              " stream " + std::to_string(c.stream.index + 1) + " for " + clipLabel(c) + " in '" + s->name + "'";
                } else if (const auto bounds = clipBoundsProblem(next, c)) {
                    problem = clipLabel(c) + " in '" + s->name + "' " + *bounds + " in the new file";
                }
            }
        });
        if (problem) {
            return std::unexpected(invalidArgument(*problem));
        }
    }
    return next;
}

std::expected<ProjectState, Error> makeOffline(const ProjectState& state, std::span<const MediaId> media) {
    ProjectState next = state;
    for (const MediaId id : media) {
        MediaPtr* slot = mediaSlot(next, id);
        if (!slot) {
            return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
        }
        if ((*slot)->isGenerator()) {
            return std::unexpected(invalidArgument("'" + (*slot)->name + "' is generated: it has no file to take offline"));
        }
        if ((*slot)->madeOffline && (*slot)->locator.offline) {
            continue;
        }
        auto item = std::make_shared<MediaItem>(**slot);
        item->madeOffline = true;
        item->locator.offline = true;
        *slot = std::move(item);
    }
    return next;
}

// --- Make Subsequence ------------------------------------------------------------------------------------------------

std::expected<Created<SequenceId>, Error> makeSubsequence(const ProjectState& state, SequenceId sequenceId,
                                                          std::span<const ClipId> clips, std::string name) {
    const Sequence* parent = state.findSequence(sequenceId);
    if (!parent) {
        return std::unexpected(notFound("sequence " + std::to_string(sequenceId.value) + " does not exist"));
    }
    // What to copy: the clips and their partners, or what lies on the targeted tracks between In and Out.
    std::set<std::uint64_t> chosen;
    std::optional<TimeRange> window;
    if (!clips.empty()) {
        for (const ClipId id : clips) {
            if (!parent->findClip(id)) {
                return std::unexpected(notFound("clip " + std::to_string(id.value) + " is not in the sequence"));
            }
        }
        chosen = withPartners(*parent, clips);
    } else {
        window = parent->markedRange();
        if (!window || window->duration.value <= 0) {
            return std::unexpected(invalidArgument("select clips, or set In and Out, to make a subsequence"));
        }
        std::vector<TrackId> targets = targetedTracks(*parent);
        parent->forEachTrack([&](const Track& t) {
            if (std::find(targets.begin(), targets.end(), t.id) == targets.end()) {
                return;
            }
            for (const Clip& c : t.clips) {
                if (c.timelineRange.overlaps(*window)) {
                    chosen.insert(c.id.value);
                }
            }
        });
    }
    if (chosen.empty()) {
        return std::unexpected(invalidArgument(window ? "nothing on the targeted tracks between In and Out"
                                                      : "select the clips to make a subsequence of"));
    }
    Ticks origin = Ticks::max();
    Ticks spanEnd = Ticks::min();
    parent->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (chosen.contains(c.id.value)) {
                origin = std::min(origin, window ? std::max(c.start(), window->start) : c.start());
                spanEnd = std::max(spanEnd, window ? std::min(c.end(), window->end()) : c.end());
            }
        }
    });
    if (window) {
        origin = window->start;
        spanEnd = window->end();
    }
    if (name.empty()) {
        for (int n = 1; n < 10000; ++n) {
            const std::string candidate = parent->name + " Sub_" + (n < 10 ? "0" : "") + std::to_string(n);
            if (std::none_of(state.sequences.begin(), state.sequences.end(),
                             [&](const SequencePtr& s) { return s->name == candidate; })) {
                name = candidate;
                break;
            }
        }
    }
    SequenceSettings settings;
    settings.name = name;
    settings.frameRate = parent->frameRate;
    settings.width = parent->width;
    settings.height = parent->height;
    settings.audioRate = parent->audioRate;
    settings.dropFrame = parent->dropFrame;
    settings.videoTracks = static_cast<std::int32_t>(parent->videoTracks.size());
    settings.audioTracks = static_cast<std::int32_t>(parent->audioTracks.size());
    settings.bin = parent->bin;
    auto created = addSequence(state, settings);
    if (!created) {
        return std::unexpected(created.error());
    }
    // The parent's other settings and track names, roles, formats and switches (not locks or targets).
    {
        auto seq = std::make_shared<Sequence>(*created->state.findSequence(created->id));
        seq->pixelAspect = parent->pixelAspect;
        seq->fields = parent->fields;
        seq->previewFormat = parent->previewFormat;
        seq->videoDisplay = parent->videoDisplay;
        seq->audioDisplay = parent->audioDisplay;
        seq->masterFormat = parent->masterFormat;
        seq->rippleMarkers = parent->rippleMarkers; // 0.4: Ripple Sequence Markers
        for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            auto& mine = kind == TrackKind::Video ? seq->videoTracks : seq->audioTracks;
            const auto& theirs = parent->tracks(kind);
            for (std::size_t i = 0; i < mine.size() && i < theirs.size(); ++i) {
                auto t = std::make_shared<Track>(*mine[i]);
                t->name = theirs[i]->name;
                t->role = theirs[i]->role;
                t->format = theirs[i]->format;
                t->enabled = theirs[i]->enabled;
                t->muted = theirs[i]->muted;
                t->syncLocked = theirs[i]->syncLocked;
                mine[i] = std::move(t);
            }
        }
        created->state = created->state.withSequence(std::move(seq));
    }
    const Sequence& fresh = *created->state.findSequence(created->id);
    SequenceEditor ed(created->state, fresh);
    std::unordered_map<std::uint64_t, LinkGroupId> groups;
    std::unordered_map<std::uint64_t, ClipId> copies;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& theirs = parent->tracks(kind);
        const auto& mine = fresh.tracks(kind);
        for (std::size_t i = 0; i < theirs.size() && i < mine.size(); ++i) {
            for (const Clip& original : theirs[i]->clips) {
                if (!chosen.contains(original.id.value)) {
                    continue;
                }
                Clip c = original;
                if (window) {
                    if (c.start() < window->start) {
                        c.moveHead(window->start - c.start());
                    }
                    if (c.end() > window->end()) {
                        c.moveTail(window->end() - c.end());
                    }
                    if (c.duration().value <= 0) {
                        continue;
                    }
                }
                c.timelineRange = {c.start() - origin, c.duration()};
                c.id = ed.newClipId();
                if (original.linkGroup.isValid()) {
                    auto [it, added] = groups.try_emplace(original.linkGroup.value);
                    if (added) {
                        it->second = ed.newLinkGroup();
                    }
                    c.linkGroup = it->second;
                }
                copies[original.id.value] = c.id;
                ed.placeNewClip(mine[i]->id, std::move(c), false);
            }
            // The transitions between copied clips (one-sided ones on a copied clip).
            for (const Transition& tr : theirs[i]->transitions) {
                const bool a = !tr.outgoing.isValid() || copies.contains(tr.outgoing.value);
                const bool b = !tr.incoming.isValid() || copies.contains(tr.incoming.value);
                if (!a || !b) {
                    continue;
                }
                Transition copy = tr;
                copy.id = ed.newTransitionId();
                copy.outgoing = tr.outgoing.isValid() ? copies[tr.outgoing.value] : ClipId{};
                copy.incoming = tr.incoming.isValid() ? copies[tr.incoming.value] : ClipId{};
                ed.mutableTrack(mine[i]->id).transitions.push_back(std::move(copy));
            }
        }
    }
    // Markers and caption cues over the span.
    const TimeRange span = TimeRange::fromStartEnd(origin, std::max(origin + Ticks{1}, spanEnd));
    Sequence& h = ed.header();
    for (const Marker& m : parent->markers) {
        if (m.start >= span.start && m.start < span.end()) {
            Marker copy = m;
            copy.start = m.start - origin;
            copy.duration = std::min(m.duration, span.end() - m.start);
            if (copy.highlight) {
                copy.highlight->peak = std::clamp(copy.highlight->peak - origin, copy.start, copy.end());
            }
            h.markers.push_back(std::move(copy));
        }
    }
    for (const CaptionTrack& ct : parent->captionTracks) {
        CaptionTrack copy = ct;
        copy.cues.clear();
        for (const CaptionCue& cue : ct.cues) {
            const Ticks start = std::max(cue.start, span.start);
            const Ticks end = std::min(cue.end(), span.end());
            if (end > start) {
                CaptionCue c = cue;
                c.start = start - origin;
                c.duration = end - start;
                copy.cues.push_back(std::move(c));
            }
        }
        if (!copy.cues.empty()) {
            copy.id = ed.newCaptionTrackId();
            h.captionTracks.push_back(std::move(copy));
        }
    }
    EditResult r = ed.finish();
    return Created<SequenceId>{std::move(r.state), created->id};
}

// --- Through edits ---------------------------------------------------------------------------------------------------

bool isThroughEdit(const Clip& left, const Clip& right) {
    if (left.end() != right.start() || left.media != right.media || left.sequence != right.sequence ||
        !(left.stream == right.stream) || !(left.angles == right.angles) || !(left.speed == right.speed) ||
        left.reversed != right.reversed || left.enabled != right.enabled || left.maintainPitch != right.maintainPitch ||
        left.colorIndex != right.colorIndex || left.hasUnknownSource() || right.hasUnknownSource()) {
        return false;
    }
    if (!left.sharesContentWith(right) && !(left.content() == right.content())) {
        return false;
    }
    if (left.isFreeze()) {
        return left.sourceIn == right.sourceIn;
    }
    if (left.isRemapped() || !left.reversed) {
        return right.sourceIn == left.sourceOut(); // curve time on a remapped clip
    }
    return right.sourceOut() == left.sourceIn; // reversed: the right piece shows the frames before
}

EditOutcome joinThroughEdits(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips,
                             LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    // The clips whose edges to look at.
    std::set<std::uint64_t> involved;
    if (!clips.empty()) {
        const auto refs = detail::resolveClips(**seq, clips, links);
        if (!refs) {
            return std::unexpected(refs.error());
        }
        for (const detail::ClipRef& r : *refs) {
            involved.insert(r.clip.value);
        }
    }
    SequenceEditor ed(state, **seq);
    std::size_t joined = 0;
    for (const TrackId id : ed.trackIds()) {
        if (ed.track(id).locked) {
            continue;
        }
        // Right to left, so removing a right piece leaves the indices of the cuts still to look at in place.
        for (std::size_t i = ed.track(id).clips.size(); i-- > 1;) {
            const Track& t = ed.track(id);
            const Clip& left = t.clips[i - 1];
            const Clip& right = t.clips[i];
            if (!clips.empty() && !involved.contains(left.id.value) && !involved.contains(right.id.value)) {
                continue;
            }
            if (!isThroughEdit(left, right)) {
                continue;
            }
            const bool transitionAtCut = std::any_of(t.transitions.begin(), t.transitions.end(), [&](const Transition& tr) {
                return tr.outgoing == left.id || tr.incoming == right.id;
            });
            if (transitionAtCut) {
                continue;
            }
            const ClipId leftId = left.id;
            const ClipId rightId = right.id;
            const Ticks length = right.duration();
            ed.removeClip(id, rightId);
            ed.mutableClip(id, i - 1).moveTail(length);
            for (Transition& tr : ed.mutableTrack(id).transitions) {
                if (tr.outgoing == rightId) {
                    tr.outgoing = leftId; // a transition at the right piece's end is now at the joined clip's
                }
            }
            ++joined;
        }
    }
    if (joined == 0) {
        return std::unexpected(invalidArgument(clips.empty() ? "the sequence has no through edits"
                                                             : "there is no through edit at the selected clips"));
    }
    return ed.finish();
}

// --- Gaps ------------------------------------------------------------------------------------------------------------

namespace {

/// Sorted, merged clip ranges of `tracks` (every track when empty).
std::vector<TimeRange> occupied(const Sequence& seq, std::span<const TrackId> tracks) {
    std::vector<TimeRange> ranges;
    seq.forEachTrack([&](const Track& t) {
        if (!tracks.empty() && std::find(tracks.begin(), tracks.end(), t.id) == tracks.end()) {
            return;
        }
        for (const Clip& c : t.clips) {
            ranges.push_back(c.timelineRange);
        }
    });
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& r : ranges) {
        if (!merged.empty() && r.start <= merged.back().end()) {
            const Ticks end = std::max(merged.back().end(), r.end());
            merged.back() = TimeRange::fromStartEnd(merged.back().start, end);
        } else {
            merged.push_back(r);
        }
    }
    return merged;
}

/// The gaps of merged `ranges` inside [from, to): empty time before, between and after them.
std::vector<TimeRange> gapsIn(const std::vector<TimeRange>& ranges, Ticks from, Ticks to) {
    std::vector<TimeRange> out;
    Ticks at = from;
    for (const TimeRange& r : ranges) {
        if (r.end() <= from) {
            continue;
        }
        if (r.start >= to) {
            break;
        }
        if (r.start > at) {
            out.push_back(TimeRange::fromStartEnd(at, r.start));
        }
        at = std::max(at, r.end());
    }
    if (at < to) {
        out.push_back(TimeRange::fromStartEnd(at, to));
    }
    return out;
}

/// Gap starts: of the whole sequence (tracks empty: times no track has a clip in, before the last clip's end), or of
/// each of `tracks` (before its first clip, between clips).
std::vector<Ticks> gapStarts(const Sequence& seq, std::span<const TrackId> tracks) {
    std::vector<Ticks> out;
    const auto add = [&](const std::vector<TimeRange>& merged) {
        if (merged.empty()) {
            return;
        }
        for (const TimeRange& g : gapsIn(merged, Ticks{}, merged.back().end())) {
            out.push_back(g.start);
        }
    };
    if (tracks.empty()) {
        add(occupied(seq, {}));
    } else {
        for (const TrackId id : tracks) {
            const std::array<TrackId, 1> one{id};
            add(occupied(seq, one));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

std::optional<Ticks> nextGap(const Sequence& sequence, Ticks t, std::span<const TrackId> tracks) {
    for (const Ticks g : gapStarts(sequence, tracks)) {
        if (g > t) {
            return g;
        }
    }
    return std::nullopt;
}

std::optional<Ticks> previousGap(const Sequence& sequence, Ticks t, std::span<const TrackId> tracks) {
    std::optional<Ticks> out;
    for (const Ticks g : gapStarts(sequence, tracks)) {
        if (g < t) {
            out = g;
        }
    }
    return out;
}

EditOutcome closeGaps(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    std::vector<TrackId> tracks;
    Ticks from{};
    Ticks to{};
    if (clips.empty()) {
        s.forEachTrack([&](const Track& t) {
            if (!t.locked) {
                tracks.push_back(t.id);
                if (!t.clips.empty()) {
                    to = std::max(to, t.clips[t.clips.size() - 1].end());
                }
            }
        });
    } else {
        const std::set<std::uint64_t> chosen = withPartners(s, clips);
        from = Ticks::max();
        s.forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                if (!chosen.contains(c.id.value)) {
                    continue;
                }
                if (t.locked) {
                    continue;
                }
                if (std::find(tracks.begin(), tracks.end(), t.id) == tracks.end()) {
                    tracks.push_back(t.id);
                }
                from = std::min(from, c.start());
                to = std::max(to, c.end());
            }
        });
        if (tracks.empty()) {
            return std::unexpected(invalidArgument("the selected clips are on locked tracks"));
        }
    }
    const std::vector<TimeRange> gaps = gapsIn(occupied(s, tracks), from, to);

    SequenceEditor ed(state, s);
    std::size_t closed = 0;
    for (auto it = gaps.rbegin(); it != gaps.rend(); ++it) {
        const TimeRange gap = *it;
        // Other unlocked sync-locked tracks lose the same time, so they must be empty there.
        std::vector<TrackId> also;
        bool blocked = false;
        for (const TrackId id : ed.trackIds()) {
            const Track& t = ed.track(id);
            if (std::find(tracks.begin(), tracks.end(), id) != tracks.end() || t.locked || !t.syncLocked) {
                continue;
            }
            for (const Clip& c : t.clips) {
                if (c.timelineRange.overlaps(gap)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                break;
            }
            also.push_back(id);
        }
        if (blocked) {
            continue;
        }
        for (const TrackId id : tracks) {
            ed.rippleRemove(id, gap);
        }
        for (const TrackId id : also) {
            ed.rippleRemove(id, gap);
        }
        ed.captionsRemove(gap);
        ++closed;
    }
    if (closed == 0) {
        return std::unexpected(invalidArgument(gaps.empty() ? "there is no gap to close"
                                                             : "the gaps have material on other sync-locked tracks"));
    }
    return ed.finish();
}

// --- Frame Hold Options ----------------------------------------------------------------------------------------------

namespace {

constexpr std::array<std::pair<HoldOn, std::string_view>, 5> kHoldOn{{
    {HoldOn::SourceTimecode, "sourceTimecode"},
    {HoldOn::SequenceTimecode, "sequenceTimecode"},
    {HoldOn::InPoint, "inPoint"},
    {HoldOn::OutPoint, "outPoint"},
    {HoldOn::Playhead, "playhead"},
}};

} // namespace

std::string_view toString(HoldOn on) {
    for (const auto& [v, name] : kHoldOn) {
        if (v == on) {
            return name;
        }
    }
    return "inPoint";
}

std::optional<HoldOn> holdOnFromString(std::string_view text) {
    for (const auto& [v, name] : kHoldOn) {
        if (name == text) {
            return v;
        }
    }
    return std::nullopt;
}

EditOutcome setFrameHoldOptions(const ProjectState& state, SequenceId sequenceId, ClipId clipId,
                                const FrameHoldOptions& options) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const std::array<ClipId, 1> one{clipId};
    const auto refs = detail::resolveClips(**seq, one, LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    const detail::ClipRef ref = refs->front();
    SequenceEditor ed(state, **seq);
    const Clip& c = ed.track(ref.track).clips[ref.index];
    const MediaItem* item = state.findMedia(c.media);
    if (c.stream.kind != StreamKind::Video || c.isNested() || !item || item->isGenerator()) {
        return std::unexpected(invalidArgument(clipLabel(c) + " is not a video clip of footage: only those hold frames"));
    }
    if (c.isRemapped()) {
        return std::unexpected(invalidArgument(clipLabel(c) + " is time-remapped: add a hold segment to its remap instead"));
    }
    const SourceBounds bounds = item->sourceBounds(c.stream);
    if (!options.enabled) {
        if (!c.isFreeze()) {
            return ed.finish(); // nothing held
        }
        if (bounds.end && c.sourceIn + c.duration() > *bounds.end) {
            return std::unexpected(invalidArgument("'" + item->name + "' ends before " + clipLabel(c) +
                                                   " could play on from its held frame"));
        }
        Clip& m = ed.mutableClip(ref.track, ref.index);
        m.speed = {1, 1};
        ClipContent content = m.content();
        content.holdFilters = false;
        m.setContent(std::move(content));
        return ed.finish();
    }
    Ticks frame;
    switch (options.holdOn) {
    case HoldOn::InPoint:
        frame = c.sourceTimeAt(c.start());
        break;
    case HoldOn::OutPoint:
        frame = c.sourceTimeAt(c.end() - Ticks{1});
        break;
    case HoldOn::SequenceTimecode:
    case HoldOn::Playhead:
        if (options.time < c.start() || options.time >= c.end()) {
            return std::unexpected(invalidArgument("the time to hold is outside " + clipLabel(c)));
        }
        frame = c.sourceTimeAt(options.time);
        break;
    case HoldOn::SourceTimecode:
        frame = options.time;
        if (frame < bounds.start || (bounds.end && frame >= *bounds.end)) {
            return std::unexpected(invalidArgument("the source time to hold is outside '" + item->name + "'"));
        }
        break;
    }
    Clip& m = ed.mutableClip(ref.track, ref.index);
    m.speed = {0, 1};
    m.reversed = false;
    m.sourceIn = frame;
    ClipContent content = m.content();
    content.holdFilters = options.holdFilters;
    m.setContent(std::move(content));
    return ed.finish();
}

// --- Merge Clips -----------------------------------------------------------------------------------------------------

std::expected<Created<SequenceId>, Error> mergeClips(const ProjectState& state, const MergeSpec& spec) {
    if (spec.audio.empty()) {
        return std::unexpected(invalidArgument("merge a video clip with at least one separately recorded audio clip"));
    }
    const MediaItem* video = spec.video.isValid() ? state.findMedia(spec.video) : nullptr;
    if (spec.video.isValid() && !video) {
        return std::unexpected(notFound("media " + std::to_string(spec.video.value) + " does not exist"));
    }
    if (video && !video->info.hasVideo()) {
        return std::unexpected(invalidArgument("'" + video->name + "' has no video to merge"));
    }
    for (const MediaId id : spec.audio) {
        const MediaItem* a = state.findMedia(id);
        if (!a) {
            return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
        }
        if (!a->info.hasAudio()) {
            return std::unexpected(invalidArgument("'" + a->name + "' has no audio to merge"));
        }
        if (id == spec.video) {
            return std::unexpected(invalidArgument("'" + a->name + "' is both the video and an audio clip of the merge"));
        }
    }
    MulticamSpec m;
    const MediaItem* first = video ? video : state.findMedia(spec.audio.front());
    m.name = spec.name.empty() ? first->name + " - Merged" : spec.name;
    m.bin = spec.bin.isValid() ? spec.bin : first->bin;
    if (video) {
        m.angles.push_back(video->id);
    }
    m.angles.insert(m.angles.end(), spec.audio.begin(), spec.audio.end());
    m.sync = spec.sync;
    m.audioSync = spec.audioSync;
    m.audioSyncControl = spec.audioSyncControl;
    auto created = createMulticam(state, m);
    if (!created) {
        return std::unexpected(created.error());
    }
    auto seq = std::make_shared<Sequence>(*created->state.findSequence(created->id));
    if (!seq->multicam) {
        return std::unexpected(invalidArgument("the merge made no synced source"));
    }
    seq->multicam->merged = true;
    // The recorders contribute their sound only: a picture they may have (a second camera's scratch video) goes.
    std::vector<TrackId> recorderVideo;
    for (std::size_t i = video ? 1 : 0; i < seq->multicam->angles.size(); ++i) {
        MulticamAngle& angle = seq->multicam->angles[i];
        if (angle.video.isValid()) {
            recorderVideo.push_back(angle.video);
            angle.video = {};
        }
    }
    std::erase_if(seq->videoTracks, [&](const TrackPtr& t) {
        return std::find(recorderVideo.begin(), recorderVideo.end(), t->id) != recorderVideo.end();
    });
    if (video && spec.removeCameraAudio && !seq->multicam->angles.empty()) {
        // The camera angle keeps its picture only; its audio tracks go.
        const std::vector<TrackId> cameraAudio = std::move(seq->multicam->angles.front().audio);
        seq->multicam->angles.front().audio.clear();
        std::erase_if(seq->audioTracks, [&](const TrackPtr& t) {
            return std::find(cameraAudio.begin(), cameraAudio.end(), t->id) != cameraAudio.end();
        });
    }
    // A clip whose linked partners were on the removed tracks is no longer linked to anything.
    std::unordered_map<std::uint64_t, int> members;
    for (const auto* tracks : {&seq->videoTracks, &seq->audioTracks}) {
        for (const TrackPtr& t : *tracks) {
            for (const Clip& c : t->clips) {
                if (c.linkGroup.isValid()) {
                    ++members[c.linkGroup.value];
                }
            }
        }
    }
    for (auto* tracks : {&seq->videoTracks, &seq->audioTracks}) {
        for (TrackPtr& t : *tracks) {
            const bool lone = std::any_of(t->clips.begin(), t->clips.end(), [&](const Clip& c) {
                return c.linkGroup.isValid() && members[c.linkGroup.value] == 1;
            });
            if (!lone) {
                continue;
            }
            auto track = std::make_shared<Track>(*t);
            for (std::size_t i = 0; i < track->clips.size(); ++i) {
                const Clip& c = track->clips[i];
                if (c.linkGroup.isValid() && members[c.linkGroup.value] == 1) {
                    track->clips.mutableAt(i).linkGroup = {};
                }
            }
            t = std::move(track);
        }
    }
    created->state = created->state.withSequence(std::move(seq));
    return created;
}

// --- Interpret Footage, Modify > Timecode ------------------------------------------------------------------------------

Speed conformSpeed(const MediaItem& item) {
    if (!item.interpretation || !item.interpretation->frameRate || item.info.video.empty()) {
        return {1, 1};
    }
    const Rate native = item.info.video.front().frameRate;
    const Rate assumed = *item.interpretation->frameRate;
    if (!native.isValid() || native.num <= 0 || !assumed.isValid()) {
        return {1, 1};
    }
    // assumed / native = (assumed.num * native.den) / (assumed.den * native.num), reduced.
    std::int64_t num = std::int64_t{assumed.num} * native.den;
    std::int64_t den = std::int64_t{assumed.den} * native.num;
    const std::int64_t g = std::gcd(num, den);
    num /= g;
    den /= g;
    if (num <= 0 || den <= 0 || num > std::numeric_limits<std::int32_t>::max() ||
        den > std::numeric_limits<std::int32_t>::max()) {
        return {1, 1};
    }
    return {static_cast<std::int32_t>(num), static_cast<std::int32_t>(den)};
}

std::optional<Ticks> mediaStartTimecode(const MediaItem& item) {
    if (item.timecode && item.timecode->start) {
        return item.timecode->start;
    }
    return item.info.startTimecode;
}

EditOutcome interpretFootage(const ProjectState& state, std::span<const MediaId> media,
                             const MediaInterpretation& interpretation) {
    const auto ratio = [](Rate r) { return static_cast<double>(r.num) / static_cast<double>(r.den); };
    if (interpretation.frameRate && (!interpretation.frameRate->isValid() || ratio(*interpretation.frameRate) < 0.001 ||
                                     ratio(*interpretation.frameRate) > 1000.0)) {
        return std::unexpected(invalidArgument("the frame rate to assume must be 0.001 to 1000 fps"));
    }
    if (interpretation.pixelAspect && (!interpretation.pixelAspect->isValid() ||
                                       ratio(*interpretation.pixelAspect) < 0.1 ||
                                       ratio(*interpretation.pixelAspect) > 10.0)) {
        return std::unexpected(invalidArgument("the pixel aspect ratio must be 0.1 to 10"));
    }
    ProjectState next = state;
    std::vector<std::pair<MediaId, std::pair<Speed, Speed>>> retime; // (media, (old conform, new conform))
    for (const MediaId id : media) {
        MediaPtr* slot = mediaSlot(next, id);
        if (!slot) {
            return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
        }
        if ((*slot)->isGenerator()) {
            return std::unexpected(invalidArgument("'" + (*slot)->name + "' is generated: it has no footage to interpret"));
        }
        const Speed before = conformSpeed(**slot);
        auto item = std::make_shared<MediaItem>(**slot);
        item->interpretation = interpretation.isEmpty() ? std::nullopt : std::optional(interpretation);
        if (item->interpretation) {
            if (item->interpretation->frameRate) {
                item->interpretation->frameRate = item->interpretation->frameRate->normalized();
            }
            if (item->interpretation->pixelAspect) {
                item->interpretation->pixelAspect = item->interpretation->pixelAspect->normalized();
            }
        }
        const Speed after = conformSpeed(*item);
        *slot = std::move(item);
        if (!(before == after)) {
            retime.emplace_back(id, std::pair{before, after});
        }
    }
    // Clips of a re-timed item: their speed times after / before, keeping their source In (Speed/Duration, no ripple).
    for (const auto& [id, conform] : retime) {
        const auto [before, after] = conform;
        for (const SequencePtr& s : state.sequences) {
            std::vector<std::pair<ClipId, Speed>> clips;
            s->forEachTrack([&](const Track& t) {
                if (t.locked) {
                    return;
                }
                for (const Clip& c : t.clips) {
                    if (c.media != id || c.isFreeze() || c.isRemapped()) {
                        continue;
                    }
                    std::int64_t num = std::int64_t{c.speed.num} * after.num * before.den;
                    std::int64_t den = std::int64_t{c.speed.den} * after.den * before.num;
                    const std::int64_t g = std::gcd(num, den);
                    num /= g;
                    den /= g;
                    if (num > 0 && den > 0 && num <= std::numeric_limits<std::int32_t>::max() &&
                        den <= std::numeric_limits<std::int32_t>::max()) {
                        clips.emplace_back(c.id, Speed{static_cast<std::int32_t>(num), static_cast<std::int32_t>(den)});
                    }
                }
            });
            for (const auto& [clip, speed] : clips) {
                SpeedChange change;
                change.speed = speed;
                const std::array<ClipId, 1> one{clip};
                auto r = setSpeed(next, s->id, one, change, LinkMode::IgnoreLinks);
                if (!r) {
                    return std::unexpected(r.error());
                }
                next = std::move(r->state);
            }
        }
    }
    EditResult out;
    out.changes = diff(state, next);
    out.state = std::move(next);
    return out;
}

std::expected<ProjectState, Error> setMediaTimecode(const ProjectState& state, std::span<const MediaId> media,
                                                    std::optional<Ticks> start, std::string reelName) {
    if (start && (start->value < 0 || *start > kMaxStartTimecode)) {
        return std::unexpected(invalidArgument("the start timecode must be 00:00:00:00 to 24 hours"));
    }
    if (reelName.size() > MediaTimecode::kMaxReelNameBytes) {
        return std::unexpected(invalidArgument("the reel name is longer than " +
                                               std::to_string(MediaTimecode::kMaxReelNameBytes) + " bytes"));
    }
    ProjectState next = state;
    for (const MediaId id : media) {
        MediaPtr* slot = mediaSlot(next, id);
        if (!slot) {
            return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
        }
        auto item = std::make_shared<MediaItem>(**slot);
        if (!start && reelName.empty()) {
            item->timecode.reset();
        } else {
            item->timecode = MediaTimecode{start, reelName};
        }
        *slot = std::move(item);
    }
    return next;
}

} // namespace wabcut::edit
