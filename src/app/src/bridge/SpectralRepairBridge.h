#pragma once

// SpectralRepairBridge (0.7, card 047; QML singleton `SpectralRepair`): the Spectral tab of Audio Tools. It follows the clip
// the user is working on (the selected audio clip, or the audio of a selected video clip), keeps the settings of the
// spectrogram view (FFT size, frequency axis, colour range, channel, zoom and scroll, original or result) and turns the
// selection tools' regions into edits of the clip's Spectral Repair effect (audio.spectralAttenuate / Heal / NoiseFloor /
// RemoveTone: one undo step each; wabcut/model/SpectralRepair.h).
//
// The analysis behind the view (SpectrogramCache, wabcut/audio/dsp/Spectrogram.h) reads the clip's media through the player's
// own decoders, mixed to mono (or one channel), in tiles that are computed off the GUI thread by the view item
// (SpectrogramView) and kept within a memory budget. "Result" shows the sound as the clip's edits repair it: the same
// repair code the player runs, over the media, so the effect of an edit can be seen before it is heard.
//
// Times are SECONDS OF THE CLIP'S SOURCE (counted from the start of the media), frequencies Hz of the source sound; the
// clip's edits are stored that way, so they stay on the sound when the clip is trimmed, moved or retimed.

#include <wabcut/audio/dsp/Spectrogram.h>
#include <wabcut/command/Command.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/SpectralRepair.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class PlaybackBridge;
class ProjectController;

/// The clip the tab works on and the sound it plays.
struct SpectralClip {
    ClipId clip;
    MediaId media;
    int streamOrdinal = 0;
    int sampleRate = 48000;
    int channels = 2;
    std::int64_t lengthSamples = 0; ///< of the media's audio stream
    double rangeStart = 0.0;        ///< seconds of the source the clip plays
    double rangeEnd = 0.0;
    QString name;
};

class SpectralRepairBridge : public QObject {
    Q_OBJECT

