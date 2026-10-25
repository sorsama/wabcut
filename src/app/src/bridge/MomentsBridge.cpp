#include "MomentsBridge.h"

#include "BridgeIds.h"
#include "GraphicsBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "ShortsBridge.h" // appGamingServices, BridgeJob

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Command.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/TimeRemap.h>

#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Undo labels are translated where ProjectController shows them.
#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

namespace wabcut::app {

namespace {

using Json = command::Json;
using Frames = std::int64_t;

Ticks secondsToTicks(double s) {
    return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))};
}
Ticks times(Ticks t, std::int64_t n) { return Ticks{t.value * n}; }
/// The nearest frame boundary.
Ticks nearestFrame(Ticks t, Ticks frame) { return Ticks{floorDiv(t.value + frame.value / 2, frame.value) * frame.value}; }
/// Whole frames in `seconds` (at least `least`).
Frames framesIn(double seconds, Ticks frame, Frames least = 1) {
    return std::max<Frames>(least, std::llround(seconds * static_cast<double>(kTicksPerSecond) / static_cast<double>(frame.value)));
}
/// Footage frames of one step of an eased speed change: it lasts about `seconds` on the timeline at `percent`.
Frames stepFootage(double seconds, double percent, Ticks frame) {
    return std::max<Frames>(1, std::llround(static_cast<double>(framesIn(seconds, frame)) * percent / 100.0));
}
/// The three steps of an eased speed change from `from` to `to` (percent): 1/4, 2/4 and 3/4 of the way.
std::array<double, 3> steps(double from, double to) {
    return {from + (to - from) * 0.25, from + (to - from) * 0.5, from + (to - from) * 0.75};
}
/// Footage lengths a whole number of timeline frames long at `percent`: multiples of 2 at 200 %, of 3 at 300 %...
Frames wholeMultiple(double percent) {
    const double k = percent / 100.0;
    return k >= 1.0 && std::floor(k) == k ? static_cast<Frames>(k) : 1;
}

/// Runs catalog commands on a copy of the project, which the caller commits as one undo step. The first failure
/// stops the rest (its message is error()).
class Scratch {
public:
    Scratch(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks frame)
        : m_session(state), m_engine(m_session), m_sequence(sequence), m_clip(clip), m_frame(frame) {}

    bool run(const char* name, Json params) {
        if (!m_error.empty())
            return false;
        params["sequence"] = m_sequence.value;
        if (!params.contains("clip"))
            params["clip"] = m_clip.value;
        command::CommandRequest request;
        request.command = name;
        request.params = std::move(params);
        const command::CommandResult result = m_engine.execute(request);
        if (!result.ok()) {
            m_error = result.error->message;
            return false;
        }
        return true;
    }
    /// The clip as it is now in the copy.
    std::optional<Clip> clip() const {
        const ProjectSnapshot snap = m_session.snapshot();
        const Sequence* seq = snap ? snap->findSequence(m_sequence) : nullptr;
        const auto loc = seq ? seq->findClip(m_clip) : std::nullopt;
        if (!loc)
            return std::nullopt;
        return loc->clip();
    }
    /// The clip and its remap curve now, or a failure.
    std::optional<std::pair<Clip, Curve>> remap() {
        auto c = clip();
        if (!c || !c->timeRemap()) {
            fail("time remapping is off for the clip");
            return std::nullopt;
        }
        Curve curve = *c->timeRemap();
        return std::pair<Clip, Curve>{std::move(*c), std::move(curve)};
    }
    /// Turns time remapping on (when it is off) and adds a key at each timeline time; their indices.
    std::vector<std::size_t> addKeys(const std::vector<Ticks>& timelineTimes) {
        if (const auto c = clip(); c && !c->isRemapped())
            run("timeRemap.enable", Json{{"enabled", true}});
        for (const Ticks t : timelineTimes)
            run("timeRemap.addKey", Json{{"time", t.value}});
        std::vector<std::size_t> out;
        const auto now = m_error.empty() ? remap() : std::nullopt;
        if (!now)
            return out;
        for (const Ticks t : timelineTimes) {
            const auto index = now->second.keyAt(now->first.curveTimeAt(t));
            if (!index) {
                fail("a speed key is missing");
                return {};
            }
            out.push_back(*index);
        }
        return out;
    }
    /// Each stretch [keys[i], keys[i + 1]) plays at speeds[i]: every moving segment inside it (a key the clip had
    /// there already splits a stretch; holds stay holds). The clip grows or shrinks by the time it gains or loses.
    /// Then the keys go onto the frame grid, so later material (and the clip's end) stays on frames.
    void setSpeeds(const std::vector<std::size_t>& keys, const std::vector<double>& speeds) {
        for (std::size_t i = 0; i < speeds.size() && i + 1 < keys.size(); ++i) {
            for (std::size_t j = keys[i]; j < keys[i + 1]; ++j) {
                const auto now = remap();
                if (!now)
                    return;
                const Curve& curve = now->second;
                if (j + 1 >= curve.keys.size())
                    return fail("a speed key is missing");
                if (curve.keys[j].value == curve.keys[j + 1].value ||
                    remapSegmentAt(curve, curve.keys[j].time).motion == RemapMotion::Hold)
                    continue;
                run("timeRemap.setSpeed",
                    Json{{"segment", static_cast<std::int64_t>(j)}, {"speed", speeds[i]}, {"extendClip", true}});
            }
        }
        snapKeys(keys);
    }
    /// Moves keys[1..] onto the nearest frame, left to right (a ripple move: the later keys and the clip's end move as
    /// much, and only the segment before each key changes speed a little). Never onto the key before; keys past the
    /// clip's end stay.
    void snapKeys(const std::vector<std::size_t>& keys) {
        for (std::size_t i = 1; i < keys.size(); ++i) {
            const auto t = keyTime(keys[i]);
            const auto previous = keyTime(keys[i - 1]);
            const auto c = clip();
            if (!t || !previous || !c)
                return;
            if (*t >= c->end())
                continue;
            const Ticks snapped = std::max(nearestFrame(*t, m_frame), nearestFrame(*previous, m_frame) + m_frame);
            if (snapped != *t)
                run("timeRemap.moveKey", Json{{"index", static_cast<std::int64_t>(keys[i])},
                                              {"time", snapped.value},
                                              {"ripple", true},
                                              {"extendClip", true}});
        }
    }
    /// The timeline time of remap key `index` now.
    std::optional<Ticks> keyTime(std::size_t index) {
        const auto now = remap();
        if (!now || index >= now->second.keys.size())
            return std::nullopt;
        return now->first.start() + (now->second.keys[index].time - now->first.sourceIn);
    }
    void fail(std::string message) {
        if (m_error.empty())
            m_error = std::move(message);
    }
    const std::string& error() const { return m_error; }
    ProjectSnapshot snapshot() const { return m_session.snapshot(); }

