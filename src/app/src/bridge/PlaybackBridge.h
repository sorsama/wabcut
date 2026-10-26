#pragma once

// PlaybackBridge: picture and sound for ProjectController's sequence, for QML (singleton `Playback`).
// Owner: Graphics/playback (I.1). It owns an editor::PlaybackSession (AudioEngine as the master clock,
// TimelineAudioPlayer + MediaAudioFeed, video PlaybackController) over ProjectController's EditSession.
//
//   * Transport: space (togglePlay), J/K/L through KeymapDispatcher's transport intents (applyTransportIntent),
//     frame steps. The audio device opens on the first play; WABCUT_AUDIO_BACKEND=null | null-realtime selects a
//     silent engine (tests, headless machines).
//   * The ProjectController playhead follows playback while playing; when paused, moving the playhead (ruler
//     scrub, arrows, Home/End) scrubs the monitor to that frame.
//   * Stats for the monitor's readout: measured fps while playing, dropped frames since play started, decode /
//     render / audio backends.
//   * Program monitors attach with attachMonitor(); zero-copy decode is on for them (WABCUT_ZERO_COPY=0 turns
//     it off).
//   * Source view (legacy): sourceMode / sourceName / sourceTimeText and showProgram() remain for the panels that still
//     read them; 0.4 removed showSource() (its only caller, the smart-search card, opens hits in the Source Monitor:
//     SourceMonitor.loadRange).
//   * 0.4: Premiere's Play Around (Shift+K: playAround) and Play from Playhead to Out (Ctrl+Space: playToOut) play
//     once (Loop ignored); Play Around parks back on the playhead it started from.
//   * P.2: a second program output (the full-screen window, Premiere's Toggle Full Screen and Transmit): attachMirror()
//     shows the program in another monitor through a mirror controller (playback::PlaybackController::addMirror): the
//     same transport and edits, its own decoding in CPU frames (that window's GPU device is not the decoders').
//   * P.2: Premiere's Loop (`loop`, remembered): playback repeats the sequence In to Out, or the whole sequence without
//     both marks; Play from outside that range starts at its beginning. playInToOut() (Ctrl+Shift+Space) plays In to
//     Out once, or repeats it with Loop on.
//   * 0.5: Premiere's audio scrubbing (`audioScrubbing`, Toggle Audio Scrubbing Shift+S, Settings > Timeline "Play audio
//     while scrubbing"; remembered, on by default as in Premiere): while stopped, a playhead dragged in the ruler or the
//     Program Monitor's time bar (between beginScrub() and endScrub()) and a frame step (stepFrames(), beginStep() /
//     endStep()) play a short window of the
//     sequence mix at the new frame (editor::PlaybackSession::scrubAudio: off the GUI thread, rate-limited). The
//     transport and the picture stay where the playhead is. A drag that goes back plays the window before the playhead
//     backwards (Premiere's backward scrub); steps play the frame stepped to forward. The Source Monitor scrubs its
//     clip the same way through requestAudioScrub() (same switch).

#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Time.h>

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace wabcut::playback {
class PlaybackController;
struct CaptionWordTimings;
}

namespace wabcut::ai::edit {
class SequenceTranscript;
}

