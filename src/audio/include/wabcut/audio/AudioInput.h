#pragma once

// AudioInput: an audio capture device (A.8, voice-over recording; docs/ai.md "Voice-over recording").
//
// The device's callback (or push() on the Null backend) only copies captured frames into lock-free rings and updates
// atomics, so it never blocks:
//   * the capture ring, drained by one consumer (the recorder's writer thread) in capture order, every frame counted:
//     a frame that does not fit is dropped and counted (framesDropped), never waited for;
//   * the monitor ring, drained by the audio output callback (input monitoring), which keeps only the newest frames;
//   * the input meters (peak and RMS since the last read), and the capture clock line.
//
// Capture clock. Frame f of the stream is the f-th frame captured since start(). Each callback delivers a block that
// ends "now"; the earliest such arrival over the session fixes the line hostTimeOf(f) = f / rate + offset (a callback
// can be late, never early, so the minimum filters its jitter). The sound of frame f reached the input latency()
// earlier than hostTimeOf(f) (the converter and the device buffer); the recorder subtracts that and its calibration.
//
// Privacy: an AudioInput only runs between start() and stop(), which the app calls on an explicit user action (arming
// a track for recording, the input meter in Preferences); it shows a recording indicator while one is running.
//
// Threading: start()/stop() and the device queries belong to one control thread. read()/readable() belong to one
// consumer thread, readMonitor() to one other (the output callback). levels(), stats(), captureLine() and latency()
// may be called from any thread. Nothing on the capture path locks or allocates. readMonitor() may run while the input
// is stopped and started again (a device switch during a pass): start() frees the monitor ring it replaces only once a
// readMonitor() call that may be reading it is over (it waits that long; readMonitor() never waits).

#include <wabcut/audio/AudioEngine.h>

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wabcut::audio {

struct AudioInputConfig {
    int sampleRate = 48000;
    int channels = 1;        ///< 1 (a microphone) or 2
    int periodFrames = 256;  ///< requested callback size; backends may round it
    /// Default: the system's capture API (WASAPI, Core Audio, PulseAudio / ALSA / JACK). Null: no device and no
    /// thread; the owner delivers captured audio with push() (tests, synthetic inputs). NullRealtime is not supported.
    AudioBackend backend = AudioBackend::Default;
    /// Input device, AudioDeviceInfo::id from enumerateInputDevices(); empty = the system default.
    std::string deviceId;
    double ringSeconds = 8.0;        ///< capture ring capacity (the writer may fall this far behind)
    double monitorRingSeconds = 0.25;
    /// Null backend only: the input latency the device reports, in frames.
    int nullLatencyFrames = 0;
};

/// The capture latency: from the sound reaching the converter to its frames being delivered.
struct InputLatency {
    std::int64_t bufferFrames = 0; ///< the device buffer (internal period x periods), or the Null configuration
    std::int64_t periodFrames = 0; ///< granted period
    Ticks total{};                 ///< bufferFrames as time
};

/// The capture clock line: frame f was delivered at hostTimeNs(f) (steady clock, nanoseconds).
struct CaptureLine {
    bool valid = false;            ///< false until the first block
    std::int64_t offsetNs = 0;     ///< host time of frame 0
    std::int64_t frames = 0;       ///< frames captured so far
    int sampleRate = 48000;
    std::int64_t hostTimeNs(std::int64_t frame) const noexcept {
        return offsetNs + static_cast<std::int64_t>(static_cast<double>(frame) * 1e9 / static_cast<double>(sampleRate));
    }
};

/// Input level since the previous read (per channel; channel 1 equals 0 on a mono input).
struct InputLevels {
    float peak[2]{};
    float rms[2]{};
    bool clipped = false; ///< a sample reached full scale
};

struct AudioInputStats {
    std::uint64_t callbacks = 0;
    std::uint64_t framesCaptured = 0;
    std::uint64_t framesDropped = 0;        ///< capture ring full: the consumer fell behind
    std::uint64_t monitorFramesDropped = 0; ///< monitor ring full or trimmed to its latency
};

class AudioInput {
public:
    AudioInput();
    ~AudioInput(); ///< stop()s.

    AudioInput(const AudioInput&) = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    /// The capture devices of the Default backend (a short-lived backend context; not on an audio thread).
    static std::expected<std::vector<AudioDeviceInfo>, Error> enumerateInputDevices();

    /// Opens the device (or the Null input) and starts capturing. Fails with InvalidArgument (bad config, already
    /// running), NotFound (no such device, or no input at all) or DeviceError.
    std::expected<void, Error> start(const AudioInputConfig& config = {});
    void stop() noexcept;
    bool isRunning() const noexcept;
    AudioInputConfig config() const;
    std::string deviceName() const;  ///< "" when stopped; the OS's name (untrusted text)
    std::string backendName() const;
    InputLatency latency() const;

    // ---- Consumer (one thread) ------------------------------------------------------------------------------------
    /// Copies up to interleaved.size() / channels captured frames, oldest first; returns the frames copied. The
    /// frames of one read are consecutive in the stream: a read stops where frames were lost (framesDropped).
    std::size_t read(std::span<float> interleaved) noexcept;
    std::size_t readable() const noexcept;
    /// Stream index just past the last frame read() returned (the frames read plus the frames lost before them): the
    /// frames of the last read are [readPosition() - n, readPosition()).
    std::int64_t readPosition() const noexcept;

    // ---- Monitoring (the output callback) -------------------------------------------------------------------------
    /// Input monitoring: copies the newest captured frames, at most `frames`; older ones beyond `keepFrames` of backlog
    /// are dropped first, so the monitor's latency stays bounded. Returns the frames copied. RT-safe.
    std::size_t readMonitor(std::span<float> interleaved, std::size_t keepFrames) noexcept;
    /// Off: the callback does not feed the monitor ring. Any thread.
    void setMonitorEnabled(bool enabled) noexcept;

    // ---- Any thread -----------------------------------------------------------------------------------------------
    CaptureLine captureLine() const noexcept;
    /// Peak and RMS since the previous call (the meter's reading resets).
    InputLevels levels() noexcept;
    AudioInputStats stats() const noexcept;

    /// Null backend: delivers captured audio as a device callback would, the block ending at steady time `now`.
    /// RT-safe. Fails with InvalidArgument when not running on the Null backend or the size is not whole frames.
    std::expected<void, Error> push(std::span<const float> interleaved,
                                    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    /// The capture callback's work (miniaudio's thread, or push()). Implementation detail; public for the trampoline.
    void onCapture(const float* interleaved, std::uint32_t frames, std::int64_t nowNs) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::audio
