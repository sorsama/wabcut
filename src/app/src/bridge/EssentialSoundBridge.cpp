#include "EssentialSoundBridge.h"

#include "AiController.h"
#include "AppAudioServices.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "TimelineView.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QWindow>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <set>
#include <utility>

namespace wabcut::app {

using command::Json;
namespace es = audio::edit;

namespace {

QString text(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

/// Every TimelineView showing `controller`'s project, in any window (as AudioMixerBridge finds them).
std::vector<TimelineView*> timelineViews(const ProjectController* controller) {
    std::vector<TimelineView*> out;
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        auto* quick = qobject_cast<QQuickWindow*>(window);
        if (!quick)
            continue;
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

/// Cancels through `cancelled`; reports "... (NN%)" checkpoints as progress (as CommandJob does).
class PreviewControl final : public command::ExecutionControl {
public:
    PreviewControl(const std::atomic<bool>& cancelled, std::function<void(QString, double)> report)
        : m_cancelled(cancelled), m_report(std::move(report)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        if (m_report) {
            static const QRegularExpression percent(QStringLiteral("\\((\\d+)%\\)\\s*$"));
            QString phase = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
            const auto match = percent.match(phase);
            double progress = -1.0;
            if (match.hasMatch()) {
                progress = match.captured(1).toDouble() / 100.0;
                phase = phase.left(match.capturedStart()).trimmed();
            }
            m_report(phase, progress);
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

constexpr std::array<std::string_view, 4> kTypes{"dialogue", "music", "sfx", "ambience"};

bool isType(const QString& type) {
    return std::ranges::any_of(kTypes, [&](std::string_view t) { return type == text(t); });
}

double defaultTarget(const QString& type) {
    if (type == QLatin1String("dialogue"))
        return -23.0;
    if (type == QLatin1String("ambience"))
        return -30.0;
    return -25.0;
}

/// The panel's settings with everything off (the type's loudness target on).
QVariantMap defaultOptions(const QString& type) {
    return {{QStringLiteral("loudness"), true},
            {QStringLiteral("targetLufs"), defaultTarget(type)},
            {QStringLiteral("repair"), QVariantMap{{QStringLiteral("noise"), 0.0},
                                                  {QStringLiteral("enhance"), false},
                                                  {QStringLiteral("rumble"), 0.0},
                                                  {QStringLiteral("hum"), 0.0},
                                                  {QStringLiteral("deess"), 0.0},
                                                  {QStringLiteral("reverb"), 0.0}}},
            {QStringLiteral("clarity"), QVariantMap{{QStringLiteral("dynamics"), 0.0},
                                                   {QStringLiteral("eq"), QString()},
                                                   {QStringLiteral("eqAmount"), 1.0}}},
            {QStringLiteral("creative"), QVariantMap{{QStringLiteral("reverb"), QString()}, {QStringLiteral("amount"), -1.0}}},
            {QStringLiteral("ducking"), QVariantMap{{QStringLiteral("enabled"), false},
                                                   {QStringLiteral("amount"), -12.0},
                                                   {QStringLiteral("attack"), 0.25},
                                                   {QStringLiteral("release"), 0.75},
                                                   {QStringLiteral("hold"), 0.25}}},
            {QStringLiteral("width"), 1.0},
            {QStringLiteral("pan"), QVariant()}};
}

/// Sets `path` ("repair.noise") in a nested map.
void setPath(QVariantMap& map, const QStringList& path, const QVariant& value) {
    if (path.isEmpty())
        return;
    if (path.size() == 1) {
        map.insert(path.front(), value);
        return;
    }
    QVariantMap child = map.value(path.front()).toMap();
    setPath(child, path.mid(1), value);
    map.insert(path.front(), child);
}

QVariant at(const QVariantMap& map, const QString& path) {
    QVariant v = map;
    for (const QString& part : path.split(QLatin1Char('.')))
        v = v.toMap().value(part);
    return v;
}

double numberAt(const QVariantMap& map, const QString& path, double fallback) {
    const QVariant v = at(map, path);
    bool ok = false;
    const double d = v.toDouble(&ok);
    return v.isValid() && ok ? d : fallback;
}

/// The record's options (only the settings that are on) over the defaults.
QVariantMap optionsFromRecord(const Json& o, const QString& type) {
    QVariantMap out = defaultOptions(type);
    if (!o.is_object())
        return out;
    const auto num = [](const Json& j, const char* key) -> std::optional<double> {
        if (j.is_object() && j.contains(key) && j[key].is_number())
            return j[key].get<double>();
        return std::nullopt;
    };
    const auto str = [](const Json& j, const char* key) -> std::optional<QString> {
        if (j.is_object() && j.contains(key) && j[key].is_string())
            return QString::fromStdString(j[key].get<std::string>());
        return std::nullopt;
    };
    if (o.contains("loudness") && o["loudness"].is_boolean())
        setPath(out, {QStringLiteral("loudness")}, o["loudness"].get<bool>());
    if (auto v = num(o, "targetLufs"))
        setPath(out, {QStringLiteral("targetLufs")}, *v);
    const Json repair = o.value("repair", Json::object());
    for (const char* key : {"noise", "rumble", "hum", "deess", "reverb"}) {
        if (auto v = num(repair, key))
            setPath(out, {QStringLiteral("repair"), QString::fromLatin1(key)}, *v);
    }
    if (repair.is_object() && repair.contains("enhance") && repair["enhance"].is_boolean())
        setPath(out, {QStringLiteral("repair"), QStringLiteral("enhance")}, repair["enhance"].get<bool>());
    const Json clarity = o.value("clarity", Json::object());
    if (auto v = num(clarity, "dynamics"))
        setPath(out, {QStringLiteral("clarity"), QStringLiteral("dynamics")}, *v);
    if (auto v = str(clarity, "eq"))
        setPath(out, {QStringLiteral("clarity"), QStringLiteral("eq")}, *v == QLatin1String("none") ? QString() : *v);
    if (auto v = num(clarity, "eqAmount"))
        setPath(out, {QStringLiteral("clarity"), QStringLiteral("eqAmount")}, *v);
    const Json creative = o.value("creative", Json::object());
    if (auto v = str(creative, "reverb"))
        setPath(out, {QStringLiteral("creative"), QStringLiteral("reverb")}, *v == QLatin1String("none") ? QString() : *v);
    if (auto v = num(creative, "amount"))
        setPath(out, {QStringLiteral("creative"), QStringLiteral("amount")}, *v);
    if (o.contains("ducking")) {
        const Json& d = o["ducking"];
        if (d.is_boolean()) {
            setPath(out, {QStringLiteral("ducking"), QStringLiteral("enabled")}, d.get<bool>());
        } else if (d.is_object()) {
            setPath(out, {QStringLiteral("ducking"), QStringLiteral("enabled")}, d.value("enabled", true));
            for (const char* key : {"amount", "attack", "release", "hold"}) {
                if (auto v = num(d, key))
                    setPath(out, {QStringLiteral("ducking"), QString::fromLatin1(key)}, *v);
            }
        }
    }
    if (auto v = num(o, "width"))
        setPath(out, {QStringLiteral("width")}, *v);
    if (auto v = num(o, "pan"))
        setPath(out, {QStringLiteral("pan")}, *v);
    return out;
}

const EffectInstance* recordOf(const Clip& clip) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == es::kEssentialSoundEffect)
            return &e;
    }
    return nullptr;
}

QString dataText(const EffectInstance& e, std::string_view key) {
    const EffectData* d = e.findData(key);
    const std::string* t = d ? d->text() : nullptr;
    return t ? QString::fromStdString(*t) : QString();
}

double jsonNumber(const Json& j, const char* key, double fallback = 0.0) {
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<double>() : fallback;
}

} // namespace

EssentialSoundBridge::EssentialSoundBridge(QObject* parent) : QObject(parent) {}

EssentialSoundBridge::~EssentialSoundBridge() = default;

EssentialSoundBridge* EssentialSoundBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new EssentialSoundBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        QPointer<AiController> ai = qmlEngine->singletonInstance<AiController*>("Wabcut", "AiController");
        QPointer<TranscriptBridge> transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
        QPointer<ProjectController> controller = bridge->m_controller;
        // Its jobs may download a model through the AI transport: stopped before AiController destroys it (A5-P2-19).
        if (ai) {
            QObject::connect(ai, &AiController::shuttingDown, bridge, [bridge] {
                bridge->m_job.stop();
                bridge->m_previewJob.stop();
            });
        }
        // The app's audio services, made when first needed (loudness, speech for ducking, the music analysis).
        bridge->m_factory = [ai, transcripts, controller] {
            AppAudioServiceOptions options;
            options.ai = ai.data();
            options.transcripts = transcripts.data();
            if (controller)
                options.projectFile = controller->projectFile();
            return appAudioServices(options);
        };
    }
    return bridge;
}

void EssentialSoundBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::selectionChanged, this, &EssentialSoundBridge::refresh);
        connect(controller, &ProjectController::revisionChanged, this, &EssentialSoundBridge::onRevisionChanged);
        connect(controller, &ProjectController::sequenceChanged, this, [this] {
            cancel();
            refresh();
        });
    }
    refresh();
}

