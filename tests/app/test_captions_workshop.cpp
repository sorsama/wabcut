// M5m: the caption workshop (C.7) in the app, offscreen; no window is shown.
//   * the style editor: every field to the model's CaptionStyle (a slider drag is one undo step), the map QML reads,
//     and the compositor's style the program monitor draws from it (live preview);
//   * presets: the five built in and the project's (apply, save the track's look, delete; built-in names refused);
//   * cues: text (typing merges), timing, frame nudges kept between neighbours, split at the playhead, join, add at the
//     playhead, delete, speakers (new, assign, rename / recolour), reading speed;
//   * corrections: a fix on the Transcript tab suggests "Update from transcript", which rewrites the cues' text;
//   * the Captions workspace in the shell: the Caption Style panel beside the monitor and the cue list (screenshot).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "CaptionStyleMapping.h"
#include "CaptionsBridge.h"
#include "CommandBridge.h"
#include "CommandJob.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/model/CaptionOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>
#include <QQuickItem>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;

namespace {

struct W {
    std::string text;
    double from = 0; ///< source frames (25 fps)
    double to = 0;
    std::optional<int> speaker = std::nullopt;
};

std::shared_ptr<const ai::Transcript> transcript(std::string language, std::vector<W> words) {
    auto t = std::make_shared<ai::Transcript>();
    t->language = std::move(language);
    const double frame = static_cast<double>(framesToTicks(1, kRate).value);
    for (const W& w : words) {
        ai::TranscriptWord tw;
        tw.text = w.text;
        tw.t0 = Ticks{static_cast<std::int64_t>(w.from * frame)};
        tw.t1 = Ticks{static_cast<std::int64_t>(w.to * frame)};
        tw.confidence = 0.95f;
        tw.speakerId = w.speaker;
        t->words.push_back(std::move(tw));
    }
    return t;
}

/// Two linked A/V clips of media A (100 frames) with an English transcript.
struct Fixture {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    ProjectController controller;
    TranscriptBridge transcriptBridge;
    CaptionsBridge captions;

