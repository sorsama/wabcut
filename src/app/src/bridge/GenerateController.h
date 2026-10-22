#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <QPointer>
#include <QUrl>
#include <filesystem>
#include <optional>
#include <QQmlEngine>
#include <wabcut/model/EditSession.h>
#include <wabcut/ai/generation/GenerationCommands.h>
#include <functional>
#include <thread>
class AiController;
namespace wabcut::app { class ProjectController;
class GenerateController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)
    Q_PROPERTY(QVariantList models READ models NOTIFY changed)
    Q_PROPERTY(bool staged READ staged NOTIFY changed)
    /// Generative Extend: the exact picture that will be uploaded (the boundary frame rendered like an export: the clip
    /// alone, with its redactions, masks and grade), shown before consent; empty until prepareExtend() (review wave 4,
    /// P1-1).
    Q_PROPERTY(QUrl uploadPreview READ uploadPreview NOTIFY uploadChanged)
    /// Non-empty when redactions cover the clip being extended: run() then needs acknowledgeRedactions.
    Q_PROPERTY(QString redactionNotice READ redactionNotice NOTIFY uploadChanged)
public:
    explicit GenerateController(QObject* parent = nullptr);
    ~GenerateController() override;
    static GenerateController* create(QQmlEngine*, QJSEngine*);
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    double progress() const { return m_progress; }
    QVariantList results() const { return m_results; }
    QVariantList models() const;
    bool staged() const { return static_cast<bool>(m_staged); }
    QUrl uploadPreview() const;
    QString redactionNotice() const { return m_redactionNotice; }
    /// Renders the boundary frame of `clip` that Extend would upload (head or tail) through the export renderer and
    /// keeps it as the pending upload. Returns an error message, or "" when the preview is ready.
    Q_INVOKABLE QString prepareExtend(const QString& clip, bool head);
    /// `seconds` 0 for a sound-effect model: the model picks the length (estimated at its longest). `loop`: sound effects.
    Q_INVOKABLE QVariantMap estimate(const QString& model, const QString& prompt, int seconds, const QString& aspect, bool loop = false) const;
    // Explicit user action after the displayed estimate. Keys never pass through QML. No agent can call this method.
    Q_INVOKABLE void run(const QString& model, const QString& prompt, int seconds, const QString& aspect,
        double maxDollars, bool consentOnce, double atSeconds = -1, const QString& clip = {}, bool head = false,
        bool acknowledgeRedactions = false, bool loop = false);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void accept();
    Q_INVOKABLE void discard();
    Q_INVOKABLE void openExtend(const QString& clip);
    Q_INVOKABLE void open();
    std::shared_ptr<command::CommandServices> services();
    void propose(const QVariantMap&, ProjectSnapshot expected);
    /// The AI controller whose platform the worker uses; its shuttingDown() stops and joins the worker (P2-4).
    void setAiController(AiController* ai);
    /// Tests: runs `work` on the worker thread as a generation would.
    void runWorkerForTests(std::function<void(std::stop_token)> work);
signals:
    void changed();
    void uploadChanged();
    void showRequested(const QString& clip);
    void proposalRequested(const QVariantMap& proposal);
private:
    QPointer<AiController> m_ai;
    ProjectController* m_project = nullptr;
    std::jthread m_worker;
    bool m_busy = false;
    double m_progress = 0;
    QString m_message;
    QVariantList m_results;
    ProjectSnapshot m_base;
    std::shared_ptr<const ProjectState> m_staged;
    struct PendingUpload {
        ClipId clip;
        bool head = false;
        std::filesystem::path file;
        std::string sha256;
        std::uint64_t revision = 0; ///< the session revision it was rendered at
        bool redacted = false;
    };
    std::optional<PendingUpload> m_upload;
    QString m_redactionNotice;
};
// Force the engine-aware factory; a default-constructible singleton otherwise bypasses create().
struct GenerateControllerForeign {
    Q_GADGET
    QML_FOREIGN(wabcut::app::GenerateController)
    QML_NAMED_ELEMENT(GenerateController)
    QML_SINGLETON
public:
    static GenerateController* create(QQmlEngine* engine, QJSEngine* jsEngine) {
        return GenerateController::create(engine, jsEngine);
    }
};
}
