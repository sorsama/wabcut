// 0.6 (card #71): Master Clip Effects in Effect Controls and the Effects panel. Apply to Master Clip puts an effect on
// the media of the selected clips (one undo step); a timeline clip shows a "Master Clip: <name>" section above its own
// effects, whose rows edit every instance at once; the caption's switch turns them off for one clip; a section moves to
// the master clip and back. Offscreen; screenshots of the section in the dark and light themes land in
// <build>/test-output/ui/.

#include "BridgeIds.h"
#include "EffectControls.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QQuickItem>

#include <functional>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// Two clips of A (picture and sound, linked) around a clip of B.
constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbbA100aaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbbA100aaaaaaaaaaaaaaaaaaaaaaaa|
)";

QStringList rowKeys(const EffectControlsModel& m) {
    QStringList keys;
    for (int i = 0; i < m.rowCount(); ++i)
        keys << m.keyAt(i);
    return keys;
}

std::vector<std::string> idsOf(const EffectStack& stack) {
    std::vector<std::string> out;
    for (const EffectInstance& e : stack)
        out.push_back(e.effect);
    return out;
}

struct Rig {
    Timeline tl{kPicture};
    ProjectController controller;
    ClipId first;
    ClipId second;
    MediaId media;

    Rig() {
        tl.linkMatching();
        first = tl.clip("V1@0");
        second = tl.clip("V1@50");
        media = tl.media('A');
        controller.resetProject(tl.state, tl.seq);
        controller.selectClip(clipKey(first));
    }
    EffectControlsModel& fx() { return *controller.effectControls(); }
    std::size_t undo() { return controller.session().undoCount(); }
    const MediaItem& item() { return *controller.snapshot()->findMedia(media); }
    const Clip& clip(ClipId id) {
        const auto loc = controller.currentSequence()->findClip(id);
        REQUIRE(loc);
        return loc->clip();
    }
    std::vector<std::string> played(ClipId id, EffectKind kind = EffectKind::Video) {
        return idsOf(playedEffects(*controller.snapshot(), clip(id), kind));
    }
    QString caption(ClipId id) const { return QStringLiteral("master:") + QString::number(id.value); }
};

using Ids = std::vector<std::string>;

} // namespace

