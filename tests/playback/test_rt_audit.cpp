// Real-time audit: the audio callback (engine + timeline player graph) never allocates or locks.
//
// This test binary replaces the global allocation functions and counts every allocation made on a thread that
// is inside an rt::ScopedRealtimeSection (the engine marks its callback and pull() as one). Every mutex in
// wabcut_audio / wabcut_playback is an rt::CheckedMutex, which counts locks taken inside a section.

#include "PlaybackFixture.h"
#include "VoiceOverBooth.h"

#include <wabcut/audio/RealTime.h>
#include <wabcut/model/ProjectOps.h>

#include <atomic>
#include <cstdlib>
#include <new>

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
    const auto a = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    if (void* p = _aligned_malloc(size == 0 ? 1 : size, a)) {
        return p;
    }
#else
    if (void* p = std::aligned_alloc(a, ((size == 0 ? 1 : size) + a - 1) / a * a)) {
        return p;
    }
#endif
    throw std::bad_alloc();
}

void releaseAligned(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
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
void operator delete(void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

TEST_CASE("The audit hooks see allocations and locks inside a real-time section", "[playback][rt]") {
    const std::uint64_t allocations = g_realtimeAllocations.load();
    const std::uint64_t locks = audio::rt::lockViolations();
    {
        audio::rt::ScopedRealtimeSection section;
        auto* p = new int(42); // deliberately breaking the rules once, to prove the hooks work
        delete p;
        audio::rt::CheckedMutex m;
        m.lock();
        m.unlock();
    }
    CHECK(g_realtimeAllocations.load() == allocations + 1);
    CHECK(audio::rt::lockViolations() == locks + 1);
    auto* outside = new int(7); // outside a section: not counted
    delete outside;
    CHECK(g_realtimeAllocations.load() == allocations + 1);
}

TEST_CASE("The callback never allocates or locks through play, edits, seeks, loops, mute and shuttle",
          "[playback][rt]") {
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 440.0;
    p.media('A', tone);
    SyntheticSignal noise;
    noise.kind = SyntheticSignal::Kind::Noise;
    noise.amplitude = 0.2f;
    p.media('B', noise);
    p.addTrack({{'A', Ticks{}, ms(800), Ticks{}}, {'A', ms(900), ms(2000), Ticks::fromSeconds(1)}});
    p.addTrack({{'B', ms(100), ms(3000), Ticks{}, -3.0, 0.3}});
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});

    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();

    rig.player->play();
    rig.pullFor(300);
    Track edited = p.track(0);
    ClipProperties props = edited.clips[0].properties();
    props.set(ParamId::VolumeDb, -9.0);
    edited.clips.mutableAt(0).setProperties(props);
    edited.muted = false;
    REQUIRE(rig.session.commit("edit", Project::withTrack(*rig.session.snapshot(), 0, edited)));
    rig.pullFor(200);
    rig.player->seek(ms(1500));
    rig.pullFor(200);
    rig.player->setLoop(TimeRange{ms(1600), ms(120)});
    rig.pullFor(400);
    rig.player->setLoop(std::nullopt);
    Track muted = *rig.session.snapshot()->sequences.front()->audioTracks[1];
    muted.muted = true;
    REQUIRE(rig.session.commit("mute", Project::withTrack(*rig.session.snapshot(), 1, muted)));
    rig.pullFor(100);
    rig.player->setSpeed({2, 1});
    rig.pullFor(100);
    rig.player->setSpeed(audio::TransportSpeed::normal());
    rig.pullFor(100);
    rig.player->pause();
    rig.pullFor(50);
    rig.player->play();
    rig.pullFor(100);
    // Blocks of a different size, as devices deliver.
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{4096}, std::size_t{1}}) {
        rig.pull(size);
    }

    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(rig.engine.stats().callbacks > 100);
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("The callback never allocates or locks with effects running: clip, track and master stacks, master edits, "
          "and time-stretched shuttle",
          "[playback][rt][effects]") {
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 330.0;
    tone.amplitude = 0.9f;
    p.media('A', tone);
    SyntheticSignal noise;
    noise.kind = SyntheticSignal::Kind::Noise;
    noise.amplitude = 0.3f;
    p.media('B', noise);
    auto fx = [&](const char* id, std::vector<EffectParam> params) {
        EffectInstance e;
        e.id = p.state.ids.allocate<EffectInstanceId>();
        e.effect = id;
        for (EffectParam& param : params) {
            e.set(std::move(param.key), std::move(param.value));
        }
        return e;
    };
    p.addTrack({{'A', Ticks{}, ms(3000), Ticks{}}});
    p.addTrack({{'B', ms(100), ms(3000), Ticks{}, -3.0, 0.3}});
    {
        // Every clip effect on A1's clip (one of them animated), a compressor on A2, a limiter on the master.
        Track a1 = p.track(0);
        a1.clips.mutableAt(0).setEffects(
            {fx("wabcut.audio.eq3", {{"midGain", ParamValue(Curve{{Keyframe{ms(0), -12.0}, Keyframe{ms(2000), 6.0}}})}}),
             fx("wabcut.audio.compressor", {{"threshold", ParamValue(-30.0)}}), fx("wabcut.audio.gain", {{"gain", ParamValue(3.0)}}),
             fx("wabcut.audio.denoise", {}), fx("wabcut.audio.limiter", {{"ceiling", ParamValue(-6.0)}})});
        p.state = Project::withTrack(p.state, 0, a1);
        Track a2 = p.track(1);
        a2.effects = {fx("wabcut.audio.compressor", {{"ratio", ParamValue(8.0)}}), fx("wabcut.audio.denoise", {})};
        p.state = Project::withTrack(p.state, 1, a2);
        Sequence seq = *p.state.sequences.front();
        seq.audioEffects = {fx("wabcut.audio.eq3", {{"highGain", ParamValue(-3.0)}}), fx("wabcut.audio.limiter", {})};
        p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});

    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();

    rig.player->play();
    rig.pullFor(300);
    // Master edits while playing: a parameter change (same processors), then a new effect (a new program, new
    // latency) and its removal. The swaps and the retired programs never touch the audio thread's allocator.
    for (int step = 0; step < 3; ++step) {
        ProjectState next = *rig.session.snapshot();
        Sequence seq = *next.sequences.front();
        if (step == 0) {
            seq.audioEffects[0].set("highGain", ParamValue(-9.0));
        } else if (step == 1) {
            EffectInstance gain;
            gain.id = next.ids.allocate<EffectInstanceId>();
            gain.effect = "wabcut.audio.denoise";
            seq.audioEffects.push_back(gain);
        } else {
            seq.audioEffects.pop_back();
        }
        next.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
        REQUIRE(rig.session.commit("master", std::move(next)));
        rig.pullFor(150);
    }
    rig.player->seek(ms(1200));
    rig.pullFor(200);
    rig.player->setSpeed({2, 1});
    rig.pullFor(200);
    rig.player->setSpeed({4, 1});
    rig.pullFor(100);
    rig.player->setSpeed({1, 2});
    rig.pullFor(100);
    rig.player->setSpeed(audio::TransportSpeed::normal());
    rig.pullFor(100);
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{4096}, std::size_t{1}}) {
        rig.pull(size);
    }

    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(rig.engine.stats().callbacks > 100);
}

