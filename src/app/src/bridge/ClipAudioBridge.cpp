#include "ClipAudioBridge.h"

#include "AiController.h"
#include "AppAudioServices.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/audio/AudioModels.h>
#include <wabcut/audio/edit/Loudness.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/playback/audio/AudioTakes.h>

#include <QLocale>
#include <QQmlEngine>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace wabcut::app {

using command::Json;

namespace {

QString text(const std::string& s) { return QString::fromStdString(s); }

const EffectInstance* enhanceEffectOf(const Clip& clip) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == ClipAudioBridge::kEnhanceEffect)
            return &e;
    }
    return nullptr;
}

/// "ready", "stale" (a take is recorded but no longer matches or is missing) or "missing" (none rendered).
QString takeState(const ProjectState& state, const Clip& clip) {
    const auto takes = playback::AudioTakes::of(state);
    if (takes && takes->find(clip.media, clip.stream.index, "enhanceSpeech"))
        return QStringLiteral("ready");
    const MediaItem* media = state.findMedia(clip.media);
    if (media && std::any_of(media->audioTakes.begin(), media->audioTakes.end(), [&](const AudioTake& t) {
            return t.stream == clip.stream.index && t.kind == kEnhanceSpeechTake;
        }))
        return QStringLiteral("stale");
    return QStringLiteral("missing");
}

QString sizeText(std::uint64_t bytes) { return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeSIFormat); }

} // namespace

QString enhanceBadge(const ProjectState& state, const Clip& clip) {
    if (clip.stream.kind != StreamKind::Audio)
        return {};
    const EffectInstance* effect = enhanceEffectOf(clip);
    if (!effect || !effect->enabled)
        return {};
    return takeState(state, clip) == QStringLiteral("ready") ? QStringLiteral("enhanced") : QStringLiteral("missingRender");
}

ClipAudioBridge::ClipAudioBridge(QObject* parent) : QObject(parent) {}

ClipAudioBridge* ClipAudioBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ClipAudioBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        QPointer<AiController> ai = qmlEngine->singletonInstance<AiController*>("Wabcut", "AiController");
        QPointer<TranscriptBridge> transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
        QPointer<ProjectController> controller = bridge->m_controller;
        // Its jobs may download a model through the AI transport: stopped before AiController destroys it (A5-P2-19).
        if (ai)
            QObject::connect(ai, &AiController::shuttingDown, bridge, [bridge] { bridge->m_job.stop(); });
        bridge->setServiceFactory([ai, transcripts, controller] {
            AppAudioServiceOptions options;
            options.ai = ai.data();
            options.transcripts = transcripts.data();
            if (controller)
                options.projectFile = controller->projectFile();
            return appAudioServices(options);
        });
    }
    return bridge;
}

void ClipAudioBridge::setController(ProjectController* controller) {
    m_controller = controller;
}

std::shared_ptr<command::CommandServices> ClipAudioBridge::services() {
    if (m_services || !m_factory)
        return m_services;
    // Takes and stems go next to the project: services built for another project file (the one open before, or this
    // project before Save As) would put them there. Built again, as PodcastBridge::stepServices() does.
    const std::filesystem::path file = m_controller ? m_controller->projectFile() : std::filesystem::path();
    if (!m_built || m_builtFor != file) {
        m_built = m_factory();
        m_builtFor = file;
    }
    return m_built;
}

void ClipAudioBridge::setModelsReadyForTests(std::optional<bool> ready) {
    m_modelsReady = ready;
    emit modelsChanged();
}

QVariantMap ClipAudioBridge::models() const {
    const ai::ModelInfo& speech = ai::audio::speechModel();
    const ai::ModelInfo& stems = ai::audio::stemModel();
    bool speechReady = false;
    bool stemsReady = false;
    if (m_modelsReady) {
        speechReady = stemsReady = *m_modelsReady;
    } else {
        const ai::ModelStore store(ai::ModelStore::defaultRoot());
        speechReady = ai::audio::speechModelInstalled(store);
        stemsReady = store.isInstalled(stems);
    }
    return {{QStringLiteral("speechReady"), speechReady},
            {QStringLiteral("speechLicense"), text(speech.licenseCard)},
            {QStringLiteral("speechSize"), sizeText(speech.size)},
            {QStringLiteral("stemsReady"), stemsReady},
            {QStringLiteral("stemsLicense"), text(stems.licenseCard)},
            {QStringLiteral("stemsSize"), sizeText(stems.size)}};
}

