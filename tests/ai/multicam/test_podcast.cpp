// AI-13 the one-click podcast / interview workflow on the synthetic 3-camera, 2-microphone recording: the plan (every
// step on or off, and why), each step run on its own (its own undo step, skippable), resuming after a crash from the
// journal and the last C.9 history checkpoint, chapters from topics, and workflow.podcast through the engine.

#include "CommandHarness.h"
#include "PodcastTestUtil.h"

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/workflow/Podcast.h>
#include <wabcut/ai/workflow/WorkflowCommands.h>
#include <wabcut/model/History.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::ai::multicam::test;
using namespace wabcut::ai::workflow;
using command::Json;

namespace {

const StepPlan& stepOf(const PodcastPlan& plan, StepKind kind) {
    const auto it = std::ranges::find_if(plan.steps, [&](const StepPlan& s) { return s.kind == kind; });
    INFO("step " << toString(kind));
    REQUIRE(it != plan.steps.end());
    return *it;
}

/// Checks the finished episode: the picture switches between the cameras, the mics are enhanced on dialogue tracks,
/// chapters are markers.
void checkEpisode(const ProjectState& state, const PodcastContext& context, bool captions) {
    const Sequence* seq = state.findSequence(context.sequence);
    REQUIRE(seq != nullptr);
    CHECK(seq->name == "Episode 12");
    const Sequence* source = state.findSequence(context.multicam);
    REQUIRE(source != nullptr);
    REQUIRE(source->multicam);
    CHECK(source->multicam->angles.size() == 5);
    // The picture: pieces of the multicam source showing more than one camera.
    std::set<std::size_t> shown;
    for (const Clip& c : seq->videoTracks.front()->clips) {
        CHECK(c.sequence == context.multicam);
        shown.insert(c.angles.first());
    }
    CHECK(shown.size() >= 3);
    // The microphones: on their own dialogue tracks, enhanced.
    std::size_t dialogue = 0;
    for (const TrackPtr& t : seq->audioTracks) {
        if (t->role == TrackRole::Dialogue && !t->clips.empty()) {
            ++dialogue;
        }
    }
    CHECK(dialogue == 2);
    // In sync after the clean-up: the picture and both microphones end together, shorter than the recordings.
    const Ticks pictureEnd = seq->videoTracks.front()->clips.back().end();
    for (const TrackPtr& t : seq->audioTracks) {
        if (t->role == TrackRole::Dialogue && !t->clips.empty()) {
            CHECK(t->clips.back().end() == pictureEnd);
        }
    }
    CHECK(pictureEnd < secs(40));
    // Chapters.
    std::size_t chapters = 0;
    for (const Marker& mk : seq->markers) {
        chapters += mk.type == MarkerType::Chapter ? 1 : 0;
    }
    CHECK(chapters >= 2);
    CHECK(context.youtubeChapters.starts_with("0:00 "));
    // Captions: a caption track only when the step ran.
    CHECK(seq->captionTracks.empty() != captions);
}

} // namespace

TEST_CASE("The podcast plan: every step on or off, and why; local and free", "[ai][multicam][podcast]") {
    PodcastStudio p;
    const PodcastPlan plan = planPodcast(p.studio.state, p.options(), {}, p.services.get());
    REQUIRE(plan.steps.size() == 10);
    CHECK(plan.cost == "local, free");
    CHECK(stepOf(plan, StepKind::Multicam).enabled);
    CHECK(stepOf(plan, StepKind::Multicam).command == "multicam.create");
    CHECK(stepOf(plan, StepKind::Sequence).enabled);
    CHECK_FALSE(stepOf(plan, StepKind::Transcribe).enabled);
    CHECK(stepOf(plan, StepKind::Transcribe).reason == "already transcribed");
    CHECK(stepOf(plan, StepKind::Enhance).enabled);
    CHECK(stepOf(plan, StepKind::Loudness).title == "Loudness to -16 LUFS");
    CHECK(stepOf(plan, StepKind::AutoSwitch).detail == "the calm preset");
    CHECK_FALSE(stepOf(plan, StepKind::Shorts).enabled);

    // Unticked steps, and a host without the services.
    PodcastOptions o = p.options();
    o.skip = {StepKind::Captions, StepKind::Enhance};
    o.shorts = true;
    const PodcastPlan bare = planPodcast(p.studio.state, o, {}, nullptr);
    CHECK(stepOf(bare, StepKind::Captions).reason == "unticked");
    CHECK(stepOf(bare, StepKind::Enhance).reason == "unticked");
    CHECK(stepOf(bare, StepKind::Multicam).reason == "no audio sync here");
    CHECK(stepOf(bare, StepKind::Transcribe).reason == "no transcription here");
    CHECK(stepOf(bare, StepKind::Loudness).reason == "no loudness measurement here");
    CHECK(stepOf(bare, StepKind::Cleanup).reason == "no transcripts (transcribe the microphones)"); // nothing will be transcribed
    CHECK(stepOf(bare, StepKind::Chapters).reason == "no transcripts (transcribe the microphones)");
    CHECK(stepOf(bare, StepKind::AutoSwitch).enabled); // the microphones' levels are enough
    CHECK(stepOf(bare, StepKind::Shorts).enabled);
    CHECK_FALSE(bare.warnings.empty());

    // What an earlier run made is not made again.
    PodcastContext made;
    made.multicam = SequenceId{77};
    CHECK(stepOf(planPodcast(p.studio.state, p.options(), made, p.services.get()), StepKind::Multicam).reason == "already made");
}

