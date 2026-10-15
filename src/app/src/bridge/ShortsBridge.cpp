#include "ShortsBridge.h"

#include "AiController.h"
#include "BridgeIds.h"
#include "ExportController.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "RedactedPictures.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/reframe/Reframe.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/ai/search/MediaSearchAccess.h>
#include <wabcut/ai/shorts/ShortsCommands.h>
#include <wabcut/ai/vision/MediaSubjects.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/editor/Activity.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/GamingOps.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QImage>
#include <QQuickImageProvider>
#include <QRegularExpression>
#include <QThread>

#include <cmath>
#include <list>
#include <map>

namespace wabcut::app {

namespace vision = ai::vision;
using command::Json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

/// What the frame provider decodes from: the project the bridges last looked at.
struct FrameContext {
    std::mutex mutex;
    std::shared_ptr<const ProjectState> state;
    std::filesystem::path projectFile;
    std::function<QImage(const MediaItem&, Ticks)> testFrames;
};

FrameContext& frameContext() {
    static FrameContext context;
    return context;
}

void setFrameContext(std::shared_ptr<const ProjectState> state, std::filesystem::path projectFile) {
    FrameContext& c = frameContext();
    std::lock_guard lock(c.mutex);
    c.state = std::move(state);
    c.projectFile = std::move(projectFile);
}

/// image://wabcut-frames/<media key>/<source ticks>: one frame of a media item, decoded off the GUI thread.
class FrameProvider final : public QQuickImageProvider {
public:
    FrameProvider() : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        QImage image = frame(id);
        if (image.isNull()) {
            image = QImage(16, 9, QImage::Format_RGB888);
            image.fill(Qt::black);
        }
        if (requestedSize.isValid() && !requestedSize.isEmpty()) {
            image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        if (size) {
            *size = image.size();
        }
        return image;
    }

private:
    QImage frame(const QString& id) {
        const QStringList parts = id.split(QLatin1Char('/'));
        const auto media = parts.size() == 2 ? detail::parseId<MediaId>(QLatin1Char('m'), parts[0]) : std::nullopt;
        bool ok = false;
        const qint64 ticks = parts.size() == 2 ? parts[1].toLongLong(&ok) : 0;
        if (!media || !ok) {
            return {};
        }
        std::shared_ptr<const ProjectState> state;
        std::filesystem::path projectFile;
        std::function<QImage(const MediaItem&, Ticks)> testFrames;
        {
            FrameContext& c = frameContext();
            std::lock_guard lock(c.mutex);
            state = c.state;
            projectFile = c.projectFile;
            testFrames = c.testFrames;
        }
        const MediaItem* item = state ? state->findMedia(*media) : nullptr;
        if (!item) {
            return {};
        }
        // The media's source redactions apply here as on the library's thumbnails and search frames (they are decoded
        // straight from the file); cached with them, so a changed redaction is a new picture (wave-4 P2-9).
        const QString key = id + QStringLiteral("#") + QString::number(redacted::keyOf(*state, *media), 16);
        {
            std::lock_guard lock(m_mutex);
            if (const auto it = m_cache.find(key); it != m_cache.end()) {
                return it->second;
            }
        }
        if (testFrames) {
            QImage image = testFrames(*item, Ticks{ticks});
            redacted::apply(image, *state, *media, Ticks{ticks});
            return image;
        }
        ai::search::MediaSearchAccess access(ai::search::MediaSearchAccessOptions{projectFile, {}});
        const Ticks times[] = {Ticks{ticks}};
        auto frames = access.frames(*item, times, 480, {});
        if (!frames || frames->empty() || !frames->front().valid()) {
            return {};
        }
        const ai::search::Image& picture = frames->front();
        QImage image = QImage(picture.rgb.data(), picture.width, picture.height, picture.width * 3, QImage::Format_RGB888).copy();
        redacted::apply(image, *state, *media, Ticks{ticks});
        std::lock_guard lock(m_mutex);
        m_order.push_back(key);
        m_cache[key] = image;
        while (m_order.size() > 160) {
            m_cache.erase(m_order.front());
            m_order.pop_front();
        }
        return image;
    }

    std::mutex m_mutex;
    std::map<QString, QImage> m_cache;
    std::list<QString> m_order;
};

} // namespace

void setPreviewFrameContext(std::shared_ptr<const ProjectState> state, std::filesystem::path projectFile) {
    setFrameContext(std::move(state), std::move(projectFile));
}

void setPreviewFramesForTests(std::function<QImage(const MediaItem&, Ticks)> frames) {
    FrameContext& c = frameContext();
    std::lock_guard lock(c.mutex);
    c.testFrames = std::move(frames);
}

