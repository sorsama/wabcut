// Streamer voice and Game under voice (A.10) measured on the real render path: OfflineMixdown stems and the media-backed
// loudness and speech analysis, on a synthetic OBS-like recording. A "Mic" track carries commentary in phrases over a
// fan's noise floor, with "s" sounds; a "Game" track carries a loud bed with explosions. audio.autoDuck "gameUnderVoice"
// ducks the game under the voice; audio.essentialSound "streamerVoice" on the mic track lowers its noise floor (Reduce
// Noise + gate) and its sibilance (de-esser) and keeps its true peak under the limiter's ceiling. The numbers are printed
// ("[streamer] ...") for docs/ai.md (MEASURED-STREAMER).
//
// The intelligibility proxy is the mic-to-game ratio: the mean power of the mic stem against the game stem inside the
// sentences, broadband and in the voice band (300 Hz - 4 kHz), each stem rendered on its own.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/dsp/Biquad.h>
#include <wabcut/audio/dsp/Loudness.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/audio/AudioFeed.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <initializer_list>
#include <map>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

constexpr int kRate = 48000;
constexpr double kSeconds = 24.0;

/// The sample index of a time (every fixture time is on the 10 ms grid the speech detector measures on).
std::int64_t sampleAt(double seconds) { return std::llround(seconds * kRate); }

struct Span {
    double from;
    double to;
};

/// The commentary: three phrases of two or three sentences. The pauses inside a phrase (0.55-0.8 s) are shorter than
/// the duck's attack + hold + release (1.27 s), so the game stays down through them; between the phrases (3.5 s and
/// more) it comes back up.
const std::vector<std::vector<Span>> kPhrases{
    {{2.00, 2.90}, {3.50, 4.70}, {5.30, 5.95}},
    {{9.50, 10.90}, {11.60, 12.40}, {13.20, 14.60}},
    {{18.40, 19.40}, {19.95, 21.10}},
};
/// "s" sounds, inside sentences.
const std::vector<Span> kSibilants{{2.50, 2.62}, {4.10, 4.22}, {10.30, 10.42}, {13.80, 13.92}, {20.40, 20.52}};

constexpr double kDuckDb = -10.0;            // gameUnderVoiceDucking()
constexpr double kHoldPlusRelease = 1.15;    // 0.35 s + 0.8 s
constexpr double kAttack = 0.12;

/// Uniform noise in [-1, 1): a hash of the sample index (splitmix64's finaliser), the same on every run.
double hashNoise(std::uint64_t seed, std::int64_t n) {
    std::uint64_t z = static_cast<std::uint64_t>(n) + 0x9E3779B97F4A7C15ULL * (seed + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) / 4503599627370496.0 - 1.0; // 53 bits over 2^52
}

/// 1 inside [from, to) with raised-cosine edges of `ramp` samples, 0 outside.
double edge(std::int64_t n, std::int64_t from, std::int64_t to, std::int64_t ramp) {
    if (n < from || n >= to) {
        return 0.0;
    }
    const std::int64_t d = std::min(n - from, to - 1 - n);
    return d >= ramp ? 1.0 : 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(d) / static_cast<double>(ramp));
}

using SampleSpans = std::vector<std::pair<std::int64_t, std::int64_t>>;

/// Scales `x` so its RMS over `spans` is `dbfs`.
void scaleTo(std::vector<double>& x, const SampleSpans& spans, double dbfs) {
    double sum = 0.0;
    std::int64_t count = 0;
    for (const auto& [a, b] : spans) {
        for (std::int64_t i = a; i < b; ++i) {
            sum += x[static_cast<std::size_t>(i)] * x[static_cast<std::size_t>(i)];
            ++count;
        }
    }
    const double rms = count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
    const double gain = rms > 0.0 ? std::pow(10.0, dbfs / 20.0) / rms : 0.0;
    for (double& v : x) {
        v *= gain;
    }
}

/// Mixes the parts into a float signal with its peak at most 0.8 (under the mixer's soft clip above -1 dBFS).
std::vector<float> mixed(const std::vector<std::vector<double>>& parts) {
    const std::size_t n = parts.front().size();
    std::vector<double> sum(n, 0.0);
    double peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        for (const std::vector<double>& p : parts) {
            sum[i] += p[i];
        }
        peak = std::max(peak, std::fabs(sum[i]));
    }
    const double scale = peak > 0.8 ? 0.8 / peak : 1.0;
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(sum[i] * scale);
    }
    return out;
}

