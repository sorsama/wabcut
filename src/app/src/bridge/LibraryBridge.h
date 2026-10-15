#pragma once

// LibraryBridge (M5k, QML singleton `Library`): Premiere's Project panel for Wabcut's Library.
//
//   * Bins: nested bins (create, rename, delete with their contents, move by drag), New Bin from Selection, a bin
//     opened in place (the breadcrumb walks back up) or as a tab of its own.
//   * Views: the grid and the list (LibraryModel: one row per bin, sequence or media item) with sortable, resizable,
//     reorderable and chosen columns, saved in the settings. 10,000 items scroll without per-item work beyond the
//     rows in view (the list is a TableView over the model).
//   * Metadata as undoable edits: rename, label colour, comment, description, poster frame (ProjectOps.h
//     setMediaAttributes; one undo step each, labelled).
//   * Subclips: Make Subclip (Ctrl/Cmd+U) from the Source Monitor's marks, restricted (Premiere's default) or not;
//     Edit Subclip; they list with their range.
//   * Find (Ctrl/Cmd+F): text over names, comments, descriptions and paths plus filters (type, label, transcribed,
//     offline, proxy, AI-generated, unused, duplicates, date). Active filters list the whole project. The Smart toggle
//     (AI-8) keeps working: its results are narrowed by the same filters (passesFilters()).
//   * Housekeeping: unused media (in no sequence) with Remove Unused (one undo step), duplicate media (same file
//     content: SourceFingerprint::sameContentAs), Reveal in File Explorer / Finder.
//   * Status columns that do not live in the model: proxy state (MediaReadiness), transcript / search index / shot
//     analysis (a background scan of the analysis cache and the smart-search index; never on the GUI thread).
//
// Principle 9: QML sees keys ("b4" bins, "s2" sequences, "m7" media), display text and colour indices; times go in as
// timecode text or fractions.

#include "BridgeIds.h"
#include "LibraryModel.h"

#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class MediaReadinessBridge;
class ProjectController;
class SourceMonitorBridge;

class LibraryBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(wabcut::app::LibraryModel* items READ items CONSTANT FINAL)
    /// "grid" or "list".
    Q_PROPERTY(QString view READ view WRITE setView NOTIFY viewChanged FINAL)
    /// Media / Audio scope of the grid and list (the panel's tabs): "all" or "audio" (bins always show).
    Q_PROPERTY(QString scope READ scope WRITE setScope NOTIFY scopeChanged FINAL)
    /// Visible columns in order: [{ id, title, width, editable }].
    Q_PROPERTY(QVariantList columns READ columns NOTIFY columnsChanged FINAL)
    /// Every column for the chooser: [{ id, title, visible }], in the default order.
    Q_PROPERTY(QVariantList allColumns READ allColumns NOTIFY columnsChanged FINAL)
    Q_PROPERTY(QString sortColumn READ sortColumn NOTIFY sortChanged FINAL)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged FINAL)
    /// Hover scrub on grid cards (Premiere's Shift+H).
    Q_PROPERTY(bool hoverScrub READ hoverScrub WRITE setHoverScrub NOTIFY hoverScrubChanged FINAL)

    /// The bin shown ("" = the project's root).
    Q_PROPERTY(QString currentBin READ currentBin NOTIFY binChanged FINAL)
    /// The path to it, root first: [{ key ("" = root), name }].
    Q_PROPERTY(QVariantList breadcrumb READ breadcrumb NOTIFY binChanged FINAL)
    /// Bins opened as tabs (the project root first, always): [{ key, name, current }].
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged FINAL)

    Q_PROPERTY(QString findText READ findText WRITE setFindText NOTIFY filterChanged FINAL)
    /// { type, label (-1 = any), transcribed, offline, proxy, generated, unused, duplicates, from, to }. Writing it
    /// replaces every filter (a key left out is off); the Find text is not part of it.
    Q_PROPERTY(QVariantMap filters READ filters WRITE setFilters NOTIFY filterChanged FINAL)
    /// Find text or a filter is active: the whole project is listed.
    Q_PROPERTY(bool filtering READ filtering NOTIFY filterChanged FINAL)
    /// Filters set (not counting the text), for the filter button's badge.
    Q_PROPERTY(int filterCount READ filterCount NOTIFY filterChanged FINAL)

    Q_PROPERTY(QStringList selection READ selection NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged FINAL)
    /// The selection holds media (label, poster frame, subclip and reveal act on it).
    Q_PROPERTY(bool mediaSelected READ mediaSelected NOTIFY selectionChanged FINAL)

    Q_PROPERTY(int unusedCount READ unusedCount NOTIFY statsChanged FINAL)
    /// Groups of media items that share a file's content: [{ name, items: [{ key, name, binPath }] }].
    Q_PROPERTY(QVariantList duplicateGroups READ duplicateGroups NOTIFY statsChanged FINAL)
    /// Make Subclip can run: the Source Monitor shows media with an In or Out mark.
    Q_PROPERTY(bool canMakeSubclip READ canMakeSubclip NOTIFY subclipSourceChanged FINAL)
    /// The last result or problem, for a toast ("" after it was shown).
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// Premiere's 16 label names, by colour index (the Label menu).
    Q_PROPERTY(QStringList labelNames READ labelNames CONSTANT FINAL)