namespace {

void addFrameProvider(QQmlEngine* engine) {
    if (engine && !engine->imageProvider(QStringLiteral("wabcut-frames"))) {
        engine->addImageProvider(QStringLiteral("wabcut-frames"), new FrameProvider);
    }
}

QString frameUrl(MediaId media, Ticks sourceTime) {
    return QStringLiteral("image://wabcut-frames/%1/%2").arg(mediaKey(media)).arg(sourceTime.value);
}

/// The frame shown at timeline time `t` of a sequence: its top enabled video clip's media and source time.
std::optional<std::pair<MediaId, Ticks>> pictureAt(const ProjectState& state, const Sequence& seq, Ticks t) {
    for (auto it = seq.videoTracks.rbegin(); it != seq.videoTracks.rend(); ++it) {
        if (!(*it)->enabled) {
            continue;
        }
        for (const Clip& clip : (*it)->clips) {
            if (clip.enabled && clip.start() <= t && t < clip.end()) {
                const MediaItem* media = state.findMedia(clip.media);
                if (media && !media->isGenerator() && !media->info.video.empty()) {
                    return std::pair{clip.media, clip.sourceTimeAt(t)};
                }
            }
        }
    }
    return std::nullopt;
}

/// Cancels through `cancelled`; reports "... (NN%)" checkpoints as progress.
class JobControl final : public command::ExecutionControl {
public:
    JobControl(const std::atomic<bool>& cancelled, std::function<void(QString, double)> report)
        : m_cancelled(cancelled), m_report(std::move(report)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        if (m_report) {
            static const QRegularExpression percent(QStringLiteral("\\((\\d+)%\\)\\s*$"));
            QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
            const auto match = percent.match(text);
            double progress = -1.0;
            if (match.hasMatch()) {
                progress = match.captured(1).toDouble() / 100.0;
                text = text.left(match.capturedStart()).trimmed();
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

std::filesystem::path projectPath(ProjectLifecycle* file) {
    if (!file || file->filePath().isEmpty()) {
        return {};
    }
    return std::filesystem::path(file->filePath().toStdWString());
}

/// The app's subject provider: one detector process, recreated once the detectors are downloaded.
std::shared_ptr<vision::MediaSubjectProvider> appSubjectProvider(const std::filesystem::path& projectFile) {
    static std::mutex mutex;
    static std::shared_ptr<vision::MediaSubjectProvider> provider;
    static bool withDetector = false;
    std::lock_guard lock(mutex);
    auto choice = vision::openSubjectDetector({}, {}, std::max(1, QThread::idealThreadCount() / 2));
    const bool available = choice.detector != nullptr;
    if (!provider || available != withDetector) {
        vision::MediaSubjectOptions options;
        options.projectFile = projectFile;
        options.cacheRoot = ai::AnalysisCache::defaultRoot();
        provider = std::make_shared<vision::MediaSubjectProvider>(options, available ? choice.detector : nullptr);
        withDetector = available;
    }
    provider->setProjectFile(projectFile);
    return provider;
}

QString joined(const std::vector<std::string>& warnings) {
    QStringList lines;
    for (const std::string& w : warnings) {
        lines << qs(w);
    }
    return lines.join(QLatin1Char('\n'));
}

QString clock(double seconds) {
    const int total = static_cast<int>(std::floor(seconds));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

int sequenceIndex(const ProjectState& state, SequenceId id) {
    for (std::size_t i = 0; i < state.sequences.size(); ++i) {
        if (state.sequences[i]->id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

std::shared_ptr<command::CommandServices> appReframeServices(const std::filesystem::path& projectFile) {
    return std::make_shared<ai::reframe::ReframeServices>(appSubjectProvider(projectFile));
}

std::shared_ptr<command::CommandServices> appGamingServices(const std::filesystem::path& projectFile) {
    auto provider = appSubjectProvider(projectFile);
    // sequence.makeVersion: Auto Reframe for versions of edits without a facecam (as wabcut-cli does).
    auto versions = std::make_shared<command::VersionServices>(
        [provider](const ProjectState& state, SequenceId source, int width, int height,
                   std::string name) -> std::expected<ReframedSequence, Error> {
            namespace rf = ai::reframe;
            rf::ReframeOptions options;
            options.aspect = rf::Aspect{width, height};
            options.width = width;
            options.height = height;
            options.name = std::move(name);
            const rf::SubjectSource subjects = [provider](const MediaItem& media, int stream) {
                return provider->subjects(media, stream, [](double) { return true; });
            };
            auto made = rf::reframeSequence(state, source, options, subjects);
            if (!made)
                return std::unexpected(made.error());
            return ReframedSequence{std::move(made->state), made->sequence, std::move(made->warnings)};
        });
    return std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<command::ActivityServices>(editor::activityMeasure(projectFile)), std::move(versions)});
}

std::shared_ptr<command::CommandServices> aiEditingServices(TranscriptBridge* transcripts, const std::filesystem::path& projectFile,
                                                            std::shared_ptr<ai::IChatModel> chat) {
    std::vector<std::shared_ptr<command::CommandServices>> parts;
    std::shared_ptr<ai::edit::TranscriptServices> transcriptServices = transcripts ? transcripts->services() : nullptr;
    if (transcriptServices) {
        parts.push_back(transcriptServices);
    }
    parts.push_back(audio::edit::makeMediaAudioServices(transcriptServices ? transcriptServices->transcripts : nullptr,
                                                        std::make_shared<ai::AnalysisCache>(ai::AnalysisCache::defaultRoot())));
    parts.push_back(std::make_shared<ai::reframe::ReframeServices>(appSubjectProvider(projectFile)));
    if (chat) {
        auto handOver = std::make_shared<std::shared_ptr<ai::IChatModel>>(std::move(chat));
        parts.push_back(std::make_shared<ai::shorts::ShortsServices>([handOver]() -> std::expected<std::unique_ptr<ai::IChatModel>, ai::AiError> {
            // A thin owner around the GUI-made model (created once, used once per find).
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
            if (!*handOver) {
                return std::unexpected(ai::AiError(ai::AiErrorCode::ModelMissing, "no cloud chat model"));
            }
            return std::make_unique<Borrowed>(*handOver);
        }));
    }
    return std::make_shared<command::ServiceBundle>(std::move(parts));
}

// --- BridgeJob ---------------------------------------------------------------------------------------------------------

namespace {

/// `work`, with an exception turned into `failed`'s result: on the worker thread it would end the app
/// (std::terminate), and a synchronous job would throw into a Qt slot.
BridgeJob::Finish runGuarded(const BridgeJob::Work& work, const BridgeJob::Failed& failed, const std::atomic<bool>& cancelled) {
    QString message;
    try {
        return work(cancelled);
    } catch (const std::exception& e) {
        message = QString::fromUtf8(e.what());
    } catch (...) {
        message = QStringLiteral("unknown error");
    }
    qWarning("A background job failed: %s", qUtf8Printable(message));
    if (!failed) {
        return {};
    }
    try {
        return failed(message);
    } catch (...) {
        return {};
    }
}

/// The job's GUI-thread part; an exception there is logged, not thrown into the event loop.
void finishGuarded(const BridgeJob::Finish& finish) {
    if (!finish) {
        return;
    }
    try {
        finish();
    } catch (const std::exception& e) {
        qWarning("A background job's result failed: %s", e.what());
    } catch (...) {
        qWarning("A background job's result failed");
    }
}

} // namespace

BridgeJob::~BridgeJob() {
    m_cancelled = true;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void BridgeJob::stop() {
    m_cancelled = true;
    reap();
}

void BridgeJob::reap() {
    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
        m_thread.join();
    }
}

bool BridgeJob::start(QObject* context, bool synchronous, Work work, Failed failed) {
    if (m_running) {
        return false;
    }
    reap();
    m_running = true;
    m_cancelled = false;
    if (!failed) {
        failed = m_failed;
    }
    if (synchronous) {
        Finish finish = runGuarded(work, failed, m_cancelled);
        m_running = false;
        finishGuarded(finish);
        return true;
    }
    QPointer<QObject> guard(context);
    m_thread = std::jthread([this, guard, work = std::move(work), failed = std::move(failed)] {
        Finish finish = runGuarded(work, failed, m_cancelled);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [this, guard, finish = std::move(finish)] {
                if (!guard) {
                    return; // the bridge is gone (its destructor joined this thread)
                }
                reap();
                m_running = false;
                finishGuarded(finish);
            },
            Qt::QueuedConnection);
    });
    return true;
}

// --- ShortsBridge ------------------------------------------------------------------------------------------------------

ShortsBridge* ShortsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ShortsBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setProjectFile(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
        bridge->setTranscripts(qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript"));
        bridge->setAi(qmlEngine->singletonInstance<::AiController*>("Wabcut", "AiController"));
        addFrameProvider(qmlEngine);
    }
    return bridge;
}

ShortsBridge::ShortsBridge(QObject* parent) : QObject(parent) {
    // A job that throws ends with its message.
    m_job.setFailed([self = QPointer<ShortsBridge>(this)](const QString& message) -> BridgeJob::Finish {
        return [self, message] {
            if (self) {
                self->setJob({}, 0.0);
                self->m_message = message;
                emit self->candidatesChanged();
            }
        };
    });
}

ShortsBridge::~ShortsBridge() = default;

void ShortsBridge::setController(ProjectController* controller) {
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::sequencesChanged, this, [this] {
            emit statusChanged();
        });
    }
}

void ShortsBridge::setProjectFile(ProjectLifecycle* file) { m_projectFile = file; }
void ShortsBridge::setTranscripts(TranscriptBridge* transcripts) { m_transcripts = transcripts; }

void ShortsBridge::setAi(::AiController* ai) {
    m_ai = ai;
    if (ai) {
        connect(ai, &::AiController::privacyChanged, this, &ShortsBridge::statusChanged);
        // The re-ranking job uses the platform's chat model: done before the platform goes (A5-P2-19).
        connect(ai, &::AiController::shuttingDown, this, [this] { m_job.stop(); });
    }
}

bool ShortsBridge::cloudAvailable() const {
    if (!m_ai || m_ai->offlineMode() || (m_controller && m_controller->snapshot()->aiNoCloud)) {
        return false;
    }
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    return resolved && !resolved->empty();
}

std::shared_ptr<command::CommandServices> ShortsBridge::services() {
    return m_services ? m_services : aiEditingServices(m_transcripts, projectPath(m_projectFile));
}

void ShortsBridge::setJob(QString phase, double progress) {
    m_phase = std::move(phase);
    m_progress = progress;
    emit jobChanged();
}

bool ShortsBridge::waitForIdle(int milliseconds) {
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

QString ShortsBridge::find(int count, const QString& query, bool rerank) {
    if (!m_controller) {
        return tr("No project");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const SequenceId sequence = m_controller->currentSequenceId();
    if (!base->findSequence(sequence)) {
        return tr("No sequence");
    }
    std::shared_ptr<command::CommandServices> services;
    if (m_services) {
        services = m_services;
    } else {
        std::shared_ptr<ai::IChatModel> chat;
        if (rerank && cloudAvailable()) {
            auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
            if (resolved && !resolved->empty()) {
                if (auto model = m_ai->platform().chat(resolved->front().model->id)) {
                    chat = std::shared_ptr<ai::IChatModel>(std::move(*model));
                }
            }
        }
        services = aiEditingServices(m_transcripts, projectPath(m_projectFile), std::move(chat));
    }
    setFrameContext(base, projectPath(m_projectFile));
    Json params{{"sequence", sequence.value}, {"count", std::clamp(count, 1, 20)}, {"rerank", rerank}};
    if (!query.trimmed().isEmpty()) {
        params["query"] = query.trimmed().toStdString();
    }
    setJob(tr("Finding shorts"), -1.0);
    QPointer<ShortsBridge> self(this);
    const bool started = m_job.start(this, m_synchronous, [self, base, services, params, sequence](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
        EditSession scratch(*base);
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 16, services});
        JobControl control(cancelled, [self](QString text, double progress) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [self, text, progress] { if (self) self->setJob(text, progress); }, Qt::QueuedConnection);
        });
        command::CommandRequest request;
        request.command = "shorts.find";
        request.params = params;
        command::CommandResult result = engine.execute(request, &control);
        return [self, base, sequence, result = std::move(result)] {
            if (!self) {
                return;
            }
            self->setJob({}, 0.0);
            self->m_sequence = sequence;
            self->m_candidates.clear();
            self->m_reranked = false;
            if (!result.ok()) {
                self->m_message = result.error->code == command::ErrorCode::Cancelled ? tr("Cancelled") : qs(result.error->message);
                emit self->candidatesChanged();
                return;
            }
            const Sequence* seq = base->findSequence(sequence);
            for (const Json& s : result.data.value("shorts", Json::array())) {
                QVariantMap card;
                const double start = s.value("start", 0.0);
                const double end = s.value("end", 0.0);
                card.insert(QStringLiteral("rank"), s.value("rank", 0));
                card.insert(QStringLiteral("start"), start);
                card.insert(QStringLiteral("end"), end);
                card.insert(QStringLiteral("duration"), s.value("duration", 0.0));
                card.insert(QStringLiteral("range"), QStringLiteral("%1 – %2").arg(clock(start), clock(end)));
                card.insert(QStringLiteral("lengthText"), tr("%1 s").arg(std::lround(s.value("duration", 0.0))));
                card.insert(QStringLiteral("score"), s.value("score", 0.0));
                card.insert(QStringLiteral("title"), qs(s.value("title", std::string())));
                card.insert(QStringLiteral("hook"), qs(s.value("hook", std::string())));
                card.insert(QStringLiteral("text"), qs(s.value("text", std::string())));
                card.insert(QStringLiteral("cloudReason"), qs(s.value("cloudReason", std::string())));
                QVariantList parts;
                const Json& p = s.value("parts", Json::object());
                const std::pair<const char*, QString> names[] = {{"hook", tr("Hook")},       {"structure", tr("Structure")},
                                                                 {"topic", tr("One topic")}, {"energy", tr("Energy")},
                                                                 {"visual", tr("Visual")},   {"clean", tr("Clean")},
                                                                 {"length", tr("Length")},   {"query", tr("Match")}};
                for (const auto& [key, label] : names) {
                    if (p.contains(key)) {
                        parts.append(QVariantMap{{QStringLiteral("key"), QString::fromLatin1(key)},
                                                 {QStringLiteral("label"), label},
                                                 {QStringLiteral("value"), p[key].get<double>()}});
                    }
                }
                card.insert(QStringLiteral("parts"), parts);
                QStringList reasons;
                for (const Json& r : s.value("reasons", Json::array())) {
                    reasons << qs(r.get<std::string>());
                }
                card.insert(QStringLiteral("reasons"), reasons);
                QStringList thumbs;
                for (int k = 0; seq && k < 6; ++k) {
                    const double t = start + (end - start) * (k + 0.5) / 6.0;
                    if (const auto at = pictureAt(*base, *seq, Ticks{static_cast<std::int64_t>(std::llround(t * static_cast<double>(kTicksPerSecond)))})) {
                        thumbs << frameUrl(at->first, at->second);
                    }
                }
                card.insert(QStringLiteral("thumbs"), thumbs);
                self->m_candidates.append(card);
            }
            self->m_reranked = result.data.value("reranked", false);
            self->m_message = joined(result.warnings);
            if (self->m_candidates.isEmpty() && self->m_message.isEmpty()) {
                self->m_message = tr("No moments of 20-90 seconds stand out.");
            }
            emit self->candidatesChanged();
        };
    });
    return started ? QString() : tr("Busy");
}

void ShortsBridge::cancel() { m_job.cancel(); }

void ShortsBridge::setTitle(int index, const QString& title) {
    if (index < 0 || index >= m_candidates.size()) {
        return;
    }
    QVariantMap card = m_candidates[index].toMap();
    card.insert(QStringLiteral("title"), title);
    m_candidates[index] = card;
    emit candidatesChanged();
}

QString ShortsBridge::create(const QVariantList& indices, const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    Json shorts = Json::array();
    QList<int> chosen;
    for (const QVariant& v : indices) {
        chosen << v.toInt();
    }
    if (chosen.isEmpty()) {
        for (int i = 0; i < m_candidates.size(); ++i) {
            chosen << i;
        }
    }
    for (const int i : chosen) {
        if (i < 0 || i >= m_candidates.size()) {
            continue;
        }
        const QVariantMap card = m_candidates[i].toMap();
        shorts.push_back(Json{{"start", card.value(QStringLiteral("start")).toDouble()},
                              {"end", card.value(QStringLiteral("end")).toDouble()},
                              {"title", card.value(QStringLiteral("title")).toString().toStdString()}});
        if (shorts.size() >= ai::shorts::kMaxShortsPerCall) {
            break;
        }
    }
    if (shorts.empty()) {
        return tr("Nothing to create");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const std::uint64_t revision = m_controller->session().revision();
    Json params{{"sequence", m_sequence.value},
                {"shorts", shorts},
                {"aspect", options.value(QStringLiteral("aspect"), QStringLiteral("9:16")).toString().toStdString()},
                {"mode", options.value(QStringLiteral("mode"), QStringLiteral("speaker")).toString().toStdString()},
                {"cleanup", options.value(QStringLiteral("cleanup"), false).toBool()},
                {"captions", options.value(QStringLiteral("captions"), true).toBool()},
                {"hookTitle", options.value(QStringLiteral("hookTitle"), true).toBool()},
                {"normalize", options.value(QStringLiteral("normalize"), false).toBool()}};
    const auto services = this->services();
    setJob(tr("Making %n short(s)", "", static_cast<int>(shorts.size())), -1.0);
    QPointer<ShortsBridge> self(this);
    const bool started = m_job.start(this, m_synchronous, [self, base, services, params, revision](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
        EditSession scratch(*base);
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 16, services});
        JobControl control(cancelled, [self](QString text, double progress) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [self, text, progress] { if (self) self->setJob(text, progress); }, Qt::QueuedConnection);
        });
        command::CommandRequest request;
        request.command = "shorts.create";
        request.params = params;
        request.dryRun = true;
        command::CommandResult result = engine.execute(request, &control);
        return [self, revision, result = std::move(result)] {
            if (!self) {
                return;
            }
            self->setJob({}, 0.0);
            self->m_pending.reset();
            self->m_pendingNames.clear();
            self->m_pendingIds.clear();
            if (!result.ok() || !result.preview) {
                self->m_message = !result.ok() && result.error->code == command::ErrorCode::Cancelled
                                      ? tr("Cancelled")
                                      : (result.ok() ? tr("Nothing was made") : qs(result.error->message));
                emit self->candidatesChanged();
                emit self->pendingChanged();
                return;
            }
            self->m_pending = result.preview;
            self->m_pendingRevision = revision;
            for (const Json& s : result.data.value("sequences", Json::array())) {
                self->m_pendingNames << qs(s.value("name", std::string()));
                self->m_pendingIds.push_back(SequenceId{s.value("sequence", std::uint64_t{0})});
            }
            self->m_message = joined(result.warnings);
            emit self->candidatesChanged();
            emit self->pendingChanged();
        };
    });
    return started ? QString() : tr("Busy");
}

