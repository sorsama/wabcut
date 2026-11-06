// P.2 in the UI, through the real shell (offscreen, nothing shown): Time Interpolation in the Speed/Duration dialog
// (applied with the speed as one undo step), the clip menu's submenu and Time Remapping's button; Warp Stabilizer's
// Analyze through the `Stabilizer` singleton (synchronous here, over synthetic shaky pictures) and its Effect Controls
// header; the Library's New Item submenu (Bars and Tone, the counting leader).

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "EffectControls.h"
#include "ExportController.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"
#include "StabilizerBridge.h"
#include "SyntheticMotion.h"
#include "TimelineFixture.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/Synthetic.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QQuickItem>

#include <cmath>
#include <filesystem>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// Pictures of the fixture's media (1920x1080, 25 fps) at most 320 wide, shaken by a known jitter per frame.
class ShakyPictures final : public masks::TrackingServices {
public:
    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                     int maxWidth) override {
        const VideoStreamInfo& v = state.findMedia(media)->info.video.at(static_cast<std::size_t>(stream));
        if (time.value < 0 || time >= v.duration) {
            return std::unexpected(Error{ErrorCode::NotFound, "past the end"});
        }
        const std::int64_t k = ticksToFrames(time, v.frameRate);
        const double x = 2.0 * std::sin(1.7 * static_cast<double>(k));
        const double y = 1.5 * std::sin(2.3 * static_cast<double>(k) + 0.5);
        const int w = std::min(maxWidth, 320);
        masks::TrackingFrame f;
        f.luma = masks::test::render(m_texture, w, w * v.height / v.width, masks::Motion{1, 0, -x, 0, 1, -y});
        f.start = framesToTicks(k, v.frameRate);
        f.duration = framesToTicks(1, v.frameRate);
        return f;
    }

private:
    masks::test::Texture m_texture{29};
};

QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* visibleNamed(Shell& shell, const char* name) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == QLatin1String(name) && i->isVisible(); });
}

TimeInterpolation interpolationOf(const ProjectController& c, ClipId clip) {
    return c.currentSequence()->findClip(clip)->clip().timeInterpolation();
}

} // namespace

