#pragma once

// I.9: colour management edits (ColorManagement.h): the project's working space and output, and a video clip's input
// colour space. Pure state-to-state operations (one undo step each through the commands).

#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <optional>
#include <span>

namespace wabcut::edit {

/// Sets the project's colour management. Errors: a value out of range (colorManagementProblem).
std::expected<ProjectState, Error> setColorManagement(const ProjectState& state, const ColorManagement& management);

/// Sets the input colour space of video clips (nullopt: Auto, from the stream's tags). Errors: an audio, nested or
/// unknown-source clip (a nested sequence is already in the working space).
EditOutcome setClipInputColorSpace(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                                   std::optional<InputColorSpace> space);

} // namespace wabcut::edit
