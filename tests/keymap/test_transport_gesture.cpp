#include <wabcut/keymap/KeyDispatcher.h>
#include <wabcut/keymap/TransportGesture.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>

using namespace std::chrono_literals;
using namespace wabcut::keymap;

namespace {

using Intents = std::vector<TransportIntent>;
using Cmds = std::vector<CommandId>;
using Role = TransportGesture::Role;

TransportIntent shuttle(int speed) {
    return TransportIntent::shuttleStep({speed, 1});
}
TransportIntent slow(int dir) {
    return TransportIntent::setSpeed({dir, 4});
}
TransportIntent step(int frames) {
    return TransportIntent::stepFrames(frames);
}
const TransportIntent kStop = TransportIntent::stop();

} // namespace

TEST_CASE("J and L climb the shuttle ladder, K stops", "[keymap][gesture]") {
    TransportGesture g;
    const Intents forward[] = {{shuttle(1)}, {shuttle(2)}, {shuttle(4)}, {shuttle(8)}, {shuttle(8)}};
    for (const Intents& expected : forward) {
        CHECK(g.press(Role::Forward, false, 0ms) == expected);
        CHECK(g.release(Role::Forward).empty());
    }
    // The other direction starts again at 1x that way.
    CHECK(g.press(Role::Backward, false, 0ms) == Intents{shuttle(-1)});
    CHECK(g.release(Role::Backward).empty());
    CHECK(g.press(Role::Backward, false, 0ms) == Intents{shuttle(-2)});
    CHECK(g.release(Role::Backward).empty());
    CHECK(g.press(Role::Forward, false, 0ms) == Intents{shuttle(1)});
    CHECK(g.release(Role::Forward).empty());
    CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
    CHECK(g.speed() == Rational{});
    CHECK(g.release(Role::Stop).empty());
    CHECK(g.press(Role::Forward, false, 0ms) == Intents{shuttle(1)});
}

TEST_CASE("OS auto-repeat never climbs the ladder", "[keymap][gesture]") {
    TransportGesture g;
    CHECK(g.press(Role::Forward, false, 0ms) == Intents{shuttle(1)});
    CHECK(g.press(Role::Forward, true, 500ms).empty());
    CHECK(g.press(Role::Forward, true, 530ms).empty());
    CHECK(g.release(Role::Forward).empty());
    CHECK(g.speed() == Rational{1, 1});
    CHECK(g.press(Role::Stop, false, 600ms) == Intents{kStop});
    CHECK(g.press(Role::Stop, true, 1100ms).empty()); // K repeat is ignored too
}

TEST_CASE("The ladder climbs from the transport's actual speed", "[keymap][gesture]") {
    TransportGesture g;
    g.syncSpeed({1, 1}); // Play/Stop started 1x playback
    CHECK(g.press(Role::Forward, false, 0ms) == Intents{shuttle(2)});
    CHECK(g.release(Role::Forward).empty());
    g.syncSpeed({3, 2}); // e.g. the transport bar set 1.5x
    CHECK(g.press(Role::Forward, false, 0ms) == Intents{shuttle(2)});
    CHECK(g.release(Role::Forward).empty());
    g.syncSpeed({}); // stopped elsewhere
    CHECK(g.press(Role::Backward, false, 0ms) == Intents{shuttle(-1)});
}

TEST_CASE("K held + tap J/L steps one frame", "[keymap][gesture]") {
    TransportGesture g;
    CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
    CHECK(g.press(Role::Backward, false, 50ms) == Intents{step(-1)});
    CHECK(g.release(Role::Backward).empty());
    CHECK(g.press(Role::Backward, false, 120ms) == Intents{step(-1)});
    CHECK(g.release(Role::Backward).empty());
    CHECK(g.press(Role::Forward, false, 200ms) == Intents{step(1)});
    CHECK(g.release(Role::Forward).empty());
    CHECK(g.tick(10s).empty()); // taps already released: nothing to promote
    CHECK(g.release(Role::Stop).empty());
    CHECK(g.slowDirection() == 0);
}