void EssentialSoundBridge::setSynchronous(bool synchronous) {
    m_synchronous = synchronous;
    m_job.setSynchronous(synchronous);
}

std::shared_ptr<command::CommandServices> EssentialSoundBridge::services() {
    if (m_services || !m_factory)
        return m_services;
    // Review A5-P2-20: the services cache per project (the music analysis, the ducking speech): ones built for the
    // project open before (or this one before Save As) would read and write that project's caches. Built again, as
    // PodcastBridge::stepServices() does.
    const std::filesystem::path file = m_controller ? m_controller->projectFile() : std::filesystem::path();
    if (!m_built || m_builtFor != file) {
        m_built = m_factory();
        m_builtFor = file;
    }
    return m_built;
}

void EssentialSoundBridge::setJob(const QString& phase, double progress) {
    m_phase = phase;
    m_progress = progress;
    emit jobChanged();
}

void EssentialSoundBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

// ---- The selection -----------------------------------------------------------------------------------------------

std::vector<ClipId> EssentialSoundBridge::selectedAudio() const {
    std::vector<ClipId> out;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return out;
    std::vector<std::pair<Ticks, ClipId>> found;
    std::set<std::uint64_t> seen;
    const auto add = [&](const Clip& c) {
        if (seen.insert(c.id.value).second)
            found.emplace_back(c.start(), c.id);
    };
    for (const ClipId id : m_controller->selection()) {
        const auto loc = seq->findClip(id);
        if (!loc)
            continue;
        const Clip& c = loc->clip();
        if (loc->track->kind == TrackKind::Audio) {
            add(c);
            continue;
        }
        // A video clip stands for its linked audio.
        if (!c.linkGroup.isValid())
            continue;
        for (const TrackPtr& t : seq->audioTracks) {
            for (const Clip& other : t->clips) {
                if (other.linkGroup == c.linkGroup)
                    add(other);
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [start, id] : found)
        out.push_back(id);
    return out;
}

QStringList EssentialSoundBridge::clips() const {
    QStringList out;
    for (const ClipId id : m_clips)
        out << clipKey(id);
    return out;
}

void EssentialSoundBridge::refresh() {
    m_clips = selectedAudio();
    m_clipSeconds = 0;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    QString type;
    QString preset;
    QVariantMap sections;
    Json recordOptions;
    bool first = true;
    for (const ClipId id : m_clips) {
        const auto loc = seq ? seq->findClip(id) : std::nullopt;
        if (!loc)
            continue;
        const Clip& c = loc->clip();
        const auto recorded = es::recordedSoundType(c.effects());
        const QString t = recorded ? text(es::toString(*recorded)) : QString();
        if (first) {
            type = t;
            m_clipSeconds = c.duration().toSeconds();
            if (const EffectInstance* record = recordOf(c)) {
                preset = dataText(*record, "preset");
                recordOptions = Json::parse(dataText(*record, "options").toStdString(), nullptr, false);
            }
            // The sections of the first clip: on when one of their marked effects is on.
            for (const EffectInstance& e : c.effects()) {
                const QString section = dataText(e, es::kSectionKey);
                if (section.isEmpty())
                    continue;
                const auto mark = [&](const QString& key) {
                    if (e.enabled || sections.value(key).toString() != QLatin1String("on"))
                        sections.insert(key, e.enabled ? QStringLiteral("on") : QStringLiteral("off"));
                };
                mark(section);
                if (section.contains(QLatin1Char('.')))
                    mark(section.section(QLatin1Char('.'), 0, 0));
            }
            first = false;
        } else if (t != type) {
            type = QStringLiteral("mixed");
        }
    }
    const QVariantMap options = optionsFromRecord(recordOptions, type);
    m_type = type;
    m_preset = type == QLatin1String("mixed") ? QString() : preset;
    m_sections = sections;
    emit selectionChanged(); // the clips may have changed even when the type did not
    if (options != m_options) {
        m_options = options;
        emit optionsChanged();
    }
}

QVariantList EssentialSoundBridge::presets() const {
    QVariantList out;
    const auto type = es::soundTypeFromString(m_type.toStdString());
    if (!type)
        return out;
    for (const es::EssentialSoundPreset& p : es::essentialSoundPresets(*type))
        out.append(QVariantMap{{QStringLiteral("id"), text(p.id)}, {QStringLiteral("name"), text(p.name)}});
    return out;
}

QVariantList EssentialSoundBridge::eqPresets() const {
    QVariantList out{QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("name"), tr("None")}}};
    for (const std::string_view id : es::clarityEqPresets()) {
        const QString key = text(id);
        const QString name = key == QLatin1String("podcastVoice") ? tr("Podcast voice")
                             : key == QLatin1String("phone")      ? tr("Telephone")
                             : key == QLatin1String("radio")      ? tr("Radio")
                             : key == QLatin1String("streamerVoice") ? tr("Streamer voice")
                                                                  : key;
        out.append(QVariantMap{{QStringLiteral("id"), key}, {QStringLiteral("name"), name}});
    }
    return out;
}

