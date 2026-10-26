#include "AudioMixerBridge.h"

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "TimelineView.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/LevelMeter.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/GameAudio.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/playback/audio/TimelineAudioPlayer.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTime>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut::app {

using command::Json;

namespace {

QString tx(const char* text) {
    return QCoreApplication::translate("wabcut::app::AudioMixerBridge", text);
}

QString tx(const char* text, const char* disambiguation, int n) {
    return QCoreApplication::translate("wabcut::app::AudioMixerBridge", text, disambiguation, n);
}

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// The "+ Effect" menu of a strip. M5m: the A.7 effects, and Enhance Speech, which is a clip effect (a rendered take of
// each media item): picked on a track it opens the Enhance Speech dialog for the track's clips (enhanceClipsRequested).
constexpr std::array kAudioEffects{"wabcut.audio.gain",     "wabcut.audio.eq3",      "wabcut.audio.compressor",
                                   "wabcut.audio.limiter",  "wabcut.audio.denoise",  "wabcut.audio.enhanceSpeech",
                                   "wabcut.audio.filter",   "wabcut.audio.dehum",    "wabcut.audio.deess",
                                   "wabcut.audio.expander", "wabcut.audio.reverb",   "wabcut.audio.width"};
constexpr std::string_view kEnhanceSpeech = "wabcut.audio.enhanceSpeech";

/// Short names for the strips' effect slots.
QString shortEffectName(std::string_view id) {
    if (id == "wabcut.audio.gain")
        return tx("Gain");
    if (id == "wabcut.audio.eq3")
        return tx("EQ");
    if (id == "wabcut.audio.compressor")
        return tx("Compressor");
    if (id == "wabcut.audio.limiter")
        return tx("Limiter");
    if (id == "wabcut.audio.denoise")
        return tx("Reduce Noise");
    if (id == "wabcut.audio.enhanceSpeech")
        return tx("Enhance");
    if (id == "wabcut.audio.filter")
        return tx("Filter");
    if (id == "wabcut.audio.dehum")
        return tx("DeHum");
    if (id == "wabcut.audio.deess")
        return tx("De-ess");
    if (id == "wabcut.audio.expander")
        return tx("Expander");
    if (id == "wabcut.audio.reverb")
        return tx("Reverb");
    if (id == "wabcut.audio.width")
        return tx("Width");
    if (const EffectDescriptor* d = EffectRegistry::builtin().find(id))
        return fromView(d->displayName);
    return fromView(id);
}

QString roleName(TrackRole role) {
    switch (role) {
    case TrackRole::Dialogue: return tx("Dialogue");
    case TrackRole::Music: return tx("Music");
    case TrackRole::Effects: return tx("Effects");
    case TrackRole::None: break;
    }
    return tx("No role");
}

QString dbText(double db, int decimals = 1) {
    if (!std::isfinite(db))
        return QStringLiteral("−∞");
    const QString n = QLocale().toString(std::abs(db), 'f', decimals);
    return db < 0 ? QStringLiteral("−") + n : db > 0 ? QStringLiteral("+") + n : n;
}

/// What an effect slot does, in a few characters ("−20 dB · 4:1").
QString effectSummary(const EffectInstance& e) {
    const auto p = [&](const char* key) { return effectParameterAt(e, key, Ticks{}); };
    if (e.effect == "wabcut.audio.gain")
        return dbText(p("gain")) + tx(" dB");
    if (e.effect == "wabcut.audio.compressor")
        return dbText(p("threshold"), 0) + tx(" dB · %1:1").arg(QLocale().toString(p("ratio"), 'g', 3));
    if (e.effect == "wabcut.audio.limiter")
        return dbText(p("ceiling")) + tx(" dBTP");
    if (e.effect == "wabcut.audio.denoise")
        return tx("%1 %").arg(std::lround(p("amount") * 100));
    if (e.effect == "wabcut.audio.eq3")
        return tx("3-band");
    if (e.effect == "wabcut.audio.enhanceSpeech")
        return tx("%1 %").arg(std::lround(p("strength") * 100));
    return {};
}

QVariantList effectsOf(const EffectStack& stack) {
    QVariantList out;
    for (const EffectInstance& e : stack) {
        out.append(QVariantMap{{QStringLiteral("key"), effectKey(e.id)},
                               {QStringLiteral("effect"), QString::fromStdString(e.effect)},
                               {QStringLiteral("name"), shortEffectName(e.effect)},
                               {QStringLiteral("enabled"), e.enabled},
                               {QStringLiteral("summary"), effectSummary(e)}});
    }
    return out;
}

double toDb(float linear) {
    return linear > 0.0f ? std::max(AudioMixerBridge::kMeterFloorDb, 20.0 * std::log10(static_cast<double>(linear)))
                         : -std::numeric_limits<double>::infinity();
}

QVariant lufsValue(const Json& v) {
    return v.is_number() ? QVariant(v.get<double>()) : QVariant();
}

/// Stops a measuring command from another thread.
class CancelControl final : public command::ExecutionControl {
public:
    std::optional<command::CommandError> checkpoint(std::string_view) override {
        if (cancelled.load()) {
            command::CommandError error;
            error.code = command::ErrorCode::Cancelled;
            error.message = "cancelled";
            return error;
        }
        return std::nullopt;
    }
    std::atomic<bool> cancelled{false};
};

/// Every TimelineView showing `controller`'s project, in any window.
std::vector<TimelineView*> timelineViews(const ProjectController* controller) {
    std::vector<TimelineView*> out;
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        auto* quick = qobject_cast<QQuickWindow*>(window);
        if (!quick)
            continue;
        // QML items are QObject children of the window or of its content item.
        QList<TimelineView*> found = quick->findChildren<TimelineView*>();
        if (quick->contentItem())
            found += quick->contentItem()->findChildren<TimelineView*>();
        for (TimelineView* view : found) {
            if (view->controller() == controller && std::ranges::find(out, view) == out.end())
                out.push_back(view);
        }
    }
    return out;
}

} // namespace

/// p02 audio: a strip's effect slots, for the submix strips (AudioMixerRouting.cpp).
QVariantList mixerEffectsOf(const EffectStack& stack) {
    return effectsOf(stack);
}

/// A measuring job on a worker thread: dry runs over a copy of the project.
struct AudioMixerBridge::Job {
    CancelControl control;
    std::jthread thread;
};

AudioMixerBridge::AudioMixerBridge(QObject* parent) : QObject(parent) {
    m_meterTimer.setInterval(33); // ~30 Hz
    m_meterTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_meterTimer, &QTimer::timeout, this, &AudioMixerBridge::pollMeters);
    // Automation passes sample the faders at the meters' rate, whether the mixer is on screen or not (Write).
    m_automationTimer.setInterval(33);
    m_automationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_automationTimer, &QTimer::timeout, this, &AudioMixerBridge::pollAutomation);
}

AudioMixerBridge::~AudioMixerBridge() {
    if (m_job) {
        m_job->control.cancelled = true;
        m_job.reset(); // joins
    }
}

AudioMixerBridge* AudioMixerBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* mixer = new AudioMixerBridge;
    if (qmlEngine) {
        mixer->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        mixer->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
        mixer->setTranscripts(qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript"));
    }
    return mixer;
}

void AudioMixerBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::revisionChanged, this, &AudioMixerBridge::rebuild);
        connect(m_controller, &ProjectController::sequenceChanged, this, [this] {
            // Another sequence: a pass in progress is committed to its own sequence and ends there (A5-P2-22).
            if (m_pass) {
                m_automationTimer.stop();
                commitPass();
            }
            rebuild();
        });
        // Read: the faders follow their automation under the playhead.
        connect(m_controller, &ProjectController::playheadChanged, this, &AudioMixerBridge::updateFaderValues);
        connect(m_controller, &ProjectController::revisionChanged, this, [this] {
            emit loudnessChanged(); // "stale" follows the revision
            // A preview of an older project cannot be applied any more.
            if (m_pending && m_controller && m_controller->session().revision() != m_pending->baseRevision)
                setPending(std::nullopt);
        });
    }
    rebuild();
}

