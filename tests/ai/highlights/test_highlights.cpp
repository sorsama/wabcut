// G.2 gameplay highlights: the measurements, chat logs and marker files, the fusion into ranked moments with reasons
// (accuracy on synthetic gameplay with scripted moments and decoys), and highlights.find / toReel / toShort through
// the engine.

#include "../AgentTestUtil.h"
#include "../EditTestUtil.h"
#include "CommandHarness.h"
#include "NestedTestUtil.h"
#include "SyntheticGameplay.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/agent/ToolRouter.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/highlights/HighlightCommands.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::test;
using namespace wabcut::ai::highlights;
using command::Json;

namespace {

Ticks secs(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::vector<float> tone(double seconds, double hz, double amplitude, double pulseHz = 0.0) {
    const auto n = static_cast<std::size_t>(seconds * kAudioRate);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kAudioRate;
        double v = 0.0;
        for (int h = 1; h <= 6; ++h) {
            v += std::sin(2.0 * 3.14159265358979 * hz * h * t) / h;
        }
        double env = 1.0;
        if (pulseHz > 0.0) {
            env = std::pow(std::sin(3.14159265358979 * pulseHz * t), 2.0);
        }
        out[i] = static_cast<float>(amplitude * env * v * 0.5);
    }
    return out;
}

AudioFeatures featuresOf(const std::vector<float>& samples) {
    AudioFeatureExtractor x;
    // Odd chunk sizes: the extractor must not care.
    for (std::size_t i = 0; i < samples.size(); i += 1234) {
        x.add(std::span<const float>(samples.data() + i, std::min<std::size_t>(1234, samples.size() - i)));
    }
    return x.finish();
}

/// The synthetic recording, measured once (the measurements are what a decoder would produce).
const Measurement& gameplayMeasurement() {
    static const Measurement m = [] {
        const gameplay::Script script = gameplay::defaultScript();
        Measurement out;
        out.audio.push_back(featuresOf(gameplay::gameAudio(script)));
        out.audio.push_back(featuresOf(gameplay::micAudio(script)));
        VisualFeatureExtractor v;
        for (const ai::analysis::AnalysisFrame& f : gameplay::frames(script)) {
            v.add(f);
        }
        out.video = v.finish();
        out.hints = gameplay::markers(script);
        return out;
    }();
    return m;
}

MediaSignals gameplaySignals(bool chat, bool hints, bool visual = true) {
    const gameplay::Script script = gameplay::defaultScript();
    const Measurement& m = gameplayMeasurement();
    MediaSignals s;
    const std::vector<AudioRole> roles = classifyStreams({&m.audio[0], &m.audio[1]});
    addAudio(s, m.audio[0], roles[0]);
    addAudio(s, m.audio[1], roles[1]);
    if (visual) {
        addVideo(s, *m.video);
    }
    if (hints) {
        addHints(s, m.hints);
    }
    if (chat) {
        auto log = parseChatLog(gameplay::chatJson(script));
        REQUIRE(log);
        addChat(s, *log, ChatOptions{});
    }
    return s;
}

} // namespace

