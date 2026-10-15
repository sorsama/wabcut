#include "PlaybackBridge.h"

#include "BridgeIds.h"
#include "CaptionStyleMapping.h"
#include "CaptionsBridge.h"
#include "TranscriptBridge.h"

#include <QQmlEngine>
#include <QSettings>
#include "KeymapDispatcher.h"
#include "ProgramMonitor.h"
#include "ProjectController.h"

#include <wabcut/editor/SourceEdit.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/playback/video/PlaybackController.h>

#include <QQmlEngine>
#include <QtGlobal>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace wabcut::app {

namespace {

constexpr int kTickMs = 33;          // playhead follow + transport state
constexpr qint64 kStatsPeriodMs = 500;
constexpr std::int32_t kMaxLadder = 8;

editor::PlaybackSessionOptions sessionOptions() {
    editor::PlaybackSessionOptions options;
    const QString backend = qEnvironmentVariable("WABCUT_AUDIO_BACKEND").toLower();
    if (backend == QStringLiteral("null")) {
        options.audio.backend = audio::AudioBackend::Null;
    } else if (backend == QStringLiteral("null-realtime")) {
        options.audio.backend = audio::AudioBackend::NullRealtime;
    }
    // The program monitor shares its D3D11 device with the decoders (ProgramMonitor); elsewhere this falls back
    // to CPU frames by itself.
    options.video.scheduler.zeroCopy = qEnvironmentVariable("WABCUT_ZERO_COPY") != QStringLiteral("0");
    return options;
}

QString speedMagnitude(std::int32_t num, std::int32_t den) {
    const std::int32_t n = std::abs(num);
    if (den == 1) {
        return QString::number(n);
    }
    if (n == 1 && den == 4) {
        return QStringLiteral("¼");
    }
    if (n == 1 && den == 2) {
        return QStringLiteral("½");
    }
    return QString::number(static_cast<double>(n) / den, 'g', 3);
}

} // namespace

