// The shorts finder (AI-9) on a synthetic podcast: sentences and topics, a deterministic, explained ranking (hook,
// whole topics, few fillers, the query), snapping to cuts and padding, the media signals, the guarded cloud re-rank
// (consent, no-cloud lock, transcript text only), shorts.create (cut, reframed, captioned, titled; one undo step; the
// cap) and the 60-minute performance figure.

#include "CommandHarness.h"
#include "NestedTestUtil.h"
#include "ShortsTestUtil.h"

#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/ai/shorts/Rerank.h>
#include <wabcut/ai/shorts/ShortsCommands.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/EditOps.h>

#include "../FakeTransport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <tuple>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::shorts;
using namespace wabcut::ai::shorts::test;
using wabcut::test::CommandHarness;
using wabcut::test::f;
using Catch::Approx;

namespace {

struct World {
    explicit World(int sentencesPerTopic = 12, int rounds = 1)
        : podcast(makePodcast(sentencesPerTopic, rounds)),
          tl(avPicture(static_cast<int>(std::ceil(podcast.seconds * 25.0))), {{'A', static_cast<std::int64_t>(std::ceil(podcast.seconds * 25.0)) + 10}}) {
        tl.linkMatching();
        transcripts = ai::edit::test::provider(tl, {{'A', ai::edit::test::transcript("en", podcast.words)}});
        transcript = ai::edit::SequenceTranscript::build(tl.state, tl.seq, *transcripts);
    }
    Podcast podcast;
    Timeline tl;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
    ai::edit::SequenceTranscript transcript;
};

double sec(Ticks t) { return t.toSeconds(); }

bool within(const ShortCandidate& s, std::pair<double, double> topic, double slack = 1.0) {
    return sec(s.range.start) >= topic.first - slack && sec(s.range.end()) <= topic.second + slack;
}

/// A probe that makes one stretch of the media louder.
class LoudStretch : public ai::edit::AudioEnergyProbe {
public:
    LoudStretch(double from, double to) : m_from(from), m_to(to) {}
    std::optional<double> rmsDb(const MediaItem&, int, TimeRange source) const override {
        const double mid = (sec(source.start) + sec(source.end())) / 2.0;
        return mid >= m_from && mid <= m_to ? -12.0 : -24.0;
    }

private:
    double m_from;
    double m_to;
};

} // namespace

TEST_CASE("Transcripts become sentences and topics", "[ai][shorts]") {
    World w;
    const auto sentences = splitSentences(w.transcript);
    REQUIRE(sentences.size() == 4 * 12);
    CHECK(sentences[12].text == "Why did we double our price overnight?");
    CHECK(sentences[0].first == 0);
    CHECK(sentences[1].first == sentences[0].last + 1);

    // Unpunctuated speech (Thai) breaks at pauses and speaker changes.
    ai::edit::MapTranscriptProvider thai;
    const Timeline t2(avPicture(100));
    thai.add(t2.media('A'), 0,
             ai::edit::test::transcript("th", {W{"สวัสดี", 0, 10, 1}, W{"ครับ", 11, 20, 1}, W{"วันนี้", 40, 50, 1}, W{"เรา", 51, 60, 2}, W{"คุย", 61, 70, 2}}));
    const auto st = ai::edit::SequenceTranscript::build(t2.state, t2.seq, thai);
    const auto split = splitSentences(st);
    REQUIRE(split.size() == 3); // a 0.8 s pause, then a new speaker
    CHECK(split[0].text == "สวัสดีครับ");

    std::vector<std::string> texts;
    for (const Sentence& s : sentences) {
        texts.push_back(s.text);
    }
    const TextVectors vectors(texts);
    std::vector<std::vector<std::pair<std::uint32_t, float>>> docs;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        docs.push_back(vectors.document(i));
    }
    const Topics topics = segmentTopics(docs);
    for (const std::size_t boundary : {12u, 24u, 36u}) { // each new topic is found (within a sentence)
        INFO(boundary);
        CHECK((topics.starts[boundary] || topics.starts[boundary - 1] || topics.starts[boundary + 1]));
    }
    CHECK(std::ranges::count(topics.starts, true) <= 8);
    CHECK(titleFrom("so, um, here's why the price doubled.") == "Here's why the price doubled");
    CHECK(titleFrom(std::string(120, 'x'), 60).size() <= 60);
}

