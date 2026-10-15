#pragma once

// Human-readable edit steps for the assistant panel: a tool call becomes "Remove filler words and pauses longer
// than 0.8 s" instead of "transcript_cleanup {...}", and a ChangeSet becomes "2 clips removed, 3 changed on V1, A1".

#include <wabcut/command/Command.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectState.h>

#include <string>

namespace wabcut::ai::agent {

/// What a call does, in the words of the panel. Times in params (ticks or timecodes) are shown as timecodes of
/// `sequence` in `state` (the state the call runs on).
std::string describeCall(const command::CommandSpec& spec, const command::Json& params, const ProjectState& state,
                         SequenceId sequence);

/// What changed between two states: "2 clips removed, 1 added, 3 changed on V1, A1; markers changed". Empty when
/// nothing changed.
std::string describeChanges(const ChangeSet& changes, const ProjectState& before, const ProjectState& after);

} // namespace wabcut::ai::agent
