#include "SourceMonitorBridge.h"

#include "BridgeIds.h"
#include "KeymapDispatcher.h"
#include "PlaybackBridge.h"
#include "ProgramMonitor.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <wabcut/editor/CameraCards.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/playback/video/PlaybackController.h>

#include <QDir>
#include <QGuiApplication>
#include <QHash>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QSettings>
#include <QUrl>
#include <QWindow>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <system_error>

namespace wabcut::app {

using command::Json;
using command::SourcePatchSetting;

namespace {

constexpr int kTickMs = 33;
constexpr std::size_t kMaxRecent = 10;
constexpr std::int32_t kMaxLadder = 8;
constexpr const char* kSourceLoopKey = "monitor/sourceLoop"; // P.2: the Source Monitor's Loop button

editor::PlaybackSessionOptions sessionOptions() {
    editor::PlaybackSessionOptions options;
    const QString backend = qEnvironmentVariable("WABCUT_AUDIO_BACKEND").toLower();
    if (backend == QStringLiteral("null")) {
        options.audio.backend = audio::AudioBackend::Null;
    } else if (backend == QStringLiteral("null-realtime")) {
        options.audio.backend = audio::AudioBackend::NullRealtime;
    }
    options.video.scheduler.zeroCopy = qEnvironmentVariable("WABCUT_ZERO_COPY") != QStringLiteral("0");
    return options;
}

QString errorText(const command::CommandResult& result) {
    return result.ok() ? QString() : QString::fromStdString(result.error->message);
}

std::optional<StreamRef> parseSource(const QString& label) {
    if (label.size() < 2 || (label.front() != QLatin1Char('V') && label.front() != QLatin1Char('A')))
        return std::nullopt;
    bool ok = false;
    const int n = QStringView(label).mid(1).toInt(&ok);
    if (!ok || n < 1)
        return std::nullopt;
    return StreamRef{label.front() == QLatin1Char('V') ? StreamKind::Video : StreamKind::Audio, n - 1};
}

QString labelOf(StreamRef stream) { return QString::fromStdString(command::sourceStreamLabel(stream)); }

/// The source tracks Premiere shows for a media item: V1 for its picture, A1..An for its audio streams.
std::vector<StreamRef> sourceTracksOf(const MediaItem& media) {
    std::vector<StreamRef> out;
    if (media.info.hasVideo())
        out.push_back({StreamKind::Video, 0});
    for (std::size_t i = 0; i < media.info.audio.size(); ++i)
        out.push_back({StreamKind::Audio, static_cast<std::int32_t>(i)});
    return out;
}

/// Position of a track counted the way the headers list them (Vn..V1, A1..An), for "nearest" choices.
int rowOf(const Sequence& seq, TrackId track) {
    int row = 0;
    for (auto it = seq.videoTracks.rbegin(); it != seq.videoTracks.rend(); ++it, ++row) {
        if ((*it)->id == track)
            return row;
    }
    for (const TrackPtr& t : seq.audioTracks) {
        if (t->id == track)
            return row;
        ++row;
    }
    return -1;
}

/// Every TimelineView showing `controller`'s project, in any window.
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

} // namespace

// ---- Services: the patching for the source commands ---------------------------------------------------------

class SourceMonitorBridge::Services final : public command::SourceMonitorServices {
public:
    explicit Services(SourceMonitorBridge* bridge) : m_bridge(bridge) {}
    std::vector<SourcePatchSetting> sourcePatching(SequenceId sequence, MediaId media) const override {
        return m_bridge ? m_bridge->patching(sequence, media) : std::vector<SourcePatchSetting>{};
    }

private:
    QPointer<SourceMonitorBridge> m_bridge;
};

// ---- Construction ---------------------------------------------------------------------------------------------

SourceMonitorBridge::SourceMonitorBridge(QObject* parent)
    : QObject(parent), m_services(std::make_shared<Services>(this)) {
    m_tick.setInterval(kTickMs);
    connect(&m_tick, &QTimer::timeout, this, &SourceMonitorBridge::tick);
    // P.2: Loop follows the source's marks.
    m_loop = QSettings().value(QLatin1String(kSourceLoopKey), false).toBool();
    connect(this, &SourceMonitorBridge::marksChanged, this, [this] { applyLoop(); });
}

SourceMonitorBridge::~SourceMonitorBridge() {
    teardown();
}

SourceMonitorBridge* SourceMonitorBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new SourceMonitorBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setProgramPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
    }
    return bridge;
}

void SourceMonitorBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &SourceMonitorBridge::onProjectChanged);
        connect(controller, &ProjectController::mediaChanged, this, &SourceMonitorBridge::onProjectChanged);
        connect(controller, &ProjectController::sequenceChanged, this, &SourceMonitorBridge::patchingChanged);
        connect(controller, &ProjectController::sequencesChanged, this, &SourceMonitorBridge::patchingChanged);
        connect(controller, &ProjectController::sessionClosing, this, &SourceMonitorBridge::teardown,
                Qt::DirectConnection);
    }
}

void SourceMonitorBridge::setProgramPlayback(PlaybackBridge* playback) {
    if (m_programPlayback)
        disconnect(m_programPlayback, nullptr, this, nullptr);
    m_programPlayback = playback;
    // One monitor plays at a time (Premiere): the program starting pauses the source.
    if (playback) {
        connect(playback, &PlaybackBridge::transportChanged, this, [this] {
            if (m_programPlayback && m_programPlayback->playing() && playing())
                pause();
        });
    }
}

// ---- The source ---------------------------------------------------------------------------------------------

const MediaItem* SourceMonitorBridge::mediaItem() const {
    if (m_pending)
        return &m_pending->item;
    if (!m_controller || !m_media)
        return nullptr;
    return m_controller->snapshot()->findMedia(m_media);
}

edit::SourceMarks SourceMonitorBridge::marks() const {
    const MediaItem* item = mediaItem();
    return item ? edit::storedSourceMarks(*item) : edit::SourceMarks{};
}

Ticks SourceMonitorBridge::frame() const { return framesToTicks(1, m_rate); }

