// 0.7 Effect Controls: the clip's own flags as rows (card #98's follow-up: Nearest Neighbor Scaling under Transform; card
// #136: a Blend Mode choice under Opacity and the Next / Previous Blend Mode shortcuts), each edit one undo step with a
// clear name, Reset on the section putting them back, and several selected clips stepped together; and card #57's pinned
// favourites (a Favorites effect is a header ready to Add on every clip that does not have it). Offscreen; no window.

#include "EffectControls.h"
#include "EffectPresetsBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Blend.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QQuickItem>
#include <QString>
#include <QVariantMap>

#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

constexpr const char* kPicture = "V2 |Bbbbbbbbbbbbbbbbbbbb|\n"
                                 "V1 |Aaaaaaaaaaaaaaaaaaaa|\n"
                                 "A1 |Aaaaaaaaaaaaaaaaaaaa|";

struct Fixture {
    Timeline tl{kPicture};
    ProjectController controller;
    ClipId v1;
    ClipId v2;

    explicit Fixture(bool link = true) {
        if (link)
            tl.linkMatching(); // a video clip then shows its audio partner's sections too
        controller.resetProject(tl.state, tl.seq);
        v1 = clipAt(*controller.snapshot(), tl.seq, "V1@0");
        v2 = clipAt(*controller.snapshot(), tl.seq, "V2@0");
        controller.selectClipById(v1, ProjectController::Replace);
        controller.setPlayhead(Ticks{});
    }
    EffectControlsModel& fx() { return *controller.effectControls(); }
    const Clip& clip(ClipId id) const { return controller.currentSequence()->findClip(id)->clip(); }
    std::size_t undoCount() { return controller.session().undoCount(); }
};

BlendMode modeAt(int index) { return blendModesInMenuOrder()[static_cast<std::size_t>(index)]; }

} // namespace

TEST_CASE("Effect Controls: Transform has a Nearest Neighbor Scaling switch (card 98 follow-up)", "[app][effectcontrols][nearest]") {
    Fixture f;
    const QString key = QStringLiteral("p:nearestNeighbor");
    QVariantMap row = f.fx().row(key);
    REQUIRE_FALSE(row.isEmpty());
    CHECK(row.value("kind").toString() == QStringLiteral("toggle"));
    CHECK(row.value("section").toString() == QStringLiteral("motion"));
    CHECK(row.value("title").toString() == QStringLiteral("Nearest Neighbor Scaling"));
    CHECK(row.value("value").toDouble() == 0.0);
    CHECK_FALSE(f.clip(f.v1).content().nearestNeighbor);

    // The same command and flag as Clip > Video Options: one undo step with its name.
    const std::size_t before = f.undoCount();
    f.fx().setValue(key, 1.0);
    f.fx().endEdit();
    CHECK(f.clip(f.v1).content().nearestNeighbor);
    CHECK(f.undoCount() == before + 1);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Nearest Neighbor Scaling"));
    CHECK(f.fx().row(key).value("value").toDouble() == 1.0);
    // Setting what it already is changes nothing (no empty undo step).
    f.fx().setValue(key, 1.0);
    f.fx().endEdit();
    CHECK(f.undoCount() == before + 1);
    // Undo and redo follow the row.
    REQUIRE(f.controller.undo());
    CHECK_FALSE(f.clip(f.v1).content().nearestNeighbor);
    CHECK(f.fx().row(key).value("value").toDouble() == 0.0);
    REQUIRE(f.controller.redo());
    CHECK(f.fx().row(key).value("value").toDouble() == 1.0);

    // The row's reset and the Transform section's Reset put it back; only the video clip has the row.
    f.fx().resetValue(key);
    CHECK_FALSE(f.clip(f.v1).content().nearestNeighbor);
    f.fx().setValue(key, 1.0);
    f.fx().endEdit();
    const std::size_t beforeReset = f.undoCount();
    f.fx().resetSection(QStringLiteral("motion"));
    CHECK_FALSE(f.clip(f.v1).content().nearestNeighbor);
    CHECK(f.undoCount() == beforeReset + 1); // one step with the position and scale reset
    REQUIRE(f.controller.undo());
    CHECK(f.clip(f.v1).content().nearestNeighbor);

    // A collapsed Transform section shows no row (as it shows no parameters).
    f.fx().setCollapsed(QStringLiteral("motion"), true);
    CHECK(f.fx().row(key).isEmpty());
    f.fx().setCollapsed(QStringLiteral("motion"), false);
    CHECK_FALSE(f.fx().row(key).isEmpty());

    // An audio clip alone (not linked to the video) has neither row.
    Fixture unlinked(false);
    unlinked.controller.selectClipById(clipAt(*unlinked.controller.snapshot(), unlinked.tl.seq, "A1@0"), ProjectController::Replace);
    CHECK(unlinked.fx().row(key).isEmpty());
    CHECK(unlinked.fx().row(QStringLiteral("p:blendMode")).isEmpty());
}

