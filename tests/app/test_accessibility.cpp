// M5j accessibility (roadmap Wave A item 6), offscreen:
//   * the Timeline list: tracks and clips in the accessibility tree with roles and names; keyboard selection;
//   * keyboard only, end to end: import, insert, edit, export;
//   * high contrast: AAA text and strong accents in both themes; the interface size preference;
//   * focus: popovers close with Esc and give focus back; switching workspaces never leaves focus on nothing.
// Screenshots: the Timeline list, high contrast (dark and light).

#include "ShellFixture.h"

#include "ClipAccessModel.h"
#include "ExportController.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"
#include "UiScale.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <wabcut/model/ProjectOps.h>

#include <QAccessible>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <functional>

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

void tap(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(window, &release);
}

/// Types `text` one key at a time (letters, digits and path punctuation).
void type(QQuickWindow* window, const QString& text) {
    for (const QChar ch : text) {
        int key = ch.toUpper().unicode();
        Qt::KeyboardModifiers mods;
        if (ch.isUpper() || ch == QLatin1Char(':') || ch == QLatin1Char('_'))
            mods = Qt::ShiftModifier;
        if (ch == QLatin1Char('\\'))
            key = Qt::Key_Backslash;
        tap(window, key, mods, QString(ch));
    }
}

struct Node {
    QAccessible::Role role;
    QString name;
};

void collect(QAccessibleInterface* iface, QList<Node>& out, int depth = 0) {
    if (!iface || depth > 60)
        return;
    out << Node{iface->role(), iface->text(QAccessible::Name)};
    for (int i = 0; i < iface->childCount(); ++i)
        collect(iface->child(i), out, depth + 1);
}

double luminance(const QColor& c) {
    const auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// Spins the event loop until `done` or `ms` pass.
bool waitUntil(const std::function<bool()>& done, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms)
        spin(20);
    return done();
}

void saveCrop(const QImage& image, QQuickItem* item, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
        .save(QDir(outputDir()).filePath(name));
}

} // namespace

