// 0.4 (qol round), stream E: background work.
//   * the export queue's summary for the title bar, the quit guard, keep-awake and the taskbar (ExportJobModel
//     runningCount / pausedCount / heldCount and the current job);
//   * PowerKeeper (Windows keeps awake while a keeper is active) and TaskbarProgress (the taskbar button's progress and
//     flash), both through their test seams - no power request is made and no taskbar is touched here.
//   * Shell (the real Main.qml, hidden): the quit guard (BackgroundWorkCloseDialog: Keep Wabcut Open, Quit When Done,
//     Cancel Exports and Quit), the title bar's Background tasks pill and popover with the recent messages, and keep
//     awake and the taskbar while an export runs. Export progress is fed into ExportController.jobs as the queue would
//     report it, so no test waits on a real render.

#include "ShellFixture.h"

#include "ExportController.h"
#include "PowerKeeper.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TaskbarProgress.h"

#include <wabcut/export/ExportJob.h>

#include <catch2/catch_test_macros.hpp>

#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <filesystem>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;
namespace fs = std::filesystem;

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "background-work" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
}

/// One 640x360 30 fps sequence of `seconds` with nothing on it.
ProjectState makeProject(std::int64_t seconds) {
    ProjectState state;
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Interview";
    seq.width = 640;
    seq.height = 360;
    seq.frameRate = kRate30;
    seq.duration = Ticks::fromSeconds(seconds);
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

/// A status of `queueId` as the export queue would report it.
ex::ExportJobStatus statusOf(ex::ExportJobId queueId, ex::ExportPhase phase, std::int64_t done = 0,
                             std::int64_t total = 0, bool held = false) {
    ex::ExportJobStatus s;
    s.id = queueId;
    s.held = held;
    s.progress.phase = phase;
    s.progress.framesDone = done;
    s.progress.framesTotal = total;
    if (phase == ex::ExportPhase::Rendering)
        s.progress.etaSeconds = 42.0;
    return s;
}

/// Restores PowerKeeper's platform backend and TaskbarProgress's taskbar calls when the test ends (also on a REQUIRE).
struct RestoreSeams {
    ~RestoreSeams() {
        PowerKeeper::setBackendForTests({});
        TaskbarProgress::setSinkForTests({});
    }
};

/// Runs `code` in Main.qml's context (its ids: window, toast, topBar, home, ...).
QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

/// The first item named `name` under `root` (the item tree: Repeaters' and Loaders' items too).
QQuickItem* findUnder(QQuickItem* root, const QString& name) {
    if (root == nullptr)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findUnder(child, name))
            return found;
    }
    return nullptr;
}

/// An item inside a Popup (a dialog, a popover): its content item's tree.
QQuickItem* inPopup(QObject* popup, const QString& name) {
    REQUIRE(popup != nullptr);
    return findUnder(popup->property("contentItem").value<QQuickItem*>(), name);
}

/// Main.readyToClose(): whether the window may close now (else it asked).
bool readyToClose(const Shell& shell) {
    QVariant ready;
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "readyToClose", Q_RETURN_ARG(QVariant, ready)));
    spin(50);
    return ready.toBool();
}

/// Feeds `phase` for the job `jobId` into the export model as the queue would report it (Done carries a result).
void feed(ExportController& exporter, const QString& jobId, ex::ExportPhase phase, std::int64_t done = 0,
          std::int64_t total = 0) {
    const ExportJobModel::Row* row = exporter.jobs()->find(jobId);
    REQUIRE(row != nullptr);
    ex::ExportJobStatus status = statusOf(row->queueId, phase, done, total);
    status.output = fs::path(row->outputPath.toStdU16String());
    if (phase == ex::ExportPhase::Done) {
        ex::ExportResult result;
        result.output = status.output;
        status.result = result;
    }
    REQUIRE(exporter.jobs()->update(row->queueId, status) != nullptr);
    spin(30);
}

/// One export of the current sequence sent to the queue, waiting for Start Queue.
QString holdExport(ExportController& exporter, const fs::path& output) {
    const QStringList ids = exporter.sendToQueue({}, {QStringLiteral("audio-wav")},
                                                 {{QStringLiteral("output"), q(output)}}, true);
    REQUIRE(ids.size() == 1);
    return ids.front();
}

QString stateOf(const ExportController& exporter, const QString& jobId) {
    return exporter.job(jobId).value(QStringLiteral("state")).toString();
}

/// The Shell with a 2 s sequence and synthetic video, Home closed, motion reduced.
struct BackgroundShell {
    Shell shell;
    ProjectController* project = nullptr;
    ExportController* exporter = nullptr;
    QQuickItem* toast = nullptr;
    int closes = 0;

    BackgroundShell() {
        REQUIRE(shell.window != nullptr);
        shell.theme->setProperty("reduceMotion", true);
        shell.resize(kShotWidth, kShotHeight);
        project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
        REQUIRE(project != nullptr);
        REQUIRE(exporter != nullptr);
        project->resetProject(makeProject(2));
        exporter->setVideoSourceFactory(ex::syntheticVideoSourceFactory());
        toast = shell.item("toast");
        REQUIRE(toast != nullptr);
        if (evaluate(shell, QStringLiteral("home.opened")).toBool())
            evaluate(shell, QStringLiteral("home.close()"));
        // The window's closing signal, counted (the lambda never touches the private event type).
        QObject::connect(shell.window.get(), &QQuickWindow::closing, shell.window.get(), [this](QQuickCloseEvent*) { ++closes; });
        spin(50);
    }
    QString toastText() const { return toast->property("text").toString(); }
};

} // namespace

