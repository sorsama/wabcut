// 0.6 (card #74): auditions in the app - Clip > Auditions > New Audition from the selected clips, the Take Selector
// panel (Use This Take, Play to audition a take in place without an undo step, rename, reorder, duplicate, remove,
// flatten), the timeline badge "Take 2 of 2" and tooltip, the clip menu's Audition submenu, the palette's availability
// and the Program Monitor's take strip. The real shell (Main.qml, hidden). Screenshots under test-output/ui
// (p1-auditions-*.png, dark and light).

#include "ShellFixture.h"

#include "AuditionsBridge.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/model/Auditions.h>
#include <wabcut/model/Sequence.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <string>
#include <string_view>

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

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

/// The shell over a project from a timeline picture: two camera takes of a line, picture and sound linked, the second
/// stacked above the first.
struct AuditionShell {
    Shell shell;
    ProjectController* controller = nullptr;
    AuditionsBridge* auditions = nullptr;
    Timeline tl{R"(
        V2 |B5bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb..........|
        V1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa..........|
        A1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa..........|
        A2 |B5bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb..........|
    )"};
    QObject* keymap = nullptr;
    QObject* palette = nullptr;

    explicit AuditionShell(bool dark = true) {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auditions = shell.engine.singletonInstance<AuditionsBridge*>("Wabcut", "Auditions");
        keymap = shell.engine.singletonInstance<QObject*>("Wabcut", "KeymapDispatcher");
        palette = shell.engine.singletonInstance<QObject*>("Wabcut", "CommandPalette");
        REQUIRE(controller);
        REQUIRE(auditions);
        REQUIRE(keymap);
        REQUIRE(palette);
        tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    const Sequence& sequence() const { return *controller->currentSequence(); }
    const Clip& clip(ClipId id) const { return sequence().findClip(id)->clip(); }
    std::string mediaName(ClipId id) const { return controller->snapshot()->findMedia(clip(id).media)->name; }
    /// The media of `id` in what plays (EditSession::shown: an auditioned take while Play previews it).
    std::string playingMediaName(ClipId id) const {
        const ProjectSnapshot shown = controller->session().shown();
        return shown->findMedia(shown->findSequence(tl.seq)->findClip(id)->clip().media)->name;
    }
    std::size_t undo() const { return controller->session().undoCount(); }
    void run(const char* command) const {
        REQUIRE(QMetaObject::invokeMethod(keymap, "trigger", Q_ARG(QString, QString::fromLatin1(command))));
        spin(50);
    }
    QVariantMap state(const char* command) const {
        QVariantMap out;
        REQUIRE(QMetaObject::invokeMethod(palette, "commandState", Q_RETURN_ARG(QVariantMap, out),
                                          Q_ARG(QString, QString::fromLatin1(command))));
        return out;
    }
    TimelineView* view() const {
        auto* v = qobject_cast<TimelineView*>(item(QStringLiteral("timelineView")));
        REQUIRE(v);
        return v;
    }
    /// Selects both takes (picture and sound) and makes them an audition, with the Take Selector open.
    ClipId makeAudition() {
        const ClipId a = clipAt(*controller->snapshot(), tl.seq, "V1@0");
        controller->selectClipById(a, ProjectController::Replace);
        controller->selectClipById(clipAt(*controller->snapshot(), tl.seq, "V2@0"), ProjectController::Toggle);
        REQUIRE(controller->selection().size() == 4);
        CHECK(state("audition.create").value(QStringLiteral("enabled")).toBool());
        run("audition.create");
        REQUIRE(clip(a).isAudition());
        run("audition.showTakeSelector");
        REQUIRE(waitFor([&] {
            QQuickItem* panel = item(QStringLiteral("takeSelectorPanel"));
            return panel && panel->isVisible() && item(QStringLiteral("takeRow_1")) != nullptr;
        }));
        return a;
    }
    void shot(const QRectF& rect, const QString& file) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = rect.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
    }
};

} // namespace

