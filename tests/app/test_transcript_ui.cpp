// M5e: the Transcript panel — the sequence transcript as paragraphs of words, click to seek, delete / keep / move words
// through the transcript commands (dry-run preview, one undo step), the filler and silence clean-up, search, captions
// (generate, edit, SRT/VTT export) and Thai text. Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "CaptionsBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/TranscriptRedaction.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/CaptionRedaction.h>
#include <wabcut/model/ProjectState.h>

#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QDir>
#include <QDataStream>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextLayout>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

bool reviewFinished(TranscriptBridge& bridge) {
    QElapsedTimer timer;
    timer.start();
    while (bridge.reviewBusy() && timer.elapsed() < 10000) spin(10);
    return !bridge.reviewBusy();
}

struct W {
    std::string text;
    double from = 0; ///< source frames (the fixture's 25 fps, or seconds * fps of the demo)
    double to = 0;
    std::optional<int> speaker = std::nullopt;
};

std::shared_ptr<const ai::Transcript> transcript(std::string language, std::vector<W> words, Rate rate = kRate) {
    auto t = std::make_shared<ai::Transcript>();
    t->language = std::move(language);
    const double frame = static_cast<double>(framesToTicks(1, rate).value);
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

QStringList wordTexts(const TranscriptBridge& bridge) {
    QStringList out;
    const auto& st = bridge.sequenceTranscript();
    for (std::size_t i = 0; i < st.size(); ++i)
        out << QString::fromStdString(std::string(st.text(i)));
    return out;
}

Ticks clipLength(const ProjectController& c, const char* track) {
    const SequencePtr seq = c.currentSequence();
    const auto& tracks = track[0] == 'V' ? seq->videoTracks : seq->audioTracks;
    Ticks total;
    for (const Clip& clip : tracks[static_cast<std::size_t>(track[1] - '1')]->clips)
        total += clip.duration();
    return total;
}

/// Two linked A/V clips of media A (40 frames), with an English transcript: two speakers, a filler and pauses.
struct Fixture {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    ProjectController controller;
    TranscriptBridge bridge;

    explicit Fixture(std::shared_ptr<const ai::Transcript> words = nullptr) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        bridge.setUseAnalysisCache(false);
        bridge.setController(&controller);
        bridge.addTranscript(tl.media('A'), 0,
                             words ? words
                                   : transcript("en", {{"Hello", 0, 4, 0},
                                                       {"world", 5, 9, 0},
                                                       {"um", 12, 14, 0},
                                                       {"this", 22, 25, 1},
                                                       {"is", 26, 28, 1},
                                                       {"great", 29, 34, 1}}));
    }
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

QQuickItem* wordItem(Shell& shell, int word) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) {
        return i->isVisible() && i->property("wordIndex").isValid() && i->property("wordIndex").toInt() == word;
    });
}

MediaId mediaNamed(const ProjectController& c, const std::string& name) {
    for (const MediaPtr& m : c.snapshot()->media) {
        if (m->name == name)
            return m->id;
    }
    return {};
}

} // namespace

// ---- The view ---------------------------------------------------------------------------------------------------------

TEST_CASE("Transcript view: paragraphs by speaker in timeline order, word click seeks, selection lights the timeline", "[app][transcript]") {
    Fixture fx;
    TranscriptBridge& t = fx.bridge;
    REQUIRE(t.wordCount() == 6);
    CHECK(t.language() == QStringLiteral("en"));
    TranscriptParagraphModel* paragraphs = t.paragraphs();
    REQUIRE(paragraphs->rowCount() == 2); // a new speaker starts a paragraph
    const QVariantMap first = paragraphs->get(0);
    CHECK(first.value(QStringLiteral("speaker")).toString() == QStringLiteral("Speaker 1"));
    CHECK(first.value(QStringLiteral("timeText")).toString() == fx.controller.formatTimecode(Ticks{}));
    const QVariantList words = first.value(QStringLiteral("words")).toList();
    REQUIRE(words.size() == 3);
    CHECK(words[0].toMap().value(QStringLiteral("text")).toString() == QStringLiteral("Hello ")); // English: spaced
    CHECK(paragraphs->get(1).value(QStringLiteral("speaker")).toString() == QStringLiteral("Speaker 2"));

    // Click a word: the playhead goes to it, it is the current word and the selection; the timeline lights its range
    // on its track and the linked video.
    t.clickWord(1, 0);
    CHECK(fx.controller.playhead() == t.sequenceTranscript()[1].timeline.start);
    CHECK(t.currentWord() == 1);
    CHECK(t.isSelected(1));
    CHECK_FALSE(t.isSelected(0));
    REQUIRE(fx.controller.timelineHighlights().size() == 1);
    const TimelineHighlight& h = fx.controller.timelineHighlights().front();
    CHECK(h.kind == TimelineHighlight::Kind::Selection);
    CHECK(h.range.start == f(5));
    CHECK(h.tracks.size() == 2);
    // Shift extends, Ctrl toggles.
    t.clickWord(3, Qt::ShiftModifier);
    CHECK(t.isSelected(2));
    CHECK(t.isSelected(3));
    CHECK(t.selectionInfo().startsWith(QStringLiteral("3 word")));
    t.clickWord(2, Qt::ControlModifier);
    CHECK_FALSE(t.isSelected(2));
    t.clearSelection();
    CHECK(fx.controller.timelineHighlights().empty());
    // The playhead between words: no current word.
    fx.controller.setPlayhead(f(10));
    CHECK(t.currentWord() == -1);
}

TEST_CASE("Deleting words: dry-run preview, then one undo step through transcript.deleteRanges; keep only; move", "[app][transcript]") {
    Fixture fx;
    TranscriptBridge& t = fx.bridge;
    const Ticks before = clipLength(fx.controller, "A1");
    const auto snapshot = fx.controller.snapshot();

    // Delete "Hello world": preview first (struck through, a red ghost on the timeline, nothing changed).
    t.selectWords(0, 1);
    REQUIRE(t.previewDelete().isEmpty());
    CHECK(t.previewing());
    CHECK(t.isRemoving(0));
    CHECK(t.isRemoving(1));
    CHECK_FALSE(t.isRemoving(2));
    CHECK(t.previewInfo().startsWith(QStringLiteral("Removes")));
    CHECK(fx.controller.snapshot() == snapshot);
    const auto& highlights = fx.controller.timelineHighlights();
    CHECK(std::any_of(highlights.begin(), highlights.end(),
                      [](const TimelineHighlight& x) { return x.kind == TimelineHighlight::Kind::Removal; }));
    // Cancel keeps everything.
    t.cancelPreview();
    CHECK_FALSE(t.previewing());
    REQUIRE(t.previewDelete().isEmpty());
    REQUIRE(t.applyPreview().isEmpty());
    CHECK(wordTexts(t) == QStringList({QStringLiteral("um"), QStringLiteral("this"), QStringLiteral("is"), QStringLiteral("great")}));
    CHECK(clipLength(fx.controller, "A1") < before);
    CHECK(clipLength(fx.controller, "V1") == clipLength(fx.controller, "A1")); // the linked video is cut too
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Delete Words"));
    CHECK_FALSE(t.previewing());
    REQUIRE(fx.controller.undo());
    CHECK(t.wordCount() == 6);
    CHECK(clipLength(fx.controller, "A1") == before);

    // Keep only "this is great".
    t.selectWords(3, 5);
    REQUIRE(t.previewKeep().isEmpty());
    CHECK(t.isRemoving(0));
    CHECK_FALSE(t.isRemoving(4));
    REQUIRE(t.applyPreview().isEmpty());
    CHECK(wordTexts(t) == QStringList({QStringLiteral("this"), QStringLiteral("is"), QStringLiteral("great")}));
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Keep Only Words"));
    REQUIRE(fx.controller.undo());

    // Drag the second paragraph before the first: transcript.moveRange.
    REQUIRE(t.moveParagraph(1, 0).isEmpty());
    CHECK(wordTexts(t).first() == QStringLiteral("this"));
    CHECK(wordTexts(t).last() == QStringLiteral("um"));
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Move Words"));
    REQUIRE(fx.controller.undo());
    CHECK(wordTexts(t).first() == QStringLiteral("Hello"));

    // Nothing selected: Delete says so.
    t.clearSelection();
    CHECK_FALSE(t.previewDelete().isEmpty());
}

