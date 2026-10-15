// I.6 in the UI: the Masks bridge through the real shell (offscreen, nothing shown) - the monitor tools (ellipse,
// rectangle, pen), the handles (one undo step per drag), the selected mask's values, a local correction on an effect,
// tracking as a job (synchronous here, over synthetic moving pictures), the Source Monitor's redaction tool and list
// (also on display-rotated and anamorphic media, checked against what playback renders), "don't redact here" on the
// selected clip; and screenshots of Effect Controls' mask section and the monitor overlay.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "FilmstripSource.h"
#include "MediaBridge.h"
#include "MaskBridge.h"
#include "ProjectController.h"
#include "RedactedPictures.h"
#include "SourceMonitorBridge.h"
#include "SyntheticMotion.h"
#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/Mask.h>
#include <wabcut/render/PreparedComposite.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QQuickItem>

#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

/// Pictures of the fixture's media (1920x1080, 25 fps) at most 320 wide; the content moves (0.8, -0.3) px per frame.
class FakePictures final : public masks::TrackingServices {
public:
    std::function<void()> onProgress;
    void progress(const masks::TrackProgress&) override {
        if (onProgress) {
            auto callback = std::move(onProgress);
            callback();
        }
    }
    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                     int maxWidth) override {
        const VideoStreamInfo& v = state.findMedia(media)->info.video.at(static_cast<std::size_t>(stream));
        if (time.value < 0 || time >= v.duration) {
            return std::unexpected(Error{ErrorCode::NotFound, "past the end"});
        }
        const std::int64_t k = ticksToFrames(time, v.frameRate);
        const int w = std::min(maxWidth, 320);
        masks::TrackingFrame f;
        f.luma = masks::test::render(m_texture, w, w * v.height / v.width,
                                     masks::Motion{1, 0, -0.8 * static_cast<double>(k), 0, 1, 0.3 * static_cast<double>(k)});
        f.start = framesToTicks(k, v.frameRate);
        f.duration = framesToTicks(1, v.frameRate);
        return f;
    }

private:
    masks::test::Texture m_texture{23};
};

constexpr const char* kPicture = "V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\n"
                                 "A1 |Aaaaaaaaaaaaaaaaaaaaaaaa|";

QPointF handleOf(const MaskBridge& masks, const QString& kind, int index = 0) {
    for (const QVariant& h : masks.handles()) {
        const QVariantMap m = h.toMap();
        if (m.value(QStringLiteral("kind")).toString() == kind && m.value(QStringLiteral("index")).toInt() == index) {
            return {m.value(QStringLiteral("x")).toDouble(), m.value(QStringLiteral("y")).toDouble()};
        }
    }
    FAIL("no " << kind.toStdString() << " handle");
    return {};
}

int handleCount(const MaskBridge& masks, const QString& kind) {
    int n = 0;
    for (const QVariant& h : masks.handles()) {
        n += h.toMap().value(QStringLiteral("kind")).toString() == kind ? 1 : 0;
    }
    return n;
}

struct MaskShell {
    Shell shell;
    ProjectController* controller = nullptr;
    MaskBridge* masks = nullptr;
    SourceMonitorBridge* source = nullptr;
    Timeline tl{kPicture};
    ClipId clip;

    MaskShell() {
        resetSettings();
        clearMessages();
        REQUIRE(shell.window != nullptr);
        shell.theme->setProperty("reduceMotion", true);
        shell.resize(kShotWidth, kShotHeight);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        masks = shell.engine.singletonInstance<MaskBridge*>("Wabcut", "Masks");
        source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
        REQUIRE(controller);
        REQUIRE(masks);
        REQUIRE(source);
        tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
        controller->selectClipById(clip, ProjectController::Replace);
        controller->setPlayhead(Ticks{});
        REQUIRE(waitFor([&] { return masks->clip() == clipKey(clip); }));
    }
    const Mask& mask(int index = 0) const {
        const Clip& c = controller->currentSequence()->findClip(clip)->clip();
        for (const EffectInstance& e : c.effects()) {
            if (e.effect == "wabcut.mask") {
                return e.masks.at(static_cast<std::size_t>(index));
            }
        }
        FAIL("no opacity mask");
        throw std::logic_error("unreachable");
    }
};

} // namespace

