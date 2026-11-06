#include "CaptureBridge.h"

#include "AppAudioServices.h"
#include "DiskSpace.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/capture/CaptureFiles.h>
#include <wabcut/capture/CapturePresets.h>
#include <wabcut/capture/Preview.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QLocale>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace wabcut::app {

namespace {

/// Under this much free space on the drive a capture is refused / stopped (the file must be finished with room to spare).
constexpr qint64 kStartFreeBytes = 500LL * 1024 * 1024;
constexpr qint64 kStopFreeBytes = 300LL * 1024 * 1024;
constexpr qint64 kDiskCheckIntervalMs = 1000;
/// The longest the shutdown waits for the worker's current task (opening a device, finishing a file).
constexpr int kShutdownWaitMs = 15000;

QString toQ(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.toStdWString());
}

QString utf(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString clockText(double seconds) {
    const auto total = static_cast<qint64>(std::max(0.0, seconds));
    return QStringLiteral("%1:%2:%3")
        .arg(total / 3600)
        .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

/// The live picture for the panel: the session's newest picture converted on the loader's thread.
class CaptureImageProvider final : public QQuickImageProvider {
public:
    explicit CaptureImageProvider(std::shared_ptr<CaptureShared> shared)
        : QQuickImageProvider(QQuickImageProvider::Image), m_shared(std::move(shared)) {}

    QImage requestImage(const QString&, QSize* size, const QSize& requestedSize) override {
        std::shared_ptr<capture::CaptureSession> session;
        {
            std::lock_guard lock(m_shared->mutex);
            session = m_shared->session;
        }
        QImage out;
        if (session) {
            const capture::CaptureFrame frame = session->latestFrame();
            if (frame.valid()) {
                const int maxWidth = std::clamp(requestedSize.width() > 0 ? requestedSize.width() : 640, 16, 1920);
                if (auto preview = capture::makePreview(frame, session->format().pixelAspect, maxWidth)) {
                    QImage image(preview->width, preview->height, QImage::Format_RGBA8888);
                    const auto rowBytes = static_cast<std::size_t>(preview->width) * 4;
                    for (int y = 0; y < preview->height; ++y) {
                        std::memcpy(image.scanLine(y), preview->rgba.data() + static_cast<std::size_t>(y) * rowBytes, rowBytes);
                    }
                    out = std::move(image);
                }
            }
        }
        if (out.isNull()) {
            out = QImage(2, 2, QImage::Format_RGBA8888);
            out.fill(Qt::transparent);
        }
        if (size) {
            *size = out.size();
        }
        return out;
    }

private:
    std::shared_ptr<CaptureShared> m_shared;
};

/// The provenance note of a capture: what a later start needs to put it in the project (read by nothing yet but humans and
/// the recovery list; the bin is the one the user chose).
std::string provenanceNote(const std::filesystem::path& project, const QString& binKey, const QString& name,
                           const std::string& device, const std::string& format, const QString& preset) {
    std::string note;
    note += "kind=capture\n";
    note += "project=" + project.generic_string() + "\n";
    note += "bin=" + binKey.toStdString() + "\n";
    note += "name=" + name.toStdString() + "\n";
    note += "device=" + device + "\n";
    note += "format=" + format + "\n";
    note += "preset=" + preset.toStdString() + "\n";
    return note;
}

} // namespace

CaptureBridge::CaptureBridge(QQmlEngine* engine, QObject* parent) : QObject(parent), m_engine(engine) {
    const QSettings settings;
    const QString preset = settings.value(QStringLiteral("capture/preset"), m_preset).toString();
    for (const capture::CapturePreset& p : capture::capturePresets()) {
        if (utf(p.id) == preset) {
            m_preset = preset;
        }
    }
    m_audioEnabled = settings.value(QStringLiteral("capture/audio"), true).toBool();
    m_audioDevice = settings.value(QStringLiteral("capture/audioDevice")).toString();

    m_timer.setInterval(33); // ~30 Hz: the live view, the counters and the meter
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &CaptureBridge::poll);

    if (engine) {
        m_controller = engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        m_lifecycle = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    }
    if (m_controller) {
        // The editor goes (the controller is destroyed): a capture running ends here with its file finished. Direct: the
        // session must still exist.
        connect(m_controller, &ProjectController::sessionClosing, this, [this] {
            finishForShutdown();
            if (m_controller && m_resetListener != 0) {
                m_controller->session().removeListener(m_resetListener);
                m_resetListener = 0;
            }
            m_controller = nullptr;
        }, Qt::DirectConnection);
        connect(m_controller, &ProjectController::mediaChanged, this, &CaptureBridge::onProjectChanged);
        // A project is replaced (File > New, Open, Close Project): a capture running, or a device open, ends first.
        m_resetListener = m_controller->session().addListener([this](const ChangeEvent& event) {
            if (event.kind == ChangeKind::Reset) {
                onProjectReset();
            }
        });
    }
    if (QCoreApplication* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, &CaptureBridge::finishForShutdown);
    }
    m_urlOpener = [](const QUrl& url) { (void)QDesktopServices::openUrl(url); };
    ensureBackend();
    m_worker = std::jthread([this](std::stop_token stop) { workerLoop(stop); });
}

