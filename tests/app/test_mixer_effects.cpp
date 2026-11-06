// r07 (stream D): the Audio Track Mixer's effects and the Sequence Settings frame size. Offscreen; no window is shown.
//   * Every entry of a strip's "+ Effect" menu goes on a track and on the master (one undo step), or says why; the
//     Essential Sound record (a clip's, refused by audio.addEffect) is no longer offered.
//   * Effect settings: every parameter of a track or master effect with its control and unit, a drag is one undo step,
//     typed values are clamped, Reset to defaults, the key at the playhead on an animated parameter, switches.
//   * Enhance Speech (rendered per clip, so it goes on the track's clips) shows on the strip as a slot of its own;
//     Bypass and Remove act on every clip of the track in one undo step each.
//   * In the shell: the "+ Effect" menu adds through its real items (Enhance Speech opens its dialog once the menu has
//     closed), a failure shows on the strip, Settings… and a double-click open the settings popup; screenshots dark and
//     light (test-output/ui/mixer-effect-settings*.png).
//   * Sequence Settings: the frame size boxes fit five digits and their unit, labels outside, wrapping; screenshots dark
//     and light (test-output/ui/sequence-settings*.png).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "ClipAudioBridge.h"
#include "EditingBridge.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QQuickItem>

#include <algorithm>
#include <cmath>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

constexpr const char* kEnhance = "wabcut.audio.enhanceSpeech";

/// A1 and A2 with clips, the mixer over them (no window).
struct MixerRig {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A2     |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )"};
    ProjectController controller;
    AudioMixerBridge mixer;

    MixerRig() {
        controller.resetProject(tl.state, tl.seq);
        mixer.setController(&controller);
    }
    QString a1() const { return trackKey(tl.track("A1")); }
    QString a2() const { return trackKey(tl.track("A2")); }
    const Track& track(const char* name) const { return *controller.currentSequence()->findTrack(tl.track(name)); }
    const EffectStack& master() const { return controller.currentSequence()->audioEffects; }
    std::size_t undo() { return controller.session().undoCount(); }
    QVariantMap strip(const QString& key) const {
        for (const QVariant& s : mixer.strips()) {
            if (s.toMap().value(QStringLiteral("key")).toString() == key)
                return s.toMap();
        }
        return {};
    }
    /// Puts `effect` on every clip of track `name` (as the Enhance Speech dialog does), one undo step.
    void addToClips(const char* name, const char* effect) {
        const SequenceId seq = tl.seq;
        std::vector<ClipId> clips;
        for (const Clip& c : track(name).clips)
            clips.push_back(c.id);
        REQUIRE(controller.applyEditFn("Add To Clips", [&](const ProjectState& state) -> edit::EditOutcome {
            ProjectState now = state;
            for (ClipId c : clips) {
                auto done = edit::addEffect(now, seq, edit::EffectTarget::ofClip(c), std::string(effect));
                if (!done)
                    return done;
                now = std::move(done->state);
            }
            edit::EditResult result;
            result.changes = diff(state, now);
            result.state = std::move(now);
            return result;
        }));
    }
};

QVariantMap paramRow(const QVariantMap& settings, const char* key) {
    for (const QVariant& p : settings.value(QStringLiteral("params")).toList()) {
        if (p.toMap().value(QStringLiteral("key")).toString() == QLatin1String(key))
            return p.toMap();
    }
    return {};
}

double modelValue(const EffectInstance& e, const char* key, Ticks at = Ticks{}) {
    return effectParameterAt(e, key, at);
}

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

QObject* loadedPopup(QObject* loader) {
    QObject* popup = nullptr;
    const bool open = waitFor([&] {
        popup = loader->property("item").value<QObject*>();
        return popup && popup->property("opened").toBool();
    }, 5000);
    return open ? popup : nullptr;
}

QQuickItem* popupItem(QObject* popup) {
    auto* content = popup->property("contentItem").value<QQuickItem*>();
    return content ? content->parentItem() : nullptr;
}

