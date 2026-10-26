#pragma once

// SourceMonitorBridge: Premiere's Source Monitor and source editing, for QML (singleton `SourceMonitor`, M5h).
//
//   * The loaded source: a media item from the Library (double-click), Match Frame or the recent-sources menu. It
//     plays on its own: an editor::PlaybackSession over a private one-clip project (picture in the Source Monitor's
//     ProgramMonitor, sound from the first play on), with its own transport, timecode, duration and In-Out length.
//     The program pauses while the source plays and the other way round.
//   * Source marks are stored on the media item (MediaItem::markIn/markOut, saved with the project) and set through
//     the source.setMarks / source.clearMarks commands: one undo step each. In = the start of the frame shown, Out =
//     the end of it (Premiere).
//   * Source patching (the track headers' source column): which source track (V1, A1, A2, ...) goes to which
//     sequence track and which indicators are switched off. View state per sequence, kept here (by source track
//     label, so the next source keeps it); the source commands read it through command::SourceMonitorServices.
//     Changes are checked by sequence.setPatches.
//   * Insert (,) / Overwrite (.): edit.insertFromSource / overwriteFromSource. A four-point edit whose ranges differ
//     needs a Fit Clip choice: edit() returns "fit" until QML passes one (the dialog, or the remembered choice).
//     previewEdit() shows the edit as a ghost on the timeline (a dry run in TimelineView's external preview).
//   * P.2: Premiere's Loop (`loop`, remembered): playback repeats the source In to Out (a missing mark is the start
//     or the end); Play from outside that range starts at its beginning, and Play In to Out repeats it.
//   * 0.2 Media Browser: a file shown before it is imported (loadFile, `preview`): it plays from its own description
//     (editor::describeCameraClip: a camera card's clip, spanned parts joined) and has no marks; the first mark,
//     marker, patch change or edit imports it through the Library (the importer: LibraryBridge::importFromBrowser,
//     one undo step into the Library's bin, with the Ingest settings) and goes on with the imported item. A preview is
//     a file, not a project's item: it stays when another project opens.
//   * Track targeting from the headers (sequence.setTargets), Match Frame (F) and Reverse Match Frame (Shift+R)
//     (sequence.matchFrame / reverseMatchFrame), and drops from the Source Monitor onto the timeline (video and
//     audio, video only, audio only), which go where they are dropped.
//
// Principle 9: QML sees opaque keys ("m12", "t3"), text and fractions of the source's length, never ticks or ids.

#include <wabcut/command/Engine.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/model/EditSession.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class PlaybackBridge;
class ProgramMonitor;
class ProjectController;

