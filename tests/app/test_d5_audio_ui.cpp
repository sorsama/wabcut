// 0.7 (stream D, batch D5) in the shell:
//   card 032  the Audio Track Mixer's menu: Show/Hide Tracks (a hidden strip stays in the mix; Show All; the checklist on
//             Ctrl+Alt+T), Meter Input(s) Only (the armed strip shows the microphone, the others the floor) and Loop
//             (audioMixer.loop is the Program Monitor's Loop);
//   card 045  the Audio Tools panel's Match tab: choose a reference and target clips, Learn (spectra and levels, nothing
//             changes), the curve refitted by Amount and Smoothing, Apply EQ / Preview / Reset / Match level, one undo step each;
//   card 104  the Loudness panel beside the timeline (Window > Loudness): the live meter;
//   card 141  the Loudness Radar (live lap and the programme of the last analysis);
//   card 075  Convert to graphics in the Captions track menu.
// Screenshots of the new pieces (dark and light) go to <build>/test-output/ui.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "CommandJob.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/model/CaptionOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <tuple>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::audio::edit;
using Catch::Approx;
using command::Json;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

/// The fake render of the Match tab: a spectrum and levels per media, the Graphic EQ sliders added to the spectrum and the
/// gains to the levels, and a short-term series for the radar.
class FakeRender final : public LoudnessAnalyzer {
public:
    struct Signal {
        double tilt = 0.0;
        double base = -60.0;
        double lufs = -23.0;
        double truePeak = -10.0;
    };
    explicit FakeRender(std::map<MediaId, Signal> media) : m_media(std::move(media)) {}

    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange>, command::ExecutionControl*) override {
        const Clip& clip = clipOf(state, sequence, target);
        LoudnessStats stats;
        stats.integratedLufs = m_media.at(clip.media).lufs;
        stats.truePeakDbtp = m_media.at(clip.media).truePeak;
        stats.rangeLu = 7.5;
        stats.duration = Ticks::fromSeconds(120);
        // 120 seconds of short-term loudness: steady at the integrated value, a loud passage and a quiet one.
        for (int i = 0; i < 120; ++i) {
            stats.shortTerm.push_back(i < 2 ? static_cast<float>(-kInf)
                                             : static_cast<float>(stats.integratedLufs + (i >= 60 && i < 70 ? 6.0 : i >= 90 && i < 100 ? -8.0 : 0.0)));
        }
        return stats;
    }

    std::expected<MatchStats, Error> measureMatch(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                  std::optional<TimeRange>, command::ExecutionControl*) override {
        const Clip& clip = clipOf(state, sequence, target);
        const Signal s = m_media.at(clip.media);
        MatchStats out;
        out.channels = 2;
        out.sampleRate = 48000;
        double gain = clip.properties().value(ParamId::VolumeDb);
        for (int b = 0; b < kMatchBandCount; ++b)
            out.spectrum.levelDb[static_cast<std::size_t>(b)] = s.base + s.tilt * (b - 15);
        for (const EffectInstance& e : clip.effects()) {
            if (!e.enabled)
                continue;
            if (e.effect == "wabcut.audio.gain")
                gain += effectParameterAt(e, "gain", Ticks{});
            if (e.effect == "wabcut.audio.graphicEq30") {
                for (int b = 0; b < kMatchBandCount; ++b)
                    out.spectrum.levelDb[static_cast<std::size_t>(b)] += effectParameterAt(e, "band" + std::to_string(b + 1), Ticks{});
            }
        }
        out.spectrum.seconds = 10.0;
        out.spectrum.activeSeconds = 10.0;
        out.spectrum.sampleRate = 48000;
        out.loudness.integratedLufs = s.lufs + gain;
        out.loudness.truePeakDbtp = s.truePeak + gain;
        out.loudness.duration = Ticks::fromSeconds(10);
        out.rmsDbfs = s.lufs + gain - 4.0;
        out.crestDb = 13.0;
        return out;
    }

private:
    static const Clip& clipOf(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target) {
        const Sequence& seq = *state.findSequence(sequence);
        if (target.scope == LoudnessTarget::Scope::Clip)
            return seq.findClip(target.clip)->clip();
        return seq.audioTracks.front()->clips.front();
    }
    std::map<MediaId, Signal> m_media;
};

