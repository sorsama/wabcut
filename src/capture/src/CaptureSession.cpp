#include <wabcut/capture/CaptureSession.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace wabcut::capture {

namespace {

std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

CaptureSession::CaptureSession(std::shared_ptr<CaptureBackend> backend) : m_backend(std::move(backend)) {}

CaptureSession::~CaptureSession() {
    stopPreview();
}

bool CaptureSession::previewing() const {
    std::lock_guard lock(m_control);
    return m_previewing;
}

std::expected<void, Error> CaptureSession::startPreview(const SessionConfig& config) {
    {
        std::lock_guard lock(m_control);
        if (m_previewing) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "The devices are open already"});
        }
    }
    // Opening a device can take a second or two: nothing is locked meanwhile, so status() stays quick.
    {
        std::lock_guard lock(m_frameMutex);
        m_latest = CaptureFrame{};
        m_pictures = 0;
        m_serial = 0;
        m_windowStartNs = 0;
        m_windowPictures = 0;
        m_fps = 0.0;
        m_peak = 0.0f;
    }
    // The sound goes first: a microphone the system refuses is found out before the camera light comes on.
    std::unique_ptr<AudioSource> audio;
    if (config.audio) {
        auto opened = m_backend->openAudio(config.audioDeviceId, config.audioRate, config.audioChannels);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        audio = std::move(*opened);
        if (auto started = audio->start([this](AudioBlock&& block) { onAudio(std::move(block)); }); !started) {
            return std::unexpected(started.error());
        }
    }
    auto video = m_backend->openVideo(config.videoDeviceId, config.format);
    if (!video) {
        if (audio) {
            audio->stop();
        }
        return std::unexpected(video.error());
    }
    if (auto started = (*video)->start([this](CaptureFrame&& frame) { onFrame(std::move(frame)); }); !started) {
        if (audio) {
            audio->stop();
        }
        return std::unexpected(started.error());
    }
    std::lock_guard lock(m_control);
    m_config = config;
    m_format = (*video)->format();
    m_videoName = (*video)->description();
    m_audioName = audio ? audio->description() : std::string();
    m_video = std::move(*video);
    m_audio = std::move(audio);
    m_previewing = true;
    return {};
}

void CaptureSession::stopPreview() {
    if (recording()) {
        auto result = stopRecording();
        if (result) {
            m_orphan = std::move(*result);
        }
    }
    std::unique_ptr<VideoSource> video;
    std::unique_ptr<AudioSource> audio;
    {
        std::lock_guard lock(m_control);
        video = std::move(m_video);
        audio = std::move(m_audio);
        m_previewing = false;
    }
    if (video) {
        video->stop();
    }
    if (audio) {
        audio->stop();
    }
    std::lock_guard lock(m_frameMutex);
    m_latest = CaptureFrame{};
    m_fps = 0.0;
}

std::shared_ptr<CaptureRecorder> CaptureSession::recorder() const {
    std::lock_guard lock(m_recorderMutex);
    return m_recorder;
}

bool CaptureSession::recording() const {
    return recorder() != nullptr;
}

std::filesystem::path CaptureSession::recordingFile() const {
    const auto rec = recorder();
    return rec ? rec->file() : std::filesystem::path();
}

std::expected<void, Error> CaptureSession::startRecording(const RecordingRequest& request) {
    RecorderConfig config;
    {
        std::lock_guard lock(m_control);
        if (!m_previewing) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Start the preview before recording"});
        }
        config.video = m_format;
        config.audio = m_audio != nullptr;
        if (m_audio) {
            config.audioRate = m_audio->sampleRate();
            config.audioChannels = m_audio->channels();
        }
    }
    if (recording()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "A capture is recording already"});
    }
    config.file = request.file;
    config.presetId = request.presetId;
    config.encoder = request.encoder;
    config.title = request.title;
    config.provenance = request.provenance;
    auto rec = std::make_shared<CaptureRecorder>();
    if (auto started = rec->start(config); !started) {
        return started;
    }
    std::lock_guard lock(m_recorderMutex);
    m_recorder = std::move(rec);
    return {};
}

std::expected<RecorderResult, Error> CaptureSession::stopRecording() {
    std::shared_ptr<CaptureRecorder> rec;
    {
        std::lock_guard lock(m_recorderMutex);
        rec = std::move(m_recorder);
        m_recorder.reset();
    }
    if (!rec) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No capture is recording"});
    }
    return rec->stop();
}

void CaptureSession::abandonRecording() noexcept {
    std::shared_ptr<CaptureRecorder> rec;
    {
        std::lock_guard lock(m_recorderMutex);
        rec = std::move(m_recorder);
        m_recorder.reset();
    }
    if (rec) {
        rec->abandon();
    }
}

void CaptureSession::cancelRecording() noexcept {
    std::shared_ptr<CaptureRecorder> rec;
    {
        std::lock_guard lock(m_recorderMutex);
        rec = std::move(m_recorder);
        m_recorder.reset();
    }
    if (rec) {
        rec->cancel();
    }
}

std::optional<RecorderResult> CaptureSession::takeOrphanResult() {
    std::optional<RecorderResult> out;
    out.swap(m_orphan);
    return out;
}

void CaptureSession::onFrame(CaptureFrame&& frame) {
    {
        std::lock_guard lock(m_frameMutex);
        m_latest = frame; // shares the picture's bytes
        ++m_pictures;
        ++m_serial;
        const std::int64_t now = steadyNowNs();
        if (m_windowStartNs == 0) {
            m_windowStartNs = now;
        }
        ++m_windowPictures;
        if (now - m_windowStartNs >= 1'000'000'000) {
            m_fps = static_cast<double>(m_windowPictures) * 1e9 / static_cast<double>(now - m_windowStartNs);
            m_windowStartNs = now;
            m_windowPictures = 0;
        }
    }
    if (const auto rec = recorder()) {
        rec->pushVideo(std::move(frame));
    }
}

void CaptureSession::onAudio(AudioBlock&& block) {
    float peak = 0.0f;
    for (const float s : block.samples) {
        peak = std::max(peak, std::abs(s));
    }
    {
        std::lock_guard lock(m_frameMutex);
        m_peak = std::max(m_peak, peak);
    }
    if (const auto rec = recorder()) {
        rec->pushAudio(std::move(block));
    }
}

SessionStatus CaptureSession::status() {
    SessionStatus s;
    {
        std::lock_guard lock(m_control);
        s.previewing = m_previewing;
        s.format = m_format;
        s.videoDescription = m_videoName;
        s.audioDescription = m_audioName;
        if (m_video) {
            s.deviceDrops = m_video->reportedDrops();
            s.failure = m_video->failure();
        }
        if (s.failure.empty() && m_audio) {
            s.failure = m_audio->failure();
        }
    }
    {
        std::lock_guard lock(m_frameMutex);
        s.pictures = m_pictures;
        s.previewSerial = m_serial;
        s.measuredFps = m_fps;
        s.audioPeak = m_peak;
        m_peak = 0.0f;
    }
    if (const auto rec = recorder()) {
        s.recording = true;
        s.recorder = rec->status();
    }
    return s;
}

CaptureFrame CaptureSession::latestFrame() const {
    std::lock_guard lock(m_frameMutex);
    return m_latest;
}

VideoFormat CaptureSession::format() const {
    std::lock_guard lock(m_control);
    return m_format;
}

} // namespace wabcut::capture
