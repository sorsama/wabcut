#pragma once

// workflow.podcast (AI-13): the podcast / interview workflow (Podcast.h) as a command. Without "run" it returns the
// plan: every step with what it will do, whether it is on and why not, the cost ("local, free"); nothing changes.
// With "run" it runs the enabled steps (or just "steps"), each through the editing catalog, in this one command's
// edit (one undo step); "context" carries what earlier runs made (the multicam source, the episode sequence), so a
// host can run the steps one by one, each its own undo step and history checkpoint (the app's Podcast workflow sheet).

#include <wabcut/ai/workflow/Podcast.h>
#include <wabcut/command/Registry.h>

namespace wabcut::ai::workflow {

/// The options a command's parameters describe (shared with the app sheet).
PodcastOptions podcastOptionsOf(command::ExecutionContext& context);
command::Json planToJson(const PodcastPlan& plan);

void registerWorkflowCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::workflow