public:
    enum SelectMode { Replace = 0, Add = 1, Toggle = 2, Range = 3 };
    Q_ENUM(SelectMode)

    /// What the analysis columns need to know of one media item, from a background thread. Tests replace it.
    using AnalysisProbe = std::function<LibraryMediaStatus(const MediaItem& item, const std::filesystem::path& file)>;
    /// How Reveal in File Explorer shows a file (tests replace it; by default Explorer, Finder or the folder).
    using Revealer = std::function<bool(const QString& path)>;
    static void setRevealer(Revealer revealer);

    explicit LibraryBridge(QObject* parent = nullptr);
    ~LibraryBridge() override;
    static LibraryBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setReadiness(MediaReadinessBridge* readiness);
    void setSourceMonitor(SourceMonitorBridge* source);
    /// The analysis scan (default: the analysis cache and the smart-search index). Null turns the scan off.
    void setAnalysisProbe(AnalysisProbe probe);
    /// Runs the analysis scan on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    /// Remembers view, columns and sort in the settings (off in tests that need a clean state).
    void setPersistent(bool persistent) { m_persistent = persistent; }

    LibraryModel* items() const { return m_model; }
    QString view() const { return m_view; }
    void setView(const QString& view);
    QString scope() const { return m_scope; }
    void setScope(const QString& scope);
    QVariantList columns() const;
    QVariantList allColumns() const;
    QString sortColumn() const;
    bool sortAscending() const { return m_model->sortAscending(); }
    bool hoverScrub() const { return m_hoverScrub; }
    void setHoverScrub(bool on);
    QString currentBin() const { return binKey(m_bin); }
    QVariantList breadcrumb() const;
    QVariantList tabs() const;
    QString findText() const { return m_filter.text; }
    void setFindText(const QString& text);
    QVariantMap filters() const;
    void setFilters(const QVariantMap& filters);
    bool filtering() const { return m_filter.active(); }
    int filterCount() const;
    QStringList selection() const;
    int selectionCount() const { return static_cast<int>(m_selection.size()); }
    bool mediaSelected() const;
    int unusedCount() const { return static_cast<int>(m_unused.size()); }
    QVariantList duplicateGroups() const { return m_duplicateGroups; }
    bool canMakeSubclip() const;
    QString message() const { return m_message; }
    QStringList labelNames() const;

    // ---- Bins ----
    Q_INVOKABLE void openBin(const QString& key);
    Q_INVOKABLE void openBinInTab(const QString& key);
    Q_INVOKABLE void closeTab(const QString& key);
    Q_INVOKABLE void goUp();
    /// Shows an item ("b4", "s3", "m7"): opens the bin it is in (leaving Find and the Audio tab) and selects it
    /// (M5l: Reveal nested sequence in Library, a new multi-camera source).
    Q_INVOKABLE bool showItem(const QString& key);
    /// New Bin (Ctrl/Cmd+B) in the current bin: "Bin 01", "Bin 02", ...; returns its key ("" on failure).
    Q_INVOKABLE QString newBin(const QString& name = {});
    /// New Bin from Selection: a new bin holding the selected items (one undo step).
    Q_INVOKABLE QString newBinFromSelection();
    /// Moves items (bins, sequences, media) into a bin ("" = root): one undo step.
    Q_INVOKABLE bool moveItems(const QStringList& keys, const QString& bin);

    // ---- Metadata (one undo step each) ----
    /// Renames a bin or a media item.
    Q_INVOKABLE bool rename(const QString& key, const QString& name);
    Q_INVOKABLE bool setLabel(const QStringList& keys, int colorIndex);
    Q_INVOKABLE bool setComment(const QString& key, const QString& text);
    Q_INVOKABLE bool setDescription(const QString& key, const QString& text);
    /// The poster frame at `fraction` of the item (its subclip range for a subclip), or cleared (fraction < 0).
    Q_INVOKABLE bool setPosterFrame(const QString& key, double fraction);
    /// Deletes items: bins with their contents, media not used by a sequence, sequences. One undo step.
    Q_INVOKABLE bool deleteItems(const QStringList& keys);
    Q_INVOKABLE bool deleteSelection();
    /// The details of one item for the editing popovers: { key, kind, name, comment, description, label, inText,
    /// outText, restricted, master, posterText, path, offline } (empty map when it does not exist).
    Q_INVOKABLE QVariantMap details(const QString& key) const;

    // ---- Subclips ----
    /// Make Subclip (Ctrl/Cmd+U): the Source Monitor's media between its marks, in the current bin. Returns its key.
    Q_INVOKABLE QString makeSubclip(const QString& name = {}, bool restricted = true);
    /// Make Subclip of a library item from its stored marks.
    Q_INVOKABLE QString makeSubclipOf(const QString& media, const QString& name = {}, bool restricted = true);
    /// Edit Subclip: new In / Out (timecode in the media's rate; Out = its last frame) and restriction.
    Q_INVOKABLE bool editSubclip(const QString& key, const QString& inText, const QString& outText, bool restricted);

    // ---- Housekeeping ----
    Q_INVOKABLE bool reveal(const QString& key);
    Q_INVOKABLE void selectUnused();
    /// Removes every unused media item: one undo step. Returns how many went.
    Q_INVOKABLE int removeUnused();
    /// Whether a media item passes the Find filters (narrows Smart results).
    Q_INVOKABLE bool passesFilters(const QString& mediaKey) const;
    Q_INVOKABLE void clearFilters();

    // ---- Selection ----
    Q_INVOKABLE void select(const QString& key, int mode = Replace);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isSelected(const QString& key) const { return m_selection.contains(key); }

    // ---- Columns ----
    Q_INVOKABLE void sortBy(const QString& column);
    Q_INVOKABLE void setColumnVisible(const QString& column, bool visible);
    Q_INVOKABLE void moveColumn(int from, int to);
    Q_INVOKABLE void setColumnWidth(const QString& column, int width);
    Q_INVOKABLE void resetColumns();

    // ---- Keyboard (docs/keymap.md: project.*, clip.makeSubclip) ----
    static bool handlesCommand(const QString& id);
    /// { handled, enabled, reason } for the command palette.
    Q_INVOKABLE QVariantMap commandAvailability(const QString& id) const;
    Q_INVOKABLE bool runCommand(const QString& id);

    // ---- C++ / tests ----
    /// Media in no sequence (subclips and generator items included).
    const std::unordered_set<std::uint64_t>& unused() const { return m_unused; }
    /// Media sharing a file's content with another item.
    const std::unordered_set<std::uint64_t>& duplicates() const { return m_duplicates; }
    const std::unordered_map<std::uint64_t, LibraryMediaStatus>& status() const { return m_status; }
    BinId bin() const { return m_bin; }
    /// Waits for the analysis scan (tests).
    bool waitForScan(int timeoutMs);

