#include "SceneDetectBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/command/Command.h>

#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <utility>

// Undo labels are translated where ProjectController shows them.
#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

namespace wabcut::app {

using command::Json;

namespace {

const QStringList& detectorIds() {
    static const QStringList ids{QStringLiteral("combined"), QStringLiteral("content"), QStringLiteral("adaptive"),
                                 QStringLiteral("threshold")};
    return ids;
}

/// The detectors that read the content threshold (Sensitivity): combined (its hard cuts) and content.
bool usesThreshold(const QString& detector) {
    return detector == QLatin1String("combined") || detector == QLatin1String("content");
}

QString clipsText(int n) {
    return n == 1 ? SceneDetectBridge::tr("1 clip") : SceneDetectBridge::tr("%1 clips").arg(n);
}
QString cutsText(int n) {
    return n == 1 ? SceneDetectBridge::tr("1 cut") : SceneDetectBridge::tr("%1 cuts").arg(n);
}
QString markersText(int n) {
    return n == 1 ? SceneDetectBridge::tr("1 marker") : SceneDetectBridge::tr("%1 markers").arg(n);
}

} // namespace

SceneDetectBridge::SceneDetectBridge(QObject* parent) : QObject(parent) {
    const QSettings settings;
    m_cuts = settings.value(QStringLiteral("sceneDetect/cuts"), true).toBool();
    m_markers = settings.value(QStringLiteral("sceneDetect/markers"), false).toBool();
    const QString detector = settings.value(QStringLiteral("sceneDetect/detector"), m_detector).toString();
    if (detectorIds().contains(detector))
        m_detector = detector;
    const double sensitivity = settings.value(QStringLiteral("sceneDetect/sensitivity"), 50.0).toDouble();
    m_sensitivity = std::isfinite(sensitivity) ? std::clamp(sensitivity, 0.0, 100.0) : 50.0;
}

SceneDetectBridge* SceneDetectBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new SceneDetectBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void SceneDetectBridge::setController(ProjectController* controller) {
    m_controller = controller;
}

std::shared_ptr<command::CommandServices> SceneDetectBridge::services() {
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

QVariantMap SceneDetectBridge::options() const {
    return {{QStringLiteral("cuts"), m_cuts},
            {QStringLiteral("markers"), m_markers},
            {QStringLiteral("detector"), m_detector},
            {QStringLiteral("sensitivity"), m_sensitivity}};
}

void SceneDetectBridge::setOption(const QString& name, const QVariant& value) {
    QSettings settings;
    if (name == QLatin1String("cuts")) {
        if (value.toBool() == m_cuts)
            return;
        m_cuts = value.toBool();
        settings.setValue(QStringLiteral("sceneDetect/cuts"), m_cuts);
    } else if (name == QLatin1String("markers")) {
        if (value.toBool() == m_markers)
            return;
        m_markers = value.toBool();
        settings.setValue(QStringLiteral("sceneDetect/markers"), m_markers);
    } else if (name == QLatin1String("detector")) {
        const QString detector = value.toString();
        if (!detectorIds().contains(detector) || detector == m_detector)
            return;
        m_detector = detector;
        settings.setValue(QStringLiteral("sceneDetect/detector"), m_detector);
    } else if (name == QLatin1String("sensitivity")) {
        const double s = value.toDouble();
        const double clamped = std::isfinite(s) ? std::clamp(s, 0.0, 100.0) : 50.0;
        if (clamped == m_sensitivity)
            return;
        m_sensitivity = clamped;
        settings.setValue(QStringLiteral("sceneDetect/sensitivity"), m_sensitivity);
    } else {
        return;
    }
    emit optionsChanged();
}

double SceneDetectBridge::thresholdFor(double sensitivity) {
    const double s = std::isfinite(sensitivity) ? std::clamp(sensitivity, 0.0, 100.0) : 50.0;
    const double threshold = 27.0 * std::pow(2.0, (50.0 - s) / 50.0);
    return std::clamp(std::round(threshold * 10.0) / 10.0, 1.0, 255.0);
}

void SceneDetectBridge::setJob(bool busy, const QString& phase, double progress) {
    m_busy = busy;
    m_phase = phase;
    m_progress = progress;
    emit jobChanged();
}

void SceneDetectBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

std::vector<SceneDetectBridge::Target> SceneDetectBridge::collect(const Sequence* seq, const std::vector<ClipId>& ids,
                                                                  int* locked) const {
    std::vector<Target> out;
    if (!seq || !m_controller)
        return out;
    const ProjectSnapshot snap = m_controller->snapshot();
    std::vector<std::pair<Ticks, Target>> ordered;
    std::set<std::uint64_t> seen;
    const auto add = [&](ClipId id) {
        const auto loc = seq->findClip(id);
        if (!loc || loc->track->kind != TrackKind::Video || !seen.insert(id.value).second)
            return;
        const Clip& c = loc->clip();
        const bool nested = c.isNested();
        if (!nested) {
            const MediaItem* media = snap->findMedia(c.media);
            const int stream = c.stream.kind == StreamKind::Video ? c.stream.index : 0;
            if (!media || media->isGenerator() || !media->hasStream(StreamRef{StreamKind::Video, stream}))
                return;
        }
        if (loc->track->locked) {
            if (locked)
                ++*locked;
            return;
        }
        ordered.emplace_back(c.start(), Target{id, nested});
    };
    for (const ClipId id : ids) {
        const auto loc = seq->findClip(id);
        if (!loc)
            continue;
        if (loc->track->kind == TrackKind::Audio) {
            // An audio clip stands for its linked picture (the shots are in the video).
            const LinkGroupId group = loc->clip().linkGroup;
            if (!group.isValid())
                continue;
            for (const TrackPtr& track : seq->videoTracks) {
                for (const Clip& c : track->clips) {
                    if (c.linkGroup == group)
                        add(c.id);
                }
            }
            continue;
        }
        add(id);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [start, target] : ordered)
        out.push_back(target);
    return out;
}

QStringList SceneDetectBridge::targets(const QStringList& clipKeys) const {
    std::vector<ClipId> ids;
    if (clipKeys.isEmpty() && m_controller) {
        ids = m_controller->selection();
    } else {
        for (const QString& key : clipKeys) {
            if (const auto id = parseClipKey(key))
                ids.push_back(*id);
        }
    }
    QStringList out;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    for (const Target& t : collect(seq.get(), ids, nullptr))
        out << clipKey(t.clip);
    return out;
}

QVariantMap SceneDetectBridge::availability() const {
    const auto refuse = [](const QString& reason, const QString& note) {
        return QVariantMap{{QStringLiteral("enabled"), false},
                           {QStringLiteral("reason"), reason},
                           {QStringLiteral("note"), note},
                           {QStringLiteral("clips"), QStringList()},
                           {QStringLiteral("count"), 0},
                           {QStringLiteral("nested"), 0}};
    };
    if (!m_controller || !m_controller->currentSequence())
        return refuse(tr("Open a sequence first"), tr("no sequence"));
    if (m_busy)
        return refuse(tr("Scene Edit Detection is running"), tr("running"));
    if (m_controller->selection().empty())
        return refuse(tr("Select a video clip first"), tr("no video clip"));
    int locked = 0;
    const SequencePtr seq = m_controller->currentSequence();
    const std::vector<Target> found = collect(seq.get(), m_controller->selection(), &locked);
    if (found.empty())
        return locked > 0 ? refuse(tr("The video track is locked"), tr("track locked"))
                          : refuse(tr("Scene Edit Detection works on video clips: select one"), tr("no video clip"));
    QStringList clips;
    int nested = 0;
    for (const Target& t : found) {
        clips << clipKey(t.clip);
        if (t.nested)
            ++nested;
    }
    return {{QStringLiteral("enabled"), true},
            {QStringLiteral("reason"), QString()},
            {QStringLiteral("note"), QString()},
            {QStringLiteral("clips"), clips},
            {QStringLiteral("count"), static_cast<int>(clips.size())},
            {QStringLiteral("nested"), nested},
            {QStringLiteral("sequence"), sequenceKey(seq->id)}};
}

bool SceneDetectBridge::handlesCommand(const QString& id) {
    return id == QLatin1String(kCommand);
}

QVariantMap SceneDetectBridge::commandAvailability(const QString& id) const {
    if (!handlesCommand(id))
        return {};
    const QVariantMap a = availability();
    return {{QStringLiteral("handled"), true},
            {QStringLiteral("enabled"), a.value(QStringLiteral("enabled"))},
            {QStringLiteral("reason"), a.value(QStringLiteral("reason"))}};
}

QString SceneDetectBridge::run(const QStringList& clipKeys, const QVariantMap& overrides) {
    if (!m_controller || !m_controller->currentSequence())
        return tr("Open a sequence first");
    if (m_busy || m_job.running())
        return tr("Scene Edit Detection is running");
    for (const char* name : {"cuts", "markers", "detector", "sensitivity"}) {
        const QString key = QString::fromLatin1(name);
        if (overrides.contains(key))
            setOption(key, overrides.value(key));
    }
    if (!m_cuts && !m_markers)
        return tr("Choose cuts, markers or both");
    std::vector<ClipId> ids;
    if (clipKeys.isEmpty()) {
        ids = m_controller->selection();
    } else {
        for (const QString& key : clipKeys) {
            if (const auto id = parseClipKey(key))
                ids.push_back(*id);
        }
    }
    // Fix round 1: the clips are looked up in the sequence the dialog was opened on (`sequence`), not whichever one is
    // current now; the commands edit that sequence.
    const ProjectSnapshot snap = m_controller->snapshot(); // owns the sequences looked at below
    const SequencePtr current = m_controller->currentSequence();
    const Sequence* seq = current.get();
    if (const QString key = overrides.value(QStringLiteral("sequence")).toString(); !key.isEmpty()) {
        const auto id = parseSequenceKey(key);
        seq = id && snap ? snap->findSequence(*id) : nullptr;
        if (!seq)
            return tr("That sequence is no longer in the project");
    }
    int locked = 0;
    const std::vector<Target> found = collect(seq, ids, &locked);
    if (found.empty())
        return locked > 0 ? tr("The video track is locked") : tr("Select a video clip first");

    Json detector{{"detector", m_detector.toStdString()}, {"sequence", seq->id.value}};
    if (usesThreshold(m_detector))
        detector["threshold"] = thresholdFor(m_sensitivity);
    const std::string label = LABEL("Scene Edit Detection");
    std::vector<CommandJob::Request> requests;
    // Markers first: inside each clip as it is now (after the razor no cut would be inside a piece).
    if (m_markers) {
        for (const Target& t : found) {
            if (t.nested)
                continue; // analysis.detectShots reads media clips only
            Json params = detector;
            params["clip"] = t.clip.value;
            params["markers"] = true;
            requests.push_back({"analysis.detectShots", std::move(params), label});
        }
    }
    if (m_cuts) {
        Json clips = Json::array();
        for (const Target& t : found)
            clips.push_back(t.clip.value);
        Json params = detector;
        params["clips"] = std::move(clips);
        params["linked"] = true;
        requests.push_back({"clip.splitAtShots", std::move(params), label});
    }
    if (requests.empty())
        return tr("Markers go on clips of media: turn on the cuts for nested and multicam clips");

    const bool cuts = m_cuts;
    const bool markers = m_markers;
    const int clipCount = static_cast<int>(found.size());
    setJob(true, tr("Detecting scene edits…"), -1);
    setMessage({});
    QPointer<SceneDetectBridge> self(this);
    const bool started = m_job.startBatch(
        this, m_controller, services(), std::move(requests),
        [self](const QString& text, double fraction) {
            if (self)
                self->setJob(true, text.isEmpty() ? self->m_phase : text, fraction);
        },
        [self, cuts, markers, clipCount](const command::CommandResult& result) {
            if (!self)
                return;
            self->setJob(false, {}, 0);
            const bool cancelled = !result.ok() && result.error->code == command::ErrorCode::Cancelled;
            const QString text = result.ok() ? summary(result, cuts, markers, clipCount) : commandErrorText(result);
            self->setMessage(text);
            emit self->finished(result.ok(), cancelled, text);
        });
    if (!started) {
        setJob(false, {}, 0);
        return tr("Scene Edit Detection is running");
    }
    return {};
}

void SceneDetectBridge::cancel() {
    m_job.cancel();
}

QString SceneDetectBridge::summary(const command::CommandResult& result, bool cuts, bool markers, int clips) {
    // The commands' data: a batch's steps, or the one command's.
    std::vector<const Json*> data;
    if (!result.steps.empty()) {
        for (const command::StepResult& step : result.steps)
            data.push_back(&step.data);
    } else {
        data.push_back(&result.data);
    }
    int markersAdded = 0;
    int markedClips = 0;
    int cutsMade = 0;
    std::set<std::uint64_t> clipsCut;
    for (const Json* d : data) {
        if (!d->is_object())
            continue;
        if (d->contains("markersAdded")) { // analysis.detectShots on one clip
            const int added = d->value("markersAdded", 0);
            markersAdded += added;
            if (added > 0)
                ++markedClips;
            continue;
        }
        // clip.splitAtShots: [clip, time, kind, confidence] per cut.
        for (const Json& cut : d->value("cuts", Json::array())) {
            if (!cut.is_array() || cut.empty() || !cut[0].is_number_unsigned())
                continue;
            ++cutsMade;
            clipsCut.insert(cut[0].get<std::uint64_t>());
        }
    }
    QString text;
    if ((cuts && cutsMade == 0 && markersAdded == 0) || (!cuts && markersAdded == 0)) {
        text = clips == 1 ? tr("No scene edits found in the clip") : tr("No scene edits found in %1 clips").arg(clips);
    } else if (cuts) {
        const QString split = clipsText(static_cast<int>(clipsCut.size()));
        text = markers && markersAdded > 0
                   ? tr("Split %1 at %2 and added %3").arg(split, cutsText(cutsMade), markersText(markersAdded))
                   : tr("Split %1 at %2").arg(split, cutsText(cutsMade));
    } else {
        text = tr("Added %1 at scene edits in %2").arg(markersText(markersAdded), clipsText(markedClips));
    }
    // Clips the commands left alone (no video in them, retimed inside a nest...).
    const auto skipped = static_cast<int>(result.warnings.size());
    if (skipped == 1)
        text = tr("%1; 1 clip was skipped").arg(text);
    else if (skipped > 1)
        text = tr("%1; %2 clips were skipped").arg(text).arg(skipped);
    return text;
}

} // namespace wabcut::app
