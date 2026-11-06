#include "AiVideoBridge.h"

#include "AiController.h"
#include "BridgeIds.h"
#include "MediaReadinessBridge.h"
#include "ProjectController.h"

#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/HelperBackend.h>
#include <wabcut/ai/video/MorphFaces.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/ai/video/SourceProcessor.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/TimeInterpolation.h>
#include <wabcut/playback/video/Evaluate.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace wabcut::app {

namespace video = ai::video;

namespace {

/// The processed-picture cache and the source processor the evaluator asks (playback, export, thumbnails): installed when the
/// app starts, so a project with AI effects shows the pictures it has at once, before any panel is opened.
void installAiVideoServices() { video::services(); }

QString toQString(const std::string& s) { return QString::fromStdString(s); }

/// "V1: Interview at 00:00:12:00": the track, the clip and where it starts (clips share names).
std::string whereOf(ProjectController& c, const Sequence& seq, const Clip& clip) {
    QString where;
    if (const auto loc = seq.findClip(clip.id)) {
        if (const auto pos = seq.trackPosition(loc->track->id)) {
            where = QStringLiteral("V") + QString::number(pos->index + 1) + QStringLiteral(": ");
        }
    }
    where += QString::fromStdString(clip.name()) + QObject::tr(" at %1").arg(c.formatTimecode(clip.start()));
    return where.toStdString();
}

QString megabytesText(std::uint64_t bytes) {
    return QObject::tr("%1 MB").arg(static_cast<qulonglong>((bytes + 500'000) / 1'000'000));
}

QString sizeText(int width, int height) { return QStringLiteral("%1 x %2").arg(width).arg(height); }

/// The stage settings in words, for the cache's provenance file: "Restore: scale 2x, High quality, deblur 50 %, ...".
std::string settingsText(const AiChain& chain) {
    std::string out;
    for (const AiStage& s : chain.stages) {
        if (!out.empty()) {
            out += "; ";
        }
        if (s.effect == kAiRestoreEffect) {
            out += "Restore: scale " + std::to_string(s.scale) + "x, " + (s.quality == 1 ? "High quality" : "Fast") + ", deblur " +
                   std::to_string(s.deblur) + " %, light denoise " + std::to_string(s.denoise) + " %, preserve grain " +
                   std::to_string(s.grain) + " %";
        } else if (s.effect == kAiRefocusEffect) {
            out += "Depth maps for AI Refocus: 8-bit distance, 518 lines on the shorter side";
        } else if (s.effect == kAiFaceRetouchEffect) {
            out += "Face maps for AI Face Retouch: 8-bit skin, mouth and eye coverage, at most 1280 pixels on the longer side";
        } else if (s.effect == kAiEyeContactEffect) {
            out += "Eye measurements for AI Eye Contact: the eyelids, irises and head turn of each face, at most 1280 pixels on the longer side";
        } else if (s.effect == kAiInterpolateStage) {
            out += "Frame interpolation: one picture per output frame, at 1/64 of a frame";
        } else {
            out += "Denoise: strength " + std::to_string(s.denoise) + " %, preserve grain " + std::to_string(s.grain) + " %";
        }
        if (!s.isSideProduct()) {
            out += ", tile " + (s.tile > 0 ? std::to_string(s.tile) : std::string("auto"));
        }
    }
    return out;
}

std::string modelsText(const AiChain& chain) {
    std::string out;
    for (const ai::ModelInfo* m : video::modelFilesOf(chain)) {
        if (m != nullptr) {
            out += (out.empty() ? "" : ", ") + m->description.substr(0, m->description.find(" (")) + " (" + m->license + ")";
        }
    }
    return out;
}

#ifdef _WIN32
constexpr bool kExpectGpu = true; // DirectML
#else
constexpr bool kExpectGpu = false;
#endif

} // namespace

Q_COREAPP_STARTUP_FUNCTION(installAiVideoServices)

struct AiVideoBridge::Found {
    SequenceId sequence;
    ClipId clip;
    EffectInstance effect;
    /// Pictures (Restore / Denoise), Depth (AI Refocus: effect is the Refocus), Faces (AI Face Retouch), Interpolation (a clip's AI Frame
    /// Interpolation: section "i<clip>", no effect).
    video::JobKind kind = video::JobKind::Pictures;
};

/// What the model layer says about a clip (video::ClipTarget) and how the interface names it.
struct AiVideoBridge::Target : video::ClipTarget {
    std::string where; ///< "V1: Interview at 00:00:12:00"
};

AiVideoBridge::AiVideoBridge(QObject* parent) : QObject(parent) {}

AiVideoBridge::~AiVideoBridge() {
    m_cancel.request_stop();
    m_downloadStop.request_stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (m_download.joinable()) {
        m_download.join();
    }
}

ProjectController* AiVideoBridge::controller() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController") : nullptr;
}

std::shared_ptr<video::ProcessedCache> AiVideoBridge::cache() const {
    return m_cache ? m_cache : video::services().cache;
}

std::filesystem::path AiVideoBridge::modelRoot() const {
    return m_modelRoot.empty() ? ai::ModelStore::defaultRoot() : m_modelRoot;
}

bool AiVideoBridge::helperAvailable() const {
    if (m_helperOverride) {
        return *m_helperOverride;
    }
    return m_backend != nullptr || video::services().helperAvailable;
}

void AiVideoBridge::setServices(std::shared_ptr<video::ProcessedCache> cache, std::shared_ptr<video::IVideoBackend> backend,
                                std::function<std::unique_ptr<video::IFrameSource>(const std::string&, int)> sources) {
    m_cache = std::move(cache);
    m_backend = std::move(backend);
    m_sources = std::move(sources);
}

void AiVideoBridge::touch() {
    ++m_revision;
    emit changed();
}

// ---- Finding the effect and its clip ---------------------------------------------------------------------------------

std::optional<AiVideoBridge::Found> AiVideoBridge::find(const QString& section) const {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    if (!seq || !snap) {
        return std::nullopt;
    }
    // 0.7 (AI Frame Interpolation): "i<clip id>", the clip's Time Interpolation (it is a clip setting, not an effect).
    if (section.size() > 1 && section.at(0) == QLatin1Char('i')) {
        bool ok = false;
        const quint64 clipId = section.mid(1).toULongLong(&ok);
        const auto loc = ok ? seq->findClip(ClipId{clipId}) : std::nullopt;
        if (!loc || loc->clip().timeInterpolation() != TimeInterpolation::AiInterpolation) {
            return std::nullopt;
        }
        return Found{seq->id, loc->clip().id, EffectInstance{}, video::JobKind::Interpolation};
    }
    const auto id = parseEffectKey(section);
    if (!id) {
        return std::nullopt;
    }
    const auto isAi = [](const EffectInstance& e) {
        return isAiVideoEffect(e.effect) || isAiRefocusEffect(e.effect) || isAiFaceRetouchEffect(e.effect) || isAiEyeContactEffect(e.effect);
    };
    const auto kindOf = [](const EffectInstance& e) {
        return isAiRefocusEffect(e.effect)      ? video::JobKind::Depth
               : isAiFaceRetouchEffect(e.effect) ? video::JobKind::Faces
               : isAiEyeContactEffect(e.effect)  ? video::JobKind::Gaze
                                                 : video::JobKind::Pictures;
    };
    // An effect of a media item's Master Clip Effects is shown with a clip of that media (the selected one first).
    std::optional<Found> master;
    const ClipId selected = c->primaryClip();
    for (const TrackPtr& track : seq->videoTracks) {
        for (const Clip& clip : track->clips) {
            for (const EffectInstance& e : clip.effects()) {
                if (e.id == *id) {
                    return isAi(e) ? std::optional<Found>(Found{seq->id, clip.id, e, kindOf(e)}) : std::nullopt;
                }
            }
            if ((master && master->clip == selected) || clip.isNested() || !clip.media.isValid()) {
                continue;
            }
            if (const MediaItem* holder = masterEffectHolder(*snap, clip.media, *id)) {
                for (const EffectInstance& e : holder->masterEffects) {
                    if (e.id == *id && isAi(e) && (!master || clip.id == selected)) {
                        master = Found{seq->id, clip.id, e, kindOf(e)};
                    }
                }
            }
        }
    }
    return master;
}

std::optional<AiVideoBridge::Target> AiVideoBridge::targetOf(const Found& found) const {
    ProjectController* c = controller();
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    const Sequence* seq = snap ? snap->findSequence(found.sequence) : nullptr;
    const auto loc = seq ? seq->findClip(found.clip) : std::nullopt;
    if (!loc) {
        return std::nullopt;
    }
    const auto pos = seq->trackPosition(loc->track->id);
    const int track = pos ? static_cast<int>(pos->index) : 0;
    auto base = found.kind == video::JobKind::Depth           ? video::depthTarget(*snap, *seq, loc->clip(), track)
                : found.kind == video::JobKind::Faces         ? video::faceTarget(*snap, *seq, loc->clip(), track)
                : found.kind == video::JobKind::Gaze          ? video::gazeTarget(*snap, *seq, loc->clip(), track)
                : found.kind == video::JobKind::Interpolation ? video::interpolationTarget(*snap, *seq, loc->clip(), track)
                                                              : video::clipTarget(*snap, *seq, loc->clip(), track);
    if (!base) {
        return std::nullopt;
    }
    Target t;
    static_cast<video::ClipTarget&>(t) = std::move(*base);
    t.where = whereOf(*c, *seq, loc->clip());
    return t;
}

std::vector<AiVideoBridge::Target> AiVideoBridge::targetsOfSequence(bool onlyIncomplete, const std::optional<TimeRange>& within) const {
    std::vector<Target> out;
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    if (!seq || !snap) {
        return out;
    }
    for (video::ClipTarget& base : video::sequenceTargets(*snap, *seq, within)) {
        Target t;
        static_cast<video::ClipTarget&>(t) = std::move(base);
        if (onlyIncomplete && static_cast<int>(coveredOf(t)) >= t.expected) {
            continue;
        }
        if (const auto loc = seq->findClip(t.clip)) {
            t.where = whereOf(*c, *seq, loc->clip());
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::size_t AiVideoBridge::coveredOf(const Target& target) const {
    if (!target.identity.valid()) {
        return 0;
    }
    return std::min<std::size_t>(video::coveredCount(*cache(), target), static_cast<std::size_t>(std::max(target.expected, 0)));
}

// ---- Header state ------------------------------------------------------------------------------------------------------

QVariantMap AiVideoBridge::infoOf(const Target& t, const QString& section) const {
    QVariantMap info{{QStringLiteral("ai"), true}};
    info.insert(QStringLiteral("label"), toQString(aiChainLabel(t.chain)));
    info.insert(QStringLiteral("where"), toQString(t.where));
    info.insert(QStringLiteral("frames"), t.expected);
    info.insert(QStringLiteral("outputSize"), sizeText(t.width * t.chain.outputScale(), t.height * t.chain.outputScale()));
    info.insert(QStringLiteral("device"), m_lastDevice);
    if (t.chain.empty()) {
        info.insert(QStringLiteral("state"), QStringLiteral("inactive"));
        info.insert(QStringLiteral("message"), tr("With these settings the effect changes nothing."));
        return info;
    }
    const ai::ModelStore store(modelRoot());
    const std::size_t covered = coveredOf(t);
    info.insert(QStringLiteral("processed"), static_cast<int>(covered));
    info.insert(QStringLiteral("percent"), t.expected > 0 ? static_cast<int>(std::lround(100.0 * static_cast<double>(covered) / t.expected)) : 0);
    QStringList names;
    for (const video::Model m : video::modelsOf(t.chain)) {
        names << toQString(std::string(video::describe(m).name));
    }
    info.insert(QStringLiteral("models"), names);
    const std::uint64_t missing = video::missingBytes(t.chain, store);
    info.insert(QStringLiteral("downloadBytes"), static_cast<qulonglong>(missing));
    info.insert(QStringLiteral("downloadText"), missing > 0 ? tr("Download %1").arg(megabytesText(missing)) : QString());
    info.insert(QStringLiteral("card"), missing > 0 ? toQString(video::downloadCard(t.chain, store)) : QString());
    const int left = std::max(0, t.expected - static_cast<int>(covered));
    const double seconds = video::estimateSeconds(t.chain, t.width, t.height, left, cache().get(), kExpectGpu);
    info.insert(QStringLiteral("estimate"), left > 0 ? toQString(video::durationText(seconds)) : QString());
    QString state;
    QString message;
    if (busy() && (m_section == section || m_section == QLatin1String("*"))) {
        state = QStringLiteral("processing");
        message = m_message;
    } else if (!helperAvailable()) {
        state = QStringLiteral("noHelper");
        message = tr("This build of Wabcut has no AI helper, so it cannot make AI pictures; they show as they are.");
    } else if (missing > 0) {
        state = QStringLiteral("needsModel");
        message = tr("Needs %1 more to download; the models run on this computer.").arg(megabytesText(missing));
    } else if (covered >= static_cast<std::size_t>(t.expected)) {
        state = QStringLiteral("ready");
        message = tr("All %n picture(s) are processed.", "", t.expected);
    } else if (covered > 0) {
        state = QStringLiteral("partial");
        message = tr("%1 of %n picture(s) are processed.", "", t.expected).arg(covered);
    } else {
        state = QStringLiteral("notProcessed");
        message = t.kind == video::JobKind::Depth           ? tr("No depth maps yet: AI Refocus leaves the picture as it is until you process the clip.")
                  : t.kind == video::JobKind::Faces         ? tr("No face maps yet: AI Face Retouch leaves the picture as it is until you process the clip.")
                  : t.kind == video::JobKind::Gaze          ? tr("The eyes are not measured yet: AI Eye Contact leaves the picture as it is until you process the clip.")
                      : t.kind == video::JobKind::Interpolation ? tr("Not processed: the clip plays with Optical Flow until you process it.")
                                                                : tr("Not processed: the clip shows its original pictures until you process it.");
    }
    info.insert(QStringLiteral("state"), state);
    info.insert(QStringLiteral("message"), message);
    return info;
}

QVariantMap AiVideoBridge::effectInfo(const QString& section) const {
    const auto found = find(section);
    if (!found) {
        return {{QStringLiteral("ai"), false}};
    }
    QVariantMap info;
    if (const auto target = targetOf(*found)) {
        info = infoOf(*target, section);
    } else {
        info = {{QStringLiteral("ai"), true},
                {QStringLiteral("state"), QStringLiteral("unsupported")},
                {QStringLiteral("message"), tr("AI effects work on video files: not on titles, graphics, still images or nested sequences.")}};
    }
    info.insert(QStringLiteral("kind"), found->kind == video::JobKind::Interpolation ? QStringLiteral("interpolation")
                                        : found->kind == video::JobKind::Depth      ? QStringLiteral("refocus")
                                        : found->kind == video::JobKind::Faces      ? QStringLiteral("faceRetouch")
                                        : found->kind == video::JobKind::Gaze       ? QStringLiteral("eyeContact")
                                        : found->effect.effect == kAiRestoreEffect  ? QStringLiteral("restore")
                                                                                    : QStringLiteral("denoise"));
    return info;
}

QVariantMap AiVideoBridge::overview() const {
    int count = 0;
    int pending = 0;
    for (const Target& t : targetsOfSequence(false, std::nullopt)) {
        ++count;
        pending += static_cast<int>(coveredOf(t)) < t.expected ? 1 : 0;
    }
    return {{QStringLiteral("count"), count}, {QStringLiteral("pending"), pending}};
}

QVariantMap AiVideoBridge::exportNeeds(const QString& range) const {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    std::optional<TimeRange> exported;
    if (seq && range == QLatin1String("inOut")) {
        exported = seq->markedRange();
    } else if (seq && range == QLatin1String("workArea")) {
        exported = seq->workArea;
    }
    const std::vector<Target> targets = targetsOfSequence(true, exported);
    QStringList names;
    int frames = 0;
    double seconds = 0.0;
    const ai::ModelStore store(modelRoot());
    bool canMake = !targets.empty() ? helperAvailable() : true;
    QString why;
    for (const Target& t : targets) {
        const int left = std::max(0, t.expected - static_cast<int>(coveredOf(t)));
        frames += left;
        seconds += video::estimateSeconds(t.chain, t.width, t.height, left, cache().get(), kExpectGpu);
        names << toQString(t.where) + QLatin1String(" (") + toQString(aiChainLabel(t.chain)) + QLatin1Char(')');
        if (!video::modelsInstalled(t.chain, store) && why.isEmpty()) {
            canMake = false;
            why = tr("%1 not downloaded: download it from the effect in Effect Controls, or turn the effect off.")
                      .arg(toQString(video::missingModelsText(t.chain, store)));
        }
    }
    if (!targets.empty() && !helperAvailable()) {
        why = tr("This build of Wabcut has no AI helper.");
    }
    return {{QStringLiteral("count"), static_cast<int>(targets.size())},
            {QStringLiteral("frames"), frames},
            {QStringLiteral("names"), names},
            {QStringLiteral("canMake"), canMake},
            {QStringLiteral("why"), why},
            {QStringLiteral("estimate"), toQString(video::durationText(seconds))}};
}

// ---- Process ------------------------------------------------------------------------------------------------------------

bool AiVideoBridge::process(const QString& section) {
    const auto found = find(section);
    if (busy() || !found) {
        return false;
    }
    auto target = targetOf(*found);
    if (!target || target->chain.empty()) {
        m_message = target ? tr("With these settings the effect changes nothing.")
                           : tr("AI effects work on video files: not on titles, graphics, still images or nested sequences.");
        emit changed();
        return false;
    }
    if (static_cast<int>(coveredOf(*target)) >= target->expected) {
        m_message = tr("Every picture of this clip is already processed.");
        emit changed();
        return false;
    }
    std::vector<Target> targets;
    targets.push_back(std::move(*target));
    return run(std::move(targets), section);
}

bool AiVideoBridge::processAll() {
    if (busy()) {
        return false;
    }
    std::vector<Target> targets = targetsOfSequence(true, std::nullopt);
    if (targets.empty()) {
        m_message = overview().value(QStringLiteral("count")).toInt() > 0 ? tr("Every AI effect in this sequence is already processed.")
                                                                           : tr("There is no AI Restore or AI Denoise in this sequence.");
        emit changed();
        return false;
    }
    return run(std::move(targets), QStringLiteral("*"));
}

void AiVideoBridge::cancel() { m_cancel.request_stop(); }

void AiVideoBridge::discard(const QString& section) {
    const auto found = find(section);
    const auto target = found ? targetOf(*found) : std::nullopt;
    if (busy() || !target || !target->identity.valid()) {
        return;
    }
    cache()->remove(target->identity);
    m_message = tr("The processed pictures were deleted.");
    touch();
    refreshPlayback();
}

bool AiVideoBridge::run(std::vector<Target> targets, const QString& section) {
    if (!helperAvailable()) {
        m_message = tr("This build of Wabcut has no AI helper, so it cannot make AI pictures.");
        emit changed();
        return false;
    }
    const ai::ModelStore store(modelRoot());
    for (const Target& t : targets) {
        if (!video::modelsInstalled(t.chain, store)) {
            m_message = tr("%1 not downloaded: use Download in Effect Controls first.").arg(toQString(video::missingModelsText(t.chain, store)));
            emit changed();
            return false;
        }
    }
    std::shared_ptr<video::IVideoBackend> backend = m_backend ? m_backend : video::services().backend;
    if (!backend) {
        m_message = tr("This build of Wabcut has no AI helper, so it cannot make AI pictures.");
        emit changed();
        return false;
    }
    int total = 0;
    for (const Target& t : targets) {
        total += std::max(1, t.expected);
    }
    m_section = section;
    m_progress = 0.0;
    m_message = tr("Processing in the background");
    m_cancel = std::stop_source();
    emit changed();

    QPointer<AiVideoBridge> self(this);
    const bool synchronous = m_synchronous;
    const auto post = [self, synchronous](std::function<void(AiVideoBridge&)> fn) {
        const auto call = [self, task = std::move(fn)] {
            if (self) {
                task(*self);
            }
        };
        if (synchronous) {
            call();
        } else {
            QMetaObject::invokeMethod(QCoreApplication::instance(), call, Qt::QueuedConnection);
        }
    };
    auto cacheRef = cache();
    auto sources = m_sources;
    const std::stop_token token = m_cancel.get_token();
    const QString app = QCoreApplication::applicationName() + QLatin1Char(' ') + QCoreApplication::applicationVersion();
    auto work = [targets = std::move(targets), cacheRef, backend, sources, token, total, post, app]() {
        QElapsedTimer sinceRefresh;
        sinceRefresh.start();
        int doneBefore = 0;
        QString failure;
        bool cancelled = false;
        int made = 0;
        std::string device;
        for (const Target& t : targets) {
            std::unique_ptr<video::IFrameSource> source =
                sources ? sources(t.path, t.containerStream) : std::make_unique<video::DecoderFrameSource>(t.path, t.containerStream);
            video::Runner runner(*source, *backend, *cacheRef);
            video::Job job;
            job.kind = t.kind;
            job.times = t.times;
            job.framePeriod = t.framePeriod;
            job.identity = t.identity;
            job.chain = t.chain;
            job.range = t.range;
            job.expectedFrames = t.expected;
            job.sourceWidth = t.width;
            job.sourceHeight = t.height;
            job.provenance.chain = aiChainLabel(t.chain);
            job.provenance.settings = settingsText(t.chain);
            job.provenance.modelVersion = t.identity.modelVersion;
            job.provenance.models = modelsText(t.chain);
            job.provenance.created = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
            job.provenance.app = app.toStdString();
            const int base = doneBefore;
            const int span = std::max(1, t.expected);
            auto result = runner.run(
                job,
                [&](const video::Progress& p) {
                    const double fraction =
                        std::clamp(static_cast<double>(base + std::min(p.processed + p.skipped, span)) / std::max(1, total), 0.0, 1.0);
                    const bool refresh = sinceRefresh.elapsed() > 1000;
                    if (refresh) {
                        sinceRefresh.restart();
                    }
                    const QString line = QObject::tr("Processing frame %1 of %2%3%4")
                                             .arg(base + p.processed + p.skipped)
                                             .arg(total)
                                             .arg(p.device.empty() ? QString() : QObject::tr(" on %1").arg(QString::fromStdString(p.device)))
                                             .arg(p.secondsLeft > 0.0 ? QObject::tr(", %1 left").arg(QString::fromStdString(video::durationText(p.secondsLeft)))
                                                                      : QString());
                    post([fraction, line, refresh](AiVideoBridge& b) {
                        if (!b.busy()) {
                            return;
                        }
                        b.m_progress = fraction;
                        b.m_message = line;
                        b.touch();
                        if (refresh) {
                            b.refreshPlayback();
                        }
                    });
                },
                token);
            doneBefore += span;
            if (!result) {
                failure = QString::fromStdString(result.error().message);
                break;
            }
            made += result->processed;
            device = result->device.empty() ? device : result->device;
            if (result->cancelled) {
                cancelled = true;
                break;
            }
        }
        QString message;
        if (!failure.isEmpty()) {
            message = QObject::tr("Processing stopped: %1").arg(failure);
        } else if (cancelled) {
            message = QObject::tr("Cancelled: %n picture(s) were made and are kept.", "", made);
        } else {
            message = made > 0 ? QObject::tr("Done: %n picture(s) processed.", "", made) : QObject::tr("Every picture was already processed.");
        }
        post([message, device](AiVideoBridge& b) {
            b.m_lastDevice = QString::fromStdString(device);
            b.finish(message);
        });
    };
    if (synchronous) {
        work();
    } else {
        m_worker = std::jthread(std::move(work));
    }
    return true;
}

void AiVideoBridge::finish(const QString& message) {
    if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
        m_worker.join();
    }
    m_section.clear();
    m_progress = 0.0;
    m_message = message;
    touch();
    refreshPlayback();
}

void AiVideoBridge::refreshPlayback() {
    if (QQmlEngine* engine = qmlEngine(this)) {
        if (auto* readiness = engine->singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness")) {
            readiness->aiPicturesChanged();
        }
    }
}

// ---- Models -------------------------------------------------------------------------------------------------------------

QVariantMap AiVideoBridge::morphInfo() const {
    ProjectController* c = controller();
    const TransitionId selected = c ? c->selectedTransitionId() : TransitionId{};
    const auto loc = selected ? c->findTransition(selected) : std::nullopt;
    if (!loc || loc->transition->effect != kMorphCutTransition) {
        return {{QStringLiteral("morphCut"), false}};
    }
    bool faceAware = true;
    for (const EffectParam& p : loc->transition->params) {
        if (p.key == "faceAware") {
            faceAware = p.value.constant() > 0.5;
        }
    }
    const ai::ModelInfo* detector = video::faceDetectorModel();
    const bool installed = video::faceDetectorInstalled(modelRoot());
    QVariantMap info{{QStringLiteral("morphCut"), true}, {QStringLiteral("faceAware"), faceAware}, {QStringLiteral("installed"), installed}};
    QString state;
    QString message;
    if (!faceAware) {
        state = QStringLiteral("off");
        message = tr("Face Aware is off: the Morph Cut follows the whole picture.");
    } else if (!helperAvailable()) {
        state = QStringLiteral("noHelper");
        message = tr("This build of Wabcut has no AI helper, so the Morph Cut cannot look for faces.");
    } else if (!installed) {
        state = QStringLiteral("needsModel");
        message = tr("The face detector (0.2 MB) is not on this computer: the Morph Cut follows the whole picture until it is downloaded.");
    } else {
        state = QStringLiteral("ready");
        message = tr("Where both sides of the cut show the same face, the Morph Cut lines it up by its eyes, nose and mouth.");
    }
    info.insert(QStringLiteral("state"), state);
    info.insert(QStringLiteral("message"), message);
    info.insert(QStringLiteral("downloadText"), detector != nullptr && !installed ? tr("Download %1 MB").arg(QString::number(static_cast<double>(detector->size) / 1.0e6, 'f', 1)) : QString());
    info.insert(QStringLiteral("card"), detector != nullptr && !installed ? toQString(detector->licenseCard) : QString());
    return info;
}

bool AiVideoBridge::downloadModels(const QString& section) {
    const bool morph = section == QLatin1String("morph"); // the Morph Cut's face detector
    const auto found = morph ? std::nullopt : find(section);
    const auto target = found ? targetOf(*found) : std::nullopt;
    if (downloading() || (!morph && (!target || target->chain.empty()))) {
        return false;
    }
    const std::filesystem::path root = modelRoot();
    const ai::ModelStore probe(root);
    if (!morph && video::missingBytes(target->chain, probe) == 0) {
        return false;
    }
    if (morph && (video::faceDetectorModel() == nullptr || probe.isInstalled(*video::faceDetectorModel()))) {
        return false;
    }
    auto* ai = qmlEngine(this) ? qmlEngine(this)->singletonInstance<AiController*>("Wabcut", "AiController") : nullptr;
    if (ai == nullptr) {
        m_message = tr("The models cannot be downloaded here.");
        emit changed();
        return false;
    }
    if (ai->offlineMode()) {
        m_message = tr("Offline mode is on: turn it off in Preferences > AI to download the models.");
        emit changed();
        return false;
    }
    std::vector<const ai::ModelInfo*> files;
    std::uint64_t total = 0;
    for (const ai::ModelInfo* m : morph ? std::vector<const ai::ModelInfo*>{video::faceDetectorModel()} : video::modelFilesOf(target->chain)) {
        if (m != nullptr && !probe.isInstalled(*m)) {
            files.push_back(m);
            total += m->size;
        }
    }
    m_downloadSection = section;
    m_downloadProgress = 0.0;
    m_downloadStop = std::stop_source();
    emit changed();
    ai::IHttpTransport* network = &ai->transport();
    // The download borrows the transport: stopped and joined before AiController destroys it.
    disconnect(ai, &AiController::shuttingDown, this, nullptr);
    connect(ai, &AiController::shuttingDown, this, [this] {
        m_downloadStop.request_stop();
        if (m_download.joinable()) {
            m_download.join();
        }
    });
    QPointer<AiVideoBridge> self(this);
    const std::stop_token stop = m_downloadStop.get_token();
    m_download = std::jthread([self, network, files, total, root, stop] {
        ai::HttpDownloadTransport transport(*network);
        ai::ModelStore store(root);
        std::uint64_t done = 0;
        QString error;
        for (const ai::ModelInfo* m : files) {
            auto ensured = store.ensure(
                *m, &transport,
                [self, done, total](const ai::DownloadProgress& p) {
                    const double fraction = total ? static_cast<double>(done + p.received) / static_cast<double>(total) : 0.0;
                    QMetaObject::invokeMethod(
                        QCoreApplication::instance(),
                        [self, fraction] {
                            if (self && self->downloading()) {
                                self->m_downloadProgress = fraction;
                                emit self->changed();
                            }
                        },
                        Qt::QueuedConnection);
                },
                stop);
            if (!ensured) {
                error = ensured.error().code == ErrorCode::Cancelled ? QString() : QString::fromStdString(ensured.error().message);
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
                self->m_downloadSection.clear();
                self->m_downloadProgress = 0.0;
                self->m_message = error.isEmpty() ? tr("The models are on this computer.") : tr("The models could not be downloaded: %1").arg(error);
                self->touch();
            },
            Qt::QueuedConnection);
    });
    return true;
}

void AiVideoBridge::cancelDownload() { m_downloadStop.request_stop(); }

} // namespace wabcut::app