signals:
    void viewChanged();
    void scopeChanged();
    void columnsChanged();
    void sortChanged();
    void hoverScrubChanged();
    void binChanged();
    void tabsChanged();
    void filterChanged();
    void selectionChanged();
    void statsChanged();
    void subclipSourceChanged();
    void messageChanged();
    /// QML: start an inline rename of `key` (after New Bin).
    void renameRequested(const QString& key);
    /// QML: focus the Find field (Ctrl/Cmd+F).
    void findRequested();
    /// QML: ask for the subclip's name (Ctrl/Cmd+U).
    void makeSubclipRequested();
    /// QML: bring the Library forward and scroll `key` into view (showItem()).
    void itemShown(const QString& key);

private:
    /// Re-lists the items when what the panel shows changed (or always with `force`).
    void refresh(bool force = false);
    void recomputeStats(const ProjectState& state);
    void refreshStatus();
    void startScan(ProjectSnapshot snapshot);
    void setMessage(const QString& text);
    bool apply(const char* label, const std::function<std::expected<ProjectState, Error>(const ProjectState&)>& fn);
    QString thumbnailFor(const MediaItem& item) const;
    /// Whether `sequence` has anything to draw (a graphic, a generator, or a video clip of media with a picture on disk;
    /// nested sequences looked into): only then does its card ask for a rendered thumbnail.
    bool sequenceHasPicture(const ProjectState& state, SequenceId sequence, std::unordered_set<std::uint64_t>& seen) const;
    void loadSettings();
    void saveSettings() const;
    void applyColumns();
    void pruneSelection();
    std::vector<MediaId> mediaOf(const QStringList& keys) const;

    QPointer<ProjectController> m_controller;
    QPointer<MediaReadinessBridge> m_readiness;
    QPointer<SourceMonitorBridge> m_source;
    LibraryModel* m_model = nullptr;
    QString m_view = QStringLiteral("grid");
    QString m_scope = QStringLiteral("all");
    bool m_hoverScrub = true;
    bool m_persistent = true;
    bool m_synchronous = false;
    BinId m_bin;
    std::vector<BinId> m_tabs; // opened as tabs (the root tab is implicit)
    LibraryFilter m_filter;
    QSet<QString> m_selection;
    QString m_anchor; // for Range selection
    struct ColumnState {
        LibraryColumn column;
        bool visible = true;
        int width = 120;
    };
    std::vector<ColumnState> m_columns; // display order
    std::unordered_set<std::uint64_t> m_used;
    std::unordered_set<std::uint64_t> m_unused;
    std::unordered_map<std::uint64_t, QString> m_thumbnails; // media id -> thumbnail URL (ProjectController's)
    std::unordered_map<std::uint64_t, QString> m_paths;      // media id -> file path (poster frames)
    std::unordered_set<std::uint64_t> m_duplicates;
    QVariantList m_duplicateGroups;
    std::unordered_map<std::uint64_t, LibraryMediaStatus> m_status;
    std::unordered_map<std::uint64_t, LibraryMediaStatus> m_analysis; // from the scan
    QString m_message;
    ProjectSnapshot m_lastSnapshot;

    // Analysis scan: one job at a time; a newer snapshot cancels the running one.
    AnalysisProbe m_probe;
    std::shared_ptr<std::atomic<std::uint64_t>> m_scanGeneration;
    std::jthread m_scan;
    std::atomic<bool> m_scanning{false};
};

/// QML registration through create().
struct LibraryBridgeForeign {
    Q_GADGET
    QML_FOREIGN(LibraryBridge)
    QML_NAMED_ELEMENT(Library)
    QML_SINGLETON
public:
    static LibraryBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return LibraryBridge::create(qmlEngine, jsEngine); }
};

/// The default analysis probe: a transcript and shot analysis in the analysis cache, an entry in the smart-search
/// index (opened through `searchIndexOf`, may be null). Runs on a worker thread.
LibraryBridge::AnalysisProbe defaultAnalysisProbe(std::function<bool(std::uint64_t fingerprint)> indexed);

} // namespace wabcut::app
