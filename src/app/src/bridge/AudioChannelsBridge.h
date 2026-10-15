#pragma once

// AudioChannelsBridge (M5n, QML singleton `AudioChannels`): the UI of A.9's audio channels (docs/ai.md "Audio channels
// and 5.1 (A.9)").
//
//   * Modify > Audio Channels… (the dialog): info() / mediaInfo() read what clips or Library items play (audio.channels,
//     MediaItem::channels), applyMap() / applyPreset() write it back (audio.setChannels; audio.setMediaChannels for
//     items). The clip menu's Fill Left with Right, Fill Right with Left, Swap Channels and Split to Mono.
//   * Channel Volume in Effect Controls: `volume` describes the clip the Audio section edits (its channels' volume at
//     the playhead); setChannelVolume() merges a drag into one undo step, toggleChannelKey() adds or removes the key at
//     the playhead, setChannelAnimated() is the stopwatch.
//   * Track formats (mono / stereo / 5.1 / adaptive, `trackFormats`), the master's format (`masterFormat`) and the 5.1
//     panners (`surround`, setSurroundPan(): a drag is one undo step).
//   * measure(): audio.channelLevels (a decode, off the GUI thread): each source channel's peak and RMS and the silent
//     channels, for the dialog's meters.
//
// Every edit is one undo step. Keys: clips "c12", tracks "t3", media "m4"; channel numbers are 1-based.

#include "CommandJob.h"

#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;

class AudioChannelsBridge : public QObject {
    Q_OBJECT
    /// Audio track key -> "mono" | "stereo" | "5.1" | "adaptive".
    Q_PROPERTY(QVariantMap trackFormats READ trackFormats NOTIFY tracksChanged FINAL)
    /// Audio track key -> { x, y, lfeDb } (the 5.1 panner; every audio track has one).
    Q_PROPERTY(QVariantMap surround READ surround NOTIFY tracksChanged FINAL)
    /// "stereo" | "5.1" ("" without a sequence).
    Q_PROPERTY(QString masterFormat READ masterFormat NOTIFY tracksChanged FINAL)
    /// The Effect Controls' Channel Volume: { available, clip ("c12"), name, format, channels: [{ channel (1-based),
    /// label ("L"), db, animated, keyed (a key at the playhead), muted }] }.
    Q_PROPERTY(QVariantMap volume READ volume NOTIFY volumeChanged FINAL)
    /// The last measure(): { clips: [{ clip ("c12"), channels: [{ channel, peakDb, rmsDb }], silent: [channel], seconds }] }.
    Q_PROPERTY(QVariantMap levels READ levels NOTIFY levelsChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit AudioChannelsBridge(QObject* parent = nullptr);
    static AudioChannelsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    /// The analysis services of audio.channelLevels (tests inject fakes; by default the app's, appAudioServices()).
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    void setServiceFactory(std::function<std::shared_ptr<command::CommandServices>()> factory) { m_factory = std::move(factory); }
    /// Jobs on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_job.setSynchronous(synchronous); }

    QVariantMap trackFormats() const { return m_trackFormats; }
    QVariantMap surround() const { return m_surround; }
    QString masterFormat() const { return m_masterFormat; }
    QVariantMap volume() const { return m_volume; }
    QVariantMap levels() const { return m_levels; }
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }

    /// What `clips` play (the first one's map; the dialog edits them together): { ok, error, clips (count),
    /// sourceChannels, mapFrom ("clip" | "media" | "none"), format, channels: [{ sources: [1-based], muted, gainDb }],
    /// sourceLabels: ["1", "2", ...] }.
    Q_INVOKABLE QVariantMap info(const QStringList& clipKeys) const;
    /// The same for Library items ("m4"): their own map (mapFrom "media") or the stream as it is ("none").
    Q_INVOKABLE QVariantMap mediaInfo(const QStringList& mediaKeys) const;
    /// The channel labels of a format: stereo ["L", "R"], 5.1 ["L", "R", "C", "LFE", "Ls", "Rs"], mono ["Mono"],
    /// adaptive ["1", ..., "count"].
    Q_INVOKABLE QStringList channelLabels(const QString& format, int count = 0) const;

