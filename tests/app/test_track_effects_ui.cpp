// 0.7 (card #139): effects on a whole video track. The TrackEffects bridge over the model's track effect stack
// (EffectTarget::ofTrack): the list of what can go on a track, add / bypass / reorder / remove, a setting's slider drag as
// ONE undo step, the stopwatch and keys at the playhead (timeline time), Reset, and what is refused (an audio track, an
// audio effect, a locked track). Then Video Track Effects... in the real shell (the track header's signal opens the
// popover, screenshots in the dark and light themes at 1600 x 960 and at the smallest window).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "ProjectController.h"
#include "TrackEffectsBridge.h"

#include <wabcut/model/Clip.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QImage>
#include <QQuickItem>

#include <tuple>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

struct Fixture {
    Timeline tl;
    ProjectController controller;
    TrackEffectsBridge bridge;
    QString v1;
    QString v2;
    QString a1;

    Fixture()
        : tl("V2 |Bbbbbbbbbbbbbbbbbbbbbbbb|\n"
             "V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\n"
             "A1 |Aaaaaaaaaaaaaaaaaaaaaaaa|") {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        controller.setPlayhead(Ticks{});
        bridge.setController(&controller);
        v1 = trackKey(tl.track("V1"));
        v2 = trackKey(tl.track("V2"));
        a1 = trackKey(tl.track("A1"));
    }
    const Track& track(const char* label) const { return *controller.currentSequence()->findTrack(tl.track(label)); }
    std::size_t undoCount() { return controller.session().undoCount(); }
    QVariantMap setting(const QString& track, const QString& effect, const QString& key) const {
        const QVariantMap settings = bridge.effectSettings(track, effect);
        for (const QVariant& p : settings.value(QStringLiteral("params")).toList()) {
            if (p.toMap().value(QStringLiteral("key")).toString() == key)
                return p.toMap();
        }
        return {};
    }
};

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Track effects: the effects a video track takes, added in one undo step each (card 139)", "[app][track][effects]") {
    Fixture f;
    CHECK(f.bridge.count(f.v1) == 0);
    CHECK(f.bridge.effectsOf(f.v1).isEmpty());
    // The list: video effects only (no audio effects, no intrinsic ones), found by name or category, by category then name.
    const QVariantList all = f.bridge.available(QString());
    REQUIRE_FALSE(all.isEmpty());
    for (const QVariant& e : all) {
        CHECK(e.toMap().value(QStringLiteral("id")).toString() != QLatin1String("wabcut.audio.gain"));
        CHECK_FALSE(e.toMap().value(QStringLiteral("id")).toString().startsWith(QLatin1String("wabcut.audio.")));
    }
    const QVariantList blurs = f.bridge.available(QStringLiteral("gaussian"));
    REQUIRE(blurs.size() >= 1);
    CHECK(blurs.first().toMap().value(QStringLiteral("id")).toString() == QLatin1String("wabcut.blur.gaussian"));
    CHECK(f.bridge.available(QStringLiteral("no effect is called this")).isEmpty());

    const std::size_t undo = f.undoCount();
    CHECK(f.bridge.addEffect(f.v1, QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    CHECK(f.undoCount() == undo + 1);
    CHECK(f.controller.undoText().contains(QStringLiteral("Add")));
    REQUIRE(f.track("V1").effects.size() == 1);
    CHECK(f.track("V1").effects[0].effect == "wabcut.blur.gaussian");
    CHECK(f.track("V2").effects.empty()); // only the track it was added to
    CHECK(f.controller.currentSequence()->findClip(clipAt(*f.controller.snapshot(), f.tl.seq, "V1@0"))->clip().effects().empty());
    CHECK(f.bridge.count(f.v1) == 1);
    CHECK(f.bridge.count(f.v2) == 0);
    const QVariantList stack = f.bridge.effectsOf(f.v1);
    REQUIRE(stack.size() == 1);
    CHECK(stack[0].toMap().value(QStringLiteral("enabled")).toBool());
    CHECK(stack[0].toMap().value(QStringLiteral("known")).toBool());
    CHECK_FALSE(stack[0].toMap().value(QStringLiteral("summary")).toString().isEmpty());

    // Undo and redo take it off and put it back.
    REQUIRE(f.controller.undo());
    CHECK(f.track("V1").effects.empty());
    REQUIRE(f.controller.redo());
    CHECK(f.track("V1").effects.size() == 1);

    // A second effect, bypassed, reordered, removed: one step each.
    CHECK(f.bridge.addEffect(f.v1, QStringLiteral("wabcut.color.basic")).isEmpty());
    REQUIRE(f.track("V1").effects.size() == 2);
    const QString blur = effectKey(f.track("V1").effects[0].id);
    const QString grade = effectKey(f.track("V1").effects[1].id);
    std::size_t before = f.undoCount();
    CHECK(f.bridge.setEffectEnabled(f.v1, blur, false).isEmpty());
    CHECK_FALSE(f.track("V1").effects[0].enabled);
    CHECK(f.undoCount() == before + 1);
    CHECK_FALSE(f.bridge.effectsOf(f.v1)[0].toMap().value(QStringLiteral("enabled")).toBool());
    before = f.undoCount();
    CHECK(f.bridge.moveEffect(f.v1, grade, 0).isEmpty());
    CHECK(effectKey(f.track("V1").effects[0].id) == grade);
    CHECK(f.undoCount() == before + 1);
    CHECK(f.bridge.removeEffect(f.v1, grade).isEmpty());
    REQUIRE(f.track("V1").effects.size() == 1);
    CHECK(effectKey(f.track("V1").effects[0].id) == blur);

    // What cannot be done says why: an audio track, an audio effect, an unknown effect, a locked track.
    CHECK_FALSE(f.bridge.addEffect(f.a1, QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    CHECK(f.bridge.count(f.a1) == 0);
    CHECK_FALSE(f.bridge.addEffect(f.v1, QStringLiteral("wabcut.audio.gain")).isEmpty());
    CHECK_FALSE(f.bridge.addEffect(f.v1, QStringLiteral("no.such.effect")).isEmpty());
    CHECK_FALSE(f.bridge.removeEffect(f.v1, QStringLiteral("e99999")).isEmpty());
    edit::TrackStateChange lock;
    lock.locked = true;
    REQUIRE(f.controller.session().apply("Lock", [&](const ProjectState& s) { return edit::setTrackState(s, f.tl.seq, f.tl.track("V2"), lock); }));
    CHECK_FALSE(f.bridge.addEffect(f.v2, QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    CHECK(f.track("V2").effects.empty());
}

TEST_CASE("Track effects: settings, a slider drag as one undo step, the stopwatch and keys at the playhead (card 139)",
          "[app][track][effects]") {
    Fixture f;
    REQUIRE(f.bridge.addEffect(f.v1, QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    const QString blur = effectKey(f.track("V1").effects[0].id);
    const QVariantMap settings = f.bridge.effectSettings(f.v1, blur);
    CHECK(settings.value(QStringLiteral("name")).toString() == QLatin1String("Gaussian Blur"));
    CHECK(settings.value(QStringLiteral("known")).toBool());
    const QVariantMap radius = f.setting(f.v1, blur, QStringLiteral("radius"));
    REQUIRE_FALSE(radius.isEmpty());
    CHECK(radius.value(QStringLiteral("kind")).toString() == QLatin1String("slider"));
    CHECK(radius.value(QStringLiteral("animatable")).toBool());
    CHECK_FALSE(radius.value(QStringLiteral("animated")).toBool());
    const double def = radius.value(QStringLiteral("defaultValue")).toDouble();

    // A drag is many setEffectParam calls and ONE undo step.
    const std::size_t before = f.undoCount();
    for (const double v : {def + 2.0, def + 5.0, def + 9.0})
        CHECK(f.bridge.setEffectParam(f.v1, blur, QStringLiteral("radius"), v, true).isEmpty());
    f.bridge.endEffectGesture();
    CHECK(f.undoCount() == before + 1);
    CHECK(f.setting(f.v1, blur, QStringLiteral("radius")).value(QStringLiteral("value")).toDouble() == Approx(def + 9.0).margin(0.01));
    CHECK(effectParameterAt(f.track("V1").effects[0], "radius", Ticks{}) == Approx(def + 9.0).margin(0.01));
    // Out of range is clamped, not stored out of range; a setting that does not exist is refused.
    CHECK(f.bridge.setEffectParam(f.v1, blur, QStringLiteral("radius"), 1e9, false).isEmpty());
    CHECK(effectParameterAt(f.track("V1").effects[0], "radius", Ticks{}) <=
          EffectRegistry::builtin().find("wabcut.blur.gaussian")->param("radius")->maxValue);
    CHECK_FALSE(f.bridge.setEffectParam(f.v1, blur, QStringLiteral("no.such"), 1.0, false).isEmpty());
    CHECK_FALSE(f.bridge.setEffectParam(f.v1, blur, QStringLiteral("radius"), std::nan(""), false).isEmpty());

    // The stopwatch: animated, then the key goes at the playhead, in timeline time.
    f.controller.setPlayhead(framesToTicks(5, kRate25));
    CHECK(f.bridge.setAnimated(f.v1, blur, QStringLiteral("radius"), true).isEmpty());
    CHECK(f.setting(f.v1, blur, QStringLiteral("radius")).value(QStringLiteral("animated")).toBool());
    REQUIRE(f.track("V1").effects[0].find("radius")->isCurve());
    f.controller.setPlayhead(framesToTicks(12, kRate25));
    CHECK(f.bridge.setEffectParam(f.v1, blur, QStringLiteral("radius"), 30.0, false).isEmpty());
    const Curve* curve = f.track("V1").effects[0].find("radius")->curve();
    REQUIRE(curve != nullptr);
    REQUIRE(curve->keys.size() == 2);
    CHECK(curve->keys[0].time == framesToTicks(5, kRate25));
    CHECK(curve->keys[1].time == framesToTicks(12, kRate25));
    CHECK(curve->keys[1].value == Approx(30.0).margin(0.01));
    // Moving the playhead: the value shown follows the keys.
    f.controller.setPlayhead(framesToTicks(12, kRate25));
    CHECK(f.setting(f.v1, blur, QStringLiteral("radius")).value(QStringLiteral("value")).toDouble() == Approx(30.0).margin(0.01));
    // Stop animating: the keys go, the value at the playhead stays.
    CHECK(f.bridge.setAnimated(f.v1, blur, QStringLiteral("radius"), false).isEmpty());
    CHECK(f.track("V1").effects[0].find("radius")->isConstant());
    CHECK(effectParameterAt(f.track("V1").effects[0], "radius", Ticks{}) == Approx(30.0).margin(0.01));

    // Reset: every setting back to its default, one step.
    const std::size_t resetFrom = f.undoCount();
    CHECK(f.bridge.resetEffect(f.v1, blur).isEmpty());
    CHECK(f.undoCount() == resetFrom + 1);
    CHECK(effectParameterAt(f.track("V1").effects[0], "radius", Ticks{}) == Approx(def).margin(0.01));
    CHECK_FALSE(f.bridge.setAnimated(f.v1, blur, QStringLiteral("no.such"), true).isEmpty());
}

TEST_CASE("Track effects: the effects of a video track survive saving and loading (card 139)", "[app][track][effects]") {
    Fixture f;
    REQUIRE(f.bridge.addEffect(f.v2, QStringLiteral("wabcut.color.basic")).isEmpty());
    const QString key = effectKey(f.track("V2").effects[0].id);
    REQUIRE(f.bridge.setEffectParam(f.v2, key, QStringLiteral("exposure"), 25.0, false).isEmpty());
    const EffectStack saved = f.track("V2").effects;
    // The session's project as the file keeps it.
    const auto json = io::toJson(*f.controller.snapshot());
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(loaded->findSequence(f.tl.seq)->findTrack(f.tl.track("V2"))->effects == saved);
}

TEST_CASE("Track effects: Video Track Effects... opens the track's stack in the shell (screenshots)", "[app][track][effects][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* bridge = shell.engine.singletonInstance<TrackEffectsBridge*>("Wabcut", "TrackEffects");
    REQUIRE(controller);
    REQUIRE(bridge);
    Timeline tl("V2 |Bbbbbbbbbbbbbbbbbbbbbbbb|\n"
                "V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\n"
                "A1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->setPlayhead(Ticks{});
    const QString v1 = trackKey(tl.track("V1"));
    REQUIRE(bridge->addEffect(v1, QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    REQUIRE(bridge->addEffect(v1, QStringLiteral("wabcut.color.basic")).isEmpty());

    QObject* popup = nullptr;
    REQUIRE(waitFor([&] {
        popup = shell.object("trackEffectsPopover"); // a Popup is not an item: found through the window's objects
        return popup != nullptr;
    }));
    CHECK_FALSE(popup->property("visible").toBool()); // closed until asked for

    // The header's context menu: Video Track Effects... (video tracks only) opens it for that track, once the menu has closed.
    const auto headerMenu = [&](const char* badge) -> QObject* {
        REQUIRE_FALSE(shell.grab().isNull()); // a hidden window builds its lanes when it renders
        const auto lookup = [&] { return findNamed(shell.window->contentItem(), QString::fromLatin1(badge)); };
        REQUIRE(waitFor([&] { return lookup() != nullptr; }));
        QQuickItem* format = lookup();
        QQuickItem* header = format->parentItem();
        REQUIRE(header != nullptr);
        REQUIRE(QMetaObject::invokeMethod(header, "openMenu", Q_ARG(QVariant, QVariant::fromValue<QObject*>(header)),
                                          Q_ARG(QVariant, QPointF(40, 10))));
        QObject* loader = header->findChild<QObject*>(QStringLiteral("trackHeaderMenuLoader"));
        REQUIRE(loader != nullptr);
        QObject* menu = loader->property("item").value<QObject*>();
        REQUIRE(menu != nullptr);
        return menu;
    };
    {
        QObject* menu = headerMenu("trackFormat_A1");
        QObject* entry = menu->findChild<QObject*>(QStringLiteral("trackMenuEffects"));
        REQUIRE(entry != nullptr);
        CHECK_FALSE(entry->property("visible").toBool()); // an audio track keeps its mixer rack
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
        spin(60);
    }
    QObject* menu = headerMenu("trackFormat_V1");
    QObject* entry = menu->findChild<QObject*>(QStringLiteral("trackMenuEffects"));
    REQUIRE(entry != nullptr);
    CHECK(entry->property("visible").toBool());
    REQUIRE(QMetaObject::invokeMethod(entry, "triggered"));
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return popup->property("visible").toBool(); }));
    REQUIRE(waitFor([&] { return popup->property("effects").toList().size() == 2; }));
    CHECK(popup->property("title").toString() == QStringLiteral("Effects on V1"));

    const auto grab = [&] {
        spin(80);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        return image;
    };
    const auto saveTo = [&](const QImage& image, const QString& name) {
        REQUIRE(image.save(outputDir() + QLatin1Char('/') + name));
    };
    for (const auto& [width, height, tag] : {std::tuple{kShotWidth, kShotHeight, "wide"}, std::tuple{1100, 700, "small"}}) {
        shell.resize(width, height);
        for (const bool dark : {true, false}) {
            shell.setAppearance(dark, "ultraviolet");
            REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
            saveTo(grab(), QStringLiteral("track-effects-%1-%2.png").arg(QLatin1String(tag), dark ? QStringLiteral("dark") : QStringLiteral("light")));
        }
    }
    shell.resize(kShotWidth, kShotHeight);
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(shell.settle(true, shell.themeColor("accentFill")));

    // Picking an effect: the filter, then a click adds it and selects it.
    REQUIRE(QMetaObject::invokeMethod(popup, "add", Q_ARG(QString, QStringLiteral("wabcut.blur.directional"))));
    CHECK(popup->property("effects").toList().size() == 3);
    CHECK(controller->currentSequence()->findTrack(tl.track("V1"))->effects.size() == 3);
    CHECK(popup->property("message").toString().isEmpty());
    saveTo(grab(), QStringLiteral("track-effects-added-dark.png"));
    // An effect that cannot go on the track says why in the popover.
    REQUIRE(QMetaObject::invokeMethod(popup, "add", Q_ARG(QString, QStringLiteral("wabcut.audio.gain"))));
    CHECK_FALSE(popup->property("message").toString().isEmpty());
    // Closing: Done.
    REQUIRE(QMetaObject::invokeMethod(popup, "close"));
    REQUIRE(waitFor([&] { return !popup->property("visible").toBool(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("TrackEffects"), Qt::CaseInsensitive));
}
