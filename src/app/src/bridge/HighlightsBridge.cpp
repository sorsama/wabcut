#include "HighlightsBridge.h"

#include "AgentController.h"
#include "BridgeIds.h"
#include "ExportController.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ShortsBridge.h" // BridgeJob, setPreviewFrameContext, image://wabcut-frames

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditSession.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QUrl>

#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Resolve.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <span>

namespace wabcut::app {

namespace {

using Json = command::Json;

constexpr int kWaveformBins = 32;
/// The signals highlights.find measures, in the order the cards show them.
constexpr std::array<const char*, 5> kFindSignals{"game", "mic", "visual", "chat", "markers"};
constexpr std::array<const char*, 5> kSignalKeys{"game", "mic", "visual", "chat", "hint"};

QVariant toVariant(const Json& value) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(Json{{"v", value}}.dump())).toVariant().toMap().value(
        QStringLiteral("v"));
}

Json toJson(const QVariantMap& map) {
    return Json::parse(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact).toStdString());
}

double num(const Json& j, const char* key, double fallback = 0.0) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

QString str(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? QString::fromStdString(it->get<std::string>()) : QString();
}

double clampTo(double v, double lo, double hi) {
    if (!std::isfinite(v))
        return lo;
    return std::clamp(v, lo, hi);
}

Ticks ticksOf(double seconds) {
    return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
}

/// "1:23.2" (or "1:02:03.5" past an hour).
QString clockText(double seconds) {
    seconds = std::max(0.0, seconds);
    const auto tenths = static_cast<std::int64_t>(std::llround(seconds * 10.0));
    const std::int64_t h = tenths / 36000;
    const std::int64_t m = (tenths / 600) % 60;
    const double s = static_cast<double>(tenths % 600) / 10.0;
    const QString sec = QStringLiteral("%1").arg(s, 4, 'f', 1, QLatin1Char('0'));
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(sec)
                 : QStringLiteral("%1:%2").arg(m).arg(sec);
}

QString signalLabel(const QString& key) {
    if (key == QLatin1String("game"))
        return QCoreApplication::translate("wabcut::app::HighlightsBridge", "Game audio");
    if (key == QLatin1String("mic"))
        return QCoreApplication::translate("wabcut::app::HighlightsBridge", "Your voice");
    if (key == QLatin1String("visual"))
        return QCoreApplication::translate("wabcut::app::HighlightsBridge", "On screen");
    if (key == QLatin1String("chat"))
        return QCoreApplication::translate("wabcut::app::HighlightsBridge", "Chat");
    if (key == QLatin1String("hint"))
        return QCoreApplication::translate("wabcut::app::HighlightsBridge", "Marker");
    return key;
}

/// The frame shown at timeline time `t`: the top enabled video clip's media and source time.
std::optional<std::pair<MediaId, Ticks>> pictureAt(const ProjectState& state, const Sequence& seq, Ticks t) {
    for (auto it = seq.videoTracks.rbegin(); it != seq.videoTracks.rend(); ++it) {
        if (!(*it)->enabled)
            continue;
        for (const Clip& clip : (*it)->clips) {
            if (!clip.enabled || t < clip.start() || clip.end() <= t)
                continue;
            if (clip.isNested()) {
                // 0.2: a nested or multicam clip (a gaming nest) shows its top leaf: the active angle, the nest's top clip.
                const std::vector<ResolvedLeaf> leaves = resolveLeaves(state, seq.id, clip, t);
                for (auto leaf = leaves.rbegin(); leaf != leaves.rend(); ++leaf) {
                    const MediaItem* media = state.findMedia(leaf->clip->media);
                    if (media && !media->isGenerator() && !media->info.video.empty())
                        return std::pair{leaf->clip->media, leaf->sourceTime};
                }
                continue;
            }
            const MediaItem* media = state.findMedia(clip.media);
            if (media && !media->isGenerator() && !media->info.video.empty())
                return std::pair{clip.media, clip.sourceTimeAt(t)};
        }
    }
    return std::nullopt;
}

/// The loudest audible clip's peaks across [start, end) in `bins` slices (0..1); empty when nothing is audible.
QVariantList waveformOf(const ProjectState& state, const Sequence& seq, const PeakSource* peaks, Ticks start, Ticks end,
                        int bins) {
    QVariantList out;
    if (!peaks || end <= start || bins <= 0)
        return out;
    bool any = false;
    std::vector<float> values(static_cast<std::size_t>(bins), 0.0f);
    const std::int64_t span = end.value - start.value;
    for (int i = 0; i < bins; ++i) {
        const Ticks t0{start.value + span * i / bins};
        const Ticks t1{start.value + span * (i + 1) / bins};
        const Ticks mid{(t0.value + t1.value) / 2};
        for (const TrackPtr& track : seq.audioTracks) {
            if (!track->enabled || track->muted)
                continue;
            for (const Clip& clip : track->clips) {
                if (!clip.enabled || clip.isNested() || mid < clip.start() || clip.end() <= mid)
                    continue;
                const MediaItem* media = state.findMedia(clip.media);
                if (!media || media->isGenerator())
                    continue;
                const Ticks a = std::max(t0, clip.start());
                const Ticks b = std::min(t1, clip.end());
                float value = 0.0f;
                Ticks s0 = clip.sourceTimeAt(a);
                Ticks s1 = clip.sourceTimeAt(b);
                if (s1 < s0)
                    std::swap(s0, s1); // reversed clips
                if (s1 > s0 && peaks->peaks(clip.media, clip.stream, s0, s1, std::span<float>(&value, 1))) {
                    values[static_cast<std::size_t>(i)] = std::max(values[static_cast<std::size_t>(i)], value);
                    any = true;
                }
            }
        }
    }
    if (!any)
        return out;
    for (const float v : values)
        out.append(std::clamp(static_cast<double>(v), 0.0, 1.0));
    return out;
}

/// Cancels through `cancelled`; reports "measuring 'VOD.mkv' 42%" (or "... (42%)") checkpoints as activity and progress.
class FindControl final : public command::ExecutionControl {
public:
    FindControl(const std::atomic<bool>& cancelled, std::function<void(QString, double)> report)
        : m_cancelled(cancelled), m_report(std::move(report)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        static const QRegularExpression percent(QStringLiteral("\\(?\\s*(\\d{1,3}(?:\\.\\d+)?)\\s*%\\s*\\)?\\s*$"));
        QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())).trimmed();
        double progress = -1.0;
        if (const auto match = percent.match(text); match.hasMatch()) {
            progress = std::clamp(match.captured(1).toDouble() / 100.0, 0.0, 1.0);
            text = text.left(match.capturedStart()).trimmed();
        }
        if (!text.isEmpty())
            text[0] = text[0].toUpper();
        m_report(text, progress);
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

command::CommandResult refused(command::ErrorCode code, std::string message) {
    command::CommandResult result;
    result.error = command::CommandError{};
    result.error->code = code;
    result.error->message = std::move(message);
    return result;
}

QString errorText(const command::CommandResult& result) {
    if (result.ok())
        return {};
    if (result.error->code == command::ErrorCode::Cancelled)
        return QCoreApplication::translate("wabcut::app::HighlightsBridge", "Stopped. Nothing was changed.");
    return QString::fromStdString(result.error->message);
}

QStringList joinedWarnings(const std::vector<std::string>& warnings) {
    QStringList out;
    for (const std::string& w : warnings)
        out << QString::fromStdString(w);
    return out;
}

} // namespace

