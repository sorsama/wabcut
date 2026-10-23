// 0.1.3 Settings window (WP-S): theme files (ThemeFiles: validation, export/import round trip), every Settings page
// opening warning-free (from Edit > Settings…), custom colours reaching Theme and its saved settings, the title bar's
// status pills coming back from Settings > Interface, and the AI features switch (UX rework: it hides Generate in the
// Window menu; the left rail and its customization are gone).

#include "ShellFixture.h"

#include "bridge/AiController.h"
#include "bridge/ThemeFiles.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

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

QVariant stored(const char* group, const char* key) {
    QSettings settings;
    settings.beginGroup(QLatin1String(group));
    return settings.value(QLatin1String(key));
}

/// The Settings window once the window created it (its Loader's item), else null.
QObject* settingsWindow(const Shell& shell) {
    QObject* loader = shell.object("settingsWindowLoader");
    return loader ? loader->property("item").value<QObject*>() : nullptr;
}

/// `name` inside the Settings window (its pages are delegates: walk the visual tree).
QQuickItem* inSettings(QObject* window, const QString& name) {
    return findNamed(window->property("contentItem").value<QQuickItem*>(), name);
}

/// Opens Settings on `page` (window.openSettings, as Edit > Settings… and Home's Settings button do); returns the window.
QObject* openSettings(Shell& shell, const QString& page) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, page), Q_ARG(QString, QString())));
    QObject* window = settingsWindow(shell);
    REQUIRE(window != nullptr);
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    return window;
}

/// The page item of the Settings window for `key` (created on first show).
QQuickItem* settingsPage(QObject* window, const QString& key) {
    QQuickItem* page = nullptr;
    REQUIRE(QMetaObject::invokeMethod(window, "page", Q_RETURN_ARG(QQuickItem*, page), Q_ARG(QString, key)));
    return page;
}

QVariantMap theme(const QString& name, const QVariantMap& colors) {
    return {{QStringLiteral("format"), QStringLiteral("wabcut-theme")}, {QStringLiteral("version"), 1},
            {QStringLiteral("name"), name}, {QStringLiteral("dark"), true}, {QStringLiteral("accent"), QStringLiteral("ice")},
            {QStringLiteral("colors"), colors}};
}

} // namespace

