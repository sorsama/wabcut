#pragma once

// HomeBridge: the Home screen's data (M5f), the QML singleton `Home`.
//
//   * Sequence presets for New Project: 1080p at 23.976 / 25 / 29.97 / 30 / 60, 4K UHD, vertical 9:16 (1080x1920),
//     square 1:1 and custom, then 1440p60, UHD 60 and vertical 60 (UX rework); `rates` lists the frame rates a preset
//     can switch to (23.976 to 240).
//   * createProject(name, folder, settings): a new project with that sequence and the New project page's Advanced
//     settings (ProjectFile.newProjectWith's keys), saved as <folder>/<name>.wabcut (ProjectFile.newProjectWith +
//     saveAs). uniqueProjectName() gives a free default name ("Untitled Project", "Untitled Project 2", ...).
//   * createFromMedia(...) (UX rework, the all-in-one Import): a project made for media files - created at the given
//     settings or matched to the first video, the files imported, edited into the sequence in order (V1 / A1..An, a
//     multi-stream recording's streams on their own named tracks), proxies when the media calls for them, saved. Once
//     the project exists, what goes wrong is a warning (a failed first save too): the project stays open.
//   * classifyImport(urls): what the all-in-one Import does with a drop or a file dialog's choice (open a project,
//     import timelines, the experimental .prproj prompt, guidance for files Wabcut cannot read - dotLottie archives
//     and web addresses among them - or the New project page with the media); folders are expanded (recursive,
//     bounded).
//   * Recent projects: every project opened or saved (ProjectFile's file changes) is remembered in QSettings
//     ("home/recentProjects", newest first, at most kMaxRecentLimit; starred projects - Preferences' "home/starred" -
//     are never pruned), with when it was last opened, its format and a thumbnail (the library thumbnail of its first
//     video file). `recent` lists the newest `recentLimit` (default kMaxRecent) and every starred one. Files that are
//     gone are flagged `missing`. Remove, Clear Unstarred, Undo, Locate (a moved project), Show in Explorer and Copy
//     Path.
//   * Learn and Help: `links` to the website's User Guide (https://wabcut.zyphite.com/guide/...), guideUrl(page).
//   * showAtLaunch: set by main() for the real app only, so the shell tests start on the editor as before.
//
// Paths cross into QML as file URLs and text; nothing here holds 64-bit ids or ticks (principle 9).

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;

