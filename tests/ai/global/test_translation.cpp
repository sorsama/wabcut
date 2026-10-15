// AI-12 caption translation: re-segmentation to the target's reading speed (English, Thai between words, Japanese),
// "[bleep]" kept as a marker and never translated back into content, the corrections layer before translation,
// the user's chat model as a translator (JSON in and out), and captions.translate through the engine (dry run, one
// undo step, no-cloud refusal, the cost estimate).

#include "AgentTestUtil.h"
#include "CommandHarness.h"
#include "EditTestUtil.h"
#include "GlobalTestUtil.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/translate/CaptionTranslation.h>
#include <wabcut/ai/translate/Languages.h>
#include <wabcut/ai/translate/Segmentation.h>
#include <wabcut/ai/translate/TranslateCommands.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::translate;
using namespace wabcut::ai::global::test;
using namespace wabcut::ai::edit::test;
using namespace wabcut::ai::test;
using command::Json;

namespace {

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

// Thai words for the word-list breaker (Thai is written without spaces between words).
const std::vector<std::string> kThaiWords{"สวัสดี", "ครับ", "วันนี้", "เรา", "จะ", "ไป", "ตลาด", "น้ำ", "กัน", "ที่", "สวย", "มาก",
                                          "และ", "ซื้อ", "ผลไม้", "อร่อย", "หลาย", "อย่าง", "ด้วย"};

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::size_t from = 0;
    while (true) {
        const auto at = text.find('\n', from);
        out.push_back(text.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (at == std::string::npos) {
            return out;
        }
        from = at + 1;
    }
}

/// Whether `line` is made of whole words of the list (and spaces).
bool wholeWords(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ') {
            ++i;
            continue;
        }
        std::size_t best = 0;
        for (const std::string& w : kThaiWords) {
            if (line.substr(i, w.size()) == w) {
                best = std::max(best, w.size());
            }
        }
        if (best == 0) {
            return false;
        }
        i += best;
    }
    return true;
}

std::string withoutBreaks(std::string text) {
    std::erase(text, '\n');
    std::erase(text, ' ');
    return text;
}

} // namespace

TEST_CASE("A translation that reads in time keeps its cue's timing", "[ai][global][translate][segmentation]") {
    const TranslatedCue cues[] = {{sec(1), sec(2), "Hola a todos.", {}, 0}, {sec(4), sec(2), "Bienvenidos.", {}, 1}};
    SegmentationReport report;
    const auto out = resegment(cues, "es", rulesFor("es"), TimingRules{}, simpleLineBreaker(), &report);
    REQUIRE(out.size() == 2);
    CHECK(out[0].start == sec(1));
    CHECK(out[0].duration == sec(2));
    CHECK(out[1].text == "Bienvenidos.");
    CHECK(report.kept == 2);
    CHECK(report.tooFast == 0);
}

TEST_CASE("Thai that is too long for its cue borrows the gap, then splits between words within 17 cps and 35 per line",
          "[ai][global][translate][segmentation][thai]") {
    std::string thai;
    for (int i = 0; i < 3; ++i) {
        thai += "วันนี้เราจะไปตลาดน้ำกันที่สวยมาก และซื้อผลไม้อร่อยหลายอย่างด้วย ";
    }
    thai.pop_back();
    // A long line (9.5 s of speech): ~150 Thai characters need about 9 s at 17 a second, and at most 70 fit one cue.
    const TranslatedCue cues[] = {{sec(1), sec(9.5), thai, {}, 0}, {sec(12), sec(2), "สวัสดีครับ", {}, 1}};
    const WordListBreaker breaker(kThaiWords);
    SegmentationReport report;
    const LanguageRules rules = rulesFor("th");
    CHECK(rules.charsPerSecond == Catch::Approx(17.0));
    CHECK(rules.maxCharsPerLine == 35);
    const auto out = resegment(cues, "th", rules, TimingRules{}, breaker, &report);
    REQUIRE(out.size() >= 3);
    CHECK(report.split >= 1);
    std::string joined;
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        const TimedCue& c = out[i];
        INFO(c.text);
        CHECK_FALSE(c.tooFast);
        CHECK(c.charsPerSecond <= rules.charsPerSecond + 0.5);
        const auto lines = linesOf(c.text);
        CHECK(lines.size() <= 2);
        for (const std::string& line : lines) {
            CHECK(captionDisplayLength(line) <= 35);
            CHECK(wholeWords(line)); // never inside a word
        }
        CHECK(c.sources == std::vector<std::size_t>{0});
        if (i > 0) {
            CHECK(c.start >= out[i - 1].end()); // in order, no overlap
        }
        joined += c.text;
    }
    CHECK(withoutBreaks(joined) == withoutBreaks(thai)); // nothing lost or reordered
    CHECK(out.back().text == "สวัสดีครับ");
    CHECK(out.back().start == sec(12)); // the next cue keeps its time
}

