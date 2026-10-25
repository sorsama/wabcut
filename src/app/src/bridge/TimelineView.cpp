#include "TimelineView.h"

#include "AudioChannelsBridge.h"
#include "BridgeIds.h"
#include "ClipAudioBridge.h"
#include "TimelineAnalysis.h"
#include "TimelineGeometry.h"
#include "TimeRemapBridge.h"

#include <wabcut/audio/edit/Loudness.h>
#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Snap.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QHoverEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGTextNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSGTransformNode>
#include <QStyleHints>
#include <QTextLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

namespace wabcut::app {

namespace {

QColor colorFrom(const QVariant& v, QColor fallback) {
    if (!v.isValid())
        return fallback;
    if (v.metaType() == QMetaType::fromType<QColor>())
        return v.value<QColor>();
    const QColor c(v.toString());
    return c.isValid() ? c : fallback;
}

std::vector<QColor> colorsFrom(const QVariant& v) {
    std::vector<QColor> out;
    for (const QVariant& item : v.toList()) {
        const QColor c = colorFrom(item, {});
        if (c.isValid())
            out.push_back(c);
    }
    return out;
}

qreal realFrom(const QVariantMap& m, const char* key, qreal fallback) {
    const QVariant v = m.value(QLatin1String(key));
    bool ok = false;
    const qreal r = v.toDouble(&ok);
    return ok ? r : fallback;
}

QColor indexed(const std::vector<QColor>& list, int index, QColor fallback) {
    if (list.empty())
        return fallback;
    const int n = static_cast<int>(list.size());
    return list[static_cast<std::size_t>(((index % n) + n) % n)];
}

QColor withAlpha(QColor c, qreal alpha) {
    c.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0) * c.alphaF()));
    return c;
}

QColor transparent(QColor c) {
    c.setAlpha(0);
    return c;
}

// [seconds, minor subdivisions] — labels on "nice" timecode intervals.
constexpr std::array<std::array<int, 2>, 11> kRulerIntervals{{
    {1, 4}, {2, 4}, {5, 5}, {10, 5}, {15, 3}, {30, 6}, {60, 4}, {300, 5}, {600, 5}, {1800, 6}, {3600, 4},
}};

int countNodes(const QSGNode* node) {
    int n = 1;
    for (const QSGNode* child = node->firstChild(); child; child = child->nextSibling())
        n += countNodes(child);
    return n;
}

/// 0.4: what a clip's speed suffix says ("" for a clip at a uniform 100 % forward, not remapped): "50%", "−100%"
/// (reversed), "Variable speed" (a remap that changes speed), "Hold" (a frame hold). Shared by the label and the
/// hover tooltip.
QString clipSpeedText(const Clip& c) {
    if (c.isRemapped())
        return TimeRemapBridge::speedLabel(c);
    if (c.isFreeze())
        return QCoreApplication::translate("wabcut::app::TimelineView", "Hold");
    if (c.speed.isUnity() && !c.reversed)
        return {};
    QString text = TimeRemapBridge::speedLabel(c); // "-100%" when reversed
    if (text.startsWith(QLatin1Char('-')))
        text[0] = QChar(0x2212); // the typographic minus, as the drag readouts
    else if (c.reversed)
        text.prepend(QChar(0x2212));
    return text;
}

/// 0.4: what a clip's fx chip says: Effects with stack effects (they win), Changed when an intrinsic (Motion, Opacity,
/// Volume, Pan) is off its default or keyframed, the blend mode is not Normal or the clip is time-remapped, else None.
ClipFx clipFxOf(const Clip& c) {
    if (!c.effects().empty())
        return ClipFx::Effects;
    if (c.blendMode() != BlendMode::Normal || c.isRemapped())
        return ClipFx::Changed;
    const ClipProperties& props = c.properties();
    for (std::size_t i = 0; i < kParamCount; ++i) {
        if (!props.isDefault(static_cast<ParamId>(i)))
            return ClipFx::Changed;
    }
    return ClipFx::None;
}

} // namespace

QColor TimelineStyle::clipBody(int index) const {
    return indexed(clipBodyColors, index, clipColor(index));
}

QColor TimelineStyle::clipColor(int index) const {
    return indexed(clipColors, index, QColor(0xCE, 0xFA, 0x44));
}
QColor TimelineStyle::audioFill(int index) const {
    return indexed(audioFillColors, index, withAlpha(clipColor(index), 0.25));
}
QColor TimelineStyle::audioBorder(int index) const {
    return indexed(audioBorderColors, index, withAlpha(clipColor(index), 0.55));
}
QColor TimelineStyle::waveform(int index) const {
    return indexed(waveformColors, index, withAlpha(clipColor(index), 0.85));
}
QColor TimelineStyle::markerColor(int index) const {
    return indexed(markerColors, index, clipColor(index));
}

// =====================================================================================================
// Scene-graph state. Everything that points into the node tree lives here, so when the scene graph is
// released (window hidden, offscreen grab) the whole state goes with it and the next frame starts clean.

namespace {

struct LabelEntry {
    QSGTextNode* node = nullptr;
    QSGTransformNode* badge = nullptr; // sibling, follows the label's sticky position
    int badgeStyleRevision = -1;
    QString badgeText;                 // the micro-tag it shows ("AI", "ENHANCED", ...)
    QSGTransformNode* fx = nullptr;    // 0.4: the fx chip (sibling, before the micro-tag)
    ClipFx fxState = ClipFx::None;     // what it shows
    int fxStyleRevision = -1;
    bool fxInverted = false;           // drawn as a clipInk pill (its colour is the clip's own)
    bool pending = false;
    QString text;
    QColor color;
    qreal x = 0;
    qreal y = 0;
};

// Index of the first clip that ends after `t`. Clips are sorted and never overlap, so this is a binary search
// on the chunked ClipList (its iterators are bidirectional: std::partition_point would walk every clip).
std::size_t firstClipEndingAfter(const ClipList& clips, Ticks t) {
    std::size_t i = clips.upperBoundStart(t);
    if (i > 0 && clips[i - 1].end() > t)
        --i;
    return i;
}

// M5o: an "Intro" / "Outro" tag on a responsive graphic's band (its text node under a transform that places it).
struct RegionTag {
    QSGTransformNode* node = nullptr;
    int styleRevision = -1;
    QString text;
};

void removeRegionTag(RegionTag& tag) {
    if (tag.node) {
        if (QSGNode* parent = tag.node->parent())
            parent->removeChildNode(tag.node);
        delete tag.node;
    }
    tag.node = nullptr;
}

struct BuiltClip {
    ClipId id;
    std::size_t index = 0; // into track->clips
    qreal x0 = 0, x1 = 0, y = 0, h = 0;
    bool audio = false;
    bool thumbs = false; // a filmstrip (or its placeholders): the label sits at the top on a scrim
    qreal header = 0;    // v3: the solid band above the filmstrip that carries the label (0: the scrim)
};

// Detached text nodes kept for reuse: creating and destroying QSGTextNodes (and their glyph nodes) is what
// made zoom frames expensive when many labels appear or disappear at once.
struct LabelPool {
    std::vector<QSGTextNode*> spare;
    LabelPool() = default;
    LabelPool(const LabelPool&) = delete;
    LabelPool& operator=(const LabelPool&) = delete;
    ~LabelPool() {
        for (QSGTextNode* node : spare)
            delete node;
    }
    void release(QSGTextNode* node) {
        if (QSGNode* parent = node->parent())
            parent->removeChildNode(node);
        if (spare.size() < 512)
            spare.push_back(node);
        else
            delete node;
    }
    QSGTextNode* take(QQuickWindow* window) {
        if (spare.empty())
            return window->createTextNode();
        QSGTextNode* node = spare.back();
        spare.pop_back();
        return node;
    }
};

// Width of each clip name in the label font, and the elided form of a name at a width, cached across frames (render
// thread only). Eliding shapes the whole string (HarfBuzz) every time: P.1 measured it at about half of a track
// rebuild, for names that repeat (a clip keeps its media's name) at widths that repeat (quantised to 8 px). Cleared
// when the style (the label font, the UI scale) changes.
struct NameWidths {
    QHash<QString, qreal> widths;
    QHash<QPair<QString, int>, QString> elided;
    qreal width(const QFontMetricsF& metrics, const QString& name) {
        auto it = widths.constFind(name);
        if (it != widths.constEnd())
            return *it;
        if (widths.size() > 50000)
            widths.clear();
        return *widths.insert(name, metrics.horizontalAdvance(name));
    }
    /// metrics.elidedText(name, Qt::ElideRight, width) for a width already quantised to whole pixels.
    QString elide(const QFontMetricsF& metrics, const QString& name, qreal width) {
        const QPair<QString, int> key{name, static_cast<int>(width)};
        auto it = elided.constFind(key);
        if (it != elided.constEnd())
            return *it;
        if (elided.size() > 50000)
            elided.clear();
        return *elided.insert(key, metrics.elidedText(name, Qt::ElideRight, width));
    }
    void clear() {
        widths.clear();
        elided.clear();
    }
};

struct TrackBucket {
    TrackPtr track; // keeps the rendered node alive so pointer comparison is reliable
    QSGGeometryNode* mesh = nullptr;
    // M5i: thumbnails (one textured node per media atlas), then what is drawn over them (hatching, label scrims,
    // clip markers, through edits, duplicate frame bands).
    std::vector<QSGGeometryNode*> film;
    QSGGeometryNode* top = nullptr;
    std::vector<BuiltClip> clips;
    std::unordered_map<std::uint64_t, LabelEntry> labels;
    std::vector<std::size_t> stuck; // indices into `clips` whose label is pinned to the left edge
    int nestedClips = 0; // M5l: clips drawn with the nested / multi-camera stripe
    int angleCuts = 0;   // M5l: angle-cut marks
    bool trackLane = false; // M5l: the track's fader / pan lane is drawn
    // M5o: graphic clips with responsive time: their band widths (intro, outro) and the "Intro" / "Outro" tags
    // (key: clip id * 2, + 1 for the outro), kept across rebuilds like the labels' micro-tags.
    std::vector<std::pair<std::uint64_t, QPair<qreal, qreal>>> responsiveBands;
    std::unordered_map<std::uint64_t, RegionTag> regionTags;
    bool visible = false;
    qreal y = -1, h = -1;
};

} // namespace

class TimelineRootNode : public QSGNode {
public:
    TimelineRootNode() {
        lanes = MeshBuilder::createNode();
        ghosts = MeshBuilder::createNode();
        content = new QSGTransformNode;
        markers = MeshBuilder::createNode();
        overlay = MeshBuilder::createNode();
        labels = new QSGNode;
        playhead = MeshBuilder::createNode();
        guides = MeshBuilder::createNode();
        marquee = MeshBuilder::createNode();
        appendChildNode(lanes);
        appendChildNode(ghosts);
        appendChildNode(content);
        content->appendChildNode(markers);
        content->appendChildNode(overlay);
        content->appendChildNode(labels);
        appendChildNode(playhead);
        appendChildNode(guides);
        appendChildNode(marquee);
    }

    QSGGeometryNode* lanes = nullptr;
    QSGGeometryNode* ghosts = nullptr; // where dragged clips came from, under the clips (view coordinates)
    QSGTransformNode* content = nullptr; // translate(originX - scrollX, -scrollY)
    QSGGeometryNode* markers = nullptr;
    QSGGeometryNode* overlay = nullptr;
    QSGNode* labels = nullptr;
    QSGGeometryNode* playhead = nullptr;
    QSGGeometryNode* guides = nullptr; // ghost outlines, snap line, razor preview, cut flashes (view coordinates)
    QSGGeometryNode* marquee = nullptr;
    std::vector<TrackBucket> buckets;

    // Filmstrip atlases uploaded as textures (M5i), per media item; swept after each full build.
    struct AtlasTexture {
        std::shared_ptr<const FilmstripAtlas> atlas;
        QSGTexture* texture = nullptr;
        int usedInBuild = 0;
    };
    std::unordered_map<std::uint64_t, AtlasTexture> textures;
    int buildSerial = 0;
    ~TimelineRootNode() override {
        for (auto& [id, t] : textures)
            delete t.texture;
    }

    // Parameters of the current build.
    double pps = -1, originX = 0, builtStartX = 0, builtEndX = 0, width = -1, height = -1;
    int markerRevision = -1;
    bool ghostsShown = false;
    double lanesScrollY = -1, lanesWidth = -1, lanesHeight = -1, contentHeight = -1;
    int styleRevision = -1, layoutRevision = -1, selectionRevision = -1, marqueeRevision = -1;
    SequencePtr sequence;
    ClipId hovered;
    double hoverProgress = -1;

    MeshBuilder mesh; // scratch
    MeshBuilder top;  // scratch: what goes over the thumbnails
    std::vector<float> peaks; // scratch
    NameWidths names;
    LabelPool pool; // declared last: its spare nodes are deleted before anything they might refer to
};

// =====================================================================================================

TimelineView::TimelineView(QQuickItem* parent)
    : QQuickItem(parent), m_peaks(std::make_shared<PlaceholderPeakSource>()),
      m_filmstrips(std::make_shared<NoFilmstripSource>()) {
    setFlag(ItemHasContents);
    setClip(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    //: The protected start of a responsive motion graphic, tagged on its clip in the timeline
    m_introText = tr("Intro");
    //: The protected end of a responsive motion graphic, tagged on its clip in the timeline
    m_outroText = tr("Outro");
    setAcceptHoverEvents(true);
    m_style.labelFont.setPixelSize(12);
    m_style.labelFont.setWeight(QFont::DemiBold);
    m_liftAnimation = new QVariantAnimation(this);
    m_liftAnimation->setStartValue(0.0);
    m_liftAnimation->setEndValue(1.0);
    m_liftAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_liftAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_hoverProgress = v.toDouble();
        update();
    });

    // Drop: lifted clips spring back to rest (OutBack dips just below rest before settling).
    m_settleAnimation = new QVariantAnimation(this);
    m_settleAnimation->setStartValue(1.0);
    m_settleAnimation->setEndValue(0.0);
    QEasingCurve spring(QEasingCurve::OutBack);
    spring.setOvershoot(2.2);
    m_settleAnimation->setEasingCurve(spring);
    connect(m_settleAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_liftAmount = v.toDouble();
        update();
    });
    connect(m_settleAnimation, &QVariantAnimation::finished, this, [this] {
        m_liftAmount = 0;
        if (!m_gesture.active)
            m_lifted.clear();
        update();
    });
    // Clips shifted by an edit, undo or cancel slide into their new place.
    m_shiftAnimation = new QVariantAnimation(this);
    m_shiftAnimation->setStartValue(0.0);
    m_shiftAnimation->setEndValue(1.0);
    m_shiftAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_shiftAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_shiftProgress = v.toDouble();
        update();
    });
    connect(m_shiftAnimation, &QVariantAnimation::finished, this, [this] {
        m_shiftProgress = 1.0;
        m_shiftFrom.clear();
        update();
    });
    // Razor cuts flash once.
    m_flashAnimation = new QVariantAnimation(this);
    m_flashAnimation->setStartValue(0.0);
    m_flashAnimation->setEndValue(1.0);
    m_flashAnimation->setEasingCurve(QEasingCurve::OutQuad);
    connect(m_flashAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_flashProgress = v.toDouble();
        update();
    });
    connect(m_flashAnimation, &QVariantAnimation::finished, this, [this] {
        m_flashProgress = 1.0;
        m_flashes.clear();
        update();
    });
}

TimelineView::~TimelineView() = default;

void TimelineView::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::changed, this, &TimelineView::onModelChanged);
        connect(m_controller, &ProjectController::mediaChanged, this, [this] {
            // Provenance and offline state can change without replacing a sequence or track (including undo/redo);
            // other media changes (Source Monitor marks, names, labels) draw nothing on the lanes (review P2-46).
            std::unordered_set<std::uint64_t> offline;
            std::unordered_set<std::uint64_t> generated;
            for (const MediaPtr& media : m_controller->snapshot()->media) {
                if (media->locator.offline && !media->isGenerator())
                    offline.insert(media->id.value);
                if (media->generated.has_value())
                    generated.insert(media->id.value);
            }
            if (offline != m_drawnOffline || generated != m_drawnGenerated || currentFacts() != m_drawnFacts) {
                m_pendingAll = true;
                update();
            }
        });
        connect(m_controller, &ProjectController::selectionChanged, this, &TimelineView::refreshSelection);
        connect(m_controller, &ProjectController::selectedEdgeChanged, this, &TimelineView::refreshSelection);
        connect(m_controller, &ProjectController::selectedTransitionChanged, this, &TimelineView::refreshSelection);
        connect(m_controller, &ProjectController::selectedGapChanged, this, &TimelineView::refreshSelection); // 0.2 UX
        connect(m_controller, &ProjectController::timelineHighlightsChanged, this, [this] {
            ++m_selectionRevision; // drawn with the overlay
            update();
        });
        connect(m_controller, &ProjectController::peaksChanged, this, [this] {
            // p02 audio: new peaks from the same source rebuild only the tracks of the media they belong to.
            std::shared_ptr<const PeakSource> source = m_controller ? m_controller->peakSource() : nullptr;
            if (source && source == m_peaks)
                refreshPeaks();
            else
                setPeakSource(std::move(source));
        });
        connect(m_controller, &ProjectController::filmstripsChanged, this,
                [this] { setFilmstripSource(m_controller ? m_controller->filmstripSource() : nullptr); });
        connect(m_controller, &ProjectController::playheadChanged, this, [this] {
            m_playhead = m_controller->finePlayhead(); // 0.2: to the sample with audio time units on
            emit playheadXChanged();
            update();
        });
    }
    setPeakSource(m_controller ? m_controller->peakSource() : nullptr);
    setFilmstripSource(m_controller ? m_controller->filmstripSource() : nullptr);
    m_pendingAll = true;
    refreshFromController();
    emit controllerChanged();
}

