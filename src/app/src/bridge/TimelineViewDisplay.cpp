// TimelineView (M5i): track heights, display settings, trim edit points, marker items for the ruler and the zoom and
// scroll each sequence keeps (sequence tabs). Drawing is in TimelineView.cpp.

#include "TimelineView.h"

#include "BridgeIds.h"

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

// ---- Track heights ------------------------------------------------------------------------------------------------

qreal TimelineView::defaultTrackHeight(bool audio) const {
    return audio ? m_style.audioTrackHeight : m_style.videoTrackHeight;
}

double TimelineView::trackHeightOf(const QString& trackId) const {
    const auto id = parseTrackKey(trackId);
    if (!id)
        return m_style.videoTrackHeight;
    for (const Row& row : m_rows) {
        if (row.track->id == *id)
            return row.height;
    }
    if (const auto it = m_trackHeights.find(id->value); it != m_trackHeights.end())
        return it->second;
    return m_style.videoTrackHeight;
}

void TimelineView::setTrackHeight(const QString& trackId, double height) {
    const auto id = parseTrackKey(trackId);
    if (!id || !std::isfinite(height))
        return;
    const qreal clamped = std::clamp<qreal>(std::round(height), m_style.minTrackHeight, m_style.maxTrackHeight);
    const auto it = m_trackHeights.find(id->value);
    if (it != m_trackHeights.end() && it->second == clamped)
        return;
    m_trackHeights[id->value] = clamped;
    relayoutRows();
    clampScroll();
    emit viewChanged();
    update();
}

void TimelineView::changeTrackHeights(const QString& kind, int steps) {
    if (steps == 0)
        return;
    const bool video = kind != QLatin1String("audio");
    const bool audio = kind != QLatin1String("video");
    for (const Row& row : m_rows) {
        if (row.audio ? !audio : !video)
            continue;
        const qreal h = std::clamp(row.height + steps * m_style.trackHeightStep, m_style.minTrackHeight,
                                   m_style.maxTrackHeight);
        m_trackHeights[row.track->id.value] = h;
    }
    relayoutRows();
    clampScroll();
    emit viewChanged();
    update();
}

void TimelineView::minimizeAllTracks() {
    for (const Row& row : m_rows)
        m_trackHeights[row.track->id.value] = m_style.minTrackHeight;
    relayoutRows();
    clampScroll();
    emit viewChanged();
    update();
}

void TimelineView::resetTrackHeights() {
    m_trackHeights.clear();
    relayoutRows();
    clampScroll();
    emit viewChanged();
    update();
}

QVariantMap TimelineView::trackHeightPreset() const {
    QVariantList video;
    QVariantList audio;
    if (m_sequence) {
        for (const TrackPtr& t : m_sequence->videoTracks)
            video << trackHeightOf(trackKey(t->id));
        for (const TrackPtr& t : m_sequence->audioTracks)
            audio << trackHeightOf(trackKey(t->id));
    }
    return {{QStringLiteral("video"), video}, {QStringLiteral("audio"), audio}};
}

void TimelineView::applyTrackHeightPreset(const QVariantMap& preset) {
    if (!m_sequence)
        return;
    const auto apply = [&](const std::vector<TrackPtr>& tracks, const QVariantList& heights) {
        for (std::size_t i = 0; i < tracks.size() && i < static_cast<std::size_t>(heights.size()); ++i) {
            bool ok = false;
            const double h = heights[static_cast<qsizetype>(i)].toDouble(&ok);
            if (ok && std::isfinite(h))
                m_trackHeights[tracks[i]->id.value] = std::clamp<qreal>(h, m_style.minTrackHeight, m_style.maxTrackHeight);
        }
    };
    apply(m_sequence->videoTracks, preset.value(QStringLiteral("video")).toList());
    apply(m_sequence->audioTracks, preset.value(QStringLiteral("audio")).toList());
    relayoutRows();
    clampScroll();
    emit viewChanged();
    update();
}

// ---- Per-sequence zoom and scroll ---------------------------------------------------------------------------------

void TimelineView::switchSequenceView(SequenceId next) {
    if (m_shownSequence)
        m_sequenceViews[m_shownSequence.value] = {m_pps, m_scrollX, m_scrollY};
    m_shownSequence = next;
    if (const auto it = m_sequenceViews.find(next.value); next && it != m_sequenceViews.end()) {
        m_pps = it->second.pps;
        m_scrollX = it->second.scrollX;
        m_scrollY = it->second.scrollY;
    }
}

// ---- Display settings ---------------------------------------------------------------------------------------------

void TimelineView::bumpDisplay() {
    ++m_styleRevision; // a full rebuild
    emit displayChanged();
    update();
}

