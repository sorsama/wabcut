#pragma once

// KeyDispatcher turns raw key events into commands and transport intents: keymap resolution for the focused
// scope, the auto-repeat policy (CommandInfo::autoRepeat) and the J/K/L gesture (TransportGesture). It is the
// Qt-free core of the app's KeymapDispatcher bridge, which decides what reaches it (text fields, popups, IME
// and AltGr are filtered out there; see docs/keymap.md).
//
// Feed it every key press AND release, call focusLost() when the window deactivates (the matching releases
// never arrive) and tick() from a short timer while isGestureActive(), so a K+J / K+L hold is recognised even
// with key repeat turned off. Presses and ticks are timed with one clock (steady_clock unless the constructor
// is given another), never with window-system event timestamps.

#include <wabcut/keymap/CommandRegistry.h>
#include <wabcut/keymap/Key.h>
#include <wabcut/keymap/KeymapManager.h>
#include <wabcut/keymap/Scope.h>
#include <wabcut/keymap/TransportGesture.h>

#include <chrono>
#include <functional>
#include <map>
#include <string_view>
#include <vector>

namespace wabcut::keymap {

/// What one key event (or trigger) produced.
struct KeyResult {
    /// The key is bound in this scope: the app should consume the event, even when nothing runs (for example
    /// the auto-repeat of a toggle such as Play/Stop).
    bool handled = false;
    std::vector<CommandId> commands;        ///< commands to run, in order
    std::vector<TransportIntent> transport; ///< J/K/L intents, in order
    /// 0.7 (card 063): on a key release, the momentary commands (CommandInfo::momentary) whose key was held past the
    /// hold threshold (or began to repeat): the app hands back what the command had replaced. A tap reports nothing.
    std::vector<CommandId> released;

    bool empty() const { return commands.empty() && transport.empty() && released.empty(); }
};

class KeyDispatcher {
public:
    /// Monotonic milliseconds; any epoch.
    using Clock = std::function<std::chrono::milliseconds()>;

    /// `keymap` must outlive the dispatcher; later keymap edits are picked up automatically. An empty `clock`
    /// means std::chrono::steady_clock.
    explicit KeyDispatcher(const KeymapManager& keymap, Clock clock = {},
                           std::chrono::milliseconds holdThreshold = TransportGesture::kDefaultHoldThreshold);

    KeyResult keyPress(Scope focused, KeyChord chord, bool autoRepeat);
    /// The J/K/L gesture reacts to releases, and so do momentary commands held past the threshold
    /// (KeyResult::released); `handled` is true when the press was handled.
    KeyResult keyRelease(KeyChord chord, bool autoRepeat);
    /// Runs a command as if its key had been tapped (menus, buttons): shuttle commands go through the gesture
    /// (press + release), anything else is returned as a command. Unknown ids are not handled.
    KeyResult trigger(std::string_view command);
    /// Lets the gesture recognise a hold that key repeat did not report.
    std::vector<TransportIntent> tick();
    /// The momentary commands (CommandInfo::momentary) whose keys are down now and have been held past the threshold:
    /// what focusLost() is about to forget, so the app can hand their state back (their releases will never arrive).
    std::vector<CommandId> heldMomentary() const;
    /// Forgets held keys and ends any slow play.
    std::vector<TransportIntent> focusLost();
    /// See TransportGesture::syncSpeed().
    void syncTransportSpeed(Rational speed) { m_gesture.syncSpeed(speed); }

    /// True while K is held, i.e. while tick() can produce intents.
    bool isGestureActive() const { return m_gesture.isStopHeld(); }
    const TransportGesture& gesture() const { return m_gesture; }

private:
    const KeymapManager* m_keymap;
    Clock m_clock;
    TransportGesture m_gesture;
    /// A key that is down: the command its press resolved to (nullptr: unbound), when it went down and whether key
    /// repeat has begun (a hold, even before the threshold).
    struct Held {
        const CommandInfo* info = nullptr;
        std::chrono::milliseconds since{0};
        bool repeated = false;
    };
    /// Key code → what its press resolved to. Releases are matched by key code, so a release still reaches the
    /// command its press resolved to after modifiers or focus changed.
    std::map<int, Held> m_held;
    std::chrono::milliseconds m_holdThreshold;
};

} // namespace wabcut::keymap