TEST_CASE("Clean-up review measures real audio and keeps a loud pause by default", "[app][transcript]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("loud-pause.wav"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    QDataStream wav(&file);
    wav.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 samples = 32000;
    wav.writeRawData("RIFF", 4); wav << quint32(36 + samples * 2);
    wav.writeRawData("WAVEfmt ", 8); wav << quint32(16) << quint16(1) << quint16(1)
        << quint32(16000) << quint32(32000) << quint16(2) << quint16(16);
    wav.writeRawData("data", 4); wav << quint32(samples * 2);
    for (quint32 i = 0; i < samples; ++i) wav << qint16(i % 16 < 8 ? 16000 : -16000);
    file.close();
    Fixture fx;
    ProjectState state = *fx.controller.snapshot();
    for (auto& media : state.media) {
        if (media->id != fx.tl.media('A')) continue;
        auto changed = std::make_shared<MediaItem>(*media);
        changed->info.path = path.toStdString();
        changed->locator.path = path.toStdString();
        changed->info.audio[0].index = 0;
        media = std::move(changed);
    }
    fx.controller.resetProject(std::move(state), fx.tl.seq);
    fx.bridge.setSilenceSeconds(0.3);
    fx.bridge.refreshReview();
    REQUIRE(reviewFinished(fx.bridge));
    bool measuredPause = false;
    for (const auto& row : fx.bridge.review()) {
        const auto item = row.toMap();
        if (item.value("kind").toString() != QStringLiteral("silence")) continue;
        measuredPause = true;
        CHECK(item.value("confidence").toDouble() < 0.5);
        CHECK_FALSE(item.value("remove").toBool());
    }
    CHECK(measuredPause);
}

TEST_CASE("Clean-up review runs off-thread and rejects cancelled and stale results", "[app][transcript]") {
    Fixture fx;
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto entered = std::make_shared<std::atomic<bool>>(false);
    const auto guiThread = std::this_thread::get_id();
    auto offThread = std::make_shared<std::atomic<bool>>(false);
    fx.bridge.setReviewRunner([release, entered, offThread, guiThread](const auto& words, const auto& state,
        const auto& options, std::stop_token stop, const auto& progress) {
        offThread->store(std::this_thread::get_id() != guiThread);
        entered->store(true);
        progress(0.5f);
        while (!release->load() && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return ai::edit::review(words, state, options); // even a late result must not replace the new context
    });
    fx.bridge.refreshReview();
    CHECK(fx.bridge.reviewBusy());
    for (int i = 0; i < 100 && !entered->load(); ++i) spin(5);
    REQUIRE(entered->load());
    CHECK(offThread->load());
    spin(20);
    CHECK(fx.bridge.reviewProgress() == 0.5);
    fx.controller.selectClipById(fx.tl.clip("V1@0"), ProjectController::Replace);
    CHECK_FALSE(fx.bridge.reviewBusy());
    release->store(true);
    spin(50);
    CHECK(fx.bridge.review().isEmpty());
    fx.bridge.refreshReview();
    REQUIRE(reviewFinished(fx.bridge));
    CHECK_FALSE(fx.bridge.review().isEmpty());
    fx.bridge.refreshReview();
    fx.bridge.cancelReview();
    spin(30);
    CHECK(fx.bridge.review().isEmpty());
}

TEST_CASE("Clean-up review: fillers and silences listed, chosen ones removed after a preview, one undo step", "[app][transcript]") {
    Fixture fx;
    TranscriptBridge& t = fx.bridge;
    t.setSilenceSeconds(0.3); // the 0.32 s pause before "this"
    t.refreshReview();
    REQUIRE(reviewFinished(t));
    const QVariantList items = t.review();
    REQUIRE(items.size() >= 2);
    QStringList kinds;
    for (const QVariant& v : items)
        kinds << v.toMap().value(QStringLiteral("kind")).toString();
    CHECK(kinds.contains(QStringLiteral("filler")));
    CHECK(kinds.contains(QStringLiteral("silence")));
    const auto filler = std::find_if(items.begin(), items.end(), [](const QVariant& v) {
        return v.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("filler");
    });
    CHECK(filler->toMap().value(QStringLiteral("label")).toString() == QStringLiteral("um"));
    CHECK(filler->toMap().value(QStringLiteral("context")).toString().contains(QStringLiteral("um")));

    // Keep the silence, remove the filler.
    t.setAllReviewRemove(false);
    CHECK(t.reviewRemoveCount() == 0);
    CHECK_FALSE(t.previewReview().isEmpty()); // nothing marked
    t.setReviewRemove(filler->toMap().value(QStringLiteral("index")).toInt(), true);
    CHECK(t.reviewRemoveCount() == 1);
    REQUIRE(t.previewReview().isEmpty());
    CHECK(t.isRemoving(2));
    const auto snapshot = fx.controller.snapshot();
    CHECK(fx.controller.snapshot() == snapshot);
    REQUIRE(t.applyPreview().isEmpty());
    CHECK_FALSE(wordTexts(t).contains(QStringLiteral("um")));
    CHECK(t.wordCount() == 5);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Delete Words"));
    REQUIRE(fx.controller.undo());
    CHECK(t.wordCount() == 6);

    // Remove all: the filler and the pause.
    t.refreshReview();
    REQUIRE(reviewFinished(t));
    t.setAllReviewRemove(true);
    REQUIRE(t.previewReview().isEmpty());
    REQUIRE(t.applyPreview().isEmpty());
    CHECK(t.wordCount() == 5);
}

TEST_CASE("Search: occurrences marked, next match selects and seeks, select all, phrases across words", "[app][transcript]") {
    Fixture fx;
    TranscriptBridge& t = fx.bridge;
    t.setSearchQuery(QStringLiteral("is"));
    CHECK(t.matchCount() == 2); // th[is], [is]
    CHECK(t.isMatch(3));
    CHECK(t.isMatch(4));
    CHECK_FALSE(t.isMatch(0));
    t.nextMatch(1);
    CHECK(t.matchIndex() == 0);
    CHECK(t.isSelected(3));
    CHECK(fx.controller.playhead() == t.sequenceTranscript()[3].timeline.start);
    t.nextMatch(1);
    CHECK(t.isSelected(4));
    CHECK_FALSE(t.isSelected(3));
    t.selectAllMatches();
    CHECK(t.isSelected(3));
    CHECK(t.isSelected(4));

    t.setSearchQuery(QStringLiteral("HELLO WORLD"));
    REQUIRE(t.matchCount() == 1);
    t.nextMatch(1);
    CHECK(t.isSelected(0));
    CHECK(t.isSelected(1));
    t.setSearchQuery(QStringLiteral("zebra"));
    CHECK(t.matchCount() == 0);
}

TEST_CASE("Captions: generate from the transcript, edit a cue's text and timing, export SRT and VTT", "[app][captions]") {
    Fixture fx;
    CaptionsBridge captions;
    captions.setController(&fx.controller);
    captions.setTranscript(&fx.bridge);
    CHECK(captions.canGenerate());
    CHECK(captions.track().isEmpty());

    REQUIRE(captions.generate(42, 2).isEmpty());
    REQUIRE_FALSE(captions.track().isEmpty());
    REQUIRE(fx.controller.currentSequence()->captionTracks.size() == 1);
    const QVariantList cues = captions.cues();
    REQUIRE_FALSE(cues.isEmpty());
    CHECK(cues.front().toMap().value(QStringLiteral("text")).toString().startsWith(QStringLiteral("Hello world")));
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Captions from Transcript")); // M5m: with speakers and corrections

    // Text, as typed; one undo step.
    REQUIRE(captions.setCueText(0, QStringLiteral("Hello, world!")).isEmpty());
    captions.endEdit();
    CHECK(captions.cues().front().toMap().value(QStringLiteral("text")).toString() == QStringLiteral("Hello, world!"));
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Edit Caption"));
    // Timing: kept inside the neighbours.
    REQUIRE(captions.setCueTiming(0, QStringLiteral("00:00:00:02"), QStringLiteral("00:00:00:10")).isEmpty());
    CHECK(fx.controller.currentSequence()->captionTracks.front().cues.front().start == f(2));
    CHECK(fx.controller.currentSequence()->captionTracks.front().cues.front().end() == f(10));

    // The cue under the playhead (the list highlights it; the compositor draws it).
    fx.controller.setPlayhead(f(5));
    CHECK(captions.activeCue() == 0);

    // Export.
    const QString srt = captions.exportText(QStringLiteral("srt"));
    CHECK(srt.startsWith(QStringLiteral("1\n00:00:00,080 --> 00:00:00,400\nHello, world!")));
    CHECK(captions.exportText(QStringLiteral("vtt")).startsWith(QStringLiteral("WEBVTT")));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("captions.srt"));
    REQUIRE(captions.exportFile(QUrl::fromLocalFile(path), QStringLiteral("srt")).isEmpty());
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(QString::fromUtf8(file.readAll()) == srt);

    // Undo the edits, then delete the track.
    REQUIRE(fx.controller.undo());
    REQUIRE(fx.controller.undo());
    CHECK(captions.cues().front().toMap().value(QStringLiteral("text")).toString().startsWith(QStringLiteral("Hello world")));
    REQUIRE(captions.removeTrack().isEmpty());
    CHECK(fx.controller.currentSequence()->captionTracks.empty());
}