/// The streamer's mic, mono. Voiced speech: a glottal pulse train gliding around 140 Hz through formants at 700 and
/// 1200 Hz, amplitude-modulated at 4.5 Hz like syllables (starting on one), -20 dBFS RMS in the sentences. "s" sounds: 40
/// sines between 5 and 8 kHz with random phases, -16 dBFS RMS. A fan's noise floor all the time: white noise with a low
/// tilt, -45 dBFS RMS.
std::vector<float> micRecording() {
    const std::int64_t total = sampleAt(kSeconds);
    const auto n = static_cast<std::size_t>(total);
    SampleSpans sentences;
    for (const std::vector<Span>& phrase : kPhrases) {
        for (const Span& s : phrase) {
            sentences.emplace_back(sampleAt(s.from), sampleAt(s.to));
        }
    }
    std::vector<double> voice(n, 0.0);
    const double r = 0.995;
    const double c1 = 2.0 * r * std::cos(2.0 * std::numbers::pi * 700.0 / kRate);
    const double c2 = 2.0 * r * std::cos(2.0 * std::numbers::pi * 1200.0 / kRate);
    double glottis = 0.0;
    double f1 = 0.0;
    double f2 = 0.0;
    double v1 = 0.0;
    double v2 = 0.0;
    const std::int64_t ramp = sampleAt(0.005);
    for (std::int64_t i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / kRate;
        glottis += (140.0 + 25.0 * std::sin(2.0 * std::numbers::pi * 0.7 * t)) / kRate;
        const double pulse = glottis >= 1.0 ? 1.0 : 0.0;
        glottis -= std::floor(glottis);
        const double y1 = pulse + c1 * f1 - r * r * v1;
        v1 = f1;
        f1 = y1;
        const double y2 = pulse + c2 * f2 - r * r * v2;
        v2 = f2;
        f2 = y2;
        double envelope = 0.0;
        for (const auto& [a, b] : sentences) {
            if (i >= a && i < b) {
                const double syllable = std::cos(std::numbers::pi * 4.5 * static_cast<double>(i - a) / kRate);
                envelope = edge(i, a, b, ramp) * (0.25 + 0.75 * syllable * syllable);
            }
        }
        voice[static_cast<std::size_t>(i)] = (y1 + 0.6 * y2) * envelope;
    }
    scaleTo(voice, sentences, -20.0);

    SampleSpans esses;
    for (const Span& s : kSibilants) {
        esses.emplace_back(sampleAt(s.from), sampleAt(s.to));
    }
    std::vector<double> sibilance(n, 0.0);
    constexpr std::size_t kSines = 40;
    std::array<double, kSines> frequency{};
    std::array<double, kSines> phase{};
    for (std::size_t k = 0; k < kSines; ++k) {
        const auto sine = static_cast<std::int64_t>(k);
        const double slot = (static_cast<double>(k) + 0.5 + 0.4 * hashNoise(5, sine)) / static_cast<double>(kSines);
        frequency[k] = 5000.0 + 3000.0 * slot;
        phase[k] = std::numbers::pi * (hashNoise(3, sine) + 1.0);
    }
    for (const auto& [a, b] : esses) {
        for (std::int64_t i = a; i < b; ++i) {
            double v = 0.0;
            for (std::size_t k = 0; k < kSines; ++k) {
                v += std::sin(2.0 * std::numbers::pi * frequency[k] * static_cast<double>(i - a) / kRate + phase[k]);
            }
            sibilance[static_cast<std::size_t>(i)] = v * edge(i, a, b, sampleAt(0.01));
        }
    }
    scaleTo(sibilance, esses, -16.0);

    std::vector<double> fan(n, 0.0);
    double low = 0.0;
    for (std::int64_t i = 0; i < total; ++i) {
        const double white = hashNoise(7, i);
        low = 0.9 * low + 0.1 * white;
        fan[static_cast<std::size_t>(i)] = 0.5 * white + 3.0 * low;
    }
    scaleTo(fan, SampleSpans{{0, total}}, -45.0);
    return mixed({voice, sibilance, fan});
}

