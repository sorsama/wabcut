// 0.2 graphics in the app, offscreen: spell check (Spelling, SpellHighlighter, SpellingMenu, the Captions page's Check
// Spelling: Change, Change All, Ignore All, Add to Dictionary) and Find and Replace's Find Next / Replace (one match).
// The checker is a word list put under the app's PersonalSpelling, so the results do not depend on the machine's
// dictionaries; the dictionary file is a temporary one.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "CaptionsBridge.h"
#include "ProjectController.h"
#include "SpellingBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/editor/SystemSpellChecker.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/Spelling.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QQmlComponent>
#include <QQuickItem>
#include <QTemporaryDir>

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

/// The app's spelling over a word list (restored to the system checker when it goes).
struct WordListSpelling {
    WordListSpelling() {
        sharedSpelling()->setBase(std::make_shared<WordListSpellChecker>(
            std::vector<std::string>{"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "colour"}));
        emit SpellingNotifier::instance().dictionaryChanged();
    }
    ~WordListSpelling() {
        for (const std::string& w : sharedSpelling()->words())
            sharedSpelling()->removeWord(w);
        sharedSpelling()->setBase(editor::systemSpellChecker());
    }
};

struct Fixture {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    ProjectController controller;
    TranscriptBridge transcriptBridge;
    CaptionsBridge captions;
    CaptionTrackId track;

    Fixture() {
        tl.linkMatching();
        ProjectState s = tl.state;
        auto added = edit::addCaptionTrack(s, tl.seq, "English", "en", CaptionStyle{});
        REQUIRE(added);
        s = added->state;
        track = added->createdCaptionTrack;
        const auto cue = [&](int from, int to, std::string text) {
            auto r = edit::addCaptionCue(s, tl.seq, track,
                                         CaptionCue{framesToTicks(from, kRate), framesToTicks(to - from, kRate), std::move(text), {}});
            REQUIRE(r);
            s = r->state;
        };
        cue(0, 10, "the quick brwn fox");
        cue(10, 20, "jumsp over the lazzy dog");
        cue(20, 30, "brwn brwn colour");
        controller.resetProject(s, tl.seq);
        transcriptBridge.setUseAnalysisCache(false);
        transcriptBridge.setController(&controller);
        captions.setController(&controller);
        captions.setTranscript(&transcriptBridge);
    }
    std::string cueText(std::size_t i) const {
        for (const CaptionTrack& t : controller.snapshot()->findSequence(tl.seq)->captionTracks)
            if (t.id == track)
                return t.cues.at(i).text;
        return {};
    }
};

} // namespace

TEST_CASE("Spelling: misspelled words, suggestions and the user's dictionary", "[app][spelling]") {
    QTemporaryDir dir;
    qputenv("WABCUT_SPELLING_DICTIONARY", QDir(dir.path()).filePath(QStringLiteral("dictionary.txt")).toUtf8());
    WordListSpelling words;
    SpellingBridge spelling;
    // UTF-16 positions: "é" is one unit, the emoji two.
    const QString text = QStringLiteral("é \U0001F600 the quik fox");
    const QVariantList issues = spelling.check(text);
    REQUIRE(issues.size() == 2); // é is a word too; quik
    const QVariantMap quik = issues[1].toMap();
    CHECK(quik.value(QStringLiteral("word")).toString() == QStringLiteral("quik"));
    CHECK(quik.value(QStringLiteral("start")).toInt() == text.indexOf(QStringLiteral("quik")));
    CHECK(quik.value(QStringLiteral("end")).toInt() == text.indexOf(QStringLiteral("quik")) + 4);
    const QVariantMap at = spelling.issueAt(text, text.indexOf(QStringLiteral("quik")) + 2);
    CHECK(at.value(QStringLiteral("suggestions")).toStringList().value(0) == QStringLiteral("quick"));
    CHECK(spelling.issueAt(text, text.indexOf(QStringLiteral("fox"))).isEmpty());
    CHECK(spelling.corrected(QStringLiteral("a quik b"), 2, 6, QStringLiteral("quick")) == QStringLiteral("a quick b"));
    CHECK(spelling.corrected(QStringLiteral("the the end"), 4, 7, QString()) == QStringLiteral("the end"));
    // Add to Dictionary and Ignore All.
    int changes = 0;
    QObject::connect(&spelling, &SpellingBridge::changed, [&] { ++changes; });
    CHECK(spelling.addWord(QStringLiteral("quik")));
    CHECK(changes == 1);
    CHECK(spelling.words() == QStringList{QStringLiteral("quik")});
    spelling.ignoreWord(QStringLiteral("é"));
    CHECK(spelling.check(text).isEmpty());
    qunsetenv("WABCUT_SPELLING_DICTIONARY");
}

TEST_CASE("SpellHighlighter underlines misspelled words and follows the dictionary", "[app][spelling]") {
    WordListSpelling words;
    Shell shell;
    QQmlComponent component(&shell.engine);
    component.setData("import QtQuick\nimport Wabcut\nTextEdit {\n id: edit\n width: 400; height: 100\n text: \"the brwn fox jumsp\"\n"
                      " SpellHighlighter { objectName: \"hl\"; document: edit.textDocument; color: \"red\" }\n}\n",
                      QUrl(QStringLiteral("qrc:/spelling/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* highlighter = object->findChild<SpellHighlighter*>(QStringLiteral("hl"));
    REQUIRE(highlighter);
    CHECK(highlighter->issueCount() == 2);
    object->setProperty("text", QStringLiteral("the brown fox jumps"));
    CHECK(highlighter->issueCount() == 0);
    object->setProperty("text", QStringLiteral("brwn"));
    CHECK(highlighter->issueCount() == 1);
    SpellingBridge spelling;
    spelling.ignoreWord(QStringLiteral("brwn"));
    CHECK(highlighter->issueCount() == 0); // the dictionary changed: checked again
    highlighter->setActive(false);
    object->setProperty("text", QStringLiteral("zzzq"));
    CHECK(highlighter->issueCount() == 0);

    // The caption cue editor underlines through its own highlighter.
    QQmlComponent row(&shell.engine);
    row.setData("import QtQuick\nimport Wabcut\nCaptionCueRow {\n width: 400\n}\n", QUrl(QStringLiteral("qrc:/spelling/Row.qml")));
    const QVariantMap cue{{QStringLiteral("index"), 0},          {QStringLiteral("text"), QStringLiteral("a lazzy dog")},
                          {QStringLiteral("startText"), QString()}, {QStringLiteral("endText"), QString()},
                          {QStringLiteral("speaker"), QString()},   {QStringLiteral("speakerName"), QString()},
                          {QStringLiteral("speakerColor"), QString()}, {QStringLiteral("cps"), 0},
                          {QStringLiteral("fast"), false}};
    std::unique_ptr<QObject> cueRow(row.createWithInitialProperties({{QStringLiteral("modelData"), cue}, {QStringLiteral("index"), 0}}));
    INFO(row.errorString().toStdString());
    REQUIRE(cueRow);
    auto* cueHighlighter = cueRow->findChild<SpellHighlighter*>(QStringLiteral("captionSpelling"));
    REQUIRE(cueHighlighter);
    CHECK(cueHighlighter->issueCount() == 2); // "a" and "lazzy" are not in the word list
}

TEST_CASE("Check Spelling in captions: Change, Change All and Add to Dictionary are one undo step each", "[app][spelling][captions]") {
    WordListSpelling words;
    Fixture fx;
    QVariantList issues = fx.captions.spellingIssues();
    REQUIRE(issues.size() == 5); // brwn, jumsp, lazzy, brwn, brwn
    QVariantMap first = issues[0].toMap();
    CHECK(first.value(QStringLiteral("cue")).toInt() == 0);
    CHECK(first.value(QStringLiteral("word")).toString() == QStringLiteral("brwn"));
    CHECK(first.value(QStringLiteral("suggestions")).toStringList().value(0) == QStringLiteral("brown"));

    // Change: this one only.
    auto before = fx.controller.session().undoCount();
    CHECK(fx.captions.changeWord(0, first.value(QStringLiteral("start")).toInt(), first.value(QStringLiteral("end")).toInt(),
                                 QStringLiteral("brown")) == QString());
    CHECK(fx.cueText(0) == "the quick brown fox");
    CHECK(fx.controller.session().undoCount() == before + 1);
    // Change All: every "brwn" of the track in one step.
    before = fx.controller.session().undoCount();
    CHECK(fx.captions.changeAll(QStringLiteral("brwn"), QStringLiteral("brown")) == QString());
    CHECK(fx.cueText(2) == "brown brown colour");
    CHECK(fx.controller.session().undoCount() == before + 1);
    // Add to Dictionary: no edit, and the word is right from now on.
    SpellingBridge spelling;
    issues = fx.captions.spellingIssues();
    REQUIRE(issues.size() == 2);
    CHECK(spelling.addWord(QStringLiteral("lazzy")));
    issues = fx.captions.spellingIssues();
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].toMap().value(QStringLiteral("word")).toString() == QStringLiteral("jumsp"));
    CHECK(fx.controller.session().undoCount() == before + 1);
}

TEST_CASE("Find and Replace: Find Next lists matches, Replace changes one and moves on", "[app][captions][findReplace]") {
    Fixture fx;
    const QVariantList matches = fx.captions.findMatches(QStringLiteral("BRWN"), false, true, QStringLiteral("track"));
    REQUIRE(matches.size() == 3);
    const QVariantMap second = matches[1].toMap();
    CHECK(second.value(QStringLiteral("cue")).toInt() == 2);
    CHECK(second.value(QStringLiteral("start")).toInt() == 0);
    CHECK(second.value(QStringLiteral("end")).toInt() == 4);
    CHECK(second.value(QStringLiteral("trackKey")).toString() == fx.captions.track());
    const auto before = fx.controller.session().undoCount();
    CHECK(fx.captions.replaceMatch(QStringLiteral("brwn"), QStringLiteral("brown"), false, true, QStringLiteral("track"), second) ==
          QString());
    CHECK(fx.cueText(2) == "brown brwn colour");
    CHECK(fx.cueText(0) == "the quick brwn fox");
    CHECK(fx.controller.session().undoCount() == before + 1);
    CHECK(fx.captions.findMatches(QStringLiteral("brwn"), false, true, QStringLiteral("track")).size() == 2);
    // A stale match is refused with a message.
    CHECK(fx.captions.replaceMatch(QStringLiteral("brwn"), QStringLiteral("brown"), false, true, QStringLiteral("track"), second) !=
          QString());
}