    Fixture() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        transcriptBridge.setUseAnalysisCache(false);
        transcriptBridge.setController(&controller);
        transcriptBridge.addTranscript(tl.media('A'), 0,
                                       transcript("en", {{"Hello", 0, 6, 0},
                                                         {"wrold", 7, 14, 0},
                                                         {"this", 30, 34, 1},
                                                         {"is", 35, 38, 1},
                                                         {"great", 39, 46, 1}}));
        captions.setController(&controller);
        captions.setTranscript(&transcriptBridge);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    const CaptionTrack& track() const { return controller.currentSequence()->captionTracks.front(); }
    /// An empty track with three cues: [0,1) s, [1,2) s, [3,4) s.
    void threeCues() {
        REQUIRE(captions.addTrack(QStringLiteral("English"), {}).isEmpty());
        for (const auto& [from, to, text] : {std::tuple{0, 25, "One"}, std::tuple{25, 50, "Two words here"}, std::tuple{75, 100, "Three"}}) {
            const auto r = CommandJob::runNow(&controller, nullptr, "captions.addCue",
                                              Json{{"start", f(from).value}, {"end", f(to).value}, {"text", text}});
            INFO(commandErrorText(r).toStdString());
            REQUIRE(r.ok());
        }
        REQUIRE(captions.cues().size() == 3);
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

TEST_CASE("Caption style: every field reaches the model's CaptionStyle; a drag is one undo step; the monitor's style follows",
          "[app][captions][style]") {
    Fixture fx;
    // Without a track: the default look, edits refused with a reason.
    CHECK(fx.captions.style().value(QStringLiteral("fontSizePx")).toDouble() == Catch::Approx(48.0));
    CHECK_FALSE(fx.captions.setStyle(QStringLiteral("box"), false).isEmpty());
    fx.threeCues();
    const auto before = fx.undo();

    // A size drag: many moves, one step.
    for (const double px : {50.0, 60.0, 72.0})
        REQUIRE(fx.captions.setStyle(QStringLiteral("fontSizePx"), px).isEmpty());
    fx.captions.endStyleEdit();
    CHECK(fx.undo() == before + 1);
    CHECK(fx.track().style.fontSize == Catch::Approx(72.0 / 1080.0));
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Caption Style"));

    // Each kind of field.
    REQUIRE(fx.captions.setStyle(QStringLiteral("fill"), QStringLiteral("#FFD13F")).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("outlineWidth"), 0.12).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("outline"), QStringLiteral("#101013")).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("box"), false).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("placement"), QStringLiteral("top")).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("alignment"), QStringLiteral("left")).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("fontWeight"), 760).isEmpty()); // rounds to 800
    REQUIRE(fx.captions.setStyle(QStringLiteral("shadowOpacity"), 0.6).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("maxWidth"), 5.0).isEmpty()); // clamped to 1
    REQUIRE(fx.captions.setStyle(QStringLiteral("animation"), QStringLiteral("wordByWord")).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("showSpeakers"), true).isEmpty());
    fx.captions.endStyleEdit();
    const CaptionStyle& s = fx.track().style;
    CHECK(s.fill[0] == Catch::Approx(1.0));
    CHECK(s.fill[2] == Catch::Approx(0x3F / 255.0).margin(0.002));
    CHECK(s.outlineWidth == Catch::Approx(0.12));
    CHECK_FALSE(s.box);
    CHECK(s.placement == CaptionPlacement::Top);
    CHECK(s.alignment == CaptionAlign::Left);
    CHECK(s.fontWeight == 800);
    CHECK(s.shadowOpacity == Catch::Approx(0.6));
    CHECK(s.maxWidth == Catch::Approx(1.0));
    CHECK(s.animation == CaptionAnimation::WordByWord);
    CHECK(s.showSpeakers);
    // Unknown keys and bad values change nothing.
    CHECK_FALSE(fx.captions.setStyle(QStringLiteral("sparkle"), 1).isEmpty());
    CHECK_FALSE(fx.captions.setStyle(QStringLiteral("fill"), QStringLiteral("not a colour")).isEmpty());
    // Custom placement keeps a position.
    REQUIRE(fx.captions.setStyle(QStringLiteral("placement"), QStringLiteral("custom")).isEmpty());
    REQUIRE(fx.captions.setStyle(QStringLiteral("positionY"), 0.3).isEmpty());
    CHECK(fx.track().style.positionY.valueAt(Ticks{}) == Catch::Approx(0.3));

    // The map QML reads, and the compositor's style (the program monitor and burn-in draw this: live preview).
    const QVariantMap map = fx.captions.style();
    CHECK(map.value(QStringLiteral("fill")).toString() == QStringLiteral("#FFD13F"));
    CHECK(map.value(QStringLiteral("placement")).toString() == QStringLiteral("custom"));
    CHECK(map.value(QStringLiteral("fontSizePx")).toDouble() == Catch::Approx(72.0));
    REQUIRE(fx.captions.setStyle(QStringLiteral("placement"), QStringLiteral("top")).isEmpty());
    const render::CaptionStyle drawn = captionStyleFrom(fx.captions.style());
    CHECK(drawn.sizePx == Catch::Approx(72.0));
    CHECK(drawn.strokeWidth == Catch::Approx(0.12 * 72.0));
    CHECK(drawn.weight == 800);
    CHECK_FALSE(drawn.box);
    CHECK(drawn.position == render::CaptionPosition::Top);
    CHECK(drawn.maxWidth == Catch::Approx(1.0));
    // The older map still draws (no model style).
    const render::CaptionStyle legacy = captionStyleFrom(QVariantMap{{QStringLiteral("size"), 40.0}, {QStringLiteral("box"), true}});
    CHECK(legacy.sizePx == Catch::Approx(40.0));
}

