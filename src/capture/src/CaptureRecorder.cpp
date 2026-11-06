#include <wabcut/capture/CaptureRecorder.h>

#include <wabcut/export/CaptureWriter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace wabcut::capture {

namespace fs = std::filesystem;

namespace {

exporter::CapturePixel writerPixel(PixelFormat format) {
    switch (format) {
    case PixelFormat::Nv12:
        return exporter::CapturePixel::Nv12;
    case PixelFormat::Yuy2:
        return exporter::CapturePixel::Yuy2;
    case PixelFormat::Uyvy:
        return exporter::CapturePixel::Uyvy;
    case PixelFormat::I420:
        return exporter::CapturePixel::Yuv420p;
    case PixelFormat::P010:
        return exporter::CapturePixel::P010;
    case PixelFormat::Bgr24:
        return exporter::CapturePixel::Bgr24;
    case PixelFormat::Bgra:
        return exporter::CapturePixel::Bgra;
    }
    return exporter::CapturePixel::Nv12;
}

constexpr double kDriftToleranceSeconds = 0.05;
constexpr double kMaxGapSeconds = 30.0;        // a signal lost for longer than this does not stretch the file further
constexpr double kHeldAudioSeconds = 20.0;     // sound waiting for the first picture
constexpr std::int64_t kMaxPadSeconds = 600;   // the longest silence one correction inserts

} // namespace

struct CaptureRecorder::Impl {
    RecorderConfig cfg;
    std::unique_ptr<exporter::CaptureWriter> writer;
    Rate rate{30, 1};
    double fps = 30.0;
    std::size_t maxVideo = 32;

    std::mutex m;
    std::condition_variable cv;
    std::deque<CaptureFrame> videoQ;
    std::deque<AudioBlock> audioQ;
    bool stopFlag = false;
    std::atomic<bool> abandonFlag{false}; // written under `m`, read by the worker in between
    std::atomic<bool> accepting{false};
    std::thread worker;

    std::atomic<RecorderState> state{RecorderState::Idle};
    std::atomic<std::int64_t> framesWritten{0};
    std::atomic<std::uint64_t> repeated{0};
    std::atomic<std::uint64_t> lost{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::int64_t> audioFrames{0};
    std::atomic<std::int64_t> audioPadded{0};
    std::atomic<std::int64_t> audioTrimmed{0};
    std::atomic<std::int64_t> bytes{0};
    std::atomic<float> peak{0.0f};
    std::mutex textMutex;
    std::string error;
    std::string encoder;

    // Worker-owned.
    bool t0Known = false;
    std::int64_t t0Ns = 0;
    std::int64_t nextSlot = 0;
    CaptureFrame last;
    bool aligned = false;
    double ema = 0.0;
    std::int64_t deviceNext = -1;
    std::int64_t pendingSkip = 0;
    std::int64_t audioWritten = 0;
    std::deque<AudioBlock> held;
    std::int64_t heldFrames = 0;
    std::chrono::steady_clock::time_point lastSize{};
    std::vector<float> silence;

    void fail(std::string message) {
        {
            std::lock_guard lock(textMutex);
            if (error.empty()) {
                error = std::move(message);
            }
        }
        state.store(RecorderState::Failed);
    }
    bool failed() const { return state.load() == RecorderState::Failed; }

    void refreshSize(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastSize < std::chrono::milliseconds(500)) {
            return;
        }
        lastSize = now;
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(cfg.file, ec);
        if (!ec) {
            bytes.store(static_cast<std::int64_t>(size));
        }
    }

    bool writeSlot(const CaptureFrame& frame) {
        exporter::CapturePicture picture;
        for (int p = 0; p < planeCount(frame.pixel) && p < 3; ++p) {
            picture.planes[p] = frame.plane(p);
            picture.strides[p] = frame.stride(p);
        }
        if (auto written = writer->writeVideo(nextSlot, picture); !written) {
            fail("The picture could not be written: " + written.error().message);
            return false;
        }
        ++nextSlot;
        framesWritten.store(nextSlot);
        return true;
    }

