#pragma once

// Colour grades and audio EQ / dynamics in FCP7 XML, both ways (0.6, card #66).
//
// What Premiere Pro really writes (checked against a Premiere export, PF): Lumetri Color is an <effect> with an EMPTY
// <name>, <effectid>Lumetri</effectid>, <pproBypass> as its switch, and numeric <parameterid>s (1 = "Blob", base64 of
// the whole Lumetri state; 7 Temperature, 8 Tint, 11 Exposure, 12 Contrast, 13 Highlights, 14 Shadows, 15 Whites,
// 16 Blacks, 20 Saturation, 25 Look, 26 Intensity, 28 Faded Film, 29 Sharpen, 30 Vibrance, 31 Saturation (Creative),
// 33 Tint Balance, 51-54 Vignette; unnamed section switches 3/23/37/46/50), each <value> in Premiere's key-list form
// (premiereKeyList). Two parameters share the name "Saturation", so Lumetri maps by id first (when the name agrees).
// Premiere does NOT write its audio effects (Parametric Equalizer, Dynamics, ...) into FCP7 XML at all.
// Hand-written and other editors' files are still recognised by effect name or id and parameter name or id (case and
// punctuation ignored):
//  * Lumetri Color -> wabcut.color.grade: Basic Correction (temperature, tint, exposure, contrast, highlights, shadows,
//    whites, blacks, saturation), Creative (intensity, faded film, vibrance, saturation, tint balance; shadow and
//    highlight tints as hue and amount), Vignette, the section switches. Its Look / Input LUT is named in the report
//    only (FCP7 XML has no LUT path); Sharpen, and curves / wheels / HSL set in the Blob, are reported.
//  * Fast Color Corrector and Three-Way Color Corrector -> the grade's saturation and its wheels (ASC CDL): input and
//    output levels as slope, offset and power; the balance wheels as lift (shadows), gamma (midtones) and gain
//    (highlights). Approximated, with the values in the report.
//  * RGB Curves -> the grade's master / red / green / blue curves, sampled into curve points.
//  * Effects Wabcut has under Premiere's own name and parameter names (ProcAmp, Levels, Color Balance, Dynamics,
//    Parametric Equalizer, Single-band and Multiband Compressor, Simple Parametric EQ, Bass, Treble, ...): exact.
//  * FCP7's 3 Band Equalizer (and Premiere's legacy EQ) -> wabcut.audio.eq3; FCP7's single-band Parametric Equalizer
//    -> wabcut.audio.parametricEq band 1; FCP7's Compressor/Limiter -> wabcut.audio.singleBandCompressor.
// Writing goes the other way (fcpFilterFor): the grade as Lumetri Color in Premiere's own shape (effectid Lumetri,
// numeric ids, key-list values), the EQ and dynamics effects under Premiere's names (for FCP7-style readers and
// Wabcut; Premiere itself has no audio filters in FCP7 XML), and every Wabcut parameter and curve again as "wabcut:"
// parameters other editors ignore, so a file Wabcut wrote comes back exactly.

#include <wabcut/model/Effects.h>
#include <wabcut/model/Parameters.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail {

/// One <parameter> of a filter as read: its value (a constant or keyframes in curve time), or its text, colour or
/// point when it is not a number.
struct FcpFilterParam {
    std::string id;
    std::string name;
    std::optional<ParamValue> value;
    std::string text;
    std::optional<std::array<double, 3>> color; ///< red, green, blue as written (0..255 or 0..1)
    std::optional<std::array<double, 2>> point; ///< horiz, vert
};

struct FcpFilter {
    std::string id;
    std::string name;
    std::string mediaType; ///< "video" / "audio" / empty
    std::vector<FcpFilterParam> params;
};

struct MappedFilter {
    EffectInstance effect;
    /// What was approximated (with the values), and what was left out; empty when exact.
    std::vector<std::string> approximated;
    std::vector<std::string> dropped;
    /// The name the report uses ("Lumetri Color").
    std::string label;
};

/// Prefix of the parameters that carry a Wabcut effect exactly beside another editor's filter.
inline constexpr std::string_view kWabcutParam = "wabcut:";
inline constexpr std::string_view kWabcutDataParam = "wabcut-data:";
inline constexpr std::string_view kWabcutEffectParam = "wabcut:effect";

/// The Wabcut effect of another editor's colour or audio filter, or nullopt when it is none of those (the FCP7
/// native filters of EffectMap.h and Wabcut's own "wabcut.*" filters are read elsewhere).
std::optional<MappedFilter> mapFcpFilter(const FcpFilter& filter);

/// A filter to write.
struct FcpWriteParam {
    std::string id;
    std::string name;
    ParamValue value;
    double scale = 1.0;           ///< written = value * scale
    std::string text;             ///< written as <value>text</value> instead (curves, LUT names, the effect id)
    bool wabcutOnly = false;      ///< a "wabcut:" parameter
    /// PF: written in Premiere's own value form (premiereKeyListText): "ticks,value,0,0,0,0,0,0" keys.
    bool premiereKeys = false;
    bool boolean = false;         ///< with premiereKeys: "true" / "false" instead of the number
};

struct FcpWriteFilter {
    std::string id;
    std::string name;             ///< may be empty (Premiere writes Lumetri with an empty <name>)
    std::string category;         ///< not written when empty
    std::string mediaType;
    bool premiereBypass = false;  ///< PF: write <pproBypass> (Premiere's on/off switch of its own effects)
    std::vector<FcpWriteParam> params;
    /// For the export report: what other editors will not get.
    std::vector<std::string> notes;
};

/// Premiere's filter for a Wabcut effect (Lumetri Color for the grade and Basic Correction, Premiere's EQ and dynamics
/// effects, the effects Wabcut has under Premiere's names), with every Wabcut parameter and curve also as a "wabcut:"
/// parameter; nullopt for effects other editors have no filter for.
std::optional<FcpWriteFilter> fcpFilterFor(const EffectInstance& effect);

/// PF: Premiere's own parameter value form in FCP7 XML (authoringApp="PremierePro"): keys separated by ';', each
/// "time,value,f2,...,f7" with the time in Premiere ticks (254016000000 per second, the same tick as Wabcut's) and the
/// value a number or true / false. Premiere writes a static value as one key at -91445760000000000 (-100 hours).
/// Read: one key, or only keys before kPremiereStaticTime / 2, -> a constant; two or more timed keys -> a curve in
/// those ticks (the clip's media time, like FCP7's <when>); nullopt when the text is not in this form.
inline constexpr std::int64_t kPremiereStaticTime = -91'445'760'000'000'000;
std::optional<ParamValue> premiereKeyList(std::string_view text);
/// Written: a constant as one key at kPremiereStaticTime; a curve as its keys at their ticks. `boolean` writes
/// true / false (non-zero / zero).
std::string premiereKeyListText(const ParamValue& value, double scale = 1.0, bool boolean = false);

/// Effect data as a parameter value: "points:x y;x y" or "text:<text>"; and back (nullopt when malformed or too long).
std::string effectDataToText(const EffectData& data);
std::optional<EffectData> effectDataFromText(std::string key, std::string_view text);

} // namespace wabcut::interchange::detail