TEST_CASE("Motion UI: the Speed/Duration dialog applies Time Interpolation with the speed in one undo step", "[app][motion][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    const ClipId clip = controller->primaryClip();
    REQUIRE(controller->speedDurationInfo().value(QStringLiteral("hasVideo")).toBool());
    CHECK(controller->speedDurationInfo().value(QStringLiteral("timeInterpolation")).toString() == QStringLiteral("frameSampling"));
    const Ticks length = controller->currentSequence()->findClip(clip)->clip().duration();

    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("clip.speedDuration"))));
    QObject* dialog = shell.object("speedDurationDialog");
    REQUIRE(dialog != nullptr);
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
    CHECK(dialog->property("interpolation").toString() == QStringLiteral("frameSampling"));
    REQUIRE(dialog->setProperty("interpolation", QStringLiteral("opticalFlow")));
    REQUIRE(QMetaObject::invokeMethod(dialog, "setSpeed", Q_ARG(QVariant, 200.0)));
    const QVariantMap preview = dialog->property("preview").toMap();
    INFO(preview.value(QStringLiteral("error")).toString().toStdString());
    CHECK(preview.value(QStringLiteral("ok")).toBool());
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    CHECK(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(Ticks{controller->currentSequence()->findClip(clip)->clip().duration().value * 2} == length);
    CHECK(interpolationOf(*controller, clip) == TimeInterpolation::OpticalFlow);
    CHECK(controller->speedDurationInfo().value(QStringLiteral("timeInterpolation")).toString() == QStringLiteral("opticalFlow"));
    REQUIRE(controller->undo());
    CHECK(controller->currentSequence()->findClip(clip)->clip().duration() == length);
    CHECK(interpolationOf(*controller, clip) == TimeInterpolation::FrameSampling);

    // The clip menu's submenu sets it alone on the selection.
    QObject* submenu = shell.object("timeInterpolationSubmenu");
    REQUIRE(submenu != nullptr);
    REQUIRE(QMetaObject::invokeMethod(submenu, "refresh"));
    CHECK(submenu->property("hasVideo").toBool());
    REQUIRE(QMetaObject::invokeMethod(submenu, "choose", Q_ARG(QVariant, QStringLiteral("frameBlending"))));
    CHECK(submenu->property("error").toString().isEmpty());
    CHECK(interpolationOf(*controller, clip) == TimeInterpolation::FrameBlending);
    CHECK(submenu->property("current").toString() == QStringLiteral("frameBlending"));
    CHECK(controller->undoText() == QStringLiteral("Undo Time Interpolation"));
    REQUIRE(controller->undo());
    CHECK(interpolationOf(*controller, clip) == TimeInterpolation::FrameSampling);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Motion UI: Warp Stabilizer's Analyze runs as a job and keys the effect in one undo step", "[app][motion][stabilize]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* stabilizer = shell.engine.singletonInstance<StabilizerBridge*>("Wabcut", "Stabilizer");
    REQUIRE(controller);
    REQUIRE(stabilizer);
    Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const SequenceId seq = tl.seq;
    REQUIRE(controller->session()
                .apply("Apply Effect",
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kWarpStabilizerEffect)); })
                .has_value());
    const EffectInstanceId id = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(id.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});

    QVariantMap info = stabilizer->effectInfo(section);
    CHECK(info.value(QStringLiteral("stabilizer")).toBool());
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("warp"));
    CHECK(info.value(QStringLiteral("status")).toString().isEmpty());
    CHECK_FALSE(stabilizer->effectInfo(QStringLiteral("e999999")).value(QStringLiteral("stabilizer")).toBool());

    // Effect Controls' header of the effect shows Analyze (the list scrolled to it, as a grab lays the shell out).
    REQUIRE_FALSE(shell.grab().isNull());
    EffectControlsModel& fx = *controller->effectControls();
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(fx.rowOf(QStringLiteral("h:") + section) >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + section)), Q_ARG(int, 0)));
    spin(50);
    QQuickItem* button = nullptr;
    CHECK(waitFor([&] { return (button = visibleNamed(shell, "stabilizerAnalyze")) != nullptr; }));
    if (button) {
        CHECK(button->property("text").toString() == QStringLiteral("Analyze"));
    }

    stabilizer->setTrackingServices(std::make_shared<ShakyPictures>());
    stabilizer->setSynchronous(true);
    const std::filesystem::path cache = std::filesystem::path(outputDir().toStdString()) / "stabilizer-cache";
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);
    stabilizer->setCacheRoot(cache);
    REQUIRE(stabilizer->analyze(section));
    INFO(stabilizer->message().toStdString());
    CHECK_FALSE(stabilizer->busy());
    CHECK(stabilizer->message().startsWith(QStringLiteral("Analysis complete")));
    info = stabilizer->effectInfo(section);
    CHECK(info.value(QStringLiteral("status")).toString() == QStringLiteral("analyzed"));
    const EffectInstance& analysed = controller->currentSequence()->findClip(clip)->clip().effects().front();
    REQUIRE(analysed.find("corner.tl.x")); // Premiere's default method, Subspace Warp: a corner pin per frame
    CHECK(analysed.find("corner.tl.x")->isCurve());
    CHECK(controller->undoText() == QStringLiteral("Undo Analyze Stabilization"));
    if (button) {
        CHECK(waitFor([&] { return button->property("text").toString() == QStringLiteral("Reanalyze"); }));
    }
    REQUIRE(controller->undo());
    CHECK(stabilizer->effectInfo(section).value(QStringLiteral("status")).toString().isEmpty());
    CHECK(controller->currentSequence()->findClip(clip)->clip().effects().front().find("corner.tl.x") == nullptr);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// Fix round 3 (NEW-01): Analyze on a Warp Stabilizer in the Master Clip Effects (Effect Controls shows it with the
