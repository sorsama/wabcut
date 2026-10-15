// Regression tests for the review-wave-4 findings on the transcript (docs/internal/reviews-wave4.md): each failed before its
// fix.

#include "CommandHarness.h"
#include "EditTestUtil.h"

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/edit/TextEdits.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;
using wabcut::ai::Transcript;
using command::Json;

namespace {

/// 24 frames of A on linked V1/A1; Hello [0,4) um [5,7) world [8,12) again [14,18).
constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
)";

std::shared_ptr<const Transcript> helloWorld() {
    return transcript("en", {{"Hello", 0, 4}, {"um", 5, 7}, {"world,", 8, 12}, {"again.", 14, 18}});
}

/// Remapping on for both clips, then a 4-frame hold of source frame 6 at timeline frame 6 (the clips keep their
/// length): the timeline shows 0..5, 6 held until 10, then 6.. from 10.
ProjectState heldAtSix(const Timeline& tl) {
    ProjectState s = tl.state;
    for (const char* ref : {"V1@0", "A1@0"}) {
        s = ok(edit::setTimeRemapEnabled(s, tl.seq, tl.clip(ref), true)).state;
    }
    for (const char* ref : {"V1@0", "A1@0"}) {
        s = ok(edit::addTimeRemapHold(s, tl.seq, tl.clip(ref), f(6), f(4))).state;
    }
    return s;
}

} // namespace

// <P1-7>
TEST_CASE("Review P1-7: words of a remapped clip map through its 100 % segments", "[ai][edit][mapping][review]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    const ProjectState held = heldAtSix(tl);

    // "um" straddles the hold: its middle (6) is heard after it; the words after it play 4 frames later.
    const auto st = SequenceTranscript::build(held, tl.seq, *p);
    CHECK(layout(st) == "Hello@0-4 um@10-11 world,@12-16 again.@18-22");

    // The incremental update follows a remap edit (the clip's window did not change).
    const auto on = ok(edit::setTimeRemapEnabled(tl.state, tl.seq, tl.clip("A1@0"), true));
    auto incremental = SequenceTranscript::build(on.state, tl.seq, *p);
    const auto hold = ok(edit::addTimeRemapHold(on.state, tl.seq, tl.clip("A1@0"), f(6), f(4)));
    incremental.update(hold.state, hold.changes, *p);
    CHECK(layout(incremental) == "Hello@0-4 um@10-11 world,@12-16 again.@18-22");

    // Deleting "world," cuts where it is heard now.
    const auto plan = planDelete(st, held, DeleteRequest{{{2, 2}}, {}, CutOptions{}});
    REQUIRE(plan.has_value());
    REQUIRE(plan->cuts.size() == 1);
    CHECK(plan->cuts[0].range == TimeRange::fromStartEnd(f(11), f(17)));
}

TEST_CASE("Review P1-7: a boomerang's reverse half is silent, its forward half heard again", "[ai][edit][mapping][review]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    ProjectState s = ok(edit::setTimeRemapEnabled(tl.state, tl.seq, tl.clip("A1@0"), true)).state;
    // From timeline 8: backwards from 8 to 5 for 3 frames (silent), then forwards at 100 % over 5..8 again (heard).
    s = ok(edit::addTimeRemapReverse(s, tl.seq, tl.clip("A1@0"), f(8), f(3))).state;
    const auto st = SequenceTranscript::build(s, tl.seq, *p);
    CHECK(layout(st) == "Hello@0-4 um@5-7 um@11-13 world,@14-18 again.@20-24");
}

TEST_CASE("Review P1-7: a remapped clip's source times map to the timeline through its curve", "[ai][analysis][review]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const ProjectState held = heldAtSix(tl);
    const Clip& clip = held.findSequence(tl.seq)->findClip(tl.clip("V1@0"))->clip();
    CHECK(ai::analysis::timelineTimeOfSource(clip, f(3)) == f(3));
    CHECK(ai::analysis::timelineTimeOfSource(clip, f(6)) == f(6)); // the hold's start
    CHECK(ai::analysis::timelineTimeOfSource(clip, f(12)) == f(16));
    CHECK(ai::analysis::timelineTimeOfSource(clip, f(19)) == f(23));
    CHECK(ai::analysis::timelineTimeOfSource(clip, f(21)) == std::nullopt); // past what it shows
}
// </P1-7>