    /// Where the playhead goes afterwards (the moment's frame, when the preset moved it).
    std::optional<Ticks> focus;

private:
    EditSession m_session;
    command::CommandEngine m_engine;
    SequenceId m_sequence;
    ClipId m_clip;
    Ticks m_frame;
    std::string m_error;
};

/// Commits what `edit` does on a copy of the project as one undo step named `label`; false with the reason.
bool commitSteps(ProjectController* controller, const char* label, ClipId clip, Ticks frame,
                 const std::function<void(Scratch&)>& edit, QString* failure) {
    std::string stepError;
    std::optional<Ticks> focus;
    const bool ok = controller->applyEditFn(label, [&](const ProjectState& state) -> edit::EditOutcome {
        Scratch scratch(state, controller->currentSequenceId(), clip, frame);
        edit(scratch);
        if (!scratch.error().empty()) {
            stepError = scratch.error();
            return std::unexpected(Error(ErrorCode::InvalidArgument, scratch.error()));
        }
        focus = scratch.focus;
        const ProjectSnapshot after = scratch.snapshot();
        edit::EditResult result;
        result.changes = diff(state, *after);
        result.state = *after;
        return result;
    });
    if (!ok) {
        if (failure)
            *failure = stepError.empty() ? controller->lastError() : QString::fromStdString(stepError);
        return false;
    }
    if (focus)
        controller->setPlayhead(*focus);
    return true;
}

} // namespace

MomentsBridge::MomentsBridge(QObject* parent) : QObject(parent), m_deadTimeJob(std::make_unique<BridgeJob>()) {
    // 0.4: instant replay's remembered options.
    const QSettings settings;
    m_replaySpeed = settings.value(QStringLiteral("moments/replaySpeed"), 0.5).toDouble() < 0.375 ? 0.25 : 0.5;
    m_replaySound = settings.value(QStringLiteral("moments/replaySound"), false).toBool();
    m_replayLabel = settings.value(QStringLiteral("moments/replayLabel"), true).toBool();
}

void MomentsBridge::setReplaySpeed(double speed) {
    const double clamped = speed < 0.375 ? 0.25 : 0.5; // 25 % or 50 %, the nearer
    if (clamped == m_replaySpeed)
        return;
    m_replaySpeed = clamped;
    QSettings().setValue(QStringLiteral("moments/replaySpeed"), clamped);
    emit replayOptionsChanged();
}

void MomentsBridge::setReplaySound(bool on) {
    if (on == m_replaySound)
        return;
    m_replaySound = on;
    QSettings().setValue(QStringLiteral("moments/replaySound"), on);
    emit replayOptionsChanged();
}

void MomentsBridge::setReplayLabel(bool on) {
    if (on == m_replayLabel)
        return;
    m_replayLabel = on;
    QSettings().setValue(QStringLiteral("moments/replayLabel"), on);
    emit replayOptionsChanged();
}

MomentsBridge::~MomentsBridge() = default;

MomentsBridge* MomentsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new MomentsBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void MomentsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &MomentsBridge::targetChanged);
        connect(controller, &ProjectController::playheadChanged, this, &MomentsBridge::targetChanged);
        connect(controller, &ProjectController::selectionChanged, this, &MomentsBridge::targetChanged);
        connect(controller, &ProjectController::sequenceChanged, this, &MomentsBridge::targetChanged);
    }
    emit targetChanged();
}

void MomentsBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

QString MomentsBridge::finish(bool ok, const QString& failure) {
    const QString error = ok ? QString() : failure;
    setMessage(error);
    return error;
}

