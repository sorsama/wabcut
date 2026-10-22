// AI-14 commands: mask.objectSelect / mask.refine / mask.propagate through the engine on a scripted video (a red object
// moving through a shot that cuts at frame 60) and an in-process segmenter: the matte mask with its bounding-box keys,
// a dry run that changes nothing (and whose mattes the apply reuses from the cache), one undo step, stopping at the cut
// (or not), In / Out, effect-local and inverted masks, refining clicks, propagation after "none", subjects by kind and
// colour, every refusal, cancel; and the assistant: "blur the background behind the speaker" and "isolate the red car
// and brighten it" staged as edits, redactions out of its reach.

#include "SegmentTestUtil.h"

#include "../AgentTestUtil.h"
#include "CommandHarness.h"
#include "NestedTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::segment;
using namespace wabcut::ai::segment::test;
using wabcut::test::CommandHarness;
using wabcut::test::Timeline;
using Catch::Approx;

namespace {

/// Finds the red pixels' box and calls it a face, a person and a car; a blue area is a (blue) car.
class ColourDetector final : public vision::SubjectDetector {
public:
    std::expected<std::vector<vision::FrameDetections>, Error> detect(std::span<const search::Image> frames, std::stop_token) override {
        ++calls;
        std::vector<vision::FrameDetections> out;
        for (const search::Image& img : frames) {
            vision::FrameDetections d;
            for (const auto& [r, g, b, people] : {std::tuple{220, 30, 40, true}, std::tuple{30, 40, 220, false}}) {
                int x0 = img.width, y0 = img.height, x1 = -1, y1 = -1;
                for (int y = 0; y < img.height; ++y) {
                    for (int x = 0; x < img.width; ++x) {
                        const std::uint8_t* p = &img.rgb[(static_cast<std::size_t>(y) * img.width + x) * 3];
                        if (std::abs(p[0] - r) < 30 && std::abs(p[1] - g) < 30 && std::abs(p[2] - b) < 30) {
                            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                        }
                    }
                }
                if (x1 < 0) {
                    continue;
                }
                vision::Detection det;
                det.box = vision::Box{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0 + 1), static_cast<float>(y1 - y0 + 1)};
                det.score = 0.9f;
                det.classId = 2; // a car
                d.objects.push_back(det);
                if (people) {
                    d.faces.push_back(det);
                    det.classId = vision::kCocoPerson;
                    d.objects.push_back(det);
                    d.people.push_back(det);
                }
            }
            out.push_back(std::move(d));
        }
        return out;
    }
    std::string modelVersion() const override { return "colour-v1"; }
    int calls = 0;
};

std::string clipPicture(int frames, int sourceIn = 0) {
    std::string clip = "A";
    if (sourceIn > 0) {
        clip += std::to_string(sourceIn);
    }
    return "V1 tgt |" + clip + std::string(static_cast<std::size_t>(frames - 1), 'a') + "|";
}

/// A project with one 100-frame clip of media A, the scripted segmentation services and the shots of A (a cut at 60).
struct Studio {
    explicit Studio(const std::string& name, ScriptedScene scene = {}, int sourceIn = 0)
        : tl(clipPicture(100, sourceIn)), seg(freshDir(name), scene) {
        seg.services->detector = detector;
        analysis::ShotAnalysis s;
        if (scene.cutFrame >= 0) {
            s.cuts.push_back(analysis::ShotCut{frameTicks(scene.cutFrame), scene.cutFrame, analysis::CutKind::Cut, 0.9f});
        }
        s.frames = scene.frames;
        s.end = frameTicks(scene.frames);
        shots->addShots(tl.media('A'), s);
    }

    std::shared_ptr<command::CommandServices> services() {
        return std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            seg.services, std::make_shared<analysis::AnalysisServices>(shots)});
    }
    CommandHarness harness() { return CommandHarness(tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, services()}, ai::edit::editingCommands()); }

    Timeline tl;
    ScriptedServices seg;
    std::shared_ptr<ColourDetector> detector = std::make_shared<ColourDetector>();
    std::shared_ptr<analysis::MapAnalysisProvider> shots = std::make_shared<analysis::MapAnalysisProvider>();
};

const Clip& clipIn(const ProjectSnapshot& s, SequenceId seq, ClipId id) {
    const auto loc = s->findSequence(seq)->findClip(id);
    REQUIRE(loc);
    return loc->clip();
}

const EffectInstance* effectNamed(const Clip& clip, std::string_view effect) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == effect) {
            return &e;
        }
    }
    return nullptr;
}

Json click(double x, double y, bool positive = true) { return Json{{"x", x}, {"y", y}, {"positive", positive}}; }

} // namespace

