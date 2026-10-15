#pragma once

// TransportMap: the pure mapping between the device clock and the timeline (contract rev 2). No device, no
// threads, no allocation: a trivially copyable value that the audio thread owns, publishes through a SeqLock
// once per callback, and that video / the scheduler evaluate with transportTime().

#include <wabcut/audio/ClockSample.h>
#include <wabcut/audio/TimeMath.h>

#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>

namespace wabcut::audio {

/// Rational transport speed: 1/1 is normal playback, 0 is paused/frozen, negative is reverse.
struct TransportSpeed {
    std::int32_t num = 1;
    std::int32_t den = 1;

    static constexpr TransportSpeed normal() noexcept { return {1, 1}; }
    static constexpr TransportSpeed zero() noexcept { return {0, 1}; }

    constexpr bool isValid() const noexcept { return den != 0; }
    constexpr bool isZero() const noexcept { return num == 0; }
    constexpr bool isNormal() const noexcept { return num != 0 && num == den; }
    constexpr bool isReverse() const noexcept { return num != 0 && den != 0 && ((num < 0) != (den < 0)); }
    /// Same speed with a positive denominator, reduced. Invalid speeds become zero().
    constexpr TransportSpeed normalized() const noexcept {
        if (den == 0 || num == 0) {
            return zero();
        }
        std::int64_t n = num;
        std::int64_t d = den;
        if (d < 0) {
            n = -n;
            d = -d;
        }
        std::int64_t a = n < 0 ? -n : n;
        std::int64_t b = d;
        while (b != 0) {
            const std::int64_t t = a % b;
            a = b;
            b = t;
        }
        return {static_cast<std::int32_t>(n / a), static_cast<std::int32_t>(d / a)};
    }
    friend constexpr bool operator==(TransportSpeed a, TransportSpeed b) noexcept {
        return static_cast<std::int64_t>(a.num) * b.den == static_cast<std::int64_t>(b.num) * a.den &&
               a.isValid() == b.isValid();
    }
};

/// Piecewise-linear map from device frames (and, through a ClockSample, host time) to timeline Ticks.
///
/// The map is a short history of segments. From segment.deviceFrame on, the timeline advances by
/// speed · (device time elapsed), optionally wrapping inside a loop range. Device frames before the oldest
/// segment hold at its start, which is how a seek "flushes": audio still in flight from before the seek
/// reports the new position, not the old one.
///
/// Commands take a device frame (the block boundary where they take effect); frames passed to successive
/// commands must not decrease. A command supersedes segments scheduled at or after its frame (e.g. the end
/// of a preroll), except setSpeed/setLoop during a preroll, which update the pending play segment instead.
///   * play / pause / setSpeed: continuous, the timeline never jumps.
///   * seek(target, at, preroll): discontinuity. History is dropped, the timeline holds at `target` for
///     `prerollFrames` (decoders get time to fill the PCM rings; nothing is rendered) and then plays.
///   * setLoop: playback that runs into the loop end wraps to the loop start (reverse: below the start to
///     the end). Playback starting beyond the loop end is not wrapped back.
///   * rebase: the device epoch changed (reopen, switch, fallback, reroute). The position at `oldFrame` in the
///     old frame domain continues at `newFrame` in the new one with the speed in effect there (0 while paused or
///     prerolling); pending segments (the end of a preroll) keep their distance, so a preroll survives a
///     rebase. The clock is forgotten until setClock().
///
/// All arithmetic is integer (Principle 1): Ticks per frame from Rate, rational speeds and ns<->ticks through
/// wabcut::mulDiv with explicit rounding. RT-safe throughout.
class TransportMap {
public:
    static constexpr int kMaxSegments = 8;

    struct Segment {
        std::int64_t deviceFrame = 0;
        std::int64_t timeline = 0; ///< Ticks at deviceFrame (already loop-wrapped).
        std::int32_t speedNum = 0; ///< Normalized: speedDen > 0.
        std::int32_t speedDen = 1;
        std::int64_t loopStart = 0;
        std::int64_t loopEnd = 0; ///< Loop active iff loopEnd > loopStart.
    };

    /// A run of frames with one linear mapping and no loop wrap inside, for renderers.
    struct Piece {
        Ticks timeline;       ///< At the first frame.
        TransportSpeed speed; ///< Normalized.
        std::int64_t frames;  ///< >= 1, <= the maxFrames asked for.
    };

    /// Paused at `position`, normal play speed, no loop, epoch 0. `sampleRate` must be valid and tick-exact
    /// (48 kHz is used otherwise).
    explicit TransportMap(Rate sampleRate, Ticks position = {}) noexcept;
    TransportMap() noexcept : TransportMap(kRate48kHz) {}

