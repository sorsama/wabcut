#pragma once

// ProjectController: the bridge between the model's EditSession and QML (QML singleton `ProjectController`).
//
// It owns the EditSession (the project snapshot + undo history) and the UI-side state that is NOT document
// state: the current sequence, the selection and the playhead. Everything QML sees follows principle 9: times
// are formatted timecode strings, ids are opaque strings (BridgeIds.h), never raw ticks or uint64 numbers.
//
// C++ consumers (TimelineView, tests, later PlaybackController) use the typed API below.
//
// Change notification: `changed(changeSet)` fires after every edit, undo, redo, cancelled gesture, reset and
// sequence switch with
//   { kind: "edit"|"undo"|"redo"|"cancel"|"reset", label, sequence: "s…", tracks: ["t…", …],
//     structural: bool, markers: bool, marks: bool }
// filled from the model's ChangeSet for the current sequence: `tracks` lists the tracks whose content or
// header changed, `structural` means everything may have changed (another project or sequence, tracks
// added/removed/reordered). C++ views read the typed lastChange() (with the time ranges to redraw) instead.
//
// Keyboard: runCommand(id) executes the keymap commands this controller owns (undo/redo, selection,
// playhead navigation, sequence marks, New Sequence, and the edit commands below); Main.qml routes
// KeymapDispatcher.commandTriggered here.
//
// Editing (M5b): every edit goes through the model's EditOps and the EditSession, never through the view.
// Selection-based edits are Q_INVOKABLE (delete, ripple delete, add edit, Q/W, E, D, link, enable, nudge,
// slide, slip, copy/cut/paste, rename, label colour). Mouse gestures are previewed with previewEdit() (the
// edit runs on the current snapshot and nothing is committed) and committed with commitGesture(), which wraps
// the edit in an EditSession gesture: one drag is one undo step.
//
// Clipboard (C.11, M5m): Cut / Copy / Paste / Paste Insert run the model's clipboard.* commands on one
// ClipboardServices the session owns (clipboardServices(): the assistant shares it), so a paste keeps speed,
// reverse, freeze, time remapping, effects, transitions and markers. With the system clipboard on (the app's
// controller), copy and cut also put the clipboard there as application/x-wabcut-clipboard, and a document another
// Wabcut window or project put there is what Paste pastes.

#include "BridgeIds.h"
#include "ParameterUi.h"
#include "FilmstripSource.h"
#include "PeakSource.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/SnapshotReleaser.h>

#include <QAbstractListModel>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::command {
class ClipboardServices;
struct CommandBatch;
struct CommandResult;
} // namespace wabcut::command

namespace wabcut::app {

class MediaPeakSource;
class MediaFilmstripSource;
class PictureProject;
class ClipAccessModel;
class EffectControlsModel;

/// What the last snapshot change did to the current sequence, for C++ views (see ProjectController::lastChange).
struct TimelineChange {
    /// Everything may have changed: a new project, another sequence, tracks added/removed/reordered.
    bool structural = true;
    /// Changed tracks with the time ranges to redraw (straight from the model's ChangeSet).
    std::vector<TrackChange> tracks;
    bool markersChanged = false;
    bool marksChanged = false;
    /// M5n (P.1 hotspots): the whole ChangeSet of the change (null after a reset: everything changed), the current
    /// sequence it was read for, and the snapshot it started from, so listeners can refresh only what they show and
    /// update incrementally instead of diffing the project again.
    std::shared_ptr<const ChangeSet> changes;
    SequenceId sequence;
    ProjectSnapshot before;

    /// The current sequence's part of `changes` (null: nothing in it changed, or no ChangeSet).
    const SequenceChange* sequenceChange() const;
    /// The clip may look different: it was added, removed or modified (or the change is structural).
    bool touches(ClipId clip) const;
    /// Any of the clips, or a media item they play, changed (or the change is structural).
    bool touchesAny(std::span<const ClipId> clips, std::span<const MediaId> media = {}) const;
    /// Media items, redactions or project-wide settings changed (what a clip shows besides its own fields).
    bool projectWide() const;
};

/// A time range lit on the timeline: the words selected in the transcript (Selection), or what a text edit would
/// remove before it is applied (Removal: the ghost of a dry run). `tracks` empty = every track.
struct TimelineHighlight {
    enum class Kind { Selection, Removal };
    Kind kind = Kind::Selection;
    TimeRange range;
    std::vector<TrackId> tracks;
};

/// Tracks of the current sequence in display order (V_n … V1, then A1 … A_n) for the track headers.
class TrackListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by ProjectController.tracks")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)

public:
    enum Role {
        TrackIdRole = Qt::UserRole + 1, // opaque "t…"
        ShortNameRole,                  // "V1", "A2"
        NameRole,                       // custom name or "Video 1"
        KindRole,                       // "video" | "audio"
        ColorIndexRole,                 // header chip tint (index into Theme.clipColors)
        EnabledRole,
        LockedRole,
        MutedRole,
        SoloRole,
        TargetedRole,
        SyncLockedRole,
    };

    explicit TrackListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Rebuilds from `sequence` (nullptr = no sequence). Keeps delegates when only flags changed.
    void update(const Sequence* sequence);

