// Filler and silence review (AI-3): language-aware filler lexicons, pauses checked against the audio energy.

#include "EditTestUtil.h"

#include <wabcut/ai/edit/Review.h>
#include <wabcut/ai/edit/TextEdits.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;
using Catch::Matchers::WithinAbs;

namespace {

/// "kind first-last label" for each item.
std::string summary(const std::vector<ReviewItem>& items) {
    std::string out;
    for (const ReviewItem& item : items) {
        out += (out.empty() ? "" : " | ") + std::string(toString(item.kind)) + " " + std::to_string(item.firstWord) + "-" +
               std::to_string(item.lastWord) + " " + item.label;
    }
    return out;
}

ReviewOptions fillersOnly() {
    ReviewOptions o;
    o.silenceThreshold.reset();
    return o;
}

/// Synthetic audio: a 440 Hz tone at -12 dBFS while a word is spoken, quiet noise floor at `floorDb` between
/// words, computed sample by sample at 16 kHz like the decoder probe does.
class SyntheticAudio : public AudioEnergyProbe {
public:
    SyntheticAudio(std::vector<TimeRange> speech, double floorDb) : m_speech(std::move(speech)), m_floor(floorDb) {}

    std::optional<double> rmsDb(const MediaItem&, int, TimeRange source) const override {
        ++calls;
        constexpr int kSampleRate = 16000;
        const auto first = static_cast<std::int64_t>(source.start.value * kSampleRate / kTicksPerSecond);
        const auto last = static_cast<std::int64_t>(source.end().value * kSampleRate / kTicksPerSecond);
        double sum = 0.0;
        const double tone = std::pow(10.0, -12.0 / 20.0) * std::numbers::sqrt2;
        const double noise = std::pow(10.0, m_floor / 20.0) * std::numbers::sqrt2;
        for (std::int64_t n = first; n < last; ++n) {
            const Ticks t{n * kTicksPerSecond / kSampleRate};
            const bool speaking = std::ranges::any_of(m_speech, [&](const TimeRange& r) { return r.contains(t); });
            const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(n) / kSampleRate;
            const double v = (speaking ? tone : noise) * std::sin(phase);
            sum += v * v;
        }
        if (last <= first) {
            return std::nullopt;
        }
        return 10.0 * std::log10(sum / static_cast<double>(last - first));
    }

    mutable int calls = 0;

private:
    std::vector<TimeRange> m_speech;
    double m_floor;
};

} // namespace

TEST_CASE("Built-in filler lexicons are data files", "[ai][edit][review]") {
    const auto languages = FillerLexicon::builtinLanguages();
    CHECK(std::ranges::find(languages, "en") != languages.end());
    CHECK(std::ranges::find(languages, "th") != languages.end());
    const FillerLexicon* en = FillerLexicon::builtin("en");
    REQUIRE(en);
    CHECK(std::ranges::any_of(en->entries, [](const auto& e) { return e.text == "um" && !e.contextual; }));
    CHECK(std::ranges::any_of(en->entries, [](const auto& e) { return e.text == "you know" && e.contextual; }));
    const FillerLexicon* th = FillerLexicon::builtin("th");
    REQUIRE(th);
    CHECK(std::ranges::any_of(th->entries, [](const auto& e) { return e.text == "เอ่อ" && !e.contextual; }));
    CHECK(std::ranges::any_of(th->entries, [](const auto& e) { return e.text == "แบบว่า" && e.contextual; }));
    CHECK(FillerLexicon::builtin("xx") == nullptr);

    const auto custom = FillerLexicon::parse("de", "# German\näh\nalso | context\n\nsozusagen | context  # adverb\n");
    REQUIRE(custom.has_value());
    REQUIRE(custom->entries.size() == 3);
    CHECK(custom->entries[1].text == "also");
    CHECK(custom->entries[1].contextual);
    CHECK(!FillerLexicon::parse("de", "äh | always\n").has_value());
}

TEST_CASE("English fillers: always, and only in disfluent context", "[ai][edit][review]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    const auto p = provider(tl, {{'A', transcript("en", {
                                                            {"So", 0, 3},
                                                            {"um,", 3, 6},          // 0.9: always a filler
                                                            {"I", 6, 7},
                                                            {"like", 7, 10},        // "I like pizza": fluent
                                                            {"pizza.", 10, 14},
                                                            {"It", 20, 22},
                                                            {"was,", 22, 25},
                                                            {"like,", 25, 28},      // after a comma word: "like," ends in a comma
                                                            {"great.", 28, 32},
                                                            {"You", 32, 34},
                                                            {"know", 34, 36},       // "You know" fluent: no pause, no comma
                                                            {"what?", 36, 39},
                                                            {"Uhh", 60, 61.2, {}, 0.3f}, // short and unsure
                                                            {"you", 70, 72},
                                                            {"know", 72, 74},       // pause after: disfluent
                                                            {"right.", 80, 84},
                                                        })}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    const auto items = review(st, tl.state, fillersOnly());
    CHECK(summary(items) == "filler 1-1 um, | filler 7-7 like, | filler 12-12 Uhh | filler 13-14 you know");
    REQUIRE(items.size() == 4);
    CHECK_THAT(items[0].confidence, WithinAbs(0.9, 1e-6));
    CHECK_THAT(items[1].confidence, WithinAbs(0.6, 1e-6));
    CHECK_THAT(items[2].confidence, WithinAbs(0.9 - 0.1 - 0.2, 1e-6)); // ASR unsure, 48 ms
    CHECK_THAT(items[3].confidence, WithinAbs(0.6 + 0.15, 1e-6));      // pauses on both sides
    CHECK(items[0].context == "So [um,] I like pizza. It...");
    CHECK(items[0].range == TimeRange::fromStartEnd(f(3), f(6)));
    CHECK(items[3].context == "...You know what? Uhh [you know] right.");
}

