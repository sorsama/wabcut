#include <wabcut/capture/FakeBackend.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>
#include <thread>

namespace wabcut::capture {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

constexpr int kCell = 16;
constexpr int kBits = 16;

std::uint8_t lumaAt(int x, int y, std::int64_t index) {
    if (y < kCell && x < kCell * kBits) {
        return ((index >> (x / kCell)) & 1) != 0 ? 235 : 16;
    }
    return static_cast<std::uint8_t>(40 + ((x / 4 + y / 4 + index * 3) % 160));
}

void sleepUntil(Clock::time_point when, const std::atomic<bool>& stop) {
    while (!stop.load() && Clock::now() < when) {
        std::this_thread::sleep_for(std::min<Clock::duration>(when - Clock::now(), std::chrono::milliseconds(5)));
    }
}

class FakeVideoSource final : public VideoSource {
public:
    FakeVideoSource(std::string name, VideoFormat format, int startDelayMs, int failAfter)
        : m_name(std::move(name)), m_format(std::move(format)), m_startDelayMs(startDelayMs), m_failAfter(failAfter) {}
    ~FakeVideoSource() override { shutdown(); } // non-virtual: no virtual call while destroying

    std::string description() const override { return m_name; }
    VideoFormat format() const override { return m_format; }

    std::expected<void, Error> start(FrameSink sink) override {
        if (m_thread.joinable()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "The fake source is running"});
        }
        m_stop = false;
        m_sink = std::move(sink);
        m_thread = std::thread([this] { run(); });
        return {};
    }

    void stop() noexcept override { shutdown(); }

    /// What stop() does; the destructor calls it directly.
    void shutdown() noexcept {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    std::string failure() const override {
        std::lock_guard lock(m_failMutex);
        return m_failure;
    }

private:
    void run() {
        const double fps = std::max(1.0, m_format.fps());
        const auto period = std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / fps));
        const auto start = Clock::now() + std::chrono::milliseconds(m_startDelayMs);
        const std::int64_t startNs = nowNs() + static_cast<std::int64_t>(m_startDelayMs) * 1'000'000;
        for (std::int64_t k = 0; !m_stop.load(); ++k) {
            if (m_failAfter >= 0 && k >= m_failAfter) {
                std::lock_guard lock(m_failMutex);
                m_failure = "The device stopped delivering pictures (it may have been unplugged).";
                return;
            }
            sleepUntil(start + period * k, m_stop);
            if (m_stop.load()) {
                return;
            }
            const std::int64_t due = startNs + static_cast<std::int64_t>(static_cast<double>(k) * 1e9 / fps);
            CaptureFrame frame = makeFrame(m_format.pixel, m_format.width, m_format.height, due,
                                           [&](std::uint8_t* bytes) { drawFakePicture(m_format.pixel, m_format.width, m_format.height, k, bytes); });
            m_sink(std::move(frame));
        }
    }

    std::string m_name;
    VideoFormat m_format;
    int m_startDelayMs;
    int m_failAfter;
    FrameSink m_sink;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_failMutex;
    std::string m_failure;
};

class FakeAudioSource final : public AudioSource {
public:
    FakeAudioSource(std::string name, int rate, int channels, int delayMs, double hz, float level)
        : m_name(std::move(name)), m_rate(rate), m_channels(channels), m_delayMs(delayMs), m_hz(hz), m_level(level) {}
    ~FakeAudioSource() override { shutdown(); } // non-virtual: no virtual call while destroying

    std::string description() const override { return m_name; }
    int sampleRate() const override { return m_rate; }
    int channels() const override { return m_channels; }

