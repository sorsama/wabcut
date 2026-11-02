#include <wabcut/interchange/CaptionTrackReport.h>

namespace wabcut::interchange {

namespace {

std::string trackName(const CaptionTrack& track) {
    return track.name.empty() ? std::string("Captions") : track.name;
}

} // namespace

std::string captionTrackLabel(const CaptionTrack& track) {
    std::string label = "caption track '" + trackName(track) + "' (";
    if (!track.language.empty()) {
        label += track.language + ", ";
    }
    label += std::to_string(track.cues.size()) + (track.cues.size() == 1 ? " cue)" : " cues)");
    return label;
}

SourceLocation captionTrackLocation(const Sequence& sequence, const CaptionTrack& track, const std::string& fileLabel) {
    return SourceLocation{fileLabel, "'" + sequence.name + "' caption track '" + trackName(track) + "'", 0};
}

void reportCaptionTracks(const Sequence& sequence, const std::string& fileLabel, std::string_view files,
                         ImportReport& report) {
    for (const CaptionTrack& track : sequence.captionTracks) {
        if (track.cues.empty()) {
            continue;
        }
        report.add(ReportOutcome::Dropped, ReportCategory::Caption,
                   captionTrackLabel(track) + " is not carried by " + std::string(files),
                   captionTrackLocation(sequence, track, fileLabel), sequence.name);
    }
}

} // namespace wabcut::interchange