/// Picks `id` in an OptionsMenu as a click does: the menu opens, its item is triggered (the menu closes after).
void pick(QObject* menu, QQuickItem* anchor, const QString& id) {
    REQUIRE(QMetaObject::invokeMethod(menu, "popup", Q_ARG(QQuickItem*, anchor)));
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 5000));
    const QVariantList options = menu->property("options").toList();
    int index = -1;
    for (int i = 0; i < options.size(); ++i) {
        if (options[i].toMap().value(QStringLiteral("id")).toString() == id)
            index = i;
    }
    INFO(id.toStdString());
    REQUIRE(index >= 0);
    QQuickItem* item = nullptr;
    REQUIRE(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index)));
    REQUIRE(item);
    REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }, 5000));
    spin(50);
}

void saveShot(const QImage& image, QRectF area, const QString& name) {
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    area = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

} // namespace

// ---- The "+ Effect" menu ---------------------------------------------------------------------------------------------

TEST_CASE("Mixer effect menu: every entry goes on a track and the master in one undo step or says why",
          "[app][mixer][effects]") {
    MixerRig r;
    const QVariantList types = r.mixer.effectTypes();
    REQUIRE(types.size() > 12);
    int checked = 0;
    for (const QVariant& t : types) {
        const QString id = t.toMap().value(QStringLiteral("id")).toString();
        INFO(id.toStdString());
        // The Essential Sound record describes a clip: audio.addEffect refuses it on tracks, so it is not offered.
        CHECK(id != QStringLiteral("wabcut.audio.essentialSound"));
        if (id == QLatin1String(kEnhance))
            continue; // the next test case
        for (const QString& target : {r.a2(), QStringLiteral("master")}) {
            INFO(target.toStdString());
            const bool master = target == QLatin1String("master");
            const std::size_t before = master ? r.master().size() : r.track("A2").effects.size();
            const std::size_t undo = r.undo();
            const QString error = r.mixer.addEffect(target, id);
            CHECK(error.isEmpty());
            if (!error.isEmpty())
                continue;
            const EffectStack& stack = master ? r.master() : r.track("A2").effects;
            REQUIRE(stack.size() == before + 1);
            CHECK(stack.back().effect == id.toStdString());
            CHECK(r.undo() == undo + 1);
            // The strip shows it, with its hover details when it has parameters.
            const QVariantList shown = master ? r.mixer.master().value(QStringLiteral("effects")).toList()
                                              : r.strip(target).value(QStringLiteral("effects")).toList();
            REQUIRE(shown.size() == static_cast<qsizetype>(before + 1));
            const EffectDescriptor* d = EffectRegistry::builtin().find(id.toStdString());
            REQUIRE(d);
            CHECK(shown.back().toMap().value(QStringLiteral("details")).toString().isEmpty() == d->params.empty());
            // Its settings list every parameter.
            const QVariantMap settings = r.mixer.effectSettings(target, shown.back().toMap().value(QStringLiteral("key")).toString());
            CHECK(settings.value(QStringLiteral("params")).toList().size() == static_cast<qsizetype>(d->params.size()));
            REQUIRE(r.controller.undo());
            ++checked;
        }
    }
    CHECK(checked >= 2 * (static_cast<int>(types.size()) - 1));
}

TEST_CASE("Mixer effect menu: Enhance Speech asks for the track's clips; an empty track and the master say why",
          "[app][mixer][effects][enhance]") {
    MixerRig r;
    QStringList asked;
    QObject::connect(&r.mixer, &AudioMixerBridge::enhanceClipsRequested, [&](const QStringList& clips) { asked = clips; });
    CHECK(r.mixer.addEffect(r.a1(), QString::fromLatin1(kEnhance)).isEmpty());
    CHECK(asked == r.mixer.trackClips(r.a1()));
    CHECK_FALSE(r.mixer.addEffect(QStringLiteral("master"), QString::fromLatin1(kEnhance)).isEmpty());
    // A track without clips: the reason (the strip shows it).
    REQUIRE(r.controller.applyEditFn("Clear A2", [&](const ProjectState& state) {
        std::vector<ClipId> clips;
        for (const Clip& c : r.track("A2").clips)
            clips.push_back(c.id);
        return edit::deleteClips(state, r.tl.seq, clips);
    }));
    asked.clear();
    CHECK(r.mixer.addEffect(r.a2(), QString::fromLatin1(kEnhance)) == QStringLiteral("The track has no clips to enhance"));
    CHECK(asked.isEmpty());
}