QString SourceMonitorBridge::mediaKeyText() const { return mediaKey(m_media); }

QString SourceMonitorBridge::timecodeOf(Ticks t) const {
    return QString::fromStdString(ticksToTimecode(t, m_rate, false));
}

double SourceMonitorBridge::fractionOf(Ticks t) const {
    if (!m_length || m_length->value <= 0)
        return 0.0;
    return std::clamp(static_cast<double>(t.value) / static_cast<double>(m_length->value), 0.0, 1.0);
}

bool SourceMonitorBridge::load(const QString& media) {
    const auto id = detail::parseId<MediaId>(QLatin1Char('m'), media);
    if (!id) {
        setMessage(tr("Choose a media item to load"));
        return false;
    }
    return loadMedia(*id, std::nullopt);
}

bool SourceMonitorBridge::loadFile(const QString& file) {
    const QString local = QUrl(file).isLocalFile() ? QUrl(file).toLocalFile() : file;
    const std::filesystem::path path = pathFromUtf8(QDir::fromNativeSeparators(local).toStdString());
    const std::filesystem::path project = m_controller ? m_controller->projectFile() : std::filesystem::path();
    auto described = editor::describeCameraClip(editor::cameraClipOf(path), project);
    if (!described || described->empty()) {
        setMessage(described ? tr("Nothing to show in %1").arg(local) : QString::fromStdString(described.error().message));
        return false;
    }
    // A P2 clip previews its picture (its sound is in other files: it comes in on import).
    MediaItem item = std::move(described->front());
    item.id = MediaId{};
    const std::optional<Ticks> extent = edit::sourceMarkExtent(item);
    const QString kind = !item.info.hasVideo()                 ? QStringLiteral("audio")
                         : item.info.isStillImage || !extent ? QStringLiteral("still")
                                                             : QStringLiteral("video");
    m_pending = Preview{file, std::move(item)};
    if (!loadItem(m_pending->item, MediaId{}, std::nullopt, kind)) {
        m_pending.reset();
        return false;
    }
    return true;
}

bool SourceMonitorBridge::adoptPreview() {
    if (!m_pending)
        return true;
    const std::optional<MediaId> id = m_importer ? m_importer(m_pending->file) : std::nullopt;
    const MediaItem* item = id && m_controller ? m_controller->snapshot()->findMedia(*id) : nullptr;
    if (!item) {
        setMessage(tr("%1 could not be imported").arg(m_name));
        return false;
    }
    m_pending.reset();
    m_media = *id;
    m_name = QString::fromStdString(item->name);
    std::erase(m_recent, *id);
    m_recent.insert(m_recent.begin(), *id);
    if (m_recent.size() > kMaxRecent)
        m_recent.resize(kMaxRecent);
    syncRedactions();
    emit sourceChanged();
    emit recentChanged();
    emit marksChanged();
    emit patchingChanged();
    return true;
}

bool SourceMonitorBridge::loadMedia(MediaId media, std::optional<Ticks> at) {
    if (!m_controller)
        return false;
    const ProjectSnapshot snap = m_controller->snapshot();
    const MediaItem* item = snap->findMedia(media);
    if (!item) {
        setMessage(tr("That media item is no longer in the project"));
        return false;
    }
    if (item->locator.offline && !item->isGenerator()) {
        setMessage(tr("%1 is offline: relink it to load it").arg(QString::fromStdString(item->name)));
        return false;
    }
    // Through the command, which checks the media and the position and reports the marks.
    Json params{{"media", media.value}};
    if (at)
        params["time"] = at->value;
    const command::CommandResult loaded = run("source.load", params);
    if (!loaded.ok()) {
        setMessage(errorText(loaded));
        return false;
    }
    m_pending.reset();
    return loadItem(*item, media, at, QString::fromStdString(loaded.data.value("kind", std::string())));
}

bool SourceMonitorBridge::loadItem(const MediaItem& source, MediaId media, std::optional<Ticks> at, const QString& kind) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : std::make_shared<const ProjectState>();
    const MediaItem* item = &source;
    // A one-clip project: the media on V1 (and A1..), in a sequence of its own size and rate.
    edit::SequenceSettings settings;
    settings.name = item->name;
    if (item->info.hasVideo()) {
        const VideoStreamInfo& v = item->info.video.front();
        if (v.width > 0 && v.height > 0) {
            settings.width = v.width;
            settings.height = v.height;
        }
        if (v.frameRate.num > 0 && v.frameRate.den > 0)
            settings.frameRate = v.frameRate;
    }
    MediaItem copy = *item;
    copy.markIn.reset(); // the private copy plays the whole media
    copy.markOut.reset();
    // A subclip plays its file in media time like any item (the monitor's positions and marks are media times); the
    // edits made from the monitor use the item itself, so a restricted subclip's range still bounds them.
    copy.subclip.reset();
    auto withMedia = edit::addMedia(ProjectState{}, copy);
    if (!withMedia)
        return false;
    auto withSequence = edit::addSequence(withMedia->state, settings);
    if (!withSequence)
        return false;
    auto session = std::make_unique<EditSession>(std::move(withSequence->state));
    if (!editor::dropMedia(*session, {.media = withMedia->id, .sequence = withSequence->id, .at = Ticks{}})) {
        setMessage(tr("%1 cannot be shown").arg(QString::fromStdString(item->name)));
        return false;
    }
    const Ticks length = session->snapshot()->findSequence(withSequence->id)->contentEnd();

    // Only media there is something to decode for gets a sequence to play (the demo's and the tests' media have no
    // files): the transport still runs, the picture stays black, and nothing tries to open a missing file.
    // 0.2: the file standing for an image sequence, spanned clip, layered file or RAW clip with Source Settings.
    const bool decodable = item->isGenerator() ||
                           (!item->locator.path.empty() && mediaFileExists(pathFromUtf8(item->locator.path)));
    teardown();
    if (m_programPlayback)
        m_programPlayback->pause();
    m_sourceSession = std::move(session);
    m_copyMedia = withMedia->id;
    m_redactionIds.clear();
    m_media = media;
    syncRedactions();
    m_playback = std::make_unique<editor::PlaybackSession>(*m_sourceSession, sessionOptions());
    m_playback->setSequence(decodable ? withSequence->id : SequenceId{});
    m_length = length.value > 0 ? std::optional<Ticks>(length) : std::nullopt; // applyLoop reads it
    if (m_programPlayback)
        m_playback->video().scheduler().setPreview(m_programPlayback->previewSettings());
    if (m_monitor)
        m_monitor->setPlaybackController(&m_playback->video());

    m_media = media;
    m_name = QString::fromStdString(item->name);
    m_hasVideo = item->info.hasVideo();
    m_hasAudio = item->info.hasAudio();
    m_aspect = settings.height > 0 ? static_cast<double>(settings.width) / settings.height : 16.0 / 9.0;
    m_kind = kind;
    m_rate = command::sourceTimecodeRate(*snap, *item);
    m_length = length.value > 0 ? std::optional<Ticks>(length) : std::nullopt;
    m_audioTried = false;
    m_wasPlaying = false;
    m_stopAt.reset();
    m_lastPosition = Ticks{-1};

    if (media) { // a preview joins the recent sources once it is imported
        std::erase(m_recent, media);
        m_recent.insert(m_recent.begin(), media);
        if (m_recent.size() > kMaxRecent)
            m_recent.resize(kMaxRecent);
    }

    const Ticks start = at ? *at : marks().in.value_or(Ticks{});
    seek(start);
    m_tick.start();
    setMessage({});
    emit sourceChanged();
    emit marksChanged();
    emit transportChanged();
    emit recentChanged();
    emit patchingChanged();
    emit showSourceRequested();
    return true;
}

