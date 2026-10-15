#pragma once

// TimelineView: the timeline lanes (M5) — one QQuickItem that draws every clip of the current sequence with a
// handful of batched scene-graph nodes, straight from the ProjectController's immutable snapshot.
//
// Rendering
//  * One vertex-coloured mesh per track holds all its clip bodies (rounded, anti-aliased, 1 px border) and the
//    audio waveforms (PeakSource seam), plus one mesh each for markers, the selection/hover overlay, the lanes,
//    the playhead and the marquee. Labels are QSGTextNodes, created only for visible clips wide enough to show
//    text and recycled through a pool. Node count is O(tracks + labelled visible clips), never O(clips in the
//    sequence).
//  * Geometry is built for a window around the view ("built range": the viewport plus half a viewport on each
//    side; only the viewport while zooming, since a zoom invalidates it on the next frame anyway) in
//    coordinates relative to that window. Horizontal scrolling inside it only moves a transform (plus the
//    sticky label at the left edge); zoom, leaving the window or a style change rebuild it. Edits, undo and
//    redo rebuild only the tracks whose ChangeSet ranges (ProjectController::lastChange) intersect the built
//    window; a track changed only elsewhere keeps its geometry (and the revision it was built from, which is
//    identical inside the window). Sub-pixel clips are merged into runs, so a zoomed-out 10,000-clip
//    sequence stays cheap.
//  * Text layout is the expensive part of a frame: name widths are cached per font, elision is quantised to
//    8 px steps, and at most a few new labels are laid out per frame (the rest follow on the next frames,
//    TimelineStats::labelsPending).
//  * Visual tokens (colours, radii, heights, font) come from QML as the `style` map built from Theme.
//
// M5i: per-track heights (drag a header's bottom edge, Ctrl/Alt +/-, expand/minimise all, presets), video filmstrips
// (FilmstripSource: texture atlases, one textured node per track and media item, placeholders while loading),
// coloured and range markers with clip markers on clips, through-edit and duplicate-frame indicators, Premiere's
// waveform styles, the trim edit points (TrimBridge) and per-sequence zoom and scroll (sequence tabs).
//
// Interaction: click selects (Shift/Ctrl/⌘ toggles, Alt inverts linked selection), dragging on empty space draws a
// marquee, hover lifts the clip. Zoom and scrolling are driven from QML (pixelsPerSecond/zoomLevel/scrollX/
// scrollY, zoomAt()). Hit-testing returns opaque ids and timecode strings (principle 9).
//
// Editing (M5b, TimelineViewInput.cpp): the active `tool` decides what a press does (TimelineEditing.h). A drag
// becomes a GestureSpec; every mouse move runs its edit on the current snapshot through
// ProjectController::previewEdit() and shows the result as a live ghost preview (the view displays the preview
// sequence, the dragged clips lifted, their old place as a ghost outline, the snap line); nothing is committed
// until release, when ProjectController::commitGesture() applies the same edit as one undo step. Esc cancels.
// Motion: dragged clips lift (Theme.liftScale + shadow) and settle with a spring on drop; clips that shift after
// an edit, undo or cancel slide to their new place (rippleDuration, ease-out); razor cuts flash. Everything is
// interpolated here in the scene graph, never in per-clip QML, and the durations come from Theme (0 under
// reduce motion).

#include "FilmstripSource.h"
#include "PeakSource.h"
#include "ProjectController.h"
#include "TimelineEditing.h"

#include <wabcut/model/Sequence.h>

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QTimer>
#include <QVariantAnimation>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wabcut::app {

class TimelineRootNode;

/// Measurements of the last updatePaintNode (for tests and profiling).
struct TimelineStats {
    qint64 updateNs = 0;   // time spent in updatePaintNode
    bool fullRebuild = false;
    int rebuiltTracks = 0;
    int skippedTracks = 0; // tracks an edit changed only outside the built range (geometry kept)
    int builtClips = 0;    // clips with geometry (built range)
    int visibleClips = 0;  // clips intersecting the viewport
    int labels = 0;        // labelled clips (one QSGTextNode each)
    int labelNodes = 0;    // nodes under those text nodes (glyph nodes included)
    int generatedBadges = 0; // AI micro-tags attached to retained clip labels
    int nestedClips = 0;   // M5l: clips drawn with the nested / multi-camera stripe (built range)
    int angleCuts = 0;     // M5l: angle-cut marks between multi-camera segments (built range)
    int trackLanes = 0;    // M5l: audio tracks drawing their fader or pan lane
    int responsiveClips = 0; // M5o: graphic clips with responsive time, their intro / outro drawn (built range)
    int responsiveTags = 0;  // M5o: "Intro" / "Outro" tags shown on those bands
    QHash<quint64, QPair<qreal, qreal>> responsiveBands; // M5o: clip id -> the intro and outro band widths in px
    QHash<quint64, QString> labelTexts; // clip id -> the label it shows
    QHash<quint64, QString> badgeTexts; // clip id -> its micro-tag ("AI", "ENHANCED", "NOT RENDERED")
    bool labelsPending = false; // some labels wait for the next frame (per-frame text layout budget)
    int nodes = 0;         // scene-graph nodes in this item's subtree
    int filmstripTiles = 0;        // thumbnail tiles drawn (M5i)
    int filmstripPlaceholders = 0; // tiles waiting for their thumbnails, or hidden by a redaction
    int filmstripRedacted = 0;     // of those, tiles whose frame a source redaction covers (review P2-9)
    int filmstripNodes = 0;        // textured nodes (one per track and media atlas)
    int textures = 0;              // atlas textures alive
    int frames = 0;        // updatePaintNode calls so far
    qint64 previewNs = 0;  // last drag preview: the edit run on the snapshot (mouse move), not in updatePaintNode
    /// 0.1.3: the dragged (or settling) clips as drawn, in view coordinates: a drag shows them where they will land.
    QHash<quint64, QRectF> liftedRects;
};