CaptureBridge::~CaptureBridge() {
    m_destroying = true;
    if (m_controller && m_resetListener != 0) {
        m_controller->session().removeListener(m_resetListener);
        m_resetListener = 0;
    }
    finishForShutdown();
    m_worker.request_stop();
    m_taskCv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

CaptureBridge* CaptureBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new CaptureBridge(qmlEngine);
    if (qmlEngine) {
        qmlEngine->addImageProvider(QStringLiteral("wabcut-capture"), new CaptureImageProvider(bridge->shared()));
    }
    return bridge;
}

// ---- The worker ----------------------------------------------------------------------------------------------------

void CaptureBridge::post(std::function<void()> task) {
    m_tasksOutstanding.fetch_add(1);
    {
        std::lock_guard lock(m_taskMutex);
        m_tasks.push_back(std::move(task));
    }
    m_taskCv.notify_one();
}

void CaptureBridge::workerLoop(std::stop_token stop) {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_taskMutex);
            if (!m_taskCv.wait(lock, stop, [this] { return !m_tasks.empty(); })) {
                return; // stop requested
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        task();
        m_tasksOutstanding.fetch_sub(1);
    }
}

bool CaptureBridge::waitIdle(int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (m_tasksOutstanding.load() == 0) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10); // the results the last task posted
            return true;
        }
        QThread::msleep(2);
    }
    return false;
}

// ---- Backend and state ---------------------------------------------------------------------------------------------

void CaptureBridge::ensureBackend() {
    if (m_backend) {
        return;
    }
    setBackend(capture::makeSystemBackend());
}

void CaptureBridge::setBackend(std::shared_ptr<capture::CaptureBackend> backend) {
    if (m_session && m_session->previewing()) {
        m_session->stopPreview();
    }
    m_backend = std::move(backend);
    m_session = std::make_shared<capture::CaptureSession>(m_backend);
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->session = m_session;
    }
    m_videoDevices.clear();
    m_audioDevices.clear();
    m_formatList.clear();
    m_formats.clear();
    m_videoDevice.clear();
    m_formatIndex = -1;
    emit devicesChanged();
    emit formatsChanged();
    emit selectionChanged();
}

bool CaptureBridge::supported() const {
    return m_backend && m_backend->supported();
}

QString CaptureBridge::unsupportedReason() const {
    if (!m_backend || m_backend->supported()) {
        return {};
    }
    if (m_backend->name() == "none") {
        return tr("Capturing from a camera or capture card works on Windows in this version. Support for Linux (V4L2) and macOS "
                  "(AVFoundation) is planned.");
    }
    return utf(m_backend->unsupportedReason());
}

QString CaptureBridge::state() const {
    switch (m_state) {
    case State::Idle:
        return QStringLiteral("idle");
    case State::Opening:
        return QStringLiteral("opening");
    case State::Previewing:
        return QStringLiteral("previewing");
    case State::Starting:
        return QStringLiteral("starting");
    case State::Recording:
        return QStringLiteral("recording");
    case State::Finishing:
        return QStringLiteral("finishing");
    }
    return QStringLiteral("idle");
}

void CaptureBridge::setState(State state) {
    if (state == m_state) {
        return;
    }
    m_state = state;
    if (state == State::Idle) {
        m_timer.stop();
        m_status.clear();
        m_level = 0.0f;
        ++m_previewSerial;
        emit statusChanged();
    }
    emit stateChanged();
}

void CaptureBridge::setLoading(bool loading) {
    m_loadingCount = std::max(0, m_loadingCount + (loading ? 1 : -1));
    const bool now = m_loadingCount > 0;
    if (now != m_loading) {
        m_loading = now;
        emit loadingChanged();
    }
}

void CaptureBridge::setError(const QString& message, const QString& access) {
    m_error = message;
    m_accessKind = access;
    if (!message.isEmpty()) {
        qWarning("Wabcut: capture: %s", qPrintable(message));
    }
    emit messageChanged();
}

void CaptureBridge::setNotice(const QString& message) {
    m_notice = message;
    emit messageChanged();
}

void CaptureBridge::clearMessages() {
    if (m_error.isEmpty() && m_notice.isEmpty() && m_accessKind.isEmpty()) {
        return;
    }
    m_error.clear();
    m_notice.clear();
    m_accessKind.clear();
    emit messageChanged();
}

