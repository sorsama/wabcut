#pragma once

// AudioEngine: the output device, the transport mapping from source time to device frames, and the master
// clock that drives A/V sync for the whole editor.

#include <wabcut/audio/AudioSource.h>
#include <wabcut/audio/ClockSample.h>
#include <wabcut/audio/TransportMap.h>

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::audio {

enum class AudioBackend {
    /// The system's output API as chosen by miniaudio (WASAPI on Windows, Core Audio on macOS, PulseAudio /
    /// ALSA / JACK on Linux).
    Default,
    /// A silent device whose thread consumes audio in real time (miniaudio's null backend). For CI and
    /// headless machines: the whole device path (callback thread, clock interpolation, watchdog) runs, but
    /// nothing is heard. Default falls back to this when no output device can be opened.
    NullRealtime,
    /// No device and no thread. The owner drives the engine by calling pull(), which runs exactly the code
    /// the device callback runs. Deterministic: used by tests, and by anything that needs the engine's
    /// transport and clock without hardware.
    Null,
};

struct AudioDeviceConfig {
    /// Engine sample rate. The device is opened at this rate; if the hardware runs at another rate the OS or
    /// miniaudio resamples, so sources and the clock always see exactly this rate.
    int sampleRate = 48000;
    int channels = 2;
    /// Requested device period (callback size) in frames. Backends may round it; latency() reports the result.
    int periodFrames = 512;
    AudioBackend backend = AudioBackend::Default;
    /// Output device, as AudioDeviceInfo::id from enumerateOutputDevices(). Empty means "the system default",
    /// which also follows the user changing the default device (e.g. plugging in headphones).
    std::string deviceId;
    /// Default backend: when no real device can be opened (none present, or all failing), run NullRealtime so
    /// the clock and playback keep working. start() then succeeds and stats().usingFallbackDevice is true.
    bool fallbackToNull = true;
    /// Added to the measured output latency (user calibration, e.g. for Bluetooth or an external DAC whose
    /// latency the OS does not report). Positive values delay the clock. Also settable at run time.
    Ticks latencyOffset{};
    /// The engine never asks a source for more than this many frames per render() call.
    int maxBlockFrames = 4096;
    /// Null backend only: simulated output latency in frames (the gap between rendering and "hearing").
    int nullLatencyFrames = 0;
    /// Length of the de-click fades at transport edges (start, pause, seek, preroll end, loop wrap, source
    /// swap, speed change): new audio fades in and the last output decays out, so the waveform never steps.
    /// 2 ms by default; Ticks{} disables it (bit-exact output, e.g. for tests that compare samples).
    Ticks declickTime{kTicksPerSecond / 500};
    /// Upper bound for one device operation (open, switch, reopen, close). The OS audio stack can stall in them
    /// (device changes, a busy audio service, a virtual mixer holding the endpoint); when one takes longer the
    /// engine abandons that device, reports a DeviceError naming the stuck stage, and carries on with a fresh
    /// device (selected, then default, then NullRealtime if fallbackToNull), counted in stats().deviceWedges.
    std::chrono::milliseconds deviceTimeout{5000};
};

struct AudioDeviceInfo {
    /// Stable, persistable identifier ("<backend>:<native id>"). Pass it back in AudioDeviceConfig::deviceId.
    std::string id;
    /// Human-readable name (UTF-8).
    std::string name;
    bool isDefault = false;
};

/// Output latency estimate: the time between a sample being rendered and being heard.
struct AudioLatency {
    std::int64_t bufferFrames = 0;  ///< Audio queued in the device buffer ahead of a freshly rendered sample.
    /// Latency the OS adds after the buffer. WASAPI: the stream latency the driver reports, or one audio-engine
    /// period when it reports none (a lower bound). Other backends: 0 (not reported).
    std::int64_t streamFrames = 0;
    std::int64_t offsetFrames = 0;  ///< The user offset (AudioDeviceConfig::latencyOffset), in frames.
    std::int64_t periodFrames = 0;  ///< Device period actually granted.
    std::int64_t totalFrames = 0;   ///< bufferFrames + streamFrames + offsetFrames (what the clock compensates).
    Ticks total{};                  ///< totalFrames as time.
};

/// Counters for diagnostics UI and tests. Each field is read independently (not a consistent snapshot).
struct AudioEngineStats {
    std::uint64_t callbacks = 0;       ///< Device callbacks (or pull() calls) since start().
    std::uint64_t framesRendered = 0;  ///< Frames delivered to the device since start().
    std::uint64_t deviceReopens = 0;   ///< Recoveries and device switches since start().
    std::uint64_t clockResyncs = 0;    ///< Times the clock model was re-anchored (start, xrun, reroute).
    std::uint32_t lastCallbackFrames = 0;
    std::uint32_t deviceEpoch = 0;     ///< ClockSample::deviceEpoch of the latest callback.
    /// Device clock vs steady clock as tracked by the DLL, in parts per million (positive: the device runs
    /// fast). Monitored continuously; the clock follows it smoothly and is never periodically snapped.
    double driftPpm = 0.0;
    bool usingFallbackDevice = false;  ///< Running on a substitute (default instead of selected, or null).
    /// Device operations that exceeded AudioDeviceConfig::deviceTimeout since start() (each one abandoned).
    std::uint64_t deviceWedges = 0;
};

/// The real-time audio engine and master clock.
///
/// Lifecycle: construct, optionally setSource(), start(config), and later stop(). The engine may be started
/// and stopped repeatedly; the source and the transport position survive a stop.
///
/// Transport. The engine owns a TransportMap (see TransportMap.h) from device frames to timeline time. It
/// starts paused at Ticks{0}, at speed 1/1, without a loop.
///   * seek(t, preroll) / setTimeline(t): discontinuity. The next rendered frame has timeline time t (after
///     `preroll`, during which nothing is rendered). The clock jumps to t at once and holds there until the first
///     sample of t is actually heard, then advances. Old audio still in the device buffer is not re-labelled.
///   * pause() / resume(): continuous. While paused the source is not rendered (the device plays silence) and
///     source time does not advance. The clock keeps following the samples being heard, so after pause() it
///     runs on for the output latency (the audio already queued) and then freezes on the last rendered sample;
///     after resume() it stays frozen until the first new sample is heard. Neither ever jumps.
///   * setSpeed(s): rational speed while playing (J/K/L shuttle). MVP policy: the source is rendered only at 1/1
///     forward; at any other speed the output is muted while the clock advances at that speed.
///   * setLoop(range): playback that reaches the loop end continues at the loop start.
///   * Commands take effect at the next audio block (they are applied by the audio thread at a block boundary),
///     or immediately while the engine is stopped.
///
/// Raw device clock. clockSample() is the latest ClockSample (device position, steady host time, negotiated
/// rate, latency and its quality, device epoch). transportMap() is a copy of the TransportMap with that sample
/// installed, for consumers that schedule ahead (TransportMap::hostTimeFor) or evaluate it themselves.
///
/// Master clock. clock() is the source time of the sample being heard now, compensated for the output latency
/// (latency()). It may be read from any thread at any rate: the audio thread publishes an anchor once per
/// callback through a SeqLock, and readers interpolate from it with std::chrono::steady_clock. Callback jitter
/// is filtered by a second-order delay-locked loop that also tracks the drift between the audio clock and the
/// steady clock, so video can sample the clock at display rate without judder. While playing the clock is
/// monotonic: if the latency estimate grows (e.g. switching to Bluetooth headphones) or the device stalls, it
/// holds rather than going backwards, and it never runs ahead of the audio that has actually been rendered.
/// Only an explicit seek moves it backwards.
///
/// Device recovery (Default and NullRealtime). A watchdog thread reopens the device when it stops on its own
/// (unplugged, invalidated by the OS, driver reset) or stops calling back, falling back from the selected
/// device to the system default and then (if allowed) to NullRealtime, and switches back when the selected
/// device returns. Default-device changes are followed automatically when deviceId is empty (by the OS
/// backend where it supports rerouting, e.g. WASAPI and Core Audio). The device frame counter, transport and
/// clock carry across a reopen, so playback resumes on the new device where it left off and the clock stays
/// monotonic.
///
/// Real-time rules. Nothing on the audio thread blocks, locks, allocates or logs. setSource() hands the audio
/// thread a raw pointer and keeps the std::shared_ptr alive on the control side until the audio thread has
/// provably stopped using it, so sources are always destroyed off the audio thread. The control functions
/// below are thread-safe and may be called from any non-audio thread; they are not RT-safe.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine(); ///< stop()s the engine.

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Lists the output devices of the Default backend. Opens a short-lived backend context; call it from a
    /// worker or GUI thread, not the audio thread.
    static std::expected<std::vector<AudioDeviceInfo>, Error> enumerateOutputDevices();

    /// Validate `config`, open the device and start rendering. Fails with InvalidArgument (bad config or
    /// already running), NotFound (deviceId unknown and fallback disabled) or DeviceError.
    std::expected<void, Error> start(const AudioDeviceConfig& config = {});
    /// Stop rendering and close the device. Idempotent. The clock freezes at its current value.
    void stop() noexcept;
    bool isRunning() const noexcept;

    /// The configuration passed to start() (with latencyOffset updated by setLatencyOffset()).
    AudioDeviceConfig config() const;
    /// Name of the device currently open ("" when stopped), e.g. "Speakers (Realtek(R) Audio)".
    std::string deviceName() const;
    /// Id of the device currently open ("" when stopped or for Null).
    std::string deviceId() const;
    /// Name of the active backend ("WASAPI", "Core Audio", "Null", ...; "" when stopped).
    std::string backendName() const;
    /// While stats().usingFallbackDevice: why the selected device (or any real device) could not be used.
    /// After a device operation timed out (stats().deviceWedges): what stalled, until the next successful
    /// device change. Empty otherwise.
    std::string deviceError() const;

    /// Switch output device while running (empty = follow the system default). Transport and clock carry over.
    /// When stopped, only records the choice for the next start().
    std::expected<void, Error> selectDevice(const std::string& deviceId);
    /// Close and reopen the current device (e.g. after the user changed OS audio settings). Clock carries over.
    /// Like start(), selectDevice() and stop(), bounded by AudioDeviceConfig::deviceTimeout: when the backend
    /// does not answer in time the call returns a DeviceError and the engine continues on a fresh device.
    std::expected<void, Error> reopenDevice();

    /// Latency the clock currently compensates for. Updated whenever the device is (re)opened or rerouted.
    AudioLatency latency() const;
    /// Change the user latency calibration while running.
    void setLatencyOffset(Ticks offset) noexcept;

    /// Replace the source the audio thread renders (nullptr = silence). Any thread; takes effect at the next
    /// block. The previous source is released on a control thread once the audio thread no longer uses it.
    void setSource(std::shared_ptr<AudioSource> source);
    std::shared_ptr<AudioSource> source() const;

    /// Seek: map the next rendered frame to `start` (see class comment). Any thread. Same as seek(start, {}).
    void setTimeline(Ticks start);
    /// Seek with a preroll: the transport holds at `target` and renders nothing for `preroll` (rounded up to
    /// whole frames), giving decoders time to fill the PCM rings, then plays. Preroll only applies while playing.
    /// Returns the seek's generation (see appliedSeekGeneration()).
    std::uint64_t seek(Ticks target, Ticks preroll);
    /// Generation of the latest seek the audio thread has applied (immediately while stopped). A component that
    /// prepares audio for a seek target (e.g. the timeline player's feeder) waits until its seek is applied, so
    /// a callback still running at the old position never sees the new data. Any thread, lock-free.
    std::uint64_t appliedSeekGeneration() const noexcept;
    /// Playback speed (any thread). Invalid speeds (den == 0) are ignored. 0 freezes; negative is reverse.
    void setSpeed(TransportSpeed speed);
    TransportSpeed speed() const;
    /// Loop range (nullopt or empty: off). Any thread.
    void setLoop(std::optional<TimeRange> range);
    std::optional<TimeRange> loop() const;
    /// Stop advancing source time and render silence (see class comment). Any thread.
    void pause();
    /// Continue from where pause() stopped. Any thread.
    void resume();
    /// The requested transport state (true from construction until the first resume()).
    bool isPaused() const noexcept;

    /// The master clock: source time of the sample being heard now. Any thread, lock-free, never blocks the
    /// audio thread.
    Ticks clock() const noexcept;
    /// clock() evaluated at an explicit steady-clock time (deterministic tests, frame scheduling ahead of
    /// vsync). Times before the latest audio callback are evaluated as of that callback.
    Ticks clockAt(std::chrono::steady_clock::time_point now) const noexcept;

    /// The latest device clock sample (contract rev 2). Any thread, lock-free (SeqLock). running == false while
    /// stopped and until the first callback.
    ClockSample clockSample() const noexcept;
    /// A copy of the transport map as of the latest callback, with clockSample() installed. A seek that the
    /// audio thread has not applied yet is not in it (clock() reports pending seeks).
    TransportMap transportMap() const noexcept;

    /// Null backend only: render interleaved.size() / channels frames through the graph as if the device had
    /// called back at steady-clock time `now`, advancing the device frame counter and the clock. Must not be
    /// called concurrently with itself. RT-safe in the same sense as the device callback. Fails with
    /// InvalidArgument when not running on the Null backend or the size is not a whole number of frames.
    std::expected<void, Error> pull(std::span<float> interleaved,
                                    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    AudioEngineStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::audio
