// AI-14 in the app, on the UI shell with a scripted model and video (the red object of tests/ai/segment; no model, no
// media decoding): the Object tool of Effect Controls' mask section - a click on the program monitor makes an object
// mask on the clip (one undo step), a negative click refines it, Track both ways follows it to the shot's cut with a
// per-frame confidence, the matte preview over the monitor - and the Source Monitor's "Track an object" redaction, which
// fails closed in thumbnails and filmstrips (RedactedPictures) wherever a frame has no matte. The Object tool's QML
// hooks (MaskSection, MonitorPanel, the redaction overlay and popover) are applied for these runs. Screenshots:
// <build>/test-output/ui/object-mask-ui.png, object-mask-effect-controls.png and object-mask-monitor.png.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "MaskBridge.h"
#include "ObjectMaskBridge.h"
#include "ProjectController.h"
#include "RedactedPictures.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"
#include "segment/SegmentTestUtil.h"

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/segment/MatteCache.h>
#include <wabcut/render/Matte.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QQmlComponent>
#include <QQuickItem>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::ai::segment::test;
namespace seg = wabcut::ai::segment;
using wabcut::ai::segment::MatteBox;
using Catch::Approx;

namespace {

/// An item of the visual tree by objectName (QML items are not always QObject children of their visual parent).
QQuickItem* findItem(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) {
        return item;
    }
    for (QQuickItem* child : item->childItems()) {
        if (QQuickItem* found = findItem(child, name)) {
            return found;
        }
    }
    return nullptr;
}

/// The Object tool's QML where the hooks put it (Effect Controls' mask section, the program monitor); without the
/// hooks (they are the integration owner's patch), made from the module and laid over `host` the same way.
QQuickItem* placed(Shell& shell, const char* type, const QString& objectName, QQuickItem* host, bool fill) {
    if (QQuickItem* hooked = findItem(shell.window->contentItem(), objectName)) {
        return hooked;
    }
    REQUIRE(host != nullptr);
    QQmlComponent component(&shell.engine);
    component.loadFromModule("Wabcut", type);
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto* item = qobject_cast<QQuickItem*>(component.create());
    REQUIRE(item != nullptr);
    item->setParent(shell.window.get());
    item->setParentItem(host);
    if (fill) {
        item->setPosition({0, 0});
        item->setSize(host->size());
    } else {
        // Over the bottom of the panel, on its background, as the section would show it.
        item->setWidth(host->width() - 24);
        item->setPosition({12, host->height() - item->implicitHeight() - 12});
        item->setZ(10);
    }
    return item;
}

std::string picture100() { return "V1 |A" + std::string(99, 'a') + "|\nA1 |A" + std::string(99, 'a') + "|"; }

struct ObjectShell {
    Shell shell;
    ProjectController* controller = nullptr;
    MaskBridge* masks = nullptr;
    ObjectMaskBridge* objects = nullptr;
    SourceMonitorBridge* source = nullptr;
    Timeline tl{picture100()};
    ClipId clip;
    std::unique_ptr<ScriptedServices> seg;
    std::shared_ptr<ai::analysis::MapAnalysisProvider> shots = std::make_shared<ai::analysis::MapAnalysisProvider>();

    explicit ObjectShell(const std::string& name) {
        resetSettings();
        clearMessages();
        REQUIRE(shell.window != nullptr);
        shell.theme->setProperty("reduceMotion", true);
        shell.resize(kShotWidth, kShotHeight);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        masks = shell.engine.singletonInstance<MaskBridge*>("Wabcut", "Masks");
        objects = shell.engine.singletonInstance<ObjectMaskBridge*>("Wabcut", "ObjectMasks");
        source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
        REQUIRE(controller);
        REQUIRE(masks);
        REQUIRE(objects);
        REQUIRE(source);
        tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
        controller->selectClipById(clip, ProjectController::Replace);
        controller->setPlayhead(frameTicks(20));
        REQUIRE(waitFor([&] { return masks->clip() == clipKey(clip); }));
        seg = std::make_unique<ScriptedServices>(freshDir(name));
        ai::analysis::ShotAnalysis cut;
        cut.cuts.push_back(ai::analysis::ShotCut{frameTicks(60), 60, ai::analysis::CutKind::Cut, 0.9f});
        shots->addShots(tl.media('A'), cut);
        objects->setServices(seg->services, shots);
        objects->setSynchronous(true);
    }

    const Mask* objectMask() const {
        const Clip& c = controller->currentSequence()->findClip(clip)->clip();
        for (const EffectInstance& e : c.effects()) {
            for (const Mask& m : e.masks) {
                if (m.shape == MaskShape::Matte) {
                    return &m;
                }
            }
        }
        return nullptr;
    }
};

} // namespace

