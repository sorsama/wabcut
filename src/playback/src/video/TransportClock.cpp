#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/SeqLock.h>
#include <wabcut/playback/video/TransportClock.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>

namespace wabcut::playback {

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace {

/// Steady-clock nanoseconds as a stream time base, for wabcut::rescale.
constexpr TimeBase kNanoseconds{1, 1'000'000'000};

/// anchor + speed * elapsed, in exact integer ticks (Principle 1: wabcut::rescale / mulDiv with explicit
/// rounding), never negative. Saturates instead of failing; only reachable centuries away from the anchor.
Ticks advance(std::int64_t anchorTicks, std::int64_t elapsedNs, TransportSpeed speed) {
    const Ticks elapsed = rescale(elapsedNs, kNanoseconds, Rounding::Floor)
                              .value_or(elapsedNs < 0 ? Ticks::min() : Ticks::max());
    const std::optional<Ticks> moved = mulDiv(elapsed, speed.num, speed.den, Rounding::Floor);
    const bool down = (elapsed.value < 0) != (speed.num < 0);
    const std::optional<Ticks> position =
        moved ? checkedAdd(Ticks{anchorTicks}, *moved) : std::optional<Ticks>{};
    const Ticks result = position.value_or(down ? Ticks{} : Ticks::max());
    return Ticks{std::max<std::int64_t>(0, result.value)};
}

std::uint64_t packSpeed(TransportSpeed s) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.num)) << 32) |
           static_cast<std::uint32_t>(s.den);
}

TransportSpeed unpackSpeed(std::uint64_t v) {
    return {static_cast<std::int32_t>(static_cast<std::uint32_t>(v >> 32)),
            static_cast<std::int32_t>(static_cast<std::uint32_t>(v & 0xffffffffu))};
}

// ---- Free-run ----

/// FreeRunTransport's published state. At namespace scope, not nested: SeqLock<State> checks that it is default
/// constructible inside the class, and GCC does not see a nested class's default member initializers until the
/// enclosing class is complete.
struct FreeRunState {
    std::int64_t anchorTicks = 0;
    std::int64_t anchorNs = 0;
    std::int32_t num = 1;
    std::int32_t den = 1;
    std::int32_t playing = 0;
    std::int32_t reserved = 0;
};

class FreeRunTransport final : public TransportClock {
public:
    explicit FreeRunTransport(Ticks position) {
        State s;
        s.anchorTicks = std::max<std::int64_t>(0, position.value);
        s.anchorNs = steadyNowNs();
        m_state.store(s);
    }

    Ticks transportTime(std::int64_t hostNs) const noexcept override {
        const State s = m_state.load();
        if (s.playing == 0) {
            return Ticks{s.anchorTicks};
        }
        return advance(s.anchorTicks, hostNs - s.anchorNs, {s.num, s.den});
    }

    TransportSpeed effectiveSpeed() const noexcept override {
        const State s = m_state.load();
        return s.playing != 0 ? TransportSpeed{s.num, s.den} : TransportSpeed::zero();
    }

    void play() override {
        update([](State& s) { s.playing = 1; });
    }
    void pause() override {
        update([](State& s) { s.playing = 0; });
    }
    void setSpeed(TransportSpeed speed) override {
        const TransportSpeed n = speed.normalized();
        if (n.isZero()) {
            return;
        }
        update([n](State& s) {
            s.num = n.num;
            s.den = n.den;
        });
    }
    void seek(Ticks target) override {
        std::lock_guard lock(m_writer);
        State s = m_state.load();
        s.anchorTicks = std::max<std::int64_t>(0, target.value);
        s.anchorNs = steadyNowNs();
        m_state.store(s);
    }
    std::string name() const override { return "free-run"; }

private:
    using State = FreeRunState;

    /// Re-anchors at "now" so the change is continuous, then applies `change`.
    template <class Fn> void update(Fn change) {
        std::lock_guard lock(m_writer);
        State s = m_state.load();
        const std::int64_t now = steadyNowNs();
        s.anchorTicks =
            s.playing != 0 ? advance(s.anchorTicks, now - s.anchorNs, {s.num, s.den}).value : s.anchorTicks;
        s.anchorNs = now;
        change(s);
        m_state.store(s);
    }

    std::mutex m_writer;
    audio::SeqLock<State> m_state;
};

// ---- Audio engine ----

class EngineTransport final : public TransportClock {
public:
    EngineTransport(audio::AudioEngine& engine, Ticks preroll) : m_engine(engine), m_preroll(preroll) {
        // AudioEngine::speed()/isPaused() lock; the commanded state is mirrored in atomics for lock-free
        // readers.
        m_speed.store(packSpeed(engine.speed().normalized().isZero() ? TransportSpeed::normal()
                                                                     : engine.speed().normalized()));
        m_playing.store(!engine.isPaused());
    }

    Ticks transportTime(std::int64_t hostNs) const noexcept override {
        const auto tp = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(hostNs)));
        const Ticks t = m_engine.clockAt(tp);
        return Ticks{std::max<std::int64_t>(0, t.value)};
    }

    TransportSpeed effectiveSpeed() const noexcept override {
        return m_playing.load(std::memory_order_acquire)
                   ? unpackSpeed(m_speed.load(std::memory_order_acquire))
                   : TransportSpeed::zero();
    }

    void play() override {
        m_engine.resume();
        m_playing.store(true, std::memory_order_release);
    }
    void pause() override {
        m_engine.pause();
        m_playing.store(false, std::memory_order_release);
    }
    void setSpeed(TransportSpeed speed) override {
        const TransportSpeed n = speed.normalized();
        if (n.isZero()) {
            return;
        }
        m_engine.setSpeed(n);
        m_speed.store(packSpeed(n), std::memory_order_release);
    }
    void seek(Ticks target) override {
        m_engine.seek(Ticks{std::max<std::int64_t>(0, target.value)}, m_preroll);
    }
    std::string name() const override {
        const std::string backend = m_engine.backendName();
        return backend.empty() ? std::string("audio engine") : "audio engine (" + backend + ")";
    }

private:
    audio::AudioEngine& m_engine;
    Ticks m_preroll;
    std::atomic<std::uint64_t> m_speed{0};
    std::atomic<bool> m_playing{false};
};

} // namespace

std::shared_ptr<TransportClock> makeEngineTransport(audio::AudioEngine& engine, Ticks seekPreroll) {
    return std::make_shared<EngineTransport>(engine, seekPreroll);
}

std::shared_ptr<TransportClock> makeFreeRunTransport(Ticks position) {
    return std::make_shared<FreeRunTransport>(position);
}

} // namespace wabcut::playback