void ClipAudioBridge::refreshModels() { emit modelsChanged(); }

void ClipAudioBridge::setJob(const QString& busy, const QString& phase, double progress) {
    m_busy = busy;
    m_phase = phase;
    m_progress = progress;
    emit jobChanged();
}

void ClipAudioBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

QStringList ClipAudioBridge::selectedClips() const {
    QStringList out;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return out;
    std::vector<std::pair<Ticks, ClipId>> ordered;
    for (const ClipId id : m_controller->selection()) {
        if (const auto loc = seq->findClip(id))
            ordered.emplace_back(loc->clip().start(), id);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [start, id] : ordered)
        out << clipKey(id);
    return out;
}

QVariantMap ClipAudioBridge::clipInfo(const QString& key) const {
    const auto id = parseClipKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return {};
    const ProjectSnapshot snap = m_controller->snapshot();
    const Clip& c = loc->clip();
    const bool audioTrack = loc->track->kind == TrackKind::Audio;
    const MediaItem* media = snap->findMedia(c.media);
    QVariantMap out{{QStringLiteral("audio"), audioTrack || (media && media->info.hasAudio())},
                    {QStringLiteral("video"), !audioTrack},
                    {QStringLiteral("enhanced"), false},
                    {QStringLiteral("bypassed"), false},
                    {QStringLiteral("strength"), 100.0},
                    {QStringLiteral("take"), QString()},
                    {QStringLiteral("badge"), enhanceBadge(*snap, c)},
                    {QStringLiteral("separatedFrom"), QString()}};
    if (const EffectInstance* e = enhanceEffectOf(c)) {
        const ParamValue strength = ProjectController::parameterValue(c, edit::ParamRef::ofEffect(e->id, "strength"));
        out.insert(QStringLiteral("enhanced"), true);
        out.insert(QStringLiteral("effect"), effectKey(e->id));
        out.insert(QStringLiteral("bypassed"), !e->enabled);
        out.insert(QStringLiteral("strength"), std::round(strength.valueAt(c.curveTimeAt(c.start())) * 1000.0) / 10.0);
        out.insert(QStringLiteral("take"), takeState(*snap, c));
    }
    if (media && media->derived) {
        const MediaItem* source = snap->findMedia(media->derived->source);
        out.insert(QStringLiteral("separatedFrom"), source ? text(source->name) : text(media->derived->part));
    }
    return out;
}

QString ClipAudioBridge::start(const QString& kind, const QString& phase, const std::string& command, Json params,
                               const std::string& label, std::function<void(const command::CommandResult&)> done) {
    if (!m_controller)
        return tr("No project");
    if (!m_busy.isEmpty() || m_job.running())
        return tr("An audio job is running");
    setJob(kind, phase, -1);
    setMessage({});
    QPointer<ClipAudioBridge> self(this);
    const bool started = m_job.start(
        this, m_controller, services(), {command, std::move(params), label},
        [self, kind](const QString& text, double progress) {
            if (self)
                self->setJob(kind, text, progress);
        },
        [self, done](const command::CommandResult& result) {
            if (!self)
                return;
            self->setJob({}, {}, 0);
            done(result);
        });
    if (!started) {
        setJob({}, {}, 0);
        return tr("An audio job is running");
    }
    return {};
}

QString ClipAudioBridge::enhanceSpeech(const QStringList& clipKeys, double strength, bool render) {
    Json clips = Json::array();
    for (const QString& key : clipKeys) {
        if (const auto id = parseClipKey(key))
            clips.push_back(id->value);
    }
    if (clips.empty())
        return tr("Select the clips to enhance");
    Json params{{"clips", clips}, {"strength", std::clamp(strength, 0.0, 100.0)}, {"render", render}};
    QPointer<ClipAudioBridge> self(this);
    return start(QStringLiteral("enhance"), tr("Enhancing speech"), "audio.enhanceSpeech", std::move(params), "Enhance Speech",
                 [self](const command::CommandResult& result) {
                     QVariantMap report{{QStringLiteral("ok"), result.ok()}};
                     QVariantList rows;
                     int rendered = 0;
                     int reused = 0;
                     if (result.ok()) {
                         for (const Json& c : result.data.value("clips", Json::array())) {
                             rows.append(QVariantMap{{QStringLiteral("clip"), clipKey(ClipId{c.value("clip", std::uint64_t{0})})},
                                                     {QStringLiteral("take"), text(c.value("take", std::string()))}});
                         }
                         for (const Json& t : result.data.value("takes", Json::array())) {
                             const std::string state = t.value("state", t.value("result", std::string()));
                             if (state == "reused" || t.value("reused", false))
                                 ++reused;
                             else
                                 ++rendered;
                         }
                     }
                     report.insert(QStringLiteral("clips"), rows);
                     report.insert(QStringLiteral("rendered"), rendered);
                     report.insert(QStringLiteral("reused"), reused);
                     self->m_enhanceReport = report;
                     emit self->enhanceReportChanged();
                     emit self->modelsChanged(); // a first run downloads the model
                     self->setMessage(result.ok() ? tr("Speech enhanced on %n clip(s)", nullptr, static_cast<int>(rows.size()))
                                                  : commandErrorText(result));
                 });
}

