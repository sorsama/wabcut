// M5g: the Audio workspace — the Audio Track Mixer's strips (roles, mute/solo, track and master effect stacks, all
// commands with undo), meters from a synthetic feed (peak, RMS, hold, clip latch, ~30 Hz polling), the audio tools
// (loudness analysis, normalize and auto-duck as dry runs then one undo step, reduce noise), the volume rubber band on
// timeline audio clips (click adds a key, drag moves it, Alt-click makes it Bezier) and Bezier handles in the Effect
// Controls value graph. Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "ClipAudioBridge.h"
#include "EffectControls.h"
#include "MeterBar.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/model/Effects.h>

#include <catch2/catch_test_macros.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::audio::edit;

namespace {

Ticks operator*(Ticks t, std::int64_t n) {
    return Ticks{t.value * n};
}

/// Loudness from a table per media, plus the gains of every Gain effect and the clip's volume (deterministic).
class FakeLoudness final : public LoudnessAnalyzer {
public:
    explicit FakeLoudness(std::map<MediaId, double> media, double fallback = -24.0)
        : m_media(std::move(media)), m_fallback(fallback) {}

    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange>, command::ExecutionControl*) override {
        const Sequence& seq = *state.findSequence(sequence);
        double l = -std::numeric_limits<double>::infinity();
        switch (target.scope) {
        case LoudnessTarget::Scope::Clip:
            l = clipLoudness(seq.findClip(target.clip)->clip());
            break;
        case LoudnessTarget::Scope::Track:
            l = trackLoudness(*seq.findTrack(target.track));
            break;
        case LoudnessTarget::Scope::Mix: {
            double power = 0.0;
            for (const TrackPtr& t : seq.audioTracks)
                power += std::pow(10.0, trackLoudness(*t) / 10.0);
            l = 10.0 * std::log10(power) + gains(seq.audioEffects);
            break;
        }
        }
        LoudnessStats stats;
        stats.integratedLufs = l;
        stats.truePeakDbtp = l + 8.0;
        stats.samplePeakDbfs = l + 7.5;
        stats.rangeLu = 6.5;
        stats.maxMomentaryLufs = l + 5.0;
        stats.maxShortTermLufs = l + 3.0;
        stats.duration = Ticks::fromSeconds(4);
        return stats;
    }

private:
    static double gains(const EffectStack& stack) {
        double g = 0.0;
        for (const EffectInstance& e : stack) {
            if (e.enabled && e.effect == "wabcut.audio.gain")
                g += effectParameterAt(e, "gain", Ticks{});
        }
        return g;
    }
    double clipLoudness(const Clip& c) const {
        const auto it = m_media.find(c.media);
        return (it == m_media.end() ? m_fallback : it->second) + c.properties().value(ParamId::VolumeDb) + gains(c.effects());
    }
    double trackLoudness(const Track& t) const {
        double power = 0.0;
        for (const Clip& c : t.clips)
            power += std::pow(10.0, clipLoudness(c) / 10.0);
        return 10.0 * std::log10(power) + gains(t.effects);
    }

    std::map<MediaId, double> m_media;
    double m_fallback;
};

class FakeSpeech final : public SpeechDetector {
public:
    explicit FakeSpeech(std::vector<TimeRange> speech) : m_speech(std::move(speech)) {}
    std::expected<std::vector<TimeRange>, Error> speech(const ProjectState&, SequenceId, std::span<const TrackId>, SpeechSource,
                                                        std::optional<TimeRange>, command::ExecutionControl*) override {
        return m_speech;
    }

private:
    std::vector<TimeRange> m_speech;
};

Ticks secs(double s) {
    return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))};
}

std::shared_ptr<AudioServices> fakeServices(std::map<MediaId, double> media = {}) {
    return std::make_shared<AudioServices>(
        std::make_shared<FakeLoudness>(std::move(media)),
        std::make_shared<FakeSpeech>(std::vector<TimeRange>{TimeRange::fromStartEnd(secs(1.0), secs(2.0))}));
}

/// Dialogue on A1, music on A2, effects-less A3; 4 s clips at 25 fps.
struct Fixture {
    Timeline tl{R"(
        V1 tgt          |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A2 music        |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
        A3              |Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )"};
    ProjectController controller;
    AudioMixerBridge mixer;

    Fixture() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        mixer.setController(&controller);
        mixer.setServices(fakeServices({{tl.media('A'), -30.0}, {tl.media('M'), -22.0}, {tl.media('B'), -40.0}}));
        mixer.setSynchronous(true);
    }
    QVariantMap strip(int i) const { return mixer.strips().value(i).toMap(); }
    QString key(int i) const { return strip(i).value(QStringLiteral("key")).toString(); }
    QVariantList effects(int i) const { return strip(i).value(QStringLiteral("effects")).toList(); }
    std::size_t undoCount() { return controller.session().undoCount(); }
    const Track& audio(int i) const { return *controller.currentSequence()->audioTracks[static_cast<std::size_t>(i)]; }
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

