// M5l: the Audio Track Mixer on A.3. Offscreen; no window is shown.
//   * the faders and pan knobs are the tracks' Volume and Panner and the master fader (one undo step per drag, the key at
//     the playhead on an automated track, double-click resets);
//   * the automation modes write the expected keys while playing (Read writes nothing; Touch while held; Latch from the
//     first touch to the stop; Write the whole pass), one undo step per pass;
//   * the live loudness readout follows a synthetic loudness feed and resets;
//   * the audio tracks' fader / pan keyframe lanes in the timeline (show / hide).
// Screenshot: the mixer with automation (test-output/ui/mixer-automation.png).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "MixerAutomation.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

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
    const Track& track(const char* name) const {
        const SequencePtr seq = controller.currentSequence();
        return *seq->findTrack(tl.track(name));
    }
    std::size_t undo() { return controller.session().undoCount(); }
    /// Plays the pass from `from` to `to`, ticking the automation every 40 ms; `during(t)` runs at each tick.
    template <class Fn>
    void play(double from, double to, Fn&& during) {
        controller.setPlayhead(seconds(from));
        mixer.setPlayingForTests(true);
        for (double t = from; t <= to + 1e-9; t += 0.04) {
            controller.setPlayhead(seconds(t));
            during(t);
            mixer.pollAutomation();
        }
        mixer.setPlayingForTests(false);
    }
};

} // namespace

TEST_CASE("Automation pass: written stretches replace the keys inside, anchored to what was there", "[app][mixer][automation]") {
    // A constant -6 dB, written over 1-2 s with a dip to -12 dB.
    AutomationSegment dip{seconds(1), seconds(2), {{seconds(1), -6}, {seconds(1.25), -9}, {seconds(1.5), -12}, {seconds(2), -6}}};
    ParamValue written = applyAutomationPass(ParamValue(-6.0), std::span(&dip, 1));
    REQUIRE(written.isCurve());
    const auto& keys = written.curve()->keys;
    // The leading anchor, the samples (1.25 s lies on the line from 1 s to 1.5 s: thinned) and the trailing anchor.
    REQUIRE(keys.size() == 5);
    CHECK(keys.front().time == seconds(1) - kAutomationAnchorGap);
    CHECK(keys.front().value == Catch::Approx(-6));
    CHECK(keys[2].time == seconds(1.5));
    CHECK(keys[2].value == Catch::Approx(-12));
    CHECK(keys.back().time == seconds(2) + kAutomationAnchorGap);
    CHECK(keys.back().value == Catch::Approx(-6));
    CHECK(written.valueAt(seconds(0.5)) == Catch::Approx(-6));
    CHECK(written.valueAt(seconds(1.5)) == Catch::Approx(-12));

    // Keys outside the stretch stay; inside they are replaced; the anchors read the old curve.
    Curve old;
    old.keys = {Keyframe{seconds(0.5), -3}, Keyframe{seconds(1.5), -20}, Keyframe{seconds(3), -10}};
    AutomationSegment flat{seconds(1), seconds(2), {{seconds(1), 0}, {seconds(2), 0}}};
    const ParamValue over = applyAutomationPass(ParamValue(old), std::span(&flat, 1));
    const auto& k = over.curve()->keys;
    CHECK(k.front().time == seconds(0.5));
    CHECK(k.back().time == seconds(3));
    CHECK(std::none_of(k.begin(), k.end(), [](const Keyframe& key) { return key.value == -20; }));
    CHECK(over.valueAt(seconds(1.5)) == Catch::Approx(0));
    CHECK(over.valueAt(seconds(1) - kAutomationAnchorGap) == Catch::Approx(ParamValue(old).valueAt(seconds(1) - kAutomationAnchorGap)));

    // A stretch from the very start has no leading anchor; nothing written keeps the value as it was.
    AutomationSegment start{Ticks{}, seconds(1), {{Ticks{}, -1}, {seconds(1), -1}}};
    CHECK(applyAutomationPass(ParamValue(-6.0), std::span(&start, 1)).curve()->keys.front().time == Ticks{});
    CHECK(applyAutomationPass(ParamValue(-6.0), {}) == ParamValue(-6.0));
    CHECK(automationModeFromName(QStringLiteral("latch")) == AutomationMode::Latch);
    CHECK(automationModeName(AutomationMode::Write) == QStringLiteral("write"));
}

