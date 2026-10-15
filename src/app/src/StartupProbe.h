#pragma once

// Startup timing for the performance benchmarks (P.1, docs/performance.md). Inert unless WABCUT_PERF_STARTUP names a
// file; then:
//   * the milestones of the start (milliseconds since the process was created: main, crashHandler, application,
//     plugins, qmlLoaded, firstIdle (the event loop's first idle moment after the window loaded) and firstFrame (the
//     window's first frame swapped)) are written to that file as JSON once both firstIdle and firstFrame happened, or
//     15 s after qmlLoaded (a platform that renders nothing, such as "offscreen" without a GPU, only gets firstIdle);
//   * WABCUT_PERF_STARTUP_QUIT=1 quits the app right after writing them;
//   * WABCUT_PERF_SETTINGS_DIR keeps the run away from the user's own Wabcut: settings in an INI file in that folder
//     and every QStandardPaths location (app data, caches, crash reports) in Qt's test locations.
// Construct it first thing in main(), before anything reads settings.

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QQuickWindow;

namespace wabcut::app {

class StartupProbe : public QObject {
public:
    StartupProbe();

    bool active() const { return m_active; }
    /// Records `name` now (the first time only).
    void mark(const char* name);
    /// After the window loaded: records qmlLoaded, waits for firstIdle and the window's firstFrame.
    void loaded(QQuickWindow* window);

private:
    double now() const;
    void maybeFinish();
    void finish();

    bool m_active = false;
    bool m_quit = false;
    bool m_done = false;
    QString m_file;
    QJsonObject m_milestones;
};

} // namespace wabcut::app
