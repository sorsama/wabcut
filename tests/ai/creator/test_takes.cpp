// Retake comparison (AI-11): repeated attempts of a line grouped across the timeline and the recordings (a false start,
// a messy take, a clean one; a take in another recording), lines that only look alike kept apart, precision and recall
// on the fixture, word diffs and quality, and "use this take" / "keep best takes" edits through the engine: the line is
// heard once, links kept, one undo step.

#include "CommandHarness.h"
#include "CreatorTestUtil.h"

#include <wabcut/ai/edit/SequenceTranscript.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::roughcut;
using namespace wabcut::ai::roughcut::test;
using wabcut::test::CommandHarness;

namespace {

/// A 40 s recording (A) cut into the sequence whole (linked V1/A1), and a second recording (B) not in the sequence.
struct TakesStudio {
    TakesStudio() : tl(picture(1000), {{'A', 1100}, {'B', 400}}) {
        tl.linkMatching();
        std::vector<ai::edit::test::W> a;
        say(a, 1.0, "Welcome back to the studio everyone.");
        say(a, 4.0, "So the new...");
        say(a, 6.0, "So the new timeline um is uh much faster.");
        say(a, 10.0, "So the new timeline is much faster.");
        say(a, 14.0, "We tested the first version yesterday.");
        say(a, 18.0, "We tested the second version today.");
        say(a, 22.0, "Exports finish in half the time now.");
        say(a, 26.0, "Exports finish in half the, the time now.");
        say(a, 31.0, "Thanks for watching and see you soon.");
        std::vector<ai::edit::test::W> b;
        say(b, 1.0, "Exports finish in half the time now!");
        say(b, 5.0, "Something completely different about lenses.");
        transcripts = ai::edit::test::provider(tl, {{'A', ai::edit::test::transcript("en", a)}, {'B', ai::edit::test::transcript("en", b)}});
        services = std::make_shared<command::ServiceBundle>(
            std::vector<std::shared_ptr<command::CommandServices>>{std::make_shared<ai::edit::TranscriptServices>(transcripts)});
    }
    static std::string picture(int frames) {
        const std::string cells = "A" + std::string(static_cast<std::size_t>(frames - 1), 'a');
        return "V1 |" + cells + "|\nA1 |" + cells + "|";
    }
    /// The sequence's words as text.
    std::string heard(const ProjectState& state) const {
        const auto st = ai::edit::SequenceTranscript::build(state, tl.seq, *transcripts);
        return st.empty() ? std::string() : st.textOf(0, st.size() - 1);
    }
    Timeline tl;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
    std::shared_ptr<command::ServiceBundle> services;
};

std::size_t count(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

void record(const std::string& line) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-creator";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "accuracy.md", std::ios::app) << line << "\n";
}

} // namespace

TEST_CASE("Repeated attempts are grouped; look-alike lines stay apart; precision and recall", "[ai][creator][takes]") {
    TakesStudio studio;
    CommandHarness h(studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, studio.services}, ai::edit::editingCommands());
    const auto found = h.ok("takes.find", Json{{"media", Json::array({studio.tl.media('B').value})}});
    const Json& groups = found.data["groups"];
    INFO(groups.dump(1));
    REQUIRE(groups.size() == 2);
    // Ground truth: attempts by where they start (timeline seconds for the sequence, "B@" for the other recording).
    const std::vector<std::set<std::string>> truth{{"4", "6", "10"}, {"22", "26", "B@1"}};
    std::size_t truePairs = 0;
    for (const auto& t : truth) {
        truePairs += t.size() * (t.size() - 1) / 2;
    }
    std::size_t foundPairs = 0;
    std::size_t correctPairs = 0;
    for (const Json& g : groups) {
        std::vector<std::string> ids;
        for (const Json& take : g["takes"]) {
            const int start = static_cast<int>(std::lround(take["start"].get<double>()));
            ids.push_back(take["onTimeline"].get<bool>() ? std::to_string(start) : "B@" + std::to_string(start));
        }
        for (std::size_t x = 0; x < ids.size(); ++x) {
            for (std::size_t y = x + 1; y < ids.size(); ++y) {
                ++foundPairs;
                correctPairs += std::ranges::any_of(truth, [&](const auto& t) { return t.contains(ids[x]) && t.contains(ids[y]); }) ? 1 : 0;
            }
        }
    }
    const double precision = foundPairs ? static_cast<double>(correctPairs) / static_cast<double>(foundPairs) : 0.0;
    const double recall = static_cast<double>(correctPairs) / static_cast<double>(truePairs);
    record("Retake grouping fixture: precision " + std::to_string(precision) + ", recall " + std::to_string(recall) + " (" +
           std::to_string(correctPairs) + " of " + std::to_string(truePairs) + " true pairs, " + std::to_string(foundPairs) + " found)");
    CHECK(precision == 1.0);
    CHECK(recall == 1.0);

    // The line's best take is the clean one; the messy one differs by its fillers; the false start is least complete.
    const Json& line = groups[0];
    CHECK(line["onTimeline"] == 3);
    CHECK(line["improvable"] == true);
    CHECK(line["takes"][0]["start"].get<double>() == Catch::Approx(10.0).margin(0.1));
    CHECK(line["takes"][0]["fillers"] == 0);
    const auto messy = std::ranges::find_if(line["takes"], [](const Json& t) { return t["fillers"].get<int>() == 2; });
    REQUIRE(messy != line["takes"].end());
    CHECK(std::ranges::any_of((*messy)["diff"], [](const Json& d) { return d["op"] == "insert" && d["text"].get<std::string>().find("um") != std::string::npos; }));
    const auto fragment = std::ranges::find_if(line["takes"], [](const Json& t) { return t["start"].get<double>() < 5.0; });
    REQUIRE(fragment != line["takes"].end());
    CHECK((*fragment)["completeness"].get<double>() < 0.6);
    // The exports line: the take with "the, the" has a false start; the one in recording B is an alternate.
    const Json& exports = groups[1];
    CHECK(exports["onTimeline"] == 2);
    CHECK(std::ranges::any_of(exports["takes"], [](const Json& t) { return t["falseStarts"].get<int>() >= 1; }));
    CHECK(std::ranges::any_of(exports["takes"], [](const Json& t) { return !t["onTimeline"].get<bool>() && t.contains("media"); }));
    // Deterministic.
    const auto again = h.ok("takes.find", Json{{"media", Json::array({studio.tl.media('B').value})}});
    Json a = found.data;
    Json b = again.data;
    a.erase("milliseconds");
    b.erase("milliseconds");
    CHECK(a == b);
}

