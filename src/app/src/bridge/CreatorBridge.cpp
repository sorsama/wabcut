#include "CreatorBridge.h"

#include "AiController.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SearchBridge.h"
#include "SourceMonitorBridge.h"
#include "TimelineView.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/AudioEnergy.h>
#include <wabcut/ai/agent/Preview.h>
#include <wabcut/ai/autozoom/MediaAutoZoom.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/roughcut/MediaTakeProbe.h>
#include <wabcut/ai/roughcut/RoughCutCommands.h>
#include <wabcut/ai/search/SearchCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EditSession.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>
#include <QWindow>

#include <cmath>
#include <set>

namespace wabcut::app {

using command::Json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

std::filesystem::path projectPath(QQmlEngine* engine) {
    auto* file = engine ? engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile") : nullptr;
    if (!file || file->filePath().isEmpty()) {
        return {};
    }
    return std::filesystem::path(file->filePath().toStdWString());
}

QString joined(const std::vector<std::string>& warnings) {
    QStringList lines;
    for (const std::string& w : warnings) {
        lines << qs(w);
    }
    return lines.join(QLatin1Char('\n'));
}

QString clock(double seconds) {
    const int total = static_cast<int>(std::floor(std::max(0.0, seconds)));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString frameUrl(std::uint64_t media, std::int64_t ticks) {
    return QStringLiteral("image://wabcut-frames/%1/%2").arg(mediaKey(MediaId{media})).arg(ticks);
}

/// JSON values as QVariant (objects as maps, arrays as lists).
QVariant variantOf(const Json& j) {
    switch (j.type()) {
    case Json::value_t::object: {
        QVariantMap m;
        for (auto it = j.begin(); it != j.end(); ++it) {
            m.insert(QString::fromStdString(it.key()), variantOf(*it));
        }
        return m;
    }
    case Json::value_t::array: {
        QVariantList l;
        for (const Json& v : j) {
            l.append(variantOf(v));
        }
        return l;
    }
    case Json::value_t::string:
        return QString::fromStdString(j.get<std::string>());
    case Json::value_t::boolean:
        return j.get<bool>();
    case Json::value_t::number_integer:
        return QVariant::fromValue(j.get<qint64>());
    case Json::value_t::number_unsigned:
        return QVariant::fromValue(j.get<quint64>());
    case Json::value_t::number_float:
        return j.get<double>();
    default:
        return {};
    }
}

/// Cancels through `cancelled`; reports "... (NN%)" checkpoints as progress.
class JobControl final : public command::ExecutionControl {
public:
    JobControl(const std::atomic<bool>& cancelled, std::function<void(QString, double)> report)
        : m_cancelled(cancelled), m_report(std::move(report)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        if (m_report) {
            QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
            double progress = -1.0;
            const qsizetype open = text.lastIndexOf(QLatin1Char('('));
            if (open >= 0 && text.endsWith(QStringLiteral("%)"))) {
                bool ok = false;
                const double value = text.mid(open + 1, text.size() - open - 3).toDouble(&ok);
                if (ok) {
                    progress = value / 100.0;
                    text = text.left(open).trimmed();
                }
            }
            m_report(text, progress);
        }
        if (m_cancelled.load()) {
            command::CommandError error;
            error.code = command::ErrorCode::Cancelled;
            error.message = "cancelled";
            return error;
        }
        return std::nullopt;
    }

private:
    const std::atomic<bool>& m_cancelled;
    std::function<void(QString, double)> m_report;
};

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

std::shared_ptr<command::CommandServices> creatorServices(QQmlEngine* engine, TranscriptBridge* transcripts,
                                                          const std::filesystem::path& projectFile,
                                                          std::shared_ptr<ai::IChatModel> chat) {
    std::vector<std::shared_ptr<command::CommandServices>> parts{aiEditingServices(transcripts, projectFile)};
    ai::AudioEnergyServiceConfig energy;
    energy.cacheRoot = ai::AnalysisCache::defaultRoot();
    static std::shared_ptr<ai::AudioEnergyService> service = std::make_shared<ai::AudioEnergyService>(energy);
    ai::roughcut::RoughCutServices::ChatOpener opener;
    if (chat) {
        opener = [chat]() -> std::expected<std::unique_ptr<ai::IChatModel>, ai::AiError> {
            class Borrowed final : public ai::IChatModel {
            public:
                explicit Borrowed(std::shared_ptr<ai::IChatModel> model) : m_model(std::move(model)) {}
                const ai::ModelSpec& spec() const override { return m_model->spec(); }
                std::expected<ai::ChatResponse, ai::AiError> chat(const ai::ChatRequest& r, const ai::ChatEventHandler& e,
                                                                  std::stop_token s) override {
                    return m_model->chat(r, e, s);
                }

            private:
                std::shared_ptr<ai::IChatModel> m_model;
            };
            return std::make_unique<Borrowed>(chat);
        };
    }
    // The sheet reads script files itself (readScript), so the commands never read files here.
    parts.push_back(std::make_shared<ai::roughcut::RoughCutServices>(
        std::move(opener), std::make_shared<ai::roughcut::ServiceTakeProbe>(service, projectFile), ai::roughcut::RoughCutServices::TextEmbedder{},
        false));
    parts.push_back(ai::autozoom::mediaAutoZoomServices(projectFile, false));
    if (auto* smart = engine ? engine->singletonInstance<SearchBridge*>("Wabcut", "SmartSearch") : nullptr) {
        parts.push_back(std::make_shared<ai::search::SearchServices>(
            [open = smart->searchOpener()]() -> std::expected<std::shared_ptr<ai::search::FootageSearch>, Error> {
                if (auto opened = open()) {
                    return opened;
                }
                return std::unexpected(Error(ErrorCode::IoError, "the search index could not be opened"));
            }));
    }
    return std::make_shared<command::ServiceBundle>(std::move(parts));
}

// --- CreatorBase -------------------------------------------------------------------------------------------------------

CreatorBase::CreatorBase(QObject* parent) : QObject(parent) {
    // A job that throws ends with its message.
    m_job.setFailed([self = QPointer<CreatorBase>(this)](const QString& message) -> BridgeJob::Finish {
        return [self, message] {
            if (self) {
                self->setJob({}, 0.0);
                self->m_message = message;
                emit self->resultChanged();
            }
        };
    });
}

CreatorBase::~CreatorBase() { clearGhost(); }

void CreatorBase::setEngine(QQmlEngine* engine) {
    m_engine = engine;
    if (engine) {
        m_controller = engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    }
}

std::shared_ptr<command::CommandServices> CreatorBase::services(std::shared_ptr<ai::IChatModel> chat) {
    if (m_services) {
        return m_services;
    }
    auto* transcripts = m_engine ? m_engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript") : nullptr;
    return creatorServices(m_engine, transcripts, projectPath(m_engine), std::move(chat));
}

void CreatorBase::setJob(QString phase, double progress) {
    m_phase = std::move(phase);
    m_progress = progress;
    emit jobChanged();
}

bool CreatorBase::waitForIdle(int milliseconds) {
    QDeadlineTimer deadline(milliseconds);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!m_job.running()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            return true;
        }
        QThread::msleep(5);
    }
    return false;
}

