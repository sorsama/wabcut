// TimelineView: mouse, keyboard and editing gestures (M5b). Rendering lives in TimelineView.cpp.
//
// A press decides what the drag will be (tool, edge zone, modifiers); past the drag threshold the gesture starts:
// the dragged material is captured (ghost outlines, snap edges, snap targets), and every move previews the edit on
// the current snapshot (ProjectController::previewEdit) and displays the result. Release commits the same edit as
// one undo step (ProjectController::commitGesture); Esc cancels and the clips slide back.

#include "TimelineView.h"

#include "BridgeIds.h"
#include "EditCursors.h"
#include "EffectControls.h"

#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Snap.h>
#include <wabcut/model/Timecode.h>

#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace wabcut::app {

namespace {

std::size_t firstEndingAfter(const ClipList& clips, Ticks t) {
    std::size_t i = clips.upperBoundStart(t);
    if (i > 0 && clips[i - 1].end() > t)
        --i;
    return i;
}

} // namespace

// ---- Tool state ---------------------------------------------------------------------------------------------------

void TimelineView::setTool(const QString& key) {
    const auto tool = parseToolKey(key);
    if (!tool || *tool == m_tool)
        return;
    cancelGesture();
    const bool band = volumeBandShown();
    m_tool = *tool;
    if (volumeBandShown() != band)
        m_pendingAll = true; // the pen tool shows the volume rubber band
    // The pointer may sit over the lanes (a key switched the tool): its cursor and edge follow at once.
    setHoverEdge({});
    if (m_hoverInside)
        updateHoverCursor(m_hoverPos, QGuiApplication::queryKeyboardModifiers());
    else
        applyCursor({});
    emit toolChanged();
    update();
}

void TimelineView::setSnapping(bool snapping) {
    if (snapping == m_snapping)
        return;
    m_snapping = snapping;
    emit snappingChanged();
}

double TimelineView::snapX() const {
    return m_gesture.active && m_gesture.snappedAt ? xOfTicks(*m_gesture.snappedAt) : qQNaN();
}

Ticks TimelineView::snapTolerance() const {
    return Ticks{static_cast<std::int64_t>(m_style.snapThreshold / std::max(1e-6, m_pps)
                                           * static_cast<double>(kTicksPerSecond))};
}

