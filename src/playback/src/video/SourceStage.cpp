#include <wabcut/playback/video/SourceStage.h>

#include <mutex>

namespace wabcut::playback {

namespace {

// A mutex, not std::atomic<std::shared_ptr> (not on every standard library CI builds with) nor the deprecated free atomic_load.
std::mutex g_mutex;
std::shared_ptr<SourceFrameProcessor> g_processor;

} // namespace

struct ScopedSourceStageMode::State {
    SourceStageMode mode = SourceStageMode::Show;
    std::stop_token stop;
    std::optional<std::string> failure;
};

namespace {

thread_local ScopedSourceStageMode::State* g_state = nullptr;

} // namespace

ScopedSourceStageMode::ScopedSourceStageMode(SourceStageMode mode, std::stop_token stop) : m_state(std::make_unique<State>()) {
    m_state->mode = mode;
    m_state->stop = std::move(stop);
    m_previous = g_state;
    g_state = m_state.get();
}

ScopedSourceStageMode::~ScopedSourceStageMode() { g_state = m_previous; }

std::optional<std::string> ScopedSourceStageMode::failure() const { return m_state->failure; }

SourceStageMode currentSourceStageMode() { return g_state != nullptr ? g_state->mode : SourceStageMode::Show; }

std::stop_token currentSourceStageStop() { return g_state != nullptr ? g_state->stop : std::stop_token{}; }

void reportSourceStageFailure(std::string reason) {
    if (g_state != nullptr && !g_state->failure) {
        g_state->failure = std::move(reason);
    }
}

void setSourceFrameProcessor(std::shared_ptr<SourceFrameProcessor> processor) {
    std::lock_guard lock(g_mutex);
    g_processor = std::move(processor);
}

std::shared_ptr<SourceFrameProcessor> sourceFrameProcessor() {
    std::lock_guard lock(g_mutex);
    return g_processor;
}

} // namespace wabcut::playback
