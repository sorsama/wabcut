// P0 round (stream D):
//   * OBS chapter markers on import, through the app: ProjectFile.importFiles turns a recording's chapters into clip
//     markers in the one Import step and its summary counts them (the toast's sentence); Preferences > Media's switch
//     (Editing.chapterMarkersOnImport, on by default) turns it off.
//   * The source clip transcript: the Transcript panel's Source mode (SourceTranscriptBridge) reads the clip in the
//     Source Monitor - per media item and audio stream - a click moves the Source Monitor there, a selection sets
//     frame-snapped source marks in one undo step (SourceMonitorBridge::setMarks), Insert / Overwrite edit that
//     soundbite in, Copy copies it, and Transcribe this clip runs the job for that media alone. Then the panel's
//     Sequence | Source switch in the shell, with dark and light screenshots. Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "EditingBridge.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SourceMonitorBridge.h"
#include "SourceTranscriptBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/editor/MediaImport.h>

#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

QString testMediaFile(const char* name) {
    return QDir(QStringLiteral(WABCUT_TEST_MEDIA_DIR)).filePath(QString::fromLatin1(name));
}

bool haveChapterMedia() { return QFile::exists(testMediaFile("obs_chapters.mkv")); }

/// "00:00:01:04" at the fixtures' 25 fps.
QString tc(int frames) {
    return QStringLiteral("00:00:%1:%2").arg(frames / 25, 2, 10, QLatin1Char('0')).arg(frames % 25, 2, 10, QLatin1Char('0'));
}

/// A word in source frames of the fixtures' 25 fps (fractions allowed).
struct W {
    std::string text;
    double from = 0;
    double to = 0;
    std::optional<int> speaker = std::nullopt;
};

std::shared_ptr<const ai::Transcript> words(std::vector<W> list, std::string language = "en") {
    auto t = std::make_shared<ai::Transcript>();
    t->language = std::move(language);
    const double frame = static_cast<double>(f(1).value);
    for (const W& w : list) {
        ai::TranscriptWord tw;
        tw.text = w.text;
        tw.t0 = Ticks{static_cast<std::int64_t>(w.from * frame)};
        tw.t1 = Ticks{static_cast<std::int64_t>(w.to * frame)};
        tw.confidence = 0.9f;
        tw.speakerId = w.speaker;
        t->words.push_back(std::move(tw));
    }
    return t;
}

/// What S (a 50-frame source with video and two audio streams) says on its first stream; the words start between frames.
std::shared_ptr<const ai::Transcript> interview() {
    return words({{"We", 2.4, 4.0, 0},
                  {"shipped", 4.2, 8.6, 0},
                  {"the", 8.6, 9.2, 0},
                  {"build", 9.4, 13.5, 0},
                  {"tonight.", 13.6, 18.2, 0},
                  {"Ship", 30.0, 33.0, 1},
                  {"it.", 33.0, 36.0, 1}});
}

// 12 frames of A on V1/A1 (linked), empty V2 and A2; S is the source.
constexpr const char* kPicture = R"(
    V2     |............|
    V1 tgt |Aaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaa|
    A2     |............|
)";

struct Rig {
    Timeline tl{kPicture, {{'S', 50}}};
    ProjectController controller;
    SourceMonitorBridge source;
    TranscriptBridge transcript;
    SourceTranscriptBridge st;

    explicit Rig(std::function<void(ProjectState&)> adjust = {}) {
        tl.linkMatching();
        ProjectState state = tl.state;
        if (adjust)
            adjust(state);
        controller.resetProject(state, tl.seq);
        source.setController(&controller);
        transcript.setUseAnalysisCache(false);
        transcript.setController(&controller);
        st.setMarksDelay(0);
        st.setController(&controller);
        st.setTranscriptBridge(&transcript);
        st.setSourceMonitor(&source);
    }
    const MediaItem& item(char letter) const { return *controller.snapshot()->findMedia(tl.media(letter)); }
    std::size_t undo() { return controller.session().undoCount(); }
    std::string picture() const { return wabcut::test::render(*controller.snapshot(), tl.seq, kPicture); }
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

QQuickItem* visibleNamed(Shell& shell, const QString& name) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->isVisible() && i->objectName() == name; });
}

void click(QQuickWindow* window, QQuickItem* item, Qt::KeyboardModifiers mods = {}) {
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, mods);
    QCoreApplication::sendEvent(window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::NoButton, mods);
    QCoreApplication::sendEvent(window, &release);
}

