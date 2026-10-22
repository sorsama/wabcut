#pragma once

// What captions.export needs from the host (P.2): the words the soundtrack bleeps or mutes are replaced in caption
// files as in exports (playback/video/CaptionRedaction.h), which the pure command module cannot compute. The app and
// wabcut-cli provide it (exporter::captionTrackForFile); without it the cues are written as they are.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <functional>

namespace wabcut::command {

class CaptionFileServices : public CommandServices {
public:
    /// `track` of `sequence` as exports write it (redacted words replaced).
    std::function<CaptionTrack(const ProjectState& project, const Sequence& sequence, const CaptionTrack& track)> redact;
};

} // namespace wabcut::command
