#include "VoiceOverBridge.h"

#include "AppAudioServices.h"
#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/AudioInput.h>
#include <wabcut/audio/WavWriter.h>
#include <wabcut/audio/edit/VoiceOver.h>
#include <wabcut/command/Engine.h>
#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/playback/audio/VoiceOverRecorder.h>
#include <wabcut/playback/video/PlaybackController.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <set>
#include <system_error>

namespace wabcut::app {

using command::Json;

namespace {

QString tx(const char* text) { return QCoreApplication::translate("wabcut::app::VoiceOverBridge", text); }

double toDb(float linear) { return linear > 0.0f ? 20.0 * std::log10(static_cast<double>(linear)) : -144.0; }

double toMs(Ticks t) { return std::round(t.toSeconds() * 10000.0) / 10.0; }

Ticks fromSeconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// A file-name-safe version of user text (a sequence name): no separators or reserved characters, bounded.
QString safeName(QString text) {
    static const QString reserved = QStringLiteral("<>:\"/\\|?*");
    for (QChar& c : text) {
        if (c.unicode() < 0x20 || reserved.contains(c))
            c = QLatin1Char('_');
    }
    text = text.trimmed().left(60);
    return text.isEmpty() ? QStringLiteral("Sequence") : text;
}

QVariant toVariant(const Json& value) { return QJsonDocument::fromJson(QByteArray::fromStdString(value.dump())).toVariant(); }

/// The capture backend: the Null input (no device; it hears what input().push() gives it) when WABCUT_AUDIO_BACKEND
/// selects a silent engine, as PlaybackBridge does for the output (tests, headless machines).
audio::AudioBackend inputBackend() {
    const QString backend = qEnvironmentVariable("WABCUT_AUDIO_BACKEND").toLower();
    return backend == QStringLiteral("null") || backend == QStringLiteral("null-realtime") ? audio::AudioBackend::Null
                                                                                            : audio::AudioBackend::Default;
}

constexpr const char* kTakeKind = "wabcut.voiceOverTake";

/// Where a take was recorded: its journal's note (written at the pass's start), for putting it back after a crash.
struct TakeProvenance {
    SequenceId sequence;
    TrackId track;
    QString trackLabel;
    Ticks punchIn{};
    std::string device;
    Ticks latency{};
    std::string createdAt;
};

std::string provenanceNote(const std::filesystem::path& project, SequenceId sequence, TrackId track, const QString& label,
                           const playback::VoiceOverOptions& options, const std::string& device, Ticks latency, bool loopback) {
    Json j{{"kind", loopback ? "wabcut.latencyTest" : kTakeKind},
           {"project", pathToUtf8(project)},
           {"sequence", sequence.value},
           {"track", track.value},
           {"trackLabel", label.toStdString()},
           {"punchIn", options.punchIn.value},
           {"device", device},
           {"latencyCompensation", latency.value},
           {"createdAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()}};
    if (options.punchOut)
        j["punchOut"] = options.punchOut->value;
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<TakeProvenance> parseProvenance(const std::string& note) {
    const Json j = Json::parse(note, nullptr, false);
    if (!j.is_object())
        return std::nullopt;
    try {
        if (j.value("kind", std::string()) != kTakeKind)
            return std::nullopt; // a latency test, or not ours
        TakeProvenance p;
        p.sequence = SequenceId{j.value("sequence", std::uint64_t{0})};
        p.track = TrackId{j.value("track", std::uint64_t{0})};
        p.trackLabel = QString::fromStdString(j.value("trackLabel", std::string()));
        p.punchIn = Ticks{j.value("punchIn", std::int64_t{0})};
        p.device = j.value("device", std::string());
        p.latency = Ticks{j.value("latencyCompensation", std::int64_t{0})};
        p.createdAt = j.value("createdAt", std::string());
        return p;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

/// The take can go back where it was recorded: its sequence and its unlocked audio track still exist.
bool placeable(const ProjectState& state, const TakeProvenance& p) {
    const Sequence* seq = state.findSequence(p.sequence);
    const Track* track = seq ? seq->findTrack(p.track) : nullptr;
    return track && track->kind == TrackKind::Audio && !track->locked;
}

std::filesystem::path pathOf(const QString& path) { return std::filesystem::path(path.toStdWString()); }

/// "<sequence> A2 20261012-093000.wav" or "... (2).wav": the names beginPass() gives takes.
bool isTakeFileName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^.+ A\\d+ \\d{8}-\\d{6}( \\(\\d+\\))?\\.wav$"));
    return pattern.match(name).hasMatch();
}

/// `path` is a plain file or folder of its own type: not a link, a junction or another reparse point that redirects.
bool isPlain(const std::filesystem::path& path, std::filesystem::file_type type) {
    std::error_code ec;
    const std::filesystem::file_type actual = std::filesystem::symlink_status(path, ec).type();
    return !ec && actual == type;
}

/// A file the recovery may look at (S5-P2-11): named as the recorder names takes, a plain file with one name (no link,
/// junction or hard link to a file elsewhere), with a plain journal next to it.
bool isOwnTakeFile(const std::filesystem::path& file) {
    if (!isTakeFileName(QString::fromStdWString(file.filename().wstring())) || !isPlain(file, std::filesystem::file_type::regular) ||
        !isPlain(audio::wavJournalPath(file), std::filesystem::file_type::regular))
        return false;
    std::error_code ec;
    return std::filesystem::hard_link_count(file, ec) == 1 && !ec;
}

} // namespace

VoiceOverBridge::FreeRun::FreeRun(PlaybackBridge* playback) : m_playback(playback) {
    if (m_playback && m_playback->session())
        m_playback->session()->video().setStopAtEnd(false);
}

VoiceOverBridge::FreeRun::~FreeRun() {
    if (m_playback && m_playback->session())
        m_playback->session()->video().setStopAtEnd(true);
}

VoiceOverBridge::VoiceOverBridge(QObject* parent) : QObject(parent), m_input(std::make_unique<audio::AudioInput>()) {
    m_timer.setInterval(33); // ~30 Hz: the pass, the count-in and the input meter
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &VoiceOverBridge::poll);
    // Quitting ends a pass while all it uses still exists (before the QML engine and its singletons go): the take is
    // kept and placed (A5-P1-7).
    if (QCoreApplication* app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, &VoiceOverBridge::endPass);
}

VoiceOverBridge::~VoiceOverBridge() {
    endPass(); // a pass still running keeps its take (A5-P1-7); the recorder goes before the input it reads
    m_input->stop();
}

audio::AudioInput& VoiceOverBridge::input() { return *m_input; }

std::filesystem::path VoiceOverBridge::takesFolder(const std::filesystem::path& projectFile) {
    return projectMediaDirectory(projectFile, "Voice-over");
}

VoiceOverBridge* VoiceOverBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new VoiceOverBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
    }
    return bridge;
}

void VoiceOverBridge::setController(ProjectController* controller) {
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    m_scannedProject.clear();
    if (!m_controller)
        return;
    // The project closes (the controller goes): a pass still running ends here and keeps its take (A5-P1-7). Direct:
    // the session must still exist.
    connect(m_controller, &ProjectController::sessionClosing, this, &VoiceOverBridge::endPass, Qt::DirectConnection);
    // Another project file opened: offer what a crash left in its Voice-over folder (A5-P2-13).
    connect(m_controller, &ProjectController::mediaChanged, this, &VoiceOverBridge::onMediaChanged);
    onMediaChanged();
}

void VoiceOverBridge::onMediaChanged() {
    if (!m_controller || recording() || m_controller->projectFile() == m_scannedProject)
        return;
    m_scannedProject = m_controller->projectFile();
    recoverTakes();
}

void VoiceOverBridge::setPlayback(PlaybackBridge* playback) {
    if (m_playback)
        disconnect(m_playback, nullptr, this, nullptr);
    m_playback = playback;
    if (m_playback) {
        // The project closes: PlaybackBridge says so while the session still plays, so the pass is finished (the take
        // written and placed), not abandoned (A5-P1-7; ProjectController's sessionClosing reaches PlaybackBridge first).
        connect(m_playback, &PlaybackBridge::sessionEnding, this, &VoiceOverBridge::endPass, Qt::DirectConnection);
        // The transport stopped during a pass (Space, the post-roll's end): the pass ends and its take is kept.
        connect(m_playback, &PlaybackBridge::transportChanged, this, [this] {
            if (m_recorder && recording() && m_playback && !m_playback->playing())
                QTimer::singleShot(0, this, [this] {
                    if (recording())
                        finishPass();
                });
        });
    }
}

// ---- The input ---------------------------------------------------------------------------------------------------

void VoiceOverBridge::refreshDevices() {
    m_devices.clear();
    if (auto devices = audio::AudioInput::enumerateInputDevices()) {
        for (const audio::AudioDeviceInfo& d : *devices) {
            m_devices.push_back(QVariantMap{{QStringLiteral("id"), QString::fromStdString(d.id)},
                                            {QStringLiteral("name"), QString::fromStdString(d.name)},
                                            {QStringLiteral("isDefault"), d.isDefault}});
        }
    } else {
        setError(QString::fromStdString(devices.error().message));
    }
    emit inputDevicesChanged();
}

void VoiceOverBridge::setInputDevice(const QString& id) {
    if (id == m_deviceId)
        return;
    m_deviceId = id;
    if (m_input->isRunning() && !recording()) {
        m_input->stop(); // reopened on the new device
        if (m_inputHeld)
            ensureInput();
    }
    emit inputChanged();
}

bool VoiceOverBridge::inputOpen() const { return m_input->isRunning(); }

QString VoiceOverBridge::inputName() const { return QString::fromStdString(m_input->deviceName()); }

bool VoiceOverBridge::ensureInput() {
    if (m_input->isRunning())
        return true;
    audio::AudioInputConfig config;
    config.deviceId = m_deviceId.toStdString();
    config.channels = 1;
    config.backend = inputBackend();
    if (auto started = m_input->start(config); !started) {
        setError(tx("The microphone could not be opened: ") + QString::fromStdString(started.error().message));
        return false;
    }
    qInfo("Wabcut: the audio input '%s' is open", m_input->deviceName().c_str());
    m_timer.start();
    emit inputChanged();
    emit latencyChanged();
    return true;
}

bool VoiceOverBridge::openInput() {
    m_inputHeld = true;
    return ensureInput();
}

void VoiceOverBridge::closeInput() {
    m_inputHeld = false;
    releaseInputIfUnheld();
}

void VoiceOverBridge::releaseInputIfUnheld() {
    if (m_inputHeld || recording() || !m_input->isRunning())
        return;
    m_input->stop();
    qInfo("Wabcut: the audio input is closed");
    m_timer.stop();
    m_levels.clear();
    emit levelsChanged();
    emit inputChanged();
}

// ---- A pass ------------------------------------------------------------------------------------------------------

bool VoiceOverBridge::recording() const { return m_recorder && m_recorder->status().state != playback::VoiceOverState::Idle; }

QString VoiceOverBridge::state() const {
    return m_recorder ? QString::fromUtf8(playback::toString(m_recorder->status().state).data()) : QStringLiteral("idle");
}

int VoiceOverBridge::countIn() const { return m_recorder ? m_recorder->status().countInRemaining : 0; }

double VoiceOverBridge::recordedSeconds() const { return m_recorder ? m_recorder->status().recorded.toSeconds() : 0.0; }

bool VoiceOverBridge::start() { return beginPass(false); }

bool VoiceOverBridge::measureLatency() { return beginPass(true); }

void VoiceOverBridge::toggle() {
    if (recording())
        stop();
    else
        start();
}

bool VoiceOverBridge::beginPass(bool loopback) {
    if (recording())
        return false;
    // The output opens for the pass, whatever the project holds: silent media and stills too (A5-P2-18).
    if (m_playback)
        m_playback->ensureAudio();
    editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr;
    playback::TimelineAudioPlayer* player = session ? session->audioPlayer() : nullptr;
    if (!m_controller || !player) {
        setError(tx("Recording needs the audio output (Preferences > Audio)"));
        return false;
    }
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq) {
        setError(tx("Open a sequence to record into"));
        return false;
    }
    // The track: the first armed audio track, else the first targeted one.
    const Track* target = nullptr;
    std::size_t index = 0;
    for (int pass = 0; pass < 2 && !target; ++pass) {
        for (std::size_t i = 0; i < seq->audioTracks.size(); ++i) {
            const Track& t = *seq->audioTracks[i];
            if (pass == 0 ? t.recordArmed : t.targeted) {
                target = &t;
                index = i;
                break;
            }
        }
    }
    if (!target && !loopback) {
        setError(tx("Arm an audio track for recording (R) first"));
        return false;
    }
    if (target && target->locked && !loopback) {
        setError(tx("The track to record onto is locked"));
        return false;
    }
    if (!ensureInput())
        return false;
    // The take's file: the project's own Voice-over folder (<project>.wabcut-media/Voice-over, so projects saved in one
    // folder do not share it; takes recorded before stay where they are), else Documents/Wabcut/Voice-over.
    QDir folder;
    const std::filesystem::path& project = m_controller->projectFile();
    if (!project.empty())
        folder = QDir(QString::fromStdWString(takesFolder(project).wstring()));
    else
        folder = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Wabcut/Voice-over"));
    if (!folder.mkpath(QStringLiteral("."))) {
        setError(tx("The Voice-over folder could not be created: ") + folder.absolutePath());
        releaseInputIfUnheld();
        return false;
    }
    m_trackLabel = target ? QStringLiteral("A%1").arg(index + 1) : QString();
    const QString stem = loopback ? QStringLiteral("Latency test")
                                  : safeName(QString::fromStdString(seq->name)) + QLatin1Char(' ') + m_trackLabel + QLatin1Char(' ') +
                                        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString file = folder.filePath(stem + QStringLiteral(".wav"));
    for (int n = 2; QFileInfo::exists(file); ++n)
        file = folder.filePath(stem + QStringLiteral(" (%1).wav").arg(n));

