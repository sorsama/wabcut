// 0.7 (card #130): Warp Stabilizer's Method "Gyro (camera metadata)" in the UI. Effect Controls lists the Gyro choice for footage
// whose file has gyro data (a fake probe here) and for a stabilizer that already uses it, shows the four Gyro settings only with
// that Method, and puts a note under the header: what the file has before the analysis, how it was synced after it, why a file
// cannot be used. Analyze through the `Stabilizer` singleton reads the camera's gyro (a fake service), keys the effect in one
// undo step and the note says what it did. Screenshots of the panel, dark and light.

#include "EffectControls.h"
#include "GyroAvailability.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "StabilizerBridge.h"
#include "TimelineFixture.h"

#include "../ai/stabilize/GyroShake.h"

#include <wabcut/gyro/Gyro.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QQuickItem>
#include <QString>
#include <QVariantMap>

#include <filesystem>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

constexpr const char* kPicture = "V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaaaaaaaaaaaaaa|";

editor::GyroProbe goPro() {
    editor::GyroProbe p;
    p.state = editor::GyroProbe::State::Readable;
    p.make = "GoPro";
    p.model = "HERO8 Black";
    p.format = "GoPro GPMF";
    p.rateHz = 202.0;
    p.axesChecked = true;
    return p;
}

editor::GyroProbe unreadable() {
    editor::GyroProbe p;
    p.state = editor::GyroProbe::State::Unreadable;
    p.reason = "DJI records its gyro in a metadata track whose layout DJI has not published";
    return p;
}

editor::GyroProbe none() {
    editor::GyroProbe p;
    p.reason = "this file carries no camera gyro data";
    return p;
}

/// A fake that gives the clip's media the shake's gyro.
class ShakeGyro final : public gyro::GyroServices {
public:
    std::expected<gyro::GyroTrack, Error> read(const ProjectState&, MediaId, int) override { return gyro_shake::gyroOfShake(-0.5, 2.5, 400.0); }
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

struct Fixture {
    Timeline tl{kPicture};
    ProjectController controller;
    ClipId clip;
    EffectInstanceId id;
    QString section;

    explicit Fixture(const editor::GyroProbe& probe) {
        tl.linkMatching();
        GyroAvailability::instance().setProbeForTests([probe](const std::string&) { return probe; });
        controller.resetProject(tl.state, tl.seq);
        clip = clipAt(*controller.snapshot(), tl.seq, "V1@0");
        const SequenceId seq = tl.seq;
        const ClipId target = clip;
        REQUIRE(controller.session()
                    .apply("Apply Effect", [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&target, 1), std::string(kWarpStabilizerEffect)); })
                    .has_value());
        id = controller.currentSequence()->findClip(clip)->clip().effects().front().id;
        section = QStringLiteral("e%1").arg(id.value);
        controller.selectClipById(clip, ProjectController::Replace);
        controller.setPlayhead(Ticks{});
    }
    ~Fixture() { GyroAvailability::instance().setProbeForTests(nullptr, true); }
    EffectControlsModel& fx() { return *controller.effectControls(); }
    void setParams(std::vector<EffectParam> params) {
        edit::EffectChange change;
        change.params = std::move(params);
        const SequenceId seq = tl.seq;
        const ClipId target = clip;
        const EffectInstanceId effect = id;
        REQUIRE(controller.session().apply("Set Effect", [&](const ProjectState& s) { return edit::setEffect(s, seq, target, effect, change); }).has_value());
    }
    QVariantMap row(const char* key) { return fx().row(section + QLatin1Char(':') + QLatin1String(key)); }
    int optionCount() { return row("method").value("options").toList().size(); }
    QString note() { return fx().row(section + QStringLiteral(":gyroNote")).value("text").toString(); }
};

} // namespace

