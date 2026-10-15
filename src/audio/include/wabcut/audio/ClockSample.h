#pragma once

// ClockSample: the raw device clock (contract rev 2). It relates a device frame position to steady host time
// and knows nothing about timelines; TransportMap maps it onto the timeline.

#include <wabcut/audio/TimeMath.h>

#include <wabcut/model/Time.h>

#include <cstdint>

namespace wabcut::audio {

/// How trustworthy ClockSample::latencyFrames is.
enum class LatencyQuality : std::uint8_t {
    Estimated,      ///< Derived from buffer sizes (and, on WASAPI, one engine period when the driver reports none).
    Measured,       ///< Reported by the backend (WASAPI GetStreamLatency) or exact by construction (Null backend).
    UserCalibrated, ///< A user latency offset (AudioDeviceConfig::latencyOffset) is applied on top.
};

/// One sample of the device clock, published by the audio thread once per callback and readable from any
/// thread (AudioEngine::clockSample(), lock-free via SeqLock).
///
/// Device frames count everything handed to the device since AudioEngine::start(). At steady time `hostTimeNs`
/// the device had been given `devicePositionFrames` frames, and `latencyFrames` of them were still queued
/// (buffers, resampler, the OS audio engine), so the frame being heard was `devicePositionFrames - latencyFrames`.
/// Successive samples lie on the engine's jitter-filtered device-clock line (a delay-locked loop tracks the
/// drift between the device crystal and the steady clock, see AudioEngineStats::driftPpm), so interpolating
/// between them at `negotiatedRate` is smooth. Within one `deviceEpoch` positions are continuous; the epoch
/// increments whenever the device is reopened, switched, rerouted by the OS or replaced by a fallback.
struct ClockSample {
    std::int64_t devicePositionFrames = 0;
    std::int64_t hostTimeNs = 0; ///< std::chrono::steady_clock, nanoseconds since its epoch.
    Rate negotiatedRate{48000, 1};
    std::int64_t latencyFrames = 0;
    LatencyQuality quality = LatencyQuality::Estimated;
    std::uint32_t deviceEpoch = 0;
    bool running = false; ///< False until the first callback after start() and after stop().

    /// Frame being heard at hostTimeNs.
    constexpr std::int64_t presentedFrame() const noexcept { return devicePositionFrames - latencyFrames; }

    /// Device time (ticks since device frame 0, sub-frame precise) being heard at `hostNs`, extrapolated at the
    /// negotiated rate. Exact integer arithmetic: 1 ns = 254.016 ticks.
    std::int64_t presentedDeviceTicksAt(std::int64_t hostNs) const noexcept {
        const std::int64_t atSample = timemath::mulDivSaturate(presentedFrame(), negotiatedRate.ticksPerUnit(), 1,
                                                               Rounding::Floor);
        const std::int64_t elapsed =
            timemath::mulDivSaturate(hostNs - hostTimeNs, kTicksPerSecond, 1'000'000'000, Rounding::Floor);
        return timemath::addSaturate(atSample, elapsed);
    }

    /// Inverse of presentedDeviceTicksAt: the host time at which device time `deviceTicks` is heard (rounded up).
    std::int64_t hostTimeForDeviceTicks(std::int64_t deviceTicks) const noexcept {
        const std::int64_t atSample = timemath::mulDivSaturate(presentedFrame(), negotiatedRate.ticksPerUnit(), 1,
                                                               Rounding::Floor);
        return timemath::addSaturate(hostTimeNs, timemath::mulDivSaturate(deviceTicks - atSample, 1'000'000'000,
                                                                          kTicksPerSecond, Rounding::Ceil));
    }
};

} // namespace wabcut::audio
