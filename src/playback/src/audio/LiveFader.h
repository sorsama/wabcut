#pragma once

// Internal: a live fader override for automation passes (Touch / Latch / Write; TimelineAudioPlayer::setLiveTrackMix).
// The control side sets a level (dB) and/or a balance, or clears them; while set, the audio thread applies them to a
// track's (or the master's) mix instead of the evaluated fader and panner. Every change, the way in (from the evaluated
// values) and the way out (back to them) is a linear ramp of `rampFrames` (~10 ms), so a dragged fader never zippers.
// The model is never touched: the UI writes the pass as keyframes when it ends.
//
// Threading: set()/clear() from control threads (relaxed atomics; the audio thread sees a new value within a block).
// apply() and engaged() belong to the audio thread. RT-safe: no allocation, no lock.

#include "Fader.h"

#include <wabcut/audio/Gain.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wabcut::playback::detail {

class LiveFader {
public:
    // ---- Control side ----------------------------------------------------------------------------------------
    /// The override; both unset clears it.
    void set(std::optional<double> gainDb, std::optional<double> pan) noexcept {
        if (gainDb) {
            m_gainDb.store(static_cast<float>(std::clamp(*gainDb, -200.0, 24.0)), std::memory_order_relaxed);
        }
        if (pan) {
            m_pan.store(static_cast<float>(std::clamp(*pan, -1.0, 1.0)), std::memory_order_relaxed);
        }
        m_flags.store((gainDb ? kGain : 0u) | (pan ? kPan : 0u), std::memory_order_release);
    }
    void clear() noexcept { m_flags.store(0, std::memory_order_release); }
    bool isSet() const noexcept { return m_flags.load(std::memory_order_acquire) != 0; }

    // ---- Audio thread ----------------------------------------------------------------------------------------
    /// Whether apply() has work: an override is set, or the ramp back to the evaluated fader is still running.
    bool engaged() const noexcept { return m_engaged || m_flags.load(std::memory_order_acquire) != 0; }

    /// Applies the override to `frames` interleaved frames at `dst`. `evalGain` (linear) and `evalBalance` are the
    /// evaluated fader and panner at the block (what the override replaces, and where it ramps from and back to).
    /// Returns false when nothing is engaged: the caller applies the evaluated fader itself, exactly. `allowBalance`
    /// false (a 5.1 mix, A.9) ignores an override's pan: the balance stays `evalBalance`.
    bool apply(float* dst, std::int64_t frames, int channels, double evalGain, audio::StereoGains evalBalance,
               int rampFrames, bool allowBalance = true) noexcept {
        const std::uint32_t flags = m_flags.load(std::memory_order_acquire);
        if (flags == 0 && !m_engaged) {
            return false;
        }
        const auto ramp = static_cast<std::int64_t>(std::max(rampFrames, 1));
        if (!m_engaged) { // the way in starts where the fader is
            m_engaged = true;
            m_gain = static_cast<float>(evalGain);
            m_left = evalBalance.left;
            m_right = evalBalance.right;
            m_targetGain = m_gain;
            m_targetLeft = m_left;
            m_targetRight = m_right;
            m_remaining = 0;
        }
        float gain = static_cast<float>(evalGain);
        audio::StereoGains balance = evalBalance;
        if ((flags & kGain) != 0) {
            gain = static_cast<float>(faderGain(m_gainDb.load(std::memory_order_relaxed)));
        }
        if ((flags & kPan) != 0 && allowBalance) {
            balance = audio::stereoBalance(m_pan.load(std::memory_order_relaxed));
        }
        const bool releasing = flags == 0;
        // A new target starts a ramp from where the output is now. On the way out the target is the evaluated fader
        // where the release began: the ramp runs to its end, then the caller's exact fader takes over.
        if (!(releasing && m_releasing) &&
            (gain != m_targetGain || balance.left != m_targetLeft || balance.right != m_targetRight)) {
            m_targetGain = gain;
            m_targetLeft = balance.left;
            m_targetRight = balance.right;
            m_stepGain = (m_targetGain - m_gain) / static_cast<float>(ramp);
            m_stepLeft = (m_targetLeft - m_left) / static_cast<float>(ramp);
            m_stepRight = (m_targetRight - m_right) / static_cast<float>(ramp);
            m_remaining = ramp;
        }
        m_releasing = releasing;
        const auto ch = static_cast<std::size_t>(std::max(channels, 1));
        for (std::int64_t k = 0; k < frames; ++k) {
            if (m_remaining > 0) {
                if (--m_remaining == 0) {
                    m_gain = m_targetGain;
                    m_left = m_targetLeft;
                    m_right = m_targetRight;
                } else {
                    m_gain += m_stepGain;
                    m_left += m_stepLeft;
                    m_right += m_stepRight;
                }
            }
            float* frame = dst + static_cast<std::size_t>(k) * ch;
            if (ch >= 2) {
                frame[0] *= m_gain * m_left;
                frame[1] *= m_gain * m_right;
                for (std::size_t c = 2; c < ch; ++c) {
                    frame[c] *= m_gain;
                }
            } else {
                frame[0] *= m_gain;
            }
        }
        if (releasing && m_remaining == 0) {
            m_engaged = false; // back on the evaluated fader from the next block
            m_releasing = false;
        }
        return true;
    }

private:
    static constexpr std::uint32_t kGain = 1;
    static constexpr std::uint32_t kPan = 2;

    std::atomic<std::uint32_t> m_flags{0};
    std::atomic<float> m_gainDb{0.0f};
    std::atomic<float> m_pan{0.0f};

    // Audio thread.
    bool m_engaged = false;
    bool m_releasing = false;
    float m_gain = 1.0f, m_left = 1.0f, m_right = 1.0f;
    float m_targetGain = 1.0f, m_targetLeft = 1.0f, m_targetRight = 1.0f;
    float m_stepGain = 0.0f, m_stepLeft = 0.0f, m_stepRight = 0.0f;
    std::int64_t m_remaining = 0;
};

} // namespace wabcut::playback::detail