TEST_CASE("The callback never allocates or locks with Essential Sound's effects: filter, DeHum, DeEsser, expander, reverb, width",
          "[playback][rt][effects][essential]") {
    // A.7: the new effects on a clip, a track and the master (the reverb's delay lines are allocated when its processor is
    // built, off the audio thread), through a live parameter change, a new reverb on the master, a seek and a loop.
    Project p;
    SyntheticSignal noise;
    noise.kind = SyntheticSignal::Kind::Noise;
    noise.amplitude = 0.3f;
    p.media('A', noise);
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 220.0;
    tone.amplitude = 0.5f;
    p.media('B', tone);
    auto fx = [&](const char* id, std::vector<EffectParam> params) {
        EffectInstance e;
        e.id = p.state.ids.allocate<EffectInstanceId>();
        e.effect = id;
        for (EffectParam& param : params) {
            e.set(std::move(param.key), std::move(param.value));
        }
        return e;
    };
    p.addTrack({{'A', Ticks{}, ms(3000), Ticks{}}});
    p.addTrack({{'B', ms(100), ms(3000), Ticks{}, -3.0, 0.3}});
    {
        Track a1 = p.track(0);
        a1.clips.mutableAt(0).setEffects({fx("wabcut.audio.essentialSound", {}), fx("wabcut.audio.filter", {{"lowCut", ParamValue(80.0)}}),
                                          fx("wabcut.audio.dehum", {}), fx("wabcut.audio.expander", {}), fx("wabcut.audio.deess", {}),
                                          fx("wabcut.audio.reverb", {{"mix", ParamValue(Curve{{Keyframe{ms(0), 0.1}, Keyframe{ms(2000), 0.5}}})}})});
        p.state = Project::withTrack(p.state, 0, a1);
        Track a2 = p.track(1);
        a2.effects = {fx("wabcut.audio.width", {{"width", ParamValue(1.5)}, {"bassMono", ParamValue(120.0)}}), fx("wabcut.audio.reverb", {})};
        p.state = Project::withTrack(p.state, 1, a2);
        Sequence seq = *p.state.sequences.front();
        seq.audioEffects = {fx("wabcut.audio.filter", {{"highCut", ParamValue(12000.0)}}), fx("wabcut.audio.width", {})};
        p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});
    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();
    rig.player->play();
    rig.pullFor(300);
    for (int step = 0; step < 2; ++step) {
        ProjectState next = *rig.session.snapshot();
        Sequence seq = *next.sequences.front();
        if (step == 0) {
            seq.audioEffects[1].set("width", ParamValue(0.5));
        } else {
            EffectInstance reverb;
            reverb.id = next.ids.allocate<EffectInstanceId>();
            reverb.effect = "wabcut.audio.reverb";
            seq.audioEffects.push_back(reverb);
        }
        next.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
        REQUIRE(rig.session.commit("master", std::move(next)));
        rig.pullFor(150);
    }
    rig.player->seek(ms(1200));
    rig.pullFor(200);
    rig.player->setLoop(TimeRange{ms(1300), ms(400)});
    rig.pullFor(600);
    rig.player->setLoop(std::nullopt);
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{4096}, std::size_t{1}}) {
        rig.pull(size);
    }
    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(rig.engine.stats().callbacks > 100);
}