TEST_CASE("Auditions UI: New Audition then Use This Take in the Take Selector with the timeline badge",
          "[app][auditions]") {
    AuditionShell s;
    clearMessages();
    // Nothing selected: New Audition says why.
    CHECK_FALSE(s.state("audition.create").value(QStringLiteral("enabled")).toBool());
    CHECK(s.state("audition.flatten").value(QStringLiteral("reason")).toString() == QStringLiteral("Select an audition first"));
    const std::size_t steps = s.undo();
    const ClipId a = s.makeAudition();
    CHECK(s.undo() == steps + 1);
    CHECK(s.controller->undoText() == QStringLiteral("Undo New Audition"));
    // The second take left V2 and A2; picture and sound are one audition.
    CHECK(s.sequence().videoTracks[1]->clips.empty());
    CHECK(s.sequence().audioTracks[1]->clips.empty());
    CHECK(s.auditions->clip() == clipKey(a));
    CHECK(s.auditions->count() == 2);
    CHECK(s.auditions->badge() == QStringLiteral("Take 1 of 2"));
    CHECK(s.auditions->kindText() == QStringLiteral("Picture and sound"));
    const QVariantList takes = s.auditions->takes();
    REQUIRE(takes.size() == 2);
    CHECK(takes[1].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("B"));
    CHECK(takes[0].toMap().value(QStringLiteral("active")).toBool());

    // Use take 2 (its check button): the clip keeps its place, the playhead stays, one undo step.
    s.controller->setPlayhead(framesToTicks(10, kRate25));
    const Ticks playhead = s.controller->playhead();
    QQuickItem* use = s.item(QStringLiteral("takeUse_1"));
    REQUIRE(use);
    REQUIRE(QMetaObject::invokeMethod(use, "clicked"));
    REQUIRE(waitFor([&] { return s.auditions->activeIndex() == 1; }));
    CHECK(s.mediaName(a) == "B");
    CHECK(s.clip(a).sourceIn == framesToTicks(5, kRate25));
    CHECK(s.controller->playhead() == playhead);
    CHECK(s.controller->undoText() == QStringLiteral("Undo Use Take"));
    CHECK(s.undo() == steps + 2);
    // The sound switched with the picture.
    const ClipId sound = clipAt(*s.controller->snapshot(), s.tl.seq, "A1@0");
    CHECK(s.mediaName(sound) == "B");

    // The timeline badge and the tooltip say which take plays.
    s.view()->setPixelsPerSecond(400.0); // the clip wide enough for its name and the badge
    REQUIRE_FALSE(s.shell.grab().isNull());
    spin(50);
    REQUIRE_FALSE(s.shell.grab().isNull());
    INFO("label: " << s.view()->stats().labelTexts.value(a.value).toStdString() << " badge: "
                   << s.view()->stats().badgeTexts.value(a.value).toStdString());
    CHECK(s.view()->stats().badgeTexts.value(a.value).startsWith(QStringLiteral("Take 2 of 2")));
    CHECK(s.view()->clipHoverInfo(clipKey(a)).value(QStringLiteral("text")).toString().contains(
        QStringLiteral("Audition: take 2 of 2")));

    // Next Take wraps around; the menus' rows are available.
    CHECK(s.state("audition.nextTake").value(QStringLiteral("enabled")).toBool());
    s.run("audition.nextTake");
    CHECK(s.auditions->activeIndex() == 0);
    CHECK(s.mediaName(a) == "A");
    // Undo goes back one step at a time.
    REQUIRE(s.controller->undo());
    CHECK(s.auditions->activeIndex() == 1);
    checkNoMessages();
}

TEST_CASE("Auditions UI: Play auditions a take in place and leaves no undo step", "[app][auditions]") {
    AuditionShell s;
    const ClipId a = s.makeAudition();
    const std::size_t steps = s.undo();
    REQUIRE(s.auditions->previewTake(1));
    CHECK(s.auditions->previewing() == 1);
    CHECK(s.playingMediaName(a) == "B"); // the take plays in the audition's place
    s.auditions->stopPreview();
    CHECK(s.auditions->previewing() == -1);
    CHECK(s.auditions->activeIndex() == 0);
    CHECK(s.mediaName(a) == "A");
    CHECK(s.undo() == steps);
    CHECK_FALSE(s.controller->session().canRedo());
    // Use This Take while it plays: it stays (one undo step).
    REQUIRE(s.auditions->previewTake(1));
    REQUIRE(s.auditions->useTake(1));
    s.auditions->stopPreview();
    CHECK(s.auditions->activeIndex() == 1);
    CHECK(s.undo() == steps + 1);
    CHECK(s.controller->undoText() == QStringLiteral("Undo Use Take"));
}

