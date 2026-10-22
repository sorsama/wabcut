#pragma once

// Auto reframe (AI-9, docs/ai.md "Auto reframe"): a sequence made again at another aspect (9:16, 1:1, 4:5, 16:9) whose
// clips keep their subjects in frame. Nothing is rendered: every video clip of the copy gets ordinary, editable
// Transform keyframes (Position X/Y and Scale; sparse Bezier keys from the L1 camera path of CameraPath.h), so the
// editor can adjust any of them afterwards.
//
//   planClip()            the crop window of one clip over its source range, per shot (paths reset at C.5 cuts),
//                         from its media's SubjectAnalysis and a mode:
//                           follow   the main subject of each shot (its biggest, steadiest face; else a person;
//                                    else the salient region)
//                           faces    every face of the shot inside the window (zooming out when they cannot fit and
//                                    the fit mode allows it)
//                           speaker  the face that is talking (mouth activity, smoothed, with hysteresis)
//                           anchor   a fixed point the editor chose
//   applyPlan()           the plan as Transform curves on the clip (media-time keyframes, like the editor's own)
//   duplicateSequence()   a deep copy of a sequence with every id renewed, at a new size
//   reframeSequence()     all of it: a new sequence (a blur-fill track under letterboxed clips when asked) that
//                         records its source and parameters (Sequence::derivedFrom, generator "reframe/1"), so
//                         reframe.rerun can make it again after the source changed

#include <wabcut/ai/reframe/CameraPath.h>
#include <wabcut/ai/vision/Subjects.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::reframe {

inline constexpr std::string_view kGenerator = "reframe/1";

enum class ReframeMode { Follow, Faces, Speaker, Anchor };
std::string_view toString(ReframeMode mode);
std::optional<ReframeMode> reframeModeFromString(std::string_view text);

/// What to do where the subjects cannot fit in the window.
enum class FitMode {
    Crop,      ///< crop anyway (the window stays full; part of the subjects is cut)
    Letterbox, ///< zoom out until they fit, bars on black
    BlurFill,  ///< zoom out until they fit, over a blurred, cropped copy of the clip
};
std::string_view toString(FitMode mode);
std::optional<FitMode> fitModeFromString(std::string_view text);

struct Aspect {
    int width = 9;
    int height = 16;
    bool operator==(const Aspect&) const = default;
};
/// "9:16", "1:1", "4:5", "16:9" or any "W:H" of positive integers up to 100.
std::optional<Aspect> aspectFromString(std::string_view text);
std::string toString(Aspect aspect);

struct ReframeOptions {
    Aspect aspect;
    ReframeMode mode = ReframeMode::Follow;
    FitMode fit = FitMode::Crop;
    double smoothness = 0.5;              ///< 0 = follow closely .. 1 = move only when needed
    double anchorX = 0.5;                 ///< anchor mode: the point to keep centred (0..1 of the picture)
    double anchorY = 0.5;
    double margin = 0.04;                 ///< room kept around a subject inside the window (fraction of the window)
    int width = 0;                        ///< output size; 0 = the source's short side kept (1920x1080 -> 1080x1920)
    int height = 0;
    std::string name;                     ///< the new sequence's name; default "<source> (9:16)"
    double tolerancePixels = 1.0;         ///< keyframe fitting: largest distance from the optimal path
    double gridFps = 10.0;                ///< path samples per second
};

/// The parameters as the derivation record stores them (a JSON object), and back.
std::string optionsToJson(const ReframeOptions& options);
std::expected<ReframeOptions, std::string> optionsFromJson(std::string_view json);

/// The output size for a source size and options: the source's short side kept, even numbers.
std::pair<int, int> outputSize(int sourceWidth, int sourceHeight, const ReframeOptions& options);

/// How a video stream is displayed: its size after sample aspect and rotation.
struct DisplayGeometry {
    double width = 0.0;
    double height = 0.0;
};
DisplayGeometry displayGeometry(const VideoStreamInfo& stream);

