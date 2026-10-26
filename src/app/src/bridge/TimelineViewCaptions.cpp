// TimelineView (0.5, P0 round): caption tracks as lanes - Premiere's caption tracks at the top of the Timeline. One
// compact row per caption track of the sequence above the top video track (Show caption tracks), its cues drawn in
// TimelineView.cpp (updateCaptionLayer). Here: the rows and their headers' items, the cue selection (in CaptionsBridge,
// so the Captions page follows it), hit-testing and the edits - trim an edge, move the body (both frame by frame,
// snapping to clip edges, the playhead, marks, markers, beats and the other cues, kept between the neighbours unless the
// style allows overlaps), split with the Razor tool or Add Edit on a targeted caption row, delete; each one undo step.
//
// Product decisions (Premiere is the reference): a click selects the cue and moves the playhead to its start (on the
// release, so a drag does not move it); the Selection tool alone drags cues, the Razor tool splits them, other tools only
// select; Ripple Delete removes a cue like Delete (captions never ripple the clips); caption targeting is view state, off
// by default, so Ctrl+K does not cut captions until a caption row is targeted; with a selection Ctrl+K cuts only the
// selected items, as Premiere does (selected clips: no caption; a selected cue: it alone).

#include "TimelineView.h"

#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "GlobalBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Snap.h>
#include <wabcut/model/Timecode.h>

#include <QGuiApplication>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wabcut::app {

namespace {

const CaptionTrack* findCaptionTrack(const Sequence* sequence, CaptionTrackId id) {
    if (!sequence || !id)
        return nullptr;
    const auto it = std::ranges::find_if(sequence->captionTracks, [&](const CaptionTrack& t) { return t.id == id; });
    return it == sequence->captionTracks.end() ? nullptr : &*it;
}

} // namespace

// ---- Rows ---------------------------------------------------------------------------------------------------------

void TimelineView::setShowCaptionTracks(bool on) {
    if (on == m_showCaptionTracks)
        return;
    m_showCaptionTracks = on;
    if (!on)
        cancelCaptionDrag();
    relayoutRows();
    clampScroll();
    emit displayChanged();
    emit viewChanged();
    update();
}

qreal TimelineView::layoutCaptionRows() {
    std::vector<CaptionRow> rows;
    qreal y = 0;
    if (m_sequence && m_showCaptionTracks) {
        for (const CaptionTrack& t : m_sequence->captionTracks) {
            rows.push_back({t.id, y, m_style.captionTrackHeight, t.enabled});
            y += m_style.captionTrackHeight + m_style.trackGap;
        }
    }
    m_captionRows = std::move(rows);
    return y;
}

void TimelineView::refreshCaptionRowItems() {
    QVariantList items;
    for (std::size_t i = 0; i < m_captionRows.size(); ++i) {
        const CaptionRow& row = m_captionRows[i];
        const CaptionTrack* t = captionTrackOf(row.id);
        if (!t)
            continue;
        items << QVariantMap{{QStringLiteral("id"), captionTrackKey(row.id)},
                             {QStringLiteral("label"), QStringLiteral("C%1").arg(i + 1)},
                             {QStringLiteral("name"), QString::fromStdString(t->name)},
                             {QStringLiteral("language"), QString::fromStdString(t->language)},
                             {QStringLiteral("enabled"), t->enabled},
                             {QStringLiteral("targeted"), m_captionTargets.contains(row.id.value)},
                             {QStringLiteral("y"), row.y},
                             {QStringLiteral("height"), row.height},
                             {QStringLiteral("cues"), static_cast<int>(t->cues.size())}};
    }
    if (items == m_captionRowItems)
        return;
    m_captionRowItems = std::move(items);
    emit captionRowsChanged();
}

int TimelineView::captionRowAt(double y) const {
    const double contentY = y + m_scrollY;
    for (std::size_t i = 0; i < m_captionRows.size(); ++i) {
        if (contentY >= m_captionRows[i].y && contentY < m_captionRows[i].y + m_captionRows[i].height)
            return static_cast<int>(i);
    }
    return -1;
}

