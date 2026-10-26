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

#include <wabcut/command/ExecutionContext.h>

#include <QCoreApplication>
#include <QLocale>
#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
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

// ---- 0.5: Remove dead time's review ----

/// A Find dead time option, clamped to what sequence.removeDeadTime takes (the UI's ranges): sensitivity 0..1,
/// minSeconds 0.5..600, handleSeconds 0..5.
double deadTimeOptionValue(std::string_view name, const QVariant& value) {
    const double v = value.toDouble();
    if (name == "sensitivity")
        return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.5;
    if (name == "minSeconds")
        return std::isfinite(v) ? std::clamp(v, 0.5, 600.0) : 3.0;
    return std::isfinite(v) ? std::clamp(v, 0.0, 5.0) : 0.3; // handleSeconds
}

/// The sequence's clips (track, id, place): a found stretch only means something while it is the same.
std::uint64_t layoutOf(const Sequence& seq) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    for (const std::vector<TrackPtr>* tracks : {&seq.videoTracks, &seq.audioTracks}) {
        for (const TrackPtr& track : *tracks) {
            mix(track->id.value);
            for (const Clip& c : track->clips) {
                mix(c.id.value);
                mix(static_cast<std::uint64_t>(c.start().value));
                mix(static_cast<std::uint64_t>(c.duration().value));
            }
        }
    }
    return h;
}

/// The frame a sequence shows at timeline time `t` (its top enabled video clip of media with a picture), as an
/// image://wabcut-frames URL (decoded off the GUI thread and cached by the provider), or "".
QString frameUrlAt(const ProjectState& state, const Sequence& seq, Ticks t) {
    for (auto it = seq.videoTracks.rbegin(); it != seq.videoTracks.rend(); ++it) {
        if (!(*it)->enabled)
            continue;
        for (const Clip& clip : (*it)->clips) {
            if (!clip.enabled || t < clip.start() || t >= clip.end())
                continue;
            const MediaItem* media = state.findMedia(clip.media);
            if (media && !media->isGenerator() && !media->info.video.empty())
                return QStringLiteral("image://wabcut-frames/%1/%2").arg(mediaKey(clip.media)).arg(clip.sourceTimeAt(t).value);
        }
    }
    return {};
}

/// "m:ss" of a length.
QString clockText(double seconds) {
    const auto whole = static_cast<int>(std::lround(std::max(0.0, seconds)));
    return QStringLiteral("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
}

/// Find dead time on the worker: stops at the next checkpoint once cancelled, reports "measuring activity (NN%)".
class DeadTimeControl final : public command::ExecutionControl {
public:
    DeadTimeControl(const std::atomic<bool>& cancelled, std::function<void(double)> report)
        : m_cancelled(cancelled), m_report(std::move(report)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        if (m_report && message.size() > 3 && message.ends_with("%)")) {
            if (const auto open = message.rfind('('); open != std::string_view::npos) {
                const std::string_view digits = message.substr(open + 1, message.size() - open - 3);
                int percent = 0;
                if (std::from_chars(digits.data(), digits.data() + digits.size(), percent).ec == std::errc{})
                    m_report(std::clamp(percent, 0, 100) / 100.0);
            }
        }
        if (m_cancelled.load()) {
            command::CommandError error;
            error.code = command::ErrorCode::Cancelled;
            error.message = "cancelled";
            return error;
        }
        return std::nullopt;
    }

private:
    const std::atomic<bool>& m_cancelled;
    std::function<void(double)> m_report;
};

} // namespace