TEST_CASE("Japanese reads at 4 characters a second; what cannot read in time is kept and flagged", "[ai][global][translate][segmentation]") {
    const TranslatedCue cues[] = {{sec(0), sec(1), "これは非常に長い説明文です", {}, 0}, {sec(1.5), sec(1), "次", {}, 1}};
    SegmentationReport report;
    const auto out = resegment(cues, "ja", rulesFor("ja"), TimingRules{}, simpleLineBreaker(), &report);
    REQUIRE_FALSE(out.empty());
    CHECK(report.tooFast >= 1);
    CHECK(report.maxCharsPerSecond > 4.0);
    std::string joined;
    for (const TimedCue& c : out) {
        joined += c.text;
    }
    CHECK(withoutBreaks(joined) == "これは非常に長い説明文です次");
}

// A short first cue that cannot stay up longer (the next speaker follows at once) grew backwards past the sequence's
// start: "a cue with a negative start", and the whole translation was refused (found by the wave-5 redaction tests).
TEST_CASE("A short first cue never starts before the sequence does", "[ai][global][translate][segmentation]") {
    const TranslatedCue cues[] = {{Ticks{}, sec(0.4), "Hola", SpeakerId{1}, 0}, {sec(0.45), sec(2), "Hasta luego, amigos", SpeakerId{2}, 1}};
    const auto out = resegment(cues, "es", rulesFor("es"), TimingRules{}, simpleLineBreaker(), nullptr);
    REQUIRE(out.size() == 2);
    CHECK(out[0].start >= Ticks{});
    CHECK(out[0].duration > Ticks{});
    CHECK(out[0].end() <= out[1].start);
    CHECK(out[1].start == sec(0.45));
    // Too long for its cue: it borrows time from the gap after it and a lead-in before it - not before 0.
    const TranslatedCue first[] = {{Ticks{}, sec(0.9), "ก็ [bleep] และสวัสดีครับทุกคนที่มาวันนี้", {}, 0},
                                   {sec(2.5), sec(1), "ขอบคุณครับ", SpeakerId{2}, 1}};
    const auto lead = resegment(first, "th", rulesFor("th"), TimingRules{}, simpleLineBreaker(), nullptr);
    REQUIRE_FALSE(lead.empty());
    CHECK(lead.front().start >= Ticks{});
    for (const TimedCue& cue : lead) {
        CHECK(cue.duration > Ticks{});
    }
}

TEST_CASE("Short neighbours of one speaker merge when together they read in time", "[ai][global][translate][segmentation]") {
    const SpeakerId host{7};
    const TranslatedCue cues[] = {{sec(0), sec(0.6), "Muchas gracias a todos", host, 0},
                                  {sec(0.7), sec(0.6), "por venir esta noche aquí.", host, 1}};
    SegmentationReport report;
    const auto out = resegment(cues, "es", rulesFor("es"), TimingRules{}, simpleLineBreaker(), &report);
    CHECK((report.merged + report.extended + report.rebalanced) >= 1);
    for (const TimedCue& c : out) {
        CHECK(c.duration >= sec(5.0 / 6.0) - Ticks{1});
    }
}

TEST_CASE("[bleep] stays a marker: a translation that loses it is redone piece by piece", "[ai][global][translate][bleep]") {
    CaptionTrack track;
    track.name = "Captions";
    track.language = "en";
    track.cues = {CaptionCue{sec(0), sec(3), "what the [bleep] is this", {}}};
    const std::vector<std::string> texts{track.cues[0].text};

    DictionaryTranslator loses({{"what the [bleep] is this", "นี่มันอะไร [bleep] กัน"}, {"what the", "อะไร"}, {"is this", "นี่"}}, true);
    TranslateOptions options;
    options.sourceLanguage = "en";
    const WordListBreaker breaker(kThaiWords);
    auto result = translateTrack(track, texts, "th", loses, options, breaker, {}, {});
    REQUIRE(result);
    CHECK(result->retried == 1);
    const std::vector<std::string> sent = loses.sent();
    REQUIRE(sent.size() == 3);
    CHECK(sent[0] == "what the [bleep] is this"); // a model sees the marker...
    CHECK(sent[1] == "what the");                 // ...and when it loses one, the pieces go alone
    CHECK(sent[2] == "is this");
    REQUIRE(result->track.cues.size() == 1);
    CHECK(countBleeps(result->track.cues[0].text) == 1);
    CHECK(result->track.cues[0].text.find("is this") == std::string::npos);

    // A local model never sees the marker at all.
    DictionaryTranslator local({{"what the", "อะไร"}, {"is this", "นี่"}});
    options.markersKept = false;
    auto pieces = translateTrack(track, texts, "th", local, options, breaker, {}, {});
    REQUIRE(pieces);
    for (const std::string& s : local.sent()) {
        CHECK(s.find("[bleep]") == std::string::npos);
    }
    CHECK(countBleeps(pieces->track.cues[0].text) == 1);
    CHECK(pieces->track.name == "Captions (ไทย)");
    CHECK(pieces->track.language == "th");
}