    playback::VoiceOverOptions options;
    const std::optional<TimeRange> marks = seq->markedRange();
    if (loopback) {
        options.punchIn = m_controller->playhead();
        options.punchOut = options.punchIn + Ticks::fromSeconds(3);
        options.preroll = Ticks{};
        options.postroll = Ticks{};
        options.countIn = 0;
        options.loopback = true;
    } else {
        options.punchIn = marks ? marks->start : m_controller->playhead();
        if (marks)
            options.punchOut = marks->end();
        options.preroll = fromSeconds(m_preroll);
        options.postroll = fromSeconds(m_postroll);
        options.countIn = m_countIn;
        options.countInBeep = m_beep;
        options.monitor = m_monitor;
        options.monitorGainDb = m_monitorGainDb;
    }
    options.calibration = fromSeconds(m_calibrationMs / 1000.0);
    options.file = std::filesystem::path(file.toStdWString());
    options.bitsPerSample = 24;
    // In the take's journal from the start: where it goes, should the app stop before it is placed (A5-P2-13).
    options.provenance = provenanceNote(project, seq->id, target ? target->id : TrackId{}, m_trackLabel, options,
                                        m_input->deviceName(), m_input->latency().total + options.calibration, loopback);
    // The pass drives the playback session's transport, so the picture follows; it may run past the sequence end.
    playback::VoiceOverRecorder::Options recorderOptions;
    recorderOptions.seek = [session](Ticks t) { session->seek(t); };
    recorderOptions.play = [session] { session->play(); };
    recorderOptions.pause = [session] { session->pause(); };
    m_freeRun.emplace(m_playback.data());
    m_recorder = std::make_unique<playback::VoiceOverRecorder>(session->engine(), *player, *m_input, recorderOptions);
    if (auto started = m_recorder->start(options); !started) {
        m_recorder.reset();
        m_freeRun.reset();
        setError(tx("Recording could not start: ") + QString::fromStdString(started.error().message));
        releaseInputIfUnheld();
        return false;
    }
    m_passLoopback = loopback;
    m_passSession = session;
    m_sequence = seq->id;
    m_track = target ? target->id : TrackId{};
    setError({});
    qInfo("Wabcut: voice-over recording onto %s", qPrintable(m_trackLabel));
    m_timer.start();
    emit stateChanged();
    return true;
}

void VoiceOverBridge::stop() {
    if (recording())
        finishPass();
}

void VoiceOverBridge::cancel() {
    if (!m_recorder)
        return;
    m_recorder->cancel(); // the file and its journal go: the only way a take is dropped
    m_recorder.reset();
    m_freeRun.reset();
    m_passSession = nullptr;
    m_trackLabel.clear();
    emit stateChanged();
    releaseInputIfUnheld();
}

void VoiceOverBridge::endPass() {
    if (!m_recorder)
        return;
    if (m_passSession && m_playback && m_playback->session() == m_passSession) {
        finishPass(); // stop(): the take is written out, the output restored, the take placed
        return;
    }
    // The playback session went first (PlaybackBridge's teardown): its engine and player are gone, so the file is
    // closed as it is, without them. It keeps its journal: the next open of the project offers it.
    m_recorder->abandon();
    m_recorder.reset();
    m_freeRun.reset();
    m_passSession = nullptr;
    m_trackLabel.clear();
    emit stateChanged();
}

void VoiceOverBridge::finishPass() {
    auto result = m_recorder->stop();
    m_recorder.reset();
    m_freeRun.reset();
    m_passSession = nullptr;
    const QString label = m_trackLabel;
    m_trackLabel.clear();
    emit stateChanged();
    releaseInputIfUnheld();
    if (!result) {
        if (result.error().code != ErrorCode::NotFound)
            setError(QString::fromStdString(result.error().message));
        return;
    }
    if (m_passLoopback) {
        std::error_code ec;
        std::filesystem::remove(result->file, ec); // the clicks are not a take
        audio::removeWavJournal(result->file);
        if (!result->loopback || result->loopback->clicksFound == 0) {
            setError(tx("No clicks came back: hold the microphone to the speakers (or connect the output to the input) and try again"));
            return;
        }
        m_calibrationMs += toMs(result->loopback->residual);
        m_roundTripMs = toMs(result->loopback->roundTrip);
        emit optionsChanged();
        emit latencyChanged();
        emit latencyMeasured(m_roundTripMs, result->loopback->clicksFound);
        return;
    }
    if (!m_controller)
        return;
    audio::edit::RecordedTake take;
    take.path = pathToUtf8(result->file); // absolute: the Voice-over folder's
    if (auto fp = fingerprintFile(result->file))
        take.fingerprint = *fp;
    take.start = result->start;
    take.duration = result->duration;
    take.sampleRate = result->sampleRate;
    take.channels = result->channels;
    take.bitsPerSample = 24;
    take.device = result->device;
    take.latencyCompensation = result->latencyCompensation;
    take.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    const ProjectSnapshot now = m_controller->session().snapshot();
    auto placed = audio::edit::placeVoiceOverTake(*now, m_sequence, m_track, take);
    if (!placed) {
        setError(tx("The take is saved but could not go on ") + label + QStringLiteral(": ") +
                 QString::fromStdString(placed.error().message));
        return;
    }
    if (auto committed = m_controller->session().commit(tx("Voice-over Record").toStdString(), placed->state); !committed) {
        setError(QString::fromStdString(committed.error().message));
        return;
    }
    if (result->droppedFrames > 0)
        setError(tx("The computer fell behind while recording: the take has a gap"));
    emit takeRecorded(placed->placed.empty() ? QString() : clipKey(placed->placed.front()));
}

void VoiceOverBridge::poll() {
    if (m_recorder) {
        const playback::VoiceOverState before = m_recorder->status().state;
        m_recorder->tick();
        const playback::VoiceOverStatus after = m_recorder->status();
        emit stateChanged();
        if (after.state == playback::VoiceOverState::Failed)
            finishPass();
        else if (after.state == playback::VoiceOverState::Finished && before != after.state)
            finishPass();
    }
    if (m_input->isRunning()) {
        const audio::InputLevels l = m_input->levels();
        m_levels = QVariantMap{{QStringLiteral("peakDb"), toDb(l.peak[0])},
                               {QStringLiteral("rmsDb"), toDb(l.rms[0])},
                               {QStringLiteral("clipped"), l.clipped}};
        emit levelsChanged();
    } else if (!m_recorder) {
        m_timer.stop();
    }
}

QVariantMap VoiceOverBridge::latency() const {
    const audio::InputLatency in = m_input->latency();
    QVariantMap out{{QStringLiteral("inputMs"), toMs(in.total)},
                    {QStringLiteral("calibrationMs"), m_calibrationMs},
                    {QStringLiteral("roundTripMs"), m_roundTripMs}};
    if (m_recorder)
        out.insert(QStringLiteral("monitorMs"), toMs(m_recorder->status().monitorLatency));
    return out;
}

QVariantMap VoiceOverBridge::options() const {
    return QVariantMap{{QStringLiteral("countIn"), m_countIn},        {QStringLiteral("beep"), m_beep},
                       {QStringLiteral("prerollSeconds"), m_preroll}, {QStringLiteral("postrollSeconds"), m_postroll},
                       {QStringLiteral("monitor"), m_monitor},        {QStringLiteral("monitorGainDb"), m_monitorGainDb},
                       {QStringLiteral("calibrationMs"), m_calibrationMs}};
}

void VoiceOverBridge::setOptions(const QVariantMap& o) {
    m_countIn = std::clamp(o.value(QStringLiteral("countIn"), m_countIn).toInt(), 0, 10);
    m_beep = o.value(QStringLiteral("beep"), m_beep).toBool();
    m_preroll = std::clamp(o.value(QStringLiteral("prerollSeconds"), m_preroll).toDouble(), 0.0, 60.0);
    m_postroll = std::clamp(o.value(QStringLiteral("postrollSeconds"), m_postroll).toDouble(), 0.0, 60.0);
    m_monitor = o.value(QStringLiteral("monitor"), m_monitor).toBool();
    m_monitorGainDb = std::clamp(o.value(QStringLiteral("monitorGainDb"), m_monitorGainDb).toDouble(), -60.0, 12.0);
    m_calibrationMs = std::clamp(o.value(QStringLiteral("calibrationMs"), m_calibrationMs).toDouble(), -1000.0, 1000.0);
    emit optionsChanged();
    emit latencyChanged();
}

void VoiceOverBridge::setError(const QString& message) {
    if (message == m_error)
        return;
    m_error = message;
    if (!message.isEmpty())
        qWarning("Wabcut: voice-over: %s", qPrintable(message));
    emit errorChanged();
}

// ---- Commands: arm, takes ------------------------------------------------------------------------------------------

namespace {

command::CommandResult runCommand(ProjectController& controller, const std::string& name, Json params) {
    command::CommandEngine engine(controller.session(), ai::edit::editingCommands(), command::EnginePolicy{});
    command::CommandRequest request;
    request.command = name;
    params["sequence"] = controller.currentSequenceId().value;
    request.params = std::move(params);
    return engine.execute(request);
}

QString errorOf(const command::CommandResult& result) { return result.ok() ? QString() : QString::fromStdString(result.error->message); }

} // namespace

QString VoiceOverBridge::setRecordArm(const QString& track, bool armed) {
    const auto id = parseTrackKey(track);
    if (!m_controller || !id)
        return tx("Choose an audio track");
    return errorOf(runCommand(*m_controller, "track.setRecordArm", Json{{"track", id->value}, {"armed", armed}}));
}

QVariantList VoiceOverBridge::takesOf(const QString& clip) {
    const auto id = parseClipKey(clip);
    if (!m_controller || !id)
        return {};
    const command::CommandResult r = runCommand(*m_controller, "record.takes", Json{{"clip", id->value}});
    if (!r.ok())
        return {};
    QVariantList out;
    for (const Json& t : r.data["takes"]) {
        QVariantMap row = toVariant(t).toMap();
        row.insert(QStringLiteral("media"), mediaKey(MediaId{t["media"].get<std::uint64_t>()}));
        row.insert(QStringLiteral("startText"), row.take(QStringLiteral("start")));
        out.push_back(row);
    }
    return out;
}

QString VoiceOverBridge::useTake(const QString& clip, int take) {
    const auto id = parseClipKey(clip);
    if (!m_controller || !id)
        return tx("Choose a recorded clip");
    return errorOf(runCommand(*m_controller, "record.useTake", Json{{"clip", id->value}, {"take", take}}));
}

QVariantList VoiceOverBridge::recoverTakes() {
    if (recording())
        return m_recovered; // (a pass's own file is being written)
    QVariantList out;
    const std::filesystem::path folder =
        m_controller && !m_controller->projectFile().empty() ? takesFolder(m_controller->projectFile()) : std::filesystem::path();
    // The project's own folder only, not one a link or junction puts somewhere else (S5-P2-11).
    if (!folder.empty() && isPlain(folder, std::filesystem::file_type::directory) &&
        isPlain(folder.parent_path(), std::filesystem::file_type::directory)) {
        const ProjectSnapshot snap = m_controller->session().snapshot();
        std::set<std::filesystem::path> used;
        for (const MediaPtr& m : snap->media)
            used.insert(pathFromUtf8(m->locator.path).lexically_normal());
        std::error_code ec;
        // Read only: nothing is repaired until the user picks a take (placeRecoveredTake).
        for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator();
             it.increment(ec)) {
            const std::filesystem::path file = it->path();
            if (used.contains(file.lexically_normal()) || !isOwnTakeFile(file))
                continue;
            // Only takes a pass recorded (their journal says where): any other WAV here is left as it is (A5-P2-12).
            const std::optional<audio::WavJournal> journal = audio::readWavJournal(file);
            const std::optional<TakeProvenance> provenance = journal ? parseProvenance(journal->note) : std::nullopt;
            if (!provenance)
                continue;
            const auto r = audio::inspectWav(file);
            if (!r || r->frames <= 0)
                continue;
            out.push_back(QVariantMap{
                {QStringLiteral("path"), QString::fromStdWString(file.wstring())},
                {QStringLiteral("name"), QString::fromStdWString(file.filename().wstring())},
                {QStringLiteral("seconds"), static_cast<double>(r->frames) / std::max(r->format.sampleRate, 1)},
                {QStringLiteral("interrupted"), r->needsRepair},
                {QStringLiteral("track"), provenance->trackLabel},
                {QStringLiteral("startText"), m_controller->formatTimecode(provenance->punchIn)},
                {QStringLiteral("placeable"), placeable(*snap, *provenance)},
            });
        }
    }
    if (out != m_recovered) {
        m_recovered = out;
        emit recoveredTakesChanged();
    }
    return m_recovered;
}