TEST_CASE("The callback never allocates or locks with redactions bleeping and muting, through their live edits and seeks",
          "[playback][rt][redaction]") {
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 220.0;
    tone.amplitude = 0.5f;
    p.media('A', tone);
    p.addTrack({{'A', Ticks{}, ms(3000), ms(250)}, {'A', ms(3000), ms(1000), ms(400)}});
    const auto redaction = [&](const ProjectState& state, Ticks start, Ticks duration, RedactionAudio mode) {
        Redaction r;
        r.media = p.mediaId('A');
        r.source = TimeRange{start, duration};
        r.audio = mode;
        r.audioRanges = {TimeRange{start, Ticks{duration.value / 2}}, TimeRange{start + Ticks{duration.value * 3 / 4}, Ticks{duration.value / 4}}};
        auto added = edit::addRedaction(state, r);
        REQUIRE(added);
        return std::move(added->state);
    };
    p.state = redaction(p.state, ms(400), ms(600), RedactionAudio::Bleep);
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});

    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();

    rig.player->play();
    rig.pullFor(300);
    // Edits while playing: another redaction (a new table, queued audio refilled), then its removal.
    REQUIRE(rig.session.commit("redact", redaction(*rig.session.snapshot(), ms(900), ms(300), RedactionAudio::Mute)));
    rig.pullFor(300);
    ProjectState fewer = *rig.session.snapshot();
    fewer.redactions.edit().pop_back(); // a SharedList (C.10): edit() makes it this snapshot's own
    REQUIRE(rig.session.commit("unredact", std::move(fewer)));
    rig.pullFor(200);
    rig.player->seek(ms(2900));
    rig.pullFor(300);
    rig.player->seek(ms(500));
    rig.pullFor(300);
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{4096}, std::size_t{1}}) {
        rig.pull(size);
    }

    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(rig.engine.stats().callbacks > 100);
    CHECK(rig.player->stats().underrunFrames == 0);
}
TEST_CASE("The callback never allocates or locks with fader, pan and master automation and the loudness tap on",
          "[playback][rt][faders]") {
    // A.3: the master and track fader curves are evaluated on the audio thread (published programs) and every block
    // goes into the loudness tap's ring. Edits to all three, live fader overrides (automation passes), seeks, shuttle.
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 440.0;
    tone.amplitude = 0.7f;
    p.media('A', tone);
    p.addTrack({{'A', Ticks{}, ms(4000), Ticks{}}});
    p.addTrack({{'A', ms(200), ms(3000), Ticks{}}});
    const auto keys = [](std::initializer_list<std::pair<Ticks, double>> list) {
        Curve c;
        for (const auto& [t, v] : list) {
            c.keys.push_back(Keyframe{t, v});
        }
        return ParamValue(std::move(c));
    };
    {
        Track a1 = p.track(0);
        a1.volume = keys({{ms(0), -20.0}, {ms(1000), 0.0}, {ms(3000), -6.0}});
        a1.pan = keys({{ms(0), -1.0}, {ms(2000), 1.0}});
        p.state = Project::withTrack(p.state, 0, a1);
        Sequence seq = *p.state.sequences.front();
        Keyframe bezier{ms(500), -3.0};
        bezier.interpolation = Interpolation::Bezier;
        Curve master;
        master.keys = {Keyframe{ms(0), 0.0}, bezier, Keyframe{ms(2500), -9.0}};
        seq.masterVolume = ParamValue(master);
        p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});

    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();

    rig.player->play();
    rig.pullFor(300);
    for (int step = 0; step < 4; ++step) {
        ProjectState next = *rig.session.snapshot();
        Sequence seq = *next.sequences.front();
        if (step % 2 == 0) {
            Track a2 = *seq.audioTracks[1];
            a2.volume = ParamValue(-3.0 * (step + 1));
            a2.pan = ParamValue(step == 0 ? 0.5 : -0.5);
            seq.audioTracks[1] = std::make_shared<const Track>(std::move(a2));
        } else {
            seq.masterVolume = ParamValue(-1.0 * step); // a new master program
        }
        next.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
        REQUIRE(rig.session.commit("fader", std::move(next)));
        rig.pullFor(120);
        (void)rig.player->loudness(); // the GUI reading the meter meanwhile
    }
    // An automation pass: live fader and pan moves on A1 and the master, every block, then released.
    for (int k = 0; k < 30; ++k) {
        REQUIRE(rig.player->setLiveTrackMix(p.track(0).id, -12.0 + 0.4 * k, k % 2 == 0 ? std::optional<double>(0.1 * (k % 10)) : std::nullopt));
        REQUIRE(rig.player->setLiveTrackMix(TrackId{}, -0.2 * k, std::nullopt));
        rig.pull();
    }
    rig.player->clearLiveMix();
    rig.pullFor(50);
    rig.player->resetLoudness();
    rig.player->seek(ms(1500));
    rig.pullFor(200);
    rig.player->setSpeed({2, 1});
    rig.pullFor(100);
    rig.player->setSpeed(audio::TransportSpeed::normal());
    rig.pullFor(200);
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{4096}, std::size_t{1}}) {
        rig.pull(size);
    }

    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(rig.player->loudness().frames > 0);
    CHECK(rig.engine.stats().callbacks > 100);
}

