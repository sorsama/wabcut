// Caption tracks of a sequence (AI-3): edit ops, validation, ChangeSet and project IO.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

namespace {

CaptionTrack captions(std::vector<CaptionCue> cues) {
    CaptionTrack t;
    t.name = "Captions";
    t.language = "th";
    t.cues = std::move(cues);
    return t;
}

} // namespace

TEST_CASE("Caption tracks are added, replaced and removed", "[model][captions]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaa|
    )");
    const auto added = ok(edit::setCaptionTrack(tl.state, tl.seq,
                                                captions({{f(0), f(5), "สวัสดีครับ"}, {f(5), f(3), "two\nlines"}})));
    const CaptionTrackId id = added.createdCaptionTrack;
    REQUIRE(id.isValid());
    const Sequence& seq = *added.state.findSequence(tl.seq);
    REQUIRE(seq.captionTracks.size() == 1);
    CHECK(seq.captionTracks[0].id == id);
    CHECK(seq.captionTracks[0].cues[1].end() == f(8));
    CHECK(diff(tl.state, added.state).sequence(tl.seq)->captionsChanged);

    CaptionTrack replacement = captions({{f(1), f(2), "one"}});
    replacement.id = id;
    const auto replaced = ok(edit::setCaptionTrack(added.state, tl.seq, replacement));
    CHECK_FALSE(replaced.createdCaptionTrack.isValid());
    REQUIRE(replaced.state.findSequence(tl.seq)->captionTracks.size() == 1);
    CHECK(replaced.state.findSequence(tl.seq)->captionTracks[0].cues.size() == 1);

    const auto removed = ok(edit::removeCaptionTrack(replaced.state, tl.seq, id));
    CHECK(removed.state.findSequence(tl.seq)->captionTracks.empty());
    CHECK(edit::removeCaptionTrack(removed.state, tl.seq, id).error().code == ErrorCode::NotFound);
    CaptionTrack unknown = captions({});
    unknown.id = CaptionTrackId{999999};
    CHECK(edit::setCaptionTrack(tl.state, tl.seq, unknown).error().code == ErrorCode::NotFound);

    // A clip edit does not touch the captions.
    const ClipId clip = tl.clip("A1@0");
    const auto moved = ok(edit::moveClips(added.state, tl.seq, std::span(&clip, 1), f(2)));
    CHECK_FALSE(diff(added.state, moved.state).sequence(tl.seq)->captionsChanged);
}

TEST_CASE("Cues must be ordered, positive and not overlap", "[model][captions]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaa|
    )");
    CHECK_FALSE(edit::setCaptionTrack(tl.state, tl.seq, captions({{f(0), Ticks{}, "empty"}})));
    CHECK_FALSE(edit::setCaptionTrack(tl.state, tl.seq, captions({{f(-1), f(2), "early"}})));
    CHECK_FALSE(edit::setCaptionTrack(tl.state, tl.seq, captions({{f(0), f(5), "a"}, {f(4), f(2), "b"}})));
    CHECK_FALSE(edit::setCaptionTrack(tl.state, tl.seq, captions({{f(5), f(1), "a"}, {f(0), f(2), "b"}})));
    CHECK(edit::setCaptionTrack(tl.state, tl.seq, captions({{f(0), f(5), "a"}, {f(5), f(2), "b"}})));
}

TEST_CASE("Caption tracks survive the project file", "[model][captions][io]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaa|
    )");
    CaptionTrack second = captions({{f(2), f(4), "English & <more>"}});
    second.language = "en";
    second.enabled = false;
    const auto first = ok(edit::setCaptionTrack(tl.state, tl.seq, captions({{f(0), f(5), "สวัสดีครับ\nบรรทัดสอง"}})));
    const auto both = ok(edit::setCaptionTrack(first.state, tl.seq, second));
    const auto json = io::toJson(both.state);
    REQUIRE(json);
    CHECK(json->find("\"captionTracks\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == both.state);
    const Sequence& seq = *loaded->findSequence(tl.seq);
    REQUIRE(seq.captionTracks.size() == 2);
    CHECK(seq.captionTracks[0].cues[0].text == "สวัสดีครับ\nบรรทัดสอง");
    CHECK_FALSE(seq.captionTracks[1].enabled);
    CHECK(*io::toJson(*loaded) == *json);
    // A new caption track id never collides with ids handed out before the save.
    const auto third = ok(edit::setCaptionTrack(*loaded, tl.seq, captions({})));
    CHECK(third.createdCaptionTrack.value > both.createdCaptionTrack.value);
}
