#pragma once

// ProjectLifecycle: the project file behind ProjectController's EditSession, for QML (singleton `ProjectFile`).
// Owner: Graphics/playback (I.1). The work happens in wabcut_editor (editor::ProjectDocument, MediaImport,
// SourceEdit); this class converts URLs and ids and reports results to QML.
//
//   * File > New / Open / Save / Save As and the dirty state (`dirty`, `windowTitle` with a leading "●").
//   * Autosave every `autosaveIntervalSeconds` (rotated "<file>.autosave", "<file>.autosave2", ...; untitled
//     projects autosave under <AppLocalData>/Autosave).
//   * Import (File > Import, drops on the Library): media files are probed and added; timelines from other
//     editors (.xml / .fcpxml / .otio / .edl / .prproj) go through the interchange importer and `importReport` is
//     emitted for the report dialog. Importing while the demo project is shown starts a new untitled project
//     whose sequence matches the first video imported. 0.4: folders (File > Import Folder…) become bins, subfolders
//     nested, in the bin given (the Library's open bin); import dialogs open where the last dialog import came from
//     (`importFolder`, QSettings import/lastFolder and import/recentFolders).
//   * Offline media and relinking: `offlineMedia`, `offlineMediaFound` after opening, relink by file or by
//     searching folders (file name + fingerprint).
//   * Library -> timeline drops (dropMedia): overwrite at the drop point, insert with Ctrl/Cmd (Premiere); files
//     dragged from the desktop onto the timeline (dropFiles): imported, then dropped the same way.
//   * 0.2 (P.3) Project locking (Premiere's Productions, and its "Enable Project Locking" preference for other
//     projects): a project in a production is locked for this session while it is open (editor/Production.h); a project
//     another session has locked opens read-only (`readOnly`, `lockOwner`): Save is refused (Save As works) and it
//     does not autosave next to the other session's file.
//   * 0.2 (P.3) Several projects open (Premiere keeps several projects open, one at a time in the panels):
//     openAlongside() opens another project without closing this one, switchTo() makes an open project the one the
//     panels show at once. Each keeps its own undo history, unsaved changes, lock and view (sequence, playhead): the
//     others are parked out of the one EditSession (EditSession::exchange), autosaved when left, and cannot change
//     while parked. The quit question goes through every open project with unsaved changes (firstDirtyProject).
//   * 0.2 (P.3) Production Settings: the active project's production may keep the media cache (waveforms,
//     filmstrips) in a folder of its own (applyScratchDisks; previews follow editor::previewFolderFor).
//   * Crash recovery: a clean exit removes the current project's autosaves, so what is left at startup (untitled
//     autosaves, or the last project's autosave newer than its file) means work an unclean exit did not save:
//     `recoveryFiles` lists it and Main.qml offers it (RecoveryDialog). Opening a project whose autosave is newer
//     offers it too.
//
// Times and ids cross into QML as timecode strings and opaque keys (BridgeIds.h), never raw ticks.

#include <wabcut/editor/ProjectDocument.h>

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::edit {
struct SequenceSettings;
} // namespace wabcut::edit

namespace wabcut::app {

class ProjectController;

class ProjectLifecycle : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(ProjectFile)
    QML_SINGLETON

