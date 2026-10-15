#pragma once

// The creator commands of the catalog (AI-11, docs/commands.md, docs/ai.md "Creator superpowers"):
//
//   roughcut.fromScript  EditProject   a new sequence from a script, outline or bullets: every beat found in the media's
//                                      transcripts, the best take chosen (alternates kept), assembled in script order
//                                      with handles; optionally cleaned of fillers and pauses and with B-roll
//                                      suggestions on their own track; optionally a loose outline first interpreted
//                                      by the user's cloud model (text only, guarded). Returns the alignment report.
//                                      One undo step; dryRun previews it.
//   roughcut.swapTake    EditSequence  puts another recorded take of a beat into a rough cut made by fromScript, in
//                                      place of the current one (a ripple: the rest follows)
//   takes.find           Read          repeated attempts of the same line in a sequence (and the media it uses), grouped,
//                                      ranked and compared word by word
//   takes.use            EditSequence  "Use this take" for one group, or "Keep best takes" for all of them ("best"):
//                                      the line is heard once, with the chosen take; one undo step with a review list
//
// They read transcripts and audio energy from edit::TranscriptServices, the footage search (B-roll, its text
// encoder) from search::SearchServices, and the rest from RoughCutServices when the host provides it.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/roughcut/RoughCut.h>
#include <wabcut/ai/roughcut/Takes.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <expected>
#include <functional>
#include <memory>

namespace wabcut::ai::roughcut {

class RoughCutServices : public command::CommandServices {
public:
    using ChatOpener = std::function<std::expected<std::unique_ptr<IChatModel>, AiError>()>;
    using TextEmbedder = std::function<std::optional<std::vector<float>>(const std::string& text)>;

    RoughCutServices() = default;
    RoughCutServices(ChatOpener chat, std::shared_ptr<TakeAudioProbe> audio, TextEmbedder embed = {}, bool readFiles = false)
        : chat(std::move(chat)), audio(std::move(audio)), embed(std::move(embed)), readFiles(readFiles) {}

    /// The user's chat model through AiPlatform::chat (guarded) for interpreting outlines; null = local only.
    ChatOpener chat;
    /// Level, peak and noise floor of takes; null = the transcript services' energy probe (level only).
    std::shared_ptr<TakeAudioProbe> audio;
    /// A text encoder; null = the footage search's (SearchServices), when it has one.
    TextEmbedder embed;
    /// roughcut.fromScript may read a script from a local text file ("file"). Hosts whose callers are local people
    /// (the CLI's cmd, the app) allow it; MCP clients and the assistant never get it.
    bool readFiles = false;
};

/// Adds roughcut.fromScript, roughcut.swapTake, takes.find and takes.use.
void registerRoughCutCommands(command::CommandRegistry& registry);

/// The alignment report as the commands return it. `ranges` (optional) holds each beat's place on the timeline.
command::Json planToJson(const RoughCutPlan& plan, const ProjectState& state, const std::vector<std::optional<TimeRange>>* ranges = nullptr);
/// The takes report as takes.find returns it (at most `limit` groups).
command::Json takesToJson(const TakesReport& report, std::size_t limit = 100);

/// Where a recorded take of a rough cut plays on `sequence` now: the stretch of the words of its media range heard on
/// the timeline (`occurrence` 0 = the first such stretch). Indices into `words`.
std::optional<std::pair<std::size_t, std::size_t>> wordsOfTake(const ProjectState& state, const Sequence& sequence,
                                                               const edit::SequenceTranscript& words, const MediaRange& take,
                                                               std::size_t occurrence = 0);

} // namespace wabcut::ai::roughcut