/// The game, mono: a continuous bed (noise and three tones, about -19 dBFS RMS) and an explosion (a low rumble decaying
/// over a second) every 4.3 s from 3.1 s.
std::vector<float> gameRecording() {
    const std::int64_t total = sampleAt(kSeconds);
    std::vector<double> game(static_cast<std::size_t>(total), 0.0);
    double boom = 0.0;
    for (std::int64_t i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double w = 2.0 * std::numbers::pi * t;
        double v = 0.18 * hashNoise(11, i) + 0.05 * std::sin(110.0 * w) + 0.04 * std::sin(440.0 * w) + 0.03 * std::sin(1700.0 * w);
        boom = 0.97 * boom + 0.03 * hashNoise(13, i);
        for (int k = 0; k < 5; ++k) {
            const double at = 3.1 + 4.3 * k;
            if (t >= at && t < at + 1.5) {
                v += 2.5 * boom * std::exp(-(t - at) / 0.35);
            }
        }
        game[static_cast<std::size_t>(i)] = v;
    }
    return mixed({game});
}

/// Media as mono sample buffers at 48 kHz, played on every channel.
class BufferFeed final : public playback::AudioFeed {
public:
    void set(MediaId media, std::vector<float> samples) { m_media[media.value] = std::move(samples); }

    std::expected<std::size_t, Error> read(MediaId media, int, Ticks sourceTime, std::span<float> interleaved, int channels,
                                           Rate rate) override {
        const auto it = m_media.find(media.value);
        if (it == m_media.end() || channels < 1 || !(rate == Rate{kRate, 1})) {
            return std::unexpected(Error{ErrorCode::NotFound, "no such recording at this rate"});
        }
        const std::vector<float>& s = it->second;
        const auto ch = static_cast<std::size_t>(channels);
        const std::size_t frames = interleaved.size() / ch;
        const std::int64_t first = ticksToSamples(sourceTime, rate);
        for (std::size_t f = 0; f < frames; ++f) {
            const std::int64_t i = first + static_cast<std::int64_t>(f);
            const float v = i >= 0 && i < static_cast<std::int64_t>(s.size()) ? s[static_cast<std::size_t>(i)] : 0.0f;
            for (std::size_t c = 0; c < ch; ++c) {
                interleaved[f * ch + c] = v;
            }
        }
        return frames;
    }

private:
    std::map<std::uint64_t, std::vector<float>> m_media;
};

/// A1 "Mic" (dialogue role) and A2 "Game" (effects role), 24 s each: the tracks a recording import makes of an OBS
/// file's microphone and game streams.
Timeline recordingTimeline() {
    Timeline tl("A1 dialogue |V" + std::string(599, 'v') + "|\nA2 effects |G" + std::string(599, 'g') + "|\n");
    const std::initializer_list<std::pair<const char*, const char*>> names{{"A1", "Mic"}, {"A2", "Game"}};
    for (const auto& [label, name] : names) {
        edit::TrackStateChange change;
        change.name = std::string(name);
        tl.state = test::ok(edit::setTrackState(tl.state, tl.seq, tl.track(label), change)).state;
    }
    return tl;
}

