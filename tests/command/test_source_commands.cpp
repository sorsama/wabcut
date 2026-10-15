// Source Monitor commands (CatalogSource.cpp): Premiere's three-point cases for insert and overwrite, every Fit
// Clip choice of a four-point edit, source marks stored on the media, patching, targeting and match frame.

#include "CommandHarness.h"

#include <wabcut/command/SourceServices.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

// 12 frames of A on V1/A1; S is a 20-frame source, T a 5-frame one. Their two untitled audio streams read as a game
// recording's Game and Mic (A.10): Game goes to A1 as Premiere's default patching puts it, and Mic, which has no track
// in this sequence, to a new A2 named Mic.
constexpr const char* kBase = R"(
    V1 tgt |Aaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaa|
)";
const std::map<char, std::int64_t> kMedia{{'S', 20}, {'T', 5}};

struct ThreePointCase {
    const char* name;
    const char* media;
    std::optional<int> sourceIn;  ///< source frame
    std::optional<int> sourceOut; ///< exclusive source frame
    std::optional<int> sequenceIn;
    std::optional<int> sequenceOut; ///< last frame included (Premiere's O)
    int playhead;
    const char* insert;
    const char* overwrite;
};

/// Marks the source (stored on the media) and the sequence, then runs the source edit at the playhead.
command::CommandResult sourceEdit(CommandHarness& h, const ThreePointCase& c, bool insert, Json extra = Json::object()) {
    if (c.sourceIn || c.sourceOut) {
        Json marks{{"media", c.media}};
        if (c.sourceIn) {
            marks["in"] = tc(*c.sourceIn);
        }
        if (c.sourceOut) {
            marks["out"] = tc(*c.sourceOut);
        }
        h.ok("source.setMarks", marks);
    }
    if (c.sequenceIn) {
        h.ok("marker.markIn", Json{{"time", tc(*c.sequenceIn)}});
    }
    if (c.sequenceOut) {
        h.ok("marker.markOut", Json{{"time", tc(*c.sequenceOut)}});
    }
    Json params{{"media", c.media}, {"at", tc(c.playhead)}};
    params.update(extra);
    return h.ok(insert ? "edit.insertFromSource" : "edit.overwriteFromSource", params);
}

const Clip& clipOf(const CommandHarness& h, std::string_view ref) {
    static ProjectSnapshot keep; // keeps the snapshot alive for the returned reference
    keep = h.state();
    return h.sequence(keep).findClip(clipAt(*keep, h.tl.seq, ref))->clip();
}

/// The app's Source Monitor patching, as SourceMonitorServices.
struct HostPatching : command::SourceMonitorServices {
    std::vector<command::SourcePatchSetting> patches;
    std::vector<command::SourcePatchSetting> sourcePatching(SequenceId, MediaId) const override { return patches; }
};

/// A.10: a game recording added to `tl`: a 20-frame video stream and one AAC stream per title (it renders with the
/// first letter of `name`).
MediaId addRecording(Timeline& tl, const std::string& name, std::initializer_list<const char*> titles) {
    MediaItem item;
    item.name = name;
    item.locator.path = "D:/recordings/" + name + ".mkv";
    item.info.path = item.locator.path;
    item.info.container = "matroska,webm";
    item.info.duration = f(20);
    VideoStreamInfo v;
    v.index = 0;
    v.codec = "h264";
    v.width = 1920;
    v.height = 1080;
    v.frameRate = kRate;
    v.duration = f(20);
    v.frameCount = 20;
    item.info.video.push_back(v);
    int index = 1;
    for (const char* title : titles) {
        AudioStreamInfo a;
        a.index = index++;
        a.codec = "aac";
        a.channels = 2;
        a.channelLayout = "stereo";
        a.duration = f(20);
        a.title = title;
        item.info.audio.push_back(a);
    }
    auto added = edit::addMedia(tl.state, std::move(item));
    REQUIRE(added);
    tl.state = std::move(added->state);
    return added->id;
}

} // namespace

