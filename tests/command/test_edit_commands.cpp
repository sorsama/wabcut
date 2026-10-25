// The built-in edit commands, end to end through the engine. They are thin adapters, so these tests check
// the mapping (references, defaults, params) on top of the model's own edit tests.

#include "CommandHarness.h"

#include <wabcut/model/Parameters.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

const Clip& clipOf(const CommandHarness& h, std::string_view ref) {
    static ProjectSnapshot keep; // keeps the snapshot alive for the returned reference
    keep = h.state();
    return h.sequence(keep).findClip(clipAt(*keep, h.tl.seq, ref))->clip();
}

} // namespace

TEST_CASE("clip.insert and clip.overwrite edit media in at a time", "[command][edit][source]") {
    SECTION("insert pushes later material and links the new clips") {
        CommandHarness h(R"(
            V1 tgt |Aaaa....|
            A1 tgt |Aaaa....|
        )");
        const auto r = h.ok("sequence.insert", Json{{"media", "B"}, {"at", tc(2)}, {"sourceOut", tc(3)}});
        // B's two untitled audio streams read as a game recording's Game and Mic (A.10): Game goes to A1, Mic to a
        // new Mic track.
        CHECK_SESSION(h, R"(
            V1 |AaBbbA2.|
            A1 |AaBbbA2.|
            A2 |..Bbb...|
        )");
        CHECK(r.data["placed"].size() == 3);
        CHECK(r.data["at"]["timecode"] == tc(2));
        CHECK(r.data["playhead"]["timecode"] == tc(5));
        CHECK(r.data["createdTracks"] == Json::array({"A2"}));
        CHECK(r.createdIds.size() == 5); // three placed clips and two split pieces
        CHECK(links(*h.state(), h.tl.seq) == "V1@2+A1@2+A2@2");
        CHECK(h.sequence(h.state()).audioTracks.at(1)->name == "Mic");
        CHECK(h.session.undoLabel() == "Insert");
    }
    SECTION("overwrite replaces material; patches can be chosen") {
        CommandHarness h(R"(
            V1 tgt |Aaaaaaaa|
            A1 tgt |Aaaaaaaa|
        )");
        h.ok("clip.overwrite", Json{{"media", h.tl.media('B').value},
                                    {"at", f(2).value},
                                    {"sourceIn", tc(10)},
                                    {"sourceOut", tc(13)},
                                    {"audio", Json::array()}});
        CHECK_SESSION(h, R"(
            V1 |AaB10A5a|
            A1 |Aaaaaaaa|
        )");
        h.ok("clip.overwrite", Json{{"media", "C"}, {"at", tc(0)}, {"sourceOut", tc(1)}, {"video", nullptr},
                                    {"audio", Json::array({"A1"})}});
        CHECK_SESSION(h, R"(
            V1 |AaB10A5a|
            A1 |CA1aaaaa|
        )");
    }
    SECTION("problems are reported, not guessed") {
        CommandHarness h(R"(
            V1 tgt |Aaaa|
            A1 tgt |Aaaa|
        )");
        CHECK(h.errorOf("clip.insert", Json{{"media", "B"}}) == "InvalidParams"); // no time, no marks
        CHECK(h.errorOf("clip.insert", Json{{"media", "Z9"}, {"at", 0}}) == "NotFound");
        CHECK(h.errorOf("clip.insert", Json{{"media", "B"}, {"at", 0}, {"audio", Json::array({"A1", "A1", "A1"})}}) ==
              "InvalidParams"); // B has two audio streams
        CHECK(h.errorOf("clip.insert", Json{{"media", "B"}, {"at", 0}, {"video", nullptr}, {"audio", Json::array()}}) ==
              "InvalidParams");
    }
}

