// AI-12 voice-over: fitting generated lines to their slots (stretch bounds, shifts, flags), the lines of a dub and of
// a narration, the run (the hard cap, a cloned voice always deleted, provenance), the one-step edit (a Dialogue "Dub"
// track, the original dialogue ducked, undone at once), and dub.create / narration.create through the engine with
// their refusals (no cap, a cap below the estimate, no approval host, a no-cloud project, cloning without the rights
// or of a redacted speaker).

#include "CommandHarness.h"
#include "EditTestUtil.h"
#include "GlobalTestUtil.h"

#include <wabcut/ai/dub/Dub.h>
#include <wabcut/ai/dub/DubCommands.h>
#include <wabcut/ai/dub/Pipeline.h>
#include <wabcut/ai/dub/TimeFit.h>
#include <wabcut/ai/dub/Voices.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::dub;
using namespace wabcut::ai::global::test;
using namespace wabcut::ai::edit::test;
using command::Json;

namespace {

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }
double secondsOf(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }

std::filesystem::path scratch(std::string_view name) {
    const auto dir = std::filesystem::temp_directory_path() / ("wabcut-dub-test-" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

} // namespace

TEST_CASE("Time fit: short lines play as they are, long ones speed up at most 15 %, then move and are flagged",
          "[ai][global][dub][timefit]") {
    const Slot targets[] = {{sec(0), sec(2)}, {sec(3), sec(2)}, {sec(5.5), sec(1)}, {sec(10), sec(2)}};
    const Ticks audio[] = {sec(1.5), sec(2.2), sec(2.0), sec(1)};
    FitReport report;
    const auto fitted = fitLines(targets, audio, FitOptions{}, &report);
    REQUIRE(fitted.size() == 4);
    // 1. shorter than its slot: as generated, from the slot's start.
    CHECK(fitted[0].start == sec(0));
    CHECK(fitted[0].duration == sec(1.5));
    CHECK(fitted[0].speed == 1.0);
    CHECK_FALSE(fitted[0].stretched);
    // 2. 10 % too long: sped up to fit exactly.
    CHECK(fitted[1].stretched);
    CHECK(fitted[1].speed == Catch::Approx(1.1));
    CHECK(secondsOf(fitted[1].duration) == Catch::Approx(2.0).margin(1e-6));
    CHECK(fitted[1].start == sec(3));
    // 3. twice its slot: 15 % at most, then earlier into the free time, and still past the next slot: flagged.
    CHECK(fitted[2].speed == Catch::Approx(1.15));
    CHECK(fitted[2].shifted);
    CHECK(fitted[2].start == fitted[1].end() + FitOptions{}.minGap); // as early as the line before allows (0.4 s)
    CHECK_FALSE(fitted[2].flagged);                                  // the next slot is far away
    CHECK(fitted[3].start == sec(10));
    CHECK(report.stretched == 2);
    CHECK(report.maxSpeed == Catch::Approx(1.15));
    CHECK(report.fitted == 2);

    // Lines that overlap: the second is pushed after the first and the first is flagged.
    const Slot tight[] = {{sec(0), sec(1)}, {sec(1.05), sec(1)}};
    const Ticks long2[] = {sec(2), sec(1)};
    FitReport tightReport;
    const auto pushed = fitLines(tight, long2, FitOptions{}, &tightReport);
    CHECK(pushed[0].flagged);
    CHECK(pushed[0].note.find("into the next line") != std::string::npos);
    CHECK(pushed[1].shifted);
    CHECK(pushed[1].start == pushed[0].end() + FitOptions{}.minGap);
    CHECK(tightReport.flagged == 1);

    // No stretching allowed: only moves.
    FitOptions none;
    none.maxStretch = 0.0;
    const auto still = fitLines(tight, long2, none);
    CHECK(still[0].speed == 1.0);
    CHECK(still[0].duration == sec(2));
}

TEST_CASE("Dub lines leave bleeped words out; narration lines come from a script", "[ai][global][dub][lines]") {
    CaptionTrack track;
    track.cues = {CaptionCue{sec(2), sec(2), "วันนี้ [bleep]\nไปตลาด", {}}, CaptionCue{sec(0), sec(1), "[bleep]", {}},
                  CaptionCue{sec(5), sec(1), "Hello , [bleep]!", {}}};
    const auto lines = linesFromCaptions(track);
    REQUIRE(lines.size() == 2); // the bleep-only cue says nothing
    CHECK(lines[0].text == "วันนี้ ไปตลาด");
    CHECK(lines[0].start == sec(2));
    CHECK(lines[0].source == 0);
    CHECK(lines[1].text == "Hello,!");
    CHECK(speakableText("  [bleep] ... ") == "");

    const auto narration = narrationLines("# Trip\n\n- Welcome to Chiang Mai.\n- [B-roll: temple] The old city has walls. And a moat.\n");
    REQUIRE(narration.size() >= 2);
    CHECK(narration[0].text == "Welcome to Chiang Mai.");
    for (const DubLine& l : narration) {
        CHECK(l.text.find("B-roll") == std::string::npos);
        CHECK(l.text.find('#') == std::string::npos);
    }

    const Ticks lengths[] = {sec(2), sec(3)};
    const DubLine two[] = {narration[0], narration[1]};
    const auto sequential = layoutNarration(two, lengths, sec(1), sec(0.5), {});
    REQUIRE(sequential.targets.size() == 2);
    CHECK(sequential.targets[1].start == sec(3.5));
    CHECK_FALSE(sequential.onClips);
    const TimeRange beats[] = {{sec(10), sec(4)}, {sec(20), sec(4)}};
    const auto onBeats = layoutNarration(two, lengths, sec(0), sec(0.5), beats);
    CHECK(onBeats.onClips);
    CHECK(onBeats.targets[1].start == sec(20));
    const TimeRange oneBeat[] = {{sec(10), sec(4)}};
    CHECK_FALSE(layoutNarration(two, lengths, sec(0), sec(0.5), oneBeat).warning.empty());
}

TEST_CASE("A dub run: one file, every line fitted, marked AI-generated, and stopped at its hard cap",
          "[ai][global][dub][pipeline]") {
    ScriptedSpeech speech("eleven_v3", 10.0);
    FakeSpeechAudio audio;
    DubJob job;
    job.lines = {DubLine{"สวัสดีครับ", sec(0), sec(2), {}, 0}, DubLine{"long line that will not fit its slot at all", sec(3), sec(2), {}, 1}};
    speech.lengths["long line that will not fit its slot at all"] = 2.2;
    job.voice.voiceId = "JBFqnCBsd6RMkjVDRZzb";
    job.voice.language = "th";
    job.directory = scratch("run");
    job.name = "Dub (ไทย)";
    job.createdAt = "2026-10-12T10:00:00Z";
    auto run = runDub(job, speech, audio, {});
    REQUIRE(run);
    REQUIRE(speech.requests.size() == 2);
    CHECK(speech.requests[0].voiceId == "JBFqnCBsd6RMkjVDRZzb");
    CHECK(speech.requests[0].language == "th");
    REQUIRE(run->audio.segments.size() == 2);
    CHECK(run->audio.segments[0].start == Ticks{});
    CHECK(run->audio.segments[1].start > run->audio.segments[0].end()); // 50 ms apart in the file
    CHECK(run->fitted[1].stretched);
    CHECK(audio.speeds.size() == 1);
    CHECK(audio.speeds[0] <= 1.15 + 1e-9);
    CHECK(secondsOf(run->generated[1]) == Catch::Approx(2.2 + 0.04).margin(0.002)); // trimmed to 20 ms of silence a side
    const MediaItem& item = run->audio.item;
    REQUIRE(item.generated);
    CHECK(item.generated->kind == "audio");
    CHECK(item.generated->operation == "dub");
    CHECK(item.generated->provider == "elevenlabs");
    CHECK(item.generated->model == "eleven_v3");
    CHECK(item.generated->promptHash.size() == 64);
    CHECK(item.generated->createdAt == "2026-10-12T10:00:00Z");
    CHECK(item.name == "Dub (ไทย)");
    CHECK(audio.written.filename().string().starts_with("dub-"));
    CHECK(run->characters == characterCount("สวัสดีครับ") + characterCount("long line that will not fit its slot at all"));

    // The cap: a run that could cost more stops before the line that would pass it.
    ScriptedSpeech capped("eleven_v3");
    DubJob expensive = job;
    expensive.lines = std::vector<DubLine>(5, DubLine{std::string(400, 'a'), sec(0), sec(30), {}, 0});
    const CostEstimate one = estimateSpeechCost(capped.spec(), 400, todayUtc());
    expensive.budget = one.ceiling * 2;
    auto stopped = runDub(expensive, capped, audio, {});
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == AiErrorCode::SpendCapExceeded);
    CHECK(capped.requests.size() == 2);

    // A provider failure names the line; nothing is written.
    ScriptedSpeech failing("eleven_v3");
    failing.failAt = 2;
    auto failed = runDub(job, failing, audio, {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == AiErrorCode::QuotaExceeded);
    CHECK(failed.error().message.starts_with("line 2"));
}

TEST_CASE("A cloned voice is used for every line and deleted after the run, also when it fails", "[ai][global][dub][clone]") {
    FakeSpeechAudio audio;
    DubJob job;
    job.lines = {DubLine{"Hola", sec(0), sec(2), {}, 0}, DubLine{"Adiós", sec(3), sec(2), {}, 1}};
    job.directory = scratch("clone");
    job.cloneSamples = {job.directory / "sample.wav"};
    {
        ScriptedSpeech speech;
        auto run = runDub(job, speech, audio, {});
        REQUIRE(run);
        CHECK(run->cloned);
        CHECK(speech.clonedFrom == job.cloneSamples);
        for (const SpeechRequest& r : speech.requests) {
            CHECK(r.voiceId == "ClonedVoice42");
        }
        CHECK(speech.deleted == std::vector<std::string>{"ClonedVoice42"});
    }
    {
        ScriptedSpeech speech;
        speech.failAt = 2;
        CHECK_FALSE(runDub(job, speech, audio, {}));
        CHECK(speech.deleted == std::vector<std::string>{"ClonedVoice42"});
    }
    {
        ScriptedSpeech speech;
        speech.cloneNeedsVerification = true;
        CHECK_FALSE(runDub(job, speech, audio, {}));
        CHECK(speech.requests.empty());
        CHECK(speech.deleted == std::vector<std::string>{"ClonedVoice42"});
    }
}

// Review wave 5, A5-P2-14: the deletion ran in a guard's destructor after the run had been moved out (and on errors the
// run was dropped), so a voice left on the provider's account - the speaker's biometric data - was never reported.
TEST_CASE("A cloned voice that cannot be deleted is reported on the run or on the error", "[ai][global][dub][clone][privacy]") {
    FakeSpeechAudio audio;
    DubJob job;
    job.lines = {DubLine{"Hola", sec(0), sec(2), {}, 0}, DubLine{"Adiós", sec(3), sec(2), {}, 1}};
    job.directory = scratch("clone-undeleted");
    job.cloneSamples = {job.directory / "sample.wav"};
    const auto reported = [](std::string_view text) { return text.find("could not be deleted") != std::string_view::npos; };
    {
        ScriptedSpeech speech;
        speech.deleteFails = true;
        auto run = runDub(job, speech, audio, {});
        REQUIRE(run);
        CHECK(speech.deleted == std::vector<std::string>{"ClonedVoice42"});
        CHECK(std::any_of(run->warnings.begin(), run->warnings.end(), reported));
    }
    {
        ScriptedSpeech speech;
        speech.deleteFails = true;
        speech.failAt = 2;
        auto run = runDub(job, speech, audio, {});
        REQUIRE_FALSE(run);
        CHECK(run.error().message.find("scripted failure") != std::string::npos);
        CHECK(reported(run.error().message));
    }
    {
        ScriptedSpeech speech;
        speech.deleteFails = true;
        speech.cloneNeedsVerification = true;
        auto run = runDub(job, speech, audio, {});
        REQUIRE_FALSE(run);
        CHECK(reported(run.error().message));
    }
}

namespace {

// A1 is the dialogue, A2 the music.
constexpr std::string_view kPicture = R"(
    V1 tgt          |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A2 music        |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
)";

// Speaker 0 talks for 5.4 s (frames 0-135), speaker 1 for half a second.
std::shared_ptr<const Transcript> conversation() {
    std::vector<W> words;
    for (int i = 0; i < 27; ++i) {
        words.push_back(W{"word" + std::to_string(i), i * 5.0, i * 5.0 + 4.0, 0});
    }
    words.push_back(W{"Thanks", 136, 142, 1});
    words.push_back(W{"bye.", 142, 148, 1});
    return transcript("en", words);
}

struct DubStudio {
    explicit DubStudio(bool noCloud = false, bool redactSpeakerOne = false, std::shared_ptr<const Transcript> words = conversation()) {
        Timeline tl(kPicture);
        tl.linkMatching();
        if (redactSpeakerOne) {
            Redaction r;
            r.media = tl.media('A');
            r.name = "Guest";
            r.source = TimeRange::fromStartEnd(frames(136), frames(142));
            r.audio = RedactionAudio::Bleep;
            auto added = wabcut::edit::addRedaction(tl.state, r);
            REQUIRE(added);
            tl.state = std::move(added->state);
        }
        tl.state.aiNoCloud = noCloud;
        transcripts = provider(tl, {{'A', std::move(words)}});
        audio = std::make_shared<FakeSpeechAudio>();
        auto opener = [this](const ModelSpec& model, std::optional<MicroUsd> cap) -> std::expected<std::unique_ptr<ISpeechSynth>, AiError> {
            openedModel = model.id;
            openedCap = cap;
            shared = std::make_shared<ScriptedSpeech>(model.id, 12.0);
            speech = shared.get();
            return std::make_unique<SharedSpeech>(shared);
        };
        dubs = std::make_shared<DirectDubServices>(opener, audio, scratch("studio"));
        auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<ai::edit::TranscriptServices>(transcripts), dubs});
        h = std::make_unique<CommandHarness>(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 1024, services},
                                             ai::edit::editingCommands());
        bare = std::make_unique<CommandHarness>(Timeline(kPicture), command::EnginePolicy{}, ai::edit::editingCommands());
    }

    /// A Thai caption track of three cues.
    std::uint64_t thaiCaptions(CommandHarness& harness) {
        const auto made = harness.ok("captions.addTrack", Json{{"name", "Captions (ไทย)"}, {"language", "th"}});
        const auto id = made.data["captionTrack"].get<std::uint64_t>();
        harness.ok("captions.addCue", Json{{"captionTrack", id}, {"start", tc(25)}, {"end", tc(75)}, {"text", "สวัสดีครับ ทุกคน"}});
        harness.ok("captions.addCue", Json{{"captionTrack", id}, {"start", tc(100)}, {"end", tc(125)}, {"text", "วันนี้เราจะไปตลาดน้ำ [bleep] กันครับ"}});
        harness.ok("captions.addCue", Json{{"captionTrack", id}, {"start", tc(130)}, {"end", tc(150)}, {"text", "[bleep]"}});
        return id;
    }

    const Sequence& sequence() const { return h->sequence(h->state()); }

    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
    std::shared_ptr<FakeSpeechAudio> audio;
    std::shared_ptr<DirectDubServices> dubs;
    std::shared_ptr<ScriptedSpeech> shared;
    ScriptedSpeech* speech = nullptr;
    std::string openedModel;
    std::optional<MicroUsd> openedCap;
    std::unique_ptr<CommandHarness> h;
    std::unique_ptr<CommandHarness> bare;
};

} // namespace

