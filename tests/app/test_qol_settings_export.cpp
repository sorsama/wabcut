// UX rework (2026-10-23, stream L): the C++ side of the library, export and settings QoL items.
//   * AppSettings (AppSettingsBridge): the start-up choice and the project to reopen, the Settings search index and
//     search, "Settings" rows in the command palette that ask Main to show a setting;
//   * export remembers the last choices (ExportController lastPreset / lastGamingPreset / lastQuickPreset /
//     lastFolder / lastRange, suggestedRange, forgetExportChoices), and a finished job is described for a toast (job,
//     openOutput, the job's origin).
// Offscreen; no window is shown. The Library keys are in test_library.cpp, Render In to Out's outcome in
// test_render_in_to_out.cpp, the Settings key in tests/keymap/test_default_tables.cpp.
// Phase 2 (QML, the real Main.qml hidden; screenshots in test-output/ui): the Settings search reveals a row and every
// index row exists on its page, Settings > General's new rows, the Library's thumbnail slider and selection keys, the
// Export panel and Quick Export starting from the last choices.

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "AppSettingsBridge.h"
#include "CommandPalette.h"
#include "ExportController.h"
#include "HomeBridge.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/export/ExportJob.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QKeyEvent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;
namespace fs = std::filesystem;

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "qol" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
}

/// Same file system path, whatever the separators (and the case, on Windows).
bool samePath(const QString& a, const QString& b) {
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return !a.isEmpty() &&
           QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), sensitivity) == 0;
}