QVariantList EssentialSoundBridge::reverbPresets() const {
    QVariantList out{QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("name"), tr("None")}}};
    for (const std::string_view id : es::reverbPresets()) {
        const QString key = text(id);
        const QString name = key == QLatin1String("smallRoom")   ? tr("Small room")
                             : key == QLatin1String("largeRoom") ? tr("Large room")
                             : key == QLatin1String("hall")      ? tr("Hall")
                             : key == QLatin1String("plate")     ? tr("Plate")
                             : key == QLatin1String("outside")   ? tr("Outside")
                                                                 : key;
        out.append(QVariantMap{{QStringLiteral("id"), key}, {QStringLiteral("name"), name}});
    }
    return out;
}

// ---- Essential Sound -----------------------------------------------------------------------------------------------

Json EssentialSoundBridge::fullOptions() const {
    const QVariantMap& o = m_options;
    Json out{{"loudness", at(o, QStringLiteral("loudness")).toBool()},
             {"targetLufs", std::clamp(numberAt(o, QStringLiteral("targetLufs"), defaultTarget(m_type)), -60.0, 0.0)}};
    double rumble = std::clamp(numberAt(o, QStringLiteral("repair.rumble"), 0.0), 0.0, 500.0);
    if (rumble > 0.0 && rumble < 20.0)
        rumble = 20.0;
    double hum = std::clamp(numberAt(o, QStringLiteral("repair.hum"), 0.0), 0.0, 70.0);
    if (hum > 0.0 && hum < 40.0)
        hum = 50.0;
    out["repair"] = Json{{"noise", std::clamp(numberAt(o, QStringLiteral("repair.noise"), 0.0), 0.0, 1.0)},
                         {"enhance", at(o, QStringLiteral("repair.enhance")).toBool()},
                         {"rumble", rumble},
                         {"hum", hum},
                         {"deess", std::clamp(numberAt(o, QStringLiteral("repair.deess"), 0.0), 0.0, 1.0)},
                         {"reverb", std::clamp(numberAt(o, QStringLiteral("repair.reverb"), 0.0), 0.0, 1.0)}};
    const QString eq = at(o, QStringLiteral("clarity.eq")).toString();
    out["clarity"] = Json{{"dynamics", std::clamp(numberAt(o, QStringLiteral("clarity.dynamics"), 0.0), 0.0, 1.0)},
                          {"eq", eq.isEmpty() ? std::string("none") : eq.toStdString()},
                          {"eqAmount", std::clamp(numberAt(o, QStringLiteral("clarity.eqAmount"), 1.0), 0.0, 1.0)}};
    const QString reverb = at(o, QStringLiteral("creative.reverb")).toString();
    Json creative{{"reverb", reverb.isEmpty() ? std::string("none") : reverb.toStdString()}};
    const double amount = numberAt(o, QStringLiteral("creative.amount"), -1.0);
    if (amount >= 0.0)
        creative["amount"] = std::min(amount, 1.0);
    out["creative"] = std::move(creative);
    if (at(o, QStringLiteral("ducking.enabled")).toBool()) {
        out["ducking"] = Json{{"enabled", true},
                              {"amount", std::clamp(numberAt(o, QStringLiteral("ducking.amount"), -12.0), -60.0, 0.0)},
                              {"attack", std::clamp(numberAt(o, QStringLiteral("ducking.attack"), 0.25), 0.0, 10.0)},
                              {"release", std::clamp(numberAt(o, QStringLiteral("ducking.release"), 0.75), 0.0, 10.0)},
                              {"hold", std::clamp(numberAt(o, QStringLiteral("ducking.hold"), 0.25), 0.0, 10.0)}};
    } else {
        out["ducking"] = Json{{"enabled", false}}; // the command takes an object
    }
    out["width"] = std::clamp(numberAt(o, QStringLiteral("width"), 1.0), 0.0, 2.0);
    const QVariant pan = at(o, QStringLiteral("pan"));
    if (m_type == QLatin1String("sfx") && pan.isValid() && !pan.isNull())
        out["pan"] = std::clamp(pan.toDouble(), -1.0, 1.0);
    return out;
}