    void handleVideo(CaptureFrame& frame) {
        if (failed()) {
            return;
        }
        if (cfg.encodeHook) {
            cfg.encodeHook();
        }
        if (!frame.valid() || frame.width != cfg.video.width || frame.height != cfg.video.height || frame.pixel != cfg.video.pixel) {
            rejected.fetch_add(1);
            return;
        }
        if (!t0Known) {
            t0Ns = frame.hostNs;
            t0Known = true;
            nextSlot = 0;
            state.store(RecorderState::Recording);
        }
        std::int64_t slot = std::llround(static_cast<double>(frame.hostNs - t0Ns) * fps / 1e9);
        if (slot < nextSlot) {
            return; // a picture for a slot that is filled already
        }
        const auto maxGap = static_cast<std::int64_t>(fps * kMaxGapSeconds) + 1;
        if (slot - nextSlot > maxGap) {
            const std::int64_t cut = slot - nextSlot - maxGap; // the clock jumped, or the signal was lost for ages
            t0Ns += static_cast<std::int64_t>(static_cast<double>(cut) * 1e9 / fps);
            slot -= cut;
        }
        while (nextSlot < slot) {
            if (!writeSlot(last)) {
                return;
            }
            repeated.fetch_add(1);
        }
        if (!writeSlot(frame)) {
            return;
        }
        last = std::move(frame);
        refreshSize(false);
    }

    bool writeSamples(const float* data, std::int64_t frames) {
        if (frames <= 0) {
            return true;
        }
        const auto channels = static_cast<std::size_t>(cfg.audioChannels);
        if (auto written = writer->writeAudio(std::span<const float>(data, static_cast<std::size_t>(frames) * channels)); !written) {
            fail("The sound could not be written: " + written.error().message);
            return false;
        }
        audioWritten += frames;
        audioFrames.store(audioWritten);
        return true;
    }

    bool pad(std::int64_t frames) {
        frames = std::min<std::int64_t>(frames, kMaxPadSeconds * cfg.audioRate);
        const auto channels = static_cast<std::size_t>(cfg.audioChannels);
        const std::int64_t chunk = std::max(cfg.audioRate, 1);
        if (silence.size() < static_cast<std::size_t>(chunk) * channels) {
            silence.assign(static_cast<std::size_t>(chunk) * channels, 0.0f);
        }
        std::int64_t left = frames;
        while (left > 0) {
            const std::int64_t n = std::min(left, chunk);
            if (!writeSamples(silence.data(), n)) {
                return false;
            }
            left -= n;
        }
        audioPadded.fetch_add(frames);
        return true;
    }

    void handleAudio(AudioBlock& block) {
        if (failed() || !cfg.audio || block.channels != cfg.audioChannels || block.samples.empty()) {
            return;
        }
        const auto frames = static_cast<std::int64_t>(block.frames());
        float blockPeak = 0.0f;
        for (const float s : block.samples) {
            blockPeak = std::max(blockPeak, std::abs(s));
        }
        float seen = peak.load();
        while (blockPeak > seen && !peak.compare_exchange_weak(seen, blockPeak)) {
        }
        if (deviceNext >= 0 && block.firstFrame > deviceNext) {
            if (!pad(block.firstFrame - deviceNext)) { // sound the device lost
                return;
            }
        }
        deviceNext = block.firstFrame + frames;

        const double sampleRate = cfg.audioRate;
        const auto byClock = static_cast<std::int64_t>(std::llround(static_cast<double>(block.hostNs - t0Ns) * sampleRate / 1e9));
        // Where this block's first kept frame belongs, less where it will land: frames still to be dropped from the front of
        // the sound (pendingSkip) move its first kept frame that much later on the device's clock.
        const std::int64_t err = byClock + pendingSkip - audioWritten;
        std::int64_t adjust = 0;
        if (!aligned) {
            aligned = true;
            ema = 0.0;
            adjust = err;
        } else {
            ema += 0.05 * (static_cast<double>(err) - ema);
            if (std::abs(ema) > kDriftToleranceSeconds * sampleRate) {
                adjust = static_cast<std::int64_t>(std::llround(ema));
                ema = 0.0;
            }
        }
        if (adjust > 0) {
            if (!pad(adjust)) {
                return;
            }
        } else if (adjust < 0) {
            pendingSkip += -adjust;
        }
        const std::int64_t skip = std::min(pendingSkip, frames);
        pendingSkip -= skip;
        if (skip > 0) {
            audioTrimmed.fetch_add(skip);
        }
        writeSamples(block.samples.data() + static_cast<std::size_t>(skip) * static_cast<std::size_t>(cfg.audioChannels), frames - skip);
    }

    void holdAudio(AudioBlock&& block) {
        heldFrames += static_cast<std::int64_t>(block.frames());
        held.push_back(std::move(block));
        while (heldFrames > static_cast<std::int64_t>(kHeldAudioSeconds * cfg.audioRate) && !held.empty()) {
            heldFrames -= static_cast<std::int64_t>(held.front().frames());
            held.pop_front();
        }
    }