TEST_CASE("Effect Controls Master Clip section: Apply to Master Clip and edits on every instance one undo step each (0.6)",
          "[app][effects][master]") {
    Rig r;
    EffectControlsModel& fx = r.fx();
    r.controller.setSelection({r.first});

    // The Effects panel's Apply to Master Clip: the selected clip's media gets it, one step.
    std::size_t before = r.undo();
    QVariantMap result = fx.applyToSelectedClips(QStringLiteral("master:wabcut.blur.gaussian"));
    REQUIRE(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("clips")).toInt() == 1);
    CHECK(result.value(QStringLiteral("message")).toString() == QStringLiteral("Added Gaussian Blur to the master clip"));
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Add Master Clip Effect"));
    REQUIRE(r.item().masterEffects.size() == 1);
    CHECK(r.clip(r.first).effects().empty());
    CHECK(r.played(r.second) == Ids{"wabcut.blur.gaussian"});
    const QString blur = effectKey(r.item().masterEffects.front().id);

    // An own effect plays after it: the Master Clip caption and its section sit above the clip's own sections.
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.color.tint")).isEmpty());
    const QString tint = effectKey(r.clip(r.first).effects().front().id);
    QStringList keys = rowKeys(fx);
    const auto caption = keys.indexOf(r.caption(r.first));
    REQUIRE(caption >= 0);
    CHECK(keys.indexOf(QStringLiteral("h:opacity")) < caption);
    CHECK(caption < keys.indexOf(QStringLiteral("h:") + blur));
    CHECK(keys.indexOf(QStringLiteral("h:") + blur) < keys.indexOf(QStringLiteral("h:") + tint));
    CHECK(fx.row(r.caption(r.first)).value(QStringLiteral("title")).toString() == QStringLiteral("Master Clip: A"));
    CHECK(fx.row(r.caption(r.first)).value(QStringLiteral("sectionKind")).toString() == QStringLiteral("master"));
    CHECK(fx.row(QStringLiteral("h:") + blur).value(QStringLiteral("stackKind")).toString() == QStringLiteral("master"));
    CHECK(fx.row(QStringLiteral("h:") + tint).value(QStringLiteral("stackKind")).toString() == QStringLiteral("video"));
    // B's clip has no master clip effects: no caption.
    r.controller.selectClip(clipKey(r.tl.clip("V1@25")));
    CHECK_FALSE(rowKeys(fx).contains(r.caption(r.tl.clip("V1@25"))));

    // A value set on the second instance changes the master clip: the first shows it too. One step.
    r.controller.selectClip(clipKey(r.second));
    REQUIRE(rowKeys(fx).contains(QStringLiteral("h:") + blur));
    before = r.undo();
    fx.setValue(blur + QStringLiteral(":radius"), 24.0);
    fx.endEdit();
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText().startsWith(QStringLiteral("Undo Master Clip: Gaussian Blur")));
    r.controller.selectClip(clipKey(r.first));
    CHECK(fx.row(blur + QStringLiteral(":radius")).value(QStringLiteral("value")).toDouble() == 24.0);
    REQUIRE(r.controller.undo());
    CHECK(fx.row(blur + QStringLiteral(":radius")).value(QStringLiteral("value")).toDouble() == 0.0);
    REQUIRE(r.controller.redo());

    // The caption's switch: off for the first clip only, one step, and on again.
    before = r.undo();
    fx.rowAction(r.caption(r.first), QStringLiteral("masterOff"));
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Turn Off Master Clip Effects"));
    CHECK(r.clip(r.first).content().masterEffectsOff);
    CHECK(r.played(r.first) == Ids{"wabcut.color.tint"});
    CHECK(r.played(r.second) == Ids{"wabcut.blur.gaussian"});
    CHECK(fx.row(r.caption(r.first)).value(QStringLiteral("title")).toString() == QStringLiteral("Master Clip: A (off for this clip)"));
    CHECK(fx.row(r.caption(r.first)).value(QStringLiteral("text")).toString() == QStringLiteral("masterOn"));
    CHECK_FALSE(fx.row(QStringLiteral("h:") + blur).value(QStringLiteral("effectEnabled")).toBool());
    fx.rowAction(r.caption(r.first), QStringLiteral("masterOn"));
    CHECK_FALSE(r.clip(r.first).content().masterEffectsOff);
    CHECK(r.played(r.first) == Ids{"wabcut.blur.gaussian", "wabcut.color.tint"});

    // Move to This Clip and Move to Master Clip (the section menus), one step each.
    before = r.undo();
    fx.rowAction(QStringLiteral("h:") + blur, QStringLiteral("toClip"));
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Move to Clip"));
    CHECK(r.item().masterEffects.empty());
    CHECK(idsOf(r.clip(r.first).effects()) == Ids{"wabcut.blur.gaussian", "wabcut.color.tint"});
    CHECK(r.played(r.second).empty());
    CHECK_FALSE(rowKeys(fx).contains(r.caption(r.first)));
    fx.rowAction(QStringLiteral("h:") + tint, QStringLiteral("toMaster"));
    CHECK(r.controller.undoText() == QStringLiteral("Undo Move to Master Clip"));
    CHECK(idsOf(r.item().masterEffects) == Ids{"wabcut.color.tint"});
    CHECK(r.played(r.second) == Ids{"wabcut.color.tint"});
    REQUIRE(r.controller.undo());
    CHECK(r.item().masterEffects.empty());

    // The sound's master clip effects: an audio effect goes on the media's audio, shown on the audio clips.
    r.controller.setSelection({r.first});
    result = fx.applyToSelectedClips(QStringLiteral("master:effect:wabcut.audio.gain"));
    REQUIRE(result.value(QStringLiteral("ok")).toBool());
    CHECK(r.played(r.tl.clip("A1@50"), EffectKind::Audio) == Ids{"wabcut.audio.gain"});
    CHECK(r.played(r.second).empty());

    // Not for transitions or with nothing selected.
    CHECK_FALSE(fx.applyToSelectedClips(QStringLiteral("master:crossDissolve")).value(QStringLiteral("ok")).toBool());
    r.controller.setSelection({});
    CHECK_FALSE(fx.applyToSelectedClips(QStringLiteral("master:wabcut.blur.gaussian")).value(QStringLiteral("ok")).toBool());
}

TEST_CASE("Effect Controls Master Clip section: a preset goes on the master clip with its keys (0.6)", "[app][effects][master]") {
    Rig r;
    EffectControlsModel& fx = r.fx();
    r.controller.setSelection({r.first, r.second});
    const std::size_t before = r.undo();
    const QVariantMap result = fx.applyToSelectedClips(QStringLiteral("master:preset:Twirl In"));
    REQUIRE(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("clips")).toInt() == 1); // both clips are of A: one master clip
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Apply Preset to Master Clip"));
    REQUIRE(r.item().masterEffects.size() == 1);
    CHECK(r.item().masterEffects.front().effect == "wabcut.distort.twirl");
    const ParamValue* angle = r.item().masterEffects.front().find("angle");
    REQUIRE(angle);
    CHECK(angle->isCurve());
    CHECK(r.clip(r.first).effects().empty());
    CHECK(r.clip(r.second).effects().empty());
    CHECK(r.played(r.second) == Ids{"wabcut.distort.twirl"});
}