QString EssentialSoundBridge::run(Json params, const char* label) {
    if (!m_controller)
        return tr("No project");
    if (m_clips.empty())
        return tr("Select audio clips first");
    if (m_job.running())
        return tr("Essential Sound is still working");
    Json ids = Json::array();
    for (const ClipId id : m_clips)
        ids.push_back(id.value);
    params["clips"] = std::move(ids);
    setMessage({});
    setJob(tr("Applying Essential Sound"), -1);
    QPointer<EssentialSoundBridge> self(this);
    const bool started = m_job.start(
        this, m_controller, services(), {"audio.essentialSound", std::move(params), label},
        [self](const QString& phase, double progress) {
            if (self)
                self->setJob(phase, progress);
        },
        [self](const command::CommandResult& result) {
            if (!self)
                return;
            self->setJob({}, 0);
            if (!result.ok())
                self->setMessage(commandErrorText(result));
            else if (!result.warnings.empty())
                self->setMessage(QString::fromStdString(result.warnings.front()));
            else
                self->setMessage(tr("Essential Sound applied"));
            self->refresh();
        });
    if (!started) {
        setJob({}, 0);
        return tr("Essential Sound is still working");
    }
    return {};
}

QString EssentialSoundBridge::setType(const QString& type) {
    if (type == QLatin1String("none"))
        return clear();
    if (!isType(type))
        return tr("Choose Dialogue, Music, SFX or Ambience");
    return run(Json{{"type", type.toStdString()}, {"preset", "default"}}, "Essential Sound");
}

