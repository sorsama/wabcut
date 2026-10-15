#pragma once

// Voice-over takes (A.8, docs/ai.md "Voice-over recording"): what a recording pass leaves in the project, and the takes
// switcher. Pure (model ops).
//
// A pass records onto an armed audio track over a range (its slot). Its file becomes a media item in the "Voice-over"
// bin with recording provenance (MediaItem::recording: the sequence, track and range, the take number, the input device
// and the latency compensation), and a clip of it is overwritten onto the track over the range: what was there, an
// earlier take of the slot included, is replaced, so the newest take is the one heard and the earlier ones stay in the
// bin as the slot's other takes. A slot's takes are the recorded media of the same sequence and track whose ranges
// overlap (AI-11's takes idea for repeated attempts at one line: "use this take" puts another one in the slot).

#include <wabcut/model/EditOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectState.h>

#include <string>
#include <vector>

namespace wabcut::audio::edit {

/// Name of the bin recorded takes go into.
inline constexpr std::string_view kVoiceOverBin = "Voice-over";

/// A finished pass, as the recorder reports it.
struct RecordedTake {
    std::string path;                 ///< UTF-8, absolute: the take's WAV
    SourceFingerprint fingerprint;    ///< of the file (fingerprintFile)
    Ticks start{};                    ///< timeline time of its first sample
    Ticks duration{};
    int sampleRate = 48000;
    int channels = 1;
    int bitsPerSample = 24;
    std::string device;               ///< the input device's name (untrusted: bounded on the way in)
    Ticks latencyCompensation{};
    std::string createdAt;            ///< ISO-8601 UTC
};

/// Puts a recorded take into the project: its media item (in the Voice-over bin, made when missing) and its clip on
/// `track` over [start, start + duration) as an overwrite edit. The take number is one more than the slot's takes so
/// far. EditResult::createdMedia is the item, EditResult::placed the clip. Errors as the model ops (a locked track, a
/// video track, a missing sequence).
wabcut::edit::EditOutcome placeVoiceOverTake(const ProjectState& state, SequenceId sequence, TrackId track, const RecordedTake& take);

struct VoiceOverTake {
    MediaId media;
    std::int32_t take = 1;
    TimeRange range;      ///< where it was recorded
    std::string name;
    std::string device;
    std::string createdAt;
    bool active = false;  ///< the clip asked about plays it
};

/// The takes of the slot `clip` plays (it must play a recorded take), newest first.
std::vector<VoiceOverTake> voiceOverTakesOf(const ProjectState& state, SequenceId sequence, ClipId clip);

/// Puts take `take` of the clip's slot in its place: that take's media over its recorded range (an overwrite edit on
/// the clip's track), with the clip's level, pan and effects. Errors: InvalidArgument (not a recorded take, no such
/// take), the model ops'.
wabcut::edit::EditOutcome useVoiceOverTake(const ProjectState& state, SequenceId sequence, ClipId clip, std::int32_t take);

/// The media name of a take: "Voice-over A2 Take 3".
std::string voiceOverTakeName(const std::string& trackLabel, std::int32_t take);

} // namespace wabcut::audio::edit
