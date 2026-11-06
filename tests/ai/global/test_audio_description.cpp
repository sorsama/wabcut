// 0.7 (card 175) audio description: the gaps in a sequence's dialogue that a line fits in (margins, minimum, a long silence
// cut in pieces, the dialogue around each gap), the fit of a model's line to its gap (words, sentence end, NONE), the
// request a vision model gets and a run over the gaps with a scripted model (pictures sent, lines kept in order, a gap with
// no picture or nothing to say left silent, a refusal ends the run), and audioDescription.findGaps / createTrack through
// the engine (a disabled "Audio Description" caption track in one undo step, replaced by the next run, ready for dub.create).

#include "CommandHarness.h"
#include "EditTestUtil.h"

#include <wabcut/ai/dub/AudioDescription.h>
#include <wabcut/ai/dub/DubCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::dub;
using namespace wabcut::ai::edit::test;
using command::Json;

namespace {

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }
double secondsOf(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }

/// A 16 s sequence at 25 fps whose dialogue is "Hello there" (4 s to 6 s) and "Welcome back" (10 s to 12 s).
std::string picture() { return "V1 tgt |A" + std::string(399, 'a') + "|\nA1 tgt dialogue |A" + std::string(399, 'a') + "|"; }

std::shared_ptr<const Transcript> dialogue() {
    return transcript("en", {W{"Hello", 100, 120}, W{"there", 125, 150}, W{"Welcome", 250, 275}, W{"back", 278, 300}});
}

struct Studio {
    Studio() : tl(picture()) {
        tl.linkMatching();
        transcripts = provider(tl, {{'A', dialogue()}});
    }
    std::unique_ptr<CommandHarness> harness(bool withTranscripts = true) {
        auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            withTranscripts ? std::make_shared<ai::edit::TranscriptServices>(transcripts)
                            : std::make_shared<ai::edit::TranscriptServices>(std::make_shared<ai::edit::MapTranscriptProvider>())});
        return std::make_unique<CommandHarness>(tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, services},
                                                ai::edit::editingCommands());
    }
    Timeline tl;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
};

/// A chat model that answers from a script and records what it was asked.
class ScriptedChat final : public IChatModel {
public:
    explicit ScriptedChat(std::vector<std::string> answers) : m_answers(std::move(answers)) { m_spec.id = "scripted-vision"; }
    const ModelSpec& spec() const override { return m_spec; }
    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler&, std::stop_token) override {
        requests.push_back(request);
        if (failAt && requests.size() == *failAt) {
            return std::unexpected(AiError(AiErrorCode::ConsentRequired, "no consent recorded"));
        }
        ChatResponse response;
        response.message = ChatMessage::assistant(m_next < m_answers.size() ? m_answers[m_next] : std::string("NONE"));
        ++m_next;
        response.usage.inputTokens = 100;
        response.usage.outputTokens = 10;
        return response;
    }
    std::vector<ChatRequest> requests;
    std::optional<std::size_t> failAt;

private:
    ModelSpec m_spec;
    std::vector<std::string> m_answers;
    std::size_t m_next = 0;
};

ImagePart picture(std::uint8_t tag) {
    ImagePart image;
    image.mediaType = "image/jpeg";
    image.bytes = {tag, 1, 2, 3};
    return image;
}

} // namespace

