#include <wabcut/audio/dsp/TimeStretch.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace wabcut::audio::dsp {

struct TimeStretch::Impl {
    int channels = 2;
    int window = 960;   ///< N: 20 ms
    int hop = 480;      ///< synthesis hop Hs = N / 2 (Hann at 50 % overlap adds up to 1)
    int tolerance = 240; ///< search range +-N/4 (5 ms)
    double maxSpeed = 4.0;
    double speed = 1.0;
    int maxPush = 8192;

    std::vector<float> hann;

    // Input: interleaved frames [base, base + count) plus a mono mix for the similarity search.
    std::vector<float> in;
    std::vector<float> mono;
    int capacity = 0;
    long long base = 0;
    int count = 0;

    double inputPos = 0.0;   ///< ideal input position of the next synthesis frame
    long long prevPos = -1;  ///< where the previous frame was taken (-1: none yet)

    std::vector<float> acc;  ///< overlap-add accumulator, N frames
    std::vector<float> fifo; ///< finished output frames (ring)
    int fifoCapacity = 0;
    int fifoRead = 0;
    int fifoCount = 0;

    long long pushed = 0;
    long long produced = 0;

    float* frameAt(long long index) { return in.data() + static_cast<std::size_t>(index - base) * channels; }
    const float* monoAt(long long index) const { return mono.data() + (index - base); }

    void compact(long long keepFrom) {
        const long long drop = std::min<long long>(keepFrom - base, count);
        if (drop <= 0) {
            return;
        }
        const auto ch = static_cast<std::size_t>(channels);
        const auto d = static_cast<std::size_t>(drop);
        const auto rest = static_cast<std::size_t>(count) - d;
        std::memmove(in.data(), in.data() + d * ch, rest * ch * sizeof(float));
        std::memmove(mono.data(), mono.data() + d, rest * sizeof(float));
        base += drop;
        count = static_cast<int>(rest);
    }

    /// Normalised similarity of the candidate at `pos` with the template at `natural`, over half a window;
    /// `stride` subsamples the sum for the coarse pass.
    double similarity(long long natural, long long pos, int stride) const {
        const float* t = monoAt(natural);
        const float* c = monoAt(pos);
        double dot = 0.0;
        double energy = 1e-9;
        for (int i = 0; i < hop; i += stride) {
            dot += static_cast<double>(t[i]) * c[i];
            energy += static_cast<double>(c[i]) * c[i];
        }
        return dot / std::sqrt(energy);
    }

    /// One synthesis hop, if the input it needs is there and the FIFO has room.
    bool produceHop() {
        if (fifoCapacity - fifoCount < hop) {
            return false;
        }
        const long long end = base + count;
        const long long target = std::llround(inputPos);
        long long chosen = target;
        if (prevPos >= 0) {
            const long long natural = prevPos + hop;
            const long long lo = std::max(target - tolerance, base);
            const long long hi = target + tolerance;
            if (hi + window > end || natural + hop > end || natural < base) {
                return false;
            }
            // Coarse pass every 4th offset (subsampled sum), then refine around the best.
            double best = -1e300;
            for (long long p = lo; p <= hi; p += 4) {
                const double s = similarity(natural, p, 2);
                if (s > best) {
                    best = s;
                    chosen = p;
                }
            }
            const long long centre = chosen;
            best = -1e300;
            for (long long p = std::max(centre - 3, lo); p <= std::min(centre + 3, hi); ++p) {
                const double s = similarity(natural, p, 1);
                if (s > best) {
                    best = s;
                    chosen = p;
                }
            }
        } else if (target + window > end || target < base) {
            return false;
        }

        const auto ch = static_cast<std::size_t>(channels);
        const float* src = frameAt(chosen);
        for (int i = 0; i < window; ++i) {
            const float w = hann[static_cast<std::size_t>(i)];
            for (std::size_t c = 0; c < ch; ++c) {
                acc[static_cast<std::size_t>(i) * ch + c] += w * src[static_cast<std::size_t>(i) * ch + c];
            }
        }
        // The first hop of the accumulator is complete: move it to the FIFO.
        for (int i = 0; i < hop; ++i) {
            const int slot = (fifoRead + fifoCount + i) % fifoCapacity;
            std::memcpy(fifo.data() + static_cast<std::size_t>(slot) * ch, acc.data() + static_cast<std::size_t>(i) * ch,
                        ch * sizeof(float));
        }
        fifoCount += hop;
        produced += hop;
        std::memmove(acc.data(), acc.data() + static_cast<std::size_t>(hop) * ch,
                     static_cast<std::size_t>(window - hop) * ch * sizeof(float));
        std::fill(acc.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(window - hop) * ch), acc.end(), 0.0f);