void TimelineView::setStyle(const QVariantMap& map) {
    m_styleMap = map;
    TimelineStyle s;
    s.laneColor = colorFrom(map.value(QStringLiteral("laneColor")), s.laneColor);
    s.clipBorder = colorFrom(map.value(QStringLiteral("clipBorder")), s.clipBorder);
    s.videoLabelColor = colorFrom(map.value(QStringLiteral("videoLabelColor")), s.videoLabelColor);
    s.audioLabelColor = colorFrom(map.value(QStringLiteral("audioLabelColor")), s.audioLabelColor);
    s.selectionColor = colorFrom(map.value(QStringLiteral("selectionColor")), s.selectionColor);
    s.glowColor = colorFrom(map.value(QStringLiteral("glowColor")), s.glowColor);
    s.shadowColor = colorFrom(map.value(QStringLiteral("shadowColor")), s.shadowColor);
    s.playheadColor = colorFrom(map.value(QStringLiteral("playheadColor")), s.playheadColor);
    s.marqueeFillColor = colorFrom(map.value(QStringLiteral("marqueeFillColor")), s.marqueeFillColor);
    s.marqueeBorderColor = colorFrom(map.value(QStringLiteral("marqueeBorderColor")), s.marqueeBorderColor);
    if (auto c = colorsFrom(map.value(QStringLiteral("clipColors"))); !c.empty())
        s.clipColors = std::move(c);
    s.audioFillColors = colorsFrom(map.value(QStringLiteral("audioFillColors")));
    s.audioBorderColors = colorsFrom(map.value(QStringLiteral("audioBorderColors")));
    s.waveformColors = colorsFrom(map.value(QStringLiteral("waveformColors")));
    if (const QVariant font = map.value(QStringLiteral("labelFont")); font.metaType() == QMetaType::fromType<QFont>())
        s.labelFont = font.value<QFont>();
    else
        s.labelFont = m_style.labelFont;
    s.laneRadius = realFrom(map, "laneRadius", s.laneRadius);
    s.clipRadius = realFrom(map, "clipRadius", s.clipRadius);
    s.selectionWidth = realFrom(map, "selectionWidth", s.selectionWidth);
    s.selectionGap = realFrom(map, "selectionGap", s.selectionGap);
    s.glowSize = realFrom(map, "glowSize", s.glowSize);
    s.shadowSize = realFrom(map, "shadowSize", s.shadowSize);
    s.liftOffset = realFrom(map, "liftOffset", s.liftOffset);
    s.playheadWidth = realFrom(map, "playheadWidth", s.playheadWidth);
    s.labelPadding = realFrom(map, "labelPadding", s.labelPadding);
    s.microSize = realFrom(map, "microSize", s.microSize);
    s.microTracking = realFrom(map, "microTracking", s.microTracking);
    s.labelTop = realFrom(map, "labelTop", s.labelTop);
    s.waveTop = realFrom(map, "waveTop", s.waveTop);
    s.waveBottom = realFrom(map, "waveBottom", s.waveBottom);
    s.markerWidth = realFrom(map, "markerWidth", s.markerWidth);
    s.markerOpacity = realFrom(map, "markerOpacity", s.markerOpacity);
    s.videoTrackHeight = realFrom(map, "videoTrackHeight", s.videoTrackHeight);
    s.audioTrackHeight = realFrom(map, "audioTrackHeight", s.audioTrackHeight);
    s.trackGap = realFrom(map, "trackGap", s.trackGap);
    s.rulerLabelSpacing = realFrom(map, "rulerLabelSpacing", s.rulerLabelSpacing);
    s.rulerMinorSpacing = realFrom(map, "rulerMinorSpacing", s.rulerMinorSpacing);
    s.maxPixelsPerFrame = realFrom(map, "maxPixelsPerFrame", s.maxPixelsPerFrame);
    s.minLabelWidth = realFrom(map, "minLabelWidth", s.minLabelWidth);
    s.liftDuration = static_cast<int>(realFrom(map, "liftDuration", s.liftDuration));
    s.snapColor = colorFrom(map.value(QStringLiteral("snapColor")), s.snapColor);
    s.ghostColor = colorFrom(map.value(QStringLiteral("ghostColor")), s.ghostColor);
    s.razorColor = colorFrom(map.value(QStringLiteral("razorColor")), s.razorColor);
    s.snapThreshold = realFrom(map, "snapThreshold", s.snapThreshold);
    s.trimHandleWidth = realFrom(map, "trimHandleWidth", s.trimHandleWidth);
    s.liftScale = realFrom(map, "liftScale", s.liftScale);
    s.settleDuration = static_cast<int>(realFrom(map, "settleDuration", s.settleDuration));
    s.rippleDuration = static_cast<int>(realFrom(map, "rippleDuration", s.rippleDuration));
    s.flashDuration = static_cast<int>(realFrom(map, "flashDuration", s.flashDuration));
    s.autoScrollZone = realFrom(map, "autoScrollZone", s.autoScrollZone);
    s.autoScrollSpeed = realFrom(map, "autoScrollSpeed", s.autoScrollSpeed);
    s.markerInkColor = colorFrom(map.value(QStringLiteral("markerInkColor")), s.markerInkColor);
    s.highlightColor = colorFrom(map.value(QStringLiteral("highlightColor")), s.highlightColor);
    s.removalColor = colorFrom(map.value(QStringLiteral("removalColor")), s.removalColor);
    s.volumeColor = colorFrom(map.value(QStringLiteral("volumeColor")), s.volumeColor);
    s.volumeKeyColor = colorFrom(map.value(QStringLiteral("volumeKeyColor")), s.volumeKeyColor);
    s.volumeKeyBorder = colorFrom(map.value(QStringLiteral("volumeKeyBorder")), s.volumeKeyBorder);
    s.clipBodyColors = colorsFrom(map.value(QStringLiteral("clipBodyColors")));
    s.stripHeight = realFrom(map, "stripHeight", s.stripHeight);
    s.clipHeaderHeight = realFrom(map, "clipHeaderHeight", s.clipHeaderHeight);
    s.transitionColor = colorFrom(map.value(QStringLiteral("transitionColor")), s.transitionColor);
    s.transitionInk = colorFrom(map.value(QStringLiteral("transitionInk")), s.transitionInk);
    s.hatchColor = colorFrom(map.value(QStringLiteral("hatchColor")), s.hatchColor);
    s.beamStart = colorFrom(map.value(QStringLiteral("beamStart")), s.beamStart);
    s.beamMid = colorFrom(map.value(QStringLiteral("beamMid")), s.beamMid);
    s.beamEnd = colorFrom(map.value(QStringLiteral("beamEnd")), s.beamEnd);
    s.playheadGlow = realFrom(map, "playheadGlow", s.playheadGlow);
    s.selectedTint = realFrom(map, "selectedTint", s.selectedTint);
    s.notchSize = realFrom(map, "notchSize", s.notchSize);
    s.sliceAngle = realFrom(map, "sliceAngle", s.sliceAngle);
    s.markerColors = colorsFrom(map.value(QStringLiteral("markerColors")));
    s.duplicateColors = colorsFrom(map.value(QStringLiteral("duplicateColors")));
    s.throughEditColor = colorFrom(map.value(QStringLiteral("throughEditColor")), s.throughEditColor);
    s.trimRollColor = colorFrom(map.value(QStringLiteral("trimRollColor")), s.trimRollColor);
    s.trimRippleColor = colorFrom(map.value(QStringLiteral("trimRippleColor")), s.trimRippleColor);
    s.placeholderColor = colorFrom(map.value(QStringLiteral("placeholderColor")), s.placeholderColor);
    s.scrimColor = colorFrom(map.value(QStringLiteral("scrimColor")), s.scrimColor);
    s.minTrackHeight = realFrom(map, "minTrackHeight", s.minTrackHeight);
    s.maxTrackHeight = std::max(s.minTrackHeight, realFrom(map, "maxTrackHeight", s.maxTrackHeight));
    s.trackHeightStep = realFrom(map, "trackHeightStep", s.trackHeightStep);
    s.duplicateBandHeight = realFrom(map, "duplicateBandHeight", s.duplicateBandHeight);
    s.nestedStripe = colorFrom(map.value(QStringLiteral("nestedStripe")), s.nestedStripe);
    s.responsiveBand = colorFrom(map.value(QStringLiteral("responsiveBand")), s.responsiveBand);
    s.responsiveEdge = colorFrom(map.value(QStringLiteral("responsiveEdge")), s.responsiveEdge);
    s.multicamStripe = colorFrom(map.value(QStringLiteral("multicamStripe")), s.multicamStripe);
    s.nestedStripeHeight = realFrom(map, "nestedStripeHeight", s.nestedStripeHeight);
    s.trackVolumeLane = colorFrom(map.value(QStringLiteral("trackVolumeLane")), s.trackVolumeLane);
    s.trackPanLane = colorFrom(map.value(QStringLiteral("trackPanLane")), s.trackPanLane);
    s.fxChangedColor = colorFrom(map.value(QStringLiteral("fxChangedColor")), s.fxChangedColor); // 0.4
    s.fxEffectsColor = colorFrom(map.value(QStringLiteral("fxEffectsColor")), s.fxEffectsColor);
    m_style = std::move(s);
    ++m_styleRevision;
    m_cursorSet = false; // the edit cursors' trim colours may have changed (0.1.3)
    relayoutRows();
    clampScroll();
    emit styleChanged();
    emit viewChanged();
    update();
}

void TimelineView::setPlaying(bool playing) {
    if (playing == m_playing)
        return;
    m_playing = playing;
    emit playingChanged();
    update();
}

void TimelineView::setPeakSource(std::shared_ptr<const PeakSource> source) {
    m_peaks = source ? std::move(source) : std::make_shared<PlaceholderPeakSource>();
    (void)m_peaks->mediaChangedSince(0, m_peakSerial); // what the full rebuild below draws
    m_pendingPeakMedia.clear();
    ++m_styleRevision; // forces a rebuild of every waveform
    update();
}

void TimelineView::refreshPeaks() {
    std::uint64_t now = m_peakSerial;
    const std::optional<std::vector<std::uint64_t>> changed = m_peaks->mediaChangedSince(m_peakSerial, now);
    m_peakSerial = now;
    if (!changed) {
        ++m_styleRevision; // the source cannot say which: every waveform
    } else if (changed->empty()) {
        return;
    } else {
        m_pendingPeakMedia.insert(changed->begin(), changed->end());
    }
    update();
}

// ---- Model -> view ----------------------------------------------------------------------------------

void TimelineView::showSequence(SequencePtr seq) {
    const SequencePtr previous = m_sequence;
    if (previous == seq)
        return;
    m_sequence = std::move(seq);
    if (previous && m_sequence && previous->id == m_sequence->id) {
        const SequenceChange change = diffSequence(*previous, *m_sequence);
        if (change.trackListChanged)
            m_pendingAll = true;
        for (const TrackChange& t : change.tracks) {
            if (t.added || t.removed) {
                m_pendingAll = true;
                continue;
            }
            std::vector<TimeRange>& ranges = m_pendingRanges[t.track.value];
            ranges.insert(ranges.end(), t.ranges.begin(), t.ranges.end());
        }
        if (change.markersChanged)
            ++m_markerRevision;
    } else {
        m_pendingAll = true;
    }
    relayoutRows();
    emit viewChanged();
    update();
}

void TimelineView::startAnimation(QVariantAnimation* animation, int duration) {
    animation->stop();
    if (duration <= 0) { // reduce motion: jump to the end state
        animation->setDuration(1);
        animation->setCurrentTime(1);
        emit animation->finished();
        return;
    }
    animation->setDuration(duration);
    animation->start();
}

void TimelineView::animateChange(const Sequence& from, const Sequence& to) {
    // Only what is around the view moves visibly; clips further away jump.
    const double margin = std::max(1.0, width());
    const Ticks t0 = ticksAtX(-margin);
    const Ticks t1 = ticksAtX(width() + margin);
    std::unordered_map<std::uint64_t, Ticks> shifts;
    std::vector<Flash> flashes;
    std::unordered_set<std::uint64_t> tracks;
    int row = 0;
    const auto compare = [&](const Track* a, const Track* b) {
        if (!a || !b || a == b)
            return;
        std::unordered_map<std::uint64_t, Ticks> before;
        for (auto it = a->clips.iteratorAt(firstClipEndingAfter(a->clips, t0)); it != a->clips.end() && it->start() < t1; ++it)
            before.emplace(it->id.value, it->start());
        const Clip* previous = nullptr;
        for (auto it = b->clips.iteratorAt(firstClipEndingAfter(b->clips, t0)); it != b->clips.end() && it->start() < t1; ++it) {
            const auto old = before.find(it->id.value);
            if (old != before.end() && old->second != it->start()) {
                shifts[it->id.value] = old->second - it->start();
                tracks.insert(b->id.value);
            }
            // A new clip that starts where an existing one now ends: a cut.
            if (old == before.end() && previous && previous->end() == it->start() && before.contains(previous->id.value))
                flashes.push_back({it->start(), row});
            previous = &*it;
        }
    };
    const auto byId = [](const std::vector<TrackPtr>& list, TrackId id) -> const Track* {
        for (const TrackPtr& t : list) {
            if (t->id == id)
                return t.get();
        }
        return nullptr;
    };
    for (auto it = to.videoTracks.rbegin(); it != to.videoTracks.rend(); ++it, ++row)
        compare(byId(from.videoTracks, (*it)->id), it->get());
    for (auto it = to.audioTracks.begin(); it != to.audioTracks.end(); ++it, ++row)
        compare(byId(from.audioTracks, (*it)->id), it->get());

    if (!shifts.empty()) {
        // Continue from where an interrupted slide currently draws the clips.
        for (auto& [id, delta] : shifts) {
            if (const auto running = m_shiftFrom.find(id); running != m_shiftFrom.end())
                delta += Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(running->second.value) * (1.0 - m_shiftProgress)))};
        }
        m_shiftFrom = std::move(shifts);
        m_shiftProgress = 0;
        m_animatedTracks.insert(tracks.begin(), tracks.end());
        startAnimation(m_shiftAnimation, m_style.rippleDuration);
    }
    if (!flashes.empty()) {
        m_flashes = std::move(flashes);
        m_flashProgress = 0;
        startAnimation(m_flashAnimation, m_style.flashDuration);
    }
}

namespace {

/// One clip's part of TimelineLaneFacts: its nested sequence's name and angles, its Enhance Speech badge and its
/// channel badge.
void addClipFacts(TimelineLaneFacts& facts, const ProjectState& snap, const Track& track, const Clip& c) {
    if (c.isNested() && !facts.nested.contains(c.sequence.value)) {
        TimelineNestInfo info;
        if (const Sequence* nested = snap.findSequence(c.sequence)) {
            info.name = QString::fromStdString(nested->name);
            if (nested->multicam) {
                info.multicam = true;
                for (const MulticamAngle& a : nested->multicam->angles)
                    info.angles.push_back(QString::fromStdString(a.name));
            }
        }
        facts.nested.emplace(c.sequence.value, std::move(info));
    }
    if (track.kind == TrackKind::Audio && !c.effects().empty()) {
        const QString badge = enhanceBadge(snap, c);
        if (!badge.isEmpty())
            facts.badges.emplace(c.id.value, badge);
    }
    // M5n (A.9): a clip that does not play its stream as it is (Modify > Audio Channels, fill, swap, mono).
    if (track.kind == TrackKind::Audio) {
        const QString channels = channelBadge(snap, c);
        if (!channels.isEmpty())
            facts.channels.emplace(c.id.value, channels);
    }
    // 0.4: the fx chip (every track kind; untouched clips have no entry).
    if (const ClipFx fx = clipFxOf(c); fx != ClipFx::None)
        facts.fx.emplace(c.id.value, fx);
}

} // namespace

TimelineLaneFacts TimelineView::laneFacts() const {
    TimelineLaneFacts facts;
    if (!m_controller)
        return facts;
    const ProjectSnapshot snap = m_controller->snapshot();
    const SequencePtr seq = m_controller->currentSequence();
    if (!snap || !seq)
        return facts;
    seq->forEachTrack([&](const Track& track) {
        for (const Clip& c : track.clips)
            addClipFacts(facts, *snap, track, c);
    });
    if (m_trackKeyframes != QLatin1String("off")) {
        for (const TrackPtr& t : seq->audioTracks)
            facts.faders.emplace_back(t->volume, t->pan);
    }
    return facts;
}

const TimelineLaneFacts& TimelineView::currentFacts() {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const SequenceId seqId = seq ? seq->id : SequenceId{};
    if (!snap || snap != m_factsSnapshot || seqId != m_factsSequence || m_trackKeyframes != m_factsKeyframes) {
        m_facts = laneFacts();
        m_factsSnapshot = snap;
        m_factsSequence = seqId;
        m_factsKeyframes = m_trackKeyframes;
    }
    return m_facts;
}

void TimelineView::updateFacts(const TimelineChange& change) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const SequenceChange* s = change.sequenceChange();
    // Only a clip-level edit of the current sequence, from the very state the cache describes, is followed: other
    // sequences (a nested one's name or angles), media (Enhance Speech renders, channel maps), track headers with the
    // lanes shown, a reset or a sequence switch recompute everything (currentFacts()).
    bool follow = snap && seq && change.changes && !change.structural && !change.projectWide()
               && m_factsSnapshot && change.before == m_factsSnapshot && seq->id == m_factsSequence
               && m_trackKeyframes == m_factsKeyframes && change.changes->addedSequences.empty()
               && change.changes->removedSequences.empty() && change.changes->sequences.size() == (s ? 1u : 0u);
    if (follow && s && m_trackKeyframes != QLatin1String("off"))
        follow = std::ranges::none_of(s->tracks, [](const TrackChange& t) { return t.headerChanged; });
    if (!follow) {
        m_factsSnapshot.reset();
        return;
    }
    if (s) {
        for (const std::vector<ClipId>* list : {&s->removedClips, &s->modifiedClips, &s->addedClips}) {
            for (ClipId id : *list) {
                m_facts.badges.erase(id.value);
                m_facts.channels.erase(id.value);
                m_facts.fx.erase(id.value);
            }
        }
        // One walk over the clips for the whole change: a ripple modifies thousands, and findClip per id is linear.
        std::vector<std::uint64_t> wanted;
        for (const std::vector<ClipId>* list : {&s->modifiedClips, &s->addedClips})
            for (ClipId id : *list)
                wanted.push_back(id.value);
        std::ranges::sort(wanted);
        if (!wanted.empty()) {
            seq->forEachTrack([&](const Track& track) {
                for (const Clip& c : track.clips)
                    if (std::ranges::binary_search(wanted, c.id.value))
                        addClipFacts(m_facts, *snap, track, c);
            });
        }
    }
    // A nested sequence no clip plays any more keeps its entry (nothing draws it), as long as the cache lives.
    m_factsSnapshot = snap;
}

void TimelineView::onModelChanged() {
    if (!m_controller)
        return;
    if (m_gesture.active) {
        // The model changed under a drag (not by it): the preview no longer applies.
        m_gesture = {};
        m_drag = Drag::Cancelled;
        m_lifted.clear();
        emit gestureChanged();
    }
    // Redactions live beside the sequences: a change to them redraws the filmstrips they hide (review P2-9).
    if (const ProjectSnapshot snap = m_controller->snapshot(); snap && snap->redactions.get() != m_drawnRedactions)
        m_pendingAll = true;
    // M5l: nested names, angles, Enhance Speech badges and the track lanes' faders are not in the tracks' clips (nor,
    // M5n, the channel badges: a media item's channel map changes what its clips play). M5n (P.1): followed per edit.
    updateFacts(m_controller->lastChange());
    if (currentFacts() != m_drawnFacts)
        m_pendingAll = true;
    if (m_drag == Drag::Speed) {
        // An undo or another edit under a speed drag: its key index and speed belong to the old curve (review P2-43).
        // The release's own edits never get here: releaseSpeed() ends the drag first.
        m_drag = Drag::Cancelled;
        m_speedDrag = {};
        m_speedReadout.clear();
        emit gestureChanged();
    }
    const TimelineChange& change = m_controller->lastChange();
    const SequencePtr shown = m_sequence;
    const SequencePtr next = m_controller->currentSequence();
    if (!change.structural && shown && next && shown->id == next->id && shown != next)
        animateChange(*shown, *next);
    if (change.structural) {
        m_pendingAll = true;
        m_pendingRanges.clear();
    } else if (!m_pendingAll) {
        for (const TrackChange& t : change.tracks) {
            if (t.added || t.removed) {
                m_pendingAll = true; // the rows change too
                continue;
            }
            if (t.transitionsChanged)
                ++m_selectionRevision; // transitions are drawn with the overlay
            std::vector<TimeRange>& ranges = m_pendingRanges[t.track.value];
            ranges.insert(ranges.end(), t.ranges.begin(), t.ranges.end());
        }
    }
    if (change.markersChanged)
        ++m_markerRevision;
    refreshFromController(); // emits viewChanged, which also covers markInX/markOutX
}

void TimelineView::refreshFromController() {
    m_sequence = m_controller ? m_controller->currentSequence() : nullptr;
    m_contentDuration = m_controller ? m_controller->contentDuration() : Ticks::fromSeconds(30);
    if (const SequenceId next = m_sequence ? m_sequence->id : SequenceId{}; next != m_shownSequence)
        switchSequenceView(next);
    m_playhead = m_controller ? m_controller->finePlayhead() : Ticks{};
    relayoutRows();
    scheduleRenderBar(); // 0.2: the render bar follows edits, undo and the sequence shown
    refreshSelection();
    clampScroll();
    emit viewChanged();
    emit playheadXChanged();
    update();
}

