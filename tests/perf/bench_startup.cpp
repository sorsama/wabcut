// Startup benchmarks (P.1, docs/performance.md): wabcut.exe launched as a user would, with StartupProbe
// (src/app/src/StartupProbe.h) writing its milestones and quitting at the first frame. Every launch keeps its settings,
// app data and QML disk cache in a folder of its own, so the user's Wabcut is never touched.
//   * cold: a fresh folder (no QML disk cache, no settings, empty caches). The operating system's file cache stays
//     warm (flushing it needs administrator rights), so a first start after booting is slower still.
//   * warm: the same folder again.
//   * recent project: warm, with the 1-hour, 10,000-clip project as the last project and on the Home screen's recent
//     list, its autosaves and backups next to it (what a day of editing leaves).
//   * open project: warm, the same project passed on the command line (Explorer's "Open with").
// "Interactive" is the window's first frame swapped after the event loop went idle (StartupProbe's firstFrame and
// firstIdle); where the platform renders nothing, the first idle moment.

#include "PerfHarness.h"
#include "PerfProjects.h"

#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

using namespace wabcut;
namespace fs = std::filesystem;

namespace {

struct Launch {
    bool ok = false;
    double interactiveMs = 0.0; ///< firstFrame, else firstIdle
    bool rendered = false;      ///< a firstFrame was seen
    QJsonObject milestones;
    double wallMs = 0.0;        ///< the parent's view: start to exit
    std::string error;
};

QString q(const fs::path& p) { return QString::fromStdU16String(p.u16string()); }

/// The app to start: WABCUT_PERF_APP_EXE (a saved build, e.g. the binaries a baseline was measured with), else this
/// build's.
fs::path appExe() {
    const QString override = qEnvironmentVariable("WABCUT_PERF_APP_EXE");
    return override.isEmpty() ? fs::path(WABCUT_APP_EXE) : fs::path(override.toStdU16String());
}

/// Runs the app once with its own settings folder; `args` are its command-line arguments.
Launch launchApp(const fs::path& folder, const QStringList& args = {}) {
    Launch out;
    const fs::path report = folder / "startup.json";
    std::error_code ec;
    fs::remove(report, ec);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // The app starts as a user's would: none of the settings this test process gave itself (the shell fixture's
    // scaling and graphics API, test paths) carry over.
    for (const char* inherited : {"QT_ENABLE_HIGHDPI_SCALING", "QSG_RHI_BACKEND", "QT_QUICK_BACKEND", "QT_SCALE_FACTOR",
                                  "WABCUT_CACHE_DIR", "WABCUT_AI_KEYSTORE", "WABCUT_PLUGIN_CACHE"}) {
        env.remove(QString::fromLatin1(inherited));
    }
    env.insert(QStringLiteral("WABCUT_PERF_STARTUP"), q(report));
    env.insert(QStringLiteral("WABCUT_PERF_STARTUP_QUIT"), QStringLiteral("1"));
    env.insert(QStringLiteral("WABCUT_PERF_SETTINGS_DIR"), q(folder / "settings"));
    env.insert(QStringLiteral("QML_DISK_CACHE_PATH"), q(folder / "qmlcache"));
    env.insert(QStringLiteral("WABCUT_CACHE_DIR"), q(folder / "cache"));
    env.insert(QStringLiteral("WABCUT_AUDIO_BACKEND"), QStringLiteral("null"));
    if (!env.contains(QStringLiteral("QT_QPA_PLATFORM"))) {
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral(WABCUT_PERF_STARTUP_PLATFORM));
    }
    // windeployqt puts only the platform plugin the app ships (qwindows) next to it; without the one asked for, Qt
    // would show its "no platform plugin" message box and wait forever. Qt's own plugin folder has them all.
    if (!fs::exists(appExe().parent_path() / "platforms" /
                    ("q" + env.value(QStringLiteral("QT_QPA_PLATFORM")).toStdString() + ".dll"))) {
        env.insert(QStringLiteral("QT_PLUGIN_PATH"), QStringLiteral(WABCUT_QT_PLUGIN_DIR));
    }
    QProcess app;
    app.setProcessEnvironment(env);
    app.setProgram(q(appExe()));
    app.setArguments(args);
    app.setProcessChannelMode(QProcess::MergedChannels);
    const auto t0 = std::chrono::steady_clock::now();
    app.start();
    if (!app.waitForStarted(10'000)) {
        out.error = "the app did not start: " + app.errorString().toStdString();
        return out;
    }
    if (!app.waitForFinished(60'000)) {
        app.kill();
        app.waitForFinished(5'000);
        out.error = "the app did not quit within 60 s";
        return out;
    }
    out.wallMs = perf::msSince(t0);
    std::ifstream in(report, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const QJsonObject j = QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
    out.milestones = j.value(QStringLiteral("milestones")).toObject();
    if (out.milestones.isEmpty()) {
        out.error = "no startup report (exit code " + std::to_string(app.exitCode()) + "): " +
                    app.readAll().left(600).toStdString();
        return out;
    }
    out.rendered = out.milestones.contains(QStringLiteral("firstFrame"));
    const double idle = out.milestones.value(QStringLiteral("firstIdle")).toDouble();
    out.interactiveMs = out.rendered ? std::max(idle, out.milestones.value(QStringLiteral("firstFrame")).toDouble()) : idle;
    out.ok = out.interactiveMs > 0.0;
    return out;
}

/// Settings of a user whose last project (also first on the recent list) is `project`.
void writeRecentSettings(const fs::path& folder, const fs::path& project) {
    // QSettings' INI layout for UserScope: <path>/<organization>/<application>.ini.
    QSettings settings(q(folder / "settings" / "Wabcut" / "Wabcut.ini"), QSettings::IniFormat);
    settings.setValue(QStringLiteral("project/lastFile"), q(project));
    settings.setValue(QStringLiteral("home/recentProjects"),
                      QVariantList{QVariantMap{{QStringLiteral("path"), q(project)},
                                               {QStringLiteral("opened"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                                               {QStringLiteral("format"), QStringLiteral("wabcut")}}});
    settings.sync();
}

/// The 1-hour project with what a day of editing leaves next to it: 5 backups, the autosave and 2 older autosaves.
fs::path writeBigProject(const fs::path& dir) {
    fs::create_directories(dir);
    const fs::path file = dir / "One hour.wabcut";
    const perf::OneHourProject p = perf::oneHourProject();
    REQUIRE(io::saveProject(p.state, file));
    for (int n = 1; n <= 5; ++n) {
        fs::copy_file(file, io::backupPath(file, n), fs::copy_options::overwrite_existing);
    }
    REQUIRE(io::writeAutosave(p.state, file));
    for (int n = 2; n <= 3; ++n) {
        fs::path rotated = io::autosavePath(file);
        rotated += std::to_string(n);
        fs::copy_file(io::autosavePath(file), rotated, fs::copy_options::overwrite_existing);
    }
    return file;
}

void addMilestones(perf::PerfResult& r, const std::string& prefix, const Launch& l) {
    for (const char* name : {"main", "crashHandler", "application", "plugins", "qmlLoaded", "firstIdle", "firstFrame"}) {
        if (l.milestones.contains(QLatin1String(name))) {
            r.note(prefix + "_" + name + "_ms", "ms", l.milestones.value(QLatin1String(name)).toDouble());
        }
    }
}

} // namespace

TEST_CASE("Benchmark startup: cold and warm to an interactive window, with and without a 10,000-clip recent project",
          "[perf][gpu][startup]") {
    if (!fs::exists(appExe())) {
        SKIP("wabcut.exe is not built: " << appExe().string());
    }
    const fs::path root = fs::path(perf::outputDir()) / "startup";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path project = writeBigProject(root / "projects");

    const auto outcome = perf::runBenchmark("startup", [&] {
        perf::PerfResult r;
        perf::Samples cold, warm, recent, open;
        Launch lastWarm, lastRecent, lastOpen, lastCold;
        bool rendered = true;
        const auto run = [&](perf::Samples& into, Launch& last, const fs::path& folder, const QStringList& args) {
            Launch l = launchApp(folder, args);
            INFO(l.error);
            REQUIRE(l.ok);
            rendered = rendered && l.rendered;
            into.add(l.interactiveMs);
            last = std::move(l);
        };
        static int attempt = 0;
        ++attempt;
        for (int i = 0; i < 3; ++i) { // a fresh folder each time
            run(cold, lastCold, root / ("cold-" + std::to_string(attempt) + "-" + std::to_string(i)), {});
        }
        const fs::path warmFolder = root / ("warm-" + std::to_string(attempt));
        launchApp(warmFolder); // fills its QML cache and settings
        for (int i = 0; i < 5; ++i) {
            run(warm, lastWarm, warmFolder, {});
        }
        const fs::path recentFolder = root / ("recent-" + std::to_string(attempt));
        launchApp(recentFolder);
        writeRecentSettings(recentFolder, project);
        for (int i = 0; i < 3; ++i) {
            run(recent, lastRecent, recentFolder, {});
            writeRecentSettings(recentFolder, project); // a run may have rewritten them
        }
        for (int i = 0; i < 3; ++i) {
            run(open, lastOpen, recentFolder, {q(project)});
            writeRecentSettings(recentFolder, project);
        }
        r.addSamples("cold_ms", "ms", cold);
        r.addSamples("warm_ms", "ms", warm);
        r.addSamples("recent_project_ms", "ms", recent);
        r.addSamples("open_project_ms", "ms", open);
        addMilestones(r, "warm", lastWarm);
        addMilestones(r, "recent", lastRecent);
        addMilestones(r, "open", lastOpen);
        r.note("warm_process_wall_ms", "ms", lastWarm.wallMs);
        r.details["platform"] = WABCUT_PERF_STARTUP_PLATFORM;
        r.details["interactive"] = rendered ? "first frame swapped after the first idle moment"
                                            : "first idle moment (this platform rendered no frame)";
        r.details["project"] = "1 hour, 10,000 clips, 5 backups, autosave + 2 rotated autosaves";
        return r;
    });
    INFO(outcome.describe());
    if (!outcome.baselineFound) {
        WARN(outcome.describe());
    }
    CHECK(outcome.ok());
}
