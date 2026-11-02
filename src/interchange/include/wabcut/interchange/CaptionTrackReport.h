#pragma once

// 0.6 (#57): caption tracks in the export report of a timeline hand-off. Timeline files (FCP7 XML, FCPXML, OTIO, EDL)
// and audio-post files (AAF, OMF) carry no caption track, so each caption track of the exported sequence that has cues
// gets a Dropped / Caption entry saying so. interchange.export turns an entry into a Carried one when it writes that
// track as a caption file next to the timeline ("captionSidecars").

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/model/Sequence.h>

#include <string>
#include <string_view>

namespace wabcut::interchange {

/// "caption track 'English' (en, 214 cues)"; an unnamed track is 'Captions', an unknown language is left out.
std::string captionTrackLabel(const CaptionTrack& track);

/// Where a caption track's entry points: the sequence and the track ("'Episode 3' caption track 'English'").
SourceLocation captionTrackLocation(const Sequence& sequence, const CaptionTrack& track, const std::string& fileLabel);

/// One Dropped / Caption entry per caption track of `sequence` with cues: "<label> is not carried by <files>"
/// (`files`: "timeline files", "AAF or OMF files").
void reportCaptionTracks(const Sequence& sequence, const std::string& fileLabel, std::string_view files,
                         ImportReport& report);

} // namespace wabcut::interchange