void AudioMixerBridge::setPlayback(PlaybackBridge* playback) {
    if (m_playback == playback)
        return;
    if (m_playback)
        disconnect(m_playback, nullptr, this, nullptr);
    m_playback = playback;
    if (playback)
        connect(playback, &PlaybackBridge::transportChanged, this, [this] { onPlayingChanged(m_playback && m_playback->playing()); });
}

void AudioMixerBridge::setTranscripts(TranscriptBridge* transcripts) {
    m_transcripts = transcripts;
}

void AudioMixerBridge::setServices(std::shared_ptr<command::CommandServices> services) {
    m_services = std::move(services);
}

void AudioMixerBridge::setMeterSource(MeterSource source) {
    m_meterSource = std::move(source);
}

std::shared_ptr<command::CommandServices> AudioMixerBridge::services() {
    if (!m_services) {
        std::shared_ptr<ai::edit::TranscriptServices> transcripts = m_transcripts ? m_transcripts->services() : nullptr;
        auto audio = audio::edit::makeMediaAudioServices(transcripts ? transcripts->transcripts : nullptr,
                                                         std::make_shared<ai::AnalysisCache>(ai::AnalysisCache::defaultRoot()));
        if (transcripts)
            m_services = std::make_shared<command::ServiceBundle>(
                std::vector<std::shared_ptr<command::CommandServices>>{transcripts, audio});
        else
            m_services = audio;
    }
    return m_services;
}

// ---- Strips ---------------------------------------------------------------------------------------------

void AudioMixerBridge::rebuild() {
    updateDuckTracks(); // 0.4: before the early return below (a track rename changes no strip key)
    QVariantList strips;
    QVariantMap master;
    m_trackOrder.clear();
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (seq) {
        std::map<QString, QVariantMap> rows; // shortName and colour by track key
        for (const QVariant& v : m_controller->audioTracks())
            rows[v.toMap().value(QStringLiteral("trackId")).toString()] = v.toMap();
        for (std::size_t i = 0; i < seq->audioTracks.size(); ++i) {
            const Track& t = *seq->audioTracks[i];
            m_trackOrder.push_back(t.id);
            const QString key = trackKey(t.id);
            const QVariantMap row = rows[key];
            QVariantMap strip{
                {QStringLiteral("key"), key},
                {QStringLiteral("name"), QString::fromStdString(t.name)},
                {QStringLiteral("shortName"), row.value(QStringLiteral("shortName"), QStringLiteral("A%1").arg(i + 1))},
                {QStringLiteral("colorIndex"), row.value(QStringLiteral("colorIndex"), 0)},
                {QStringLiteral("role"), fromView(toString(t.role))},
                {QStringLiteral("roleName"), roleName(t.role)},
                {QStringLiteral("muted"), t.muted},
                {QStringLiteral("solo"), t.solo},
                {QStringLiteral("recordArmed"), t.recordArmed},
                {QStringLiteral("gainDb"), t.volume.valueAt(m_controller->playhead())},
                {QStringLiteral("pan"), t.pan.valueAt(m_controller->playhead())},
                {QStringLiteral("automated"), t.volume.isCurve() || t.pan.isCurve()},
                {QStringLiteral("automation"), automationMode(key)},
                {QStringLiteral("meter"), static_cast<int>(i)},
                {QStringLiteral("effects"), effectsOf(t.effects)},
            };
            addRouting(strip, t, *seq); // p02 audio: output and sends
            strips.append(strip);
        }
        const bool limiter = !seq->audioEffects.empty() && seq->audioEffects.back().effect == "wabcut.audio.limiter";
        master = {{QStringLiteral("effects"), effectsOf(seq->audioEffects)},
                  {QStringLiteral("hasLimiter"), limiter},
                  {QStringLiteral("gainDb"), seq->masterVolume.valueAt(m_controller->playhead())},
                  {QStringLiteral("automated"), seq->masterVolume.isCurve()},
                  {QStringLiteral("automation"), automationMode(QStringLiteral("master"))}};
    }
    // p02 audio: the submix strips and the output choices.
    QVariantList submixes;
    QVariantList outputs;
    m_submixOrder.clear();
    if (seq) {
        for (const TrackPtr& t : seq->submixTracks)
            m_submixOrder.push_back(t->id);
        submixes = buildSubmixes(*seq);
        outputs = buildOutputs(*seq);
    }
    updateFaderValues();
    if (strips == m_strips && master == m_master && submixes == m_submixes && outputs == m_outputs)
        return;
    m_strips = strips;
    m_master = master;
    m_submixes = submixes;
    m_outputs = outputs;
    emit stripsChanged();
}

QVariantList AudioMixerBridge::roles() const {
    QVariantList out;
    for (TrackRole role : {TrackRole::None, TrackRole::Dialogue, TrackRole::Music, TrackRole::Effects})
        out.append(QVariantMap{{QStringLiteral("id"), fromView(toString(role))}, {QStringLiteral("name"), roleName(role)}});
    return out;
}

QVariantList AudioMixerBridge::effectTypes() const {
    QVariantList out;
    for (const char* id : kAudioEffects) {
        const EffectDescriptor* d = EffectRegistry::builtin().find(id);
        out.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(id)},
                               {QStringLiteral("name"), d ? fromView(d->displayName) : shortEffectName(id)},
                               {QStringLiteral("shortName"), shortEffectName(id)}});
    }
    // p02 audio: then the rest of the audio effects (Premiere's catalog), by category: "Delay and Echo › Analog Delay".
    std::vector<const EffectDescriptor*> rest;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (d.kind != EffectKind::Audio || d.intrinsic
            || std::ranges::any_of(kAudioEffects, [&](const char* id) { return d.id == id; }))
            continue;
        rest.push_back(&d);
    }
    std::ranges::stable_sort(rest, [](const EffectDescriptor* a, const EffectDescriptor* b) {
        return std::pair{a->category, a->displayName} < std::pair{b->category, b->displayName};
    });
    for (const EffectDescriptor* d : rest) {
        out.append(QVariantMap{{QStringLiteral("id"), fromView(d->id)},
                               {QStringLiteral("name"), QStringLiteral("%1 › %2").arg(fromView(d->category), fromView(d->displayName))},
                               {QStringLiteral("category"), fromView(d->category)},
                               {QStringLiteral("shortName"), shortEffectName(d->id)}});
    }
    return out;
}

QString AudioMixerBridge::faderNote() const {
    return {};
}

QVariantList AudioMixerBridge::automationModes() const {
    QVariantList out;
    for (const auto& [mode, name] : {std::pair{AutomationMode::Read, tx("Read")}, std::pair{AutomationMode::Touch, tx("Touch")},
                                     std::pair{AutomationMode::Latch, tx("Latch")}, std::pair{AutomationMode::Write, tx("Write")}})
        out.append(QVariantMap{{QStringLiteral("id"), automationModeName(mode)}, {QStringLiteral("name"), name}});
    return out;
}

std::optional<Json> AudioMixerBridge::targetParam(const QString& key) const {
    if (key == QLatin1String("master"))
        return Json("master");
    if (const auto id = parseTrackKey(key))
        return Json(id->value);
    return std::nullopt;
}

command::CommandResult AudioMixerBridge::run(const std::string& name, Json params) {
    command::CommandResult result;
    if (!m_controller) {
        result.error = command::CommandError{};
        result.error->message = "no project";
        return result;
    }
    command::EnginePolicy policy;
    policy.services = services();
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), policy);
    command::CommandRequest request;
    request.command = name;
    params["sequence"] = m_controller->currentSequenceId().value;
    request.params = std::move(params);
    return engine.execute(request);
}

command::CommandResult AudioMixerBridge::runBatch(std::vector<command::CommandRequest> requests, const std::string& label) {
    command::CommandResult result;
    if (!m_controller) {
        result.error = command::CommandError{};
        result.error->message = "no project";
        return result;
    }
    command::EnginePolicy policy;
    policy.services = services();
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), policy);
    command::CommandBatch batch;
    for (command::CommandRequest& r : requests)
        r.params["sequence"] = m_controller->currentSequenceId().value;
    batch.commands = std::move(requests);
    batch.label = label;
    return engine.execute(batch);
}