// selected clip) keys the media item's instance, for every clip of the media, in one undo step.
TEST_CASE("Motion UI: Analyze keys a Warp Stabilizer in the Master Clip Effects", "[app][motion][stabilize]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* stabilizer = shell.engine.singletonInstance<StabilizerBridge*>("Wabcut", "Stabilizer");
    REQUIRE(controller);
    REQUIRE(stabilizer);
    Timeline tl("V1 |Aaaaaaaaaaaa......A30aaaaaaaaaaa|");
    controller->resetProject(tl.state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const ClipId other = clipAt(*controller->snapshot(), tl.seq, "V1@18");
    const SequenceId seq = tl.seq;
    const MediaId media = tl.media('A');
    const auto added = controller->session().apply("Add Master Clip Effect", [&](const ProjectState& s) {
        return edit::addEffect(s, seq, edit::EffectTarget::ofMedia(media), std::string(kWarpStabilizerEffect));
    });
    REQUIRE(added);
    const EffectInstanceId id = added->createdEffects.front();
    const QString section = QStringLiteral("e%1").arg(id.value);
    const auto masterWarp = [&]() -> const EffectInstance* {
        for (const EffectInstance& e : controller->snapshot()->findMedia(media)->masterEffects) {
            if (e.id == id)
                return &e;
        }
        return nullptr;
    };
    controller->selectClipById(other, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    // Effect Controls lists it under the clip's Master Clip group; its header asks the Stabilizer.
    REQUIRE_FALSE(shell.grab().isNull());
    REQUIRE(controller->effectControls()->rowOf(QStringLiteral("h:") + section) >= 0);
    QVariantMap info = stabilizer->effectInfo(section);
    CHECK(info.value(QStringLiteral("stabilizer")).toBool());
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("warp"));
    CHECK(info.value(QStringLiteral("status")).toString().isEmpty());

    stabilizer->setTrackingServices(std::make_shared<ShakyPictures>());
    stabilizer->setSynchronous(true);
    const std::filesystem::path cache = std::filesystem::path(outputDir().toStdString()) / "stabilizer-master-cache";
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);
    stabilizer->setCacheRoot(cache);
    REQUIRE(stabilizer->analyze(section));
    INFO(stabilizer->message().toStdString());
    CHECK_FALSE(stabilizer->busy());
    CHECK(stabilizer->message().startsWith(QStringLiteral("Analysis complete")));
    CHECK(stabilizer->effectInfo(section).value(QStringLiteral("status")).toString() == QStringLiteral("analyzed"));
    const EffectInstance* warp = masterWarp();
    REQUIRE(warp);
    REQUIRE(warp->find("corner.tl.x"));
    CHECK(warp->find("corner.tl.x")->isCurve());
    // Keyed over both clips' frames (0 to 41), on the media item: no clip got an effect of its own.
    const Curve* curve = std::get_if<Curve>(&warp->find("corner.tl.x")->data);
    REQUIRE(curve);
    REQUIRE_FALSE(curve->keys.empty());
    CHECK(curve->keys.front().time <= framesToTicks(0, kRate25));
    CHECK(curve->keys.back().time >= framesToTicks(40, kRate25));
    CHECK(controller->currentSequence()->findClip(clip)->clip().effects().empty());
    CHECK(controller->currentSequence()->findClip(other)->clip().effects().empty());
    CHECK(controller->undoText() == QStringLiteral("Undo Analyze Stabilization"));
    REQUIRE(controller->undo());
    CHECK(stabilizer->effectInfo(section).value(QStringLiteral("status")).toString().isEmpty());
    REQUIRE(masterWarp());
    CHECK(masterWarp()->find("corner.tl.x") == nullptr);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Motion UI: the Library's New Item submenu makes Bars and Tone and the counting leader", "[app][motion][library]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    QObject* menu = shell.object("libraryNewItemMenu");
    REQUIRE(menu != nullptr);
    const auto count = [&](SyntheticKind kind) {
        int n = 0;
        for (const MediaPtr& m : controller->snapshot()->media) {
            const SyntheticDescriptor* d = m ? syntheticOf(*m) : nullptr;
            n += d && d->kind == kind ? 1 : 0;
        }
        return n;
    };
    const QVariantMap sd{{QStringLiteral("pattern"), 1}};
    REQUIRE(QMetaObject::invokeMethod(menu, "make", Q_ARG(QVariant, QStringLiteral("barsAndTone")), Q_ARG(QVariant, sd)));
    INFO(menu->property("error").toString().toStdString());
    CHECK(menu->property("error").toString().isEmpty());
    CHECK(count(SyntheticKind::BarsAndTone) == 1);
    const QVariantMap none;
    REQUIRE(QMetaObject::invokeMethod(menu, "make", Q_ARG(QVariant, QStringLiteral("countingLeader")), Q_ARG(QVariant, none)));
    CHECK(count(SyntheticKind::CountingLeader) == 1);
    REQUIRE(controller->undo());
    CHECK(count(SyntheticKind::CountingLeader) == 0);
    CHECK(count(SyntheticKind::BarsAndTone) == 1);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// 0.7 (cards #128 and #107): Analyze All through the Stabilizer singleton - one job, one undo step, current effects skipped,
// out-of-date ones redone - the header's "Out of date" state and Analyze All button, and the Export panel's data: a
// stabilizer that is not analysed exports unstabilized, per exported range.
TEST_CASE("Motion UI: Analyze All analyses every stabilizer in one undo step, and the export range counts the pending ones",
          "[app][motion][stabilize][analyzeAll]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* stabilizer = shell.engine.singletonInstance<StabilizerBridge*>("Wabcut", "Stabilizer");
    REQUIRE(controller);
    REQUIRE(stabilizer);
    Timeline tl("V1 |Aaaaaaaaaa..A40aaaaaaaaa|\nA1 |Aaaaaaaaaa..A40aaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId first = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const ClipId second = clipAt(*controller->snapshot(), tl.seq, "V1@12");
    const SequenceId seq = tl.seq;
    for (const ClipId clip : {first, second}) {
        REQUIRE(controller->session()
                    .apply("Apply Effect",
                           [&](const ProjectState& s) {
                               return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kWarpStabilizerEffect));
                           })
                    .has_value());
    }
    controller->selectClipById(first, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    const auto effectOf = [&](ClipId clip) { return controller->currentSequence()->findClip(clip)->clip().effects().front().id; };
    const QString sectionA = QStringLiteral("e%1").arg(effectOf(first).value);
    const QString sectionB = QStringLiteral("e%1").arg(effectOf(second).value);

    // Nothing analysed: two pending; the export would render both unstabilized, and says so (whole range).
    QVariantMap overview = stabilizer->overview();
    CHECK(overview.value(QStringLiteral("count")).toInt() == 2);
    CHECK(overview.value(QStringLiteral("pending")).toInt() == 2);
    CHECK(overview.value(QStringLiteral("notAnalyzed")).toInt() == 2);
    QVariantMap needs = stabilizer->exportNeeds(QStringLiteral("whole"));
    CHECK(needs.value(QStringLiteral("count")).toInt() == 2);
    CHECK(needs.value(QStringLiteral("names")).toStringList().front().contains(QStringLiteral("Warp Stabilizer")));
    CHECK(needs.value(QStringLiteral("names")).toStringList().front().startsWith(QStringLiteral("V1: A")));
    // A range that holds only the second clip: one.
    controller->setPlayhead(framesToTicks(12, kRate25));
    controller->markInAtPlayhead();
    controller->setPlayhead(framesToTicks(22, kRate25));
    controller->markOutAtPlayhead();
    controller->setPlayhead(Ticks{});
    needs = stabilizer->exportNeeds(QStringLiteral("inOut"));
    CHECK(needs.value(QStringLiteral("count")).toInt() == 1);
    needs = stabilizer->exportNeeds(QStringLiteral("whole"));
    CHECK(needs.value(QStringLiteral("count")).toInt() == 2);

    // The header offers Analyze All (two stabilizers) with the number pending.
    REQUIRE_FALSE(shell.grab().isNull());
    EffectControlsModel& fx = *controller->effectControls();
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(fx.rowOf(QStringLiteral("h:") + sectionA) >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + sectionA)), Q_ARG(int, 0)));
    spin(50);
    QQuickItem* all = nullptr;
    REQUIRE(waitFor([&] { return (all = visibleNamed(shell, "stabilizerAnalyzeAll")) != nullptr; }));
    CHECK(all->property("text").toString() == QStringLiteral("Analyze All (2)"));
    const auto shootPanel = [&](const char* name) {
        QQuickItem* panel = shell.item("effectControls");
        REQUIRE(panel != nullptr);
        for (const bool dark : {true, false}) {
            shell.setAppearance(dark, "ultraviolet");
            REQUIRE(shell.settle(dark, shell.themeColor("accentFill"))); // the palette cross-fade has finished
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            const double dpr = image.devicePixelRatio();
            const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
            REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                        .save(outputDir() + QStringLiteral("/stabilizer-%1-%2.png").arg(QLatin1String(name), dark ? QStringLiteral("dark") : QStringLiteral("light"))));
        }
        shell.setAppearance(true, "ultraviolet");
    };
    shootPanel("analyze-all-pending");

    // One job: both analysed over their 10 frames each, one undo step, nothing left pending.
    stabilizer->setTrackingServices(std::make_shared<ShakyPictures>());
    stabilizer->setSynchronous(true);
    const std::filesystem::path cache = std::filesystem::path(outputDir().toStdString()) / "stabilizer-all-cache";
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);
    stabilizer->setCacheRoot(cache);
    const std::size_t before = controller->session().undoCount();
    // Sequence > Analyze All Stabilizers (the keymap command the menu item and the palette run, routed by Main.qml).
    auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(keys != nullptr);
    REQUIRE(keys->trigger(QStringLiteral("sequence.analyzeStabilizers")));
    INFO(stabilizer->message().toStdString());
    CHECK_FALSE(stabilizer->busy());
    CHECK(stabilizer->message().startsWith(QStringLiteral("Analyzed 2 stabilizer")));
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Analyze All Stabilizers"));
    for (const QString& section : {sectionA, sectionB})
        CHECK(stabilizer->effectInfo(section).value(QStringLiteral("status")).toString() == QStringLiteral("analyzed"));
    CHECK(stabilizer->overview().value(QStringLiteral("pending")).toInt() == 0);
    CHECK(stabilizer->exportNeeds(QStringLiteral("whole")).value(QStringLiteral("count")).toInt() == 0);
    // Nothing to do now: false, with the reason.
    CHECK_FALSE(stabilizer->analyzeAll(false));
    CHECK(stabilizer->message() == QStringLiteral("Every stabilizer is already analyzed"));
    CHECK(controller->session().undoCount() == before + 1);

    // A setting changed afterwards: that one is out of date (the header says so), and Analyze All redoes only it.
    REQUIRE(controller->session()
                .apply("Smoothness",
                       [&](const ProjectState& s) {
                           edit::EffectChange change;
                           change.params = {EffectParam{"smoothness", ParamValue(300.0)}};
                           return edit::setEffect(s, seq, first, effectOf(first), change);
                       })
                .has_value());
    CHECK(stabilizer->effectInfo(sectionA).value(QStringLiteral("outOfDate")).toBool());
    CHECK_FALSE(stabilizer->effectInfo(sectionB).value(QStringLiteral("outOfDate")).toBool());
    CHECK(stabilizer->overview().value(QStringLiteral("outOfDate")).toInt() == 1);
    CHECK(stabilizer->exportNeeds(QStringLiteral("whole")).value(QStringLiteral("count")).toInt() == 1);
    spin(50);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* status = visibleNamed(shell, "stabilizerStatus");
    REQUIRE(status != nullptr);
    CHECK(status->property("text").toString() == QStringLiteral("Out of date"));
    QQuickItem* allAgain = visibleNamed(shell, "stabilizerAnalyzeAll");
    REQUIRE(allAgain != nullptr);
    CHECK(allAgain->property("text").toString() == QStringLiteral("Analyze All (1)"));
    shootPanel("analyze-all-out-of-date");
    CHECK(stabilizer->analyzeAll(false));
    CHECK(stabilizer->overview().value(QStringLiteral("pending")).toInt() == 0);
    CHECK(controller->undoText() == QStringLiteral("Undo Analyze All Stabilizers"));
    // Reanalyze All redoes every one.
    CHECK(stabilizer->analyzeAll(true));
    CHECK(stabilizer->message().startsWith(QStringLiteral("Analyzed 2 stabilizer"))); // the same keys again: no new undo step

    // A project that changed while it ran is not overwritten.
    struct Nudge final : masks::TrackingServices {
        masks::TrackingServices* inner = nullptr;
        std::function<void()> onFirst;
        std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time, int maxWidth) override {
            return inner->frame(state, media, stream, time, maxWidth);
        }
        void progress(const masks::TrackProgress&) override {
            if (onFirst) {
                auto run = std::move(onFirst);
                run();
            }
        }
    };
    auto nudge = std::make_shared<Nudge>();
    ShakyPictures shaky;
    nudge->inner = &shaky;
    nudge->onFirst = [&] {
        REQUIRE(controller->session()
                    .apply("Opacity", [&](const ProjectState& s) { return edit::setParameter(s, seq, first, edit::ParamRef::of(ParamId::Opacity), ParamValue(0.5)); })
                    .has_value());
    };
    stabilizer->setTrackingServices(nudge);
    CHECK(stabilizer->analyzeAll(true));
    CHECK(stabilizer->message().contains(QStringLiteral("changed")));
    CHECK(controller->undoText() == QStringLiteral("Undo Opacity"));
    stabilizer->setTrackingServices(std::make_shared<ShakyPictures>());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// 0.7 (card #107): the Export panel warns, before the export, about a stabilizer that is not analysed (it would export
