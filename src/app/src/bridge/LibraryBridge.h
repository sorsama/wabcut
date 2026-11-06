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
//   * P1 round (0.6): Move to Bin (binTree), Cut and Paste of items between bins (cutItems / pasteItems), proxies for
//     the selection or a whole bin (proxyPlan, createProxies...), the File Size and Data Rate columns with the footer's
//     summaryText, and Get Properties (mediaProperties / propertyLines / copyProperties, file.getProperties).
//   * P1 round (0.6, stream 2D): usage (#40: the Video / Audio Usage columns, usages() and Find in Timeline), deleting
//     items whose media sequences use (#38: deleteImpact, a confirmation, then their clips go too in the same undo
//     step; Ctrl/Cmd+Delete is Delete with Options), and the list view's twirl-down bins (#46: setExpanded, kept per
//     project in the settings).
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
#include <span>
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
    /// 0.7 (card 023), Premiere's Project panel List view > Thumbnails: the list's Name cell shows the item's thumbnail and
    /// the rows grow with thumbnailScale (the size slider, Ctrl+wheel and the zoom keys then size the list too). Saved
    /// (QSettings library/listThumbnails); off by default.
    Q_PROPERTY(bool listThumbnails READ listThumbnails WRITE setListThumbnails NOTIFY listThumbnailsChanged FINAL)
    /// 0.2, Premiere's Project panel zoom (icon and thumbnail size): the grid's card size, 0.5 .. 2 (1 = the default),
    /// in kThumbnailScaleStep steps. Per user (QSettings), like the view and hover scrub.
    Q_PROPERTY(qreal thumbnailScale READ thumbnailScale WRITE setThumbnailScale NOTIFY thumbnailScaleChanged FINAL)
    /// UX rework (L): the bounds and step of thumbnailScale, for the toolbar's size slider (0.5, 2, 0.25).
    Q_PROPERTY(qreal thumbnailScaleMin READ thumbnailScaleMin CONSTANT FINAL)
    Q_PROPERTY(qreal thumbnailScaleMax READ thumbnailScaleMax CONSTANT FINAL)
    Q_PROPERTY(qreal thumbnailScaleStep READ thumbnailScaleStep CONSTANT FINAL)

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
    /// undo step, "Use Transcoded Media"), makes "proxies" of them (the proxy settings' format), or (0.7, card 022)
    /// "copyProxies": copies the files into `ingestFolder` and makes the proxies of the copies. Per user.
    Q_PROPERTY(bool ingest READ ingest WRITE setIngest NOTIFY ingestChanged FINAL)
    Q_PROPERTY(QString ingestAction READ ingestAction WRITE setIngestAction NOTIFY ingestChanged FINAL)
    /// A local folder ("/"-separated); a file URL is accepted.
    Q_PROPERTY(QString ingestFolder READ ingestFolder WRITE setIngestFolder NOTIFY ingestChanged FINAL)
    Q_PROPERTY(QString ingestPreset READ ingestPreset WRITE setIngestPreset NOTIFY ingestChanged FINAL)
    /// 0.7 (card 021): a cell of the list view is open for editing (the list sets it). While it is, Tab / Shift+Tab and
    /// Enter / Shift+Enter (project.nextColumnField, previousColumnField, nextRowField, previousRowField) move to the
    /// next or previous cell: runCommand emits fieldStepRequested and the list view answers.
    Q_PROPERTY(bool fieldEditing READ fieldEditing WRITE setFieldEditing NOTIFY fieldEditingChanged FINAL)
    /// 0.7 (card 144): named column layouts (set, order and widths of the list view's columns), by name.
    Q_PROPERTY(QStringList columnPresets READ columnPresets NOTIFY columnPresetsChanged FINAL)
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

    // ---- P1 round (0.6) ----
    /// #41: the items Cut marked (Ctrl/Cmd+X in the Library): they dim until Paste moves them or Escape forgets them.
    Q_PROPERTY(QStringList cutKeys READ cutKeys NOTIFY cutChanged FINAL)
    /// #45: the footer line, "48 items · 3 selected · 12.4 GB · 1:22:10": what is listed, how much is selected, and the
    /// file size and length of the selection (with what its bins hold), or of everything listed when nothing is.
    Q_PROPERTY(QString summaryText READ summaryText NOTIFY summaryChanged FINAL)

