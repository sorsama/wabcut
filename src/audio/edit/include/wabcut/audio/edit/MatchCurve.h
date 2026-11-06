#pragma once

// Reference matching, the deriving half (0.7, EQ Match and Level Match; docs/audio.md "Match"): what is made of the
// measurements of a reference and a target (dsp/Match.h measures them), and the Loudness Radar's folding of the short-term
// loudness history. Pure, no host services.
//
//   fitEqMatch      the correction that turns the target's spectrum into the reference's: the difference of the two in
//                   the 24 bands from 78.7 Hz to 16 kHz, the overall level difference removed (EQ Match changes the
//                   tone; Level Match changes the level), smoothed, limited to +-18 dB and scaled by the amount. The
//                   Graphic Equalizer (30 Bands) realises the curve as a cascade of minimum-phase peaking filters whose
//                   response at the band centres is the curve (FilterEffects.cpp GraphicEqProcessor).
//   foldProgramme   the radar of a whole programme; foldLive the radar of the last lap of a live measurement.
//   loudnessBand    the spread of the short-term loudness (EBU Tech 3342 on the samples).

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace wabcut::audio::edit {

inline constexpr int kMatchBandCount = 30;   ///< the Graphic Equalizer (30 Bands) sliders (dsp::kSpectrumBands)
inline constexpr int kMatchFirstBand = 4;    ///< the first band that is matched (0-based): 78.7 Hz
inline constexpr int kMatchBands = 24;       ///< bands matched: 78.7 Hz .. 16 kHz
inline constexpr double kMatchMaxGainDb = 18.0;
inline constexpr double kMatchMinActiveSeconds = 1.0; ///< less audible audio than this cannot be matched

/// Centre of band `band` (0-based): 31.25 Hz x 2^(band / 3).
double matchBandHz(int band) noexcept;

/// The average spectrum of a reference or a target: power per bin, dB, per band (NaN above the usable range).
struct SpectrumProfile {
    std::array<double, kMatchBandCount> levelDb;
    double seconds = 0.0;       ///< the signal's length
    double activeSeconds = 0.0; ///< of it, the frames that counted (not pauses)
    int sampleRate = 0;

    SpectrumProfile() { levelDb.fill(std::numeric_limits<double>::quiet_NaN()); }
    /// Enough audible audio to match from.
    bool usable() const noexcept { return activeSeconds >= kMatchMinActiveSeconds; }
    bool operator==(const SpectrumProfile&) const = default;
};

struct EqMatchOptions {
    double amount = 1.0;      ///< 0..1: how much of the correction is applied
    double smoothing = 0.5;   ///< 0..1: 0 follows the measurement band by band, 1 is a gentle curve
    double maxGainDb = kMatchMaxGainDb; ///< the limit of a band's gain, 0..18
};

struct EqMatchCurve {
    bool valid = false;
    /// The Graphic Equalizer (30 Bands) slider of each band, dB. The bands beside the matched ones follow the edge band's
    /// gain, fading to nothing: at half its value per band below, at 0.9 per band above, so that the curve ends gently.
    std::array<double, kMatchBandCount> gainDb{};
    /// reference - target in the matched bands, before the level offset, smoothing and limits (NaN outside).
    std::array<double, kMatchBandCount> rawDb;
    double levelOffsetDb = 0.0; ///< the mean difference that was removed: the reference is this much louder in these bands

    EqMatchCurve() { rawDb.fill(std::numeric_limits<double>::quiet_NaN()); }
};

/// The correction from `target` to `reference`. Invalid (valid false) when either profile is not usable or too few
/// bands have values in both.
EqMatchCurve fitEqMatch(const SpectrumProfile& reference, const SpectrumProfile& target, const EqMatchOptions& options = {});

// ---- The Loudness Radar ------------------------------------------------------------------------------------------------

/// One revolution of the radar: `values` are the short-term loudness (LUFS) of each slot in time order, NaN where
/// nothing was measured.
struct RadarFrame {
    std::vector<float> values;
    double secondsPerSlot = 1.0;
    int head = -1; ///< live: the slot the sweep is in; -1 for a whole programme
};

/// The most slots a revolution has (one degree each).
inline constexpr int kRadarMaxSlots = 360;

/// A whole programme in one revolution: `shortTerm` holds one short-term loudness per `periodSeconds` (-inf where there was
/// no measurement); the slots average the energies of the samples they cover. At most `slots` (<= kRadarMaxSlots).
RadarFrame foldProgramme(std::span<const float> shortTerm, double periodSeconds, int slots = kRadarMaxSlots);
/// The last lap of a live measurement: `shortTerm` is one value per second since the measurement started, `lapSeconds`
/// the length of a revolution. The slot of each sample is its time modulo the lap, newer laps replacing older ones; the
/// head is the slot of the newest sample. Slots not reached yet in the first lap are NaN.
RadarFrame foldLive(std::span<const float> shortTerm, int lapSeconds, int maxSlots = kRadarMaxSlots);

/// The spread of the short-term loudness of a programme (EBU Tech 3342 on the samples: gated at -70 LUFS absolute and 20 LU
/// under the energy mean, then the 10th to the 95th percentile): the low and high ends in LUFS, and their difference.
struct LoudnessBand {
    bool valid = false;
    double lowLufs = 0.0;
    double highLufs = 0.0;
    double rangeLu() const noexcept { return highLufs - lowLufs; }
};
LoudnessBand loudnessBand(std::span<const float> shortTerm);

} // namespace wabcut::audio::edit
