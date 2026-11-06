#pragma once

// ProductionBridge (0.2, P.3; QML singleton `Production`): Premiere's Productions and other open projects for the
// Production panel, over editor/Production.h.
//
//   * A production is a folder of projects (File > New > Production, or opening one): the panel lists its folders and
//     projects with who has each one open (project locks), makes new projects and folders, and adds existing projects
//     (a copy that still plays the same media files). Opening a project of a production opens its production too.
//   * Another project, open next to the one being edited (browse): its bins, sequences and media listed read-only;
//     items dragged or imported from it come into the project being edited as copies in one undo step, with what they
//     play, reusing media the project already has (edit::importProjectItems), never copying media files. A project
//     open in this session (ProjectFile.openProjects) is read as it is in memory, unsaved changes included. Editing it
//     opens it next to the others (ProjectFile.openAlongside): several projects stay open, one in the panels.
//   * Production Settings > Scratch Disks (settings / setSettings): the folders for its projects' previews and media
//     cache, shared by every project of the production.
//   * 0.7 (card 094) Search across the production: search() reads every project of the production on a worker thread
//     (a project open in this session as it is in memory, the others from their files, a file that did not change since
//     the last search not at all) and lists the sequences and footage that match, best first, with the project each is
//     in (editor/ProductionSearch.h); a newer search replaces an older one, cancelSearch() stops it. A result is
//     browsed (browseResult: the project read-only, the item pointed at; the project being edited shows it in the
//     Library) or imported as a copy (importResult), or its project opened (resultProject).
//   * Copies remember where they came from (MediaItem / Sequence importSource): Reveal in Source Project switches to
//     that project when it is open (else opens it next to this one, the item pointed at), and when the source project
//     relinked a media item to another file, the copies are offered the same relink (relinkOffers). Copies stay copies:
//     a project never reads another one live, so it opens, saves and plays on its own whatever happens to the other.
//
// Principle 9: QML sees file URLs, the browsed project's item keys ("b4", "s3", "m7" of that project) and display text.

#include <wabcut/editor/ProductionSearch.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <expected>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectLifecycle;

class ProductionBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Production)
    QML_SINGLETON

    Q_PROPERTY(bool open READ isOpen NOTIFY productionChanged FINAL)
    Q_PROPERTY(QString name READ name NOTIFY productionChanged FINAL)
    /// The production folder (native separators), and as a URL for file dialogs.
    Q_PROPERTY(QString folder READ folder NOTIFY productionChanged FINAL)
    Q_PROPERTY(QUrl folderUrl READ folderUrl NOTIFY productionChanged FINAL)
    /// Its folders and projects, depth first: [{ url, relative, name, folder, depth, locked (by another session),
    /// lockOwner, current (the project being edited), browsing }].
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged FINAL)
    /// The project open next to the one being edited (empty URL: none), its name and items: [{ key, kind ("bin",
    /// "sequence", "video", "audio", "still", "title", "subclip"), name, depth, meta }] (bins with their contents
    /// indented below them).
    Q_PROPERTY(QUrl browsing READ browsing NOTIFY browsingChanged FINAL)
    Q_PROPERTY(QString browsingName READ browsingName NOTIFY browsingChanged FINAL)
    Q_PROPERTY(QVariantList browseItems READ browseItems NOTIFY browsingChanged FINAL)
    /// The last result or problem, for a toast.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// Production Settings > Scratch Disks: { previews, mediaCache } as set ("" = the default; relative to the
    /// production folder or absolute) and as shown: { previewsText, mediaCacheText } (the folder, or the default).
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY productionChanged FINAL)
    /// The browsed item Reveal in Source Project points at ("m7", "s3" of the browsed project; "" = none).
    Q_PROPERTY(QString revealedKey READ revealedKey NOTIFY browsingChanged FINAL)
    /// 0.7 (card 094): a search is reading the production's projects; how far (0..1, -1 unknown) and what it reads.
    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged FINAL)
    Q_PROPERTY(double searchProgress READ searchProgress NOTIFY searchChanged FINAL)
    Q_PROPERTY(QString searchStatus READ searchStatus NOTIFY searchChanged FINAL)
    /// The last search's results, best first: [{ key ("m7" / "s3" in its project), kind ("sequence", "video", "audio",
    /// "still", "title", "subclip"), name, location (its bins), meta (size and length), matched (the fields the words were
    /// found in, as words), project (url), projectName, projectRelative, projectOpen (open in this session), current (the
    /// project being edited), offline }].
    Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchChanged FINAL)
    /// "12 results in 3 projects" / "No results"; empty before the first search.
    Q_PROPERTY(QString searchSummary READ searchSummary NOTIFY searchChanged FINAL)
    /// Whether a search has been run and its results (maybe none) are shown.
    Q_PROPERTY(bool searched READ searched NOTIFY searchChanged FINAL)

