// p02 audio: the Audio Track Mixer's routing (AudioMixerRouting.cpp). Offscreen; no window is shown.
//   * submix strips: added from the mixer, listed after the tracks with their output; deleted (tracks back to the Mix);
//   * output assignment and sends through the strips: each one undo step, a send level drag one step, "Create ...
//     Submix" choices make and assign a submix in one step;
//   * the submix fader is the submix track's Volume (one undo step per drag);
//   * the mixer QML with submix strips and sends loads without warnings (screenshot test-output/ui/mixer-submixes.png).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "ProjectController.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

struct RoutingRig {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
        A2     |Mmmmmmmmmm|
    )"};
    ProjectController controller;
    AudioMixerBridge mixer;

    RoutingRig() {
        controller.resetProject(tl.state, tl.seq);
        mixer.setController(&controller);
    }
    QString a1() const { return trackKey(tl.track("A1")); }
    QString a2() const { return trackKey(tl.track("A2")); }
    const Sequence& seq() const { return *controller.currentSequence(); }
    std::size_t undo() { return controller.session().undoCount(); }
};

/// An item by object name in the visual tree (Repeater delegates are not QObject children of the window).
QQuickItem* routingItem(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = routingItem(child, name))
            return found;
    }
    return nullptr;
}

QVariantMap stripOf(const QVariantList& strips, const QString& key) {
    for (const QVariant& v : strips) {
        if (v.toMap().value(QStringLiteral("key")).toString() == key)
            return v.toMap();
    }
    return {};
}

} // namespace

TEST_CASE("Mixer routing: submix strips, output assignment and sends, one undo step each", "[app][mixer][submix]") {
    RoutingRig rig;
    std::size_t undo = rig.undo();
    REQUIRE(rig.mixer.addSubmix(QStringLiteral("stereo")).isEmpty());
    CHECK(rig.undo() == ++undo);
    REQUIRE(rig.mixer.submixes().size() == 1);
    const QVariantMap s1 = rig.mixer.submixes().front().toMap();
    const QString bus = s1.value(QStringLiteral("key")).toString();
    CHECK(s1.value(QStringLiteral("shortName")).toString() == QStringLiteral("S1"));
    CHECK(s1.value(QStringLiteral("name")).toString() == QStringLiteral("Submix 1"));
    CHECK(s1.value(QStringLiteral("output")).toString() == QStringLiteral("mix"));
    CHECK(rig.mixer.outputs().size() == 2); // the Mix and S1

    // A1 to S1.
    REQUIRE(rig.mixer.setOutput(rig.a1(), bus).isEmpty());
    CHECK(rig.undo() == ++undo);
    CHECK(rig.seq().findTrack(rig.tl.track("A1"))->output == parseTrackKey(bus).value());
    CHECK(stripOf(rig.mixer.strips(), rig.a1()).value(QStringLiteral("outputName")).toString() == QStringLiteral("Submix 1"));
    // S1 cannot output to itself.
    CHECK_FALSE(rig.mixer.setOutput(bus, bus).isEmpty());

    // A send from A2: a level drag is one step, then pre-fader and mute.
    REQUIRE(rig.mixer.addSend(rig.a2(), bus).isEmpty());
    CHECK(rig.undo() == ++undo);
    for (double db = 0; db >= -12; db -= 1.5)
        REQUIRE(rig.mixer.setSendLevel(rig.a2(), 1, db).isEmpty());
    rig.mixer.endSendGesture();
    CHECK(rig.undo() == ++undo);
    const TrackSend& send = rig.seq().findTrack(rig.tl.track("A2"))->sends.front();
    CHECK(send.level == ParamValue(-12.0));
    REQUIRE(rig.mixer.setSendOption(rig.a2(), 1, QStringLiteral("preFader"), true).isEmpty());
    REQUIRE(rig.mixer.setSendOption(rig.a2(), 1, QStringLiteral("muted"), true).isEmpty());
    undo += 2;
    CHECK(rig.undo() == undo);
    const QVariantMap a2 = stripOf(rig.mixer.strips(), rig.a2());
    const QVariantList sends = a2.value(QStringLiteral("sends")).toList();
    REQUIRE(sends.size() == 1);
    CHECK(sends.front().toMap().value(QStringLiteral("preFader")).toBool());
    CHECK(sends.front().toMap().value(QStringLiteral("muted")).toBool());
    CHECK(sends.front().toMap().value(QStringLiteral("levelDb")).toDouble() == Catch::Approx(-12.0));

    // "Create Mono Submix" from the send menu: a new submix and the send to it, one step.
    REQUIRE(rig.mixer.addSend(rig.a2(), QStringLiteral("new:mono")).isEmpty());
    CHECK(rig.undo() == ++undo);
    REQUIRE(rig.seq().submixTracks.size() == 2);
    CHECK(rig.seq().submixTracks[1]->format == ChannelFormat::Mono);
    CHECK(rig.seq().findTrack(rig.tl.track("A2"))->sends.size() == 2);

    // The submix fader: its Volume, one step per drag.
    for (double db = 0; db >= -6; db -= 1)
        REQUIRE(rig.mixer.setGainDb(bus, db).isEmpty());
    rig.mixer.endFaderGesture();
    CHECK(rig.undo() == ++undo);
    CHECK(rig.seq().findSubmix(parseTrackKey(bus).value())->volume == ParamValue(-6.0));
    CHECK(rig.mixer.faderValues().value(bus).toMap().value(QStringLiteral("gainDb")).toDouble() == Catch::Approx(-6.0));

    // Removing a send, then the submix: A1 goes back to the Mix.
    REQUIRE(rig.mixer.removeSend(rig.a2(), 1).isEmpty());
    REQUIRE(rig.mixer.removeSubmix(bus).isEmpty());
    CHECK(rig.seq().submixTracks.size() == 1);
    CHECK_FALSE(rig.seq().findTrack(rig.tl.track("A1"))->output.isValid());
    CHECK(stripOf(rig.mixer.strips(), rig.a1()).value(QStringLiteral("output")).toString() == QStringLiteral("mix"));
}