namespace wabcut::app {

/// Karaoke word timings of `track`'s cues from the sequence's `transcript`: a cue gets them when the transcript's words
/// in its time line up one-to-one with the cue's words. The program monitor and a burn-in export use this same map
/// (Codex wave-3 P2-8: the export highlights the words the preview highlights).
std::shared_ptr<const playback::CaptionWordTimings> captionWordTimings(const CaptionTrack& track,
                                                                       const ai::edit::SequenceTranscript& transcript);

class CaptionsBridge;
class ProgramMonitor;
class TranscriptBridge;
class ProjectController;

class PlaybackBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Playback)
    QML_SINGLETON

    Q_PROPERTY(bool playing READ playing NOTIFY transportChanged FINAL)
    /// Signed speed as a multiple of real time (0 while stopped), e.g. 2, -1, 0.25.
    Q_PROPERTY(double speed READ speed NOTIFY transportChanged FINAL)
    Q_PROPERTY(int speedNum READ speedNum NOTIFY transportChanged FINAL)
    Q_PROPERTY(int speedDen READ speedDen NOTIFY transportChanged FINAL)
    /// "■ 0×", "▶ 1×", "◀ 2×", "▶ ¼×".
    Q_PROPERTY(QString speedText READ speedText NOTIFY transportChanged FINAL)
    /// Measured presentation rate while playing ("59.94 fps"; "— fps" while nothing is presented, e.g. a hidden
    /// monitor); the sequence rate while stopped.
    Q_PROPERTY(QString fpsText READ fpsText NOTIFY statsChanged FINAL)
    /// Frames dropped while the monitor was presenting, since play started.
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY statsChanged FINAL)
    /// Effects in the picture that cannot be rendered (unknown or from a newer version): a warning badge.
    Q_PROPERTY(int bypassedEffects READ bypassedEffects NOTIFY statsChanged FINAL)
    /// P.2: motion is being analysed (Morph Cut, Optical Flow; playback::MotionAnalyzer): Premiere's "Analyzing in
    /// background" banner on the program monitor.
    Q_PROPERTY(bool analyzingMotion READ analyzingMotion NOTIFY statsChanged FINAL)
    /// Captions are drawn by the compositor (I.4): the Captions panel's track and style, shown while its "show in
    /// monitor" toggle is on. Karaoke highlights the words already spoken (transcript word times when they line up
    /// with the cue, else estimated); burned-in exports follow both.
    Q_PROPERTY(bool captionsShown READ captionsShown NOTIFY captionsChanged FINAL)
    Q_PROPERTY(bool captionKaraoke READ captionKaraoke WRITE setCaptionKaraoke NOTIFY captionsChanged FINAL)
    Q_PROPERTY(QString decodeBackend READ decodeBackend NOTIFY statsChanged FINAL)
    Q_PROPERTY(QString audioBackend READ audioBackend NOTIFY statsChanged FINAL)
    /// Multi-line diagnostics for a tooltip.
    Q_PROPERTY(QString statsText READ statsText NOTIFY statsChanged FINAL)
    Q_PROPERTY(bool sourceMode READ sourceMode NOTIFY sourceChanged FINAL)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY sourceChanged FINAL)
    Q_PROPERTY(QString sourceTimeText READ sourceTimeText NOTIFY sourceTimeChanged FINAL)
    /// P.2: Premiere's Loop button in the Program Monitor.
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged FINAL)
    /// 0.5: Premiere's Play Audio While Scrubbing (Toggle Audio Scrubbing, Shift+S), remembered; on by default.
    Q_PROPERTY(bool audioScrubbing READ audioScrubbing WRITE setAudioScrubbing NOTIFY audioScrubbingChanged FINAL)
    /// 0.5: windows of audio scrubbing asked for so far (tests).
    Q_PROPERTY(int audioScrubRequests READ audioScrubRequests NOTIFY audioScrubbed FINAL)