std::shared_ptr<AudioServices> fakeServices(const Timeline& tl) {
    return std::make_shared<AudioServices>(
        std::make_shared<FakeRender>(std::map<MediaId, FakeRender::Signal>{
            {tl.media('A'), {0.0, -60.0, -30.0, -20.0}}, {tl.media('M'), {0.3, -56.0, -24.0, -14.0}}, {tl.media('B'), {-0.1, -64.0, -40.0, -30.0}}}),
        nullptr);
}

struct Fixture {
    Timeline tl{R"(
        V1 tgt          |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A2 music        |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
        A3              |Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )"};
    ProjectController controller;
    AudioMixerBridge mixer;

    Fixture() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        mixer.setController(&controller);
        mixer.setServices(fakeServices(tl));
        mixer.setSynchronous(true);
    }
    QVariantMap strip(int i) const { return mixer.strips().value(i).toMap(); }
    QString key(int i) const { return strip(i).value(QStringLiteral("key")).toString(); }
    std::size_t undoCount() { return controller.session().undoCount(); }
    ClipId clipOn(int track) const { return controller.currentSequence()->audioTracks[static_cast<std::size_t>(track)]->clips.front().id; }
    const Clip& clip(int track) const { return controller.currentSequence()->audioTracks[static_cast<std::size_t>(track)]->clips.front(); }
    bool hasMatch(int track) const {
        for (const EffectInstance& e : clip(track).effects()) {
            if (e.effect == "wabcut.audio.graphicEq30" && e.findData("eqMatch") != nullptr)
                return true;
        }
        return false;
    }
};

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

void runCommand(const Shell& shell, const char* id) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(id))));
}

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void saveCrop(const Shell& shell, const QRectF& area, const QString& name) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(name)));
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

} // namespace

// ---- Card 032: the Audio Track Mixer's menu --------------------------------------------------------------------------

TEST_CASE("Show/Hide Tracks: a hidden strip is flagged, counted and still in the mix; Show All brings them back", "[app][audio][mixer][d5]") {
    Fixture fx;
    AudioMixerBridge& m = fx.mixer;
    REQUIRE(m.strips().size() == 3);
    CHECK(m.hiddenCount() == 0);
    int changed = 0;
    QObject::connect(&m, &AudioMixerBridge::stripsChanged, [&] { ++changed; });
    const std::size_t undo = fx.undoCount();
    const std::uint64_t revision = fx.controller.session().revision();

    m.setTrackHidden(fx.key(1), true);
    CHECK(fx.strip(1).value("hidden").toBool());
    CHECK_FALSE(fx.strip(0).value("hidden").toBool());
    CHECK(m.hiddenCount() == 1);
    CHECK(m.isTrackHidden(fx.key(1)));
    CHECK(changed >= 1);
    // Only the mixer's room: no undo step, the project is the same, the track still plays (not muted).
    CHECK(fx.undoCount() == undo);
    CHECK(fx.controller.session().revision() == revision);
    CHECK_FALSE(fx.strip(1).value("muted").toBool());

    m.setTrackHidden(fx.key(2), true);
    CHECK(m.hiddenCount() == 2);
    m.setTrackHidden(QStringLiteral("t99999"), true); // not a strip of this sequence
    m.setTrackHidden(QStringLiteral("master"), true);
    CHECK(m.hiddenCount() == 2);
    m.setTrackHidden(fx.key(1), false);
    CHECK(m.hiddenCount() == 1);
    m.showAllTracks();
    CHECK(m.hiddenCount() == 0);
    CHECK_FALSE(fx.strip(2).value("hidden").toBool());
    m.showAllTracks(); // nothing hidden: no change signal storm, no harm
    CHECK(m.hiddenCount() == 0);
}