HighlightsBridge::HighlightsBridge(QObject* parent) : QObject(parent), m_job(std::make_unique<BridgeJob>()) {
    m_peaksTimer.setSingleShot(true);
    m_peaksTimer.setInterval(250);
    connect(&m_peaksTimer, &QTimer::timeout, this, &HighlightsBridge::refreshWaveforms);
}

HighlightsBridge::~HighlightsBridge() = default;

HighlightsBridge* HighlightsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new HighlightsBridge;
    if (qmlEngine) {
        (void)qmlEngine->singletonInstance<ShortsBridge*>("Wabcut", "Shorts"); // registers image://wabcut-frames
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
        // The assistant's services (analysis caches, the audio and picture readers); everything runs locally.
        QPointer<QQmlEngine> engine(qmlEngine);
        bridge->setServicesFactory([engine]() -> std::shared_ptr<command::CommandServices> {
            auto* agent = engine ? engine->singletonInstance<AgentController*>("Wabcut", "AgentController") : nullptr;
            return agent ? agent->commandServices() : nullptr;
        });
        // 0.4: Export all queues through the engine's ExportController (as ShortsBridge::exportAll does).
        bridge->setQueueExport([engine](const QString& sequence, const QString& preset, const QVariantMap& overrides) {
            auto* exporter = engine ? engine->singletonInstance<ExportController*>("Wabcut", "ExportController") : nullptr;
            return exporter ? exporter->queueExport(sequence, preset, overrides) : QString();
        });
    }
    return bridge;
}

void HighlightsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::playheadChanged, this, &HighlightsBridge::onPlayhead);
        connect(controller, &ProjectController::peaksChanged, this, [this] {
            if (!m_moments.empty())
                m_peaksTimer.start();
        });
        // The highlight markers are the list: a sequence switch, an undo or redo, another project list them again
        // (markersChanged also comes for a switch and a reset; nothing is rebuilt when the markers are the same).
        connect(controller, &ProjectController::markersChanged, this, [this] { refreshFromMarkers(); });
        connect(controller, &ProjectController::sequencesChanged, this, [this] {
            refreshFromMarkers();
            emit madeShortsChanged(); // 0.4: an undo or a delete takes made Shorts away
        });
    }
    refreshFromMarkers(true);
}

void HighlightsBridge::setPlayback(PlaybackBridge* playback) {
    if (m_playback)
        disconnect(m_playback, nullptr, this, nullptr);
    m_playback = playback;
    if (playback) {
        // Stopped from the monitor or the keyboard: the preview is over.
        connect(playback, &PlaybackBridge::transportChanged, this, [this] {
            if (m_previewRank != 0 && m_playback && !m_playback->playing()) {
                m_previewRank = 0;
                emit previewChanged();
            }
        });
    }
}

void HighlightsBridge::setRegistry(const command::CommandRegistry* registry) {
    m_registry = registry;
    emit catalogChanged();
}

const command::CommandRegistry& HighlightsBridge::registry() const {
    return m_registry ? *m_registry : ai::edit::editingCommands();
}

bool HighlightsBridge::available() const {
    return registry().knows("highlights.find");
}

bool HighlightsBridge::reelAvailable() const {
    return registry().knows("highlights.toReel");
}

bool HighlightsBridge::shortAvailable() const {
    return registry().knows("highlights.toShort");
}

bool HighlightsBridge::busy() const {
    return m_job->running();
}

int HighlightsBridge::acceptedCount() const {
    return static_cast<int>(std::count_if(m_moments.begin(), m_moments.end(),
                                          [](const Moment& m) { return m.review == Review::Accepted; }));
}

int HighlightsBridge::reelCount() const {
    return static_cast<int>(reelMoments().size());
}

void HighlightsBridge::setSelectedRank(int rank) {
    if (rank != 0 && !findMoment(rank))
        return;
    if (m_selectedRank == rank)
        return;
    m_selectedRank = rank;
    emit selectedRankChanged();
}

bool HighlightsBridge::chatAvailable() const {
    return registry().knows("highlights.importChat");
}

QString HighlightsBridge::chatFileName() const {
    return m_chatFile.isEmpty() ? QString() : QFileInfo(m_chatFile).fileName();
}

QString HighlightsBridge::chatSummary() const {
    if (m_chat.isEmpty())
        return {};
    const QString format = m_chat.value(QStringLiteral("format")).toString();
    const QString kind = format == QLatin1String("twitch")    ? tr("Twitch chat")
                         : format == QLatin1String("youtube") ? tr("YouTube chat")
                         : format.isEmpty()                   ? tr("Chat")
                                                              : tr("Chat (%1)").arg(format);
    QStringList parts{kind, tr("Messages: %1").arg(QLocale().toString(m_chat.value(QStringLiteral("messages")).toLongLong()))};
    if (const QString name = m_chat.value(QStringLiteral("name")).toString(); !name.isEmpty())
        parts << name;
    return parts.join(QStringLiteral(" · "));
}

void HighlightsBridge::setChatOffset(double seconds) {
    seconds = clampTo(seconds, -86400, 86400);
    if (seconds == m_chatOffset)
        return;
    m_chatOffset = seconds;
    emit chatChanged();
}

QStringList HighlightsBridge::chatFileFilters() const {
    return {tr("Chat logs (*.json *.jsonl)"), tr("All files (*)")};
}

void HighlightsBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

void HighlightsBridge::setJob(const QString& kind, const QString& activity, double progress) {
    m_jobKind = kind;
    m_activity = activity;
    m_progress = progress;
    emit jobChanged();
}

const HighlightsBridge::Moment* HighlightsBridge::findMoment(int rank) const {
    const auto it = std::find_if(m_moments.begin(), m_moments.end(), [rank](const Moment& m) { return m.rank == rank; });
    return it == m_moments.end() ? nullptr : &*it;
}

HighlightsBridge::Moment* HighlightsBridge::findMoment(int rank) {
    const auto it = std::find_if(m_moments.begin(), m_moments.end(), [rank](const Moment& m) { return m.rank == rank; });
    return it == m_moments.end() ? nullptr : &*it;
}

// --- Parameters ---------------------------------------------------------------------------------------------------------

QVariantMap HighlightsBridge::findParams(const QVariantMap& options) const {
    QVariantMap p;
    if (m_controller)
        p.insert(QStringLiteral("sequence"), static_cast<qulonglong>(m_controller->currentSequenceId().value));
    if (options.contains(QStringLiteral("signals"))) {
        const QStringList asked = options.value(QStringLiteral("signals")).toStringList();
        QStringList chosen;
        for (const char* known : kFindSignals) {
            if (asked.contains(QString::fromLatin1(known)))
                chosen << QString::fromLatin1(known);
        }
        p.insert(QStringLiteral("signals"), chosen);
    }
    if (options.contains(QStringLiteral("count")))
        p.insert(QStringLiteral("count"), static_cast<int>(clampTo(options.value(QStringLiteral("count")).toDouble(), 1, 200)));
    if (options.contains(QStringLiteral("minScore")))
        p.insert(QStringLiteral("minScore"), clampTo(options.value(QStringLiteral("minScore")).toDouble(), 0, 1));
    if (options.contains(QStringLiteral("minGap")))
        p.insert(QStringLiteral("minGap"), clampTo(options.value(QStringLiteral("minGap")).toDouble(), 0, 3600));
    // The chat log was imported with its offset (highlights.importChat); a changed offset overrides it here.
    if (options.contains(QStringLiteral("chatOffset")))
        p.insert(QStringLiteral("chatOffset"), clampTo(options.value(QStringLiteral("chatOffset")).toDouble(), -86400, 86400));
    else if (!m_chat.isEmpty() && m_chat.value(QStringLiteral("offset")).toDouble() != m_chatOffset)
        p.insert(QStringLiteral("chatOffset"), m_chatOffset);
    if (options.contains(QStringLiteral("chatDelay")))
        p.insert(QStringLiteral("chatDelay"), clampTo(options.value(QStringLiteral("chatDelay")).toDouble(), 0, 120));
    if (options.contains(QStringLiteral("keywords"))) {
        QStringList words;
        for (const QString& w : options.value(QStringLiteral("keywords")).toStringList()) {
            if (!w.trimmed().isEmpty())
                words << w.trimmed();
        }
        if (!words.isEmpty())
            p.insert(QStringLiteral("keywords"), words);
    }
    return p;
}

