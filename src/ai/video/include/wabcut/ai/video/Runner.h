#pragma once

// The AI video-effect runner (0.7): a clip's frames through a chain of AI stages into the processed cache, with progress,
// Cancel, resume and an estimate. Nothing here knows how frames are decoded or how a model runs: the frame source (the
// decoder, in wabcut/ai/video/MediaFrames.h) and the backend (wabcut-embed, in HelperBackend.h) are injected, so the
// whole flow - skipping what the cache has, a chain of steps with strength / grain / resize, alpha, cancel in the middle,
// a failing model - is tested with fakes and no model.
//
// For every frame not in the cache yet: decode it to a 16-bit RGB picture, run the chain's steps (each: model on the whole
// picture -> mixed into its input by the step's strength; denoise puts back `grain` of the removed noise; the fast 2x halves
// a 4x result), resize the alpha plane to the result, encode the picture as the planes the compositor reads and store it.
// Frames already stored are skipped, so a cancelled or failed run continues where it stopped, and a second clip of the same
// media with the same effects costs nothing.

#include <wabcut/ai/video/Backend.h>
#include <wabcut/ai/video/Picture.h>
#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::video {

/// One decoded frame of the media as the runner sees it. The picture is made on demand (conversion costs time the runner
/// skips for frames the cache has).
struct SourceFrame {
    Ticks pts{};
    Ticks duration{};
    /// The frame's colour as a picture (16-bit RGB, with alpha when the media has it).
    std::function<std::expected<Picture, Error>()> picture;
    /// A processed picture (any size) as the planes to store, with this frame's timing and colour description.
    std::function<std::expected<StoredFrame, Error>(const Picture&)> encode;
};

/// 0.7 (AI Frame Interpolation): how finely the position between two frames is told apart in the cache: 1/64 of a frame period.
inline constexpr int kInterpolationSteps = 64;
/// The cache key of the picture at exact source time `time` for a media of frame period `framePeriod`: the time in 1/64 frame
/// periods. The runner and the evaluator compute it from the same exact source time, so they never disagree about which of the
/// clip's frames a stored picture is (the decoder's own timestamps do not enter into it).
std::int64_t interpolationKey(Ticks time, Ticks framePeriod);
/// The frame between `a` and `b` at `timestep` (0..1) with the backend's interpolation model; the pictures must be one size.
/// Speeds are recorded in `cache` (when given). Errors: the backend's, Cancelled, InvalidArgument, DecoderError (wrong size).
std::expected<Picture, Error> interpolatePicture(const Picture& a, const Picture& b, double timestep, IVideoBackend& backend,
                                                 ProcessedCache* cache, std::stop_token stop);

/// One picture through a chain (what the runner does per frame, also for the export that makes a missing picture on the spot):
/// every step's model on the whole picture, mixed into its input by the step's strength, the removed noise of a denoise put
/// back by its grain, the fast 2x halved, the alpha plane resized to the result. The measured speed of each step goes to
/// `cache` (when given). Errors: the backend's, Cancelled, DecoderError (a picture of the wrong size).
std::expected<Picture, Error> processPicture(const Picture& in, const AiChain& chain, IVideoBackend& backend, ProcessedCache* cache,
                                             std::stop_token stop);

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    /// The frame that contains `time` (the decoder's choice); nullopt past the end of the media. Errors: IoError,
    /// DecoderError, Cancelled.
    virtual std::expected<std::optional<SourceFrame>, Error> frameAt(Ticks time, const std::stop_token& stop) = 0;
};

/// What a job makes. Pictures: the chain's processed picture of every frame in the range (AI Restore, AI Denoise). Depth: the
/// 8-bit depth map of every frame (AI Refocus; the chain is the depth stage). Interpolation: the picture at each exact source
/// time of `times` (AI Frame Interpolation), made from the two frames around it. Faces: the face map of every frame (AI Face
/// Retouch): three 8-bit coverage planes (skin, mouth, eyes) at the working size. Gaze: the eye measurements of every frame (AI Eye Contact,
/// Gaze.h): a small table of floats, the faces' eyes, irises and turns.
enum class JobKind { Pictures, Depth, Interpolation, Faces, Gaze };

/// The stored form of a face map: three 8-bit planes (skin, mouth, eyes), each at the map's size, with the frame's timing. `map` is the
/// helper's answer (a picture whose red, green and blue are the planes, 16 bits). Errors: DecoderError (not a valid map).
std::expected<StoredFrame, Error> faceStoredFrame(const Picture& map, Ticks pts, Ticks duration);

struct Job {
    JobKind kind = JobKind::Pictures;
    CacheIdentity identity;
    AiChain chain;
    TimeRange range;           ///< the source time to cover: [start, end)
    /// Interpolation: the exact source times to make pictures for (any order; the run goes through them ascending), and the
    /// media's frame period (the key's unit, interpolationKey).
    std::vector<Ticks> times;
    Ticks framePeriod{};
    int expectedFrames = 0;    ///< how many frames the range holds, for the progress bar (0 = unknown)
    int sourceWidth = 0;       ///< of the media's pictures, for the speed record and the estimate
    int sourceHeight = 0;
    Provenance provenance;     ///< written to the cache when the run starts
};

struct Progress {
    int processed = 0;         ///< pictures made in this run
    int skipped = 0;           ///< pictures the cache already had
    int total = 0;             ///< expectedFrames, or what the run has seen so far
    double msPerFrame = 0.0;   ///< smoothed, of the pictures made
    double secondsLeft = -1.0; ///< -1 = unknown
    std::string device;
};

struct RunResult {
    int processed = 0;
    int skipped = 0;
    std::string device;
    bool cancelled = false;
};

class Runner {
public:
    /// The runner uses the three but owns none of them; they outlive it.
    Runner(IFrameSource& source, IVideoBackend& backend, ProcessedCache& cache);

    /// Processes the job's range. Errors end the run (what was made stays in the cache): the backend's (a model that is not
    /// installed, a helper that keeps crashing), the source's; a cancel returns a result with `cancelled`.
    std::expected<RunResult, Error> run(const Job& job, const std::function<void(const Progress&)>& progress, std::stop_token stop);

    /// One picture through a chain (what run() does per frame): the result is the chain's output size. Errors as run().
    std::expected<Picture, Error> process(const Picture& in, const AiChain& chain, std::stop_token stop) {
        return processPicture(in, chain, m_backend, &m_cache, std::move(stop));
    }

private:
    std::expected<RunResult, Error> runInterpolation(const Job& job, const std::function<void(const Progress&)>& progress,
                                                     std::stop_token stop);
    IFrameSource& m_source;
    IVideoBackend& m_backend;
    ProcessedCache& m_cache;
};

/// A guess of the seconds a run takes: `frames` pictures of `width` x `height` through `chain`, from the speeds measured
/// before (cache.speed) where there are some, else the catalogue's first guesses. `gpu`: a GPU provider is expected.
double estimateSeconds(const AiChain& chain, int width, int height, int frames, ProcessedCache* cache, bool gpu);

/// "about 4 min", "about 1 h 20 min", "under a minute".
std::string durationText(double seconds);

} // namespace wabcut::ai::video