TEST_CASE("mask.objectSelect: a click makes a matte mask that follows the object to the cut; dry run, cache, undo",
          "[ai][segment][commands]") {
    Studio s("select");
    CommandHarness h = s.harness();
    const ClipId clip = h.clip("V1@0");
    const auto before = h.state();
    const Json params{{"clip", clip.value}, {"time", frameTicks(20).value}, {"points", Json::array({click(0.25, 0.4)})}};

    const auto dry = h.run(Json{{"command", "mask.objectSelect"}, {"params", params}, {"dryRun", true}});
    INFO(command::toJson(dry).dump());
    REQUIRE(dry.ok());
    CHECK(h.state() == before);
    CHECK(dry.data["frames"] == 60); // frames 0..59: the cut at 60 ends it
    CHECK(dry.data["lost"] == 0);
    CHECK(dry.data["cached"] == false);
    CHECK(dry.data["stoppedAtCutAfter"] == "00:00:02:10");
    CHECK(dry.data["stoppedAtCutBefore"].is_null());
    CHECK(dry.data["confidence"]["lowest"].get<double>() > 0.85);
    CHECK(dry.data["confidence"]["strip"].size() == 60);
    CHECK(dry.data["device"] == "TestCPU");
    CHECK(s.seg.opened == 1);
    CHECK(s.seg.framesReported == 60);
    REQUIRE(s.seg.remembered.size() == 1);

    const auto applied = h.ok("mask.objectSelect", params);
    CHECK(applied.data["cached"] == true); // the dry run's mattes, from the analysis cache
    CHECK(applied.data["segmented"] == 0);
    CHECK(s.seg.opened == 1);
    CHECK(h.session.undoCount() == 1);
    const ProjectSnapshot held = h.state(); // c refers into it: held, not a temporary
    const Clip& c = clipIn(held, h.tl.seq, clip);
    const EffectInstance* opacity = effectNamed(c, "wabcut.mask");
    REQUIRE(opacity);
    REQUIRE(opacity->masks.size() == 1);
    const Mask& m = opacity->masks.front();
    CHECK(m.id == applied.data["mask"].get<std::uint32_t>());
    CHECK(m.shape == MaskShape::Matte);
    CHECK(m.name == "Object");
    CHECK_FALSE(m.inverted);
    REQUIRE(m.matte);
    CHECK(m.matte->track == applied.data["track"].get<std::string>());
    CHECK(m.matte->media == h.tl.media('A'));
    CHECK(m.matte->stream == 0);
    CHECK(m.matte->source == TimeRange{Ticks{}, frameTicks(60)});
    CHECK(m.matte->model == "red-v1");
    CHECK(maskValue(m, "feather", Ticks{}) == Approx(kDefaultMatteFeather));
    // The bounding box keys hold the object's box at every frame (the fail-closed stand-in).
    const ScriptedScene scene;
    for (const int n : {0, 13, 20, 41, 59}) {
        const MatteBox truth = scene.objectAt(n);
        const Ticks t = frameTicks(n);
        const double cx = maskValue(m, "center.x", t), cy = maskValue(m, "center.y", t);
        const double w = maskValue(m, "size.x", t), hh = maskValue(m, "size.y", t);
        INFO("frame " << n);
        CHECK(cx - w / 2 <= truth.x + 0.005);
        CHECK(cx + w / 2 >= truth.x + truth.width - 0.005);
        CHECK(cy - hh / 2 <= truth.y + 0.005);
        CHECK(cy + hh / 2 >= truth.y + truth.height - 0.005);
        CHECK(w < truth.width + 0.02);
    }
    // The track is in the cache under the mask's identity.
    auto stored = MatteStore(s.seg.services->cacheRoot).get(m.matte->track);
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    CHECK((*stored)->frames.size() == 60);

    REQUIRE(h.session.undo());
    CHECK(effectNamed(clipIn(h.state(), h.tl.seq, clip), "wabcut.mask") == nullptr);
}

TEST_CASE("mask.objectSelect on an effect: blur the background (inverted), across the cut, within In / Out", "[ai][segment][commands]") {
    Studio s("effect");
    CommandHarness h = s.harness();
    const ClipId clip = h.clip("V1@0");
    const auto blur = h.ok("clip.addEffect", Json{{"clips", Json::array({clip.value})}, {"effect", "wabcut.blur.gaussian"}});
    REQUIRE(blur.createdIds.size() == 1);
    const std::uint64_t effect = blur.createdIds.front();
    const auto r = h.ok("mask.objectSelect", Json{{"clip", clip.value},
                                                  {"time", frameTicks(20).value},
                                                  {"box", Json{{"x", 0.15}, {"y", 0.25}, {"width", 0.25}, {"height", 0.3}}},
                                                  {"effect", effect},
                                                  {"inverted", true},
                                                  {"acrossShots", true},
                                                  {"in", frameTicks(10).value},
                                                  {"out", frameTicks(90).value},
                                                  {"name", "Speaker"},
                                                  {"feather", 0.01},
                                                  {"expansion", -0.002}});
    CHECK(r.data["effect"] == effect);
    CHECK(r.data["frames"] == 80);
    CHECK(r.data["stoppedAtCutAfter"].is_null());
    const ProjectSnapshot held = h.state(); // c refers into it: held, not a temporary
    const Clip& c = clipIn(held, h.tl.seq, clip);
    CHECK(effectNamed(c, "wabcut.mask") == nullptr);
    const EffectInstance* e = effectNamed(c, "wabcut.blur.gaussian");
    REQUIRE(e);
    REQUIRE(e->masks.size() == 1);
    const Mask& m = e->masks.front();
    CHECK(m.inverted);
    CHECK(m.name == "Speaker");
    CHECK(maskValue(m, "feather", Ticks{}) == Approx(0.01));
    CHECK(maskValue(m, "expansion", Ticks{}) == Approx(-0.002));
    CHECK(m.matte->source == TimeRange::fromStartEnd(frameTicks(10), frameTicks(90)));
    REQUIRE(s.seg.segmenter->lastPrompt.box);
    CHECK(s.seg.segmenter->lastPrompt.box->width == Approx(0.25));
}