QVariantList HighlightsBridge::reelMoments() const {
    const bool anyAccepted = acceptedCount() > 0;
    std::vector<const Moment*> chosen;
    for (const Moment& m : m_moments) {
        if (anyAccepted ? m.review == Review::Accepted : m.review != Review::Rejected)
            chosen.push_back(&m);
    }
    std::stable_sort(chosen.begin(), chosen.end(), [](const Moment* a, const Moment* b) { return a->start < b->start; });
    QVariantList out;
    for (const Moment* m : chosen) {
        out.append(QVariantMap{{QStringLiteral("start"), m->start},
                               {QStringLiteral("end"), m->end},
                               {QStringLiteral("peak"), m->peak},
                               {QStringLiteral("score"), m->score},
                               {QStringLiteral("title"), m->title}});
    }
    return out;
}

QVariantMap HighlightsBridge::reelParams(const QVariantMap& options) const {
    QVariantMap p;
    p.insert(QStringLiteral("sequence"), static_cast<qulonglong>(m_sequence.value));
    p.insert(QStringLiteral("moments"), reelMoments());
    const QString order = options.value(QStringLiteral("order")).toString();
    p.insert(QStringLiteral("order"), order == QLatin1String("score") ? order : QStringLiteral("time"));
    if (options.contains(QStringLiteral("count")))
        p.insert(QStringLiteral("count"), static_cast<int>(clampTo(options.value(QStringLiteral("count")).toDouble(), 1, 50)));
    if (const double max = options.value(QStringLiteral("maxDuration")).toDouble(); std::isfinite(max) && max > 0)
        p.insert(QStringLiteral("maxDuration"), std::min(max, 3600.0));
    const QString transition = options.value(QStringLiteral("transition")).toString();
    if (transition == QLatin1String("crossDissolve") || transition == QLatin1String("dipToBlack") ||
        transition == QLatin1String("dipToWhite") || transition == QLatin1String("none"))
        p.insert(QStringLiteral("transition"), transition);
    if (const auto music = detail::parseId<MediaId>(QLatin1Char('m'), options.value(QStringLiteral("music")).toString()))
        p.insert(QStringLiteral("music"), static_cast<qulonglong>(music->value));
    if (options.contains(QStringLiteral("slowMotion")))
        p.insert(QStringLiteral("slowMotion"), options.value(QStringLiteral("slowMotion")).toBool());
    if (const QString name = options.value(QStringLiteral("name")).toString().trimmed(); !name.isEmpty())
        p.insert(QStringLiteral("name"), name);
    return p;
}

QVariantMap HighlightsBridge::shortParams(int rank, const QVariantMap& options) const {
    const Moment* m = findMoment(rank);
    if (!m)
        return {};
    QVariantMap p;
    p.insert(QStringLiteral("sequence"), static_cast<qulonglong>(m_sequence.value));
    p.insert(QStringLiteral("start"), m->start);
    p.insert(QStringLiteral("end"), m->end);
    p.insert(QStringLiteral("title"), m->title);
    // The layout (layout.apply's names and ranges); auto picks the split when there is a facecam.
    const QString layout = options.value(QStringLiteral("layout")).toString();
    if (layout == QLatin1String("auto") || layout == QLatin1String("verticalSplit") ||
        layout == QLatin1String("verticalBlurFill"))
        p.insert(QStringLiteral("layout"), layout);
    if (const auto facecam = parseClipKey(options.value(QStringLiteral("facecam")).toString()))
        p.insert(QStringLiteral("facecam"), static_cast<qulonglong>(facecam->value));
    // The webcam inside the capture (the layout picker's region), in fractions of the gameplay frame.
    if (const QVariantMap region = options.value(QStringLiteral("facecamRegion")).toMap(); !region.isEmpty()) {
        const double x = clampTo(region.value(QStringLiteral("x")).toDouble(), 0, 1);
        const double y = clampTo(region.value(QStringLiteral("y")).toDouble(), 0, 1);
        const double w = clampTo(region.value(QStringLiteral("width")).toDouble(), 0, 1 - x);
        const double h = clampTo(region.value(QStringLiteral("height")).toDouble(), 0, 1 - y);
        if (w >= 0.01 && h >= 0.01)
            p.insert(QStringLiteral("facecamRegion"), QVariantMap{{QStringLiteral("x"), x},
                                                                  {QStringLiteral("y"), y},
                                                                  {QStringLiteral("width"), w},
                                                                  {QStringLiteral("height"), h}});
    }
    if (options.contains(QStringLiteral("split")))
        p.insert(QStringLiteral("split"), clampTo(options.value(QStringLiteral("split")).toDouble(), 0.2, 0.8));
    if (options.contains(QStringLiteral("facecamOnTop")))
        p.insert(QStringLiteral("facecamOnTop"), options.value(QStringLiteral("facecamOnTop")).toBool());
    if (options.contains(QStringLiteral("blur")))
        p.insert(QStringLiteral("blur"), clampTo(options.value(QStringLiteral("blur")).toDouble(), 0, 500));
    if (options.contains(QStringLiteral("zoom")))
        p.insert(QStringLiteral("zoom"), clampTo(options.value(QStringLiteral("zoom")).toDouble(), 1, 4));
    for (const char* flag : {"captions", "hookTitle", "normalize"}) {
        const QString key = QString::fromLatin1(flag);
        if (options.contains(key))
            p.insert(key, options.value(key).toBool());
    }
    // 0.4: the caption style (a preset's name; "" = the default look), with captions (on unless switched off).
    const bool captions = !options.contains(QStringLiteral("captions")) || options.value(QStringLiteral("captions")).toBool();
    if (captions && options.contains(QStringLiteral("captionPreset")))
        p.insert(QStringLiteral("captionPreset"), options.value(QStringLiteral("captionPreset")).toString().trimmed().left(64));
    // 0.4: lead-in and tail (the build-up before the moment, the reaction after; 0..30 s each), and "At least": a Short
    // shorter than minLength gets the missing time two thirds before, one third after (each side at most 30 s: what one
    // side cannot take goes to the other). A side that runs past the sequence's start or end gives what it cannot use
    // to the other side.
    double before = options.contains(QStringLiteral("handleBefore"))
                        ? clampTo(options.value(QStringLiteral("handleBefore")).toDouble(), 0, 30)
                        : 0.0;
    double after = options.contains(QStringLiteral("handleAfter"))
                       ? clampTo(options.value(QStringLiteral("handleAfter")).toDouble(), 0, 30)
                       : 0.0;
    const double minLength = options.contains(QStringLiteral("minLength"))
                                 ? clampTo(options.value(QStringLiteral("minLength")).toDouble(), 0, 180)
                                 : 0.0;
    const double length = m->end - m->start;
    if (const double missing = minLength - (length + before + after); missing > 0) {
        before += missing * 2.0 / 3.0;
        after += missing / 3.0;
        // Fix round 1: the 30 s cap's overflow is not dropped (At least 60 s on a 4 s moment with 3 s / 1 s gave 52 s).
        if (before > 30.0) {
            after = std::min(30.0, after + (before - 30.0));
            before = 30.0;
        }
        if (after > 30.0) {
            before = std::min(30.0, before + (after - 30.0));
            after = 30.0;
        }
    }
    double roomAfter = 30.0;
    if (const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : ProjectSnapshot{}) {
        if (const Sequence* seq = snap->findSequence(m_sequence)) {
            const Ticks end = std::max(seq->duration, seq->contentEnd());
            roomAfter = std::max(0.0, end.toSeconds() - m->end);
        }
    }
    const double roomBefore = std::max(0.0, m->start);
    if (before > roomBefore) {
        after = std::min({30.0, roomAfter, after + (before - roomBefore)});
        before = roomBefore;
    }
    if (after > roomAfter) {
        before = std::min({30.0, roomBefore, before + (after - roomAfter)});
        after = roomAfter;
    }
    const auto millis = [](double s) { return std::round(s * 1000.0) / 1000.0; };
    if (millis(before) > 0)
        p.insert(QStringLiteral("handleBefore"), millis(before));
    if (millis(after) > 0)
        p.insert(QStringLiteral("handleAfter"), millis(after));
    return p;
}

