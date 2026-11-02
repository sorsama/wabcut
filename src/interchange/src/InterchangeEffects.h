#pragma once

// Generators and effects in FCPXML and OpenTimelineIO (0.6, cards #67 and #69), shared by both formats' readers and
// writers (FCP7 XML has its own filter table, EffectMap.h, and uses the generator helpers below).
//
// Generators: Wabcut's synthetic items (Synthetic.h) and adjustment layers travel as IrMedia::generator. Each format
// writes them as its own generator where it has one (an FCPXML solid, an OTIO SolidColor / SMPTEBars reference, an
// FCP7 Color / Slug / Bars and Tone generator) and adds Wabcut's kind and parameters privately, so a Wabcut round trip
// is exact and other editors show the closest thing they have.
//
// Effects: a Wabcut effect with a well-known equivalent (Gaussian Blur, Levels, Proc Amp, Color Balance, Channel EQ,
// Compressor, Limiter, ...) is written under that name with the settings both sides share; every Wabcut key is also
// written privately so Wabcut reads the instance back exactly. A foreign effect Wabcut does not know is kept as a
// disabled placeholder (an unknown effect id "fcpxml:<name>" / "otio:<name>") whose data holds the source's name, id
// and parameter values, so an export writes it back as it came; Wabcut does not render it.

#include "Ir.h"

#include <wabcut/model/Effects.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail {

// ---- generators (card #69) -------------------------------------------------------------------------------------------

/// FCPXML effect uid prefix of Wabcut generators ("com.wabcut.generator/wabcut.synthetic.countingLeader").
inline constexpr std::string_view kWabcutGeneratorUid = "com.wabcut.generator";

/// Premiere's name for a generator kind ("Bars and Tone", "Adjustment Layer"); empty when Wabcut cannot make it.
std::string_view generatorDisplayName(std::string_view kind);
/// The parameters a generator of `kind` keeps: known keys, finite constants inside their range, sorted by key. The
/// names of the others are appended to `dropped`.
std::vector<EffectParam> cleanGeneratorParams(std::string_view kind, std::vector<EffectParam> params,
                                              std::vector<std::string>& dropped);
/// A generator's parameter: its value, else the descriptor's default.
double generatorParam(const IrGenerator& generator, std::string_view key);
/// A MediaTable key: generators of the same kind and parameters share one media item.
std::string generatorKey(const IrGenerator& generator);
/// IrMedia for a generator (key, name, video / audio streams of its kind).
IrMedia generatorMedia(IrGenerator generator, std::string name);
/// Whether the kind has a sound (Bars and Tone, the counting leader).
bool generatorHasAudio(std::string_view kind);

// ---- effects (card #67) ----------------------------------------------------------------------------------------------

/// FCPXML effect uid prefix of Wabcut effects without a Final Cut equivalent ("com.wabcut.effect/wabcut.color.basic").
inline constexpr std::string_view kWabcutEffectUid = "com.wabcut.effect";
/// Placeholder effect ids: "fcpxml:<effect name>", "otio:<effect_name>".
inline constexpr std::string_view kFcpxPlaceholder = "fcpxml:";
inline constexpr std::string_view kOtioPlaceholder = "otio:";
/// Data keys of a foreign effect's origin (placeholders, and mapped effects so an export writes the same effect back).
inline constexpr std::string_view kSourceName = "source.name";
inline constexpr std::string_view kSourceUid = "source.uid";
inline constexpr std::string_view kSourceEnabled = "source.enabled";
inline constexpr std::string_view kSourceMetadata = "source.metadata";
/// Data key prefix of a foreign parameter value kept as text ("param:Amount").
inline constexpr std::string_view kSourceParam = "param:";

/// A setting both sides share: Wabcut key, the other editor's parameter name, its value = Wabcut value * scale.
struct ForeignParam {
    std::string_view key;
    std::string_view name;
    double scale = 1.0;
};

struct ForeignEffect {
    std::string_view wabcut;
    std::string_view name;    ///< the name written (FCPXML effect name, OTIO effect_name)
    std::string_view fcpxUid; ///< the FCPXML effect uid written
    std::span<const std::string_view> aliases; ///< other names editors use (read, case-insensitive)
    std::span<const ForeignParam> params;
};

/// The well-known equivalent of a Wabcut effect, or null.
const ForeignEffect* foreignEffectFor(std::string_view wabcutId);
/// The Wabcut effect of `kind` an editor's effect is: by uid, name or alias, then by a Wabcut display name
/// ("Gaussian Blur", "Brightness & Contrast"). Empty when none.
std::string wabcutEffectNamed(std::string_view name, std::string_view uid, EffectKind kind);

/// Whether an instance is a foreign placeholder ("fcpxml:...", "otio:...").
bool isForeignPlaceholder(const EffectInstance& e);
/// What reports call an effect: its display name, else its id.
std::string effectLabel(const EffectInstance& e);
/// Text data of an instance, or empty.
std::string dataText(const EffectInstance& e, std::string_view key);
/// Adds a text data entry when it fits (kMaxEffectDataEntries, kMaxEffectText); false when it does not.
bool addDataText(EffectInstance& e, std::string key, std::string text);

/// A number as the formats write it: integers without a point, others with up to 10 significant digits.
std::string numberText(double v);

} // namespace wabcut::interchange::detail