QString EssentialSoundBridge::applyPreset(const QString& preset) {
    if (!isType(m_type))
        return tr("Choose a type for the clips first");
    return run(Json{{"type", m_type.toStdString()}, {"preset", preset.toStdString()}}, "Essential Sound Preset");
}

void EssentialSoundBridge::previewOption(const QString& path, const QVariant& value) {
    setPath(m_options, path.split(QLatin1Char('.')), value);
    emit optionsChanged();
}

QString EssentialSoundBridge::setOption(const QString& path, const QVariant& value) {
    previewOption(path, value);
    return commitOptions();
}

QString EssentialSoundBridge::commitOptions() {
    if (!isType(m_type))
        return tr("Choose a type for the clips first");
    return run(Json{{"type", m_type.toStdString()},
                    {"preset", m_preset.isEmpty() ? std::string("default") : m_preset.toStdString()},
                    {"options", fullOptions()}},
               "Essential Sound");
}

QString EssentialSoundBridge::clear() {
    return run(Json{{"type", "none"}}, "Clear Essential Sound");
}

QString EssentialSoundBridge::setSectionEnabled(const QString& section, bool enabled) {
    if (!m_controller)
        return tr("No project");
    if (m_clips.empty())
        return tr("Select audio clips first");
    const SequenceId sequence = m_controller->currentSequenceId();
    const std::vector<ClipId> clips = m_clips;
    const std::string wanted = section.toStdString();
    const bool ok = m_controller->applyEditFn(
        enabled ? QT_TRANSLATE_NOOP("wabcut::app::EssentialSoundBridge", "Essential Sound Section On")
                : QT_TRANSLATE_NOOP("wabcut::app::EssentialSoundBridge", "Essential Sound Section Off"),
        [&](const ProjectState& state) -> edit::EditOutcome {
            const Sequence* seq = state.findSequence(sequence);
            if (!seq)
                return std::unexpected(Error(ErrorCode::NotFound, "the sequence is gone"));
            std::vector<std::pair<ClipId, EffectInstanceId>> marks;
            for (const ClipId id : clips) {
                const auto loc = seq->findClip(id);
                if (!loc)
                    continue;
                for (const EffectInstance& e : loc->clip().effects()) {
                    const EffectData* d = e.findData(es::kSectionKey);
                    const std::string* s = d ? d->text() : nullptr;
                    if (!s || e.enabled == enabled)
                        continue;
                    if (*s == wanted || s->starts_with(wanted + "."))
                        marks.emplace_back(id, e.id);
                }
            }
            if (marks.empty())
                return std::unexpected(Error(ErrorCode::InvalidArgument, "nothing to change in that section"));
            ProjectState next = state;
            edit::EffectChange change;
            change.enabled = enabled;
            for (const auto& [clip, effect] : marks) {
                auto changed = edit::setEffect(next, sequence, clip, effect, change);
                if (!changed)
                    return std::unexpected(changed.error());
                next = std::move(changed->state);
            }
            edit::EditResult result;
            result.changes = diff(state, next);
            result.state = std::move(next);
            return result;
        });
    refresh();
    return ok ? QString() : m_controller->lastError();
}

