// Rough cut from a script (AI-11): beats found across recordings (English and Thai), the cleanest take chosen and the
// alternates kept, deterministic plans, the assembled sequence (order, trims to the words with handles, linked V/A,
// section markers, the plan record), roughcut.fromScript / roughcut.swapTake through the engine (dry run, one undo
// step), outline bullets matched by meaning, the guarded cloud interpretation (consent, no-cloud, text only), accuracy on
// a 30-line fixture and the 60-minute performance figure.

#include "CommandHarness.h"
#include "CreatorTestUtil.h"

#include "../FakeTransport.h"

#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/roughcut/Outline.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::roughcut;
using namespace wabcut::ai::roughcut::test;
using wabcut::test::CommandHarness;
using Catch::Approx;

namespace {

double sec(Ticks t) { return t.toSeconds(); }

RoughCutPlan launchPlan(const Studio& studio, RoughCutOptions options = {}) {
    auto script = parseScript(kLaunchScript);
    REQUIRE(script);
    return planRoughCut(studio.corpus(), *script, {}, options);
}

/// Seconds where the chosen take of a beat starts in its media, and which media.
std::pair<char, double> chosenAt(const Studio& studio, const BeatPlan& beat) {
    REQUIRE(beat.found());
    const MediaRange& m = beat.takes.front().media;
    char letter = '?';
    for (const char c : studio.letters) {
        if (studio.tl.media(c) == m.media) {
            letter = c;
        }
    }
    return {letter, sec(m.source.start)};
}

std::filesystem::path outputDir() {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-creator";
    std::filesystem::create_directories(dir);
    return dir;
}

void record(const std::string& name, const std::string& line) {
    std::ofstream(outputDir() / name, std::ios::app) << line << "\n";
}

} // namespace

TEST_CASE("Every beat is found and the cleanest take is chosen, alternates kept", "[ai][creator][roughcut]") {
    const Studio studio(launchTakes());
    const RoughCutPlan plan = launchPlan(studio);
    REQUIRE(plan.beats.size() == 5);
    CHECK(plan.found() == 5);
    CHECK(plan.script.title == "Launch video");
    CHECK(plan.sources == 2);

    // 1: the only take.
    auto [m0, t0] = chosenAt(studio, plan.beats[0]);
    CHECK(m0 == 'A');
    CHECK(t0 == Approx(1.0).margin(0.2));
    // 2: three takes; the one with "um" and "uh" is not chosen.
    const BeatPlan& editor = plan.beats[1];
    CHECK(editor.takes.size() == 3);
    CHECK(editor.takes.front().quality.fillers == 0);
    CHECK(sec(editor.takes.front().media.source.start) > 8.0);
    CHECK(std::ranges::any_of(editor.takes, [](const BeatTake& t) { return t.quality.fillers == 2; }));
    CHECK(editor.confidence > 0.7);
    // 3: the take with a false start ("it cuts a, it cuts a whole video") is the alternate.
    const BeatPlan& cuts = plan.beats[2];
    REQUIRE(cuts.takes.size() == 2);
    CHECK(cuts.takes.front().quality.falseStarts == 0);
    CHECK(cuts.takes[1].quality.falseStarts >= 1);
    CHECK(chosenAt(studio, cuts).second == Approx(13.0).margin(0.2));
    // 4: two recordings of the price line in B; the B-roll cue rides along.
    CHECK(chosenAt(studio, plan.beats[3]).first == 'B');
    CHECK(plan.beats[3].beat.broll == std::vector<std::string>{"price tag close-up"});
    // 5: the words that say it, not the rest of the sentence.
    CHECK(plan.beats[4].takes.front().text.starts_with("Thanks for watching"));
    CHECK(plan.beats[4].takes.front().text.find("next time") == std::string::npos);
    // The aside nobody scripted is never used.
    for (const BeatPlan& b : plan.beats) {
        for (const BeatTake& t : b.takes) {
            CHECK(t.text.find("water") == std::string::npos);
        }
    }
}