TEST_CASE("Auditions UI: Play keeps the redo history and never reaches the saved project", "[app][auditions]") {
    AuditionShell s;
    const ClipId a = s.makeAudition();
    // A take's length reads in the sequence's display format (timecode), not seconds.
    CHECK(s.auditions->takes()[0].toMap().value(QStringLiteral("durationText")).toString() ==
          s.controller->formatTimecode(auditionRange(s.clip(a)).duration));
    // Something to redo: a rename, undone.
    REQUIRE(s.auditions->renameTake(1, QStringLiteral("Angry read")));
    REQUIRE(s.controller->undo());
    REQUIRE(s.controller->redoText() == QStringLiteral("Redo Rename Take"));
    const std::size_t steps = s.undo();

    REQUIRE(s.auditions->previewTake(1));
    CHECK(s.playingMediaName(a) == "B"); // the take plays in the audition's place
    CHECK(s.controller->session().inPreview());
    // A save or an autosave now writes the take that is really active.
    const ProjectState saved = s.controller->session().stateForSave();
    const Clip& savedClip = saved.findSequence(s.tl.seq)->findClip(a)->clip();
    CHECK(saved.findMedia(savedClip.media)->name == "A");
    CHECK(s.undo() == steps);
    CHECK(s.controller->redoText() == QStringLiteral("Redo Rename Take"));
    s.auditions->stopPreview();
    CHECK(s.mediaName(a) == "A");
    CHECK_FALSE(s.controller->session().inPreview());
    CHECK(s.undo() == steps);
    CHECK(s.controller->redoText() == QStringLiteral("Redo Rename Take")); // the redo history survived

    // An undo while a take plays: the preview goes first, then the undo (of the committed state).
    REQUIRE(s.auditions->previewTake(1));
    REQUIRE(s.controller->undo());
    CHECK(s.auditions->previewing() == -1);
    CHECK_FALSE(s.controller->session().inPreview());
    CHECK(s.mediaName(a) == "A");
    CHECK(s.undo() == steps - 1);
    s.auditions->stopPreview();
    CHECK(s.mediaName(a) == "A");

    // The clip menu on the audition's sound half lists its takes too.
    REQUIRE(s.controller->redo());
    const ClipId sound = clipAt(*s.controller->snapshot(), s.tl.seq, "A1@0");
    REQUIRE(sound != a);
    s.controller->selectClipById(a, ProjectController::Replace);
    QObject* submenu = s.shell.window->findChild<QObject*>(QStringLiteral("auditionSubmenu"));
    REQUIRE(submenu);
    submenu->setProperty("clipKey", clipKey(sound));
    REQUIRE(QMetaObject::invokeMethod(submenu, "refresh"));
    CHECK(submenu->property("isAudition").toBool());
    CHECK(submenu->property("takes").toList().size() == 2);
    checkNoMessages();
}

