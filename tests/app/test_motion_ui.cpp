// P.2 in the UI, through the real shell (offscreen, nothing shown): Time Interpolation in the Speed/Duration dialog
// (applied with the speed as one undo step), the clip menu's submenu and Time Remapping's button; Warp Stabilizer's
// Analyze through the `Stabilizer` singleton (synchronous here, over synthetic shaky pictures) and its Effect Controls
// header; the Library's New Item submenu (Bars and Tone, the counting leader).

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "EffectControls.h"
#include "ProjectController.h"
#include "StabilizerBridge.h"
#include "SyntheticMotion.h"
#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/Synthetic.h>

#include <catch2/catch_test_macros.hpp>

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
