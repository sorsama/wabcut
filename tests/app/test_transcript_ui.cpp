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

#include <QDir>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTextLayout>

#include <fstream>
#include <functional>

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
