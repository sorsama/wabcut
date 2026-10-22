#pragma once

// 0.2 (Adobe Media Encoder's Watch Folders): media files dropped into a folder are transcoded with a preset into an
// output folder, through the export queue (one at a time, after what is queued already).
//
// A file is taken once it has kept its size and modification time over `settleScans` scans (it is not being copied any
// more). Each becomes a project with one sequence that plays it whole at its own size and rate (projectForMedia), its
// export goes to "<output>/<name><extension>" (numbered "<name>_1" when taken), and - as Media Encoder does - the source
// moves into "<folder>/Source" once encoded (WatchFolder::moveSources). Files that fail stay where they are and are not
// tried again until they change. Only the folder's own files are watched (not its subfolders, so not Output or Source).

#include <wabcut/export/ExportJob.h>
#include <wabcut/model/Error.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wabcut::exporter {

struct WatchFolder {
    std::filesystem::path folder;
    std::string presetId;         ///< what the queue shows (a built-in or user preset id)
    ExportSettings settings;      ///< the preset's settings
    std::filesystem::path output; ///< empty = "<folder>/Output"
    bool moveSources = true;      ///< move each encoded source into "<folder>/Source"
};

struct WatchFolderStatus {
    std::uint64_t id = 0;
    WatchFolder folder;
    int queued = 0; ///< exports queued or running
    int done = 0;
    int failed = 0;
    std::string lastError;
};

/// A project of `file` alone: the media item (probed) and one sequence that plays it from start to end - its video at
/// its own size and rate, each audio stream on a track of its own (audio only: a 1920x1080 30 fps sequence). Errors:
/// NotFound, Unsupported (not media), DecoderError.
std::expected<ProjectState, Error> projectForMedia(const std::filesystem::path& file);

/// The files a watch folder takes: video, audio and still extensions FFmpeg reads (".mp4", ".mov", ".mkv", ".wav",
/// ...), case-insensitive.
bool isWatchedMediaFile(const std::filesystem::path& file);

class WatchFolderService {
public:
    struct Options {
        /// How often the folders are scanned; 0 = never by itself (scanNow() only: tests, wabcut-cli --once).
        std::chrono::milliseconds interval{2000};
        /// Video source of the exports (empty = the rendered source; tests: the synthetic one).
        VideoSourceFactory videoSource;
        int settleScans = 2;
    };

    /// `queue` must outlive the service.
    WatchFolderService(ExportQueue& queue, Options options);
    ~WatchFolderService();
    WatchFolderService(const WatchFolderService&) = delete;
    WatchFolderService& operator=(const WatchFolderService&) = delete;

    /// Watches `folder` (created when missing, with its output folder). Errors: InvalidArgument (settings), IoError.
    std::expected<std::uint64_t, Error> add(WatchFolder folder);
    bool remove(std::uint64_t id);
    std::vector<WatchFolderStatus> folders() const;

    /// Scans every folder once; returns the jobs it queued.
    std::vector<ExportJobId> scanNow();

private:
    struct Core; ///< the folders; shared with the jobs' completion callbacks (which may outlive the service)

    ExportQueue& m_queue;
    Options m_options;
    std::shared_ptr<Core> m_core;
    std::jthread m_thread; // last: started after the state above exists
};

} // namespace wabcut::exporter