namespace {
QString errorOf(const command::CommandResult& result) {
    return result.ok() ? QString() : QString::fromStdString(result.error->message);
}
} // namespace

QString AudioMixerBridge::setRole(const QString& track, const QString& role) {
    const auto target = targetParam(track);
    if (!target || target->is_string())
        return tx("Choose an audio track");
    return errorOf(run("track.setRole", Json{{"track", *target}, {"role", role.toStdString()}}));
}

void AudioMixerBridge::setMuted(const QString& track, bool muted) {
    if (m_controller)
        m_controller->setTrackFlag(track, QStringLiteral("muted"), muted);
}

void AudioMixerBridge::setSolo(const QString& track, bool solo) {
    if (m_controller)
        m_controller->setTrackFlag(track, QStringLiteral("solo"), solo);
}

// ---- Faders and automation (M5l) ---------------------------------------------------------------------------------

const ParamValue* AudioMixerBridge::modelParam(const QString& target, const QString& param) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return nullptr;
    if (target == QLatin1String("master"))
        return param == QLatin1String("volume") ? &seq->masterVolume : nullptr;
    const auto id = parseTrackKey(target);
    if (const Track* t = id ? seq->findAudioBus(*id) : nullptr) // p02 audio: a submix's fader too
        return param == QLatin1String("pan") ? &t->pan : &t->volume;
    return nullptr;
}

std::optional<double> AudioMixerBridge::modelValue(const QString& target, const QString& param) const {
    const ParamValue* value = modelParam(target, param);
    if (!value)
        return std::nullopt;
    return value->valueAt(m_controller->playhead());
}

namespace {

/// The parameter of `target` ("master" or a track key) in `seq`, for an edit on that state.
const ParamValue* paramIn(const Sequence& seq, const QString& target, const QString& param) {
    if (target == QLatin1String("master"))
        return param == QLatin1String("volume") ? &seq.masterVolume : nullptr;
    const auto id = parseTrackKey(target);
    if (const Track* t = id ? seq.findAudioBus(*id) : nullptr) // p02 audio: a submix's fader too
        return param == QLatin1String("pan") ? &t->pan : &t->volume;
    return nullptr;
}

edit::EffectTarget effectTargetOf(const QString& target) {
    if (target == QLatin1String("master"))
        return edit::EffectTarget::masterAudio();
    return edit::EffectTarget::ofTrack(parseTrackKey(target).value_or(TrackId{}));
}

edit::ParamRef paramRefOf(const QString& param) {
    return edit::ParamRef::of(param == QLatin1String("pan") ? ParamId::Pan : ParamId::VolumeDb);
}

double clampParam(const QString& param, double value) {
    return param == QLatin1String("pan") ? std::clamp(value, -1.0, 1.0) : std::clamp(value, -144.0, 24.0);
}

} // namespace

QString AudioMixerBridge::setFader(const QString& target, const QString& param, double value, bool gesture) {
    if (!m_controller)
        return tx("No project");
    const ParamValue* current = modelParam(target, param);
    if (!current)
        return target == QLatin1String("master") ? tx("The master has no pan") : tx("Choose an audio track or the master");
    value = clampParam(param, value);
    // Playing in Touch / Latch / Write: the pass records it; it is written when playback stops.
    if (m_pass && automationMode(target) != QLatin1String("read")) {
        m_live[{target, param}] = value;
        updateFaderValues();
        return {};
    }
    const SequenceId sequence = m_controller->currentSequenceId();
    const Ticks at = m_controller->playhead();
    const char* label = target == QLatin1String("master") ? QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Master Volume")
                        : param == QLatin1String("pan")   ? QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Track Pan")
                                                          : QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Track Volume");
    const std::string merge = gesture ? "fader:" + target.toStdString() + ":" + param.toStdString() : std::string();
    const bool ok = m_controller->applyEditFn(
        label,
        [&](const ProjectState& state) -> edit::EditOutcome {
            const Sequence* seq = state.findSequence(sequence);
            const ParamValue* now = seq ? paramIn(*seq, target, param) : nullptr;
            if (!now)
                return std::unexpected(Error(ErrorCode::NotFound, "the track is gone"));
            ParamValue next(value);
            if (const Curve* curve = now->curve()) {
                // Automated: the key at the playhead (Premiere's fader on an automated track, paused).
                Curve c = *curve;
                if (const auto index = c.keyAt(at))
                    c.keys[*index].value = value;
                else
                    c.keys.insert(c.keys.begin() + static_cast<std::ptrdiff_t>(c.insertionIndex(at)), Keyframe{at, value});
                next = ParamValue(std::move(c));
            }
            return edit::setParameter(state, sequence, effectTargetOf(target), paramRefOf(param), std::move(next));
        },
        merge);
    return ok ? QString() : m_controller->lastError();
}

QString AudioMixerBridge::setGainDb(const QString& target, double gainDb) {
    return setFader(target, QStringLiteral("volume"), gainDb, true);
}

QString AudioMixerBridge::setPan(const QString& track, double pan) {
    return setFader(track, QStringLiteral("pan"), pan, true);
}

void AudioMixerBridge::endFaderGesture() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

QString AudioMixerBridge::resetGain(const QString& target) {
    endFaderGesture();
    const QString error = setFader(target, QStringLiteral("volume"), 0.0, false);
    endFaderGesture();
    return error;
}

QString AudioMixerBridge::resetPan(const QString& track) {
    endFaderGesture();
    const QString error = setFader(track, QStringLiteral("pan"), 0.0, false);
    endFaderGesture();
    return error;
}

QString AudioMixerBridge::automationMode(const QString& target) const {
    const auto it = m_modes.find(target);
    return automationModeName(it == m_modes.end() ? AutomationMode::Read : it->second);
}

void AudioMixerBridge::setAutomationMode(const QString& target, const QString& mode) {
    const auto parsed = automationModeFromName(mode);
    if (!parsed || (target != QLatin1String("master") && !parseTrackKey(target)))
        return;
    if (automationMode(target) == mode)
        return;
    m_modes[target] = *parsed;
    // A writing mode chosen while playing starts the pass now.
    if (m_playing && *parsed != AutomationMode::Read && !m_pass)
        startPass();
    rebuild();
}

void AudioMixerBridge::touch(const QString& target, const QString& param, bool held) {
    m_held[{target, param}] = held;
    if (!m_pass)
        return;
    const QString mode = automationMode(target);
    if (held) {
        if (!m_live.contains({target, param}))
            if (const auto v = modelValue(target, param))
                m_live[{target, param}] = *v;
    } else if (mode == QLatin1String("touch")) {
        // Touch: back to the automation that was there.
        m_live.erase({target, param});
    }
    pollAutomation();
}

void AudioMixerBridge::startPass() {
    m_pass.emplace();
    m_pass->sequence = m_controller ? m_controller->currentSequenceId() : SequenceId{};
    m_automationTimer.start();
}

void AudioMixerBridge::onPlayingChanged(bool playing) {
    // The source view's playback is not the program's: it writes no automation (A5-P2-22).
    if (playing && m_playback && m_playback->sourceMode())
        playing = false;
    if (playing == m_playing)
        return;
    m_playing = playing;
    if (playing) {
        const bool writing = std::any_of(m_modes.begin(), m_modes.end(), [](const auto& m) { return m.second != AutomationMode::Read; });
        if (writing) {
            startPass();
            m_live.clear();
            // Write records the faders where they stand, touched or not.
            for (const auto& [target, mode] : m_modes) {
                if (mode != AutomationMode::Write)
                    continue;
                for (const QString param : {QStringLiteral("volume"), QStringLiteral("pan")}) {
                    if (const auto v = modelValue(target, param))
                        m_live[{target, param}] = *v;
                }
            }
            pollAutomation();
        }
    } else {
        m_automationTimer.stop();
        if (m_pass) {
            pollAutomation(); // the last sample, at the stop
            commitPass();
        }
    }
    updateFaderValues();
}

