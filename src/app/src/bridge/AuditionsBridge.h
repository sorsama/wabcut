#pragma once

// AuditionsBridge: auditions in the app, for QML (singleton `Auditions`, 0.6 card #74; model: wabcut/model/Auditions.h).
//
//   * The audition the Take Selector panel and the Program Monitor's take strip show: the primary selected clip when
//     it is an audition (else another selected one), otherwise the audition under the playhead (the topmost video
//     track holding one, then audio). Its takes in take order with name, source, length, how far a take is short of the
//     audition's range, effect count and a thumbnail of its first frame.
//   * Edits, each one undo step through the model's audition ops: New Audition from the selected clips, Add Selected
//     Clips / the Source Monitor's clip as takes, Use Take, Duplicate, Rename, Move, Remove, Flatten, Next / Previous
//     Take. A switch keeps the clip in place and never moves the playhead.
//   * Play (audition a take in place): the take is shown active as the edit session's preview (no undo step, the redo
//     history stays, saving and autosave write the committed project), the audition's range plays once and the
//     playhead parks where it was; when playback stops the take that was active comes back, unless Use this take was
//     chosen meanwhile (then that is one real undo step). Any other edit, undo or redo takes the preview back first.
//   * The keymap commands Main routes here (audition.*) and their availability for the palette and menus.
//
// Principle 9: QML sees clip keys ("c42"), take indices (0-based), text and image URLs; never model types.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>
#include <string>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class PlaybackBridge;
class ProjectController;
class SourceMonitorBridge;

class AuditionsBridge : public QObject {
    Q_OBJECT
    /// The audition shown ("c42"), or "" when there is none (selected or under the playhead).
    Q_PROPERTY(QString clip READ clip NOTIFY changed FINAL)
    /// The active take's name and "Take 2 of 4".
    Q_PROPERTY(QString name READ name NOTIFY changed FINAL)
    Q_PROPERTY(QString badge READ badge NOTIFY changed FINAL)
    Q_PROPERTY(int count READ count NOTIFY changed FINAL)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY changed FINAL)
    /// "Picture", "Sound" or "Picture and sound" (the audition's lanes).
    Q_PROPERTY(QString kindText READ kindText NOTIFY changed FINAL)
    /// The audition's whole range as timecodes: "00:00:04:00 - 00:00:10:00 (6:00)".
    Q_PROPERTY(QString rangeText READ rangeText NOTIFY changed FINAL)
    /// [{ index, name, active, source, kind ("video" | "audio" | "still" | "nested" | "multicam" | "graphic"),
    ///    durationText, isShort, shortText, effects, thumbnail }] in take order.
    Q_PROPERTY(QVariantList takes READ takes NOTIFY changed FINAL)
    /// The take being auditioned with Play, or -1.
    Q_PROPERTY(int previewing READ previewing NOTIFY previewChanged FINAL)
    /// New Audition can run: clips are selected.
    Q_PROPERTY(bool canCreate READ canCreate NOTIFY changed FINAL)
    /// Add Selected Clips can run: an audition and other clips are selected.
    Q_PROPERTY(bool canAddSelected READ canAddSelected NOTIFY changed FINAL)
    /// Add Source Monitor Clip can run: an audition is shown and the Source Monitor holds media.
    Q_PROPERTY(bool canAddSource READ canAddSource NOTIFY changed FINAL)
    /// The Program Monitor shows the take strip over the picture (remembered).
    Q_PROPERTY(bool overlayShown READ overlayShown WRITE setOverlayShown NOTIFY overlayShownChanged FINAL)
    /// The last problem or result, for a toast ("Take 3 is 1:12 short of the audition").
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit AuditionsBridge(QObject* parent = nullptr);
    ~AuditionsBridge() override;
    static AuditionsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setPlayback(PlaybackBridge* playback);
    void setSourceMonitor(SourceMonitorBridge* source);

    QString clip() const;
    QString name() const;
    QString badge() const;
    int count() const;
    int activeIndex() const;
    QString kindText() const;
    QString rangeText() const;
    QVariantList takes() const;
    int previewing() const { return m_preview ? m_preview->take : -1; }
    bool canCreate() const;
    bool canAddSelected() const;
    bool canAddSource() const;
    bool overlayShown() const { return m_overlay; }
    void setOverlayShown(bool shown);
    QString message() const { return m_message; }

    /// Clip > Auditions > New Audition: the selected clips become one audition (one clip: an audition with a copy).
    Q_INVOKABLE bool createFromSelection();
    /// Add Selected Clips to Audition: the other selected clips become takes of the selected (else shown) audition.
    Q_INVOKABLE bool addSelected();
    /// Add Source Monitor Clip to Audition: the Source Monitor's media from its In becomes a take.
    Q_INVOKABLE bool addSource();
    /// Use this take (the panel's check button, Enter, a card click in the Program Monitor).
    Q_INVOKABLE bool useTake(int index);
    /// The right-clicked clip `clipKey` is the shown audition or one of its linked partners (its sound or picture half).
    Q_INVOKABLE bool isShownAudition(const QString& clipKey) const;
    /// Play: auditions take `index` in place (see the header). Stops a preview of another take first.
    Q_INVOKABLE bool previewTake(int index);
    /// Ends a preview: the take that was active before comes back (unless Use this take was chosen).
    Q_INVOKABLE void stopPreview();
    Q_INVOKABLE bool removeTake(int index);
    Q_INVOKABLE bool moveTake(int from, int to);
    Q_INVOKABLE bool renameTake(int index, const QString& name);
    /// Duplicate Take: a copy of take `index` (-1: the active one) after it, made active.
    Q_INVOKABLE bool duplicateTake(int index = -1);
    /// Flatten Audition: the audition keeps its active take only.
    Q_INVOKABLE bool flatten();
    Q_INVOKABLE bool nextTake();
    Q_INVOKABLE bool previousTake();

    /// The keymap commands Main routes here (audition.*; audition.showTakeSelector opens the panel in Main).
    static bool handlesCommand(const QString& commandId);
    Q_INVOKABLE bool runCommand(const QString& commandId);
    /// { handled, enabled, reason } for the palette and the menus ("Select an audition first").
    Q_INVOKABLE QVariantMap commandAvailability(const QString& commandId) const;

    // ---- C++ (tests) ----
    ClipId currentClip() const { return m_clip; }

signals:
    void changed();
    void previewChanged();
    void overlayShownChanged();
    void messageChanged();

private:
    struct Preview {
        ClipId clip;
        int take = -1;
        int original = -1;       ///< the take that was active before
        bool shown = false;      ///< the take is shown as the session's preview (it was not active already)
        bool kept = false;       ///< Use this take was chosen meanwhile
    };

    void refresh(bool force = false);
    ClipId resolveClip() const;
    bool apply(const char* label, const std::function<edit::EditOutcome(const ProjectState&)>& fn);
    void setMessage(const QString& message);
    void onTransportChanged();
    void endPreview();

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_playback;
    QPointer<SourceMonitorBridge> m_source;
    ClipId m_clip;
    int m_revision = -1;
    std::optional<Preview> m_preview;
    bool m_overlay = false;
    QString m_message;
};

struct AuditionsSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::AuditionsBridge)
    QML_NAMED_ELEMENT(Auditions)
    QML_SINGLETON
public:
    static AuditionsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return AuditionsBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