TEST_CASE("Automation pass and another sequence: the keys land in the sequence the pass started on (A5-P2-22)",
          "[app][mixer][automation]") {
    MixerRig r;
    const SequenceId a = r.controller.currentSequenceId();
    r.controller.newSequence(); // B, shown
    const SequenceId b = r.controller.currentSequenceId();
    REQUIRE(b != a);
    r.controller.setCurrentSequenceIndex(0);
    REQUIRE(r.controller.currentSequenceId() == a);
    const auto sequence = [&](SequenceId id) { return r.controller.session().snapshot()->findSequence(id); };
    const ParamValue masterOfB = sequence(b)->masterVolume;

    // Write on A1 and the master, play A, move both faders.
    r.mixer.setAutomationMode(r.a1(), QStringLiteral("write"));
    r.mixer.setAutomationMode(QStringLiteral("master"), QStringLiteral("write"));
    const auto before = r.undo();
    r.controller.setPlayhead(seconds(0));
    r.mixer.setPlayingForTests(true);
    for (double t = 0; t <= 1.0 + 1e-9; t += 0.04) {
        r.controller.setPlayhead(seconds(t));
        if (std::abs(t - 0.5) < 0.02) {
            r.mixer.setGainDb(r.a1(), -10);
            r.mixer.setGainDb(QStringLiteral("master"), -6);
        }
        r.mixer.pollAutomation();
    }
    CHECK(r.undo() == before);

    // Switching to B mid-pass commits the pass to A (one undo step); B's ticks and the stop write nothing.
    r.controller.setCurrentSequenceIndex(1);
    REQUIRE(r.controller.currentSequenceId() == b);
    CHECK(r.undo() == before + 1);
    for (double t = 1.0; t <= 2.0 + 1e-9; t += 0.04) {
        r.controller.setPlayhead(seconds(t));
        r.mixer.pollAutomation();
    }
    r.mixer.setPlayingForTests(false);
    CHECK(r.undo() == before + 1);

    const Sequence* seqA = sequence(a);
    REQUIRE(seqA);
    const Track* a1 = seqA->findTrack(r.tl.track("A1"));
    REQUIRE(a1);
    REQUIRE(a1->volume.isCurve());
    CHECK(a1->volume.valueAt(seconds(0.25)) == Catch::Approx(0));
    CHECK(a1->volume.valueAt(seconds(0.9)) == Catch::Approx(-10));
    REQUIRE(seqA->masterVolume.isCurve());
    CHECK(seqA->masterVolume.valueAt(seconds(0.9)) == Catch::Approx(-6));
    // B is untouched: its master did not get A's lane.
    CHECK(sequence(b)->masterVolume == masterOfB);
}

TEST_CASE("Mixer faders are A.3's track Volume and Panner and the master fader: one undo step per drag",
          "[app][mixer][faders]") {
    MixerRig r;
    CHECK(r.mixer.faderAvailable());
    CHECK(r.mixer.faderNote().isEmpty());
    const auto before = r.undo();
    // A drag: many moves, one step.
    for (const double db : {-1.0, -3.0, -6.0})
        CHECK(r.mixer.setGainDb(r.a1(), db).isEmpty());
    r.mixer.endFaderGesture();
    CHECK(r.undo() == before + 1);
    CHECK(r.track("A1").volume == ParamValue(-6.0));
    CHECK(r.mixer.faderValues().value(r.a1()).toMap().value(QStringLiteral("gainDb")).toDouble() == Catch::Approx(-6));
    CHECK(r.mixer.setPan(r.a1(), -0.4).isEmpty());
    r.mixer.endFaderGesture();
    CHECK(r.track("A1").pan == ParamValue(-0.4));
    CHECK(r.mixer.setGainDb(QStringLiteral("master"), -2.5).isEmpty());
    r.mixer.endFaderGesture();
    CHECK(r.controller.currentSequence()->masterVolume == ParamValue(-2.5));
    CHECK_FALSE(r.mixer.setPan(QStringLiteral("master"), 0.5).isEmpty()); // the master has no pan
    // Double-click: 0 dB / centre, one step each.
    const auto beforeReset = r.undo();
    CHECK(r.mixer.resetGain(r.a1()).isEmpty());
    CHECK(r.mixer.resetPan(r.a1()).isEmpty());
    CHECK(r.undo() == beforeReset + 2);
    CHECK(r.track("A1").volume == ParamValue(0.0));
    CHECK(r.track("A1").pan == ParamValue(0.0));
    // −∞ (the fader's bottom) is the model's floor.
    r.mixer.setGainDb(r.a2(), -std::numeric_limits<double>::infinity());
    r.mixer.endFaderGesture();
    CHECK(r.track("A2").volume == ParamValue(-144.0));

    // An automated track, paused: the fader sets the key at the playhead (added, then moved).
    Curve curve;
    curve.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{seconds(3), -10.0}};
    REQUIRE(r.controller.applyEditFn("Automate", [&](const ProjectState& s) {
        return edit::setParameter(s, r.tl.seq, edit::EffectTarget::ofTrack(r.tl.track("A1")), edit::ParamRef::of(ParamId::VolumeDb),
                                  ParamValue(curve));
    }));
    r.controller.setPlayhead(seconds(1));
    r.mixer.setGainDb(r.a1(), -4);
    r.mixer.endFaderGesture();
    REQUIRE(r.track("A1").volume.isCurve());
    CHECK(r.track("A1").volume.curve()->keys.size() == 3);
    CHECK(r.track("A1").volume.valueAt(seconds(1)) == Catch::Approx(-4));
}

