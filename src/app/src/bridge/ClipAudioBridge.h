#pragma once

// ClipAudioBridge (M5l, QML singleton `ClipAudio`): the clip menu's audio tools.
//
//   * Enhance Speech… (A.4): audio.enhanceSpeech on the selected clips, with render progress (the take is cached per
//     media stream: a second request renders nothing); its Strength (the effect's parameter, live, one undo step per
//     drag) and bypass (the effect off: the A/B); the take's state for the clip badge: "enhanced" when the take
//     plays, "missing render" when the effect waits for one (render it again from the menu).
//   * Separate Stems… (A.4): audio.separateStems on a clip: drums, bass, other and vocals as media items in a
//     "<source> stems" bin, each marked as separated from its source (the Library shows it).
//   * Synchronize (A.5, Premiere's Synchronize by audio): audio.syncClips {clips, apply, link} on the selected video
//     and external audio clips, with its report: each recording's offset, confidence and drift.
//   * Audio Gain… (G, 0.1.2, Premiere's dialog): set the gain to / adjust it by a level, or normalize the loudest
//     sample peak / every clip's peak to a level. The gain is the clip's normalisation gain (the first
//     wabcut.audio.gain of its stack, audio::edit::normalizationGain), as loudness normalising uses it.
//
// Jobs run off the GUI thread (CommandJob) and are one undo step. A model's first use downloads it after its licence
// card (the dialogs show it while `models` says it is missing). Principle 9: keys ("c12", "e5") and text.

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

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;

class ClipAudioBridge : public QObject {
    Q_OBJECT
    /// "" | "enhance" | "stems" | "sync"
    Q_PROPERTY(QString busy READ busy NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// The last Synchronize: { ok, applied, reference ("c12"), rows: [{ clip ("c12"), name, offset ("+00:00:01:12"),
    /// seconds, confidence ("97 %"), drift ("" or "40.0 ppm, speed 1.00004"), synced, problem }], warnings }.
    Q_PROPERTY(QVariantMap syncReport READ syncReport NOTIFY syncReportChanged FINAL)
    /// The last Enhance Speech: { ok, clips: [{ clip, take ("ready" | "missing" | "stale") }], rendered, reused }.
    Q_PROPERTY(QVariantMap enhanceReport READ enhanceReport NOTIFY enhanceReportChanged FINAL)
    /// The local models: { speechReady, speechLicense, speechSize ("7.6 MB"), stemsReady, stemsLicense, stemsSize }.
    Q_PROPERTY(QVariantMap models READ models NOTIFY modelsChanged FINAL)

public:
    static constexpr const char* kEnhanceEffect = "wabcut.audio.enhanceSpeech";

    explicit ClipAudioBridge(QObject* parent = nullptr);
    static ClipAudioBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    /// The audio services (tests inject fakes; by default the app's, appAudioServices()).
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    /// Makes the services for the open project (they put takes and stems next to it): called again whenever the
    /// controller's project file has changed since.
    void setServiceFactory(std::function<std::shared_ptr<command::CommandServices>()> factory) { m_factory = std::move(factory); }
    /// Jobs on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_job.setSynchronous(synchronous); }
    /// Tests: whether the models count as installed (default: the model store says).
    void setModelsReadyForTests(std::optional<bool> ready);

    QString busy() const { return m_busy; }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }
    QVariantMap syncReport() const { return m_syncReport; }
    QVariantMap enhanceReport() const { return m_enhanceReport; }
    QVariantMap models() const;

