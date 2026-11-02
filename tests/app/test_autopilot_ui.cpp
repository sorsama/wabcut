// 0.6 (P1 round, 3A, #62): the Creator autopilot sheet in the app, offscreen. Sequence > Creator Autopilot... (Main's
// runCommand) opens it; on fixtures (transcripts, library items with names, a Description and a clip marker, a reference
// video's shots in memory, drawn frames and a fake still writer; the bridge's synchronous switch stands in for the
// worker) Find B-roll stages three suggestions on a new track, one can be switched off and on, Apply is ONE undo step;
// Prepare kit lists the title, chapters from the markers, tags and thumbnails, Write files writes them; Analyse measures
// the reference's rhythm and Add as markers is ONE undo step. Screenshots (dark and light) in <build>/test-output/ui/:
// autopilot-broll.png, autopilot-kit.png, autopilot-pacing.png and their -light versions.

#include "AutopilotTestUtil.h"
#include "ShellFixture.h"

#include "AutopilotBridge.h"
#include "ProjectController.h"
#include "ShortsBridge.h"

#include <wabcut/ai/analysis/AnalysisCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QPainter>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::test::autopilotfx;

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

void saveItemShot(const QImage& image, const QQuickItem* item, const QString& name) {
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

/// A drawn shot of each library item: its own colours, a horizon and a sun, so the thumbnails read as pictures.
QImage drawnShot(const MediaItem& media, Ticks time) {
    const std::size_t hash = std::hash<std::string>{}(media.name);
    const QColor sky = QColor::fromHsv(static_cast<int>(hash % 360), 120, 200);
    const QColor ground = QColor::fromHsv(static_cast<int>((hash / 7) % 360), 160, 110);
    QImage image(320, 180, QImage::Format_RGB888);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRect(0, 0, 320, 110), sky);
    p.fillRect(QRect(0, 110, 320, 70), ground);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 236, 170));
    p.drawEllipse(QPointF(60 + std::fmod(time.toSeconds() * 9.0, 200.0), 60), 18, 18);
    return image;
}

/// A still writer that saves a real JPEG (the sheet shows the written thumbnails) and remembers where it was asked for.
std::shared_ptr<command::FrameExportServices> jpegStills(std::shared_ptr<std::vector<Ticks>> asked) {
    return std::make_shared<command::FrameExportServices>(
        [asked](const ProjectState&, SequenceId, Ticks time, const std::filesystem::path& file, const std::string&,
                const command::FrameExportServices::Options& o) -> std::expected<command::FrameExportServices::Written, Error> {
            asked->push_back(time);
            MediaItem media;
            media.name = "thumbnail";
            const QImage image = drawnShot(media, time).scaled(o.width, o.height);
            if (!image.save(QString::fromStdWString(file.wstring()), "JPG", 90)) {
                return std::unexpected(Error(ErrorCode::IoError, "cannot write"));
            }
            return command::FrameExportServices::Written{file, o.width, o.height, static_cast<std::int64_t>(std::filesystem::file_size(file)), 90};
        },
        command::FrameExportServices::Describer{}, std::vector<std::string>{"jpeg"});
}

} // namespace