PlaybackBridge::PlaybackBridge(ProjectController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {
    Q_ASSERT(controller);
    m_session = std::make_unique<editor::PlaybackSession>(controller->session(), sessionOptions());
    m_session->setSequence(playableSequence());
    m_session->scrub(controller->playhead());
    m_fpsText = controller->frameRateText();

    connect(controller, &ProjectController::playheadChanged, this, &PlaybackBridge::onPlayheadChanged);
    connect(controller, &ProjectController::sequenceChanged, this, &PlaybackBridge::onSequenceChanged);
    connect(controller, &ProjectController::mediaChanged, this, &PlaybackBridge::onSequenceChanged);
    connect(controller, &ProjectController::sessionClosing, this, &PlaybackBridge::teardown, Qt::DirectConnection);

    m_tick.setInterval(kTickMs);
    connect(&m_tick, &QTimer::timeout, this, &PlaybackBridge::tick);
    m_tick.start();
    // Captions: the Captions/Transcript singletons are looked up once the engine has finished creating this one.
    m_captionKaraoke = QSettings().value(QLatin1String(kCaptionKaraokeKey), false).toBool();
    QTimer::singleShot(0, this, &PlaybackBridge::attachCaptions);
    m_statsClock.start();
}

PlaybackBridge::~PlaybackBridge() {
    teardown();
}

void PlaybackBridge::attachCaptions() {
    if (m_captions) {
        return;
    }
    QQmlEngine* engine = qmlEngine(this);
    if (!engine) {
        return;
    }
    m_captions = engine->singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    m_transcriptBridge = engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    if (m_captions) {
        connect(m_captions, &CaptionsBridge::styleChanged, this, &PlaybackBridge::applyCaptions);
        connect(m_captions, &CaptionsBridge::tracksChanged, this, &PlaybackBridge::applyCaptions);
        connect(m_captions, &CaptionsBridge::cuesChanged, this, &PlaybackBridge::applyCaptions);
    }
    if (m_transcriptBridge) {
        connect(m_transcriptBridge, &TranscriptBridge::transcriptChanged, this, &PlaybackBridge::applyCaptions);
    }
    applyCaptions();
}

void PlaybackBridge::setCaptionKaraoke(bool karaoke) {
    if (karaoke == m_captionKaraoke) {
        return;
    }
    m_captionKaraoke = karaoke;
    QSettings().setValue(QLatin1String(kCaptionKaraokeKey), karaoke);
    applyCaptions();
    emit captionsChanged();
}

std::shared_ptr<const playback::CaptionWordTimings> captionWordTimings(const CaptionTrack& track,
                                                                       const ai::edit::SequenceTranscript& transcript) {
    auto words = std::make_shared<playback::CaptionWordTimings>();
    for (const CaptionCue& cue : track.cues) {
        const auto [first, last] = transcript.wordsIn(TimeRange{cue.start, cue.duration});
        if (last > first && last - first == render::captionWordSpans(cue.text).size()) {
            std::vector<playback::CaptionWordTiming> timings;
            for (std::size_t i = first; i < last; ++i) {
                timings.push_back({transcript[i].timeline.start, transcript[i].timeline.end()});
            }
            words->cues[{track.id.value, cue.start.value}] = std::move(timings);
        }
    }
    return words;
}

void PlaybackBridge::applyCaptions() {
    std::optional<playback::CaptionSettings> settings;
    if (m_captions && m_captions->overlay()) {
        playback::CaptionSettings c; // no style override: the track's own CaptionStyle, as export burns it in (I.9)
        if (const auto id = parseCaptionTrackKey(m_captions->track())) {
            c.track = *id;
        }
        c.karaoke = m_captionKaraoke;
        const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
        if (c.karaoke && seq && m_transcriptBridge) {
            // Word times from the transcript where its words line up with the cue's.
            if (const CaptionTrack* track = playback::captionTrackFor(*seq, c)) {
                c.words = captionWordTimings(*track, m_transcriptBridge->sequenceTranscript());
            }
        }
        settings = std::move(c);
    }
    const bool shownChanged = settings.has_value() != m_captionSettings.has_value();
    m_captionSettings = std::move(settings);
    if (m_session) {
        m_session->video().scheduler().setCaptions(m_captionSettings);
    }
    if (shownChanged) {
        emit captionsChanged();
    }
}

void PlaybackBridge::setPreviewSettings(playback::PreviewSettings preview) {
    m_preview = std::move(preview);
    if (m_session) {
        m_session->video().scheduler().setPreview(m_preview);
    }
    if (m_sourceVideo) {
        m_sourceVideo->scheduler().setPreview(m_preview);
    }
}

void PlaybackBridge::setColorPreview(playback::ColorPreview preview) {
    m_colorPreview = std::move(preview);
    if (m_session) {
        m_session->video().scheduler().setColorPreview(m_colorPreview);
    }
}

PlaybackBridge* PlaybackBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* controller = qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    return new PlaybackBridge(controller);
}

void PlaybackBridge::teardown() {
    if (m_session) {
        emit sessionEnding(); // still playable: a voice-over pass places its take now
    }
    m_tick.stop();
    for (const QPointer<ProgramMonitor>& monitor : std::as_const(m_monitors)) {
        if (monitor) {
            monitor->setPlaybackController(nullptr);
        }
    }
    m_monitors.clear();
    m_sourceVideo.reset();
    m_sourceSession.reset();
    m_session.reset(); // its threads read the controller's session: stop them while it still exists
    m_controller = nullptr;
}

playback::PlaybackController* PlaybackBridge::activeController() const {
    if (m_sourceVideo) {
        return m_sourceVideo.get();
    }
    return m_session ? &m_session->video() : nullptr;
}

void PlaybackBridge::startAudio() {
    if (m_audioTried || !m_session || !m_controller) {
        return;
    }
    // Open the audio device only once there is something to hear (not for the demo project, whose media has no
    // files): the transport runs on a free-run clock until then.
    const ProjectSnapshot snap = m_controller->session().snapshot();
    const bool audible = std::ranges::any_of(snap->media, [](const MediaPtr& media) {
        std::error_code ec;
        return media->info.hasAudio() && !media->locator.offline &&
               std::filesystem::is_regular_file(pathFromUtf8(media->locator.path), ec);
    });
    if (!audible) {
        return;
    }
    m_audioTried = true;
    if (auto started = m_session->start(); !started) {
        qWarning("Wabcut: playback without sound: %s", started.error().message.c_str());
    }
    updateStats();
}