TEST_CASE("dub.create: a Dialogue Dub track, the dialogue ducked under it, provenance, all in one undo step",
          "[ai][global][dub][command]") {
    DubStudio s;
    const auto track = s.thaiCaptions(*s.h);
    const ProjectSnapshot before = s.h->state();
    const std::size_t undoBefore = s.h->session.undoCount();

    // The estimate sends nothing and changes nothing.
    const auto estimate = s.h->ok("dub.create", Json{{"captionTrack", track}, {"estimate", true}});
    CHECK(s.speech == nullptr);
    CHECK(estimate.data["lines"] == 2);
    CHECK(estimate.data["model"] == "eleven_v3");
    CHECK(estimate.data["voice"] == "JBFqnCBsd6RMkjVDRZzb");
    CHECK(estimate.data["track"] == "Dub (ไทย)");
    CHECK(estimate.data["estimate"]["usd"].get<double>() > 0.0);
    CHECK(s.h->state() == before);

    const auto done = s.h->ok("dub.create", Json{{"captionTrack", track}, {"maxCost", 1.0}});
    CHECK(s.h->session.undoCount() == undoBefore + 1);
    REQUIRE(s.speech != nullptr);
    CHECK(s.openedCap == MicroUsd{1'000'000});
    REQUIRE(s.speech->requests.size() == 2);
    for (const SpeechRequest& r : s.speech->requests) {
        CHECK(r.text.find("[bleep]") == std::string::npos); // a bleeped word is never voiced
    }
    CHECK(done.data["clips"] == 2);
    CHECK(done.data["duckedClips"].get<int>() >= 1);
    CHECK(done.data["fit"]["maxSpeed"].get<double>() <= 1.15 + 1e-9);

    const Sequence& seq = s.sequence();
    REQUIRE(seq.audioTracks.size() == 3);
    const Track& dub = *seq.audioTracks.back();
    CHECK(dub.name == "Dub (ไทย)");
    CHECK(dub.role == TrackRole::Dialogue);
    REQUIRE(dub.clips.size() == 2);
    const MediaItem* media = s.h->state()->findMedia(dub.clips[0].media);
    REQUIRE(media != nullptr);
    REQUIRE(media->generated); // the timeline badges it "AI"
    CHECK(media->generated->kind == "audio");
    CHECK(media->generated->operation == "dub");
    // The dialogue (A1) is ducked under the new lines; the music (A2) is not.
    CHECK(seq.audioTracks[0]->clips[0].properties()[ParamId::VolumeDb].curve() != nullptr);
    CHECK(seq.audioTracks[1]->clips[0].properties()[ParamId::VolumeDb].curve() == nullptr);
    requireValid(*s.h->state());

    REQUIRE(s.h->session.undo());
    CHECK(s.sequence().audioTracks.size() == 2);
    CHECK(s.h->state()->media.size() == before->media.size());
}

