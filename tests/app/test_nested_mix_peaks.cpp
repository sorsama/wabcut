// 0.5 (P0 round, stream A, phase 2): exact waveforms of nested-sequence, multicam and merged audio clips. The mix such a
// clip plays is rendered in the background (NestedMixPeaks: playback::OfflineMixdown over the real media, kept as 2 ms
// peaks), cached by its sequence and a stamp of what it depends on; an edit renders it again and stops the stale render;
// until it is complete (and when it fails) the approximation from the leaf clips stays, shaded as building, with the part
// rendered so far already exact. Real media: the generated clips of tools/gen_test_media.py (skipped when missing).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "NestedMixPeaks.h"
#include "NestedPeaks.h"
#include "PeakSource.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/media/Probe.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

Ticks seconds(double s) {
    return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))};
}

std::string testMedia(const char* name) {
    const std::u8string p = (std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name).generic_u8string();
    return std::string(reinterpret_cast<const char*>(p.data()), p.size());
}

bool haveTestMedia(const char* name) {
    return std::filesystem::is_regular_file(std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name);
}

/// `state` with media `id` played from `path` (probed: its real streams and length).
ProjectState withFile(ProjectState state, MediaId id, const std::string& path) {
    for (MediaPtr& m : state.media) {
        if (m->id != id)
            continue;
        auto copy = std::make_shared<MediaItem>(*m);
        copy->locator.path = path;
        copy->locator.offline = false;
        if (const auto info = media::probe(path))
            copy->info = *info;
        m = copy;
    }
    return state;
}

/// A level per media item (the approximation's peaks).
class LevelPeaks final : public PeakSource {
public:
    bool peaks(MediaId media, StreamRef, Ticks start, Ticks end, std::span<float> out) const override {
        const auto it = level.find(media.value);
        if (it == level.end() || end <= start)
            return false;
        std::fill(out.begin(), out.end(), it->second);
        return true;
    }
    std::map<std::uint64_t, float> level;
};

/// The clips at frame 0 of every track of `seq` nested; the parent's nested audio clip is on A1.
ProjectState nestAll(const ProjectState& state, SequenceId seq) {
    std::vector<ClipId> clips;
    state.findSequence(seq)->forEachTrack([&](const Track& track) {
        if (!track.clips.empty() && track.clips[0].start() == Ticks{})
            clips.push_back(track.clips[0].id);
    });
    auto nested = edit::nestClips(state, seq, clips);
    INFO((nested ? std::string() : nested.error().message));
    REQUIRE(nested);
    return std::move(nested->edit.state);
}

Clip audioNest(const ProjectState& state, SequenceId seq) {
    const Sequence* parent = state.findSequence(seq);
    REQUIRE(parent);
    REQUIRE_FALSE(parent->audioTracks[0]->clips.empty());
    const Clip clip = parent->audioTracks[0]->clips[0];
    REQUIRE(clip.isNested());
    return clip;
}

/// Waits (events processed) until the mix of `clip` is no longer pending.
NestedMixSource::Lookup waitForMix(const NestedMixPeaks& mixes, const Clip& clip, const ProjectSnapshot& project) {
    NestedMixSource::Lookup found;
    REQUIRE(waitFor([&] {
        found = mixes.lookup(clip, project);
        return found.state != NestedMixSource::State::Pending;
    }, 20000));
    return found;
}

/// A renderer of a constant level (or `spike` at one time) for the request's length (or `length`), quickly.
NestedMixPeaks::Renderer constantRenderer(float level, std::optional<Ticks> spikeAt = std::nullopt, float spike = 0.0f,
                                          std::optional<Ticks> length = std::nullopt) {
    return [=](const NestedMixPeaks::Request& request, const NestedMixPeaks::Sink& sink, std::stop_token stop) {
        const std::int64_t perFrame = Rate{MixPeaks::kSampleRate, 1}.ticksPerUnit();
        const std::int64_t frames = (length.value_or(request.length).value + perFrame - 1) / perFrame;
        const std::int64_t spikeFrame = spikeAt ? spikeAt->value / perFrame : -1;
        constexpr std::int64_t kBlock = 16384;
        std::vector<float> block(kBlock * 2);
        for (std::int64_t done = 0; done < frames; done += kBlock) {
            if (stop.stop_requested())
                return false;
            const std::int64_t n = std::min(kBlock, frames - done);
            for (std::int64_t i = 0; i < n; ++i) {
                const float v = done + i == spikeFrame ? spike : (i % 2 == 0 ? level : -level);
                block[static_cast<std::size_t>(2 * i)] = v;
                block[static_cast<std::size_t>(2 * i + 1)] = -v;
            }
            if (!sink(std::span<const float>(block.data(), static_cast<std::size_t>(2 * n)), 2))
                return false;
        }
        return true;
    };
}

} // namespace

