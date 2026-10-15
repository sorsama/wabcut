#pragma once

// VoiceOverRecorder: Premiere's Voice-over Record (A.8, docs/ai.md "Voice-over recording"). One recording pass: a
// count-in, playback from the pre-roll, the input written to a new WAV between punch-in and punch-out (or stop()),
// the post-roll, and the take's exact timeline position.
//
//   * Timeline of a pass. The transport starts at punchIn - preroll - countIn seconds. During the count-in the program
//     is muted and a beep sounds on each second (3, 2, 1, when countInBeep); from punchIn - preroll the program plays
//     (the pre-roll). Input whose sound reached the microphone while the transport presented [punchIn, punchOut) is
//     recorded; after punchOut the program plays the post-roll and the transport stops.
//   * Placement. A captured frame's microphone time is its capture time (AudioInput's clock line) less the input
//     latency the device reports and the calibration (the loopback measurement's residual). The transport map gives
//     the timeline position presented (heard) at that host time, the output latency included, on the sample grid;
//     the frames placed in [punchIn, punchOut) are the take, which starts at its first frame's position: to the
//     sample when the latencies are right. (The input and output clocks are taken to run at one rate: a take keeps
//     its length; drift between two devices over a long take is not corrected.)
//   * Real time. The output callback runs the player's source wrapped by the recorder's source (program mute during
//     the count-in, the beeps, input monitoring from AudioInput's monitor ring: the newest frames, the backlog kept to
//     one period so the monitor's latency stays bounded). The capture callback only fills AudioInput's rings. A
//     writer thread (or writeOnce() in tests) moves captured frames to the WavWriter. Nothing on either callback
//     blocks, locks or allocates.
//
// Threading: start(), tick(), stop() and cancel() belong to the thread that drives the player (the GUI thread);
// status() may be read from any thread.