void mouse(QQuickWindow* window, QEvent::Type type, QPointF p, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, p, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
    QCoreApplication::sendEvent(window, &event);
}

} // namespace

// ---- Mixer strips -----------------------------------------------------------------------------------------------

TEST_CASE("Mixer strips reflect the model: roles, mute/solo, track and master effects; edits are commands with undo",
          "[app][audio]") {
    Fixture fx;
    AudioMixerBridge& m = fx.mixer;
    REQUIRE(m.strips().size() == 3);
    CHECK(fx.strip(0).value("role").toString() == QStringLiteral("dialogue"));
    CHECK(fx.strip(1).value("role").toString() == QStringLiteral("music"));
    CHECK(fx.strip(2).value("role").toString() == QStringLiteral("none"));
    CHECK(fx.strip(0).value("shortName").toString() == QStringLiteral("A1"));
    CHECK(fx.strip(0).value("meter").toInt() == 0);
    CHECK(m.roles().size() == 4);
    CHECK(m.effectTypes().size() == 12); // M5m: Enhance Speech (for the track's clips) and the six A.7 effects

    // Role: track.setRole, one undo step.
    std::size_t undo = fx.undoCount();
    REQUIRE(m.setRole(fx.key(2), QStringLiteral("effects")).isEmpty());
    CHECK(fx.strip(2).value("role").toString() == QStringLiteral("effects"));
    CHECK(fx.strip(2).value("roleName").toString() == QStringLiteral("Effects"));
    CHECK(fx.undoCount() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Set Track Role"));
    REQUIRE(fx.controller.undo());
    CHECK(fx.strip(2).value("role").toString() == QStringLiteral("none"));

    // Mute and solo.
    m.setMuted(fx.key(2), true);
    CHECK(fx.strip(2).value("muted").toBool());
    m.setSolo(fx.key(0), true);
    CHECK(fx.strip(0).value("solo").toBool());

    // The track's effect stack: add, reorder, bypass, remove — each one command.
    undo = fx.undoCount();
    REQUIRE(m.addEffect(fx.key(0), QStringLiteral("wabcut.audio.compressor")).isEmpty());
    REQUIRE(m.addEffect(fx.key(0), QStringLiteral("wabcut.audio.eq3")).isEmpty());
    REQUIRE(fx.effects(0).size() == 2);
    CHECK(fx.undoCount() == undo + 2);
    const QVariantMap compressor = fx.effects(0)[0].toMap();
    CHECK(compressor.value("name").toString() == QStringLiteral("Compressor"));
    CHECK(compressor.value("summary").toString().contains(QStringLiteral("4:1")));
    const QString eq = fx.effects(0)[1].toMap().value("key").toString();
    REQUIRE(m.moveEffect(fx.key(0), eq, 0).isEmpty());
    CHECK(fx.effects(0)[0].toMap().value("key").toString() == eq);
    CHECK(fx.audio(0).effects.front().effect == "wabcut.audio.eq3");
    REQUIRE(m.setEffectEnabled(fx.key(0), compressor.value("key").toString(), false).isEmpty());
    CHECK_FALSE(fx.effects(0)[1].toMap().value("enabled").toBool());
    CHECK_FALSE(fx.audio(0).effects[1].enabled);
    REQUIRE(m.removeEffect(fx.key(0), eq).isEmpty());
    CHECK(fx.effects(0).size() == 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Remove Track or Master Effect"));
    REQUIRE(fx.controller.undo());
    CHECK(fx.effects(0).size() == 2);

    // Master: the limiter preset.
    CHECK_FALSE(m.master().value("hasLimiter").toBool());
    REQUIRE(m.addMasterLimiter().isEmpty());
    CHECK(m.master().value("hasLimiter").toBool());
    const QVariantMap limiter = m.master().value("effects").toList().back().toMap();
    CHECK(limiter.value("effect").toString() == QStringLiteral("wabcut.audio.limiter"));
    CHECK(limiter.value("summary").toString().contains(QStringLiteral("1.0 dBTP")));
    CHECK(fx.controller.currentSequence()->audioEffects.size() == 1);

    // M5l: fader and pan are the track's Volume and Panner (A.3; test_mixer_automation covers them).
    CHECK(m.faderAvailable());
    CHECK(m.setGainDb(fx.key(0), -6.0).isEmpty());
    CHECK(m.setPan(fx.key(0), 0.5).isEmpty());
    m.endFaderGesture();
    CHECK(fx.strip(0).value("gainDb").toDouble() == -6.0);
    CHECK(fx.strip(0).value("pan").toDouble() == 0.5);

    // Bad keys
    CHECK_FALSE(m.addEffect(QStringLiteral("x1"), QStringLiteral("wabcut.audio.gain")).isEmpty());
    CHECK_FALSE(m.setRole(QStringLiteral("master"), QStringLiteral("music")).isEmpty());
}

// ---- Meters -----------------------------------------------------------------------------------------------------

TEST_CASE("Meters follow a synthetic feed: dBFS peak and RMS, peak hold, clip latch, ~30 Hz polling, MeterBar", "[app][audio]") {
    Fixture fx;
    AudioMixerBridge& m = fx.mixer;
    std::map<int, AudioMixerBridge::MeterReading> feed;
    feed[0] = {2, {0.5f, 0.25f}, {0.25f, 0.125f}, false};
    feed[-1] = {2, {1.2f, 0.9f}, {0.6f, 0.5f}, true};
    m.setMeterSource([&](int index) -> std::optional<AudioMixerBridge::MeterReading> {
        const auto it = feed.find(index);
        return it == feed.end() ? std::nullopt : std::optional(it->second);
    });
    int updates = 0;
    QObject::connect(&m, &AudioMixerBridge::metersUpdated, [&] { ++updates; });
    const auto db = [](double linear) { return 20.0 * std::log10(linear); };

    m.pollMeters();
    CHECK(updates == 1);
    AudioMixerBridge::MeterLevels a1 = m.meter(fx.key(0));
    REQUIRE(a1.channels == 2);
    CHECK(std::abs(a1.peakDb[0] - db(0.5)) < 0.01);
    CHECK(std::abs(a1.peakDb[1] - db(0.25)) < 0.01);
    CHECK(std::abs(a1.rmsDb[0] - db(0.25)) < 0.01);
    CHECK(a1.holdDb[0] == a1.peakDb[0]);
    CHECK(m.meter(fx.key(1)).channels == 0); // nothing playing there

    m.pollMeters(); // unchanged: no repaint asked
    CHECK(updates == 1);

    // A lower peak: the hold stays for a moment.
    feed[0].peak = {0.1f, 0.1f};
    m.pollMeters();
    a1 = m.meter(fx.key(0));
    CHECK(std::abs(a1.peakDb[0] - db(0.1)) < 0.01);
    CHECK(std::abs(a1.holdDb[0] - db(0.5)) < 0.01);

    // The master clipped: latched until cleared.
    CHECK(m.meter(QStringLiteral("master")).clipped);
    feed[-1].clipped = false;
    feed[-1].peak = {0.5f, 0.5f};
    m.pollMeters();
    CHECK(m.meter(QStringLiteral("master")).clipped);
    m.clearClip(QStringLiteral("master"));
    CHECK_FALSE(m.meter(QStringLiteral("master")).clipped);

    // MeterBar reads the bridge in C++; QML sees the hold text and the clip light only.
    MeterBar bar;
    bar.setSource(&m);
    bar.setChannel(fx.key(0));
    CHECK(bar.channels() == 2);
    CHECK(bar.peakText() == QStringLiteral("−6.0"));
    CHECK_FALSE(bar.clipped());
    CHECK(bar.fraction(0.0) == 1.0);
    CHECK(bar.fraction(-30.0) == 0.5);
    CHECK(bar.fraction(-std::numeric_limits<double>::infinity()) == 0.0);

    // While the mixer is on screen the bridge polls by itself (~30 Hz).
    m.setMetersActive(true);
    feed[0].peak = {0.7f, 0.7f};
    REQUIRE(waitFor([&] { return std::abs(m.meter(fx.key(0)).peakDb[0] - db(0.7)) < 0.01; }));
    m.setMetersActive(false);
}

// ---- Audio tools ------------------------------------------------------------------------------------------------

TEST_CASE("Audio tools: loudness report, normalize and auto-duck dry run then one undo step, reduce noise", "[app][audio]") {
    Fixture fx;
    AudioMixerBridge& m = fx.mixer;

    // Analyze every track and the mix.
    REQUIRE(m.analyze(QStringLiteral("tracks")).isEmpty());
    REQUIRE(m.report().size() == 4);
    CHECK(m.report().back().toMap().value("kind").toString() == QStringLiteral("mix"));
    const QVariantMap loudness = m.loudness();
    REQUIRE(loudness.value("valid").toBool());
    CHECK_FALSE(loudness.value("stale").toBool());
    CHECK(loudness.value("deltaText").toString().contains(QStringLiteral("LU")));
    CHECK(std::abs(loudness.value("truePeak").toDouble() - (loudness.value("integrated").toDouble() + 8.0)) < 0.02);

    // Normalize the mix to YouTube (-14): a dry run that changes nothing, then Apply as one undo step.
    m.setTarget(QStringLiteral("youtube"));
    CHECK(m.targetLufs() == -14.0);
    const std::uint64_t revision = fx.controller.session().revision();
    std::size_t undo = fx.undoCount();
    REQUIRE(m.previewNormalize(QStringLiteral("mix")).isEmpty());
    QVariantMap pending = m.pending();
    REQUIRE(pending.value("kind").toString() == QStringLiteral("normalize"));
    REQUIRE(pending.value("rows").toList().size() == 1);
    CHECK(std::abs(pending.value("rows").toList()[0].toMap().value("after").toDouble() + 14.0) < 0.2);
    CHECK(fx.controller.session().revision() == revision);
    CHECK(fx.controller.currentSequence()->audioEffects.empty());
    REQUIRE(m.applyPending().isEmpty());
    CHECK(m.pending().isEmpty());
    CHECK(fx.undoCount() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Normalize Loudness"));
    REQUIRE_FALSE(fx.controller.currentSequence()->audioEffects.empty());
    CHECK(fx.controller.currentSequence()->audioEffects.front().effect == "wabcut.audio.gain");
    CHECK(m.loudness().value("stale").toBool()); // measured before the change
    REQUIRE(fx.controller.undo());
    CHECK(fx.controller.currentSequence()->audioEffects.empty());

    // A preview of an older project is dropped.
    REQUIRE(m.previewNormalize(QStringLiteral("mix")).isEmpty());
    REQUIRE_FALSE(m.pending().isEmpty());
    m.setMuted(fx.key(2), true);
    CHECK(m.pending().isEmpty());
    CHECK_FALSE(m.applyPending().isEmpty());

    // Auto-duck the music under the dialogue: the preview, then the keys on the music clip.
    m.setDuckAmount(-15.0);
    undo = fx.undoCount();
    REQUIRE(m.previewAutoDuck().isEmpty());
    pending = m.pending();
    REQUIRE(pending.value("kind").toString() == QStringLiteral("autoDuck"));
    CHECK(pending.value("info").toString().contains(QStringLiteral("keyframes")));
    CHECK(fx.audio(1).clips[0].properties()[ParamId::VolumeDb].isConstant()); // nothing yet
    REQUIRE(m.applyPending().isEmpty());
    CHECK(fx.undoCount() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Auto-Duck Music"));
    const ParamValue ducked = fx.audio(1).clips[0].properties()[ParamId::VolumeDb];
    REQUIRE(ducked.isCurve());
    CHECK(ducked.curve()->keys.size() >= 4);
    double lowest = 0.0;
    for (const Keyframe& k : ducked.curve()->keys)
        lowest = std::min(lowest, k.value);
    CHECK(lowest == -15.0);
    CHECK(fx.audio(0).clips[0].properties()[ParamId::VolumeDb].isConstant()); // the dialogue is untouched

    // Reduce noise on the selected audio clips: added to both in one step, then its amount updated.
    fx.controller.setSelection({fx.audio(0).clips[0].id, fx.audio(2).clips[0].id});
    undo = fx.undoCount();
    REQUIRE(m.reduceNoise().isEmpty());
    CHECK(fx.undoCount() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Reduce Noise"));
    for (int t : {0, 2}) {
        const EffectStack& stack = fx.audio(t).clips[0].effects();
        REQUIRE(stack.size() == 1);
        CHECK(stack[0].effect == "wabcut.audio.denoise");
    }
    m.setDenoiseAmount(0.5);
    REQUIRE(m.reduceNoise().isEmpty());
    CHECK(fx.audio(0).clips[0].effects().size() == 1);
    CHECK(effectParameterAt(fx.audio(0).clips[0].effects()[0], "amount", Ticks{}) == 0.5);
    fx.controller.setSelection({});
    CHECK_FALSE(m.reduceNoise().isEmpty());
}

// ---- Bezier handles -------------------------------------------------------------------------------------------------

TEST_CASE("Value graph: Bezier keys show handles; dragging one shapes the tangents (linked, Alt breaks)", "[app][audio][keyframes]") {
    ProjectController controller;
    EffectControlsModel& fx = *controller.effectControls();
    const ClipId city = controller.primaryClip();
    const Clip clip = controller.currentSequence()->findClip(city)->clip();
    const Ticks frame = controller.frameTicks();
    const QString opacity = QStringLiteral("p:opacity");
    const auto curve = [&] { return *controller.currentSequence()->findClip(city)->clip().properties()[ParamId::Opacity].curve(); };
    // Three keys: 100 %, 40 %, 90 %.
    controller.setPlayhead(clip.start() + frame * 24);
    fx.setAnimated(opacity, true);
    controller.setPlayhead(clip.start() + frame * 96);
    fx.setValue(opacity, 40.0);
    fx.endEdit();
    controller.setPlayhead(clip.start() + frame * 168);
    fx.setValue(opacity, 90.0);
    fx.endEdit();
    REQUIRE(curve().keys.size() == 3);

    // Make the middle key Bezier: smooth tangents (the slope between its neighbours), not flat.
    const auto keyX = [&](int i) {
        return fx.row(opacity).value("keys").toList()[i].toMap().value("x").toDouble();
    };
    REQUIRE(fx.pressKey(opacity, keyX(1), 0.01, 0));
    fx.setKeyInterpolation(QStringLiteral("bezier"));
    Keyframe middle = curve().keys[1];
    CHECK(middle.interpolation == Interpolation::Bezier);
    CHECK(middle.out.slope != 0.0);
    CHECK(middle.in.slope == middle.out.slope);

    // The graph: the curve, the keys at their values, the handles of the Bezier key.
    fx.setGraphOpen(opacity, true);
    QVariantMap graph = fx.row(opacity).value("graph").toMap();
    CHECK(fx.row(opacity).value("graphOpen").toBool());
    CHECK(graph.value("path").toList().size() == 2 * 97);
    QVariantMap key = graph.value("keys").toList()[1].toMap();
    REQUIRE(key.value("hasIn").toBool());
    REQUIRE(key.value("hasOut").toBool());
    CHECK_FALSE(key.value("broken").toBool());
    CHECK(key.value("outX").toDouble() > key.value("x").toDouble());
    CHECK(key.value("inX").toDouble() < key.value("x").toDouble());
    CHECK_FALSE(graph.value("keys").toList()[0].toMap().value("hasOut").toBool()); // linear keys have none
    CHECK(key.value("y").toDouble() > graph.value("keys").toList()[0].toMap().value("y").toDouble()); // 40 % below 100 %

    // Drag the out handle up and further along: steeper and longer; the in handle follows (linked).
    const double outX = key.value("outX").toDouble();
    const double outY = key.value("outY").toDouble();
    REQUIRE(fx.pressHandle(opacity, outX, outY, 0.01, 0.02));
    for (int step = 1; step <= 4; ++step)
        fx.dragHandle(outX + 0.01 * step, outY - 0.08 * step, false);
    fx.endHandleDrag();
    Keyframe shaped = curve().keys[1];
    CHECK(shaped.out.slope > middle.out.slope);
    CHECK(shaped.out.influence > middle.out.influence);
    CHECK(shaped.in.slope == shaped.out.slope);
    CHECK(shaped.in.influence == middle.in.influence);
    CHECK(controller.undoText() == QStringLiteral("Undo Keyframe Handles"));
    REQUIRE(controller.undo());
    CHECK(curve().keys[1].out.slope == middle.out.slope);
    REQUIRE(controller.redo());

    // Alt breaks the pair: the in handle alone.
    key = fx.row(opacity).value("graph").toMap().value("keys").toList()[1].toMap();
    const double inX = key.value("inX").toDouble();
    const double inY = key.value("inY").toDouble();
    REQUIRE(fx.pressHandle(opacity, inX, inY, 0.01, 0.02));
    fx.dragHandle(inX, inY + 0.2, true);
    fx.endHandleDrag();
    const Keyframe broken = curve().keys[1];
    CHECK(broken.in.slope != broken.out.slope);
    CHECK(broken.out.slope == shaped.out.slope);
    CHECK(fx.row(opacity).value("graph").toMap().value("keys").toList()[1].toMap().value("broken").toBool());

    // A press away from the handles does nothing; closing the graph hides them.
    CHECK_FALSE(fx.pressHandle(opacity, 0.99, 0.99, 0.01, 0.01));
    fx.setGraphOpen(opacity, false);
    CHECK_FALSE(fx.row(opacity).value("graphOpen").toBool());
    CHECK_FALSE(fx.pressHandle(opacity, inX, inY, 0.01, 0.02));
}

// ---- In the shell -----------------------------------------------------------------------------------------------

TEST_CASE("Timeline volume rubber band: click adds a key, drag moves it (one undo step), Alt-click makes it Bezier",
          "[app][audio][timeline][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* view = qobject_cast<TimelineView*>(shell.object("timelineView"));
    QObject* timelinePanel = shell.object("timelinePanel");
    REQUIRE(view != nullptr);
    REQUIRE(timelinePanel != nullptr);
    timelinePanel->setProperty("showVolume", true);
    spin(100);
    CHECK(view->showVolume());

    const TrackPtr a1 = controller->currentSequence()->audioTracks[0];
    const Clip& first = a1->clips[0];
    const ClipId clipId = first.id;
    const auto volume = [&] { return controller->currentSequence()->findClip(clipId)->clip().properties()[ParamId::VolumeDb]; };
    REQUIRE(volume().isConstant());
    const auto row = std::ranges::find_if(view->rows(), [&](const TimelineView::Row& r) { return r.track->id == a1->id; });
    REQUIRE(row != view->rows().end());
    const auto xOf = [&](Ticks t) { return t.toSeconds() * view->pixelsPerSecond() - view->scrollX(); };
    const QRectF clipRect(xOf(first.start()), row->y - view->scrollY(), xOf(first.end()) - xOf(first.start()), row->height);
    const QRectF area = TimelineView::volumeArea(clipRect);
    const auto yOf = [&](double db) { return area.top() + TimelineView::volumeFraction(db) * area.height(); };
    const auto scene = [&](double x, double y) { return view->mapToScene(QPointF(x, y)); };
    const double x = clipRect.left() + std::min(120.0, clipRect.width() / 2);
    REQUIRE(x < view->width());

    // Click the line: a key at 0 dB there.
    mouse(shell.window.get(), QEvent::MouseButtonPress, scene(x, yOf(0.0)), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, scene(x, yOf(0.0)), Qt::NoButton);
    spin(50);
    REQUIRE(volume().isCurve());
    REQUIRE(volume().curve()->keys.size() == 1);
    CHECK(volume().curve()->keys[0].value == 0.0);
    CHECK(controller->undoText() == QStringLiteral("Undo Add Volume Keyframe"));

    // Drag the key down (Shift: level only): one undo step.
    const std::size_t undo = controller->session().undoCount();
    const Ticks keyTime = volume().curve()->keys[0].time;
    mouse(shell.window.get(), QEvent::MouseButtonPress, scene(x, yOf(0.0)), Qt::LeftButton);
    for (int step = 1; step <= 4; ++step)
        mouse(shell.window.get(), QEvent::MouseMove, scene(x, yOf(0.0) + 5 * step), Qt::LeftButton, Qt::ShiftModifier);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, scene(x, yOf(0.0) + 20), Qt::NoButton, Qt::ShiftModifier);
    spin(50);
    REQUIRE(volume().curve()->keys.size() == 1);
    const double dragged = volume().curve()->keys[0].value;
    CHECK(dragged < -5.0);
    CHECK(std::abs(dragged - TimelineView::volumeDbAtFraction((yOf(0.0) + 20 - area.top()) / area.height())) < 0.01);
    CHECK(volume().curve()->keys[0].time == keyTime);
    CHECK(controller->session().undoCount() == undo + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Volume Keyframe"));

    // A second key further right, then Alt-click it: Bezier.
    const double x2 = x + 60;
    mouse(shell.window.get(), QEvent::MouseButtonPress, scene(x2, yOf(dragged)), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, scene(x2, yOf(dragged)), Qt::NoButton);
    spin(50);
    REQUIRE(volume().curve()->keys.size() == 2);
    mouse(shell.window.get(), QEvent::MouseButtonPress, scene(x2, yOf(dragged)), Qt::LeftButton, Qt::AltModifier);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, scene(x2, yOf(dragged)), Qt::NoButton, Qt::AltModifier);
    spin(50);
    CHECK(volume().curve()->keys[1].interpolation == Interpolation::Bezier);
    CHECK(controller->undoText() == QStringLiteral("Undo Keyframe Interpolation"));

    // Hidden band: the same press is a clip press again.
    timelinePanel->setProperty("showVolume", false);
    spin(50);
    CHECK_FALSE(view->showVolume());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Audio workspace: mixer strips with meters, tools with a report and an auto-duck preview; screenshots",
          "[app][audio][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(mixer != nullptr);
    mixer->setServices(fakeServices());
    mixer->setSynchronous(true);
    const auto strips = mixer->strips();
    REQUIRE(strips.size() >= 3);
    const auto key = [&](int i) { return strips[i].toMap().value("key").toString(); };

    // A dialogue track with a compressor and an EQ, effects, music; a master limiter.
    REQUIRE(mixer->setRole(key(0), QStringLiteral("dialogue")).isEmpty());
    REQUIRE(mixer->setRole(key(1), QStringLiteral("effects")).isEmpty());
    REQUIRE(mixer->setRole(key(2), QStringLiteral("music")).isEmpty());
    REQUIRE(mixer->addEffect(key(0), QStringLiteral("wabcut.audio.eq3")).isEmpty());
    REQUIRE(mixer->addEffect(key(0), QStringLiteral("wabcut.audio.compressor")).isEmpty());
    REQUIRE(mixer->addEffect(key(2), QStringLiteral("wabcut.audio.gain")).isEmpty());
    REQUIRE(mixer->addMasterLimiter().isEmpty());

    // Live levels from a synthetic feed.
    mixer->setMeterSource([](int index) -> std::optional<AudioMixerBridge::MeterReading> {
        switch (index) {
        case 0: return AudioMixerBridge::MeterReading{2, {0.62f, 0.55f}, {0.28f, 0.24f}, false};
        case 1: return AudioMixerBridge::MeterReading{2, {0.12f, 0.1f}, {0.05f, 0.04f}, false};
        case 2: return AudioMixerBridge::MeterReading{2, {0.35f, 0.4f}, {0.16f, 0.18f}, false};
        case -1: return AudioMixerBridge::MeterReading{2, {0.92f, 1.05f}, {0.42f, 0.45f}, true};
        default: return std::nullopt;
        }
    });

    // The Audio workspace: Audio Tools in the browser, the mixer wide, the volume keyframes on.
    shell.window->setProperty("workspace", 1);
    spin(500);
    CHECK(shell.window->property("browserPanel").toString() == QStringLiteral("audio"));
    CHECK(mixer->metersActive());
    QQuickItem* mixerPanel = shell.item("audioMixerPanel");
    QQuickItem* tools = shell.item("audioToolsPanel");
    REQUIRE(mixerPanel != nullptr);
    REQUIRE(tools != nullptr);
    CHECK(mixerPanel->isVisible());
    CHECK(tools->isVisible());
    CHECK(mixerPanel->width() >= 460);
    auto* view = qobject_cast<TimelineView*>(shell.object("timelineView"));
    REQUIRE(view != nullptr);
    CHECK(view->showVolume());
    REQUIRE(waitFor([&] { return mixer->meter(QStringLiteral("master")).clipped; }));

    // The strips show the model; the meters draw.
    QQuickItem* a1Strip = findItem(shell.window->contentItem(), [](QQuickItem* i) { return i->objectName() == QStringLiteral("mixerStrip_A1"); });
    REQUIRE(a1Strip != nullptr);
    auto* meter = qobject_cast<MeterBar*>(findItem(a1Strip, [](QQuickItem* i) { return i->objectName() == QStringLiteral("meter"); }));
    REQUIRE(meter != nullptr);
    CHECK(meter->channels() == 2);
    REQUIRE_FALSE(shell.grab().isNull()); // a frame renders the meters
    CHECK(meter->drawnRects() >= 4);

    // Tools: the loudness of the tracks and the mix, then an auto-duck preview (ghost keys on the timeline).
    REQUIRE(mixer->analyze(QStringLiteral("tracks")).isEmpty());
    REQUIRE(mixer->report().size() == strips.size() + 1);
    REQUIRE(mixer->previewAutoDuck().isEmpty());
    REQUIRE(mixer->pending().value("kind").toString() == QStringLiteral("autoDuck"));
    REQUIRE(view->externalPreview() != nullptr);
    CHECK_FALSE(view->externalPreview()->volumes.empty());
    spin(150);
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/audio-workspace.png")));
    }

    // Apply: the music ducks under the dialogue (one undo step); the rubber band shows the keys.
    REQUIRE(mixer->applyPending().isEmpty());
    CHECK(view->externalPreview() == nullptr);
    CHECK(controller->undoText() == QStringLiteral("Undo Auto-Duck Music"));
    spin(150);
    {
        const QImage image = shell.grab();
        const double dpr = shell.window->effectiveDevicePixelRatio();
        QQuickItem* timeline = shell.item("timelinePanel");
        REQUIRE(timeline != nullptr);
        const QRectF r = timeline->mapRectToScene(QRectF(0, 0, timeline->width(), timeline->height()));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(outputDir() + QStringLiteral("/timeline-audio-keys.png")));
    }

    // Back to Edit: the browser and the band go back.
    shell.window->setProperty("workspace", 0);
    spin(450);
    CHECK(shell.window->property("browserPanel").toString() == QStringLiteral("library"));
    CHECK_FALSE(view->showVolume());
    CHECK_FALSE(mixer->metersActive());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Effect Controls value graph in the shell: Bezier handles; screenshot", "[app][audio][keyframes][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    EffectControlsModel& fx = *controller->effectControls();
    const ClipId city = controller->primaryClip();
    const Clip clip = controller->currentSequence()->findClip(city)->clip();
    const Ticks frame = controller->frameTicks();
    const QString scale = QStringLiteral("p:scale");
    controller->setPlayhead(clip.start() + frame * 12);
    fx.setAnimated(scale, true);
    for (const auto& [frames, value] : {std::pair{90, 140.0}, std::pair{180, 110.0}, std::pair{300, 160.0}}) {
        controller->setPlayhead(clip.start() + frame * frames);
        fx.setValue(scale, value);
        fx.endEdit();
    }
    const QVariantList keys = fx.row(scale).value("keys").toList();
    REQUIRE(keys.size() == 4);
    for (int i : {1, 2})
        fx.pressKey(scale, keys[i].toMap().value("x").toDouble(), 0.005, i == 1 ? 0 : Qt::ShiftModifier);
    fx.setKeyInterpolation(QStringLiteral("bezier"));
    fx.setGraphOpen(scale, true);
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull()); // a frame lays out the rows (hidden windows polish on render)
    QQuickItem* graph = findItem(shell.window->contentItem(), [](QQuickItem* i) {
        return i->objectName() == QStringLiteral("valueGraph") && i->isVisible();
    });
    REQUIRE(graph != nullptr);
    CHECK(graph->height() > 60);
    QQuickItem* handle = findItem(graph, [](QQuickItem* i) { return i->objectName() == QStringLiteral("handleOut") && i->isVisible(); });
    REQUIRE(handle != nullptr);

    // Drag that handle with the mouse in the lane: the tangent changes.
    const double before = controller->currentSequence()->findClip(city)->clip().properties()[ParamId::Scale].curve()->keys[1].out.slope;
    const QPointF from = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2));
    mouse(shell.window.get(), QEvent::MouseButtonPress, from, Qt::LeftButton);
    for (int step = 1; step <= 4; ++step)
        mouse(shell.window.get(), QEvent::MouseMove, from + QPointF(2 * step, -6 * step), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, from + QPointF(8, -24), Qt::NoButton);
    spin(100);
    const double after = controller->currentSequence()->findClip(city)->clip().properties()[ParamId::Scale].curve()->keys[1].out.slope;
    CHECK(after != before);
    CHECK(controller->undoText() == QStringLiteral("Undo Keyframe Handles"));
    {
        const QImage image = shell.grab();
        const double dpr = shell.window->effectiveDevicePixelRatio();
        QQuickItem* inspector = shell.item("effectControls");
        REQUIRE(inspector != nullptr);
        const QRectF r = inspector->mapRectToScene(QRectF(0, 0, inspector->width(), inspector->height()));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(outputDir() + QStringLiteral("/effect-controls-bezier.png")));
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Audio Gain (G, 0.1.2) ----------------------------------------------------------------------------------------

TEST_CASE("Audio Gain (G): set, adjust, normalize the max peak and all peaks; a video clip stands for its audio",
          "[app][audio][gain]") {
    Fixture fx;
    ClipAudioBridge gain;
    gain.setController(&fx.controller);
    gain.setServices(fakeServices({{fx.tl.media('A'), -30.0}, {fx.tl.media('M'), -22.0}, {fx.tl.media('B'), -40.0}}));
    gain.setSynchronous(true);
    const ClipId video = fx.controller.currentSequence()->videoTracks[0]->clips[0].id;
    const ClipId a1 = fx.audio(0).clips[0].id;
    const ClipId a2 = fx.audio(1).clips[0].id;
    const ClipId a3 = fx.audio(2).clips[0].id;
    const QStringList all{clipKey(a1), clipKey(a2), clipKey(a3)};
    const auto db = [&](ClipId id) { return gain.clipGainDb({clipKey(id)}); };
    const auto gainEffects = [&](ClipId id) {
        const Clip& c = fx.controller.currentSequence()->findClip(id)->clip();
        return std::ranges::count_if(c.effects(), [](const EffectInstance& e) { return e.effect == "wabcut.audio.gain"; });
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };

    // The video clip stands for its linked audio.
    CHECK(gain.gainClips({clipKey(video)}) == std::vector<ClipId>{a1});
    CHECK(db(a1) == 0.0);

    // Set gain to -6 dB: a Gain at the top of the stack, one undo step.
    std::size_t undo = fx.undoCount();
    CHECK(gain.audioGain({clipKey(video)}, QStringLiteral("set"), -6.0).isEmpty());
    CHECK(near(db(a1), -6.0));
    CHECK(gainEffects(a1) == 1);
    CHECK(fx.undoCount() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Audio Gain"));

    // Adjust by +2 dB: the same effect, -4 dB.
    CHECK(gain.audioGain({clipKey(a1)}, QStringLiteral("adjust"), 2.0).isEmpty());
    CHECK(near(db(a1), -4.0));
    CHECK(gainEffects(a1) == 1);

    // Normalize max peak to -1 dBFS: the loudest peak (A2's, -14.5 dBFS) reaches it; every clip gets the same +13.5 dB.
    undo = fx.undoCount();
    CHECK(gain.audioGain(all, QStringLiteral("normalizeMax"), -1.0).isEmpty());
    CHECK(near(db(a1), -4.0 + 13.5));
    CHECK(near(db(a2), 13.5));
    CHECK(near(db(a3), 13.5));
    CHECK(fx.undoCount() == undo + 1); // measuring changed nothing; the gains are one step

    // Normalize all peaks to -3 dBFS: each clip's own peak (-13, -1, -19 dBFS now); the Gain effect tops out at +24 dB.
    CHECK(gain.audioGain(all, QStringLiteral("normalizeAll"), -3.0).isEmpty());
    CHECK(near(db(a1), 9.5 + 10.0));
    CHECK(near(db(a2), 13.5 - 2.0));
    CHECK(near(db(a3), 24.0));

    // Undo brings the previous gains back.
    REQUIRE(fx.controller.undo());
    CHECK(near(db(a2), 13.5));

    // Nothing with sound, or a mode it does not know: refused.
    CHECK_FALSE(gain.audioGain({}, QStringLiteral("set"), 0.0).isEmpty());
    CHECK_FALSE(gain.audioGain(all, QStringLiteral("louder"), 0.0).isEmpty());
}

TEST_CASE("G opens Audio Gain on the selected clip and applies it; Shift+G opens Audio Channels", "[app][audio][gain][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* clipAudio = shell.engine.singletonInstance<ClipAudioBridge*>("Wabcut", "ClipAudio");
    REQUIRE(controller);
    REQUIRE(clipAudio);
    REQUIRE_FALSE(shell.grab().isNull());
    const ClipId clip = controller->currentSequence()->audioTracks[0]->clips[0].id;
    controller->selectClip(clipKey(clip));

    // G (clip.audioGain through Main): the dialog for the selection; Set gain to -6 dB, one undo step.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("clip.audioGain"))));
    QObject* loader = shell.object("audioGainDialogLoader");
    REQUIRE(loader);
    auto* dialog = loader->property("item").value<QObject*>();
    REQUIRE(dialog);
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }, 3000));
    CHECK(dialog->property("modeIndex").toInt() == 0);
    const auto undo = controller->session().undoCount();
    dialog->setProperty("level", -6.0);
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    CHECK(std::abs(clipAudio->clipGainDb({clipKey(clip)}) + 6.0) < 1e-6);
    CHECK(controller->session().undoCount() == undo + 1);
    CHECK_FALSE(dialog->property("opened").toBool());

    // Shift+G (clip.audioChannels): the Audio Channels dialog for the selection.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("clip.audioChannels"))));
    QObject* channels = shell.object("audioChannelsDialogLoader");
    REQUIRE(channels);
    REQUIRE(waitFor([&] {
        auto* item = channels->property("item").value<QObject*>();
        return item && item->property("visible").toBool();
    }, 3000));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