void CreatorBase::stopJobsWith(::AiController* ai) {
    if (!ai || ai == m_stopsWith) {
        return;
    }
    if (m_stopsWith) {
        disconnect(m_stopsWith, &::AiController::shuttingDown, this, nullptr);
    }
    m_stopsWith = ai;
    connect(ai, &::AiController::shuttingDown, this, [this] { m_job.stop(); });
}

bool CreatorBase::runJob(QString phase, ProjectSnapshot base, std::string name, Json params, bool dryRun, std::function<void(Run&&)> finish,
                         std::shared_ptr<command::CommandServices> services) {
    if (!services) {
        services = this->services();
    }
    if (m_engine && !m_services) { // the app's services: they may hold the platform's models or transport (A5-P2-19)
        stopJobsWith(m_engine->singletonInstance<::AiController*>("Wabcut", "AiController"));
    }
    setPreviewFrameContext(base, projectPath(m_engine)); // the report's thumbnails and the zoom preview's frames
    setJob(std::move(phase), -1.0);
    QPointer<CreatorBase> self(this);
    return m_job.start(this, m_synchronous,
                       [self, base, services, name = std::move(name), params = std::move(params), dryRun,
                        finish = std::move(finish)](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
                           EditSession scratch(*base);
                           command::CommandEngine engine(scratch, ai::edit::editingCommands(),
                                                         command::EnginePolicy{command::CapabilitySet::all(), 16, services});
                           JobControl control(cancelled, [self](QString text, double progress) {
                               QMetaObject::invokeMethod(
                                   QCoreApplication::instance(), [self, text, progress] { if (self) self->setJob(text, progress); },
                                   Qt::QueuedConnection);
                           });
                           command::CommandRequest request;
                           request.command = name;
                           request.params = params;
                           request.dryRun = dryRun;
                           command::CommandResult result = engine.execute(request, &control);
                           Run run;
                           run.data = std::move(result.data);
                           run.warnings = std::move(result.warnings);
                           run.preview = result.preview ? result.preview : (result.committed ? scratch.snapshot() : nullptr);
                           run.error = std::move(result.error);
                           return [self, finish, run = std::move(run)]() mutable {
                               if (!self) {
                                   return;
                               }
                               self->setJob({}, 0.0);
                               finish(std::move(run));
                           };
                       });
}