void EssentialSoundBridge::cancelJob() {
    m_job.cancel();
    m_previewJob.cancel();
}

// ---- Remix ---------------------------------------------------------------------------------------------------------

QString EssentialSoundBridge::preview(const QString& key, double seconds) {
    const auto id = parseClipKey(key);
    if (!id)
        return tr("Choose a music clip");
    if (!(seconds >= 0.1))
        return tr("Choose a length of at least 0.1 s");
    return startPreview(Json{{"clip", id->value}, {"duration", std::round(seconds * 1000.0) / 1000.0}});
}

QString EssentialSoundBridge::previewFit(const QString& key) {
    const auto id = parseClipKey(key);
    if (!id)
        return tr("Choose a music clip");
    return startPreview(Json{{"clip", id->value}, {"fit", "video"}});
}

QString EssentialSoundBridge::previewSelected(double seconds) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || m_clips.empty())
        return tr("Select a music clip first");
    ClipId pick = m_clips.front();
    for (const ClipId id : m_clips) {
        const auto loc = seq->findClip(id);
        if (loc && es::recordedSoundType(loc->clip().effects()) == es::SoundType::Music) {
            pick = id;
            break;
        }
    }
    return preview(clipKey(pick), seconds);
}

QString EssentialSoundBridge::startPreview(Json params) {
    if (!m_controller)
        return tr("No project");
    if (m_previewJob.running())
        return tr("The music is still being analysed");
    const ProjectSnapshot base = m_controller->snapshot();
    const SequencePtr seq = m_controller->currentSequence();
    const ClipId clip{params["clip"].get<std::uint64_t>()};
    if (!seq || !seq->findClip(clip))
        return tr("Choose a music clip in this sequence");
    params["sequence"] = m_controller->currentSequenceId().value;
    clearRemixPreview();
    const std::uint64_t generation = ++m_remixGeneration;
    m_remixRequest = params;
    m_remix = {{QStringLiteral("clip"), clipKey(clip)},
               {QStringLiteral("busy"), true},
               {QStringLiteral("phase"), tr("Analysing the music")},
               {QStringLiteral("progress"), -1.0},
               {QStringLiteral("joins"), QVariantList()},
               {QStringLiteral("warnings"), QStringList()},
               {QStringLiteral("error"), QString()}};
    if (params.contains("duration"))
        m_remix.insert(QStringLiteral("targetSeconds"), params["duration"].get<double>());
    emit remixChanged();
    QPointer<EssentialSoundBridge> self(this);
    const bool synchronous = m_synchronous;
    auto report = [self, generation, synchronous](QString phase, double progress) {
        if (synchronous) {
            if (self)
                self->setRemixProgress(phase, progress, generation);
            return;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, generation, phase = std::move(phase), progress] {
                if (self)
                    self->setRemixProgress(phase, progress, generation);
            },
            Qt::QueuedConnection);
    };
    const std::shared_ptr<command::CommandServices> svc = services();
    const bool started = m_previewJob.start(this, synchronous, [self, base, svc, params, report, generation](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
        EditSession scratch(*base);
        command::EnginePolicy policy;
        policy.services = svc;
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
        PreviewControl control(cancelled, report);
        command::CommandRequest request;
        request.command = "audio.remix";
        request.params = params;
        request.dryRun = true;
        command::CommandResult result = engine.execute(request, &control);
        return [self, generation, base, result = std::move(result)]() {
            if (self)
                self->finishPreview(result, generation, base);
        };
    });
    if (!started) {
        m_remix.clear();
        m_remixRequest.reset();
        emit remixChanged();
        return tr("The music is still being analysed");
    }
    emit jobChanged();
    return {};
}