TEST_CASE("Effect Controls: Opacity has a Blend Mode choice (card 136)", "[app][effectcontrols][blend]") {
    Fixture f;
    const QString key = QStringLiteral("p:blendMode");
    QVariantMap row = f.fx().row(key);
    REQUIRE_FALSE(row.isEmpty());
    CHECK(row.value("kind").toString() == QStringLiteral("choice"));
    CHECK(row.value("section").toString() == QStringLiteral("opacity"));
    CHECK(row.value("options").toList().size() == static_cast<qsizetype>(kBlendModeCount));
    CHECK(row.value("options").toList().front().toMap().value("label").toString() == QStringLiteral("Normal"));
    CHECK(row.value("text").toString() == QStringLiteral("0"));
    CHECK(f.clip(f.v1).blendMode() == BlendMode::Normal);

    // Picking an option (its value is the menu index) sets the clip's blend mode: one step named "Blend Mode".
    const std::size_t before = f.undoCount();
    f.fx().setValue(key, 3.0);
    f.fx().endEdit();
    CHECK(f.clip(f.v1).blendMode() == modeAt(3));
    CHECK(f.undoCount() == before + 1);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Blend Mode"));
    CHECK(f.fx().row(key).value("text").toString() == QStringLiteral("3"));
    // The label of the picked option is the mode's Premiere name.
    CHECK(f.fx().row(key).value("options").toList()[3].toMap().value("label").toString().toStdString() ==
          std::string(displayName(modeAt(3))));
    // The same pick again, or an index past the list, is not an empty step; the last index clamps.
    f.fx().setValue(key, 3.0);
    f.fx().endEdit();
    CHECK(f.undoCount() == before + 1);
    f.fx().setValue(key, 99.0);
    f.fx().endEdit();
    CHECK(f.clip(f.v1).blendMode() == modeAt(static_cast<int>(kBlendModeCount) - 1));
    REQUIRE(f.controller.undo());
    CHECK(f.clip(f.v1).blendMode() == modeAt(3));

    // Reset of the row, and of the Opacity section (one step: opacity and blend mode together).
    f.fx().resetValue(key);
    CHECK(f.clip(f.v1).blendMode() == BlendMode::Normal);
    f.fx().setValue(key, 5.0);
    f.fx().endEdit();
    const std::size_t beforeReset = f.undoCount();
    f.fx().resetSection(QStringLiteral("opacity"));
    CHECK(f.clip(f.v1).blendMode() == BlendMode::Normal);
    CHECK(f.undoCount() == beforeReset + 1);
    REQUIRE(f.controller.undo());
    CHECK(f.clip(f.v1).blendMode() == modeAt(5));

    // A project saved and loaded keeps it (the model's own attribute): the clip says so in a copy of the state.
    CHECK(f.controller.snapshot()->findSequence(f.tl.seq)->findClip(f.v1)->clip().blendMode() == modeAt(5));
}

