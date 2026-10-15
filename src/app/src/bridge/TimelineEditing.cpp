#include "TimelineEditing.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Snap.h>

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

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
    if (!stretched || edge == edit::Edge::Tail)
        return stretched;
    const SequencePtr after = stretched->state.sequencePtr(sequence);
    const auto moved = after ? after->findClip(clip) : std::nullopt;
    if (!moved)
        return stretched;
    const Ticks shift = c.end() - moved->clip().end();
    if (shift == Ticks{0})
        return stretched;
    auto result = edit::moveClips(stretched->state, sequence, clips, shift, {}, edit::MoveMode::Overwrite, links);
    if (result)
        result->changes = wabcut::diff(state, result->state); // both steps
    return result;
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
        if (spec.newAudioTracks <= 0)
            return edit::moveClips(state, sequence, spec.clips, spec.delta, spec.tracks, spec.moveMode, spec.links);
        // Dropping below the last track: add the tracks, then move onto them (one edit, one undo step).
        ProjectState next = state;
        for (int i = 0; i < spec.newAudioTracks; ++i) {
            auto added = edit::addTrack(next, sequence, TrackKind::Audio);
            if (!added)
                return std::unexpected(added.error());
            next = std::move(added->state);
        }
        return edit::moveClips(next, sequence, spec.clips, spec.delta, spec.tracks, spec.moveMode, spec.links);
    }
    case GestureKind::Trim:
        return edit::trimEdge(state, sequence, clip, spec.edge, spec.delta, spec.links);
    case GestureKind::RippleTrim:
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