TEST_CASE("Mixer effect menu lists the Premiere audio effects by category and puts them on tracks and submixes",
          "[app][mixer][submix]") {
    RoutingRig rig;
    QStringList ids;
    QStringList names;
    for (const QVariant& t : rig.mixer.effectTypes()) {
        ids << t.toMap().value(QStringLiteral("id")).toString();
        names << t.toMap().value(QStringLiteral("name")).toString();
    }
    CHECK(ids.removeDuplicates() == 0);
    for (const char* id : {"wabcut.audio.analogDelay", "wabcut.audio.graphicEq10", "wabcut.audio.pitchShifter",
                           "wabcut.audio.convolutionReverb", "wabcut.audio.multibandCompressor", "wabcut.audio.swapChannels"})
        CHECK(ids.contains(QString::fromLatin1(id)));
    const auto at = ids.indexOf(QStringLiteral("wabcut.audio.analogDelay"));
    REQUIRE(at >= 0);
    CHECK(names[at] == QStringLiteral("Delay and Echo › Analog Delay"));

    REQUIRE(rig.mixer.addSubmix(QStringLiteral("stereo")).isEmpty());
    const QString bus = rig.mixer.submixes().front().toMap().value(QStringLiteral("key")).toString();
    CHECK(rig.mixer.addEffect(rig.a1(), QStringLiteral("wabcut.audio.analogDelay")).isEmpty());
    CHECK(rig.mixer.addEffect(bus, QStringLiteral("wabcut.audio.studioReverb")).isEmpty());
    REQUIRE(rig.seq().findTrack(rig.tl.track("A1"))->effects.size() == 1);
    CHECK(rig.seq().findTrack(rig.tl.track("A1"))->effects.front().effect == "wabcut.audio.analogDelay");
    REQUIRE(rig.seq().submixTracks.front()->effects.size() == 1);
    CHECK(rig.seq().submixTracks.front()->effects.front().effect == "wabcut.audio.studioReverb");
}

TEST_CASE("Screenshot: the Audio Track Mixer with submix strips and sends", "[app][mixer][submix][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(controller);
    REQUIRE(mixer);
    RoutingRig fixture; // only its timeline
    controller->resetProject(fixture.tl.state, fixture.tl.seq);
    const QString a1 = trackKey(fixture.tl.track("A1"));
    const QString a2 = trackKey(fixture.tl.track("A2"));
    REQUIRE(mixer->addSubmix(QStringLiteral("stereo")).isEmpty());
    const QString verb = mixer->submixes().front().toMap().value(QStringLiteral("key")).toString();
    REQUIRE(mixer->setOutput(a1, QStringLiteral("new:stereo")).isEmpty());
    REQUIRE(mixer->addSend(a1, verb).isEmpty());
    REQUIRE(mixer->addSend(a2, verb).isEmpty());
    REQUIRE(mixer->setSendLevel(a2, 1, -9).isEmpty());
    mixer->endSendGesture();
    REQUIRE(mixer->addEffect(verb, QStringLiteral("wabcut.audio.reverb")).isEmpty());
    mixer->setMeterSource([](int index) -> std::optional<AudioMixerBridge::MeterReading> {
        switch (index) {
        case 0: return AudioMixerBridge::MeterReading{2, {0.52f, 0.48f}, {0.22f, 0.2f}, false};
        case 1: return AudioMixerBridge::MeterReading{2, {0.3f, 0.34f}, {0.12f, 0.14f}, false};
        case -1: return AudioMixerBridge::MeterReading{2, {0.7f, 0.66f}, {0.3f, 0.29f}, false};
        case -2: return AudioMixerBridge::MeterReading{2, {0.4f, 0.42f}, {0.18f, 0.19f}, false};
        case -3: return AudioMixerBridge::MeterReading{2, {0.6f, 0.58f}, {0.25f, 0.24f}, false};
        default: return std::nullopt;
        }
    });
    shell.window->setProperty("workspace", 1);
    spin(500);
    mixer->pollMeters();
    spin(100);
    QQuickItem* content = shell.window->contentItem();
    QQuickItem* strip = routingItem(content, QStringLiteral("submixStrip_S1"));
    REQUIRE(strip);
    CHECK(strip->isVisible());
    CHECK(routingItem(content, QStringLiteral("submixStrip_S2")));
    CHECK(routingItem(content, QStringLiteral("sendSlot_0")));
    CHECK(mixer->meter(verb).channels == 2);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("mixer-submixes.png"))));
    mixer->setMeterSource({});
    shell.window->setProperty("workspace", 0);
    spin(100);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