TEST_CASE("Meter Input(s) Only: the armed strip shows the microphone, the others the floor, the master the mix", "[app][audio][mixer][d5]") {
    Fixture fx;
    AudioMixerBridge& m = fx.mixer;
    REQUIRE(command::CommandResult(CommandJob::runNow(&fx.controller, nullptr, "track.setRecordArm", Json{{"track", "A1"}, {"armed", true}})).ok());
    REQUIRE(fx.strip(0).value("recordArmed").toBool());
    std::map<int, AudioMixerBridge::MeterReading> feed;
    feed[0] = {2, {0.5f, 0.5f}, {0.25f, 0.25f}, false};
    feed[1] = {2, {0.4f, 0.4f}, {0.2f, 0.2f}, false};
    feed[-1] = {2, {0.3f, 0.3f}, {0.1f, 0.1f}, false};
    m.setMeterSource([&](int index) -> std::optional<AudioMixerBridge::MeterReading> {
        const auto it = feed.find(index);
        return it == feed.end() ? std::nullopt : std::optional(it->second);
    });
    bool open = true;
    AudioMixerBridge::MeterLevels input;
    input.channels = 2;
    input.peakDb.fill(-12.0);
    input.rmsDb.fill(-18.0);
    input.holdDb.fill(-12.0);
    m.setInputSource([&]() -> std::optional<AudioMixerBridge::MeterLevels> { return open ? std::optional(input) : std::nullopt; });
    m.pollMeters();
    const double aboutMinusSix = 20.0 * std::log10(0.5);
    CHECK(m.meter(fx.key(0)).peakDb[0] == Approx(aboutMinusSix).margin(0.1));

    int updates = 0;
    QObject::connect(&m, &AudioMixerBridge::metersUpdated, [&] { ++updates; });
    m.setMeterInputsOnly(true);
    CHECK(m.meterInputsOnly());
    CHECK(updates >= 1);
    // The armed track shows the input; the other track is empty (-inf); the master keeps metering the mix.
    CHECK(m.meter(fx.key(0)).peakDb[0] == Approx(-12.0));
    CHECK(m.meter(fx.key(0)).rmsDb[1] == Approx(-18.0));
    CHECK(m.meter(fx.key(0)).channels == 2);
    CHECK_FALSE(std::isfinite(m.meter(fx.key(1)).peakDb[0]));
    CHECK_FALSE(std::isfinite(m.meter(fx.key(1)).rmsDb[1]));
    CHECK(m.meter(QStringLiteral("master")).peakDb[0] == Approx(20.0 * std::log10(0.3)).margin(0.1));
    // The microphone closed: the tracks meter their playback again.
    open = false;
    CHECK(m.meter(fx.key(1)).peakDb[0] == Approx(20.0 * std::log10(0.4)).margin(0.1));
    open = true;
    m.setMeterInputsOnly(false);
    CHECK_FALSE(m.meterInputsOnly());
    CHECK(m.meter(fx.key(1)).peakDb[0] == Approx(20.0 * std::log10(0.4)).margin(0.1));
}

TEST_CASE("Loop in the Audio Track Mixer is the Program Monitor's Loop", "[app][audio][mixer][d5][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    REQUIRE(playback != nullptr);
    const bool before = playback->loop();
    runCommand(shell, "audioMixer.loop");
    CHECK(playback->loop() == !before);
    runCommand(shell, "audioMixer.loop");
    CHECK(playback->loop() == before);
    // And the Meter Input(s) Only key flips the mixer's switch.
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(mixer != nullptr);
    CHECK_FALSE(mixer->meterInputsOnly());
    runCommand(shell, "audioMixer.meterInputsOnly");
    CHECK(mixer->meterInputsOnly());
    runCommand(shell, "audioMixer.meterInputsOnly");
    CHECK_FALSE(mixer->meterInputsOnly());
}