QString ShortsBridge::apply() {
    if (!m_pending || !m_controller) {
        return tr("Nothing to apply");
    }
    if (m_controller->session().revision() != m_pendingRevision) {
        discard();
        return tr("The project changed since the preview: create them again");
    }
    const auto preview = m_pending;
    const auto ids = m_pendingIds;
    if (auto committed = m_controller->session().commit(tr("Create Shorts").toStdString(), *preview); !committed) {
        return qs(committed.error().message);
    }
    m_created = ids;
    m_pending.reset();
    m_pendingNames.clear();
    m_pendingIds.clear();
    emit pendingChanged();
    if (!ids.empty()) {
        const int index = sequenceIndex(*m_controller->snapshot(), ids.front());
        if (index >= 0) {
            m_controller->setCurrentSequenceIndex(index);
        }
    }
    return {};
}

void ShortsBridge::discard() {
    m_pending.reset();
    m_pendingNames.clear();
    m_pendingIds.clear();
    emit pendingChanged();
}

int ShortsBridge::exportAll(bool hevc) {
    auto* engine = qmlEngine(this);
    auto* exporter = engine ? engine->singletonInstance<ExportController*>("Wabcut", "ExportController") : nullptr;
    if (!exporter) {
        return 0;
    }
    int queued = 0;
    QVariantMap overrides;
    if (hevc) {
        overrides.insert(QStringLiteral("videoCodec"), QStringLiteral("hevc"));
    }
    for (const SequenceId id : m_created) {
        if (!exporter->queueExport(sequenceKey(id), QStringLiteral("vertical-1080x1920"), overrides).isEmpty()) {
            ++queued;
        }
    }
    return queued;
}