TEST_CASE("Shorts are ranked deterministically, explained and snapped", "[ai][shorts]") {
    World w;
    const auto review = ai::edit::review(w.transcript, w.tl.state);
    ShortsOptions options;
    options.count = 3;
    const ShortsReport report = findShorts(w.tl.state, w.tl.seq, w.transcript, review, {}, options);
    REQUIRE(report.shorts.size() == 3);
    CHECK(report.sentences == 48);
    CHECK(report.candidates > 50);
    for (const ShortCandidate& s : report.shorts) {
        INFO(s.title);
        CHECK(sec(s.range.duration) >= 20.0);
        CHECK(sec(s.range.duration) <= 91.0);
        CHECK_FALSE(s.title.empty());
        CHECK_FALSE(s.reasons.empty());
        CHECK(s.score > 0.0);
        CHECK(s.score <= 1.0);
        // Snapped around whole words: not cutting into the first or last word, nor taking in a neighbour.
        CHECK(s.range.start <= w.transcript[s.firstWord].timeline.start);
        CHECK(s.range.end() >= w.transcript[s.lastWord].timeline.end());
        if (s.firstWord > 0) {
            CHECK(s.range.start >= w.transcript[s.firstWord - 1].timeline.end());
        }
        CHECK(s.range.start.value % f(1).value == 0); // frame-aligned
    }
    // The pricing topic opens with a hook question: it ranks first, as a whole topic.
    const ShortCandidate& best = report.shorts.front();
    CHECK(within(best, w.podcast.topics[1]));
    CHECK(best.hook == "Why did we double our price overnight?");
    CHECK(best.parts.hook >= 0.45);
    CHECK(std::ranges::find(best.reasons, "opens with a question") != best.reasons.end());
    CHECK(best.title.find("price") != std::string::npos);
    // The messy topic (fillers, long pauses) is the least clean.
    for (const ShortCandidate& s : report.shorts) {
        if (within(s, w.podcast.topics[2])) {
            CHECK(s.parts.clean < best.parts.clean);
        }
    }
    // No two picked shorts overlap; the same input gives the same answer.
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = i + 1; j < 3; ++j) {
            const auto& a = report.shorts[i].range;
            const auto& b = report.shorts[j].range;
            const Ticks overlap = std::min(a.end(), b.end()) - std::max(a.start, b.start);
            CHECK(sec(overlap) <= 0.2 * std::min(sec(a.duration), sec(b.duration)) + 1e-6);
        }
    }
    const ShortsReport again = findShorts(w.tl.state, w.tl.seq, w.transcript, review, {}, options);
    REQUIRE(again.shorts.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(again.shorts[i].range == report.shorts[i].range);
        CHECK(again.shorts[i].title == report.shorts[i].title);
    }

    // A query pulls the matching topic up.
    options.query = "product launch feedback";
    options.count = 1;
    const ShortsReport about = findShorts(w.tl.state, w.tl.seq, w.transcript, review, {}, options);
    REQUIRE(about.shorts.size() == 1);
    CHECK(within(about.shorts[0], w.podcast.topics[3]));
    CHECK(about.shorts[0].parts.query > 0.3);
}