TEST_CASE("dub.create refuses without a cap, above its cap, without an approval host and in a no-cloud project",
          "[ai][global][dub][command][privacy]") {
    DubStudio s;
    const auto track = s.thaiCaptions(*s.h);
    CHECK(s.h->errorOf("dub.create", Json{{"captionTrack", track}}) == "InvalidParams");
    CHECK(s.h->errorOf("dub.create", Json{{"captionTrack", track}, {"maxCost", 0.000001}}) == "PermissionDenied");
    CHECK(s.speech == nullptr); // nothing was opened, let alone sent
    CHECK(s.h->errorOf("dub.create", Json{{"captionTrack", track}, {"maxCost", 1.0}, {"model", "eleven_flash_v2_5"}}) == "InvalidParams");
    CHECK(s.h->errorOf("dub.create", Json{{"captionTrack", track}, {"maxCost", 1.0}, {"model", "gpt-4o-mini-tts"}, {"voice", "nobody"}}) ==
          "InvalidParams");

    // The standalone CLI / MCP (no approval host) can estimate but not spend.
    const auto bareTrack = s.thaiCaptions(*s.bare);
    s.bare->ok("dub.create", Json{{"captionTrack", bareTrack}, {"estimate", true}});
    CHECK(s.bare->errorOf("dub.create", Json{{"captionTrack", bareTrack}, {"maxCost", 1.0}}) == "InvalidRequest");

    DubStudio locked(true);
    const auto lockedTrack = locked.thaiCaptions(*locked.h);
    CHECK(locked.h->errorOf("dub.create", Json{{"captionTrack", lockedTrack}, {"maxCost", 1.0}}) == "PermissionDenied");
    CHECK(locked.h->errorOf("narration.create", Json{{"text", "Hello."}, {"estimate", true}}) == "PermissionDenied");
}