// Premiere's three-point editing: any three of source In, source Out, sequence In and sequence Out (the playhead
// standing in for a missing sequence In). The table is in the M5h report.
TEST_CASE("Source edits follow Premiere's three-point rules", "[command][source][threepoint]") {
    // A2 is the Mic track the edit adds (see kBase).
    const std::vector<ThreePointCase> cases{
        {"source In/Out at the playhead", "S", 4, 7, {}, {}, 2, //
         "V1 |AaS4sA2aaaaaaaa|\nA1 |AaS4sA2aaaaaaaa|\nA2 |..S4s|", "V1 |AaS4sA5aaaaa|\nA1 |AaS4sA5aaaaa|\nA2 |..S4s|"},
        {"source In/Out at the sequence In (the playhead is ignored)", "S", 4, 7, 5, {}, 2,
         "V1 |AaaaaS4sA5aaaaa|\nA1 |AaaaaS4sA5aaaaa|\nA2 |.....S4s|",
         "V1 |AaaaaS4sA8aa|\nA1 |AaaaaS4sA8aa|\nA2 |.....S4s|"},
        {"source In/Out back-timed from the sequence Out", "S", 4, 7, {}, 8, 2,
         "V1 |AaaaaaS4sA6aaaa|\nA1 |AaaaaaS4sA6aaaa|\nA2 |......S4s|",
         "V1 |AaaaaaS4sA9a|\nA1 |AaaaaaS4sA9a|\nA2 |......S4s|"},
        {"source In with sequence In/Out: the sequence sets the length", "S", 4, {}, 3, 6, 0,
         "V1 |AaaS4ssA3aaaaaaa|\nA1 |AaaS4ssA3aaaaaaa|\nA2 |...S4ss|",
         "V1 |AaaS4ssA7aaa|\nA1 |AaaS4ssA7aaa|\nA2 |...S4ss|"},
        {"source Out with sequence In/Out: back-timed in the source", "S", {}, 10, 3, 6, 0,
         "V1 |AaaS6ssA3aaaaaaa|\nA1 |AaaS6ssA3aaaaaaa|\nA2 |...S6ss|",
         "V1 |AaaS6ssA7aaa|\nA1 |AaaS6ssA7aaa|\nA2 |...S6ss|"},
        {"no source marks with sequence In/Out: from the source start", "S", {}, {}, 3, 6, 0,
         "V1 |AaaSsssA3aaaaaaa|\nA1 |AaaSsssA3aaaaaaa|\nA2 |...Ssss|",
         "V1 |AaaSsssA7aaa|\nA1 |AaaSsssA7aaa|\nA2 |...Ssss|"},
        {"no marks: the whole source at the playhead", "T", {}, {}, {}, {}, 2,
         "V1 |AaTttttA2aaaaaaaa|\nA1 |AaTttttA2aaaaaaaa|\nA2 |..Ttttt|",
         "V1 |AaTttttA7aaa|\nA1 |AaTttttA7aaa|\nA2 |..Ttttt|"},
        {"source In only: to the end of the source", "T", 2, {}, {}, {}, 2,
         "V1 |AaT2tA2aaaaaaaa|\nA1 |AaT2tA2aaaaaaaa|\nA2 |..T2t|", "V1 |AaT2tA5aaaaa|\nA1 |AaT2tA5aaaaa|\nA2 |..T2t|"},
        {"source Out only: from the start of the source", "T", {}, 3, {}, {}, 2,
         "V1 |AaTttA2aaaaaaaa|\nA1 |AaTttA2aaaaaaaa|\nA2 |..Ttt|", "V1 |AaTttA5aaaaa|\nA1 |AaTttA5aaaaa|\nA2 |..Ttt|"},
        {"four marks of the same length: a plain edit", "S", 4, 8, 3, 6, 0,
         "V1 |AaaS4ssA3aaaaaaa|\nA1 |AaaS4ssA3aaaaaaa|\nA2 |...S4ss|",
         "V1 |AaaS4ssA7aaa|\nA1 |AaaS4ssA7aaa|\nA2 |...S4ss|"},
    };
    for (const ThreePointCase& c : cases) {
        for (const bool insert : {true, false}) {
            INFO(c.name << (insert ? " (insert)" : " (overwrite)"));
            CommandHarness h(Timeline(kBase, kMedia));
            const auto r = sourceEdit(h, c, insert);
            CHECK_SESSION(h, insert ? c.insert : c.overwrite);
            CHECK(r.data["fourPoint"] == false);
            CHECK(r.data["fit"].is_null());
            CHECK(h.session.undoLabel() == (insert ? "Insert from Source" : "Overwrite from Source"));
            CHECK_FALSE(links(*h.state(), h.tl.seq).empty()); // the new V/A pair is linked
        }
    }
}

