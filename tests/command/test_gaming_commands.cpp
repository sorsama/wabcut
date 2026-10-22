// C.16 through the engine: clip.instantReplay, clip.zoomPunch, clip.shake, sequence.removeDeadTime (preview, then apply)
// and sequence.makeVersion, each one undo step; the built-in template library in template.list, template.resolve and
// template.instantiate.

#include "CommandHarness.h"
#include "NestedTestUtil.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/BuiltinTemplates.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
using Catch::Approx;

namespace {

Json dryRun(std::string_view name, Json params) {
    return Json{{"command", name}, {"params", std::move(params)}, {"dryRun", true}};
}

/// Activity for media A: busy, quiet from 2 s to 7 s, busy again. Other media fail to measure.
std::shared_ptr<command::ActivityServices> fakeActivity(MediaId a, int* calls = nullptr) {
    return std::make_shared<command::ActivityServices>(
        [a, calls](const ProjectState&, MediaId media, const std::function<bool(double)>& progress)
            -> std::expected<MediaActivity, Error> {
            if (calls) {
                ++*calls;
            }
            if (!progress(1.0)) {
                return std::unexpected(Error{ErrorCode::Cancelled, "stopped"});
            }
            if (media != a) {
                return std::unexpected(Error{ErrorCode::NotFound, "offline"});
            }
            MediaActivity out;
            out.step = Ticks{kTicksPerSecond / 10};
            for (int i = 0; i < 400; ++i) {
                const bool quiet = i >= 20 && i < 70;
                out.audio.push_back(quiet ? 0.0F : 0.7F);
                out.motion.push_back(quiet ? 0.0F : 0.5F);
            }
            return out;
        });
}

std::string longClip(char letter, int frames) {
    return std::string(1, letter) + std::string(static_cast<std::size_t>(frames - 1), static_cast<char>(letter - 'A' + 'a'));
}

} // namespace

TEST_CASE("clip.instantReplay replays a clip with the built-in stinger and flashes in one undo step (C.16)", "[command][gaming]") {
    CommandHarness h("    V1 tgt |" + longClip('A', 40) + longClip('B', 10) + "|\n    A1 tgt |" + longClip('A', 40) +
                     longClip('B', 10) + "|\n");
    const ClipId a = h.clip("V1@0");
    // A dry run changes nothing.
    REQUIRE(h.run(dryRun("clip.instantReplay", Json{{"clip", a.value}})).ok());
    CHECK(h.session.undoCount() == 0);

    const auto r = h.ok("clip.instantReplay", Json{{"clip", a.value}, {"speed", 0.5}, {"labelText", "รีเพลย์"}});
    CHECK(h.session.undoCount() == 1);
    CHECK(r.data["moment"]["startTicks"] == 0);
    CHECK(r.data["moment"]["endTicks"] == f(40).value);
    CHECK(r.data["replay"]["startTicks"] == f(40).value);
    REQUIRE(r.data["clips"].size() == 1);
    CHECK(r.data["flashes"].size() == 2);
    REQUIRE(r.data["label"].is_number());
    const auto snap = h.state();
    const Clip& label = h.sequence(snap).findClip(ClipId{r.data["label"].get<std::uint64_t>()})->clip();
    REQUIRE(label.graphic());
    REQUIRE(label.graphic()->instance.has_value());
    CHECK(label.graphic()->instance->templateId == kReplayTemplateId);
    bool thai = false;
    for (const TemplateField& field : label.graphic()->instance->fields) {
        thai = thai || (field.id == "label" && field.value.text == "รีเพลย์");
    }
    CHECK(thai);
    const Clip& replay = h.sequence(snap).findClip(ClipId{r.data["clips"][0].get<std::uint64_t>()})->clip();
    CHECK(replay.isRemapped());
    h.session.undo();
    CHECK(h.sequence(h.state()).contentEnd() == f(50));

    // A range from the sequence In/Out marks, no stinger or flash.
    h.ok("marker.markIn", Json{{"time", tc(10)}});
    h.ok("marker.markOut", Json{{"time", tc(19)}});
    const auto ranged = h.ok("clip.instantReplay", Json{{"label", false}, {"flash", false}, {"ramp", false}});
    CHECK(ranged.data["moment"]["startTicks"] == f(10).value);
    CHECK(ranged.data["moment"]["endTicks"] == f(20).value);
    CHECK(ranged.data["replay"]["endTicks"] == f(40).value);
    CHECK(ranged.data["label"].is_null());

    // Errors.
    CHECK(h.errorOf("clip.instantReplay", Json{{"clip", a.value}, {"in", tc(1)}}) == "InvalidParams");
    CHECK(h.errorOf("clip.instantReplay", Json{{"clip", a.value}, {"labelTemplate", "no.such"}}) == "NotFound");
    CHECK(h.errorOf("clip.instantReplay", Json{{"clip", a.value}, {"speed", 2.0}}) == "InvalidParams");
    CHECK(h.errorOf("clip.instantReplay", Json{{"clip", h.clip("A1@0").value}}) == "EditRejected");
    CommandHarness unmarked("    V1 |Aaaaaaaaaa|\n");
    CHECK(unmarked.errorOf("clip.instantReplay", Json::object()) == "InvalidParams");
}