void AudioMixerBridge::pollAutomation() {
    // Samples only while the pass's own sequence is shown (its strips are the ones on screen).
    if (!m_pass || !m_controller || m_controller->currentSequenceId() != m_pass->sequence)
        return;
    const Ticks t = m_controller->playhead();
    for (const auto& [target, mode] : m_modes) {
        if (mode == AutomationMode::Read)
            continue;
        for (const QString param : {QStringLiteral("volume"), QStringLiteral("pan")}) {
            if (!modelParam(target, param))
                continue;
            Pass::Lane& lane = m_pass->lanes[{target, param}];
            const bool held = m_held.contains({target, param}) && m_held.at({target, param});
            if (mode == AutomationMode::Latch && held)
                lane.latched = true;
            const bool writing = mode == AutomationMode::Write || (mode == AutomationMode::Touch && held) ||
                                 (mode == AutomationMode::Latch && lane.latched);
            if (!writing) {
                lane.open = false;
                continue;
            }
            if (!m_live.contains({target, param})) {
                if (const auto v = modelValue(target, param))
                    m_live[{target, param}] = *v;
            }
            const double value = m_live[{target, param}];
            // A jump back (looping) starts a new stretch.
            if (lane.open && !lane.segments.empty() && t < lane.segments.back().end)
                lane.open = false;
            if (!lane.open) {
                lane.segments.push_back(AutomationSegment{t, t, {}});
                lane.open = true;
            }
            AutomationSegment& segment = lane.segments.back();
            if (segment.samples.empty() || t > segment.samples.back().first)
                segment.samples.emplace_back(t, value);
            segment.end = std::max(segment.end, t);
        }
    }
    updateFaderValues();
}

void AudioMixerBridge::commitPass() {
    if (!m_pass || !m_controller)
        return;
    Pass pass = std::move(*m_pass);
    m_pass.reset();
    m_live.clear();
    std::vector<std::tuple<QString, QString, std::vector<AutomationSegment>>> written;
    for (auto& [key, lane] : pass.lanes) {
        std::erase_if(lane.segments, [](const AutomationSegment& seg) { return seg.samples.empty(); });
        if (!lane.segments.empty())
            written.emplace_back(key.first, key.second, std::move(lane.segments));
    }
    if (written.empty())
        return;
    // The sequence the pass started on, even when another is current now (A5-P2-22).
    const SequenceId sequence = pass.sequence;
    // Every lane of the pass in one edit: one undo step.
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Mixer Automation"), [&](const ProjectState& state) -> edit::EditOutcome {
            ProjectState next = state;
            for (const auto& [target, param, segments] : written) {
                const Sequence* seq = next.findSequence(sequence);
                const ParamValue* now = seq ? paramIn(*seq, target, param) : nullptr;
                if (!now)
                    continue;
                ParamValue value = applyAutomationPass(*now, segments, param == QLatin1String("pan") ? 0.005 : 0.05);
                auto edited = edit::setParameter(next, sequence, effectTargetOf(target), paramRefOf(param), std::move(value));
                if (!edited)
                    return std::unexpected(edited.error());
                next = std::move(edited->state);
            }
            edit::EditResult result;
            result.changes = diff(state, next);
            result.state = std::move(next);
            return result;
        });
    if (!ok)
        setMessage(m_controller->lastError());
}

void AudioMixerBridge::updateFaderValues() {
    QVariantMap values;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (seq) {
        const auto row = [&](const QString& key) {
            const auto live = [&](const QString& param) -> std::optional<double> {
                if (!m_pass || automationMode(key) == QLatin1String("read"))
                    return std::nullopt;
                const auto it = m_live.find({key, param});
                return it == m_live.end() ? std::nullopt : std::optional<double>(it->second);
            };
            const auto lane = [&](const QString& param) {
                if (!m_pass)
                    return false;
                const auto it = m_pass->lanes.find({key, param});
                return it != m_pass->lanes.end() && it->second.open;
            };
            const ParamValue* volume = modelParam(key, QStringLiteral("volume"));
            const ParamValue* pan = modelParam(key, QStringLiteral("pan"));
            const Ticks t = m_controller->playhead();
            return QVariantMap{{QStringLiteral("gainDb"), live(QStringLiteral("volume")).value_or(volume ? volume->valueAt(t) : 0.0)},
                               {QStringLiteral("pan"), live(QStringLiteral("pan")).value_or(pan ? pan->valueAt(t) : 0.0)},
                               {QStringLiteral("automated"), volume && volume->isCurve()},
                               {QStringLiteral("panAutomated"), pan && pan->isCurve()},
                               {QStringLiteral("writing"), lane(QStringLiteral("volume")) || lane(QStringLiteral("pan"))}};
        };
        for (const TrackPtr& t : seq->audioTracks)
            values.insert(trackKey(t->id), row(trackKey(t->id)));
        for (const TrackPtr& t : seq->submixTracks)
            values.insert(trackKey(t->id), row(trackKey(t->id)));
        values.insert(QStringLiteral("master"), row(QStringLiteral("master")));
    }
    pushLiveMix();
    if (values == m_faderValues)
        return;
    m_faderValues = values;
    emit faderValuesChanged();
}

void AudioMixerBridge::pushLiveMix() {
    // What the pass writes now, per strip (Touch while held, Latch since touched, Write throughout).
    std::map<QString, std::pair<std::optional<double>, std::optional<double>>> live;
    if (m_pass) {
        for (const auto& [key, value] : m_live) {
            if (automationMode(key.first) == QLatin1String("read"))
                continue;
            auto& entry = live[key.first];
            (key.second == QLatin1String("pan") ? entry.second : entry.first) = value;
        }
    }
    if (live == m_liveSent)
        return;
    // Recorded as sent only once applied: a value the player did not take (its slot not published yet, the first
    // play) is sent again on the next tick (A5-P2-26).
    std::map<QString, std::pair<std::optional<double>, std::optional<double>>> sent;
    for (const auto& [target, values] : live) {
        const auto before = m_liveSent.find(target);
        const bool already = before != m_liveSent.end() && before->second == values;
        if (already || sendLiveMix(target, values.first, values.second))
            sent.emplace(target, values);
    }
    for (const auto& [target, values] : m_liveSent) {
        if (!live.contains(target))
            sendLiveMix(target, std::nullopt, std::nullopt); // back to the model's automation
    }
    m_liveSent = std::move(sent);
}

bool AudioMixerBridge::sendLiveMix(const QString& target, std::optional<double> gainDb, std::optional<double> pan) {
    if (m_liveMixSink)
        return m_liveMixSink(target, gainDb, pan);
    // The player's live override: heard at once, ramped, the model untouched (the pass is written at stop).
    editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr;
    playback::TimelineAudioPlayer* player = session ? session->audioPlayer() : nullptr;
    if (!player)
        return false;
    const TrackId track = target == QLatin1String("master") ? TrackId{} : parseTrackKey(target).value_or(TrackId{});
    if (target != QLatin1String("master") && !track.isValid())
        return false;
    return player->setLiveTrackMix(track, gainDb, pan);
}

// ---- Live loudness ---------------------------------------------------------------------------------------------

