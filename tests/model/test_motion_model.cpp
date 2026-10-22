// P.2, the motion features on the model side: Time Interpolation (where it applies, the edit, undo, save and load,
// the render bar and content hashes), the Morph Cut / Warp Stabilizer / Rolling Shutter Repair descriptors, and Bars
// and Tone and the Universal Counting Leader (their items, layouts and sound).

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Synthetic.h>
#include <wabcut/model/TimeInterpolation.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <span>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId id) { return state.findSequence(seq)->findClip(id)->clip(); }

/// `state` with media `letter`'s video at `rate` (a 24 or 30 fps file in the 25 fps fixture sequence).
ProjectState withMediaRate(const ProjectState& state, MediaId media, Rate rate) {
    ProjectState s = state;
    for (MediaPtr& m : s.media) {
        if (m->id == media) {
            MediaItem item = *m;
            item.info.video.front().frameRate = rate;
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    return s;
}

const RenderSegment* segmentAt(const std::vector<RenderSegment>& bar, Ticks t) {
    for (const RenderSegment& s : bar) {
        if (s.range.contains(t)) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Time Interpolation names follow Premiere's menu", "[model][motion][interpolation]") {
    for (const TimeInterpolation m : {TimeInterpolation::FrameSampling, TimeInterpolation::FrameBlending,
                                      TimeInterpolation::OpticalFlow}) {
        CHECK(timeInterpolationFromString(toString(m)) == m);
    }
    CHECK(toString(TimeInterpolation::OpticalFlow) == "opticalFlow");
    CHECK(displayName(TimeInterpolation::FrameSampling) == "Frame Sampling");
    CHECK(displayName(TimeInterpolation::FrameBlending) == "Frame Blending");
    CHECK(displayName(TimeInterpolation::OpticalFlow) == "Optical Flow");
    CHECK_FALSE(timeInterpolationFromString("OpticalFlow").has_value());
    CHECK(Clip{}.timeInterpolation() == TimeInterpolation::FrameSampling); // the default: what clips always did
}

TEST_CASE("Time Interpolation applies to retimed video and other frame rates", "[model][motion][interpolation]") {
    Timeline tl(R"(
        V1 |Aaaa..Bbbb..Cccc|
        A1 |Aaaa..........|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@6");
    const ClipId c = tl.clip("V1@12");
    const Rate rate = tl.sequence().frameRate;
    const auto retimes = [&](const ProjectState& s, ClipId id) {
        const Clip& clip = clipOf(s, tl.seq, id);
        return retimesFrames(clip, *s.findMedia(clip.media), rate);
    };
    CHECK_FALSE(retimes(tl.state, a)); // 100 %, the sequence's rate
    edit::SpeedChange half;
    half.speed = Speed{1, 2};
    half.ripple = true;
    CHECK(retimes(ok(edit::setSpeed(tl.state, tl.seq, std::vector{a}, half)).state, a));
    edit::SpeedChange reverse;
    reverse.speed = Speed{1, 1};
    reverse.reversed = true;
    CHECK(retimes(ok(edit::setSpeed(tl.state, tl.seq, std::vector{b}, reverse)).state, b));
    CHECK(retimes(ok(edit::setTimeRemapEnabled(tl.state, tl.seq, c, true)).state, c));
    // 30 fps footage in the 25 fps sequence, at 100 %.
    CHECK(retimes(withMediaRate(tl.state, tl.media('B'), kRate30), b));
    // Not: a freeze frame, an audio clip.
    const ProjectState held = ok(edit::frameHold(tl.state, tl.seq, a, f(1), std::nullopt)).state;
    const ClipId frozen = clipAt(held, tl.seq, "V1@1");
    REQUIRE(clipOf(held, tl.seq, frozen).isFreeze());
    CHECK_FALSE(retimes(held, frozen));
    const ClipId audio = tl.clip("A1@0");
    CHECK_FALSE(retimes(tl.state, audio));
}

TEST_CASE("Setting Time Interpolation is one undoable edit of the video clips", "[model][motion][interpolation]") {
    Timeline tl(R"(
        V1 |Aaaa..Bbbb|
        A1 |Aaaa..Bbbb|
    )");
    tl.linkMatching();
    const ClipId v = tl.clip("V1@0");
    const ClipId a = tl.clip("A1@0");
    const ClipId other = tl.clip("V1@6");

    const edit::EditResult r = ok(edit::setTimeInterpolation(tl.state, tl.seq, std::vector{v}, TimeInterpolation::OpticalFlow));
    CHECK(clipOf(r.state, tl.seq, v).timeInterpolation() == TimeInterpolation::OpticalFlow);
    CHECK(clipOf(r.state, tl.seq, a).timeInterpolation() == TimeInterpolation::FrameSampling); // audio partner untouched
    CHECK(clipOf(r.state, tl.seq, other).timeInterpolation() == TimeInterpolation::FrameSampling);
    REQUIRE(r.changes.sequences.size() == 1);
    CHECK(r.changes.sequences[0].roles.has(PropertyRole::Source));
    CHECK(r.changes.sequences[0].modifiedClips == std::vector{v});
    // Starting from the audio clip, its video partner follows.
    const edit::EditResult fromAudio =
        ok(edit::setTimeInterpolation(tl.state, tl.seq, std::vector{a}, TimeInterpolation::FrameBlending));
    CHECK(clipOf(fromAudio.state, tl.seq, v).timeInterpolation() == TimeInterpolation::FrameBlending);
    // Only audio: refused.
    const auto audioOnly =
        edit::setTimeInterpolation(tl.state, tl.seq, std::vector{a}, TimeInterpolation::OpticalFlow, edit::LinkMode::IgnoreLinks);
    REQUIRE_FALSE(audioOnly.has_value());
    CHECK(audioOnly.error().code == ErrorCode::InvalidArgument);
    // A razor cut keeps it on both halves (shared content).
    const edit::EditResult cut = ok(edit::addEdit(r.state, tl.seq, f(2), std::vector{tl.track("V1")}));
    CHECK(clipAt(cut.state, tl.seq, "V1@0") == v);
    CHECK(clipOf(cut.state, tl.seq, clipAt(cut.state, tl.seq, "V1@2")).timeInterpolation() == TimeInterpolation::OpticalFlow);

    // One undo step in a session.
    EditSession session(tl.state);
    REQUIRE(session.apply("Time Interpolation", [&](const ProjectState& s) {
        return edit::setTimeInterpolation(s, tl.seq, std::vector{v}, TimeInterpolation::OpticalFlow);
    }));
    CHECK(clipOf(*session.snapshot(), tl.seq, v).timeInterpolation() == TimeInterpolation::OpticalFlow);
    REQUIRE(session.undo());
    CHECK(clipOf(*session.snapshot(), tl.seq, v).timeInterpolation() == TimeInterpolation::FrameSampling);
    REQUIRE(session.redo());
    CHECK(clipOf(*session.snapshot(), tl.seq, v).timeInterpolation() == TimeInterpolation::OpticalFlow);
}

TEST_CASE("Time Interpolation is saved only when set and read back checked", "[model][motion][interpolation][io]") {
    Timeline tl("    V1 |Aaaa|\n");
    const ClipId v = tl.clip("V1@0");
    const std::string plain = io::toJson(tl.state).value();
    CHECK(plain.find("timeInterpolation") == std::string::npos);
    const ProjectState flow =
        ok(edit::setTimeInterpolation(tl.state, tl.seq, std::vector{v}, TimeInterpolation::OpticalFlow)).state;
    const std::string saved = io::toJson(flow).value();
    CHECK(saved.find("\"timeInterpolation\": \"opticalFlow\"") != std::string::npos);
    CHECK(io::kSchemaVersion >= 17); // P.2's schema or a later one
    CHECK(saved.find("\"schemaVersion\": " + std::to_string(io::kSchemaVersion)) != std::string::npos);
    const auto back = io::fromJson(saved);
    REQUIRE(back.has_value());
    CHECK(clipOf(*back, tl.seq, v).timeInterpolation() == TimeInterpolation::OpticalFlow);
    CHECK(io::toJson(*back).value() == saved);
    // Untrusted input: an unknown method, a wrong type.
    std::string unknown = saved;
    unknown.replace(unknown.find("\"opticalFlow\""), 13, "\"warpDrive!!\"");
    const auto refused = io::fromJson(unknown);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::ParseError);
    std::string typed = saved;
    typed.replace(typed.find("\"opticalFlow\""), 13, "42");
    CHECK_FALSE(io::fromJson(typed).has_value());
    // Back to frame sampling: the file is as before.
    CHECK(io::toJson(ok(edit::setTimeInterpolation(flow, tl.seq, std::vector{v}, TimeInterpolation::FrameSampling)).state)
              .value() == plain);
}

TEST_CASE("The render bar shows optical flow and Morph Cut red and frame blending yellow", "[model][motion][render]") {
    Timeline tl(R"(
        V1 |Aaaa......Bbbb......|
    )");
    const ClipId a = tl.clip("V1@0");
    edit::SpeedChange half;
    half.speed = Speed{1, 2}; // 4 frames become 8
    ProjectState s = ok(edit::setSpeed(tl.state, tl.seq, std::vector{a}, half)).state;
    const auto barAt = [&](const ProjectState& state, Ticks t) {
        const std::vector<RenderSegment> bar = renderSegments(state, tl.seq);
        const RenderSegment* seg = segmentAt(bar, t);
        REQUIRE(seg != nullptr);
        return *seg;
    };
    CHECK(barAt(s, f(1)).status == RenderStatus::None); // frame sampling: plays as it is
    const ProjectState blended = ok(edit::setTimeInterpolation(s, tl.seq, std::vector{a}, TimeInterpolation::FrameBlending)).state;
    CHECK(barAt(blended, f(1)).status == RenderStatus::Yellow);
    CHECK(barAt(blended, f(1)).reason.find("frame blending") != std::string::npos);
    const ProjectState flow = ok(edit::setTimeInterpolation(s, tl.seq, std::vector{a}, TimeInterpolation::OpticalFlow)).state;
    CHECK(barAt(flow, f(1)).status == RenderStatus::Red);
    CHECK(barAt(flow, f(1)).reason == "optical flow");
    // An unretimed clip with Optical Flow set plays as it is.
    const ClipId b = tl.clip("V1@10");
    const ProjectState idle = ok(edit::setTimeInterpolation(tl.state, tl.seq, std::vector{b}, TimeInterpolation::OpticalFlow)).state;
    CHECK(barAt(idle, f(11)).status == RenderStatus::None);
    // The setting changes what the segment shows: its content hash changes, so a preview file of it goes.
    const TimeRange range{f(0), f(8)};
    CHECK(renderContentHash(s, tl.seq, range) != renderContentHash(flow, tl.seq, range));
    CHECK(renderContentHash(blended, tl.seq, range) != renderContentHash(flow, tl.seq, range));

    // A Morph Cut on a jump cut: red over the transition.
    Timeline jump(R"(
        V1 |A10aaaaaaaA40aaaaaaa|
    )");
    edit::TransitionSpec spec;
    spec.outgoing = jump.clip("V1@0");
    spec.incoming = jump.clip("V1@10");
    spec.before = f(3);
    spec.after = f(3);
    spec.effect = std::string(kMorphCutTransition);
    const ProjectState morphed = ok(edit::addTransition(jump.state, jump.seq, jump.track("V1"), spec)).state;
    const std::vector<RenderSegment> bar = renderSegments(morphed, jump.seq);
    // Red over the transition, on both sides of the cut (segments end at edit points), and nowhere else.
    for (const std::int64_t frame : {7, 9, 10, 12}) {
        INFO("frame " << frame);
        const RenderSegment* seg = segmentAt(bar, f(frame));
        REQUIRE(seg != nullptr);
        CHECK(seg->status == RenderStatus::Red);
        CHECK(seg->reason == "Morph Cut");
        CHECK(seg->range.start >= f(7));
        CHECK(seg->range.end() <= f(13));
    }
    CHECK(segmentAt(bar, f(2))->status == RenderStatus::None);
    CHECK(segmentAt(bar, f(15))->status == RenderStatus::None);
}

TEST_CASE("The motion effects are registered with Premiere's names", "[model][motion][effects]") {
    const EffectRegistry& r = EffectRegistry::builtin();
    const EffectDescriptor* morph = r.find(kMorphCutTransition);
    REQUIRE(morph != nullptr);
    CHECK(morph->kind == EffectKind::VideoTransition);
    CHECK(morph->displayName == "Morph Cut");
    CHECK(morph->category == "Dissolve");
    const EffectDescriptor* warp = r.find(kWarpStabilizerEffect);
    REQUIRE(warp != nullptr);
    CHECK(warp->displayName == "Warp Stabilizer");
    CHECK(warp->category == "Distort");
    for (const char* key : {"result", "smoothness", "method", "framing", "maxScale", "cropLessSmoothMore", "detailedAnalysis",
                            "rollingShutter", "transform.x", "corner.bl.y"}) {
        INFO(key);
        CHECK(warp->param(key) != nullptr);
    }
    CHECK(warp->param("smoothness")->defaultValue == 50.0);
    CHECK(warp->param("maxScale")->defaultValue == 1.5);
    CHECK(warp->param("framing")->defaultValue == 2.0); // Stabilize, Crop, Auto-scale
    const EffectDescriptor* shutter = r.find(kRollingShutterRepairEffect);
    REQUIRE(shutter != nullptr);
    CHECK(shutter->displayName == "Rolling Shutter Repair");
    CHECK(shutter->param("rate")->defaultValue == 50.0);
    // The C.3 catalog is still all there (the motion block is appended, not a replacement).
    CHECK(r.find("crossDissolve") != nullptr);
    CHECK(r.find("wabcut.blur.gaussian") != nullptr);
}

// ---- Bars and Tone, Universal Counting Leader ------------------------------------------------------------------

TEST_CASE("Bars and tone plays a 1 kHz tone at -20 dBFS and the leader pops on the 2", "[model][motion][synthetic]") {
    Timeline tl("    V1 |Aaaa|\n");
    const auto bars = edit::addSyntheticMedia(tl.state, SyntheticKind::BarsAndTone, {}, tl.seq);
    REQUIRE(bars.has_value());
    const MediaItem& item = *bars->state.findMedia(bars->id);
    REQUIRE(item.info.audio.size() == 1);
    CHECK(item.info.audio.front().sampleRate == tl.sequence().audioRate);
    CHECK(item.info.audio.front().channels == 2);
    requireValid(bars->state);
    // One second at 48 kHz, stereo, read in two halves: phase-continuous, peak 0.1 (-20 dBFS), 1000 cycles.
    std::vector<float> first(48000);
    std::vector<float> second(48000);
    CHECK(renderSyntheticAudio(item, Ticks{}, first, 2, kRate48kHz) == 24000);
    CHECK(renderSyntheticAudio(item, Ticks{kTicksPerSecond / 2}, second, 2, kRate48kHz) == 24000);
    std::vector<float> left;
    for (std::size_t i = 0; i < first.size(); i += 2) {
        CHECK(first[i] == first[i + 1]);
        left.push_back(first[i]);
    }
    for (std::size_t i = 0; i < second.size(); i += 2) {
        left.push_back(second[i]);
    }
    float peak = 0.0f;
    int crossings = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        peak = std::max(peak, std::abs(left[i]));
        if (i > 0 && left[i - 1] < 0.0f && left[i] >= 0.0f) {
            ++crossings;
        }
    }
    CHECK(peak == Approx(0.1).margin(1e-4));
    CHECK(std::abs(crossings - 1000) <= 1);
    CHECK(std::abs(left[24000] - left[23999]) < 0.02f); // no jump at the seam between the two reads

    const auto leader = edit::addSyntheticMedia(tl.state, SyntheticKind::CountingLeader, {}, tl.seq);
    REQUIRE(leader.has_value());
    const MediaItem& l = *leader->state.findMedia(leader->id);
    CHECK_FALSE(l.info.isStillImage);
    CHECK(l.info.duration == kCountingLeaderLength);
    requireValid(leader->state);
    CHECK(countingLeaderAt(l, Ticks{}).number == 8);
    const Ticks halfPast5{5 * kTicksPerSecond + kTicksPerSecond / 2};
    CHECK(countingLeaderAt(l, halfPast5).number == 3);
    CHECK(countingLeaderAt(l, halfPast5).sweep == Approx(0.5));
    CHECK(countingLeaderAt(l, Ticks{6 * kTicksPerSecond}).number == 2);
    CHECK(countingLeaderAt(l, Ticks{7 * kTicksPerSecond}).number == 0);
    // The 2-pop: sound only in the frame of the 2 (Cue Blip on 2 is on by default).
    std::vector<float> pop(2 * 4800);
    renderSyntheticAudio(l, Ticks{6 * kTicksPerSecond - kTicksPerSecond / 20}, pop, 2, kRate48kHz);
    const auto loud = [](std::span<const float> s) {
        float m = 0.0f;
        for (const float v : s) {
            m = std::max(m, std::abs(v));
        }
        return m;
    };
    CHECK(loud(std::span<const float>(pop).subspan(0, 2 * 2400)) == 0.0f);     // before 6 s: silence
    CHECK(loud(std::span<const float>(pop).subspan(2 * 2400, 2 * 1200)) > 0.05f); // just after 6 s: the blip
    std::vector<float> after(2 * 4800);
    renderSyntheticAudio(l, Ticks{6 * kTicksPerSecond + kTicksPerSecond / 10}, after, 2, kRate48kHz);
    CHECK(loud(after) == 0.0f);
    // A leader edited to the wrong length or streams is refused by validation.
    MediaItem bad = l;
    bad.info.duration = Ticks{kTicksPerSecond};
    CHECK(syntheticProblem(bad).has_value());
    bad = l;
    bad.info.audio.clear();
    CHECK(syntheticProblem(bad).has_value());
}

TEST_CASE("Bars patterns tile the frame", "[model][motion][synthetic]") {
    for (const int pattern : {0, 1}) {
        INFO(pattern);
        const std::vector<BarsPatch> patches = barsPattern(pattern);
        REQUIRE_FALSE(patches.empty());
        double area = 0.0;
        for (const BarsPatch& p : patches) {
            CHECK(p.x0 >= 0.0);
            CHECK(p.x1 <= 1.0 + 1e-12);
            CHECK(p.x1 > p.x0);
            CHECK(p.y1 > p.y0);
            area += (p.x1 - p.x0) * (p.y1 - p.y0);
        }
        CHECK(area == Approx(1.0).margin(1e-9));
        for (std::size_t i = 0; i < patches.size(); ++i) {
            for (std::size_t j = i + 1; j < patches.size(); ++j) {
                const BarsPatch& a = patches[i];
                const BarsPatch& b = patches[j];
                const bool overlap = a.x0 < b.x1 - 1e-12 && b.x0 < a.x1 - 1e-12 && a.y0 < b.y1 - 1e-12 && b.y0 < a.y1 - 1e-12;
                CHECK_FALSE(overlap);
            }
        }
    }
    // HD: the top row is 40 % grey side panels around seven 75 % bars.
    const std::vector<BarsPatch> hd = barsPattern(0);
    CHECK(hd[0].rgb == std::array<double, 3>{0.4, 0.4, 0.4});
    CHECK(hd[1].rgb == std::array<double, 3>{0.75, 0.75, 0.75});
}
