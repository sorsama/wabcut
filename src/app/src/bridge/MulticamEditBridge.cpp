#include "MulticamEditBridge.h"

#include "AiController.h"
#include "AppAudioServices.h"
#include "BridgeIds.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace wabcut::app {

using command::Json;

namespace {

QString text(const std::string& s) { return QString::fromStdString(s); }

QString syncName(const QString& sync) {
    if (sync == QStringLiteral("timecode"))
        return MulticamEditBridge::tr("Timecode");
    if (sync == QStringLiteral("inPoints"))
        return MulticamEditBridge::tr("In points");
    if (sync == QStringLiteral("outPoints"))
        return MulticamEditBridge::tr("Out points");
    if (sync == QStringLiteral("markers"))
        return MulticamEditBridge::tr("Clip markers");
    if (sync == QStringLiteral("audio"))
        return MulticamEditBridge::tr("Audio");
    if (sync == QStringLiteral("manual"))
        return MulticamEditBridge::tr("Offsets");
    return sync;
}

/// "multicam.selectCamera7" -> 7, else 0.
int cameraOf(const QString& id, QLatin1StringView prefix) {
    if (!id.startsWith(prefix))
        return 0;
    bool ok = false;
    const int n = QStringView(id).mid(prefix.size()).toInt(&ok);
    return ok && n >= 1 && n <= 9 ? n : 0;
}

constexpr QLatin1StringView kSelect("multicam.selectCamera");
constexpr QLatin1StringView kCut("multicam.cutToCamera");

} // namespace

MulticamEditBridge::MulticamEditBridge(QObject* parent) : QObject(parent) {}

MulticamEditBridge::~MulticamEditBridge() {
    m_renderer.reset(); // joins the grid worker before the members it posts to go
}

MulticamEditBridge* MulticamEditBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new MulticamEditBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
        bridge->setLibrary(qmlEngine->singletonInstance<LibraryBridge*>("Wabcut", "Library"));
        QPointer<AiController> ai = qmlEngine->singletonInstance<AiController*>("Wabcut", "AiController");
        QPointer<TranscriptBridge> transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
        QPointer<ProjectController> controller = bridge->m_controller;
        // Its jobs may download a model through the AI transport: stopped before AiController destroys it (A5-P2-19).
        if (ai)
            QObject::connect(ai, &AiController::shuttingDown, bridge, [bridge] { bridge->m_job.stop(); });
        // The default services, made when first needed (the project file decides where stems and takes go).
        bridge->m_serviceFactory = [ai, transcripts, controller] {
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

void MulticamEditBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &MulticamEditBridge::updateClip);
        connect(controller, &ProjectController::sequenceChanged, this, &MulticamEditBridge::updateClip);
        connect(controller, &ProjectController::playheadChanged, this, &MulticamEditBridge::updateClip);
    }
    updateClip();
}

void MulticamEditBridge::setPlayback(PlaybackBridge* playback) {
    if (m_playback == playback)
        return;
    if (m_playback)
        disconnect(m_playback, nullptr, this, nullptr);
    m_playback = playback;
    if (playback)
        connect(playback, &PlaybackBridge::transportChanged, this, [this] { onPlayingChanged(m_playback && m_playback->playing()); });
}

void MulticamEditBridge::setLibrary(LibraryBridge* library) { m_library = library; }

void MulticamEditBridge::setSynchronous(bool synchronous) {
    m_synchronous = synchronous;
    m_job.setSynchronous(synchronous);
    if (synchronous)
        m_renderer.reset();
    m_gridFrom.reset(); // drawn again by the new path
    rebuildGrid();
    pushGridTime();
}

void MulticamEditBridge::setGridSize(QSize size) {
    if (size == m_gridSize || size.isEmpty())
        return;
    m_gridSize = size;
    m_gridFrom.reset(); // the grid source is set again with the new size
    rebuildGrid();
    pushGridTime();
}

std::shared_ptr<command::CommandServices> MulticamEditBridge::services() {
    if (m_services || !m_serviceFactory)
        return m_services;
    // Stems and takes go next to the project: built again when the project file changed (another project, Save As),
    // as PodcastBridge::stepServices() does.
    const std::filesystem::path file = m_controller ? m_controller->projectFile() : std::filesystem::path();
    if (!m_built || m_builtFor != file) {
        m_built = m_serviceFactory();
        m_builtFor = file;
    }
    return m_built;
}

void MulticamEditBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