TEST_CASE("Lift and extract act on a range of the chosen tracks", "[command][edit][lift]") {
    SECTION("lift leaves a gap on the targeted tracks") {
        CommandHarness h(R"(
            V1 tgt |Aaaaaaaa|
            A1     |Aaaaaaaa|
        )");
        h.ok("sequence.lift", Json{{"in", tc(2)}, {"out", tc(5)}});
        CHECK_SESSION(h, R"(
            V1 |Aa...A5a|
            A1 |Aaaaaaaa|
        )");
    }
    SECTION("tracks override the targeting for this command only") {
        CommandHarness h(R"(
            V1 tgt |Aaaaaaaa|
            A1     |Aaaaaaaa|
        )");
        h.ok("sequence.lift", Json{{"in", tc(2)}, {"out", tc(5)}, {"tracks", Json::array({"A1"})}});
        CHECK_SESSION(h, R"(
            V1 |Aaaaaaaa|
            A1 |Aa...A5a|
        )");
        const auto s = h.state();
        CHECK(h.sequence(s).videoTracks[0]->targeted);
        CHECK_FALSE(h.sequence(s).audioTracks[0]->targeted);
    }
    SECTION("extract closes the range") {
        CommandHarness h(R"(
            V1 tgt |Aaaaaaaa|
            A1 tgt |Aaaaaaaa|
        )");
        h.ok("sequence.extract", Json{{"in", tc(2)}, {"out", f(5).value}});
        CHECK_SESSION(h, R"(
            V1 |AaA5a...|
            A1 |AaA5a...|
        )");
    }
    SECTION("the sequence marks are used when in/out are left out") {
        CommandHarness h(R"(
            V1 tgt |Aaaaaaaa|
        )");
        h.ok("marker.markIn", Json{{"time", tc(1)}});
        h.ok("marker.markOut", Json{{"time", tc(2)}});
        h.ok("sequence.extract");
        CHECK_SESSION(h, "V1 |AA3aaa..|");
    }
}

TEST_CASE("Add edit cuts the targeted or the given tracks", "[command][edit][razor]") {
    CommandHarness h(Timeline(R"(
        V1 tgt |Aaaaaaaa|
        A1     |Aaaaaaaa|
    )")
                         .linkMatching());
    SECTION("the targeted tracks by default") {
        h.ok("timeline.razor", Json{{"time", tc(3)}});
        CHECK_SESSION(h, R"(
            V1 |AaaA3aaa|
            A1 |Aaaaaaaa|
        )");
    }
    SECTION("linked partners when asked") {
        h.ok("sequence.addEdit", Json{{"time", tc(3)}, {"linked", true}});
        CHECK_SESSION(h, R"(
            V1 |AaaA3aaa|
            A1 |AaaA3aaa|
        )");
    }
    SECTION("the given tracks") {
        h.ok("sequence.addEdit", Json{{"time", tc(3)}, {"tracks", Json::array({"A1"})}});
        CHECK_SESSION(h, R"(
            V1 |Aaaaaaaa|
            A1 |AaaA3aaa|
        )");
    }
    SECTION("every track") {
        h.ok("sequence.addEditToAllTracks", Json{{"time", tc(3)}});
        CHECK_SESSION(h, R"(
            V1 |AaaA3aaa|
            A1 |AaaA3aaa|
        )");
    }
    SECTION("nothing targeted and no tracks given") {
        CommandHarness none(R"(
            V1 |Aaaa|
        )");
        CHECK(none.errorOf("sequence.addEdit", Json{{"time", tc(2)}}) == "EditRejected");
    }
}