public:
    explicit ProductionBridge(ProjectLifecycle* lifecycle, QObject* parent = nullptr);
    ~ProductionBridge() override;
    static ProductionBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    bool isOpen() const { return !m_folder.empty(); }
    QString name() const { return m_name; }
    QString folder() const;
    QUrl folderUrl() const;
    QVariantList entries() const { return m_entries; }
    QUrl browsing() const;
    QString browsingName() const;
    QVariantList browseItems() const { return m_browseItems; }
    QString message() const { return m_message; }
    QVariantMap settings() const { return m_settings; }
    QString revealedKey() const { return m_revealedKey; }
    bool searching() const { return m_searching; }
    double searchProgress() const { return m_searchProgress; }
    QString searchStatus() const { return m_searchStatus; }
    QVariantList searchResults() const { return m_searchResults; }
    QString searchSummary() const { return m_searchSummary; }
    bool searched() const { return m_searched; }

    // ---- Search across the production (0.7, card 094) ----
    /// Finds sequences and footage in any project of the open production: `text` (words; "quoted words" are a phrase),
    /// narrowed to one `field` ("" = every field; "name", "comment", "description", "path", "tapeName", "logNote", ...),
    /// one `type` ("sequence", "video", "audio", "still", "title", "subclip") and one `project` (its relative path). No
    /// text with a type lists everything of that type; no text and no type clears the results. Returns at once: the
    /// projects are read on a worker thread (searching, searchProgress), the results arrive in searchResults. False with
    /// a message when there is no production or the query is not valid.
    Q_INVOKABLE bool search(const QString& text, const QString& field = {}, const QString& type = {}, const QString& project = {});
    /// Stops the search that is running; the earlier results stay.
    Q_INVOKABLE void cancelSearch();
    /// Forgets the results (the next search starts from the projects' files, not from what was read).
    Q_INVOKABLE void clearSearch();
    /// Opens the project of result `index` read-only next to the one being edited and points at the item (revealedKey);
    /// a result in the project being edited is shown in the Library. False when the result or project is gone.
    Q_INVOKABLE bool browseResult(int index);
    /// Imports the item of result `index` into the project being edited, as a copy that still plays the same files, in one
    /// undo step (importItems' result; { ok: false } when it could not).
    Q_INVOKABLE QVariantMap importResult(int index);
    /// The project of result `index` (for Open Project); an empty URL when there is no such result.
    Q_INVOKABLE QUrl resultProject(int index) const;
    /// Tests: run the search on the calling thread.
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }

    // ---- Productions ----
    /// File > New > Production: makes `folder` a production named `name` and opens it.
    Q_INVOKABLE bool makeProduction(const QUrl& folder, const QString& name = {});
    /// Opens the production in `folder` (its folder or its production file).
    Q_INVOKABLE bool openProduction(const QUrl& folder);
    Q_INVOKABLE void closeProduction();
    /// Lists the production again (projects added on disk, locks taken or released).
    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool rename(const QString& name);
    /// A new project `name` in folder `relative` ("" = the top) with one empty sequence; returns its URL (QML opens it).
    Q_INVOKABLE QUrl newProject(const QString& relative, const QString& name);
    Q_INVOKABLE bool newFolder(const QString& relative, const QString& name);
    /// Add Project to Production: a copy of `project` in folder `relative`; returns the copy's URL.
    Q_INVOKABLE QUrl addProject(const QUrl& project, const QString& relative = {});
    /// Saves Production Settings: `previews` / `mediaCache` a folder (URL or path; "" = the default); a folder inside
    /// the production is kept relative to it. Keys left out stay. The project being edited uses them at once.
    Q_INVOKABLE bool setSettings(const QVariantMap& settings);

    // ---- Another project, open next to this one ----
    /// Opens `project` read-only next to the one being edited (any project file, in the production or not).
    Q_INVOKABLE bool browse(const QUrl& project);
    Q_INVOKABLE void stopBrowsing();
    /// Imports items of the browsed project (`keys` of browseItems; a bin brings everything in it) into the project
    /// being edited, in the Library's current bin or a new bin `binName`. One undo step. Returns { ok, sequences,
    /// media, bins, reusedMedia, notes } (counts and notes).
    Q_INVOKABLE QVariantMap importItems(const QStringList& keys, const QString& binName = {});
    /// Premiere's import of a whole project: everything of `project` in a new bin named after it. One undo step.
    Q_INVOKABLE QVariantMap importProject(const QUrl& project);

    // ---- Where imported items came from ----
    /// Where the Library item `key` ("m7", "s3") was imported from: { project (url), projectName, key (its key there),
    /// open (in this session), exists (the file) }; empty when it was not imported from a project.
    Q_INVOKABLE QVariantMap sourceOf(const QString& key) const;
    /// Premiere's Reveal in Project for a copy: switches to its source project when that is open and shows the item in
    /// the Library; else opens the source next to this one, read-only, the item pointed at (revealedKey). Returns
    /// { ok, switched, project, key }.
    Q_INVOKABLE QVariantMap revealSource(const QString& key);
    /// Copies whose source item now plays another file (relinked in the source project), or plays a file while the
    /// copy is offline: [{ key, name, from, to, project, offline }].
    Q_INVOKABLE QVariantList relinkOffers();
    /// Relinks the copies `keys` (of relinkOffers; empty = all) to their source items' files, in one undo step.
    /// Returns how many were relinked.
    Q_INVOKABLE int applyRelinkOffers(const QStringList& keys = {});

    // ---- C++ / tests ----
    const std::filesystem::path& productionFolder() const { return m_folder; }
    /// The Library's current bin for imports (tests set it; QML builds read Library).
    void setImportBin(std::function<BinId()> bin) { m_importBin = std::move(bin); }
    /// Shows an item in the Library after Reveal in Source Project switched projects (QML builds use Library).
    void setShowItem(std::function<void(const QString&)> show) { m_showItem = std::move(show); }