QVariantList CaptureBridge::presets() const {
    // The labels are translated here (the library holds English); an unknown id never reaches the user.
    QVariantList list;
    for (const capture::CapturePreset& p : capture::capturePresets()) {
        QString label;
        QString description;
        if (p.id == "high") {
            label = tr("High quality (H.264)");
            description = tr("A good picture in files any editor reads. Uses the graphics card's encoder when there is one.");
        } else if (p.id == "compact") {
            label = tr("Compact (H.264)");
            description = tr("About half the size of High quality, for long recordings.");
        } else {
            label = tr("Every frame whole (ProRes 422)");
            description = tr("For tape and for material that will be cut hard or graded. Large files: about 65 GB an hour at 1080p30, "
                             "12 GB at standard definition.");
        }
        list.append(QVariantMap{{QStringLiteral("id"), utf(p.id)}, {QStringLiteral("label"), label}, {QStringLiteral("description"), description}});
    }
    return list;
}

double CaptureBridge::aspect() const {
    if (m_formatIndex < 0 || m_formatIndex >= static_cast<int>(m_formats.size())) {
        return 16.0 / 9.0;
    }
    const capture::VideoFormat& f = m_formats[static_cast<std::size_t>(m_formatIndex)];
    const double par = f.pixelAspect.isValid() ? static_cast<double>(f.pixelAspect.num) / f.pixelAspect.den : 1.0;
    return f.height > 0 ? f.width * par / f.height : 16.0 / 9.0;
}

std::filesystem::path CaptureBridge::folderPath() const {
    const std::filesystem::path project = m_controller ? m_controller->projectFile() : std::filesystem::path();
    if (!project.empty()) {
        return projectMediaDirectory(project, "Capture");
    }
    return toPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Wabcut/Capture"));
}

QString CaptureBridge::captureFolder() const {
    return QDir::toNativeSeparators(toQ(folderPath()));
}

QString CaptureBridge::suggestName() const {
    return utf(capture::defaultClipName(folderPath()));
}

// ---- Devices ---------------------------------------------------------------------------------------------------------

void CaptureBridge::refresh() {
    if (!m_backend || !m_backend->supported()) {
        emit devicesChanged();
        return;
    }
    rescanInterrupted();
    updateDisk();
    if (m_state != State::Idle && m_state != State::Previewing) {
        return;
    }
    setLoading(true);
    auto backend = m_backend;
    post([this, backend] {
        auto video = backend->videoDevices();
        auto audio = backend->audioDevices();
        const QString problem = !video ? utf(video.error().message) : QString();
        QMetaObject::invokeMethod(
            this,
            [this, cameras = video ? std::move(*video) : std::vector<capture::VideoDevice>{},
             inputs = audio ? std::move(*audio) : std::vector<capture::AudioDevice>{}, problem]() mutable {
                applyDevices(std::move(cameras), std::move(inputs), problem);
            },
            Qt::QueuedConnection);
    });
}

void CaptureBridge::applyDevices(std::vector<capture::VideoDevice> video, std::vector<capture::AudioDevice> audio, const QString& problem) {
    setLoading(false);
    m_videoDevices.clear();
    bool stillThere = false;
    for (const capture::VideoDevice& d : video) {
        m_videoDevices.append(QVariantMap{{QStringLiteral("id"), utf(d.id)}, {QStringLiteral("name"), utf(d.name)}});
        if (utf(d.id) == m_videoDevice) {
            stillThere = true;
            m_videoDeviceName = utf(d.name);
        }
    }
    m_audioDevices.clear();
    for (const capture::AudioDevice& d : audio) {
        m_audioDevices.append(QVariantMap{{QStringLiteral("id"), utf(d.id)}, {QStringLiteral("name"), utf(d.name)}, {QStringLiteral("isDefault"), d.isDefault}});
    }
    if (!stillThere && !m_videoDevice.isEmpty() && m_state == State::Idle) {
        m_videoDevice.clear(); // the device was unplugged since
        m_formats.clear();
        m_formatList.clear();
        m_formatIndex = -1;
        emit formatsChanged();
        emit selectionChanged();
    }
    if (!problem.isEmpty()) {
        setError(problem);
    }
    emit devicesChanged();
}

void CaptureBridge::setVideoDevice(const QString& id) {
    if (id == m_videoDevice || (m_state != State::Idle && m_state != State::Previewing)) {
        return;
    }
    if (m_state == State::Previewing) {
        stopPreview(); // another device: the first one closes
    }
    m_videoDevice = id;
    m_videoDeviceName.clear();
    for (const QVariant& v : m_videoDevices) {
        if (v.toMap().value(QStringLiteral("id")).toString() == id) {
            m_videoDeviceName = v.toMap().value(QStringLiteral("name")).toString();
        }
    }
    m_formats.clear();
    m_formatList.clear();
    m_formatIndex = -1;
    clearMessages();
    emit formatsChanged();
    emit selectionChanged();
    if (id.isEmpty() || !m_backend) {
        return;
    }
    // The system opens the device for the moment it takes to read its formats: this runs because the user picked it.
    setLoading(true);
    auto backend = m_backend;
    const std::string deviceId = id.toStdString();
    post([this, backend, deviceId, id] {
        auto formats = backend->videoFormats(deviceId);
        std::optional<Error> problem;
        if (!formats) {
            problem = formats.error();
        }
        QMetaObject::invokeMethod(
            this,
            [this, id, list = formats ? std::move(*formats) : std::vector<capture::VideoFormat>{}, problem]() mutable {
                applyFormats(id, std::move(list), problem);
            },
            Qt::QueuedConnection);
    });
}

