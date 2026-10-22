#pragma once

// 0.2: interlaced footage. Premiere's Clip > Video Options > Field Options (Reverse Field Dominance and the
// Processing Options None / Interlace Consecutive Frames / Always Deinterlace / Flicker Removal) and the automatic
// deinterlacing of interlaced footage in a progressive sequence.
//
// Wabcut composites progressive frames. Where a clip shows an interlaced picture (its stream's field order, or the
// one Interpret Footage gives the item), the layer is deinterlaced the way Premiere does it: one field is kept and
// the other's lines are interpolated from it (render's LayerFieldMode, the layer shader and render::referencePixel).
//   * A progressive sequence deinterlaces interlaced footage on its own. When the sequence plays at the field rate or
//     faster (29.97i footage in a 59.94p sequence), the first half of each frame shows its first field and the second
//     half its second field, so both fields' motion is seen; slower sequences keep the first (dominant) field.
//   * Always Deinterlace deinterlaces in interlaced sequences too, always from the dominant field.
//   * Reverse Field Dominance swaps the clip's field order.
//   * Flicker Removal blends the two fields slightly (a vertical [1 2 1] / 4 filter) instead of dropping one.
//   * Interlace Consecutive Frames (progressive footage in an interlaced sequence) is kept for interchange: Wabcut
//     sequences render progressive frames, so it shows the frames as they are.

#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace wabcut {

struct Clip;
struct MediaItem;
struct Sequence;

/// Premiere's Field Options > Processing Options.
enum class FieldProcessing : std::uint8_t { None, InterlaceConsecutiveFrames, AlwaysDeinterlace, FlickerRemoval };

/// "none", "interlaceConsecutiveFrames", "alwaysDeinterlace", "flickerRemoval".
std::string_view toString(FieldProcessing processing);
std::optional<FieldProcessing> fieldProcessingFromString(std::string_view text);
/// Premiere's labels: "None", "Interlace Consecutive Frames", "Always Deinterlace", "Flicker Removal".
std::string_view displayName(FieldProcessing processing);

/// A video clip's Field Options (ClipContent::fieldOptions). Saved only when not the default (schema 18).
struct ClipFieldOptions {
    bool reverseFieldDominance = false;
    FieldProcessing processing = FieldProcessing::None;

    bool isDefault() const { return !reverseFieldDominance && processing == FieldProcessing::None; }
    bool operator==(const ClipFieldOptions&) const = default;
};

/// How a layer shows its frame's lines (render::CompositeLayer::fieldMode reads the same values).
enum class FieldRender : std::uint8_t {
    Progressive = 0, ///< the frame as it is
    KeepUpper = 1,   ///< the upper field's lines (even rows, counting from 0 at the top); the others interpolated
    KeepLower = 2,   ///< the lower field's lines (odd rows); the others interpolated
    Blend = 3,       ///< Flicker Removal: every line blended with its neighbours ([1 2 1] / 4)
};

/// The field order `item`'s stream `streamIndex` (an index into item.info.video) is shown with: Interpret Footage's
/// when set, else the file's. Progressive for stills, generated items and unknown streams.
FieldOrder effectiveFieldOrder(const MediaItem& item, int streamIndex);

/// How a clip shows its frames' lines (see the file comment): `first` in the first half of each frame, `second` in the
/// second half. They differ only when both fields are seen (the sequence runs at the field rate or faster).
struct FieldRendering {
    FieldRender first = FieldRender::Progressive;
    FieldRender second = FieldRender::Progressive;

    bool isProgressive() const { return first == FieldRender::Progressive && second == FieldRender::Progressive; }
    bool operator==(const FieldRendering&) const = default;
};

/// How `clip` shows its frames in `sequence`: a media clip of `item` (its stream `streamIndex`), or with `item` null a
/// nested or multicam clip (its canvas is progressive: only Always Deinterlace and Flicker Removal apply).
FieldRendering fieldRenderingOf(const Clip& clip, const MediaItem* item, int streamIndex, const Sequence& sequence);

/// The field the frame [framePts, framePts + frameDuration) shows at source time `sourceTime`.
FieldRender fieldRenderAt(const FieldRendering& rendering, Ticks sourceTime, Ticks framePts, Ticks frameDuration);

} // namespace wabcut
