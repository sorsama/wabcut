#pragma once

// EditingBridge (P.2, E5; QML singleton `Editing`): Premiere's editing and sequence commands in the app, each over its
// typed command (CatalogEditing.cpp), so the menus, the keys, the palette, the assistant and the CLI do the same thing
// in one undo step:
//   * Clip > Video Options (the clip menu's submenu): Scale to Frame Size, Set to Frame Size, Set to Fill Frame, Frame
//     Hold Options..., Add Frame Hold, Insert Frame Hold Segment;
//   * Edit > Duplicate and File > New > Sequence From Clip (the Library's menu), Sequence > Sequence Settings...;
//   * Replace With Clip (From Source Monitor / Match Frame / From Bin; Shift-drag from the Source Monitor, Alt- or
//     Shift-drag from the Library onto a clip), Replace Footage..., Make Offline, Edit Original (Ctrl+E);
//   * Make Subsequence (Shift+U), Join Through Edits, Go to Gap (Shift+; ...), Close Gap;
//   * Merge Clips..., Modify > Interpret Footage... and Timecode...;
//   * Window > History (the undo steps: click one to go back to it; history.stepBackward / stepForward, Left / Right
//     in the History panel's keymap scope, go one step);
//   * Preferences > Media (Default Media Scaling) and > Timeline (still image, video and audio transition default
//     durations), read where they apply (ProjectLifecycle's import, ProjectController's default transitions);
//   * 0.2 UX (stream T): Set as Default Transition (the default video and audio transition ids), and the clip menu's
//     Reveal in Project / Reveal in File Explorer (clip.revealInProject).
// The keymap commands it runs (handlesCommand) are routed by Main.qml's runCommand; the palette lists them as ready.
//
// Principle 9: QML sees keys ("c12", "m3", "s4") and names.

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectState.h>

#include <wabcut/command/Command.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class LibraryBridge;
class ProjectController;
class SequenceTabsBridge;
class SourceMonitorBridge;

class EditingBridge : public QObject {
    Q_OBJECT
    /// Preferences > Media > Default Media Scaling: "none", "scaleToFrameSize", "setToFrameSize" or "setToFillFrame".
    Q_PROPERTY(QString defaultMediaScaling READ defaultMediaScaling WRITE setDefaultMediaScaling NOTIFY preferencesChanged FINAL)
    /// Preferences > Timeline: the still image default duration and the default transition durations, in seconds.
    Q_PROPERTY(double stillDuration READ stillDuration WRITE setStillDuration NOTIFY preferencesChanged FINAL)
    Q_PROPERTY(double videoTransitionDuration READ videoTransitionDuration WRITE setVideoTransitionDuration NOTIFY preferencesChanged FINAL)
    Q_PROPERTY(double audioTransitionDuration READ audioTransitionDuration WRITE setAudioTransitionDuration NOTIFY preferencesChanged FINAL)
    /// 0.2 UX (stream T), Premiere's Set as Default Transition: the transition id Apply Video / Audio Transition
    /// (Ctrl+D / Ctrl+Shift+D), Apply Default Transitions to Selection (Shift+D) and the timeline transition menu's
    /// Replace with Default use (QSettings "timeline/defaultVideoTransition" / "timeline/defaultAudioTransition"). A saved
    /// id that is no longer a transition of that kind reads as the built-in "crossDissolve" / "constantPower"; writing
    /// such an id is ignored.
    Q_PROPERTY(QString defaultVideoTransition READ defaultVideoTransition WRITE setDefaultVideoTransition NOTIFY preferencesChanged FINAL)
    Q_PROPERTY(QString defaultAudioTransition READ defaultAudioTransition WRITE setDefaultAudioTransition NOTIFY preferencesChanged FINAL)
    /// The History panel: [{ label, state: "done" | "current" | "undone" }], the project as opened first.
    Q_PROPERTY(QVariantList historySteps READ historySteps NOTIFY historyChanged FINAL)
    /// What the last action reported ("" after success; why it could not run otherwise).
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit EditingBridge(QObject* parent = nullptr);
    static EditingBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setLibrary(LibraryBridge* library);
    void setSourceMonitor(SourceMonitorBridge* source);
    void setTabs(SequenceTabsBridge* tabs);

