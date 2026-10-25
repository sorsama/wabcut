#include "TimelineEditing.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Snap.h>

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wabcut::app {

namespace {

constexpr std::array<std::pair<TimelineTool, const char*>, 11> kToolKeys{{
    {TimelineTool::Select, "select"},
    {TimelineTool::TrackForward, "trackForward"},
    {TimelineTool::TrackBackward, "trackBackward"},
    {TimelineTool::Ripple, "ripple"},
    {TimelineTool::Roll, "roll"},
    {TimelineTool::Slip, "slip"},
    {TimelineTool::Slide, "slide"},
    {TimelineTool::Razor, "razor"},
    {TimelineTool::Remix, "remix"},
    {TimelineTool::RateStretch, "rateStretch"},
    {TimelineTool::Pen, "pen"},
}};

/// Calls `fn` for every clip of `track` that lies (partly) in `range`.
template <typename Fn>
void forClipsIn(const Track& track, TimeRange range, Fn&& fn) {
    const std::size_t first = track.firstClipAtOrAfter(range.start);
    for (std::size_t i = first > 0 ? first - 1 : 0; i < track.clips.size(); ++i) {
        const Clip& clip = track.clips[i];
        if (clip.start() >= range.end())
            break;
        if (clip.timelineRange.overlaps(range))
            fn(clip);
    }
}

/// The track shift a move applies (2026-10-15): the grabbed clip's kind crosses `spec.tracks` as dragged; the other
/// kind (its linked partners) goes along only when each of its clips lands on a free stretch of an unlocked track,
/// else it keeps its tracks. Dragging linked audio from A2 to A1 used to move the picture from V3 onto the clip on V2;
/// now the picture stays on V3, and goes to V1 when the drag crosses two tracks and V1 is free there. A clip the
/// grabbed kind lands on is replaced, so its own link partners count as free: an A/V clip's sound dragged onto another
/// one's replaces both halves, as a drop does.
/// 0.4, a copy (GestureSpec::copy): the originals stay, so moving clips where a partner's copy would land count as
/// occupied; `partnersLeftOut` says the grabbed kind changes track and its partners' copies cannot go with it (the copy
/// is then the grabbed kind only, unlinked).
struct MovedTracks {
    edit::TrackShift tracks;
    bool partnersLeftOut = false;
};

MovedTracks movedTracks(const ProjectState& state, SequenceId sequence, const GestureSpec& spec) {
    MovedTracks result{spec.tracks, false};
    edit::TrackShift& tracks = result.tracks;
    const Sequence* seq = state.findSequence(sequence);
    const auto grabbed = seq ? seq->findClip(spec.clips.front()) : std::nullopt;
    if (!grabbed)
        return result;
    const TrackKind grabbedKind = grabbed->track->kind;
    int& follow = grabbedKind == TrackKind::Video ? tracks.audio : tracks.video;
    const int lead = grabbedKind == TrackKind::Video ? tracks.video : tracks.audio;
    if (follow == 0) {
        result.partnersLeftOut = spec.copy && lead != 0;
        return result;
    }

    const std::vector<ClipId> ids = spec.links == edit::LinkMode::FollowLinks ? edit::withLinkedClips(*seq, spec.clips)
                                                                               : spec.clips;
    std::unordered_set<std::uint64_t> moving;
    for (const ClipId id : ids)
        moving.insert(id.value);
    // Where the moving clips are, and the delta moveClips applies (no clip starts before 0).
    struct Moving {
        TrackKind kind;
        std::size_t index;
        TimeRange range;
    };
    std::vector<Moving> leads;
    std::vector<Moving> partners;
    Ticks delta = spec.delta;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const std::vector<TrackPtr>& list = seq->tracks(kind);
        for (std::size_t i = 0; i < list.size(); ++i) {
            for (const Clip& clip : list[i]->clips) {
                if (!moving.contains(clip.id.value))
                    continue;
                (kind == grabbedKind ? leads : partners).push_back({kind, i, clip.timelineRange});
                delta = std::max(delta, -clip.start());
            }
        }
    }
    const auto landing = [&](const Moving& m, int shift) -> const Track* {
        const std::vector<TrackPtr>& list = seq->tracks(m.kind);
        const std::int64_t to = static_cast<std::int64_t>(m.index) + shift;
        if (to < 0 || to >= static_cast<std::int64_t>(list.size()))
            return nullptr;
        return list[static_cast<std::size_t>(to)].get();
    };
    const auto at = [&](const Moving& m) { return TimeRange{m.range.start + delta, m.range.duration}; };

    std::vector<LinkGroupId> replaced;
    for (const Moving& m : leads) {
        if (const Track* to = landing(m, lead)) {
            forClipsIn(*to, at(m), [&](const Clip& clip) {
                if (!moving.contains(clip.id.value) && clip.linkGroup)
                    replaced.push_back(clip.linkGroup);
            });
        }
    }
    for (const Moving& m : partners) {
        const Track* to = landing(m, follow);
        bool free = to && !to->locked;
        if (free) {
            forClipsIn(*to, at(m), [&](const Clip& clip) {
                if ((spec.copy || !moving.contains(clip.id.value)) // a copy leaves the originals where they are
                    && (!clip.linkGroup || std::ranges::find(replaced, clip.linkGroup) == replaced.end()))
                    free = false;
            });
        }
        if (!free) {
            follow = 0;
            result.partnersLeftOut = spec.copy && lead != 0;
            break;
        }
    }
    return result;
}

/// 0.4: a copy whose partners cannot go along (MovedTracks::partnersLeftOut) copies the grabbed kind only: the clips
/// (with their linked partners unless the spec ignores links) on tracks of the grabbed clip's kind.
std::vector<ClipId> grabbedKindOnly(const ProjectState& state, SequenceId sequence, const GestureSpec& spec) {
    const Sequence* seq = state.findSequence(sequence);
    const auto grabbed = seq ? seq->findClip(spec.clips.front()) : std::nullopt;
    if (!grabbed)
        return spec.clips;
    const TrackKind kind = grabbed->track->kind;
    const std::vector<ClipId> all = spec.links == edit::LinkMode::FollowLinks ? edit::withLinkedClips(*seq, spec.clips)
                                                                              : spec.clips;
    std::vector<ClipId> out;
    for (const ClipId id : all) {
        if (const auto loc = seq->findClip(id); loc && loc->track->kind == kind)
            out.push_back(id);
    }
    return out;
}

/// Rate Stretch (R, Premiere): the clip keeps its source In and plays the same material over the new length - the
/// Speed/Duration edit at the speed that fills it (linked clips follow, keeping their pitch setting). From the head the
/// clip's end stays where it was.
edit::EditOutcome rateStretch(const ProjectState& state, SequenceId sequence, ClipId clip, edit::Edge edge, Ticks delta,
                              edit::LinkMode links) {
    const SequencePtr seq = state.sequencePtr(sequence);
    const auto loc = seq ? seq->findClip(clip) : std::nullopt;
    if (!loc)
        return std::unexpected(Error{ErrorCode::NotFound, "no such clip"});
    const Clip& c = loc->clip();
    const Ticks length = edge == edit::Edge::Tail ? c.duration() + delta : c.duration() - delta;
    if (length <= Ticks{0})
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the clip cannot be stretched to nothing"});
    const auto speed = edit::speedForDuration(c.sourceDuration(), length);
    if (!speed)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the clip cannot be stretched to that length"});
    edit::SpeedChange change;
    change.speed = *speed;
    const std::array<ClipId, 1> clips{clip};
    auto stretched = edit::setSpeed(state, sequence, clips, change, links);
    if (!stretched)
        return stretched;
    const SequencePtr after = stretched->state.sequencePtr(sequence);
    const auto moved = after ? after->findClip(clip) : std::nullopt;
    if (!moved)
        return stretched;
    if (edge == edit::Edge::Tail) {
        // 0.4: the tail moved by the change of length (the readout's delta; setSpeed itself reports none).
        stretched->appliedDelta = moved->clip().duration() - c.duration();
        return stretched;
    }
    const Ticks shift = c.end() - moved->clip().end();
    if (shift == Ticks{0})
        return stretched;
    auto result = edit::moveClips(stretched->state, sequence, clips, shift, {}, edit::MoveMode::Overwrite, links);
    if (result)
        result->changes = wabcut::diff(state, result->state); // both steps
    return result;
}

/// 0.2: a group's outer edge (GestureSpec::groupTrim): the grabbed clip and the other members trimmed by one delta, the
/// smallest any of them can take (each clamps to its neighbours and its media on its own), as one edit.
edit::EditOutcome trimGroup(const ProjectState& state, SequenceId sequence, const GestureSpec& spec) {
    std::vector<ClipId> clips{spec.clips.front()};
    clips.insert(clips.end(), spec.groupTrim.begin(), spec.groupTrim.end());
    Ticks delta = spec.delta;
    for (const ClipId clip : clips) {
        auto probe = edit::trimEdge(state, sequence, clip, spec.edge, delta, spec.links);
        if (!probe)
            return probe;
        if (std::abs(probe->appliedDelta.value) < std::abs(delta.value) || (probe->appliedDelta.value < 0) != (delta.value < 0))
            delta = probe->appliedDelta;
    }
    ProjectState next = state;
    edit::EditOutcome out = std::unexpected(Error{ErrorCode::InvalidArgument, "no clip to edit"});
    for (const ClipId clip : clips) {
        out = edit::trimEdge(next, sequence, clip, spec.edge, delta, spec.links);
        if (!out)
            return out;
        next = out->state;
    }
    out->changes = wabcut::diff(state, out->state); // every member's trim
    out->appliedDelta = delta;
    return out;
}

} // namespace