TEST_CASE("Thai: word units side by side without spaces, search across units, captions without spaces", "[app][transcript]") {
    Fixture fx(transcript("th", {{"สวัสดี", 0, 6, 0}, {"ครับ", 6, 9, 0}, {"วันนี้", 12, 16, 0}, {"อากาศ", 16, 20, 0}, {"ดี", 20, 23, 0}}));
    TranscriptBridge& t = fx.bridge;
    REQUIRE(t.wordCount() == 5);
    CHECK(t.language() == QStringLiteral("th"));
    const QVariantList words = t.paragraphs()->get(0).value(QStringLiteral("words")).toList();
    REQUIRE(words.size() == 5);
    CHECK(words[0].toMap().value(QStringLiteral("text")).toString() == QStringLiteral("สวัสดี")); // no space
    t.setSearchQuery(QStringLiteral("ดีคร")); // spans "สวัสดี" and "ครับ"
    REQUIRE(t.matchCount() == 1);
    CHECK(t.isMatch(0));
    CHECK(t.isMatch(1));
    CaptionsBridge captions;
    captions.setController(&fx.controller);
    captions.setTranscript(&t);
    REQUIRE(captions.generate().isEmpty());
    const QString text = captions.cues().front().toMap().value(QStringLiteral("text")).toString();
    CHECK(text.startsWith(QStringLiteral("สวัสดีครับ")));
    CHECK_FALSE(text.contains(QLatin1Char(' ')));
    // Deleting a Thai word works like any other.
    t.selectWords(1, 1);
    REQUIRE(t.previewDelete().isEmpty());
    REQUIRE(t.applyPreview().isEmpty());
    CHECK(t.wordCount() == 4);
}

TEST_CASE("Transcript Find All text: the first selected run as its language writes it", "[app][transcript]") {
    {
        Fixture fx(transcript("th", {{"สวัสดี", 0, 6, 0}, {"ครับ", 6, 9, 0}, {"วันนี้", 12, 16, 0}, {"อากาศ", 16, 20, 0}, {"ดี", 20, 23, 0}}));
        TranscriptBridge& t = fx.bridge;
        CHECK(t.selectionSearchText().isEmpty()); // nothing selected
        // Two Thai units: no space between them, so the search finds them (the Fix field keeps the units apart).
        t.selectWords(0, 1);
        CHECK(t.selectionSearchText() == QStringLiteral("สวัสดีครับ"));
        CHECK(t.selectedText() == QStringLiteral("สวัสดี ครับ"));
        t.setSearchQuery(t.selectionSearchText());
        CHECK(t.matchCount() == 1);
        // Two runs (Ctrl-click): the first one, not an empty search.
        t.clickWord(3, Qt::ControlModifier);
        REQUIRE(t.isSelected(3));
        CHECK(t.selectedText().isEmpty());
        CHECK(t.selectionSearchText() == QStringLiteral("สวัสดีครับ"));
    }
    {
        Fixture fx;
        TranscriptBridge& t = fx.bridge;
        t.selectWords(0, 1);
        CHECK(t.selectionSearchText() == QStringLiteral("Hello world"));
        t.setSearchQuery(t.selectionSearchText());
        CHECK(t.matchCount() == 1);
    }
}

// ---- Transcription ----------------------------------------------------------------------------------------------------

TEST_CASE("Transcription: providers, the set-up state, a job with progress that fills the transcript", "[app][transcript]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    // The media needs a file for a job: point A at a temporary one.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("a.wav"));
    {
        std::ofstream(file.toStdString()) << "RIFF";
    }
    ProjectState state = tl.state;
    for (MediaPtr& m : state.media) {
        if (m->id == tl.media('A')) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->locator.path = file.toStdString();
            m = copy;
        }
    }
    ProjectController controller;
    controller.resetProject(state, tl.seq);
    TranscriptBridge t;
    t.setUseAnalysisCache(false);
    t.setController(&controller);

    // No local helper in this build and no AI controller: nothing can transcribe.
    const QVariantList providers = t.providers();
    REQUIRE_FALSE(providers.isEmpty());
    CHECK(providers.front().toMap().value(QStringLiteral("id")).toString() == QStringLiteral("local"));
    if (!providers.front().toMap().value(QStringLiteral("available")).toBool()) {
        CHECK(t.setupNeeded());
        CHECK_FALSE(providers.front().toMap().value(QStringLiteral("reason")).toString().isEmpty());
        CHECK_FALSE(t.transcribe(QStringLiteral("sequence")).isEmpty());
    }

    // A transcriber (the seam cloud and local ones plug into): progress, then the words appear.
    std::atomic<int> calls = 0;
    t.setTranscriber(QStringLiteral("fake"), QStringLiteral("Fake"),
                     [&](const std::string& path, int, const std::function<void(float)>& progress, std::stop_token) {
                         ++calls;
                         CHECK(path == file.toStdString());
                         progress(0.5f);
                         return std::expected<ai::Transcript, Error>(*transcript("en", {{"Hi", 0, 4}, {"there", 5, 9}}));
                     });
    CHECK_FALSE(t.setupNeeded());
    t.setProvider(QStringLiteral("fake"));
    REQUIRE(t.transcribe(QStringLiteral("sequence")).isEmpty());
    CHECK(t.jobStatus() == QStringLiteral("running"));
    REQUIRE(waitFor([&] { return t.jobStatus() != QStringLiteral("running"); }));
    CHECK(t.jobStatus() == QStringLiteral("done"));
    CHECK(calls == 1);
    CHECK(t.wordCount() == 2);
    CHECK(t.jobProgress() == 1.0);
    // Everything is transcribed now: nothing more to do.
    CHECK_FALSE(t.transcribe(QStringLiteral("sequence")).isEmpty());
}

TEST_CASE("Transcription: the project's audio redactions never come back as words, whatever transcribes (P1-3)",
          "[app][transcript][redaction]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("a.wav"));
    {
        std::ofstream(file.toStdString()) << "RIFF";
    }
    ProjectState state = tl.state;
    for (MediaPtr& m : state.media) {
        if (m->id == tl.media('A')) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->locator.path = file.toStdString();
            m = copy;
        }
    }
    // A bleep over media frames 4 .. 9: the second word.
    Redaction r;
    r.media = tl.media('A');
    r.source = TimeRange::fromStartEnd(framesToTicks(4, kRate), framesToTicks(9, kRate));
    r.audio = RedactionAudio::Bleep;
    auto added = edit::addRedaction(state, r);
    REQUIRE(added);
    state = std::move(added->state);
    ProjectController controller;
    controller.resetProject(state, tl.seq);
    TranscriptBridge t;
    t.setUseAnalysisCache(false);
    t.setController(&controller);
    // A transcriber that hears everything (as a provider that ignored the silenced PCM would): the panel still redacts.
    t.setTranscriber(QStringLiteral("fake"), QStringLiteral("Fake"),
                     [&](const std::string&, int, const std::function<void(float)>&, std::stop_token) {
                         return std::expected<ai::Transcript, Error>(*transcript("en", {{"Hi", 0, 3}, {"secret", 5, 8}, {"there", 10, 14}}));
                     });
    t.setProvider(QStringLiteral("fake"));
    REQUIRE(t.transcribe(QStringLiteral("sequence")).isEmpty());
    REQUIRE(waitFor([&] { return t.jobStatus() != QStringLiteral("running"); }));
    CHECK(t.jobStatus() == QStringLiteral("done"));
    CHECK(wordTexts(t) == QStringList{QStringLiteral("Hi"), QStringLiteral("[bleep]"), QStringLiteral("there")});
}

TEST_CASE("Redaction: transcripts, captions, burn-in and sidecars use one redacted-word token", "[app][redaction]") {
    // Two owners once chose different tokens ("[redacted]" vs "[bleep]"); pin them together.
    CHECK(wabcut::ai::kRedactedWord == wabcut::ai::edit::kRedactedWordText);
    CHECK(wabcut::ai::kRedactedWord == wabcut::exporter::kRedactedWordToken);
    CHECK(wabcut::ai::kRedactedWord == wabcut::playback::kRedactedWordToken);
}

// ---- In the shell -----------------------------------------------------------------------------------------------------