std::vector<Ticks> TimelineView::localSnapTargets(Ticks t) const {
    std::vector<Ticks> targets;
    if (!m_sequence)
        return targets;
    targets.push_back(m_playhead);
    if (m_sequence->markIn)
        targets.push_back(*m_sequence->markIn);
    if (m_sequence->markOut)
        targets.push_back(*m_sequence->markOut);
    for (const Marker& m : m_sequence->markers) {
        targets.push_back(m.start);
        if (m.duration.value > 0)
            targets.push_back(m.start + m.duration);
    }
    // 0.1.3: the beats of the beat grid near `t`, like a drag's targets (edit::snapTargets).
    if (m_sequence->beatGrid) {
        const Ticks reach = snapTolerance();
        for (const Beat& b : m_sequence->beatGrid->beatsIn(TimeRange::fromStartEnd(t > reach ? t - reach : Ticks{}, t + reach + Ticks{1})))
            targets.push_back(b.time);
    }
    m_sequence->forEachTrack([&](const Track& track) {
        const std::size_t i = firstEndingAfter(track.clips, t);
        for (std::size_t k = i > 0 ? i - 1 : 0; k < std::min(track.clips.size(), i + 2); ++k) {
            targets.push_back(track.clips[k].start());
            targets.push_back(track.clips[k].end());
        }
    });
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

Ticks TimelineView::snappedTimeAt(double x, std::optional<Ticks>* snappedAt) const {
    const Ticks t = snapToFrame(ticksAtX(x), frameRate());
    if (snappedAt)
        snappedAt->reset();
    if (!m_snapping)
        return t;
    const std::vector<Ticks> targets = localSnapTargets(t);
    const auto target = edit::nearestSnapTarget(targets, t, snapTolerance());
    if (!target)
        return t;
    if (snappedAt)
        *snappedAt = target;
    return *target;
}

Ticks TimelineView::dropTimeAt(double x, std::optional<Ticks>* snappedAt) const {
    const Rate rate = frameRate();
    const Ticks pointer = ticksAtX(x);
    const Ticks frame = snapToFrame(pointer, rate);
    if (snappedAt)
        snappedAt->reset();
    if (!m_snapping)
        return frame;
    // The pointer (not the start of its frame) picks the nearest target; a drop lands on a frame, so an edge between
    // frames (an AAC tail) rounds to the nearest one.
    const std::vector<Ticks> targets = localSnapTargets(pointer);
    const auto target = edit::nearestSnapTarget(targets, pointer, snapTolerance());
    if (!target)
        return frame;
    const Ticks at = snapToFrame(*target + Ticks{framesToTicks(1, rate).value / 2}, rate);
    if (snappedAt)
        *snappedAt = at;
    return at;
}

QVariantMap TimelineView::dropPoint(double x, double y) const {
    std::optional<Ticks> snapped;
    const Ticks at = dropTimeAt(x, &snapped);
    const int row = rowAt(y);
    const bool dropFrame = m_sequence && m_sequence->dropFrame;
    QVariantMap point;
    point.insert(QStringLiteral("time"), QString::fromStdString(ticksToTimecode(at, frameRate(), dropFrame)));
    point.insert(QStringLiteral("x"), xOfTicks(at));
    point.insert(QStringLiteral("snapped"), snapped.has_value());
    point.insert(QStringLiteral("track"), row < 0 ? QString() : trackKey(m_rows[static_cast<std::size_t>(row)].track->id));
    point.insert(QStringLiteral("rowY"), row < 0 ? 0.0 : m_rows[static_cast<std::size_t>(row)].y - m_scrollY);
    point.insert(QStringLiteral("rowHeight"), row < 0 ? height() : m_rows[static_cast<std::size_t>(row)].height);
    return point;
}

QVariantMap TimelineView::showDropIndicator(double x, double y) {
    std::optional<Ticks> snapped;
    const Ticks at = dropTimeAt(x, &snapped);
    const int row = rowAt(y);
    const bool changed = !m_dropIndicator.shown || m_dropIndicator.at != at || m_dropIndicator.row != row
                         || m_dropIndicator.snapped != snapped.has_value();
    m_dropIndicator.shown = true;
    m_dropIndicator.at = at;
    m_dropIndicator.row = row;
    m_dropIndicator.snapped = snapped.has_value();
    m_dropIndicator.point = dropPoint(x, y);
    if (changed) {
        emit dropIndicatorChanged();
        update();
    }
    return m_dropIndicator.point;
}

void TimelineView::clearDropIndicator() {
    if (!m_dropIndicator.shown)
        return;
    m_dropIndicator = {};
    emit dropIndicatorChanged();
    update();
}

QVariantMap TimelineView::showDropIndicatorOnTrack(double x, const QString& track) {
    const auto id = parseTrackKey(track);
    if (!id)
        return {};
    for (const Row& row : m_rows) {
        if (row.track->id == *id)
            return showDropIndicator(x, row.y - m_scrollY + row.height / 2);
    }
    return {};
}

// ---- Tracks a held drag adds (2026-10-15) ---------------------------------------------------------------------------
// TimelinePanel times the hold; these add the track and take it away again when the drag drops elsewhere.

QString TimelineView::addDropTrack(const QString& kind) {
    if (!m_controller || !m_sequence)
        return {};
    const TrackKind trackKind = kind == QLatin1String("audio") ? TrackKind::Audio : TrackKind::Video;
    const SequenceId sequence = m_sequence->id;
    EditSession& session = m_controller->session();
    const char* label = QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Track");
    auto created = session.apply(label, [&](const ProjectState& state) {
        return edit::addTrack(state, sequence, trackKind);
    });
    if (!created)
        return {};
    m_dropTrack = {created->id, session.revision()};
    // The session told the controller, which relaid the rows: the new track is the top or the bottom row.
    setScrollY(trackKind == TrackKind::Video ? 0.0 : m_contentHeight);
    return trackKey(created->id);
}

bool TimelineView::releaseDropTrack(const QString& track) {
    const DropTrack added = std::exchange(m_dropTrack, {});
    const auto id = parseTrackKey(track);
    if (!m_controller || !id || !added.track || *id != added.track)
        return false;
    // Anything edited since (the drop itself comes after this) keeps the track: undo would take that edit instead.
    EditSession& session = m_controller->session();
    if (session.revision() != added.revision)
        return false;
    const ProjectSnapshot now = session.snapshot();
    for (const SequencePtr& sequence : now->sequences) {
        if (const Track* t = sequence->findTrack(added.track); t && !t->clips.empty())
            return false;
    }
    return m_controller->undo();
}

QString TimelineView::nextTrackLabel(const QString& kind) const {
    const bool audio = kind == QLatin1String("audio");
    const std::size_t count = !m_sequence ? 0 : m_sequence->tracks(audio ? TrackKind::Audio : TrackKind::Video).size();
    return QStringLiteral("%1%2").arg(audio ? QLatin1Char('A') : QLatin1Char('V')).arg(count + 1);
}

int TimelineView::nearestRow(double y) const {
    if (m_rows.empty())
        return -1;
    const double contentY = y + m_scrollY;
    int best = 0;
    double bestDistance = 1e300;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        const Row& r = m_rows[i];
        if (contentY >= r.y && contentY < r.y + r.height)
            return static_cast<int>(i);
        const double distance = std::abs(contentY - (r.y + r.height / 2));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void TimelineView::markTracksWith(const std::unordered_set<std::uint64_t>& clips) {
    if (clips.empty() || !m_sequence)
        return;
    const double margin = std::max(1.0, width());
    const Ticks t0 = ticksAtX(-margin);
    const Ticks t1 = ticksAtX(width() + margin);
    m_sequence->forEachTrack([&](const Track& track) {
        for (auto it = track.clips.iteratorAt(firstEndingAfter(track.clips, t0)); it != track.clips.end() && it->start() < t1; ++it) {
            if (clips.contains(it->id.value)) {
                m_animatedTracks.insert(track.id.value);
                return;
            }
        }
    });
}

// ---- Hit-testing ----------------------------------------------------------------------------------------------------

TimelineView::Hit TimelineView::hitTest(double x, double y) const {
    Hit hit;
    hit.row = rowAt(y);
    if (hit.row < 0)
        return hit;
    // Transitions sit over the edit point: they take the press before the clips' edges.
    if (const Hit transition = transitionHit(x, hit.row); transition.transition)
        return transition;
    const Track& track = *m_rows[static_cast<std::size_t>(hit.row)].track;
    const Ticks t = ticksAtX(x);
    const double handle = m_style.trimHandleWidth;
    if (const auto index = track.clipIndexAt(t)) {
        const Clip& c = track.clips[*index];
        const double x0 = xOfTicks(c.start());
        const double x1 = xOfTicks(c.end());
        const double reach = std::min(handle, (x1 - x0) / 3.0);
        hit.clip = c.id;
        hit.zone = x - x0 <= reach ? Zone::Head : x1 - x <= reach ? Zone::Tail : Zone::Body;
        return speedHit(x, y, volumeHit(x, y, hit));
    }
    // In a gap: an edge within reach still counts (the edge zone straddles the clip's border).
    const std::size_t next = track.clips.upperBoundStart(t);
    if (next < track.clips.size() && xOfTicks(track.clips[next].start()) - x <= handle) {
        hit.clip = track.clips[next].id;
        hit.zone = Zone::Head;
    } else if (next > 0 && x - xOfTicks(track.clips[next - 1].end()) <= handle) {
        hit.clip = track.clips[next - 1].id;
        hit.zone = Zone::Tail;
    }
    return hit;
}

GestureKind TimelineView::gestureFor(const Hit& hit, Qt::KeyboardModifiers mods) const {
    if (!hit.clip)
        return GestureKind::None;
    const bool edge = hit.zone == Zone::Head || hit.zone == Zone::Tail;
    switch (m_tool) {
    case TimelineTool::Select:
        if (edge)
            return (mods & Qt::ControlModifier) ? GestureKind::RippleTrim : GestureKind::Trim;
        return GestureKind::Move;
    case TimelineTool::TrackForward:
    case TimelineTool::TrackBackward:
        return GestureKind::Move;
    case TimelineTool::Ripple:
        return edge ? GestureKind::RippleTrim : GestureKind::None;
    case TimelineTool::Roll:
        return edge ? GestureKind::Roll : GestureKind::None;
    case TimelineTool::Slip:
        return GestureKind::Slip;
    case TimelineTool::Slide:
        return GestureKind::Slide;
    case TimelineTool::RateStretch:
        return edge ? GestureKind::RateStretch : GestureKind::None;
    case TimelineTool::Razor:
    case TimelineTool::Remix:
    case TimelineTool::Pen:
        break;
    }
    return GestureKind::None;
}

// ---- Mouse --------------------------------------------------------------------------------------------------------

void TimelineView::mouseDoubleClickEvent(QMouseEvent* event) {
    // Only a double-click on a clip means something (a nested clip opens); elsewhere it does what it did before.
    const Hit hit = m_controller && !m_gesture.active && event->button() == Qt::LeftButton
                        ? hitTest(event->position().x(), event->position().y())
                        : Hit{};
    if (!hit.clip) {
        QQuickItem::mouseDoubleClickEvent(event);
        return;
    }
    event->accept();
    emit clipDoubleClicked(clipKey(hit.clip));
}

void TimelineView::mousePressEvent(QMouseEvent* event) {
    emit pressed();
    forceActiveFocus(Qt::MouseFocusReason); // Esc reaches the view while it drags
    event->accept();
    if (m_gesture.active || !m_controller)
        return; // another button during a drag
    if (m_drag == Drag::Speed && event->buttons() != event->button())
        return; // another button during a speed drag (review P2-43)
    m_pressPos = event->position();
    m_pressMods = event->modifiers();
    m_pressHit = {};
    m_pressClip = {};
    m_pendingKind = GestureKind::None;
    const Hit hit = hitTest(m_pressPos.x(), m_pressPos.y());

    if (event->button() == Qt::RightButton) {
        m_drag = Drag::None;
        if (hit.transition) {
            m_controller->selectTransitionById(hit.transition);
            return;
        }
        if (hit.clip) {
            if (!m_controller->isSelected(hit.clip))
                m_controller->selectClipById(hit.clip, ProjectController::Replace, followLinks(m_pressMods));
            emit contextMenuRequested(clipKey(hit.clip), m_pressPos.x(), m_pressPos.y());
        }
        return;
    }

    switch (m_tool) {
    case TimelineTool::Razor:
        razorClick(m_pressPos.x(), hit.row, m_pressMods);
        m_drag = Drag::None;
        return;
    case TimelineTool::Remix: {
        // M5m, Premiere's Remix tool: drag a music clip's end to propose a length (released: remixRequested).
        m_drag = Drag::None;
        const SequencePtr seq = m_controller->currentSequence();
        const auto loc = hit.clip && seq ? seq->findClip(hit.clip) : std::nullopt;
        if (loc && loc->track->kind == TrackKind::Audio) {
            m_controller->selectClipById(hit.clip, ProjectController::Replace, false);
            const Clip& c = loc->clip();
            m_remix = RemixDrag{hit.clip, hit.row, c.start(), c.end(), c.end()};
            m_drag = Drag::Remix;
            ++m_marqueeRevision;
            update();
        }
        return;
    }
    case TimelineTool::TrackForward:
    case TimelineTool::TrackBackward:
        if (hit.row >= 0) {
            trackSelect(m_pressPos.x(), hit.row, m_tool == TimelineTool::TrackBackward,
                        m_pressMods & Qt::ShiftModifier);
            m_pressHit = hit;
            m_pressClip = hit.clip;
            m_pendingKind = GestureKind::Move;
            m_drag = Drag::Pending;
        }
        return;
    default:
        break;
    }

    if (hit.transition) {
        // Select the transition (Effect Controls shows it); its edges change its duration.
        m_controller->selectTransitionById(hit.transition);
        if (hit.zone == Zone::Head || hit.zone == Zone::Tail) {
            m_transitionDrag = {hit.transition, hit.zone == Zone::Head,
                                "transition-edge:" + std::to_string(++m_transitionDragCounter)};
            m_drag = Drag::TransitionEdge;
        } else {
            m_drag = Drag::Click;
        }
        return;
    }
    if (hit.zone == Zone::SpeedKey || hit.zone == Zone::SpeedLine) {
        m_pressHit = hit;
        m_pressClip = hit.clip;
        pressSpeed(hit);
        return;
    }
    if (hit.zone == Zone::VolumeKey || hit.zone == Zone::VolumeLine) {
        m_pressHit = hit;
        m_pressClip = hit.clip;
        pressVolume(hit, m_pressMods);
        return;
    }
    if (hit.clip) {
        pressClip(hit, m_pressMods);
        return;
    }
    m_drag = Drag::Marquee;
    m_marqueeBase.clear();
    if (m_pressMods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier))
        m_marqueeBase = m_controller->selection();
    else
        m_controller->clearSelection();
}

void TimelineView::pressClip(const Hit& hit, Qt::KeyboardModifiers mods) {
    m_pressHit = hit;
    m_pressClip = hit.clip;
    m_pendingKind = gestureFor(hit, mods);
    m_pressWasSelected = m_controller->isSelected(hit.clip);
    const bool links = followLinks(mods);
    if (mods & Qt::ShiftModifier)
        m_controller->selectClipById(hit.clip, ProjectController::Toggle, links);
    else if (!m_pressWasSelected)
        m_controller->selectClipById(hit.clip,
                                     m_pendingKind == GestureKind::Move && (mods & Qt::ControlModifier)
                                         ? ProjectController::Add
                                         : ProjectController::Replace,
                                     links);
    m_drag = m_pendingKind == GestureKind::None ? Drag::Click : Drag::Pending;
    // Holding a clip's body says it will move (Ctrl: insert); an edge keeps its trim cursor.
    if (m_pendingKind == GestureKind::Move)
        applyCursor((mods & Qt::ControlModifier) ? QStringLiteral("insert") : QStringLiteral("move"));
}

void TimelineView::mouseMoveEvent(QMouseEvent* event) {
    event->accept();
    const QPointF pos = event->position();
    switch (m_drag) {
    case Drag::Marquee: {
        if (!m_controller)
            return;
        if (!m_marqueeActive
            && (pos - m_pressPos).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
            return;
        if (!m_marqueeActive) {
            m_marqueeActive = true;
            emit marqueeChanged();
        }
        updateMarquee(pos);
        updateAutoScroll(pos);
        return;
    }
    case Drag::Pending:
        if ((pos - m_pressPos).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
            return;
        if (!startGesture()) {
            m_drag = Drag::Cancelled;
            return;
        }
        m_drag = Drag::Gesture;
        updateGesture(pos, event->modifiers());
        return;
    case Drag::Gesture:
        m_lastMods = event->modifiers();
        updateGesture(pos, event->modifiers());
        updateAutoScroll(pos);
        return;
    case Drag::TransitionEdge:
        dragTransitionEdge(pos);
        return;
    case Drag::VolumeKey:
    case Drag::VolumeLine:
        dragVolume(pos, event->modifiers());
        return;
    case Drag::Speed:
        dragSpeed(pos);
        return;
    case Drag::Remix:
        if (m_remix && m_controller) {
            const SequencePtr seq = m_controller->currentSequence();
            const Rate rate = seq ? seq->frameRate : kRate30;
            // At least a second of music; on a frame.
            const Ticks target = snapToFrame(std::max(ticksAtX(pos.x()), m_remix->start + Ticks::fromSeconds(1)), rate);
            if (target != m_remix->target) {
                m_remix->target = target;
                ++m_marqueeRevision;
                update();
            }
            updateAutoScroll(pos);
        }
        return;
    default:
        return;
    }
}

void TimelineView::updateMarquee(QPointF pos) {
    m_marquee = QRectF(m_pressPos, pos).normalized();
    ++m_marqueeRevision;
    std::vector<ClipId> next = m_marqueeBase;
    std::unordered_set<std::uint64_t> seen;
    for (ClipId id : next)
        seen.insert(id.value);
    for (ClipId id : clipsInRect(m_marquee)) {
        if (seen.insert(id.value).second)
            next.push_back(id);
    }
    m_controller->setSelection(std::move(next));
    update();
}

// ---- Auto-scroll --------------------------------------------------------------------------------------------------

void TimelineView::updateAutoScroll(QPointF pos) {
    m_lastPointer = pos;
    const double vx = autoScrollVelocity(pos.x(), width(), m_style.autoScrollZone, m_style.autoScrollSpeed);
    const double vy = m_contentHeight > height()
                          ? autoScrollVelocity(pos.y(), height(), m_style.autoScrollZone, m_style.autoScrollSpeed)
                          : 0.0;
    if (!m_autoScrollTimer) {
        m_autoScrollTimer = new QTimer(this);
        m_autoScrollTimer->setInterval(16);
        m_autoScrollTimer->setTimerType(Qt::PreciseTimer);
        connect(m_autoScrollTimer, &QTimer::timeout, this, [this] {
            const double seconds = std::clamp(static_cast<double>(m_autoScrollClock.restart()) / 1000.0, 0.001, 0.05);
            stepAutoScroll(seconds);
        });
    }
    if ((vx != 0 || vy != 0) && !m_autoScrollTimer->isActive()) {
        m_autoScrollClock.start();
        m_autoScrollTimer->start();
        emit autoScrollingChanged();
    }
}

void TimelineView::stepAutoScroll(double seconds) {
    const bool dragging = m_drag == Drag::Gesture || m_drag == Drag::Marquee;
    const double vx = dragging ? autoScrollVelocity(m_lastPointer.x(), width(), m_style.autoScrollZone, m_style.autoScrollSpeed) : 0.0;
    const double vy = dragging && m_contentHeight > height()
                          ? autoScrollVelocity(m_lastPointer.y(), height(), m_style.autoScrollZone, m_style.autoScrollSpeed)
                          : 0.0;
    // Smooth: the velocity follows the pointer's depth into the edge zone over ~80 ms.
    const double follow = std::min(1.0, seconds * 12.0);
    m_scrollVelocity += (QPointF(vx, vy) - m_scrollVelocity) * follow;
    if (vx == 0 && vy == 0 && std::abs(m_scrollVelocity.x()) < 5 && std::abs(m_scrollVelocity.y()) < 5) {
        m_scrollVelocity = {};
        if (m_autoScrollTimer && m_autoScrollTimer->isActive()) {
            m_autoScrollTimer->stop();
            emit autoScrollingChanged();
        }
        return;
    }
    const double oldX = m_scrollX;
    const double oldY = m_scrollY;
    setScrollX(m_scrollX + m_scrollVelocity.x() * seconds);
    setScrollY(m_scrollY + m_scrollVelocity.y() * seconds);
    const QPointF moved(m_scrollX - oldX, m_scrollY - oldY);
    if (moved.isNull())
        return;
    // The press point stays fixed in the content: the drag continues as if the pointer had moved.
    m_pressPos -= moved;
    if (m_drag == Drag::Gesture)
        updateGesture(m_lastPointer, m_lastMods);
    else if (m_drag == Drag::Marquee)
        updateMarquee(m_lastPointer);
}

QString TimelineView::newTrackLabel() const {
    if (!m_gesture.active || m_gesture.spec.newAudioTracks <= 0)
        return {};
    return QStringLiteral("A%1").arg(m_modelAudioRows + 1);
}

void TimelineView::revealClip(const QString& clipId) {
    const auto id = parseClipKey(clipId);
    if (!id || !m_sequence)
        return;
    const auto loc = m_sequence->findClip(*id);
    if (!loc)
        return;
    const double x0 = xOfTicks(loc->clip().start());
    const double x1 = xOfTicks(loc->clip().end());
    if (x0 < 0 || x0 > width() * 0.9)
        setScrollX(m_scrollX + x0 - width() * 0.1);
    else if (x1 > width() && x1 - x0 < width() * 0.8)
        setScrollX(m_scrollX + x1 - width() * 0.9);
    for (const Row& row : m_rows) {
        if (row.track.get() != loc->track)
            continue;
        if (row.y < m_scrollY)
            setScrollY(row.y);
        else if (row.y + row.height > m_scrollY + height())
            setScrollY(row.y + row.height - height());
    }
}

void TimelineView::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
    if (m_drag == Drag::Speed && event->buttons() != Qt::NoButton)
        return; // another button released during a speed drag: the drag goes on (review P2-43)
    if (m_autoScrollTimer && m_autoScrollTimer->isActive()) {
        m_autoScrollTimer->stop();
        m_scrollVelocity = {};
        emit autoScrollingChanged();
    }
    switch (m_drag) {
    case Drag::Marquee:
        if (m_marqueeActive) {
            m_marqueeActive = false;
            ++m_marqueeRevision;
            emit marqueeChanged();
            update();
        }
        break;
    case Drag::Gesture:
        finishGesture();
        break;
    case Drag::TransitionEdge:
        m_transitionDrag = {};
        if (m_controller)
            m_controller->endPropertyGesture(); // one undo step per edge drag
        break;
    case Drag::VolumeKey:
    case Drag::VolumeLine:
        releaseVolume();
        break;
    case Drag::Speed:
        releaseSpeed(event->position());
        break;
    case Drag::Remix:
        if (m_remix) {
            const RemixDrag r = *m_remix;
            m_remix.reset();
            ++m_marqueeRevision;
            update();
            if (r.target != r.end)
                emit remixRequested(clipKey(r.clip), (r.target - r.start).toSeconds(), event->position().x(), event->position().y());
        }
        break;
    case Drag::Pending:
    case Drag::Click:
        // A click (no drag) on a clip edge selects that edit point for trimming (M5i, TrimBridge): the Selection
        // tool a regular trim (Ctrl/Cmd: ripple), the Ripple and Roll tools theirs.
        if (m_controller && m_pressClip && (m_pressHit.zone == Zone::Head || m_pressHit.zone == Zone::Tail)
            && (m_tool == TimelineTool::Select || m_tool == TimelineTool::Ripple || m_tool == TimelineTool::Roll)) {
            const QString type = m_tool == TimelineTool::Roll     ? QStringLiteral("roll")
                                 : m_tool == TimelineTool::Ripple ? QStringLiteral("ripple")
                                 : (m_pressMods & Qt::ControlModifier) ? QStringLiteral("ripple")
                                                                        : QStringLiteral("regular");
            emit editPointClicked(clipKey(m_pressClip),
                                  m_pressHit.zone == Zone::Head ? QStringLiteral("head") : QStringLiteral("tail"), type,
                                  static_cast<int>(m_pressMods.toInt()));
            break;
        }
        // A click (no drag) on a clip that was already selected: Ctrl/Cmd deselects it, a plain click narrows a
        // multi-selection to it (Premiere).
        if (m_controller && m_pressClip && m_pressWasSelected && !(m_pressMods & Qt::ShiftModifier)
            && m_tool != TimelineTool::TrackForward && m_tool != TimelineTool::TrackBackward) {
            m_controller->selectClipById(m_pressClip,
                                         (m_pressMods & Qt::ControlModifier) ? ProjectController::Toggle
                                                                              : ProjectController::Replace,
                                         followLinks(m_pressMods));
        }
        break;
    default:
        break;
    }
    m_drag = Drag::None;
    // The press is over: the cursor says again what the pointer is over (an edge, a body, a gap).
    if (contains(event->position()))
        updateHoverCursor(event->position(), event->modifiers());
    else
        applyCursor({});
}

// The pointer entering the lanes already shows what a press there would do (not only from its next move on).
void TimelineView::hoverEnterEvent(QHoverEvent* event) {
    hoverMoveEvent(event);
}

void TimelineView::hoverMoveEvent(QHoverEvent* event) {
    const QPointF pos = event->position();
    setHovered(m_tool == TimelineTool::Razor ? ClipId{} : clipIdAt(pos.x(), pos.y()));
    updateHoverCursor(pos, event->modifiers());
}

void TimelineView::hoverLeaveEvent(QHoverEvent*) {
    setHovered({});
    m_hoverInside = false;
    setHoverEdge({});
    applyCursor({});
    update();
}

void TimelineView::updateHoverCursor(QPointF pos, Qt::KeyboardModifiers mods) {
    if (m_drag == Drag::Gesture)
        return;
    const Hit hit = hitTest(pos.x(), pos.y());
    applyCursor(hoverCursorKey(hit, mods));
    setHoverEdge(hoverEdgeFor(hit, mods));
    m_hoverX = pos.x();
    m_hoverPos = pos;
    m_hoverRow = hit.row;
    m_hoverAllTracks = mods & Qt::ShiftModifier;
    m_hoverInside = true;
    if (m_tool == TimelineTool::Razor)
        update(); // the cut preview line follows the pointer
}

// ---- Edit cursors (0.1.3) -------------------------------------------------------------------------------------------

QString TimelineView::hoverCursorKey(const Hit& hit, Qt::KeyboardModifiers mods) const {
    const bool edge = hit.zone == Zone::Head || hit.zone == Zone::Tail;
    const bool head = hit.zone == Zone::Head;
    const auto trim = [&](bool ripple) {
        return ripple ? (head ? QStringLiteral("rippleHead") : QStringLiteral("rippleTail"))
                      : (head ? QStringLiteral("trimHead") : QStringLiteral("trimTail"));
    };
    switch (m_tool) {
    case TimelineTool::Select:
        if (hit.zone == Zone::VolumeKey || hit.zone == Zone::SpeedKey)
            return QStringLiteral("point");
        if (hit.zone == Zone::VolumeLine || hit.zone == Zone::SpeedLine)
            return QStringLiteral("vertical");
        if (edge) // a transition's edge changes its duration; a clip's trims it (Ctrl: ripple)
            return trim(!hit.transition && (mods & Qt::ControlModifier));
        return {};
    case TimelineTool::TrackForward:
        return hit.row >= 0 ? QStringLiteral("trackForward") : QString();
    case TimelineTool::TrackBackward:
        return hit.row >= 0 ? QStringLiteral("trackBackward") : QString();
    case TimelineTool::Ripple:
        return edge ? trim(true) : QString();
    case TimelineTool::Roll:
        return edge ? QStringLiteral("roll") : QString();
    case TimelineTool::RateStretch:
        return edge ? (head ? QStringLiteral("rateHead") : QStringLiteral("rateTail")) : QString();
    case TimelineTool::Pen:
        // The pen adds a key on the rubber band (the plus) and moves keys.
        if (hit.zone == Zone::VolumeLine)
            return QStringLiteral("penAdd");
        return hit.clip ? QStringLiteral("pen") : QString();
    case TimelineTool::Slip:
        return hit.clip ? QStringLiteral("slip") : QString();
    case TimelineTool::Slide:
        return hit.clip ? QStringLiteral("slide") : QString();
    case TimelineTool::Razor:
        return hit.row >= 0 ? QStringLiteral("razor") : QString();
    case TimelineTool::Remix:
        if (hit.zone == Zone::Tail && hit.row >= 0 && static_cast<std::size_t>(hit.row) < m_rows.size()
            && m_rows[static_cast<std::size_t>(hit.row)].audio)
            return QStringLiteral("remix");
        return {};
    }
    return {};
}

QString TimelineView::gestureCursorKey(Qt::KeyboardModifiers mods) const {
    const bool head = m_gesture.spec.edge == edit::Edge::Head;
    switch (m_gesture.spec.kind) {
    case GestureKind::Move:
        return (mods & Qt::ControlModifier) ? QStringLiteral("insert") : QStringLiteral("move");
    case GestureKind::Trim:
        return head ? QStringLiteral("trimHead") : QStringLiteral("trimTail");
    case GestureKind::RippleTrim:
        return head ? QStringLiteral("rippleHead") : QStringLiteral("rippleTail");
    case GestureKind::Roll:
        return QStringLiteral("roll");
    case GestureKind::Slip:
        return QStringLiteral("slip");
    case GestureKind::Slide:
        return QStringLiteral("slide");
    case GestureKind::RateStretch:
        return head ? QStringLiteral("rateHead") : QStringLiteral("rateTail");
    case GestureKind::None:
        break;
    }
    return {};
}

void TimelineView::applyCursor(const QString& key) {
    if (key == m_cursorKey && m_cursorSet)
        return;
    const bool changed = key != m_cursorKey;
    m_cursorKey = key;
    m_cursorSet = true;
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    setCursor(editcursors::cursor(key, dpr, EditCursorColors{m_style.trimRollColor, m_style.trimRippleColor}));
    if (changed)
        emit cursorKeyChanged();
}

TimelineView::HoverEdge TimelineView::hoverEdgeFor(const Hit& hit, Qt::KeyboardModifiers mods) const {
    HoverEdge edge;
    if (!hit.clip || hit.transition || (hit.zone != Zone::Head && hit.zone != Zone::Tail))
        return edge;
    switch (m_tool) {
    case TimelineTool::Select:
        edge.kind = (mods & Qt::ControlModifier) ? EdgeKind::Ripple : EdgeKind::Trim;
        break;
    case TimelineTool::Ripple:
        edge.kind = EdgeKind::Ripple;
        break;
    case TimelineTool::Roll:
        edge.kind = EdgeKind::Roll;
        break;
    case TimelineTool::RateStretch:
        edge.kind = EdgeKind::Rate;
        break;
    case TimelineTool::Remix:
        if (hit.zone == Zone::Tail && hit.row >= 0 && static_cast<std::size_t>(hit.row) < m_rows.size()
            && m_rows[static_cast<std::size_t>(hit.row)].audio)
            edge.kind = EdgeKind::Remix;
        break;
    default:
        break;
    }
    if (edge.kind == EdgeKind::None)
        return edge;
    edge.clip = hit.clip;
    edge.row = hit.row;
    edge.head = hit.zone == Zone::Head;
    return edge;
}

void TimelineView::setHoverEdge(const HoverEdge& edge) {
    if (edge == m_hoverEdge)
        return;
    m_hoverEdge = edge;
    update();
}

void TimelineView::modifiersChanged(Qt::KeyboardModifiers mods) {
    if (m_drag == Drag::Gesture) {
        // Ctrl turns a move into an insert move (and back) without waiting for the pointer to move.
        m_lastMods = mods;
        updateGesture(m_lastPointer, mods);
        return;
    }
    if (m_drag == Drag::None && m_hoverInside)
        updateHoverCursor(m_hoverPos, mods);
}

// ---- Keyboard -----------------------------------------------------------------------------------------------------

bool TimelineView::event(QEvent* event) {
    // Esc belongs to a drag in progress, before any shortcut.
    if (event->type() == QEvent::ShortcutOverride && (m_gesture.active || m_drag == Drag::Speed || m_drag == Drag::Remix)
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

namespace {

bool isModifierKey(int key) {
    return key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Shift || key == Qt::Key_Meta;
}

} // namespace

void TimelineView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && cancelGesture()) {
        event->accept();
        return;
    }
    if (isModifierKey(event->key()))
        modifiersChanged(event->modifiers());
    QQuickItem::keyPressEvent(event);
}

void TimelineView::keyReleaseEvent(QKeyEvent* event) {
    if (isModifierKey(event->key()))
        modifiersChanged(event->modifiers());
    QQuickItem::keyReleaseEvent(event);
}

// ---- Tools without drags --------------------------------------------------------------------------------------------

void TimelineView::razorClick(double x, int row, Qt::KeyboardModifiers mods) {
    if (!m_controller || row < 0)
        return;
    const Ticks at = snappedTimeAt(x);
    m_controller->razorAt(m_rows[static_cast<std::size_t>(row)].track->id, at, mods & Qt::ShiftModifier,
                          !followLinks(mods));
}

void TimelineView::trackSelect(double x, int row, bool backward, bool oneTrack) {
    if (!m_controller || !m_sequence)
        return;
    const Ticks t = ticksAtX(x);
    std::vector<ClipId> ids;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (oneTrack && static_cast<int>(i) != row)
            continue;
        const ClipList& clips = m_rows[i].track->clips;
        if (backward) {
            for (auto it = clips.begin(); it != clips.end() && it->start() < t; ++it)
                ids.push_back(it->id);
        } else {
            for (auto it = clips.iteratorAt(firstEndingAfter(clips, t)); it != clips.end(); ++it)
                ids.push_back(it->id);
        }
    }
    if (followLinks(m_pressMods))
        ids = edit::withLinkedClips(*m_sequence, ids);
    m_controller->setSelection(std::move(ids));
}

// ---- Gestures -----------------------------------------------------------------------------------------------------

bool TimelineView::startGesture() {
    if (!m_controller || !m_sequence || m_pendingKind == GestureKind::None)
        return false;
    const Sequence& seq = *m_sequence;
    Gesture g;
    g.spec.kind = m_pendingKind;
    g.spec.links = followLinks(m_pressMods) ? edit::LinkMode::FollowLinks : edit::LinkMode::IgnoreLinks;
    const bool multi = g.spec.kind == GestureKind::Move || g.spec.kind == GestureKind::Slip;
    std::vector<ClipId> clips = multi ? m_controller->selection() : std::vector<ClipId>{};
    ClipId grabbed = m_pressClip;
    if (!grabbed && !clips.empty())
        grabbed = clips.front();
    if (!grabbed)
        return false;
    std::erase(clips, grabbed);
    clips.insert(clips.begin(), grabbed);
    g.spec.clips = clips;
    g.grabbed = grabbed;
    const auto loc = seq.findClip(grabbed);
    const auto position = loc ? seq.trackPosition(loc->track->id) : std::nullopt;
    if (!position)
        return false;
    g.grabbedVideo = position->kind == TrackKind::Video;
    g.grabbedTrackIndex = static_cast<int>(position->index);
    const Clip& grabbedClip = loc->clip();
    if (g.spec.kind == GestureKind::Trim || g.spec.kind == GestureKind::RippleTrim || g.spec.kind == GestureKind::Roll
        || g.spec.kind == GestureKind::RateStretch)
        g.spec.edge = m_pressHit.zone == Zone::Head ? edit::Edge::Head : edit::Edge::Tail;

    // The material the gesture moves: the clips plus (unless Alt) their linked partners.
    const std::vector<ClipId> involved =
        g.spec.links == edit::LinkMode::FollowLinks ? edit::withLinkedClips(seq, g.spec.clips) : g.spec.clips;
    std::unordered_set<std::uint64_t> moving;
    for (ClipId id : involved)
        moving.insert(id.value);

    // One pass: extent of the dragged material and ghost outlines of what is around the view.
    const double margin = std::max(1.0, width());
    const Ticks view0 = ticksAtX(-margin);
    const Ticks view1 = ticksAtX(width() + margin);
    Ticks first = Ticks::max();
    Ticks last = Ticks::min();
    for (std::size_t row = 0; row < m_rows.size(); ++row) {
        for (const Clip& c : m_rows[row].track->clips) {
            if (!moving.contains(c.id.value))
                continue;
            first = std::min(first, c.start());
            last = std::max(last, c.end());
            if (g.spec.kind != GestureKind::Slip && c.end() > view0 && c.start() < view1 && g.ghosts.size() < 512)
                g.ghosts.push_back({c.start(), c.end(), m_rows[row].track->id});
        }
    }

    // Snapping: the moving edges, against every other edge, the playhead, marks and markers.
    switch (g.spec.kind) {
    case GestureKind::Move:
        g.edges = {grabbedClip.start(), grabbedClip.end(), first, last};
        break;
    case GestureKind::Slide:
        g.edges = {grabbedClip.start(), grabbedClip.end()};
        break;
    case GestureKind::Trim:
    case GestureKind::RippleTrim:
    case GestureKind::Roll:
    case GestureKind::RateStretch:
        g.edges = {g.spec.edge == edit::Edge::Head ? grabbedClip.start() : grabbedClip.end()};
        break;
    default:
        break;
    }
    if (!g.edges.empty()) {
        edit::SnapOptions options;
        options.exclude = involved;
        g.targets = edit::snapTargets(seq, m_playhead, options);
        // The dragged edges' own starting positions are not targets (a drag would stick to where it began).
        for (Ticks e : g.edges)
            std::erase(g.targets, e);
    }
    g.active = true;
    m_gesture = std::move(g);
    setHoverEdge({});
    applyCursor(gestureCursorKey(m_pressMods));
    m_modelRowsBottom = m_rows.empty() ? 0.0 : m_rows.back().y + m_rows.back().height;
    m_modelAudioRows = static_cast<int>(std::count_if(m_rows.begin(), m_rows.end(), [](const Row& r) { return r.audio; }));

    // Dragged clips lift (moves and slides; trims and slips edit in place).
    m_settleAnimation->stop();
    if (m_gesture.spec.kind == GestureKind::Move || m_gesture.spec.kind == GestureKind::Slide) {
        m_lifted = std::move(moving);
        m_liftAmount = 1.0;
        markTracksWith(m_lifted);
    } else {
        m_lifted.clear();
        m_liftAmount = 0;
    }
    emit gestureChanged();
    update();
    return true;
}

void TimelineView::updateGesture(QPointF pos, Qt::KeyboardModifiers mods) {
    if (!m_gesture.active || !m_controller)
        return;
    m_lastPointer = pos; // a modifier change re-runs the gesture here (modifiersChanged)
    Gesture& g = m_gesture;
    const Rate rate = frameRate();
    const double seconds = (pos.x() - m_pressPos.x()) / std::max(1e-6, m_pps);
    Ticks delta = snapToFrame(Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))},
                              rate);
    std::optional<Ticks> snapped;
    if (m_snapping && !g.edges.empty()) {
        const SnapResult snap = snapDelta(g.edges, delta, g.targets, snapTolerance());
        delta = snap.delta;
        snapped = snap.target;
    }

    GestureSpec spec = g.spec;
    spec.delta = spec.kind == GestureKind::Slip ? -delta : delta; // dragging left shows later source frames
    int shift = 0;
    if (spec.kind == GestureKind::Move) {
        spec.moveMode = (mods & Qt::ControlModifier) ? edit::MoveMode::Insert : edit::MoveMode::Overwrite;
        spec.newAudioTracks = 0;
        const int row = nearestRow(pos.y());
        if (!g.grabbedVideo && pos.y() + m_scrollY > m_modelRowsBottom + m_style.trackGap) {
            // Below the last track: the drop creates a new audio track there (Premiere).
            shift = m_modelAudioRows - g.grabbedTrackIndex;
            spec.newAudioTracks = 1;
        } else if (row >= 0) {
            const int videoRows = static_cast<int>(std::count_if(m_rows.begin(), m_rows.end(), [](const Row& r) { return !r.audio; }));
            const bool audioRow = m_rows[static_cast<std::size_t>(row)].audio;
            if (g.grabbedVideo)
                shift = audioRow ? -g.grabbedTrackIndex : (videoRows - 1 - row) - g.grabbedTrackIndex;
            else
                shift = audioRow ? (row - videoRows) - g.grabbedTrackIndex : -g.grabbedTrackIndex;
        }
        spec.tracks = {shift, shift}; // linked partners move outward with it (V up, A down)
    }

    const SequenceId seqId = m_controller->currentSequenceId();
    QElapsedTimer timer;
    timer.start();
    auto run = [&](const GestureSpec& s) {
        return m_controller->previewEdit([&](const ProjectState& state) { return applyGesture(state, seqId, s); });
    };
    auto outcome = run(spec);
    if (!outcome && spec.kind == GestureKind::Move && shift != 0) {
        // The partner has no track to go to: move only the grabbed kind across tracks.
        spec.tracks = g.grabbedVideo ? edit::TrackShift{shift, 0} : edit::TrackShift{0, shift};
        outcome = run(spec);
    }
    if (!outcome && spec.kind == GestureKind::Move && spec.newAudioTracks > 0) {
        // Linked video cannot follow it down: the audio moves alone.
        spec.links = edit::LinkMode::IgnoreLinks;
        outcome = run(spec);
    }
    if (outcome) {
        g.spec = spec;
        g.appliedDelta = outcome->appliedDelta;
        const Ticks requested = spec.kind == GestureKind::Slip ? -delta : delta;
        g.snappedAt = outcome->appliedDelta == requested ? snapped : std::nullopt; // clamped: not on the target
        g.preview = std::make_shared<const ProjectState>(std::move(outcome->state));
        showSequence(g.preview->sequencePtr(seqId));
    }
    m_stats.previewNs = timer.nsecsElapsed();

    // Readout next to the pointer.
    const bool dropFrame = m_sequence && m_sequence->dropFrame;
    if (g.spec.kind == GestureKind::Slip) {
        const auto loc = m_sequence ? m_sequence->findClip(g.grabbed) : std::nullopt;
        if (loc) {
            const Clip& c = loc->clip();
            const Ticks lastFrame = c.sourceOut() - framesToTicks(1, rate);
            m_gestureReadout = tr("In %1 · Out %2")
                                   .arg(QString::fromStdString(ticksToTimecode(c.sourceIn, rate, dropFrame)),
                                        QString::fromStdString(ticksToTimecode(std::max(c.sourceIn, lastFrame), rate, dropFrame)));
        }
    } else {
        const Ticks applied = g.appliedDelta;
        const QString amount = QString::fromStdString(ticksToTimecode(applied.value < 0 ? -applied : applied, rate, dropFrame));
        m_gestureReadout = (applied.value < 0 ? QStringLiteral("−") : QStringLiteral("+")) + amount;
        if (g.spec.kind == GestureKind::Move && g.spec.moveMode == edit::MoveMode::Insert)
            m_gestureReadout = tr("Insert %1").arg(m_gestureReadout);
    }
    m_gesturePoint = pos;
    applyCursor(gestureCursorKey(mods));
    emit gestureChanged();
    update();
}