QVariantList HighlightsBridge::musicChoices() const {
    QVariantList out;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : ProjectSnapshot{};
    if (!snap)
        return out;
    for (const MediaPtr& media : snap->media) {
        if (media->isGenerator() || media->info.audio.empty() || !media->info.video.empty())
            continue;
        out.append(QVariantMap{{QStringLiteral("id"), mediaKey(media->id)},
                               {QStringLiteral("name"), QString::fromStdString(media->name)}});
    }
    return out;
}

// --- Jobs ---------------------------------------------------------------------------------------------------------------

QString HighlightsBridge::runJob(const QString& kind, const QString& startText, const std::string& name,
                                 const QVariantMap& params, bool staleIsError, Done done) {
    // One request of runJobs (0.4): the same steps, its one result.
    command::CommandRequest request;
    request.command = name;
    request.params = toJson(params);
    return runJobs(kind, startText, {std::move(request)}, QString(), staleIsError,
                   [done = std::move(done)](const std::vector<command::CommandResult>& results, bool stale, bool) {
                       if (!done)
                           return;
                       done(results.empty() ? refused(command::ErrorCode::Cancelled, "cancelled") : results.front(), stale);
                   });
}

QString HighlightsBridge::runJobs(const QString& kind, const QString& startText, std::vector<command::CommandRequest> requests,
                                  const QString& label, bool staleIsError, DoneMany done) {
    if (!m_controller)
        return tr("No project is open");
    if (m_job->running())
        return tr("Busy");
    if (requests.empty())
        return tr("Nothing to do");
    const ProjectSnapshot base = m_controller->snapshot();
    const command::CommandRegistry* catalog = &registry();
    std::shared_ptr<command::CommandServices> services = m_servicesFactory ? m_servicesFactory() : nullptr;
    const std::string commitLabel = label.toStdString();
    //: The batch's activity: "Short 2 of 6", then ": <what it does now>"
    const QString stepFormat = tr("Short %1 of %2");
    const unsigned generation = ++m_generation;
    const bool synchronous = m_synchronous;
    QPointer<HighlightsBridge> self(this);
    QPointer<ProjectController> live(m_controller);
    // Checkpoints reach the GUI thread queued (directly when synchronous); late ones of a finished job are dropped.
    auto report = [self, kind, generation, synchronous](QString text, double progress) {
        auto apply = [self, kind, generation, text = std::move(text), progress] {
            if (self && self->m_generation == generation && self->m_jobKind == kind)
                self->setJob(kind, text.isEmpty() ? self->m_activity : text, progress);
        };
        if (synchronous)
            apply();
        else
            QMetaObject::invokeMethod(QCoreApplication::instance(), std::move(apply), Qt::QueuedConnection);
    };
    setMessage(QString());
    setJob(kind, startText, -1.0);
    const bool started = m_job->start(
        this, synchronous,
        [base, catalog, services, requests = std::move(requests), report, self, live, staleIsError, done, commitLabel,
         stepFormat](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
            // On the worker: a copy of the project, every request on it in turn.
            EditSession scratch(*base);
            command::EnginePolicy policy;
            policy.services = services;
            command::CommandEngine engine(scratch, *catalog, policy);
            const std::size_t count = requests.size();
            std::vector<command::CommandResult> results;
            ProjectSnapshot kept = scratch.snapshot(); // the copy after the last step that is kept
            bool stopped = false;
            for (std::size_t i = 0; i < count; ++i) {
                // Cancel between steps (the first runs as a single job always did: its checkpoints say Cancelled).
                if (i > 0 && cancelled.load()) {
                    stopped = true;
                    break;
                }
                std::function<void(QString, double)> stepReport = report;
                if (count > 1) {
                    // "Short 2 of 6: <checkpoint>", progress over the whole batch.
                    const QString prefix = stepFormat.arg(i + 1).arg(count);
                    const double stepStart = static_cast<double>(i) / static_cast<double>(count);
                    report(prefix, stepStart);
                    stepReport = [report, prefix, stepStart, count](QString text, double progress) {
                        report(text.isEmpty() ? prefix : prefix + QStringLiteral(": ") + text,
                               progress < 0 ? stepStart : stepStart + progress / static_cast<double>(count));
                    };
                }
                FindControl control(cancelled, stepReport);
                command::CommandResult result = engine.execute(requests[i], &control);
                if (count > 1 && cancelled.load()) {
                    // Cancelled while this step ran: the Short being made is dropped, the ones before are kept.
                    results.push_back(refused(command::ErrorCode::Cancelled, "cancelled"));
                    stopped = true;
                    break;
                }
                const bool cancelledHere = !result.ok() && result.error->code == command::ErrorCode::Cancelled;
                if (result.ok())
                    kept = scratch.snapshot();
                results.push_back(std::move(result));
                if (cancelledHere) {
                    stopped = true;
                    break;
                }
            }
            const ProjectSnapshot after = kept;
            const std::string label = !commitLabel.empty() ? commitLabel : scratch.undoLabel().value_or(std::string());
            return [self, live, base, after, label, staleIsError, done, stopped, results = std::move(results)]() mutable {
                // Back on the GUI thread: the copy's result is one undo step when nothing changed meanwhile; a project
                // that changed is never edited behind the user's edits.
                bool stale = false;
                const bool anyOk = std::ranges::any_of(results, [](const command::CommandResult& r) { return r.ok(); });
                const auto refuseAll = [&](command::ErrorCode code, const std::string& message) {
                    for (command::CommandResult& r : results) {
                        if (r.ok())
                            r = refused(code, message);
                    }
                };
                if (anyOk && after.get() != base.get()) {
                    if (!live) {
                        refuseAll(command::ErrorCode::InvalidRequest, "the project was closed");
                    } else if (live->snapshot().get() != base.get()) {
                        stale = true;
                        if (staleIsError)
                            refuseAll(command::ErrorCode::StaleRevision,
                                      QCoreApplication::translate("wabcut::app::HighlightsBridge",
                                                                  "The project changed while this ran. Run it again.")
                                          .toStdString());
                    } else {
                        // done() reads the new markers itself (with the result's signals), once.
                        if (self)
                            self->m_committing = true;
                        auto committed = live->session().commit(label, *after);
                        if (self)
                            self->m_committing = false;
                        if (!committed)
                            refuseAll(command::ErrorCode::EditRejected, committed.error().message);
                    }
                }
                if (!self)
                    return;
                self->setJob(QString(), QString(), -1.0);
                if (done)
                    done(results, stale, stopped);
                self->refreshFromMarkers();
            };
        },
        // The command threw: the job ends with its message (nothing is committed).
        [self](const QString& message) -> BridgeJob::Finish {
            return [self, message] {
                if (!self)
                    return;
                self->setJob(QString(), QString(), -1.0);
                self->setMessage(message.isEmpty() ? tr("Something went wrong. Nothing was changed.") : message);
            };
        });
    if (!started) {
        setJob(QString(), QString(), -1.0);
        return tr("Busy");
    }
    return {};
}

