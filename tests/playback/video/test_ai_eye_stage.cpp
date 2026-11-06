// AI Eye Contact (card 72) in the evaluator. A clip with AI Eye Contact keeps its effect for the compositor, and buildCompositeLayers turns the
// frame's eye measurements (the installed SourceFrameProcessor's gaze()) and the effect's settings at that time into the effect's parameters
// (model/EyeContact.h bakeEyeContact): one copy of the effect per face (the largest first, "Largest" keeps one), each with that face's eyes and
// shifts; a face that needs nothing adds nothing; without measurements the effect draws nothing and the layer says it is not processed; with
// Amount at 0 the processor is not even asked.

#include "PlaybackTestSupport.h"

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/EyeContact.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/SourceStage.h>
#include <wabcut/render/EffectLibrary.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace wabcut;
using namespace wabcut::playback;
using Catch::Approx;

namespace {

std::shared_ptr<MediaItem> mediaOf(ProjectState& s, int w, int h) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "fake.mp4";
    VideoStreamInfo v;
    v.index = 0;
    v.width = w;
    v.height = h;
    v.frameRate = kRate60;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

struct Fixture {
    ProjectState state;
    Sequence seq;
    std::shared_ptr<MediaItem> media;

    Fixture(int w, int h, const std::function<void(ProjectState&, Clip&)>& setup) {
        media = mediaOf(state, w, h);
        state.media = {media};
        seq.id = state.ids.allocate<SequenceId>();
        seq.frameRate = kRate60;
        seq.width = 1920;
        seq.height = 1080;
        Clip c;
        c.id = state.ids.allocate<ClipId>();
        c.media = media->id;
        c.timelineRange = {Ticks{}, framesToTicks(120, kRate60)};
        setup(state, c);
        Track t;
        t.id = state.ids.allocate<TrackId>();
        t.clips = ClipList({c});
        seq.videoTracks = {std::make_shared<const Track>(std::move(t))};
    }

    std::vector<LayerSpec> layers(std::int64_t frame) const {
        std::vector<LayerSpec> out;
        evaluateVideoLayers(state, seq, framesToTicks(frame, kRate60), out);
        return out;
    }
};

media::VideoFrame frameOf(int w, int h, Ticks pts) {
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * h * 4, 128);
    media::VideoFrame f;
    f.pts = pts;
    f.duration = framesToTicks(1, kRate60);
    f.width = w;
    f.height = h;
    f.format = media::PixelFormat::RGBA8;
    media::CpuPlanes cpu;
    cpu.planes[0] = media::Plane{pixels->data(), w * 4, w, h};
    cpu.owner = pixels;
    f.payload = std::move(cpu);
    return f;
}

/// An open eye (the picture 1000 px wide: 60 px between the corners, lids 7.5 px above / 4.5 px below, an iris of 12 px) looking `down` (a share
/// of the eye's width) below where it would look into the lens.
GazeEye eyeLooking(double down, double cx) {
    GazeEye e;
    e.cx = cx;
    e.cy = 0.30;
    e.halfWidth = 0.030;
    e.upper = 0.0075;
    e.lower = 0.0045;
    e.irisRadius = 0.012;
    e.irisX = 0.0;
    e.irisY = 0.5 * (e.lower - e.upper) + down * 2.0 * e.halfWidth;
    return e;
}

GazeFace faceLooking(double down, double centre, double size) {
    GazeFace f;
    f.eyes[0] = eyeLooking(down, centre - 0.1);
    f.eyes[1] = eyeLooking(down, centre + 0.1);
    f.size = size;
    return f;
}

/// A processor with eye measurements to give, or none.
class GazeProcessor final : public SourceFrameProcessor {
public:
    bool haveMap = true;
    int calls = 0;
    Ticks lastPts{};
    std::vector<GazeFace> faces;
    SourceStageResult process(const SourceStageQuery&) override { return {}; }
    GazeResult gaze(const GazeQuery& q) override {
        ++calls;
        lastPts = q.decoded ? q.decoded->pts : Ticks{};
        if (!haveMap) {
            return {nullptr, true};
        }
        auto map = std::make_shared<GazeMap>();
        map->faces = faces;
        return {map, false};
    }
};

struct InstalledProcessor {
    explicit InstalledProcessor(std::shared_ptr<SourceFrameProcessor> p) { setSourceFrameProcessor(std::move(p)); }
    ~InstalledProcessor() { setSourceFrameProcessor(nullptr); }
};

EffectInstance eyeContact(ProjectState& s, double amount = 100.0) {
    EffectInstance e;
    e.id = s.ids.allocate<EffectInstanceId>();
    e.effect = std::string(kAiEyeContactEffect);
    e.set("amount", ParamValue(amount));
    return e;
}

const render::CanvasSize kCanvas{1920, 1080, 1.0};

