#pragma once

// Effect presets and the user's Effects panel items (0.2, docs/effects.md "Presets, custom bins and favourites"):
// Premiere's Save Preset, the Effects panel's Presets bin (its own presets and the user's, in presets bins), custom
// bins and favourites.
//
// A preset is effects saved from a clip - stack effects with their parameters, keyframes, data and masks, and the
// clip's fixed effects (Motion, Opacity with its blend mode, Volume, Panner) - and how its keyframes land on the clip it
// is applied to, Premiere's preset types:
//   Scale               the keyframes keep their place relative to the clip's length (75 % in stays 75 % in);
//   Anchor to In Point  they keep their distance from the clip's In point (no scaling);
//   Anchor to Out Point they keep their distance from the clip's Out point.
// A preset's keyframe times are timeline ticks from the In point of the clip it was saved from (`duration` long, at
// 100 %); applying maps them into the target clip's curve time (Parameters.h), at its speed and direction.
//
// Fixed effects are entries with the intrinsic ids (kMotionEffect...) whose parameters are the clip parameters' keys
// ("motion.scale", Parameters.h); Opacity's blend mode is its data text "blendMode" (Blend.h names). Applying a preset
// replaces those parameters of the target (keyframes included, as Premiere does) and adds the stack effects after the
// target's own. 0.2: an anchored preset (Anchor to In / Out Point) with keys replaces only the target's keys inside the
// preset's keyed time, so an In and an Out animation (the built-in Animations bin) both stay on one clip.
//
// Data that names something of one project is made portable when a preset is saved: a LUT asset reference
// ("asset:<id>") becomes "file:<absolute path>", which applying turns back into an asset of the target project (the
// one with that path, or a new one); masks that follow a tracked matte (MaskShape::Matte) are left out.
//
// The library is the user's, not the project's (Premiere keeps presets with the user's profile): the host stores it
// (command::EffectLibraryServices: a JSON file in the app's data folder). Library files and preset files are untrusted
// input: read bounded and checked (effectPresetProblem, effectsLibraryProblem).

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

/// Premiere's preset types (the Save Preset dialog).
enum class EffectPresetType : std::uint8_t { Scale, AnchorToInPoint, AnchorToOutPoint };

/// "scale", "anchorToInPoint", "anchorToOutPoint".
std::string_view toString(EffectPresetType type);
std::optional<EffectPresetType> effectPresetTypeFromString(std::string_view text);
/// "Scale", "Anchor to In Point", "Anchor to Out Point".
std::string_view displayName(EffectPresetType type);

/// The fixed effects every clip has (Effects.cpp's intrinsic descriptors).
inline constexpr std::string_view kMotionEffect = "wabcut.motion";
inline constexpr std::string_view kOpacityEffect = "wabcut.opacity";
inline constexpr std::string_view kVolumeEffect = "wabcut.volume";
inline constexpr std::string_view kPannerEffect = "wabcut.panner";
/// The data key of Opacity's blend mode in a preset.
inline constexpr std::string_view kPresetBlendModeKey = "blendMode";
/// A preset's portable reference to a file (a LUT): "file:<absolute UTF-8 path>".
inline constexpr std::string_view kPresetFilePrefix = "file:";

// ---- Bounds (library and preset files are untrusted input) -------------------------------------------------------------
inline constexpr std::size_t kMaxEffectPresets = 1000;      ///< the user's presets
inline constexpr std::size_t kMaxPresetEffects = 64;        ///< effects in one preset
inline constexpr std::size_t kMaxPresetNameBytes = 256;     ///< preset, bin and custom bin names
inline constexpr std::size_t kMaxPresetDescriptionBytes = 4096;
inline constexpr std::size_t kMaxPresetBins = 256;
inline constexpr std::size_t kMaxPresetBinDepth = 8;        ///< presets bins inside presets bins
inline constexpr std::size_t kMaxCustomBins = 128;
inline constexpr std::size_t kMaxCustomBinItems = 1000;
inline constexpr std::size_t kMaxFavorites = 1000;
inline constexpr std::size_t kMaxEffectsLibraryBytes = 16 * 1024 * 1024;