public:
    enum SelectMode { Replace = 0, Add = 1, Toggle = 2, Range = 3 };
    Q_ENUM(SelectMode)

    /// What the analysis columns need to know of one media item, from a background thread. Tests replace it.
    using AnalysisProbe = std::function<LibraryMediaStatus(const MediaItem& item, const std::filesystem::path& file)>;
    /// How Reveal in File Explorer shows a file (tests replace it; by default Explorer, Finder or the folder).
    using Revealer = std::function<bool(const QString& path)>;
    static void setRevealer(Revealer revealer);
    /// UX rework (H): shows `path` in the file manager the way Reveal in File Explorer does (the Revealer tests set, else
    /// Explorer /select, Finder -R or the folder). Home's Show in Explorer uses it too.
    static bool revealPath(const QString& path);

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
    bool listThumbnails() const { return m_listThumbnails; }
    void setListThumbnails(bool on);
    bool ingest() const { return m_ingest; }
    void setIngest(bool on);
    QString ingestAction() const { return m_ingestAction; }
    void setIngestAction(const QString& action);
    QString ingestFolder() const { return m_ingestFolder; }
    void setIngestFolder(const QString& folder);
    QString ingestPreset() const { return m_ingestPreset; }
    void setIngestPreset(const QString& preset);
    bool fieldEditing() const { return m_fieldEditing; }
    void setFieldEditing(bool editing);
    QStringList columnPresets() const;
    static constexpr qreal kThumbnailScaleStep = 0.25;
    static constexpr qreal kThumbnailScaleMin = 0.5;
    static constexpr qreal kThumbnailScaleMax = 2.0;
    /// 0.2: the frames of a Media Browser clip's hover-scrub strip.
    static constexpr int kBrowserStripFrames = 10;
    int browserStripFrames() const { return kBrowserStripFrames; }
    qreal thumbnailScale() const { return m_thumbnailScale; }
    void setThumbnailScale(qreal scale);
    qreal thumbnailScaleMin() const { return kThumbnailScaleMin; }
    qreal thumbnailScaleMax() const { return kThumbnailScaleMax; }
    qreal thumbnailScaleStep() const { return kThumbnailScaleStep; }
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
    QStringList cutKeys() const;
    QString summaryText() const;

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

    // ---- Move to Bin, Cut and Paste (P1 round, #41) ----
    /// The bins Move to Bin offers, depth first and by name: [{ key ("" = the project's root), name, depth (0 = the
    /// root), disabled, reason }]. A bin is disabled when it is one of `moving` or inside one of them (a bin cannot move
    /// into itself) or a search bin (it lists what it finds); `reason` says why. Fix round 1 (ARGON-22): the bin every one
    /// of `moving` is in already is disabled too, with `current` set ("Already in this bin").
    Q_INVOKABLE QVariantList binTree(const QStringList& moving = {}) const;
    /// Cut: marks `keys` (bins, sequences, media) for the next Paste; nothing moves yet. False for nothing to mark.
    Q_INVOKABLE bool cutItems(const QStringList& keys);
    /// Paste: moves the cut items into pasteTarget() in one undo step ("Move to Bin") and forgets the marks. False with
    /// `message` when nothing is cut or the move is refused (a bin into itself, a search bin).
    Q_INVOKABLE bool pasteItems();
    /// Where Paste puts the items: the selected bin when exactly one bin is selected, else the bin shown (the parent of
    /// a search bin shown). "" = the project's root.
    Q_INVOKABLE QString pasteTarget() const;
    /// Forgets the cut marks (Escape).
    Q_INVOKABLE void clearCut();

    // ---- Metadata (one undo step each) ----
    /// Renames a bin or a media item.
    Q_INVOKABLE bool rename(const QString& key, const QString& name);
    Q_INVOKABLE bool setLabel(const QStringList& keys, int colorIndex);
    Q_INVOKABLE bool setComment(const QString& key, const QString& text);
    Q_INVOKABLE bool setDescription(const QString& key, const QString& text);
    /// The poster frame at `fraction` of the item (its subclip range for a subclip), or cleared (fraction < 0).
    Q_INVOKABLE bool setPosterFrame(const QString& key, double fraction);
    /// Deletes items: bins with their contents, media not used by a sequence, sequences. One undo step. P1 round (#38):
    /// with `removeClips`, media and sequences that sequences use (directly or inside deleted bins) go too, with every
    /// clip that plays them and those clips' linked partners, lifted without a ripple ("Delete and Remove Clips"); else
    /// such a delete is refused with `message`, as before.
    Q_INVOKABLE bool deleteItems(const QStringList& keys, bool removeClips = false);
    Q_INVOKABLE bool deleteSelection();
    /// P1 round (#38): what deleting `keys` would take out of sequences: { items, clips (clips removed, linked partners
    /// included, and 0.6 auditions losing takes), takes (0.6: audition takes removed from auditions that stay),
    /// sequences (how many sequences lose clips or takes), sequenceNames, firstName (the first used item's name),
    /// usedItems (how many of the items deleted are used, an inactive audition take counting), unusedKeys (fix round 1:
    /// the keys no sequence uses on their own - what Delete with Options deletes when the clips stay) }.
    Q_INVOKABLE QVariantMap deleteImpact(const QStringList& keys) const;
    /// Delete (project.delete) and Delete with Options (Ctrl/Cmd+Delete, `withOptions`): when sequences use what goes,
    /// or always with options, deleteConfirmRequested asks first (the panel's confirmation calls deleteItems); else the
    /// items are deleted at once. True when something was deleted or asked about.
    Q_INVOKABLE bool requestDelete(const QStringList& keys, bool withOptions = false);

    // ---- Usage and Find in Timeline (P1 round, #40) ----
    /// Where item `key` (media, a subclip, or a sequence nested in others) is used, one row per placement (a linked
    /// video / audio pair is one): [{ sequenceKey, sequenceName, clipId (its video clip when it has one), clipName,
    /// timecode (its start in the sequence's timecode), tracks ("V1, A1"), seconds }], by sequence name then start.
    Q_INVOKABLE QVariantList usages(const QString& key) const;
    /// The usage badge's tooltip: "Used 3 times in 2 sequences (video 3, audio 3)", "Not used in any sequence".
    Q_INVOKABLE QString usageText(const QString& key) const;
    /// Find in Timeline: opens sequence `sequenceKey` (its tab), puts the playhead at the clip's start, selects it (with
    /// its linked partners) and asks the window to scroll the timeline to it (revealInTimelineRequested). False with
    /// `message` when the clip is gone.
    Q_INVOKABLE bool findInTimeline(const QString& sequenceKey, const QString& clipId);

    // ---- Twirl-down bins in the list view (P1 round, #46) ----
    /// Expands (shows the contents of) or collapses bin `binKey` in the list view; `recursive` (Alt+click) does the
    /// same to every bin inside it. Kept per project file in the settings.
    Q_INVOKABLE void setExpanded(const QString& binKey, bool expanded, bool recursive = false);
    Q_INVOKABLE bool isExpanded(const QString& binKey) const;
    /// The bin item `key` is in ("" = the project's root).
    Q_INVOKABLE QString parentOf(const QString& key) const;
    /// The list view shows the twirl-down tree (list view, no Find, not a search bin).
    Q_INVOKABLE bool treeShown() const;
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

    // ---- Proxies for the selection or a bin (P1 round, #44) ----
    /// The media keys in bin `bin` ("" = the root), with what the bins inside it hold when `recursive`.
    Q_INVOKABLE QStringList mediaInBin(const QString& bin, bool recursive) const;
    /// What a Proxy menu row on `key` acts on: a bin's media (with its bins'), the selection's media when `key` is
    /// selected, else `key` itself; only media that can have a proxy (video: not stills, titles or audio).
    Q_INVOKABLE QStringList proxyTargets(const QString& key) const;
    /// The Proxy menu's counts for `key`: { targets, create (no proxy and none being made), ready, making, deletable }.
    Q_INVOKABLE QVariantMap proxyPlan(const QString& key) const;
    /// Create / Remake / Delete (or cancel) the proxies of proxyTargets(key). Create skips media whose proxy is ready
    /// or being made. Each returns the toast's line ("27 queued, 3 already have proxies.").
    Q_INVOKABLE QString createProxies(const QString& key);
    Q_INVOKABLE QString remakeProxies(const QString& key);
    Q_INVOKABLE QString deleteProxies(const QString& key);

    // ---- Get Properties (P1 round, #39) ----
    /// Everything the Properties dialog shows of a Library item, from what the project stored (nothing is probed
    /// again): browserFileInfo's fields (name, path, container, duration, video { codec, profile, frameRate,
    /// variableFrameRate, pixelFormat, bitDepth, chroma, fieldOrder, alpha, rotation, ... }, audio [...], bytes, files)
    /// plus kind, bitrate (bits per second), proxy (its state), uses (clips in sequences) and offline. A sequence's key
    /// gives its settings ({ kind: "sequence", name, width, height, frameRate, duration, tracks }). Empty: not found.
    Q_INVOKABLE QVariantMap mediaProperties(const QString& key) const;
    /// The rows of a mediaProperties (or browserFileInfo) map: [{ label, value }], translated. The Media Browser's
    /// metadata view shows the same rows.
    Q_INVOKABLE QVariantList propertyLines(const QVariantMap& info) const;
    /// 0.7 (card 103): what this build does with the newer codecs, asked of the FFmpeg it links: [{ id, name, plays
    /// (the build has a decoder), exports (it has an encoder for the format), text (a plain sentence: what plays, what is
    /// decode-only) }] for APV, 4:2:2 HEVC, 4:2:2 AV1 and MV-HEVC. About shows it; Get Properties names the entry that
    /// applies to a clip.
    Q_INVOKABLE QVariantList codecSupport() const;
    /// "Label: value" lines of `key`'s properties (Copy all), and that copy onto the clipboard.
    Q_INVOKABLE QString propertiesText(const QString& key) const;
    Q_INVOKABLE bool copyProperties(const QString& key) const;
    /// What Get Properties for Selection (file.getProperties) shows: the Library's selected item when the Library has
    /// the focus, else the Source Monitor's clip, else the media of the selected timeline clip ("" = nothing).
    Q_INVOKABLE QString propertiesTarget(bool libraryFocused) const;
    /// Asks the window to show the Properties dialog of `key` (propertiesRequested): Properties… in the item menu.
    Q_INVOKABLE void requestProperties(const QString& key);
    /// P1 round (#43): asks the window to relink the offline items `keys` (linkMediaRequested: the Relink dialog for
    /// them): Link Media… on Library items and timeline clips, a double-click on an offline Library item.
    Q_INVOKABLE void requestLinkMedia(const QStringList& keys);

    // ---- Housekeeping ----
    Q_INVOKABLE bool reveal(const QString& key);
    Q_INVOKABLE void selectUnused();
    /// Removes every unused media item: one undo step. Returns how many went.
    Q_INVOKABLE int removeUnused();
    /// 0.7 (card 025): what Consolidate Duplicates would do, as the confirmation lists it, without changing anything: one
    /// map per set of duplicates { keeper (item key), keeperName, removed [{ key, name }], clips (clips that move to the
    /// keeper), skipped [{ name, reason }], notes [text] }. Empty when there is nothing to merge.
    Q_INVOKABLE QVariantList consolidatePreview() const;
    /// 0.7 (card 025): merges every set of duplicates (media items that are one file) into one item each: one undo step.
    /// Returns how many items went; the message says what was merged and what could not be.
    Q_INVOKABLE int consolidateDuplicates();
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
    /// 0.7 (card 144): saves the columns as they are now (which show, their order and widths) as a layout called `name`
    /// (a layout of that name is replaced; at most kMaxColumnPresets), applies a saved one, forgets one. Kept per user.
    /// saveColumnPreset returns false for an empty name (message set).
    Q_INVOKABLE bool saveColumnPreset(const QString& name);
    Q_INVOKABLE bool applyColumnPreset(const QString& name);
    Q_INVOKABLE bool deleteColumnPreset(const QString& name);
    static constexpr int kMaxColumnPresets = 20;
    /// 0.7 (card 112): the bin `key` is (on) or is no longer (off) where Nest… files nested sequences. One undo step.
    Q_INVOKABLE bool setNestBin(const QString& key, bool on);

    // ---- Keyboard (docs/keymap.md: project.*, clip.makeSubclip) ----
    /// UX rework (L): also Premiere's Project panel zoom keys (project.thumbnailSizeNext / Previous, project.zoomIn /
    /// Out: thumbnailScale one step up or down) and the selection keys (project.moveSelection* and
    /// project.extendSelection*: selectionMoveRequested, which the panel answers in the view it shows).
    static bool handlesCommand(const QString& id);
    /// { handled, enabled, reason } for the command palette. P1 round (#41): also Edit > Cut and Paste ("edit.cut",
    /// "edit.paste"), which Main.qml gives the Library only while it has the focus (handlesCommand leaves them out).
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
    /// P1 round (#40): how Find in Timeline opens a sequence's tab (QML builds: SequenceTabs.open; unset: the
    /// controller's current sequence).
    void setSequenceOpener(std::function<bool(const QString& sequenceKey)> opener) { m_openSequence = std::move(opener); }
    /// P1 round (#40): every media item's / sequence's uses (C++, tests).
    const std::unordered_map<std::uint64_t, LibraryUsage>& usage() const { return m_usage; }
    const std::unordered_map<std::uint64_t, LibraryUsage>& sequenceUsage() const { return m_sequenceUsage; }