        prevPos = chosen;
        inputPos += speed * hop;
        // Keep what the next frame can still read: its template and its search range.
        compact(std::min(prevPos + hop, std::llround(inputPos) - tolerance));
        return true;
    }
};

TimeStretch::TimeStretch(int channels, double sampleRate, double maxSpeed, int maxPushFrames)
    : m_impl(std::make_unique<Impl>()) {
    Impl& d = *m_impl;
    d.channels = std::clamp(channels, 1, kMaxDspChannels);
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    d.window = std::max(64, 2 * static_cast<int>(std::lround(0.01 * fs)));
    d.hop = d.window / 2;
    d.tolerance = d.window / 4;
    d.maxSpeed = std::max(maxSpeed, 1.0);
    d.maxPush = std::max(maxPushFrames, 1);
    d.hann.resize(static_cast<std::size_t>(d.window));
    for (int i = 0; i < d.window; ++i) { // periodic Hann: overlapping by half it sums to exactly 1
        d.hann[static_cast<std::size_t>(i)] =
            static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / d.window));
    }
    // Room for one push on top of everything a hop may still need: its window, the search range and the advance.
    d.capacity = d.maxPush + 2 * d.window + 2 * d.tolerance + static_cast<int>(std::ceil(d.maxSpeed * d.hop)) + d.hop;
    const auto ch = static_cast<std::size_t>(d.channels);
    d.in.assign(static_cast<std::size_t>(d.capacity) * ch, 0.0f);
    d.mono.assign(static_cast<std::size_t>(d.capacity), 0.0f);
    d.acc.assign(static_cast<std::size_t>(d.window) * ch, 0.0f);
    d.fifoCapacity = d.maxPush + 4 * d.window;
    d.fifo.assign(static_cast<std::size_t>(d.fifoCapacity) * ch, 0.0f);
    reset();
}

TimeStretch::~TimeStretch() = default;

void TimeStretch::setSpeed(double speed) noexcept {
    m_impl->speed = std::clamp(std::isfinite(speed) ? speed : 1.0, 0.125, m_impl->maxSpeed);
}

double TimeStretch::speed() const noexcept { return m_impl->speed; }

void TimeStretch::push(const float* interleaved, int frames) noexcept {
    Impl& d = *m_impl;
    frames = std::clamp(frames, 0, d.maxPush);
    if (d.count + frames > d.capacity) {
        // Not consumed fast enough (more input than the speed accounts for): drop the oldest.
        d.compact(d.base + (d.count + frames - d.capacity));
        if (d.prevPos >= 0 && d.prevPos + d.hop < d.base) {
            d.prevPos = -1; // the template is gone: start a fresh run of frames
        }
        d.inputPos = std::max(d.inputPos, static_cast<double>(d.base + d.tolerance));
    }
    const auto ch = static_cast<std::size_t>(d.channels);
    std::memcpy(d.in.data() + static_cast<std::size_t>(d.count) * ch, interleaved,
                static_cast<std::size_t>(frames) * ch * sizeof(float));
    for (int i = 0; i < frames; ++i) {
        float m = 0.0f;
        for (std::size_t c = 0; c < ch; ++c) {
            m += interleaved[static_cast<std::size_t>(i) * ch + c];
        }
        d.mono[static_cast<std::size_t>(d.count + i)] = m;
    }
    d.count += frames;
    d.pushed += frames;
    while (d.produceHop()) {
    }
}

int TimeStretch::pull(float* interleaved, int frames) noexcept {
    Impl& d = *m_impl;
    while (d.fifoCount < frames && d.produceHop()) {
    }
    const int n = std::min(frames, d.fifoCount);
    const auto ch = static_cast<std::size_t>(d.channels);
    for (int i = 0; i < n; ++i) {
        std::memcpy(interleaved + static_cast<std::size_t>(i) * ch,
                    d.fifo.data() + static_cast<std::size_t>((d.fifoRead + i) % d.fifoCapacity) * ch, ch * sizeof(float));
    }
    d.fifoRead = (d.fifoRead + n) % d.fifoCapacity;
    d.fifoCount -= n;
    return n;
}

int TimeStretch::available() const noexcept { return m_impl->fifoCount; }

int TimeStretch::latencyInputFrames() const noexcept { return m_impl->window + m_impl->tolerance; }

void TimeStretch::reset() noexcept {
    Impl& d = *m_impl;
    d.base = 0;
    d.count = 0;
    d.inputPos = 0.0;
    d.prevPos = -1;
    std::fill(d.acc.begin(), d.acc.end(), 0.0f);
    d.fifoRead = 0;
    d.fifoCount = 0;
    d.pushed = 0;
    d.produced = 0;
}

long long TimeStretch::inputFramesPushed() const noexcept { return m_impl->pushed; }
long long TimeStretch::outputFramesProduced() const noexcept { return m_impl->produced; }

} // namespace wabcut::audio::dsp