QString ClipAudioBridge::setStrength(const QString& key, double strength) {
    const auto id = parseClipKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    const EffectInstance* effect = loc ? enhanceEffectOf(loc->clip()) : nullptr;
    if (!effect)
        return tr("Enhance Speech is not on this clip");
    const EffectInstanceId effectId = effect->id;
    const SequenceId sequence = seq->id;
    const ClipId clip = *id;
    const double value = std::clamp(strength, 0.0, 100.0) / 100.0;
    const std::string merge = "enhanceStrength:" + std::to_string(clip.value);
    const bool ok = m_controller->applyEditFn(
        "Enhance Speech Strength",
        [&](const ProjectState& s) {
            return edit::setParameter(s, sequence, clip, edit::ParamRef::ofEffect(effectId, "strength"), ParamValue(value));
        },
        merge);
    return ok ? QString() : m_controller->lastError();
}

void ClipAudioBridge::endStrength() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

QString ClipAudioBridge::setBypassed(const QString& key, bool bypassed) {
    const auto id = parseClipKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    const EffectInstance* effect = loc ? enhanceEffectOf(loc->clip()) : nullptr;
    if (!effect)
        return tr("Enhance Speech is not on this clip");
    if (effect->enabled == !bypassed)
        return {};
    edit::EffectChange change;
    change.enabled = !bypassed;
    const EffectInstanceId effectId = effect->id;
    const SequenceId sequence = seq->id;
    const ClipId clip = *id;
    const bool ok = m_controller->applyEditFn(bypassed ? "Bypass Enhance Speech" : "Enable Enhance Speech", [&](const ProjectState& s) {
        return edit::setEffect(s, sequence, clip, effectId, change);
    });
    return ok ? QString() : m_controller->lastError();
}

QString ClipAudioBridge::separateStems(const QString& key) {
    const auto id = parseClipKey(key);
    if (!id)
        return tr("Choose a clip");
    QPointer<ClipAudioBridge> self(this);
    return start(QStringLiteral("stems"), tr("Separating stems"), "audio.separateStems", Json{{"clip", id->value}},
                 "Separate Stems", [self](const command::CommandResult& result) {
                     emit self->modelsChanged();
                     if (!result.ok()) {
                         self->setMessage(commandErrorText(result));
                         return;
                     }
                     const Json media = result.data.value("media", result.data.value("stems", Json::array()));
                     const int count = media.is_array() && !media.empty() ? static_cast<int>(media.size()) : 4;
                     self->setMessage(tr("Separated into %n stem(s) in the Library", nullptr, count));
                 });
}

// ---- Audio Gain (G, 0.1.2) ------------------------------------------------------------------------------------

namespace {

/// The clip's normalisation gain (dB): 0 without one.
double normalizationGainDb(const Clip& clip) {
    const EffectInstance* gain = audio::edit::normalizationGain(clip.effects());
    if (gain == nullptr)
        return 0.0;
    const ParamValue* value = gain->find("gain");
    return value != nullptr ? value->valueAt(Ticks{}) : 0.0;
}

/// Each clip's normalisation gain at its new level (a Gain at the top of its stack when it has none): one edit.
edit::EditOutcome withGains(const ProjectState& state, SequenceId sequence, const std::vector<std::pair<ClipId, double>>& gains) {
    ProjectState next = state;
    for (const auto& [clip, db] : gains) {
        const SequencePtr seq = next.sequencePtr(sequence);
        const auto loc = seq ? seq->findClip(clip) : std::nullopt;
        if (!loc)
            return std::unexpected(Error{ErrorCode::NotFound, "the clip is gone"});
        const EffectStack& stack = loc->clip().effects();
        std::vector<EffectParam> params{{"gain", ParamValue(std::clamp(db, -96.0, 24.0))}};
        edit::EditOutcome step;
        if (const EffectInstance* gain = audio::edit::normalizationGain(stack)) {
            edit::EffectChange change;
            change.params = std::move(params);
            step = edit::setEffect(next, sequence, clip, gain->id, change);
        } else {
            const std::array<ClipId, 1> one{clip};
            step = edit::addEffect(next, sequence, one, std::string(audio::edit::kGainEffect), std::move(params),
                                   audio::edit::normalizationGainIndex(stack));
        }
        if (!step)
            return step;
        next = std::move(step->state);
    }
    edit::EditResult result;
    result.changes = wabcut::diff(state, next);
    result.state = std::move(next);
    return result;
}

} // namespace

