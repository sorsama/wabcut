#pragma once

// AI Object Mask (AI-14; docs/ai.md "AI Object Mask", docs/masks.md "Object masks", docs/commands.md): click on a
// person or an object (or draw a box around it, or name it) on a frame of a clip, and get a mask that follows it
// through the shot. The commands of the catalog, EditSequence, each one undo step (dryRun previews it):
//
//   mask.objectSelect  positive / negative clicks and / or a box on a frame, or a subject ("person", "face", "car", any
//                      COCO class; optionally its colour and which one): SAM 2.1 segments it there and, as a job,
//                      tracks it forward, backward or both through the clip (or In / Out), stopping at the shot cuts
//                      (C.5) unless acrossShots. The result is a matte mask (MaskShape::Matte) on an effect of the clip
//                      (blur the background, grade only the subject) or, without "effect", on the clip's opacity mask
//                      (a cut-out that removes the background).
//   mask.refine        more clicks or a box on a frame of a matte mask: the frame's prompt grows, the object is
//                      segmented again there and re-tracked from it (forward by default; both ways on a prompted frame).
//   mask.propagate     tracks a matte mask's object further: after "propagate": "none", across a cut, or past In / Out.
//
// The mattes go to the analysis cache (Matte.h: per frame, run-length encoded, keyed by the media's fingerprint, the
// stream, the model and a hash of the prompts and the run): a request that was made before (a dryRun's apply, undo and
// redo, the same clicks again) reads them back instead of running the model. The mask names its track and keeps the
// object's bounding box as Hold keyframes (the union over windows of frames, so it always contains the object): where a
// frame has no matte (not tracked there, the cache cleared, another computer) the box stands in, and a redaction
// covers it, never nothing.
//
// Nested and multicam clips (0.2): the three commands segment what the clip shows - the nested sequence's canvas, a
// multicam source's active angle, rendered by the export renderer at nested time (SegmentServices::nestedFrames) -
// and the mask goes on the clip itself, its matte naming the nested sequence (MaskMatte::sequence) at nested time (the
// clip's curve time). Nothing is flattened: a multicam clip switches angles as before and its mask stays where it was
// on the frame (as a tracked mask does in Premiere); mask.propagate re-tracks the angle it shows then. The cuts inside
// the nest (its clips' edit points on the tracks the clip shows) stop the tracking like shot cuts.
//
// No redaction is made here: redaction.* stay out of the assistant's reach. The app makes AI-tracked redactions with
// segmentObject() and objectMask() (a Redaction whose region is the matte mask).

#include <wabcut/ai/segment/Matte.h>
#include <wabcut/ai/segment/Propagation.h>
#include <wabcut/ai/vision/Subjects.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::analysis {
class AnalysisProvider;
}

namespace wabcut::ai::segment {

inline constexpr double kDefaultMatteFeather = 0.004; ///< frame heights: about 4 px at 1080p, a natural edge
inline constexpr std::size_t kMaxBoxKeys = 240;        ///< bounding-box keys per matte mask

/// Where a segmentation is (the UI's progress strip and per-frame confidence).
struct SegmentProgress {
    double fraction = 0.0;  ///< 0..1
    std::string message;    ///< "Tracking forward: frame 12 of 240"
    bool frame = false;     ///< a frame was just made:
    Ticks mediaTime{};
    float confidence = 0.0f;
    bool present = false;
};

/// What the object-mask commands need from the host (wabcut-cli and the app: wabcut_ai_segment_media).
class SegmentServices : public command::CommandServices {
public:
    using SegmenterFactory = std::function<std::expected<std::shared_ptr<Segmenter>, Error>(std::stop_token)>;
    using FrameOpener = std::function<std::expected<std::unique_ptr<FrameSource>, Error>(const MediaItem& media, int videoStream)>;
    using Fingerprinter = std::function<std::expected<std::uint64_t, Error>(const MediaItem& media)>;
    /// What a nested or multicam clip of `state` shows, at nested sequence time (NestedFrameSource in full builds).
    using NestedFrameOpener = std::function<std::expected<std::unique_ptr<FrameSource>, Error>(const ProjectState& state, const Clip& clip)>;