    /// File name without extension, or "Untitled".
    Q_PROPERTY(QString name READ name NOTIFY documentChanged FINAL)
    /// "● name — Wabcut" while there are unsaved changes, "name — Wabcut" otherwise.
    Q_PROPERTY(QString windowTitle READ windowTitle NOTIFY documentChanged FINAL)
    Q_PROPERTY(bool dirty READ dirty NOTIFY documentChanged FINAL)
    Q_PROPERTY(bool untitled READ untitled NOTIFY documentChanged FINAL)
    /// The built-in demo project is shown (no project opened, created or imported into yet).
    Q_PROPERTY(bool demo READ demo NOTIFY documentChanged FINAL)
    Q_PROPERTY(QString filePath READ filePath NOTIFY documentChanged FINAL)
    /// Folder for file dialogs: the project's, else the user's videos folder.
    Q_PROPERTY(QUrl folder READ folder NOTIFY documentChanged FINAL)
    /// 0.4: where import dialogs open: the folder the last dialog import came from (QSettings import/lastFolder) while
    /// it exists, else `folder`.
    Q_PROPERTY(QUrl importFolder READ importFolder NOTIFY importFolderChanged FINAL)
    /// "Autosaved 14:05", "Not autosaved yet", ...
    Q_PROPERTY(QString autosaveText READ autosaveText NOTIFY autosaveChanged FINAL)
    /// 0.4: false after an autosave could not be written, until an autosave, save, open, revert or recovery succeeds.
    Q_PROPERTY(bool autosaveOk READ autosaveOk NOTIFY autosaveChanged FINAL)
    Q_PROPERTY(int autosaveIntervalSeconds READ autosaveIntervalSeconds WRITE setAutosaveIntervalSeconds NOTIFY
                   autosaveChanged FINAL)
    /// [{ id: "m…", name, path }] of the media items whose file is missing or not the original.
    Q_PROPERTY(QVariantList offlineMedia READ offlineMedia NOTIFY mediaStatusChanged FINAL)
    Q_PROPERTY(int offlineCount READ offlineCount NOTIFY mediaStatusChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /// File dialog filters for File > Import.
    Q_PROPERTY(QStringList importNameFilters READ importNameFilters CONSTANT FINAL)
    /// Work an unclean exit left: [{ url, name, when, project (url of the project file, "" when untitled) }],
    /// newest first.
    Q_PROPERTY(QVariantList recoveryFiles READ recoveryFiles NOTIFY recoveryChanged FINAL)
    /// The recovery dashboard (I.4, File > Recover…): every version of the open (else the last) project - as saved now,
    /// its autosaves and its backups - and the untitled autosaves: [{ url, kind ("current", "autosave", "backup",
    /// "untitled"), title, when, loadable, error, sequences, clips, media, cues, duration, sequence, poster (image url,
    /// "" until rendered), newer, comparison (vs the version saved now) }], newest first after the current one.
    Q_PROPERTY(QVariantList recoveryVersions READ recoveryVersions NOTIFY recoveryVersionsChanged FINAL)
    Q_PROPERTY(bool recoveryScanning READ recoveryScanning NOTIFY recoveryVersionsChanged FINAL)
    /// 0.2 (P.3): another session has the project locked: it is read-only here ("" / false otherwise).
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY documentChanged FINAL)
    Q_PROPERTY(QString lockOwner READ lockOwner NOTIFY documentChanged FINAL)
    /// The production folder the project lies in ("" when none).
    Q_PROPERTY(QString productionFolder READ productionFolder NOTIFY documentChanged FINAL)
    /// Premiere's Preferences > General > Enable Project Locking: lock projects outside productions too (per user).
    Q_PROPERTY(bool lockProjects READ lockProjects WRITE setLockProjects NOTIFY lockProjectsChanged FINAL)
    /// 0.2 (P.3): the projects open now, in the order they were opened: [{ index, name, path, url, dirty, active,
    /// readOnly, demo }]; and the index of the active one (the one the panels show).
    Q_PROPERTY(QVariantList openProjects READ openProjects NOTIFY documentChanged FINAL)
    Q_PROPERTY(int activeProject READ activeProject NOTIFY documentChanged FINAL)

public:
    explicit ProjectLifecycle(ProjectController* controller, QObject* parent = nullptr);
    ~ProjectLifecycle() override;
    /// QML singleton factory: attaches to the engine's ProjectController singleton.
    static ProjectLifecycle* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    // ---- C++ ----
    editor::ProjectDocument& document() { return *m_document; }
    ProjectController* controller() const { return m_controller; }