TEST_CASE("Creator autopilot sheet previews and applies B-roll and pacing in one undo step each and writes the kit; screenshots",
          "[app][ai][autopilot][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            INFO(join(shell.errors));
            REQUIRE(shell.window);
            shell.resize(1600, 1100);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* autopilot = shell.engine.singletonInstance<AutopilotBridge*>("Wabcut", "Autopilot");
            REQUIRE(controller);
            REQUIRE(autopilot);

            // A minute of a video essay, its library, chapter markers and a reference video cut every 3 s.
            Recording essay = brollEssay();
            essay.describe('R', "Reference trailer.mp4");
            essay.addMarker(0.0, "Intro");
            essay.addMarker(18.0, "The kitchen");
            essay.addMarker(40.0, "The beach");
            auto shots = std::make_shared<ai::analysis::MapAnalysisProvider>();
            shots->addShots(essay.media('R'), ai::search::test::shotsAt(40, {3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36}));
            auto stills = std::make_shared<std::vector<Ticks>>();
            autopilot->setServices(bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts),
                                           std::make_shared<ai::analysis::AnalysisServices>(shots),
                                           // Every frame a usable picture here (the frame rules are the command tests').
                                           std::make_shared<ai::autopilot::AutopilotServices>(
                                               [](const MediaItem& media, Ticks) { return drawnFrame(media, secs(70)); }),
                                           jpegStills(stills)}));
            autopilot->setSynchronous(true);
            setPreviewFramesForTests(drawnShot);
            controller->resetProject(essay.tl.state, essay.tl.seq);
            spin(50);
            const std::size_t undoBefore = controller->session().undoCount();

            // Sequence > Creator Autopilot...
            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.creatorAutopilot"))));
            QObject* loader = shell.object("autopilotSheetLoader");
            REQUIRE(loader);
            REQUIRE(waitFor([&] { return loader->property("item").value<QObject*>() != nullptr; }));
            QObject* sheet = loader->property("item").value<QObject*>();
            REQUIRE(waitFor([&] { return sheet->property("opened").toBool(); }));
            auto* popup = qobject_cast<QQuickItem*>(sheet->property("background").value<QObject*>());
            REQUIRE(popup);

            // ---- B-roll: three suggestions staged on a new track; one off and on again; Apply is one undo step.
            QObject* brollPage = sheet->findChild<QObject*>(QStringLiteral("autopilotBrollPage"));
            REQUIRE(brollPage);
            REQUIRE(QMetaObject::invokeMethod(brollPage, "find"));
            REQUIRE(autopilot->waitForIdle(20'000));
            INFO(autopilot->message().toStdString());
            QVariantList suggestions = autopilot->broll().value(QStringLiteral("suggestions")).toList();
            REQUIRE(suggestions.size() == 3);
            CHECK(autopilot->stagedKind() == QStringLiteral("broll"));
            CHECK(autopilot->broll().value(QStringLiteral("track")).toMap().value(QStringLiteral("label")).toString() == QStringLiteral("V2"));
            CHECK(autopilot->broll().value(QStringLiteral("unmatched")).toList().size() >= 2);
            CHECK(controller->session().undoCount() == undoBefore); // a preview
            QQuickItem* list = findNamed(popup->parentItem(), QStringLiteral("autopilotBrollList"));
            REQUIRE(list);
            REQUIRE(waitFor([&] { return list->property("count").toInt() == 3; }));
            CHECK(autopilot->setBrollChosen(2, false).isEmpty());
            CHECK(autopilot->broll().value(QStringLiteral("placed")).toInt() == 2);
            CHECK(autopilot->stagedKind() == QStringLiteral("broll"));
            CHECK(autopilot->setBrollChosen(2, true).isEmpty());
            CHECK(autopilot->broll().value(QStringLiteral("placed")).toInt() == 3);
            spin(400);
            saveItemShot(shell.grab(), popup, dark ? QStringLiteral("autopilot-broll.png") : QStringLiteral("autopilot-broll-light.png"));
            QQuickItem* apply = findNamed(popup->parentItem(), QStringLiteral("autopilotApplyBroll"));
            REQUIRE(apply);
            CHECK(apply->property("enabled").toBool());
            REQUIRE(QMetaObject::invokeMethod(apply, "clicked"));
            CHECK(controller->session().undoCount() == undoBefore + 1);
            {
                const SequencePtr seq = controller->currentSequence();
                REQUIRE(seq->videoTracks.size() == 2);
                CHECK(seq->videoTracks[1]->clips.size() == 3);
            }
            CHECK(autopilot->broll().value(QStringLiteral("done")).toBool());
            REQUIRE(controller->session().undo());
            CHECK(controller->currentSequence()->videoTracks.size() == 1);

            // ---- Publishing kit: chapters from the markers, thumbnails from usable frames; Write files.
            sheet->setProperty("page", 1);
            QObject* kitPage = sheet->findChild<QObject*>(QStringLiteral("autopilotKitPage"));
            REQUIRE(kitPage);
            REQUIRE(QMetaObject::invokeMethod(kitPage, "prepare"));
            REQUIRE(autopilot->waitForIdle(20'000));
            const QVariantMap kit = autopilot->kit();
            INFO(autopilot->message().toStdString());
            CHECK_FALSE(kit.value(QStringLiteral("title")).toString().isEmpty());
            CHECK(kit.value(QStringLiteral("chaptersText")).toString() == QStringLiteral("0:00 Intro\n0:18 The kitchen\n0:40 The beach\n"));
            CHECK_FALSE(kit.value(QStringLiteral("thumbnails")).toList().isEmpty());
            auto* title = findNamed(popup->parentItem(), QStringLiteral("autopilotKitTitle"));
            REQUIRE(title);
            CHECK(title->property("text").toString() == kit.value(QStringLiteral("title")).toString());
            spin(400);
            saveItemShot(shell.grab(), popup, dark ? QStringLiteral("autopilot-kit.png") : QStringLiteral("autopilot-kit-light.png"));
            const QString folder = QDir(outputDir()).filePath(dark ? QStringLiteral("autopilot-kit-dark") : QStringLiteral("autopilot-kit-light"));
            QDir(folder).removeRecursively();
            REQUIRE(QDir().mkpath(folder));
            kitPage->setProperty("folder", QUrl::fromLocalFile(folder));
            title->setProperty("text", QStringLiteral("I flew a drone over the city at night"));
            REQUIRE(QMetaObject::invokeMethod(kitPage, "write"));
            REQUIRE(autopilot->waitForIdle(20'000));
            CHECK(autopilot->kit().value(QStringLiteral("written")).toBool());
            const QString kitFolder = folder + QStringLiteral("/Telescope build - publishing kit");
            CHECK(QFile::exists(kitFolder + QStringLiteral("/title.txt")));
            CHECK(QFile::exists(kitFolder + QStringLiteral("/description.txt")));
            CHECK(QFile::exists(kitFolder + QStringLiteral("/thumbnail-1.jpg")));
            CHECK(QString::fromStdString(readFile((kitFolder + QStringLiteral("/title.txt")).toStdWString())) == QStringLiteral("I flew a drone over the city at night\n"));
            CHECK(controller->session().undoCount() == undoBefore); // the kit changes nothing in the project

            // ---- Pacing: the reference's rhythm, suggestions staged as markers; Add as markers is one undo step.
            sheet->setProperty("page", 2);
            QObject* pacingPage = sheet->findChild<QObject*>(QStringLiteral("autopilotPacingPage"));
            REQUIRE(pacingPage);
            REQUIRE(QMetaObject::invokeMethod(pacingPage, "refresh"));
            const QVariantList references = pacingPage->property("references").toList();
            int reference = -1;
            for (int i = 0; i < references.size(); ++i) {
                if (references[i].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Reference trailer.mp4"))
                    reference = i;
            }
            REQUIRE(reference >= 0);
            auto* picker = findNamed(popup->parentItem(), QStringLiteral("autopilotPacingReference"));
            REQUIRE(picker);
            pacingPage->setProperty("referenceKey", references[reference].toMap().value(QStringLiteral("key")));
            CHECK(picker->property("text").toString().startsWith(QStringLiteral("Reference trailer.mp4")));
            REQUIRE(QMetaObject::invokeMethod(pacingPage, "analyse"));
            REQUIRE(autopilot->waitForIdle(20'000));
            const QVariantMap pacing = autopilot->pacing();
            INFO(autopilot->message().toStdString());
            const QVariantList cuts = pacing.value(QStringLiteral("suggestions")).toList();
            REQUIRE(cuts.size() >= 10);
            CHECK(pacing.value(QStringLiteral("reference")).toMap().value(QStringLiteral("cuts")).toInt() == 12);
            CHECK(autopilot->stagedKind() == QStringLiteral("pacing"));
            CHECK(autopilot->setPacingChosen(1, false).isEmpty());
            spin(400);
            saveItemShot(shell.grab(), popup, dark ? QStringLiteral("autopilot-pacing.png") : QStringLiteral("autopilot-pacing-light.png"));
            QQuickItem* markers = findNamed(popup->parentItem(), QStringLiteral("autopilotPacingMarkers"));
            REQUIRE(markers);
            REQUIRE(QMetaObject::invokeMethod(markers, "clicked"));
            CHECK(controller->session().undoCount() == undoBefore + 1);
            const SequencePtr marked = controller->currentSequence();
            CHECK(std::ranges::count_if(marked->markers, [](const Marker& m) { return m.name == "Cut (pace)"; }) ==
                  static_cast<std::ptrdiff_t>(cuts.size() - 1));
            REQUIRE(controller->session().undo());
            CHECK(std::ranges::none_of(controller->currentSequence()->markers, [](const Marker& m) { return m.name == "Cut (pace)"; }));

            REQUIRE(QMetaObject::invokeMethod(sheet, "close"));
            REQUIRE(waitFor([&] { return !sheet->property("opened").toBool(); }));
            CHECK_FALSE(autopilot->staged());
            setPreviewFramesForTests({});
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}

TEST_CASE("Creator autopilot runs its jobs off the GUI thread and Stop cancels them", "[app][ai][autopilot]") {
    AutopilotBridge bridge;
    ProjectController controller;
    Recording essay = brollEssay();
    controller.resetProject(essay.tl.state, essay.tl.seq);
    bridge.setController(&controller);
    bridge.setServices(bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
    // Off the GUI thread: the call returns at once with the job running; Stop ends it as "Cancelled".
    std::atomic<bool> release{false};
    REQUIRE(bridge.runWorkerForTests([&](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
        while (!release.load() && !cancelled.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return {};
    }));
    CHECK(bridge.busy());
    CHECK(bridge.previewBroll() == QStringLiteral("Busy"));
    bridge.cancel();
    REQUIRE(bridge.waitForIdle(10'000));
    CHECK_FALSE(bridge.busy());
    // A real preview on the worker.
    CHECK(bridge.previewBroll().isEmpty());
    REQUIRE(bridge.waitForIdle(20'000));
    CHECK(bridge.broll().value(QStringLiteral("suggestions")).toList().size() == 3);
    CHECK(bridge.staged());
    // The project changed since the preview: Apply refuses and drops it.
    ProjectState renamed = *controller.snapshot();
    auto seq = std::make_shared<Sequence>(*renamed.findSequence(essay.tl.seq));
    seq->name = "Another name";
    renamed = renamed.withSequence(seq);
    REQUIRE(controller.session().commit("Rename Sequence", renamed));
    CHECK_FALSE(bridge.applyBroll().isEmpty());
    CHECK_FALSE(bridge.staged());
    // Fix round 1 (ARGON-17): it says so and Run again makes the preview on the timeline as it is now, then applies.
    CHECK(bridge.outdated());
    CHECK(bridge.rerun().isEmpty());
    CHECK_FALSE(bridge.outdated()); // running
    REQUIRE(bridge.waitForIdle(20'000));
    CHECK(bridge.staged());
    CHECK_FALSE(bridge.outdated());
    const std::size_t steps = controller.session().undoCount();
    CHECK(bridge.applyBroll().isEmpty());
    CHECK(controller.session().undoCount() == steps + 1);
    CHECK(controller.currentSequence()->name == "Another name"); // the edit made meanwhile is kept
    // The timeline edited while the job runs: the preview lands marked out of date, and Apply would not use it.
    REQUIRE(controller.undo());
    CHECK(bridge.previewBroll().isEmpty());
    ProjectState third = *controller.snapshot();
    auto again = std::make_shared<Sequence>(*third.findSequence(essay.tl.seq));
    again->name = "A third name";
    third = third.withSequence(again);
    REQUIRE(controller.session().commit("Rename Sequence", third));
    REQUIRE(bridge.waitForIdle(20'000));
    CHECK(bridge.outdated());
    CHECK_FALSE(bridge.applyBroll().isEmpty());
    CHECK(controller.session().undoCount() == steps + 1);
    CHECK(bridge.rerun().isEmpty());
    REQUIRE(bridge.waitForIdle(20'000));
    CHECK_FALSE(bridge.outdated());
    CHECK(bridge.staged());
    release = true;
}

// Fix round 3 (ARGON-17 remainder): the publishing kit (its chapters come from the timeline's chapter markers) takes the
// same outdated / Run again path as B-roll and pacing: an edit after Prepare kit marks it out of date, Write files
// refuses it (nothing is written) and Run again makes it for the timeline as it is now.
TEST_CASE("Creator autopilot kit chapters go out of date with the timeline and Run again makes them again",
          "[app][ai][autopilot]") {
    resetSettings();
    AutopilotBridge bridge;
    ProjectController controller;
    Recording essay = brollEssay();
    essay.addMarker(0.0, "Intro");
    essay.addMarker(18.0, "The kitchen");
    essay.addMarker(40.0, "The beach");
    controller.resetProject(essay.tl.state, essay.tl.seq);
    bridge.setController(&controller);
    bridge.setServices(bundle({std::make_shared<ai::edit::TranscriptServices>(essay.transcripts)}));
    const QVariantMap options{{QStringLiteral("thumbnails"), 0}, {QStringLiteral("chapters"), QStringLiteral("markers")}};
    CHECK(bridge.makeKit(options).isEmpty());
    REQUIRE(bridge.waitForIdle(20'000));
    INFO(bridge.message().toStdString());
    CHECK(bridge.kit().value(QStringLiteral("chaptersText")).toString() ==
          QStringLiteral("0:00 Intro\n0:18 The kitchen\n0:40 The beach\n"));
    CHECK_FALSE(bridge.outdated());

    // A chapter marker moves: the kit's chapters are the old timeline's.
    ProjectState moved = *controller.snapshot();
    auto seq = std::make_shared<Sequence>(*moved.findSequence(essay.tl.seq));
    REQUIRE(seq->markers.size() == 3);
    seq->markers[2].start = secs(45.0);
    moved = moved.withSequence(seq);
    REQUIRE(controller.session().commit("Move Marker", moved));
    CHECK(bridge.outdated());
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    CHECK_FALSE(bridge.writeKit(QUrl::fromLocalFile(dir.path())).isEmpty());
    CHECK(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()); // nothing written
    CHECK(bridge.outdated());

    // Run again: the kit of the timeline as it is now, which writes.
    CHECK(bridge.rerun().isEmpty());
    REQUIRE(bridge.waitForIdle(20'000));
    CHECK_FALSE(bridge.outdated());
    CHECK(bridge.kit().value(QStringLiteral("chaptersText")).toString() ==
          QStringLiteral("0:00 Intro\n0:18 The kitchen\n0:45 The beach\n"));
    CHECK(bridge.writeKit(QUrl::fromLocalFile(dir.path())).isEmpty());
    REQUIRE(bridge.waitForIdle(20'000));
    CHECK(bridge.kit().value(QStringLiteral("written")).toBool());
    CHECK_FALSE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}