TEST_CASE("A four-point edit whose ranges differ takes each Fit Clip choice", "[command][source][fourpoint]") {
    // Source 4..10 (6 frames) into the sequence 3..6 (4 frames).
    const ThreePointCase c{"four points", "S", 4, 10, 3, 6, 0, "", ""};
    struct Choice {
        const char* fit;
        const char* insert;
        const char* overwrite;
    };
    const std::vector<Choice> choices{
        {"ignoreSourceOut", "V1 |AaaS4ssA3aaaaaaa|\nA1 |AaaS4ssA3aaaaaaa|\nA2 |...S4ss|",
         "V1 |AaaS4ssA7aaa|\nA1 |AaaS4ssA7aaa|\nA2 |...S4ss|"},
        {"ignoreSourceIn", "V1 |AaaS6ssA3aaaaaaa|\nA1 |AaaS6ssA3aaaaaaa|\nA2 |...S6ss|",
         "V1 |AaaS6ssA7aaa|\nA1 |AaaS6ssA7aaa|\nA2 |...S6ss|"},
        {"ignoreSequenceIn", "V1 |AS4ssssA1aaaaaaaaa|\nA1 |AS4ssssA1aaaaaaaaa|\nA2 |.S4ssss|",
         "V1 |AS4ssssA7aaa|\nA1 |AS4ssssA7aaa|\nA2 |.S4ssss|"},
        {"ignoreSequenceOut", "V1 |AaaS4ssssA3aaaaaaa|\nA1 |AaaS4ssssA3aaaaaaa|\nA2 |...S4ssss|",
         "V1 |AaaS4ssssA9a|\nA1 |AaaS4ssssA9a|\nA2 |...S4ssss|"},
    };
    for (const Choice& choice : choices) {
        for (const bool insert : {true, false}) {
            INFO(choice.fit << (insert ? " (insert)" : " (overwrite)"));
            CommandHarness h(Timeline(kBase, kMedia));
            const auto r = sourceEdit(h, c, insert, Json{{"fit", choice.fit}});
            CHECK_SESSION(h, insert ? choice.insert : choice.overwrite);
            CHECK(r.data["fourPoint"] == true);
            CHECK(r.data["fit"] == choice.fit);
            CHECK(r.warnings.empty());
            // The sequence marks stay for the next edit.
            CHECK(h.sequence(h.state()).markIn == f(3));
            CHECK(h.sequence(h.state()).markOut == f(7));
        }
    }

    SECTION("without a choice the source Out is ignored, with a warning") {
        CommandHarness h(Timeline(kBase, kMedia));
        const auto r = sourceEdit(h, c, true);
        CHECK_SESSION(h, "V1 |AaaS4ssA3aaaaaaa|\nA1 |AaaS4ssA3aaaaaaa|\nA2 |...S4ss|");
        CHECK(r.data["fit"] == "ignoreSourceOut");
        CHECK(r.warnings.size() == 1);
    }

    SECTION("change clip speed (fit to fill), insert: one undo step, later material follows") {
        CommandHarness h(Timeline(kBase, kMedia));
        const auto r = sourceEdit(h, c, true, Json{{"fit", "speed"}});
        CHECK(h.session.undoCount() == 4); // source marks, sequence In, sequence Out, then the edit as one step
        CHECK(h.session.undoLabel() == "Insert from Source");
        CHECK(r.data["speed"]["num"] == 3);
        CHECK(r.data["speed"]["den"] == 2);
        CHECK(r.data["speed"]["percent"] == 150.0);
        CHECK(r.data["playhead"]["timecode"] == tc(7));
        for (const char* track : {"V1", "A1"}) {
            const Clip& fitted = clipOf(h, std::string(track) + "@3");
            CHECK(fitted.duration() == f(4));
            CHECK(fitted.sourceIn == f(4));
            CHECK(fitted.sourceOut() == f(10)); // the whole marked source plays in the marked range
            CHECK(fitted.speed == Speed{3, 2});
            CHECK(clipOf(h, std::string(track) + "@7").sourceIn == f(3)); // the cut-off rest of A follows
        }
        CHECK(h.sequence(h.state()).contentEnd() == f(16));
        h.session.undo(); // the insert and the retime go together
        CHECK_SESSION(h, "V1 |Aaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaa|");
    }

    SECTION("change clip speed (fit to fill), overwrite: exactly [In, Out) is replaced") {
        CommandHarness h(Timeline(kBase, kMedia));
        const auto r = sourceEdit(h, c, false, Json{{"fit", "speed"}});
        CHECK(r.data["fit"] == "speed");
        for (const char* track : {"V1", "A1"}) {
            const Clip& fitted = clipOf(h, std::string(track) + "@3");
            CHECK(fitted.duration() == f(4));
            CHECK(fitted.speed == Speed{3, 2});
            CHECK(clipOf(h, std::string(track) + "@7").sourceIn == f(7));
            CHECK(clipOf(h, std::string(track) + "@0").duration() == f(3));
        }
        CHECK(links(*h.state(), h.tl.seq).find("V1@3+A1@3") != std::string::npos);
        // Parking the clip past the end while retiming it left no trace in the sequence's length.
        CHECK(h.sequence(h.state()).contentEnd() == f(12));
        CHECK(h.sequence(h.state()).duration == h.tl.sequence().duration);
    }

    SECTION("slower: a short source fills a longer range") {
        CommandHarness h(Timeline(kBase, kMedia));
        sourceEdit(h, ThreePointCase{"slow", "S", 4, 6, 3, 6, 0, "", ""}, false, Json{{"fit", "speed"}});
        const Clip& fitted = clipOf(h, "V1@3");
        CHECK(fitted.duration() == f(4));
        CHECK(fitted.speed == Speed{1, 2});
        CHECK(clipOf(h, "V1@7").sourceIn == f(7));
    }
}