class SourceMonitorBridge : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool loaded READ loaded NOTIFY sourceChanged FINAL)
    /// Opaque key of the loaded media item ("m12"), or "" (also for a Media Browser preview).
    Q_PROPERTY(QString media READ mediaKeyText NOTIFY sourceChanged FINAL)
    /// 0.2: a Media Browser file shown before it is imported (loadFile); its first mark or edit imports it.
    Q_PROPERTY(bool preview READ preview NOTIFY sourceChanged FINAL)
    Q_PROPERTY(QString name READ name NOTIFY sourceChanged FINAL)
    /// "video", "audio" (no picture: the panel draws a waveform), "still", or "" when nothing is loaded.
    Q_PROPERTY(QString kind READ kind NOTIFY sourceChanged FINAL)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY sourceChanged FINAL)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY sourceChanged FINAL)
    /// Width / height of the picture (16:9 without one).
    Q_PROPERTY(double aspect READ aspect NOTIFY sourceChanged FINAL)
    Q_PROPERTY(QString durationText READ durationText NOTIFY sourceChanged FINAL)
    /// Source timecode of the frame shown, and where it is as a fraction of the source's length.
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionChanged FINAL)
    Q_PROPERTY(double position READ positionFraction NOTIFY positionChanged FINAL)
    Q_PROPERTY(bool hasIn READ hasIn NOTIFY marksChanged FINAL)
    Q_PROPERTY(bool hasOut READ hasOut NOTIFY marksChanged FINAL)
    /// Mark positions as fractions of the source's length (0 / 1 when unset).
    Q_PROPERTY(double inPosition READ inFraction NOTIFY marksChanged FINAL)
    Q_PROPERTY(double outPosition READ outFraction NOTIFY marksChanged FINAL)
    Q_PROPERTY(QString inText READ inText NOTIFY marksChanged FINAL)
    Q_PROPERTY(QString outText READ outText NOTIFY marksChanged FINAL)
    /// Length of the marked range (the whole source without marks): Premiere's In/Out duration.
    Q_PROPERTY(QString inOutText READ inOutText NOTIFY marksChanged FINAL)
    // 0.4: the sequence In/Out length moved to ProjectController.markDurationText (sequenceInOutText is gone).
    Q_PROPERTY(bool playing READ playing NOTIFY transportChanged FINAL)
    /// P.2: Premiere's Loop button in the Source Monitor.
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged FINAL)
    /// Recently loaded sources, newest first: [{ media, name, kind }].
    Q_PROPERTY(QVariantList recent READ recent NOTIFY recentChanged FINAL)
    /// The source track indicators: [{ source: "V1"|"A1"..., kind: "video"|"audio", track: key or "", enabled }].
    Q_PROPERTY(QVariantList indicators READ indicators NOTIFY patchingChanged FINAL)
    /// Track key -> { source, enabled } for the track headers' source column.
    Q_PROPERTY(QVariantMap patchMap READ patchMap NOTIFY patchingChanged FINAL)
    /// A source is loaded and at least one of its indicators is patched and on.
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY patchingChanged FINAL)
    /// The last problem (a refused edit, a source that cannot load), "" otherwise.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit SourceMonitorBridge(QObject* parent = nullptr);
    ~SourceMonitorBridge() override;
    static SourceMonitorBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setProgramPlayback(PlaybackBridge* playback);
    /// 0.2: imports a Media Browser file (its path as loadFile got it) and returns its item (LibraryBridge sets it).
    using Importer = std::function<std::optional<MediaId>(const QString& file)>;
    void setImporter(Importer importer) { m_importer = std::move(importer); }

    // ---- C++ (tests) ----
    MediaId mediaId() const { return m_media; }
    Ticks positionTicks() const;
    /// Moves to a source time (paused), snapped to the source's frames.
    void seek(Ticks t);
    /// P0 round (the source clip transcript): marks source [in, out) on the loaded source, frame-snapped as loadRange
    /// does (In = the start of the frame holding `in`, Out = the end of the frame holding the range's last tick) and
    /// clamped to the media: one undo step "Set Source Marks" (source.setMarks); marks that are already those change
    /// nothing. False (with `message`) without a loaded source or for an empty range.
    bool setMarks(Ticks in, Ticks out);
    /// The source's frame rate (source timecodes, frame snapping).
    Rate rate() const { return m_rate; }
    std::optional<Ticks> lengthTicks() const { return m_length; }
    editor::PlaybackSession* playback() { return m_playback.get(); }
    /// The private one-clip project the Source Monitor plays (its picture, with the source's redactions), or null.
    ProjectSnapshot sourceProject() const;
    /// The patching source edits use for `media` in `sequence` (SourceMonitorServices).
    std::vector<command::SourcePatchSetting> patching(SequenceId sequence, MediaId media) const;
    /// Runs a source command on the project (the loaded source and the current sequence filled in).
    command::CommandResult run(const std::string& name, command::Json params, bool dryRun = false);

    // ---- Properties ----
    bool loaded() const { return m_media.isValid() || m_pending.has_value(); }
    bool preview() const { return m_pending.has_value(); }
    QString mediaKeyText() const;
    QString name() const { return m_name; }
    QString kind() const { return m_kind; }
    bool hasVideo() const { return m_hasVideo; }
    bool hasAudio() const { return m_hasAudio; }
    double aspect() const { return m_aspect; }
    QString durationText() const;
    QString positionText() const;
    double positionFraction() const;
    bool hasIn() const { return marks().in.has_value(); }
    bool hasOut() const { return marks().out.has_value(); }
    double inFraction() const;
    double outFraction() const;
    QString inText() const;
    QString outText() const;
    QString inOutText() const;
    bool playing() const;
    bool loop() const { return m_loop; }
    void setLoop(bool loop);
    QVariantList recent() const;
    QVariantList indicators() const;
    QVariantMap patchMap() const;
    bool canEdit() const;
    QString message() const { return m_message; }

    // ---- QML: the source ----
    /// Loads a library item ("m12"); false (with `message`) for unknown or offline media.
    Q_INVOKABLE bool load(const QString& media);
    /// 0.4: loads a media item ("m12") with its source In / Out set to [sourceIn, sourceOut) (tick strings, as
    /// SmartSearch cards carry them), frame-snapped (In = the start of the frame holding sourceIn, Out = the end of the
    /// frame holding its last tick) and clamped to the media, and moves to the In. The marks are one undo step ("Set
    /// Source Marks"). False (with `message`) for an unknown or offline item or an empty range.
    Q_INVOKABLE bool loadRange(const QString& media, const QString& sourceIn, const QString& sourceOut);
    /// 0.2 Media Browser: shows `file` (a file, or the first file of a camera card's clip) without importing it; false
    /// (with `message`) when it is not media Wabcut reads.
    Q_INVOKABLE bool loadFile(const QString& file);
    /// 0.2 UX (stream T), files dropped from File Explorer onto the Source Monitor: loads the project's media item for
    /// `file` (a file URL or a local path; the item whose file resolves to it), as load() does - QML imports the files
    /// first (ProjectFile.importFiles) and then calls this with the first one. False (with `message`) when the project
    /// has no item for that file.
    Q_INVOKABLE bool loadFileItem(const QString& file);
    Q_INVOKABLE void unload();
    /// The Source Monitor panel's picture (a ProgramMonitor).
    Q_INVOKABLE void attachMonitor(QObject* monitor);

    // ---- QML: transport ----
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    /// J/K/L: -1, 0, +1 (Premiere's doubling ladder).
    Q_INVOKABLE void shuttle(int direction);
    /// KeymapDispatcher.transportIntent(kind, speedNum, speedDen, frames) while the Source Monitor has focus.
    Q_INVOKABLE void applyTransportIntent(int kind, int speedNum, int speedDen, int frames);
    Q_INVOKABLE void stepFrames(int frames);
    Q_INVOKABLE void goToStart();
    Q_INVOKABLE void goToEnd();
    /// The mini timeline and the waveform: jump to (seek) or scrub through a fraction of the source.
    Q_INVOKABLE void seekFraction(double fraction);
    Q_INVOKABLE void scrubFraction(double fraction);
    /// 0.2 UX (stream T), the Source Monitor timecode's typed entry (Premiere): pauses and moves to `text`, a source
    /// timecode as positionText shows it ("00:00:02:10", "2:10", packed digits "210", ';' '.' ',' as separators, an
    /// empty field is "00": "2." = two seconds), or with a leading '+' / '-' an offset from the frame shown ("+15",
    /// "-1:00"). Clamped to the source. False (nothing moves) without a source or for text it cannot read.
    Q_INVOKABLE bool seekToText(const QString& text);

    // ---- QML: marks ----
    Q_INVOKABLE void markIn();
    Q_INVOKABLE void markOut();
    /// M (M5i): a clip marker on the loaded source at the frame shown (source.addMarker).
    Q_INVOKABLE bool addMarker();
    /// X: the whole source.
    Q_INVOKABLE void markClip();
    Q_INVOKABLE void clearIn();
    Q_INVOKABLE void clearOut();
    Q_INVOKABLE void clearInOut();
    Q_INVOKABLE void goToIn();
    Q_INVOKABLE void goToOut();
    /// Plays from the In (or the start) and stops at the Out (or the end).
    Q_INVOKABLE void playInToOut();
    /// 0.4, Premiere's Play Around (Shift+K) with the Source Monitor focused: plays from the frame shown minus
    /// `prerollSeconds` to it plus `postrollSeconds` once (Loop ignored), clamped to the source, then parks back on the
    /// frame shown. Space / K / J / L or a seek during it stop it where it is.
    Q_INVOKABLE void playAround(double prerollSeconds, double postrollSeconds);
    /// 0.4, Premiere's Play from Playhead to Out (Ctrl+Space): plays from the frame shown to the source Out (no Out: the
    /// end) once and stops on the last marked frame. At or past the Out: nothing.
    Q_INVOKABLE void playToOut();

    /// A keymap command while the Source Monitor has focus (marks, transport, Reverse Match Frame); false when it is
    /// not a Source Monitor command (the caller routes it on).
    Q_INVOKABLE bool runCommand(const QString& id);

    // ---- QML: editing ----
    /// True when the next Insert/Overwrite is a four-point edit whose ranges differ (it needs a Fit Clip choice).
    Q_INVOKABLE bool needsFit() const;
    /// Insert (true) or Overwrite from the source. `fit` = "" | "speed" | "ignoreSourceIn" | "ignoreSourceOut" |
    /// "ignoreSequenceIn" | "ignoreSequenceOut". Returns "" when done, "fit" when a Fit Clip choice is needed, else
    /// the problem (also in `message`).
    Q_INVOKABLE QString edit(bool insert, const QString& fit = {});
    /// The edit as a ghost on the timeline (hovering Insert/Overwrite); clearPreview() takes it away.
    Q_INVOKABLE void previewEdit(bool insert, const QString& fit = {});
    Q_INVOKABLE void clearPreview();
    /// A drop from the Source Monitor at a timeline timecode on a track ("t3"; "" = the default patching): the
    /// marked range, `streams` = "av" | "video" | "audio". Overwrite, or insert (Ctrl; Main.qml picks). Placed where
    /// dropped; the streams of the other kind go to the matching track or a free one (editor::patchesForDrop).
    Q_INVOKABLE bool dropAt(const QString& timecode, const QString& track, bool overwrite, const QString& streams);

    // ---- QML: patching and targeting (the track headers) ----
    /// A click on a track's empty source slot: a source track of its kind moves there.
    Q_INVOKABLE void patchTrack(const QString& track);
    /// Drag: source track "V1" / "A2" goes to `track` (an indicator already there swaps places).
    Q_INVOKABLE void movePatch(const QString& source, const QString& track);
    /// Alt-click: switches an indicator off or on.
    Q_INVOKABLE void togglePatch(const QString& source);
    /// The track header's target toggle: sequence.setTargets.
    Q_INVOKABLE void setTargeted(const QString& track, bool targeted);

    // ---- QML: match frame ----
    /// F: the clip under the program playhead (targeted tracks), loaded at that frame with its range marked.
    Q_INVOKABLE bool matchFrame();
    /// Shift+R: moves the program playhead to where the source frame shown is used.
    Q_INVOKABLE bool reverseMatchFrame();