void TimelineView::refreshSelection() {
    m_selected.clear();
    m_primary = {};
    m_selectedEdge.clear();
    if (m_controller) {
        for (ClipId id : m_controller->selection())
            m_selected.insert(id.value);
        m_primary = m_controller->primaryClip();
        m_selectedEdge = m_controller->selectedEdge();
        m_selectedTransition = m_controller->selectedTransitionId();
    } else {
        m_selectedTransition = {};
    }
    ++m_selectionRevision;
    update();
}

void TimelineView::relayoutRows() {
    std::vector<Row> rows;
    qreal y = 0;
    const auto add = [&](const TrackPtr& track) {
        const bool audio = track->kind == TrackKind::Audio;
        const auto custom = m_trackHeights.find(track->id.value);
        const qreal h = custom != m_trackHeights.end() ? custom->second : defaultTrackHeight(audio);
        rows.push_back({track, y, h, audio});
        y += h + m_style.trackGap;
    };
    if (m_sequence) {
        for (auto it = m_sequence->videoTracks.rbegin(); it != m_sequence->videoTracks.rend(); ++it)
            add(*it);
        for (const TrackPtr& t : m_sequence->audioTracks)
            add(t);
    }
    const qreal contentHeight = rows.empty() ? 0 : y - m_style.trackGap;
    const bool shapeChanged = rows.size() != m_rows.size()
        || !std::equal(rows.begin(), rows.end(), m_rows.begin(), [](const Row& a, const Row& b) {
               return a.track->id == b.track->id && a.y == b.y && a.height == b.height;
           });
    m_rows = std::move(rows);
    if (shapeChanged || contentHeight != m_contentHeight) {
        m_contentHeight = contentHeight;
        ++m_layoutRevision;
        emit layoutChanged();
    }
}

// ---- View parameters --------------------------------------------------------------------------------

Rate TimelineView::frameRate() const {
    return m_sequence ? m_sequence->frameRate : kRate30;
}

Rate TimelineView::displayRate() const {
    if (m_audioTimeUnits && m_sequence && m_sequence->audioRate.isValid())
        return m_sequence->audioRate;
    return frameRate();
}

double TimelineView::fitPixelsPerSecond() const {
    const double seconds = std::max(1.0, m_contentDuration.toSeconds());
    return std::max(1e-3, width() / seconds);
}

double TimelineView::maxPixelsPerSecond() const {
    // 0.2, Show Audio Time Units: down to single samples (two pixels each), as Premiere zooms in on audio.
    const double units = m_audioTimeUnits ? 2.0 * displayRate().toDouble() : m_style.maxPixelsPerFrame * frameRate().toDouble();
    return std::max(fitPixelsPerSecond(), units);
}

void TimelineView::setPixelsPerSecond(double pps) {
    if (!std::isfinite(pps) || pps <= 0)
        return;
    const double clamped = std::clamp(pps, std::min(fitPixelsPerSecond(), maxPixelsPerSecond()), maxPixelsPerSecond());
    if (clamped == m_pps)
        return;
    // Keep the left edge's time in place.
    m_scrollX = m_scrollX / m_pps * clamped;
    m_pps = clamped;
    clampScroll();
    emit viewChanged();
    emit playheadXChanged();
    update();
}

double TimelineView::zoomLevel() const {
    const double fit = fitPixelsPerSecond();
    const double max = maxPixelsPerSecond();
    if (max <= fit * 1.0001)
        return 0.0;
    return std::clamp(std::log(m_pps / fit) / std::log(max / fit), 0.0, 1.0);
}

void TimelineView::setZoomLevel(double level) {
    zoomAt(level, width() / 2);
}

void TimelineView::zoomAt(double level, double anchorX) {
    if (!std::isfinite(level))
        return;
    const double fit = fitPixelsPerSecond();
    const double max = maxPixelsPerSecond();
    const double pps = fit * std::pow(max / fit, std::clamp(level, 0.0, 1.0));
    const double anchorSeconds = (anchorX + m_scrollX) / m_pps;
    if (pps == m_pps)
        return;
    m_pps = pps;
    m_scrollX = anchorSeconds * pps - anchorX;
    clampScroll();
    emit viewChanged();
    emit playheadXChanged();
    update();
}

void TimelineView::setScrollX(double x) {
    if (!std::isfinite(x))
        return;
    const double previous = m_scrollX;
    m_scrollX = x;
    clampScroll();
    if (m_scrollX == previous)
        return;
    emit viewChanged();
    emit playheadXChanged();
    update();
}

void TimelineView::setScrollY(double y) {
    if (!std::isfinite(y))
        return;
    const double previous = m_scrollY;
    m_scrollY = y;
    clampScroll();
    if (m_scrollY == previous)
        return;
    emit viewChanged();
    update();
}

void TimelineView::scrollBy(double dx, double dy) {
    const double sx = m_scrollX;
    const double sy = m_scrollY;
    m_scrollX += dx;
    m_scrollY += dy;
    clampScroll();
    if (sx == m_scrollX && sy == m_scrollY)
        return;
    emit viewChanged();
    emit playheadXChanged();
    update();
}

void TimelineView::clampScroll() {
    m_scrollX = std::clamp(m_scrollX, 0.0, std::max(0.0, contentWidth() - width()));
    m_scrollY = std::clamp(m_scrollY, 0.0, std::max(0.0, m_contentHeight - height()));
}

double TimelineView::contentWidth() const {
    // A drag may run past the end: allow one more view width to scroll into.
    return m_contentDuration.toSeconds() * m_pps + (m_gesture.active ? width() : 0.0);
}

int TimelineView::visibleTrackCount() const {
    int count = 0;
    for (const Row& r : m_rows) {
        if (r.y >= m_scrollY - 0.5 && r.y + r.height <= m_scrollY + height() + 0.5)
            ++count;
    }
    return count;
}

double TimelineView::xOfTicks(Ticks t) const {
    return t.toSeconds() * m_pps - m_scrollX;
}

double TimelineView::playheadX() const {
    return xOfTicks(m_playhead);
}

double TimelineView::markInX() const {
    const auto in = m_controller ? m_controller->markIn() : std::nullopt;
    return in ? xOfTicks(*in) : qQNaN();
}

double TimelineView::markOutX() const {
    const auto out = m_controller ? m_controller->markOut() : std::nullopt;
    return out ? xOfTicks(*out) : qQNaN();
}

Ticks TimelineView::ticksAtX(double x) const {
    const double seconds = std::max(0.0, (x + m_scrollX) / m_pps);
    return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
}

void TimelineView::revealPlayhead() {
    const double x = playheadX();
    if (x < 0)
        setScrollX(m_scrollX + x - width() * 0.1);
    else if (x > width())
        setScrollX(m_scrollX + x - width() * 0.9);
}

void TimelineView::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        clampScroll();
        emit viewChanged();
        update();
    }
}

// ---- Ruler ------------------------------------------------------------------------------------------

TimelineView::RulerStep TimelineView::rulerStep() const {
    const Rate rate = frameRate();
    const std::int64_t base = timecodeBase(rate);
    RulerStep step;
    if (m_audioTimeUnits) {
        // 0.2, Show Audio Time Units: labels every 1, 2, 5, 10 ... samples up to a second, then the second intervals;
        // `frames` counts samples.
        const Rate audio = displayRate();
        const std::int64_t perSecond = std::max<std::int64_t>(1, audio.num / std::max<std::int64_t>(1, audio.den));
        constexpr std::array<std::pair<std::int64_t, int>, 14> kSampleIntervals{{
            {1, 1}, {2, 2}, {5, 5}, {10, 5}, {20, 4}, {50, 5}, {100, 4}, {200, 4}, {500, 5}, {1000, 4}, {2000, 4},
            {5000, 5}, {10000, 4}, {20000, 4}}};
        for (const auto& [samples, subdivisions] : kSampleIntervals) {
            step.frames = samples;
            step.subdivisions = subdivisions;
            step.px = static_cast<double>(samples) / static_cast<double>(perSecond) * m_pps;
            if (step.px >= m_style.rulerLabelSpacing)
                return step;
        }
        for (const auto& [seconds, subdivisions] : kRulerIntervals) {
            step.frames = std::int64_t{seconds} * perSecond;
            step.subdivisions = subdivisions;
            step.px = std::int64_t{seconds} * m_pps;
            if (step.px >= m_style.rulerLabelSpacing)
                break;
        }
        return step;
    }
    for (const auto& [seconds, subdivisions] : kRulerIntervals) {
        step.frames = std::int64_t{seconds} * base;
        step.subdivisions = subdivisions;
        step.px = framesToTicks(step.frames, rate).toSeconds() * m_pps;
        if (step.px >= m_style.rulerLabelSpacing)
            break;
    }
    return step;
}

double TimelineView::rulerStepPx() const {
    return rulerStep().px;
}

int TimelineView::rulerFirstIndex() const {
    const double px = rulerStep().px;
    return px > 0 ? static_cast<int>(std::floor(m_scrollX / px)) : 0;
}

double TimelineView::rulerFirstX() const {
    return rulerFirstIndex() * rulerStep().px - m_scrollX;
}

int TimelineView::rulerStepFrames() const {
    return static_cast<int>(rulerStep().frames);
}

int TimelineView::rulerSubdivisions() const {
    const RulerStep step = rulerStep();
    return step.px / step.subdivisions >= m_style.rulerMinorSpacing ? step.subdivisions : 1;
}

QString TimelineView::rulerLabel(int tickIndex, int stepFrames) const {
    if (m_audioTimeUnits && m_sequence && m_controller) // samples (0.2)
        return m_controller->formatAudioTime(framesToTicks(std::int64_t{tickIndex} * stepFrames, displayRate()));
    const bool dropFrame = m_sequence && m_sequence->dropFrame;
    return QString::fromStdString(framesToTimecode(std::int64_t{tickIndex} * stepFrames, frameRate(), dropFrame));
}

QVariantList TimelineView::markerPositions() const {
    QVariantList list;
    if (m_sequence) {
        for (const Marker& m : m_sequence->markers)
            list << xOfTicks(m.start);
    }
    return list;
}

// ---- Hit-testing ------------------------------------------------------------------------------------

int TimelineView::rowAt(double y) const {
    const double contentY = y + m_scrollY;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (contentY >= m_rows[i].y && contentY < m_rows[i].y + m_rows[i].height)
            return static_cast<int>(i);
    }
    return -1;
}

ClipId TimelineView::clipIdAt(double x, double y) const {
    const int row = rowAt(y);
    if (row < 0)
        return {};
    const Track& track = *m_rows[static_cast<std::size_t>(row)].track;
    const auto index = track.clipIndexAt(ticksAtX(x));
    return index ? track.clips[*index].id : ClipId{};
}

QString TimelineView::clipAt(double x, double y) const {
    return clipKey(clipIdAt(x, y));
}

QString TimelineView::timeAt(double x) const {
    if (m_audioTimeUnits && m_controller) // 0.2
        return m_controller->formatAudioTime(snapToFrame(ticksAtX(x), displayRate()));
    const bool dropFrame = m_sequence && m_sequence->dropFrame;
    return QString::fromStdString(ticksToTimecode(ticksAtX(x), frameRate(), dropFrame));
}

QString TimelineView::trackAt(double y) const {
    const int row = rowAt(y);
    return row < 0 ? QString() : trackKey(m_rows[static_cast<std::size_t>(row)].track->id);
}

void TimelineView::seekToX(double x, bool forceSnap) {
    if (!m_controller)
        return;
    const Ticks raw = ticksAtX(x);
    // 0.4, Premiere's "Snap Playhead in Timeline When Snap Is Enabled" (Shift snaps with Snap off): the nearest clip
    // edge, marker, In, Out or beat within the snap threshold. The playhead itself is not a target (it is what moves).
    std::optional<Ticks> snapped;
    if (forceSnap || (m_snapping && m_snapPlayhead)) {
        const std::vector<Ticks> targets = localSnapTargets(raw, false);
        snapped = edit::nearestSnapTarget(targets, raw, snapTolerance());
    }
    const bool lineChanged = snapped != m_playheadSnapAt;
    m_playheadSnapAt = snapped;
    m_controller->setPlayhead(snapped.value_or(raw));
    if (lineChanged) {
        emit gestureChanged(); // snapX
        update();              // the snap line (guides)
    }
}

void TimelineView::endScrub() {
    if (!m_playheadSnapAt)
        return;
    m_playheadSnapAt.reset();
    emit gestureChanged();
    update();
}

void TimelineView::followPlayhead(const QString& mode, bool reverse) {
    // 0.4, Premiere's Timeline Playback Auto-Scrolling (Main calls this on every playhead change during playback, with
    // the direction it plays in). Reverse play (J) mirrors each rule, so it pages and scrolls as smoothly as forward.
    if (mode == QLatin1String("none") || width() <= 0)
        return;
    const double x = playheadX();
    if (mode == QLatin1String("smooth")) {
        // The playhead travels to the middle, then stays there while the clips scroll under it (at either end of the
        // sequence the scroll stops and it moves on); a jump the other way (a loop) re-centres it.
        const double target = width() * 0.5;
        if (reverse ? (x < target || x > width()) : (x > target || x < 0))
            setScrollX(m_scrollX + x - target);
        return;
    }
    // "page" (the default): off the edge it plays towards, the view turns a page and the playhead lands on the other
    // edge (left going forward, right in reverse); off the edge behind it (a loop back to In, or to Out in reverse) it
    // sits 10 % in from that edge.
    if (!reverse) {
        if (x > width())
            setScrollX(m_scrollX + x);
        else if (x < 0)
            setScrollX(m_scrollX + x - width() * 0.1);
    } else {
        if (x < 0)
            setScrollX(m_scrollX + x - width());
        else if (x > width())
            setScrollX(m_scrollX + x - width() * 0.9);
    }
}

void TimelineView::toggleZoomToSequence() {
    // 0.4: \ fits the sequence; pressed again while the view is still exactly at that fit, it goes back (Premiere).
    if (m_zoomBeforeFit && m_zoomBeforeFit->sequence == m_shownSequence && qFuzzyCompare(m_pps, m_zoomBeforeFit->fitPps)
        && std::abs(m_scrollX - m_zoomBeforeFit->fitScrollX) < 0.5) {
        const ZoomMemory back = *m_zoomBeforeFit;
        m_zoomBeforeFit.reset();
        setPixelsPerSecond(back.pps);
        setScrollX(back.scrollX);
        return;
    }
    ZoomMemory memory;
    memory.sequence = m_shownSequence;
    memory.pps = m_pps;
    memory.scrollX = m_scrollX;
    zoomAt(0, 0);
    memory.fitPps = m_pps;
    memory.fitScrollX = m_scrollX;
    m_zoomBeforeFit = memory;
}

QVariantMap TimelineView::clipHoverInfo(const QString& clipId) const {
    const auto id = parseClipKey(clipId);
    if (!id || !m_sequence || !m_controller)
        return {};
    const auto loc = m_sequence->findClip(*id);
    const auto position = loc ? m_sequence->trackPosition(loc->track->id) : std::nullopt;
    if (!loc || !position)
        return {};
    const Clip& c = loc->clip();
    const bool audio = position->kind == TrackKind::Audio;
    const bool samples = audio && m_audioTimeUnits && m_sequence->audioRate.isValid();
    const auto format = [&](Ticks t) { return samples ? m_controller->formatAudioTime(t) : m_controller->formatTimecode(t); };
    const ProjectSnapshot snap = m_controller->snapshot();

    QVariantMap info;
    const QString name = QString::fromStdString(c.name());
    const QString track = QStringLiteral("%1%2").arg(audio ? QLatin1Char('A') : QLatin1Char('V')).arg(position->index + 1);
    const QString speed = clipSpeedText(c);
    info.insert(QStringLiteral("name"), name);
    info.insert(QStringLiteral("track"), track);
    info.insert(QStringLiteral("start"), format(c.start()));
    info.insert(QStringLiteral("end"), format(c.end()));
    info.insert(QStringLiteral("duration"), format(c.duration()));
    info.insert(QStringLiteral("speed"), speed);
    info.insert(QStringLiteral("reversed"), c.reversed);
    info.insert(QStringLiteral("remapped"), c.isRemapped());
    info.insert(QStringLiteral("freeze"), c.isFreeze());

    // The source: a media file, a nested sequence, a multicam source's angle, or nothing this build knows.
    QString source;
    QString sourceKind = QStringLiteral("none");
    QString sourceLine;
    bool offline = false;
    if (c.isNested()) {
        if (const Sequence* nested = snap ? snap->findSequence(c.sequence) : nullptr) {
            source = QString::fromStdString(nested->name);
            if (nested->multicam) {
                sourceKind = QStringLiteral("multicam");
                const std::size_t angle = c.angles.first();
                if (angle < nested->multicam->angles.size())
                    source += QStringLiteral(" · ") + QString::fromStdString(nested->multicam->angles[angle].name);
                sourceLine = tr("Multi-Camera %1").arg(source);
            } else {
                sourceKind = QStringLiteral("sequence");
                sourceLine = tr("Sequence %1").arg(source);
            }
        }
    } else if (const MediaItem* media = snap && c.media.isValid() ? snap->findMedia(c.media) : nullptr) {
        sourceKind = QStringLiteral("media");
        const QString path = QString::fromStdString(media->locator.path);
        source = path.isEmpty() ? QString::fromStdString(media->name) : QFileInfo(path).fileName();
        offline = media->locator.offline && !media->isGenerator();
    }
    QString sourceIn;
    QString sourceOut;
    if (sourceKind != QLatin1String("none") && !c.isRemapped()) {
        const Ticks unit = samples ? framesToTicks(1, m_sequence->audioRate) : framesToTicks(1, frameRate());
        sourceIn = format(c.sourceIn);
        sourceOut = format(std::max(c.sourceIn, c.sourceOut() - unit)); // the last source frame, as the Slip readout
    }
    if (sourceKind == QLatin1String("media")) {
        sourceLine = c.isRemapped() ? tr("Source %1 · time remapped").arg(source)
                                    : tr("Source %1 · In %2 · Out %3").arg(source, sourceIn, sourceOut);
    }
    const int effects = static_cast<int>(c.effects().size());
    info.insert(QStringLiteral("source"), source);
    info.insert(QStringLiteral("sourceKind"), sourceKind);
    info.insert(QStringLiteral("sourceIn"), sourceIn);
    info.insert(QStringLiteral("sourceOut"), sourceOut);
    info.insert(QStringLiteral("effects"), effects);
    info.insert(QStringLiteral("disabled"), !c.enabled);
    info.insert(QStringLiteral("offline"), offline);

    // The tooltip: lines and parts that say nothing (100 % speed, no effects, enabled, online) are left out.
    QStringList lines;
    lines << (name.isEmpty() ? track : name);
    lines << tr("Start %1 · End %2").arg(info.value(QStringLiteral("start")).toString(),
                                         info.value(QStringLiteral("end")).toString());
    QString durationLine = tr("Duration %1").arg(info.value(QStringLiteral("duration")).toString());
    if (!speed.isEmpty())
        durationLine += tr(" · Speed %1").arg(speed);
    lines << durationLine;
    if (!sourceLine.isEmpty())
        lines << sourceLine;
    QStringList state;
    if (effects == 1)
        state << tr("1 effect");
    else if (effects > 1)
        state << tr("%1 effects").arg(effects);
    if (!c.enabled)
        state << tr("Disabled");
    if (offline)
        state << tr("Offline");
    if (!state.isEmpty())
        lines << state.join(QStringLiteral(" · "));
    info.insert(QStringLiteral("text"), lines.join(QLatin1Char('\n')));
    return info;
}