void SourceMonitorBridge::unload() {
    if (!loaded())
        return;
    m_pending.reset();
    clearPreview();
    teardown();
    m_media = {};
    m_name.clear();
    m_kind.clear();
    m_hasVideo = m_hasAudio = false;
    m_length.reset();
    emit sourceChanged();
    emit positionChanged();
    emit marksChanged();
    emit transportChanged();
    emit patchingChanged();
}

void SourceMonitorBridge::teardown() {
    m_tick.stop();
    // The monitor lets go of the controller before it is destroyed.
    if (m_monitor)
        m_monitor->setPlaybackController(nullptr);
    m_playback.reset();
    m_sourceSession.reset();
}

void SourceMonitorBridge::attachMonitor(QObject* object) {
    auto* monitor = qobject_cast<ProgramMonitor*>(object);
    if (m_monitor == monitor)
        return;
    if (m_monitor)
        m_monitor->setPlaybackController(nullptr);
    m_monitor = monitor;
    if (m_monitor && m_playback)
        m_monitor->setPlaybackController(&m_playback->video());
}

void SourceMonitorBridge::onProjectChanged() {
    if (!m_controller)
        return;
    const ProjectSnapshot snap = m_controller->snapshot();
    // A project replaced, or the item deleted: let go of it (ids are only meaningful within one project).
    if (m_media) {
        const MediaItem* item = snap->findMedia(m_media);
        if (!item || QString::fromStdString(item->name) != m_name)
            unload();
    }
    if (m_media) {
        syncRedactions();
    }
    const auto before = m_recent.size();
    std::erase_if(m_recent, [&](MediaId id) { return snap->findMedia(id) == nullptr; });
    if (before != m_recent.size())
        emit recentChanged();
    emit marksChanged();
    emit patchingChanged();
}

void SourceMonitorBridge::syncRedactions() {
    if (!m_controller || !m_sourceSession || !m_media) {
        return;
    }
    const ProjectSnapshot snap = m_controller->snapshot();
    const ProjectSnapshot current = m_sourceSession->snapshot();
    ProjectState next = *current;
    std::vector<Redaction> mine;
    // A subclip shows its master's redactions and the reverse (model/RedactionMedia.h; review wave 5 E5-P1-1).
    for (const Redaction* covering : redactionsCovering(*snap, m_media)) {
        Redaction copy = *covering;
        auto [it, fresh] = m_redactionIds.try_emplace(covering->id.value);
        if (fresh) {
            it->second = next.ids.allocate<RedactionId>();
        }
        copy.id = it->second;
        copy.media = m_copyMedia;
        copy.exceptions.clear(); // no clip of the program is in here
        mine.push_back(std::move(copy));
    }
    // Sorted by id, as the model keeps them.
    std::sort(mine.begin(), mine.end(), [](const Redaction& a, const Redaction& b) { return a.id < b.id; });
    if (current->redactions.get() == mine) {
        return;
    }
    next.redactions = std::move(mine);
    [[maybe_unused]] const auto committed = m_sourceSession->commit("Redactions", next);
}

void SourceMonitorBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

// ---- Properties ---------------------------------------------------------------------------------------------

QString SourceMonitorBridge::durationText() const { return m_length ? timecodeOf(*m_length) : QString(); }

Ticks SourceMonitorBridge::positionTicks() const {
    if (!m_playback)
        return {};
    return snapToFrame(m_playback->position(), m_rate);
}

ProjectSnapshot SourceMonitorBridge::sourceProject() const {
    return m_sourceSession ? m_sourceSession->snapshot() : nullptr;
}

QString SourceMonitorBridge::positionText() const { return loaded() ? timecodeOf(positionTicks()) : QString(); }

double SourceMonitorBridge::positionFraction() const { return fractionOf(positionTicks()); }

double SourceMonitorBridge::inFraction() const {
    const auto m = marks();
    return m.in ? fractionOf(*m.in) : 0.0;
}

double SourceMonitorBridge::outFraction() const {
    const auto m = marks();
    return m.out ? fractionOf(*m.out) : 1.0;
}

QString SourceMonitorBridge::inText() const {
    const auto m = marks();
    return m.in ? timecodeOf(*m.in) : QString();
}

QString SourceMonitorBridge::outText() const {
    // Shown as the last marked frame, like the sequence Out.
    const auto m = marks();
    return m.out ? timecodeOf(std::max(Ticks{}, *m.out - frame())) : QString();
}

