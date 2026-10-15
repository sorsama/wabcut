#pragma once

// The transcript commands of the catalog: transcribe media, read the sequence's words and review list, delete / keep /
// move words, clean up fillers and silences, generate captions. They need the transcripts of the project's media,
// which the host provides as TranscriptServices (EnginePolicy::services, McpServerOptions::services), and
// transcript.transcribe a MediaTranscriber (TranscribeServices); without them the commands fail with InvalidRequest.
// editingCommands() is the built-in catalog plus these commands: what wabcut-cli, the MCP server and the app serve.
//
// Word indices refer to the transcript of the sequence as it is at the request's revision (transcript.words /
// transcript.text list them): pass expectedRevision to be sure they still mean the same words.

#include <wabcut/ai/edit/Review.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::edit {

class TranscriptServices : public command::CommandServices {
public:
    explicit TranscriptServices(std::shared_ptr<TranscriptProvider> transcripts,
                                std::shared_ptr<AudioEnergyProbe> energy = nullptr)
        : transcripts(std::move(transcripts)), energy(std::move(energy)) {}

    std::shared_ptr<TranscriptProvider> transcripts;
    std::shared_ptr<AudioEnergyProbe> energy; ///< optional: silence checks against the audio
};

/// Turns media audio into transcripts that TranscriptServices then finds: ai::TranscribeService in full builds
/// (ServiceMediaTranscriber, MediaServices.h), fakes in tests.
class MediaTranscriber {
public:
    struct Options {
        std::string provider = "auto"; ///< "auto", "local", "cloud:<model id>", ... (providers() lists them)
        std::string language = "auto"; ///< ISO 639-1 hint or "auto"
        bool force = false;            ///< transcribe even when a transcript of the audio exists
        /// Media times the project's audio redactions cover (ai::audioRedactionRanges): never transcribed, uploaded
        /// or returned as words (review P1-3).
        std::vector<TimeRange> redacted;
    };
    struct Result {
        std::string provider; ///< what made it ("local", "cloud:<model>", ...)
        bool fromCache = false;
        std::size_t words = 0;
        std::string language;
    };
    struct Provider {
        std::string id;
        std::string label;
        bool available = false;
        std::string reason; ///< why not, when unavailable
    };

    virtual ~MediaTranscriber() = default;
    /// `audioStream` indexes MediaInfo::audio. `progress` (0..1, on the calling thread) returns false to stop, after
    /// which the call fails with Cancelled. Errors name what went wrong (a cloud refusal names the AI error, e.g.
    /// "ConsentRequired", "Offline").
    virtual std::expected<Result, Error> transcribe(const MediaItem& media, int audioStream, const Options& options,
                                                    const std::function<bool(double)>& progress) = 0;
    /// Whether a transcript of the stream exists already.
    virtual bool hasTranscript(const MediaItem& media, int audioStream) const = 0;
    /// What Options::provider can name here, with availability.
    virtual std::vector<Provider> providers() const = 0;
    /// What `provider` runs as ("auto" resolved), or why nothing can.
    virtual std::expected<std::string, Error> resolve(std::string_view provider) const = 0;
};

class TranscribeServices : public command::CommandServices {
public:
    explicit TranscribeServices(std::shared_ptr<MediaTranscriber> transcriber) : transcriber(std::move(transcriber)) {}
    std::shared_ptr<MediaTranscriber> transcriber;
};

/// Adds transcript.transcribe, transcript.text, transcript.words, transcript.review, transcript.deleteRanges,
/// transcript.keepRanges, transcript.moveRange, transcript.cleanup, captions.generate and (C.7)
/// captions.generateFromTranscript, captions.updateFromTranscript, transcript.correctWord, transcript.correctRange,
/// transcript.speakers and transcript.corrections. transcript.transcribe needs TranscribeServices (alone or in a
/// command::ServiceBundle); the others TranscriptServices. Caption line breaking uses command::CaptionServices when
/// the host bundles one.
void registerTranscriptCommands(command::CommandRegistry& registry);

/// `after` with its stored caption cues brought in line with a redaction change from `before`: a cue made from the
/// transcript gets the transcript's words again ("[bleep]" where the sound bleeps), any other cue (translated, typed,
/// imported) gets "[bleep]" for a newly redacted word where its recognised spelling is, or as a whole - never
/// source-language words. The redaction.* commands of editingCommands() run it when TranscriptServices are there; a
/// host that edits redactions itself (the app's Masks panel) calls it with its transcripts (review wave 5, S5-P1-2).
ProjectState refreshRedactedCues(const ProjectState& before, ProjectState after, const TranscriptProvider& provider);

/// The catalog wabcut-cli, the MCP server and the app serve: the built-in commands, the transcript commands,
/// the media-analysis commands (wabcut/ai/analysis/AnalysisCommands.h), the audio commands
/// (wabcut/audio/edit/AudioCommands.h) and the smart-search commands (wabcut/ai/search/SearchCommands.h).
const command::CommandRegistry& editingCommands();

} // namespace wabcut::ai::edit