TEST_CASE("Glossary terms a translation missed are reported", "[ai][global][translate]") {
    CaptionTrack track;
    track.name = "Captions";
    track.language = "en";
    track.cues = {CaptionCue{sec(0), sec(3), "Edit with Wabcut today", {}}};
    DictionaryTranslator t({{"Edit with Wabcut today", "Edita hoy"}});
    TranslateOptions options;
    options.sourceLanguage = "en";
    options.glossary = {GlossaryTerm{"Wabcut", "Wabcut"}};
    auto result = translateTrack(track, {track.cues[0].text}, "es", t, options, simpleLineBreaker(), {}, {});
    REQUIRE(result);
    CHECK(result->missedTerms == std::vector<std::size_t>{0});
    CHECK_FALSE(result->warnings.empty());
    CHECK(translationContext("en", "es", options).find("Wabcut") != std::string::npos);
}

TEST_CASE("The chat model translates JSON entries by id and keeps the markers", "[ai][global][translate][llm]") {
    auto model = std::make_unique<ScriptedModel>("claude-sonnet-5-5");
    ScriptedModel* scripted = model.get();
    scripted->text(R"({"translations":[{"id":"1","text":"สวัสดี [bleep] ครับ"},{"id":"0","text":"ยินดีต้อนรับ"}]})");
    LlmTranslator translator(std::move(model));
    TranslationRequest request;
    request.sourceLanguage = "en";
    request.targetLanguage = "th";
    request.segments = {{"0", "Welcome"}, {"1", "Hello [bleep] friends"}, {"2", "Missing"}};
    auto result = translator.translate(request, {});
    REQUIRE(result);
    REQUIRE(result->segments.size() == 3);
    CHECK(result->segments[0].text == "ยินดีต้อนรับ");
    CHECK(result->segments[1].text == "สวัสดี [bleep] ครับ");
    CHECK(result->segments[2].text.empty()); // missing: the caller retries it on its own
    const auto requests = scripted->requests();
    REQUIRE(requests.size() == 1);
    const std::string sent = requests[0].messages.back().text();
    CHECK(sent.find("\"Hello [bleep] friends\"") != std::string::npos);
    CHECK(requests[0].responseFormat.has_value());
    CHECK(requests[0].system.find("[bleep]") != std::string::npos);
}

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

std::shared_ptr<const Transcript> interview() {
    return transcript("en", {{"So", 0, 3, 0}, {"welcome", 8, 13, 0}, {"to", 13, 15, 0}, {"teh", 15, 17, 0}, {"show.", 17, 22, 0},
                             {"Thanks", 62, 67, 1}, {"for", 67, 69, 1}, {"having", 69, 74, 1}, {"me.", 74, 80, 1}});
}

/// The interview with "welcome" bleeped and a translator for each language.
struct Studio {
    explicit Studio(bool noCloud = false, std::shared_ptr<ScriptedModel> chat = nullptr) : chatModel(std::move(chat)) {
        Timeline tl(kPicture);
        tl.linkMatching();
        Redaction r;
        r.media = tl.media('A');
        r.name = "Name";
        r.source = TimeRange::fromStartEnd(frames(8), frames(13));
        r.audio = RedactionAudio::Bleep;
        auto added = wabcut::edit::addRedaction(tl.state, r);
        REQUIRE(added);
        tl.state = std::move(added->state);
        tl.state.aiNoCloud = noCloud;
        transcripts = provider(tl, {{'A', interview()}});
        auto local = [this](std::string_view, std::string_view target) -> std::expected<std::unique_ptr<ITranslator>, AiError> {
            if (target == "ko") {
                return std::unexpected(AiError(AiErrorCode::ModelMissing, "no local model for ko"));
            }
            auto t = std::make_shared<DictionaryTranslator>(dictionary);
            translators.push_back(t);
            return std::make_unique<SharedTranslator>(t);
        };
        auto chatOpener = [this]() -> std::expected<std::unique_ptr<IChatModel>, AiError> {
            if (!chatModel) {
                return std::unexpected(AiError(AiErrorCode::NoCredentials, "no key"));
            }
            return std::make_unique<SharedModel>(chatModel);
        };
        auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<ai::edit::TranscriptServices>(transcripts), std::make_shared<TranslateServices>(local, chatOpener),
            std::make_shared<command::CaptionServices>(std::make_shared<WordListBreaker>(kThaiWords))});
        h = std::make_unique<CommandHarness>(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 1024, services},
                                             ai::edit::editingCommands());
    }

    std::vector<CaptionTrack> tracks() const { return h->sequence(h->state()).captionTracks; }

    std::map<std::string, std::string> dictionary{{"So", "ก็"}, {"to the show.", "สู่รายการ"}, {"Thanks for having me.", "ขอบคุณที่เชิญครับ"}};
    std::shared_ptr<ScriptedModel> chatModel;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
    std::vector<std::shared_ptr<DictionaryTranslator>> translators;
    std::unique_ptr<CommandHarness> h;
};

} // namespace

