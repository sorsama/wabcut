// ProjectController: finishing (M5d) — parameters at the playhead, transition selection and default transitions,
// and the Speed/Duration dialog's numbers. Effect stacks and keyframes are edited by EffectControlsModel through
// applyEditFn() and setParameterAtPlayhead().

#include "ProjectController.h"

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace wabcut::app {

namespace {

/// Exact speed from a percentage (three decimals), like the typed command clip.speedDuration.
Speed speedFromPercent(double percent) {
    const auto num = static_cast<std::int64_t>(std::llround(percent * 1000.0));
    std::int64_t den = 100000;
    const std::int64_t g = std::gcd(num, den);
    return Speed{static_cast<std::int32_t>(num / (g ? g : 1)), static_cast<std::int32_t>(den / (g ? g : 1))};
}

} // namespace

// ---- Time helpers -----------------------------------------------------------------------------------

std::optional<Ticks> ProjectController::parseTimecodeText(const QString& text) const {
    const SequencePtr seq = currentSequence();
    const Rate rate = seq ? seq->frameRate : kRate30;
    const auto parsed = parseTimecode(text.trimmed().toStdString(), rate, seq && seq->dropFrame);
    if (!parsed)
        return std::nullopt;
    return *parsed;
}

Ticks ProjectController::frameTicks() const {
    const SequencePtr seq = currentSequence();
    return framesToTicks(1, seq ? seq->frameRate : kRate30);
}

QString ProjectController::normalizeTimecode(const QString& text) const {
    const auto t = parseTimecodeText(text);
    return t && *t >= Ticks{} ? formatTimecode(*t) : QString();
}

// ---- Parameters -------------------------------------------------------------------------------------

PropertyFrame ProjectController::propertyFrame(const Clip& clip) const {
    const SequencePtr seq = currentSequence();
    PropertyFrame frame;
    if (seq) {
        frame.sequenceWidth = frame.mediaWidth = seq->width;
        frame.sequenceHeight = frame.mediaHeight = seq->height;
    }
    if (const MediaItem* media = snapshot()->findMedia(clip.media); media && !media->info.video.empty()) {
        frame.mediaWidth = media->info.video.front().width;
        frame.mediaHeight = media->info.video.front().height;
    }
    return frame;
}

Ticks ProjectController::curveTimeAtPlayhead(const Clip& clip) const {
    return clip.curveTimeAt(std::clamp(m_playhead, clip.start(), std::max(clip.start(), clip.end() - Ticks{1})));
}

ParamValue ProjectController::parameterValue(const Clip& clip, const edit::ParamRef& param) {
    if (param.intrinsic)
        return clip.properties()[*param.intrinsic];
    for (const EffectInstance& instance : clip.effects()) {
        if (instance.id != param.effect)
            continue;
        if (const ParamValue* value = instance.find(param.key))
            return *value;
        if (const EffectDescriptor* d = EffectRegistry::builtin().find(instance.effect)) {
            if (const EffectParamInfo* info = d->param(param.key))
                return ParamValue(info->defaultValue);
        }
    }
    return ParamValue(0.0);
}

bool ProjectController::setParameterAtPlayhead(ClipId clipId, const edit::ParamRef& param, double value,
                                               const char* label, std::string_view mergeKey) {
    const SequencePtr seq = currentSequence();
    const auto loc = seq ? seq->findClip(clipId) : std::nullopt;
    if (!loc || !std::isfinite(value))
        return false;
    const Clip& clip = loc->clip();
    const ParamValue current = parameterValue(clip, param);
    ParamValue next(value);
    if (const Curve* curve = current.curve()) {
        // Animated: the value goes into the key at the playhead (Premiere's Effect Controls).
        const Ticks t = curveTimeAtPlayhead(clip);
        Curve keys = *curve;
        if (const auto at = keys.keyAt(t)) {
            if (keys.keys[*at].value == value)
                return true;
            keys.keys[*at].value = value;
        } else {
            const std::size_t insertAt = keys.insertionIndex(t);
            Keyframe key{t, value};
            if (insertAt > 0)
                key.interpolation = keys.keys[insertAt - 1].interpolation; // continue the curve's shape
            keys.keys.insert(keys.keys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
        }
        next = ParamValue(std::move(keys));
    } else if (current.constant() == value) {
        return true;
    }
    const SequenceId seqId = m_sequence;
    return applyEditFn(
        label, [&](const ProjectState& state) { return edit::setParameter(state, seqId, clipId, param, next); },
        mergeKey);
}

// ---- Transitions ------------------------------------------------------------------------------------

QString ProjectController::selectedTransition() const {
    return transitionKey(m_selectedTransition);
}

std::optional<ProjectController::TransitionLocation> ProjectController::findTransition(TransitionId id) const {
    const SequencePtr seq = currentSequence();
    if (!seq || !id)
        return std::nullopt;
    std::optional<TransitionLocation> found;
    seq->forEachTrack([&](const Track& track) {
        for (const Transition& t : track.transitions) {
            if (t.id == id)
                found = TransitionLocation{&track, &t};
        }
    });
    // The pointers stay valid while the snapshot holding the sequence lives (the session keeps it).
    return found;
}

void ProjectController::selectTransitionById(TransitionId transition) {
    if (transition && !findTransition(transition))
        transition = {};
    if (transition)
        setSelection({}); // a transition replaces the clip selection
    if (transition == m_selectedTransition)
        return;
    m_selectedTransition = transition;
    emit selectedTransitionChanged();
}

void ProjectController::selectTransition(const QString& transitionId) {
    selectTransitionById(parseTransitionKey(transitionId).value_or(TransitionId{}));
}

bool ProjectController::deleteSelectedTransition() {
    if (!m_selectedTransition)
        return false;
    const SequenceId seq = m_sequence;
    const TransitionId id = m_selectedTransition;
    applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Delete Transition"),
                [=](const ProjectState& state) { return edit::removeTransition(state, seq, id); });
    return true;
}

bool ProjectController::applyDefaultTransition(bool audio) {
    const SequenceId seq = m_sequence;
    const Ticks playhead = m_playhead;
    const TrackKind kind = audio ? TrackKind::Audio : TrackKind::Video;
    return applyEditFn(audio ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Apply Audio Transition")
                             : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Apply Video Transition"),
                       [=](const ProjectState& state) { return edit::applyDefaultTransition(state, seq, playhead, kind); });
}

bool ProjectController::applyDefaultTransitionsToSelection() {
    const std::vector<ClipId> clips = selectedClipIds();
    if (clips.empty())
        return false;
    const SequenceId seq = m_sequence;
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Apply Default Transitions"),
                       [&](const ProjectState& state) { return edit::applyDefaultTransitions(state, seq, clips); });
}