/// The project's one media item (a new project after one import), or null.
const MediaItem* onlyMedia(const ProjectController& c) {
    const ProjectSnapshot snap = c.snapshot();
    return snap->media.size() == 1 ? snap->media.front().get() : nullptr;
}

MediaId mediaNamed(const ProjectController& c, const std::string& name) {
    for (const MediaPtr& m : c.snapshot()->media) {
        if (m->name == name)
            return m->id;
    }
    return {};
}

} // namespace

// ---- OBS chapter markers on import --------------------------------------------------------------------------------

TEST_CASE("Import summary: a recording's chapter markers become clip markers in the Import step and are counted",
          "[app][import][chapters]") {
    if (!haveChapterMedia())
        SKIP("test media not generated");
    resetSettings();
    EditingBridge editing;
    CHECK(editing.chapterMarkersOnImport()); // on by default
    {
        ProjectController controller;
        ProjectLifecycle lifecycle{&controller};
        lifecycle.newProject();
        const std::size_t before = controller.session().undoCount();
        const QVariantMap result = lifecycle.importFiles({QUrl::fromLocalFile(testMediaFile("obs_chapters.mkv"))});
        CHECK(result.value("added").toInt() == 1);
        CHECK(result.value("chapterMarkers").toInt() == 4);
        CHECK(result.value("chapterSummary").toString() == QStringLiteral("4 chapter markers became clip markers."));
        CHECK(result.value("folderSummary").toString().isEmpty());
        CHECK(controller.session().undoCount() == before + 1);
        CHECK(controller.session().undoLabel() == "Import");
        const MediaItem* item = controller.snapshot()->findMedia(mediaNamed(controller, "obs_chapters.mkv"));
        REQUIRE(item != nullptr);
        REQUIRE(item->markers.size() == 4);
        CHECK(item->markers.front().name == "Boss fight");
        CHECK(item->markers.front().colorIndex == editor::kChapterMarkerColor);
        // Undo takes the item and its markers away together.
        controller.undo();
        CHECK(mediaNamed(controller, "obs_chapters.mkv") == MediaId{});
    }
    {
        // Preferences > Media: off, the same recording comes in without markers and the summary says nothing.
        editing.setChapterMarkersOnImport(false);
        CHECK_FALSE(EditingBridge::chapterMarkersSetting());
        ProjectController controller;
        ProjectLifecycle lifecycle{&controller};
        lifecycle.newProject();
        const QVariantMap result = lifecycle.importFiles({QUrl::fromLocalFile(testMediaFile("obs_chapters.mkv"))});
        CHECK(result.value("added").toInt() == 1);
        CHECK(result.value("chapterMarkers").toInt() == 0);
        CHECK(result.value("chapterSummary").toString().isEmpty());
        const MediaItem* item = controller.snapshot()->findMedia(mediaNamed(controller, "obs_chapters.mkv"));
        REQUIRE(item != nullptr);
        CHECK(item->markers.empty());
        editing.setChapterMarkersOnImport(true);
        CHECK(EditingBridge::chapterMarkersSetting());
    }
    {
        // Import Folder: the folder's toast ends with the chapter sentence.
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const QString folder = dir.filePath(QStringLiteral("Stream VODs"));
        REQUIRE(QDir().mkpath(folder));
        REQUIRE(QFile::copy(testMediaFile("obs_chapters.mkv"), folder + QStringLiteral("/vod.mkv")));
        ProjectController controller;
        ProjectLifecycle lifecycle{&controller};
        lifecycle.newProject();
        const QVariantMap result = lifecycle.importFiles({QUrl::fromLocalFile(folder)});
        CHECK(result.value("chapterMarkers").toInt() == 4);
        CHECK(result.value("folderSummary").toString().endsWith(QStringLiteral("4 chapter markers became clip markers.")));
    }
    resetSettings();
}