const CaptionTrack* TimelineView::captionTrackOf(CaptionTrackId id) const {
    return findCaptionTrack(m_sequence.get(), id);
}

TimelineView::CaptionHit TimelineView::captionHit(double x, double y) const {
    CaptionHit hit;
    hit.row = captionRowAt(y);
    if (hit.row < 0)
        return hit;
    const CaptionTrack* track = captionTrackOf(m_captionRows[static_cast<std::size_t>(hit.row)].id);
    if (!track)
        return hit;
    const Ticks at = ticksAtX(x);
    const double handle = m_style.trimHandleWidth;
    // The cue under the pointer (where overlaps are allowed, the one that starts last), its edges within the trim
    // handle (a third of a narrow cue at most).
    for (std::size_t k = track->cues.size(); k-- > 0;) {
        const CaptionCue& c = track->cues[k];
        if (at < c.start || at >= c.end())
            continue;
        const double x0 = xOfTicks(c.start);
        const double x1 = xOfTicks(c.end());
        const double reach = std::min(handle, (x1 - x0) / 3.0);
        hit.cue = static_cast<int>(k);
        hit.zone = x - x0 <= reach ? Zone::Head : x1 - x <= reach ? Zone::Tail : Zone::Body;
        return hit;
    }
    // In a gap: an edge within reach still counts (the edge zone straddles the cue's border, as on clips).
    double best = handle;
    for (std::size_t k = 0; k < track->cues.size(); ++k) {
        const CaptionCue& c = track->cues[k];
        const double head = xOfTicks(c.start) - x;
        const double tail = x - xOfTicks(c.end());
        if (head >= 0 && head <= best) {
            best = head;
            hit.cue = static_cast<int>(k);
            hit.zone = Zone::Head;
        }
        if (tail >= 0 && tail <= best) {
            best = tail;
            hit.cue = static_cast<int>(k);
            hit.zone = Zone::Tail;
        }
    }
    return hit;
}

QVariantMap TimelineView::captionAt(double x, double y) const {
    const CaptionHit hit = captionHit(x, y);
    if (hit.row < 0 || hit.cue < 0)
        return {};
    return {{QStringLiteral("track"), captionTrackKey(m_captionRows[static_cast<std::size_t>(hit.row)].id)},
            {QStringLiteral("cue"), hit.cue},
            {QStringLiteral("edge"), hit.zone == Zone::Head   ? QStringLiteral("head")
                                     : hit.zone == Zone::Tail ? QStringLiteral("tail")
                                                              : QString()}};
}

QVariantMap TimelineView::captionInfo(const QString& track, int cue) const {
    const auto id = parseCaptionTrackKey(track);
    const CaptionTrack* t = id ? captionTrackOf(*id) : nullptr;
    if (!t || !m_controller || cue < 0 || cue >= static_cast<int>(t->cues.size()))
        return {};
    const CaptionCue& c = t->cues[static_cast<std::size_t>(cue)];
    const auto row = std::ranges::find_if(m_captionRows, [&](const CaptionRow& r) { return r.id == *id; });
    const Ticks playhead = m_controller->playhead();
    return {{QStringLiteral("track"), track},
            {QStringLiteral("cue"), cue},
            {QStringLiteral("label"), row == m_captionRows.end() ? QString() : QStringLiteral("C%1").arg(row - m_captionRows.begin() + 1)},
            {QStringLiteral("text"), QString::fromStdString(c.text).section(QLatin1Char('\n'), 0, 0).trimmed()},
            {QStringLiteral("start"), m_controller->formatTimecode(c.start)},
            {QStringLiteral("end"), m_controller->formatTimecode(c.end())},
            {QStringLiteral("duration"), m_controller->formatTimecode(c.duration)},
            {QStringLiteral("splittable"), c.start < playhead && playhead < c.end()}};
}