TEST_CASE("Source marks are stored on the media and used by source edits", "[command][source][marks]") {
    CommandHarness h(Timeline(kBase, kMedia));
    const MediaId s = h.tl.media('S');

    auto r = h.ok("source.setMarks", Json{{"media", "S"}, {"in", tc(4)}, {"out", tc(9)}});
    CHECK(r.data["marks"]["in"]["timecode"] == tc(4));
    CHECK(h.state()->findMedia(s)->markIn == f(4));
    CHECK(h.state()->findMedia(s)->markOut == f(9));
    CHECK(h.session.undoLabel() == "Set Source Marks");

    // A key left out keeps its mark; an In at or after the Out drops the Out (Premiere).
    h.ok("source.setMarks", Json{{"media", "S"}, {"in", tc(12)}});
    CHECK(h.state()->findMedia(s)->markIn == f(12));
    CHECK_FALSE(h.state()->findMedia(s)->markOut);
    h.ok("source.setMarks", Json{{"media", "S"}, {"out", f(3).value}});
    CHECK_FALSE(h.state()->findMedia(s)->markIn);
    CHECK(h.state()->findMedia(s)->markOut == f(3));
    h.ok("source.setMarks", Json{{"media", "S"}, {"in", tc(1)}, {"out", nullptr}});
    CHECK(h.state()->findMedia(s)->markIn == f(1));
    CHECK_FALSE(h.state()->findMedia(s)->markOut);
    CHECK(h.errorOf("source.setMarks", Json{{"media", "S"}, {"out", tc(21)}}) == "EditRejected");

    h.ok("source.setMarks", Json{{"media", "S"}, {"in", tc(2)}, {"out", tc(5)}});
    h.ok("source.clearMarks", Json{{"media", "S"}, {"out", false}});
    CHECK_FALSE(h.state()->findMedia(s)->markIn);
    CHECK(h.state()->findMedia(s)->markOut == f(5));
    h.ok("source.clearMarks", Json{{"media", "S"}});
    CHECK_FALSE(h.state()->findMedia(s)->markOut);

    // source.load reports the marks, the length and the patching a source edit would use.
    h.ok("source.setMarks", Json{{"media", "S"}, {"in", tc(2)}, {"out", tc(5)}});
    r = h.ok("source.load", Json{{"media", "S"}});
    CHECK(r.data["kind"] == "video");
    CHECK(r.data["duration"]["timecode"] == tc(20));
    CHECK(r.data["position"]["timecode"] == tc(2));
    CHECK(r.data["marks"]["out"]["timecode"] == tc(5));
    CHECK(r.data["streams"]["audio"] == 2);
    // V1, A1 and the second audio stream, which goes to a new Mic track: the sequence has one audio track.
    REQUIRE(r.data["patches"].size() == 3);
    CHECK(r.data["patches"][0] == Json{{"source", "V1"}, {"track", "V1"}, {"enabled", true}});
    CHECK(r.data["patches"][1] == Json{{"source", "A1"}, {"track", "A1"}, {"enabled", true}});
    CHECK(r.data["patches"][2] == Json{{"source", "A2"}, {"track", nullptr}, {"enabled", true}, {"newTrack", "Mic"}});

    // Stored marks drive the edit; given ones override them. The second edit's Mic finds the Mic track.
    r = h.ok("edit.overwriteFromSource", Json{{"media", "S"}, {"at", tc(0)}});
    CHECK(r.data["createdTracks"] == Json::array({"A2"}));
    CHECK_SESSION(h, "V1 |S2sA3aaaaaaa|\nA1 |S2sA3aaaaaaa|\nA2 |S2s|");
    r = h.ok("edit.overwriteFromSource",
             Json{{"media", "S"}, {"at", tc(6)}, {"sourceIn", tc(10)}, {"sourceOut", nullptr}});
    CHECK_FALSE(r.data.contains("createdTracks"));
    CHECK_SESSION(h, "V1 |S2sA3aS10sssssss|\nA1 |S2sA3aS10sssssss|\nA2 |S2s...S10sssssss|");
    CHECK(h.sequence(h.state()).audioTracks.at(1)->name == "Mic");
}