TEST_CASE("Effect Controls: Next and Previous Blend Mode step the menu order and wrap (card 136)", "[app][effectcontrols][blend]") {
    Fixture f;
    EffectControlsModel& fx = f.fx();
    CHECK(EffectControlsModel::handlesCommand(QStringLiteral("effectControls.nextBlendMode")));
    CHECK(EffectControlsModel::handlesCommand(QStringLiteral("effectControls.previousBlendMode")));

    // Forward from Normal through the whole list ends back on Normal, one undo step per press.
    const std::size_t before = f.undoCount();
    for (int i = 1; i <= static_cast<int>(kBlendModeCount); ++i) {
        CHECK(fx.runCommand(QStringLiteral("effectControls.nextBlendMode")));
        CHECK(f.clip(f.v1).blendMode() == modeAt(i % static_cast<int>(kBlendModeCount)));
    }
    CHECK(f.undoCount() == before + kBlendModeCount);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Blend Mode"));
    CHECK(f.clip(f.v1).blendMode() == BlendMode::Normal);

    // Backward from Normal wraps to the last mode; the row follows and the panel says which mode it is.
    CHECK(fx.runCommand(QStringLiteral("effectControls.previousBlendMode")));
    CHECK(f.clip(f.v1).blendMode() == modeAt(static_cast<int>(kBlendModeCount) - 1));
    CHECK(fx.row(QStringLiteral("p:blendMode")).value("text").toString() == QString::number(kBlendModeCount - 1));
    CHECK(fx.notice().contains(QString::fromUtf8(displayName(modeAt(static_cast<int>(kBlendModeCount) - 1)).data())));
    CHECK(fx.runCommand(QStringLiteral("effectControls.nextBlendMode")));
    CHECK(f.clip(f.v1).blendMode() == BlendMode::Normal);

    // Several selected video clips take the next mode of the primary one together, as one undo step; the audio partner
    // and unselected clips are left alone.
    f.controller.selectClipById(f.v1, ProjectController::Replace);
    f.controller.selectClipById(f.v2, ProjectController::Add);
    REQUIRE(fx.mode() == QStringLiteral("multi"));
    const std::size_t beforeMulti = f.undoCount();
    CHECK(fx.runCommand(QStringLiteral("effectControls.nextBlendMode")));
    CHECK(f.clip(f.v1).blendMode() == modeAt(1));
    CHECK(f.clip(f.v2).blendMode() == modeAt(1));
    CHECK(f.undoCount() == beforeMulti + 1);
    REQUIRE(f.controller.undo());
    CHECK(f.clip(f.v1).blendMode() == BlendMode::Normal);
    CHECK(f.clip(f.v2).blendMode() == BlendMode::Normal);

    // Nothing selected, or only audio: it says so and changes nothing.
    f.controller.clearSelection();
    const std::size_t beforeNone = f.undoCount();
    CHECK(fx.runCommand(QStringLiteral("effectControls.nextBlendMode"))); // handled (the key is spent), nothing stepped
    CHECK(f.undoCount() == beforeNone);
    CHECK(fx.notice() == QStringLiteral("Select a video clip first"));
}

