#include "AgentController.h"

#include "AiController.h"
#include "GenerateController.h"
#include "GraphicsBridge.h"
#include "GlobalBridge.h"
#include "MediaReadinessBridge.h"
#include "MulticamBridge.h"
#include "ObjectMaskBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "ReviewFrames.h"
#include "SearchBridge.h"
#include "ShortsBridge.h"
#include "TimelineView.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/agent/Describe.h>
#include <wabcut/ai/agent/Preview.h>
#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/ai/audio/AudioJobs.h>
#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/highlights/MediaHighlights.h>
#include <wabcut/ai/multicam/MulticamAiCommands.h>
#include <wabcut/ai/search/SearchCommands.h>
#include <wabcut/audio/edit/AudioSync.h>
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
#include <wabcut/plughost/PluginSystem.h>
#endif
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QUrl>

#include <cmath>
#include <filesystem>
#include <set>

namespace wabcut::app {

namespace agent = wabcut::ai::agent;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString money(ai::MicroUsd micro) {
    if (micro <= 0) {
        return QStringLiteral("US$0.00");
    }
    if (micro < 10'000) {
        return QStringLiteral("<US$0.01");
    }
    return QStringLiteral("US$%1").arg(static_cast<double>(micro) / static_cast<double>(ai::kMicroUsdPerUsd), 0, 'f', 2);
}

QString tokens(std::int64_t n) {
    if (n >= 10'000) {
        return QStringLiteral("%1k").arg(std::llround(static_cast<double>(n) / 1000.0));
    }
    if (n >= 1'000) {
        return QStringLiteral("%1k").arg(static_cast<double>(n) / 1000.0, 0, 'f', 1);
    }
    return QString::number(n);
}

QString stepKind(agent::AgentStep::Kind kind) {
    switch (kind) {
    case agent::AgentStep::Kind::Read: return QStringLiteral("read");
    case agent::AgentStep::Kind::Edit: return QStringLiteral("edit");
    case agent::AgentStep::Kind::Error: return QStringLiteral("error");
    case agent::AgentStep::Kind::Verify: return QStringLiteral("verify");
    case agent::AgentStep::Kind::Note: return QStringLiteral("note");
    }
    return QStringLiteral("note");
}

QVariantMap stepMap(const agent::AgentStep& s, bool running) {
    return QVariantMap{{QStringLiteral("kind"), running ? QStringLiteral("running") : stepKind(s.kind)},
                       {QStringLiteral("title"), qs(s.title)},
                       {QStringLiteral("detail"), qs(s.detail)},
                       {QStringLiteral("tool"), qs(s.tool)},
                       {QStringLiteral("ok"), s.ok},
                       {QStringLiteral("running"), running}};
}

QString hostOf(const std::string& url) {
    const QUrl u(qs(url));
    return u.host().isEmpty() ? qs(url) : u.host();
}

/// Every TimelineView of the application's Quick windows.
std::vector<TimelineView*> timelineViews() {
    std::vector<TimelineView*> out;
    std::set<TimelineView*> seen;
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        auto* quick = qobject_cast<QQuickWindow*>(w);
        if (!quick) {
            continue;
        }
        QList<TimelineView*> found = quick->findChildren<TimelineView*>();
        if (quick->contentItem()) {
            found += quick->contentItem()->findChildren<TimelineView*>();
        }
        for (TimelineView* v : found) {
            if (seen.insert(v).second) {
                out.push_back(v);
            }
        }
    }
    return out;
}

} // namespace

AgentController::AgentController(QObject* parent) : QObject(parent) { updateDataFlow(); }

AgentController::~AgentController() {
    m_stop.request_stop();
    if (m_worker.joinable()) {
        m_worker.join(); // queued results for this object are dropped with it
    }
    clearPreview();
}

AgentController* AgentController::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* controller = new AgentController;
    controller->m_engine = qmlEngine;
    if (qmlEngine) {
        controller->setProjectController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        controller->setAiController(qmlEngine->singletonInstance<AiController*>("Wabcut", "AiController"));
    }
    return controller;
}

