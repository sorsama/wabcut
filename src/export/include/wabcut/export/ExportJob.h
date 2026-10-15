#pragma once

// ExportJob renders one sequence of a fixed project snapshot to a file; ExportQueue runs jobs in background.

#include <wabcut/export/ExportSettings.h>
#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace wabcut::exporter {

/// Creates the video source of an export (it receives the fixed snapshot and what resolve() decided).
using VideoSourceFactory = std::function<std::expected<std::unique_ptr<IVideoFrameSource>, Error>(
    const ProjectState& snapshot, const Sequence& sequence, const VideoSourceFormat& format)>;

/// The production video source: renderedVideoSourceFactory() with default options (RenderedVideoSource.h):
/// the snapshot evaluated, decoded and composited by render::OffscreenRenderer, like the preview.
VideoSourceFactory defaultVideoSourceFactory();

/// A factory for SyntheticVideoSource.
VideoSourceFactory syntheticVideoSourceFactory(SyntheticPattern pattern = {});

enum class ExportPhase : std::uint8_t { Queued, Preparing, Rendering, Finalizing, Done, Failed, Cancelled };
std::string_view exportPhaseName(ExportPhase phase);

struct ExportProgress {
    ExportPhase phase = ExportPhase::Queued;
    std::int64_t framesDone = 0; ///< video frames encoded (audio-only: blocks of 1024 sample frames)
    std::int64_t framesTotal = 0;
    double fps = 0.0; ///< encoded frames per second so far
    double elapsedSeconds = 0.0;
    std::optional<double> etaSeconds;
    std::string videoEncoder; ///< the encoder actually used, once chosen
    std::string audioEncoder;
    double fraction() const { return framesTotal > 0 ? static_cast<double>(framesDone) / framesTotal : 0.0; }
};

struct ExportRequest {
    /// The project revision to export: acquired once (Principle 3); later edits do not affect the job.
    ProjectSnapshot snapshot;
    SequenceId sequence;
    ExportSettings settings;
    std::filesystem::path output;
    /// The project file, to resolve media paths relative to it (MediaLocator::relativePath). Optional.
    std::filesystem::path projectFile;
    /// Empty = defaultVideoSourceFactory().
    VideoSourceFactory videoSource;
    /// Optional: called on the job's thread at every phase change and at most ~10 times a second while
    /// rendering. Must be quick and must not call back into the job.
    std::function<void(const ExportProgress&)> onProgress;
};

struct ExportResult {
    std::filesystem::path output;
    std::int64_t frames = 0;       ///< video frames written
    std::int64_t audioSamples = 0; ///< audio sample frames written (per channel)
    Ticks duration{};
    std::string videoEncoder;                  ///< e.g. "h264_nvenc"; empty without video
    std::vector<std::string> encoderFallbacks; ///< encoders tried and rejected, with the reason
    std::string audioEncoder;
    std::string videoSource;     ///< IVideoFrameSource::description()
    double seconds = 0.0;        ///< wall-clock time of the job
    double realtimeFactor = 0.0; ///< exported duration / wall-clock time
    /// The captions sidecar written next to the output (CaptionExport::Srt / ::Vtt); empty when none.
    std::filesystem::path captions;
    /// What the file does not have although the project asks for it: audio plugins missing on this computer or that
    /// failed to load (known before rendering), and plugins that crashed, hung, were late or produced invalid samples
    /// while it rendered (their processing is missing from then on). Empty when the export is complete.
    std::vector<std::string> warnings;
    /// I.9: the output file's size in bytes, and how many encodes it took (2: a target-size export that ended over
    /// its size the first time and was encoded again at the rate that fits).
    std::int64_t bytes = 0;
    int attempts = 1;
    std::int64_t videoBitrate = 0; ///< the bitrate the video was encoded at (Bitrate and TargetSize; bits/s)
};

/// One export, run synchronously by run() on the calling thread (a job-queue worker, never the GUI thread).
///
/// Pipeline: fixed snapshot → per output frame k at the exact rational time start + k/rate → video source
/// (bounded, in order) → swscale to the encoder's pixel format (BT.709, limited range) → libavcodec encoder;
/// audio rendered offline through the playback mix path (playback::renderTrack per track + audio::Mixer,
/// MediaAudioFeed over the snapshot) with exact sample counts, encoded alongside the video and interleaved by
/// libavformat. AAC priming is carried as the stream's initial padding (edit list in MP4/MOV), the last
/// frame's padding is trimmed. The file is written to a temporary name next to the output and renamed into
/// place only when complete, so a failed or cancelled export leaves no partial file (an existing output is
/// replaced atomically on success).
///
/// Errors: InvalidArgument (settings, range), NotFound (unknown sequence; offline or missing originals: the
/// message lists them), Unsupported (no working encoder or video source), EncoderError, IoError, Cancelled.
class ExportJob {
public:
    explicit ExportJob(ExportRequest request);
    ~ExportJob();
    ExportJob(const ExportJob&) = delete;
    ExportJob& operator=(const ExportJob&) = delete;

    std::expected<ExportResult, Error> run(std::stop_token stop = {});

    /// Thread-safe snapshot of the progress.
    ExportProgress progress() const;
    const ExportRequest& request() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

using ExportJobId = std::uint64_t;

struct ExportJobStatus {
    ExportJobId id = 0;
    std::filesystem::path output;
    std::string presetId;
    ExportProgress progress;
    std::optional<ExportResult> result; ///< set when Done
    std::optional<Error> error;         ///< set when Failed or Cancelled
};

/// The export job queue: one worker thread runs jobs in submission order (exports are heavy; one at a time).
/// All member functions are thread-safe. The listener is called on the worker thread whenever a job's status
/// changes (throttled to ~10 Hz while rendering); bridge it to the GUI thread yourself. The destructor
/// cancels the running job and everything queued, then joins.
class ExportQueue {
public:
    using Listener = std::function<void(const ExportJobStatus&)>;

    explicit ExportQueue(Listener listener = {});
    ~ExportQueue();
    ExportQueue(const ExportQueue&) = delete;
    ExportQueue& operator=(const ExportQueue&) = delete;

    ExportJobId enqueue(ExportRequest request);
    /// Cancels a queued or running job (no effect once it finished).
    void cancel(ExportJobId id);
    std::vector<ExportJobStatus> jobs() const;
    std::optional<ExportJobStatus> status(ExportJobId id) const;
    /// Blocks until the job has finished (Done, Failed or Cancelled) and returns its final status.
    std::optional<ExportJobStatus> wait(ExportJobId id) const;

private:
    struct Entry;
    void run(std::stop_token stop);
    void notify(const ExportJobStatus& status);

    Listener m_listener;
    mutable std::mutex m_mutex;
    mutable std::condition_variable_any m_changed;
    std::deque<std::shared_ptr<Entry>> m_entries;
    ExportJobId m_nextId = 1;
    std::jthread m_worker;
};

} // namespace wabcut::exporter
