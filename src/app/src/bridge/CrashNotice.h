#pragma once

// CrashNotice: the "Wabcut closed unexpectedly" notice of the next start (singleton `CrashNotice`; I.7, docs/release.md
// "Crash reports"). Owner: Graphics (integration). It reads the crash folder (crash::pendingReports) once at startup
// and offers the user three things, none of which sends anything anywhere:
//
//   * the Recovery dashboard (Main.qml opens it: autosaves and backups of the work in progress),
//   * the folder with the crash reports and dumps (the system file manager),
//   * "Copy diagnostic summary": version, OS, GPU and driver, graphics and decode backends only.
//
// It also records this session's renderer (recordRenderer, from the GPU status pill's RendererInfo) as crash
// diagnostics, so a crash later in this run has them.

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <vector>

#include "../crash/CrashReports.h"

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class CrashNotice : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CrashNotice)
    QML_SINGLETON

    /// A run ended badly and the user has not dismissed it yet.
    Q_PROPERTY(bool pending READ pending NOTIFY changed FINAL)
    /// "Wabcut crashed on 12 Oct at 10:20 (access violation)." / "... closed without saying why ..."
    Q_PROPERTY(QString detail READ detail NOTIFY changed FINAL)
    /// Whether a crash report (dump or backtrace) was written for the newest one.
    Q_PROPERTY(bool hasDump READ hasDump NOTIFY changed FINAL)
    /// The crash reports folder, for "Show crash reports".
    Q_PROPERTY(QUrl folderUrl READ folderUrl NOTIFY changed FINAL)
    /// What "Copy diagnostic summary" copies (shown in the dialog too, so the user sees exactly what it is).
    Q_PROPERTY(QString summary READ summary NOTIFY changed FINAL)
    /// More than one run ended badly since the last look.
    Q_PROPERTY(int count READ count NOTIFY changed FINAL)

public:
    explicit CrashNotice(QObject* parent = nullptr);
    static CrashNotice* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    /// The folder crash::install() writes to in the app: "<app local data>/crashes".
    static std::filesystem::path defaultFolder();

    // ---- C++ (tests) ----
    /// Reads `folder` (pending reports other than this process's own session).
    void setFolder(std::filesystem::path folder);
    std::filesystem::path folderPath() const { return m_folder; }

    bool pending() const { return !m_reports.empty(); }
    QString detail() const;
    bool hasDump() const;
    QUrl folderUrl() const;
    QString summary() const;
    int count() const { return static_cast<int>(m_reports.size()); }

    // ---- QML ----
    /// Puts the diagnostic summary on the clipboard.
    Q_INVOKABLE void copyDiagnosticSummary();
    /// Opens the crash reports folder in the system file manager.
    Q_INVOKABLE bool openFolder();
    /// The user has seen it: the notice does not come back for these runs.
    Q_INVOKABLE void dismiss();
    /// This session's renderer, for a later crash report ("D3D11", "NVIDIA GeForce RTX 3060").
    Q_INVOKABLE void recordRenderer(const QString& api, const QString& adapter);

signals:
    void changed();

private:
    void reload();

    std::filesystem::path m_folder;
    std::vector<crash::CrashReport> m_reports;
};

} // namespace wabcut::app