TEST_CASE("mask.refine and mask.propagate: more clicks on a frame, tracking after none, corrections onwards", "[ai][segment][commands]") {
    Studio s("refine");
    CommandHarness h = s.harness();
    const ClipId clip = h.clip("V1@0");
    const auto first = h.ok("mask.objectSelect", Json{{"clip", clip.value}, {"time", frameTicks(20).value}, {"points", Json::array({click(0.25, 0.4)})},
                                                       {"propagate", "none"}});
    CHECK(first.data["frames"] == 1);
    const std::uint32_t mask = first.data["mask"];
    const std::string firstTrack = first.data["track"];

    // A negative click on the same frame: one prompt with two points; still that one frame.
    const auto refined = h.ok("mask.refine", Json{{"clip", clip.value}, {"mask", mask}, {"time", frameTicks(20).value},
                                                  {"points", Json::array({click(0.6, 0.8, false)})}});
    CHECK(refined.data["frames"] == 1);
    CHECK(refined.data["prompts"] == 1);
    CHECK(refined.data["track"] != firstTrack);
    CHECK(s.seg.segmenter->lastPrompt.points.size() == 2);
    CHECK_FALSE(s.seg.segmenter->lastPrompt.points[1].positive);
    CHECK(h.session.undoCount() == 2);

    // Track it through the shot.
    const auto tracked = h.ok("mask.propagate", Json{{"clip", clip.value}, {"mask", mask}});
    CHECK(tracked.data["frames"] == 60);
    CHECK(tracked.data["stoppedAtCutAfter"] == "00:00:02:10");
    const Mask* m = &effectNamed(clipIn(h.state(), h.tl.seq, clip), "wabcut.mask")->masks.front();
    CHECK(m->matte->source == TimeRange{Ticks{}, frameTicks(60)});
    CHECK(m->id == mask);

    // A correction on frame 40: re-tracked from there on, the frames before it kept.
    const int promptsBefore = s.seg.segmenter->prompts;
    const auto corrected = h.ok("mask.refine", Json{{"clip", clip.value}, {"mask", mask}, {"time", frameTicks(40).value},
                                                    {"points", Json::array({click(0.3, 0.4)})}});
    CHECK(corrected.data["frames"] == 60);
    CHECK(corrected.data["segmented"] == 20); // 40..59
    CHECK(corrected.data["prompts"] == 2);
    CHECK(s.seg.segmenter->prompts == promptsBefore + 1);

    // Undo walks back through each step.
    REQUIRE(h.session.undo());
    REQUIRE(h.session.undo());
    REQUIRE(h.session.undo());
    m = &effectNamed(clipIn(h.state(), h.tl.seq, clip), "wabcut.mask")->masks.front();
    CHECK(m->matte->track == firstTrack);
}

TEST_CASE("mask.objectSelect finds a subject by kind, colour and position", "[ai][segment][commands]") {
    SECTION("the person on the frame, through the engine") {
        Studio s("subject");
        CommandHarness h = s.harness();
        const ClipId clip = h.clip("V1@0");
        const auto r = h.ok("mask.objectSelect", Json{{"clip", clip.value}, {"time", frameTicks(30).value}, {"subject", "person"}, {"propagate", "none"}});
        CHECK(r.data["name"] == "Person");
        const MatteBox truth = ScriptedScene{}.objectAt(30);
        CHECK(r.data["box"]["x"].get<double>() == Approx(truth.x).margin(0.01));
        CHECK(r.data["box"]["width"].get<double>() == Approx(truth.width).margin(0.01));
        CHECK(s.detector->calls == 1);
        CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"subject", "unicorn"}}) == "InvalidParams");
        CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"subject", "dog"}}) == "EditRejected"); // none there
        CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"subject", "person"}, {"points", Json::array({click(0.2, 0.2)})}}) ==
              "InvalidParams");
    }
    SECTION("the red car, the blue car, the left and the right one") {
        ColourDetector detector;
        search::Image picture;
        picture.width = picture.height = 256;
        picture.rgb.assign(256 * 256 * 3, 100);
        const auto paint = [&](int x0, int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    std::uint8_t* p = &picture.rgb[(static_cast<std::size_t>(y) * 256 + x) * 3];
                    p[0] = r, p[1] = g, p[2] = b;
                }
            }
        };
        paint(20, 100, 80, 140, 220, 30, 40);   // red, left
        paint(150, 90, 240, 160, 30, 40, 220);  // blue, right and larger
        auto red = findSubject(detector, picture, 16.0 / 9.0, "car", "red", "largest");
        REQUIRE(red);
        CHECK(red->x < 0.5);
        auto blue = findSubject(detector, picture, 16.0 / 9.0, "car", "blue", "");
        REQUIRE(blue);
        CHECK(blue->x > 0.5);
        auto largest = findSubject(detector, picture, 16.0 / 9.0, "car", "", "largest");
        REQUIRE(largest);
        CHECK(largest->x > 0.5);
        auto left = findSubject(detector, picture, 16.0 / 9.0, "car", "", "left");
        REQUIRE(left);
        CHECK(left->x < 0.5);
        CHECK(findSubject(detector, picture, 16.0 / 9.0, "car", "green", "").error().code == ErrorCode::NotFound);
        CHECK(findSubject(detector, picture, 16.0 / 9.0, "car", "mauve", "").error().code == ErrorCode::InvalidArgument);
        CHECK(findSubject(detector, picture, 16.0 / 9.0, "car", "", "middle").error().code == ErrorCode::InvalidArgument);
        auto face = findSubject(detector, picture, 16.0 / 9.0, "face", "", "");
        REQUIRE(face);
        CHECK(face->x < 0.5);
    }
    SECTION("the COCO class names") {
        const auto names = cocoClassNames();
        REQUIRE(names.size() == 80);
        CHECK(names.front() == "person");
        CHECK(names[2] == "car");
        CHECK(names.back() == "toothbrush");
        CHECK(std::ranges::none_of(names, [](std::string_view n) { return n.empty(); }));
    }
}