void MulticamEditBridge::setJob(const QString& phase, double progress) {
    m_phase = phase;
    m_progress = progress;
    emit jobChanged();
}

// ---- Create Multi-Camera Source Sequence -----------------------------------------------------------------------

QVariantList MulticamEditBridge::describeMedia(const QStringList& keys) const {
    QVariantList out;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return out;
    for (const QString& key : keys) {
        const auto id = parseMediaKey(key);
        const MediaItem* m = id ? snap->findMedia(*id) : nullptr;
        if (!m || m->isGenerator() || (!m->info.hasVideo() && !m->info.hasAudio()))
            continue;
        out.append(QVariantMap{{QStringLiteral("key"), key},
                               {QStringLiteral("name"), text(m->name)},
                               {QStringLiteral("kind"), m->info.hasVideo() ? QStringLiteral("video") : QStringLiteral("audio")},
                               {QStringLiteral("hasTimecode"), m->info.startTimecode.has_value()},
                               {QStringLiteral("hasAudio"), m->info.hasAudio()}});
    }
    return out;
}

QString MulticamEditBridge::proposedSync(const QStringList& keys) const {
    const QVariantList items = describeMedia(keys);
    if (items.isEmpty())
        return QStringLiteral("inPoints");
    const auto all = [&](const char* field) {
        return std::all_of(items.begin(), items.end(), [&](const QVariant& v) { return v.toMap().value(QLatin1String(field)).toBool(); });
    };
    if (all("hasTimecode"))
        return QStringLiteral("timecode");
    if (all("hasAudio") && items.size() >= 2)
        return QStringLiteral("audio");
    return QStringLiteral("inPoints");
}

QString MulticamEditBridge::createSource(const QStringList& media, const QString& sync, const QString& name,
                                     const QStringList& angleNames) {
    if (!m_controller)
        return tr("No project");
    if (busy())
        return tr("A multi-camera source is being made");
    const ProjectSnapshot snap = m_controller->snapshot();
    Json ids = Json::array();
    Json names = Json::array();
    bool named = false;
    for (int i = 0; i < media.size(); ++i) {
        const auto id = parseMediaKey(media[i]);
        const MediaItem* m = id ? snap->findMedia(*id) : nullptr;
        if (!m)
            return tr("Choose media items in the Library");
        ids.push_back(id->value);
        const QString given = i < angleNames.size() ? angleNames[i].trimmed() : QString();
        named = named || !given.isEmpty();
        names.push_back(given.isEmpty() ? m->name : given.toStdString());
    }
    if (ids.empty())
        return tr("Choose media items in the Library");
    Json params{{"media", ids}, {"sync", sync.toStdString()}};
    if (!name.trimmed().isEmpty())
        params["name"] = name.trimmed().toStdString();
    if (named)
        params["angleNames"] = names;
    m_report.clear();
    emit reportChanged();
    setMessage({});
    QPointer<MulticamEditBridge> self(this);
    const auto progress = [self](const QString& phase, double fraction) {
        if (self)
            self->setJob(phase, fraction);
    };
    const auto create = [self, params, progress](const QVariantList& audioRows) {
        if (!self)
            return;
        self->setJob(tr("Making the multi-camera source"), -1);
        const bool started = self->m_job.start(self, self->m_controller, self->services(),
                                               {"multicam.create", params, "Create Multi-Camera Source Sequence"}, progress,
                                               [self, audioRows](const command::CommandResult& result) {
                                                   if (self)
                                                       self->finishCreate(result, audioRows);
                                               });
        if (!started) {
            self->m_creating = false;
            self->setJob({}, 0);
            self->setMessage(tr("A job is running"));
        }
    };
    m_creating = true;
    if (sync == QStringLiteral("audio") && ids.size() >= 2) {
        // Measure first: the report shows each angle's confidence and drift (the analysis is cached, so creating the
        // source afterwards does not run it again).
        setJob(tr("Measuring the angles' sound"), -1);
        const bool started = m_job.start(this, m_controller, services(), {"audio.syncClips", Json{{"media", ids}}, "Synchronize"},
                                         progress, [self, create](const command::CommandResult& result) {
                                             if (!self)
                                                 return;
                                             if (!result.ok()) {
                                                 self->finishCreate(result, {});
                                                 return;
                                             }
                                             QVariantList rows;
                                             for (const Json& r : result.data.value("recordings", Json::array()))
                                                 rows.append(QVariant::fromValue(QString::fromStdString(r.dump())));
                                             create(rows);
                                         });
        if (!started) {
            m_creating = false;
            setJob({}, 0);
            return tr("A job is running");
        }
        return {};
    }
    create({});
    return {};
}