void TimelineView::finishGesture() {
    Gesture g = std::move(m_gesture);
    m_gesture = {};
    emit gestureChanged();
    if (!m_controller)
        return;
    const SequencePtr model = m_controller->currentSequence();
    bool committed = false;
    if (g.preview) { // a preview of no change commits nothing (no undo step)
        const SequenceId seqId = m_controller->currentSequenceId();
        const GestureSpec spec = g.spec;
        committed = m_controller->commitGesture(gestureLabel(spec), [seqId, spec](const ProjectState& state) {
            return applyGesture(state, seqId, spec);
        });
    }
    if (!committed) {
        // Nothing changed (no movement, or the model refused): back to the model, sliding.
        const SequencePtr shown = m_sequence;
        if (shown && model && shown != model)
            animateChange(*shown, *model);
        showSequence(model);
    }
    // Drop: lifted clips settle with a spring.
    if (!m_lifted.empty()) {
        markTracksWith(m_lifted);
        startAnimation(m_settleAnimation, m_style.settleDuration);
    }
    update();
}

bool TimelineView::cancelGesture() {
    if (m_drag == Drag::Remix && m_remix) {
        m_remix.reset();
        m_drag = Drag::Cancelled;
        ++m_marqueeRevision;
        update();
        return true;
    }
    if (m_drag == Drag::Speed) {
        m_drag = Drag::Cancelled;
        m_speedDrag = {};
        m_speedReadout.clear();
        emit gestureChanged();
        return true;
    }
    if (!m_gesture.active)
        return false;
    m_gesture = {};
    if (m_drag == Drag::Gesture)
        m_drag = Drag::Cancelled; // ignore the rest of this drag
    if (m_controller) {
        const SequencePtr shown = m_sequence;
        const SequencePtr model = m_controller->currentSequence();
        if (shown && model && shown != model)
            animateChange(*shown, *model);
        showSequence(model);
    }
    if (!m_lifted.empty()) {
        markTracksWith(m_lifted);
        startAnimation(m_settleAnimation, m_style.settleDuration);
    }
    emit gestureChanged();
    update();
    return true;
}