std::optional<MomentsBridge::Target> MomentsBridge::resolve(const QString& clipKeyText, bool speed, QString* why) const {
    const auto refuse = [why](const QString& reason) -> std::optional<Target> {
        if (why)
            *why = reason;
        return std::nullopt;
    };
    if (!m_controller)
        return refuse(tr("No project is open"));
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return refuse(tr("No sequence is open"));
    const Ticks playhead = m_controller->playhead();
    std::optional<ClipId> chosen;
    if (!clipKeyText.isEmpty()) {
        const auto id = parseClipKey(clipKeyText);
        if (!id || !seq->findClip(*id))
            return refuse(tr("That clip is not in this sequence"));
        chosen = *id;
    } else {
        const ClipId primary = m_controller->primaryClip();
        if (primary.isValid()) {
            if (const auto loc = seq->findClip(primary); loc && loc->clip().timelineRange.contains(playhead))
                chosen = primary;
        }
        // The topmost enabled video clip under the playhead.
        for (auto it = seq->videoTracks.rbegin(); !chosen && it != seq->videoTracks.rend(); ++it) {
            const Track& track = **it;
            if (track.locked)
                continue;
            if (const auto index = track.clipIndexAt(playhead); index && track.clips[*index].enabled)
                chosen = track.clips[*index].id;
        }
        if (!chosen && primary.isValid() && seq->findClip(primary))
            chosen = primary;
        if (!chosen)
            return refuse(tr("Move the playhead over a clip"));
    }
    auto loc = seq->findClip(*chosen);
    // An audio clip hands over to its linked video: the picture is what the moment is about.
    if (loc->track->kind == TrackKind::Audio) {
        const Clip& audio = loc->clip();
        std::optional<ClipId> video;
        if (audio.linkGroup.isValid()) {
            for (const TrackPtr& track : seq->videoTracks) {
                for (const Clip& c : track->clips) {
                    if (c.linkGroup == audio.linkGroup)
                        video = c.id;
                }
            }
        }
        if (!video)
            return refuse(tr("Moments work on video clips"));
        loc = seq->findClip(*video);
    }
    const Clip& clip = loc->clip();
    if (loc->track->locked)
        return refuse(tr("The clip's track is locked"));
    if (speed) {
        if (clip.isFreeze())
            return refuse(tr("A freeze frame has no motion to retime"));
        if (!clip.isNested()) {
            const ProjectSnapshot snap = m_controller->snapshot();
            const MediaItem* media = snap ? snap->findMedia(clip.media) : nullptr;
            if (!media || media->isGenerator())
                return refuse(tr("Speed moments need footage, not a title or a graphic"));
        }
    }
    const Ticks at = clip.timelineRange.contains(playhead) ? playhead : clip.start() + Ticks{clip.duration().value / 2};
    return Target{clip.id, std::clamp(snapToFrame(at, seq->frameRate), clip.start(), clip.end() - Ticks{1})};
}

QVariantMap MomentsBridge::targetFor(const QString& clipKeyText) const {
    QString why;
    const auto picked = resolve(clipKeyText, false, &why);
    if (!picked)
        return {{QStringLiteral("available"), false}, {QStringLiteral("reason"), why}};
    QString speedWhy;
    const bool speed = resolve(clipKey(picked->clip), true, &speedWhy).has_value();
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = seq->findClip(picked->clip);
    return {{QStringLiteral("available"), true},
            {QStringLiteral("clip"), clipKey(picked->clip)},
            {QStringLiteral("name"), QString::fromStdString(loc->clip().name())},
            {QStringLiteral("video"), loc->track->kind == TrackKind::Video},
            {QStringLiteral("speed"), speed},
            {QStringLiteral("speedReason"), speedWhy},
            {QStringLiteral("atText"), m_controller->formatTimecode(picked->at)}};
}

// ---- Speed ------------------------------------------------------------------------------------------------------
// Keys go on frames of the current timing (a frame in from each end of the clip); lengths are footage frames.

QString MomentsBridge::slowMo(double percent, double seconds, const QString& clipKeyText) {
    QString why;
    const auto target = resolve(clipKeyText, true, &why);
    if (!target)
        return finish(false, why);
    if (!(percent > 0.0 && percent < 100.0))
        return finish(false, tr("Slow motion is slower than 100 %"));
    const SequencePtr seq = m_controller->currentSequence();
    const Clip clip = seq->findClip(target->clip)->clip();
    const Ticks frame = framesToTicks(1, seq->frameRate);
    const Frames first = ticksToFrames(clip.start() + frame - Ticks{1}, seq->frameRate) + 1;
    const Frames last = ticksToFrames(clip.end(), seq->frameRate) - 1;
    const Frames room = last - first;
    // Down in three steps of about 0.16 s each, the slow stretch, and back up the same way.
    const std::array<double, 3> down = steps(100.0, percent);
    std::array<Frames, 3> ramp{};
    for (std::size_t j = 0; j < 3; ++j)
        ramp[j] = stepFootage(0.16, down[j], frame);
    Frames rampLength = ramp[0] + ramp[1] + ramp[2];
    const Frames wanted = framesIn(std::isfinite(seconds) ? std::clamp(seconds, 0.04, 60.0) : 1.0, frame, 2);
    Frames slow = wanted;
    if (2 * rampLength + slow > room) {
        // A short clip: the slow stretch gives way first, then the eased steps.
        slow = room - 2 * rampLength;
        if (slow < std::max<Frames>(2, framesIn(0.2, frame))) {
            rampLength = 0;
            slow = std::min(wanted, room);
        }
    }
    if (slow < 2)
        return finish(false, tr("The clip is too short for slow motion"));
    // Centred on the moment (the slow stretch's footage centred on its frame), kept inside the clip.
    const Frames span = 2 * rampLength + slow;
    const Frames at = ticksToFrames(target->at, seq->frameRate);
    const Frames a = std::clamp(at - slow / 2 - rampLength, first, std::max(first, last - span));
    std::vector<Frames> keyFrames;
    std::vector<double> speeds;
    std::size_t slowIndex = 0; // the stretch at `percent`
    if (rampLength > 0) {
        const Frames b = a + rampLength;
        const Frames c = b + slow;
        keyFrames = {a, a + ramp[0], a + ramp[0] + ramp[1], b, c, c + ramp[2], c + ramp[2] + ramp[1], c + rampLength};
        speeds = {down[0], down[1], down[2], percent, down[2], down[1], down[0]};
        slowIndex = 3;
    } else {
        keyFrames = {a, a + slow};
        speeds = {percent};
    }
    std::vector<Ticks> keys;
    for (const Frames k : keyFrames)
        keys.push_back(framesToTicks(k, seq->frameRate));
    QString failure;
    const bool ok = commitSteps(m_controller, LABEL("Slow Motion"), clip.id, frame,
                                [&](Scratch& s) {
                                    const std::vector<std::size_t> indices = s.addKeys(keys);
                                    s.setSpeeds(indices, speeds);
                                    // The playhead stays on the moment's frame: the middle of the slow stretch.
                                    if (indices.size() > slowIndex + 1) {
                                        const auto from = s.keyTime(indices[slowIndex]);
                                        const auto to = s.keyTime(indices[slowIndex + 1]);
                                        if (from && to && *to > *from)
                                            s.focus = snapToFrame(*from + Ticks{(*to - *from).value / 2}, seq->frameRate);
                                    }
                                },
                                &failure);
    return finish(ok, failure);
}