QString toolKey(TimelineTool tool) {
    for (const auto& [t, key] : kToolKeys) {
        if (t == tool)
            return QString::fromLatin1(key);
    }
    return QStringLiteral("select");
}

std::optional<TimelineTool> parseToolKey(const QString& key) {
    for (const auto& [t, k] : kToolKeys) {
        if (key == QLatin1String(k))
            return t;
    }
    return std::nullopt;
}

edit::EditOutcome applyGesture(const ProjectState& state, SequenceId sequence, const GestureSpec& spec) {
    if (spec.clips.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "no clip to edit"});
    const ClipId clip = spec.clips.front();
    switch (spec.kind) {
    case GestureKind::Move: {
        const MovedTracks moved = movedTracks(state, sequence, spec);
        // 0.4: Alt at the drop copies (edit::duplicateClips with the same delta, tracks and mode; the originals stay).
        std::vector<ClipId> clips = spec.clips;
        edit::LinkMode links = spec.links;
        if (spec.copy && moved.partnersLeftOut) {
            clips = grabbedKindOnly(state, sequence, spec);
            links = edit::LinkMode::IgnoreLinks;
        }
        const auto run = [&](const ProjectState& s) {
            return spec.copy ? edit::duplicateClips(s, sequence, clips, spec.delta, moved.tracks, spec.moveMode, links)
                             : edit::moveClips(s, sequence, clips, spec.delta, moved.tracks, spec.moveMode, links);
        };
        if (spec.newAudioTracks <= 0)
            return run(state);
        // Dropping below the last track: add the tracks, then move onto them (one edit, one undo step).
        ProjectState next = state;
        for (int i = 0; i < spec.newAudioTracks; ++i) {
            auto added = edit::addTrack(next, sequence, TrackKind::Audio);
            if (!added)
                return std::unexpected(added.error());
            next = std::move(added->state);
        }
        return run(next);
    }
    case GestureKind::Trim:
        if (!spec.groupTrim.empty())
            return trimGroup(state, sequence, spec);
        return edit::trimEdge(state, sequence, clip, spec.edge, spec.delta, spec.links);
    case GestureKind::RippleTrim:
        if (!spec.groupTrim.empty()) {
            // 0.2: a group's outer edge ripples as one edit (every member on it, downstream material, sync locks).
            std::vector<ClipId> edges{clip};
            edges.insert(edges.end(), spec.groupTrim.begin(), spec.groupTrim.end());
            return edit::rippleTrimEdges(state, sequence, edges, spec.edge, spec.delta, spec.links);
        }
        return edit::rippleTrim(state, sequence, clip, spec.edge, spec.delta, spec.links);
    case GestureKind::Roll:
        return edit::rollEdit(state, sequence, clip, spec.edge, spec.delta, spec.links);
    case GestureKind::Slip:
        return edit::slip(state, sequence, spec.clips, spec.delta, spec.links);
    case GestureKind::Slide:
        return edit::slide(state, sequence, clip, spec.delta, spec.links);
    case GestureKind::RateStretch:
        return rateStretch(state, sequence, clip, spec.edge, spec.delta, spec.links);
    case GestureKind::None:
        break;
    }
    return std::unexpected(Error{ErrorCode::InvalidArgument, "no gesture"});
}