TEST_CASE("Voice cloning: only with the rights confirmed, never for a redacted speaker, from the speaker's own speech",
          "[ai][global][dub][clone][privacy]") {
    DubStudio s(false, true);
    s.h->ok("captions.generateFromTranscript");
    const auto speakers = s.h->ok("transcript.speakers");
    REQUIRE(speakers.data["labels"].size() == 2);
    const auto host = speakers.data["labels"][0][3].get<std::uint64_t>();
    const auto guest = speakers.data["labels"][1][3].get<std::uint64_t>();
    const auto track = s.thaiCaptions(*s.h);
    const Json base{{"captionTrack", track}, {"maxCost", 1.0}, {"cloneVoice", true}};

    Json noRights = base;
    noRights["speaker"] = host;
    CHECK(s.h->errorOf("dub.create", noRights) == "PermissionDenied");
    Json redacted = base;
    redacted["speaker"] = guest;
    redacted["rightsConfirmed"] = true;
    CHECK(s.h->errorOf("dub.create", redacted) == "PermissionDenied");
    Json openai = base;
    openai["speaker"] = host;
    openai["rightsConfirmed"] = true;
    openai["model"] = "gpt-4o-mini-tts";
    CHECK(s.h->errorOf("dub.create", openai) == "InvalidParams");
    CHECK(s.speech == nullptr);

    Json ok = base;
    ok["speaker"] = host;
    ok["rightsConfirmed"] = true;
    const auto done = s.h->ok("dub.create", ok);
    CHECK(done.data["cloned"] == true);
    REQUIRE(s.speech != nullptr);
    CHECK(s.speech->deleted == std::vector<std::string>{"ClonedVoice42"});
    CHECK(s.audio->sampleMedia == s.h->tl.media('A'));
    REQUIRE_FALSE(s.audio->sampleRanges.empty());
    for (const TimeRange& r : s.audio->sampleRanges) {
        CHECK(r.end() <= frames(135)); // only the host's speech
    }
    CHECK_FALSE(std::filesystem::exists(s.audio->samplePath)); // the sample is removed after the run
}