MomentsBridge::MomentsBridge(QObject* parent) : QObject(parent), m_deadTimeJob(std::make_unique<BridgeJob>()) {
    // 0.4: instant replay's remembered options.
    const QSettings settings;
    m_replaySpeed = settings.value(QStringLiteral("moments/replaySpeed"), 0.5).toDouble() < 0.375 ? 0.25 : 0.5;
    m_replaySound = settings.value(QStringLiteral("moments/replaySound"), false).toBool();
    m_replayLabel = settings.value(QStringLiteral("moments/replayLabel"), true).toBool();
    // 0.5: Find dead time's remembered options.
    m_deadTimeSensitivity = deadTimeOptionValue("sensitivity", settings.value(QStringLiteral("moments/deadTimeSensitivity"), 0.5));
    m_deadTimeMinSeconds = deadTimeOptionValue("minSeconds", settings.value(QStringLiteral("moments/deadTimeMinSeconds"), 3.0));
    m_deadTimeHandleSeconds =
        deadTimeOptionValue("handleSeconds", settings.value(QStringLiteral("moments/deadTimeHandleSeconds"), 0.3));
    m_deadTimeInOut = settings.value(QStringLiteral("moments/deadTimeInOut"), false).toBool();
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

MomentsBridge::~MomentsBridge() {
    // 0.5: the review's bands go with it.
    if (m_controller && m_deadTimeBands)
        m_controller->setTimelineHighlights(QStringLiteral("deadTime"), {});
}

MomentsBridge* MomentsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new MomentsBridge;
    if (qmlEngine) {
        (void)qmlEngine->singletonInstance<ShortsBridge*>("Wabcut", "Shorts"); // 0.5: registers image://wabcut-frames
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    }
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
        // 0.5: the dead-time review says when its stretches no longer match the sequence, and its bands show on the
        // sequence they were found on only.
        connect(controller, &ProjectController::revisionChanged, this, [this] {
            if (!m_deadStretches.empty())
                refreshDeadTime(false);
        });
        connect(controller, &ProjectController::sequenceChanged, this, [this] {
            if (!m_deadStretches.empty() || m_deadTimeBands)
                refreshDeadTime(false);
        });
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

void MomentsBridge::findDeadTime(const QVariantMap& options) {
    // 0.5: the options given are remembered (the review sheet's sliders, scripts).
    for (const char* name : {"sensitivity", "minSeconds", "handleSeconds", "inOut"}) {
        const QString key = QString::fromLatin1(name);
        if (options.contains(key))
            setDeadTimeOption(key, options.value(key));
    }
    if (m_deadTimeJob->running()) {
        // A cancelled find still winds down (it stops at its next checkpoint): this one starts once it has.
        if (!m_deadTimeBusy) {
            m_deadTimeQueued = true;
            m_deadTimeBusy = true;
            m_deadTimeError.clear();
            m_deadTimePhase = tr("Measuring the gameplay…");
            m_deadTimeProgress = -1.0;
            refreshDeadTime(false);
        }
        return;
    }
    m_deadTimeQueued = false;
    const auto refuse = [this](const QString& why) {
        m_deadTimeFoundWith.reset();
        m_deadStretches.clear();
        m_deadTimeBusy = false;
        m_deadTimeSearched = false;
        m_deadTimeError = why;
        m_deadTimeNote.clear();
        m_deadTimeCurrent = -1;
        refreshDeadTime(true);
        emit deadTimeCurrentChanged();
    };
    if (!m_controller || !registry().knows("sequence.removeDeadTime"))
        return refuse(tr("Remove dead time arrives with the next engine update"));
    const SequencePtr current = m_controller->currentSequence();
    if (!current)
        return refuse(tr("Open a sequence first"));
    if (m_deadTimeInOut) {
        const auto in = m_controller->markIn();
        const auto out = m_controller->markOut();
        if (!in || !out || *out <= *in)
            return refuse(tr("Set an In and an Out mark first, or turn off Only In to Out"));
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const std::uint64_t sequence = m_controller->currentSequenceId().value;
    const std::uint64_t layout = layoutOf(*current);
    const command::CommandRegistry* catalog = &registry();
    const std::shared_ptr<command::CommandServices> hostServices = services();
    const command::Json params{{"sequence", sequence},
                               {"sensitivity", m_deadTimeSensitivity},
                               {"minSeconds", m_deadTimeMinSeconds},
                               {"handleSeconds", m_deadTimeHandleSeconds},
                               {"inOut", m_deadTimeInOut}};
    m_deadStretches.clear();
    m_deadTimeBusy = true;
    m_deadTimeSearched = false;
    m_deadTimeError.clear();
    m_deadTimeNote.clear();
    m_deadTimePhase = tr("Measuring the gameplay…");
    m_deadTimeProgress = -1.0;
    m_deadTimeCurrent = -1;
    m_deadTimeFoundWith = deadTimeOptionValues(); // what this find measures with
    const std::uint64_t run = ++m_deadTimeRun;
    refreshDeadTime(true);
    emit deadTimeCurrentChanged();
    QPointer<MomentsBridge> self(this);
    const bool synchronous = m_synchronous;
    // Progress from the worker reaches the GUI thread queued (directly when synchronous).
    auto report = [self, run, synchronous](double fraction) {
        const auto show = [self, run, fraction] {
            if (!self || self->m_deadTimeRun != run || !self->m_deadTimeBusy)
                return;
            self->m_deadTimeProgress = fraction;
            self->refreshDeadTime(false);
        };
        if (synchronous)
            show();
        else
            QMetaObject::invokeMethod(QCoreApplication::instance(), show, Qt::QueuedConnection);
    };
    m_deadTimeJob->start(
        this, m_synchronous,
        [base, params, catalog, hostServices, self, run, sequence, layout,
         report](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
            // On the worker: the preview on a copy (it decodes and measures; nothing changes).
            EditSession scratch(*base);
            command::EnginePolicy policy;
            policy.services = hostServices;
            command::CommandEngine engine(scratch, *catalog, policy);
            command::CommandRequest request;
            request.command = "sequence.removeDeadTime";
            request.params = params;
            DeadTimeControl control(cancelled, report);
            command::CommandResult result = engine.execute(request, &control);
            return [self, run, sequence, layout, result = std::move(result)] {
                if (!self)
                    return;
                if (self->m_deadTimeRun != run) { // cancelled: a find asked for meanwhile starts now
                    if (self->m_deadTimeQueued)
                        self->findDeadTime();
                    return;
                }
                self->m_deadTimeBusy = false;
                if (!result.ok()) {
                    self->m_deadTimeError = result.error->code == command::ErrorCode::Cancelled
                                                ? QString()
                                                : QString::fromStdString(result.error->message);
                    self->refreshDeadTime(true);
                    return;
                }
                for (const command::Json& r : result.data.value("ranges", command::Json::array())) {
                    const Ticks start{r.value("startTicks", std::int64_t{0})};
                    const Ticks end{r.value("endTicks", std::int64_t{0})};
                    if (end > start)
                        self->m_deadStretches.push_back({start, end, true});
                }
                std::sort(self->m_deadStretches.begin(), self->m_deadStretches.end(),
                          [](const DeadStretch& a, const DeadStretch& b) { return a.start < b.start; });
                self->m_deadTimeSequence = sequence;
                self->m_deadTimeLayout = layout; // the sequence as it was measured
                self->m_deadTimeSearched = true;
                self->refreshDeadTime(true);
            };
        },
        [self, run](const QString& message) -> BridgeJob::Finish {
            return [self, run, message] {
                if (!self)
                    return;
                if (self->m_deadTimeRun != run) {
                    if (self->m_deadTimeQueued)
                        self->findDeadTime();
                    return;
                }
                self->m_deadTimeBusy = false;
                self->m_deadTimeError = message;
                self->refreshDeadTime(true);
            };
        });
}

QString MomentsBridge::removeDeadTime() {
    if (!m_controller || m_deadTimeBusy || m_deadStretches.empty())
        return finish(false, tr("Find the dead time first"));
    if (m_controller->currentSequenceId().value != m_deadTimeSequence)
        return finish(false, tr("The dead time was found on another sequence: find it again"));
    if (deadTimeStale())
        return finish(false, tr("The sequence changed since the dead time was found: find it again"));
    if (m_deadTimeFoundWith && *m_deadTimeFoundWith != deadTimeOptionValues())
        return finish(false, tr("The options changed since the dead time was found: find it again, or set them back"));
    // 0.5: only the switched-on stretches.
    command::Json ranges = command::Json::array();
    int count = 0;
    double seconds = 0.0;
    for (const DeadStretch& s : m_deadStretches) {
        if (!s.remove)
            continue;
        ranges.push_back(command::Json{{"start", s.start.value}, {"end", s.end.value}});
        ++count;
        seconds += (s.end - s.start).toSeconds();
    }
    if (ranges.empty())
        return finish(false, tr("Every stretch is kept: switch one on to remove it"));
    const command::CommandResult result =
        runLive(m_controller, registry(), "sequence.removeDeadTime",
                command::Json{{"sequence", m_deadTimeSequence}, {"apply", true}, {"ranges", std::move(ranges)}}, services());
    if (!result.ok())
        return finish(false, QString::fromStdString(result.error->message)); // the list stays for another try
    cancelDeadTime();
    m_deadTimeNote = count == 1 ? tr("Removed 1 stretch, %1").arg(clockText(seconds))
                                : tr("Removed %1 stretches, %2 in all").arg(count).arg(clockText(seconds));
    refreshDeadTime(false);
    return finish(true, {});
}

void MomentsBridge::cancelDeadTime() {
    if (m_deadTimeJob->running())
        m_deadTimeJob->cancel(); // it stops at its next checkpoint; its result is dropped (m_deadTimeRun)
    ++m_deadTimeRun;
    m_deadTimeQueued = false;
    m_deadTimeFoundWith.reset();
    m_deadStretches.clear();
    m_deadTimeBusy = false;
    m_deadTimeSearched = false;
    m_deadTimeError.clear();
    m_deadTimeNote.clear();
    const bool moved = m_deadTimeCurrent != -1;
    m_deadTimeCurrent = -1;
    refreshDeadTime(true);
    if (moved)
        emit deadTimeCurrentChanged();
}

// ---- 0.5: the review ----

bool MomentsBridge::deadTimeStale() const {
    if (!m_controller || m_deadStretches.empty())
        return false;
    const ProjectSnapshot snap = m_controller->snapshot();
    const Sequence* seq = snap ? snap->findSequence(SequenceId{m_deadTimeSequence}) : nullptr;
    return !seq || layoutOf(*seq) != m_deadTimeLayout;
}

void MomentsBridge::refreshDeadTime(bool rangesChanged) {
    const int count = static_cast<int>(m_deadStretches.size());
    double seconds = 0.0;
    double removeSeconds = 0.0;
    int removeCount = 0;
    for (const DeadStretch& s : m_deadStretches) {
        const double length = (s.end - s.start).toSeconds();
        seconds += length;
        if (s.remove) {
            ++removeCount;
            removeSeconds += length;
        }
    }
    const int whole = static_cast<int>(std::lround(seconds));
    QVariantMap map{{QStringLiteral("count"), count},
                    {QStringLiteral("seconds"), seconds},
                    {QStringLiteral("removeCount"), removeCount},
                    {QStringLiteral("removeSeconds"), removeSeconds},
                    {QStringLiteral("removeText"),
                     tr("%1 of %2 to remove · %3").arg(removeCount).arg(count).arg(clockText(removeSeconds))},
                    {QStringLiteral("searched"), m_deadTimeSearched},
                    {QStringLiteral("stale"), deadTimeStale()},
                    {QStringLiteral("otherSequence"),
                     count > 0 && m_controller && m_controller->currentSequenceId().value != m_deadTimeSequence},
                    {QStringLiteral("optionsChanged"), !m_deadTimeBusy && (count > 0 || m_deadTimeSearched) &&
                                                           m_deadTimeFoundWith && *m_deadTimeFoundWith != deadTimeOptionValues()}};
    if (m_deadTimeBusy) {
        map.insert(QStringLiteral("busy"), true);
        map.insert(QStringLiteral("text"), m_deadTimePhase);
        map.insert(QStringLiteral("phase"), m_deadTimePhase);
        map.insert(QStringLiteral("progress"), m_deadTimeProgress);
    } else if (!m_deadTimeError.isEmpty()) {
        map.insert(QStringLiteral("error"), m_deadTimeError);
    } else if (count > 0) {
        map.insert(QStringLiteral("found"), true);
        map.insert(QStringLiteral("text"), tr("Dead time: %1 stretches, %2:%3 in all")
                                               .arg(count)
                                               .arg(whole / 60)
                                               .arg(whole % 60, 2, 10, QLatin1Char('0')));
    } else if (m_deadTimeSearched) {
        map.insert(QStringLiteral("text"), tr("No dead time found"));
    } else if (!m_deadTimeNote.isEmpty()) {
        map.insert(QStringLiteral("text"), m_deadTimeNote);
    }
    m_deadTime = std::move(map);
    // The rows' thumbnails decode from the project as it is now (image://wabcut-frames).
    if (rangesChanged && count > 0 && m_controller)
        setPreviewFrameContext(m_controller->snapshot(), m_controller->projectFile());
    if (rangesChanged)
        emit deadTimeRangesChanged();
    emit deadTimeChanged();
    updateDeadTimeBands();
}

void MomentsBridge::updateDeadTimeBands() {
    if (!m_controller)
        return;
    // The switched-on stretches, on every track (the removal ripples them all), on the sequence they were found on,
    // while the review is open and they still match it.
    std::vector<TimelineHighlight> bands;
    if (m_deadTimeReviewing && !m_deadTimeBusy && m_controller->currentSequenceId().value == m_deadTimeSequence &&
        !deadTimeStale()) {
        for (const DeadStretch& s : m_deadStretches) {
            if (!s.remove)
                continue;
            TimelineHighlight band;
            band.kind = TimelineHighlight::Kind::Removal;
            band.range = TimeRange::fromStartEnd(s.start, s.end);
            bands.push_back(std::move(band));
        }
    }
    if (bands.empty() && !m_deadTimeBands)
        return;
    m_deadTimeBands = !bands.empty();
    m_controller->setTimelineHighlights(QStringLiteral("deadTime"), std::move(bands));
}

QVariantList MomentsBridge::deadTimeRanges() const {
    QVariantList out;
    const QLocale locale;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const Sequence* seq = snap ? snap->findSequence(SequenceId{m_deadTimeSequence}) : nullptr;
    for (std::size_t i = 0; i < m_deadStretches.size(); ++i) {
        const DeadStretch& s = m_deadStretches[i];
        const double seconds = (s.end - s.start).toSeconds();
        // The frame at the stretch's middle (on a frame of the sequence).
        QString thumbnail;
        if (seq) {
            const Ticks middle = snapToFrame(s.start + Ticks{(s.end - s.start).value / 2}, seq->frameRate);
            thumbnail = frameUrlAt(*snap, *seq, middle);
        }
        out << QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                           {QStringLiteral("start"), s.start.toSeconds()},
                           {QStringLiteral("end"), s.end.toSeconds()},
                           {QStringLiteral("startText"), m_controller ? m_controller->formatTimecode(s.start) : QString()},
                           {QStringLiteral("lengthText"), tr("%1 s").arg(locale.toString(seconds, 'f', 1))},
                           {QStringLiteral("seconds"), seconds},
                           {QStringLiteral("remove"), s.remove},
                           {QStringLiteral("thumbnail"), thumbnail}};
    }
    return out;
}

std::array<double, 4> MomentsBridge::deadTimeOptionValues() const {
    return {m_deadTimeSensitivity, m_deadTimeMinSeconds, m_deadTimeHandleSeconds, m_deadTimeInOut ? 1.0 : 0.0};
}

QVariantMap MomentsBridge::deadTimeOptions() const {
    return {{QStringLiteral("sensitivity"), m_deadTimeSensitivity},
            {QStringLiteral("minSeconds"), m_deadTimeMinSeconds},
            {QStringLiteral("handleSeconds"), m_deadTimeHandleSeconds},
            {QStringLiteral("inOut"), m_deadTimeInOut}};
}

void MomentsBridge::setDeadTimeOption(const QString& name, const QVariant& value) {
    QSettings settings;
    if (name == QLatin1String("inOut")) {
        if (value.toBool() == m_deadTimeInOut)
            return;
        m_deadTimeInOut = value.toBool();
        settings.setValue(QStringLiteral("moments/deadTimeInOut"), m_deadTimeInOut);
    } else {
        double* field = name == QLatin1String("sensitivity")     ? &m_deadTimeSensitivity
                        : name == QLatin1String("minSeconds")    ? &m_deadTimeMinSeconds
                        : name == QLatin1String("handleSeconds") ? &m_deadTimeHandleSeconds
                                                                 : nullptr;
        if (!field)
            return;
        const QByteArray key = name.toLatin1();
        const double clamped = deadTimeOptionValue(std::string_view(key.constData(), static_cast<std::size_t>(key.size())), value);
        if (clamped == *field)
            return;
        *field = clamped;
        settings.setValue(QStringLiteral("moments/deadTime") + name.left(1).toUpper() + name.mid(1), clamped);
    }
    emit deadTimeOptionsChanged();
    // A list from other options says so (optionsChanged); nothing is measured again until Find.
    if (m_deadTimeFoundWith && (!m_deadStretches.empty() || m_deadTimeSearched))
        refreshDeadTime(false);
}

void MomentsBridge::setDeadTimeReviewing(bool reviewing) {
    if (reviewing == m_deadTimeReviewing)
        return;
    m_deadTimeReviewing = reviewing;
    emit deadTimeReviewingChanged();
    updateDeadTimeBands();
}

void MomentsBridge::setDeadTimeCurrent(int index) {
    const int count = static_cast<int>(m_deadStretches.size());
    const int clamped = count == 0 ? -1 : std::clamp(index, -1, count - 1);
    if (clamped == m_deadTimeCurrent)
        return;
    m_deadTimeCurrent = clamped;
    emit deadTimeCurrentChanged();
}

void MomentsBridge::setDeadTimeRemove(int index, bool remove) {
    if (index < 0 || index >= static_cast<int>(m_deadStretches.size()))
        return;
    DeadStretch& s = m_deadStretches[static_cast<std::size_t>(index)];
    if (s.remove == remove)
        return;
    s.remove = remove;
    refreshDeadTime(true);
}

void MomentsBridge::setAllDeadTimeRemove(bool remove) {
    bool changed = false;
    for (DeadStretch& s : m_deadStretches) {
        changed = changed || s.remove != remove;
        s.remove = remove;
    }
    if (changed)
        refreshDeadTime(true);
}

void MomentsBridge::jumpToDeadTime(int index) {
    if (index < 0 || index >= static_cast<int>(m_deadStretches.size()))
        return;
    setDeadTimeCurrent(index);
    if (!m_controller || m_controller->currentSequenceId().value != m_deadTimeSequence)
        return;
    m_controller->setPlayhead(m_deadStretches[static_cast<std::size_t>(index)].start);
    emit revealRequested();
}

bool MomentsBridge::reviewDeadTime(const QString& action) {
    const int count = static_cast<int>(m_deadStretches.size());
    if (count == 0)
        return false;
    const int at = m_deadTimeCurrent;
    if (action == QLatin1String("next")) {
        jumpToDeadTime(at < 0 ? 0 : std::min(at + 1, count - 1));
        return true;
    }
    if (action == QLatin1String("previous")) {
        jumpToDeadTime(at < 0 ? 0 : std::max(at - 1, 0));
        return true;
    }
    if (action == QLatin1String("first") || action == QLatin1String("last")) {
        jumpToDeadTime(action == QLatin1String("first") ? 0 : count - 1);
        return true;
    }
    const int row = at < 0 ? 0 : at;
    if (action == QLatin1String("toggle")) {
        setDeadTimeRemove(row, !m_deadStretches[static_cast<std::size_t>(row)].remove);
        setDeadTimeCurrent(row);
        return true;
    }
    if (action == QLatin1String("keep") || action == QLatin1String("remove")) {
        // Set it, then on to the next stretch (the playhead goes there): the review moves at the pace of the keys.
        setDeadTimeRemove(row, action == QLatin1String("remove"));
        if (row + 1 < count)
            jumpToDeadTime(row + 1);
        else
            setDeadTimeCurrent(row);
        return true;
    }
    return false;
}

void MomentsBridge::openDeadTimeReview() {
    setDeadTimeReviewing(true);
    emit deadTimeReviewRequested();
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