QString SourceMonitorBridge::inOutText() const {
    if (!loaded())
        return {};
    const auto m = marks();
    const Ticks end = m.out.value_or(m_length.value_or(Ticks{}));
    return timecodeOf(std::max(Ticks{}, end - m.in.value_or(Ticks{})));
}

QString SourceMonitorBridge::sequenceInOutText() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || !seq->markIn || !seq->markOut)
        return {};
    return m_controller->formatTimecode(*seq->markOut - *seq->markIn);
}

bool SourceMonitorBridge::playing() const { return m_playback && m_playback->isPlaying(); }

QVariantList SourceMonitorBridge::recent() const {
    QVariantList out;
    if (!m_controller)
        return out;
    const ProjectSnapshot snap = m_controller->snapshot();
    for (const MediaId id : m_recent) {
        const MediaItem* item = snap->findMedia(id);
        if (!item)
            continue;
        out.append(QVariantMap{{QStringLiteral("media"), mediaKey(id)},
                               {QStringLiteral("name"), QString::fromStdString(item->name)},
                               {QStringLiteral("kind"), item->info.hasVideo()
                                                            ? (item->info.isStillImage ? QStringLiteral("still")
                                                                                       : QStringLiteral("video"))
                                                            : QStringLiteral("audio")}});
    }
    return out;
}

// ---- Transport ----------------------------------------------------------------------------------------------

void SourceMonitorBridge::startAudio() {
    if (m_audioTried || !m_playback || !m_hasAudio)
        return;
    const MediaItem* item = mediaItem();
    if (!item || item->locator.offline || !mediaFileExists(pathFromUtf8(item->locator.path)))
        return;
    m_audioTried = true;
    if (auto started = m_playback->start(); !started)
        qWarning("Wabcut: source playback without sound: %s", started.error().message.c_str());
}

void SourceMonitorBridge::tick() {
    if (!m_playback)
        return;
    const bool isPlaying = m_playback->isPlaying();
    const Ticks position = positionTicks();
    if (isPlaying && m_length && position >= *m_length) {
        // The end (also without a picture to run off): park on the last frame.
        m_playback->pause();
        m_playback->scrub(std::max(Ticks{}, *m_length - frame()));
        m_stopAt.reset();
    } else if (isPlaying && m_stopAt && position >= *m_stopAt) {
        // Play In to Out: park on the last marked frame.
        m_stopAt.reset();
        m_playback->pause();
        m_playback->scrub(std::max(Ticks{}, snapToFrame(position, m_rate) - frame()));
    }
    if (position != m_lastPosition) {
        m_lastPosition = position;
        emit positionChanged();
    }
    if (m_playback->isPlaying() != m_wasPlaying) {
        m_wasPlaying = m_playback->isPlaying();
        if (!m_wasPlaying)
            m_stopAt.reset();
        emit transportChanged();
    }
}

void SourceMonitorBridge::play() {
    if (!m_playback)
        return;
    if (m_programPlayback)
        m_programPlayback->pause();
    startAudio();
    if (m_loop)
        applyLoop(true); // from the loop's start when outside it
    else if (m_length && positionTicks() >= *m_length - frame())
        m_playback->seek(Ticks{}); // at the end: from the top
    m_playback->play();
    tick();
}

// ---- Loop (P.2) -------------------------------------------------------------------------------------

void SourceMonitorBridge::setLoop(bool loop) {
    if (loop == m_loop)
        return;
    m_loop = loop;
    QSettings().setValue(QLatin1String(kSourceLoopKey), loop);
    m_stopAt.reset();
    applyLoop();
    emit loopChanged();
}

void SourceMonitorBridge::applyLoop(bool starting) {
    if (!m_playback)
        return;
    std::optional<TimeRange> range;
    if (m_loop && m_length) {
        const auto m = marks();
        const Ticks start = m.in.value_or(Ticks{});
        const Ticks end = std::min(m.out.value_or(*m_length), *m_length);
        if (end > start)
            range = TimeRange::fromStartEnd(start, end);
    }
    m_playback->setLoop(range);
    if (starting && range) {
        const Ticks here = positionTicks();
        if (here < range->start || here >= range->end() - frame())
            seek(range->start);
    }
}

void SourceMonitorBridge::pause() {
    if (!m_playback)
        return;
    m_playback->pause();
    m_stopAt.reset();
    tick();
}

void SourceMonitorBridge::togglePlay() {
    if (playing())
        pause();
    else
        play();
}

void SourceMonitorBridge::shuttle(int direction) {
    if (!m_playback)
        return;
    if (direction == 0) {
        pause();
        return;
    }
    if (m_programPlayback)
        m_programPlayback->pause();
    startAudio();
    const int sign = direction > 0 ? 1 : -1;
    const playback::TransportSpeed current = m_playback->speed();
    int next = sign;
    if (playing() && current.den == 1 && current.num != 0 && (current.num > 0) == (sign > 0))
        next = std::clamp(current.num * 2, -kMaxLadder, kMaxLadder);
    m_playback->setSpeed({next, 1});
    tick();
}

void SourceMonitorBridge::applyTransportIntent(int kind, int speedNum, int speedDen, int frames) {
    if (!m_playback)
        return;
    switch (static_cast<KeymapDispatcher::TransportKind>(kind)) {
    case KeymapDispatcher::TransportKind::ShuttleStep:
    case KeymapDispatcher::TransportKind::SetSpeed:
        if (speedNum == 0) {
            pause();
        } else {
            if (m_programPlayback)
                m_programPlayback->pause();
            startAudio();
            m_playback->setSpeed({speedNum, std::max(1, speedDen)});
            tick();
        }
        break;
    case KeymapDispatcher::TransportKind::StepFrames:
        stepFrames(frames);
        break;
    case KeymapDispatcher::TransportKind::Stop:
        pause();
        break;
    }
}

void SourceMonitorBridge::seek(Ticks t) {
    if (!m_playback)
        return;
    const Ticks last = m_length ? std::max(Ticks{}, *m_length - frame()) : t;
    const Ticks target = snapToFrame(std::clamp(t, Ticks{}, last), m_rate);
    if (m_playback->isPlaying())
        m_playback->seek(target);
    else
        m_playback->scrub(target);
    tick();
}

