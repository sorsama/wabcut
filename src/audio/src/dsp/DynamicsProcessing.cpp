#include <wabcut/audio/dsp/DynamicsProcessing.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>

namespace wabcut::audio::dsp {

namespace {

constexpr double kFlush = 1e-30;
constexpr double kButterworthQ = 0.70710678118654752;
constexpr double kPi = std::numbers::pi;

double flushed(double v) noexcept { return std::abs(v) < kFlush ? 0.0 : v; }
double dbToGain(double db) noexcept { return db <= -144.0 ? 0.0 : std::pow(10.0, db / 20.0); }
double gainToDb(double gain) noexcept { return gain > 1e-12 ? 20.0 * std::log10(gain) : -240.0; }
/// One-pole coefficient of a time constant (1 - 1/e of a step in `ms`); 0 ms = instant.
double timeCoefficient(double ms, double rate) noexcept { return ms > 0.0 ? std::exp(-1000.0 / (ms * rate)) : 0.0; }

/// The second-order all-pass of the RBJ cookbook.
BiquadCoefficients allPassSection(double rate, double frequency, double q) noexcept {
    const double w0 = 2.0 * kPi * frequency / rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw = std::cos(w0);
    const double a0 = 1.0 + alpha;
    return {(1.0 - alpha) / a0, -2.0 * cosw / a0, 1.0, -2.0 * cosw / a0, (1.0 - alpha) / a0};
}

struct MeterRegistry {
    std::mutex mutex;
    std::map<std::uint64_t, std::weak_ptr<DynamicsMeters>> meters;
};

MeterRegistry& registry() {
    static MeterRegistry r;
    return r;
}

void raiseTo(std::atomic<float>& cell, float value) noexcept {
    float current = cell.load(std::memory_order_relaxed);
    while (value > current && !cell.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

// ---- The static curve -----------------------------------------------------------------------------------------------------

double dynamicsReductionDb(double levelDb, const DynamicsBand& band) noexcept {
    const double knee = std::max(band.kneeDb, 0.0);
    const double ratio = std::max(band.ratio, 1.0);
    const bool downward = band.mode == DynamicsMode::Compress || band.mode == DynamicsMode::Limit;
    double slope = 1.0;
    switch (band.mode) {
    case DynamicsMode::Compress:
        slope = 1.0 - 1.0 / ratio;
        break;
    case DynamicsMode::Limit:
        slope = 1.0; // ratio infinity
        break;
    case DynamicsMode::Expand:
        slope = ratio - 1.0;
        break;
    case DynamicsMode::Gate:
        slope = 1000.0; // the whole Range as soon as it is under
        break;
    }
    // How far into the section the level is: above the threshold for Compress and Limit, below it for Expand and Gate.
    const double over = downward ? levelDb - band.thresholdDb : band.thresholdDb - levelDb;
    double reduction = 0.0;
    if (knee > 0.0 && 2.0 * std::abs(over) <= knee) {
        const double d = over + 0.5 * knee;
        reduction = slope * d * d / (2.0 * knee);
    } else if (over > 0.0) {
        reduction = slope * over;
    }
    return std::clamp(reduction, 0.0, std::clamp(band.rangeDb, 0.0, 120.0));
}

double dynamicsCurveDb(double inputDb, const DynamicsBand& band) noexcept {
    if (band.bypass) {
        return inputDb;
    }
    return inputDb - dynamicsReductionDb(inputDb, band) + band.makeupDb;
}

// ---- Meters ---------------------------------------------------------------------------------------------------------------

void DynamicsMeters::note(float inputPeak, float outputPeak, const std::array<float, kDynamicsMaxBands>& reductionDb) noexcept {
    raiseTo(m_input, inputPeak);
    raiseTo(m_output, outputPeak);
    for (std::size_t b = 0; b < kDynamicsMaxBands; ++b) {
        raiseTo(m_reduction[b], reductionDb[b]);
    }
    m_blocks.fetch_add(1, std::memory_order_relaxed);
}

DynamicsMeters::Snapshot DynamicsMeters::take() noexcept {
    Snapshot s;
    s.inputPeak = m_input.exchange(0.0f, std::memory_order_relaxed);
    s.outputPeak = m_output.exchange(0.0f, std::memory_order_relaxed);
    for (std::size_t b = 0; b < kDynamicsMaxBands; ++b) {
        s.reductionDb[b] = m_reduction[b].exchange(0.0f, std::memory_order_relaxed);
    }
    s.blocks = m_blocks.load(std::memory_order_relaxed);
    return s;
}

DynamicsMeters::Snapshot DynamicsMeters::peek() const noexcept {
    Snapshot s;
    s.inputPeak = m_input.load(std::memory_order_relaxed);
    s.outputPeak = m_output.load(std::memory_order_relaxed);
    for (std::size_t b = 0; b < kDynamicsMaxBands; ++b) {
        s.reductionDb[b] = m_reduction[b].load(std::memory_order_relaxed);
    }
    s.blocks = m_blocks.load(std::memory_order_relaxed);
    return s;
}

std::shared_ptr<DynamicsMeters> dynamicsMetersFor(std::uint64_t instance) {
    MeterRegistry& r = registry();
    const std::lock_guard lock(r.mutex);
    if (r.meters.size() > 256) {
        std::erase_if(r.meters, [](const auto& entry) { return entry.second.expired(); });
    }
    std::weak_ptr<DynamicsMeters>& slot = r.meters[instance];
    if (auto alive = slot.lock()) {
        return alive;
    }
    auto fresh = std::make_shared<DynamicsMeters>();
    slot = fresh;
    return fresh;
}

std::shared_ptr<DynamicsMeters> findDynamicsMeters(std::uint64_t instance) {
    MeterRegistry& r = registry();
    const std::lock_guard lock(r.mutex);
    const auto it = r.meters.find(instance);
    return it == r.meters.end() ? nullptr : it->second.lock();
}

// ---- The processor --------------------------------------------------------------------------------------------------------

DynamicsProcessing::DynamicsProcessing(int channels, double sampleRate, const DynamicsStructure& structure)
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_rate(sampleRate > 0.0 ? sampleRate : 48000.0),
      m_structure(structure), m_bands(std::clamp(structure.bands, 1, kDynamicsMaxBands)),
      m_envelopeDecay(timeCoefficient(10.0, m_rate)), m_switchCoefficient(timeCoefficient(5.0, m_rate)) {
    m_structure.bands = m_bands;
    const bool hasLfe = m_channels == 6 || m_channels == 8;
    for (int c = 0; c < m_channels; ++c) {
        m_detects[static_cast<std::size_t>(c)] = !(hasLfe && c == 3);
    }
    if (m_structure.limiter) {
        m_limiter = std::make_unique<Limiter>(m_channels, m_rate, 5.0);
        m_latency = m_limiter->latencyFrames();
        m_dryRing.assign(static_cast<std::size_t>(m_latency + 1) * static_cast<std::size_t>(m_channels), 0.0f);
    }
    reset();
}

void DynamicsProcessing::attachMeters(std::shared_ptr<DynamicsMeters> meters) {
    m_meters = std::move(meters);
}

int DynamicsProcessing::settleFrames(const DynamicsSettings& settings, const DynamicsStructure& structure,
                                     double sampleRate) noexcept {
    double longest = 0.0;
    for (int b = 0; b < std::clamp(structure.bands, 1, kDynamicsMaxBands); ++b) {
        longest = std::max(longest, settings.band[static_cast<std::size_t>(b)].releaseMs);
    }
    if (structure.autoGate) {
        longest = std::max(longest, settings.autoGateReleaseMs + 100.0);
    }
    if (structure.limiter) {
        longest = std::max(longest, settings.limiterReleaseMs);
    }
    const auto floorFrames = static_cast<int>(sampleRate / 5.0);
    const auto frames = static_cast<int>(5.0 * longest * sampleRate / 1000.0);
    return std::max(floorFrames, std::min(frames, static_cast<int>(2.0 * sampleRate)));
}

void DynamicsProcessing::reset() noexcept {
    for (Channel& c : m_filters) {
        c = Channel{};
    }
    for (BandState& s : m_state) {
        s = BandState{};
    }
    m_reduction.fill(0.0);
    m_gateEnvelope = 0.0;
    m_gateGain = 0.0;
    m_gateHoldLeft = 0;
    std::fill(m_dryRing.begin(), m_dryRing.end(), 0.0f);
    m_dryPos = 0;
    if (m_limiter) {
        m_limiter->reset();
    }
}

void DynamicsProcessing::setCrossovers(const DynamicsSettings& settings) noexcept {
    double previous = 10.0;
    for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(kDynamicsMaxBands); ++i) {
        const double f = std::max(std::clamp(settings.crossoverHz[i], 10.0, 0.45 * m_rate), previous);
        previous = f;
        if (f == m_frequency[i]) {
            continue;
        }
        m_frequency[i] = f;
        m_k.lowPass[i] = lowPass(m_rate, f, kButterworthQ);
        m_k.highPass[i] = highPass(m_rate, f, kButterworthQ);
        m_k.allPass[i] = allPassSection(m_rate, f, kButterworthQ);
    }
}

void DynamicsProcessing::process(float* interleaved, int frames, const DynamicsSettings& settings, DynamicsRamps ramps) noexcept {
    if (frames <= 0 || interleaved == nullptr) {
        return;
    }
    setCrossovers(settings);
    const auto ch = static_cast<std::size_t>(m_channels);
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, kDynamicsBlock);
        processBlock(interleaved + static_cast<std::size_t>(done) * ch, n, settings, ramps);
        done += n;
    }
}

void DynamicsProcessing::processBlock(float* io, int frames, const DynamicsSettings& settings, DynamicsRamps& ramps) noexcept {
    const auto ch = static_cast<std::size_t>(m_channels);
    const auto bands = static_cast<std::size_t>(m_bands);

    // ---- The block's constants: each band's section, time constants, who is heard ----
    struct Section {
        DynamicsBand cfg;
        bool downward = true;
        double attack = 0.0;
        double release = 0.0;
        double switchTarget = 1.0;   ///< the dynamics (1) or the band untouched (0)
        double audibleTarget = 1.0;  ///< heard (1) or muted by a solo elsewhere (0)
    };
    std::array<Section, kDynamicsMaxBands> sections{};
    bool anySolo = false;
    for (std::size_t b = 0; b < bands; ++b) {
        anySolo = anySolo || settings.band[b].solo;
    }
    for (std::size_t b = 0; b < bands; ++b) {
        Section& s = sections[b];
        s.cfg = settings.band[b];
        s.cfg.mode = m_structure.modes[b];
        s.downward = s.cfg.mode == DynamicsMode::Compress || s.cfg.mode == DynamicsMode::Limit;
        // Expand and Gate: the attack opens the gate (the reduction falls), the release closes it (the reduction rises).
        s.attack = timeCoefficient(s.cfg.attackMs, m_rate);
        s.release = timeCoefficient(s.cfg.releaseMs, m_rate);
        s.switchTarget = s.cfg.bypass ? 0.0 : 1.0;
        s.audibleTarget = !anySolo || s.cfg.solo ? 1.0 : 0.0;
    }
    const double gateOpenStep = 1.0 / std::max(1.0, 2.0 * m_rate / 1000.0);
    const double gateCloseStep = 1.0 / std::max(1.0, settings.autoGateReleaseMs * m_rate / 1000.0);
    const auto gateHold = static_cast<std::int64_t>(std::lround(0.05 * m_rate));
    const double switchRate = 1.0 - m_switchCoefficient;

    // The dry signal of the mix, delayed by the limiter's lookahead (the frames this block starts from).
    const bool delayed = m_latency > 0;
    const bool mixing = ramps.mix != 1.0 || ramps.mixStep != 0.0;
    const double firstPosition = ramps.position; // the cell position of this block's first frame (the mix is applied after the loop)
    std::array<float, kDynamicsBlock * kMaxDspChannels> input{};
    for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * ch; ++i) {
        input[i] = io[i];
    }
    if (delayed) {
        const std::size_t slots = static_cast<std::size_t>(m_latency) + 1;
        for (std::size_t i = 0; i < static_cast<std::size_t>(frames); ++i) {
            float* slot = m_dryRing.data() + m_dryPos * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                slot[c] = input[i * ch + c];
            }
            m_dryPos = (m_dryPos + 1) % slots;
            const float* oldest = m_dryRing.data() + m_dryPos * ch; // the frame written `latency` frames ago
            for (std::size_t c = 0; c < ch; ++c) {
                m_dry[i * ch + c] = oldest[c];
            }
        }
    }

    double inputPeak = 0.0;
    std::array<double, kDynamicsMaxBands> blockReduction{};
    std::array<std::array<double, kMaxDspChannels>, kDynamicsMaxBands> band{};

    for (int i = 0; i < frames; ++i) {
        float* frame = io + static_cast<std::size_t>(i) * ch;
        std::array<double, kMaxDspChannels> x{};
        for (std::size_t c = 0; c < ch; ++c) {
            inputPeak = std::max(inputPeak, static_cast<double>(std::fabs(frame[c])));
            x[c] = frame[c] * (ramps.input + ramps.inputStep * ramps.position);
        }

        // ---- Auto Gate (before the bands; one gain for every channel) ----
        if (m_structure.autoGate) {
            double peak = 0.0;
            for (std::size_t c = 0; c < ch; ++c) {
                if (m_detects[c]) {
                    peak = std::max(peak, std::abs(x[c]));
                }
            }
            m_gateEnvelope = flushed(std::max(peak, m_gateEnvelope * m_envelopeDecay));
            if (gainToDb(m_gateEnvelope) > settings.autoGateThresholdDb) {
                m_gateHoldLeft = gateHold;
                m_gateGain = std::min(1.0, m_gateGain + gateOpenStep);
            } else if (m_gateHoldLeft > 0) {
                --m_gateHoldLeft;
                m_gateGain = std::min(1.0, m_gateGain + gateOpenStep);
            } else {
                m_gateGain = std::max(0.0, m_gateGain - gateCloseStep);
            }
            for (std::size_t c = 0; c < ch; ++c) {
                x[c] *= m_gateGain;
            }
        }

        // ---- The crossovers: the bands of every channel ----
        for (std::size_t c = 0; c < ch; ++c) {
            Channel& f = m_filters[c];
            float rest = static_cast<float>(x[c]);
            for (std::size_t k = 0; k + 1 < bands; ++k) {
                float low = f.lowPass[k][1].process(m_k.lowPass[k], f.lowPass[k][0].process(m_k.lowPass[k], rest));
                for (std::size_t j = k + 1; j + 1 < bands; ++j) {
                    low = f.allPass[k][j].process(m_k.allPass[j], low); // in phase with the bands above
                }
                const float upper = f.highPass[k][1].process(m_k.highPass[k], f.highPass[k][0].process(m_k.highPass[k], rest));
                band[k][c] = low;
                rest = upper;
            }
            band[bands - 1][c] = rest;
        }

        // ---- Each band's gain ----
        std::array<double, kDynamicsMaxBands> gain{};
        for (std::size_t b = 0; b < bands; ++b) {
            BandState& st = m_state[b];
            const Section& s = sections[b];
            st.active += (s.switchTarget - st.active) * switchRate;
            st.audible += (s.audibleTarget - st.audible) * switchRate;
            if (st.active < 1e-5 && s.switchTarget == 0.0) {
                st.active = 0.0;
            }
            if (std::abs(st.audible - s.audibleTarget) < 1e-5) {
                st.audible = s.audibleTarget;
            }
            double g = 1.0;
            if (st.active > 0.0) {
                double peak = 0.0;
                for (std::size_t c = 0; c < ch; ++c) {
                    if (m_detects[c]) {
                        peak = std::max(peak, std::abs(band[b][c]));
                    }
                }
                if (s.downward) {
                    const double reduction = dynamicsReductionDb(gainToDb(peak), s.cfg);
                    st.held = std::max(reduction, s.release * st.held + (1.0 - s.release) * reduction);
                    st.smooth = s.attack * st.smooth + (1.0 - s.attack) * st.held;
                    if (st.held < 1e-9 && st.smooth < 1e-9) {
                        st.held = 0.0;
                        st.smooth = 0.0;
                    }
                } else {
                    st.envelope = flushed(std::max(peak, st.envelope * m_envelopeDecay));
                    const double reduction = dynamicsReductionDb(gainToDb(st.envelope), s.cfg);
                    const double coefficient = reduction > st.smooth ? s.release : s.attack;
                    st.smooth = coefficient * st.smooth + (1.0 - coefficient) * reduction;
                    if (std::abs(st.smooth - reduction) < 1e-9) {
                        st.smooth = reduction;
                    }
                }
                const double processed = dbToGain(s.cfg.makeupDb - st.smooth);
                g = st.active * processed + (1.0 - st.active);
                blockReduction[b] = std::max(blockReduction[b], st.smooth * st.active);
            } else {
                st.smooth = 0.0;
                st.held = 0.0;
            }
            m_reduction[b] = st.smooth * st.active;
            gain[b] = g * st.audible;
        }

        // ---- Sum the bands and apply the output gain ----
        for (std::size_t c = 0; c < ch; ++c) {
            double y = 0.0;
            for (std::size_t b = 0; b < bands; ++b) {
                y += gain[b] * band[b][c];
            }
            frame[c] = static_cast<float>(y * (ramps.output + ramps.outputStep * ramps.position));
        }
        ramps.position += 1.0;
    }

    // ---- The lookahead limiter, then the mix with the dry signal ----
    if (m_limiter) {
        m_limiter->setSettings(settings.limiterCeilingDb, settings.limiterReleaseMs);
        m_limiter->process(io, frames);
    }
    if (mixing) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(frames); ++i) {
            const double mix = std::clamp(ramps.mix + ramps.mixStep * (firstPosition + static_cast<double>(i)), 0.0, 1.0);
            for (std::size_t c = 0; c < ch; ++c) {
                const double dry = delayed ? m_dry[i * ch + c] : input[i * ch + c];
                const double wet = io[i * ch + c];
                io[i * ch + c] = static_cast<float>(dry + mix * (wet - dry));
            }
        }
    }

    if (m_meters) {
        double outputPeak = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * ch; ++i) {
            outputPeak = std::max(outputPeak, static_cast<double>(std::fabs(io[i])));
        }
        std::array<float, kDynamicsMaxBands> reductions{};
        for (std::size_t b = 0; b < kDynamicsMaxBands; ++b) {
            reductions[b] = static_cast<float>(blockReduction[b]);
        }
        m_meters->note(static_cast<float>(inputPeak), static_cast<float>(outputPeak), reductions);
    }
}

} // namespace wabcut::audio::dsp