TEST_CASE("Masks UI: tools, handles, values, a local correction, the pen and tracking", "[app][masks]") {
    MaskShell m;
    MaskBridge& masks = *m.masks;
    ProjectController& c = *m.controller;

    // Ellipse tool: a drag on the monitor (the clip fills the canvas) is one mask on the clip's opacity mask.
    masks.setTool(QStringLiteral("ellipse"));
    REQUIRE(masks.drawShape(QStringLiteral("ellipse"), 0.3, 0.25, 0.7, 0.75));
    CHECK(masks.tool().isEmpty());
    REQUIRE(masks.masks().size() == 1);
    CHECK(c.undoText() == QStringLiteral("Undo Draw Ellipse Mask"));
    const QString ellipse = masks.selected();
    CHECK_FALSE(ellipse.isEmpty());
    REQUIRE(masks.outlines().size() == 1);
    CHECK(masks.outlines()[0].toMap().value(QStringLiteral("points")).toList().size() >= 32);
    CHECK(handleOf(masks, QStringLiteral("center")).x() == Approx(0.5).margin(1e-6));
    CHECK(handleOf(masks, QStringLiteral("center")).y() == Approx(0.5).margin(1e-6));
    CHECK(maskValue(m.mask(), "size.x", Ticks{}) == Approx(0.4).margin(1e-6));
    CHECK(masks.maskAt(0.5, 0.5) == ellipse);
    CHECK(masks.maskAt(0.05, 0.05).isEmpty());

    // Dragging the centre handle: one undo step for the whole drag.
    masks.beginDrag(QStringLiteral("center"), 0, 0.5, 0.5);
    masks.drag(0.55, 0.5);
    masks.drag(0.6, 0.45);
    masks.endDrag();
    CHECK(handleOf(masks, QStringLiteral("center")).x() == Approx(0.6).margin(1e-6));
    CHECK(maskValue(m.mask(), "center.y", Ticks{}) == Approx(0.45).margin(1e-6));
    CHECK(c.undoText() == QStringLiteral("Undo Edit Mask"));
    c.undo();
    CHECK(maskValue(m.mask(), "center.x", Ticks{}) == Approx(0.5).margin(1e-6));
    // The corner handle sizes it, the feather handle feathers it.
    const QPointF corner = handleOf(masks, QStringLiteral("corner"));
    masks.beginDrag(QStringLiteral("corner"), 0, corner.x(), corner.y());
    masks.drag(corner.x() + 0.05, corner.y());
    masks.endDrag();
    CHECK(maskValue(m.mask(), "size.x", Ticks{}) == Approx(0.5).margin(1e-6));

    // Values of the selected mask: a slider gesture is one undo step; the feather band shows.
    REQUIRE(masks.setValue(QStringLiteral("feather"), 0.05));
    REQUIRE(masks.setValue(QStringLiteral("feather"), 0.08));
    masks.endEdit();
    CHECK(masks.current().value(QStringLiteral("feather")).toDouble() == Approx(0.08));
    CHECK_FALSE(masks.featherOuter().isEmpty());
    CHECK_FALSE(masks.featherInner().isEmpty());
    c.undo();
    CHECK(masks.current().value(QStringLiteral("feather")).toDouble() == Approx(0.0));
    REQUIRE(masks.setInverted(ellipse, true));
    REQUIRE(masks.setMode(ellipse, QStringLiteral("intersect")));
    CHECK(m.mask().inverted);
    CHECK(m.mask().mode == MaskMode::Intersect);

    // A local correction: masks on Basic Correction (the effect then works only inside).
    REQUIRE(c.applyEditFn("Add Effect", [&](const ProjectState& s) {
        const std::vector<ClipId> clips{m.clip};
        return edit::addEffect(s, m.tl.seq, clips, std::string("wabcut.color.basic"));
    }));
    REQUIRE(masks.targets().size() == 2);
    const QString basic = masks.targets()[1].toMap().value(QStringLiteral("effect")).toString();
    masks.setTarget(basic);
    CHECK(masks.masks().isEmpty());
    REQUIRE(masks.addMask(QStringLiteral("rectangle")));
    REQUIRE(masks.masks().size() == 1);
    CHECK(masks.selected().startsWith(basic + QLatin1Char('/')));
    const EffectInstance& fx = m.controller->currentSequence()->findClip(m.clip)->clip().effects().back();
    CHECK(fx.effect == "wabcut.color.basic");
    CHECK(fx.masks.size() == 1);

    // The pen: three points (one pulled into a curve), closed into a path mask on the opacity mask.
    masks.setTarget(QString());
    masks.setTool(QStringLiteral("pen"));
    masks.penPoint(0.2, 0.2);
    masks.penPoint(0.45, 0.2);
    masks.penTangent(0.5, 0.3);
    CHECK(masks.penPoints().size() == 2);
    CHECK_FALSE(masks.penClose()); // too few
    masks.penPoint(0.3, 0.45);
    REQUIRE(masks.penClose());
    REQUIRE(masks.masks().size() == 2);
    CHECK(m.mask(1).shape == MaskShape::Path);
    CHECK(handleCount(masks, QStringLiteral("vertex")) == 3);
    CHECK(handleCount(masks, QStringLiteral("in")) == 1);
    CHECK(handleCount(masks, QStringLiteral("out")) == 1);
    const QPointF v1 = handleOf(masks, QStringLiteral("vertex"), 1);
    masks.beginDrag(QStringLiteral("vertex"), 1, v1.x(), v1.y());
    masks.drag(v1.x() + 0.1, v1.y());
    masks.endDrag();
    CHECK(m.mask(1).path.front().vertices[1].x == Approx(0.55).margin(1e-6));

    // Tracking (a job; synchronous here): five frames forward from the playhead, the confidence per frame, keys on the
    // mask's transform, one undo step, and the playhead on the last frame tracked.
    masks.setSynchronous(true);
    masks.setTrackingServices(std::make_shared<FakePictures>());
    masks.select(ellipse);
    REQUIRE(masks.track(QStringLiteral("forward"), 5));
    CHECK_FALSE(masks.tracking());
    CHECK(masks.confidence().size() == 5);
    CHECK(masks.trackStatus() == QStringLiteral("Tracked 5 frame(s)"));
    CHECK(c.undoText() == QStringLiteral("Undo Track Mask"));
    CHECK(masks.masks()[0].toMap().value(QStringLiteral("tracked")).toBool());
    CHECK(c.playhead() == framesToTicks(5, kRate25));
    CHECK(maskValue(m.mask(), "transform.x", framesToTicks(5, kRate25)) == Approx(5 * 0.8 / 320).margin(0.001));
    for (const QVariant& v : masks.confidence()) {
        CHECK(v.toDouble() > 0.8);
    }
    // Removing a mask.
    REQUIRE(masks.removeMask(ellipse));
    CHECK(masks.masks().size() == 1);
    CHECK(masks.selected().isEmpty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(std::none_of(messages.begin(), messages.end(), [](const QString& line) {
        return line.contains(QStringLiteral("masks/")) || line.contains(QStringLiteral("Masks"));
    }));
}

TEST_CASE("Masks UI: a tracking result cannot overwrite an edit made while it ran", "[app][masks][tracking]") {
    MaskShell m;
    MaskBridge& masks = *m.masks;
    REQUIRE(masks.drawShape(QStringLiteral("ellipse"), 0.3, 0.2, 0.62, 0.78));
    masks.setSynchronous(true);
    auto pictures = std::make_shared<FakePictures>();
    pictures->onProgress = [&] {
        REQUIRE(masks.setValue(QStringLiteral("feather"), 0.12));
        masks.endEdit();
    };
    masks.setTrackingServices(pictures);
    REQUIRE(masks.track(QStringLiteral("forward"), 2));
    CHECK_FALSE(masks.tracking());
    CHECK(maskValue(m.mask(), "feather", Ticks{}) == Approx(0.12));
    CHECK(m.mask().find("transform.x") == nullptr);
    CHECK(masks.trackStatus().contains(QStringLiteral("changed")));
    CHECK(m.controller->undoText() != QStringLiteral("Undo Track Mask"));
}

TEST_CASE("Masks UI: the Source Monitor's redaction tool marks the source for every use", "[app][masks][redaction]") {
    MaskShell m;
    MaskBridge& masks = *m.masks;
    REQUIRE(m.source->load(mediaKey(m.tl.media('A'))));
    REQUIRE(waitFor([&] { return m.source->mediaId() == m.tl.media('A'); }));
    masks.setRedactStyle(QStringLiteral("mosaic"));
    masks.setRedactAudio(QStringLiteral("bleep"));
    masks.setRedactTool(true);
    REQUIRE(masks.addRedaction(0.1, 0.1, 0.4, 0.3));
    CHECK_FALSE(masks.redactTool());
    REQUIRE(masks.redactions().size() == 1);
    const QVariantMap r = masks.redactions()[0].toMap();
    CHECK(r.value(QStringLiteral("style")).toString() == QStringLiteral("mosaic"));
    CHECK(r.value(QStringLiteral("audio")).toString() == QStringLiteral("bleep"));
    CHECK(r.value(QStringLiteral("start")).toDouble() == Approx(0.0));
    CHECK(r.value(QStringLiteral("end")).toDouble() == Approx(1.0)); // no marks: the whole media
    REQUIRE(masks.activeRegions().size() == 1);
    const ProjectSnapshot snap = m.controller->snapshot();
    REQUIRE(snap->redactions.size() == 1);
    CHECK(maskValue(snap->redactions[0].region, "center.x", Ticks{}) == Approx(0.25));
    CHECK(snap->redactions[0].appliesTo(m.clip)); // the program's clip of that media is redacted
    CHECK(m.controller->undoText() == QStringLiteral("Undo Add Redaction"));

    // "Don't redact here" on the selected clip excepts it and its linked audio.
    const QString key = r.value(QStringLiteral("key")).toString();
    REQUIRE(masks.setRedactionOverride(key, false));
    CHECK_FALSE(m.controller->snapshot()->redactions[0].appliesTo(m.clip));
    CHECK(m.controller->snapshot()->redactions[0].exceptions.size() == 2);
    REQUIRE(masks.setRedactionStyle(key, QStringLiteral("solid")));
    REQUIRE(masks.setRedactionEnabled(key, false));
    CHECK(masks.activeRegions().isEmpty()); // off: nothing covered
    REQUIRE(masks.removeRedaction(key));
    CHECK(masks.redactions().isEmpty());
}

// Codex wave-4 P1-2: the Source Monitor shows a phone clip rotated and an HDV clip stretched; a box dragged over the
// picture must be redacted where it was drawn, in what playback renders (the private clip's evaluated layer and the
// redaction's coverage, the CPU reference of the compositor's), and its outline must be drawn there too.
TEST_CASE("Masks UI: a Source Monitor redaction covers the dragged area of rotated and anamorphic media",
          "[app][masks][redaction]") {
    struct Case {
        const char* name;
        int width;
        int height;
        int rotation;
        Rate pixelAspect;
        double x0, y0, x1, y1; ///< the drag, fractions of the displayed picture (the Source Monitor's canvas)
    };
    // Off-centre boxes inside the displayed picture: a wrong mapping moves them off what was marked.
    const Case cases[] = {
        {"square pixels, unrotated", 1920, 1080, 0, Rate{1, 1}, 0.62, 0.20, 0.78, 0.38},
        {"phone, rotated 90", 1920, 1080, 90, Rate{1, 1}, 0.42, 0.12, 0.54, 0.30},
        {"phone, rotated 180", 1920, 1080, 180, Rate{1, 1}, 0.62, 0.20, 0.78, 0.38},
        {"phone, rotated 270", 1920, 1080, 270, Rate{1, 1}, 0.42, 0.12, 0.54, 0.30},
        {"HDV 1440x1080, 4:3 pixels", 1440, 1080, 0, Rate{4, 3}, 0.80, 0.30, 0.95, 0.50},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            MaskShell m;
            MaskBridge& masks = *m.masks;
            ProjectState state = m.tl.state;
            for (MediaPtr& item : state.media) {
                if (item && item->id == m.tl.media('A')) {
                    auto copy = std::make_shared<MediaItem>(*item);
                    VideoStreamInfo& v = copy->info.video.front();
                    v.width = c.width;
                    v.height = c.height;
                    v.rotationDegrees = c.rotation;
                    v.sampleAspect = c.pixelAspect;
                    item = std::move(copy);
                }
            }
            m.controller->resetProject(state, m.tl.seq);
            REQUIRE(m.source->load(mediaKey(m.tl.media('A'))));
            REQUIRE(waitFor([&] { return m.source->mediaId() == m.tl.media('A'); }));
            masks.setRedactStyle(QStringLiteral("solid"));
            masks.setRedactTool(true);
            REQUIRE(masks.addRedaction(c.x0, c.y0, c.x1, c.y1));
            REQUIRE(m.controller->snapshot()->redactions.size() == 1);

            // What playback renders in the Source Monitor: its private clip's layer with the redaction.
            const ProjectSnapshot source = m.source->sourceProject();
            REQUIRE(source);
            REQUIRE(source->redactions.size() == 1);
            const Sequence& seq = *source->sequences.front();
            std::vector<playback::LayerSpec> layers;
            playback::evaluateVideoLayers(*source, seq, m.source->positionTicks(), layers, false);
            REQUIRE(layers.size() == 1);
            const playback::LayerSpec& layer = layers.front();
            REQUIRE(layer.redactions.size() == 1);
            REQUIRE(layer.redactions.front().mask);
            const VideoStreamInfo& v = layer.item->info.video.front();
            const render::Affine2D toFrame =
                render::layerToCanvas(layer.transform, v.width, v.height, v.sampleAspect.toDouble(), {seq.width, seq.height})
                    .inverted();
            constexpr int kTexels = 256;
            const std::vector<float> coverage = render::maskCoverageCpu(*layer.redactions.front().mask, kTexels, kTexels);
            // Coverage of the redaction at a point of the displayed picture (canvas fractions); -1 off the frame.
            const auto coveredAt = [&](double fx, double fy) {
                const render::Vec2 p = toFrame.map({fx * seq.width, fy * seq.height});
                const double u = p.x / v.width, w = p.y / v.height;
                if (u < 0.0 || u >= 1.0 || w < 0.0 || w >= 1.0) {
                    return -1.0;
                }
                const auto col = static_cast<std::size_t>(u * kTexels), row = static_cast<std::size_t>(w * kTexels);
                return static_cast<double>(coverage[row * kTexels + col]);
            };
            const double cx = (c.x0 + c.x1) / 2.0, cy = (c.y0 + c.y1) / 2.0;
            INFO("drag centre (" << cx << ", " << cy << ")");
            CHECK(coveredAt(cx, cy) > 0.99);
            // Just inside each edge of the drag is covered; well outside it is not.
            const double insetX = (c.x1 - c.x0) * 0.15, insetY = (c.y1 - c.y0) * 0.15;
            CHECK(coveredAt(c.x0 + insetX, cy) > 0.99);
            CHECK(coveredAt(c.x1 - insetX, cy) > 0.99);
            CHECK(coveredAt(cx, c.y0 + insetY) > 0.99);
            CHECK(coveredAt(cx, c.y1 - insetY) > 0.99);
            CHECK(coveredAt(c.x0 - 0.06, cy) == Approx(0.0).margin(0.01));
            CHECK(coveredAt(cx, std::min(c.y1 + 0.08, 0.99)) == Approx(0.0).margin(0.01));

            // The dashed outline is drawn where the drag was.
            REQUIRE(masks.activeRegions().size() == 1);
            double left = 1.0, right = 0.0, top = 1.0, bottom = 0.0;
            for (const QVariant& point : masks.activeRegions()[0].toMap().value(QStringLiteral("points")).toList()) {
                const QPointF q = point.toPointF();
                left = std::min(left, q.x());
                right = std::max(right, q.x());
                top = std::min(top, q.y());
                bottom = std::max(bottom, q.y());
            }
            CHECK(left == Approx(c.x0).margin(0.002));
            CHECK(right == Approx(c.x1).margin(0.002));
            CHECK(top == Approx(c.y0).margin(0.002));
            CHECK(bottom == Approx(c.y1).margin(0.002));
        }
    }
}