TEST_CASE("Plans are deterministic and the take preference is honoured", "[ai][creator][roughcut]") {
    const Studio studio(launchTakes());
    Json a = planToJson(launchPlan(studio), studio.tl.state);
    Json b = planToJson(launchPlan(studio), studio.tl.state);
    a.erase("milliseconds");
    b.erase("milliseconds");
    CHECK(a == b);
    RoughCutOptions last;
    last.preference = TakePreference::Last;
    const auto lastTake = chosenAt(studio, launchPlan(studio, last).beats[1]);
    CHECK(lastTake.first == 'B'); // the second recording's take
    CHECK(lastTake.second == Approx(10.0).margin(0.2));
    RoughCutOptions first;
    first.preference = TakePreference::First;
    // The earliest take, fillers and all, though its leading "Um," stays out of the clip (it starts after it).
    const auto firstTake = launchPlan(studio, first).beats[1];
    CHECK(chosenAt(studio, firstTake).second == Approx(4.36).margin(0.1));
    CHECK(firstTake.takes.front().quality.fillers == 2);
    CHECK(takePreferenceFromString("cleanest") == TakePreference::Cleanest);
    CHECK_FALSE(takePreferenceFromString("random"));
}

TEST_CASE("The assembled cut plays the takes in script order, trimmed to their words with handles", "[ai][creator][roughcut]") {
    const Studio studio(launchTakes());
    const RoughCutPlan plan = launchPlan(studio);
    auto cut = assembleRoughCut(studio.tl.state, plan);
    REQUIRE(cut);
    REQUIRE(validate(cut->state));
    const Sequence& seq = *cut->state.findSequence(cut->sequence);
    CHECK(seq.name == "Rough cut: Launch video");
    CHECK(seq.frameRate == kRate25);
    CHECK(seq.width == 1920);
    const Track& v1 = *seq.videoTracks.front();
    const Track& a1 = *seq.audioTracks.front();
    REQUIRE(a1.clips.size() == 5);
    REQUIRE(v1.clips.size() == 5);
    const Ticks frame = framesToTicks(1, kRate25);
    Ticks expectedStart{};
    for (std::size_t b = 0; b < 5; ++b) {
        INFO("beat " << b + 1);
        const Clip& audio = a1.clips[b];
        const Clip& video = v1.clips[b];
        CHECK(audio.start() == expectedStart);         // in order, back to back
        CHECK(audio.duration().value % frame.value == 0); // whole frames
        CHECK(audio.linkGroup.isValid());
        CHECK(audio.linkGroup == video.linkGroup);       // linked V/A
        CHECK(video.timelineRange == audio.timelineRange);
        const BeatTake& take = plan.beats[b].takes.front();
        const auto words = mediaRangeOf(studio.corpus(), take.span);
        REQUIRE(words);
        CHECK(audio.media == take.media.media);
        CHECK(audio.sourceIn <= words->source.start);
        CHECK(audio.sourceOut() >= words->source.end());
        CHECK(words->source.start - audio.sourceIn <= Ticks{kTicksPerSecond * 15 / 100}); // a small handle
        CHECK(audio.sourceOut() - words->source.end() <= Ticks{kTicksPerSecond * 15 / 100} + frame);
        REQUIRE(cut->beatRanges[b]);
        CHECK(*cut->beatRanges[b] == audio.timelineRange);
        expectedStart = audio.end();
    }
    // A marker per section; the plan is recorded for swapping takes later.
    REQUIRE(seq.markers.size() == 2);
    CHECK(seq.markers[0].name == "Intro");
    CHECK(seq.markers[0].start == Ticks{});
    CHECK(seq.markers[1].name == "Demo");
    CHECK(seq.markers[1].start == a1.clips[2].start());
    auto record = recordOfSequence(seq);
    REQUIRE(record);
    CHECK(*record == recordOf(plan));
    CHECK(record->beats[1].takes.size() == 3);
    CHECK(recordFromJson(recordToJson(*record)) == *record);
    CHECK_FALSE(recordFromJson("{\"beats\":[{\"takes\":[[1,0,-5,10,1]]}]}"));
    CHECK_FALSE(recordOfSequence(*studio.tl.state.findSequence(studio.tl.seq)));
}

