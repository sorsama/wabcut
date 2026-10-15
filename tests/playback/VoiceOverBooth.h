#pragma once

// Test helpers for voice-over recording (A.8): the timeline player on a Null engine and a Null AudioInput fed by a
// simulated room. The room hears the speakers (a loopback: what the output plays) and/or a voice, and the input
// delivers what it heard a known time later. Placement is checked against the engine's own presentation clock: the
// sound of output frame F is heard when ClockSample says F is presented, and reaches the input `inputLatency` later.

#include "PlaybackFixture.h"

#include <wabcut/playback/audio/VoiceOverRecorder.h>

#include <wabcut/audio/AudioInput.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <utility>

namespace wabcut::test {

inline std::filesystem::path voiceOverScratch(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "wabcut-tests" / "voice-over-playback" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

/// Samples of a 32-bit float or 24-bit PCM WAV (interleaved), as floats.
inline std::vector<float> wavSamples(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<std::uint8_t> b{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(b.size() >= 44);
    const int bits = b[34] | (b[35] << 8);
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(b[40]) | (static_cast<std::uint32_t>(b[41]) << 8) |
                                    (static_cast<std::uint32_t>(b[42]) << 16) | (static_cast<std::uint32_t>(b[43]) << 24);
    REQUIRE(44 + dataBytes <= b.size());
    std::vector<float> out;
    const std::size_t step = static_cast<std::size_t>(bits / 8);
    for (std::size_t at = 44; at + step <= 44 + dataBytes; at += step) {
        if (bits == 32) {
            float v = 0.0f;
            std::memcpy(&v, b.data() + at, 4);
            out.push_back(v);
        } else {
            std::int32_t s = b[at] | (b[at + 1] << 8) | (b[at + 2] << 16);
            if ((s & 0x800000) != 0) {
                s -= 0x1000000;
            }
            out.push_back(static_cast<float>(s / 8388607.0));
        }
    }
    return out;
}

class Booth {
public:
    struct Setup {
        int outputLatency = 0;          ///< frames the Null output device reports (and has)
        int reportedInputLatency = 0;   ///< frames the Null input reports
        int actualInputLatency = 0;     ///< frames the input really takes
        bool speakersInRoom = true;     ///< the microphone hears the output (loopback)
        int channels = 1;
        std::function<float(std::int64_t)> voice; ///< also heard: a sound by device frame (optional)
    };

    Booth(Project& project, Setup setup)
        : m_setup(std::move(setup)), rig(project, Rig::Options{Ticks{}, Ticks{}, false, Ticks{}, m_setup.outputLatency}) {
        audio::AudioInputConfig config;
        config.backend = audio::AudioBackend::Null;
        config.channels = m_setup.channels;
        config.nullLatencyFrames = m_setup.reportedInputLatency;
        REQUIRE(input.start(config));
        playback::VoiceOverRecorder::Options options;
        options.writerThread = false;
        options.clock = [this] { return rig.simulatedTime(); };
        recorder = std::make_unique<playback::VoiceOverRecorder>(rig.engine, *rig.player, input, options);
    }

    /// One block (default 10 ms): the output pulled, what the room heard delivered to the input, the writer and the
    /// pass run.
    void step(std::size_t frameCount = 480) {
        const std::int64_t first = rig.m_elapsedFrames;
        const std::vector<float> out = rig.pull(frameCount);
        const std::size_t n = out.size() / 2;
        m_blocks.push_back({first, static_cast<std::int64_t>(n), rig.engine.transportMap().timelineAtFrame(first)});
        const auto ch = static_cast<std::size_t>(m_setup.channels);
        std::vector<float> heard(n * ch, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            const float v = (m_setup.speakersInRoom ? out[2 * i] : 0.0f) +
                            (m_setup.voice ? m_setup.voice(first + static_cast<std::int64_t>(i)) : 0.0f);
            for (std::size_t c = 0; c < ch; ++c) {
                heard[i * ch + c] = v;
            }
        }
        // The block's end is heard when the engine presents frame first + n; the input delivers it the latency later.
        const audio::ClockSample clock = rig.engine.clockSample();
        const double heardEndNs = static_cast<double>(clock.hostTimeNs) +
                                  static_cast<double>(first + static_cast<std::int64_t>(n) - clock.presentedFrame()) * 1e9 / 48000.0;
        const auto deliveredNs = static_cast<std::int64_t>(std::llround(heardEndNs + m_setup.actualInputLatency * 1e9 / 48000.0));
        REQUIRE(input.push(heard, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deliveredNs))));
        recorder->writeOnce();
        recorder->tick();
    }
    void run(double milliseconds) {
        for (int i = 0; i < static_cast<int>(std::llround(milliseconds / 10.0)); ++i) {
            step();
        }
    }
    /// Steps until the pass is in `state` (at most `seconds` of audio). True when it got there.
    bool runUntil(playback::VoiceOverState state, double seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 100.0); ++i) {
            if (recorder->status().state == state) {
                return true;
            }
            step();
        }
        return recorder->status().state == state;
    }

    /// The output stream's frame that rendered timeline position `t` (while playing at normal speed), or -1.
    std::int64_t frameOf(Ticks t) const {
        for (const Block& b : m_blocks) {
            if (t >= b.timeline && t < b.timeline + frames(b.frames)) {
                return b.first + (t - b.timeline).value / kTicksPerFrame;
            }
        }
        return -1;
    }

    Setup m_setup;
    Rig rig;
    audio::AudioInput input;
    std::unique_ptr<playback::VoiceOverRecorder> recorder;
    struct Block {
        std::int64_t first = 0;  ///< device frame
        std::int64_t frames = 0;
        Ticks timeline;          ///< rendered at `first`
    };
    std::vector<Block> m_blocks;
};

} // namespace wabcut::test