    /// The keymap commands this bridge runs (Main.qml's runCommand asks runCommand() first).
    static bool handlesCommand(const QString& commandId);
    /// The saved preferences, read where they apply (QSettings "media/defaultMediaScaling", "timeline/...").
    static MediaScaling defaultMediaScalingSetting();
    /// Still Image Default Duration; nullopt while it is the built-in 5 seconds.
    static std::optional<Ticks> stillDurationSetting();
    /// The default transitions with the preferred lengths.
    static edit::DefaultTransitions defaultTransitionsSetting();

    QString defaultMediaScaling() const;
    void setDefaultMediaScaling(const QString& scaling);
    double stillDuration() const;
    void setStillDuration(double seconds);
    double videoTransitionDuration() const;
    void setVideoTransitionDuration(double seconds);
    double audioTransitionDuration() const;
    void setAudioTransitionDuration(double seconds);
    QString defaultVideoTransition() const;
    void setDefaultVideoTransition(const QString& effectId);
    QString defaultAudioTransition() const;
    void setDefaultAudioTransition(const QString& effectId);
    QVariantList historySteps() const;
    QString message() const { return m_message; }

    /// Runs one of the keymap commands above; false for any other id.
    Q_INVOKABLE bool runCommand(const QString& commandId);
    /// Runs a typed command on the project (the current sequence filled in); "" or why not. One undo step.
    command::CommandResult run(const std::string& name, command::Json params);

    // ---- Clip > Video Options ----
    /// What the clip menu shows for a clip: { video (it or a linked partner is a video clip), scaleToFrame, frozen,
    /// throughEdit (it meets a clip it continues: Join Through Edits applies) }.
    Q_INVOKABLE QVariantMap clipVideoInfo(const QString& key) const;
    /// Scale to Frame Size on or off for the selection (or `key` when it is not selected). Returns "" or why not.
    Q_INVOKABLE QString scaleToFrameSize(const QString& key, bool enabled);
    Q_INVOKABLE QString setToFrameSize(const QString& key);
    Q_INVOKABLE QString setToFillFrame(const QString& key);
    /// Add Frame Hold / Insert Frame Hold Segment (2 seconds) at the playhead on the video clip `key`.
    Q_INVOKABLE QString addFrameHold(const QString& key, bool insertSegment);
    /// Frame Hold Options: { available, frozen, holdFilters, playhead (timecode) } and apply { enabled, holdOn, time,
    /// holdFilters }.
    Q_INVOKABLE QVariantMap frameHoldInfo(const QString& key) const;
    Q_INVOKABLE QString applyFrameHoldOptions(const QString& key, const QVariantMap& options);
    Q_INVOKABLE void openFrameHoldOptions(const QString& key);
    /// Field Options (0.2): { available, name, reverseFieldDominance, processing ("none", "alwaysDeinterlace", ...),
    /// fieldOrder (the footage's: "progressive", "upperFirst", "lowerFirst") } and apply { reverseFieldDominance,
    /// processing } to the selection (or `key` when it is not selected). One undo step.
    Q_INVOKABLE QVariantMap fieldOptionsInfo(const QString& key) const;
    Q_INVOKABLE QString applyFieldOptions(const QString& key, const QVariantMap& options);
    Q_INVOKABLE void openFieldOptions(const QString& key);

    // ---- Duplicate, New Sequence From Clip, Sequence Settings ----
    /// Edit > Duplicate on Library items ("m3", "s4"); one undo step.
    Q_INVOKABLE QString duplicate(const QStringList& keys);
    /// New Sequence From Clip: makes it and opens it in the timeline.
    Q_INVOKABLE QString newSequenceFromClip(const QString& key);
    /// The current sequence's key, or "".
    Q_INVOKABLE QString currentSequenceKey() const;
    /// Sequence Settings of `key` (sequence.settings: frameRate { num, den, fps }, width, height, pixelAspect, fields,
    /// audioRate, dropFrame, previewFormat, videoDisplay, audioDisplay, startTimecode, name), or {}.
    Q_INVOKABLE QVariantMap sequenceSettings(const QString& key) const;
    /// sequence.setSettings with `settings` (its keys as the command's params). "" or why not.
    Q_INVOKABLE QString applySequenceSettings(const QString& key, const QVariantMap& settings);
    /// The presets: [{ id, name, group, width, height, fps, dropFrame }].
    Q_INVOKABLE QVariantList sequencePresets() const;
    Q_INVOKABLE void openSequenceSettings(const QString& key);

