#include "SequenceTimeBar.h"

#include "TimelineGeometry.h"

#include <wabcut/model/Timecode.h>

#include <QSGGeometryNode>
#include <QtNumeric>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::app {

namespace {

// The shortest window the zoom allows, and the shortest bar (an empty or very short sequence still spans this).
constexpr double kMinWindowSeconds = 0.5;
constexpr std::int64_t kMinSpanSeconds = 5;
// Time ticks at least this far apart.
constexpr double kTickSpacing = 48;
// Edit points closer than this to the last one drawn are skipped (a dense cut reads as a texture, not noise).
constexpr double kEditSpacing = 3;

} // namespace

SequenceTimeBar::SequenceTimeBar(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setClip(true);
}

void SequenceTimeBar::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::changed, this, &SequenceTimeBar::onSequence);
        connect(m_controller, &ProjectController::sequenceChanged, this, &SequenceTimeBar::onSequence);
        connect(m_controller, &ProjectController::marksChanged, this, &SequenceTimeBar::onSequence);
        connect(m_controller, &ProjectController::markersChanged, this, &SequenceTimeBar::onSequence);
        connect(m_controller, &ProjectController::playheadChanged, this, &SequenceTimeBar::onPlayhead);
    }
    onSequence();
    emit controllerChanged();
}

void SequenceTimeBar::setColor(QColor& member, const QColor& value) {
    if (member == value)
        return;
    member = value;
    markDirty();
    emit appearanceChanged();
}

void SequenceTimeBar::setMarkerColors(const QVariantList& colors) {
    m_markerColorList = colors;
    m_markerColors.clear();
    for (const QVariant& v : colors) {
        const QColor c = v.metaType() == QMetaType::fromType<QColor>() ? v.value<QColor>() : QColor(v.toString());
        if (c.isValid())
            m_markerColors.push_back(c);
    }
    markDirty();
    emit appearanceChanged();
}

void SequenceTimeBar::markDirty() {
    m_dirty = true;
    update();
}

void SequenceTimeBar::onSequence() {
    markDirty();
    emit layoutChanged();
    emit marksChanged();
    emit playheadXChanged();
}

void SequenceTimeBar::onPlayhead() {
    // Zoomed in, the window pages along with a playhead that leaves it (playback, keys), like the timeline does.
    if (zoomed() && m_controller) {
        const Ticks whole = span();
        if (whole.value > 0) {
            const double f = static_cast<double>(m_controller->playhead().value) / static_cast<double>(whole.value);
            const double w = m_viewEnd - m_viewStart;
            if (f < m_viewStart || f > m_viewEnd)
                setView(f - w * 0.1, f + w * 0.9);
        }
    }
    emit playheadXChanged();
}

void SequenceTimeBar::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    markDirty();
    emit playheadXChanged();
    emit marksChanged();
}

Ticks SequenceTimeBar::span() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return Ticks{};
    return std::max({seq->duration, seq->contentEnd(), Ticks::fromSeconds(kMinSpanSeconds)});
}

bool SequenceTimeBar::active() const {
    return m_controller && m_controller->currentSequence() != nullptr;
}

double SequenceTimeBar::xOf(Ticks t) const {
    const Ticks whole = span();
    const double w = m_viewEnd - m_viewStart;
    if (whole.value <= 0 || w <= 0)
        return qQNaN();
    const double f = static_cast<double>(t.value) / static_cast<double>(whole.value);
    return (f - m_viewStart) / w * width();
}

Ticks SequenceTimeBar::timeAtX(double x) const {
    const Ticks whole = span();
    if (whole.value <= 0 || width() <= 0)
        return Ticks{};
    const double f = std::clamp(m_viewStart + x / width() * (m_viewEnd - m_viewStart), 0.0, 1.0);
    return Ticks{static_cast<std::int64_t>(std::llround(f * static_cast<double>(whole.value)))};
}

double SequenceTimeBar::playheadX() const {
    if (!active())
        return qQNaN();
    const double x = xOf(m_controller->playhead());
    return x >= 0 && x <= width() ? x : qQNaN();
}

bool SequenceTimeBar::hasIn() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return seq && seq->markIn.has_value();
}

bool SequenceTimeBar::hasOut() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return seq && seq->markOut.has_value();
}

double SequenceTimeBar::inX() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return seq && seq->markIn ? xOf(*seq->markIn) : qQNaN();
}