TEST_CASE("The mixer menu and the Show/Hide Tracks checklist; screenshots", "[app][audio][mixer][d5][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(controller != nullptr);
    REQUIRE(mixer != nullptr);
    Timeline tl{R"(
        V1 tgt          |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A2 music        |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
        A3              |Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )"};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    spin(200);
    runCommand(shell, "window.audioTrackMixer");
    spin(300);
    QQuickItem* panel = shell.item("audioMixerPanel");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->isVisible());
    QQuickItem* menuButton = findNamed(panel, QStringLiteral("mixerMenuButton"));
    REQUIRE(menuButton != nullptr);
    CHECK(menuButton->isVisible());

    // Ctrl+Alt+T: the checklist opens with a row per strip, all on.
    runCommand(shell, "audioMixer.showHideTracks");
    spin(300);
    QObject* popover = shell.window->findChild<QObject*>(QStringLiteral("showHideTracksPopover"));
    REQUIRE(popover != nullptr);
    CHECK(popover->property("opened").toBool());
    CHECK(popover->property("rows").toList().size() == 3);
    // Hide the music track through the checklist's own switch.
    const QString musicKey = mixer->strips().value(1).toMap().value(QStringLiteral("key")).toString();
    QQuickItem* toggle = findNamed(shell.window->contentItem(), QStringLiteral("showHideTrack_") + musicKey);
    REQUIRE(toggle != nullptr);
    CHECK(toggle->property("checked").toBool());
    // The switch flipped the way the repo's other tests do (toggle() does not emit the user's `toggled`: it is sent too).
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
    spin(200);
    CHECK(mixer->hiddenCount() == 1);
    CHECK(mixer->isTrackHidden(musicKey));
    // The strip left the row; the others stay.
    QQuickItem* strips = findNamed(panel, QStringLiteral("mixerStrips"));
    REQUIRE(strips != nullptr);
    int shown = 0;
    std::function<void(QQuickItem*)> count = [&](QQuickItem* item) {
        for (QQuickItem* child : item->childItems()) {
            if (child->objectName().startsWith(QStringLiteral("mixerStrip_A")) && child->isVisible())
                ++shown;
            count(child);
        }
    };
    count(strips);
    CHECK(shown == 2);
    QQuickItem* card = findNamed(shell.window->contentItem(), QStringLiteral("showHideTracksScroll"));
    REQUIRE(card != nullptr);
    const QRectF shot = sceneRect(panel).adjusted(-8, -8, 8, 8).united(sceneRect(card).adjusted(-30, -80, 30, 90));
    saveCrop(shell, shot, QStringLiteral("mixer-show-hide-tracks.png"));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(100);
    saveCrop(shell, shot, QStringLiteral("mixer-show-hide-tracks-light.png"));
    mixer->showAllTracks();
    CHECK(mixer->hiddenCount() == 0);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Card 045: EQ Match and Level Match -----------------------------------------------------------------------------

