// Insert (,) and overwrite (.) edits, source patching and three-point editing.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <string_view>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// Source clip of `media` [in, in + frames) patched to the listed tracks: "V1" gets video stream 0, the
/// n-th listed audio track gets audio stream n.
edit::SourceClip source(const Timeline& tl, char media, std::int64_t in, std::int64_t frames,
                        std::initializer_list<std::string_view> tracks) {
    edit::SourceClip s;
    s.media = tl.media(media);
    s.sourceRange = {f(in), f(frames)};
    std::int32_t audio = 0;
    for (const std::string_view t : tracks) {
        const bool video = t.front() == 'V';
        s.patches.push_back({{video ? StreamKind::Video : StreamKind::Audio, video ? 0 : audio++}, tl.track(t)});
    }
    return s;
}

} // namespace

TEST_CASE("Insert splits the clip under the playhead and ripples every sync-locked track", "[model][edit][insert]") {
    Timeline tl(R"(
        V2     |....Cccc..|
        V1 tgt |Aaaaaa....|
        A1 tgt |Aaaaaa....|
        A2     |Mmmmmmmmmm|
    )");
    tl.linkMatching();

    const auto r = ok(edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 3, {"V1", "A1"}), f(2)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |.......Cccc..|
        V1 |AaSssA2aa....|
        A1 |AaSssA2aa....|
        A2 |Mm...M2mmmmmm|
    )");
    // The new A/V clips are linked; the halves of the cut A/V clip stay linked pairwise.
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@2+A1@2 V1@5+A1@5");
    CHECK(r.placed.size() == 2);
    CHECK(r.created.size() == 5); // 2 placed + 3 right-hand pieces
    CHECK(r.removed.empty());
    REQUIRE(r.playhead);
    CHECK(*r.playhead == f(5)); // Premiere parks the playhead after the inserted clip
}

TEST_CASE("Insert leaves locked and non-sync-locked tracks where they are", "[model][edit][insert]") {
    Timeline tl(R"(
        V2 lock   |..Cc|
        V1 tgt    |Aaaa|
        A1 nosync |Mmmm|
    )");
    const auto r = ok(edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 2, {"V1"}), f(1)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |..Cc..|
        V1 |ASsA1a|
        A1 |Mmmm..|
    )");
    // Untouched tracks are shared with the previous revision, not copied.
    CHECK(r.state.findSequence(tl.seq)->videoTracks[1] == tl.sequence().videoTracks[1]);
    CHECK(r.state.findSequence(tl.seq)->audioTracks[0] == tl.sequence().audioTracks[0]);
}

TEST_CASE("Insert at an edit point or after the end does not cut anything", "[model][edit][insert]") {
    Timeline tl(R"(
        V1 tgt |AaBb|
    )");
    const auto atEdit = ok(edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 2, {"V1"}), f(2)));
    CHECK_TIMELINE(atEdit.state, tl.seq, R"(
        V1 |AaSsBb|
    )");
    CHECK(atEdit.created == atEdit.placed);

    const auto afterEnd = ok(edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 2, {"V1"}), f(6)));
    CHECK_TIMELINE(afterEnd.state, tl.seq, R"(
        V1 |AaBb..Ss|
    )");
}

TEST_CASE("Overwrite replaces the range on the destination tracks only", "[model][edit][overwrite]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaa..Bbbb|
        A1 tgt |Aaaaaa..Bbbb|
        A2     |Mmmmmmmmmmmm|
    )");
    tl.linkMatching();
    const ClipId b = tl.clip("V1@8");

    const auto r = ok(edit::overwriteEdit(tl.state, tl.seq, source(tl, 'S', 10, 5, {"V1", "A1"}), f(4)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaaS10ssB1b|
        A1 |AaaaS10ssB1b|
        A2 |Mmmmmmmmmmmm|
    )");
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@4+A1@4 V1@9+A1@9");
    // A lost its tail and B its head; both are the same clips as before, only trimmed.
    CHECK(r.removed.empty());
    CHECK(clipAt(r.state, tl.seq, "V1@9") == b);
    CHECK(r.created == r.placed);
    REQUIRE(r.playhead);
    CHECK(*r.playhead == f(9));
}

