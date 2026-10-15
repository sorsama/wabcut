#pragma once

// Pictures for the tracker (I.6, docs/masks.md "Tracking"): masks::TrackingServices over the media decoders, so
// track.mask and track.transform look at the clip's own source frames (not the edited picture), as luma with square
// pixels at a working width. The original file is used when it is on disk, else its proxy (frame for frame the same).
//
// Threading: one command at a time (the decoders are kept open between calls, keyed by media item and stream). The
// progress callback runs on the thread that runs the command.

#include <wabcut/masks/MaskCommands.h>
#include <wabcut/media/FrameHandle.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace wabcut::media {
class VideoDecoder;
}

namespace wabcut::editor {

/// The luma of a decoded frame, `width` pixels wide (height by the display aspect: square pixels), area-averaged.
masks::GrayImage lumaOf(const media::FrameHandle& frame, int width);

class MediaTrackingService final : public masks::TrackingServices {
public:
    explicit MediaTrackingService(std::filesystem::path projectFile = {});
    ~MediaTrackingService() override;

    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                     int maxWidth) override;
    void progress(const masks::TrackProgress& progress) override;

    /// Receives every progress report (a job's progress bar and confidence strip).
    void setProgressCallback(std::function<void(const masks::TrackProgress&)> callback);

private:
    struct Open {
        std::string path;
        std::unique_ptr<media::VideoDecoder> decoder;
    };
    std::filesystem::path m_projectFile;
    std::mutex m_mutex;
    std::map<std::pair<std::uint64_t, int>, Open> m_open;
    std::function<void(const masks::TrackProgress&)> m_progress;
};

} // namespace wabcut::editor