class HomeBridge : public QObject {
    Q_OBJECT
    /// [{ id, name, group ("landscape" | "vertical" | "custom"), rateLabel ("29.97"), sizeText ("1920 × 1080"), width,
    /// height, rateNum, rateDen, shape ("landscape" | "portrait" | "square"), rateChoice (the frame rate can be
    /// changed), custom }]
    Q_PROPERTY(QVariantList presets READ presets CONSTANT FINAL)
    /// [{ label ("29.97"), rateNum, rateDen, canDropFrame (29.97 and 59.94) }]
    Q_PROPERTY(QVariantList rates READ rates CONSTANT FINAL)
    /// The New project page's Advanced settings as a new project has them ("Reset to defaults"): { audioRate: 48000,
    /// dropFrame: false, videoTracks: 3, audioTracks: 3, trackFormat: "stereo", masterFormat: "stereo", pixelAspect:
    /// {num: 1, den: 1}, fields: "progressive", startTimecode: "00:00:00:00", previewFormat: "", colorManagement:
    /// {mode: "rec709"} } (createProject's keys).
    Q_PROPERTY(QVariantMap advancedDefaults READ advancedDefaults CONSTANT FINAL)
    /// The Advanced settings' pill choices: { audioRates: [{ value (Hz), label }], pixelAspects: [{ id, label, num,
    /// den }], fields: [{ id, label }], masterFormats: [{ id, label }], trackFormats: [{ id, label }], previewFormats:
    /// [{ id ("" = Auto), label }] }. Colour management: Color.colorManagementModes.
    Q_PROPERTY(QVariantMap advancedChoices READ advancedChoices CONSTANT FINAL)
    /// [{ url, path, name, folder, opened (ISO date-time, "" unknown), openedText, format, thumbnail, missing }],
    /// newest first.
    Q_PROPERTY(QVariantList recent READ recent NOTIFY recentChanged FINAL)
    /// Where new projects go by default: `defaultFolderSetting` when set, else the folder of the last project, else
    /// <Movies>/Wabcut.
    Q_PROPERTY(QUrl defaultFolder READ defaultFolder NOTIFY defaultFolderChanged FINAL)
    /// Settings > Projects: the default location for new projects (QSettings "home/defaultFolder"); empty = automatic
    /// (the last project's folder).
    Q_PROPERTY(QUrl defaultFolderSetting READ defaultFolderSetting WRITE setDefaultFolderSetting NOTIFY
                   defaultFolderChanged FINAL)
    /// Settings > Projects "Recent projects to show": how many recent projects Home and File > Open Recent list
    /// (QSettings "home/recentLimit", kMinRecent to kMaxRecentLimit, default kMaxRecent). Starred projects are listed
    /// on top of it. It only changes what is listed: the stored history keeps up to kMaxRecentLimit.
    Q_PROPERTY(int recentLimit READ recentLimit WRITE setRecentLimit NOTIFY recentLimitChanged FINAL)
    /// The last removeRecent / clearUnstarred / clearRecent can be undone (undoRemoveRecent).
    Q_PROPERTY(bool canUndoRemove READ canUndoRemove NOTIFY recentChanged FINAL)
    /// Before the UX rework (the QML no longer reads it): [{ id: "premiere" | "resolve" | "finalcut", name, steps (what
    /// to export there), formats, nameFilters }] for the Home screen's former timeline import.
    Q_PROPERTY(QVariantList importSources READ importSources CONSTANT FINAL)
    /// Before the UX rework (the QML no longer reads it): the former drop zone's file dialog filters (every timeline
    /// format, then all files). importNameFilters replaced it.
    Q_PROPERTY(QStringList timelineNameFilters READ timelineNameFilters CONSTANT FINAL)
    /// The all-in-one Import's file dialog filters: "All supported (...)", "Media (...)", "Timelines (...)",
    /// "Wabcut projects (*.wabcut)", "All files (*)".
    Q_PROPERTY(QStringList importNameFilters READ importNameFilters CONSTANT FINAL)
    /// Open... and Locate...: "Wabcut projects (*.wabcut)", "All files (*)".
    Q_PROPERTY(QStringList projectNameFilters READ projectNameFilters CONSTANT FINAL)
    /// The website's pages for Learn and Help: [{ id, group ("guide" | "support"), title, detail, url }].
    Q_PROPERTY(QVariantList links READ links CONSTANT FINAL)
    /// https://wabcut.zyphite.com
    Q_PROPERTY(QUrl websiteUrl READ websiteUrl CONSTANT FINAL)
    Q_PROPERTY(bool showAtLaunch READ showAtLaunch CONSTANT FINAL)

public:
    /// The default number of recent projects listed (Settings > Projects can change it: recentLimit, kMinRecent to
    /// kMaxRecentLimit). The stored history keeps up to kMaxRecentLimit, starred projects on top.
    static constexpr int kMaxRecent = 50;
    static constexpr int kMinRecent = 5;
    static constexpr int kMaxRecentLimit = 200;
    /// classifyImport: at most this many files are taken from the folders given, at most this deep.
    static constexpr int kMaxFolderFiles = 1000;
    static constexpr int kMaxFolderDepth = 8;

    /// Asks for proxies of the given media keys ("m12"); returns how many were queued.
    using ProxyRequester = std::function<int(const QStringList& mediaKeys)>;

    explicit HomeBridge(QObject* parent = nullptr);

    static HomeBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    /// main() turns this on before loading the window: the real app opens on the Home screen.
    static void setShowAtLaunch(bool show);

    void setLifecycle(ProjectLifecycle* lifecycle);
    void setController(ProjectController* controller);
    /// Where createFromMedia's proxy request goes by default: the engine's MediaReadiness, resolved on first use.
    void setEngine(QQmlEngine* engine);
    /// Tests: records the proxy request instead of MediaReadiness.generateProxies.
    void setProxyRequester(ProxyRequester requester);

    QVariantList presets() const;
    QVariantList rates() const;
    QVariantMap advancedDefaults() const;
    QVariantMap advancedChoices() const;
    QVariantList recent() const { return m_recent; }
    QUrl defaultFolder() const;
    QUrl defaultFolderSetting() const;
    void setDefaultFolderSetting(const QUrl& folder);
    int recentLimit() const;
    void setRecentLimit(int limit);
    bool canUndoRemove() const { return !m_removed.isEmpty(); }
    QVariantList importSources() const;
    QStringList timelineNameFilters() const;
    QStringList importNameFilters() const;
    QStringList projectNameFilters() const;
    QVariantList links() const;
    QUrl websiteUrl() const;
    bool showAtLaunch() const;