TEST_CASE("Thai fillers match across word units", "[ai][edit][review]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    // Thai is written without spaces; transcripts hold word units, so a filler can span several of them.
    const auto p = provider(tl, {{'A', transcript("th", {
                                                            {"วันนี้", 0, 5},
                                                            {"เอ่", 5, 7}, {"อ", 7, 8},       // เอ่อ, in two units
                                                            {"เรา", 8, 10},
                                                            {"แบบ", 10, 12}, {"นี้", 12, 14}, // "แบบนี้" (like this): not a filler
                                                            {"ก็", 20, 22},
                                                            {"แบบ", 22, 24}, {"ว่า", 24, 26}, // แบบว่า + a pause: filler
                                                            {"ดี", 32, 35},
                                                            {"อืม", 40, 44},
                                                        })}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    const auto items = review(st, tl.state, fillersOnly());
    CHECK(summary(items) == "filler 1-2 เอ่อ | filler 7-8 แบบว่า | filler 10-10 อืม");
    REQUIRE(items.size() == 3);
    CHECK_THAT(items[0].confidence, WithinAbs(0.9, 1e-6));
    CHECK_THAT(items[1].confidence, WithinAbs(0.6, 1e-6));
    CHECK(items[1].context == "...เราแบบนี้ก็ [แบบว่า] ดีอืม");

    SECTION("A project's own lexicon replaces the built-in one") {
        ReviewOptions o = fillersOnly();
        o.lexicons.push_back(*FillerLexicon::parse("th", "ดี\n"));
        CHECK(summary(review(st, tl.state, o)) == "filler 9-9 ดี");
    }
}

TEST_CASE("Silences: long pauses, checked against the audio", "[ai][edit][review][silence]") {
    Timeline tl(R"(
        V1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    // Pauses: 0.4 s after "one", 1.2 s after "two" (silent), 1.2 s after "three" (someone laughs).
    const auto p = provider(tl, {{'A', transcript("en", {{"one", 0, 5}, {"two", 15, 20}, {"three", 50, 55}, {"four", 85, 90}})}});
    const auto st = SequenceTranscript::build(tl.state, tl.seq, *p);
    ReviewOptions o;
    o.fillers = false;

    SECTION("Without audio: every pause over the threshold, at 0.7") {
        const auto items = review(st, tl.state, o);
        CHECK(summary(items) == "silence 1-2 1.2 s pause | silence 2-3 1.2 s pause");
        REQUIRE(items.size() == 2);
        CHECK(items[0].range == TimeRange::fromStartEnd(f(20), f(50)));
        CHECK_THAT(items[0].confidence, WithinAbs(0.7, 1e-6));
        CHECK(items[0].context == "one two [1.2 s] three four");
    }
    SECTION("Synthetic audio: a quiet pause is sure, a loud one is not") {
        const SyntheticAudio audio({TimeRange::fromStartEnd(f(0), f(20)), TimeRange::fromStartEnd(f(50), f(55)),
                                    TimeRange::fromStartEnd(f(60), f(80)) /* laughter */,
                                    TimeRange::fromStartEnd(f(85), f(90))},
                                   -70.0);
        const auto items = review(st, tl.state, o, &audio);
        REQUIRE(items.size() == 2);
        REQUIRE(items[0].rmsDb.has_value());
        CHECK_THAT(*items[0].rmsDb, WithinAbs(-70.0, 0.5));
        CHECK_THAT(items[0].confidence, WithinAbs(0.9, 1e-6));
        REQUIRE(items[1].rmsDb.has_value());
        CHECK(*items[1].rmsDb > -40.0);
        CHECK_THAT(items[1].confidence, WithinAbs(0.3, 1e-6));
        CHECK(audio.calls == 2); // one clip under each pause
    }
    SECTION("A shorter threshold finds the short pause too") {
        o.silenceThreshold = Ticks{kTicksPerSecond * 3 / 10};
        INFO(summary(review(st, tl.state, o)));
        CHECK(review(st, tl.state, o).size() == 3);
    }
    SECTION("Removed with room tone on both sides") {
        const auto items = review(st, tl.state, o);
        const DeleteRequest request = deleteRequestFor(std::span(items).first(1));
        CHECK(request.gapsAfter == std::vector<std::size_t>{1});
        const auto plan = planDelete(st, tl.state, request);
        REQUIRE(plan.has_value());
        REQUIRE(plan->cuts.size() == 1);
        CHECK(plan->cuts[0].range == TimeRange::fromStartEnd(f(24), f(46))); // 150 ms = 3.75 frames kept each side
        CHECK(plan->cuts[0].tracks.size() == 2);
    }
}