void TimelineView::setCaptionHoverEdge(const CaptionHoverEdge& edge) {
    if (edge == m_captionHoverEdge)
        return;
    m_captionHoverEdge = edge;
    update();
}

QString TimelineView::captionCursorKey(const CaptionHit& hit) const {
    if (hit.row < 0)
        return {};
    if (m_tool == TimelineTool::Razor)
        return QStringLiteral("razor");
    if (m_tool == TimelineTool::Select && hit.cue >= 0 && hit.zone == Zone::Head)
        return QStringLiteral("trimHead");
    if (m_tool == TimelineTool::Select && hit.cue >= 0 && hit.zone == Zone::Tail)
        return QStringLiteral("trimTail");
    return {};
}

// ---- Targeting (view state) -----------------------------------------------------------------------------------------

void TimelineView::setCaptionTargeted(const QString& track, bool targeted) {
    const auto id = parseCaptionTrackKey(track);
    if (!id)
        return;
    if (targeted)
        m_captionTargets.insert(id->value);
    else
        m_captionTargets.erase(id->value);
    refreshCaptionRowItems();
}

bool TimelineView::isCaptionTargeted(const QString& track) const {
    const auto id = parseCaptionTrackKey(track);
    return id && m_captionTargets.contains(id->value);
}

// ---- Selection ------------------------------------------------------------------------------------------------------

QObject* TimelineView::captions() const {
    return m_captionsBridge.data();
}

void TimelineView::setCaptions(QObject* captions) {
    if (m_captionsBridge == captions)
        return;
    if (m_captionsBridge)
        disconnect(m_captionsBridge, nullptr, this, nullptr);
    m_captionsBridge = captions;
    if (auto* bridge = qobject_cast<CaptionsBridge*>(captions))
        connect(bridge, &CaptionsBridge::cueSelectionChanged, this, &TimelineView::onCaptionsSelectionChanged);
    m_localCaptionSelection = {};
    onCaptionsSelectionChanged();
    emit captionsChanged();
}

TimelineView::CaptionSelection TimelineView::captionSelection() const {
    if (const auto* bridge = qobject_cast<const CaptionsBridge*>(m_captionsBridge.data())) {
        const int cue = bridge->selectedCue();
        return cue >= 0 ? CaptionSelection{bridge->shownTrackId(), cue} : CaptionSelection{};
    }
    return m_localCaptionSelection;
}

QVariantMap TimelineView::selectedCaptionItem() const {
    const CaptionSelection selection = captionSelection();
    if (!selection.valid())
        return {};
    return {{QStringLiteral("track"), captionTrackKey(selection.track)}, {QStringLiteral("cue"), selection.cue}};
}

void TimelineView::setCaptionSelection(CaptionTrackId track, int cue) {
    if (!track || cue < 0) {
        track = {};
        cue = -1;
    }
    if (auto* bridge = qobject_cast<CaptionsBridge*>(m_captionsBridge.data())) {
        if (track)
            bridge->selectCueOf(track, cue);
        else
            bridge->clearCueSelection();
        return; // its cueSelectionChanged() redraws the lanes
    }
    if (m_localCaptionSelection.track == track && m_localCaptionSelection.cue == cue)
        return;
    m_localCaptionSelection = {track, cue};
    onCaptionsSelectionChanged();
}

void TimelineView::onCaptionsSelectionChanged() {
    ++m_captionRevision;
    emit captionSelectionChanged();
    update();
}

void TimelineView::selectCaptionCue(CaptionTrackId track, int cue) {
    if (m_controller) {
        // A cue and clips (a gap, a transition) are selected one or the other (Premiere).
        m_controller->clearSelection();
        m_controller->selectTransitionById({});
    }
    setCaptionSelection(track, cue);
}

void TimelineView::selectCaption(const QString& track, int cue) {
    const auto id = parseCaptionTrackKey(track);
    if (!id || cue < 0) {
        setCaptionSelection({}, -1);
        return;
    }
    selectCaptionCue(*id, cue);
}

