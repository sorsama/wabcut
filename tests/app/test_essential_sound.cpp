// M5m: the Essential Sound panel and Remix (A.7) on fake analysis, offscreen; no window is shown.
//   * tagging the selection runs audio.essentialSound: the clip's stack starts with the record and the marked effects
//     follow; presets per type; a video clip stands for its linked audio;
//   * a slider applies once (one undo step) with the full option set, a section's switch bypasses its effects, a
//     marked effect disabled elsewhere shows its section off, Clear keeps the clip's own effects;
//   * the Remix preview (a dry run) draws its joins on the timeline; Apply cuts the music into its pieces in one undo
//     step; Cancel, an edit and a refusal drop it; Fit to video; a rendered take.
// Screenshots: essential-sound.png (the panel on a dialogue clip), remix-preview.png (the joins and the Remix popover).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "../audio_edit/MusicTestSignals.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "EssentialSoundBridge.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>
#include <QQmlComponent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::audio::edit;
using Catch::Approx;
using command::Json;

namespace {

/// A clip's loudness: -35 LUFS plus its volume and the gain effects of its stack.
class FakeLoudness final : public LoudnessAnalyzer {
public:
    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange>, command::ExecutionControl*) override {
        LoudnessStats stats;
        if (target.scope == LoudnessTarget::Scope::Clip) {
            const Clip& c = state.findSequence(sequence)->findClip(target.clip)->clip();
            double l = -35.0 + c.properties().value(ParamId::VolumeDb);
            for (const EffectInstance& e : c.effects()) {
                if (e.enabled && e.effect == "wabcut.audio.gain")
                    l += effectParameterAt(e, "gain", Ticks{});
            }
            stats.integratedLufs = l;
            stats.truePeakDbtp = l + 10.0;
        }
        stats.duration = Ticks::fromSeconds(10);
        return stats;
    }
};

/// People speak at 4-7 s and 20-25 s.
class FakeSpeech final : public SpeechDetector {
public:
    std::expected<std::vector<TimeRange>, Error> speech(const ProjectState&, SequenceId, std::span<const TrackId>, SpeechSource,
                                                        std::optional<TimeRange>, command::ExecutionControl*) override {
        return std::vector<TimeRange>{TimeRange{Ticks::fromSeconds(4), Ticks::fromSeconds(3)},
                                      TimeRange{Ticks::fromSeconds(20), Ticks::fromSeconds(5)}};
    }
};

/// The synthetic song's analysis (MusicTestSignals.h: 80 s, 4/4 at 120 BPM, intro, verse, chorus, verse, chorus, outro)
/// for any media; takes are written as small files.
class FakeMusic final : public MusicAnalyzer {
public:
    explicit FakeMusic(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    std::expected<MusicAnalysis, Error> analyze(const MediaItem&, int, std::stop_token, const std::function<void(double)>& progress) override {
        ++analyses;
        if (progress)
            progress(1.0);
        return defaultSongAnalysis();
    }
    std::expected<Take, Error> render(const MediaItem& item, int, const RemixPlan& plan, std::stop_token,
                                      const std::function<void(double)>&) override {
        ++renders;
        Take t;
        const auto file = m_dir / (item.name + "-remix.wav");
        std::ofstream(file) << "remix";
        t.path = pathToUtf8(file);
        t.info.path = t.path;
        t.info.duration = plan.duration;
        AudioStreamInfo a;
        a.index = 0;
        a.codec = "pcm_f32le";
        a.sampleRate = Rate{48000, 1};
        a.channels = 2;
        a.duration = plan.duration;
        t.info.audio.push_back(a);
        t.fingerprint.size = 99;
        t.fingerprint.sampleHash = "0123456789abcdef";
        t.elapsed = 0.5;
        return t;
    }
    int analyses = 0;
    int renders = 0;

private:
    std::filesystem::path m_dir;
};

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "app-essential" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// V1: 60 s of picture linked to A1's 20 s of dialogue (media D); A2: the 80 s song (media M, 100 s long). 25 fps.
Timeline fixtureTimeline() {
    const std::string picture = "V1 tgt |V" + std::string(1499, 'v') + "|\n" + "A1 tgt |D" + std::string(499, 'd') + "|\n" +
                                "A2     |M" + std::string(1999, 'm') + "|\n";
    Timeline tl(picture, {{'V', 3000}, {'D', 3000}, {'M', 2500}});
    tl.link("V1@0 A1@0");
    return tl;
}

std::shared_ptr<AudioServices> fakeServices(const std::shared_ptr<FakeMusic>& music) {
    auto services = std::make_shared<AudioServices>(std::make_shared<FakeLoudness>(), std::make_shared<FakeSpeech>());
    services->music = music;
    return services;
}

struct EsRig {
    std::filesystem::path dir;
    std::shared_ptr<FakeMusic> music;
    Timeline tl = fixtureTimeline();
    ProjectController controller;
    EssentialSoundBridge es;