/// One track rendered on its own (its clips and effects, no master), stereo, the whole fixture.
std::vector<float> stem(const ProjectState& state, SequenceId seq, TrackId track, playback::AudioFeed& feed) {
    playback::OfflineMixdownOptions options;
    options.tracks = std::vector<TrackId>{track};
    options.honourMuteSolo = false;
    options.masterEffects = false;
    playback::OfflineMixdown mix(state.sequencePtr(seq), 2, kRate, Ticks{}, feed, options);
    const auto frames = static_cast<std::size_t>(sampleAt(kSeconds));
    std::vector<float> out(frames * 2);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min<std::size_t>(4800, frames - done);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

/// Mean power (dBFS) of an interleaved signal over the windows, all channels.
double levelDb(const std::vector<float>& x, int channels, const std::vector<Span>& windows) {
    const auto ch = static_cast<std::size_t>(channels);
    double sum = 0.0;
    std::size_t count = 0;
    for (const Span& w : windows) {
        const std::size_t a = static_cast<std::size_t>(sampleAt(w.from)) * ch;
        const std::size_t b = std::min(static_cast<std::size_t>(sampleAt(w.to)) * ch, x.size());
        for (std::size_t i = a; i < b; ++i) {
            sum += static_cast<double>(x[i]) * x[i];
            ++count;
        }
    }
    return count > 0 && sum > 0.0 ? 10.0 * std::log10(sum / static_cast<double>(count)) : -200.0;
}

/// Channel 0 of a stereo stem through a 4th-order Butterworth band pass, mono.
std::vector<float> band(const std::vector<float>& stereo, double low, double high) {
    namespace dsp = audio::dsp;
    const std::array<dsp::BiquadCoefficients, 4> c{
        dsp::highPass(kRate, low, dsp::butterworthQ(2, 0)), dsp::highPass(kRate, low, dsp::butterworthQ(2, 1)),
        dsp::lowPass(kRate, high, dsp::butterworthQ(2, 0)), dsp::lowPass(kRate, high, dsp::butterworthQ(2, 1))};
    std::array<dsp::BiquadState, 4> state{};
    std::vector<float> out(stereo.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        float v = stereo[2 * i];
        for (std::size_t k = 0; k < c.size(); ++k) {
            v = state[k].process(c[k], v);
        }
        out[i] = v;
    }
    return out;
}

audio::dsp::LoudnessResult loudnessOf(const std::vector<float>& stereo) {
    audio::dsp::LoudnessMeter meter(2, kRate);
    meter.add(stereo.data(), stereo.size() / 2);
    return meter.result();
}

/// The duck's gain (dB) over [t, t + 5 ms): the ducked game stem against the plain one (the same samples, scaled).
double duckAt(const std::vector<float>& ducked, const std::vector<float>& plain, double t) {
    const std::vector<Span> w{{t, t + 0.005}};
    return levelDb(ducked, 2, w) - levelDb(plain, 2, w);
}

struct DuckTiming {
    double down = 1e9;  ///< when the game is first fully down (within 0.1 dB), from the phrase's first word, s
    double up = 1e9;    ///< when it is back up (within 0.1 dB), from the phrase's last word + hold + release, s
    bool earlyUp = true;    ///< it was still up just before the attack
    bool heldDown = false;  ///< it stayed fully down from the first word to the last word + hold
};

DuckTiming timingOf(const std::vector<float>& ducked, const std::vector<float>& plain, const std::vector<Span>& phrase) {
    const double start = phrase.front().from;
    const double end = phrase.back().to;
    DuckTiming t;
    for (int k = -60; k < 60; ++k) {
        const double at = start + 0.005 * k;
        if (duckAt(ducked, plain, at) <= kDuckDb + 0.1) {
            t.down = at - start;
            break;
        }
    }
    for (int k = 0; k < 500; ++k) {
        const double at = end + 0.005 * k;
        if (duckAt(ducked, plain, at) >= -0.1) {
            t.up = at - (end + kHoldPlusRelease);
            break;
        }
    }
    t.earlyUp = duckAt(ducked, plain, start - kAttack - 0.025) >= -0.1;
    double highest = -1e9;
    for (double at = start + 0.02; at < end + 0.33; at += 0.005) {
        highest = std::max(highest, duckAt(ducked, plain, at));
    }
    t.heldDown = highest <= kDuckDb + 0.1;
    return t;
}

struct Rig {
    Rig() {
        feed->set(tl.media('V'), micRecording());
        feed->set(tl.media('G'), gameRecording());
    }
    std::shared_ptr<AudioServices> services() const {
        const std::shared_ptr<BufferFeed> f = feed;
        const FeedFactory feeds = [f](const ProjectState&) -> std::shared_ptr<playback::AudioFeed> { return f; };
        return std::make_shared<AudioServices>(std::make_shared<RenderLoudnessAnalyzer>(feeds),
                                               std::make_shared<RenderSpeechDetector>(feeds, nullptr));
    }
    Timeline tl = recordingTimeline();
    std::shared_ptr<BufferFeed> feed = std::make_shared<BufferFeed>();
};

} // namespace

