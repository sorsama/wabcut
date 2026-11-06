// 0.6 (P1 round, stream 2E): the Transcript tab's speakers, the voice-over script prompter and the loudness QC.
//   * #36: paragraphs carry the project speaker's name and colour (else "Speaker N"); a paragraph's speaker is renamed
//     or recoloured, its words or the selected words are given to a speaker or a new one, a speaker is merged into
//     another - one undo step each;
//   * #53: a narration script (pasted or a .txt / .md file, Markdown stripped) in paragraphs, stepped through, kept by
//     project file with the paragraph reached; the prompter shows while a track is armed; a marker per paragraph read
//     while recording (a real pass on the Null input); Settings > Audio's section; the overlay band; screenshots;
//   * #54: the mix analysis says where its loudest moments and true-peak overs are; the QC block's links move the
//     playhead and "Add markers for overs" adds red range markers in one undo step; screenshots (dark and light).
// Offscreen; no window is shown and no audio device is opened.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"
#include "VoiceOverBridge.h"

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/AudioInput.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/editor/PlaybackSession.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

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

QQuickItem* findVisibleNamed(QQuickItem* root, const QString& name) {
    if (!root || !root->isVisible())
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findVisibleNamed(child, name))
            return found;
    }
    return nullptr;
}

QObject* child(QObject* root, const char* name) {
    QObject* found = root->findChild<QObject*>(QString::fromLatin1(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

void saveItemShot(const QImage& image, const QQuickItem* item, const QString& name) {
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

// ---- #36: speakers ------------------------------------------------------------------------------------------------

struct W {
    std::string text;
    double from = 0; ///< source frames (25 fps)
    double to = 0;
    std::optional<int> speaker = std::nullopt;
};

std::shared_ptr<const ai::Transcript> transcript(std::vector<W> words) {
    auto t = std::make_shared<ai::Transcript>();
    t->language = "en";
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

/// Two linked A/V clips of media A (40 frames): "Hello world um" by diarization label 0, "this is great" by label 1.
struct SpeakerFixture {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    ProjectController controller;
    TranscriptBridge bridge;

    SpeakerFixture() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        bridge.setUseAnalysisCache(false);
        bridge.setController(&controller);
        bridge.addTranscript(tl.media('A'), 0,
                             transcript({{"Hello", 0, 4, 0},
                                         {"world", 5, 9, 0},
                                         {"um", 12, 14, 0},
                                         {"this", 22, 25, 1},
                                         {"is", 26, 28, 1},
                                         {"great", 29, 34, 1}}));
    }
    QVariantMap paragraph(int i) const { return bridge.paragraphs()->get(i); }
    QString speaker(int i) const { return paragraph(i).value(QStringLiteral("speaker")).toString(); }
    QString key(int i) const { return paragraph(i).value(QStringLiteral("speakerKey")).toString(); }
    int count() const { return bridge.paragraphs()->rowCount(); }
    std::size_t undo() { return controller.session().undoCount(); }
    /// The speaker of every paragraph, in order.
    QStringList speakers() const {
        QStringList out;
        for (int i = 0; i < count(); ++i)
            out << speaker(i);
        return out;
    }
};

// ---- #53: the prompter --------------------------------------------------------------------------------------------

constexpr const char* kVoicePicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaa|
    A2     |.........................|
)";

constexpr const char* kScript = "# Chapter one\n"
                                "\n"
                                "The *first* paragraph of the narration\n"
                                "goes on a second line.\n"
                                "\n"
                                "- **Second** paragraph, a list item read aloud\n"
                                "\n"
                                "## Part two\n"
                                "Third paragraph straight after a heading, long enough to need the ellipsis in its marker name.\n";

/// A project saved as <temp>/Film.wabcut, playback on the Null engine and the VoiceOver bridge on the Null input, A1
/// armed, no count-in, pre- or post-roll (as the VoiceOver bridge tests do).
struct VoiceRig {
    QTemporaryDir dir;
    Timeline tl{kVoicePicture};
    ProjectController controller;
    std::unique_ptr<PlaybackBridge> playback;
    std::unique_ptr<VoiceOverBridge> voiceOver;

    VoiceRig() {
        REQUIRE(dir.isValid());
        qputenv("WABCUT_AUDIO_BACKEND", "null");
        controller.resetProject(tl.state, tl.seq);
        controller.setProjectFile(projectFile());
        playback = std::make_unique<PlaybackBridge>(&controller);
        voiceOver = std::make_unique<VoiceOverBridge>();
        voiceOver->setController(&controller);
        voiceOver->setPlayback(playback.get());
        voiceOver->setOptions(QVariantMap{{QStringLiteral("countIn"), 0},
                                          {QStringLiteral("prerollSeconds"), 0.0},
                                          {QStringLiteral("postrollSeconds"), 0.0}});
    }
    ~VoiceRig() {
        voiceOver.reset();
        playback.reset();
        qunsetenv("WABCUT_AUDIO_BACKEND");
    }
    std::filesystem::path projectFile() const { return std::filesystem::path(dir.path().toStdWString()) / "Film.wabcut"; }
    void step() {
        std::vector<float> out(480 * 2);
        REQUIRE(playback->session()->engine().pull(out));
        REQUIRE(voiceOver->input().push(std::vector<float>(480, 0.25f)));
        spin(10);
    }
    const std::vector<Marker>& markers() const { return controller.currentSequence()->markers; }
};

// ---- #54: the loudness QC -----------------------------------------------------------------------------------------

Ticks ms(std::int64_t millis) { return Ticks{millis * (kTicksPerSecond / 1000)}; }

/// The mix and every track: -16 LUFS, the loudest momentary window from 2 s, short-term from 1 s, the true peak
/// (+0.4 dBTP) at 2.3 s, hot blocks at 2.2-2.5 s and 5.0-5.1 s (-0.5 dBTP) above -1 dBTP and one at 7 s below it.
class WhereLoudness final : public audio::edit::LoudnessAnalyzer {
public:
    std::expected<audio::edit::LoudnessStats, Error> measure(const ProjectState&, SequenceId, const audio::edit::LoudnessTarget&,
                                                             std::optional<TimeRange>, command::ExecutionControl*) override {
        audio::edit::LoudnessStats stats;
        stats.integratedLufs = -16.0;
        stats.rangeLu = 5.0;
        stats.truePeakDbtp = 0.4;
        stats.samplePeakDbfs = 0.1;
        stats.maxMomentaryLufs = -9.0;
        stats.maxShortTermLufs = -12.0;
        stats.duration = ms(10000);
        stats.maxMomentaryAt = ms(2000);
        stats.maxShortTermAt = ms(1000);
        stats.truePeakAt = ms(2300);
        stats.peakFloorDbtp = -6.0;
        for (const auto& [at, db] : std::vector<std::pair<std::int64_t, double>>{{2200, -0.8}, {2300, 0.4}, {2400, -0.2},
                                                                                 {5000, -0.5}, {7000, -3.0}})
            stats.peakBlocks.push_back({ms(at), ms(100), db});
        return stats;
    }
};

std::shared_ptr<audio::edit::AudioServices> whereServices() {
    return std::make_shared<audio::edit::AudioServices>(std::make_shared<WhereLoudness>(), nullptr);
}

/// Card 166: a very clipped programme, measured with only the blocks above -0.5 dBTP kept (the floor rose from -6, past
/// the mixer's -1 dBTP ceiling): an over of two blocks at 5.0 s and another at 8.0 s.
class HotFloorLoudness final : public audio::edit::LoudnessAnalyzer {
public:
    std::expected<audio::edit::LoudnessStats, Error> measure(const ProjectState&, SequenceId, const audio::edit::LoudnessTarget&,
                                                             std::optional<TimeRange>, command::ExecutionControl*) override {
        audio::edit::LoudnessStats stats;
        stats.integratedLufs = -9.0;
        stats.truePeakDbtp = 0.3;
        stats.maxMomentaryLufs = -6.0;
        stats.maxShortTermLufs = -7.0;
        stats.duration = ms(10000);
        stats.truePeakAt = ms(5000);
        stats.peakFloorDbtp = -0.5;
        for (const auto& [at, db] : std::vector<std::pair<std::int64_t, double>>{{5000, 0.3}, {5100, -0.2}, {8000, 0.1}})
            stats.peakBlocks.push_back({ms(at), ms(100), db});
        return stats;
    }
};

} // namespace

// ---- #36 ----------------------------------------------------------------------------------------------------------

TEST_CASE("Transcript speakers: a label is renamed then recoloured from its paragraph in one undo step each",
          "[app][transcript][speakers]") {
    SpeakerFixture fx;
    REQUIRE(fx.count() == 2);
    CHECK(fx.speakers() == QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Speaker 2")});
    CHECK(fx.key(0).isEmpty()); // a diarization label nobody named yet
    CHECK(fx.paragraph(0).value(QStringLiteral("speakerColor")).toString().isEmpty());

    // Rename: a project speaker for label 0 of media A, which every paragraph of the label follows.
    std::size_t undo = fx.undo();
    CHECK(fx.bridge.renameParagraphSpeaker(0, QStringLiteral("  Ana "), {}).isEmpty());
    CHECK(fx.undo() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Rename Speaker"));
    REQUIRE(fx.count() == 2);
    CHECK(fx.speakers() == QStringList{QStringLiteral("Ana"), QStringLiteral("Speaker 2")});
    const QString ana = fx.key(0);
    CHECK(ana.startsWith(QLatin1Char('v')));
    CHECK(fx.paragraph(0).value(QStringLiteral("speakerColor")).toString().startsWith(QLatin1Char('#')));

    // Recolour the named speaker: the name stays.
    undo = fx.undo();
    CHECK(fx.bridge.renameParagraphSpeaker(0, {}, QStringLiteral("#FF8800")).isEmpty());
    CHECK(fx.undo() == undo + 1);
    CHECK(fx.speaker(0) == QStringLiteral("Ana"));
    CHECK(fx.key(0) == ana);
    CHECK(fx.paragraph(0).value(QStringLiteral("speakerColor")).toString() == QStringLiteral("#FF8800"));

    // Nothing to do, or no such paragraph: refused with a reason, nothing changes.
    undo = fx.undo();
    CHECK_FALSE(fx.bridge.renameParagraphSpeaker(0, QStringLiteral("  "), {}).isEmpty());
    CHECK_FALSE(fx.bridge.renameParagraphSpeaker(7, QStringLiteral("Zed"), {}).isEmpty());
    CHECK(fx.undo() == undo);

    // Undo twice: back to the raw label.
    REQUIRE(fx.controller.undo());
    CHECK(fx.paragraph(0).value(QStringLiteral("speakerColor")).toString() != QStringLiteral("#FF8800"));
    REQUIRE(fx.controller.undo());
    CHECK(fx.speakers() == QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Speaker 2")});
    CHECK(fx.key(0).isEmpty());
}

TEST_CASE("Transcript speakers: a paragraph and the selected words are given to a speaker or a new one",
          "[app][transcript][speakers]") {
    SpeakerFixture fx;
    // "This paragraph is" a new speaker: Ben speaks the second paragraph.
    std::size_t undo = fx.undo();
    CHECK(fx.bridge.assignParagraph(1, {}, QStringLiteral("Ben")).isEmpty());
    CHECK(fx.undo() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Assign Speaker"));
    REQUIRE(fx.count() == 2);
    CHECK(fx.speakers() == QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Ben")});
    const QString ben = fx.key(1);
    REQUIRE(ben.startsWith(QLatin1Char('v')));

    // The first paragraph is Ben's too: the two join (no pause long enough to part them).
    undo = fx.undo();
    CHECK(fx.bridge.assignParagraph(0, ben).isEmpty());
    CHECK(fx.undo() == undo + 1);
    CHECK(fx.count() == 1);
    CHECK(fx.speakers() == QStringList{QStringLiteral("Ben")});
    // Already theirs: nothing to do.
    undo = fx.undo();
    CHECK(fx.bridge.assignParagraph(0, ben).isEmpty());
    CHECK(fx.undo() == undo);
    REQUIRE(fx.controller.undo());
    CHECK(fx.speakers() == QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Ben")});

    // The selected words ("world um") to a new speaker: their own paragraph between the others.
    fx.bridge.selectWords(1, 2);
    undo = fx.undo();
    CHECK(fx.bridge.assignSelection({}, QStringLiteral("Cleo")).isEmpty());
    CHECK(fx.undo() == undo + 1);
    CHECK(fx.speakers() == QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Cleo"), QStringLiteral("Ben")});
    CHECK(fx.paragraph(1).value(QStringLiteral("first")).toInt() == 1);
    CHECK(fx.paragraph(1).value(QStringLiteral("last")).toInt() == 2);
    // And then to Ben: Cleo's paragraph joins his.
    fx.bridge.selectWords(1, 2);
    CHECK(fx.bridge.assignSelection(ben).isEmpty());
    CHECK(fx.speakers() == QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Ben")});
    REQUIRE(fx.controller.undo());
    CHECK(fx.speakers() == QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Cleo"), QStringLiteral("Ben")});

    // Refusals: nothing selected, no name, no such speaker.
    fx.bridge.clearSelection();
    undo = fx.undo();
    CHECK_FALSE(fx.bridge.assignSelection(ben).isEmpty());
    CHECK_FALSE(fx.bridge.assignParagraph(0, {}, QStringLiteral("   ")).isEmpty());
    CHECK_FALSE(fx.bridge.assignParagraph(0, QStringLiteral("v999999")).isEmpty());
    CHECK(fx.undo() == undo);
}

TEST_CASE("Transcript speakers: Merge into makes one speaker of two in one undo step, a raw label included",
          "[app][transcript][speakers]") {
    SpeakerFixture fx;
    REQUIRE(fx.bridge.renameParagraphSpeaker(1, QStringLiteral("Ben"), {}).isEmpty());
    const QString ben = fx.key(1);
    REQUIRE_FALSE(ben.isEmpty());

    // The first paragraph is a label with no speaker: it gets one and merges into Ben in the same undo step.
    std::size_t undo = fx.undo();
    CHECK(fx.bridge.mergeParagraphSpeaker(0, ben).isEmpty());
    CHECK(fx.undo() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Merge Speakers"));
    CHECK(fx.speakers() == QStringList{QStringLiteral("Ben")});
    REQUIRE(fx.controller.undo());
    CHECK(fx.speakers() == QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Ben")});

    // Two named speakers: Ana merges into Ben (transcript.mergeSpeakers).
    REQUIRE(fx.bridge.renameParagraphSpeaker(0, QStringLiteral("Ana"), {}).isEmpty());
    undo = fx.undo();
    CHECK(fx.bridge.mergeParagraphSpeaker(0, ben).isEmpty());
    CHECK(fx.undo() == undo + 1);
    CHECK(fx.speakers() == QStringList{QStringLiteral("Ben")});
    REQUIRE(fx.controller.undo());
    CHECK(fx.speakers() == QStringList{QStringLiteral("Ana"), QStringLiteral("Ben")});
    // Into itself or an unknown key: nothing.
    undo = fx.undo();
    CHECK(fx.bridge.mergeParagraphSpeaker(1, ben).isEmpty());
    CHECK_FALSE(fx.bridge.mergeParagraphSpeaker(0, QStringLiteral("nope")).isEmpty());
    CHECK(fx.undo() == undo);
}

TEST_CASE("Transcript tab in the shell: the speaker label is a button with its menu and the selection's Speaker button",
          "[app][transcript][speakers][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* transcripts = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(controller);
    REQUIRE(transcripts);
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    transcripts->setUseAnalysisCache(false);
    transcripts->addTranscript(tl.media('A'), 0,
                               transcript({{"Hello", 0, 4, 0}, {"world", 5, 9, 0}, {"this", 22, 25, 1}, {"is", 26, 28, 1}}));
    REQUIRE(transcripts->renameParagraphSpeaker(1, QStringLiteral("Ben"), QStringLiteral("#3BA0FF")).isEmpty());
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);

    QQuickItem* label0 = findNamed(shell.window->contentItem(), QStringLiteral("transcriptSpeaker_0"));
    QQuickItem* label1 = findNamed(shell.window->contentItem(), QStringLiteral("transcriptSpeaker_1"));
    REQUIRE(label0);
    REQUIRE(label1);
    CHECK(label0->property("text").toString().isEmpty()); // the label is its content item's text
    QQuickItem* text1 = label1->property("contentItem").value<QQuickItem*>();
    REQUIRE(text1);
    CHECK(text1->property("text").toString() == QStringLiteral("Ben"));
    CHECK(text1->property("color").value<QColor>() == QColor(QStringLiteral("#3BA0FF")));
    // Its menu has the three rows; the merge submenu offers the other speaker.
    QObject* menu = child(shell.window.get(), "transcriptSpeakerMenu");
    CHECK(child(menu, "transcriptSpeakerRename"));
    QObject* merge = child(menu, "transcriptSpeakerMerge");
    QObject* assign = child(menu, "transcriptSpeakerAssign");
    REQUIRE(QMetaObject::invokeMethod(label0, "clicked"));
    REQUIRE(waitFor([&] { return menu->property("opened").toBool() || menu->property("visible").toBool(); }, 3000));
    CHECK(menu->property("paragraph").toInt() == 0);
    CHECK(menu->property("speakerKey").toString().isEmpty());
    CHECK(merge->property("offered").toList().size() == 1); // Ben
    CHECK(assign->property("offered").toList().size() == 1);
    // "This paragraph is" Ben from the submenu: one undo step, the paragraphs join.
    const std::size_t undo = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(assign, "chosen", Q_ARG(QString, transcripts->paragraphs()->get(1).value("speakerKey").toString())));
    CHECK(controller->session().undoCount() == undo + 1);
    CHECK(transcripts->paragraphs()->rowCount() == 1);
    QMetaObject::invokeMethod(menu, "close");
    spin(100);

    // The selected words get a Speaker button in the actions bar.
    transcripts->selectWords(0, 1);
    spin(100);
    QQuickItem* speakerButton = findNamed(shell.window->contentItem(), QStringLiteral("transcriptSelectionSpeaker"));
    REQUIRE(speakerButton);
    CHECK(speakerButton->isVisible());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- #53 ----------------------------------------------------------------------------------------------------------

TEST_CASE("Script prompter: Markdown scripts become paragraphs to read", "[app][voiceover][prompter]") {
    const QStringList p = VoiceOverBridge::scriptParagraphs(QString::fromUtf8(kScript));
    REQUIRE(p.size() == 3);
    CHECK(p[0] == QStringLiteral("The first paragraph of the narration goes on a second line."));
    CHECK(p[1] == QStringLiteral("Second paragraph, a list item read aloud"));
    CHECK(p[2].startsWith(QStringLiteral("Third paragraph straight after a heading")));
    // No blank lines at all: a line is a paragraph (the prompter never shows the whole script at once).
    CHECK(VoiceOverBridge::scriptParagraphs(QStringLiteral("One.\r\nTwo.\r\nThree.")) ==
          QStringList{QStringLiteral("One."), QStringLiteral("Two."), QStringLiteral("Three.")});
    // Rules, quotes, code marks, _emphasis_.
    CHECK(VoiceOverBridge::scriptParagraphs(QStringLiteral("> A `quoted` _line_\n\n---\n\n~~gone~~ marks")) ==
          QStringList{QStringLiteral("A quoted line"), QStringLiteral("gone marks")});
    CHECK(VoiceOverBridge::scriptParagraphs(QStringLiteral("\n\n# Only a heading\n\n")).isEmpty());
}

TEST_CASE("Script prompter: paste or open a script, step through it, kept by project with the place reached",
          "[app][voiceover][prompter]") {
    resetSettings();
    VoiceRig r;
    VoiceOverBridge& vo = *r.voiceOver;
    CHECK(vo.paragraphs().isEmpty());
    CHECK_FALSE(vo.prompterVisible());

    // Pasted text.
    CHECK(vo.setScriptText(QString::fromUtf8(kScript)).isEmpty());
    CHECK(vo.paragraphs().size() == 3);
    CHECK(vo.scriptPath().isEmpty());
    CHECK(vo.paragraphIndex() == 0);
    CHECK_FALSE(vo.previousParagraph());
    CHECK(vo.nextParagraph());
    CHECK(vo.nextParagraph());
    CHECK_FALSE(vo.nextParagraph()); // the end
    CHECK(vo.paragraphIndex() == 2);
    CHECK(vo.previousParagraph());
    CHECK(vo.paragraphIndex() == 1);
    vo.setParagraphIndex(99);
    CHECK(vo.paragraphIndex() == 2); // clamped
    vo.setParagraphIndex(1);

    // Refusals keep the script as it is.
    CHECK_FALSE(vo.setScriptText(QStringLiteral("# just a heading")).isEmpty());
    CHECK_FALSE(vo.setScriptText(QString(300 * 1024, QLatin1Char('a'))).isEmpty());
    CHECK(vo.paragraphs().size() == 3);
    CHECK_FALSE(vo.loadScript(QUrl::fromLocalFile(r.dir.filePath(QStringLiteral("script.docx")))).isEmpty());
    CHECK_FALSE(vo.loadScript(QUrl::fromLocalFile(r.dir.filePath(QStringLiteral("missing.md")))).isEmpty());

    // Shown while a track is armed (Show when armed is on by default), not otherwise.
    CHECK(vo.showScript());
    CHECK_FALSE(vo.armed());
    REQUIRE(vo.setRecordArm(trackKey(r.tl.track("A1")), true).isEmpty());
    CHECK(vo.armed());
    CHECK(vo.prompterVisible());
    vo.setShowScript(false);
    CHECK_FALSE(vo.prompterVisible());
    vo.setShowScript(true);

    // The options are the user's: a new bridge reads them.
    vo.setPrompterTextSize(200);
    CHECK(vo.prompterTextSize() == 96);
    vo.setPrompterTextSize(48);
    vo.setMirror(true);
    vo.setMarkerPerParagraph(true);

    // The script and the place reached are kept by project file: the project opened again finds them.
    {
        VoiceOverBridge again;
        again.setController(&r.controller);
        CHECK(again.paragraphs().size() == 3);
        CHECK(again.paragraphIndex() == 1);
        CHECK(again.prompterTextSize() == 48);
        CHECK(again.mirror());
        CHECK(again.markerPerParagraph());
    }
    // Another project file: no script there.
    r.controller.setProjectFile(std::filesystem::path(r.dir.path().toStdWString()) / "Other.wabcut");
    {
        VoiceOverBridge other;
        other.setController(&r.controller);
        CHECK(other.paragraphs().isEmpty());
    }
    r.controller.setProjectFile(r.projectFile());

    // A file: .md read and stripped; its path shown.
    const QString path = r.dir.filePath(QStringLiteral("narration.md"));
    {
        QFile out(path);
        REQUIRE(out.open(QIODevice::WriteOnly));
        out.write("# Title\n\nFirst from the file.\n\nSecond from the file.\n");
    }
    CHECK(vo.loadScript(QUrl::fromLocalFile(path)).isEmpty());
    CHECK(vo.scriptPath() == path);
    CHECK(vo.paragraphs() == QStringList{QStringLiteral("First from the file."), QStringLiteral("Second from the file.")});
    CHECK(vo.paragraphIndex() == 0);

    // Remove: gone, and gone for the project too.
    vo.clearScript();
    CHECK(vo.paragraphs().isEmpty());
    CHECK_FALSE(vo.prompterVisible());
    {
        VoiceOverBridge again;
        again.setController(&r.controller);
        CHECK(again.paragraphs().isEmpty());
    }
}

TEST_CASE("Script prompter: a marker per paragraph read while recording, named after its first words, one undo step each",
          "[app][voiceover][prompter]") {
    resetSettings();
    VoiceRig r;
    VoiceOverBridge& vo = *r.voiceOver;
    REQUIRE(vo.setScriptText(QString::fromUtf8(kScript)).isEmpty());
    vo.setMarkerPerParagraph(true);
    REQUIRE(vo.setRecordArm(trackKey(r.tl.track("A1")), true).isEmpty());
    const std::size_t markersBefore = r.markers().size();

    const bool started = vo.start();
    INFO(vo.lastError().toStdString());
    REQUIRE(started);
    for (int i = 0; i < 100 && vo.state() != QStringLiteral("recording"); ++i)
        r.step();
    REQUIRE(vo.state() == QStringLiteral("recording"));
    CHECK(vo.prompterVisible());
    // The paragraph being read when the take starts.
    REQUIRE(r.markers().size() == markersBefore + 1);
    CHECK(r.markers().back().name == "The first paragraph of the narration\xE2\x80\xA6"); // six words, then "…"
    CHECK(r.markers().back().comment == "The first paragraph of the narration goes on a second line.");

    // Moving on drops the next one at the playhead; going back does not.
    for (int i = 0; i < 10; ++i)
        r.step();
    const std::size_t undo = r.controller.session().undoCount();
    REQUIRE(vo.nextParagraph());
    CHECK(r.controller.session().undoCount() == undo + 1);
    REQUIRE(r.markers().size() == markersBefore + 2);
    CHECK(r.markers().back().name == "Second paragraph, a list item read\xE2\x80\xA6");
    CHECK(vo.previousParagraph());
    CHECK(r.markers().size() == markersBefore + 2);
    // The direct form, at the playhead: the last paragraph's name is cut at 40 characters.
    vo.setParagraphIndex(2);
    vo.markParagraph();
    REQUIRE(r.markers().size() == markersBefore + 3);
    CHECK(r.markers().back().name.size() <= std::string("Third paragraph straight after a heading").size() + 3);
    CHECK(r.markers().back().name.starts_with("Third paragraph straight after a"));
    // Undo takes the last one away.
    REQUIRE(r.controller.undo());
    CHECK(r.markers().size() == markersBefore + 2);

    vo.cancel();
    CHECK_FALSE(vo.recording());
    // The place reached survives the take.
    CHECK(vo.paragraphIndex() == 2);
}

// Fix round 1 (ARGON-26): a pass that starts while a text field has the keyboard takes it, so Space, Esc and the clicker's
// arrows drive the pass instead of typing; the field gets it back when the pass ends.
TEST_CASE("Script prompter: a voice-over pass takes the keyboard from a text field and gives it back",
          "[app][voiceover][prompter][qml]") {
    resetSettings();
    clearMessages();
    {
        Shell shell;
        REQUIRE(shell.window);
        QQuickItem* well = findNamed(shell.window->contentItem(), QStringLiteral("programMonitorWell"));
        REQUIRE(well);
        QQuickItem* overlay = nullptr;
        for (QQuickItem* c : well->childItems()) {
            if (c->objectName() == QStringLiteral("voiceOverOverlay"))
                overlay = c;
        }
        REQUIRE(overlay);
        QQmlComponent component(&shell.engine);
        component.setData("import QtQuick\nTextInput { width: 200; height: 30; text: \"Script\" }\n",
                          QUrl(QStringLiteral("qrc:/f1d/Field.qml")));
        std::unique_ptr<QObject> object(component.create());
        INFO(component.errorString().toStdString());
        REQUIRE(object);
        auto* field = qobject_cast<QQuickItem*>(object.get());
        REQUIRE(field);
        field->setParentItem(shell.window->contentItem());
        shell.window->requestActivate();
        field->forceActiveFocus();
        spin(50);
        REQUIRE(field->hasActiveFocus());
        REQUIRE(QMetaObject::invokeMethod(overlay, "keysForPass", Q_ARG(bool, true)));
        CHECK_FALSE(field->hasActiveFocus());
        CHECK(overlay->hasActiveFocus());
        REQUIRE(QMetaObject::invokeMethod(overlay, "keysForPass", Q_ARG(bool, false)));
        CHECK(field->hasActiveFocus());
        // Without a text field in the way, a pass leaves the focus where it is.
        QQmlComponent plainComponent(&shell.engine);
        plainComponent.setData("import QtQuick\nItem { width: 20; height: 20 }\n", QUrl(QStringLiteral("qrc:/f1d/Plain.qml")));
        std::unique_ptr<QObject> plainObject(plainComponent.create());
        auto* plain = qobject_cast<QQuickItem*>(plainObject.get());
        REQUIRE(plain);
        plain->setParentItem(shell.window->contentItem());
        plain->forceActiveFocus();
        REQUIRE(plain->hasActiveFocus());
        REQUIRE(QMetaObject::invokeMethod(overlay, "keysForPass", Q_ARG(bool, true)));
        CHECK(plain->hasActiveFocus());
        REQUIRE(QMetaObject::invokeMethod(overlay, "keysForPass", Q_ARG(bool, false)));
        CHECK(plain->hasActiveFocus());
        plain->setParentItem(nullptr);
        field->setParentItem(nullptr);
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Script prompter in the shell: the overlay band while armed, Settings Audio's section; screenshots",
          "[app][voiceover][prompter][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* vo = shell.engine.singletonInstance<VoiceOverBridge*>("Wabcut", "VoiceOver");
    REQUIRE(controller);
    REQUIRE(vo);
    Timeline tl{kVoicePicture};
    controller->resetProject(tl.state, tl.seq);
    spin(200);

    QQuickItem* well = findNamed(shell.window->contentItem(), QStringLiteral("programMonitorWell"));
    REQUIRE(well);
    QQuickItem* overlay = nullptr;
    for (QQuickItem* c : well->childItems()) {
        if (c->objectName() == QStringLiteral("voiceOverOverlay"))
            overlay = c;
    }
    REQUIRE(overlay);
    CHECK_FALSE(overlay->isVisible());

    // A script and an armed track: the band shows the paragraph and the next one dimmed.
    REQUIRE(vo->setScriptText(QString::fromUtf8(kScript)).isEmpty());
    vo->setPrompterTextSize(36);
    REQUIRE(vo->setRecordArm(trackKey(tl.track("A1")), true).isEmpty());
    spin(200);
    CHECK(overlay->isVisible());
    QQuickItem* band = findNamed(overlay, QStringLiteral("voiceOverPrompter"));
    QQuickItem* current = findNamed(overlay, QStringLiteral("voiceOverPrompterText"));
    QQuickItem* next = findNamed(overlay, QStringLiteral("voiceOverPrompterNextText"));
    QQuickItem* countText = findNamed(overlay, QStringLiteral("voiceOverPrompterCount"));
    REQUIRE(band);
    REQUIRE(current);
    REQUIRE(next);
    REQUIRE(countText);
    CHECK(band->isVisible());
    CHECK(current->property("text").toString() == vo->paragraphs().at(0));
    CHECK(next->property("text").toString() == vo->paragraphs().at(1));
    CHECK(countText->property("text").toString().contains(QStringLiteral("1 of 3")));
    CHECK_FALSE(findNamed(overlay, QStringLiteral("voiceOverBadge"))->isVisible()); // no pass: no recording badge
    // Its arrows step through the script any time.
    REQUIRE(QMetaObject::invokeMethod(findNamed(overlay, QStringLiteral("voiceOverPrompterNext")), "clicked"));
    CHECK(vo->paragraphIndex() == 1);
    CHECK(current->property("text").toString() == vo->paragraphs().at(1));
    spin(150);
    {
        const QImage image = shell.grab();
        saveItemShot(image, well, QStringLiteral("voice-over-prompter.png"));
    }
    // Show when armed off: hidden until a pass runs.
    vo->setShowScript(false);
    spin(50);
    CHECK_FALSE(overlay->isVisible());
    vo->setShowScript(true);

    // Settings > Audio's section reflects the bridge and sets it.
    const auto makeDialog = [&]() {
        QQmlComponent component(&shell.engine);
        component.setData("import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: settings\n"
                          "    AudioSettings {\n        id: settings\n        refreshOnOpen: false\n        devices: []\n    }\n}\n",
                          QUrl(QStringLiteral("qrc:/p1-2e/Host.qml")));
        QObject* object = component.create();
        INFO(component.errorString().toStdString());
        REQUIRE(object);
        auto* item = qobject_cast<QQuickItem*>(object);
        REQUIRE(item);
        item->setParentItem(shell.window->contentItem());
        item->setSize(shell.window->contentItem()->size());
        return std::unique_ptr<QQuickItem>(item);
    };
    const auto host = makeDialog();
    auto* dialog = host->property("dialog").value<QObject*>();
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    QObject* section = child(dialog, "scriptPrompterSection");
    CHECK(child(section, "scriptSummary")->property("text").toString().contains(QStringLiteral("Pasted text")));
    CHECK(child(section, "scriptSummary")->property("text").toString().contains(QStringLiteral("3 paragraphs")));
    QObject* mirror = child(section, "prompterMirrorToggle");
    CHECK_FALSE(mirror->property("checked").toBool());
    mirror->setProperty("checked", true);
    REQUIRE(QMetaObject::invokeMethod(mirror, "toggled"));
    CHECK(vo->mirror());
    REQUIRE(QMetaObject::invokeMethod(child(section, "prompterSizeField"), "valueModified", Q_ARG(double, 40.0)));
    CHECK(vo->prompterTextSize() == 40);
    QObject* markerToggle = child(section, "prompterMarkerToggle");
    markerToggle->setProperty("checked", true);
    REQUIRE(QMetaObject::invokeMethod(markerToggle, "toggled"));
    CHECK(vo->markerPerParagraph());
    // Paste: the area opens with the script, and new text replaces it.
    REQUIRE(QMetaObject::invokeMethod(section, "openPaste"));
    QObject* pasteText = child(section, "scriptPasteText");
    pasteText->setProperty("text", QStringLiteral("New one.\n\nNew two."));
    REQUIRE(QMetaObject::invokeMethod(section, "usePasted"));
    CHECK(vo->paragraphs() == QStringList{QStringLiteral("New one."), QStringLiteral("New two.")});
    CHECK_FALSE(section->property("pasting").toBool());
    CHECK(section->property("error").toString().isEmpty());
    // The section's preview is mirrored now (the overlay's band too).
    CHECK(child(section, "prompterPreview")->property("visible").toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Script prompter section in Settings Audio, light theme; screenshot", "[app][voiceover][prompter][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(false, "ultraviolet");
    auto* vo = shell.engine.singletonInstance<VoiceOverBridge*>("Wabcut", "VoiceOver");
    REQUIRE(vo);
    REQUIRE(vo->setScriptText(QString::fromUtf8(kScript)).isEmpty());
    QQmlComponent component(&shell.engine);
    component.setData("import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: settings\n"
                      "    AudioSettings {\n        id: settings\n        refreshOnOpen: false\n        devices: []\n    }\n}\n",
                      QUrl(QStringLiteral("qrc:/p1-2e/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* host = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(host);
    host->setParentItem(shell.window->contentItem());
    host->setSize(shell.window->contentItem()->size());
    auto* dialog = host->property("dialog").value<QObject*>();
    REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    auto* section = qobject_cast<QQuickItem*>(child(dialog, "scriptPrompterSection"));
    REQUIRE(section);
    // Scroll the section into the dialog's view when the dialog scrolls (the screenshot is of the section itself).
    spin(300);
    const QImage image = shell.grab();
    saveItemShot(image, section, QStringLiteral("settings-audio-prompter-light.png"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- #54 ----------------------------------------------------------------------------------------------------------

TEST_CASE("Loudness QC: the mix analysis says where, and Add markers for overs is one undo step", "[app][audio][qc]") {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    AudioMixerBridge mixer;
    mixer.setController(&controller);
    mixer.setServices(whereServices());
    mixer.setSynchronous(true);

    CHECK_FALSE(mixer.addOverMarkers().isEmpty()); // nothing analyzed yet
    REQUIRE(mixer.analyze(QStringLiteral("mix")).isEmpty());
    const QVariantMap l = mixer.loudness();
    REQUIRE(l.value(QStringLiteral("valid")).toBool());
    CHECK(l.value(QStringLiteral("momentaryAt")).toString() == controller.formatTimecode(ms(2000)));
    CHECK(l.value(QStringLiteral("shortTermAt")).toString() == controller.formatTimecode(ms(1000)));
    CHECK(l.value(QStringLiteral("truePeakAt")).toString() == controller.formatTimecode(ms(2300)));
    CHECK(l.value(QStringLiteral("overCount")).toInt() == 2); // above the -1 dBTP ceiling
    const QVariantList overs = l.value(QStringLiteral("overs")).toList();
    REQUIRE(overs.size() == 2);
    CHECK(overs[0].toMap().value(QStringLiteral("start")).toString() == controller.formatTimecode(ms(2200)));
    CHECK(overs[0].toMap().value(QStringLiteral("end")).toString() == controller.formatTimecode(ms(2500)));
    CHECK(overs[0].toMap().value(QStringLiteral("truePeak")).toDouble() == 0.4);
    // The report rows carry them too.
    REQUIRE(mixer.report().size() == 1);
    CHECK(mixer.report()[0].toMap().value(QStringLiteral("overCount")).toInt() == 2);

    // Markers for the overs: red range markers named with their peak, one undo step.
    const std::size_t undo = controller.session().undoCount();
    const std::size_t before = controller.currentSequence()->markers.size();
    CHECK(mixer.addOverMarkers().isEmpty());
    CHECK(controller.session().undoCount() == undo + 1);
    CHECK(controller.undoText() == QStringLiteral("Undo Add True-Peak Over Markers"));
    const std::vector<Marker>& markers = controller.currentSequence()->markers;
    REQUIRE(markers.size() == before + 2);
    const auto over = std::find_if(markers.begin(), markers.end(), [](const Marker& m) { return m.start == ms(2200); });
    REQUIRE(over != markers.end());
    CHECK(over->duration == ms(300));
    CHECK(over->colorIndex == 1);
    CHECK(over->name.find("0.4") != std::string::npos);
    // The analysis still holds (markers do not change the sound) and is marked: not offered twice.
    CHECK_FALSE(mixer.loudness().value(QStringLiteral("stale")).toBool());
    CHECK(mixer.loudness().value(QStringLiteral("overMarkers")).toBool());
    REQUIRE(controller.undo());
    CHECK(controller.currentSequence()->markers.size() == before);
    // After an edit the analysis is stale: no markers from it.
    REQUIRE(mixer.setRole(mixer.strips().value(0).toMap().value(QStringLiteral("key")).toString(), QStringLiteral("music")).isEmpty());
    REQUIRE(mixer.loudness().value(QStringLiteral("stale")).toBool());
    CHECK_FALSE(mixer.addOverMarkers().isEmpty());
    CHECK(controller.currentSequence()->markers.size() == before);
}

TEST_CASE("Loudness QC: a very clipped programme counts its overs above the ceiling the measurement kept and says so",
          "[app][audio][qc]") {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    AudioMixerBridge mixer;
    mixer.setController(&controller);
    mixer.setServices(std::make_shared<audio::edit::AudioServices>(std::make_shared<HotFloorLoudness>(), nullptr));
    mixer.setSynchronous(true);
    REQUIRE(mixer.analyze(QStringLiteral("mix")).isEmpty());
    // The mixer's ceiling is -1 dBTP, below the kept floor (-0.5): counted above the floor, and flagged.
    const QVariantMap l = mixer.loudness();
    REQUIRE(l.value(QStringLiteral("valid")).toBool());
    CHECK(l.value(QStringLiteral("oversTruncated")).toBool());
    CHECK(l.value(QStringLiteral("oversAbove")).toDouble() == -0.5);
    CHECK(l.value(QStringLiteral("overCount")).toInt() == 2); // 5.0-5.2 s and 8.0 s
    REQUIRE(mixer.report().size() == 1);
    CHECK(mixer.report()[0].toMap().value(QStringLiteral("oversTruncated")).toBool());
    // A measurement kept down to -6 dBTP is exact: not flagged.
    mixer.setServices(whereServices());
    REQUIRE(mixer.analyze(QStringLiteral("mix")).isEmpty());
    CHECK_FALSE(mixer.loudness().value(QStringLiteral("oversTruncated")).toBool());
    CHECK(mixer.loudness().value(QStringLiteral("oversAbove")).toDouble() == -1.0);
}

TEST_CASE("Loudness QC in the Audio workspace: a very clipped programme shows its count with a plus; screenshots",
          "[app][audio][qc][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
            REQUIRE(mixer);
            mixer->setServices(std::make_shared<audio::edit::AudioServices>(std::make_shared<HotFloorLoudness>(), nullptr));
            mixer->setSynchronous(true);
            shell.window->setProperty("workspace", 1);
            spin(400);
            QQuickItem* tools = shell.item("audioToolsPanel");
            REQUIRE(tools);
            REQUIRE(mixer->analyze(QStringLiteral("tracks")).isEmpty());
            spin(200);
            QQuickItem* qc = findVisibleNamed(tools, QStringLiteral("toolsLoudnessQc"));
            REQUIRE(qc);
            QQuickItem* overCount = findNamed(qc, QStringLiteral("qcOverCount"));
            REQUIRE(overCount);
            CHECK(overCount->property("text").toString() == QStringLiteral("2 true-peak overs above -0.50 dBTP"));
            QQuickItem* reportOvers = findNamed(tools, QStringLiteral("reportOvers_0"));
            REQUIRE(reportOvers);
            CHECK(reportOvers->property("text").toString() == QStringLiteral("2+"));
            spin(150);
            const QImage image = shell.grab();
            saveItemShot(image, tools, dark ? QStringLiteral("audio-tools-loudness-qc-clipped.png")
                                            : QStringLiteral("audio-tools-loudness-qc-clipped-light.png"));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}

TEST_CASE("Loudness QC in the Audio workspace: links move the playhead, the overs stepper and markers; screenshots",
          "[app][audio][qc][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
            REQUIRE(controller);
            REQUIRE(mixer);
            mixer->setServices(whereServices());
            mixer->setSynchronous(true);
            shell.window->setProperty("workspace", 1); // Audio: Audio Tools and the mixer
            spin(400);
            QQuickItem* tools = shell.item("audioToolsPanel");
            REQUIRE(tools);
            REQUIRE(mixer->analyze(QStringLiteral("tracks")).isEmpty());
            spin(200);

            QQuickItem* qc = findVisibleNamed(tools, QStringLiteral("toolsLoudnessQc"));
            REQUIRE(qc);
            QQuickItem* overCount = findNamed(qc, QStringLiteral("qcOverCount"));
            REQUIRE(overCount);
            CHECK(overCount->property("text").toString() == QStringLiteral("2 true-peak overs above -1 dBTP"));
            // A loudest moment's link moves the playhead to it.
            QQuickItem* peakLink = findNamed(qc, QStringLiteral("qcLink_truePeak"));
            REQUIRE(peakLink);
            CHECK(peakLink->isVisible());
            CHECK(peakLink->property("timecode").toString() == controller->formatTimecode(ms(2300)));
            REQUIRE(QMetaObject::invokeMethod(peakLink, "clicked"));
            CHECK(controller->formatTimecode(controller->playhead()) == controller->formatTimecode(ms(2300)));
            // The stepper goes through the overs.
            QQuickItem* overLink = findNamed(qc, QStringLiteral("qcOverLink"));
            REQUIRE(overLink);
            CHECK(overLink->property("timecode").toString() == controller->formatTimecode(ms(2200)));
            REQUIRE(QMetaObject::invokeMethod(findNamed(qc, QStringLiteral("qcOverNext")), "clicked"));
            CHECK(overLink->property("timecode").toString() == controller->formatTimecode(ms(5000)));
            CHECK(controller->formatTimecode(controller->playhead()) == controller->formatTimecode(ms(5000)));
            // The report's Overs column links to the first over.
            QQuickItem* reportOvers = findNamed(tools, QStringLiteral("reportOvers_0"));
            REQUIRE(reportOvers);
            CHECK(reportOvers->property("text").toString() == QStringLiteral("2"));
            // The master strip shows the same block.
            QQuickItem* master = findNamed(shell.window->contentItem(), QStringLiteral("masterStrip"));
            REQUIRE(master);
            CHECK(findNamed(master, QStringLiteral("loudnessQc")));
            spin(150);
            {
                const QImage image = shell.grab();
                saveItemShot(image, tools, dark ? QStringLiteral("audio-tools-loudness-qc.png")
                                                : QStringLiteral("audio-tools-loudness-qc-light.png"));
            }
            // Add markers for overs: one undo step, then the button says so and is off.
            QQuickItem* add = findNamed(qc, QStringLiteral("qcAddOverMarkers"));
            REQUIRE(add);
            CHECK(add->property("enabled").toBool());
            const std::size_t undo = controller->session().undoCount();
            REQUIRE(QMetaObject::invokeMethod(add, "clicked"));
            CHECK(controller->session().undoCount() == undo + 1);
            CHECK(controller->currentSequence()->markers.size() >= 2);
            spin(50);
            CHECK_FALSE(add->property("enabled").toBool());
            CHECK(add->property("text").toString() == QStringLiteral("Markers added"));
            REQUIRE(controller->undo());
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}