TEST_CASE("Match: learn from a clip, refit with Amount, apply, preview, reset and match the level, one undo step each", "[app][audio][match][d5]") {
    Fixture fx;
    AudioMixerBridge& m = fx.mixer;
    QVariantMap state = m.match();
    CHECK_FALSE(state.value("reference").toMap().value("set").toBool());
    CHECK_FALSE(state.value("learned").toBool());
    // Nothing chosen: Learn says what to choose.
    CHECK_FALSE(m.matchLearn().isEmpty());
    CHECK_FALSE(m.matchUseSelectedClip().isEmpty());
    CHECK_FALSE(m.matchUseSelectedTargets().isEmpty());

    // Reference: the music clip (A2); target: the dialogue clip (A1).
    fx.controller.setSelection({fx.clipOn(1)});
    REQUIRE(m.matchUseSelectedClip().isEmpty());
    CHECK(m.match().value("reference").toMap().value("set").toBool());
    fx.controller.setSelection({fx.clipOn(0)});
    REQUIRE(m.matchUseSelectedTargets().isEmpty());
    CHECK(m.match().value("target").toMap().value("count").toInt() == 1);

    const std::size_t undo = fx.undoCount();
    REQUIRE(m.matchLearn().isEmpty());
    state = m.match();
    CHECK(fx.undoCount() == undo); // learning changes nothing
    REQUIRE(state.value("learned").toBool());
    const QVariantList bands = state.value("bands").toList();
    REQUIRE(bands.size() == 30);
    CHECK(state.value("referenceStats").toMap().value("integrated").toDouble() == Approx(-24.0));
    CHECK(state.value("targetStats").toMap().value("integrated").toDouble() == Approx(-30.0));
    CHECK(state.value("levelGainDb").toDouble() == Approx(6.0));
    // The curve follows the tilt of 0.3 dB per band (smoothing 50 % leaves a straight line alone away from the ends).
    CHECK(bands[15].toMap().value("gainDb").toDouble() == Approx(0.3 * (15 - 15.5)).margin(0.02));
    // Amount scales the curve at once, with no new measuring.
    const double full = bands[20].toMap().value("gainDb").toDouble();
    m.setMatchOption(QStringLiteral("amount"), 50.0);
    CHECK(m.match().value("bands").toList()[20].toMap().value("gainDb").toDouble() == Approx(full / 2.0).margin(1e-6));
    m.setMatchOption(QStringLiteral("amount"), 100.0);

    // Apply EQ: a marked Graphic EQ on the target clip, one undo step; Preview switches it off and on; Reset removes it.
    REQUIRE(m.matchApplyEq().isEmpty());
    CHECK(fx.undoCount() == undo + 1);
    CHECK(fx.hasMatch(0));
    CHECK(m.match().value("applied").toInt() == 1);
    CHECK(m.match().value("bypassed").toInt() == 0);
    REQUIRE(m.matchTogglePreview().isEmpty());
    CHECK(m.match().value("bypassed").toInt() == 1);
    CHECK(fx.undoCount() == undo + 2);
    REQUIRE(m.matchTogglePreview().isEmpty());
    CHECK(m.match().value("bypassed").toInt() == 0);
    REQUIRE(m.matchReset().isEmpty());
    CHECK_FALSE(fx.hasMatch(0));
    CHECK(m.match().value("applied").toInt() == 0);
    CHECK_FALSE(m.matchTogglePreview().isEmpty()); // nothing to switch

    // Match level: the dialogue moves from -30 to the music's -24 LUFS; a report with before and after.
    const std::size_t beforeLevel = fx.undoCount();
    REQUIRE(m.matchApplyLevel().isEmpty());
    CHECK(fx.undoCount() == beforeLevel + 1);
    const QVariantList report = m.match().value("report").toList();
    REQUIRE(report.size() == 1);
    CHECK(report[0].toMap().value("target").toString() == QStringLiteral("A")); // the clip's name, not its id
    CHECK(report[0].toMap().value("before").toMap().value("integrated").toDouble() == Approx(-30.0));
    CHECK(report[0].toMap().value("after").toMap().value("integrated").toDouble() == Approx(-24.0).margin(0.1));
    CHECK(report[0].toMap().value("gainDb").toDouble() == Approx(6.0).margin(0.1));
    CHECK(fx.controller.currentSequence()->audioTracks[0]->clips.front().effects().front().effect == "wabcut.audio.gain");
    // What was learned stays on show; the target's levels are the matched ones now and the gain left to apply is nothing.
    state = m.match();
    CHECK(state.value("learned").toBool());
    CHECK(state.value("targetStats").toMap().value("integrated").toDouble() == Approx(-24.0).margin(0.1));
    CHECK(state.value("levelGainDb").toDouble() == Approx(0.0).margin(0.1));
}

// ---- Cards 104 and 141: the Loudness panel and the radar -------------------------------------------------------------

