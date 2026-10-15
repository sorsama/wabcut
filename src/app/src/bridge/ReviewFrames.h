#pragma once

// ReviewFrames (M5m): the thumbnails of a review package (C.9, command/ReviewPackage.h ReviewFrameSource) rendered by the
// export renderer - exporter::RenderedReviewFrames (I.9): the sequence's composited picture at the note's time from the
// originals, the export's compositor and colour pipeline (Principle 5), redactions included, at most `width` pixels
// wide at the sequence's aspect, as JPEG. The app's part: media paths relative to the project file are resolved first.
// One frame per call on the calling thread (review.exportPackage asks once per note); the headless renderer is kept
// between calls on the same thread. A frame that cannot be rendered is an error for that note only: the package gets
// no thumbnail for it and lists a warning (never a picture without its redactions).

#include <wabcut/command/ReviewPackage.h>

#include <QImage>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace wabcut::exporter {
class RenderedReviewFrames;
}

namespace wabcut::app {

class ReviewFrames final : public command::ReviewFrameSource {
public:
    /// `projectFile` resolves the media paths relative to the project (empty for an unsaved project).
    explicit ReviewFrames(std::function<std::filesystem::path()> projectFile = {});
    ~ReviewFrames() override;

    std::expected<command::ReviewFrame, Error> frame(const ProjectState& state, SequenceId sequence, Ticks time,
                                                     int width) const override;

    /// The rendered picture (tests and the panel's preview).
    std::expected<QImage, Error> render(const ProjectState& state, SequenceId sequence, Ticks time, int width) const;

private:
    /// The export path's frame source, created on (and kept for) the thread asking.
    exporter::RenderedReviewFrames& renderer() const;

    std::function<std::filesystem::path()> m_projectFile;
    mutable std::mutex m_mutex;
    mutable std::unique_ptr<exporter::RenderedReviewFrames> m_frames;
    mutable std::thread::id m_thread;
};

} // namespace wabcut::app
