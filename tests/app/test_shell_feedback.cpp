// UX rework (stream S): the editor shell's feedback, offscreen in the real Main.qml.
//   * The toast: show(message, color) as before, and show(message, color, { actionText, action, timeoutMs }) with an
//     action pill and a close button; problems and toasts with an action stay longer. The pointer holds it once it
//     moved onto it; a toast that appears under a resting pointer still leaves on time.
//   * Opening and saving never fail silently: the reason shows as a warning toast in the editor (a save failure offers
//     Save As...) or in Home's message strip while Home is open (with the same Save As... there). Background work that
//     ends while Home covers the editor (exports, Render In to Out) says so on Home's strip too.
//   * Leaving the edited sample project (Open, Open Next to This One, Import, Home's Create...) asks "Discard your
//     changes to the sample project?" first, once, before anything is imported.
//   * The History panel's keys: Left / Right step one state back / forward while it has the focus.
//   * Reveal in Project (Editing.libraryRequested) brings the Library forward.
//   * 0.4: an error toast is never replaced (what comes after it waits; MessageLog keeps every message), a second
//     action and untilSeen; a failed autosave toasts once per failure streak with Save As… and turns the save dot red.

#include "ShellFixture.h"

#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineFixture.h"

#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <filesystem>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// Runs `code` in Main.qml's context (its ids: window, toast, home, docking, ...).
QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

void tapKey(QQuickWindow* window, int key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &release);
    spin(50);
}

/// An undoable edit of the project shown: a sequence marker at the playhead, moved on first so each one is new.
void makeEdit(Shell& shell, int second) {
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller != nullptr);
    controller->setPlayhead(Ticks{kTicksPerSecond * second});
    const std::size_t before = controller->session().undoCount();
    evaluate(shell, QStringLiteral("Markers.add(false)"));
    REQUIRE(controller->session().undoCount() == before + 1);
}

/// The pointer moves (no button) to `scene` in the window: hover.
void hoverAt(QQuickWindow* window, QPointF scene) {
    QMouseEvent event(QEvent::MouseMove, scene, scene, window->mapToGlobal(scene), Qt::NoButton, Qt::NoButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
    spin(30);
}

/// The first item named `name` under `root` (the item tree, so Loaders' and Repeaters' items too).
QQuickItem* findUnder(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findUnder(child, name))
            return found;
    }
    return nullptr;
}

/// A QML list of URLs for `files` (local paths), for a call through QQmlExpression.
QString urlList(const QStringList& files) {
    QStringList quoted;
    for (const QString& f : files)
        quoted << QStringLiteral("'%1'").arg(QUrl::fromLocalFile(f).toString());
    return QStringLiteral("[") + quoted.join(QStringLiteral(", ")) + QStringLiteral("]");
}

} // namespace