void SourceMonitorBridge::stepFrames(int frames) {
    if (!m_playback)
        return;
    m_playback->pause();
    seek(positionTicks() + Ticks{frame().value * frames});
}

void SourceMonitorBridge::goToStart() { seek(Ticks{}); }

void SourceMonitorBridge::goToEnd() {
    if (m_length)
        seek(*m_length);
}

void SourceMonitorBridge::seekFraction(double fraction) {
    if (!m_length)
        return;
    pause();
    seek(Ticks{static_cast<std::int64_t>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(m_length->value))});
}

void SourceMonitorBridge::scrubFraction(double fraction) {
    if (!m_length || !m_playback)
        return;
    m_playback->pause();
    const Ticks t{static_cast<std::int64_t>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(m_length->value))};
    const Ticks last = std::max(Ticks{}, *m_length - frame());
    m_playback->scrub(snapToFrame(std::min(t, last), m_rate));
    tick();
}

// ---- Marks --------------------------------------------------------------------------------------------------

bool SourceMonitorBridge::setMarks(Json params) {
    if (!loaded() || !adoptPreview())
        return false;
    const bool isClear = params.value("clear", false);
    params.erase("clear");
    params["media"] = m_media.value;
    const command::CommandResult r = run(isClear ? "source.clearMarks" : "source.setMarks", std::move(params));
    if (!r.ok()) {
        setMessage(errorText(r));
        return false;
    }
    setMessage({});
    emit marksChanged();
    return true;
}

void SourceMonitorBridge::markIn() {
    if (loaded())
        setMarks(Json{{"in", positionTicks().value}});
}

bool SourceMonitorBridge::addMarker() {
    if (!loaded() || !adoptPreview())
        return false;
    const command::CommandResult r = run("source.addMarker", Json{{"media", m_media.value}, {"time", positionTicks().value}});
    if (!r.ok()) {
        setMessage(errorText(r));
        return false;
    }
    setMessage({});
    return true;
}

void SourceMonitorBridge::markOut() {
    if (!loaded())
        return;
    // The end of the frame shown (Premiere's Out includes it).
    Ticks out = positionTicks() + frame();
    if (m_length)
        out = std::min(out, *m_length);
    setMarks(Json{{"out", out.value}});
}

void SourceMonitorBridge::markClip() {
    if (loaded() && m_length)
        setMarks(Json{{"in", 0}, {"out", m_length->value}});
}

void SourceMonitorBridge::clearIn() { setMarks(Json{{"clear", true}, {"out", false}}); }
void SourceMonitorBridge::clearOut() { setMarks(Json{{"clear", true}, {"in", false}}); }
void SourceMonitorBridge::clearInOut() { setMarks(Json{{"clear", true}}); }

void SourceMonitorBridge::goToIn() {
    if (const auto in = marks().in)
        seek(*in);
}

void SourceMonitorBridge::goToOut() {
    // Premiere parks on the last marked frame.
    if (const auto out = marks().out)
        seek(*out - frame());
}

void SourceMonitorBridge::playInToOut() {
    if (!m_playback)
        return;
    const auto m = marks();
    seek(m.in.value_or(Ticks{}));
    play();
    // Loop on: the range repeats (play() handed it over); off: stop at the Out.
    if (!m_loop)
        m_stopAt = m.out.value_or(m_length.value_or(Ticks::max()));
}

bool SourceMonitorBridge::runCommand(const QString& id) {
    static const QHash<QString, void (SourceMonitorBridge::*)()> kActions{
        {QStringLiteral("marker.markIn"), &SourceMonitorBridge::markIn},
        {QStringLiteral("marker.markOut"), &SourceMonitorBridge::markOut},
        {QStringLiteral("marker.markClip"), &SourceMonitorBridge::markClip},
        {QStringLiteral("marker.clearIn"), &SourceMonitorBridge::clearIn},
        {QStringLiteral("marker.clearOut"), &SourceMonitorBridge::clearOut},
        {QStringLiteral("marker.clearInAndOut"), &SourceMonitorBridge::clearInOut},
        {QStringLiteral("marker.goToIn"), &SourceMonitorBridge::goToIn},
        {QStringLiteral("marker.goToOut"), &SourceMonitorBridge::goToOut},
        {QStringLiteral("transport.playInToOut"), &SourceMonitorBridge::playInToOut},
        {QStringLiteral("monitor.loop"), &SourceMonitorBridge::toggleLoop},
        {QStringLiteral("transport.playStop"), &SourceMonitorBridge::togglePlay},
        {QStringLiteral("transport.goToStart"), &SourceMonitorBridge::goToStart},
        {QStringLiteral("transport.goToEnd"), &SourceMonitorBridge::goToEnd},
    };
    if (const auto it = kActions.constFind(id); it != kActions.constEnd()) {
        ((*this).*(*it))();
        return true;
    }
    if (id == QLatin1String("transport.stepBackward") || id == QLatin1String("transport.stepForward") ||
        id == QLatin1String("transport.stepBackwardFive") || id == QLatin1String("transport.stepForwardFive")) {
        const int n = id.endsWith(QLatin1String("Five")) ? 5 : 1;
        stepFrames(id.startsWith(QLatin1String("transport.stepBackward")) ? -n : n);
        return true;
    }
    if (id == QLatin1String("transport.shuttleBackward") || id == QLatin1String("transport.shuttleStop") ||
        id == QLatin1String("transport.shuttleForward")) {
        shuttle(id.endsWith(QLatin1String("Backward")) ? -1 : id.endsWith(QLatin1String("Stop")) ? 0 : 1);
        return true;
    }
    if (id == QLatin1String("sequence.reverseMatchFrame")) {
        reverseMatchFrame();
        return true;
    }
    if (id == QLatin1String("marker.add")) { // M5i: Premiere's M in the Source Monitor marks the clip
        addMarker();
        return true;
    }
    // Sequence-only commands do nothing while the Source Monitor has focus (Premiere): they never reach the
    // sequence behind it.
    static const QSet<QString> kSequenceOnly{
        QStringLiteral("marker.markSelection"),          QStringLiteral("sequence.matchFrame"),
        QStringLiteral("transport.goToPreviousEdit"),    QStringLiteral("transport.goToNextEdit"),
        QStringLiteral("transport.goToPreviousEditAnyTrack"), QStringLiteral("transport.goToNextEditAnyTrack"),
        QStringLiteral("transport.goToSelectedClipStart"),    QStringLiteral("transport.goToSelectedClipEnd"),
    };
    return kSequenceOnly.contains(id);
}

