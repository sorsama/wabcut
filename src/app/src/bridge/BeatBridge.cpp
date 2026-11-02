#include "BeatBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/command/Command.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

// Undo labels are translated where ProjectController shows them.
#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

namespace wabcut::app {

using command::Json;

namespace {

QString clipCountText(int n) {
    return n == 1 ? BeatBridge::tr("1 clip") : BeatBridge::tr("%1 clips").arg(n);
}

} // namespace

BeatBridge::BeatBridge(QObject* parent) : QObject(parent) {}

BeatBridge* BeatBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new BeatBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void BeatBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        // The grid changes with edits (detections, hand-set grids, undo) and with sequence switches.
        connect(controller, &ProjectController::revisionChanged, this, &BeatBridge::refreshGrid);
        connect(controller, &ProjectController::sequenceChanged, this, &BeatBridge::refreshGrid);
    }
    emit gridChanged();
}

void BeatBridge::refreshGrid() { emit gridChanged(); }

bool BeatBridge::hasGrid() const { return m_controller && m_controller->hasBeatGrid(); }

QVariantMap BeatBridge::gridInfo() const { return m_controller ? m_controller->beatGridInfo() : QVariantMap{}; }

std::shared_ptr<command::CommandServices> BeatBridge::services() {
    if (m_services)
        return m_services;
    // Media paths resolve against the project file: services made for another one are made again.
    const std::filesystem::path file = m_controller ? m_controller->projectFile() : std::filesystem::path();
    if (!m_built || m_builtFor != file) {
        ai::analysis::MediaAnalyzerOptions options;
        options.cacheRoot = ai::AnalysisCache::defaultRoot();
        m_built = std::make_shared<ai::analysis::AnalysisServices>(
            std::make_shared<ai::analysis::MediaAnalysisProvider>(options, file));
        m_builtFor = file;
    }
    return m_built;
}

void BeatBridge::setJob(bool busy, const QString& phase, double progress) {
    m_busy = busy;
    m_phase = phase;
    m_progress = progress;
    emit jobChanged();
}

void BeatBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

QString BeatBridge::bpmText(double bpm) {
    const double rounded = std::round(bpm * 10.0) / 10.0;
    return rounded == std::floor(rounded) ? QString::number(static_cast<qlonglong>(rounded))
                                          : QString::number(rounded, 'f', 1);
}

QVariantMap BeatBridge::detectInfo() const {
    const auto refuse = [](const QString& reason) {
        return QVariantMap{{QStringLiteral("enabled"), false},
                           {QStringLiteral("reason"), reason},
                           {QStringLiteral("clip"), QString()},
                           {QStringLiteral("name"), QString()}};
    };
    if (!m_controller || !m_controller->currentSequence())
        return refuse(tr("Open a sequence first"));
    if (m_busy || m_job.running())
        return refuse(tr("Detecting beats is running"));
    const SequencePtr seq = m_controller->currentSequence();
    const ProjectSnapshot snap = m_controller->snapshot();
    // A clip on an audio track comes before a video clip with sound (the music is usually on its own track).
    for (const bool audioTrack : {true, false}) {
        for (const ClipId id : m_controller->selection()) {
            const auto loc = seq->findClip(id);
            if (!loc || (loc->track->kind == TrackKind::Audio) != audioTrack)
                continue;
            const Clip& clip = loc->clip();
            if (clip.isNested())
                continue; // a nest has no media to analyse
            const MediaItem* media = snap->findMedia(clip.media);
            if (!media || media->isGenerator() || !media->info.hasAudio())
                continue;
            return {{QStringLiteral("enabled"), true},
                    {QStringLiteral("reason"), QString()},
                    {QStringLiteral("clip"), clipKey(id)},
                    {QStringLiteral("name"), QString::fromStdString(media->name)}};
        }
    }
    return refuse(tr("Select a clip with sound"));
}