TEST_CASE("captions.translate: corrections first, bleeps never sent, a new track per language in one undo step",
          "[ai][global][translate][command]") {
    Studio s;
    s.h->ok("captions.generateFromTranscript", Json{{"maxCharsPerLine", 20}});
    REQUIRE(s.tracks().size() == 1);
    CHECK(s.tracks()[0].cues[0].text.find("[bleep]") != std::string::npos); // the cue over the redaction
    s.h->ok("transcript.correctWord", Json{{"word", 3}, {"text", "the"}});
    CHECK(s.tracks()[0].cues[0].text.find("teh") != std::string::npos); // the cue was made before the correction

    const ProjectSnapshot before = s.h->state();
    const auto dry = s.h->run(Json{{"command", "captions.translate"}, {"params", Json{{"languages", Json::array({"th"})}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(s.h->state() == before);

    const std::size_t undoBefore = s.h->session.undoCount();
    const auto done = s.h->ok("captions.translate", Json{{"languages", Json::array({"th", "es"})}, {"provider", "local"}});
    CHECK(s.h->session.undoCount() == undoBefore + 1);
    CHECK(done.data["corrected"] == 1);
    CHECK(done.data["redacted"] == 1);
    REQUIRE(s.translators.size() >= 2);
    for (const auto& t : s.translators) {
        for (const std::string& text : t->sent()) {
            INFO(text);
            CHECK(text.find("welcome") == std::string::npos); // the redacted word never leaves
            CHECK(text.find("[bleep]") == std::string::npos); // a local model gets the pieces
            CHECK(text.find("teh") == std::string::npos);     // the correction applies first
        }
    }
    const auto tracks = s.tracks();
    REQUIRE(tracks.size() == 3);
    const CaptionTrack& thai = tracks[1];
    CHECK(thai.language == "th");
    CHECK(thai.name == tracks[0].name + " (ไทย)");
    REQUIRE_FALSE(thai.cues.empty());
    CHECK(thai.cues[0].text.find("[bleep]") != std::string::npos);
    CHECK(thai.cues[0].text.find("สู่รายการ") != std::string::npos);
    CHECK(thai.cues[0].start == tracks[0].cues[0].start);
    CHECK(tracks[2].language == "es");

    // Again: the same tracks are replaced, not duplicated; undo takes the whole translation back.
    s.h->ok("captions.translate", Json{{"captionTrack", tracks[0].id.value}, {"languages", Json::array({"th"})}, {"provider", "local"}});
    CHECK(s.tracks().size() == 3);
    REQUIRE(s.h->session.undo());
    REQUIRE(s.h->session.undo());
    CHECK(s.tracks().size() == 1);
}

TEST_CASE("captions.translate refuses the cloud in a no-cloud project and prices a cloud run without sending it",
          "[ai][global][translate][command][privacy]") {
    {
        Studio locked(true, std::make_shared<ScriptedModel>("claude-sonnet-5-5"));
        locked.h->ok("captions.generateFromTranscript");
        CHECK(locked.h->errorOf("captions.translate", Json{{"languages", Json::array({"th"})}, {"provider", "cloud"}}) == "EditRejected");
        // ko has no local model: auto would need the cloud, which the project forbids.
        CHECK(locked.h->errorOf("captions.translate", Json{{"languages", Json::array({"ko"})}}) == "EditRejected");
        CHECK(locked.chatModel->requests().empty());
    }
    auto chat = std::make_shared<ScriptedModel>("claude-sonnet-5-5");
    Studio cloud(false, chat);
    cloud.h->ok("captions.generateFromTranscript");
    const auto estimate = cloud.h->ok("captions.translate", Json{{"languages", Json::array({"ko"})}, {"estimate", true}});
    CHECK(chat->requests().empty()); // nothing sent
    REQUIRE(estimate.data.contains("estimate"));
    CHECK(estimate.data["estimate"]["usd"].get<double>() > 0.0);
    CHECK(estimate.data["estimate"]["languages"][0]["provider"] == "cloud");
    CHECK(cloud.tracks().size() == 1);
}

// Review wave 5, A5-P1-4: what a cloud translator gets of a typed or imported cue over redacted speech. Without a
// transcript (or with one the cue no longer matches) which word was redacted is unknown, so the whole cue is "[bleep]",
// as burn-in and sidecars show it; a cue the transcript accounts for keeps its other words.
TEST_CASE("Caption text over redacted speech never leaves as text whatever made the cue", "[ai][global][translate][privacy]") {
    Studio s;
    const SequencePtr seq = s.h->state()->sequences.front();
    CaptionTrack typed;
    typed.id = CaptionTrackId{999};
    typed.language = "en";
    // "welcome" (frames 8-13) is redacted. The first cue was typed over it with another spelling, so the transcript cannot
    // tell which of its words is the redacted one; the second cue is clear of it.
    typed.cues = {CaptionCue{frames(0), frames(30), "Hi Bob, wellcome!", {}}, CaptionCue{frames(60), frames(25), "Thanks for having me.", {}}};
    const SourceTexts withTranscript = sourceTexts(*s.h->state(), seq->id, typed, s.transcripts.get());
    CHECK(withTranscript.texts[0] == "[bleep]");
    CHECK(withTranscript.texts[1] == "Thanks for having me.");
    const SourceTexts without = sourceTexts(*s.h->state(), seq->id, typed, nullptr); // no transcript at all
    CHECK(without.texts[0] == "[bleep]");
    CHECK(without.texts[1] == "Thanks for having me.");
    // Typed with the recognised spelling: only the redacted word goes.
    typed.cues[0].text = "Hi Bob, welcome!";
    CHECK(sourceTexts(*s.h->state(), seq->id, typed, s.transcripts.get()).texts[0] == "Hi Bob, [bleep]!");
}

// Review wave 5, A5-P1-5: a redaction edit rebuilt every cue over the touched speech from the source transcript - the
// Thai track got English words ("[bleep] for having me."), and a later Thai dub spoke them. Only cues made from the
// transcript are rebuilt now; a translated cue masks the redacted word where it can find it, else as a whole.
TEST_CASE("A redaction never writes source-language words into a translated caption track", "[ai][global][translate][privacy]") {
    Studio s;
    s.h->ok("captions.generateFromTranscript", Json{{"maxCharsPerLine", 40}});
    s.h->ok("captions.translate", Json{{"languages", Json::array({"th"})}, {"provider", "local"}});
    REQUIRE(s.tracks().size() == 2);
    const auto thaiBefore = s.tracks()[1];
    REQUIRE(thaiBefore.language == "th");
    // Bleep "Thanks" (frames 62-67).
    s.h->ok("redaction.add", Json{{"media", "A"}, {"source", Json{{"start", frames(62).value}, {"end", frames(67).value}}}, {"audio", "bleep"}});
    const auto tracks = s.tracks();
    REQUIRE(tracks.size() == 2);
    const CaptionTrack& english = tracks[0];
    const CaptionTrack& thai = tracks[1];
    bool englishBleeped = false;
    for (const CaptionCue& cue : english.cues) {
        englishBleeped = englishBleeped || cue.text.find("[bleep] for having me.") != std::string::npos;
    }
    CHECK(englishBleeped); // the transcript's own track: rebuilt from the transcript, as before
    for (const CaptionCue& cue : thai.cues) {
        if (cue.start >= frames(80) || cue.end() <= frames(62)) {
            continue; // the cues over untouched speech (the fake translator marks what it does not know as "th:...")
        }
        INFO(cue.text);
        for (const char* word : {"Thanks", "for", "having", "me."}) {
            CHECK(cue.text.find(word) == std::string::npos);
        }
    }
    // The Thai cue over the new bleep was masked as a whole (its words cannot be told apart); the others kept their text.
    std::size_t masked = 0;
    for (std::size_t i = 0; i < thai.cues.size() && i < thaiBefore.cues.size(); ++i) {
        if (thai.cues[i].text != thaiBefore.cues[i].text) {
            CHECK(thai.cues[i].text == "[bleep]");
            ++masked;
        }
    }
    CHECK(masked == 1);
}