// Review wave 5, A5-P2-15: words less than 0.35 s apart are one run of the sample, so a redacted word of someone else
// in the gap between two of the speaker's words was uploaded with it.
TEST_CASE("A voice-clone sample never holds redacted audio between the speaker's words", "[ai][global][dub][clone][privacy]") {
    // The host speaks every 0.5 s for 0.2 s (frames at 25 fps); the guest's "Bob" at 1.25-1.45 s is bleeped.
    std::vector<W> words;
    for (int k = 0; k < 12; ++k) {
        words.push_back(W{"word" + std::to_string(k), k * 12.5, k * 12.5 + 5.0, 0});
    }
    words.push_back(W{"Bob", 31.25, 36.25, 1});
    DubStudio s(false, false, transcript("en", words));
    Redaction r;
    r.media = s.h->tl.media('A');
    r.source = TimeRange::fromStartEnd(sec(1.25), sec(1.45));
    r.audio = RedactionAudio::Bleep;
    auto added = wabcut::edit::addRedaction(*s.h->state(), r);
    REQUIRE(added);
    REQUIRE(s.h->session.commit("Add Redaction", added->state));
    s.h->ok("captions.generateFromTranscript");
    const auto speakers = s.h->ok("transcript.speakers");
    REQUIRE(speakers.data["labels"].size() == 2);
    const auto host = speakers.data["labels"][0][3].get<std::uint64_t>();
    const auto track = s.thaiCaptions(*s.h);
    const auto done = s.h->ok("dub.create", Json{{"captionTrack", track}, {"maxCost", 1.0}, {"cloneVoice", true}, {"speaker", host},
                                                 {"rightsConfirmed", true}});
    CHECK(done.data["cloned"] == true);
    REQUIRE_FALSE(s.audio->sampleRanges.empty());
    for (const TimeRange& range : s.audio->sampleRanges) {
        INFO(secondsOf(range.start) << " - " << secondsOf(range.end()));
        CHECK_FALSE(range.overlaps(r.source));
    }
}