TEST_CASE("K+J / K+L held slow-plays at 1/4x until a key is released", "[keymap][gesture]") {
    TransportGesture g;
    SECTION("hold recognised by key repeat") {
        CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
        CHECK(g.press(Role::Backward, false, 50ms) == Intents{step(-1)});
        CHECK(g.press(Role::Backward, true, 550ms) == Intents{slow(-1)});
        CHECK(g.slowDirection() == -1);
        CHECK(g.speed() == Rational{-1, 4});
        CHECK(g.press(Role::Backward, true, 580ms).empty()); // further repeats change nothing
        CHECK(g.release(Role::Backward) == Intents{kStop});  // releasing J pauses
        CHECK(g.slowDirection() == 0);
        CHECK(g.isStopHeld());
        // K is still down: L can now step / slow-play forward.
        CHECK(g.press(Role::Forward, false, 700ms) == Intents{step(1)});
        CHECK(g.press(Role::Forward, true, 1200ms) == Intents{slow(1)});
        CHECK(g.release(Role::Stop) == Intents{kStop}); // releasing K also stops
        CHECK(g.release(Role::Forward).empty());
    }
    SECTION("hold recognised by tick() when key repeat is off") {
        CHECK(g.press(Role::Stop, false, 1000ms) == Intents{kStop});
        CHECK(g.press(Role::Forward, false, 1100ms) == Intents{step(1)});
        CHECK(g.tick(1100ms + TransportGesture::kDefaultHoldThreshold - 1ms).empty());
        CHECK(g.tick(1100ms + TransportGesture::kDefaultHoldThreshold) == Intents{slow(1)});
        CHECK(g.tick(5s).empty()); // already slow-playing
        CHECK(g.release(Role::Forward) == Intents{kStop});
    }
    SECTION("switching direction while K stays down") {
        CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
        CHECK(g.press(Role::Backward, false, 10ms) == Intents{step(-1)});
        CHECK(g.press(Role::Backward, true, 400ms) == Intents{slow(-1)});
        CHECK(g.press(Role::Forward, false, 450ms) == Intents{step(1)});
        CHECK(g.press(Role::Forward, true, 900ms) == Intents{slow(1)});
        CHECK(g.slowDirection() == 1);
        CHECK(g.release(Role::Backward).empty()); // the old direction's release no longer matters
        CHECK(g.release(Role::Forward) == Intents{kStop});
    }
}

TEST_CASE("Simultaneous releases stop exactly once", "[keymap][gesture]") {
    for (const bool stopFirst : {true, false}) {
        INFO("K released first: " << stopFirst);
        TransportGesture g;
        CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
        CHECK(g.press(Role::Forward, false, 10ms) == Intents{step(1)});
        CHECK(g.press(Role::Forward, true, 400ms) == Intents{slow(1)});
        const Role first = stopFirst ? Role::Stop : Role::Forward;
        const Role second = stopFirst ? Role::Forward : Role::Stop;
        CHECK(g.release(first) == Intents{kStop});
        CHECK(g.release(second).empty());
        CHECK_FALSE(g.isStopHeld());
        CHECK(g.speed() == Rational{});
    }
}

TEST_CASE("The gesture is K-first", "[keymap][gesture]") {
    TransportGesture g;
    CHECK(g.press(Role::Backward, false, 0ms) == Intents{shuttle(-1)});
    CHECK(g.press(Role::Stop, false, 100ms) == Intents{kStop}); // K while J is held: a plain stop
    CHECK(g.press(Role::Backward, true, 600ms).empty());        // J repeat does not start slow play
    CHECK(g.tick(10s).empty());
    // L tapped with K held steps; its release must not hand the hold to J, which went down before K.
    CHECK(g.press(Role::Forward, false, 11s) == Intents{step(1)});
    CHECK(g.release(Role::Forward).empty());
    CHECK(g.press(Role::Backward, true, 12s).empty());
    CHECK(g.tick(20s).empty());
    CHECK(g.release(Role::Backward).empty());
    CHECK(g.release(Role::Stop).empty());
}