    // ---- Replace, Replace Footage, Make Offline, Edit Original ----
    /// Replace With Clip on the selected clips: "sourceMonitor" (its item from its In), "matchFrame" (the Source
    /// Monitor's frame at the sequence playhead), "bin" (the Library's selected item from its In).
    Q_INVOKABLE QString replaceWithClip(const QString& mode);
    /// A drop with the replace modifier (Shift from the Source Monitor; Alt or Shift from the Library) onto `clipKey`:
    /// Replace Edit with `mediaKey` (the Source Monitor's item when empty) from its In. False when it is not one.
    Q_INVOKABLE bool replaceDrop(const QString& clipKey, const QString& mediaKey, bool fromSourceMonitor);
    /// Whether the replace modifier is held now (for drops).
    Q_INVOKABLE bool replaceModifierHeld(bool fromSourceMonitor) const;
    Q_INVOKABLE QString replaceFootage(const QString& mediaKey, const QUrl& file, bool rename);
    Q_INVOKABLE QString makeOffline(const QStringList& keys);
    /// Edit Original: the item's file in the system's default application. False when it has none.
    Q_INVOKABLE bool editOriginal(const QString& mediaKey);
    Q_INVOKABLE void openReplaceFootage(const QString& mediaKey);

    // ---- Subsequence, through edits, gaps ----
    /// Make Subsequence of the selection (or In to Out when nothing is selected or `fromInOut`).
    Q_INVOKABLE QString makeSubsequence(bool fromInOut);
    /// Join Through Edits at `key`'s edges (empty: every one of the selection, else of the sequence).
    Q_INVOKABLE QString joinThroughEdits(const QString& key);
    /// Go to Gap: next / previous, in the sequence or in the targeted tracks; moves the playhead. False when none.
    Q_INVOKABLE bool goToGap(bool next, bool inSequence);
    /// Close Gap on the selection (every gap when nothing is selected).
    Q_INVOKABLE QString closeGap();

    // ---- Merge Clips, Interpret Footage, Timecode ----
    Q_INVOKABLE QString mergeClips(const QStringList& mediaKeys, const QString& sync, bool removeCameraAudio,
                                   const QString& name);
    /// The first item's interpretation: { frameRate (fps or 0 = the file's), fileRate (fps), pixelAspect (or 0),
    /// filePixelAspect, fieldOrder ("" = the file's), ignoreAlpha, invertAlpha, premultiplication ("" = the file's),
    /// hasAlpha }.
    Q_INVOKABLE QVariantMap interpretation(const QStringList& mediaKeys) const;
    Q_INVOKABLE QString applyInterpretation(const QStringList& mediaKeys, const QVariantMap& settings);
    /// { timecode ("" = the file's), fileTimecode, reelName }.
    Q_INVOKABLE QVariantMap mediaTimecode(const QStringList& mediaKeys) const;
    Q_INVOKABLE QString applyMediaTimecode(const QStringList& mediaKeys, const QString& timecode, const QString& reelName);
    Q_INVOKABLE void openMergeClips(const QStringList& mediaKeys);
    Q_INVOKABLE void openInterpretFootage(const QStringList& mediaKeys);
    Q_INVOKABLE void openTimecode(const QStringList& mediaKeys);

    // ---- Clip > Modify > Source Settings for camera RAW (0.2, RawSourceSettings.h) ----
    /// Whether the item is camera RAW (ProRes RAW, CinemaDNG): Source Settings apply.
    Q_INVOKABLE bool isRawMedia(const QString& mediaKey) const;
    /// The first RAW item's: { name, count (RAW items), exposure (stops), whiteBalance ("asShot" / "custom"),
    /// temperature (K), tint }; empty when none is RAW.
    Q_INVOKABLE QVariantMap rawSourceSettings(const QStringList& mediaKeys) const;
    /// Develops the RAW items among `mediaKeys` with { exposure, whiteBalance, temperature, tint }: one undo step.
    /// "" or the error.
    Q_INVOKABLE QString applyRawSourceSettings(const QStringList& mediaKeys, const QVariantMap& settings);
    Q_INVOKABLE void openRawSourceSettings(const QStringList& mediaKeys);