#include <wabcut/playback/audio/TimelineAudioPlayer.h>

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/AudioInput.h>

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::playback {

struct VoiceOverOptions {
    Ticks punchIn;                    ///< where the take starts: the playhead, or the sequence In
    std::optional<Ticks> punchOut;    ///< where it ends: the sequence Out; unset: at stop()
    Ticks preroll{2 * kTicksPerSecond};
    Ticks postroll{kTicksPerSecond};
    int countIn = 3;                  ///< seconds counted before the pre-roll (0..10)
    bool countInBeep = true;
    bool monitor = false;             ///< input monitoring through the output
    double monitorGainDb = 0.0;
    Ticks calibration{};              ///< latency to compensate beyond the device's report (loopback residual)
    std::filesystem::path file;       ///< the take's WAV (created; its folder must exist)
    int bitsPerSample = 24;
    /// Kept verbatim in the take's journal (WavWriter's `<file>.journal`) from start(): what the take is for (the app's
    /// sequence, track and punch-in), so a take a crash interrupted can be put back. Opaque here.
    std::string provenance;
    /// Latency calibration: the pass plays clicks (every kLoopbackClickPeriod from the punch-in) instead of the
    /// program, and stop() measures where they came back (the output wired, or held, to the input).
    bool loopback = false;
};

/// The click interval of a loopback pass.
inline constexpr Ticks kLoopbackClickPeriod{kTicksPerSecond / 4};

/// The round trip measured by a loopback pass.
struct LoopbackResult {
    Ticks reportedOutput{};  ///< the output latency the engine compensates
    Ticks reportedInput{};   ///< the input latency the device reports
    Ticks residual{};        ///< where the clicks were placed, less where they were played: add it to the calibration
    Ticks roundTrip{};       ///< output to input in all: reportedOutput + reportedInput + the calibration + residual
    int clicksFound = 0;
};

enum class VoiceOverState : std::uint8_t {
    Idle,      ///< no pass
    CountIn,   ///< counting 3, 2, 1 (program muted)
    Preroll,   ///< playing before the punch-in
    Recording, ///< the input is being written
    Postroll,  ///< past the punch-out
    Finished,  ///< the transport stopped after the post-roll; stop() collects the take
    Failed,    ///< the file could not be written (status().error says why)
};
std::string_view toString(VoiceOverState state);

struct VoiceOverStatus {
    VoiceOverState state = VoiceOverState::Idle;
    int countInRemaining = 0;          ///< the count shown: 3, 2, 1 (0 outside the count-in)
    Ticks recorded{};                  ///< length written so far
    Ticks monitorLatency{};            ///< input to ear when monitoring (input buffer + backlog + output latency)
    std::uint64_t droppedFrames = 0;
};

struct VoiceOverResult {
    std::filesystem::path file;
    Ticks start{};                     ///< timeline time of the take's first sample
    std::int64_t frames = 0;
    int sampleRate = 48000;
    int channels = 1;
    Ticks duration{};
    Ticks latencyCompensation{};       ///< the input latency plus the calibration it was placed by
    std::uint64_t droppedFrames = 0;   ///< captured audio lost (the writer fell behind; the take has a gap)
    bool punchedOut = false;           ///< the punch-out ended it (rather than stop())
    std::string device;                ///< the input device's name
    std::optional<LoopbackResult> loopback; ///< a loopback pass's measurement
};

class VoiceOverRecorder {
public:
    struct Options {
        bool writerThread = true; ///< false: nothing is written until writeOnce() (deterministic tests)
        std::chrono::milliseconds writerInterval{5};
        /// "Now" for the transport position and the end of a pass (default: the steady clock). Tests that drive a
        /// Null engine on simulated time pass theirs.
        std::function<std::chrono::steady_clock::time_point()> clock;
        /// The transport a pass drives (default: the player's seek, play and pause). The app passes its playback
        /// session's, so the picture follows the pass.
        std::function<void(Ticks)> seek;
        std::function<void()> play;
        std::function<void()> pause;
    };

    /// The engine, the player (started, on the sequence) and the input (running) must outlive the recorder.
    VoiceOverRecorder(audio::AudioEngine& engine, TimelineAudioPlayer& player, audio::AudioInput& input, Options options);
    VoiceOverRecorder(audio::AudioEngine& engine, TimelineAudioPlayer& player, audio::AudioInput& input);
    /// Ends a pass in progress as stop() does: what was recorded stays in its file (with its journal). Only cancel()
    /// deletes a take.
    ~VoiceOverRecorder();

    VoiceOverRecorder(const VoiceOverRecorder&) = delete;
    VoiceOverRecorder& operator=(const VoiceOverRecorder&) = delete;

    /// Starts a pass: creates the file, wraps the engine's source, seeks the player to the count-in and plays.
    /// Errors: InvalidArgument (a pass is running, the punch range is empty, the input is not running), IoError.
    std::expected<void, Error> start(const VoiceOverOptions& options);
    /// Advances the pass (call it at display rate): the states, and the transport stop after the post-roll.
    void tick();
    /// Ends the pass now (or collects a Finished one): the input captured until now is written, the file finished,
    /// the transport paused and the engine's source restored. The file's journal stays: its owner removes it (with the
    /// file) when the take needs no recovery any more. Errors: InvalidArgument (no pass), IoError, and NotFound when
    /// nothing was recorded (stopped before the punch-in: the empty file and its journal are deleted).
    std::expected<VoiceOverResult, Error> stop();
    /// Ends the pass and deletes its file and journal: an explicit cancel only (quitting or closing keeps the take).
    void cancel();
    /// Ends the pass when the engine or the player may already be gone (their owner tore them down first): the writer
    /// stops and the file is closed with what was written, journal included, for a later recovery; the engine, the
    /// player and the transport are not touched (the pass's source is left installed). Use stop() whenever they exist.
    void abandon();

    VoiceOverStatus status() const;
    /// Manual writer mode: one pass of the writer now. Not on an audio thread.
    void writeOnce();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Finds the clicks of a loopback recording: the first sample of each burst at least `threshold` above the noise, in
/// interleaved `samples` (channel 0) at `sampleRate`. Pure; exposed for the measurement and its tests.
std::vector<std::int64_t> findClicks(std::span<const float> samples, int channels, int sampleRate, float threshold = 0.1f);

} // namespace wabcut::playback
