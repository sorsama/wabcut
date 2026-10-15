#pragma once

// Layout presets (C.15, gaming): facecam picture-in-picture and vertical 9:16 gaming-shorts layouts in one edit. A layout
// is not a new kind of object: it sets plain, editable clip properties and effects - Motion (scale, position, anchor),
// Crop, a rounded-rectangle or ellipse mask (wabcut.mask), a border (wabcut.border), a drop shadow, a Gaussian blur - and,
// where it needs a second copy of the gameplay (a blurred background, a facecam cut from the gameplay picture), places a
// copy of the gameplay clip on a free video track above it. Everything is one undo step and stays editable afterwards.
//
//   PictureInPicture   gameplay fills the frame; the facecam sits in a corner: `size` of the frame width, `margin` from
//                      the edges, rounded corners (or a circle), a border and a drop shadow.
//   VerticalSplit      (made for 9:16) gameplay fills the top `split` of the frame, the facecam the rest (or the other way
//                      round with `facecamOnTop`), each centre-cropped to its part.
//   VerticalBlurFill   (made for 9:16) a blurred copy of the gameplay fills the frame behind the gameplay fitted to the
//                      frame's width (`zoom` > 1 crops its sides); an optional facecam goes on top like PictureInPicture.
//
// The facecam is either a clip (a webcam recording on a video track above the gameplay's) or `facecamRegion`: the part of
// the gameplay picture where the capture already shows the webcam (a copy of the gameplay clip cropped to it). Effects a
// layout adds carry the data key "layout"; applying a layout again to the same clips replaces them instead of stacking.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

enum class LayoutPreset : std::uint8_t { PictureInPicture, VerticalSplit, VerticalBlurFill };
enum class LayoutCorner : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

std::string_view toString(LayoutPreset preset);
std::optional<LayoutPreset> layoutPresetFromString(std::string_view text);
std::string_view toString(LayoutCorner corner);
std::optional<LayoutCorner> layoutCornerFromString(std::string_view text);

/// EffectData key that marks the effects a layout added (its value: "layout").
inline constexpr std::string_view kLayoutDataKey = "layout";

/// A rectangle in fractions of a frame (x right, y down).
struct NormalizedRect {
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;

    bool operator==(const NormalizedRect&) const = default;
};

struct LayoutOptions {
    // The facecam box (PictureInPicture, and the facecam of VerticalBlurFill).
    LayoutCorner corner = LayoutCorner::BottomRight;
    double size = 0.28;          ///< the facecam's width as a fraction of the frame width, 0.05..1
    double margin = 0.03;        ///< gap to the frame's edges, a fraction of the frame height, 0..0.4
    bool circle = false;         ///< a round facecam (the centre square of the facecam source, an ellipse mask)
    double cornerRadius = 0.12;  ///< rounded corners: radius as a fraction of the box's shorter side, 0..0.5
    double borderWidth = 4.0;    ///< sequence pixels, 0..100 (0 = no border)
    std::array<double, 3> borderColor{1.0, 1.0, 1.0};
    bool shadow = true;          ///< a soft drop shadow under the facecam
    // VerticalSplit.
    double split = 0.6;          ///< the gameplay's share of the frame height, 0.2..0.8
    bool facecamOnTop = false;
    // VerticalBlurFill.
    double blur = 40.0;          ///< the background's blur radius, sequence pixels, 0..500
    double zoom = 1.0;           ///< the foreground's scale over fit-to-width, 1..4 (above 1 crops its sides)
    /// Where the capture shows the webcam, as a part of the gameplay frame. Set: the facecam is a copy of the gameplay
    /// cropped to it (no facecam clip needed).
    std::optional<NormalizedRect> facecamRegion;

    bool operator==(const LayoutOptions&) const = default;
};

/// Why options are out of range, if they are.
std::optional<std::string> layoutOptionsProblem(const LayoutOptions& options);

struct LayoutRequest {
    LayoutPreset preset = LayoutPreset::PictureInPicture;
    ClipId gameplay;  ///< a video clip of the sequence
    ClipId facecam;   ///< a video clip on a track above the gameplay's; invalid: none (or options.facecamRegion)
    LayoutOptions options;
};

struct LayoutResult {
    edit::EditResult edit;
    ClipId gameplay;   ///< the gameplay as laid out (the blurred background for VerticalBlurFill)
    ClipId foreground; ///< VerticalBlurFill: the sharp gameplay copy on top; else invalid
    ClipId facecam;    ///< the facecam clip (the given one, or the copy cut from the gameplay); invalid when none
};

/// Where a source region lands: Motion and Crop values that show `region` of a source picture of `sourceWidth` x
/// `sourceHeight` display pixels centre-cropped to fill `target` (pixels of a canvas of `canvasWidth` x `canvasHeight`).
struct RegionPlacement {
    double scale = 1.0;
    double positionX = 0.0, positionY = 0.0; ///< Motion position: anchor offset from the canvas centre (sequence px)
    double anchorX = 0.0, anchorY = 0.0;     ///< Motion anchor: offset from the source frame centre (source px)
    NormalizedRect shown;                    ///< the part of the source shown (region trimmed to the target's aspect)
};
RegionPlacement placeRegion(double sourceWidth, double sourceHeight, const NormalizedRect& region, const NormalizedRect& target,
                            double canvasWidth, double canvasHeight);

namespace edit {

/// Applies a layout (see the file comment). Errors: NotFound (clips), InvalidArgument (options out of range, a clip that
/// is not video, no picture size, a facecam below the gameplay or not overlapping it, a locked track, a preset that needs
/// a facecam without one).
std::expected<LayoutResult, Error> applyLayout(const ProjectState& state, SequenceId sequence, const LayoutRequest& request);

} // namespace edit

} // namespace wabcut
