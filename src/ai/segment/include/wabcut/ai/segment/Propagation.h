#pragma once

// Segmentation and propagation of an object through a shot (AI-14; docs/ai.md "AI Object Mask"):
//
//   Segmenter    the model: a prompt (clicks, a box) on one picture starts an object; each next picture is tracked from
//                the object's memory. The app and wabcut-cli use the wabcut-embed helper (HelperSegmenter, SAM 2.1);
//                tests use scripted ones.
//   FrameSource  the media's pictures in STORED orientation at the model's input size (masks and mattes live in
//                normalised stored-frame coordinates), and its frame grid.
//   propagate()  the prompt frame, then frame by frame forward and / or backward through the span, stopping at the
//                shot cuts around the prompt (C.5) unless asked to go across: the matte frames, each with its
//                confidence (the model's predicted IoU times its certainty that the object is there) and bounding box.
//                A job: progress through a callback, cancellable.

#include <wabcut/ai/search/Embedding.h>
#include <wabcut/ai/segment/Matte.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::segment {

/// One picture's segmentation.
struct Segmentation {
    MatteImage matte;          ///< at the requested size
    float iou = 0.0f;          ///< the model's predicted IoU
    float objectScore = 0.0f;  ///< > 0: the object is there
    bool present = false;
    double milliseconds = 0.0;
    std::string device;
};

class Segmenter {
public:
    virtual ~Segmenter() = default;
    /// The model's input side (SAM 2.1: 1024): pictures are this size, square, the frame stretched.
    virtual int inputSize() const = 0;
    /// Identity for cache keys ("sam2.1-tiny-video-aa7a8542"), file-name safe.
    virtual std::string modelVersion() const = 0;
    /// A new object from `prompt` (normalised points and box) on `picture`; `frame` / `frames`: its index in the run and
    /// the run's length.
    virtual std::expected<Segmentation, Error> prompt(const search::Image& picture, const Prompt& prompt, int frame, int frames,
                                                       int width, int height, std::stop_token stop) = 0;
    /// The next picture of the object.
    virtual std::expected<Segmentation, Error> track(const search::Image& picture, int frame, int frames, int width, int height,
                                                      std::stop_token stop) = 0;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;
    /// The picture at `mediaTime` (stored orientation, `size` x `size`, stretched). Errors: the decoder's.
    virtual std::expected<search::Image, Error> pictureAt(Ticks mediaTime, int size) = 0;
    /// The media's frame interval.
    virtual Ticks frameDuration() const = 0;
    /// The media's frames: [first, end).
    virtual TimeRange frames() const = 0;
    /// The frame's display aspect (width / height, sample aspect included).
    virtual double aspect() const = 0;
};

enum class Direction { None, Forward, Backward, Both };
std::string_view toString(Direction direction);
std::optional<Direction> directionFromString(std::string_view text);

struct PropagationOptions {
    Prompt prompt;                ///< at its media time
    Direction direction = Direction::Both;
    std::optional<TimeRange> span; ///< media time to stay within (a clip's source range, In / Out); default the media
    bool acrossShots = false;     ///< else stop at the cuts around the prompt
    std::vector<Ticks> cuts;      ///< shot cuts of the media (media time, C.5)
    int longSide = kDefaultMatteSide;
    /// Progress 0..1 with what is going on; returning false cancels.
    std::function<bool(double, const std::string&)> progress;
    /// Each frame as it is made (the UI's confidence strip).
    std::function<void(const MatteFrame&)> frameDone;
};

struct PropagationResult {
    MatteTrack track;              ///< fingerprint, stream and model left for the caller to fill
    std::optional<Ticks> stoppedBefore; ///< a cut before the prompt that ended the backward run
    std::optional<Ticks> stoppedAfter;  ///< a cut after it that ended the forward run
    std::size_t lost = 0;          ///< frames where the model did not see the object
    double seconds = 0.0;
    double framesPerSecond = 0.0;
    std::string device;
};

/// Errors: NotFound (the prompt is outside the media), InvalidArgument (an empty prompt), Cancelled, the model's and
/// the decoder's.
std::expected<PropagationResult, Error> propagate(Segmenter& segmenter, FrameSource& frames, const PropagationOptions& options,
                                                  std::stop_token stop = {});

} // namespace wabcut::ai::segment
