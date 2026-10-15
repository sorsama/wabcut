// Text edits -> timeline edits: deleting, keeping and moving words (AI-3).

#include "EditTestUtil.h"

#include <wabcut/ai/edit/TextEdits.h>
#include <wabcut/ai/edit/TranscriptCommands.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;
using wabcut::ai::Transcript;

namespace {

/// 24 frames of A on linked V1/A1; Hello [0,4) um [5,7) world [8,12) again [14,18).
constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
)";

std::shared_ptr<const Transcript> helloWorld() {
    return transcript("en", {{"Hello", 0, 4}, {"um", 5, 7}, {"world,", 8, 12}, {"again.", 14, 18}});
}

ProjectState apply(const ProjectState& state, const command::CommandBatch& batch) {
    return runInline(editingCommands(), state, batch);
}

EditPlan planned(std::expected<EditPlan, Error> plan) {
    INFO((plan ? std::string() : plan.error().message));
    REQUIRE(plan.has_value());
    return std::move(*plan);
}

DeleteRequest deleting(std::vector<WordRange> words, std::vector<std::size_t> gaps = {}, CutOptions options = {}) {
    return DeleteRequest{std::move(words), std::move(gaps), options};
}

} // namespace

TEST_CASE("Deleting a word ripple-deletes it on frame boundaries just outside it", "[ai][edit][delete]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);

    const EditPlan plan = planned(planDelete(st, tl.state, deleting({{1, 1}})));
    REQUIRE(plan.cuts.size() == 1);
    CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(4), f(8))); // 40 ms each side, up to the neighbours
    CHECK(plan.cuts[0].tracks == std::vector<TrackId>{tl.track("V1"), tl.track("A1")}); // the linked video too
    CHECK(plan.words == 1);
    CHECK(plan.removed() == f(4));

    const command::CommandBatch batch = toCommandBatch(plan);
    REQUIRE(batch.commands.size() == 1);
    CHECK(batch.commands[0].command == "sequence.extract");
    const ProjectState after = apply(tl.state, batch);
    requireValid(after);
    CHECK_TIMELINE(after, tl.seq, R"(
        V1 |AaaaA8aaaaaaaaaaaaaa|
        A1 |AaaaA8aaaaaaaaaaaaaa|
    )");
    CHECK(links(after, tl.seq) == "V1@0+A1@0 V1@4+A1@4");
    CHECK(SequenceTranscript::build(after, tl.seq, *p).textOf(0, 2) == "Hello world, again.");
}

TEST_CASE("Padding never reaches into the neighbouring words", "[ai][edit][delete]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    // um sits between words that end and start inside frames 4 and 7.
    const auto p = provider(tl, {{'A', transcript("en", {{"Hello", 0, 4.5}, {"um", 5.2, 6.7}, {"world", 7.4, 12}})}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);

    SECTION("Default 40 ms: the frames holding the neighbours stay") {
        const EditPlan plan = planned(planDelete(st, tl.state, deleting({{1, 1}})));
        REQUIRE(plan.cuts.size() == 1);
        CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(5), f(7)));
    }
    SECTION("No padding: the frames the word touches") {
        const EditPlan plan = planned(planDelete(st, tl.state, deleting({{1, 1}}, {}, CutOptions{Ticks{}, Ticks{}})));
        REQUIRE(plan.cuts.size() == 1);
        CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(5), f(7)));
    }
    SECTION("The first word may not reach before 0") {
        const EditPlan plan = planned(planDelete(st, tl.state, deleting({{0, 0}})));
        REQUIRE(plan.cuts.size() == 1);
        CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(0), f(5)));
    }
}

TEST_CASE("Neighbouring deletions merge into one cut", "[ai][edit][delete]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);

    SECTION("Adjacent words") {
        const EditPlan plan = planned(planDelete(st, tl.state, deleting({{2, 2}, {1, 1}})));
        REQUIRE(plan.cuts.size() == 1);
        CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(4), f(13)));
        CHECK(plan.words == 2);
        CHECK_TIMELINE(apply(tl.state, toCommandBatch(plan)), tl.seq, R"(
            V1 |AaaaA13aaaaaaaa|
            A1 |AaaaA13aaaaaaaa|
        )");
    }
    SECTION("Words and the pause after them") {
        const EditPlan plan = planned(planDelete(st, tl.state, deleting({{1, 2}}, {2}, CutOptions{f(1), f(1), Ticks{}})));
        REQUIRE(plan.cuts.size() == 1);
        CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(4), f(14)));
    }
    SECTION("A kept word between two deletions keeps them apart") {
        const EditPlan plan = planned(planDelete(st, tl.state, deleting({{1, 1}, {3, 3}})));
        REQUIRE(plan.cuts.size() == 2);
        CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(4), f(8)));
        CHECK(plan.cuts[1].range == TimeRange::fromStartEnd(f(13), f(19)));
        const command::CommandBatch batch = toCommandBatch(plan);
        REQUIRE(batch.commands.size() == 2);
        CHECK(batch.commands[0].params["in"] == f(13).value); // last cut first
        CHECK_TIMELINE(apply(tl.state, batch), tl.seq, R"(
            V1 |AaaaA8aaaA19aa|
            A1 |AaaaA8aaaA19aa|
        )");
    }
}