TEST_CASE("Releasing a tapped J/L hands the hold back to the other held one", "[keymap][gesture]") {
    TransportGesture g;
    CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
    CHECK(g.press(Role::Backward, false, 10ms) == Intents{step(-1)});
    CHECK(g.press(Role::Forward, false, 50ms) == Intents{step(1)}); // L tapped while J is still down
    CHECK(g.release(Role::Forward).empty());
    CHECK(g.press(Role::Backward, true, 500ms) == Intents{slow(-1)}); // J's hold still promotes
    CHECK(g.release(Role::Backward) == Intents{kStop});
    CHECK(g.release(Role::Stop).empty());
}

TEST_CASE("Pressing stop during slow play stops it for good", "[keymap][gesture]") {
    TransportGesture g;
    CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
    CHECK(g.press(Role::Backward, false, 10ms) == Intents{step(-1)});
    CHECK(g.press(Role::Backward, true, 400ms) == Intents{slow(-1)});
    // A second stop key (or K pressed again after a lost release).
    CHECK(g.press(Role::Stop, false, 450ms) == Intents{kStop});
    CHECK(g.slowDirection() == 0);
    CHECK(g.press(Role::Backward, true, 480ms).empty()); // J held from before this K: no new hold
    CHECK(g.release(Role::Backward).empty());            // no second stop
}

TEST_CASE("Several keys can share the stop role", "[keymap][gesture]") {
    TransportGesture g;
    CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
    CHECK(g.press(Role::Stop, false, 5ms) == Intents{kStop}); // e.g. K and keypad 5
    CHECK(g.release(Role::Stop).empty());
    CHECK(g.isStopHeld()); // the other stop key is still down
    CHECK(g.press(Role::Forward, false, 20ms) == Intents{step(1)});
    CHECK(g.tick(20ms + TransportGesture::kDefaultHoldThreshold) == Intents{slow(1)});
    CHECK(g.release(Role::Stop) == Intents{kStop});
    CHECK_FALSE(g.isStopHeld());
    CHECK(g.release(Role::Stop).empty()); // an extra release is harmless
    CHECK_FALSE(g.isStopHeld());
}

TEST_CASE("reset() forgets held keys and stops only slow play", "[keymap][gesture]") {
    TransportGesture g;
    CHECK(g.press(Role::Stop, false, 0ms) == Intents{kStop});
    CHECK(g.press(Role::Forward, false, 10ms) == Intents{step(1)});
    CHECK(g.press(Role::Forward, true, 400ms) == Intents{slow(1)});
    CHECK(g.reset() == Intents{kStop});
    CHECK_FALSE(g.isStopHeld());
    CHECK(g.reset().empty());
    CHECK(g.press(Role::Forward, false, 1s) == Intents{shuttle(1)}); // back to plain shuttle
    CHECK(g.reset().empty()); // normal shuttle play is not stopped by focus loss
    CHECK(g.speed() == Rational{1, 1});
}

TEST_CASE("roleOf maps the shuttle and slow shuttle commands", "[keymap][gesture]") {
    CHECK(TransportGesture::roleOf("transport.shuttleBackward") == Role::Backward);
    CHECK(TransportGesture::roleOf("transport.shuttleStop") == Role::Stop);
    CHECK(TransportGesture::roleOf("transport.shuttleForward") == Role::Forward);
    // 0.4: Shuttle Slow Left / Right (Shift+J / Shift+L).
    CHECK(TransportGesture::roleOf("transport.shuttleSlowBackward") == Role::SlowBackward);
    CHECK(TransportGesture::roleOf("transport.shuttleSlowForward") == Role::SlowForward);
    CHECK_FALSE(TransportGesture::roleOf("transport.playStop"));
    CHECK_FALSE(TransportGesture::roleOf("transport.playAround"));
}