std::vector<ClipId> TimelineView::clipsInRect(const QRectF& rect) const {
    std::vector<ClipId> ids;
    if (!m_sequence)
        return ids;
    const QRectF r = rect.normalized();
    const Ticks t0 = ticksAtX(r.left());
    const Ticks t1 = ticksAtX(r.right());
    for (const Row& row : m_rows) {
        const double top = row.y - m_scrollY;
        if (top + row.height < r.top() || top > r.bottom())
            continue;
        const auto& clips = row.track->clips;
        for (auto it = clips.iteratorAt(firstClipEndingAfter(clips, t0)); it != clips.end() && it->start() <= t1; ++it)
            ids.push_back(it->id);
    }
    return m_linkedSelection ? edit::withGroupedClips(*m_sequence, ids) : ids; // links and (0.2) groups
}

void TimelineView::selectAt(double x, double y, int modifiers) {
    if (!m_controller)
        return;
    const auto mods = Qt::KeyboardModifiers::fromInt(modifiers);
    const bool toggle = mods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier);
    const bool ignoreLinks = !followLinks(mods);
    const ClipId hit = clipIdAt(x, y);
    if (!hit) {
        if (!toggle)
            m_controller->clearSelection();
        return;
    }
    m_controller->selectClipById(hit, toggle ? ProjectController::Toggle : ProjectController::Replace, !ignoreLinks);
}

void TimelineView::selectInRect(const QRectF& rect, int modifiers) {
    if (!m_controller)
        return;
    const auto mods = Qt::KeyboardModifiers::fromInt(modifiers);
    std::vector<ClipId> next;
    if (mods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier))
        next = m_controller->selection();
    for (ClipId id : clipsInRect(rect)) {
        if (std::find(next.begin(), next.end(), id) == next.end())
            next.push_back(id);
    }
    m_controller->setSelection(std::move(next));
}

// Mouse, keyboard and gestures: TimelineViewInput.cpp.

void TimelineView::setHovered(ClipId clip) {
    if (clip == m_hovered)
        return;
    m_hovered = clip;
    m_liftAnimation->stop();
    if (clip && m_style.liftDuration > 0) {
        m_hoverProgress = 0;
        m_liftAnimation->setDuration(m_style.liftDuration);
        m_liftAnimation->start();
    } else {
        m_hoverProgress = clip ? 1.0 : 0.0;
    }
    emit hoveredClipChanged();
    update();
}

// ---- Scene graph ------------------------------------------------------------------------------------

namespace {

struct BuildContext {
    const TimelineStyle& style;
    QQuickWindow* window;
    QSGNode* labelGroup;
    const QFontMetricsF& metrics;
    double pps;
    double originX;
    double builtWidth;  // built range width in px
    double viewLeft;    // view's left edge in built coordinates
    double viewRight;
    double scrollY;
    double viewHeight;
    ClipId hovered;
    double lift;        // current hover lift in px
    const PeakSource& peaks;
    std::vector<float>& peakScratch;
    MeshBuilder& mesh;
    NameWidths& names;
    LabelPool& pool;
    // Text layout is the expensive part of a frame: at most this many labels are (re)laid out per frame; the
    // rest follow in the next frames (labelsPending schedules one).
    int labelBudget = 8;
    bool labelsPending = false;
    // M5o: the responsive bands' tags, translated on the GUI thread.
    QString introText;
    QString outroText;
    const std::unordered_set<std::uint64_t>* generated = nullptr;
    // M5l: nested / multi-camera clips and the Enhance Speech badges; the audio tracks' fader or pan lane.
    const TimelineLaneFacts* facts = nullptr;
    enum class TrackLane { Off, Volume, Pan } trackLane = TrackLane::Off;
    int styleRevision = 0;
    // Motion: lifted (dragged / settling) clips grow in height around their middle; sliding clips are offset in x.
    const std::unordered_set<std::uint64_t>* lifted = nullptr;
    double liftScale = 1.0;
    const std::unordered_map<std::uint64_t, double>* shiftPx = nullptr;
    // The volume rubber band on audio clips (M5g).
    bool showVolume = false;
    bool showOpacity = false; // 0.2: the pen shows video clips' Opacity band (TimelineViewVolume.cpp)
    bool showSpeed = false;
    bool showClipSpeed = true; // 0.4: "[50%]" after the name
    bool showFxBadges = true;  // 0.4: the fx chip
    // Media whose files are offline: their clips are hatched (design language v2).
    const std::unordered_set<std::uint64_t>* offline = nullptr;
    // M5i
    MeshBuilder* top = nullptr;              // drawn over the thumbnails
    QSGNode* content = nullptr;              // the film nodes' parent
    const FilmstripSource* filmstrips = nullptr;
    enum class Film { Off, Head, HeadTail, Continuous } film = Film::Off;
    /// The texture of a media atlas (uploaded on first use).
    std::function<QSGTexture*(MediaId, const std::shared_ptr<const FilmstripAtlas>&)> texture;
    const std::unordered_map<std::uint64_t, std::vector<DuplicateRange>>* duplicates = nullptr;
    const std::unordered_map<std::uint64_t, const std::vector<Marker>*>* clipMarkers = nullptr;
    bool throughEdits = false;
    bool rectified = false;
    bool logarithmic = false;
    bool volumeWaveforms = false; // p02 audio: scaled by the clip's Audio Gain and Volume
    /// Enabled source redactions by media id: thumbnails of the frames they cover are not shown (review P2-9; the
    /// thumbnails are decoded from the raw file).
    const std::unordered_map<std::uint64_t, std::vector<const Redaction*>>* redactions = nullptr; ///< by redaction root
    /// Media whose redaction root is another item (subclips, stems): media -> root.
    const std::unordered_map<std::uint64_t, std::uint64_t>* redactionRoots = nullptr;
    int filmTiles = 0;
    int filmPlaceholders = 0;
    int filmRedacted = 0;
    int filmNodes = 0;
    int buildingWaveforms = 0; // p02 audio: audio clips shaded where their peaks are still building
    float loudestWaveform = 0;
};

/// A thumbnail quad: where it goes (built coordinates) and which part of the atlas it shows (normalised).
struct FilmQuad {
    QRectF dst;
    QRectF src;
};

/// Lays out one video clip's thumbnails (Premiere's Video Head / Head and Tail / Continuous; Continuous adapts to
/// the clip's width: the head frame when narrow, then head and tail, then a full strip). Ready tiles are appended to
/// `quads`; while the atlas loads, placeholder tiles go into the clip's mesh.
void layoutFilmstrip(BuildContext& ctx, const Clip& clip, const QRectF& body, double shiftX,
                     const FilmstripSource::Lookup& look, std::vector<FilmQuad>& quads) {
    const FilmstripAtlas* atlas = look.atlas.get();
    const double aspect = atlas && atlas->tileHeight > 0 ? static_cast<double>(atlas->tileWidth) / atlas->tileHeight
                                                         : 16.0 / 9.0;
    const double tileW = std::max(4.0, body.height() * aspect);
    const double width = body.width();
    const auto sourceAtX = [&](double x) {
        const double seconds = (x - shiftX + ctx.originX) / ctx.pps;
        Ticks t{static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond))};
        t = std::clamp(t, clip.start(), clip.end() - Ticks{1});
        return clip.sourceTimeAt(t);
    };
    struct Tile {
        double x0, x1;
        Ticks source;
        bool alignRight;
    };
    std::vector<Tile> tiles;
    const auto head = [&] { tiles.push_back({body.left(), std::min(body.right(), body.left() + tileW), clip.sourceTimeAt(clip.start()), false}); };
    const auto tail = [&] {
        tiles.push_back({std::max(body.left(), body.right() - tileW), body.right(), clip.sourceTimeAt(clip.end() - Ticks{1}), true});
    };
    using Film = BuildContext::Film;
    if (ctx.film == Film::Head || (ctx.film == Film::Continuous && width < 2 * tileW)) {
        head();
    } else if (ctx.film == Film::HeadTail || (ctx.film == Film::Continuous && width < 3 * tileW)) {
        head();
        if (width >= 2 * tileW)
            tail();
    } else {
        // A full strip; only the tiles inside the built range.
        double x = body.left();
        if (x < -tileW)
            x += std::floor((-tileW - x) / tileW) * tileW;
        for (; x < body.right() && x < ctx.builtWidth + tileW; x += tileW)
            tiles.push_back({x, std::min(body.right(), x + tileW), sourceAtX(x + tileW / 2), false});
    }
    const std::vector<const Redaction*>* redactions = nullptr;
    if (ctx.redactions) {
        std::uint64_t root = clip.media.value;
        if (ctx.redactionRoots) {
            if (const auto r = ctx.redactionRoots->find(root); r != ctx.redactionRoots->end())
                root = r->second;
        }
        if (const auto it = ctx.redactions->find(root); it != ctx.redactions->end())
            redactions = &it->second;
    }
    const auto redacted = [&](Ticks source) {
        return redactions && std::any_of(redactions->begin(), redactions->end(), [&](const Redaction* r) {
                   return r->appliesTo(clip.id) && r->source.contains(source);
               });
    };
    for (const Tile& t : tiles) {
        const double x0 = std::max(t.x0, 0.0);
        const double x1 = std::min(t.x1, ctx.builtWidth);
        if (x1 - x0 < 1)
            continue;
        // A tile shows the atlas frame nearest its source time: hide it when a redaction covers either.
        const bool hidden = redacted(t.source) ||
                            (atlas && atlas->count() > 0 &&
                             redacted(atlas->times[static_cast<std::size_t>(atlas->tileAt(t.source))]));
        if (hidden) {
            ctx.mesh.addRect(QRectF(x0 + 0.5, body.top() + 0.5, std::max(1.0, x1 - x0 - 1), body.height() - 1),
                             ctx.style.placeholderColor);
            ++ctx.filmPlaceholders;
            ++ctx.filmRedacted;
            continue;
        }
        if (!atlas) {
            ctx.mesh.addRect(QRectF(x0 + 0.5, body.top() + 0.5, std::max(1.0, x1 - x0 - 1), body.height() - 1),
                             ctx.style.placeholderColor);
            ++ctx.filmPlaceholders;
            continue;
        }
        // The tile's full extent, then the part of it that is drawn.
        const double full0 = t.alignRight ? t.x1 - tileW : t.x0;
        const QRect r = atlas->tileRect(atlas->tileAt(t.source));
        const double aw = atlas->atlas.width();
        const double ah = atlas->atlas.height();
        const double u0 = (r.x() + (x0 - full0) / tileW * r.width()) / aw;
        const double u1 = (r.x() + (x1 - full0) / tileW * r.width()) / aw;
        quads.push_back({QRectF(x0, body.top(), x1 - x0, body.height()), QRectF(u0, r.y() / ah, u1 - u0, r.height() / ah)});
        ++ctx.filmTiles;
    }
}

/// Puts each media's thumbnail quads into one textured node (reusing the bucket's nodes, in order).
void commitFilm(BuildContext& ctx, TrackBucket& b,
                const std::vector<std::pair<std::uint64_t, std::vector<FilmQuad>>>& groups,
                const std::unordered_map<std::uint64_t, std::shared_ptr<const FilmstripAtlas>>& atlases) {
    std::size_t used = 0;
    for (const auto& [media, quads] : groups) {
        if (quads.empty())
            continue;
        const auto atlas = atlases.find(media);
        QSGTexture* texture = atlas != atlases.end() ? ctx.texture(MediaId{media}, atlas->second) : nullptr;
        if (!texture)
            continue;
        QSGGeometryNode* node = nullptr;
        if (used < b.film.size()) {
            node = b.film[used];
        } else {
            node = new QSGGeometryNode;
            auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            node->setGeometry(geometry);
            node->setFlag(QSGNode::OwnsGeometry);
            auto* material = new QSGOpaqueTextureMaterial;
            material->setFiltering(QSGTexture::Linear);
            node->setMaterial(material);
            node->setFlag(QSGNode::OwnsMaterial);
            ctx.content->insertChildNodeBefore(node, b.top);
            b.film.push_back(node);
        }
        ++used;
        QSGGeometry* g = node->geometry();
        g->allocate(static_cast<int>(quads.size() * 6));
        QSGGeometry::TexturedPoint2D* v = g->vertexDataAsTexturedPoint2D();
        for (const FilmQuad& q : quads) {
            const auto set = [&](double x, double y, double u, double w) {
                v->set(static_cast<float>(x), static_cast<float>(y), static_cast<float>(u), static_cast<float>(w));
                ++v;
            };
            set(q.dst.left(), q.dst.top(), q.src.left(), q.src.top());
            set(q.dst.right(), q.dst.top(), q.src.right(), q.src.top());
            set(q.dst.right(), q.dst.bottom(), q.src.right(), q.src.bottom());
            set(q.dst.left(), q.dst.top(), q.src.left(), q.src.top());
            set(q.dst.right(), q.dst.bottom(), q.src.right(), q.src.bottom());
            set(q.dst.left(), q.dst.bottom(), q.src.left(), q.src.bottom());
        }
        auto* material = static_cast<QSGOpaqueTextureMaterial*>(node->material());
        if (material->texture() != texture)
            material->setTexture(texture);
        node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    }
    // Nodes this build does not need.
    while (b.film.size() > used) {
        QSGGeometryNode* node = b.film.back();
        b.film.pop_back();
        if (QSGNode* parent = node->parent())
            parent->removeChildNode(node);
        delete node;
    }
    ctx.filmNodes += static_cast<int>(used);
}

/// The UV beam's colour at t in [0, 1] (start → mid → end).
QColor beamAt(const TimelineStyle& st, double t) {
    const auto mix = [](const QColor& a, const QColor& b, double u) {
        return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * u),
                                static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * u),
                                static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * u),
                                static_cast<float>(a.alphaF() + (b.alphaF() - a.alphaF()) * u));
    };
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.55 ? mix(st.beamStart, st.beamMid, t / 0.55) : mix(st.beamMid, st.beamEnd, (t - 0.55) / 0.45);
}

/// A line from `bottom` to `top` of `width`, coloured along its length by the UV beam (bottom = start), in `segments`
/// steps, at `alpha`.
void addBeamLine(MeshBuilder& mesh, const TimelineStyle& st, QPointF bottom, QPointF top, double width, int segments,
                 double alpha) {
    const QPointF d = top - bottom;
    const double len = std::hypot(d.x(), d.y());
    if (len <= 0)
        return;
    const QPointF n(-d.y() / len * width / 2, d.x() / len * width / 2);
    for (int i = 0; i < segments; ++i) {
        const QPointF a = bottom + d * (static_cast<double>(i) / segments);
        const QPointF b = bottom + d * (static_cast<double>(i + 1) / segments);
        const QColor c = withAlpha(beamAt(st, (i + 0.5) / segments), alpha);
        mesh.addTriangle(a - n, a + n, b + n, c);
        mesh.addTriangle(a - n, b + n, b - n, c);
    }
}

/// Diagonal hatching over a disabled or offline clip (30°: a fixed angle, not the v2 slice's, which v3 sets to 0).
void addHatch(MeshBuilder& mesh, const TimelineStyle& st, const QRectF& rect) {
    constexpr double kHatchDegrees = 30.0;
    const double lean = rect.height() * std::tan(kHatchDegrees * std::numbers::pi / 180.0);
    constexpr double kStep = 7.0;
    constexpr double kWidth = 1.2;
    for (double xb = rect.left() - lean; xb < rect.right(); xb += kStep) {
        // The line from (xb, bottom) to (xb + lean, top), cut to the clip's width.
        double t0 = 0.0;
        double t1 = 1.0;
        if (lean > 0) {
            t0 = std::max(0.0, (rect.left() - xb) / lean);
            t1 = std::min(1.0, (rect.right() - xb) / lean);
        }
        if (t1 <= t0)
            continue;
        const QPointF a(xb + lean * t0, rect.bottom() - rect.height() * t0);
        const QPointF b(xb + lean * t1, rect.bottom() - rect.height() * t1);
        const QPointF n(kWidth / 2, 0);
        mesh.addTriangle(a - n, a + n, b + n, st.hatchColor);
        mesh.addTriangle(a - n, b + n, b - n, st.hatchColor);
    }
}

/// The volume rubber band of an audio clip (TimelineViewVolume.cpp has its editing): the line, sampled every 3 px so
/// any interpolation shows, and the keys (diamond: linear, circle: Bezier, square: hold). 0.2, `opacity`: a video clip's
/// Opacity band instead (the pen tool), drawn over its thumbnails (addVolumeLine below).
/// p02 audio ("Show Audio Waveforms Using Clip Volume"): each peak column times the clip's Audio Gain and its Volume at
/// the column's middle (level and keyframes), clamped to full scale.
void scaleByClipVolume(const Clip& clip, double x0, double step, const std::function<Ticks(double)>& tickAt,
                       std::span<float> peaks) {
    double gainDb = 0.0;
    if (const EffectInstance* gain = audio::edit::normalizationGain(clip.effects()); gain && gain->enabled) {
        if (const ParamValue* value = gain->find("gain"))
            gainDb = value->valueAt(Ticks{});
    }
    const ParamValue& volume = clip.properties()[ParamId::VolumeDb];
    const bool animated = volume.curve() != nullptr;
    const double constantDb = animated ? 0.0 : volume.constant();
    for (std::size_t k = 0; k < peaks.size(); ++k) {
        double db = gainDb + constantDb;
        if (animated) {
            const Ticks t = std::clamp(tickAt(x0 + (static_cast<double>(k) + 0.5) * step), clip.start(), clip.end() - Ticks{1});
            db += volume.valueAt(clip.curveTimeAt(t));
        }
        peaks[k] = static_cast<float>(std::min(1.0, static_cast<double>(peaks[k]) * std::pow(10.0, db / 20.0)));
    }
}

