#pragma once

// Fitting pictures into the sequence frame (P.2, E5): Premiere's Scale to Frame Size, Set to Frame Size and Set to Fill
// Frame (Clip > Video Options and the clip's right-click menu) and Preferences > Media > Default Media Scaling.
//
//  * Scale to Frame Size (ClipContent::scaleToFrame) fits the picture into the frame before Motion: Motion > Scale stays
//    100 % and a Scale set later applies to the fitted picture. Premiere rasterises the source at the fitted size; Wabcut
//    scales the decoded picture by scaleToFrameFactor() in the evaluator (the same geometry; the resampling is the
//    renderer's).
//  * Set to Frame Size sets Motion > Scale to the factor that fits the whole picture inside the frame (letterboxed or
//    pillarboxed); Set to Fill Frame to the factor that covers the frame (the overhang is cropped). Both turn Scale to
//    Frame Size off and replace a keyframed Scale with the constant.
//  * Default media scaling (MediaItem::scaling) applies one of them to the video clips a source edit places (insert,
//    overwrite, Library drops, three-point edits) in a sequence of another frame size.
//
// A picture's size is its displayed size: the stored pixels times their pixel aspect (MediaInterpretation's when set),
// turned by the stream's display rotation; a nested sequence's frame; a generator's canvas.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace wabcut {

/// "none", "scaleToFrameSize", "setToFrameSize", "setToFillFrame".
std::string_view toString(MediaScaling scaling);
std::optional<MediaScaling> mediaScalingFromString(std::string_view text);

struct PictureSize {
    double width = 0.0;
    double height = 0.0;
};

/// The displayed size of a video stream as probed: its pixels times their aspect, turned by its rotation; nullopt for an
/// unknown size.
std::optional<PictureSize> streamPictureSize(const VideoStreamInfo& stream);
/// The displayed size of a media item's video stream `index` (see above), or nullopt (no such stream, unknown size).
std::optional<PictureSize> mediaPictureSize(const MediaItem& item, int index);
/// The displayed size of what a video clip shows: its media's stream, a nested sequence's frame; nullopt for audio clips,
/// missing media or an unknown size.
std::optional<PictureSize> clipPictureSize(const ProjectState& state, const Clip& clip);

/// The uniform scale that fits `picture` inside a `width` x `height` frame (`fill`: that covers it). 1 for an empty
/// picture or frame.
double frameFitScale(PictureSize picture, std::int32_t width, std::int32_t height, bool fill);

/// What Scale to Frame Size multiplies a clip's Motion > Scale by in a `width` x `height` sequence: frameFitScale() of its
/// picture for a clip with ClipContent::scaleToFrame, else 1. Evaluators (preview and export) apply it.
double scaleToFrameFactor(const ProjectState& state, const Clip& clip, std::int32_t width, std::int32_t height);
/// The same for a clip whose picture size the caller knows.
double scaleToFrameFactor(const Clip& clip, std::optional<PictureSize> picture, std::int32_t width, std::int32_t height);

} // namespace wabcut

namespace wabcut::edit {

/// Clip > Video Options > Scale to Frame Size on (or off) for the video clips among `clips` (audio clips, linked partners
/// included, are skipped). Errors: NotFound, InvalidArgument (no video clip, a clip on a locked track).
EditOutcome setScaleToFrameSize(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, bool enabled);

enum class FrameFit : std::uint8_t {
    Frame, ///< Set to Frame Size: the whole picture inside the frame
    Fill,  ///< Set to Fill Frame: the frame covered
};

/// Set to Frame Size / Set to Fill Frame on the video clips among `clips`: Motion > Scale becomes the fit (fill) factor of
/// each clip's picture for the sequence's frame (a constant: keys on Scale are replaced), and Scale to Frame Size goes
/// off. Clips whose picture size is unknown are left as they are. Errors as setScaleToFrameSize.
EditOutcome setToFrameSize(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, FrameFit fit);

/// Sets the default media scaling of media items (MediaItem::scaling): what later source edits apply to their video
/// clips. Clips already placed keep their size. Errors: NotFound.
std::expected<ProjectState, Error> setMediaScaling(const ProjectState& state, std::span<const MediaId> media,
                                                   MediaScaling scaling);

/// The content a source edit gives a new video clip of `item`'s stream `stream` in `sequence`: `base` with the item's
/// default scaling applied (ScaleToFrame sets the flag; SetToFrame / SetToFill set Motion > Scale when the picture's
/// size differs from the frame). `base` itself when nothing applies.
std::shared_ptr<const ClipContent> scaledClipContent(const MediaItem& item, StreamRef stream, const Sequence& sequence,
                                                     std::shared_ptr<const ClipContent> base);

} // namespace wabcut::edit