/// One 640x360 30 fps sequence of `seconds` with nothing on it (exports render silence and the synthetic picture),
/// optionally with In/Out marks.
ProjectState makeProject(std::int64_t seconds, bool marked = false) {
    ProjectState state;
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Interview";
    seq.width = 640;
    seq.height = 360;
    seq.frameRate = kRate30;
    seq.duration = Ticks::fromSeconds(seconds);
    if (marked) {
        seq.markIn = Ticks{};
        seq.markOut = Ticks::fromSeconds(1);
    }
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

bool finished(const ExportController& c, const QString& jobId, int timeoutMs = 60'000) {
    return waitFor([&] { return c.job(jobId).value(QStringLiteral("finished")).toBool(); }, timeoutMs);
}

// ---- QML helpers (Phase 2) ----

QQuickItem* findWith(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findWith(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* named(QQuickItem* root, const QString& name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

/// A QML `var` property (a JS array or object) as a QVariant.
QVariant fromJs(const QVariant& value) {
    return value.canConvert<QJSValue>() && value.metaType() == QMetaType::fromType<QJSValue>()
               ? value.value<QJSValue>().toVariant()
               : value;
}

void tapKey(QQuickWindow* window, int key, Qt::KeyboardModifiers modifiers = {}) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QCoreApplication::sendEvent(window, &release);
}

/// The Settings window (its Loader made active, as window.openSettings does), not opened yet.
QObject* settingsWindow(Shell& shell) {
    QObject* loader = shell.object("settingsWindowLoader");
    REQUIRE(loader != nullptr);
    loader->setProperty("active", true);
    QObject* window = loader->property("item").value<QObject*>();
    REQUIRE(window != nullptr);
    return window;
}

QQuickItem* inSettings(QObject* window, const QString& name) {
    return named(window->property("contentItem").value<QQuickItem*>(), name);
}

/// The flash rectangle of a SettingsRow (or a SettingsSection's card).
QQuickItem* flashOf(QQuickItem* row) {
    return findWith(row, [](QQuickItem* i) {
        return i->objectName() == QStringLiteral("settingsRowFlash") || i->objectName() == QStringLiteral("settingsSectionFlash");
    });
}

/// Runs `code` in Main.qml's context (its ids: window, docking, ...; the Workspaces singleton).
QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

/// Saves `rect` (scene coordinates) of `image` under test-output/ui.
void saveCrop(const QImage& image, const QRectF& rect, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRect pixels(static_cast<int>(rect.x() * dpr), static_cast<int>(rect.y() * dpr),
                       static_cast<int>(rect.width() * dpr), static_cast<int>(rect.height() * dpr));
    REQUIRE(image.copy(pixels).save(QDir(outputDir()).filePath(name)));
}

} // namespace

// ---- AppSettings ------------------------------------------------------------------------------------------------------

TEST_CASE("AppSettings: the start-up choice and the project to reopen", "[app][settings][ux]") {
    resetSettings();
    const fs::path dir = freshDir("reopen");
    std::ofstream(dir / "Last.wabcut") << "{}";
    std::ofstream(dir / "Older.wabcut") << "{}";
    const QVariantMap last{{QStringLiteral("url"), QUrl::fromLocalFile(q(dir / "Last.wabcut"))},
                           {QStringLiteral("path"), q(dir / "Last.wabcut")},
                           {QStringLiteral("missing"), false}};
    const QVariantMap older{{QStringLiteral("url"), QUrl::fromLocalFile(q(dir / "Older.wabcut"))},
                            {QStringLiteral("path"), q(dir / "Older.wabcut")},
                            {QStringLiteral("missing"), false}};

    AppSettingsBridge settings;
    int changes = 0;
    QObject::connect(&settings, &AppSettingsBridge::launchActionChanged, [&] { ++changes; });
    // By default Wabcut shows Home: nothing is reopened.
    CHECK(settings.launchAction() == QStringLiteral("home"));
    CHECK(settings.projectToReopen({last, older}).isEmpty());

    // "Reopen the last project": the newest recent project.
    settings.setLaunchAction(QStringLiteral("reopen"));
    CHECK(settings.launchAction() == QStringLiteral("reopen"));
    CHECK(changes == 1);
    settings.setLaunchAction(QStringLiteral("reopen"));
    CHECK(changes == 1); // no change, no signal
    const QUrl reopened = settings.projectToReopen({last, older});
    REQUIRE(reopened.isLocalFile());
    CHECK(samePath(reopened.toLocalFile(), q(dir / "Last.wabcut")));
    // An entry with a path only (no url) works too.
    CHECK(samePath(settings.projectToReopen({QVariantMap{{QStringLiteral("path"), q(dir / "Older.wabcut")}}}).toLocalFile(),
                   q(dir / "Older.wabcut")));
    // The newest one is missing or gone: Home shows (an older project is not opened instead).
    QVariantMap missing = last;
    missing[QStringLiteral("missing")] = true;
    CHECK(settings.projectToReopen({missing, older}).isEmpty());
    CHECK(settings.projectToReopen({QVariantMap{{QStringLiteral("path"), q(dir / "Gone.wabcut")}}, older}).isEmpty());
    CHECK(settings.projectToReopen({}).isEmpty());

    // Kept for the next start; junk reads as Home.
    {
        AppSettingsBridge next;
        CHECK(next.launchAction() == QStringLiteral("reopen"));
    }
    settings.setLaunchAction(QStringLiteral("splash"));
    CHECK(settings.launchAction() == QStringLiteral("home"));
    CHECK(changes == 2);
}

TEST_CASE("AppSettings: the settings search finds rows by title and keyword and opens them", "[app][settings][ux]") {
    AppSettingsBridge settings;

    // The index: unique ids, the Settings window's pages, the dialogs runAction knows.
    const QVariantList index = settings.index();
    REQUIRE(index.size() >= 40);
    const QStringList pages{QStringLiteral("general"), QStringLiteral("appearance"), QStringLiteral("interface"),
                            QStringLiteral("timeline"), QStringLiteral("performance"), QStringLiteral("audio"),
                            QStringLiteral("ai"), QStringLiteral("keyboard"), QStringLiteral("extensions")};
    const QStringList dialogs{QString(), QStringLiteral("media"), QStringLiteral("plugins"), QStringLiteral("audio"),
                              QStringLiteral("extensions"), QStringLiteral("shortcuts"), QStringLiteral("trainer"),
                              QStringLiteral("editing")};
    QSet<QString> ids;
    for (const QVariant& v : index) {
        const QVariantMap e = v.toMap();
        const QString id = e.value(QStringLiteral("id")).toString();
        INFO(id.toStdString());
        CHECK_FALSE(ids.contains(id));
        ids.insert(id);
        CHECK(pages.contains(e.value(QStringLiteral("page")).toString()));
        CHECK(dialogs.contains(e.value(QStringLiteral("opens")).toString()));
        CHECK_FALSE(e.value(QStringLiteral("row")).toString().isEmpty());
        CHECK_FALSE(e.value(QStringLiteral("title")).toString().isEmpty());
        CHECK(e.value(QStringLiteral("detail")).toString().startsWith(e.value(QStringLiteral("pageTitle")).toString()));
    }
    // The General page's new rows are there.
    for (const char* id : {"launchAction", "projectLocation", "recentProjects", "lockProjects"}) {
        INFO(id);
        CHECK(ids.contains(QString::fromLatin1(id)));
    }

    // Search: by the title's words first, then keywords; every word has to match.
    const auto first = [&](const char* query) {
        const QVariantList hits = settings.search(QString::fromLatin1(query));
        return hits.isEmpty() ? QString() : hits.front().toMap().value(QStringLiteral("id")).toString();
    };
    CHECK(settings.search({}).isEmpty());
    CHECK(settings.search(QStringLiteral("   ")).isEmpty());
    CHECK(first("autosave") == QStringLiteral("autosave"));
    CHECK(first("Dark") == QStringLiteral("mode"));
    CHECK(first("proxies") == QStringLiteral("mediaCache"));
    CHECK(first("reopen") == QStringLiteral("launchAction"));
    CHECK(first("default folder") == QStringLiteral("projectLocation"));
    CHECK(first("lock") == QStringLiteral("lockProjects"));
    CHECK(first("intro") == QStringLiteral("launchIntro"));
    CHECK(first("vst") == QStringLiteral("audioPlugins"));
    CHECK(settings.search(QStringLiteral("dark proxies")).isEmpty());
    CHECK(settings.search(QStringLiteral("qqxxzz")).isEmpty());
    CHECK(settings.search(QStringLiteral("e"), 3).size() == 3);
    const QVariantMap hit = settings.search(QStringLiteral("autosave")).front().toMap();
    CHECK(hit.value(QStringLiteral("page")).toString() == QStringLiteral("general"));
    CHECK(hit.value(QStringLiteral("row")).toString() == QStringLiteral("autosave"));
    CHECK(hit.value(QStringLiteral("score")).toInt() > 0);
    CHECK(settings.search(QStringLiteral("proxies")).front().toMap().value(QStringLiteral("opens")).toString()
          == QStringLiteral("media"));

    // entry() and openSetting(): Main shows the page and reveals the row.
    CHECK(settings.entry(QStringLiteral("autosave")).value(QStringLiteral("title")).toString()
          == QStringLiteral("Autosave interval"));
    CHECK(settings.entry(QStringLiteral("no-such-setting")).isEmpty());
    QStringList opened;
    QObject::connect(&settings, &AppSettingsBridge::openRequested,
                     [&](const QString& page, const QString& rowKey) { opened << page + QLatin1Char('/') + rowKey; });
    CHECK(settings.openSetting(QStringLiteral("projectLocation")));
    CHECK_FALSE(settings.openSetting(QStringLiteral("no-such-setting")));
    CHECK(opened == QStringList{QStringLiteral("general/projectLocation")});
}

TEST_CASE("AppSettings: the command palette lists every setting and opens it", "[app][settings][palette][ux]") {
    AppSettingsBridge settings;
    CommandPalette palette;
    palette.setPersistRecent(false);
    palette.addProvider(AppSettingsBridge::paletteProvider(&settings));

    palette.setQuery(QStringLiteral("autosave"));
    QVariantMap row;
    for (const QVariant& v : palette.results()) {
        if (v.toMap().value(QStringLiteral("key")).toString() == QStringLiteral("action:settings.autosave"))
            row = v.toMap();
    }
    REQUIRE_FALSE(row.isEmpty());
    CHECK(row.value(QStringLiteral("title")).toString() == QStringLiteral("Autosave interval"));
    CHECK(row.value(QStringLiteral("group")).toString() == QStringLiteral("Settings"));
    CHECK(row.value(QStringLiteral("enabled")).toBool());

    QStringList opened;
    QObject::connect(&settings, &AppSettingsBridge::openRequested,
                     [&](const QString& page, const QString& rowKey) { opened << page + QLatin1Char('/') + rowKey; });
    CHECK(palette.activate(QStringLiteral("action:settings.autosave")).isEmpty());
    CHECK(opened == QStringList{QStringLiteral("general/autosave")});

    // Settings... itself and the Library's zoom keys run from the palette (Main.qml routes them).
    CHECK(KeymapCommandProvider::viewHandles(QStringLiteral("edit.preferences")));
    CHECK(KeymapCommandProvider::viewHandles(QStringLiteral("project.zoomIn")));
    CHECK(KeymapCommandProvider::viewHandles(QStringLiteral("project.moveSelectionHome")));
}

// ---- Export ----------------------------------------------------------------------------------------------------------

TEST_CASE("Export memory: the last presets folder and range are kept for the next export", "[app][export][ux]") {
    resetSettings();
    const fs::path dir = freshDir("export-memory");
    ExportController::setUserPresetFolder(dir / "presets");
    ProjectController project;
    project.resetProject(makeProject(2));
    ExportController controller;
    controller.setProject(&project);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    controller.setProjectFile(dir / "Interview.wabcut");
    int changes = 0;
    QObject::connect(&controller, &ExportController::exportMemoryChanged, [&] { ++changes; });

    // Nothing remembered yet: the defaults.
    CHECK(controller.lastPreset() == QStringLiteral("match-source"));
    CHECK(controller.lastGamingPreset().isEmpty());
    CHECK(controller.lastQuickPreset() == QStringLiteral("match-source"));
    CHECK(controller.lastFolder().isEmpty());
    CHECK(controller.lastFolderUrl().isEmpty());
    CHECK(controller.lastRange().isEmpty());
    CHECK(controller.suggestedRange({}) == QStringLiteral("whole"));
    CHECK(samePath(controller.defaultOutputPath({}, QStringLiteral("match-source")), q(dir / "Interview.mp4")));

    // Presets: known ids are kept (the next start reads them); unknown ones are ignored.
    controller.setLastPreset(QStringLiteral("youtube-1080p"));
    controller.setLastPreset(QStringLiteral("no-such-preset"));
    CHECK(controller.lastPreset() == QStringLiteral("youtube-1080p"));
    controller.setLastQuickPreset(QStringLiteral("audio-wav"));
    CHECK(controller.lastQuickPreset() == QStringLiteral("audio-wav"));
    controller.setLastGamingPreset(QStringLiteral("youtube-1080p60"));
    controller.setLastGamingPreset(QStringLiteral("no-such-gaming-preset"));
    CHECK(controller.lastGamingPreset() == QStringLiteral("youtube-1080p60"));
    CHECK(changes == 3);
    {
        ExportController next;
        CHECK(next.lastPreset() == QStringLiteral("youtube-1080p"));
        CHECK(next.lastQuickPreset() == QStringLiteral("audio-wav"));
        CHECK(next.lastGamingPreset() == QStringLiteral("youtube-1080p60"));
    }
    controller.setLastGamingPreset({}); // a built-in preset was chosen after it
    CHECK(controller.lastGamingPreset().isEmpty());
    // A user preset deleted later falls back to Match Source without a word.
    const QVariantMap saved = controller.savePreset(QStringLiteral("Podcast"), {}, QStringLiteral("audio-wav"));
    REQUIRE(saved.value(QStringLiteral("ok")).toBool());
    const QString userId = saved.value(QStringLiteral("id")).toString();
    controller.setLastPreset(userId);
    CHECK(controller.lastPreset() == userId);
    REQUIRE(controller.deletePreset(userId));
    CHECK(controller.lastPreset() == QStringLiteral("match-source"));

    // The folder: an export to a chosen destination makes it the default for the next one.
    const fs::path exports = dir / "Exports";
    fs::create_directories(exports);
    const QString job = controller.queueExport({}, QStringLiteral("audio-wav"),
                                               {{QStringLiteral("output"), q(exports / "mix.wav")}});
    REQUIRE_FALSE(job.isEmpty());
    CHECK(samePath(controller.lastFolder(), q(exports)));
    CHECK(samePath(controller.lastFolderUrl().toLocalFile(), q(exports)));
    CHECK(samePath(controller.folderUrlOf(controller.lastFolder()).toLocalFile(), q(exports)));
    CHECK(samePath(controller.folderUrlOf(q(exports / "mix.wav")).toLocalFile(), q(exports)));
    CHECK(controller.folderUrlOf({}).isEmpty());
    CHECK(samePath(controller.defaultOutputPath({}, QStringLiteral("match-source")), q(exports / "Interview.mp4")));
    CHECK(samePath(controller.describe({}, QStringLiteral("match-source")).value(QStringLiteral("output")).toString(),
                   q(exports / "Interview.mp4")));
    REQUIRE(finished(controller, job));
    {
        ExportController next;
        CHECK(samePath(next.lastFolder(), q(exports)));
    }
    // An export to the default name does not move the folder.
    const QString again = controller.queueExport({}, QStringLiteral("audio-wav"));
    REQUIRE_FALSE(again.isEmpty());
    REQUIRE(finished(controller, again));
    CHECK(samePath(controller.lastFolder(), q(exports)));
    // The folder is gone (an unplugged drive): next to the project again.
    for (int attempt = 0; attempt < 50 && fs::exists(exports); ++attempt) {
        std::error_code ec;
        fs::remove_all(exports, ec); // a scanner may hold the new files for a moment
        if (fs::exists(exports))
            spin(100);
    }
    REQUIRE_FALSE(fs::exists(exports));
    CHECK(controller.lastFolder().isEmpty());
    CHECK(controller.lastFolderUrl().isEmpty());
    CHECK(samePath(controller.defaultOutputPath({}, QStringLiteral("match-source")), q(dir / "Interview.mp4")));

    // The range: only real choices are kept; the suggestion follows the sequence's marks.
    controller.setLastRange(QStringLiteral("custom"));
    CHECK(controller.lastRange().isEmpty());
    controller.setLastRange(QStringLiteral("workArea"));
    CHECK(controller.lastRange() == QStringLiteral("workArea"));
    CHECK(controller.suggestedRange({}) == QStringLiteral("whole")); // no work area here
    project.resetProject(makeProject(2, true));
    controller.setLastRange({});
    CHECK(controller.suggestedRange({}) == QStringLiteral("inOut")); // marks and no choice: In to Out
    controller.setLastRange(QStringLiteral("whole"));
    CHECK(controller.suggestedRange({}) == QStringLiteral("whole")); // the user's choice wins
    controller.setLastRange(QStringLiteral("inOut"));
    CHECK(controller.suggestedRange({}) == QStringLiteral("inOut"));
    project.resetProject(makeProject(2));
    CHECK(controller.suggestedRange({}) == QStringLiteral("whole")); // In to Out without marks: the whole sequence
    {
        ExportController next;
        CHECK(next.lastRange() == QStringLiteral("inOut"));
    }

    // Reset to defaults.
    const int before = changes;
    controller.forgetExportChoices();
    CHECK(changes == before + 1);
    CHECK(controller.lastPreset() == QStringLiteral("match-source"));
    CHECK(controller.lastQuickPreset() == QStringLiteral("match-source"));
    CHECK(controller.lastGamingPreset().isEmpty());
    CHECK(controller.lastFolder().isEmpty());
    CHECK(controller.lastRange().isEmpty());
}

TEST_CASE("Export jobs: a finished job is described for its toast and opens in its app", "[app][export][ux]") {
    resetSettings();
    const fs::path dir = freshDir("export-jobs");
    ProjectController project;
    project.resetProject(makeProject(2));
    ExportController controller;
    controller.setProject(&project);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    controller.setProjectFile(dir / "Interview.wabcut");
    QStringList opened;
    controller.setOpenHandler([&](const QString& path) {
        opened << path;
        return true;
    });
    QStringList finishedJobs;
    QObject::connect(&controller, &ExportController::jobFinished,
                     [&](const QString& jobId, bool, const QString&) { finishedJobs << jobId; });

    // Quick Export (with an override: the range) - "quick"; not done yet, so it does not open.
    const QString quick = controller.quickExport(QStringLiteral("audio-wav"), q(dir / "quick.wav"),
                                                 {{QStringLiteral("range"), QStringLiteral("whole")}});
    REQUIRE_FALSE(quick.isEmpty());
    CHECK(controller.job(quick).value(QStringLiteral("origin")).toString() == QStringLiteral("quick"));
    CHECK_FALSE(controller.openOutput(quick));
    REQUIRE(finished(controller, quick));
    const QVariantMap info = controller.job(quick);
    CHECK(info.value(QStringLiteral("jobId")).toString() == quick);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("done"));
    CHECK(info.value(QStringLiteral("title")).toString() == QStringLiteral("quick.wav"));
    CHECK(samePath(info.value(QStringLiteral("outputPath")).toString(), q(dir / "quick.wav")));
    CHECK(samePath(info.value(QStringLiteral("outputUrl")).toUrl().toLocalFile(), q(dir / "quick.wav")));
    CHECK(samePath(info.value(QStringLiteral("folderUrl")).toUrl().toLocalFile(), q(dir)));
    CHECK(info.value(QStringLiteral("exists")).toBool());
    CHECK(info.value(QStringLiteral("finished")).toBool());
    CHECK_FALSE(info.value(QStringLiteral("presetName")).toString().isEmpty());
    CHECK(finishedJobs.contains(quick));
    CHECK(controller.openOutput(quick));
    REQUIRE(opened.size() == 1);
    CHECK(samePath(opened.front(), q(dir / "quick.wav")));
    // Quick Export's chosen file also sets the folder.
    CHECK(samePath(controller.lastFolder(), q(dir)));

    // The other ways in: the Export workspace ("export") and Send to Media Encoder ("queue", held).
    const QString exported = controller.queueExport({}, QStringLiteral("audio-wav"),
                                                    {{QStringLiteral("output"), q(dir / "mix.wav")}});
    REQUIRE_FALSE(exported.isEmpty());
    CHECK(controller.job(exported).value(QStringLiteral("origin")).toString() == QStringLiteral("export"));
    const QStringList held = controller.sendToQueue({}, {QStringLiteral("audio-wav")},
                                                    {{QStringLiteral("output"), q(dir / "held.wav")}}, true);
    REQUIRE(held.size() == 1);
    CHECK(controller.job(held.front()).value(QStringLiteral("origin")).toString() == QStringLiteral("queue"));
    CHECK(controller.job(held.front()).value(QStringLiteral("state")).toString() == QStringLiteral("held"));
    // The model has the role too.
    CHECK(controller.jobs()->roleNames().value(ExportJobModel::OriginRole) == QByteArray("origin"));
    // A cancelled job does not open.
    controller.cancel(held.front());
    REQUIRE(finished(controller, held.front()));
    CHECK(controller.job(held.front()).value(QStringLiteral("state")).toString() == QStringLiteral("cancelled"));
    CHECK_FALSE(controller.openOutput(held.front()));
    REQUIRE(finished(controller, exported));
    // A file deleted after the export does not open either.
    bool removed = false;
    for (int attempt = 0; attempt < 50 && !removed; ++attempt) {
        removed = QFile::remove(q(dir / "mix.wav")) || !QFile::exists(q(dir / "mix.wav"));
        if (!removed)
            spin(100); // a scanner may hold the new file for a moment
    }
    REQUIRE(removed);
    CHECK_FALSE(controller.job(exported).value(QStringLiteral("exists")).toBool());
    CHECK_FALSE(controller.openOutput(exported));
    CHECK(opened.size() == 1);
    // Unknown ids (e.g. after Clear Finished).
    CHECK(controller.job(QStringLiteral("j999")).isEmpty());
    CHECK_FALSE(controller.openOutput(QStringLiteral("j999")));
    controller.clearFinished();
    CHECK(controller.job(quick).isEmpty());
}

// ---- Phase 2: the QML ------------------------------------------------------------------------------------------------

TEST_CASE("Settings search: a result shows its row and every indexed row is on its page", "[app][settings][qml][ux]") {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    auto* appSettings = shell.engine.singletonInstance<AppSettingsBridge*>("Wabcut", "AppSettings");
    REQUIRE(appSettings != nullptr);
    const auto focusedIs = [&](const char* name) {
        QQuickItem* f = w->activeFocusItem();
        return f != nullptr && f->objectName() == QLatin1String(name);
    };

    QObject* window = settingsWindow(shell);
    REQUIRE(QMetaObject::invokeMethod(window, "openAt", Q_ARG(QString, QString())));
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    // The search field has the focus when the window opens: type to find.
    QQuickItem* search = inSettings(window, QStringLiteral("settingsSearch"));
    REQUIRE(search != nullptr);
    CHECK(waitFor([&] { return search->hasActiveFocus(); }, 3000));

    // Typing lists the matches instead of the categories, best first.
    QQuickItem* general = inSettings(window, QStringLiteral("settingsCategory_general"));
    REQUIRE(general != nullptr);
    CHECK(general->isVisible());
    window->setProperty("searchText", QStringLiteral("default folder"));
    const QVariantList results = fromJs(window->property("results")).toList();
    REQUIRE_FALSE(results.isEmpty());
    CHECK(results.front().toMap().value(QStringLiteral("id")).toString() == QStringLiteral("projectLocation"));
    CHECK_FALSE(general->isVisible());
    QQuickItem* hit = nullptr;
    REQUIRE(waitFor([&] {
        hit = inSettings(window, QStringLiteral("settingsResult_projectLocation"));
        return hit != nullptr && hit->isVisible();
    }));

    // Choosing it from another page: General shows, the row flashes, and its control has the keyboard focus.
    REQUIRE(QMetaObject::invokeMethod(window, "showPage", Q_ARG(QString, QStringLiteral("appearance"))));
    REQUIRE(QMetaObject::invokeMethod(hit, "clicked"));
    CHECK(window->property("currentPage").toString() == QStringLiteral("general"));
    QQuickItem* row = nullptr;
    REQUIRE(waitFor([&] { return (row = inSettings(window, QStringLiteral("projectLocationRow"))) != nullptr; }));
    QQuickItem* flash = flashOf(row);
    REQUIRE(flash != nullptr);
    CHECK(waitFor([&] { return flash->opacity() > 0.5; }, 3000));
    CHECK(waitFor([&] { return focusedIs("projectLocationBrowse"); }, 3000));

    // Every control in the sidebar (the search, the results) and on the General page is named for a screen reader.
    CHECK(auditAccessibility(search->parentItem()).isEmpty());
    INFO(auditAccessibility(row->parentItem()->parentItem()).join(QStringLiteral("; ")).toStdString());
    CHECK(auditAccessibility(row->parentItem()->parentItem()).isEmpty());
    CHECK(tabReaches(search->parentItem(), QStringLiteral("settingsResult_projectLocation")));

    // Screenshot: the results beside the row they found.
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("settings-search.png"))));

    // A row that opens a dialog of its own says so; nothing found says so.
    window->setProperty("searchText", QStringLiteral("proxies"));
    CHECK(fromJs(window->property("results")).toList().value(0).toMap().value(QStringLiteral("opens")).toString()
          == QStringLiteral("media"));
    window->setProperty("searchText", QStringLiteral("qqxxzz"));
    QQuickItem* none = inSettings(window, QStringLiteral("settingsNoResults"));
    REQUIRE(none != nullptr);
    CHECK(none->isVisible());

    // Enter in the field shows the best match: "dark" is Appearance's Dark or light mode.
    window->setProperty("searchText", QStringLiteral("dark"));
    search->forceActiveFocus(Qt::TabFocusReason);
    tapKey(w, Qt::Key_Return);
    CHECK(waitFor([&] { return window->property("currentPage").toString() == QStringLiteral("appearance"); }, 3000));
    QQuickItem* modeRow = findWith(window->property("contentItem").value<QQuickItem*>(),
                                   [](QQuickItem* i) { return i->property("rowKey").toString() == QStringLiteral("mode"); });
    REQUIRE(modeRow != nullptr);
    CHECK(waitFor([&] { return flashOf(modeRow) != nullptr && flashOf(modeRow)->opacity() > 0.5; }, 3000));
    // Esc in the field clears the search (the window stays): the categories come back.
    search->forceActiveFocus(Qt::TabFocusReason);
    tapKey(w, Qt::Key_Escape);
    CHECK(window->property("searchText").toString().isEmpty());
    CHECK(window->property("opened").toBool());
    CHECK(general->isVisible());

    // Every row the search knows is on its page (rowKey) and can be revealed.
    for (const QVariant& v : appSettings->index()) {
        const QVariantMap entry = v.toMap();
        const QString page = entry.value(QStringLiteral("page")).toString();
        const QString rowKey = entry.value(QStringLiteral("row")).toString();
        INFO(page.toStdString() << " / " << rowKey.toStdString());
        REQUIRE(QMetaObject::invokeMethod(window, "showPage", Q_ARG(QString, page)));
        bool revealed = false;
        REQUIRE(QMetaObject::invokeMethod(window, "revealRow", Q_RETURN_ARG(bool, revealed), Q_ARG(QString, page),
                                          Q_ARG(QString, rowKey)));
        CHECK(revealed);
    }

    // From outside (AppSettings.openRequested: the palette's Settings rows): the window opens on the row, the search
    // cleared, the row's control focused.
    window->setProperty("searchText", QStringLiteral("lock"));
    REQUIRE(QMetaObject::invokeMethod(window, "close"));
    REQUIRE(waitFor([&] { return !window->property("visible").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(window, "revealSetting", Q_ARG(QString, QStringLiteral("general")),
                                      Q_ARG(QString, QStringLiteral("lockProjects"))));
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    CHECK(window->property("currentPage").toString() == QStringLiteral("general"));
    CHECK(window->property("searchText").toString().isEmpty());
    CHECK(waitFor([&] { return focusedIs("settingsLockProjects"); }, 3000));

    // The command palette's Settings rows: AppSettings.openSetting -> openRequested -> Main's openSettings(page, row).
    REQUIRE(QMetaObject::invokeMethod(window, "close"));
    REQUIRE(waitFor([&] { return !window->property("visible").toBool(); }));
    REQUIRE(appSettings->openSetting(QStringLiteral("highContrast")));
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    CHECK(window->property("currentPage").toString() == QStringLiteral("appearance"));
    QQuickItem* contrastRow = findWith(window->property("contentItem").value<QQuickItem*>(), [](QQuickItem* i) {
        return i->property("rowKey").toString() == QStringLiteral("highContrast");
    });
    REQUIRE(contrastRow != nullptr);
    CHECK(waitFor([&] { return flashOf(contrastRow) != nullptr && flashOf(contrastRow)->opacity() > 0.5; }, 3000));
    CHECK(waitFor([&] { return focusedIs("highContrastToggle"); }, 3000));
    REQUIRE(QMetaObject::invokeMethod(window, "close"));
    spin(100);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

TEST_CASE("Settings General: the start-up choice and the Projects rows", "[app][settings][qml][ux]") {
    resetSettings();
    clearMessages();
    const fs::path folder = freshDir("default-location");
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        auto* appSettings = shell.engine.singletonInstance<AppSettingsBridge*>("Wabcut", "AppSettings");
        auto* home = shell.engine.singletonInstance<HomeBridge*>("Wabcut", "Home");
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        REQUIRE(appSettings != nullptr);
        REQUIRE(home != nullptr);
        REQUIRE(project != nullptr);
        // The light theme (the screenshot below shows the new rows on it).
        shell.setAppearance(false, "ultraviolet");
        REQUIRE(shell.settle(false, shell.themeColor("accentFill")));

        QObject* window = settingsWindow(shell);
        REQUIRE(QMetaObject::invokeMethod(window, "openAt", Q_ARG(QString, QStringLiteral("general"))));
        REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
        spin(50); // the search field takes the focus once the window has opened

        // Start-up: Show Home (the default) or Reopen the last project.
        QQuickItem* launch = inSettings(window, QStringLiteral("launchActionTabs"));
        REQUIRE(launch != nullptr);
        CHECK(launch->property("currentIndex").toInt() == 0);
        REQUIRE(QMetaObject::invokeMethod(launch, "activated", Q_ARG(int, 1)));
        CHECK(appSettings->launchAction() == QStringLiteral("reopen"));
        CHECK(launch->property("currentIndex").toInt() == 1);
        appSettings->setLaunchAction(QStringLiteral("home")); // the row follows a change made elsewhere
        CHECK(launch->property("currentIndex").toInt() == 0);

        // Recent projects to keep (Home.recentLimit; 50 by default).
        QQuickItem* recent = inSettings(window, QStringLiteral("recentProjectsTabs"));
        REQUIRE(recent != nullptr);
        CHECK(recent->property("currentIndex").toInt() == 2);
        REQUIRE(QMetaObject::invokeMethod(recent, "activated", Q_ARG(int, 0)));
        CHECK(home->recentLimit() == 10);
        CHECK(recent->property("currentIndex").toInt() == 0);

        // Project locking: ProjectFile.lockProjects, the switch Preferences > Media and Timeline has too.
        QQuickItem* lock = inSettings(window, QStringLiteral("settingsLockProjects"));
        REQUIRE(lock != nullptr);
        const bool locked = project->lockProjects();
        CHECK(lock->property("checked").toBool() == locked);
        REQUIRE(QMetaObject::invokeMethod(lock, "toggle"));
        REQUIRE(QMetaObject::invokeMethod(lock, "toggled"));
        CHECK(project->lockProjects() == !locked);
        project->setLockProjects(locked);
        CHECK(lock->property("checked").toBool() == locked);

        // Default location for new projects: automatic until a folder is chosen; "Automatic" goes back.
        QQuickItem* location = inSettings(window, QStringLiteral("projectLocationRow"));
        QQuickItem* automatic = inSettings(window, QStringLiteral("projectLocationAutomatic"));
        REQUIRE(location != nullptr);
        REQUIRE(automatic != nullptr);
        CHECK(location->property("description").toString().startsWith(QStringLiteral("Automatic")));
        CHECK_FALSE(automatic->isVisible());
        home->setDefaultFolderSetting(QUrl::fromLocalFile(q(folder)));
        CHECK(samePath(location->property("description").toString(), q(folder)));
        CHECK(automatic->isVisible());
        CHECK(samePath(home->defaultFolder().toLocalFile(), q(folder))); // Home's New project page starts there

        // Screenshot: the Projects section in view, the location row flashed.
        bool revealed = false;
        REQUIRE(QMetaObject::invokeMethod(window, "revealRow", Q_RETURN_ARG(bool, revealed),
                                          Q_ARG(QString, QStringLiteral("general")),
                                          Q_ARG(QString, QStringLiteral("projectLocation"))));
        CHECK(revealed);
        QQuickItem* focused = shell.window->activeFocusItem();
        INFO("focus: " << (focused ? focused->objectName().toStdString() : std::string("none")));
        CHECK((focused != nullptr && focused->objectName() == QStringLiteral("projectLocationAutomatic")));
        spin(100);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("settings-general-light.png"))));
        // A raised sheet: a hairline ring and a soft shadow (the light theme's white window on the light editor).
        QQuickItem* sheet = window->property("background").value<QQuickItem*>();
        REQUIRE(sheet != nullptr);
        QQuickItem* shadow = named(sheet, QStringLiteral("settingsWindowShadow"));
        REQUIRE(shadow != nullptr);
        CHECK(shadow->isVisible());
        CHECK(shadow->property("color").value<QColor>().alpha() > 0);
        CHECK(sheet->property("border").value<QObject*>()->property("width").toDouble() > 0);

        REQUIRE(QMetaObject::invokeMethod(automatic, "clicked"));
        CHECK(home->defaultFolderSetting().isEmpty());
        CHECK_FALSE(automatic->isVisible());
        home->setRecentLimit(50);

        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
    resetSettings();
}