TEST_CASE("Media Browser imports and timeline drops bring the chapter markers too; the toast counts them",
          "[app][import][chapters][media-browser]") {
    if (!haveChapterMedia())
        SKIP("test media not generated");
    resetSettings();
    const QString file = testMediaFile("obs_chapters.mkv");
    {
        // The Media Browser (Library.importFromBrowser, media.importCameraClips): markers and the toast's sentence.
        ProjectController controller;
        ProjectLifecycle lifecycle{&controller};
        lifecycle.newProject();
        LibraryBridge library;
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setController(&controller);
        const std::size_t before = controller.session().undoCount();
        QStringList notices;
        QObject::connect(&library, &LibraryBridge::notice, [&](const QString& text) { notices << text; });
        REQUIRE_FALSE(library.importFromBrowser({file}).isEmpty());
        CHECK(controller.session().undoCount() == before + 1);
        // Fix round 1: a notice for the toast; Library.message (the Media Browser's empty / unreadable text) stays empty.
        CHECK(notices == QStringList{QStringLiteral("4 chapter markers became clip markers.")});
        CHECK(library.message().isEmpty());
        const MediaItem* item = onlyMedia(controller);
        REQUIRE(item != nullptr);
        REQUIRE(item->markers.size() == 4);
        CHECK(item->markers.front().colorIndex == editor::kChapterMarkerColor);
    }
    {
        // Preferences > Media off: the Media Browser brings none either.
        EditingBridge editing;
        editing.setChapterMarkersOnImport(false);
        ProjectController controller;
        ProjectLifecycle lifecycle{&controller};
        lifecycle.newProject();
        LibraryBridge library;
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setController(&controller);
        QStringList notices;
        QObject::connect(&library, &LibraryBridge::notice, [&](const QString& text) { notices << text; });
        REQUIRE_FALSE(library.importFromBrowser({file}).isEmpty());
        CHECK(library.message().isEmpty());
        CHECK(notices.isEmpty());
        const MediaItem* item = onlyMedia(controller);
        REQUIRE(item != nullptr);
        CHECK(item->markers.empty());
        editing.setChapterMarkersOnImport(true);
    }
    {
        // Files dropped on the timeline (ProjectFile.dropFiles) import through the same path: markers and the summary.
        ProjectController controller;
        ProjectLifecycle lifecycle{&controller};
        lifecycle.newProject();
        const QVariantMap result = lifecycle.dropFiles({QUrl::fromLocalFile(file)}, QStringLiteral("00:00:00:00"), QString(), true);
        CHECK(result.value("chapterMarkers").toInt() == 4);
        CHECK(result.value("chapterSummary").toString() == QStringLiteral("4 chapter markers became clip markers."));
        const MediaItem* item = onlyMedia(controller);
        REQUIRE(item != nullptr);
        CHECK(item->markers.size() == 4);
    }
    resetSettings();
}