// Fix round 3 (NEW-02): a take that Play previews is only what plays. The Take Selector's Active take, Next and Previous
// Take, and everything that writes a file (here an EDL export) use the take that is really active.
TEST_CASE("Auditions UI: while Play previews a take the active take and exports stay the committed one",
          "[app][auditions]") {
    AuditionShell s;
    const ClipId a = s.makeAudition();
    auto* project = s.shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(project);
    REQUIRE(s.auditions->activeIndex() == 0);
    const std::size_t steps = s.undo();

    REQUIRE(s.auditions->previewTake(1));
    CHECK(s.auditions->previewing() == 1);
    CHECK(s.playingMediaName(a) == "B");
    // The Take Selector and the take strip: take 1 stays Active (the previewed one is marked by `previewing`).
    CHECK(s.auditions->activeIndex() == 0);
    CHECK(s.auditions->badge() == QStringLiteral("Take 1 of 2"));
    const QVariantList takes = s.auditions->takes();
    REQUIRE(takes.size() == 2);
    CHECK(takes[0].toMap().value(QStringLiteral("active")).toBool());
    CHECK_FALSE(takes[1].toMap().value(QStringLiteral("active")).toBool());
    // What every export, save and command reads.
    CHECK(s.mediaName(a) == "A");

    // A timeline export started while the take plays writes the committed take.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cut.edl"));
    QVariantMap report;
    bool done = false;
    const auto connection = QObject::connect(project, &ProjectLifecycle::timelineExported, project,
                                             [&](const QVariantMap& r) {
                                                 report = r;
                                                 done = true;
                                             });
    REQUIRE(s.controller->session().inPreview());
    REQUIRE(project->exportTimeline(QStringLiteral("edl"), 0, QUrl::fromLocalFile(path), {}));
    REQUIRE(waitFor([&] { return done; }, 20000));
    QObject::disconnect(connection);
    INFO(report.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray edl = file.readAll();
    INFO(edl.toStdString());
    CHECK(edl.contains("A.mov"));
    CHECK_FALSE(edl.contains("B.mov"));
    CHECK(s.undo() == steps);

    // Next Take while take 2 previews: take 2 becomes the active take (one undo step), not back to take 1.
    if (s.auditions->previewing() != 1) // the preview may have played to its end meanwhile
        REQUIRE(s.auditions->previewTake(1));
    CHECK(s.state("audition.nextTake").value(QStringLiteral("enabled")).toBool());
    REQUIRE(s.auditions->nextTake());
    s.auditions->stopPreview();
    CHECK(s.auditions->activeIndex() == 1);
    CHECK(s.mediaName(a) == "B");
    CHECK(s.undo() == steps + 1);
    CHECK(s.controller->undoText() == QStringLiteral("Undo Use Take"));
    // Previous Take while take 1 previews (take 2 active): back to take 1.
    REQUIRE(s.auditions->previewTake(0));
    REQUIRE(s.auditions->previousTake());
    s.auditions->stopPreview();
    CHECK(s.auditions->activeIndex() == 0);
    CHECK(s.mediaName(a) == "A");
    CHECK(s.undo() == steps + 2);
    checkNoMessages();
}

TEST_CASE("Auditions UI: rename reorder duplicate remove and flatten from the Take Selector and the clip menu",
          "[app][auditions]") {
    AuditionShell s;
    const ClipId a = s.makeAudition();
    REQUIRE(s.auditions->renameTake(1, QStringLiteral("Angry read")));
    CHECK(s.auditions->takes()[1].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Angry read"));
    CHECK(s.controller->undoText() == QStringLiteral("Undo Rename Take"));
    REQUIRE(s.auditions->moveTake(1, 0));
    CHECK(s.auditions->activeIndex() == 1); // A stays active
    REQUIRE(s.auditions->duplicateTake(-1));
    CHECK(s.auditions->count() == 3);
    CHECK(s.auditions->name() == QStringLiteral("A copy"));
    REQUIRE(s.auditions->removeTake(2));
    CHECK(s.auditions->count() == 2);
    CHECK(s.auditions->name() == QStringLiteral("A"));

    // The clip menu's Audition submenu lists the takes, the active one checked.
    s.controller->selectClipById(a, ProjectController::Replace);
    QObject* submenu = s.shell.window->findChild<QObject*>(QStringLiteral("auditionSubmenu"));
    REQUIRE(submenu);
    submenu->setProperty("clipKey", clipKey(a));
    REQUIRE(QMetaObject::invokeMethod(submenu, "refresh"));
    CHECK(submenu->property("isAudition").toBool());
    CHECK(submenu->property("takes").toList().size() == 2);

    REQUIRE(s.auditions->flatten());
    CHECK_FALSE(s.clip(a).isAudition());
    CHECK(s.auditions->clip().isEmpty());
    CHECK(s.controller->undoText() == QStringLiteral("Undo Flatten Audition"));
    CHECK(s.item(QStringLiteral("takeSelectorEmptyState"))->isVisible());
}

TEST_CASE("Auditions UI: the Take Selector and the Program Monitor take strip in the dark and light themes",
          "[app][auditions][screenshot]") {
    for (const bool dark : {true, false}) {
        AuditionShell s(dark);
        const ClipId a = s.makeAudition();
        REQUIRE(s.auditions->renameTake(0, QStringLiteral("Calm read")));
        REQUIRE(s.auditions->renameTake(1, QStringLiteral("Angry read")));
        REQUIRE(s.auditions->useTake(1));
        s.auditions->setOverlayShown(true);
        s.controller->setPlayhead(framesToTicks(10, kRate25));
        s.controller->selectClipById(a, ProjectController::Replace);
        REQUIRE(waitFor([&] {
            QQuickItem* strip = s.item(QStringLiteral("takeStripOverlay"));
            return strip && strip->isVisible() && s.item(QStringLiteral("takeCard_1")) != nullptr;
        }));
        REQUIRE_FALSE(s.shell.grab().isNull());
        spin(50);
        QQuickItem* panel = s.item(QStringLiteral("takeSelectorPanel"));
        REQUIRE(panel);
        const QString suffix = dark ? QString() : QStringLiteral("-light");
        s.shot(sceneRect(panel).adjusted(-4, -32, 4, 4), QStringLiteral("p1-auditions-take-selector%1.png").arg(suffix));
        QQuickItem* strip = s.item(QStringLiteral("takeStripOverlay"));
        s.shot(sceneRect(strip).adjusted(-24, -24, 24, 24), QStringLiteral("p1-auditions-program-strip%1.png").arg(suffix));
        s.shot(QRectF(0, 0, kShotWidth, kShotHeight), QStringLiteral("p1-auditions-shell%1.png").arg(suffix));
        // A card click uses its take.
        QQuickItem* card = s.item(QStringLiteral("takeCard_0"));
        REQUIRE(card);
        REQUIRE(s.auditions->useTake(0));
        CHECK(s.auditions->activeIndex() == 0);
        s.auditions->setOverlayShown(false);
    }
}