TEST_CASE("Audio description: how many words fit a silence and how a model's line is fitted to it", "[ai][global][ad]") {
    CHECK(wordBudget(sec(4)) == 10);   // 2.5 words a second
    CHECK(wordBudget(sec(0.2)) == 1);  // never less than one
    CHECK(wordBudget(sec(60)) == 40);  // never more than the cap
    AdOptions fast;
    fast.wordsPerSecond = 3.0;
    CHECK(wordBudget(sec(4), fast) == 12);

    CHECK(fitDescription("A woman opens a red door", 10) == "A woman opens a red door.");
    CHECK(fitDescription("  A woman\n opens   a red door!  ", 10) == "A woman opens a red door!");
    CHECK(fitDescription("\"A woman opens a red door.\"", 10) == "A woman opens a red door.");
    CHECK(fitDescription("- A man waves.", 10) == "A man waves.");
    // Over the limit: cut at the last sentence end that fits.
    CHECK(fitDescription("A woman opens a red door. She steps into a dark hall and looks around slowly.", 6) == "A woman opens a red door.");
    // No sentence end: cut at the word, never after a word that cannot end a line.
    CHECK(fitDescription("A woman steps into the dark hall and looks around", 6) == "A woman steps into the dark.");
    CHECK(fitDescription("A woman walks to the", 4) == "A woman walks.");
    // Nothing worth saying.
    CHECK(fitDescription("NONE", 10).empty());
    CHECK(fitDescription(" none. ", 10).empty());
    CHECK(fitDescription("", 10).empty());
    CHECK(fitDescription("   \n  ", 10).empty());
    CHECK(wordCount("A woman opens  a red door.") == 6);
}

TEST_CASE("Audio description: gaps between speech keep their margins, need the minimum, and a long silence is cut in pieces",
          "[ai][global][ad]") {
    const TimeRange speech[] = {{sec(4), sec(2)}, {sec(10), sec(2)}};
    const auto gaps = findGaps(speech, sec(16));
    REQUIRE(gaps.size() == 3);
    // The lead-in: no margin at the start of the sequence, 0.3 s kept clear of the speech.
    CHECK(secondsOf(gaps[0].range.start) == Catch::Approx(0.0));
    CHECK(secondsOf(gaps[0].range.end()) == Catch::Approx(3.7));
    CHECK(gaps[0].maxWords == 9);
    // Between the speech: a margin on both sides.
    CHECK(secondsOf(gaps[1].range.start) == Catch::Approx(6.3));
    CHECK(secondsOf(gaps[1].range.end()) == Catch::Approx(9.7));
    CHECK(gaps[1].maxWords == 8);
    // The tail: a margin at the speech only.
    CHECK(secondsOf(gaps[2].range.start) == Catch::Approx(12.3));
    CHECK(secondsOf(gaps[2].range.end()) == Catch::Approx(16.0));

    // A silence shorter than the minimum after the margins gets nothing.
    const TimeRange tight[] = {{sec(0), sec(4)}, {sec(6), sec(2)}};
    CHECK(findGaps(tight, sec(10)).empty()); // 2 s between, 1.4 s usable; 2 s at the end, 1.7 s usable
    AdOptions short2;
    short2.minGap = sec(1);
    const auto shorter = findGaps(tight, sec(10), short2);
    REQUIRE(shorter.size() == 2);
    CHECK(secondsOf(shorter[0].range.duration) == Catch::Approx(1.4));

    // A long silence is cut into pieces no longer than maxGap; a sequence without speech is one long silence.
    const auto silent = findGaps(std::span<const TimeRange>{}, sec(40));
    REQUIRE(silent.size() == 3); // 40 s / 15 s
    for (const AdGap& gap : silent) {
        CHECK(secondsOf(gap.range.duration) <= 15.0);
        CHECK(gap.range.duration >= AdOptions{}.minGap);
    }
    CHECK(silent.front().range.start == Ticks{});
    CHECK(silent.back().range.end() == sec(40));
    CHECK(findGaps(std::span<const TimeRange>{}, sec(1)).empty());
}