void EssentialSoundBridge::setRemixProgress(const QString& phase, double progress, std::uint64_t generation) {
    if (generation != m_remixGeneration || m_remix.isEmpty())
        return;
    if (!phase.isEmpty())
        m_remix.insert(QStringLiteral("phase"), phase);
    m_remix.insert(QStringLiteral("progress"), progress);
    emit remixChanged();
}

void EssentialSoundBridge::finishPreview(const command::CommandResult& result, std::uint64_t generation, const ProjectSnapshot& base) {
    emit jobChanged();
    if (generation != m_remixGeneration || !m_controller)
        return; // cancelled or replaced
    const QString clip = m_remix.value(QStringLiteral("clip")).toString();
    if (!result.ok()) {
        m_remix = {{QStringLiteral("clip"), clip},
                   {QStringLiteral("busy"), false},
                   {QStringLiteral("joins"), QVariantList()},
                   {QStringLiteral("warnings"), QStringList()},
                   {QStringLiteral("error"), commandErrorText(result)}};
        m_remixRequest.reset();
        emit remixChanged();
        return;
    }
    if (m_controller->snapshot() != base) {
        m_remix = {{QStringLiteral("clip"), clip},
                   {QStringLiteral("busy"), false},
                   {QStringLiteral("joins"), QVariantList()},
                   {QStringLiteral("warnings"), QStringList()},
                   {QStringLiteral("error"), tr("The project changed while the music was analysed: preview it again")}};
        m_remixRequest.reset();
        emit remixChanged();
        return;
    }
    const SequencePtr seq = m_controller->currentSequence();
    const auto id = parseClipKey(clip);
    const auto loc = seq && id ? seq->findClip(*id) : std::nullopt;
    const Ticks start = loc ? loc->clip().start() : Ticks{};
    const Json& data = result.data;
    QVariantList joins;
    for (const Json& j : data.value("joins", Json::array())) {
        const std::int64_t atTicks = j.value("atTicks", std::int64_t{0});
        joins.append(QVariantMap{{QStringLiteral("at"), static_cast<qint64>(atTicks)},
                                 {QStringLiteral("seconds"), static_cast<double>(atTicks - start.value) / static_cast<double>(kTicksPerSecond)},
                                 {QStringLiteral("timecode"), QString::fromStdString(j.value("at", std::string()))},
                                 {QStringLiteral("confidence"), jsonNumber(j, "confidence")},
                                 {QStringLiteral("onBar"), j.value("onBar", true)}});
    }
    QStringList warnings;
    for (const std::string& w : result.warnings)
        warnings << QString::fromStdString(w);
    m_remix = {{QStringLiteral("clip"), clip},
               {QStringLiteral("busy"), false},
               {QStringLiteral("targetSeconds"), jsonNumber(data, "targetSeconds")},
               {QStringLiteral("resultSeconds"), jsonNumber(data, "seconds")},
               {QStringLiteral("errorSeconds"), jsonNumber(data, "errorSeconds")},
               {QStringLiteral("bpm"), jsonNumber(data, "bpm")},
               {QStringLiteral("introEnd"), jsonNumber(data, "introEnd")},
               {QStringLiteral("outroStart"), jsonNumber(data, "outroStart")},
               {QStringLiteral("joins"), joins},
               {QStringLiteral("warnings"), warnings},
               {QStringLiteral("error"), QString()}};
    emit remixChanged();
    showRemixPreview(m_remix);
}