TEST_CASE("Editing preferences: the chapter markers switch shows the preference and changes it", "[app][import][chapters][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* editing = shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
    REQUIRE(editing != nullptr);
    REQUIRE(editing->runCommand(QStringLiteral("edit.preferencesMedia")));
    QObject* prefs = nullptr;
    REQUIRE(waitFor([&] {
        prefs = shell.window->findChild<QObject*>(QStringLiteral("editingPreferencesDialog"));
        return prefs && prefs->property("opened").toBool();
    }, 5000));
    QQuickItem* toggle = nullptr;
    REQUIRE(waitFor([&] { return (toggle = visibleNamed(shell, QStringLiteral("chapterMarkersToggle"))) != nullptr; }));
    CHECK(toggle->property("checked").toBool());
    spin(200);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/preferences-media-chapter-markers.png")));
    click(shell.window.get(), toggle);
    REQUIRE(waitFor([&] { return !editing->chapterMarkersOnImport(); }));
    CHECK_FALSE(toggle->property("checked").toBool());
    // The bridge leads: the switch follows it.
    editing->setChapterMarkersOnImport(true);
    CHECK(waitFor([&] { return toggle->property("checked").toBool(); }));
    QMetaObject::invokeMethod(prefs, "close");
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

// ---- SourceMonitorBridge::setMarks -------------------------------------------------------------------------------

TEST_CASE("Source marks from a time range: frame-snapped outward, clamped, one undo step, none when unchanged",
          "[app][source][transcript]") {
    Rig r;
    CHECK_FALSE(r.source.setMarks(f(1), f(2))); // nothing loaded
    REQUIRE(r.source.load(mediaKey(r.tl.media('S'))));
    const std::size_t before = r.undo();
    // [10.4, 14.6) frames: In at the start of frame 10, Out at the end of frame 14.
    const Ticks frame = f(1);
    REQUIRE(r.source.setMarks(f(10) + Ticks{frame.value * 4 / 10}, f(14) + Ticks{frame.value * 6 / 10}));
    CHECK(r.item('S').markIn == f(10));
    CHECK(r.item('S').markOut == f(15));
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Set Source Marks"));
    // The same range again: no new step.
    REQUIRE(r.source.setMarks(f(10), f(15)));
    CHECK(r.undo() == before + 1);
    // Past the end: clamped to the media; an empty range is refused.
    REQUIRE(r.source.setMarks(f(45), f(80)));
    CHECK(r.item('S').markOut == f(50));
    CHECK_FALSE(r.source.setMarks(f(20), f(20)));
    CHECK_FALSE(r.source.message().isEmpty());
    r.controller.undo();
    CHECK(r.item('S').markIn == f(10));
}

// ---- The source clip transcript ----------------------------------------------------------------------------------

TEST_CASE("Source transcript: the Source Monitor clip's words, a click moves the source, a selection marks it",
          "[app][source][transcript]") {
    Rig r;
    CHECK(r.st.state() == QStringLiteral("none"));
    r.transcript.addTranscript(r.tl.media('S'), 0, interview());
    REQUIRE(r.source.load(mediaKey(r.tl.media('S'))));
    CHECK(r.st.state() == QStringLiteral("ready"));
    CHECK(r.st.mediaName() == QStringLiteral("S"));
    CHECK(r.st.stream() == 0); // the stream with a transcript
    REQUIRE(r.st.wordCount() == 7);
    CHECK(r.st.language() == QStringLiteral("en"));
    // Paragraphs at the speaker change, with source timecodes.
    REQUIRE(r.st.paragraphs()->rowCount() == 2);
    CHECK(r.st.paragraphs()->get(0).value(QStringLiteral("timeText")).toString() == tc(2));
    CHECK(r.st.paragraphs()->get(1).value(QStringLiteral("speaker")).toString() == QStringLiteral("Speaker 2"));
    // The sequence's transcript is untouched (S is not in the sequence).
    CHECK(r.transcript.wordCount() == 0);

    // A click: the Source Monitor goes to the word (on a frame inside it) and the word is marked In to Out.
    const std::size_t before = r.undo();
    r.st.clickWord(1, 0); // "shipped" [4.2, 8.6)
    CHECK(r.source.positionTicks() == f(5));
    CHECK(r.st.currentWord() == 1);
    CHECK(r.st.isSelected(1));
    CHECK(r.item('S').markIn == f(4));
    CHECK(r.item('S').markOut == f(9));
    CHECK(r.undo() == before + 1);
    // Shift-click extends the run: In at the first word's frame, Out at the end of the last word's frame.
    r.st.clickWord(4, Qt::ShiftModifier); // "tonight." ends at 18.2
    CHECK(r.st.isSelected(2));
    CHECK(r.st.selectionInfo().startsWith(QStringLiteral("4 word")));
    CHECK(r.item('S').markIn == f(4));
    CHECK(r.item('S').markOut == f(19));
    CHECK(r.undo() == before + 2);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Set Source Marks"));
    // Undo goes back to the previous marks; the words stay selected (they read the same).
    r.controller.undo();
    CHECK(r.item('S').markOut == f(9));
    CHECK(r.st.hasSelection());

    // A drag sets the marks once, when it ends.
    r.st.setMarksDelay(10000);
    const std::size_t dragged = r.undo();
    r.st.selectWords(5, 5);
    r.st.selectWords(5, 6);
    CHECK(r.undo() == dragged); // still waiting
    CHECK(r.st.applyMarks().isEmpty());
    CHECK(r.undo() == dragged + 1);
    CHECK(r.item('S').markIn == f(30));
    CHECK(r.item('S').markOut == f(36));
    CHECK(r.st.applyMarks().isEmpty()); // nothing waits now
    CHECK(r.undo() == dragged + 1);

    // Copy: the words as text.
    REQUIRE(r.st.copySelection(false).isEmpty());
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("Ship it."));
    REQUIRE(r.st.copySelection(true).isEmpty());
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("[") + tc(30) + QStringLiteral("] Speaker 2: Ship it."));

    // Find: a match becomes the selection and the marks.
    r.st.setMarksDelay(0);
    r.st.setSearchQuery(QStringLiteral("the build"));
    CHECK(r.st.matchCount() == 1);
    r.st.nextMatch(1);
    CHECK(r.st.selectionFirst() == 2);
    CHECK(r.item('S').markIn == f(8));
    CHECK(r.item('S').markOut == f(14));

    // Another clip in the Source Monitor: its words (none), the selection goes.
    REQUIRE(r.source.load(mediaKey(r.tl.media('A'))));
    CHECK(r.st.mediaName() == QStringLiteral("A"));
    CHECK(r.st.wordCount() == 0);
    CHECK_FALSE(r.st.hasSelection());
    r.source.unload();
    CHECK(r.st.state() == QStringLiteral("none"));
}