QString HighlightsBridge::find(const QVariantMap& options) {
    if (!available())
        return tr("Highlights arrive with the next engine update");
    if (!m_controller)
        return tr("No project is open");
    const QVariantMap params = findParams(options);
    if (params.contains(QStringLiteral("signals")) && params.value(QStringLiteral("signals")).toStringList().isEmpty()) {
        const QString why = tr("Pick at least one signal to listen for");
        setMessage(why);
        return why;
    }
    stopPreview();
    const SequenceId sequence = m_controller->currentSequenceId();
    QPointer<HighlightsBridge> self(this);
    const QString error = runJob(QStringLiteral("find"), tr("Starting"), "highlights.find", params, false,
                                 [self, sequence](const command::CommandResult& result, bool stale) {
                                     if (!self)
                                         return;
                                     if (!result.ok()) {
                                         self->setMessage(errorText(result));
                                         return;
                                     }
                                     self->takeFindResult(result, sequence);
                                     if (stale)
                                         self->setMessage(tr("The project changed while highlights were found, so their "
                                                             "markers were not added. Find again to add them."));
                                 });
    if (!error.isEmpty())
        setMessage(error);
    return error;
}

void HighlightsBridge::cancel() {
    if (m_job->running())
        m_job->cancel();
}

QString HighlightsBridge::importChat(const QString& fileOrUrl) {
    if (!chatAvailable())
        return tr("Chat logs arrive with the next engine update");
    if (!m_controller)
        return tr("No project is open");
    QString path = fileOrUrl.trimmed();
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    if (path.isEmpty()) {
        const QString why = tr("Choose a chat log file");
        setMessage(why);
        return why;
    }
    path = QFileInfo(path).absoluteFilePath();
    const QVariantMap params{{QStringLiteral("sequence"), static_cast<qulonglong>(m_controller->currentSequenceId().value)},
                             {QStringLiteral("file"), path},
                             {QStringLiteral("offset"), m_chatOffset}};
    QPointer<HighlightsBridge> self(this);
    // Nothing in the project changes (the log is kept with the recording's analyses): no undo step.
    const QString error = runJob(QStringLiteral("chat"), tr("Reading the chat log"), "highlights.importChat", params, false,
                                 [self, path](const command::CommandResult& result, bool) {
                                     if (!self)
                                         return;
                                     if (!result.ok()) {
                                         self->setMessage(errorText(result));
                                         return;
                                     }
                                     self->m_chatFile = path;
                                     self->m_chat = result.data.is_object() ? toVariant(result.data).toMap() : QVariantMap{};
                                     if (!self->m_chat.contains(QStringLiteral("offset")))
                                         self->m_chat.insert(QStringLiteral("offset"), self->m_chatOffset);
                                     self->m_chatOffset = self->m_chat.value(QStringLiteral("offset")).toDouble();
                                     emit self->chatChanged();
                                     QStringList lines{tr("Chat log read: %1").arg(self->chatSummary())};
                                     lines << joinedWarnings(result.warnings);
                                     self->setMessage(lines.join(QLatin1Char('\n')));
                                 });
    if (!error.isEmpty())
        setMessage(error);
    return error;
}

void HighlightsBridge::takeFindResult(const command::CommandResult& result, SequenceId sequence) {
    const Json& data = result.data;
    if (data.contains("sequence") && data["sequence"].is_number_unsigned())
        sequence = SequenceId{data["sequence"].get<std::uint64_t>()};
    // What the markers do not keep: the measured signals and the numeric confidence, by rank and start.
    m_extras.clear();
    m_findSequence = sequence;
    if (const auto it = data.find("moments"); it != data.end() && it->is_array()) {
        for (const Json& j : *it) {
            if (!j.is_object())
                continue;
            FindExtra extra;
            extra.rank = static_cast<int>(num(j, "rank"));
            extra.start = num(j, "start");
            extra.confidence = clampTo(num(j, "confidence"), 0, 1);
            // The measured signals, in a fixed order; keys the panel does not know follow.
            if (const auto s = j.find("signals"); s != j.end() && s->is_object()) {
                QStringList keys;
                for (const char* k : kSignalKeys)
                    keys << QString::fromLatin1(k);
                for (auto e = s->begin(); e != s->end(); ++e) {
                    if (!keys.contains(QString::fromStdString(e.key())))
                        keys << QString::fromStdString(e.key());
                }
                for (const QString& key : keys) {
                    const auto v = s->find(key.toStdString());
                    if (v == s->end() || !v->is_number())
                        continue;
                    extra.signalBars.append(QVariantMap{{QStringLiteral("key"), key},
                                                        {QStringLiteral("label"), signalLabel(key)},
                                                        {QStringLiteral("value"), clampTo(v->get<double>(), 0, 1)}});
                }
            }
            m_extras.push_back(std::move(extra));
        }
    }
    m_recordings = data.contains("recordings") ? toVariant(data["recordings"]).toList() : QVariantList{};
    m_warnings = joinedWarnings(result.warnings);
    m_findChatMessages = 0;
    if (const auto chat = data.find("chat"); chat != data.end() && chat->is_object())
        m_findChatMessages = static_cast<int>(num(*chat, "messages"));
    // A new verdict: the earlier review of this sequence goes.
    for (auto it = m_reviews.begin(); it != m_reviews.end();)
        it = std::get<0>(it->first) == sequence.value ? m_reviews.erase(it) : std::next(it);
    m_foundSequences.insert(sequence.value);
    m_selectedRank = 0;
    refreshFromMarkers(true);
    if (!m_moments.empty())
        setSelectedRank(m_moments.front().rank);
    else
        setMessage(m_warnings.isEmpty() ? tr("No moment stood out. Try more signals or a lower minimum score.")
                                        : m_warnings.join(QLatin1Char('\n')));
}

HighlightsBridge::ReviewKey HighlightsBridge::keyOf(const Moment& moment) const {
    return {m_sequence.value, moment.startTicks.value, moment.endTicks.value};
}

QString HighlightsBridge::sequenceName() const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : ProjectSnapshot{};
    const Sequence* seq = snap ? snap->findSequence(m_sequence) : nullptr;
    return seq ? QString::fromStdString(seq->name) : QString();
}

bool HighlightsBridge::otherSequence() const {
    return m_controller && m_sequence.isValid() && m_sequence != m_controller->currentSequenceId();
}

