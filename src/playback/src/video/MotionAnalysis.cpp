// Motion analysis for the flow mixes (P.2, MotionAnalysis.h): its own CPU decoders, the canvas pictures, the flow, an
// LRU of results and the background workers.

#include <wabcut/playback/video/MotionAnalysis.h>

#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wabcut::playback {

namespace {

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

std::uint64_t mixDouble(std::uint64_t h, double v) { return mix(h, std::bit_cast<std::uint64_t>(v)); }

std::uint64_t mixString(std::uint64_t h, const std::string& s) {
    std::uint64_t f = 0xcbf29ce484222325ull;
    for (const char c : s) {
        f ^= static_cast<unsigned char>(c);
        f *= 0x100000001b3ull;
    }
    return mix(h, f);
}

std::uint64_t pictureKey(std::uint64_t h, const MotionPicture& p) {
    h = mixString(h, p.path);
    h = mix(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(p.stream)));
    h = mix(h, static_cast<std::uint64_t>(p.time.value));
    h = mixDouble(h, p.transform.position.x);
    h = mixDouble(h, p.transform.position.y);
    h = mixDouble(h, p.transform.scale);
    h = mixDouble(h, p.transform.rotationDegrees);
    h = mixDouble(h, p.transform.anchor.x);
    h = mixDouble(h, p.transform.anchor.y);
    h = mix(h, static_cast<std::uint64_t>(p.frameWidth));
    h = mix(h, static_cast<std::uint64_t>(p.frameHeight));
    h = mixDouble(h, p.pixelAspect);
    return mix(h, p.identity);
}

std::size_t bytesOf(const motion::FlowPair& pair) { return pair.data.size() * sizeof(float) + sizeof(motion::FlowPair); }

/// Decoders of the analysis: CPU frames of one stream, borrowed by one thread at a time.
class DecoderShelf {
public:
    using DecoderPtr = std::unique_ptr<media::VideoDecoder>;

    /// A free decoder of (path, stream), opened when there is none.
    std::expected<DecoderPtr, Error> borrow(const std::string& path, int stream) {
        {
            std::lock_guard lock(m_mutex);
            for (auto it = m_idle.begin(); it != m_idle.end(); ++it) {
                if (it->path == path && it->stream == stream) {
                    DecoderPtr d = std::move(it->decoder);
                    m_idle.erase(it);
                    return d;
                }
            }
        }
        media::DecoderOptions options;
        options.outputMode = media::OutputMode::Cpu;
        options.snapToStreamTicks = true; // the frames playback and export show for the same source time
        auto opened = media::VideoDecoder::open(path, stream, options);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        return std::move(*opened);
    }

    /// Back on the shelf (most recent first); the oldest beyond the cap are closed (outside the lock).
    void giveBack(const std::string& path, int stream, DecoderPtr decoder) {
        std::vector<DecoderPtr> closing;
        {
            std::lock_guard lock(m_mutex);
            m_idle.push_front(Idle{path, stream, std::move(decoder)});
            while (m_idle.size() > kMaxIdle) {
                closing.push_back(std::move(m_idle.back().decoder));
                m_idle.pop_back();
            }
        }
    }

    void clear() {
        std::list<Idle> closing;
        std::lock_guard lock(m_mutex);
        closing.swap(m_idle);
    }

private:
    static constexpr std::size_t kMaxIdle = 6;
    struct Idle {
        std::string path;
        int stream = -1;
        DecoderPtr decoder;
    };
    std::mutex m_mutex;
    std::list<Idle> m_idle;
};

} // namespace

bool MotionRequest::isValid() const {
    const auto ok = [](const MotionPicture& p) {
        return !p.path.empty() && p.time.value >= 0 && p.frameWidth > 0 && p.frameHeight > 0 && p.pixelAspect > 0.0 &&
               std::isfinite(p.pixelAspect) && std::isfinite(p.transform.scale) && p.transform.scale != 0.0 &&
               std::isfinite(p.transform.rotationDegrees) && std::isfinite(p.transform.position.x) &&
               std::isfinite(p.transform.position.y) && std::isfinite(p.transform.anchor.x) &&
               std::isfinite(p.transform.anchor.y);
    };
    return ok(a) && ok(b) && canvas.isValid() && canvas.width <= 16384 && canvas.height <= 16384;
}

std::uint64_t MotionRequest::key() const {
    std::uint64_t h = 0x4D6F74696F6E5032ull; // "MotionP2"
    h = pictureKey(h, a);
    h = pictureKey(h, b);
    h = mix(h, static_cast<std::uint64_t>(canvas.width));
    return mix(h, static_cast<std::uint64_t>(canvas.height));
}