TEST_CASE("Source transcript: Insert and Overwrite edit the selected soundbite into the sequence", "[app][source][transcript]") {
    Rig r;
    r.transcript.addTranscript(r.tl.media('S'), 0, interview());
    REQUIRE(r.source.load(mediaKey(r.tl.media('S'))));
    r.st.clickWord(1, 0);
    r.st.clickWord(3, Qt::ShiftModifier); // "shipped the build": frames 4..13
    REQUIRE(r.item('S').markIn == f(4));
    REQUIRE(r.item('S').markOut == f(14));
    r.controller.setPlayhead(f(2));
    CHECK(r.source.edit(true).isEmpty()); // Insert (,)
    CHECK(r.picture() == normalize("V2 |......................|\nV1 |AaS4ssssssssA2aaaaaaaa|\nA1 |AaS4ssssssssA2aaaaaaaa|\nA2 |..S4ssssssss..........|"));
    r.controller.undo();
    r.controller.setPlayhead(f(2));
    CHECK(r.source.edit(false).isEmpty()); // Overwrite (.)
    CHECK(r.picture() == normalize("V2 |............|\nV1 |AaS4ssssssss|\nA1 |AaS4ssssssss|\nA2 |..S4ssssssss|"));
}

TEST_CASE("Source transcript: per audio stream, remembered per clip; Transcribe this clip runs that media alone",
          "[app][source][transcript]") {
    // S needs a file for a job.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("s.mkv"));
    {
        std::ofstream(file.toStdString()) << "not really a recording";
    }
    Rig r([&](ProjectState& state) {
        for (MediaPtr& m : state.media) {
            if (m->name == "S") {
                auto copy = std::make_shared<MediaItem>(*m);
                copy->locator.path = file.toStdString();
                m = copy;
            }
        }
    });
    // Only the second stream (S's mic) has words.
    r.transcript.addTranscript(r.tl.media('S'), 1, words({{"Mic", 1, 4}, {"check.", 5, 9}}));
    REQUIRE(r.source.load(mediaKey(r.tl.media('S'))));
    CHECK(r.st.stream() == 1);
    REQUIRE(r.st.streams().size() == 2);
    CHECK(r.st.streams()[1].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("A2 · Mic"));
    CHECK(r.st.streams()[1].toMap().value(QStringLiteral("hasTranscript")).toBool());
    CHECK_FALSE(r.st.streams()[0].toMap().value(QStringLiteral("hasTranscript")).toBool());
    CHECK(r.st.wordCount() == 2);
    r.st.setStream(0);
    CHECK(r.st.stream() == 0);
    CHECK(r.st.wordCount() == 0);
    // The choice is kept for that clip.
    REQUIRE(r.source.load(mediaKey(r.tl.media('A'))));
    REQUIRE(r.source.load(mediaKey(r.tl.media('S'))));
    CHECK(r.st.stream() == 0);

    // Transcribe this clip: the job transcribes S's first stream only (not the sequence's media).
    std::atomic<int> calls = 0;
    std::atomic<int> streamSeen = -1;
    std::atomic<bool> rightFile = false;
    const std::string expectedPath = file.toStdString();
    r.transcript.setTranscriber(QStringLiteral("fake"), QStringLiteral("Fake"),
                                [&](const std::string& path, int containerStream, const std::function<void(float)>& progress,
                                    std::stop_token) {
                                    // A worker thread: no Catch2 assertions here.
                                    ++calls;
                                    streamSeen = containerStream;
                                    rightFile = path == expectedPath;
                                    progress(0.5f);
                                    return std::expected<ai::Transcript, Error>(*words({{"Game", 0, 3}, {"audio.", 4, 8}}));
                                });
    r.transcript.setProvider(QStringLiteral("fake"));
    REQUIRE(r.st.transcribe(false).isEmpty());
    CHECK(r.transcript.jobStatus() == QStringLiteral("running"));
    REQUIRE(waitFor([&] { return r.transcript.jobStatus() != QStringLiteral("running"); }));
    CHECK(r.transcript.jobStatus() == QStringLiteral("done"));
    CHECK(calls == 1);
    CHECK(rightFile);
    CHECK(streamSeen == r.item('S').info.audio[0].index);
    CHECK(r.st.wordCount() == 2);
    CHECK(r.st.streams()[0].toMap().value(QStringLiteral("hasTranscript")).toBool());
    // Transcribed already: nothing more to do.
    CHECK_FALSE(r.st.transcribe(false).isEmpty());
    CHECK(calls == 1);
}