void CaptureBridge::applyFormats(const QString& deviceId, std::vector<capture::VideoFormat> formats, const std::optional<Error>& problem) {
    setLoading(false);
    if (deviceId != m_videoDevice) {
        return; // another device was picked meanwhile
    }
    if (problem) {
        const auto kind = capture::accessDeniedKind(*problem);
        setError(utf(problem->message), kind ? (*kind == capture::AccessKind::Camera ? QStringLiteral("camera") : QStringLiteral("microphone"))
                                             : QString());
        return;
    }
    m_formats = std::move(formats);
    m_formatList.clear();
    int index = 0;
    for (const capture::VideoFormat& f : m_formats) {
        m_formatList.append(QVariantMap{{QStringLiteral("index"), index},
                                        {QStringLiteral("label"), utf(f.label())},
                                        {QStringLiteral("width"), f.width},
                                        {QStringLiteral("height"), f.height},
                                        {QStringLiteral("fps"), f.fps()},
                                        {QStringLiteral("native"), utf(f.nativeName)},
                                        {QStringLiteral("compressed"), f.compressed},
                                        {QStringLiteral("interlaced"), f.interlaced}});
        ++index;
    }
    // The format used last time with this device, else the best one.
    m_formatIndex = m_formats.empty() ? -1 : 0;
    const QString remembered = QSettings().value(QStringLiteral("capture/format/") + m_videoDeviceName).toString();
    if (!remembered.isEmpty()) {
        for (std::size_t i = 0; i < m_formats.size(); ++i) {
            if (utf(m_formats[i].label()) == remembered) {
                m_formatIndex = static_cast<int>(i);
            }
        }
    }
    clearMessages();
    emit formatsChanged();
    emit selectionChanged();
    updateDisk();
}

void CaptureBridge::setFormatIndex(int index) {
    if (index < 0 || index >= static_cast<int>(m_formats.size()) || index == m_formatIndex) {
        return;
    }
    if (m_state != State::Idle && m_state != State::Previewing) {
        return;
    }
    m_formatIndex = index;
    emit formatsChanged();
    emit selectionChanged();
    updateDisk();
    if (m_state == State::Previewing) {
        restartPreview(); // the picture follows the choice
    }
}

void CaptureBridge::setAudioEnabled(bool enabled) {
    if (enabled == m_audioEnabled || (m_state != State::Idle && m_state != State::Previewing)) {
        return;
    }
    m_audioEnabled = enabled;
    QSettings().setValue(QStringLiteral("capture/audio"), enabled);
    emit selectionChanged();
    updateDisk();
    if (m_state == State::Previewing) {
        restartPreview();
    }
}

void CaptureBridge::setAudioDevice(const QString& id) {
    if (id == m_audioDevice || (m_state != State::Idle && m_state != State::Previewing)) {
        return;
    }
    m_audioDevice = id;
    QSettings().setValue(QStringLiteral("capture/audioDevice"), id);
    emit selectionChanged();
    if (m_state == State::Previewing && m_audioEnabled) {
        restartPreview();
    }
}

void CaptureBridge::setPreset(const QString& id) {
    if (id == m_preset || m_state == State::Recording || m_state == State::Starting || m_state == State::Finishing) {
        return;
    }
    for (const capture::CapturePreset& p : capture::capturePresets()) {
        if (utf(p.id) == id) {
            m_preset = id;
            QSettings().setValue(QStringLiteral("capture/preset"), id);
            emit selectionChanged();
            updateDisk();
            return;
        }
    }
}

capture::SessionConfig CaptureBridge::sessionConfig() const {
    capture::SessionConfig config;
    config.videoDeviceId = m_videoDevice.toStdString();
    if (m_formatIndex >= 0 && m_formatIndex < static_cast<int>(m_formats.size())) {
        config.format = m_formats[static_cast<std::size_t>(m_formatIndex)];
    }
    config.audio = m_audioEnabled;
    config.audioDeviceId = m_audioDevice.toStdString();
    config.audioRate = 48000;
    config.audioChannels = 2;
    return config;
}

void CaptureBridge::rememberFormat() {
    if (m_formatIndex >= 0 && m_formatIndex < static_cast<int>(m_formats.size()) && !m_videoDeviceName.isEmpty()) {
        QSettings().setValue(QStringLiteral("capture/format/") + m_videoDeviceName, utf(m_formats[static_cast<std::size_t>(m_formatIndex)].label()));
    }
}