    /// The model, for one run at a time (it may wait for the helper to start and load). Called only when the cache does
    /// not have the run.
    SegmenterFactory segmenter;
    /// The model's identity (Segmenter::modelVersion()), known without starting it: cache keys.
    std::string model;
    FrameOpener frames;
    Fingerprinter fingerprint;
    /// 0.2: nested and multicam clips' pictures. Null: such clips are refused with the reason.
    NestedFrameOpener nestedFrames;
    /// The analysis cache root the tracks go to (MatteStore).
    std::filesystem::path cacheRoot;
    /// "subject" prompts: faces, and COCO objects (FrameDetections::objects). Null: they fail with Unsupported.
    std::shared_ptr<vision::SubjectDetector> detector;
    /// Tells the renderer's matte source about a track just made (CacheMatteSource::remember). Optional.
    std::function<void(std::shared_ptr<const MatteTrack>)> remember;
    /// Called while a run goes, from the thread running it. Optional.
    std::function<void(const SegmentProgress&)> progress;
};

/// One run of the model over one object of one media stream.
struct ObjectRequest {
    /// Every prompt of the object, the one the run starts from LAST (refine moves the refined frame's prompt there).
    std::vector<Prompt> prompts;
    Direction direction = Direction::Both;
    std::optional<TimeRange> span; ///< media time to stay within (default the media)
    bool acrossShots = false;
    /// refine / propagate: the track whose frames this run's replace (same media, stream and size).
    std::shared_ptr<const MatteTrack> base;
};

struct ObjectResult {
    std::shared_ptr<const MatteTrack> track; ///< stored in the cache, remembered by the render source
    std::optional<Ticks> stoppedBefore;      ///< the cut that ended the backward run (media time)
    std::optional<Ticks> stoppedAfter;
    std::size_t made = 0;                    ///< frames this run segmented (0: read from the cache)
    std::size_t lost = 0;                    ///< of the track's frames, those where the object was not seen
    bool cached = false;
    double seconds = 0.0;
    double framesPerSecond = 0.0;
    std::string device;
};

/// Runs (or reads from the cache) `request` on stream `videoStream` (index into media.info.video) of `media`. `shots`
/// gives the cuts (null: none known, the run goes to the span's ends). `checkpoint(fraction, message)` is called between
/// frames: it may throw to stop the run (the commands' ExecutionContext::checkpoint) or return false to cancel.
/// Errors: InvalidArgument (no prompt, no video), NotFound (the prompt is outside the media), Unsupported (no model),
/// Cancelled, the model's, the decoder's, the cache's.
std::expected<ObjectResult, Error> segmentObject(SegmentServices& services, analysis::AnalysisProvider* shots, const MediaItem& media,
                                                 int videoStream, const ObjectRequest& request, std::stop_token stop = {},
                                                 const std::function<bool(double, const std::string&)>& checkpoint = {});

/// 0.2: `request` on what nested or multicam clip `clip` of `state` shows (SegmentServices::nestedFrames), at nested
/// sequence time: prompts, span and frames are nested times (clip.sourceTimeAt). The cuts are the nest's edit points
/// on the tracks the clip shows (nestedCuts). Errors: as segmentObject(), InvalidArgument for a clip that is not nested.
std::expected<ObjectResult, Error> segmentNested(SegmentServices& services, const ProjectState& state, const Clip& clip,
                                                 const ObjectRequest& request, std::stop_token stop = {},
                                                 const std::function<bool(double, const std::string&)>& checkpoint = {});
/// The identity of what nested clip `clip` shows: a hash of its nested sequences (every level), the media they use and
/// the clip's angles. Edits inside the nest change it; edits elsewhere in the project do not.
std::uint64_t nestedFingerprint(const ProjectState& state, const Clip& clip);
/// The edit points inside nested clip `clip`'s sequence on the video tracks it shows (nested time, sorted, without 0
/// and the end).
std::vector<Ticks> nestedCuts(const ProjectState& state, const Clip& clip);

/// A subject on a picture: the box of the detection of `kind` ("face", or a COCO class name: "person", "car", "dog",
/// ...) that best fits: of colour `colour` when given ("red", "orange", "yellow", "green", "cyan", "blue", "purple",
/// "pink", "white", "black", "grey"), then by `pick` ("largest", "center", "left", "right"). Errors: InvalidArgument
/// (an unknown kind, colour or pick), NotFound (none there), the detector's.
std::expected<MatteBox, Error> findSubject(vision::SubjectDetector& detector, const search::Image& picture, double aspect,
                                           std::string_view kind, std::string_view colour, std::string_view pick,
                                           std::stop_token stop = {});
/// `picture` (any size, the frame stretched, as FrameSource::pictureAt gives it) resampled to its display aspect,
/// `longSide` on the long side: what the detectors expect.
search::Image naturalPicture(const search::Image& picture, double aspect, int longSide = 640);
/// The 80 COCO class names, in class order.
std::span<const std::string_view> cocoClassNames();

/// The matte mask showing `track` of `media`: MaskShape::Matte, its bounding box as Hold keys, `feather` and
/// `expansion` (frame heights; negative chokes). `nested`: a track of a nested clip's picture (segmentNested) instead,
/// `media` then invalid.
Mask objectMask(const MatteTrack& track, MediaId media, std::string name, double feather = kDefaultMatteFeather, double expansion = 0.0,
                SequenceId nested = {});
/// Points `mask` at `track` (its matte and its bounding-box keys); its other settings stay.
void setMatteTrack(Mask& mask, const MatteTrack& track, MediaId media, SequenceId nested = {});

/// Adds mask.objectSelect, mask.refine and mask.propagate.
void registerObjectMaskCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::segment
