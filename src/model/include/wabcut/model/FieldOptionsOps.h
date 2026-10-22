#pragma once

// 0.2: editing Field Options (FieldOptions.h).

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FieldOptions.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <span>

namespace wabcut::edit {

/// Clip > Video Options > Field Options on the video clips among `clips` (audio clips, linked partners included, are
/// skipped; nested and multicam clips are video clips too: their nested canvas is progressive, so only Flicker Removal
/// and Always Deinterlace change what they show). One edit. Errors: NotFound, InvalidArgument (no video clip, a clip
/// on a locked track).
EditOutcome setFieldOptions(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                            const ClipFieldOptions& options);

} // namespace wabcut::edit