TEST_CASE("Audio features hear loudness and a voice with its pitch and laughter-like bursts", "[ai][highlights]") {
    SECTION("silence") {
        const AudioFeatures f = featuresOf(std::vector<float>(kAudioRate * 2, 0.0f));
        REQUIRE(f.size() == 20);
        CHECK(f.levelDb[10] == kSilenceDb);
        CHECK(f.voiced[10] == 0.0f);
    }
    SECTION("a voice at 150 Hz") {
        const AudioFeatures f = featuresOf(tone(2.0, 150.0, 0.1));
        REQUIRE(f.size() == 20);
        CHECK(f.voiced[10] > 0.8f);
        CHECK(f.pitchHz[10] == Catch::Approx(150.0).margin(6.0));
        CHECK(f.levelDb[10] == Catch::Approx(-27.3).margin(1.5)); // 6 harmonics at 0.05 / h
        CHECK(f.burst[15] < 0.3f);
    }
    SECTION("noise is not voiced") {
        std::vector<float> noise(kAudioRate * 2);
        std::uint32_t state = 1;
        for (float& v : noise) {
            state = state * 1664525u + 1013904223u;
            v = (static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 0.2f;
        }
        const AudioFeatures f = featuresOf(noise);
        CHECK(f.voiced[10] < 0.2f);
    }
    SECTION("a laugh pulses at 5 Hz") {
        const AudioFeatures f = featuresOf(tone(2.0, 170.0, 0.2, 5.0));
        CHECK(f.burst[15] > 0.5f);
    }
}

TEST_CASE("Visual features see motion and a flash and a picture change", "[ai][highlights]") {
    VisualFeatureExtractor x;
    const auto frame = [](int index, int value, bool noisy) {
        ai::analysis::AnalysisFrame f;
        f.pts = Ticks{static_cast<std::int64_t>(index) * (kTicksPerSecond / 30)};
        f.duration = Ticks{kTicksPerSecond / 30};
        f.width = 32;
        f.height = 18;
        f.rgb.assign(32 * 18 * 3, static_cast<std::uint8_t>(value));
        if (noisy) {
            for (std::size_t i = 0; i < f.rgb.size(); ++i) {
                f.rgb[i] = static_cast<std::uint8_t>((i * 7919u + static_cast<std::size_t>(index) * 104729u) % 256u);
            }
        }
        return f;
    };
    for (int i = 0; i < 90; ++i) { // 3 s: still, a flash at 1 s, then action from 2 s
        x.add(frame(i, i == 30 || i == 31 ? 250 : 60, i >= 60));
    }
    const VisualFeatures v = x.finish();
    REQUIRE(v.size() == 30);
    CHECK(v.motion[5] == 0.0f);
    CHECK(v.flash[10] > 0.5f);
    CHECK(v.flash[5] == 0.0f);
    CHECK(v.cut[20] == 1.0f); // still to noise: the picture changed completely
    CHECK(v.motion[25] > 0.1f);
}

TEST_CASE("Chat logs from Twitch and YouTube and chat-downloader read the same", "[ai][highlights]") {
    const std::string twitch = R"({"comments":[{"content_offset_seconds":12.5,"message":{"body":"KEKW that was close",
        "fragments":[{"text":"KEKW","emoticon":{"emoticon_id":"1"}},{"text":" that was close","emoticon":null}]}},
        {"content_offset_seconds":3.0,"message":{"body":"hi"}}]})";
    auto t = parseChatLog(twitch);
    REQUIRE(t);
    CHECK(t->format == "twitch");
    REQUIRE(t->messages.size() == 2);
    CHECK(t->messages[0].seconds == 3.0); // sorted
    CHECK(t->messages[1].emotes == std::vector<std::string>{"KEKW"});

    const std::string youtube =
        R"({"replayChatItemAction":{"actions":[{"addChatItemAction":{"item":{"liveChatTextMessageRenderer":{"message":{"runs":[{"text":"no way "},{"emoji":{"emojiId":"x","shortcuts":[":fire:"]}}]}}}}}],"videoOffsetTimeMsec":"61500"}})"
        "\n"
        R"({"replayChatItemAction":{"actions":[{"addChatItemAction":{"item":{"liveChatTextMessageRenderer":{"message":{"runs":[{"text":"gg"}]}}}}}],"videoOffsetTimeMsec":"70000"}})"
        "\n";
    auto y = parseChatLog(youtube);
    REQUIRE(y);
    CHECK(y->format == "youtube");
    REQUIRE(y->messages.size() == 2);
    CHECK(y->messages[0].seconds == 61.5);
    CHECK(y->messages[0].emotes == std::vector<std::string>{"fire"});

    auto d = parseChatLog(R"([{"time_in_seconds":5,"message":"Pog","emotes":[{"name":"Pog"}]}])");
    REQUIRE(d);
    CHECK(d->format == "chat-downloader");
    CHECK(d->messages[0].emotes == std::vector<std::string>{"Pog"});

    CHECK_FALSE(parseChatLog("not json at all"));
    CHECK_FALSE(parseChatLog(R"({"something":"else"})"));
}

TEST_CASE("Marker files read as text and CSV and JSON", "[ai][highlights]") {
    auto text = parseMarkerFile("# my markers\n1:02:03.5 clutch\n00:45 - ace\n12.25\tnice\n");
    REQUIRE(text);
    REQUIRE(text->size() == 3);
    CHECK((*text)[0].at == secs(12.25));
    CHECK((*text)[0].label == "nice");
    CHECK((*text)[1].at == secs(45.0));
    CHECK((*text)[1].label == "ace");
    CHECK((*text)[2].at == secs(3723.5));
    CHECK((*text)[2].source == "marker file");

    auto csv = parseMarkerFile("time,label\n10.5,\"first blood\"\n20,second\n");
    REQUIRE(csv);
    REQUIRE(csv->size() == 2);
    CHECK((*csv)[0].label == "first blood");

    auto json = parseMarkerFile(R"({"markers":[{"time":"0:30","label":"a"},{"seconds":40,"name":"b"}]})");
    REQUIRE(json);
    REQUIRE(json->size() == 2);
    CHECK((*json)[0].at == secs(30.0));
    CHECK((*json)[1].label == "b");

    CHECK_FALSE(parseMarkerFile("nothing here\n"));
}

TEST_CASE("The most voice-like of several streams is the mic", "[ai][highlights]") {
    const Measurement& m = gameplayMeasurement();
    const std::vector<AudioRole> roles = classifyStreams({&m.audio[0], &m.audio[1]});
    CHECK(roles == std::vector<AudioRole>{AudioRole::Game, AudioRole::Mic});
    CHECK(classifyStreams({&m.audio[0]}) == std::vector<AudioRole>{AudioRole::Mixed});
    CHECK(voicedShare(m.audio[1]) > voicedShare(m.audio[0]));
}