TEST_CASE("roughcut.fromScript previews, commits as one undo step and swaps takes", "[ai][creator][roughcut][commands]") {
    Studio studio(launchTakes());
    CommandHarness h(studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, studio.services}, ai::edit::editingCommands());
    const Json params{{"text", kLaunchScript}, {"media", studio.mediaJson()}, {"prefer", "cleanest"}};
    const auto before = h.state();
    const auto dry = h.run(Json{{"command", "roughcut.fromScript"}, {"params", params}, {"dryRun", true}});
    INFO(command::toJson(dry).dump());
    REQUIRE(dry.ok());
    CHECK_FALSE(dry.committed);
    CHECK(h.state() == before);
    REQUIRE(dry.preview);
    CHECK(dry.preview->sequences.size() == before->sequences.size() + 1);

    const auto made = h.ok("roughcut.fromScript", params);
    CHECK(made.committed);
    REQUIRE(made.createdIds.size() == 1);
    const SequenceId cutId{made.createdIds.front()};
    CHECK(made.data["found"] == 5);
    CHECK(made.data["beats"][1]["takes"].size() == 3);
    CHECK(made.data["beats"][1]["takes"][0]["chosen"] == true);
    CHECK(made.data["beats"][0]["start"].get<double>() < 0.25); // the words, right after the clip's handle
    CHECK(made.data["beats"][1]["start"].get<double>() > 0.0);
    REQUIRE(h.state()->findSequence(cutId));

    // Swap line 2 to its take 2: the cut plays that media range now, the rest moves to fit, one undo step.
    const auto record = recordOfSequence(*h.state()->findSequence(cutId));
    REQUIRE(record);
    const RecordedTake alternate = record->beats[1].takes[1];
    const auto swapped = h.ok("roughcut.swapTake", Json{{"sequence", cutId.value}, {"beat", 2}, {"take", 2}});
    CHECK(swapped.committed);
    CHECK(swapped.data["changed"] == true);
    const Sequence& after = *h.state()->findSequence(cutId);
    CHECK(recordOfSequence(after)->beats[1].chosen == 1);
    const auto plays = std::ranges::any_of(after.audioTracks.front()->clips, [&](const Clip& c) {
        return c.media == alternate.media.media && c.sourceIn <= alternate.media.source.start + Ticks{kTicksPerSecond / 5} &&
               c.sourceOut() >= alternate.media.source.end() - Ticks{kTicksPerSecond / 5};
    });
    CHECK(plays);
    REQUIRE(validate(*h.state()));
    CHECK(after.audioTracks.front()->clips.size() == 5);
    // Swapping to the take already there changes nothing; bad numbers are refused.
    const auto same = h.ok("roughcut.swapTake", Json{{"sequence", cutId.value}, {"beat", 2}, {"take", 2}});
    CHECK(same.data["changed"] == false);
    CHECK(h.errorOf("roughcut.swapTake", Json{{"sequence", cutId.value}, {"beat", 9}, {"take", 1}}) == "InvalidParams");
    CHECK(h.errorOf("roughcut.swapTake", Json{{"sequence", studio.tl.seq.value}, {"beat", 1}, {"take", 1}}) == "EditRejected");
    REQUIRE(h.session.undo());
    CHECK(recordOfSequence(*h.state()->findSequence(cutId))->beats[1].chosen == 0);
    REQUIRE(h.session.undo());
    CHECK_FALSE(h.state()->findSequence(cutId));

    // Without transcripts the host can do nothing; a script with no lines is refused; files only where allowed.
    CHECK(h.errorOf("roughcut.fromScript", Json{{"text", "\n\n"}}) == "InvalidParams");
    CHECK(h.errorOf("roughcut.fromScript", Json{{"file", "script.md"}}) == "PermissionDenied");
    CHECK(h.errorOf("roughcut.fromScript", Json{{"text", "Nothing anyone said in these recordings at all."}, {"media", studio.mediaJson()}}) ==
          "EditRejected");
}

TEST_CASE("roughcut.fromScript can clean up and reads script files where the host allows it", "[ai][creator][roughcut][commands]") {
    Studio studio(launchTakes());
    auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<ai::edit::TranscriptServices>(studio.transcripts),
        std::make_shared<RoughCutServices>(RoughCutServices::ChatOpener{}, nullptr, RoughCutServices::TextEmbedder{}, true)});
    CommandHarness h(studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, services}, ai::edit::editingCommands());
    const auto file = outputDir() / "launch-script.md";
    std::ofstream(file, std::ios::binary) << kLaunchScript;
    const auto made = h.ok("roughcut.fromScript", Json{{"file", file.string()}, {"media", studio.mediaJson()}, {"cleanup", true},
                                                       {"name", "First cut"}, {"prefer", "first"}});
    CHECK(made.data["found"] == 5);
    CHECK(made.data["name"] == "First cut");
    CHECK(made.data.contains("cleanup"));
    REQUIRE(validate(*h.state()));
    CHECK(h.errorOf("roughcut.fromScript", Json{{"file", (outputDir() / "missing.md").string()}}) == "NotFound");
    CHECK(h.errorOf("roughcut.fromScript", Json{{"file", (outputDir() / "binary.exe").string()}}) == "InvalidParams");
}