QString MomentsBridge::rampIn(double percent, double seconds, const QString& clipKeyText) {
    QString why;
    const auto target = resolve(clipKeyText, true, &why);
    if (!target)
        return finish(false, why);
    if (!(percent > 100.0 && percent <= 1000.0))
        return finish(false, tr("A ramp in starts faster than 100 %"));
    const SequencePtr seq = m_controller->currentSequence();
    const Clip clip = seq->findClip(target->clip)->clip();
    const Ticks frame = framesToTicks(1, seq->frameRate);
    const Frames first = ticksToFrames(clip.start() + frame - Ticks{1}, seq->frameRate) + 1;
    const Frames last = ticksToFrames(clip.end(), seq->frameRate) - 1;
    // It lands on 100 % at the moment, so the moment needs a little room before it.
    const Frames at = std::max(ticksToFrames(target->at, seq->frameRate), first + 2);
    if (at > last)
        return finish(false, tr("The clip is too short for a ramp in"));
    const Frames room = at - first;
    // The fast stretch, then down to 100 % in three steps of about 0.08 s each.
    const std::array<double, 3> down = steps(percent, 100.0);
    std::array<Frames, 3> ramp{};
    for (std::size_t j = 0; j < 3; ++j)
        ramp[j] = stepFootage(0.08, down[j], frame);
    Frames rampLength = ramp[0] + ramp[1] + ramp[2];
    const Frames multiple = wholeMultiple(percent);
    const Frames wanted = framesIn(std::isfinite(seconds) ? std::clamp(seconds, 0.04, 60.0) : 1.0, frame, 2);
    Frames fast = std::max(multiple, wanted / multiple * multiple);
    if (fast + rampLength > room) {
        fast = (room - rampLength) / multiple * multiple;
        if (fast < std::max<Frames>(2, multiple)) {
            rampLength = 0;
            fast = room / multiple * multiple;
        }
    }
    if (fast < 2)
        return finish(false, tr("The clip is too short for a ramp in"));
    std::vector<Frames> keyFrames;
    std::vector<double> speeds;
    if (rampLength > 0) {
        const Frames b = at - rampLength;
        keyFrames = {b - fast, b, b + ramp[0], b + ramp[0] + ramp[1], at};
        speeds = {percent, down[0], down[1], down[2]};
    } else {
        keyFrames = {at - fast, at};
        speeds = {percent};
    }
    std::vector<Ticks> keys;
    for (const Frames k : keyFrames)
        keys.push_back(framesToTicks(k, seq->frameRate));
    QString failure;
    const bool ok = commitSteps(m_controller, LABEL("Speed Ramp In"), clip.id, frame,
                                [&](Scratch& s) {
                                    const std::vector<std::size_t> indices = s.addKeys(keys);
                                    s.setSpeeds(indices, speeds);
                                    // The playhead stays on the moment's frame, where the ramp lands on 100 %.
                                    if (!indices.empty())
                                        s.focus = s.keyTime(indices.back());
                                },
                                &failure);
    return finish(ok, failure);
}