    explicit EsRig(const std::string& name) : dir(scratch(name)), music(std::make_shared<FakeMusic>(dir)) {
        controller.resetProject(tl.state, tl.seq);
        es.setController(&controller);
        es.setServices(fakeServices(music));
        es.setSynchronous(true);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    const Clip& clip(ClipId id) const { return controller.currentSequence()->findClip(id)->clip(); }
    const Track& track(std::string_view label) const { return *controller.currentSequence()->findTrack(tl.track(label)); }
    /// "effect" or "effect:section" per effect of the clip's stack.
    std::vector<std::string> stackOf(ClipId id) const {
        std::vector<std::string> out;
        for (const EffectInstance& e : clip(id).effects()) {
            const EffectData* section = e.findData(kSectionKey);
            out.push_back(e.effect + (section != nullptr ? ":" + *section->text() : ""));
        }
        return out;
    }
    const EffectInstance* marked(ClipId id, std::string_view section) const {
        for (const EffectInstance& e : clip(id).effects()) {
            const EffectData* d = e.findData(kSectionKey);
            if (d && *d->text() == section)
                return &e;
        }
        return nullptr;
    }
};

QVariant optionAt(const QVariantMap& options, const QString& path) {
    QVariant v = options;
    for (const QString& part : path.split(QLatin1Char('.')))
        v = v.toMap().value(part);
    return v;
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

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

/// An object of the Wabcut module made in the shell's engine (`qml`: its QML source).
std::unique_ptr<QObject> makeQml(Shell& shell, const QByteArray& qml) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/m5m-tests/EssentialHost.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    return object;
}

} // namespace

TEST_CASE("Essential Sound: tagging the selection builds the record and the marked effects, one undo step each",
          "[app][audio][essential]") {
    EsRig r("tag");
    const ClipId dialogue = r.tl.clip("A1@0");
    CHECK(r.es.clips().isEmpty());
    CHECK_FALSE(r.es.setType(QStringLiteral("dialogue")).isEmpty()); // nothing selected
    // A video clip stands for its linked audio.
    r.controller.setSelection({r.tl.clip("V1@0")});
    REQUIRE(r.es.clips() == QStringList{clipKey(dialogue)});
    CHECK(r.es.type().isEmpty());
    CHECK(r.es.presets().isEmpty());

    std::size_t before = r.undo();
    REQUIRE(r.es.setType(QStringLiteral("dialogue")).isEmpty());
    INFO(r.es.message().toStdString());
    CHECK(r.undo() == before + 1);
    CHECK(r.es.type() == QStringLiteral("dialogue"));
    CHECK(r.es.preset() == QStringLiteral("default"));
    CHECK(r.stackOf(dialogue).front() == "wabcut.audio.essentialSound");
    REQUIRE(r.marked(dialogue, "loudness"));
    // Loudness matched: -35 LUFS to dialogue's -23 by the Gain.
    CHECK(effectParameterAt(*r.marked(dialogue, "loudness"), "gain", Ticks{}) == Approx(12.0).margin(0.05));
    CHECK(r.es.sections().value(QStringLiteral("loudness")).toString() == QStringLiteral("on"));
    CHECK(r.track("A1").role == TrackRole::Dialogue);
    CHECK_FALSE(r.es.busy());

    const QVariantList presets = r.es.presets();
    REQUIRE(presets.size() >= 5);
    CHECK(presets.front().toMap().value(QStringLiteral("id")).toString() == QStringLiteral("default"));
    CHECK(std::ranges::any_of(presets, [](const QVariant& p) { return p.toMap().value(QStringLiteral("id")) == QStringLiteral("podcastVoice"); }));

    before = r.undo();
    REQUIRE(r.es.applyPreset(QStringLiteral("podcastVoice")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.stackOf(dialogue) == std::vector<std::string>{"wabcut.audio.essentialSound", "wabcut.audio.gain:loudness",
                                                          "wabcut.audio.filter:repair.rumble", "wabcut.audio.denoise:repair.noise",
                                                          "wabcut.audio.deess:repair.deess", "wabcut.audio.filter:clarity.band",
                                                          "wabcut.audio.eq3:clarity.eq", "wabcut.audio.compressor:clarity.dynamics"});
    CHECK(r.es.preset() == QStringLiteral("podcastVoice"));
    const QVariantMap options = r.es.options();
    CHECK(optionAt(options, QStringLiteral("repair.rumble")).toDouble() == 80.0);
    CHECK(optionAt(options, QStringLiteral("clarity.eq")).toString() == QStringLiteral("podcastVoice"));
    CHECK(optionAt(options, QStringLiteral("repair.noise")).toDouble() == Approx(0.3));
    CHECK(optionAt(options, QStringLiteral("loudness")).toBool());
    const QVariantMap sections = r.es.sections();
    CHECK(sections.value(QStringLiteral("repair")).toString() == QStringLiteral("on"));
    CHECK(sections.value(QStringLiteral("clarity")).toString() == QStringLiteral("on"));
    CHECK(sections.value(QStringLiteral("creative")).toString().isEmpty());
    // Undo takes the preset back to the default stack.
    REQUIRE(r.controller.undo());
    CHECK(r.es.preset() == QStringLiteral("default"));
    // An unknown preset: the command refuses it, the message says why, nothing changes.
    before = r.undo();
    CHECK(r.es.applyPreset(QStringLiteral("noSuchPreset")).isEmpty()); // started
    CHECK_FALSE(r.es.message().isEmpty());
    CHECK(r.undo() == before);
    CHECK(r.es.preset() == QStringLiteral("default"));
}

TEST_CASE("Essential Sound: a slider applies once, a section switch bypasses its effects, Clear keeps the clip's own",
          "[app][audio][essential]") {
    EsRig r("options");
    const ClipId dialogue = r.tl.clip("A1@0");
    r.controller.setSelection({dialogue});
    REQUIRE(r.es.setType(QStringLiteral("dialogue")).isEmpty());
    REQUIRE(r.es.applyPreset(QStringLiteral("podcastVoice")).isEmpty());

    // A drag: previews move only the panel; its end applies once, with every setting (the rumble filter stays).
    std::size_t before = r.undo();
    r.es.previewOption(QStringLiteral("repair.noise"), 0.45);
    r.es.previewOption(QStringLiteral("repair.noise"), 0.6);
    CHECK(r.undo() == before);
    CHECK(optionAt(r.es.options(), QStringLiteral("repair.noise")).toDouble() == Approx(0.6));
    REQUIRE(r.es.commitOptions().isEmpty());
    INFO("message: " << r.es.message().toStdString());
    CHECK(r.undo() == before + 1);
    REQUIRE(r.marked(dialogue, "repair.noise"));
    CHECK(effectParameterAt(*r.marked(dialogue, "repair.noise"), "amount", Ticks{}) == Approx(0.6));
    REQUIRE(r.marked(dialogue, "repair.rumble"));
    CHECK(effectParameterAt(*r.marked(dialogue, "repair.rumble"), "lowCut", Ticks{}) == Approx(80.0));
    CHECK(r.es.preset() == QStringLiteral("podcastVoice"));

    // A setting turned off stays off although the preset turns it on.
    REQUIRE(r.es.setOption(QStringLiteral("repair.rumble"), 0).isEmpty());
    CHECK_FALSE(r.marked(dialogue, "repair.rumble"));
    CHECK(optionAt(r.es.options(), QStringLiteral("repair.rumble")).toDouble() == 0.0);
    // A choice applies at once.
    REQUIRE(r.es.setOption(QStringLiteral("clarity.eq"), QStringLiteral("phone")).isEmpty());
    CHECK(optionAt(r.es.options(), QStringLiteral("clarity.eq")).toString() == QStringLiteral("phone"));
    REQUIRE(r.es.setOption(QStringLiteral("creative.reverb"), QStringLiteral("smallRoom")).isEmpty());
    CHECK(r.marked(dialogue, "creative.reverb"));
    CHECK(r.es.sections().value(QStringLiteral("creative")).toString() == QStringLiteral("on"));

    // The section switch: Repair's effects bypassed, then on again; one undo step each.
    before = r.undo();
    REQUIRE(r.es.setSectionEnabled(QStringLiteral("repair"), false).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.es.sections().value(QStringLiteral("repair")).toString() == QStringLiteral("off"));
    for (const EffectInstance& e : r.clip(dialogue).effects()) {
        const EffectData* d = e.findData(kSectionKey);
        if (d && d->text()->starts_with("repair."))
            CHECK_FALSE(e.enabled);
    }
    CHECK_FALSE(r.es.setSectionEnabled(QStringLiteral("repair"), false).isEmpty()); // nothing left to turn off
    REQUIRE(r.es.setSectionEnabled(QStringLiteral("repair"), true).isEmpty());
    CHECK(r.es.sections().value(QStringLiteral("repair")).toString() == QStringLiteral("on"));

    // A marked effect turned off in Effect Controls shows its section off.
    const EffectInstanceId compressor = r.marked(dialogue, "clarity.dynamics")->id;
    REQUIRE(r.controller.applyEditFn("Bypass", [&](const ProjectState& s) {
        edit::EffectChange change;
        change.enabled = false;
        return edit::setEffect(s, r.tl.seq, dialogue, compressor, change);
    }));
    CHECK(r.es.sections().value(QStringLiteral("clarity.dynamics")).toString() == QStringLiteral("off"));
    CHECK(r.es.sections().value(QStringLiteral("clarity")).toString() == QStringLiteral("on")); // the band and EQ play

    // Clear: Essential Sound's effects go, the clip's own stay.
    const auto added = CommandJob::runNow(&r.controller, nullptr, "clip.addEffect",
                                          Json{{"clips", Json::array({dialogue.value})}, {"effect", "wabcut.audio.eq3"}});
    REQUIRE(added.ok());
    before = r.undo();
    REQUIRE(r.es.clear().isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.stackOf(dialogue) == std::vector<std::string>{"wabcut.audio.eq3"});
    CHECK(r.es.type().isEmpty());
    CHECK(r.es.sections().isEmpty());
}

TEST_CASE("Essential Sound: music ducks under the dialogue and widens, sound effects pan, a mixed selection",
          "[app][audio][essential]") {
    EsRig r("music");
    const ClipId dialogue = r.tl.clip("A1@0");
    const ClipId music = r.tl.clip("A2@0");
    r.controller.setSelection({dialogue});
    REQUIRE(r.es.setType(QStringLiteral("dialogue")).isEmpty());

    r.controller.setSelection({music});
    REQUIRE(r.es.setType(QStringLiteral("music")).isEmpty());
    CHECK(r.es.type() == QStringLiteral("music"));
    QStringList ids;
    for (const QVariant& p : r.es.presets())
        ids << p.toMap().value(QStringLiteral("id")).toString();
    CHECK(ids == QStringList{QStringLiteral("default"), QStringLiteral("background"), QStringLiteral("wide")});
    REQUIRE(r.es.applyPreset(QStringLiteral("wide")).isEmpty());
    CHECK(r.es.sections().value(QStringLiteral("width")).toString() == QStringLiteral("on"));
    CHECK(optionAt(r.es.options(), QStringLiteral("width")).toDouble() > 1.0);
    // Ducking: volume keyframes under the dialogue's speech.
    REQUIRE(r.es.setOption(QStringLiteral("ducking.enabled"), true).isEmpty());
    CHECK(optionAt(r.es.options(), QStringLiteral("ducking.enabled")).toBool());
    CHECK(r.clip(music).properties()[ParamId::VolumeDb].isCurve());
    CHECK(r.es.sections().value(QStringLiteral("width")).toString() == QStringLiteral("on")); // kept
    // Clips of two types: "mixed".
    r.controller.setSelection({dialogue, music});
    CHECK(r.es.type() == QStringLiteral("mixed"));
    CHECK(r.es.preset().isEmpty());
    CHECK(r.es.clips().size() == 2);
    // Sound effects: the position is the clip's panner.
    r.controller.setSelection({dialogue});
    REQUIRE(r.es.setType(QStringLiteral("sfx")).isEmpty());
    REQUIRE(r.es.setOption(QStringLiteral("pan"), -0.5).isEmpty());
    CHECK(r.clip(dialogue).properties().value(ParamId::Pan) == Approx(-0.5));
    CHECK(optionAt(r.es.options(), QStringLiteral("pan")).toDouble() == Approx(-0.5));
}

TEST_CASE("Remix: the preview draws its joins on the timeline; Apply cuts the music into its pieces in one undo step",
          "[app][audio][remix]") {
    EsRig r("remix");
    RenderHarness harness(QSize(900, 260));
    REQUIRE(harness.initialize());
    TimelineView* view = addView(harness, r.controller, QSize(900, 260));
    view->setParent(harness.window().contentItem()); // found like the app's views (QObject children of the window)
    harness.frame();
    const ClipId music = r.tl.clip("A2@0");
    const QString key = clipKey(music);
    const Ticks start = r.clip(music).start();
    const double beat = defaultSong().beatSeconds();

    // The preview: a dry run, nothing changes; the joins and the new length on the timeline.
    std::size_t before = r.undo();
    REQUIRE(r.es.preview(key, 48).isEmpty());
    QVariantMap remix = r.es.remix();
    INFO(remix.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(remix.value(QStringLiteral("error")).toString().isEmpty());
    CHECK_FALSE(remix.value(QStringLiteral("busy")).toBool());
    CHECK(remix.value(QStringLiteral("clip")).toString() == key);
    CHECK(remix.value(QStringLiteral("targetSeconds")).toDouble() == Approx(48.0));
    const double result = remix.value(QStringLiteral("resultSeconds")).toDouble();
    CHECK(std::fabs(result - 48.0) <= beat);
    const QVariantList joins = remix.value(QStringLiteral("joins")).toList();
    REQUIRE_FALSE(joins.isEmpty());
    for (const QVariant& j : joins) {
        const QVariantMap join = j.toMap();
        CHECK(join.value(QStringLiteral("confidence")).toDouble() > 0.8); // the song repeats: seamless joins
        CHECK(join.value(QStringLiteral("at")).toLongLong() > start.value);
        CHECK(join.value(QStringLiteral("seconds")).toDouble() > 0.0);
        CHECK_FALSE(join.value(QStringLiteral("timecode")).toString().isEmpty());
    }
    CHECK(r.undo() == before);
    CHECK(r.track("A2").clips.size() == 1);
    CHECK(r.music->analyses == 1);
    REQUIRE(view->externalPreview());
    CHECK(view->externalPreview()->markers.size() == static_cast<std::size_t>(joins.size()));
    REQUIRE(view->externalPreview()->added.size() == 1);
    CHECK(view->externalPreview()->added.front().start == start);
    CHECK(std::fabs((view->externalPreview()->added.front().end - start).toSeconds() - result) < 0.01);
    REQUIRE(view->externalPreview()->removed.size() == 1); // 80 s down to 48 s: the tail goes
    CHECK(view->externalPreview()->removed.front().end == r.clip(music).end());
    harness.frame();

    // Apply: the pieces on the track, back to back, one undo step; the preview is gone.
    REQUIRE(r.es.apply().isEmpty());
    CHECK(r.undo() == before + 1);
    const std::size_t pieces = r.track("A2").clips.size();
    CHECK(pieces >= 2);
    CHECK(r.track("A2").clips.front().id == music);
    CHECK(std::fabs((r.track("A2").clips.back().end() - start).toSeconds() - result) < 0.01);
    CHECK(view->externalPreview() == nullptr);
    CHECK(r.es.remix().isEmpty());
    CHECK(r.es.message().contains(QStringLiteral("Remixed")));
    CHECK_FALSE(r.es.apply().isEmpty()); // nothing previewed now
    REQUIRE(r.controller.undo());
    CHECK(r.track("A2").clips.size() == 1);

    // Cancel drops the preview.
    REQUIRE(r.es.preview(key, 60).isEmpty());
    REQUIRE(view->externalPreview());
    r.es.cancel();
    CHECK(view->externalPreview() == nullptr);
    CHECK(r.es.remix().isEmpty());
    // An edit makes it stale: it goes too.
    REQUIRE(r.es.preview(key, 50).isEmpty());
    REQUIRE(view->externalPreview());
    REQUIRE(CommandJob::runNow(&r.controller, nullptr, "clip.addEffect",
                               Json{{"clips", Json::array({r.tl.clip("A1@0").value})}, {"effect", "wabcut.audio.eq3"}})
                .ok());
    CHECK(view->externalPreview() == nullptr);
    CHECK(r.es.remix().isEmpty());

    // Fit to video: V1 ends at 60 s.
    REQUIRE(r.es.previewFit(key).isEmpty());
    CHECK(std::fabs(r.es.remix().value(QStringLiteral("resultSeconds")).toDouble() - 60.0) <= beat);
    // A length the intro and the outro cannot fit is refused with the reason; nothing to apply.
    REQUIRE(r.es.preview(key, 2.0).isEmpty());
    CHECK_FALSE(r.es.remix().value(QStringLiteral("error")).toString().isEmpty());
    CHECK(view->externalPreview() == nullptr);
    CHECK_FALSE(r.es.apply().isEmpty());
    CHECK_FALSE(r.es.preview(QStringLiteral("junk"), 30).isEmpty());

    // A rendered take instead of cuts: one new media item the clip plays.
    REQUIRE(r.es.preview(key, 100).isEmpty());
    REQUIRE(r.es.remix().value(QStringLiteral("error")).toString().isEmpty());
    const std::size_t mediaBefore = r.controller.snapshot()->media.size();
    before = r.undo();
    REQUIRE(r.es.renderTake().isEmpty());
    // The job's outcome (a refusal or an error) is the bridge's message: shown when the take did not land.
    INFO("Essential Sound: " << r.es.message().toStdString());
    CHECK(r.undo() == before + 1);
    CHECK(r.music->renders == 1);
    CHECK(r.controller.snapshot()->media.size() == mediaBefore + 1);
    const MediaItem* take = r.controller.snapshot()->findMedia(r.clip(music).media);
    REQUIRE(take);
    REQUIRE(take->derived);
    CHECK(take->derived->operation == "remix");
    CHECK(r.track("A2").clips.size() == 1);
}

TEST_CASE("Remix tool: dragging a music clip's end proposes a length; nothing changes until a remix is applied",
          "[app][audio][remix][timeline]") {
    // 50 frames of music (2 s at 25 fps) and a video clip, 10 px a frame.
    Rig rig(R"(
        V1 |Vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv|
        A1 |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )", 10.0, false);
    TimelineView* v = rig.view;
    v->setTool(QStringLiteral("remix"));
    REQUIRE(v->tool() == QStringLiteral("remix"));
    QString asked;
    double seconds = 0;
    QObject::connect(v, &TimelineView::remixRequested, [&](const QString& clip, double s, double, double) {
        asked = clip;
        seconds = s;
    });
    const auto before = rig.controller.session().undoCount();
    const Ticks length = rig.controller.currentSequence()->audioTracks[0]->clips[0].duration();
    const double y = rig.y("A1");
    rig.press(QPointF(rig.x(49.5), y));
    rig.move(QPointF(rig.x(60), y));
    rig.move(QPointF(rig.x(75), y));
    rig.release(QPointF(rig.x(75), y));
    CHECK(asked == clipKey(rig.clip("A1@0")));
    CHECK(seconds == Catch::Approx(3.0)); // to frame 75
    CHECK(rig.controller.session().undoCount() == before); // a proposal only
    CHECK(rig.controller.currentSequence()->audioTracks[0]->clips[0].duration() == length);

    // Esc cancels the drag: nothing is asked.
    asked.clear();
    rig.press(QPointF(rig.x(49.5), y));
    rig.move(QPointF(rig.x(70), y));
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&rig.harness.window(), &esc);
    rig.release(QPointF(rig.x(70), y));
    CHECK(asked.isEmpty());
    // At least a second of music.
    rig.press(QPointF(rig.x(49.5), y));
    rig.move(QPointF(rig.x(5), y));
    rig.release(QPointF(rig.x(5), y));
    CHECK(seconds == Catch::Approx(1.0));
    // A video clip is not music: no proposal.
    asked.clear();
    const double vy = rig.y("V1");
    rig.press(QPointF(rig.x(49.5), vy));
    rig.move(QPointF(rig.x(70), vy));
    rig.release(QPointF(rig.x(70), vy));
    CHECK(asked.isEmpty());
}

TEST_CASE("Essential Sound in the shell: the panel on a dialogue clip; screenshot", "[app][audio][essential][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* es = shell.engine.singletonInstance<EssentialSoundBridge*>("Wabcut", "EssentialSound");
    REQUIRE(controller);
    REQUIRE(es);
    const auto dir = scratch("shell");
    es->setServices(fakeServices(std::make_shared<FakeMusic>(dir)));
    es->setSynchronous(true);
    Timeline tl = fixtureTimeline();
    controller->resetProject(tl.state, tl.seq);
    controller->setSelection({tl.clip("A1@0")});
    REQUIRE(es->setType(QStringLiteral("dialogue")).isEmpty());
    REQUIRE(es->applyPreset(QStringLiteral("podcastVoice")).isEmpty());
    REQUIRE(es->setOption(QStringLiteral("creative.reverb"), QStringLiteral("smallRoom")).isEmpty());

    // The browser panel "essential" (Window > Essential Sound).
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("essential")))));
    spin(400);
    QQuickWindow* w = shell.window.get();
    QQuickItem* panel = findNamed(w->contentItem(), QStringLiteral("essentialSoundPanel"));
    REQUIRE(panel);
    CHECK(panel->isVisible());
    QQuickItem* dialogueButton = findNamed(panel, QStringLiteral("essentialType_dialogue"));
    REQUIRE(dialogueButton);
    CHECK(dialogueButton->property("variant").toString() == QStringLiteral("primary"));
    QQuickItem* repair = findNamed(panel, QStringLiteral("essentialRepair"));
    REQUIRE(repair);
    CHECK(repair->isVisible());
    CHECK(repair->property("sectionState").toString() == QStringLiteral("on"));
    QQuickItem* duration = findNamed(panel, QStringLiteral("essentialDuration"));
    REQUIRE(duration);
    CHECK_FALSE(duration->isVisible()); // music only
    // The panel's switch bypasses Repair (as the bridge does).
    REQUIRE(QMetaObject::invokeMethod(repair, "switched", Q_ARG(bool, false)));
    CHECK(es->sections().value(QStringLiteral("repair")).toString() == QStringLiteral("off"));
    REQUIRE(QMetaObject::invokeMethod(repair, "switched", Q_ARG(bool, true)));
    CHECK(es->sections().value(QStringLiteral("repair")).toString() == QStringLiteral("on"));
    spin(150);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, panel, QStringLiteral("essential-sound.png"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Remix in the shell: the joins on the timeline and the Remix popover, then Apply; screenshot",
          "[app][audio][remix][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* es = shell.engine.singletonInstance<EssentialSoundBridge*>("Wabcut", "EssentialSound");
    REQUIRE(controller);
    REQUIRE(es);
    const auto dir = scratch("shell-remix");
    es->setServices(fakeServices(std::make_shared<FakeMusic>(dir)));
    es->setSynchronous(true);
    Timeline tl = fixtureTimeline();
    controller->resetProject(tl.state, tl.seq);
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(view);
    view->setPixelsPerSecond(14);
    view->setScrollX(0);
    const ClipId music = tl.clip("A2@0");
    controller->setSelection({music});

    REQUIRE(es->preview(clipKey(music), 48).isEmpty());
    REQUIRE(es->remix().value(QStringLiteral("error")).toString().isEmpty());
    REQUIRE(view->externalPreview());
    const std::size_t joins = es->remix().value(QStringLiteral("joins")).toList().size();
    CHECK(view->externalPreview()->markers.size() == joins);

    // The popover, as the timeline's Remix tool opens it at the dragged end.
    QQuickItem* lanes = findNamed(w->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    std::unique_ptr<QObject> popover = makeQml(shell, "import QtQuick\nimport Wabcut\nRemixPopover {}\n");
    REQUIRE(QMetaObject::invokeMethod(popover.get(), "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                      Q_ARG(QVariant, QPointF(48 * 14 + 40, 8))));
    REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }, 3000));
    auto* content = popover->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* apply = findNamed(content->parentItem(), QStringLiteral("remixApply"));
    REQUIRE(apply);
    CHECK(apply->isEnabled());
    CHECK(findNamed(content->parentItem(), QStringLiteral("remixJoin_0")));
    spin(150);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(timeline);
    saveCrop(image, timeline, QStringLiteral("remix-preview.png"));

    // Apply from the popover: the music in its pieces, the popover closed, the preview gone.
    const auto before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(apply, "clicked"));
    REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }, 3000));
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(controller->currentSequence()->findTrack(tl.track("A2"))->clips.size() >= 2);
    CHECK(view->externalPreview() == nullptr);
    popover.reset();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