bool PlaybackBridge::ensureAudio() {
    if (!m_session || !m_controller) {
        return false;
    }
    if (!m_session->hasSound()) {
        if (auto started = m_session->start(); !started) {
            qWarning("Wabcut: no audio output: %s", started.error().message.c_str());
        }
        m_audioTried = m_audioTried || m_session->hasSound(); // started: startAudio() has nothing left to do
        updateStats();
    }
    return m_session->hasSound();
}

// ---- Properties -------------------------------------------------------------------------------------

bool PlaybackBridge::playing() const {
    const playback::PlaybackController* active = activeController();
    return active && active->isPlaying();
}

int PlaybackBridge::speedNum() const {
    const playback::PlaybackController* active = activeController();
    return active ? active->transportSpeed().num : 0;
}

int PlaybackBridge::speedDen() const {
    const playback::PlaybackController* active = activeController();
    return active ? std::max(1, active->transportSpeed().den) : 1;
}

double PlaybackBridge::speed() const {
    return static_cast<double>(speedNum()) / speedDen();
}

QString PlaybackBridge::speedText() const {
    const int num = speedNum();
    if (num == 0) {
        return tr("■ 0×");
    }
    const QString magnitude = speedMagnitude(num, speedDen());
    return num < 0 ? tr("◀ %1×").arg(magnitude) : tr("▶ %1×").arg(magnitude);
}

QString PlaybackBridge::sourceTimeText() const {
    if (!m_sourceVideo || !m_sourceSession) {
        return {};
    }
    const ProjectSnapshot snap = m_sourceSession->snapshot();
    const Sequence* seq = snap->sequences.empty() ? nullptr : snap->sequences.front().get();
    const Rate rate = seq ? seq->frameRate : kRate30;
    return QString::fromStdString(ticksToTimecode(m_sourceVideo->position(), rate, seq && seq->dropFrame));
}

// ---- Ticks and stats --------------------------------------------------------------------------------

void PlaybackBridge::tick() {
    playback::PlaybackController* active = activeController();
    if (!active) {
        return;
    }
    const bool isPlaying = active->isPlaying();
    if (!m_sourceVideo && m_controller && (isPlaying || m_wasPlaying)) {
        m_settingPlayhead = true;
        m_controller->setPlayhead(active->position());
        m_settingPlayhead = false;
    }
    if (m_sourceVideo && (isPlaying || m_wasPlaying)) {
        emit sourceTimeChanged();
    }
    const playback::TransportSpeed speed = active->transportSpeed();
    if (isPlaying != m_wasPlaying || speed.num != m_lastSpeedNum || speed.den != m_lastSpeedDen) {
        if (isPlaying && !m_wasPlaying) {
            const playback::PlaybackStats s = active->stats();
            m_dropped = 0;
            m_droppedAtLastSample = s.dropped;
            m_presentedAtLastSample = s.presented;
            m_statsClock.restart();
        }
        m_wasPlaying = isPlaying;
        m_lastSpeedNum = speed.num;
        m_lastSpeedDen = speed.den;
        emit transportChanged();
    }
    if (m_statsClock.elapsed() >= kStatsPeriodMs) {
        updateStats();
    }
}