// --- ReframeBridge -----------------------------------------------------------------------------------------------------

ReframeBridge* ReframeBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ReframeBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setProjectFile(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
        bridge->setTranscripts(qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript"));
        bridge->setAi(qmlEngine->singletonInstance<::AiController*>("Wabcut", "AiController"));
        addFrameProvider(qmlEngine);
    }
    return bridge;
}

ReframeBridge::ReframeBridge(QObject* parent) : QObject(parent) {
    // A job that throws ends with its message.
    m_job.setFailed([self = QPointer<ReframeBridge>(this)](const QString& message) -> BridgeJob::Finish {
        return [self, message] {
            if (self) {
                self->setJob({}, 0.0);
                self->m_message = message;
                emit self->previewChanged();
            }
        };
    });
}

ReframeBridge::~ReframeBridge() {
    m_download.request_stop();
}

void ReframeBridge::setController(ProjectController* controller) {
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::sequencesChanged, this, &ReframeBridge::statusChanged);
    }
}

void ReframeBridge::setProjectFile(ProjectLifecycle* file) { m_projectFile = file; }
void ReframeBridge::setTranscripts(TranscriptBridge* transcripts) { m_transcripts = transcripts; }
void ReframeBridge::setAi(::AiController* ai) {
    m_ai = ai;
    if (ai) {
        // The detector download borrows the platform's transport: stopped before it goes (A5-P2-19).
        connect(ai, &::AiController::shuttingDown, this, [this] {
            m_job.stop();
            m_download.request_stop();
            if (m_download.joinable()) {
                m_download.join();
            }
        });
    }
}

