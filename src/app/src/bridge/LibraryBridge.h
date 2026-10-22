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
//   * 0.2 (P.3): Premiere's clip metadata (Scene, Shot, Take, Log Note, Good, Client, Tape Name) as list columns and in
//     the Metadata panel (setMetadata, one undo step), XMP read from and written next to the files (readXmp /
//     writeXmp / fileMetadata); Search Bins (createSearchBin from the Find bar, editSearchBin); the Freeform view
//     (view "freeform": places, stacks, clip size, Reset / Align to Grid, saved layouts; all saved in the project).
//
// Principle 9: QML sees keys ("b4" bins, "s2" sequences, "m7" media), display text and colour indices; times go in as
// timecode text or fractions.

#include "BridgeIds.h"
#include "LibraryModel.h"

#include <wabcut/command/Command.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
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

class ExportController;
class MediaReadinessBridge;
class ProjectController;
class SourceMonitorBridge;

class LibraryBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(wabcut::app::LibraryModel* items READ items CONSTANT FINAL)
    /// "grid", "list" or (0.2, P.3) "freeform".
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
    /// 0.2, Premiere's Project panel zoom (icon and thumbnail size): the grid's card size, 0.5 .. 2 (1 = the default),
    /// in kThumbnailScaleStep steps. Per user (QSettings), like the view and hover scrub.
    Q_PROPERTY(qreal thumbnailScale READ thumbnailScale WRITE setThumbnailScale NOTIFY thumbnailScaleChanged FINAL)

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
    /// 0.2 Media Browser Ingest (Premiere's Ingest Settings): with `ingest` on, importing from the Media Browser first
    /// "copy"-s the clips' files into `ingestFolder`, "transcode"-s them there with the export preset `ingestPreset`
    /// (the originals import at once; each item switches to its transcoded file when the export queue has made it: one
    /// undo step, "Use Transcoded Media"), or makes "proxies" of them (the proxy settings' format). Per user.
    Q_PROPERTY(bool ingest READ ingest WRITE setIngest NOTIFY ingestChanged FINAL)
    Q_PROPERTY(QString ingestAction READ ingestAction WRITE setIngestAction NOTIFY ingestChanged FINAL)
    /// A local folder ("/"-separated); a file URL is accepted.
    Q_PROPERTY(QString ingestFolder READ ingestFolder WRITE setIngestFolder NOTIFY ingestChanged FINAL)
    Q_PROPERTY(QString ingestPreset READ ingestPreset WRITE setIngestPreset NOTIFY ingestChanged FINAL)
    /// The frames of a Media Browser clip's hover-scrub strip (browseFolder's "strip": one image, side by side).
    Q_PROPERTY(int browserStripFrames READ browserStripFrames CONSTANT FINAL)

    // ---- 0.2 (P.3) ----
    /// The bin shown is a search bin, and its query ({ text, field, type, label, good, offline, generated, transcribed,
    /// proxy, unused, duplicates, from, to }; empty map otherwise).
    Q_PROPERTY(bool searchBinOpen READ searchBinOpen NOTIFY binChanged FINAL)
    Q_PROPERTY(QVariantMap currentSearch READ currentSearch NOTIFY binChanged FINAL)
    /// Where a search bin can look: [{ id ("" = all), title }].
    Q_PROPERTY(QVariantList searchFields READ searchFields CONSTANT FINAL)
    /// The Freeform view of the bin shown: key -> { x, y, size, z } for every listed item (items never placed get a
    /// place after the placed ones, in the list's order), in view units at clip size 1 (see freeformCellWidth).
    Q_PROPERTY(QVariantMap freeform READ freeform NOTIFY freeformChanged FINAL)
    /// Its clip size (Premiere's size slider), kFreeformMinSize .. kFreeformMaxSize; saved layouts' names.
    Q_PROPERTY(qreal freeformScale READ freeformScale NOTIFY freeformChanged FINAL)
    Q_PROPERTY(QStringList freeformLayouts READ freeformLayouts NOTIFY freeformChanged FINAL)
    /// The Freeform view can be arranged (a plain bin, nothing filtered).
    Q_PROPERTY(bool freeformAvailable READ freeformAvailable NOTIFY freeformChanged FINAL)
    /// Freeform View Options: the two metadata lines under each card (column ids, "" = none). Per user.
    Q_PROPERTY(QStringList freeformLines READ freeformLines WRITE setFreeformLines NOTIFY freeformLinesChanged FINAL)
    /// The grid cell and card size at clip size 1 (edit::kFreeformCellWidth / Height, kFreeformCardWidth).
    Q_PROPERTY(qreal freeformCellWidth READ freeformCellWidth CONSTANT FINAL)
    Q_PROPERTY(qreal freeformCellHeight READ freeformCellHeight CONSTANT FINAL)
    Q_PROPERTY(qreal freeformCardWidth READ freeformCardWidth CONSTANT FINAL)
    /// 0.2 (P.3) Preferences > Media: write XMP into MP4 / MOV files themselves (Premiere's "Write XMP ID to files"
    /// family) instead of a sidecar next to them. Off by default; per user.
    Q_PROPERTY(bool embedXmp READ embedXmp WRITE setEmbedXmp NOTIFY embedXmpChanged FINAL)

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
    /// Also gives the Source Monitor the importer of its Media Browser previews (importFromBrowser's).
    void setSourceMonitor(SourceMonitorBridge* source);
    /// The export queue Ingest > Transcode uses.
    void setExport(ExportController* exporter);
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
    bool ingest() const { return m_ingest; }
    void setIngest(bool on);
    QString ingestAction() const { return m_ingestAction; }
    void setIngestAction(const QString& action);
    QString ingestFolder() const { return m_ingestFolder; }
    void setIngestFolder(const QString& folder);
    QString ingestPreset() const { return m_ingestPreset; }
    void setIngestPreset(const QString& preset);
    static constexpr qreal kThumbnailScaleStep = 0.25;
    /// 0.2: the frames of a Media Browser clip's hover-scrub strip.
    static constexpr int kBrowserStripFrames = 10;
    int browserStripFrames() const { return kBrowserStripFrames; }
    qreal thumbnailScale() const { return m_thumbnailScale; }
    void setThumbnailScale(qreal scale);
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
    bool searchBinOpen() const;
    QVariantMap currentSearch() const;
    QVariantList searchFields() const;
    QVariantMap freeform() const { return m_freeform; }
    qreal freeformScale() const { return m_freeformScale; }
    QStringList freeformLayouts() const { return m_freeformLayouts; }
    bool freeformAvailable() const;
    QStringList freeformLines() const { return m_freeformLines; }
    void setFreeformLines(const QStringList& lines);
    qreal freeformCellWidth() const;
    qreal freeformCellHeight() const;
    qreal freeformCardWidth() const;

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
    /// 0.2, Premiere's Import with "Image Sequence" ticked: the numbered stills from `firstFrame` on as one clip at `fps`
    /// in the current bin (media.importImageSequence). Returns the new item's key, or empty with `message` set.
    Q_INVOKABLE QString importImageSequence(const QUrl& firstFrame, double fps);
    /// 0.2, Premiere's Import Layered File: whether `file` is a Photoshop file (.psd / .psb), what the dialog lists for
    /// it (media.layeredFileInfo: { width, height, depth, layers: [{ index, name, visible, hasPixels, group, groups }] },
    /// empty with `message` set when it cannot be read), and the import { as: "mergeAll" | "merged" | "layers" |
    /// "sequence", layers: [indices], footage: "document" | "layer" } into the current bin, one undo step. Returns the
    /// new item's key (the new bin's for layers and sequences), or empty with `message` set.
    Q_INVOKABLE bool isLayeredFile(const QUrl& file) const;
    Q_INVOKABLE QVariantMap layeredFileInfo(const QUrl& file);
    Q_INVOKABLE QString importLayeredFile(const QUrl& file, const QVariantMap& options);
    /// 0.2, the Media Browser: places to start from ([{ name, path }]: the drives, the home and video folders), the
    /// camera-aware listing of a folder (editor::browseFolder: [{ folder, name, path, card, files, spanned,
    /// audioChannels, bytes, thumbnail }]), the folder above one ("" at a drive's root), and importing clips by their
    /// first files (media.importCameraClips: spanned clips as one item, P2 as a merged clip) into the current bin, one
    /// undo step; with `copyTo` (a folder URL or path) the clips' files are copied there first (Ingest). Returns the
    /// first new item's key, or empty with `message` set.
    Q_INVOKABLE QVariantList browserPlaces() const;
    Q_INVOKABLE QVariantList browseFolder(const QString& folder);
    Q_INVOKABLE QString parentFolder(const QString& folder) const;
    Q_INVOKABLE QString importCameraClips(const QStringList& files, const QString& copyTo = {});
    /// The Media Browser's Import (and the first mark or edit of a clip it showed in the Source Monitor): as
    /// importCameraClips with the Ingest settings. An Ingest that copies or transcodes without a folder is refused.
    Q_INVOKABLE QString importFromBrowser(const QStringList& files);
    /// The Media Browser's metadata view of a clip (what probing its file reads; nothing is imported): { name, path,
    /// card, files, bytes, container, duration (timecode), seconds, still, startTimecode, video: { codec, profile,
    /// width, height, frameRate, variableFrameRate, pixelFormat, bitDepth, chroma, fieldOrder ("progressive", "upper",
    /// "lower"), alpha, rotation, frames } (no picture: absent), audio: [{ codec, channels, layout, sampleRate, title
    /// }] }; empty with `message` set when it is not media.
    Q_INVOKABLE QVariantMap browserFileInfo(const QString& file);

    // ---- Clip metadata and XMP (0.2, P.3) ----
    /// Sets one clip metadata field of media items: "comment", "description", "scene", "shot", "take", "logNote",
    /// "client", "tapeName" (text) or "good" (bool). One undo step.
    Q_INVOKABLE bool setMetadata(const QStringList& keys, const QString& field, const QVariant& value);
    /// The Metadata panel's Clip section for one media item: { key, name, kind, label, labelName, type, fps, resolution,
    /// audio, duration, scene, shot, take, logNote, client, good, tapeName, comment, description, path, offline }
    /// (empty map for anything else).
    Q_INVOKABLE QVariantMap clipMetadata(const QString& key) const;
    /// The File section: the XMP of its file and the tags its container has: { xmpSource ("sidecar" | "embedded" |
    /// ""), xmpFile, xmp: [{ group, label, name, value }], tags: [{ scope, key, value }], error }.
    Q_INVOKABLE QVariantMap fileMetadata(const QString& key) const;
    /// Fills clip metadata from the files' XMP (media.readXmp; `overwrite` replaces what is set). One undo step.
    /// Returns how many items had XMP.
    Q_INVOKABLE int readXmp(const QStringList& keys, bool overwrite = true);
    /// Writes the items' clip metadata into XMP sidecars next to their files (interchange::writeXmpSidecar; not a
    /// command: it writes where no parameter says), or into MP4 / MOV files themselves with embedXmp on. Returns how many.
    Q_INVOKABLE int writeXmp(const QStringList& keys);
    /// The File section's edit: XMP property `ns` / `prefix` / `property` of the item's file set to `value` ("" removes
    /// it): into the file itself with embedXmp on and an MP4 / MOV (a checked copy replaces it; a failure leaves it as it
    /// was), else into its sidecar. A linked property (Scene, Shot, Status, the camera fields, ...) sets the clip
    /// metadata too, in one undo step. False with `message` when it cannot be written.
    Q_INVOKABLE bool setFileMetadata(const QString& key, const QString& ns, const QString& prefix, const QString& property,
                                     const QString& value);
    /// The properties the File section offers to add: [{ ns, prefix, property, name ("xmpDM:scene"), label, group }].
    Q_INVOKABLE QVariantList xmpFieldChoices() const;
    bool embedXmp() const { return m_embedXmp; }
    void setEmbedXmp(bool on);

    // ---- Search Bins (0.2, P.3) ----
    /// Premiere's "Create new search bin" next to the Find field: a search bin in the current bin (the parent of an open
    /// search bin) finding what the Find text and filters find, named after the text. Returns its key.
    Q_INVOKABLE QString createSearchBin(const QString& name = {});
    /// A search bin with an explicit query ({ text, field, type, label, good, ... }). Returns its key.
    Q_INVOKABLE QString newSearchBin(const QString& name, const QVariantMap& search);
    /// Edit Search Bin: a new query for search bin `key`. One undo step.
    Q_INVOKABLE bool editSearchBin(const QString& key, const QVariantMap& search);
    /// The query of search bin `key` (empty map for a plain bin).
    Q_INVOKABLE QVariantMap searchOf(const QString& key) const;

    // ---- Freeform view (0.2, P.3; one undo step each) ----
    /// Moves items by (dx, dy) view units (a drag), bringing them to the front: a drop over other items stacks them.
    Q_INVOKABLE bool moveFreeform(const QStringList& keys, qreal dx, qreal dy);
    /// The items' own size (Premiere's per-clip size), kFreeformMinSize .. kFreeformMaxSize.
    Q_INVOKABLE bool setFreeformSize(const QStringList& keys, qreal size);
    /// The view's clip size (the size slider).
    Q_INVOKABLE bool setFreeformScale(qreal scale);
    /// Reset to Grid by "name", "label", "type", "duration" or "date" (bins, sequences and media keep their groups),
    /// `columns` per row (0: as many as fit `width` view pixels).
    Q_INVOKABLE bool freeformResetToGrid(const QString& by = QStringLiteral("name"), int columns = 0, qreal width = 0);
    /// Align to Grid: the selection (or everything when `selectionOnly` is false or nothing is selected).
    Q_INVOKABLE bool freeformAlignToGrid(bool selectionOnly = true);
    Q_INVOKABLE bool saveFreeformLayout(const QString& name);
    Q_INVOKABLE bool restoreFreeformLayout(const QString& name);
    Q_INVOKABLE bool removeFreeformLayout(const QString& name);
    /// Ctrl+wheel over a stack: the next (direction > 0) or previous item of the stack under `key` comes to the front.
    /// Returns the key now in front.
    Q_INVOKABLE QString cycleFreeformStack(const QString& key, int direction);
    /// The keys stacked with `key` (overlapping its card), back to front.
    Q_INVOKABLE QStringList freeformStackOf(const QString& key) const;
    /// The text of column `column` ("scene", "duration", ...) for a listed item (Freeform View Options' lines).
    Q_INVOKABLE QString cellText(const QString& key, const QString& column) const;

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
    /// After XMP was written into a media file: points the item at it again (its fingerprint changed). QML builds use
    /// ProjectFile.relink (forced); tests set their own.
    void setRelinker(std::function<bool(const QString& key, const std::filesystem::path& file)> relinker) {
        m_relinker = std::move(relinker);
    }
    /// Ingest > Transcode: the export queue finished `jobId` (ExportController::jobFinished); a transcode's item
    /// switches to the file it made.
    void transcodeFinished(const QString& jobId, bool succeeded, const QString& message);
    /// The transcodes waiting for the export queue (tests).
    std::size_t pendingTranscodes() const { return m_transcodes.size(); }

