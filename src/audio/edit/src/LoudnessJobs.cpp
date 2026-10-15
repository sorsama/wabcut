#include <wabcut/audio/edit/LoudnessJobs.h>

#include <algorithm>

namespace wabcut::audio::edit {

namespace {

/// Stops a measurement when the job is cancelled.
class StopControl final : public command::ExecutionControl {
public:
    explicit StopControl(std::stop_token token) : m_token(std::move(token)) {}
    std::optional<command::CommandError> checkpoint(std::string_view) override {
        if (m_token.stop_requested()) {
            return command::CommandError{command::ErrorCode::Cancelled, "loudness analysis cancelled", nullptr};
        }
        return std::nullopt;
    }

private:
    std::stop_token m_token;
};

} // namespace

LoudnessJobs::LoudnessJobs(std::shared_ptr<RenderLoudnessAnalyzer> analyzer) : m_analyzer(std::move(analyzer)) {}

LoudnessJobs::~LoudnessJobs() {
    std::vector<std::pair<std::jthread, media::Job<LoudnessStats>>> jobs;
    {
        std::lock_guard lock(m_mutex);
        jobs = std::move(m_jobs);
    }
    for (auto& [thread, job] : jobs) {
        job.cancel();
    }
    // The jthreads join as they go out of scope.
}

media::Job<LoudnessStats> LoudnessJobs::measure(ProjectState state, SequenceId sequence, LoudnessTarget target,
                                                std::optional<TimeRange> range) {
    auto shared = std::make_shared<media::detail::JobState<LoudnessStats>>();
    media::Job<LoudnessStats> job(shared);
    std::lock_guard lock(m_mutex);
    // Reap finished workers.
    std::erase_if(m_jobs, [](const auto& entry) { return entry.second.isFinished(); });
    m_jobs.emplace_back(std::jthread([analyzer = m_analyzer, shared, state = std::move(state), sequence, target, range] {
                            StopControl control(shared->stop.get_token());
                            auto result = analyzer->measure(state, sequence, target, range, &control,
                                                            [&](double fraction) {
                                                                shared->progress.store(static_cast<float>(fraction),
                                                                                       std::memory_order_relaxed);
                                                            });
                            shared->complete(std::move(result));
                        }),
                        job);
    return job;
}

} // namespace wabcut::audio::edit
