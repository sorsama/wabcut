#pragma once

// AgentController: the editing assistant's QML bridge (owner: AI platform engineer; docs/ai.md "Editing assistant").
//
// The palette's "Ask AI…" row opens the assistant panel (qml/panels/ai/AssistantPanel.qml) through open(). A request
// runs wabcut::ai::agent::EditAgent on a worker thread with the user's chat model from AiController (consent,
// spend cap and request log apply to every call; the first use of a provider asks for consent first). The panel
// streams the phase, the model's reasoning summary and the steps as human-readable edits; the staged result shows
// on the timeline as a read-only preview (TimelineView::setExternalPreview: cuts struck through, new content as
// ghost clips) together with the verification report. Accept applies the staged batch as ONE undo step (rebased
// or refused when the timeline changed meanwhile), Reject drops it, Refine continues the same staged edit.
//
// QML sees display values only: titles, timecodes, "US$0.04" strings, booleans.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/agent/QuickActions.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <stop_token>
#include <thread>

class AiController;
class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;

class AgentController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool panelOpen READ panelOpen WRITE setPanelOpen NOTIFY panelOpenChanged FINAL)
    /// The request being written (the palette query lands here).
    Q_PROPERTY(QString prompt READ prompt WRITE setPrompt NOTIFY promptChanged FINAL)
    /// "idle", "consent" (waiting for the consent answer), "running", "result", "applied".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
    /// What the run is doing now ("Thinking", "Checking the result").
    Q_PROPERTY(QString phase READ phase NOTIFY progressChanged FINAL)
    /// The model's reasoning summary and answer as they stream.
    Q_PROPERTY(QString reasoning READ reasoning NOTIFY progressChanged FINAL)
    Q_PROPERTY(QString answer READ answer NOTIFY progressChanged FINAL)
    /// [{kind: "read"|"edit"|"error"|"verify"|"note", title, detail, ok, running}]
    Q_PROPERTY(QVariantList steps READ steps NOTIFY progressChanged FINAL)
    /// "US$0.04 · 9.1k in / 120 out · 3 calls"
    Q_PROPERTY(QString usageText READ usageText NOTIFY progressChanged FINAL)
    /// The verification report: [{id, label, passed, detail, blocking}]
    Q_PROPERTY(QVariantList checks READ checks NOTIFY resultChanged FINAL)
    Q_PROPERTY(bool checksPassed READ checksPassed NOTIFY resultChanged FINAL)
    /// AgentStatus name ("Staged", "VerificationFailed", ...) or "".
    Q_PROPERTY(QString status READ status NOTIFY resultChanged FINAL)
    /// One line for the result header: "All 7 checks passed", "1 of 7 checks failed", "Stopped at the budget".
    Q_PROPERTY(QString statusText READ statusText NOTIFY resultChanged FINAL)
    /// What the staged edit changes: "2 clips removed, 3 changed on V1, A1; length 00:00:12:00 → 00:00:10:12".
    Q_PROPERTY(QString changeSummary READ changeSummary NOTIFY resultChanged FINAL)
    Q_PROPERTY(bool canAccept READ canAccept NOTIFY resultChanged FINAL)
    Q_PROPERTY(bool canRefine READ canRefine NOTIFY resultChanged FINAL)
    /// The timeline changed after the edit was staged: Accept re-checks it on the current timeline.
    Q_PROPERTY(bool stale READ stale NOTIFY resultChanged FINAL)
    /// Errors and the outcome of Accept ("Applied as one undo step.").
    Q_PROPERTY(QString message READ message NOTIFY resultChanged FINAL)
    /// The data-flow badge: {provider, model, local, destination, sends, consent: "granted"|"needed"|"local"|"none"}.
    Q_PROPERTY(QVariantMap dataFlow READ dataFlow NOTIFY dataFlowChanged FINAL)
    /// [{id, title, description, valueLabel, value, min, max, track}] ("Cut to the beat" names the music track).
    Q_PROPERTY(QVariantList quickActions READ quickActions NOTIFY dataFlowChanged FINAL)
    /// What the consent prompt shows while state == "consent" (AiController.consentPrompt()).
    Q_PROPERTY(QVariantMap consentPrompt READ consentPrompt NOTIFY stateChanged FINAL)