namespace {

/// What a nested or multicam clip shows, scripted: the scene at nested time; angle 1 of a multicam source shows the
/// object elsewhere (the second shot's place). Records the clips it was asked for.
struct NestedPictures {
    std::vector<AngleSet> asked;
    std::shared_ptr<SegmentServices> attach(ScriptedServices& seg) {
        seg.services->nestedFrames = [this](const ProjectState&, const Clip& clip) -> std::expected<std::unique_ptr<FrameSource>, wabcut::Error> {
            asked.push_back(clip.angles);
            ScriptedScene scene;
            scene.cutFrame = clip.angles == AngleSet::of(1) ? 0 : -1; // angle 1: the object where the second shot has it
            return std::unique_ptr<FrameSource>(std::make_unique<ScriptedVideo>(scene));
        };
        return seg.services;
    }
};

const Mask& onlyObjectMask(const ProjectSnapshot& s, SequenceId seq, ClipId clip) {
    const EffectInstance* opacity = effectNamed(clipIn(s, seq, clip), "wabcut.mask");
    REQUIRE(opacity);
    REQUIRE(opacity->masks.size() == 1);
    return opacity->masks.front();
}

} // namespace

// 0.2 (C.17 / 0.1.2 follow-up): object masks were refused on nested and multicam clips. They now segment what the clip
// shows - the nest's picture, a multicam source's active angle - at nested time, and put the mask on the clip itself:
// nothing flattened, the matte names the nested sequence, the nest's edit points stop the tracking.
TEST_CASE("mask.objectSelect on a nested clip: the nest's picture, the mask on the nested clip, one undo step (0.2)",
          "[ai][segment][commands][nested]") {
    Studio s("nested");
    NestedPictures pictures;
    pictures.attach(s.seg);
    CommandHarness h = s.harness();
    const SequenceId seq = h.tl.seq;
    REQUIRE(h.session.apply("Nest", [&](const ProjectState& state) -> std::expected<ProjectState, wabcut::Error> {
        return wabcut::test::nestEverything(state, seq).edit.state;
    }));
    const ClipId nested = h.clip("V1@0");
    const SequenceId inner = clipIn(h.state(), seq, nested).sequence;
    REQUIRE(inner.isValid());
    const auto before = h.state();
    const std::size_t undo = h.session.undoCount();
    const Json params{{"sequence", seq.value}, {"clip", nested.value}, {"time", frameTicks(20).value}, {"points", Json::array({click(0.25, 0.4)})}};

    const auto dry = h.run(Json{{"command", "mask.objectSelect"}, {"params", params}, {"dryRun", true}});
    INFO(command::toJson(dry).dump());
    REQUIRE(dry.ok());
    CHECK(h.state() == before);
    CHECK(dry.data["frames"] == 100); // one clip inside: no edit point stops it
    CHECK(pictures.asked.size() == 1);

    const auto applied = h.ok("mask.objectSelect", params);
    CHECK(applied.data["cached"] == true);
    CHECK(h.session.undoCount() == undo + 1);
    const Clip& c = clipIn(h.state(), seq, nested);
    CHECK(c.isNested()); // still the nest
    const Mask& m = onlyObjectMask(h.state(), seq, nested);
    REQUIRE(m.matte);
    CHECK(m.shape == MaskShape::Matte);
    CHECK(m.matte->sequence == inner);
    CHECK_FALSE(m.matte->media.isValid());
    CHECK(m.matte->source == TimeRange{Ticks{}, frameTicks(100)});
    // The box keys follow the object at nested time (= the source's time here).
    const MatteBox truth = ScriptedScene{1000, -1}.objectAt(20);
    CHECK(maskValue(m, "center.x", frameTicks(20)) == Approx(truth.x + truth.width / 2).margin(0.02));
    // It saves and loads (schema 18).
    const auto text = io::toJson(*h.state());
    REQUIRE(text);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(onlyObjectMask(std::make_shared<const ProjectState>(*loaded), seq, nested) == m);

    // Refine and propagate go through the nest too.
    const std::uint32_t id = applied.data["mask"];
    const auto refined = h.ok("mask.refine", Json{{"sequence", seq.value}, {"clip", nested.value}, {"mask", id}, {"time", frameTicks(40).value},
                                                  {"points", Json::array({click(0.3, 0.4)})}});
    CHECK(refined.data["prompts"] == 2);
    CHECK(onlyObjectMask(h.state(), seq, nested).matte->sequence == inner);
    REQUIRE(h.session.undo());
    REQUIRE(h.session.undo());
    CHECK(effectNamed(clipIn(h.state(), seq, nested), "wabcut.mask") == nullptr);

    // A host that renders no nested pictures says how to do it instead.
    s.seg.services->nestedFrames = nullptr;
    const auto refused = h.run(Json{{"command", "mask.objectSelect"}, {"params", params}});
    REQUIRE(refused.error);
    CHECK(refused.error->code == command::ErrorCode::EditRejected);
    CHECK(refused.error->message.find("clip.openNested") != std::string::npos);
}