void PlaybackBridge::updateStats() {
    playback::PlaybackController* active = activeController();
    if (!active) {
        return;
    }
    const qint64 elapsed = std::max<qint64>(1, m_statsClock.restart());
    const playback::PlaybackStats s = active->stats();
    const bool isPlaying = active->isPlaying();
    const std::uint64_t presented = s.presented - std::min(s.presented, m_presentedAtLastSample);
    const std::uint64_t dropped = s.dropped - std::min(s.dropped, m_droppedAtLastSample);
    if (isPlaying && presented == 0) {
        // Nothing reached the screen in this period (monitor hidden, minimized or not attached): no rate to show,
        // and composites superseded meanwhile were not dropped in front of anyone.
        m_measuredFps = 0.0;
        m_fpsText = tr("— fps");
    } else if (isPlaying) {
        m_measuredFps = static_cast<double>(presented) * 1000.0 / static_cast<double>(elapsed);
        m_fpsText = tr("%1 fps").arg(m_measuredFps, 0, 'f', 2);
        m_dropped += static_cast<int>(dropped);
    } else if (m_controller) {
        m_fpsText = m_controller->frameRateText();
    }
    m_presentedAtLastSample = s.presented;
    m_droppedAtLastSample = s.dropped;
    m_bypassedEffects = s.bypassedEffects;
    m_decodeBackend = QString::fromStdString(s.decodeBackends);
    const editor::PlaybackSessionStats session = m_session ? m_session->stats() : editor::PlaybackSessionStats{};
    m_audioBackend = session.hasSound ? QString::fromStdString(session.audioBackend) : tr("off");
    m_statsText = tr("Presented %1 · dropped %2 · late %3 · black %4\nDecode: %5 (%6 errors)\nRender: %7\n"
                     "Clock: %8 · audio: %9")
                      .arg(s.presented)
                      .arg(s.dropped)
                      .arg(s.late)
                      .arg(s.blackFrames)
                      .arg(m_decodeBackend.isEmpty() ? tr("idle") : m_decodeBackend)
                      .arg(s.decodeErrors)
                      .arg(QString::fromStdString(s.renderBackend))
                      .arg(QString::fromStdString(s.transport))
                      .arg(m_audioBackend);
    emit statsChanged();
}

// ---- Project changes --------------------------------------------------------------------------------

void PlaybackBridge::onPlayheadChanged() {
    if (m_settingPlayhead || !m_session || !m_controller) {
        return;
    }
    const Ticks t = m_controller->playhead();
    if (m_session->isPlaying()) {
        m_session->seek(t);
    } else {
        m_session->scrub(t); // ruler scrub, arrows, Home/End: show that frame
    }
}

SequenceId PlaybackBridge::playableSequence() const {
    return m_controller && m_controller->hasMediaFiles() ? m_controller->currentSequenceId() : SequenceId{};
}

void PlaybackBridge::onSequenceChanged() {
    if (!m_session || !m_controller) {
        return;
    }
    const SequenceId sequence = playableSequence();
    if (sequence == m_session->sequence()) {
        return;
    }
    m_session->pause();
    m_session->setSequence(sequence);
    m_session->scrub(m_controller->playhead());
    updateStats();
}

// ---- Transport --------------------------------------------------------------------------------------

void PlaybackBridge::attachMonitor(QObject* object) {
    auto* monitor = qobject_cast<ProgramMonitor*>(object);
    if (!monitor) {
        return;
    }
    if (!m_monitors.contains(monitor)) {
        m_monitors.append(monitor);
    }
    monitor->setPlaybackController(activeController());
}

void PlaybackBridge::attachAll(playback::PlaybackController* controller) {
    m_monitors.removeAll(nullptr);
    for (const QPointer<ProgramMonitor>& monitor : std::as_const(m_monitors)) {
        monitor->setPlaybackController(controller);
    }
}

void PlaybackBridge::togglePlay() {
    startAudio();
    if (auto* active = activeController()) {
        active->togglePlay();
    }
    tick();
}

void PlaybackBridge::play() {
    startAudio();
    if (auto* active = activeController()) {
        active->play();
    }
    tick();
}

void PlaybackBridge::pause() {
    if (auto* active = activeController()) {
        active->pause();
    }
    tick();
}

bool PlaybackBridge::setSpeed(int num, int den) {
    playback::PlaybackController* active = activeController();
    if (!active) {
        return false;
    }
    if (num == 0) {
        active->pause();
        tick();
        return true;
    }
    startAudio();
    const bool ok = active->setTransportSpeed({num, std::max(1, den)});
    tick();
    return ok;
}