void HighlightsBridge::refreshFromMarkers(bool force) {
    if (m_committing)
        return; // the job's done() reads them
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : ProjectSnapshot{};
    const auto highlightsOf = [](const Sequence* seq) {
        std::vector<Marker> out;
        if (seq) {
            for (const Marker& m : seq->markers) {
                if (m.highlight)
                    out.push_back(m);
            }
        }
        return out;
    };
    // The current sequence's highlights; a sequence without any that no find ran on (a reel just made, a new one)
    // keeps the list shown.
    const SequenceId current = m_controller ? m_controller->currentSequenceId() : SequenceId{};
    SequenceId target = current;
    std::vector<Marker> markers = highlightsOf(snap ? snap->findSequence(current) : nullptr);
    if (markers.empty() && snap && current != m_sequence && !m_foundSequences.contains(current.value)) {
        if (std::vector<Marker> kept = highlightsOf(snap->findSequence(m_sequence)); !kept.empty()) {
            target = m_sequence;
            markers = std::move(kept);
        }
    }
    // sequenceName / otherSequence follow switches even when the list stays.
    const auto noteShown = [this, current] {
        if (m_shownListed != m_sequence || m_shownCurrent != current) {
            m_shownListed = m_sequence;
            m_shownCurrent = current;
            emit shownChanged();
        }
    };
    if (!force && target == m_sequence && markers == m_listedMarkers) {
        noteShown();
        return;
    }

    // The chosen and the previewed moment, by their ranges (ranks follow the scores).
    const Moment* chosen = findMoment(m_selectedRank);
    std::optional<ReviewKey> chosenKey = chosen ? std::optional(keyOf(*chosen)) : std::nullopt;
    const Moment* previewed = findMoment(m_previewRank);
    std::optional<ReviewKey> previewKey = previewed ? std::optional(keyOf(*previewed)) : std::nullopt;

    // Fix round 1: a ripple edit (Ripple Sequence Markers, on by default in 0.4; dead-time removal; instant replay)
    // moves highlight markers. Their accept / reject, and the chosen and previewed moment, go with them: a marker that
    // is no longer where it was is the new one that equals it in all but its start, length and peak (nearest first).
    if (target == m_sequence && !m_listedMarkers.empty()) {
        const auto keyAt = [&](const Marker& m) { return ReviewKey{target.value, m.start.value, m.end().value}; };
        const auto sameMarker = [](const Marker& a, const Marker& b) {
            if (a.name != b.name || a.comment != b.comment || a.colorIndex != b.colorIndex || a.type != b.type ||
                a.url != b.url || a.target != b.target || !a.highlight || !b.highlight)
                return false;
            const MarkerHighlight& x = *a.highlight;
            const MarkerHighlight& y = *b.highlight;
            return x.score == y.score && x.confidence == y.confidence && x.reasons == y.reasons && x.source == y.source;
        };
        // Still there: the same marker over the same range.
        const auto stays = [&](const std::vector<Marker>& in, const Marker& m) {
            return std::ranges::any_of(in, [&](const Marker& o) {
                return o.start == m.start && o.duration == m.duration && sameMarker(o, m);
            });
        };
        std::vector<const Marker*> arrived;
        for (const Marker& m : markers) {
            if (!stays(m_listedMarkers, m))
                arrived.push_back(&m);
        }
        std::vector<std::pair<ReviewKey, ReviewKey>> moves; // from, to
        for (const Marker& old : m_listedMarkers) {
            if (arrived.empty())
                break;
            if (stays(markers, old))
                continue;
            auto best = arrived.end();
            for (auto it = arrived.begin(); it != arrived.end(); ++it) {
                if (sameMarker(old, **it) &&
                    (best == arrived.end() ||
                     std::llabs((*it)->start.value - old.start.value) < std::llabs((*best)->start.value - old.start.value)))
                    best = it;
            }
            if (best == arrived.end())
                continue;
            moves.emplace_back(keyAt(old), keyAt(**best));
            arrived.erase(best);
        }
        // All taken out first, then put back: a marker may land where another one was.
        std::vector<std::pair<ReviewKey, Review>> carried;
        for (const auto& [from, to] : moves) {
            if (const auto r = m_reviews.find(from); r != m_reviews.end()) {
                carried.emplace_back(to, r->second);
                m_reviews.erase(r);
            }
        }
        for (const auto& [to, review] : carried)
            m_reviews[to] = review;
        const auto follow = [&](const std::optional<ReviewKey>& key) {
            if (key) {
                for (const auto& [from, to] : moves) {
                    if (from == *key)
                        return std::optional(to);
                }
            }
            return key;
        };
        chosenKey = follow(chosenKey);
        previewKey = follow(previewKey);
    }

    m_sequence = target;
    m_listedMarkers = markers;
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) {
        return a.highlight->score != b.highlight->score ? a.highlight->score > b.highlight->score : a.start < b.start;
    });
    static const QRegularExpression named(QStringLiteral("^Highlight\\s+(\\d+)\\s*:\\s*(.*)$"));
    const Sequence* seq = snap ? snap->findSequence(target) : nullptr;
    if (seq)
        setPreviewFrameContext(snap, m_controller->projectFile());
    const std::shared_ptr<const PeakSource> peaks = m_controller ? m_controller->peakSource() : nullptr;
    m_moments.clear();
    for (const Marker& marker : markers) {
        const MarkerHighlight& h = *marker.highlight;
        Moment m;
        m.rank = static_cast<int>(m_moments.size()) + 1;
        m.startTicks = marker.start;
        m.endTicks = marker.end();
        m.start = marker.start.toSeconds();
        m.end = marker.end().toSeconds();
        m.duration = m.end - m.start;
        m.peak = std::clamp(h.peak, marker.start, marker.end()).toSeconds();
        m.thumbnail = m.peak; // the peak is the thumbnail time
        m.score = clampTo(h.score, 0, 1);
        m.confidenceLabel = QString::fromUtf8(toString(h.confidence).data(), static_cast<qsizetype>(toString(h.confidence).size()));
        m.confidence = h.confidence == HighlightConfidence::High ? 0.9 : h.confidence == HighlightConfidence::Medium ? 0.6 : 0.3;
        for (const std::string& reason : h.reasons)
            m.reasons << QString::fromStdString(reason);
        // "Highlight 3: Clutch round": the title; the find's rank finds what it measured.
        const QString name = QString::fromStdString(marker.name);
        int findRank = 0;
        if (const auto match = named.match(name); match.hasMatch()) {
            findRank = match.captured(1).toInt();
            m.title = match.captured(2).trimmed();
        } else {
            m.title = name;
        }
        if (target == m_findSequence) {
            for (const FindExtra& extra : m_extras) {
                if ((findRank == 0 || extra.rank == findRank) && std::abs(extra.start - m.start) < 0.5) {
                    m.signalBars = extra.signalBars;
                    if (extra.confidence > 0)
                        m.confidence = extra.confidence;
                    break;
                }
            }
        }
        if (seq) {
            if (const auto at = pictureAt(*snap, *seq, ticksOf(m.thumbnail)))
                m.image = QStringLiteral("image://wabcut-frames/%1/%2").arg(mediaKey(at->first)).arg(at->second.value);
            m.waveform = waveformOf(*snap, *seq, peaks.get(), m.startTicks, m.endTicks, kWaveformBins);
        }
        m.review = Review::Undecided;
        if (const auto r = m_reviews.find(keyOf(m)); r != m_reviews.end())
            m.review = r->second;
        m_moments.push_back(std::move(m));
    }

    m_searched = m_foundSequences.contains(target.value);
    m_recordingCount = target == m_findSequence ? static_cast<int>(m_recordings.size()) : 0;
    m_chatMessages = target == m_findSequence ? m_findChatMessages : 0;

    // The same moments stay chosen and previewed when they are still there.
    const auto rankOf = [this](const std::optional<ReviewKey>& key) {
        for (const Moment& m : m_moments) {
            if (key && keyOf(m) == *key)
                return m.rank;
        }
        return 0;
    };
    const int selected = rankOf(chosenKey);
    const int wasSelected = m_selectedRank;
    m_selectedRank = selected != 0 ? selected : (m_moments.empty() ? 0 : m_moments.front().rank);
    const int previewing = rankOf(previewKey);
    if (m_previewRank != 0 && previewing == 0) {
        m_previewRank = 0;
        emit previewChanged();
        transport(false);
    } else if (previewing != m_previewRank) {
        m_previewRank = previewing;
        emit previewChanged();
    }
    rebuild();
    if (m_selectedRank != wasSelected)
        emit selectedRankChanged();
    noteShown();
}