TEST_CASE("Nested mix peaks render the mix a nested clip plays from real media with its gains", "[app][timeline][waveform][nested][media]") {
    if (!haveTestMedia("sine_48k.wav"))
        SKIP("test media missing (tools/gen_test_media.py)");
    Timeline tl(R"(
        V1 tgt |.........................|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa.....|
    )");
    // A: 5 s of a 1 kHz sine at 0.5 (left) and a 1.5 kHz one at 0.25 (right).
    const ProjectState real = withFile(tl.state, tl.media('A'), testMedia("sine_48k.wav"));
    const ProjectState nested = nestAll(real, tl.seq);
    const Clip nest = audioNest(nested, tl.seq);
    LevelPeaks peaks;
    peaks.level[tl.media('A').value] = 0.9f; // what the approximation says
    NestedMixPeaks mixes;
    auto snap = std::make_shared<const ProjectState>(nested);

    // First the approximation, shaded as building; then the rendered mix: the sine's 0.5.
    std::vector<float> out(16);
    std::vector<std::uint8_t> pending;
    NestedPeaksResult r = nestedPeaks(nest, *snap, peaks, nest.start(), nest.end(), out, &pending, &mixes, snap);
    CHECK_FALSE(r.exact);
    CHECK(r.building);
    CHECK(out[3] == 0.9f);
    const NestedMixSource::Lookup ready = waitForMix(mixes, nest, snap);
    REQUIRE(ready.state == NestedMixSource::State::Ready);
    REQUIRE(ready.peaks);
    CHECK(ready.peaks->complete);
    CHECK(ready.peaks->reach() >= f(20));
    r = nestedPeaks(nest, *snap, peaks, nest.start(), nest.end(), out, &pending, &mixes, snap);
    CHECK(r.exact);
    CHECK_FALSE(r.building);
    const float full = out[3];
    CHECK(full > 0.4f);
    CHECK(full < 0.6f);
    CHECK(std::ranges::all_of(pending, [](std::uint8_t p) { return p == 0; }));

    // The leaf at -6 dB inside the nested sequence: another stamp, rendered again, half as loud.
    const Sequence* inner = snap->findSequence(nest.sequence);
    REQUIRE(inner);
    const ClipId leaf = inner->audioTracks[0]->clips[0].id;
    auto quieter = edit::setParameter(nested, nest.sequence, leaf, edit::ParamRef::of(ParamId::VolumeDb), ParamValue(-6.0));
    REQUIRE(quieter);
    CHECK(nestedMixStamp(quieter->state, nest.sequence) != nestedMixStamp(nested, nest.sequence));
    auto snap2 = std::make_shared<const ProjectState>(quieter->state);
    r = nestedPeaks(nest, *snap2, peaks, nest.start(), nest.end(), out, &pending, &mixes, snap2);
    CHECK_FALSE(r.exact); // the approximation again while it renders
    REQUIRE(waitForMix(mixes, nest, snap2).state == NestedMixSource::State::Ready);
    r = nestedPeaks(nest, *snap2, peaks, nest.start(), nest.end(), out, nullptr, &mixes, snap2);
    CHECK(r.exact);
    CHECK(std::abs(out[3] / full - 0.501f) < 0.05f);
    CHECK(mixes.stats().completed == 2);
    CHECK(mixes.stats().failed == 0);

    // A clip at another speed is silent (nested sound plays at 100 % only): flat, though the mix is there.
    Clip slow = nest;
    slow.speed = Speed{1, 2};
    r = nestedPeaks(slow, *snap2, peaks, slow.start(), slow.end(), out, nullptr, &mixes, snap2);
    CHECK(std::ranges::all_of(out, [](float v) { return v == 0.0f; }));
}

