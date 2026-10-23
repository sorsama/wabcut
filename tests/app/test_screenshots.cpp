// Design-review screenshots in <build>/test-output/ui/. Each one is the first grab of a freshly created hidden
// window (see Shell::grab()). A warm-up grab first initializes the renderer once, so the GPU pill already shows
// the real backend/adapter (RendererInfo keeps the latest capture process-wide).

#include "ShellFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QMetaObject>
#include <QQuickItem>

#include <cmath>

using namespace wabcut::test;

namespace {

const QColor kViolet(0x9B, 0x6C, 0xFF); // design language v3 violet, the default accent

void warmUpRenderer() {
    Shell warmUp;
    REQUIRE(warmUp.window != nullptr);
    REQUIRE_FALSE(warmUp.grab().isNull());
    QObject* renderer = warmUp.object("rendererInfo");
    REQUIRE(renderer != nullptr);
    REQUIRE(waitFor([&] { return renderer->property("ready").toBool(); }));
}

void save(const QImage& image, const char* file) {
    const QString path = outputDir() + QLatin1Char('/') + QLatin1String(file);
    REQUIRE(image.save(path));
    UNSCOPED_INFO("wrote " << path.toStdString());
}

// Repeater delegates are not QObject children of the window: search the visual tree.
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

void requireCleanLog() {
    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("Design-review screenshots (1600x960)", "[app][screenshot]") {
    struct Variant {
        const char* file;
        bool dark;
        const char* accent;
        QColor fill;
    };
    // Design language v3: dark and light × the five accents (violet, pink, lime, orange, blue).
    const Variant variants[] = {
        {"dark-ultraviolet.png", true, "ultraviolet", kViolet},
        {"light-ultraviolet.png", false, "ultraviolet", kViolet},
        {"dark-infrared.png", true, "infrared", QColor(0xFF, 0x5F, 0xA8)},
        {"light-infrared.png", false, "infrared", QColor(0xFF, 0x5F, 0xA8)},
        {"dark-phosphor.png", true, "phosphor", QColor(0xD6, 0xF5, 0x4C)},
        {"light-phosphor.png", false, "phosphor", QColor(0xD6, 0xF5, 0x4C)},
        {"dark-sodium.png", true, "sodium", QColor(0xFF, 0x7A, 0x2F)},
        {"light-sodium.png", false, "sodium", QColor(0xFF, 0x7A, 0x2F)},
        {"dark-ice.png", true, "ice", QColor(0x4D, 0x8D, 0xFF)},
        {"light-ice.png", false, "ice", QColor(0x4D, 0x8D, 0xFF)},
    };

    resetSettings();
    clearMessages();
    warmUpRenderer();

    for (const Variant& v : variants) {
        INFO(v.file);
        resetSettings();
        // Theme switched at runtime on a live shell, then rendered.
        Shell shell;
        REQUIRE(shell.window != nullptr);
        REQUIRE(shell.theme != nullptr);
        shell.setAppearance(v.dark, v.accent);
        REQUIRE(shell.settle(v.dark, v.fill));
        CHECK_FALSE(shell.theme->property("compact").toBool());

        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        CHECK(image.width() == kShotWidth);
        CHECK(image.height() == kShotHeight);

        // The canvas shows in the window's left gutter (UX rework: no rail, the work area starts there); the active
        // workspace tab sits on the accent pill (sampled above its label, inside the pill's rounded end); the picture
        // sits straight on the panel (0.1.3: no black well round it).
        QQuickItem* content = shell.window->contentItem();
        QQuickItem* tab = findNamed(shell.window->contentItem(), QStringLiteral("workspaceTab_edit"));
        QQuickItem* slot = shell.item("programMonitorSlot");
        REQUIRE(tab != nullptr);
        REQUIRE(slot != nullptr);
        // The menu bar shows its titles at this size (one workspace).
        QQuickItem* fileTitle = findNamed(content, QStringLiteral("menuBar_file"));
        REQUIRE(fileTitle != nullptr);
        CHECK(fileTitle->isVisible());
        const QColor canvas = shell.themeColor("canvas");
        const QColor gap = pixelAt(image, content, QPointF(shell.theme->property("gap").toInt() / 2.0, content->height() / 2), 1.0);
        INFO("gap pixel " << describe(gap) << " vs canvas " << describe(canvas));
        CHECK(sameColor(gap, canvas));
        const QColor plate = pixelAt(image, tab, QPointF(tab->height() / 2, tab->height() / 4 - 1), 1.0);
        INFO("workspace plate pixel " << describe(plate) << " vs accent " << describe(v.fill));
        CHECK(sameColor(plate, v.fill, 3));
        // Just left of the 16:9 frame area (the slot fills that area) is the panel.
        const QColor well = pixelAt(image, slot, QPointF(-4, slot->height() / 2), 1.0);
        const QColor panel = shell.themeColor("panel");
        INFO("monitor surround pixel " << describe(well) << " vs panel " << describe(panel));
        CHECK(sameColor(well, panel, 3));


        save(image, v.file);
        requireCleanLog();
    }
}

TEST_CASE("Workspace, compact and popover screenshots", "[app][screenshot]") {
    resetSettings();
    clearMessages();
    warmUpRenderer();

    struct Shot {
        const char* file;
        int workspace;
        QSize size;
        // Object to open before the grab, "settings:<page>" (0.1.3 Settings window), "menu:<index>" (a menu of the
        // menu bar: AppMenuBar.open), or nullptr.
        const char* popup;
    };
    const Shot shots[] = {
        {"workspace-audio.png", 1, QSize(kShotWidth, kShotHeight), nullptr},
        {"workspace-export.png", 2, QSize(kShotWidth, kShotHeight), nullptr},
        {"workspace-color.png", 3, QSize(kShotWidth, kShotHeight), nullptr},
        {"workspace-captions.png", 4, QSize(kShotWidth, kShotHeight), nullptr},
        {"compact-1100x700.png", 0, QSize(1100, 700), nullptr},
        {"settings-general.png", 0, QSize(kShotWidth, kShotHeight), "settings:general"},
        {"settings-appearance.png", 0, QSize(kShotWidth, kShotHeight), "settings:appearance"},
        {"menu-sequences.png", 0, QSize(kShotWidth, kShotHeight), "sequencePill"},
        // UX rework: the menu bar's File and Window menus, and the collapsed bar (one Menu button) with File open.
        {"menubar-file.png", 0, QSize(kShotWidth, kShotHeight), "menu:0"},
        {"menubar-window.png", 0, QSize(kShotWidth, kShotHeight), "menu:7"},
        {"menubar-collapsed.png", 0, QSize(1100, 700), "menu:0"},
    };
    for (const Shot& shot : shots) {
        INFO(shot.file);
        resetSettings();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(shot.size.width(), shot.size.height());
        shell.window->setProperty("workspace", shot.workspace);
        if (shot.popup && QByteArray(shot.popup).startsWith("settings:")) {
            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings",
                                              Q_ARG(QString, QString::fromLatin1(shot.popup).mid(int(qstrlen("settings:")))),
                                              Q_ARG(QString, QString())));
        } else if (shot.popup && QByteArray(shot.popup).startsWith("menu:")) {
            QObject* bar = shell.object("appMenuBar");
            REQUIRE(bar != nullptr);
            spin(50); // the title bar lays out (and collapses the menus when it is narrow) first
            REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, QByteArray(shot.popup).mid(5).toInt()),
                                              Q_ARG(bool, false)));
        } else if (shot.popup) {
            QObject* target = shell.object(shot.popup);
            REQUIRE(target != nullptr);
            // The settings popover opens directly; the sequence menu opens from its pill's click handler.
            REQUIRE(QMetaObject::invokeMethod(target, qstrcmp(shot.popup, "sequencePill") == 0 ? "clicked" : "open"));
        }
        REQUIRE(shell.settle(true, kViolet)); // also lets layout glides and popup transitions finish

        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        CHECK(image.size() == shot.size);
        save(image, shot.file);
        requireCleanLog();
    }
}

namespace {

// Renders the shell at the process-wide scale factor (set from the test's tag, see the fixture).
void renderAtScale(double scale, const char* file) {
    if (appScaleFactor() != scale)
        SKIP("needs its own process (ctest runs it that way): the scale factor is fixed at startup");
    resetSettings();
    clearMessages();
    warmUpRenderer();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.settle(true, kViolet));
    CHECK(shell.window->effectiveDevicePixelRatio() == scale);

    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.width() == int(std::lround(kShotWidth * scale)));
    CHECK(image.height() == int(std::lround(kShotHeight * scale)));
    save(image, file);
    requireCleanLog();
}

} // namespace

TEST_CASE("High-DPI rendering at 125%", "[app][screenshot][hidpi125]") {
    renderAtScale(1.25, "dark-ultraviolet@125.png");
}

TEST_CASE("High-DPI rendering at 150%", "[app][screenshot][hidpi150]") {
    renderAtScale(1.5, "dark-ultraviolet@150.png");
}