void MulticamEditBridge::finishCreate(const command::CommandResult& result, const QVariantList& audioRows) {
    m_creating = false;
    setJob({}, 0);
    QVariantMap report;
    if (!result.ok()) {
        report.insert(QStringLiteral("ok"), false);
        report.insert(QStringLiteral("error"), commandErrorText(result));
        m_report = report;
        emit reportChanged();
        setMessage(commandErrorText(result));
        return;
    }
    const Json& data = result.data;
    const QString sync = text(data.value("sync", std::string()));
    const QString key = sequenceKey(SequenceId{data.value("multicam", std::uint64_t{0})});
    QVariantList angles;
    const Json list = data.value("angles", Json::array());
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& a = list[i];
        QVariantMap row{{QStringLiteral("angle"), a.value("angle", static_cast<int>(i) + 1)},
                        {QStringLiteral("name"), text(a.value("name", std::string()))},
                        {QStringLiteral("offset"), text(a.value("offset", Json::object()).value("timecode", std::string()))},
                        {QStringLiteral("confidence"), sync == QStringLiteral("audio") ? QString() : tr("exact")},
                        {QStringLiteral("drift"), QString()},
                        {QStringLiteral("problem"), QString()}};
        if (static_cast<qsizetype>(i) < audioRows.size()) {
            const Json r = Json::parse(audioRows[static_cast<qsizetype>(i)].toString().toStdString(), nullptr, false);
            if (r.is_object()) {
                if (r.value("synced", false))
                    row.insert(QStringLiteral("confidence"),
                               QStringLiteral("%1 %").arg(std::lround(r.value("confidence", 0.0) * 100.0)));
                if (r.value("driftSignificant", false))
                    row.insert(QStringLiteral("drift"), tr("%1 ppm").arg(r.value("driftPpm", 0.0), 0, 'f', 1));
                row.insert(QStringLiteral("problem"), text(r.value("problem", std::string())));
            }
        }
        angles.append(row);
    }
    QStringList warnings;
    for (const std::string& w : result.warnings)
        warnings << text(w);
    report = {{QStringLiteral("ok"), true},
              {QStringLiteral("sequence"), key},
              {QStringLiteral("name"), text(data.value("name", std::string()))},
              {QStringLiteral("sync"), sync},
              {QStringLiteral("syncName"), syncName(sync)},
              {QStringLiteral("angles"), angles},
              {QStringLiteral("warnings"), warnings}};
    m_report = report;
    emit reportChanged();
    if (m_library)
        m_library->showItem(key);
    setMessage(tr("Made “%1” with %n angle(s)", nullptr, static_cast<int>(angles.size()))
                   .arg(report.value(QStringLiteral("name")).toString()));
}

void MulticamEditBridge::cancel() {
    m_job.cancel();
}

// ---- The multi-camera view ---------------------------------------------------------------------------------------

std::optional<MulticamEditBridge::Site> MulticamEditBridge::multicamClipAt(const ProjectState& state, const Sequence& sequence, Ticks time) {
    const auto find = [&](const std::vector<TrackPtr>& tracks, bool topFirst) -> std::optional<Site> {
        for (std::size_t n = 0; n < tracks.size(); ++n) {
            const Track& t = *tracks[topFirst ? tracks.size() - 1 - n : n];
            const auto index = t.clipIndexAt(time);
            if (!index)
                continue;
            const Clip& c = t.clips[*index];
            if (!c.isNested())
                continue;
            const Sequence* source = state.findSequence(c.sequence);
            if (source && source->multicam)
                return Site{c.id, t.id, c.sequence};
        }
        return std::nullopt;
    };
    if (auto video = find(sequence.videoTracks, true))
        return video;
    return find(sequence.audioTracks, false);
}

QString MulticamEditBridge::clip() const { return m_site ? clipKey(m_site->clip) : QString(); }

void MulticamEditBridge::setViewOn(bool on) {
    if (on == m_viewOn)
        return;
    m_viewOn = on;
    if (!on) {
        m_renderer.reset();
        m_gridFrom.reset();
        m_gridState.reset();
        m_gridImage = {};
        emit gridChanged();
    }
    emit viewChanged();
    updateClip();
    rebuildGrid();
    pushGridTime(); // the first frame at the playhead
}

