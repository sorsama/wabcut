#pragma once

// Timeline pictures for edit tests: build a project from ASCII art and render results back to it, so a
// test reads like a Premiere behaviour spec:
//
//     Timeline tl(R"(
//         V1 tgt |Aaaa..Bbbb|
//         A1 tgt |Aaaa..Bbbb|
//     )");
//
// One character is one frame at 25 fps. '.' is a gap. A clip is an uppercase media letter, optionally
// followed by its source In in frames, then lowercase letters of the same media for the remaining
// frames: "Aaaa" is 4 frames of media A from source frame 0, "A12a" is 4 frames from source frame 12.
// Two clips of the same media touching ("AaaAaa") are separate clips (a through edit).
//
// Track lines are listed top to bottom like Premiere draws them: Vn..V1, then A1..An. Words between the
// label and the first '|' set track flags: tgt (targeted), lock, nosync (sync lock off), mute, solo,
// off (track output disabled), and on audio tracks a role: dialogue, music, effects. Every letter A-Z is a
// media item with one video and two audio streams of 1000 frames (override per letter with `mediaFrames`).
// Clips on V tracks play video stream 0, clips on A tracks audio stream 0, and clips are named after their
// media letter. A clip of a sequence (a nested or multicam clip, C.8) renders with the first letter of that
// sequence's name.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectState.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

namespace wabcut::test {

/// Wall-clock budgets in tests are set on a developer machine; shared CI runners are slower and noisier.
/// WABCUT_TIMING_SLACK (a factor, at least 1; default 1) widens every budget that is multiplied by this. CI sets it,
/// so a slow runner does not fail a test whose results are right; local runs keep the strict budgets.
inline double timingSlack() {
    static const double slack = [] {
        const char* text = std::getenv("WABCUT_TIMING_SLACK");
        const double value = text != nullptr ? std::strtod(text, nullptr) : 1.0;
        return value > 1.0 ? value : 1.0;
    }();
    return slack;
}

inline constexpr Rate kRate = kRate25;
constexpr Ticks f(std::int64_t frames) { return framesToTicks(frames, kRate); }

class Timeline {
public:
    explicit Timeline(std::string_view picture, const std::map<char, std::int64_t>& mediaFrames = {});

    ProjectState state;
    SequenceId seq;

    const Sequence& sequence() const;
    TrackId track(std::string_view label) const;
    /// "V1@4": the clip starting at frame 4 on V1 (in `state`).
    ClipId clip(std::string_view ref) const;
    MediaId media(char letter) const;

    /// Links the referenced clips: link("V1@0 A1@0").
    Timeline& link(std::string_view refs);
    /// Links every video clip with the audio clips of the same media, start and length.
    Timeline& linkMatching();
};

/// Resolves "V1@4" in any state of the sequence.
ClipId clipAt(const ProjectState& state, SequenceId seq, std::string_view ref);

/// The picture without indentation and flags: "V1 |Aaaa..Bbbb|" lines joined by '\n', short lines padded
/// with gaps to the widest line.
std::string normalize(std::string_view picture);

/// Renders the sequence in normalized picture form, at least as wide as `widthFrom` (a picture).
std::string render(const ProjectState& state, SequenceId seq, std::string_view widthFrom = {});

/// Link groups as "V1@0+A1@0 V1@6+A1@6" (each group in track order, groups sorted).
std::string links(const ProjectState& state, SequenceId seq);

/// Requires validate(state, Strict) to pass, printing the violations otherwise.
void requireValid(const ProjectState& state);

/// Requires the edit to succeed and leave a valid state; returns its result.
edit::EditResult ok(edit::EditOutcome outcome);

} // namespace wabcut::test

#define CHECK_TIMELINE(state, seq, picture)                                                                  \
    CHECK(::wabcut::test::render((state), (seq), (picture)) == ::wabcut::test::normalize(picture))