// Codex wave-4 P2-38: the Source Monitor copies the project's redactions of its media into its private one-clip project,
// whose ids start at 1. Copies keeping the project's redaction ids collided with the private media, sequence, track
// or clip ids; a debug build's edit validation then rejected the commit and the source played unredacted. The private
// project must stay valid and carry every redaction, also after more are added and one is removed. Failed before the
// fix (duplicate ids).
TEST_CASE("Masks UI: the Source Monitor's private copy of the redactions keeps its ids unique", "[app][masks][redaction]") {
    MaskShell m;
    MaskBridge& masks = *m.masks;
    // A project as small as a new one: one media item and a sequence; its redactions get the next ids, 9 to 12, which
    // are also the ids the Source Monitor's private project gives its own clip and link group.
    MediaItem item = *m.tl.state.findMedia(m.tl.media('A'));
    auto withMedia = edit::addMedia(ProjectState{}, item);
    REQUIRE(withMedia);
    auto withSequence = edit::addSequence(withMedia->state, edit::SequenceSettings{});
    REQUIRE(withSequence);
    ProjectState state = withSequence->state;
    state.ids.next = std::max<std::uint64_t>(state.ids.next, 9);
    for (int i = 0; i < 4; ++i) {
        Redaction r;
        r.media = withMedia->id;
        r.source = TimeRange::fromStartEnd(Ticks{}, item.info.duration);
        r.region.shape = MaskShape::Rectangle;
        r.region.set("center.x", 0.2 + 0.2 * i);
        r.region.set("size.x", 0.1);
        auto added = edit::addRedaction(state, r);
        REQUIRE(added);
        state = std::move(added->state);
    }
    m.controller->resetProject(state, withSequence->id);
    REQUIRE(m.source->load(mediaKey(withMedia->id)));
    REQUIRE(waitFor([&] { return m.source->mediaId() == withMedia->id; }));
    const auto check = [&](std::size_t count) {
        REQUIRE(waitFor([&] { return m.source->sourceProject() && m.source->sourceProject()->redactions.size() == count; }));
        const ProjectSnapshot source = m.source->sourceProject();
        const auto valid = validate(*source, Validation::Strict);
        INFO((valid ? std::string() : valid.error().message));
        CHECK(valid.has_value());
        std::vector<playback::LayerSpec> layers;
        playback::evaluateVideoLayers(*source, *source->sequences.front(), m.source->positionTicks(), layers, false);
        REQUIRE(layers.size() == 1);
        CHECK(layers.front().redactions.size() == count); // every one applies to the private clip
    };
    check(4);
    REQUIRE(masks.removeRedaction(masks.redactions()[1].toMap().value(QStringLiteral("key")).toString()));
    check(3);
}