TEST_CASE("A cut near a short's edge is used; media signals refine the score", "[ai][shorts]") {
    World w;
    // A cut (razor on both tracks) 0.3 s before the pricing hook.
    const Ticks hookStart = ai::edit::test::frames(w.podcast.words[108].from);
    {
        const Ticks at = hookStart - Ticks{kTicksPerSecond * 3 / 10};
        auto razored = wabcut::edit::addEditToAllTracks(w.tl.state, w.tl.seq, Ticks{floorDiv(at.value, f(1).value) * f(1).value});
        REQUIRE(razored);
        w.tl.state = razored->state;
        w.transcript = ai::edit::SequenceTranscript::build(w.tl.state, w.tl.seq, *w.transcripts);
    }
    ShortsOptions options;
    options.count = 1;
    const auto first = findShorts(w.tl.state, w.tl.seq, w.transcript, {}, {}, options);
    REQUIRE(first.shorts.size() == 1);
    const auto cuts = cutPoints(w.tl.state, w.tl.seq, {});
    CHECK(std::ranges::binary_search(cuts, first.shorts[0].range.start)); // snapped to the cut

    // A loud stretch (the product topic) gets a better energy part than the rest.
    const LoudStretch loud(w.podcast.topics[3].first, w.podcast.topics[3].second);
    ShortsSignals signals;
    signals.energy = &loud;
    options.count = 4;
    const auto lively = findShorts(w.tl.state, w.tl.seq, w.transcript, {}, signals, options);
    for (const ShortCandidate& s : lively.shorts) {
        INFO(s.title);
        if (within(s, w.podcast.topics[3])) {
            CHECK(s.parts.energy > 0.6);
            CHECK(std::ranges::find(s.reasons, "lively delivery") != s.reasons.end());
        } else if (sec(s.range.end()) < w.podcast.topics[3].first) {
            CHECK(s.parts.energy <= 0.5);
        }
    }
    // Faces on screen raise the visual part.
    vision::SubjectAnalysis faces;
    faces.end = Ticks{kTicksPerSecond * 600};
    for (double t = 0.1; t < 600.0; t += 0.5) {
        vision::SubjectSample sample;
        sample.time = Ticks{static_cast<std::int64_t>(t * static_cast<double>(kTicksPerSecond))};
        sample.faces.push_back(vision::SubjectBox{vision::Box{0.4f, 0.2f, 0.2f, 0.3f}, 0.9f, 1, 0.0f});
        faces.samples.push_back(sample);
    }
    signals.subjects = [&](const MediaItem&, int) { return &faces; };
    const auto seen = findShorts(w.tl.state, w.tl.seq, w.transcript, {}, signals, options);
    REQUIRE_FALSE(seen.shorts.empty());
    CHECK(seen.shorts[0].parts.visual >= 0.5);
    CHECK(std::ranges::find(seen.shorts[0].reasons, "faces on screen") != seen.shorts[0].reasons.end());
}

TEST_CASE("Shorts hear and see a podcast through a multicam clip and a nest like the flat recording (0.2)", "[ai][shorts][multicam]") {
    World w;
    const LoudStretch loud(w.podcast.topics[3].first, w.podcast.topics[3].second);
    vision::SubjectAnalysis faces;
    faces.end = Ticks{kTicksPerSecond * 600};
    for (double t = 0.1; t < 600.0; t += 0.5) {
        vision::SubjectSample sample;
        sample.time = Ticks{static_cast<std::int64_t>(t * static_cast<double>(kTicksPerSecond))};
        if (t >= w.podcast.topics[3].first) { // faces from the product topic on
            sample.faces.push_back(vision::SubjectBox{vision::Box{0.4f, 0.2f, 0.2f, 0.3f}, 0.9f, 1, 0.0f});
        }
        faces.samples.push_back(sample);
    }
    ShortsSignals signals;
    signals.energy = &loud;
    signals.subjects = [&](const MediaItem& media, int) { return media.id == w.tl.media('A') ? &faces : nullptr; };
    ShortsOptions options;
    options.count = 4;
    const auto parts = [&](const ProjectState& state) {
        const auto transcript = ai::edit::SequenceTranscript::build(state, w.tl.seq, *w.transcripts);
        REQUIRE(transcript.size() == w.transcript.size()); // the transcript reads through (C.17b)
        std::vector<std::tuple<double, double, double, double>> out; // start, end, energy, visual
        for (const ShortCandidate& s : findShorts(state, w.tl.seq, transcript, {}, signals, options).shorts) {
            out.emplace_back(sec(s.range.start), sec(s.range.end()), s.parts.energy, s.parts.visual);
        }
        return out;
    };
    const auto flat = parts(w.tl.state);
    REQUIRE(flat.size() == 4);
    CHECK(std::ranges::any_of(flat, [](const auto& p) { return std::get<2>(p) > 0.6; })); // the loud topic
    CHECK(std::ranges::any_of(flat, [](const auto& p) { return std::get<3>(p) >= 0.5; })); // faces

    SECTION("a two-angle multicam clip playing and showing the recording") {
        const Ticks length = w.tl.sequence().contentEnd();
        const wabcut::test::PlacedMulticam mc = wabcut::test::placeMulticam(w.tl.state, w.tl.seq, {w.tl.media('A'), w.tl.media('B')}, {Ticks{}, Ticks{}},
                                                              TimeRange{Ticks{}, length}, Ticks{});
        CHECK(parts(mc.state) == flat);
    }
    SECTION("a nest") {
        CHECK(parts(wabcut::test::nestEverything(w.tl.state, w.tl.seq).edit.state) == flat);
    }
}