TEST_CASE("Shift+J and Shift+L latch slow play and step up to 1x", "[keymap][gesture]") {
    TransportGesture g;
    // From a stop: 1/4x, then 1/2x, then 1x, and 1x stays.
    CHECK(g.press(Role::SlowForward, false, 0ms) == Intents{TransportIntent::setSpeed({1, 4})});
    CHECK(g.press(Role::SlowForward, true, 500ms).empty()); // OS auto-repeat does not climb
    CHECK(g.release(Role::SlowForward).empty());
    CHECK(g.press(Role::SlowForward, false, 1s) == Intents{TransportIntent::setSpeed({1, 2})});
    CHECK(g.release(Role::SlowForward).empty());
    CHECK(g.press(Role::SlowForward, false, 2s) == Intents{TransportIntent::setSpeed({1, 1})});
    CHECK(g.release(Role::SlowForward).empty());
    CHECK(g.press(Role::SlowForward, false, 3s).empty()); // at 1x: no change
    CHECK(g.speed() == Rational{1, 1});
    // The other way: 1/4x backward at once.
    CHECK(g.press(Role::SlowBackward, false, 4s) == Intents{TransportIntent::setSpeed({-1, 4})});
    CHECK(g.release(Role::SlowBackward).empty());
    CHECK(g.press(Role::SlowBackward, false, 5s) == Intents{TransportIntent::setSpeed({-1, 2})});
    // Faster than 1x that way (J J): Shift+J changes nothing.
    g.syncSpeed({-2, 1});
    CHECK(g.press(Role::SlowBackward, false, 6s).empty());
    // L after slow play goes to 1x, then up the ladder.
    g.syncSpeed({1, 4});
    CHECK(g.press(Role::Forward, false, 7s) == Intents{shuttle(1)});
    CHECK(g.release(Role::Forward).empty());
    CHECK(g.press(Role::Forward, false, 8s) == Intents{shuttle(2)});
}

TEST_CASE("Releasing Shift+L does not stop latched slow play and K does", "[keymap][gesture]") {
    TransportGesture g;
    CHECK(g.press(Role::SlowForward, false, 0ms) == Intents{TransportIntent::setSpeed({1, 4})});
    CHECK(g.release(Role::SlowForward).empty()); // latched: still playing
    CHECK(g.slowDirection() == 0);               // not the K+L hold
    CHECK(g.tick(2s).empty());
    CHECK(g.reset().empty()); // focus loss keeps latched play going, as with L
    CHECK(g.speed() == Rational{1, 4});
    CHECK(g.press(Role::Stop, false, 3s) == Intents{kStop});
    CHECK(g.release(Role::Stop).empty());
    CHECK(g.speed() == Rational{});
    // A slow press with K held still latches (it is not part of the K-first gesture).
    CHECK(g.press(Role::Stop, false, 4s) == Intents{kStop});
    CHECK(g.press(Role::SlowBackward, false, 4100ms) == Intents{TransportIntent::setSpeed({-1, 4})});
    CHECK(g.release(Role::SlowBackward).empty());
    CHECK(g.release(Role::Stop).empty()); // K's release ends only K+J/L slow play
}

// ---- KeyDispatcher: press/release routing over the real default keymap ----

namespace {

/// A manual clock: presses and ticks are timed with the same source.
struct FakeClock {
    std::chrono::milliseconds now{0};
    KeyDispatcher::Clock fn() {
        return [this] { return now; };
    }
};

} // namespace