std::vector<render::CompositeLayer> build(const Fixture& f, std::int64_t frame) {
    auto specs = f.layers(frame);
    std::vector<media::VideoFrame> frames{frameOf(1920, 1080, framesToTicks(frame, kRate60))};
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(specs, frames, out, nullptr, kCanvas);
    return out;
}

constexpr std::uint16_t kEyeOp = static_cast<std::uint16_t>(render::LibraryOp::EyeContact);

} // namespace

TEST_CASE("AI Eye Contact stays a compositor effect and the layer is flagged for its measurements", "[playback][evaluate][ai][eyecontact]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({eyeContact(s)}); });
    const auto specs = f.layers(10);
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].eyeContact);
    CHECK_FALSE(specs[0].faceRetouch);
    CHECK_FALSE(specs[0].refocus);
    CHECK(specs[0].ai.empty()); // not a source-stage effect: the decoded picture is not replaced
    REQUIRE(specs[0].effects.size() == 1);
    CHECK(specs[0].effects[0].type == render::EffectType::Library);
    CHECK(specs[0].effects[0].op == kEyeOp);
    CHECK(specs[0].effects[0].params[0] == Approx(100.0)); // Amount, keyframable: read at the frame's time
    CHECK(specs[0].effects[0].params[1] == Approx(5.0));   // Down From
    CHECK(specs[0].effects[0].params[2] == Approx(45.0));  // Down To
    CHECK(specs[0].effects[0].params[3] == Approx(25.0));  // Side Limit
    // The registry's keys give the compositor the settings first and leave room for two baked eyes.
    const render::EffectLibraryEntry* entry = render::findLibraryEffect(kAiEyeContactEffect);
    REQUIRE(entry != nullptr);
    CHECK(entry->op == render::LibraryOp::EyeContact);
    CHECK(entry->keys[kEyeParamView] == "view");
    CHECK(entry->keys[kEyeParamCount].empty());
    Fixture plain(1920, 1080, [](ProjectState&, Clip&) {});
    CHECK_FALSE(plain.layers(10)[0].eyeContact);
}

TEST_CASE("The frame's eye measurements are baked into the effect: one copy per face, the largest first", "[playback][evaluate][ai][eyecontact]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({eyeContact(s)}); });
    auto processor = std::make_shared<GazeProcessor>();
    processor->faces = {faceLooking(0.08, 0.75, 0.20), faceLooking(0.12, 0.30, 0.40)}; // the smaller face first
    InstalledProcessor installed(processor);
    const auto out = build(f, 10);
    REQUIRE(out.size() == 1);
    CHECK(processor->calls == 1);
    CHECK(processor->lastPts == framesToTicks(10, kRate60)); // asked for the decoded frame's own time
    CHECK_FALSE(out[0].aiUnprocessed);
    // One effect per face, the larger face (centre 0.30) first, each with both its eyes.
    REQUIRE(out[0].effects.size() == 2);
    for (const render::LayerEffect& e : out[0].effects) {
        CHECK(e.type == render::EffectType::Library);
        CHECK(e.op == kEyeOp);
        CHECK(e.params[0] == Approx(100.0)); // the settings are as they were
        CHECK(e.params[kEyeParamCount] == 2.0);
    }
    const auto& first = out[0].effects[0].params;
    const auto& second = out[0].effects[1].params;
    CHECK(first[kEyeParamFirst + 0] == Approx(0.20));                       // the left eye of the face centred at 0.30
    CHECK(first[kEyeParamFirst + kEyeParamStride + 0] == Approx(0.40));     // and its right eye
    CHECK(second[kEyeParamFirst + 0] == Approx(0.65));                      // the second face's
    // Each face has its own shift: the iris of the first moves up by 12 % of the eye's width, the second's by 8 %.
    CHECK(first[kEyeParamFirst + 10] == Approx(-0.12 * 0.060));
    CHECK(second[kEyeParamFirst + 10] == Approx(-0.08 * 0.060));
    CHECK(first[kEyeParamFirst + 9] == Approx(0.0).margin(1e-12)); // not sideways
    // "Largest" keeps only the first.
    Fixture largest(1920, 1080, [](ProjectState& s, Clip& c) {
        EffectInstance e = eyeContact(s);
        e.set("faces", ParamValue(1.0));
        c.setEffects({e});
    });
    const auto one = build(largest, 10);
    REQUIRE(one.size() == 1);
    REQUIRE(one[0].effects.size() == 1);
    CHECK(one[0].effects[0].params[kEyeParamFirst + 0] == Approx(0.20));
}