TEST_CASE("Recent projects to show: a lower number hides the older ones and a higher one lists them again", "[app][settings][home][ux]") {
    resetSettings();
    const fs::path dir = freshDir("recent-limit");
    {
        ProjectController controller;
        ProjectLifecycle lifecycle{&controller};
        HomeBridge home;
        home.setController(&controller);
        home.setLifecycle(&lifecycle);
        home.refreshRecent();
        const auto project = [&](int i) { return QUrl::fromLocalFile(q(dir / ("P" + std::to_string(i) + ".wabcut"))); };
        const auto nameAt = [&](qsizetype at) { return home.recent().at(at).toMap().value(QStringLiteral("name")).toString(); };
        for (int i = 0; i < 30; ++i)
            home.rememberProject(project(i));
        REQUIRE(home.recent().size() == 30);
        CHECK(nameAt(0) == QStringLiteral("P29"));

        // 10: the newest ten are listed (Home, File > Open Recent); nothing is forgotten.
        home.setRecentLimit(10);
        REQUIRE(home.recent().size() == 10);
        CHECK(nameAt(9) == QStringLiteral("P20"));
        home.setRecentLimit(50);
        REQUIRE(home.recent().size() == 30);
        CHECK(nameAt(29) == QStringLiteral("P0"));

        // A project opened while the number is low is listed first; the older ones still come back.
        home.setRecentLimit(10);
        home.rememberProject(project(30));
        REQUIRE(home.recent().size() == 10);
        CHECK(nameAt(0) == QStringLiteral("P30"));
        home.setRecentLimit(50);
        REQUIRE(home.recent().size() == 31);
        CHECK(nameAt(30) == QStringLiteral("P0"));
    }
    resetSettings();
}