TEST_CASE("KeyDispatcher drives the JKL gesture from raw key events", "[keymap][dispatcher]") {
    const auto keymap = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(keymap.has_value());
    FakeClock clock;
    KeyDispatcher d(*keymap, clock.fn());

    KeyResult r = d.keyPress(Scope::Timeline, {Key_L}, false);
    CHECK(r.handled);
    CHECK(r.commands.empty());
    CHECK(r.transport == Intents{shuttle(1)});
    CHECK(d.keyRelease({Key_L}, false).handled);

    clock.now = 100ms;
    CHECK(d.keyPress(Scope::Timeline, {Key_K}, false).transport == Intents{kStop});
    CHECK(d.isGestureActive());
    clock.now = 150ms;
    CHECK(d.keyPress(Scope::Timeline, {Key_J}, false).transport == Intents{step(-1)});
    CHECK(d.keyRelease({Key_J}, true).empty()); // synthetic auto-repeat release
    clock.now = 650ms;
    CHECK(d.keyPress(Scope::Timeline, {Key_J}, true).transport == Intents{slow(-1)});
    CHECK(d.keyRelease({Key_J}, false).transport == Intents{kStop});
    CHECK(d.keyRelease({Key_K}, false).empty());
    CHECK_FALSE(d.isGestureActive());

    // tick() path: the hold is measured on the dispatcher's own clock.
    clock.now = 1000ms;
    CHECK(d.keyPress(Scope::ProgramMonitor, {Key_K}, false).transport == Intents{kStop});
    clock.now = 1010ms;
    CHECK(d.keyPress(Scope::ProgramMonitor, {Key_L}, false).transport == Intents{step(1)});
    clock.now = 1100ms;
    CHECK(d.tick().empty()); // a tap so far
    clock.now = 1400ms;
    CHECK(d.tick() == Intents{slow(1)});
    CHECK(d.focusLost() == Intents{kStop}); // window deactivated: the releases will never come
    CHECK_FALSE(d.isGestureActive());
}

TEST_CASE("KeyDispatcher recovers from a lost key release", "[keymap][dispatcher]") {
    const auto keymap = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(keymap.has_value());
    FakeClock clock;
    KeyDispatcher d(*keymap, clock.fn());
    CHECK(d.keyPress(Scope::Timeline, {Key_K}, false).transport == Intents{kStop});
    // K's release is lost (e.g. it happened in another window); K goes down again.
    clock.now = 5s;
    CHECK(d.keyPress(Scope::Timeline, {Key_K}, false).transport == Intents{kStop});
    CHECK(d.keyRelease({Key_K}, false).transport.empty());
    CHECK_FALSE(d.isGestureActive()); // not latched by the lost release
    CHECK(d.keyPress(Scope::Timeline, {Key_J}, false).transport == Intents{shuttle(-1)});
}

TEST_CASE("KeyDispatcher applies the auto-repeat policy and scope routing", "[keymap][dispatcher]") {
    const auto keymap = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(keymap.has_value());
    KeyDispatcher d(*keymap); // real steady clock

    // Frame stepping repeats while the arrow is held; Play/Stop does not, but the key is still consumed.
    CHECK(d.keyPress(Scope::Timeline, {Key_Right}, false).commands == Cmds{"transport.stepForward"});
    CHECK(d.keyPress(Scope::Timeline, {Key_Right}, true).commands == Cmds{"transport.stepForward"});
    CHECK(d.keyRelease({Key_Right}, false).handled);
    CHECK(d.keyPress(Scope::Timeline, {Key_Space}, false).commands == Cmds{"transport.playStop"});
    const KeyResult repeat = d.keyPress(Scope::Timeline, {Key_Space}, true);
    CHECK(repeat.handled);
    CHECK(repeat.empty());
    CHECK(d.keyRelease({Key_Space}, false).handled);

    // Panel scope is honoured, and a repeat keeps the command its press resolved to.
    CHECK(d.keyPress(Scope::History, {Key_Left}, false).commands == Cmds{"history.stepBackward"});
    CHECK(d.keyPress(Scope::Timeline, {Key_Left}, true).commands == Cmds{"history.stepBackward"});
    CHECK(d.keyRelease({Key_Left}, false).handled);
    CHECK(d.keyPress(Scope::Timeline, {Key_Left}, false).commands == Cmds{"transport.stepBackward"});
    CHECK(d.keyRelease({Key_Left}, false).handled);

    // Qt quirks: Shift+3 arrives as Shift+'#'.
    CHECK(d.keyPress(Scope::Project, KeyChord::fromQt(Key_NumberSign, 0x02000000), false).commands ==
          Cmds{"window.timeline"});
    // Bare modifiers and unbound keys produce nothing and are not handled.
    CHECK_FALSE(d.keyPress(Scope::Timeline, {Key_Shift, Modifier::Shift}, false).handled);
    CHECK_FALSE(d.keyPress(Scope::Timeline, {Key_F9}, false).handled);
    CHECK_FALSE(d.keyRelease({Key_F9}, false).handled);
    CHECK_FALSE(d.keyRelease({Key_F10}, false).handled); // release without a press
    CHECK(d.tick().empty());
}