signals:
    void viewChanged();
    void scopeChanged();
    void columnsChanged();
    void sortChanged();
    void hoverScrubChanged();
    void listThumbnailsChanged();
    void thumbnailScaleChanged();
    void ingestChanged();
    void fieldEditingChanged();
    void columnPresetsChanged();
    /// 0.7 (card 021): a field-step key was pressed while a cell is open: move `columnDelta` cells across and `rowDelta`
    /// rows down (-1 / +1); the list view commits the open cell and opens the next one.
    void fieldStepRequested(int columnDelta, int rowDelta);
    void binChanged();
    void tabsChanged();
    void filterChanged();
    void selectionChanged();
    void statsChanged();
    void subclipSourceChanged();
    void messageChanged();
    /// P0 round (fix round 1): a passing note for a toast that is not a result or problem (`message` stays as it
    /// was): "4 chapter markers became clip markers." after a Media Browser import.
    void notice(const QString& text);
    void freeformChanged();
    void freeformLinesChanged();
    /// QML: start an inline rename of `key` (after New Bin).
    void renameRequested(const QString& key);
    /// QML: focus the Find field (Ctrl/Cmd+F).
    void findRequested();
    /// QML: ask for the subclip's name (Ctrl/Cmd+U).
    void makeSubclipRequested();
    /// 0.7 (card 025), QML: open the confirmation of Edit > Consolidate Duplicates ("consolidate") or Remove Unused
    /// ("removeUnused").
    void housekeepingRequested(const QString& kind);
    /// UX rework (L), Premiere's Project panel selection keys: move the selection in the view shown. `where`: "up",
    /// "down", "left", "right" (one card / row; up and down are a row of cards in the grid), "home", "end" (the first /
    /// last listed item), "pageUp", "pageDown" (a page of the view). `extend`: Shift (Range selection from the anchor).
    void selectionMoveRequested(const QString& where, bool extend);
    /// QML: bring the Library forward and scroll `key` into view (showItem()).
    void itemShown(const QString& key);
    void embedXmpChanged();
    /// The XMP of `key`'s file changed (the File section reads it again).
    void fileMetadataChanged(const QString& key);
    void cutChanged();
    void summaryChanged();
    /// QML (the window): show the Properties dialog of `key` / the Relink dialog for the offline items `keys`.
    void propertiesRequested(const QString& key);
    void linkMediaRequested(const QStringList& keys);
    /// P1 round (#38): the panel asks before deleting `keys` (deleteImpact's `impact`; `withOptions`: Delete with
    /// Options, which offers to remove the clips even when none would go).
    void deleteConfirmRequested(const QStringList& keys, const QVariantMap& impact, bool withOptions);
    /// P1 round (#40): the window scrolls the timeline to clip `clipId` (Find in Timeline).
    void revealInTimelineRequested(const QString& clipId);
    /// P1 round (#46): a bin was expanded or collapsed.
    void expandedChanged();

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
    bool m_fieldEditing = false;
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
    bool m_listThumbnails = false; ///< 0.7 (card 023)
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
    /// 0.7 (card 144): the saved column layouts, in the order they were made (settings: library/columnPresets).
    std::vector<std::pair<QString, std::vector<ColumnState>>> m_columnPresets;
    /// The settings text of a column layout ("id:width:visible" each), and the layout it stands for (columns this build
    /// does not know are skipped, columns it added since come last as their defaults say); nullopt for an empty list.
    static QStringList encodeColumns(const std::vector<ColumnState>& columns);
    static std::optional<std::vector<ColumnState>> decodeColumns(const QStringList& saved);
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
    // P1 round (#41): the items Cut marked; pruneCut() drops the ones that no longer exist.
    QSet<QString> m_cut;
    void pruneCut();
    /// The media and sequences `keys` stand for (bins: everything inside them, recursively), each once.
    void collectUnder(const QStringList& keys, std::vector<const MediaItem*>& media,
                      std::vector<const Sequence*>& sequences) const;
    // P1 round (#40): uses per media item and per sequence (nested), from the last snapshot.
    std::unordered_map<std::uint64_t, LibraryUsage> m_usage;
    std::unordered_map<std::uint64_t, LibraryUsage> m_sequenceUsage;
    std::function<bool(const QString&)> m_openSequence;
    // P1 round (#46): the bins expanded in the list view, kept per project file (QSettings "library/expanded").
    std::unordered_set<std::uint64_t> m_expanded;
    QString m_expansionFile; // the project file m_expanded belongs to ("" = untitled: not kept)
    bool m_expansionLoaded = false;
    void loadExpansion(const QString& projectFile);
    void saveExpansion() const;

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