// Card 57: a favourite effect is pinned in Effect Controls as a header ready to use on every clip that does not have it.
TEST_CASE("Effect Controls: a favourite effect is pinned as a header ready to Add (card 57)", "[app][effectcontrols][pinned]") {
    sharedEffectLibrary()->set({});
    Fixture f(false); // video and audio clips apart: each panel shows one kind
    EffectPresetsBridge presets;
    presets.setController(&f.controller);
    presets.setPersistent(false);
    EffectControlsModel& fx = f.fx();
    const QString blur = QStringLiteral("h:pin:wabcut.blur.gaussian");

    // No favourites: no pinned headers.
    CHECK(fx.rowOf(blur) < 0);

    // Add to Favorites (the Effects panel's menu): a pinned header on a video clip, collapsed, with the effect's name.
    REQUIRE(presets.setFavorite(QStringLiteral("effect:wabcut.blur.gaussian"), true).isEmpty());
    fx.refreshPinned(); // the panel does this on EffectPresets.changed
    QVariantMap header = fx.row(blur);
    REQUIRE_FALSE(header.isEmpty());
    CHECK(header.value("kind").toString() == QStringLiteral("header"));
    CHECK(header.value("sectionKind").toString() == QStringLiteral("pinned"));
    CHECK(header.value("section").toString() == QStringLiteral("pin:wabcut.blur.gaussian"));
    CHECK(header.value("title").toString() == QStringLiteral("Gaussian Blur"));
    CHECK(header.value("removable").toBool() == false);
    CHECK(header.value("movable").toBool() == false);
    CHECK(fx.effectIdOfSection(QStringLiteral("pin:wabcut.blur.gaussian")) == QStringLiteral("wabcut.blur.gaussian"));
    // It comes after the clip's own stack (here: after Opacity), before nothing else of the video part.
    CHECK(fx.rowOf(blur) > fx.rowOf(QStringLiteral("p:blendMode")));
    // An audio favourite shows on audio clips only; a transition or what is not an effect is never pinned.
    REQUIRE(presets.setFavorite(QStringLiteral("effect:wabcut.audio.gain"), true).isEmpty());
    REQUIRE(presets.setFavorite(QStringLiteral("effect:crossDissolve"), true).isEmpty());
    REQUIRE(presets.setFavorite(QStringLiteral("preset:Twirl In"), true).isEmpty());
    fx.refreshPinned();
    CHECK(fx.rowOf(QStringLiteral("h:pin:wabcut.audio.gain")) < 0);
    CHECK(fx.rowOf(QStringLiteral("h:pin:crossDissolve")) < 0);
    CHECK(fx.rowOf(QStringLiteral("h:pin:preset:Twirl In")) < 0);

    // Add puts the effect on the clip: one undo step, the pinned header is replaced by the real section.
    const std::size_t before = f.undoCount();
    CHECK(fx.addPinned(QStringLiteral("pin:wabcut.blur.gaussian")).isEmpty());
    CHECK(f.undoCount() == before + 1);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Add Effect"));
    REQUIRE(f.clip(f.v1).effects().size() == 1);
    CHECK(f.clip(f.v1).effects()[0].effect == "wabcut.blur.gaussian");
    CHECK(fx.rowOf(blur) < 0);
    const QString real = QStringLiteral("h:e") + QString::number(f.clip(f.v1).effects()[0].id.value);
    CHECK(fx.rowOf(real) >= 0);
    CHECK(fx.effectIdOfSection(real.mid(2)) == QStringLiteral("wabcut.blur.gaussian"));
    // Undo takes it off and the pinned header is back.
    REQUIRE(f.controller.undo());
    CHECK(f.clip(f.v1).effects().empty());
    CHECK(fx.rowOf(blur) >= 0);

    // The audio clip's panel pins the audio favourite and not the video one.
    f.controller.selectClipById(clipAt(*f.controller.snapshot(), f.tl.seq, "A1@0"), ProjectController::Replace, false);
    CHECK(fx.rowOf(QStringLiteral("h:pin:wabcut.audio.gain")) >= 0);
    CHECK(fx.rowOf(blur) < 0);
    CHECK(fx.addPinned(QStringLiteral("pin:wabcut.audio.gain")).isEmpty());
    CHECK(fx.rowOf(QStringLiteral("h:pin:wabcut.audio.gain")) < 0);
    f.controller.selectClipById(f.v1, ProjectController::Replace, false);

    // Several clips (multi mode) show no pinned headers; a wrong key and a missing clip say so.
    f.controller.selectClipById(f.v1, ProjectController::Replace);
    f.controller.selectClipById(f.v2, ProjectController::Add);
    CHECK(fx.rowOf(blur) < 0);
    f.controller.selectClipById(f.v1, ProjectController::Replace);
    CHECK_FALSE(fx.addPinned(QStringLiteral("e12")).isEmpty());
    f.controller.clearSelection();
    CHECK_FALSE(fx.addPinned(QStringLiteral("pin:wabcut.blur.gaussian")).isEmpty());
    f.controller.selectClipById(f.v1, ProjectController::Replace);

    // Unpin: the header goes with the favourite.
    REQUIRE(presets.setFavorite(QStringLiteral("effect:wabcut.blur.gaussian"), false).isEmpty());
    fx.refreshPinned();
    CHECK(fx.rowOf(blur) < 0);
    sharedEffectLibrary()->set({});
}

