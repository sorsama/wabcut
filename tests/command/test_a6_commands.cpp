// 0.7, batch A6 through the command engine: clip.lock (card 064), sequence.moveTrack (card 067), and sequence.matchFrame
// on a nested clip (card 097).

#include "CommandHarness.h"

#include <wabcut/model/ClipLock.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

Json in(const CommandHarness& h, Json params) {
    params["sequence"] = h.tl.seq.value;
    return params;
}

std::string refusal(CommandHarness& h, std::string_view name, Json params) {
    const auto result = h.run(Json{{"command", name}, {"params", std::move(params)}});
    return result.ok() ? std::string() : result.error->message;
}

} // namespace

TEST_CASE("clip.lock locks and unlocks clips, one undo step each; a locked clip refuses edits through commands",
          "[command][clip-lock]") {
    CommandHarness h(R"(
        V1 tgt |AaaaBbbbCccc|
        A1 tgt |AaaaBbbbCccc|
    )");
    const ClipId b = h.clip("V1@4");
    const auto locked = h.ok("clip.lock", Json{{"clips", Json::array({at("V1", 4)})}, {"locked", true}});
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "Lock Clip");
    CHECK(h.state()->findSequence(h.tl.seq)->findClip(b)->clip().locked);
    CHECK_FALSE(h.state()->findSequence(h.tl.seq)->findClip(h.clip("A1@4"))->clip().locked);
    CHECK(h.ok("clip.details", in(h, Json{{"clip", at("V1", 4)}})).data["locked"] == true);
    CHECK(h.ok("clip.details", in(h, Json{{"clip", at("V1", 0)}})).data["locked"] == false);

    // Everything that would change it is refused with the clip's name; nothing is stored.
    const std::size_t steps = h.session.undoCount();
    CHECK(refusal(h, "clip.move", Json{{"clips", Json::array({at("V1", 4)})}, {"delta", tc(2)}}) == "clip 'B' is locked");
    CHECK(refusal(h, "edit.clear", Json{{"clips", Json::array({at("V1", 4)})}}) == "clip 'B' is locked");
    CHECK(refusal(h, "clip.rename", Json{{"clips", Json::array({at("V1", 4)})}, {"name", "x"}}) == "clip 'B' is locked");
    // A ripple that would push it, and a move that would overwrite it, are refused too.
    CHECK(refusal(h, "edit.rippleDelete", Json{{"clips", Json::array({at("V1", 0)})}}) == "clip 'B' is locked");
    CHECK(refusal(h, "clip.move", Json{{"clips", Json::array({at("V1", 8)})}, {"delta", "-" + tc(4)}}) == "clip 'B' is locked");
    CHECK(h.session.undoCount() == steps);
    CHECK_SESSION(h, R"(
        V1 |AaaaBbbbCccc|
        A1 |AaaaBbbbCccc|
    )");

    // Unlock: it is edited again.
    h.ok("clip.lock", Json{{"clips", Json::array({at("V1", 4)})}, {"locked", false}});
    CHECK(h.session.undoLabel() == "Lock Clip");
    h.ok("edit.clear", Json{{"clips", Json::array({at("V1", 4)})}});
    CHECK_SESSION(h, R"(
        V1 |Aaaa....Cccc|
        A1 |AaaaBbbbCccc|
    )");
    CHECK(h.errorOf("clip.lock", Json{{"clips", Json::array({at("V1", 99)})}, {"locked", true}}) != "");
}