TEST_CASE("Overwrite removes clips it covers completely", "[model][edit][overwrite]") {
    Timeline tl(R"(
        V1 tgt |..Bb..|
    )");
    const ClipId b = tl.clip("V1@2");
    const auto r = ok(edit::overwriteEdit(tl.state, tl.seq, source(tl, 'S', 0, 6, {"V1"}), f(0)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |Ssssss|
    )");
    CHECK(r.removed == std::vector<ClipId>{b});
}

TEST_CASE("Source patches to locked tracks are skipped", "[model][edit][patching]") {
    Timeline tl(R"(
        V1 tgt  |Aaaa|
        A1 lock |Aaaa|
    )");
    const auto r = ok(edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 2, {"V1", "A1"}), f(2)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaSsA2|
        A1 |Aaaa..|
    )");
    CHECK(r.placed.size() == 1);
    CHECK(links(r.state, tl.seq).empty()); // a single new clip is not linked to anything

    const auto allLocked = edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 2, {"A1"}), f(0));
    REQUIRE_FALSE(allLocked);
    CHECK(allLocked.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Source edits reject impossible requests without changing anything", "[model][edit][patching]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
        V2     |....|
        A1 tgt |....|
    )",
                {{'S', 10}});
    // Note: the picture lists V1 before V2, which is fine for building.
    SECTION("negative position") {
        CHECK_FALSE(edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 2, {"V1"}), f(-1)));
    }
    SECTION("source range past the end of the media") {
        const auto r = edit::overwriteEdit(tl.state, tl.seq, source(tl, 'S', 8, 4, {"V1"}), f(0));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("video patched to an audio track") {
        auto s = source(tl, 'S', 0, 2, {"V1"});
        s.patches[0].destination = tl.track("A1");
        CHECK_FALSE(edit::insertEdit(tl.state, tl.seq, s, f(0)));
    }
    SECTION("two streams patched to one track") {
        auto s = source(tl, 'S', 0, 2, {"V1"});
        s.patches.push_back({{StreamKind::Video, 0}, tl.track("V1")});
        CHECK_FALSE(edit::insertEdit(tl.state, tl.seq, s, f(0)));
    }
    SECTION("unknown media") {
        auto s = source(tl, 'S', 0, 2, {"V1"});
        s.media = MediaId{999999};
        const auto r = edit::insertEdit(tl.state, tl.seq, s, f(0));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::NotFound);
    }
    SECTION("empty source range") {
        CHECK_FALSE(edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 0, {"V1"}), f(0)));
    }
    SECTION("no patches") {
        CHECK_FALSE(edit::insertEdit(tl.state, tl.seq, source(tl, 'S', 0, 2, {}), f(0)));
    }
}

TEST_CASE("Default source patching follows Premiere: V1 and A1, A2, ...", "[model][edit][patching]") {
    Timeline tl(R"(
        V2 |.|
        V1 |.|
        A1 |.|
        A2 |.|
        A3 |.|
    )",
                {{'S', 10}});
    const auto patches = edit::defaultSourcePatches(tl.sequence(), *tl.state.findMedia(tl.media('S')));
    REQUIRE(patches.size() == 3);
    CHECK(patches[0].source == StreamRef{StreamKind::Video, 0});
    CHECK(patches[0].destination == tl.track("V1"));
    CHECK(patches[1].source == StreamRef{StreamKind::Audio, 0});
    CHECK(patches[1].destination == tl.track("A1"));
    CHECK(patches[2].source == StreamRef{StreamKind::Audio, 1});
    CHECK(patches[2].destination == tl.track("A2"));
    // A.10: two untitled streams with a picture read as a game recording's Game and Mic; the empty tracks they take
    // are named so (test_source_tracks.cpp).
    CHECK(patches[1].newTrack == "Game");
    CHECK(patches[2].newTrack == "Mic");
}