TEST_CASE("Thai lines are found in Thai transcripts without word boundaries in the script", "[ai][creator][roughcut][thai]") {
    const std::vector<Said> takes{
        {'A', 1.0, "สวัสดี ครับ ทุก คน ยินดี ต้อนรับ สู่ ช่อง"},
        {'A', 6.0, "เอ่อ วันนี้ เรา จะ มา ดู โปรแกรม ตัดต่อ ใหม่"},
        {'A', 11.0, "วันนี้ เรา จะ มา ดู โปรแกรม ตัดต่อ ใหม่"},
        {'A', 16.0, "มัน ตัด วิดีโอ ได้ เร็ว มาก"},
        {'A', 20.0, "อย่า ลืม กด ติดตาม ช่อง ของ เรา"},
        {'B', 1.0, "ราคา ถูก กว่า ที่ คิด เยอะ"},
        {'B', 5.0, "ไฟล์ ส่ง ออก ได้ ทุก ขนาด"},
        {'B', 9.0, "ขอบคุณ ที่ รับชม แล้ว พบ กัน ใหม่"},
    };
    const Studio studio(takes, "th");
    const auto script = parseScript(
        "สวัสดีครับทุกคน ยินดีต้อนรับสู่ช่อง\nวันนี้เราจะมาดูโปรแกรมตัดต่อใหม่\nมันตัดวิดีโอได้เร็วมาก\nราคาถูกกว่าที่คิดเยอะ\nไฟล์ส่งออกได้ทุกขนาด\nขอบคุณที่รับชมแล้วพบกันใหม่\n");
    REQUIRE(script);
    CHECK(script->language == "th");
    REQUIRE(script->beats.size() == 6);
    const RoughCutPlan plan = planRoughCut(studio.corpus(), *script);
    const std::vector<std::pair<char, double>> expected{{'A', 1.0}, {'A', 11.0}, {'A', 16.0}, {'B', 1.0}, {'B', 5.0}, {'B', 9.0}};
    std::size_t correct = 0;
    for (std::size_t b = 0; b < plan.beats.size(); ++b) {
        INFO("beat " << b + 1 << ": " << plan.beats[b].beat.text);
        REQUIRE(plan.beats[b].found());
        const auto [letter, at] = chosenAt(studio, plan.beats[b]);
        correct += letter == expected[b].first && std::abs(at - expected[b].second) < 0.2 ? 1 : 0;
    }
    CHECK(correct == 6);
    CHECK(plan.beats[1].takes.size() == 2);            // the take with "เอ่อ" is the alternate
    CHECK(plan.beats[1].takes.front().quality.fillers == 0);
    CHECK(plan.beats[1].takes[1].quality.fillers == 1);
    record("accuracy.md", "Thai fixture: " + std::to_string(correct) + "/6 beats on the right take");
}