void HighlightsBridge::refreshWaveforms() {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : ProjectSnapshot{};
    const Sequence* seq = snap ? snap->findSequence(m_sequence) : nullptr;
    if (!seq)
        return;
    const std::shared_ptr<const PeakSource> peaks = m_controller->peakSource();
    bool changed = false;
    for (Moment& m : m_moments) {
        if (!m.waveform.isEmpty())
            continue;
        m.waveform = waveformOf(*snap, *seq, peaks.get(), ticksOf(m.start), ticksOf(m.end), kWaveformBins);
        changed = changed || !m.waveform.isEmpty();
    }
    if (changed)
        rebuild();
}

void HighlightsBridge::rebuild() {
    // Best first, rejected ones last.
    std::vector<const Moment*> order;
    for (const Moment& m : m_moments)
        order.push_back(&m);
    std::stable_sort(order.begin(), order.end(), [](const Moment* a, const Moment* b) {
        return (a->review == Review::Rejected) < (b->review == Review::Rejected);
    });
    m_list.clear();
    for (const Moment* m : order) {
        const QString state = m->review == Review::Accepted   ? QStringLiteral("accepted")
                              : m->review == Review::Rejected ? QStringLiteral("rejected")
                                                                      : QString();
        m_list.append(QVariantMap{{QStringLiteral("rank"), m->rank},
                                  {QStringLiteral("start"), m->start},
                                  {QStringLiteral("end"), m->end},
                                  {QStringLiteral("peak"), m->peak},
                                  {QStringLiteral("thumbnail"), m->thumbnail},
                                  {QStringLiteral("duration"), m->duration},
                                  {QStringLiteral("score"), m->score},
                                  {QStringLiteral("confidence"), m->confidence},
                                  {QStringLiteral("confidenceLabel"), m->confidenceLabel},
                                  {QStringLiteral("title"), m->title.isEmpty() ? tr("Moment %1").arg(m->rank) : m->title},
                                  {QStringLiteral("reasons"), m->reasons},
                                  {QStringLiteral("signals"), m->signalBars},
                                  {QStringLiteral("waveform"), m->waveform},
                                  {QStringLiteral("image"), m->image},
                                  {QStringLiteral("rangeText"), tr("%1 – %2").arg(clockText(m->start), clockText(m->end))},
                                  {QStringLiteral("durationText"), tr("%1 s").arg(m->duration, 0, 'f', 1)},
                                  {QStringLiteral("peakText"), clockText(m->peak)},
                                  {QStringLiteral("onTimeline"), true},
                                  {QStringLiteral("state"), state}});
    }
    emit momentsChanged();
}

void HighlightsBridge::accept(int rank) {
    Moment* m = findMoment(rank);
    if (!m)
        return;
    m->review = m->review == Review::Accepted ? Review::Undecided : Review::Accepted;
    m_reviews[keyOf(*m)] = m->review;
    rebuild();
}

void HighlightsBridge::reject(int rank) {
    Moment* m = findMoment(rank);
    if (!m)
        return;
    m->review = m->review == Review::Rejected ? Review::Undecided : Review::Rejected;
    m_reviews[keyOf(*m)] = m->review;
    if (m->review == Review::Rejected && m_previewRank == rank)
        stopPreview();
    rebuild();
}

bool HighlightsBridge::showSequence(SequenceId sequence) {
    if (!m_controller)
        return false;
    if (m_controller->currentSequenceId() == sequence)
        return true;
    const ProjectSnapshot snap = m_controller->snapshot();
    for (std::size_t i = 0; snap && i < snap->sequences.size(); ++i) {
        if (snap->sequences[i]->id == sequence) {
            m_controller->setCurrentSequenceIndex(static_cast<int>(i));
            return m_controller->currentSequenceId() == sequence;
        }
    }
    return false;
}

QString HighlightsBridge::jump(int rank) {
    const Moment* m = findMoment(rank);
    if (!m || !m_controller)
        return tr("No such moment");
    if (!showSequence(m_sequence))
        return tr("The moment's sequence is gone");
    if (m_previewRank != 0)
        stopPreview();
    setSelectedRank(rank);
    m_controller->setPlayhead(ticksOf(m->peak));
    emit revealRequested();
    return {};
}

QString HighlightsBridge::preview(int rank) {
    if (m_previewRank == rank && rank != 0) {
        stopPreview();
        return {};
    }
    const Moment* m = findMoment(rank);
    if (!m || !m_controller)
        return tr("No such moment");
    if (!showSequence(m_sequence))
        return tr("The moment's sequence is gone");
    if (m_previewRank != 0)
        stopPreview();
    setSelectedRank(rank);
    m_previewStart = ticksOf(m->start);
    m_previewEnd = ticksOf(m->end);
    m_previewRank = rank; // before the seek, so the playhead check knows the range
    m_controller->setPlayhead(m_previewStart);
    emit revealRequested();
    emit previewChanged();
    transport(true);
    return {};
}

void HighlightsBridge::stopPreview() {
    if (m_previewRank == 0)
        return;
    m_previewRank = 0;
    emit previewChanged();
    transport(false);
}

void HighlightsBridge::onPlayhead() {
    if (m_previewRank == 0 || !m_controller)
        return;
    const Ticks t = m_controller->playhead();
    if (t >= m_previewEnd) {
        stopPreview(); // the end of the moment
    } else if (t < m_previewStart) {
        // Moved away by hand: no longer a preview.
        m_previewRank = 0;
        emit previewChanged();
    }
}

void HighlightsBridge::transport(bool play) {
    if (m_transport) {
        m_transport(play);
        return;
    }
    if (!m_playback)
        return;
    if (play) {
        m_playback->showProgram();
        m_playback->play();
    } else if (m_playback->playing()) {
        m_playback->pause();
    }
}

// --- Reel and Short -------------------------------------------------------------------------------------------------------