public:
    explicit PlaybackBridge(ProjectController* controller, QObject* parent = nullptr);
    ~PlaybackBridge() override;
    static PlaybackBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    // ---- C++ ----
    /// The program's playback (null after the project controller went away).
    editor::PlaybackSession* session() { return m_session.get(); }
    /// The controller attached monitors show now (program or source).
    playback::PlaybackController* activeController() const;
    /// Starts the audio engine now instead of on the first play (tests use it with the Null backend).
    void startAudio();
    /// Starts the audio device now, whatever the project holds (startAudio() waits for media with sound): voice-over
    /// recording runs on the output's clock, over silent media too. Tried again on each call until it starts. True when
    /// the session has sound.
    bool ensureAudio();
    /// What the program and source monitors decode: proxies or originals, decode resolution, the proxy registry
    /// (set by MediaReadiness). Kept for source views opened later.
    void setPreviewSettings(playback::PreviewSettings preview);
    const playback::PreviewSettings& previewSettings() const { return m_preview; }
    /// The Color workspace's views of the program (I.5): the HSL matte, before/after split, a reference side by side.
    void setColorPreview(playback::ColorPreview preview);
    const playback::ColorPreview& colorPreview() const { return m_colorPreview; }
    /// What the program monitor draws as captions (nullopt: none).
    const std::optional<playback::CaptionSettings>& captionSettings() const { return m_captionSettings; }
    /// Looks up the Captions and Transcript singletons (done once after construction) and applies their state.
    void attachCaptions();
    bool captionsShown() const { return m_captionSettings.has_value(); }
    bool captionKaraoke() const { return m_captionKaraoke; }
    void setCaptionKaraoke(bool karaoke);

    // ---- Properties ----
    bool playing() const;
    double speed() const;
    int speedNum() const;
    int speedDen() const;
    QString speedText() const;
    QString fpsText() const { return m_fpsText; }
    int droppedFrames() const { return m_dropped; }
    int bypassedEffects() const { return m_bypassedEffects; }
    bool analyzingMotion() const { return m_analyzingMotion; }
    QString decodeBackend() const { return m_decodeBackend; }
    QString audioBackend() const { return m_audioBackend; }
    QString statsText() const { return m_statsText; }
    bool sourceMode() const { return m_sourceVideo != nullptr; }
    QString sourceName() const { return m_sourceName; }
    QString sourceTimeText() const;
    bool loop() const { return m_loop; }
    void setLoop(bool loop);
    bool audioScrubbing() const { return m_audioScrubbing; }
    void setAudioScrubbing(bool on);
    int audioScrubRequests() const { return m_audioScrubRequests; }
    /// 0.5: where the last scrub window was asked for and its direction (tests; nullopt: none yet).
    std::optional<Ticks> lastAudioScrub() const { return m_lastAudioScrub; }
    int lastAudioScrubDirection() const { return m_lastAudioScrubDirection; }
    /// 0.5 (tests): what a session's scrubAudio is asked instead (the app: the session's own). `session`: the program's
    /// or the Source Monitor's.
    using AudioScrubSink = std::function<bool(editor::PlaybackSession* session, Ticks at, int direction)>;
    void setAudioScrubSink(AudioScrubSink sink) { m_audioScrubSink = std::move(sink); }
    /// 0.5: a scrub window of `session` (the program's, or the Source Monitor's clip) at `at`, `direction` < 0 played
    /// backwards, when audio scrubbing is on and `session` is stopped (through the test sink when one is set). Counted
    /// in audioScrubRequests. Returns whether one was asked for.
    bool requestAudioScrub(editor::PlaybackSession* session, Ticks at, int direction);
    /// The range playback repeats with Loop on: the sequence In to Out, else the whole sequence (nullopt: no sequence).
    std::optional<TimeRange> loopRange() const;

    // ---- QML ----
    /// Shows this bridge's playback in `monitor` (a ProgramMonitor).
    Q_INVOKABLE void attachMonitor(QObject* monitor);
    /// P.2: shows the program in `monitor` (a ProgramMonitor in another window) through a mirror; detachMirror() lets go.
    Q_INVOKABLE void attachMirror(QObject* monitor);
    Q_INVOKABLE void detachMirror(QObject* monitor);
    /// The mirror controller while a mirror is attached (tests).
    playback::PlaybackController* mirror() const { return m_mirror.get(); }
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    /// The transport bar's J/K/L buttons: -1 = J, 0 = K (stop), +1 = L (Premiere's doubling ladder).
    Q_INVOKABLE void shuttle(int direction);
    /// KeymapDispatcher.transportIntent(kind, speedNum, speedDen, frames).
    Q_INVOKABLE void applyTransportIntent(int kind, int speedNum, int speedDen, int frames);
    Q_INVOKABLE void stepFrames(int frames);
    /// P.2: plays from the In (or the start) to the Out (or the end) and stops there; with Loop on it repeats.
    Q_INVOKABLE void playInToOut();
    /// 0.4, Premiere's Play Around (Shift+K): plays from the playhead minus `prerollSeconds` to the playhead plus
    /// `postrollSeconds` once (Loop ignored; clamped at 0 and the sequence end), then parks back on the playhead it
    /// started from. Space / K / J / L or a seek during it stop it where it is (no park-back).
    Q_INVOKABLE void playAround(double prerollSeconds, double postrollSeconds);
    /// 0.4, Premiere's Play from Playhead to Out (Ctrl+Space): plays from the playhead to the sequence Out (no Out: the
    /// sequence end) once and stops on the last marked frame. At or past the Out: nothing. Loop is ignored.
    Q_INVOKABLE void playToOut();
    /// Returns the attached monitors from a legacy source view to the sequence.
    Q_INVOKABLE void showProgram();
    /// 0.5: a playhead drag starts (the ruler, the Program Monitor's time bar) or ends: while it lasts, the playhead
    /// changes of a stopped transport are scrubs and play audio (with audioScrubbing on).
    Q_INVOKABLE void beginScrub();
    Q_INVOKABLE void endScrub();
    /// 0.5 (fix round): the pointer moves in the scrub begun (the drag, after the press): from now on a move back plays
    /// backwards. The press itself (a click) plays the frame it lands on forward, wherever the playhead was.
    Q_INVOKABLE void scrubMoving();
    /// 0.5: a frame step (Main.qml's transport.step* commands) is under way: the frame stepped to is heard, forward.
    Q_INVOKABLE void beginStep();
    Q_INVOKABLE void endStep();