void addVolumeLine(BuildContext& ctx, const Clip& clip, const QRectF& rect, double x0, double x1, bool opacity = false) {
    if (x1 - x0 < 2)
        return;
    const TimelineStyle& st = ctx.style;
    const QRectF area = TimelineView::volumeArea(rect);
    const auto yOf = [&](double v) { return area.top() + TimelineView::bandFraction(!opacity, v) * area.height(); };
    const auto tickAt = [&](double x) {
        return Ticks{static_cast<std::int64_t>((x + ctx.originX) / ctx.pps * static_cast<double>(kTicksPerSecond))};
    };
    MeshBuilder& mesh = opacity && ctx.top ? *ctx.top : ctx.mesh;
    const ParamValue& volume = clip.properties()[opacity ? ParamId::Opacity : ParamId::VolumeDb];
    // A light line on a dark underlay: it reads on every clip colour and waveform, in both themes.
    constexpr double kWidth = 1.5;
    constexpr double kUnderlay = 3.5;
    QColor underlay = st.volumeKeyBorder;
    underlay.setAlphaF(0.55f);
    const Curve* curve = volume.curve();
    if (!curve) {
        const double y = yOf(volume.constant());
        mesh.addRect(QRectF(x0, y - kUnderlay / 2, x1 - x0, kUnderlay), underlay);
        mesh.addRect(QRectF(x0, y - kWidth / 2, x1 - x0, kWidth), st.volumeColor);
        return;
    }
    const int n = static_cast<int>(std::clamp(std::ceil((x1 - x0) / 3.0), 1.0, 2000.0));
    std::vector<QPointF> points;
    points.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
        const double x = x0 + (x1 - x0) * i / n;
        const Ticks t = std::clamp(tickAt(x), clip.start(), clip.end() - Ticks{1});
        points.emplace_back(x, yOf(volume.valueAt(clip.curveTimeAt(t))));
    }
    mesh.addPolyline(points, kUnderlay, underlay);
    mesh.addPolyline(points, kWidth, st.volumeColor);
    constexpr double kKey = 4.0;
    for (const Keyframe& k : curve->keys) {
        const auto t = clip.timelineTimeOfCurveTime(k.time);
        if (!t)
            continue;
        const double x = t->toSeconds() * ctx.pps - ctx.originX;
        if (x < x0 - kKey || x > x1 + kKey)
            continue;
        const QPointF c(x, yOf(k.value));
        if (k.interpolation == Interpolation::Bezier) {
            mesh.addRoundedRect(QRectF(c.x() - kKey, c.y() - kKey, kKey * 2, kKey * 2), kKey, st.volumeKeyColor,
                                    st.volumeKeyBorder, 1.0);
        } else if (k.interpolation == Interpolation::Hold) {
            mesh.addRect(QRectF(c.x() - kKey, c.y() - kKey, kKey * 2, kKey * 2), st.volumeKeyBorder);
            mesh.addRect(QRectF(c.x() - kKey + 1, c.y() - kKey + 1, kKey * 2 - 2, kKey * 2 - 2), st.volumeKeyColor);
        } else {
            const double o = kKey + 1;
            mesh.addTriangle({c.x(), c.y() - o}, {c.x() + o, c.y()}, {c.x(), c.y() + o}, st.volumeKeyBorder);
            mesh.addTriangle({c.x(), c.y() - o}, {c.x(), c.y() + o}, {c.x() - o, c.y()}, st.volumeKeyBorder);
            mesh.addTriangle({c.x(), c.y() - kKey}, {c.x() + kKey, c.y()}, {c.x(), c.y() + kKey}, st.volumeKeyColor);
            mesh.addTriangle({c.x(), c.y() - kKey}, {c.x(), c.y() + kKey}, {c.x() - kKey, c.y()}, st.volumeKeyColor);
        }
    }
}

void addSpeedLine(BuildContext& ctx, const Clip& clip, const QRectF& rect, double x0, double x1) {
    if (x1 - x0 < 2) return;
    MeshBuilder& mesh = ctx.top ? *ctx.top : ctx.mesh;
    const QRectF area = TimelineView::volumeArea(rect);
    const auto yOf = [&](double percent) { return area.top() + TimeRemapBridge::speedFraction(percent) * area.height(); };
    const int count = static_cast<int>(std::clamp(std::ceil((x1 - x0) / 3), 1.0, 2000.0));
    std::vector<QPointF> points;
    for (int i = 0; i <= count; ++i) {
        const double x = x0 + (x1 - x0) * i / count;
        const Ticks at{static_cast<std::int64_t>((x + ctx.originX) / ctx.pps * kTicksPerSecond)};
        const Ticks t = std::clamp(at, clip.start(), clip.end() - Ticks{1});
        points.emplace_back(x, yOf(TimeRemapBridge::speedAt(clip, clip.curveTimeAt(t))));
    }
    mesh.addPolyline(points, 3.5, ctx.style.volumeKeyBorder);
    mesh.addPolyline(points, 1.5, ctx.style.volumeColor);
    if (const Curve* curve = clip.timeRemap()) {
        for (const auto& key : curve->keys) {
            const Ticks t = clip.start() + key.time - clip.sourceIn;
            if (t < clip.start() || t >= clip.end()) continue;
            const double x = t.toSeconds() * ctx.pps - ctx.originX;
            if (x < x0 || x > x1) continue;
            const double y = yOf(TimeRemapBridge::speedAt(clip, key.time));
            // Separate halves communicate the handles used to spread a speed transition.
            mesh.addRect(QRectF(x - 6, y - 4, 5, 8), ctx.style.selectionColor);
            mesh.addRect(QRectF(x + 1, y - 4, 5, 8), ctx.style.selectionColor);
        }
    }
}

void removeLabel(LabelPool& pool, TrackBucket& b, std::unordered_map<std::uint64_t, LabelEntry>::iterator it) {
    delete it->second.badge;
    delete it->second.fx;
    if (it->second.node)
        pool.release(it->second.node);
    b.labels.erase(it);
}

// 0.4: the fx chip right after the unabridged name (before the micro-tag): "fx" in clipInk on a pill in the fx colour
// (violet: stack effects; amber: changed or keyframed intrinsics, time remapping) with a clipInk hairline, so it reads on
// any label colour. On a clip whose own fill is close to that colour (the default violet label under the violet chip) the
// chip is inverted: a clipInk pill with "fx" in the fx colour. Only when it fits, like the micro-tags. Returns the width
// it takes (with the gap after it), 0 = none.
qreal placeFxChip(BuildContext& ctx, LabelEntry& e, const BuiltClip& c, ClipFx fx, const QColor& clipFill,
                  qreal available) {
    const auto remove = [&] {
        delete e.fx;
        e.fx = nullptr;
        e.fxState = ClipFx::None;
    };
    if (fx == ClipFx::None || e.pending) {
        remove();
        return 0;
    }
    const TimelineStyle& st = ctx.style;
    QFont font = st.labelFont;
    font.setPixelSize(static_cast<int>(st.microSize));
    font.setLetterSpacing(QFont::AbsoluteSpacing, st.microTracking);
    const QFontMetricsF metrics(font);
    const QString text = QStringLiteral("fx");
    const qreal chipHeight = std::ceil(metrics.height()) + 2;
    const qreal chipWidth = std::ceil(metrics.horizontalAdvance(text)) + 8;
    const qreal offset = ctx.names.width(ctx.metrics, e.text) + 6;
    const qreal chipY = e.y + (ctx.metrics.height() - chipHeight) / 2;
    if (offset + chipWidth > available || chipY < c.y + st.stripHeight || chipY + chipHeight > c.y + c.h) {
        remove();
        return 0;
    }
    const QColor fill = fx == ClipFx::Effects ? st.fxEffectsColor : st.fxChangedColor;
    const double dr = fill.redF() - clipFill.redF();
    const double dg = fill.greenF() - clipFill.greenF();
    const double db = fill.blueF() - clipFill.blueF();
    const bool inverted = clipFill.alphaF() > 0.5 && std::sqrt(dr * dr + dg * dg + db * db) < 0.25;
    if (e.fx && (e.fxStyleRevision != ctx.styleRevision || e.fxState != fx || e.fxInverted != inverted))
        remove();
    if (!e.fx) {
        e.fx = new QSGTransformNode;
        ctx.labelGroup->appendChildNode(e.fx);
        auto* plate = MeshBuilder::createNode();
        e.fx->appendChildNode(plate);
        MeshBuilder mesh;
        mesh.addRoundedRect(QRectF(0, 0, chipWidth, chipHeight), chipHeight / 2, inverted ? st.markerInkColor : fill,
                            st.markerInkColor, 0.75);
        mesh.commit(plate);
        auto* glyphs = ctx.window->createTextNode();
        e.fx->appendChildNode(glyphs);
        glyphs->setColor(inverted ? fill : st.markerInkColor);
        QTextLayout layout(text, font);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        line.setLineWidth(chipWidth);
        layout.endLayout();
        glyphs->addTextLayout({4, 1}, &layout);
        e.fxStyleRevision = ctx.styleRevision;
        e.fxState = fx;
        e.fxInverted = inverted;
    }
    QMatrix4x4 transform;
    transform.translate(static_cast<float>(e.x + offset), static_cast<float>(chipY));
    e.fx->setMatrix(transform);
    return chipWidth + 4;
}

// A scene-graph micro-tag, only when it fits AFTER the unabridged label (and, 0.4, the fx chip: `after` px). Never
// steals label space. Kept with the label rather than the track mesh so sticky labels, scrolling and clip motion stay
// together.
void placeGeneratedBadge(BuildContext& ctx, LabelEntry& e, const BuiltClip& c, const QString& tag,
                         qreal available, qreal after = 0) {
    const auto remove = [&] {
        delete e.badge;
        e.badge = nullptr;
    };
    if (tag.isEmpty() || e.pending) {
        remove();
        return;
    }
    const TimelineStyle& st = ctx.style;
    QFont font = st.labelFont;
    font.setPixelSize(static_cast<int>(st.microSize));
    font.setLetterSpacing(QFont::AbsoluteSpacing, st.microTracking);
    const QFontMetricsF metrics(font);
    const QString text = tag;
    const qreal tagHeight = std::ceil(metrics.height()) + 2;
    const qreal lean = tagHeight * std::tan(st.sliceAngle * std::numbers::pi / 180.0);
    const qreal tagWidth = std::ceil(metrics.horizontalAdvance(text)) + 8 + lean;
    const qreal offset = ctx.names.width(ctx.metrics, e.text) + 6 + after;
    const qreal tagY = e.y + (ctx.metrics.height() - tagHeight) / 2;
    if (offset + tagWidth > available || tagY < c.y + st.stripHeight || tagY + tagHeight > c.y + c.h) {
        remove();
        return;
    }
    if (e.badge && (e.badgeStyleRevision != ctx.styleRevision || e.badgeText != text))
        remove();
    if (!e.badge) {
        e.badge = new QSGTransformNode;
        ctx.labelGroup->appendChildNode(e.badge);
        auto* plate = MeshBuilder::createNode();
        e.badge->appendChildNode(plate);
        MeshBuilder mesh;
        // The plate takes the label's own ink (v3: black on the solid clip), so it reads on whatever the label does.
        const QColor fill = withAlpha(e.color, 0.14);
        mesh.addRect(QRectF(0, 0, tagWidth - lean, tagHeight), fill);
        mesh.addTriangle({tagWidth - lean, 0}, {tagWidth, tagHeight}, {tagWidth - lean, tagHeight}, fill);
        const std::array<QPointF, 5> edge{{{0, 0}, {tagWidth - lean, 0}, {tagWidth, tagHeight},
                                          {0, tagHeight}, {0, 0}}};
        mesh.addPolyline(edge, 0.75, withAlpha(e.color, 0.55));
        mesh.commit(plate);
        auto* glyphs = ctx.window->createTextNode();
        e.badge->appendChildNode(glyphs);
        glyphs->setColor(e.color);
        QTextLayout layout(text, font);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        line.setLineWidth(tagWidth);
        layout.endLayout();
        glyphs->addTextLayout({4, 1}, &layout);
        e.badgeStyleRevision = ctx.styleRevision;
        e.badgeText = text;
    }
    QMatrix4x4 transform;
    transform.translate(static_cast<float>(e.x + offset), static_cast<float>(tagY));
    e.badge->setMatrix(transform);
}

// Places (or removes) the label of `c`. Returns true when the label is pinned to the view's left edge.
bool placeLabel(BuildContext& ctx, TrackBucket& b, const BuiltClip& c) {
    const TimelineStyle& st = ctx.style;
    const qreal x = std::max(c.x0, ctx.viewLeft) + st.labelPadding;
    const qreal available = c.x1 - st.labelPadding - x;
    auto existing = b.labels.find(c.id.value);
    if (available < st.minLabelWidth) {
        if (existing != b.labels.end())
            removeLabel(ctx.pool, b, existing);
        return false;
    }
    QString name = QString::fromStdString(b.track->clips[c.index].name());
    if (const Clip& nestedClip = b.track->clips[c.index]; nestedClip.isNested() && ctx.facts) {
        if (const auto info = ctx.facts->nested.find(nestedClip.sequence.value); info != ctx.facts->nested.end() && info->second.multicam) {
            const std::size_t angle = nestedClip.angles.first();
            if (angle < info->second.angles.size())
                name = QStringLiteral("MC%1 · %2").arg(angle + 1).arg(info->second.angles[angle]);
        }
    }
    if (ctx.showSpeed && !c.audio) {
        const Clip& clip = b.track->clips[c.index];
        name += QStringLiteral(" · ") + TimeRemapBridge::speedLabel(clip);
    } else if (ctx.showClipSpeed) {
        // 0.4: a clip not at a uniform 100 % forward shows its speed in brackets, as Premiere does ("[50%]").
        if (const QString speed = clipSpeedText(b.track->clips[c.index]); !speed.isEmpty())
            name += QStringLiteral(" [") + speed + QLatin1Char(']');
    }
    // Elide only when the name does not fit, at an 8 px-quantised width, so zooming re-lays text out rarely.
    const QString text = ctx.names.width(ctx.metrics, name) <= available
                             ? name
                             : ctx.names.elide(ctx.metrics, name, std::floor(available / 8.0) * 8.0);
    if (text.isEmpty()) {
        if (existing != b.labels.end())
            removeLabel(ctx.pool, b, existing);
        return false;
    }
    const QColor color = c.audio ? st.audioLabelColor : st.videoLabelColor;
    const qreal y = c.audio        ? c.y + st.stripHeight + st.labelTop
                    : c.header > 0 ? c.y + st.stripHeight + (c.header - ctx.metrics.height()) / 2 // in the name band
                    : c.thumbs || ctx.showSpeed ? c.y + st.stripHeight + 2 // above the thumbnail or speed band
                                   : c.y + st.stripHeight + (c.h - st.stripHeight - ctx.metrics.height()) / 2;
    const bool needsLayout = existing == b.labels.end() || existing->second.text != text || existing->second.color != color;
    if (needsLayout && ctx.labelBudget <= 0) {
        ctx.labelsPending = true; // keep the old text (or no label) until a later frame
        if (existing == b.labels.end())
            return false;
    }
    LabelEntry& e = b.labels[c.id.value];
    e.pending = needsLayout && ctx.labelBudget <= 0;
    if (!e.node) {
        e.node = ctx.pool.take(ctx.window);
        ctx.labelGroup->appendChildNode(e.node);
    }
    if (needsLayout && ctx.labelBudget > 0) {
        --ctx.labelBudget;
        if (e.color != color) {
            delete e.badge;
            e.badge = nullptr;
        }
        e.node->clear();
        e.node->setColor(color);
        QTextLayout layout(text, st.labelFont);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        line.setLineWidth(available + st.labelPadding);
        line.setPosition({0, 0});
        layout.endLayout();
        e.node->addTextLayout({0, 0}, &layout);
        e.text = text;
        e.color = color;
    }
    if (e.x != x || e.y != y) {
        QMatrix4x4 m;
        m.translate(static_cast<float>(x), static_cast<float>(y));
        e.node->setMatrix(m);
        e.x = x;
        e.y = y;
    }
    const Clip& clip = b.track->clips[c.index];
    QString tag;
    if (ctx.generated && ctx.generated->contains(clip.media.value))
        tag = QStringLiteral("AI");
    else if (ctx.facts) {
        if (const auto badge = ctx.facts->badges.find(clip.id.value); badge != ctx.facts->badges.end())
            tag = badge->second == QLatin1String("enhanced") ? QStringLiteral("ENHANCED") : QStringLiteral("NOT RENDERED");
    }
    if (ctx.facts) {
        if (const auto channels = ctx.facts->channels.find(clip.id.value); channels != ctx.facts->channels.end())
            tag = tag.isEmpty() ? channels->second : tag + QStringLiteral(" · ") + channels->second;
    }
    // 0.4: the fx chip (read from the clip itself, so a drag's preview and an Alt-drag's copies show theirs too).
    const ClipFx fx = ctx.showFxBadges && text == name ? clipFxOf(clip) : ClipFx::None;
    const qreal fxWidth = placeFxChip(ctx, e, c, fx, c.audio ? st.audioFill(clip.colorIndex) : st.clipBody(clip.colorIndex),
                                      available);
    placeGeneratedBadge(ctx, e, c, text == name ? tag : QString(), available, fxWidth);
    return c.x0 < ctx.viewLeft;
}

/// M5l: the track's fader (dB) or pan (-1..1) as a line across the row, sampled every 3 px, keys as squares.
void addTrackLane(BuildContext& ctx, const Track& track, const TimelineView::Row& row, double x0, double x1) {
    if (x1 - x0 < 2 || row.height < 16)
        return;
    const TimelineStyle& st = ctx.style;
    const bool pan = ctx.trackLane == BuildContext::TrackLane::Pan;
    const ParamValue& value = pan ? track.pan : track.volume;
    const QColor color = pan ? st.trackPanLane : st.trackVolumeLane;
    const QRectF area = TimelineView::volumeArea(QRectF(0, row.y, 1, row.height));
    const auto yOf = [&](double v) {
        return pan ? area.top() + (1.0 - (std::clamp(v, -1.0, 1.0) + 1.0) / 2.0) * area.height()
                   : area.top() + TimelineView::volumeFraction(v) * area.height();
    };
    const auto tickAt = [&](double x) {
        return Ticks{static_cast<std::int64_t>((x + ctx.originX) / ctx.pps * static_cast<double>(kTicksPerSecond))};
    };
    QColor underlay = st.volumeKeyBorder;
    underlay.setAlphaF(0.55f);
    std::vector<QPointF> points;
    if (!value.isCurve()) {
        points = {{x0, yOf(value.constant())}, {x1, yOf(value.constant())}};
    } else {
        const int n = static_cast<int>(std::clamp(std::ceil((x1 - x0) / 3.0), 1.0, 4000.0));
        points.reserve(static_cast<std::size_t>(n) + 1);
        for (int i = 0; i <= n; ++i) {
            const double x = x0 + (x1 - x0) * i / n;
            points.emplace_back(x, yOf(value.valueAt(tickAt(x))));
        }
    }
    ctx.top->addPolyline(points, 3.5, underlay);
    ctx.top->addPolyline(points, 1.5, color);
    if (const Curve* curve = value.curve()) {
        for (const Keyframe& k : curve->keys) {
            const double x = static_cast<double>(k.time.value) / static_cast<double>(kTicksPerSecond) * ctx.pps - ctx.originX;
            if (x < x0 - 4 || x > x1 + 4)
                continue;
            const double y = yOf(k.value);
            ctx.top->addRect(QRectF(x - 3.5, y - 3.5, 7, 7), st.volumeKeyBorder);
            ctx.top->addRect(QRectF(x - 2.5, y - 2.5, 5, 5), color);
        }
    }
}