void MulticamEditBridge::setTarget(const QString& target) {
    const QString next = target == QStringLiteral("video") || target == QStringLiteral("audio") ? target : QStringLiteral("both");
    if (next == m_target)
        return;
    m_target = next;
    emit targetChanged();
}

void MulticamEditBridge::updateClip() {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    std::optional<Site> site;
    if (snap && seq)
        site = multicamClipAt(*snap, *seq, m_controller->playhead());
    const bool sameSite = site.has_value() == m_site.has_value() &&
                          (!site || (site->clip == m_site->clip && site->source == m_site->source));
    const bool sameSnapshot = snap == m_siteSnapshot;
    m_site = site;
    m_siteSnapshot = snap;
    if (!sameSite) {
        const Sequence* source = site && snap ? snap->findSequence(site->source) : nullptr;
        m_sourceName = source ? text(source->name) : QString();
        m_columns = source ? multicamGridColumns(source->multicam->angles.size()) : 2;
        emit clipChanged();
    }
    if (!sameSite || !sameSnapshot) {
        emit anglesChanged();
        rebuildGrid();
    }
    pushGridTime();
}

std::size_t MulticamEditBridge::videoAngleNow() const {
    if (!m_site || !m_siteSnapshot || !m_controller)
        return 0;
    for (auto it = m_switches.rbegin(); it != m_switches.rend(); ++it) {
        if (it->clip == m_site->clip && m_target != QStringLiteral("audio"))
            return static_cast<std::size_t>(it->angle - 1);
    }
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = seq ? seq->findClip(m_site->clip) : std::nullopt;
    if (!loc)
        return 0;
    const std::size_t first = loc->clip().angles.first();
    return first < kMaxMulticamAngles ? first : 0;
}

QVariantList MulticamEditBridge::angles() const {
    QVariantList out;
    if (!m_site || !m_siteSnapshot || !m_controller)
        return out;
    const Sequence* source = m_siteSnapshot->findSequence(m_site->source);
    const SequencePtr seq = m_controller->currentSequence();
    if (!source || !source->multicam || !seq)
        return out;
    const auto loc = seq->findClip(m_site->clip);
    const Clip* video = loc ? &loc->clip() : nullptr;
    // What is heard: the audio clips of this source at the playhead, linked to the video clip (or all, without one).
    AngleSet heard;
    const Ticks t = m_controller->playhead();
    for (const TrackPtr& track : seq->audioTracks) {
        const auto index = track->clipIndexAt(t);
        if (!index)
            continue;
        const Clip& c = track->clips[*index];
        if (c.sequence != m_site->source)
            continue;
        if (video && video->linkGroup.isValid() && c.linkGroup != video->linkGroup)
            continue;
        heard.bits |= c.angles.bits;
    }
    const std::size_t shown = videoAngleNow();
    for (auto it = m_switches.rbegin(); it != m_switches.rend(); ++it) {
        if (it->clip == m_site->clip && m_target != QStringLiteral("video")) {
            heard = AngleSet::of(static_cast<std::size_t>(it->angle - 1));
            break;
        }
    }
    const auto& list = source->multicam->angles;
    for (std::size_t i = 0; i < list.size(); ++i) {
        out.append(QVariantMap{{QStringLiteral("angle"), static_cast<int>(i) + 1},
                               {QStringLiteral("name"), text(list[i].name)},
                               {QStringLiteral("video"), video && list[i].video.isValid() && i == shown},
                               {QStringLiteral("audio"), heard.contains(i)},
                               {QStringLiteral("hasVideo"), list[i].video.isValid()},
                               {QStringLiteral("keyText"), i < 9 ? QString::number(i + 1) : QString()}});
    }
    return out;
}