    // ---- Properties ----
    QString name() const;
    QString windowTitle() const;
    bool dirty() const;
    bool untitled() const;
    bool demo() const { return m_demo; }
    QString filePath() const;
    QUrl folder() const;
    QUrl importFolder() const;
    QString autosaveText() const { return m_autosaveText; }
    bool autosaveOk() const { return m_autosaveOk; }
    int autosaveIntervalSeconds() const { return m_autosaveSeconds; }
    void setAutosaveIntervalSeconds(int seconds);
    QVariantList offlineMedia() const;
    int offlineCount() const;
    QString lastError() const { return m_lastError; }
    QStringList importNameFilters() const;
    bool readOnly() const { return m_readOnly; }
    QString lockOwner() const { return m_lockOwner; }
    QString productionFolder() const;
    bool lockProjects() const;
    void setLockProjects(bool on);
    QVariantList recoveryFiles() const {
        collectRecovery(true);
        return m_recovery;
    }
    QVariantList openProjects() const;
    int activeProject() const { return static_cast<int>(m_active); }

    // ---- File ----
    /// An empty untitled project with one sequence (1920x1080, 30 fps).
    Q_INVOKABLE void newProject();
    /// UX rework (Home): the built-in demo project again, as at launch (Home's Learn page "Explore the sample
    /// project", Help > Open the Sample Project, File > Close Project with one project open). It replaces the active
    /// project without a question (QML asks about unsaved changes first): that project's autosaves are removed and its
    /// lock released, as closeProject does; other open projects stay open. A demo already shown starts over (its edits
    /// are dropped). `demo` is true afterwards, nothing is dirty and the history is empty. False (lastError) while an
    /// edit gesture is in progress.
    Q_INVOKABLE bool openDemo();
    Q_INVOKABLE bool open(const QUrl& file);
    /// The project file the app was started with (File Explorer's "Open" through the installer's opt-in .wabcut
    /// association runs `wabcut.exe "<file>"`), or an empty URL. Main.qml opens it through the normal path (I.7).
    Q_INVOKABLE QUrl launchProject() const;
    /// launchProject() for an argument list: the first argument that names an existing .wabcut file (any case), skipping
    /// the program, options and the value of --rhi; anything else is ignored.
    static QUrl launchProjectIn(const QStringList& arguments);
    /// False while untitled (QML then asks for a file and calls saveAs) or when writing fails (lastError).
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QUrl& file);
    /// UX rework (stream S), File > Save a Copy... (Ctrl+Alt+S): writes the project as it is now to `file` (".wabcut"
    /// added when missing; relative media paths for that folder) and changes nothing else - the project keeps its file,
    /// its unsaved changes, lock and autosaves; the copy is not opened. Works for untitled projects and the demo too.
    /// False (lastError) when it cannot be written, or `file` is this project's own file or one open next to it.
    Q_INVOKABLE bool saveCopy(const QUrl& file);
    /// UX rework (stream S), File > Revert: the project as last saved, read again from its file (QML asks first). The
    /// unsaved changes, the undo history and this project's autosaves are dropped (so no recovery offer follows); the
    /// sequence shown and the playhead stay when they still exist. False (lastError) while untitled, for the demo,
    /// during an edit gesture, or when the file cannot be read (nothing changes then).
    Q_INVOKABLE bool revert();
    /// Writes the autosave now when something changed since the last one.
    Q_INVOKABLE bool autosaveNow();
    /// What the autosave timer runs (P.1): the job is prepared here and written on a worker, one at a time; its
    /// result arrives through the event loop (autosaveChanged, or an error). Tests call it instead of waiting.
    void startBackgroundAutosave();

    // ---- Several projects open (0.2, P.3) ----
    /// Opens `file` next to the projects already open and makes it the active one; the one left stays loaded with its
    /// undo history and unsaved changes (it is autosaved first). A project already open is switched to. The demo is
    /// replaced (it is not a project). False (lastError) when the file cannot be opened: nothing changes then.
    Q_INVOKABLE bool openAlongside(const QUrl& file);
    /// Makes open project `index` (of openProjects) the one the panels show, at once: nothing is saved or closed.
    /// Refused while a drag or another gesture is in progress.
    Q_INVOKABLE bool switchTo(int index);
    /// Closes open project `index` without a question (QML asks first, switching to a project with unsaved changes):
    /// its lock is released and its autosaves removed. Closing the active one makes its neighbour active. False for
    /// the only project open.
    Q_INVOKABLE bool closeProject(int index);
    /// The index of an open project with unsaved changes, the active one first; -1 when none (the quit question).
    Q_INVOKABLE int firstDirtyProject() const;
    /// The index (in openProjects) of the open project whose file is `file`, or -1.
    Q_INVOKABLE int indexOfProject(const QUrl& file) const;
    /// The state of the open project whose file is `file` (the active one or a parked one, with its unsaved changes);
    /// null when it is not open.
    ProjectSnapshot openProjectState(const std::filesystem::path& file) const;
    /// Applies the active project's Production Settings (its media cache folder, else the app's); emits
    /// scratchDisksChanged when the project or its settings changed. Called whenever the active project changes;
    /// ProductionBridge calls it after changing the settings.
    void applyScratchDisks();

    // ---- Crash recovery ----
    /// Opens a recovery file (from recoveryFiles) as its project, with unsaved changes, and autosaves that revision
    /// at once. The recovery file is never deleted here: a titled project's autosave rotates into "autosave2" when the
    /// new one is written, and an untitled session's file is retired by the document only after its replacement was
    /// written (Codex wave-3 P1-3). The offer is dismissed.
    Q_INVOKABLE bool recover(const QUrl& file);
    /// Removes a recovery offer from the list; the file stays on disk.
    Q_INVOKABLE void dismissRecovery(const QUrl& file);
    /// Forgets a recovery file the user chose to throw away (deletes it).
    Q_INVOKABLE void discardRecovery(const QUrl& file);
    Q_INVOKABLE void discardAllRecovery();
    /// Re-scans for recovery files (startup does it once).
    void scanRecovery();
    /// Lists every version (recoveryVersions) in the background, then renders their poster frames.
    Q_INVOKABLE void scanRecoveryVersions();
    /// Opens a version (from recoveryVersions) as an untitled copy with unsaved changes: the project file is not
    /// touched until the user saves somewhere.
    Q_INVOKABLE bool openRecoveryCopy(const QUrl& file);
    QVariantList recoveryVersions() const { return m_versions; }
    bool recoveryScanning() const { return m_versionsScanning; }

    // ---- Import ----
    /// Imports files (see the class comment). Returns { added, existing, failed: [{ name, error }],
    /// timelines, projects: [urls of .wabcut files, which are opened rather than imported] }; emits importReport
    /// for every timeline file.
    /// `options` (all off by default; the Home screen's one-click import, M5f):
    ///   newProject                  start a new untitled project for the files even when one is open; when they
    ///                               are only timelines, the new project holds just the imported sequences (its
    ///                               empty first sequence goes in the same undo step as the import)
    ///   openImported                make the first imported sequence current; the result gets `firstSequence`
    ///                               (its index in ProjectController.sequenceNames, -1 when none)
    ///   experimentalPremiereProject read .prproj files with the experimental reader (only after the user chose
    ///                               it: the UI labels it experimental)
    ///   bin                         0.4: the bin for the media (a Library bin key "b12"; "" or unknown: the root; a
    ///                               search bin: its parent); ignored when this call starts a new project
    /// 0.4: folders given (Import Folder, folder drops) are expanded (editor::collectImportFolder): each becomes a bin
    /// of its name in `bin`, subfolders nested, in the same one undo step, named "Import Folder". The summary then also
    /// has folders (how many were given), bins (how many were made), skipped: [{ name, reason: "notMedia" | "hidden" |
    /// "link" | "bundle" | "cameraCard" | "tooDeep" }], truncated (only the first 1000 files were looked at) and
    /// folderSummary (the toast: "Imported 12 files from Session1 into 3 bins. ..."; "" when no folder was given).
    Q_INVOKABLE QVariantMap importFiles(const QList<QUrl>& files, const QVariantMap& options = {});
    /// 0.4: an import dialog's choice (files or folders): import/lastFolder = the first one's folder (a folder's parent);
    /// import/recentFolders gets each file's folder / each folder (deduplicated, case-insensitive on Windows, 5 kept).
    Q_INVOKABLE void rememberImportFolder(const QList<QUrl>& chosen);
    /// 0.4: import/recentFolders, newest first, only folders that exist (native separators).
    static QStringList recentImportFolders();

    // ---- Export Timeline (0.1.1, File > Export Timeline...) ----
    /// Writes sequence `sequenceIndex` (an index in ProjectController.sequenceNames) as a timeline file other editors
    /// read, through the interchange.export command on a worker, on a snapshot of the project (nothing changes).
    /// `format`: "fcp7-xml", "fcpxml", "otio" or "edl"; or for audio post (P.2) "aaf" or "omf", with `audioPost` the
    /// AAF / OMF settings of interchange.export (sampleRate, bitsPerSample, files, audioFormat, render, handleFrames,
    /// mixdownVideo, breakoutToMono, renderAudioClipEffects, includeClipCopiesWithoutEffects, omfTitle, includePan):
    /// the clips' audio is rendered by the playback engine. False when an export is running or nothing can be
    /// exported (lastError says why); otherwise timelineExported follows.
    Q_INVOKABLE bool exportTimeline(const QString& format, int sequenceIndex, const QUrl& file,
                                    const QVariantMap& audioPost = {});
    /// 0.2: Export Timeline of several sequences at once: one file per sequence (indices in sequenceNames) in `folder`
    /// (an existing folder), named after the sequence ("Name.xml"; "Name (2).xml" for a second sequence of that name),
    /// written one after another on a worker. timelineExported then carries one combined report: ok (every file
    /// written), file ("3 timelines"), summary, files: [{ sequence, file, path, ok, error }], the summed counts and
    /// every file's entries (their location names the sequence); error lists the sequences that failed.
    Q_INVOKABLE bool exportTimelines(const QString& format, const QVariantList& sequenceIndices, const QUrl& folder,
                                     const QVariantMap& audioPost = {});

    // ---- Home screen (M5f) ----
    /// An empty untitled project whose one sequence comes from a preset: { name, width, height, rateNum,
    /// rateDen }. False (lastError) for an invalid size or rate; nothing changes then.
    /// UX rework: the New project page's Advanced settings are applied before the project is shown (no undo step, not
    /// dirty). Optional keys (unknown keys are ignored; a bad value is an error and nothing changes):
    ///   preset          a model preset id (Editing.sequencePresets: "1080p29.97df", "2160p60", "shorts", ...): size,
    ///                   rate and drop-frame at once; the other keys override it
    ///   width, height   16 to 16384 pixels
    ///   rateNum/rateDen an exact rate up to 1000 fps
    ///   audioRate       Hz, 8000 to 384000 (44100, 48000, 96000)
    ///   dropFrame       drop-frame timecode (kept only at 29.97 and 59.94)
    ///   videoTracks, audioTracks  1 to 99 (default 3 each)
    ///   trackFormat     the audio tracks' channel format: "stereo" (default), "mono", "5.1", "adaptive"
    ///   masterFormat    "stereo" (default) or "5.1"
    ///   pixelAspect     { num, den } or a ratio (1, 0.9091, 2), 0.1 to 10
    ///   fields          "progressive" (default), "upperFirst", "lowerFirst"
    ///   startTimecode   the first frame's timecode ("01:00:00:00"; "" = 00:00:00:00)
    ///   previewFormat   "" or "auto" (default), "prores", "dnxhr", "h264"
    ///   colorManagement { mode (Color.colorManagementModes ids: rec709, hdr-hlg, hdr-pq, aces, wide-gamut, off),
    ///                   peakNits, referenceWhiteNits, autoDetectLog } or just the mode id
    Q_INVOKABLE bool newProjectWith(const QVariantMap& sequence);
    /// What newProjectWith would refuse in `sequence` (the message it would give), or "" when it can be created.
    static QString checkNewProjectSettings(const QVariantMap& sequence);
    /// What importing `file` would do, before importing it: { kind: "media" | "timeline" | "project" | "captions" |
    /// "folder" (0.4),
    /// format (id, "fcp7-xml"…), formatName ("Final Cut Pro 7 XML"…), supported (can be read now),
    /// experimental (only with the experimental reader: Premiere .prproj), guidance (what to export instead,
    /// for files that cannot be read) }. Timeline fields are empty for media and projects.
    Q_INVOKABLE QVariantMap detectImport(const QUrl& file) const;

    // ---- Relink ----
    /// [{ url, path, matches }] for `mediaId` under `folders`, fingerprint matches first.
    Q_INVOKABLE QVariantList relinkCandidates(const QString& mediaId, const QList<QUrl>& folders) const;
    /// Points `mediaId` at `file`; refuses a different file (lastError) unless `force`.
    Q_INVOKABLE bool relink(const QString& mediaId, const QUrl& file, bool force = false);
    /// Relinks every offline item found (by name and fingerprint) under `folders`; returns how many.
    Q_INVOKABLE int relinkAll(const QList<QUrl>& folders);

    // ---- Library -> timeline ----
    /// Edits `mediaId` into the current sequence at `timecode` on the track `trackId` ("" = targeted tracks). A
    /// sequence's key instead of a media item's nests that sequence there (refused for the sequence itself, or one
    /// that plays it).
    Q_INVOKABLE bool dropMedia(const QString& mediaId, const QString& timecode, const QString& trackId,
                               bool overwrite);
    /// Files dragged from the desktop (Explorer, Finder) onto the timeline: imported as importFiles does, then the
    /// media placed one after another from `timecode` on the track `trackId` (dropMedia; a new project started for
    /// the demo has none of its tracks: the targeted ones). A dropped folder is imported into bins at the Library
    /// root (as Import Folder) and its files are not placed. Returns importFiles' summary plus `placed` (how many
    /// files went on the timeline); the new clips are selected.
    Q_INVOKABLE QVariantMap dropFiles(const QList<QUrl>& files, const QString& timecode, const QString& trackId,
                                      bool overwrite);
    /// Ctrl (Cmd on macOS) is held now: a drop inserts (Premiere: a plain drop overwrites).
    Q_INVOKABLE bool insertModifierHeld() const;