TEST_CASE("Source transcript: corrections and redactions of the clip read in the source view", "[app][source][transcript][redaction]") {
    Rig r;
    r.transcript.addTranscript(r.tl.media('S'), 0, interview());
    REQUIRE(r.source.load(mediaKey(r.tl.media('S'))));
    REQUIRE(r.st.wordCount() == 7);
    r.st.clickWord(3, 0);
    // A bleep over "build" anywhere S is used: the word reads [bleep] here too, and the selection goes.
    command::CommandEngine engine(r.controller.session(), ai::edit::editingCommands(),
                                  command::EnginePolicy{command::CapabilitySet::all(), 1024, r.transcript.services()});
    command::CommandRequest add;
    add.command = "redaction.add";
    add.params = command::Json{{"media", r.tl.media('S').value},
                               {"source", command::Json{{"start", f(0).value}, {"end", f(50).value}}},
                               {"audio", "bleep"},
                               {"words", command::Json::array({command::Json{{"start", f(9).value}, {"end", f(14).value}}})}};
    const auto added = engine.execute(add);
    INFO(command::toJson(added).dump());
    REQUIRE(added.ok());
    REQUIRE(waitFor([&] { return std::string(r.st.mediaTranscript().text(3)) == "[bleep]"; }));
    CHECK_FALSE(r.st.hasSelection());
}