TEST_CASE("Gyro UI: the Method menu offers Gyro for footage with gyro data, the Gyro settings show only with that Method", "[app][gyro][effectcontrols]") {
    {
        Fixture f(goPro());
        // A file with gyro data: the menu lists Gyro (camera metadata) last, the note says so, no Gyro settings yet.
        REQUIRE(f.optionCount() == 5);
        CHECK(f.row("method").value("options").toList().back().toMap().value("label").toString() == QStringLiteral("Gyro (camera metadata)"));
        CHECK(f.row("gyroOffset").isEmpty());
        CHECK(f.row("gyroSync").isEmpty());
        CHECK(f.note().contains(QStringLiteral("This clip has gyro data (GoPro HERO8 Black, 202 readings per second)")));

        // Choosing it: one undo step, the four settings appear, the note asks for an analysis.
        const std::size_t before = f.controller.session().undoCount();
        f.fx().setValue(f.section + QStringLiteral(":method"), 4.0);
        f.fx().endEdit();
        CHECK(f.controller.session().undoCount() == before + 1);
        for (const char* key : {"gyroSync", "gyroOffset", "gyroFov", "gyroReadout"}) {
            INFO(key);
            CHECK_FALSE(f.row(key).isEmpty());
        }
        CHECK(f.row("gyroSync").value("kind").toString() == QStringLiteral("choice"));
        CHECK(f.row("gyroFov").value("title").toString().contains(QStringLiteral("Field of View")));
        CHECK(f.note().startsWith(QStringLiteral("Gyro data: GoPro HERO8 Black, 202 readings per second. Analyze")));
        CHECK(f.optionCount() == 5);
        // Back to a picture-based method: the settings and the note go.
        f.fx().setValue(f.section + QStringLiteral(":method"), 1.0);
        f.fx().endEdit();
        CHECK(f.row("gyroOffset").isEmpty());
    }
    {
        Fixture f(none());
        // A file without gyro data never offers it, and says nothing.
        CHECK(f.optionCount() == 4);
        CHECK(f.note().isEmpty());
        CHECK_FALSE(f.row("method").isEmpty());
    }
    {
        Fixture f(unreadable());
        CHECK(f.optionCount() == 4);
        // Camera data Wabcut does not read says so, and that the picture-based methods are unaffected.
        CHECK(f.note().startsWith(QStringLiteral("No gyro for this clip: DJI records its gyro")));
        CHECK(f.note().endsWith(QStringLiteral("The picture-based methods work as usual.")));
        // A project that already uses the Gyro method keeps the choice and says why it cannot work.
        f.setParams({EffectParam{"method", ParamValue(4.0)}});
        CHECK(f.optionCount() == 5);
        CHECK(f.note().startsWith(QStringLiteral("No usable gyro data: DJI records its gyro")));
    }
}