const char* gestureLabel(const GestureSpec& spec) {
    switch (spec.kind) {
    case GestureKind::Move:
        if (spec.copy)
            return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Duplicate");
        if (spec.moveMode == edit::MoveMode::Rearrange)
            return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Rearrange");
        return spec.moveMode == edit::MoveMode::Insert
                   ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Insert Move")
                   : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Move");
    case GestureKind::Trim:
        return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Trim");
    case GestureKind::RippleTrim:
        return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Ripple Trim");
    case GestureKind::Roll:
        return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Rolling Edit");
    case GestureKind::Slip:
        return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Slip");
    case GestureKind::Slide:
        return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Slide");
    case GestureKind::RateStretch:
        return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Rate Stretch");
    case GestureKind::None:
        break;
    }
    return QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Edit");
}

double autoScrollVelocity(double pointer, double extent, double zone, double maxSpeed) {
    if (zone <= 0 || extent <= 0)
        return 0;
    zone = std::min(zone, extent / 2);
    if (pointer < zone)
        return -maxSpeed * std::min(1.0, (zone - pointer) / zone);
    if (pointer > extent - zone)
        return maxSpeed * std::min(1.0, (pointer - (extent - zone)) / zone);
    return 0;
}

SnapResult snapDelta(std::span<const Ticks> edges, Ticks delta, std::span<const Ticks> sortedTargets, Ticks tolerance) {
    SnapResult best{delta, std::nullopt};
    std::int64_t bestDistance = 0;
    for (Ticks edge : edges) {
        const Ticks moved = edge + delta;
        const auto target = edit::nearestSnapTarget(sortedTargets, moved, tolerance);
        if (!target)
            continue;
        const std::int64_t distance = std::abs((*target - moved).value);
        if (!best.target || distance < bestDistance) {
            best = {*target - edge, target};
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace wabcut::app