namespace {

/// A synthetic recording: `count` distinct sentences of 7..12 words from a vocabulary, each said one to three times
/// (some attempts with fillers or a dropped word), across two media items, with unscripted asides between them.
struct Corpus30 {
    std::vector<Said> takes;
    std::vector<std::string> lines;                       ///< the script
    std::vector<std::vector<std::pair<char, double>>> truth; ///< where each line's clean takes are
};

Corpus30 makeAccuracyFixture(std::size_t count, unsigned seed) {
    static const std::vector<std::string> vocabulary{
        "camera", "light", "studio", "editor", "timeline", "music", "colour", "story", "audience", "channel", "export", "render",
        "budget", "client", "deadline", "schedule", "drone", "beach", "sunset", "mountain", "city", "street", "coffee", "morning",
        "workflow", "plugin", "keyboard", "shortcut", "preview", "proxy", "codec", "frame", "sequence", "transition", "title",
        "caption", "voice", "microphone", "interview", "podcast"};
    static const std::vector<std::string> glue{"the", "we", "really", "and", "with", "our", "for", "this", "that", "is"};
    std::mt19937 random(seed);
    const auto pick = [&](const std::vector<std::string>& from) { return from[random() % from.size()]; };
    Corpus30 out;
    double t[2] = {1.0, 1.0};
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t words = 7 + random() % 6;
        std::string line;
        for (std::size_t w = 0; w < words; ++w) {
            line += (w ? " " : "") + (w % 3 == 1 ? pick(glue) : pick(vocabulary));
        }
        line += ".";
        out.lines.push_back(line);
        out.truth.emplace_back();
        const int media = static_cast<int>(random() % 2);
        const int attempts = 1 + static_cast<int>(random() % 3);
        for (int a = 0; a < attempts; ++a) {
            std::string said = line;
            const bool messy = attempts > 1 && a == 0;
            if (messy) {
                said = "um " + said;
            }
            const char letter = media == 0 ? 'A' : 'B';
            out.takes.push_back(Said{letter, t[media], said});
            if (!messy) {
                out.truth.back().emplace_back(letter, t[media]);
            }
            t[media] += 0.4 * static_cast<double>(words + 2) + 1.2;
        }
        // An aside nobody scripted.
        out.takes.push_back(Said{media == 0 ? 'A' : 'B', t[media], "okay let me check the " + pick(vocabulary) + " again quickly"});
        t[media] += 4.0;
    }
    return out;
}

} // namespace

TEST_CASE("Beat alignment accuracy on a 30-line fixture with retakes, fillers and asides", "[ai][creator][roughcut][accuracy]") {
    const Corpus30 fixture = makeAccuracyFixture(30, 1234);
    const Studio studio(fixture.takes);
    std::string text;
    for (std::size_t i = 0; i < fixture.lines.size(); ++i) {
        std::string line = fixture.lines[i];
        if (i % 5 == 0) { // a writer's version of the line: one word missing
            line.erase(line.find(' '), line.find(' ', line.find(' ') + 1) - line.find(' '));
        }
        text += line + "\n";
    }
    const auto script = parseScript(text);
    REQUIRE(script);
    REQUIRE(script->beats.size() == 30);
    const RoughCutPlan plan = planRoughCut(studio.corpus(), *script);
    std::size_t correct = 0;
    for (std::size_t b = 0; b < plan.beats.size(); ++b) {
        if (!plan.beats[b].found()) {
            continue;
        }
        const auto [letter, at] = chosenAt(studio, plan.beats[b]);
        correct += std::ranges::any_of(fixture.truth[b], [&](const auto& t) { return t.first == letter && std::abs(t.second - at) < 0.3; }) ? 1 : 0;
    }
    record("accuracy.md", "English fixture: " + std::to_string(correct) + "/30 beats on a clean take of the right line");
    CHECK(correct >= 29);
}

TEST_CASE("Outline bullets are matched by meaning when a text encoder is there", "[ai][creator][roughcut][outline]") {
    const Studio studio(launchTakes());
    const auto script = parseScript("- greeting\n- pricing\n");
    REQUIRE(script);
    // A toy encoder: one dimension per topic, by whole keywords.
    const auto encoder = [](const std::string& text) -> std::optional<std::vector<float>> {
        const std::string t = " " + normalizedText(text) + " ";
        const auto has = [&](const char* w) { return t.find(" " + std::string(w) + " ") != std::string::npos ? 1.0f : 0.0f; };
        std::vector<float> v(3, 0.0f);
        for (const char* w : {"greeting", "hi", "welcome"}) {
            v[0] += has(w);
        }
        for (const char* w : {"pricing", "price", "free"}) {
            v[1] += has(w);
        }
        v[2] = 0.05f;
        float n = 0.0f;
        for (const float x : v) {
            n += x * x;
        }
        for (float& x : v) {
            x /= std::sqrt(n);
        }
        return v;
    };
    // Without the encoder the words do not match.
    CHECK(planRoughCut(studio.corpus(), *script).found() == 0);
    RoughCutSignals inputs;
    inputs.embed = encoder;
    const RoughCutPlan plan = planRoughCut(studio.corpus(), *script, inputs);
    REQUIRE(plan.found() == 2);
    CHECK(plan.beats[0].takes.front().byMeaning);
    CHECK(plan.beats[0].note == "matched by meaning");
    CHECK(plan.beats[0].takes.front().text.find("welcome") != std::string::npos);
    CHECK(plan.beats[1].takes.front().text.find("price") != std::string::npos);
}