double SequenceTimeBar::outX() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return seq && seq->markOut ? xOf(*seq->markOut) : qQNaN();
}

void SequenceTimeBar::seekToX(double x) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return;
    m_controller->setPlayhead(std::clamp(snapToFrame(timeAtX(x), seq->frameRate), Ticks{}, span()));
}

QString SequenceTimeBar::timecodeAt(double x) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return {};
    return m_controller->formatTimecode(snapToFrame(timeAtX(x), seq->frameRate));
}

QVariantMap SequenceTimeBar::markerAt(double x, double reach) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return {};
    int best = -1;
    double bestDistance = std::max(0.0, reach);
    for (std::size_t i = 0; i < seq->markers.size(); ++i) {
        const Marker& m = seq->markers[i];
        // A range marker counts along its whole bar; a point marker within `reach` px.
        const double x0 = xOf(m.start);
        const double x1 = xOf(m.end());
        const double distance = x >= x0 && x <= x1 ? 0.0 : std::min(std::abs(x - x0), std::abs(x - x1));
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    if (best < 0)
        return {};
    const Marker& m = seq->markers[static_cast<std::size_t>(best)];
    const std::string_view type = markerTypeName(m.type);
    return {
        {QStringLiteral("index"), best},
        {QStringLiteral("x"), xOf(m.start)},
        {QStringLiteral("endX"), xOf(m.end())},
        {QStringLiteral("name"), QString::fromStdString(m.name)},
        {QStringLiteral("comment"), QString::fromStdString(m.comment)},
        {QStringLiteral("timecode"), m_controller->formatTimecode(m.start)},
        {QStringLiteral("durationText"), m.duration > Ticks{} ? m_controller->formatTimecode(m.duration) : QString()},
        {QStringLiteral("colorIndex"), m.colorIndex},
        {QStringLiteral("type"), QString::fromLatin1(type.data(), static_cast<qsizetype>(type.size()))},
    };
}

void SequenceTimeBar::setView(double start, double end) {
    const double whole = span().toSeconds();
    const double minimum = whole > 0 ? std::min(1.0, kMinWindowSeconds / whole) : 1.0;
    double w = std::clamp(end - start, minimum, 1.0);
    start = std::clamp(start, 0.0, 1.0 - w);
    // A window within a hair of the whole sequence is the whole sequence (no zoom bar for rounding errors).
    if (w > 0.999) {
        start = 0;
        w = 1;
    }
    if (start == m_viewStart && start + w == m_viewEnd)
        return;
    m_viewStart = start;
    m_viewEnd = start + w;
    markDirty();
    emit viewChanged();
    emit playheadXChanged();
    emit marksChanged();
}

void SequenceTimeBar::zoomAt(double factor, double x) {
    if (factor <= 0 || width() <= 0)
        return;
    const double w = m_viewEnd - m_viewStart;
    const double anchor = m_viewStart + std::clamp(x / width(), 0.0, 1.0) * w;
    const double next = w / factor;
    const double share = w > 0 ? (anchor - m_viewStart) / w : 0;
    setView(anchor - share * next, anchor - share * next + next);
}

void SequenceTimeBar::panBy(double fraction) {
    setView(m_viewStart + fraction, m_viewEnd + fraction);
}

