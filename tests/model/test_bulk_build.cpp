// X.1: the bulk-build API importers use (edit::placeClips, edit::linkClipGroups) gives what the single edits give, in
// one edit, and refuses what they refuse; the sequence start timecode is checked, saved only when set and read back.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V1      |Aaaa....Bbbb|
    V2      |............|
    A1      |Aaaa........|
    A2 lock |............|
)";

/// Every clip of the sequence: track, start, length, media, stream, source In and name.
std::string clipList(const ProjectState& state, SequenceId seq) {
    std::ostringstream out;
    state.findSequence(seq)->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            out << t.id.value << "@" << c.start().value << "+" << c.duration().value << " m" << c.media.value << " s"
                << static_cast<int>(c.stream.kind) << "." << c.stream.index << " in" << c.sourceIn.value << " '" << c.name()
                << "'\n";
        }
    });
    return out.str();
}

ErrorCode errorOf(const edit::EditOutcome& outcome) {
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error().code;
}

} // namespace

TEST_CASE("placeClips gives the clips overwrite edits give - in one edit (X.1)", "[model][bulk]") {
    const Timeline tl(kPicture);
    const std::vector<edit::ClipPlacement> placements{
        {tl.track("V2"), tl.media('B'), {StreamKind::Video, 0}, f(4), f(2), Ticks{}},
        {tl.track("V2"), tl.media('A'), {StreamKind::Video, 0}, f(0), f(3), f(5)},
        {tl.track("A1"), tl.media('B'), {StreamKind::Audio, 0}, f(8), f(4), f(1)},
        {tl.track("V1"), tl.media('A'), {StreamKind::Video, 0}, f(4), f(4), f(20)}, // the gap between A and B, exactly
    };
    const edit::EditResult bulk = ok(edit::placeClips(tl.state, tl.seq, placements));
    REQUIRE(bulk.placed.size() == placements.size());

    ProjectState single = tl.state;
    for (const edit::ClipPlacement& p : placements) {
        edit::SourceClip source;
        source.media = p.media;
        source.sourceRange = {p.sourceIn, p.duration};
        source.patches.push_back({p.stream, p.track});
        single = ok(edit::overwriteEdit(single, tl.seq, source, p.start)).state;
    }
    CHECK(clipList(bulk.state, tl.seq) == clipList(single, tl.seq));
    CHECK_TIMELINE(bulk.state, tl.seq, R"(
        V2 |A5a.Bb......|
        V1 |AaaaA20aBbbb|
        A1 |Aaaa....B1bb|
        A2 |............|
    )");
    // The new clips in the order given, nothing linked.
    const Sequence& seq = *bulk.state.findSequence(tl.seq);
    CHECK(seq.findClip(bulk.placed[0])->clip().start() == f(4));
    CHECK(seq.findClip(bulk.placed[3])->clip().sourceIn == f(20));
    CHECK(links(bulk.state, tl.seq).empty());
    CHECK(ok(edit::placeClips(tl.state, tl.seq, {})).placed.empty());
}