// <P1-3>
TEST_CASE("Review P1-3: redacted speech reads as [bleep] in the transcript tools and captions",
          "[ai][edit][commands][captions][review]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    CommandHarness h(tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, std::make_shared<TranscriptServices>(p)},
                     editingCommands());
    // Bleep "world," (source 8..12) of A wherever it is used.
    const auto added = h.ok("redaction.add", Json{{"media", "A"},
                                                   {"source", Json{{"start", tc(0)}, {"end", tc(24)}}},
                                                   {"audio", "bleep"},
                                                   {"words", Json::array({Json{{"start", tc(8)}, {"end", tc(12)}}})}});
    const std::int64_t redaction = added.data["id"];

    const auto text = h.ok("transcript.text");
    CHECK(text.data["paragraphs"][0][5] == "Hello um [bleep] again.");
    const auto words = h.ok("transcript.words", Json{{"first", 2}, {"limit", 1}});
    CHECK(words.data["words"][0][1] == "[bleep]");

    for (const char* generate : {"captions.generate", "captions.generateFromTranscript"}) {
        INFO(generate);
        const auto made = h.ok(generate);
        const auto cues = h.ok("captions.get", Json{{"captionTrack", made.data["captionTrack"]}}).data["cues"];
        std::string all;
        for (const Json& cue : cues) {
            all += cue[2].get<std::string>() + " ";
        }
        CHECK(all.find("[bleep]") != std::string::npos);
        CHECK(all.find("world") == std::string::npos);
    }

    // A correction over the redacted word: what it was and what it says stay private.
    h.ok("transcript.correctWord", Json{{"word", 2}, {"text", "Worldwide"}});
    const Json row = h.ok("transcript.corrections").data["corrections"][0];
    CHECK(row[4] == "[bleep]");
    CHECK(row[5] == "[bleep]");
    CHECK(row[7] == "[bleep]");

    // A clip excepted from the redaction, or the redaction off: the words read again.
    h.ok("redaction.override", Json{{"redaction", redaction}, {"clip", h.clip("A1@0").value}});
    CHECK(h.ok("transcript.text").data["paragraphs"][0][5] == "Hello um Worldwide again.");
    h.ok("redaction.override", Json{{"redaction", redaction}, {"clip", h.clip("A1@0").value}, {"redact", true}});
    h.ok("redaction.update", Json{{"redaction", redaction}, {"enabled", false}});
    CHECK(h.ok("transcript.text").data["paragraphs"][0][5] == "Hello um Worldwide again.");
}

TEST_CASE("Review P1-3: a redaction added after mapping masks the words in place (incremental update)",
          "[ai][edit][mapping][review]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    Redaction r;
    r.media = tl.media('A');
    r.name = "Name";
    r.source = TimeRange::fromStartEnd(f(0), f(24));
    r.audio = RedactionAudio::Mute;
    r.audioRanges = {TimeRange::fromStartEnd(f(1), f(2))}; // inside "Hello"
    const auto added = edit::addRedaction(tl.state, r);
    REQUIRE(added.has_value());
    st.update(added->state, diff(tl.state, added->state), *p);
    CHECK(st.textOf(0, st.size() - 1) == "[bleep] um world, again.");
}
// </P1-3>

// <P1-3-follow-up>
TEST_CASE("Review P1-3 follow-up: captions generated before a redaction read [bleep] after it, and back after it goes",
          "[ai][edit][commands][captions][review]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const auto p = provider(tl, {{'A', helloWorld()}});
    CommandHarness h(tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, std::make_shared<TranscriptServices>(p)},
                     editingCommands());
    const std::uint64_t track = h.ok("captions.generate").data["captionTrack"];
    // The stored cues (what the refresh writes); captions.get masks what leaves by time range (wave 5, S5-P1-2).
    const auto cueText = [&] {
        std::string all;
        for (const CaptionCue& cue : h.sequence(h.state()).captionTracks.front().cues) {
            all += cue.text + " ";
        }
        return all;
    };
    REQUIRE(cueText().find("world") != std::string::npos);
    const std::size_t undo = h.session.undoCount();
    const std::int64_t redaction = h.ok("redaction.add", Json{{"media", "A"},
                                                             {"source", Json{{"start", tc(0)}, {"end", tc(24)}}},
                                                             {"audio", "bleep"},
                                                             {"words", Json::array({Json{{"start", tc(8)}, {"end", tc(12)}}})}})
                                       .data["id"];
    CHECK(h.session.undoCount() == undo + 1); // one step: the redaction and the cues
    CHECK(cueText().find("world") == std::string::npos);
    CHECK(cueText().find("[bleep]") != std::string::npos);
    CHECK(cueText().find("Hello") != std::string::npos);
    const Json read = h.ok("captions.get", Json{{"captionTrack", track}}).data;
    CHECK(read.dump().find("world") == std::string::npos);
    CHECK(read.contains("redactedCues"));
    h.ok("redaction.remove", Json{{"redaction", redaction}});
    CHECK(cueText().find("world") != std::string::npos);
    h.session.undo();
    CHECK(cueText().find("[bleep]") != std::string::npos);
}
// </P1-3-follow-up>