    Rate sampleRate() const noexcept { return m_rate; }
    std::uint32_t deviceEpoch() const noexcept { return m_epoch; }

    // ---- Commands -------------------------------------------------------------------------------------------
    void play(std::int64_t atFrame) noexcept;
    void pause(std::int64_t atFrame) noexcept;
    /// Speed used while playing (0 freezes, negative reverses). Invalid speeds are ignored.
    void setSpeed(TransportSpeed speed, std::int64_t atFrame) noexcept;
    void seek(Ticks target, std::int64_t atFrame, std::int64_t prerollFrames = 0) noexcept;
    /// nullopt or an empty range turns looping off.
    void setLoop(std::optional<TimeRange> range, std::int64_t atFrame) noexcept;
    void rebase(std::uint32_t newEpoch, std::int64_t oldFrame, std::int64_t newFrame) noexcept;
    /// A seek's preroll holds the transport at `frame` (its play segment is still to come).
    bool prerollHolding(std::int64_t frame) const noexcept;
    /// Ends that preroll at `atFrame`: the playback that was to follow it starts there, from the seek target (P.1:
    /// when the source already has the audio). False, changing nothing, when no preroll holds at `atFrame`.
    bool endPreroll(std::int64_t atFrame) noexcept;
    /// The device clock used by transportTime() / hostTimeFor(). A sample with running == false clears it.
    void setClock(const ClockSample& clock) noexcept;

    // ---- State ----------------------------------------------------------------------------------------------
    bool isPlaying() const noexcept { return m_playing; }
    TransportSpeed playSpeed() const noexcept { return m_playSpeed; }
    /// Speed in effect at `frame` (0 while paused or prerolling).
    TransportSpeed speedAtFrame(std::int64_t frame) const noexcept;
    std::optional<TimeRange> loop() const noexcept;
    bool hasClock() const noexcept { return m_hasClock; }
    const ClockSample& clock() const noexcept { return m_clock; }
    int segmentCount() const noexcept { return m_count; }
    const Segment& segment(int index) const noexcept { return m_segments[index]; }

    // ---- Mapping (pure) -------------------------------------------------------------------------------------
    Ticks timelineAtFrame(std::int64_t frame) const noexcept;
    /// Sub-frame precise: `deviceTicks` is device time since frame 0 (frame f starts at f · ticksPerFrame).
    Ticks timelineAtDeviceTicks(std::int64_t deviceTicks) const noexcept;
    /// The run starting at `frame`, at most `maxFrames` (>= 1) long, ending early at a segment start or loop wrap.
    Piece piece(std::int64_t frame, std::int64_t maxFrames) const noexcept;
    /// What video follows: the timeline position being heard at steady time `hostTimeNs`. Without a clock this
    /// is the position at the latest command.
    Ticks transportTime(std::int64_t hostTimeNs) const noexcept;
    /// Inverse: the device time at which `timeline` is heard under the latest segment, i.e. if the transport
    /// continues as commanded (a position already passed on that segment yields a time in the past; with a loop,
    /// an earlier position inside the loop yields the next lap). nullopt if the segment never presents it:
    /// paused elsewhere, before the segment start in its direction of travel, or at/after a loop end it wraps at.
    std::optional<std::int64_t> deviceTicksFor(Ticks timeline) const noexcept;
    /// Inverse in host time (requires a clock): transportTime(*hostTimeFor(t)) >= t, and one nanosecond
    /// earlier the transport is still before t.
    std::optional<std::int64_t> hostTimeFor(Ticks timeline) const noexcept;

private:
    Segment makeSegment(std::int64_t atFrame, std::int64_t timeline, TransportSpeed speed) const noexcept;
    void push(const Segment& segment) noexcept;
    int indexFor(std::int64_t frame) const noexcept; ///< Last segment starting at or before `frame`, or -1.
    std::int64_t frameTicks(std::int64_t frame) const noexcept;
    std::int64_t evaluate(const Segment& segment, std::int64_t offsetTicks) const noexcept;
    TransportSpeed effectiveSpeed() const noexcept { return m_playing ? m_playSpeed : TransportSpeed::zero(); }

    Rate m_rate{48000, 1};
    std::int64_t m_ticksPerFrame = 0;
    Segment m_segments[kMaxSegments]{};
    std::int32_t m_count = 1;
    bool m_playing = false;
    bool m_hasClock = false;
    std::uint32_t m_epoch = 0;
    TransportSpeed m_playSpeed = TransportSpeed::normal();
    std::int64_t m_loopStart = 0;
    std::int64_t m_loopEnd = 0;
    ClockSample m_clock{};
};

} // namespace wabcut::audio