    std::expected<void, Error> start(BlockSink sink) override {
        if (m_thread.joinable()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "The fake source is running"});
        }
        m_stop = false;
        m_sink = std::move(sink);
        m_thread = std::thread([this] { run(); });
        return {};
    }

    void stop() noexcept override { shutdown(); }

    /// What stop() does; the destructor calls it directly.
    void shutdown() noexcept {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    void run() {
        const int blockFrames = std::max(1, m_rate / 100); // 10 ms
        const auto start = Clock::now() + std::chrono::milliseconds(m_delayMs);
        const std::int64_t startNs = nowNs() + static_cast<std::int64_t>(m_delayMs) * 1'000'000;
        for (std::int64_t n = 0; !m_stop.load(); ++n) {
            // A block is delivered when its last frame has been captured.
            sleepUntil(start + std::chrono::microseconds(10'000) * (n + 1), m_stop);
            if (m_stop.load()) {
                return;
            }
            AudioBlock block;
            block.channels = m_channels;
            block.firstFrame = n * blockFrames;
            block.hostNs = startNs + static_cast<std::int64_t>(static_cast<double>(block.firstFrame) * 1e9 / m_rate);
            block.samples.resize(static_cast<std::size_t>(blockFrames) * static_cast<std::size_t>(m_channels));
            for (int f = 0; f < blockFrames; ++f) {
                const double t = static_cast<double>(block.firstFrame + f) / m_rate;
                const float v = m_level * static_cast<float>(std::sin(2.0 * std::numbers::pi * m_hz * t));
                for (int c = 0; c < m_channels; ++c) {
                    block.samples[static_cast<std::size_t>(f) * static_cast<std::size_t>(m_channels) + static_cast<std::size_t>(c)] = v;
                }
            }
            m_sink(std::move(block));
        }
    }

    std::string m_name;
    int m_rate;
    int m_channels;
    int m_delayMs;
    double m_hz;
    float m_level;
    BlockSink m_sink;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};

VideoFormat format(int w, int h, Rate rate, PixelFormat pixel, const char* native, bool compressed = false) {
    VideoFormat f;
    f.width = w;
    f.height = h;
    f.frameRate = rate;
    f.pixel = pixel;
    f.nativeName = native;
    f.compressed = compressed;
    return f;
}

} // namespace

void drawFakePicture(PixelFormat pixel, int width, int height, std::int64_t index, std::uint8_t* bytes) {
    switch (pixel) {
    case PixelFormat::Nv12: {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                bytes[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = lumaAt(x, y, index);
            }
        }
        std::memset(bytes + static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 128,
                    static_cast<std::size_t>(width) * static_cast<std::size_t>((height + 1) / 2));
        break;
    }
    case PixelFormat::Yuy2: {
        const std::size_t stride = static_cast<std::size_t>(planeStride(pixel, width, 0));
        for (int y = 0; y < height; ++y) {
            std::uint8_t* row = bytes + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                row[2 * x] = lumaAt(x, y, index);
                row[2 * x + 1] = 128;
            }
        }
        break;
    }
    case PixelFormat::Bgra: {
        for (int y = 0; y < height; ++y) {
            std::uint8_t* row = bytes + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            for (int x = 0; x < width; ++x) {
                const int luma = lumaAt(x, y, index);
                const auto g = static_cast<std::uint8_t>(std::clamp((luma - 16) * 255 / 219, 0, 255));
                row[4 * x] = g;
                row[4 * x + 1] = g;
                row[4 * x + 2] = g;
                row[4 * x + 3] = 255;
            }
        }
        break;
    }
    default:
        std::memset(bytes, 128, frameBytes(pixel, width, height));
        break;
    }
}