TEST_CASE("Synthetic gameplay: the scripted moments are found and the decoys mostly not", "[ai][highlights]") {
    const gameplay::Script script = gameplay::defaultScript();
    const std::vector<Ticks> truth = gameplay::eventTimes(script);
    FindOptions options;
    options.maxMoments = truth.size();
    struct Run {
        const char* name;
        bool chat;
        bool hints;
        bool visual;
        double minRecall;
    };
    for (const Run& run : {Run{"all signals", true, true, true, 0.85}, Run{"audio + picture", false, false, true, 0.75},
                           Run{"audio only", false, false, false, 0.65}}) {
        const MediaSignals s = gameplaySignals(run.chat, run.hints, run.visual);
        const std::vector<Moment> moments = findMoments(s, secs(script.seconds), options);
        const Accuracy a = measure(moments, truth);
        // The numbers docs/ai.md quotes ("Gameplay highlights": accuracy on the synthetic fixture).
        std::printf("highlights accuracy (%s): precision %.2f, recall %.2f (%zu found, %zu true, %zu false, %zu missed)\n", run.name,
                    a.precision, a.recall, moments.size(), a.truePositives, a.falsePositives, a.falseNegatives);
        INFO(run.name << ": precision " << a.precision << ", recall " << a.recall);
        CHECK(a.recall >= run.minRecall);
        CHECK(a.precision >= run.minRecall);
    }
}

TEST_CASE("A moment says why: the reasons name the signals that fired", "[ai][highlights]") {
    const MediaSignals s = gameplaySignals(true, true);
    FindOptions options;
    options.maxMoments = 20;
    const std::vector<Moment> moments = findMoments(s, secs(480.0), options);
    REQUIRE_FALSE(moments.empty());
    // The first scripted event (30 s) has every signal.
    const auto first = std::find_if(moments.begin(), moments.end(), [](const Moment& m) { return m.range.start <= secs(30.0) && m.range.end() > secs(30.0); });
    REQUIRE(first != moments.end());
    std::string all;
    for (const std::string& r : first->reasons) {
        all += r + "; ";
    }
    INFO(all);
    CHECK(all.find("game audio +") != std::string::npos);
    CHECK((all.find("you shouted") != std::string::npos || all.find("your voice") != std::string::npos));
    CHECK(all.find("chat") != std::string::npos);
    CHECK(first->confidence >= 0.75);
    CHECK(confidenceLabel(first->confidence) == "high");
    CHECK(first->levels.at("game") > 0.5);
    CHECK(first->peak >= first->range.start);
    CHECK(first->peak < first->range.end());
    CHECK(first->range.duration >= secs(6.0));
    CHECK(first->range.duration <= secs(18.1));
    // The marked event (75 s): its chapter marker is a reason.
    const auto marked = std::find_if(moments.begin(), moments.end(), [](const Moment& m) { return m.range.start <= secs(75.0) && m.range.end() > secs(75.0); });
    REQUIRE(marked != moments.end());
    CHECK(std::any_of(marked->reasons.begin(), marked->reasons.end(), [](const std::string& r) { return r.find("chapter marker 'Clip 1'") != std::string::npos; }));
    // No two moments overlap.
    std::vector<Moment> byTime = moments;
    std::sort(byTime.begin(), byTime.end(), [](const Moment& a, const Moment& b) { return a.range.start < b.range.start; });
    for (std::size_t i = 1; i < byTime.size(); ++i) {
        CHECK(byTime[i - 1].range.end() <= byTime[i].range.start);
    }
}

TEST_CASE("A lone marker makes a moment and measuring counts each event once", "[ai][highlights]") {
    MediaSignals s;
    s.length = 1200;
    s.game.assign(1200, 0.0f);
    s.gameRiseDb.assign(1200, 0.0f);
    addHints(s, {Hint{secs(60.0), "clutch", "marker file"}});
    const std::vector<Moment> moments = findMoments(s, secs(120.0));
    REQUIRE(moments.size() == 1);
    CHECK(moments[0].range.start < secs(60.0));
    CHECK(moments[0].title == "Marked: clutch");
    const Accuracy a = measure(moments, {secs(55.0), secs(56.0)});
    CHECK(a.truePositives == 1);
    CHECK(a.falseNegatives == 1);
    CHECK(a.precision == 1.0);
    CHECK(a.recall == 0.5);
}

TEST_CASE("Highlight measurements round-trip through the cache's JSON", "[ai][highlights]") {
    const Measurement& m = gameplayMeasurement();
    auto audio = audioFeaturesFromJson(toJson(m.audio[1]));
    REQUIRE(audio);
    REQUIRE(audio->size() == m.audio[1].size());
    CHECK(audio->levelDb[100] == Catch::Approx(m.audio[1].levelDb[100]).margin(0.01));
    CHECK(audio->pitchHz[100] == Catch::Approx(m.audio[1].pitchHz[100]).margin(0.1));
    auto video = visualFeaturesFromJson(toJson(*m.video));
    REQUIRE(video);
    CHECK(video->size() == m.video->size());
    CHECK_FALSE(audioFeaturesFromJson(toJson(*m.video)));
    CHECK_FALSE(visualFeaturesFromJson("{}"));
}

