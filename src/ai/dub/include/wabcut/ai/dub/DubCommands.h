#pragma once

// dub.create and narration.create (AI-12, docs/ai.md "Going global"): AI voice-over in the user's voice provider
// (BYOK: ElevenLabs, OpenAI, Gemini), placed as one staged edit.
//
//   dub.create        a caption track (usually a translated one) spoken line by line, each line fitted to its cue
//                     (sped up by at most maxStretch, pitch kept; else moved and flagged), on a new "Dub (<language>)"
//                     audio track with the Dialogue role, the original dialogue ducked under it
//   narration.create  a script spoken line by line, one line per rough-cut beat when the sequence is a rough cut
//                     (AI-11), else one after another; a "Narration" track, everything else ducked under it
//
// Both are paid uploads of text, so they follow AI-10's generation policy: "estimate" prices a run without sending
// anything, anywhere; a real run needs a host that has the user's approval of the exact request (DubServices: the
// app's Dub dialog, after consent and with a hard cap). Standalone CLI/MCP and the assistant cannot spend: the app
// turns an assistant's call into a prefilled Dub dialog. A no-cloud project refuses both.
//
// Voice cloning is opt-in: "cloneVoice" with "rightsConfirmed" (the user has the speaker's permission) and the
// speaker; refused for a speaker with any redacted speech, and the cloned voice is deleted after the run.

#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/dub/Pipeline.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::ai::dub {

struct DubRequest {
    DubKind kind = DubKind::Dub;
    CaptionTrackId captionTrack;   ///< dub: the cues (invalid: the sequence's only caption track)
    std::string text;              ///< narration: the script
    bool onBeats = true;           ///< narration on a rough cut: line i on beat i
    std::optional<Ticks> at;       ///< narration: where the first line starts (default 0)
    Ticks gap{kTicksPerSecond / 2};
    std::string language;          ///< ISO 639-1; dub default: the caption track's
    std::string model;             ///< a model with the speak capability; empty: the host's (else eleven_v3)
    std::string voice;             ///< empty: the model's default voice
    std::string style;             ///< delivery instructions, where the model takes them
    double maxStretch = 0.15;
    std::optional<double> duckDb;  ///< default -18 (dub) / -12 (narration); 0 = none
    std::string trackName;
    bool cloneVoice = false;
    bool rightsConfirmed = false;
    SpeakerId cloneSpeaker;
    std::optional<MicroUsd> maxCost; ///< the run's hard cap
};

/// Recordings of the speaker to clone: ranges of one media item's audio stream.
struct CloneSource {
    MediaId media;
    int stream = 0;
    std::vector<TimeRange> ranges; ///< media time
    Ticks total{};
};

struct DubPlan {
    DubRequest request;
    SequenceId sequence;
    ModelSpec model;
    std::string voice;
    std::string language;
    std::string trackName;
    double duckDb = -18.0;
    std::vector<DubLine> lines;
    std::vector<TimeRange> beats;  ///< narration on a rough cut
    std::size_t characters = 0;
    CostEstimate cost;
    std::optional<CloneSource> clone;
    std::vector<std::string> warnings;
};

struct PlanError {
    command::ErrorCode code = command::ErrorCode::InvalidParams;
    std::string message;
};

/// Checks a request and plans it without sending anything: the lines, the model and voice, the language, the cost,
/// and for a clone the speaker's recordings (refused without rightsConfirmed, for a speaker with redacted speech, or
/// with less than 5 s of their speech). Errors: InvalidParams, NotFound, PermissionDenied (no-cloud, clone policy).
/// r07 (QA-07): `onThisComputer` says whether the chosen voice model runs on this computer (a custom provider on
/// localhost): a no-cloud project may use such a model. Without it a no-cloud project refuses every run.
std::expected<DubPlan, PlanError> planDub(const ProjectState& state, SequenceId sequence, const DubRequest& request,
                                      const ModelRegistry& registry, const ai::edit::TranscriptProvider* transcripts,
                                      std::chrono::year_month_day today,
                                      const std::function<bool(const ModelSpec&)>& onThisComputer = {});

/// Where a run stops and asks: the host that may spend the user's money.
class DubServices : public command::CommandServices {
public:
    /// Runs the plan (or, for an assistant, proposes it for approval) and returns the staged edit and its report.
    virtual command::CommandOutput dub(const DubPlan& plan, command::ExecutionContext& context) = 0;
    /// The model a request without one uses (the first voice model with a key), or "" for the built-in default.
    virtual std::string defaultModel(std::string_view language) const {
        (void)language;
        return {};
    }
    /// r07 (QA-02): the host's model registry (the app's: catalogue, models the user added, custom providers), a copy
    /// made on the host's thread; null = the shipped file (ModelRegistry::builtin()).
    virtual std::shared_ptr<const ModelRegistry> registry() const { return {}; }
    /// r07 (QA-07): whether a voice model runs on this computer (the host knows the addresses the user set).
    virtual bool onThisComputer(const ModelSpec& model) const {
        (void)model;
        return false;
    }
    /// True for a host that only proposes (the app's assistant: a prefilled Dub dialog where the user sets the cap,
    /// consents and runs it): such a request needs no "maxCost".
    virtual bool proposes() const { return false; }
};

/// The runner itself: guarded speech from `speech` (AiPlatform::speech with the user's approval), decoding and files
/// from `audio`, generated files in `directory`.
class DirectDubServices final : public DubServices {
public:
    using SpeechOpener = std::function<std::expected<std::unique_ptr<ISpeechSynth>, AiError>(const ModelSpec& model, std::optional<MicroUsd> cap)>;

    DirectDubServices(SpeechOpener speech, std::shared_ptr<SpeechAudio> audio, std::filesystem::path directory, std::string defaultModel = {},
                      std::shared_ptr<const ModelRegistry> registry = {}, std::vector<std::string> localModels = {})
        : m_speech(std::move(speech)), m_audio(std::move(audio)), m_directory(std::move(directory)), m_defaultModel(std::move(defaultModel)),
          m_registry(std::move(registry)), m_localModels(std::move(localModels)) {}

    command::CommandOutput dub(const DubPlan& plan, command::ExecutionContext& context) override;
    std::string defaultModel(std::string_view) const override { return m_defaultModel; }
    std::shared_ptr<const ModelRegistry> registry() const override { return m_registry; }
    bool onThisComputer(const ModelSpec& model) const override {
        return std::find(m_localModels.begin(), m_localModels.end(), model.id) != m_localModels.end();
    }

private:
    SpeechOpener m_speech;
    std::shared_ptr<SpeechAudio> m_audio;
    std::filesystem::path m_directory;
    std::string m_defaultModel;
    std::shared_ptr<const ModelRegistry> m_registry;
    std::vector<std::string> m_localModels; ///< voice models on this computer (QA-07)
};

/// Runs a plan to the staged state (what DirectDubServices does; the app's Dub dialog calls it on its worker).
struct DubOutcome {
    DubRun run;
    Assembly assembly;
};
std::expected<DubOutcome, AiError> executeDub(const ProjectState& state, const DubPlan& plan, ISpeechSynth& speech, SpeechAudio& audio,
                                              const std::filesystem::path& directory, std::stop_token stop,
                                              const LineProgress& progress = {});

command::Json planToJson(const DubPlan& plan);
command::Json outcomeToJson(const DubPlan& plan, const DubOutcome& outcome);

/// The request a command's parameters describe (shared with the app's proposal path).
DubRequest requestFromParams(command::ExecutionContext& context, DubKind kind);

void registerDubCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::dub
