// P.2 (E5) through the engine: Premiere's editing and sequence commands (CatalogEditing.cpp) preview in a dry run and
// commit as one undo step.

#include "CommandHarness.h"

#include <wabcut/command/EditingServices.h>
#include <wabcut/model/FrameScaling.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
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

/// The fixture with media `letter`'s picture resized to `width` x `height`.
Timeline resized(std::string_view picture, char letter, int width, int height) {
    Timeline tl(picture);
    const MediaId id = tl.media(letter);
    for (MediaPtr& m : tl.state.media) {
        if (m->id == id) {
            auto item = std::make_shared<MediaItem>(*m);
            item->info.video[0].width = width;
            item->info.video[0].height = height;
            m = std::move(item);
        }
    }
    return tl;
}

const Clip& clipIn(const ProjectSnapshot& s, SequenceId seq, ClipId id) { return s->findSequence(seq)->findClip(id)->clip(); }

} // namespace

TEST_CASE("clip.scaleToFrameSize, setToFrameSize and setToFillFrame scale video clips in one undo step",
          "[command][editing][scaling]") {
    Timeline tl = resized(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )", 'A', 3840, 2160);
    tl.linkMatching();
    CommandHarness h(std::move(tl));
    const ClipId video = h.clip("V1@0");
    const ClipId audio = h.clip("A1@0");

    // A dry run changes nothing; the linked audio clip is skipped.
    const auto preview = h.run(dryRun("clip.scaleToFrameSize", Json{{"clips", Json::array({audio.value})}}));
    REQUIRE(preview.ok());
    CHECK_FALSE(clipIn(h.state(), h.tl.seq, video).content().scaleToFrame);

    const std::size_t undo = h.session.undoCount();
    h.ok("clip.scaleToFrameSize", Json{{"clips", Json::array({audio.value})}});
    CHECK(h.session.undoCount() == undo + 1);
    CHECK(clipIn(h.state(), h.tl.seq, video).content().scaleToFrame);
    CHECK(scaleToFrameFactor(*h.state(), clipIn(h.state(), h.tl.seq, video), 1920, 1080) == Approx(0.5));
    const auto details = h.ok("clip.details", Json{{"clip", video.value}});
    CHECK(details.data.value("scaleToFrame", false));

    const auto set = h.ok("clip.setToFrameSize", Json{{"clips", Json::array({video.value})}});
    CHECK(set.data["scale"].get<double>() == Approx(50.0));
    CHECK_FALSE(clipIn(h.state(), h.tl.seq, video).content().scaleToFrame);
    const auto fill = h.ok("clip.setToFillFrame", Json{{"clips", Json::array({video.value})}});
    CHECK(fill.data["scale"].get<double>() == Approx(50.0)); // 16:9 into 16:9: fit and fill agree

    // Audio alone (not following links) has nothing to scale.
    CHECK(h.errorOf("clip.setToFrameSize", Json{{"clips", Json::array({audio.value})}, {"linked", false}}) ==
          "EditRejected");
}

