// AI-13 in the app, on the UI shell with the synthetic 3-camera podcast (scripted transcripts, mic levels and faces;
// no model, no media decoding): the "Auto switch" dialog finds who is on which camera (face thumbnails), lists the cuts
// with their reasons, takes the user's angle assignment and adds the angle cuts as one undo step; the "Podcast
// workflow" sheet runs the steps one by one (preview, apply: each its own undo step, checkpointed in the project's
// history), survives a crash (resume from the journal and the last checkpoint) and finishes the episode. The dialogs are
// made from the Wabcut module directly (their menu hooks are Main.qml's). Screenshots: <build>/test-output/ui/
// auto-switch-dialog.png and podcast-workflow-sheet.png.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "MulticamBridge.h"
#include "ProjectController.h"
#include "multicam/PodcastTestUtil.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/History.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QPainter>
#include <QQmlComponent>
#include <QQuickItem>
#include <QUrl>

#include <algorithm>
#include <atomic>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::ai::multicam::test;

namespace {

QString saveFixture(const ProjectState& state, const QString& name) {
    const QString path = outputDir() + "/" + name;
    REQUIRE(io::saveProject(state, std::filesystem::path(path.toStdWString())));
    return path;
}

QObject* make(Shell& shell, const char* type) {
    QQmlComponent component(&shell.engine);
    component.loadFromModule("Wabcut", type);
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    QObject* object = component.createWithInitialProperties({{QStringLiteral("parent"), QVariant::fromValue(shell.window->contentItem())}});
    REQUIRE(object != nullptr);
    object->setParent(shell.window.get());
    return object;
}

void showSequence(ProjectController* controller, SequenceId id) {
    const ProjectSnapshot s = controller->snapshot();
    for (std::size_t i = 0; i < s->sequences.size(); ++i) {
        if (s->sequences[i]->id == id) {
            controller->setCurrentSequenceIndex(static_cast<int>(i));
        }
    }
    REQUIRE(controller->currentSequenceId() == id);
}

/// A studio frame: the host's camera warm, the guest's cool, the wide shot with both; faces where ScriptSignals puts them.
QImage studioFrame(const MediaItem& media, Ticks) {
    QImage image(640, 360, QImage::Format_RGB32);
    QPainter p(&image);
    const bool host = media.name.find('A') != std::string::npos;
    const bool guest = media.name.find('B') != std::string::npos;
    QLinearGradient bg(0, 0, 640, 360);
    bg.setColorAt(0, host ? QColor(70, 40, 30) : guest ? QColor(25, 45, 70) : QColor(40, 36, 52));
    bg.setColorAt(1, QColor(12, 10, 16));
    p.fillRect(image.rect(), bg);
    p.fillRect(QRect(0, 250, 640, 110), QColor(30, 26, 34)); // the desk
    const auto face = [&](double x, QColor shirt) {
        const QRectF box(x * 640, 0.2 * 360, 0.3 * 640, 0.45 * 360);
        p.setPen(Qt::NoPen);
        p.setBrush(shirt);
        p.drawRoundedRect(QRectF(box.x() - 30, box.bottom() - 10, box.width() + 60, 140), 40, 40);
        p.setBrush(QColor(214, 170, 140));
        p.drawEllipse(box.adjusted(box.width() * 0.2, 0, -box.width() * 0.2, 0));
        p.setBrush(QColor(40, 28, 24));
        p.drawRect(QRectF(box.center().x() - 18, box.bottom() - 40, 36, 8)); // the mouth
    };
    if (host || guest) {
        face(0.2, host ? QColor(160, 60, 50) : QColor(50, 90, 160));
    } else {
        face(0.2, QColor(160, 60, 50));
        face(0.6, QColor(50, 90, 160));
    }
    return image;
}

} // namespace