TEST_CASE("Library: the fx badge counts the Master Clip Effects of an item (0.6)", "[app][effects][master][library]") {
    Rig r;
    LibraryBridge library;
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setAnalysisProbe({});
    library.setController(&r.controller);
    const auto badge = [&](char letter) {
        LibraryModel* items = library.items();
        const int row = items->rowOf(mediaKey(r.tl.media(letter)));
        REQUIRE(row >= 0);
        return items->data(items->index(row, 0), LibraryModel::MasterEffectsRole).toInt();
    };
    CHECK(badge('A') == 0);
    r.controller.setSelection({r.first});
    REQUIRE(r.fx().applyToSelectedClips(QStringLiteral("master:wabcut.blur.gaussian")).value(QStringLiteral("ok")).toBool());
    REQUIRE(r.fx().applyToSelectedClips(QStringLiteral("master:effect:wabcut.audio.gain")).value(QStringLiteral("ok")).toBool());
    CHECK(badge('A') == 2);
    CHECK(badge('B') == 0);
    REQUIRE(r.controller.undo());
    CHECK(badge('A') == 1);
}

namespace {

void masterSectionShot(bool dark) {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, 1280);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl{kPicture};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId first = tl.clip("V1@0");
    controller->setSelection({first});
    EffectControlsModel& fx = *controller->effectControls();
    REQUIRE(fx.applyToSelectedClips(QStringLiteral("master:wabcut.stylize.glow")).value(QStringLiteral("ok")).toBool());
    controller->selectClip(clipKey(first));
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.color.tint")).isEmpty());
    fx.setCollapsed(QStringLiteral("motion"), true);
    fx.setCollapsed(QStringLiteral("opacity"), true);
    if (!dark) {
        shell.setAppearance(false, "ultraviolet");
        REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    }
    spin(300);
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex",
                                      Q_ARG(int, fx.rowOf(QStringLiteral("master:") + QString::number(first.value))), Q_ARG(int, 0)));
    spin(150);
    QQuickItem* panel = shell.item("effectControls");
    REQUIRE(panel != nullptr);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    REQUIRE(image.copy(QRectF(rect.topLeft() * dpr, rect.size() * dpr).toRect())
                .save(outputDir() + (dark ? QStringLiteral("/effect-controls-master-dark.png")
                                          : QStringLiteral("/effect-controls-master-light.png"))));
    const QStringList messages = takeMessages();
    INFO(messages.join(QLatin1Char('\n')).toStdString());
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("Effect Controls Master Clip section screenshot in the dark theme (0.6)", "[app][screenshots][master]") {
    masterSectionShot(true);
}

TEST_CASE("Effect Controls Master Clip section screenshot in the light theme (0.6)", "[app][screenshots][master]") {
    masterSectionShot(false);
}