TEST_CASE("Delete and ripple delete take clips or a gap", "[command][edit][delete]") {
    CommandHarness h(Timeline(R"(
        V1 tgt |Aa..Bbbb|
        A1 tgt |Aa..Bbbb|
    )")
                         .linkMatching());
    SECTION("delete leaves gaps and follows links by default") {
        h.ok("edit.clear", Json{{"clips", Json::array({at("V1", 0)})}});
        CHECK_SESSION(h, R"(
            V1 |....Bbbb|
            A1 |....Bbbb|
        )");
    }
    SECTION("linked: false deletes only the named clips") {
        h.ok("clip.delete", Json{{"clips", Json::array({at("V1", 0)})}, {"linked", false}});
        CHECK_SESSION(h, R"(
            V1 |....Bbbb|
            A1 |Aa..Bbbb|
        )");
    }
    SECTION("ripple delete closes the clips' time") {
        h.ok("timeline.rippleDelete", Json{{"clips", Json::array({h.clip("V1@4").value})}});
        CHECK_SESSION(h, R"(
            V1 |Aa......|
            A1 |Aa......|
        )");
    }
    SECTION("ripple delete closes a gap") {
        h.ok("timeline.rippleDelete", Json{{"gap", at("V1", 3)}});
        CHECK_SESSION(h, R"(
            V1 |AaBbbb..|
            A1 |AaBbbb..|
        )");
    }
    SECTION("clips or a gap, not both") {
        CHECK(h.errorOf("timeline.rippleDelete", Json::object()) == "InvalidParams");
        CHECK(h.errorOf("timeline.rippleDelete", Json{{"clips", Json::array({at("V1", 0)})}, {"gap", at("V1", 3)}}) ==
              "InvalidParams");
    }
}

TEST_CASE("Ripple trim to the playhead (Q and W)", "[command][edit][trim]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaa|
    )");
    SECTION("previous edit to playhead") {
        const auto r = h.ok("sequence.rippleTrimPreviousToPlayhead", Json{{"time", tc(3)}});
        CHECK_SESSION(h, "V1 |A3aaa...|");
        CHECK(r.data["playhead"]["ticks"] == 0);
    }
    SECTION("next edit to playhead") {
        h.ok("sequence.rippleTrimNextToPlayhead", Json{{"time", tc(3)}});
        CHECK_SESSION(h, "V1 |Aaa.....|");
    }
}

TEST_CASE("Trim, ripple, roll, slip, slide and move by a delta", "[command][edit][trim]") {
    SECTION("trim reports the delta it applied") {
        CommandHarness h(R"(
            V1 tgt |AaaaBbbb|
        )");
        const auto r = h.ok("clip.trim", Json{{"clip", at("V1", 0)}, {"edge", "tail"}, {"delta", "-00:00:00:02"}});
        CHECK_SESSION(h, "V1 |Aa..Bbbb|");
        CHECK(r.data["appliedDelta"]["ticks"] == -f(2).value);
        // Clamped by the neighbour.
        const auto clamped = h.ok("clip.trim", Json{{"clip", at("V1", 0)}, {"edge", "tail"}, {"delta", tc(5)}});
        CHECK(clamped.data["appliedDelta"]["ticks"] == f(2).value);
        CHECK_SESSION(h, "V1 |AaaaBbbb|");
    }
    SECTION("ripple trim moves later material") {
        CommandHarness h(R"(
            V1 tgt |AaaaBbbb|
        )");
        h.ok("clip.rippleTrim", Json{{"clip", at("V1", 4)}, {"edge", "head"}, {"delta", tc(2)}});
        CHECK_SESSION(h, "V1 |AaaaB2..|");
    }
    SECTION("roll moves the edit point") {
        CommandHarness h(R"(
            V1 tgt |AaaaBbbb|
        )");
        h.ok("clip.roll", Json{{"clip", at("V1", 0)}, {"edge", "tail"}, {"delta", f(1).value}});
        CHECK_SESSION(h, "V1 |AaaaaB1b|");
    }
    SECTION("slip shifts the source") {
        CommandHarness h(R"(
            V1 tgt |AaaaBbbb|
        )");
        h.ok("clip.slip", Json{{"clips", Json::array({at("V1", 4)})}, {"delta", tc(5)}});
        CHECK_SESSION(h, "V1 |AaaaB5bb|");
    }
    SECTION("slide moves a clip between its neighbours") {
        CommandHarness h(R"(
            V1 tgt |Aaaa..Bbbb|
        )");
        h.ok("clip.slide", Json{{"clip", at("V1", 6)}, {"delta", "-1"}});
        CHECK_SESSION(h, "V1 |Aaaa.Bbbb.|");
    }
    SECTION("move across tracks") {
        CommandHarness h(R"(
            V2     |........|
            V1 tgt |AaaaBbbb|
        )");
        h.ok("clip.move", Json{{"clips", Json::array({at("V1", 4)})}, {"videoTracks", 1}});
        CHECK_SESSION(h, R"(
            V2 |....Bbbb|
            V1 |Aaaa....|
        )");
        h.ok("clip.move", Json{{"clips", Json::array({at("V2", 4)})}, {"videoTracks", -1}, {"delta", "-4"},
                               {"mode", "insert"}});
        CHECK_SESSION(h, R"(
            V2 |........|
            V1 |BbbbAaaa|
        )");
    }
}