TEST_CASE("Transcript panel in the shell: click a word, Delete previews, Thai renders; screenshots", "[app][transcript][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(t != nullptr);
    t->setUseAnalysisCache(false);
    const Rate rate = controller->currentSequence()->frameRate;
    const double fps = rate.toDouble();
    const auto s = [&](double seconds) { return seconds * fps; };
    // Interview A (V1/A1 from 0 s) and Interview B (from 24 s, Thai).
    t->addTranscript(mediaNamed(*controller, "Interview A.mov"), 0,
                     transcript("en",
                                {{"So", s(0.4), s(0.6), 0},       {"um,", s(0.7), s(1.0), 0},   {"we", s(1.6), s(1.8), 0},
                                 {"started", s(1.8), s(2.3), 0},  {"the", s(2.3), s(2.4), 0},   {"project", s(2.4), s(2.9), 0},
                                 {"last", s(2.9), s(3.2), 0},     {"spring.", s(3.2), s(3.7), 0}, {"It", s(5.2), s(5.3), 1},
                                 {"was", s(5.3), s(5.5), 1},      {"uh", s(5.6), s(5.9), 1},    {"a", s(6.4), s(6.5), 1},
                                 {"small", s(6.5), s(6.9), 1},    {"team", s(6.9), s(7.3), 1},  {"and", s(7.3), s(7.5), 1},
                                 {"a", s(7.5), s(7.6), 1},        {"big", s(7.6), s(7.9), 1},   {"idea.", s(7.9), s(8.5), 1},
                                 {"Everyone", s(10.0), s(10.5), 0}, {"brought", s(10.5), s(10.9), 0}, {"something", s(10.9), s(11.5), 0},
                                 {"different", s(11.5), s(12.1), 0}, {"to", s(12.1), s(12.2), 0}, {"the", s(12.2), s(12.3), 0},
                                 {"table.", s(12.3), s(12.9), 0}},
                                rate));
    t->addTranscript(mediaNamed(*controller, "Interview B.mov"), 0,
                     transcript("th", {{"สวัสดี", s(0.5), s(1.1), 2}, {"ครับ", s(1.1), s(1.4), 2}, {"วันนี้", s(1.6), s(2.0), 2},
                                       {"เรา", s(2.0), s(2.2), 2}, {"จะ", s(2.2), s(2.3), 2}, {"มา", s(2.3), s(2.5), 2},
                                       {"ตัดต่อ", s(2.5), s(3.0), 2}, {"วิดีโอ", s(3.0), s(3.6), 2}},
                                rate));
    REQUIRE(t->wordCount() == 33);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull());

    // Click "project" with the mouse: the playhead goes there.
    QQuickItem* project = wordItem(shell, 5);
    REQUIRE(project != nullptr);
    const QPointF p = project->mapToScene(QPointF(project->width() / 2, project->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::NoButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &release);
    spin(30);
    // The playhead is on frames: the first one inside the word, which is the current word.
    const TimeRange clicked = t->sequenceTranscript()[5].timeline;
    CHECK(controller->playhead() >= clicked.start);
    CHECK(controller->playhead() < clicked.end());
    CHECK(t->currentWord() == 5);
    CHECK(t->isSelected(5));

    // Select "project last spring." and press Delete: a preview, nothing removed yet.
    t->selectWords(5, 7);
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &del);
    QKeyEvent delUp(QEvent::KeyRelease, Qt::Key_Delete, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &delUp);
    spin(50);
    CHECK(t->previewing());
    CHECK(t->isRemoving(6));
    CHECK(controller->canUndo() == false);

    // Thai words render: their glyph runs exist and the items have width.
    QQuickItem* thai = wordItem(shell, 25);
    REQUIRE(thai != nullptr);
    CHECK(thai->implicitWidth() > 0);
    QTextLayout layout(thai->property("text").toString(), thai->property("font").value<QFont>());
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    CHECK(layout.glyphRuns().size() > 0);

    // Screenshots: the transcript with a selection and the strike-through preview, then the clean-up review.
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const auto crop = [&](const QImage& image, const char* name, const QString& file) {
        QQuickItem* item = shell.item(name);
        REQUIRE(item != nullptr);
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(outputDir() + file));
    };
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/transcript-preview.png")));
        crop(image, "transcriptPanel", QStringLiteral("/transcript-preview-crop.png"));
    }
    t->cancelPreview();
    t->setSilenceSeconds(1.0);
    t->refreshReview();
    REQUIRE(reviewFinished(*t));
    REQUIRE(t->review().size() >= 2);
    REQUIRE(t->previewReview().isEmpty());
    QObject* tabs = shell.object("transcriptTabs");
    REQUIRE(tabs != nullptr);
    tabs->setProperty("currentIndex", 1);
    REQUIRE(t->previewReview().isEmpty()); // the tab switch cancels a preview; show it again
    spin(300);
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/transcript-cleanup.png")));
        crop(image, "transcriptPanel", QStringLiteral("/transcript-cleanup-crop.png"));
    }
    t->cancelPreview();

    // Captions: generated from the transcript; the compositor draws the cue under the playhead in the picture (I.4).
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    REQUIRE(captions != nullptr);
    REQUIRE(captions->generate().isEmpty());
    REQUIRE_FALSE(captions->cues().isEmpty());
    tabs->setProperty("currentIndex", 2);
    captions->seekToCue(1);
    spin(300);
    CHECK(captions->activeCue() == 1);
    // I.4: the compositor draws the cue (preview == burned-in export); the QML overlay is gone.
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    REQUIRE(playback != nullptr);
    CHECK(playback->captionsShown());
    CHECK(shell.item("captionOverlayText") == nullptr);
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/transcript-captions.png")));
        crop(image, "transcriptPanel", QStringLiteral("/transcript-captions-crop.png"));
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// Review wave 5, A5-P2-28: the redaction ranges were keyed by file path from the media items being transcribed, so a
// second media item of the same file (imported twice) holding the redaction did not count, and the span was uploaded.
TEST_CASE("Transcription: a redaction on another media item of the same file still holds", "[app][transcript][redaction]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("a.wav"));
    {
        std::ofstream(file.toStdString()) << "RIFF";
    }
    ProjectState state = tl.state;
    for (MediaPtr& m : state.media) {
        if (m->id == tl.media('A')) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->locator.path = file.toStdString();
            m = copy;
        }
    }
    // The same file imported again: not on the timeline, but it holds the bleep over media frames 4 .. 9.
    MediaItem twin = *state.findMedia(tl.media('A'));
    twin.name = "a (2)";
    auto imported = edit::addMedia(state, twin);
    REQUIRE(imported);
    state = std::move(imported->state);
    Redaction r;
    r.media = imported->id;
    r.source = TimeRange::fromStartEnd(framesToTicks(4, kRate), framesToTicks(9, kRate));
    r.audio = RedactionAudio::Bleep;
    auto redacted = edit::addRedaction(state, r);
    REQUIRE(redacted);
    state = std::move(redacted->state);
    ProjectController controller;
    controller.resetProject(state, tl.seq);
    TranscriptBridge t;
    t.setUseAnalysisCache(false);
    t.setController(&controller);
    t.setTranscriber(QStringLiteral("fake"), QStringLiteral("Fake"),
                     [&](const std::string&, int, const std::function<void(float)>&, std::stop_token) {
                         return std::expected<ai::Transcript, Error>(*transcript("en", {{"Hi", 0, 3}, {"secret", 5, 8}, {"there", 10, 14}}));
                     });
    t.setProvider(QStringLiteral("fake"));
    REQUIRE(t.transcribe(QStringLiteral("sequence")).isEmpty());
    REQUIRE(waitFor([&] { return t.jobStatus() != QStringLiteral("running"); }));
    CHECK(t.jobStatus() == QStringLiteral("done"));
    CHECK(wordTexts(t) == QStringList{QStringLiteral("Hi"), QStringLiteral("[bleep]"), QStringLiteral("there")});
}

// ---- 0.4: follow the playhead, the word menu, voice tracks --------------------------------------------------------------