void EssentialSoundBridge::showRemixPreview(const QVariantMap& remix) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto id = parseClipKey(remix.value(QStringLiteral("clip")).toString());
    const auto loc = seq && id ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return;
    const Clip& c = loc->clip();
    const Ticks length{static_cast<std::int64_t>(std::llround(remix.value(QStringLiteral("resultSeconds")).toDouble() *
                                                              static_cast<double>(kTicksPerSecond)))};
    TimelineView::ExternalPreview ghost;
    for (const QVariant& j : remix.value(QStringLiteral("joins")).toList())
        ghost.markers.push_back(Ticks{j.toMap().value(QStringLiteral("at")).toLongLong()});
    if (length.value > 0) {
        ghost.added.push_back({loc->track->id, c.start(), c.start() + length});
        if (c.start() + length < c.end())
            ghost.removed.push_back({loc->track->id, c.start() + length, c.end()});
    }
    for (TimelineView* view : timelineViews(m_controller))
        view->setExternalPreview(ghost);
    m_previewShown = true;
}

void EssentialSoundBridge::clearRemixPreview() {
    if (!m_previewShown)
        return;
    m_previewShown = false;
    for (TimelineView* view : timelineViews(m_controller))
        view->clearExternalPreview();
}

QString EssentialSoundBridge::commitRemix(bool take) {
    if (!m_controller)
        return tr("No project");
    if (!m_remixRequest || m_remix.value(QStringLiteral("busy")).toBool() ||
        !m_remix.value(QStringLiteral("error")).toString().isEmpty())
        return tr("Preview a remix first");
    if (m_job.running())
        return tr("Essential Sound is still working");
    // The rendered take adds a media item: its own command (an EditProject edit since A.8), same parameters.
    const char* command = take ? "audio.remixRender" : "audio.remix";
    Json params = *m_remixRequest;
    // The preview goes now: the result replaces it on the track.
    m_remixRequest.reset();
    m_remix.clear();
    ++m_remixGeneration;
    clearRemixPreview();
    emit remixChanged();
    setMessage({});
    setJob(take ? tr("Rendering the remix") : tr("Remixing the music"), -1);
    QPointer<EssentialSoundBridge> self(this);
    const bool started = m_job.start(
        this, m_controller, services(), {command, std::move(params), take ? "Remix Music (Take)" : "Remix Music"},
        [self](const QString& phase, double progress) {
            if (self)
                self->setJob(phase, progress);
        },
        [self](const command::CommandResult& result) {
            if (!self)
                return;
            self->setJob({}, 0);
            if (!result.ok())
                self->setMessage(commandErrorText(result));
            else
                self->setMessage(tr("Remixed to %1 s").arg(QLocale().toString(jsonNumber(result.data, "seconds"), 'f', 1)));
            self->refresh();
        });
    if (!started) {
        setJob({}, 0);
        return tr("Essential Sound is still working");
    }
    return {};
}

QString EssentialSoundBridge::apply() {
    return commitRemix(false);
}

QString EssentialSoundBridge::renderTake() {
    return commitRemix(true);
}

void EssentialSoundBridge::cancel() {
    if (m_previewJob.running())
        m_previewJob.cancel();
    ++m_remixGeneration;
    m_remixRequest.reset();
    clearRemixPreview();
    if (!m_remix.isEmpty()) {
        m_remix.clear();
        emit remixChanged();
    }
}

void EssentialSoundBridge::onRevisionChanged() {
    refresh();
    // Any edit makes a shown preview stale (the result of a commit replaces it anyway).
    if (!m_remix.isEmpty() && !m_remix.value(QStringLiteral("busy")).toBool())
        cancel();
}

} // namespace wabcut::app