std::int64_t fakePictureNumber(PixelFormat pixel, int width, int height, const std::uint8_t* bytes) {
    if (width < kCell * kBits || height < kCell) {
        return -1;
    }
    std::int64_t value = 0;
    for (int bit = 0; bit < kBits; ++bit) {
        const int x = bit * kCell + kCell / 2;
        const int y = kCell / 2;
        int sample = 0;
        switch (pixel) {
        case PixelFormat::Nv12:
            sample = bytes[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            break;
        case PixelFormat::Yuy2:
            sample = bytes[static_cast<std::size_t>(y) * static_cast<std::size_t>(planeStride(pixel, width, 0)) + static_cast<std::size_t>(2 * x)];
            break;
        case PixelFormat::Bgra:
            sample = bytes[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4 + static_cast<std::size_t>(4 * x) + 1];
            break;
        default:
            return -1;
        }
        if (sample >= 128) {
            value |= std::int64_t{1} << bit;
        }
    }
    return value;
}

FakeBackendOptions FakeCaptureBackend::defaults() {
    FakeBackendOptions o;
    VideoDevice hdmi;
    hdmi.id = "fake:hdmi";
    hdmi.name = "Fake HDMI Capture";
    hdmi.formats = {format(1920, 1080, Rate{30, 1}, PixelFormat::Nv12, "MJPG", true),
                    format(1280, 720, Rate{60, 1}, PixelFormat::Nv12, "NV12"),
                    format(1280, 720, Rate{30, 1}, PixelFormat::Nv12, "NV12"),
                    format(640, 360, Rate{30, 1}, PixelFormat::Yuy2, "YUY2")};
    VideoDevice deck;
    deck.id = "fake:deck";
    deck.name = "Fake DV Deck";
    VideoFormat dv = format(720, 480, Rate{30000, 1001}, PixelFormat::Yuy2, "YUY2");
    dv.pixelAspect = Rate{10, 11};
    dv.interlaced = true;
    deck.formats = {dv};
    o.video = {hdmi, deck};
    o.audio = {AudioDevice{"fake:mic", "Fake Microphone", true}, AudioDevice{"fake:line", "Fake Line In", false}};
    return o;
}

FakeCaptureBackend::FakeCaptureBackend(FakeBackendOptions options) : m_options(std::move(options)) {}

std::expected<std::vector<VideoDevice>, Error> FakeCaptureBackend::videoDevices() {
    std::vector<VideoDevice> devices = m_options.video;
    for (VideoDevice& d : devices) {
        d.formats.clear(); // listing opens nothing and knows no formats
    }
    return devices;
}

std::expected<std::vector<VideoFormat>, Error> FakeCaptureBackend::videoFormats(const std::string& deviceId) {
    m_formatQueries.fetch_add(1);
    for (const VideoDevice& d : m_options.video) {
        if (d.id == deviceId) {
            if (m_options.denyCamera) {
                return std::unexpected(accessDeniedError(AccessKind::Camera));
            }
            return d.formats;
        }
    }
    return std::unexpected(Error{ErrorCode::NotFound, "That video device is gone"});
}

std::expected<std::vector<AudioDevice>, Error> FakeCaptureBackend::audioDevices() {
    return m_options.audio;
}

std::expected<std::unique_ptr<VideoSource>, Error> FakeCaptureBackend::openVideo(const std::string& deviceId, const VideoFormat& fmt) {
    for (const VideoDevice& d : m_options.video) {
        if (d.id != deviceId) {
            continue;
        }
        if (m_options.denyCamera) {
            return std::unexpected(accessDeniedError(AccessKind::Camera));
        }
        if (std::ranges::find(d.formats, fmt) == d.formats.end()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "The device does not deliver that format"});
        }
        m_videoOpens.fetch_add(1);
        return std::make_unique<FakeVideoSource>(d.name, fmt, m_options.videoStartDelayMs, m_options.videoFailAfter);
    }
    return std::unexpected(Error{ErrorCode::NotFound, "That video device is gone"});
}

std::expected<std::unique_ptr<AudioSource>, Error> FakeCaptureBackend::openAudio(const std::string& deviceId, int sampleRate, int channels) {
    const AudioDevice* device = nullptr;
    for (const AudioDevice& d : m_options.audio) {
        if (deviceId.empty() ? d.isDefault : d.id == deviceId) {
            device = &d;
            break;
        }
    }
    if (!device) {
        return std::unexpected(Error{ErrorCode::NotFound, "That sound device is gone"});
    }
    if (m_options.denyMicrophone) {
        return std::unexpected(accessDeniedError(AccessKind::Microphone));
    }
    m_audioOpens.fetch_add(1);
    return std::make_unique<FakeAudioSource>(device->name, sampleRate, channels, m_options.audioDelayMs, m_options.toneHz, m_options.toneLevel);
}

} // namespace wabcut::capture