void AudioMixerBridge::pollLoudness() {
    std::optional<audio::LoudnessReading> reading;
    if (m_loudnessSource) {
        reading = m_loudnessSource();
    } else {
        editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr;
        if (playback::TimelineAudioPlayer* player = session ? session->audioPlayer() : nullptr)
            reading = player->loudness();
    }
    QVariantMap out{{QStringLiteral("valid"), false}};
    if (reading && reading->frames > 0) {
        const auto number = [](double v) { return std::isfinite(v) ? QVariant(v) : QVariant(); };
        const auto lufs = [](double v) { return std::isfinite(v) ? dbText(v) : QStringLiteral("−∞"); };
        out = {{QStringLiteral("valid"), true},
               {QStringLiteral("momentary"), number(reading->momentaryLufs)},
               {QStringLiteral("shortTerm"), number(reading->shortTermLufs)},
               {QStringLiteral("integrated"), number(reading->integratedLufs)},
               {QStringLiteral("truePeak"), number(reading->truePeakDbtp)},
               {QStringLiteral("momentaryText"), lufs(reading->momentaryLufs)},
               {QStringLiteral("shortTermText"), lufs(reading->shortTermLufs)},
               {QStringLiteral("integratedText"), lufs(reading->integratedLufs)},
               {QStringLiteral("truePeakText"), lufs(reading->truePeakDbtp)},
               {QStringLiteral("over"), std::isfinite(reading->truePeakDbtp) && reading->truePeakDbtp > targetTruePeak()}};
        if (std::isfinite(reading->integratedLufs)) {
            const double delta = reading->integratedLufs - targetLufs();
            out.insert(QStringLiteral("deltaText"), std::abs(delta) < 0.05 ? tx("on the target")
                                                    : delta > 0 ? tx("%1 LU above the target").arg(QLocale().toString(delta, 'f', 1))
                                                                : tx("%1 LU below the target").arg(QLocale().toString(-delta, 'f', 1)));
        }
    }
    if (out == m_liveLoudness)
        return;
    m_liveLoudness = out;
    emit liveLoudnessChanged();
}

void AudioMixerBridge::resetLoudness() {
    ++m_loudnessResets;
    if (!m_loudnessSource) {
        editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr;
        if (playback::TimelineAudioPlayer* player = session ? session->audioPlayer() : nullptr)
            player->resetLoudness();
    }
    m_liveLoudness = {{QStringLiteral("valid"), false}};
    emit liveLoudnessChanged();
}

QString AudioMixerBridge::addEffect(const QString& target, const QString& effectId) {
    const auto site = targetParam(target);
    if (!site)
        return tx("Choose a track or the master");
    if (effectId.toStdString() == kEnhanceSpeech) {
        // A clip effect: the track's clips get it, through the Enhance Speech dialog (strength, render, licence).
        if (target == QLatin1String("master"))
            return tx("Enhance Speech works on dialogue clips: pick it on their track");
        const QStringList clips = trackClips(target);
        if (clips.isEmpty())
            return tx("The track has no clips to enhance");
        emit enhanceClipsRequested(clips);
        return {};
    }
    return errorOf(run("audio.addEffect", Json{{"target", *site}, {"effect", effectId.toStdString()}}));
}

QStringList AudioMixerBridge::trackClips(const QString& track) const {
    QStringList out;
    const auto id = parseTrackKey(track);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const Track* t = id && seq ? seq->findTrack(*id) : nullptr;
    if (!t || t->kind != TrackKind::Audio)
        return out;
    for (const Clip& c : t->clips)
        out << clipKey(c.id);
    return out;
}

QString AudioMixerBridge::removeEffect(const QString& target, const QString& effect) {
    const auto site = targetParam(target);
    const auto id = parseEffectKey(effect);
    if (!site || !id)
        return tx("No such effect");
    return errorOf(run("audio.removeEffect", Json{{"target", *site}, {"effect", id->value}}));
}

QString AudioMixerBridge::setEffectEnabled(const QString& target, const QString& effect, bool enabled) {
    const auto site = targetParam(target);
    const auto id = parseEffectKey(effect);
    if (!site || !id)
        return tx("No such effect");
    return errorOf(run("audio.setEffect", Json{{"target", *site}, {"effect", id->value}, {"enabled", enabled}}));
}

QString AudioMixerBridge::moveEffect(const QString& target, const QString& effect, int index) {
    const auto site = targetParam(target);
    const auto id = parseEffectKey(effect);
    if (!site || !id || index < 0)
        return tx("No such effect");
    return errorOf(run("audio.moveEffect", Json{{"target", *site}, {"effect", id->value}, {"index", index}}));
}

QString AudioMixerBridge::addMasterLimiter() {
    return errorOf(run("audio.masterLimiter", Json{{"ceiling", targetTruePeak()}}));
}

// ---- Meters ---------------------------------------------------------------------------------------------

void AudioMixerBridge::setMetersActive(bool active) {
    if (m_metersHeld == active)
        return;
    m_metersHeld = active;
    updateMetersActive();
}

void AudioMixerBridge::setMeterViewer(QObject* viewer, bool showing) {
    if (!viewer)
        return;
    const auto it = std::ranges::find(m_meterViewers, viewer);
    const bool counted = it != m_meterViewers.end();
    if (showing == counted)
        return;
    if (showing) {
        m_meterViewers.push_back(viewer);
        // A viewer destroyed while it shows meters stops counting (a panel that goes away without saying so).
        connect(viewer, &QObject::destroyed, this, [this](QObject* gone) {
            std::erase(m_meterViewers, gone);
            updateMetersActive();
            emit metersActiveChanged(); // meterViewers
        });
    } else {
        m_meterViewers.erase(it);
        disconnect(viewer, &QObject::destroyed, this, nullptr);
    }
    updateMetersActive();
    emit metersActiveChanged(); // meterViewers
}

bool AudioMixerBridge::isMeterViewer(const QObject* viewer) const {
    return std::ranges::find(m_meterViewers, viewer) != m_meterViewers.end();
}

void AudioMixerBridge::updateMetersActive() {
    const bool active = m_metersHeld || !m_meterViewers.empty();
    if (m_metersActive == active)
        return;
    m_metersActive = active;
    if (active)
        m_meterTimer.start();
    else
        m_meterTimer.stop();
    // The live loudness worker only works while someone looks at it.
    editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr;
    if (playback::TimelineAudioPlayer* player = session ? session->audioPlayer() : nullptr)
        player->setLoudnessEnabled(active);
    emit metersActiveChanged();
}

void AudioMixerBridge::pollMeters() {
    // Review A5-P2-3: the player hands out copies of its meters (a change of the master's format replaces the source
    // that holds them).
    const auto playerMeter = [this](int index) -> std::optional<playback::MeterSnapshot> {
        editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr;
        playback::TimelineAudioPlayer* player = session ? session->audioPlayer() : nullptr;
        if (!player)
            return std::nullopt;
        return index <= -2 ? player->submixMeter(-2 - index) : index < 0 ? player->masterMeter() : player->trackMeter(index);
    };
    const auto reading = [&](int index) -> std::optional<MeterReading> {
        if (m_meterSource)
            return m_meterSource(index);
        const std::optional<playback::MeterSnapshot> m = playerMeter(index);
        if (!m || m->channels() <= 0)
            return std::nullopt;
        MeterReading r;
        r.channels = std::min(kMeterChannels, m->channels()); // a 5.1 master meters six channels
        for (int ch = 0; ch < r.channels; ++ch) {
            r.peak[static_cast<std::size_t>(ch)] = m->peak(ch);
            r.rms[static_cast<std::size_t>(ch)] = m->rms(ch);
        }
        r.clipped = m->clipped();
        return r;
    };
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kHoldMs = 1500;
    bool changed = false;
    const auto update = [&](const QString& key, int index) {
        const std::optional<MeterReading> r = reading(index);
        MeterLevels next;
        const MeterLevels previous = m_levels[key];
        auto& since = m_holdSince[key];
        if (r && r->channels > 0) {
            next.channels = r->channels;
            next.clipped = r->clipped || previous.clipped; // latched until clearClip()
            for (std::size_t ch = 0; ch < static_cast<std::size_t>(r->channels); ++ch) {
                next.peakDb[ch] = toDb(r->peak[ch]);
                next.rmsDb[ch] = toDb(r->rms[ch]);
                // Peak hold: the highest peak for 1.5 s, then the current one.
                const double held = previous.channels > 0 ? previous.holdDb[ch] : -std::numeric_limits<double>::infinity();
                if (next.peakDb[ch] >= held || now - since[ch] > kHoldMs) {
                    next.holdDb[ch] = next.peakDb[ch];
                    since[ch] = now;
                } else {
                    next.holdDb[ch] = held;
                }
            }
        }
        const bool same = next.channels == previous.channels && next.clipped == previous.clipped
                          && next.peakDb == previous.peakDb && next.rmsDb == previous.rmsDb && next.holdDb == previous.holdDb;
        if (!same) {
            m_levels[key] = next;
            changed = true;
        }
    };
    for (std::size_t i = 0; i < m_trackOrder.size(); ++i)
        update(trackKey(m_trackOrder[i]), static_cast<int>(i));
    for (std::size_t k = 0; k < m_submixOrder.size(); ++k)
        update(trackKey(m_submixOrder[k]), -2 - static_cast<int>(k));
    update(QStringLiteral("master"), -1);
    if (changed)
        emit metersUpdated();
    pollLoudness();
}

