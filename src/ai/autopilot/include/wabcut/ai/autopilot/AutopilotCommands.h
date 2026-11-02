#pragma once

// The creator autopilot's catalog commands (P1 #62, docs/creator-autopilot.md):
//
//   autopilot.broll       B-roll from the library for what is said, on a free video track; one undo step ("Auto
//                         B-roll"); dryRun previews it. With "placements" it places exactly those (the preview's
//                         chosen suggestions) without searching again.
//   autopilot.publishKit  title ideas, a description, chapters, tags and thumbnail candidates for a sequence; with
//                         "folder" it writes them as files there (a "<sequence> - publishing kit" folder). Changes
//                         nothing in the project. With "cloud" the user's chat model writes the title ideas,
//                         description and tags from the transcript text.
//   autopilot.pacing      the cut rhythm of a reference video (its shot cuts) against the sequence's, and cuts that
//                         would give the sequence a similar pace; "apply" adds them as markers or cuts (Add Edit on all
//                         tracks), one undo step ("Pacing Match").
//
// They need the transcripts (edit::TranscriptServices). They use when given: the smart search (search::SearchServices:
// B-roll by picture), the shot analysis (analysis::AnalysisServices: the reference's cuts), the frames of media items
// (AutopilotServices::frames: thumbnail scores), a still writer (command::FrameExportServices: the thumbnail files) and
// the user's guarded cloud chat model (AutopilotServices::chat).

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Capability.h>
#include <wabcut/ai/autopilot/Autopilot.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <expected>
#include <functional>
#include <memory>

namespace wabcut::ai::autopilot {

class AutopilotServices : public command::CommandServices {
public:
    using ChatOpener = std::function<std::expected<std::unique_ptr<IChatModel>, AiError>()>;
    explicit AutopilotServices(FramePicture frames = {}, ChatOpener chat = {}) : frames(std::move(frames)), chat(std::move(chat)) {}

    FramePicture frames;
    /// Opens the user's chat model for "cloud" (consent, locks and the spend cap are the host's); empty = none.
    ChatOpener chat;
};

/// Adds autopilot.broll, autopilot.publishKit and autopilot.pacing.
void registerAutopilotCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::autopilot