// --- The cloud re-rank -------------------------------------------------------------------------------------------------

namespace {

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
        return r;
    }

private:
    ModelSpec m_spec;
    std::string m_answer;
    int& m_calls;
    ChatRequest& m_last;
};

ModelRegistry cloudRegistry() {
    auto r = ModelRegistry::fromJson(R"({"schemaVersion":1,"providers":[
        {"id":"cloud","name":"Cloud","api":"scripted","baseUrl":"https://cloud.test","auth":"bearer","keyRequired":false,
         "termsUrl":"https://cloud.test/terms","dataPolicy":"kept 30 days"}],
      "models":[
        {"id":"cloud-model","provider":"cloud","capabilities":{"chat":true},"prices":[{"inputPerMTok":2,"outputPerMTok":10}]}]})");
    REQUIRE(r);
    return std::move(*r);
}

/// The user's cloud model behind AiPlatform: the consent ledger, the no-cloud lock and the spend cap apply.
struct CloudBench {
    explicit CloudBench(std::string answer)
        : registry(cloudRegistry()), providers(registry), keys(store, [](std::string_view) { return std::optional<std::string>(); }),
          platform(providers, keys, consent, spend, log, transport), answer(std::move(answer)) {
        providers.registerChat("scripted", [this](ProviderContext ctx) {
            return std::make_unique<RecordingModel>(ctx.model, this->answer, calls, last);
        });
        platform.setClock([] { return std::chrono::year_month_day{std::chrono::year{2026}, std::chrono::month{10}, std::chrono::day{11}}; });
    }
    ShortsServices::ChatOpener opener() {
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

const char* kAnswer = R"({"shorts":[{"id":1,"score":2,"title":"Meh","why":"slow start"},
                                     {"id":2,"score":9.5,"title":"Hiring is broken","why":"strong opinion"},
                                     {"id":3,"score":6,"title":"Our roadmap","why":"clear"}]})";

} // namespace

TEST_CASE("The cloud re-rank reorders and retitles from the transcript text only", "[ai][shorts][rerank]") {
    World w;
    ShortsOptions options;
    options.count = 3;
    auto shorts = findShorts(w.tl.state, w.tl.seq, w.transcript, {}, {}, options).shorts;
    REQUIRE(shorts.size() == 3);
    const auto second = shorts[1].range;
    int calls = 0;
    ChatRequest last;
    RecordingModel model(*ModelRegistry::builtin().model("claude-sonnet-5-5"), kAnswer, calls, last);
    auto outcome = rerankShorts(model, shorts, RerankOptions{});
    REQUIRE(outcome);
    CHECK(outcome->scored == 3);
    CHECK(shorts[0].range == second);
    CHECK(shorts[0].title == "Hiring is broken");
    CHECK(shorts[0].cloudScore == Approx(0.95));
    CHECK(shorts[0].cloudReason == "strong opinion");
    // Only transcript text leaves: no media name, file path or project detail.
    const std::string sent = last.system + last.messages.front().text();
    CHECK(sent.find("Why did we double our price overnight?") != std::string::npos);
    CHECK(sent.find("D:/footage") == std::string::npos);
    CHECK(sent.find(".mov") == std::string::npos);
    REQUIRE(last.responseFormat);

    RecordingModel garbage(*ModelRegistry::builtin().model("claude-sonnet-5-5"), "I like them all!", calls, last);
    const auto before = shorts;
    const auto failed = rerankShorts(garbage, shorts, RerankOptions{});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == AiErrorCode::ProviderError);
    CHECK(shorts[0].range == before[0].range);
}