    /// Display order: track pointer per row.
    const std::vector<const Track*>& rows() const { return m_rows; }

signals:
    void countChanged();

private:
    struct Row {
        TrackId id;
        TrackKind kind = TrackKind::Video;
        int index = 0; // 0 = V1/A1
        QString name;
        bool enabled = true, locked = false, muted = false, solo = false, targeted = false, syncLocked = true;
    };
    static Row makeRow(const Track& track, int index);

    std::vector<Row> m_data;
    std::vector<const Track*> m_rows;
};

class ProjectController : public QObject {
    Q_OBJECT

    // ---- Sequence ----
    Q_PROPERTY(QStringList sequenceNames READ sequenceNames NOTIFY sequencesChanged FINAL)
    Q_PROPERTY(int currentSequenceIndex READ currentSequenceIndex WRITE setCurrentSequenceIndex NOTIFY
                   sequenceChanged FINAL)
    Q_PROPERTY(QString sequenceId READ sequenceIdKey NOTIFY sequenceChanged FINAL)
    Q_PROPERTY(QString sequenceName READ sequenceName NOTIFY sequenceChanged FINAL)
    Q_PROPERTY(QString frameRateText READ frameRateText NOTIFY sequenceChanged FINAL)
    Q_PROPERTY(QString formatText READ formatText NOTIFY sequenceChanged FINAL)
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY sequenceChanged FINAL)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY sequenceChanged FINAL)
    Q_PROPERTY(QString durationText READ durationText NOTIFY sequenceChanged FINAL)
    Q_PROPERTY(wabcut::app::TrackListModel* tracks READ tracks CONSTANT FINAL)
    /// Every clip as a list row for assistive technology and keyboard navigation (ClipAccessModel.h).
    Q_PROPERTY(wabcut::app::ClipAccessModel* clipList READ clipList CONSTANT FINAL)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY sequenceChanged FINAL)
    /// Audio tracks for the mixer: [{ trackId, shortName, name, colorIndex }].
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY revisionChanged FINAL)
    /// [{ name, comment, timecode, colorIndex, durationText ("" for a point marker, else the range's length as a
    /// timecode), type ("comment", "chapter", ...: markerTypeName) }] in time order.
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged FINAL)
    /// Sequence In/Out marks as timecode ("" when unset). Out is shown as its last marked frame.
    Q_PROPERTY(QString markInText READ markInText NOTIFY marksChanged FINAL)
    Q_PROPERTY(QString markOutText READ markOutText NOTIFY marksChanged FINAL)

    // ---- Media ----
    /// Project media for the library: [{ id: "m…", name, kind: "video"|"audio"|"text", durationText, meta,
    /// colors: [a, b], thumbnail: "image://wabcut-thumbnails/…" or "" when the file is not available,
    /// offline: the file is missing or not the original (MediaLocator::offline), path: its absolute path }].
    Q_PROPERTY(QVariantList mediaItems READ mediaItems NOTIFY mediaChanged FINAL)
    /// True when at least one media item resolves to a file on disk (false for the demo project).
    Q_PROPERTY(bool hasMediaFiles READ hasMediaFiles NOTIFY mediaChanged FINAL)

    // ---- Playhead (UI state, not in undo) ----
    Q_PROPERTY(QString playheadText READ playheadText NOTIFY playheadChanged FINAL)
    /// 0.2, Premiere's Show Audio Time Units (the Program Monitor's and the Timeline's menu; MonitorBridge keeps the
    /// setting): the playhead moves by audio samples (finePlayhead) and playheadText counts them (the sequence's audio
    /// display format: samples or milliseconds). playhead() stays the frame that holds the sample, so every edit at
    /// the playhead still lands on a frame.
    Q_PROPERTY(bool audioTimeUnits READ audioTimeUnits WRITE setAudioTimeUnits NOTIFY audioTimeUnitsChanged FINAL)
    /// Index of the marker exactly at the playhead, or -1.
    Q_PROPERTY(int playheadMarkerIndex READ playheadMarkerIndex NOTIFY playheadChanged FINAL)

    // ---- Selection (UI state, not in undo) ----
    Q_PROPERTY(QStringList selectedClips READ selectedClips NOTIFY selectionChanged FINAL)
    /// The primary selected clip for the clip editor (empty map when nothing is selected):
    /// { id, name, trackName, rangeText, colorIndex, hasVideo, hasAudio,
    ///   properties: { scale, positionX, positionY, rotation, anchorX, anchorY, opacity, volume, pan },
    ///   defaults: same keys, the values "reset" returns to }   — all in UI units (%, px, °, dB, -100..100)
    Q_PROPERTY(QVariantMap selectedClip READ selectedClip NOTIFY selectedClipChanged FINAL)
    /// The selection's link and enable state, for the clip context menu (Link/Unlink, Enable).
    Q_PROPERTY(bool selectionLinked READ selectionLinked NOTIFY selectedClipChanged FINAL)
    Q_PROPERTY(bool selectionEnabled READ selectionEnabled NOTIFY selectedClipChanged FINAL)
    /// 0.2: a selected clip is grouped (Clip > Group / Ungroup, ClipGroups.h).
    Q_PROPERTY(bool selectionGrouped READ selectionGrouped NOTIFY selectedClipChanged FINAL)
    /// 0.2, Premiere's Sequence > Selection Follows Playhead: while on, moving the playhead selects the clips under it on
    /// the targeted tracks (with their groups and links). UI state of the session, off at start.
    Q_PROPERTY(bool selectionFollowsPlayhead READ selectionFollowsPlayhead WRITE setSelectionFollowsPlayhead NOTIFY
                   selectionFollowsPlayheadChanged FINAL)
    /// The clipboard holds clips: this session's, or another Wabcut's on the system clipboard.
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged FINAL)
    /// The edge of the primary selected clip that keyboard trims move: "head", "tail" or "" (none chosen: tail).
    Q_PROPERTY(QString selectedEdge READ selectedEdge WRITE setSelectedEdge NOTIFY selectedEdgeChanged FINAL)
    /// The selected transition ("x…"), or "". Selecting a transition clears the clip selection and vice versa.
    Q_PROPERTY(QString selectedTransition READ selectedTransition NOTIFY selectedTransitionChanged FINAL)
    /// 0.2 UX (stream T): the selected gap - the empty time on one track between two clips, or between the sequence start
    /// and its first clip - as { track: "t3", trackName: "V1", start, end, duration (timecodes in the sequence's display
    /// format) }, or an empty map. A click in a gap selects it (TimelineView); Delete, Backspace and Ripple Delete then
    /// close it (rippleDeleteGap). Selecting clips or a transition, Deselect All, any edit, undo, redo and another
    /// sequence clear it.
    Q_PROPERTY(QVariantMap selectedGap READ selectedGap NOTIFY selectedGapChanged FINAL)
    /// Effect Controls (M5d): the selected clip's parameters, effect stack and keyframes, or the selected
    /// transition (EffectControls.h).
    Q_PROPERTY(wabcut::app::EffectControlsModel* effectControls READ effectControls CONSTANT FINAL)

    // ---- History ----
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged FINAL)
    Q_PROPERTY(QString undoText READ undoText NOTIFY historyChanged FINAL)
    Q_PROPERTY(QString redoText READ redoText NOTIFY historyChanged FINAL)
    /// Increments on every snapshot change (bind to it to re-evaluate derived expressions).
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)