TEST_CASE("The podcast workflow step by step: each step its own undo step, skippable, undone one by one", "[ai][multicam][podcast]") {
    PodcastStudio p;
    PodcastOptions o = p.options();
    o.skip = {StepKind::Captions};
    EditSession session(p.studio.state);
    const ProjectSnapshot original = session.snapshot();
    PodcastContext context;
    std::vector<StepKind> ran;
    for (const StepPlan& s : planPodcast(*session.snapshot(), o, context, p.services.get()).steps) {
        if (!s.enabled) {
            continue;
        }
        const std::size_t undo = session.undoCount();
        auto r = runStep(s.kind, *session.snapshot(), context, o, p.services, nullptr);
        INFO(toString(s.kind) << ": " << (r ? r->report.dump() : r.error().message));
        REQUIRE(r);
        REQUIRE(session.commit("Podcast workflow: " + s.title, r->state));
        CHECK(session.undoCount() == undo + 1);
        ran.push_back(s.kind);
    }
    CHECK(ran == std::vector<StepKind>{StepKind::Multicam, StepKind::Sequence, StepKind::Enhance, StepKind::Loudness, StepKind::Cleanup,
                                       StepKind::AutoSwitch, StepKind::Chapters});
    CHECK(p.enhancer->renders == 2);
    CHECK(p.loudness->calls >= 1);
    checkEpisode(*session.snapshot(), context, false);
    const ProjectSnapshot finished = session.snapshot();
    for (const MediaId m : p.studio.mics()) {
        CHECK(finished->findMedia(m)->audioTake(0, kEnhanceSpeechTake) != nullptr);
    }

    // The captions afterwards, on their own.
    auto captions = runStep(StepKind::Captions, *session.snapshot(), context, o, p.services, nullptr);
    REQUIRE(captions);
    REQUIRE(session.commit("Podcast workflow: Captions", captions->state));
    CHECK_FALSE(session.snapshot()->findSequence(context.sequence)->captionTracks.empty());

    // Undone step by step, back to the recordings.
    for (std::size_t i = 0; i < ran.size() + 1; ++i) {
        REQUIRE(session.undo());
    }
    CHECK(*session.snapshot() == *original);

    // A later step without the episode: a clear refusal.
    PodcastContext none;
    const auto early = runStep(StepKind::AutoSwitch, *original, none, o, p.services, nullptr);
    REQUIRE_FALSE(early);
    CHECK(early.error().message.find("lay out the episode first") != std::string::npos);
}