TEST_CASE("clip.zoomPunch and clip.shake key Motion in one undo step each (C.16)", "[command][gaming]") {
    CommandHarness h("    V1 tgt |" + longClip('A', 50) + "|\n");
    const ClipId a = h.clip("V1@0");
    h.ok("clip.zoomPunch", Json{{"clip", a.value}, {"at", tc(20)}, {"scale", 1.5}, {"seconds", 0.4}, {"easing", "easeOut"}});
    CHECK(h.session.undoCount() == 1);
    {
        const auto snap = h.state();
        const Clip& c = h.sequence(snap).findClip(a)->clip();
        CHECK(c.parameterAt(ParamId::Scale, f(20)) == Approx(1.5));
        CHECK(c.parameterAt(ParamId::Scale, f(40)) == Approx(1.0));
    }
    h.ok("clip.shake", Json{{"clip", a.value}, {"at", tc(30)}, {"intensity", 40}, {"seed", 3}});
    CHECK(h.session.undoCount() == 2);
    {
        const auto snap = h.state();
        const Clip& c = h.sequence(snap).findClip(a)->clip();
        CHECK(c.properties()[ParamId::PositionX].isCurve());
        CHECK(c.parameterAt(ParamId::Scale, f(20)) == Approx(1.5)); // the punch is still there
    }
    h.session.undo();
    CHECK(h.sequence(h.state()).findClip(a)->clip().properties()[ParamId::PositionX].isConstant());
    CHECK(h.errorOf("clip.zoomPunch", Json{{"clip", a.value}, {"at", tc(20)}, {"scale", 20}}) == "InvalidParams");
    CHECK(h.errorOf("clip.zoomPunch", Json{{"clip", a.value}, {"at", tc(20)}, {"easing", "bounce"}}) == "InvalidParams");
    CHECK(h.errorOf("clip.zoomPunch", Json{{"clip", a.value}, {"at", tc(70)}}) == "EditRejected");
    CHECK(h.errorOf("clip.shake", Json{{"clip", a.value}, {"at", tc(10)}, {"frequency", 100}}) == "InvalidParams");
}