TEST_CASE("shorts.find re-ranks only with consent and never for a no-cloud project", "[ai][shorts][rerank][commands]") {
    for (const bool consented : {false, true}) {
        for (const bool locked : {false, true}) {
            INFO("consent " << consented << ", no-cloud " << locked);
            World w;
            w.tl.state.aiNoCloud = locked;
            CloudBench cloud(kAnswer);
            if (consented) {
                cloud.consent.grantAlways(*cloud.registry.provider("cloud"), Capability::Chat, "2026-10-11");
            }
            cloud.consent.setProjectLockSource([locked] { return locked; });
            auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
                std::make_shared<ai::edit::TranscriptServices>(w.transcripts), std::make_shared<ShortsServices>(cloud.opener())});
            CommandHarness h(w.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, services}, ai::edit::editingCommands());
            const auto result = h.ok("shorts.find", Json{{"count", 3}, {"rerank", true}});
            const bool expected = consented && !locked;
            CHECK(result.data["reranked"] == expected);
            CHECK(cloud.calls == (expected ? 1 : 0)); // nothing left the machine otherwise
            CHECK(result.data["shorts"].size() == 3);
            if (!expected) {
                REQUIRE_FALSE(result.warnings.empty());
                CHECK(result.warnings.back().find(locked ? "no-cloud" : "local ranking stands") != std::string::npos);
            } else {
                CHECK(result.data["shorts"][0]["title"] == "Hiring is broken");
                CHECK(cloud.log.entries().size() == 1); // recorded in the request log
            }
        }
    }
}

// --- shorts.create -------------------------------------------------------------------------------------------------------

TEST_CASE("shorts.create makes reframed, captioned, titled shorts in one undo step", "[ai][shorts][commands]") {
    World w;
    auto subjects = std::make_shared<vision::MapSubjectProvider>();
    auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<ai::edit::TranscriptServices>(w.transcripts), std::make_shared<reframe::ReframeServices>(subjects)});
    CommandHarness h(w.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, services}, ai::edit::editingCommands());
    const auto found = h.ok("shorts.find", Json{{"count", 2}});
    REQUIRE(found.data["shorts"].size() == 2);
    Json picks = Json::array();
    for (const Json& s : found.data["shorts"]) {
        picks.push_back(Json{{"start", s["start"]}, {"end", s["end"]}, {"title", s["title"]}});
    }
    const auto source = h.state()->sequencePtr(h.tl.seq);
    const auto made = h.ok("shorts.create", Json{{"shorts", picks}, {"mode", "follow"}});
    CHECK(made.committed);
    REQUIRE(made.createdIds.size() == 2);
    CHECK(h.state()->sequencePtr(h.tl.seq) == source); // the source is untouched
    REQUIRE(validate(*h.state()));
    for (std::size_t i = 0; i < 2; ++i) {
        const Sequence& s = *h.state()->findSequence(SequenceId{made.createdIds[i]});
        INFO(s.name);
        CHECK(s.width == 1080);
        CHECK(s.height == 1920);
        const double length = found.data["shorts"][i]["end"].get<double>() - found.data["shorts"][i]["start"].get<double>();
        CHECK(s.duration.toSeconds() == Approx(length).margin(0.05));
        REQUIRE(s.derivedFrom);
        CHECK(s.derivedFrom->generator == "shorts/1");
        CHECK(s.derivedFrom->source == h.tl.seq);
        REQUIRE(s.captionTracks.size() == 1);
        CHECK_FALSE(s.captionTracks[0].cues.empty());
        CHECK(s.captionTracks[0].cues.front().start >= Ticks{0});
        // 0.4: the vertical short's captions in the "Shorts" style (above the platforms' buttons), short lines.
        const CaptionStyle& style = s.captionTracks[0].style;
        CHECK(style.placement == CaptionPlacement::Custom);
        CHECK(style.positionY.valueAt(Ticks{}) == Approx(0.68));
        CHECK(style.animation == CaptionAnimation::Pop);
        CHECK_FALSE(style.box);
        CHECK(made.data["sequences"][i].value("captionPreset", std::string()) == "Shorts");
        for (const CaptionCue& cue : s.captionTracks[0].cues) {
            std::size_t lines = 1;
            std::size_t from = 0;
            for (std::size_t at = cue.text.find('\n'); ; at = cue.text.find('\n', from)) {
                const std::size_t end = at == std::string::npos ? cue.text.size() : at;
                INFO(cue.text);
                // At most 10 characters (fix round 1: what stays one line on a rendered 1080x1920 frame), unless the
                // line is one longer word (a word is never split).
                CHECK((end - from <= 10 || cue.text.substr(from, end - from).find(' ') == std::string::npos));
                if (at == std::string::npos)
                    break;
                from = at + 1;
                ++lines;
            }
            CHECK(lines <= 2);
            CHECK(cue.duration <= Ticks{kTicksPerSecond * 5 / 2} + f(1)); // 2.5 s
        }
        // The hook title on its own top track, for its first seconds.
        const Track& top = *s.videoTracks.back();
        CHECK(top.name == "Hook title");
        REQUIRE(top.clips.size() == 1);
        CHECK(top.clips.front().start() == Ticks{0});
        CHECK(top.clips.front().properties().value(ParamId::PositionY) < 0.0);
        // The picture is reframed: filled to 1920 high.
        const Clip& picture = s.videoTracks.front()->clips.front();
        CHECK(picture.properties().value(ParamId::Scale) == Approx(1920.0 / 1080.0));
        CHECK(s.name.find(found.data["shorts"][i]["title"].get<std::string>()) != std::string::npos);
    }
    REQUIRE(h.session.undo()); // all of them at once
    CHECK(h.state()->sequences.size() == 1);

    // With cleanup the fillers and long pauses go: a messy stretch gets shorter than its range.
    const auto& messy = w.podcast.topics[2];
    const auto cleaned = h.ok("shorts.create", Json{{"shorts", Json::array({Json{{"start", messy.first}, {"end", messy.first + 40.0}}})},
                                                  {"cleanup", true},
                                                  {"cleanupSilences", 0.5},
                                                  {"captions", false},
                                                  {"hookTitle", false}});
    const Sequence& tidy = *h.state()->findSequence(SequenceId{cleaned.createdIds[0]});
    CHECK(tidy.contentEnd().toSeconds() < 40.0 - 3.0);

    // The limits: at most 10 shorts a call; a range must be valid.
    Json eleven = Json::array();
    for (int i = 0; i < 11; ++i) {
        eleven.push_back(Json{{"start", i * 10.0}, {"end", i * 10.0 + 5.0}});
    }
    CHECK(h.errorOf("shorts.create", Json{{"shorts", eleven}}) == "InvalidParams");
    CHECK(h.errorOf("shorts.create", Json{{"shorts", Json::array({Json{{"start", 30.0}, {"end", 20.0}}})}}) == "InvalidParams");
    CommandHarness readOnly(w.tl, command::EnginePolicy{command::CapabilitySet::of({command::Capability::Read}), 64, services},
                            ai::edit::editingCommands());
    CHECK(readOnly.errorOf("shorts.create", Json{{"shorts", picks}}) == "PermissionDenied");
    CHECK(readOnly.errorOf("shorts.find").empty());
    CommandHarness bare(w.tl, command::EnginePolicy{}, ai::edit::editingCommands());
    CHECK(bare.errorOf("shorts.find") == "InvalidRequest");
}