std::shared_ptr<command::CommandServices> ReframeBridge::services() {
    return m_services ? m_services : aiEditingServices(m_transcripts, projectPath(m_projectFile));
}

void ReframeBridge::setJob(QString phase, double progress) {
    m_phase = std::move(phase);
    m_progress = progress;
    emit jobChanged();
}

bool ReframeBridge::waitForIdle(int milliseconds) {
    QDeadlineTimer deadline(milliseconds);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!m_job.running() && !m_downloading) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            return true;
        }
        QThread::msleep(5);
    }
    return false;
}

bool ReframeBridge::detectorsAvailable() const { return vision::openSubjectDetector({}, {}).detector != nullptr; }

QString ReframeBridge::detectorStatus() const {
    const auto choice = vision::openSubjectDetector({}, {});
    return choice.detector ? tr("Faces and people: YuNet + YOLOX-Tiny, on this computer") : qs(choice.unavailableReason);
}

QString ReframeBridge::detectorLicense() const {
    QStringList cards;
    for (const ai::ModelInfo* m : vision::subjectModelFiles()) {
        if (m) {
            cards << qs(m->licenseCard);
        }
    }
    return cards.join(QStringLiteral("\n\n"));
}

QString ReframeBridge::sourceName() const {
    if (!m_controller) {
        return {};
    }
    const Sequence* seq = m_controller->snapshot()->findSequence(m_controller->currentSequenceId());
    return seq ? qs(seq->name) : QString();
}

