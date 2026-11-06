#pragma once

// Spectral Repair (0.7): the edit list of the effect wabcut.audio.spectralRepair. A clip's repairs are not baked into a
// file: they are a list of edits on the time-frequency plane of the clip's own sound, stored in the effect's data and
// rendered by the audio engine the same way in preview and export (wabcut/audio/dsp/SpectralRepair.h).
//
// An edit has a kind and a region.
//   Attenuate   changes the level of the region by `gainDb` (<= 0), with a soft edge.
//   Heal        replaces the region with what surrounds it: the levels of the nearest untouched sound before, after,
//               above and below, with a natural phase (no tonal "musical noise").
//   NoiseFloor  replaces the region with the quiet background of its surroundings.
//   Tone        removes a steady tone and its harmonics (mains hum, a whistle, an alarm), or a harmonic whose pitch
//               moves: the fundamental follows `path` (a trace of the tone over time; one point = a steady tone).
// The region of the other kinds is a Rectangle (start..end seconds x low..high Hz), a Lasso (a polygon of `path` points)
// or a Brush (the stroke `path`, drawn with a brush `radiusSeconds` x `radiusOctaves` wide).
//
// Times are SECONDS OF THE CLIP'S SOURCE (counted from the start of the media), so an edit stays on the sound it was
// made on when the clip is trimmed, moved or its speed changed; frequencies are Hz of the source sound. Nothing here is
// animated (no keyframes).
//
// Storage: the edits are text lines packed into the effect's data entries "e00".."e62" (EffectData texts of at most
// kMaxEffectText bytes), so older builds keep them untouched. The text of one edit is a few tokens separated by spaces:
//   <kind><shape><axis> gainDb amount start end low high radiusSeconds radiusOctaves harmonics widthHz n t:hz t:hz ...
// kind a/h/n/t, shape r/l/b, axis g (the lasso's straight lines are straight on a logarithmic axis) or n (linear).

#include <wabcut/model/Effects.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

inline constexpr std::string_view kSpectralRepairEffect = "wabcut.audio.spectralRepair";
/// Most edits one clip's Spectral Repair holds, and most points in a lasso, a brush stroke or a tone trace.
inline constexpr std::size_t kMaxSpectralEdits = 400;
inline constexpr std::size_t kMaxSpectralPoints = 48;
/// Range of a spectral edit's level change and the lowest/highest frequency an edit may name.
inline constexpr double kMinSpectralGainDb = -96.0;
inline constexpr double kMinSpectralHz = 1.0;
inline constexpr double kMaxSpectralHz = 192'000.0;

enum class SpectralKind : std::uint8_t { Attenuate, Heal, NoiseFloor, Tone };
enum class SpectralShape : std::uint8_t { Rectangle, Lasso, Brush };

/// "attenuate", "heal", "noiseFloor", "tone".
std::string_view toString(SpectralKind kind);
std::optional<SpectralKind> spectralKindFromString(std::string_view text);
/// "rectangle", "lasso", "brush".
std::string_view toString(SpectralShape shape);
std::optional<SpectralShape> spectralShapeFromString(std::string_view text);

struct SpectralPoint {
    double time = 0.0; ///< seconds of the source
    double hz = 0.0;
    bool operator==(const SpectralPoint&) const = default;
};

struct SpectralEdit {
    SpectralKind kind = SpectralKind::Attenuate;
    SpectralShape shape = SpectralShape::Rectangle;
    /// A lasso's edges are straight lines on a logarithmic frequency axis (true) or a linear one (false).
    bool logFrequency = true;
    /// Attenuate: the change in level, dB (kMinSpectralGainDb..0). Tone: how far the tone goes down, dB (positive depth
    /// is stored negative as well: -60 removes it to a thousandth).
    double gainDb = -24.0;
    /// Heal and NoiseFloor: how much of the repair replaces the sound, percent (1..100).
    double amount = 100.0;
    /// Rectangle: the box. Tone: the seconds the removal is on for (start..end).
    double start = 0.0;
    double end = 0.0;
    double low = 0.0;
    double high = 0.0;
    /// Lasso: the polygon. Brush: the stroke's centre line. Tone: the fundamental's trace (>= 1 point).
    std::vector<SpectralPoint> path;
    /// Brush: the half-sizes of the brush in time and in octaves.
    double radiusSeconds = 0.05;
    double radiusOctaves = 0.25;
    /// Tone: the fundamental and this many harmonics (1..16), each removed in a band `widthHz` wide.
    int harmonics = 1;
    double widthHz = 12.0;

    bool operator==(const SpectralEdit&) const = default;
};

/// The area an edit covers: seconds and Hz.
struct SpectralBounds {
    double start = 0.0;
    double end = 0.0;
    double low = 0.0;
    double high = 0.0;
};

/// The problem with an edit, if any: non-finite or out-of-range numbers, a region with no area, a lasso with fewer than
/// three points, too many points, a tone without a trace.
std::optional<std::string> spectralEditProblem(const SpectralEdit& edit);
/// A copy with every number brought into range (never returns an invalid edit for a finite input).
SpectralEdit clampedSpectralEdit(SpectralEdit edit);
/// The box around the region an edit changes (a tone: the whole of its time range and its harmonics).
SpectralBounds spectralBounds(const SpectralEdit& edit);

/// One edit as text (see the file comment), at most kMaxEffectText bytes (a long path is thinned to fit).
std::string encodeSpectralEdit(const SpectralEdit& edit);
/// The edit a text describes, or nothing when it is malformed. Numbers are read in the "C" locale.
std::optional<SpectralEdit> decodeSpectralEdit(std::string_view text);

/// The data entries that hold `edits` ("e00", "e01", ... each up to kMaxEffectText bytes, edits separated by a newline),
/// sorted by key. At most kMaxSpectralEdits edits; the rest are left out.
std::vector<EffectData> packSpectralEdits(std::span<const SpectralEdit> edits);
/// The edits stored in an instance's data, in order. Malformed lines are skipped.
std::vector<SpectralEdit> unpackSpectralEdits(const EffectInstance& instance);
/// Whether a data key is one of the edit entries.
bool isSpectralEditKey(std::string_view key);

/// Plain-language one-line summary of an edit for lists ("Attenuate -24 dB, 1.20-1.65 s, 700-3200 Hz").
std::string describeSpectralEdit(const SpectralEdit& edit);

} // namespace wabcut