public:
    enum SelectMode { Replace = 0, Add = 1, Toggle = 2 };
    Q_ENUM(SelectMode)

    explicit ProjectController(QObject* parent = nullptr);
    ~ProjectController() override;
    /// QML singleton factory (ProjectControllerSingleton): also registers the library thumbnail provider.
    static ProjectController* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    // ---- C++ API ----
    EditSession& session() { return *m_session; }
    ProjectSnapshot snapshot() const { return m_session->snapshot(); }
    SequenceId currentSequenceId() const { return m_sequence; }
    /// The current sequence in the current snapshot (kept alive by the returned pointer).
    SequencePtr currentSequence() const;
    /// Replaces the project (clears the history). `sequence` invalid = first sequence.
    void resetProject(ProjectState state, SequenceId sequence = {}, std::vector<ClipId> selection = {});

    Ticks playhead() const { return m_playhead; }
    void setPlayhead(Ticks t);
    /// Content extent used for scrolling: the sequence duration (at least one minute) plus 4 % tail room.
    Ticks contentDuration() const;

    const std::vector<ClipId>& selection() const { return m_selection; }
    ClipId primaryClip() const { return m_primary; }
    bool isSelected(ClipId id) const;
    void setSelection(std::vector<ClipId> clips, ClipId primary = {});
    /// Selects/toggles a clip; with `followLinks` its linked partners too (Premiere's linked selection).
    void selectClipById(ClipId clip, SelectMode mode, bool followLinks = true);

    QString formatTimecode(Ticks t) const;
    /// Parses a timecode typed in the sequence's rate ("00:00:01:12", "1:12", "36"); nullopt when invalid.
    std::optional<Ticks> parseTimecodeText(const QString& text) const;
    /// One frame of the current sequence.
    Ticks frameTicks() const;

    // ---- Finishing (M5d: effects, keyframes, transitions, speed) ----
    /// The clip that receives a property edit: the primary clip or its linked partner of `kind`.
    std::optional<ClipId> propertyTarget(StreamKind kind) const;
    /// Sizes the position/anchor UI origins of `clip` depend on (the sequence canvas, its media's frame).
    PropertyFrame propertyFrame(const Clip& clip) const;
    /// The curve time the editors show `clip`'s parameters at: the playhead, held inside the clip.
    Ticks curveTimeAtPlayhead(const Clip& clip) const;
    /// Applies an edit as one undo step named `label`; consecutive edits with the same non-empty `mergeKey`
    /// merge into one step until endPropertyGesture(). False, with lastError set, when the model rejects it.
    bool applyEditFn(const char* label, const std::function<edit::EditOutcome(const ProjectState&)>& fn,
                     std::string_view mergeKey = {});
    /// Sets a parameter of `clip` (model units) at the playhead: the constant, or for an animated parameter the
    /// key at the playhead (updated, else added). Merges like applyEditFn.
    bool setParameterAtPlayhead(ClipId clip, const edit::ParamRef& param, double value, const char* label,
                                std::string_view mergeKey = {});
    /// A parameter's value on `clip`: intrinsic, or an effect parameter (its descriptor default when unset).
    static ParamValue parameterValue(const Clip& clip, const edit::ParamRef& param);
    TransitionId selectedTransitionId() const { return m_selectedTransition; }
    /// Ranges lit on the timeline by other panels (the transcript); UI state, not in undo.
    void setTimelineHighlights(std::vector<TimelineHighlight> highlights);
    const std::vector<TimelineHighlight>& timelineHighlights() const { return m_highlights; }
    void selectTransitionById(TransitionId transition);
    /// The selected transition's track and value in the current sequence.
    struct TransitionLocation {
        const Track* track = nullptr;
        const Transition* transition = nullptr;
    };
    std::optional<TransitionLocation> findTransition(TransitionId id) const;

    // ---- Gap selection (0.2 UX, stream T; ProjectControllerTimeline.cpp) ----
    struct GapSelection {
        TrackId track;
        TimeRange range;
        bool operator==(const GapSelection&) const = default;
    };
    /// The selected gap of the current sequence (see selectedGap), or nullopt.
    const std::optional<GapSelection>& gapSelection() const { return m_gap; }
    /// Selects the gap at `at` on `track` and clears the clip and transition selection. False, changing nothing, when
    /// `at` is not in a gap followed by a clip (edit::rippleDeleteGap's rule) on a track of the current sequence.
    bool selectGapAt(TrackId track, Ticks at);
    /// The gap at `at` on `track` of the current sequence (as selectGapAt would select it), or nullopt.
    std::optional<GapSelection> gapAt(TrackId track, Ticks at) const;

    // ---- Clipboard (C.11: Clipboard.h, the clipboard.* and clip.*Attributes commands) ----
    /// The session's clipboard. The assistant's engine bundles it (AgentController's services), so clips it copies
    /// paste with Ctrl+V and the other way round; call notifyClipboardChanged() after such an engine ran.
    std::shared_ptr<command::ClipboardServices> clipboardServices() const { return m_clipboardServices; }
    /// Runs commands on the session with clipboardServices() as their services, as one undo step named
    /// `batch.label` (a dry run commits nothing). Requests without "sequence" get the current one.
    command::CommandResult runClipboardBatch(const command::CommandBatch& batch);
    /// The system clipboard: copy and cut also put the clipboard there (clipboardMimeType()), and a document another
    /// Wabcut window or project put there is what Paste pastes. On for the app's controller (create()); off by
    /// default, so a controller made in C++ leaves the system clipboard alone.
    void setSystemClipboardEnabled(bool on);
    bool systemClipboardEnabled() const { return m_systemClipboard; }
    /// When the system clipboard holds a clipboard document that is not this session's own copy, reads it into
    /// clipboardServices() (the newest copy wins, as with any clipboard). True when it did; a document that cannot
    /// be read sets lastError and leaves the clipboard as it was.
    bool adoptSystemClipboard();
    /// Emits clipboardChanged (another engine set clipboardServices()).
    void notifyClipboardChanged();
    static QString clipboardMimeType();

    // ---- Project file (I.1: set by ProjectLifecycle, which owns open/save) ----
    /// The file media locators resolve against (empty while untitled); refreshes the library.
    void setProjectFile(std::filesystem::path file);
    const std::filesystem::path& projectFile() const { return m_projectFile; }

    // ---- Edits for gestures (C++: TimelineView) ----
    using EditFn = std::function<edit::EditOutcome(const ProjectState&)>;
    /// Runs `fn` on the current snapshot and returns its result without committing anything (ghost previews).
    edit::EditOutcome previewEdit(const EditFn& fn) const;
    /// Commits `fn` as one EditSession gesture (one undo step named `label`). False, with lastError set, when the
    /// model rejects it (e.g. a locked track).
    bool commitGesture(const char* label, const EditFn& fn);
    /// Razor tool: cuts the clip under `at` on `track` (and its linked partners unless `ignoreLinks`), or on every
    /// unlocked track with `allTracks`.
    bool razorAt(TrackId track, Ticks at, bool allTracks, bool ignoreLinks = false);

    /// Waveform envelopes for the timeline: real peaks for media files, placeholders for the rest.
    std::shared_ptr<const PeakSource> peakSource() const;
    MediaPeakSource* mediaPeakSource() const { return m_peaks.get(); }
    /// Video thumbnails for the timeline's filmstrips (M5i): ThumbnailService atlases, proxies preferred.
    std::shared_ptr<const FilmstripSource> filmstripSource() const;
    MediaFilmstripSource* mediaFilmstripSource() const { return m_filmstrips.get(); }
    /// Where waveform peaks and thumbnails are cached (default: <CacheLocation>/media).
    void setMediaCacheDirectory(const QString& directory);
    QString mediaCacheDirectory() const { return m_mediaCacheDir; }

    /// The last change to the current sequence (valid inside and after `changed`).
    const TimelineChange& lastChange() const { return m_lastChange; }
    /// The last change may have put another clip under the playhead on a video track (a video track changed there,
    /// or its header, or the change was structural): panels that follow the clip under the playhead when nothing is
    /// selected (Color, Masks) look again only then (M5n, P.1).
    bool lastChangeAtPlayhead() const;
    std::optional<Ticks> markIn() const;
    std::optional<Ticks> markOut() const;

    // ---- QML properties ----
    QStringList sequenceNames() const;
    int currentSequenceIndex() const;
    void setCurrentSequenceIndex(int index);
    QString sequenceIdKey() const { return sequenceKey(m_sequence); }
    QString sequenceName() const;
    QString frameRateText() const;
    QString formatText() const;
    int frameWidth() const;
    int frameHeight() const;
    QString durationText() const;
    TrackListModel* tracks() const { return m_tracks; }
    ClipAccessModel* clipList() const { return m_clipList; }
    QString selectedEdge() const { return m_selectedEdge; }
    void setSelectedEdge(const QString& edge);
    QString selectedTransition() const;
    QVariantMap selectedGap() const;
    EffectControlsModel* effectControls() const { return m_effectControls; }
    int clipCount() const;
    QVariantList markers() const;
    QVariantList audioTracks() const;
    QString markInText() const;
    QString markOutText() const;
    QVariantList mediaItems() const { return m_mediaItems; }
    bool hasMediaFiles() const { return m_hasMediaFiles; }
    QString playheadText() const;
    bool audioTimeUnits() const { return m_audioTimeUnits; }
    void setAudioTimeUnits(bool on);
    /// The playhead to the audio sample with audio time units on (playhead() is the frame holding it); else playhead().
    Ticks finePlayhead() const { return m_finePlayhead; }
    /// `t` as the sequence's audio time (samples or milliseconds, Show Audio Time Units).
    QString formatAudioTime(Ticks t) const;
    int playheadMarkerIndex() const;
    QStringList selectedClips() const;
    QVariantMap selectedClip() const;
    bool selectionLinked() const;
    bool selectionEnabled() const;
    bool canPaste() const;
    bool canUndo() const { return m_session->canUndo(); }
    bool canRedo() const { return m_session->canRedo(); }
    QString undoText() const;
    QString redoText() const;
    int revision() const { return m_revision; }
    QString lastError() const { return m_lastError; }

    // ---- QML actions ----
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE void newSequence();
    /// Opens a .wabcut project (file URL or local path). Returns false and sets lastError on failure.
    Q_INVOKABLE bool loadProject(const QUrl& file);

    Q_INVOKABLE void stepFrames(int frames);
    Q_INVOKABLE void goToStart();
    Q_INVOKABLE void goToEnd();
    /// Up/Down: the previous/next clip boundary on the targeted tracks (all tracks when none is targeted);
    /// `anyTrack` (Shift+Up/Down) uses every track.
    Q_INVOKABLE void goToPreviousEdit(bool anyTrack = false);
    Q_INVOKABLE void goToNextEdit(bool anyTrack = false);
    Q_INVOKABLE void seekToMarker(int index);
    /// Shift+Home / Shift+End: start or end of the primary selected clip.
    Q_INVOKABLE void goToSelectedClipStart();
    Q_INVOKABLE void goToSelectedClipEnd();
    /// 0.2 UX (stream T), Premiere's typed playhead entry (the Timeline's and the Program Monitor's timecode): moves the
    /// playhead to `text`, a time as the sequence displays it - timecode ("00:01:02:03", "1:02:03", ';' '.' ',' as
    /// separators, digits packed from the right: "10203" = 00:01:02:03, "110" = 00:00:01:10), a frame count with the
    /// Frames display format, feet+frames ("0012+05") with a feet display format - or, with a leading '+' or '-', an
    /// offset from the playhead in the same units ("+15" = 15 frames later, "-1:00" = one second earlier, "+1." = one
    /// second later: Premiere's '.' ends a field). With audio time units on, the audio time is read too
    /// ("00:00:01:24000" samples, "00:00:01.500" milliseconds; offsets "+24000" count samples). Clamped at the sequence
    /// start. False, moving nothing, for text it cannot read.
    Q_INVOKABLE bool seekToText(const QString& text);
    /// The time `text` names (as seekToText reads it, offsets from the playhead) in the display format ("" when it is
    /// not a time): QML can show what Enter will do.
    Q_INVOKABLE QString timeEntryText(const QString& text) const;

    /// Sequence In/Out marks (undoable model edits): I, O, X (around the clips under the playhead), / (around
    /// the selection), and clearing them.
    Q_INVOKABLE void markInAtPlayhead();
    Q_INVOKABLE void markOutAtPlayhead();
    Q_INVOKABLE void markClipAtPlayhead();
    Q_INVOKABLE void markSelection();
    Q_INVOKABLE void clearMarks(bool clearIn = true, bool clearOut = true);
    Q_INVOKABLE void goToMarkIn();
    Q_INVOKABLE void goToMarkOut();

    // ---- Editing (undoable; act on the selection, the playhead and the targeted tracks) ----
    /// Delete (clear): removes the selected clips, leaving gaps.
    Q_INVOKABLE void deleteSelection();
    /// Ripple Delete: removes the selected clips and closes the gaps.
    Q_INVOKABLE void rippleDeleteSelection();
    /// Add Edit (Ctrl/Cmd+K): cuts at the playhead on the tracks of the selected clips under it, else on the
    /// targeted tracks; `allTracks` (Shift+Ctrl/Cmd+K) cuts every unlocked track.
    Q_INVOKABLE void razorAtPlayhead(bool allTracks = false);
    Q_INVOKABLE void addEditAtPlayhead();
    /// Q / W: ripple-trims the previous / next edit to the playhead on the targeted tracks.
    Q_INVOKABLE void rippleTrimPreviousToPlayhead();
    Q_INVOKABLE void rippleTrimNextToPlayhead();
    /// E: moves the selected clip's nearest edge to the playhead (a regular trim).
    Q_INVOKABLE void extendEditToPlayhead();
    /// D: selects the clips under the playhead on the targeted tracks (all tracks when none is targeted).
    Q_INVOKABLE void selectClipAtPlayhead();
    /// Ctrl/Cmd+L: links the selected clips, or unlinks them when they are linked.
    Q_INVOKABLE void toggleLinkSelection();
    Q_INVOKABLE void linkSelection();
    Q_INVOKABLE void unlinkSelection();
    /// Shift+E: enables or disables the selected clips.
    Q_INVOKABLE void toggleEnabledSelection();
    /// 0.2, Clip > Group (Ctrl/Cmd+G) / Ungroup (Ctrl/Cmd+Shift+G) on the selection (the clip.group / clip.ungroup
    /// commands; ProjectControllerTimeline.cpp).
    Q_INVOKABLE void groupSelection();
    Q_INVOKABLE void ungroupSelection();
    /// 0.2, Sequence > Go to Gap > Next in Sequence (Shift+;) / Previous in Sequence (Ctrl/Cmd+Shift+;): the playhead
    /// to the start of the next / previous gap where no track has a clip.
    Q_INVOKABLE void goToSequenceGap(bool forward);
    bool selectionGrouped() const;
    bool selectionFollowsPlayhead() const { return m_selectionFollowsPlayhead; }
    void setSelectionFollowsPlayhead(bool on);
    /// Alt+Left/Right (±1, ±5): moves the selection; Alt+,/. slides and Ctrl+Alt+Left/Right slips the clip.
    Q_INVOKABLE void nudgeSelection(int frames);
    Q_INVOKABLE void slideSelection(int frames);
    Q_INVOKABLE void slipSelection(int frames);
    /// Copy / Cut / Paste (overwrite) / Paste Insert at the playhead on the targeted tracks (Premiere): the
    /// clipboard.* commands. Copy and cut take exactly the selection (linked selection already decided whether a
    /// clip's partners are in it); paste selects the pasted clips, parks the playhead after them and reports how
    /// media was found (clipboardNotice).
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void cutSelection();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void pasteInsert();
    Q_INVOKABLE void renameClip(const QString& clipId, const QString& name);
    /// Label colour of the selected clips (index into Theme.clipColors).
    Q_INVOKABLE void setSelectionColor(int colorIndex);
    /// Wrap several edits in one undo step (EditSession gestures); cancelGesture() returns to the state before it.
    Q_INVOKABLE void beginGesture(const QString& label);
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void cancelGesture();

    // ---- Keyboard-only editing (the timeline's clip list) ----
    /// Selects the previous (-1) or next (+1) clip on the primary clip's track (with its links); with nothing
    /// selected, the first clip at or after the playhead on the top track that has one.
    Q_INVOKABLE void selectAdjacentClip(int direction);
    /// 0.2, Premiere's Select Next / Previous Clip (timeline.selectNextClip / selectPreviousClip): the next (+1) or
    /// previous (-1) clip on the targeted tracks (every track when none is targeted) after / before the primary clip -
    /// every targeted track's clip that starts there, with links and groups; with nothing selected the clips under the
    /// playhead, else the first ones after it.
    void selectNextClip(int direction);
    /// Selects the clip nearest in time on the track above (-1) or below (+1), skipping empty tracks.
    Q_INVOKABLE void selectClipOnAdjacentTrack(int direction);
    /// Moves the selected edge (selectedEdge; the tail when none is chosen) by `frames`: a regular trim, one
    /// undo step (Premiere's Trim Backward/Forward).
    Q_INVOKABLE void trimSelectedEdge(int frames);

    /// Whether a keymap command this controller owns can run now: { handled, enabled, reason }. `reason` is
    /// a translated sentence for the command palette ("Select a clip first").
    Q_INVOKABLE QVariantMap commandAvailability(const QString& commandId) const;
    static bool handlesCommand(const QString& commandId);

    /// Runs a keymap command this controller owns (docs/keymap.md command ids). Returns false for ids it does
    /// not handle, so the caller can route them elsewhere.
    Q_INVOKABLE bool runCommand(const QString& commandId);

    Q_INVOKABLE void selectClip(const QString& clipId, int mode = Replace, bool followLinks = true);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE QString clipName(const QString& clipId) const;

    /// Track header toggles (undoable edits): flag = "enabled" | "locked" | "muted" | "solo" | "targeted" | "syncLocked".
    Q_INVOKABLE void setTrackFlag(const QString& trackId, const QString& flag, bool value);

    // ---- Track header menu (0.2 UX, stream T; ProjectControllerTimeline.cpp): one undo step each ----
    /// What the menu of a track of the current sequence shows: { id, kind: "video" | "audio", shortName ("V2"), name
    /// (the custom name, else "Video 2"), customName ("" for the default), index (0 = V1 / A1), clipCount, locked,
    /// syncLocked, targeted, canDelete, deleteReason ("" or why Delete Track is off), emptyTracks, emptyVideoTracks,
    /// emptyAudioTracks (what Delete Empty Tracks would remove) }; {} for a key that is not a track of the sequence.
    Q_INVOKABLE QVariantMap trackInfo(const QString& trackId) const;
    /// Rename...: the track's custom name ("" or only spaces: back to the default "Video n" / "Audio n"; at most 256
    /// characters). Allowed on locked tracks (like the header toggles). False with lastError when refused.
    Q_INVOKABLE bool renameTrack(const QString& trackId, const QString& name);
    /// Add Track / Add Tracks... (Premiere's dialog) as one undo step "Add Tracks" ("Add Track" for one): `spec` =
    ///   { video: 0..99, audio: 0..99 (default 0),
    ///     videoPlacement, audioPlacement: "first" (before the first track: the new tracks become V1... / A1...) |
    ///       "last" (above the top video track / below the last audio track; the default) | "after" | "before" (next to
    ///       `track` in numbering: video "after" = above it, audio "after" = below it),
    ///     track: "t3" - the track "after" / "before" refer to, per kind (a track of the other kind is ignored for that
    ///       kind); default: the topmost targeted track of the kind, else the last one,
    ///     audioFormat: "stereo" (default) | "mono" | "5.1" | "adaptive" - the new audio tracks' channel format }.
    /// Returns the new tracks' keys (video first, each kind in the order made), [] when nothing was added (lastError
    /// says why for a bad spec: counts out of range, an unknown placement or format).
    Q_INVOKABLE QStringList addTracks(const QVariantMap& spec);
    /// Delete Track: the track and its clips (their linked partners on other tracks stay, unlinked). Refused (false,
    /// lastError) on a locked track or the only track of its kind; trackInfo().canDelete says so first.
    Q_INVOKABLE bool deleteTrack(const QString& trackId);
    /// Delete Empty Tracks: every unlocked track of `kind` ("video", "audio" or "" = both) without clips, keeping at
    /// least one track of each kind (the lowest-numbered). One undo step; returns how many went (0: nothing changed).
    Q_INVOKABLE int deleteEmptyTracks(const QString& kind = {});

    // ---- Gap selection (0.2 UX, stream T) ----
    /// Deselects the gap (selectedGap becomes {}).
    Q_INVOKABLE void clearGapSelection();
    /// Ripple Delete of the selected gap (timeline.rippleDelete with a gap): one undo step "Ripple Delete", the gap
    /// selection goes. False with lastError (the model's reason: another sync-locked track has clips in that time, a
    /// locked track) when it cannot close; nothing changes then and the gap stays selected.
    Q_INVOKABLE bool rippleDeleteGap();
    /// Clip editor: sets a property of the selected clip in UI units (see selectedClip.properties). Consecutive
    /// changes of the same property merge into one undo step until endPropertyGesture().
    Q_INVOKABLE void setClipProperty(const QString& key, double value);
    Q_INVOKABLE void endPropertyGesture();

    // ---- Finishing (M5d) ----
    /// Selects a transition ("x…"; "" clears it).
    Q_INVOKABLE void selectTransition(const QString& transitionId);
    /// Removes the selected transition (Delete with a transition selected).
    Q_INVOKABLE bool deleteSelectedTransition();
    /// 0.2 UX (stream T), the timeline transition menu: { id, kind: "video" | "audio", effect (its transition id),
    /// name (display name), durationText (timecode), alignment: "center" | "startAtCut" | "endAtCut", twoSided (between
    /// two clips: Alignment applies), locked (its track is locked), defaultEffect (Editing's default transition of its
    /// kind), isDefault (effect == defaultEffect) } of a transition of the current sequence; {} when unknown. The menu's
    /// actions run on the selected transition (a right-click selects it): Set Duration... ->
    /// effectControls.setTransitionDuration(timecode), Alignment -> effectControls.setTransitionAlignment(alignment),
    /// Replace with Default -> effectControls.setTransitionEffect(defaultEffect), Set as Default ->
    /// Editing.setDefaultTransition(effect), Clear -> deleteSelectedTransition().
    Q_INVOKABLE QVariantMap transitionInfo(const QString& transitionId) const;
    /// Apply Video / Audio Transition (Ctrl/Cmd+D, Ctrl/Cmd+Shift+D): the default transition at the edit point
    /// nearest the playhead on the targeted tracks.
    Q_INVOKABLE bool applyDefaultTransition(bool audio);
    /// Apply Default Transitions to Selection (Shift+D).
    Q_INVOKABLE bool applyDefaultTransitionsToSelection();
    /// Speed/Duration dialog (Ctrl/Cmd+R): the primary selected clip's { available, reason, speed (percent),
    /// durationText, reversed, maintainPitch, hasAudio, count (selected clips), frozen }.
    Q_INVOKABLE QVariantMap speedDurationInfo() const;
    /// The primary clip's length at `percent` speed (timecode), or "" when it cannot be computed.
    Q_INVOKABLE QString durationForSpeed(double percent) const;
    /// The speed (percent) at which the primary clip lasts `timecode`; NaN when the text is not a length.
    Q_INVOKABLE double speedForDuration(const QString& timecode) const;
    /// A typed length normalised to the sequence's timecode ("" when invalid).
    Q_INVOKABLE QString normalizeTimecode(const QString& text) const;