TEST_CASE("Export jobs model: running paused and held counts and the current job", "[app][export][background]") {
    resetSettings();
    const fs::path dir = freshDir("summary");
    ProjectController project;
    project.resetProject(makeProject(2));
    ExportController controller;
    controller.setProject(&project);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    controller.setProjectFile(dir / "Interview.wabcut");
    ExportJobModel* jobs = controller.jobs();
    int summaries = 0;
    QObject::connect(jobs, &ExportJobModel::summaryChanged, [&] { ++summaries; });

    // Nothing in the queue.
    CHECK(jobs->runningCount() == 0);
    CHECK(jobs->pausedCount() == 0);
    CHECK(jobs->heldCount() == 0);
    CHECK(jobs->currentJobId().isEmpty());
    CHECK(jobs->currentState().isEmpty());
    CHECK(jobs->currentProgress() == -1.0);

    // Two exports sent to the queue, waiting for Start Queue: held, nothing runs, no current job.
    const QStringList first = controller.sendToQueue({}, {QStringLiteral("audio-wav")},
                                                     {{QStringLiteral("output"), q(dir / "one.wav")}}, true);
    const QStringList second = controller.sendToQueue({}, {QStringLiteral("audio-wav")},
                                                      {{QStringLiteral("output"), q(dir / "two.wav")}}, true);
    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    CHECK(jobs->heldCount() == 2);
    CHECK(jobs->runningCount() == 0);
    CHECK(jobs->pausedCount() == 0);
    CHECK(jobs->currentJobId().isEmpty());
    CHECK(jobs->currentState().isEmpty());
    CHECK(jobs->currentProgress() == -1.0);
    CHECK(summaries >= 1);

    // The first one renders (its status fed into the model as the queue reports it): it is the current job.
    const ExportJobModel::Row* one = jobs->find(first.front());
    const ExportJobModel::Row* two = jobs->find(second.front());
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    const ex::ExportJobId oneId = one->queueId;
    const ex::ExportJobId twoId = two->queueId;
    int before = summaries;
    REQUIRE(jobs->update(oneId, statusOf(oneId, ex::ExportPhase::Rendering, 40, 100)) != nullptr);
    CHECK(summaries == before + 1);
    CHECK(jobs->runningCount() == 1);
    CHECK(jobs->heldCount() == 1);
    CHECK(jobs->pausedCount() == 0);
    CHECK(jobs->currentJobId() == first.front());
    CHECK(jobs->currentState() == QStringLiteral("running"));
    CHECK(std::abs(jobs->currentProgress() - 0.4) < 1e-9);
    CHECK(jobs->currentTitle() == QStringLiteral("one.wav"));
    CHECK(jobs->currentEta().endsWith(QStringLiteral("left")));
    // The same report again changes nothing, so nothing is signalled.
    jobs->update(oneId, statusOf(oneId, ex::ExportPhase::Rendering, 40, 100));
    CHECK(summaries == before + 1);

    // Paused while running: paused, not running; still the current job.
    jobs->update(oneId, statusOf(oneId, ex::ExportPhase::Paused, 40, 100));
    CHECK(jobs->pausedCount() == 1);
    CHECK(jobs->runningCount() == 0);
    CHECK(jobs->currentState() == QStringLiteral("paused"));
    CHECK(jobs->currentJobId() == first.front());
    CHECK(jobs->currentEta().isEmpty());

    // The second one is released (queued) while the first is paused. The queue has one worker and the paused export
    // holds it, so the queued one cannot start until the user resumes: it does not count as running (nothing keeps the
    // computer awake, Quit When Done is not offered, the taskbar shows paused). The paused one stays current.
    before = summaries;
    jobs->update(twoId, statusOf(twoId, ex::ExportPhase::Queued));
    CHECK(summaries == before + 1);
    CHECK(jobs->runningCount() == 0);
    CHECK(jobs->heldCount() == 0);
    CHECK(jobs->pausedCount() == 1);
    CHECK(jobs->activeCount() == 2);
    CHECK(jobs->currentJobId() == first.front());
    CHECK(jobs->currentState() == QStringLiteral("paused"));
    // The paused one resumes: running wins over queued.
    jobs->update(oneId, statusOf(oneId, ex::ExportPhase::Rendering, 60, 100));
    CHECK(jobs->runningCount() == 2);
    CHECK(jobs->pausedCount() == 0);
    CHECK(jobs->currentState() == QStringLiteral("running"));
    CHECK(std::abs(jobs->currentProgress() - 0.6) < 1e-9);

    // Cancel everything: the queue reports both cancelled; nothing is left.
    controller.cancelAll();
    REQUIRE(waitFor([&] { return jobs->activeCount() == 0; }, 10000));
    CHECK(jobs->runningCount() == 0);
    CHECK(jobs->pausedCount() == 0);
    CHECK(jobs->heldCount() == 0);
    CHECK(jobs->currentJobId().isEmpty());
    CHECK(jobs->currentTitle().isEmpty());
    CHECK(jobs->currentState().isEmpty());
    CHECK(jobs->currentProgress() == -1.0);
    CHECK(controller.job(first.front()).value(QStringLiteral("state")).toString() == QStringLiteral("cancelled"));
    // A cancelled job has no error kind (only failures do).
    CHECK(controller.job(first.front()).value(QStringLiteral("errorKind")).toString().isEmpty());
    // Clearing the finished rows keeps the summary empty.
    controller.clearFinished();
    CHECK(jobs->rowCount() == 0);
    CHECK(jobs->currentProgress() == -1.0);
}

TEST_CASE("PowerKeeper: the computer is kept awake while a keeper is active and released after", "[app][background]") {
    REQUIRE(PowerKeeper::holds() == 0);
    RestoreSeams restore;
    std::vector<bool> calls;
    PowerKeeper::setBackendForTests([&](bool hold) { calls.push_back(hold); });
    {
        PowerKeeper a;
        PowerKeeper b;
        int changes = 0;
        QObject::connect(&a, &PowerKeeper::activeChanged, [&] { ++changes; });
        CHECK_FALSE(a.active());

        // The first active keeper holds the computer awake.
        a.setActive(true);
        CHECK(a.active());
        CHECK(changes == 1);
        CHECK(calls == std::vector<bool>{true});
        CHECK(PowerKeeper::holds() == 1);
        a.setActive(true); // no change
        CHECK(changes == 1);
        CHECK(calls.size() == 1);

        // A second one shares the hold: no new call.
        b.setActive(true);
        CHECK(calls.size() == 1);
        CHECK(PowerKeeper::holds() == 2);
        a.setActive(false);
        CHECK(calls.size() == 1);
        CHECK(PowerKeeper::holds() == 1);
        CHECK(changes == 2);

        // The last one lets go: released.
        b.setActive(false);
        CHECK(calls == std::vector<bool>{true, false});
        CHECK(PowerKeeper::holds() == 0);

        // A keeper that goes away while active releases its hold.
        {
            PowerKeeper c;
            c.setActive(true);
            CHECK(calls == std::vector<bool>{true, false, true});
            CHECK(PowerKeeper::holds() == 1);
        }
        CHECK(calls == std::vector<bool>{true, false, true, false});
        CHECK(PowerKeeper::holds() == 0);
    }
    // Inactive keepers going away change nothing.
    CHECK(calls.size() == 4);
    CHECK(PowerKeeper::holds() == 0);
}