TEST_CASE("Automation modes: Read writes nothing, Touch while held, Latch to the stop, Write the pass; one undo step each",
          "[app][mixer][automation]") {
    SECTION("Write: the whole pass, from where the fader stands") {
        MixerRig r;
        r.mixer.setAutomationMode(r.a1(), QStringLiteral("write"));
        CHECK(r.mixer.automationMode(r.a1()) == QStringLiteral("write"));
        const auto before = r.undo();
        r.play(0.0, 2.0, [&](double t) {
            if (std::abs(t - 1.0) < 0.02)
                r.mixer.setGainDb(r.a1(), -10); // recorded, not applied while playing
            if (t < 1.0)
                CHECK(r.undo() == before);
        });
        CHECK(r.undo() == before + 1);
        const ParamValue& v = r.track("A1").volume;
        REQUIRE(v.isCurve());
        CHECK(v.valueAt(seconds(0.5)) == Catch::Approx(0));
        CHECK(v.valueAt(seconds(1.5)) == Catch::Approx(-10));
        CHECK(r.track("A2").volume == ParamValue(0.0)); // only Write / Touch / Latch strips
        CHECK(r.controller.undoText().contains(QStringLiteral("Automation")));
    }
    SECTION("Touch: writes while held, then returns to the automation") {
        MixerRig r;
        r.mixer.setAutomationMode(r.a1(), QStringLiteral("touch"));
        const auto before = r.undo();
        r.play(0.0, 3.0, [&](double t) {
            if (std::abs(t - 1.0) < 0.02) {
                r.mixer.touch(r.a1(), QStringLiteral("volume"), true);
                r.mixer.setGainDb(r.a1(), -12);
            }
            if (std::abs(t - 1.6) < 0.02)
                r.mixer.touch(r.a1(), QStringLiteral("volume"), false);
        });
        CHECK(r.undo() == before + 1);
        const ParamValue& v = r.track("A1").volume;
        REQUIRE(v.isCurve());
        CHECK(v.valueAt(seconds(0.5)) == Catch::Approx(0));
        CHECK(v.valueAt(seconds(1.3)) == Catch::Approx(-12));
        CHECK(v.valueAt(seconds(2.5)) == Catch::Approx(0)); // back to what was there
    }
    SECTION("Latch: from the first touch to the stop, at the last value") {
        MixerRig r;
        r.mixer.setAutomationMode(r.a1(), QStringLiteral("latch"));
        const auto before = r.undo();
        r.play(0.0, 3.0, [&](double t) {
            if (std::abs(t - 1.0) < 0.02) {
                r.mixer.touch(r.a1(), QStringLiteral("volume"), true);
                r.mixer.setGainDb(r.a1(), -8);
            }
            if (std::abs(t - 1.2) < 0.02)
                r.mixer.touch(r.a1(), QStringLiteral("volume"), false);
        });
        CHECK(r.undo() == before + 1);
        const ParamValue& v = r.track("A1").volume;
        REQUIRE(v.isCurve());
        CHECK(v.valueAt(seconds(0.5)) == Catch::Approx(0));
        CHECK(v.valueAt(seconds(2.5)) == Catch::Approx(-8)); // held after the release
    }
    SECTION("Read: follows the automation, writes nothing") {
        MixerRig r;
        Curve curve;
        curve.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{seconds(2), -20.0}};
        REQUIRE(r.controller.applyEditFn("Automate", [&](const ProjectState& s) {
            return edit::setParameter(s, r.tl.seq, edit::EffectTarget::ofTrack(r.tl.track("A1")),
                                      edit::ParamRef::of(ParamId::VolumeDb), ParamValue(curve));
        }));
        const auto before = r.undo();
        double seen = 0;
        r.play(0.0, 2.0, [&](double t) {
            if (std::abs(t - 1.0) < 0.02)
                seen = r.mixer.faderValues().value(r.a1()).toMap().value(QStringLiteral("gainDb")).toDouble();
        });
        CHECK(r.undo() == before);
        CHECK(seen == Catch::Approx(-10).margin(0.5)); // the fader followed the curve
        CHECK_FALSE(r.mixer.automationActive());
    }
    SECTION("Pan writes too, in its own range") {
        MixerRig r;
        r.mixer.setAutomationMode(r.a2(), QStringLiteral("write"));
        r.play(0.0, 1.0, [&](double t) {
            if (std::abs(t - 0.4) < 0.02)
                r.mixer.setPan(r.a2(), 0.75);
        });
        REQUIRE(r.track("A2").pan.isCurve());
        CHECK(r.track("A2").pan.valueAt(seconds(0.8)) == Catch::Approx(0.75));
    }
}