// ---- Patching -----------------------------------------------------------------------------------------------

std::vector<SourcePatchSetting> SourceMonitorBridge::patching(SequenceId sequence, MediaId media) const {
    if (!m_controller)
        return {};
    const MediaItem* item = m_controller->snapshot()->findMedia(media);
    return item ? patchingFor(sequence, *item) : std::vector<SourcePatchSetting>{};
}

std::vector<SourcePatchSetting> SourceMonitorBridge::patchingFor(SequenceId sequence, const MediaItem& source) const {
    if (!m_controller)
        return {};
    const ProjectSnapshot snap = m_controller->snapshot();
    const Sequence* seq = snap->findSequence(sequence);
    const MediaItem* item = &source;
    if (!seq)
        return {};
    const auto stored = m_patching.find(sequence.value);
    std::vector<SourcePatchSetting> out;
    std::size_t audioIndex = 0;
    // p02 audio (the audio UI tidy): a multi-track recording's streams default to the routing drops and default edits
    // use (A.10: the track named for each stream, Game / Mic / Discord / Music / Track N, or a new one of that name),
    // so the indicators show where an edit will really put them, not stream k on A(k+1).
    std::vector<edit::SourcePatch> named;
    if (edit::routesAudioByName(item->info)) {
        named = edit::audioPatchesByName(*seq, item->info, 0);
    }
    for (const StreamRef stream : sourceTracksOf(*item)) {
        std::optional<SourcePatchSetting> kept;
        if (stored != m_patching.end()) {
            for (const SourcePatchSetting& s : stored->second) {
                if (s.source != stream)
                    continue;
                const Track* track = s.destination ? seq->findTrack(s.destination) : nullptr;
                if (!s.destination || (track && streamKindOf(track->kind) == stream.kind))
                    kept = s;
            }
        }
        if (kept) {
            out.push_back(*kept);
        } else if (stream.kind == StreamKind::Video) {
            out.push_back({stream, seq->videoTracks.empty() ? TrackId{} : seq->videoTracks.front()->id,
                           !seq->videoTracks.empty()});
        } else if (!named.empty()) {
            const auto byName = std::find_if(named.begin(), named.end(), [&](const edit::SourcePatch& p) { return p.source == stream; });
            if (byName != named.end()) {
                // A new track (no destination) is still an enabled patch: the edit makes the track.
                out.push_back({stream, byName->destination, true, byName->newTrack});
            } else {
                out.push_back({stream, TrackId{}, false});
            }
        } else {
            const bool has = audioIndex < seq->audioTracks.size();
            out.push_back({stream, has ? seq->audioTracks[audioIndex]->id : TrackId{}, has});
        }
        if (stream.kind == StreamKind::Audio)
            ++audioIndex;
    }
    // Two indicators on one track: the later one gives way (switched off), as the command requires. Patches to new
    // tracks (no destination yet) never share one.
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (out[i].enabled && out[j].enabled && out[i].destination && out[i].destination == out[j].destination)
                out[i].enabled = false;
        }
    }
    return out;
}

std::vector<SourcePatchSetting> SourceMonitorBridge::currentPatching() const {
    if (m_controller && m_pending)
        return patchingFor(m_controller->currentSequenceId(), m_pending->item);
    if (!m_controller || !m_media)
        return {};
    return patching(m_controller->currentSequenceId(), m_media);
}

Json SourceMonitorBridge::patchesJson(const std::vector<SourcePatchSetting>& patches) const {
    Json list = Json::array();
    for (const SourcePatchSetting& p : patches) {
        Json patch{{"source", command::sourceStreamLabel(p.source)},
                   {"track", p.destination ? Json(p.destination.value) : Json(nullptr)},
                   {"enabled", p.enabled}};
        if (!p.newTrack.empty())
            patch["newTrack"] = p.newTrack; // p02 audio: the named routing's track (a new one when there is no track)
        list.push_back(std::move(patch));
    }
    return list;
}

void SourceMonitorBridge::storePatching(std::vector<SourcePatchSetting> patches) {
    if (!m_controller || !adoptPreview() || !m_media)
        return;
    // Checked (and put in order) by the typed command; the result is view state kept here.
    const command::CommandResult r = run("sequence.setPatches", Json{{"media", m_media.value}, {"patches", patchesJson(patches)}});
    if (!r.ok()) {
        setMessage(errorText(r));
        return;
    }
    std::vector<SourcePatchSetting>& stored = m_patching[m_controller->currentSequenceId().value];
    for (const SourcePatchSetting& p : patches) {
        std::erase_if(stored, [&](const SourcePatchSetting& s) { return s.source == p.source; });
        stored.push_back(p);
    }
    setMessage({});
    emit patchingChanged();
}

QVariantList SourceMonitorBridge::indicators() const {
    QVariantList out;
    for (const SourcePatchSetting& p : currentPatching()) {
        // p02 audio: `name` is the routed track's name (Mic, Game...) when the stream goes by name; `newTrack` says the
        // edit will make that track.
        out.append(QVariantMap{{QStringLiteral("source"), labelOf(p.source)},
                               {QStringLiteral("kind"), p.source.kind == StreamKind::Video ? QStringLiteral("video")
                                                                                           : QStringLiteral("audio")},
                               {QStringLiteral("track"), trackKey(p.destination)},
                               {QStringLiteral("enabled"), p.enabled},
                               {QStringLiteral("name"), QString::fromStdString(p.newTrack)},
                               {QStringLiteral("newTrack"), !p.destination && !p.newTrack.empty()}});
    }
    return out;
}