QString MomentsBridge::fastForward(double percent, const QString& clipKeyText) {
    QString why;
    const auto target = resolve(clipKeyText, true, &why);
    if (!target)
        return finish(false, why);
    if (!(percent > 100.0 && percent <= 1000.0))
        return finish(false, tr("Fast forward is faster than 100 %"));
    const SequencePtr seq = m_controller->currentSequence();
    const Clip clip = seq->findClip(target->clip)->clip();
    const Ticks frame = framesToTicks(1, seq->frameRate);
    const Frames first = ticksToFrames(clip.start() + frame - Ticks{1}, seq->frameRate) + 1;
    const Frames last = ticksToFrames(clip.end(), seq->frameRate) - 1;
    // In to Out when both marks are inside the clip, else 3 s from the moment; a frame in from each end.
    Frames from = ticksToFrames(target->at, seq->frameRate);
    Frames to = from + framesIn(3.0, frame);
    const auto in = m_controller->markIn();
    const auto out = m_controller->markOut();
    if (in && out && *in < *out && *in >= clip.start() && *out <= clip.end()) {
        from = ticksToFrames(*in + frame - Ticks{1}, seq->frameRate);
        to = ticksToFrames(*out, seq->frameRate);
    }
    from = std::max(from, first);
    to = std::min(to, last);
    const Frames multiple = wholeMultiple(percent);
    const Frames length = (to - from) / multiple * multiple;
    if (length < std::max<Frames>(2, multiple))
        return finish(false, tr("The clip is too short to fast forward"));
    const std::vector<Ticks> keys{framesToTicks(from, seq->frameRate), framesToTicks(from + length, seq->frameRate)};
    QString failure;
    const bool ok = commitSteps(m_controller, LABEL("Fast Forward"), clip.id, frame,
                                [&](Scratch& s) { s.setSpeeds(s.addKeys(keys), {percent}); }, &failure);
    return finish(ok, failure);
}

QString MomentsBridge::freezeReplay(double freezeSeconds, double replaySeconds, double percent, const QString& clipKeyText) {
    QString why;
    const auto target = resolve(clipKeyText, true, &why);
    if (!target)
        return finish(false, why);
    if (!(percent > 0.0 && percent <= 100.0))
        return finish(false, tr("The replay plays at 100 % or slower"));
    const SequencePtr seq = m_controller->currentSequence();
    const Clip clip = seq->findClip(target->clip)->clip();
    const Ticks frame = framesToTicks(1, seq->frameRate);
    const Ticks first = framesToTicks(ticksToFrames(clip.start() + frame - Ticks{1}, seq->frameRate) + 1, seq->frameRate);
    const Ticks last = framesToTicks(ticksToFrames(clip.end(), seq->frameRate) - 1, seq->frameRate);
    if (last < first)
        return finish(false, tr("The clip is too short to freeze and replay"));
    const Ticks at = std::clamp(target->at, first, last);
    const Ticks freeze = times(frame, framesIn(std::isfinite(freezeSeconds) ? std::clamp(freezeSeconds, 0.04, 10.0) : 0.5, frame));
    const Ticks wanted =
        times(frame, framesIn(std::isfinite(replaySeconds) ? std::clamp(replaySeconds, 0.08, 30.0) : 2.0, frame, 2));
    QString failure;
    const bool ok = commitSteps(
        m_controller, LABEL("Freeze and Replay"), clip.id, frame,
        [&](Scratch& s) {
            if (const auto c = s.clip(); c && !c->isRemapped())
                s.run("timeRemap.enable", Json{{"enabled", true}});
            // 1. The frame at the moment holds (a hold segment; the rest plays later and the clip grows by it).
            s.run("timeRemap.addHold", Json{{"time", at.value}, {"duration", freeze.value}, {"extendClip", true}});
            auto now = s.remap();
            if (!now)
                return;
            const Ticks curveAt = now->first.curveTimeAt(at);
            const auto held = now->second.keyAt(curveAt);
            const auto resume = now->second.keyAt(curveAt + freeze);
            if (!held || !resume)
                return s.fail("the hold did not make its keys");
            if (*resume + 1 >= now->second.keys.size())
                return s.fail("there is no footage after the moment to replay into");
            const Ticks shown{static_cast<std::int64_t>(now->second.keys[*held].value)};
            // The replay starts this far back in the footage (whole frames, not before its first frame).
            const Ticks replay = times(frame, std::min(wanted.value, shown.value) / frame.value);
            if (replay < times(frame, 2))
                return s.fail("there is not enough footage before the moment to replay");
            // 2. The hold is a step, so the jump back to the replay's first frame shows nothing in between.
            s.run("timeRemap.setInterpolation",
                  Json{{"keys", Json::array({static_cast<std::int64_t>(*held)})}, {"interpolation", "hold"}});
            s.run("timeRemap.setKey",
                  Json{{"index", static_cast<std::int64_t>(*resume)}, {"source", (shown - replay).value}});
            // 3. From there at 100 % (the clip grows by the replay), with a key where it is back on the moment's
            //    frame: the replay ends there and the clip carries on at 100 %, audible again.
            s.setSpeeds({*resume, *resume + 1}, {100.0});
            const Ticks caughtCurve = curveAt + freeze + replay;
            now = s.remap();
            if (!now)
                return;
            s.run("timeRemap.addKey", Json{{"time", (now->first.start() + (caughtCurve - now->first.sourceIn)).value}});
            now = s.remap();
            if (!now)
                return;
            const auto caught = now->second.keyAt(caughtCurve);
            if (!caught)
                return s.fail("the replay's end key is missing");
            if (static_cast<std::int64_t>(now->second.keys[*caught].value) != shown.value)
                s.run("timeRemap.setKey", Json{{"index", static_cast<std::int64_t>(*caught)}, {"source", shown.value}});
            // 4. The replay at its speed.
            if (percent < 100.0)
                s.setSpeeds({*resume, *caught}, {percent});
        },
        &failure);
    return finish(ok, failure);
}

// ---- Zoom -------------------------------------------------------------------------------------------------------