TEST_CASE("Automation passes are heard while they record: the live mix follows Touch and Write, cleared after",
          "[app][mixer][automation]") {
    MixerRig r;
    struct Call {
        QString target;
        std::optional<double> gain;
        std::optional<double> pan;
    };
    std::vector<Call> calls;
    r.mixer.setLiveMixSink([&](const QString& target, std::optional<double> gain, std::optional<double> pan) {
        calls.push_back({target, gain, pan});
        return true;
    });
    r.mixer.setAutomationMode(r.a1(), QStringLiteral("touch"));
    std::size_t atRelease = 0;
    r.play(0.0, 2.0, [&](double t) {
        if (std::abs(t - 0.5) < 0.02) {
            r.mixer.touch(r.a1(), QStringLiteral("volume"), true);
            r.mixer.setGainDb(r.a1(), -12);
        }
        if (std::abs(t - 1.0) < 0.02) {
            atRelease = calls.size();
            r.mixer.touch(r.a1(), QStringLiteral("volume"), false);
        }
    });
    // Heard while held: the strip's live gain; back to the automation on release.
    const auto held = std::find_if(calls.begin(), calls.end(), [&](const Call& c) {
        return c.target == r.a1() && c.gain && std::abs(*c.gain - -12.0) < 1e-9;
    });
    CHECK(held != calls.end());
    REQUIRE(calls.size() > atRelease);
    CHECK(calls[atRelease].target == r.a1());
    CHECK_FALSE(calls[atRelease].gain.has_value());
    CHECK_FALSE(calls[atRelease].pan.has_value());
    CHECK(std::none_of(calls.begin(), calls.end(), [&](const Call& c) { return c.target == r.a2(); }));

    // Write: heard from the start of the pass, cleared at its stop.
    calls.clear();
    r.mixer.setAutomationMode(r.a2(), QStringLiteral("write"));
    r.play(0.0, 1.0, [&](double t) {
        if (std::abs(t - 0.4) < 0.02)
            r.mixer.setPan(r.a2(), 0.5);
    });
    REQUIRE_FALSE(calls.empty());
    CHECK(std::any_of(calls.begin(), calls.end(), [&](const Call& c) {
        return c.target == r.a2() && c.pan && std::abs(*c.pan - 0.5) < 1e-9;
    }));
    CHECK(calls.back().target == r.a2());
    CHECK_FALSE(calls.back().gain.has_value()); // cleared when the pass was written
}

TEST_CASE("A live fader value the player did not take is sent again on the next tick",
          "[app][mixer][automation][review]") {
    // Fails before A5-P2-26: m_liveSent recorded the value although setLiveTrackMix returned false (the slot not
    // published yet, the first play), so a Write pass on an untouched fader was never heard.
    MixerRig r;
    int refused = 2; // the player takes nothing for the first two ticks
    std::vector<double> applied;
    r.mixer.setLiveMixSink([&](const QString& target, std::optional<double> gain, std::optional<double>) {
        if (target != r.a1() || !gain)
            return true;
        if (refused > 0) {
            --refused;
            return false;
        }
        applied.push_back(*gain);
        return true;
    });
    r.mixer.setAutomationMode(r.a1(), QStringLiteral("write"));
    r.play(0.0, 0.4, [](double) {}); // the fader untouched: its constant written, and heard
    CHECK(refused == 0);
    REQUIRE(applied.size() == 1); // applied once the player took it, then not sent again
}