TEST_CASE("shorts.create takes another caption preset and warns about an unknown one", "[ai][shorts][commands]") {
    World w;
    auto subjects = std::make_shared<vision::MapSubjectProvider>();
    auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<ai::edit::TranscriptServices>(w.transcripts), std::make_shared<reframe::ReframeServices>(subjects)});
    CommandHarness h(w.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, services}, ai::edit::editingCommands());
    const auto found = h.ok("shorts.find", Json{{"count", 1}});
    REQUIRE(found.data["shorts"].size() == 1);
    const Json pick{{"start", found.data["shorts"][0]["start"]}, {"end", found.data["shorts"][0]["end"]}};
    const auto styleOf = [&](const command::CommandResult& made) -> CaptionTrack {
        const Sequence& s = *h.state()->findSequence(SequenceId{made.createdIds.front()});
        REQUIRE(s.captionTracks.size() == 1);
        return s.captionTracks.front();
    };

    // Every call names the source: the shorts made before are sequences too.
    // "Clean": that preset's look (no box, bottom), mid-size cues on a vertical short.
    const auto clean = h.ok("shorts.create", Json{{"sequence", h.tl.seq.value}, {"shorts", Json::array({pick})}, {"captionPreset", "Clean"}, {"hookTitle", false}});
    const CaptionTrack cleanTrack = styleOf(clean);
    const auto cleanStyle = wabcut::edit::captionPresetStyle(*h.state(), "Clean");
    REQUIRE(cleanStyle);
    CHECK(cleanTrack.style.box == cleanStyle->box);
    CHECK(cleanTrack.style.placement == cleanStyle->placement);
    CHECK(cleanTrack.style.outlineWidth == Approx(cleanStyle->outlineWidth));
    CHECK(clean.data["sequences"][0].value("captionPreset", std::string()) == "Clean");
    for (const CaptionCue& cue : cleanTrack.cues)
        CHECK(cue.duration <= Ticks::fromSeconds(4) + f(1));

    // An unknown preset: the short is made in the default look, with a warning.
    const auto unknown = h.ok("shorts.create", Json{{"sequence", h.tl.seq.value}, {"shorts", Json::array({pick})}, {"captionPreset", "Nope"}, {"hookTitle", false}});
    CHECK_FALSE(unknown.data["sequences"][0].contains("captionPreset"));
    CHECK(styleOf(unknown).style.placement == CaptionStyle{}.placement);
    CHECK(styleOf(unknown).style.box == CaptionStyle{}.box);
    CHECK(std::any_of(unknown.warnings.begin(), unknown.warnings.end(),
                      [](const std::string& w) { return w.find("Nope") != std::string::npos; }));

    // "" is the default look; landscape output keeps it unless a preset is given.
    const auto plain = h.ok("shorts.create", Json{{"sequence", h.tl.seq.value}, {"shorts", Json::array({pick})}, {"captionPreset", ""}, {"hookTitle", false}});
    CHECK(styleOf(plain).style.placement == CaptionStyle{}.placement);
    const auto wide = h.ok("shorts.create", Json{{"sequence", h.tl.seq.value}, {"shorts", Json::array({pick})}, {"aspect", "16:9"}, {"hookTitle", false}});
    CHECK(styleOf(wide).style.placement == CaptionStyle{}.placement);
    CHECK_FALSE(wide.data["sequences"][0].contains("captionPreset"));
    CHECK(h.errorOf("shorts.create", Json{{"sequence", h.tl.seq.value}, {"shorts", Json::array({pick})}, {"captionPreset", std::string(65, 'x')}}) == "InvalidParams");
}