TEST_CASE("Theme files: checked on the way in and out", "[app][settings][theme]") {
    SECTION("unknown keys and bad colours are dropped, the rest upper-cased") {
        const auto t = ThemeFiles::normalizeTheme(theme(QStringLiteral("  Midnight   blue "),
                                                        {{QStringLiteral("panel"), QStringLiteral("#0e1320")},
                                                         {QStringLiteral("border"), QStringLiteral("#1a8fb3ff")},
                                                         {QStringLiteral("notAToken"), QStringLiteral("#FFFFFF")},
                                                         {QStringLiteral("canvas"), QStringLiteral("red")}}));
        REQUIRE(t.has_value());
        CHECK(t->value(QStringLiteral("name")).toString() == QStringLiteral("Midnight blue"));
        const QVariantMap colors = t->value(QStringLiteral("colors")).toMap();
        CHECK(colors.size() == 2);
        CHECK(colors.value(QStringLiteral("panel")).toString() == QStringLiteral("#0E1320"));
        CHECK(colors.value(QStringLiteral("border")).toString() == QStringLiteral("#1A8FB3FF"));
        CHECK(t->value(QStringLiteral("accent")).toString() == QStringLiteral("ice"));
    }
    SECTION("clip labels: all 16 #RRGGBB or none") {
        QVariantMap t = theme(QStringLiteral("Labels"), {});
        t.insert(QStringLiteral("clipColors"), QVariantList{QStringLiteral("#FF0000")});
        CHECK_FALSE(ThemeFiles::normalizeTheme(t).has_value());
        QVariantList sixteen;
        for (int i = 0; i < 16; ++i)
            sixteen.push_back(QStringLiteral("#a0a0a%1").arg(i % 10));
        t.insert(QStringLiteral("clipColors"), sixteen);
        const auto ok = ThemeFiles::normalizeTheme(t);
        REQUIRE(ok.has_value());
        CHECK(ok->value(QStringLiteral("clipColors")).toList().front().toString() == QStringLiteral("#A0A0A0"));
        sixteen[3] = QStringLiteral("#80A0A0A0"); // a label colour has no alpha
        t.insert(QStringLiteral("clipColors"), sixteen);
        CHECK_FALSE(ThemeFiles::normalizeTheme(t).has_value());
    }
    SECTION("another format or a newer version is refused") {
        QVariantMap other = theme(QStringLiteral("x"), {{QStringLiteral("panel"), QStringLiteral("#000000")}});
        other.insert(QStringLiteral("format"), QStringLiteral("vscode-theme"));
        CHECK_FALSE(ThemeFiles::normalizeTheme(other).has_value());
        QVariantMap newer = theme(QStringLiteral("x"), {{QStringLiteral("panel"), QStringLiteral("#000000")}});
        newer.insert(QStringLiteral("version"), 2);
        const auto refused = ThemeFiles::normalizeTheme(newer);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains(QStringLiteral("newer")));
        CHECK_FALSE(ThemeFiles::fromJson("{ not json").has_value());
        CHECK_FALSE(ThemeFiles::fromJson(R"({"colors": {"panel": "#000000"}})").has_value()); // no format
    }
    SECTION("export then import gives the same theme") {
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        ThemeFiles files;
        const QUrl url = QUrl::fromLocalFile(dir.filePath(files.suggestedFileName(QStringLiteral("Mid/night"))));
        CHECK(url.fileName() == QStringLiteral("Mid-night.wabcut-theme.json"));
        const QVariantMap original = theme(QStringLiteral("Midnight"), {{QStringLiteral("panel"), QStringLiteral("#0E1320")},
                                                                       {QStringLiteral("textPrimary"), QStringLiteral("#EDF2FB")}});
        CHECK(files.exportTheme(url, original).isEmpty());
        QFile written(url.toLocalFile());
        REQUIRE(written.open(QIODevice::ReadOnly));
        const QJsonObject json = QJsonDocument::fromJson(written.readAll()).object();
        CHECK(json.value(QStringLiteral("format")).toString() == QStringLiteral("wabcut-theme"));
        CHECK(json.value(QStringLiteral("version")).toInt() == 1);
        const QVariantMap back = files.importTheme(url);
        REQUIRE(back.value(QStringLiteral("ok")).toBool());
        const QVariantMap t = back.value(QStringLiteral("theme")).toMap();
        CHECK(t.value(QStringLiteral("name")).toString() == QStringLiteral("Midnight"));
        CHECK(t.value(QStringLiteral("colors")).toMap() == original.value(QStringLiteral("colors")).toMap());
        const QVariantMap missing = files.importTheme(QUrl::fromLocalFile(dir.filePath(QStringLiteral("none.json"))));
        CHECK_FALSE(missing.value(QStringLiteral("ok")).toBool());
        CHECK_FALSE(missing.value(QStringLiteral("error")).toString().isEmpty());
    }
}

TEST_CASE("Settings window: every page opens without warnings", "[app][settings][qml]") {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);

    // Edit > Settings… opens it (once the menu has closed).
    QObject* item = shell.object("editMenuSettings");
    REQUIRE(item != nullptr);
    CHECK(item->property("command").toString() == QStringLiteral("edit.preferences"));
    REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
    REQUIRE(waitFor([&] { return settingsWindow(shell) != nullptr; }));
    QObject* window = settingsWindow(shell);
    REQUIRE(window != nullptr);
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    CHECK(window->property("currentPage").toString() == QStringLiteral("general"));

    for (const char* key : {"general", "appearance", "interface", "timeline", "performance", "audio", "ai", "keyboard",
                            "extensions"}) {
        INFO("page " << key);
        QObject* category = inSettings(window, QStringLiteral("settingsCategory_%1").arg(QLatin1String(key)));
        REQUIRE(category != nullptr);
        REQUIRE(QMetaObject::invokeMethod(category, "clicked"));
        CHECK(window->property("currentPage").toString() == QLatin1String(key));
        QQuickItem* page = nullptr;
        REQUIRE(waitFor([&] {
            page = settingsPage(window, QLatin1String(key));
            return page != nullptr && page->isVisible();
        }));
        CHECK(page->width() > 400);
        spin(30);
    }
    REQUIRE_FALSE(shell.grab().isNull());
    writeOutputFile(QStringLiteral("settings-pages.txt"), QStringLiteral("every Settings page opened\n"));

    // Escape-free close and reopen on a given page (the transcript's "set up transcription" opens AI).
    REQUIRE(QMetaObject::invokeMethod(window, "close"));
    REQUIRE(waitFor([&] { return !window->property("visible").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openAiPreferences"));
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    CHECK(window->property("currentPage").toString() == QStringLiteral("ai"));
    QObject* ai = inSettings(window, QStringLiteral("aiPreferences"));
    REQUIRE(ai != nullptr);
    CHECK(waitFor([&] { return ai->property("opened").toBool(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Settings Appearance: custom colours and themes reach Theme and are kept", "[app][settings][theme][qml]") {
    resetSettings();
    clearMessages();
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        const QColor builtInPanel = shell.themeColor("panel");

        QObject* window = openSettings(shell, QStringLiteral("appearance"));
        QQuickItem* page = nullptr;
        REQUIRE(waitFor([&] { return (page = settingsPage(window, QStringLiteral("appearance"))) != nullptr; }));
        CHECK(findNamed(page, QStringLiteral("themeColorEditor")) != nullptr);
        CHECK(findNamed(page, QStringLiteral("themeSwatch_panel")) != nullptr);

        // One token overridden: Theme follows, the settings keep it, high contrast pauses it.
        shell.theme->setProperty("customColors", QVariantMap{{QStringLiteral("panel"), QStringLiteral("#123456")}});
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("panel"), QColor(0x12, 0x34, 0x56), 0); }));
        // QtCore Settings writes a moment later.
        CHECK(waitFor([] { return stored("appearance", "customColors").toString().contains(QStringLiteral("#123456")); }));
        shell.theme->setProperty("highContrast", true);
        REQUIRE(waitFor([&] { return !sameColor(shell.themeColor("panel"), QColor(0x12, 0x34, 0x56), 0); }));
        shell.theme->setProperty("highContrast", false);
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("panel"), QColor(0x12, 0x34, 0x56), 0); }));

        // A preset: Midnight re-tints the surfaces and takes the Ice accent; Ultraviolet gives the built-in look back.
        QVariant presets = page->property("presets");
        QVariantMap midnight;
        for (const QVariant& p : presets.toList()) {
            if (p.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Midnight"))
                midnight = p.toMap();
        }
        REQUIRE_FALSE(midnight.isEmpty());
        REQUIRE(QMetaObject::invokeMethod(page, "applyTheme", Q_ARG(QVariant, QVariant(midnight))));
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("panel"), QColor(0x0E, 0x13, 0x20), 0); }));
        CHECK(shell.theme->property("accentName").toString() == QStringLiteral("ice"));
        CHECK(shell.preferences->property("themeName").toString() == QStringLiteral("Midnight"));
        CHECK(page->property("matchingTheme").toString() == QStringLiteral("Midnight"));

        // Saved under a name, it is a theme card of its own.
        bool saved = false;
        REQUIRE(QMetaObject::invokeMethod(page, "saveCurrentAs", Q_RETURN_ARG(bool, saved),
                                          Q_ARG(QString, QStringLiteral("Night shift"))));
        CHECK(saved);
        CHECK(waitFor([] { return stored("appearance", "savedThemes").toString().contains(QStringLiteral("Night shift")); }));
        REQUIRE(waitFor([&] { return findNamed(page, QStringLiteral("themeCard_Night shift")) != nullptr; }));

        QVariantMap ultraviolet = presets.toList().front().toMap();
        REQUIRE(QMetaObject::invokeMethod(page, "applyTheme", Q_ARG(QVariant, QVariant(ultraviolet))));
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("panel"), builtInPanel, 0); }));
        CHECK(shell.theme->property("customColors").toMap().isEmpty());

        // Export and import go through ThemeFiles.
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        shell.theme->setProperty("customColors", QVariantMap{{QStringLiteral("raised"), QStringLiteral("#202024")}});
        const QUrl file = QUrl::fromLocalFile(dir.filePath(QStringLiteral("mine.wabcut-theme.json")));
        QString error;
        REQUIRE(QMetaObject::invokeMethod(page, "exportTo", Q_RETURN_ARG(QString, error), Q_ARG(QUrl, file)));
        CHECK(error.isEmpty());
        shell.theme->setProperty("customColors", QVariantMap{});
        REQUIRE(QMetaObject::invokeMethod(page, "importFrom", Q_RETURN_ARG(QString, error), Q_ARG(QUrl, file)));
        CHECK(error.isEmpty());
        CHECK(shell.theme->property("customColors").toMap().value(QStringLiteral("raised")).toString() == QStringLiteral("#202024"));
        REQUIRE_FALSE(shell.grab().isNull());
    }
    // The next start applies the kept colours before anything binds to them.
    {
        clearMessages();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        CHECK(shell.theme->property("customColors").toMap().value(QStringLiteral("raised")).toString() == QStringLiteral("#202024"));
        CHECK(sameColor(shell.themeColor("raised"), QColor(0x20, 0x20, 0x24), 0));
    }
    resetSettings();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Settings Interface: the title bar status pills come back on request", "[app][settings][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    QQuickItem* gpu = shell.item("gpuPill");
    QQuickItem* autosave = shell.item("autosavePill");
    QQuickItem* save = shell.item("saveButton");
    REQUIRE(gpu != nullptr);
    REQUIRE(autosave != nullptr);
    REQUIRE(save != nullptr);
    CHECK_FALSE(gpu->isVisible());
    CHECK_FALSE(autosave->isVisible());
    CHECK(shell.item("appearanceButton") == nullptr); // Appearance is a Settings page now
    CHECK_FALSE(save->property("tooltip").toString().isEmpty());

    QObject* window = openSettings(shell, QStringLiteral("interface"));
    QQuickItem* page = nullptr;
    REQUIRE(waitFor([&] { return (page = settingsPage(window, QStringLiteral("interface"))) != nullptr; }));
    QQuickItem* gpuToggle = findNamed(page, QStringLiteral("showGpuStatusToggle"));
    QQuickItem* saveToggle = findNamed(page, QStringLiteral("showSaveStatusToggle"));
    REQUIRE(gpuToggle != nullptr);
    REQUIRE(saveToggle != nullptr);
    REQUIRE(QMetaObject::invokeMethod(gpuToggle, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(gpuToggle, "toggled"));
    REQUIRE(QMetaObject::invokeMethod(saveToggle, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(saveToggle, "toggled"));
    CHECK(gpu->isVisible());
    CHECK(autosave->isVisible());
    CHECK(waitFor([] { return stored("interface", "showGpuStatus").toBool(); })); // written a moment later
    CHECK(waitFor([] { return stored("interface", "showSaveStatus").toBool(); }));

    resetSettings();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Settings AI: one switch turns the AI features off and on again", "[app][settings][ai][qml]") {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    REQUIRE(ai != nullptr);
    ai->setOfflineMode(false);
    // The Window menu lists Generate while AI is on (UX rework: it replaced the left rail's entry).
    REQUIRE(shell.object("windowMenu_generate") != nullptr);

    QObject* window = openSettings(shell, QStringLiteral("ai"));
    QObject* page = inSettings(window, QStringLiteral("aiPreferences"));
    REQUIRE(page != nullptr);
    REQUIRE(QMetaObject::invokeMethod(page, "setAiEnabled", Q_ARG(bool, false)));
    CHECK_FALSE(shell.preferences->property("aiEnabled").toBool());
    CHECK(ai->offlineMode());
    spin(30);
    CHECK(shell.object("windowMenu_generate") == nullptr);
    CHECK(shell.object("windowMenu_library") != nullptr); // the other panels stay

    REQUIRE(QMetaObject::invokeMethod(page, "setAiEnabled", Q_ARG(bool, true)));
    CHECK(shell.preferences->property("aiEnabled").toBool());
    CHECK_FALSE(ai->offlineMode()); // what it was before
    spin(30);
    CHECK(shell.object("windowMenu_generate") != nullptr);

    // A model by name becomes the assistant's.
    bool used = false;
    REQUIRE(QMetaObject::invokeMethod(page, "useCustomModel", Q_RETURN_ARG(bool, used),
                                      Q_ARG(QString, QStringLiteral("my-custom-model-1"))));
    CHECK(used);
    CHECK(ai->chatModel() == QStringLiteral("my-custom-model-1"));
    CHECK(ai->removeModel(QStringLiteral("my-custom-model-1")));

    resetSettings();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