TEST_CASE("TaskbarProgress: the taskbar shows progress pause and error and flashes only when inactive",
          "[app][background]") {
    RestoreSeams restore;
    std::vector<std::pair<QString, double>> shown;
    int flashes = 0;
    TaskbarProgress::setSinkForTests(
        {[&](const QString& mode, double progress) { shown.emplace_back(mode, progress); }, [&] { ++flashes; }});
    const auto last = [&] { return shown.empty() ? std::pair<QString, double>{} : shown.back(); };
    {
        QQuickWindow window; // never shown: not the active window
        TaskbarProgress taskbar;
        int modes = 0;
        int progresses = 0;
        int windows = 0;
        QObject::connect(&taskbar, &TaskbarProgress::modeChanged, [&] { ++modes; });
        QObject::connect(&taskbar, &TaskbarProgress::progressChanged, [&] { ++progresses; });
        QObject::connect(&taskbar, &TaskbarProgress::windowChanged, [&] { ++windows; });
        CHECK(taskbar.mode() == QStringLiteral("none"));
        CHECK(taskbar.progress() == 0.0);

        // A new button shows no progress: nothing to tell it.
        taskbar.setWindow(&window);
        CHECK(windows == 1);
        CHECK(taskbar.window() == &window);
        CHECK(shown.empty());

        // Each change reaches the taskbar once.
        taskbar.setMode(QStringLiteral("normal"));
        REQUIRE(shown.size() == 1);
        CHECK(last().first == QStringLiteral("normal"));
        CHECK(last().second == 0.0);
        taskbar.setProgress(0.4);
        REQUIRE(shown.size() == 2);
        CHECK(last() == std::pair<QString, double>{QStringLiteral("normal"), 0.4});
        taskbar.setProgress(0.4002); // the same permille: the taskbar is not told again
        CHECK(shown.size() == 2);
        CHECK(progresses == 2);
        taskbar.setMode(QStringLiteral("paused"));
        REQUIRE(shown.size() == 3);
        CHECK(last().first == QStringLiteral("paused"));
        taskbar.setMode(QStringLiteral("paused"));
        CHECK(shown.size() == 3);
        CHECK(modes == 2);

        // Unknown modes read "none"; the progress does not matter without a value.
        taskbar.setMode(QStringLiteral("bogus"));
        CHECK(taskbar.mode() == QStringLiteral("none"));
        REQUIRE(shown.size() == 4);
        CHECK(last().first == QStringLiteral("none"));
        taskbar.setProgress(0.9);
        CHECK(shown.size() == 4);
        taskbar.setMode(QStringLiteral("indeterminate"));
        REQUIRE(shown.size() == 5);
        CHECK(last().first == QStringLiteral("indeterminate"));
        taskbar.setProgress(0.95);
        CHECK(shown.size() == 5);

        // A failed export: red, full; progress is clamped to 0..1.
        taskbar.setMode(QStringLiteral("error"));
        taskbar.setProgress(1.5);
        CHECK(taskbar.progress() == 1.0);
        REQUIRE(shown.size() == 7);
        CHECK(last() == std::pair<QString, double>{QStringLiteral("error"), 1.0});

        // The window is not active: the button flashes. Without a window nothing does.
        CHECK_FALSE(window.isActive());
        CHECK(taskbar.flash());
        CHECK(flashes == 1);
        TaskbarProgress lone;
        CHECK_FALSE(lone.flash());
        CHECK(flashes == 1);
    }
    // The keeper went away while showing an error: the button is back to no progress (the lone one showed nothing).
    REQUIRE(shown.size() == 8);
    CHECK(last().first == QStringLiteral("none"));
}