TEST_CASE("placeClips refuses what an overwrite would cover or cannot play - and places nothing (X.1)", "[model][bulk]") {
    const Timeline tl(kPicture);
    const auto one = [&](edit::ClipPlacement p) {
        return edit::placeClips(tl.state, tl.seq, std::vector<edit::ClipPlacement>{p});
    };
    const edit::ClipPlacement good{tl.track("V2"), tl.media('A'), {StreamKind::Video, 0}, f(0), f(3), Ticks{}};
    CHECK(one(good).has_value());

    auto overlapsClip = good;
    overlapsClip.track = tl.track("V1"); // A is there
    CHECK(errorOf(one(overlapsClip)) == ErrorCode::InvalidArgument);
    // Two placements over each other.
    auto second = good;
    second.start = f(2);
    CHECK(errorOf(edit::placeClips(tl.state, tl.seq, std::vector{good, second})) == ErrorCode::InvalidArgument);
    auto locked = good;
    locked.track = tl.track("A2");
    locked.stream = {StreamKind::Audio, 0};
    CHECK(errorOf(one(locked)) == ErrorCode::InvalidArgument);
    auto wrongKind = good;
    wrongKind.stream = {StreamKind::Audio, 0}; // sound on a video track
    CHECK(errorOf(one(wrongKind)) == ErrorCode::InvalidArgument);
    auto noStream = good;
    noStream.stream = {StreamKind::Video, 7};
    CHECK(errorOf(one(noStream)) == ErrorCode::InvalidArgument);
    auto empty = good;
    empty.duration = Ticks{};
    CHECK(errorOf(one(empty)) == ErrorCode::InvalidArgument);
    auto beforeZero = good;
    beforeZero.start = -f(1);
    CHECK(errorOf(one(beforeZero)) == ErrorCode::InvalidArgument);
    auto pastMedia = good;
    pastMedia.sourceIn = tl.state.findMedia(tl.media('A'))->sourceExtent(good.stream).value_or(Ticks{}) - f(1);
    CHECK(errorOf(one(pastMedia)) == ErrorCode::InvalidArgument);
    auto noMedia = good;
    noMedia.media = MediaId{999'999};
    CHECK(errorOf(one(noMedia)) == ErrorCode::NotFound);
    auto noTrack = good;
    noTrack.track = TrackId{999'999};
    CHECK(errorOf(one(noTrack)) == ErrorCode::NotFound);
}

TEST_CASE("linkClipGroups links each group as linkClips would - in one edit (X.1)", "[model][bulk]") {
    Timeline tl(R"(
        V1 |Aaaa..Bbbb|
        A1 |Aaaa..Bbbb|
        A2 |Aaaa......|
    )");
    const std::vector<std::vector<ClipId>> groups{{tl.clip("V1@0"), tl.clip("A1@0"), tl.clip("A2@0")},
                                                  {tl.clip("V1@6"), tl.clip("A1@6")}};
    const edit::EditResult bulk = ok(edit::linkClipGroups(tl.state, tl.seq, groups));
    ProjectState single = tl.state;
    for (const auto& g : groups) {
        single = ok(edit::linkClips(single, tl.seq, g)).state;
    }
    CHECK(links(bulk.state, tl.seq) == links(single, tl.seq));
    CHECK(links(bulk.state, tl.seq) == "V1@0+A1@0+A2@0 V1@6+A1@6");
    // Regrouping takes clips out of their old groups, as linkClips does.
    const std::vector<std::vector<ClipId>> regroup{{tl.clip("V1@0"), tl.clip("V1@6")}};
    CHECK(links(ok(edit::linkClipGroups(bulk.state, tl.seq, regroup)).state, tl.seq) ==
          links(ok(edit::linkClips(bulk.state, tl.seq, regroup.front())).state, tl.seq));

    CHECK(errorOf(edit::linkClipGroups(tl.state, tl.seq, std::vector<std::vector<ClipId>>{{tl.clip("V1@0")}})) ==
          ErrorCode::InvalidArgument);
    CHECK(errorOf(edit::linkClipGroups(tl.state, tl.seq,
                                       std::vector<std::vector<ClipId>>{{tl.clip("V1@0"), tl.clip("A1@0")},
                                                                        {tl.clip("A1@0"), tl.clip("V1@6")}})) ==
          ErrorCode::InvalidArgument); // A1@0 in two groups
    CHECK(errorOf(edit::linkClipGroups(tl.state, tl.seq,
                                       std::vector<std::vector<ClipId>>{{tl.clip("V1@0"), ClipId{999'999}}})) ==
          ErrorCode::NotFound);
    CHECK(ok(edit::linkClipGroups(tl.state, tl.seq, {})).state.findSequence(tl.seq) != nullptr);
}

TEST_CASE("The sequence start timecode is checked - saved only when set - and read back (X.1)", "[model][bulk][io]") {
    const Timeline tl(kPicture);
    CHECK(errorOf(edit::setSequenceStartTimecode(tl.state, tl.seq, Ticks{-1})) == ErrorCode::InvalidArgument);
    CHECK(errorOf(edit::setSequenceStartTimecode(tl.state, tl.seq, kMaxStartTimecode + Ticks{1})) ==
          ErrorCode::InvalidArgument);
    CHECK(errorOf(edit::setSequenceStartTimecode(tl.state, SequenceId{999'999}, Ticks{})) == ErrorCode::NotFound);

    // Not set: the file has no key for it (projects without one save as they did).
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain.has_value());
    CHECK(plain->find("startTimecode") == std::string::npos);

    const ProjectState hour = ok(edit::setSequenceStartTimecode(tl.state, tl.seq, Ticks::fromSeconds(3600))).state;
    CHECK(hour.findSequence(tl.seq)->startTimecode == Ticks::fromSeconds(3600));
    const auto saved = io::toJson(hour);
    REQUIRE(saved.has_value());
    CHECK(saved->find("\"startTimecode\"") != std::string::npos);
    const auto back = io::fromJson(*saved);
    REQUIRE(back.has_value());
    CHECK(back->findSequence(tl.seq)->startTimecode == Ticks::fromSeconds(3600));
    // Set back to 0 it is not written again.
    const ProjectState zero = ok(edit::setSequenceStartTimecode(hour, tl.seq, Ticks{})).state;
    CHECK(io::toJson(zero).value() == *plain);
}