/// Visual tokens, parsed from the QML `style` map (keys match the member names).
struct TimelineStyle {
    QColor laneColor{0x0A, 0x0A, 0x0C};
    QColor clipBorder{16, 16, 19, 64};
    QColor videoLabelColor{0x10, 0x10, 0x13};
    QColor audioLabelColor{0xF4, 0xF3, 0xF7};
    QColor selectionColor{0x8B, 0x5C, 0xFF};
    QColor glowColor{0x8B, 0x5C, 0xFF, 102};
    QColor shadowColor{0, 0, 0, 140};
    QColor playheadColor{Qt::white};
    QColor marqueeFillColor{0x8B, 0x5C, 0xFF, 36};
    QColor marqueeBorderColor{0x8B, 0x5C, 0xFF};
    std::vector<QColor> clipColors{QColor(0xCE, 0xFA, 0x44)};
    std::vector<QColor> audioFillColors;
    std::vector<QColor> audioBorderColors;
    std::vector<QColor> waveformColors;
    QFont labelFont;
    qreal microSize = 10, microTracking = 0.6; // Theme micro-tag typography
    qreal laneRadius = 6, clipRadius = 6, selectionWidth = 2, selectionGap = 1, glowSize = 12, shadowSize = 8;
    qreal liftOffset = 2, playheadWidth = 2, labelPadding = 8, labelTop = 4, waveTop = 16, waveBottom = 4;
    qreal markerWidth = 1, markerOpacity = 0.35;
    qreal videoTrackHeight = 40, audioTrackHeight = 44, trackGap = 2;
    qreal rulerLabelSpacing = 110, rulerMinorSpacing = 8, maxPixelsPerFrame = 24, minLabelWidth = 14;
    int liftDuration = 160;
    // Editing (M5b)
    QColor snapColor{0x8B, 0x5C, 0xFF};     // snap indicator line
    QColor ghostColor{255, 255, 255, 64};   // where dragged clips came from
    QColor razorColor{255, 255, 255, 220};  // razor preview line and cut flash
    qreal snapThreshold = 8;                // px
    qreal trimHandleWidth = 6;              // px: edge zone for trims
    qreal liftScale = 1.02;                 // dragged clips
    int settleDuration = 340;               // drop spring
    int rippleDuration = 180;               // shifted clips slide into place
    int flashDuration = 220;                // razor cut flash
    qreal autoScrollZone = 48;              // px next to each edge where dragging scrolls
    qreal autoScrollSpeed = 1600;           // px/s at the edge
    QColor markerInkColor{0x10, 0x10, 0x13}; // dark half of the selection corner marker and edge marker
    // Transcript (M5e): words selected in the transcript, and what a text edit would remove (its dry run).
    QColor highlightColor{0x8B, 0x5C, 0xFF};
    QColor removalColor{0xF4, 0x3F, 0x5E};
    // Audio keyframes (M5g): the volume rubber band on audio clips and its keys.
    QColor volumeColor{0xFF, 0xFF, 0xFF};
    QColor volumeKeyColor{0xFB, 0xBF, 0x24};
    QColor volumeKeyBorder{0x10, 0x10, 0x13};
    // Transitions (v3): a light frame across the edit point with a dark bow tie meeting at the cut.
    QColor transitionColor{0xFF, 0xFF, 0xFF};
    QColor transitionInk{0x00, 0x00, 0x00};
    // Design language v2 and v3: the clip bodies (v3: solid label colours), the label strip (v2; 0 = none), the beam
    // (playhead, cut spark; flat in v3: all three stops alike), hatching on disabled/offline clips, the slice notch on
    // a selected clip (v2; 0 = none).
    std::vector<QColor> clipBodyColors;          // video clip bodies, per label colour (default: the label colour)
    qreal stripHeight = 3;                        // the label strip along a clip's top (0: none)
    qreal clipHeaderHeight = 0;                   // v3: a solid band carrying the name above a clip's thumbnails (0: a scrim)
    QColor hatchColor{255, 255, 255, 56};
    QColor beamStart{0x38, 0x00, 0xBA};
    QColor beamMid{0x7B, 0x3B, 0xFF};
    QColor beamEnd{0xE0, 0x40, 0xFB};
    qreal playheadGlow = 6;                       // the playhead's glow while playing
    qreal selectedTint = 0.08;                    // added label tint on a selected clip's body
    qreal notchSize = 7;                          // the selected clip's slice notch
    qreal sliceAngle = 12;                        // degrees
    // M5i
    std::vector<QColor> markerColors;             // Premiere's 8 marker colours (green, red, purple, orange, ...)
    std::vector<QColor> duplicateColors;          // duplicate frame bands, one per reused source range
    QColor throughEditColor{255, 255, 255, 190};
    QColor trimRollColor{0xF4, 0x3F, 0x5E};       // Premiere: roll and regular trims red, ripple yellow
    QColor trimRippleColor{0xFB, 0xBF, 0x24};
    QColor placeholderColor{255, 255, 255, 18};   // a thumbnail tile still loading
    QColor scrimColor{0, 0, 0, 120};              // under a label drawn over thumbnails
    qreal minTrackHeight = 24, maxTrackHeight = 240, trackHeightStep = 12;
    qreal duplicateBandHeight = 3;
    // M5l: nested and multi-camera clips (a second stripe under the label strip, a stacked icon, angle cuts), the
    // track fader / pan lanes on audio tracks.
    QColor nestedStripe{0x9B, 0x6B, 0xFF};
    // M5o: a responsive graphic's protected intro and outro (tinted bands, an edge line where the stretch begins).
    QColor responsiveBand{0x9B, 0x6B, 0xFF, 36};
    QColor responsiveEdge{0x9B, 0x6B, 0xFF};
    QColor multicamStripe{0x2E, 0xE6, 0xA8};
    qreal nestedStripeHeight = 2;
    QColor trackVolumeLane{0x9B, 0x6B, 0xFF};
    QColor trackPanLane{0xB4, 0xAC, 0xC8};

    QColor clipBody(int index) const;
    QColor clipColor(int index) const;
    QColor audioFill(int index) const;
    QColor audioBorder(int index) const;
    QColor waveform(int index) const;
    QColor markerColor(int index) const;
};