TEST_CASE("Transcript panel Source mode in the shell: the switch, a click marks the source, Insert edits it; screenshots",
          "[app][source][transcript][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    auto* st = shell.engine.singletonInstance<SourceTranscriptBridge*>("Wabcut", "SourceTranscript");
    REQUIRE(controller != nullptr);
    REQUIRE(source != nullptr);
    REQUIRE(t != nullptr);
    REQUIRE(st != nullptr);
    t->setUseAnalysisCache(false);
    st->setMarksDelay(0);
    const MediaId interview = mediaNamed(*controller, "Interview A.mov");
    REQUIRE(interview.isValid());
    // Words in seconds of the interview.
    auto said = std::make_shared<ai::Transcript>();
    said->language = "en";
    const auto word = [&](const char* text, double from, double to, int speaker) {
        ai::TranscriptWord w;
        w.text = text;
        w.t0 = Ticks{static_cast<std::int64_t>(from * static_cast<double>(kTicksPerSecond))};
        w.t1 = Ticks{static_cast<std::int64_t>(to * static_cast<double>(kTicksPerSecond))};
        w.confidence = 0.95f;
        w.speakerId = speaker;
        said->words.push_back(w);
    };
    word("We", 0.4, 0.6, 0);         word("started", 0.7, 1.2, 0);   word("with", 1.2, 1.4, 0);
    word("one", 1.4, 1.6, 0);        word("question:", 1.6, 2.3, 0); word("what", 2.5, 2.7, 0);
    word("does", 2.7, 2.9, 0);       word("a", 2.9, 3.0, 0);         word("player", 3.0, 3.4, 0);
    word("remember", 3.4, 3.9, 0);   word("after", 3.9, 4.1, 0);     word("the", 4.1, 4.2, 0);
    word("match?", 4.2, 4.8, 0);     word("The", 6.6, 6.8, 1);       word("moment", 6.8, 7.2, 1);
    word("it", 7.2, 7.3, 1);         word("turned.", 7.3, 7.9, 1);
    t->addTranscript(interview, 0, said);
    REQUIRE(source->load(mediaKey(interview)));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull());

    // The Sequence | Source switch: Source shows the Source Monitor's clip.
    QObject* modes = shell.object("transcriptModeTabs");
    if (!modes)
        modes = visibleNamed(shell, QStringLiteral("transcriptModeTabs"));
    REQUIRE(modes != nullptr);
    REQUIRE(QMetaObject::invokeMethod(modes, "activated", Q_ARG(int, 1)));
    REQUIRE(waitFor([&] { return visibleNamed(shell, QStringLiteral("sourceTranscriptPage")) != nullptr; }));
    CHECK(visibleNamed(shell, QStringLiteral("transcriptPage")) == nullptr);
    REQUIRE(st->wordCount() == 17);
    spin(300); // the words lay out
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* name = visibleNamed(shell, QStringLiteral("sourceTranscriptClipName"));
    REQUIRE(name != nullptr);
    CHECK(name->property("text").toString() == QStringLiteral("Interview A.mov"));
    // The sequence-only actions are not here; the source ones are.
    CHECK(visibleNamed(shell, QStringLiteral("transcriptDelete")) == nullptr);
    CHECK(visibleNamed(shell, QStringLiteral("transcriptKeep")) == nullptr);

    // Click "player" with the mouse, Shift-click "match?": the source In to Out is those words.
    QQuickItem* page = visibleNamed(shell, QStringLiteral("sourceTranscriptPage"));
    REQUIRE(page != nullptr);
    const auto wordItem = [&](int index) {
        return findItem(page, [&](QQuickItem* i) {
            return i->isVisible() && i->property("wordIndex").isValid() && i->property("wordIndex").toInt() == index;
        });
    };
    QQuickItem* player = wordItem(8);
    REQUIRE(player != nullptr);
    click(shell.window.get(), player);
    spin(30);
    INFO(join(takeMessages()));
    INFO("player at " << player->mapToScene(QPointF(0, 0)).x() << "," << player->mapToScene(QPointF(0, 0)).y() << " size "
                      << player->width() << "x" << player->height());
    REQUIRE(st->isSelected(8));
    QQuickItem* match = wordItem(12);
    REQUIRE(match != nullptr);
    click(shell.window.get(), match, Qt::ShiftModifier);
    spin(50);
    REQUIRE(st->selection().has_value());
    CHECK(st->selection()->first == 8);
    CHECK(st->selection()->second == 12);
    {
        // Fix round 1: Shift pressed on "match?" and the key let go during the drag: the run 8..12 stays (no drag from
        // the clicked word).
        QQuickItem* after = wordItem(14);
        REQUIRE(after != nullptr);
        const QPointF at = match->mapToScene(QPointF(match->width() / 2, match->height() / 2));
        const QPointF to = after->mapToScene(QPointF(after->width() / 2, after->height() / 2));
        QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(shell.window.get(), &press);
        QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(shell.window.get(), &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(shell.window.get(), &release);
        spin(30);
        REQUIRE(st->selection().has_value());
        CHECK(st->selection()->first == 8);
        CHECK(st->selection()->second == 12);
    }
    const MediaItem* item = controller->snapshot()->findMedia(interview);
    REQUIRE(item != nullptr);
    REQUIRE(item->markIn.has_value());
    REQUIRE(item->markOut.has_value());
    const Rate rate = source->rate();
    CHECK(*item->markIn == snapToFrame(Ticks{static_cast<std::int64_t>(3.0 * static_cast<double>(kTicksPerSecond))}, rate));
    CHECK(*item->markOut > Ticks{static_cast<std::int64_t>(4.79 * static_cast<double>(kTicksPerSecond))});
    QQuickItem* insert = visibleNamed(shell, QStringLiteral("sourceTranscriptInsert"));
    REQUIRE(insert != nullptr);
    CHECK(visibleNamed(shell, QStringLiteral("sourceTranscriptOverwrite")) != nullptr);
    CHECK(visibleNamed(shell, QStringLiteral("sourceTranscriptCopy")) != nullptr);
    spin(200);
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/transcript-source-mode.png")));
        QQuickItem* panel = shell.item("transcriptPanel");
        REQUIRE(panel != nullptr);
        const double dpr = shell.window->effectiveDevicePixelRatio();
        const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        REQUIRE(image.copy(QRectF(rect.topLeft() * dpr, rect.size() * dpr).toRect())
                    .save(outputDir() + QStringLiteral("/transcript-source-mode-crop.png")));
    }
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/transcript-source-mode-light.png")));
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(shell.settle(true, shell.themeColor("accentFill")));

    // Insert: the soundbite goes into the sequence at the playhead (Main.qml's sourceEdit, one undo step). No sequence
    // marks, so it is a three-point edit (no Fit Clip question).
    controller->clearMarks(true, true);
    controller->setPlayhead(Ticks{});
    const std::size_t before = controller->session().undoCount();
    click(shell.window.get(), insert);
    REQUIRE(waitFor([&] { return controller->session().undoCount() == before + 1; }));
    CHECK(controller->undoText() == QStringLiteral("Undo Insert from Source"));

    // Back to Sequence: the sequence's words and actions.
    REQUIRE(QMetaObject::invokeMethod(modes, "activated", Q_ARG(int, 0)));
    REQUIRE(waitFor([&] { return visibleNamed(shell, QStringLiteral("transcriptPage")) != nullptr; }));
    CHECK(visibleNamed(shell, QStringLiteral("sourceTranscriptPage")) == nullptr);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