// Codex wave-4 P2-9: thumbnails, filmstrips and smart-search frames are decoded straight from the file, so they get the
// source's redactions on the CPU: in the right place on display-rotated media (a phone clip's thumbnail is portrait),
// with the style; and their cache keys (the thumbnail URL, the filmstrip's per-media key) change with the redactions,
// so a new redaction re-renders them. Before the fix nothing redacted these pictures (the review's finding); the seam
// this test drives is new, so it has no pre-fix run.
TEST_CASE("Masks UI: thumbnails, filmstrips and search frames show the source's redactions", "[app][masks][redaction]") {
    Timeline tl("V1 |Aaaaaaaaaa|");
    ProjectState state = tl.state;
    const MediaId media = tl.media('A');
    for (MediaPtr& item : state.media) {
        if (item && item->id == media) {
            auto copy = std::make_shared<MediaItem>(*item);
            copy->info.video.front().width = 1920;
            copy->info.video.front().height = 1080;
            copy->info.video.front().rotationDegrees = 90; // shown portrait
            copy->locator.path = "C:/footage/phone.mp4";
            item = std::move(copy);
        }
    }
    Redaction r;
    r.media = media;
    r.source = TimeRange::fromStartEnd(Ticks{}, Ticks::fromSeconds(1));
    r.style = RedactionStyle::Solid;
    r.colorR = 1.0;
    r.region.shape = MaskShape::Rectangle;
    r.region.set("center.x", 0.25); // stored-frame fractions: left quarter, middle
    r.region.set("center.y", 0.5);
    r.region.set("size.x", 0.2);
    r.region.set("size.y", 0.3);
    auto added = edit::addRedaction(state, r);
    REQUIRE(added);
    const ProjectState redacted = added->state;

    // A 54 x 96 (portrait) thumbnail: stored pixel (u, v) of the 96 x 54 frame shows at display (53 - v, u).
    QImage picture(54, 96, QImage::Format_RGBA8888);
    picture.fill(QColor(128, 128, 128));
    QImage clear = picture;
    CHECK(redacted::apply(picture, redacted, media, Ticks::fromSeconds(0)));
    const auto at = [&](int u, int v) { return picture.pixelColor(53 - v, u); };
    CHECK(at(24, 27) == QColor(255, 0, 0));      // the region's centre (0.25 x 96, 0.5 x 54)
    CHECK(at(72, 27) == QColor(128, 128, 128));  // the right of the stored frame: clear
    CHECK(at(24, 5) == QColor(128, 128, 128));   // above the region: clear
    CHECK_FALSE(redacted::apply(clear, redacted, media, Ticks::fromSeconds(3))); // after the redacted range
    CHECK_FALSE(redacted::apply(clear, state, media, Ticks{}));                 // a project without it

    // The library thumbnail's URL and the filmstrip key carry the redactions.
    redacted::publish(std::make_shared<const ProjectState>(state));
    const QString plainUrl = ThumbnailImageProvider::urlFor("C:/footage/phone.mp4");
    CHECK(redacted::keyOfFile("C:/footage/phone.mp4") == 0);
    redacted::publish(std::make_shared<const ProjectState>(redacted));
    const QString redactedUrl = ThumbnailImageProvider::urlFor("C:/footage/phone.mp4");
    CHECK(redactedUrl != plainUrl);
    CHECK(redactedUrl.startsWith(plainUrl + QStringLiteral("/r")));
    QImage viaFile(54, 96, QImage::Format_RGBA8888);
    viaFile.fill(QColor(128, 128, 128));
    CHECK(redacted::applyToFile(viaFile, "C:/footage/phone.mp4", Ticks{}));
    CHECK(viaFile.pixelColor(53 - 27, 24) == QColor(255, 0, 0));

    // A filmstrip already extracted is dropped when the media's redactions change, then extracted (redacted) again.
    int requests = 0;
    MediaFilmstripSource strips([&](const std::string&, int count, int height, MediaFilmstripSource::Done done) {
        ++requests;
        MediaFilmstripSource::Frames frames;
        for (int i = 0; i < count; ++i) {
            QImage image(height * 9 / 16, height, QImage::Format_RGBA8888);
            image.fill(Qt::gray);
            frames.images.push_back(image);
            frames.times.push_back(Ticks{i});
        }
        done(std::move(frames));
    });
    strips.setMediaFiles({{media.value, {"C:/footage/phone.mp4", 10.0, redacted::keyOf(state, media)}}});
    (void)strips.lookup(media);
    REQUIRE(waitFor([&] { return strips.lookup(media).state == FilmstripSource::State::Ready; }));
    CHECK(requests == 1);
    strips.setMediaFiles({{media.value, {"C:/footage/phone.mp4", 10.0, redacted::keyOf(redacted, media)}}});
    // The strip extracted before is gone: the next lookup extracts it again (this requester answers at once).
    REQUIRE(waitFor([&] { return strips.lookup(media).state == FilmstripSource::State::Ready; }));
    CHECK(requests == 2);
    redacted::publish(nullptr);
}

