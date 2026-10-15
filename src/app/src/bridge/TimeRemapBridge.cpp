#include "TimeRemapBridge.h"

#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/model/TimeRemap.h>

#include <QPointF>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut::app {
using command::Json;

TimeRemapBridge::TimeRemapBridge(QObject* parent) : QObject(parent) {}
TimeRemapBridge::~TimeRemapBridge() {
    cancelGesture();
}
TimeRemapBridge* TimeRemapBridge::create(QQmlEngine* engine, QJSEngine*) {
    auto* bridge = new TimeRemapBridge;
    if (engine)
        bridge->setController(engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}
void TimeRemapBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    cancelGesture();
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::sessionClosing, this, &TimeRemapBridge::cancelGesture);
        connect(controller, &ProjectController::revisionChanged, this, &TimeRemapBridge::changed);
        connect(controller, &ProjectController::playheadChanged, this, &TimeRemapBridge::changed);
        connect(controller, &ProjectController::selectionChanged, this, [this] {
            endGesture();
            emit changed();
        });
        connect(controller, &ProjectController::sequenceChanged, this, &TimeRemapBridge::changed);
    }
    emit changed();
}
std::optional<Clip> TimeRemapBridge::clip() const {
    if (!m_controller)
        return {};
    const auto seq = m_controller->currentSequence();
    const auto target =
        m_gesture ? std::optional<ClipId>(m_gestureClip) : m_controller->propertyTarget(StreamKind::Video);
    const auto loc = seq && target ? seq->findClip(*target) : std::nullopt;
    return loc ? std::optional<Clip>(loc->clip()) : std::nullopt;
}
bool TimeRemapBridge::available() const {
    return clip().has_value();
}
bool TimeRemapBridge::enabled() const {
    const auto c = clip();
    return c && c->isRemapped();
}
QString TimeRemapBridge::clipName() const {
    const auto c = clip();
    return c ? QString::fromStdString(c->name()) : QString();
}
double TimeRemapBridge::duration() const {
    const auto c = clip();
    return c ? c->duration().toSeconds() : 0;
}
double TimeRemapBridge::playhead() const {
    const auto c = clip();
    return c ? std::clamp((m_controller->playhead() - c->start()).toSeconds() / c->duration().toSeconds(),
                          0.0, 1.0)
             : 0;
}
double TimeRemapBridge::speedAt(const Clip& clip, Ticks at) {
    const Curve* curve = clip.timeRemap();
    if (!curve)
        return clip.isFreeze() ? 0 : (clip.reversed ? -100.0 : 100.0) * clip.speed.num / clip.speed.den;
    const RemapSegment s = remapSegmentAt(*curve, at);
    if (s.motion == RemapMotion::Hold)
        return 0;
    if (s.straight)
        return 100.0 * static_cast<double>((s.sourceTo - s.sourceFrom).value) /
               static_cast<double>((s.to - s.from).value);
    const Ticks delta{std::max<std::int64_t>(1, (s.to - s.from).value / 10000)};
    const Ticks a = std::max(s.from, at - delta), b = std::min(s.to, at + delta);
    return b > a ? 100.0 * (curve->valueAt(b) - curve->valueAt(a)) / static_cast<double>((b - a).value) : 0;
}
double TimeRemapBridge::speedFraction(double percent) {
    return std::clamp(0.75 - percent / 800.0, 0.0, 1.0);
}
QString TimeRemapBridge::speedLabel(const Clip& clip) {
    std::optional<double> uniform;
    if (const Curve* curve = clip.timeRemap()) {
        for (const auto& segment : remapSegments(*curve)) {
            if (segment.to <= clip.sourceIn || segment.from >= clip.sourceOut())
                continue;
            if (!segment.straight && segment.motion != RemapMotion::Hold)
                return tr("Variable speed");
            const double speed = speedAt(clip, std::max(segment.from, clip.sourceIn));
            if (uniform && std::abs(*uniform - speed) > 0.001)
                return tr("Variable speed");
            uniform = speed;
        }
    }
    return tr("%1%").arg(uniform.value_or(speedAt(clip, clip.sourceIn)), 0, 'f', 0);
}
double TimeRemapBridge::fractionSpeed(double fraction) {
    return (0.75 - std::clamp(fraction, 0.0, 1.0)) * 800.0;
}
QVariantList TimeRemapBridge::keys() const {
    QVariantList out;
    const auto c = clip();
    if (!c || !c->timeRemap())
        return out;
    const auto& curve = *c->timeRemap();
    for (std::size_t i = 0; i < curve.keys.size(); ++i) {
        const Keyframe& k = curve.keys[i];
        const double x =
            static_cast<double>((k.time - c->sourceIn).value) / static_cast<double>(c->duration().value);
        if (x < 0 || x > 1)
            continue;
        out << QVariantMap{{"index", static_cast<int>(i)},
                           {"x", x},
                           {"source", k.value / kTicksPerSecond},
                           {"speed", speedAt(*c, k.time)},
                           {"interpolation", QString::fromUtf8(toString(k.interpolation).data())}};
    }
    return out;
}
QVariantList TimeRemapBridge::sourcePoints() const {
    QVariantList out;
    const auto c = clip();
    if (!c || !c->timeRemap())
        return out;
    for (int i = 0; i <= 128; ++i) {
        const double x = i / 128.0;
        const Ticks t =
            c->sourceIn + Ticks{static_cast<std::int64_t>(static_cast<double>(c->duration().value) * x)};
        out << QPointF(x, c->timeRemap()->valueAt(t) / kTicksPerSecond);
    }
    return out;
}
QVariantList TimeRemapBridge::speedPoints() const {
    QVariantList out;
    const auto c = clip();
    if (!c || !c->timeRemap())
        return out;
    for (int i = 0; i <= 128; ++i) {
        const double x = i / 128.0;
        const Ticks t =
            c->sourceIn +
            Ticks{std::min(c->duration().value - 1,
                           static_cast<std::int64_t>(static_cast<double>(c->duration().value) * x))};
        out << QPointF(x, speedAt(*c, t));
    }
    return out;
}
double TimeRemapBridge::sourceMin() const {
    double value = std::numeric_limits<double>::max();
    for (const auto& p : sourcePoints())
        value = std::min(value, p.toPointF().y());
    return value == std::numeric_limits<double>::max() ? 0 : value;
}
double TimeRemapBridge::sourceMax() const {
    double value = 0;
    for (const auto& p : sourcePoints())
        value = std::max(value, p.toPointF().y());
    return std::max(sourceMin() + 0.001, value);
}
Ticks TimeRemapBridge::timelineAt(const Clip& c, double fraction) const {
    // Pixel -> fraction -> ticks can land a fraction of a tick below an exact frame.
    // Round back to the tick before frame snapping, or a click on a key's left half can become its right
    // half.
    const Ticks t =
        c.start() +
        Ticks{std::llround(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(c.duration().value))};
    return std::clamp(snapToFrame(t, m_controller->currentSequence()->frameRate), c.start(),
                      c.end() - Ticks{1});
}
void TimeRemapBridge::setMessage(const QString& text) {
    m_message = text;
    emit messageChanged();
}
bool TimeRemapBridge::run(const char* name, Json params) {
    const auto c = clip();
    if (!c || !m_controller)
        return false;
    params["clip"] = c->id.value;
    params["sequence"] = m_controller->currentSequenceId().value;
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    command::CommandEngine engine(m_controller->session());
    const auto result = engine.execute(request);
    setMessage(result.ok() ? QString() : QString::fromStdString(result.error->message));
    return result.ok();
}
bool TimeRemapBridge::setEnabled(bool on) {
    return run("timeRemap.enable", {{"enabled", on}});
}
bool TimeRemapBridge::addKey(double fraction) {
    const auto c = clip();
    return c && std::isfinite(fraction) &&
           run("timeRemap.addKey", {{"time", timelineAt(*c, fraction).value}});
}
bool TimeRemapBridge::moveKey(int index, double fraction) {
    const auto c = clip();
    return c && std::isfinite(fraction) &&
           run("timeRemap.moveKey", {{"index", index}, {"time", timelineAt(*c, fraction).value}});
}
bool TimeRemapBridge::setSpeed(int segment, double percent) {
    return std::isfinite(percent) &&
           run("timeRemap.setSpeed", {{"segment", segment}, {"speed", percent}, {"extendClip", false}});
}
bool TimeRemapBridge::setInterpolation(int index, const QString& interpolation) {
    return run("timeRemap.setInterpolation",
               {{"keys", Json::array({index})}, {"interpolation", interpolation.toStdString()}});
}
bool TimeRemapBridge::removeKey(int index) {
    return run("timeRemap.removeKey", {{"index", index}});
}
bool TimeRemapBridge::batch(std::vector<command::CommandRequest> commands) {
    const auto c = clip();
    if (!c || !m_controller)
        return false;
    for (auto& request : commands) {
        request.params["clip"] = c->id.value;
        request.params["sequence"] = m_controller->currentSequenceId().value;
    }
    command::CommandBatch batch;
    batch.label = "Time remapping";
    batch.commands = std::move(commands);
    command::CommandEngine engine(m_controller->session());
    const auto result = engine.execute(batch);
    setMessage(result.ok() ? QString() : QString::fromStdString(result.error->message));
    return result.ok();
}
bool TimeRemapBridge::splitKey(int index, double fraction, bool left) {
    const auto c = clip();
    if (!c || !c->timeRemap() || !std::isfinite(fraction))
        return false;
    const Curve& curve = *c->timeRemap();
    if (index <= 0 || index + 1 >= static_cast<int>(curve.keys.size()))
        return false;
    const Keyframe& key = curve.keys[static_cast<std::size_t>(index)];
    const Ticks t = timelineAt(*c, fraction);
    const Ticks at = c->curveTimeAt(t);
    if ((left && (at >= key.time || at <= curve.keys[static_cast<std::size_t>(index - 1)].time)) ||
        (!left && (at <= key.time || at >= curve.keys[static_cast<std::size_t>(index + 1)].time)))
        return false;
    const double before = speedAt(*c, key.time - Ticks{1}) / 100.0 * kTicksPerSecond;
    const double after = speedAt(*c, key.time) / 100.0 * kTicksPerSecond;
    const int first = index, last = index + 1;
    const auto request = [](const char* name, Json params) {
        command::CommandRequest r;
        r.command = name;
        r.params = std::move(params);
        return r;
    };
    // C.6 validates the complete batch before committing. The two halves join the adjacent speeds
    // continuously.
    return batch({request("timeRemap.addKey", {{"time", t.value}}),
                  request("timeRemap.setKey", {{"index", first},
                                               {"interpolation", "bezier"},
                                               {"inSlope", before},
                                               {"outSlope", before},
                                               {"inInfluence", 1.0 / 3},
                                               {"outInfluence", 1.0 / 3}}),
                  request("timeRemap.setKey", {{"index", last},
                                               {"interpolation", "bezier"},
                                               {"inSlope", after},
                                               {"outSlope", after},
                                               {"inInfluence", 1.0 / 3},
                                               {"outInfluence", 1.0 / 3}})});
}
bool TimeRemapBridge::insertSegment(const char* name, double fraction, double seconds) {
    const auto c = clip();
    if (!c || !std::isfinite(fraction) || !std::isfinite(seconds) || seconds <= 0 ||
        seconds > c->duration().toSeconds())
        return false;
    const bool own = !m_gesture;
    if (own)
        beginGesture();
    const bool ok = (enabled() || setEnabled(true)) &&
                    run(name, {{"time", timelineAt(*c, fraction).value},
                               {"duration", static_cast<std::int64_t>(seconds * kTicksPerSecond)},
                               {"extendClip", false}});
    if (own) {
        if (ok)
            endGesture();
        else
            cancelGesture();
    }
    return ok;
}
bool TimeRemapBridge::hold(double fraction, double seconds) {
    return insertSegment("timeRemap.addHold", fraction, seconds);
}
bool TimeRemapBridge::reverse(double fraction, double seconds) {
    return insertSegment("timeRemap.addReverse", fraction, seconds);
}
void TimeRemapBridge::beginGesture() {
    if (m_gesture || !m_controller)
        return;
    const auto c = clip();
    if (!c)
        return;
    m_gestureClip = c->id;
    m_gesture = true;
    m_controller->beginGesture(tr("Time remapping"));
}
void TimeRemapBridge::endGesture() {
    if (!m_gesture)
        return;
    m_gesture = false;
    if (m_controller)
        m_controller->endGesture();
}
void TimeRemapBridge::cancelGesture() {
    if (!m_gesture)
        return;
    m_gesture = false;
    if (m_controller)
        m_controller->cancelGesture();
}
bool TimeRemapBridge::preset(const QString& name) {
    if (name == QLatin1String("hold") || name == QLatin1String("reverse")) {
        const auto c = clip();
        if (!c || !c->timelineRange.contains(m_controller->playhead())) {
            setMessage(tr("Move the playhead inside the selected clip."));
            return false;
        }
    }
    if (name == QLatin1String("hold"))
        return hold(playhead(), std::min(1.0, duration() / 4));
    if (name == QLatin1String("reverse"))
        return reverse(playhead(), std::min(0.5, duration() / 4));
    if (name != QLatin1String("in") && name != QLatin1String("out") && name != QLatin1String("inOut"))
        return false;
    beginGesture();
    bool ok = enabled() || setEnabled(true);
    if (ok)
        ok = addKey(0) && addKey(1);
    const auto c = clip();
    if (ok && c && c->timeRemap()) {
        const auto first = c->timeRemap()->keyAt(c->curveTimeAt(timelineAt(*c, 0)));
        const auto last = c->timeRemap()->keyAt(c->curveTimeAt(timelineAt(*c, 1)));
        if (first && last) {
            if (name != QLatin1String("out"))
                ok = setInterpolation(static_cast<int>(*first), QStringLiteral("easeOut"));
            if (ok && name != QLatin1String("in"))
                ok = setInterpolation(static_cast<int>(*last), QStringLiteral("easeIn"));
        } else
            ok = false;
    }
    if (ok)
        endGesture();
    else
        cancelGesture();
    return ok;
}
} // namespace wabcut::app