TEST_CASE("Audio description: the gaps of a sequence come from its transcript, with the dialogue around them", "[ai][global][ad]") {
    Studio studio;
    const auto words = ai::edit::SequenceTranscript::build(studio.tl.state, studio.tl.seq, *studio.transcripts);
    REQUIRE(words.size() == 4);
    const auto speech = speechRanges(words);
    REQUIRE(speech.size() == 2); // "Hello there" and "Welcome back": the pause inside a phrase is not a gap
    CHECK(secondsOf(speech[0].start) == Catch::Approx(4.0));
    CHECK(secondsOf(speech[0].end()) == Catch::Approx(6.0));
    const auto gaps = findGaps(words, sec(16));
    REQUIRE(gaps.size() == 3);
    CHECK(gaps[0].before.empty());
    CHECK(gaps[0].after == "Hello there Welcome back");
    CHECK(gaps[1].before == "Hello there");
    CHECK(gaps[1].after == "Welcome back");
    CHECK(gaps[2].before == "Hello there Welcome back");
    CHECK(gaps[2].after.empty());
    AdOptions wide;
    wide.joinGap = sec(0.01); // words 0.2 s apart now are separate
    CHECK(speechRanges(words, wide).size() == 4);
}

TEST_CASE("Audio description: the request names the pictures, the pace and the dialogue, and asks for NONE when nothing matters",
          "[ai][global][ad]") {
    AdGap gap;
    gap.range = TimeRange{sec(6.3), sec(3.4)};
    gap.maxWords = 8;
    gap.before = "Hello there";
    gap.after = "Welcome back";
    DescribeOptions options;
    options.language = "th";
    options.style = "name the woman Ana";
    const std::vector<std::string> previous{"A man waves.", "A dog barks.", "Rain starts.", "A car stops."};
    const ChatRequest request = describeRequest(gap, {picture(1), picture(2)}, options, previous);
    CHECK(request.system.find("NONE") != std::string::npos);
    CHECK(request.system.find("\"th\"") != std::string::npos);
    CHECK(request.system.find("name the woman Ana") != std::string::npos);
    REQUIRE(request.messages.size() == 1);
    const ChatMessage& message = request.messages.front();
    int images = 0;
    for (const ContentPart& part : message.parts) {
        images += std::holds_alternative<ImagePart>(part) ? 1 : 0;
    }
    CHECK(images == 2);
    const std::string text = message.text();
    CHECK(text.find("at most 8 words") != std::string::npos);
    CHECK(text.find("3.4 seconds") != std::string::npos);
    CHECK(text.find("Hello there") != std::string::npos);
    CHECK(text.find("Welcome back") != std::string::npos);
    CHECK(text.find("time order") != std::string::npos);
    // The last three lines only.
    CHECK(text.find("A man waves.") == std::string::npos);
    CHECK(text.find("A car stops.") != std::string::npos);
    // The system prompt tells the model not to repeat the dialogue and to keep to what is visible.
    CHECK(request.system.find("Never repeat") != std::string::npos);
    CHECK(request.system.find("visible") != std::string::npos);
}