TEST_CASE("Use this take: the line is heard once, with the chosen take, links kept, one undo step", "[ai][creator][takes][commands]") {
    TakesStudio studio;
    CommandHarness h(studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, studio.services}, ai::edit::editingCommands());
    const auto before = h.state();
    const Json scope{{"media", Json::array({studio.tl.media('B').value})}};
    CHECK(count(studio.heard(*before), "So the new") == 3);

    // The clean take is on the timeline: the false start and the messy take go.
    Json params = scope;
    params["group"] = 1;
    params["take"] = 1;
    const auto used = h.ok("takes.use", params);
    CHECK(used.committed);
    CHECK(used.data["review"].size() == 1);
    CHECK(used.data["review"][0]["removed"] == 2);
    REQUIRE(validate(*h.state()));
    std::string heard = studio.heard(*h.state());
    INFO(heard);
    CHECK(count(heard, "So the new") == 1);
    CHECK(heard.find("um") == std::string::npos);
    CHECK(count(heard, "Welcome back") == 1);
    CHECK(count(heard, "Exports finish") == 2); // the other line untouched
    CHECK(count(heard, "first version") == 1);
    CHECK(count(heard, "second version") == 1);
    for (const auto& track : {h.state()->findSequence(studio.tl.seq)->videoTracks.front(), h.state()->findSequence(studio.tl.seq)->audioTracks.front()}) {
        for (const Clip& c : track->clips) {
            CHECK(c.linkGroup.isValid()); // every piece still linked to its partner
        }
    }
    REQUIRE(h.session.undo());
    CHECK(*h.state() == *before);

    // A take only in recording B replaces the first exports take; the second one goes.
    const auto found = h.ok("takes.find", scope);
    const Json& exports = found.data["groups"][1];
    std::size_t fromB = 0;
    for (std::size_t t = 0; t < exports["takes"].size(); ++t) {
        if (!exports["takes"][t]["onTimeline"].get<bool>()) {
            fromB = t + 1;
        }
    }
    REQUIRE(fromB > 0);
    params = scope;
    params["group"] = 2;
    params["take"] = fromB;
    const auto replaced = h.ok("takes.use", params);
    CHECK(replaced.data["review"][0]["replaced"] == true);
    REQUIRE(validate(*h.state()));
    heard = studio.heard(*h.state());
    CHECK(count(heard, "Exports finish") == 1);
    CHECK(heard.find("now!") != std::string::npos); // recording B's words
    const Sequence& seq = *h.state()->findSequence(studio.tl.seq);
    const auto fromRecordingB = [&](const TrackPtr& t) {
        return std::ranges::count_if(t->clips, [&](const Clip& c) { return c.media == studio.tl.media('B'); });
    };
    CHECK(fromRecordingB(seq.audioTracks.front()) == 1);
    CHECK(fromRecordingB(seq.videoTracks.front()) == 1);
    for (const Clip& c : seq.audioTracks.front()->clips) {
        if (c.media == studio.tl.media('B')) {
            CHECK(c.linkGroup.isValid());
            // Handles: a little media around the words, never into "Something completely different".
            CHECK(c.sourceOut() <= Ticks::fromSeconds(5));
        }
    }
    REQUIRE(h.session.undo());
    CHECK(*h.state() == *before);
}

TEST_CASE("Keep best takes applies every improvable group as one undo step with a review list", "[ai][creator][takes][commands]") {
    TakesStudio studio;
    CommandHarness h(studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, studio.services}, ai::edit::editingCommands());
    const auto before = h.state();
    const auto best = h.ok("takes.use", Json{{"best", true}, {"media", Json::array({studio.tl.media('B').value})}});
    CHECK(best.committed);
    CHECK(best.data["review"].size() == 2);
    REQUIRE(validate(*h.state()));
    const std::string heard = studio.heard(*h.state());
    INFO(heard);
    CHECK(count(heard, "So the new") == 1);
    CHECK(count(heard, "Exports finish") == 1);
    CHECK(count(heard, "Thanks for watching") == 1);
    REQUIRE(h.session.undo());
    CHECK(*h.state() == *before);
    // Once applied, nothing is left to improve: a no-op with a warning. Bad numbers are refused.
    CHECK(h.ok("takes.use", Json{{"best", true}}).committed);
    const auto none = h.ok("takes.use", Json{{"best", true}});
    CHECK(none.data["changed"] == false);
    CHECK_FALSE(none.warnings.empty());
    CHECK(h.errorOf("takes.use", Json{{"group", 9}}) == "InvalidParams");
    CHECK(h.errorOf("takes.use", Json::object()) == "InvalidParams");
}