TEST_CASE("Transcript paragraphOfWord finds the paragraph at its first and last word", "[app][transcript]") {
    Fixture fx;
    TranscriptBridge& t = fx.bridge;
    REQUIRE(t.paragraphs()->rowCount() == 2); // words 0..2, then 3..5
    CHECK(t.paragraphOfWord(0) == 0);
    CHECK(t.paragraphOfWord(2) == 0);
    CHECK(t.paragraphOfWord(3) == 1);
    CHECK(t.paragraphOfWord(5) == 1);
    CHECK(t.paragraphOfWord(6) == -1);
    CHECK(t.paragraphOfWord(-1) == -1);
    CHECK(t.selectionFirst() == -1);
    t.selectWords(4, 5);
    CHECK(t.selectionFirst() == 4);
    // An empty transcript.
    Timeline tl(R"(
        V1 tgt |Bbbbbbbbbb|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    TranscriptBridge empty;
    empty.setUseAnalysisCache(false);
    empty.setController(&controller);
    CHECK(empty.paragraphOfWord(0) == -1);
    CHECK(empty.selectionFirst() == -1);
}

TEST_CASE("Transcript selection: copy and copy with timecodes fill the system clipboard", "[app][transcript][clipboard]") {
    Fixture fx;
    TranscriptBridge& t = fx.bridge;
    QClipboard* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard);
    const bool canPaste = fx.controller.canPaste();

    // Nothing selected: says so.
    CHECK_FALSE(t.copySelection(false).isEmpty());
    CHECK_FALSE(t.message().isEmpty());

    // The words as their language writes them.
    t.selectWords(0, 1);
    REQUIRE(t.copySelection(false).isEmpty());
    CHECK(clipboard->text() == QStringLiteral("Hello world"));
    // Separate runs (Ctrl-click) on separate lines.
    t.clickWord(0, 0);
    t.clickWord(4, Qt::ControlModifier);
    t.clickWord(5, Qt::ControlModifier);
    REQUIRE(t.copySelection(false).isEmpty());
    CHECK(clipboard->text() == QStringLiteral("Hello\nis great"));

    // With timecodes: one line per run and paragraph, the speaker when it is known.
    t.selectWords(1, 4);
    REQUIRE(t.copySelection(true).isEmpty());
    CHECK(clipboard->text() == QStringLiteral("[") + fx.controller.formatTimecode(f(5)) + QStringLiteral("] Speaker 1: world um\n[")
                                   + fx.controller.formatTimecode(f(22)) + QStringLiteral("] Speaker 2: this is"));
    CHECK(fx.controller.canPaste() == canPaste); // the clip clipboard is untouched
}