namespace {

/// FakeLoudness that records the threads it measures on and holds its first measurement until released.
class GatedLoudness final : public LoudnessAnalyzer {
public:
    std::expected<LoudnessStats, Error> measure(const ProjectState& state, SequenceId sequence, const LoudnessTarget& target,
                                                std::optional<TimeRange> range, command::ExecutionControl* control) override {
        {
            std::lock_guard lock(mutex);
            threads.push_back(std::this_thread::get_id());
        }
        entered = true;
        while (!released.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return inner.measure(state, sequence, target, range, control);
    }
    FakeLoudness inner;
    std::mutex mutex;
    std::vector<std::thread::id> threads;
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
};

} // namespace

// Review wave 5, A5-P2-24: when the user edited while a job ran, CommandJob ran the whole command again on the GUI
// thread (cheap only while its results were cached: loudness after a trim, Enhance or stems after a relink re-render or
// re-infer, with no progress and no cancel). It runs again on a worker now.
TEST_CASE("A command job whose project changed meanwhile runs again off the GUI thread", "[app][audio][jobs]") {
    Timeline tl = fixtureTimeline();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    auto loudness = std::make_shared<GatedLoudness>();
    auto services = std::make_shared<AudioServices>(loudness, std::make_shared<FakeSpeech>());
    const ClipId dialogue = tl.clip("A1@0");
    QObject context;
    CommandJob job;
    bool finished = false;
    command::CommandResult outcome;
    REQUIRE(job.start(&context, &controller, services,
                      CommandJob::Request{"audio.normalize", Json{{"clips", Json::array({dialogue.value})}, {"lufs", -16.0}},
                                          "Normalize Loudness"},
                      {}, [&](const command::CommandResult& r) {
                          finished = true;
                          outcome = r;
                      }));
    REQUIRE(waitFor([&] { return loudness->entered.load(); }, 5000));
    // The user renames the sequence while it measures.
    ProjectState next = *controller.snapshot();
    for (SequencePtr& s : next.sequences) {
        if (s->id == tl.seq) {
            auto copy = std::make_shared<Sequence>(*s);
            copy->name += " (edited)";
            s = copy;
        }
    }
    REQUIRE(controller.session().commit("Rename Sequence", next));
    loudness->released = true;
    REQUIRE(waitFor([&] { return finished; }, 20000));
    CHECK(outcome.ok());
    {
        std::lock_guard lock(loudness->mutex);
        REQUIRE_FALSE(loudness->threads.empty());
        for (const std::thread::id id : loudness->threads) {
            CHECK(id != std::this_thread::get_id()); // never measured on the GUI thread
        }
    }
    const Clip& clip = controller.currentSequence()->findClip(dialogue)->clip();
    CHECK(std::ranges::any_of(clip.effects(), [](const EffectInstance& e) { return e.effect == "wabcut.audio.gain"; }));
    CHECK(controller.currentSequence()->name.ends_with(" (edited)")); // the user's edit stays
}