TEST_CASE("The podcast workflow resumes after a crash from its journal and the last history checkpoint", "[ai][multicam][podcast]") {
    PodcastStudio p;
    const PodcastOptions o = p.options();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "wabcut-podcast-resume";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path project = dir / "Episode 12.wabcut";
    const std::filesystem::path journalFile = journalFileFor(project);
    CHECK(journalFile.filename() == "podcast-workflow.json");

    // The first run: three steps, each checkpointed, then the app dies.
    {
        EditSession session(p.studio.state);
        auto history = ProjectHistory::open(ProjectHistory::folderFor(project));
        REQUIRE(history);
        Journal journal;
        journal.options = podcastOptionsToJson(o);
        for (const StepKind k : {StepKind::Multicam, StepKind::Sequence, StepKind::Enhance}) {
            auto r = runStep(k, *session.snapshot(), journal.context, o, p.services, nullptr);
            REQUIRE(r);
            REQUIRE(session.commit(std::string(toString(k)), r->state));
            auto rev = history->checkpoint(*session.snapshot(), HistoryKind::Agent, "Podcast workflow: " + std::string(toString(k)));
            REQUIRE(rev);
            journal.steps.emplace_back(k, "done");
            journal.revisions.push_back(std::to_string(rev->id));
            REQUIRE(saveJournal(journalFile, journal));
        }
        journal.steps.emplace_back(StepKind::Loudness, "failed"); // the crash
        REQUIRE(saveJournal(journalFile, journal));
    }

    // After the restart: the journal says where it was, history has the project as it was.
    auto journal = loadJournal(journalFile);
    REQUIRE(journal);
    const PodcastOptions resumed = podcastOptionsFromJson(journal->options);
    CHECK(resumed.cameras == o.cameras);
    CHECK(resumed.name == "Episode 12");
    REQUIRE(journal->context.sequence.isValid());
    auto history = ProjectHistory::open(ProjectHistory::folderFor(project));
    REQUIRE(history);
    auto restored = history->load(std::stoull(journal->revisions.back()));
    REQUIRE(restored);
    CHECK(restored->findSequence(journal->context.sequence) != nullptr);

    const PodcastPlan plan = planPodcast(*restored, resumed, journal->context, p.services.get());
    CHECK(stepOf(plan, StepKind::Multicam).reason == "already made");
    CHECK(stepOf(plan, StepKind::Sequence).reason == "already made");
    REQUIRE(journal->next(plan) == StepKind::Loudness);
    EditSession session(*restored);
    PodcastContext context = journal->context;
    while (const auto k = journal->next(plan)) {
        auto r = runStep(*k, *session.snapshot(), context, resumed, p.services, nullptr);
        INFO(toString(*k));
        REQUIRE(r);
        REQUIRE(session.commit(std::string(toString(*k)), r->state));
        journal->steps.emplace_back(*k, "done");
    }
    CHECK(p.enhancer->renders == 2); // not again
    checkEpisode(*session.snapshot(), context, true);
    std::filesystem::remove_all(dir);
}

// Review wave 5, A5-P1-8. saveJournal named its temporary file file.string() + ".part", and MSVC's narrow string()
// throws std::system_error for a name outside the ANSI code page (emoji in every code page, Thai on an English
// Windows): the podcast step that writes the journal ended the app. Fails today with that exception.
TEST_CASE("The podcast journal saves and loads in a folder named in Thai with an emoji", "[ai][multicam][podcast]") {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "wabcut-podcast-utf8";
    std::filesystem::remove_all(root);
    const std::filesystem::path project = root / u8"วิดีโอ 😀" / u8"ตอนที่ 12 😀.wabcut";
    const std::filesystem::path journalFile = journalFileFor(project);
    Journal journal;
    journal.steps.emplace_back(StepKind::Multicam, "done");
    journal.revisions.push_back("7");

    std::expected<void, Error> saved;
    REQUIRE_NOTHROW(saved = saveJournal(journalFile, journal));
    REQUIRE(saved);
    CHECK(std::filesystem::is_regular_file(journalFile));
    std::filesystem::path part = journalFile;
    part += ".part";
    CHECK_FALSE(std::filesystem::exists(part));

    std::expected<Journal, Error> loaded = std::unexpected(Error{ErrorCode::NotFound, "not loaded"});
    REQUIRE_NOTHROW(loaded = loadJournal(journalFile));
    REQUIRE(loaded);
    REQUIRE(loaded->steps.size() == 1);
    CHECK(loaded->steps.front().first == StepKind::Multicam);
    CHECK(loaded->revisions == std::vector<std::string>{"7"});
    std::filesystem::remove_all(root);
}

TEST_CASE("Chapters come from the topics: the first at 0, each long enough, at most the maximum", "[ai][multicam][podcast]") {
    PodcastStudio p;
    PodcastContext context;
    PodcastOptions o = p.options();
    ProjectState state = p.studio.state;
    for (const StepKind k : {StepKind::Multicam, StepKind::Sequence}) {
        auto r = runStep(k, state, context, o, p.services, nullptr);
        REQUIRE(r);
        state = r->state;
    }
    const auto st = ai::edit::SequenceTranscript::build(state, context.sequence, *p.studio.transcripts);
    const std::vector<Chapter> chapters = chaptersFromTopics(st, 10.0, 12);
    REQUIRE(chapters.size() == 2);
    CHECK(chapters[0].start == Ticks{});
    CHECK(chapters[0].title.find("offee") != std::string::npos);
    CHECK(chapters[1].start >= secs(16.0));
    CHECK(chapters[1].start <= secs(18.0));
    CHECK(chaptersFromTopics(st, 30.0, 12).size() == 1); // too short for two 30 s chapters
    CHECK(chaptersFromTopics(st, 10.0, 1).size() == 1);
}