TEST_CASE("Transcript selection: Mark In and Out and markers from the words in one undo step each", "[app][transcript]") {
    // Words that start and end inside frames: "this" from 22.4, "great" up to 33.6 (25 fps).
    Fixture fx(transcript("en", {{"Hello", 0, 4, 0},
                                 {"world", 5, 9, 0},
                                 {"um", 12, 14, 0},
                                 {"this", 22.4, 25, 1},
                                 {"is", 26, 28, 1},
                                 {"great", 29, 33.6, 1}}));
    TranscriptBridge& t = fx.bridge;
    fx.controller.setPlayhead(f(2));
    t.selectWords(3, 5);
    const Ticks playhead = fx.controller.playhead();

    // In at the start of the first word's frame, Out at the end of the last word's frame: one undo step.
    const auto undos = fx.controller.session().undoCount();
    REQUIRE(t.markSelection().isEmpty());
    CHECK(fx.controller.session().undoCount() == undos + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Mark In and Out"));
    CHECK(fx.controller.markIn() == f(22));
    CHECK(fx.controller.markOut() == f(34));
    CHECK(fx.controller.playhead() == playhead); // the playhead and the selection stay
    CHECK(t.hasSelection());
    REQUIRE(fx.controller.undo());
    CHECK_FALSE(fx.controller.markIn());

    // A range marker named from the words, its comment the whole text.
    REQUIRE(t.addMarkerFromSelection(false).isEmpty());
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Add Marker"));
    REQUIRE(fx.controller.currentSequence()->markers.size() == 1);
    const Marker marker = fx.controller.currentSequence()->markers.front();
    CHECK(marker.start == f(22));
    CHECK(marker.duration == f(12));
    CHECK(marker.name == "this is great");
    CHECK(marker.comment == "this is great");
    CHECK(marker.type == MarkerType::Comment);
    REQUIRE(fx.controller.undo());
    CHECK(fx.controller.currentSequence()->markers.empty());
    // A chapter marker: a point marker of type chapter.
    REQUIRE(t.addMarkerFromSelection(true).isEmpty());
    REQUIRE(fx.controller.currentSequence()->markers.size() == 1);
    const Marker chapter = fx.controller.currentSequence()->markers.front();
    CHECK(chapter.type == MarkerType::Chapter);
    CHECK(chapter.start == f(22));
    CHECK(chapter.duration == Ticks{});
    CHECK(chapter.name == "this is great");

    // A long selection: the name is cut at a word with an ellipsis; the comment keeps everything.
    std::vector<W> many;
    for (int i = 0; i < 30; ++i)
        many.push_back(W{"word" + std::to_string(i), static_cast<double>(i), i + 0.8, 0});
    Fixture longer(transcript("en", many));
    longer.bridge.selectWords(0, 29);
    REQUIRE(longer.bridge.addMarkerFromSelection(false).isEmpty());
    const Marker named = longer.controller.currentSequence()->markers.front();
    CHECK(named.name.size() <= 64);
    CHECK(QString::fromStdString(named.name).endsWith(QChar(0x2026)));
    CHECK(named.comment.size() > named.name.size());
}

TEST_CASE("Transcript selection: Lift leaves a gap on the words tracks and one undo restores", "[app][transcript]") {
    Fixture fx;
    TranscriptBridge& t = fx.bridge;
    const Ticks duration = fx.controller.currentSequence()->duration;
    t.selectWords(3, 5);
    const auto undos = fx.controller.session().undoCount();
    REQUIRE(t.liftSelection().isEmpty());
    CHECK(fx.controller.session().undoCount() == undos + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Lift"));
    // A gap on the audio track and its linked video; the sequence keeps its length.
    const SequencePtr seq = fx.controller.currentSequence();
    CHECK(seq->duration == duration);
    CHECK(seq->videoTracks[0]->clips.size() == 2);
    CHECK(seq->audioTracks[0]->clips.size() == 2);
    CHECK(seq->audioTracks[0]->clips[1].start() == f(34));
    // The lifted words left the transcript; In/Out are cleared; the playhead parks at the gap.
    CHECK(t.wordCount() == 3);
    CHECK_FALSE(t.hasSelection());
    CHECK_FALSE(fx.controller.markIn());
    CHECK_FALSE(fx.controller.markOut());
    CHECK(fx.controller.playhead() == f(22));
    REQUIRE(fx.controller.undo());
    CHECK(t.wordCount() == 6);
    CHECK(fx.controller.currentSequence()->audioTracks[0]->clips.size() == 1);

    // A locked track refuses and says so.
    const SequenceId seqId = fx.controller.currentSequenceId();
    const TrackId v1 = fx.controller.currentSequence()->videoTracks[0]->id;
    REQUIRE(fx.controller.applyEditFn("Lock", [&](const ProjectState& s) {
        edit::TrackStateChange change;
        change.locked = true;
        return edit::setTrackState(s, seqId, v1, change);
    }));
    t.selectWords(0, 1);
    const auto locked = fx.controller.session().undoCount();
    CHECK(t.liftSelection() == QStringLiteral("A locked track would change"));
    CHECK(t.message() == QStringLiteral("A locked track would change"));
    CHECK(fx.controller.session().undoCount() == locked);
}

namespace {

/// An OBS-style recording: A1 "Game" (no role), A2 "Mic" (dialogue) and A3 "Discord" (dialogue, voice chat), linked with
/// the video; each audio file exists, so a job can run. A counting fake transcriber records the files it hears.
struct VoiceTracks {
    QTemporaryDir dir;
    Timeline tl{R"(
        V1 tgt |Gggggggggggggggggggg|
        A1 tgt |Gggggggggggggggggggg|
        A2 tgt |Mmmmmmmmmmmmmmmmmmmm|
        A3 tgt |Dddddddddddddddddddd|
    )"};
    ProjectController controller;
    TranscriptBridge t;
    std::mutex mutex;
    std::vector<std::string> heard;
    bool clearChoicesOnExit = true; ///< false: the next VoiceTracks reads the stored choice

    /// linkDiscord false: the Discord clip is not linked to the recording (a separate voice-chat file).
    explicit VoiceTracks(bool clearChoices = true, bool micMuted = false, bool linkDiscord = true) {
        if (clearChoices)
            QSettings().remove(QStringLiteral("transcript"));
        REQUIRE(dir.isValid());
        tl.link(linkDiscord ? "V1@0 A1@0 A2@0 A3@0" : "V1@0 A1@0 A2@0");
        ProjectState state = tl.state;
        for (MediaPtr& m : state.media) {
            for (const char letter : {'G', 'M', 'D'}) {
                if (m->id == tl.media(letter)) {
                    const QString file = dir.filePath(QString(QLatin1Char(letter)) + QStringLiteral(".wav"));
                    std::ofstream(file.toStdString()) << "RIFF";
                    auto copy = std::make_shared<MediaItem>(*m);
                    copy->locator.path = file.toStdString();
                    m = copy;
                }
            }
        }
        const auto name = [&](std::size_t track, std::string text, std::optional<TrackRole> role, bool muted) {
            edit::TrackStateChange change;
            change.name = std::move(text);
            change.role = role;
            if (muted)
                change.muted = true;
            state = ok(edit::setTrackState(state, tl.seq, state.findSequence(tl.seq)->audioTracks[track]->id, change)).state;
        };
        name(0, "Game", std::nullopt, false);
        name(1, "Mic", TrackRole::Dialogue, micMuted);
        name(2, "Discord", TrackRole::Dialogue, false);
        controller.resetProject(state, tl.seq);
        t.setUseAnalysisCache(false);
        t.setController(&controller);
        t.setTranscriber(QStringLiteral("fake"), QStringLiteral("Fake"),
                         [this](const std::string& path, int, const std::function<void(float)>&, std::stop_token) {
                             const std::lock_guard lock(mutex);
                             heard.push_back(std::filesystem::path(path).filename().string());
                             return std::expected<ai::Transcript, Error>(*transcript("en", {{"Hi", 0, 4}}));
                         });
        t.setProvider(QStringLiteral("fake"));
    }
    ~VoiceTracks() {
        if (clearChoicesOnExit)
            QSettings().remove(QStringLiteral("transcript"));
    }

    /// The files a transcription of `scope` sent, sorted.
    std::vector<std::string> transcribe(const QString& scope) {
        {
            const std::lock_guard lock(mutex);
            heard.clear();
        }
        const QString error = t.transcribe(scope);
        INFO(error.toStdString());
        REQUIRE(error.isEmpty());
        REQUIRE(waitFor([&] { return t.jobStatus() != QStringLiteral("running"); }));
        const std::lock_guard lock(mutex);
        std::vector<std::string> out = heard;
        std::sort(out.begin(), out.end());
        return out;
    }
    bool chosen(int index) const { return t.audioTracks().value(index).toMap().value(QStringLiteral("chosen")).toBool(); }
};

} // namespace

TEST_CASE("Transcription: only the voice tracks are chosen by default and the choice is remembered by name",
          "[app][transcript][tracks]") {
    {
        VoiceTracks v;
        const QVariantList tracks = v.t.audioTracks();
        REQUIRE(tracks.size() == 3);
        const QVariantMap mic = tracks[1].toMap();
        CHECK(mic.value(QStringLiteral("index")).toInt() == 1);
        CHECK(mic.value(QStringLiteral("name")).toString() == QStringLiteral("Mic"));
        CHECK(mic.value(QStringLiteral("role")).toString() == QStringLiteral("dialogue"));
        CHECK(mic.value(QStringLiteral("label")).toString().startsWith(QStringLiteral("A2")));
        CHECK(mic.value(QStringLiteral("label")).toString().contains(QStringLiteral("Mic")));
        CHECK_FALSE(mic.value(QStringLiteral("muted")).toBool());
        // The mic only: game audio and voice chat are left out.
        CHECK_FALSE(v.chosen(0));
        CHECK(v.chosen(1));
        CHECK_FALSE(v.chosen(2));
        CHECK(v.transcribe(QStringLiteral("sequence")) == std::vector<std::string>{"M.wav"});
        // Choosing Discord is remembered by its name.
        int changes = 0;
        QObject::connect(&v.t, &TranscriptBridge::audioTracksChanged, [&] { ++changes; });
        v.t.setTrackChosen(2, true);
        CHECK(v.chosen(2));
        CHECK(changes == 1);
        CHECK(QSettings().value(QStringLiteral("transcript/trackChoices")).toString().contains(QStringLiteral("discord")));
        v.clearChoicesOnExit = false; // the next bridge reads it
    }
    {
        // Another project (a new bridge) with a track of that name: chosen, so two files go.
        VoiceTracks again(false);
        CHECK(again.chosen(2));
        CHECK(again.transcribe(QStringLiteral("sequence")) == std::vector<std::string>({"D.wav", "M.wav"}));
    }
    {
        // A muted dialogue track is not chosen (with no voice track left, every unmuted track is).
        VoiceTracks muted(true, true);
        CHECK_FALSE(muted.chosen(1));
        CHECK(muted.t.audioTracks().value(1).toMap().value(QStringLiteral("muted")).toBool());
        // No track chosen: nothing to send.
        muted.t.setTrackChosen(0, false);
        muted.t.setTrackChosen(2, false);
        CHECK(muted.t.transcribe(QStringLiteral("sequence")) == QStringLiteral("Choose at least one audio track"));
    }
}

TEST_CASE("Transcription: the selection scope keeps linked partners only on chosen tracks", "[app][transcript][tracks]") {
    VoiceTracks v;
    // The game audio clip is selected (with its linked partners): only the mic is transcribed.
    v.controller.selectClipById(clipAt(*v.controller.snapshot(), v.tl.seq, "A1@0"), ProjectController::Replace);
    CHECK(v.transcribe(QStringLiteral("selection")) == std::vector<std::string>{"M.wav"});
}

TEST_CASE("Transcription: selected clips only on switched-off tracks say which tracks", "[app][transcript][tracks]") {
    VoiceTracks v(true, false, false);
    // The Discord clip alone (not linked): its track is off, so nothing goes, and the message says why.
    v.controller.selectClipById(clipAt(*v.controller.snapshot(), v.tl.seq, "A3@0"), ProjectController::Replace);
    const QString off = v.t.transcribe(QStringLiteral("selection"));
    CHECK(off == QStringLiteral("Nothing to transcribe: the selected clips are on tracks switched off under Tracks (A3 · Discord)"));
    CHECK(v.t.message() == off);
    CHECK(v.t.jobStatus() != QStringLiteral("running"));
    // Its track switched on: it is transcribed. Then it has a transcript: the usual message.
    v.t.setTrackChosen(2, true);
    CHECK(v.transcribe(QStringLiteral("selection")) == std::vector<std::string>{"D.wav"});
    CHECK(v.t.transcribe(QStringLiteral("selection")).startsWith(QStringLiteral("Nothing to transcribe: every clip here has a transcript")));
}

// ---- 0.4: the Transcript tab in the shell (follow, the word menu, the Tracks checklist) ----------------------------------

namespace {

void sendKey(Shell& shell, int key, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(shell.window.get(), &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(shell.window.get(), &release);
    spin(30);
}

void clickItem(Shell& shell, QQuickItem* item, Qt::MouseButton button = Qt::LeftButton) {
    REQUIRE(item != nullptr);
    spin(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 30);
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, p, p, button, button, {});
    QCoreApplication::sendEvent(shell.window.get(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, p, p, button, Qt::NoButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &release);
    spin(60);
}

QQuickItem* visibleNamed(Shell& shell, const QString& name) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == name && i->isVisible(); });
}

/// The panel and the open menu that holds `menuRow` (null: the panel alone) in one crop.
void saveUnionCrop(const QImage& image, const QQuickItem* panel, const QQuickItem* menuRow, const QString& file) {
    const double dpr = image.devicePixelRatio();
    QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    if (menuRow) {
        const auto* menu = menuRow->property("menu").value<QObject*>();
        const auto* content = menu ? menu->property("contentItem").value<QQuickItem*>() : nullptr;
        const QQuickItem* popup = content && content->parentItem() ? content->parentItem() : menuRow;
        r = r.united(popup->mapRectToScene(QRectF(0, 0, popup->width(), popup->height())).adjusted(-16, -16, 16, 16));
    }
    r = r.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(outputDir() + file));
}

} // namespace