// unstabilized), for the range being exported, with "Analyze now" (the Analyze All job); the export stays allowed.
TEST_CASE("Motion UI: the Export panel warns about stabilizers that are not analysed and Analyze now fixes it (card 107)",
          "[app][motion][stabilize][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* stabilizer = shell.engine.singletonInstance<StabilizerBridge*>("Wabcut", "Stabilizer");
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(controller);
    REQUIRE(stabilizer);
    REQUIRE(exporter);
    exporter->setVideoSourceFactory(wabcut::exporter::syntheticVideoSourceFactory());
    Timeline tl("V1 |Aaaaaaaaaa..A40aaaaaaaaa|\nA1 |Aaaaaaaaaa..A40aaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId first = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const ClipId second = clipAt(*controller->snapshot(), tl.seq, "V1@12");
    const SequenceId seq = tl.seq;
    for (const ClipId clip : {first, second}) {
        REQUIRE(controller->session()
                    .apply("Apply Effect",
                           [&](const ProjectState& s) {
                               return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kWarpStabilizerEffect));
                           })
                    .has_value());
    }
    stabilizer->setTrackingServices(std::make_shared<ShakyPictures>());
    stabilizer->setSynchronous(true);
    const std::filesystem::path cache = std::filesystem::path(outputDir().toStdString()) / "stabilizer-export-cache";
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);
    stabilizer->setCacheRoot(cache);

    shell.window->setProperty("workspace", 2); // the Export workspace
    spin(200);
    QQuickItem* content = shell.window->contentItem();
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = findItem(content, [](QQuickItem* i) { return i->property("sessionPicks").isValid(); });
        return panel != nullptr && panel->isVisible();
    }));
    const auto named = [&](const char* name) {
        return findItem(content, [&](QQuickItem* i) { return i->objectName() == QLatin1String(name); });
    };

    // Two stabilizers that are not analysed: the warning names them, says what happens, and offers Analyze now.
    QQuickItem* warning = named("exportStabilizerWarning");
    QQuickItem* analyze = named("exportAnalyze");
    REQUIRE(warning != nullptr);
    REQUIRE(analyze != nullptr);
    REQUIRE(waitFor([&] { return warning->isVisible(); }));
    const QString text = warning->property("text").toString();
    INFO(text.toStdString());
    CHECK(text.startsWith(QStringLiteral("2 stabilizers are not analyzed, so they will export unstabilized: V1: A at 00:00:00:00 (Warp Stabilizer), V1: A at ")));
    CHECK(analyze->property("text").toString() == QStringLiteral("Analyze now"));
    {
        const QImage shot = shell.grab();
        REQUIRE_FALSE(shot.isNull());
        const double dpr = shot.devicePixelRatio();
        const QRectF r = warning->mapRectToScene(QRectF(0, 0, warning->width(), warning->height())).adjusted(-8, -8, 8, 8);
        REQUIRE(shot.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                    .save(outputDir() + QStringLiteral("/export-stabilizer-warning.png")));
        shell.setAppearance(false, "ultraviolet");
        REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        const QImage light = shell.grab();
        REQUIRE_FALSE(light.isNull());
        REQUIRE(light.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                    .save(outputDir() + QStringLiteral("/export-stabilizer-warning-light.png")));
        shell.setAppearance(true, "ultraviolet");
    }

    // Only the second clip is exported (In to Out over it): one.
    controller->setPlayhead(framesToTicks(12, kRate25));
    controller->markInAtPlayhead();
    controller->setPlayhead(framesToTicks(22, kRate25));
    controller->markOutAtPlayhead();
    CHECK(stabilizer->exportNeeds(QStringLiteral("inOut")).value(QStringLiteral("count")).toInt() == 1);
    CHECK(stabilizer->exportNeeds(QStringLiteral("whole")).value(QStringLiteral("count")).toInt() == 2);

    // Analyze now: one job, one undo step; the warning goes.
    const std::size_t before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(analyze, "clicked"));
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Analyze All Stabilizers"));
    REQUIRE(waitFor([&] { return !warning->isVisible(); }));
    CHECK(stabilizer->exportNeeds(QStringLiteral("whole")).value(QStringLiteral("count")).toInt() == 0);
    // Undo brings the warning back.
    REQUIRE(controller->undo());
    REQUIRE(waitFor([&] { return warning->isVisible(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("ExportPanel"), Qt::CaseInsensitive));
}
