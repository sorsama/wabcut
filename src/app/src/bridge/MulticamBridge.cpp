#include "MulticamBridge.h"

#include "AiController.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "ShortsBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/TranscribeService.h>
#include <wabcut/ai/audio/AudioJobs.h>
#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/multicam/MediaAngleSignals.h>
#include <wabcut/ai/multicam/MulticamAiCommands.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/audio/edit/AudioSync.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/History.h>

#include <QCoreApplication>
#include <QDir>

#include <cmath>
#include <mutex>

namespace wabcut::app {

using command::Json;
namespace mc = ai::multicam;
namespace wf = ai::workflow;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

std::filesystem::path projectPathOf(QQmlEngine* engine) {
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

QString frameUrl(MediaId media, Ticks t) { return QStringLiteral("image://wabcut-frames/%1/%2").arg(mediaKey(media)).arg(t.value); }

std::optional<MediaId> parseMediaKey(const QString& text) { return detail::parseId<MediaId>(QLatin1Char('m'), text); }

double secondsOf(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }

/// Cancels through `cancelled`; reports "... (NN%)" checkpoints as progress.
class StepControl final : public command::ExecutionControl {
public:
    StepControl(const std::atomic<bool>& cancelled, std::function<void(QString, double)> report)
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

QString presetTitle(const std::string& id) {
    if (id == "calm") {
        return QObject::tr("Podcast calm");
    }
    if (id == "energetic") {
        return QObject::tr("Energetic");
    }
    return QObject::tr("Interview");
}

QString presetDescription(const std::string& id) {
    if (id == "calm") {
        return QObject::tr("Long holds, an opening wide shot, few cuts");
    }
    if (id == "energetic") {
        return QObject::tr("Quick cuts, reaction shots, frequent variety");
    }
    return QObject::tr("Follows the speaker, wide for crosstalk");
}

QVariantMap optionsMap(const mc::SwitchOptions& o) {
    return {{QStringLiteral("id"), qs(o.preset)},
            {QStringLiteral("name"), presetTitle(o.preset)},
            {QStringLiteral("description"), presetDescription(o.preset)},
            {QStringLiteral("minHold"), secondsOf(o.minHold)},
            {QStringLiteral("lead"), secondsOf(o.lead)},
            {QStringLiteral("wideOnOverlap"), o.wideOnOverlap},
            {QStringLiteral("overlapMin"), secondsOf(o.overlapMin)},
            {QStringLiteral("wideOnLaughter"), o.wideOnLaughter},
            {QStringLiteral("silenceWide"), secondsOf(o.silenceWide)},
            {QStringLiteral("varietyAfter"), secondsOf(o.varietyAfter)},
            {QStringLiteral("varietyLength"), secondsOf(o.varietyLength)},
            {QStringLiteral("reactions"), o.reactions},
            {QStringLiteral("reactionStrength"), o.reactionStrength},
            {QStringLiteral("reactionHold"), secondsOf(o.reactionHold)},
            {QStringLiteral("openWide"), o.openWide},
            {QStringLiteral("openLength"), secondsOf(o.openLength)},
            {QStringLiteral("avoidMidWord"), o.avoidMidWord},
            {QStringLiteral("maxSnap"), secondsOf(o.maxSnap)}};
}

/// The angle's picture at multicam source time `t`: its media and source time.
std::optional<std::pair<MediaId, Ticks>> anglePicture(const ProjectState& state, const Sequence& source, std::size_t angle, Ticks t) {
    if (!source.multicam || angle >= source.multicam->angles.size()) {
        return std::nullopt;
    }
    const Track* track = source.findTrack(source.multicam->angles[angle].video);
    if (!track) {
        return std::nullopt;
    }
    const Clip* best = nullptr;
    for (const Clip& c : track->clips) {
        if (c.isNested() || !state.findMedia(c.media)) {
            continue;
        }
        if (c.start() <= t && t < c.end()) {
            best = &c;
            break;
        }
        if (!best) {
            best = &c;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return std::pair{best->media, best->sourceTimeAt(std::clamp(t, best->start(), best->end() - Ticks{1}))};
}

} // namespace

/// The angle signals of a project, kept for the session: microphone levels are decoded once per file and range, so a
/// second preview with other rules is quick.
std::shared_ptr<mc::AngleSignals> appAngleSignals(const std::filesystem::path& projectFile) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::shared_ptr<mc::AngleSignals>> signalsOf;
    std::lock_guard lock(mutex);
    auto& slot = signalsOf[projectFile];
    if (!slot) {
        const auto reframe = appReframeServices(projectFile);
        const auto* subjects = command::findService<ai::reframe::ReframeServices>(reframe.get());
        slot = mc::makeMediaAngleSignals(subjects ? subjects->subjects : nullptr, projectFile);
    }
    return slot;
}

std::shared_ptr<command::CommandServices> multicamServices(QQmlEngine* engine, TranscriptBridge* transcripts,
                                                           const std::filesystem::path& projectFile) {
    // Found first: Enhance Speech on this computer and audio sync (the creator services' audio services have neither).
    auto cached = std::make_shared<ai::edit::CachedTranscriptProvider>(ai::AnalysisCache::defaultRoot(), projectFile);
    auto audioServices = audio::edit::makeMediaAudioServices(cached, std::make_shared<ai::AnalysisCache>(ai::AnalysisCache::defaultRoot()));
    ai::audio::LocalAudioOptions local;
    if (auto* controller = engine ? engine->singletonInstance<::AiController*>("Wabcut", "AiController") : nullptr) {
        local.transport = std::make_shared<ai::HttpDownloadTransport>(controller->transport());
    }
    audioServices->enhancer = ai::audio::makeSpeechEnhancer(local);
    std::vector<std::shared_ptr<command::CommandServices>> parts;
    parts.push_back(std::make_shared<command::MulticamServices>(std::make_shared<audio::edit::MulticamAudioSync>(audioServices->sync)));
    parts.push_back(std::move(audioServices));
    parts.push_back(std::make_shared<mc::SpeakerServices>(appAngleSignals(projectFile)));
    // Transcription on this computer (whisper.cpp; the cloud stays with the Transcript panel and its consent prompt).
    parts.push_back(std::make_shared<ai::edit::TranscribeServices>(std::make_shared<ai::edit::ServiceMediaTranscriber>(
        std::make_shared<ai::TranscribeService>(ai::TranscribeServiceConfig{}), projectFile)));
    parts.push_back(creatorServices(engine, transcripts, projectFile));
    return std::make_shared<command::ServiceBundle>(std::move(parts));
}

// --- AutoSwitchBridge --------------------------------------------------------------------------------------------------

AutoSwitchBridge* AutoSwitchBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new AutoSwitchBridge;
    bridge->setEngine(qmlEngine);
    if (qmlEngine) {
        (void)qmlEngine->singletonInstance<ShortsBridge*>("Wabcut", "Shorts"); // registers image://wabcut-frames
    }
    return bridge;
}

AutoSwitchBridge::AutoSwitchBridge(QObject* parent) : CreatorBase(parent) {}

QVariantList AutoSwitchBridge::presets() const {
    QVariantList out;
    for (const std::string& name : mc::presetNames()) {
        if (const auto o = mc::presetOptions(name)) {
            out.append(optionsMap(*o));
        }
    }
    return out;
}

QVariantMap AutoSwitchBridge::presetValues(const QString& preset) const {
    const auto o = mc::presetOptions(preset.toStdString());
    return o ? optionsMap(*o) : QVariantMap{};
}

QString AutoSwitchBridge::defaultClip() const {
    if (!m_controller) {
        return {};
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const Sequence* seq = base->findSequence(m_controller->currentSequenceId());
    if (!seq) {
        return {};
    }
    const Ticks playhead = m_controller->playhead();
    const Clip* first = nullptr;
    for (const TrackPtr& t : seq->videoTracks) {
        for (const Clip& c : t->clips) {
            const Sequence* nested = c.isNested() ? base->findSequence(c.sequence) : nullptr;
            if (!nested || !nested->multicam) {
                continue;
            }
            if (c.start() <= playhead && playhead < c.end()) {
                return clipKey(c.id);
            }
            if (!first || c.start() < first->start()) {
                first = &c;
            }
        }
    }
    return first ? clipKey(first->id) : QString();
}

bool AutoSwitchBridge::isMulticam(const QString& clip) const {
    const auto id = parseClipKey(clip);
    if (!id || !m_controller) {
        return false;
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const Sequence* seq = base->findSequence(m_controller->currentSequenceId());
    const auto location = seq ? seq->findClip(*id) : std::nullopt;
    const Sequence* nested = location && location->clip().isNested() ? base->findSequence(location->clip().sequence) : nullptr;
    return nested && nested->multicam;
}

QString AutoSwitchBridge::preview(const QString& clipText, const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    const auto clip = parseClipKey(clipText.isEmpty() ? defaultClip() : clipText);
    const ProjectSnapshot base = m_controller->snapshot();
    const SequenceId sequence = m_controller->currentSequenceId();
    const Sequence* seq = base->findSequence(sequence);
    const auto location = clip && seq ? seq->findClip(*clip) : std::nullopt;
    const Sequence* source = location && location->clip().isNested() ? base->findSequence(location->clip().sequence) : nullptr;
    if (!location || !source || !source->multicam) {
        m_message = tr("Choose a multicam clip");
        emit resultChanged();
        return m_message;
    }
    Json params{{"sequence", sequence.value}, {"clip", clip->value}};
    if (const QString preset = options.value(QStringLiteral("preset")).toString(); !preset.isEmpty()) {
        params["preset"] = preset.toStdString();
    }
    for (const char* number : {"minHold", "lead", "overlapMin", "silenceWide", "varietyAfter", "varietyLength", "reactionStrength", "reactionHold",
                               "openLength", "maxSnap"}) {
        if (options.contains(QString::fromLatin1(number))) {
            params[number] = options.value(QString::fromLatin1(number)).toDouble();
        }
    }
    for (const char* flag : {"wideOnOverlap", "wideOnLaughter", "reactions", "openWide", "avoidMidWord", "faces"}) {
        if (options.contains(QString::fromLatin1(flag))) {
            params[flag] = options.value(QString::fromLatin1(flag)).toBool();
        }
    }
    if (options.value(QStringLiteral("target")).toString() == QStringLiteral("both")) {
        params["target"] = "both";
    }
    Json angles = Json::array();
    for (const QVariant& v : options.value(QStringLiteral("angles")).toList()) {
        const QVariantMap a = v.toMap();
        const QString role = a.value(QStringLiteral("role")).toString();
        if (role.isEmpty() || role == QStringLiteral("auto")) {
            continue;
        }
        Json row{{"angle", a.value(QStringLiteral("angle")).toInt()}, {"role", role.toStdString()}};
        if (const QString who = a.value(QStringLiteral("speaker")).toString(); !who.isEmpty()) {
            row["speaker"] = who.toStdString();
        }
        angles.push_back(std::move(row));
    }
    if (!angles.empty()) {
        params["angles"] = std::move(angles);
    }
    const std::uint64_t revision = m_controller->session().revision();
    const Clip c = location->clip();
    const std::filesystem::path file = projectPathOf(m_engine);
    discard();
    m_clip = *clip;
    auto* transcripts = m_engine ? m_engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript") : nullptr;
    auto jobServices = m_services ? m_services : multicamServices(m_engine, transcripts, file);
    QPointer<AutoSwitchBridge> self(this);
    const bool started = runJob(
        tr("Finding who speaks when"), base, "multicam.autoSwitch", params, true,
        [self, base, revision, c, sequence](Run&& run) {
            if (run.error || !run.preview) {
                self->m_message = run.error ? qs(run.error->message) : tr("No cuts");
                emit self->resultChanged();
                return;
            }
            self->m_staged = run.preview;
            self->m_stagedBase = base;
            self->m_stagedRevision = revision;
            QVariantMap result = variantOf(run.data).toMap();
            const Sequence* src = base->findSequence(c.sequence);
            // The angles with a face thumbnail of what each camera sees.
            QVariantList angles;
            QStringList names;
            if (src && src->multicam) {
                for (const MulticamAngle& a : src->multicam->angles) {
                    names << qs(a.name);
                }
            }
            for (const QVariant& v : result.value(QStringLiteral("angles")).toList()) {
                QVariantMap a = v.toMap();
                const auto index = static_cast<std::size_t>(std::max(1, a.value(QStringLiteral("angle")).toInt()) - 1);
                const QVariantMap face = a.value(QStringLiteral("face")).toMap();
                const Ticks at = face.isEmpty() ? Ticks{} : Ticks{face.value(QStringLiteral("ticks")).toLongLong()};
                if (src) {
                    if (const auto pic = anglePicture(*base, *src, index, at)) {
                        a.insert(QStringLiteral("frame"), frameUrl(pic->first, pic->second));
                    }
                }
                angles.append(a);
            }
            result.insert(QStringLiteral("angles"), angles);
            QVariantList cuts;
            for (const QVariant& v : result.value(QStringLiteral("cuts")).toList()) {
                QVariantMap cut = v.toMap();
                const int angle = cut.value(QStringLiteral("angle")).toInt();
                cut.insert(QStringLiteral("angleName"), angle >= 1 && angle <= names.size() ? names[angle - 1] : tr("Angle %1").arg(angle));
                cut.insert(QStringLiteral("offset"), cut.value(QStringLiteral("seconds")).toDouble() - c.start().toSeconds());
                cuts.append(cut);
            }
            result.insert(QStringLiteral("cuts"), cuts);
            result.insert(QStringLiteral("name"), qs(c.name()));
            result.insert(QStringLiteral("start"), c.start().toSeconds());
            result.insert(QStringLiteral("duration"), c.duration().toSeconds());
            result.insert(QStringLiteral("angleNames"), names);
            self->m_result = result;
            self->m_message = joined(run.warnings);
            self->showGhost(sequence); // the cut list over the timeline
            emit self->resultChanged();
        },
        jobServices);
    return started ? QString() : tr("Busy");
}

QString AutoSwitchBridge::apply() {
    if (QString error = commitStaged(tr("Auto Switch")); !error.isEmpty()) {
        return error;
    }
    m_result.clear();
    emit resultChanged();
    return {};
}

void AutoSwitchBridge::discard() {
    m_result.clear();
    CreatorBase::discard();
}

// --- PodcastBridge -----------------------------------------------------------------------------------------------------

PodcastBridge* PodcastBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new PodcastBridge;
    bridge->setEngine(qmlEngine);
    if (qmlEngine) {
        if (auto* file = qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
            // Another project: another run.
            bridge->m_lastProject = file->filePath();
            connect(file, &ProjectLifecycle::documentChanged, bridge, [bridge, file] {
                if (file->filePath() != bridge->m_lastProject) {
                    bridge->m_lastProject = file->filePath();
                    bridge->reset();
                }
            });
        }
    }
    return bridge;
}

PodcastBridge::PodcastBridge(QObject* parent) : CreatorBase(parent) {
    connect(this, &CreatorBase::jobChanged, this, &PodcastBridge::stepsChanged);
}

std::filesystem::path PodcastBridge::projectFile() const {
    return m_projectFileOverride.empty() ? projectPathOf(m_engine) : m_projectFileOverride;
}

std::shared_ptr<command::CommandServices> PodcastBridge::stepServices() const {
    if (m_services) {
        return m_services;
    }
    const std::filesystem::path file = projectFile();
    if (!m_built || m_builtFor != file) {
        auto* transcripts = m_engine ? m_engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript") : nullptr;
        m_built = multicamServices(m_engine, transcripts, file);
        m_builtFor = file;
    }
    return m_built;
}

wf::PodcastPlan PodcastBridge::plan() const {
    if (!m_controller) {
        return {};
    }
    const auto services = stepServices();
    const ProjectSnapshot state = m_controller->snapshot();
    // The sheet reads the steps at every progress tick, and planning reads transcripts and audio fingerprints from
    // disk: the plan is made again only when the project, the options, what earlier steps made, the services or the
    // analysis cache changed - or after two seconds, for what the services find on disk otherwise (a model installed
    // meanwhile) (review wave 5, A5-P2-25).
    std::string key = std::to_string(ai::AnalysisCache::writeCount()) + '\n' + wf::podcastOptionsToJson(m_options).dump() + '\n' +
                      wf::contextToJson(m_context).dump();
    const auto now = std::chrono::steady_clock::now();
    if (state != m_planState || services != m_planServices || key != m_planKey || now - m_plannedAt > std::chrono::seconds(2)) {
        m_plan = wf::planPodcast(*state, m_options, m_context, services.get());
        m_planState = state;
        m_planServices = services;
        m_planKey = std::move(key);
        m_plannedAt = now;
    }
    return m_plan;
}

QVariantList PodcastBridge::recordings() const {
    QVariantList out;
    const ProjectSnapshot s = m_controller ? m_controller->snapshot() : nullptr;
    const auto add = [&](MediaId id, const char* kind) {
        const MediaItem* m = s ? s->findMedia(id) : nullptr;
        out.append(QVariantMap{{QStringLiteral("key"), mediaKey(id)},
                               {QStringLiteral("id"), QVariant::fromValue(id.value)},
                               {QStringLiteral("name"), m ? qs(m->name) : tr("(missing)")},
                               {QStringLiteral("kind"), QString::fromLatin1(kind)},
                               {QStringLiteral("seconds"), m ? m->info.duration.toSeconds() : 0.0}});
    };
    for (const MediaId m : m_options.cameras) {
        add(m, "camera");
    }
    for (const MediaId m : m_options.mics) {
        add(m, "mic");
    }
    return out;
}

QVariantList PodcastBridge::library() const {
    QVariantList out;
    if (!m_controller) {
        return out;
    }
    for (const MediaPtr& m : m_controller->snapshot()->media) {
        if (m->isGenerator() || (!m->info.hasVideo() && m->info.audio.empty())) {
            continue;
        }
        const bool added = std::ranges::find(m_options.cameras, m->id) != m_options.cameras.end() ||
                           std::ranges::find(m_options.mics, m->id) != m_options.mics.end();
        out.append(QVariantMap{{QStringLiteral("key"), mediaKey(m->id)},
                               {QStringLiteral("id"), QVariant::fromValue(m->id.value)},
                               {QStringLiteral("name"), qs(m->name)},
                               {QStringLiteral("hasVideo"), m->info.hasVideo()},
                               {QStringLiteral("hasAudio"), !m->info.audio.empty()},
                               {QStringLiteral("seconds"), m->info.duration.toSeconds()},
                               {QStringLiteral("added"), added}});
    }
    return out;
}

QVariantList PodcastBridge::steps() const {
    QVariantList out;
    if (m_options.cameras.empty() && !m_context.multicam.isValid()) {
        return out;
    }
    const wf::PodcastPlan p = plan();
    for (std::size_t i = 0; i < p.steps.size(); ++i) {
        const wf::StepPlan& s = p.steps[i];
        const auto status = m_status.find(s.kind);
        QString state = status != m_status.end() ? qs(status->second) : (s.enabled ? QStringLiteral("todo") : QStringLiteral("off"));
        if (m_current == s.kind && m_job.running()) {
            state = QStringLiteral("running");
        }
        const auto summary = m_summary.find(s.kind);
        out.append(QVariantMap{{QStringLiteral("step"), QString::fromLatin1(wf::toString(s.kind))},
                               {QStringLiteral("index"), static_cast<int>(i + 1)},
                               {QStringLiteral("title"), qs(s.title)},
                               {QStringLiteral("detail"), qs(s.detail)},
                               {QStringLiteral("command"), qs(s.command)},
                               {QStringLiteral("enabled"), s.enabled},
                               {QStringLiteral("heavy"), s.heavy},
                               {QStringLiteral("reason"), qs(s.reason)},
                               {QStringLiteral("status"), state},
                               {QStringLiteral("summary"), summary != m_summary.end() ? summary->second : QString()}});
    }
    return out;
}

int PodcastBridge::doneCount() const {
    return static_cast<int>(std::ranges::count_if(m_status, [](const auto& s) { return s.second == "done"; }));
}

int PodcastBridge::enabledCount() const {
    if (m_options.cameras.empty() && !m_context.multicam.isValid()) {
        return 0;
    }
    // The run's steps: those on, and those already done (a done step is off in the plan: "already made").
    const wf::PodcastPlan p = plan();
    return static_cast<int>(std::ranges::count_if(p.steps, [&](const wf::StepPlan& s) {
        const auto status = m_status.find(s.kind);
        return s.enabled || (status != m_status.end() && status->second == "done");
    }));
}

bool PodcastBridge::canResume() const {
    const std::filesystem::path file = projectFile();
    if (file.empty() || doneCount() > 0 || m_job.running() || !m_controller) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(wf::journalFileFor(file), ec)) {
        return false;
    }
    const auto journal = wf::loadJournal(wf::journalFileFor(file));
    if (!journal || std::ranges::none_of(journal->steps, [](const auto& s) { return s.second == "done"; })) {
        return false;
    }
    // Unfinished: a step of the journal's plan is neither done nor skipped.
    const auto services = stepServices();
    const wf::PodcastPlan p = wf::planPodcast(*m_controller->snapshot(), wf::podcastOptionsFromJson(journal->options), journal->context, services.get());
    return journal->next(p).has_value();
}

QVariantMap PodcastBridge::options() const { return variantOf(wf::podcastOptionsToJson(m_options)).toMap(); }

QString PodcastBridge::episode() const { return m_context.sequence.isValid() ? sequenceKey(m_context.sequence) : QString(); }

QString PodcastBridge::importFiles(const QList<QUrl>& files) {
    auto* lifecycle = m_engine ? m_engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile") : nullptr;
    if (!lifecycle || !m_controller) {
        return tr("No project");
    }
    lifecycle->importFiles(files);
    const ProjectSnapshot s = m_controller->snapshot();
    int added = 0;
    for (const QUrl& url : files) {
        const QString wanted = QDir::cleanPath(url.toLocalFile());
        for (const MediaPtr& m : s->media) {
            if (QDir::cleanPath(QString::fromStdString(m->locator.path)).compare(wanted, Qt::CaseInsensitive) == 0) {
                addMedia(mediaKey(m->id));
                ++added;
                break;
            }
        }
    }
    return added == files.size() ? QString() : tr("%n file(s) could not be imported", "", static_cast<int>(files.size()) - added);
}

void PodcastBridge::addMedia(const QString& media, const QString& kind) {
    const auto id = parseMediaKey(media);
    const MediaItem* m = id && m_controller ? m_controller->snapshot()->findMedia(*id) : nullptr;
    if (!m) {
        return;
    }
    std::erase(m_options.cameras, *id);
    std::erase(m_options.mics, *id);
    const bool camera = kind.isEmpty() ? m->info.hasVideo() : kind == QStringLiteral("camera");
    (camera ? m_options.cameras : m_options.mics).push_back(*id);
    emit recordingsChanged();
    emit stepsChanged();
}

void PodcastBridge::removeMedia(const QString& media) {
    if (const auto id = parseMediaKey(media)) {
        std::erase(m_options.cameras, *id);
        std::erase(m_options.mics, *id);
        emit recordingsChanged();
        emit stepsChanged();
    }
}

void PodcastBridge::setOptions(const QVariantMap& options) {
    Json j = wf::podcastOptionsToJson(m_options);
    for (auto it = options.begin(); it != options.end(); ++it) {
        const std::string key = it.key().toStdString();
        if (key == "cameras" || key == "mics" || key == "skip" || !j.contains(key)) {
            continue;
        }
        const QVariant& v = it.value();
        if (j[key].is_boolean()) {
            j[key] = v.toBool();
        } else if (j[key].is_number_unsigned() || j[key].is_number_integer()) {
            j[key] = v.toLongLong();
        } else if (j[key].is_number()) {
            j[key] = v.toDouble();
        } else {
            j[key] = v.toString().toStdString();
        }
    }
    m_options = wf::podcastOptionsFromJson(j);
    emit stepsChanged();
}

void PodcastBridge::setStepEnabled(const QString& step, bool enabled) {
    if (const auto k = wf::stepFromString(step.toStdString())) {
        if (*k == wf::StepKind::Shorts) {
            m_options.shorts = enabled;
        }
        if (enabled) {
            m_options.skip.erase(*k);
        } else {
            m_options.skip.insert(*k);
        }
        emit stepsChanged();
    }
}

QString PodcastBridge::previewStep(const QString& step) {
    const auto k = wf::stepFromString(step.toStdString());
    if (!k) {
        return tr("Unknown step");
    }
    return startStep(*k);
}

QString PodcastBridge::startStep(wf::StepKind kind) {
    if (!m_controller) {
        return tr("No project");
    }
    if (m_job.running()) {
        return tr("Busy");
    }
    if (m_staged) {
        discard();
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const std::uint64_t revision = m_controller->session().revision();
    auto services = stepServices();
    const wf::PodcastOptions options = m_options;
    const wf::PodcastContext context = m_context;
    m_current = kind;
    m_status[kind] = "running";
    m_message.clear();
    setPreviewFrameContext(base, projectFile());
    QString title;
    for (const wf::StepPlan& s : plan().steps) {
        if (s.kind == kind) {
            title = qs(s.title);
        }
    }
    setJob(title, -1.0);
    QPointer<PodcastBridge> self(this);
    const bool started = m_job.start(this, m_synchronous, [self, base, revision, services, options, context, kind](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
        StepControl control(cancelled, [self](QString text, double progress) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [self, text, progress] { if (self) self->setJob(text, progress); }, Qt::QueuedConnection);
        });
        wf::PodcastContext ctx = context;
        auto r = wf::runStep(kind, *base, ctx, options, services, &control);
        std::optional<wf::StepResult> result;
        QString error;
        if (r) {
            result = std::move(*r);
        } else {
            error = qs(r.error().message);
        }
        return [self, base, revision, kind, result = std::move(result), error, ctx]() mutable {
            if (!self) {
                return;
            }
            self->m_stagedBase = base;
            self->m_stagedRevision = revision;
            self->finishStep(kind, std::move(result), error, ctx);
        };
    });
    if (!started) {
        m_current.reset();
        m_status.erase(kind);
        return tr("Busy");
    }
    emit stepsChanged();
    return {};
}

void PodcastBridge::finishStep(wf::StepKind kind, std::optional<wf::StepResult> result, QString error, wf::PodcastContext context) {
    setJob({}, 0.0);
    if (!result) {
        m_status[kind] = "failed";
        m_summary[kind] = error;
        m_message = error;
        m_runAll = false;
        m_current.reset();
        record(kind, "failed");
        emit resultChanged();
        emit stepsChanged();
        return;
    }
    m_staged = std::make_shared<const ProjectState>(std::move(result->state));
    m_stagedContext = context;
    m_stagedStep = kind;
    m_status[kind] = "staged";
    // A one-line summary of what the step did.
    const Json& r = result->report;
    QString summary;
    switch (kind) {
    case wf::StepKind::Multicam:
        summary = tr("%n angle(s) in sync", "", static_cast<int>(m_options.cameras.size() + m_options.mics.size()));
        break;
    case wf::StepKind::Sequence:
        summary = tr("“%1”: the picture on V1, %n microphone track(s)", "", r.value("micClips", 0)).arg(qs(m_options.name));
        break;
    case wf::StepKind::AutoSwitch:
        summary = tr("%n cut(s)", "", static_cast<int>(r.value("cuts", Json::array()).size() > 0 ? r["cuts"].size() - 1 : 0));
        break;
    case wf::StepKind::Chapters:
        summary = tr("%n chapter(s)", "", static_cast<int>(r.value("chapters", Json::array()).size()));
        break;
    default:
        summary = r.contains("note") ? qs(r["note"].get<std::string>()) : joined(result->warnings);
        break;
    }
    m_summary[kind] = summary;
    m_message = joined(result->warnings);
    if (context.sequence.isValid() && m_controller && m_controller->currentSequenceId() == context.sequence) {
        showGhost(context.sequence);
    }
    emit resultChanged();
    emit stepsChanged();
    if (m_runAll) {
        if (const QString applied = applyStep(); !applied.isEmpty()) {
            m_runAll = false;
            m_message = applied;
            emit resultChanged();
            emit stepsChanged();
            return;
        }
        continueRun();
    }
}

QString PodcastBridge::applyStep() {
    if (!m_stagedStep || !m_staged) {
        return tr("Nothing to apply");
    }
    const wf::StepKind kind = *m_stagedStep;
    QString title;
    for (const wf::StepPlan& s : plan().steps) {
        if (s.kind == kind) {
            title = qs(s.title);
        }
    }
    const bool laidOut = kind == wf::StepKind::Sequence;
    if (QString error = commitStaged(tr("Podcast workflow: %1").arg(title)); !error.isEmpty()) {
        m_status[kind] = "todo";
        m_stagedStep.reset();
        m_current.reset();
        emit stepsChanged();
        return error;
    }
    m_context = m_stagedContext;
    m_status[kind] = "done";
    m_stagedStep.reset();
    m_current.reset();
    // A checkpoint in the project's version history after every step, for resuming after a crash.
    std::string revision;
    if (const std::filesystem::path file = projectFile(); !file.empty()) {
        if (auto history = ProjectHistory::open(ProjectHistory::folderFor(file))) {
            if (auto rev = history->checkpoint(*m_controller->snapshot(), HistoryKind::Agent, "Podcast workflow: " + title.toStdString())) {
                revision = std::to_string(rev->id);
            }
        }
    }
    record(kind, "done", revision);
    if (laidOut) {
        openEpisode();
    }
    emit stepsChanged();
    return {};
}

QString PodcastBridge::skipStep(const QString& step) {
    const auto k = wf::stepFromString(step.toStdString());
    if (!k) {
        return tr("Unknown step");
    }
    if (m_stagedStep == *k) {
        discard();
    }
    m_status[*k] = "skipped";
    record(*k, "skipped");
    emit stepsChanged();
    return {};
}

void PodcastBridge::record(wf::StepKind kind, const std::string& status, const std::string& revision) {
    m_journal.options = wf::podcastOptionsToJson(m_options);
    m_journal.context = m_context;
    std::erase_if(m_journal.steps, [&](const auto& s) { return s.first == kind; });
    m_journal.steps.emplace_back(kind, status);
    if (!revision.empty()) {
        m_journal.revisions.push_back(revision);
    }
    m_journal.updatedAt.clear();
    if (const std::filesystem::path file = projectFile(); !file.empty()) {
        if (auto saved = wf::saveJournal(wf::journalFileFor(file), m_journal); !saved) {
            m_message = qs(saved.error().message);
        }
    }
}

QString PodcastBridge::runAll() {
    if (m_job.running()) {
        return tr("Busy");
    }
    if (m_options.cameras.empty() && !m_context.multicam.isValid()) {
        return tr("Add the camera recordings first");
    }
    if (m_staged && m_stagedStep) {
        if (QString error = applyStep(); !error.isEmpty()) {
            return error;
        }
    }
    m_runAll = true;
    emit stepsChanged();
    return continueRun();
}

QString PodcastBridge::continueRun() {
    if (!m_runAll) {
        return {};
    }
    for (const wf::StepPlan& s : plan().steps) {
        const auto status = m_status.find(s.kind);
        if (!s.enabled || (status != m_status.end() && (status->second == "done" || status->second == "skipped"))) {
            continue;
        }
        if (QString error = startStep(s.kind); !error.isEmpty()) {
            m_runAll = false;
            m_message = error;
            emit resultChanged();
            emit stepsChanged();
            return error;
        }
        return {};
    }
    m_runAll = false;
    emit resultChanged();
    emit stepsChanged();
    return {};
}

void PodcastBridge::cancel() {
    m_runAll = false;
    CreatorBase::cancel();
    emit stepsChanged();
}

QString PodcastBridge::resume() {
    const std::filesystem::path file = projectFile();
    if (file.empty() || !m_controller) {
        return tr("Save the project first");
    }
    auto journal = wf::loadJournal(wf::journalFileFor(file));
    if (!journal) {
        return qs(journal.error().message);
    }
    m_options = wf::podcastOptionsFromJson(journal->options);
    m_context = journal->context;
    m_journal = *journal;
    m_status.clear();
    m_summary.clear();
    for (const auto& [kind, status] : journal->steps) {
        if (status != "failed") {
            m_status[kind] = status;
        }
    }
    // The open project lost the steps (it was not saved before the crash): back to the last checkpoint, one undo step.
    const ProjectSnapshot now = m_controller->snapshot();
    const bool lost = (m_context.sequence.isValid() && !now->findSequence(m_context.sequence)) ||
                      (m_context.multicam.isValid() && !now->findSequence(m_context.multicam));
    if (lost && !journal->revisions.empty()) {
        auto history = ProjectHistory::open(ProjectHistory::folderFor(file));
        if (!history) {
            return qs(history.error().message);
        }
        auto restored = history->load(std::stoull(journal->revisions.back()));
        if (!restored) {
            return qs(restored.error().message);
        }
        if (auto committed = m_controller->session().commit(tr("Podcast workflow: back to the last step").toStdString(), *restored); !committed) {
            return qs(committed.error().message);
        }
    }
    if (m_context.sequence.isValid()) {
        openEpisode();
    }
    emit recordingsChanged();
    emit stepsChanged();
    return {};
}

void PodcastBridge::reset() {
    if (m_job.running()) {
        m_job.cancel();
    }
    m_runAll = false;
    CreatorBase::discard();
    m_options = {};
    m_context = {};
    m_stagedContext = {};
    m_status.clear();
    m_summary.clear();
    m_current.reset();
    m_stagedStep.reset();
    m_journal = {};
    m_message.clear();
    m_built.reset();
    emit recordingsChanged();
    emit stepsChanged();
}

void PodcastBridge::openEpisode() {
    if (!m_controller || !m_context.sequence.isValid()) {
        return;
    }
    const ProjectSnapshot s = m_controller->snapshot();
    for (std::size_t i = 0; i < s->sequences.size(); ++i) {
        if (s->sequences[i]->id == m_context.sequence) {
            m_controller->setCurrentSequenceIndex(static_cast<int>(i));
            return;
        }
    }
}

void PodcastBridge::discard() {
    if (m_stagedStep) {
        m_status.erase(*m_stagedStep);
        m_stagedStep.reset();
    }
    m_current.reset();
    CreatorBase::discard();
    emit stepsChanged();
}

} // namespace wabcut::app