void MulticamEditBridge::rebuildGrid() {
    if (!m_viewOn || !m_site || !m_siteSnapshot) {
        if (m_renderer)
            m_renderer->clear();
        m_gridFrom.reset();
        m_gridState.reset();
        if (!m_gridImage.isNull()) {
            m_gridImage = {};
            emit gridChanged();
        }
        return;
    }
    SequencePtr source;
    for (const SequencePtr& s : m_siteSnapshot->sequences) {
        if (s->id == m_site->source)
            source = s;
    }
    if (!source)
        return;
    if (source == m_gridFrom && m_gridState)
        return; // the angles have not changed: keep the open source
    auto grid = multicamGridState(*m_siteSnapshot, m_site->source, m_columns);
    if (!grid) {
        m_gridError = text(grid.error().message);
        emit gridChanged();
        return;
    }
    m_gridFrom = source;
    m_gridState = std::make_shared<const ProjectState>(std::move(*grid));
    ++m_gridGeneration;
    m_costs.clear();
    m_gridFrames = 0;
    m_lastMs = 0;
    m_openMs = 0;
    if (!m_synchronous) {
        if (!m_renderer) {
            QPointer<MulticamEditBridge> self(this);
            m_renderer = std::make_unique<MulticamGridRenderer>([self](std::expected<MulticamGridFrame, QString> frame, std::uint64_t generation) {
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [self, frame = std::move(frame), generation]() mutable {
                        if (self)
                            self->onGridFrame(std::move(frame), generation);
                    },
                    Qt::QueuedConnection);
            });
        }
        m_renderer->setSource(m_gridState, m_site->source, m_gridSize,
                              m_controller ? m_controller->projectFile() : std::filesystem::path{}, m_gridGeneration);
    }
}

void MulticamEditBridge::pushGridTime() {
    if (!m_viewOn || !m_site || !m_gridState || !m_controller)
        return;
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = seq ? seq->findClip(m_site->clip) : std::nullopt;
    if (!loc)
        return;
    const Clip& c = loc->clip();
    const Ticks t = std::clamp(m_controller->playhead(), c.start(), c.end() - Ticks{1});
    const Ticks nested = c.sourceTimeAt(t);
    if (m_synchronous) {
        auto frame = renderMulticamGrid(*m_gridState, m_site->source, nested, m_gridSize, m_controller->projectFile());
        onGridFrame(frame ? std::expected<MulticamGridFrame, QString>(std::move(*frame))
                          : std::expected<MulticamGridFrame, QString>(std::unexpected(text(frame.error().message))),
                    m_gridGeneration);
        return;
    }
    if (m_renderer)
        m_renderer->setTime(nested, m_playing);
}

void MulticamEditBridge::refreshGrid() {
    m_gridFrom.reset();
    m_gridState.reset();
    rebuildGrid();
    pushGridTime();
}

void MulticamEditBridge::onGridFrame(std::expected<MulticamGridFrame, QString> frame, std::uint64_t generation) {
    if (generation != m_gridGeneration)
        return; // drawn for an older source
    if (!frame) {
        m_gridError = frame.error();
        emit gridChanged();
        return;
    }
    m_gridError.clear();
    m_gridImage = std::move(frame->image);
    m_backend = frame->backend;
    m_lastMs = frame->milliseconds;
    // A frame that opened the renderer is not a per-frame cost: it is reported as the open time.
    if (frame->opened) {
        m_openMs = frame->milliseconds;
    } else {
        m_costs.push_back(frame->milliseconds);
        while (m_costs.size() > 120)
            m_costs.pop_front();
    }
    ++m_gridFrames;
    emit gridChanged();
}

QVariantMap MulticamEditBridge::gridStats() const {
    std::vector<double> costs(m_costs.begin(), m_costs.end());
    double mean = 0, p95 = 0;
    if (!costs.empty()) {
        mean = std::accumulate(costs.begin(), costs.end(), 0.0) / static_cast<double>(costs.size());
        std::sort(costs.begin(), costs.end());
        p95 = costs[std::min(costs.size() - 1, static_cast<std::size_t>(0.95 * static_cast<double>(costs.size() - 1) + 0.5))];
    }
    return {{QStringLiteral("lastMs"), m_lastMs},
            {QStringLiteral("meanMs"), mean},
            {QStringLiteral("p95Ms"), p95},
            {QStringLiteral("steadyFrames"), static_cast<int>(m_costs.size())},
            {QStringLiteral("openMs"), m_openMs},
            {QStringLiteral("frames"), m_gridFrames},
            {QStringLiteral("size"), QStringLiteral("%1×%2").arg(m_gridSize.width()).arg(m_gridSize.height())},
            {QStringLiteral("backend"), m_backend},
            {QStringLiteral("error"), m_gridError}};
}

// ---- Switching -----------------------------------------------------------------------------------------------------

void MulticamEditBridge::onPlayingChanged(bool playing) {
    if (playing == m_playing)
        return;
    m_playing = playing;
    if (!playing)
        flushSwitches();
    pushGridTime();
}