TEST_CASE("Stored marks past a shorter relinked file are clamped with a warning", "[command][source][marks]") {
    Timeline tl(kBase, kMedia);
    // Saved with marks 12..18 on S, then relinked to a 10-frame file.
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media('S')) {
            MediaItem copy = *m;
            copy.markIn = f(12);
            copy.markOut = f(18);
            copy.info.duration = f(10);
            for (auto& v : copy.info.video) {
                v.duration = f(10);
            }
            for (auto& a : copy.info.audio) {
                a.duration = f(10);
            }
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
    CommandHarness h(std::move(tl));
    const auto r = h.ok("edit.overwriteFromSource", Json{{"media", "S"}, {"at", tc(0)}});
    CHECK(r.warnings.size() == 1);
    CHECK(r.data["sourceIn"]["timecode"] == tc(0));
    CHECK(r.data["sourceOut"]["timecode"] == tc(10));
}

TEST_CASE("Source patching chooses the tracks; switched-off indicators take no part", "[command][source][patching]") {
    constexpr const char* picture = R"(
        V2     |............|
        V1 tgt |Aaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaa|
        A2     |............|
    )";
    SECTION("explicit patches: video to V2, audio 1 to A2, audio 2 off") {
        CommandHarness h(Timeline(picture, kMedia));
        const auto r = h.ok("edit.overwriteFromSource",
                            Json{{"media", "S"}, {"at", tc(2)}, {"sourceOut", tc(3)},
                                 {"patches", Json::array({Json{{"source", "V1"}, {"track", "V2"}},
                                                          Json{{"source", "A1"}, {"track", "A2"}},
                                                          Json{{"source", "A2"}, {"track", "A1"}, {"enabled", false}}})}});
        CHECK_SESSION(h, R"(
            V2 |..Sss.......|
            V1 |Aaaaaaaaaaaa|
            A1 |Aaaaaaaaaaaa|
            A2 |..Sss.......|
        )");
        CHECK(r.data["patches"][2] == Json{{"source", "A2"}, {"track", "A1"}, {"enabled", false}});
    }
    SECTION("the host's patching applies when the request has none") {
        auto host = std::make_shared<HostPatching>();
        Timeline tl(picture, kMedia);
        host->patches = {{{StreamKind::Video, 0}, tl.track("V2"), true}, {{StreamKind::Audio, 0}, tl.track("A1"), false}};
        command::EnginePolicy policy;
        policy.services = host;
        CommandHarness h(std::move(tl), policy);
        h.ok("edit.insertFromSource", Json{{"media", "T"}, {"at", tc(0)}, {"sourceOut", tc(2)}});
        CHECK_SESSION(h, R"(
            V2 |Tt............|
            V1 |..Aaaaaaaaaaaa|
            A1 |..Aaaaaaaaaaaa|
            A2 |..............|
        )");
    }
    SECTION("sequence.setPatches checks and orders a patching without storing it") {
        CommandHarness h(Timeline(picture, kMedia));
        const auto r = h.ok("sequence.setPatches",
                            Json{{"media", "S"}, {"patches", Json::array({Json{{"source", "A1"}, {"track", "A2"}},
                                                                          Json{{"source", "V1"}, {"track", "V2"}}})}});
        CHECK(r.data["patches"][0]["source"] == "V1");
        CHECK(r.data["patches"][1]["track"] == "A2");
        CHECK(r.warnings.size() == 1); // nothing keeps it outside the app
        CHECK_FALSE(r.committed);
        const auto bad = [&](Json patches) {
            return h.errorOf("sequence.setPatches", Json{{"media", "S"}, {"patches", std::move(patches)}});
        };
        CHECK(bad(Json::array({Json{{"source", "V1"}, {"track", "A1"}}})) == "InvalidParams");
        CHECK(bad(Json::array({Json{{"source", "A3"}, {"track", "A1"}}})) == "InvalidParams");
        CHECK(bad(Json::array({Json{{"source", "A1"}, {"track", "A1"}}, Json{{"source", "A2"}, {"track", "A1"}}})) ==
              "InvalidParams");
        CHECK(bad(Json::array({Json{{"source", "X1"}, {"track", "A1"}}})) == "InvalidParams");
    }
    SECTION("nothing patched and on is refused") {
        CommandHarness h(Timeline(picture, kMedia));
        CHECK(h.errorOf("edit.insertFromSource",
                        Json{{"media", "S"}, {"at", tc(0)}, {"patches", Json::array({Json{{"source", "V1"}, {"track", nullptr}}})}}) ==
              "InvalidParams");
    }
}