    // ---- New project ----
    /// Why a project cannot be created as <folder>/<name>.wabcut, or "" when it can.
    Q_INVOKABLE QString validateProject(const QString& name, const QUrl& folder) const;
    /// Why `settings` (createProject's keys) cannot make a project ("Fields must be ..."), or "" when they can.
    Q_INVOKABLE QString validateSettings(const QVariantMap& settings) const;
    /// <folder>/<name>.wabcut, as a URL and as the path to show (native separators).
    Q_INVOKABLE QUrl projectUrl(const QString& name, const QUrl& folder) const;
    Q_INVOKABLE QString projectPath(const QString& name, const QUrl& folder) const;
    /// A name for a new project in `folder` that is free and valid: `base` (characters a file name cannot have
    /// replaced, "Untitled Project" when empty), else "<base> 2", "<base> 3", ...
    Q_INVOKABLE QString uniqueProjectName(const QString& base, const QUrl& folder) const;
    /// "1920 × 1080 · 29.97 fps · 48 kHz stereo · 3 video / 3 audio tracks" for createProject's settings (Advanced
    /// keys left out count as their defaults).
    Q_INVOKABLE QString settingsSummary(const QVariantMap& settings) const;
    /// "29.97", "60", "47.952" for a frame rate.
    Q_INVOKABLE QString rateLabel(int rateNum, int rateDen) const;
    /// Creates the project and saves it (the folder is created when missing). `settings`: { width, height, rateNum,
    /// rateDen } (a preset, possibly with another rate) plus any of ProjectFile.newProjectWith's Advanced keys
    /// (audioRate, dropFrame, videoTracks, audioTracks, trackFormat, masterFormat, pixelAspect, fields, startTimecode,
    /// previewFormat, colorManagement, preset), and `sequenceName` (default "Sequence 01"; a `name` key - a preset's -
    /// is ignored). Returns "" or the error for the user. The name, the folder (also one that cannot be made: a drive
    /// that is not there, a file in its path) and the settings are checked first, so such an error creates nothing; only
    /// when the final save fails is the new project left open, untitled, with that error.
    Q_INVOKABLE QString createProject(const QString& name, const QUrl& folder, const QVariantMap& settings);
    /// UX rework, "Start with media": creates the project (createProject; `settings` empty or { match: true } matches
    /// the first file that probes as video, whatever its extension: its size as shown and its frame rate, a variable or
    /// inexact rate snapped to the nominal one, 60 fps when unknown), imports `files` (media and caption files; Lottie
    /// and layered Photoshop files are not imported here: they come back in `deferred` for Main's importUrls; dotLottie
    /// archives and web addresses are skipped), edits the media into the sequence one after another from 00:00 and
    /// saves.
    /// `options`:
    ///   addToSequence  (default true) edit the clips into the sequence in order; false: only in the Library
    ///   tracks         the first video's audio plan [{ index (as GamingStart.recording.audio), name, include }]: its
    ///                  picture on V1 and the included streams on A1, A2, ... named (tracks added as needed); a stream
    ///                  left out of the list is included under its suggested name (Game, Mic, Discord, Audio n). A
    ///                  first video with several audio streams gets this plan with the suggested names when no
    ///                  `tracks` are given.
    ///   proxies        "auto" (default: when the first video is 4K, above 60 fps or 20 minutes or longer), "on"
    ///                  (for the first video), "off"
    /// Returns { ok, error (ok false: nothing was created), warning (ok true: a step after the project was made went
    /// wrong, one line each; the project is open), project (url; empty when its first save failed - the media are still
    /// imported and placed, the project is open untitled and the warning says to save it with File > Save As), added,
    /// captionTracks, failed: [{ name, error }], placed (clips edited in), deferred: [urls] }.
    Q_INVOKABLE QVariantMap createFromMedia(const QString& name, const QUrl& folder, const QVariantMap& settings,
                                            const QList<QUrl>& files, const QVariantMap& options = {});
    /// createFromMedia with this proxy request (GamingStart's create passes its own).
    QVariantMap createFromMediaWith(const QString& name, const QUrl& folder, const QVariantMap& settings,
                                    const QList<QUrl>& files, const QVariantMap& options, const ProxyRequester& requester);