TEST_CASE("The settings at the frame decide which gazes are corrected", "[playback][evaluate][ai][eyecontact]") {
    auto processor = std::make_shared<GazeProcessor>();
    processor->faces = {faceLooking(0.12, 0.30, 0.40)};
    InstalledProcessor installed(processor);
    // Amount scales the shift.
    Fixture half(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({eyeContact(s, 50.0)}); });
    const auto halved = build(half, 10);
    REQUIRE(halved[0].effects.size() == 1);
    CHECK(halved[0].effects[0].params[kEyeParamFirst + 10] == Approx(-0.5 * 0.12 * 0.060));
    // A range that does not hold the gaze (Down To a point, 11 points under 12 %: past the ramp): nothing to correct, the effect is dropped, the picture is as it is.
    Fixture narrow(1920, 1080, [](ProjectState& s, Clip& c) {
        EffectInstance e = eyeContact(s);
        e.set("downTo", ParamValue(1.0));
        e.set("downFrom", ParamValue(0.0));
        c.setEffects({e});
    });
    const auto none = build(narrow, 10);
    REQUIRE(none.size() == 1);
    CHECK(none[0].effects.empty());
    CHECK_FALSE(none[0].aiUnprocessed); // it was measured: there is simply nothing to do
    // The Eye Map view of a frame with nothing to correct keeps the effect, with no eyes (a black map).
    Fixture map(1920, 1080, [](ProjectState& s, Clip& c) {
        EffectInstance e = eyeContact(s);
        e.set("downTo", ParamValue(1.0));
        e.set("downFrom", ParamValue(0.0));
        e.set("view", ParamValue(1.0));
        c.setEffects({e});
    });
    const auto black = build(map, 10);
    REQUIRE(black[0].effects.size() == 1);
    CHECK(black[0].effects[0].params[kEyeParamCount] == 0.0);
    CHECK(black[0].effects[0].params[kEyeParamView] == 1.0);
    // A face already looking into the lens, or turned away, adds nothing; a frame without faces too.
    processor->faces = {faceLooking(0.0, 0.30, 0.40)};
    Fixture lens(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({eyeContact(s)}); });
    CHECK(build(lens, 10)[0].effects.empty());
    GazeFace away = faceLooking(0.12, 0.30, 0.40);
    away.turn = 0.8;
    processor->faces = {away};
    CHECK(build(lens, 10)[0].effects.empty());
    processor->faces.clear();
    CHECK(build(lens, 10)[0].effects.empty());
    // A face the landmark model hardly believes in is not corrected.
    GazeFace doubtful = faceLooking(0.12, 0.30, 0.40);
    doubtful.score = 0.3;
    processor->faces = {doubtful};
    CHECK(build(lens, 10)[0].effects.empty());
    // The other effects of the clip stay where they are, in order.
    Fixture stack(1920, 1080, [](ProjectState& s, Clip& c) {
        EffectInstance blur;
        blur.id = s.ids.allocate<EffectInstanceId>();
        blur.effect = "wabcut.blur.camera";
        c.setEffects({blur, eyeContact(s)});
    });
    processor->faces = {faceLooking(0.12, 0.30, 0.40)};
    const auto stacked = build(stack, 10);
    REQUIRE(stacked[0].effects.size() == 2);
    CHECK(stacked[0].effects[0].op != kEyeOp);
    CHECK(stacked[0].effects[1].op == kEyeOp);
}

TEST_CASE("Without measurements the effect draws nothing and the layer says it is not processed", "[playback][evaluate][ai][eyecontact]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({eyeContact(s)}); });
    SECTION("the processor has none for this frame") {
        auto processor = std::make_shared<GazeProcessor>();
        processor->haveMap = false;
        InstalledProcessor installed(processor);
        const auto out = build(f, 10);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        REQUIRE(out[0].effects.size() == 1);
        CHECK(out[0].effects[0].params[kEyeParamCount] == 0.0);
    }
    SECTION("no processor is installed (a build without the AI helper)") {
        const auto out = build(f, 10);
        REQUIRE(out.size() == 1);
        CHECK(out[0].aiUnprocessed);
        REQUIRE(out[0].effects.size() == 1);
        CHECK(out[0].effects[0].params[kEyeParamCount] == 0.0);
    }
    SECTION("a clip without Eye Contact never asks") {
        Fixture plain(1920, 1080, [](ProjectState&, Clip&) {});
        auto processor = std::make_shared<GazeProcessor>();
        InstalledProcessor installed(processor);
        const auto out = build(plain, 10);
        REQUIRE(out.size() == 1);
        CHECK(processor->calls == 0);
        CHECK_FALSE(out[0].aiUnprocessed);
    }
}

TEST_CASE("An Eye Contact with Amount 0 asks for nothing and flags nothing", "[playback][evaluate][ai][eyecontact]") {
    Fixture f(1920, 1080, [](ProjectState& s, Clip& c) { c.setEffects({eyeContact(s, 0.0)}); });
    auto processor = std::make_shared<GazeProcessor>();
    processor->faces = {faceLooking(0.12, 0.30, 0.40)};
    InstalledProcessor installed(processor);
    const auto out = build(f, 10);
    REQUIRE(out.size() == 1);
    CHECK(processor->calls == 0);
    CHECK_FALSE(out[0].aiUnprocessed);
}