// ---- Preview ---------------------------------------------------------------------------------------------------------

void CaptureBridge::startPreview() {
    if (m_state != State::Idle) {
        return;
    }
    if (m_videoDevice.isEmpty() || m_formatIndex < 0 || m_formatIndex >= static_cast<int>(m_formats.size())) {
        setError(tr("Pick a video device and a format first."));
        return;
    }
    clearMessages();
    setState(State::Opening);
    const std::uint64_t epoch = ++m_epoch;
    auto session = m_session;
    const capture::SessionConfig config = sessionConfig();
    post([this, session, config, epoch] {
        auto started = session->startPreview(config);
        QMetaObject::invokeMethod(
            this,
            [this, epoch, started] {
                if (epoch != m_epoch) {
                    return; // closed, or the project went, while it opened (the worker closes it next)
                }
                if (!started) {
                    setState(State::Idle);
                    const auto kind = capture::accessDeniedKind(started.error());
                    setError(utf(started.error().message),
                             kind ? (*kind == capture::AccessKind::Camera ? QStringLiteral("camera") : QStringLiteral("microphone")) : QString());
                    return;
                }
                rememberFormat();
                setState(State::Previewing);
                m_lastDiskCheckMs = 0;
                m_timer.start();
                poll();
            },
            Qt::QueuedConnection);
    });
}

void CaptureBridge::restartPreview() {
    // Changing the format, the sound input or the sound switch while the picture is live: close and open again.
    stopPreview();
    startPreview();
}

void CaptureBridge::stopPreview() {
    if (m_state == State::Idle) {
        return;
    }
    const bool wasRecording = m_state == State::Recording;
    if (m_state == State::Starting || m_state == State::Finishing) {
        return; // let the capture finish its step first
    }
    ++m_epoch;
    auto session = m_session;
    setState(wasRecording ? State::Finishing : State::Idle);
    const std::uint64_t epoch = m_epoch;
    post([this, session, wasRecording, epoch] {
        std::expected<capture::RecorderResult, Error> result = std::unexpected(Error{ErrorCode::InvalidArgument, "not recording"});
        if (wasRecording) {
            result = session->stopRecording();
        }
        session->stopPreview();
        QMetaObject::invokeMethod(
            this,
            [this, wasRecording, epoch, result] {
                if (wasRecording) {
                    finishRecording(result, false);
                }
                if (epoch == m_epoch) {
                    setState(State::Idle);
                }
            },
            Qt::QueuedConnection);
    });
}

// ---- Recording ---------------------------------------------------------------------------------------------------------