TEST_CASE("Transcript panel Source mode without a clip or a transcript: what to do; screenshots",
          "[app][source][transcript][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(t != nullptr);
    t->setUseAnalysisCache(false);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);
    QObject* modes = shell.object("transcriptModeTabs");
    if (!modes)
        modes = visibleNamed(shell, QStringLiteral("transcriptModeTabs"));
    REQUIRE(modes != nullptr);
    REQUIRE(QMetaObject::invokeMethod(modes, "activated", Q_ARG(int, 1)));
    QQuickItem* title = nullptr;
    REQUIRE(waitFor([&] { return (title = visibleNamed(shell, QStringLiteral("sourceTranscriptEmptyTitle"))) != nullptr; }));
    CHECK(title->property("text").toString() == QStringLiteral("No clip in the Source Monitor"));
    CHECK(visibleNamed(shell, QStringLiteral("transcribeClipButton")) == nullptr);

    // A clip without a transcript: Transcribe this clip.
    REQUIRE(source->load(mediaKey(mediaNamed(*controller, "Interview B.mov"))));
    REQUIRE(waitFor([&] { return title->property("text").toString() == QStringLiteral("No transcript of this clip yet"); }));
    const bool canStart = visibleNamed(shell, QStringLiteral("transcribeClipButton")) != nullptr
                          || visibleNamed(shell, QStringLiteral("sourceTranscriptSetupButton")) != nullptr;
    CHECK(canStart);
    spin(200);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/transcript-source-empty.png")));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/transcript-source-empty-light.png")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

TEST_CASE("Source transcript fix round 1: Insert sets the selection's marks again after the Source Monitor's changed",
          "[app][source][transcript]") {
    Rig r;
    r.transcript.addTranscript(r.tl.media('S'), 0, interview());
    REQUIRE(r.source.load(mediaKey(r.tl.media('S'))));
    r.st.clickWord(1, 0);
    r.st.clickWord(3, Qt::ShiftModifier); // "shipped the build": frames 4..13
    REQUIRE(r.item('S').markIn == f(4));
    REQUIRE(r.item('S').markOut == f(14));
    // The user clears the Source Monitor's marks (or sets others) while the words stay selected.
    r.source.clearInOut();
    REQUIRE_FALSE(r.item('S').markIn.has_value());
    CHECK(r.st.hasSelection());
    // Nothing waits: a plain apply leaves them alone.
    CHECK(r.st.applyMarks().isEmpty());
    CHECK_FALSE(r.item('S').markIn.has_value());
    // Insert / Overwrite force them: the selected words again, one undo step.
    const std::size_t before = r.undo();
    CHECK(r.st.applyMarks(true).isEmpty());
    CHECK(r.item('S').markIn == f(4));
    CHECK(r.item('S').markOut == f(14));
    CHECK(r.undo() == before + 1);
    // Forced again with the marks already those: no new step.
    CHECK(r.st.applyMarks(true).isEmpty());
    CHECK(r.undo() == before + 1);
    r.controller.setPlayhead(f(2));
    CHECK(r.source.edit(true).isEmpty());
    CHECK(r.picture() == normalize("V2 |......................|\nV1 |AaS4ssssssssA2aaaaaaaa|\nA1 |AaS4ssssssssA2aaaaaaaa|\nA2 |..S4ssssssss..........|"));
}

TEST_CASE("Source transcript fix round 1: a zero-length word is current on the frame that holds it",
          "[app][source][transcript]") {
    Rig r;
    r.transcript.addTranscript(r.tl.media('S'), 0, words({{"Go", 2, 5}, {"[beep]", 10.5, 10.5}, {"now.", 12, 16}}));
    REQUIRE(r.source.load(mediaKey(r.tl.media('S'))));
    REQUIRE(r.st.wordCount() == 3);
    r.st.clickWord(1, 0);
    CHECK(r.source.positionTicks() == f(10));
    CHECK(r.st.currentWord() == 1);
    // Its marks are one frame.
    CHECK(r.item('S').markIn == f(10));
    CHECK(r.item('S').markOut == f(11));
    r.source.seek(f(12));
    CHECK(r.st.currentWord() == 2);
}