// ---- Transitions (M5d) ----------------------------------------------------------------------------------------------

std::optional<TimeRange> TimelineView::transitionRange(const Track& track, const Transition& t) {
    Ticks cut;
    if (const Clip* a = t.outgoing ? track.findClip(t.outgoing) : nullptr)
        cut = a->end();
    else if (const Clip* b = t.incoming ? track.findClip(t.incoming) : nullptr)
        cut = b->start();
    else
        return std::nullopt;
    return TimeRange{cut - t.before, t.duration()};
}

TimelineView::Hit TimelineView::transitionHit(double x, int row) const {
    Hit hit;
    hit.row = row;
    if (row < 0 || row >= static_cast<int>(m_rows.size()) || m_tool != TimelineTool::Select)
        return hit;
    const Track& track = *m_rows[static_cast<std::size_t>(row)].track;
    for (const Transition& t : track.transitions) {
        const auto range = transitionRange(track, t);
        if (!range)
            continue;
        const double x0 = xOfTicks(range->start);
        const double x1 = xOfTicks(range->end());
        if (x < x0 - 1 || x > x1 + 1)
            continue;
        const double reach = std::min(m_style.trimHandleWidth, (x1 - x0) / 3.0);
        hit.transition = t.id;
        hit.zone = x - x0 <= reach ? Zone::Head : x1 - x <= reach ? Zone::Tail : Zone::Body;
        // The edge at the cut of a one-sided transition does not resize it.
        if ((hit.zone == Zone::Head && t.before == Ticks{}) || (hit.zone == Zone::Tail && t.after == Ticks{}))
            hit.zone = Zone::Body;
        return hit;
    }
    return hit;
}