    void run() {
        for (;;) {
            std::deque<CaptureFrame> videos;
            std::deque<AudioBlock> blocks;
            bool stopping = false;
            {
                std::unique_lock lock(m);
                cv.wait(lock, [&] { return stopFlag || abandonFlag || !videoQ.empty() || !audioQ.empty(); });
                if (abandonFlag) {
                    return;
                }
                videos.swap(videoQ);
                blocks.swap(audioQ);
                stopping = stopFlag;
            }
            for (CaptureFrame& frame : videos) {
                if (abandonFlag) {
                    return;
                }
                handleVideo(frame);
            }
            for (AudioBlock& block : blocks) {
                holdAudio(std::move(block));
            }
            if (t0Known) {
                while (!held.empty()) {
                    if (abandonFlag) {
                        return;
                    }
                    AudioBlock block = std::move(held.front());
                    held.pop_front();
                    heldFrames -= static_cast<std::int64_t>(block.frames());
                    handleAudio(block);
                }
            }
            refreshSize(false);
            if (stopping) {
                std::lock_guard lock(m);
                if (videoQ.empty() && audioQ.empty()) {
                    return;
                }
            }
        }
    }

    void join() {
        if (worker.joinable()) {
            worker.join();
        }
    }
};

CaptureRecorder::CaptureRecorder() : m_impl(std::make_unique<Impl>()) {}

CaptureRecorder::~CaptureRecorder() {
    if (m_impl->worker.joinable()) {
        abandon();
    }
}

std::expected<void, Error> CaptureRecorder::start(const RecorderConfig& config) {
    Impl& i = *m_impl;
    if (i.state.load() != RecorderState::Idle || i.worker.joinable()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "A capture is recording already"});
    }
    const VideoFormat& v = config.video;
    if (v.width < 2 || v.height < 2 || (v.width & 1) != 0 || (v.height & 1) != 0 || !v.frameRate.isValid()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The capture format is not usable"});
    }
    if (config.audio && (config.audioChannels < 1 || config.audioChannels > 2 || config.audioRate < 8000)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The capture's audio format is not usable"});
    }
    const CapturePreset& preset = capturePreset(config.presetId);
    exporter::CaptureWriterSettings ws;
    ws.file = config.file;
    ws.container = preset.container;
    ws.video = preset.video;
    if (!config.encoder.empty()) {
        ws.video.encoder = config.encoder;
    }
    ws.width = v.width;
    ws.height = v.height;
    ws.frameRate = snapFrameRate(v.frameRate);
    ws.pixelAspect = v.pixelAspect.isValid() ? v.pixelAspect : Rate{1, 1};
    ws.pixel = writerPixel(v.pixel);
    ws.hasAudio = config.audio;
    ws.audio = preset.audio;
    ws.audio.sampleRate = config.audioRate;
    ws.audio.channels = config.audioChannels;
    ws.title = config.title;
    auto writer = exporter::CaptureWriter::open(ws);
    if (!writer) {
        return std::unexpected(writer.error());
    }
    if (auto journal = writeJournal(config.file, config.provenance); !journal) {
        writer->reset();
        std::error_code ec;
        fs::remove(config.file, ec);
        return std::unexpected(journal.error());
    }

    i.cfg = config;
    i.writer = std::move(*writer);
    i.rate = ws.frameRate;
    i.fps = static_cast<double>(i.rate.num) / i.rate.den;
    {
        std::size_t cap = config.maxQueuedFrames > 0 ? static_cast<std::size_t>(config.maxQueuedFrames)
                                                      : static_cast<std::size_t>(std::clamp(i.fps * 1.5, 4.0, 128.0));
        if (config.maxQueuedFrames <= 0) {
            const std::size_t bytesPerFrame = std::max<std::size_t>(frameBytes(v.pixel, v.width, v.height), 1);
            cap = std::clamp<std::size_t>((512u << 20) / bytesPerFrame, 4, cap);
        }
        i.maxVideo = cap;
    }
    i.videoQ.clear();
    i.audioQ.clear();
    i.held.clear();
    i.heldFrames = 0;
    i.stopFlag = false;
    i.abandonFlag = false;
    i.t0Known = false;
    i.t0Ns = 0;
    i.nextSlot = 0;
    i.last = CaptureFrame{};
    i.aligned = false;
    i.ema = 0.0;
    i.deviceNext = -1;
    i.pendingSkip = 0;
    i.audioWritten = 0;
    i.framesWritten = 0;
    i.repeated = 0;
    i.lost = 0;
    i.rejected = 0;
    i.audioFrames = 0;
    i.audioPadded = 0;
    i.audioTrimmed = 0;
    i.bytes = 0;
    i.peak = 0.0f;
    i.lastSize = {};
    {
        std::lock_guard lock(i.textMutex);
        i.error.clear();
        i.encoder = i.writer->encoderName();
    }
    i.state.store(RecorderState::Waiting);
    i.accepting.store(true);
    i.worker = std::thread([this] { m_impl->run(); });
    return {};
}