TEST_CASE("The Loudness Radar folds the live history by lap and the analysed programme in one turn", "[app][audio][loudness][radar][d5]") {
    Fixture fx;
    AudioMixerBridge& m = fx.mixer;
    // Live: 130 seconds at -23 with a shout at the end; a 60 s lap.
    std::vector<float> history(130, -23.0F);
    for (std::size_t i = 120; i < 130; ++i)
        history[i] = -14.0F;
    m.setLoudnessHistorySource([&] { return history; });
    audio::LoudnessReading reading;
    reading.momentaryLufs = -20.0;
    reading.shortTermLufs = -23.0;
    reading.integratedLufs = -23.5;
    reading.truePeakDbtp = -3.0;
    reading.rangeLu = 6.5;
    reading.frames = 48000 * 130;
    m.setLoudnessSource([&]() -> std::optional<audio::LoudnessReading> { return reading; });

    CHECK_FALSE(m.radar().value("valid").toBool()); // not shown: nothing folded
    m.setRadarLap(60);
    m.setRadarShown(true);
    QVariantMap radar = m.radar();
    CHECK(radar.value("valid").toBool());
    CHECK(radar.value("mode").toString() == QStringLiteral("live"));
    CHECK(radar.value("slots").toInt() == 60);
    CHECK(radar.value("lap").toInt() == 60);
    CHECK(radar.value("head").toInt() == 9); // second 130 is the 10th second of the third lap
    const QVariantList values = radar.value("values").toList();
    REQUIRE(values.size() == 60);
    CHECK(values[0].toDouble() == Approx(-14.0)); // seconds 120 to 129 are the third lap's first ten slots: the shout
    CHECK(values[9].toDouble() == Approx(-14.0));
    CHECK(values[10].toDouble() == Approx(-23.0)); // the rest of the ring is still the lap before
    CHECK(radar.value("target").toDouble() == Approx(m.targetLufs()));
    CHECK(radar.value("text").toString().contains(QStringLiteral("LU")));
    // The live reading carries the range and the true peak.
    m.pollMeters();
    CHECK(m.liveLoudness().value("rangeText").toString() == QStringLiteral("6.5"));
    // A longer lap shares slots; the lap is one of the five choices.
    m.setRadarLap(3600);
    CHECK(m.radarLap() == 3600);
    CHECK(m.radar().value("slots").toInt() == 360);
    m.setRadarLap(250); // nearest of 60, 300, 600, 1800, 3600
    CHECK(m.radarLap() == 300);

    // Programme: nothing analysed yet.
    m.setRadarMode(QStringLiteral("programme"));
    CHECK_FALSE(m.radar().value("valid").toBool());
    CHECK(m.radar().value("text").toString().contains(QStringLiteral("Analyze")));
    // The analysis of the mix (the fake: 120 s with a loud and a quiet passage).
    fx.controller.setSelection({});
    REQUIRE(m.analyze(QStringLiteral("mix")).isEmpty());
    radar = m.radar();
    REQUIRE(radar.value("valid").toBool());
    CHECK(radar.value("mode").toString() == QStringLiteral("programme"));
    CHECK(radar.value("head").toInt() == -1);
    CHECK(radar.value("slots").toInt() == 120);
    CHECK(radar.value("secondsPerSlot").toDouble() == Approx(1.0));
    CHECK(radar.value("lra").toDouble() == Approx(7.5));
    CHECK(radar.value("lowLufs").isValid());
    CHECK(radar.value("highLufs").toDouble() > radar.value("lowLufs").toDouble());
    const QVariantList programme = radar.value("values").toList();
    CHECK(programme[0].isNull()); // the first seconds of a measurement have no 3 s window
    CHECK(programme[65].toDouble() > programme[80].toDouble() + 5.0); // the loud passage over the steady part
    CHECK(programme[95].toDouble() < programme[80].toDouble() - 5.0); // the quiet one under it
    CHECK_FALSE(radar.value("stale").toBool());
    fx.mixer.setTarget(QStringLiteral("broadcast"));
    CHECK(m.radar().value("target").toDouble() == Approx(-23.0));
}

