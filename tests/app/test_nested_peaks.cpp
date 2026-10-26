// 0.5 (P0 round, stream A, phase 1): waveforms on nested-sequence, multicam and merged audio clips. Such a clip has no
// media of its own; its waveform is the loudest peak of the leaf clips it plays (NestedPeaks.h): a plain nested clip the
// audio tracks whose output is on, a multicam clip its active angles' tracks, a merged clip every angle's, nested clips
// inside followed one level further. The timeline draws it, shades it while a leaf's peaks build, and rebuilds only the
// tracks whose nested clips read media whose peaks changed.

#include "TimelineHarness.h"

#include "NestedPeaks.h"
#include "PeakSource.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/model/EditingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// A level per media item (or, with `ramp`, the slice's source time in seconds / 2); says which media changed (by
/// serial) and which one is still building.
class LevelPeaks final : public PeakSource {
public:
    bool peaks(MediaId media, StreamRef, Ticks sourceStart, Ticks sourceEnd, std::span<float> out) const override {
        const auto it = level.find(media.value);
        if (it == level.end() || sourceEnd <= sourceStart || out.empty())
            return false;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double centre = sourceStart.toSeconds()
                                  + (sourceEnd - sourceStart).toSeconds() * (static_cast<double>(i) + 0.5) / static_cast<double>(out.size());
            out[i] = ramp ? static_cast<float>(std::min(1.0, centre / 2.0)) : it->second;
        }
        return true;
    }
    std::optional<Ticks> buildingUntil(MediaId media, StreamRef) const override {
        if (media == building)
            return until;
        return std::nullopt;
    }
    std::optional<std::vector<std::uint64_t>> mediaChangedSince(std::uint64_t serial, std::uint64_t& now) const override {
        now = m_serial;
        std::vector<std::uint64_t> out;
        for (const auto& [media, at] : m_changed) {
            if (at > serial)
                out.push_back(media);
        }
        return out;
    }
    void touch(MediaId media) { m_changed[media.value] = ++m_serial; }

    std::map<std::uint64_t, float> level;
    bool ramp = false;
    MediaId building;
    Ticks until;

private:
    std::uint64_t m_serial = 1;
    std::map<std::uint64_t, std::uint64_t> m_changed;
};

/// V1 and A1 play media A for 20 frames, A2 media B for its first 10.
constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    A2 tgt |Bbbbbbbbbb..........|
)";

/// `state` with the clips at frame 0 of `seq` (V1, A1, A2) nested; the parent sequence's nested audio clip is on A1.
ProjectState nestAll(const ProjectState& state, SequenceId seq) {
    std::vector<ClipId> clips;
    const Sequence* sequence = state.findSequence(seq);
    REQUIRE(sequence);
    sequence->forEachTrack([&](const Track& track) {
        if (!track.clips.empty() && track.clips[0].start() == Ticks{})
            clips.push_back(track.clips[0].id);
    });
    auto nested = edit::nestClips(state, seq, clips);
    INFO((nested ? std::string() : nested.error().message));
    REQUIRE(nested);
    return std::move(nested->edit.state);
}

const Clip& audioNest(const ProjectState& state, SequenceId seq) {
    const Sequence* parent = state.findSequence(seq);
    REQUIRE(parent);
    REQUIRE_FALSE(parent->audioTracks[0]->clips.empty());
    const Clip& clip = parent->audioTracks[0]->clips[0];
    REQUIRE(clip.isNested());
    return clip;
}

std::vector<float> waveform(const Clip& clip, const ProjectState& state, const PeakSource& peaks, std::size_t columns,
                            NestedPeaksResult* result = nullptr, std::vector<std::uint8_t>* pending = nullptr) {
    std::vector<float> out(columns, -1.0f);
    const NestedPeaksResult r = nestedPeaks(clip, state, peaks, clip.start(), clip.end(), out, pending);
    if (result)
        *result = r;
    return out;
}

} // namespace