TEST_CASE("The callback never allocates or locks with nested and multicam clips playing, through nested edits, angle "
          "switches, seeks and loops",
          "[playback][rt][nested]") {
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 330.0;
    tone.amplitude = 0.6f;
    p.media('A', tone);
    SyntheticSignal noise;
    noise.kind = SyntheticSignal::Kind::Noise;
    noise.amplitude = 0.3f;
    p.media('B', noise);
    auto fx = [&](const char* id, std::vector<EffectParam> params) {
        EffectInstance e;
        e.id = p.state.ids.allocate<EffectInstanceId>();
        e.effect = id;
        for (EffectParam& param : params) {
            e.set(std::move(param.key), std::move(param.value));
        }
        return e;
    };
    auto track = [&](char media, Ticks start, Ticks duration, EffectStack effects = {}) {
        Track t;
        t.id = p.state.ids.allocate<TrackId>();
        t.kind = TrackKind::Audio;
        t.clips.push_back(p.makeClip({media, start, duration, Ticks{}}));
        t.effects = std::move(effects);
        return std::make_shared<const Track>(std::move(t));
    };
    // A nested sequence (a compressor on its first track, a limiter on its master) and a two-angle multicam source.
    Sequence nested;
    nested.id = p.state.ids.allocate<SequenceId>();
    nested.duration = ms(4000);
    nested.audioTracks = {track('A', Ticks{}, ms(4000), {fx("wabcut.audio.compressor", {{"threshold", ParamValue(-20.0)}})}),
                          track('B', ms(200), ms(3000))};
    nested.audioEffects = {fx("wabcut.audio.limiter", {{"ceiling", ParamValue(-3.0)}})};
    Sequence multicam;
    multicam.id = p.state.ids.allocate<SequenceId>();
    multicam.duration = ms(4000);
    multicam.audioTracks = {track('A', Ticks{}, ms(4000)), track('B', ms(50), ms(3950))};
    MulticamInfo info;
    info.sync = MulticamSync::Manual;
    info.angles = {MulticamAngle{"Camera 1", TrackId{}, {multicam.audioTracks[0]->id}, Ticks{}},
                   MulticamAngle{"Camera 2", TrackId{}, {multicam.audioTracks[1]->id}, ms(50)}};
    multicam.multicam = info;
    const SequenceId nestedId = nested.id;
    const SequenceId multicamId = multicam.id;
    p.state.sequences.push_back(std::make_shared<const Sequence>(std::move(nested)));
    p.state.sequences.push_back(std::make_shared<const Sequence>(std::move(multicam)));
    auto sequenceClip = [&](SequenceId seq, Ticks start, Ticks duration, Ticks sourceIn, AngleSet angles) {
        Clip c;
        c.id = p.state.ids.allocate<ClipId>();
        c.sequence = seq;
        c.stream = {StreamKind::Audio, 0};
        c.timelineRange = {start, duration};
        c.sourceIn = sourceIn;
        c.angles = angles;
        return c;
    };
    {
        Sequence top = *p.state.sequences.front();
        Track a1;
        a1.id = p.state.ids.allocate<TrackId>();
        a1.kind = TrackKind::Audio;
        a1.clips.push_back(sequenceClip(nestedId, Ticks{}, ms(3000), ms(100), AngleSet{}));
        Track a2;
        a2.id = p.state.ids.allocate<TrackId>();
        a2.kind = TrackKind::Audio;
        a2.clips.push_back(sequenceClip(multicamId, Ticks{}, ms(1000), ms(100), AngleSet::of(0)));
        a2.clips.push_back(sequenceClip(multicamId, ms(1000), ms(2000), ms(1100), AngleSet::of(1)));
        top.audioTracks = {std::make_shared<const Track>(std::move(a1)), std::make_shared<const Track>(std::move(a2))};
        p.state.sequences.front() = std::make_shared<const Sequence>(std::move(top));
    }
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});

    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();

    rig.player->play();
    rig.pullFor(300);
    // An edit inside the nested sequence (its second track turned down): the parent's nested clip refills.
    {
        ProjectState next = *rig.session.snapshot();
        Sequence seq = *next.findSequence(nestedId);
        Track t = *seq.audioTracks[1];
        t.volume = ParamValue(-9.0);
        seq.audioTracks[1] = std::make_shared<const Track>(std::move(t));
        REQUIRE(rig.session.commit("nested", next.withSequence(std::make_shared<const Sequence>(std::move(seq)))));
    }
    rig.pullFor(300);
    // An angle switch across the playhead: the second multicam clip plays both angles now.
    {
        Track a2 = *rig.session.snapshot()->sequences.front()->audioTracks[1];
        a2.clips.mutableAt(1).angles = AngleSet{0b11};
        REQUIRE(rig.session.commit("switch", Project::withTrack(*rig.session.snapshot(), 1, a2)));
    }
    rig.pullFor(300);
    rig.player->seek(ms(1800));
    rig.pullFor(200);
    rig.player->setLoop(TimeRange{ms(1900), ms(150)});
    rig.pullFor(400);
    rig.player->setLoop(std::nullopt);
    rig.player->pause();
    rig.pullFor(50);
    rig.player->play();
    rig.pullFor(100);
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{4096}, std::size_t{1}}) {
        rig.pull(size);
    }

    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(rig.engine.stats().callbacks > 100);
    CHECK(rig.player->stats().underrunFrames == 0);
    CHECK(rig.player->stats().feedErrors == 0);
    double energy = 0.0;
    for (const float v : rig.stream) {
        energy += static_cast<double>(v) * v;
    }
    CHECK(energy > 1.0); // the nested and multicam clips were heard
}