TEST_CASE("narration.create: script lines one after another on a Narration track, the rest ducked", "[ai][global][dub][narration]") {
    DubStudio s;
    const auto estimate = s.h->ok("narration.create", Json{{"text", "Welcome to Chiang Mai.\nThe old city has walls."}, {"estimate", true},
                                                           {"model", "gpt-4o-mini-tts"}});
    CHECK(estimate.data["lines"] == 2);
    CHECK(estimate.data["voice"] == defaultVoice("openai-speech"));
    CHECK(estimate.data["onBeats"] == false);
    const auto done = s.h->ok("narration.create", Json{{"text", "Welcome to Chiang Mai.\nThe old city has walls."}, {"maxCost", 1.0},
                                                       {"model", "gpt-4o-mini-tts"}, {"voice", "marin"}, {"style", "warm, unhurried"},
                                                       {"at", tc(50)}});
    REQUIRE(s.speech != nullptr);
    CHECK(s.speech->requests[0].style == "warm, unhurried");
    const Sequence& seq = s.sequence();
    const Track& narration = *seq.audioTracks.back();
    CHECK(narration.name == "Narration");
    CHECK(narration.role == TrackRole::Dialogue);
    REQUIRE(narration.clips.size() == 2);
    CHECK(narration.clips[0].start() == sec(2));
    CHECK(narration.clips[1].start() > narration.clips[0].end());
    const MediaItem* media = s.h->state()->findMedia(narration.clips[0].media);
    REQUIRE(media->generated);
    CHECK(media->generated->operation == "narration");
    CHECK(seq.audioTracks[1]->clips[0].properties()[ParamId::VolumeDb].curve() != nullptr); // music ducked too
    CHECK(done.data["fit"]["stretched"] == 0);
}