TEST_CASE("Mixer effect menu: Enhance Speech enhances the track's clips (a clip effect); the A.7 effects go on tracks",
          "[app][mixer]") {
    MixerRig r;
    QStringList ids;
    for (const QVariant& t : r.mixer.effectTypes())
        ids << t.toMap().value(QStringLiteral("id")).toString();
    for (const char* id : {"wabcut.audio.enhanceSpeech", "wabcut.audio.filter", "wabcut.audio.dehum", "wabcut.audio.deess",
                           "wabcut.audio.expander", "wabcut.audio.reverb", "wabcut.audio.width"})
        CHECK(ids.contains(QString::fromLatin1(id)));
    QStringList asked;
    QObject::connect(&r.mixer, &AudioMixerBridge::enhanceClipsRequested, [&](const QStringList& clips) { asked = clips; });
    const auto before = r.undo();
    CHECK(r.mixer.addEffect(r.a1(), QStringLiteral("wabcut.audio.enhanceSpeech")).isEmpty());
    CHECK(asked == r.mixer.trackClips(r.a1()));
    CHECK(asked.size() == 1);
    CHECK(r.undo() == before);          // the dialog does the work (ClipAudio)
    CHECK(r.track("A1").effects.empty()); // never a track effect
    CHECK_FALSE(r.mixer.addEffect(QStringLiteral("master"), QStringLiteral("wabcut.audio.enhanceSpeech")).isEmpty());
    CHECK(r.mixer.addEffect(r.a2(), QStringLiteral("wabcut.audio.reverb")).isEmpty());
    REQUIRE(r.track("A2").effects.size() == 1);
    CHECK(r.track("A2").effects.front().effect == "wabcut.audio.reverb");
}

TEST_CASE("Live loudness follows the player's loudness tap; Reset starts a new measurement", "[app][mixer][loudness]") {
    MixerRig r;
    audio::LoudnessReading reading;
    bool feeding = false;
    r.mixer.setLoudnessSource([&]() -> std::optional<audio::LoudnessReading> {
        if (!feeding)
            return std::nullopt;
        return reading;
    });
    r.mixer.pollMeters();
    CHECK_FALSE(r.mixer.liveLoudness().value(QStringLiteral("valid")).toBool());

    // A synthetic feed: -18 momentary, -16 short-term, -14.6 integrated, -0.4 dBTP (over YouTube's -1 dBTP ceiling).
    feeding = true;
    reading.momentaryLufs = -18.0;
    reading.shortTermLufs = -16.0;
    reading.integratedLufs = -14.6;
    reading.truePeakDbtp = -0.4;
    reading.frames = 48000 * 5;
    r.mixer.pollMeters();
    QVariantMap live = r.mixer.liveLoudness();
    REQUIRE(live.value(QStringLiteral("valid")).toBool());
    CHECK(live.value(QStringLiteral("momentary")).toDouble() == Catch::Approx(-18.0));
    CHECK(live.value(QStringLiteral("integrated")).toDouble() == Catch::Approx(-14.6));
    CHECK(live.value(QStringLiteral("over")).toBool());
    CHECK(live.value(QStringLiteral("deltaText")).toString().contains(QStringLiteral("below")));
    // The feed moves on: the readout follows.
    reading.momentaryLufs = -12.0;
    reading.integratedLufs = -13.0;
    r.mixer.pollMeters();
    live = r.mixer.liveLoudness();
    CHECK(live.value(QStringLiteral("momentary")).toDouble() == Catch::Approx(-12.0));
    CHECK(live.value(QStringLiteral("deltaText")).toString().contains(QStringLiteral("above")));
    // Silence reads −∞, not a number.
    reading.momentaryLufs = -std::numeric_limits<double>::infinity();
    r.mixer.pollMeters();
    CHECK_FALSE(r.mixer.liveLoudness().value(QStringLiteral("momentary")).isValid());
    // Reset: a new integration (the readout empties until the next reading).
    r.mixer.resetLoudness();
    CHECK_FALSE(r.mixer.liveLoudness().value(QStringLiteral("valid")).toBool());
}