void AgentController::setProjectController(ProjectController* controller) {
    if (m_controller == controller) {
        return;
    }
    if (m_controller) {
        disconnect(m_controller, nullptr, this, nullptr);
    }
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::revisionChanged, this, &AgentController::onRevisionChanged);
    }
}

void AgentController::setAiController(AiController* ai) {
    if (m_ai == ai) {
        return;
    }
    if (m_ai) {
        disconnect(m_ai, nullptr, this, nullptr);
    }
    m_ai = ai;
    if (m_ai) {
        connect(m_ai, &AiController::modelsChanged, this, &AgentController::updateDataFlow);
        connect(m_ai, &AiController::privacyChanged, this, &AgentController::updateDataFlow);
        // A turn runs the platform's chat model (or its local runtime): it stops and is joined before AiController
        // destroys them (review wave 5, A5-P2-19).
        connect(m_ai, &AiController::shuttingDown, this, [this] {
            m_stop.request_stop();
            if (m_worker.joinable()) {
                m_worker.join();
            }
        });
    }
    updateDataFlow();
}

void AgentController::setModelFactory(ModelFactory factory) {
    m_factory = std::move(factory);
    updateDataFlow();
}

void AgentController::setServices(std::shared_ptr<command::CommandServices> services) {
    m_services = std::move(services);
    m_servicesSet = true;
}

void AgentController::setAgentOptions(agent::AgentOptions options) { m_options = std::move(options); }