void ProjectController::setTimelineHighlights(std::vector<TimelineHighlight> highlights) {
    m_highlights = std::move(highlights);
    emit timelineHighlightsChanged();
}

// ---- Speed/Duration ---------------------------------------------------------------------------------

QVariantMap ProjectController::speedDurationInfo() const {
    const SequencePtr seq = currentSequence();
    const auto loc = seq && m_primary ? seq->findClip(m_primary) : std::nullopt;
    if (!loc)
        return {{QStringLiteral("available"), false}, {QStringLiteral("reason"), tr("Select a clip first")}};
    const Clip& c = loc->clip();
    if (c.isFreeze())
        return {{QStringLiteral("available"), false}, {QStringLiteral("reason"), tr("Frame holds cannot be retimed")},
                {QStringLiteral("frozen"), true}};
    return {
        {QStringLiteral("available"), true},
        {QStringLiteral("reason"), QString()},
        {QStringLiteral("speed"), 100.0 * c.speed.num / c.speed.den},
        {QStringLiteral("durationText"), formatTimecode(c.duration())},
        {QStringLiteral("reversed"), c.reversed},
        // A.10: a clip still at 100 % shows Maintain Audio Pitch on, the default of its first speed change.
        {QStringLiteral("maintainPitch"), c.maintainPitch || (c.speed.isUnity() && !c.reversed && !c.isRemapped())},
        {QStringLiteral("hasAudio"), c.stream.kind == StreamKind::Audio || propertyTarget(StreamKind::Audio).has_value()},
        {QStringLiteral("count"), static_cast<int>(selectedClipIds().size())},
        {QStringLiteral("frozen"), false},
    };
}

QString ProjectController::durationForSpeed(double percent) const {
    const SequencePtr seq = currentSequence();
    const auto loc = seq && m_primary ? seq->findClip(m_primary) : std::nullopt;
    if (!loc || !std::isfinite(percent) || percent <= 0.0 || loc->clip().isFreeze())
        return {};
    const Speed speed = speedFromPercent(percent);
    if (speed.num <= 0)
        return {};
    const Ticks length = snapToFrame(toTimelineTicks(loc->clip().sourceDuration(), speed), seq->frameRate);
    return formatTimecode(std::max(length, frameTicks()));
}

double ProjectController::speedForDuration(const QString& timecode) const {
    const SequencePtr seq = currentSequence();
    const auto loc = seq && m_primary ? seq->findClip(m_primary) : std::nullopt;
    const auto length = parseTimecodeText(timecode);
    if (!loc || !length || *length <= Ticks{} || loc->clip().isFreeze())
        return std::numeric_limits<double>::quiet_NaN();
    const auto speed = edit::speedForDuration(loc->clip().sourceDuration(), *length);
    if (!speed)
        return std::numeric_limits<double>::quiet_NaN();
    return 100.0 * speed->num / speed->den;
}

} // namespace wabcut::app
