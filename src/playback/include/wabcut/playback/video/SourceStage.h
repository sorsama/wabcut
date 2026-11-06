#pragma once

// The source stage of the AI video effects (0.7, model/AiEffects.h). The evaluator knows which AI effects a clip has
// (LayerSpec::ai); it never runs a model. Whoever owns the processed pictures (the app, wabcut-cli and export install
// the cache-backed one, wabcut/ai/video/SourceProcessor.h) turns the decoded picture of such a layer into the processed
// one: buildCompositeLayers asks it, so the preview and the export show the same pictures. Where it has none it says so
// and the layer keeps the unprocessed picture (CompositeLayer::aiUnprocessed; the monitor shows a badge, the export
// processes what is missing first).

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::playback {

struct SourceStageQuery {
    ClipId clip;
    const MediaItem* item = nullptr;
    int streamIndex = 0;
    const AiChain* chain = nullptr;
    /// The decoded (possibly proxy or reduced) picture the layer would show, and the source time it was asked for.
    const media::VideoFrame* decoded = nullptr;
    Ticks sourceTime{};
};

struct SourceStageResult {
    /// The processed picture (its size is the media's times the chain's scale) mixed with the decoded one by the chain's
    /// Amount; invalid when none exists.
    media::VideoFrame frame;
    /// The chain has no processed picture for this frame: the decoded one is shown as it is.
    bool missing = false;
};

/// 0.7 (AI Refocus): the depth map of a frame, 8 bits, 0 = the nearest part of the picture .. 255 = the farthest, at a working
/// size with the frame's aspect (the evaluator stretches it over the frame).
struct DepthMap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> distance; ///< width x height, rows top-down
};

struct DepthQuery {
    ClipId clip;
    const MediaItem* item = nullptr;
    int streamIndex = 0;
    /// The decoded picture the layer shows (its time and, for an export that has to make the map, its pixels).
    const media::VideoFrame* decoded = nullptr;
    Ticks sourceTime{};
};

struct DepthResult {
    std::shared_ptr<const DepthMap> map; ///< null when none exists
    bool missing = false;                ///< no depth map for this frame yet: the effect leaves the picture as it is
};

/// 0.7 (AI Face Retouch): the face map of a frame, three 8-bit coverage planes at a working size with the frame's aspect (the
/// evaluator stretches it over the frame): how much of each pixel is skin (face, ears, neck; not eyes, brows, lips, hair or
/// glasses), the inside of the mouth, and the eyes.
struct FaceMap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> coverage; ///< width x height x 3 (skin, mouth, eyes), rows top-down
};

struct FaceQuery {
    ClipId clip;
    const MediaItem* item = nullptr;
    int streamIndex = 0;
    /// The decoded picture the layer shows (its time and, for an export that has to make the map, its pixels).
    const media::VideoFrame* decoded = nullptr;
    Ticks sourceTime{};
};

struct FaceResult {
    std::shared_ptr<const FaceMap> map; ///< null when none exists
    bool missing = false;               ///< no face map for this frame yet: the effect leaves the picture as it is
};

/// 0.7 (AI Eye Contact): the eye measurements of a frame (model/EyeContact.h): for every face the landmark model found, its two eyes' opening,
/// iris and the face's turn, in fractions of the picture they were measured on (which is the frame, at most 1280 pixels wide).
struct GazeMap {
    std::vector<GazeFace> faces;
};

struct GazeQuery {
    ClipId clip;
    const MediaItem* item = nullptr;
    int streamIndex = 0;
    /// The decoded picture the layer shows (its time and, for an export that has to make the measurements, its pixels).
    const media::VideoFrame* decoded = nullptr;
    Ticks sourceTime{};
};

struct GazeResult {
    std::shared_ptr<const GazeMap> map; ///< null when none exists (a frame without faces has a map with no faces)
    bool missing = false;               ///< not measured yet: the effect leaves the picture as it is
};

/// 0.7 (AI Frame Interpolation): the picture at `sourceTime` between two decoded frames (the one containing it and the next).
struct InterpolationQuery {
    ClipId clip;
    const MediaItem* item = nullptr;
    int streamIndex = 0;
    const media::VideoFrame* first = nullptr;
    const media::VideoFrame* second = nullptr;
    Ticks sourceTime{};
    double weight = 0.0; ///< where `sourceTime` lies between first's and second's start (0 < weight < 1)
};

struct InterpolationResult {
    media::VideoFrame frame; ///< the in-between picture, valid when it exists
    bool missing = false;    ///< not made yet: the evaluator falls back to the clip's flow or blend
};

class SourceFrameProcessor {
public:
    virtual ~SourceFrameProcessor() = default;
    /// Called from the playback and export threads at the frame rate: thread-safe, and never waits for a model.
    virtual SourceStageResult process(const SourceStageQuery& query) = 0;
    /// AI Refocus's depth map of a frame, and AI Frame Interpolation's in-between picture: same rules (cache only in playback,
    /// made on the spot in an export). A processor without them reports everything missing.
    virtual DepthResult depth(const DepthQuery& query) {
        (void)query;
        return DepthResult{nullptr, true};
    }
    virtual InterpolationResult interpolate(const InterpolationQuery& query) {
        (void)query;
        return InterpolationResult{{}, true};
    }
    /// AI Face Retouch's face map of a frame: same rules as depth().
    virtual FaceResult faces(const FaceQuery& query) {
        (void)query;
        return FaceResult{nullptr, true};
    }
    /// AI Eye Contact's eye measurements of a frame: same rules as depth().
    virtual GazeResult gaze(const GazeQuery& query) {
        (void)query;
        return GazeResult{nullptr, true};
    }
};

/// The process's processor (none by default: AI layers show their unprocessed pictures and count as missing). Thread-safe.
void setSourceFrameProcessor(std::shared_ptr<SourceFrameProcessor> processor);
std::shared_ptr<SourceFrameProcessor> sourceFrameProcessor();

/// What a processor does with a picture it does not have, on the calling thread. Show (the default, playback): nothing, it is
/// reported missing and the decoded picture stays. ProcessMissing (an export): the processor makes the picture on the spot
/// (decoded frames of an export are the originals at full size), so an export is never short of one; when it cannot (no model
/// on this computer, no helper in this build, a failure) it says why through the scope and the export stops with that reason.
enum class SourceStageMode { Show, ProcessMissing };

/// Sets the calling thread's mode for its lifetime (and a stop token the processor passes to the model run, so a cancelled
/// export does not wait for a picture).
class ScopedSourceStageMode {
public:
    explicit ScopedSourceStageMode(SourceStageMode mode, std::stop_token stop = {});
    ~ScopedSourceStageMode();
    ScopedSourceStageMode(const ScopedSourceStageMode&) = delete;
    ScopedSourceStageMode& operator=(const ScopedSourceStageMode&) = delete;

    /// Why the processor could not make a picture since the scope began (the first reason), or nullopt.
    std::optional<std::string> failure() const;

    struct State; ///< (internal)

private:
    State* m_previous = nullptr;
    std::unique_ptr<State> m_state;
};

/// The calling thread's mode and stop token (Show / none outside a scope).
SourceStageMode currentSourceStageMode();
std::stop_token currentSourceStageStop();
/// A processor reports that it could not make a picture (the first call in a scope counts); ignored outside one.
void reportSourceStageFailure(std::string reason);

} // namespace wabcut::playback