    /// Whether there is a clip to repair, and why not ("Select an audio clip ...").
    Q_PROPERTY(bool available READ available NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString reason READ reason NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString clip READ clip NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString clipName READ clipName NOTIFY clipChanged FINAL)
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY clipChanged FINAL)
    Q_PROPERTY(int channels READ channels NOTIFY clipChanged FINAL)
    /// The seconds of the source the clip plays: what the view can show.
    Q_PROPERTY(double rangeStart READ rangeStart NOTIFY clipChanged FINAL)
    Q_PROPERTY(double rangeEnd READ rangeEnd NOTIFY clipChanged FINAL)

    // ---- The view ----
    /// 1024, 2048 or 4096. With a Spectral Repair on the clip it is the effect's FFT size (changing it is an undo step).
    Q_PROPERTY(int fftSize READ fftSize WRITE setFftSize NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool logFrequency READ logFrequency WRITE setLogFrequency NOTIFY viewChanged FINAL)
    /// The sound as the edits repair it, instead of the original.
    Q_PROPERTY(bool showResult READ showResult WRITE setShowResult NOTIFY viewChanged FINAL)
    /// -1: all channels mixed; 0..channels-1: one channel.
    Q_PROPERTY(int channel READ channel WRITE setChannel NOTIFY viewChanged FINAL)
    /// The levels the colours span, dB (re a full-scale sine).
    Q_PROPERTY(double floorDb READ floorDb WRITE setFloorDb NOTIFY viewChanged FINAL)
    Q_PROPERTY(double ceilingDb READ ceilingDb WRITE setCeilingDb NOTIFY viewChanged FINAL)
    Q_PROPERTY(double viewStart READ viewStart WRITE setViewStart NOTIFY viewChanged FINAL)
    Q_PROPERTY(double viewSpan READ viewSpan WRITE setViewSpan NOTIFY viewChanged FINAL)
    /// Where the timeline's playhead is on the source, seconds (-1: not on the clip).
    Q_PROPERTY(double playhead READ playhead NOTIFY playheadChanged FINAL)
    /// Bumped whenever the analysis the view draws changes (the clip, the FFT size, the channel, the edits in Result).
    Q_PROPERTY(int analysisRevision READ analysisRevision NOTIFY analysisChanged FINAL)

    // ---- The clip's repairs ----
    /// [{ index, kind, shape, label, start, end, low, high, gainDb }], in order.
    Q_PROPERTY(QVariantList edits READ edits NOTIFY editsChanged FINAL)
    Q_PROPERTY(bool hasRepair READ hasRepair NOTIFY editsChanged FINAL)
    /// A/B: false plays the clip without its repairs.
    Q_PROPERTY(bool repairEnabled READ repairEnabled NOTIFY editsChanged FINAL)
    /// 0 repaired sound, 1 only the selected sound, 2 only what the repair removes.
    Q_PROPERTY(int listen READ listen NOTIFY editsChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit SpectralRepairBridge(QObject* parent = nullptr);
    static SpectralRepairBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    /// Tests: the sound of a clip (mono, or `channel`) instead of the project's media.
    using SourceFactory = std::function<std::shared_ptr<audio::dsp::SampleSource>(const SpectralClip& clip, int channel)>;
    void setSourceFactory(SourceFactory factory);
    /// Tests: a clip to work on without a project's media (the same data a project clip gives).
    void setClipOverride(std::optional<SpectralClip> clip);

    bool available() const { return m_available; }
    QString reason() const { return m_reason; }
    QString clip() const;
    QString clipName() const { return m_clip.name; }
    int sampleRate() const { return m_clip.sampleRate; }
    int channels() const { return m_clip.channels; }
    double rangeStart() const { return m_clip.rangeStart; }
    double rangeEnd() const { return m_clip.rangeEnd; }
    int fftSize() const { return m_fftSize; }
    void setFftSize(int size);
    bool logFrequency() const { return m_log; }
    void setLogFrequency(bool log);
    bool showResult() const { return m_showResult; }
    void setShowResult(bool show);
    int channel() const { return m_channel; }
    void setChannel(int channel);
    double floorDb() const { return m_floorDb; }
    void setFloorDb(double db);
    double ceilingDb() const { return m_ceilingDb; }
    void setCeilingDb(double db);
    double viewStart() const { return m_viewStart; }
    void setViewStart(double seconds);
    double viewSpan() const { return m_viewSpan; }
    void setViewSpan(double seconds);
    double playhead() const { return m_playhead; }
    int analysisRevision() const { return m_revision; }
    QVariantList edits() const { return m_editsVariant; }
    bool hasRepair() const { return m_hasRepair; }
    bool repairEnabled() const { return m_repairEnabled; }
    int listen() const { return m_listen; }
    QString message() const { return m_message; }

    /// The clip's edits as the model has them (the view draws them).
    const std::vector<SpectralEdit>& editList() const { return m_edits; }
    /// The analysis the view draws (original or result), made on first use; null without a clip.
    std::shared_ptr<audio::dsp::SpectrogramCache> displayCache();

    /// Adds an edit to the clip: `kind` "attenuate" | "heal" | "noiseFloor" | "tone"; `shape` "rectangle" | "lasso" | "brush";
    /// `points` [{time, hz}] (rectangle: two opposite corners); `options` { gainDb, amount, radiusSeconds, radiusOctaves,
    /// harmonics, widthHz, hz, logFrequency, start, end }. One undo step. Returns "" or why not.
    Q_INVOKABLE QString addEdit(const QString& kind, const QString& shape, const QVariantList& points, const QVariantMap& options);
    Q_INVOKABLE QString removeEdit(int index);
    Q_INVOKABLE QString clearEdits();
    /// A/B: plays the clip without (false) or with (true) its repairs. One undo step.
    Q_INVOKABLE QString setRepairEnabled(bool enabled);
    /// 0 repaired sound, 1 only the selected sound, 2 only what the repair removes (to judge an edit by ear). One undo step.
    Q_INVOKABLE QString setListen(int mode);
    /// The frequency of the steadiest strong tone in [low, high] Hz over [start, end] s of what the view analysed, or 0
    /// when the part is not analysed yet or holds nothing standing out.
    Q_INVOKABLE double findTone(double start, double end, double low, double high);
    /// The loudest level (dB) the view has analysed within [start, end] s and [low, high] Hz, or -1000.
    Q_INVOKABLE double peakLevel(double start, double end, double low, double high);

    Q_INVOKABLE void zoomView(double factor, double anchorSeconds);
    Q_INVOKABLE void scrollView(double seconds);
    Q_INVOKABLE void fitView();
    /// Moves the timeline's playhead to this source time (on the clip).
    Q_INVOKABLE void seekTo(double sourceSeconds);
    /// Plays the part of the clip between these source times once (the playhead returns where it was).
    Q_INVOKABLE QString playSelection(double startSeconds, double endSeconds);
    /// Asks the shell to show the Spectral tab (the clip menu's Spectral Repair).
    Q_INVOKABLE void open();
    /// Duration text: "0.45 s" / "120 ms".
    Q_INVOKABLE QString durationText(double seconds) const;

signals:
    void clipChanged();
    void viewChanged();
    void playheadChanged();
    void editsChanged();
    void messageChanged();
    void analysisChanged();
    void openRequested();

private:
    void refreshClip();
    void refreshEdits();
    void refreshPlayhead();
    void setMessage(const QString& message);
    void invalidateCaches(bool original, bool result);
    void clampView();
    QString run(const std::string& command, command::Json params);
    command::Json clipParams() const;
    std::shared_ptr<audio::dsp::SampleSource> makeSource(int channel);

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_playback;
    SourceFactory m_sourceFactory;
    std::optional<SpectralClip> m_override;

    bool m_available = false;
    QString m_reason;
    SpectralClip m_clip;
    int m_fftSize = 2048;
    bool m_log = true;
    bool m_showResult = false;
    int m_channel = -1;
    double m_floorDb = -100.0;
    double m_ceilingDb = -12.0;
    double m_viewStart = 0.0;
    double m_viewSpan = 10.0;
    double m_playhead = -1.0;
    int m_revision = 0;

    std::vector<SpectralEdit> m_edits;
    QVariantList m_editsVariant;
    bool m_hasRepair = false;
    bool m_repairEnabled = true;
    int m_listen = 0;
    std::uint64_t m_editsHash = 0;
    QString m_message;

    std::shared_ptr<audio::dsp::SpectrogramCache> m_original;
    std::shared_ptr<audio::dsp::SpectrogramCache> m_result;
    std::uint64_t m_resultHash = 0;
};

struct SpectralRepairBridgeForeign {
    Q_GADGET
    QML_FOREIGN(SpectralRepairBridge)
    QML_NAMED_ELEMENT(SpectralRepair)
    QML_SINGLETON
public:
    static SpectralRepairBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return SpectralRepairBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