TEST_CASE("Quit guard: running or waiting exports ask before the window closes", "[app][shell][background][qml]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("quit-guard");
    BackgroundShell b;
    Shell& shell = b.shell;
    INFO(join(shell.errors));
    QObject* dialog = shell.object("backgroundWorkCloseDialog");
    REQUIRE(dialog != nullptr);
    QQuickItem* keepOpen = inPopup(dialog, QStringLiteral("backgroundWorkKeepOpen"));
    QQuickItem* quitWhenDone = inPopup(dialog, QStringLiteral("backgroundWorkQuitWhenDone"));
    QQuickItem* cancelAndQuit = inPopup(dialog, QStringLiteral("backgroundWorkCancelAndQuit"));
    REQUIRE(keepOpen != nullptr);
    REQUIRE(quitWhenDone != nullptr);
    REQUIRE(cancelAndQuit != nullptr);
    const auto opened = [&] { return dialog->property("opened").toBool(); };
    const auto message = [&] { return dialog->property("message").toString(); };

    // Nothing runs: closing does not ask.
    CHECK(readyToClose(shell));
    CHECK_FALSE(dialog->property("visible").toBool());

    // An export waits for Start Queue: closing asks; Quit When Done is not offered (nothing would end on its own).
    const QString job = holdExport(*b.exporter, dir / "held.wav");
    CHECK_FALSE(readyToClose(shell));
    REQUIRE(waitFor(opened));
    CHECK(dialog->property("title").toString() == QStringLiteral("Quit with exports in the queue?"));
    CHECK(message().contains(QStringLiteral("1 export waits for Start Queue.")));
    CHECK(message().contains(QStringLiteral("Exports that have not finished are lost when Wabcut closes.")));
    CHECK_FALSE(dialog->property("canWait").toBool());
    CHECK_FALSE(quitWhenDone->isVisible());
    CHECK(cancelAndQuit->property("text").toString() == QStringLiteral("Cancel Exports and Quit"));
    // A second close while it asks keeps the question open.
    CHECK_FALSE(readyToClose(shell));
    CHECK(opened());
    // Keep Wabcut Open: nothing changes.
    REQUIRE(QMetaObject::invokeMethod(keepOpen, "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(b.closes == 0);
    CHECK(stateOf(*b.exporter, job) == QStringLiteral("held"));

    // The export renders (40 %): the question says so, live, and offers Quit When Done.
    feed(*b.exporter, job, ex::ExportPhase::Rendering, 40, 100);
    CHECK(evaluate(shell, QStringLiteral("window.backgroundWorkRunning")).toBool());
    CHECK_FALSE(readyToClose(shell));
    REQUIRE(waitFor(opened));
    CHECK(dialog->property("title").toString() == QStringLiteral("Quit while exporting?"));
    CHECK(message().contains(QStringLiteral("“held.wav” is exporting (40 %).")));
    CHECK(dialog->property("canWait").toBool());
    CHECK(quitWhenDone->isVisible());
    feed(*b.exporter, job, ex::ExportPhase::Rendering, 45, 100);
    CHECK(message().contains(QStringLiteral("is exporting (45 %)")));
    // Esc (a close without a button) is Keep Wabcut Open; then ask again for the screenshots.
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(b.closes == 0);
    CHECK_FALSE(readyToClose(shell));
    REQUIRE(waitFor(opened));
    spin(400);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/background-work-close.png")));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/background-work-close-light.png")));

    // Cancel Exports and Quit: the export is cancelled and the window closes (nothing unsaved asks).
    REQUIRE(QMetaObject::invokeMethod(cancelAndQuit, "clicked"));
    REQUIRE(waitFor([&] { return stateOf(*b.exporter, job) == QStringLiteral("cancelled"); }));
    REQUIRE(waitFor([&] { return b.closes == 1; }));
    CHECK(evaluate(shell, QStringLiteral("window.backgroundWorkAnswered")).toBool());
    CHECK(readyToClose(shell));
    CHECK(b.closes == 1);
    // A new export forgets that answer: closing asks again.
    holdExport(*b.exporter, dir / "again.wav");
    CHECK_FALSE(evaluate(shell, QStringLiteral("window.backgroundWorkAnswered")).toBool());
    CHECK_FALSE(readyToClose(shell));
    REQUIRE(waitFor(opened));
    REQUIRE(QMetaObject::invokeMethod(keepOpen, "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    b.exporter->cancelAll();
    REQUIRE(waitFor([&] { return b.exporter->jobs()->activeCount() == 0; }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Quit guard: Quit When Done closes once the running work ends", "[app][shell][background][qml]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("quit-when-done");
    {
        BackgroundShell b;
        Shell& shell = b.shell;
        INFO(join(shell.errors));
        QObject* dialog = shell.object("backgroundWorkCloseDialog");
        REQUIRE(dialog != nullptr);
        const QString job = holdExport(*b.exporter, dir / "done.wav");
        feed(*b.exporter, job, ex::ExportPhase::Rendering, 40, 100);
        CHECK_FALSE(readyToClose(shell));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
        REQUIRE(QMetaObject::invokeMethod(inPopup(dialog, QStringLiteral("backgroundWorkQuitWhenDone")), "clicked"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
        // The window stays and says it will close; Keep Wabcut Open on that toast would call it off.
        CHECK(evaluate(shell, QStringLiteral("window.quitWhenIdle")).toBool());
        CHECK(b.toastText().contains(QStringLiteral("will close")));
        CHECK(b.toast->property("actionText").toString() == QStringLiteral("Keep Wabcut Open"));
        feed(*b.exporter, job, ex::ExportPhase::Rendering, 80, 100);
        CHECK(b.closes == 0);
        // The export is done: Wabcut closes as if the user closed it, the question answered.
        feed(*b.exporter, job, ex::ExportPhase::Done, 100, 100);
        REQUIRE(waitFor([&] { return b.closes == 1; }));
        CHECK_FALSE(evaluate(shell, QStringLiteral("window.quitWhenIdle")).toBool());
        CHECK(readyToClose(shell));
    }
    {
        // A failure meanwhile calls the quit off, and its message says Wabcut stayed open.
        BackgroundShell b;
        Shell& shell = b.shell;
        QObject* dialog = shell.object("backgroundWorkCloseDialog");
        REQUIRE(dialog != nullptr);
        const QString job = holdExport(*b.exporter, dir / "failed.wav");
        feed(*b.exporter, job, ex::ExportPhase::Rendering, 40, 100);
        CHECK_FALSE(readyToClose(shell));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
        REQUIRE(QMetaObject::invokeMethod(inPopup(dialog, QStringLiteral("backgroundWorkQuitWhenDone")), "clicked"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
        REQUIRE(evaluate(shell, QStringLiteral("window.quitWhenIdle")).toBool());
        REQUIRE(QMetaObject::invokeMethod(b.exporter, "jobFinished", Q_ARG(QString, job), Q_ARG(bool, false),
                                          Q_ARG(QString, QStringLiteral("The disk is full."))));
        spin(50);
        CHECK_FALSE(evaluate(shell, QStringLiteral("window.quitWhenIdle")).toBool());
        CHECK(b.toastText().startsWith(QStringLiteral("Export failed")));
        CHECK(b.toastText().endsWith(QStringLiteral("Wabcut stayed open.")));
        // The work ending later does not close the window any more.
        feed(*b.exporter, job, ex::ExportPhase::Done, 100, 100);
        spin(100);
        CHECK(b.closes == 0);
        // Keep Wabcut Open on the "will close" toast calls a pending quit off too.
        const QString other = holdExport(*b.exporter, dir / "other.wav");
        feed(*b.exporter, other, ex::ExportPhase::Rendering, 10, 100);
        evaluate(shell, QStringLiteral("toast.dismissAll()"));
        CHECK_FALSE(readyToClose(shell));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
        REQUIRE(QMetaObject::invokeMethod(inPopup(dialog, QStringLiteral("backgroundWorkQuitWhenDone")), "clicked"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
        REQUIRE(QMetaObject::invokeMethod(b.toast, "runAction"));
        CHECK_FALSE(evaluate(shell, QStringLiteral("window.quitWhenIdle")).toBool());
        feed(*b.exporter, other, ex::ExportPhase::Done, 100, 100);
        spin(100);
        CHECK(b.closes == 0);
        b.exporter->cancelAll();
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Activity pill: exports show in the title bar and the popover lists them with the recent messages",
          "[app][shell][background][qml]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("activity");
    BackgroundShell b;
    Shell& shell = b.shell;
    INFO(join(shell.errors));
    QQuickItem* pill = shell.item("activityPill");
    QObject* popover = shell.object("activityPopover");
    REQUIRE(pill != nullptr);
    REQUIRE(popover != nullptr);
    evaluate(shell, QStringLiteral("MessageLog.clear()"));
    const auto pillText = [&] { return pill->property("text").toString(); };
    const auto popoverOpen = [&] { return popover->property("opened").toBool(); };

    // Nothing runs: no pill; the popover still opens (the command window.backgroundTasks) and says so.
    CHECK_FALSE(pill->isVisible());
    evaluate(shell, QStringLiteral("window.runCommand('window.backgroundTasks')"));
    REQUIRE(waitFor(popoverOpen));
    QQuickItem* nothing = inPopup(popover, QStringLiteral("activityNothing"));
    REQUIRE(nothing != nullptr);
    CHECK(nothing->isVisible());
    REQUIRE(QMetaObject::invokeMethod(popover, "close"));
    REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }));

    // Two exports wait for Start Queue (a wide window: the pill has room for its full text next to the menus).
    shell.resize(1920, 1080);
    spin(50);
    const QString first = holdExport(*b.exporter, dir / "Highlights.wav");
    const QString second = holdExport(*b.exporter, dir / "Teaser.wav");
    CHECK(pill->isVisible());
    CHECK(pillText() == QStringLiteral("2 exports waiting"));
    // One renders (38 %): it leads, the other counts as one more.
    feed(*b.exporter, first, ex::ExportPhase::Rendering, 38, 100);
    CHECK(pillText().startsWith(QStringLiteral("Exporting 38 %")));
    CHECK(pillText().contains(QStringLiteral("left")));
    CHECK(pillText().endsWith(QStringLiteral("+1")));
    QQuickItem* beam = findUnder(pill, QStringLiteral("activityPillProgress"));
    REQUIRE(beam != nullptr);
    CHECK(beam->isVisible());

    // Some messages for the popover's list (newest first): a finished export with its action, a warning, an error.
    evaluate(shell, QStringLiteral("MessageLog.add('Exported Final cut.mp4', 'success', 'Open', () => {})"));
    evaluate(shell, QStringLiteral("MessageLog.add('Proxies paused while you edit', 'warning', '', null)"));
    evaluate(shell, QStringLiteral("MessageLog.add('Export failed: The disk is full.', 'danger', 'Show the queue', () => {})"));

    // Click: the popover lists both exports and the messages.
    shell.resize(kShotWidth, kShotHeight);
    spin(50);
    // Short of room the pill shortens itself before the menus fold.
    CHECK_FALSE(evaluate(shell, QStringLiteral("topBar.menusCollapsed")).toBool());
    const bool fullOrShort = pillText().startsWith(QStringLiteral("Exporting 38 %")) || pillText() == QStringLiteral("38 %");
    CHECK(fullOrShort);
    REQUIRE(QMetaObject::invokeMethod(pill, "clicked"));
    REQUIRE(waitFor(popoverOpen));
    QQuickItem* rowOne = inPopup(popover, QStringLiteral("activityExport_") + first);
    QQuickItem* rowTwo = inPopup(popover, QStringLiteral("activityExport_") + second);
    REQUIRE(rowOne != nullptr);
    REQUIRE(rowTwo != nullptr);
    CHECK_FALSE(nothing->isVisible());
    QQuickItem* startQueue = inPopup(popover, QStringLiteral("activityStartQueue"));
    REQUIRE(startQueue != nullptr);
    CHECK(startQueue->isVisible());
    QQuickItem* messages = inPopup(popover, QStringLiteral("activityMessages"));
    REQUIRE(messages != nullptr);
    CHECK(messages->isVisible());
    CHECK(messages->property("count").toInt() == 3);
    spin(400);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/activity-popover.png")));
    // A row's own cancel stops that export.
    QQuickItem* cancel = findUnder(rowOne, QStringLiteral("exportJobCancel"));
    REQUIRE(cancel != nullptr);
    REQUIRE(QMetaObject::invokeMethod(cancel, "clicked"));
    REQUIRE(waitFor([&] { return b.exporter->jobs()->activeCount() == 1; }));
    CHECK(stateOf(*b.exporter, second) == QStringLiteral("held"));
    // Export Queue... closes the popover and shows the queue (the Export workspace).
    QQuickItem* openQueue = inPopup(popover, QStringLiteral("activityOpenQueue"));
    REQUIRE(openQueue != nullptr);
    REQUIRE(QMetaObject::invokeMethod(openQueue, "clicked"));
    REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }));
    CHECK(evaluate(shell, QStringLiteral("docking.isOpen('export')")).toBool());

    // Nothing runs any more; an error toast nobody saw (it left on its time): the pill says "1 problem".
    b.exporter->cancelAll();
    REQUIRE(waitFor([&] { return b.exporter->jobs()->activeCount() == 0; }));
    CHECK_FALSE(pill->isVisible());
    evaluate(shell, QStringLiteral("toast.dismissAll()"));
    evaluate(shell, QStringLiteral("toast.show('Autosave could not save your project', Theme.danger)"));
    QObject* timer = b.toast->findChild<QObject*>(QStringLiteral("toastHideTimer"));
    REQUIRE(timer != nullptr);
    REQUIRE(QMetaObject::invokeMethod(timer, "triggered"));
    CHECK(evaluate(shell, QStringLiteral("MessageLog.unseen")).toInt() == 1);
    CHECK(pill->isVisible());
    shell.resize(1920, 1080);
    spin(50);
    CHECK(pillText() == QStringLiteral("1 problem"));
    CHECK(pill->property("iconName").toString() == QStringLiteral("triangle-alert"));
    // Opening the popover marks it seen: the pill goes.
    REQUIRE(QMetaObject::invokeMethod(pill, "clicked"));
    REQUIRE(waitFor(popoverOpen));
    CHECK(evaluate(shell, QStringLiteral("MessageLog.unseen")).toInt() == 0);
    CHECK_FALSE(pill->isVisible());
    // Clear forgets the messages.
    QQuickItem* clear = inPopup(popover, QStringLiteral("activityClearMessages"));
    REQUIRE(clear != nullptr);
    REQUIRE(QMetaObject::invokeMethod(clear, "clicked"));
    CHECK(evaluate(shell, QStringLiteral("MessageLog.count")).toInt() == 0);
    REQUIRE(QMetaObject::invokeMethod(popover, "close"));
    REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }));

    // A message's action runs and closes the popover.
    evaluate(shell, QStringLiteral("MessageLog.add('Exported Final cut.mp4', 'success', 'Open', () => toast.objectName = 'opened')"));
    evaluate(shell, QStringLiteral("topBar.openActivity()"));
    REQUIRE(waitFor(popoverOpen));
    QQuickItem* action = inPopup(popover, QStringLiteral("activityMessageAction_0"));
    REQUIRE(action != nullptr);
    CHECK(action->isVisible());
    REQUIRE(QMetaObject::invokeMethod(action, "clicked"));
    CHECK(b.toast->objectName() == QStringLiteral("opened"));
    b.toast->setObjectName(QStringLiteral("toast"));
    REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }));

    // A short window folds the menus: the pill shows only the percentage.
    const QString third = holdExport(*b.exporter, dir / "Short.wav");
    feed(*b.exporter, third, ex::ExportPhase::Rendering, 38, 100);
    shell.resize(1100, 700);
    spin(100);
    QObject* topBar = evaluate(shell, QStringLiteral("topBar")).value<QObject*>();
    REQUIRE(topBar != nullptr);
    CHECK(topBar->property("menusCollapsed").toBool());
    CHECK(pillText() == QStringLiteral("38 %"));
    spin(300);
    const QImage compact = shell.grab();
    REQUIRE_FALSE(compact.isNull());
    const double dpr = compact.devicePixelRatio();
    const int barHeight = int(std::lround((qobject_cast<QQuickItem*>(topBar)->height() + 8) * dpr));
    REQUIRE(compact.copy(0, 0, compact.width(), barHeight).save(outputDir() + QStringLiteral("/activity-pill-compact.png")));
    b.exporter->cancelAll();
    REQUIRE(waitFor([&] { return b.exporter->jobs()->activeCount() == 0; }));

    const QStringList qtMessages = takeMessages();
    INFO(join(qtMessages));
    CHECK(qtMessages.isEmpty());
}

