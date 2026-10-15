#pragma once

// The projects the benchmarks edit (P.1). No Catch2 (failures throw std::runtime_error), so any benchmark can use them.
//
//   * oneHourProject(): a 1-hour, 10,000-clip timeline in one sequence, the shape of a long edit: 200 media files
//     (10-minute camera clips with sound), V1/A1 and V2/A2 each holding 2,500 linked video + audio pairs of 36 frames
//     (1.44 s at 25 fps: 2,500 x 36 frames = 1 hour), and 100 markers. Clips keep their media's name and default
//     parameters, as imported clips do.
//   * C.10's ScaleProject (tests/model/ScaleProject.h): the 10,080-clip project spread over 52 sequences.

#include <wabcut/model/ProjectState.h>

#include <vector>

namespace wabcut::perf {

struct OneHourProject {
    ProjectState state;
    SequenceId sequence;
    std::vector<ClipId> v1;  ///< V1's clips in timeline order
    std::vector<ClipId> a1;  ///< A1's clips, linked to V1's
    std::size_t clips = 0;   ///< 10,000
};

OneHourProject oneHourProject(int clipPairsPerTrack = 2500, int framesPerClip = 36);

} // namespace wabcut::perf