signals:
    void changed(const QVariantMap& changeSet);
    void revisionChanged();
    void sequencesChanged();
    void sequenceChanged();
    void markersChanged();
    void marksChanged();
    void mediaChanged();
    void clipboardChanged();
    /// A paste's notes ("'A.mov': added to the project"), translated and joined, for a toast.
    void clipboardNotice(const QString& text);
    void selectedEdgeChanged();
    void selectedTransitionChanged();
    void selectedGapChanged();
    /// timelineHighlights() changed (C++ only).
    void timelineHighlightsChanged();
    /// The peak source was replaced or has new peaks: views redraw their waveforms (C++ only).
    void peaksChanged();
    /// The filmstrip source was replaced or has new atlases (M5i, C++ only).
    void filmstripsChanged();
    void playheadChanged();
    void selectionChanged();
    void selectedClipChanged();
    void historyChanged();
    void lastErrorChanged();
    void selectionFollowsPlayheadChanged();
    void audioTimeUnitsChanged();
    /// Emitted first in the destructor, while session() is still valid: engine threads reading the session
    /// (playback) stop here. C++ only.
    void sessionClosing();

private:
    void onSessionChanged(const ChangeEvent& event);
    /// Updates the derived state after a snapshot change. `changes` is the model's ChangeSet; nullptr means
    /// "assume everything changed" (reset, sequence switch).
    void syncAfterChange(const ChangeSet* changes, ChangeKind kind, std::string_view label);
    void ensureValidSequence();
    /// Resolves the media files of the current snapshot (peak source and library list).
    void refreshMedia();
    /// Hands the current snapshot to the sequence pictures (every revision; I.9).
    void publishPictures();
    /// Drops selected clips that no longer exist: only the ones `changes` removed when the selection was checked
    /// before (P.1: no set of every clip per edit); every selected id otherwise.
    void pruneSelection(const ChangeSet* changes, bool sequenceSwitched);
    void setError(const QString& message);
    template <class Fn>
    bool applyEdit(const char* label, Fn&& fn, std::string_view mergeKey = {});
    /// Applies an EditOps edit; with `followPlayhead` the playhead goes where Premiere leaves it.
    bool applyResult(const char* label, const EditFn& fn, bool followPlayhead = false);
    void pasteClips(bool insert);
    /// clipboard.copy / clipboard.cut of the selection (exported to the system clipboard when it is on).
    bool copyToClipboard(bool cut);
    /// Puts a clipboard document on the system clipboard as this session's own copy.
    void publishToSystemClipboard(const std::string& document);
    /// The system clipboard changed: does it hold another Wabcut's document?
    void onSystemClipboardChanged();
    /// Selected clips of the current sequence that still exist.
    std::vector<ClipId> selectedClipIds() const;
    std::vector<Ticks> editPoints(bool anyTrack) const;
    Ticks lastMarkedFrame() const;
    /// Selection Follows Playhead: selects the clips under the playhead (setPlayhead calls it).
    void followPlayheadSelection();
    /// Where seekToText(text) would put the playhead (clamped at 0), or nullopt when `text` is not a time.
    std::optional<Ticks> timeEntryTarget(const QString& text) const;
    /// Sets (or with nullopt clears) the gap selection and tells the views.
    void setGapSelection(std::optional<GapSelection> gap);

    std::unique_ptr<EditSession> m_session;
    /// P.1: frees what the session lets go (evicted undo steps, the previous project) on a thread of its own; before
    /// it, nothing did until the controller died. Declared after m_session: it goes first.
    std::unique_ptr<SnapshotReleaser> m_releaser;
    EditSession::ListenerId m_listener = 0;
    TimelineChange m_lastChange;
    /// The snapshot the last change ended on (the next change's `before`).
    ProjectSnapshot m_seenSnapshot;
    /// The selection as pruneSelection last checked it against the whole sequence.
    std::vector<ClipId> m_checkedSelection;
    std::shared_ptr<command::ClipboardServices> m_clipboardServices;
    bool m_systemClipboard = false;
    /// The system clipboard holds a document that is not this session's own copy.
    bool m_systemForeign = false;
    /// SHA-256 of the last document this session put on (or read from) the system clipboard.
    QByteArray m_systemSeen;
    QMetaObject::Connection m_systemConnection;
    std::filesystem::path m_projectFile; // empty for the demo / unsaved project
    QString m_mediaCacheDir;
    std::shared_ptr<MediaPeakSource> m_peaks;
    std::shared_ptr<MediaFilmstripSource> m_filmstrips; // M5i
    /// The project sequence pictures render from (nested strips, Library thumbnails of sequences; I.9).
    std::shared_ptr<PictureProject> m_pictures;
    QVariantList m_mediaItems;
    bool m_hasMediaFiles = false;
    SequenceId m_sequence;
    Ticks m_playhead;
    Ticks m_finePlayhead;         // 0.2: to the audio sample with audio time units on, else m_playhead
    bool m_audioTimeUnits = false;
    std::vector<ClipId> m_selection;
    ClipId m_primary;
    TrackListModel* m_tracks = nullptr;
    ClipAccessModel* m_clipList = nullptr;
    EffectControlsModel* m_effectControls = nullptr;
    TransitionId m_selectedTransition;
    std::optional<GapSelection> m_gap; // 0.2 UX (stream T): the selected gap
    std::vector<TimelineHighlight> m_highlights;
    QString m_selectedEdge;
    bool m_selectionFollowsPlayhead = false;
    int m_revision = 0;
    QString m_lastError;
};

/// QML registration through the factory. Qt prefers a default constructor over create() when both exist, and the
/// class stays default-constructible for C++ and tests, so the singleton is registered by this foreign wrapper.
struct ProjectControllerSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::ProjectController)
    QML_NAMED_ELEMENT(ProjectController)
    QML_SINGLETON
public:
    static ProjectController* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return ProjectController::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
