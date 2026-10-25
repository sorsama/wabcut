#include <wabcut/keymap/TransportGesture.h>

namespace wabcut::keymap {
namespace {

constexpr int directionOf(TransportGesture::Role role) {
    switch (role) {
    case TransportGesture::Role::Backward:
    case TransportGesture::Role::SlowBackward:
        return -1;
    case TransportGesture::Role::Forward:
    case TransportGesture::Role::SlowForward:
        return 1;
    case TransportGesture::Role::Stop:
        break;
    }
    return 0;
}

constexpr bool isSlow(TransportGesture::Role role) {
    return role == TransportGesture::Role::SlowBackward || role == TransportGesture::Role::SlowForward;
}

constexpr std::int32_t kLadder[] = {1, 2, 4, TransportGesture::kMaxShuttleSpeed};

} // namespace

TransportGesture::TransportGesture(std::chrono::milliseconds holdThreshold)
    : m_holdThreshold(holdThreshold) {}

std::optional<TransportGesture::Role> TransportGesture::roleOf(std::string_view command) {
    if (command == commands::kShuttleBackward) {
        return Role::Backward;
    }
    if (command == commands::kShuttleStop) {
        return Role::Stop;
    }
    if (command == commands::kShuttleForward) {
        return Role::Forward;
    }
    if (command == commands::kShuttleSlowBackward) {
        return Role::SlowBackward;
    }
    if (command == commands::kShuttleSlowForward) {
        return Role::SlowForward;
    }
    return std::nullopt;
}

Rational TransportGesture::nextRung(int dir) const {
    // Moving the other way (or stopped): start at 1x in the new direction.
    if (m_speed.sign() != dir) {
        return {dir, 1};
    }
    // Same direction: the next rung above the current magnitude, capped at the top rung.
    const Rational magnitude{m_speed.num * dir, m_speed.den};
    for (const std::int32_t rung : kLadder) {
        if (magnitude.lessThan({rung, 1})) {
            return {rung * dir, 1};
        }
    }
    return {kMaxShuttleSpeed * dir, 1};
}

std::vector<TransportIntent> TransportGesture::startSlow(int dir) {
    m_slow = dir;
    m_speed = {kSlowSpeed.num * dir, kSlowSpeed.den};
    return {TransportIntent::setSpeed(m_speed)};
}

std::vector<TransportIntent> TransportGesture::slowStep(int dir) {
    // Same direction below 1x: the next rung up; at 1x or faster that way: no change. Stopped or the other way: 1/4x.
    if (m_speed.sign() == dir) {
        const Rational magnitude{m_speed.num * dir, m_speed.den};
        for (const Rational rung : kSlowLadder) {
            if (magnitude.lessThan(rung)) {
                m_speed = {rung.num * dir, rung.den};
                return {TransportIntent::setSpeed(m_speed)};
            }
        }
        return {};
    }
    m_speed = {kSlowLadder[0].num * dir, kSlowLadder[0].den};
    return {TransportIntent::setSpeed(m_speed)};
}

std::vector<TransportIntent> TransportGesture::stopAll() {
    m_slow = 0;
    m_speed = {};
    return {TransportIntent::stop()};
}

std::vector<TransportIntent> TransportGesture::press(Role role, bool autoRepeat,
                                                     std::chrono::milliseconds now) {
    if (role == Role::Stop) {
        if (autoRepeat) {
            return {};
        }
        // A stop key: whatever was playing stops, and J/L already down do not join the gesture (K-first).
        ++m_stopKeysDown;
        m_pending = 0;
        m_backward.withStop = false;
        m_forward.withStop = false;
        return stopAll();
    }

    const int dir = directionOf(role);
    if (isSlow(role)) {
        // Shift+J / Shift+L (0.4): latched, so it never touches the K+J/L state (m_slow, m_pending) and no release
        // ends it; OS auto-repeat does not climb the slow ladder either.
        return autoRepeat ? std::vector<TransportIntent>{} : slowStep(dir);
    }
    Direction& d = direction(dir);
    if (autoRepeat) {
        // A held J/L: with K down this is the hold that starts slow play; without K it is ignored, so holding
        // J does not race up the ladder.
        if (isStopHeld() && m_pending == dir && m_slow != dir) {
            return startSlow(dir);
        }
        return {};
    }
    d.down = true;
    d.since = now;
    d.withStop = isStopHeld();
    if (d.withStop) {
        m_pending = dir;
        m_speed = {};
        return {TransportIntent::stepFrames(dir)};
    }
    m_speed = nextRung(dir);
    return {TransportIntent::shuttleStep(m_speed)};
}

std::vector<TransportIntent> TransportGesture::release(Role role) {
    if (role == Role::Stop) {
        if (m_stopKeysDown > 0) {
            --m_stopKeysDown;
        }
        if (isStopHeld()) {
            return {}; // another stop key is still down
        }
        m_pending = 0;
        return m_slow != 0 ? stopAll() : std::vector<TransportIntent>{};
    }
    if (isSlow(role)) {
        return {}; // latched slow play keeps playing after Shift+J / Shift+L come up
    }

    const int dir = directionOf(role);
    direction(dir) = {};
    if (m_pending == dir) {
        // Hand over to the other J/L if it went down with K and is still held.
        const Direction& other = direction(-dir);
        m_pending = isStopHeld() && other.down && other.withStop ? -dir : 0;
    }
    return m_slow == dir ? stopAll() : std::vector<TransportIntent>{};
}

std::vector<TransportIntent> TransportGesture::tick(std::chrono::milliseconds now) {
    if (isStopHeld() && m_pending != 0 && m_slow != m_pending &&
        now - direction(m_pending).since >= m_holdThreshold) {
        return startSlow(m_pending);
    }
    return {};
}

std::vector<TransportIntent> TransportGesture::reset() {
    const bool wasSlow = m_slow != 0;
    m_stopKeysDown = 0;
    m_backward = {};
    m_forward = {};
    m_pending = 0;
    return wasSlow ? stopAll() : std::vector<TransportIntent>{};
}

} // namespace wabcut::keymap
