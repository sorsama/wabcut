#pragma once

// "New gaming video" (M5o, gaming): the Home screen's start from a game recording (GamingVideoCard, singleton
// `GamingStart`).
//   inspect(url)  probes the recording off the GUI thread (media::probe) and publishes `recording`: its size, frame
//                 rate (the nominal rate, also for a variable frame rate capture: the probe snaps it to a standard
//                 rate), duration, codec, HDR (PQ / HLG, shown tone-mapped), the flags that suggest proxies (above
//                 60 fps, 4K, 20 minutes or longer) with the reason, and one row per audio stream with a suggested
//                 track name: Game, Mic, Discord, then "Audio n" (OBS's usual track order).
//   create(...)   makes the project in one go, each step on the live project (Home and ProjectFile, then catalog
//                 commands through CommandJob::runNow): Home.createProject at the recording's size and nominal rate
//                 (saved and opened), ProjectFile.importFiles, one audio track per included stream (sequence.addTrack
//                 while the sequence has fewer), the recording at 00:00 with V1 and the included streams on A1, A2, ...
//                 in stream order (edit.insertFromSource with explicit patches: an excluded stream is not patched),
//                 the track names (track.setState), proxies when suggested (MediaReadiness.generateProxies), then a
//                 save. It returns "" or the failed step's message; no step is skipped silently.
// Stream titles: FFmpeg's AudioStreamInfo carries no title, so the names are suggestions by position. A.10's OBS ingest
// should provide the real track titles (OBS writes them as the streams' "title" tags) and replace these suggestions.

#include <wabcut/model/Error.h>
#include <wabcut/model/MediaInfo.h>

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QJSEngine;

namespace wabcut::app {

class HomeBridge;
class ProjectController;
class ProjectLifecycle;

class GamingStartBridge : public QObject {
    Q_OBJECT
    /// The last inspected recording: {ok, error, url, file, name, width, height, fps: {num, den, text}, vfr,
    /// durationSeconds, durationText, videoCodec, hdr ("PQ", "HLG" or ""), sizeText, fileSizeText, highFps, is4k,
    /// isLong, proxies, proxyReason, audio: [{index, stream, channels, layout, codec, suggested}]}; empty before any.
    /// `index` is the stream's place among the audio streams (0 = A1 of the source), `stream` its container index.
    Q_PROPERTY(QVariantMap recording READ recording NOTIFY recordingChanged FINAL)
    Q_PROPERTY(bool inspecting READ inspecting NOTIFY inspectingChanged FINAL)
    /// FileDialog filters for game recordings (OBS, ShadowPlay, Xbox Game Bar, ...).
    Q_PROPERTY(QStringList nameFilters READ nameFilters CONSTANT FINAL)

public:
    using Probe = std::function<std::expected<MediaInfo, Error>(const std::string& utf8Path)>;
    /// Asks for proxies of the given media keys ("m12"); returns how many were queued.
    using ProxyRequester = std::function<int(const QStringList& mediaKeys)>;

    explicit GamingStartBridge(QObject* parent = nullptr);
    ~GamingStartBridge() override;

    static GamingStartBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setHome(HomeBridge* home);
    void setLifecycle(ProjectLifecycle* lifecycle);
    void setController(ProjectController* controller);
    /// Where the proxy request goes by default: the engine's MediaReadiness, resolved on first use.
    void setEngine(QQmlEngine* engine);
    /// Tests: a probe instead of media::probe (called on a worker thread).
    void setProbe(Probe probe);
    /// Tests: records the proxy request instead of MediaReadiness.generateProxies.
    void setProxyRequester(ProxyRequester requester);

    /// The `recording` map for a probed file (pure; the tests check the flags and names with it).
    static QVariantMap describe(const MediaInfo& info, const QString& file);
    /// "Game", "Mic", "Discord", then "Audio 4", ... for the stream at `position` among the audio streams.
    static QString suggestedTrackName(int position);

    QVariantMap recording() const { return m_recording; }
    bool inspecting() const { return m_inspecting; }
    QStringList nameFilters() const;

    /// Whether a dropped file looks like a game recording (a video container by its extension).
    Q_INVOKABLE bool isRecording(const QUrl& file) const;
    /// Probes `file` off the GUI thread; `recording` changes when it is done (a later call wins).
    Q_INVOKABLE void inspect(const QUrl& file);
    /// Forgets the recording (the card's Change…).
    Q_INVOKABLE void clear();
    /// Creates the project from the inspected recording. `tracks`: [{index, name, include}] per audio stream (index as
    /// in `recording.audio`; a stream left out of the list is included under its suggested name). Returns "" or the
    /// error of the step that failed.
    Q_INVOKABLE QString create(const QString& name, const QUrl& folder, const QVariantList& tracks);

signals:
    void recordingChanged();
    void inspectingChanged();
    /// A project was made from the recording (after its save).
    void created(const QUrl& projectFile);

private:
    struct Gate;
    void finishInspect(std::uint64_t generation, const QUrl& file, const std::expected<MediaInfo, Error>& info);
    int requestProxies(const QStringList& mediaKeys);

    QPointer<HomeBridge> m_home;
    QPointer<ProjectLifecycle> m_lifecycle;
    QPointer<ProjectController> m_controller;
    QPointer<QQmlEngine> m_engine;
    Probe m_probe;
    ProxyRequester m_proxyRequester;
    std::shared_ptr<Gate> m_gate;
    std::uint64_t m_generation = 0;
    QVariantMap m_recording;
    std::optional<MediaInfo> m_info; ///< the probe behind `recording`
    bool m_inspecting = false;
};

// The singleton for QML (a wrapper so the bridge stays default-constructible without becoming QML-creatable).
struct GamingStartSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::GamingStartBridge)
    QML_NAMED_ELEMENT(GamingStart)
    QML_SINGLETON
public:
    static GamingStartBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return GamingStartBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
