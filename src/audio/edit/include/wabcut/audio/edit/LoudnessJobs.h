#pragma once

// Loudness analysis as background jobs (full builds), for the app's loudness readout: progress, cancel, the result
// cached like every measurement (MediaAnalysis.h). Each job runs on its own worker thread.

#include <wabcut/audio/edit/MediaAnalysis.h>

#include <wabcut/media/Job.h>

#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace wabcut::audio::edit {

class LoudnessJobs {
public:
    explicit LoudnessJobs(std::shared_ptr<RenderLoudnessAnalyzer> analyzer);
    /// Cancels the jobs still running and waits for them.
    ~LoudnessJobs();
    LoudnessJobs(const LoudnessJobs&) = delete;
    LoudnessJobs& operator=(const LoudnessJobs&) = delete;

    /// Measures `target` of `sequence` in `state` (a snapshot copy is kept by the job). Job::progress() follows the
    /// render; Job::cancel() stops it within half a second of audio (ErrorCode::Cancelled).
    media::Job<LoudnessStats> measure(ProjectState state, SequenceId sequence, LoudnessTarget target,
                                      std::optional<TimeRange> range = std::nullopt);

private:
    std::shared_ptr<RenderLoudnessAnalyzer> m_analyzer;
    std::mutex m_mutex;
    std::vector<std::pair<std::jthread, media::Job<LoudnessStats>>> m_jobs;
};

} // namespace wabcut::audio::edit