signals:
    void productionChanged();
    void entriesChanged();
    void browsingChanged();
    void messageChanged();
    void searchChanged();

private:
    void startPendingSearch();
    void onSearchProgress(std::uint64_t generation, int done, int total, const QString& current);
    void onSearchDone(std::uint64_t generation, std::shared_ptr<const editor::ProductionIndex> index, editor::SearchResult result,
                      bool stopped);
    void resetSearch();
    QVariantMap resultRow(const editor::SearchHit& hit) const;

    void setMessage(const QString& text);
    /// Opens the production of the project being edited, when it lies in one.
    void followDocument();
    void listBrowsed();
    void loadSettings();
    std::filesystem::path currentFile() const;
    /// Another project for browsing and import: the one open in this session as it is now, else read from its file.
    std::expected<ProjectState, Error> sourceState(const std::filesystem::path& project) const;
    QVariantMap runImport(const ProjectState& source, const std::filesystem::path& sourceFile, const QStringList& keys,
                          bool everything, const QString& binName);

    QPointer<ProjectLifecycle> m_lifecycle;
    std::filesystem::path m_folder;
    QString m_name;
    QVariantList m_entries;
    std::filesystem::path m_browsingFile;
    std::optional<ProjectState> m_browsed;
    QVariantList m_browseItems;
    QString m_message;
    std::function<BinId()> m_importBin;
    std::function<void(const QString&)> m_showItem;
    QVariantMap m_settings;
    QString m_revealedKey;

    // Search across the production (card 094).
    bool m_synchronous = false;
    bool m_searching = false;
    bool m_searched = false;
    double m_searchProgress = -1;
    QString m_searchStatus;
    QString m_searchSummary;
    QVariantList m_searchResults;
    std::vector<editor::SearchHit> m_hits;                    ///< m_searchResults' rows, for the actions
    std::shared_ptr<const editor::ProductionIndex> m_searchIndex; ///< what the last scan read (reused while files stand)
    std::uint64_t m_searchGeneration = 0;
    bool m_scanRunning = false;       ///< a worker is reading (it posts onSearchDone exactly once)
    std::optional<editor::SearchQuery> m_pendingSearch; ///< the newest query, started when the worker ended
    std::jthread m_searchThread;
};

} // namespace wabcut::app
