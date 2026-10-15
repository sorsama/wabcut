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
//   * Source view (library double-click): showSource() plays one media item on its own (video only, free-run
//     clock) in the attached monitors until showProgram().

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
#include <memory>

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
    QString decodeBackend() const { return m_decodeBackend; }
    QString audioBackend() const { return m_audioBackend; }
    QString statsText() const { return m_statsText; }
    bool sourceMode() const { return m_sourceVideo != nullptr; }
    QString sourceName() const { return m_sourceName; }
    QString sourceTimeText() const;

    // ---- QML ----
    /// Shows this bridge's playback in `monitor` (a ProgramMonitor).
    Q_INVOKABLE void attachMonitor(QObject* monitor);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    /// The transport bar's J/K/L buttons: -1 = J, 0 = K (stop), +1 = L (Premiere's doubling ladder).
    Q_INVOKABLE void shuttle(int direction);
    /// KeymapDispatcher.transportIntent(kind, speedNum, speedDen, frames).
    Q_INVOKABLE void applyTransportIntent(int kind, int speedNum, int speedDen, int frames);
    Q_INVOKABLE void stepFrames(int frames);
    /// Source view of a library item ("m…"); showProgram() returns to the sequence.
    Q_INVOKABLE bool showSource(const QString& mediaId);
    Q_INVOKABLE void showProgram();

signals:
    /// The playback session is about to go (the project closes, or this bridge does), emitted while it still exists:
    /// a voice-over pass ends here and places its take (review A5-P1-7). Direct connections only.
    void sessionEnding();
    void transportChanged();
    void statsChanged();
    void captionsChanged();
    void sourceChanged();
    void sourceTimeChanged();

private:
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

    // Source view
    std::unique_ptr<EditSession> m_sourceSession;
    std::unique_ptr<playback::PlaybackController> m_sourceVideo;
    QString m_sourceName;

    QTimer m_tick;
    bool m_settingPlayhead = false;
    bool m_wasPlaying = false;
    std::int32_t m_lastSpeedNum = 0;
    std::int32_t m_lastSpeedDen = 1;

    // Stats
    QElapsedTimer m_statsClock;
    std::uint64_t m_presentedAtLastSample = 0;
    std::uint64_t m_droppedAtLastSample = 0;
    int m_bypassedEffects = 0;
    double m_measuredFps = 0.0;
    int m_dropped = 0;
    QString m_fpsText;
    QString m_decodeBackend;
    QString m_audioBackend;
    QString m_statsText;
};

} // namespace wabcut::app
