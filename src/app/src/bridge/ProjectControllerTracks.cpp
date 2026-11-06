// ProjectController: 0.7 stream A, batch A2 - keys for a track's switches and a marker from the In/Out range.
//
//   * Cards 106 and 117, Alt+Shift+O / U / S (Windows; Option on macOS) and Ctrl+Alt+Shift+S: Toggle Track Output (the
//     eye), Toggle Track Mute, Toggle Track Solo and Solo Selected Clip's Track act on the tracks of the selected clips
//     (a linked picture and sound: both rows), else on the targeted tracks. One undo step each, like the track header's
//     own toggles (edit::setTrackState). A video track can be soloed too: while any video track is soloed only the
//     soloed ones show (model/Resolve.h videoTrackShown).
//   * Card 105, Markers > Add Marker from In/Out: a range marker over the sequence's In to Out range.

#include "ProjectController.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <optional>

namespace wabcut::app {

namespace {

enum class SwitchKind { Output, Mute, Solo };

std::optional<SwitchKind> parseSwitch(const QString& what) {
    if (what == QLatin1String("output"))
        return SwitchKind::Output;
    if (what == QLatin1String("mute"))
        return SwitchKind::Mute;
    if (what == QLatin1String("solo"))
        return SwitchKind::Solo;
    return std::nullopt;
}

bool appliesTo(SwitchKind what, TrackKind kind) {
    return what == SwitchKind::Output ? kind == TrackKind::Video : what == SwitchKind::Mute ? kind == TrackKind::Audio : true;
}

bool isOn(SwitchKind what, const Track& t) {
    return what == SwitchKind::Output ? t.enabled : what == SwitchKind::Mute ? t.muted : t.solo;
}

edit::TrackStateChange changeOf(SwitchKind what, bool value) {
    edit::TrackStateChange change;
    if (what == SwitchKind::Output)
        change.enabled = value;
    else if (what == SwitchKind::Mute)
        change.muted = value;
    else
        change.solo = value;
    return change;
}

/// The tracks the track keys act on, in sequence order (V1 up, then A1 down): those of `selection` (a clip's track),
/// else the targeted tracks.
std::vector<const Track*> keyTracks(const Sequence& seq, std::span<const ClipId> selection) {
    std::vector<TrackId> ids;
    for (const ClipId clip : selection) {
        if (const auto loc = seq.findClip(clip); loc && std::ranges::find(ids, loc->track->id) == ids.end())
            ids.push_back(loc->track->id);
    }
    if (ids.empty())
        ids = edit::targetedTracks(seq);
    std::vector<const Track*> tracks;
    for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
        for (const TrackPtr& t : *list) {
            if (t && std::ranges::find(ids, t->id) != ids.end())
                tracks.push_back(t.get());
        }
    }
    return tracks;
}

} // namespace

QString ProjectController::trackKeyProblem(const QString& what) const {
    const auto kind = parseSwitch(what);
    const SequencePtr seq = currentSequence();
    if (!seq)
        return tr("Open a sequence first");
    const std::vector<const Track*> tracks = keyTracks(*seq, selectedClipIds());
    if (tracks.empty())
        return tr("Select a clip or target a track first");
    if (kind && !std::ranges::any_of(tracks, [&](const Track* t) { return appliesTo(*kind, t->kind); }))
        return *kind == SwitchKind::Output ? tr("Track output is a video track switch: select a video clip or target a video track")
                                           : tr("Mute is an audio track switch: select an audio clip or target an audio track");
    return {};
}

