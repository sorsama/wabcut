// Real-time audit with plugins (A.6): the audio callback with plugin effects on a clip, a track and the master bus
// never allocates, locks or waits for a helper - through play, an edit of a plugin parameter, a seek, a loop and a
// plugin crash. Allocations made inside rt::ScopedRealtimeSection (the engine marks its callback) are counted by the
// global operator new below; locks by rt::CheckedMutex; waits by PluginHost::realtimeWaits().

#include "PlughostFixture.h"

#include "../playback/PlaybackFixture.h"

#include <wabcut/audio/RealTime.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>

namespace {
std::atomic<std::uint64_t> g_realtimeAllocations{0};

void* allocate(std::size_t size) {
    if (wabcut::audio::rt::inRealtimeSection()) {
        g_realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void* allocateAligned(std::size_t size, std::align_val_t alignment) {
    if (wabcut::audio::rt::inRealtimeSection()) {
        g_realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = _aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment))) {
        return p;
    }
    throw std::bad_alloc();
}
} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t a) { return allocateAligned(size, a); }
void* operator new[](std::size_t size, std::align_val_t a) { return allocateAligned(size, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

TEST_CASE("The callback never allocates, locks or waits with plugin effects playing, through edits, seeks, loops and a "
          "crash",
          "[plughost][rt]") {
    TestPlugins plugins;
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 330.0;
    tone.amplitude = 0.6f;
    p.media('A', tone);
    p.addTrack({{'A', Ticks{}, ms(3000), Ticks{}}});
    p.addTrack({{'A', ms(200), ms(2500), Ticks{}}});
    {
        Track a1 = p.track(0);
        a1.clips.mutableAt(0).setEffects({pluginEffect(plugins.clap(), 401, {{"p1", ParamValue(-3.0)}}, clapState(0.0, 64.0))});
        a1.effects = {pluginEffect(plugins.vst3(), 402, {{"p0", ParamValue(0.8)}})};
        p.state = Project::withTrack(p.state, 0, a1);
        Track a2 = p.track(1);
        a2.effects = {pluginEffect(plugins.clap(), 403, {{"p1", ParamValue(-1.0)}})};
        p.state = Project::withTrack(p.state, 1, a2);
        Sequence seq = *p.state.sequences.front();
        seq.audioEffects = {pluginEffect(plugins.clap(), 404, {{"p1", ParamValue(-1.0)}}, clapState(0.0, 32.0))};
        p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});
    const auto pace = [&](double milliseconds) {
        for (int i = 0; i < static_cast<int>(milliseconds / 10.0); ++i) {
            rig.pull();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();
    const std::uint64_t waitsBefore = plughost::PluginHost::realtimeWaits();

    rig.player->play();
    pace(300);
    // A plugin parameter edit (the master's gain; the track's): the master program swaps in the callback.
    {
        ProjectState next = *rig.session.snapshot();
        Sequence seq = *next.sequences.front();
        seq.audioEffects[0].set("p1", ParamValue(-4.0));
        next.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
        REQUIRE(rig.session.commit("master plugin gain", std::move(next)));
    }
    pace(200);
    {
        Track a2 = *rig.session.snapshot()->sequences.front()->audioTracks[1];
        a2.effects[0].set("p1", ParamValue(-6.0));
        REQUIRE(rig.session.commit("track plugin gain", Project::withTrack(*rig.session.snapshot(), 1, a2)));
    }
    pace(200);
    rig.player->seek(ms(1200));
    pace(200);
    rig.player->setLoop(TimeRange{ms(1300), ms(200)});
    pace(400);
    rig.player->setLoop(std::nullopt);
    // A crash of the track plugin's helper (the VST3 one, on its own process): bypassed from then on.
    {
        Track a1 = *rig.session.snapshot()->sequences.front()->audioTracks[0];
        a1.effects[0].set("p1", ParamValue(1.0)); // p1 is the VST3 test plugin's "Crash"
        REQUIRE(rig.session.commit("crash", Project::withTrack(*rig.session.snapshot(), 0, a1)));
    }
    pace(400);
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{1}}) {
        rig.pull(size);
    }

    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(plughost::PluginHost::realtimeWaits() == waitsBefore);
    CHECK(rig.engine.stats().callbacks > 100);
    const auto vst3 = plugins.effects->instanceFor(EffectInstanceId{402});
    REQUIRE(vst3);
    CHECK(vst3->health() == plughost::InstanceHealth::Crashed);
    double energy = 0.0;
    for (const float v : rig.stream) {
        energy += static_cast<double>(v) * v;
    }
    CHECK(energy > 1.0); // heard throughout, the crashed plugin bypassed
}