TEST_CASE("Effect Controls: Transform has Scale Width and a Uniform Scale switch (card 12)", "[app][effectcontrols][scalewidth]") {
    Fixture f;
    const QString uniform = QStringLiteral("p:uniformScale");
    const QString width = QStringLiteral("p:scaleWidth");
    const QString scale = QStringLiteral("p:scale");

    // Uniform on, the default: one Scale row and the switch, ticked; no Scale Width row.
    CHECK(f.fx().row(scale).value("title").toString() == QStringLiteral("Scale"));
    const QVariantMap sw = f.fx().row(uniform);
    REQUIRE_FALSE(sw.isEmpty());
    CHECK(sw.value("kind").toString() == QStringLiteral("toggle"));
    CHECK(sw.value("section").toString() == QStringLiteral("motion"));
    CHECK(sw.value("title").toString() == QStringLiteral("Uniform Scale"));
    CHECK(sw.value("value").toDouble() == 1.0);
    CHECK_FALSE(sw.value("animatable").toBool()); // a switch has no stopwatch
    CHECK(f.fx().row(width).isEmpty());
    CHECK(f.fx().rowOf(uniform) > f.fx().rowOf(scale)); // Premiere's order: Scale, [Scale Width], Uniform Scale, Rotation

    // Switch it off: Scale becomes Scale Height, Scale Width appears with the same value; one undo step with its name.
    const std::size_t before = f.undoCount();
    f.fx().setValue(uniform, 0.0);
    f.fx().endEdit();
    CHECK(f.undoCount() == before + 1);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Uniform Scale"));
    CHECK_FALSE(f.clip(f.v1).properties().uniformScale());
    CHECK(f.fx().row(scale).value("title").toString() == QStringLiteral("Scale Height"));
    const QVariantMap w = f.fx().row(width);
    REQUIRE_FALSE(w.isEmpty());
    CHECK(w.value("title").toString() == QStringLiteral("Scale Width"));
    CHECK(w.value("kind").toString() == QStringLiteral("param"));
    CHECK(w.value("value").toDouble() == 100.0);
    CHECK(w.value("animatable").toBool());
    CHECK(f.fx().rowOf(width) < f.fx().rowOf(uniform));
    CHECK(f.fx().row(uniform).value("value").toDouble() == 0.0);

    // The width and the height now move on their own; each edit is one undo step.
    f.fx().setValue(width, 250.0);
    f.fx().endEdit();
    CHECK(f.clip(f.v1).properties().scaleAt() == std::pair<double, double>{2.5, 1.0});
    f.fx().setValue(scale, 50.0);
    f.fx().endEdit();
    CHECK(f.clip(f.v1).properties().scaleAt() == std::pair<double, double>{2.5, 0.5});
    CHECK(f.undoCount() == before + 3);

    // A stopwatch on Scale Width keys only the width.
    const std::size_t keyed = f.undoCount();
    f.fx().setAnimated(width, true);
    CHECK(f.clip(f.v1).properties()[ParamId::ScaleWidth].isCurve());
    CHECK_FALSE(f.clip(f.v1).properties()[ParamId::Scale].isCurve());
    CHECK(f.undoCount() == keyed + 1);

    // Reset on Transform puts the switch and the width back (one step), the row for the width goes.
    f.fx().resetSection(QStringLiteral("motion"));
    CHECK(f.clip(f.v1).properties().uniformScale());
    CHECK(f.clip(f.v1).properties().isDefault(ParamId::ScaleWidth));
    CHECK(f.clip(f.v1).properties().isDefault(ParamId::Scale));
    CHECK(f.fx().row(width).isEmpty());
    CHECK(f.fx().row(scale).value("title").toString() == QStringLiteral("Scale"));
}