    /// The selected clips of the current sequence ("c12"), in timeline order.
    Q_INVOKABLE QStringList selectedClips() const;
    /// What the menus and the dialog need to know of a clip: { audio (it has sound to enhance), video, enhanced (the
    /// effect is on it), effect ("e5"), strength (0..100), bypassed, take ("ready" | "missing" | "stale" | ""),
    /// badge ("enhanced" | "missingRender" | ""), separatedFrom (the stem's source name or "") }.
    Q_INVOKABLE QVariantMap clipInfo(const QString& clipKey) const;
    /// Enhance Speech on clips (video clips stand for their linked audio) at `strength` (0..100); `render` false only
    /// applies the effect (bypassed until a take is rendered). Returns "" when started, else why not.
    Q_INVOKABLE QString enhanceSpeech(const QStringList& clipKeys, double strength = 100.0, bool render = true);
    /// The effect's Strength (0..100) on a clip, live; drags merge into one undo step until endStrength().
    Q_INVOKABLE QString setStrength(const QString& clipKey, double strength);
    Q_INVOKABLE void endStrength();
    /// Turns the effect off (true) or on: the A/B. One undo step.
    Q_INVOKABLE QString setBypassed(const QString& clipKey, bool bypassed);
    /// Separate Stems on a clip. Returns "" when started, else why not.
    Q_INVOKABLE QString separateStems(const QString& clipKey);
    /// Synchronize the clips by their sound: the first clip with a picture is the reference; the others move into
    /// sync and the external audio links to it. Returns "" when started, else why not.
    Q_INVOKABLE QString synchronize(const QStringList& clipKeys);
    /// Audio Gain on clips (video clips stand for their linked audio). `mode`: "set" (the gain to `db`), "adjust" (by
    /// `db`), "normalizeMax" (one gain for all: the loudest sample peak to `db` dBFS) or "normalizeAll" (each clip's
    /// peak to `db`). The normalize modes measure the clips (audio.loudness) off the GUI thread first. One undo step.
    /// Returns "" when applied or started, else why not.
    Q_INVOKABLE QString audioGain(const QStringList& clipKeys, const QString& mode, double db);
    /// The gain (dB) of the first audio clip among these now: 0 without a gain effect (the dialog's start value).
    Q_INVOKABLE double clipGainDb(const QStringList& clipKeys) const;
    /// The audio clips Audio Gain works on for these keys (video clips give their linked audio).
    std::vector<ClipId> gainClips(const QStringList& clipKeys) const;
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void refreshModels();
    /// The clip menu: open the Enhance Speech dialog for these clips / the Separate Stems dialog for a clip.
    Q_INVOKABLE void openEnhance(const QStringList& clipKeys) { emit enhanceRequested(clipKeys); }
    Q_INVOKABLE void openStems(const QString& clipKey) { emit stemsRequested(clipKey); }

signals:
    /// Audio Gain finished: "" or the error.
    void audioGainDone(const QString& error);
    void jobChanged();
    void messageChanged();
    void syncReportChanged();
    void enhanceReportChanged();
    void modelsChanged();
    /// Main.qml opens the dialogs.
    void enhanceRequested(const QStringList& clipKeys);
    void stemsRequested(const QString& clipKey);

private:
    std::shared_ptr<command::CommandServices> services();
    void setJob(const QString& busy, const QString& phase, double progress);
    void setMessage(const QString& message);
    QString start(const QString& kind, const QString& phase, const std::string& command, command::Json params,
                  const std::string& label, std::function<void(const command::CommandResult&)> done);

    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::CommandServices> m_services;
    std::function<std::shared_ptr<command::CommandServices>()> m_factory;
    std::shared_ptr<command::CommandServices> m_built; ///< m_factory's, for the project file m_builtFor
    std::filesystem::path m_builtFor;
    CommandJob m_job;
    QString m_busy;
    QString m_phase;
    double m_progress = 0;
    QString m_message;
    QVariantMap m_syncReport;
    QVariantMap m_enhanceReport;
    std::optional<bool> m_modelsReady;
};

struct ClipAudioBridgeForeign {
    Q_GADGET
    QML_FOREIGN(ClipAudioBridge)
    QML_NAMED_ELEMENT(ClipAudio)
    QML_SINGLETON
public:
    static ClipAudioBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return ClipAudioBridge::create(qmlEngine, jsEngine); }
};

/// The Enhance Speech badge of a clip in `state`: "enhanced" (the effect is on and its take plays), "missingRender"
/// (the effect waits for a take: none rendered, stale or missing) or "" (no effect, or it is off). The timeline draws it.
QString enhanceBadge(const ProjectState& state, const Clip& clip);

} // namespace wabcut::app