TEST_CASE("A drop ignores the sequence marks when asked", "[command][source][threepoint]") {
    CommandHarness h(Timeline(kBase, kMedia));
    h.ok("marker.markIn", Json{{"time", tc(5)}});
    h.ok("marker.markOut", Json{{"time", tc(8)}});
    h.ok("edit.overwriteFromSource",
         Json{{"media", "T"}, {"at", tc(1)}, {"sourceOut", tc(2)}, {"sequenceMarks", false}});
    CHECK_SESSION(h, "V1 |ATtA3aaaaaaa|\nA1 |ATtA3aaaaaaa|\nA2 |.Tt|");
    CHECK(h.sequence(h.state()).markIn == f(5)); // left alone
    CHECK(h.errorOf("edit.insertFromSource", Json{{"media", "T"}, {"sequenceMarks", false}}) == "InvalidParams");
}

TEST_CASE("Track targeting is set as one step, also after a source edit", "[command][source][targets]") {
    CommandHarness h(Timeline(R"(
        V2     |....|
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
        A2     |....|
    )",
                              kMedia));
    const std::uint64_t before = h.session.undoCount();
    auto r = h.ok("sequence.setTargets", Json{{"tracks", Json::array({"V2", "A2"})}});
    CHECK(h.session.undoCount() == before + 1);
    CHECK(r.data["targeted"] == Json::array({"V2", "A2"}));
    CHECK(edit::targetedTracks(h.sequence(h.state())) == std::vector<TrackId>{h.tl.track("V2"), h.tl.track("A2")});
    r = h.ok("sequence.setTargets", Json{{"tracks", Json::array()}});
    CHECK(edit::targetedTracks(h.sequence(h.state())).empty());

    h.ok("edit.overwriteFromSource", Json{{"media", "T"}, {"at", tc(0)}, {"sourceOut", tc(1)}, {"targets", Json::array({"V1"})}});
    CHECK(edit::targetedTracks(h.sequence(h.state())) == std::vector<TrackId>{h.tl.track("V1")});
}

