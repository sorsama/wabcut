#pragma once

// Synthetic media and adjustment layers (C.15): Premiere's File > New items that need no file. Each is a generator media
// item (MediaItem::generator, GeneratorSpec::kind below) with its parameters in GeneratorSpec::params (constants by
// key, the descriptor's defaults when missing). The media engine never decodes them (isGenerator()); the compositor
// draws the picture (docs/internal/c15-parity-handoff.md).
//
//   kind                               picture
//   wabcut.synthetic.colorMatte        one colour (color.r/g/b), opaque
//   wabcut.synthetic.blackVideo        black, opaque
//   wabcut.synthetic.transparentVideo  nothing (alpha 0): a layer of its own for effects that draw
//   wabcut.adjustment                  an adjustment layer: its effects apply to the composite of the tracks below
//                                      (CompositePlan, Resolve.h)
//
// Every item has one unbounded video stream (still-image-like: clips of it can be any length) of the frame size and rate
// it was made for (the sequence's, like Premiere's New Item dialog), alpha straight, and no sound.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

inline constexpr std::string_view kColorMatteGenerator = "wabcut.synthetic.colorMatte";
inline constexpr std::string_view kBlackVideoGenerator = "wabcut.synthetic.blackVideo";
inline constexpr std::string_view kTransparentVideoGenerator = "wabcut.synthetic.transparentVideo";
/// GeneratorSpec::kind of adjustment layers.
inline constexpr std::string_view kAdjustmentGenerator = "wabcut.adjustment";

enum class SyntheticKind : std::uint8_t { ColorMatte, BlackVideo, TransparentVideo };

struct SyntheticDescriptor {
    SyntheticKind kind;
    std::string_view generator;   ///< GeneratorSpec::kind
    std::string_view shortName;   ///< "colorMatte", as the commands take it
    std::string_view displayName; ///< Premiere's menu name, also the new item's default name
    std::span<const EffectParamInfo> params;
    std::string_view description;
};

/// Every synthetic kind, in Premiere's New Item menu order.
std::span<const SyntheticDescriptor> syntheticKinds();
/// By GeneratorSpec::kind or short name; null when unknown.
const SyntheticDescriptor* findSynthetic(std::string_view kindOrShortName);
/// The synthetic descriptor of a media item, or null (not generated, or another generator).
const SyntheticDescriptor* syntheticOf(const MediaItem& item);
bool isAdjustmentLayer(const MediaItem& item);

/// The picture format a new item is made for (the sequence's settings).
struct SyntheticFormat {
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    Rate frameRate = kRate30;
};

/// A new synthetic item of `kind` (not yet in a project: add it with addMedia). `params` are checked against the
/// descriptor: known keys, finite, constant, in range (InvalidArgument otherwise); missing keys take the defaults (only
/// the given ones are stored). `name` empty: the descriptor's display name. Format: sizes 1..16384, a positive rate.
std::expected<MediaItem, Error> syntheticMediaItem(SyntheticKind kind, std::vector<EffectParam> params,
                                                   const SyntheticFormat& format, std::string name = {});
/// A new adjustment layer item ("Adjustment Layer") of that frame size and rate.
std::expected<MediaItem, Error> adjustmentMediaItem(const SyntheticFormat& format, std::string name = {});

/// A synthetic item's parameter: its stored value, else the descriptor's default (`fallback` for an unknown key).
double syntheticParam(const MediaItem& item, std::string_view key, double fallback = 0.0);

/// Why a generator item of a kind above is malformed, if it is (validate): one unbounded video stream and no sound,
/// parameters known, constant, finite and in range, sorted and unique. Null for other items.
std::optional<std::string> syntheticProblem(const MediaItem& item);

namespace edit {

/// File > New > (synthetic item): a new item of `kind` with `params` (syntheticMediaItem) made for `sequence`'s frame
/// size and rate, into `bin` (invalid: the root). Errors: NotFound (sequence, bin), InvalidArgument (params).
std::expected<Created<MediaId>, Error> addSyntheticMedia(const ProjectState& state, SyntheticKind kind,
                                                         std::vector<EffectParam> params, SequenceId sequence,
                                                         BinId bin = {}, std::string name = {});

/// Places an adjustment layer clip on video track `track` over [at, at + duration) (an overwrite edit): of `media`, an
/// existing adjustment layer item, or, when `media` is invalid, of a new "Adjustment Layer" item made for the sequence
/// (into `bin`; EditResult::createdMedia names it). Errors: NotFound, InvalidArgument (not a video track, locked, not
/// an adjustment layer item, empty range or a start before 0).
EditOutcome addAdjustmentLayer(const ProjectState& state, SequenceId sequence, TrackId track, Ticks at, Ticks duration,
                               MediaId media = {}, BinId bin = {});

} // namespace edit

} // namespace wabcut