QString TimelineView::transitionAt(double x, double y) const {
    return transitionKey(transitionHit(x, rowAt(y)).transition);
}

void TimelineView::dragTransitionEdge(QPointF pos) {
    if (!m_controller)
        return;
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = m_controller->findTransition(m_transitionDrag.id);
    if (!seq || !loc)
        return;
    const Transition& t = *loc->transition;
    const auto range = transitionRange(*loc->track, t);
    if (!range)
        return;
    const Ticks cut = range->start + t.before;
    const Ticks frame = framesToTicks(1, seq->frameRate);
    const Ticks at = snapToFrame(ticksAtX(pos.x()) + Ticks{frame.value / 2}, seq->frameRate);
    edit::TransitionChange change;
    if (m_transitionDrag.left) {
        Ticks before = std::max(Ticks{}, cut - at);
        if (const Clip* a = loc->track->findClip(t.outgoing))
            before = std::min(before, a->duration()); // the part before the cut lies over the outgoing clip
        before = std::max(before, frame - t.after);
        if (before == t.before)
            return;
        change.before = before;
    } else {
        Ticks after = std::max(Ticks{}, at - cut);
        if (const Clip* b = loc->track->findClip(t.incoming))
            after = std::min(after, b->duration());
        after = std::max(after, frame - t.before);
        if (after == t.after)
            return;
        change.after = after;
    }
    const SequenceId seqId = seq->id;
    const TransitionId id = t.id;
    // Past the media's handles the transition stops growing (the model shortens it to what there is).
    m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Transition Duration"),
        [&](const ProjectState& state) {
            return edit::setTransition(state, seqId, id, change, edit::HandleFit::UseAvailable);
        },
        m_transitionDrag.mergeKey);
}