QString CreatorBase::commitStaged(const QString& label) {
    if (!m_staged || !m_controller) {
        return tr("Nothing to apply");
    }
    if (m_controller->session().revision() != m_stagedRevision) {
        discard();
        return tr("The project changed since the preview: run it again");
    }
    const auto staged = m_staged;
    if (auto committed = m_controller->session().commit(label.toStdString(), *staged); !committed) {
        return qs(committed.error().message);
    }
    clearGhost();
    m_staged.reset();
    emit resultChanged();
    return {};
}

void CreatorBase::discard() {
    clearGhost();
    m_staged.reset();
    emit resultChanged();
}

void CreatorBase::showGhost(SequenceId sequence) {
    if (!m_staged || !m_stagedBase || !m_controller) {
        return;
    }
    const ai::agent::StagedPreview p = ai::agent::stagedPreview(*m_stagedBase, *m_staged, sequence);
    TimelineView::ExternalPreview preview;
    for (const auto& r : p.removed) {
        preview.removed.push_back({r.track, r.start, r.end});
    }
    for (const auto& r : p.added) {
        preview.added.push_back({r.track, r.start, r.end});
    }
    preview.markers = p.markers;
    for (TimelineView* view : timelineViews()) {
        if (view->controller() == m_controller && m_controller->currentSequenceId() == sequence) {
            view->setExternalPreview(preview);
        }
    }
}

void CreatorBase::clearGhost() {
    for (TimelineView* view : timelineViews()) {
        if (view->externalPreview()) {
            view->clearExternalPreview();
        }
    }
}

// --- RoughCutBridge ----------------------------------------------------------------------------------------------------

RoughCutBridge* RoughCutBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new RoughCutBridge;
    bridge->setEngine(qmlEngine);
    if (qmlEngine) {
        bridge->m_ai = qmlEngine->singletonInstance<::AiController*>("Wabcut", "AiController");
        bridge->stopJobsWith(bridge->m_ai);
        bridge->m_transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
        bridge->m_projectFile = qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        (void)qmlEngine->singletonInstance<ShortsBridge*>("Wabcut", "Shorts"); // registers image://wabcut-frames
    }
    return bridge;
}

RoughCutBridge::RoughCutBridge(QObject* parent) : CreatorBase(parent) {}

bool RoughCutBridge::cloudAvailable() const {
    if (!m_ai || m_ai->offlineMode() || (m_controller && m_controller->snapshot()->aiNoCloud)) {
        return false;
    }
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    return resolved && !resolved->empty();
}

QVariantMap RoughCutBridge::consentPrompt(int characters) const {
    if (!cloudAvailable()) {
        return {};
    }
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    if (!resolved || resolved->empty()) {
        return {};
    }
    return m_ai->consentPrompt(QString::fromStdString(resolved->front().model->id), QStringLiteral("chat"),
                               tr("the outline (%n character(s)) and the transcript sentences that may match its bullets, as text",
                                  nullptr, characters));
}