TEST_CASE("Library: the thumbnail slider sizes the cards and the selection keys move in the grid", "[app][library][qml][ux]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(library != nullptr);
    library->setPersistent(false);
    library->setView(QStringLiteral("grid"));
    QQuickItem* content = w->contentItem();
    QQuickItem* panel = shell.item("libraryPanel");
    QQuickItem* slider = named(content, QStringLiteral("libraryThumbnailSlider"));
    QQuickItem* grid = named(content, QStringLiteral("libraryGrid"));
    QQuickItem* zoomIn = named(content, QStringLiteral("libraryZoomIn"));
    QQuickItem* zoomOut = named(content, QStringLiteral("libraryZoomOut"));
    REQUIRE(panel != nullptr);
    REQUIRE(slider != nullptr);
    REQUIRE(grid != nullptr);
    REQUIRE(zoomIn != nullptr);
    REQUIRE(zoomOut != nullptr);
    spin(50);
    CHECK(slider->isVisible());
    CHECK(slider->property("from").toDouble() == 0.5);
    CHECK(slider->property("to").toDouble() == 2.0);
    CHECK(slider->property("stepSize").toDouble() == 0.25);
    CHECK(slider->property("value").toDouble() == 1.0);
    // Named and reached with Tab (the buttons and the slider).
    QQuickItem* zoomRow = named(content, QStringLiteral("libraryZoomRow"));
    REQUIRE(zoomRow != nullptr);
    INFO(auditAccessibility(zoomRow).join(QStringLiteral("; ")).toStdString());
    CHECK(auditAccessibility(zoomRow).isEmpty());
    CHECK(tabReaches(panel, QStringLiteral("libraryThumbnailSlider")));

    // Dragging the handle (a value, then moved) sizes the cards.
    const double cellBefore = grid->property("cellWidth").toDouble();
    slider->setProperty("value", 1.5);
    REQUIRE(QMetaObject::invokeMethod(slider, "moved"));
    CHECK(library->thumbnailScale() == 1.5);
    CHECK(grid->property("cellWidth").toDouble() > cellBefore);
    // The buttons step it; at a bound the button is off, and the slider follows changes made elsewhere.
    REQUIRE(QMetaObject::invokeMethod(zoomIn, "clicked"));
    CHECK(library->thumbnailScale() == 1.75);
    REQUIRE(QMetaObject::invokeMethod(zoomOut, "clicked"));
    CHECK(library->thumbnailScale() == 1.5);
    library->setThumbnailScale(2.0);
    CHECK(slider->property("value").toDouble() == 2.0);
    CHECK_FALSE(zoomIn->isEnabled());
    CHECK(zoomOut->isEnabled());

    // The keyboard: the focused slider takes the arrows (they are the Library's selection keys elsewhere).
    library->setThumbnailScale(1.0);
    slider->forceActiveFocus(Qt::TabFocusReason);
    const QStringList selectionBefore = library->selection();
    tapKey(w, Qt::Key_Right);
    CHECK(library->thumbnailScale() == 1.25);
    CHECK(library->selection() == selectionBefore);

    // The Project panel's selection keys: End, Home, Shift+Right, Page Down (the panel answers in the grid).
    const int count = library->items()->rowCount();
    REQUIRE(count >= 2);
    REQUIRE(library->runCommand(QStringLiteral("project.moveSelectionEnd")));
    CHECK(library->selection() == QStringList{library->items()->keyOf(count - 1)});
    REQUIRE(library->runCommand(QStringLiteral("project.moveSelectionHome")));
    CHECK(library->selection() == QStringList{library->items()->keyOf(0)});
    REQUIRE(library->runCommand(QStringLiteral("project.extendSelectionRight")));
    CHECK(library->selection().size() == 2);
    REQUIRE(library->runCommand(QStringLiteral("project.moveSelectionPageDown")));
    CHECK(library->selection().size() == 1);
    // The list view moves by rows (and has no card size: the slider row hides).
    library->setView(QStringLiteral("list"));
    spin(50);
    CHECK_FALSE(slider->isVisible());
    REQUIRE(library->runCommand(QStringLiteral("project.moveSelectionEnd")));
    CHECK(library->selection() == QStringList{library->items()->keyOf(count - 1)});
    REQUIRE(library->runCommand(QStringLiteral("project.moveSelectionUp")));
    CHECK(library->selection() == QStringList{library->items()->keyOf(count - 2)});
    library->setView(QStringLiteral("grid"));
    spin(50);
    CHECK(slider->isVisible());

    // Screenshot: the cards at 150 %, the slider under them.
    library->setThumbnailScale(1.5);
    REQUIRE(library->runCommand(QStringLiteral("project.moveSelectionHome")));
    spin(100);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height())),
             QStringLiteral("library-thumbnail-slider.png"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

TEST_CASE("Library selection keys: Shift grows and shrinks the range and hidden items never move", "[app][library][qml][ux]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(library != nullptr);
    library->setPersistent(false);
    QQuickItem* content = w->contentItem();
    QQuickItem* panel = shell.item("libraryPanel");
    REQUIRE(panel != nullptr);
    const int count = library->items()->rowCount();
    REQUIRE(count >= 4);
    const auto rows = [&](int first, int last) {
        QStringList keys;
        for (int row = first; row <= last; ++row)
            keys << library->items()->keyOf(row);
        return keys;
    };

    // The list's own keys: Shift+Up grows the range from the anchor row by row, Shift+Down shrinks it back past the
    // anchor and on (the moving end is the row the keys went to, not the last selected row).
    library->setView(QStringLiteral("list"));
    spin(50);
    library->select(library->items()->keyOf(2), LibraryBridge::Replace);
    QQuickItem* list = named(content, QStringLiteral("libraryList"));
    REQUIRE(list != nullptr);
    list->forceActiveFocus(Qt::TabFocusReason);
    tapKey(w, Qt::Key_Up, Qt::ShiftModifier);
    CHECK(library->selection() == rows(1, 2));
    tapKey(w, Qt::Key_Up, Qt::ShiftModifier);
    CHECK(library->selection() == rows(0, 2));
    tapKey(w, Qt::Key_Down, Qt::ShiftModifier);
    CHECK(library->selection() == rows(1, 2));
    tapKey(w, Qt::Key_Down, Qt::ShiftModifier);
    CHECK(library->selection() == rows(2, 2));
    tapKey(w, Qt::Key_Down, Qt::ShiftModifier);
    CHECK(library->selection() == rows(2, 3));

    // The grid with the Project panel's commands (as the keymap sends them): the same, and a selection made another
    // way (a click) starts over from it.
    library->setView(QStringLiteral("grid"));
    spin(50);
    library->select(library->items()->keyOf(2), LibraryBridge::Replace);
    REQUIRE(library->runCommand(QStringLiteral("project.extendSelectionLeft")));
    CHECK(library->selection() == rows(1, 2));
    REQUIRE(library->runCommand(QStringLiteral("project.extendSelectionLeft")));
    CHECK(library->selection() == rows(0, 2));
    REQUIRE(library->runCommand(QStringLiteral("project.extendSelectionRight")));
    CHECK(library->selection() == rows(1, 2));
    library->select(library->items()->keyOf(3), LibraryBridge::Replace);
    REQUIRE(library->runCommand(QStringLiteral("project.extendSelectionLeft")));
    CHECK(library->selection() == rows(2, 3));

    // The Text tab hides the items: the selection keys leave the hidden selection alone (Delete would act on it), and
    // a focused Text tab slider gets its arrows instead of the Library's keys (the card size stays too).
    QQuickItem* tabs = named(content, QStringLiteral("libraryTabs"));
    REQUIRE(tabs != nullptr);
    tabs->setProperty("currentIndex", 2);
    spin(50);
    const QStringList before = library->selection();
    for (const char* id : {"project.moveSelectionEnd", "project.moveSelectionHome", "project.moveSelectionDown",
                           "project.extendSelectionUp", "project.moveSelectionPageDown"}) {
        INFO(id);
        library->runCommand(QString::fromLatin1(id));
        CHECK(library->selection() == before);
    }
    QQuickItem* slider = findWith(panel, [](QQuickItem* i) { return i->inherits("QQuickSlider") && i->isVisible(); });
    REQUIRE(slider != nullptr);
    CHECK(slider->objectName() != QStringLiteral("libraryThumbnailSlider"));
    slider->forceActiveFocus(Qt::TabFocusReason);
    const double value = slider->property("value").toDouble();
    const double scale = library->thumbnailScale();
    tapKey(w, Qt::Key_Right);
    CHECK(slider->property("value").toDouble() > value);
    tapKey(w, Qt::Key_End);
    tapKey(w, Qt::Key_Equal);
    CHECK(library->selection() == before);
    CHECK(library->thumbnailScale() == scale);

    // Back on the Media tab the keys move the selection again.
    tabs->setProperty("currentIndex", 0);
    spin(50);
    REQUIRE(library->runCommand(QStringLiteral("project.moveSelectionHome")));
    CHECK(library->selection() == rows(0, 0));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

TEST_CASE("Export memory: the Export panel and Quick Export start from the last choices", "[app][export][qml][ux]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("export-panel-memory");
    fs::create_directories(dir / "Renders");
    const auto earlierChoices = [&] {
        ExportController earlier;
        earlier.setLastPreset(QStringLiteral("youtube-1080p"));
        earlier.setLastQuickPreset(QStringLiteral("audio-wav"));
        earlier.setLastFolder(q(dir / "Renders"));
        earlier.setLastRange(QStringLiteral("inOut"));
        REQUIRE(earlier.lastPreset() == QStringLiteral("youtube-1080p"));
        REQUIRE(samePath(earlier.lastFolder(), q(dir / "Renders")));
    };
    earlierChoices();
    {
        A11yScope a11y;
        Shell shell;
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        QQuickWindow* w = shell.window.get();
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
        REQUIRE(controller != nullptr);
        REQUIRE(exporter != nullptr);
        controller->resetProject(makeProject(4, true)); // In and Out marks
        QQuickItem* content = w->contentItem();

        // Quick Export: the preset picked last, the range picked last (the sequence has marks).
        QObject* quick = shell.object("quickExportButton");
        REQUIRE(quick != nullptr);
        CHECK(quick->property("presetId").toString() == QStringLiteral("audio-wav"));
        REQUIRE(QMetaObject::invokeMethod(quick, "clicked"));
        REQUIRE(waitFor([&] { return quick->property("checked").toBool(); }));
        CHECK(quick->property("range").toString() == QStringLiteral("inOut"));
        QQuickItem* quickRange = nullptr;
        QQuickItem* quickGo = nullptr;
        REQUIRE(waitFor([&] {
            quickRange = named(content, QStringLiteral("quickExportRange"));
            quickGo = named(content, QStringLiteral("quickExportGo"));
            return quickRange != nullptr && quickGo != nullptr && quickRange->isVisible();
        }));
        CHECK(quickGo->property("text").toString() == QStringLiteral("Export In to Out"));
        INFO(auditAccessibility(quickGo->parentItem()).join(QStringLiteral("; ")).toStdString());
        CHECK(auditAccessibility(quickGo->parentItem()).isEmpty());
        // The preset list has the keyboard (its current row, inside the list): Down picks the next preset, and that is
        // remembered.
        CHECK(waitFor([&] {
            for (QQuickItem* f = w->activeFocusItem(); f != nullptr; f = f->parentItem()) {
                if (f->objectName() == QStringLiteral("quickExportPresets"))
                    return true;
            }
            return false;
        }, 3000));
        // Its focus ring waits for a key (the pointer opened it), then shows on the row the keys picked.
        QQuickItem* presets = named(content, QStringLiteral("quickExportPresets"));
        REQUIRE(presets != nullptr);
        CHECK_FALSE(presets->property("keyboardUsed").toBool());
        tapKey(w, Qt::Key_Down);
        CHECK(quick->property("presetId").toString() != QStringLiteral("audio-wav"));
        CHECK(exporter->lastQuickPreset() == quick->property("presetId").toString());
        CHECK(presets->property("keyboardUsed").toBool());
        QQuickItem* pickedRow = named(content, QStringLiteral("quickExportPreset_") + quick->property("presetId").toString());
        REQUIRE(pickedRow != nullptr);
        QQuickItem* ring = findWith(pickedRow, [](QQuickItem* i) {
            return QString::fromLatin1(i->metaObject()->className()).startsWith(QLatin1String("FocusRing"));
        });
        REQUIRE(ring != nullptr);
        CHECK(ring->isVisible());
        // The whole sequence, picked here, is remembered too.
        REQUIRE(QMetaObject::invokeMethod(quickRange, "activated", Q_ARG(int, 0)));
        CHECK(quick->property("range").toString() == QStringLiteral("whole"));
        CHECK(exporter->lastRange() == QStringLiteral("whole"));
        CHECK(quickGo->property("text").toString() == QStringLiteral("Export"));
        REQUIRE(QMetaObject::invokeMethod(quick, "clicked")); // closes
        REQUIRE(waitFor([&] { return !quick->property("checked").toBool(); }));
        exporter->setLastRange(QStringLiteral("inOut"));

        // The Export workspace: the preset, the range (the button says so) and the folder picked last.
        shell.window->setProperty("workspace", 2);
        QQuickItem* panel = nullptr;
        REQUIRE(waitFor([&] {
            panel = findWith(content, [](QQuickItem* i) {
                return i->property("rangeChosen").isValid() && i->property("gamingPreset").isValid();
            });
            return panel != nullptr && panel->isVisible();
        }));
        CHECK(panel->property("presetId").toString() == QStringLiteral("youtube-1080p"));
        CHECK(panel->property("gamingPreset").toString().isEmpty());
        CHECK(panel->property("range").toString() == QStringLiteral("inOut"));
        QQuickItem* tile = named(content, QStringLiteral("exportPreset_youtube-1080p"));
        QQuickItem* button = named(content, QStringLiteral("exportButton"));
        QQuickItem* output = named(content, QStringLiteral("exportOutputField"));
        QQuickItem* reset = named(content, QStringLiteral("exportResetChoices"));
        REQUIRE(tile != nullptr);
        REQUIRE(button != nullptr);
        REQUIRE(output != nullptr);
        REQUIRE(reset != nullptr);
        CHECK(tile->property("selected").toBool());
        CHECK(button->property("text").toString() == QStringLiteral("Export In to Out"));
        REQUIRE(waitFor([&] { return !output->property("text").toString().isEmpty(); }));
        CHECK(samePath(QFileInfo(output->property("text").toString()).absolutePath(), q(dir / "Renders")));
        CHECK(reset->isVisible());

        // Screenshot: the panel as it comes back, scrolled to its "Save to" card (a narrow slot stacks it under the
        // tiles).
        QQuickItem* scroll = named(content, QStringLiteral("exportPresetScroll"));
        REQUIRE(scroll != nullptr);
        spin(300); // the estimates and notes settle the card heights first
        // A hidden window lays its layouts out only when it renders: polish them now (inner ones first), so the
        // content height is the real one.
        const std::function<void(QQuickItem*)> polish = [&](QQuickItem* item) {
            for (QQuickItem* child : item->childItems())
                polish(child);
            item->ensurePolished();
        };
        polish(panel);
        polish(panel);
        scroll->setProperty("contentY", std::max(0.0, scroll->property("contentHeight").toDouble() - scroll->height()));
        spin(100);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        saveCrop(image, panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height())),
                 QStringLiteral("export-memory.png"));

        // Picks are remembered: a preset (the range stays: it is the sequence's), a gaming preset, a range.
        REQUIRE(QMetaObject::invokeMethod(panel, "choosePreset", Q_ARG(QVariant, QVariant(QStringLiteral("match-source")))));
        CHECK(exporter->lastPreset() == QStringLiteral("match-source"));
        CHECK(panel->property("range").toString() == QStringLiteral("inOut"));
        QQuickItem* gaming = named(content, QStringLiteral("gamingPreset_youtube-1080p60"));
        REQUIRE(gaming != nullptr);
        REQUIRE(QMetaObject::invokeMethod(gaming, "clicked"));
        CHECK(exporter->lastGamingPreset() == QStringLiteral("youtube-1080p60"));
        QQuickItem* rangeTabs = named(content, QStringLiteral("exportRangeTabs"));
        REQUIRE(rangeTabs != nullptr);
        REQUIRE(QMetaObject::invokeMethod(rangeTabs, "activated", Q_ARG(int, 0)));
        CHECK(exporter->lastRange() == QStringLiteral("whole"));
        CHECK(panel->property("range").toString() == QStringLiteral("whole"));
        CHECK(button->property("text").toString() == QStringLiteral("Export"));

        // Reset to defaults: everything forgotten; the marked sequence suggests In to Out again. Quick Export forgets its
        // preset too, and the keyboard focus goes on to Browse… (the button hides).
        REQUIRE(quick->property("presetId").toString() != QStringLiteral("match-source"));
        reset->forceActiveFocus(Qt::TabFocusReason);
        REQUIRE(reset->hasActiveFocus());
        REQUIRE(QMetaObject::invokeMethod(reset, "clicked"));
        QQuickItem* focusedAfterReset = w->activeFocusItem();
        CHECK((focusedAfterReset != nullptr && focusedAfterReset->objectName() == QStringLiteral("exportBrowse")));
        CHECK(quick->property("presetId").toString() == QStringLiteral("match-source"));
        CHECK(exporter->lastPreset() == QStringLiteral("match-source"));
        CHECK(exporter->lastGamingPreset().isEmpty());
        CHECK(exporter->lastQuickPreset() == QStringLiteral("match-source"));
        CHECK(exporter->lastFolder().isEmpty());
        CHECK(exporter->lastRange().isEmpty());
        CHECK(panel->property("presetId").toString() == QStringLiteral("match-source"));
        CHECK(panel->property("gamingPreset").toString().isEmpty());
        CHECK(panel->property("range").toString() == QStringLiteral("inOut"));
        CHECK_FALSE(reset->isVisible());

        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }

    // Quick Export's popover with a marked sequence (one grab per window: another shell).
    earlierChoices();
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller != nullptr);
        controller->resetProject(makeProject(4, true));
        QObject* quick = shell.object("quickExportButton");
        REQUIRE(quick != nullptr);
        REQUIRE(QMetaObject::invokeMethod(quick, "clicked"));
        REQUIRE(waitFor([&] { return quick->property("checked").toBool(); }));
        spin(300);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("quick-export-range.png"))));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
    resetSettings();
}