TEST_CASE("The callbacks never allocate or lock while recording a voice-over: count-in beeps, monitoring, capture, playback",
          "[playback][rt][voiceover]") {
    // A.8: the output callback runs the pass's source (program mute, beeps, the input monitor) over the player's graph;
    // the capture callback (push() here) only fills AudioInput's rings. The writer runs outside both (writeOnce).
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 440.0;
    p.media('A', tone);
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(30), Ticks{}}});
    Booth booth(p, {.outputLatency = 480,
                    .reportedInputLatency = 240,
                    .actualInputLatency = 240,
                    .speakersInRoom = true,
                    .channels = 2,
                    .voice = [](std::int64_t f) { return 0.1f * static_cast<float>(f % 7) / 7.0f; }});
    const auto dir = voiceOverScratch("rt");
    VoiceOverOptions options;
    options.punchIn = Ticks::fromSeconds(2);
    options.punchOut = Ticks::fromSeconds(3);
    options.preroll = ms(500);
    options.postroll = ms(250);
    options.countIn = 1;
    options.monitor = true;
    options.file = dir / "rt.wav";
    REQUIRE(booth.recorder->start(options));
    booth.step(); // the first block after start() (the pass's source settles in)
    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();
    REQUIRE(booth.runUntil(VoiceOverState::Recording, 3.0));
    booth.run(300);
    // Blocks of other sizes, as devices deliver.
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{1}}) {
        booth.step(size);
    }
    REQUIRE(booth.runUntil(VoiceOverState::Finished, 3.0));
    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    const auto result = booth.recorder->stop();
    REQUIRE(result);
    CHECK(result->frames == 48000);
    CHECK(result->droppedFrames == 0);
}

