#pragma once

// J/K/L transport gestures (Premiere behaviour), as a pure state machine fed with key presses AND releases.
// It emits typed transport intents for PlaybackController::setTransportSpeed / stepFrames
// (docs/architecture.md):
//
//   L / J            ShuttleStep: forward / backward along the ladder 1x, 2x, 4x, 8x (one rung per press;
//                    pressing the other direction starts at 1x that way)
//   K                Stop
//   hold K, tap J/L  StepFrames(-1 / +1)
//   hold K, hold J/L SetSpeed(-1/4 / +1/4) while both are held
//   release J/L or K during slow play: Stop
//   Shift+L / Shift+J (0.4, Shuttle Slow Right / Left): SetSpeed(±1/4) and stays playing after the release
//                    (latched); the next press the same way steps 1/4 -> 1/2 -> 1x and stays at 1x; a press while
//                    stopped or going the other way starts at 1/4x that way. K, Space and J/L end or change it.
//
// The gesture is K-first: only a J/L that goes down while K is held takes part; pressing K always stops (also
// during slow play). OS auto-repeat never climbs the ladder. A hold is recognised by the first auto-repeat
// press of J/L, or by tick() once J/L has been down for the hold threshold (for systems with key repeat off).
// With K held, the most recently pressed J/L is the one that can become a hold; releasing it hands over to
// the other one if that is still down. Roles are bound by command, not by key: whatever keys the user binds
// to transport.shuttleBackward / shuttleStop / shuttleForward drive the gesture, and several keys may share a
// role.

#include <wabcut/keymap/CommandRegistry.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace wabcut::keymap {

/// Signed exact ratio, e.g. a transport speed (-2/1 = twice real time backward, 1/4 = quarter speed).
struct Rational {
    std::int32_t num = 0;
    std::int32_t den = 1; ///< always positive

    constexpr bool operator==(const Rational&) const = default;
    constexpr bool isZero() const { return num == 0; }
    constexpr int sign() const { return num > 0 ? 1 : (num < 0 ? -1 : 0); }
    /// Exact comparison (den > 0).
    constexpr bool lessThan(Rational o) const {
        return static_cast<std::int64_t>(num) * o.den < static_cast<std::int64_t>(o.num) * den;
    }
};

/// What the gesture asks the transport to do.
struct TransportIntent {
    enum class Kind : std::uint8_t {
        ShuttleStep, ///< J/L: one rung along the shuttle ladder; `speed` is the new speed (±1, ±2, ±4, ±8)
        SetSpeed,    ///< K+J / K+L held: play at `speed` (∓1/4) until a Stop
        StepFrames,  ///< K held + tap J/L: move `frames` frames (±1) and stay paused
        Stop,        ///< K, or the end of slow play
    };

    Kind kind = Kind::Stop;
    Rational speed{};
    int frames = 0;

    static constexpr TransportIntent shuttleStep(Rational s) { return {Kind::ShuttleStep, s, 0}; }
    static constexpr TransportIntent setSpeed(Rational s) { return {Kind::SetSpeed, s, 0}; }
    static constexpr TransportIntent stepFrames(int n) { return {Kind::StepFrames, {}, n}; }
    static constexpr TransportIntent stop() { return {Kind::Stop, {}, 0}; }

    constexpr bool operator==(const TransportIntent&) const = default;
};

class TransportGesture {
public:
    /// The J, K and L roles; 0.4: Shift+J and Shift+L (latched slow shuttle).
    enum class Role { Backward, Stop, Forward, SlowBackward, SlowForward };

    static constexpr std::chrono::milliseconds kDefaultHoldThreshold{300};
    static constexpr Rational kSlowSpeed{1, 4};
    static constexpr std::int32_t kMaxShuttleSpeed = 8;
    /// 0.4: the slow shuttle's rungs (Shift+J / Shift+L), from a stop or the other direction: 1/4, 1/2, 1x.
    static constexpr Rational kSlowLadder[] = {{1, 4}, {1, 2}, {1, 1}};

    explicit TransportGesture(std::chrono::milliseconds holdThreshold = kDefaultHoldThreshold);

    /// The role of a shuttle command id (transport.shuttleBackward/Stop/Forward, 0.4: shuttleSlowBackward/Forward),
    /// nullopt for other commands.
    static std::optional<Role> roleOf(std::string_view command);

    /// A key with `role` went down (`autoRepeat` = OS key repeat). `now` must come from the same monotonic
    /// clock as tick()'s argument. Returns the intents to apply, in order.
    std::vector<TransportIntent> press(Role role, bool autoRepeat, std::chrono::milliseconds now);
    /// A key with `role` came up (ignore auto-repeat releases before calling this).
    std::vector<TransportIntent> release(Role role);
    /// Promotes a K+J / K+L tap to slow play once it has been held for the threshold.
    std::vector<TransportIntent> tick(std::chrono::milliseconds now);
    /// Forgets all held keys (focus lost); stops slow play if it was running. Normal shuttle play continues.
    std::vector<TransportIntent> reset();
    /// Tells the gesture the transport's actual speed when something else changed it (Play/Stop, a seek, the
    /// transport bar), so the next J/L press climbs from there.
    void syncSpeed(Rational speed) { m_speed = speed; }

    bool isStopHeld() const { return m_stopKeysDown > 0; }
    /// -1 while slow-playing backward, +1 forward, 0 otherwise.
    int slowDirection() const { return m_slow; }
    /// The speed the gesture believes the transport runs at.
    Rational speed() const { return m_speed; }

private:
    struct Direction {
        bool down = false;
        bool withStop = false; ///< went down while K was held, so it takes part in the gesture
        std::chrono::milliseconds since{};
    };

    Direction& direction(int dir) { return dir < 0 ? m_backward : m_forward; }
    std::vector<TransportIntent> startSlow(int dir);
    std::vector<TransportIntent> stopAll();
    Rational nextRung(int dir) const;
    /// 0.4: a Shift+J (-1) / Shift+L (+1) press: the next slow rung that way (latched; no release ends it).
    std::vector<TransportIntent> slowStep(int dir);

    std::chrono::milliseconds m_holdThreshold;
    int m_stopKeysDown = 0;
    Direction m_backward;
    Direction m_forward;
    int m_pending = 0; ///< the J/L (-1/+1) that can become a hold while K is down, 0 for none
    int m_slow = 0;
    Rational m_speed{};
};

} // namespace wabcut::keymap
