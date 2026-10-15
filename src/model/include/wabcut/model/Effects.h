#pragma once

// Effects: the per-clip effect stack (plus track and master stack placeholders) and the registry of effect
// descriptors that says what each effect id means.
//
// The model stores effects as data only: an EffectInstance names an effect by a stable string id
// ("wabcut.blur.gaussian") and version and holds its parameter values (constants or keyframe curves, keyed
// by the descriptor's parameter keys). The compositor and the audio engine implement the effects; the
// registry tells them (and the UI, commands and validation) the parameters, units, ranges, defaults and
// what the evaluator needs (EffectCapability).
//
// Unknown effects (an id the registry does not know, or a newer version than it knows) are kept exactly as
// they are and saved again unchanged; evaluators bypass them. Unknown parameter keys are kept too.
//
// Transitions use the same registry (kinds VideoTransition / AudioTransition, short ids such as
// "crossDissolve"); their parameters are constants (Transition::params).
//
// The fixed effects every clip has (Motion, Opacity, Volume, Panner) are "intrinsic" descriptors: their
// parameters live in ClipProperties (ParamId keys), never in the stack.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/Parameters.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wabcut {

/// A parameter value of an effect or transition, by the descriptor's parameter key.
struct EffectParam {
    std::string key;
    ParamValue value;

    bool operator==(const EffectParam&) const = default;
};

/// A control point of a curve-shaped effect setting (a grade's RGB and hue curves), in the effect's own units.
struct EffectPoint {
    double x = 0.0;
    double y = 0.0;

    bool operator==(const EffectPoint&) const = default;
};

/// Structured, non-keyframable data of an effect instance, by a key the effect documents: control points (a curve,
/// strictly increasing in x) or text (an asset reference such as "asset:12" for a ProjectAsset, or
/// "builtin:teal-orange"). Keys an effect does not know are kept unchanged. The grade's keys are listed with its
/// descriptor (Effects.cpp) and in docs/color.md.
struct EffectData {
    std::string key;
    std::variant<std::vector<EffectPoint>, std::string> value;

    const std::vector<EffectPoint>* points() const { return std::get_if<std::vector<EffectPoint>>(&value); }
    const std::string* text() const { return std::get_if<std::string>(&value); }
    bool operator==(const EffectData&) const = default;
};

/// Bounds on EffectData (project files are untrusted input): entries per instance, points per list, text bytes.
inline constexpr std::size_t kMaxEffectDataEntries = 64;
inline constexpr std::size_t kMaxEffectPoints = 256;
inline constexpr std::size_t kMaxEffectText = 1024;

/// The problem with one data entry, if any: an empty key, too many points, non-finite or not strictly increasing x,
/// text longer than kMaxEffectText.
std::optional<std::string> effectDataProblem(const EffectData& entry);

struct EffectInstance {
    EffectInstanceId id;
    std::string effect;        ///< stable effect id, e.g. "wabcut.blur.gaussian"
    std::int32_t version = 1;  ///< the descriptor version the parameters were written for
    bool enabled = true;
    /// Sorted by key, keys unique. A parameter not listed has the descriptor's default.
    std::vector<EffectParam> params;
    std::string mask;          ///< reserved: a mask reference; empty for none
    /// Sorted by key, keys unique, at most kMaxEffectDataEntries. Structured data (curve points, asset references);
    /// empty for most effects. A key not listed has the effect's default (an identity curve, no asset).
    std::vector<EffectData> data;
    /// Masks limiting where the effect applies (Masks.h, I.6): the effect's result is mixed into its input by the
    /// masks' coverage. Empty = everywhere. At most kMaxMasks. Written to a project only when not empty.
    std::vector<Mask> masks;

    const ParamValue* find(std::string_view key) const;
    /// Sets (or adds) a parameter, keeping `params` sorted.
    void set(std::string key, ParamValue value);
    const EffectData* findData(std::string_view key) const;
    /// Sets (or adds) a data entry, keeping `data` sorted. An empty point list or empty text removes the key.
    void setData(EffectData entry);

    bool operator==(const EffectInstance&) const = default;
};

using EffectStack = std::vector<EffectInstance>;

enum class EffectKind : std::uint8_t { Video, Audio, VideoTransition, AudioTransition };

std::string_view toString(EffectKind kind);

/// What an evaluator must provide to run an effect. Bit flags.
enum class EffectCapability : std::uint32_t {
    None = 0,
    PixelLocal = 1u << 0,     ///< output pixel depends only on the same input pixel
    Neighborhood = 1u << 1,   ///< reads pixels around the output pixel (blur, sharpen): needs edge handling
    ExtendsBounds = 1u << 2,  ///< draws outside the layer's bounds (shadow, blur): the layer must be padded
    UsesAlpha = 1u << 3,      ///< reads or writes the alpha channel
    LinearLight = 1u << 4,    ///< operates on linear-light RGB (convert from the working space first)
    TwoInputs = 1u << 5,      ///< a transition: reads the outgoing and incoming layers
    AudioStateful = 1u << 6,  ///< keeps state across samples (filters, dynamics): process in order, pre-roll
    AudioLookahead = 1u << 7, ///< needs future samples (adds latency the engine must compensate)
};

struct EffectCapabilities {
    std::uint32_t bits = 0;

