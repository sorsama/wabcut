#include "TimeRemapBridge.h"
#include "TimelineView.h"

#include <wabcut/model/TimeRemap.h>

#include <QGuiApplication>
#include <QQmlEngine>
#include <QStyleHints>

#include <algorithm>
#include <cmath>

namespace wabcut::app {
void TimelineView::setShowSpeed(bool show) {
    if (show == m_showSpeed)
        return;
    m_showSpeed = show;
    m_pendingAll = true;
    emit showSpeedChanged();
    update();
}
TimelineView::Hit TimelineView::speedHit(double x, double y, Hit hit) const {
    if (!m_showSpeed || m_tool != TimelineTool::Select || !hit.clip || hit.row < 0)
        return hit;
    const Row& row = m_rows[static_cast<std::size_t>(hit.row)];
    if (row.audio)
        return hit;
    const Clip* c = row.track->findClip(hit.clip);
    if (!c)
        return hit;
    const QRectF area = volumeArea(QRectF(xOfTicks(c->start()), row.y - m_scrollY,
                                          xOfTicks(c->end()) - xOfTicks(c->start()), row.height));
    const auto yOf = [&](double speed) {
        return area.top() + TimeRemapBridge::speedFraction(speed) * area.height();
    };
    if (const Curve* curve = c->timeRemap()) {
        for (std::size_t i = 0; i < curve->keys.size(); ++i) {
            const Ticks t = c->start() + curve->keys[i].time - c->sourceIn;
            if (t < c->start() || t >= c->end())
                continue;
            if (std::abs(xOfTicks(t) - x) <= 7 &&
                std::abs(yOf(TimeRemapBridge::speedAt(*c, curve->keys[i].time)) - y) <= 8) {
                hit.zone = Zone::SpeedKey;
                hit.volumeKey = static_cast<int>(i);
                return hit;
            }
        }
    }
    if (hit.zone == Zone::Body) {
        const Ticks t = std::clamp(ticksAtX(x), c->start(), c->end() - Ticks{1});
        if (std::abs(yOf(TimeRemapBridge::speedAt(*c, c->curveTimeAt(t))) - y) <= 5)
            hit.zone = Zone::SpeedLine;
    }
    return hit;
}
void TimelineView::pressSpeed(const Hit& hit) {
    if (!m_controller)
        return;
    m_controller->selectClipById(hit.clip, ProjectController::Replace, true);
    const SequencePtr sequence = m_controller->currentSequence();
    const auto loc = sequence ? sequence->findClip(hit.clip) : std::nullopt;
    if (!loc)
        return;
    const Clip& c = loc->clip();
    const Ticks at = std::clamp(ticksAtX(m_pressPos.x()), c.start(), c.end() - Ticks{1});
    const Curve* curve = c.timeRemap();
    const bool keyExists = hit.zone == Zone::SpeedKey && curve && hit.volumeKey >= 0 &&
                           static_cast<std::size_t>(hit.volumeKey) < curve->keys.size();
    m_speedDrag.key = keyExists ? hit.volumeKey : -1;
    m_speedDrag.percent = TimeRemapBridge::speedAt(c, c.curveTimeAt(at));
    m_speedDrag.left =
        keyExists && m_pressPos.x() < xOfTicks(c.start() + curve->keys[static_cast<std::size_t>(hit.volumeKey)].time -
                                               c.sourceIn);
    m_drag = Drag::Speed;
    dragSpeed(m_pressPos);
}
void TimelineView::dragSpeed(QPointF pos) {
    const bool moved =
        (pos - m_pressPos).manhattanLength() >= QGuiApplication::styleHints()->startDragDistance();
    if (m_pressMods & Qt::AltModifier)
        m_speedReadout = tr("Frame hold · %1 s").arg(std::abs(pos.x() - m_pressPos.x()) / m_pps, 0, 'f', 2);
    else if ((m_pressMods & (Qt::ControlModifier | Qt::MetaModifier)) || (moved && m_speedDrag.key < 0))
        m_speedReadout =
            tr("Speed · %1%")
                .arg(std::clamp(std::abs(m_speedDrag.percent) * std::exp((m_pressPos.y() - pos.y()) / 40),
                                1.0, 1000.0),
                     0, 'f', 0);
    else if (m_speedDrag.key >= 0)
        m_speedReadout =
            tr("Ramp transition · %1 s").arg(std::abs(pos.x() - m_pressPos.x()) / m_pps, 0, 'f', 2);
    else
        m_speedReadout = tr("Add speed key · %1%").arg(m_speedDrag.percent, 0, 'f', 0);
    emit gestureChanged();
}
void TimelineView::releaseSpeed(QPointF pos) {
    // The drag ends here, before the edits below change the model (onModelChanged cancels a drag still in progress).
    const SpeedDrag drag = m_speedDrag;
    m_speedDrag = {};
    m_drag = Drag::None;
    m_speedReadout.clear();
    emit gestureChanged();
    if (!m_controller)
        return;
    const SequencePtr sequence = m_controller->currentSequence();
    const auto loc = sequence ? sequence->findClip(m_pressClip) : std::nullopt;
    if (!loc)
        return;
    const Clip c = loc->clip();
    const auto fraction = [&](double x) {
        return std::clamp((ticksAtX(x) - c.start()).toSeconds() / c.duration().toSeconds(), 0.0, 1.0);
    };
    const bool moved =
        (pos - m_pressPos).manhattanLength() >= QGuiApplication::styleHints()->startDragDistance();
    // Decide what the gesture does before touching the model, so one that does nothing commits nothing (review P2-40):
    // Alt-drag holds, Ctrl-drag or a plain drag of the line changes the segment's speed (Premiere drags the rubber
    // band), a drag of a key splits it into a ramp, a click on the line adds a key.
    enum class Action { None, Hold, Speed, Split, AddKey };
    Action action = Action::None;
    if ((m_pressMods & Qt::AltModifier) && moved)
        action = Action::Hold;
    else if (moved && ((m_pressMods & (Qt::ControlModifier | Qt::MetaModifier)) || drag.key < 0))
        action = Action::Speed;
    else if (moved && drag.key >= 0)
        action = Action::Split;
    else if (!moved && drag.key < 0)
        action = Action::AddKey;
    if (action == Action::None)
        return;
    TimeRemapBridge local;
    auto* engine = qmlEngine(this);
    auto* shared = engine ? engine->singletonInstance<TimeRemapBridge*>("Wabcut", "TimeRemap") : nullptr;
    TimeRemapBridge& bridge = shared ? *shared : local;
    bridge.setController(m_controller);
    bridge.beginGesture();
    bool ok = bridge.enabled() || bridge.setEnabled(true);
    if (ok && action == Action::Hold) {
        const double seconds =
            std::max(m_controller->frameTicks().toSeconds(), std::abs(pos.x() - m_pressPos.x()) / m_pps);
        ok = bridge.hold(fraction(m_pressPos.x()), seconds);
    } else if (ok && action == Action::Speed) {
        // Bound an initially uniform map to the visible clip, so changing speed cannot move its source In.
        if (!c.isRemapped())
            ok = bridge.addKey(0) && bridge.addKey(1);
        const auto now = m_controller->currentSequence()->findClip(c.id);
        if (ok && now && now->clip().timeRemap()) {
            const auto& current = now->clip();
            const auto& curve = *current.timeRemap();
            const Ticks at = current.curveTimeAt(
                std::clamp(ticksAtX(m_pressPos.x()), current.start(), current.end() - Ticks{1}));
            const auto next = std::upper_bound(curve.keys.begin(), curve.keys.end(), at,
                                               [](Ticks t, const Keyframe& k) { return t < k.time; });
            const int segment = static_cast<int>(next - curve.keys.begin()) - 1;
            const double speed =
                std::clamp(std::abs(drag.percent) * std::exp((m_pressPos.y() - pos.y()) / 40), 1.0, 1000.0);
            ok = bridge.setSpeed(segment, speed);
        }
    } else if (ok && action == Action::Split) {
        ok = bridge.splitKey(drag.key, fraction(pos.x()), drag.left);
    } else if (ok && action == Action::AddKey) {
        ok = bridge.addKey(fraction(m_pressPos.x()));
    }
    if (ok)
        bridge.endGesture();
    else
        bridge.cancelGesture();
}
} // namespace wabcut::app