QString ReframeBridge::makePreview(const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const SequenceId sequence = m_controller->currentSequenceId();
    const Sequence* seq = base->findSequence(sequence);
    if (!seq) {
        return tr("No sequence");
    }
    std::vector<std::uint64_t> media;
    for (const TrackPtr& t : seq->videoTracks) {
        for (const Clip& c : t->clips) {
            const MediaItem* m = base->findMedia(c.media);
            if (m && !m->isGenerator() && !m->info.video.empty() && std::ranges::find(media, c.media.value) == media.end()) {
                media.push_back(c.media.value);
            }
        }
    }
    const std::uint64_t revision = m_controller->session().revision();
    Json params{{"sequence", sequence.value},
                {"aspect", options.value(QStringLiteral("aspect"), QStringLiteral("9:16")).toString().toStdString()},
                {"mode", options.value(QStringLiteral("mode"), QStringLiteral("follow")).toString().toStdString()},
                {"fit", options.value(QStringLiteral("fit"), QStringLiteral("crop")).toString().toStdString()},
                {"smoothness", std::clamp(options.value(QStringLiteral("smoothness"), 0.5).toDouble(), 0.0, 1.0)},
                {"preview", true}};
    const auto services = this->services();
    setFrameContext(base, projectPath(m_projectFile));
    setJob(tr("Finding subjects"), -1.0);
    QPointer<ReframeBridge> self(this);
    const bool started = m_job.start(this, m_synchronous, [self, base, services, params, media, revision](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
        EditSession scratch(*base);
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 16, services});
        JobControl control(cancelled, [self](QString text, double progress) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [self, text, progress] { if (self) self->setJob(text, progress); }, Qt::QueuedConnection);
        });
        std::vector<std::string> notes;
        for (const std::uint64_t id : media) { // the subjects first (cached per file): a long job, with progress
            command::CommandRequest analyse;
            analyse.command = "analysis.subjects";
            analyse.params = Json{{"media", id}};
            const command::CommandResult r = engine.execute(analyse, &control);
            if (!r.ok()) {
                if (r.error->code == command::ErrorCode::Cancelled) {
                    return [self] {
                        if (self) {
                            self->setJob({}, 0.0);
                            self->m_message = tr("Cancelled");
                            emit self->previewChanged();
                        }
                    };
                }
                notes.push_back(r.error->message);
            } else {
                notes.insert(notes.end(), r.warnings.begin(), r.warnings.end());
            }
        }
        command::CommandRequest request;
        request.command = "reframe.createSequence";
        request.params = params;
        request.dryRun = true;
        command::CommandResult result = engine.execute(request, &control);
        return [self, base, revision, notes, result = std::move(result)] {
            if (!self) {
                return;
            }
            self->setJob({}, 0.0);
            self->m_pending.reset();
            self->m_preview.clear();
            if (!result.ok() || !result.preview) {
                self->m_message = result.ok() ? tr("Nothing to reframe") : qs(result.error->message);
                emit self->previewChanged();
                return;
            }
            self->m_pending = result.preview;
            self->m_pendingRevision = revision;
            self->m_pendingSequence = result.createdIds.empty() ? SequenceId{} : SequenceId{result.createdIds.front()};
            const Json& data = result.data;
            QVariantList clips;
            for (const Json& c : data.value("clips", Json::array())) {
                QVariantMap clip;
                clip.insert(QStringLiteral("name"), qs(c.value("name", std::string())));
                clip.insert(QStringLiteral("guide"), qs(c.value("guide", std::string())));
                clip.insert(QStringLiteral("zoomedOut"), c.value("zoomedOut", false));
                const MediaId media{c.value("media", std::uint64_t{0})};
                QVariantList path;
                QStringList frames;
                for (const Json& p : c.value("path", Json::array())) {
                    path.append(QVariant(QVariantList{p[0].get<double>(), p[1].get<double>(), p[2].get<double>(), p[3].get<double>(), p[4].get<double>()}));
                    frames << frameUrl(media, Ticks{static_cast<std::int64_t>(std::llround(p[0].get<double>() * static_cast<double>(kTicksPerSecond)))});
                }
                QVariantList subjects;
                for (const Json& s : c.value("subjects", Json::array())) {
                    subjects.append(QVariant(QVariantList{s[0].get<double>(), s[1].get<double>(), s[2].get<double>(), s[3].get<double>(), s[4].get<double>()}));
                }
                clip.insert(QStringLiteral("path"), path);
                clip.insert(QStringLiteral("subjects"), subjects);
                clip.insert(QStringLiteral("frames"), frames);
                const MediaItem* item = base->findMedia(media);
                double aspect = 16.0 / 9.0;
                if (item && !item->info.video.empty()) {
                    const auto display = ai::reframe::displayGeometry(item->info.video.front());
                    if (display.height > 0.0) {
                        aspect = display.width / display.height;
                    }
                }
                clip.insert(QStringLiteral("sourceAspect"), aspect);
                clips.append(clip);
            }
            self->m_preview = QVariantMap{{QStringLiteral("name"), qs(data.value("name", std::string()))},
                                          {QStringLiteral("width"), data.value("width", 0)},
                                          {QStringLiteral("height"), data.value("height", 0)},
                                          {QStringLiteral("keyframes"), data.value("keyframes", 0)},
                                          {QStringLiteral("clipsReframed"), data.value("clipsReframed", 0)},
                                          {QStringLiteral("clips"), clips}};
            std::vector<std::string> all = notes;
            all.insert(all.end(), result.warnings.begin(), result.warnings.end());
            self->m_message = joined(all);
            emit self->previewChanged();
        };
    });
    return started ? QString() : tr("Busy");
}