TEST_CASE("Clip attributes: parameters, enable, name, colour, links", "[command][edit][attributes]") {
    CommandHarness h(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    SECTION("set a parameter, checked against its range") {
        h.ok("clip.setParam", Json{{"clips", Json::array({at("A1", 0), at("A1", 4)})}, {"param", "volume.level"},
                                   {"value", -6}});
        CHECK(clipOf(h, "A1@0").properties().value(ParamId::VolumeDb) == -6.0);
        CHECK(clipOf(h, "A1@4").properties().value(ParamId::VolumeDb) == -6.0);
        CHECK(clipOf(h, "V1@0").properties().value(ParamId::VolumeDb) == 0.0);
        CHECK(h.session.undoCount() == 1);
        h.ok("clip.setParam", Json{{"clips", Json::array({at("V1", 0)})}, {"param", "opacity.opacity"}, {"value", 0.25}});
        CHECK(clipOf(h, "V1@0").properties().value(ParamId::Opacity) == 0.25);
        CHECK(h.errorOf("clip.setParam", Json{{"clips", Json::array({at("A1", 0)})}, {"param", "volume.level"},
                                              {"value", 30}}) == "InvalidParams");
        CHECK(h.errorOf("clip.setParam", Json{{"clips", Json::array({at("A1", 0)})}, {"param", "volume.gain"},
                                              {"value", 1}}) == "InvalidParams");
    }
    SECTION("enable, rename, colour") {
        h.ok("clip.enable", Json{{"clips", Json::array({at("V1", 0)})}, {"enabled", false}});
        h.ok("clip.rename", Json{{"clips", Json::array({at("V1", 0)})}, {"name", "Opening"}});
        h.ok("clip.setColor", Json{{"clips", Json::array({at("V1", 0)})}, {"colorIndex", 5}});
        const Clip& c = clipOf(h, "V1@0");
        CHECK_FALSE(c.enabled);
        CHECK(c.name() == "Opening");
        CHECK(c.colorIndex == 5);
        CHECK(h.session.undoCount() == 3);
    }
    SECTION("link and unlink") {
        h.ok("clip.link", Json{{"clips", Json::array({at("V1", 0), at("A1", 0)})}});
        CHECK(links(*h.state(), h.tl.seq) == "V1@0+A1@0");
        CHECK(h.errorOf("clip.link", Json{{"clips", Json::array({at("V1", 4)})}}) == "InvalidParams"); // minItems 2
        h.ok("clip.unlink", Json{{"clips", Json::array({at("V1", 0)})}});
        CHECK(links(*h.state(), h.tl.seq).empty());
    }
}

TEST_CASE("Markers and In/Out marks", "[command][edit][marks]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaa|
    )");
    h.ok("markers.add", Json{{"time", tc(3)}, {"name", "Chapter 2"}, {"colorIndex", 2}});
    h.ok("marker.add", Json{{"time", tc(1)}, {"duration", tc(2)}});
    auto s = h.state();
    REQUIRE(h.sequence(s).markers.size() == 2);
    CHECK(h.sequence(s).markers[0].start == f(1));
    CHECK(h.sequence(s).markers[0].duration == f(2));
    CHECK(h.sequence(s).markers[1].name == "Chapter 2");
    h.ok("marker.remove", Json{{"index", 0}});
    s = h.state();
    REQUIRE(h.sequence(s).markers.size() == 1);
    CHECK(h.sequence(s).markers[0].name == "Chapter 2");

    h.ok("marker.markIn", Json{{"time", tc(2)}});
    h.ok("marker.markOut", Json{{"time", tc(5)}});
    s = h.state();
    CHECK(h.sequence(s).markIn == f(2));
    CHECK(h.sequence(s).markOut == f(6)); // the end of the marked frame
    h.ok("marker.clearInAndOut");
    s = h.state();
    CHECK_FALSE(h.sequence(s).markIn);
    CHECK_FALSE(h.sequence(s).markOut);
}