TEST_CASE("Masks UI: Effect Controls' mask section and the monitor overlay (screenshots)", "[app][masks][screenshots]") {
    MaskShell m;
    MaskBridge& masks = *m.masks;
    REQUIRE(masks.drawShape(QStringLiteral("ellipse"), 0.3, 0.2, 0.62, 0.78));
    REQUIRE(masks.setValue(QStringLiteral("feather"), 0.06));
    masks.endEdit();
    masks.setTool(QStringLiteral("pen"));
    masks.penPoint(0.68, 0.3);
    masks.penPoint(0.85, 0.35);
    masks.penTangent(0.9, 0.45);
    masks.penPoint(0.75, 0.62);
    REQUIRE(masks.penClose());
    masks.setSynchronous(true);
    masks.setTrackingServices(std::make_shared<FakePictures>());
    REQUIRE(masks.track(QStringLiteral("forward"), 12));
    spin(100);
    const QImage image = m.shell.grab();
    REQUIRE_FALSE(image.isNull());
    image.save(outputDir() + QStringLiteral("/masks-ui.png"));
    const double dpr = image.devicePixelRatio();
    for (const auto& [name, file] : {std::pair{"effectControls", "masks-effect-controls.png"}, std::pair{"programFrameArea", "masks-monitor-overlay.png"}}) {
        QQuickItem* item = m.shell.item(name);
        if (!item) {
            item = qobject_cast<QQuickItem*>(m.shell.window->contentItem()->findChild<QObject*>(QLatin1String(name)));
        }
        REQUIRE(item != nullptr);
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
            .save(outputDir() + QStringLiteral("/") + QLatin1String(file));
    }
    const std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == QLatin1String("maskSection"))
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = find(child))
                return found;
        }
        return nullptr;
    };
    QQuickItem* section = find(m.shell.window->contentItem());
    REQUIRE(section != nullptr);
    CHECK(section->isVisible());
    CHECK(section->height() > 100);
}

