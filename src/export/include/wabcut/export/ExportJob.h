#pragma once

// ExportJob renders one sequence of a fixed project snapshot to a file; ExportQueue runs jobs in background.

#include <wabcut/export/DynamicMetadata.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/PreviewCache.h>
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

/// 0.2: Paused - a running export held between two frames (ExportQueue::pause, Media Encoder's Pause).
enum class ExportPhase : std::uint8_t { Queued, Preparing, Rendering, Finalizing, Done, Failed, Cancelled, Paused };
std::string_view exportPhaseName(ExportPhase phase);

/// 0.2: holds a running export between two frames while paused. Thread-safe.
class PauseGate {
public:
    void pause();
    void resume();
    bool paused() const;
    /// Blocks while paused; false when `stop` was requested meanwhile.
    bool wait(const std::stop_token& stop);

private:
    mutable std::mutex m_mutex;
    std::condition_variable_any m_changed;
    bool m_paused = false;
};

struct ExportProgress {
    ExportPhase phase = ExportPhase::Queued;
    std::int64_t framesDone = 0; ///< video frames encoded (audio-only: blocks of 1024 sample frames)
    std::int64_t framesTotal = 0;
    double fps = 0.0; ///< encoded frames per second so far
    double elapsedSeconds = 0.0;
    std::optional<double> etaSeconds;
    std::string videoEncoder; ///< the encoder actually used, once chosen
    std::string audioEncoder;
    /// 0.7: the first pass of an HDR10+ export is running (the frames are measured at half size before they are encoded).
    bool analyzing = false;
    double fraction() const { return framesTotal > 0 ? static_cast<double>(framesDone) / static_cast<double>(framesTotal) : 0.0; }
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
    /// Optional (AI-15: Content Credentials): runs on the job's thread once the file is complete and closed, before
    /// it is renamed into place, with that temporary file; it may rewrite the file (e.g. sign a C2PA manifest into
    /// it). It never fails the export: it returns a warning for ExportResult::warnings ("" = none), and a step that
    /// cannot do its work must leave the file as it was. Exceptions become warnings too.
    std::function<std::string(const std::filesystem::path& file, std::stop_token stop)> finalize;
    /// 0.2: pauses the export between frames while engaged (the queue sets it). Optional.
    std::shared_ptr<PauseGate> pause;
    /// 0.2, Use Previews (ExportRenderOptions::usePreviews): the project's preview render cache (C.15). Optional.
    std::shared_ptr<PreviewCache> previews;
};

