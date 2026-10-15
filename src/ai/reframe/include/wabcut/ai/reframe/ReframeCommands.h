#pragma once

// The auto-reframe commands of the catalog (docs/commands.md, docs/ai.md "Auto reframe"):
//
//   analysis.subjects        MediaAnalysis  faces, people and salient regions of a media item over time (a job:
//                                           decodes frames, runs the detectors in wabcut-embed; cached)
//   reframe.createSequence   EditProject    a copy of a sequence at another aspect whose clips follow their subjects
//                                           with editable Transform keyframes; one undo step, dryRun previews it
//   reframe.rerun            EditProject    makes a reframed sequence again from its source (after the source was
//                                           edited), with its recorded parameters or new ones
//
// Subjects come from the host's ReframeServices (alone or in a command::ServiceBundle). Without them reframing still
// works (each clip stays centred, or on the anchor) and says so in a warning; analysis.subjects fails with
// InvalidRequest.

#include <wabcut/ai/reframe/Reframe.h>
#include <wabcut/ai/vision/Subjects.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <memory>

namespace wabcut::ai::reframe {

class ReframeServices : public command::CommandServices {
public:
    explicit ReframeServices(std::shared_ptr<vision::SubjectProvider> subjects) : subjects(std::move(subjects)) {}
    std::shared_ptr<vision::SubjectProvider> subjects;
};

/// Adds analysis.subjects, reframe.createSequence and reframe.rerun.
void registerReframeCommands(command::CommandRegistry& registry);

/// A reframe result as the commands report it: the new sequence, its clips' guides and (with `preview`) the window
/// path and subjects of each clip, sampled for drawing (the Reframe dialog's overlay).
command::Json reframeResultJson(const ProjectState& state, const ReframeResult& result, const ReframeOptions& options, bool preview);

} // namespace wabcut::ai::reframe