TEST_CASE("Every voice list names its model's default", "[ai][global][dub][voices]") {
    for (const char* api : {"elevenlabs-tts", "openai-speech", "gemini-tts"}) {
        INFO(api);
        CHECK_FALSE(builtinVoices(api).empty());
        CHECK(isValidVoice(api, defaultVoice(api)));
    }
    CHECK(defaultVoice("gemini-tts") == "Kore");
    CHECK(isValidVoice("elevenlabs-tts", "AnyLibraryVoice123"));
    CHECK_FALSE(isValidVoice("elevenlabs-tts", "../etc"));
    CHECK_FALSE(isValidVoice("openai-speech", "Kore"));
}

// Review wave 5, A5-P1-4: captions typed by hand or imported from an SRT keep the words as typed; the model masks them
// only where it draws or exports. A dub planned from them sent the redacted name to the voice provider and spoke it.
// The plan now speaks what may be translated: a cue over redacted speech whose words cannot be told apart says nothing.
TEST_CASE("A dub never speaks or uploads a typed caption over redacted speech", "[ai][global][dub][privacy]") {
    Timeline tl("V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|\n"
                "A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    Redaction r;
    r.media = tl.media('A');
    r.source = TimeRange::fromStartEnd(sec(2.5), sec(3.0));
    r.audio = RedactionAudio::Bleep;
    auto redacted = wabcut::edit::addRedaction(tl.state, r);
    REQUIRE(redacted);
    ProjectState state = std::move(redacted->state);
    CaptionTrack track;
    track.id = state.ids.allocate<CaptionTrackId>();
    track.name = "Imported";
    track.language = "en";
    track.cues = {CaptionCue{sec(0.2), sec(1.5), "Hello there", {}}, CaptionCue{sec(2.0), sec(2.0), "call John now", {}}};
    auto seq = std::make_shared<Sequence>(*state.findSequence(tl.seq));
    seq->captionTracks = {track};
    for (SequencePtr& s : state.sequences) {
        if (s->id == tl.seq) {
            s = seq;
        }
    }
    DubRequest request;
    request.captionTrack = track.id;
    const auto plan = planDub(state, tl.seq, request, ModelRegistry::builtin(), nullptr, todayUtc());
    REQUIRE(plan);
    for (const DubLine& line : plan->lines) {
        INFO(line.text);
        CHECK(line.text.find("John") == std::string::npos);
    }
    REQUIRE(plan->lines.size() == 1); // the masked cue says nothing at all
    CHECK(plan->lines[0].text == "Hello there");
    REQUIRE_FALSE(plan->warnings.empty());
    CHECK(plan->warnings.back().find("not spoken") != std::string::npos);
}

// Review wave 5, S5-P1-2: captions made from the transcript, then a redaction added where the caption refresh does not
// run (the app's Masks panel edited redactions with the model ops). The stored cue still says "Thanks"; the dub must
// not send it to the voice provider, and captions.get must not hand it to the assistant or an MCP agent.
TEST_CASE("A redaction added after the captions keeps its words out of the dub and captions.get", "[ai][global][dub][privacy]") {
    DubStudio s;
    s.h->ok("captions.generateFromTranscript");
    REQUIRE(s.sequence().captionTracks.size() == 1);
    const CaptionTrackId english = s.sequence().captionTracks.front().id;
    Redaction r;
    r.media = s.h->tl.media('A');
    r.source = TimeRange::fromStartEnd(frames(136), frames(142)); // "Thanks"
    r.audio = RedactionAudio::Bleep;
    auto added = wabcut::edit::addRedaction(*s.h->state(), r);
    REQUIRE(added);
    REQUIRE(s.h->session.commit("Add Redaction", added->state));
    bool stored = false;
    for (const CaptionCue& cue : s.sequence().captionTracks.front().cues) {
        stored = stored || cue.text.find("Thanks") != std::string::npos;
    }
    REQUIRE(stored); // the precondition: the stored text still has the word

    DubRequest request;
    request.captionTrack = english;
    const auto plan = planDub(*s.h->state(), s.h->tl.seq, request, ModelRegistry::builtin(), s.transcripts.get(), todayUtc());
    REQUIRE(plan);
    REQUIRE_FALSE(plan->lines.empty());
    for (const DubLine& line : plan->lines) {
        INFO(line.text);
        CHECK(line.text.find("Thanks") == std::string::npos);
    }
    const auto got = s.h->ok("captions.get", Json{{"captionTrack", english.value}});
    for (const Json& cue : got.data["cues"]) {
        INFO(cue.dump());
        CHECK(cue[2].get<std::string>().find("Thanks") == std::string::npos);
    }
    CHECK(got.data.contains("redactedCues"));
}

namespace {

/// FakeSpeechAudio that really writes its files (the samples as raw floats), replacing any file there like a rename does.
class WritingSpeechAudio final : public SpeechAudio {
public:
    std::expected<std::vector<float>, Error> decode(const SpeechResult& speech, int rate) override { return fake.decode(speech, rate); }
    std::vector<float> stretch(std::span<const float> samples, int rate, double speed) override { return fake.stretch(samples, rate, speed); }
    std::expected<MediaItem, Error> write(const std::filesystem::path& file, std::span<const float> samples, int rate) override {
        std::ofstream(file, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(float)));
        return fake.write(file, samples, rate);
    }
    std::expected<std::filesystem::path, Error> extractSample(const MediaItem& media, int stream, std::span<const TimeRange> ranges,
                                                              const std::filesystem::path& file) override {
        return fake.extractSample(media, stream, ranges, file);
    }
    FakeSpeechAudio fake;
};

std::string bytesOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

// Review wave 5, A5-P1-6: the file was named by the text and voice only, so a second run of the same lines (another
// version of the sequence, a retry) replaced the first run's audio under its clips - or failed after every line was
// billed when the first file was open. Files are named by their samples now and never replaced.
TEST_CASE("A second dub run never replaces the first run's audio", "[ai][global][dub][pipeline]") {
    DubJob job;
    job.lines = {DubLine{"Hello there", sec(0), sec(2), {}, 0}, DubLine{"See you soon", sec(3), sec(2), {}, 1}};
    job.voice.voiceId = "JBFqnCBsd6RMkjVDRZzb";
    job.directory = scratch("rerun");
    WritingSpeechAudio audio;
    ScriptedSpeech first("eleven_v3", 10.0);
    auto one = runDub(job, first, audio, {});
    REQUIRE(one);
    const std::filesystem::path firstFile = one->audio.item.locator.path;
    REQUIRE(std::filesystem::exists(firstFile));
    const std::string firstBytes = bytesOf(firstFile);

    // The same lines and voice, spoken differently (a provider is not deterministic).
    ScriptedSpeech second("eleven_v3", 12.0);
    auto two = runDub(job, second, audio, {});
    REQUIRE(two);
    const std::filesystem::path secondFile = two->audio.item.locator.path;
    CHECK(secondFile != firstFile);
    CHECK(bytesOf(firstFile) == firstBytes); // the first run's clips still play what they played

    // Exactly the same samples again: still a file of its own, the first untouched.
    ScriptedSpeech again("eleven_v3", 10.0);
    auto three = runDub(job, again, audio, {});
    REQUIRE(three);
    CHECK(std::filesystem::path(three->audio.item.locator.path) != firstFile);
    CHECK(bytesOf(firstFile) == firstBytes);
    std::error_code ec;
    std::filesystem::remove_all(job.directory, ec);
}