TEST_CASE("mask.objectSelect on a multicam clip: the active angle, never flattened; an angle switch keeps the mask (0.2)",
          "[ai][segment][commands][multicam]") {
    Studio s("multicam");
    NestedPictures pictures;
    pictures.attach(s.seg);
    Timeline tl(clipPicture(100) + "\nV2     |B" + std::string(99, 'b') + "|");
    const auto mc = wabcut::test::placeMulticam(tl.state, tl.seq, {tl.media('A'), tl.media('B')}, {Ticks{}, Ticks{}},
                                                TimeRange{Ticks{}, frameTicks(100)}, Ticks{}, 0, 0);
    tl.state = mc.state;
    CommandHarness h(tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, s.services()}, ai::edit::editingCommands());
    const SequenceId seq = h.tl.seq;
    const std::size_t clipsBefore = h.state()->findSequence(seq)->videoTracks.front()->clips.size();
    // The project has two sequences now (the multicam source): the commands name the edit's.
    const Json params{{"sequence", seq.value}, {"clip", mc.video.value}, {"time", frameTicks(20).value}, {"points", Json::array({click(0.25, 0.4)})}};

    const auto r = h.ok("mask.objectSelect", params);
    REQUIRE(pictures.asked.size() == 1);
    CHECK(pictures.asked.front() == AngleSet::of(0)); // angle 0's picture
    CHECK(r.data["frames"] == 100);
    const Clip& c = clipIn(h.state(), seq, mc.video);
    CHECK(c.isNested());
    CHECK(c.sequence == mc.multicam);
    CHECK(c.angles == AngleSet::of(0));
    CHECK(h.state()->findSequence(seq)->videoTracks.front()->clips.size() == clipsBefore); // not flattened
    const Mask first = onlyObjectMask(h.state(), seq, mc.video);
    CHECK(first.matte->sequence == mc.multicam);
    const std::uint32_t id = r.data["mask"];

    // Switch to angle 1: the mask stays on the clip, where it was on the frame.
    REQUIRE(h.session.apply("Switch", [&](const ProjectState& state) { return wabcut::edit::switchMulticamAngle(state, seq, mc.video, std::nullopt, 1); }));
    const Clip& switched = clipIn(h.state(), seq, mc.video);
    CHECK(switched.angles == AngleSet::of(1));
    CHECK(onlyObjectMask(h.state(), seq, mc.video) == first);

    // mask.propagate re-tracks the angle shown now: its object, the same mask.
    const auto again = h.ok("mask.propagate", Json{{"sequence", seq.value}, {"clip", mc.video.value}, {"mask", id}});
    REQUIRE(pictures.asked.size() == 2);
    CHECK(pictures.asked.back() == AngleSet::of(1));
    const Mask& moved = onlyObjectMask(h.state(), seq, mc.video);
    CHECK(moved.id == id);
    CHECK(moved.matte->sequence == mc.multicam);
    CHECK(moved.matte->track != first.matte->track);
    const MatteBox elsewhere = ScriptedScene{1000, 0}.objectAt(30);
    CHECK(maskValue(moved, "center.x", frameTicks(30)) == Approx(elsewhere.x + elsewhere.width / 2).margin(0.02));
    CHECK(again.data["mask"] == id);
    REQUIRE(h.session.undo());
    CHECK(onlyObjectMask(h.state(), seq, mc.video) == first);
}