QString MomentsBridge::punchIn(double percent, bool hold, const QString& clipKeyText) {
    QString why;
    const auto target = resolve(clipKeyText, false, &why);
    if (!target)
        return finish(false, why);
    if (!(percent > 100.0 && percent <= 1000.0))
        return finish(false, tr("A punch-in zooms in past 100 %"));
    const SequencePtr seq = m_controller->currentSequence();
    const Clip clip = seq->findClip(target->clip)->clip();
    const Ticks frame = framesToTicks(1, seq->frameRate);
    const Ticks hi = clip.end() - frame;
    // In over about 0.12 s, held about 1 s, out over about 0.2 s; the hold gives way on a short clip.
    const Ticks in = times(frame, framesIn(0.12, frame));
    const Ticks t0 = std::min(target->at, snapToFrame(hi - in, seq->frameRate));
    if (t0 < clip.start())
        return finish(false, tr("The clip is too short for a punch-in"));
    // Relative to the scale at the moment (a clip already at 50 % punches in to 65 %).
    const double base =
        ProjectController::parameterValue(clip, edit::ParamRef::of(ParamId::Scale)).valueAt(clip.curveTimeAt(t0));
    const double zoomed = std::clamp(base * percent / 100.0, 0.0, 100.0);
    std::vector<Ticks> keys{t0, t0 + in};
    bool back = !hold;
    if (back) {
        const Ticks out = times(frame, framesIn(0.2, frame));
        Ticks t2 = keys[1] + times(frame, framesIn(1.0, frame));
        if (t2 + out > hi)
            t2 = snapToFrame(hi - out, seq->frameRate);
        if (t2 > keys[1]) {
            keys.push_back(t2);
            keys.push_back(t2 + out);
        } else {
            back = false; // no room to come back out: it stays in
        }
    }
    QString failure;
    const bool ok = commitSteps(
        m_controller, back ? LABEL("Punch In") : LABEL("Punch In and Hold"), clip.id, frame,
        [&](Scratch& s) {
            // In fast and settle (linear out of the first key, a flat arrival); back out softly at both ends.
            static constexpr std::array<const char*, 4> kShapes{"linear", "easeIn", "easeOut", "easeIn"};
            const std::array<double, 4> values{base, zoomed, zoomed, base};
            for (std::size_t i = 0; i < keys.size(); ++i)
                s.run("clip.addKeyframe", Json{{"param", "motion.scale"},
                                               {"time", keys[i].value},
                                               {"value", values[i]},
                                               {"interpolation", kShapes[i]}});
        },
        &failure);
    return finish(ok, failure);
}

QString MomentsBridge::shake(double pixels, const QString& clipKeyText) {
    QString why;
    const auto target = resolve(clipKeyText, false, &why);
    if (!target)
        return finish(false, why);
    if (!(pixels > 0.0 && pixels <= 500.0))
        return finish(false, tr("A shake moves a few pixels"));
    const SequencePtr seq = m_controller->currentSequence();
    const Clip clip = seq->findClip(target->clip)->clip();
    const Ticks frame = framesToTicks(1, seq->frameRate);
    const Ticks hi = clip.end() - frame;
    // A decaying wiggle in 8 steps of about 45 ms (a frame at least), ending where it started.
    static constexpr std::array<double, 9> kX{0.0, 1.0, -0.85, 0.7, -0.55, 0.4, -0.25, 0.1, 0.0};
    static constexpr std::array<double, 9> kY{0.0, -0.6, 0.9, -0.5, 0.7, -0.35, 0.3, -0.1, 0.0};
    const Ticks step = times(frame, framesIn(0.045, frame));
    const Ticks length = times(step, static_cast<std::int64_t>(kX.size() - 1));
    const Ticks t0 = std::max(clip.start(), std::min(target->at, snapToFrame(hi - length, seq->frameRate)));
    if (t0 + length > hi)
        return finish(false, tr("The clip is too short for a shake"));
    const Ticks curveAt = clip.curveTimeAt(t0);
    const double bx = ProjectController::parameterValue(clip, edit::ParamRef::of(ParamId::PositionX)).valueAt(curveAt);
    const double by = ProjectController::parameterValue(clip, edit::ParamRef::of(ParamId::PositionY)).valueAt(curveAt);
    QString failure;
    const bool ok = commitSteps(m_controller, LABEL("Shake"), clip.id, frame,
                                [&](Scratch& s) {
                                    for (std::size_t k = 0; k < kX.size(); ++k) {
                                        const Ticks t = t0 + times(step, static_cast<std::int64_t>(k));
                                        s.run("clip.addKeyframe", Json{{"param", "motion.position.x"},
                                                                       {"time", t.value},
                                                                       {"value", bx + pixels * kX[k]}});
                                        s.run("clip.addKeyframe", Json{{"param", "motion.position.y"},
                                                                       {"time", t.value},
                                                                       {"value", by + pixels * kY[k]}});
                                    }
                                },
                                &failure);
    return finish(ok, failure);
}

// ---- By name ------------------------------------------------------------------------------------------------------

