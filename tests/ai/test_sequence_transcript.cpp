// SequenceTranscript: transcripts of media mapped onto a sequence through its audio clips (AI-3).

#include "EditTestUtil.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>

#include <wabcut/model/Nesting.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;

TEST_CASE("Words appear through the clip windows, in timeline order", "[ai][edit][mapping]") {
    Timeline tl(R"(
        V1 |A10aaaaaaa..B5bbb|
        A1 |A10aaaaaaa..B5bbb|
    )");
    // A1@0 plays A [10, 20), A1@12 plays B [5, 10). A word counts where its midpoint is.
    const auto p = provider(tl, {{'A', transcript("en", {{"zero", 2, 5},     // before the In: trimmed away
                                                         {"edge", 9, 12},    // midpoint 10.5: in, clamped to the clip
                                                         {"one", 12, 14},
                                                         {"two", 15, 18},
                                                         {"cut", 19, 22}})}, // midpoint 20.5: past the Out
                                 {'B', transcript("en", {{"bee", 0, 3}, {"sea", 6, 9}, {"dee", 9, 12}})}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    CHECK(layout(st) == "edge@0-2 one@2-4 two@5-8 sea@13-16");
    CHECK(st[0].clip == tl.clip("A1@0"));
    CHECK(st[3].clip == tl.clip("A1@12"));
    CHECK(st[3].source == TimeRange::fromStartEnd(f(6), f(9)));
    CHECK(st.textOf(0, 3) == "edge one two sea");
    CHECK(st.extent() == TimeRange::fromStartEnd(f(0), f(17)));
}

TEST_CASE("Speed and reverse map source words through the clip's time map", "[ai][edit][mapping]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaa|
    )");
    const auto p = provider(tl, {{'A', transcript("en", {{"one", 1, 3}, {"two", 4, 8}})}});
    CHECK(layout(SequenceTranscript::build(tl.state, tl.seq, *p)) == "one@1-3 two@4-8");

    SECTION("200% plays the same words in half the time") {
        const ClipId clip = tl.clip("A1@0");
        const auto fast = ok(edit::setSpeed(tl.state, tl.seq, std::span(&clip, 1), edit::SpeedChange{Speed{2, 1}}));
        CHECK(layout(SequenceTranscript::build(fast.state, tl.seq, *p)) == "one@0.50-1.50 two@2-4");
    }
    SECTION("50% stretches them") {
        const ClipId clip = tl.clip("A1@0");
        const auto slow = ok(edit::setSpeed(tl.state, tl.seq, std::span(&clip, 1), edit::SpeedChange{Speed{1, 2}}));
        CHECK(layout(SequenceTranscript::build(slow.state, tl.seq, *p)) == "one@2-6 two@8-16");
    }
    SECTION("Reversed: the last words play first") {
        const ClipId clip = tl.clip("A1@0");
        edit::SpeedChange change{Speed{1, 1}};
        change.reversed = true;
        const auto rev = ok(edit::setSpeed(tl.state, tl.seq, std::span(&clip, 1), change));
        CHECK(layout(SequenceTranscript::build(rev.state, tl.seq, *p)) == "two@2-6 one@7-9");
    }
}

TEST_CASE("Linked A/V: words come from the audio clip, selections map to the link group", "[ai][edit][mapping]") {
    Timeline tl(R"(
        V1 |Aaaaaaaaaa|
        A1 |Aaaaaaaaaa|
        A2 |Aaaaaaaaaa|
    )");
    const auto p = provider(tl, {{'A', transcript("en", {{"hello", 1, 4}, {"there", 5, 8}})}});

    SECTION("A stereo pair in one link group is heard once") {
        tl.linkMatching();
        const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
        CHECK(layout(st) == "hello@1-4 there@5-8");
        CHECK(st[0].track == tl.track("A1"));
        CHECK(st[0].linkGroup.isValid());
        const auto clips = st.clipsFor(0, 1, tl.sequence());
        CHECK(clips.size() == 3); // V1, A1 and A2: the whole link group
        CHECK(std::ranges::find(clips, tl.clip("V1@0")) != clips.end());
    }
    SECTION("Unlinked copies are separate clips, each heard") {
        const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
        CHECK(layout(st) == "hello@1-4 hello@1-4 there@5-8 there@5-8");
        CHECK(st[0].track == tl.track("A1"));
        CHECK(st[1].track == tl.track("A2"));
        CHECK(st.clipsFor(0, 0, tl.sequence()) == std::vector<ClipId>{tl.clip("A1@0")});
    }
}

TEST_CASE("Media without a transcript and video-only clips contribute nothing", "[ai][edit][mapping]") {
    Timeline tl(R"(
        V1 |Ccccc|
        A1 |AaaaaBbbbb|
    )");
    const auto p = provider(tl, {{'B', transcript("en", {{"bee", 1, 3}})}, {'C', transcript("en", {{"sea", 1, 3}})}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    CHECK(layout(st) == "bee@6-8");
}

TEST_CASE("Word lookup by time", "[ai][edit][mapping]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaaaaaaaaaaaa|
    )");
    const auto p = provider(tl, {{'A', transcript("en", {{"a", 1, 3}, {"b", 5, 8}, {"c", 12, 14}})}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    CHECK(st.wordsIn(TimeRange::fromStartEnd(f(2), f(6))) == std::pair<std::size_t, std::size_t>{0, 2});
    CHECK(st.wordsIn(TimeRange::fromStartEnd(f(8), f(12))) == std::pair<std::size_t, std::size_t>{2, 2});
    CHECK(st.wordAt(f(6)) == 1u);
    CHECK(st.wordAt(f(9)) == 1u);  // nearer to b than to c
    CHECK(st.wordAt(f(11)) == 2u);
    CHECK(st.wordAt(f(40)) == 2u);
}

TEST_CASE("Thai words join without spaces", "[ai][edit][mapping]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaaaaaaaaaaaa|
    )");
    const auto p = provider(tl, {{'A', transcript("th", {{"สวัสดี", 1, 4}, {"ครับ", 4, 6}, {"Wabcut", 7, 10}, {"ดี", 10, 12}})}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    CHECK(st.language(0) == "th");
    CHECK(st.textOf(0, 3) == "สวัสดีครับWabcutดี");
}

TEST_CASE("Incremental update equals a full rebuild", "[ai][edit][mapping]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb..Cccccccccc|
        A1 tgt |AaaaaaaaaaBbbbbbbbbb..Cccccccccc|
        A2     |Dddddddddddddddddddddddddddddddd|
    )");
    tl.linkMatching();
    const auto words = [](std::string prefix) {
        std::vector<W> out;
        for (int i = 0; i < 40; ++i) {
            out.push_back(W{prefix + std::to_string(i), i * 3.0, i * 3.0 + 2.0});
        }
        return out;
    };
    const auto p = provider(tl, {{'A', transcript("en", words("a"))},
                                 {'B', transcript("en", words("b"))},
                                 {'C', transcript("en", words("c"))},
                                 {'D', transcript("en", words("d"))}});
    const ClipId a = tl.clip("A1@0");
    const ClipId b = tl.clip("A1@10");
    const ClipId c = tl.clip("A1@22");
    const TrackId v1 = tl.track("V1");
    const TrackId a1 = tl.track("A1");

    struct Case {
        const char* name;
        edit::EditOutcome outcome;
    };
    const ClipId bc[] = {b};
    const ClipId cc[] = {c};
    const TrackId both[] = {v1, a1};
    std::vector<Case> cases;
    cases.push_back({"ripple delete", edit::rippleDeleteClips(tl.state, tl.seq, std::span(bc))});
    cases.push_back({"trim", edit::trimEdge(tl.state, tl.seq, a, edit::Edge::Tail, f(-3))});
    cases.push_back({"ripple trim", edit::rippleTrim(tl.state, tl.seq, a, edit::Edge::Head, f(2))});
    cases.push_back({"move", edit::moveClips(tl.state, tl.seq, std::span(cc), f(3))});
    cases.push_back({"razor", edit::addEdit(tl.state, tl.seq, f(15), std::span(both))});
    cases.push_back({"speed", edit::setSpeed(tl.state, tl.seq, std::span(bc), edit::SpeedChange{Speed{2, 1}, {}, {}, true})});
    cases.push_back({"unlink", edit::unlinkClips(tl.state, tl.seq, std::span(bc))});
    for (Case& k : cases) {
        INFO(k.name);
        const auto result = ok(std::move(k.outcome));
        SequenceTranscript st = SequenceTranscript::build(tl.state, tl.seq, *p);
        st.update(result.state, diff(tl.state, result.state), *p);
        const auto full = SequenceTranscript::build(result.state, tl.seq, *p);
        INFO(layout(st));
        INFO(layout(full));
        CHECK(sameWords(st, full));
    }
}

TEST_CASE("A 1-hour transcript maps in under 50 ms and updates in under 5 ms", "[ai][edit][mapping][perf]") {
    // 10,000 words of 6 frames every 9 frames = 90,000 frames = 1 hour at 25 fps, heard through 200 linked A/V
    // clips (through edits every 450 frames).
    std::string video = "V1 tgt |";
    std::string audio = "A1 tgt |";
    for (int k = 0; k < 200; ++k) {
        const std::string in = k == 0 ? std::string() : std::to_string(k * 450);
        const std::string clip = "A" + in + std::string(450 - 1 - in.size(), 'a');
        video += clip;
        audio += clip;
    }
    Timeline tl(video + "|\n" + audio + "|\n", {{'A', 100000}});
    tl.linkMatching();
    std::vector<W> words;
    for (int i = 0; i < 10000; ++i) {
        words.push_back(W{"w" + std::to_string(i), i * 9.0, i * 9.0 + 6.0});
    }
    const auto p = provider(tl, {{'A', transcript("en", std::move(words))}});

    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    double best = 1e9;
    SequenceTranscript st;
    for (int run = 0; run < 3; ++run) {
        const auto t0 = Clock::now();
        st = SequenceTranscript::build(tl.state, tl.seq, *p);
        best = std::min(best, ms(Clock::now() - t0));
    }
    REQUIRE(st.size() == 10000);

    // A ripple delete in the middle: 100 clips shift, one goes.
    const ClipId middle[] = {tl.clip("A1@45000")};
    const auto edited = ok(edit::rippleDeleteClips(tl.state, tl.seq, std::span(middle)));
    const ChangeSet changes = diff(tl.state, edited.state);
    double bestUpdate = 1e9;
    double bestTrim = 1e9;
    const ClipId tail = tl.clip("A1@44550");
    const auto trimmed = ok(edit::trimEdge(tl.state, tl.seq, tail, edit::Edge::Tail, f(-20)));
    const ChangeSet trimChanges = diff(tl.state, trimmed.state);
    for (int run = 0; run < 3; ++run) {
        SequenceTranscript copy = st;
        auto t0 = Clock::now();
        copy.update(edited.state, changes, *p);
        bestUpdate = std::min(bestUpdate, ms(Clock::now() - t0));
        REQUIRE(copy.size() == 10000 - 50);

        SequenceTranscript copy2 = st;
        t0 = Clock::now();
        copy2.update(trimmed.state, trimChanges, *p);
        bestTrim = std::min(bestTrim, ms(Clock::now() - t0));
    }
    std::printf("[perf] SequenceTranscript: build 10,000 words / 200 clips %.2f ms; update after a ripple delete "
                "(100 clips shift) %.3f ms; after a trim %.3f ms\n",
                best, bestUpdate, bestTrim);
    CHECK(best < 50.0);
    CHECK(bestUpdate < 5.0);
    CHECK(bestTrim < 5.0);
}

// ---- C.17: nested and multicam clips read through ----------------------------------------------------------------------

TEST_CASE("Words of a nested clip read through with the times they had before nesting (C.17)", "[ai][edit][mapping][nested]") {
    Timeline tl(R"(
        V1 |A10aaaaaaaaaaaaaaaaa..........|
        A1 |A10aaaaaaaaaaaaaaaaa..........|
    )");
    tl.linkMatching();
    const auto p = provider(tl, {{'A', transcript("en", {{"so", 11, 13}, {"we", 14, 16}, {"began", 18, 24}, {"late", 31, 34}})}});
    const auto before = SequenceTranscript::build(tl.state, tl.seq, *p);
    REQUIRE(layout(before) == "so@1-3 we@4-6 began@8-14");
    const std::vector<ClipId> clips{tl.clip("V1@0"), tl.clip("A1@0")};
    const auto nested = edit::nestClips(tl.state, tl.seq, clips, "Interview");
    REQUIRE(nested.has_value());
    const ProjectState& s = nested->edit.state;
    const auto after = SequenceTranscript::build(s, tl.seq, *p);
    CHECK(layout(after) == layout(before));
    REQUIRE(after.size() == 3);
    const ClipId outerAudio = clipAt(s, tl.seq, "A1@0");
    CHECK(s.findSequence(tl.seq)->findClip(outerAudio)->clip().isNested());
    CHECK(after[0].clip == outerAudio);
    CHECK(after[0].source == before[0].source); // the leaf media's word
    CHECK(after.wordAt(f(9)) == std::optional<std::size_t>(2)); // click-to-seek finds it
    CHECK(after.clipsFor(0, 2, *s.findSequence(tl.seq)).size() == 2); // the nested video and audio clips

    // Moved later: its words follow (update() from the ChangeSet equals a fresh build).
    const std::vector<ClipId> both{clipAt(s, tl.seq, "V1@0"), outerAudio};
    const auto moved = ok(edit::moveClips(s, tl.seq, both, f(5)));
    SequenceTranscript live = after;
    live.update(moved.state, moved.changes, *p);
    CHECK(layout(live) == "so@6-8 we@9-11 began@13-19");
    CHECK(sameWords(live, SequenceTranscript::build(moved.state, tl.seq, *p)));

    // An edit inside the nested sequence shows in the outer transcript (update() rebuilds).
    const ClipId innerAudio = s.findSequence(nested->nested)->audioTracks[0]->clips[0].id;
    const auto trimmed = ok(edit::trimEdge(moved.state, nested->nested, innerAudio, edit::Edge::Head, f(5), edit::LinkMode::IgnoreLinks));
    live.update(trimmed.state, trimmed.changes, *p);
    CHECK(sameWords(live, SequenceTranscript::build(trimmed.state, tl.seq, *p)));
    CHECK(layout(live).find("so@") == std::string::npos); // trimmed away inside the nest
}

TEST_CASE("Words of a multicam clip come from its active audio angle (C.17)", "[ai][edit][mapping][multicam]") {
    Timeline tl(R"(
        V1 |Aaaaa...........................................|
        A1 |Bbbbb...........................................|
    )");
    const auto p = provider(tl, {{'A', transcript("en", {{"alpha", 2, 5}, {"bravo", 12, 15}})},
                                 {'B', transcript("en", {{"one", 0, 3}, {"two", 10, 14}})}});
    edit::MulticamSpec spec;
    spec.angles = {tl.media('A'), tl.media('B')};
    spec.sync = MulticamSync::Manual;
    spec.offsets = {Ticks{}, f(5)}; // B starts 5 frames into the multicam source
    const auto mc = edit::createMulticam(tl.state, spec);
    REQUIRE(mc.has_value());
    edit::SourceClip src;
    src.sequence = mc->id;
    src.sourceRange = TimeRange::fromStartEnd(f(10), f(30));
    src.patches = edit::defaultSourcePatches(*mc->state.findSequence(tl.seq), *mc->state.findSequence(mc->id));
    src.videoAngle = AngleSet{1};
    src.audioAngles = AngleSet{1};
    const auto placed = ok(edit::overwriteEdit(mc->state, tl.seq, src, f(20)));
    const ClipId audio = clipAt(placed.state, tl.seq, "A1@20");
    // A1@0 (B) gives "one"; the multicam clip [20, 40) plays the source [10, 30) with angle A's sound: "bravo".
    const auto st = SequenceTranscript::build(placed.state, tl.seq, *p);
    CHECK(layout(st) == "one@0-3 bravo@22-25");
    CHECK(st[1].clip == audio);
    // The sound switched to angle B (5 frames later in the source): its "two" at source 10-14 plays at 25-29.
    const auto switched = ok(edit::switchMulticamAngle(placed.state, tl.seq, audio, std::nullopt, 1, edit::AngleTarget::Audio));
    SequenceTranscript live = st;
    live.update(switched.state, switched.changes, *p);
    CHECK(layout(live) == "one@0-3 two@25-29");
    CHECK(sameWords(live, SequenceTranscript::build(switched.state, tl.seq, *p)));
}