TEST_CASE("nestedCuts and nestedFingerprint: the nest's edit points; edits elsewhere leave the identity (0.2)", "[ai][segment][nested]") {
    Timeline tl("V1 tgt |AaaaBbbb|");
    const auto nest = wabcut::test::nestEverything(tl.state, tl.seq);
    const ProjectState& state = nest.edit.state;
    const Clip* nested = nullptr;
    for (const Clip& c : state.findSequence(tl.seq)->videoTracks.front()->clips) {
        nested = c.isNested() ? &c : nested;
    }
    REQUIRE(nested);
    const std::vector<Ticks> cuts = nestedCuts(state, *nested);
    REQUIRE(cuts.size() == 1);
    CHECK(cuts.front() == frameTicks(4));
    const std::uint64_t id = nestedFingerprint(state, *nested);
    // A marker on the outer sequence does not change what the nest shows.
    const auto marked = wabcut::edit::addMarker(state, tl.seq, Marker{frameTicks(1), frameTicks(1), "Note"});
    REQUIRE(marked);
    CHECK(nestedFingerprint(marked->state, *nested) == id);
    // Removing a clip inside the nest does.
    const Sequence& inner = *state.findSequence(nested->sequence);
    const ClipId first = inner.videoTracks.front()->clips.front().id;
    const auto removed = wabcut::edit::deleteClips(state, nested->sequence, std::span(&first, 1));
    REQUIRE(removed);
    CHECK(nestedFingerprint(removed->state, *nested) != id);
}

TEST_CASE("Object masks refuse what they cannot do; nothing is committed", "[ai][segment][commands]") {
    Studio s("refusals");
    CommandHarness h = s.harness();
    const ClipId clip = h.clip("V1@0");
    const Json ok{{"clip", clip.value}, {"time", frameTicks(20).value}, {"points", Json::array({click(0.25, 0.4)})}, {"propagate", "none"}};

    CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}}) == "InvalidParams");          // no prompt
    CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"points", Json::array({click(1.2, 0.4)})}}) == "InvalidParams");
    CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"points", Json::array({click(0.2, 0.4)})}, {"time", frameTicks(200).value}}) ==
          "InvalidParams"); // outside the clip
    CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"box", Json{{"x", 0.5}, {"y", 0.5}, {"width", 0.0}, {"height", 0.1}}}}) ==
          "InvalidParams");
    CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"points", Json::array({click(0.2, 0.4)})}, {"in", frameTicks(50).value},
                                              {"out", frameTicks(40).value}}) == "InvalidParams");

    // The model failing mid-run: an error, nothing changed.
    s.seg.segmenter->failOnTrack = 5;
    const std::uint64_t revision = h.session.revision();
    CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"time", frameTicks(20).value}, {"points", Json::array({click(0.25, 0.4)})}}) ==
          "EditRejected");
    CHECK(h.session.revision() == revision);
    s.seg.segmenter->failOnTrack.reset();

    // Refine needs an object mask, and its mattes on this computer.
    const auto ellipse = h.ok("mask.add", Json{{"clip", clip.value}, {"shape", "ellipse"}});
    CHECK(h.errorOf("mask.refine", Json{{"clip", clip.value}, {"mask", ellipse.createdIds.front()}, {"points", Json::array({click(0.2, 0.4)})}}) ==
          "InvalidParams");
    const auto made = h.ok("mask.objectSelect", ok);
    std::filesystem::remove_all(s.seg.services->cacheRoot);
    const auto gone = h.run(Json{{"command", "mask.propagate"}, {"params", Json{{"clip", clip.value}, {"mask", made.data["mask"]}}}});
    REQUIRE(gone.error);
    CHECK(gone.error->code == command::ErrorCode::EditRejected);
    CHECK(gone.error->message.find("select the object again") != std::string::npos);

    // A subject needs the detector.
    s.seg.services->detector = nullptr;
    CHECK(h.errorOf("mask.objectSelect", Json{{"clip", clip.value}, {"subject", "person"}}) == "EditRejected");

    // Without the services (a host that decodes nothing).
    CommandHarness bare(s.tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, nullptr}, ai::edit::editingCommands());
    CHECK(bare.errorOf("mask.objectSelect", ok) == "InvalidRequest");

    // An audio clip has no picture.
    Studio sound("refusals-audio");
    Timeline both("V1 tgt |Aaaaaaaaaa|\nA1 tgt |Aaaaaaaaaa|");
    CommandHarness a(both, command::EnginePolicy{command::CapabilitySet::all(), 1024, sound.services()}, ai::edit::editingCommands());
    CHECK(a.errorOf("mask.objectSelect", Json{{"clip", a.clip("A1@0").value}, {"points", Json::array({click(0.2, 0.4)})}}) == "EditRejected");
}