// --- Commands ------------------------------------------------------------------------------------------------------

namespace {

constexpr std::int64_t kFrames = 480 * 25;

/// V1/A1 play the synthetic recording A (8 minutes); M is music.
Timeline gameplayTimeline() {
    const std::string clip = "A" + std::string(static_cast<std::size_t>(kFrames - 1), 'a');
    Timeline tl("V1 tgt |" + clip + "|\nA1 tgt |" + clip + "|\n",
                {{'A', kFrames}, {'M', 120 * 25}});
    tl.linkMatching();
    return tl;
}

ai::analysis::BeatAnalysis musicBeats() {
    ai::analysis::BeatAnalysis b;
    b.bpm = 120.0;
    b.confidence = 0.9f;
    b.tracker = "dsp-1";
    for (int k = 0; k < 240; ++k) {
        b.beats.push_back(secs(0.25 + 0.5 * k));
        if (k % 4 == 0) {
            b.downbeats.push_back(secs(0.25 + 0.5 * k));
        }
    }
    b.duration = secs(120.0);
    return b;
}

struct Fixture {
    Fixture()
        : highlights(std::make_shared<MapHighlightProvider>()), analysisProvider(std::make_shared<ai::analysis::MapAnalysisProvider>()),
          h(gameplayTimeline(), policy(highlights, analysisProvider), ai::edit::editingCommands()) {
        highlights->add(h.tl.media('A'), gameplayMeasurement());
        analysisProvider->addBeats(h.tl.media('M'), musicBeats());
    }
    static command::EnginePolicy policy(const std::shared_ptr<MapHighlightProvider>& p, const std::shared_ptr<ai::analysis::MapAnalysisProvider>& a) {
        auto bundle = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<HighlightServices>(p), std::make_shared<ai::analysis::AnalysisServices>(a)});
        return command::EnginePolicy{command::CapabilitySet::all(), 1024, bundle};
    }
    std::shared_ptr<MapHighlightProvider> highlights;
    std::shared_ptr<ai::analysis::MapAnalysisProvider> analysisProvider;
    CommandHarness h;
};

std::vector<const Marker*> highlightMarkers(const Sequence& seq) {
    std::vector<const Marker*> out;
    for (const Marker& m : seq.markers) {
        if (m.name.starts_with("Highlight ")) {
            out.push_back(&m);
        }
    }
    return out;
}

} // namespace