// Cards 57, 98, 136 in the real Effect Controls panel (QML): the Nearest Neighbor Scaling switch under Transform, the Blend
// Mode menu under Opacity (a pick changes the clip) and a pinned favourite with its Add button (a click adds the effect),
// with screenshots in the dark and light themes.
TEST_CASE("Effect Controls in the shell: Blend Mode, Nearest Neighbor and a pinned favourite (screenshots)",
          "[app][effectcontrols][qml][screenshots]") {
    resetSettings();
    clearMessages();
    sharedEffectLibrary()->set({});
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* presets = shell.engine.singletonInstance<EffectPresetsBridge*>("Wabcut", "EffectPresets");
    REQUIRE(controller);
    REQUIRE(presets);
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    const ClipId v1 = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    controller->selectClipById(v1, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    EffectControlsModel& fx = *controller->effectControls();
    REQUIRE(presets->setFavorite(QStringLiteral("effect:wabcut.blur.gaussian"), true).isEmpty());
    REQUIRE_FALSE(shell.grab().isNull());
    spin(100);

    const std::function<QQuickItem*(QQuickItem*, const QString&)> findRow = [&](QQuickItem* item, const QString& key) -> QQuickItem* {
        if (item->property("rowKey").toString() == key && item->property("kind").isValid() && item->isVisible())
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = findRow(child, key))
                return found;
        }
        return nullptr;
    };
    const std::function<QQuickItem*(QQuickItem*, const QString&)> findNamedItem = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
        if (item->objectName() == name && item->isVisible())
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = findNamedItem(child, name))
                return found;
        }
        return nullptr;
    };
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    const auto reveal = [&](const QString& key) {
        const int row = fx.rowOf(key);
        REQUIRE(row >= 0);
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, row), Q_ARG(int, 2))); // ListView.Contain
        REQUIRE_FALSE(shell.grab().isNull());
        spin(40);
    };
    const auto saveTheme = [&](const char* name) {
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
                        .save(outputDir() + QStringLiteral("/effect-controls-%1-%2.png").arg(QLatin1String(name), dark ? QStringLiteral("dark") : QStringLiteral("light"))));
        }
        shell.setAppearance(true, "ultraviolet");
    };

    // 0.7 (card #12): Uniform Scale is a real switch row; switching it off shows Scale Height and Scale Width.
    {
        const QString uniformKey = QStringLiteral("p:uniformScale");
        reveal(uniformKey);
        QQuickItem* uniformRow = findRow(shell.window->contentItem(), uniformKey);
        REQUIRE(uniformRow != nullptr);
        CHECK(uniformRow->property("kind").toString() == QStringLiteral("toggle"));
        CHECK(uniformRow->property("title").toString() == QStringLiteral("Uniform Scale"));
        CHECK(findRow(shell.window->contentItem(), QStringLiteral("p:scaleWidth")) == nullptr); // uniform: no width row
        saveTheme("uniform-scale");
        QQuickItem* toggle = findNamedItem(uniformRow, QStringLiteral("paramToggle"));
        REQUIRE(toggle != nullptr);
        CHECK(toggle->property("checked").toBool());
        const std::size_t steps = controller->session().undoCount();
        REQUIRE(toggle->setProperty("checked", false)); // what the pointer does, then the signal the row listens to
        REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
        spin(80);
        CHECK(controller->session().undoCount() == steps + 1);
        CHECK_FALSE(controller->currentSequence()->findClip(v1)->clip().properties().uniformScale());
        reveal(QStringLiteral("p:scaleWidth"));
        QQuickItem* widthRow = findRow(shell.window->contentItem(), QStringLiteral("p:scaleWidth"));
        REQUIRE(widthRow != nullptr);
        CHECK(widthRow->property("title").toString() == QStringLiteral("Scale Width"));
        QQuickItem* heightRow = findRow(shell.window->contentItem(), QStringLiteral("p:scale"));
        REQUIRE(heightRow != nullptr);
        CHECK(heightRow->property("title").toString() == QStringLiteral("Scale Height"));
        saveTheme("scale-width");
        fx.setValue(uniformKey, 1.0); // back to a plain clip for the rest
        fx.endEdit();
        spin(40);
    }

    // Transform ends with the switch, Opacity with the Blend Mode menu: real rows of the panel.
    reveal(QStringLiteral("p:nearestNeighbor"));
    QQuickItem* nearest = findRow(shell.window->contentItem(), QStringLiteral("p:nearestNeighbor"));
    REQUIRE(nearest != nullptr);
    CHECK(nearest->property("title").toString() == QStringLiteral("Nearest Neighbor Scaling"));
    // QA: the row of a switch gives its label the room a slider would have had: the whole name is shown, not "Nearest N...".
    QQuickItem* nearestLabel = findNamedItem(nearest, QStringLiteral("paramLabel"));
    REQUIRE(nearestLabel != nullptr);
    CHECK(nearestLabel->property("text").toString() == QStringLiteral("Nearest Neighbor Scaling"));
    CHECK_FALSE(nearestLabel->property("truncated").toBool());
    CHECK(nearestLabel->width() >= nearestLabel->property("implicitWidth").toDouble());
    saveTheme("nearest-neighbor");
    reveal(QStringLiteral("p:blendMode"));
    QQuickItem* blend = findRow(shell.window->contentItem(), QStringLiteral("p:blendMode"));
    REQUIRE(blend != nullptr);
    CHECK(blend->property("kind").toString() == QStringLiteral("choice"));
    CHECK(findNamedItem(blend, QStringLiteral("choiceMenuButton")) != nullptr); // 27 modes: a menu, not pills
    // Picking an option the way the menu does (the row's chooseOption) sets the clip's mode.
    REQUIRE(QMetaObject::invokeMethod(blend, "chooseOption", Q_ARG(QVariant, QStringLiteral("3"))));
    CHECK(controller->currentSequence()->findClip(v1)->clip().blendMode() == blendModesInMenuOrder()[3]);
    spin(60);
    reveal(QStringLiteral("p:blendMode"));
    saveTheme("blend-mode");

    // The pinned favourite: a header at the end of the stack with Add; a click adds the effect, and the header goes.
    const QString pinned = QStringLiteral("h:pin:wabcut.blur.gaussian");
    reveal(pinned);
    QQuickItem* header = findRow(shell.window->contentItem(), pinned);
    REQUIRE(header != nullptr);
    CHECK(header->property("title").toString() == QStringLiteral("Gaussian Blur"));
    QQuickItem* add = findNamedItem(header, QStringLiteral("pinnedAdd"));
    REQUIRE(add != nullptr);
    CHECK(findNamedItem(header, QStringLiteral("pinnedUnpin")) != nullptr);
    CHECK(findNamedItem(header, QStringLiteral("sectionChevron")) == nullptr); // nothing to collapse yet
    saveTheme("pinned-favourite");
    const std::size_t before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(add, "clicked"));
    spin(60);
    CHECK(controller->session().undoCount() == before + 1);
    REQUIRE(controller->currentSequence()->findClip(v1)->clip().effects().size() == 1);
    CHECK(fx.rowOf(pinned) < 0);
    // Unpin from the real section's menu text path: the favourite goes, nothing is pinned any more.
    REQUIRE(presets->setFavorite(QStringLiteral("effect:wabcut.blur.gaussian"), false).isEmpty());
    sharedEffectLibrary()->set({});

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("EffectControlsRow"), Qt::CaseInsensitive));
}