/// P1 round (#38): the clips that deleting `bins` (with everything inside them), `media` and `sequences` takes out of
/// the sequences that stay: every clip playing such media or nesting such a sequence, plus their linked partners; by
/// sequence. Clips of sequences deleted with them are not listed. 0.6 (card #74): an audition is listed when every
/// one of its takes plays what goes (auditionsLosingTakes lists the others).
std::map<std::uint64_t, std::vector<ClipId>> clipsUsingLibraryItems(const ProjectState& state, std::span<const BinId> bins,
                                                                     std::span<const MediaId> media,
                                                                     std::span<const SequenceId> sequences);
/// 0.6 (card #74): the auditions in the sequences that stay with some takes, not all, playing what goes (an inactive
/// take counts): they lose just those takes. By sequence.
std::map<std::uint64_t, std::vector<ClipId>> auditionsLosingTakes(const ProjectState& state, std::span<const BinId> bins,
                                                                  std::span<const MediaId> media,
                                                                  std::span<const SequenceId> sequences);
/// Deletes Library items as one edit: every sequence that goes (named or inside a deleted bin, the nesting ones before
/// those they nest, in any selection order), then media outside the deleted bins, then the bins with their contents.
/// With `removeClips` the auditionsLosingTakes lose those takes (edit::removeAuditionTake) and the clips
/// clipsUsingLibraryItems lists are deleted first (gaps stay: no ripple); without it, items in use are refused
/// (edit::removeMedia / removeSequence / removeBins).
std::expected<ProjectState, Error> removeLibraryItems(const ProjectState& state, std::span<const BinId> bins,
                                                      std::span<const MediaId> media, std::span<const SequenceId> sequences,
                                                      bool removeClips);

} // namespace wabcut::app
