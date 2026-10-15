// Accuracy and CPU numbers of the audio DSP, for reports: `wabcut_audio_tests "[.report]"`. Hidden (not part of the
// regular run); the regular tests assert the same properties with fixed tolerances.

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/Compressor.h>
#include <wabcut/audio/dsp/Denoiser.h>
#include <wabcut/audio/dsp/Equalizer.h>
#include <wabcut/audio/dsp/Limiter.h>
#include <wabcut/audio/dsp/Loudness.h>
#include <wabcut/audio/dsp/TimeStretch.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace wabcut::audio;
using namespace wabcut::audio::test;

namespace {

double seconds(const std::function<void()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

double loudnessOf(const std::vector<float>& x, int channels = 2) {
    dsp::LoudnessMeter m(channels, 48000);
    m.add(x.data(), x.size() / static_cast<std::size_t>(channels));
    return m.result().integratedLufs;
}

} // namespace

TEST_CASE("DSP accuracy and cost", "[.report]") {
    std::string out;
    char line[256];
    auto row = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof line, fmt, args...);
        out += line;
        out += '\n';
    };

    // EQ: processed sines vs the analytic response over 20 Hz..20 kHz.
    {
        dsp::Eq3Settings s{150.0, 6.0, 2000.0, -9.0, 1.5, 9000.0, 4.0};
        dsp::Eq3 ref(1, 48000.0);
        ref.setSettings(s);
        double worst = 0.0;
        for (double f = 20.0; f <= 20000.0; f *= 1.26) {
            dsp::Eq3 eq(1, 48000.0);
            eq.setSettings(s);
            const auto in = sine(f, 0.25, 1.5, 1);
            auto y = in;
            eq.process(y.data(), static_cast<int>(y.size()));
            const std::size_t a = 36000;
            const std::size_t b = 72000;
            worst = std::max(worst, std::fabs(ampToDb(rms(y, 1, a, b) / rms(in, 1, a, b)) - ref.magnitudeDb(f)));
        }
        row("EQ (3-band) measured vs analytic response, 20 Hz-20 kHz:  max error %.4f dB", worst);
        row("EQ peaking band gain at f0: %.6f dB (asked 12)", dsp::magnitudeDb(dsp::peaking(48000.0, 1000.0, 12.0, 1.0), 1000.0, 48000.0));
        row("EQ low shelf +6 dB: %.4f dB at 5 Hz, %.4f dB at f0 (half gain), %.4f dB at 5 kHz",
            dsp::magnitudeDb(dsp::lowShelf(48000.0, 100.0, 6.0), 5.0, 48000.0),
            dsp::magnitudeDb(dsp::lowShelf(48000.0, 100.0, 6.0), 100.0, 48000.0),
            dsp::magnitudeDb(dsp::lowShelf(48000.0, 100.0, 6.0), 5000.0, 48000.0));
    }
    // Compressor: steady output vs the static curve.
    {
        dsp::CompressorSettings s;
        s.thresholdDb = -24.0;
        s.ratio = 4.0;
        s.attackMs = 5.0;
        s.releaseMs = 50.0;
        s.makeupDb = 3.0;
        double worst = 0.0;
        for (double level = -60.0; level <= 0.0; level += 1.0) {
            dsp::Compressor c(1, 48000.0);
            c.setSettings(s);
            std::vector<float> x(48000);
            for (std::size_t i = 0; i < x.size(); ++i) {
                x[i] = static_cast<float>((i % 2 == 0 ? 1.0 : -1.0) * dbToAmp(level));
            }
            c.process(x.data(), static_cast<int>(x.size()));
            worst = std::max(worst, std::fabs(ampToDb(std::fabs(x.back())) - (dsp::compressorCurveDb(level, s) + s.makeupDb)));
        }
        row("Compressor steady gain vs static curve, -60..0 dBFS:      max error %.4f dB", worst);
    }
    // Limiter: sample and true peak against the ceiling on hot material.
    {
        for (const double ceiling : {-1.0, -2.0}) {
            dsp::Limiter lim(2, 48000.0, 5.0);
            lim.setSettings(ceiling, 50.0);
            auto x = noise(2.0, 2.0);
            append(x, sine(997.0, dbToAmp(12.0), 2.0));
            append(x, sine(12000.0, dbToAmp(6.0), 2.0, 2, 48000, std::numbers::pi / 4.0));
            lim.process(x.data(), static_cast<int>(x.size() / 2));
            dsp::LoudnessMeter m(2, 48000);
            m.add(x.data(), x.size() / 2);
            row("Limiter ceiling %.1f dBTP on +12 dB material: sample peak %.3f dBFS, true peak %.3f dBTP", ceiling,
                ampToDb(peak(x, 2)), m.result().truePeakDbtp);
        }
    }
    // Loudness: EBU Tech 3341 / 3342 recipes.
    {
        auto tones = [](std::vector<std::pair<double, double>> parts) {
            std::vector<float> x;
            for (const auto& [db, secs] : parts) {
                append(x, sine(1000.0, dbToAmp(db), secs));
            }
            return x;
        };
        row("Tech 3341 case 1 (-23 dBFS):         %.3f LUFS", loudnessOf(tones({{-23.0, 20.0}})));
        row("Tech 3341 case 2 (-33 dBFS):         %.3f LUFS", loudnessOf(tones({{-33.0, 20.0}})));
        row("Tech 3341 case 3 (gating):           %.3f LUFS (expect -23)", loudnessOf(tones({{-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}})));
        row("Tech 3341 case 4 (absolute gate):    %.3f LUFS (expect -23)",
            loudnessOf(tones({{-72.0, 10.0}, {-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}, {-72.0, 10.0}})));
        for (const auto& [a, b, lra] : std::vector<std::tuple<double, double, double>>{{-20, -30, 10}, {-20, -15, 5}, {-40, -20, 20}}) {
            dsp::LoudnessMeter m(2, 48000);
            const auto x = tones({{a, 20.0}, {b, 20.0}});
            m.add(x.data(), x.size() / 2);
            row("Tech 3342 LRA %.0f/%.0f dBFS:          %.3f LU (expect %.0f)", a, b, m.result().rangeLu, lra);
        }
        const auto isp = sine(12000.0, dbToAmp(-6.0), 2.0, 2, 48000, std::numbers::pi / 4.0);
        dsp::LoudnessMeter m(2, 48000);
        m.add(isp.data(), isp.size() / 2);
        row("Inter-sample peak tone (TP -6 dB):   true peak %.3f dBTP, sample peak %.3f dBFS", m.result().truePeakDbtp,
            m.result().samplePeakDbfs);
    }
    // Time stretch.
    {
        for (const double speed : {0.5, 2.0, 4.0}) {
            dsp::TimeStretch ts(2, 48000.0);
            ts.setSpeed(speed);
            const auto x = sine(440.0, 0.5, 8.0);
            std::vector<float> y;
            std::vector<float> buf(2 * 480);
            double pushed = 0.0;
            std::size_t at = 0;
            while (at + 4096 < x.size() / 2) {
                pushed += speed * 480.0;
                const auto n = static_cast<std::size_t>(pushed) - at;
                ts.push(x.data() + at * 2, static_cast<int>(n));
                at += n;
                const int got = ts.pull(buf.data(), 480);
                y.insert(y.end(), buf.begin(), buf.begin() + 2 * got);
            }
            int crossings = 0;
            std::size_t first = 0;
            std::size_t last = 0;
            double step = 0.0;
            for (std::size_t k = 4801; k < y.size() / 2; ++k) {
                if (y[(k - 1) * 2] < 0.0f && y[k * 2] >= 0.0f) {
                    first = crossings == 0 ? k : first;
                    last = k;
                    ++crossings;
                }
                step = std::max(step, static_cast<double>(std::fabs(y[k * 2] - y[(k - 1) * 2])));
            }
            const double hz = 48000.0 * (crossings - 1) / static_cast<double>(last - first);
            const double ratio = static_cast<double>(ts.inputFramesPushed()) / static_cast<double>(ts.outputFramesProduced());
            row("WSOLA %.1fx: pitch %.2f Hz (440), largest step %.3f x the sine's, consumed/produced %.4f (%.1f)", speed, hz,
                step / (2.0 * std::numbers::pi * 440.0 / 48000.0 * 0.5), ratio, speed);
        }
    }
    // CPU: x real time on one core, 48 kHz stereo, 60 s.
    {
        const auto x = noise(0.5, 60.0);
        auto cost = [&](const char* name, const std::function<void(std::vector<float>&)>& fn) {
            auto y = x;
            const double t = seconds([&] { fn(y); });
            row("%-26s %8.0fx real time", name, 60.0 / t);
        };
        cost("EQ (3 bands active)", [](std::vector<float>& y) {
            dsp::Eq3 eq(2, 48000.0);
            eq.setSettings({150.0, 3.0, 2000.0, -4.0, 1.0, 9000.0, 2.0});
            for (std::size_t i = 0; i < y.size() / 2; i += 512) {
                eq.process(y.data() + i * 2, static_cast<int>(std::min<std::size_t>(512, y.size() / 2 - i)));
            }
        });
        cost("Compressor", [](std::vector<float>& y) {
            dsp::Compressor c(2, 48000.0);
            c.process(y.data(), static_cast<int>(y.size() / 2));
        });
        cost("True-peak limiter (5 ms)", [](std::vector<float>& y) {
            dsp::Limiter l(2, 48000.0, 5.0);
            l.process(y.data(), static_cast<int>(y.size() / 2));
        });
        cost("Reduce Noise (RNNoise)", [](std::vector<float>& y) {
            dsp::Denoiser d(2, 48000.0);
            d.process(y.data(), static_cast<int>(y.size() / 2));
        });
        cost("Loudness meter", [](std::vector<float>& y) {
            dsp::LoudnessMeter m(2, 48000);
            m.add(y.data(), y.size() / 2);
            (void)m.result();
        });
        cost("WSOLA 2x (per output s)", [](std::vector<float>& y) {
            dsp::TimeStretch ts(2, 48000.0);
            ts.setSpeed(2.0);
            std::vector<float> buf(2 * 480);
            for (std::size_t at = 0; at + 960 < y.size() / 2; at += 960) {
                ts.push(y.data() + at * 2, 960);
                ts.pull(buf.data(), 480);
            }
        });
    }
    WARN("\n" + out);
    SUCCEED();
}