TEST_CASE("Nested mix peaks of a multicam clip render its angles only from real media", "[app][timeline][waveform][nested][media]") {
    if (!haveTestMedia("sine_48k.wav") || !haveTestMedia("ramp_48k.wav"))
        SKIP("test media missing (tools/gen_test_media.py)");
    Timeline tl(R"(
        V1 tgt |....................|
        A1 tgt |....................|
    )");
    ProjectState state = withFile(tl.state, tl.media('A'), testMedia("sine_48k.wav")); // peaks 0.5
    state = withFile(std::move(state), tl.media('B'), testMedia("ramp_48k.wav"));      // a full-scale ramp
    edit::MulticamSpec spec;
    spec.name = "Two mics";
    spec.angles = {tl.media('A'), tl.media('B')};
    spec.sync = MulticamSync::InPoints;
    auto made = edit::createMulticam(state, spec);
    INFO((made ? std::string() : made.error().message));
    REQUIRE(made);
    auto snap = std::make_shared<const ProjectState>(made->state);
    Clip clip;
    clip.id = ClipId{910001};
    clip.sequence = made->id;
    clip.stream = StreamRef{StreamKind::Audio, 0};
    clip.timelineRange = TimeRange{Ticks{}, f(20)};
    clip.angles = AngleSet::of(0);
    LevelPeaks none;
    NestedMixPeaks mixes;
    std::vector<float> out(8);
    REQUIRE(waitForMix(mixes, clip, snap).state == NestedMixSource::State::Ready);
    REQUIRE(nestedPeaks(clip, *snap, none, clip.start(), clip.end(), out, nullptr, &mixes, snap).exact);
    CHECK(out[4] > 0.4f);
    CHECK(out[4] < 0.6f);
    // The other angle is another mix.
    clip.angles = AngleSet::of(1);
    REQUIRE(waitForMix(mixes, clip, snap).state == NestedMixSource::State::Ready);
    REQUIRE(nestedPeaks(clip, *snap, none, clip.start(), clip.end(), out, nullptr, &mixes, snap).exact);
    // The ramp starts at -1 and climbs: loud at first, |-0.41| at 0.4 s.
    CHECK(out[0] > 0.85f);
    CHECK(std::abs(out[4] - 0.41f) < 0.05f);
    CHECK(mixes.stats().entries == 2);
}