QString RoughCutBridge::readScript(const QUrl& file) {
    QFile in(file.isLocalFile() ? file.toLocalFile() : file.toString());
    const QString lower = in.fileName().toLower();
    if (!lower.endsWith(QStringLiteral(".txt")) && !lower.endsWith(QStringLiteral(".md")) && !lower.endsWith(QStringLiteral(".markdown")) &&
        !lower.endsWith(QStringLiteral(".fountain"))) {
        m_message = tr("Choose a .txt or .md file");
        emit resultChanged();
        return {};
    }
    if (in.size() > 256 * 1024 || !in.open(QIODevice::ReadOnly)) {
        m_message = tr("The file cannot be read or is larger than 256 KB");
        emit resultChanged();
        return {};
    }
    return QString::fromUtf8(in.readAll());
}

QString RoughCutBridge::preview(const QString& text, const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    if (text.trimmed().isEmpty()) {
        return tr("Paste a script, an outline or bullet points first");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    Json params{{"text", text.toStdString()},
                {"prefer", options.value(QStringLiteral("prefer"), QStringLiteral("best")).toString().toStdString()},
                {"split", options.value(QStringLiteral("split"), QStringLiteral("auto")).toString().toStdString()},
                {"cleanup", options.value(QStringLiteral("cleanup"), false).toBool()},
                {"broll", options.value(QStringLiteral("broll"), false).toBool()}};
    if (options.value(QStringLiteral("scope")).toString() == QLatin1String("sequence") && base->findSequence(m_controller->currentSequenceId())) {
        params["fromSequence"] = m_controller->currentSequenceId().value;
    }
    if (const QString name = options.value(QStringLiteral("name")).toString().trimmed(); !name.isEmpty()) {
        params["name"] = name.toStdString();
    }
    std::shared_ptr<ai::IChatModel> chat;
    const bool interpret = options.value(QStringLiteral("interpret"), false).toBool();
    if (interpret && !m_services && cloudAvailable()) {
        // Never a quiet fallback to literal matching (review wave 5, A5-P2-27): the sheet asks first (consentPrompt) and
        // passes the answer; without one nothing runs.
        const bool allowOnce = options.value(QStringLiteral("allowOnce"), false).toBool();
        if (!allowOnce && consentPrompt(static_cast<int>(text.size())).value(QStringLiteral("needed")).toBool()) {
            return tr("Placing bullets with your AI needs your consent first: nothing was sent");
        }
        auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
        if (resolved && !resolved->empty()) {
            if (auto model = m_ai->platform().chat(resolved->front().model->id, ai::CallOptions{allowOnce, {}})) {
                chat = std::shared_ptr<ai::IChatModel>(std::move(*model));
            }
        }
    }
    params["interpret"] = interpret;
    discard();
    const std::uint64_t revision = m_controller->session().revision();
    QPointer<RoughCutBridge> self(this);
    const bool started = runJob(tr("Finding the lines in the recordings"), base, "roughcut.fromScript", params, true,
                                [self, base, revision](Run&& run) {
                                    if (!run.error && run.preview) {
                                        self->m_staged = run.preview;
                                        self->m_stagedBase = base;
                                        self->m_stagedRevision = revision;
                                        self->m_cut = SequenceId{run.data.value("sequence", std::uint64_t{0})};
                                        self->setReport(run.data, *run.preview);
                                        self->m_message = joined(run.warnings);
                                    } else {
                                        self->m_report.clear();
                                        self->m_message = run.error && run.error->code == command::ErrorCode::Cancelled ? tr("Cancelled")
                                                          : run.error ? qs(run.error->message)
                                                                      : tr("Nothing was made");
                                        if (run.error && run.error->details.contains("report")) {
                                            self->setReport(run.error->details["report"], *base);
                                        }
                                    }
                                    emit self->resultChanged();
                                },
                                services(chat));
    return started ? QString() : tr("Busy");
}

void RoughCutBridge::setReport(const Json& data, const ProjectState& state) {
    QVariantMap report = variantOf(data).toMap();
    QVariantList beats;
    for (const Json& beat : data.value("beats", Json::array())) {
        QVariantMap b = variantOf(beat).toMap();
        if (beat.contains("start") && beat.contains("end")) {
            b.insert(QStringLiteral("range"), QStringLiteral("%1 – %2").arg(clock(beat["start"].get<double>()), clock(beat["end"].get<double>())));
        }
        QVariantList takes;
        for (const Json& take : beat.value("takes", Json::array())) {
            QVariantMap t = variantOf(take).toMap();
            const std::uint64_t media = take.value("media", std::uint64_t{0});
            const MediaItem* item = state.findMedia(MediaId{media});
            if (item && item->info.hasVideo()) {
                const std::int64_t in = take.value("sourceInTicks", std::int64_t{0});
                const std::int64_t out = take.value("sourceOutTicks", in);
                t.insert(QStringLiteral("thumb"), frameUrl(media, in + (out - in) / 2));
            }
            t.insert(QStringLiteral("sourceRange"),
                     QStringLiteral("%1 – %2").arg(clock(take.value("sourceIn", 0.0)), clock(take.value("sourceOut", 0.0))));
            takes.append(t);
        }
        b.insert(QStringLiteral("takes"), takes);
        beats.append(b);
    }
    report.insert(QStringLiteral("beats"), beats);
    m_report = report;
}

QString RoughCutBridge::swapTake(int beat, int take) {
    if (!m_staged || !m_cut.isValid()) {
        return tr("Make a rough cut first");
    }
    const ProjectSnapshot base = std::make_shared<const ProjectState>(*m_staged);
    QPointer<RoughCutBridge> self(this);
    const bool started = runJob(tr("Swapping the take"), base, "roughcut.swapTake",
                                Json{{"sequence", m_cut.value}, {"beat", beat}, {"take", take}}, false, [self, beat, take](Run&& run) {
                                    if (run.error || !run.preview) {
                                        self->m_message = run.error ? qs(run.error->message) : tr("The take was not swapped");
                                        emit self->resultChanged();
                                        return;
                                    }
                                    self->m_staged = run.preview; // the staged cut, with the other take
                                    QVariantMap report = self->m_report;
                                    QVariantList beats = report.value(QStringLiteral("beats")).toList();
                                    if (beat >= 1 && beat <= beats.size()) {
                                        QVariantMap b = beats[beat - 1].toMap();
                                        QVariantList takes = b.value(QStringLiteral("takes")).toList();
                                        for (int k = 0; k < takes.size(); ++k) {
                                            QVariantMap t = takes[k].toMap();
                                            t.insert(QStringLiteral("chosen"), k + 1 == take);
                                            takes[k] = t;
                                        }
                                        b.insert(QStringLiteral("takes"), takes);
                                        beats[beat - 1] = b;
                                    }
                                    report.insert(QStringLiteral("beats"), beats);
                                    self->m_report = report;
                                    self->m_message = tr("Line %1 now uses take %2").arg(beat).arg(take);
                                    emit self->resultChanged();
                                });
    return started ? QString() : tr("Busy");
}

QString RoughCutBridge::apply() {
    const SequenceId cut = m_cut;
    if (QString error = commitStaged(tr("Rough Cut from Script")); !error.isEmpty()) {
        return error;
    }
    const ProjectSnapshot now = m_controller->snapshot();
    for (std::size_t i = 0; i < now->sequences.size(); ++i) {
        if (now->sequences[i]->id == cut) {
            m_controller->setCurrentSequenceIndex(static_cast<int>(i));
        }
    }
    m_report.clear();
    emit resultChanged();
    return {};
}

void RoughCutBridge::discard() {
    m_cut = {};
    CreatorBase::discard();
}

// --- TakesBridge -------------------------------------------------------------------------------------------------------

TakesBridge* TakesBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new TakesBridge;
    bridge->setEngine(qmlEngine);
    return bridge;
}