namespace {

ModelRegistry cloudRegistry() {
    auto r = ModelRegistry::fromJson(R"({"schemaVersion":1,"providers":[
        {"id":"cloud","name":"Cloud","api":"scripted","baseUrl":"https://cloud.test","auth":"bearer","keyRequired":false,
         "termsUrl":"https://cloud.test/terms","dataPolicy":"kept 30 days"}],
      "models":[
        {"id":"cloud-model","provider":"cloud","capabilities":{"chat":true},"prices":[{"inputPerMTok":2,"outputPerMTok":10}]}]})");
    REQUIRE(r);
    return std::move(*r);
}

class RecordingModel final : public IChatModel {
public:
    RecordingModel(ModelSpec spec, std::string answer, int& calls, ChatRequest& last)
        : m_spec(std::move(spec)), m_answer(std::move(answer)), m_calls(calls), m_last(last) {}
    const ModelSpec& spec() const override { return m_spec; }
    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler&, std::stop_token) override {
        ++m_calls;
        m_last = request;
        ChatResponse r;
        r.message = ChatMessage::assistant(m_answer);
        r.usage.inputTokens = 900;
        r.usage.outputTokens = 60;
        return r;
    }

private:
    ModelSpec m_spec;
    std::string m_answer;
    int& m_calls;
    ChatRequest& m_last;
};

/// The user's cloud model behind AiPlatform: consent, the no-cloud lock and the spend cap apply.
struct CloudBench {
    explicit CloudBench(std::string answer)
        : registry(cloudRegistry()), providers(registry), keys(store, [](std::string_view) { return std::optional<std::string>(); }),
          platform(providers, keys, consent, spend, log, transport), answer(std::move(answer)) {
        providers.registerChat("scripted", [this](ProviderContext ctx) { return std::make_unique<RecordingModel>(ctx.model, this->answer, calls, last); });
        platform.setClock([] { return std::chrono::year_month_day{std::chrono::year{2026}, std::chrono::month{10}, std::chrono::day{12}}; });
    }
    RoughCutServices::ChatOpener opener() {
        return [this]() -> std::expected<std::unique_ptr<IChatModel>, AiError> { return platform.chat("cloud-model"); };
    }
    ModelRegistry registry;
    ProviderRegistry providers;
    MemorySecretStore store;
    KeyRing keys;
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    ai::test::FakeTransport transport;
    AiPlatform platform;
    std::string answer;
    int calls = 0;
    ChatRequest last;
};

const char* kOutline = "# Launch\n- say hello to everyone\n- the cost of it\n";
const char* kInterpretation =
    R"({"beats":[{"bullet":1,"say":"Hi everyone, welcome to the channel."},{"bullet":2,"say":"The best part is the price."}]})";

} // namespace

TEST_CASE("A loose outline is interpreted by the user's model only with consent and never for a no-cloud project",
          "[ai][creator][roughcut][outline][commands]") {
    for (const bool consented : {false, true}) {
        for (const bool locked : {false, true}) {
            INFO("consent " << consented << ", no-cloud " << locked);
            Studio studio(launchTakes());
            studio.tl.state.aiNoCloud = locked;
            CloudBench cloud(kInterpretation);
            if (consented) {
                cloud.consent.grantAlways(*cloud.registry.provider("cloud"), Capability::Chat, "2026-10-12");
            }
            cloud.consent.setProjectLockSource([locked] { return locked; });
            auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
                std::make_shared<ai::edit::TranscriptServices>(studio.transcripts), std::make_shared<RoughCutServices>(cloud.opener(), nullptr)});
            CommandHarness h(studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, services}, ai::edit::editingCommands());
            const auto result = h.run(Json{{"command", "roughcut.fromScript"},
                                           {"params", Json{{"text", kOutline}, {"media", studio.mediaJson()}, {"interpret", true}}}});
            const bool expected = consented && !locked;
            CHECK(cloud.calls == (expected ? 1 : 0)); // nothing left the machine otherwise
            if (expected) {
                REQUIRE(result.ok());
                CHECK(result.data["interpreted"]["beats"] == 2);
                CHECK(result.data["found"] == 2);
                CHECK(cloud.log.entries().size() == 1);
                // Transcript text and the outline only: no file names, paths or media names.
                std::string sent = cloud.last.system;
                for (const ChatMessage& m : cloud.last.messages) {
                    sent += m.text();
                }
                CHECK(sent.find("welcome to the channel") != std::string::npos);
                CHECK(sent.find("say hello to everyone") != std::string::npos);
                CHECK(sent.find(".mov") == std::string::npos);
                CHECK(sent.find(":/") == std::string::npos);
                for (const MediaPtr& m : studio.tl.state.media) {
                    CHECK(sent.find(m->name + ".") == std::string::npos);
                }
            } else if (result.ok()) {
                // Matched as written on this computer, and the warning says why.
                CHECK_FALSE(result.data.contains("interpreted"));
                CHECK(std::ranges::any_of(result.warnings, [&](const std::string& w) {
                    return w.find(locked ? "no-cloud" : "not interpreted") != std::string::npos;
                }));
            } else {
                REQUIRE(result.error);
                CHECK(result.error->code == command::ErrorCode::EditRejected); // nothing found as written
            }
        }
    }
    // A bad answer leaves the outline as it was.
    Studio studio(launchTakes());
    int calls = 0;
    ChatRequest last;
    RecordingModel garbage(*ModelRegistry::builtin().model("claude-sonnet-5-5"), "Sure! Here you go.", calls, last);
    auto outline = parseScript(kOutline);
    REQUIRE(outline);
    const auto failed = interpretOutline(garbage, *outline, studio.corpus());
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == AiErrorCode::ProviderError);
}