motion::Plane lumaOf(const media::FrameHandle& frame) {
    const media::CpuPlanes* cpu = frame.cpu();
    if (!cpu || frame.width <= 0 || frame.height <= 0) {
        return {};
    }
    const media::Plane& p = cpu->planes[0];
    if (!p.data || p.width <= 0 || p.height <= 0) {
        return {};
    }
    using F = media::PixelFormat;
    const int w = std::min(frame.width, p.width);
    const int h = std::min(frame.height, p.height);
    motion::Plane out(w, h);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
        float* dst = out.px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        switch (frame.format) {
        case F::NV12:
        case F::YUV420P:
        case F::YUV422P:
        case F::YUV444P:
        case F::YUVA420P:
            for (int x = 0; x < w; ++x) {
                dst[x] = static_cast<float>(row[x]) * (1.0f / 255.0f);
            }
            break;
        case F::P010:
            for (int x = 0; x < w; ++x) {
                std::uint16_t v = 0;
                std::memcpy(&v, row + 2 * x, 2);
                dst[x] = static_cast<float>(v >> 6) * (1.0f / 1023.0f);
            }
            break;
        case F::YUV420P10:
        case F::YUV422P10:
        case F::YUV444P10:
        case F::YUVA444P10:
            for (int x = 0; x < w; ++x) {
                std::uint16_t v = 0;
                std::memcpy(&v, row + 2 * x, 2);
                dst[x] = static_cast<float>(v & 0x3FF) * (1.0f / 1023.0f);
            }
            break;
        case F::RGBA8:
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* px = row + 4 * x;
                dst[x] = (0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2]) * (1.0f / 255.0f);
            }
            break;
        default:
            return {};
        }
    }
    return out;
}

motion::Plane placeOnCanvas(const motion::Plane& luma, const MotionPicture& picture, render::CanvasSize canvas, int width,
                            int height) {
    motion::Plane out(width, height);
    if (luma.empty() || width <= 0 || height <= 0 || !canvas.isValid()) {
        return out;
    }
    const double fw = picture.frameWidth > 0 ? picture.frameWidth : luma.width;
    const double fh = picture.frameHeight > 0 ? picture.frameHeight : luma.height;
    const render::Affine2D toCanvas = render::layerToCanvas(picture.transform, fw, fh, picture.pixelAspect, canvas);
    if (std::abs(toCanvas.determinant()) < 1e-12) {
        return out;
    }
    const render::Affine2D inverse = toCanvas.inverted();
    // Frame pixels per analysis pixel: shrink the luma that far first (an area average), so sampling does not alias.
    const double canvasPerPixel = static_cast<double>(canvas.width) / width;
    const double framePerCanvas = 1.0 / std::sqrt(std::abs(toCanvas.determinant()));
    const double k = std::max(1.0, canvasPerPixel * framePerCanvas * fw / luma.width);
    const motion::Plane small = k > 1.25 ? motion::resize(luma, std::max(1, static_cast<int>(std::lround(luma.width / k))),
                                                          std::max(1, static_cast<int>(std::lround(luma.height / k))))
                                         : luma;
    const double sx = static_cast<double>(small.width) / fw; // frame pixels -> small pixels
    const double sy = static_cast<double>(small.height) / fh;
    const double cx = static_cast<double>(canvas.width) / width;
    const double cy = static_cast<double>(canvas.height) / height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const render::Vec2 p = inverse.map({(x + 0.5) * cx, (y + 0.5) * cy});
            if (!(p.x >= 0.0 && p.y >= 0.0 && p.x < fw && p.y < fh)) {
                continue; // outside the frame: 0, as the transparent canvas around it
            }
            out(x, y) = small.sample(static_cast<float>(p.x * sx - 0.5), static_cast<float>(p.y * sy - 0.5));
        }
    }
    return out;
}

struct MotionAnalyzer::Impl {
    struct Job {
        MotionRequest request;
        std::uint64_t key = 0;
    };
    struct Entry {
        std::uint64_t key = 0;
        std::shared_ptr<const motion::FlowPair> motion;
        std::size_t bytes = 0;
    };