TakesBridge::TakesBridge(QObject* parent) : CreatorBase(parent) {}

QString TakesBridge::find(const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const SequenceId sequence = m_controller->currentSequenceId();
    if (!base->findSequence(sequence)) {
        return tr("No sequence");
    }
    Json params{{"sequence", sequence.value}};
    if (options.value(QStringLiteral("allMedia"), false).toBool()) {
        Json media = Json::array();
        for (const MediaPtr& m : base->media) {
            if (m->info.hasAudio() && !m->isGenerator()) {
                media.push_back(m->id.value);
            }
        }
        if (!media.empty()) {
            params["media"] = std::move(media);
        }
    }
    if (options.contains(QStringLiteral("minSimilarity"))) {
        params["minSimilarity"] = options.value(QStringLiteral("minSimilarity")).toDouble();
    }
    m_findParams = params;
    m_sequence = sequence;
    discard();
    QPointer<TakesBridge> self(this);
    const bool started = runJob(tr("Comparing takes"), base, "takes.find", params, false, [self](Run&& run) {
        self->m_groups.clear();
        if (run.error) {
            self->m_message = run.error->code == command::ErrorCode::Cancelled ? tr("Cancelled") : qs(run.error->message);
        } else {
            for (const Json& g : run.data.value("groups", Json::array())) {
                QVariantMap group = variantOf(g).toMap();
                QVariantList takes;
                for (const Json& t : g.value("takes", Json::array())) {
                    QVariantMap take = variantOf(t).toMap();
                    take.insert(QStringLiteral("range"), QStringLiteral("%1 – %2").arg(clock(t.value("start", 0.0)), clock(t.value("end", 0.0))));
                    takes.append(take);
                }
                group.insert(QStringLiteral("takes"), takes);
                self->m_groups.append(group);
            }
            self->m_message = self->m_groups.isEmpty() ? tr("No line was recorded more than once") : joined(run.warnings);
        }
        emit self->resultChanged();
    });
    return started ? QString() : tr("Busy");
}

