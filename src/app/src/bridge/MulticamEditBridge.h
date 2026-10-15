#pragma once

// MulticamEditBridge (M5l, QML singleton `Multicam`): Premiere's multi-camera editing.
//
//   * Create Multi-Camera Source Sequence… from the Library's selection: multicam.create with the sync method
//     (timecode, In points, Out points, clip markers, audio) on a worker. Syncing by audio first measures the angles
//     with audio.syncClips (A.5) for the report: each angle's offset and how sure the match is; the multicam source
//     is then created on the same (cached) analysis.
//   * The Program Monitor's multi-camera view: the angle grid of the multicam clip under the playhead (the topmost
//     video track that has one), MulticamGrid's one-composite grid at a reduced resolution, live while playing, with
//     its GPU cost; the program output stays beside it.
//   * Switching (Premiere's keys, docs/keymap.md): Select Camera 1-9 and Cut to Camera 1-9, or a click on an angle.
//     While playing, switches are recorded and sent when playback stops as multicam.switchAngle "switches" (one per
//     clip, in one batch: one undo step); paused, Select switches the cut under the playhead and Cut cuts there.
//     `target` switches the picture, the sound or both.
//   * Flatten (the clip menu): multicam.flatten.
//
// Principle 9: QML sees keys ("c12", "s3", "m7"), 1-based angle numbers, display text and an image.

#include "CommandJob.h"
#include "MulticamGrid.h"

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class LibraryBridge;
class PlaybackBridge;
class ProjectController;

class MulticamEditBridge : public QObject {
    Q_OBJECT
    // ---- Create Multi-Camera Source Sequence ----
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    /// The last creation: { ok, sequence ("s3"), name, sync, syncName, angles: [{ angle, name, offset (timecode),
    /// confidence ("98 %", "exact" or ""), drift ("" or "40 ppm"), problem }], warnings: [text] }; empty before.
    Q_PROPERTY(QVariantMap report READ report NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

    // ---- The multi-camera view ----
    Q_PROPERTY(bool viewOn READ viewOn WRITE setViewOn NOTIFY viewChanged FINAL)
    /// The multicam clip under the playhead ("c12"), or "".
    Q_PROPERTY(QString clip READ clip NOTIFY clipChanged FINAL)
    /// Its multicam source's name.
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY clipChanged FINAL)
    /// [{ angle (1-based), name, video (in the picture now), audio (heard now), hasVideo, keyText ("1".."9" or "") }].
    Q_PROPERTY(QVariantList angles READ angles NOTIFY anglesChanged FINAL)
    /// Grid columns (2, 3 or 4) and the visible angle count.
    Q_PROPERTY(int columns READ columns NOTIFY clipChanged FINAL)
    /// What a switch changes: "both", "video" or "audio".
    Q_PROPERTY(QString target READ target WRITE setTarget NOTIFY targetChanged FINAL)
    /// Switches recorded while playing, not sent yet.
    Q_PROPERTY(int pendingSwitches READ pendingSwitches NOTIFY anglesChanged FINAL)
    /// The angle grid (RGBA, the reduced resolution), null until drawn.
    Q_PROPERTY(QImage gridImage READ gridImage NOTIFY gridChanged FINAL)
    /// The grid's cost: { lastMs (the last frame), meanMs and p95Ms (per frame, the renderer open; over steadyFrames),
    /// openMs (the last frame that opened the renderer), frames, size ("640×360"), backend, error }.
    Q_PROPERTY(QVariantMap gridStats READ gridStats NOTIFY gridChanged FINAL)

public:
    explicit MulticamEditBridge(QObject* parent = nullptr);
    ~MulticamEditBridge() override;
    static MulticamEditBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setPlayback(PlaybackBridge* playback);
    void setLibrary(LibraryBridge* library);
    /// The services of multicam.create's audio sync and audio.syncClips (tests inject fakes).
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    /// Makes the services for the open project (the app's in create()): called again whenever the controller's project
    /// file has changed since.
    void setServiceFactory(std::function<std::shared_ptr<command::CommandServices>()> factory) {
        m_serviceFactory = std::move(factory);
    }
    /// Jobs and grid frames on the calling thread (tests).
    void setSynchronous(bool synchronous);
    /// The grid's render size (default 640x360; the bench sets others).
    void setGridSize(QSize size);
    /// Tests: playing or not without a transport (the recording follows it as it follows Playback).
    void setPlayingForTests(bool playing) { onPlayingChanged(playing); }

    bool busy() const { return m_job.running() || m_creating; }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    QVariantMap report() const { return m_report; }
    QString message() const { return m_message; }
    bool viewOn() const { return m_viewOn; }
    void setViewOn(bool on);
    QString clip() const;
    QString sourceName() const { return m_sourceName; }
    QVariantList angles() const;
    int columns() const { return m_columns; }
    QString target() const { return m_target; }
    void setTarget(const QString& target);
    int pendingSwitches() const { return static_cast<int>(m_switches.size()); }
    QImage gridImage() const { return m_gridImage; }
    QVariantMap gridStats() const;

