#include <wabcut/audio/dsp/Loudness.h>

#include <ebur128.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut::audio::dsp {

namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double toDb(double linear) { return linear > 0.0 ? 20.0 * std::log10(linear) : kNegInf; }

/// libebur128 reports silence as -HUGE_VAL; keep it as -infinity.
double finiteOrNegInf(double v) { return std::isfinite(v) ? v : kNegInf; }

} // namespace

struct LoudnessMeter::Impl {
    ebur128_state* state = nullptr;
    std::size_t sinceWindow = 0;  ///< frames added since the momentary/short-term maxima were last read
    std::size_t window = 4800;    ///< read them every 100 ms
    double maxMomentary = kNegInf;
    double maxShortTerm = kNegInf;
    std::int64_t frames = 0;

    ~Impl() {
        if (state != nullptr) {
            ebur128_destroy(&state);
        }
    }

    void readWindows() {
        double v = 0.0;
        if (ebur128_loudness_momentary(state, &v) == EBUR128_SUCCESS && std::isfinite(v)) {
            maxMomentary = std::max(maxMomentary, v);
        }
        // Short-term needs 3 s of audio; before that libebur128 reports the loudness of what it has.
        if (frames >= static_cast<std::int64_t>(window) * 30 &&
            ebur128_loudness_shortterm(state, &v) == EBUR128_SUCCESS && std::isfinite(v)) {
            maxShortTerm = std::max(maxShortTerm, v);
        }
        sinceWindow = 0;
    }
};

LoudnessMeter::LoudnessMeter(int channels, int sampleRate)
    : m_impl(std::make_unique<Impl>()), m_channels(std::max(channels, 1)), m_sampleRate(std::max(sampleRate, 1)) {
    reset();
}

LoudnessMeter::~LoudnessMeter() = default;
LoudnessMeter::LoudnessMeter(LoudnessMeter&&) noexcept = default;
LoudnessMeter& LoudnessMeter::operator=(LoudnessMeter&&) noexcept = default;

bool LoudnessMeter::valid() const noexcept { return m_impl && m_impl->state != nullptr; }

void LoudnessMeter::reset() {
    Impl& d = *m_impl;
    if (d.state != nullptr) {
        ebur128_destroy(&d.state);
    }
    d.state = ebur128_init(static_cast<unsigned>(m_channels), static_cast<unsigned long>(m_sampleRate),
                           EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_S);
    if (d.state != nullptr) {
        static constexpr int kMap[] = {EBUR128_LEFT, EBUR128_RIGHT, EBUR128_CENTER, EBUR128_UNUSED,
                                       EBUR128_LEFT_SURROUND, EBUR128_RIGHT_SURROUND};
        for (int c = 0; c < m_channels; ++c) {
            const int role = m_channels == 1 ? EBUR128_DUAL_MONO : (c < 6 ? kMap[c] : EBUR128_UNUSED);
            ebur128_set_channel(d.state, static_cast<unsigned>(c), role);
        }
    }
    d.window = static_cast<std::size_t>(std::max(m_sampleRate / 10, 1));
    d.sinceWindow = 0;
    d.maxMomentary = kNegInf;
    d.maxShortTerm = kNegInf;
    d.frames = 0;
}

void LoudnessMeter::add(const float* interleaved, std::size_t frames) {
    Impl& d = *m_impl;
    if (d.state == nullptr) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    while (frames > 0) {
        const std::size_t n = std::min(frames, d.window - d.sinceWindow);
        ebur128_add_frames_float(d.state, interleaved, n);
        interleaved += n * ch;
        frames -= n;
        d.frames += static_cast<std::int64_t>(n);
        d.sinceWindow += n;
        if (d.sinceWindow == d.window) {
            d.readWindows();
        }
    }
}

LoudnessResult LoudnessMeter::result() const {
    LoudnessResult r;
    const Impl& d = *m_impl;
    r.frames = d.frames;
    if (d.state == nullptr) {
        return r;
    }
    double v = 0.0;
    if (ebur128_loudness_global(d.state, &v) == EBUR128_SUCCESS) {
        r.integratedLufs = finiteOrNegInf(v);
    }
    if (ebur128_loudness_range(d.state, &v) == EBUR128_SUCCESS && std::isfinite(v)) {
        r.rangeLu = v;
    }
    double truePeak = 0.0;
    double samplePeak = 0.0;
    for (int c = 0; c < m_channels; ++c) {
        if (ebur128_true_peak(d.state, static_cast<unsigned>(c), &v) == EBUR128_SUCCESS) {
            truePeak = std::max(truePeak, v);
        }
        if (ebur128_sample_peak(d.state, static_cast<unsigned>(c), &v) == EBUR128_SUCCESS) {
            samplePeak = std::max(samplePeak, v);
        }
    }
    r.truePeakDbtp = toDb(truePeak);
    r.samplePeakDbfs = toDb(samplePeak);
    r.maxMomentaryLufs = d.maxMomentary;
    r.maxShortTermLufs = d.maxShortTerm;
    return r;
}

double LoudnessMeter::momentaryLufs() const {
    double v = 0.0;
    if (m_impl->state != nullptr && m_impl->frames > 0 && ebur128_loudness_momentary(m_impl->state, &v) == EBUR128_SUCCESS) {
        return finiteOrNegInf(v);
    }
    return kNegInf;
}

double LoudnessMeter::shortTermLufs() const {
    double v = 0.0;
    if (m_impl->state != nullptr && m_impl->frames > 0 && ebur128_loudness_shortterm(m_impl->state, &v) == EBUR128_SUCCESS) {
        return finiteOrNegInf(v);
    }
    return kNegInf;
}

double LoudnessMeter::integratedLufs() const {
    double v = 0.0;
    if (m_impl->state != nullptr && ebur128_loudness_global(m_impl->state, &v) == EBUR128_SUCCESS) {
        return finiteOrNegInf(v);
    }
    return kNegInf;
}

double LoudnessMeter::truePeakDbtp() const {
    double peak = 0.0;
    double v = 0.0;
    for (int c = 0; m_impl->state != nullptr && c < m_channels; ++c) {
        if (ebur128_true_peak(m_impl->state, static_cast<unsigned>(c), &v) == EBUR128_SUCCESS) {
            peak = std::max(peak, v);
        }
    }
    return toDb(peak);
}

double LoudnessMeter::maxMomentaryLufs() const { return m_impl->maxMomentary; }
double LoudnessMeter::maxShortTermLufs() const { return m_impl->maxShortTerm; }
std::int64_t LoudnessMeter::frames() const { return m_impl->frames; }

double normalizationGainDb(const LoudnessResult& measured, double targetLufs) noexcept {
    return measured.hasIntegrated() ? targetLufs - measured.integratedLufs : 0.0;
}

} // namespace wabcut::audio::dsp