TEST_CASE("clip.move with copy and mode insert is the insert-copy the timeline's Alt-then-Ctrl drag makes", "[command][clip-lock]") {
    CommandHarness h(R"(
        V1 tgt |AaaaBbbbCccc....|
    )");
    // A locked clip can be copied: the copy is unlocked, the original stays where it is and locked.
    h.ok("clip.lock", Json{{"clips", Json::array({at("V1", 4)})}, {"locked", true}});
    const auto moved = h.ok("clip.move", Json{{"clips", Json::array({at("V1", 0)})}, {"delta", tc(8)}, {"mode", "insert"}, {"copy", true},
                                              {"linked", false}});
    CHECK(moved.createdIds.size() == 1);
    // Inserting at 8 would push locked B? No: B is before 8; C moves later, B is untouched.
    CHECK_SESSION(h, R"(
        V1 |AaaaBbbbAaaaCccc|
    )");
    const ClipId copy{moved.createdIds.front()};
    CHECK_FALSE(h.state()->findSequence(h.tl.seq)->findClip(copy)->clip().locked);
    // Copying a locked clip in front of itself would push it: refused, as any edit that moves a locked clip.
    CHECK(refusal(h, "clip.move", Json{{"clips", Json::array({at("V1", 0)})}, {"delta", tc(2)}, {"mode", "insert"}, {"copy", true},
                                       {"linked", false}}) == "clip 'B' is locked");
}

TEST_CASE("sequence.moveTrack moves a track within its stack as one undo step", "[command][track-move]") {
    CommandHarness h(R"(
        V2     |Bbbb|
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
        A2     |Cccc|
    )");
    h.ok("sequence.moveTrack", Json{{"track", "V1"}, {"position", 2}});
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "Move Track");
    CHECK_SESSION(h, R"(
        V2 |Aaaa|
        V1 |Bbbb|
        A1 |Aaaa|
        A2 |Cccc|
    )");
    h.ok("sequence.moveTrack", Json{{"track", "A2"}, {"position", 1}});
    CHECK_SESSION(h, R"(
        V2 |Aaaa|
        V1 |Bbbb|
        A1 |Cccc|
        A2 |Aaaa|
    )");
    CHECK(h.errorOf("sequence.moveTrack", Json{{"track", "V9"}, {"position", 1}}) != "");
    CHECK(h.errorOf("sequence.moveTrack", Json{{"track", "V1"}, {"position", 0}}) != ""); // positions start at 1
    // Dry run: nothing changes.
    const auto before = h.state();
    const auto dry = h.run(Json{{"command", "sequence.moveTrack"}, {"params", Json{{"track", "V1"}, {"position", 2}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(h.state() == before);
}

TEST_CASE("sequence.matchFrame on a nested clip names the source sequence and the frame it shows there", "[command][matchframe][nesting]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
        A1 tgt |AaaaBbbb....|
    )");
    tl.linkMatching();
    CommandHarness h(std::move(tl));
    const auto nested = h.ok("sequence.nest", Json{{"clips", Json::array({at("V1", 0), at("V1", 4)})}, {"name", "Talk"}});
    const std::uint64_t nestedId = nested.data["nested"];
    // Move the nest later, so its time in the sequence differs from the time inside it.
    h.ok("clip.move", in(h, Json{{"clips", Json::array({at("V1", 0)})}, {"delta", tc(3)}}));
    CHECK_SESSION(h, R"(
        V1 |...Tttttttt.|
        A1 |...Tttttttt.|
    )");
    const std::size_t steps = h.session.undoCount();
    const auto r = h.ok("sequence.matchFrame", in(h, Json{{"time", tc(8)}}));
    CHECK(r.data["nested"] == true);
    CHECK(r.data["multicam"] == false);
    CHECK(r.data["sequence"] == nestedId);
    CHECK(r.data["name"] == "Talk");
    CHECK(r.data["track"] == "V1");
    CHECK(r.data["sequenceTime"]["timecode"] == tc(5)); // frame 8 of the sequence is frame 5 of the nest
    CHECK(r.data["sequenceTime"]["ticks"] == f(5).value);
    CHECK(h.session.undoCount() == steps); // it stores nothing: a nest has no source marks to set
    CHECK(r.data.contains("media") == false);

    // Past the nest's end the clip is not there; on the A1 track alone the same nest answers.
    CHECK(h.errorOf("sequence.matchFrame", in(h, Json{{"time", tc(2)}})) == "NotFound");
    const auto audio = h.ok("sequence.matchFrame", in(h, Json{{"time", tc(4)}, {"tracks", Json::array({"A1"})}}));
    CHECK(audio.data["track"] == "A1");
    CHECK(audio.data["sequence"] == nestedId);
    CHECK(audio.data["sequenceTime"]["timecode"] == tc(1));
}