QSGNode* SequenceTimeBar::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGGeometryNode*>(oldNode);
    if (!node) {
        node = MeshBuilder::createNode();
        m_dirty = true;
    }
    if (!m_dirty)
        return node;
    m_dirty = false;

    MeshBuilder mesh;
    int rects = 0;
    const auto rect = [&](const QRectF& r, const QColor& c) {
        mesh.addRect(r, c);
        ++rects;
    };
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const double w = width();
    const double h = height();
    const Ticks whole = span();
    if (seq && w > 0 && h > 0 && whole.value > 0) {
        const double visible = (m_viewEnd - m_viewStart) * whole.toSeconds();
        const double pxPerSecond = w / std::max(1e-9, visible);
        const double bandTop = std::round(h * 0.18);
        const double bandBottom = std::round(h * 0.62);

        // Where the sequence has content: every track's clips, merged into runs (gaps show as the bar's ground).
        std::vector<std::pair<double, double>> spans;
        std::vector<double> edits;
        seq->forEachTrack([&](const Track& track) {
            for (const Clip& c : track.clips) {
                const double x0 = xOf(c.start());
                const double x1 = xOf(c.end());
                if (x1 < 0 || x0 > w)
                    continue;
                spans.emplace_back(std::max(0.0, x0), std::min(w, x1));
                edits.push_back(x0);
                edits.push_back(x1);
            }
        });
        std::sort(spans.begin(), spans.end());
        double runX0 = -1, runX1 = -1;
        for (const auto& [x0, x1] : spans) {
            if (runX1 >= 0 && x0 <= runX1 + 0.5) {
                runX1 = std::max(runX1, x1);
                continue;
            }
            if (runX1 > runX0)
                rect(QRectF(runX0, bandTop, std::max(1.0, runX1 - runX0), bandBottom - bandTop), m_contentColor);
            runX0 = x0;
            runX1 = x1;
        }
        if (runX1 > runX0)
            rect(QRectF(runX0, bandTop, std::max(1.0, runX1 - runX0), bandBottom - bandTop), m_contentColor);

        // Edit points: one hairline per cut, thinned to one every few pixels.
        std::sort(edits.begin(), edits.end());
        double lastEdit = -1e9;
        for (double x : edits) {
            if (x <= 0.5 || x >= w - 0.5 || x - lastEdit < kEditSpacing)
                continue;
            rect(QRectF(std::round(x), bandTop, 1, bandBottom - bandTop), m_editColor);
            lastEdit = x;
        }

        // Time ticks along the bottom third, at the first step at least kTickSpacing px apart.
        static constexpr std::array<double, 15> steps{0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
        double step = steps.back();
        for (double s : steps) {
            if (s * pxPerSecond >= kTickSpacing) {
                step = s;
                break;
            }
        }
        const double first = std::ceil(m_viewStart * whole.toSeconds() / step) * step;
        for (double s = first; s <= m_viewEnd * whole.toSeconds() + 1e-9; s += step) {
            const double x = std::round((s - m_viewStart * whole.toSeconds()) * pxPerSecond);
            if (x <= 0 || x >= w)
                continue;
            rect(QRectF(x, std::round(h * 2 / 3), 1, h - std::round(h * 2 / 3)), m_tickColor);
        }

        // In/Out: the range washed, its ends bracketed (one mark alone runs to the other end, as in the Source Monitor).
        if (seq->markIn || seq->markOut) {
            const double x0 = seq->markIn ? xOf(*seq->markIn) : xOf(Ticks{});
            const double x1 = seq->markOut ? xOf(*seq->markOut) : xOf(whole);
            const double a = std::clamp(x0, 0.0, w);
            const double b = std::clamp(x1, 0.0, w);
            if (b > a)
                rect(QRectF(a, 0, b - a, h), m_rangeColor);
            constexpr double bracket = 2;
            if (seq->markIn && x0 >= 0 && x0 <= w)
                rect(QRectF(std::round(x0), 0, bracket, h), m_markColor);
            if (seq->markOut && x1 >= 0 && x1 <= w)
                rect(QRectF(std::round(x1) - bracket, 0, bracket, h), m_markColor);
        }

        // Markers: a line with a small cap; a ranged marker also shades its span along the top.
        for (const Marker& m : seq->markers) {
            const QColor color = m_markerColors.empty()
                                     ? m_markColor
                                     : m_markerColors[static_cast<std::size_t>(
                                           ((m.colorIndex % static_cast<int>(m_markerColors.size())) +
                                            static_cast<int>(m_markerColors.size())) %
                                           static_cast<int>(m_markerColors.size()))];
            const double x = xOf(m.start);
            if (m.duration.value > 0) {
                const double end = xOf(m.start + m.duration);
                if (end >= 0 && x <= w) {
                    QColor band = color;
                    band.setAlphaF(0.45f);
                    rect(QRectF(std::max(0.0, x), 0, std::min(w, end) - std::max(0.0, x), std::round(h * 0.18)), band);
                }
            }
            if (x < -3 || x > w + 3)
                continue;
            const double cx = std::round(x);
            rect(QRectF(cx - 0.75, 0, 1.5, h), color);
            mesh.addTriangle(QPointF(cx - 3.5, 0), QPointF(cx + 3.5, 0), QPointF(cx, 4), color);
        }
    }
    mesh.commit(node);
    m_lastRects = rects;
    return node;
}

} // namespace wabcut::app