void TimelineView::validateCaptionSelection() {
    // The bridge follows its own cues (CaptionsBridge::refresh); the view's own selection drops a cue that is gone.
    if (m_captionsBridge || !m_localCaptionSelection.valid())
        return;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const CaptionTrack* track = findCaptionTrack(seq.get(), m_localCaptionSelection.track);
    if (!track || m_localCaptionSelection.cue >= static_cast<int>(track->cues.size())) {
        m_localCaptionSelection = {};
        onCaptionsSelectionChanged();
    }
}

// ---- Mouse ----------------------------------------------------------------------------------------------------------

void TimelineView::pressCaption(const CaptionHit& hit, Qt::MouseButton button) {
    m_drag = Drag::None;
    m_captionDrag = {};
    if (!m_controller || hit.row < 0 || hit.row >= static_cast<int>(m_captionRows.size()))
        return;
    const CaptionTrackId track = m_captionRows[static_cast<std::size_t>(hit.row)].id;
    const CaptionTrack* t = captionTrackOf(track);
    if (!t)
        return;
    if (hit.cue < 0 || hit.cue >= static_cast<int>(t->cues.size())) {
        // Empty caption lane: a click deselects everything; never a marquee.
        if (button == Qt::LeftButton) {
            m_controller->clearSelection();
            m_controller->selectTransitionById({});
            setCaptionSelection({}, -1);
        }
        return;
    }
    selectCaptionCue(track, hit.cue);
    if (button == Qt::RightButton) { // the cue menu (QML: Split at Playhead, Delete, Edit Text, Select in Captions)
        emit captionContextMenuRequested(captionTrackKey(track), hit.cue, m_pressPos.x(), m_pressPos.y());
        return;
    }
    if (button != Qt::LeftButton)
        return;
    if (m_tool == TimelineTool::Razor) {
        // The razor cuts the cue where it clicks, on a frame (snapped as it is on clips).
        splitCaptionCue(track, hit.cue, snappedTimeAt(m_pressPos.x()));
        return;
    }
    const CaptionCue& cue = t->cues[static_cast<std::size_t>(hit.cue)];
    m_captionDrag.track = track;
    m_captionDrag.row = hit.row;
    m_captionDrag.cue = hit.cue;
    m_captionDrag.zone = hit.zone;
    m_captionDrag.editable = m_tool == TimelineTool::Select;
    m_captionDrag.start = m_captionDrag.previewStart = cue.start;
    m_captionDrag.end = m_captionDrag.previewEnd = cue.end();
    m_drag = Drag::CaptionPending;
    if (m_captionDrag.editable && hit.zone == Zone::Body)
        applyCursor(QStringLiteral("move"));
}

