// 0.6 (P1 round, #37): Correct the transcript from your script, in the app. Sequence > Captions > Correct Transcript
// from Script... (Main's runCommand) opens the sheet once the sequence has a transcript; Find corrections runs
// transcript.correctFromScript as a dry run (the worker path, synchronous here) and lists "time  before -> after" with
// the capitals fixes tagged, the words the speaker changed ("Left as said") and the lines not found; a listed change
// moves the playhead there; Apply makes them as ONE undo step and offers Update captions, which rewrites the caption
// track from the corrected transcript. Screenshots (dark and light): <build>/test-output/ui/script-correction-sheet.png
// and script-correction-sheet-light.png. Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "CaptionsBridge.h"
#include "ProjectController.h"
#include "ScriptCorrectionBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/edit/TranscriptCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QQuickItem>
#include <QUrl>
#include <QVariant>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

struct SpokenWord {
    std::string text;
    double from = 0; ///< seconds of the media
    double to = 0;
};

/// Lays out the words of `text` from `start` seconds (0.32 s each, 0.08 s apart); returns the time after the last.
double say(std::vector<SpokenWord>& words, double start, std::string_view text) {
    double t = start;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t space = text.find(' ', i);
        const std::size_t end = space == std::string_view::npos ? text.size() : space;
        if (end > i) {
            words.push_back(SpokenWord{std::string(text.substr(i, end - i)), t, t + 0.32});
            t += 0.40;
        }
        i = end + 1;
    }
    return t;
}