    std::size_t budget;
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    std::deque<Job> queue;
    std::unordered_map<std::uint64_t, std::vector<std::function<void()>>> waiting; // queued or running: their callbacks
    int running = 0;
    std::list<Entry> lru; // most recent first
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> byKey;
    std::size_t bytes = 0;
    std::uint64_t computed = 0;
    std::uint64_t failed = 0;
    std::unordered_set<std::uint64_t> failedKeys; ///< bounded: cleared when it grows past kMaxFailedKeys
    double lastMs = 0.0;
    static constexpr std::size_t kMaxQueued = 64;
    static constexpr std::size_t kMaxFailedKeys = 1024;

    void markFailed(std::uint64_t key) {
        ++failed;
        if (failedKeys.size() >= kMaxFailedKeys) {
            failedKeys.clear();
        }
        failedKeys.insert(key);
    }
    DecoderShelf decoders;
    std::vector<std::thread> workers;

    std::shared_ptr<const motion::FlowPair> lookup(std::uint64_t key) {
        const auto it = byKey.find(key);
        if (it == byKey.end()) {
            return nullptr;
        }
        lru.splice(lru.begin(), lru, it->second);
        return it->second->motion;
    }

    void store(std::uint64_t key, std::shared_ptr<const motion::FlowPair> motion) {
        if (byKey.contains(key)) {
            return;
        }
        Entry e{key, std::move(motion), 0};
        e.bytes = bytesOf(*e.motion);
        bytes += e.bytes;
        lru.push_front(std::move(e));
        byKey[key] = lru.begin();
        while (bytes > budget && lru.size() > 1) {
            bytes -= lru.back().bytes;
            byKey.erase(lru.back().key);
            lru.pop_back();
        }
    }

    /// Decodes one picture and draws it on the canvas at the analysis size.
    std::expected<motion::Plane, Error> picture(const MotionPicture& p, render::CanvasSize canvas, int width, int height) {
        auto decoder = decoders.borrow(p.path, p.stream);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        auto frame = (*decoder)->frameAt(p.time);
        decoders.giveBack(p.path, p.stream, std::move(*decoder));
        if (!frame) {
            return std::unexpected(frame.error());
        }
        const motion::Plane luma = lumaOf(*frame);
        if (luma.empty()) {
            return std::unexpected(Error{ErrorCode::Unsupported, "the frame's pixels cannot be read for motion analysis"});
        }
        return placeOnCanvas(luma, p, canvas, width, height);
    }

    std::expected<std::shared_ptr<const motion::FlowPair>, Error> analyse(const MotionRequest& r, int threads) {
        const auto start = std::chrono::steady_clock::now();
        int w = 0;
        int h = 0;
        motion::analysisSize(r.canvas.width, r.canvas.height, motion::kAnalysisSide, w, h);
        auto a = picture(r.a, r.canvas, w, h);
        if (!a) {
            return std::unexpected(a.error());
        }
        auto b = picture(r.b, r.canvas, w, h);
        if (!b) {
            return std::unexpected(b.error());
        }
        motion::FlowOptions options = motion::analysisFlowOptions();
        options.threads = threads;
        auto pair = std::make_shared<motion::FlowPair>(motion::estimateFlowPair(*a, *b, options));
        if (pair->empty()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "nothing to analyse"});
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::lock_guard lock(mutex);
        lastMs = ms;
        ++computed;
        return std::shared_ptr<const motion::FlowPair>(std::move(pair));
    }

    void work() {
        for (;;) {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return stopping || !queue.empty(); });
            if (stopping) {
                return;
            }
            const Job job = std::move(queue.front());
            queue.pop_front();
            ++running;
            lock.unlock();
            // Background work shares the CPU with decoding and the UI: two flow threads per worker.
            auto result = analyse(job.request, 2);
            lock.lock();
            if (result) {
                store(job.key, std::move(*result));
            } else {
                markFailed(job.key);
            }
            std::vector<std::function<void()>> callbacks;
            if (const auto it = waiting.find(job.key); it != waiting.end()) {
                callbacks = std::move(it->second);
                waiting.erase(it);
            }
            --running;
            lock.unlock();
            for (const auto& callback : callbacks) {
                if (callback) {
                    callback();
                }
            }
        }
    }
};

MotionAnalyzer::MotionAnalyzer(int workers, std::size_t budgetBytes) : m_impl(std::make_unique<Impl>()) {
    m_impl->budget = std::max<std::size_t>(budgetBytes, 1);
    for (int i = 0; i < std::max(1, workers); ++i) {
        m_impl->workers.emplace_back([impl = m_impl.get()] { impl->work(); });
    }
}

MotionAnalyzer::~MotionAnalyzer() {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->stopping = true;
        m_impl->queue.clear();
    }
    m_impl->cv.notify_all();
    for (std::thread& t : m_impl->workers) {
        t.join();
    }
}

