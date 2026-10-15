#include <wabcut/export/VideoFrameSource.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace wabcut::exporter {

void drawSyntheticFrame(std::uint8_t* rgba, int width, int height, std::int64_t frameIndex, bool barcode) {
    if (!rgba || width <= 0 || height <= 0) {
        return;
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto k = static_cast<std::size_t>(std::max<std::int64_t>(frameIndex, 0));

    // Layer 1: background. Red is a triangle wave across x that drifts 2 steps per frame, green a vertical
    // ramp, blue a diagonal ramp. Tables keep the inner loop free of divisions.
    std::vector<std::uint8_t> red(w);
    for (std::size_t x = 0; x < w; ++x) {
        const std::size_t t = (x * 512 / w + 2 * k) % 512;
        red[x] = static_cast<std::uint8_t>(t < 256 ? t : 511 - t);
    }
    std::vector<std::uint8_t> blue(w + h);
    for (std::size_t d = 0; d < w + h; ++d) {
        blue[d] = static_cast<std::uint8_t>(64 + d * 127 / (w + h));
    }
    for (std::size_t y = 0; y < h; ++y) {
        std::uint8_t* row = rgba + y * w * 4;
        const auto green = static_cast<std::uint8_t>(h > 1 ? y * 255 / (h - 1) : 0);
        for (std::size_t x = 0; x < w; ++x) {
            row[4 * x + 0] = red[x];
            row[4 * x + 1] = green;
            row[4 * x + 2] = blue[x + y];
            row[4 * x + 3] = 255;
        }
    }

    auto fillRect = [&](std::size_t x0, std::size_t y0, std::size_t bw, std::size_t bh, std::uint8_t r,
                        std::uint8_t g, std::uint8_t b, bool blend) {
        const std::size_t x1 = std::min(w, x0 + bw);
        const std::size_t y1 = std::min(h, y0 + bh);
        for (std::size_t y = y0; y < y1; ++y) {
            std::uint8_t* p = rgba + (y * w + x0) * 4;
            for (std::size_t x = x0; x < x1; ++x, p += 4) {
                if (blend) { // 50 % over: (dst + src + 1) / 2
                    p[0] = static_cast<std::uint8_t>((p[0] + r + 1) / 2);
                    p[1] = static_cast<std::uint8_t>((p[1] + g + 1) / 2);
                    p[2] = static_cast<std::uint8_t>((p[2] + b + 1) / 2);
                } else {
                    p[0] = r;
                    p[1] = g;
                    p[2] = b;
                }
            }
        }
    };

    // Layer 2: an opaque box moving left to right, 8 px per frame.
    const std::size_t box1W = std::max<std::size_t>(2, w / 6);
    const std::size_t box1H = std::max<std::size_t>(2, h / 4);
    fillRect((k * 8) % std::max<std::size_t>(1, w > box1W ? w - box1W : 1), h / 3, box1W, box1H, 230, 60, 40,
             false);

    // Layer 3: a 50 % translucent box moving top to bottom, 6 px per frame.
    const std::size_t box2W = std::max<std::size_t>(2, w / 5);
    const std::size_t box2H = std::max<std::size_t>(2, h / 5);
    fillRect(w / 2 > box2W / 2 ? w / 2 - box2W / 2 : 0,
             (k * 6) % std::max<std::size_t>(1, h > box2H ? h - box2H : 1), box2W, box2H, 40, 120, 230, true);

    // Frame index barcode on top: bit b is the 32x32 cell at x = 32b, white = 1.
    if (barcode) {
        for (std::size_t bit = 0; bit < 16 && 32 * (bit + 1) <= w && h >= 32; ++bit) {
            const std::uint8_t v = ((k >> bit) & 1u) != 0 ? 255 : 0;
            fillRect(32 * bit, 0, 32, 32, v, v, v, false);
        }
    }
}

namespace {

class SyntheticVideoSource final : public IVideoFrameSource {
public:
    SyntheticVideoSource(const VideoSourceFormat& format, SyntheticPattern pattern)
        : m_format(format), m_pattern(pattern), m_maxInFlight(std::clamp(pattern.maxInFlight, 1, 16)) {
        // Frames are independent: render up to two at a time, like a GPU renderer overlapping readbacks.
        const int threads = std::min(m_maxInFlight, 2);
        for (int i = 0; i < threads; ++i) {
            m_workers.emplace_back([this] { workerMain(); });
        }
    }

    ~SyntheticVideoSource() override { stop(); }

    std::string description() const override { return "synthetic 3-layer test pattern"; }
    int maxInFlight() const override { return m_maxInFlight; }

    std::expected<void, Error> request(std::int64_t frameIndex) override {
        std::lock_guard lock(m_mutex);
        if (m_stopped) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Video source stopped"});
        }
        if (frameIndex < 0 || frameIndex >= m_format.frameCount) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Frame index out of range"});
        }
        if (static_cast<int>(m_order.size()) >= m_maxInFlight) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Too many frames in flight"});
        }
        m_order.push_back(frameIndex);
        m_todo.push_back(frameIndex);
        m_changed.notify_all();
        return {};
    }

    std::expected<media::FrameHandle, Error> next() override {
        std::unique_lock lock(m_mutex);
        if (m_order.empty()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "next() without a pending request"});
        }
        const std::int64_t index = m_order.front();
        m_changed.wait(lock, [&] { return m_stopped || m_done.contains(index); });
        if (m_stopped) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Video source stopped"});
        }
        auto node = m_done.extract(index);
        m_order.pop_front();
        return std::move(node.mapped());
    }

    void stop() override {
        // Callable from any thread, also concurrently (a cancel and the job's own teardown): one caller
        // joins, the others wait for it.
        std::lock_guard stopping(m_stopMutex);
        {
            std::lock_guard lock(m_mutex);
            m_stopped = true;
            m_todo.clear();
            m_done.clear();
        }
        m_changed.notify_all();
        for (std::jthread& t : m_workers) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

private:
    void workerMain() {
        for (;;) {
            std::int64_t index = 0;
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, [&] { return m_stopped || !m_todo.empty(); });
                if (m_stopped) {
                    return;
                }
                index = m_todo.front();
                m_todo.pop_front();
            }
            media::FrameHandle frame = render(index);
            {
                std::lock_guard lock(m_mutex);
                if (m_stopped) {
                    return;
                }
                m_done.emplace(index, std::move(frame));
            }
            m_changed.notify_all();
        }
    }

    media::FrameHandle render(std::int64_t index) const {
        const int w = m_format.width;
        const int h = m_format.height;
        const std::size_t bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
        std::shared_ptr<std::uint8_t[]> pixels(new std::uint8_t[bytes]);
        // The pattern (and barcode) show the sequence frame number, so a range export can be checked too.
        const std::int64_t startFrame =
            ticksToFrames(m_format.start, m_format.frameRate, Rounding::Nearest).value_or(0);
        drawSyntheticFrame(pixels.get(), w, h, startFrame + index, m_pattern.barcode);

        media::FrameHandle f;
        const Ticks period = framesToTicks(1, m_format.frameRate, Rounding::Nearest).value_or(Ticks{});
        f.pts =
            m_format.start + framesToTicks(index, m_format.frameRate, Rounding::Nearest).value_or(Ticks{});
        f.duration = period;
        f.streamPts = index;
        f.timeBase = timeBaseOf(m_format.frameRate);
        f.width = w;
        f.height = h;
        f.format = media::PixelFormat::RGBA8;
        f.color = ColorInfo{ColorMatrix::RGB, ColorRange::Full, ColorPrimaries::BT709,
                            TransferFunction::BT709, ChromaLocation::Unknown};
        media::CpuPlanes cpu;
        cpu.planes[0] = media::Plane{pixels.get(), w * 4, w, h};
        cpu.owner = std::shared_ptr<const void>(pixels, pixels.get());
        f.payload = std::move(cpu);
        return f;
    }

    VideoSourceFormat m_format;
    SyntheticPattern m_pattern;
    int m_maxInFlight = 3;
    std::mutex m_stopMutex; // serializes stop()
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::int64_t> m_order; // requested, not yet returned by next()
    std::deque<std::int64_t> m_todo;  // requested, not yet picked by a worker
    std::map<std::int64_t, media::FrameHandle> m_done;
    bool m_stopped = false;
    std::vector<std::jthread> m_workers; // last: joined before the state above is destroyed
};

} // namespace

std::unique_ptr<IVideoFrameSource> makeSyntheticVideoSource(const VideoSourceFormat& format,
                                                            SyntheticPattern pattern) {
    return std::make_unique<SyntheticVideoSource>(format, pattern);
}

} // namespace wabcut::exporter