void TimelineView::dragCaption(QPointF pos, Qt::KeyboardModifiers mods) {
    m_lastPointer = pos;
    m_lastMods = mods;
    CaptionDrag& cueDrag = m_captionDrag;
    if (!m_controller || !m_sequence || !cueDrag.editable)
        return;
    const CaptionTrack* t = captionTrackOf(cueDrag.track);
    if (!t || cueDrag.cue < 0 || cueDrag.cue >= static_cast<int>(t->cues.size())) {
        cancelCaptionDrag();
        return;
    }
    const auto index = static_cast<std::size_t>(cueDrag.cue);
    if (m_drag == Drag::CaptionPending) {
        if ((pos - m_pressPos).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
            return;
        m_drag = Drag::Caption;
        cueDrag.active = true;
        // Snap targets: clip edges, the playhead, marks, markers and beats, and the edges of the other cues; not where
        // the dragged edges start (a drag would stick to where it began).
        std::vector<Ticks> targets = edit::snapTargets(*m_sequence, m_playhead);
        for (const CaptionTrack& other : m_sequence->captionTracks) {
            for (std::size_t k = 0; k < other.cues.size(); ++k) {
                if (other.id == cueDrag.track && k == index)
                    continue;
                targets.push_back(other.cues[k].start);
                targets.push_back(other.cues[k].end());
            }
        }
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        if (cueDrag.zone != Zone::Tail)
            std::erase(targets, cueDrag.start);
        if (cueDrag.zone != Zone::Head)
            std::erase(targets, cueDrag.end);
        cueDrag.targets = std::move(targets);
    }
    const Rate rate = frameRate();
    const double seconds = (pos.x() - m_pressPos.x()) / std::max(1e-6, m_pps);
    Ticks delta = snapToFrame(Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))}, rate);
    std::optional<Ticks> snapped;
    if (m_snapping) {
        std::vector<Ticks> edges;
        if (cueDrag.zone != Zone::Tail)
            edges.push_back(cueDrag.start);
        if (cueDrag.zone != Zone::Head)
            edges.push_back(cueDrag.end);
        const SnapResult snap = snapDelta(edges, delta, cueDrag.targets, snapTolerance());
        delta = snap.delta;
        snapped = snap.target;
    }
    // Kept between the neighbouring cues unless the track's style allows overlaps, never before 0, at least a frame.
    // Fix round 1 (QA #1): cues may already overlap although the style forbids it (a caption preset saved again without
    // overlaps restyles the tracks that follow it, edit::saveCaptionPreset): the bounds never pass the cue as it is, so
    // lo <= start < end <= hi holds and every clamp range below is non-empty; the far bound leaves room for the length,
    // so start + length cannot overflow.
    const bool free = t->style.allowOverlap;
    const Ticks far{Ticks::max().value / 2};
    const Ticks lo = !free && index > 0 ? std::min(t->cues[index - 1].end(), cueDrag.start) : Ticks{};
    const Ticks hi = !free && index + 1 < t->cues.size() ? std::max(t->cues[index + 1].start, cueDrag.end) : far;
    const Ticks frame = framesToTicks(1, rate);
    Ticks start = cueDrag.start;
    Ticks end = cueDrag.end;
    if (cueDrag.zone == Zone::Body) {
        const Ticks length = cueDrag.end - cueDrag.start;
        start = std::clamp(cueDrag.start + delta, lo, std::max(lo, hi - length));
        end = start + length;
    } else if (cueDrag.zone == Zone::Head) {
        start = std::clamp(cueDrag.start + delta, lo, std::max(lo, std::max(cueDrag.start, cueDrag.end - frame)));
    } else {
        const Ticks shortest = std::min(cueDrag.end, cueDrag.start + frame);
        end = std::clamp(cueDrag.end + delta, std::min(shortest, hi), hi);
    }
    const Ticks applied = cueDrag.zone == Zone::Tail ? end - cueDrag.end : start - cueDrag.start;
    if (applied != delta)
        snapped.reset(); // clamped: not on the target
    cueDrag.previewStart = start;
    cueDrag.previewEnd = end;
    cueDrag.snappedAt = snapped;
    // The readout: the move, and for a trim the new duration.
    const bool dropFrame = m_sequence->dropFrame;
    const auto length = [&](Ticks v) { return QString::fromStdString(ticksToTimecode(v, rate, dropFrame)); };
    const Ticks magnitude = applied.value < 0 ? -applied : applied;
    m_gestureReadout = (applied.value < 0 ? QStringLiteral("−") : QStringLiteral("+")) + length(magnitude);
    if (cueDrag.zone != Zone::Body)
        m_gestureReadout = tr("%1 · Duration %2").arg(m_gestureReadout, length(end - start));
    m_gesturePoint = pos;
    applyCursor(cueDrag.zone == Zone::Body ? QStringLiteral("move") : cueDrag.zone == Zone::Head ? QStringLiteral("trimHead") : QStringLiteral("trimTail"));
    ++m_captionRevision;
    emit gestureChanged();
    update();
}