void HighlightsBridge::openCreated(const command::CommandResult& result) {
    std::uint64_t created = result.createdIds.empty() ? 0 : result.createdIds.front();
    if (created == 0 && result.data.contains("sequence") && result.data["sequence"].is_number_unsigned())
        created = result.data["sequence"].get<std::uint64_t>();
    QString name = str(result.data, "name");
    if (created != 0 && showSequence(SequenceId{created}) && name.isEmpty())
        name = m_controller->sequenceName();
    QString made = tr("Made '%1'").arg(name);
    const QString layout = str(result.data, "layout");
    if (!layout.isEmpty()) {
        const QString how = layout == QLatin1String("verticalSplit")      ? tr("gameplay and facecam split")
                            : layout == QLatin1String("verticalBlurFill") ? tr("gameplay over a blurred fill")
                            : layout == QLatin1String("pip")              ? tr("facecam in a corner")
                                                                          : layout;
        const int width = static_cast<int>(num(result.data, "width"));
        const int height = static_cast<int>(num(result.data, "height"));
        made = width > 0 && height > 0 ? tr("Made '%1': %2 × %3, %4").arg(name).arg(width).arg(height).arg(how)
                                       : tr("Made '%1': %2").arg(name, how);
    }
    QStringList lines{made};
    lines << joinedWarnings(result.warnings);
    setMessage(lines.join(QLatin1Char('\n')));
    emit sequenceOpened(name);
}

QString HighlightsBridge::makeReel(const QVariantMap& options) {
    if (!reelAvailable())
        return tr("Highlight reels arrive with the next engine update");
    if (reelCount() == 0) {
        const QString why = tr("Accept the moments to put in the reel first");
        setMessage(why);
        return why;
    }
    stopPreview();
    QPointer<HighlightsBridge> self(this);
    const QString error = runJob(QStringLiteral("reel"), tr("Making the reel"), "highlights.toReel", reelParams(options), true,
                                 [self](const command::CommandResult& result, bool) {
                                     if (!self)
                                         return;
                                     if (!result.ok())
                                         self->setMessage(errorText(result));
                                     else
                                         self->openCreated(result);
                                 });
    if (!error.isEmpty())
        setMessage(error);
    return error;
}

QString HighlightsBridge::makeShort(int rank, const QVariantMap& options) {
    if (!shortAvailable())
        return tr("Shorts from highlights arrive with the next engine update");
    if (rank == 0)
        rank = m_selectedRank;
    if (!findMoment(rank)) {
        const QString why = tr("Pick a moment first");
        setMessage(why);
        return why;
    }
    stopPreview();
    QPointer<HighlightsBridge> self(this);
    const QString error = runJob(QStringLiteral("short"), tr("Making the short"), "highlights.toShort",
                                 shortParams(rank, options), true, [self](const command::CommandResult& result, bool) {
                                     if (!self)
                                         return;
                                     if (!result.ok())
                                         self->setMessage(errorText(result));
                                     else
                                         self->openCreated(result);
                                 });
    if (!error.isEmpty())
        setMessage(error);
    return error;
}

// --- 0.4: Shorts of every chosen moment, and their exports ---------------------------------------------------------------

QString HighlightsBridge::makeShorts(const QVariantMap& options) {
    if (!shortAvailable())
        return tr("Shorts from highlights arrive with the next engine update");
    // The reel's rule: the accepted moments, else every moment not rejected; in time order.
    const bool anyAccepted = acceptedCount() > 0;
    std::vector<const Moment*> chosen;
    for (const Moment& m : m_moments) {
        if (anyAccepted ? m.review == Review::Accepted : m.review != Review::Rejected)
            chosen.push_back(&m);
    }
    std::stable_sort(chosen.begin(), chosen.end(), [](const Moment* a, const Moment* b) { return a->start < b->start; });
    if (chosen.empty()) {
        const QString why = tr("Accept the moments to make Shorts of first");
        setMessage(why);
        return why;
    }
    std::vector<command::CommandRequest> requests;
    QStringList titles; // "#4 Triple kill", per request
    for (const Moment* m : chosen) {
        command::CommandRequest request;
        request.command = "highlights.toShort";
        request.params = toJson(shortParams(m->rank, options));
        requests.push_back(std::move(request));
        titles << (m->title.isEmpty() ? QStringLiteral("#%1").arg(m->rank) : QStringLiteral("#%1 %2").arg(m->rank).arg(m->title));
    }
    stopPreview();
    const int total = static_cast<int>(requests.size());
    QPointer<HighlightsBridge> self(this);
    //: The undo step of Highlights' Make Shorts
    const QString label = tr("Make Highlight Shorts");
    const QString error = runJobs(
        QStringLiteral("shorts"), tr("Making %n Short(s)", "", total), std::move(requests), label, true,
        [self, total, titles](const std::vector<command::CommandResult>& results, bool, bool cancelled) {
            if (!self)
                return;
            std::vector<SequenceId> made;
            QStringList names;
            QStringList problems;
            const ProjectSnapshot snap = self->m_controller ? self->m_controller->snapshot() : ProjectSnapshot{};
            for (std::size_t i = 0; i < results.size(); ++i) {
                const command::CommandResult& r = results[i];
                if (!r.ok()) {
                    if (r.error->code == command::ErrorCode::StaleRevision) {
                        // Nothing was committed: the project changed meanwhile.
                        self->setMessage(errorText(r));
                        return;
                    }
                    if (r.error->code != command::ErrorCode::Cancelled && static_cast<qsizetype>(i) < titles.size())
                        problems << QStringLiteral("%1: %2").arg(titles[static_cast<qsizetype>(i)], errorText(r));
                    continue;
                }
                std::uint64_t created = r.createdIds.empty() ? 0 : r.createdIds.front();
                if (created == 0 && r.data.contains("sequence") && r.data["sequence"].is_number_unsigned())
                    created = r.data["sequence"].get<std::uint64_t>();
                if (created == 0)
                    continue;
                made.push_back(SequenceId{created});
                QString name = str(r.data, "name");
                if (const Sequence* seq = snap ? snap->findSequence(SequenceId{created}) : nullptr; name.isEmpty() && seq)
                    name = QString::fromStdString(seq->name);
                names << name;
            }
            self->m_madeShorts = made;
            emit self->madeShortsChanged();
            QStringList lines;
            lines << (made.empty() ? tr("No Short was made.")
                                   : tr("Made %n Short(s): %1", "", static_cast<int>(made.size())).arg(names.join(QStringLiteral(", "))));
            if (cancelled)
                lines << tr("Cancelled after %1 of %2.").arg(made.size()).arg(total);
            lines << problems;
            self->setMessage(lines.join(QLatin1Char('\n')));
        });
    if (!error.isEmpty())
        setMessage(error);
    return error;
}

int HighlightsBridge::madeShortsCount() const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : ProjectSnapshot{};
    if (!snap)
        return 0;
    return static_cast<int>(std::ranges::count_if(m_madeShorts, [&](SequenceId id) { return snap->findSequence(id) != nullptr; }));
}

void HighlightsBridge::setQueueExport(
    std::function<QString(const QString& sequenceKey, const QString& presetId, const QVariantMap& overrides)> queue) {
    m_queueExport = std::move(queue);
}

int HighlightsBridge::exportShorts(bool hevc) {
    if (!m_queueExport || !m_controller)
        return 0;
    QVariantMap overrides;
    if (hevc)
        overrides.insert(QStringLiteral("videoCodec"), QStringLiteral("hevc"));
    const ProjectSnapshot snap = m_controller->snapshot();
    int queued = 0;
    for (const SequenceId id : m_madeShorts) {
        if (!snap || !snap->findSequence(id))
            continue; // undone or deleted since
        if (!m_queueExport(sequenceKey(id), QStringLiteral("vertical-1080x1920"), overrides).isEmpty())
            ++queued;
    }
    setMessage(tr("Queued %n export(s)", "", queued));
    return queued;
}

} // namespace wabcut::app