    constexpr EffectCapabilities() = default;
    constexpr EffectCapabilities(EffectCapability c) : bits(static_cast<std::uint32_t>(c)) {} // NOLINT
    constexpr bool has(EffectCapability c) const { return (bits & static_cast<std::uint32_t>(c)) != 0; }
    constexpr bool operator==(const EffectCapabilities&) const = default;
};

constexpr EffectCapabilities operator|(EffectCapabilities a, EffectCapabilities b) {
    EffectCapabilities out;
    out.bits = a.bits | b.bits;
    return out;
}
constexpr EffectCapabilities operator|(EffectCapability a, EffectCapability b) {
    return EffectCapabilities(a) | EffectCapabilities(b);
}

std::vector<std::string_view> capabilityNames(EffectCapabilities capabilities);

struct EffectParamInfo {
    std::string_view key;          ///< stable within the effect id
    std::string_view displayName;
    ParamUnit unit;
    double defaultValue;
    double minValue;
    double maxValue;
    bool animatable = true;        ///< false: constants only (switches, choices)
};

struct EffectDescriptor {
    std::string_view id;           ///< stable, never reused for another effect
    std::int32_t version = 1;      ///< bumped when parameters change meaning
    std::string_view displayName;
    std::string_view category;
    EffectKind kind = EffectKind::Video;
    /// A fixed effect every clip has; its parameters are ClipProperties (ParamId keys), not a stack entry.
    bool intrinsic = false;
    std::span<const EffectParamInfo> params;
    EffectCapabilities capabilities;
    std::string_view description;

    const EffectParamInfo* param(std::string_view key) const;
};

/// C.15 effect ids: Track Matte Key and Ultra Key (chroma key; docs/internal/c15-parity-handoff.md).
inline constexpr std::string_view kTrackMatteEffect = "wabcut.key.trackMatte";
inline constexpr std::string_view kUltraKeyEffect = "wabcut.key.ultra";
/// C.15 (layouts): a stroke of `width` sequence pixels around the layer's alpha edge (after its crop and masks), in
/// `color.r/g/b` at `opacity` - a facecam's border.
inline constexpr std::string_view kBorderEffect = "wabcut.border";

/// Effect ids of audio plugins (A.6, docs/plugins.md): "plugin.vst3.<class id>", "plugin.clap.<plugin id>".
inline constexpr std::string_view kPluginEffectPrefix = "plugin.";

/// Descriptors of effects from outside the built-in catalog: the audio plugins this computer has (A.6). The host
/// installs one provider (setEffectDescriptorProvider); EffectRegistry::builtin().find() falls back to it for ids
/// that start with kPluginEffectPrefix, so validation, keyframing and the evaluators treat a scanned plugin like any
/// effect and a plugin this computer does not have like an unknown effect (kept as it is, bypassed). Every descriptor
/// a provider returns must stay valid for the life of the process; find() may be called on any thread.
class EffectDescriptorProvider {
public:
    virtual ~EffectDescriptorProvider() = default;
    virtual const EffectDescriptor* find(std::string_view id) const = 0;
};

/// Installs the provider of plugin descriptors (null: none). Earlier providers stay alive, so descriptors they
/// returned remain valid.
void setEffectDescriptorProvider(std::shared_ptr<const EffectDescriptorProvider> provider);

/// The effects and transitions Wabcut knows. Data only: implementations live in the renderer and the
/// audio engine.
class EffectRegistry {
public:
    /// The built-in catalog.
    static const EffectRegistry& builtin();

    /// The descriptor for an effect id, or null if unknown. The built-in registry also answers plugin ids, through the
    /// installed EffectDescriptorProvider.
    const EffectDescriptor* find(std::string_view id) const;
    /// All descriptors, in a stable order (intrinsic first, then by category and id).
    std::span<const EffectDescriptor> all() const { return m_descriptors; }

    explicit EffectRegistry(std::vector<EffectDescriptor> descriptors) : m_descriptors(std::move(descriptors)) {}

private:
    std::vector<EffectDescriptor> m_descriptors;
};

/// Whether an evaluator can run this instance: a known, non-intrinsic effect of the right kind whose
/// version is not newer than the registry's. Anything else is kept but bypassed.
bool isEvaluable(const EffectInstance& instance, EffectKind kind, const EffectRegistry& registry = EffectRegistry::builtin());

/// The value of an effect parameter at a curve time: the instance's value, else the descriptor default,
/// else `fallback` (unknown effect or key).
double effectParameterAt(const EffectInstance& instance, std::string_view key, Ticks curveTime, double fallback = 0.0,
                         const EffectRegistry& registry = EffectRegistry::builtin());

/// The problem with a stack, if any: duplicate or invalid instance ids, an empty effect id, an intrinsic
/// effect in the stack, unsorted or duplicate parameter keys, malformed curves, and - for known effects of a
/// version the registry knows - a wrong kind or a parameter value outside its range. Unknown effects and
/// unknown parameter keys are only checked for well-formed, finite values. Effect data must be well formed too
/// (effectDataProblem; sorted, unique keys; at most kMaxEffectDataEntries). References in data ("asset:<id>") are
/// not checked here: a dangling one is a warning (assetReferenceWarnings, ProjectState.h), never invalid state.
std::optional<std::string> effectStackProblem(const EffectStack& stack, EffectKind kind, std::uint64_t idLimit,
                                              const EffectRegistry& registry = EffectRegistry::builtin());

} // namespace wabcut
