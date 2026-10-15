#pragma once

// HomeBridge: the Home screen's data (M5f), the QML singleton `Home`.
//
//   * Sequence presets for New Project: 1080p at 23.976 / 25 / 29.97 / 30 / 60, 4K UHD, vertical 9:16 (1080x1920),
//     square 1:1 and custom; `rates` lists the frame rates a preset can switch to.
//   * createProject(name, folder, sequence): a new project with that sequence, saved as <folder>/<name>.wabcut
//     (ProjectFile.newProjectWith + saveAs).
//   * Recent projects: every project opened or saved (ProjectFile's file changes) is remembered in QSettings
//     ("home/recentProjects", newest first, at most kMaxRecent), with when it was last opened, its format and a
//     thumbnail (the library thumbnail of its first video file). Files that are gone are flagged `missing`.
//   * One-click import: the editors it imports from (`importSources`: what to export there, the file filters).
//     The import itself is ProjectFile.importFiles(..., { newProject, openImported }) from QML, after the
//     unsaved-changes prompt.
//   * Learn / What's new: local documents that exist next to the app or in the source tree (`links`).
//   * showAtLaunch: set by main() for the real app only, so the shell tests start on the editor as before.
//
// Paths cross into QML as file URLs and text; nothing here holds 64-bit ids or ticks (principle 9).

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;

class HomeBridge : public QObject {
    Q_OBJECT
    /// [{ id, name, detail ("1920 × 1080 · 23.976 fps"), width, height, rateNum, rateDen, shape ("landscape" |
    /// "portrait" | "square"), rateChoice (the frame rate can be changed), custom }]
    Q_PROPERTY(QVariantList presets READ presets CONSTANT FINAL)
    /// [{ label ("29.97"), rateNum, rateDen }]
    Q_PROPERTY(QVariantList rates READ rates CONSTANT FINAL)
    /// [{ url, path, name, folder, openedText, format, thumbnail, missing }], newest first.
    Q_PROPERTY(QVariantList recent READ recent NOTIFY recentChanged FINAL)
    /// Where new projects go by default: <Movies>/Wabcut, or the folder of the last project.
    Q_PROPERTY(QUrl defaultFolder READ defaultFolder NOTIFY recentChanged FINAL)
    /// [{ id: "premiere" | "resolve" | "finalcut", name, steps (what to export there), formats, nameFilters }]
    Q_PROPERTY(QVariantList importSources READ importSources CONSTANT FINAL)
    /// Name filters for the drop zone's file dialog: every timeline format, then all files.
    Q_PROPERTY(QStringList timelineNameFilters READ timelineNameFilters CONSTANT FINAL)
    /// [{ id, title, detail, url }]: local documents that exist (Learn, What's new).
    Q_PROPERTY(QVariantList links READ links CONSTANT FINAL)
    Q_PROPERTY(bool showAtLaunch READ showAtLaunch CONSTANT FINAL)

public:
    static constexpr int kMaxRecent = 12;

    explicit HomeBridge(QObject* parent = nullptr);

    static HomeBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    /// main() turns this on before loading the window: the real app opens on the Home screen.
    static void setShowAtLaunch(bool show);

    void setLifecycle(ProjectLifecycle* lifecycle);
    void setController(ProjectController* controller);

    QVariantList presets() const;
    QVariantList rates() const;
    QVariantList recent() const { return m_recent; }
    QUrl defaultFolder() const;
    QVariantList importSources() const;
    QStringList timelineNameFilters() const;
    QVariantList links() const;
    bool showAtLaunch() const;

    /// Why a project cannot be created as <folder>/<name>.wabcut, or "" when it can.
    Q_INVOKABLE QString validateProject(const QString& name, const QUrl& folder) const;
    /// <folder>/<name>.wabcut, as a URL and as the path to show (native separators).
    Q_INVOKABLE QUrl projectUrl(const QString& name, const QUrl& folder) const;
    Q_INVOKABLE QString projectPath(const QString& name, const QUrl& folder) const;
    /// Creates the project and saves it (the folder is created when missing). `sequence`: { width, height,
    /// rateNum, rateDen } (a preset, possibly with another rate). Returns "" or the error for the user.
    Q_INVOKABLE QString createProject(const QString& name, const QUrl& folder, const QVariantMap& sequence);
    /// Re-reads the recent list (which files are missing now).
    Q_INVOKABLE void refreshRecent();
    Q_INVOKABLE void removeRecent(const QUrl& file);
    Q_INVOKABLE void clearRecent();
    /// Remembers `file` as the most recently opened project (ProjectFile's file changes do this).
    Q_INVOKABLE void rememberProject(const QUrl& file);

signals:
    void recentChanged();

private:
    void onDocumentChanged();
    QString currentThumbnail() const;
    QString currentFormat() const;
    void load();
    void store(const QVariantList& entries);

    QPointer<ProjectLifecycle> m_lifecycle;
    QPointer<ProjectController> m_controller;
    QVariantList m_recent;
    QString m_lastPath;
};

/// QML: the singleton `Home` (created with the engine's ProjectFile and ProjectController).
struct HomeBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::HomeBridge)
    QML_NAMED_ELEMENT(Home)
    QML_SINGLETON
public:
    static HomeBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return HomeBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