TEST_CASE("sequence.removeDeadTime previews the quiet stretches then removes them in one undo step (C.16)", "[command][gaming]") {
    const std::string picture = "    V1 tgt |" + longClip('A', 250) + longClip('B', 50) + "|\n    A1 tgt |" + longClip('A', 250) +
                                longClip('B', 50) + "|\n";
    // No way to measure: refused, but given ranges still apply.
    {
        CommandHarness bare(picture);
        CHECK(bare.errorOf("sequence.removeDeadTime", Json::object()) == "InvalidRequest");
        const auto applied = bare.ok("sequence.removeDeadTime",
                                     Json{{"apply", true}, {"ranges", Json::array({Json{{"start", tc(50)}, {"end", tc(100)}}})}});
        CHECK(applied.data["applied"] == true);
        CHECK(bare.sequence(bare.state()).contentEnd() == f(250));
        CHECK(bare.errorOf("sequence.removeDeadTime", Json{{"ranges", Json::array({Json{{"start", tc(1)}, {"end", tc(2)}}})}}) ==
              "InvalidParams");
    }
    Timeline tl(picture);
    tl.linkMatching();
    int calls = 0;
    command::EnginePolicy policy;
    policy.services = fakeActivity(tl.media('A'), &calls);
    CommandHarness h(std::move(tl), policy);
    const auto preview = h.ok("sequence.removeDeadTime", Json{{"minSeconds", 2}, {"includeCurve", true}});
    CHECK(preview.data["applied"] == false);
    REQUIRE(preview.data["ranges"].size() == 1);
    const Json& range = preview.data["ranges"][0];
    CHECK(range["startTicks"].get<std::int64_t>() > Ticks::fromSeconds(2).value);
    CHECK(range["endTicks"].get<std::int64_t>() < Ticks::fromSeconds(7).value);
    CHECK(preview.data["curve"]["activity"].size() == 120);
    CHECK(h.session.undoCount() == 0);
    CHECK(calls == 2); // A and B, once each
    REQUIRE(preview.warnings.size() == 1); // B could not be measured: kept
    CHECK(preview.warnings[0].find("offline") != std::string::npos);

    const auto applied = h.ok("sequence.removeDeadTime", Json{{"minSeconds", 2}, {"apply", true}});
    CHECK(applied.data["applied"] == true);
    CHECK(h.session.undoCount() == 1);
    const Ticks removed{range["endTicks"].get<std::int64_t>() - range["startTicks"].get<std::int64_t>()};
    CHECK(h.sequence(h.state()).contentEnd() == f(300) - removed);
    h.session.undo();
    CHECK(h.sequence(h.state()).contentEnd() == f(300));
    CHECK(h.errorOf("sequence.removeDeadTime", Json{{"sensitivity", 2}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.removeDeadTime", Json{{"inOut", true}}) == "InvalidParams");
}

TEST_CASE("sequence.removeDeadTime measures a multicam clip's active angle and cuts it in place in one undo step (0.2)",
          "[command][gaming][multicam]") {
    const std::string picture = "    V1 tgt |" + longClip('A', 250) + "|\n    A1 tgt |" + longClip('A', 250) + "|\n";
    Timeline tl(picture, {{'A', 250}, {'B', 250}});
    tl.linkMatching();
    const PlacedMulticam mc = placeMulticam(tl.state, tl.seq, {tl.media('A'), tl.media('B')}, {Ticks{}, Ticks{}}, TimeRange{Ticks{}, f(250)}, Ticks{});
    tl.state = mc.state;
    int calls = 0;
    command::EnginePolicy policy;
    policy.services = fakeActivity(tl.media('A'), &calls);
    CommandHarness h(std::move(tl), policy);
    const Json params{{"sequence", h.tl.seq.value}, {"minSeconds", 2}};
    const auto preview = h.ok("sequence.removeDeadTime", params);
    REQUIRE(preview.data["ranges"].size() == 1);
    CHECK(preview.warnings.empty()); // angle 2 (B, offline here) is neither shown nor heard: not measured
    CHECK(calls == 1);
    Json apply = params;
    apply["apply"] = true;
    const auto before = h.state();
    REQUIRE(h.run(dryRun("sequence.removeDeadTime", apply)).ok());
    CHECK(h.state() == before);
    h.ok("sequence.removeDeadTime", apply);
    CHECK(h.session.undoCount() == 1);
    const Sequence& after = h.sequence(h.state());
    CHECK(after.videoTracks[0]->clips.size() == 2);
    for (const auto* tracks : {&after.videoTracks, &after.audioTracks}) {
        for (const TrackPtr& t : *tracks) {
            for (const Clip& c : t->clips) {
                CHECK(c.sequence == mc.multicam); // cut in place, never flattened
            }
        }
    }
    h.session.undo();
    CHECK(*h.state() == *before);
}

TEST_CASE("sequence.makeVersion adds a Shorts or square sequence in one undo step (C.16)", "[command][gaming]") {
    CommandHarness h(R"(
        V2 tgt |Ffffffffff|
        V1 tgt |Gggggggggg|
        A1 tgt |Gggggggggg|
    )");
    h.ok("track.setState", Json{{"track", "V2"}, {"name", "Facecam"}});
    const auto shorts = h.ok("sequence.makeVersion", Json::object());
    CHECK(shorts.data["width"] == 1080);
    CHECK(shorts.data["height"] == 1920);
    CHECK(shorts.data["method"] == "layout");
    CHECK(shorts.data["clipsLaidOut"] == 1);
    const SequenceId made{shorts.data["sequence"].get<std::uint64_t>()};
    REQUIRE(h.state()->findSequence(made));
    CHECK(std::find(shorts.createdIds.begin(), shorts.createdIds.end(), made.value) != shorts.createdIds.end());
    // Two sequences now: the source is named from here on.
    const auto square = h.ok("sequence.makeVersion", Json{{"sequence", h.tl.seq.value}, {"format", "square"}, {"name", "Square cut"},
                                                          {"corner", "topLeft"}});
    CHECK(square.data["height"] == 1080);
    CHECK(square.data["name"] == "Square cut");
    h.session.undo();
    CHECK_FALSE(h.state()->findSequence(SequenceId{square.data["sequence"].get<std::uint64_t>()}));
    CHECK(h.errorOf("sequence.makeVersion", Json{{"sequence", h.tl.seq.value}, {"format", "wide"}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.makeVersion", Json{{"sequence", h.tl.seq.value}, {"method", "reframe"}}) == "EditRejected");
    CHECK(h.errorOf("sequence.makeVersion", Json{{"sequence", made.value}}) == "EditRejected"); // a vertical source

    // The host's Auto Reframe for an edit without a facecam.
    int calls = 0;
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::VersionServices>(
        [&calls](const ProjectState& state, SequenceId source, int, int, std::string name) -> std::expected<ReframedSequence, Error> {
            ++calls;
            auto copied = edit::copySequenceFrom(state, state, source, std::move(name));
            if (!copied) {
                return std::unexpected(copied.error());
            }
            return ReframedSequence{std::move(copied->state), copied->id, {}};
        });
    CommandHarness plain(Timeline("    V1 |Gggggggggg|\n"), policy);
    const auto reframed = plain.ok("sequence.makeVersion", Json::object());
    CHECK(reframed.data["method"] == "reframe");
    CHECK(calls == 1);
    CHECK(plain.ok("sequence.makeVersion", Json{{"sequence", plain.tl.seq.value}, {"method", "blurFill"}}).data["method"] ==
          "blurFill");
}

TEST_CASE("The built-in templates are listed - resolved for thumbnails and placed by id (C.16)", "[command][gaming][templates]") {
    CommandHarness h("    V1 tgt |" + longClip('A', 50) + "|\n");
    const auto list = h.ok("template.list", Json{{"library", "builtin"}});
    REQUIRE(list.data["builtin"].size() == builtinTemplates().size());
    CHECK(list.data["builtin"].size() == 12);
    CHECK(list.data["project"].empty());
    for (const Json& t : list.data["builtin"]) {
        CHECK(t["category"] == "gaming");
        CHECK(t["posterTime"].get<std::int64_t>() >= 0);
    }
    CHECK(h.ok("template.list").data["builtin"].size() == 12); // "all" includes them
    const auto resolved = h.ok("template.resolve", Json{{"template", "wabcut.gaming.lowerThird"}});
    CHECK(resolved.data["library"] == "builtin");
    CHECK_FALSE(resolved.data["items"].empty());
    CHECK(h.errorOf("template.resolve", Json{{"template", "wabcut.gaming.lowerThird"}, {"values", Json{{"nope", "x"}}}}) ==
          "InvalidParams");
    const auto placed = h.ok("template.instantiate", Json{{"template", std::string(kReplayTemplateId)}, {"at", tc(5)}});
    CHECK(placed.data["library"] == "builtin");
    CHECK(h.errorOf("template.instantiate", Json{{"template", "wabcut.gaming.nope"}, {"at", tc(5)}}) == "NotFound");
}

TEST_CASE("Templates and titles added without a track go above the footage instead of overwriting it", "[command][gaming][templates]") {
    CommandHarness h("    V1 tgt |" + longClip('A', 50) + "|\n    A1 tgt |" + longClip('A', 50) + "|\n");
    const ClipId footage = h.clip("V1@0");
    const auto placed = h.ok("template.instantiate", Json{{"template", "wabcut.gaming.lowerThird"}, {"at", tc(10)}});
    const ClipId graphic{placed.data["clip"].get<std::uint64_t>()};
    const auto snap = h.state();
    const Sequence& seq = h.sequence(snap);
    // The footage is untouched; the graphic is on V2.
    REQUIRE(seq.findClip(footage));
    CHECK(seq.findClip(footage)->clip().timelineRange == TimeRange{Ticks{}, f(50)});
    CHECK(seq.videoTracks[0]->clips.size() == 1);
    REQUIRE(seq.findClip(graphic));
    CHECK(seq.trackPosition(seq.findClip(graphic)->track->id)->index == 1);
    // A title the same way; a named track still overwrites there (explicit).
    h.ok("title.add", Json{{"text", "Chapter 1"}, {"at", tc(20)}});
    CHECK(h.sequence(h.state()).videoTracks[0]->clips.size() == 1);
    h.ok("template.instantiate", Json{{"template", "wabcut.gaming.lowerThird"}, {"at", tc(30)}, {"track", "V1"}});
    CHECK(h.sequence(h.state()).videoTracks[0]->clips.size() == 2); // the footage's tail is overwritten (5 s graphic)
}
