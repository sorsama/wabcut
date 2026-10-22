#pragma once

// ClipMixerBridge (p02 audio): Premiere's Audio Clip Mixer, the QML singleton `ClipMixer`.
//
//   * One strip per audio track of the current sequence, for the clip under the playhead on it (none: the strip is
//     empty): the clip's Volume (dB) and Panner (balance) as a fader and a pan knob, the track's meter, mute and solo.
//     The strips follow the playhead.
//   * Paused (or playing without Write Keyframes): a fader move sets the clip's level, or on an animated clip the key at
//     the playhead (ProjectController::setParameterAtPlayhead); a drag is one undo step (endGesture()).
//   * Write Keyframes (per strip, Premiere's keyframe button on the clip mixer): while playing, the fader and pan knob
//     write keyframes into the clip while they are held, and the result is committed when playback stops: one undo
//     step per pass for every clip written. While writing, the change is heard at once through the track's live mix
//     override (TimelineAudioPlayer::setLiveTrackMix: the track fader plus the written change of the clip's level).
//
// Keys cross into QML as opaque strings ("t12", "c7"); no ticks (principle 9).

#include "MixerAutomation.h"

#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <map>
#include <optional>
#include <set>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class PlaybackBridge;
class ProjectController;

class ClipMixerBridge : public QObject {
    Q_OBJECT
    /// [{ key (the track: "t12"), shortName, name, colorIndex, clip ("c7" or ""), clipName, gainDb, pan, automated,
    ///    panAutomated, writeKeyframes, writing, muted, solo }], A1 first.
    Q_PROPERTY(QVariantList strips READ strips NOTIFY stripsChanged FINAL)

public:
    explicit ClipMixerBridge(QObject* parent = nullptr);
    ~ClipMixerBridge() override;

    static ClipMixerBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setPlayback(PlaybackBridge* playback);

    /// What a write pass sounds like while it records (tests); by default the player's live track override.
    using LiveMixSink = std::function<bool(TrackId track, std::optional<double> gainDb)>;
    void setLiveMixSink(LiveMixSink sink) { m_liveMixSink = std::move(sink); }
    /// Tests: playing or not without a transport.
    void setPlayingForTests(bool playing) { onPlayingChanged(playing); }
    /// One write tick now (a timer runs it ~30 times a second while a pass writes).
    void pollWrite();

    QVariantList strips() const { return m_strips; }

    /// The clip's level (dB) / pan (-1..1) on track `track`'s clip under the playhead. Returns "" or the error.
    Q_INVOKABLE QString setGainDb(const QString& track, double gainDb);
    Q_INVOKABLE QString setPan(const QString& track, double pan);
    Q_INVOKABLE void endGesture();
    /// Double-click: 0 dB / centre.
    Q_INVOKABLE QString resetGain(const QString& track);
    Q_INVOKABLE QString resetPan(const QString& track);
    /// The fader ("volume") or knob ("pan") is held: Write Keyframes writes while it is.
    Q_INVOKABLE void touch(const QString& track, const QString& param, bool held);
    Q_INVOKABLE void setWriteKeyframes(const QString& track, bool on);
    Q_INVOKABLE void setMuted(const QString& track, bool muted);
    Q_INVOKABLE void setSolo(const QString& track, bool solo);

signals:
    void stripsChanged();

private:
    struct Lane {
        std::vector<AutomationSegment> segments; ///< in the clip's curve time
        bool open = false;
    };
    void rebuild();
    void onPlayingChanged(bool playing);
    void commitPass();
    void pushLive();
    QString setParam(const QString& track, const QString& param, double value);
    /// The clip on `track` under the playhead.
    const Clip* clipAt(TrackId track, const Sequence& seq) const;

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_playback;
    LiveMixSink m_liveMixSink;
    QVariantList m_strips;
    std::set<QString> m_writeKeyframes;                    ///< strips with Write Keyframes on
    std::map<std::pair<QString, QString>, bool> m_held;    ///< (track, param) held
    std::map<std::pair<QString, QString>, double> m_live;  ///< what a held control is at while writing
    std::map<std::pair<std::uint64_t, QString>, Lane> m_lanes; ///< (clip id, param) of the running pass
    std::set<std::uint64_t> m_liveSent;                    ///< tracks the live override was sent for
    bool m_playing = false;
    SequenceId m_passSequence;
    QTimer m_timer;
};

/// QML: the singleton `ClipMixer`.
struct ClipMixerBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::ClipMixerBridge)
    QML_NAMED_ELEMENT(ClipMixer)
    QML_SINGLETON
public:
    static ClipMixerBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return ClipMixerBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