/// M5l: what clips of other sequences show in the lanes. A nested sequence by id: its name and, for a multicam
/// source, its angle names.
struct TimelineNestInfo {
    QString name;
    bool multicam = false;
    std::vector<QString> angles;
    bool operator==(const TimelineNestInfo&) const = default;
};
struct TimelineLaneFacts {
    std::unordered_map<std::uint64_t, TimelineNestInfo> nested; ///< by sequence id
    std::unordered_map<std::uint64_t, QString> badges;          ///< clip id -> "enhanced" | "missingRender"
    std::unordered_map<std::uint64_t, QString> channels;        ///< M5n (A.9): clip id -> channel badge ("SWAP", ...)
    std::vector<std::pair<ParamValue, ParamValue>> faders;      ///< audio tracks' volume / pan (lanes shown)
    bool operator==(const TimelineLaneFacts&) const = default;
};

class TimelineView : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(wabcut::app::ProjectController* controller READ controller WRITE setController NOTIFY
                   controllerChanged FINAL)
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged FINAL)

    // ---- View (driven from QML) ----
    Q_PROPERTY(double pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond NOTIFY viewChanged FINAL)
    /// 0 = the whole sequence fits, 1 = maxPixelsPerFrame (logarithmic in between).
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY viewChanged FINAL)
    /// Horizontal scroll in px from the sequence start.
    Q_PROPERTY(double scrollX READ scrollX WRITE setScrollX NOTIFY viewChanged FINAL)
    Q_PROPERTY(double scrollY READ scrollY WRITE setScrollY NOTIFY viewChanged FINAL)
    Q_PROPERTY(double contentWidth READ contentWidth NOTIFY viewChanged FINAL)
    Q_PROPERTY(double contentHeight READ contentHeight NOTIFY layoutChanged FINAL)
    /// Track rows completely inside the viewport.
    Q_PROPERTY(int visibleTrackCount READ visibleTrackCount NOTIFY viewChanged FINAL)
    /// Playhead x in view coordinates (may lie outside [0, width]).
    Q_PROPERTY(double playheadX READ playheadX NOTIFY playheadXChanged FINAL)
    /// Sequence In/Out marks in view coordinates (NaN when unset), for the ruler's marked-range bar.
    Q_PROPERTY(double markInX READ markInX NOTIFY viewChanged FINAL)
    Q_PROPERTY(double markOutX READ markOutX NOTIFY viewChanged FINAL)
    Q_PROPERTY(QString hoveredClip READ hoveredClip NOTIFY hoveredClipChanged FINAL)
    Q_PROPERTY(bool marqueeActive READ marqueeActive NOTIFY marqueeChanged FINAL)

    // ---- Editing (M5b) ----
    /// "select", "trackForward", "trackBackward", "ripple", "roll", "slip", "slide" or "razor".
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool snapping READ snapping WRITE setSnapping NOTIFY snappingChanged FINAL)
    /// A drag (move, trim, ripple, roll, slip, slide) is showing its ghost preview.
    Q_PROPERTY(bool gestureActive READ gestureActive NOTIFY gestureChanged FINAL)
    /// Readout next to the pointer while dragging: the applied delta ("+00:00:01:05") or, for slip, the source
    /// In/Out of the clip.
    Q_PROPERTY(QString gestureReadout READ gestureReadout NOTIFY gestureChanged FINAL)
    Q_PROPERTY(QPointF gesturePoint READ gesturePoint NOTIFY gestureChanged FINAL)
    /// Snap indicator x in view coordinates; NaN when the drag is not snapped.
    Q_PROPERTY(double snapX READ snapX NOTIFY gestureChanged FINAL)
    /// The label of the track a drop would create ("A4"), or "" (for the ghost track header).
    Q_PROPERTY(QString newTrackLabel READ newTrackLabel NOTIFY gestureChanged FINAL)
    /// Auto-scrolling while a drag sits near an edge.
    Q_PROPERTY(bool autoScrolling READ autoScrolling NOTIFY autoScrollingChanged FINAL)
    /// 0.1.3: the edit cursor the lanes show now (an EditCursors key; "" = the arrow): what a press would do where the
    /// pointer hovers (trim and ripple brackets on edges, the tool's own cursor), or what the drag in progress does.
    Q_PROPERTY(QString cursorKey READ cursorKey NOTIFY cursorKeyChanged FINAL)
    /// 0.1.3: where a drag from outside the lanes would land while it is over them (showDropIndicator), as dropPoint()
    /// describes it; empty when none is shown.
    Q_PROPERTY(QVariantMap dropIndicator READ dropIndicator NOTIFY dropIndicatorChanged FINAL)

    // ---- Ruler support: labels sit every rulerStepPx starting at rulerFirstX; the label of tick i is
    //      rulerLabel(rulerFirstIndex + i, rulerStepFrames). ----
    Q_PROPERTY(double rulerStepPx READ rulerStepPx NOTIFY viewChanged FINAL)
    Q_PROPERTY(double rulerFirstX READ rulerFirstX NOTIFY viewChanged FINAL)
    Q_PROPERTY(int rulerFirstIndex READ rulerFirstIndex NOTIFY viewChanged FINAL)
    Q_PROPERTY(int rulerStepFrames READ rulerStepFrames NOTIFY viewChanged FINAL)
    Q_PROPERTY(int rulerSubdivisions READ rulerSubdivisions NOTIFY viewChanged FINAL)
    /// View x of each sequence marker (same order as ProjectController.markers).
    Q_PROPERTY(QVariantList markerPositions READ markerPositions NOTIFY viewChanged FINAL)
    /// The volume rubber band on audio clips (M5g): a line at the clip's volume with its keyframes, editable in
    /// place with the Selection tool (click to add a key, drag a key or the line, Alt-click a key for Bezier).
    Q_PROPERTY(bool showVolume READ showVolume WRITE setShowVolume NOTIFY showVolumeChanged FINAL)
    Q_PROPERTY(bool showSpeed READ showSpeed WRITE setShowSpeed NOTIFY showSpeedChanged FINAL)
    Q_PROPERTY(QString speedReadout READ speedReadout NOTIFY gestureChanged FINAL)
    /// Playback is running: the playhead beam glows (design language v2; no glow at rest).
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged FINAL)

    // ---- M5i ----
    /// Bumps whenever a row's position or height changes (track headers bind their height through it).
    Q_PROPERTY(int layoutRevision READ layoutRevision NOTIFY layoutChanged FINAL)
    /// Sequence markers for the ruler: [{ index, x, endX (= x for a point marker), colorIndex, name, type }].
    Q_PROPERTY(QVariantList markerItems READ markerItems NOTIFY viewChanged FINAL)
    /// Premiere's Video Head / Head and Tail / Continuous thumbnails: "off", "head", "headTail" or "continuous".
    Q_PROPERTY(QString filmstripMode READ filmstripMode WRITE setFilmstripMode NOTIFY displayChanged FINAL)
    /// Waveforms drawn from the bottom (Premiere's Rectified Audio Waveforms) and on a dB scale.
    Q_PROPERTY(bool rectifiedWaveforms READ rectifiedWaveforms WRITE setRectifiedWaveforms NOTIFY displayChanged FINAL)
    Q_PROPERTY(bool logarithmicWaveforms READ logarithmicWaveforms WRITE setLogarithmicWaveforms NOTIFY displayChanged
                   FINAL)
    /// Coloured bands under video that uses source frames another clip also uses (Premiere's duplicate frame markers).
    Q_PROPERTY(bool showDuplicateFrames READ showDuplicateFrames WRITE setShowDuplicateFrames NOTIFY displayChanged
                   FINAL)
    /// Marks on cuts between clips that continue the same source (Premiere's Show Through Edits).
    Q_PROPERTY(bool showThroughEdits READ showThroughEdits WRITE setShowThroughEdits NOTIFY displayChanged FINAL)
    Q_PROPERTY(bool showClipMarkers READ showClipMarkers WRITE setShowClipMarkers NOTIFY displayChanged FINAL)
    /// M5l: the track fader or pan lane on audio tracks ("off", "volume", "pan"): the track's automation as a line
    /// across the row, its keys as squares.
    Q_PROPERTY(QString trackKeyframes READ trackKeyframes WRITE setTrackKeyframes NOTIFY displayChanged FINAL)
    /// Draw the snap line while a drag is snapped.
    Q_PROPERTY(bool showSnapIndicator READ showSnapIndicator WRITE setShowSnapIndicator NOTIFY displayChanged FINAL)
    /// Clicking a clip selects its linked clips too; Alt inverts it for one click (Premiere's Linked Selection).
    Q_PROPERTY(bool linkedSelection READ linkedSelection WRITE setLinkedSelection NOTIFY displayChanged FINAL)
    /// Trim edit points to draw (TrimBridge.points): [{ clip: "c…", edge: "head"|"tail" }], and their type
    /// ("regular", "ripple" or "roll").
    Q_PROPERTY(QVariantList trimPoints READ trimPoints WRITE setTrimPoints NOTIFY trimPointsChanged FINAL)
    Q_PROPERTY(QString trimType READ trimType WRITE setTrimType NOTIFY trimPointsChanged FINAL)