void CaptureRecorder::pushVideo(CaptureFrame frame) noexcept {
    Impl& i = *m_impl;
    if (!i.accepting.load(std::memory_order_acquire)) {
        return;
    }
    try {
        {
            std::lock_guard lock(i.m);
            if (i.videoQ.size() >= i.maxVideo) {
                i.lost.fetch_add(1);
                return;
            }
            i.videoQ.push_back(std::move(frame));
        }
        i.cv.notify_one();
    } catch (...) {
        i.lost.fetch_add(1);
    }
}

void CaptureRecorder::pushAudio(AudioBlock block) noexcept {
    Impl& i = *m_impl;
    if (!i.accepting.load(std::memory_order_acquire) || !i.cfg.audio) {
        return;
    }
    try {
        {
            std::lock_guard lock(i.m);
            i.audioQ.push_back(std::move(block));
        }
        i.cv.notify_one();
    } catch (...) {
    }
}

std::expected<RecorderResult, Error> CaptureRecorder::stop() {
    Impl& i = *m_impl;
    if (!i.worker.joinable()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No capture is recording"});
    }
    i.accepting.store(false);
    {
        std::lock_guard lock(i.m);
        i.stopFlag = true;
    }
    i.cv.notify_all();
    i.join();

    const fs::path file = i.cfg.file;
    const bool failed = i.failed();
    std::string error;
    {
        std::lock_guard lock(i.textMutex);
        error = i.error;
    }
    const std::int64_t frames = i.framesWritten.load();
    if (failed) {
        i.writer.reset(); // the file stays, with its journal, for recovery
        i.state.store(RecorderState::Idle);
        return std::unexpected(Error{ErrorCode::IoError, error});
    }
    if (frames == 0) {
        i.writer.reset();
        std::error_code ec;
        fs::remove(file, ec);
        removeJournal(file);
        i.state.store(RecorderState::Idle);
        return std::unexpected(Error{ErrorCode::NotFound, "No picture arrived from the device"});
    }
    auto finished = i.writer->finish();
    i.writer.reset();
    i.state.store(RecorderState::Idle);
    if (!finished) {
        return std::unexpected(finished.error()); // the file and the journal stay
    }
    removeJournal(file);
    RecorderResult result;
    result.file = file;
    result.videoFrames = finished->videoFrames;
    result.audioFrames = finished->audioFrames;
    result.seconds = static_cast<double>(frames) / i.fps;
    result.framesDropped = i.repeated.load();
    result.encoder = finished->encoder;
    result.video = i.cfg.video;
    result.audio = i.cfg.audio;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    result.bytes = ec ? 0 : static_cast<std::int64_t>(size);
    return result;
}

void CaptureRecorder::abandon() noexcept {
    Impl& i = *m_impl;
    i.accepting.store(false);
    {
        std::lock_guard lock(i.m);
        i.abandonFlag = true;
        i.stopFlag = true;
    }
    i.cv.notify_all();
    i.join();
    i.writer.reset(); // closes the file as it is: no index, the journal stays
    i.state.store(RecorderState::Idle);
}

void CaptureRecorder::cancel() noexcept {
    const fs::path file = m_impl->cfg.file;
    const bool wasActive = m_impl->worker.joinable();
    abandon();
    if (wasActive) {
        std::error_code ec;
        fs::remove(file, ec);
        removeJournal(file);
    }
}

RecorderStatus CaptureRecorder::status() {
    Impl& i = *m_impl;
    RecorderStatus s;
    s.state = i.state.load();
    s.framesWritten = i.framesWritten.load();
    s.framesRepeated = i.repeated.load();
    s.framesDropped = s.framesRepeated;
    s.framesLost = i.lost.load();
    s.framesRejected = i.rejected.load();
    s.audioFrames = i.audioFrames.load();
    s.audioPadded = i.audioPadded.load();
    s.audioTrimmed = i.audioTrimmed.load();
    s.seconds = i.fps > 0.0 ? static_cast<double>(s.framesWritten) / i.fps : 0.0;
    s.bytes = i.bytes.load();
    s.audioPeak = i.peak.exchange(0.0f);
    std::lock_guard lock(i.textMutex);
    s.error = i.error;
    s.encoder = i.encoder;
    return s;
}

bool CaptureRecorder::active() const {
    return m_impl->worker.joinable();
}

const fs::path& CaptureRecorder::file() const {
    return m_impl->cfg.file;
}

} // namespace wabcut::capture