TEST_CASE("media.setScaling sets the default media scaling later edits apply", "[command][editing][scaling]") {
    CommandHarness h(resized(R"(
        V1 tgt |..........|
        A1 tgt |..........|
    )", 'U', 3840, 2160));
    const MediaId uhd = h.tl.media('U');
    h.ok("media.setScaling", Json{{"media", Json::array({uhd.value})}, {"scaling", "setToFrameSize"}});
    CHECK(h.state()->findMedia(uhd)->scaling == MediaScaling::SetToFrame);
    h.ok("clip.overwrite", Json{{"media", uhd.value}, {"at", 0}, {"sourceIn", 0}, {"sourceOut", tc(5)}});
    CHECK(h.sequence(h.state()).videoTracks[0]->clips[0].properties()[ParamId::Scale].constant() == Approx(0.5));
    CHECK(h.errorOf("media.setScaling", Json{{"media", Json::array({uhd.value})}, {"scaling", "stretch"}}) ==
          "InvalidParams");
}

TEST_CASE("Duplicate, New Sequence From Clip and Sequence Settings run as commands", "[command][editing][sequence]") {
    CommandHarness h(R"(
        V1 tgt |A5aaaaa.B4bbbbbbbbC3c|
        A1 tgt |A5aaaaa.B4bbbbbbbbC3c|
    )");
    // Duplicate a sequence and an item.
    const std::size_t sequences = h.state()->sequences.size();
    const auto dup = h.ok("sequence.duplicate");
    CHECK(h.state()->sequences.size() == sequences + 1);
    CHECK(dup.data["name"] == "Fixture Copy");
    const auto item = h.ok("media.duplicate", Json{{"media", "A"}});
    CHECK(item.data["name"] == "A Copy");
    REQUIRE(item.createdIds.size() == 1);

    // New Sequence From Clip: named after the item, with it at the start.
    const auto fromClip = h.ok("sequence.newFromClip", Json{{"media", "B"}});
    CHECK(fromClip.data["name"] == "B");
    CHECK(fromClip.data["clips"].get<int>() >= 2);
    CHECK(fromClip.data["frameRate"]["num"] == 25);

    // Settings: read, then a preset with a new timebase in one undo step.
    const auto read = h.ok("sequence.settings", Json{{"sequence", h.tl.seq.value}});
    CHECK(read.data["width"] == 1920);
    const std::size_t undo = h.session.undoCount();
    const auto preview = h.run(Json{{"command", "sequence.setSettings"},
                                    {"params", Json{{"sequence", h.tl.seq.value}, {"preset", "1080p29.97df"}}},
                                    {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(h.sequence(h.state()).frameRate == kRate); // nothing committed
    const auto set = h.ok("sequence.setSettings", Json{{"sequence", h.tl.seq.value},
                                                       {"preset", "1080p29.97df"},
                                                       {"startTimecode", "01:00:00;00"},
                                                       {"videoDisplay", "frames"}});
    CHECK(h.session.undoCount() == undo + 1);
    const Sequence& s = h.sequence(h.state());
    CHECK(s.frameRate == Rate{30000, 1001});
    CHECK(s.dropFrame);
    CHECK(s.videoDisplay == VideoTimeDisplay::Frames);
    CHECK(s.startTimecode > Ticks::fromSeconds(3590));
    CHECK(set.data["dropFrame"] == true);
    // Every edit point on the 29.97 grid.
    s.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            CHECK(framesToTicks(ticksToFrames(c.start(), s.frameRate), s.frameRate) == c.start());
            CHECK(framesToTicks(ticksToFrames(c.end(), s.frameRate), s.frameRate) == c.end());
        }
    });
    // A frame rate by number, and bad values.
    h.ok("sequence.setSettings", Json{{"sequence", h.tl.seq.value}, {"frameRate", 23.976}});
    CHECK(h.sequence(h.state()).frameRate == Rate{24000, 1001});
    CHECK(h.errorOf("sequence.setSettings", Json{{"sequence", h.tl.seq.value}, {"frameRate", 2000}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.setSettings", Json{{"sequence", h.tl.seq.value}, {"preset", "8k"}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.setSettings", Json{{"sequence", h.tl.seq.value}, {"width", 8}}) == "InvalidParams");
    // 0.2: the sequence's label colour.
    const auto labelled = h.ok("sequence.setSettings", Json{{"sequence", h.tl.seq.value}, {"label", 6}});
    CHECK(h.sequence(h.state()).colorIndex == 6);
    CHECK(labelled.data["label"] == 6);
    CHECK(h.ok("sequence.settings", Json{{"sequence", h.tl.seq.value}}).data["label"] == 6);
    CHECK(h.errorOf("sequence.setSettings", Json{{"sequence", h.tl.seq.value}, {"label", 16}}) == "InvalidParams");
}

TEST_CASE("Replace, Make Offline and Replace Footage run as commands", "[command][editing][replace]") {
    Timeline tl(R"(
        V1 tgt |..A10aaaaa..|
        A1 tgt |..A10aaaaa..|
    )");
    tl.linkMatching();
    const MediaItem original = *tl.state.findMedia(tl.media('A'));
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::MediaProbeServices>([original](const std::filesystem::path& file)
                                                                        -> std::expected<MediaItem, Error> {
        if (file.filename() != "A_v2.mov") {
            return std::unexpected(Error{ErrorCode::NotFound, "no such file"});
        }
        MediaItem item = original;
        item.name = "A_v2.mov";
        item.locator.path = "D:/footage/A_v2.mov";
        return item;
    });
    CommandHarness h(std::move(tl), policy);
    const ClipId video = h.clip("V1@2");
    const auto replaced = h.ok("clip.replace", Json{{"clips", Json::array({video.value})}, {"media", "B"},
                                                    {"sourceIn", tc(40)}});
    CHECK(replaced.data["clips"] == 2);
    CHECK_SESSION(h, R"(
        V1 |..B40bbbbb..|
        A1 |..B40bbbbb..|
    )");
    h.ok("clip.replace", Json{{"clips", Json::array({video.value})}, {"media", "C"},
                                                   {"matchFrame", Json{{"sourceTime", tc(50)}, {"at", tc(4)}}}});
    CHECK(h.sequence(h.state()).videoTracks[0]->clips[0].sourceTimeAt(f(4)) == f(50));
    CHECK(h.errorOf("clip.replace", Json{{"clips", Json::array({video.value})}, {"media", "B"}, {"sourceIn", tc(998)}}) ==
          "EditRejected");

    h.ok("media.makeOffline", Json{{"media", Json::array({"A"})}});
    CHECK(h.state()->findMedia(h.tl.media('A'))->madeOffline);
    const auto footage = h.ok("media.replaceFootage", Json{{"media", "A"}, {"path", "D:/footage/A_v2.mov"}});
    CHECK(footage.data["name"] == "A_v2.mov");
    CHECK_FALSE(h.state()->findMedia(h.tl.media('A'))->madeOffline);
    CHECK(h.errorOf("media.replaceFootage", Json{{"media", "B"}, {"path", "D:/footage/missing.mov"}}) == "NotFound");

    // Without a host that reads files, Replace Footage is refused.
    CommandHarness bare("    V1 tgt |Aaaa|\n");
    CHECK(bare.errorOf("media.replaceFootage", Json{{"media", "A"}, {"path", "D:/x.mov"}}) == "InvalidRequest");
}

TEST_CASE("Subsequences, through edits, gaps and frame holds run as commands", "[command][editing][sequenceTools]") {
    Timeline tl(R"(
        V1 tgt |..Aaa..Bbb...Ccc|
        A1 tgt |..Aaa..Bbb...Ccc|
    )");
    tl.linkMatching();
    CommandHarness h(std::move(tl));

    const auto next = h.ok("sequence.goToGap", Json{{"time", tc(3)}, {"direction", "next"}});
    CHECK(next.data["time"]["ticks"] == f(5).value);
    const auto previous = h.ok("sequence.goToGap", Json{{"time", tc(3)}, {"direction", "previous"}, {"scope", "track"}});
    CHECK(previous.data["time"]["ticks"] == 0);
    const auto none = h.ok("sequence.goToGap", Json{{"time", tc(15)}});
    CHECK(none.data["time"].is_null());

    h.ok("sequence.closeGap");
    CHECK_SESSION(h, R"(
        V1 |AaaBbbCcc.......|
        A1 |AaaBbbCcc.......|
    )");
    CHECK(h.errorOf("sequence.closeGap") == "EditRejected");

    h.ok("sequence.addEdit", Json{{"time", tc(1)}, {"tracks", Json::array({"V1", "A1"})}});
    CHECK(h.sequence(h.state()).videoTracks[0]->clips.size() == 4);
    const auto joined = h.ok("sequence.joinThroughEdits");
    CHECK(joined.data["joined"] == 2);
    CHECK(h.sequence(h.state()).videoTracks[0]->clips.size() == 3);
    CHECK(h.errorOf("sequence.joinThroughEdits") == "EditRejected");

    const ClipId b = h.clip("V1@3");
    const auto held = h.ok("clip.frameHoldOptions", Json{{"clip", b.value}, {"holdOn", "playhead"}, {"time", tc(4)},
                                                         {"holdFilters", true}});
    CHECK(held.data["held"] == true);
    CHECK(held.data["holdFilters"] == true);
    const Clip& frozen = h.sequence(h.state()).videoTracks[0]->clips[1];
    CHECK(frozen.isFreeze());
    CHECK(frozen.sourceIn == f(1));
    h.ok("clip.frameHoldOptions", Json{{"clip", b.value}, {"enabled", false}});
    CHECK_FALSE(h.sequence(h.state()).videoTracks[0]->clips[1].isFreeze());
    CHECK(h.errorOf("clip.frameHoldOptions", Json{{"clip", b.value}, {"holdOn", "sequenceTimecode"}}) == "InvalidParams");

    // Last (it adds a sequence): a subsequence of B and its linked audio.
    const auto sub = h.ok("sequence.makeSubsequence", Json{{"clips", Json::array({b.value})}});
    CHECK(sub.data["name"] == "Fixture Sub_01");
    CHECK(sub.data["clips"] == 2);
}

TEST_CASE("Merge Clips, Interpret Footage and Modify Timecode run as commands", "[command][editing][footage]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa....|
        A1 tgt |Aaaa....|
    )");
    h.ok("source.setMarks", Json{{"media", "A"}, {"in", tc(40)}});
    h.ok("source.setMarks", Json{{"media", "R"}, {"in", tc(10)}});
    const auto merged = h.ok("media.mergeClips", Json{{"video", "A"}, {"audio", Json::array({"R"})},
                                                      {"sync", "inPoints"}, {"removeCameraAudio", true}});
    CHECK(merged.data["name"] == "A - Merged");
    REQUIRE(merged.createdIds.size() == 1);
    const Sequence* m = h.state()->findSequence(SequenceId{merged.createdIds.front()});
    REQUIRE(m);
    REQUIRE(m->multicam);
    CHECK(m->multicam->merged);
    CHECK(h.errorOf("media.mergeClips", Json{{"video", "A"}, {"audio", Json::array({"R"})}, {"sync", "audio"}}) ==
          "InvalidRequest"); // no audio analysis here

    const auto interpreted = h.ok("media.interpretFootage", Json{{"media", Json::array({"A"})}, {"frameRate", 12.5}});
    CHECK(interpreted.data["conformSpeed"] == 50.0);
    CHECK(h.sequence(h.state()).videoTracks[0]->clips[0].speed == Speed{1, 2});
    h.ok("media.interpretFootage", Json{{"media", Json::array({"A"})}, {"frameRate", nullptr}, {"pixelAspect", 1.3333},
                                        {"premultiplication", "premultiplied"}});
    const MediaItem& a = *h.state()->findMedia(h.tl.media('A'));
    REQUIRE(a.interpretation);
    CHECK_FALSE(a.interpretation->frameRate);
    CHECK(a.interpretation->premultiplication == AlphaMode::Premultiplied);
    CHECK(h.sequence(h.state()).videoTracks[0]->clips[0].speed == Speed{1, 1});

    h.ok("media.setTimecode", Json{{"media", Json::array({"A"})}, {"timecode", "01:00:00:00"}, {"reelName", "A001"}});
    REQUIRE(h.state()->findMedia(h.tl.media('A'))->timecode);
    CHECK(h.state()->findMedia(h.tl.media('A'))->timecode->reelName == "A001");
    CHECK(h.errorOf("media.setTimecode", Json{{"media", Json::array({"A"})}, {"timecode", "nonsense"}}) == "InvalidParams");
}
TEST_CASE("RAW Source Settings run as commands", "[command][editing][raw]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
        V2 tgt |Bbbb|
    )");
    // A is a ProRes RAW clip, B an ordinary one.
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media('A')) {
            auto raw = std::make_shared<MediaItem>(*m);
            raw->locator.path = "D:/Shoot/A001.mov";
            VideoStreamInfo v;
            v.codec = "prores_raw";
            v.pixelFormat = "bayer_rggb16le";
            v.width = 64;
            v.height = 32;
            raw->info.video = {v};
            m = raw;
        }
    }
    CommandHarness h(tl);
    const auto none = h.ok("media.rawSourceSettings", Json{{"media", "A"}});
    CHECK(none.data["raw"] == true);
    CHECK(none.data["whiteBalance"] == "asShot");
    CHECK(none.data["exposure"] == 0.0);
    const std::size_t before = h.session.undoCount();
    const auto set = h.ok("media.setRawSourceSettings", Json{{"media", Json::array({"A"})}, {"exposure", 1.5},
                                                             {"whiteBalance", "custom"}, {"temperature", 3200}, {"tint", 5}});
    CHECK(h.session.undoCount() == before + 1);
    CHECK(set.data["exposure"] == 1.5);
    CHECK(set.data["temperature"] == 3200);
    CHECK(h.state()->findMedia(h.tl.media('A'))->locator.path == "D:/Shoot/A001.mov?raw=exposure:+1.50,white:3200:5");
    // Absent fields keep the item's.
    h.ok("media.setRawSourceSettings", Json{{"media", Json::array({"A"})}, {"tint", -20}});
    const auto now = h.ok("media.rawSourceSettings", Json{{"media", "A"}});
    CHECK(now.data["exposure"] == 1.5);
    CHECK(now.data["temperature"] == 3200);
    CHECK(now.data["tint"] == -20);
    h.ok("media.setRawSourceSettings", Json{{"media", Json::array({"A"})}, {"exposure", 0}, {"whiteBalance", "asShot"}});
    CHECK(h.state()->findMedia(h.tl.media('A'))->locator.path == "D:/Shoot/A001.mov");
    // Out of bounds, a temperature with As Shot, an item that is not RAW.
    CHECK(h.errorOf("media.setRawSourceSettings", Json{{"media", Json::array({"A"})}, {"exposure", 9}}) == "InvalidParams");
    CHECK(h.errorOf("media.setRawSourceSettings",
                    Json{{"media", Json::array({"A"})}, {"whiteBalance", "asShot"}, {"temperature", 3000}}) == "InvalidParams");
    CHECK(h.errorOf("media.setRawSourceSettings", Json{{"media", Json::array({"B"})}, {"exposure", 1}}) == "EditRejected");
    CHECK(h.ok("media.rawSourceSettings", Json{{"media", "B"}}).data["raw"] == false);
}