TEST_CASE("Object tool: click, refine, track to the cut with its confidence, the matte preview; screenshots",
          "[ai][ui][segment][screenshots]") {
    ObjectShell s("ui-object");
    ObjectMaskBridge& objects = *s.objects;
    REQUIRE(objects.available());
    CHECK(s.shell.engine.imageProvider(QStringLiteral("wabcut-matte")) != nullptr);
    s.masks->setTool(QStringLiteral("ellipse"));
    objects.setArmed(true);
    CHECK(s.masks->tool().isEmpty()); // one monitor tool at a time
    const std::size_t undo = s.controller->session().undoCount();

    // A click on the object (the clip fills the canvas: canvas fractions are frame fractions).
    REQUIRE(objects.click(0.25, 0.4, true));
    INFO(objects.status().toStdString());
    CHECK_FALSE(objects.busy());
    REQUIRE(s.objectMask() != nullptr);
    CHECK(s.controller->session().undoCount() == undo + 1);
    CHECK(s.controller->undoText() == QStringLiteral("Undo Select Object"));
    CHECK(objects.mask() == s.masks->selected());
    CHECK(objects.status().contains(QStringLiteral("1 frame")));
    REQUIRE(objects.points().size() == 1);
    CHECK(objects.points()[0].toMap().value(QStringLiteral("x")).toDouble() == Approx(0.25).margin(1e-6));
    CHECK(objects.points()[0].toMap().value(QStringLiteral("positive")).toBool());
    REQUIRE(s.masks->masks().size() == 1);
    CHECK(s.masks->masks()[0].toMap().value(QStringLiteral("shape")).toString() == QStringLiteral("matte"));
    // With the hooks patch (MaskBridge): an object mask's box is a stand-in, not handles.
    if (findItem(s.shell.window->contentItem(), QStringLiteral("programObjectOverlay")) != nullptr) {
        CHECK(s.masks->handles().isEmpty());
    }

    // A negative click refines it (the same mask, one more undo step).
    const std::string firstTrack = s.objectMask()->matte->track;
    REQUIRE(objects.click(0.7, 0.8, false));
    CHECK(s.controller->session().undoCount() == undo + 2);
    CHECK(s.objectMask()->matte->track != firstTrack);
    REQUIRE(objects.points().size() == 2);
    CHECK_FALSE(objects.points()[1].toMap().value(QStringLiteral("positive")).toBool());

    // Track both ways: to the cut at frame 60, the confidence of every frame.
    REQUIRE(objects.propagate(QStringLiteral("both")));
    INFO(objects.status().toStdString());
    CHECK(s.controller->undoText() == QStringLiteral("Undo Track Object Mask"));
    CHECK(objects.confidence().size() == 60);
    CHECK(objects.total() == 60);
    CHECK(objects.status().contains(QStringLiteral("stopped at the cut")));
    CHECK(s.objectMask()->matte->source == TimeRange{Ticks{}, frameTicks(60)});

    // The matte preview: the object tinted, where the frame lies on the canvas.
    objects.setPreview(QStringLiteral("colour"));
    REQUIRE_FALSE(objects.previewSource().isEmpty());
    REQUIRE(objects.previewQuad().size() == 6);
    CHECK(objects.previewQuad()[0].toDouble() == Approx(0.0).margin(1e-6));
    CHECK(objects.previewQuad()[2].toDouble() == Approx(1.0).margin(1e-6));
    CHECK(objects.previewQuad()[5].toDouble() == Approx(1.0).margin(1e-6));
    const QImage preview = objects.previewImage();
    REQUIRE_FALSE(preview.isNull());
    const MatteBox truth = ScriptedScene{}.objectAt(20);
    const auto alphaAt = [&](double u, double v) { return qAlpha(preview.pixel(static_cast<int>(u * preview.width()), static_cast<int>(v * preview.height()))); };
    CHECK(alphaAt(truth.x + truth.width / 2, truth.y + truth.height / 2) > 100);
    CHECK(alphaAt(0.8, 0.8) == 0);
    objects.setPreview(QStringLiteral("alpha"));
    CHECK(qGray(objects.previewImage().pixel(static_cast<int>((truth.x + 0.05) * preview.width()), static_cast<int>(0.4 * preview.height()))) > 200);
    objects.setPreview(QStringLiteral("colour"));

    // The Object tool's QML (hooked into the shell, or laid over the same panels).
    const bool hooked = findItem(s.shell.window->contentItem(), QStringLiteral("programObjectOverlay")) != nullptr;
    QQuickItem* frameArea = s.shell.item("programFrameArea");
    QQuickItem* inspector = s.shell.item("effectControls");
    QQuickItem* overlay = placed(s.shell, "ObjectPromptOverlay", QStringLiteral("programObjectOverlay"), frameArea, true);
    QQuickItem* controls = placed(s.shell, "ObjectMaskControls", QStringLiteral("objectMaskControls"), inspector, false);
    REQUIRE(overlay != nullptr);
    REQUIRE(controls != nullptr);
    spin(150);
    // The preview is drawn over the monitor: the image item is shown and its matte loaded.
    QQuickItem* matteItem = findItem(overlay, QStringLiteral("objectMattePreview"));
    REQUIRE(matteItem != nullptr);
    CHECK(matteItem->isVisible());
    INFO("source " << matteItem->property("source").toString().toStdString() << "; Qt: " << join(takeMessages()));
    CHECK(matteItem->property("status").toInt() == 1); // Image.Ready
    CHECK(matteItem->property("implicitWidth").toDouble() > 100);
    QQuickItem* download = findItem(controls, QStringLiteral("objectDownload"));
    CHECK((download == nullptr || !download->isVisible())); // the model is there: the tool, not the download card
    const QImage image = s.shell.grab();
    REQUIRE_FALSE(image.isNull());
    // Screenshots of the real layout need the hooks patch (MaskSection, MonitorPanel); without it, no screenshots.
    if (hooked) {
        image.save(outputDir() + QStringLiteral("/object-mask-ui.png"));
        const double dpr = image.devicePixelRatio();
        for (const auto& [name, file] : {std::pair{"effectControls", "object-mask-effect-controls.png"}, std::pair{"programFrameArea", "object-mask-monitor.png"}}) {
            QQuickItem* item = s.shell.item(name);
            if (!item) {
                item = qobject_cast<QQuickItem*>(s.shell.window->contentItem()->findChild<QObject*>(QLatin1String(name)));
            }
            REQUIRE(item != nullptr);
            const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect()).save(outputDir() + QStringLiteral("/") + QLatin1String(file));
        }
    }
    // The Object tool shows, and with an object its track buttons.
    CHECK(controls->isVisible());
    QQuickItem* trackBoth = findItem(controls, QStringLiteral("objectTrackBoth"));
    REQUIRE(trackBoth != nullptr);
    CHECK(trackBoth->isVisible());

    // Undo walks it all back.
    REQUIRE(s.controller->session().undo());
    REQUIRE(s.controller->session().undo());
    REQUIRE(s.controller->session().undo());
    CHECK(s.objectMask() == nullptr);
    objects.setArmed(false);
    objects.setPreview(QStringLiteral("off"));
}