double TakesBridge::audition(int group, int take) {
    if (group < 1 || group > m_groups.size() || !m_controller) {
        return 0.0;
    }
    const QVariantList takes = m_groups[group - 1].toMap().value(QStringLiteral("takes")).toList();
    if (take < 1 || take > takes.size()) {
        return 0.0;
    }
    const QVariantMap t = takes[take - 1].toMap();
    const double start = t.value(QStringLiteral("start")).toDouble();
    const double end = t.value(QStringLiteral("end")).toDouble();
    if (t.value(QStringLiteral("onTimeline")).toBool()) {
        m_controller->setPlayhead(Ticks{t.value(QStringLiteral("startTicks")).toLongLong()});
    } else if (auto* source = m_engine ? m_engine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor") : nullptr) {
        const auto media = t.value(QStringLiteral("media")).toULongLong();
        const MediaItem* item = m_controller->snapshot()->findMedia(MediaId{media});
        if (item && source->load(mediaKey(MediaId{media})) && item->info.duration > Ticks{}) {
            source->seekFraction(t.value(QStringLiteral("sourceIn")).toDouble() / item->info.duration.toSeconds());
        }
    }
    return std::max(0.0, end - start);
}

QString TakesBridge::stageUse(Json params, const QString& phase) {
    if (!m_controller) {
        return tr("No project");
    }
    for (auto it = m_findParams.begin(); it != m_findParams.end(); ++it) {
        if (!params.contains(it.key())) {
            params[it.key()] = *it;
        }
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const std::uint64_t revision = m_controller->session().revision();
    discard();
    QPointer<TakesBridge> self(this);
    const SequenceId sequence = m_sequence;
    const bool started = runJob(phase, base, "takes.use", params, true, [self, base, revision, sequence](Run&& run) {
        self->m_review.clear();
        if (run.error) {
            self->m_message = qs(run.error->message);
        } else if (!run.data.value("changed", false) || !run.preview) {
            self->m_message = joined(run.warnings);
        } else {
            self->m_staged = run.preview;
            self->m_stagedBase = base;
            self->m_stagedRevision = revision;
            self->m_review = variantOf(run.data.value("review", Json::array())).toList();
            self->m_message = joined(run.warnings);
            self->showGhost(sequence);
        }
        emit self->resultChanged();
    });
    return started ? QString() : tr("Busy");
}

QString TakesBridge::use(int group, int take) {
    return stageUse(Json{{"group", group}, {"take", take}}, tr("Using the take"));
}

QString TakesBridge::useBest() { return stageUse(Json{{"best", true}}, tr("Keeping the best takes")); }

QString TakesBridge::apply() {
    if (QString error = commitStaged(tr("Use Takes")); !error.isEmpty()) {
        return error;
    }
    m_review.clear();
    m_groups.clear();
    emit resultChanged();
    return {};
}

void TakesBridge::discard() {
    m_review.clear();
    CreatorBase::discard();
}

// --- AutoZoomBridge ----------------------------------------------------------------------------------------------------

AutoZoomBridge* AutoZoomBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new AutoZoomBridge;
    bridge->setEngine(qmlEngine);
    if (qmlEngine) {
        (void)qmlEngine->singletonInstance<ShortsBridge*>("Wabcut", "Shorts"); // registers image://wabcut-frames
    }
    return bridge;
}