std::vector<ClipId> ClipAudioBridge::gainClips(const QStringList& clipKeys) const {
    std::vector<ClipId> out;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return out;
    std::vector<ClipId> picked;
    for (const QString& key : clipKeys) {
        if (const auto id = parseClipKey(key))
            picked.push_back(*id);
    }
    for (const ClipId id : edit::withLinkedClips(*seq, picked)) {
        const auto loc = seq->findClip(id);
        if (loc && loc->track->kind == TrackKind::Audio && std::ranges::find(out, id) == out.end())
            out.push_back(id);
    }
    return out;
}

double ClipAudioBridge::clipGainDb(const QStringList& clipKeys) const {
    const std::vector<ClipId> clips = gainClips(clipKeys);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (clips.empty() || !seq)
        return 0.0;
    const auto loc = seq->findClip(clips.front());
    return loc ? normalizationGainDb(loc->clip()) : 0.0;
}

QString ClipAudioBridge::audioGain(const QStringList& clipKeys, const QString& mode, double db) {
    if (!m_controller)
        return tr("No project");
    if (!std::isfinite(db))
        return tr("Enter a level in dB");
    const std::vector<ClipId> clips = gainClips(clipKeys);
    if (clips.empty())
        return tr("Select clips with sound");
    const SequenceId sequence = m_controller->currentSequenceId();
    QPointer<ClipAudioBridge> self(this);
    const auto apply = [self, sequence](const std::vector<std::pair<ClipId, double>>& gains) -> QString {
        if (!self || !self->m_controller)
            return {};
        const bool ok = self->m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Audio Gain"),
                                                        [&](const ProjectState& state) { return withGains(state, sequence, gains); });
        return ok ? QString() : tr("The gain could not be applied");
    };

    if (mode == QLatin1String("set") || mode == QLatin1String("adjust")) {
        // No media is read: a parameter edit, now.
        const SequencePtr seq = m_controller->currentSequence();
        std::vector<std::pair<ClipId, double>> gains;
        for (const ClipId clip : clips) {
            const double current = normalizationGainDb(seq->findClip(clip)->clip());
            gains.emplace_back(clip, mode == QLatin1String("set") ? db : audio::edit::nextNormalizationGainDb(current, db));
        }
        const QString error = apply(gains);
        emit audioGainDone(error);
        return error;
    }
    if (mode != QLatin1String("normalizeMax") && mode != QLatin1String("normalizeAll"))
        return tr("Unknown gain mode %1").arg(mode);

    // Normalize: each clip's sample peak as it plays now (with its gain), measured on a worker.
    Json targets = Json::array();
    for (const ClipId clip : clips)
        targets.push_back(Json{{"clip", clip.value}});
    const bool all = mode == QLatin1String("normalizeAll");
    return start(QStringLiteral("gain"), tr("Measuring peaks"), "audio.loudness", Json{{"targets", targets}}, "Audio Gain",
                 [self, clips, all, db, apply](const command::CommandResult& result) {
                     if (!self)
                         return;
                     QString error;
                     if (!result.ok()) {
                         error = commandErrorText(result);
                     } else {
                         std::vector<double> peaks;
                         for (const Json& r : result.data.value("results", Json::array())) {
                             const Json peak = r.value("samplePeakDbfs", Json());
                             peaks.push_back(peak.is_number() ? peak.get<double>() : -std::numeric_limits<double>::infinity());
                         }
                         const SequencePtr seq = self->m_controller ? self->m_controller->currentSequence() : nullptr;
                         const double loudest = peaks.empty() ? -std::numeric_limits<double>::infinity()
                                                              : *std::ranges::max_element(peaks);
                         std::vector<std::pair<ClipId, double>> gains;
                         for (std::size_t i = 0; seq && i < clips.size() && i < peaks.size(); ++i) {
                             const double peak = all ? peaks[i] : loudest;
                             const auto loc = seq->findClip(clips[i]);
                             if (!loc || !std::isfinite(peak))
                                 continue; // silent: left as it is
                             gains.emplace_back(clips[i],
                                                audio::edit::nextNormalizationGainDb(normalizationGainDb(loc->clip()), db - peak));
                         }
                         error = gains.empty() ? tr("Nothing to normalize: the clips are silent") : apply(gains);
                     }
                     if (!error.isEmpty())
                         self->setMessage(error);
                     emit self->audioGainDone(error);
                 });
}

