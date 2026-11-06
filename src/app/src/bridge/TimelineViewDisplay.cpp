// TimelineView (M5i): track heights, display settings, trim edit points, marker items for the ruler and the zoom and
// scroll each sequence keeps (sequence tabs). 0.4: the clip speed, fx badge and playhead snap switches. 0.6: the Clip
// Mismatch Warning's look and match, and the input modifiers for Shift-clicks. Drawing is in TimelineView.cpp.

#include "TimelineView.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QGuiApplication>

#include <algorithm>
#include <cmath>
#include <limits>

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
        if (row.track->id == *id) {
            if (!row.hidden)
                return row.height;
            // 0.7 (card 124): folded into a collapsed group: the height it has when shown
            const auto stored = m_trackHeights.find(id->value);
            return stored != m_trackHeights.end() ? stored->second : defaultTrackHeight(row.audio);
        }
    }
    if (const auto it = m_trackHeights.find(id->value); it != m_trackHeights.end())
        return it->second;
    return m_style.videoTrackHeight;
}

double TimelineView::trackYOf(const QString& trackId) const {
    const auto id = parseTrackKey(trackId);
    if (!id)
        return 0;
    for (const Row& row : m_rows) {
        if (row.track->id == *id)
            return row.y;
    }
    return 0;
}

bool TimelineView::trackVisible(const QString& trackId) const {
    const auto id = parseTrackKey(trackId);
    if (!id)
        return false;
    for (const Row& row : m_rows) {
        if (row.track->id == *id)
            return !row.hidden;
    }
    return false;
}

void TimelineView::setGroupTrackHeight(const QString& groupId, double height) {
    const auto id = parseTrackGroupKey(groupId);
    const TrackGroup* group = m_sequence && id ? m_sequence->findTrackGroup(*id) : nullptr;
    if (!group || !std::isfinite(height))
        return;
    const qreal clamped = std::clamp<qreal>(std::round(height), m_style.minTrackHeight, m_style.maxTrackHeight);
    bool changed = false;
    for (const TrackId member : group->members) {
        const auto it = m_trackHeights.find(member.value);
        if (it == m_trackHeights.end() || it->second != clamped) {
            m_trackHeights[member.value] = clamped;
            changed = true;
        }
    }
    if (!changed)
        return;
    relayoutRows();
    clampScroll();
    emit viewChanged();
    update();
}