TEST_CASE("Remove silences ripple-deletes whole frames of every range in one step", "[command][edit][silences]") {
    CommandHarness h(R"(
        V1     |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
        A2     |Mmmmmmmmmm|
    )");
    h.ok("marker.markIn", Json{{"time", tc(1)}});
    const auto r = h.ok("timeline.removeSilences",
                        Json{{"ranges", Json::array({Json{{"start", f(6).value}, {"end", tc(8)}},
                                                     // rounded inward to frame 3 only
                                                     Json{{"start", f(2).value + 1}, {"end", f(4).value + 5}},
                                                     // overlaps the first range: merged
                                                     Json{{"start", tc(7)}, {"end", tc(8)}}})}});
    CHECK_SESSION(h, R"(
        V1 |AaaA4A8...|
        A1 |AaaA4A8...|
        A2 |MmmM4M8...|
    )");
    CHECK(r.data["rangesRemoved"] == 2);
    CHECK(r.data["removed"]["ticks"] == f(3).value);
    CHECK(h.session.undoCount() == 2);
    // Marks and targeting are as they were.
    const auto s = h.state();
    CHECK(h.sequence(s).markIn == f(1));
    CHECK_FALSE(h.sequence(s).markOut);
    CHECK_FALSE(h.sequence(s).videoTracks[0]->targeted);
    CHECK(h.sequence(s).audioTracks[0]->targeted);
    CHECK_FALSE(h.sequence(s).audioTracks[1]->targeted);

    SECTION("only some tracks") {
        CommandHarness some(R"(
            V1 nosync |Aaaaaaaa|
            A1        |Aaaaaaaa|
        )");
        some.ok("timeline.removeSilences", Json{{"ranges", Json::array({Json{{"start", tc(1)}, {"end", tc(3)}}})},
                                                {"tracks", Json::array({"A1"})}});
        CHECK_SESSION(some, R"(
            V1 |Aaaaaaaa|
            A1 |AA3aaa..|
        )");
    }
    SECTION("ranges without a whole frame change nothing") {
        const auto none = h.ok("timeline.removeSilences",
                               Json{{"ranges", Json::array({Json{{"start", f(2).value + 1}, {"end", f(3).value - 1}}})}});
        CHECK_FALSE(none.committed);
        CHECK(none.warnings.size() == 1);
    }
    SECTION("an empty or reversed range is refused") {
        CHECK(h.errorOf("timeline.removeSilences", Json{{"ranges", Json::array()}}) == "InvalidParams");
        CHECK(h.errorOf("timeline.removeSilences",
                        Json{{"ranges", Json::array({Json{{"start", tc(4)}, {"end", tc(2)}}})}}) == "InvalidParams");
    }
}

TEST_CASE("Tracks: header state, add and delete", "[command][edit][tracks]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
    )");
    h.ok("track.setState", Json{{"track", "A1"}, {"muted", true}, {"name", "Dialogue"}, {"targeted", false}});
    auto s = h.state();
    const Track& a1 = *h.sequence(s).audioTracks[0];
    CHECK(a1.muted);
    CHECK(a1.name == "Dialogue");
    CHECK_FALSE(a1.targeted);
    CHECK(a1.syncLocked); // untouched

    const auto added = h.ok("sequence.addTrack", Json{{"kind", "video"}});
    CHECK(added.data["track"] == "V2");
    REQUIRE(added.createdIds.size() == 1);
    s = h.state();
    REQUIRE(h.sequence(s).videoTracks.size() == 2);
    CHECK(h.sequence(s).videoTracks[1]->id.value == added.createdIds[0]);

    h.ok("sequence.removeTrack", Json{{"track", "V2"}});
    s = h.state();
    CHECK(h.sequence(s).videoTracks.size() == 1);
    h.ok("track.setState", Json{{"track", "V1"}, {"locked", true}});
    CHECK(h.errorOf("sequence.removeTrack", Json{{"track", "V1"}}) == "EditRejected");
}