struct EffectPreset {
    std::string name;        ///< unique in the library (case-insensitive), not a built-in's name
    std::string description;
    EffectPresetType type = EffectPresetType::Scale;
    /// The presets bin it is in, below the Presets bin: "" for the Presets bin itself, "Lower thirds/Blue" for a bin
    /// inside a bin ('/' separates; segments are names).
    std::string bin;
    /// Length of the clip it was saved from (timeline ticks, > 0): Scale and Anchor to Out Point measure from it.
    Ticks duration{};
    /// In stack order: fixed effects (kMotionEffect...) and stack effects. Ids 1..n (unique; applying gives new ones).
    std::vector<EffectInstance> effects;

    /// Video or audio, by its effects (a preset holds one kind).
    EffectKind kind() const;
    bool operator==(const EffectPreset&) const = default;
};

/// What an Effects panel item refers to, in custom bins and favourites: "effect:<id>" (an effect or a transition of the
/// registry, also a plug-in's) or "preset:<name>" (a built-in or user preset).
inline constexpr std::string_view kEffectItemPrefix = "effect:";
inline constexpr std::string_view kPresetItemPrefix = "preset:";

/// Premiere's custom bin (Effects panel > New Custom Bin): shortcuts to effects, transitions and presets.
struct EffectsCustomBin {
    std::string name; ///< unique among custom bins (case-insensitive)
    std::vector<std::string> items; ///< item references, no duplicates, in the order they were added

    bool operator==(const EffectsCustomBin&) const = default;
};

/// The user's Effects panel items.
struct EffectsLibrary {
    std::vector<EffectPreset> presets; ///< sorted by name (case-insensitive)
    /// Presets bins the user made ("A", "A/B"), sorted; a preset's bin need not be listed (it shows anyway).
    std::vector<std::string> presetBins;
    std::vector<EffectsCustomBin> customBins; ///< in the order they were made
    std::vector<std::string> favorites;       ///< item references, no duplicates

    const EffectPreset* findPreset(std::string_view name) const;
    const EffectsCustomBin* findCustomBin(std::string_view name) const;
    bool isFavorite(std::string_view item) const;
    bool operator==(const EffectsLibrary&) const = default;
};

/// Case-insensitive (ASCII) equality of names.
bool samePresetName(std::string_view a, std::string_view b);
/// Puts the presets in name order (case-insensitive) and the presets bins in order, as the library keeps them.
void sortEffectsLibrary(EffectsLibrary& library);

/// Why a preset is invalid: its name (empty, too long, control characters), description, bin path, duration, number of
/// effects, a fixed effect's parameter unknown or out of range, a stack effect problem (effectStackProblem), effects of
/// both kinds; or nullopt.
std::optional<std::string> effectPresetProblem(const EffectPreset& preset);
/// Why a library is invalid (a preset's problem, duplicate names, a built-in's name, bounds, bad item references), or
/// nullopt.
std::optional<std::string> effectsLibraryProblem(const EffectsLibrary& library);
/// Whether `item` is a well-formed item reference (it need not exist).
bool isEffectItemReference(std::string_view item);
/// Why a presets bin path is invalid (empty segment, too deep, too long, control characters), or nullopt. "" is the
/// Presets bin itself (valid).
std::optional<std::string> presetBinProblem(std::string_view bin);

// ---- Built-in presets ---------------------------------------------------------------------------------------------------