void ReframeBridge::cancel() { m_job.cancel(); }

QString ReframeBridge::apply() {
    if (!m_pending || !m_controller) {
        return tr("Nothing to apply");
    }
    if (m_controller->session().revision() != m_pendingRevision) {
        discard();
        return tr("The project changed since the preview: preview again");
    }
    const auto preview = m_pending;
    const SequenceId made = m_pendingSequence;
    if (auto committed = m_controller->session().commit(tr("Auto Reframe").toStdString(), *preview); !committed) {
        return qs(committed.error().message);
    }
    discard();
    const int index = sequenceIndex(*m_controller->snapshot(), made);
    if (index >= 0) {
        m_controller->setCurrentSequenceIndex(index);
    }
    return {};
}

void ReframeBridge::discard() {
    m_pending.reset();
    m_preview.clear();
    m_message.clear();
    emit previewChanged();
}

void ReframeBridge::downloadDetectors() {
    if (m_downloading) {
        return;
    }
    if (!m_ai) {
        emit notice(tr("The detectors cannot be downloaded here."));
        return;
    }
    if (m_ai->offlineMode()) {
        emit notice(tr("Offline mode is on: turn it off in Preferences > AI to download the detectors."));
        return;
    }
    m_downloading = true;
    m_downloadProgress = 0.0;
    emit statusChanged();
    ai::IHttpTransport* network = &m_ai->transport();
    QPointer<ReframeBridge> self(this);
    m_download = std::jthread([self, network](std::stop_token stop) {
        ai::HttpDownloadTransport transport(*network);
        ai::ModelStore store(ai::ModelStore::defaultRoot());
        const auto files = vision::subjectModelFiles();
        std::uint64_t total = 0;
        for (const ai::ModelInfo* m : files) {
            total += m ? m->size : 0;
        }
        std::uint64_t done = 0;
        QString error;
        for (const ai::ModelInfo* m : files) {
            if (!m) {
                continue;
            }
            auto ensured = store.ensure(
                *m, &transport,
                [self, done, total](const ai::DownloadProgress& p) {
                    const double fraction = total ? static_cast<double>(done + p.received) / static_cast<double>(total) : 0.0;
                    QMetaObject::invokeMethod(
                        QCoreApplication::instance(),
                        [self, fraction] {
                            if (self) {
                                self->m_downloadProgress = fraction;
                                emit self->statusChanged();
                            }
                        },
                        Qt::QueuedConnection);
                },
                stop);
            if (!ensured) {
                error = ensured.error().code == ErrorCode::Cancelled ? QString() : qs(ensured.error().message);
                break;
            }
            done += m->size;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, error] {
                if (!self) {
                    return;
                }
                self->m_downloading = false;
                if (!error.isEmpty()) {
                    emit self->notice(tr("The detectors could not be downloaded: %1").arg(error));
                }
                emit self->statusChanged();
            },
            Qt::QueuedConnection);
    });
}

void ReframeBridge::cancelDownload() { m_download.request_stop(); }

} // namespace wabcut::app