std::optional<TimelineView::EdgeTarget> TimelineView::edgeTargetAt(double x, int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size()))
        return std::nullopt;
    const Track& track = *m_rows[static_cast<std::size_t>(row)].track;
    const Ticks t = ticksAtX(x);
    constexpr double kReach = 48.0; // px: a drop this close to a clip edge goes to that edit point
    std::optional<EdgeTarget> best;
    double bestDistance = kReach;
    const auto consider = [&](const Clip& c, edit::Edge edge) {
        const double ex = xOfTicks(edge == edit::Edge::Head ? c.start() : c.end());
        const double distance = std::abs(ex - x);
        if (distance <= bestDistance) {
            best = EdgeTarget{c.id, edge};
            bestDistance = distance;
        }
    };
    const std::size_t next = track.clips.upperBoundStart(t);
    if (next < track.clips.size())
        consider(track.clips[next], edit::Edge::Head);
    if (next > 0) {
        consider(track.clips[next - 1], edit::Edge::Tail);
        consider(track.clips[next - 1], edit::Edge::Head);
    }
    // Over the middle of a clip: its nearer end (Premiere applies a transition dropped on a clip at that end).
    if (!best) {
        if (const auto i = track.clipIndexAt(t)) {
            const Clip& c = track.clips[*i];
            best = EdgeTarget{c.id, x - xOfTicks(c.start()) < xOfTicks(c.end()) - x ? edit::Edge::Head : edit::Edge::Tail};
        }
    }
    return best;
}