TEST_CASE("Object masks: an object never seen fails; a stop request cancels a run, nothing committed", "[ai][segment][commands]") {
    SECTION("never seen") {
        Studio s("unseen", ScriptedScene{1000, -1}, 300); // frames 300.. : the object has left the frame
        CommandHarness h = s.harness();
        const auto r = h.run(Json{{"command", "mask.objectSelect"},
                                  {"params", Json{{"clip", h.clip("V1@0").value}, {"points", Json::array({click(0.5, 0.5)})}}}});
        REQUIRE(r.error);
        CHECK(r.error->code == command::ErrorCode::EditRejected);
        CHECK(r.error->message.find("found no object") != std::string::npos);
    }
    SECTION("cancel") {
        struct StopAfter final : command::ExecutionControl {
            int left = 10;
            std::optional<command::CommandError> checkpoint(std::string_view) override {
                return --left < 0 ? std::optional(command::CommandError{command::ErrorCode::Cancelled, "stopped", nullptr}) : std::nullopt;
            }
        } control;
        Studio s("cancel");
        CommandHarness h = s.harness();
        const std::uint64_t revision = h.session.revision();
        const auto r = h.engine.executeJson(Json{{"command", "mask.objectSelect"},
                                                 {"params", Json{{"clip", h.clip("V1@0").value}, {"time", frameTicks(20).value},
                                                                 {"points", Json::array({click(0.25, 0.4)})}}}},
                                            &control);
        REQUIRE(r.error);
        CHECK(r.error->code == command::ErrorCode::Cancelled);
        CHECK(h.session.revision() == revision);
        CHECK(s.seg.segmenter->tracked < 20);
    }
}

TEST_CASE("The object-mask commands in the catalog: EditSequence, strict schemas", "[ai][segment][commands]") {
    const command::CommandRegistry& registry = ai::edit::editingCommands();
    for (const char* name : {"mask.objectSelect", "mask.refine", "mask.propagate"}) {
        const command::CommandSpec* spec = registry.find(name);
        REQUIRE(spec);
        CHECK(spec->capability == command::Capability::EditSequence);
        CHECK(command::unsupportedSchemaKeywords(spec->params).empty());
        CHECK(command::validateAgainstSchema(spec->params, spec->example).empty());
        // Every object in the schema is closed (the providers' strict tool schemas refuse open ones).
        std::function<void(const Json&, const std::string&)> closed = [&](const Json& schema, const std::string& path) {
            if (!schema.is_object()) {
                return;
            }
            const Json type = schema.value("type", Json());
            const bool object = type == "object" || (type.is_array() && std::ranges::find(type, Json("object")) != type.end());
            if (object && schema.contains("properties")) {
                INFO(name << path);
                CHECK(schema.value("additionalProperties", true) == false);
            }
            if (schema.contains("properties")) {
                for (const auto& [key, value] : schema["properties"].items()) {
                    closed(value, path + "/" + key);
                }
            }
            if (schema.contains("items")) {
                closed(schema["items"], path + "/items");
            }
        };
        closed(spec->params, "");
    }
}

TEST_CASE("The assistant blurs the background behind the speaker and brightens the red car: staged, verified, undoable",
          "[ai][segment][agent]") {
    using namespace wabcut::ai::agent;
    Studio s("agent");
    EditSession session(s.tl.state);
    AgentOptions options;
    options.services = s.services();
    EditAgent agent(options);
    const auto tools = agent.tools();
    const auto offered = [&](const std::string& name) { return std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == name; }); };
    CHECK(offered("mask_objectSelect"));
    CHECK(offered("mask_refine"));
    CHECK(offered("mask_propagate"));
    CHECK_FALSE(offered("redaction_add")); // AI redactions stay in the user's hands
    CHECK_FALSE(offered("redaction_detectFaces")); // automatic face redaction too (0.2)

    const ClipId clip = wabcut::test::clipAt(s.tl.state, s.tl.seq, "V1@0");
    // The effect instance a staged clip.addEffect will create on the session's current state (ids are allocated
    // deterministically, and never reused after an undo).
    const auto nextEffect = [&] {
        EditSession probe(session.stateForSave());
        command::CommandEngine probeEngine(probe, ai::edit::editingCommands());
        const auto made = probeEngine.executeJson(
            Json{{"command", "clip.addEffect"}, {"params", Json{{"clips", Json::array({clip.value})}, {"effect", "wabcut.blur.gaussian"}}}});
        REQUIRE(made.ok());
        return made.createdIds.front();
    };
    const std::uint64_t effect = nextEffect();

    const auto requestFor = [&](std::string prompt) {
        AgentRequest r;
        r.prompt = std::move(prompt);
        r.sequence = s.tl.seq;
        r.base = std::make_shared<const ProjectState>(session.stateForSave());
        r.baseRevision = session.revision();
        r.constraints = parseConstraints(r.prompt, *r.base, s.tl.seq);
        return r;
    };

    ai::test::ScriptedModel model;
    model.tool("clip_addEffect", Json{{"clips", Json::array({clip.value})}, {"effect", "wabcut.blur.gaussian"}, {"params", Json{{"radius", 20}}}})
        .tool("mask_objectSelect", Json{{"clip", clip.value}, {"time", frameTicks(20).value}, {"subject", "person"}, {"effect", effect}, {"inverted", true}})
        .text("The background is blurred; the speaker stays sharp, tracked through the shot.");
    const auto before = session.snapshot();
    const AgentOutcome out = agent.run(model, requestFor("blur the background behind the speaker"), {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verification.passed());
    REQUIRE(out.batch.commands.size() == 2);
    CHECK(out.batch.commands[1].command == "mask.objectSelect");
    CHECK(session.snapshot() == before);

    ApplyOptions apply;
    apply.services = s.services();
    const ApplyResult applied = applyStaged(session, out, apply);
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    const ProjectSnapshot held = session.snapshot(); // c refers into it: held, not a temporary
    const Clip& c = clipIn(held, s.tl.seq, clip);
    const EffectInstance* blur = effectNamed(c, "wabcut.blur.gaussian");
    REQUIRE(blur);
    REQUIRE(blur->masks.size() == 1);
    CHECK(blur->masks.front().inverted);
    CHECK(blur->masks.front().shape == MaskShape::Matte);
    REQUIRE(session.undo());
    CHECK(*session.snapshot() == *before);

    // "Isolate the red car and brighten it": a grade limited to the car.
    const std::uint64_t grade = nextEffect();
    ai::test::ScriptedModel brighten;
    brighten.tool("clip_addEffect", Json{{"clips", Json::array({clip.value})}, {"effect", "wabcut.color.basic"}})
        .tool("mask_objectSelect", Json{{"clip", clip.value}, {"time", frameTicks(20).value}, {"subject", "car"}, {"colour", "red"}, {"effect", grade}})
        .text("The red car is masked; the correction applies only to it.");
    const AgentOutcome car = agent.run(brighten, requestFor("isolate the red car and brighten it"), {}, {});
    INFO(car.error << "\n" << car.verification.failuresForModel());
    REQUIRE(car.status == AgentStatus::Staged);
    REQUIRE(car.batch.commands.size() == 2);
    CHECK(car.batch.commands[1].params["colour"] == "red");
}

