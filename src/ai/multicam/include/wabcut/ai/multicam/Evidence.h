#pragma once

// What a multicam source says about who speaks (AI-13): the evidence for analyzeSpeakers (Speakers.h), read from the
// project and its media:
//   - the transcript's words, through each angle's audio tracks of the source (a multicam source is an ordinary
//     sequence of media clips, so its words are the transcripts of those clips' media);
//   - each angle's microphone level, from its first audio track (AngleSignals::levels: decoded in full builds);
//   - each camera's faces with their lower-face motion (AngleSignals::faces: the AI-9 subject analysis, cached).
// All of it in the source's time. Missing pieces are notes, not errors: transcripts alone, levels alone or faces
// alone each give a usable speaker timeline.

#include <wabcut/ai/multicam/Speakers.h>
#include <wabcut/ai/vision/Subjects.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::edit {
class TranscriptProvider;
}

namespace wabcut::ai::multicam {

/// Per-angle signals from media. wabcut_ai_multicam_media decodes them; tests bring their own.
class AngleSignals {
public:
    virtual ~AngleSignals() = default;
    /// The level (dBFS per `frame`) of `media`'s audio stream `audioStream` over `source` (media time).
    virtual std::expected<std::vector<float>, Error> levels(const MediaItem& media, int audioStream, TimeRange source, Ticks frame,
                                                            std::stop_token stop) = 0;
    /// The faces of `media`'s video stream (the AI-9 subject analysis, cached by its provider).
    virtual std::expected<vision::SubjectAnalysis, Error> faces(const MediaItem& media, int videoStream,
                                                                const std::function<bool(double)>& progress) = 0;
};

struct EvidenceOptions {
    Ticks frame{kTicksPerSecond / 20}; ///< mic level frames
    bool levels = true;
    bool faces = true;
};

/// Progress 0..1 with what is being read; returning false cancels.
using EvidenceProgress = std::function<bool(double, const std::string&)>;

/// Errors: NotFound (no such sequence), InvalidArgument (not a multicam source), Cancelled, and a signal's own errors
/// are notes (that angle's cue is left out).
std::expected<SpeakerEvidence, Error> gatherEvidence(const ProjectState& state, SequenceId multicam, const edit::TranscriptProvider* transcripts,
                                                     AngleSignals* angleSignals, const EvidenceOptions& options = {},
                                                     const EvidenceProgress& progress = {}, std::stop_token stop = {});

/// The word ranges of the source's transcript (source time), for cuts between words.
std::vector<TimeRange> sourceWords(const SpeakerEvidence& evidence);

/// The multicam source a clip plays, or nullopt (not a multicam clip).
std::optional<SequenceId> multicamSourceOf(const ProjectState& state, const Clip& clip);

} // namespace wabcut::ai::multicam
