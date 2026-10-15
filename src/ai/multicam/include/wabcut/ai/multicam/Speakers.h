#pragma once

// Active-speaker detection for a multicam source (AI-13, docs/ai.md "Podcast and interview editing"): who speaks when,
// and which angle shows whom. Three kinds of evidence, each optional, all in the multicam source's time:
//   - each angle's microphone level (dB envelope; a speaker's own mic when they have one: bleed from the others is
//     discounted by comparing every mic against its own noise floor and speech level);
//   - the transcript's words with their speakers (C.7 project speakers, else the recogniser's labels per mic), the
//     same words heard by two mics kept once (the louder mic's);
//   - the faces each camera sees (AI-9) with their lower-face motion (a lips cue).
// They are fused per 100 ms frame into a speaking score per speaker (transcript 0.5, mic 0.35, face 0.15 of the cues a
// speaker has), thresholded with hysteresis into speech; frames where several speak are crosstalk, frames where mics
// are loud without words are non-speech (laughter, reactions); a listener's quick "wow" or "right?" while someone else
// holds the floor is a reaction, not a turn (no crosstalk). Each video angle is a close-up of one speaker (mostly
// one face, whose motion follows that speaker), a wide shot (mostly two faces or more) or unknown; the user's
// confirmed assignments win over the guesses.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::ai::multicam {

// ---- Evidence ----------------------------------------------------------------------------------------------------

struct AngleDescription {
    std::size_t angle = 0; ///< 0-based in the multicam source
    std::string name;
    bool video = false;
    bool audio = false;
};

/// One angle's microphone level: dBFS per frame from `start`.
struct MicEnvelope {
    std::size_t angle = 0;
    Ticks start{};
    Ticks frame{kTicksPerSecond / 20};
    std::vector<float> db;
};

struct FacePoint {
    Ticks time{};
    float x = 0.0f; ///< the face box, normalised 0..1
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float activity = 0.0f; ///< lower-face motion since the previous sample (0..1)
};

/// One face track of one camera.
struct FaceTrack {
    std::size_t angle = 0;
    int track = 0;
    std::vector<FacePoint> points; ///< in time order
};

/// A face camera sample count: how many faces a camera saw at a time (for close-up / wide), faces or none.
struct FaceCount {
    std::size_t angle = 0;
    Ticks time{};
    int faces = 0;
};

struct SpokenWord {
    TimeRange range;
    std::size_t angle = 0;      ///< whose audio track it was heard on
    std::optional<int> label;   ///< the recogniser's speaker label
    SpeakerId speaker;          ///< the project speaker (C.7), when assigned
    std::string text;
    float confidence = 0.0f;
};

struct SpeakerEvidence {
    Ticks duration{};
    std::vector<AngleDescription> angles;
    std::vector<MicEnvelope> mics;
    std::vector<FaceTrack> faces;
    std::vector<FaceCount> faceCounts;
    std::vector<SpokenWord> words;
    std::vector<std::pair<SpeakerId, std::string>> speakerNames; ///< project speakers' names
    std::vector<std::string> notes;                              ///< what was missing or skipped
};

// ---- The timeline ------------------------------------------------------------------------------------------------

struct Speaker {
    std::string key;                ///< "speaker:<id>", "a<angle>:L<label>" or "mic:<angle>"
    std::string name;               ///< for people: "Speaker 2", the project speaker's name, "Mic 2"
    SpeakerId project;
    std::optional<std::size_t> mic; ///< the angle whose microphone is theirs
    double seconds = 0.0;           ///< how long they speak
};

enum class SegmentKind { Silence, Single, Overlap, NonSpeech };
std::string_view toString(SegmentKind kind);

struct SpeakerSegment {
    TimeRange range;
    SegmentKind kind = SegmentKind::Silence;
    std::vector<std::size_t> speakers; ///< indices into SpeakerTimeline::speakers
    double confidence = 0.0;           ///< 0..1
};

enum class AngleRole { Unknown, CloseUp, Wide };
std::string_view toString(AngleRole role);

struct AngleAssignment {
    std::size_t angle = 0;
    AngleRole role = AngleRole::Unknown;
    std::vector<std::size_t> speakers; ///< whom it shows (a close-up: one)
    double confidence = 0.0;
    bool confirmed = false;            ///< the user's, not a guess
    std::string reason;
    std::optional<FacePoint> face;     ///< a representative face (thumbnail), when it sees one
};

/// A listener's strong reaction while someone else speaks (laughter, a quick "wow"): reaction-shot material.
struct Reaction {
    TimeRange range;
    std::size_t speaker = 0;
    double strength = 0.0; ///< 0..1
    std::string kind;      ///< "laughs", "reacts"
};

struct SpeakerTimeline {
    Ticks duration{};
    Ticks frame{};
    std::vector<Speaker> speakers;
    std::vector<SpeakerSegment> segments; ///< cover [0, duration) without gaps
    std::vector<AngleAssignment> angles;  ///< one per angle with a picture
    std::vector<Reaction> reactions;
    std::vector<std::vector<float>> activity; ///< [speaker][frame] the fused score 0..1
    std::vector<std::string> notes;

    /// The angle that shows `speaker` best (a close-up), or nullopt.
    std::optional<std::size_t> closeUpOf(std::size_t speaker) const;
    /// The wide angle (the most confident), or nullopt.
    std::optional<std::size_t> wide() const;
    /// The segment at `t`, or null.
    const SpeakerSegment* segmentAt(Ticks t) const;
};

/// The user's word on an angle: whom it shows, or that it is the wide shot.
struct AngleOverride {
    std::size_t angle = 0;
    AngleRole role = AngleRole::CloseUp;
    std::vector<std::string> speakers; ///< speaker keys (or names)
};

struct SpeakerOptions {
    Ticks frame{kTicksPerSecond / 10};
    double transcriptWeight = 0.5;
    double micWeight = 0.35;
    double faceWeight = 0.15;
    double on = 0.5;   ///< hysteresis: speaking starts above this score...
    double off = 0.35; ///< ...and ends below this one
    Ticks minSpeech{kTicksPerSecond / 4};
    Ticks bridge{kTicksPerSecond * 3 / 10}; ///< gaps shorter than this inside speech are bridged
    double bleed = 0.25; ///< a mic counts fully only within this of the loudest mic (normalised levels)
    std::vector<AngleOverride> overrides;
};

SpeakerTimeline analyzeSpeakers(const SpeakerEvidence& evidence, const SpeakerOptions& options = {});

} // namespace wabcut::ai::multicam