TEST_CASE("Match frame and reverse match frame", "[command][source][matchframe]") {
    CommandHarness h(Timeline(R"(
        V2     |..C3cc....|
        V1 tgt |AaaS5sssss|
        A1 tgt |AaaS5sssss|
    )",
                              kMedia));
    SECTION("the topmost targeted clip; its used range becomes the source marks") {
        const auto r = h.ok("sequence.matchFrame", Json{{"time", tc(4)}});
        CHECK(r.data["track"] == "V1"); // V2 is not targeted
        CHECK(r.data["name"] == "S");
        CHECK(r.data["sourceTime"]["timecode"] == tc(6));
        CHECK(r.data["marks"]["in"]["timecode"] == tc(5));
        CHECK(r.data["marks"]["out"]["timecode"] == tc(12));
        CHECK(h.state()->findMedia(h.tl.media('S'))->markIn == f(5));
        CHECK(h.state()->findMedia(h.tl.media('S'))->markOut == f(12));
        CHECK(h.session.undoLabel() == "Match Frame");

        // Targeting V2 matches the title there; without a clip on the targeted tracks every track is searched.
        h.ok("sequence.setTargets", Json{{"tracks", Json::array({"V2"})}});
        CHECK(h.ok("sequence.matchFrame", Json{{"time", tc(4)}, {"setMarks", false}}).data["name"] == "C");
        CHECK(h.ok("sequence.matchFrame", Json{{"time", tc(8)}, {"setMarks", false}}).data["track"] == "V1");
        CHECK(h.errorOf("sequence.matchFrame", Json{{"time", tc(10)}}) == "NotFound");
    }
    SECTION("reverse match frame finds where a source frame is used") {
        auto r = h.ok("sequence.reverseMatchFrame", Json{{"media", "S"}, {"time", tc(7)}});
        CHECK(r.data["time"]["timecode"] == tc(5));
        CHECK(r.data["track"] == "V1");
        r = h.ok("sequence.reverseMatchFrame", Json{{"media", "A"}, {"time", tc(1)}, {"from", tc(2)}});
        CHECK(r.data["time"]["timecode"] == tc(1)); // no use after the playhead: the first one
        CHECK(h.errorOf("sequence.reverseMatchFrame", Json{{"media", "S"}, {"time", tc(2)}}) == "NotFound");
    }
}