/// The presets Wabcut ships, in Premiere's Presets bin structure (bins Bevel Edges, Blurs, Mosaics, PiPs/25% and
/// PiPs/40% with their Spin In / Out, Solarizes, Twirls, Image Pans; 0.2: Animations - Fade, Pop, Spin, Pull, Travel and
/// Spring In / Out) and its Lumetri Presets (the built-in looks, bin "Lumetri Presets/..."),
/// made for a sequence frame of width x height (PiP positions and pans are sequence pixels). Docs: docs/effects.md.
std::vector<EffectPreset> builtinEffectPresets(std::int32_t width = 1920, std::int32_t height = 1080);
/// A built-in preset by name (case-insensitive) for that frame, or nullopt.
std::optional<EffectPreset> findBuiltinEffectPreset(std::string_view name, std::int32_t width = 1920,
                                                    std::int32_t height = 1080);
/// The top-level bin of a built-in preset: "Presets" or "Lumetri Presets" (its `bin` begins with "Lumetri Presets").
bool isLumetriPreset(const EffectPreset& preset);

namespace edit {

/// What to save from a clip: stack effects by index, and fixed effects by id (kMotionEffect...).
struct PresetSelection {
    std::vector<std::size_t> effects;
    std::vector<std::string> fixed;
};

/// A preset of `clip`'s effects (Save Preset): keyframes relative to its In point at its speed, LUT assets as file
/// references, matte masks left out (`notes` says what was left out). Errors: InvalidArgument (nothing selected, an
/// index past the stack, a fixed effect of the other kind or unknown, effectPresetProblem).
std::expected<EffectPreset, Error> makeEffectPreset(const ProjectState& state, const Clip& clip,
                                                    const PresetSelection& selection, std::string name,
                                                    EffectPresetType type, std::string description = {},
                                                    std::string bin = {}, std::vector<std::string>* notes = nullptr);

struct PresetClipReport {
    ClipId clip;
    std::vector<std::string> applied; ///< effect names ("Motion", "Gaussian Blur")
    std::vector<std::string> skipped; ///< with the reason ("Twirl: not a video clip")
};

struct PresetResult {
    EditResult edit;
    std::vector<PresetClipReport> clips;
};

/// Applies a preset to clips (the Effects panel's double-click or drag): fixed effects' parameters replace the clip's,
/// stack effects are added after its own, keyframes are timed by the preset's type. Clips of the other kind are skipped.
/// LUT file references become assets of the project (an existing one with that path, else a new one). One undo step.
/// Errors: NotFound, InvalidArgument (a clip on a locked track, nothing applied to any clip, effectPresetProblem).
std::expected<PresetResult, Error> applyEffectPreset(const ProjectState& state, SequenceId sequence,
                                                     std::span<const ClipId> clips, const EffectPreset& preset);

} // namespace edit

namespace io {

inline constexpr std::string_view kEffectsLibraryFormat = "wabcut-effects-library";
inline constexpr std::string_view kEffectPresetsFileFormat = "wabcut-effect-presets";
inline constexpr int kEffectsLibraryVersion = 1;
/// The suffix of exported preset files (Effects panel > Export Preset).
inline constexpr std::string_view kEffectPresetsFileSuffix = ".wabcut-presets.json";

/// {"name", "description", "type", "bin", "duration", "effects": [the project file's effect JSON]}.
std::string effectPresetToJson(const EffectPreset& preset);
std::expected<EffectPreset, Error> effectPresetFromJson(std::string_view text);

/// The library file: {"format": "wabcut-effects-library", "version": 1, "presets", "presetBins", "customBins",
/// "favorites"}. A newer version is refused (VersionMismatch), not half-read.
std::string effectsLibraryToJson(const EffectsLibrary& library);
std::expected<EffectsLibrary, Error> effectsLibraryFromJson(std::string_view text);

/// An exported presets file: {"format": "wabcut-effect-presets", "version": 1, "presets": [...]}.
std::string effectPresetsFileToJson(std::span<const EffectPreset> presets);
std::expected<std::vector<EffectPreset>, Error> effectPresetsFileFromJson(std::string_view text);

} // namespace io

} // namespace wabcut