TEST_CASE("Window > Loudness: the live meter beside the timeline, the radar, light and dark; screenshots", "[app][audio][loudness][radar][d5][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(mixer != nullptr);
    audio::LoudnessReading reading;
    reading.momentaryLufs = -21.4;
    reading.shortTermLufs = -23.2;
    reading.integratedLufs = -23.6;
    reading.truePeakDbtp = -2.1;
    reading.rangeLu = 7.1;
    reading.frames = 48000 * 200;
    mixer->setLoudnessSource([&]() -> std::optional<audio::LoudnessReading> { return reading; });
    std::vector<float> history;
    for (int i = 0; i < 200; ++i)
        history.push_back(static_cast<float>(-23.0 + 4.0 * std::sin(i / 9.0) + (i % 40 > 30 ? 5.0 : 0.0)));
    mixer->setLoudnessHistorySource([&] { return history; });
    mixer->setTarget(QStringLiteral("broadcast"));
    spin(200);
    REQUIRE_FALSE(shell.grab().isNull());

    // The Edit preset has no Loudness panel; Window > Loudness opens it right of the timeline.
    CHECK(evaluate(shell, QStringLiteral("docking.groupOfPanel('loudness') === null")).toBool());
    runCommand(shell, "window.loudness");
    spin(400);
    QQuickItem* panel = shell.item("loudnessPanel");
    REQUIRE(panel != nullptr);
    CHECK(panel->isVisible());
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(timeline != nullptr);
    CHECK(sceneRect(panel).left() >= sceneRect(timeline).right());
    CHECK(std::abs(sceneRect(panel).top() - sceneRect(timeline).top()) < 1.0);
    CHECK(sceneRect(panel).width() > 120);
    // It polls while it shows.
    CHECK(mixer->metersActive());
    CHECK(mixer->isMeterViewer(panel));
    mixer->pollMeters();
    spin(100);

    // The meter view: the numbers and the bars.
    auto text = [&](const char* name) {
        QQuickItem* item = findNamed(panel, QString::fromLatin1(name));
        INFO(name);
        REQUIRE(item != nullptr);
        return item;
    };
    CHECK(text("loudnessShortTerm")->isVisible());
    CHECK(text("loudnessBarShortTerm")->isVisible());
    CHECK(text("loudnessBarMomentary")->isVisible());
    CHECK(text("loudnessMeterView")->isVisible());
    CHECK_FALSE(text("loudnessRadarView")->isVisible());
    saveCrop(shell, sceneRect(panel).united(sceneRect(timeline).adjusted(sceneRect(timeline).width() - 300, 0, 0, 0)).adjusted(-8, -8, 8, 8),
             QStringLiteral("loudness-meter.png"));

    // The radar view.
    panel->setProperty("view", 1);
    spin(300);
    CHECK(mixer->radarShown());
    CHECK(text("loudnessRadarView")->isVisible());
    CHECK_FALSE(text("loudnessMeterView")->isVisible());
    CHECK(mixer->radar().value("valid").toBool());
    QQuickItem* radar = text("loudnessRadar");
    CHECK(radar->isVisible());
    CHECK(radar->width() > 100);
    CHECK(radar->height() > 100);
    saveCrop(shell, sceneRect(panel).adjusted(-8, -8, 8, 8), QStringLiteral("loudness-radar.png"));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(200);
    saveCrop(shell, sceneRect(panel).adjusted(-8, -8, 8, 8), QStringLiteral("loudness-radar-light.png"));
    panel->setProperty("view", 0);
    spin(200);
    saveCrop(shell, sceneRect(panel).united(sceneRect(timeline).adjusted(sceneRect(timeline).width() - 300, 0, 0, 0)).adjusted(-8, -8, 8, 8),
             QStringLiteral("loudness-meter-light.png"));

    // Closed: nothing polls for it.
    evaluate(shell, QStringLiteral("docking.closePanel('loudness')"));
    spin(200);
    CHECK_FALSE(mixer->isMeterViewer(panel));
    CHECK_FALSE(mixer->radarShown());
    mixer->setLoudnessSource({});
    mixer->setLoudnessHistorySource({});
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("The Match tab of Audio Tools; screenshots", "[app][audio][match][d5][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(controller != nullptr);
    REQUIRE(mixer != nullptr);
    Timeline tl{R"(
        V1 tgt          |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A2 music        |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )"};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    mixer->setServices(fakeServices(tl));
    mixer->setSynchronous(true);
    spin(200);

    // Audio Tools is a browser panel (the Audio workspace's slot): open it from the Window menu's key and pick the Match tab.
    evaluate(shell, QStringLiteral("window.focusPanel('audio')"));
    spin(400);
    QQuickItem* panel = shell.item("audioToolsPanel");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->isVisible());
    QQuickItem* tabs = findNamed(panel, QStringLiteral("audioToolsTabs"));
    REQUIRE(tabs != nullptr);
    panel->setProperty("view", 1);
    spin(300);
    QQuickItem* match = findNamed(panel, QStringLiteral("matchTab"));
    REQUIRE(match != nullptr);
    CHECK(match->isVisible());
    CHECK_FALSE(findNamed(panel, QStringLiteral("loudnessTool"))->isVisible());

    // Choose, learn: the spectra and the curve fill in.
    controller->setSelection({controller->currentSequence()->audioTracks[1]->clips.front().id});
    REQUIRE(mixer->matchUseSelectedClip().isEmpty());
    controller->setSelection({controller->currentSequence()->audioTracks[0]->clips.front().id});
    REQUIRE(mixer->matchUseSelectedTargets().isEmpty());
    REQUIRE(mixer->matchLearn().isEmpty());
    spin(300);
    CHECK(findNamed(panel, QStringLiteral("matchReferenceLabel"))->property("text").toString() != QStringLiteral("Nothing chosen yet"));
    QQuickItem* spectrum = findNamed(panel, QStringLiteral("matchSpectrum"));
    REQUIRE(spectrum != nullptr);
    CHECK(spectrum->property("hasData").toBool());
    CHECK(findNamed(panel, QStringLiteral("matchApplyEq"))->property("enabled").toBool());
    CHECK(findNamed(panel, QStringLiteral("matchLimit")) != nullptr);
    const QRectF area = sceneRect(panel).adjusted(-8, -8, 8, 8);
    saveCrop(shell, area, QStringLiteral("audio-match-tab.png"));
    // Taller than the panel: the EQ card, then the level card, are scrolled to.
    QQuickItem* flick = findNamed(match, QStringLiteral("matchScroll"));
    QQuickItem* eqCard = findNamed(match, QStringLiteral("matchEqCard"));
    QQuickItem* levelCard = findNamed(match, QStringLiteral("matchLevelCard"));
    REQUIRE(flick != nullptr);
    REQUIRE(eqCard != nullptr);
    REQUIRE(levelCard != nullptr);
    flick->setProperty("contentY", eqCard->mapToItem(flick->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y() - 8);
    spin(200);
    saveCrop(shell, area, QStringLiteral("audio-match-tab-eq.png"));
    flick->setProperty("contentY", levelCard->mapToItem(flick->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y() - 8);
    spin(200);
    saveCrop(shell, area, QStringLiteral("audio-match-tab-level.png"));
    flick->setProperty("contentY", 0);

    REQUIRE(mixer->matchApplyEq().isEmpty());
    REQUIRE(mixer->matchApplyLevel().isEmpty());
    spin(300);
    CHECK(findNamed(panel, QStringLiteral("matchPreview"))->property("enabled").toBool());
    CHECK(findNamed(panel, QStringLiteral("matchReset"))->property("enabled").toBool());
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(200);
    saveCrop(shell, area, QStringLiteral("audio-match-tab-light.png"));
    flick->setProperty("contentY", eqCard->mapToItem(flick->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y() - 8);
    spin(200);
    saveCrop(shell, area, QStringLiteral("audio-match-tab-eq-light.png"));
    flick->setProperty("contentY", levelCard->mapToItem(flick->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y() - 8);
    spin(200);
    saveCrop(shell, area, QStringLiteral("audio-match-tab-level-light.png"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Card 075: captions to graphics --------------------------------------------------------------------------------

TEST_CASE("Convert to graphics from the Captions panel: graphic clips, the track switched off, one undo step", "[app][captions][d5]") {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    REQUIRE(CommandJob::runNow(&controller, nullptr, "captions.addTrack", Json{{"name", "English"}, {"language", "en"}}).ok());
    for (const auto& [from, to, text] : {std::tuple{0, 50, "First line"}, std::tuple{60, 110, "Second\nline"}}) {
        REQUIRE(CommandJob::runNow(&controller, nullptr, "captions.addCue",
                                   Json{{"start", framesToTicks(from, kRate25).value}, {"end", framesToTicks(to, kRate25).value}, {"text", text}})
                    .ok());
    }
    CaptionsBridge captions;
    captions.setController(&controller);
    const std::size_t undo = controller.session().undoCount();
    CHECK(captions.convertToGraphics(QStringLiteral("hide")).isEmpty());
    CHECK(controller.session().undoCount() == undo + 1);
    const Sequence& seq = *controller.currentSequence();
    REQUIRE(seq.videoTracks.size() == 2);
    REQUIRE(seq.videoTracks[1]->clips.size() == 2);
    CHECK(seq.videoTracks[1]->clips[1].graphic()->layers[0].text.text() == "Second\nline");
    CHECK_FALSE(seq.captionTracks.front().enabled);
    REQUIRE(controller.undo());
    CHECK(controller.currentSequence()->videoTracks.size() == 1);
    CHECK(controller.currentSequence()->captionTracks.front().enabled);
    // No cues: a message, nothing changes.
    CaptionsBridge empty;
    ProjectController other;
    other.resetProject(tl.state, tl.seq);
    empty.setController(&other);
    CHECK_FALSE(empty.convertToGraphics(QStringLiteral("hide")).isEmpty());
}