TEST_CASE("Caption presets: built in and the project's; apply, save the track's look, delete", "[app][captions][presets]") {
    Fixture fx;
    fx.threeCues();
    QVariantList presets = fx.captions.presets();
    REQUIRE(presets.size() == 5);
    CHECK(presets[0].toMap().value(QStringLiteral("builtin")).toBool());
    const auto before = fx.undo();
    REQUIRE(fx.captions.applyPreset(QStringLiteral("Bold Box")).isEmpty());
    CHECK(fx.undo() == before + 1);
    CHECK(fx.track().style == findBuiltinCaptionPreset("Bold Box")->style);
    presets = fx.captions.presets();
    const auto current = std::find_if(presets.begin(), presets.end(),
                                      [](const QVariant& p) { return p.toMap().value(QStringLiteral("current")).toBool(); });
    REQUIRE(current != presets.end());
    CHECK(current->toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Bold Box"));

    // Save the track's look (after a tweak) as a project preset; it is current; apply it elsewhere; delete it.
    REQUIRE(fx.captions.setStyle(QStringLiteral("fill"), QStringLiteral("#2EE6A8")).isEmpty());
    fx.captions.endStyleEdit();
    REQUIRE(fx.captions.savePreset(QStringLiteral("Channel look")).isEmpty());
    REQUIRE(fx.controller.snapshot()->captionPresets.size() == 1);
    presets = fx.captions.presets();
    REQUIRE(presets.size() == 6);
    CHECK(presets.back().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Channel look"));
    CHECK_FALSE(presets.back().toMap().value(QStringLiteral("builtin")).toBool());
    CHECK(presets.back().toMap().value(QStringLiteral("current")).toBool());
    REQUIRE(fx.captions.applyPreset(QStringLiteral("Clean")).isEmpty());
    REQUIRE(fx.captions.applyPreset(QStringLiteral("channel look")).isEmpty()); // case does not matter
    CHECK(fx.track().style.fill[1] == Catch::Approx(0xE6 / 255.0).margin(0.002));
    CHECK_FALSE(fx.captions.savePreset(QStringLiteral("Podcast")).isEmpty()); // a built-in name
    CHECK_FALSE(fx.captions.savePreset(QStringLiteral("  ")).isEmpty());
    REQUIRE(fx.captions.removePreset(QStringLiteral("Channel look")).isEmpty());
    CHECK(fx.controller.snapshot()->captionPresets.empty());
    CHECK_FALSE(fx.captions.applyPreset(QStringLiteral("Channel look")).isEmpty());
}

TEST_CASE("Caption cues: text, timing, nudges, split, join, add, delete, speakers, reading speed",
          "[app][captions][cues]") {
    Fixture fx;
    fx.threeCues();
    CaptionsBridge& c = fx.captions;

    // Typing merges into one step; a field left ends it.
    auto before = fx.undo();
    REQUIRE(c.setCueText(1, QStringLiteral("Two")).isEmpty());
    REQUIRE(c.setCueText(1, QStringLiteral("Two words")).isEmpty());
    c.endEdit();
    CHECK(fx.undo() == before + 1);
    CHECK(fx.track().cues[1].text == "Two words");

    // Nudges: a frame at a time, never into a neighbour.
    REQUIRE(c.nudgeCue(1, QStringLiteral("end"), 5).isEmpty());
    c.endEdit();
    CHECK(fx.track().cues[1].end() == f(55));
    REQUIRE(c.nudgeCue(1, QStringLiteral("start"), 2).isEmpty());
    c.endEdit();
    CHECK(fx.track().cues[1].start == f(27));
    CHECK_FALSE(c.nudgeCue(1, QStringLiteral("start"), -3).isEmpty()); // into cue 0
    CHECK(fx.track().cues[1].start == f(27));
    REQUIRE(c.nudgeCue(2, QStringLiteral("both"), -2).isEmpty());
    c.endEdit();
    CHECK(fx.track().cues[2].start == f(73));
    CHECK(fx.track().cues[2].end() == f(98));
    // Typed timing.
    REQUIRE(c.setCueTiming(0, QStringLiteral("00:00:00:02"), QStringLiteral("00:00:00:20")).isEmpty());
    CHECK(fx.track().cues[0].start == f(2));
    CHECK(fx.track().cues[0].end() == f(20));

    // Split at the playhead (inside the cue only), then join again.
    fx.controller.setPlayhead(f(40));
    REQUIRE(c.activeCue() == 1);
    before = fx.undo();
    REQUIRE(c.splitCueAtPlayhead(1).isEmpty());
    CHECK(fx.undo() == before + 1);
    REQUIRE(fx.track().cues.size() == 4);
    CHECK(fx.track().cues[2].start == f(40));
    CHECK(fx.track().cues[1].text == "Two");
    CHECK(fx.track().cues[2].text == "words");
    CHECK_FALSE(c.splitCueAtPlayhead(0).isEmpty()); // the playhead is not in cue 0
    REQUIRE(c.mergeWithNext(1).isEmpty());
    REQUIRE(fx.track().cues.size() == 3);
    CHECK(fx.track().cues[1].text == "Two words");
    CHECK_FALSE(c.mergeWithNext(2).isEmpty()); // the last cue has no next

    // A new cue at the playhead fills up to the next cue (at most two seconds); not over another cue.
    fx.controller.setPlayhead(f(60));
    REQUIRE(c.addCueAtPlayhead(QStringLiteral("Between")).isEmpty());
    REQUIRE(fx.track().cues.size() == 4);
    CHECK(fx.track().cues[2].start == f(60));
    CHECK(fx.track().cues[2].end() == f(73));
    fx.controller.setPlayhead(f(10));
    CHECK_FALSE(c.addCueAtPlayhead(QStringLiteral("Overlap")).isEmpty());
    REQUIRE(c.removeCue(2).isEmpty());
    CHECK(fx.track().cues.size() == 3);

    // Speakers: a new one assigned to a cue, renamed and recoloured; cleared.
    const QString host = c.addSpeaker(QStringLiteral("Host"));
    REQUIRE_FALSE(host.isEmpty());
    REQUIRE(c.setCueSpeaker(0, host).isEmpty());
    QVariantMap cue = c.cues()[0].toMap();
    CHECK(cue.value(QStringLiteral("speaker")).toString() == host);
    CHECK(cue.value(QStringLiteral("speakerName")).toString() == QStringLiteral("Host"));
    REQUIRE(c.renameSpeaker(host, QStringLiteral("Dr. Somchai"), QStringLiteral("#2EE6A8")).isEmpty());
    cue = c.cues()[0].toMap();
    CHECK(cue.value(QStringLiteral("speakerName")).toString() == QStringLiteral("Dr. Somchai"));
    CHECK(cue.value(QStringLiteral("speakerColor")).toString() == QStringLiteral("#2EE6A8"));
    CHECK(c.speakers().size() >= 1);
    REQUIRE(c.setCueSpeaker(0, {}).isEmpty());
    CHECK(c.cues()[0].toMap().value(QStringLiteral("speaker")).toString().isEmpty());
    CHECK_FALSE(c.setCueSpeaker(0, QStringLiteral("nonsense")).isEmpty());

    // Reading speed: a long text in a short cue reads too fast.
    REQUIRE(c.setCueText(0, QStringLiteral("This caption has far too many words for its very short time on screen")).isEmpty());
    c.endEdit();
    cue = c.cues()[0].toMap();
    CHECK(cue.value(QStringLiteral("cps")).toDouble() > 20.0);
    CHECK(cue.value(QStringLiteral("fast")).toBool());
    CHECK_FALSE(c.cues()[2].toMap().value(QStringLiteral("fast")).toBool());

    // Track settings: name, language, hidden.
    REQUIRE(c.setTrackName(QStringLiteral("Subtitles")).isEmpty());
    REQUIRE(c.setTrackLanguage(QStringLiteral("EN-us")).isEmpty());
    REQUIRE(c.setTrackEnabled(false).isEmpty());
    CHECK(fx.track().name == "Subtitles");
    CHECK(fx.track().language == "en");
    CHECK_FALSE(fx.track().enabled);
    CHECK(c.trackInfo().value(QStringLiteral("enabled")).toBool() == false);
}

TEST_CASE("Corrections: a fix on the transcript suggests Update from transcript, which rewrites the cues",
          "[app][captions][corrections]") {
    Fixture fx;
    REQUIRE(fx.captions.generate(42, 2).isEmpty());
    const QString first = fx.captions.cues().front().toMap().value(QStringLiteral("text")).toString();
    REQUIRE(first.contains(QStringLiteral("wrold")));
    CHECK_FALSE(fx.captions.updateSuggested());

    // Fix "wrold" on the Transcript tab.
    fx.transcriptBridge.selectWords(1, 1);
    CHECK(fx.transcriptBridge.selectedText() == QStringLiteral("wrold"));
    REQUIRE(fx.transcriptBridge.correctSelection(QStringLiteral("world")).isEmpty());
    CHECK(fx.captions.updateSuggested());
    // The cues keep their text until updated.
    CHECK(fx.captions.cues().front().toMap().value(QStringLiteral("text")).toString().contains(QStringLiteral("wrold")));
    const auto before = fx.undo();
    REQUIRE(fx.captions.updateFromTranscript().isEmpty());
    CHECK(fx.undo() == before + 1);
    CHECK_FALSE(fx.captions.updateSuggested());
    CHECK(fx.captions.cues().front().toMap().value(QStringLiteral("text")).toString().contains(QStringLiteral("world")));
    // A run of words is fixed at once; nothing selected is refused.
    fx.transcriptBridge.selectWords(2, 3);
    REQUIRE(fx.transcriptBridge.correctSelection(QStringLiteral("this really is")).isEmpty());
    fx.transcriptBridge.clearSelection();
    CHECK_FALSE(fx.transcriptBridge.correctSelection(QStringLiteral("x")).isEmpty());
}

TEST_CASE("Captions workspace in the shell: the Caption Style panel and the cue list; screenshot",
          "[app][captions][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    REQUIRE(controller);
    REQUIRE(captions);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(captions->addTrack(QStringLiteral("English"), QStringLiteral("Podcast")).isEmpty());
    for (const auto& [from, to, text] : {std::tuple{0, 30, "Welcome back to the show."},
                                         std::tuple{32, 70, "Today we cut a whole episode\nwithout leaving the timeline."},
                                         std::tuple{72, 100, "Let's get into it."}}) {
        REQUIRE(CommandJob::runNow(controller, nullptr, "captions.addCue",
                                   Json{{"start", f(from).value}, {"end", f(to).value}, {"text", text}})
                    .ok());
    }
    const QString host = captions->addSpeaker(QStringLiteral("Host"));
    REQUIRE(captions->setCueSpeaker(0, host).isEmpty());
    controller->setPlayhead(f(40));

    shell.window->setProperty("workspace", 4); // Captions
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("captions")))));
    spin(300);
    QQuickItem* tabs = findNamed(shell.window->contentItem(), QStringLiteral("transcriptTabs"));
    REQUIRE(tabs);
    tabs->setProperty("currentIndex", 2); // Captions
    spin(200);
    QQuickItem* style = findNamed(shell.window->contentItem(), QStringLiteral("captionStylePanel"));
    REQUIRE(style);
    CHECK(style->isVisible());
    QQuickItem* presets = findNamed(style, QStringLiteral("captionPresets"));
    REQUIRE(presets);
    CHECK(presets->isVisible());
    QQuickItem* list = findNamed(shell.window->contentItem(), QStringLiteral("captionCues"));
    REQUIRE(list);
    CHECK(list->isVisible());
    // The preset chips apply on a tap (the bridge call the chip makes).
    REQUIRE(captions->applyPreset(QStringLiteral("Bold Box")).isEmpty());
    spin(100);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("caption-workshop.png"))));
    shell.window->setProperty("workspace", 0);
    spin(100);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Captions page 0.2: Find and Replace in captions and the transcript, Import captions, Export transcript",
          "[app][captions][findReplace]") {
    Fixture fx;
    fx.threeCues();
    const auto before = fx.undo();
    // This track: "two" anywhere, any case.
    REQUIRE(fx.captions.findReplace(QStringLiteral("two"), QStringLiteral("2"), false, false, QStringLiteral("track")).isEmpty());
    CHECK(fx.undo() == before + 1);
    CHECK(fx.captions.cues()[1].toMap().value(QStringLiteral("text")).toString() == QStringLiteral("2 words here"));
    CHECK(fx.captions.message() == QStringLiteral("Replaced 1 match(es)"));
    // Whole word: "here" but not inside "Three".
    REQUIRE(fx.captions.findReplace(QStringLiteral("re"), QStringLiteral("X"), false, true, QStringLiteral("all")).isEmpty());
    CHECK(fx.captions.message().contains(QStringLiteral("was not found")));
    // The transcript: a correction, so "Update from transcript" is suggested.
    REQUIRE(fx.captions.findReplace(QStringLiteral("wrold"), QStringLiteral("world"), true, true, QStringLiteral("transcript")).isEmpty());
    CHECK(fx.transcriptBridge.run("transcript.text", Json::object()).data["paragraphs"][0][5] == "Hello world");

    // Export the transcript, import it back as a caption track.
    const QString dir = QDir::temp().filePath(QStringLiteral("wabcut-captions-findreplace"));
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QUrl txt = QUrl::fromLocalFile(QDir(dir).filePath(QStringLiteral("talk.txt")));
    REQUIRE(fx.captions.exportTranscript(txt).isEmpty());
    REQUIRE(fx.captions.importFile(txt).isEmpty());
    CHECK(fx.controller.currentSequence()->captionTracks.size() == 2);
    CHECK(fx.captions.cues().size() == 2);
    CHECK(fx.captions.cues()[0].toMap().value(QStringLiteral("text")).toString() == QStringLiteral("Hello world"));
    CHECK_FALSE(fx.captions.importFile(QUrl::fromLocalFile(QDir(dir).filePath(QStringLiteral("missing.srt")))).isEmpty());
    QDir(dir).removeRecursively();
}