bool ProjectController::toggleTrackSwitch(const QString& what) {
    const auto kind = parseSwitch(what);
    if (!kind) {
        setError(tr("Unknown track switch"));
        return false;
    }
    if (const QString problem = trackKeyProblem(what); !problem.isEmpty()) {
        setError(problem);
        return false;
    }
    const SequencePtr seq = currentSequence();
    std::vector<TrackId> ids;
    bool allOn = true;
    for (const Track* t : keyTracks(*seq, selectedClipIds())) {
        if (!appliesTo(*kind, t->kind))
            continue;
        ids.push_back(t->id);
        allOn = allOn && isOn(*kind, *t);
    }
    // Like Toggle All Targets: when every track already has the switch on it goes off, else every one gets it on.
    // Output is the eye (on = shown): the key hides first, then shows again.
    const bool value = !allOn;
    const char* label = *kind == SwitchKind::Output ? (value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Show Track")
                                                             : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Hide Track"))
                        : *kind == SwitchKind::Mute ? (value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Mute Track")
                                                             : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Unmute Track"))
                                                    : (value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Solo Track")
                                                             : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Unsolo Track"));
    const SequenceId sequence = m_sequence;
    const edit::TrackStateChange change = changeOf(*kind, value);
    return applyEditFn(label, [&](const ProjectState& state) -> edit::EditOutcome {
        ProjectState next = state;
        for (const TrackId id : ids) {
            auto step = edit::setTrackState(next, sequence, id, change);
            if (!step)
                return std::unexpected(step.error());
            next = std::move(step->state);
        }
        edit::EditResult result;
        result.changes = diff(state, next);
        result.state = std::move(next);
        return result;
    });
}

bool ProjectController::soloSelectedClipTracks() {
    if (const QString problem = trackKeyProblem(QStringLiteral("solo")); !problem.isEmpty()) {
        setError(problem);
        return false;
    }
    const SequencePtr seq = currentSequence();
    const std::vector<const Track*> mine = keyTracks(*seq, selectedClipIds());
    // Per kind present among the clips' tracks: if exactly those are the soloed ones, the solo ends (Unsolo); else it
    // moves to exactly those (every other track of the kind is unsoloed). A kind with no track there is left alone.
    struct Step {
        TrackId track;
        bool solo = false;
    };
    std::vector<Step> steps;
    bool soloed = false;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        std::vector<const Track*> keep;
        for (const Track* t : mine) {
            if (t->kind == kind)
                keep.push_back(t);
        }
        if (keep.empty())
            continue;
        const auto& all = kind == TrackKind::Video ? seq->videoTracks : seq->audioTracks;
        const bool already = std::ranges::all_of(all, [&](const TrackPtr& t) {
            return t && (t->solo == (std::ranges::find(keep, t.get()) != keep.end()));
        });
        for (const TrackPtr& t : all) {
            if (!t)
                continue;
            const bool wanted = !already && std::ranges::find(keep, t.get()) != keep.end();
            if (t->solo != wanted)
                steps.push_back({t->id, wanted});
        }
        soloed = soloed || !already;
    }
    if (steps.empty())
        return true; // nothing to change (cannot happen: `already` flips every kind it finds on)
    const SequenceId sequence = m_sequence;
    const char* label = soloed ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Solo Track")
                               : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Unsolo Tracks");
    return applyEditFn(label, [&](const ProjectState& state) -> edit::EditOutcome {
        ProjectState next = state;
        for (const Step& s : steps) {
            edit::TrackStateChange change;
            change.solo = s.solo;
            auto step = edit::setTrackState(next, sequence, s.track, change);
            if (!step)
                return std::unexpected(step.error());
            next = std::move(step->state);
        }
        edit::EditResult result;
        result.changes = diff(state, next);
        result.state = std::move(next);
        return result;
    });
}

QString ProjectController::markerFromInOutProblem() const {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return tr("Open a sequence first");
    if (!seq->markIn || !seq->markOut || *seq->markOut <= *seq->markIn)
        return tr("Set In and Out first");
    return {};
}

bool ProjectController::addMarkerFromInOut() {
    if (const QString problem = markerFromInOutProblem(); !problem.isEmpty()) {
        setError(problem);
        return false;
    }
    const SequencePtr seq = currentSequence();
    Marker marker;
    marker.start = *seq->markIn;
    marker.duration = *seq->markOut - *seq->markIn; // the Out mark is exclusive: the range covers the last marked frame
    const SequenceId sequence = m_sequence;
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Marker from In/Out"),
                       [&](const ProjectState& state) { return edit::addMarker(state, sequence, marker); });
}

} // namespace wabcut::app