void TimelineView::releaseCaption() {
    const CaptionDrag cueDrag = std::exchange(m_captionDrag, {});
    const bool dragged = m_drag == Drag::Caption && cueDrag.active;
    m_drag = Drag::None; // the drag ends before its edit (onModelChanged would cancel it)
    if (cueDrag.active) {
        m_gestureReadout.clear();
        emit gestureChanged();
    }
    ++m_captionRevision;
    update();
    const CaptionTrack* t = m_controller ? captionTrackOf(cueDrag.track) : nullptr;
    if (!t || cueDrag.cue < 0 || cueDrag.cue >= static_cast<int>(t->cues.size()))
        return;
    const auto index = static_cast<std::size_t>(cueDrag.cue);
    if (!dragged) {
        // A click: the playhead goes to the cue (its start frame), and the Captions page shows it.
        const CaptionCue& cue = t->cues[index];
        m_controller->setPlayhead(seekPointIn(TimeRange{cue.start, cue.duration}, frameRate()));
        return;
    }
    if (cueDrag.previewStart == cueDrag.start && cueDrag.previewEnd == cueDrag.end)
        return; // back where it was: no undo step
    const SequenceId seq = m_controller->currentSequenceId();
    const CaptionTrackId track = cueDrag.track;
    const Ticks start = cueDrag.previewStart;
    const Ticks end = cueDrag.previewEnd;
    const std::string text = t->cues[index].text;
    const char* label = cueDrag.zone == Zone::Body ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Move Caption")
                                             : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Trim Caption");
    const bool ok = m_controller->commitGesture(label, [=](const ProjectState& state) {
        edit::CaptionCueChange change;
        change.start = start;
        change.end = end;
        return edit::updateCaptionCue(state, seq, track, index, change);
    });
    if (!ok)
        return;
    // Where overlaps are allowed the cue may have moved to another place in start order: it stays selected there.
    const SequencePtr now = m_controller->currentSequence();
    if (const CaptionTrack* after = findCaptionTrack(now.get(), track)) {
        for (std::size_t k = 0; k < after->cues.size(); ++k) {
            const CaptionCue& c = after->cues[k];
            if (c.start == start && c.end() == end && c.text == text) {
                setCaptionSelection(track, static_cast<int>(k));
                break;
            }
        }
    }
}

void TimelineView::cancelCaptionDrag() {
    const bool active = m_captionDrag.active;
    m_captionDrag = {};
    if (m_drag == Drag::Caption || m_drag == Drag::CaptionPending)
        m_drag = Drag::Cancelled; // ignore the rest of this drag
    if (active) {
        m_gestureReadout.clear();
        emit gestureChanged();
    }
    ++m_captionRevision;
    update();
}

// ---- Edits ----------------------------------------------------------------------------------------------------------

bool TimelineView::splitCaptionCue(CaptionTrackId track, int cue, Ticks at) {
    if (!m_controller)
        return false;
    const SequencePtr seq = m_controller->currentSequence();
    const CaptionTrack* t = findCaptionTrack(seq.get(), track);
    if (!t || cue < 0 || cue >= static_cast<int>(t->cues.size()))
        return false;
    const auto index = static_cast<std::size_t>(cue);
    if (at <= t->cues[index].start || at >= t->cues[index].end())
        return false; // on an edge or outside: nothing to split
    const SequenceId seqId = seq->id;
    const std::shared_ptr<const LineBreaker> breaker = qtLineBreaker();
    return m_controller->commitGesture(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Split Caption"),
                                       [=](const ProjectState& state) {
                                           return edit::splitCaptionCue(state, seqId, track, index, at, std::nullopt, *breaker);
                                       });
}

