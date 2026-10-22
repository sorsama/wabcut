// TimelineView, 0.2 timeline parity: Premiere's render bar above the ruler (render.status: red, yellow, green), the
// Show Video / Audio Fade Handles switches and Show Audio Time Units (the quantum of drags of audio-only material).
//
// The render bar is the model's (PreviewCache.h renderSegments / PreviewCache::renderBar): computed on the current
// snapshot a short while after every edit, undo, redo or sequence switch (a burst of edits costs one evaluation), and
// when MediaReadiness says the preview files changed (Render In to Out finished). Rendered segments need the project's
// preview cache (MediaReadiness.previewCache); without one nothing is green, as in render.status.

#include "TimelineView.h"

#include "MediaReadinessBridge.h"

#include <QTimer>

#include <algorithm>

namespace wabcut::app {

namespace {

/// Edits come in bursts (a drag's commit, a nudge held down): the bar waits this long after the last one.
constexpr int kRenderBarDelayMs = 120;

QString statusKey(RenderStatus status) {
    switch (status) {
    case RenderStatus::Red:
        return QStringLiteral("red");
    case RenderStatus::Yellow:
        return QStringLiteral("yellow");
    case RenderStatus::Rendered:
        return QStringLiteral("rendered");
    case RenderStatus::None:
        break;
    }
    return QStringLiteral("none");
}

} // namespace

void TimelineView::setReadiness(QObject* readiness) {
    if (m_readiness == readiness)
        return;
    if (m_readiness)
        disconnect(m_readiness, nullptr, this, nullptr);
    m_readiness = readiness;
    if (auto* bridge = qobject_cast<MediaReadinessBridge*>(readiness))
        connect(bridge, &MediaReadinessBridge::previewFilesChanged, this, &TimelineView::scheduleRenderBar);
    emit readinessChanged();
    scheduleRenderBar();
}

void TimelineView::scheduleRenderBar() {
    if (!m_renderBarTimer) {
        m_renderBarTimer = new QTimer(this);
        m_renderBarTimer->setSingleShot(true);
        m_renderBarTimer->setInterval(kRenderBarDelayMs);
        connect(m_renderBarTimer, &QTimer::timeout, this, &TimelineView::refreshRenderBar);
    }
    m_renderBarTimer->start();
}

void TimelineView::refreshRenderBar() {
    if (m_renderBarTimer)
        m_renderBarTimer->stop();
    std::vector<RenderBarSegment> bar;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (snap && seq && seq->duration > Ticks{}) {
        auto* bridge = qobject_cast<MediaReadinessBridge*>(m_readiness.data());
        const std::shared_ptr<PreviewCache> cache = bridge ? bridge->previewCache() : nullptr;
        const std::vector<RenderSegment> segments = cache ? cache->renderBar(*snap, seq->id) : renderSegments(*snap, seq->id);
        for (const RenderSegment& s : segments) {
            if (s.status == RenderStatus::None)
                continue; // plays as it is: no bar (Premiere)
            const QString reason = QString::fromStdString(s.reason);
            if (!bar.empty() && bar.back().status == s.status && bar.back().end == s.range.start) {
                RenderBarSegment& last = bar.back();
                last.end = s.range.end();
                if (!reason.isEmpty() && !last.reason.split(QStringLiteral("; ")).contains(reason))
                    last.reason = last.reason.isEmpty() ? reason : last.reason + QStringLiteral("; ") + reason;
                continue;
            }
            bar.push_back({s.range.start, s.range.end(), s.status, reason});
        }
    }
    if (bar == m_renderBar)
        return;
    m_renderBar = std::move(bar);
    emit renderBarChanged();
}

QVariantList TimelineView::renderBarItems() const {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(m_renderBar.size()));
    for (const RenderBarSegment& s : m_renderBar) {
        out.append(QVariantMap{{QStringLiteral("start"), s.start.toSeconds()},
                               {QStringLiteral("end"), s.end.toSeconds()},
                               {QStringLiteral("status"), statusKey(s.status)},
                               {QStringLiteral("reason"), s.reason}});
    }
    return out;
}

void TimelineView::setShowVideoFadeHandles(bool on) {
    if (on == m_showVideoFades)
        return;
    m_showVideoFades = on;
    ++m_selectionRevision; // drawn with the overlay
    emit displayChanged();
    update();
}

void TimelineView::setShowAudioFadeHandles(bool on) {
    if (on == m_showAudioFades)
        return;
    m_showAudioFades = on;
    ++m_selectionRevision;
    emit displayChanged();
    update();
}

bool TimelineView::fadeHandlesShown(const Clip& clip, bool audio) const {
    if (!(audio ? m_showAudioFades : m_showVideoFades))
        return false;
    return m_selected.contains(clip.id.value) || (m_hovered && clip.id == m_hovered);
}

void TimelineView::setAudioTimeUnits(bool on) {
    if (on == m_audioTimeUnits)
        return;
    m_audioTimeUnits = on;
    // The zoom range changes (single samples): keep the view where it is, clamped into the new range.
    setPixelsPerSecond(m_pps);
    clampScroll();
    emit displayChanged();
    emit viewChanged(); // the ruler's units
    update();
}

Rate TimelineView::editRate(std::span<const ClipId> clips) const {
    if (!m_audioTimeUnits || !m_sequence || !m_sequence->audioRate.isValid() || clips.empty())
        return frameRate();
    for (const ClipId id : clips) {
        const auto loc = m_sequence->findClip(id);
        if (!loc || loc->track->kind != TrackKind::Audio)
            return frameRate(); // video moves by frames (Premiere: sample edits never move video)
    }
    return m_sequence->audioRate;
}

} // namespace wabcut::app