public:
    explicit AgentController(QObject* parent = nullptr);
    ~AgentController() override;

    static AgentController* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setProjectController(ProjectController* controller);
    void setAiController(AiController* ai);
    /// Tests: runs this model instead of the user's chat model (no consent prompt, no network).
    using ModelFactory = std::function<std::unique_ptr<wabcut::ai::IChatModel>()>;
    void setModelFactory(ModelFactory factory);
    /// Transcripts for the transcript commands (default: the analysis cache next to the project file).
    void setServices(std::shared_ptr<wabcut::command::CommandServices> services);
    /// The services the assistant's commands run with (C.12, M5n: extensions are remote callers like the assistant
    /// and run their commands with the same ones).
    std::shared_ptr<wabcut::command::CommandServices> commandServices() { return services(); }
    /// Limits and options of the runs (tests); `services` and `registry` are filled in by the controller.
    void setAgentOptions(wabcut::ai::agent::AgentOptions options);
    /// The last outcome (tests).
    std::shared_ptr<const wabcut::ai::agent::AgentOutcome> outcome() const { return m_outcome; }
    /// Blocks until the running request finished and its result is shown (tests). False on timeout.
    bool waitForIdle(int timeoutMs);

    bool panelOpen() const { return m_panelOpen; }
    void setPanelOpen(bool open);
    QString prompt() const { return m_prompt; }
    void setPrompt(const QString& prompt);
    QString state() const { return m_state; }
    bool busy() const { return m_state == QLatin1String("running"); }
    QString phase() const { return m_phase; }
    QString reasoning() const { return m_reasoning; }
    QString answer() const { return m_answer; }
    QVariantList steps() const { return m_steps; }
    QString usageText() const { return m_usageText; }
    QVariantList checks() const { return m_checks; }
    bool checksPassed() const;
    QString status() const;
    QString statusText() const { return m_statusText; }
    QString changeSummary() const { return m_changeSummary; }
    bool canAccept() const;
    bool canRefine() const;
    bool stale() const { return m_stale; }
    QString message() const { return m_message; }
    QVariantMap dataFlow() const { return m_dataFlow; }
    QVariantList quickActions() const;
    QVariantMap consentPrompt() const { return m_consentPrompt; }

    /// Opens the panel, optionally with a request typed in the palette.
    Q_INVOKABLE void open(const QString& query = {});
    Q_INVOKABLE void close();
    /// Runs a free-form request (its constraints are read from the text).
    Q_INVOKABLE void ask(const QString& prompt);
    /// Runs a quick action (`value`: its seconds, ignored by actions without one; `track`: the music track of
    /// "Cut to the beat", default the one quickActions lists).
    Q_INVOKABLE void runQuickAction(const QString& id, double value, const QString& track = {});
    /// Continues the staged edit with more instructions.
    Q_INVOKABLE void refine(const QString& feedback);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void accept();
    Q_INVOKABLE void reject();
    /// The consent prompt's answer: allow (always = remember it) or decline.
    Q_INVOKABLE void answerConsent(bool allow, bool always);

signals:
    void panelOpenChanged();
    void promptChanged();
    void stateChanged();
    void progressChanged();
    void resultChanged();
    void dataFlowChanged();
    /// A run finished (any status).
    void finished();

private:
    struct Pending {
        QString prompt;
        wabcut::ai::agent::Constraints constraints;
        std::shared_ptr<const wabcut::ai::agent::AgentOutcome> previous;
    };
    void start(Pending pending);
    void launch(Pending pending, bool allowOnce);
    void onEvent(const wabcut::ai::agent::AgentEvent& event);
    void onFinished(std::shared_ptr<const wabcut::ai::agent::AgentOutcome> outcome);
    void setState(const QString& state);
    void resetRun();
    void showPreview();
    void clearPreview();
    void onRevisionChanged();
    void updateDataFlow();
    void fail(const QString& message);
    std::shared_ptr<wabcut::command::CommandServices> services();
    wabcut::ai::agent::AgentOptions optionsForRun();
    /// The model to run: the factory's, or the user's chat model through the guarded platform.
    std::unique_ptr<wabcut::ai::IChatModel> makeModel(bool allowOnce, QString* error);
    QString chatModelId() const;
    /// Why no chat model can run: the project lock, offline mode, or no model chosen.
    QString noModelReason() const;
    /// The last audio track with clips (where a music bed usually is), as a label ("A3"); "" without one.
    QString musicTrack() const;

    QPointer<ProjectController> m_controller;
    QPointer<AiController> m_ai;
    QQmlEngine* m_engine = nullptr;
    ModelFactory m_factory;
    std::shared_ptr<wabcut::command::CommandServices> m_services;
    bool m_servicesSet = false;
    wabcut::ai::agent::AgentOptions m_options;
    std::unique_ptr<wabcut::ai::agent::EditAgent> m_agent;

    bool m_panelOpen = false;
    QString m_prompt;
    QString m_state = QStringLiteral("idle");
    QString m_phase;
    QString m_reasoning;
    QString m_answer;
    QVariantList m_steps;
    QString m_usageText;
    QVariantList m_checks;
    QString m_statusText;
    QString m_changeSummary;
    QString m_message;
    bool m_stale = false;
    QVariantMap m_dataFlow;
    QVariantMap m_consentPrompt;
    Pending m_pending;

    std::shared_ptr<const wabcut::ai::agent::AgentOutcome> m_outcome;
    std::stop_source m_stop;
    std::jthread m_worker;
    std::uint64_t m_runId = 0;
};

/// Registers the singleton with its create() (the class stays default-constructible for tests).
struct AgentControllerForeign {
    Q_GADGET
    QML_FOREIGN(wabcut::app::AgentController)
    QML_NAMED_ELEMENT(AgentController)
    QML_SINGLETON

public:
    static AgentController* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return AgentController::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