TEST_CASE("highlights.find marks the best moments of a sequence in one undo step", "[ai][highlights][commands]") {
    Fixture fx;
    const ProjectSnapshot before = fx.h.state();
    const auto dry = fx.h.run(Json{{"command", "highlights.find"}, {"params", Json{{"count", 9}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*fx.h.state() == *before);

    const auto r = fx.h.ok("highlights.find", Json{{"count", 9}, {"signals", Json::array({"game", "mic", "visual", "markers"})}});
    CHECK(r.data["timeBase"] == "timeline");
    REQUIRE(r.data["moments"].size() == 9);
    const Json& best = r.data["moments"][0];
    CHECK(best["rank"] == 1);
    CHECK(best["score"].get<double>() > 0.4);
    CHECK(best["thumbnail"] == best["peak"]);
    CHECK(best["peak"].get<double>() >= best["start"].get<double>());
    CHECK_FALSE(best["reasons"].empty());
    CHECK(best["signals"].contains("game"));
    CHECK(r.data["recordings"][0]["audioRoles"] == Json::array({"game", "mic"}));
    CHECK(r.data["markersAdded"] == 9);
    const Sequence& seq = *fx.h.state()->findSequence(fx.h.tl.seq);
    const auto markers = highlightMarkers(seq);
    REQUIRE(markers.size() == 9);
    CHECK(markers[0]->colorIndex == kMarkerColor);
    CHECK(markers[0]->comment.starts_with("score "));
    // C.15's highlight metadata: what the Highlights panel and the reel read.
    REQUIRE(markers[0]->highlight.has_value());
    CHECK(markers[0]->highlight->source == "highlights");
    CHECK(markers[0]->highlight->peak >= markers[0]->start);
    CHECK(markers[0]->highlight->peak <= markers[0]->end());
    CHECK_FALSE(markers[0]->highlight->reasons.empty());
    CHECK(markers[0]->highlight->score > 0.0);
    CHECK(fx.h.session.undoCount() == 1);
    // The recording's own moments were kept with its analyses.
    CHECK(fx.highlights->remembered.contains(fx.h.tl.media('A').value));

    // Again: the markers are replaced, not added to; a user's marker stays.
    fx.h.ok("marker.add", Json{{"time", tc(100)}, {"name", "my note"}});
    fx.h.ok("highlights.find", Json{{"count", 5}});
    const Sequence& again = *fx.h.state()->findSequence(fx.h.tl.seq);
    CHECK(highlightMarkers(again).size() == 5);
    CHECK(std::any_of(again.markers.begin(), again.markers.end(), [](const Marker& m) { return m.name == "my note"; }));
    fx.h.session.undo();
    fx.h.session.undo();
    fx.h.session.undo();
    CHECK(*fx.h.state() == *before);

    // A media item: clip markers in media time.
    const auto media = fx.h.ok("highlights.find", Json{{"media", "A"}, {"count", 3}});
    CHECK(media.data["timeBase"] == "media");
    CHECK(fx.h.state()->findMedia(fx.h.tl.media('A'))->markers.size() == 3);

    // Without the host's analysis there is nothing to measure with.
    CommandHarness bare(gameplayTimeline(), {}, ai::edit::editingCommands());
    CHECK(bare.errorOf("highlights.find", Json::object()) == "InvalidRequest");
}

TEST_CASE("highlights.importChat keeps a chat log that highlights.find then counts", "[ai][highlights][commands]") {
    Fixture fx;
    const auto dir = std::filesystem::temp_directory_path() / "wabcut-highlights-chat";
    std::filesystem::create_directories(dir);
    const auto file = dir / "chat.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << gameplay::chatJson(gameplay::defaultScript());
    }
    // A dry run keeps nothing; the import itself changes nothing in the project (no undo step).
    const auto dry = fx.h.run(Json{{"command", "highlights.importChat"}, {"params", Json{{"file", pathToUtf8(file)}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(fx.highlights->chats.empty());
    const auto imported = fx.h.ok("highlights.importChat", Json{{"file", pathToUtf8(file)}});
    CHECK(imported.data["format"] == "twitch");
    CHECK(imported.data["media"] == fx.h.tl.media('A').value);
    CHECK(imported.data["messages"].get<int>() > 200);
    CHECK(fx.h.session.undoCount() == 0);
    REQUIRE(fx.highlights->chats.contains(fx.h.tl.media('A').value));

    const auto r = fx.h.ok("highlights.find", Json{{"count", 9}});
    CHECK(r.data["chat"]["format"] == "twitch");
    CHECK(r.data["recordings"][0]["chat"] == true);
    bool chatReason = false;
    for (const Json& m : r.data["moments"]) {
        for (const Json& reason : m["reasons"]) {
            chatReason = chatReason || reason.get<std::string>().find("KEKW") != std::string::npos ||
                         reason.get<std::string>().find("chat") != std::string::npos;
        }
    }
    CHECK(chatReason);
    // Without chat as a signal it is not used.
    const auto quiet = fx.h.ok("highlights.find", Json{{"count", 9}, {"signals", Json::array({"game", "mic", "visual"})}});
    CHECK_FALSE(quiet.data.contains("chat"));
    CHECK(fx.h.errorOf("highlights.importChat", Json{{"file", pathToUtf8(dir / "missing.json")}}) == "InvalidParams");
    {
        std::ofstream out(dir / "bad.json", std::ios::binary);
        out << "{\"not\": \"a chat\"}";
    }
    CHECK(fx.h.errorOf("highlights.importChat", Json{{"file", pathToUtf8(dir / "bad.json")}}) == "InvalidParams");
    std::filesystem::remove_all(dir);
}

TEST_CASE("highlights.find reads through a multicam clip and a nested clip (0.2)", "[ai][highlights][commands][multicam]") {
    // The same recording flat, as the active angle of a two-angle multicam clip, and inside a nest: the same moments.
    Fixture fx;
    const auto moments = [](const command::CommandResult& r) {
        std::vector<std::pair<double, double>> out; // (peak, score)
        for (const Json& m : r.data["moments"]) {
            out.emplace_back(m["peak"].get<double>(), m["score"].get<double>());
        }
        return out;
    };
    const auto flat = fx.h.ok("highlights.find", Json{{"count", 5}, {"markers", false}});
    REQUIRE(flat.data["moments"].size() == 5);
    const SequenceId seq = fx.h.tl.seq;

    SECTION("a two-angle multicam clip: the active angle only") {
        REQUIRE(fx.h.session.apply("Multicam", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
            return placeMulticam(s, seq, {fx.h.tl.media('A'), fx.h.tl.media('B')}, {Ticks{}, Ticks{}}, TimeRange{Ticks{}, f(kFrames)}, Ticks{}).state;
        }));
        const ProjectSnapshot before = fx.h.state();
        const auto dry = fx.h.run(Json{{"command", "highlights.find"}, {"params", Json{{"sequence", seq.value}, {"count", 5}}}, {"dryRun", true}});
        REQUIRE(dry.ok());
        CHECK(*fx.h.state() == *before);
        const std::size_t steps = fx.h.session.undoCount();
        const auto r = fx.h.ok("highlights.find", Json{{"sequence", seq.value}, {"count", 5}});
        CHECK(moments(r) == moments(flat));
        REQUIRE(r.data["recordings"].size() == 1); // B is neither shown nor heard
        CHECK(r.data["recordings"][0]["media"] == fx.h.tl.media('A').value);
        CHECK(fx.h.session.undoCount() == steps + 1);
        const Sequence& after = *fx.h.state()->findSequence(seq);
        CHECK(highlightMarkers(after).size() == 5);
        CHECK(after.videoTracks[0]->clips.size() == 1); // the multicam clip itself is untouched
        CHECK(after.videoTracks[0]->clips[0].isNested());
        fx.h.session.undo();
        CHECK(*fx.h.state() == *before);
    }

    SECTION("a nested clip") {
        REQUIRE(fx.h.session.apply("Nest", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
            return nestEverything(s, seq).edit.state;
        }));
        REQUIRE(fx.h.state()->findSequence(seq)->videoTracks[0]->clips[0].isNested());
        const auto r = fx.h.ok("highlights.find", Json{{"sequence", seq.value}, {"count", 5}});
        CHECK(moments(r) == moments(flat));
        CHECK(r.data["recordings"].size() == 1);
        CHECK(highlightMarkers(*fx.h.state()->findSequence(seq)).size() == 5);
    }
}

TEST_CASE("A kept chat log round-trips through its JSON", "[ai][highlights]") {
    auto log = parseChatLog(gameplay::chatJson(gameplay::defaultScript()));
    REQUIRE(log);
    auto back = chatFromJson(toJson(*log, -12.5));
    REQUIRE(back);
    CHECK(back->second == -12.5);
    CHECK(back->first.format == "twitch");
    CHECK(back->first.messages == log->messages);
    CHECK_FALSE(chatFromJson("{}"));
}

TEST_CASE("A small local model is offered the highlight tools for a highlight request", "[ai][highlights][agent]") {
    ai::agent::AgentOptions options;
    options.allowed = command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence});
    const std::vector<ai::ToolSpec> tools = ai::agent::EditAgent(options).tools();
    ai::agent::RouteOptions route;
    route.maxTools = 8; // what a 6k context holds
    for (const char* request : {"make a highlight reel of my best moments", "find the highlights in this stream",
                                "cut a montage of the clutch moments to the music"}) {
        std::vector<std::string> names;
        for (const ai::ToolSpec& t : ai::agent::routeTools(tools, ai::edit::editingCommands(), request, route)) {
            names.push_back(t.name);
        }
        INFO(request);
        CHECK(std::ranges::find(names, "highlights_find") != names.end());
        if (std::string_view(request).find("reel") != std::string_view::npos || std::string_view(request).find("montage") != std::string_view::npos) {
            CHECK(std::ranges::find(names, "highlights_toReel") != names.end());
        }
    }
}

TEST_CASE("highlights.toReel cuts the marked moments into a new sequence", "[ai][highlights][commands]") {
    Fixture fx;
    fx.h.ok("highlights.find", Json{{"count", 6}});
    const ProjectSnapshot before = fx.h.state();
    const std::size_t sequencesBefore = before->sequences.size();

    SECTION("in time order, with transitions") {
        const auto r = fx.h.ok("highlights.toReel", Json{{"order", "time"}, {"transitionDuration", 0.4}});
        REQUIRE(r.createdIds.size() == 1);
        CHECK(fx.h.state()->sequences.size() == sequencesBefore + 1);
        const Sequence& reel = *fx.h.state()->findSequence(SequenceId{r.createdIds[0]});
        CHECK(reel.derivedFrom->generator == "highlights/1");
        REQUIRE(r.data["moments"].size() == 6);
        double previous = -1.0;
        for (const Json& m : r.data["moments"]) {
            CHECK(m["sourceStart"].get<double>() > previous); // time order
            previous = m["sourceStart"].get<double>();
        }
        CHECK(reel.videoTracks[0]->clips.size() == 6);
        CHECK(reel.audioTracks[0]->clips.size() == 6);
        CHECK(r.data["transitions"].get<int>() >= 5); // at least the video cuts
        CHECK(std::abs(reel.duration.toSeconds() - r.data["duration"].get<double>()) < 0.001);
        CHECK(fx.h.session.undoCount() == 2);
        fx.h.session.undo();
        CHECK(*fx.h.state() == *before);
    }
    SECTION("best first, a slow-motion peak") {
        const auto r = fx.h.ok("highlights.toReel", Json{{"order", "score"}, {"count", 3}, {"slowMotion", true}, {"transition", "none"}});
        const Sequence& reel = *fx.h.state()->findSequence(SequenceId{r.createdIds[0]});
        REQUIRE(r.data["moments"].size() == 3);
        CHECK(r.data["moments"][0]["slowMotion"] == true);
        // Each moment: before the peak, the slow part (50 %), after it.
        CHECK(reel.videoTracks[0]->clips.size() == 9);
        bool slow = false;
        for (const Clip& c : reel.videoTracks[0]->clips) {
            slow = slow || (c.speed == Speed{500, 1000});
        }
        CHECK(slow);
        CHECK(r.data["transitions"] == 0);
    }
    SECTION("cut to the beats of music") {
        const auto r = fx.h.ok("highlights.toReel", Json{{"music", "M"}, {"beatsPerCut", 4}, {"count", 4}});
        const Sequence& reel = *fx.h.state()->findSequence(SequenceId{r.createdIds[0]});
        REQUIRE(reel.beatGrid.has_value());
        CHECK(r.data["bpm"] == 120.0);
        // Every cut between moments lands on a beat of the music (beats at 0.25 + 0.5 k s; the music starts at 0).
        for (const Json& m : r.data["moments"]) {
            const double end = m["end"].get<double>();
            const double k = (end - 0.25) / 0.5;
            CHECK(std::abs(k - std::round(k)) < 0.02);
        }
        const TrackPtr& music = reel.audioTracks.back();
        CHECK(music->name == "Music");
        REQUIRE(music->clips.size() == 1);
        CHECK(music->clips.toVector()[0].timelineRange.duration == reel.duration);
    }
    SECTION("given moments, and the errors") {
        const auto r = fx.h.ok("highlights.toReel", Json{{"moments", Json::array({Json{{"start", 10.0}, {"end", 14.0}}, Json{{"start", 40.0}, {"end", 44.0}, {"title", "x"}}})},
                                                        {"handleBefore", 0.0},
                                                        {"handleAfter", 0.0},
                                                        {"transition", "none"}});
        CHECK(r.data["duration"] == 8.0);
        CHECK(fx.h.errorOf("highlights.toReel", Json{{"moments", Json::array({Json{{"start", 14.0}, {"end", 10.0}}})}}) == "InvalidParams");
        CHECK_FALSE(fx.h.errorOf("highlights.toReel", Json{{"music", "A"}, {"count", 2}}).empty()); // A is no music with beats
    }
}

TEST_CASE("highlights.toShort makes the best moment a vertical sequence", "[ai][highlights][commands]") {
    Fixture fx;
    fx.h.ok("highlights.find", Json{{"count", 3}});
    const auto r = fx.h.ok("highlights.toShort", Json{{"moment", 1}, {"captions", false}, {"normalize", false}});
    REQUIRE(r.createdIds.size() == 1);
    const Sequence& made = *fx.h.state()->findSequence(SequenceId{r.createdIds[0]});
    CHECK(made.width == 1080);
    CHECK(made.height == 1920);
    CHECK(r.data["layout"] == "verticalBlurFill"); // no facecam in this recording
    CHECK(made.name.find("Highlight Short") != std::string::npos);
    CHECK(made.derivedFrom->generator == "highlights/1");
    CHECK(fx.h.session.undoCount() == 2);
    CHECK(fx.h.errorOf("highlights.toShort", Json{{"moment", 9}}) == "InvalidParams");
    CommandHarness unmarked(gameplayTimeline(), Fixture::policy(fx.highlights, fx.analysisProvider), ai::edit::editingCommands());
    CHECK(unmarked.errorOf("highlights.toShort", Json::object()) == "EditRejected");
}

// 0.4: the Short's captions get the "Shorts" caption style (above the platforms' buttons) unless captionPreset names
// another; an unknown one is a warning and the Short keeps the default look.
TEST_CASE("highlights.toShort applies a caption preset after the captions", "[ai][highlights][commands]") {
    const Timeline tl = gameplayTimeline();
    // The streamer talks all the way through (a word every 10 frames), so the Short has words to caption.
    std::vector<ai::edit::test::W> words;
    for (std::int64_t k = 0; k + 1 < kFrames / 10; ++k)
        words.push_back({k % 2 == 0 ? "what" : "nice", static_cast<double>(k * 10), static_cast<double>(k * 10 + 8)});
    auto transcripts = ai::edit::test::provider(tl, {{'A', ai::edit::test::transcript("en", std::move(words))}});
    auto highlights = std::make_shared<MapHighlightProvider>();
    auto analysis = std::make_shared<ai::analysis::MapAnalysisProvider>();
    auto bundle = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<HighlightServices>(highlights), std::make_shared<ai::analysis::AnalysisServices>(analysis),
        std::make_shared<ai::edit::TranscriptServices>(transcripts)});
    CommandHarness h(tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, bundle}, ai::edit::editingCommands());
    highlights->add(h.tl.media('A'), gameplayMeasurement());
    h.ok("highlights.find", Json{{"count", 3}});
    const auto trackOf = [&](const command::CommandResult& r) {
        const Sequence& made = *h.state()->findSequence(SequenceId{r.createdIds.front()});
        REQUIRE(made.captionTracks.size() == 1);
        return made.captionTracks.front();
    };

    // The default: the Shorts preset, short cues. Every call names the source: the Shorts made before are sequences too.
    const auto shorts = h.ok("highlights.toShort", Json{{"sequence", h.tl.seq.value}, {"moment", 1}, {"normalize", false}, {"hookTitle", false}});
    INFO(shorts.data.dump());
    CHECK(shorts.data.value("captionPreset", std::string()) == "Shorts");
    const CaptionTrack styled = trackOf(shorts);
    CHECK(styled.style.placement == CaptionPlacement::Custom);
    CHECK(styled.style.positionY.valueAt(Ticks{}) == Catch::Approx(0.68));
    CHECK(styled.style.animation == CaptionAnimation::Pop);
    CHECK_FALSE(styled.style.box);
    for (const CaptionCue& cue : styled.cues) {
        INFO(cue.text);
        for (std::size_t from = 0;;) {
            const std::size_t at = cue.text.find('\n', from);
            const std::size_t end = at == std::string::npos ? cue.text.size() : at;
            // At most 10 characters (fix round 1: what stays one line on a rendered 1080x1920 frame), unless the line
            // is one longer word (a word is never split).
            CHECK((end - from <= 10 || cue.text.substr(from, end - from).find(' ') == std::string::npos));
            if (at == std::string::npos)
                break;
            from = at + 1;
        }
    }

    // Another preset by name (case does not matter).
    const auto clean = h.ok("highlights.toShort", Json{{"sequence", h.tl.seq.value}, {"moment", 1}, {"normalize", false}, {"hookTitle", false}, {"captionPreset", "clean"}});
    CHECK(clean.data.value("captionPreset", std::string()) == "clean");
    CHECK_FALSE(trackOf(clean).style.box);
    CHECK(trackOf(clean).style.placement == CaptionPlacement::Bottom);

    // An unknown preset: still made, in the default look, with a warning.
    const auto nope = h.ok("highlights.toShort", Json{{"sequence", h.tl.seq.value}, {"moment", 1}, {"normalize", false}, {"hookTitle", false}, {"captionPreset", "Nope"}});
    CHECK_FALSE(nope.data.contains("captionPreset"));
    CHECK(trackOf(nope).style.box == CaptionStyle{}.box);
    CHECK(std::any_of(nope.warnings.begin(), nope.warnings.end(), [](const std::string& w) { return w.find("Nope") != std::string::npos; }));

    // No captions: no style step.
    const auto silent = h.ok("highlights.toShort", Json{{"sequence", h.tl.seq.value}, {"moment", 1}, {"normalize", false}, {"captions", false}, {"captionPreset", "Shorts"}});
    CHECK_FALSE(silent.data.contains("captionPreset"));
    CHECK(h.state()->findSequence(SequenceId{silent.createdIds.front()})->captionTracks.empty());
    CHECK(h.errorOf("highlights.toShort", Json{{"sequence", h.tl.seq.value}, {"moment", 1}, {"captionPreset", std::string(65, 'x')}}) == "InvalidParams");
}