void TimelineView::refreshGroupRowItems() {
    QVariantList items;
    if (m_sequence) {
        for (const GroupRow& row : m_groupRows) {
            const TrackGroup* group = m_sequence->findTrackGroup(row.id);
            if (!group)
                continue;
            // 0: no track of the group has the flag, 1: some do, 2: all do.
            const auto state = [&](bool (*has)(const Track&)) {
                int count = 0;
                for (const TrackId id : group->members) {
                    const Track* t = m_sequence->findTrack(id);
                    if (t && has(*t))
                        ++count;
                }
                return count == 0 ? 0 : count == static_cast<int>(group->members.size()) ? 2 : 1;
            };
            items << QVariantMap{{QStringLiteral("id"), trackGroupKey(group->id)},
                                 {QStringLiteral("name"), QString::fromStdString(group->name)},
                                 {QStringLiteral("kind"), group->kind == TrackKind::Video ? QStringLiteral("video") : QStringLiteral("audio")},
                                 {QStringLiteral("colorIndex"), group->colorIndex},
                                 {QStringLiteral("collapsed"), group->collapsed},
                                 {QStringLiteral("count"), static_cast<int>(group->members.size())},
                                 {QStringLiteral("y"), row.y},
                                 {QStringLiteral("height"), row.height},
                                 {QStringLiteral("muted"), state([](const Track& t) { return t.muted; })},
                                 {QStringLiteral("solo"), state([](const Track& t) { return t.solo; })},
                                 {QStringLiteral("locked"), state([](const Track& t) { return t.locked; })},
                                 {QStringLiteral("targeted"), state([](const Track& t) { return t.targeted; })},
                                 {QStringLiteral("enabled"), state([](const Track& t) { return t.enabled; })}};
        }
    }
    if (items == m_groupRowItems)
        return;
    m_groupRowItems = std::move(items);
    emit groupRowsChanged();
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
        if (row.hidden || (row.audio ? !audio : !video)) // 0.7: a folded track keeps the height it has when shown
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
    m_zoomBeforeFit.reset(); // 0.4: a tab switch forgets Zoom to Sequence's way back (the next \ fits again)
    if (m_playheadSnapAt) {  // 0.4: and a ruler scrub's snap line, which belongs to the sequence it was on
        m_playheadSnapAt.reset();
        emit gestureChanged();
    }
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

// 0.4: the speed after a clip's name and its fx chip (TimelineView.cpp placeLabel), and the playhead snap of ruler
// scrubs (TimelineView.cpp seekToX).
void TimelineView::setShowClipSpeed(bool on) {
    if (on == m_showClipSpeed)
        return;
    m_showClipSpeed = on;
    bumpDisplay();
}

void TimelineView::setShowFxBadges(bool on) {
    if (on == m_showFxBadges)
        return;
    m_showFxBadges = on;
    bumpDisplay();
}

void TimelineView::setSnapPlayhead(bool on) {
    if (on == m_snapPlayhead)
        return;
    m_snapPlayhead = on;
    emit snappingChanged();
}

// ---- 0.7 (stream A, batch A2) ----

void TimelineView::setSnapTargetMask(int mask) {
    mask &= kSnapAll;
    if (mask == m_snapTargetMask)
        return;
    m_snapTargetMask = mask;
    emit snappingChanged();
}

edit::SnapOptions TimelineView::snapOptions() const {
    edit::SnapOptions options;
    options.clipEdges = (m_snapTargetMask & kSnapClipEdges) != 0;
    options.playhead = (m_snapTargetMask & kSnapPlayhead) != 0;
    options.marks = (m_snapTargetMask & kSnapMarks) != 0;
    options.markers = (m_snapTargetMask & kSnapMarkers) != 0;
    options.beats = (m_snapTargetMask & kSnapBeats) != 0;
    return options;
}

void TimelineView::setShowClipDuration(bool on) {
    if (on == m_showClipDuration)
        return;
    m_showClipDuration = on;
    bumpDisplay();
}

void TimelineView::setShowClipNotes(bool on) {
    if (on == m_showClipNotes)
        return;
    m_showClipNotes = on;
    bumpDisplay();
}

void TimelineView::setShowStreamNames(bool on) {
    if (on == m_showStreamNames)
        return;
    m_showStreamNames = on;
    bumpDisplay();
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
    const auto& markers = m_sequence->markers;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const Marker& m = markers[i];
        // 0.2 UX (stream T): what the ruler's name label and hover tooltip show.
        const QString timecode = m_controller ? m_controller->formatTimecode(m.start) : QString();
        const QString duration = m_controller && m.duration > Ticks{} ? m_controller->formatTimecode(m.duration) : QString();
        const double nextX = i + 1 < markers.size() ? xOfTicks(markers[i + 1].start) : std::numeric_limits<double>::infinity();
        list << QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                            {QStringLiteral("x"), xOfTicks(m.start)},
                            {QStringLiteral("endX"), xOfTicks(m.end())},
                            {QStringLiteral("colorIndex"), m.colorIndex},
                            {QStringLiteral("name"), QString::fromStdString(m.name)},
                            {QStringLiteral("type"), QString::fromLatin1(markerTypeName(m.type).data(), static_cast<qsizetype>(markerTypeName(m.type).size()))},
                            {QStringLiteral("comment"), QString::fromStdString(m.comment)},
                            {QStringLiteral("timecode"), timecode},
                            {QStringLiteral("durationText"), duration},
                            {QStringLiteral("nextX"), nextX}};
    }
    return list;
}

void TimelineView::setShowBeatGrid(bool on) {
    if (on == m_showBeatGrid)
        return;
    m_showBeatGrid = on;
    emit displayChanged();
    emit viewChanged(); // the ruler re-reads beatTicks
    update();
}