TEST_CASE("Gyro UI: Analyze reads the camera's gyro, keys the effect in one undo step, and the note says how it was synced",
          "[app][gyro][stabilize]") {
    resetSettings();
    clearMessages();
    GyroAvailability::instance().setProbeForTests([](const std::string&) { return goPro(); });
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* stabilizer = shell.engine.singletonInstance<StabilizerBridge*>("Wabcut", "Stabilizer");
    REQUIRE(controller);
    REQUIRE(stabilizer);
    Timeline tl(kPicture);
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const SequenceId seq = tl.seq;
    REQUIRE(controller->session()
                .apply("Apply Effect", [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kWarpStabilizerEffect)); })
                .has_value());
    const EffectInstanceId id = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(id.value);
    {
        edit::EffectChange change;
        change.params = {EffectParam{"method", ParamValue(4.0)}, EffectParam{"gyroSync", ParamValue(0.0)},
                         EffectParam{"gyroFov", ParamValue(90.0)}, EffectParam{"framing", ParamValue(0.0)}};
        REQUIRE(controller->session().apply("Set Effect", [&](const ProjectState& s) { return edit::setEffect(s, seq, clip, id, change); }).has_value());
    }
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});

    CHECK(stabilizer->effectInfo(section).value(QStringLiteral("gyro")).toBool());
    CHECK(stabilizer->effectInfo(section).value(QStringLiteral("status")).toString().isEmpty());

    stabilizer->setGyroServices(std::make_shared<ShakeGyro>());
    stabilizer->setSynchronous(true);
    const std::size_t before = controller->session().undoCount();
    REQUIRE(stabilizer->analyze(section));
    INFO(stabilizer->message().toStdString());
    CHECK_FALSE(stabilizer->busy());
    CHECK(stabilizer->message().startsWith(QStringLiteral("Analysis complete")));
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Analyze Stabilization"));
    const EffectInstance& analysed = controller->currentSequence()->findClip(clip)->clip().effects().front();
    REQUIRE(analysed.find("corner.tl.x"));
    CHECK(analysed.find("corner.tl.x")->isCurve());
    REQUIRE(analysed.findData("gyro"));

    // The note under the header: what the file has and how it was used.
    EffectControlsModel& fx = *controller->effectControls();
    const QString note = fx.row(section + QStringLiteral(":gyroNote")).value("text").toString();
    INFO(note.toStdString());
    CHECK(note.startsWith(QStringLiteral("Gyro data: GoPro HERO8 Black, 202 readings per second.")));
    CHECK(note.contains(QStringLiteral("Auto-sync is off: offset +0 ms, lens 90 degrees (set by you).")));

    // Effect Controls shows it, wrapped, under the stabilizer's header; shot dark and light.
    REQUIRE_FALSE(shell.grab().isNull());
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(fx.rowOf(QStringLiteral("h:") + section) >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + section)), Q_ARG(int, 0)));
    spin(50);
    QQuickItem* noteItem = nullptr;
    REQUIRE(waitFor([&] {
        const QString name = QStringLiteral("effectNote_") + section;
        noteItem = findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == name && i->isVisible(); });
        return noteItem != nullptr;
    }));
    CHECK(noteItem->property("text").toString() == note);
    CHECK(noteItem->height() > 0);
    CHECK(noteItem->property("lineCount").toInt() >= 1);
    const auto shoot = [&](const char* name) {
        for (const bool dark : {true, false}) {
            shell.setAppearance(dark, "ultraviolet");
            REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
            QQuickItem* panel = shell.item("effectControls");
            REQUIRE(panel != nullptr);
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            const double dpr = image.devicePixelRatio();
            const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
            REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                        .save(outputDir() + QStringLiteral("/stabilizer-gyro%1-%2.png").arg(QLatin1String(name), dark ? QStringLiteral("dark") : QStringLiteral("light"))));
        }
        shell.setAppearance(true, "ultraviolet");
    };
    shoot("");
    // The smallest window: the note wraps inside the row, left of the keyframe lane, and nothing runs off the panel.
    shell.resize(1100, 700);
    REQUIRE_FALSE(shell.grab().isNull());
    // The list is short in a window this size: scrolled to the note, it shows it.
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(section + QStringLiteral(":gyroNote"))), Q_ARG(int, 0)));
    spin(50);
    REQUIRE(waitFor([&] {
        const QString name = QStringLiteral("effectNote_") + section;
        noteItem = findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == name && i->isVisible(); });
        return noteItem != nullptr;
    }));
    {
        QQuickItem* panel = shell.item("effectControls");
        auto* listItem = qobject_cast<QQuickItem*>(list);
        REQUIRE(panel != nullptr);
        REQUIRE(listItem != nullptr);
        const QRectF noteRect = noteItem->mapRectToScene(QRectF(0, 0, noteItem->width(), noteItem->height()));
        const QRectF panelRect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        const QRectF listRect = listItem->mapRectToScene(QRectF(0, 0, listItem->width(), listItem->height()));
        CHECK(noteRect.left() >= panelRect.left());
        CHECK(noteRect.right() <= panelRect.right());
        CHECK(noteRect.intersected(listRect).height() > 12.0); // the first lines of the note are in view
        CHECK(noteItem->property("lineCount").toInt() >= 2);   // wrapped, not elided
    }
    shoot("-narrow");
    shell.resize(kShotWidth, kShotHeight);

    // Undo takes the keys and the note's numbers back.
    REQUIRE(controller->undo());
    CHECK(controller->currentSequence()->findClip(clip)->clip().effects().front().find("corner.tl.x") == nullptr);
    CHECK(fx.row(section + QStringLiteral(":gyroNote")).value("text").toString().startsWith(QStringLiteral("Gyro data: GoPro HERO8 Black, 202 readings per second. Analyze")));
    GyroAvailability::instance().setProbeForTests(nullptr, true);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