AudioMixerBridge::MeterLevels AudioMixerBridge::meter(const QString& key) const {
    const auto it = m_levels.find(key);
    return it == m_levels.end() ? MeterLevels{} : it->second;
}

void AudioMixerBridge::clearClip(const QString& key) {
    auto it = m_levels.find(key);
    if (it != m_levels.end() && it->second.clipped) {
        it->second.clipped = false;
        emit metersUpdated();
    }
    editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr;
    if (playback::TimelineAudioPlayer* player = session ? session->audioPlayer() : nullptr) {
        if (key == QLatin1String("master")) {
            player->clearMasterClip();
        } else {
            const auto id = parseTrackKey(key);
            const auto pos = id ? std::ranges::find(m_trackOrder, *id) : m_trackOrder.end();
            if (pos != m_trackOrder.end())
                player->clearTrackClip(static_cast<int>(pos - m_trackOrder.begin()));
            const auto sub = id ? std::ranges::find(m_submixOrder, *id) : m_submixOrder.end();
            if (sub != m_submixOrder.end())
                player->clearSubmixClip(static_cast<int>(sub - m_submixOrder.begin()));
        }
    }
}

// ---- Loudness target ------------------------------------------------------------------------------------

QVariantList AudioMixerBridge::targets() const {
    const auto t = [](const char* id, const QString& name, double lufs, double truePeak) {
        return QVariantMap{{QStringLiteral("id"), QString::fromLatin1(id)}, {QStringLiteral("name"), name},
                           {QStringLiteral("lufs"), lufs}, {QStringLiteral("truePeak"), truePeak}};
    };
    return {t("youtube", tx("YouTube"), -14, -1), t("podcast", tx("Podcast"), -16, -1),
            t("broadcast", tx("Broadcast (EBU R128)"), -23, -1), t("custom", tx("Custom"), m_customLufs, -1)};
}

void AudioMixerBridge::setTarget(const QString& target) {
    if (m_target == target)
        return;
    m_target = target;
    emit targetChanged();
    emit loudnessChanged();
}

void AudioMixerBridge::setCustomLufs(double lufs) {
    lufs = std::clamp(lufs, -70.0, 0.0);
    if (m_customLufs == lufs)
        return;
    m_customLufs = lufs;
    emit targetChanged();
    emit loudnessChanged();
}

double AudioMixerBridge::targetLufs() const {
    if (m_target == QLatin1String("podcast"))
        return -16;
    if (m_target == QLatin1String("broadcast"))
        return -23;
    if (m_target == QLatin1String("custom"))
        return m_customLufs;
    return -14;
}

double AudioMixerBridge::targetTruePeak() const {
    return -1.0;
}

QVariantMap AudioMixerBridge::loudness() const {
    if (!m_mixLoudness)
        return {{QStringLiteral("valid"), false}};
    QVariantMap out = m_mixLoudness->first;
    out.insert(QStringLiteral("valid"), true);
    out.insert(QStringLiteral("stale"), m_controller && m_controller->session().revision() != m_mixLoudness->second);
    const QVariant integrated = out.value(QStringLiteral("integrated"));
    if (integrated.isValid() && !integrated.isNull()) {
        const double delta = integrated.toDouble() - targetLufs();
        out.insert(QStringLiteral("delta"), delta);
        out.insert(QStringLiteral("deltaText"),
                   std::abs(delta) < 0.5 ? tx("On target")
                   : delta < 0 ? tx("%1 LU below the target").arg(QLocale().toString(-delta, 'f', 1))
                               : tx("%1 LU above the target").arg(QLocale().toString(delta, 'f', 1)));
    } else {
        out.insert(QStringLiteral("deltaText"), tx("Silent"));
    }
    return out;
}

// 0.4: each duck mode keeps its own timing (m_duckTiming[m_duckIndex]).
void AudioMixerBridge::setDuckAmount(double v) {
    v = std::clamp(v, -60.0, 0.0);
    DuckTiming& t = m_duckTiming[static_cast<std::size_t>(m_duckIndex)];
    if (v != t.amount) {
        t.amount = v;
        emit toolSettingsChanged();
    }
}
void AudioMixerBridge::setDuckAttack(double v) {
    v = std::clamp(v, 0.0, 10.0);
    DuckTiming& t = m_duckTiming[static_cast<std::size_t>(m_duckIndex)];
    if (v != t.attack) {
        t.attack = v;
        emit toolSettingsChanged();
    }
}
void AudioMixerBridge::setDuckRelease(double v) {
    v = std::clamp(v, 0.0, 10.0);
    DuckTiming& t = m_duckTiming[static_cast<std::size_t>(m_duckIndex)];
    if (v != t.release) {
        t.release = v;
        emit toolSettingsChanged();
    }
}
void AudioMixerBridge::setDuckHold(double v) {
    v = std::clamp(v, 0.0, 10.0);
    DuckTiming& t = m_duckTiming[static_cast<std::size_t>(m_duckIndex)];
    if (v != t.hold) {
        t.hold = v;
        emit toolSettingsChanged();
    }
}

QString AudioMixerBridge::duckMode() const {
    return m_duckIndex == 1 ? QStringLiteral("gameUnderVoice") : QStringLiteral("default");
}

void AudioMixerBridge::setDuckMode(const QString& mode) {
    int index = 0;
    if (mode == QLatin1String("gameUnderVoice"))
        index = 1;
    else if (mode != QLatin1String("default"))
        return;
    m_duckModeChosen = true; // the pick sticks until the current sequence changes
    if (index == m_duckIndex)
        return;
    m_duckIndex = index;
    emit duckChanged();
    emit toolSettingsChanged(); // the timing of the other mode
}

void AudioMixerBridge::setDuckIncludeMusic(bool on) {
    if (on == m_duckIncludeMusic)
        return;
    m_duckIncludeMusic = on;
    updateDuckTracks();
    emit duckChanged();
}

void AudioMixerBridge::updateDuckTracks() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const SequenceId sequence = seq ? seq->id : SequenceId{};
    bool changed = false;
    if (sequence != m_duckSequence) {
        m_duckSequence = sequence;
        m_duckModeChosen = false; // another sequence: suggest again
    }
    QStringList voice;
    QStringList game;
    bool gameNamed = false; // an audio track named for the game ("Game", "Desktop Audio", ...)
    if (seq) {
        const auto nameOf = [&](TrackId id) {
            for (std::size_t i = 0; i < seq->audioTracks.size(); ++i) {
                const Track& t = *seq->audioTracks[i];
                if (t.id == id)
                    return t.name.empty() ? QStringLiteral("A%1").arg(i + 1) : QString::fromStdString(t.name);
            }
            return QString();
        };
        const audio::edit::GameVoiceTracks chosen = audio::edit::chooseGameVoiceTracks(*seq, m_duckIncludeMusic);
        for (const TrackId id : chosen.voice)
            voice << nameOf(id);
        for (const TrackId id : chosen.game)
            game << nameOf(id);
        for (const TrackPtr& t : seq->audioTracks) {
            const edit::SourceTrackName named = edit::sourceTrackName(t->name);
            if (named.origin == edit::SourceTrackName::Origin::Recognised && named.name.starts_with("Game"))
                gameNamed = true;
        }
    }
    if (!m_duckModeChosen) {
        const int suggested = gameNamed && !voice.isEmpty() ? 1 : 0;
        if (suggested != m_duckIndex) {
            m_duckIndex = suggested;
            changed = true;
            emit toolSettingsChanged();
        }
    }
    const QVariantMap tracks{{QStringLiteral("voice"), voice}, {QStringLiteral("game"), game}};
    if (tracks != m_gameTracks) {
        m_gameTracks = tracks;
        changed = true;
    }
    if (changed)
        emit duckChanged();
}
void AudioMixerBridge::setDenoiseAmount(double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (v != m_denoiseAmount) {
        m_denoiseAmount = v;
        emit toolSettingsChanged();
    }
}