TEST_CASE("Track keyframe lanes: the audio tracks' fader or pan line in the timeline, shown or hidden",
          "[app][mixer][timeline]") {
    Rig rig(R"(
        V1 |Aaaaaaaaaaaaaaaaaaaa|
        A1 |Aaaaaaaaaaaaaaaaaaaa|
        A2 |Mmmmmmmmmmmmmmmmmmmm|
    )");
    TimelineView* v = rig.view;
    CHECK(v->trackKeyframes() == QStringLiteral("off"));
    CHECK(v->stats().trackLanes == 0);
    v->setTrackKeyframes(QStringLiteral("volume"));
    rig.harness.frame();
    CHECK(v->stats().trackLanes == 2); // A1 and A2, not V1
    v->setTrackKeyframes(QStringLiteral("pan"));
    rig.harness.frame();
    CHECK(v->stats().trackLanes == 2);
    v->setTrackKeyframes(QStringLiteral("sideways")); // unknown: ignored
    CHECK(v->trackKeyframes() == QStringLiteral("pan"));
    v->setTrackKeyframes(QStringLiteral("off"));
    rig.harness.frame();
    CHECK(v->stats().trackLanes == 0);
}

TEST_CASE("Screenshot: the Audio Track Mixer after an automation pass, with live loudness",
          "[app][mixer][qml][screenshot]") {
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
    MixerRig fixture; // only its timeline
    controller->resetProject(fixture.tl.state, fixture.tl.seq);
    const QString a1 = trackKey(fixture.tl.track("A1"));
    const QString a2 = trackKey(fixture.tl.track("A2"));
    mixer->setSynchronous(true);

    // A pass: A1 in Write (a slow dip), A2 in Touch (held for a second), the master in Read.
    mixer->setAutomationMode(a1, QStringLiteral("write"));
    mixer->setAutomationMode(a2, QStringLiteral("touch"));
    const auto before = controller->session().undoCount();
    controller->setPlayhead(Ticks{});
    mixer->setPlayingForTests(true);
    for (double t = 0; t <= 3.5 + 1e-9; t += 0.04) {
        controller->setPlayhead(seconds(t));
        mixer->setGainDb(a1, -3.0 - 6.0 * std::sin(t * 1.4));
        if (std::abs(t - 1.0) < 0.02) {
            mixer->touch(a2, QStringLiteral("volume"), true);
            mixer->setGainDb(a2, -9);
        }
        if (std::abs(t - 2.0) < 0.02)
            mixer->touch(a2, QStringLiteral("volume"), false);
        mixer->pollAutomation();
    }
    mixer->setPlayingForTests(false);
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(controller->currentSequence()->findTrack(fixture.tl.track("A1"))->volume.isCurve());
    CHECK(controller->currentSequence()->findTrack(fixture.tl.track("A2"))->volume.isCurve());
    controller->setPlayhead(seconds(1.5));

    // Live loudness from a synthetic feed.
    audio::LoudnessReading reading;
    reading.momentaryLufs = -15.2;
    reading.shortTermLufs = -14.8;
    reading.integratedLufs = -14.3;
    reading.truePeakDbtp = -1.6;
    reading.frames = 48000 * 12;
    mixer->setLoudnessSource([reading]() -> std::optional<audio::LoudnessReading> { return reading; });
    mixer->setMeterSource([](int index) -> std::optional<AudioMixerBridge::MeterReading> {
        switch (index) {
        case 0: return AudioMixerBridge::MeterReading{2, {0.52f, 0.48f}, {0.22f, 0.2f}, false};
        case 1: return AudioMixerBridge::MeterReading{2, {0.3f, 0.34f}, {0.12f, 0.14f}, false};
        case -1: return AudioMixerBridge::MeterReading{2, {0.7f, 0.66f}, {0.3f, 0.29f}, false};
        default: return std::nullopt;
        }
    });

    // The Audio workspace with the track volume lanes on.
    shell.window->setProperty("workspace", 1);
    shell.preferences->setProperty("trackKeyframes", QStringLiteral("volume"));
    spin(500);
    mixer->pollMeters();
    REQUIRE(mixer->liveLoudness().value(QStringLiteral("valid")).toBool());
    QQuickItem* mixerPanel = shell.item("audioMixerPanel");
    REQUIRE(mixerPanel);
    CHECK(mixerPanel->isVisible());
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(view);
    CHECK(view->trackKeyframes() == QStringLiteral("volume"));
    view->setPixelsPerSecond(300);
    view->setScrollX(0);
    spin(150);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(view->stats().trackLanes >= 2);
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("mixer-automation.png"))));
    mixer->setLoudnessSource({});
    mixer->setMeterSource({});
    shell.window->setProperty("workspace", 0);
    spin(100);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