TEST_CASE("The Timeline list: tracks and clips in the accessibility tree, chosen with the keyboard",
          "[app][a11y][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QAccessible::setActive(true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    QQuickItem* panel = shell.item("timelinePanel");
    REQUIRE(panel);

    // The header button shows the list; it takes focus.
    QQuickItem* button = findNamed(shell.window->contentItem(), QStringLiteral("timelineListButton"));
    REQUIRE(button);
    panel->setProperty("listMode", true);
    spin(50);
    QQuickItem* list = findNamed(shell.window->contentItem(), QStringLiteral("timelineListView"));
    REQUIRE(list);
    CHECK(list->isVisible());
    CHECK(list->hasActiveFocus());
    REQUIRE_FALSE(shell.grab().isNull()); // lays out the rows

    // The accessibility tree: the list, a heading per track, a list item per clip with its spoken name.
    QList<Node> nodes;
    collect(QAccessible::queryAccessibleInterface(shell.window.get()), nodes);
    const auto has = [&](QAccessible::Role role, const QString& prefix) {
        return std::ranges::any_of(nodes, [&](const Node& n) { return n.role == role && n.name.startsWith(prefix); });
    };
    CHECK(has(QAccessible::List, QStringLiteral("Timeline list: 16 clip")));
    CHECK(has(QAccessible::Heading, QStringLiteral("Track V2")));
    CHECK(has(QAccessible::Heading, QStringLiteral("Track A1")));
    CHECK(has(QAccessible::ListItem, QStringLiteral("City B-roll, V2, 00:00:04:00 to 00:00:17:23, ")));
    CHECK(has(QAccessible::ListItem, QStringLiteral("Interview A, V1, ")));
    CHECK(std::ranges::any_of(nodes, [](const Node& n) {
        return n.role == QAccessible::ListItem && n.name.contains(QStringLiteral(" long, selected"));
    }));

    // Keyboard: Home, Down, Enter selects that clip and moves the playhead to it; Esc goes back to the lanes.
    tap(shell.window.get(), Qt::Key_Home);
    tap(shell.window.get(), Qt::Key_Down);
    tap(shell.window.get(), Qt::Key_Return);
    REQUIRE(controller->selection().size() >= 1);
    const ClipId chosen = controller->primaryClip();
    REQUIRE(chosen);
    const auto loc = controller->currentSequence()->findClip(chosen);
    REQUIRE(loc);
    CHECK(controller->playhead() == loc->clip().start());
    CHECK(controller->clipList()->currentRow() == 1);

    saveCrop(shell.grab(), panel, QStringLiteral("timeline-list.png"));

    tap(shell.window.get(), Qt::Key_Escape);
    CHECK_FALSE(panel->property("listMode").toBool());
    CHECK(findNamed(shell.window->contentItem(), QStringLiteral("timelineLanesArea"))->hasActiveFocus());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Keyboard only: import, insert, edit and export", "[app][a11y][keyboard][qml]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // A still to import.
    const QString still = dir.filePath(QStringLiteral("card.jpg"));
    {
        QImage image(320, 180, QImage::Format_RGB32);
        QPainter p(&image);
        QLinearGradient g(0, 0, 320, 180);
        g.setColorAt(0, QColor(40, 20, 90));
        g.setColorAt(1, QColor(220, 90, 200));
        p.fillRect(image.rect(), g);
        p.end();
        REQUIRE(image.save(still, "JPG", 90));
    }
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    // Tab moves between all controls (Windows' default; the offscreen platform's is text fields only).
    QGuiApplication::styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
    QQuickWindow* w = shell.window.get();
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* keymap = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(controller);
    REQUIRE(keymap);
    REQUIRE(exporter);
    // Setup: an empty project with one small sequence (640x360, 30 fps).
    edit::SequenceSettings settings;
    settings.width = 640;
    settings.height = 360;
    settings.videoTracks = 2;
    settings.audioTracks = 2;
    auto created = edit::addSequence(ProjectState{}, settings);
    REQUIRE(created);
    controller->resetProject(std::move(created->state), created->id);
    REQUIRE_FALSE(shell.grab().isNull());

    // 1. Import: Ctrl+I opens File > Import (the system's file dialog, keyboard-driven by the OS); it hands the file
    //    to the import, as here.
    CHECK(keymap->shortcutText(QStringLiteral("file.import")) == QStringLiteral("Ctrl+I"));
    QVariant result;
    REQUIRE(QMetaObject::invokeMethod(w, "importUrls", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(still)}))));
    REQUIRE(waitUntil([&] { return controller->mediaItems().size() == 1; }, 5000));

    // 2. Library (Shift+1), Tab to the card, Enter loads it in the Source Monitor, , inserts it at the playhead.
    tap(w, Qt::Key_1, Qt::ShiftModifier, QStringLiteral("!"));
    spin(50);
    bool onCard = false;
    QStringList chain;
    for (int i = 0; i < 40 && !onCard; ++i) {
        QQuickItem* f = w->activeFocusItem();
        chain << (f ? QStringLiteral("%1(%2)").arg(QString::fromLatin1(f->metaObject()->className()), f->objectName())
                    : QStringLiteral("none"));
        // The imported still's Library card (M5k: LibraryCard, keyed "libraryCard_m…", named after the file).
        onCard = f && f->objectName().startsWith(QStringLiteral("libraryCard_"))
              && f->property("name").toString().contains(QStringLiteral("card"));
        if (!onCard)
            tap(w, Qt::Key_Tab);
    }
    INFO("focus chain: " << chain.join(QStringLiteral(" > ")).toStdString());
    INFO("media: " << controller->mediaItems().size() << " "
                   << (controller->mediaItems().isEmpty() ? std::string()
                                                          : controller->mediaItems()[0].toMap().value(QStringLiteral("kind")).toString().toStdString()));
    REQUIRE(onCard);
    tap(w, Qt::Key_Return);
    auto* source = shell.engine.singletonInstance<QObject*>("Wabcut", "SourceMonitor");
    REQUIRE(source);
    REQUIRE(waitUntil([&] { return source->property("loaded").toBool(); }, 5000));
    tap(w, Qt::Key_Comma, {}, QStringLiteral(","));
    REQUIRE(waitUntil([&] { return controller->clipCount() >= 1; }, 2000));
    const int inserted = controller->clipCount();

    // 3. Edit in the Timeline (Shift+3): 15 frames in, Ctrl+K adds an edit, D selects the clip at the playhead,
    //    Delete clears it.
    tap(w, Qt::Key_3, Qt::ShiftModifier, QStringLiteral("#"));
    spin(50);
    controller->setPlayhead({}); // the Home key does the same
    tap(w, Qt::Key_Home);
    for (int i = 0; i < 15; ++i)
        tap(w, Qt::Key_Right);
    CHECK(controller->playhead() == framesToTicks(15, controller->currentSequence()->frameRate));
    tap(w, Qt::Key_K, Qt::ControlModifier);
    CHECK(controller->clipCount() == inserted + 1);
    tap(w, Qt::Key_D, {}, QStringLiteral("d"));
    REQUIRE_FALSE(controller->selection().empty());
    tap(w, Qt::Key_Delete);
    CHECK(controller->clipCount() == inserted);

    // 4. Export (Ctrl+M): Tab to the output field, type the path, Tab to Export, Space.
    tap(w, Qt::Key_M, Qt::ControlModifier);
    spin(100);
    const QString output = QDir::toNativeSeparators(dir.filePath(QStringLiteral("keyboard.mp4")));
    bool onField = false;
    QStringList exportChain;
    // Reachability, not a count: the window has about 60 Tab stops before the field (rail, monitors, presets).
    for (int i = 0; i < 120 && !onField; ++i) {
        QQuickItem* f = w->activeFocusItem();
        exportChain << (f ? QStringLiteral("%1(%2)").arg(QString::fromLatin1(f->metaObject()->className()), f->objectName())
                          : QStringLiteral("none"));
        onField = f && f->objectName() == QStringLiteral("exportOutputField");
        if (!onField)
            tap(w, Qt::Key_Tab);
    }
    INFO("export focus chain: " << exportChain.join(QStringLiteral(" > ")).toStdString());
    REQUIRE(onField);
    // Select the suggested path (End, Shift+Home: plain keys stay in the field) and type over it.
    tap(w, Qt::Key_End);
    tap(w, Qt::Key_Home, Qt::ShiftModifier);
    type(w, output);
    bool onButton = false;
    for (int i = 0; i < 20 && !onButton; ++i) {
        tap(w, Qt::Key_Tab);
        QQuickItem* f = w->activeFocusItem();
        onButton = f && f->objectName() == QStringLiteral("exportButton");
    }
    REQUIRE(onButton);
    tap(w, Qt::Key_Space, {}, QStringLiteral(" "));
    REQUIRE(waitUntil([&] { return exporter->jobs()->rowCount() == 1; }, 5000));
    const QModelIndex job = exporter->jobs()->index(0);
    CHECK(QDir::toNativeSeparators(exporter->jobs()->data(job, ExportJobModel::OutputPathRole).toString()) == output);
    REQUIRE(waitUntil([&] { return exporter->jobs()->data(job, ExportJobModel::FinishedRole).toBool(); }, 120000));
    INFO("export: " << exporter->jobs()->data(job, ExportJobModel::DetailRole).toString().toStdString());
    CHECK(exporter->jobs()->data(job, ExportJobModel::StateRole).toString() == QStringLiteral("done"));
    CHECK(QFileInfo(output).size() > 0);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("High contrast: AAA text and strong accents in both themes", "[app][a11y][design]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.theme->setProperty("highContrast", true);
    QStringList report;
    for (const bool dark : {true, false}) {
        shell.theme->setProperty("dark", dark);
        spin(20);
        for (const char* surface : {"panel", "raised", "canvas"}) {
            const QColor bg = shell.themeColor(surface);
            for (const char* token : {"textPrimary", "textSecondary", "textTertiary"}) {
                const double ratio = contrast(shell.themeColor(token), bg);
                report << QStringLiteral("%1 %2 on %3: %4").arg(dark ? "dark" : "light", token, surface).arg(ratio, 0, 'f', 2);
                INFO(report.last().toStdString());
                CHECK(ratio >= 7.0);
            }
            const double stroke = contrast(shell.themeColor("accentStroke"), bg);
            report << QStringLiteral("%1 accentStroke on %2: %3").arg(dark ? "dark" : "light", surface).arg(stroke, 0, 'f', 2);
            INFO(report.last().toStdString());
            CHECK(stroke >= 4.5);
            const double border = contrast(shell.themeColor("borderStrong"), bg);
            CHECK(border >= 3.0);
        }
    }
    writeOutputFile(QStringLiteral("contrast-high.txt"), report.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshots: high contrast, dark and light", "[app][a11y][screenshots][qml]") {
    const bool dark = GENERATE(true, false);
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.theme->setProperty("highContrast", true);
    shell.theme->setProperty("dark", dark);
    spin(50);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("high-contrast-dark.png")
                                               : QStringLiteral("high-contrast-light.png")));
    CHECK(shell.themeColor("panel") == (dark ? QColor("#0A0A0A") : QColor("#FFFFFF")));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Interface size: 100, 125 or 150 %, applied at launch", "[app][a11y]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
    CHECK(savedUiScale(settings) == 100);
    for (const int value : {125, 150}) {
        settings.setValue(QStringLiteral("appearance/uiScale"), value);
        CHECK(savedUiScale(settings) == value);
    }
    settings.setValue(QStringLiteral("appearance/uiScale"), 175);
    CHECK(savedUiScale(settings) == 100); // only the offered sizes

    // applySavedUiScale(): the saved size becomes QT_SCALE_FACTOR, unless the environment already has one.
    const QByteArray before = qgetenv("QT_SCALE_FACTOR");
    const bool hadBefore = qEnvironmentVariableIsSet("QT_SCALE_FACTOR");
    qunsetenv("QT_SCALE_FACTOR");
    {
        QSettings app;
        app.setValue(QStringLiteral("appearance/uiScale"), 150);
        app.sync();
    }
    CHECK(applySavedUiScale() == 150);
    CHECK(qgetenv("QT_SCALE_FACTOR") == QByteArray("1.5"));
    qputenv("QT_SCALE_FACTOR", "2");
    CHECK(applySavedUiScale() == 100);
    CHECK(qgetenv("QT_SCALE_FACTOR") == QByteArray("2"));
    if (hadBefore)
        qputenv("QT_SCALE_FACTOR", before);
    else
        qunsetenv("QT_SCALE_FACTOR");
    resetSettings();
}