TEST_CASE("Audio description: a run over the gaps keeps a line for each, leaves a gap silent and stops at a refusal",
          "[ai][global][ad]") {
    std::vector<AdGap> gaps(4);
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        gaps[i].range = TimeRange{sec(10.0 * static_cast<double>(i) + 1), sec(4)};
        gaps[i].maxWords = 6;
    }
    const FrameSource frames = [](const AdGap& gap) {
        if (gap.range.start == sec(21)) {
            return std::vector<ImagePart>{}; // no picture for the third gap
        }
        return std::vector<ImagePart>{picture(static_cast<std::uint8_t>(secondsOf(gap.range.start)))};
    };
    ScriptedChat model({"A woman opens a red door and steps through it into the hall", "NONE", "Rain falls on a quiet street"});
    std::vector<double> reported;
    const auto result = describeGaps(gaps, model, frames, DescribeOptions{}, [&](double p) { reported.push_back(p); return true; }, {});
    REQUIRE(result);
    CHECK(result->gaps == 4);
    // Gap 1: cut to 6 words; gap 2: NONE; gap 3: no picture (the model was not asked); gap 4: kept.
    REQUIRE(result->lines.size() == 2);
    CHECK(result->lines[0].text == "A woman opens a red door.");
    CHECK(result->lines[0].start == sec(1));
    CHECK(result->lines[0].duration == sec(4));
    CHECK(result->lines[0].maxWords == 6);
    CHECK(result->lines[1].text == "Rain falls on a quiet street.");
    CHECK(result->lines[1].start == sec(31));
    CHECK(result->warnings.size() == 2);
    CHECK(model.requests.size() == 3);
    CHECK(result->usage.inputTokens == 300);
    CHECK(reported.back() == Catch::Approx(1.0));
    // The second request knows the first line, so the descriptions do not repeat each other.
    CHECK(model.requests[1].messages.front().text().find("A woman opens a red door.") != std::string::npos);

    // A refusal (no consent) ends the run with that error: the other gaps are not tried one by one.
    ScriptedChat refusing({"x", "y", "z", "w"});
    refusing.failAt = 2;
    const auto refused = describeGaps(gaps, refusing, frames, DescribeOptions{}, {}, {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == AiErrorCode::ConsentRequired);
    CHECK(refusing.requests.size() == 2);

    // Stop from the progress callback keeps what was written so far.
    ScriptedChat stopping({"A man waves.", "A dog barks.", "Rain starts.", "A car stops."});
    int calls = 0;
    const auto partial = describeGaps(gaps, stopping, frames, DescribeOptions{}, [&](double) { return ++calls <= 2; }, {});
    REQUIRE(partial);
    CHECK(partial->lines.size() == 2);
}

TEST_CASE("audioDescription.findGaps: the gaps of the sequence with their word budgets; nothing changes", "[ai][global][ad][command]") {
    Studio studio;
    auto h = studio.harness();
    const ProjectSnapshot before = h->state();
    const auto result = h->ok("audioDescription.findGaps", Json::object());
    CHECK(*h->state() == *before);
    REQUIRE(result.data["gaps"].size() == 3);
    CHECK(result.data["count"] == 3);
    CHECK(result.data["words"] == 4);
    CHECK(result.data["gaps"][1]["maxWords"] == 8);
    CHECK(result.data["gaps"][1]["before"] == "Hello there");
    CHECK(result.data["gaps"][1]["start"]["timecode"].is_string());
    // A bigger minimum leaves the shorter silences out.
    const auto fewer = h->ok("audioDescription.findGaps", Json{{"minGap", 3.6}});
    CHECK(fewer.data["gaps"].size() == 2);
    // No transcript: say so, unless the sequence has no speech.
    auto bare = studio.harness(false);
    CHECK(bare->errorOf("audioDescription.findGaps", Json::object()) == "NotFound");
    const auto silent = bare->ok("audioDescription.findGaps", Json{{"silent", true}});
    CHECK(silent.data["gaps"].size() == 2); // 16 s of silence, cut in two pieces of 8 s
    CHECK(bare->errorOf("audioDescription.findGaps", Json{{"minGap", 100.0}}) == "InvalidParams");
}