TEST_CASE("Streamer voice and Game under voice on an OBS-like recording: duck depth and timing; floor; sibilance; true peak",
          "[audio][edit][analysis][streamer]") {
    Rig rig;
    CommandHarness h(rig.tl, command::EnginePolicy{command::CapabilitySet::all(), 16, rig.services()},
                     ai::edit::editingCommands());
    const SequenceId seq = rig.tl.seq;
    const TrackId micTrack = rig.tl.track("A1");
    const TrackId gameTrack = rig.tl.track("A2");
    const std::vector<float> rawMic = stem(*h.state(), seq, micTrack, *rig.feed);
    const std::vector<float> plainGame = stem(*h.state(), seq, gameTrack, *rig.feed);

    // The windows measured: the sentences (30 ms in from their edges), the "s" sounds, the pauses between phrases (from
    // 0.6 s after a phrase: the gate has closed), and the game where no duck reaches (after the release, with room for
    // the later release of the second run).
    std::vector<Span> sentences;
    for (const std::vector<Span>& phrase : kPhrases) {
        for (const Span& s : phrase) {
            sentences.push_back({s.from + 0.03, s.to - 0.03});
        }
    }
    std::vector<Span> esses;
    for (const Span& s : kSibilants) {
        esses.push_back({s.from + 0.01, s.to - 0.01});
    }
    const std::vector<Span> pauses{{1.0, 1.9}, {6.55, 9.40}, {15.20, 18.30}, {21.70, 23.80}};
    const std::vector<Span> gameUp{{0.5, 1.75}, {7.80, 9.25}, {16.45, 18.15}, {22.95, 23.90}};

    // 1. Game under voice on the raw recording: the mic is the speech, the game ducks.
    std::size_t undo = h.session.undoCount();
    const auto duck = h.ok("audio.autoDuck", {{"preset", "gameUnderVoice"}});
    CHECK(h.session.undoCount() == undo + 1);
    CHECK(duck.data["dialogue"] == Json::array({Json{{"track", "A1"}, {"name", "Mic"}}}));
    CHECK(duck.data["music"] == Json::array({Json{{"track", "A2"}, {"name", "Game"}}}));
    REQUIRE(duck.data["regions"].size() == kPhrases.size());
    for (std::size_t k = 0; k < kPhrases.size(); ++k) {
        const Json& speech = duck.data["regions"][k]["ticks"]["speech"];
        const double from = static_cast<double>(speech[0].get<std::int64_t>()) / static_cast<double>(kTicksPerSecond);
        const double to = static_cast<double>(speech[1].get<std::int64_t>()) / static_cast<double>(kTicksPerSecond);
        INFO("phrase " << k << ": speech found " << from << " - " << to << " s");
        CHECK(std::fabs(from - kPhrases[k].front().from) <= 0.021);
        CHECK(std::fabs(to - kPhrases[k].back().to) <= 0.021);
    }
    const std::vector<float> duckedGame = stem(*h.state(), seq, gameTrack, *rig.feed);

    // 2. Streamer voice on the mic track, in one request.
    undo = h.session.undoCount();
    const auto voice =
        h.ok("audio.essentialSound", {{"tracks", Json::array({"A1"})}, {"type", "dialogue"}, {"preset", "streamerVoice"}});
    CHECK(h.session.undoCount() == undo + 1);
    INFO(voice.data["clips"][0]["loudness"].dump());
    const ProjectSnapshot processed = h.state();
    const std::vector<float> mic = stem(*processed, seq, micTrack, *rig.feed);
    // The same mic with sections bypassed, to tell what they do.
    const ClipId micClip = h.clip("A1@0");
    const auto bypassed = [&](std::initializer_list<std::string_view> sections) {
        ProjectState state = *processed;
        for (const std::string_view section : sections) {
            EffectInstanceId id;
            for (const EffectInstance& e : state.findSequence(seq)->findClip(micClip)->clip().effects()) {
                if (const EffectData* d = e.findData(kSectionKey); d != nullptr && *d->text() == section) {
                    id = e.id;
                }
            }
            REQUIRE(id.isValid());
            edit::EffectChange off;
            off.enabled = false;
            state = test::ok(edit::setEffect(state, seq, micClip, id, off)).state;
        }
        return stem(state, seq, micTrack, *rig.feed);
    };
    const std::vector<float> micNoDeess = bypassed({"repair.deess"});
    const std::vector<float> micNoRepair = bypassed({"repair.noise", "repair.gate"});

    // Loudness and true peak: matched to -16 LUFS, under the limiter's -1 dBTP.
    const audio::dsp::LoudnessResult before = loudnessOf(rawMic);
    const audio::dsp::LoudnessResult after = loudnessOf(mic);
    CHECK(after.integratedLufs == Approx(-16.0).margin(1.0));
    CHECK(after.truePeakDbtp <= -1.0 + 0.5);
    std::printf("[streamer] mic: %.1f LUFS, true peak %.2f dBTP -> streamerVoice %.1f LUFS (target -16), true peak %.2f dBTP "
                "(ceiling -1); loudness Gain %s dB in %s passes\n",
                before.integratedLufs, before.truePeakDbtp, after.integratedLufs, after.truePeakDbtp,
                voice.data["clips"][0]["loudness"]["gainDb"].dump().c_str(),
                voice.data["clips"][0]["loudness"]["passes"].dump().c_str());

    // The noise floor in the pauses: Reduce Noise and the gate take at least 10 dB off it, and it is at least 10 dB further
    // under the voice than in the recording.
    const double floorRaw = levelDb(rawMic, 2, pauses);
    const double floorAfter = levelDb(mic, 2, pauses);
    const double floorUnrepaired = levelDb(micNoRepair, 2, pauses);
    const double voiceRaw = levelDb(rawMic, 2, sentences);
    const double voiceAfter = levelDb(mic, 2, sentences);
    CHECK(floorAfter <= floorUnrepaired - 10.0);
    CHECK((voiceAfter - floorAfter) - (voiceRaw - floorRaw) >= 10.0);
    std::printf("[streamer] mic noise floor in the pauses: %.1f dBFS -> %.1f dBFS (Reduce Noise + gate: %.1f dB); voice over "
                "floor %.1f dB -> %.1f dB\n",
                floorRaw, floorAfter, floorAfter - floorUnrepaired, voiceRaw - floorRaw, voiceAfter - floorAfter);

    // Sibilance (5-8 kHz in the "s" sounds): the chain lowers it against the voice band. On these synthetic s-sounds
    // Reduce Noise (RNNoise, before the de-esser) already takes most of them, so the de-esser alone only must not add
    // any (measured: -0.2 dB alone, 1.4 dB less sibilance against the voice through the chain).
    const std::vector<float> rawVoiceBand = band(rawMic, 300.0, 4000.0);
    const std::vector<float> voiceBand = band(mic, 300.0, 4000.0);
    const double essRaw = levelDb(band(rawMic, 5000.0, 8000.0), 1, esses);
    const double ess = levelDb(band(mic, 5000.0, 8000.0), 1, esses);
    const double essUndeessed = levelDb(band(micNoDeess, 5000.0, 8000.0), 1, esses);
    CHECK(ess <= essUndeessed + 0.1);
    CHECK(ess - levelDb(voiceBand, 1, sentences) <= essRaw - levelDb(rawVoiceBand, 1, sentences) - 1.0);
    std::printf("[streamer] sibilance 5-8 kHz in the s sounds: de-esser %.1f dB; against the voice band %.1f dB -> %.1f dB\n",
                ess - essUndeessed, essRaw - levelDb(rawVoiceBand, 1, sentences), ess - levelDb(voiceBand, 1, sentences));

    // The mic-to-game ratio in the sentences: the duck adds at least 8 dB, broadband and in the voice band. Where no
    // duck reaches, the game keeps its level.
    const double gamePlain = levelDb(plainGame, 2, sentences);
    const double gameDucked = levelDb(duckedGame, 2, sentences);
    const std::vector<float> plainGameBand = band(plainGame, 300.0, 4000.0);
    const std::vector<float> duckedGameBand = band(duckedGame, 300.0, 4000.0);
    const double gameBandPlain = levelDb(plainGameBand, 1, sentences);
    const double gameBandDucked = levelDb(duckedGameBand, 1, sentences);
    CHECK(gamePlain - gameDucked >= 8.0);
    CHECK(gameBandPlain - gameBandDucked >= 8.0);
    const double outside = levelDb(duckedGame, 2, gameUp) - levelDb(plainGame, 2, gameUp);
    CHECK(std::fabs(outside) <= 0.5);
    std::printf("[streamer] mic-to-game in the sentences: recording %.1f dB, streamerVoice %.1f dB, + ducked game %.1f dB "
                "(duck %.1f dB); voice band %.1f -> %.1f dB; game outside the ducks %+.2f dB\n",
                voiceRaw - gamePlain, voiceAfter - gamePlain, voiceAfter - gameDucked, gameDucked - gamePlain,
                levelDb(rawVoiceBand, 1, sentences) - gameBandPlain, levelDb(voiceBand, 1, sentences) - gameBandDucked, outside);

    // The duck's timing, speech found on the recording: the game is fully down by the first word (within one 10 ms
    // detector frame), stays down through the phrase, and is back up hold + release after the last word.
    for (std::size_t k = 0; k < kPhrases.size(); ++k) {
        const DuckTiming t = timingOf(duckedGame, plainGame, kPhrases[k]);
        INFO("phrase " << k << ": down " << t.down << " s, up " << t.up << " s");
        CHECK(t.down >= -0.02);
        CHECK(t.down <= 0.02);
        CHECK(std::fabs(t.up) <= 0.03);
        CHECK(t.earlyUp);
        CHECK(t.heldDown);
        std::printf("[streamer] duck, speech found on the recording, phrase %zu: fully down %+.0f ms from the first word, back up "
                    "%+.0f ms from the last word + hold + release\n",
                    k + 1, t.down * 1000.0, t.up * 1000.0);
    }

    // 3. Game under voice again on the processed mic (the order a streamer works in): its speech is found the same at
    //    the start; at the end the detector hears the gate's release, so the game comes back a little later. A clip's
    //    effects start from rest at the clip's start (no pre-roll over its source before it), so the gate is open for
    //    its first fraction of a second there and the detector may take that floor for speech: at most one short region
    //    at the clip's start beside the phrases.
    const auto again = h.ok("audio.autoDuck", {{"preset", "gameUnderVoice"}});
    std::size_t atClipStart = 0;
    for (const Json& region : again.data["regions"]) {
        if (region["ticks"]["speech"][0].get<std::int64_t>() < Ticks::fromSeconds(1).value) {
            ++atClipStart;
            CHECK(region["ticks"]["speech"][1].get<std::int64_t>() < Ticks::fromSeconds(1).value);
        }
    }
    CHECK(atClipStart <= 1);
    REQUIRE(again.data["regions"].size() == kPhrases.size() + atClipStart);
    std::printf("[streamer] speech found on the streamerVoice mic: %zu region(s) at the clip's start besides the phrases\n",
                atClipStart);
    const std::vector<float> duckedAgain = stem(*h.state(), seq, gameTrack, *rig.feed);
    for (std::size_t k = 0; k < kPhrases.size(); ++k) {
        const DuckTiming t = timingOf(duckedAgain, plainGame, kPhrases[k]);
        INFO("phrase " << k << ": down " << t.down << " s, up " << t.up << " s");
        CHECK(t.down >= -0.02);
        CHECK(t.down <= 0.02);
        CHECK(t.up >= -0.03);
        CHECK(t.up <= 0.6);
        CHECK(t.earlyUp);
        CHECK(t.heldDown);
        std::printf("[streamer] duck, speech found on the streamerVoice mic, phrase %zu: fully down %+.0f ms, back up %+.0f ms\n",
                    k + 1, t.down * 1000.0, t.up * 1000.0);
    }
    CHECK(levelDb(duckedAgain, 2, sentences) <= gamePlain - 8.0);
    const std::vector<Span> gameUpAfterIntro(gameUp.begin() + 1, gameUp.end());
    CHECK(std::fabs(levelDb(duckedAgain, 2, gameUpAfterIntro) - levelDb(plainGame, 2, gameUpAfterIntro)) <= 0.5);
    requireValid(*h.state());
}