TimelineView::DropPlan TimelineView::planDrop(double x, double y, const QString& effectId) const {
    DropPlan plan;
    const EffectDescriptor* desc = EffectRegistry::builtin().find(effectId.toStdString());
    if (!m_controller || !desc || desc->intrinsic) {
        plan.message = tr("Unknown effect");
        return plan;
    }
    const QString name = QCoreApplication::translate("wabcut::app::EffectControlsModel", desc->displayName.data());
    const int row = rowAt(y);
    const bool transition = desc->kind == EffectKind::VideoTransition || desc->kind == EffectKind::AudioTransition;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return plan;
    if (!transition) {
        plan.kind = QStringLiteral("clip");
        const ClipId clip = clipIdAt(x, y);
        const auto loc = clip ? seq->findClip(clip) : std::nullopt;
        if (!loc || row < 0) {
            plan.message = tr("Drop %1 on a clip").arg(name);
            return plan;
        }
        const StreamKind want = desc->kind == EffectKind::Video ? StreamKind::Video : StreamKind::Audio;
        bool fits = loc->clip().stream.kind == want;
        if (!fits) {
            const ClipId ids[] = {clip};
            for (ClipId partner : edit::withLinkedClips(*seq, ids)) {
                const auto p = seq->findClip(partner);
                fits = fits || (p && p->clip().stream.kind == want);
            }
        }
        if (!fits) {
            plan.message = want == StreamKind::Video ? tr("%1 is a video effect: drop it on a video clip").arg(name)
                                                     : tr("%1 is an audio effect: drop it on an audio clip").arg(name);
            return plan;
        }
        const Row& r = m_rows[static_cast<std::size_t>(row)];
        const double x0 = xOfTicks(loc->clip().start());
        plan.rect = QRectF(x0, r.y - m_scrollY, xOfTicks(loc->clip().end()) - x0, r.height);
        plan.clip = clip;
        plan.ok = true;
        plan.message = tr("Add %1").arg(name);
        return plan;
    }

    plan.kind = QStringLiteral("edit");
    if (row < 0) {
        plan.message = tr("Drop %1 on an edit point").arg(name);
        return plan;
    }
    const Row& r = m_rows[static_cast<std::size_t>(row)];
    if ((desc->kind == EffectKind::VideoTransition) == r.audio) {
        plan.message = desc->kind == EffectKind::VideoTransition
                           ? tr("%1 is a video transition: drop it on a video track").arg(name)
                           : tr("%1 is an audio transition: drop it on an audio track").arg(name);
        return plan;
    }
    const auto target = edgeTargetAt(x, row);
    if (!target) {
        plan.message = tr("Drop %1 on an edit point").arg(name);
        return plan;
    }
    const ProjectSnapshot snap = m_controller->snapshot();
    const Ticks second = framesToTicks(timecodeBase(seq->frameRate), seq->frameRate);
    auto spec = edit::transitionAtEdge(*snap, seq->id, target->clip, target->edge, second,
                                       edit::TransitionAlignment::Center, std::string(desc->id));
    if (!spec) {
        plan.message = QString::fromStdString(spec.error().message);
        return plan;
    }
    // Dry run: as asked, else shortened to the media there is.
    const SequenceId seqId = seq->id;
    const auto attempt = [&](edit::HandleFit fit) {
        return m_controller->previewEdit([&](const ProjectState& state) {
            return edit::addTransition(state, seqId, spec->track, spec->spec, fit);
        });
    };
    auto result = attempt(edit::HandleFit::Exact);
    if (!result) {
        const QString exactError = QString::fromStdString(result.error().message);
        result = attempt(edit::HandleFit::UseAvailable);
        if (!result) {
            plan.message = exactError;
            return plan;
        }
        plan.fit = edit::HandleFit::UseAvailable;
        plan.warning = true;
    }
    // Where the new transition sits (the model may have shortened it).
    Ticks before = spec->spec.before, after = spec->spec.after;
    if (!result->createdTransitions.empty()) {
        if (const Sequence* next = result->state.findSequence(seqId)) {
            if (const Track* track = next->findTrack(spec->track)) {
                for (const Transition& t : track->transitions) {
                    if (t.id == result->createdTransitions.front()) {
                        before = t.before;
                        after = t.after;
                    }
                }
            }
        }
    }
    const Clip* clip = r.track->findClip(target->clip);
    const Ticks cut = clip ? (target->edge == edit::Edge::Head ? clip->start() : clip->end()) : Ticks{};
    const double x0 = xOfTicks(cut - before);
    plan.rect = QRectF(x0, r.y - m_scrollY, std::max(2.0, xOfTicks(cut + after) - x0), r.height);
    const QString length = m_controller->formatTimecode(before + after);
    plan.message = plan.warning ? tr("%1 · %2, using the available handles").arg(name, length)
                                : tr("%1 · %2").arg(name, length);
    plan.transition = std::move(*spec);
    plan.ok = true;
    return plan;
}