TEST_CASE("Transcript view follows the playhead unless turned off or the user just scrolled", "[app][transcript][follow][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(controller != nullptr);
    REQUIRE(t != nullptr);
    t->setUseAnalysisCache(false);
    // 400 words, two a paragraph (the speaker alternates): 200 paragraphs. "zebra" is word 380 (paragraph 190).
    const std::string body = "A" + std::string(419, 'a');
    Timeline tl("V1 tgt |" + body + "|\nA1 tgt |" + body + "|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    std::vector<W> words;
    for (int i = 0; i < 400; ++i)
        words.push_back(W{i == 380 ? std::string("zebra") : "word" + std::to_string(i), static_cast<double>(i), i + 0.8, (i / 2) % 2});
    t->addTranscript(tl.media('A'), 0, transcript("en", words));
    REQUIRE(t->wordCount() == 400);
    REQUIRE(t->paragraphs()->rowCount() == 200);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull()); // lays the hidden window out
    QQuickItem* list = visibleNamed(shell, QStringLiteral("transcriptList"));
    REQUIRE(list != nullptr);
    CHECK(shell.preferences->property("transcriptFollowPlayhead").toBool()); // on by default
    const auto inView = [&](int paragraph) {
        QQuickItem* row = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, paragraph));
        if (!row || !row->isVisible())
            return false;
        const double y = row->mapToItem(list, QPointF(0, 0)).y();
        return y >= -1 && y + row->height() <= list->height() + 1;
    };
    const auto contentY = [&] { return list->property("contentY").toDouble(); };
    REQUIRE(inView(0));

    // The playhead in paragraph 150: it comes into view.
    controller->setPlayhead(f(300));
    spin(50);
    CHECK(t->currentWord() == 300);
    CHECK(inView(150));
    // The playhead a word further on, in the same view: no jump (Contain).
    const double shown = contentY();
    controller->setPlayhead(f(301));
    spin(50);
    CHECK(contentY() == shown);

    // Follow off (the crosshair button, remembered): the view stays.
    QQuickItem* toggle = visibleNamed(shell, QStringLiteral("transcriptFollowToggle"));
    REQUIRE(toggle != nullptr);
    clickItem(shell, toggle);
    CHECK_FALSE(shell.preferences->property("transcriptFollowPlayhead").toBool());
    CHECK_FALSE(toggle->property("checked").toBool());
    controller->setPlayhead(f(20));
    spin(50);
    CHECK(t->currentWord() == 20);
    CHECK(contentY() == shown);
    CHECK_FALSE(inView(10));

    // Follow on, but the user just scrolled: it waits; after the pause the next word brings it back.
    clickItem(shell, toggle);
    CHECK(shell.preferences->property("transcriptFollowPlayhead").toBool());
    CHECK(toggle->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
    spin(30);
    const double top = contentY();
    list->setProperty("userScrolledAt", static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    controller->setPlayhead(f(300));
    spin(50);
    CHECK(contentY() == top);
    CHECK_FALSE(inView(150));
    list->setProperty("userScrolledAt", 0.0);
    controller->setPlayhead(f(302));
    spin(50);
    CHECK(inView(151));

    // Find's next match always shows it, even with follow off.
    clickItem(shell, toggle);
    REQUIRE_FALSE(shell.preferences->property("transcriptFollowPlayhead").toBool());
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
    spin(30);
    t->setSearchQuery(QStringLiteral("zebra"));
    REQUIRE(t->matchCount() == 1);
    t->nextMatch(1);
    spin(50);
    CHECK(t->isSelected(380));
    CHECK(inView(190));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Transcript view follows the word in a paragraph taller than the view", "[app][transcript][follow][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(controller != nullptr);
    REQUIRE(t != nullptr);
    t->setUseAnalysisCache(false);
    // A monologue with no pause: 80 long words make one paragraph (the most one takes), then paragraphs of two words.
    // "zebra" is word 70, low in the long paragraph.
    const std::string body = "A" + std::string(149, 'a');
    Timeline tl("V1 tgt |" + body + "|\nA1 tgt |" + body + "|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    std::vector<W> words;
    for (int i = 0; i < 120; ++i) {
        const std::string text = i == 70 ? std::string("zebra") : "monologue" + std::to_string(i) + std::string(36, 'o');
        words.push_back(W{text, static_cast<double>(i), i + 0.8, i < 80 ? 0 : (i / 2) % 2 + 1});
    }
    t->addTranscript(tl.media('A'), 0, transcript("en", words));
    REQUIRE(t->wordCount() == 120);
    REQUIRE(t->paragraphOfWord(79) == 0);
    REQUIRE(t->paragraphOfWord(80) == 1);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull()); // lays the hidden window out
    QQuickItem* list = visibleNamed(shell, QStringLiteral("transcriptList"));
    REQUIRE(list != nullptr);
    QQuickItem* longParagraph = nullptr;
    QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, longParagraph), Q_ARG(int, 0));
    REQUIRE(longParagraph != nullptr);
    INFO("paragraph " << longParagraph->height() << " px, view " << list->height() << " px");
    REQUIRE(longParagraph->height() > list->height() * 1.5); // the case: Contain would top-align it
    const auto wordInView = [&](int word) {
        QQuickItem* w = wordItem(shell, word);
        if (!w)
            return false;
        const double y = w->mapToItem(list, QPointF(0, 0)).y();
        return y >= -1 && y + w->height() <= list->height() + 1;
    };
    const auto contentY = [&] { return list->property("contentY").toDouble(); };

    controller->setPlayhead(f(2));
    spin(50);
    REQUIRE(t->currentWord() == 2);
    CHECK(wordInView(2));
    // Late in the long paragraph: the word's line comes into view (not the paragraph's top), and the next word, shown
    // already, does not move the view.
    controller->setPlayhead(f(76));
    spin(50);
    REQUIRE(t->currentWord() == 76);
    CHECK(wordInView(76));
    const double shown = contentY();
    controller->setPlayhead(f(77));
    spin(50);
    REQUIRE(t->currentWord() == 77);
    CHECK(wordInView(77));
    CHECK(contentY() == shown);
    // Back near the start: the top lines again.
    controller->setPlayhead(f(1));
    spin(50);
    CHECK(wordInView(1));

    // Find (follow off) shows a match low in the long paragraph.
    QQuickItem* toggle = visibleNamed(shell, QStringLiteral("transcriptFollowToggle"));
    REQUIRE(toggle != nullptr);
    clickItem(shell, toggle);
    REQUIRE_FALSE(shell.preferences->property("transcriptFollowPlayhead").toBool());
    t->setSearchQuery(QStringLiteral("zebra"));
    REQUIRE(t->matchCount() == 1);
    t->nextMatch(1);
    spin(50);
    CHECK(t->isSelected(70));
    CHECK(wordInView(70));

    // The Menu key opens the word menu at a selected word low in the paragraph, scrolled into view.
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
    spin(30);
    t->selectWords(78, 78);
    QQuickItem* text = visibleNamed(shell, QStringLiteral("transcriptText"));
    REQUIRE(text != nullptr);
    text->forceActiveFocus(Qt::OtherFocusReason);
    sendKey(shell, Qt::Key_Menu);
    QQuickItem* copyRow = nullptr;
    REQUIRE(waitFor([&] { return (copyRow = visibleNamed(shell, QStringLiteral("transcriptCopyItem"))) != nullptr; }, 3000));
    CHECK(wordInView(78));
    QObject* menu = copyRow->property("menu").value<QObject*>();
    REQUIRE(menu != nullptr);
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    spin(100);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Transcript context menu in the shell: right-click opens it and Ctrl+C copies the words; screenshot",
          "[app][transcript][clipboard][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(t != nullptr);
    t->setUseAnalysisCache(false);
    const Rate rate = controller->currentSequence()->frameRate;
    const double fps = rate.toDouble();
    const auto s = [&](double seconds) { return seconds * fps; };
    t->addTranscript(mediaNamed(*controller, "Interview A.mov"), 0,
                     transcript("en",
                                {{"So", s(0.4), s(0.6), 0},       {"um,", s(0.7), s(1.0), 0},   {"we", s(1.6), s(1.8), 0},
                                 {"started", s(1.8), s(2.3), 0},  {"the", s(2.3), s(2.4), 0},   {"project", s(2.4), s(2.9), 0},
                                 {"last", s(2.9), s(3.2), 0},     {"spring.", s(3.2), s(3.7), 0}, {"It", s(5.2), s(5.3), 1},
                                 {"was", s(5.3), s(5.5), 1},      {"uh", s(5.6), s(5.9), 1},    {"a", s(6.4), s(6.5), 1},
                                 {"small", s(6.5), s(6.9), 1},    {"team", s(6.9), s(7.3), 1},  {"and", s(7.3), s(7.5), 1},
                                 {"a", s(7.5), s(7.6), 1},        {"big", s(7.6), s(7.9), 1},   {"idea.", s(7.9), s(8.5), 1}},
                                rate));
    REQUIRE(t->wordCount() == 18);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull());
    QClipboard* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);
    clipboard->clear();
    const bool canPaste = controller->canPaste();

    // Right-click a selected word: the selection stays and the word menu opens with its rows.
    t->selectWords(5, 7);
    clickItem(shell, wordItem(shell, 6), Qt::RightButton);
    CHECK(t->isSelected(5));
    CHECK(t->isSelected(7));
    QQuickItem* copyRow = visibleNamed(shell, QStringLiteral("transcriptCopyItem"));
    REQUIRE(copyRow != nullptr);
    for (const char* name : {"transcriptCopyTimecodesItem", "transcriptMarkItem", "transcriptMarkerItem", "transcriptChapterItem",
                             "transcriptLiftItem", "transcriptDeleteItem", "transcriptKeepItem", "transcriptFixItem",
                             "transcriptFindAllItem"}) {
        INFO(name);
        QQuickItem* row = visibleNamed(shell, QString::fromLatin1(name));
        REQUIRE(row != nullptr);
        CHECK(row->isEnabled());
    }
    spin(100);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/transcript-context-menu.png")));
        QQuickItem* panel = shell.item("transcriptPanel");
        REQUIRE(panel != nullptr);
        saveUnionCrop(image, panel, copyRow, QStringLiteral("/transcript-context-menu-crop.png"));
    }
    // Mark In and Out from the menu: one undo step; the selection stays.
    const TimeRange first = t->sequenceTranscript()[5].timeline;
    REQUIRE(QMetaObject::invokeMethod(visibleNamed(shell, QStringLiteral("transcriptMarkItem")), "triggered"));
    CHECK(controller->undoText() == QStringLiteral("Undo Mark In and Out"));
    REQUIRE(controller->markIn().has_value());
    CHECK(*controller->markIn() <= first.start);
    CHECK(t->hasSelection());
    REQUIRE(controller->undo());
    QObject* menu = copyRow->property("menu").value<QObject*>();
    REQUIRE(menu != nullptr);
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    spin(100);

    // Right-click a word outside the selection: it alone is selected.
    clickItem(shell, wordItem(shell, 12), Qt::RightButton);
    CHECK(t->isSelected(12));
    CHECK_FALSE(t->isSelected(5));
    CHECK(menu->property("visible").toBool());
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    spin(100);

    // Ctrl+C on the focused words copies them (not the clips); the Menu key opens the menu from the keyboard.
    t->selectWords(5, 7);
    QQuickItem* text = visibleNamed(shell, QStringLiteral("transcriptText"));
    REQUIRE(text != nullptr);
    text->forceActiveFocus(Qt::OtherFocusReason);
    sendKey(shell, Qt::Key_C, Qt::ControlModifier, QStringLiteral("c"));
    CHECK(clipboard->text() == QStringLiteral("project last spring."));
    CHECK(controller->canPaste() == canPaste);
    sendKey(shell, Qt::Key_Menu);
    REQUIRE(waitFor([&] { return menu->property("visible").toBool(); }, 3000));
    // Fix… runs once the menu has closed: the fix row with the words.
    REQUIRE(QMetaObject::invokeMethod(visibleNamed(shell, QStringLiteral("transcriptFixItem")), "triggered"));
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    QQuickItem* fixRow = nullptr;
    REQUIRE(waitFor([&] { return (fixRow = visibleNamed(shell, QStringLiteral("transcriptFixRow"))) != nullptr; }, 3000));
    QQuickItem* fixField = visibleNamed(shell, QStringLiteral("transcriptFixField"));
    REQUIRE(fixField != nullptr);
    CHECK(fixField->property("text").toString() == QStringLiteral("project last spring."));

    // Find All runs once the menu has closed: the words in the search field as shown, so they are found.
    text->forceActiveFocus(Qt::OtherFocusReason);
    sendKey(shell, Qt::Key_Menu);
    REQUIRE(waitFor([&] { return menu->property("visible").toBool(); }, 3000));
    REQUIRE(QMetaObject::invokeMethod(visibleNamed(shell, QStringLiteral("transcriptFindAllItem")), "triggered"));
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return t->searchQuery() == QStringLiteral("project last spring."); }, 3000));
    CHECK(t->matchCount() == 1);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Transcription setup in the shell: the Tracks checklist and its summary; screenshot",
          "[app][transcript][tracks][qml][screenshots]") {
    resetSettings();
    QSettings().remove(QStringLiteral("transcript"));
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(t != nullptr);
    t->setUseAnalysisCache(false);
    // An OBS-style recording: A1 "Game", A2 "Mic" (dialogue), A3 "Discord" (dialogue, voice chat).
    Timeline tl(R"(
        V1 tgt |Gggggggggggggggggggg|
        A1 tgt |Gggggggggggggggggggg|
        A2 tgt |Mmmmmmmmmmmmmmmmmmmm|
        A3 tgt |Dddddddddddddddddddd|
    )");
    tl.link("V1@0 A1@0 A2@0 A3@0");
    ProjectState state = tl.state;
    const auto name = [&](std::size_t track, std::string text, std::optional<TrackRole> role) {
        edit::TrackStateChange change;
        change.name = std::move(text);
        change.role = role;
        state = ok(edit::setTrackState(state, tl.seq, state.findSequence(tl.seq)->audioTracks[track]->id, change)).state;
    };
    name(0, "Game", std::nullopt);
    name(1, "Mic", TrackRole::Dialogue);
    name(2, "Discord", TrackRole::Dialogue);
    controller->resetProject(state, tl.seq);
    t->setTranscriber(QStringLiteral("fake"), QStringLiteral("This computer (test)"),
                      [](const std::string&, int, const std::function<void(float)>&, std::stop_token) {
                          return std::expected<ai::Transcript, Error>(*transcript("en", {{"Hi", 0, 4}}));
                      });
    t->setProvider(QStringLiteral("fake"));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull());

    // One switch a track; the mic alone is on; the summary and the hint say so.
    QQuickItem* game = visibleNamed(shell, QStringLiteral("transcribeTrack_0"));
    QQuickItem* mic = visibleNamed(shell, QStringLiteral("transcribeTrack_1"));
    QQuickItem* discord = visibleNamed(shell, QStringLiteral("transcribeTrack_2"));
    REQUIRE(game != nullptr);
    REQUIRE(mic != nullptr);
    REQUIRE(discord != nullptr);
    CHECK(game->property("text").toString().startsWith(QStringLiteral("A1")));
    CHECK(mic->property("text").toString().contains(QStringLiteral("Mic")));
    CHECK_FALSE(game->property("checked").toBool());
    CHECK(mic->property("checked").toBool());
    CHECK_FALSE(discord->property("checked").toBool());
    QQuickItem* summary = visibleNamed(shell, QStringLiteral("transcribeTracksSummary"));
    REQUIRE(summary != nullptr);
    CHECK(summary->property("text").toString() == QStringLiteral("1 of 3 audio tracks"));
    CHECK(visibleNamed(shell, QStringLiteral("transcribeTracksHint")) != nullptr);
    QQuickItem* transcribe = visibleNamed(shell, QStringLiteral("transcribeSequenceButton"));
    REQUIRE(transcribe != nullptr);
    CHECK(transcribe->isEnabled());
    // The Tracks come after the transcribers: the setup scrolls to them (the panel is short in this layout).
    QQuickItem* setup = visibleNamed(shell, QStringLiteral("transcribeSetup"));
    QQuickItem* label = visibleNamed(shell, QStringLiteral("transcribeTracksLabel"));
    REQUIRE(setup != nullptr);
    REQUIRE(label != nullptr);
    QQuickItem* setupContent = setup->property("contentItem").value<QQuickItem*>();
    REQUIRE(setupContent != nullptr);
    const double most = std::max(0.0, setup->property("contentHeight").toDouble() - setup->height());
    setup->setProperty("contentY", std::clamp(label->mapToItem(setupContent, QPointF(0, 0)).y() - 8.0, 0.0, most));
    spin(100);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/transcript-tracks-setup.png")));
        QQuickItem* panel = shell.item("transcriptPanel");
        REQUIRE(panel != nullptr);
        saveUnionCrop(image, panel, nullptr, QStringLiteral("/transcript-tracks-setup-crop.png"));
    }

    // A click switches the game audio on: remembered, two of three.
    clickItem(shell, game);
    CHECK(t->audioTracks().value(0).toMap().value(QStringLiteral("chosen")).toBool());
    CHECK(game->property("checked").toBool());
    CHECK(summary->property("text").toString() == QStringLiteral("2 of 3 audio tracks"));
    CHECK(QSettings().value(QStringLiteral("transcript/trackChoices")).toString().contains(QStringLiteral("game")));
    // None chosen: Transcribe waits for a track.
    t->setTrackChosen(0, false);
    t->setTrackChosen(1, false);
    spin(30);
    CHECK(summary->property("text").toString() == QStringLiteral("0 of 3 audio tracks"));
    QQuickItem* hint = visibleNamed(shell, QStringLiteral("transcribeTracksHint"));
    REQUIRE(hint != nullptr);
    CHECK(hint->property("text").toString() == QStringLiteral("Choose at least one audio track"));
    CHECK_FALSE(transcribe->isEnabled());

    QSettings().remove(QStringLiteral("transcript"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
