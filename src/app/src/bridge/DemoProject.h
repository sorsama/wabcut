#pragma once

// Projects built through the model API for the shell and the tests.
//
// buildDemoProject(): the M1 mock content (Sequence 01 at 23.976 fps: V3..V1 and A1..A3 with the same clips,
// names, label colours and markers, plus two more sequences), so the real timeline looks like the M1 design.
// buildStressProject(): a large sequence for performance tests and profiling.

#include <wabcut/model/ProjectState.h>

#include <cstdint>

namespace wabcut::app {

struct DemoProject {
    ProjectState state;
    SequenceId mainSequence;
    /// "City B-roll" on V2: selected when the shell opens (as in the M1 design).
    ClipId initialSelection;
    /// Where the playhead starts (00:00:12:08 in the demo, as in the M1 design; zero otherwise).
    Ticks initialPlayhead;
};

DemoProject buildDemoProject();

struct StressProjectSpec {
    int videoTracks = 3;
    int audioTracks = 3;
    /// Total clips, spread evenly over the tracks (adjacent clips, no gaps).
    int clips = 10'000;
    /// Length of each clip in frames.
    int clipFrames = 10;
};

/// One sequence with `spec.clips` clips. Video/audio clips at the same index on Vn/An are linked pairs.
/// Built directly from model types (like the model's own perf tests) because placing 10,000 clips one edit
/// at a time would dominate the test; the result passes validate().
DemoProject buildStressProject(const StressProjectSpec& spec = {});

} // namespace wabcut::app