    // ---- History ----
    /// Goes back (or forward) to the step `index` of historySteps.
    Q_INVOKABLE bool goToHistoryStep(int index);

    // ---- Default transitions (0.2 UX, stream T) ----
    /// Set as Default Transition (the Effects panel's item menu, the timeline transition menu): `effectId` becomes the
    /// default video or audio transition, by its kind. False (nothing changes) for an id that is not a built-in
    /// transition.
    Q_INVOKABLE bool setDefaultTransition(const QString& effectId);
    /// `effectId` is the default video or audio transition now (the Effects panel marks it).
    Q_INVOKABLE bool isDefaultTransition(const QString& effectId) const;
    /// The built-in transitions of `kind` ("video" | "audio") for a picker: [{ id, name, category, isDefault }], by name.
    Q_INVOKABLE QVariantList transitionChoices(const QString& kind) const;

    // ---- Reveal (0.2 UX, stream T: the clip menu's Reveal in Project / Reveal in File Explorer) ----
    /// What a timeline clip can reveal: { item: its media item ("m12") or nested sequence ("s3") key, "" for none;
    /// name (the item's); inProject (the item is in the project: Reveal in Project applies); inExplorer (a media file
    /// that is found: Reveal in File Explorer applies); offline (its file is missing) }; {} for a clip that is not in the
    /// current sequence.
    Q_INVOKABLE QVariantMap revealInfo(const QString& clipKey) const;
    /// Reveal in Project (Premiere; keymap clip.revealInProject, no default key): the clip's item - media, or the
    /// nested sequence - shown in the Library (Library.showItem: its bin opens, filters clear, it is selected) and
    /// libraryRequested emitted so the panel comes forward. `clipKey` "" = the primary selected clip. False (message)
    /// when there is no such item.
    Q_INVOKABLE bool revealInProject(const QString& clipKey = {});
    /// Reveal in File Explorer (Finder on macOS): the clip's media file, selected in the system file manager
    /// (Library.reveal). False (message) for a clip without a file or with an offline one.
    Q_INVOKABLE bool revealInExplorer(const QString& clipKey = {});

signals:
    void preferencesChanged();
    void messageChanged();
    void historyChanged();
    /// QML (EditingHost): open a dialog or panel.
    void preferencesRequested(const QString& page);
    void sequenceSettingsRequested(const QString& sequenceKey);
    void frameHoldOptionsRequested(const QString& clipKey);
    void fieldOptionsRequested(const QString& clipKey);
    void replaceFootageRequested(const QString& mediaKey);
    void mergeClipsRequested(const QStringList& mediaKeys);
    void interpretFootageRequested(const QStringList& mediaKeys);
    void rawSourceSettingsRequested(const QStringList& mediaKeys);
    void timecodeRequested(const QStringList& mediaKeys);
    void historyRequested();
    /// The Library should come forward (a new sequence or item to show).
    void libraryRequested();

private:
    /// The clips an action on `key` applies to: the selection when it holds the clip (or `key` is empty), else the
    /// clip alone.
    std::vector<ClipId> targetClips(const QString& key) const;
    QString runOnClips(const char* command, const std::vector<ClipId>& clips, command::Json extra = command::Json::object());
    /// Library keys of media items: `keys`, or the Library's selection when empty.
    std::vector<MediaId> mediaOf(const QStringList& keys) const;
    /// The camera RAW items among mediaOf(keys).
    std::vector<MediaId> rawMediaOf(const QStringList& keys) const;
    QString result(const command::CommandResult& result);
    void setMessage(const QString& message);

    QPointer<ProjectController> m_controller;
    QPointer<LibraryBridge> m_library;
    QPointer<SourceMonitorBridge> m_source;
    QPointer<SequenceTabsBridge> m_tabs;
    QString m_message;
};

struct EditingBridgeForeign {
    Q_GADGET
    QML_FOREIGN(EditingBridge)
    QML_NAMED_ELEMENT(Editing)
    QML_SINGLETON
public:
    static EditingBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return EditingBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