TEST_CASE("KeyDispatcher follows rebound shuttle keys", "[keymap][dispatcher]") {
    auto keymap = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(keymap.has_value());
    REQUIRE(keymap->setChords("transport.shuttleStop", {KeyChord{Key_5, Modifier::Keypad}}));
    KeyDispatcher d(*keymap);
    CHECK_FALSE(d.keyPress(Scope::Timeline, {Key_K}, false).handled);
    CHECK(d.keyPress(Scope::Timeline, {Key_5, Modifier::Keypad}, false).transport == Intents{kStop});
    CHECK(d.keyPress(Scope::Timeline, {Key_L}, false).transport == Intents{step(1)});
    CHECK(d.keyRelease({Key_5, Modifier::Keypad}, false).handled);
}

TEST_CASE("trigger() runs commands for menus and buttons", "[keymap][dispatcher]") {
    const auto keymap = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(keymap.has_value());
    KeyDispatcher d(*keymap);
    CHECK(d.trigger("edit.undo").commands == Cmds{"edit.undo"});
    CHECK(d.trigger("transport.shuttleForward").transport == Intents{shuttle(1)});
    CHECK(d.trigger("transport.shuttleForward").transport == Intents{shuttle(2)});
    CHECK(d.trigger("transport.shuttleStop").transport == Intents{kStop});
    CHECK_FALSE(d.isGestureActive()); // a tap leaves nothing held
    CHECK_FALSE(d.trigger("no.such").handled);
    d.syncTransportSpeed({2, 1});
    CHECK(d.trigger("transport.shuttleForward").transport == Intents{shuttle(4)});
}

TEST_CASE("KeyDispatcher drives slow shuttle from Shift+J and Shift+L", "[keymap][dispatcher]") {
    for (const Platform platform : {Platform::Windows, Platform::MacOS}) {
        INFO(platformId(platform));
        const auto keymap = KeymapManager::withDefaults(platform);
        REQUIRE(keymap.has_value());
        FakeClock clock;
        KeyDispatcher d(*keymap, clock.fn());
        // Shift+L: 1/4x, latched past the release; the next press 1/2x. No command reaches the app: transport only.
        KeyResult r = d.keyPress(Scope::Timeline, {Key_L, Modifier::Shift}, false);
        CHECK(r.handled);
        CHECK(r.commands.empty());
        CHECK(r.transport == Intents{TransportIntent::setSpeed({1, 4})});
        CHECK(d.keyRelease({Key_L, Modifier::Shift}, false).transport.empty());
        clock.now = 300ms;
        CHECK(d.keyPress(Scope::SourceMonitor, {Key_L, Modifier::Shift}, false).transport ==
              Intents{TransportIntent::setSpeed({1, 2})});
        CHECK(d.keyRelease({Key_L, Modifier::Shift}, false).transport.empty());
        CHECK(d.focusLost().empty()); // latched play is not the K+J/L hold
        // Shift+J turns round at 1/4x; K stops.
        CHECK(d.keyPress(Scope::Timeline, {Key_J, Modifier::Shift}, false).transport ==
              Intents{TransportIntent::setSpeed({-1, 4})});
        CHECK(d.keyRelease({Key_J, Modifier::Shift}, false).transport.empty());
        CHECK(d.keyPress(Scope::Timeline, {Key_K}, false).transport == Intents{kStop});
        CHECK(d.keyRelease({Key_K}, false).transport.empty());
        // The palette and menus (trigger: press and release at once) latch too.
        CHECK(d.trigger("transport.shuttleSlowForward").transport == Intents{TransportIntent::setSpeed({1, 4})});
        CHECK_FALSE(d.isGestureActive());
    }
}