MotionAnalyzer& MotionAnalyzer::shared() {
    static MotionAnalyzer analyzer;
    return analyzer;
}

std::shared_ptr<const motion::FlowPair> MotionAnalyzer::find(const MotionRequest& request) const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->lookup(request.key());
}

void MotionAnalyzer::request(const MotionRequest& request, bool urgent, std::function<void()> done) {
    if (!request.isValid()) {
        return;
    }
    const std::uint64_t key = request.key();
    std::vector<std::function<void()>> dropped;
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->lookup(key)) {
            return;
        }
        if (const auto it = m_impl->waiting.find(key); it != m_impl->waiting.end()) {
            if (done) {
                it->second.push_back(std::move(done));
            }
            if (urgent) {
                // A queued one moves to the front (a running one is already on its way).
                for (auto q = m_impl->queue.begin(); q != m_impl->queue.end(); ++q) {
                    if (q->key == key) {
                        Impl::Job job = std::move(*q);
                        m_impl->queue.erase(q);
                        m_impl->queue.push_front(std::move(job));
                        break;
                    }
                }
            }
            return;
        }
        std::vector<std::function<void()>>& callbacks = m_impl->waiting[key];
        if (done) {
            callbacks.push_back(std::move(done));
        }
        if (urgent) {
            m_impl->queue.push_front(Impl::Job{request, key});
        } else {
            m_impl->queue.push_back(Impl::Job{request, key});
        }
        // Bounded: the newest lookahead goes first (whoever waits for it is woken and asks again).
        while (m_impl->queue.size() > Impl::kMaxQueued) {
            if (const auto it = m_impl->waiting.find(m_impl->queue.back().key); it != m_impl->waiting.end()) {
                for (auto& c : it->second) {
                    dropped.push_back(std::move(c));
                }
                m_impl->waiting.erase(it);
            }
            m_impl->queue.pop_back();
        }
    }
    m_impl->cv.notify_one();
    for (const auto& callback : dropped) {
        if (callback) {
            callback();
        }
    }
}

std::expected<std::shared_ptr<const motion::FlowPair>, Error> MotionAnalyzer::compute(const MotionRequest& request) {
    if (!request.isValid()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "an invalid motion request"});
    }
    const std::uint64_t key = request.key();
    {
        std::lock_guard lock(m_impl->mutex);
        if (auto cached = m_impl->lookup(key)) {
            return cached;
        }
    }
    auto result = m_impl->analyse(request, 0);
    if (!result) {
        std::lock_guard lock(m_impl->mutex);
        m_impl->markFailed(key);
        return result;
    }
    std::lock_guard lock(m_impl->mutex);
    m_impl->store(key, *result);
    return result;
}

bool MotionAnalyzer::pending(const MotionRequest& request) const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->waiting.contains(request.key());
}

bool MotionAnalyzer::failed(const MotionRequest& request) const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->failedKeys.contains(request.key());
}

void MotionAnalyzer::cancelQueued() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard lock(m_impl->mutex);
        for (const Impl::Job& job : m_impl->queue) {
            if (const auto it = m_impl->waiting.find(job.key); it != m_impl->waiting.end()) {
                for (auto& c : it->second) {
                    callbacks.push_back(std::move(c));
                }
                m_impl->waiting.erase(it);
            }
        }
        m_impl->queue.clear();
    }
    for (const auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

MotionAnalyzer::Stats MotionAnalyzer::stats() const {
    std::lock_guard lock(m_impl->mutex);
    Stats s;
    s.queued = static_cast<int>(m_impl->queue.size());
    s.running = m_impl->running;
    s.computed = m_impl->computed;
    s.failed = m_impl->failed;
    s.cachedEntries = m_impl->lru.size();
    s.cachedBytes = m_impl->bytes;
    s.lastMilliseconds = m_impl->lastMs;
    return s;
}

void MotionAnalyzer::clear() {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->lru.clear();
        m_impl->byKey.clear();
        m_impl->bytes = 0;
        m_impl->failedKeys.clear();
    }
    m_impl->decoders.clear();
}

namespace {

class ComputedMotion final : public MotionSource {
public:
    std::shared_ptr<const motion::FlowPair> motion(const MotionRequest& request) override {
        auto result = MotionAnalyzer::shared().compute(request);
        return result ? *result : nullptr;
    }
};

} // namespace

MotionSource& computedMotion() {
    static ComputedMotion source;
    return source;
}

} // namespace wabcut::playback