TEST_CASE("The assistant makes a highlight reel of the best moments as one undo step", "[ai][highlights][agent]") {
    Fixture fx;
    ai::test::ScriptedModel model;
    model.tool("highlights_find", Json{{"count", 5}})
        .tool("highlights_toReel", Json{{"order", "time"}, {"transition", "crossDissolve"}})
        .text("I found your 5 best moments and cut them into a highlight reel.");
    ai::agent::AgentOptions options;
    options.services = Fixture::policy(fx.highlights, fx.analysisProvider).services;
    ai::agent::EditAgent agent(options);
    const auto tools = agent.tools();
    const auto offered = [&](std::string_view name) { return std::ranges::any_of(tools, [&](const ai::ToolSpec& t) { return t.name == name; }); };
    CHECK(offered("highlights_find"));
    CHECK(offered("highlights_toReel"));
    CHECK(offered("highlights_toShort"));

    ai::agent::AgentRequest request;
    request.prompt = "make a highlight reel of my best moments";
    request.sequence = fx.h.tl.seq;
    request.base = std::make_shared<const ProjectState>(fx.h.session.stateForSave());
    request.baseRevision = fx.h.session.revision();
    request.constraints = ai::agent::parseConstraints(request.prompt, *request.base, fx.h.tl.seq);
    const auto before = fx.h.session.snapshot();
    const ai::agent::AgentOutcome out = agent.run(model, request, {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == ai::agent::AgentStatus::Staged);
    CHECK(out.verification.passed());
    REQUIRE(out.staged);
    CHECK(out.staged->sequences.size() == 2);
    CHECK(fx.h.session.snapshot() == before); // staged only

    const ai::agent::ApplyResult applied = ai::agent::applyStaged(fx.h.session, out, ai::agent::ApplyOptions{nullptr, command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence}), ai::agent::defaultAdditiveProjectCommands(), options.services});
    INFO(applied.message);
    REQUIRE(applied.status == ai::agent::ApplyResult::Status::Applied);
    CHECK(fx.h.session.snapshot()->sequences.size() == 2);
    CHECK(highlightMarkers(*fx.h.session.snapshot()->findSequence(fx.h.tl.seq)).size() == 5);
    REQUIRE(fx.h.session.undo());
    CHECK(*fx.h.session.snapshot() == *before);
}