TEST_CASE("Source edits route a multi-track recording's streams to tracks named for them",
          "[command][source][patching][multitrack]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
        A2     |....|
    )",
                kMedia);
    const MediaId obs = addRecording(tl, "OBS", {"Game", "Mic", "Discord"});
    CommandHarness h(std::move(tl));

    // The default patching: A1 holds clips, so Game takes the free A2 and Mic and Discord get new tracks.
    auto r = h.ok("source.load", Json{{"media", obs.value}});
    REQUIRE(r.data["patches"].size() == 4);
    CHECK(r.data["patches"][1] == Json{{"source", "A1"}, {"track", "A2"}, {"enabled", true}, {"newTrack", "Game"}});
    CHECK(r.data["patches"][2] == Json{{"source", "A2"}, {"track", nullptr}, {"enabled", true}, {"newTrack", "Mic"}});
    CHECK(r.data["patches"][3] ==
          Json{{"source", "A3"}, {"track", nullptr}, {"enabled", true}, {"newTrack", "Discord"}});

    r = h.ok("edit.insertFromSource", Json{{"media", obs.value}, {"at", tc(4)}, {"sourceOut", tc(2)}});
    CHECK(r.data["createdTracks"] == Json::array({"A3", "A4"}));
    CHECK(r.data["placed"].size() == 4);
    CHECK_SESSION(h, R"(
        V1 |AaaaOo|
        A1 |Aaaa..|
        A2 |....Oo|
        A3 |....Oo|
        A4 |....Oo|
    )");
    {
        const ProjectSnapshot snap = h.state();
        const Sequence& seq = h.sequence(snap);
        REQUIRE(seq.audioTracks.size() == 4);
        CHECK(seq.audioTracks[0]->name.empty());
        CHECK(seq.audioTracks[1]->name == "Game");
        CHECK(seq.audioTracks[1]->role == TrackRole::Effects);
        CHECK(seq.audioTracks[2]->name == "Mic");
        CHECK(seq.audioTracks[2]->role == TrackRole::Dialogue);
        CHECK(seq.audioTracks[3]->name == "Discord");
        CHECK(seq.audioTracks[3]->role == TrackRole::Dialogue);
    }
    // One undo step: the new tracks go with the clips.
    CHECK(h.session.undoLabel() == "Insert from Source");
    REQUIRE(h.session.undo());
    CHECK(h.sequence(h.state()).audioTracks.size() == 2);
    CHECK(h.sequence(h.state()).audioTracks[1]->name.empty());
}

TEST_CASE("A newTrack patch adds a named track", "[command][source][patching][multitrack]") {
    CommandHarness h(Timeline(kBase, kMedia));
    const auto r = h.ok("edit.overwriteFromSource",
                        Json{{"media", "T"},
                             {"at", tc(0)},
                             {"sourceOut", tc(2)},
                             {"patches", Json::array({Json{{"source", "A2"}, {"newTrack", "Voice Chat"}}})}});
    CHECK(r.data["createdTracks"] == Json::array({"A2"}));
    CHECK(r.data["patches"][0] ==
          Json{{"source", "A2"}, {"track", nullptr}, {"enabled", true}, {"newTrack", "Voice Chat"}});
    CHECK_SESSION(h, "V1 |Aaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaa|\nA2 |Tt|");
    {
        const ProjectSnapshot snap = h.state();
        const Track& added = *h.sequence(snap).audioTracks.at(1);
        CHECK(added.name == "Voice Chat");
        CHECK(added.role == TrackRole::Dialogue); // voice chat is dialogue, like the mic
    }

    // sequence.setPatches keeps the name; only audio source tracks take one.
    const Json named = Json::array({Json{{"source", "A1"}, {"newTrack", "Mic"}}});
    const auto checked = h.ok("sequence.setPatches", Json{{"media", "S"}, {"patches", named}});
    CHECK(checked.data["patches"][0]["newTrack"] == "Mic");
    CHECK(checked.data["patches"][0]["enabled"] == true);
    const Json picture = Json::array({Json{{"source", "V1"}, {"newTrack", "Picture"}}});
    CHECK(h.errorOf("edit.insertFromSource", Json{{"media", "T"}, {"at", tc(0)}, {"patches", picture}}) ==
          "InvalidParams");
}