TEST_CASE("Toast: an action pill runs its action and problems stay longer", "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    spin(50);
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    QQuickItem* action = toast->findChild<QQuickItem*>(QStringLiteral("toastAction"));
    QQuickItem* close = toast->findChild<QQuickItem*>(QStringLiteral("toastClose"));
    REQUIRE(action != nullptr);
    REQUIRE(close != nullptr);

    // The two-argument calls stay as they were: a note for 4 s, a problem (warning, danger) for 7 s; no action.
    evaluate(shell, QStringLiteral("toast.show('Frame saved')"));
    CHECK(toast->property("shown").toBool());
    CHECK(toast->property("timeout").toInt() == 4000);
    CHECK_FALSE(action->isVisible());
    evaluate(shell, QStringLiteral("toast.show('The disk is full', Theme.warning)"));
    CHECK(toast->property("timeout").toInt() == 7000);
    CHECK(sameColor(toast->property("accent").value<QColor>(), shell.themeColor("warning")));
    evaluate(shell, QStringLiteral("toast.show('Export failed', Theme.danger)"));
    CHECK(toast->property("timeout").toInt() == 7000);
    // 0.4: an error is never replaced (what comes next waits behind it): close it first.
    evaluate(shell, QStringLiteral("toast.dismiss()"));

    // With an action: the pill shows its text, stays 8 s (or timeoutMs), and a click runs the action and closes it.
    evaluate(shell, QStringLiteral("toast.show('Exported Final cut.mp4', undefined, { actionText: 'Show in Explorer', "
                                   "action: () => toast.objectName = 'toastActionRan' })"));
    spin(50);
    CHECK(toast->property("timeout").toInt() == 8000);
    CHECK(action->isVisible());
    CHECK(close->isVisible());
    CHECK(action->property("text").toString() == QStringLiteral("Show in Explorer"));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/toast-action.png")));
    REQUIRE(QMetaObject::invokeMethod(action, "clicked"));
    spin(50);
    CHECK(toast->objectName() == QStringLiteral("toastActionRan"));
    CHECK_FALSE(toast->property("shown").toBool());
    toast->setObjectName(QStringLiteral("toast"));
    evaluate(shell, QStringLiteral("toast.show('Long one', Theme.warning, { timeoutMs: 15000 })"));
    CHECK(toast->property("timeout").toInt() == 15000);
    CHECK_FALSE(action->isVisible());
    // The close button closes it without running anything.
    evaluate(shell, QStringLiteral("toast.show('Undo the reset?', undefined, { actionText: 'Undo', action: () => toast.objectName = 'ran' })"));
    REQUIRE(QMetaObject::invokeMethod(close, "clicked"));
    CHECK_FALSE(toast->property("shown").toBool());
    CHECK(toast->objectName() == QStringLiteral("toast"));
    // A long message wraps instead of running off the card.
    evaluate(shell, QStringLiteral("toast.show('Could not save C:/Users/someone/Videos/Projects/A rather long project name/Edit.wabcut: "
                                   "the folder is read-only or the disk is full', Theme.warning, { actionText: 'Save As…', action: () => {} })"));
    spin(50);
    CHECK(toast->width() <= 4 * 150 + 1);
    QQuickItem* text = toast->findChild<QQuickItem*>(QStringLiteral("toastText"));
    REQUIRE(text != nullptr);
    CHECK(text->property("lineCount").toInt() >= 2);
    CHECK(text->height() + 8 <= toast->height());
    // The light theme too (the action pill keeps black text on the accent; the dot is the warning colour).
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/toast-long-warning.png")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Toast: an error is not replaced and the messages after it wait their turn", "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    spin(50);
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    QObject* timer = toast->findChild<QObject*>(QStringLiteral("toastHideTimer"));
    QQuickItem* waiting = toast->findChild<QQuickItem*>(QStringLiteral("toastPending"));
    QQuickItem* close = toast->findChild<QQuickItem*>(QStringLiteral("toastClose"));
    REQUIRE(timer != nullptr);
    REQUIRE(waiting != nullptr);
    REQUIRE(close != nullptr);
    evaluate(shell, QStringLiteral("MessageLog.clear()"));
    const auto text = [&] { return toast->property("text").toString(); };
    const auto shown = [&] { return toast->property("shown").toBool(); };
    const auto pending = [&] { return toast->property("pendingCount").toInt(); };
    const auto logged = [&] { return evaluate(shell, QStringLiteral("MessageLog.count")).toInt(); };
    const auto unseen = [&] { return evaluate(shell, QStringLiteral("MessageLog.unseen")).toInt(); };

    // An error, then a note: the error stays and the note waits behind it; both are in the log.
    evaluate(shell, QStringLiteral("toast.show('Export failed: The disk is full.', Theme.danger)"));
    evaluate(shell, QStringLiteral("toast.show('Frame saved next to the project')"));
    CHECK(text() == QStringLiteral("Export failed: The disk is full."));
    CHECK(pending() == 1);
    CHECK(logged() == 2);
    // The same error again only restarts its time: nothing more waits.
    evaluate(shell, QStringLiteral("toast.show('Export failed: The disk is full.', Theme.danger)"));
    CHECK(pending() == 1);
    CHECK(timer->property("running").toBool());
    // The card says one more message waits, and has a close button (an error without an action too).
    spin(50);
    CHECK(waiting->isVisible());
    CHECK(waiting->property("text").toString() == QStringLiteral("+1"));
    CHECK(close->isVisible());
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/toast-error-queue.png")));
    // The error goes (×): the note shows.
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    CHECK(shown());
    CHECK(text() == QStringLiteral("Frame saved next to the project"));
    CHECK(pending() == 0);
    // Notes and warnings still replace each other.
    evaluate(shell, QStringLiteral("toast.show('Clip volume +1 dB')"));
    CHECK(text() == QStringLiteral("Clip volume +1 dB"));
    evaluate(shell, QStringLiteral("toast.show('Clip volume +2 dB', Theme.warning)"));
    CHECK(text() == QStringLiteral("Clip volume +2 dB"));
    CHECK(pending() == 0);

    // An error that leaves on its time (nobody saw it) counts as unseen; one closed with × does not.
    evaluate(shell, QStringLiteral("toast.show('Autosave could not save your project', Theme.danger)"));
    CHECK(unseen() == 0);
    REQUIRE(QMetaObject::invokeMethod(timer, "triggered"));
    CHECK_FALSE(shown());
    CHECK(unseen() == 1);
    evaluate(shell, QStringLiteral("toast.show('Render In to Out failed', Theme.danger)"));
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    CHECK(unseen() == 1);
    // Newest first; a repeat of the newest message only refreshes its time.
    CHECK(evaluate(shell, QStringLiteral("MessageLog.entries[0].text")).toString() == QStringLiteral("Render In to Out failed"));
    CHECK(evaluate(shell, QStringLiteral("MessageLog.entries[0].tone")).toString() == QStringLiteral("danger"));
    const int before = logged();
    evaluate(shell, QStringLiteral("toast.show('Render In to Out failed', Theme.danger)"));
    CHECK(logged() == before);
    evaluate(shell, QStringLiteral("MessageLog.markSeen()"));
    CHECK(unseen() == 0);

    // At most five wait: when full, the oldest waiting message that is not an error goes.
    for (const char* note : {"Note 1", "Note 2"})
        evaluate(shell, QStringLiteral("toast.show('%1')").arg(QLatin1String(note)));
    evaluate(shell, QStringLiteral("toast.show('Second error', Theme.danger)"));
    for (const char* note : {"Note 4", "Note 5", "Note 6"})
        evaluate(shell, QStringLiteral("toast.show('%1')").arg(QLatin1String(note)));
    CHECK(text() == QStringLiteral("Render In to Out failed"));
    CHECK(pending() == 5);
    CHECK(evaluate(shell, QStringLiteral("toast.pending[0].text")).toString() == QStringLiteral("Note 2"));
    // dismissAll drops what waits; the error that never showed counts as unseen.
    evaluate(shell, QStringLiteral("toast.dismissAll()"));
    CHECK_FALSE(shown());
    CHECK(pending() == 0);
    CHECK(unseen() == 1);

    // 0.4: a second action, a ghost pill before the first; each closes the toast and runs its own.
    evaluate(shell, QStringLiteral("toast.show('Exported Highlights.mp4', Theme.success, { actionText: 'Open', "
                                   "action: () => toast.objectName = 'opened', secondaryActionText: 'Show in Explorer', "
                                   "secondaryAction: () => toast.objectName = 'revealed' })"));
    spin(50);
    QQuickItem* secondary = toast->findChild<QQuickItem*>(QStringLiteral("toastSecondaryAction"));
    QQuickItem* action = toast->findChild<QQuickItem*>(QStringLiteral("toastAction"));
    REQUIRE(secondary != nullptr);
    REQUIRE(action != nullptr);
    CHECK(secondary->isVisible());
    CHECK(action->isVisible());
    CHECK(secondary->property("text").toString() == QStringLiteral("Show in Explorer"));
    // Before the primary pill in the row (a hidden window lays the Row out only when it renders).
    const QList<QQuickItem*> row = action->parentItem()->childItems();
    CHECK(row.indexOf(secondary) >= 0);
    CHECK(row.indexOf(secondary) < row.indexOf(action));
    CHECK(toast->property("hasSecondary").toBool());
    REQUIRE(QMetaObject::invokeMethod(secondary, "clicked"));
    CHECK(toast->objectName() == QStringLiteral("revealed"));
    CHECK_FALSE(shown());
    toast->setObjectName(QStringLiteral("toast"));

    // untilSeen: its time waits while the window is not active (a hidden test window never is); others keep theirs.
    evaluate(shell, QStringLiteral("toast.show('Exported Teaser.mp4', Theme.success, { untilSeen: true })"));
    CHECK(shown());
    CHECK_FALSE(timer->property("running").toBool());
    evaluate(shell, QStringLiteral("toast.show('Frame saved next to the project')"));
    CHECK(timer->property("running").toBool());
    evaluate(shell, QStringLiteral("toast.dismiss()"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Recent messages keep errors past the limit and a readout or a repeat replaces its own message",
          "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    QObject* timer = toast->findChild<QObject*>(QStringLiteral("toastHideTimer"));
    REQUIRE(timer != nullptr);
    evaluate(shell, QStringLiteral("toast.dismissAll()"));
    evaluate(shell, QStringLiteral("MessageLog.clear()"));
    const auto logged = [&] { return evaluate(shell, QStringLiteral("MessageLog.count")).toInt(); };
    const auto unseen = [&] { return evaluate(shell, QStringLiteral("MessageLog.unseen")).toInt(); };
    const auto inLog = [&](const QString& text) {
        return evaluate(shell, QStringLiteral("MessageLog.entries.some(e => e.text === '%1')").arg(text)).toBool();
    };

    // An export failure nobody saw (its toast left on its time), then forty different notes - more than the log keeps
    // (30): the oldest notes go, the error stays and still counts as unseen.
    evaluate(shell, QStringLiteral("toast.show('Export failed: The disk is full.', Theme.danger)"));
    REQUIRE(QMetaObject::invokeMethod(timer, "triggered"));
    CHECK(unseen() == 1);
    for (int i = 1; i <= 40; ++i)
        evaluate(shell, QStringLiteral("MessageLog.add('Nudged clip %1', 'info', '', null)").arg(i));
    CHECK(logged() == 30);
    CHECK(inLog(QStringLiteral("Export failed: The disk is full.")));
    CHECK(inLog(QStringLiteral("Nudged clip 40")));
    CHECK_FALSE(inLog(QStringLiteral("Nudged clip 1")));
    CHECK(unseen() == 1);
    // Only when every entry is a problem does the oldest problem go - and an unseen one stops counting with it.
    for (int i = 1; i <= 30; ++i)
        evaluate(shell, QStringLiteral("MessageLog.add('Proxy %1 failed', 'warning', '', null)").arg(i));
    CHECK(logged() == 30);
    CHECK_FALSE(inLog(QStringLiteral("Export failed: The disk is full.")));
    CHECK(unseen() == 0);

    // A readout (the clip volume keys hold ] down): each value replaces the last one, in the toast's queue behind an
    // error and in the log.
    evaluate(shell, QStringLiteral("MessageLog.clear()"));
    evaluate(shell, QStringLiteral("toast.show('Export failed: The disk is full.', Theme.danger)"));
    for (int i = 1; i <= 12; ++i)
        evaluate(shell, QStringLiteral("toast.show('Volume +1 dB (now %1 dB)', undefined, { replaceKey: 'clipVolume' })").arg(i));
    CHECK(toast->property("text").toString() == QStringLiteral("Export failed: The disk is full."));
    CHECK(toast->property("pendingCount").toInt() == 1);
    CHECK(evaluate(shell, QStringLiteral("toast.pending[0].text")).toString() == QStringLiteral("Volume +1 dB (now 12 dB)"));
    CHECK(logged() == 2);
    CHECK(evaluate(shell, QStringLiteral("MessageLog.entries[0].text")).toString() == QStringLiteral("Volume +1 dB (now 12 dB)"));
    // The error goes: the readout shows; the next value replaces it there too.
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    CHECK(toast->property("text").toString() == QStringLiteral("Volume +1 dB (now 12 dB)"));
    evaluate(shell, QStringLiteral("toast.show('Volume +1 dB (now 13 dB)', undefined, { replaceKey: 'clipVolume' })"));
    CHECK(toast->property("text").toString() == QStringLiteral("Volume +1 dB (now 13 dB)"));
    CHECK(logged() == 2);

    // The same message again (two exports of one file name, in two folders): the toast takes the new call's actions,
    // so Open opens the second export's file.
    evaluate(shell, QStringLiteral("toast.dismissAll()"));
    evaluate(shell, QStringLiteral("toast.show('Exported Sequence 01.mp4', Theme.success, { actionText: 'Open', "
                                   "action: () => toast.objectName = 'first', untilSeen: true })"));
    evaluate(shell, QStringLiteral("toast.show('Exported Sequence 01.mp4', Theme.success, { actionText: 'Open', "
                                   "action: () => toast.objectName = 'second', secondaryActionText: 'Show in Explorer', "
                                   "secondaryAction: () => toast.objectName = 'revealed', untilSeen: true })"));
    CHECK(toast->property("hasSecondary").toBool());
    REQUIRE(QMetaObject::invokeMethod(toast, "runAction"));
    CHECK(toast->objectName() == QStringLiteral("second"));
    toast->setObjectName(QStringLiteral("toast"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Toast: one that appears under a resting pointer leaves on time and a pointer moved onto it holds it",
          "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    spin(50);
    QQuickWindow* window = shell.window.get();
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    QObject* timer = toast->findChild<QObject*>(QStringLiteral("toastHideTimer"));
    QObject* hover = toast->findChild<QObject*>(QStringLiteral("toastHover"));
    REQUIRE(timer != nullptr);
    REQUIRE(hover != nullptr);

    // Where a toast shows (bottom-right, its text): the pointer rests there while none shows.
    evaluate(shell, QStringLiteral("toast.show('Exported Final cut.mp4')"));
    REQUIRE(waitFor([&] { return toast->isVisible(); }));
    const QPointF at = toast->mapToScene(QPointF(toast->width() / 2, toast->height() / 2));
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    REQUIRE(waitFor([&] { return !toast->isVisible(); }));
    hoverAt(window, at);

    // A toast appears under it. Qt Quick reports the resting pointer where it is (again with each frame): the toast is
    // hovered, but that does not hold it - it leaves after its time.
    evaluate(shell, QStringLiteral("toast.show('Exported Final cut.mp4')"));
    REQUIRE(waitFor([&] { return toast->isVisible(); }));
    hoverAt(window, at);
    hoverAt(window, at);
    CHECK(hover->property("hovered").toBool());
    CHECK_FALSE(toast->property("held").toBool());
    CHECK(timer->property("running").toBool());
    // A new message under that resting pointer starts its time too.
    evaluate(shell, QStringLiteral("toast.show('Exported Teaser.mp4')"));
    hoverAt(window, at);
    CHECK(timer->property("running").toBool());

    // The pointer moves over it: it holds; a new message while it holds is held too.
    hoverAt(window, at + QPointF(4, 0));
    hoverAt(window, at + QPointF(8, 1));
    CHECK(toast->property("held").toBool());
    CHECK_FALSE(timer->property("running").toBool());
    evaluate(shell, QStringLiteral("toast.show('Exported Trailer.mp4')"));
    CHECK(toast->property("shown").toBool());
    CHECK_FALSE(timer->property("running").toBool());
    // The pointer leaves: it goes after its time again.
    hoverAt(window, QPointF(window->width() / 2.0, 20));
    CHECK_FALSE(hover->property("hovered").toBool());
    CHECK_FALSE(toast->property("held").toBool());
    CHECK(timer->property("running").toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Open and save failures are never silent in the editor or on Home", "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(file != nullptr);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString missing = QUrl::fromLocalFile(dir.filePath(QStringLiteral("Missing.wabcut"))).toString();

    // File > Open (and every other way in) with a file that cannot be opened: a warning toast says why.
    CHECK_FALSE(evaluate(shell, QStringLiteral("window.openProjectNow('%1')").arg(missing)).toBool());
    CHECK_FALSE(file->lastError().isEmpty());
    CHECK(toast->property("shown").toBool());
    CHECK(toast->property("text").toString() == file->lastError());
    CHECK(sameColor(toast->property("accent").value<QColor>(), shell.themeColor("warning")));
    // Open Project Next to This One... (the Production panel's projects) too.
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openProjectFile", Q_ARG(QVariant, QVariant(QUrl(missing)))));
    CHECK(toast->property("shown").toBool());
    CHECK(toast->property("text").toString() == file->lastError());

    // While Home is open the message goes to Home's strip (the toast is behind Home).
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    evaluate(shell, QStringLiteral("home.open()"));
    spin(50);
    CHECK_FALSE(evaluate(shell, QStringLiteral("window.openProjectNow('%1')").arg(missing)).toBool());
    CHECK(evaluate(shell, QStringLiteral("home.message")).toString() == file->lastError());
    CHECK_FALSE(toast->property("shown").toBool());
    evaluate(shell, QStringLiteral("home.close()"));
    spin(50);

    // Save fails (the project's file became a folder): the reason, with Save As... on the toast.
    const QString saved = dir.filePath(QStringLiteral("Saved.wabcut"));
    REQUIRE(file->saveAs(QUrl::fromLocalFile(saved)));
    makeEdit(shell, 1);
    REQUIRE(file->dirty());
    REQUIRE(QFile::remove(saved));
    REQUIRE(QDir().mkpath(saved));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "saveProject", Q_ARG(QVariant, QVariant())));
    spin(50);
    CHECK(file->dirty());
    CHECK(toast->property("shown").toBool());
    CHECK(toast->property("text").toString() == file->lastError());
    CHECK(toast->property("actionText").toString() == QStringLiteral("Save As…"));

    // The same failure while Home is open (Save at "Save changes?" before Home opens another project): Home's strip says
    // why and offers Save As... too (the open waits for it).
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    evaluate(shell, QStringLiteral("home.open()"));
    spin(50);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "saveProject", Q_ARG(QVariant, QVariant())));
    spin(50);
    CHECK_FALSE(toast->property("shown").toBool());
    CHECK(evaluate(shell, QStringLiteral("home.message")).toString() == file->lastError());
    CHECK(evaluate(shell, QStringLiteral("home.messageTone")).toString() == QStringLiteral("warning"));
    QQuickItem* homeAction = findUnder(shell.window->contentItem(), QStringLiteral("homeMessageAction"));
    REQUIRE(homeAction != nullptr);
    CHECK(homeAction->isVisible());
    CHECK(homeAction->property("text").toString() == QStringLiteral("Save As…"));
    CHECK(evaluate(shell, QStringLiteral("typeof home.messageAction")).toString() == QStringLiteral("function"));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/home-message-action.png")));
    // A message without an action has no pill.
    evaluate(shell, QStringLiteral("window.showProblem('The project could not be read')"));
    CHECK_FALSE(homeAction->isVisible());
    evaluate(shell, QStringLiteral("home.close()"));
    spin(50);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Autosave failure: one danger toast per failure streak with Save As and a red save dot", "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    spin(50);
    QQuickItem* toast = shell.item("toast");
    QQuickItem* dot = shell.item("unsavedDot");
    REQUIRE(toast != nullptr);
    REQUIRE(dot != nullptr);
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(file != nullptr);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString saved = dir.filePath(QStringLiteral("Film.wabcut"));
    REQUIRE(file->saveAs(QUrl::fromLocalFile(saved)));
    makeEdit(shell, 1);
    REQUIRE(file->dirty());
    CHECK(dot->isVisible());
    CHECK(sameColor(dot->property("color").value<QColor>(), shell.themeColor("warning")));
    evaluate(shell, QStringLiteral("toast.dismissAll()"));
    evaluate(shell, QStringLiteral("MessageLog.clear()"));
    const auto logged = [&] { return evaluate(shell, QStringLiteral("MessageLog.count")).toInt(); };

    // A folder where the autosave's temporary file goes: it cannot be written. One danger toast says why, with
    // Save As…; the Save button's dot turns red.
    std::filesystem::path blocked = io::autosavePath(std::filesystem::path(saved.toStdU16String()));
    blocked += ".tmp";
    std::filesystem::create_directories(blocked);
    CHECK_FALSE(file->autosaveNow());
    spin(50);
    CHECK_FALSE(file->autosaveOk());
    const QString status = file->autosaveText();
    REQUIRE(status.startsWith(QStringLiteral("Autosave failed ")));
    QString reason = status.mid(status.indexOf(QStringLiteral(": ")) + 2);
    if (reason.endsWith(QLatin1Char('.')))
        reason.chop(1);
    CHECK(toast->property("shown").toBool());
    const QString text = toast->property("text").toString();
    // What to do comes first (the reason, which may hold a long path, is last: the toast shows three lines).
    CHECK(text.startsWith(QStringLiteral("Autosave could not save your project. Save it somewhere else to keep your work safe. ")));
    CHECK(text.contains(reason));
    CHECK(text.endsWith(reason + QStringLiteral(".")));
    CHECK_FALSE(text.contains(QStringLiteral("..")));
    CHECK(sameColor(toast->property("accent").value<QColor>(), shell.themeColor("danger")));
    CHECK(toast->property("actionText").toString() == QStringLiteral("Save As…"));
    CHECK(toast->property("timeout").toInt() == 15000);
    CHECK(dot->isVisible());
    CHECK(sameColor(dot->property("color").value<QColor>(), shell.themeColor("danger")));
    CHECK(logged() == 1);
    QQuickItem* save = shell.item("saveButton");
    REQUIRE(save != nullptr);
    CHECK(save->property("tooltip").toString().contains(QStringLiteral("Autosave failed")));
    spin(300);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/autosave-failed.png")));

    // Every tick retries; the same streak toasts no more.
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    makeEdit(shell, 2);
    CHECK_FALSE(file->autosaveNow());
    spin(50);
    CHECK_FALSE(toast->property("shown").toBool());
    CHECK(logged() == 1);

    // The way is clear again: the autosave works, the dot is the unsaved one again (the project is still unsaved).
    std::filesystem::remove_all(blocked);
    makeEdit(shell, 3);
    CHECK(file->autosaveNow());
    CHECK(file->autosaveOk());
    CHECK(sameColor(dot->property("color").value<QColor>(), shell.themeColor("warning")));
    // A later failure starts a new streak: it says so again.
    std::filesystem::create_directories(blocked);
    makeEdit(shell, 4);
    CHECK_FALSE(file->autosaveNow());
    spin(50);
    CHECK(toast->property("shown").toBool());
    CHECK(toast->property("text").toString() == text);
    // The log's newest entry is the same message: it only gets the new time.
    CHECK(logged() == 1);
    evaluate(shell, QStringLiteral("toast.dismissAll()"));
    std::filesystem::remove_all(blocked);
    makeEdit(shell, 5);
    CHECK(file->autosaveNow());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Leaving the edited sample project asks to discard its changes first", "[app][shell][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(file != nullptr);
    REQUIRE(file->demo());
    QObject* dialog = shell.object("unsavedChangesDialog");
    REQUIRE(dialog != nullptr);

    // Untouched, the sample project is left without a question (the action runs at once).
    evaluate(shell, QStringLiteral("window.whenSaved(() => toast.objectName = 'leftDemo')"));
    QQuickItem* toast = shell.item("leftDemo");
    REQUIRE(toast != nullptr);
    toast->setObjectName(QStringLiteral("toast"));
    CHECK_FALSE(dialog->property("visible").toBool());

    // Edited (it never counts as unsaved: no file), Open Project asks first: Save As... / Discard / Cancel.
    makeEdit(shell, 1);
    CHECK_FALSE(file->dirty());
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openProject"));
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
    CHECK(dialog->property("demo").toBool());
    CHECK(dialog->property("title").toString().contains(QStringLiteral("sample project")));
    QObject* save = dialog->findChild<QObject*>(QStringLiteral("unsavedSaveButton"));
    QObject* discard = dialog->findChild<QObject*>(QStringLiteral("unsavedDiscardButton"));
    REQUIRE(save != nullptr);
    REQUIRE(discard != nullptr);
    CHECK(save->property("text").toString() == QStringLiteral("Save As…"));
    CHECK(discard->property("text").toString() == QStringLiteral("Discard"));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/demo-leave-prompt.png")));
    // Cancel: still the sample project, with its edits (and no file dialog).
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(file->demo());

    // An import over it asks the same way first (it would start a new project); Cancel imports nothing.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // (Through an expression: importUrls takes an optional second argument.)
    CHECK(evaluate(shell, QStringLiteral("window.importUrls(%1).pending === true").arg(urlList({dir.filePath(QStringLiteral("clip.mp4"))})))
              .toBool());
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(file->demo());

    // Discard: what was asked for runs (here a new project, as Home's Create does through whenSaved).
    evaluate(shell, QStringLiteral("window.whenSaved(() => ProjectFile.newProject())"));
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(dialog, "discardChosen"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    spin(50);
    CHECK_FALSE(file->demo());

    // A project of one's own with unsaved changes still gets the usual question.
    makeEdit(shell, 2);
    REQUIRE(file->dirty());
    evaluate(shell, QStringLiteral("window.whenSaved(() => ProjectFile.newProject())"));
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
    CHECK_FALSE(dialog->property("demo").toBool());
    CHECK(save->property("text").toString() == QStringLiteral("Save"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Leaving the edited sample project by Open Next to This One or an import asks once before anything is imported",
          "[app][shell][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(file != nullptr);
    REQUIRE(file->demo());
    QObject* dialog = shell.object("unsavedChangesDialog");
    REQUIRE(dialog != nullptr);
    const auto asked = [&] { return waitFor([&] { return dialog->property("visible").toBool(); }, 3000); };
    const auto closeDialog = [&] {
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    };
    // Discard as its button does: the dialog closes, then the action runs.
    const auto discard = [&] {
        closeDialog();
        REQUIRE(QMetaObject::invokeMethod(dialog, "discardChosen"));
        spin(80);
    };
    const auto graphicsMessage = [&] { return evaluate(shell, QStringLiteral("GraphicsImport.message")).toString(); };

    // A project of one's own on disk (a copy of the sample: Save a Copy leaves the sample as it is), then edits.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString other = dir.filePath(QStringLiteral("Other.wabcut"));
    REQUIRE(file->saveCopy(QUrl::fromLocalFile(other)));
    REQUIRE(file->demo());
    makeEdit(shell, 1);

    // Open Project Next to This One (File menu, the Production panel) and Home's project menu: the sample makes way for
    // the project, so they ask first; Cancel keeps the sample with its edits.
    const QVariant otherUrl(QUrl::fromLocalFile(other));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openProjectFile", Q_ARG(QVariant, otherUrl)));
    REQUIRE(asked());
    CHECK(dialog->property("demo").toBool());
    closeDialog();
    CHECK(file->demo());
    evaluate(shell, QStringLiteral("home.openAlongsideRequested('%1')").arg(QUrl::fromLocalFile(other).toString()));
    REQUIRE(asked());
    closeDialog();
    spin(50);
    CHECK(file->demo());
    CHECK(evaluate(shell, QStringLiteral("ProjectController.canUndo")).toBool());
    // Discard: the project opens, after one question.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openProjectFile", Q_ARG(QVariant, otherUrl)));
    REQUIRE(asked());
    discard();
    CHECK_FALSE(dialog->property("visible").toBool());
    CHECK_FALSE(file->demo());
    CHECK(file->name() == QStringLiteral("Other"));

    // The edited sample again: File > Import with the project file asks once; after Discard the project opens without a
    // second question about the same edits.
    REQUIRE(evaluate(shell, QStringLiteral("ProjectFile.openDemo()")).toBool());
    REQUIRE(file->demo());
    makeEdit(shell, 2);
    CHECK(evaluate(shell, QStringLiteral("window.importUrls(%1).pending === true").arg(urlList({other}))).toBool());
    REQUIRE(asked());
    discard();
    CHECK_FALSE(dialog->property("visible").toBool());
    CHECK_FALSE(file->demo());
    CHECK(file->name() == QStringLiteral("Other"));

    // Whatever the answered action leads to (a timeline drop of a project file asks again inside it) is not asked
    // twice; the next leaving is asked about again.
    REQUIRE(evaluate(shell, QStringLiteral("ProjectFile.openDemo()")).toBool());
    makeEdit(shell, 3);
    evaluate(shell, QStringLiteral("window.whenSaved(() => window.whenSaved(() => toast.objectName = 'askedOnce'))"));
    REQUIRE(asked());
    discard();
    CHECK_FALSE(dialog->property("visible").toBool());
    QQuickItem* toast = shell.item("askedOnce");
    REQUIRE(toast != nullptr);
    toast->setObjectName(QStringLiteral("toast"));
    CHECK(file->demo());
    evaluate(shell, QStringLiteral("window.whenSaved(() => toast.objectName = 'askedAgain')"));
    REQUIRE(asked());
    closeDialog();
    CHECK(shell.item("askedAgain") == nullptr);

    // An import of an image and an animation: nothing is imported before the answer (a dotLottie file is refused at
    // once, with a reason - there is none yet), and Cancel imports nothing. Discard: the image starts the new project,
    // then the animation is handled in it (refused there with its reason).
    QImage still(32, 18, QImage::Format_RGB32);
    still.fill(Qt::darkCyan);
    const QString image = dir.filePath(QStringLiteral("still.png"));
    REQUIRE(still.save(image));
    const QString sticker = dir.filePath(QStringLiteral("sticker.lottie"));
    const QString before = graphicsMessage();
    CHECK_FALSE(before.contains(QStringLiteral("dotLottie")));
    CHECK(evaluate(shell, QStringLiteral("window.importUrls(%1).pending === true").arg(urlList({sticker, image}))).toBool());
    REQUIRE(asked());
    CHECK(graphicsMessage() == before);
    closeDialog();
    CHECK(file->demo());
    CHECK(graphicsMessage() == before);
    evaluate(shell, QStringLiteral("window.importUrls(%1)").arg(urlList({sticker, image})));
    REQUIRE(asked());
    discard();
    CHECK_FALSE(dialog->property("visible").toBool());
    CHECK_FALSE(file->demo());
    CHECK(graphicsMessage().contains(QStringLiteral("dotLottie")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Background work that ends while Home is open says so on Home's strip with its action", "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    QObject* exports = evaluate(shell, QStringLiteral("ExportController")).value<QObject*>();
    QObject* readiness = evaluate(shell, QStringLiteral("MediaReadiness")).value<QObject*>();
    REQUIRE(exports != nullptr);
    REQUIRE(readiness != nullptr);
    QQuickItem* action = findUnder(shell.window->contentItem(), QStringLiteral("homeMessageAction"));
    REQUIRE(action != nullptr);
    const auto homeMessage = [&] { return evaluate(shell, QStringLiteral("home.message")).toString(); };
    const auto homeTone = [&] { return evaluate(shell, QStringLiteral("home.messageTone")).toString(); };
    const auto finished = [&](bool succeeded, const QString& message) {
        REQUIRE(QMetaObject::invokeMethod(exports, "jobFinished", Q_ARG(QString, QStringLiteral("no-such-job")),
                                          Q_ARG(bool, succeeded), Q_ARG(QString, message)));
        spin(30);
    };

    // In the editor: the toast, as before.
    finished(false, QStringLiteral("the disk is full"));
    CHECK(toast->property("shown").toBool());
    CHECK(toast->property("text").toString() == QStringLiteral("Export failed: the disk is full"));
    CHECK(toast->property("actionText").toString() == QStringLiteral("Show the queue"));
    evaluate(shell, QStringLiteral("toast.dismiss()"));

    // Home covers the editor (a toast would run out unseen behind it): an export that ends says so on Home's strip,
    // with its action.
    evaluate(shell, QStringLiteral("home.open()"));
    spin(50);
    finished(true, QString());
    CHECK_FALSE(toast->property("shown").toBool());
    CHECK(homeMessage() == QStringLiteral("Exported the sequence"));
    CHECK(homeTone() == QStringLiteral("info"));
    CHECK(action->isVisible());
    // 0.4: Home's strip takes one action: Open (the toast has "Show in Explorer" beside it).
    CHECK(action->property("text").toString() == QStringLiteral("Open"));
    // A failed one: a danger strip; Show the queue leaves Home for the editor's Export panel.
    finished(false, QStringLiteral("the disk is full"));
    CHECK_FALSE(toast->property("shown").toBool());
    CHECK(homeMessage() == QStringLiteral("Export failed: the disk is full"));
    CHECK(homeTone() == QStringLiteral("danger"));
    CHECK(action->property("text").toString() == QStringLiteral("Show the queue"));
    REQUIRE(QMetaObject::invokeMethod(action, "clicked"));
    spin(100);
    CHECK_FALSE(evaluate(shell, QStringLiteral("home.opened")).toBool());
    CHECK(evaluate(shell, QStringLiteral("docking.isOpen('export')")).toBool());
    // Render In to Out failing while Home is open: a warning strip with Try Again.
    evaluate(shell, QStringLiteral("home.open()"));
    spin(50);
    REQUIRE(QMetaObject::invokeMethod(readiness, "renderFinished", Q_ARG(QString, QStringLiteral("failed")),
                                      Q_ARG(QString, QStringLiteral("no room for the previews"))));
    spin(30);
    CHECK_FALSE(toast->property("shown").toBool());
    CHECK(homeMessage() == QStringLiteral("Render In to Out failed: no room for the previews"));
    CHECK(homeTone() == QStringLiteral("warning"));
    CHECK(action->property("text").toString() == QStringLiteral("Try Again"));
    evaluate(shell, QStringLiteral("home.close()"));
    spin(50);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Home strip: an error is not replaced by a later message and an error nobody closed counts as unseen",
          "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QObject* exports = evaluate(shell, QStringLiteral("ExportController")).value<QObject*>();
    REQUIRE(exports != nullptr);
    const auto homeMessage = [&] { return evaluate(shell, QStringLiteral("home.message")).toString(); };
    const auto unseen = [&] { return evaluate(shell, QStringLiteral("MessageLog.unseen")).toInt(); };
    const auto logged = [&] { return evaluate(shell, QStringLiteral("MessageLog.count")).toInt(); };
    const auto finished = [&](bool succeeded, const QString& message) {
        REQUIRE(QMetaObject::invokeMethod(exports, "jobFinished", Q_ARG(QString, QStringLiteral("no-such-job")),
                                          Q_ARG(bool, succeeded), Q_ARG(QString, message)));
        spin(30);
    };
    evaluate(shell, QStringLiteral("home.open()"));
    spin(50);
    evaluate(shell, QStringLiteral("MessageLog.clear()"));

    // Export A fails while Home is open, then export B finishes: the failure stays on the strip, B's message waits
    // (both are in Recent messages) and shows once the failure is closed.
    finished(false, QStringLiteral("The disk is full."));
    CHECK(homeMessage() == QStringLiteral("Export failed: The disk is full."));
    finished(true, QString());
    CHECK(homeMessage() == QStringLiteral("Export failed: The disk is full."));
    CHECK(evaluate(shell, QStringLiteral("home.messageTone")).toString() == QStringLiteral("danger"));
    CHECK(logged() == 2);
    CHECK(unseen() == 0);
    evaluate(shell, QStringLiteral("home.message = ''")); // its ×
    spin(30);
    CHECK(homeMessage() == QStringLiteral("Exported the sequence"));
    CHECK(unseen() == 0); // closed by the user: seen

    // A later error replaces an earlier one on the strip: the earlier one counts as unseen.
    finished(false, QStringLiteral("The disk is full."));
    finished(false, QStringLiteral("No permission to write the file."));
    CHECK(homeMessage() == QStringLiteral("Export failed: No permission to write the file."));
    CHECK(unseen() == 1);
    // Home closes with the error still on the strip (nobody closed it): unseen too, so the Background tasks pill says
    // "2 problems" in the editor.
    evaluate(shell, QStringLiteral("home.close()"));
    spin(50);
    CHECK(unseen() == 2);
    QQuickItem* pill = shell.item("activityPill");
    REQUIRE(pill != nullptr);
    CHECK(pill->isVisible());
    CHECK(pill->property("fullText").toString() == QStringLiteral("2 problems"));
    evaluate(shell, QStringLiteral("MessageLog.markSeen()"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("History panel: Left and Right step one state back and forward while it has the focus", "[app][shell][history][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller != nullptr);
    makeEdit(shell, 1);
    makeEdit(shell, 2);
    const std::size_t undo = controller->session().undoCount();

    evaluate(shell, QStringLiteral("Editing.runCommand('window.history')"));
    QObject* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = shell.window->findChild<QObject*>(QStringLiteral("historyPanel"));
        return panel && panel->property("opened").toBool();
    }));
    spin(50);
    CHECK(evaluate(shell, QStringLiteral("KeymapDispatcher.scopeAt(window.activeFocusItem)")).toString() ==
          QStringLiteral("history"));
    tapKey(shell.window.get(), Qt::Key_Left);
    CHECK(controller->session().undoCount() == undo - 1);
    tapKey(shell.window.get(), Qt::Key_Left);
    CHECK(controller->session().undoCount() == undo - 2);
    tapKey(shell.window.get(), Qt::Key_Right);
    CHECK(controller->session().undoCount() == undo - 1);
    evaluate(shell, QStringLiteral("Editing.runCommand('window.history')")); // closes it
    CHECK(waitFor([&] { return !panel->property("opened").toBool(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Reveal in Project brings the Library forward through Editing.libraryRequested", "[app][shell][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    evaluate(shell, QStringLiteral("docking.closePanel('library')"));
    spin(50);
    REQUIRE_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('library')")).toBool());
    evaluate(shell, QStringLiteral("Editing.libraryRequested()"));
    spin(100);
    CHECK(evaluate(shell, QStringLiteral("docking.isOpen('library')")).toBool());
    CHECK(shell.item("libraryPanel")->isVisible());
    CHECK(evaluate(shell, QStringLiteral("docking.activeGroup === docking.groupOfPanel('library').group")).toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Clip volume keys held down leave one readout in the toast queue and in Recent messages",
          "[app][shell][toast][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    if (evaluate(shell, QStringLiteral("home.opened")).toBool())
        evaluate(shell, QStringLiteral("home.close()"));
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller != nullptr);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->setSelection({clipAt(*controller->snapshot(), tl.seq, "V1@0")});
    spin(50);
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    evaluate(shell, QStringLiteral("toast.dismissAll()"));
    evaluate(shell, QStringLiteral("MessageLog.clear()"));
    const auto logged = [&] { return evaluate(shell, QStringLiteral("MessageLog.count")).toInt(); };

    // An export failure shows; then ] is held down (key repeat: eight presses, each with its own value). The readout
    // waits behind the error as one message - the latest value - and Recent messages keeps one line for it, so the
    // presses neither replay for half a minute after the error nor push the error out of the log.
    evaluate(shell, QStringLiteral("toast.show('Export failed: The disk is full.', Theme.danger)"));
    for (int i = 0; i < 8; ++i)
        evaluate(shell, QStringLiteral("window.runCommand('clip.increaseVolume')"));
    CHECK(toast->property("text").toString() == QStringLiteral("Export failed: The disk is full."));
    CHECK(toast->property("pendingCount").toInt() == 1);
    const QString last = evaluate(shell, QStringLiteral("toast.pending[0].text")).toString();
    CHECK(last == QStringLiteral("Volume +1 dB (now 8.0 dB)"));
    CHECK(logged() == 2);
    CHECK(evaluate(shell, QStringLiteral("MessageLog.entries[0].text")).toString() == last);
    CHECK(evaluate(shell, QStringLiteral("MessageLog.entries[1].text")).toString()
          == QStringLiteral("Export failed: The disk is full."));
    // The error goes: the readout shows, and the next press replaces it there.
    evaluate(shell, QStringLiteral("toast.dismiss()"));
    CHECK(toast->property("text").toString() == last);
    evaluate(shell, QStringLiteral("window.runCommand('clip.decreaseVolume')"));
    CHECK(toast->property("text").toString() == QStringLiteral("Volume -1 dB (now 7.0 dB)"));
    CHECK(toast->property("pendingCount").toInt() == 0);
    CHECK(logged() == 2);
    evaluate(shell, QStringLiteral("toast.dismissAll()"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