// Rebuilds one track's mesh and labels for the built range. Returns {built clips, visible clips}.
std::pair<int, int> buildTrack(BuildContext& ctx, TrackBucket& b, const TimelineView::Row& row) {
    const TimelineStyle& st = ctx.style;
    ctx.mesh.clear();
    ctx.top->clear();
    b.clips.clear();
    b.stuck.clear();
    b.nestedClips = 0;
    b.angleCuts = 0;
    b.trackLane = false;
    b.responsiveBands.clear();
    std::vector<std::uint64_t> usedTags;
    // Thumbnails of this track, grouped by media (one textured node each).
    std::vector<std::pair<std::uint64_t, std::vector<FilmQuad>>> filmGroups;
    std::unordered_map<std::uint64_t, std::shared_ptr<const FilmstripAtlas>> filmAtlases;
    b.y = row.y;
    b.h = row.height;
    int built = 0;
    int visible = 0;
    std::vector<std::uint64_t> used;

    // Rows more than a viewport above/below the view get no geometry.
    b.visible = row.y + row.height >= ctx.scrollY - ctx.viewHeight && row.y <= ctx.scrollY + 2 * ctx.viewHeight;
    if (b.visible && b.track) {
        const auto& clips = b.track->clips;
        const double secondsPerTick = 1.0 / static_cast<double>(kTicksPerSecond);
        const auto toX = [&](Ticks t) { return static_cast<double>(t.value) * secondsPerTick * ctx.pps - ctx.originX; };
        const Ticks t0{static_cast<std::int64_t>(ctx.originX / ctx.pps * static_cast<double>(kTicksPerSecond))};
        const double endX = ctx.builtWidth;
        std::size_t index = firstClipEndingAfter(clips, t0);
        auto it = clips.iteratorAt(index);

        // Runs of sub-pixel clips are merged into one rectangle.
        bool run = false;
        double runX0 = 0, runX1 = 0;
        QColor runColor;
        const auto flush = [&] {
            if (run)
                ctx.mesh.addRect(QRectF(runX0, row.y, std::max(1.0, runX1 - runX0), row.height), runColor);
            run = false;
        };

        // While clips slide into place, ones starting past the built range may still be drawn inside it.
        const bool sliding = ctx.shiftPx && !ctx.shiftPx->empty();
        const double stopX = sliding ? endX + ctx.builtWidth : endX;
        const Clip* previous = index > 0 ? &clips[index - 1] : nullptr; // through edits
        for (; it != clips.end(); ++it, ++index) {
            const Clip* before = previous;
            previous = &*it;
            double x0 = toX(it->start());
            if (x0 >= stopX)
                break;
            double x1 = toX(it->end());
            double shiftX = 0;
            if (sliding) {
                if (const auto shift = ctx.shiftPx->find(it->id.value); shift != ctx.shiftPx->end()) {
                    shiftX = shift->second;
                    x0 += shift->second;
                    x1 += shift->second;
                }
            }
            if (x0 >= endX || x1 <= 0)
                continue;
            ++built;
            if (x1 > ctx.viewLeft && x0 < ctx.viewRight)
                ++visible;
            // v3: solid clips in the label colour (v2: a tinted body under a strip of the label colour).
            const QColor label = st.clipColor(it->colorIndex);
            const QColor fill = row.audio ? st.audioFill(it->colorIndex) : st.clipBody(it->colorIndex);
            if (x1 - x0 < 1.5) {
                if (run && x0 <= runX1 + 1.0) {
                    runX1 = std::max(runX1, x1);
                } else {
                    flush();
                    run = true;
                    runX0 = x0;
                    runX1 = x1;
                    runColor = label;
                }
                continue;
            }
            flush();
            const double right = x1 - (x1 - x0 > 3 ? 1.0 : 0.0); // 1 px gap between adjacent clips
            const double lift = it->id == ctx.hovered ? -ctx.lift : 0.0;
            QRectF rect(x0, row.y + lift, right - x0, row.height);
            // A dragged clip lifts by growing in height only (0.1.3): its edges stay on the frames it will land on. Scaled
            // around its centre, a long clip's head used to reach tens of pixels into the clip it was snapped against,
            // and jumped back on release.
            if (ctx.lifted && ctx.liftScale != 1.0 && ctx.lifted->contains(it->id.value)) {
                const double centre = rect.center().y();
                rect.setHeight(rect.height() * ctx.liftScale);
                rect.moveTop(centre - rect.height() / 2);
            }
            const QColor border = row.audio ? st.audioBorder(it->colorIndex) : st.clipBorder;
            ctx.mesh.addRoundedRect(rect, st.clipRadius, fill, border, 1.0);
            if (st.stripHeight > 0 && rect.height() > st.stripHeight * 3)
                ctx.mesh.addRoundedRect(QRectF(rect.left(), rect.top(), rect.width(), st.stripHeight),
                                        std::min<qreal>(st.clipRadius, st.stripHeight / 2), label);
            // Video filmstrips (M5i); a nested or multicam clip shows its nested sequence's pictures (I.9). Looked up
            // first: with thumbnails, v3 gives the name a solid band in the label colour above them (`band`), and the
            // nested stripe and icon go under it.
            const QRectF body = rect.adjusted(1, st.stripHeight, -1, -1);
            FilmstripSource::Lookup look;
            if (!row.audio && ctx.film != BuildContext::Film::Off && ctx.filmstrips && body.height() >= 12 && body.width() >= 2)
                look = it->isNested() ? ctx.filmstrips->lookupNested(*it) : ctx.filmstrips->lookup(it->media);
            const bool film = look.state != FilmstripSource::State::Unavailable;
            const qreal band = film && st.clipHeaderHeight > 0 && body.height() >= st.clipHeaderHeight + 12
                                   ? st.clipHeaderHeight
                                   : 0.0;
            // Where marks along the clip's top start: under the strip and the name band. A mark on the clip's very top
            // edge (no strip, no band) keeps clear of the rounded corners.
            const double markTop = rect.top() + st.stripHeight + band;
            const double cornerInset = st.stripHeight + band > 0 ? 1.0 : std::max(1.0, st.clipRadius / 2);
            // M5l: a nested or multi-camera clip has a second stripe under its label strip and a stacked icon at its
            // right end; a cut between two segments of one multi-camera clip (an angle switch) is marked.
            if (it->isNested() && ctx.facts && rect.height() > std::max<qreal>(st.stripHeight * 3, 8)) {
                const auto info = ctx.facts->nested.find(it->sequence.value);
                const bool multicam = info != ctx.facts->nested.end() && info->second.multicam;
                const QColor stripe = multicam ? st.multicamStripe : st.nestedStripe;
                // At the foot of the name band (the thumbnails start under it), else under the strip, else just
                // inside the clip's top.
                const double stripeTop = band > 0               ? markTop - st.nestedStripeHeight
                                         : st.stripHeight > 0 ? markTop
                                                              : markTop + 2;
                ctx.mesh.addRect(QRectF(rect.left() + cornerInset, stripeTop, rect.width() - 2 * cornerInset,
                                        st.nestedStripeHeight),
                                 stripe);
                ++b.nestedClips;
                if (rect.width() >= 40 && rect.height() >= 18) {
                    const double ix = rect.right() - 15;
                    // In the name band when there is one (its right end), else under the stripe.
                    const double iy = band > 0 ? rect.top() + st.stripHeight + (band - 9) / 2
                                               : stripeTop + st.nestedStripeHeight + 4;
                    const auto box = [&](double x, double y) {
                        const std::array<QPointF, 5> edge{{{x, y}, {x + 8, y}, {x + 8, y + 6}, {x, y + 6}, {x, y}}};
                        ctx.top->addPolyline(edge, 1.25, stripe);
                    };
                    box(ix + 3, iy);
                    box(ix, iy + 3);
                }
                if (multicam && before && before->isNested() && before->sequence == it->sequence && before->end() == it->start()) {
                    const double cx = rect.left();
                    const double s = std::min(4.0, rect.height() / 6);
                    const double top = stripeTop + st.nestedStripeHeight;
                    ctx.top->addTriangle({cx - s, top}, {cx + s, top}, {cx, top + 1.6 * s}, st.multicamStripe);
                    ++b.angleCuts;
                }
            }
            const bool thumbs = film;
            if (film) {
                std::vector<FilmQuad>* quads = nullptr;
                const std::uint64_t filmKey = look.key != 0 ? look.key : it->media.value;
                if (look.atlas) {
                    auto group = std::find_if(filmGroups.begin(), filmGroups.end(),
                                              [&](const auto& g) { return g.first == filmKey; });
                    if (group == filmGroups.end()) {
                        filmGroups.emplace_back(filmKey, std::vector<FilmQuad>{});
                        group = std::prev(filmGroups.end());
                        filmAtlases[filmKey] = look.atlas;
                    }
                    quads = &group->second;
                }
                std::vector<FilmQuad> none;
                // v3: the thumbnails start under the name band, which is the clip's own solid fill.
                layoutFilmstrip(ctx, *it, body.adjusted(0, band, 0, 0), shiftX, look, quads ? *quads : none);
                // Without a band (a short track), the label reads on a scrim along the top of the thumbnails.
                if (band <= 0 && rect.width() > 2 * st.labelPadding + st.minLabelWidth)
                    ctx.top->addRect(QRectF(body.left(), body.top(), body.width(), std::min(body.height(), ctx.metrics.height() + 4)),
                                     st.scrimColor);
            }
            if (!it->enabled || (ctx.offline && ctx.offline->contains(it->media.value)))
                addHatch(*ctx.top, st, body);
            // M5o: a graphic with responsive time (Premiere's Responsive Design - Time): its protected intro plays from
            // the clip's start and its outro ends at its end; the middle stretches. Both are tinted bands with an edge
            // line where the stretch begins / ends, tagged when wide enough.
            if (!row.audio && body.width() >= 2 && body.height() >= 6) {
                if (const Graphic* graphic = it->graphic(); graphic && graphic->responsive) {
                    const ResponsiveTime& r = *graphic->responsive;
                    double intro = static_cast<double>(std::max<std::int64_t>(0, r.intro.value));
                    double outro = static_cast<double>(std::max<std::int64_t>(0, r.outro.value));
                    const double length = static_cast<double>(it->duration().value);
                    if (intro + outro > length && intro + outro > 0) { // a short clip plays both, squeezed in proportion
                        const double k = length / (intro + outro);
                        intro *= k;
                        outro *= k;
                    }
                    const double pxPerTick = ctx.pps / static_cast<double>(kTicksPerSecond);
                    const qreal introW = std::min<qreal>(body.width(), intro * pxPerTick);
                    const qreal outroW = std::min<qreal>(body.width(), outro * pxPerTick);
                    if (introW > 0) {
                        ctx.top->addRect(QRectF(body.left(), body.top(), introW, body.height()), st.responsiveBand);
                        ctx.top->addRect(QRectF(body.left() + introW - 0.5, body.top(), 1, body.height()), st.responsiveEdge);
                    }
                    if (outroW > 0) {
                        ctx.top->addRect(QRectF(body.right() - outroW, body.top(), outroW, body.height()), st.responsiveBand);
                        ctx.top->addRect(QRectF(body.right() - outroW - 0.5, body.top(), 1, body.height()), st.responsiveEdge);
                    }

                    b.responsiveBands.emplace_back(it->id.value, QPair<qreal, qreal>(introW, outroW));
                    // The tags sit at the bottom of the bands (the clip's label is at the top).
                    const auto tag = [&](std::uint64_t key, const QString& text, qreal x0, qreal width) {
                        QFont font = st.labelFont;
                        font.setPixelSize(static_cast<int>(st.microSize));
                        font.setLetterSpacing(QFont::AbsoluteSpacing, st.microTracking);
                        const QFontMetricsF metrics(font);
                        const qreal tagWidth = std::ceil(metrics.horizontalAdvance(text)) + 6;
                        const qreal tagHeight = std::ceil(metrics.height());
                        if (text.isEmpty() || width < tagWidth + 2 || body.height() < tagHeight + st.stripHeight)
                            return;
                        RegionTag& t = b.regionTags[key];
                        if (t.node && (t.styleRevision != ctx.styleRevision || t.text != text))
                            removeRegionTag(t);
                        if (!t.node) {
                            t.node = new QSGTransformNode;
                            ctx.labelGroup->appendChildNode(t.node);
                            auto* glyphs = ctx.window->createTextNode();
                            t.node->appendChildNode(glyphs);
                            glyphs->setColor(st.responsiveEdge);
                            QTextLayout layout(text, font);
                            layout.beginLayout();
                            QTextLine line = layout.createLine();
                            line.setLineWidth(tagWidth);
                            layout.endLayout();
                            glyphs->addTextLayout({3, 0}, &layout);
                            t.styleRevision = ctx.styleRevision;
                            t.text = text;
                        }
                        QMatrix4x4 m;
                        m.translate(static_cast<float>(x0), static_cast<float>(body.bottom() - tagHeight - 1));
                        t.node->setMatrix(m);
                        usedTags.push_back(key);
                    };
                    tag(it->id.value * 2, ctx.introText, body.left(), introW);
                    tag(it->id.value * 2 + 1, ctx.outroText, body.right() - outroW, outroW);
                }
            }
            // Through edits: the cut continues the same source (Premiere's bow tie).
            if (ctx.throughEdits && before && isThroughEdit(*before, *it) && rect.height() >= 12) {
                const double s = std::min(5.0, rect.height() / 5);
                const double cx = rect.left();
                const double cy = rect.center().y();
                ctx.top->addTriangle({cx - s - 1, cy - s}, {cx - 1, cy}, {cx - s - 1, cy + s}, st.throughEditColor);
                ctx.top->addTriangle({cx + s + 1, cy - s}, {cx + 1, cy}, {cx + s + 1, cy + s}, st.throughEditColor);
            }
            // Duplicate frames: a band along the bottom in the source's colour.
            if (!row.audio && ctx.duplicates) {
                if (const auto dup = ctx.duplicates->find(it->id.value); dup != ctx.duplicates->end()) {
                    for (const DuplicateRange& d : dup->second) {
                        const double xa = std::max(rect.left(), toX(d.range.start) + shiftX);
                        const double xb = std::min(rect.right(), toX(d.range.end()) + shiftX);
                        if (xb - xa >= 0.5)
                            ctx.top->addRect(QRectF(xa, rect.bottom() - st.duplicateBandHeight - 1, xb - xa, st.duplicateBandHeight),
                                             indexed(st.duplicateColors, d.colorIndex, st.selectionColor));
                    }
                }
            }
            // Clip markers (the media's markers inside the source range this clip uses).
            if (ctx.clipMarkers && rect.height() >= 10) {
                if (const auto found = ctx.clipMarkers->find(it->media.value); found != ctx.clipMarkers->end()) {
                    for (const ClipMarkerMark& m : clipMarkerMarks(*it, *found->second)) {
                        const double x = toX(m.at) + shiftX;
                        const QColor c = st.markerColor(m.colorIndex);
                        const double top = rect.top() + st.stripHeight;
                        if (m.ranged) {
                            const double xe = m.end ? std::min(rect.right(), toX(*m.end) + shiftX) : rect.right();
                            if (xe > x)
                                ctx.top->addRect(QRectF(x, top, xe - x, 2), c);
                        }
                        ctx.top->addRect(QRectF(x - 0.5, top, 1, rect.height() - st.stripHeight - 1), withAlpha(c, 0.55));
                        ctx.top->addTriangle({x, top}, {x + 6, top}, {x, top + 7}, c);
                    }
                }
            }

            if (row.audio) {
                // Waveform over the built part of the clip, 2 px per peak column.
                const double wx0 = std::max(rect.left(), 0.0);
                const double wx1 = std::min(rect.right(), endX);
                const double half = (row.height - st.waveTop - st.waveBottom) / 2;
                if (wx1 - wx0 >= 2 && half >= 2) {
                    const auto n = static_cast<std::size_t>(std::min(4096.0, std::floor((wx1 - wx0) / 2)));
                    const auto tickAt = [&](double x) {
                        return Ticks{static_cast<std::int64_t>((x + ctx.originX) / ctx.pps * static_cast<double>(kTicksPerSecond))};
                    };
                    const Ticks ta = std::clamp(tickAt(wx0), it->start(), it->end());
                    const Ticks tb = std::clamp(tickAt(wx1), it->start(), it->end());
                    ctx.peakScratch.resize(n);
                    if (n > 0 && tb > ta
                        && ctx.peaks.peaks(it->media, it->stream, it->sourceTimeAt(ta), it->sourceTimeAt(tb),
                                           ctx.peakScratch)) {
                        if (ctx.volumeWaveforms)
                            scaleByClipVolume(*it, wx0, (wx1 - wx0) / static_cast<double>(n), tickAt, ctx.peakScratch);
                        ctx.loudestWaveform = std::max(ctx.loudestWaveform, *std::ranges::max_element(ctx.peakScratch));
                        if (ctx.logarithmic) {
                            // Premiere's logarithmic waveform scaling: a 48 dB range.
                            for (float& p : ctx.peakScratch)
                                p = p <= 0 ? 0.0f : static_cast<float>(std::clamp((20.0 * std::log10(p) + 48.0) / 48.0, 0.0, 1.0));
                        }
                        const double step = (wx1 - wx0) / static_cast<double>(n);
                        if (ctx.rectified) {
                            ctx.mesh.addColumns(wx0, step, rect.top() + st.waveTop + 2 * half, 2 * half, ctx.peakScratch,
                                                st.waveform(it->colorIndex));
                        } else {
                            const double centre = rect.top() + st.waveTop + half;
                            ctx.mesh.addEnvelope(wx0, step, centre, half, ctx.peakScratch, st.waveform(it->colorIndex));
                        }
                    }
                    // p02 audio: the part of the clip whose peaks are still being decoded is shaded (the waveform fills
                    // in as they build; refreshPeaks() rebuilds this track when its media's peaks move on).
                    if (n > 0 && tb > ta) {
                        if (const std::optional<Ticks> until = ctx.peaks.buildingUntil(it->media, it->stream)) {
                            const double step = (wx1 - wx0) / static_cast<double>(n);
                            const QColor shade = withAlpha(st.waveform(it->colorIndex), 0.16);
                            const QColor edge = withAlpha(st.waveform(it->colorIndex), 0.6);
                            const double top = rect.top() + st.waveTop;
                            bool any = false;
                            std::size_t k = 0;
                            while (k < n) {
                                // Columns whose source time is past the decoded part (either side, for reversed clips).
                                const auto pending = [&](std::size_t c) {
                                    return it->sourceTimeAt(tickAt(wx0 + (static_cast<double>(c) + 0.5) * step)) >= *until;
                                };
                                if (!pending(k)) {
                                    ++k;
                                    continue;
                                }
                                const std::size_t from = k;
                                while (k < n && pending(k))
                                    ++k;
                                const double xa = wx0 + static_cast<double>(from) * step;
                                const double xb = wx0 + static_cast<double>(k) * step;
                                ctx.mesh.addRect(QRectF(xa, top, xb - xa, 2 * half), shade);
                                // A thin edge where the decoded part ends (the build's progress).
                                if (from > 0)
                                    ctx.mesh.addRect(QRectF(xa, top, 1, 2 * half), edge);
                                else if (k < n)
                                    ctx.mesh.addRect(QRectF(xb - 1, top, 1, 2 * half), edge);
                                any = true;
                            }
                            if (any)
                                ++ctx.buildingWaveforms;
                        }
                    }
                }
                if (ctx.showVolume)
                    addVolumeLine(ctx, *it, rect, std::max(rect.left(), 0.0), std::min(rect.right(), endX));
            }

            if (ctx.showOpacity && !row.audio)
                addVolumeLine(ctx, *it, rect, std::max(rect.left(), 0.0), std::min(rect.right(), endX), true);
            if (ctx.showSpeed && !row.audio)
                addSpeedLine(ctx, *it, rect, std::max(rect.left(), 0.0), std::min(rect.right(), endX));

            BuiltClip bc{it->id, index, rect.left(), rect.right(), rect.top(), rect.height(), row.audio, thumbs, band};
            b.clips.push_back(bc);
            // Labels only for clips in the view; clips scrolled into view get theirs in the scroll update.
            if (x1 > ctx.viewLeft && x0 < ctx.viewRight && right - x0 >= st.minLabelWidth + 2 * st.labelPadding) {
                if (placeLabel(ctx, b, bc))
                    b.stuck.push_back(b.clips.size() - 1);
                used.push_back(bc.id.value);
            } else if (b.labels.contains(bc.id.value) && right - x0 >= st.minLabelWidth + 2 * st.labelPadding) {
                used.push_back(bc.id.value); // keep labels of clips just outside the view (scrolling back)
                LabelEntry& entry = b.labels.at(bc.id.value);
                delete entry.badge; // do not retain stale provenance/style on an offscreen label
                entry.badge = nullptr;
                delete entry.fx; // 0.4: nor its fx chip
                entry.fx = nullptr;
                entry.fxState = ClipFx::None;
                entry.pending = true; // place its tag again if it scrolls into view
            }
        }
        flush();
        if (row.audio && ctx.trackLane != BuildContext::TrackLane::Off) {
            addTrackLane(ctx, *b.track, row, std::max(0.0, ctx.viewLeft - ctx.builtWidth / 4), std::min(endX, ctx.viewRight + ctx.builtWidth / 4));
            b.trackLane = true;
        }
    }
    ctx.mesh.commit(b.mesh);
    ctx.top->commit(b.top);
    commitFilm(ctx, b, filmGroups, filmAtlases);

    // M5o: drop the band tags of clips that are gone, narrower now, or no longer responsive.
    std::sort(usedTags.begin(), usedTags.end());
    for (auto it = b.regionTags.begin(); it != b.regionTags.end();) {
        if (!std::binary_search(usedTags.begin(), usedTags.end(), it->first)) {
            removeRegionTag(it->second);
            it = b.regionTags.erase(it);
        } else {
            ++it;
        }
    }
    // Drop labels of clips that are gone or too small now.
    std::sort(used.begin(), used.end());
    for (auto it = b.labels.begin(); it != b.labels.end();) {
        if (!std::binary_search(used.begin(), used.end(), it->first)) {
            auto next = std::next(it);
            removeLabel(ctx.pool, b, it);
            it = next;
        } else {
            ++it;
        }
    }
    return {built, visible};
}

} // namespace

