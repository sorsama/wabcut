// 0.7 (card 106): solo for video tracks. While any video track of a sequence is soloed only the soloed video tracks
// show, as only soloed audio tracks are heard: for the sequence itself and for a nested sequence it plays.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// The media letters (names) of the leaves a sequence shows at `frame`, bottom track first.
std::string shown(const ProjectState& state, SequenceId seq, std::int64_t frame) {
    std::string out;
    for (const ResolvedLeaf& leaf : resolveSequenceLeaves(state, seq, StreamKind::Video, f(frame)))
        out += state.findMedia(leaf.clip->media)->name;
    return out;
}

} // namespace

TEST_CASE("A soloed video track is the only one shown (card 106)", "[model][solo][video]") {
    Timeline tl(R"(
        V3     |Cccc|
        V2 solo|Bbbb|
        V1     |Aaaa|
    )");
    const Sequence& seq = *tl.state.findSequence(tl.seq);
    CHECK(anyVideoSolo(seq));
    CHECK(videoTrackShown(seq, *seq.videoTracks[1]));
    CHECK_FALSE(videoTrackShown(seq, *seq.videoTracks[0]));
    CHECK_FALSE(videoTrackShown(seq, *seq.videoTracks[2]));
    CHECK(shown(tl.state, tl.seq, 1) == "B");

    SECTION("two soloed tracks show together, in order") {
        const ProjectState two = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V3"), [] {
                                        edit::TrackStateChange change;
                                        change.solo = true;
                                        return change;
                                    }())).state;
        CHECK(shown(two, tl.seq, 1) == "BC");
    }
    SECTION("unsoloing brings every track back") {
        edit::TrackStateChange change;
        change.solo = false;
        const ProjectState none = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V2"), change)).state;
        CHECK_FALSE(anyVideoSolo(*none.findSequence(tl.seq)));
        CHECK(shown(none, tl.seq, 1) == "ABC");
    }
    SECTION("the eye still hides a soloed track") {
        edit::TrackStateChange change;
        change.enabled = false;
        const ProjectState hidden = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V2"), change)).state;
        CHECK_FALSE(videoTrackShown(*hidden.findSequence(tl.seq), *hidden.findSequence(tl.seq)->videoTracks[1]));
        CHECK(shown(hidden, tl.seq, 1).empty()); // the other tracks stay out: a solo is still on
    }
    SECTION("a video solo does not touch audio, and an audio solo does not touch video") {
        Timeline mixed(R"(
            V2     |Bbbb|
            V1     |Aaaa|
            A1 solo|Aaaa|
            A2     |Bbbb|
        )");
        const Sequence& m = *mixed.state.findSequence(mixed.seq);
        CHECK_FALSE(anyVideoSolo(m));
        CHECK(shown(mixed.state, mixed.seq, 1) == "AB");
        CHECK(resolveSequenceLeaves(mixed.state, mixed.seq, StreamKind::Audio, f(1)).size() == 1);
    }
    SECTION("the solo is saved with the project and loads back") {
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "wabcut_video_solo.wabcut";
        REQUIRE(io::saveProject(tl.state, file, io::SaveOptions{0}));
        const auto loaded = io::loadProject(file);
        std::filesystem::remove(file);
        REQUIRE(loaded.has_value());
        const Sequence& back = *loaded->findSequence(tl.seq);
        CHECK(anyVideoSolo(back));
        CHECK(shown(*loaded, tl.seq, 1) == "B");
    }
}

TEST_CASE("A nested sequence shows only its soloed video track (card 106)", "[model][solo][video][nesting]") {
    // The inner sequence has two video tracks, the lower one soloed; the outer one plays it as a nested clip.
    Timeline inner(R"(
        V2     |Bbbb|
        V1 solo|Aaaa|
    )");
    const Sequence& innerSeq = *inner.state.findSequence(inner.seq);
    Clip nested;
    nested.sequence = inner.seq;
    REQUIRE(nested.isNested());
    // The clip reads the tracks nestedTracksRead() gives: only the soloed one.
    nested.stream = {StreamKind::Video, 0};
    const std::vector<std::size_t> read = nestedTracksRead(innerSeq, nested);
    REQUIRE(read.size() == 1);
    CHECK(read.front() == 0);
}

TEST_CASE("The composite plan (render bar, preview cache) reads the solo like the picture does (card 106)",
          "[model][solo][video][plan]") {
    Timeline tl(R"(
        V3     |Cccc|
        V2 solo|Bbbb|
        V1     |Aaaa|
    )");
    // Before the fix resolveComposite read only the eye: all three tracks composited under a solo.
    const CompositePlan soloed = resolveComposite(tl.state, tl.seq, f(1));
    REQUIRE(soloed.steps.size() == 1);
    CHECK(tl.state.findMedia(soloed.steps.front().clip->media)->name == "B");

    edit::TrackStateChange unsolo;
    unsolo.solo = false;
    const ProjectState none = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V2"), unsolo)).state;
    CHECK(resolveComposite(none, tl.seq, f(1)).steps.size() == 3);

    edit::TrackStateChange eye;
    eye.enabled = false;
    const ProjectState hidden = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V2"), eye)).state;
    CHECK(resolveComposite(hidden, tl.seq, f(1)).steps.empty()); // the soloed track is hidden: nothing shows
}