bool TimelineView::addEditAtPlayhead() {
    m_captionMessage.clear();
    if (!m_controller)
        return false;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return false;
    const Ticks at = m_controller->playhead();
    const auto under = [&](const CaptionCue& c) { return c.start < at && at < c.end(); };
    // Premiere: with something selected, Add Edit cuts the selected items only (fix round 1, QA #11). Selected clips
    // under the playhead: ProjectController's Add Edit cuts those, the captions stay whole. (Selected clips the playhead
    // misses cut nothing there, so ProjectController::razorAtPlayhead falls back to the targeted tracks, and so does
    // this, with the targeted caption rows.)
    if (std::ranges::any_of(m_controller->selection(), [&](ClipId id) {
            const auto loc = seq->findClip(id);
            return loc && loc->clip().timelineRange.contains(at);
        }))
        return false;
    // A selected cue: it alone, where the playhead is inside it; else nothing is cut, and the panel says why.
    if (const CaptionSelection selected = captionSelection(); selected.valid()) {
        const CaptionTrack* t = findCaptionTrack(seq.get(), selected.track);
        if (!t || selected.cue >= static_cast<int>(t->cues.size()))
            return false;
        if (!under(t->cues[static_cast<std::size_t>(selected.cue)])) {
            m_captionMessage = tr("Put the playhead inside the selected caption to cut it there");
        } else if (!splitCaptionCue(selected.track, selected.cue, at)) {
            m_captionMessage = tr("The caption was not cut: %1").arg(m_controller->lastError());
        }
        return true;
    }
    // Nothing selected: every cue under the playhead on the targeted caption rows (each one where cues overlap).
    std::vector<std::pair<CaptionTrackId, std::size_t>> cuts;
    for (const CaptionRow& row : m_captionRows) {
        if (!m_captionTargets.contains(row.id.value))
            continue;
        const CaptionTrack* t = findCaptionTrack(seq.get(), row.id);
        if (!t)
            continue;
        for (std::size_t k = 0; k < t->cues.size() && t->cues[k].start < at; ++k) {
            if (under(t->cues[k]))
                cuts.emplace_back(row.id, k);
        }
    }
    if (cuts.empty())
        return false; // ProjectController's Add Edit does the clips
    // Later cues first: a split puts its second half after every cue that starts before the playhead, so the indices of
    // the earlier cues stay valid.
    std::ranges::sort(cuts, [](const auto& a, const auto& b) { return a.second > b.second; });
    // With the clips of the targeted tracks (ProjectController::razorAtPlayhead with nothing selected), in one undo step.
    const std::vector<TrackId> tracks = edit::targetedTracks(*seq);
    const SequenceId seqId = seq->id;
    const std::shared_ptr<const LineBreaker> breaker = qtLineBreaker();
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Edit"), [&](const ProjectState& state) -> edit::EditOutcome {
            ProjectState next = state;
            bool any = false;
            if (!tracks.empty()) {
                if (auto clips = edit::addEdit(next, seqId, at, tracks); clips && !clips->changes.empty()) {
                    next = std::move(clips->state);
                    any = true;
                }
            }
            for (const auto& [track, index] : cuts) {
                // A cue whose text has nowhere to split stays whole.
                if (auto split = edit::splitCaptionCue(next, seqId, track, index, at, std::nullopt, *breaker)) {
                    next = std::move(split->state);
                    any = true;
                }
            }
            if (!any)
                return std::unexpected(Error(ErrorCode::InvalidArgument, "nothing to cut under the playhead"));
            edit::EditResult result;
            result.changes = diff(state, next);
            result.state = std::move(next);
            return result;
        });
    if (!ok)
        m_captionMessage = tr("Nothing was cut: %1").arg(m_controller->lastError());
    return true;
}

bool TimelineView::deleteSelectedCaption() {
    const CaptionSelection selected = captionSelection();
    if (!m_controller || !selected.valid())
        return false;
    const SequencePtr seq = m_controller->currentSequence();
    const CaptionTrack* t = findCaptionTrack(seq.get(), selected.track);
    if (!seq || !t || selected.cue >= static_cast<int>(t->cues.size())) {
        setCaptionSelection({}, -1);
        return false;
    }
    const SequenceId seqId = seq->id;
    const CaptionTrackId track = selected.track;
    const auto index = static_cast<std::size_t>(selected.cue);
    setCaptionSelection({}, -1);
    m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Delete Caption"),
                              [=](const ProjectState& state) { return edit::removeCaptionCue(state, seqId, track, index); });
    return true;
}

} // namespace wabcut::app