TEST_CASE("Background work: an export or Render In to Out that ends says so in a toast with an action", "[app][export][qml][ux]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("export-toasts");
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    QObject* readiness = shell.engine.singletonInstance<QObject*>("Wabcut", "MediaReadiness");
    REQUIRE(controller != nullptr);
    REQUIRE(exporter != nullptr);
    REQUIRE(readiness != nullptr);
    controller->resetProject(makeProject(2));
    exporter->setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    QStringList revealed;
    exporter->setRevealHandler([&](const QString& path) {
        revealed << path;
        return true;
    });
    auto* toast = qobject_cast<QQuickItem*>(shell.object("toast"));
    REQUIRE(toast != nullptr);

    // Quick Export says it started (Cancel), then that it is done ("Show in Explorer" shows the file).
    const QString job = exporter->quickExport(QStringLiteral("audio-wav"), q(dir / "done.wav"));
    REQUIRE_FALSE(job.isEmpty());
    CHECK(toast->property("text").toString() == QStringLiteral("Exporting done.wav in the background"));
    CHECK(toast->property("actionText").toString() == QStringLiteral("Cancel"));
    REQUIRE(finished(*exporter, job));
    REQUIRE(waitFor([&] { return toast->property("text").toString() == QStringLiteral("Exported done.wav"); }));
    CHECK(toast->property("actionText").toString() == QStringLiteral("Show in %1").arg(exporter->fileManagerName()));
    spin(300);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const QRectF toastRect = toast->mapRectToScene(QRectF(0, 0, toast->width(), toast->height()));
    saveCrop(image, toastRect.adjusted(-24, -24, 24, 24).intersected(QRectF(0, 0, kShotWidth, kShotHeight)),
             QStringLiteral("toast-export-done.png"));
    REQUIRE(QMetaObject::invokeMethod(toast, "runAction"));
    REQUIRE(revealed.size() == 1);
    CHECK(samePath(revealed.front(), q(dir / "done.wav")));
    CHECK_FALSE(toast->property("shown").toBool());

    // Its job row cleared meanwhile (Clear Finished): the action still shows the file it wrote; a file that is gone
    // too is said so.
    const auto exportAndClear = [&](const char* name) {
        const QString id = exporter->quickExport(QStringLiteral("audio-wav"), q(dir / name));
        REQUIRE_FALSE(id.isEmpty());
        REQUIRE(finished(*exporter, id));
        REQUIRE(waitFor([&] { return toast->property("text").toString() == QStringLiteral("Exported %1").arg(QLatin1String(name)); }));
        exporter->clearFinished();
        REQUIRE(exporter->job(id).isEmpty());
    };
    exportAndClear("kept.wav");
    REQUIRE(QMetaObject::invokeMethod(toast, "runAction"));
    REQUIRE(revealed.size() == 2);
    CHECK(samePath(revealed.back(), q(dir / "kept.wav")));
    exportAndClear("gone.wav");
    std::error_code removed;
    REQUIRE(fs::remove(dir / "gone.wav", removed));
    REQUIRE(QMetaObject::invokeMethod(toast, "runAction"));
    CHECK(revealed.size() == 2);
    CHECK(toast->property("text").toString() == QStringLiteral("gone.wav is no longer there: it was moved or deleted."));
    REQUIRE(QMetaObject::invokeMethod(toast, "dismiss"));

    // A failed export says why and offers the queue: the Export workspace shows it (the user's Edit layout gets no
    // Export panel docked into it). A cancelled one says nothing.
    const int editWorkspace = evaluate(shell, QStringLiteral("window.workspace")).toInt();
    const QString editHasExport = QStringLiteral("Workspaces.groupsOf(Workspaces.find(%1).layout).some(g => g.panels.includes('export'))")
                                      .arg(editWorkspace);
    REQUIRE_FALSE(evaluate(shell, editHasExport).toBool());
    REQUIRE_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('export')")).toBool());
    REQUIRE(QMetaObject::invokeMethod(exporter, "jobFinished", Q_ARG(QString, QStringLiteral("j999")), Q_ARG(bool, false),
                                      Q_ARG(QString, QStringLiteral("The disk is full."))));
    CHECK(toast->property("text").toString() == QStringLiteral("Export failed: The disk is full."));
    CHECK(toast->property("actionText").toString() == QStringLiteral("Show the queue"));
    REQUIRE(QMetaObject::invokeMethod(toast, "runAction"));
    CHECK(evaluate(shell, QStringLiteral("Workspaces.find(window.workspace).preset")).toString() == QStringLiteral("export"));
    CHECK(evaluate(shell, QStringLiteral("docking.isOpen('export')")).toBool());
    CHECK_FALSE(evaluate(shell, editHasExport).toBool());
    const QStringList held = exporter->sendToQueue({}, {QStringLiteral("audio-wav")},
                                                   {{QStringLiteral("output"), q(dir / "held.wav")}}, true);
    REQUIRE(held.size() == 1);
    exporter->cancel(held.front());
    REQUIRE(finished(*exporter, held.front()));
    spin(50);
    CHECK_FALSE(toast->property("shown").toBool());

    // Render In to Out: done, or why not with Try Again; cancelled says nothing.
    REQUIRE(QMetaObject::invokeMethod(readiness, "renderFinished", Q_ARG(QString, QStringLiteral("done")), Q_ARG(QString, QString())));
    CHECK(toast->property("text").toString().startsWith(QStringLiteral("Previews rendered")));
    REQUIRE(QMetaObject::invokeMethod(toast, "dismiss"));
    REQUIRE(QMetaObject::invokeMethod(readiness, "renderFinished", Q_ARG(QString, QStringLiteral("cancelled")),
                                      Q_ARG(QString, QStringLiteral("Cancelled"))));
    CHECK_FALSE(toast->property("shown").toBool());
    REQUIRE(QMetaObject::invokeMethod(readiness, "renderFinished", Q_ARG(QString, QStringLiteral("failed")),
                                      Q_ARG(QString, QStringLiteral("The GPU was lost."))));
    CHECK(toast->property("text").toString() == QStringLiteral("Render In to Out failed: The GPU was lost."));
    CHECK(toast->property("actionText").toString() == QStringLiteral("Try Again"));
    REQUIRE(QMetaObject::invokeMethod(toast, "dismiss"));
    spin(100);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}