// ---- Audio tools ----------------------------------------------------------------------------------------

void AudioMixerBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

QVariantMap AudioMixerBridge::pending() const {
    if (!m_pending)
        return {};
    return {{QStringLiteral("kind"), m_pending->kind},   {QStringLiteral("title"), m_pending->title},
            {QStringLiteral("info"), m_pending->info},   {QStringLiteral("rows"), m_pending->rows},
            {QStringLiteral("warnings"), m_pending->warnings}};
}

void AudioMixerBridge::setPending(std::optional<Pending> pending) {
    if (!pending && !m_pending)
        return;
    const bool wasDuck = m_pending && m_pending->kind == QLatin1String("autoDuck");
    m_pending = std::move(pending);
    if (wasDuck && !(m_pending && m_pending->kind == QLatin1String("autoDuck")))
        clearDuckPreview();
    emit pendingChanged();
}

QString AudioMixerBridge::startJob(const QString& kind, std::vector<command::CommandRequest> requests) {
    if (!m_controller)
        return tx("No project");
    if (!m_busy.isEmpty())
        return tx("Another audio job is running");
    const ProjectSnapshot base = m_controller->snapshot();
    const std::uint64_t revision = m_controller->session().revision();
    const auto services = this->services();
    const std::uint64_t sequence = m_controller->currentSequenceId().value;
    for (command::CommandRequest& r : requests) {
        r.params["sequence"] = sequence;
        r.dryRun = true;
    }
    setPending(std::nullopt);
    setMessage({});
    m_busy = kind;
    emit jobChanged();

    // Dry runs over a copy of the project: the real session stays untouched until applyPending().
    auto work = [base, services, requests](command::ExecutionControl* control) {
        EditSession scratch(*base);
        command::EnginePolicy policy;
        policy.services = services;
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
        std::vector<command::CommandResult> results;
        for (const command::CommandRequest& r : requests)
            results.push_back(engine.execute(r, control));
        return results;
    };
    if (m_synchronous) {
        CancelControl control;
        finishJob(kind, work(&control), revision);
        return {};
    }
    m_job = std::make_unique<Job>();
    Job* job = m_job.get();
    QPointer<AudioMixerBridge> self(this);
    job->thread = std::jthread([self, job, work, kind, revision] {
        auto results = work(&job->control);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, kind, revision, results = std::move(results)]() mutable {
                if (self)
                    self->finishJob(kind, std::move(results), revision);
            },
            Qt::QueuedConnection);
    });
    return {};
}

void AudioMixerBridge::finishJob(const QString& kind, std::vector<command::CommandResult> results, std::uint64_t baseRevision) {
    if (m_job) {
        if (m_job->thread.joinable() && m_job->thread.get_id() != std::this_thread::get_id())
            m_job->thread.join();
        m_job.reset();
    }
    m_busy.clear();
    emit jobChanged();
    for (const command::CommandResult& r : results) {
        if (!r.ok()) {
            setMessage(r.error->code == command::ErrorCode::Cancelled ? tx("Cancelled")
                                                             : tx("Could not finish: %1").arg(QString::fromStdString(r.error->message)));
            return;
        }
    }
    if (results.empty())
        return;
    const Json& data = results.front().data;

    if (kind == QLatin1String("analyze")) {
        QVariantList rows;
        for (const Json& r : data.value("results", Json::array())) {
            const Json& target = r.value("target", Json());
            QString name;
            QString what = QStringLiteral("mix");
            if (target.is_string()) {
                name = tx("Mix");
            } else if (target.contains("track")) {
                what = QStringLiteral("track");
                name = QString::fromStdString(target["track"].is_string() ? target["track"].get<std::string>()
                                                                          : target["track"].dump());
            } else if (target.contains("clip")) {
                what = QStringLiteral("clip");
                name = tx("Clip %1").arg(QString::fromStdString(target["clip"].dump()));
                if (m_controller && target["clip"].is_number_unsigned()) {
                    const SequencePtr seq = m_controller->currentSequence();
                    if (const auto loc = seq ? seq->findClip(ClipId{target["clip"].get<std::uint64_t>()}) : std::nullopt)
                        name = QString::fromStdString(std::string(loc->clip().name()));
                }
            }
            const QVariantMap row{{QStringLiteral("target"), name},
                                  {QStringLiteral("kind"), what},
                                  {QStringLiteral("integrated"), lufsValue(r.value("integratedLufs", Json()))},
                                  {QStringLiteral("range"), lufsValue(r.value("loudnessRangeLu", Json()))},
                                  {QStringLiteral("truePeak"), lufsValue(r.value("truePeakDbtp", Json()))},
                                  {QStringLiteral("samplePeak"), lufsValue(r.value("samplePeakDbfs", Json()))},
                                  {QStringLiteral("momentary"), lufsValue(r.value("maxMomentaryLufs", Json()))},
                                  {QStringLiteral("shortTerm"), lufsValue(r.value("maxShortTermLufs", Json()))},
                                  {QStringLiteral("seconds"), lufsValue(r.value("seconds", Json()))}};
            rows.append(row);
            if (what == QLatin1String("mix")) {
                QVariantMap mix = row;
                mix.insert(QStringLiteral("whenText"), QLocale().toString(QTime::currentTime(), QLocale::ShortFormat));
                m_mixLoudness = std::make_pair(mix, baseRevision);
                emit loudnessChanged();
            }
        }
        m_report = rows;
        emit reportChanged();
        setMessage(tx("Measured %n target(s)", "", static_cast<int>(rows.size())));
        return;
    }

    Pending p;
    p.kind = kind;
    p.preview = results.front().preview;
    p.baseRevision = baseRevision;
    for (const std::string& w : results.front().warnings)
        p.warnings << QString::fromStdString(w);
    if (!p.preview) {
        setMessage(tx("Nothing to change"));
        return;
    }
    if (kind == QLatin1String("normalize")) {
        p.undoLabel = tx("Normalize Loudness").toStdString();
        p.title = tx("Normalize to %1 LUFS").arg(QLocale().toString(data.value("targetLufs", targetLufs()), 'f', 1));
        for (const Json& r : data.value("results", Json::array())) {
            const Json& before = r.value("before", Json::object());
            const Json& after = r.value("after", Json::object());
            const Json& target = r.value("target", Json());
            p.rows.append(QVariantMap{
                {QStringLiteral("target"), target.is_string() ? tx("Mix") : QString::fromStdString(target.dump())},
                {QStringLiteral("before"), lufsValue(before.value("integratedLufs", Json()))},
                {QStringLiteral("gain"), lufsValue(r.value("gainDb", Json()))},
                {QStringLiteral("after"), lufsValue(after.value("integratedLufs", Json()))},
                {QStringLiteral("truePeak"), lufsValue(after.value("truePeakDbtp", Json()))},
            });
        }
        p.info = tx("Gain is set on the first Gain effect of each target (added when missing).");
    } else {
        const int regions = static_cast<int>(data.value("regions", Json::array()).size());
        const Json clips = data.value("clips", Json::array());
        int keys = 0;
        for (const Json& c : clips)
            keys += c.value("keys", 0);
        const double amount = std::abs(data.value("amountDb", duckAmount()));
        if (data.value("preset", std::string()) == "gameUnderVoice") {
            // 0.4: "Mic → ducks Game, Music · 4 speech regions, 12 volume keyframes on 2 clips"
            const auto names = [](const Json& tracks) {
                QStringList out;
                for (const Json& t : tracks) {
                    const std::string name = t.value("name", std::string());
                    out << QString::fromStdString(name.empty() ? t.value("track", std::string()) : name);
                }
                return out.join(QStringLiteral(", "));
            };
            p.undoLabel = tx("Duck Game Under Voice").toStdString();
            p.title = tx("Duck the game by %1 dB").arg(QLocale().toString(amount, 'f', 1));
            p.info = tx("%1 → ducks %2 · %n speech region(s), %3 volume keyframes on %4 clip(s)", "", regions)
                         .arg(names(data.value("dialogue", Json::array())), names(data.value("music", Json::array())))
                         .arg(keys)
                         .arg(clips.size());
        } else {
            p.undoLabel = tx("Auto-Duck Music").toStdString();
            p.title = tx("Duck the music by %1 dB").arg(QLocale().toString(amount, 'f', 1));
            p.info = tx("%n speech region(s), %1 volume keyframes on %2 music clip(s)", "", regions)
                         .arg(keys)
                         .arg(clips.size());
        }
        p.rows.append(QVariantMap{{QStringLiteral("target"), tx("Speech regions")}, {QStringLiteral("value"), regions}});
        p.rows.append(QVariantMap{{QStringLiteral("target"), tx("Volume keyframes")}, {QStringLiteral("value"), keys}});
        showDuckPreview(*p.preview);
    }
    setPending(std::move(p));
}