/// 0.2: the one picture track a nested or multicam video clip shows - a multicam clip's active angle's video track, a
/// nested sequence's only enabled video track with clips. Null for layers (several such tracks) and other clips.
/// Analyses of the picture (Auto Reframe, Auto Zoom) read the media clips there and place what they find on the nested
/// canvas with canvasPlacement().
const Track* nestedPictureTrack(const ProjectState& state, const Clip& clip);

/// Where a clip's displayed picture sits on its sequence's canvas at curve time `time`, as fractions: a picture point
/// (x, y) in 0..1 is at canvas (offsetX + factorX * x, offsetY + factorY * y). From Position, Scale and Anchor; rotation
/// is not followed.
struct CanvasPlacement {
    double offsetX = 0.0;
    double offsetY = 0.0;
    double factorX = 1.0;
    double factorY = 1.0;
};
CanvasPlacement canvasPlacement(const Clip& clip, DisplayGeometry picture, double canvasWidth, double canvasHeight, Ticks time);

/// The crop window of one clip over its source range.
struct ClipPlan {
    std::vector<Ticks> times;         ///< source (curve) times of the samples
    std::vector<double> centreX;      ///< window centre, 0..1 of the displayed picture
    std::vector<double> centreY;
    std::vector<double> zoom;         ///< window size over the largest window of the aspect (1 = fill; > 1 = zoomed out)
    std::vector<double> lowerX;       ///< where the centre may be (the safe region within the picture), per sample
    std::vector<double> upperX;
    std::vector<double> lowerY;
    std::vector<double> upperY;
    std::vector<std::optional<vision::Box>> subjects; ///< what the window keeps in view (the safe region), per sample
    std::vector<Ticks> cuts;          ///< shot starts inside the range (the path resets there)
    double windowWidth = 1.0;         ///< the largest window of the aspect, 0..1 of the picture
    double windowHeight = 1.0;
    std::string guide;                ///< "faces", "people", "saliency", "anchor" or "centre": what led the path
    bool zoomedOut = false;           ///< somewhere the window is larger than the picture
};

/// Plans the window for the source range [sourceIn, sourceOut) of a picture of display size `display`, shown in a
/// `targetWidth` x `targetHeight` frame. `analysis` may be null (the window stays centred, or on the anchor).
ClipPlan planClip(const vision::SubjectAnalysis* analysis, TimeRange sourceRange, DisplayGeometry display, int targetWidth,
                  int targetHeight, const ReframeOptions& options);

/// The plan as the clip's Transform: Position X/Y and Scale curves (or constants), Anchor at 0; rotation kept.
/// Returns how many keyframes it wrote.
std::size_t applyPlan(Clip& clip, const ClipPlan& plan, DisplayGeometry display, int targetWidth, int targetHeight,
               const ReframeOptions& options);

/// A copy of sequence `source` with new ids for itself and everything in it (tracks, clips, link groups,
/// transitions, effect instances, caption tracks), sized `width` x `height`, appended to the project.
struct SequenceCopy {
    ProjectState state;
    SequenceId sequence;
    std::map<std::uint64_t, ClipId> clips; ///< old clip id -> new clip id
};
std::expected<SequenceCopy, Error> duplicateSequence(const ProjectState& state, SequenceId source, std::string name, int width,
                                                     int height);

struct ReframeResult {
    ProjectState state;
    SequenceId sequence;
    std::size_t clipsReframed = 0;
    std::size_t keyframes = 0;                ///< Transform keys written
    std::map<std::uint64_t, ClipPlan> plans;  ///< by clip id in the new sequence
    std::vector<std::string> warnings;
};

/// Subjects of a media item (a SubjectProvider, or a fixture). Errors are reported as warnings: that clip stays
/// centred.
using SubjectSource = std::function<std::expected<vision::SubjectAnalysis, Error>(const MediaItem&, int videoStream)>;

/// Makes the reframed sequence. `replace`: an existing sequence to make again in place (reframe.rerun: same id,
/// name and bin). Errors: NotFound (no such sequence), InvalidArgument (bad options; an empty sequence).
std::expected<ReframeResult, Error> reframeSequence(const ProjectState& state, SequenceId source, const ReframeOptions& options,
                                                    const SubjectSource& subjects, std::optional<SequenceId> replace = std::nullopt);

} // namespace wabcut::ai::reframe