// ---- 0.4: Shift Captions… --------------------------------------------------------------------------------------------

namespace {

/// A 10 s clip with two caption tracks: the first has one cue at 1 s; the second (shown) has cues at 1 s, 3 s and 5 s,
/// each a second long.
struct ShiftFixture {
    Timeline tl{"V1 tgt |A" + std::string(249, 'a') + "|\n"};
    ProjectController controller;
    CaptionsBridge captions;
    CaptionTrackId first, second;

    ShiftFixture() {
        controller.resetProject(tl.state, tl.seq);
        captions.setController(&controller);
        REQUIRE(captions.addTrack(QStringLiteral("First"), {}).isEmpty());
        first = controller.currentSequence()->captionTracks.back().id;
        addCue(first, 25, "Other track");
        REQUIRE(captions.addTrack(QStringLiteral("Second"), {}).isEmpty());
        second = controller.currentSequence()->captionTracks.back().id;
        REQUIRE(captions.shownTrackId() == second);
        addCue(second, 25, "One");
        addCue(second, 75, "Two");
        addCue(second, 125, "Three");
        REQUIRE(captions.cues().size() == 3);
    }
    void addCue(CaptionTrackId track, std::int64_t at, const char* text) {
        const auto r = CommandJob::runNow(&controller, nullptr, "captions.addCue",
                                          Json{{"captionTrack", track.value}, {"start", f(at).value}, {"end", f(at + 25).value},
                                               {"text", text}});
        INFO(commandErrorText(r).toStdString());
        REQUIRE(r.ok());
    }
    std::vector<Ticks> starts(CaptionTrackId id) const {
        std::vector<Ticks> out;
        for (const CaptionTrack& t : controller.currentSequence()->captionTracks) {
            if (t.id == id) {
                for (const CaptionCue& c : t.cues)
                    out.push_back(c.start);
            }
        }
        return out;
    }
    std::size_t undos() { return controller.session().undoCount(); }
};

} // namespace