QVariantMap BeatBridge::cutInfo() const {
    const auto refuse = [](const QString& reason) {
        return QVariantMap{{QStringLiteral("enabled"), false},
                           {QStringLiteral("reason"), reason},
                           {QStringLiteral("clips"), 0},
                           {QStringLiteral("beatsPerCut"), 4}};
    };
    if (!m_controller || !m_controller->currentSequence())
        return refuse(tr("Open a sequence first"));
    if (!hasGrid())
        return refuse(tr("Detect beats first"));
    const std::vector<ClipId> selection = m_controller->selection();
    if (selection.empty())
        return refuse(tr("Select a clip first"));
    return {{QStringLiteral("enabled"), true},
            {QStringLiteral("reason"), QString()},
            {QStringLiteral("clips"), static_cast<int>(selection.size())},
            {QStringLiteral("beatsPerCut"), 4}};
}

bool BeatBridge::handlesCommand(const QString& id) {
    return id == QLatin1String(kDetect) || id == QLatin1String(kSnap) || id == QLatin1String(kCut)
           || id == QLatin1String(kClear);
}

QVariantMap BeatBridge::commandAvailability(const QString& id) const {
    if (!handlesCommand(id))
        return {};
    const auto state = [](bool enabled, const QString& reason) {
        return QVariantMap{{QStringLiteral("handled"), true},
                           {QStringLiteral("enabled"), enabled},
                           {QStringLiteral("reason"), enabled ? QString() : reason}};
    };
    if (id == QLatin1String(kDetect)) {
        const QVariantMap info = detectInfo();
        return state(info.value(QStringLiteral("enabled")).toBool(),
                     info.value(QStringLiteral("reason")).toString());
    }
    if (!m_controller || !m_controller->currentSequence())
        return state(false, tr("Open a sequence first"));
    if (id == QLatin1String(kClear)) {
        return hasGrid() ? state(true, {}) : state(false, tr("No beat grid to clear"));
    }
    // Snap and Cut both need a grid and something selected.
    if (!hasGrid())
        return state(false, tr("Detect beats first"));
    if (m_controller->selection().empty())
        return state(false, tr("Select a clip first"));
    return state(true, {});
}

QString BeatBridge::detect(const QString& clipKey, double minBpm, double maxBpm) {
    if (!m_controller || !m_controller->currentSequence())
        return tr("Open a sequence first");
    if (m_busy || m_job.running())
        return tr("Detecting beats is running");
    std::optional<ClipId> clip = parseClipKey(clipKey);
    if (!clip) {
        const QVariantMap info = detectInfo();
        clip = parseClipKey(info.value(QStringLiteral("clip")).toString());
        if (!clip)
            return info.value(QStringLiteral("reason")).toString();
    }
    if (minBpm > 0 && maxBpm > 0 && minBpm > maxBpm)
        return tr("The lowest tempo is above the highest");
    Json params{{"clip", clip->value}, {"markers", "downbeats"}, {"grid", true}};
    if (minBpm > 0)
        params["minBpm"] = minBpm;
    if (maxBpm > 0)
        params["maxBpm"] = maxBpm;

    setJob(true, tr("Detecting beats…"), -1);
    setMessage({});
    QPointer<BeatBridge> self(this);
    const bool started = m_job.start(
        this, m_controller, services(), {"analysis.detectBeats", std::move(params), LABEL("Detect Beats")},
        [self](const QString& text, double fraction) {
            if (self)
                self->setJob(true, text.isEmpty() ? self->m_phase : text, fraction);
        },
        [self](const command::CommandResult& result) {
            if (!self)
                return;
            self->setJob(false, {}, 0);
            const bool cancelled = !result.ok() && result.error->code == command::ErrorCode::Cancelled;
            const QString text = result.ok() ? detectSummary(result) : commandErrorText(result);
            self->setMessage(text);
            emit self->finished(result.ok(), cancelled, text);
        });
    if (!started) {
        setJob(false, {}, 0);
        return tr("Detecting beats is running");
    }
    return {};
}