    // ---- Import ----
    /// The all-in-one Import's router: sorts `files` (folders expanded, recursive, at most kMaxFolderFiles files and
    /// kMaxFolderDepth deep; only media, captions, Lottie and layered files are taken from folders) and says what to do:
    /// { route: "project" (open projects[0]) | "timelines" (import as a new project) | "experimental" (ask first:
    /// .prproj) | "media" (the New project page, "Start with media") | "unsupported" (show `message`) | "none",
    ///   projects: [{ url, path, name }], timelines: [{ url, path, name, format, formatName }],
    ///   experimental and unsupported: [{ url, path, name, format, formatName, guidance }] (unsupported: project and
    ///   timeline files of other editors, wrong .xml / .json, unreadable files, dotLottie (.lottie) archives - Lottie
    ///   JSON is what imports - and web addresses: a URL that is not a local file),
    ///   media: [{ url, path, name, kind ("video" | "audio" | "image"), sizeText }], captions: [{ url, path, name,
    ///   format, formatName, sizeText }], lottie and layered: [{ url, path, name, sizeText }],
    ///   firstVideo (url of the first video file, empty when none), hasVideo, suggestedName (the first file's base
    ///   name), notes: [text] (what was left out and why), message (route "unsupported" / "none": what to tell),
    ///   folders (how many were expanded), skipped (files in them left out), truncated (the folder cap was hit) }
    Q_INVOKABLE QVariantMap classifyImport(const QList<QUrl>& files) const;

    // ---- Recent projects ----
    /// Re-reads the recent list (which files are missing now).
    Q_INVOKABLE void refreshRecent();
    /// Removes the entry (Undo: undoRemoveRecent). The star stays (Preferences).
    Q_INVOKABLE void removeRecent(const QUrl& file);
    /// Removes every entry (Undo: undoRemoveRecent).
    Q_INVOKABLE void clearRecent();
    /// "Clear unstarred projects...": removes every entry whose path is not in `starredPaths`
    /// (Preferences.starredProjects()). Returns how many went (Undo: undoRemoveRecent).
    Q_INVOKABLE int clearUnstarred(const QStringList& starredPaths);
    /// Puts back what the last removeRecent / clearUnstarred / clearRecent took out, where it was. False when nothing.
    Q_INVOKABLE bool undoRemoveRecent();
    /// Entries like `recent`'s for these paths (the Starred filter: Preferences.starredProjects()), in that order: the
    /// recent entry when the project is in the list, else one made from the file (opened "", missing when gone).
    Q_INVOKABLE QVariantList projectEntries(const QStringList& paths) const;
    /// Remembers `file` as the most recently opened project (ProjectFile's file changes do this).
    Q_INVOKABLE void rememberProject(const QUrl& file);
    /// Locate...: the entry of `oldFile` (a missing project) points at `newFile` (a .wabcut that exists) from now on,
    /// in its place, keeping when it was opened, its format and thumbnail. A project that is not in the stored list (a
    /// starred one that left it) is ok too and changes nothing here. Returns { ok, error, url, path, name, oldPath };
    /// QML moves the star (Preferences.setStarred(oldPath, false), setStarred(path, true)) in both cases.
    Q_INVOKABLE QVariantMap locateProject(const QUrl& oldFile, const QUrl& newFile);
    /// The nearest folder of `file` that exists (its own folder, else the closest parent; the default folder when none
    /// does): where Locate...'s file dialog starts for a missing project.
    Q_INVOKABLE QUrl existingFolder(const QUrl& file) const;
    /// Show in Explorer (Finder; the folder elsewhere): false when the file is gone or cannot be shown.
    Q_INVOKABLE bool revealProject(const QUrl& file) const;
    /// Copy Path: the project's path (native separators) on the clipboard; returns it.
    Q_INVOKABLE QString copyPath(const QUrl& file) const;

    // ---- Learn and Help ----
    /// The website page of the User Guide: "" or "guide" -> https://wabcut.zyphite.com/guide, "first-run" ->
    /// .../guide/first-run, "/guide/install" -> .../guide/install; an http(s) URL is returned as it is.
    Q_INVOKABLE QUrl guideUrl(const QString& page) const;

signals:
    void recentChanged();
    void defaultFolderChanged();
    void recentLimitChanged();

private:
    void onDocumentChanged();
    QString currentThumbnail() const;
    QString currentFormat() const;
    void load();
    void store(const QVariantList& entries);
    /// What `recent` lists: drops the oldest entries that are not starred until at most recentLimit() are left (the
    /// stored history is capped at kMaxRecentLimit by rememberProject).
    QVariantList pruned(QVariantList entries) const;
    int requestProxies(const QStringList& mediaKeys, const ProxyRequester& requester);

    QPointer<ProjectLifecycle> m_lifecycle;
    QPointer<ProjectController> m_controller;
    QPointer<QQmlEngine> m_engine;
    ProxyRequester m_proxyRequester;
    QVariantList m_recent;
    QString m_lastPath;
    /// What the last removal took out: (index in the stored list, stored entry), in index order.
    QList<QPair<int, QVariant>> m_removed;
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