QVariantMap TimelineView::effectDropTarget(double x, double y, const QString& effectId) const {
    const DropPlan plan = planDrop(x, y, effectId);
    return {
        {QStringLiteral("ok"), plan.ok},
        {QStringLiteral("warning"), plan.warning},
        {QStringLiteral("kind"), plan.kind},
        {QStringLiteral("message"), plan.message},
        {QStringLiteral("x"), plan.rect.x()},
        {QStringLiteral("y"), plan.rect.y()},
        {QStringLiteral("width"), plan.rect.width()},
        {QStringLiteral("height"), plan.rect.height()},
    };
}

QString TimelineView::dropEffect(double x, double y, const QString& effectId) {
    const DropPlan plan = planDrop(x, y, effectId);
    if (!plan.ok || !m_controller)
        return plan.message;
    if (plan.kind == QLatin1String("clip"))
        return m_controller->effectControls()->addEffectToClip(clipKey(plan.clip), effectId);
    const SequenceId seqId = m_controller->currentSequenceId();
    const edit::EdgeTransition spec = *plan.transition;
    const edit::HandleFit fit = plan.fit;
    std::optional<TransitionId> created;
    const bool ok = m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Transition"),
                                              [&](const ProjectState& state) {
                                                  auto r = edit::addTransition(state, seqId, spec.track, spec.spec, fit);
                                                  if (r && !r->createdTransitions.empty())
                                                      created = r->createdTransitions.front();
                                                  return r;
                                              });
    if (!ok)
        return m_controller->lastError();
    if (created)
        m_controller->selectTransitionById(*created);
    return {};
}

} // namespace wabcut::app