TEST_CASE("Keep awake and taskbar: a running export holds the computer awake and shows its progress",
          "[app][shell][background][qml]") {
    resetSettings();
    clearMessages();
    RestoreSeams restore;
    REQUIRE(PowerKeeper::holds() == 0);
    std::vector<bool> holds;
    PowerKeeper::setBackendForTests([&](bool hold) { holds.push_back(hold); });
    std::vector<std::pair<QString, double>> shown;
    int flashes = 0;
    TaskbarProgress::setSinkForTests(
        {[&](const QString& mode, double progress) { shown.emplace_back(mode, progress); }, [&] { ++flashes; }});
    const auto mode = [&] { return shown.empty() ? QStringLiteral("none") : shown.back().first; };
    const fs::path dir = freshDir("keep-awake");
    {
        BackgroundShell b;
        Shell& shell = b.shell;
        INFO(join(shell.errors));
        QObject* keeper = shell.object("powerKeeper");
        QObject* taskbar = shell.object("taskbarProgress");
        REQUIRE(keeper != nullptr);
        REQUIRE(taskbar != nullptr);
        CHECK(taskbar->property("window").value<QQuickWindow*>() == shell.window.get());

        // A held export does not run: no hold, no progress.
        const QString job = holdExport(*b.exporter, dir / "held.wav");
        CHECK(PowerKeeper::holds() == 0);
        CHECK(mode() == QStringLiteral("none"));
        // It renders: Windows is kept awake and the taskbar shows 40 %.
        feed(*b.exporter, job, ex::ExportPhase::Rendering, 40, 100);
        CHECK(keeper->property("active").toBool());
        CHECK(PowerKeeper::holds() == 1);
        CHECK(holds == std::vector<bool>{true});
        CHECK(mode() == QStringLiteral("normal"));
        REQUIRE_FALSE(shown.empty());
        CHECK(std::abs(shown.back().second - 0.4) < 1e-3);
        // Paused: it lets go, the button turns yellow.
        feed(*b.exporter, job, ex::ExportPhase::Paused, 40, 100);
        CHECK(PowerKeeper::holds() == 0);
        CHECK(mode() == QStringLiteral("paused"));
        // A second export released behind the paused one cannot start (the paused export holds the queue's only
        // worker): still no hold, still paused, and nothing runs that Quit When Done could wait for.
        const QString waiting = holdExport(*b.exporter, dir / "waiting.wav");
        feed(*b.exporter, waiting, ex::ExportPhase::Queued);
        CHECK(b.exporter->jobs()->runningCount() == 0);
        CHECK_FALSE(keeper->property("active").toBool());
        CHECK(PowerKeeper::holds() == 0);
        CHECK(mode() == QStringLiteral("paused"));
        CHECK_FALSE(evaluate(shell, QStringLiteral("window.backgroundWorkRunning")).toBool());
        b.exporter->cancel(waiting);
        REQUIRE(waitFor([&] { return b.exporter->jobs()->activeCount() == 1; }));
        // The setting off: no hold while it renders again; on again: held.
        shell.preferences->setProperty("keepAwake", false);
        feed(*b.exporter, job, ex::ExportPhase::Rendering, 50, 100);
        CHECK(PowerKeeper::holds() == 0);
        CHECK(mode() == QStringLiteral("normal"));
        shell.preferences->setProperty("keepAwake", true);
        spin(30);
        CHECK(PowerKeeper::holds() == 1);
        // It fails while Wabcut is not the active window (a hidden test window never is): red, and the button flashes.
        CHECK(flashes == 0);
        REQUIRE(QMetaObject::invokeMethod(b.exporter, "jobFinished", Q_ARG(QString, job), Q_ARG(bool, false),
                                          Q_ARG(QString, QStringLiteral("The disk is full."))));
        spin(30);
        CHECK(flashes == 1);
        CHECK(mode() == QStringLiteral("error"));
        CHECK(evaluate(shell, QStringLiteral("window.exportFailedUnseen")).toBool());
        // Its message waits to be seen (the window is not active).
        CHECK(b.toast->property("untilSeen").toBool());
        QObject* timer = b.toast->findChild<QObject*>(QStringLiteral("toastHideTimer"));
        REQUIRE(timer != nullptr);
        CHECK_FALSE(timer->property("running").toBool());
        // Cancelled: nothing holds the computer; the button stays red until Wabcut is active again.
        b.exporter->cancel(job);
        REQUIRE(waitFor([&] { return b.exporter->jobs()->activeCount() == 0; }));
        CHECK(PowerKeeper::holds() == 0);
        CHECK(mode() == QStringLiteral("error"));
        evaluate(shell, QStringLiteral("window.exportFailedUnseen = false")); // what activating the window does
        CHECK(mode() == QStringLiteral("none"));
        CHECK(holds == std::vector<bool>{true, false, true, false});

        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
    CHECK(PowerKeeper::holds() == 0);
}

TEST_CASE("Quit guard: a close given up at the unsaved-changes question asks about the exports again next time",
          "[app][shell][background][qml]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("quit-again");
    BackgroundShell b;
    Shell& shell = b.shell;
    INFO(join(shell.errors));
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(file != nullptr);
    QObject* dialog = shell.object("backgroundWorkCloseDialog");
    QObject* unsaved = shell.object("unsavedChangesDialog");
    REQUIRE(dialog != nullptr);
    REQUIRE(unsaved != nullptr);
    const auto asking = [&] { return dialog->property("visible").toBool(); };
    // A titled project with unsaved changes.
    REQUIRE(file->saveAs(QUrl::fromLocalFile(q(dir / "Film.wabcut"))));
    evaluate(shell, QStringLiteral("Markers.add(false)"));
    REQUIRE(file->dirty());

    // Export A renders, export B waits for Start Queue. The window's × asks; Quit When Done.
    const QString a = holdExport(*b.exporter, dir / "a.wav");
    const QString later = holdExport(*b.exporter, dir / "b.wav");
    feed(*b.exporter, a, ex::ExportPhase::Rendering, 40, 100);
    shell.window->close();
    spin(50);
    CHECK(b.closes == 1);
    REQUIRE(waitFor(asking));
    REQUIRE(QMetaObject::invokeMethod(inPopup(dialog, QStringLiteral("backgroundWorkQuitWhenDone")), "clicked"));
    REQUIRE(waitFor([&] { return !asking(); }));
    // A is done: the close goes on (the question answered) to "Save changes?" - and the user picks Cancel there.
    feed(*b.exporter, a, ex::ExportPhase::Done, 100, 100);
    REQUIRE(waitFor([&] { return b.closes == 2; }));
    REQUIRE(waitFor([&] { return unsaved->property("visible").toBool(); }));
    CHECK_FALSE(asking());
    REQUIRE(QMetaObject::invokeMethod(unsaved, "close"));
    REQUIRE(waitFor([&] { return !unsaved->property("visible").toBool(); }));
    CHECK(stateOf(*b.exporter, later) == QStringLiteral("held"));

    // A new close (the ×, File > Exit, Alt+F4) asks about B again: the earlier answer was for the close given up.
    shell.window->close();
    spin(50);
    CHECK(b.closes == 3);
    REQUIRE(waitFor(asking));
    CHECK_FALSE(unsaved->property("visible").toBool());
    CHECK(dialog->property("message").toString().contains(QStringLiteral("1 export waits for Start Queue.")));
    REQUIRE(QMetaObject::invokeMethod(inPopup(dialog, QStringLiteral("backgroundWorkKeepOpen")), "clicked"));
    REQUIRE(waitFor([&] { return !asking(); }));
    CHECK(stateOf(*b.exporter, later) == QStringLiteral("held"));
    // More work (B released: Start Queue) forgets an answer too.
    evaluate(shell, QStringLiteral("window.backgroundWorkAnswered = true"));
    feed(*b.exporter, later, ex::ExportPhase::Queued);
    CHECK_FALSE(evaluate(shell, QStringLiteral("window.backgroundWorkAnswered")).toBool());
    b.exporter->cancelAll();
    REQUIRE(waitFor([&] { return b.exporter->jobs()->activeCount() == 0; }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Quit guard: work that ends while the question is open closes it and the close goes on unless it failed",
          "[app][shell][background][qml]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("quit-settles");
    {
        // The export finishes while Wabcut asks: the question closes, and the close the user asked for goes on.
        BackgroundShell b;
        Shell& shell = b.shell;
        INFO(join(shell.errors));
        QObject* dialog = shell.object("backgroundWorkCloseDialog");
        REQUIRE(dialog != nullptr);
        const QString job = holdExport(*b.exporter, dir / "done.wav");
        feed(*b.exporter, job, ex::ExportPhase::Rendering, 98, 100);
        shell.window->close();
        spin(50);
        REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
        CHECK(b.closes == 1);
        CHECK(dialog->property("title").toString() == QStringLiteral("Quit while exporting?"));
        feed(*b.exporter, job, ex::ExportPhase::Done, 100, 100);
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
        REQUIRE(waitFor([&] { return b.closes == 2; }));
        // Nothing it named runs any more: its wording says so (never "Quit while rendering?" with an empty body).
        CHECK(dialog->property("title").toString() == QStringLiteral("Quit Wabcut?"));
        CHECK(dialog->property("message").toString() == QStringLiteral("The work in progress is done."));
        CHECK_FALSE(evaluate(shell, QStringLiteral("window.quitWhenIdle")).toBool());
    }
    {
        // It fails while Wabcut asks: the question closes, Wabcut stays open and the failure says so.
        BackgroundShell b;
        Shell& shell = b.shell;
        QObject* dialog = shell.object("backgroundWorkCloseDialog");
        REQUIRE(dialog != nullptr);
        const QString job = holdExport(*b.exporter, dir / "failed.wav");
        feed(*b.exporter, job, ex::ExportPhase::Rendering, 60, 100);
        shell.window->close();
        spin(50);
        REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
        // As the controller reports it: the row finishes, then jobFinished with the reason (one event).
        const ExportJobModel::Row* row = b.exporter->jobs()->find(job);
        REQUIRE(row != nullptr);
        ex::ExportJobStatus status = statusOf(row->queueId, ex::ExportPhase::Failed);
        REQUIRE(b.exporter->jobs()->update(row->queueId, status) != nullptr);
        REQUIRE(QMetaObject::invokeMethod(b.exporter, "jobFinished", Q_ARG(QString, job), Q_ARG(bool, false),
                                          Q_ARG(QString, QStringLiteral("The disk is full."))));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
        spin(100);
        CHECK(b.closes == 1);
        CHECK(b.toastText().startsWith(QStringLiteral("Export failed")));
        CHECK(b.toastText().endsWith(QStringLiteral("Wabcut stayed open.")));
        CHECK_FALSE(evaluate(shell, QStringLiteral("window.backgroundWorkAnswered")).toBool());
        b.exporter->cancelAll();
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Activity popover: a press elsewhere in the title bar closes it and a long message opens in full",
          "[app][shell][background][qml]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("activity-close");
    BackgroundShell b;
    Shell& shell = b.shell;
    INFO(join(shell.errors));
    shell.resize(1920, 1080);
    spin(50);
    QQuickItem* pill = shell.item("activityPill");
    QQuickItem* tabs = shell.item("workspaceTabs");
    QObject* popover = shell.object("activityPopover");
    REQUIRE(pill != nullptr);
    REQUIRE(tabs != nullptr);
    REQUIRE(popover != nullptr);
    evaluate(shell, QStringLiteral("MessageLog.clear()"));
    const auto open = [&] { return popover->property("visible").toBool(); };
    // Through the window system, as the platform delivers the pointer (as test_menu_bar does: presses on the title bar
    // then reach its buttons, not the window-drag surface under them).
    const auto mouse = [&](QEvent::Type type, QPointF scene, Qt::MouseButtons buttons) {
        QQuickWindow* window = shell.window.get();
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            window, scene, window->mapToGlobal(scene), buttons, button, type);
    };
    const auto pressAt = [&](QPointF scene) {
        mouse(QEvent::MouseMove, scene, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, scene, Qt::LeftButton);
        spin(40);
    };
    const auto releaseAt = [&](QPointF scene) {
        mouse(QEvent::MouseButtonRelease, scene, Qt::NoButton);
        spin(80);
    };
    const auto clickAt = [&](QPointF scene) {
        pressAt(scene);
        releaseAt(scene);
    };
    // An empty spot of the title bar: between the activity pill (or where it would be) and the workspace tabs.
    const auto emptySpot = [&] {
        const QPointF left = pill->mapToScene(QPointF(pill->isVisible() ? pill->width() : 0, pill->height() / 2));
        const QPointF right = tabs->mapToScene(QPointF(0, 0));
        return QPointF((left.x() + right.x()) / 2, left.y());
    };

    const QString job = holdExport(*b.exporter, dir / "Highlights.wav");
    REQUIRE(pill->isVisible());
    shell.grab(); // a hidden window lays its rows out only when it renders: the pill takes its place now
    INFO("empty spot " << emptySpot().x() << " between " << pill->mapToScene(QPointF(pill->width(), 0)).x() << " and "
                       << tabs->mapToScene(QPointF(0, 0)).x());
    REQUIRE(tabs->mapToScene(QPointF(0, 0)).x() - pill->mapToScene(QPointF(pill->width(), 0)).x() > 40);
    // A click on the pill opens it. It hangs from the pill: a press elsewhere in the title bar (a menu, a tab, Save)
    // closes it.
    const QPointF onPill = pill->mapToScene(QPointF(pill->width() / 2, pill->height() / 2));
    pressAt(onPill);
    CHECK(pill->property("pressed").toBool());
    releaseAt(onPill);
    REQUIRE(waitFor(open));
    CHECK(popover->property("parent").value<QQuickItem*>() == shell.item("activityAnchor"));
    clickAt(emptySpot());
    REQUIRE(waitFor([&] { return !open(); }));
    // A click on the pill while it is open closes it (and does not open it again).
    REQUIRE(QMetaObject::invokeMethod(pill, "clicked"));
    REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }));
    pressAt(onPill);
    CHECK(popover->property("opened").toBool()); // a press on the pill is the pill's
    CHECK(pill->property("pressed").toBool());
    releaseAt(onPill);
    REQUIRE(waitFor([&] { return !open(); }));
    spin(100);
    CHECK_FALSE(open());
    // Nothing runs (no pill): opened by the command (where the pill would be), a press elsewhere closes it too (below).
    b.exporter->cancel(job);
    REQUIRE(waitFor([&] { return b.exporter->jobs()->activeCount() == 0; }));
    evaluate(shell, QStringLiteral("MessageLog.clear()"));
    spin(50);
    REQUIRE_FALSE(pill->isVisible());
    shell.grab();
    // A long message (an autosave failure with its path) shows two lines; a click shows all of it.
    const QString longText = QStringLiteral("Autosave could not save your project. Save it somewhere else to keep your work "
                                            "safe. cannot create: G:/dev/tmp/a very long folder name/another long folder "
                                            "name/and one more/Film.wabcut.autosave.tmp: Access is denied.");
    evaluate(shell, QStringLiteral("MessageLog.add('%1', 'danger', '', null)").arg(longText));
    evaluate(shell, QStringLiteral("topBar.openActivity()"));
    REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }));
    spin(100);
    // A hidden window lays the popover's columns out only when it renders: until then the message list is 0 wide and
    // its text does not wrap (a shown window renders at once).
    shell.grab();
    spin(50);
    QQuickItem* entry = inPopup(popover, QStringLiteral("activityMessage_0"));
    QQuickItem* entryText = inPopup(popover, QStringLiteral("activityMessageText_0"));
    REQUIRE(entry != nullptr);
    REQUIRE(entryText != nullptr);
    INFO("message text " << entryText->width() << " wide, " << entryText->property("lineCount").toInt() << " lines");
    REQUIRE(entryText->width() > 100);
    CHECK(entryText->property("text").toString() == longText);
    CHECK(entry->property("cut").toBool());
    CHECK(entryText->property("lineCount").toInt() == 2);
    const double shortHeight = entry->height();
    REQUIRE(QMetaObject::invokeMethod(entry, "toggleExpanded"));
    shell.grab();
    spin(50);
    CHECK(entry->property("expanded").toBool());
    CHECK_FALSE(entryText->property("truncated").toBool());
    CHECK(entryText->property("lineCount").toInt() > 2);
    CHECK(entry->height() > shortHeight);
    // A second click: two lines again.
    REQUIRE(QMetaObject::invokeMethod(entry, "toggleExpanded"));
    shell.grab();
    spin(50);
    CHECK_FALSE(entry->property("expanded").toBool());
    CHECK(entry->property("cut").toBool());
    CHECK(entry->height() == shortHeight);
    // A short message is not cut: a click leaves it as it is.
    evaluate(shell, QStringLiteral("MessageLog.add('Exported Teaser.wav', 'success', '', null)"));
    shell.grab();
    spin(50);
    QQuickItem* shortEntry = inPopup(popover, QStringLiteral("activityMessage_0"));
    REQUIRE(shortEntry != nullptr);
    CHECK_FALSE(shortEntry->property("cut").toBool());
    REQUIRE(QMetaObject::invokeMethod(shortEntry, "toggleExpanded"));
    CHECK_FALSE(shortEntry->property("expanded").toBool());
    clickAt(emptySpot());
    REQUIRE(waitFor([&] { return !open(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