TEST_CASE("The callback never allocates or locks with a 5.1 master downmixed for a stereo device", "[playback][rt][channels]") {
    // A.9: the mix runs at 6 channels and the callback converts it to the device's 2 (ITU-R BS.775); channel maps, track
    // formats and the 5.1 panners render on the feeder. Live fader moves and a master switch while playing included.
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 330.0;
    tone.channelGains = {1.0f, 0.5f};
    p.media('A', tone);
    p.addTrack({{'A', Ticks{}, ms(3000), Ticks{}}});
    p.addTrack({{'A', ms(100), ms(2500), Ticks{}}});
    {
        Track a1 = p.track(0);
        ClipContent content = a1.clips[0].content();
        content.channels = AudioChannelMap::fill(1);
        a1.clips.mutableAt(0).setContent(std::move(content));
        p.state = Project::withTrack(p.state, 0, a1);
        Track a2 = p.track(1);
        a2.format = ChannelFormat::Mono;
        a2.surround = SurroundPan{ParamValue(0.4), ParamValue(-0.3), ParamValue(-10.0)};
        p.state = Project::withTrack(p.state, 1, a2);
        Sequence seq = *p.state.sequences.front();
        seq.masterFormat = ChannelFormat::Surround51;
        p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});
    rig.player->play();
    rig.pullFor(200);

    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();
    rig.pullFor(300);
    for (int k = 0; k < 20; ++k) {
        REQUIRE(rig.player->setLiveTrackMix(p.track(0).id, -6.0 + 0.2 * k, std::nullopt));
        rig.pull();
    }
    rig.player->clearLiveMix();
    rig.player->seek(ms(1200));
    rig.pullFor(200);
    rig.player->setSpeed({2, 1});
    rig.pullFor(100);
    rig.player->setSpeed(audio::TransportSpeed::normal());
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{1}}) {
        rig.pull(size);
    }
    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("The callback never allocates or locks with submixes and sends: routing edits, submix effects with latency, live submix faders",
          "[playback][rt][submix]") {
    // p02 audio: tracks assigned to submixes, pre- and post-fader sends, a submix chain with a lookahead limiter (latency
    // compensation delay lines), a mono submix, edits of the routing and of a submix's effects while playing.
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 330.0;
    tone.amplitude = 0.5f;
    p.media('A', tone);
    SyntheticSignal noise;
    noise.kind = SyntheticSignal::Kind::Noise;
    noise.amplitude = 0.3f;
    p.media('B', noise);
    auto fx = [&](const char* id, std::vector<EffectParam> params) {
        EffectInstance e;
        e.id = p.state.ids.allocate<EffectInstanceId>();
        e.effect = id;
        for (EffectParam& param : params) {
            e.set(std::move(param.key), std::move(param.value));
        }
        return e;
    };
    p.addTrack({{'A', Ticks{}, ms(3000), Ticks{}}});
    p.addTrack({{'B', ms(100), ms(3000), Ticks{}, -3.0, 0.3}});
    TrackId s1;
    TrackId s2;
    {
        Sequence seq = *p.state.sequences.front();
        Track bus;
        bus.id = p.state.ids.allocate<TrackId>();
        bus.kind = TrackKind::Audio;
        bus.effects = {fx("wabcut.audio.compressor", {{"threshold", ParamValue(-20.0)}}), fx("wabcut.audio.limiter", {})};
        s1 = bus.id;
        seq.submixTracks.push_back(std::make_shared<const Track>(bus));
        Track mono;
        mono.id = p.state.ids.allocate<TrackId>();
        mono.kind = TrackKind::Audio;
        mono.format = ChannelFormat::Mono;
        mono.effects = {fx("wabcut.audio.reverb", {{"mix", ParamValue(0.5)}})};
        mono.output = s1;
        s2 = mono.id;
        seq.submixTracks.push_back(std::make_shared<const Track>(mono));
        Track a1 = *seq.audioTracks[0];
        a1.output = s1;
        TrackSend send;
        send.submix = s2;
        send.level = ParamValue(-6.0);
        a1.sends.push_back(send);
        seq.audioTracks[0] = std::make_shared<const Track>(a1);
        Track a2 = *seq.audioTracks[1];
        send.preFader = true;
        a2.sends.push_back(send);
        seq.audioTracks[1] = std::make_shared<const Track>(a2);
        p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }
    Rig rig(p, Rig::Options{ms(2), ms(2), false, ms(30)});

    const std::uint64_t allocationsBefore = g_realtimeAllocations.load();
    const std::uint64_t locksBefore = audio::rt::lockViolations();

    rig.player->play();
    rig.pullFor(300);
    for (int step = 0; step < 4; ++step) {
        ProjectState next = *rig.session.snapshot();
        Sequence seq = *next.sequences.front();
        if (step == 0) { // a submix effect's parameter: same processors
            Track bus = *seq.submixTracks[0];
            bus.effects[0].set("threshold", ParamValue(-30.0));
            seq.submixTracks[0] = std::make_shared<const Track>(bus);
        } else if (step == 1) { // A2 assigned to the mono submix too
            Track a2 = *seq.audioTracks[1];
            a2.output = s2;
            seq.audioTracks[1] = std::make_shared<const Track>(a2);
        } else if (step == 2) { // the limiter goes: other latency, other delays
            Track bus = *seq.submixTracks[0];
            bus.effects.pop_back();
            seq.submixTracks[0] = std::make_shared<const Track>(bus);
        } else { // solo the mono submix
            Track mono = *seq.submixTracks[1];
            mono.solo = true;
            seq.submixTracks[1] = std::make_shared<const Track>(mono);
        }
        next.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
        REQUIRE(rig.session.commit("routing", std::move(next)));
        rig.pullFor(150);
    }
    REQUIRE(rig.player->setLiveTrackMix(s1, -6.0, 0.2));
    rig.pullFor(100);
    rig.player->clearLiveMix();
    rig.player->seek(ms(1200));
    rig.pullFor(200);
    rig.player->setLoop(TimeRange{ms(1300), ms(150)});
    rig.pullFor(300);
    rig.player->setLoop(std::nullopt);
    rig.player->setSpeed({2, 1});
    rig.pullFor(100);
    rig.player->setSpeed(audio::TransportSpeed::normal());
    for (const std::size_t size : {std::size_t{37}, std::size_t{1024}, std::size_t{4096}, std::size_t{1}}) {
        rig.pull(size);
    }
    CHECK(g_realtimeAllocations.load() == allocationsBefore);
    CHECK(audio::rt::lockViolations() == locksBefore);
    CHECK(rig.player->stats().underrunFrames == 0);
}