TEST_CASE("Source Monitor: an AI-tracked redaction, failing closed in thumbnails wherever a frame has no matte",
          "[ai][ui][segment][redaction]") {
    ObjectShell s("ui-redact");
    ObjectMaskBridge& objects = *s.objects;
    const MediaId media = s.tl.media('A');
    REQUIRE(s.source->load(mediaKey(media)));
    REQUIRE(waitFor([&] { return s.source->mediaId() == media; }));
    s.masks->setRedactStyle(QStringLiteral("solid"));
    objects.setTrackRedactions(true);
    REQUIRE(objects.redactObject(0.08, 0.28, 0.27, 0.52));
    INFO(objects.status().toStdString());
    CHECK(objects.status().contains(QStringLiteral("60 frame")));
    CHECK(s.controller->undoText() == QStringLiteral("Undo Add Tracked Redaction"));
    const ProjectSnapshot snap = s.controller->snapshot();
    REQUIRE(snap->redactions.size() == 1);
    const Redaction& r = snap->redactions.front();
    CHECK(r.region.shape == MaskShape::Matte);
    CHECK(r.source == TimeRange{Ticks{}, frameTicks(60)});
    CHECK(r.appliesTo(s.clip));

    // Thumbnails and filmstrips (RedactedPictures) through the installed matte source over the cache.
    const auto installed = seg::installMatteSource(s.seg->services->cacheRoot);
    const auto covered = [&](const ProjectState& state, int frame, const MatteBox& where) {
        QImage picture(192, 108, QImage::Format_RGBA8888);
        picture.fill(QColor(128, 128, 128));
        if (!redacted::apply(picture, state, media, frameTicks(frame))) {
            return false;
        }
        const QColor c = picture.pixelColor(static_cast<int>((where.x + where.width / 2) * 192), static_cast<int>((where.y + where.height / 2) * 108));
        return c != QColor(128, 128, 128);
    };
    const ScriptedScene scene;
    CHECK(covered(*snap, 10, scene.objectAt(10)));                                   // the matte at the frame
    CHECK_FALSE(covered(*snap, 10, MatteBox{0.75, 0.75, 0.1, 0.1}));                 // not the rest of the picture
    // A frame inside the redaction without a matte of its own (the cache lost it): the nearest matte, grown.
    Redaction gap = r;
    gap.source = TimeRange{Ticks{}, frameTicks(80)};
    ProjectState widened = *snap;
    widened.redactions.front() = gap;
    CHECK(covered(widened, 70, scene.objectAt(59)));
    // No matte of that track on this computer at all: the bounding box, grown - never nothing.
    render::setMatteSource(std::make_shared<seg::CacheMatteSource>(freshDir("ui-redact-empty-cache")));
    CHECK(covered(*snap, 10, scene.objectAt(10)));
    render::setMatteSource(nullptr);
    CHECK(covered(*snap, 30, scene.objectAt(30)));
    seg::installMatteSource();
    REQUIRE(s.controller->session().undo());
    CHECK(s.controller->snapshot()->redactions.empty());
}