TEST_CASE("Keyframe clipboard: keys of a Master Clip Effect copy and paste like a clip effect's (fix F1A)",
          "[app][effects][master][keyframes][clipboard]") {
    // ARGON-09: Copy found no effect for a master clip effect's keys (it looked in the clip's own stack), so Paste said
    // "This clip has none of the copied parameters" everywhere.
    Rig r;
    EffectControlsModel& fx = r.fx();
    r.controller.setSelection({r.first});
    REQUIRE(fx.applyToSelectedClips(QStringLiteral("master:wabcut.blur.gaussian")).value(QStringLiteral("ok")).toBool());
    const EffectInstanceId blurId = r.item().masterEffects.front().id;
    const QString radius = effectKey(blurId) + QStringLiteral(":radius");
    r.controller.setPlayhead(f(2));
    fx.setAnimated(radius, true);
    r.controller.setPlayhead(f(6));
    fx.setValue(radius, 40.0);
    fx.endEdit();
    const auto masterRadius = [&]() -> const ParamValue* { return r.item().masterEffects.front().find("radius"); };
    REQUIRE(masterRadius());
    REQUIRE(masterRadius()->isCurve());
    REQUIRE(masterRadius()->curve()->keys.size() == 2);
    fx.selectKeysInRange({radius}, 0.0, 1.0, 0);
    REQUIRE(fx.selectedKeyCount() == 2);
    REQUIRE(fx.copySelectedKeys() == 2);

    // B's clip has a Gaussian Blur of its own: the keys go there, with their spacing and values. One step.
    const ClipId bClip = r.tl.clip("V1@25");
    r.controller.selectClip(clipKey(bClip));
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    const Ticks bStart = r.clip(bClip).start();
    r.controller.setPlayhead(bStart + f(1));
    std::size_t before = r.undo();
    REQUIRE(fx.pasteKeys().isEmpty());
    CHECK(r.undo() == before + 1);
    const ParamValue* pasted = r.clip(bClip).effects().back().find("radius");
    REQUIRE(pasted);
    REQUIRE(pasted->isCurve());
    REQUIRE(pasted->curve()->keys.size() == 2);
    CHECK(pasted->curve()->keys[0].time == r.clip(bClip).curveTimeAt(bStart + f(1)));
    CHECK(pasted->curve()->keys[1].time == r.clip(bClip).curveTimeAt(bStart + f(5)));
    CHECK(pasted->curve()->keys[1].value == Catch::Approx(40.0));

    // On A's second instance they go back to the master clip's Gaussian Blur (every instance), not onto the clip.
    r.controller.selectClip(clipKey(r.second));
    r.controller.setPlayhead(r.clip(r.second).start() + f(10));
    before = r.undo();
    REQUIRE(fx.pasteKeys().isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(masterRadius()->curve()->keys.size() == 4);
    CHECK(r.clip(r.second).effects().empty());
    REQUIRE(r.controller.undo());
    CHECK(masterRadius()->curve()->keys.size() == 2);

    // Cut works on them too (one step), and they paste back.
    r.controller.selectClip(clipKey(r.first));
    fx.selectKeysInRange({radius}, 0.0, 1.0, 0);
    REQUIRE(fx.copySelectedKeys(true) == 2);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Cut Keyframes"));
    CHECK_FALSE(masterRadius()->isCurve());
    r.controller.setPlayhead(f(2));
    REQUIRE(fx.pasteKeys().isEmpty());
    REQUIRE(masterRadius()->isCurve());
    CHECK(masterRadius()->curve()->keys.size() == 2);
}

TEST_CASE("Source Monitor: the media's Master Clip Effects show there and follow their edits (fix F1A)",
          "[app][effects][master][monitor]") {
    // ARGON-21 / ARGON-06: the Source Monitor's private copy kept the master clip effects of the moment it loaded (with
    // the project's ids, which collide there), never a later edit, and a subclip lost its master item's.
    Rig r;
    // A subclip of A, for the end.
    auto made = edit::makeSubclip(*r.controller.snapshot(), r.media, TimeRange{f(5), f(10)}, "Take");
    REQUIRE(made);
    const MediaId sub = made->id;
    r.controller.resetProject(made->state, r.tl.seq);
    SourceMonitorBridge source;
    source.setController(&r.controller);
    REQUIRE(source.load(mediaKey(r.media)));
    CHECK(source.shownMasterEffects().empty());

    // Added on the master clip: shown, under an id of the monitor's own project.
    REQUIRE(r.controller.applyEditFn("Add Master Clip Effect", [&](const ProjectState& s) {
        return edit::addEffect(s, r.tl.seq, edit::EffectTarget::ofMedia(r.media), "wabcut.blur.gaussian",
                               {EffectParam{"radius", ParamValue(12.0)}});
    }));
    const EffectInstanceId blurId = r.item().masterEffects.front().id;
    REQUIRE(source.shownMasterEffects().size() == 1);
    CHECK(source.shownMasterEffects().front().effect == "wabcut.blur.gaussian");
    CHECK(source.shownMasterEffects().front().id.isValid());
    CHECK(source.shownMasterEffects().front().find("radius")->constant() == Catch::Approx(12.0));

    // Edited through a timeline instance: the monitor follows.
    REQUIRE(r.controller.applyEditFn("Radius", [&](const ProjectState& s) {
        return edit::setParameter(s, r.tl.seq, r.first, edit::ParamRef::ofEffect(blurId, "radius"), ParamValue(30.0));
    }));
    REQUIRE(source.shownMasterEffects().size() == 1);
    CHECK(source.shownMasterEffects().front().find("radius")->constant() == Catch::Approx(30.0));
    // The clip switch is the timeline clip's: the source (the media) still shows them.
    const ClipId first[] = {r.first};
    REQUIRE(r.controller.applyEditFn("Off", [&](const ProjectState& s) { return edit::setMasterEffectsOff(s, r.tl.seq, first, true); }));
    CHECK(source.shownMasterEffects().size() == 1);
    // Undone back to none: gone from the monitor too.
    REQUIRE(r.controller.undo());
    REQUIRE(r.controller.undo());
    REQUIRE(r.controller.undo());
    CHECK(source.shownMasterEffects().empty());
    REQUIRE(r.controller.redo());

    // The subclip of A plays A's master clip effects first.
    REQUIRE(source.load(mediaKey(sub)));
    REQUIRE(source.shownMasterEffects().size() == 1);
    CHECK(source.shownMasterEffects().front().effect == "wabcut.blur.gaussian");
}