// 0.2: automatic face redaction. The detector's faces (here: the red object) are followed and each covered by one
// moving source-anchored redaction; the tracking starts again at the shot cut (the object jumps there: a second face).
TEST_CASE("redaction.detectFaces: one moving redaction per face, fail closed, one undo step (0.2)", "[ai][segment][commands][redaction]") {
    Studio s("faces");
    CommandHarness h = s.harness();
    const ClipId clip = h.clip("V1@0");
    const auto before = h.state();
    const Json params{{"clip", clip.value}, {"style", "mosaic"}, {"samplesPerSecond", 5}};

    const auto dry = h.run(Json{{"command", "redaction.detectFaces"}, {"params", params}, {"dryRun", true}});
    INFO(command::toJson(dry).dump());
    REQUIRE(dry.ok());
    CHECK(h.state() == before);
    CHECK(dry.data["samples"] == 20); // 100 frames at 25 fps, 5 a second
    CHECK(dry.data["faces"] == 2);    // before and after the cut

    const auto made = h.ok("redaction.detectFaces", params);
    CHECK(h.session.undoCount() == 1);
    const auto& redactions = h.state()->redactions;
    REQUIRE(redactions.size() == 2);
    const Redaction& first = *redactions.begin();
    CHECK(first.media == h.tl.media('A'));
    CHECK(first.name == "Face 1");
    CHECK(first.style == RedactionStyle::Mosaic);
    CHECK(first.region.shape == MaskShape::Ellipse);
    CHECK(first.source.start == Ticks{});
    CHECK(first.source.end() >= frameTicks(55)); // the last sighting (frame 55) and a margin
    CHECK(first.source.end() <= frameTicks(60));
    // The ellipse follows the face and covers it with a margin, between the samples too.
    const ScriptedScene scene;
    for (const int n : {0, 12, 30, 47}) {
        INFO("frame " << n);
        const MatteBox face = scene.objectAt(n);
        const double cx = maskValue(first.region, "center.x", frameTicks(n));
        const double w = maskValue(first.region, "size.x", frameTicks(n));
        CHECK(cx == Approx(face.x + face.width / 2).margin(0.02));
        CHECK(w > face.width);
    }
    const Redaction& second = *std::next(redactions.begin());
    CHECK(second.name == "Face 2");
    CHECK(second.source.start <= frameTicks(60));
    CHECK(maskValue(second.region, "center.x", frameTicks(80)) == Approx(0.725).margin(0.02));
    REQUIRE(made.data["redactions"].size() == 2);
    REQUIRE(h.session.undo());
    CHECK(h.state()->redactions.empty());

    // No face: nothing redacted, said so.
    Studio empty("faces-none", ScriptedScene{1000, -1}, 300);
    CommandHarness none = empty.harness();
    const auto nothing = none.ok("redaction.detectFaces", Json{{"clip", none.clip("V1@0").value}});
    CHECK(nothing.data["faces"] == 0);
    REQUIRE_FALSE(nothing.warnings.empty());
    CHECK(none.state()->redactions.empty());

    // No detector: refused with the reason.
    s.seg.services->detector = nullptr;
    const auto refused = h.run(Json{{"command", "redaction.detectFaces"}, {"params", params}});
    REQUIRE(refused.error);
    CHECK(refused.error->code == command::ErrorCode::EditRejected);
    CHECK(refused.error->message.find("face detector") != std::string::npos);
    // A media item's span by media time works as well as a clip.
    s.seg.services->detector = s.detector;
    const auto byMedia = h.ok("redaction.detectFaces", Json{{"media", h.tl.media('A').value}, {"in", frameTicks(70).value}, {"out", frameTicks(90).value}});
    CHECK(byMedia.data["faces"] == 1);
}