TEST_CASE("Integer params above int64 are refused, never wrapped into an index", "[command][edit]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    h.ok("marker.add", Json{{"time", tc(2)}});
    const ProjectSnapshot before = h.state();
    const auto r = h.run(Json::parse(R"({"command":"marker.remove","params":{"index":18446744073709551615}})"));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error->code == command::ErrorCode::InvalidParams);
    CHECK(h.errorOf("marker.remove", Json{{"index", 9223372036854775807LL}}) == "NotFound"); // fits: just no such marker
    CHECK(*h.state() == *before);
}

TEST_CASE("clip.move copies with copy true and rearranges with mode rearrange", "[command][edit][move]") {
    SECTION("copy: the originals stay and createdIds lists the copies") {
        Timeline tl(R"(
            V1 tgt |AaaaBbbb....|
            A1 tgt |AaaaBbbb....|
        )");
        tl.linkMatching();
        CommandHarness h(std::move(tl));
        const ProjectSnapshot before = h.state();
        const ClipId b = h.clip("V1@4");
        const auto r = h.ok("clip.move", Json{{"clips", Json::array({at("V1", 4)})}, {"delta", tc(4)}, {"copy", true}});
        CHECK_SESSION(h, R"(
            V1 |AaaaBbbbBbbb|
            A1 |AaaaBbbbBbbb|
        )");
        CHECK(h.clip("V1@4") == b);
        REQUIRE(r.createdIds.size() == 2);
        CHECK(r.createdIds[0] == h.clip("V1@8").value);
        CHECK(r.createdIds[1] == h.clip("A1@8").value);
        CHECK(r.data["appliedDelta"]["timecode"] == tc(4));
        CHECK(links(*h.state(), h.tl.seq) == "V1@0+A1@0 V1@4+A1@4 V1@8+A1@8");
        CHECK(h.session.undoCount() == 1);
        REQUIRE(h.session.undo().has_value());
        CHECK(*h.state() == *before);

        // Insert mode makes room; linked false copies one side.
        h.ok("clip.move", Json{{"clips", Json::array({at("V1", 0)})}, {"delta", tc(4)}, {"copy", true},
                               {"mode", "insert"}, {"linked", false}});
        CHECK_SESSION(h, R"(
            V1 |AaaaAaaaBbbb|
            A1 |Aaaa....Bbbb|
        )");
    }
    SECTION("rearrange: the old place closes and the drop opens") {
        CommandHarness h(R"(
            V1 tgt |AaaaBbbbCccc|
        )");
        const ProjectSnapshot before = h.state();
        h.ok("clip.move", Json{{"clips", Json::array({at("V1", 0)})}, {"delta", tc(12)}, {"mode", "rearrange"}});
        CHECK_SESSION(h, "V1 |BbbbCcccAaaa|");
        CHECK(h.session.undoCount() == 1);
        REQUIRE(h.session.undo().has_value());
        CHECK(*h.state() == *before);
        h.ok("clip.move", Json{{"clips", Json::array({at("V1", 0)})}, {"delta", tc(8)}, {"mode", "rearrange"}});
        CHECK_SESSION(h, "V1 |BbbbAaaaCccc|"); // dropped at the cut after B: the two swap
    }
    SECTION("a rearranged copy is refused") {
        CommandHarness h(R"(
            V1 tgt |AaaaBbbb....|
        )");
        const ProjectSnapshot before = h.state();
        CHECK(h.errorOf("clip.move", Json{{"clips", Json::array({at("V1", 0)})}, {"delta", tc(8)}, {"copy", true},
                                          {"mode", "rearrange"}}) == "EditRejected");
        CHECK(h.errorOf("clip.move", Json{{"clips", Json::array({at("V1", 0)})}, {"mode", "sideways"}}) == "InvalidParams");
        CHECK(*h.state() == *before);
    }
}