// AI-14 review, finding 3: thumbnails and filmstrips of a matte redaction were keyed by the matte track's name only;
// refining the track keeps its name, so the old (possibly under-redacted) pictures stayed. The key now carries the
// matte source's revision of the track's content.
TEST_CASE("Masks UI: thumbnails of a matte redaction are redone when the matte track content changes", "[app][masks][redaction]") {
    struct Versioned final : public render::MatteSource {
        std::shared_ptr<const render::MatteField> fieldAt(const MaskMatte&, Ticks, double, render::MatteUse) override { return nullptr; }
        std::uint64_t revision(const MaskMatte&) override { return version; }
        std::uint64_t version = 11;
    };
    auto source = std::make_shared<Versioned>();
    render::setMatteSource(source);
    Timeline tl("V1 |Aaaaaaaaaa|");
    const MediaId media = tl.media('A');
    Redaction r;
    r.media = media;
    r.source = TimeRange::fromStartEnd(Ticks{}, Ticks::fromSeconds(1));
    r.region.shape = MaskShape::Matte;
    r.region.matte = MaskMatte{"0000000000001234/0/red-v1/0123456789abcdef", media, 0, r.source, "red-v1"};
    r.region.set("center.x", 0.5);
    r.region.set("center.y", 0.5);
    r.region.set("size.x", 0.2);
    r.region.set("size.y", 0.3);
    auto added = edit::addRedaction(tl.state, r);
    REQUIRE(added);
    const std::uint64_t first = redacted::keyOf(added->state, media);
    CHECK(first != 0);
    CHECK(redacted::keyOf(added->state, media) == first); // stable while nothing changes
    source->version = 12;                                 // the track was refined: same name, new mattes
    CHECK(redacted::keyOf(added->state, media) != first);
    render::setMatteSource(nullptr);
}