QString MomentsBridge::apply(const QString& preset, const QVariantMap& options) {
    const QString clip = options.value(QStringLiteral("clip")).toString();
    const double seconds = options.value(QStringLiteral("seconds"), 1.0).toDouble();
    if (preset == QLatin1String("slowMo25"))
        return slowMo(25.0, seconds, clip);
    if (preset == QLatin1String("slowMo50"))
        return slowMo(50.0, seconds, clip);
    if (preset == QLatin1String("rampIn"))
        return rampIn(300.0, 1.0, clip);
    if (preset == QLatin1String("freezeReplay"))
        return freezeReplay(0.5, 2.0, 50.0, clip);
    if (preset == QLatin1String("fastForward200"))
        return fastForward(200.0, clip);
    if (preset == QLatin1String("fastForward300"))
        return fastForward(300.0, clip);
    if (preset == QLatin1String("fastForward400"))
        return fastForward(400.0, clip);
    if (preset == QLatin1String("punchIn"))
        return punchIn(130.0, false, clip);
    if (preset == QLatin1String("punchInHold"))
        return punchIn(130.0, true, clip);
    if (preset == QLatin1String("shake"))
        return shake(14.0, clip);
    if (preset == QLatin1String("instantReplay"))
        return instantReplay(clip, options); // 0.4: speed, includeAudio, label override the remembered options
    if (preset == QLatin1String("shortsVersion"))
        return makeVersion(QStringLiteral("shorts"));
    if (preset == QLatin1String("squareVersion"))
        return makeVersion(QStringLiteral("square"));
    return finish(false, tr("Unknown moment"));
}

// ---- C.16's gaming edits (catalog commands; shown when the catalog has them) ----

std::shared_ptr<command::CommandServices> MomentsBridge::services() const {
    std::vector<std::shared_ptr<command::CommandServices>> parts;
    std::filesystem::path projectFile;
    if (QQmlEngine* engine = qmlEngine(this)) {
        if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"))
            projectFile = std::filesystem::path(file->filePath().toStdWString());
        if (auto* graphics = engine->singletonInstance<GraphicsBridge*>("Wabcut", "Graphics"))
            parts.push_back(graphics->graphicsServices());
    }
    parts.push_back(appGamingServices(projectFile));
    return std::make_shared<command::ServiceBundle>(std::move(parts));
}

const command::CommandRegistry& MomentsBridge::registry() const {
    return m_registry ? *m_registry : ai::edit::editingCommands();
}

void MomentsBridge::setRegistry(const command::CommandRegistry* registry) {
    m_registry = registry;
    emit catalogChanged();
}

QVariantMap MomentsBridge::gamingEdits() const {
    return {{QStringLiteral("replay"), registry().knows("clip.instantReplay")},
            {QStringLiteral("version"), registry().knows("sequence.makeVersion")},
            {QStringLiteral("deadTime"), registry().knows("sequence.removeDeadTime")}};
}

namespace {

/// `name` on the live session: one undo step (the engine commits it), or the command's refusal.
command::CommandResult runLive(ProjectController* controller, const command::CommandRegistry& registry,
                               const std::string& name, command::Json params,
                               std::shared_ptr<command::CommandServices> services) {
    command::EnginePolicy policy;
    policy.services = std::move(services);
    command::CommandEngine engine(controller->session(), registry, policy);
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    return engine.execute(request);
}

} // namespace

QString MomentsBridge::instantReplay(const QString& clipKeyText, const QVariantMap& options) {
    if (!registry().knows("clip.instantReplay"))
        return finish(false, tr("Instant replay arrives with the next engine update"));
    QString why;
    const auto target = resolve(clipKeyText, true, &why);
    if (!target)
        return finish(false, why);
    const SequencePtr seq = m_controller->currentSequence();
    const Clip clip = seq->findClip(target->clip)->clip();
    // 0.4: the remembered options, unless given.
    const double speed = options.contains(QStringLiteral("speed"))
                             ? std::clamp(options.value(QStringLiteral("speed")).toDouble(), 0.05, 1.0)
                             : m_replaySpeed;
    command::Json params{{"sequence", m_controller->currentSequenceId().value},
                         {"speed", std::isfinite(speed) ? speed : 0.5},
                         {"ramp", true},
                         {"flash", true},
                         {"label", options.value(QStringLiteral("label"), m_replayLabel).toBool()},
                         {"includeAudio", options.value(QStringLiteral("includeAudio"), m_replaySound).toBool()}};
    // 0.4, what is replayed: 1. In to Out when both marks are set and the range overlaps the clip (the command's range
    // mode: every clip of media in it, linked audio and facecam too); 2. on a clip longer than 20 s, the 5 s before the
    // moment (not before the clip's start; under 1 s of that: the 5 s after it); 3. the whole clip (0.3).
    const auto in = m_controller->markIn();
    const auto out = m_controller->markOut();
    const Ticks fiveSeconds = Ticks::fromSeconds(5);
    if (in && out && *in < *out && *in < clip.end() && *out > clip.start()) {
        params["in"] = in->value;
        params["out"] = out->value;
    } else if (clip.duration() > Ticks::fromSeconds(20)) {
        Ticks from = std::max(clip.start(), target->at - fiveSeconds);
        Ticks to = target->at;
        if (to - from < Ticks::fromSeconds(1)) {
            from = target->at;
            to = std::min(clip.end(), target->at + fiveSeconds);
        }
        params["in"] = from.value;
        params["out"] = to.value;
    } else {
        params["clip"] = target->clip.value;
    }
    const command::CommandResult result =
        runLive(m_controller, registry(), "clip.instantReplay", std::move(params), services());
    if (!result.ok())
        return finish(false, QString::fromStdString(result.error->message));
    return finish(true, {});
}