public:
    explicit TimelineView(QQuickItem* parent = nullptr);
    ~TimelineView() override;

    ProjectController* controller() const { return m_controller; }
    void setController(ProjectController* controller);
    QVariantMap style() const { return m_styleMap; }
    void setStyle(const QVariantMap& style);

    double pixelsPerSecond() const { return m_pps; }
    void setPixelsPerSecond(double pps);
    double zoomLevel() const;
    void setZoomLevel(double level);
    double scrollX() const { return m_scrollX; }
    void setScrollX(double x);
    double scrollY() const { return m_scrollY; }
    void setScrollY(double y);
    double contentWidth() const;
    double contentHeight() const { return m_contentHeight; }
    int visibleTrackCount() const;
    double playheadX() const;
    double markInX() const;
    double markOutX() const;
    QString hoveredClip() const { return clipKey(m_hovered); }
    bool marqueeActive() const { return m_marqueeActive; }
    QString tool() const { return toolKey(m_tool); }
    void setTool(const QString& tool);
    bool snapping() const { return m_snapping; }
    void setSnapping(bool snapping);
    bool gestureActive() const { return m_gesture.active; }
    QString gestureReadout() const { return m_gestureReadout; }
    QPointF gesturePoint() const { return m_gesturePoint; }
    double snapX() const;
    QString newTrackLabel() const;
    bool autoScrolling() const { return m_autoScrollTimer && m_autoScrollTimer->isActive(); }
    QString cursorKey() const { return m_cursorKey; }
    QVariantMap dropIndicator() const { return m_dropIndicator.shown ? m_dropIndicator.point : QVariantMap(); }
    /// Advances auto-scroll by `seconds` (the timer does this every frame; tests call it directly).
    void stepAutoScroll(double seconds);
    /// Scrolls so the clip is in view (keyboard selection).
    Q_INVOKABLE void revealClip(const QString& clipId);
    double rulerStepPx() const;
    double rulerFirstX() const;
    int rulerFirstIndex() const;
    int rulerStepFrames() const;
    int rulerSubdivisions() const;
    QVariantList markerPositions() const;
    bool showVolume() const { return m_showVolume; }
    void setShowVolume(bool show);
    /// The volume rubber band is drawn: shown for the selection tool, or the pen tool is on (0.1.2).
    bool volumeBandShown() const { return m_showVolume || m_tool == TimelineTool::Pen; }
    bool showSpeed() const { return m_showSpeed; }
    void setShowSpeed(bool show);
    QString speedReadout() const { return m_speedReadout; }
    bool playing() const { return m_playing; }
    void setPlaying(bool playing);

    // ---- M5i: track heights (view state, per track) ----
    int layoutRevision() const { return m_layoutRevision; }
    /// The height of a track's row in px.
    Q_INVOKABLE double trackHeightOf(const QString& trackId) const;
    /// Sets one track's height (clamped to the style's min/max).
    Q_INVOKABLE void setTrackHeight(const QString& trackId, double height);
    /// Changes every "video", "audio" or "all" track by `steps` steps (Ctrl/Alt +/-, Shift+=).
    Q_INVOKABLE void changeTrackHeights(const QString& kind, int steps);
    /// Every track at the minimum height (Premiere's Minimize All Tracks).
    Q_INVOKABLE void minimizeAllTracks();
    Q_INVOKABLE void resetTrackHeights();
    /// The current heights as a preset { video: [V1, V2, ...], audio: [A1, ...] } and back (Premiere's track height
    /// presets: tracks past the preset's lists keep their height).
    Q_INVOKABLE QVariantMap trackHeightPreset() const;
    Q_INVOKABLE void applyTrackHeightPreset(const QVariantMap& preset);

    // ---- M5i: display settings ----
    QString filmstripMode() const { return m_filmstripMode; }
    void setFilmstripMode(const QString& mode);
    bool rectifiedWaveforms() const { return m_rectified; }
    void setRectifiedWaveforms(bool on);
    bool logarithmicWaveforms() const { return m_logarithmic; }
    void setLogarithmicWaveforms(bool on);
    bool showDuplicateFrames() const { return m_showDuplicates; }
    void setShowDuplicateFrames(bool on);
    bool showThroughEdits() const { return m_showThroughEdits; }
    void setShowThroughEdits(bool on);
    bool showClipMarkers() const { return m_showClipMarkers; }
    void setShowClipMarkers(bool on);
    QString trackKeyframes() const { return m_trackKeyframes; }
    void setTrackKeyframes(const QString& mode);
    bool showSnapIndicator() const { return m_showSnapIndicator; }
    void setShowSnapIndicator(bool on);
    bool linkedSelection() const { return m_linkedSelection; }
    void setLinkedSelection(bool on);
    QVariantList trimPoints() const { return m_trimPointList; }
    void setTrimPoints(const QVariantList& points);
    QString trimType() const { return m_trimType; }
    void setTrimType(const QString& type);
    QVariantList markerItems() const;
    /// The sequence marker under the ruler x (within a few px), or -1.
    Q_INVOKABLE int markerAt(double x) const;
    /// The time under x as a frame-quantised key for marker drags: "ticks" text (principle 9: opaque).
    Q_INVOKABLE QString timeKeyAt(double x) const;

    // ---- Volume rubber band (M5g; TimelineViewVolume.cpp) ----
    /// Where `db` sits on the rubber band: 0 at its top (+6 dB) to 1 at its bottom (-60 dB and below), 0 dB at 0.25.
    static double volumeFraction(double db);
    static double volumeDbAtFraction(double fraction);
    /// The band's vertical extent inside an audio clip's rectangle.
    static QRectF volumeArea(const QRectF& clipRect);

    // ---- Hit-testing and actions (view coordinates) ----
    /// Opaque id of the clip under (x, y), or "".
    Q_INVOKABLE QString clipAt(double x, double y) const;
    /// Timecode of the frame under x.
    Q_INVOKABLE QString timeAt(double x) const;
    /// Opaque id of the track under y, or "".
    Q_INVOKABLE QString trackAt(double y) const;
    /// Label of ruler tick `tickIndex` (`tickIndex * stepFrames` frames from the start).
    Q_INVOKABLE QString rulerLabel(int tickIndex, int stepFrames) const;
    /// Ruler scrub: moves the playhead to the frame under x.
    Q_INVOKABLE void seekToX(double x);
    /// Zooms to `level` keeping the time under `anchorX` in place.
    Q_INVOKABLE void zoomAt(double level, double anchorX);
    Q_INVOKABLE void scrollBy(double dx, double dy);
    /// Scrolls so the playhead is visible (keyboard stepping).
    Q_INVOKABLE void revealPlayhead();
    /// Scrolls the tracks so the audio tracks are in view (the Audio workspace).
    Q_INVOKABLE void revealAudioTracks();
    /// Click selection: modifiers = Qt.KeyboardModifiers of the click.
    Q_INVOKABLE void selectAt(double x, double y, int modifiers);
    /// Marquee selection of every clip intersecting `rect` (view coordinates).
    Q_INVOKABLE void selectInRect(const QRectF& rect, int modifiers);
    /// Esc: drops the drag in progress and restores the timeline (nothing was committed). False when idle.
    Q_INVOKABLE bool cancelGesture();

    // ---- Effects and transitions dragged from the Effects panel (M5d) ----
    /// What dropping `effectId` at (x, y) would do, for the drop highlight and its tooltip:
    /// { ok, warning, kind: "clip" | "edit", message, x, y, width, height (view coordinates of the clip or the
    /// transition it would make) }. A transition snaps to the nearest edit point and is dry-run: "using available
    /// handles" when the media is short, or the model's reason when it cannot go there.
    Q_INVOKABLE QVariantMap effectDropTarget(double x, double y, const QString& effectId) const;
    /// Drops `effectId` at (x, y): an effect onto the clip there, a transition onto the nearest edit point (it is
    /// selected afterwards). Returns "" or a message saying why not.
    Q_INVOKABLE QString dropEffect(double x, double y, const QString& effectId);
    /// The transition under (x, y) ("x…"), or "".
    Q_INVOKABLE QString transitionAt(double x, double y) const;

    // ---- Drops from outside the lanes (0.1.3) ----
    /// Where a drop from outside the lanes (a Library card, the Source Monitor, a search hit, a graphics template) at
    /// (x, y) lands: { time (timecode of the frame), x (its view x), snapped, track ("" off the tracks), rowY,
    /// rowHeight (view coordinates; the lanes' height off the tracks) }. With snapping on, the time snaps to the
    /// nearest clip edge (the end of the clip in front, typically), the playhead, a mark, a marker or a beat within the
    /// snap threshold, rounded to a frame.
    Q_INVOKABLE QVariantMap dropPoint(double x, double y) const;
    /// Shows (or moves) the insertion line of such a drag at (x, y), in the snap colour with the snap tick when it
    /// snaps, and returns dropPoint(x, y).
    Q_INVOKABLE QVariantMap showDropIndicator(double x, double y);
    Q_INVOKABLE void clearDropIndicator();

    // ---- C++ ----
    const TimelineStats& stats() const { return m_stats; }
    /// Peaks for audio waveforms (default: PlaceholderPeakSource). Must outlive no render: shared ownership.
    void setPeakSource(std::shared_ptr<const PeakSource> source);
    /// Thumbnails for video filmstrips (default: none). New atlases need filmstripsChanged() to be drawn.
    void setFilmstripSource(std::shared_ptr<const FilmstripSource> source);
    /// Atlases arrived: rebuilds the video tracks.
    void filmstripsChanged();
    const TimelineStyle& parsedStyle() const { return m_style; }

    struct Row {
        TrackPtr track;
        qreal y = 0;
        qreal height = 0;
        bool audio = false;
    };
    const std::vector<Row>& rows() const { return m_rows; }

    // ---- External preview (AI assistant; owner: AI platform engineer; TimelineViewPreview.cpp) ----
    // A read-only picture of an edit that is not applied yet, drawn over the current sequence by an overlay child
    // item: `removed` ranges struck through, `added` content as ghost clips, `markers` as ghost marker lines. It
    // never touches the model, the selection or the gesture state, and nothing in it is editable; the owner
    // (AgentController) replaces or clears it, e.g. when the model changes.
    struct ExternalPreview {
        struct Range {
            TrackId track;
            Ticks start;
            Ticks end;
        };
        std::vector<Range> removed;
        std::vector<Range> added;
        std::vector<Ticks> markers;
        /// A clip's volume as it would be (M5g: auto-duck): drawn as a dashed rubber band over the clip.
        struct VolumeCurve {
            TrackId track;
            Ticks start;
            Ticks end;
            std::vector<std::pair<Ticks, double>> points; ///< timeline time, dB
        };
        std::vector<VolumeCurve> volumes;
    };
    void setExternalPreview(ExternalPreview preview);
    Q_INVOKABLE void clearExternalPreview();
    const ExternalPreview* externalPreview() const { return m_externalPreview.get(); }