QVariantMap SourceMonitorBridge::patchMap() const {
    QVariantMap out;
    for (const SourcePatchSetting& p : currentPatching()) {
        if (!p.destination)
            continue;
        const QString key = trackKey(p.destination);
        if (out.contains(key) && !p.enabled)
            continue; // an indicator that is on wins the slot
        // p02 audio: a stream routed by name shows its name (Mic, Game...) on the indicator, not only its position.
        out.insert(key, QVariantMap{{QStringLiteral("source"), labelOf(p.source)},
                                    {QStringLiteral("enabled"), p.enabled},
                                    {QStringLiteral("name"), QString::fromStdString(p.newTrack)}});
    }
    return out;
}

bool SourceMonitorBridge::canEdit() const {
    const auto patches = currentPatching();
    return loaded() && std::ranges::any_of(patches, [](const SourcePatchSetting& p) {
               return p.enabled && (p.destination || !p.newTrack.empty()); // p02 audio: or a new track the edit makes
           });
}

void SourceMonitorBridge::patchTrack(const QString& track) {
    const auto id = parseTrackKey(track);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const Track* target = id && seq ? seq->findTrack(*id) : nullptr;
    if (!target || !loaded())
        return;
    std::vector<SourcePatchSetting> patches = currentPatching();
    const StreamKind kind = streamKindOf(target->kind);
    SourcePatchSetting* chosen = nullptr;
    // A source track of that kind that is not patched or is off, else the one patched nearest.
    for (SourcePatchSetting& p : patches) {
        if (p.source.kind == kind && (!p.destination || !p.enabled)) {
            chosen = &p;
            break;
        }
    }
    if (!chosen) {
        const int row = rowOf(*seq, *id);
        int best = std::numeric_limits<int>::max();
        for (SourcePatchSetting& p : patches) {
            const int distance = std::abs(rowOf(*seq, p.destination) - row);
            if (p.source.kind == kind && distance < best) {
                best = distance;
                chosen = &p;
            }
        }
    }
    if (!chosen)
        return;
    for (SourcePatchSetting& p : patches) {
        if (&p != chosen && p.destination == *id)
            p.enabled = false; // a switched-off indicator left there gives way
    }
    chosen->destination = *id;
    chosen->enabled = true;
    storePatching(std::move(patches));
}

void SourceMonitorBridge::movePatch(const QString& source, const QString& track) {
    const auto stream = parseSource(source);
    const auto id = parseTrackKey(track);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const Track* target = id && seq ? seq->findTrack(*id) : nullptr;
    if (!stream || !target || streamKindOf(target->kind) != stream->kind)
        return;
    std::vector<SourcePatchSetting> patches = currentPatching();
    const auto moving = std::ranges::find_if(patches, [&](const SourcePatchSetting& p) { return p.source == *stream; });
    if (moving == patches.end() || (moving->destination == *id && moving->enabled))
        return;
    for (SourcePatchSetting& p : patches) {
        if (p.source != *stream && p.destination == *id)
            p.destination = moving->destination; // the indicator there takes the other's place
    }
    moving->destination = *id;
    moving->enabled = true;
    storePatching(std::move(patches));
}

void SourceMonitorBridge::togglePatch(const QString& source) {
    const auto stream = parseSource(source);
    if (!stream)
        return;
    std::vector<SourcePatchSetting> patches = currentPatching();
    const auto it = std::ranges::find_if(patches, [&](const SourcePatchSetting& p) { return p.source == *stream; });
    if (it == patches.end() || !it->destination)
        return;
    it->enabled = !it->enabled;
    if (it->enabled) {
        for (SourcePatchSetting& p : patches) {
            if (&p != &*it && p.enabled && p.destination == it->destination)
                p.enabled = false;
        }
    }
    storePatching(std::move(patches));
}

void SourceMonitorBridge::setTargeted(const QString& track, bool targeted) {
    const auto id = parseTrackKey(track);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!id || !seq)
        return;
    std::vector<TrackId> targets = edit::targetedTracks(*seq);
    std::erase(targets, *id);
    if (targeted)
        targets.push_back(*id);
    Json list = Json::array();
    for (const TrackId t : targets)
        list.push_back(t.value);
    const command::CommandResult r = run("sequence.setTargets", Json{{"tracks", list}});
    setMessage(errorText(r));
}

// ---- Editing ------------------------------------------------------------------------------------------------

command::CommandResult SourceMonitorBridge::run(const std::string& name, Json params, bool dryRun) {
    command::CommandResult result;
    if (!m_controller) {
        result.error = command::CommandError{command::ErrorCode::InvalidRequest, "no project", nullptr};
        return result;
    }
    command::EnginePolicy policy;
    policy.services = m_services;
    command::CommandEngine engine(m_controller->session(), command::builtinCommands(), policy);
    command::CommandRequest request;
    request.command = name;
    if (m_controller->currentSequenceId().isValid() && !params.contains("sequence"))
        params["sequence"] = m_controller->currentSequenceId().value;
    request.params = std::move(params);
    request.dryRun = dryRun;
    return engine.execute(request);
}

bool SourceMonitorBridge::needsFit() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto m = marks();
    if (!loaded() || !seq || !m.in || !m.out || !seq->markIn || !seq->markOut)
        return false;
    const Ticks source = *m.out - *m.in;
    const Ticks sequence = *seq->markOut - *seq->markIn;
    return std::llabs(source.value - sequence.value) >= framesToTicks(1, seq->frameRate).value;
}

Json SourceMonitorBridge::editParams(const QString& fit) const {
    Json params{{"media", m_media.value},
                {"at", m_controller ? m_controller->playhead().value : 0},
                {"patches", patchesJson(currentPatching())}};
    if (!fit.isEmpty())
        params["fit"] = fit.toStdString();
    return params;
}