QString MomentsBridge::makeVersion(const QString& format) {
    if (!registry().knows("sequence.makeVersion"))
        return finish(false, tr("Shorts versions arrive with the next engine update"));
    if (!m_controller)
        return finish(false, tr("No project is open"));
    const command::CommandResult result =
        runLive(m_controller, registry(), "sequence.makeVersion",
                command::Json{{"sequence", m_controller->currentSequenceId().value},
                              {"format", format == QLatin1String("square") ? "square" : "shorts"},
                              {"captions", true}},
                services());
    if (!result.ok())
        return finish(false, QString::fromStdString(result.error->message));
    // Open the new sequence.
    const std::uint64_t created = result.data.is_object() ? result.data.value("sequence", std::uint64_t{0}) : 0;
    const SequenceId id{created ? created : (result.createdIds.empty() ? 0 : result.createdIds.front())};
    const ProjectSnapshot snapshot = m_controller->snapshot();
    for (std::size_t i = 0; i < snapshot->sequences.size(); ++i) {
        if (snapshot->sequences[i]->id == id) {
            m_controller->setCurrentSequenceIndex(static_cast<int>(i));
            break;
        }
    }
    return finish(true, {});
}

void MomentsBridge::findDeadTime() {
    if (m_deadTimeJob->running())
        return;
    if (!m_controller || !registry().knows("sequence.removeDeadTime")) {
        m_deadTime = {{QStringLiteral("error"), tr("Remove dead time arrives with the next engine update")}};
        emit deadTimeChanged();
        return;
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const std::uint64_t sequence = m_controller->currentSequenceId().value;
    const command::CommandRegistry* catalog = &registry();
    const std::shared_ptr<command::CommandServices> hostServices = services();
    m_deadTime = {{QStringLiteral("busy"), true}, {QStringLiteral("text"), tr("Measuring the gameplay…")}};
    m_deadTimeRanges.clear();
    emit deadTimeChanged();
    QPointer<MomentsBridge> self(this);
    m_deadTimeJob->start(
        this, m_synchronous,
        [base, sequence, catalog, hostServices, self](const std::atomic<bool>&) -> BridgeJob::Finish {
            // On the worker: the preview on a copy (it decodes and measures; nothing changes).
            EditSession scratch(*base);
            command::EnginePolicy policy;
            policy.services = hostServices;
            command::CommandEngine engine(scratch, *catalog, policy);
            command::CommandRequest request;
            request.command = "sequence.removeDeadTime";
            request.params = command::Json{{"sequence", sequence}};
            command::CommandResult result = engine.execute(request);
            return [self, sequence, result = std::move(result)] {
                if (!self)
                    return;
                if (!result.ok()) {
                    self->m_deadTime = {{QStringLiteral("error"), QString::fromStdString(result.error->message)}};
                    emit self->deadTimeChanged();
                    return;
                }
                command::Json ranges = command::Json::array();
                for (const command::Json& r : result.data.value("ranges", command::Json::array()))
                    ranges.push_back(command::Json{{"start", r.value("startTicks", std::int64_t{0})},
                                                   {"end", r.value("endTicks", std::int64_t{0})}});
                const double seconds = result.data.value("removeSeconds", 0.0);
                const int count = static_cast<int>(ranges.size());
                const int whole = static_cast<int>(std::lround(seconds));
                self->m_deadTimeRanges = ranges.dump();
                self->m_deadTimeSequence = sequence;
                self->m_deadTime = {
                    {QStringLiteral("found"), count > 0},
                    {QStringLiteral("count"), count},
                    {QStringLiteral("seconds"), seconds},
                    {QStringLiteral("text"),
                     count > 0 ? tr("Dead time: %1 stretches, %2:%3 in all").arg(count).arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'))
                               : tr("No dead time found")}};
                emit self->deadTimeChanged();
            };
        },
        [self](const QString& message) -> BridgeJob::Finish {
            return [self, message] {
                if (!self)
                    return;
                self->m_deadTime = {{QStringLiteral("error"), message}};
                emit self->deadTimeChanged();
            };
        });
}

QString MomentsBridge::removeDeadTime() {
    if (!m_controller || m_deadTimeRanges.empty() || !m_deadTime.value(QStringLiteral("found")).toBool())
        return finish(false, tr("Find the dead time first"));
    if (m_controller->currentSequenceId().value != m_deadTimeSequence)
        return finish(false, tr("The dead time was found on another sequence: find it again"));
    const command::CommandResult result =
        runLive(m_controller, registry(), "sequence.removeDeadTime",
                command::Json{{"sequence", m_deadTimeSequence},
                              {"apply", true},
                              {"ranges", command::Json::parse(m_deadTimeRanges)}},
                services());
    cancelDeadTime();
    if (!result.ok())
        return finish(false, QString::fromStdString(result.error->message));
    return finish(true, {});
}

void MomentsBridge::cancelDeadTime() {
    if (m_deadTimeJob->running())
        m_deadTimeJob->cancel();
    m_deadTimeRanges.clear();
    m_deadTime = {};
    emit deadTimeChanged();
}

bool MomentsBridge::runCommand(const QString& commandId) {
    if (commandId == QLatin1String("moments.slowMo")) {
        slowMo();
        return true;
    }
    if (commandId == QLatin1String("moments.punchIn")) {
        punchIn();
        return true;
    }
    return false;
}

} // namespace wabcut::app