QSGNode* TimelineView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    QElapsedTimer timer;
    timer.start();
    auto* root = static_cast<TimelineRootNode*>(oldNode);
    const bool fresh = root == nullptr;
    if (fresh)
        root = new TimelineRootNode;

    TimelineStats stats;
    stats.frames = m_stats.frames + 1;
    stats.previewNs = m_stats.previewNs; // measured by the last drag move, not here
    const double w = width();
    const double h = height();

    // ---- Built range ----
    bool full = fresh || m_pendingAll || root->pps != m_pps || root->width != w || root->height != h
             || root->styleRevision != m_styleRevision || root->layoutRevision != m_layoutRevision
             || root->buckets.size() != m_rows.size()
             || (root->sequence && m_sequence && root->sequence->id != m_sequence->id);
    if (!full && (m_scrollX < root->builtStartX || m_scrollX + w > root->builtEndX))
        full = true;
    if (full) {
        // A zoom rebuilds every frame of the gesture: build just the view. Otherwise add half a viewport on
        // each side so scrolling mostly only moves the transform.
        const bool zooming = !fresh && root->pps != m_pps;
        const double margin = zooming ? 0.0 : 0.5 * w;
        root->pps = m_pps;
        root->width = w;
        root->height = h;
        root->builtStartX = std::max(0.0, m_scrollX - margin);
        root->builtEndX = m_scrollX + w + margin;
        root->originX = root->builtStartX;
        if (root->styleRevision != m_styleRevision)
            root->names.clear(); // the label font (and the UI scale) may have changed: widths and elisions with it
        root->styleRevision = m_styleRevision;
        root->layoutRevision = m_layoutRevision;
        // Match the buckets to the rows.
        while (root->buckets.size() > m_rows.size()) {
            TrackBucket& b = root->buckets.back();
            for (auto it = b.labels.begin(); it != b.labels.end();)
                removeLabel(root->pool, b, it++);
            for (auto& [key, tag] : b.regionTags)
                removeRegionTag(tag);
            b.regionTags.clear();
            root->content->removeChildNode(b.mesh);
            delete b.mesh;
            for (QSGGeometryNode* node : b.film) {
                root->content->removeChildNode(node);
                delete node;
            }
            root->content->removeChildNode(b.top);
            delete b.top;
            root->buckets.pop_back();
        }
        while (root->buckets.size() < m_rows.size()) {
            TrackBucket b;
            b.mesh = MeshBuilder::createNode();
            b.top = MeshBuilder::createNode();
            root->content->insertChildNodeBefore(b.mesh, root->markers);
            root->content->insertChildNodeBefore(b.top, root->markers);
            root->buckets.push_back(std::move(b));
        }
    }
    root->sequence = m_sequence;

    const QFontMetricsF metrics(m_style.labelFont);
    const double lift = m_style.liftOffset * std::clamp(m_hoverProgress, 0.0, 1.0);
    BuildContext ctx{m_style,
                     window(),
                     root->labels,
                     metrics,
                     m_pps,
                     root->originX,
                     root->builtEndX - root->builtStartX,
                     m_scrollX - root->originX,
                     m_scrollX - root->originX + w,
                     m_scrollY,
                     h,
                     m_hovered,
                     lift,
                     *m_peaks,
                     root->peaks,
                     root->mesh,
                     root->names,
                     root->pool};
    ctx.showVolume = volumeBandShown();
    ctx.showOpacity = m_tool == TimelineTool::Pen;
    ctx.showSpeed = m_showSpeed;
    ctx.showClipSpeed = m_showClipSpeed;
    ctx.showFxBadges = m_showFxBadges;
    ctx.styleRevision = m_styleRevision;
    ctx.introText = m_introText;
    ctx.outroText = m_outroText;
    std::unordered_set<std::uint64_t> offline;
    std::unordered_set<std::uint64_t> generated;
    if (m_controller) {
        for (const MediaPtr& media : m_controller->snapshot()->media) {
            if (media->locator.offline && !media->isGenerator())
                offline.insert(media->id.value);
            if (media->generated.has_value())
                generated.insert(media->id.value);
        }
    }
    if (full) {
        m_drawnOffline = offline;
        m_drawnGenerated = generated;
    }
    // Keyed by redaction root: a subclip's thumbnails hide what its master's redactions cover, and the reverse (review
    // wave 5 E5-P1-1, model/RedactionMedia.h).
    std::unordered_map<std::uint64_t, std::vector<const Redaction*>> redactions;
    std::unordered_map<std::uint64_t, std::uint64_t> redactionRoots; // media -> root, where not itself
    const ProjectSnapshot drawnSnapshot = m_controller ? m_controller->snapshot() : nullptr;
    if (drawnSnapshot) {
        for (const Redaction& r : drawnSnapshot->redactions) {
            if (r.enabled)
                redactions[redactionRoot(*drawnSnapshot, r.media).value].push_back(&r);
        }
        if (!redactions.empty()) {
            for (const MediaPtr& m : drawnSnapshot->media) {
                if (m->subclip || m->derived) {
                    if (const MediaId master = redactionRoot(*drawnSnapshot, m->id); master != m->id)
                        redactionRoots.emplace(m->id.value, master.value);
                }
            }
        }
        if (full)
            m_drawnRedactions = drawnSnapshot->redactions;
    }
    ctx.redactions = &redactions;
    ctx.redactionRoots = &redactionRoots;
    ctx.offline = &offline;
    ctx.generated = &generated;
    const TimelineLaneFacts& facts = currentFacts();
    if (full)
        m_drawnFacts = facts;
    ctx.facts = &facts;
    ctx.trackLane = m_trackKeyframes == QLatin1String("volume") ? BuildContext::TrackLane::Volume
                    : m_trackKeyframes == QLatin1String("pan")  ? BuildContext::TrackLane::Pan
                                                                : BuildContext::TrackLane::Off;
    // M5i: filmstrips, clip markers, duplicate frames, through edits, waveform styles.
    ctx.top = &root->top;
    ctx.content = root->content;
    ctx.filmstrips = m_filmstrips.get();
    ctx.film = m_filmstripMode == QLatin1String("head")       ? BuildContext::Film::Head
             : m_filmstripMode == QLatin1String("headTail")   ? BuildContext::Film::HeadTail
             : m_filmstripMode == QLatin1String("continuous") ? BuildContext::Film::Continuous
                                                              : BuildContext::Film::Off;
    if (full)
        ++root->buildSerial;
    QQuickWindow* win = window();
    ctx.texture = [root, win](MediaId media, const std::shared_ptr<const FilmstripAtlas>& atlas) -> QSGTexture* {
        if (!win || !atlas || atlas->atlas.isNull())
            return nullptr;
        TimelineRootNode::AtlasTexture& t = root->textures[media.value];
        if (t.atlas != atlas || !t.texture) {
            delete t.texture;
            t.texture = win->createTextureFromImage(atlas->atlas, QQuickWindow::TextureIsOpaque);
            if (t.texture)
                t.texture->setFiltering(QSGTexture::Linear);
            t.atlas = atlas;
        }
        t.usedInBuild = root->buildSerial;
        return t.texture;
    };
    // Clip markers: pointers into the media items of this snapshot, which stays alive for the build.
    const ProjectSnapshot markerSnapshot = m_showClipMarkers && m_controller ? m_controller->snapshot() : nullptr;
    std::unordered_map<std::uint64_t, const std::vector<Marker>*> clipMarkers;
    if (markerSnapshot) {
        for (const MediaPtr& media : markerSnapshot->media) {
            if (!media->markers.empty())
                clipMarkers.emplace(media->id.value, &media->markers);
        }
    }
    if (!clipMarkers.empty())
        ctx.clipMarkers = &clipMarkers;
    std::unordered_map<std::uint64_t, std::vector<DuplicateRange>> duplicates;
    if (m_showDuplicates && m_sequence) {
        duplicates = duplicateFrameRanges(*m_sequence);
        ctx.duplicates = &duplicates;
    }
    ctx.throughEdits = m_showThroughEdits;
    ctx.rectified = m_rectified;
    ctx.logarithmic = m_logarithmic;
    ctx.volumeWaveforms = m_volumeWaveforms;
    std::unordered_map<std::uint64_t, double> shiftPx;
    if (!m_shiftFrom.empty()) {
        for (const auto& [id, from] : m_shiftFrom)
            shiftPx.emplace(id, from.toSeconds() * m_pps * (1.0 - m_shiftProgress));
        ctx.shiftPx = &shiftPx;
    }
    if (!m_lifted.empty()) {
        ctx.lifted = &m_lifted;
        ctx.liftScale = 1.0 + (m_style.liftScale - 1.0) * m_liftAmount;
    }
    const bool animating = m_settleAnimation->state() == QAbstractAnimation::Running
                        || m_shiftAnimation->state() == QAbstractAnimation::Running;

    // ---- Tracks: rebuild what changed ----
    // An edit rebuilds a track only when one of its ChangeSet ranges reaches into the built window.
    const auto ticksAtContentX = [&](double x) {
        return Ticks{static_cast<std::int64_t>(std::floor(x / m_pps * static_cast<double>(kTicksPerSecond)))};
    };
    const Ticks builtStart = ticksAtContentX(root->builtStartX);
    const Ticks builtEnd = ticksAtContentX(root->builtEndX) + Ticks{1};
    const auto changedInWindow = [&](const Row& row) {
        const auto it = m_pendingRanges.find(row.track->id.value);
        if (it == m_pendingRanges.end())
            return false;
        return std::ranges::any_of(it->second, [&](const TimeRange& r) {
            return r.start < builtEnd && std::max(r.end(), r.start + Ticks{1}) > builtStart;
        });
    };
    const bool hoverChanged = root->hovered != m_hovered || root->hoverProgress != m_hoverProgress;
    bool anyTrackRebuilt = false;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        TrackBucket& b = root->buckets[i];
        const Row& row = m_rows[i];
        const bool nowVisible = row.y + row.height >= m_scrollY - h && row.y <= m_scrollY + 2 * h;
        bool rebuild = full || nowVisible != b.visible || b.y != row.y || b.h != row.height || !b.track
                    || b.track->id != row.track->id || m_animatedTracks.contains(row.track->id.value);
        if (!rebuild && b.track != row.track) {
            // Changed since this bucket was built. Outside the window the old revision is identical, so the
            // bucket keeps it (its clip indices stay consistent with the built geometry).
            if (changedInWindow(row))
                rebuild = true;
            else if (m_pendingRanges.contains(row.track->id.value))
                ++stats.skippedTracks;
        }
        if (!rebuild && row.audio && !m_pendingPeakMedia.empty()) {
            // p02 audio: new peaks for media this track plays.
            rebuild = std::ranges::any_of(row.track->clips,
                                          [&](const Clip& c) { return m_pendingPeakMedia.contains(c.media.value); });
        }
        if (!rebuild && hoverChanged) {
            const auto touches = [&](ClipId id) {
                return id && std::any_of(b.clips.begin(), b.clips.end(), [&](const BuiltClip& c) { return c.id == id; });
            };
            rebuild = touches(root->hovered) || touches(m_hovered);
        }
        if (rebuild) {
            b.track = row.track;
            const auto [built, visible] = buildTrack(ctx, b, row);
            stats.builtClips += built;
            stats.visibleClips += visible;
            ++stats.rebuiltTracks;
            anyTrackRebuilt = true;
        } else {
            stats.builtClips += static_cast<int>(b.clips.size());
            for (const BuiltClip& c : b.clips) {
                if (c.x1 > ctx.viewLeft && c.x0 < ctx.viewRight)
                    ++stats.visibleClips;
            }
            // Scrolling inside the built range moves no geometry. Labels pinned to the left edge follow it,
            // and clips scrolled in from the left get their labels now.
            std::vector<std::size_t> nowStuck;
            const auto first = std::partition_point(b.clips.begin(), b.clips.end(),
                                                    [&](const BuiltClip& c) { return c.x1 <= ctx.viewLeft; });
            std::vector<std::size_t> candidates = b.stuck;
            for (auto it = first; it != b.clips.end() && it->x0 < ctx.viewRight; ++it) {
                const auto label = b.labels.find(it->id.value);
                if (it->x0 < ctx.viewLeft || label == b.labels.end() || label->second.pending)
                    candidates.push_back(static_cast<std::size_t>(it - b.clips.begin()));
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            for (std::size_t index : candidates) {
                const BuiltClip& c = b.clips[index];
                if (c.x1 - c.x0 < m_style.minLabelWidth + 2 * m_style.labelPadding)
                    continue;
                if (placeLabel(ctx, b, c))
                    nowStuck.push_back(index);
            }
            b.stuck = std::move(nowStuck);
        }
    }
    stats.fullRebuild = full;
    if (full) {
        // Atlases no track used in this full build: their textures go.
        for (auto it = root->textures.begin(); it != root->textures.end();) {
            if (it->second.usedInBuild != root->buildSerial) {
                delete it->second.texture;
                it = root->textures.erase(it);
            } else {
                ++it;
            }
        }
    }
    stats.filmstripTiles = ctx.filmTiles;
    stats.filmstripPlaceholders = ctx.filmPlaceholders;
    stats.buildingWaveforms = ctx.buildingWaveforms;
    stats.loudestWaveform = ctx.loudestWaveform;
    m_pendingPeakMedia.clear();
    stats.filmstripRedacted = ctx.filmRedacted;
    for (const TrackBucket& b : root->buckets)
        stats.filmstripNodes += static_cast<int>(b.film.size());
    stats.textures = static_cast<int>(root->textures.size());
    m_pendingAll = false;
    m_pendingRanges.clear();
    if (!animating && !m_gesture.active)
        m_animatedTracks.clear(); // the last animation frame has been built

    // ---- Content transform ----
    QMatrix4x4 m;
    m.translate(static_cast<float>(root->originX - m_scrollX), static_cast<float>(-m_scrollY));
    if (root->content->matrix() != m)
        root->content->setMatrix(m);

    // ---- Markers (content coordinates) ----
    if (full || root->contentHeight != m_contentHeight || anyTrackRebuilt || root->markerRevision != m_markerRevision) {
        root->markerRevision = m_markerRevision;
        MeshBuilder& mesh = root->mesh;
        mesh.clear();
        if (m_sequence) {
            for (const Marker& marker : m_sequence->markers) {
                const double x = marker.start.toSeconds() * m_pps - root->originX;
                const double xe = marker.end().toSeconds() * m_pps - root->originX;
                if (xe < -1 || x > ctx.builtWidth + 1)
                    continue;
                const QColor color = m_style.markerColor(marker.colorIndex);
                if (xe - x >= 1) // a range marker: a wash across its length and a line at its end
                    mesh.addRect(QRectF(x, 0, xe - x, m_contentHeight), withAlpha(color, m_style.markerOpacity * 0.3));
                mesh.addRect(QRectF(x - m_style.markerWidth / 2, 0, m_style.markerWidth, m_contentHeight),
                             withAlpha(color, m_style.markerOpacity));
                if (xe - x >= 1)
                    mesh.addRect(QRectF(xe - m_style.markerWidth / 2, 0, m_style.markerWidth, m_contentHeight),
                                 withAlpha(color, m_style.markerOpacity * 0.6));
            }
        }
        mesh.commit(root->markers);
        root->contentHeight = m_contentHeight;
    }

    // ---- Overlay: hover shadow, selection glow and outline ----
    if (full || anyTrackRebuilt || hoverChanged || root->selectionRevision != m_selectionRevision) {
        MeshBuilder& mesh = root->mesh;
        mesh.clear();
        const QColor shadow = withAlpha(m_style.shadowColor, std::clamp(m_hoverProgress, 0.0, 1.0));
        const QColor liftShadow = withAlpha(m_style.shadowColor, std::clamp(m_liftAmount, 0.0, 1.0));
        for (const TrackBucket& b : root->buckets) {
            for (const BuiltClip& c : b.clips) {
                const QRectF rect(c.x0, c.y, c.x1 - c.x0, c.h);
                if (m_style.shadowSize > 0) {
                    if (m_liftAmount > 0 && m_lifted.contains(c.id.value))
                        mesh.addRing(rect.translated(0, 2), m_style.clipRadius, 0, m_style.shadowSize * 1.5, liftShadow,
                                     transparent(liftShadow));
                    else if (c.id == m_hovered && m_hoverProgress > 0)
                        mesh.addRing(rect.translated(0, 1), m_style.clipRadius, 0, m_style.shadowSize, shadow,
                                     transparent(shadow));
                }
                if (m_selected.contains(c.id.value)) {
                    if (m_style.glowSize > 0)
                        mesh.addRing(rect, m_style.clipRadius, 0, m_style.glowSize, m_style.glowColor,
                                     transparent(m_style.glowColor));
                    // A brighter body (the label tint goes from ~12% to ~20%), the accent outline...
                    if (b.track && c.index < b.track->clips.size())
                        mesh.addRoundedRect(rect, m_style.clipRadius,
                                            withAlpha(m_style.clipColor(b.track->clips[c.index].colorIndex), m_style.selectedTint));
                    mesh.addOutline(rect, m_style.clipRadius, m_style.selectionGap, m_style.selectionWidth,
                                    m_style.selectionColor);
                    // ...and, shape not only colour, the slice notch: a 12° sliver in the top-left corner (v2; v3 sets
                    // notchSize 0: its outline is the ring around the solid clip).
                    if (m_style.notchSize > 0 && m_style.sliceAngle > 0 && rect.width() >= 16 && rect.height() >= 16) {
                        const double drop = std::min(rect.height() - 2,
                                                     m_style.notchSize / std::tan(m_style.sliceAngle * std::numbers::pi / 180.0));
                        const QPointF o = rect.topLeft() + QPointF(1, 1);
                        mesh.addTriangle(o, o + QPointF(m_style.notchSize, 0), o + QPointF(0, drop), m_style.selectionColor);
                    }
                    // The edge keyboard trims move: a bracket on that edge ("[" head, "]" tail).
                    if (!m_selectedEdge.isEmpty() && c.id == m_primary) {
                        const bool head = m_selectedEdge == QLatin1String("head");
                        const double x = head ? rect.left() : rect.right() - 4;
                        const auto bracket = [&](const QColor& color, double grow) {
                            mesh.addRect(QRectF(x - grow, rect.top() - grow, 4 + 2 * grow, rect.height() + 2 * grow), color);
                            const double tx = head ? x : x - 6;
                            mesh.addRect(QRectF(tx - grow, rect.top() - grow, 10 + 2 * grow, 3 + 2 * grow), color);
                            mesh.addRect(QRectF(tx - grow, rect.bottom() - 3 - grow, 10 + 2 * grow, 3 + 2 * grow), color);
                        };
                        bracket(m_style.markerInkColor, 1);
                        bracket(m_style.selectionColor, 0);
                    }
                }
                // 0.2 (TimelineViewFade.cpp): the fade handles of a selected clip or the one under the pointer.
                if (b.track && c.index < b.track->clips.size()
                    && fadeHandlesShown(b.track->clips[c.index], b.track->kind == TrackKind::Audio))
                    addFadeHandles(mesh, rect, b.track->clips[c.index]);
            }
        }
        // Trim edit points (M5i, TrimBridge): Premiere's brackets, red for roll and regular trims, yellow for ripple; a
        // roll covers both sides of the cut.
        if (!m_trimPoints.empty()) {
            const bool ripple = m_trimType == QLatin1String("ripple");
            const bool roll = m_trimType == QLatin1String("roll");
            const QColor color = ripple ? m_style.trimRippleColor : m_style.trimRollColor;
            for (const TrimPoint& point : m_trimPoints) {
                for (const TrackBucket& b : root->buckets) {
                    const auto c = std::find_if(b.clips.begin(), b.clips.end(),
                                                [&](const BuiltClip& bc) { return bc.id == point.clip; });
                    if (c == b.clips.end())
                        continue;
                    const double cut = point.head ? c->x0 : c->x1;
                    const auto bracket = [&](bool opensRight, const QColor& fill, double grow) {
                        const double x = opensRight ? cut : cut - 4;
                        mesh.addRect(QRectF(x - grow, c->y - grow, 4 + 2 * grow, c->h + 2 * grow), fill);
                        const double tx = opensRight ? x : x - 6;
                        mesh.addRect(QRectF(tx - grow, c->y - grow, 10 + 2 * grow, 3 + 2 * grow), fill);
                        mesh.addRect(QRectF(tx - grow, c->y + c->h - 3 - grow, 10 + 2 * grow, 3 + 2 * grow), fill);
                    };
                    for (const double grow : {1.0, 0.0}) {
                        const QColor fill = grow > 0 ? m_style.markerInkColor : color;
                        if (roll) {
                            bracket(false, fill, grow);
                            bracket(true, fill, grow);
                        } else {
                            bracket(point.head, fill, grow);
                        }
                    }
                    break;
                }
            }
        }
        // Transitions (M5d): a frame across the edit point with a bow tie meeting at the cut (a shape no clip has),
        // over the clips; the selected one gets the selection outline and glow. v3: a light frame with a dark bow tie
        // (transitionColor / transitionInk). They come from the shown track revision, so a transition-only change
        // needs no track rebuild.
        const QColor transitionFill = m_style.transitionColor;
        const QColor transitionWedge = withAlpha(m_style.transitionInk, 0.8);
        const QColor transitionEdge = withAlpha(m_style.transitionInk, 0.45);
        for (std::size_t i = 0; i < m_rows.size() && i < root->buckets.size(); ++i) {
            const Row& row = m_rows[i];
            if (!root->buckets[i].visible || row.track->transitions.empty())
                continue;
            for (const Transition& t : row.track->transitions) {
                const auto range = transitionRange(*row.track, t);
                if (!range || range->end() <= builtStart || range->start >= builtEnd)
                    continue;
                const double x0 = range->start.toSeconds() * m_pps - root->originX;
                const double x1 = range->end().toSeconds() * m_pps - root->originX;
                const double cut = (range->start + t.before).toSeconds() * m_pps - root->originX;
                const QRectF rect(x0, row.y + 3, std::max(4.0, x1 - x0), std::max(4.0, row.height - 6));
                const bool selected = t.id == m_selectedTransition;
                if (selected)
                    mesh.addRing(rect, m_style.clipRadius, 0, m_style.glowSize, m_style.glowColor,
                                 transparent(m_style.glowColor));
                mesh.addRoundedRect(rect, m_style.clipRadius, transitionFill, transitionEdge, 1.0);
                const double inset = std::min(4.0, rect.width() / 4);
                const double mid = rect.center().y();
                if (t.before > Ticks{})
                    mesh.addTriangle({rect.left() + inset, rect.top() + inset}, {rect.left() + inset, rect.bottom() - inset},
                                     {cut, mid}, transitionWedge);
                if (t.after > Ticks{})
                    mesh.addTriangle({rect.right() - inset, rect.top() + inset}, {rect.right() - inset, rect.bottom() - inset},
                                     {cut, mid}, transitionWedge);
                mesh.addRect(QRectF(cut - 0.5, rect.top() + 2, 1, rect.height() - 4), transitionEdge);
                if (selected)
                    mesh.addOutline(rect, m_style.clipRadius, m_style.selectionGap, m_style.selectionWidth,
                                    m_style.selectionColor);
            }
        }
        // Transcript highlights (M5e): selected words light their clips' time range (accent wash, edge lines); a
        // text edit's dry run shows what it would remove as a red ghost with a strike line across it.
        if (m_controller) {
            for (const TimelineHighlight& highlight : m_controller->timelineHighlights()) {
                const bool removal = highlight.kind == TimelineHighlight::Kind::Removal;
                const QColor base = removal ? m_style.removalColor : m_style.highlightColor;
                const double x0 = highlight.range.start.toSeconds() * m_pps - root->originX;
                const double x1 = std::max(x0 + 2.0, highlight.range.end().toSeconds() * m_pps - root->originX);
                if (highlight.range.end() <= builtStart || highlight.range.start >= builtEnd)
                    continue;
                for (const Row& row : m_rows) {
                    if (!highlight.tracks.empty() && std::find(highlight.tracks.begin(), highlight.tracks.end(), row.track->id) == highlight.tracks.end())
                        continue;
                    const QRectF rect(x0, row.y, x1 - x0, row.height);
                    mesh.addRect(rect, withAlpha(base, removal ? 0.42 : 0.26));
                    mesh.addRect(QRectF(rect.left(), rect.top(), rect.width(), 2), base);
                    mesh.addRect(QRectF(rect.left(), rect.bottom() - 2, rect.width(), 2), base);
                    if (removal)
                        mesh.addRect(QRectF(rect.left(), rect.center().y() - 1, rect.width(), 2), base);
                }
            }
            // 0.2 UX (stream T): the selected gap - an accent wash over the empty time on its track, outlined like a
            // selected clip, so Delete / Ripple Delete visibly act on it.
            if (const auto& gap = m_controller->gapSelection();
                gap && gap->range.end() > builtStart && gap->range.start < builtEnd) {
                for (const Row& row : m_rows) {
                    if (row.track->id != gap->track)
                        continue;
                    const double x0 = gap->range.start.toSeconds() * m_pps - root->originX;
                    const double x1 = std::max(x0 + 2.0, gap->range.end().toSeconds() * m_pps - root->originX);
                    const QRectF rect(x0, row.y + 3, x1 - x0, std::max(4.0, row.height - 6));
                    mesh.addRoundedRect(rect, m_style.clipRadius, withAlpha(m_style.selectionColor, 0.22));
                    mesh.addOutline(rect, m_style.clipRadius, m_style.selectionGap, m_style.selectionWidth,
                                    m_style.selectionColor);
                    break;
                }
            }
        }
        mesh.commit(root->overlay);
        root->selectionRevision = m_selectionRevision;
    }
    root->hovered = m_hovered;
    root->hoverProgress = m_hoverProgress;

    // ---- Lanes (view coordinates) ----
    if (full || root->lanesScrollY != m_scrollY || root->lanesWidth != w || root->lanesHeight != h) {
        MeshBuilder& mesh = root->mesh;
        mesh.clear();
        for (const Row& row : m_rows) {
            const double y = row.y - m_scrollY;
            if (y + row.height < 0 || y > h)
                continue;
            mesh.addRoundedRect(QRectF(0, y, w, row.height), m_style.laneRadius, m_style.laneColor);
        }
        mesh.commit(root->lanes);
        root->lanesScrollY = m_scrollY;
        root->lanesWidth = w;
        root->lanesHeight = h;
    }

    // ---- Playhead line (view coordinates; the triangle head lives in the QML ruler) ----
    {
        MeshBuilder& mesh = root->mesh;
        mesh.clear();
        const double x = playheadX();
        if (x > -m_style.playheadGlow && x < w + m_style.playheadGlow) {
            const double bottom = std::min(h, m_contentHeight - m_scrollY);
            // The UV beam (bottom = start, top = end); a soft glow around it only while playing.
            if (m_playing && m_style.playheadGlow > 0) {
                const double half = m_style.playheadGlow / 2 + m_style.playheadWidth / 2;
                mesh.addRect(QRectF(x - half, 0, 2 * half, bottom), withAlpha(m_style.beamMid, 0.10));
                mesh.addRect(QRectF(x - half / 2, 0, half, bottom), withAlpha(m_style.beamMid, 0.18));
            }
            addBeamLine(mesh, m_style, QPointF(x, bottom), QPointF(x, 0), m_style.playheadWidth, 24, 1.0);
        }
        mesh.commit(root->playhead);
    }

    // ---- Guides (view coordinates): ghost outlines, snap line, razor preview, cut flashes ----
    {
        MeshBuilder& mesh = root->mesh;
        mesh.clear();
        const double visibleHeight = std::min(h, m_contentHeight - m_scrollY);
        const auto rowRect = [&](int row, double x0, double x1) {
            const Row& r = m_rows[static_cast<std::size_t>(row)];
            return QRectF(x0, r.y - m_scrollY, x1 - x0, r.height);
        };
        // The snap line across the lanes with its tick: a 12° slash across the line's top.
        const auto addSnapLine = [&](Ticks at) {
            const double x = std::round(xOfTicks(at));
            mesh.addRect(QRectF(x - m_style.selectionWidth / 2, 0, m_style.selectionWidth, visibleHeight), m_style.snapColor);
            const double lean = 8 * std::tan(m_style.sliceAngle * std::numbers::pi / 180.0);
            const QPointF a(x - 3 - lean / 2, 8), b(x + 3 + lean / 2, 0);
            const QPointF n(1, 0);
            mesh.addTriangle(a - n, a + n, b + n, m_style.snapColor);
            mesh.addTriangle(a - n, b + n, b - n, m_style.snapColor);
        };
        if (m_gesture.active) {
            // Ghosts sit under the clips: only the vacated part shows. 0.4: a copy leaves the originals in place, so it
            // has none.
            MeshBuilder ghosts;
            for (const auto& ghost : m_gesture.ghosts) {
                if (m_gesture.spec.copy)
                    break;
                const auto row = std::ranges::find_if(m_rows, [&](const Row& r) { return r.track->id == ghost.track; });
                if (row == m_rows.end())
                    continue;
                const QRectF r = rowRect(static_cast<int>(row - m_rows.begin()), xOfTicks(ghost.start), xOfTicks(ghost.end) - 1);
                if (r.right() < 0 || r.left() > w)
                    continue;
                ghosts.addRoundedRect(r, m_style.clipRadius, withAlpha(m_style.ghostColor, 0.35), m_style.ghostColor, 1.0);
            }
            ghosts.commit(root->ghosts);
            root->ghostsShown = true;
            if (m_gesture.snappedAt && m_showSnapIndicator)
                addSnapLine(*m_gesture.snappedAt);
        } else if (m_playheadSnapAt && m_showSnapIndicator) {
            // 0.4: a ruler scrub snapped the playhead (until the ruler is released).
            addSnapLine(*m_playheadSnapAt);
        } else if (m_tool == TimelineTool::Razor && m_hoverInside && m_hoverRow >= 0
                   && m_hoverRow < static_cast<int>(m_rows.size())) {
            std::optional<Ticks> snapped;
            const double x = std::round(xOfTicks(snappedTimeAt(m_hoverX, &snapped)));
            const QColor color = snapped ? m_style.snapColor : m_style.razorColor;
            for (int row = 0; row < static_cast<int>(m_rows.size()); ++row) {
                if (m_hoverAllTracks || row == m_hoverRow)
                    mesh.addRect(rowRect(row, x - 0.5, x + 0.5), color);
            }
        }
        // 0.1.3: the clip edge a press would edit lights up like a trim edit point (red trim and roll, yellow ripple,
        // the accent for rate stretch and remix), with the edit cursor over it.
        if (!m_gesture.active && m_hoverEdge.kind != EdgeKind::None && m_hoverEdge.row >= 0
            && m_hoverEdge.row < static_cast<int>(m_rows.size())) {
            if (const Clip* clip = m_rows[static_cast<std::size_t>(m_hoverEdge.row)].track->findClip(m_hoverEdge.clip)) {
                const double cut = std::round(xOfTicks(m_hoverEdge.head ? clip->start() : clip->end()));
                const QRectF r = rowRect(m_hoverEdge.row, cut, cut).adjusted(0, 1, 0, -1);
                const QColor color = m_hoverEdge.kind == EdgeKind::Ripple ? m_style.trimRippleColor
                                     : m_hoverEdge.kind == EdgeKind::Rate || m_hoverEdge.kind == EdgeKind::Remix
                                         ? m_style.selectionColor
                                         : m_style.trimRollColor;
                // A bracket: a 3 px bar on the edge (inside the clip) with short serifs, on a dark rim.
                const auto bracket = [&](bool opensRight, const QColor& fill, double grow) {
                    const double x = opensRight ? cut : cut - 3;
                    mesh.addRect(QRectF(x - grow, r.top() - grow, 3 + 2 * grow, r.height() + 2 * grow), fill);
                    const double tx = opensRight ? x : x - 5;
                    mesh.addRect(QRectF(tx - grow, r.top() - grow, 8 + 2 * grow, 2 + 2 * grow), fill);
                    mesh.addRect(QRectF(tx - grow, r.bottom() - 2 - grow, 8 + 2 * grow, 2 + 2 * grow), fill);
                };
                for (const double grow : {1.0, 0.0}) {
                    const QColor fill = grow > 0 ? m_style.markerInkColor : color;
                    if (m_hoverEdge.kind == EdgeKind::Roll) {
                        bracket(false, fill, grow);
                        bracket(true, fill, grow);
                    } else {
                        bracket(m_hoverEdge.head, fill, grow);
                    }
                }
            }
        }
        // 0.1.3: where a drag from outside the lanes (Library, Source Monitor, a search hit, a template) would land:
        // an insertion line with caps on the row under the pointer (every row below the tracks), in the snap colour
        // with the snap tick when it snaps.
        if (m_dropIndicator.shown) {
            const double x = std::round(xOfTicks(m_dropIndicator.at));
            const QColor color = m_dropIndicator.snapped ? m_style.snapColor : m_style.razorColor;
            const bool onRow = m_dropIndicator.row >= 0 && m_dropIndicator.row < static_cast<int>(m_rows.size());
            const QRectF r = onRow ? rowRect(m_dropIndicator.row, x, x) : QRectF(x, 0, 0, visibleHeight);
            mesh.addRect(QRectF(x - 1, r.top(), 2, r.height()), color);
            mesh.addRect(QRectF(x - 4, r.top(), 8, 2), color);
            mesh.addRect(QRectF(x - 4, r.bottom() - 2, 8, 2), color);
            if (m_dropIndicator.snapped && m_showSnapIndicator) {
                // The snap tick: a short 12° slash across the line's top.
                const double lean = 8 * std::tan(m_style.sliceAngle * std::numbers::pi / 180.0);
                const QPointF a(x - 3 - lean / 2, r.top() + 8), b(x + 3 + lean / 2, r.top());
                const QPointF n(1, 0);
                mesh.addTriangle(a - n, a + n, b + n, m_style.snapColor);
                mesh.addTriangle(a - n, b + n, b - n, m_style.snapColor);
            }
        }
        if (!m_flashes.empty() && m_flashProgress < 1.0) {
            const double alpha = 1.0 - m_flashProgress;
            for (const Flash& f : m_flashes) {
                if (f.row < 0 || f.row >= static_cast<int>(m_rows.size()))
                    continue;
                // The cut spark: a 12° UV-beam slash across the edit point that fades out.
                const double x = std::round(xOfTicks(f.at));
                const QRectF r = rowRect(f.row, x, x);
                const double lean = r.height() * std::tan(m_style.sliceAngle * std::numbers::pi / 180.0);
                const QPointF bottom(x - lean / 2, r.bottom() + 2), top(x + lean / 2, r.top() - 2);
                addBeamLine(mesh, m_style, bottom, top, 6.0, 8, alpha * 0.25);
                addBeamLine(mesh, m_style, bottom, top, 2.0, 8, alpha);
            }
        }
        mesh.commit(root->guides);
        if (!m_gesture.active && root->ghostsShown) {
            mesh.clear();
            mesh.commit(root->ghosts);
            root->ghostsShown = false;
        }
    }

    // ---- Marquee ----
    if (full || root->marqueeRevision != m_marqueeRevision) {
        MeshBuilder& mesh = root->mesh;
        mesh.clear();
        if (m_marqueeActive) {
            mesh.addRect(m_marquee, m_style.marqueeFillColor);
            mesh.addOutline(m_marquee, 0, -1, 1, m_style.marqueeBorderColor, 0.5);
        }
        // M5m: the Remix tool's proposed length, from the clip's start to the dragged end.
        if (m_remix && m_remix->row >= 0 && static_cast<std::size_t>(m_remix->row) < m_rows.size()) {
            const Row& row = m_rows[static_cast<std::size_t>(m_remix->row)];
            const double x0 = xOfTicks(m_remix->start);
            const double x1 = xOfTicks(m_remix->target);
            const QRectF ghost(x0, row.y - m_scrollY, std::max(1.0, x1 - x0), row.height);
            mesh.addRect(ghost, m_style.marqueeFillColor);
            mesh.addOutline(ghost, 0, -1, 1, m_style.marqueeBorderColor, 1.0);
            mesh.addRect(QRectF(x1 - 1, row.y - m_scrollY, 2, row.height), m_style.marqueeBorderColor);
        }
        mesh.commit(root->marquee);
        root->marqueeRevision = m_marqueeRevision;
    }

    for (const TrackBucket& b : root->buckets) {
        stats.labels += static_cast<int>(b.labels.size());
        stats.nestedClips += b.nestedClips;
        stats.angleCuts += b.angleCuts;
        stats.trackLanes += b.trackLane ? 1 : 0;
        stats.responsiveClips += static_cast<int>(b.responsiveBands.size());
        for (const auto& [id, widths] : b.responsiveBands)
            stats.responsiveBands.insert(id, widths);
        for (const auto& [key, tag] : b.regionTags)
            stats.responsiveTags += tag.node ? 1 : 0;
        for (const auto& [id, label] : b.labels) {
            stats.labelNodes += countNodes(label.node);
            if (label.badge) {
                ++stats.generatedBadges;
                stats.badgeTexts.insert(id, label.badgeText);
            }
            if (label.fx) // 0.4
                stats.fxBadges.insert(id, label.fxState == ClipFx::Effects ? QStringLiteral("effects") : QStringLiteral("changed"));
            stats.labelTexts.insert(id, label.text);
        }
    }
    if (!m_lifted.empty()) {
        for (const TrackBucket& b : root->buckets) {
            for (const BuiltClip& c : b.clips) {
                if (m_lifted.contains(c.id.value))
                    stats.liftedRects.insert(c.id.value, QRectF(c.x0 + root->originX - m_scrollX, c.y - m_scrollY, c.x1 - c.x0, c.h));
            }
        }
    }
    stats.nodes = countNodes(root);
    stats.labelsPending = ctx.labelsPending;
    stats.updateNs = timer.nsecsElapsed();
    m_stats = stats;
    if (ctx.labelsPending) // finish the deferred labels in the next frame
        QMetaObject::invokeMethod(this, &QQuickItem::update, Qt::QueuedConnection);
    return root;
}

} // namespace wabcut::app