bool CaptureBridge::record(const QString& clipName, const QString& binKey) {
    if (m_state == State::Starting || m_state == State::Recording || m_state == State::Finishing) {
        return false; // one capture at a time; the panel shows Stop meanwhile
    }
    if (m_state != State::Previewing) {
        setError(tr("Start the preview before recording."));
        return false;
    }
    if (m_lifecycle && m_lifecycle->demo()) {
        setError(tr("Open or start a project to capture into."));
        return false;
    }
    const std::filesystem::path folder = folderPath();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        setError(tr("The Capture folder could not be created: %1").arg(toQ(folder)));
        return false;
    }
    const qint64 freeNow = diskspace::freeBytesAt(toQ(folder));
    if (freeNow >= 0 && freeNow < kStartFreeBytes) {
        setError(tr("Only %1 is free on %2: free some space before capturing.")
                     .arg(diskspace::bytesText(static_cast<double>(freeNow)), diskspace::volumeNameOf(toQ(folder))));
        return false;
    }
    clearMessages();
    const QString name = clipName.trimmed().isEmpty() ? suggestName() : clipName.trimmed();
    const std::filesystem::path file = capture::captureFilePath(folder, name.toUtf8().toStdString());
    const capture::VideoFormat format = m_formats[static_cast<std::size_t>(m_formatIndex)];
    m_pending = Pending{file, binKey, name,
                        provenanceNote(m_controller ? m_controller->projectFile() : std::filesystem::path(), binKey, name,
                                       m_videoDeviceName.toStdString(), format.label(), m_preset)};
    capture::RecordingRequest request;
    request.file = file;
    request.presetId = m_preset.toStdString();
    request.encoder = m_encoder;
    request.title = name.toStdString();
    request.provenance = m_pending.note;
    m_autoStopping = false;
    setState(State::Starting);
    const std::uint64_t epoch = m_epoch;
    auto session = m_session;
    post([this, session, request, epoch] {
        auto started = session->startRecording(request);
        QMetaObject::invokeMethod(
            this,
            [this, epoch, started] {
                if (epoch != m_epoch) {
                    return;
                }
                if (!started) {
                    setState(State::Previewing);
                    setError(tr("Recording could not start: %1").arg(utf(started.error().message)));
                    return;
                }
                setState(State::Recording);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void CaptureBridge::stop() {
    if (m_state != State::Recording) {
        return;
    }
    setState(State::Finishing);
    const std::uint64_t epoch = m_epoch;
    auto session = m_session;
    post([this, session, epoch] {
        auto result = session->stopRecording();
        QMetaObject::invokeMethod(
            this,
            [this, epoch, result] {
                if (epoch != m_epoch) {
                    return;
                }
                finishRecording(result, true);
            },
            Qt::QueuedConnection);
    });
}

void CaptureBridge::finishRecording(const std::expected<capture::RecorderResult, Error>& result, bool keepPreview) {
    const Pending pending = m_pending;
    m_pending = Pending{};
    if (keepPreview && m_state == State::Finishing) {
        setState(State::Previewing);
    }
    if (!result) {
        if (result.error().code != ErrorCode::InvalidArgument) { // InvalidArgument: nothing was recording
            setError(tr("The capture could not be finished: %1").arg(utf(result.error().message)));
            rescanInterrupted();
        }
        return;
    }
    importCaptured(result->file, pending.binKey, pending.name);
    if (result->framesDropped > 0 && m_error.isEmpty()) {
        setNotice(m_notice + QLatin1Char(' ') + tr("%n frame(s) were dropped: the computer or the device could not keep up.", "", static_cast<int>(result->framesDropped)));
    }
}

void CaptureBridge::importCaptured(const std::filesystem::path& file, const QString& binKey, const QString& name) {
    if (!m_lifecycle) {
        setError(tr("The capture is saved in %1 but there is no project to add it to.").arg(toQ(file)));
        return;
    }
    QVariantMap options{{QStringLiteral("bin"), binKey}, {QStringLiteral("undoLabel"), QStringLiteral("Capture")}};
    const QVariantMap summary = m_lifecycle->importFiles({QUrl::fromLocalFile(toQ(file))}, options);
    if (summary.value(QStringLiteral("added")).toInt() + summary.value(QStringLiteral("existing")).toInt() == 0) {
        QString why;
        const QVariantList failed = summary.value(QStringLiteral("failed")).toList();
        if (!failed.isEmpty()) {
            why = QStringLiteral(" (") + failed.front().toMap().value(QStringLiteral("error")).toString() + QLatin1Char(')');
        }
        setError(tr("The capture is saved in %1 but could not be added to the project%2.").arg(toQ(file), why));
        return;
    }
    setNotice(tr("Added \"%1\" to the project.").arg(name.isEmpty() ? QFileInfo(toQ(file)).completeBaseName() : name));
    emit captured(toQ(file));
    rescanInterrupted();
}

// ---- Poll --------------------------------------------------------------------------------------------------------------

double CaptureBridge::estimatedBytesPerSecond() const {
    if (m_formatIndex < 0 || m_formatIndex >= static_cast<int>(m_formats.size())) {
        return 0.0;
    }
    return static_cast<double>(capture::estimateBytesPerSecond(capture::capturePreset(m_preset.toStdString()),
                                                               m_formats[static_cast<std::size_t>(m_formatIndex)], m_audioEnabled, 2, 48000));
}

/// The free space and the room it gives at the picked setting (looked up at most once a second).
void CaptureBridge::updateDisk() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_freeBytes < 0 || now - m_lastDiskCheckMs >= kDiskCheckIntervalMs) {
        m_lastDiskCheckMs = now;
        m_freeBytes = diskspace::freeBytesAt(toQ(folderPath()));
    }
    const QString text = freeSpaceText(estimatedBytesPerSecond());
    if (text != m_diskText) {
        m_diskText = text;
        emit statusChanged();
    }
}

QString CaptureBridge::freeSpaceText(double bytesPerSecond) const {
    if (m_freeBytes < 0) {
        return {};
    }
    const QString folder = toQ(folderPath());
    const QString volume = diskspace::volumeNameOf(folder);
    const QString freeText = diskspace::bytesText(static_cast<double>(m_freeBytes));
    if (bytesPerSecond > 0.0) {
        const std::string room = capture::durationText(static_cast<double>(m_freeBytes) / bytesPerSecond);
        return tr("%1 free on %2: room for about %3 at this setting").arg(freeText, volume, utf(room));
    }
    return tr("%1 free on %2").arg(freeText, volume);
}

void CaptureBridge::poll() {
    if (!m_session || m_state == State::Idle || m_state == State::Opening) {
        return;
    }
    const capture::SessionStatus s = m_session->status();
    if (!s.previewing && m_state != State::Finishing && m_state != State::Starting) {
        return;
    }
    const bool recordingNow = s.recording;
    const capture::RecorderStatus& r = s.recorder;

    m_level = std::max(s.audioPeak, m_level * 0.85f);
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastDiskCheckMs >= kDiskCheckIntervalMs) {
        m_lastDiskCheckMs = nowMs;
        m_freeBytes = diskspace::freeBytesAt(toQ(folderPath()));
    }
    // Bytes a second: measured once the capture has run a few seconds, else the preset's estimate.
    double bytesPerSecond = 0.0;
    if (recordingNow && r.seconds >= 5.0 && r.bytes > 0) {
        bytesPerSecond = static_cast<double>(r.bytes) / r.seconds;
    } else {
        bytesPerSecond = estimatedBytesPerSecond();
    }
    m_diskText = freeSpaceText(bytesPerSecond);

    QVariantMap st;
    st.insert(QStringLiteral("picture"), s.pictures > 0);
    st.insert(QStringLiteral("seconds"), recordingNow ? r.seconds : 0.0);
    st.insert(QStringLiteral("elapsedText"), clockText(recordingNow ? r.seconds : 0.0));
    st.insert(QStringLiteral("dropped"), static_cast<int>(r.framesDropped));
    st.insert(QStringLiteral("droppedText"), tr("%n frame(s) dropped", "", static_cast<int>(r.framesDropped)));
    st.insert(QStringLiteral("bytes"), static_cast<double>(r.bytes));
    st.insert(QStringLiteral("sizeText"), diskspace::bytesText(static_cast<double>(r.bytes)));
    st.insert(QStringLiteral("fps"), s.measuredFps);
    st.insert(QStringLiteral("fpsText"), s.measuredFps > 0.0 ? tr("%1 fps").arg(s.measuredFps, 0, 'f', 1) : QString());
    st.insert(QStringLiteral("level"), static_cast<double>(std::min(m_level, 1.0f)));
    st.insert(QStringLiteral("freeText"), freeSpaceText(bytesPerSecond));
    st.insert(QStringLiteral("freeBytes"), static_cast<double>(m_freeBytes));
    st.insert(QStringLiteral("encoder"), utf(r.encoder));
    st.insert(QStringLiteral("deviceName"), utf(s.videoDescription));
    st.insert(QStringLiteral("audioName"), utf(s.audioDescription));
    st.insert(QStringLiteral("recordFile"), recordingNow ? QFileInfo(toQ(m_session->recordingFile())).fileName() : QString());
    m_status = st;
    m_previewSerial = static_cast<int>(s.previewSerial % 1'000'000'000);
    emit statusChanged();

    if (!s.failure.empty() && (m_state == State::Previewing || m_state == State::Recording)) {
        // A device that stopped on its own (unplugged, driver failed): a capture running is finished with what it has, then the
        // devices are closed.
        if (m_state == State::Recording) {
            if (!m_autoStopping) {
                m_autoStopping = true;
                setError(utf(s.failure));
                stop();
            }
        } else {
            if (m_error.isEmpty()) {
                setError(utf(s.failure));
            }
            stopPreview();
        }
        return;
    }
    // The file could not be written any more (a drive that dropped out, a full network share): the recorder dropped
    // everything after the fault, so the take is finished with what it has and the person is told why.
    if (m_state == State::Recording && !m_autoStopping && r.state == capture::RecorderState::Failed) {
        m_autoStopping = true;
        setError(r.error.empty() ? tr("The capture stopped: the file could not be written.")
                                 : tr("The capture stopped: %1").arg(utf(r.error)));
        stop();
        return;
    }
    if (m_state == State::Recording && !m_autoStopping && m_freeBytes >= 0 && m_freeBytes < kStopFreeBytes) {
        m_autoStopping = true;
        setError(tr("The capture stopped: only %1 is left on %2.")
                     .arg(diskspace::bytesText(static_cast<double>(m_freeBytes)), diskspace::volumeNameOf(toQ(folderPath()))));
        stop();
    }
}

// ---- Privacy page ---------------------------------------------------------------------------------------------------------

void CaptureBridge::openPrivacySettings() {
    if (m_accessKind.isEmpty() || !m_urlOpener) {
        return;
    }
    // Windows' Settings pages for the two permissions.
    m_urlOpener(QUrl(m_accessKind == QLatin1String("microphone") ? QStringLiteral("ms-settings:privacy-microphone")
                                                                 : QStringLiteral("ms-settings:privacy-webcam")));
}

// ---- Captures a run left behind -------------------------------------------------------------------------------------------

QVariantMap CaptureBridge::interruptedEntry(const capture::InterruptedCapture& entry) const {
    QString started = utf(entry.journal.startedAt);
    const QDateTime when = QDateTime::fromString(started, Qt::ISODate);
    if (when.isValid()) {
        started = QLocale().toString(when.toLocalTime(), QLocale::ShortFormat);
    }
    return QVariantMap{{QStringLiteral("path"), toQ(entry.file)},
                       {QStringLiteral("name"), QString::fromStdWString(entry.file.stem().wstring())},
                       {QStringLiteral("sizeText"), diskspace::bytesText(static_cast<double>(entry.bytes))},
                       {QStringLiteral("startedAt"), started}};
}

void CaptureBridge::rescanInterrupted() {
    std::vector<std::filesystem::path> active;
    if (m_session && m_session->recording()) {
        active.push_back(m_session->recordingFile());
    }
    if (m_pending.file.empty() == false && (m_state == State::Starting || m_state == State::Recording || m_state == State::Finishing)) {
        active.push_back(m_pending.file);
    }
    QVariantList list;
    for (const capture::InterruptedCapture& entry : capture::findInterruptedCaptures(folderPath(), active)) {
        list.append(interruptedEntry(entry));
    }
    if (list != m_interrupted) {
        m_interrupted = list;
        emit interruptedChanged();
    }
}

void CaptureBridge::onProjectChanged() {
    const std::string key = m_controller ? m_controller->projectFile().generic_string() : std::string();
    if (key == m_scannedProject) {
        return;
    }
    m_scannedProject = key;
    m_freeBytes = -1;
    emit folderChanged();
    rescanInterrupted();
    updateDisk();
    if (!m_interrupted.isEmpty() && m_state == State::Idle) {
        emit interruptedFound(static_cast<int>(m_interrupted.size()));
    }
}

void CaptureBridge::onProjectReset() {
    if (m_state != State::Idle) {
        // The capture goes into the project it was started for or stays in its folder: it cannot follow the user to another one.
        finishForShutdown();
    }
    m_freeBytes = -1;
    onProjectChanged();
}

void CaptureBridge::recoverInterrupted(const QString& path, const QString& binKey) {
    if (m_state == State::Starting || m_state == State::Recording || m_state == State::Finishing) {
        return;
    }
    const std::filesystem::path wanted = toPath(path);
    std::vector<std::filesystem::path> active;
    const auto found = capture::findInterruptedCaptures(folderPath(), active);
    const auto it = std::ranges::find_if(found, [&](const capture::InterruptedCapture& c) { return c.file == wanted; });
    if (it == found.end()) {
        setError(tr("That capture is not waiting for an import any more."));
        rescanInterrupted();
        return;
    }
    clearMessages();
    setLoading(true);
    const capture::InterruptedCapture entry = *it;
    post([this, entry, binKey] {
        auto recovered = capture::recoverCapture(entry);
        QMetaObject::invokeMethod(
            this,
            [this, entry, binKey, recovered] {
                setLoading(false);
                if (!recovered) {
                    setError(tr("That capture could not be recovered: %1").arg(utf(recovered.error().message)));
                    rescanInterrupted();
                    return;
                }
                importCaptured(recovered->file, binKey, QString::fromStdWString(entry.file.stem().wstring()));
            },
            Qt::QueuedConnection);
    });
}

void CaptureBridge::discardInterrupted(const QString& path) {
    if (m_state == State::Starting || m_state == State::Recording || m_state == State::Finishing) {
        return;
    }
    const std::filesystem::path wanted = toPath(path);
    const auto found = capture::findInterruptedCaptures(folderPath(), {});
    const auto it = std::ranges::find_if(found, [&](const capture::InterruptedCapture& c) { return c.file == wanted; });
    if (it != found.end()) {
        std::error_code ec;
        std::filesystem::remove(it->file, ec);
        capture::removeJournal(it->file);
    }
    rescanInterrupted();
}

// ---- Shutdown --------------------------------------------------------------------------------------------------------------

void CaptureBridge::finishForShutdown() {
    if (!m_session) {
        return;
    }
    // The worker's current task (a device opening, a file being finished) completes first; nothing queued behind it runs.
    ++m_epoch;
    {
        std::lock_guard lock(m_taskMutex);
        m_tasksOutstanding.fetch_sub(static_cast<int>(m_tasks.size()));
        m_tasks.clear();
    }
    QElapsedTimer clock;
    clock.start();
    while (m_tasksOutstanding.load() > 0 && clock.elapsed() < kShutdownWaitMs) {
        QThread::msleep(5);
    }
    m_loadingCount = 0;
    if (m_loading && !m_destroying) {
        m_loading = false;
        emit loadingChanged();
    }
    const bool wasOpen = m_state != State::Idle;
    if (m_session->recording()) {
        const std::filesystem::path file = m_session->recordingFile();
        const QString name = QString::fromStdWString(file.stem().wstring());
        const std::string note = m_pending.note;
        auto result = m_session->stopRecording();
        if (result && !note.empty()) {
            // Finished, not imported: the journal stays so that the next start (or this project's next opening) offers it.
            (void)capture::writeJournal(file, note);
            if (!m_destroying) {
                setNotice(tr("The capture \"%1\" was finished and kept in the Capture folder. It is offered for import the next time the project opens.").arg(name));
            }
        }
        m_pending = Pending{};
    }
    m_session->stopPreview();
    (void)m_session->takeOrphanResult();
    if (wasOpen) {
        if (m_destroying) {
            m_state = State::Idle;
        } else {
            setState(State::Idle);
        }
    }
}

} // namespace wabcut::app
