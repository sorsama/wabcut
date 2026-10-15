#include "ReviewFrames.h"

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/ReviewFrames.h>

namespace wabcut::app {

ReviewFrames::ReviewFrames(std::function<std::filesystem::path()> projectFile) : m_projectFile(std::move(projectFile)) {}

ReviewFrames::~ReviewFrames() = default;

exporter::RenderedReviewFrames& ReviewFrames::renderer() const {
    // The headless renderer belongs to the thread that made it (a QRhi is used from one thread at a time; OpenGL needs
    // its own thread): a job on another thread gets its own.
    if (!m_frames || m_thread != std::this_thread::get_id()) {
        m_frames = std::make_unique<exporter::RenderedReviewFrames>();
        m_thread = std::this_thread::get_id();
    }
    return *m_frames;
}

std::expected<QImage, Error> ReviewFrames::render(const ProjectState& state, SequenceId sequence, Ticks time, int width) const {
    const std::filesystem::path file = m_projectFile ? m_projectFile() : std::filesystem::path{};
    const ProjectState checked = editor::withMediaChecked(state, file);
    std::lock_guard lock(m_mutex);
    return renderer().picture(checked, sequence, time, width);
}

std::expected<command::ReviewFrame, Error> ReviewFrames::frame(const ProjectState& state, SequenceId sequence, Ticks time,
                                                               int width) const {
    const std::filesystem::path file = m_projectFile ? m_projectFile() : std::filesystem::path{};
    const ProjectState checked = editor::withMediaChecked(state, file);
    std::lock_guard lock(m_mutex);
    return renderer().frame(checked, sequence, time, width);
}

} // namespace wabcut::app