TEST_CASE("Nested peaks take the loudest leaf of the audio tracks a nested clip plays", "[app][timeline][waveform][nested]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const ProjectState state = nestAll(tl.state, tl.seq);
    const Clip& nest = audioNest(state, tl.seq);
    LevelPeaks peaks;
    peaks.level[tl.media('A').value] = 0.3f;
    peaks.level[tl.media('B').value] = 0.7f;

    NestedPeaksResult result;
    const std::vector<float> out = waveform(nest, state, peaks, 20, &result);
    CHECK(result.drawn);
    CHECK_FALSE(result.building);
    // B plays under the first half (louder), A alone after it.
    CHECK(out[2] == 0.7f);
    CHECK(out[7] == 0.7f);
    CHECK(out[13] == 0.3f);
    CHECK(out[18] == 0.3f);

    // What the waveform reads, and a stamp that follows edits inside.
    std::vector<std::uint64_t> media{tl.media('A').value, tl.media('B').value};
    std::ranges::sort(media);
    CHECK(nestedPeakMedia(state, nest.sequence) == media);
    const std::uint64_t stamp = nestedPeakStamp(state, nest.sequence);
    CHECK(stamp != 0);
    CHECK(nestedPeakStamp(state, SequenceId{987654}) == 0);

    SECTION("a muted track inside is not heard, so it is not drawn") {
        const Sequence* inner = state.findSequence(nest.sequence);
        REQUIRE(inner);
        REQUIRE(inner->audioTracks.size() >= 2);
        edit::TrackStateChange mute;
        mute.muted = true;
        auto muted = edit::setTrackState(state, nest.sequence, inner->audioTracks[1]->id, mute);
        REQUIRE(muted);
        const std::vector<float> quiet = waveform(nest, muted->state, peaks, 20);
        CHECK(quiet[2] == 0.3f);
        CHECK(quiet[13] == 0.3f);
        CHECK(nestedPeakStamp(muted->state, nest.sequence) != stamp);
    }
    SECTION("a nested clip sounds at 100 % only: forwards, reversed, never at another speed") {
        peaks.ramp = true;
        const std::vector<float> forwards = waveform(nest, state, peaks, 10);
        CHECK(forwards.front() < forwards.back());
        Clip reversed = nest;
        reversed.reversed = true;
        const std::vector<float> backwards = waveform(reversed, state, peaks, 10);
        CHECK(backwards.front() > backwards.back());
        CHECK(std::abs(backwards.front() - forwards.back()) < 0.05f);
        Clip slow = nest;
        slow.speed = Speed{1, 2};
        NestedPeaksResult slowResult;
        const std::vector<float> silent = waveform(slow, state, peaks, 10, &slowResult);
        CHECK_FALSE(slowResult.drawn);
        CHECK(std::ranges::all_of(silent, [](float v) { return v == 0.0f; }));
    }
    SECTION("a disabled leaf clip gives nothing") {
        const Sequence* inner = state.findSequence(nest.sequence);
        const ClipId b = inner->audioTracks[1]->clips[0].id;
        edit::ClipAttributeChange off;
        off.enabled = false;
        auto disabled = edit::setClipAttributes(state, nest.sequence, std::vector<ClipId>{b}, off);
        REQUIRE(disabled);
        const std::vector<float> out2 = waveform(nest, disabled->state, peaks, 20);
        CHECK(out2[2] == 0.3f);
    }
    SECTION("a leaf still building marks the columns it has not reached") {
        peaks.building = tl.media('A');
        peaks.until = f(15); // A's source decoded up to frame 15
        NestedPeaksResult building;
        std::vector<std::uint8_t> pending;
        waveform(nest, state, peaks, 20, &building, &pending);
        CHECK(building.building);
        REQUIRE(pending.size() == 20);
        CHECK(pending[5] == 0);
        CHECK(pending[18] == 1);
    }
}

TEST_CASE("Nested peaks follow nested clips two levels deep and no further", "[app][timeline][waveform][nested]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    LevelPeaks peaks;
    peaks.level[tl.media('A').value] = 0.3f;
    peaks.level[tl.media('B').value] = 0.7f;
    const ProjectState one = nestAll(tl.state, tl.seq);
    const ProjectState two = nestAll(one, tl.seq);
    NestedPeaksResult result;
    const std::vector<float> deep = waveform(audioNest(two, tl.seq), two, peaks, 20, &result);
    CHECK(result.drawn);
    CHECK(deep[2] == 0.7f);
    CHECK(deep[13] == 0.3f);
    const ProjectState three = nestAll(two, tl.seq);
    waveform(audioNest(three, tl.seq), three, peaks, 20, &result);
    CHECK_FALSE(result.drawn);
}