TEST_CASE("audioDescription.createTrack: a disabled caption track of the lines, one undo step, replaced by the next run",
          "[ai][global][ad][command]") {
    Studio studio;
    auto h = studio.harness();
    const auto undoBefore = h->session.undoCount();
    const Json lines = Json::array({Json{{"start", sec(6.3).value}, {"end", sec(9.7).value}, {"text", "A woman opens a red door"}},
                                    Json{{"start", sec(0.0).value}, {"duration", sec(3.7).value}, {"text", "\"Rain falls on a quiet street.\""}},
                                    Json{{"start", sec(12.3).value}, {"end", sec(16.0).value}, {"text", "NONE"}}});
    const auto made = h->ok("audioDescription.createTrack", Json{{"lines", lines}, {"language", "en"}});
    CHECK(h->session.undoCount() == undoBefore + 1);
    REQUIRE(made.data["lines"] == 2); // the NONE line is a gap left silent
    const Sequence& seq = h->sequence(h->state());
    REQUIRE(seq.captionTracks.size() == 1);
    const CaptionTrack& track = seq.captionTracks.front();
    CHECK(track.name == "Audio Description");
    CHECK(track.language == "en");
    CHECK_FALSE(track.enabled); // a script to voice, not subtitles to burn in
    REQUIRE(track.cues.size() == 2);
    CHECK(track.cues[0].start == sec(0)); // sorted
    CHECK(track.cues[0].text == "Rain falls on a quiet street."); // quotes dropped
    CHECK(track.cues[1].text == "A woman opens a red door.");     // ended with a full stop
    CHECK(secondsOf(track.cues[1].duration) == Catch::Approx(3.4));

    // Making them again replaces the track (the earlier lines and the edits in them with it).
    h->ok("audioDescription.createTrack", Json{{"lines", Json::array({Json{{"start", sec(1).value}, {"end", sec(3).value}, {"text", "A dog barks."}}})}});
    REQUIRE(h->sequence(h->state()).captionTracks.size() == 1);
    CHECK(h->sequence(h->state()).captionTracks.front().cues.size() == 1);
    // "replace": false adds another.
    h->ok("audioDescription.createTrack",
          Json{{"lines", Json::array({Json{{"start", sec(1).value}, {"end", sec(3).value}, {"text", "A cat sleeps."}}})}, {"replace", false}});
    CHECK(h->sequence(h->state()).captionTracks.size() == 2);

    // Refusals leave the project as it is.
    const ProjectSnapshot now = h->state();
    CHECK(h->errorOf("audioDescription.createTrack", Json{{"lines", Json::array()}}) == "InvalidParams");
    CHECK(h->errorOf("audioDescription.createTrack",
                     Json{{"lines", Json::array({Json{{"start", sec(1).value}, {"end", sec(4).value}, {"text", "One."}},
                                                 Json{{"start", sec(3).value}, {"end", sec(6).value}, {"text", "Two."}}})}}) == "InvalidParams"); // overlap
    CHECK(h->errorOf("audioDescription.createTrack", Json{{"lines", Json::array({Json{{"start", sec(5).value}, {"end", sec(4).value}, {"text", "Backwards."}}})}}) ==
          "InvalidParams");
    CHECK(h->errorOf("audioDescription.createTrack", Json{{"lines", Json::array({Json{{"start", sec(5).value}, {"end", sec(6).value}, {"text", "NONE"}}})}}) ==
          "InvalidParams"); // nothing to say at all
    CHECK(*h->state() == *now);
}

TEST_CASE("audioDescription: the track is what dub.create speaks, under the name Audio Description", "[ai][global][ad][command]") {
    Studio studio;
    auto h = studio.harness();
    const auto made = h->ok("audioDescription.createTrack",
                            Json{{"lines", Json::array({Json{{"start", sec(0).value}, {"end", sec(3.7).value}, {"text", "Rain falls on a quiet street."}},
                                                        Json{{"start", sec(6.3).value}, {"end", sec(9.7).value}, {"text", "A woman opens a red door."}}})},
                                 {"language", "en"}});
    const auto track = made.data["captionTrack"].get<std::uint64_t>();
    // The plan of the voicing (nothing is sent): both lines, spoken into the slots, on a track of that name.
    const auto plan = h->ok("dub.create", Json{{"captionTrack", track}, {"name", "Audio Description"}, {"language", "en"}, {"duck", -12.0}, {"estimate", true}});
    CHECK(plan.data["lines"] == 2);
    CHECK(plan.data["track"] == "Audio Description");
    REQUIRE(plan.data["script"].size() == 2);
    CHECK(plan.data["script"][1]["text"] == "A woman opens a red door.");
    CHECK(plan.data["duckDb"] == -12.0);
}