    /// The Library items a Create dialog lists: [{ key, name, kind ("video" | "audio"), hasTimecode, hasAudio }];
    /// keys that are not media are left out.
    Q_INVOKABLE QVariantList describeMedia(const QStringList& keys) const;
    /// The sync method the dialog proposes for them: "timecode" when all have one, else "audio" when all have
    /// sound, else "inPoints".
    Q_INVOKABLE QString proposedSync(const QStringList& keys) const;
    /// Create Multi-Camera Source Sequence: `media` keys in angle order, `sync` (timecode | inPoints | outPoints |
    /// markers | audio), the name ("" = default) and angle names (empty entries = the media names). Returns "" when
    /// started, else why not; the result arrives in `report`.
    Q_INVOKABLE QString createSource(const QStringList& media, const QString& sync, const QString& name,
                                     const QStringList& angleNames = {});
    Q_INVOKABLE void cancel();
    /// The Library's menu: open the Create dialog with these items (Main.qml shows it).
    Q_INVOKABLE void openCreate(const QStringList& media) { emit createRequested(media); }

    /// Select Camera `angle` (1-based): playing, records a switch; paused, switches the cut under the playhead.
    Q_INVOKABLE bool selectAngle(int angle);
    /// Cut to Camera `angle`: playing, records a switch; paused, cuts at the playhead.
    Q_INVOKABLE bool cutToAngle(int angle);
    /// Flatten a multicam clip ("c12"; "" = the one under the playhead).
    Q_INVOKABLE QString flatten(const QString& clipKey = {});
    /// Draws the grid now (paused), on the calling thread when synchronous.
    Q_INVOKABLE void refreshGrid();

    static bool handlesCommand(const QString& id);
    /// { handled, enabled, reason } for the palette and menus.
    Q_INVOKABLE QVariantMap commandAvailability(const QString& id) const;
    Q_INVOKABLE bool runCommand(const QString& id);

    /// The multicam clip under the playhead of `sequence` at `time`: its track and clip (C++ / tests).
    struct Site {
        ClipId clip;
        TrackId track;
        SequenceId source;
    };
    static std::optional<Site> multicamClipAt(const ProjectState& state, const Sequence& sequence, Ticks time);

signals:
    void jobChanged();
    void reportChanged();
    void messageChanged();
    void viewChanged();
    void clipChanged();
    void anglesChanged();
    void targetChanged();
    void gridChanged();
    /// The view asks for the Create dialog with these Library items (Main.qml opens it).
    void createRequested(const QStringList& media);

private:
    struct Recorded {
        Ticks at;
        int angle = 1; ///< 1-based
        ClipId clip;
    };
    std::shared_ptr<command::CommandServices> services();
    void setMessage(const QString& message);
    void setJob(const QString& phase, double progress);
    void updateClip();
    void rebuildGrid();
    void pushGridTime();
    void onPlayingChanged(bool playing);
    void flushSwitches();
    bool switchAngle(int angle, bool cut);
    void finishCreate(const command::CommandResult& result, const QVariantList& audioRows);
    void onGridFrame(std::expected<MulticamGridFrame, QString> frame, std::uint64_t generation);
    std::size_t videoAngleNow() const; ///< 0-based angle the picture shows now (pending switches included)

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_playback;
    QPointer<LibraryBridge> m_library;
    std::shared_ptr<command::CommandServices> m_services;
    std::function<std::shared_ptr<command::CommandServices>()> m_serviceFactory; ///< the app's (create())
    std::shared_ptr<command::CommandServices> m_built; ///< m_serviceFactory's, for the project file m_builtFor
    std::filesystem::path m_builtFor;
    CommandJob m_job;
    bool m_creating = false; ///< between the audio measure and the create
    bool m_synchronous = false;
    QString m_phase;
    double m_progress = 0;
    QVariantMap m_report;
    QString m_message;

    bool m_viewOn = false;
    bool m_playing = false;
    std::optional<Site> m_site;
    ProjectSnapshot m_siteSnapshot; ///< the snapshot m_site was found in
    QString m_sourceName;
    int m_columns = 2;
    QString m_target = QStringLiteral("both");
    std::vector<Recorded> m_switches;

    std::unique_ptr<MulticamGridRenderer> m_renderer;
    QSize m_gridSize{640, 360};
    std::shared_ptr<const ProjectState> m_gridState;
    SequencePtr m_gridFrom; ///< the source sequence node the grid state was built from
    std::uint64_t m_gridGeneration = 0;
    QImage m_gridImage;
    std::deque<double> m_costs; ///< recent grid frame costs with the renderer open, ms
    int m_gridFrames = 0;
    double m_lastMs = 0;
    double m_openMs = 0;
    QString m_backend;
    QString m_gridError;
};

struct MulticamEditBridgeForeign {
    Q_GADGET
    QML_FOREIGN(MulticamEditBridge)
    QML_NAMED_ELEMENT(Multicam)
    QML_SINGLETON
public:
    static MulticamEditBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return MulticamEditBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