void PlaybackBridge::shuttle(int direction) {
    if (direction == 0) {
        setSpeed(0, 1);
        return;
    }
    const int sign = direction > 0 ? 1 : -1;
    const int current = speedNum();
    const int den = speedDen();
    int next = sign;
    if (playing() && den == 1 && current != 0 && (current > 0) == (sign > 0)) {
        next = std::clamp(current * 2, -kMaxLadder, kMaxLadder); // Premiere: each press doubles, up to 8x
    }
    setSpeed(next, 1);
}

void PlaybackBridge::applyTransportIntent(int kind, int speedNum, int speedDen, int frames) {
    switch (static_cast<KeymapDispatcher::TransportKind>(kind)) {
    case KeymapDispatcher::TransportKind::ShuttleStep:
    case KeymapDispatcher::TransportKind::SetSpeed:
        setSpeed(speedNum, speedDen);
        break;
    case KeymapDispatcher::TransportKind::StepFrames:
        stepFrames(frames);
        break;
    case KeymapDispatcher::TransportKind::Stop:
        setSpeed(0, 1);
        break;
    }
}

void PlaybackBridge::stepFrames(int frames) {
    if (m_sourceVideo) {
        m_sourceVideo->stepFrames(frames);
        emit sourceTimeChanged();
        tick();
        return;
    }
    if (!m_session || !m_controller) {
        return;
    }
    m_session->pause();
    tick();
    m_controller->stepFrames(frames); // the playhead moves; onPlayheadChanged scrubs to it
}

// ---- Source view ------------------------------------------------------------------------------------

bool PlaybackBridge::showSource(const QString& mediaId) {
    const auto id = detail::parseId<MediaId>(QLatin1Char('m'), mediaId);
    if (!id || !m_controller || !m_session) {
        return false;
    }
    const ProjectSnapshot snap = m_controller->session().snapshot();
    const MediaItem* item = snap->findMedia(*id);
    if (!item || item->locator.offline) {
        return false;
    }
    // A one-clip project: the media on V1 (and A1..), in a sequence of its own size and rate.
    edit::SequenceSettings settings;
    settings.name = item->name;
    if (item->info.hasVideo()) {
        const VideoStreamInfo& v = item->info.video.front();
        if (v.width > 0 && v.height > 0) {
            settings.width = v.width;
            settings.height = v.height;
        }
        if (v.frameRate.num > 0 && v.frameRate.den > 0) {
            settings.frameRate = v.frameRate;
        }
    }
    MediaItem copy = *item;
    auto withMedia = edit::addMedia(ProjectState{}, copy);
    if (!withMedia) {
        return false;
    }
    auto withSequence = edit::addSequence(withMedia->state, settings);
    if (!withSequence) {
        return false;
    }
    auto session = std::make_unique<EditSession>(std::move(withSequence->state));
    if (!editor::dropMedia(*session, {.media = withMedia->id, .sequence = withSequence->id, .at = Ticks{}})) {
        return false;
    }

    m_session->pause();
    playback::PlaybackOptions options;
    options.scheduler.zeroCopy = qEnvironmentVariable("WABCUT_ZERO_COPY") != QStringLiteral("0");
    auto video = std::make_unique<playback::PlaybackController>(options);
    video->scheduler().setPreview(m_preview); // proxies and preview resolution apply to source views too
    video->setSession(session.get());
    video->setSequence(withSequence->id);
    video->seek(Ticks{});
    attachAll(video.get());
    m_sourceVideo = std::move(video); // the previous source (if any) is detached already
    m_sourceSession = std::move(session);
    m_sourceName = QString::fromStdString(item->name);
    m_wasPlaying = false;
    emit sourceChanged();
    emit sourceTimeChanged();
    emit transportChanged();
    return true;
}

void PlaybackBridge::showProgram() {
    if (!m_sourceVideo) {
        return;
    }
    attachAll(m_session ? &m_session->video() : nullptr);
    m_sourceVideo.reset();
    m_sourceSession.reset();
    m_sourceName.clear();
    m_wasPlaying = false;
    emit sourceChanged();
    emit transportChanged();
    if (m_session && m_controller) {
        m_session->scrub(m_controller->playhead());
    }
}

} // namespace wabcut::app