QString SourceMonitorBridge::edit(bool insert, const QString& fit) {
    if (!loaded()) {
        setMessage(tr("Load a source in the Source Monitor first"));
        return m_message;
    }
    if (!adoptPreview())
        return m_message;
    if (fit.isEmpty() && needsFit())
        return QStringLiteral("fit");
    clearPreview();
    const command::CommandResult r = run(insert ? "edit.insertFromSource" : "edit.overwriteFromSource", editParams(fit));
    if (!r.ok()) {
        setMessage(errorText(r));
        return m_message;
    }
    setMessage({});
    // Premiere parks the playhead after the new clips.
    if (r.data.contains("playhead") && m_controller)
        m_controller->setPlayhead(Ticks{r.data["playhead"].value("ticks", std::int64_t{0})});
    return {};
}

void SourceMonitorBridge::previewEdit(bool insert, const QString& fit) {
    if (!loaded() || !m_controller || m_pending) // a preview is not imported just to show the ghost
        return;
    const QString used = fit.isEmpty() && needsFit() ? QStringLiteral("ignoreSourceOut") : fit;
    const command::CommandResult r =
        run(insert ? "edit.insertFromSource" : "edit.overwriteFromSource", editParams(used), true);
    if (!r.ok() || !r.preview) {
        clearPreview();
        return;
    }
    showPreview(r, insert);
}

void SourceMonitorBridge::showPreview(const command::CommandResult& dryRun, bool insert) {
    const SequenceId seqId = m_controller->currentSequenceId();
    const Sequence* after = dryRun.preview->findSequence(seqId);
    const SequencePtr before = m_controller->currentSequence();
    if (!after || !before)
        return;
    TimelineView::ExternalPreview ghost;
    for (const Json& placed : dryRun.data.value("placed", Json::array())) {
        const auto location = after->findClip(ClipId{placed.get<std::uint64_t>()});
        if (!location)
            continue;
        const Clip& clip = location->clip();
        ghost.added.push_back({location->track->id, clip.start(), clip.end()});
        if (insert)
            continue;
        // Overwrite: what it replaces.
        if (const Track* track = before->findTrack(location->track->id)) {
            for (const Clip& old : track->clips) {
                const Ticks start = std::max(old.start(), clip.start());
                const Ticks end = std::min(old.end(), clip.end());
                if (start < end)
                    ghost.removed.push_back({track->id, start, end});
            }
        }
    }
    if (dryRun.data.contains("at"))
        ghost.markers.push_back(Ticks{dryRun.data["at"].value("ticks", std::int64_t{0})});
    for (TimelineView* view : timelineViews(m_controller))
        view->setExternalPreview(ghost);
    m_previewShown = true;
}

void SourceMonitorBridge::clearPreview() {
    if (!m_previewShown)
        return;
    m_previewShown = false;
    for (TimelineView* view : timelineViews(m_controller))
        view->clearExternalPreview();
}

bool SourceMonitorBridge::dropAt(const QString& timecode, const QString& track, bool overwrite, const QString& streams) {
    const MediaItem* item = mediaItem();
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!item || !seq)
        return false;
    const TrackId under = parseTrackKey(track).value_or(TrackId{});
    const bool video = streams != QLatin1String("audio");
    const bool audio = streams != QLatin1String("video");
    // Where the marked range lands: the streams that follow the pointer's track avoid tracks with other clips there.
    std::optional<TimeRange> landing;
    if (const auto at = parseTimecode(timecode.toStdString(), seq->frameRate, seq->dropFrame)) {
        const edit::SourceMarks m = marks();
        const Ticks in = m.in.value_or(Ticks{});
        const Ticks out = m.out.value_or(edit::sourceMarkExtent(*item).value_or(in + edit::kDefaultStillDuration));
        if (out > in)
            landing = TimeRange{*at, out - in};
    }
    Json patches = Json::array();
    for (const edit::SourcePatch& p : editor::patchesForDrop(*seq, *item, under, landing)) {
        if ((p.source.kind == StreamKind::Video && video) || (p.source.kind == StreamKind::Audio && audio)) {
            // A.10: a multi-track recording's stream names its track; without a track it gets a new one of that name.
            Json patch{{"source", command::sourceStreamLabel(p.source)}};
            if (p.destination)
                patch["track"] = p.destination.value;
            if (!p.newTrack.empty())
                patch["newTrack"] = p.newTrack;
            patches.push_back(std::move(patch));
        }
    }
    if (patches.empty()) {
        setMessage(streams == QLatin1String("audio") ? tr("The source has no audio to drop there")
                                                     : tr("The source has no picture to drop there"));
        return false;
    }
    if (!adoptPreview())
        return false;
    const command::CommandResult r = run(overwrite ? "edit.overwriteFromSource" : "edit.insertFromSource",
                                         Json{{"media", m_media.value},
                                              {"at", timecode.toStdString()},
                                              {"patches", patches},
                                              {"sequenceMarks", false}});
    if (!r.ok()) {
        setMessage(errorText(r));
        return false;
    }
    setMessage({});
    std::vector<ClipId> placed;
    for (const Json& id : r.data.value("placed", Json::array()))
        placed.push_back(ClipId{id.get<std::uint64_t>()});
    m_controller->setSelection(placed, placed.empty() ? ClipId{} : placed.front());
    return true;
}

// ---- Match frame --------------------------------------------------------------------------------------------

bool SourceMonitorBridge::matchFrame() {
    if (!m_controller)
        return false;
    const command::CommandResult r = run("sequence.matchFrame", Json{{"time", m_controller->playhead().value}});
    if (!r.ok()) {
        setMessage(errorText(r));
        return false;
    }
    const MediaId media{r.data.value("media", std::uint64_t{0})};
    const Ticks at{r.data["sourceTime"].value("ticks", std::int64_t{0})};
    return loadMedia(media, at);
}

bool SourceMonitorBridge::reverseMatchFrame() {
    if (!m_controller || !m_media) // a Media Browser preview is in no sequence yet
        return false;
    const command::CommandResult r =
        run("sequence.reverseMatchFrame",
            Json{{"media", m_media.value}, {"time", positionTicks().value}, {"from", m_controller->playhead().value}});
    if (!r.ok()) {
        setMessage(errorText(r));
        return false;
    }
    setMessage({});
    m_controller->setPlayhead(Ticks{r.data["time"].value("ticks", std::int64_t{0})});
    emit showProgramRequested();
    return true;
}

} // namespace wabcut::app