TEST_CASE("Nested peaks of multicam clips read their active angles and merged clips every angle", "[app][timeline][waveform][nested]") {
    Timeline tl(R"(
        V1 tgt |....................|
        A1 tgt |....................|
    )");
    LevelPeaks peaks;
    peaks.level[tl.media('A').value] = 0.2f;
    peaks.level[tl.media('B').value] = 0.5f;
    peaks.level[tl.media('C').value] = 0.9f;

    SECTION("multicam: the angle the clip plays") {
        edit::MulticamSpec spec;
        spec.name = "Interview";
        spec.angles = {tl.media('A'), tl.media('B'), tl.media('C')};
        spec.sync = MulticamSync::InPoints;
        auto made = edit::createMulticam(tl.state, spec);
        REQUIRE(made);
        Clip clip;
        clip.id = ClipId{900001};
        clip.sequence = made->id;
        clip.stream = StreamRef{StreamKind::Audio, 0};
        clip.timelineRange = TimeRange{Ticks{}, f(20)};
        clip.angles = AngleSet::of(1);
        NestedPeaksResult result;
        std::vector<float> out = waveform(clip, made->state, peaks, 10, &result);
        CHECK(result.drawn);
        CHECK(out[4] == 0.5f);
        clip.angles = AngleSet::of(2);
        out = waveform(clip, made->state, peaks, 10);
        CHECK(out[4] == 0.9f);
        CHECK(nestedPeakMedia(made->state, made->id).size() == 3);
    }
    SECTION("merged: the camera's and the recorder's sound together") {
        edit::MergeSpec spec;
        spec.video = tl.media('A');
        spec.audio = {tl.media('B')};
        auto made = edit::mergeClips(tl.state, spec);
        INFO((made ? std::string() : made.error().message));
        REQUIRE(made);
        const Sequence* merged = made->state.findSequence(made->id);
        REQUIRE(merged);
        REQUIRE(merged->multicam);
        CHECK(merged->multicam->merged);
        Clip clip;
        clip.id = ClipId{900002};
        clip.sequence = made->id;
        clip.stream = StreamRef{StreamKind::Audio, 0};
        clip.timelineRange = TimeRange{Ticks{}, f(20)};
        for (std::size_t i = 0; i < merged->multicam->angles.size(); ++i) {
            if (!merged->multicam->angles[i].audio.empty())
                clip.angles.bits |= AngleSet::of(i).bits; // as an edit places a merged clip's sound (EditOps)
        }
        NestedPeaksResult result;
        const std::vector<float> out = waveform(clip, made->state, peaks, 10, &result);
        CHECK(result.drawn);
        CHECK(out[4] == 0.5f); // the louder of A (0.2) and B (0.5)
    }
}

TEST_CASE("The timeline draws nested audio waveforms and rebuilds them when a leaf's peaks change", "[app][timeline][waveform][nested]") {
    Rig rig(kPicture);
    const ProjectState nested = nestAll(rig.controller.snapshot() ? *rig.controller.snapshot() : rig.tl.state, rig.tl.seq);
    rig.controller.resetProject(nested, rig.tl.seq);
    rig.view->setNestedMixSource(nullptr); // the approximation alone (phase 2: test_nested_mix_peaks.cpp)
    auto peaks = std::make_shared<LevelPeaks>();
    peaks->level[rig.tl.media('A').value] = 0.3f;
    peaks->level[rig.tl.media('B').value] = 0.7f;
    rig.view->setPeakSource(peaks);
    rig.harness.frame();
    CHECK(rig.view->stats().fullRebuild);
    CHECK(rig.view->stats().nestedWaveforms == 1);
    CHECK(rig.view->stats().loudestWaveform == 0.7f);
    CHECK(rig.view->stats().buildingWaveforms == 0);

    SECTION("new peaks of a leaf rebuild the track of the nested clip that reads it") {
        peaks->touch(rig.tl.media('B'));
        rig.view->refreshPeaks();
        rig.harness.frame();
        CHECK_FALSE(rig.view->stats().fullRebuild);
        CHECK(rig.view->stats().rebuiltTracks == 1);
        CHECK(rig.view->stats().nestedWaveforms == 1);
        // Peaks of media nothing nested reads rebuild nothing.
        peaks->touch(rig.tl.media('Z'));
        rig.view->refreshPeaks();
        rig.view->update();
        rig.harness.frame();
        CHECK(rig.view->stats().rebuiltTracks == 0);
    }
    SECTION("a leaf still building shades the nested clip until it is complete") {
        peaks->building = rig.tl.media('A');
        peaks->until = f(15);
        peaks->touch(rig.tl.media('A'));
        rig.view->refreshPeaks();
        rig.harness.frame();
        CHECK(rig.view->stats().rebuiltTracks == 1);
        CHECK(rig.view->stats().buildingWaveforms == 1);
        peaks->building = MediaId{};
        peaks->touch(rig.tl.media('A'));
        rig.view->refreshPeaks();
        rig.harness.frame();
        CHECK(rig.view->stats().buildingWaveforms == 0);
    }
    SECTION("an edit inside the nested sequence redraws the waveform") {
        const Clip& nest = audioNest(*rig.controller.snapshot(), rig.tl.seq);
        const Sequence* inner = rig.controller.snapshot()->findSequence(nest.sequence);
        REQUIRE(inner);
        const TrackId a2 = inner->audioTracks[1]->id;
        const SequenceId innerId = nest.sequence;
        edit::TrackStateChange mute;
        mute.muted = true;
        REQUIRE(rig.controller.applyEditFn("Mute", [&](const ProjectState& s) { return edit::setTrackState(s, innerId, a2, mute); }));
        rig.harness.frame();
        CHECK(rig.view->stats().fullRebuild);
        CHECK(rig.view->stats().loudestWaveform == 0.3f);
    }
}
