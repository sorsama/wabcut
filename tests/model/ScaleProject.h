#pragma once

// A synthetic project of any size for scale tests and fuzz seeds (C.10): media, sequences of linked clip pairs, a chain
// of nested sequences, multicam sources and clips, effects with keyframes, transitions, markers and review notes (and in
// the small one a brand kit, a graphic template and an animated instance of it, C.13) - the shapes a real project has, built deterministically through the model's own operations (bulk clips directly, then
// validated). No Catch2: the fuzz seed tool links it too; failures throw std::runtime_error.

#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <vector>

namespace wabcut::test {

struct ScaleSpec {
    int media = 200;
    int sequences = 50;
    int clipPairsPerSequence = 100;  ///< linked video + audio clips on V1 / A1
    int nestingDepth = 8;            ///< sequence k plays sequence k-1 on V2 / A2, for k = 1..depth
    int multicamSources = 2;         ///< 4-angle multicam sources...
    int multicamClipsPerSource = 10; ///< ...each edited into this many sequences
    int effectsPerSequence = 10;     ///< a Gaussian blur and an Opacity keyframe on the first clips
    int transitionsPerSequence = 5;  ///< cross dissolves between clips on V1
    int markersPerSequence = 100;
    int notesPerSequence = 20;       ///< review notes, every other one with a reply and resolved
    bool graphics = false;           ///< a brand kit, a template and an instance on V2 of the first sequence
};

/// The 10,000-clip project of the C.10 scale tests.
inline ScaleSpec largeScaleSpec() { return {}; }
/// A small project with every shape (fuzz seeds, quick checks).
inline ScaleSpec smallScaleSpec() {
    return ScaleSpec{.media = 6,
                     .sequences = 4,
                     .clipPairsPerSequence = 6,
                     .nestingDepth = 2,
                     .multicamSources = 1,
                     .multicamClipsPerSource = 1,
                     .effectsPerSequence = 1,
                     .transitionsPerSequence = 1,
                     .markersPerSequence = 2,
                     .notesPerSequence = 2,
                     .graphics = true};
}

struct ScaleProject {
    ProjectState state;
    std::vector<SequenceId> sequences;   ///< the editing sequences, in creation order
    std::vector<SequenceId> multicams;   ///< the multicam sources
    std::size_t clips = 0;               ///< clips in all sequences
};

ScaleProject makeScaleProject(const ScaleSpec& spec);

/// Clips in every sequence of `state`.
std::size_t countClips(const ProjectState& state);

} // namespace wabcut::test