bool MulticamEditBridge::switchAngle(int angle, bool cut) {
    if (!m_controller || !m_site || !m_siteSnapshot) {
        setMessage(tr("Put the playhead over a multi-camera clip"));
        return false;
    }
    const Sequence* source = m_siteSnapshot->findSequence(m_site->source);
    if (!source || !source->multicam || angle < 1 || angle > static_cast<int>(source->multicam->angles.size())) {
        setMessage(tr("This multi-camera clip has no angle %1").arg(angle));
        return false;
    }
    if (m_playing) {
        // Recorded now, sent when playback stops (one undo step).
        if (!m_switches.empty() && m_switches.back().clip == m_site->clip && m_switches.back().angle == angle)
            return true;
        m_switches.push_back({m_controller->playhead(), angle, m_site->clip});
        emit anglesChanged();
        return true;
    }
    Json params{{"clip", m_site->clip.value}, {"angle", angle}, {"target", m_target.toStdString()}};
    if (cut)
        params["at"] = m_controller->playhead().value;
    const command::CommandResult result = CommandJob::runNow(m_controller, services(), "multicam.switchAngle", params);
    setMessage(commandErrorText(result));
    return result.ok();
}

bool MulticamEditBridge::selectAngle(int angle) { return switchAngle(angle, false); }

bool MulticamEditBridge::cutToAngle(int angle) { return switchAngle(angle, true); }

void MulticamEditBridge::flushSwitches() {
    if (m_switches.empty() || !m_controller)
        return;
    // One request per clip (switches in time order), all in one batch: one undo step for the pass.
    std::vector<command::CommandRequest> requests;
    std::vector<ClipId> order;
    std::map<std::uint64_t, Json> byClip;
    for (const Recorded& r : m_switches) {
        if (!byClip.contains(r.clip.value)) {
            order.push_back(r.clip);
            byClip[r.clip.value] = Json::array();
        }
        byClip[r.clip.value].push_back(Json{{"at", r.at.value}, {"angle", r.angle}});
    }
    for (const ClipId clip : order) {
        command::CommandRequest request;
        request.command = "multicam.switchAngle";
        request.params = Json{{"sequence", m_controller->currentSequenceId().value},
                              {"clip", clip.value},
                              {"switches", byClip[clip.value]},
                              {"target", m_target.toStdString()}};
        requests.push_back(std::move(request));
    }
    m_switches.clear();
    command::EnginePolicy policy;
    policy.services = services();
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), policy);
    command::CommandBatch batch;
    batch.commands = std::move(requests);
    batch.label = "Multi-Camera Switches";
    const command::CommandResult result = engine.execute(batch);
    setMessage(commandErrorText(result));
    emit anglesChanged();
}

QString MulticamEditBridge::flatten(const QString& key) {
    std::optional<ClipId> clip = key.isEmpty() ? (m_site ? std::optional<ClipId>(m_site->clip) : std::nullopt) : parseClipKey(key);
    if (!clip)
        return tr("Choose a multi-camera clip");
    const command::CommandResult result = CommandJob::runNow(m_controller, services(), "multicam.flatten", Json{{"clip", clip->value}});
    setMessage(commandErrorText(result));
    return commandErrorText(result);
}

// ---- Commands ------------------------------------------------------------------------------------------------------

bool MulticamEditBridge::handlesCommand(const QString& id) {
    return id == QStringLiteral("multicam.toggleView") || cameraOf(id, kSelect) > 0 || cameraOf(id, kCut) > 0;
}

QVariantMap MulticamEditBridge::commandAvailability(const QString& id) const {
    if (!handlesCommand(id))
        return {{QStringLiteral("handled"), false}};
    bool enabled = true;
    QString reason;
    if (id != QStringLiteral("multicam.toggleView")) {
        const int n = std::max(cameraOf(id, kSelect), cameraOf(id, kCut));
        if (!m_viewOn) {
            enabled = false;
            reason = tr("Turn on the multi-camera view first");
        } else if (!m_site) {
            enabled = false;
            reason = tr("Put the playhead over a multi-camera clip");
        } else if (n > angles().size()) {
            enabled = false;
            reason = tr("This multi-camera clip has no angle %1").arg(n);
        }
    }
    return {{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), enabled}, {QStringLiteral("reason"), reason}};
}

bool MulticamEditBridge::runCommand(const QString& id) {
    if (id == QStringLiteral("multicam.toggleView")) {
        setViewOn(!m_viewOn);
        return true;
    }
    if (!m_viewOn)
        return false; // the camera keys act in the multi-camera view only
    if (const int n = cameraOf(id, kSelect))
        return selectAngle(n);
    if (const int n = cameraOf(id, kCut))
        return cutToAngle(n);
    return false;
}

} // namespace wabcut::app
