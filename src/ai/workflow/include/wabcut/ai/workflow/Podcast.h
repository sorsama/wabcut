#pragma once

// The one-click podcast / interview workflow (AI-13, docs/ai.md "Podcast and interview editing"): from camera and
// microphone recordings to an edited, captioned, chaptered episode, as a list of steps that are each previewable,
// skippable and their own undo step:
//   multicam    multicam.create, synced by audio (A.5)
//   sequence    a new sequence: the multicam picture on V1, each microphone on its own Dialogue track, in sync
//               (so transcript clean-up, captions, Enhance Speech and loudness work on ordinary clips)
//   transcribe  transcript.transcribe of the microphones (local), unless they have transcripts
//   enhance     audio.enhanceSpeech on the microphones (A.4)
//   loudness    audio.normalize to the podcast target (-16 LUFS, A.3)
//   cleanup     transcript.cleanup: fillers and long pauses (AI-3)
//   autoswitch  multicam.autoSwitch with a preset
//   captions    captions.generateFromTranscript with a caption preset (C.7)
//   chapters    chapter markers from the transcript's topics (AI-9's topic segmentation), and YouTube chapters text
//   shorts      (off by default) shorts.find and shorts.create (AI-9)
// Every step runs through the editing catalog with the host's services, so it does exactly what the command does.
// Steps share what earlier steps made (PodcastContext); a journal of the run (done steps, history revisions) lets
// the app resume after a crash from the last C.9 history checkpoint. Everything is local: no cloud, no cost.

#include <wabcut/command/Command.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::edit {
class SequenceTranscript;
}

namespace wabcut::ai::workflow {

enum class StepKind { Multicam, Sequence, Transcribe, Enhance, Loudness, Cleanup, AutoSwitch, Captions, Chapters, Shorts };
std::string_view toString(StepKind kind);
std::optional<StepKind> stepFromString(std::string_view text);
std::vector<StepKind> allSteps();

struct PodcastOptions {
    std::vector<MediaId> cameras;      ///< the picture angles
    std::vector<MediaId> mics;         ///< the microphones (audio-only recordings); none: the cameras' sound
    std::string name = "Podcast";
    std::string sync = "audio";        ///< multicam.create's sync
    double enhanceStrength = 70.0;
    double lufs = -16.0;
    bool fillers = true;
    double silences = 1.0;             ///< pauses longer than this (s) are shortened; 0 = keep pauses
    std::string switchPreset = "calm";
    /// Who is on which camera, for the switching ([{angle, role, speaker}] as multicam.autoSwitch takes them; angle 1 =
    /// the first camera, the microphones after the cameras). Empty: the analysis guesses.
    command::Json angles = command::Json::array();
    std::string captionPreset = "Podcast";
    std::size_t maxChapters = 12;
    double minChapterSeconds = 60.0;
    bool shorts = false;
    std::size_t shortsCount = 3;
    std::set<StepKind> skip;           ///< the user's unticks
};

/// What earlier steps made, for the later ones (and for resuming).
struct PodcastContext {
    SequenceId multicam;
    SequenceId sequence;
    std::vector<MediaId> transcribed;
    std::string youtubeChapters;
};

command::Json contextToJson(const PodcastContext& context);
PodcastContext contextFromJson(const command::Json& json);

struct StepPlan {
    StepKind kind = StepKind::Multicam;
    std::string title;
    std::string detail;
    std::string command;     ///< the catalog command it runs ("" = the workflow's own edit)
    bool enabled = true;
    std::string reason;      ///< why it is off ("unticked", "already transcribed", "no Enhance Speech here")
    bool heavy = false;      ///< decodes media or runs a model (minutes on long recordings)
};

struct PodcastPlan {
    std::vector<StepPlan> steps;
    std::string cost = "local, free";
    std::vector<std::string> warnings;
};

/// The steps for these options on this project with these services (a step whose service is missing is off, with
/// the reason).
PodcastPlan planPodcast(const ProjectState& state, const PodcastOptions& options, const PodcastContext& context,
                        command::CommandServices* services);

struct StepResult {
    ProjectState state;
    command::Json report = command::Json::object();
    std::vector<std::string> warnings;
};

/// Runs one step on `state` through the editing catalog (a scratch session with `services`, the outer control's
/// checkpoints) and records what it made in `context`. Errors: the step's command's error.
std::expected<StepResult, command::CommandError> runStep(StepKind kind, const ProjectState& state, PodcastContext& context,
                                                        const PodcastOptions& options, std::shared_ptr<command::CommandServices> services,
                                                        command::ExecutionControl* control);

// ---- Chapters ----------------------------------------------------------------------------------------------------

struct Chapter {
    Ticks start{};
    std::string title;
};

/// Chapters from the transcript's topic changes: the first at 0, each at least `minSeconds` long, at most `max`
/// (the strongest topic changes win), titled from their first sentence.
std::vector<Chapter> chaptersFromTopics(const edit::SequenceTranscript& transcript, double minSeconds, std::size_t max);

// ---- Resuming after a crash --------------------------------------------------------------------------------------

struct Journal {
    command::Json options = command::Json::object(); ///< the run's options (podcastOptionsToJson)
    PodcastContext context;
    std::vector<std::pair<StepKind, std::string>> steps; ///< status: "done", "skipped", "failed"
    std::vector<std::string> revisions;                  ///< history revision after each done step
    std::string updatedAt;

    std::optional<StepKind> next(const PodcastPlan& plan) const; ///< the first enabled step not done or skipped
};

command::Json podcastOptionsToJson(const PodcastOptions& options);
PodcastOptions podcastOptionsFromJson(const command::Json& json);
command::Json journalToJson(const Journal& journal);
std::optional<Journal> journalFromJson(const command::Json& json);
/// Written atomically (a temporary file, then renamed).
std::expected<void, Error> saveJournal(const std::filesystem::path& file, const Journal& journal);
std::expected<Journal, Error> loadJournal(const std::filesystem::path& file);
/// Where the app keeps a project's journal: "<project>.history/podcast-workflow.json".
std::filesystem::path journalFileFor(const std::filesystem::path& projectFile);

} // namespace wabcut::ai::workflow