bool AgentController::waitForIdle(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (m_state == QLatin1String("running") && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return m_state != QLatin1String("running");
}

// --- Properties -----------------------------------------------------------------------------------------------

void AgentController::setPanelOpen(bool open) {
    if (m_panelOpen == open) {
        return;
    }
    m_panelOpen = open;
    emit panelOpenChanged();
}

void AgentController::setPrompt(const QString& prompt) {
    if (m_prompt == prompt) {
        return;
    }
    m_prompt = prompt;
    emit promptChanged();
}

bool AgentController::checksPassed() const { return m_outcome && m_outcome->verified && m_outcome->verification.passed(); }

QString AgentController::status() const { return m_outcome ? qs(std::string(agent::toString(m_outcome->status))) : QString(); }

bool AgentController::canAccept() const {
    return m_state == QLatin1String("result") && m_outcome && m_outcome->canApply() && m_controller;
}

bool AgentController::canRefine() const { return m_state == QLatin1String("result") && m_outcome && m_outcome->hasChanges(); }

QVariantList AgentController::quickActions() const {
    QVariantList out;
    agent::QuickActionContext context;
    context.transcripts = true;   // the analysis cache may hold them; without one the assistant says so
    context.beatDetection = true; // the analysis services detect beats (C.5)
    const QString music = musicTrack();
    for (const agent::QuickAction& a : agent::visibleQuickActions(context)) {
        if (a.takesTrack && music.isEmpty()) {
            continue; // no music to cut to
        }
        out.push_back(QVariantMap{{QStringLiteral("id"), qs(a.id)},
                                  {QStringLiteral("track"), a.takesTrack ? music : QString()},
                                  {QStringLiteral("title"), qs(a.title)},
                                  {QStringLiteral("description"), qs(a.description)},
                                  {QStringLiteral("valueLabel"), qs(a.valueLabel)},
                                  {QStringLiteral("value"), a.defaultValue},
                                  {QStringLiteral("min"), a.minValue},
                                  {QStringLiteral("max"), a.maxValue}});
    }
    return out;
}

// --- Requests -------------------------------------------------------------------------------------------------

void AgentController::open(const QString& query) {
    if (!query.trimmed().isEmpty() && !busy()) {
        setPrompt(query.trimmed());
    }
    updateDataFlow();
    setPanelOpen(true);
}

void AgentController::close() { setPanelOpen(false); }

void AgentController::ask(const QString& prompt) {
    const QString text = prompt.trimmed();
    if (text.isEmpty() || busy() || !m_controller) {
        return;
    }
    setPrompt(text);
    Pending p;
    p.prompt = text;
    p.constraints = agent::parseConstraints(text.toStdString(), *m_controller->session().snapshot(), m_controller->currentSequenceId());
    start(std::move(p));
}

void AgentController::runQuickAction(const QString& id, double value, const QString& track) {
    if (busy() || !m_controller) {
        return;
    }
    const QString music = track.isEmpty() ? musicTrack() : track;
    const auto request = agent::quickActionRequest(id.toStdString(), value, *m_controller->session().snapshot(),
                                                   m_controller->currentSequenceId(), music.toStdString());
    if (!request) {
        return;
    }
    const agent::QuickAction* action = agent::findQuickAction(id.toStdString());
    QString title = qs(action->title);
    title.replace(QStringLiteral("{value}"), QString::number(value));
    title.replace(QStringLiteral("{track}"), music);
    setPrompt(title);
    Pending p;
    p.prompt = qs(request->prompt);
    p.constraints = request->constraints;
    start(std::move(p));
}

void AgentController::refine(const QString& feedback) {
    const QString text = feedback.trimmed();
    if (text.isEmpty() || !canRefine() || !m_controller) {
        return;
    }
    Pending p;
    p.prompt = text;
    p.previous = m_outcome;
    p.constraints = agent::mergeConstraints(
        m_outcome->constraints, agent::parseConstraints(text.toStdString(), *m_outcome->base, m_outcome->sequence));
    start(std::move(p));
}

void AgentController::start(Pending pending) {
    m_message.clear();
    if (!m_factory) {
        if (!m_ai) {
            fail(tr("The AI platform is not available."));
            return;
        }
        const QString model = chatModelId();
        if (model.isEmpty()) {
            fail(noModelReason());
            return;
        }
        const QVariantMap prompt = m_ai->consentPrompt(model, QStringLiteral("chat"), m_dataFlow.value(QStringLiteral("sends")).toString());
        if (prompt.value(QStringLiteral("needed")).toBool()) {
            m_pending = std::move(pending);
            m_consentPrompt = prompt;
            setState(QStringLiteral("consent"));
            return;
        }
    }
    launch(std::move(pending), false);
}

void AgentController::answerConsent(bool allow, bool always) {
    if (m_state != QLatin1String("consent")) {
        return;
    }
    Pending pending = std::move(m_pending);
    m_pending = {};
    if (!allow) {
        m_message = tr("Nothing was sent: the request needs your consent for this provider.");
        setState(m_outcome ? QStringLiteral("result") : QStringLiteral("idle"));
        emit resultChanged();
        return;
    }
    if (always && m_ai) {
        m_ai->grantConsent(m_consentPrompt.value(QStringLiteral("provider")).toString(), QStringLiteral("chat"));
    }
    launch(std::move(pending), !always);
}

void AgentController::launch(Pending pending, bool allowOnce) {
    if (!m_controller) {
        fail(tr("No project is open."));
        return;
    }
    QString error;
    std::shared_ptr<ai::IChatModel> model = makeModel(allowOnce, &error);
    if (!model) {
        fail(error);
        return;
    }
    agent::AgentRequest request;
    request.prompt = pending.prompt.toStdString();
    request.constraints = pending.constraints;
    request.previous = pending.previous;
    if (!pending.previous) {
        clearPreview();
        request.base = std::make_shared<const ProjectState>(m_controller->session().stateForSave());
        request.baseRevision = m_controller->session().revision();
        request.sequence = m_controller->currentSequenceId();
    }

    if (m_worker.joinable()) {
        m_worker.join(); // the previous run has finished (we are not busy); collect its thread
    }
    resetRun();
    if (pending.previous) {
        for (const agent::AgentStep& s : pending.previous->steps) {
            m_steps.push_back(stepMap(s, false));
        }
    }
    m_agent = std::make_unique<agent::EditAgent>(optionsForRun());
    m_stop = std::stop_source();
    const std::uint64_t runId = ++m_runId;
    m_phase = tr("Starting");
    setState(QStringLiteral("running"));
    emit progressChanged();

    agent::EditAgent* runner = m_agent.get();
    m_worker = std::jthread([this, runner, model, request = std::move(request), token = m_stop.get_token(), runId] {
        // EditAgent::run does not throw; this boundary keeps it that way for the thread (an exception escaping a
        // std::jthread terminates the editor; review wave 3, P1-2).
        std::shared_ptr<const agent::AgentOutcome> outcome;
        try {
            outcome = std::make_shared<const agent::AgentOutcome>(runner->run(
                *model, request,
                [this, runId](const agent::AgentEvent& event) {
                    QMetaObject::invokeMethod(
                        this, [this, runId, event] {
                            if (runId == m_runId) {
                                onEvent(event);
                            }
                        },
                        Qt::QueuedConnection);
                },
                token));
        } catch (const std::exception& e) {
            outcome = std::make_shared<const agent::AgentOutcome>(
                agent::failedOutcome(request, {}, std::string("The assistant stopped on an internal error: ") + e.what()));
        } catch (...) {
            outcome = std::make_shared<const agent::AgentOutcome>(
                agent::failedOutcome(request, {}, "The assistant stopped on an internal error."));
        }
        QMetaObject::invokeMethod(
            this, [this, runId, outcome] {
                if (runId == m_runId) {
                    onFinished(outcome);
                }
            },
            Qt::QueuedConnection);
    });
}

void AgentController::cancel() {
    if (m_state == QLatin1String("consent")) {
        answerConsent(false, false);
        return;
    }
    if (busy()) {
        m_phase = tr("Stopping");
        emit progressChanged();
        m_stop.request_stop();
    }
}

void AgentController::onEvent(const agent::AgentEvent& event) {
    using Kind = agent::AgentEvent::Kind;
    switch (event.kind) {
    case Kind::Phase:
        m_phase = qs(event.text);
        break;
    case Kind::Text:
        m_answer += qs(event.text);
        break;
    case Kind::Reasoning:
        m_reasoning += qs(event.text);
        break;
    case Kind::StepStarted:
        m_steps.push_back(stepMap(event.step, true));
        break;
    case Kind::StepFinished: {
        const QVariantMap done = stepMap(event.step, false);
        if (!m_steps.isEmpty() && m_steps.last().toMap().value(QStringLiteral("running")).toBool()) {
            m_steps.last() = done;
        } else {
            m_steps.push_back(done);
        }
        break;
    }
    case Kind::Usage: {
        const bool local = m_dataFlow.value(QStringLiteral("local")).toBool();
        m_usageText = (local ? tr("No cost (local)") : money(event.cost)) + QStringLiteral(" · ") +
                      tr("%1 in / %2 out").arg(tokens(event.usage.inputTokens), tokens(event.usage.outputTokens));
        break;
    }
    }
    emit progressChanged();
}

void AgentController::onFinished(std::shared_ptr<const agent::AgentOutcome> outcome) {
    m_outcome = std::move(outcome);
    const agent::AgentOutcome& o = *m_outcome;
    m_steps.clear();
    for (const agent::AgentStep& s : o.steps) {
        m_steps.push_back(stepMap(s, false));
    }
    m_answer = qs(o.answer);
    m_phase.clear();
    const bool local = m_dataFlow.value(QStringLiteral("local")).toBool();
    m_usageText = (local ? tr("No cost (local)") : money(o.cost)) + QStringLiteral(" · ") +
                  tr("%1 in / %2 out").arg(tokens(o.usage.inputTokens), tokens(o.usage.outputTokens)) +
                  QStringLiteral(" · ") + tr("%n call(s)", nullptr, o.modelCalls);

    m_checks.clear();
    int failed = 0;
    int blocking = 0;
    for (const agent::VerificationItem& item : o.verification.items) {
        m_checks.push_back(QVariantMap{{QStringLiteral("id"), qs(item.id)},
                                       {QStringLiteral("label"), qs(item.label)},
                                       {QStringLiteral("passed"), item.passed},
                                       {QStringLiteral("detail"), qs(item.detail)},
                                       {QStringLiteral("blocking"), item.blocking}});
        blocking += item.blocking ? 1 : 0;
        failed += (!item.passed && item.blocking) ? 1 : 0;
    }
    switch (o.status) {
    case agent::AgentStatus::Staged:
        m_statusText = tr("All %n check(s) passed", nullptr, blocking);
        break;
    case agent::AgentStatus::VerificationFailed:
        m_statusText = tr("%1 of %2 checks failed — review before accepting").arg(failed).arg(blocking);
        break;
    case agent::AgentStatus::NoChanges:
        m_statusText = tr("No changes");
        break;
    case agent::AgentStatus::Cancelled:
        m_statusText = tr("Cancelled");
        break;
    default:
        m_statusText = qs(o.error);
        break;
    }
    if (o.status != agent::AgentStatus::Staged && o.status != agent::AgentStatus::VerificationFailed &&
        o.status != agent::AgentStatus::NoChanges && o.status != agent::AgentStatus::Cancelled && !o.error.empty()) {
        m_message = qs(o.error);
    }
    m_changeSummary = o.hasChanges() ? qs(agent::describeChanges(o.changes, *o.base, *o.staged)) : QString();
    m_stale = m_controller && m_controller->session().revision() != o.baseRevision;
    if (m_controller) {
        m_controller->notifyClipboardChanged(); // the run may have copied (C.11: canPaste)
    }
    setState(QStringLiteral("result"));
    if (!m_stale) {
        showPreview();
    }
    emit progressChanged();
    emit resultChanged();
    emit finished();
}

void AgentController::accept() {
    if (!canAccept()) {
        return;
    }
    agent::ApplyOptions options;
    options.registry = &ai::edit::editingCommands();
    options.allowed = m_options.allowed;
    options.services = services();
    clearPreview();
    // The batch keeps its own "AI: <request>" undo label.
    const agent::ApplyResult r = agent::applyStaged(m_controller->session(), *m_outcome, options);
    m_message = qs(r.message);
    switch (r.status) {
    case agent::ApplyResult::Status::Applied:
    case agent::ApplyResult::Status::Rebased:
        m_stale = false;
        setState(QStringLiteral("applied"));
        break;
    case agent::ApplyResult::Status::Refused:
    case agent::ApplyResult::Status::Failed:
        m_stale = true;
        break;
    }
    m_controller->notifyClipboardChanged();
    emit resultChanged();
}

void AgentController::reject() {
    if (busy()) {
        return;
    }
    clearPreview();
    m_outcome.reset();
    resetRun();
    m_message.clear();
    setState(QStringLiteral("idle"));
    emit progressChanged();
    emit resultChanged();
}

void AgentController::onRevisionChanged() {
    if (m_state != QLatin1String("result") || !m_outcome || !m_controller || m_stale) {
        return;
    }
    if (m_controller->session().revision() != m_outcome->baseRevision) {
        m_stale = true; // the preview no longer matches what is on screen
        clearPreview();
        emit resultChanged();
    }
}

// --- Helpers --------------------------------------------------------------------------------------------------

void AgentController::setState(const QString& state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    if (state != QLatin1String("consent")) {
        m_consentPrompt.clear();
    }
    emit stateChanged();
    emit resultChanged(); // canAccept / canRefine follow the state
}

void AgentController::resetRun() {
    m_phase.clear();
    m_reasoning.clear();
    m_answer.clear();
    m_steps.clear();
    m_usageText.clear();
    m_checks.clear();
    m_statusText.clear();
    m_changeSummary.clear();
    m_stale = false;
}

void AgentController::fail(const QString& message) {
    m_message = message;
    setState(m_outcome ? QStringLiteral("result") : QStringLiteral("idle"));
    emit resultChanged();
}

void AgentController::showPreview() {
    if (!m_outcome || !m_outcome->hasChanges() || !m_outcome->staged || !m_outcome->base || !m_controller) {
        return;
    }
    const agent::StagedPreview p = agent::stagedPreview(*m_outcome->base, *m_outcome->staged, m_outcome->sequence);
    TimelineView::ExternalPreview preview;
    for (const auto& r : p.removed) {
        preview.removed.push_back({r.track, r.start, r.end});
    }
    for (const auto& r : p.added) {
        preview.added.push_back({r.track, r.start, r.end});
    }
    preview.markers = p.markers;
    for (TimelineView* view : timelineViews()) {
        if (view->controller() == m_controller && m_controller->currentSequenceId() == m_outcome->sequence) {
            view->setExternalPreview(preview);
        }
    }
}

void AgentController::clearPreview() {
    for (TimelineView* view : timelineViews()) {
        if (view->externalPreview()) {
            view->clearExternalPreview();
        }
    }
}

std::shared_ptr<command::CommandServices> AgentController::services() {
    if (m_servicesSet) {
        return m_services;
    }
    // Transcripts the app made (wabcut-infer or a cloud transcriber) live in the analysis cache by source
    // fingerprint; relative media paths resolve against the project file.
    std::filesystem::path projectFile;
    if (m_engine) {
        if (auto* file = m_engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
            projectFile = std::filesystem::path(file->filePath().toStdWString());
        }
    }
    // Beats and shots (C.5) come from the same cache, or are analysed on the worker when missing.
    ai::analysis::MediaAnalyzerOptions analysis;
    analysis.cacheRoot = ai::AnalysisCache::defaultRoot();
    auto cachedTranscripts = std::make_shared<ai::edit::CachedTranscriptProvider>(ai::AnalysisCache::defaultRoot(), projectFile);
    std::vector<std::shared_ptr<command::CommandServices>> parts{
        std::make_shared<ai::edit::TranscriptServices>(cachedTranscripts, std::make_shared<ai::edit::DecoderEnergyProbe>(projectFile)),
        std::make_shared<ai::analysis::AnalysisServices>(
            std::make_shared<ai::analysis::MediaAnalysisProvider>(analysis, projectFile))};
    // G.2: gameplay highlights measure recordings once and keep the measurements in the same cache.
    ai::highlights::MediaHighlightOptions highlights;
    highlights.cacheRoot = ai::AnalysisCache::defaultRoot();
    parts.push_back(std::make_shared<ai::highlights::HighlightServices>(
        std::make_shared<ai::highlights::MediaHighlightProvider>(highlights, projectFile)));
    // A.3: loudness and speech for audio.loudness / normalize / autoDuck ("mix the music under the dialogue, -14 LUFS").
    auto audioServices = audio::edit::makeMediaAudioServices(cachedTranscripts,
                                                             std::make_shared<ai::AnalysisCache>(ai::AnalysisCache::defaultRoot()));
    // A.4: Enhance Speech and stem separation on this computer (wabcut-embed, below normal priority): "clean up the
    // voice on all interview clips". First-use model downloads go through the AI network stack (offline mode refuses).
    ai::audio::LocalAudioOptions local;
    if (m_ai) {
        local.transport = std::make_shared<ai::HttpDownloadTransport>(m_ai->transport());
    }
    audioServices->enhancer = ai::audio::makeSpeechEnhancer(local);
    audioServices->separator = ai::audio::makeStemSeparator(local);
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    // A.6: plugins.list (the assistant adds and removes plugin effects; it never scans: plugins.scan is not its).
    if (auto* plugins = plughost::PluginSystem::current()) {
        parts.push_back(plugins->services());
    }
#endif
    // A.5 / C.8: multicam.create's "audio" sync, on the analysis audio.syncClips uses.
    parts.push_back(std::make_shared<command::MulticamServices>(std::make_shared<audio::edit::MulticamAudioSync>(audioServices->sync)));
    parts.push_back(std::move(audioServices));
    // AI-9: subjects for reframe.createSequence and shorts.create (the app's one detector process).
    parts.push_back(appReframeServices(projectFile));
    // C.16: remove dead time's media activity and Make Shorts version's Auto Reframe.
    parts.push_back(appGamingServices(projectFile));
    // AI-13: "switch cameras to whoever is talking" (analysis.speakers, multicam.autoSwitch) and "edit this 3-camera
    // podcast" (workflow.podcast): each angle's microphone level and the same faces, on this computer.
    parts.push_back(std::make_shared<ai::multicam::SpeakerServices>(appAngleSignals(projectFile)));
    // AI-14: "blur the background behind the speaker", "isolate the red car and brighten it": mask.objectSelect /
    // refine / propagate with SAM 2.1 on this computer (the Object tool's model, kept loaded between runs).
    if (auto* objects = m_engine ? m_engine->singletonInstance<ObjectMaskBridge*>("Wabcut", "ObjectMasks") : nullptr)
        parts.push_back(objects->servicesFor(projectFile));
    // AI-8: search.footage, over the Library's smart search (the same index; opened on first use, from any thread).
    if (auto* smart = m_engine ? m_engine->singletonInstance<SearchBridge*>("Wabcut", "SmartSearch") : nullptr) {
        parts.push_back(std::make_shared<ai::search::SearchServices>(
            [open = smart->searchOpener()]() -> std::expected<std::shared_ptr<ai::search::FootageSearch>, Error> {
                if (auto opened = open()) {
                    return opened;
                }
                return std::unexpected(Error(ErrorCode::IoError, "the search index could not be opened"));
            }));
    }
    if (auto* generate = m_engine ? m_engine->singletonInstance<GenerateController*>("Wabcut", "GenerateController") : nullptr)
        parts.push_back(generate->services());
    // AI-12: "make Thai and Japanese subtitles and a Thai voice-over": captions.translate on the installed local models
    // (or the cloud model the user already allowed); dub.create / narration.create prefill the Dub dialog (paid: the
    // user approves the voice, cost, cap and consent there).
    if (auto* translate = m_engine ? m_engine->singletonInstance<TranslateBridge*>("Wabcut", "TranslateCaptions") : nullptr)
        parts.push_back(translate->agentServices());
    if (auto* dub = m_engine ? m_engine->singletonInstance<DubBridge*>("Wabcut", "Dub") : nullptr)
        parts.push_back(dub->proposalServices());
    // C.11 (M5m): the timeline's clipboard: what the assistant copies pastes with Ctrl+V, and the other way round.
    if (m_controller) {
        // Seen as a cloud-bound caller: clips copied from a no-cloud project stay out of reach elsewhere (S5-P2-12).
        parts.push_back(m_controller->clipboardServices()->forCloudCaller());
    }
    // C.13 (M5o): template.* read the user's template library; text boxes measure with the app's Qt layout.
    if (auto* graphics = m_engine ? m_engine->singletonInstance<GraphicsBridge*>("Wabcut", "Graphics") : nullptr)
        parts.push_back(graphics->graphicsServices());
    // 0.1.1: render.previewInOut / deletePreviewFiles / status on the project's preview cache (the one playback uses).
    if (auto* readiness = m_engine ? m_engine->singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness") : nullptr)
        parts.push_back(readiness->previewServices());
    // C.9 / I.9: review packages the assistant exports get rendered thumbnails (the export path), not placeholders.
    parts.push_back(std::make_shared<command::ReviewServices>(
        std::make_shared<ReviewFrames>([projectFile] { return projectFile; })));
    return std::make_shared<command::ServiceBundle>(std::move(parts));
}

agent::AgentOptions AgentController::optionsForRun() {
    agent::AgentOptions o = m_options;
    o.registry = &ai::edit::editingCommands();
    o.services = services();
    for (const std::string name : {"gen.extend", "gen.broll", "gen.image", "dub.create", "narration.create"}) o.additiveProjectCommands.push_back(name);
    if (o.transcriptDir.empty()) {
        // The redacted record of every run stays on this computer, next to the AI request log.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/ai/agent");
        o.transcriptDir = std::filesystem::path(dir.toStdWString());
    }
    return o;
}

std::unique_ptr<ai::IChatModel> AgentController::makeModel(bool allowOnce, QString* error) {
    if (m_factory) {
        auto model = m_factory();
        if (!model) {
            *error = tr("No model is available.");
        }
        return model;
    }
    if (!m_ai) {
        *error = tr("The AI platform is not available.");
        return nullptr;
    }
    const QString id = chatModelId();
    if (id.isEmpty()) {
        *error = noModelReason();
        return nullptr;
    }
    auto chat = m_ai->platform().chat(id.toStdString(), ai::CallOptions{allowOnce});
    if (!chat) {
        *error = qs(chat.error().message);
        return nullptr;
    }
    return std::move(*chat);
}

QString AgentController::musicTrack() const {
    if (!m_controller) {
        return {};
    }
    const ProjectSnapshot state = m_controller->session().snapshot();
    const Sequence* seq = state->findSequence(m_controller->currentSequenceId());
    if (!seq) {
        return {};
    }
    for (std::size_t i = seq->audioTracks.size(); i-- > 0;) {
        if (!seq->audioTracks[i]->clips.empty()) {
            return QStringLiteral("A%1").arg(i + 1);
        }
    }
    return {};
}

QString AgentController::noModelReason() const {
    if (m_ai) {
        const auto& consent = m_ai->platform().consent();
        if (consent.projectLock()) {
            return tr("This project is locked to no-cloud AI: nothing is sent. Use a model on this computer, or unlock "
                      "the project in Preferences > AI.");
        }
        if (consent.offlineMode()) {
            return tr("Offline mode is on: nothing is sent. Use a model on this computer, or turn offline mode off in "
                      "Preferences > AI.");
        }
    }
    return tr("Choose a chat model in Preferences > AI first.");
}

QString AgentController::chatModelId() const {
    if (!m_ai) {
        return {};
    }
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    if (!resolved || resolved->empty()) {
        return {};
    }
    return qs(resolved->front().model->id);
}

void AgentController::updateDataFlow() {
    QVariantMap flow;
    const ai::ModelSpec* model = nullptr;
    const ai::ProviderSpec* provider = nullptr;
    const ai::ModelRegistry* registry = nullptr;
    std::unique_ptr<ai::IChatModel> probe;
    if (m_factory) {
        probe = m_factory();
        registry = &ai::ModelRegistry::builtin();
        model = probe ? registry->model(probe->spec().id) : nullptr;
    } else if (m_ai) {
        registry = &m_ai->platform().providers().models();
        const QString id = chatModelId();
        model = id.isEmpty() ? nullptr : registry->model(id.toStdString());
    }
    if (model && registry) {
        provider = registry->provider(model->provider);
    }
    const QString sends = tr("Your request, a summary of the sequence (track, clip and marker names and times) and "
                             "the transcript excerpts the assistant reads. Never media files.");
    flow[QStringLiteral("sends")] = sends;
    if (!model || !provider) {
        flow[QStringLiteral("provider")] = QString();
        flow[QStringLiteral("model")] = QString();
        flow[QStringLiteral("local")] = false;
        flow[QStringLiteral("destination")] = QString();
        flow[QStringLiteral("consent")] = QStringLiteral("none");
    } else {
        flow[QStringLiteral("provider")] = qs(provider->name);
        flow[QStringLiteral("model")] = qs(model->name.empty() ? model->id : model->name);
        flow[QStringLiteral("local")] = provider->local;
        flow[QStringLiteral("destination")] = provider->local ? tr("this computer") : hostOf(provider->baseUrl);
        QString consent = provider->local ? QStringLiteral("local") : QStringLiteral("granted");
        if (!provider->local && !m_factory && m_ai) {
            const QVariantMap prompt = m_ai->consentPrompt(qs(model->id), QStringLiteral("chat"), sends);
            consent = prompt.value(QStringLiteral("needed")).toBool() ? QStringLiteral("needed") : QStringLiteral("granted");
        }
        flow[QStringLiteral("consent")] = consent;
    }
    if (flow != m_dataFlow) {
        m_dataFlow = flow;
        emit dataFlowChanged();
    }
}

} // namespace wabcut::app