signals:
    void controllerChanged();
    void styleChanged();
    void viewChanged();
    void layoutChanged();
    void playheadXChanged();
    void hoveredClipChanged();
    void marqueeChanged();
    /// A mouse press landed on the view (QML moves keyboard focus here).
    void pressed();
    void toolChanged();
    void snappingChanged();
    void gestureChanged();
    /// Right click on a clip (already selected): QML opens the clip context menu at (x, y).
    void contextMenuRequested(const QString& clipId, double x, double y);
    /// A double-click on a clip (M5l: a nested or multi-camera clip opens its sequence).
    void clipDoubleClicked(const QString& clipId);
    /// M5m, the Remix tool: the user dragged music clip `clipId`'s end to propose a new length (seconds); (x, y) is where
    /// the drag ended, for the Remix popover.
    void remixRequested(const QString& clipId, double durationSeconds, double x, double y);
    void autoScrollingChanged();
    void cursorKeyChanged();
    void dropIndicatorChanged();
    void showVolumeChanged();
    void showSpeedChanged();
    void playingChanged();
    void displayChanged();
    void trimPointsChanged();
    /// A click (no drag) on a clip's edge with the Selection, Ripple or Roll tool: QML selects that edit point
    /// (TrimBridge). `type` is the tool's trim ("regular", "ripple", "roll"); Ctrl/⌘ makes a Selection-tool click a
    /// ripple (Premiere).
    void editPointClicked(const QString& clipId, const QString& edge, const QString& type, int modifiers);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void hoverEnterEvent(QHoverEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    friend class TimelineRootNode;

    void refreshFromController();
    /// Displays `seq` (a preview or the model's sequence) and marks the tracks that differ from the one shown.
    void showSequence(SequencePtr seq);
    /// Starts the motion from what is shown (`from`) to `to`: shifted clips slide, new cuts flash.
    void animateChange(const Sequence& from, const Sequence& to);
    void startAnimation(QVariantAnimation* animation, int duration);
    /// Collects the ChangeSet ranges of the last change for the next frame.
    void onModelChanged();
    void refreshSelection();
    void relayoutRows();
    void clampScroll();
    /// Linked selection for a click with `mods` (Alt inverts the setting).
    bool followLinks(Qt::KeyboardModifiers mods) const { return m_linkedSelection != bool(mods & Qt::AltModifier); }
    qreal defaultTrackHeight(bool audio) const;
    /// Saves the zoom and scroll of the sequence shown and restores those of `next` (sequence tabs).
    void switchSequenceView(SequenceId next);
    void bumpDisplay();
    void setHovered(ClipId clip);

    // ---- Gestures (TimelineViewInput.cpp) ----
    enum class Zone { None, Body, Head, Tail, VolumeLine, VolumeKey, SpeedLine, SpeedKey };
    struct Hit {
        ClipId clip;
        int row = -1;
        Zone zone = Zone::None;
        TransitionId transition; ///< set instead of `clip` when a transition covers the point (Head/Tail = its edges)
        int volumeKey = -1;      ///< VolumeKey: the key's index in the clip's volume curve
    };
    /// The rubber band under (x, y) of an audio clip hit (`hit`): a key, the line, or `hit` unchanged.
    Hit volumeHit(double x, double y, Hit hit) const;
    void pressVolume(const Hit& hit, Qt::KeyboardModifiers mods);
    void dragVolume(QPointF pos, Qt::KeyboardModifiers mods);
    void releaseVolume();
    Hit speedHit(double x, double y, Hit hit) const;
    void pressSpeed(const Hit& hit);
    void dragSpeed(QPointF pos);
    void releaseSpeed(QPointF pos);
    /// The transition on `row` under x, if any (select tool), with its edge zone.
    Hit transitionHit(double x, int row) const;
    /// [start, end) a transition covers on its track, from its clips.
    static std::optional<TimeRange> transitionRange(const Track& track, const Transition& transition);
    /// Where a transition dropped at x on `row` would go: the clip and edge of the nearest edit point.
    struct EdgeTarget {
        ClipId clip;
        edit::Edge edge = edit::Edge::Head;
    };
    std::optional<EdgeTarget> edgeTargetAt(double x, int row) const;
    /// What a drop of `effectId` at (x, y) does (effectDropTarget / dropEffect).
    struct DropPlan {
        bool ok = false, warning = false;
        QString kind, message;
        QRectF rect; ///< view coordinates
        ClipId clip;
        std::optional<edit::EdgeTransition> transition;
        edit::HandleFit fit = edit::HandleFit::Exact;
    };
    DropPlan planDrop(double x, double y, const QString& effectId) const;
    void dragTransitionEdge(QPointF pos);
    Hit hitTest(double x, double y) const;
    GestureKind gestureFor(const Hit& hit, Qt::KeyboardModifiers mods) const;
    void pressClip(const Hit& hit, Qt::KeyboardModifiers mods);
    void trackSelect(double x, int row, bool backward, bool oneTrack);
    void razorClick(double x, int row, Qt::KeyboardModifiers mods);
    bool startGesture();
    void updateGesture(QPointF pos, Qt::KeyboardModifiers mods);
    void finishGesture();
    void updateHoverCursor(QPointF pos, Qt::KeyboardModifiers mods);
    // ---- Edit cursors (0.1.3, EditCursors.h) ----
    /// What hovering `hit` with `mods` means for the active tool, as a cursor key ("" = the arrow).
    QString hoverCursorKey(const Hit& hit, Qt::KeyboardModifiers mods) const;
    /// The cursor of the drag in progress.
    QString gestureCursorKey(Qt::KeyboardModifiers mods) const;
    /// Shows the cursor `key` (no-op when it is already shown).
    void applyCursor(const QString& key);
    /// The clip edge under the pointer that a press would edit, highlighted in the lanes like a trim edit point.
    enum class EdgeKind { None, Trim, Ripple, Roll, Rate, Remix };
    struct HoverEdge {
        ClipId clip;
        int row = -1;
        bool head = false;
        EdgeKind kind = EdgeKind::None;
        bool operator==(const HoverEdge&) const = default;
    };
    HoverEdge hoverEdgeFor(const Hit& hit, Qt::KeyboardModifiers mods) const;
    void setHoverEdge(const HoverEdge& edge);
    /// A modifier went down or up: the hover cursor (Ctrl: ripple) and a move in progress (Ctrl: insert) follow at once.
    void modifiersChanged(Qt::KeyboardModifiers mods);
    /// Frame-quantised time of a drop at x, snapped (see dropPoint()).
    Ticks dropTimeAt(double x, std::optional<Ticks>* snappedAt = nullptr) const;
    /// Starts or stops auto-scroll for a drag at `pos`.
    void updateAutoScroll(QPointF pos);
    void updateMarquee(QPointF pos);
    /// Frame-quantised time under x, snapped to a target when snapping is on (razor).
    Ticks snappedTimeAt(double x, std::optional<Ticks>* snappedAt = nullptr) const;
    Ticks snapTolerance() const;
    /// Snap positions near `t`: the playhead, marks, markers and the clip edges around `t` on every track.
    std::vector<Ticks> localSnapTargets(Ticks t) const;
    /// The row under y, or the nearest one (above the first row: 0; below the last: the last).
    int nearestRow(double y) const;
    /// Rebuilds the tracks that hold any of `clips` while an animation runs.
    void markTracksWith(const std::unordered_set<std::uint64_t>& clips);
    double fitPixelsPerSecond() const;
    double maxPixelsPerSecond() const;
    Rate frameRate() const;
    int rowAt(double y) const;
    ClipId clipIdAt(double x, double y) const;
    Ticks ticksAtX(double x) const;
    double xOfTicks(Ticks t) const; // view coordinates
    std::vector<ClipId> clipsInRect(const QRectF& rect) const;
    struct RulerStep {
        std::int64_t frames = 24;
        int subdivisions = 4;
        double px = 0;
    };
    RulerStep rulerStep() const;
    void updateRoot(TimelineRootNode* root);

    QPointer<ProjectController> m_controller;
    QVariantMap m_styleMap;
    TimelineStyle m_style;
    int m_styleRevision = 0;
    QString m_introText; // M5o: the responsive bands' tags
    QString m_outroText;
    int m_layoutRevision = 0;

    SequencePtr m_sequence;
    /// Changes since the last frame: rebuild everything, or these time ranges per track id.
    bool m_pendingAll = true;
    /// What the lanes draw from media items (offline hatching, the AI badge) as of the last full build: a media change
    /// that leaves both alike rebuilds nothing (review P2-46).
    std::unordered_set<std::uint64_t> m_drawnOffline;
    std::unordered_set<std::uint64_t> m_drawnGenerated;
    /// The project's redactions as of the last full build: filmstrips hide the frames they cover (review P2-9).
    std::vector<Redaction> m_drawnRedactions;
    int m_markerRevision = 0;
    std::unordered_map<std::uint64_t, std::vector<TimeRange>> m_pendingRanges;
    Ticks m_contentDuration = Ticks::fromSeconds(30);
    std::vector<Row> m_rows;
    qreal m_contentHeight = 0;
    std::unordered_set<std::uint64_t> m_selected;
    ClipId m_primary;
    QString m_selectedEdge; // "head", "tail" or "" (keyboard trims)
    TransitionId m_selectedTransition;
    struct TransitionDrag {
        TransitionId id;
        bool left = false; ///< the left edge (before the cut) or the right one
        std::string mergeKey;
    } m_transitionDrag;
    int m_transitionDragCounter = 0;
    int m_selectionRevision = 0;
    Ticks m_playhead;

    double m_pps = 48.0;
    double m_scrollX = 0;
    double m_scrollY = 0;

    ClipId m_hovered;
    double m_hoverProgress = 0;
    QVariantAnimation* m_liftAnimation = nullptr;

    enum class Drag { None, Click, Marquee, Pending, Gesture, Cancelled, TransitionEdge, VolumeKey, VolumeLine, Speed, Remix };
    /// M5m: the Remix tool's drag of a music clip's end (a proposed length; the model is not touched).
    struct RemixDrag {
        ClipId clip;
        int row = -1;
        Ticks start;
        Ticks end;    ///< the clip's end now
        Ticks target; ///< the proposed end
    };
    std::optional<RemixDrag> m_remix;
    Drag m_drag = Drag::None;
    Qt::KeyboardModifiers m_pressMods;
    ClipId m_pressClip;
    bool m_pressWasSelected = false;
    Hit m_pressHit;
    GestureKind m_pendingKind = GestureKind::None;

    // Editing state
    TimelineTool m_tool = TimelineTool::Select;
    bool m_snapping = true;
    struct Gesture {
        GestureSpec spec;
        ClipId grabbed;
        bool grabbedVideo = true;
        int grabbedTrackIndex = 0; // V1/A1 = 0
        std::vector<Ticks> targets; // sorted snap targets
        std::vector<Ticks> edges;   // edges of the dragged material at press time
        std::optional<Ticks> snappedAt;
        Ticks appliedDelta;
        std::shared_ptr<const ProjectState> preview; // last valid preview
        struct Ghost {
            Ticks start, end;
            TrackId track; // rows can change in the preview (a new track): looked up when drawn
        };
        std::vector<Ghost> ghosts;
        bool active = false;
    } m_gesture;
    QString m_gestureReadout;
    QPointF m_gesturePoint;
    // Motion: lifted (dragged / settling) clips, shifted clips sliding into place, razor flashes.
    std::unordered_set<std::uint64_t> m_lifted;
    double m_liftAmount = 0;     // 1 while dragging; springs to 0 on drop (OutBack: dips below 0 briefly)
    QVariantAnimation* m_settleAnimation = nullptr;
    std::unordered_map<std::uint64_t, Ticks> m_shiftFrom; // clip -> old start - new start
    double m_shiftProgress = 1.0;
    QVariantAnimation* m_shiftAnimation = nullptr;
    struct Flash {
        Ticks at;
        int row = 0;
    };
    std::vector<Flash> m_flashes;
    double m_flashProgress = 1.0;
    QVariantAnimation* m_flashAnimation = nullptr;
    std::unordered_set<std::uint64_t> m_animatedTracks; // rebuilt every frame while lift/shift animations run
    double m_hoverX = 0;
    QPointF m_hoverPos;
    int m_hoverRow = -1;
    // Edit cursors and the hovered edge (0.1.3)
    QString m_cursorKey;
    bool m_cursorSet = false; ///< a cursor was set for m_cursorKey with the current style's colours
    HoverEdge m_hoverEdge;
    // The insertion line of a drag from outside the lanes (0.1.3)
    struct DropIndicator {
        bool shown = false;
        Ticks at;
        int row = -1;
        bool snapped = false;
        QVariantMap point; ///< dropPoint() at the last position
    } m_dropIndicator;
    bool m_hoverAllTracks = false;
    bool m_hoverInside = false;
    // Auto-scroll during drags
    QTimer* m_autoScrollTimer = nullptr;
    QElapsedTimer m_autoScrollClock;
    QPointF m_lastPointer;
    Qt::KeyboardModifiers m_lastMods;
    QPointF m_scrollVelocity; // smoothed px/s
    // Model rows at the start of a move (a new track in the preview must not change them)
    double m_modelRowsBottom = 0;
    int m_modelAudioRows = 0;
    QPointF m_pressPos;
    QRectF m_marquee;
    bool m_marqueeActive = false;
    int m_marqueeRevision = 0;
    std::vector<ClipId> m_marqueeBase;

    std::shared_ptr<const PeakSource> m_peaks;
    std::shared_ptr<const FilmstripSource> m_filmstrips;
    TimelineStats m_stats;

    // ---- M5i ----
    std::unordered_map<std::uint64_t, qreal> m_trackHeights; // track id -> height (unset: the style's default)
    struct SequenceView {
        double pps = 0, scrollX = 0, scrollY = 0;
    };
    std::unordered_map<std::uint64_t, SequenceView> m_sequenceViews;
    SequenceId m_shownSequence;
    QString m_filmstripMode = QStringLiteral("continuous");
    bool m_rectified = false;
    bool m_logarithmic = false;
    bool m_showDuplicates = false;
    bool m_showThroughEdits = true;
    bool m_showClipMarkers = true;
    QString m_trackKeyframes = QStringLiteral("off");
    /// What the lanes draw beside the tracks themselves (nested names, multi-camera angles, Enhance Speech badges,
    /// the faders of the track lanes): a change to them rebuilds everything.
    TimelineLaneFacts m_drawnFacts;
    /// Every clip's facts, computed from the current sequence (a walk of every clip).
    TimelineLaneFacts laneFacts() const;
    /// M5n (P.1): laneFacts() of the current snapshot, cached: recomputed when the snapshot, the sequence or the
    /// lanes shown changed in a way updateFacts() did not follow. The model-change handler and updatePaintNode both
    /// read it (it used to be computed twice per edit, walking every clip each time).
    const TimelineLaneFacts& currentFacts();
    /// Follows a clip-level edit of the current sequence in the cached facts (only its added, removed and modified
    /// clips); anything wider drops the cache.
    void updateFacts(const TimelineChange& change);
    TimelineLaneFacts m_facts;
    ProjectSnapshot m_factsSnapshot; ///< the snapshot m_facts describes (null: none)
    SequenceId m_factsSequence;
    QString m_factsKeyframes;
    bool m_showSnapIndicator = true;
    bool m_linkedSelection = true;
    QVariantList m_trimPointList;
    struct TrimPoint {
        ClipId clip;
        bool head = false;
    };
    std::vector<TrimPoint> m_trimPoints;
    QString m_trimType = QStringLiteral("roll");

    // ---- Volume rubber band (TimelineViewVolume.cpp) ----
    bool m_showVolume = false;
    bool m_showSpeed = false;
    QString m_speedReadout;
    struct SpeedDrag {
        int key = -1;
        bool left = false;
        double percent = 100;
    } m_speedDrag;
    bool m_playing = false;
    struct VolumeDrag {
        ClipId clip;
        int key = -1;          ///< the dragged key, or -1 for the line
        ParamValue original;   ///< the volume at the press
        Ticks pressTime;       ///< timeline time of the press
        double pressDb = 0;
        bool moved = false;
        std::string mergeKey;
    } m_volumeDrag;
    int m_volumeDragCounter = 0;

    // ---- External preview (TimelineViewPreview.cpp) ----
    friend class ExternalPreviewItem;
    std::unique_ptr<ExternalPreview> m_externalPreview;
    QPointer<QQuickItem> m_externalPreviewItem;
};

} // namespace wabcut::app
