// TimelineView's fade handles (0.2, Premiere 25.x/26 timeline fade handles): a selected clip, or the clip under the
// pointer, shows a small handle in each top corner, at the end of its fade in (left) and the start of its fade out
// (right); Show Video / Audio Fade Handles (Timeline Display Settings) hide them per kind. Dragging one with the
// Selection tool sets that fade's length (ClipFades.h: a one-sided Cross Dissolve on video, Constant Power on audio),
// shown in the gesture readout while dragging; dragging it back to the clip's edge removes the fade. One undo step per
// drag. The handles are drawn with the selection overlay (TimelineView.cpp).

#include "TimelineView.h"

#include "TimelineGeometry.h"

#include <wabcut/model/ClipFades.h>

#include <QGuiApplication>
#include <QStyleHints>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

constexpr double kHandle = 7.0;     // px, the handle's side
constexpr double kHandleReach = 6.0; // px around the handle's centre, horizontally
constexpr double kBandHeight = 10.0; // px from the clip's top: the band the handles live in

#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

/// The handle's centre x inside `rect` (the clip's rectangle) for a fade of `fadePx` pixels.
double handleX(const QRectF& rect, bool head, double fadePx) {
    const double half = kHandle / 2;
    const double x = head ? rect.left() + fadePx : rect.right() - fadePx;
    return std::clamp(x, rect.left() + half, std::max(rect.left() + half, rect.right() - half));
}

} // namespace

TimelineView::Hit TimelineView::fadeHit(double x, double y, int rowIndex) const {
    Hit hit;
    if (m_tool != TimelineTool::Select || !m_controller || !m_sequence || rowIndex < 0
        || rowIndex >= static_cast<int>(m_rows.size()))
        return hit;
    const Row& row = m_rows[static_cast<std::size_t>(rowIndex)];
    const double top = row.y - m_scrollY;
    if (y < top || y > top + kBandHeight || row.track->locked)
        return hit;
    // The clip under x, or the one whose edge is just outside it (the handle straddles nothing: it is inside the clip).
    const auto index = row.track->clipIndexAt(ticksAtX(x));
    if (!index)
        return hit;
    const Clip& clip = row.track->clips[*index];
    if (!fadeHandlesShown(clip, row.audio))
        return hit;
    const QRectF rect(xOfTicks(clip.start()), top, xOfTicks(clip.end()) - xOfTicks(clip.start()), row.height);
    if (rect.width() < 4 * kHandle)
        return hit; // too short to tell the handles from the trim edges
    for (const bool head : {true, false}) {
        const auto fade = edit::clipFade(*m_sequence, clip.id, head ? edit::Edge::Head : edit::Edge::Tail);
        const double fadePx = fade ? fade->toSeconds() * m_pps : 0.0;
        if (std::abs(x - handleX(rect, head, fadePx)) <= kHandleReach) {
            hit.row = rowIndex;
            hit.clip = clip.id;
            hit.zone = Zone::FadeHandle;
            hit.fadeHead = head;
            return hit;
        }
    }
    return hit;
}

void TimelineView::addFadeHandles(MeshBuilder& mesh, const QRectF& rect, const Clip& clip) const {
    if (m_tool != TimelineTool::Select || !m_sequence || rect.width() < 4 * kHandle || rect.height() < 2 * kHandle)
        return;
    for (const bool head : {true, false}) {
        const auto fade = edit::clipFade(*m_sequence, clip.id, head ? edit::Edge::Head : edit::Edge::Tail);
        const double x = handleX(rect, head, fade ? fade->toSeconds() * m_pps : 0.0);
        const QRectF box(x - kHandle / 2, rect.top() + 1, kHandle, kHandle);
        mesh.addRect(box.adjusted(-1, -1, 1, 1), m_style.markerInkColor);
        mesh.addRect(box, m_style.selectionColor);
    }
}

void TimelineView::pressFade(const Hit& hit) {
    const auto loc = m_sequence ? m_sequence->findClip(hit.clip) : std::nullopt;
    if (!loc || !m_controller)
        return;
    m_fadeDrag = {hit.clip, hit.fadeHead, false, "fade:" + std::to_string(++m_fadeDragCounter)};
    m_drag = Drag::Fade;
}

void TimelineView::dragFade(QPointF pos) {
    if (!m_controller || !m_fadeDrag.clip)
        return;
    if (!m_fadeDrag.moved && (pos - m_pressPos).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
        return;
    m_fadeDrag.moved = true;
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = seq ? seq->findClip(m_fadeDrag.clip) : std::nullopt;
    if (!loc)
        return;
    const Clip& clip = loc->clip();
    const Ticks at = snapToFrame(ticksAtX(pos.x()), editRate(std::span(&clip.id, 1))); // samples on audio (0.2)
    const Ticks length = std::clamp(m_fadeDrag.head ? at - clip.start() : clip.end() - at, Ticks{}, clip.duration());
    const SequenceId seqId = seq->id;
    const ClipId clipId = clip.id;
    const edit::Edge edge = m_fadeDrag.head ? edit::Edge::Head : edit::Edge::Tail;
    m_controller->applyEditFn(
        m_fadeDrag.head ? LABEL("Fade In") : LABEL("Fade Out"),
        [&](const ProjectState& state) { return edit::setClipFade(state, seqId, clipId, edge, length); },
        m_fadeDrag.mergeKey);
    m_speedReadout = (m_fadeDrag.head ? tr("Fade In %1") : tr("Fade Out %1")).arg(m_controller->formatTimecode(length));
    emit gestureChanged();
    ++m_selectionRevision; // the handle follows (drawn with the overlay)
    update();
}

void TimelineView::releaseFade() {
    if (m_controller && m_fadeDrag.moved)
        m_controller->endPropertyGesture(); // one undo step per drag
    m_fadeDrag = {};
    if (!m_speedReadout.isEmpty()) {
        m_speedReadout.clear();
        emit gestureChanged();
    }
}

} // namespace wabcut::app