TEST_CASE("Three-point editing resolves position and source range like Premiere", "[model][edit][threepoint]") {
    Timeline tl(R"(
        V1 tgt |..........|
        A1 tgt |..........|
    )",
                {{'S', 20}});
    const auto patches = edit::defaultSourcePatches(tl.sequence(), *tl.state.findMedia(tl.media('S')));
    const auto resolve = [&](const ProjectState& state, edit::SourceMarks marks, Ticks playhead) {
        return edit::resolveThreePointEdit(state, tl.seq, tl.media('S'), patches, marks, playhead);
    };

    SECTION("no marks: the whole source at the playhead") {
        const auto e = resolve(tl.state, {}, f(3));
        REQUIRE(e);
        CHECK(e->at == f(3));
        CHECK(e->sourceRange == TimeRange{f(0), f(20)});
    }
    SECTION("source In/Out: that range at the playhead") {
        const auto e = resolve(tl.state, {f(4), f(9)}, f(3));
        REQUIRE(e);
        CHECK(e->at == f(3));
        CHECK(e->sourceRange == TimeRange{f(4), f(5)});
    }
    SECTION("sequence In wins over the playhead") {
        const auto marked = ok(edit::markIn(tl.state, tl.seq, f(6)));
        const auto e = resolve(marked.state, {f(4), f(9)}, f(3));
        REQUIRE(e);
        CHECK(e->at == f(6));
        CHECK(e->sourceRange == TimeRange{f(4), f(5)});
    }
    SECTION("sequence In and Out set the duration") {
        auto marked = ok(edit::markIn(tl.state, tl.seq, f(2))).state;
        marked = ok(edit::markOut(marked, tl.seq, f(4))).state; // Out at the end of frame 4: 3 frames
        const auto e = resolve(marked, {f(10), std::nullopt}, f(0));
        REQUIRE(e);
        CHECK(e->at == f(2));
        CHECK(e->sourceRange == TimeRange{f(10), f(3)});

        const auto backTimed = resolve(marked, {std::nullopt, f(12)}, f(0));
        REQUIRE(backTimed);
        CHECK(backTimed->sourceRange == TimeRange{f(9), f(3)});

        const auto tooShort = resolve(marked, {f(19), std::nullopt}, f(0));
        REQUIRE_FALSE(tooShort);
    }
    SECTION("only a sequence Out back-times the edit") {
        const auto marked = ok(edit::markOut(tl.state, tl.seq, f(7))).state; // ends at frame 8
        const auto e = resolve(marked, {f(0), f(5)}, f(0));
        REQUIRE(e);
        CHECK(e->at == f(3));
        CHECK(e->sourceRange == TimeRange{f(0), f(5)});

        // Longer than the time before the Out: the head of the source is dropped.
        const auto longer = resolve(marked, {f(0), f(12)}, f(0));
        REQUIRE(longer);
        CHECK(longer->at == f(0));
        CHECK(longer->sourceRange == TimeRange{f(4), f(8)});
    }
    SECTION("marks outside the media are rejected") {
        CHECK_FALSE(resolve(tl.state, {f(5), f(25)}, f(0)));
        CHECK_FALSE(resolve(tl.state, {f(5), f(5)}, f(0)));
    }
    SECTION("the resolved edit feeds insertEdit") {
        const auto e = resolve(tl.state, {f(2), f(5)}, f(4));
        REQUIRE(e);
        edit::SourceClip s{tl.media('S'), e->sourceRange, patches};
        const auto r = ok(edit::insertEdit(tl.state, tl.seq, s, e->at));
        // S's two untitled audio streams read as a game recording's Game and Mic (A.10): the second one, which has
        // no track here, gets a new one.
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |....S2s...|
            A1 |....S2s...|
            A2 |....S2s...|
        )");
    }
}

TEST_CASE("Still images without a source Out get the default still duration", "[model][edit][threepoint]") {
    Timeline tl(R"(
        V1 tgt |.|
    )");
    MediaItem still;
    still.name = "P";
    still.info.isStillImage = true;
    still.info.video.push_back({});
    auto added = edit::addMedia(tl.state, still);
    REQUIRE(added);
    const std::vector<edit::SourcePatch> patches{{{StreamKind::Video, 0}, tl.track("V1")}};
    const auto e = edit::resolveThreePointEdit(added->state, tl.seq, added->id, patches, {}, f(0));
    REQUIRE(e);
    CHECK(e->sourceRange == TimeRange{f(0), edit::kDefaultStillDuration});
}