// ---- Effect settings -------------------------------------------------------------------------------------------------

TEST_CASE("Mixer effect settings: every parameter with its unit; a drag is one undo step; typed values clamp; reset",
          "[app][mixer][effects][settings]") {
    MixerRig r;
    REQUIRE(r.mixer.addEffect(r.a1(), QStringLiteral("wabcut.audio.compressor")).isEmpty());
    const EffectInstanceId id = r.track("A1").effects.front().id;
    const QString key = effectKey(id);
    const auto compressor = [&]() -> const EffectInstance& { return r.track("A1").effects.front(); };

    QVariantMap s = r.mixer.effectSettings(r.a1(), key);
    CHECK(s.value(QStringLiteral("name")).toString() == QStringLiteral("Compressor"));
    CHECK(s.value(QStringLiteral("enabled")).toBool());
    CHECK(s.value(QStringLiteral("known")).toBool());
    REQUIRE(s.value(QStringLiteral("params")).toList().size() == 5);
    QVariantMap threshold = paramRow(s, "threshold");
    CHECK(threshold.value(QStringLiteral("kind")).toString() == QStringLiteral("slider"));
    CHECK(threshold.value(QStringLiteral("unit")).toString() == QStringLiteral("dB"));
    CHECK(threshold.value(QStringLiteral("value")).toDouble() == Catch::Approx(-20.0));
    CHECK(threshold.value(QStringLiteral("from")).toDouble() == Catch::Approx(-60.0));
    CHECK(threshold.value(QStringLiteral("to")).toDouble() == Catch::Approx(0.0));
    CHECK(paramRow(s, "attack").value(QStringLiteral("unit")).toString() == QStringLiteral("ms"));

    // A drag: three moves, one undo step named after the parameter.
    std::size_t undo = r.undo();
    for (double v : {-25.0, -30.0, -35.0})
        REQUIRE(r.mixer.setEffectParam(r.a1(), key, QStringLiteral("threshold"), v, true).isEmpty());
    r.mixer.endEffectGesture();
    CHECK(r.undo() == undo + 1);
    CHECK(modelValue(compressor(), "threshold") == Catch::Approx(-35.0));
    CHECK(r.controller.undoText() == QStringLiteral("Undo Compressor: Threshold"));
    CHECK(paramRow(r.mixer.effectSettings(r.a1(), key), "threshold").value(QStringLiteral("value")).toDouble() == Catch::Approx(-35.0));
    REQUIRE(r.controller.undo());
    CHECK(modelValue(compressor(), "threshold") == Catch::Approx(-20.0));

    // A typed value is its own step and is clamped to the range; the slot's details follow.
    undo = r.undo();
    REQUIRE(r.mixer.setEffectParam(r.a1(), key, QStringLiteral("ratio"), 100.0).isEmpty());
    REQUIRE(r.mixer.setEffectParam(r.a1(), key, QStringLiteral("makeup"), 3.0).isEmpty());
    CHECK(r.undo() == undo + 2);
    CHECK(modelValue(compressor(), "ratio") == Catch::Approx(20.0));
    const QString details = r.strip(r.a1()).value(QStringLiteral("effects")).toList().front().toMap().value(QStringLiteral("details")).toString();
    INFO(details.toStdString());
    CHECK(details.contains(QStringLiteral("Threshold")));
    CHECK(details.contains(QStringLiteral("Ratio 20.00")));
    CHECK(details.contains(QStringLiteral("1 more")));

    // Reset to defaults: one undo step.
    undo = r.undo();
    REQUIRE(r.mixer.resetEffect(r.a1(), key).isEmpty());
    CHECK(r.undo() == undo + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Reset Compressor"));
    CHECK(modelValue(compressor(), "ratio") == Catch::Approx(4.0));
    CHECK(modelValue(compressor(), "makeup") == Catch::Approx(0.0));
    REQUIRE(r.controller.undo());
    CHECK(modelValue(compressor(), "ratio") == Catch::Approx(20.0));

    // Errors: no such setting, effect or target.
    CHECK_FALSE(r.mixer.setEffectParam(r.a1(), key, QStringLiteral("nope"), 1.0).isEmpty());
    CHECK_FALSE(r.mixer.setEffectParam(r.a1(), QStringLiteral("e999999"), QStringLiteral("ratio"), 1.0).isEmpty());
    CHECK(r.mixer.effectSettings(QStringLiteral("x1"), key).isEmpty());
    CHECK(r.mixer.effectSettings(r.a2(), key).isEmpty());
}

TEST_CASE("Mixer effect settings: an animated parameter takes its key at the playhead; switches; the master",
          "[app][mixer][effects][settings]") {
    MixerRig r;
    REQUIRE(r.mixer.addEffect(r.a1(), QStringLiteral("wabcut.audio.gain")).isEmpty());
    const EffectInstanceId id = r.track("A1").effects.front().id;
    const QString key = effectKey(id);
    // Two keys (0 s: 0 dB, 2 s: -12 dB), then at 1 s the setting is a third key there; the others stay.
    for (const auto& [t, v] : {std::pair{0.0, 0.0}, std::pair{2.0, -12.0}}) {
        const double at = t;
        const double value = v;
        REQUIRE(r.controller.applyEditFn("Key", [&](const ProjectState& state) {
            return edit::addKeyframe(state, r.tl.seq, edit::EffectTarget::ofTrack(r.tl.track("A1")),
                                     edit::ParamRef::ofEffect(id, "gain"), Keyframe{seconds(at), value});
        }));
    }
    r.controller.setPlayhead(seconds(1.0));
    QVariantMap gain = paramRow(r.mixer.effectSettings(r.a1(), key), "gain");
    CHECK(gain.value(QStringLiteral("animated")).toBool());
    CHECK(gain.value(QStringLiteral("value")).toDouble() == Catch::Approx(-6.0));
    const std::size_t undo = r.undo();
    REQUIRE(r.mixer.setEffectParam(r.a1(), key, QStringLiteral("gain"), 3.0).isEmpty());
    CHECK(r.undo() == undo + 1);
    const EffectParam& p = *std::ranges::find_if(r.track("A1").effects.front().params, [](const EffectParam& e) { return e.key == "gain"; });
    REQUIRE(p.value.isCurve());
    CHECK(p.value.curve()->keys.size() == 3);
    CHECK(p.value.valueAt(seconds(1.0)) == Catch::Approx(3.0));
    CHECK(p.value.valueAt(seconds(2.0)) == Catch::Approx(-12.0));

    // A switch (Dynamics' AutoGate) is on or off.
    REQUIRE(r.mixer.addEffect(QStringLiteral("master"), QStringLiteral("wabcut.audio.dynamics")).isEmpty());
    const QString dyn = effectKey(r.master().back().id);
    QVariantMap gate = paramRow(r.mixer.effectSettings(QStringLiteral("master"), dyn), "gateEnable");
    CHECK(gate.value(QStringLiteral("kind")).toString() == QStringLiteral("toggle"));
    CHECK(gate.value(QStringLiteral("value")).toDouble() == Catch::Approx(0.0));
    REQUIRE(r.mixer.setEffectParam(QStringLiteral("master"), dyn, QStringLiteral("gateEnable"), 0.8).isEmpty());
    CHECK(modelValue(r.master().back(), "gateEnable") == Catch::Approx(1.0));
    CHECK(paramRow(r.mixer.effectSettings(QStringLiteral("master"), dyn), "gateEnable").value(QStringLiteral("valueText")).toString()
          == QStringLiteral("On"));
}

// ---- Enhance Speech on the strip ---------------------------------------------------------------------------------------

TEST_CASE("Mixer Enhance Speech slot: the track's enhanced clips show as one slot; bypass and remove are one undo step each",
          "[app][mixer][effects][enhance]") {
    MixerRig r;
    CHECK(r.strip(r.a1()).value(QStringLiteral("clipEffects")).toList().isEmpty());
    r.addToClips("A1", kEnhance);
    QVariantList clipEffects = r.strip(r.a1()).value(QStringLiteral("clipEffects")).toList();
    REQUIRE(clipEffects.size() == 1);
    QVariantMap slot = clipEffects.front().toMap();
    CHECK(slot.value(QStringLiteral("effect")).toString() == QLatin1String(kEnhance));
    CHECK(slot.value(QStringLiteral("name")).toString() == QStringLiteral("Enhance Speech"));
    CHECK(slot.value(QStringLiteral("enabled")).toBool());
    CHECK(slot.value(QStringLiteral("count")).toInt() == 1);
    CHECK(slot.value(QStringLiteral("total")).toInt() == 1);
    CHECK(slot.value(QStringLiteral("clips")).toStringList() == r.mixer.trackClips(r.a1()));
    CHECK(slot.value(QStringLiteral("summary")).toString() == QStringLiteral("1 of 1 clip"));
    CHECK(r.track("A1").effects.empty()); // never a track effect

    const auto clipEffect = [&]() -> const EffectInstance* {
        for (const EffectInstance& e : r.track("A1").clips.front().effects()) {
            if (e.effect == kEnhance)
                return &e;
        }
        return nullptr;
    };
    std::size_t undo = r.undo();
    REQUIRE(r.mixer.setClipEffectEnabled(r.a1(), QString::fromLatin1(kEnhance), false).isEmpty());
    CHECK(r.undo() == undo + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Bypass Enhance Speech"));
    REQUIRE(clipEffect());
    CHECK_FALSE(clipEffect()->enabled);
    CHECK_FALSE(r.strip(r.a1()).value(QStringLiteral("clipEffects")).toList().front().toMap().value(QStringLiteral("enabled")).toBool());
    REQUIRE(r.controller.undo());
    CHECK(clipEffect()->enabled);

    undo = r.undo();
    REQUIRE(r.mixer.removeClipEffect(r.a1(), QString::fromLatin1(kEnhance)).isEmpty());
    CHECK(r.undo() == undo + 1);
    CHECK(clipEffect() == nullptr);
    CHECK(r.strip(r.a1()).value(QStringLiteral("clipEffects")).toList().isEmpty());
    REQUIRE(r.controller.undo());
    CHECK(clipEffect() != nullptr);

    // A track without it says so.
    CHECK_FALSE(r.mixer.removeClipEffect(r.a2(), QString::fromLatin1(kEnhance)).isEmpty());
    CHECK_FALSE(r.mixer.setClipEffectEnabled(QStringLiteral("master"), QString::fromLatin1(kEnhance), false).isEmpty());
}

// ---- In the shell ------------------------------------------------------------------------------------------------------

TEST_CASE("Mixer effects in the shell: the Add menu and a failure on the strip and Enhance Speech's dialog and the settings popup; screenshots",
          "[app][mixer][effects][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            INFO(join(shell.errors));
            REQUIRE(shell.errors.isEmpty());
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
            auto* clipAudio = shell.engine.singletonInstance<ClipAudioBridge*>("Wabcut", "ClipAudio");
            REQUIRE(controller);
            REQUIRE(mixer);
            REQUIRE(clipAudio);
            clipAudio->setModelsReadyForTests(false); // Enhance Speech's model still to download
            Timeline tl{R"(
                V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
                A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
                A2     |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
            )"};
            tl.linkMatching();
            controller->resetProject(tl.state, tl.seq);
            shell.window->setProperty("workspace", 1); // Audio: the mixer
            spin(400);
            QQuickItem* content = shell.window->contentItem();
            // The track strips are made again whenever the mixer changes: look them up each time.
            const auto stripNamed = [&](const char* name) {
                QQuickItem* strip = findNamed(content, QString::fromLatin1(name));
                REQUIRE(strip);
                return strip;
            };
            const auto a1 = [&] { return stripNamed("mixerStrip_A1"); };
            const auto a2 = [&] { return stripNamed("mixerStrip_A2"); };
            QQuickItem* master = stripNamed("masterStrip");
            const auto addMenuOf = [](QQuickItem* strip) {
                QObject* menu = strip->findChild<QObject*>(QStringLiteral("effectAddMenu"));
                REQUIRE(menu);
                return menu;
            };
            QQuickItem* a2Add = findNamed(a2(), QStringLiteral("effectSlotAdd"));
            REQUIRE(a2Add);

            // The Add menu's real items: a compressor goes on A2 (one undo step).
            std::size_t undo = controller->session().undoCount();
            pick(addMenuOf(a2()), a2Add, QStringLiteral("wabcut.audio.compressor"));
            REQUIRE(waitFor([&] { return findNamed(a2(), QStringLiteral("effectSlot_0")) != nullptr; }, 5000));
            CHECK(controller->session().undoCount() == undo + 1);

            // Enhance Speech on A1: its dialog opens (after the menu closed) for A1's clips, and the strip says the model
            // downloads first, with the button that opens it again.
            QQuickItem* a1Add = findNamed(a1(), QStringLiteral("effectSlotAdd"));
            REQUIRE(a1Add);
            pick(addMenuOf(a1()), a1Add, QString::fromLatin1(kEnhance));
            QObject* enhanceLoader = shell.object("enhanceSpeechDialogLoader");
            REQUIRE(enhanceLoader);
            QObject* enhance = loadedPopup(enhanceLoader);
            REQUIRE(enhance);
            CHECK(enhance->property("clips").toStringList() == mixer->trackClips(trackKey(tl.track("A1"))));
            QMetaObject::invokeMethod(enhance, "close");
            REQUIRE(waitFor([&] { return !enhance->property("visible").toBool(); }, 5000));
            QQuickItem* a1Message = findNamed(a1(), QStringLiteral("effectSlotsMessage"));
            REQUIRE(a1Message);
            CHECK(a1Message->isVisible());
            QQuickItem* a1Action = findNamed(a1(), QStringLiteral("effectSlotsMessageAction"));
            REQUIRE(a1Action);
            CHECK(a1Action->isVisible());

            // Enhance Speech on the master: the reason shows on the master strip (it used to be kept unseen).
            QQuickItem* masterAdd = findNamed(master, QStringLiteral("effectSlotAdd"));
            REQUIRE(masterAdd);
            pick(addMenuOf(master), masterAdd, QString::fromLatin1(kEnhance));
            QQuickItem* masterMessage = findNamed(master, QStringLiteral("effectSlotsMessage"));
            REQUIRE(masterMessage);
            CHECK(masterMessage->isVisible());
            QQuickItem* masterText = findNamed(master, QStringLiteral("effectSlotsMessageText"));
            REQUIRE(masterText);
            CHECK(masterText->property("text").toString().contains(QStringLiteral("dialogue clips")));
            REQUIRE(QMetaObject::invokeMethod(findNamed(master, QStringLiteral("effectSlotsMessageClose")), "clicked"));
            CHECK_FALSE(masterMessage->isVisible());

            // The enhanced clips show as a slot on A1.
            {
                std::vector<ClipId> clips;
                for (const Clip& c : controller->currentSequence()->findTrack(tl.track("A1"))->clips)
                    clips.push_back(c.id);
                REQUIRE(controller->applyEditFn("Enhance", [&](const ProjectState& state) {
                    return edit::addEffect(state, tl.seq, edit::EffectTarget::ofClip(clips.front()), std::string(kEnhance));
                }));
            }
            REQUIRE(waitFor([&] { return findNamed(a1(), QStringLiteral("clipEffectSlot_0")) != nullptr; }, 5000));
            QQuickItem* a1MessageNow = findNamed(a1(), QStringLiteral("effectSlotsMessage"));
            REQUIRE(a1MessageNow);
            CHECK_FALSE(a1MessageNow->isVisible()); // the model message went with it

            // A2's compressor: its menu has Settings…, and a double-click opens the settings popup too.
            QQuickItem* slot = findNamed(a2(), QStringLiteral("effectSlot_0"));
            REQUIRE(slot);
            QObject* slotMenu = slot->findChild<QObject*>(QStringLiteral("effectSlotMenu"));
            REQUIRE(slotMenu);
            CHECK(slotMenu->property("options").toList().front().toMap().value(QStringLiteral("id")).toString() == QStringLiteral("settings"));
            REQUIRE(QMetaObject::invokeMethod(slot, "doubleClicked"));
            QObject* settingsLoader = shell.window->findChild<QObject*>(QStringLiteral("mixerEffectSettingsLoader"));
            if (!settingsLoader)
                settingsLoader = content->findChild<QObject*>(QStringLiteral("mixerEffectSettingsLoader"));
            REQUIRE(settingsLoader);
            QObject* settings = loadedPopup(settingsLoader);
            REQUIRE(settings);
            QQuickItem* popup = popupItem(settings);
            REQUIRE(popup);
            QQuickItem* threshold = findNamed(popup, QStringLiteral("mixerEffectParam_threshold"));
            REQUIRE(threshold);
            QQuickItem* number = findNamed(threshold, QStringLiteral("mixerEffectNumber"));
            REQUIRE(number);
            // A value typed in the box: one undo step, and the box shows it.
            undo = controller->session().undoCount();
            REQUIRE(QMetaObject::invokeMethod(number, "valueModified", Q_ARG(double, -32.0)));
            mixer->endEffectGesture();
            spin(50);
            CHECK(controller->session().undoCount() == undo + 1);
            const EffectInstance& comp = controller->currentSequence()->findTrack(tl.track("A2"))->effects.front();
            CHECK(effectParameterAt(comp, "threshold", Ticks{}) == Catch::Approx(-32.0));
            CHECK(number->property("value").toDouble() == Catch::Approx(-32.0));
            // Every number box shows its whole value (Release: up to "2000.0 ms").
            QQuickItem* release = findNamed(findNamed(popup, QStringLiteral("mixerEffectParam_release")), QStringLiteral("mixerEffectNumber"));
            REQUIRE(release);
            REQUIRE(QMetaObject::invokeMethod(release, "valueModified", Q_ARG(double, 2000.0)));
            mixer->endEffectGesture();
            spin(50);
            const double room = release->width() - release->property("leftPadding").toDouble() - release->property("rightPadding").toDouble();
            CHECK(release->property("contentWidth").toDouble() <= room + 0.5);
            // The popup lies inside the window.
            const QRectF box = sceneRect(popup);
            CHECK(box.left() >= 0);
            CHECK(box.top() >= 0);
            CHECK(box.right() <= shell.window->width());
            CHECK(box.bottom() <= shell.window->height());
            spin(150);
            {
                const QImage image = shell.grab();
                saveShot(image, box.united(sceneRect(a2())).adjusted(-16, -16, 16, 16),
                         dark ? QStringLiteral("mixer-effect-settings.png") : QStringLiteral("mixer-effect-settings-light.png"));
                saveShot(image, sceneRect(a1()).united(sceneRect(a2())).united(sceneRect(master)).adjusted(-8, -8, 8, 8),
                         dark ? QStringLiteral("mixer-effect-slots.png") : QStringLiteral("mixer-effect-slots-light.png"));
            }
            // Reset to defaults in the popup: one undo step.
            undo = controller->session().undoCount();
            REQUIRE(QMetaObject::invokeMethod(findNamed(popup, QStringLiteral("mixerEffectReset")), "clicked"));
            CHECK(controller->session().undoCount() == undo + 1);
            CHECK(effectParameterAt(controller->currentSequence()->findTrack(tl.track("A2"))->effects.front(), "threshold", Ticks{})
                  == Catch::Approx(-20.0));
            QMetaObject::invokeMethod(settings, "close");
            spin(100);
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}

// ---- Sequence Settings -------------------------------------------------------------------------------------------------

TEST_CASE("Sequence Settings: the frame size boxes fit five digits with their unit and labels outside; screenshots",
          "[app][editing][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            INFO(join(shell.errors));
            REQUIRE(shell.errors.isEmpty());
            // The smallest main window (1100 x 700) and a wide one.
            const int width = dark ? 1100 : kShotWidth;
            const int height = dark ? 700 : kShotHeight;
            shell.resize(width, height);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* editing = shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
            REQUIRE(controller);
            REQUIRE(editing);
            Timeline tl{R"(
                V1 tgt |Aaaaaaaaaa|
                A1 tgt |Aaaaaaaaaa|
            )"};
            tl.linkMatching();
            controller->resetProject(tl.state, tl.seq);
            spin(100);
            REQUIRE(editing->runCommand(QStringLiteral("sequence.settings")));
            QObject* dialog = nullptr;
            REQUIRE(waitFor([&] {
                dialog = shell.window->findChild<QObject*>(QStringLiteral("sequenceSettingsDialog"));
                return dialog && dialog->property("opened").toBool();
            }, 5000));
            dialog->setProperty("frameWidth", 16384);
            dialog->setProperty("frameHeight", 16384);
            spin(100);
            QQuickItem* box = popupItem(dialog);
            REQUIRE(box);
            const QRectF dialogRect = sceneRect(box);
            for (const char* name : {"sequenceSettingsWidth", "sequenceSettingsHeight"}) {
                INFO(name);
                QQuickItem* field = findNamed(box, QString::fromLatin1(name));
                REQUIRE(field);
                CHECK(field->property("text").toString() == QStringLiteral("16384"));
                CHECK(field->property("suffix").toString() == QStringLiteral("px"));
                const double room = field->width() - field->property("leftPadding").toDouble() - field->property("rightPadding").toDouble();
                CHECK(field->property("contentWidth").toDouble() <= room + 0.5);
                // Inside the dialog.
                const QRectF r = sceneRect(field);
                CHECK(r.left() >= dialogRect.left());
                CHECK(r.right() <= dialogRect.right());
            }
            QQuickItem* aspect = findNamed(box, QStringLiteral("sequenceSettingsAspect"));
            REQUIRE(aspect);
            CHECK(aspect->property("text").toString() == QStringLiteral("1:1"));
            dialog->setProperty("frameWidth", 1920);
            dialog->setProperty("frameHeight", 1080);
            spin(50);
            CHECK(aspect->property("text").toString() == QStringLiteral("16:9"));
            // Nothing in the dialog runs past its right edge.
            QQuickItem* frameSize = findNamed(box, QStringLiteral("sequenceSettingsFrameSize"));
            REQUIRE(frameSize);
            CHECK(sceneRect(frameSize).right() <= dialogRect.right());
            spin(150);
            saveShot(shell.grab(), dialogRect.adjusted(-16, -16, 16, 16),
                     dark ? QStringLiteral("sequence-settings.png") : QStringLiteral("sequence-settings-light.png"));
            QMetaObject::invokeMethod(dialog, "close");
            spin(100);
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}