TEST_CASE("Nested mix peaks fall back to the approximation when media cannot be read", "[app][timeline][waveform][nested]") {
    Timeline tl(R"(
        V1 tgt |....................|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    const ProjectState missing = withFile(tl.state, tl.media('A'), testMedia("no-such-recording.wav"));
    const ProjectState nested = nestAll(missing, tl.seq);
    const Clip nest = audioNest(nested, tl.seq);
    auto snap = std::make_shared<const ProjectState>(nested);
    LevelPeaks peaks;
    peaks.level[tl.media('A').value] = 0.3f;
    NestedMixPeaks mixes;
    CHECK(waitForMix(mixes, nest, snap).state == NestedMixSource::State::Failed);
    CHECK(mixes.stats().failed == 1);
    std::vector<float> out(10);
    std::vector<std::uint8_t> pending;
    const NestedPeaksResult r = nestedPeaks(nest, *snap, peaks, nest.start(), nest.end(), out, &pending, &mixes, snap);
    CHECK(r.drawn);
    CHECK_FALSE(r.exact);
    CHECK_FALSE(r.building);
    CHECK(out[5] == 0.3f);
    // Not tried again for the same content.
    CHECK(mixes.lookup(nest, snap).state == NestedMixSource::State::Failed);
    CHECK(mixes.stats().started == 1);
}

TEST_CASE("Nested mix peaks publish the rendered start early and stop a render of stale content", "[app][timeline][waveform][nested]") {
    Timeline tl(R"(
        V1 tgt |....................................................................................................|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    const ProjectState nested = nestAll(tl.state, tl.seq);
    const Clip nest = audioNest(nested, tl.seq); // 4 s
    auto snap = std::make_shared<const ProjectState>(nested);
    LevelPeaks peaks;
    peaks.level[tl.media('A').value] = 0.3f;
    NestedMixPeaks mixes;
    // The first render goes slowly (1 s of 0.7 every 150 ms) until it is stopped; any later one is quick, at 0.4.
    std::atomic<int> calls = 0;
    std::atomic<bool> firstStopped = false;
    const NestedMixPeaks::Renderer quick = constantRenderer(0.4f);
    mixes.setRenderer([&](const NestedMixPeaks::Request& request, const NestedMixPeaks::Sink& sink, std::stop_token stop) {
        if (++calls > 1)
            return quick(request, sink, stop);
        std::vector<float> second(static_cast<std::size_t>(MixPeaks::kSampleRate) * 2);
        for (std::size_t i = 0; i < second.size(); ++i)
            second[i] = i % 2 == 0 ? 0.7f : -0.7f;
        for (int s = 0; s < 400 && !stop.stop_requested(); ++s) {
            // 2 s rendered, then it waits (empty blocks: the service publishes and says when to stop).
            if (!sink(s < 2 ? std::span<const float>(second) : std::span<const float>(), 2))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        firstStopped = stop.stop_requested();
        return false;
    });

    // While it renders: the first two seconds exact (0.7), the rest the approximation (0.3), shaded.
    std::vector<float> out(8);
    std::vector<std::uint8_t> pending;
    REQUIRE(waitFor([&] {
        const NestedMixSource::Lookup l = mixes.lookup(nest, snap);
        return l.state == NestedMixSource::State::Pending && l.peaks && l.peaks->reach() >= Ticks::fromSeconds(2);
    }, 10000));
    NestedPeaksResult r = nestedPeaks(nest, *snap, peaks, nest.start(), nest.end(), out, &pending, &mixes, snap);
    CHECK_FALSE(r.exact);
    CHECK(r.building);
    CHECK(std::abs(out[1] - 0.7f) < 0.01f);
    CHECK(pending[1] == 0);
    CHECK(out[6] == 0.3f);
    CHECK(pending[6] == 1);

    // An edit inside: the stale render stops, the new content renders.
    const Sequence* inner = snap->findSequence(nest.sequence);
    edit::TrackStateChange rename;
    rename.name = "Game";
    auto edited = edit::setTrackState(nested, nest.sequence, inner->audioTracks[0]->id, rename);
    REQUIRE(edited);
    auto snap2 = std::make_shared<const ProjectState>(edited->state);
    REQUIRE(nestedMixStamp(*snap2, nest.sequence) != nestedMixStamp(*snap, nest.sequence));
    (void)mixes.lookup(nest, snap2);
    REQUIRE(waitFor([&] { return firstStopped.load(); }, 10000));
    const NestedMixSource::Lookup ready = waitForMix(mixes, nest, snap2);
    REQUIRE(ready.state == NestedMixSource::State::Ready);
    CHECK(std::abs(ready.peaks->peak(Ticks{}, Ticks::fromSeconds(4)) - 0.4f) < 0.01f);
    CHECK(mixes.stats().cancelled == 1);
    CHECK(mixes.stats().completed == 1);
}

TEST_CASE("Nested mix peaks keep 2 ms peaks and an hour of mix in a few megabytes", "[app][timeline][waveform][nested]") {
    // The peak of a 2 ms window survives at any zoom (the coarse level is exact too).
    Timeline tl(R"(
        V1 tgt |....................|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    const ProjectState state = nestAll(tl.state, tl.seq);
    const Clip nest = audioNest(state, tl.seq);
    auto snap = std::make_shared<const ProjectState>(state);
    NestedMixPeaks mixes;
    // An hour of a quiet mix with one click at 30:00.
    mixes.setRenderer(constantRenderer(0.05f, Ticks::fromSeconds(1800), 0.9f, Ticks::fromSeconds(3600)));
    const NestedMixSource::Lookup mix = waitForMix(mixes, nest, snap);
    REQUIRE(mix.state == NestedMixSource::State::Ready);
    REQUIRE(mix.peaks);
    CHECK(mix.peaks->reach() >= Ticks::fromSeconds(3600));
    CHECK(mix.peaks->bytes() < std::size_t{5} << 20);
    CHECK(mixes.stats().bytes == mix.peaks->bytes());
    CHECK(std::abs(mix.peaks->peak(seconds(1799.99), seconds(1800.01)) - 0.9f) < 0.01f);
    CHECK(std::abs(mix.peaks->peak(Ticks{}, Ticks::fromSeconds(3600)) - 0.9f) < 0.01f); // through the coarse level
    CHECK(std::abs(mix.peaks->peak(Ticks::fromSeconds(10), Ticks::fromSeconds(1700)) - 0.05f) < 0.01f);
    CHECK(mix.peaks->peak(Ticks::fromSeconds(4000), Ticks::fromSeconds(4001)) == 0.0f); // past the mix
}

TEST_CASE("The timeline draws a nested clip from its rendered mix once it is complete", "[app][timeline][waveform][nested]") {
    Rig rig(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A2 tgt |Bbbbbbbbbb..........|
    )");
    const ProjectState nested = nestAll(*rig.controller.snapshot(), rig.tl.seq);
    rig.controller.resetProject(nested, rig.tl.seq);
    auto peaks = std::make_shared<LevelPeaks>();
    peaks->level[rig.tl.media('A').value] = 0.3f;
    peaks->level[rig.tl.media('B').value] = 0.7f;
    rig.view->setPeakSource(peaks);
    auto mixes = std::make_shared<NestedMixPeaks>();
    std::atomic<bool> release = false;
    const NestedMixPeaks::Renderer level = constantRenderer(0.6f);
    mixes->setRenderer([&](const NestedMixPeaks::Request& request, const NestedMixPeaks::Sink& sink, std::stop_token stop) {
        while (!release && !stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return level(request, sink, stop);
    });
    rig.view->setNestedMixSource(mixes);
    rig.harness.frame();
    // While it renders: the approximation, shaded.
    CHECK(rig.view->stats().nestedWaveforms == 1);
    CHECK(rig.view->stats().exactWaveforms == 0);
    CHECK(rig.view->stats().buildingWaveforms == 1);
    CHECK(rig.view->stats().loudestWaveform == 0.7f);
    // Done: only the track that shows it is rebuilt, from the mix.
    release = true;
    REQUIRE(waitFor([&] { return mixes->stats().completed == 1; }, 10000));
    REQUIRE(waitFor([&] {
        QCoreApplication::processEvents();
        rig.harness.frame();
        return rig.view->stats().exactWaveforms == 1;
    }, 10000));
    CHECK_FALSE(rig.view->stats().fullRebuild);
    CHECK(rig.view->stats().buildingWaveforms == 0);
    CHECK(std::abs(rig.view->stats().loudestWaveform - 0.6f) < 0.01f);
    // An edit inside the nested sequence: rendered again (the approximation meanwhile), then exact again.
    const Clip nest = audioNest(*rig.controller.snapshot(), rig.tl.seq);
    const SequenceId innerId = nest.sequence;
    const TrackId a2 = rig.controller.snapshot()->findSequence(innerId)->audioTracks[1]->id;
    edit::TrackStateChange mute;
    mute.muted = true;
    REQUIRE(rig.controller.applyEditFn("Mute", [&](const ProjectState& s) { return edit::setTrackState(s, innerId, a2, mute); }));
    rig.harness.frame();
    REQUIRE(waitFor([&] {
        QCoreApplication::processEvents();
        rig.harness.frame();
        return mixes->stats().completed == 2 && rig.view->stats().exactWaveforms == 1;
    }, 10000));
    // With no mix source the approximation alone (phase 1).
    rig.view->setNestedMixSource(nullptr);
    rig.harness.frame();
    CHECK(rig.view->stats().exactWaveforms == 0);
    CHECK(rig.view->stats().loudestWaveform == 0.3f);
}