QString BeatBridge::detectSummary(const command::CommandResult& result) {
    const Json& data = result.data;
    const int beats = data.is_object() ? data.value("beats", 0) : 0;
    const bool grid = data.is_object() && data.value("grid", false);
    if (!grid || beats < 2)
        return tr("No steady beat found in this clip: nothing was added");
    QString text = tr("%1 BPM, %2").arg(bpmText(data.value("bpm", 0.0)), beats == 1 ? tr("1 beat")
                                                                                     : tr("%1 beats").arg(beats));
    const int markers = data.value("markersAdded", 0);
    if (markers > 0)
        text += markers == 1 ? tr(" · 1 downbeat marker") : tr(" · %1 downbeat markers").arg(markers);
    return text;
}

QString BeatBridge::snapSelection() {
    if (!m_controller || !m_controller->currentSequence())
        return tr("Open a sequence first");
    if (!hasGrid())
        return tr("Detect beats first");
    const std::vector<ClipId> selection = m_controller->selection();
    if (selection.empty())
        return tr("Select a clip first");
    Json clips = Json::array();
    for (const ClipId id : selection)
        clips.push_back(id.value);
    const command::CommandResult result =
        CommandJob::runNow(m_controller, nullptr, "edit.snapToBeats", Json{{"clips", std::move(clips)}});
    if (!result.ok()) {
        setMessage(commandErrorText(result));
        return m_message;
    }
    const Json snapped = result.data.is_object() ? result.data.value("snapped", Json::array()) : Json::array();
    const int count = static_cast<int>(snapped.is_array() ? snapped.size() : 0);
    QString text = count == 0 ? tr("No clip edge was close enough to a beat (a quarter beat)")
                              : (count == 1 ? tr("Snapped 1 clip edge to the beats")
                                            : tr("Snapped %1 clip edges to the beats").arg(count));
    if (!result.warnings.empty()) {
        const int left = static_cast<int>(result.warnings.size());
        text += left == 1 ? tr(" · 1 edge was left where it is") : tr(" · %1 edges were left where they are").arg(left);
    }
    setMessage(text);
    emit finished(true, false, text);
    return {};
}

QString BeatBridge::cutSelection(int beatsPerCut) {
    const QVariantMap info = cutInfo();
    if (info.value(QStringLiteral("enabled")).toBool() != true)
        return info.value(QStringLiteral("reason")).toString();
    const int perCut = std::clamp(beatsPerCut, 1, 64);
    Json clips = Json::array();
    for (const ClipId id : m_controller->selection())
        clips.push_back(id.value);
    Json params{{"clips", std::move(clips)},
                {"at", m_controller->playhead().value},
                {"beatsPerCut", perCut}};
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "edit.cutToBeats", std::move(params));
    if (!result.ok()) {
        setMessage(commandErrorText(result));
        return m_message;
    }
    const Json placed = result.data.is_object() ? result.data.value("placed", Json::array()) : Json::array();
    const int count = static_cast<int>(placed.is_array() ? placed.size() : 0);
    QString text = count == 0 ? tr("No clip could be placed on the beats")
                              : tr("Cut %1 to the beat, %2 beats each").arg(clipCountText(count)).arg(perCut);
    if (!result.warnings.empty())
        text += tr(" · %1 left out").arg(static_cast<int>(result.warnings.size()));
    setMessage(text);
    emit finished(true, false, text);
    return {};
}

QString BeatBridge::clearGrid() {
    if (!m_controller || !m_controller->currentSequence())
        return tr("Open a sequence first");
    if (!hasGrid())
        return tr("No beat grid to clear");
    const command::CommandResult result =
        CommandJob::runNow(m_controller, nullptr, "sequence.setBeatGrid", Json{{"clear", true}});
    if (!result.ok()) {
        setMessage(commandErrorText(result));
        return m_message;
    }
    const QString text = tr("Beat grid cleared");
    setMessage(text);
    emit finished(true, false, text);
    return {};
}

void BeatBridge::cancel() { m_job.cancel(); }

} // namespace wabcut::app