TEST_CASE("workflow.podcast: the plan without changes, then the whole run as one undo step", "[ai][multicam][podcast][command]") {
    PodcastStudio p;
    CommandHarness h(p.studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, p.services}, ai::edit::editingCommands());
    h.session.reset(p.studio.state);
    const ProjectSnapshot before = h.state();
    Json media = Json::array();
    for (const char m : {'A', 'B', 'C', 'D', 'E'}) {
        media.push_back(p.studio.tl.media(m).value);
    }

    const auto plan = h.ok("workflow.podcast", Json{{"media", media}, {"name", "Episode 12"}, {"minChapterSeconds", 10}});
    CHECK(h.state() == before);
    CHECK(plan.data["cost"] == "local, free");
    REQUIRE(plan.data["steps"].size() == 10);
    CHECK(plan.data["steps"][2]["reason"] == "already transcribed");

    const std::size_t undo = h.session.undoCount();
    const auto done = h.ok("workflow.podcast",
                           Json{{"media", media}, {"name", "Episode 12"}, {"minChapterSeconds", 10}, {"skip", Json::array({"captions"})}, {"run", true}});
    CHECK(h.session.undoCount() == undo + 1);
    std::size_t doneSteps = 0;
    for (const Json& r : done.data["results"]) {
        doneSteps += r["status"] == "done" ? 1 : 0;
    }
    CHECK(doneSteps == 7);
    const PodcastContext context = contextFromJson(done.data["context"]);
    checkEpisode(*h.state(), context, false);
    REQUIRE_FALSE(done.createdIds.empty());

    // Then just the captions, continuing from the context: another undo step.
    h.ok("workflow.podcast", Json{{"context", done.data["context"]}, {"steps", Json::array({"captions"})}, {"run", true}});
    CHECK_FALSE(h.state()->findSequence(context.sequence)->captionTracks.empty());
    REQUIRE(h.session.undo());
    REQUIRE(h.session.undo());
    CHECK(h.state() == before);

    CHECK(h.errorOf("workflow.podcast", Json{{"media", media}, {"skip", Json::array({"dance"})}}) == "InvalidParams");
    CHECK(h.errorOf("workflow.podcast", Json{{"name", "x"}}) == "InvalidParams");
    // A dry run of the whole workflow changes nothing.
    const auto dry = h.run(Json{{"command", "workflow.podcast"}, {"params", Json{{"media", media}, {"run", true}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(h.state() == before);
}

TEST_CASE("workflow.podcast: a step that fails keeps the steps before it and stops the run", "[ai][multicam][podcast][command]") {
    PodcastStudio p;
    class BrokenMeter final : public wabcut::audio::edit::LoudnessAnalyzer {
    public:
        std::expected<wabcut::audio::edit::LoudnessStats, Error> measure(const ProjectState&, SequenceId, const wabcut::audio::edit::LoudnessTarget&,
                                                                         std::optional<TimeRange>, command::ExecutionControl*) override {
            return std::unexpected(Error{ErrorCode::IoError, "the meter broke"});
        }
    };
    auto audio = std::make_shared<wabcut::audio::edit::AudioServices>(std::make_shared<BrokenMeter>(), nullptr);
    audio->enhancer = p.enhancer;
    auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{audio, p.services});
    CommandHarness h(p.studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, services}, ai::edit::editingCommands());
    h.session.reset(p.studio.state);
    const ProjectSnapshot before = h.state();
    Json media = Json::array();
    for (const char m : {'A', 'B', 'C', 'D', 'E'}) {
        media.push_back(p.studio.tl.media(m).value);
    }
    const auto done = h.ok("workflow.podcast", Json{{"media", media}, {"run", true}});
    std::vector<std::string> statuses;
    for (const Json& r : done.data["results"]) {
        statuses.push_back(r["step"].get<std::string>() + ":" + r["status"].get<std::string>());
    }
    CHECK(statuses == std::vector<std::string>{"multicam:done", "sequence:done", "transcribe:skipped", "enhance:done", "loudness:failed"});
    REQUIRE_FALSE(done.warnings.empty());
    CHECK(done.warnings.back().find("the meter broke") != std::string::npos);
    const PodcastContext context = contextFromJson(done.data["context"]);
    CHECK(h.state()->findSequence(context.sequence) != nullptr); // the episode is there
    REQUIRE(h.session.undo());
    CHECK(h.state() == before);
}