AutoZoomBridge::AutoZoomBridge(QObject* parent) : CreatorBase(parent) {}

QString AutoZoomBridge::preview(const QString& clipText, const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    const auto clip = parseClipKey(clipText);
    const ProjectSnapshot base = m_controller->snapshot();
    const SequenceId sequence = m_controller->currentSequenceId();
    const Sequence* seq = base->findSequence(sequence);
    const auto location = clip && seq ? seq->findClip(*clip) : std::nullopt;
    if (!location) {
        return tr("Choose a video clip");
    }
    Json params{{"sequence", sequence.value}, {"clip", clip->value}, {"preview", true}};
    for (const char* number : {"maxZoom", "hold", "idle", "transition", "smoothness"}) {
        if (options.contains(QString::fromLatin1(number))) {
            params[number] = options.value(QString::fromLatin1(number)).toDouble();
        }
    }
    for (const char* flag : {"clicks", "typing", "dwell"}) {
        if (options.contains(QString::fromLatin1(flag))) {
            params[flag] = options.value(QString::fromLatin1(flag)).toBool();
        }
    }
    if (options.contains(QStringLiteral("easing"))) {
        params["easing"] = options.value(QStringLiteral("easing")).toString().toStdString();
    }
    const std::uint64_t revision = m_controller->session().revision();
    const Clip c = location->clip();
    discard();
    m_clip = *clip;
    QPointer<AutoZoomBridge> self(this);
    const bool started = runJob(tr("Looking for activity"), base, "autozoom.apply", params, true, [self, base, revision, c](Run&& run) {
        self->m_preview.clear();
        if (run.error || !run.preview) {
            self->m_message = run.error ? qs(run.error->message) : tr("Nothing to zoom");
            emit self->resultChanged();
            return;
        }
        self->m_staged = run.preview;
        self->m_stagedBase = base;
        self->m_stagedRevision = revision;
        QVariantMap preview = variantOf(run.data).toMap();
        QVariantList path;
        const MediaItem* media = base->findMedia(c.media);
        for (const Json& sample : run.data.value("path", Json::array())) {
            if (!sample.is_array() || sample.size() != 6) {
                continue;
            }
            const double t = sample[0].get<double>();
            const Ticks timeline{static_cast<std::int64_t>(std::llround(t * static_cast<double>(kTicksPerSecond)))};
            QVariantMap s{{QStringLiteral("t"), t},         {QStringLiteral("zoom"), sample[1].get<double>()},
                          {QStringLiteral("x"), sample[2].get<double>()}, {QStringLiteral("y"), sample[3].get<double>()},
                          {QStringLiteral("w"), sample[4].get<double>()}, {QStringLiteral("h"), sample[5].get<double>()}};
            if (media && !media->isGenerator()) {
                s.insert(QStringLiteral("frame"), frameUrl(c.media.value, c.sourceTimeAt(std::clamp(timeline, c.start(), c.end() - Ticks{1})).value));
            }
            path.append(s);
        }
        preview.insert(QStringLiteral("path"), path);
        preview.insert(QStringLiteral("name"), qs(c.name()));
        preview.insert(QStringLiteral("start"), c.start().toSeconds());
        preview.insert(QStringLiteral("duration"), c.duration().toSeconds());
        self->m_preview = preview;
        self->m_message = joined(run.warnings);
        emit self->resultChanged();
    });
    return started ? QString() : tr("Busy");
}

QString AutoZoomBridge::apply() {
    if (QString error = commitStaged(tr("Auto Zoom")); !error.isEmpty()) {
        return error;
    }
    m_preview.clear();
    emit resultChanged();
    return {};
}

void AutoZoomBridge::discard() {
    m_preview.clear();
    CreatorBase::discard();
}

} // namespace wabcut::app