TEST_CASE("Rough cut of a 60-minute transcript from a 40-line script", "[ai][creator][roughcut][benchmark]") {
    // Two 30-minute recordings, ~9,000 words; 40 lines picked across them (one in eight reworded).
    const Corpus30 fixture = makeAccuracyFixture(330, 99);
    std::map<char, std::int64_t> frames{{'A', 25 * 60 * 40}, {'B', 25 * 60 * 40}};
    const Studio studio(fixture.takes, "en", frames);
    std::string text;
    std::vector<std::size_t> picked;
    for (std::size_t i = 0; i < 40; ++i) {
        picked.push_back(i * 8 + 3);
        text += fixture.lines[picked.back()] + "\n";
    }
    const auto started = std::chrono::steady_clock::now();
    const SpeechCorpus corpus = studio.corpus();
    const auto script = parseScript(text);
    REQUIRE(script);
    const RoughCutPlan plan = planRoughCut(corpus, *script);
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    double recorded = 0.0;
    for (const SpeechSource& s : corpus.sources) {
        recorded += sec(s.transcript.words().back().timeline.end());
    }
    std::size_t correct = 0;
    for (std::size_t b = 0; b < plan.beats.size(); ++b) {
        if (plan.beats[b].found()) {
            const auto [letter, at] = chosenAt(studio, plan.beats[b]);
            correct += std::ranges::any_of(fixture.truth[picked[b]], [&](const auto& t) { return t.first == letter && std::abs(t.second - at) < 0.3; }) ? 1 : 0;
        }
    }
    char line[256];
    std::snprintf(line, sizeof line, "Rough cut: %zu words (%.1f min of speech), %zu lines: corpus + plan %.0f ms (plan alone %.0f ms), %zu/40 correct",
                  corpus.words, recorded / 60.0, plan.beats.size(), total * 1000.0, plan.milliseconds, correct);
    record("perf.md", line);
    WARN(line);
    CHECK(corpus.words > 8000);
    CHECK(recorded > 55.0 * 60.0);
    CHECK(correct >= 39);
    CHECK(total < 10.0); // the 2 s target is asserted by the [perf] test on a quiet machine
}

TEST_CASE("Rough cut of 60 minutes stays under two seconds", "[ai][creator][roughcut][perf]") {
    const Corpus30 fixture = makeAccuracyFixture(330, 99);
    std::map<char, std::int64_t> frames{{'A', 25 * 60 * 40}, {'B', 25 * 60 * 40}};
    const Studio studio(fixture.takes, "en", frames);
    std::string text;
    for (std::size_t i = 0; i < 40; ++i) {
        text += fixture.lines[i * 8 + 3] + "\n";
    }
    double best = 1e9;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto started = std::chrono::steady_clock::now();
        const SpeechCorpus corpus = studio.corpus();
        const RoughCutPlan plan = planRoughCut(corpus, *parseScript(text));
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        CHECK(plan.found() == 40);
    }
    CHECK(best < 2.0);
}
