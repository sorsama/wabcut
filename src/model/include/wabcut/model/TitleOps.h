#pragma once

// Title clips: a synthetic media item (MediaItem::generator, kind "wabcut.title") holding the text and font,
// a clip of it on a video track, and the clip's "wabcut.title.style" effect holding the numeric styling
// (size, fill/stroke colours, stroke width, shadow, alignment, line spacing, tracking), keyframable like any
// effect parameter. Position, scale and rotation are the clip's intrinsic Transform (Motion).
//
// The title item has no file: no locator path, fingerprint or proxy, nothing to relink. Its info is one still
// video stream at the sequence's canvas size and frame rate, unbounded, no audio, so its clips can be trimmed
// to any length. Several clips may use one title item (a copied clip shares it, like a Premiere legacy title):
// setTitleText / setTitleFont change every clip that shows it.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/ProjectState.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::edit {

/// The style effect every title clip gets.
inline constexpr std::string_view kTitleStyleEffect = "wabcut.title.style";

struct TitleFont {
    std::string family = "Arial";
    std::string style = "Regular";
};

/// The MediaItem of a new title (not yet in a project): generator, name (the first line of the text, else
/// "Title") and info for a canvas of width x height at `rate`.
MediaItem titleMediaItem(std::string text, const TitleFont& font, std::int32_t width, std::int32_t height, Rate rate);

/// Adds a title: a title media item, a clip of it overwriting [at, at + duration) on the video track `track`
/// (Premiere drops new graphics as an overwrite on the targeted track), and its style effect with `style`
/// (parameters of "wabcut.title.style": constants or curves; others take their defaults). One edit, one
/// ChangeSet: EditResult::createdMedia, placed (the clip) and createdEffects (the style instance). The title item
/// goes into `bin` (the root bin by default).
/// Errors: NotFound (sequence, track), InvalidArgument (audio or locked track, duration <= 0, at < 0, text
/// that is not UTF-8, empty font family, a style parameter that is unknown, out of range or not animatable).
EditOutcome addTitle(const ProjectState& state, SequenceId sequence, TrackId track, Ticks at, Ticks duration,
                     std::string text, std::vector<EffectParam> style = {}, TitleFont font = {}, BinId bin = {});

/// Replaces a title's text (every clip of the title shows it). Errors: NotFound, InvalidArgument (not a title,
/// not UTF-8, a clip of the title on a locked track). Unchanged text is a no-op.
EditOutcome setTitleText(const ProjectState& state, MediaId title, std::string text);

/// Changes a title's font family and/or style. Errors: NotFound, InvalidArgument (not a title, empty family, a
/// clip of the title on a locked track).
EditOutcome setTitleFont(const ProjectState& state, MediaId title, std::optional<std::string> family,
                         std::optional<std::string> style);

/// Whether `text` is well-formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF).
bool isValidUtf8(std::string_view text);

} // namespace wabcut::edit