QVariantList TimelineView::beatTicks() const {
    QVariantList list;
    if (!m_showBeatGrid || !m_sequence || !m_sequence->beatGrid)
        return list;
    const BeatGrid& grid = *m_sequence->beatGrid;
    const Ticks left = ticksAtX(-8.0);
    const Ticks right = ticksAtX(width() + 8.0);
    if (right <= left)
        return list;
    double lastX = -std::numeric_limits<double>::infinity();
    constexpr int kMaxTicks = 600; // a zoomed-out hour of music stays a handful of delegates
    for (const Beat& beat : grid.beatsIn(TimeRange::fromStartEnd(std::max(Ticks{}, left), right))) {
        const double x = xOfTicks(beat.time);
        // Thin ticks: one every few pixels at most, so a dense grid does not smear the ruler; downbeats always show.
        if (!beat.isDownbeat() && x - lastX < 4.0)
            continue;
        lastX = x;
        list << QVariantMap{{QStringLiteral("x"), x}, {QStringLiteral("downbeat"), beat.isDownbeat()}};
        if (list.size() >= kMaxTicks)
            break;
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

// ---- 0.6 (P1 round, stream 1A) ------------------------------------------------------------------------------------

int TimelineView::keyboardModifiers() const {
    return static_cast<int>(QGuiApplication::keyboardModifiers());
}

bool TimelineView::sequenceEmpty() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return false;
    bool empty = true;
    seq->forEachTrack([&](const Track& t) { empty = empty && t.clips.empty(); });
    return empty;
}

namespace {

/// "1920 x 1080, 29.97 fps" for the Clip Mismatch Warning.
QString shapeText(std::int32_t width, std::int32_t height, Rate rate) {
    const double fps = rate.isValid() ? rate.toDouble() : 0.0;
    const bool whole = std::abs(fps - std::round(fps)) < 0.005;
    const QString fpsText = whole ? QString::number(std::lround(fps)) : QString::number(fps, 'f', 2);
    return QCoreApplication::translate("wabcut::app::TimelineView", "%1 x %2, %3 fps").arg(width).arg(height).arg(fpsText);
}

/// The media of the first video clip (track order) and the change that matches the sequence to it.
std::optional<std::pair<const MediaItem*, edit::SequenceSettingsChange>> mismatchOf(const ProjectState& state,
                                                                                   const Sequence& seq) {
    for (const TrackPtr& t : seq.videoTracks) {
        for (const Clip& c : t->clips) {
            if (c.isNested() || !c.media.isValid())
                continue;
            const MediaItem* media = state.findMedia(c.media);
            if (!media)
                continue;
            if (auto change = edit::settingsToMatchMedia(seq, *media))
                return std::pair{media, std::move(*change)};
            return std::nullopt; // the first clip matches (or is a still): nothing to ask
        }
    }
    return std::nullopt;
}

} // namespace

QVariantMap TimelineView::clipMismatch() const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!snap || !seq)
        return {};
    const auto found = mismatchOf(*snap, *seq);
    if (!found)
        return {};
    const auto& [media, change] = *found;
    return {{QStringLiteral("media"), mediaKey(media->id)},
            {QStringLiteral("name"), QString::fromStdString(media->name)},
            {QStringLiteral("mediaText"), shapeText(change.width.value_or(seq->width), change.height.value_or(seq->height),
                                                    change.frameRate.value_or(seq->frameRate))},
            {QStringLiteral("sequenceText"), shapeText(seq->width, seq->height, seq->frameRate)}};
}

bool TimelineView::matchSequenceToClip(bool withLastStep, int revision) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!snap || !seq)
        return false;
    const auto found = mismatchOf(*snap, *seq);
    if (!found)
        return false;
    const SequenceId seqId = seq->id;
    const edit::SequenceSettingsChange change = found->second;
    EditSession& session = m_controller->session();
    const bool merge = withLastStep && session.canUndo() && (revision < 0 || revision == m_controller->revision());
    if (!merge) {
        return m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Sequence Settings"),
                                         [&](const ProjectState& s) { return edit::setSequenceSettings(s, seqId, change); });
    }
    // The drop and the new settings as one step: back to before the drop, then both at once (the clips keep their ids,
    // so the selection the drop made comes back).
    auto next = edit::setSequenceSettings(*snap, seqId, change);
    if (!next)
        return m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Sequence Settings"),
                                         [&](const ProjectState& s) { return edit::setSequenceSettings(s, seqId, change); });
    const std::string label = session.undoLabel().value_or("Sequence Settings");
    const std::vector<ClipId> selection = m_controller->selection();
    const ClipId primary = m_controller->primaryClip();
    m_controller->undo();
    const ProjectState combined = std::move(next->state);
    const bool ok = m_controller->applyEditFn(label.c_str(), [&](const ProjectState& s) -> edit::EditOutcome {
        edit::EditResult result;
        result.changes = diff(s, combined);
        result.state = combined;
        return result;
    });
    if (ok)
        m_controller->setSelection(selection, primary);
    return ok;
}

} // namespace wabcut::app
