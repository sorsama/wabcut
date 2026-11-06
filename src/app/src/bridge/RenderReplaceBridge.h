#pragma once

// RenderReplaceBridge: Clip > Render and Replace… and Restore Unrendered in the app (singleton `RenderReplace`, 0.7
// card 069; model: wabcut/model/RenderReplace.h).
//
//   * Render and Replace bakes the selected nested-sequence clips: each nest the clips play is rendered over the stretch
//     they use, widened by the handles, to a media file with the chosen export preset (picture and sound in one file),
//     and the clips play that file from then on - same place, In and Out, speed, effects and links. The render is a job of
//     the export queue (ExportController::queueSequenceRange): it runs off the GUI thread, shows its progress in the queue
//     and the dialog and stops with Cancel. When a job is done the file is added to the project and the clips take it
//     over as ONE undo step "Render and Replace" per nest. Nothing is replaced when the nest was edited while it
//     rendered (the file would not match it), the clips changed, or the render failed or was cancelled.
//   * Restore Unrendered plays the nest again (one undo step); the rendered file stays in the project.
//   * The keymap commands Main routes here (clip.renderAndReplace opens the dialog, clip.restoreUnrendered runs) and their
//     availability for the palette and the menus ("Select a nested sequence clip first").
//
// Principle 9: QML sees text, numbers and clip counts; never model types.

#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <map>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ExportController;
class ProjectController;

class RenderReplaceBridge : public QObject {
    Q_OBJECT
    /// At least one render is still running (the dialog shows its progress and Cancel).
    Q_PROPERTY(bool running READ running NOTIFY changed FINAL)
    /// 0..1 across the running renders.
    Q_PROPERTY(double progress READ progress NOTIFY changed FINAL)
    /// What is happening or happened last, for the dialog ("Rendering Nest 1…", "Nest 1 is a file now").
    Q_PROPERTY(QString status READ status NOTIFY changed FINAL)
    /// The formats to render to: [{ id, name, note, alpha (keeps transparency) }].
    Q_PROPERTY(QVariantList presets READ presets CONSTANT FINAL)
    /// The format and the handles (seconds each side) chosen last, remembered.
    Q_PROPERTY(QString presetId READ presetId WRITE setPresetId NOTIFY settingsChanged FINAL)
    Q_PROPERTY(double handleSeconds READ handleSeconds WRITE setHandleSeconds NOTIFY settingsChanged FINAL)

public:
    explicit RenderReplaceBridge(QObject* parent = nullptr);
    ~RenderReplaceBridge() override;
    static RenderReplaceBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setExport(ExportController* exporter);

    bool running() const { return !m_pending.empty(); }
    double progress() const { return m_progress; }
    QString status() const { return m_status; }
    QVariantList presets() const;
    QString presetId() const { return m_presetId; }
    void setPresetId(const QString& id);
    double handleSeconds() const { return m_handleSeconds; }
    void setHandleSeconds(double seconds);

    /// What Render and Replace would do with the selected clips and `handleSeconds` of handles: { ok, reason, units: [{
    /// name, clips, kind ("Picture and sound" | "Picture" | "Sound"), usedText, rangeText, transparent }], preset (the
    /// format to start from: ProRes 4444 when a nest may show transparency, else the remembered one), folder (where the
    /// files go by default) }.
    Q_INVOKABLE QVariantMap inspect(double handleSeconds) const;
    /// The folder the rendered files go to unless another is chosen: "<project> Rendered Media" next to a saved project,
    /// else under the default export folder. Not made until a render starts.
    Q_INVOKABLE QString defaultFolder() const;
    /// Queues the renders for the selected clips (`folder` "" = defaultFolder()). False, with `status` and a notice, when
    /// nothing can be rendered.
    Q_INVOKABLE bool start(const QString& presetId, double handleSeconds, const QString& folder);
    /// Stops the renders that are running; their clips stay nested.
    Q_INVOKABLE void cancel();
    /// Forgets the last result text (the dialog opens clean).
    Q_INVOKABLE void clearStatus();
    /// The local path of a folder chosen in a dialog (a file URL), native separators.
    Q_INVOKABLE QString localPath(const QUrl& url) const;
    /// Restore Unrendered on the selected clips (one undo step).
    Q_INVOKABLE bool restore();

    /// The keymap commands Main routes here.
    static bool handlesCommand(const QString& commandId);
    Q_INVOKABLE bool runCommand(const QString& commandId);
    /// { handled, enabled, reason } for the palette and the menus.
    Q_INVOKABLE QVariantMap commandAvailability(const QString& commandId) const;

    // ---- C++ (tests) ----
    int pendingCount() const { return static_cast<int>(m_pending.size()); }

signals:
    void changed();
    void settingsChanged();
    /// A result to toast: `warning` when it is a problem.
    void notice(const QString& text, bool warning);
    /// Render and Replace… asks Main to open its dialog.
    void dialogRequested();

private:
    struct Pending {
        SequenceId outer;
        SequenceId nested;
        std::vector<ClipId> clips;
        Ticks origin;
        std::filesystem::path output;
        QString name;
        /// The nested sequence and everything it plays, as they were when the render started: any edit of them makes the
        /// file out of date.
        std::vector<std::pair<SequenceId, SequencePtr>> sources;
    };

    void onJobFinished(const QString& jobId, bool succeeded, const QString& message);
    void pollProgress();
    void setStatus(const QString& text);
    void finishUnit(const Pending& unit);
    bool selectionProblem(QString* reason) const;

    QPointer<ProjectController> m_controller;
    QPointer<ExportController> m_export;
    std::map<QString, Pending> m_pending;
    QTimer m_poll;
    double m_progress = 0.0;
    QString m_status;
    QString m_presetId;
    double m_handleSeconds = 1.0;
};

struct RenderReplaceSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::RenderReplaceBridge)
    QML_NAMED_ELEMENT(RenderReplace)
    QML_SINGLETON
public:
    static RenderReplaceBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return RenderReplaceBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
