#pragma once

// Synthetic media and adjustment layers (C.15): Premiere's File > New items that need no file. Each is a generator media
// item (MediaItem::generator, GeneratorSpec::kind below) with its parameters in GeneratorSpec::params (constants by
// key, the descriptor's defaults when missing). The media engine never decodes them (isGenerator()); the compositor
// draws the picture (docs/internal/c15-parity-handoff.md) and the audio feed calls renderSyntheticAudio() for the sound.
//
//   kind                               picture                                        sound                       length
//   wabcut.synthetic.colorMatte        one colour (color.r/g/b), opaque               -                           unbounded
//   wabcut.synthetic.barsAndTone       SMPTE bars (barsPattern(): HD RP 219, SD EG 1) a sine, 1 kHz at -20 dBFS   unbounded
//   wabcut.synthetic.blackVideo        black, opaque                                  -                           unbounded
//   wabcut.synthetic.transparentVideo  nothing (alpha 0): a layer of its own for      -                           unbounded
//                                      effects that draw
//   wabcut.synthetic.countingLeader    the universal counting leader, 8 to 2          blips (the 2-pop)           8 s
//   wabcut.adjustment                  an adjustment layer: its effects apply to the  -                           unbounded
//                                      composite of the tracks below (CompositePlan, Resolve.h)
//
// Unbounded items are still-image-like (MediaInfo::isStillImage): clips of them can be any length. Every item's video
// stream has the frame size and rate it was made for (the sequence's, like Premiere's New Item dialog), alpha straight.
// Bars and Tone and the counting leader (P.2) also have one stereo stream at the sequence's audio rate.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <array>
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
inline constexpr std::string_view kBarsAndToneGenerator = "wabcut.synthetic.barsAndTone";       ///< P.2
inline constexpr std::string_view kCountingLeaderGenerator = "wabcut.synthetic.countingLeader"; ///< P.2
/// GeneratorSpec::kind of adjustment layers.
inline constexpr std::string_view kAdjustmentGenerator = "wabcut.adjustment";

enum class SyntheticKind : std::uint8_t { ColorMatte, BlackVideo, TransparentVideo, BarsAndTone, CountingLeader };

struct SyntheticDescriptor {
    SyntheticKind kind;
    std::string_view generator;   ///< GeneratorSpec::kind
    std::string_view shortName;   ///< "colorMatte", as the commands take it
    std::string_view displayName; ///< Premiere's menu name, also the new item's default name
    std::span<const EffectParamInfo> params;
    bool hasAudio = false;        ///< one stereo stream at the format's audio rate
    std::string_view description;
};

/// Every synthetic kind, in Premiere's New Item menu order.
std::span<const SyntheticDescriptor> syntheticKinds();
/// By GeneratorSpec::kind or short name; null when unknown.
const SyntheticDescriptor* findSynthetic(std::string_view kindOrShortName);
/// The synthetic descriptor of a media item, or null (not generated, or another generator).
const SyntheticDescriptor* syntheticOf(const MediaItem& item);
bool isAdjustmentLayer(const MediaItem& item);

/// The length of the universal counting leader: 8 seconds (8 to 3, one second each, the 2 for one frame, then black
/// to picture start).
inline constexpr Ticks kCountingLeaderLength{8 * kTicksPerSecond};

/// The picture and sound format a new item is made for (the sequence's settings).
struct SyntheticFormat {
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    Rate frameRate = kRate30;
    Rate audioRate = kRate48kHz;
};

/// A new synthetic item of `kind` (not yet in a project: add it with addMedia). `params` are checked against the
/// descriptor: known keys, finite, constant, in range (InvalidArgument otherwise); missing keys take the defaults (only
/// the given ones are stored). `name` empty: the descriptor's display name. Format: sizes 1..16384, positive rates.
std::expected<MediaItem, Error> syntheticMediaItem(SyntheticKind kind, std::vector<EffectParam> params,
                                                   const SyntheticFormat& format, std::string name = {});
/// A new adjustment layer item ("Adjustment Layer") of that frame size and rate.
std::expected<MediaItem, Error> adjustmentMediaItem(const SyntheticFormat& format, std::string name = {});

/// A synthetic item's parameter: its stored value, else the descriptor's default (`fallback` for an unknown key).
double syntheticParam(const MediaItem& item, std::string_view key, double fallback = 0.0);

/// Why a generator item of a kind above is malformed, if it is (validate): the streams and length of its kind,
/// parameters known, constant, finite and in range, sorted and unique. Null for other items.
std::optional<std::string> syntheticProblem(const MediaItem& item);

// ---- What the render and audio owners draw and play (P.2) ---------------------------------------------------------

/// One rectangle of a bars pattern, in fractions of the frame (x right, y down). A `ramp` patch goes from `rgb` at x0 to
/// `rgbEnd` at x1. Colours are video levels in the sequence's (Rec. 709) encoding: 1.0 = 100 %, 0.75 = 75 %; the pluge
/// patches below black (-2 %, -4 %) clip in full-range RGB.
struct BarsPatch {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    std::array<double, 3> rgb{};
    bool ramp = false;
    std::array<double, 3> rgbEnd{};

    bool operator==(const BarsPatch&) const = default;
};

/// The bars of a Bars and Tone item: "pattern" 0 = SMPTE RP 219 HD (16:9 with side panels, Premiere's HD Bars and
/// Tone), 1 = SMPTE EG 1 SD (Premiere's Bars and Tone; the 4:3 layout stretched to the frame). The patches tile the
/// frame without gaps or overlaps.
std::vector<BarsPatch> barsPattern(int pattern);

/// The counting leader at media time `t` (0 <= t < kCountingLeaderLength): the number shown (8..2; 0 = black) and how
/// far the clock wipe has swept through the current second (0..1, clockwise from 12 o'clock).
struct LeaderFrame {
    int number = 0;
    double sweep = 0.0;

    bool operator==(const LeaderFrame&) const = default;
};
LeaderFrame countingLeaderAt(const MediaItem& item, Ticks t);

/// The sound of a synthetic item: interleaved samples of its stereo stream from media time `sourceTime` at
/// `sampleRate`, `channels` per frame (every channel gets the same signal). Bars and Tone: a sine of "tone.frequency"
/// Hz at "tone.level" dBFS, phase-continuous across calls. Counting leader: 1 kHz blips at -20 dBFS one frame long, at
/// the "2" (Cue Blip on 2), at every second (Cue Blip at All Second Starts) and on its last frame (Cue Blip on Out).
/// Others: silence. Returns the frames written (all of them).
std::size_t renderSyntheticAudio(const MediaItem& item, Ticks sourceTime, std::span<float> interleaved, int channels,
                                 Rate sampleRate);

namespace edit {

/// File > New > (synthetic item): a new item of `kind` with `params` (syntheticMediaItem) made for `sequence`'s frame
/// size, rate and audio rate, into `bin` (invalid: the root). Errors: NotFound (sequence, bin), InvalidArgument (params).
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