QString AudioMixerBridge::analyze(const QString& scope) {
    Json targets = Json::array();
    if (scope == QLatin1String("clips")) {
        const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
        for (ClipId id : m_controller ? m_controller->selection() : std::vector<ClipId>{}) {
            const auto loc = seq ? seq->findClip(id) : std::nullopt;
            if (loc && loc->track->kind == TrackKind::Audio)
                targets.push_back(Json{{"clip", id.value}});
        }
        if (targets.empty())
            return tx("Select audio clips to measure");
    } else {
        if (scope == QLatin1String("tracks")) {
            for (TrackId t : m_trackOrder)
                targets.push_back(Json{{"track", t.value}});
        }
        targets.push_back("mix");
    }
    command::CommandRequest request;
    request.command = "audio.loudness";
    request.params = Json{{"targets", targets}};
    return startJob(QStringLiteral("analyze"), {request});
}

QString AudioMixerBridge::previewNormalize(const QString& scope) {
    command::CommandRequest request;
    request.command = "audio.normalize";
    Json params = Json::object();
    if (m_target == QLatin1String("custom")) {
        params["lufs"] = m_customLufs;
        params["truePeak"] = targetTruePeak();
    } else {
        params["preset"] = m_target.toStdString();
    }
    if (scope == QLatin1String("clips")) {
        Json clips = Json::array();
        const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
        for (ClipId id : m_controller ? m_controller->selection() : std::vector<ClipId>{}) {
            const auto loc = seq ? seq->findClip(id) : std::nullopt;
            if (loc && loc->track->kind == TrackKind::Audio)
                clips.push_back(id.value);
        }
        if (clips.empty())
            return tx("Select audio clips to normalize");
        params["clips"] = clips;
    }
    request.params = params;
    return startJob(QStringLiteral("normalize"), {request});
}

QString AudioMixerBridge::previewAutoDuck() {
    command::CommandRequest request;
    request.command = "audio.autoDuck";
    const DuckTiming& timing = m_duckTiming[static_cast<std::size_t>(m_duckIndex)];
    request.params = Json{{"amount", timing.amount}, {"attack", timing.attack}, {"release", timing.release}, {"hold", timing.hold}};
    if (m_duckIndex == 1) {
        // 0.4: the gaming preset picks the voice and the game by track name (the timing sent still wins).
        request.params["preset"] = "gameUnderVoice";
        request.params["includeMusic"] = m_duckIncludeMusic;
    }
    return startJob(QStringLiteral("autoDuck"), {request});
}

QString AudioMixerBridge::reduceNoise() {
    if (!m_controller)
        return tx("No project");
    const SequencePtr seq = m_controller->currentSequence();
    Json add = Json::array();
    std::vector<command::CommandRequest> requests;
    for (ClipId id : m_controller->selection()) {
        const auto loc = seq ? seq->findClip(id) : std::nullopt;
        if (!loc || loc->track->kind != TrackKind::Audio)
            continue;
        const EffectStack& stack = loc->clip().effects();
        const auto it = std::ranges::find_if(stack, [](const EffectInstance& e) { return e.effect == "wabcut.audio.denoise"; });
        if (it == stack.end()) {
            add.push_back(id.value);
        } else {
            command::CommandRequest set;
            set.command = "clip.setEffect";
            set.params = Json{{"clip", id.value}, {"effect", it->id.value}, {"enabled", true},
                              {"params", Json{{"amount", m_denoiseAmount}}}};
            requests.push_back(std::move(set));
        }
    }
    if (!add.empty()) {
        command::CommandRequest request;
        request.command = "clip.addEffect";
        request.params = Json{{"clips", add}, {"effect", "wabcut.audio.denoise"}, {"params", Json{{"amount", m_denoiseAmount}}}};
        requests.insert(requests.begin(), std::move(request));
    }
    if (requests.empty())
        return tx("Select audio clips to clean up");
    const int count = static_cast<int>(add.size() + requests.size() - (add.empty() ? 0 : 1));
    const command::CommandResult result = runBatch(std::move(requests), tx("Reduce Noise").toStdString());
    if (!result.ok())
        return QString::fromStdString(result.error->message);
    setMessage(tx("Reduce Noise on %n clip(s)", "", count));
    return {};
}

QString AudioMixerBridge::applyPending() {
    if (!m_pending || !m_controller)
        return tx("Nothing to apply");
    if (m_controller->session().revision() != m_pending->baseRevision) {
        setPending(std::nullopt);
        return tx("The project changed since the preview: preview it again");
    }
    const std::string label = !m_pending->undoLabel.empty()                  ? m_pending->undoLabel
                              : m_pending->kind == QLatin1String("normalize") ? tx("Normalize Loudness").toStdString()
                                                                              : tx("Auto-Duck Music").toStdString();
    const auto preview = m_pending->preview;
    setPending(std::nullopt);
    if (auto committed = m_controller->session().commit(label, *preview); !committed)
        return QString::fromStdString(committed.error().message);
    setMessage(tx("Applied"));
    return {};
}

void AudioMixerBridge::cancelPending() {
    setPending(std::nullopt);
}

void AudioMixerBridge::cancelJob() {
    if (m_job)
        m_job->control.cancelled = true;
}

// ---- Auto-duck preview on the timeline ------------------------------------------------------------------

void AudioMixerBridge::showDuckPreview(const ProjectState& preview) {
    if (!m_controller)
        return;
    const SequencePtr current = m_controller->currentSequence();
    const Sequence* next = current ? preview.findSequence(current->id) : nullptr;
    if (!current || !next)
        return;
    TimelineView::ExternalPreview ghost;
    for (const TrackPtr& track : next->audioTracks) {
        for (const Clip& clip : track->clips) {
            const ParamValue after = clip.properties()[ParamId::VolumeDb];
            const auto before = current->findClip(clip.id);
            if (!after.curve() || (before && before->clip().properties()[ParamId::VolumeDb] == after))
                continue;
            TimelineView::ExternalPreview::VolumeCurve curve;
            curve.track = track->id;
            curve.start = clip.start();
            curve.end = clip.end();
            curve.points.emplace_back(clip.start(), after.valueAt(clip.curveTimeAt(clip.start())));
            for (const Keyframe& k : after.curve()->keys) {
                const auto t = clip.timelineTimeOfCurveTime(k.time);
                if (t && *t > clip.start() && *t < clip.end())
                    curve.points.emplace_back(*t, k.value);
            }
            curve.points.emplace_back(clip.end(), after.valueAt(clip.curveTimeAt(clip.end() - Ticks{1})));
            ghost.volumes.push_back(std::move(curve));
        }
    }
    for (TimelineView* view : timelineViews(m_controller))
        view->setExternalPreview(ghost);
    m_duckPreviewShown = true;
}

void AudioMixerBridge::clearDuckPreview() {
    if (!m_duckPreviewShown)
        return;
    m_duckPreviewShown = false;
    for (TimelineView* view : timelineViews(m_controller))
        view->clearExternalPreview();
}

} // namespace wabcut::app