QString VoiceOverBridge::placeRecoveredTake(const QString& path) {
    if (!m_controller)
        return tx("Open the project first");
    if (recording())
        return tx("Stop recording first");
    const auto listed = std::find_if(m_recovered.cbegin(), m_recovered.cend(), [&](const QVariant& row) {
        return row.toMap().value(QStringLiteral("path")).toString() == path;
    });
    if (listed == m_recovered.cend())
        return tx("That take is not waiting to be recovered");
    const std::filesystem::path file = pathOf(path);
    if (!isOwnTakeFile(file))
        return tx("That take is not one this project recorded: import it from the Voice-over folder");
    const std::optional<audio::WavJournal> journal = audio::readWavJournal(file);
    const std::optional<TakeProvenance> provenance = journal ? parseProvenance(journal->note) : std::nullopt;
    if (!provenance)
        return tx("Where this take was recorded is not known: import it from the Voice-over folder");
    // The one repair: of the take the user picked, our recorder's own file (its journal, its name, one plain file in
    // the project's folder). In place, as the writer's next sync would have written it: the sizes and a cut frame.
    const auto wav = audio::recoverWav(file);
    if (!wav)
        return tx("The take could not be read: ") + QString::fromStdString(wav.error().message);
    if (wav->frames <= 0)
        return tx("The take is empty");
    audio::edit::RecordedTake take;
    take.path = pathToUtf8(file);
    if (auto fp = fingerprintFile(file))
        take.fingerprint = *fp;
    take.start = provenance->punchIn; // where the take starts when the latencies are right
    take.duration = samplesToTicks(wav->frames, Rate{wav->format.sampleRate, 1});
    take.sampleRate = wav->format.sampleRate;
    take.channels = wav->format.channels;
    take.bitsPerSample = wav->format.bitsPerSample;
    take.device = provenance->device;
    take.latencyCompensation = provenance->latency;
    take.createdAt = !provenance->createdAt.empty() ? provenance->createdAt
                                                     : QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    const ProjectSnapshot now = m_controller->session().snapshot();
    auto placed = audio::edit::placeVoiceOverTake(*now, provenance->sequence, provenance->track, take);
    if (!placed)
        return QString::fromStdString(placed.error().message);
    if (auto committed = m_controller->session().commit(tx("Voice-over Record").toStdString(), placed->state); !committed)
        return QString::fromStdString(committed.error().message);
    m_recovered.removeIf([&](const QVariant& row) { return row.toMap().value(QStringLiteral("path")).toString() == path; });
    emit recoveredTakesChanged();
    emit takeRecorded(placed->placed.empty() ? QString() : clipKey(placed->placed.front()));
    return {};
}

void VoiceOverBridge::dismissRecoveredTakes() {
    if (m_recovered.isEmpty())
        return;
    for (const QVariant& row : std::as_const(m_recovered))
        audio::removeWavJournal(pathOf(row.toMap().value(QStringLiteral("path")).toString()));
    m_recovered.clear();
    emit recoveredTakesChanged();
}

} // namespace wabcut::app