TEST_CASE("A cut sequence keeps what lies inside the range", "[ai][shorts]") {
    Timeline tl(R"(
        V1 tgt |AaaaB5bbbbbCcccc|
        A1 tgt |AaaaB5bbbbb.....|
    )");
    tl.linkMatching();
    // A: frames 0-4, B (source In 5, the "5" takes a cell): 4-11, C: 11-15.
    auto cut = cutSequence(tl.state, tl.seq, TimeRange::fromStartEnd(f(2), f(12)), "Short", 1080, 1920);
    REQUIRE(cut);
    REQUIRE(validate(cut->state));
    const Sequence& s = *cut->state.findSequence(cut->sequence);
    INFO(wabcut::test::render(cut->state, cut->sequence));
    CHECK(s.duration == f(10));
    REQUIRE(s.videoTracks[0]->clips.size() == 3);
    CHECK(s.videoTracks[0]->clips.front().start() == Ticks{0});
    CHECK(s.videoTracks[0]->clips.front().duration() == f(2)); // A's last two frames
    CHECK(s.videoTracks[0]->clips.back().duration() == f(1));  // C's first frame
    CHECK_FALSE(s.videoTracks[0]->clips.back().linkGroup.isValid()); // C had no partner
    CHECK(s.videoTracks[0]->clips.front().linkGroup.isValid());       // A still with its audio
    CHECK(cutSequence(tl.state, tl.seq, TimeRange::fromStartEnd(f(100), f(110)), "x", 2, 2).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Finding shorts in an hour of transcript is fast", "[ai][shorts][perf]") {
    // ~60 minutes: 4 topics x 15 sentences, 15 rounds (8100 words).
    World w(15, 15);
    REQUIRE(w.podcast.seconds > 3500.0);
    const auto started = std::chrono::steady_clock::now();
    const auto review = ai::edit::review(w.transcript, w.tl.state);
    ShortsOptions options;
    options.count = 5;
    const ShortsReport report = findShorts(w.tl.state, w.tl.seq, w.transcript, review, {}, options);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::printf("[perf] shorts.find on a %.0f-minute transcript (%zu words, %zu sentences, %zu candidates): %.0f ms (finder %.0f ms)\n",
                w.podcast.seconds / 60.0, w.transcript.size(), report.sentences, report.candidates, ms, report.milliseconds);
    CHECK(report.shorts.size() == 5);
    CHECK(ms < 10000.0);
}