QString ClipAudioBridge::synchronize(const QStringList& clipKeys) {
    Json clips = Json::array();
    for (const QString& key : clipKeys) {
        if (const auto id = parseClipKey(key))
            clips.push_back(id->value);
    }
    if (clips.size() < 2)
        return tr("Select a video clip and the external audio to sync with it");
    QPointer<ClipAudioBridge> self(this);
    const ProjectSnapshot base = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return start(QStringLiteral("sync"), tr("Synchronizing by audio"), "audio.syncClips",
                 Json{{"clips", clips}, {"apply", true}, {"link", true}}, "Synchronize",
                 [self, base, seq](const command::CommandResult& result) {
                     QVariantMap report{{QStringLiteral("ok"), result.ok()}};
                     if (!result.ok()) {
                         report.insert(QStringLiteral("error"), commandErrorText(result));
                         self->m_syncReport = report;
                         emit self->syncReportChanged();
                         self->setMessage(commandErrorText(result));
                         return;
                     }
                     const Json& data = result.data;
                     QVariantList rows;
                     for (const Json& r : data.value("recordings", Json::array())) {
                         const ClipId clip{r.value("clip", std::uint64_t{0})};
                         QString name;
                         if (seq) {
                             if (const auto loc = seq->findClip(clip))
                                 name = text(loc->clip().name());
                         }
                         const bool synced = r.value("synced", false);
                         const double seconds = r.value("offsetSeconds", 0.0);
                         const Rate rate = seq ? seq->frameRate : Rate{30, 1};
                         const Ticks offset = Ticks{r.value("offsetTicks", std::int64_t{0})};
                         QString offsetText;
                         if (synced) {
                             offsetText = (offset.value < 0 ? QStringLiteral("−") : QStringLiteral("+")) +
                                          text(ticksToTimecode(Ticks{std::abs(offset.value)}, rate, false));
                         }
                         QString drift;
                         if (r.value("driftSignificant", false))
                             drift = tr("%1 ppm, speed %2").arg(r.value("driftPpm", 0.0), 0, 'f', 1).arg(r.value("speedProposal", 1.0), 0, 'f', 5);
                         rows.append(QVariantMap{{QStringLiteral("clip"), clipKey(clip)},
                                                 {QStringLiteral("name"), name},
                                                 {QStringLiteral("synced"), synced},
                                                 {QStringLiteral("offset"), offsetText},
                                                 {QStringLiteral("seconds"), seconds},
                                                 {QStringLiteral("confidence"), synced ? QStringLiteral("%1 %").arg(std::lround(r.value("confidence", 0.0) * 100.0)) : QString()},
                                                 {QStringLiteral("drift"), drift},
                                                 {QStringLiteral("problem"), text(r.value("problem", std::string()))}});
                     }
                     QStringList warnings;
                     for (const std::string& w : result.warnings)
                         warnings << text(w);
                     // "reference" is the index of the recording the others were synced to.
                     const auto reference = static_cast<qsizetype>(data.value("reference", std::uint64_t{0}));
                     if (reference < rows.size()) {
                         QVariantMap row = rows[reference].toMap();
                         row.insert(QStringLiteral("reference"), true);
                         rows[reference] = row;
                         report.insert(QStringLiteral("reference"), row.value(QStringLiteral("clip")));
                     }
                     report.insert(QStringLiteral("rows"), rows);
                     report.insert(QStringLiteral("warnings"), warnings);
                     report.insert(QStringLiteral("applied"), true);
                     self->m_syncReport = report;
                     emit self->syncReportChanged();
                     const auto synced = std::count_if(rows.begin(), rows.end(), [](const QVariant& v) { return v.toMap().value(QStringLiteral("synced")).toBool(); });
                     self->setMessage(tr("Synchronized %n clip(s) by audio", nullptr, static_cast<int>(synced)));
                 });
}

void ClipAudioBridge::cancel() { m_job.cancel(); }

} // namespace wabcut::app