TEST_CASE("Removing a pause keeps room tone next to the words", "[ai][edit][delete]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', transcript("en", {{"one", 0, 3}, {"two", 12, 15}})}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    // The 9-frame pause [3, 12) keeps 150 ms (3.75 frames, rounded inward to whole frames) at each side.
    const EditPlan plan = planned(planDelete(st, tl.state, deleting({}, {0})));
    REQUIRE(plan.cuts.size() == 1);
    CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(7), f(8)));
    CHECK(plan.words == 0);
    CHECK(!planDelete(st, tl.state, deleting({}, {1})).has_value()); // no pause after the last word
}

TEST_CASE("Deletions follow sync locks and respect locked tracks", "[ai][edit][delete]") {
    SECTION("Linked video is cut even without sync lock; sync-locked tracks ripple, others stay") {
        Timeline tl(R"(
            V1 nosync |Aaaaaaaaaaaaaaaaaaaaaaaa|
            A1        |Aaaaaaaaaaaaaaaaaaaaaaaa|
            A2        |Mmmmmmmmmmmmmmmmmmmmmmmm|
            A3 nosync |Nnnnnnnnnnnnnnnnnnnnnnnn|
        )");
        tl.link("V1@0 A1@0");
        const auto p = provider(tl, {{'A', helloWorld()}});
        const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
        const EditPlan plan = planned(planDelete(st, tl.state, deleting({{1, 1}})));
        CHECK_TIMELINE(apply(tl.state, toCommandBatch(plan)), tl.seq, R"(
            V1 |AaaaA8aaaaaaaaaaaaaa....|
            A1 |AaaaA8aaaaaaaaaaaaaa....|
            A2 |MmmmM8mmmmmmmmmmmmmm....|
            A3 |Nnnnnnnnnnnnnnnnnnnnnnnn|
        )");
    }
    SECTION("Words heard on a locked track cannot be deleted") {
        Timeline tl(R"(
            V1      |Aaaaaaaaaaaaaaaaaaaaaaaa|
            A1 lock |Aaaaaaaaaaaaaaaaaaaaaaaa|
        )");
        const auto p = provider(tl, {{'A', helloWorld()}});
        const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
        const auto plan = planDelete(st, tl.state, deleting({{1, 1}}));
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error().message.find("locked") != std::string::npos);
    }
    SECTION("Bad word ranges are refused") {
        Timeline tl(kPicture);
        const auto p = provider(tl, {{'A', helloWorld()}});
        const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
        CHECK(!planDelete(st, tl.state, deleting({{2, 1}})).has_value());
        CHECK(!planDelete(st, tl.state, deleting({{3, 4}})).has_value());
    }
}

TEST_CASE("Keep only these ranges: a rough cut", "[ai][edit][keep]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    const std::vector<WordRange> keep{{3, 3}, {0, 0}};
    const EditPlan plan = planned(planKeep(st, tl.state, keep));
    REQUIRE(plan.cuts.size() == 2);
    CHECK(plan.cuts[0].range == TimeRange::fromStartEnd(f(5), f(13)));
    CHECK(plan.cuts[1].range == TimeRange::fromStartEnd(f(19), f(24)));
    CHECK(plan.words == 2);
    const ProjectState after = apply(tl.state, toCommandBatch(plan, "Keep Words"));
    CHECK_TIMELINE(after, tl.seq, R"(
        V1 |AaaaaA13aaa|
        A1 |AaaaaA13aaa|
    )");
    CHECK(SequenceTranscript::build(after, tl.seq, *p).textOf(0, 1) == "Hello again.");
    CHECK(!planKeep(st, tl.state, std::vector<WordRange>{}).has_value());
}

TEST_CASE("Moving words to another word boundary", "[ai][edit][move]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);

    SECTION("Earlier") {
        const auto plan = planMove(st, tl.state, WordRange{3, 3}, 1);
        REQUIRE(plan.has_value());
        CHECK(plan->source == TimeRange::fromStartEnd(f(13), f(19)));
        CHECK(plan->destination == f(4));
        const ProjectState after = apply(tl.state, toCommandBatch(*plan, tl.state));
        requireValid(after);
        CHECK_TIMELINE(after, tl.seq, R"(
            V1 |AaaaA13aaaA4aaaaaaaA19aa|
            A1 |AaaaA13aaaA4aaaaaaaA19aa|
        )");
        CHECK(SequenceTranscript::build(after, tl.seq, *p).textOf(0, 3) == "Hello again. um world,");
    }
    SECTION("Later") {
        const auto plan = planMove(st, tl.state, WordRange{0, 0}, 4);
        REQUIRE(plan.has_value());
        CHECK(plan->source == TimeRange::fromStartEnd(f(0), f(5)));
        CHECK(plan->destination == f(19));
        const ProjectState after = apply(tl.state, toCommandBatch(*plan, tl.state));
        requireValid(after);
        CHECK_TIMELINE(after, tl.seq, R"(
            V1 |A5aaaaaaaaaaaaAaaaaA19aa|
            A1 |A5aaaaaaaaaaaaAaaaaA19aa|
        )");
        CHECK(SequenceTranscript::build(after, tl.seq, *p).textOf(0, 3) == "um world, again. Hello");
    }
    SECTION("Not into itself") {
        CHECK(!planMove(st, tl.state, WordRange{1, 2}, 2).has_value());
        CHECK(!planMove(st, tl.state, WordRange{1, 2}, 3).has_value());
        CHECK(!planMove(st, tl.state, WordRange{1, 2}, 5).has_value());
    }
}