signals:
    /// The playback session is about to go (the project closes, or this bridge does), emitted while it still exists:
    /// a voice-over pass ends here and places its take (review A5-P1-7). Direct connections only.
    void sessionEnding();
    void transportChanged();
    void statsChanged();
    void captionsChanged();
    void sourceChanged();
    void sourceTimeChanged();
    void loopChanged();
    void audioScrubbingChanged();
    /// 0.5: a scrub window was asked for (audioScrubRequests).
    void audioScrubbed();

private:
    /// 0.5: plays the program's scrub window at `at` (audioScrubbing on, stopped, the program shown).
    void scrubAudioAt(Ticks at, int direction);
    void tick();
    void updateStats();
    void onPlayheadChanged();
    void onSequenceChanged();
    /// The sequence to play: the current one once the project has media files (the demo's media has none, so
    /// nothing is decoded for it).
    SequenceId playableSequence() const;
    void attachAll(playback::PlaybackController* controller);
    void teardown();
    bool setSpeed(int num, int den);
    /// Hands the loop range (or none) to the session; with Loop on and the playhead outside the range before forward
    /// play starts (`starting`), the playhead goes to its start.
    void applyLoop(bool starting = false);

    ProjectController* m_controller = nullptr;
    std::unique_ptr<editor::PlaybackSession> m_session;
    bool m_audioTried = false;
    QList<QPointer<ProgramMonitor>> m_monitors;

    playback::PreviewSettings m_preview;
    void applyCaptions();
    QPointer<CaptionsBridge> m_captions;
    QPointer<TranscriptBridge> m_transcriptBridge;
    bool m_captionKaraoke = false;
    std::optional<playback::CaptionSettings> m_captionSettings;
    playback::ColorPreview m_colorPreview;

    // P.2: the second output (full screen / transmit)
    std::unique_ptr<playback::PlaybackController> m_mirror;
    QPointer<ProgramMonitor> m_mirrorMonitor;

    // Source view
    std::unique_ptr<EditSession> m_sourceSession;
    std::unique_ptr<playback::PlaybackController> m_sourceVideo;
    QString m_sourceName;

    QTimer m_tick;
    bool m_settingPlayhead = false;
    bool m_wasPlaying = false;
    std::int32_t m_lastSpeedNum = 0;
    std::int32_t m_lastSpeedDen = 1;
    bool m_loop = false;
    std::optional<Ticks> m_stopAt; ///< Play In to Out without Loop: where playback stops
    std::optional<Ticks> m_returnTo; ///< 0.4 Play Around: where the playhead parks when playback reaches m_stopAt
    // 0.5: audio scrubbing
    bool m_audioScrubbing = true;
    int m_scrubGestures = 0;  ///< beginScrub() calls not ended yet
    bool m_scrubMoving = false; ///< the scrub is past its press (scrubMoving())
    bool m_stepping = false;  ///< stepFrames() moves the playhead now
    int m_audioScrubRequests = 0;
    std::optional<Ticks> m_lastAudioScrub;
    int m_lastAudioScrubDirection = 0;
    AudioScrubSink m_audioScrubSink;

    // Stats
    QElapsedTimer m_statsClock;
    std::uint64_t m_presentedAtLastSample = 0;
    std::uint64_t m_droppedAtLastSample = 0;
    int m_bypassedEffects = 0;
    bool m_analyzingMotion = false;
    double m_measuredFps = 0.0;
    int m_dropped = 0;
    QString m_fpsText;
    QString m_decodeBackend;
    QString m_audioBackend;
    QString m_statsText;
};

} // namespace wabcut::app