TEST_CASE("Auto switch dialog: cameras and speakers, the cut list with reasons, one undo step; screenshot", "[ai][ui][multicam][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* autoSwitch = shell.engine.singletonInstance<AutoSwitchBridge*>("Wabcut", "AutoSwitch");
    REQUIRE(controller != nullptr);
    REQUIRE(autoSwitch != nullptr);

    Studio s;
    REQUIRE(controller->loadProject(QUrl::fromLocalFile(saveFixture(s.state, QStringLiteral("auto-switch-fixture.wabcut")))));
    showSequence(controller, s.tl.seq);
    setPreviewFramesForTests(studioFrame);
    autoSwitch->setServices(s.services());
    autoSwitch->setSynchronous(true);
    shell.resize(1600, 1100);
    CHECK(autoSwitch->defaultClip() == clipKey(s.picture));
    CHECK(autoSwitch->presets().size() == 3);
    CHECK(autoSwitch->presetValues(QStringLiteral("calm")).value(QStringLiteral("minHold")).toDouble() == 3.0);

    QObject* dialog = make(shell, "AutoSwitchDialog");
    REQUIRE(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant(clipKey(s.picture)))));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE(autoSwitch->waitForIdle(20'000));
    INFO("Qt messages: " << join(takeMessages()));
    INFO(autoSwitch->message().toStdString());
    const QVariantMap result = autoSwitch->result();
    REQUIRE(autoSwitch->staged());
    const QVariantList cuts = result.value(QStringLiteral("cuts")).toList();
    const QVariantList angles = result.value(QStringLiteral("angles")).toList();
    REQUIRE(cuts.size() >= 4);
    REQUIRE(angles.size() == 3);
    CHECK(result.value(QStringLiteral("speakers")).toList().size() == 2);
    CHECK(angles[0].toMap().value(QStringLiteral("role")).toString() == QStringLiteral("closeUp"));
    CHECK(angles[2].toMap().value(QStringLiteral("role")).toString() == QStringLiteral("wide"));
    CHECK(angles[0].toMap().value(QStringLiteral("frame")).toString().startsWith(QStringLiteral("image://wabcut-frames/")));
    CHECK(cuts[1].toMap().value(QStringLiteral("explanation")).toString().contains(QStringLiteral("Cut to")));
    CHECK(cuts[1].toMap().value(QStringLiteral("angleName")).toString().startsWith(QStringLiteral("Cam ")));
    auto* list = dialog->findChild<QQuickItem*>("autoSwitchCuts");
    REQUIRE(list != nullptr);
    REQUIRE(waitFor([&] { return list->property("count").toInt() == cuts.size(); }));
    // The cameras' cards (Repeater delegates: children of the Flow's item, not of the popup).
    auto* cards = dialog->findChild<QQuickItem*>("autoSwitchAngles");
    REQUIRE(cards != nullptr);
    CHECK(std::ranges::count_if(cards->childItems(), [](QQuickItem* i) { return i->objectName().startsWith(QStringLiteral("autoSwitchAngle_")); }) == 3);
    spin(800);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/auto-switch-dialog.png")));

    // The user confirms Cam C as the wide shot: their word, marked as such.
    REQUIRE(QMetaObject::invokeMethod(dialog, "assign", Q_ARG(QVariant, 3), Q_ARG(QVariant, QStringLiteral("wide"))));
    REQUIRE(autoSwitch->waitForIdle(20'000));
    CHECK(autoSwitch->result().value(QStringLiteral("angles")).toList()[2].toMap().value(QStringLiteral("confirmed")).toBool());

    const std::size_t undo = controller->session().undoCount();
    CHECK(autoSwitch->apply().isEmpty());
    CHECK(controller->session().undoCount() == undo + 1);
    const Sequence* seq = controller->snapshot()->findSequence(s.tl.seq);
    REQUIRE(seq != nullptr);
    CHECK(seq->videoTracks.front()->clips.size() >= 4);
    for (const Clip& c : seq->videoTracks.front()->clips) {
        CHECK(c.sequence == s.multicam);
    }
    REQUIRE(controller->session().undo());
    CHECK(controller->snapshot()->findSequence(s.tl.seq)->videoTracks.front()->clips.size() == 1);
    setPreviewFramesForTests({});
    delete dialog;
}

TEST_CASE("Podcast workflow sheet: the step checklist, steps previewed and applied one by one, resumed after a crash; screenshot",
          "[ai][ui][multicam][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* podcast = shell.engine.singletonInstance<PodcastBridge*>("Wabcut", "PodcastWorkflow");
    REQUIRE(controller != nullptr);
    REQUIRE(podcast != nullptr);

    PodcastStudio p;
    // Only the five recordings (the timeline fixture has a media item per letter).
    std::erase_if(p.studio.state.media, [&](const MediaPtr& m) {
        return std::ranges::none_of(std::string("ABCDE"), [&](char c) { return m->id == p.studio.tl.media(c); });
    });
    const QString path = saveFixture(p.studio.state, QStringLiteral("podcast-fixture.wabcut"));
    const std::filesystem::path project(path.toStdWString());
    std::filesystem::remove_all(ProjectHistory::folderFor(project));
    REQUIRE(controller->loadProject(QUrl::fromLocalFile(path)));
    podcast->setServices(p.services);
    podcast->setSynchronous(true);
    podcast->setProjectFileForTests(project);
    podcast->reset();
    shell.resize(1600, 1100);
    for (const char m : {'A', 'B', 'C', 'D', 'E'}) {
        podcast->addMedia(mediaKey(p.studio.tl.media(m)));
    }
    const QVariantList recordings = podcast->recordings();
    REQUIRE(recordings.size() == 5);
    CHECK(recordings[0].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("camera"));
    CHECK(recordings[4].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("mic"));
    podcast->setOptions({{QStringLiteral("name"), QStringLiteral("Episode 12")}, {QStringLiteral("minChapterSeconds"), 10}});
    QVariantList steps = podcast->steps();
    REQUIRE(steps.size() == 10);
    CHECK(steps[2].toMap().value(QStringLiteral("status")).toString() == QStringLiteral("off"));
    CHECK(steps[2].toMap().value(QStringLiteral("reason")).toString() == QStringLiteral("already transcribed"));
    CHECK(podcast->cost() == QStringLiteral("local, free"));

    QObject* sheet = make(shell, "PodcastWorkflowSheet");
    REQUIRE(QMetaObject::invokeMethod(sheet, "open"));
    REQUIRE(waitFor([&] { return sheet->property("opened").toBool(); }));

    // Three steps by hand: preview (staged), then apply (an undo step each, a history checkpoint each).
    const std::size_t undo = controller->session().undoCount();
    for (const char* step : {"multicam", "sequence", "enhance"}) {
        INFO(step);
        CHECK(podcast->previewStep(QString::fromLatin1(step)).isEmpty());
        REQUIRE(podcast->waitForIdle(30'000));
        INFO(podcast->message().toStdString());
        REQUIRE(podcast->staged());
        CHECK(podcast->applyStep().isEmpty());
    }
    CHECK(controller->session().undoCount() == undo + 3);
    CHECK(podcast->doneCount() == 3);
    REQUIRE_FALSE(podcast->episode().isEmpty());
    CHECK(sequenceKey(controller->currentSequenceId()) == podcast->episode()); // the episode is open
    podcast->setStepEnabled(QStringLiteral("captions"), false);
    auto* list = sheet->findChild<QQuickItem*>("podcastSteps");
    REQUIRE(list != nullptr);
    REQUIRE(waitFor([&] { return list->property("count").toInt() == 10; }));
    spin(800);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/podcast-workflow-sheet.png")));
    REQUIRE(std::filesystem::exists(ai::workflow::journalFileFor(project)));
    {
        auto history = ProjectHistory::open(ProjectHistory::folderFor(project));
        REQUIRE(history);
        CHECK(history->revisions().size() == 3);
    }

    // The app dies before the project is saved: the reopened file has none of it, the journal and history do.
    REQUIRE(controller->loadProject(QUrl::fromLocalFile(path)));
    podcast->reset();
    podcast->setProjectFileForTests(project);
    CHECK(podcast->canResume());
    CHECK(podcast->resume().isEmpty());
    CHECK(podcast->doneCount() == 3);
    CHECK(podcast->recordings().size() == 5);
    CHECK(sequenceKey(controller->currentSequenceId()) == podcast->episode()); // back from the last checkpoint
    podcast->setStepEnabled(QStringLiteral("captions"), false);

    const std::size_t resumedUndo = controller->session().undoCount();
    CHECK(podcast->runAll().isEmpty());
    REQUIRE(podcast->waitForIdle(60'000));
    INFO(podcast->message().toStdString());
    CHECK_FALSE(podcast->running());
    CHECK(podcast->doneCount() == 7); // + loudness, clean-up, switching, chapters
    CHECK(podcast->enabledCount() == 7);
    CHECK(controller->session().undoCount() == resumedUndo + 4);
    CHECK(podcast->youtubeChapters().startsWith(QStringLiteral("0:00 ")));
    CHECK_FALSE(podcast->canResume());
    steps = podcast->steps();
    CHECK(steps[7].toMap().value(QStringLiteral("status")).toString() == QStringLiteral("off")); // captions, unticked
    CHECK(steps[6].toMap().value(QStringLiteral("status")).toString() == QStringLiteral("done"));
    const Sequence* episode = controller->snapshot()->findSequence(controller->currentSequenceId());
    REQUIRE(episode != nullptr);
    std::set<std::size_t> shown;
    for (const Clip& c : episode->videoTracks.front()->clips) {
        shown.insert(c.angles.first());
    }
    CHECK(shown.size() >= 3);
    spin(400);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/podcast-workflow-done.png")));
    delete sheet;
}

TEST_CASE("AI-13 hooks: the clip menu's Auto Switch and Sequence > Podcast Workflow open their dialogs", "[ai][ui][multicam]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    QObject* switchLoader = shell.object("autoSwitchDialogLoader");
    QObject* podcastLoader = shell.object("podcastWorkflowSheetLoader");
    if (!switchLoader || !podcastLoader) {
        SKIP("the AI-13 hooks patch (Main.qml loaders, clip and Sequence menu items) is not applied in this branch");
    }
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* autoSwitch = shell.engine.singletonInstance<AutoSwitchBridge*>("Wabcut", "AutoSwitch");
    auto* podcast = shell.engine.singletonInstance<PodcastBridge*>("Wabcut", "PodcastWorkflow");
    Studio s;
    REQUIRE(controller->loadProject(QUrl::fromLocalFile(saveFixture(s.state, QStringLiteral("auto-switch-hooks.wabcut")))));
    showSequence(controller, s.tl.seq);
    autoSwitch->setServices(s.services());
    autoSwitch->setSynchronous(true);
    CHECK(autoSwitch->isMulticam(clipKey(s.picture)));
    shell.resize(1600, 1100);

    autoSwitch->open(clipKey(s.picture));
    REQUIRE(waitFor([&] { return switchLoader->property("active").toBool(); }));
    QObject* dialog = switchLoader->property("item").value<QObject*>();
    REQUIRE(dialog != nullptr);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    CHECK(dialog->property("clipKey").toString() == clipKey(s.picture));
    REQUIRE(autoSwitch->waitForIdle(20'000));
    CHECK(autoSwitch->staged());
    QMetaObject::invokeMethod(dialog, "close");

    podcast->open();
    REQUIRE(waitFor([&] { return podcastLoader->property("active").toBool(); }));
    QObject* sheet = podcastLoader->property("item").value<QObject*>();
    REQUIRE(sheet != nullptr);
    REQUIRE(waitFor([&] { return sheet->property("opened").toBool(); }));
}

namespace {

/// Counts the transcript lookups planning makes.
class CountingTranscripts final : public ai::edit::TranscriptProvider {
public:
    explicit CountingTranscripts(std::shared_ptr<ai::edit::TranscriptProvider> inner) : m_inner(std::move(inner)) {}
    std::shared_ptr<const ai::Transcript> transcript(const MediaItem& media, int audioStream) const override {
        ++calls;
        return m_inner ? m_inner->transcript(media, audioStream) : nullptr;
    }
    mutable std::atomic<int> calls{0};

private:
    std::shared_ptr<ai::edit::TranscriptProvider> m_inner;
};

} // namespace

// Review wave 5, A5-P2-25: the sheet reads the steps at every progress tick, and every read planned the whole workflow
// again on the GUI thread (transcripts and fingerprints read from disk). The plan is made again only when something it
// depends on changed.
TEST_CASE("Podcast workflow: the steps are planned once per change, not at every read", "[ai][ui][multicam]") {
    resetSettings();
    PodcastStudio p;
    ProjectController controller;
    controller.resetProject(p.studio.state, p.studio.tl.seq);
    const auto* original = command::findService<ai::edit::TranscriptServices>(p.services.get());
    auto counting = std::make_shared<CountingTranscripts>(original ? original->transcripts : nullptr);
    auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<ai::edit::TranscriptServices>(counting), p.services});
    PodcastBridge podcast;
    podcast.setController(&controller);
    podcast.setServices(services);
    podcast.setProjectFileForTests(std::filesystem::path(outputDir().toStdWString()) / "podcast-plan-cache.wabcut");
    for (const char m : {'A', 'B', 'C', 'D', 'E'}) {
        podcast.addMedia(mediaKey(p.studio.tl.media(m)));
    }
    (void)podcast.steps();
    const int once = counting->calls.load();
    REQUIRE(once > 0);
    for (int tick = 0; tick < 10; ++tick) {
        (void)podcast.steps(); // what the sheet reads while a step runs
    }
    CHECK(counting->calls.load() == once);
    podcast.setOptions({{QStringLiteral("name"), QStringLiteral("Episode 13")}});
    (void)podcast.steps();
    CHECK(counting->calls.load() > once); // other options: planned again
}