TEST_CASE("sequence.setSettings rippleMarkers turns marker rippling off and reports it", "[command][editing][markers]") {
    CommandHarness h(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb|
        A1 tgt |AaaaaaaaaaBbbbbbbbbb|
    )");
    h.ok("marker.add", Json{{"time", tc(12)}, {"name", "Chapter"}});
    CHECK(h.ok("sequence.settings", Json{{"sequence", h.tl.seq.value}}).data["rippleMarkers"] == true);
    // On (the default): an extract before the marker pulls it back.
    h.ok("sequence.extract", Json{{"in", tc(2)}, {"out", tc(4)}});
    REQUIRE(h.sequence(h.state()).markers.size() == 1);
    CHECK(h.sequence(h.state()).markers[0].start == f(10));
    // Off: one undo step, reported in data; the next extract leaves the marker where it is.
    const std::size_t undo = h.session.undoCount();
    const auto off = h.ok("sequence.setSettings", Json{{"sequence", h.tl.seq.value}, {"rippleMarkers", false}});
    CHECK(off.data["rippleMarkers"] == false);
    CHECK(h.session.undoCount() == undo + 1);
    CHECK_FALSE(h.sequence(h.state()).rippleMarkers);
    CHECK(h.ok("sequence.settings", Json{{"sequence", h.tl.seq.value}}).data["rippleMarkers"] == false);
    h.ok("sequence.extract", Json{{"in", tc(2)}, {"out", tc(4)}});
    CHECK(h.sequence(h.state()).markers[0].start == f(10));
    CHECK(h.errorOf("sequence.setSettings", Json{{"sequence", h.tl.seq.value}, {"rippleMarkers", "no"}}) == "InvalidParams");
    // Undoing the extract and the setting turns it back on.
    REQUIRE(h.session.undo());
    REQUIRE(h.session.undo());
    CHECK(h.sequence(h.state()).rippleMarkers);
    CHECK(h.sequence(h.state()).markers[0].start == f(10));
}