signals:
    void viewChanged();
    void scopeChanged();
    void columnsChanged();
    void sortChanged();
    void hoverScrubChanged();
    void thumbnailScaleChanged();
    void ingestChanged();
    void binChanged();
    void tabsChanged();
    void filterChanged();
    void selectionChanged();
    void statsChanged();
    void subclipSourceChanged();
    void messageChanged();
    void freeformChanged();
    void freeformLinesChanged();
    /// QML: start an inline rename of `key` (after New Bin).
    void renameRequested(const QString& key);
    /// QML: focus the Find field (Ctrl/Cmd+F).
    void findRequested();
    /// QML: ask for the subclip's name (Ctrl/Cmd+U).
    void makeSubclipRequested();
    /// QML: bring the Library forward and scroll `key` into view (showItem()).
    void itemShown(const QString& key);
    void embedXmpChanged();
    /// The XMP of `key`'s file changed (the File section reads it again).
    void fileMetadataChanged(const QString& key);

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
    /// Re-reads the Freeform view of the bin shown (positions of the listed items).
    void refreshFreeform();
    /// The bin a new item goes in: the bin shown, or the parent of a search bin shown.
    BinId targetBin() const;
    void refreshFingerprint(const QString& key, const std::filesystem::path& file);
    /// media.importCameraClips into the current bin: its "imported" entries, or nullopt with `message` set.
    std::optional<command::Json> runCameraImport(const QStringList& files, const QString& copyTo);
    /// runCameraImport with the Ingest settings.
    std::optional<command::Json> ingestClips(const QStringList& files);

    QPointer<ProjectController> m_controller;
    QPointer<MediaReadinessBridge> m_readiness;
    QPointer<SourceMonitorBridge> m_source;
    QPointer<ExportController> m_export;
    bool m_ingest = false;
    QString m_ingestAction = QStringLiteral("copy");
    QString m_ingestFolder;
    QString m_ingestPreset = QStringLiteral("prores-422-hq");
    struct PendingTranscode {
        MediaId media;
        std::filesystem::path output;
    };
    std::map<QString, PendingTranscode> m_transcodes; // export job id -> the item it transcodes
    LibraryModel* m_model = nullptr;
    QString m_view = QStringLiteral("grid");
    QString m_scope = QStringLiteral("all");
    bool m_hoverScrub = true;
    qreal m_thumbnailScale = 1.0;
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
    // 0.2 (P.3): the Freeform view of the bin shown.
    QVariantMap m_freeform;
    qreal m_freeformScale = 1.0;
    QStringList m_freeformLayouts;
    QStringList m_freeformLines;
    bool m_embedXmp = false;
    std::function<bool(const QString&, const std::filesystem::path&)> m_relinker;

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