TEST_CASE("Appearance: high contrast and the interface size from Settings", "[app][a11y][qml]") {
    // 0.1.3: the title bar's Appearance popover became Settings > Appearance.
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    QObject* rail = shell.object("leftRail");
    REQUIRE(rail);
    REQUIRE(QMetaObject::invokeMethod(rail, "openSettings", Q_ARG(QString, QStringLiteral("appearance"))));
    QObject* loader = shell.object("settingsWindowLoader");
    REQUIRE(loader);
    QObject* settings = loader->property("item").value<QObject*>();
    REQUIRE(settings);
    spin(50);
    CHECK(settings->property("visible").toBool());
    QQuickItem* root = settings->property("contentItem").value<QQuickItem*>();
    REQUIRE(root);
    QQuickItem* toggle = findNamed(root, QStringLiteral("highContrastToggle"));
    QQuickItem* tabs = findNamed(root, QStringLiteral("uiScaleTabs"));
    QQuickItem* row = findNamed(root, QStringLiteral("uiScaleRow"));
    REQUIRE(toggle);
    REQUIRE(tabs);
    REQUIRE(row);
    const QString before = row->property("description").toString();
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
    CHECK(shell.theme->property("highContrast").toBool());
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 1)));
    CHECK(shell.preferences->property("uiScale").toInt() == 125);
    CHECK(row->property("description").toString() != before); // "applies when Wabcut restarts"
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Focus: Esc closes a popover and gives focus back; workspace switches keep focus", "[app][a11y][focus][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());

    // A popover opened from a focused button: Esc closes it, focus returns to the button.
    QQuickItem* button = findNamed(w->contentItem(), QStringLiteral("timelineDisplayButton"));
    REQUIRE(button);
    button->forceActiveFocus(Qt::TabFocusReason);
    tap(w, Qt::Key_Space, {}, QStringLiteral(" "));
    spin(50);
    QObject* popover = nullptr;
    for (QObject* o : button->findChildren<QObject*>()) {
        if (o->objectName() == QStringLiteral("timelineDisplayPopover"))
            popover = o;
    }
    REQUIRE(popover);
    REQUIRE(popover->property("opened").toBool());
    tap(w, Qt::Key_Escape);
    spin(200);
    CHECK_FALSE(popover->property("visible").toBool());
    CHECK(button->hasActiveFocus());

    // Workspace switches: focus never ends up on nothing or on a hidden item.
    QQuickItem* lanes = findNamed(w->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    lanes->forceActiveFocus();
    const auto focusOk = [&] {
        QQuickItem* f = w->activeFocusItem();
        return f && f != w->contentItem() && f->isVisible();
    };
    for (const int workspace : {2, 3, 1, 4, 0}) {
        w->setProperty("workspace", workspace);
        spin(60);
        INFO("workspace " << workspace);
        CHECK(focusOk());
    }
    // Panel switches from the rail put focus in the panel.
    for (const char* key : {"effects", "markers", "library"}) {
        REQUIRE(QMetaObject::invokeMethod(w, "showBrowserPanel", Q_ARG(QVariant, QString::fromLatin1(key))));
        spin(60);
        INFO(key);
        CHECK(focusOk());
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