void TimelineView::setFilmstripMode(const QString& mode) {
    static const QStringList modes{QStringLiteral("off"), QStringLiteral("head"), QStringLiteral("headTail"),
                                   QStringLiteral("continuous")};
    if (!modes.contains(mode) || mode == m_filmstripMode)
        return;
    m_filmstripMode = mode;
    bumpDisplay();
}

void TimelineView::setRectifiedWaveforms(bool on) {
    if (on == m_rectified)
        return;
    m_rectified = on;
    bumpDisplay();
}

void TimelineView::setLogarithmicWaveforms(bool on) {
    if (on == m_logarithmic)
        return;
    m_logarithmic = on;
    bumpDisplay();
}

void TimelineView::setVolumeWaveforms(bool on) {
    if (on == m_volumeWaveforms)
        return;
    m_volumeWaveforms = on;
    bumpDisplay();
}

void TimelineView::setShowDuplicateFrames(bool on) {
    if (on == m_showDuplicates)
        return;
    m_showDuplicates = on;
    bumpDisplay();
}

void TimelineView::setShowThroughEdits(bool on) {
    if (on == m_showThroughEdits)
        return;
    m_showThroughEdits = on;
    bumpDisplay();
}

void TimelineView::setTrackKeyframes(const QString& mode) {
    static const QStringList modes{QStringLiteral("off"), QStringLiteral("volume"), QStringLiteral("pan")};
    if (!modes.contains(mode) || mode == m_trackKeyframes)
        return;
    m_trackKeyframes = mode;
    bumpDisplay();
}

void TimelineView::setShowClipMarkers(bool on) {
    if (on == m_showClipMarkers)
        return;
    m_showClipMarkers = on;
    bumpDisplay();
}

void TimelineView::setShowSnapIndicator(bool on) {
    if (on == m_showSnapIndicator)
        return;
    m_showSnapIndicator = on;
    emit displayChanged();
    update();
}

void TimelineView::setLinkedSelection(bool on) {
    if (on == m_linkedSelection)
        return;
    m_linkedSelection = on;
    emit displayChanged();
}

void TimelineView::setFilmstripSource(std::shared_ptr<const FilmstripSource> source) {
    m_filmstrips = source ? std::move(source) : std::make_shared<NoFilmstripSource>();
    filmstripsChanged();
}

void TimelineView::filmstripsChanged() {
    ++m_styleRevision;
    update();
}

// ---- Trim edit points -----------------------------------------------------------------------------------------------

void TimelineView::setTrimPoints(const QVariantList& points) {
    if (points == m_trimPointList)
        return;
    m_trimPointList = points;
    m_trimPoints.clear();
    for (const QVariant& v : points) {
        const QVariantMap m = v.toMap();
        if (const auto clip = parseClipKey(m.value(QStringLiteral("clip")).toString()))
            m_trimPoints.push_back({*clip, m.value(QStringLiteral("edge")).toString() == QLatin1String("head")});
    }
    ++m_selectionRevision; // drawn with the overlay
    emit trimPointsChanged();
    update();
}

void TimelineView::setTrimType(const QString& type) {
    if (type == m_trimType)
        return;
    m_trimType = type;
    ++m_selectionRevision;
    emit trimPointsChanged();
    update();
}

// ---- Markers for the ruler ----------------------------------------------------------------------------------------

QVariantList TimelineView::markerItems() const {
    QVariantList list;
    if (!m_sequence)
        return list;
    int index = 0;
    for (const Marker& m : m_sequence->markers) {
        list << QVariantMap{{QStringLiteral("index"), index++},
                            {QStringLiteral("x"), xOfTicks(m.start)},
                            {QStringLiteral("endX"), xOfTicks(m.end())},
                            {QStringLiteral("colorIndex"), m.colorIndex},
                            {QStringLiteral("name"), QString::fromStdString(m.name)},
                            {QStringLiteral("type"), QString::fromLatin1(markerTypeName(m.type).data(), static_cast<qsizetype>(markerTypeName(m.type).size()))}};
    }
    return list;
}

int TimelineView::markerAt(double x) const {
    if (!m_sequence)
        return -1;
    int best = -1;
    double bestDistance = 6.0; // px
    int index = 0;
    for (const Marker& m : m_sequence->markers) {
        const double distance = std::abs(xOfTicks(m.start) - x);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = index;
        }
        ++index;
    }
    return best;
}

QString TimelineView::timeKeyAt(double x) const {
    // Frame-quantised: the start of the frame under x.
    const Rate rate = frameRate();
    const Ticks t = ticksAtX(x);
    const Ticks frame = framesToTicks(ticksToFrames(t, rate), rate);
    return QString::number(frame.value);
}

} // namespace wabcut::app