std::shared_ptr<const ai::Transcript> transcriptOf(const std::vector<SpokenWord>& words) {
    auto t = std::make_shared<ai::Transcript>();
    t->language = "en";
    for (const SpokenWord& w : words) {
        ai::TranscriptWord tw;
        tw.text = w.text;
        tw.t0 = Ticks{static_cast<std::int64_t>(w.from * static_cast<double>(kTicksPerSecond))};
        tw.t1 = Ticks{static_cast<std::int64_t>(w.to * static_cast<double>(kTicksPerSecond))};
        tw.confidence = 0.9f;
        t->words.push_back(std::move(tw));
    }
    return t;
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

void saveItemShot(const QImage& image, const QQuickItem* item, const QString& name) {
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

constexpr const char* kScript = "Today we test Wabcut with Dr. Somchai in our Bangkok studio.\n"
                                "It runs on Kubernetes, and it is a good tool for editors.\n"
                                "This line was never recorded at all.\n";

} // namespace

TEST_CASE("Correct from script sheet finds and applies corrections in one undo step and updates captions; screenshots",
          "[app][captions][transcript][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            INFO(join(shell.errors));
            REQUIRE(shell.window);
            shell.resize(1600, 1000);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* transcript = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
            auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
            auto* corrections = shell.engine.singletonInstance<ScriptCorrectionBridge*>("Wabcut", "ScriptCorrection");
            REQUIRE(controller);
            REQUIRE(transcript);
            REQUIRE(captions);
            REQUIRE(corrections);
            corrections->setSynchronous(true);

            // 30 s of an interview read from a script (A/V linked).
            const std::string clip = "A" + std::string(749, 'a');
            Timeline tl("V1 tgt |" + clip + "|\nA1 tgt |" + clip + "|\n", std::map<char, std::int64_t>{{'A', 850}});
            tl.linkMatching();
            transcript->setUseAnalysisCache(false);
            controller->resetProject(tl.state, tl.seq);
            spin(50);
            // No transcript yet: the menu row says why.
            const QVariantMap before = captions->commandAvailability(QStringLiteral("transcript.correctFromScript"));
            CHECK(before.value(QStringLiteral("handled")).toBool());
            CHECK_FALSE(before.value(QStringLiteral("enabled")).toBool());
            CHECK_FALSE(before.value(QStringLiteral("reason")).toString().isEmpty());

            std::vector<SpokenWord> words;
            double t = say(words, 1.0, "So today we test web cut with Doctor Somchay in our Bangkok studio.");
            t = say(words, t + 1.0, "It runs on kubernetes and it is a great tool for editors.");
            say(words, t + 1.0, "Thanks for watching and see you next time.");
            transcript->addTranscript(tl.media('A'), 0, transcriptOf(words));
            spin(50);
            REQUIRE(transcript->wordCount() == static_cast<int>(words.size()));
            CHECK(captions->commandAvailability(QStringLiteral("transcript.correctFromScript")).value(QStringLiteral("enabled")).toBool());
            // Captions made before the corrections (they read "web cut" until updated).
            REQUIRE(captions->generate().isEmpty());
            REQUIRE_FALSE(captions->track().isEmpty());
            const std::size_t undoBefore = controller->session().undoCount();

            // Sequence > Captions > Correct Transcript from Script...
            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("transcript.correctFromScript"))));
            QObject* loader = shell.object("scriptCorrectionSheetLoader");
            REQUIRE(loader);
            REQUIRE(waitFor([&] { return loader->property("item").value<QObject*>() != nullptr; }));
            QObject* sheet = loader->property("item").value<QObject*>();
            REQUIRE(waitFor([&] { return sheet->property("opened").toBool(); }));
            auto* script = sheet->findChild<QObject*>(QStringLiteral("scriptCorrectionText"));
            REQUIRE(script);
            // A failed file read shows its own reason, never an earlier refusal left on the sheet (QA ARGON-15):
            // pick a file over the 256 KB limit while a find's refusal is showing, then accept the file dialog.
            {
                const QString big = QDir::temp().filePath(QStringLiteral("wabcut-big-script.txt"));
                {
                    QFile oversized(big);
                    REQUIRE(oversized.open(QIODevice::WriteOnly | QIODevice::Truncate));
                    oversized.write(QByteArray(300 * 1024, 'a'));
                }
                sheet->setProperty("refusal", QStringLiteral("An earlier refusal"));
                QObject* fileDialog = sheet->findChild<QObject*>(QStringLiteral("scriptCorrectionFileDialog"));
                REQUIRE(fileDialog);
                fileDialog->setProperty("selectedFile", QUrl::fromLocalFile(big));
                REQUIRE(QMetaObject::invokeMethod(fileDialog, "accepted"));
                QObject* error = sheet->findChild<QObject*>(QStringLiteral("scriptCorrectionError"));
                REQUIRE(error);
                CHECK(error->property("text").toString() == QStringLiteral("The file cannot be read or is larger than 256 KB"));
                QFile::remove(big);
            }
            script->setProperty("text", QString::fromUtf8(kScript));
            REQUIRE(QMetaObject::invokeMethod(sheet, "findCorrections"));
            REQUIRE(corrections->waitForIdle(20'000));
            INFO(corrections->message().toStdString());
            const QVariantMap result = corrections->result();
            CHECK(result.value(QStringLiteral("corrected")).toInt() == 3);
            CHECK(result.value(QStringLiteral("caseFixes")).toInt() == 1);
            CHECK(result.value(QStringLiteral("beatsMatched")).toInt() == 2);
            CHECK(result.value(QStringLiteral("beatsSkipped")).toInt() == 1);
            CHECK(result.value(QStringLiteral("keptCount")).toInt() == 1);
            const QVariantList changes = result.value(QStringLiteral("changes")).toList();
            REQUIRE(changes.size() == 3);
            CHECK(changes[0].toMap().value(QStringLiteral("before")).toString() == QStringLiteral("web cut"));
            CHECK(changes[0].toMap().value(QStringLiteral("after")).toString() == QStringLiteral("Wabcut"));
            CHECK(changes[1].toMap().value(QStringLiteral("before")).toString() == QStringLiteral("Doctor Somchay"));
            CHECK(changes[1].toMap().value(QStringLiteral("after")).toString() == QStringLiteral("Dr. Somchai"));
            CHECK(changes[2].toMap().value(QStringLiteral("after")).toString() == QStringLiteral("Kubernetes"));
            CHECK(changes[2].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("case"));
            CHECK_FALSE(changes[0].toMap().contains(QStringLiteral("ticks"))); // QML gets timecodes only
            const QVariantList kept = result.value(QStringLiteral("kept")).toList();
            REQUIRE(kept.size() == 1);
            CHECK(kept[0].toMap().value(QStringLiteral("said")).toString() == QStringLiteral("great"));
            CHECK(kept[0].toMap().value(QStringLiteral("script")).toString() == QStringLiteral("good"));
            CHECK(corrections->staged());
            CHECK(controller->session().undoCount() == undoBefore); // a dry run: nothing changed yet

            auto* popup = qobject_cast<QQuickItem*>(sheet->property("background").value<QObject*>());
            REQUIRE(popup);
            QQuickItem* list = findNamed(popup->parentItem(), QStringLiteral("scriptCorrectionChanges"));
            REQUIRE(list);
            REQUIRE(waitFor([&] { return list->property("count").toInt() == 3; }));
            QQuickItem* keptBlock = findNamed(popup->parentItem(), QStringLiteral("scriptCorrectionKept"));
            REQUIRE(keptBlock);
            CHECK(keptBlock->isVisible());
            QQuickItem* skippedNote = findNamed(popup->parentItem(), QStringLiteral("scriptCorrectionSkipped"));
            REQUIRE(skippedNote);
            CHECK(skippedNote->isVisible());
            QQuickItem* apply = findNamed(popup->parentItem(), QStringLiteral("scriptCorrectionApply"));
            REQUIRE(apply);
            CHECK(apply->property("enabled").toBool());

            // A listed change moves the playhead to its words ("web cut", words 4..5).
            const ai::edit::SequenceTranscript& st = transcript->sequenceTranscript();
            REQUIRE(st.text(4) == "web");
            corrections->reveal(0, false);
            CHECK(controller->playhead() >= st[4].timeline.start);
            CHECK(controller->playhead() < st[4].timeline.end());

            spin(400);
            {
                const QImage image = shell.grab();
                saveItemShot(image, popup, dark ? QStringLiteral("script-correction-sheet.png") : QStringLiteral("script-correction-sheet-light.png"));
            }

            // Apply: one undo step; the transcript reads the script's words, the speaker's own stay.
            REQUIRE(QMetaObject::invokeMethod(apply, "clicked"));
            CHECK(corrections->applied() == 3);
            CHECK(controller->session().undoCount() == undoBefore + 1);
            const ai::edit::SequenceTranscript& now = transcript->sequenceTranscript();
            const std::string text = now.textOf(0, now.size() - 1);
            INFO(text);
            CHECK(text.find("So today we test Wabcut with Dr. Somchai in our Bangkok studio.") != std::string::npos);
            CHECK(text.find("It runs on Kubernetes and it is a great tool for editors.") != std::string::npos);
            CHECK(captions->updateSuggested());
            QQuickItem* applied = findNamed(popup->parentItem(), QStringLiteral("scriptCorrectionApplied"));
            REQUIRE(applied);
            REQUIRE(waitFor([&] { return applied->isVisible(); }));
            CHECK_FALSE(apply->property("enabled").toBool());

            // Update captions: the cues read the corrections (their timing kept), and the sheet closes.
            QQuickItem* update = findNamed(popup->parentItem(), QStringLiteral("scriptCorrectionUpdateCaptions"));
            REQUIRE(update);
            CHECK(update->isVisible());
            REQUIRE(QMetaObject::invokeMethod(update, "clicked"));
            REQUIRE(waitFor([&] { return !sheet->property("opened").toBool(); }));
            QString cueText;
            for (const QVariant& cue : captions->cues())
                cueText += cue.toMap().value(QStringLiteral("text")).toString() + QLatin1Char(' ');
            cueText = cueText.simplified(); // cues wrap lines
            INFO(cueText.toStdString());
            CHECK(cueText.contains(QStringLiteral("Wabcut")));
            CHECK(cueText.contains(QStringLiteral("Dr. Somchai")));
            CHECK_FALSE(cueText.contains(QStringLiteral("web cut")));

            // Undo the caption update and the corrections: the transcript reads as transcribed again.
            REQUIRE(controller->session().undoCount() == undoBefore + 2);
            REQUIRE(controller->session().undo());
            REQUIRE(controller->session().undo());
            spin(50);
            const ai::edit::SequenceTranscript& undone = transcript->sequenceTranscript();
            CHECK(undone.textOf(0, undone.size() - 1).find("we test web cut with Doctor Somchay") != std::string::npos);

            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}

TEST_CASE("Correct from script finds on a worker and drops a stale find", "[app][captions][transcript]") {
    const std::string clip = "A" + std::string(249, 'a');
    Timeline tl("V1 tgt |" + clip + "|\nA1 tgt |" + clip + "|\n");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    TranscriptBridge transcript;
    transcript.setUseAnalysisCache(false);
    transcript.setController(&controller);
    std::vector<SpokenWord> words;
    say(words, 1.0, "So today we test web cut with Doctor Somchay in our Bangkok studio.");
    transcript.addTranscript(tl.media('A'), 0, transcriptOf(words));
    REQUIRE(transcript.wordCount() == static_cast<int>(words.size()));
    ScriptCorrectionBridge corrections;
    corrections.setController(&controller);
    corrections.setServices(transcript.services());
    const QString script = QStringLiteral("Today we test Wabcut with Dr. Somchai in our Bangkok studio.");

    // On a worker: busy until its result reaches the GUI thread; nothing changes until Apply.
    CHECK(corrections.find(script).isEmpty());
    CHECK(corrections.busy());
    REQUIRE(corrections.waitForIdle(20'000));
    CHECK(corrections.result().value(QStringLiteral("corrected")).toInt() == 2);
    CHECK(corrections.staged());
    CHECK(controller.session().undoCount() == 0);

    // The script changed (or the sheet closed) while a find ran: its result is dropped.
    CHECK(corrections.find(QStringLiteral("Today we test Wabcut with Dr. Somchai.")).isEmpty());
    corrections.discard();
    REQUIRE(corrections.waitForIdle(20'000));
    CHECK(corrections.result().isEmpty());
    CHECK_FALSE(corrections.staged());

    // Refusals before anything runs.
    CHECK_FALSE(corrections.find(QStringLiteral("   ")).isEmpty());
    CHECK_FALSE(corrections.apply().isEmpty());

    // The project changed after the find: Apply finds them again instead of applying a stale list.
    CHECK(corrections.find(script).isEmpty());
    REQUIRE(corrections.waitForIdle(20'000));
    REQUIRE(corrections.staged());
    ProjectState renamed = *controller.snapshot();
    auto sequence = std::make_shared<Sequence>(*renamed.findSequence(tl.seq));
    sequence->name = "Renamed";
    renamed = renamed.withSequence(sequence);
    REQUIRE(controller.session().commit("Rename", renamed).has_value());
    CHECK_FALSE(corrections.apply().isEmpty());
    CHECK(controller.session().undoCount() == 1); // only the rename
    REQUIRE(corrections.waitForIdle(20'000));
    REQUIRE(corrections.staged());
    CHECK(corrections.apply().isEmpty());
    CHECK(corrections.applied() == 2);
    CHECK(controller.session().undoCount() == 2);
    const ai::edit::SequenceTranscript& now = transcript.sequenceTranscript();
    CHECK(now.textOf(0, now.size() - 1) == "So today we test Wabcut with Dr. Somchai in our Bangkok studio.");
}