signals:
    void documentChanged();
    void autosaveChanged();
    void mediaStatusChanged();
    void lastErrorChanged();
    /// After importing a timeline: { file, format, sequences, media, unresolved: [{ name, path }],
    /// carried, approximated, dropped, entries: [{ outcome, category, reason, location }], summary }.
    void importReport(const QVariantMap& report);
    /// After exportTimeline: { ok, error, exported: true, path } and, when it worked, ImportReportDialog's map
    /// ({ file, format, summary, sequences, carried, approximated, dropped, entries }).
    void timelineExported(const QVariantMap& report);
    /// After opening a project with missing media.
    void offlineMediaFound(int count);
    void recoveryChanged();
    void recoveryVersionsChanged();
    void lockProjectsChanged();
    /// Recovery files appeared after startup (an opened project has a newer autosave).
    void recoveryAvailable();
    /// 0.2 (P.3): the active project's scratch disks may have changed (another project, or its Production Settings).
    void scratchDisksChanged();
    /// 0.4: importFolder may have changed (a dialog import was remembered, or the project's folder changed).
    void importFolderChanged();
    /// 0.4: every failed autosave (timer or autosaveNow), with the reason; not deduplicated.
    void autosaveFailed(const QString& reason);

private:
    bool m_autosaveOk = true;
    void autosaveFailedWith(const QString& reason);
    void setError(const QString& message);
    void adoptDocument(); // after the document changed file or content wholesale
    void onRevisionChanged();
    void startNewProject(const QList<QUrl>& firstMedia);
    void startNewProjectWith(const edit::SequenceSettings& settings);
    void startNewProjectFrom(ProjectState initial); // an untitled project with this content, not dirty, no history
    void rememberProject(); // the last project, for crash recovery at the next start
    void cleanExit();
    /// 0.2 (P.3): lets go of the lock held, then locks `project` (empty: none) when it is in a production or locking
    /// is on; read-only when another session holds it.
    void updateLock(const std::filesystem::path& project);

    ProjectController* m_controller = nullptr;
    std::unique_ptr<editor::ProjectDocument> m_document;
    QTimer m_autosaveTimer;
    int m_autosaveSeconds = 60;
    QString m_autosaveText;
    QString m_lastError;
    bool m_demo = true;
    bool m_exportingTimeline = false;
    /// The media items the last importFiles gave its media files, in the order given (new or already there).
    std::vector<MediaId> m_importedMedia;
    mutable QVariantList m_recovery;
    /// P.1: scanRecovery() runs on a worker (it used to parse the last project's autosaves and backups on the GUI
    /// thread at every start). Whatever reads the list collects the scan first, waiting only if the worker has not
    /// finished yet, so every caller sees what the synchronous scan gave.
    struct RecoveryScan {
        std::vector<editor::RecoveryFile> untitled;
        std::optional<editor::RecoveryFile> last;
        QString lastProject;
    };
    mutable std::future<RecoveryScan> m_recoveryScan;
    /// P.1: the autosave timer writes on a worker (prepareAutosave() here, AutosaveJob::write() there); one at a time.
    /// Saving, opening, recovering, a new project and a clean exit wait for it first (waitForAutosave()), so no
    /// autosave is written after they changed or removed the files.
    std::optional<editor::ProjectDocument::AutosaveJob> m_autosaveJob;
    std::future<std::expected<void, Error>> m_autosaveWrite;
    std::uint64_t m_autosaveSerial = 0;
    void collectAutosave();
    void waitForAutosave();
    std::uint64_t m_recoveryScanSerial = 0;
    /// Takes a finished (or waits for a running) scan into m_recovery; `queuedNotify`: announce a change later (from
    /// a getter), else now.
    void collectRecovery(bool queuedNotify) const;
    QVariantList m_versions;
    bool m_versionsScanning = false;
    std::uint64_t m_versionsScan = 0;
    /// Background scans report back only while this object lives.
    struct ScanGate {
        std::mutex mutex;
        ProjectLifecycle* owner = nullptr;
    };
    std::shared_ptr<ScanGate> m_scanGate = std::make_shared<ScanGate>();
    std::filesystem::path m_untitledAutosaveDir;
    bool m_lastDirty = false;
    int m_lastOfflineCount = 0;
    bool m_readOnly = false;
    QString m_lockOwner;
    std::filesystem::path m_locked; ///< the project this session holds the lock of (the active one)

    /// 0.2 (P.3): an open project. The active one is in the session, m_document and the members above (m_demo,
    /// m_readOnly, ...) and its slot only keeps its place in the order; the others are parked here.
    struct OpenProject {
        editor::ProjectDocument::Identity identity;
        EditSession::Parked parked;
        bool demo = false;
        bool readOnly = false;
        QString lockOwner;
        QString autosaveText;
        bool autosaveOk = true;
        std::filesystem::path locked;
        int sequenceIndex = 0;
        Ticks playhead;
    };
    std::vector<OpenProject> m_open = std::vector<OpenProject>(1);
    std::size_t m_active = 0;
    editor::ProjectDocumentOptions m_documentOptions;
    bool m_scratchCache = false; ///< the media cache is the production's (m_appMediaCache is the app's)
    QString m_appMediaCache;
    QString m_scratchKey; ///< the project and its scratch disks last announced (scratchDisksChanged)
    int indexOfOpen(const std::filesystem::path& file) const;
    void stashActive();                   // the active project's document and fields into its slot
    void takeActive(std::size_t index);   // a slot's document and fields become the active project's
    void showActive();                    // the controller shows the active project's file, sequence and playhead
    void releaseSlot(OpenProject& slot);  // lets go of a parked project's lock and autosaves (it is closed)
    static bool slotDirty(const OpenProject& slot);
};

} // namespace wabcut::app