TEST_CASE("Shift Captions: all or from the playhead on the shown track in one undo step", "[app][captions][shift]") {
    ShiftFixture fx;
    // All captions 12 frames earlier: only the shown track moves; one undo step.
    const auto before = fx.undos();
    REQUIRE(fx.captions.shiftCues(QStringLiteral("-00:00:00:12"), QStringLiteral("all")).isEmpty());
    CHECK(fx.undos() == before + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Shift Captions"));
    CHECK(fx.starts(fx.second) == std::vector<Ticks>{f(13), f(63), f(113)});
    CHECK(fx.starts(fx.first) == std::vector<Ticks>{f(25)});
    REQUIRE(fx.controller.undo());
    CHECK(fx.starts(fx.second) == std::vector<Ticks>{f(25), f(75), f(125)});

    // From the playhead (3.2 s, inside the second cue): that cue and the later ones; frames and a "+" work too.
    fx.controller.setPlayhead(f(80));
    REQUIRE(fx.captions.shiftCues(QStringLiteral("-12"), QStringLiteral("fromPlayhead")).isEmpty());
    CHECK(fx.starts(fx.second) == std::vector<Ticks>{f(25), f(63), f(113)});
    REQUIRE(fx.controller.undo());
    // Between cues (2.4 s): the first cue that starts after it.
    fx.controller.setPlayhead(f(60));
    REQUIRE(fx.captions.shiftCues(QStringLiteral("+00:00:00:05"), QStringLiteral("fromPlayhead")).isEmpty());
    CHECK(fx.starts(fx.second) == std::vector<Ticks>{f(25), f(80), f(130)});
    CHECK(fx.captions.message().isEmpty());
}

TEST_CASE("Shift Captions: a refused offset changes nothing and says why", "[app][captions][shift]") {
    ShiftFixture fx;
    const auto before = fx.undos();
    const std::vector<Ticks> starts = fx.starts(fx.second);
    // Before 0.
    CHECK_FALSE(fx.captions.shiftCues(QStringLiteral("-00:00:02:00"), QStringLiteral("all")).isEmpty());
    CHECK_FALSE(fx.captions.message().isEmpty());
    // Onto a cue that stays: from the second cue, 1 s 12 frames earlier.
    fx.controller.setPlayhead(f(80));
    CHECK_FALSE(fx.captions.shiftCues(QStringLiteral("-1:12"), QStringLiteral("fromPlayhead")).isEmpty());
    // No offset, a zero offset, an unknown scope; no caption at or after the playhead.
    CHECK_FALSE(fx.captions.shiftCues(QString(), QStringLiteral("all")).isEmpty());
    CHECK_FALSE(fx.captions.shiftCues(QStringLiteral("0"), QStringLiteral("all")).isEmpty());
    CHECK_FALSE(fx.captions.shiftCues(QStringLiteral("abc"), QStringLiteral("all")).isEmpty());
    CHECK_FALSE(fx.captions.shiftCues(QStringLiteral("12"), QStringLiteral("everything")).isEmpty());
    fx.controller.setPlayhead(f(200));
    CHECK(fx.captions.shiftCues(QStringLiteral("12"), QStringLiteral("fromPlayhead")) ==
          QStringLiteral("There is no caption at or after the playhead"));
    CHECK(fx.undos() == before);
    CHECK(fx.starts(fx.second) == starts);
}

TEST_CASE("Command palette fills captionTrack from the shown caption track", "[app][captions][shift][palette]") {
    Shell shell;
    REQUIRE(shell.window);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    auto* commands = shell.engine.singletonInstance<CommandBridge*>("Wabcut", "CommandBridge");
    REQUIRE(controller);
    REQUIRE(captions);
    REQUIRE(commands);
    Timeline tl{"V1 tgt |A" + std::string(249, 'a') + "|\n"};
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(captions->addTrack(QStringLiteral("First"), {}).isEmpty());
    REQUIRE(captions->addTrack(QStringLiteral("Second"), {}).isEmpty());
    const CaptionTrackId first = controller->currentSequence()->captionTracks.front().id;
    const CaptionTrackId second = controller->currentSequence()->captionTracks.back().id;
    REQUIRE(captions->shownTrackId() == second);
    for (const CaptionTrackId track : {first, second}) {
        const auto r = CommandJob::runNow(controller, nullptr, "captions.addCue",
                                          Json{{"captionTrack", track.value}, {"start", f(25).value}, {"end", f(50).value},
                                               {"text", "Cue"}});
        REQUIRE(r.ok());
    }

    // The palette row does not ask for a raw track id.
    bool listed = false;
    for (const QVariant& v : commands->commands()) {
        const QVariantMap c = v.toMap();
        if (c.value(QStringLiteral("name")).toString() != QStringLiteral("captions.shiftCues"))
            continue;
        listed = true;
        for (const QVariant& p : c.value(QStringLiteral("params")).toList())
            CHECK(p.toMap().value(QStringLiteral("key")).toString() != QStringLiteral("captionTrack"));
    }
    CHECK(listed);

    // Running it moves the shown track only.
    const QVariantMap result = commands->apply(QStringLiteral("captions.shiftCues"),
                                               {{QStringLiteral("delta"), static_cast<qlonglong>(f(5).value)}});
    INFO(result.value(QStringLiteral("detail")).toString().toStdString());
    REQUIRE(result.value(QStringLiteral("ok")).toBool());
    const auto startOf = [&](CaptionTrackId id) {
        for (const CaptionTrack& t : controller->currentSequence()->captionTracks) {
            if (t.id == id)
                return t.cues.front().start;
        }
        return Ticks{-1};
    };
    CHECK(startOf(second) == f(30));
    CHECK(startOf(first) == f(25));
}

TEST_CASE("Shift Captions row in the shell; screenshot", "[app][captions][shift][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    REQUIRE(controller);
    REQUIRE(captions);
    Timeline tl{"V1 tgt |A" + std::string(249, 'a') + "|\nA1 tgt |A" + std::string(249, 'a') + "|\n"};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(captions->addTrack(QStringLiteral("English"), {}).isEmpty());
    for (const auto& [at, text] : {std::pair{25, "Welcome back to the show."}, std::pair{75, "The subtitles were half a second late."},
                                   std::pair{125, "Now they land on the words."}}) {
        REQUIRE(CommandJob::runNow(controller, nullptr, "captions.addCue",
                                   Json{{"start", f(at).value}, {"end", f(at + 25).value}, {"text", text}})
                    .ok());
    }
    const auto starts = [&] {
        std::vector<Ticks> out;
        for (const CaptionCue& c : controller->currentSequence()->captionTracks.front().cues)
            out.push_back(c.start);
        return out;
    };
    REQUIRE(starts() == std::vector<Ticks>{f(25), f(75), f(125)});
    controller->setPlayhead(f(80));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("captions")))));
    spin(300);
    QQuickItem* tabs = findNamed(shell.window->contentItem(), QStringLiteral("transcriptTabs"));
    REQUIRE(tabs);
    tabs->setProperty("currentIndex", 2); // Captions
    spin(200);
    REQUIRE_FALSE(shell.grab().isNull()); // lays the hidden window out

    // The track menu's Shift Captions… opens the row once the menu has closed, the offset field focused.
    QQuickItem* row = findNamed(shell.window->contentItem(), QStringLiteral("captionsShiftRow"));
    REQUIRE(row);
    CHECK_FALSE(row->isVisible());
    QQuickItem* trackButton = findNamed(shell.window->contentItem(), QStringLiteral("captionsTrackMenu"));
    REQUIRE(trackButton);
    REQUIRE(QMetaObject::invokeMethod(trackButton, "clicked"));
    QQuickItem* shiftItem = nullptr;
    REQUIRE(waitFor([&] {
        shiftItem = findNamed(shell.window->contentItem(), QStringLiteral("captionsShiftItem"));
        return shiftItem != nullptr && shiftItem->isVisible();
    }, 3000));
    CHECK(shiftItem->isEnabled());
    QObject* menu = shiftItem->property("menu").value<QObject*>();
    REQUIRE(menu);
    REQUIRE(QMetaObject::invokeMethod(shiftItem, "triggered"));
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool() && row->isVisible(); }, 3000));
    QQuickItem* field = findNamed(row, QStringLiteral("captionsShiftField"));
    QQuickItem* scope = findNamed(row, QStringLiteral("captionsShiftScope"));
    QQuickItem* apply = findNamed(row, QStringLiteral("captionsShiftApply"));
    QQuickItem* error = findNamed(row, QStringLiteral("captionsShiftError"));
    REQUIRE(field);
    REQUIRE(scope);
    REQUIRE(apply);
    REQUIRE(error);
    CHECK(field->hasActiveFocus());
    CHECK(field->property("placeholderText").toString() == QStringLiteral("-00:00:00:12"));
    CHECK_FALSE(apply->isEnabled()); // no offset yet

    // From the playhead (3.2 s, in the second caption), 12 frames earlier.
    field->setProperty("text", QStringLiteral("-00:00:00:12"));
    scope->setProperty("currentIndex", 1);
    CHECK(apply->isEnabled());
    spin(100);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("captions-shift.png"))));
        QQuickItem* page = findNamed(shell.window->contentItem(), QStringLiteral("captionsPage"));
        REQUIRE(page);
        const double dpr = image.devicePixelRatio();
        const QRectF r = page->mapRectToScene(QRectF(0, 0, page->width(), page->height()))
                             .adjusted(-12, -60, 12, 12)
                             .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect())
                    .save(QDir(outputDir()).filePath(QStringLiteral("captions-shift-crop.png"))));
    }
    const auto undos = controller->session().undoCount();
    apply->forceActiveFocus(Qt::MouseFocusReason); // as a click on Shift does
    REQUIRE(QMetaObject::invokeMethod(apply, "clicked"));
    CHECK(starts() == std::vector<Ticks>{f(25), f(63), f(113)});
    CHECK(controller->session().undoCount() == undos + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Shift Captions"));
    CHECK(row->isVisible()); // open for another try
    CHECK(field->hasActiveFocus()); // from the keyboard

    // A refused offset (before 0): nothing changes, the row says why.
    field->setProperty("text", QStringLiteral("-00:00:05:00"));
    scope->setProperty("currentIndex", 0);
    REQUIRE(QMetaObject::invokeMethod(apply, "clicked"));
    CHECK(starts() == std::vector<Ticks>{f(25), f(63), f(113)});
    CHECK(controller->session().undoCount() == undos + 1);
    CHECK(error->property("text").toString() == captions->message());
    CHECK_FALSE(captions->message().isEmpty());

    const auto escape = [&] {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(shell.window.get(), &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(shell.window.get(), &release);
        spin(50);
    };
    // Esc closes the row with the focus on a button or the scope tabs too (a click focuses them)...
    apply->forceActiveFocus(Qt::MouseFocusReason);
    REQUIRE(apply->hasActiveFocus());
    escape();
    CHECK_FALSE(row->isVisible());
    QQuickItem* page = findNamed(shell.window->contentItem(), QStringLiteral("captionsPage"));
    REQUIRE(page);
    REQUIRE(QMetaObject::invokeMethod(page, "openShiftRow"));
    REQUIRE(row->isVisible());
    scope->forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(scope->hasActiveFocus());
    escape();
    CHECK_FALSE(row->isVisible());
    // ...and in the field.
    REQUIRE(QMetaObject::invokeMethod(page, "openShiftRow"));
    REQUIRE(row->isVisible());
    field->forceActiveFocus(Qt::OtherFocusReason);
    escape();
    CHECK_FALSE(row->isVisible());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