    /// Modify > Audio Channels on clips: a map { format, channels: [{ sources: [1-based], muted }] }. Each clip keeps
    /// its channels' volumes (constant or keyframed). One undo step. Returns "" or the error.
    Q_INVOKABLE QString applyMap(const QStringList& clipKeys, const QVariantMap& map);
    /// A preset: "fillLeft", "fillRight", "swap", "sumToMono", "mono" (+ `channel`), "stereo", "identity", "default".
    Q_INVOKABLE QString applyPreset(const QStringList& clipKeys, const QString& preset, int channel = 1);
    /// The same on Library items (audio.setMediaChannels: its clips without a map of their own follow).
    Q_INVOKABLE QString applyMediaMap(const QStringList& mediaKeys, const QVariantMap& map);
    Q_INVOKABLE QString applyMediaPreset(const QStringList& mediaKeys, const QString& preset, int channel = 1);
    /// Fill Left with Right (from "right") / Fill Right with Left (from "left").
    Q_INVOKABLE QString fill(const QStringList& clipKeys, const QString& from);
    Q_INVOKABLE QString swap(const QStringList& clipKeys);
    /// Split to Mono: one linked mono clip per source channel on the free audio tracks below.
    Q_INVOKABLE QString splitToMono(const QString& clipKey);

    /// Channel Volume of the `volume` clip, dB: the constant, or the key at the playhead of an animated channel. A drag
    /// merges into one undo step until endChannelVolume().
    Q_INVOKABLE QString setChannelVolume(int channel, double db);
    Q_INVOKABLE void endChannelVolume();
    /// The key at the playhead: removed when there is one, else added at the channel's current value.
    Q_INVOKABLE QString toggleChannelKey(int channel);
    /// The stopwatch: on keys the channel at the playhead; off makes it the constant it has there.
    Q_INVOKABLE QString setChannelAnimated(int channel, bool animated);

    /// track.setFormat: "mono" | "stereo" | "5.1" | "adaptive".
    Q_INVOKABLE QString setTrackFormat(const QString& trackKey, const QString& format);
    /// sequence.setMasterFormat: "stereo" | "5.1".
    Q_INVOKABLE QString setMasterFormat(const QString& format);
    /// The track's 5.1 panner (x, y in -1..1, the LFE send -144..0 dB). A drag merges until endSurroundPan().
    Q_INVOKABLE QString setSurroundPan(const QString& trackKey, double x, double y, double lfeDb);
    Q_INVOKABLE void endSurroundPan();

    /// audio.channelLevels on clips (at most 120 s each), into `levels`. Returns "" when started, else why not.
    Q_INVOKABLE QString measure(const QStringList& clipKeys);
    Q_INVOKABLE void cancel();

    /// The clip menu and the Library: Main.qml opens the dialog ("clips" or "media"). Video clips stand for their linked
    /// audio clips (the dialog edits the sound).
    Q_INVOKABLE void openDialog(const QStringList& clipKeys);
    Q_INVOKABLE void openForMedia(const QStringList& mediaKeys) { emit dialogRequested(QStringLiteral("media"), mediaKeys); }
    /// The audio clips of `clipKeys` ("c12"), in order, without repeats: an audio clip itself, a video clip's linked audio
    /// clips (none for a picture without sound).
    Q_INVOKABLE QStringList audioClips(const QStringList& clipKeys) const;

signals:
    void tracksChanged();
    void volumeChanged();
    void levelsChanged();
    void busyChanged();
    void messageChanged();
    void dialogRequested(const QString& mode, const QStringList& keys);

private:
    std::shared_ptr<command::CommandServices> services();
    void rebuildTracks();
    void rebuildVolume();
    void setMessage(const QString& message);
    /// A command on the current sequence (one undo step for an edit).
    QString run(const std::string& command, command::Json params);
    /// A project command (audio.setMediaChannels): it takes no "sequence".
    QString runProject(const std::string& command, command::Json params);
    std::vector<ClipId> audioClipIds(const QStringList& clipKeys) const;
    QString applyMapEdit(const QStringList& clipKeys, const QVariantMap& map);

    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::CommandServices> m_services;
    std::function<std::shared_ptr<command::CommandServices>()> m_factory;
    CommandJob m_job;
    QVariantMap m_trackFormats;
    QVariantMap m_surround;
    QString m_masterFormat;
    QVariantMap m_volume;
    QVariantMap m_levels;
    bool m_busy = false;
    QString m_message;
};

struct AudioChannelsBridgeForeign {
    Q_GADGET
    QML_FOREIGN(AudioChannelsBridge)
    QML_NAMED_ELEMENT(AudioChannels)
    QML_SINGLETON
public:
    static AudioChannelsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return AudioChannelsBridge::create(qmlEngine, jsEngine); }
};

/// The timeline's channel badge of an audio clip in `state` (A.9): "" when it plays the stream as it is (no map), else
/// a micro-tag: "FILL L" / "FILL R" (both sides from one channel), "SWAP", "MONO" or "CH 2" (one source channel),
/// "SUM" (sum to mono), "5.1", "MUTED" (a channel muted) or "MAP" (any other map).
QString channelBadge(const ProjectState& state, const Clip& clip);

} // namespace wabcut::app