signals:
    void sourceChanged();
    void positionChanged();
    void marksChanged();
    void transportChanged();
    void recentChanged();
    void patchingChanged();
    void messageChanged();
    void loopChanged();
    /// A source was loaded (Library, Match Frame, recent sources), Reverse Match Frame moved the program playhead:
    /// the narrow layout shows the monitor it concerns.
    void showSourceRequested();
    void showProgramRequested();

private:
    class Services;

    edit::SourceMarks marks() const;
    const MediaItem* mediaItem() const;
    Ticks frame() const;
    double fractionOf(Ticks t) const;
    QString timecodeOf(Ticks t) const;
    bool loadMedia(MediaId media, std::optional<Ticks> at);
    /// Plays `item` (the project's item `projectMedia`, or a preview's description with none) from `at`.
    bool loadItem(const MediaItem& item, MediaId projectMedia, std::optional<Ticks> at, const QString& kind);
    /// A preview's first use: imports it (the importer) and goes on with the item. True when there is no preview.
    bool adoptPreview();
    std::vector<command::SourcePatchSetting> patchingFor(SequenceId sequence, const MediaItem& item) const;
    void teardown();
    void tick();
    void startAudio();
    /// 0.5 (stream B): audio scrubbing in the Source Monitor - the clip's own sound at `at` (`direction` < 0: played
    /// backwards from there), through the program's switch and request (PlaybackBridge::requestAudioScrub), while stopped.
    void scrubAudioAt(Ticks at, int direction);
    void setMessage(const QString& message);
    /// Hands the loop range (In to Out of the source) to the playback, or none; with `starting` and the position
    /// outside the range, moves to its start (P.2).
    void applyLoop(bool starting = false);
    void toggleLoop() { setLoop(!m_loop); }
    bool setMarks(command::Json params);
    /// [in, out) frame-snapped and clamped as setMarks(Ticks, Ticks) marks it, or nullopt when nothing is left.
    std::optional<std::pair<Ticks, Ticks>> snappedMarks(Ticks in, Ticks out) const;
    void onProjectChanged();
    /// The project's redactions of the loaded media, re-pointed at the private copy (I.6: the source shows them).
    void syncRedactions();
    /// The stored patching of the current sequence, completed for the loaded source.
    std::vector<command::SourcePatchSetting> currentPatching() const;
    void storePatching(std::vector<command::SourcePatchSetting> patches);
    command::Json patchesJson(const std::vector<command::SourcePatchSetting>& patches) const;
    command::Json editParams(const QString& fit) const;
    void showPreview(const command::CommandResult& dryRun, bool insert);

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_programPlayback;
    QPointer<ProgramMonitor> m_monitor;
    std::shared_ptr<Services> m_services;

    // The loaded source and its private one-clip project.
    MediaId m_media;
    MediaId m_copyMedia; ///< the media item's id in the private project
    /// 0.2: a Media Browser preview - the file and its description - until it is imported.
    struct Preview {
        QString file;
        MediaItem item;
    };
    std::optional<Preview> m_pending;
    Importer m_importer;
    /// The project's redaction ids -> their copies' ids in the private project, allocated there (its ids start at 1, so
    /// keeping the project's would collide with its media, sequence, tracks or clips; Codex wave-4 P2-38). Stable while
    /// the source stays loaded, so an unchanged set of redactions makes no new commit.
    std::map<std::uint64_t, RedactionId> m_redactionIds;
    QString m_name;
    QString m_kind;
    bool m_hasVideo = false;
    bool m_hasAudio = false;
    double m_aspect = 16.0 / 9.0;
    Rate m_rate{30, 1};
    std::optional<Ticks> m_length;
    std::unique_ptr<EditSession> m_sourceSession;
    std::unique_ptr<editor::PlaybackSession> m_playback;
    bool m_audioTried = false;

    QTimer m_tick;
    bool m_wasPlaying = false;
    std::optional<Ticks> m_stopAt; ///< Play In to Out
    std::optional<Ticks> m_returnTo; ///< 0.4 Play Around: where playback parks when it reaches m_stopAt
    bool m_loop = false;           ///< P.2: Premiere's Loop
    Ticks m_lastPosition{-1};
    std::vector<MediaId> m_recent;
    /// Per sequence: the source track indicators (V1, A1, A2, ...), kept across sources.
    std::map<std::uint64_t, std::vector<command::SourcePatchSetting>> m_patching;
    bool m_previewShown = false;
    QString m_message;
};

/// QML: the singleton `SourceMonitor`.
struct SourceMonitorBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::SourceMonitorBridge)
    QML_NAMED_ELEMENT(SourceMonitor)
    QML_SINGLETON
public:
    static SourceMonitorBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return SourceMonitorBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