/// 0.7 (card 049): a reason some frames of a smart-rendered export were encoded again, and where it first happens.
struct SmartRenderNote {
    std::string reason;        ///< "Clip starts inside a GOP", "Effects on the clip", ...
    std::int64_t frames = 0;   ///< how many frames
    std::int64_t firstFrame = 0; ///< the first of them (output frame)
    std::string firstTimecode;   ///< that frame's timecode in the sequence
    bool operator==(const SmartRenderNote&) const = default;
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
    /// while it rendered (their processing is missing from then on), and what a finalize step could not do (the file
    /// itself is complete), e.g. "Content Credentials not signed: ...". Empty when the export is complete.
    std::vector<std::string> warnings;
    /// I.9: what the picture was written as, and a PQ export's measured content light levels (nits; 0 = not PQ).
    DynamicRange dynamicRange = DynamicRange::Sdr;
    int maxCll = 0;
    int maxFall = 0;
    /// 0.6 (#72): the colour space written, and what SDR Conform mapped (an SDR export of an HDR or Wide Gamut setup
    /// with SDR Conform on; nullopt otherwise).
    OutputColorSpace outputSpace = OutputColorSpace::Rec709;
    std::optional<SdrConformReport> sdrConform;
    /// I.9: the output file's size in bytes, and how many encodes it took (2: a target-size export that ended over
    /// its size the first time and was encoded again at the rate that fits).
    std::int64_t bytes = 0;
    int attempts = 1;
    std::int64_t videoBitrate = 0; ///< the bitrate the video was encoded at (Bitrate and TargetSize; bits/s)
    // 0.2:
    std::int64_t imageFiles = 0;      ///< Container::Image: the stills written (`output` is the first)
    int chapters = 0;                 ///< chapters written from chapter markers
    std::string startTimecode;        ///< the timecode track's / BWF's start ("" = none written)
    int subtitleCues = 0;             ///< cues in the soft subtitle track
    std::filesystem::path xmp;        ///< the XMP sidecar written (empty: none, or embedded)
    bool xmpEmbedded = false;
    std::optional<double> loudnessLufs; ///< Loudness Normalization: what the mix measured (integrated, LUFS)
    double loudnessGainDb = 0.0;        ///< and the gain applied
    std::filesystem::path loudnessLog;  ///< "<output>.loudness.txt" when asked for
    std::int64_t copiedFrames = 0;      ///< smart rendering: frames whose packets were copied, not encoded
    /// 0.7 (card 049): "H.264" or "HEVC" when long-GOP smart rendering looked at the export (even when nothing could be
    /// copied), else empty; the frames it encoded again, by reason, most first (SmartRenderNote).
    std::string smartRenderCodec;
    std::vector<SmartRenderNote> smartRenderNotes;
    // 0.6 (#73): broadcast delivery - what the export details name.
    std::string operationalPattern;              ///< MXF: "OP1a" or "OP-Atom" ("" otherwise)
    std::string materialUmid;                    ///< MXF: the (video) file's material package UMID
    std::vector<std::filesystem::path> extraFiles; ///< OP-Atom: the audio files ("<name>_A1.mxf", ...)
    std::string audioLayout;                     ///< MXF: "8 mono tracks · PCM 24-bit 48 kHz · 1-2 stereo mix, 3-8 silent"
    std::string embeddedCaptions;                ///< Embed: "CEA-608 CC1 + CEA-708 service 1 in an SMPTE 436M track"
    int embeddedCaptionCues = 0;                 ///< Embed: the captions carried (0 with a warning: none in the range)
    /// 0.7 (HDR10+ / Dolby Vision, DynamicMetadata.h): what was measured and written, shot by shot (kind None: nothing asked for).
    DynamicMetadataReport dynamicMetadata;
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

/// P1 (#27): an original media file an export would read but cannot find.
struct MissingOriginal {
    MediaId media;
    std::string name;
    std::string path; ///< as the project has it ("" when it has none)
};

/// P1 (#27): before an export starts, the original media it would read that is offline or missing - exactly what
/// ExportJob::run() stops on (the range resolve() picks, the tracks that play, nested and multicam clips' media; never
/// proxies, generated media skipped). Paths resolve against `projectFile` like the export's. In project order; empty
/// when every file is found or the settings do not resolve.
std::vector<MissingOriginal> missingOriginals(const ProjectState& snapshot, const Sequence& sequence,
                                              const ExportSettings& settings,
                                              const std::filesystem::path& projectFile = {});

using ExportJobId = std::uint64_t;

struct ExportJobStatus {
    ExportJobId id = 0;
    std::filesystem::path output;
    std::string presetId;
    ExportProgress progress;
    std::optional<ExportResult> result; ///< set when Done
    std::optional<Error> error;         ///< set when Failed or Cancelled
    /// 0.2: queued but held (Media Encoder's "Ready": sent to the queue, waiting for Start Queue / start()).
    bool held = false;
};

/// The export job queue: one worker thread runs jobs in submission order (exports are heavy; one at a time).
/// All member functions are thread-safe. The listener is called on the worker thread whenever a job's status
/// changes (throttled to ~10 Hz while rendering); bridge it to the GUI thread yourself. The destructor
/// cancels the running job and everything queued, then joins.
///
/// 0.2 (Media Encoder's queue): a job can be enqueued held (it waits for start()), a queued job paused (held) and a
/// running one paused between frames (ExportPhase::Paused) and resumed.
class ExportQueue {
public:
    using Listener = std::function<void(const ExportJobStatus&)>;

    explicit ExportQueue(Listener listener = {});
    ~ExportQueue();
    ExportQueue(const ExportQueue&) = delete;
    ExportQueue& operator=(const ExportQueue&) = delete;

    /// `hold`: the job waits for start() (Send to Media Encoder without starting the queue).
    ExportJobId enqueue(ExportRequest request, bool hold = false);
    /// Cancels a queued or running job (no effect once it finished).
    void cancel(ExportJobId id);
    /// 0.2: releases a held job (no effect on others).
    void start(ExportJobId id);
    /// 0.2: holds a queued job, pauses a running one between frames.
    void pause(ExportJobId id);
    /// 0.2: resumes a paused running job, releases a held one.
    void resume(ExportJobId id);
    /// 0.2: drops a finished job from jobs() (no effect on others).
    void remove(ExportJobId id);
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
