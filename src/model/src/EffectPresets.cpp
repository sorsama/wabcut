// Effect presets, custom bins and favourites (EffectPresets.h): checks, built-in presets, Save Preset and applying a
// preset, and the JSON of library and preset files.

#include <wabcut/model/EffectPresets.h>

#include <wabcut/model/Blend.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>

#include "JsonIo.h"
#include "SequenceEditor.h"

#include <algorithm>
#include <tuple>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace wabcut {

namespace {

constexpr std::int64_t kMaxPresetTicks = std::int64_t{1} << 60;

struct FixedEffect {
    std::string_view id;
    std::string_view name; // Premiere's section name
    EffectKind kind;
    std::array<ParamId, 6> params;
    std::size_t count;
};

constexpr std::array<FixedEffect, 4> kFixedEffects{{
    {kMotionEffect, "Motion", EffectKind::Video,
     {ParamId::PositionX, ParamId::PositionY, ParamId::Scale, ParamId::Rotation, ParamId::AnchorX, ParamId::AnchorY}, 6},
    {kOpacityEffect, "Opacity", EffectKind::Video, {ParamId::Opacity}, 1},
    {kVolumeEffect, "Volume", EffectKind::Audio, {ParamId::VolumeDb}, 1},
    {kPannerEffect, "Panner", EffectKind::Audio, {ParamId::Pan}, 1},
}};

const FixedEffect* fixedEffect(std::string_view id) {
    for (const FixedEffect& f : kFixedEffects) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

std::optional<ParamId> fixedParam(const FixedEffect& f, std::string_view key) {
    for (std::size_t i = 0; i < f.count; ++i) {
        if (parameterInfo(f.params[i]).key == key) {
            return f.params[i];
        }
    }
    return std::nullopt;
}

std::string effectNameOf(const EffectInstance& e) {
    if (const FixedEffect* f = fixedEffect(e.effect)) {
        return std::string(f->name);
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
    return d ? std::string(d->displayName) : e.effect;
}

std::optional<EffectKind> stackKindOf(const EffectInstance& e) {
    if (const FixedEffect* f = fixedEffect(e.effect)) {
        return f->kind;
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
    if (d && (d->kind == EffectKind::Video || d->kind == EffectKind::Audio)) {
        return d->kind;
    }
    return std::nullopt; // unknown effects take the preset's kind
}

/// A name, bin segment or custom bin name: 1..kMaxPresetNameBytes of UTF-8 without control characters, not only
/// spaces.
std::optional<std::string> nameProblem(std::string_view name, std::string_view what) {
    if (name.empty()) {
        return std::string(what) + " is empty";
    }
    if (name.size() > kMaxPresetNameBytes) {
        return std::string(what) + " is longer than " + std::to_string(kMaxPresetNameBytes) + " bytes";
    }
    if (!edit::isValidUtf8(name)) {
        return std::string(what) + " is not UTF-8";
    }
    bool visible = false;
    for (const char ch : name) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u == 0x7f) {
            return std::string(what) + " has a control character";
        }
        visible = visible || ch != ' ';
    }
    if (!visible) {
        return std::string(what) + " is blank";
    }
    return std::nullopt;
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool lessName(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return lower(x) < lower(y); });
}

/// Curve ticks per timeline tick of a clip, as a ratio: its speed (negative reversed); 1 on a freeze frame or a
/// remapped clip, whose curve time runs with the timeline (Clip::curveTimeAt).
std::pair<std::int64_t, std::int64_t> curveRate(const Clip& c) {
    if (c.isFreeze() || c.isRemapped()) {
        return {1, 1};
    }
    return {c.reversed ? -std::int64_t{c.speed.num} : std::int64_t{c.speed.num}, c.speed.den};
}

Ticks clampTicks(long double v) {
    const long double lim = static_cast<long double>(kMaxPresetTicks);
    return Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(std::clamp(v, -lim, lim))))};
}

/// Maps the keys of a value (constants stay) and keeps them sorted and unique (a reversed clip turns them around).
ParamValue mapCurve(const ParamValue& value, const auto& map) {
    const Curve* curve = value.curve();
    if (!curve) {
        return value;
    }
    Curve out = *curve;
    for (Keyframe& k : out.keys) {
        k.time = map(k.time);
    }
    std::stable_sort(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    out.keys.erase(std::unique(out.keys.begin(), out.keys.end(),
                               [](const Keyframe& a, const Keyframe& b) { return a.time == b.time; }),
                   out.keys.end());
    return ParamValue(std::move(out));
}

void mapMasks(std::vector<Mask>& masks, const auto& map) {
    for (Mask& m : masks) {
        for (MaskParam& p : m.params) {
            p.value = mapCurve(p.value, map);
        }
        for (MaskPathKey& k : m.path) {
            k.time = map(k.time);
        }
        std::stable_sort(m.path.begin(), m.path.end(), [](const MaskPathKey& a, const MaskPathKey& b) { return a.time < b.time; });
        m.path.erase(std::unique(m.path.begin(), m.path.end(),
                                 [](const MaskPathKey& a, const MaskPathKey& b) { return a.time == b.time; }),
                     m.path.end());
    }
}

// ---- Built-in presets ------------------------------------------------------------------------------------------------

constexpr Ticks kSecond{kTicksPerSecond};
constexpr Ticks kBuiltinDuration{5 * kTicksPerSecond};

ParamValue keyed(std::initializer_list<std::pair<Ticks, double>> keys) {
    Curve c;
    for (const auto& [t, v] : keys) {
        c.keys.push_back(Keyframe{t, v, Interpolation::Linear, {}, {}});
    }
    return ParamValue(std::move(c));
}

EffectInstance stackEffect(std::string_view id, std::initializer_list<std::pair<std::string_view, ParamValue>> params) {
    EffectInstance e;
    e.effect = std::string(id);
    for (const auto& [key, value] : params) {
        e.set(std::string(key), value);
    }
    return e;
}

/// Keys with their interpolation (the eased animations).
ParamValue eased(std::initializer_list<std::tuple<Ticks, double, Interpolation>> keys) {
    Curve c;
    for (const auto& [t, v, i] : keys) {
        c.keys.push_back(Keyframe{t, v, i, {}, {}});
    }
    return ParamValue(std::move(c));
}

/// A fixed effect (kMotionEffect, kOpacityEffect) setting only the given parameters: applying it leaves the clip's others.
EffectInstance fixedOnly(std::string_view effect, std::initializer_list<std::pair<std::string_view, ParamValue>> params) {
    EffectInstance e;
    e.effect = std::string(effect);
    for (const auto& [key, value] : params) {
        e.set(std::string(key), value);
    }
    return e;
}

EffectInstance motion(ParamValue x, ParamValue y, ParamValue scale) {
    EffectInstance e;
    e.effect = std::string(kMotionEffect);
    e.set("motion.position.x", std::move(x));
    e.set("motion.position.y", std::move(y));
    e.set("motion.scale", std::move(scale));
    e.set("motion.rotation", 0.0);
    e.set("motion.anchor.x", 0.0);
    e.set("motion.anchor.y", 0.0);
    return e;
}

EffectPreset preset(std::string name, std::string bin, EffectPresetType type, std::string description,
                    std::vector<EffectInstance> effects) {
    EffectPreset p;
    p.name = std::move(name);
    p.bin = std::move(bin);
    p.type = type;
    p.description = std::move(description);
    p.duration = kBuiltinDuration;
    std::uint64_t id = 1;
    for (EffectInstance& e : effects) {
        e.id = EffectInstanceId{id++};
    }
    p.effects = std::move(effects);
    return p;
}

} // namespace

// ---- Names ---------------------------------------------------------------------------------------------------------------

std::string_view toString(EffectPresetType type) {
    switch (type) {
    case EffectPresetType::Scale:
        return "scale";
    case EffectPresetType::AnchorToInPoint:
        return "anchorToInPoint";
    case EffectPresetType::AnchorToOutPoint:
        return "anchorToOutPoint";
    }
    return "scale";
}

std::optional<EffectPresetType> effectPresetTypeFromString(std::string_view text) {
    for (const EffectPresetType t : {EffectPresetType::Scale, EffectPresetType::AnchorToInPoint, EffectPresetType::AnchorToOutPoint}) {
        if (toString(t) == text) {
            return t;
        }
    }
    return std::nullopt;
}

std::string_view displayName(EffectPresetType type) {
    switch (type) {
    case EffectPresetType::Scale:
        return "Scale";
    case EffectPresetType::AnchorToInPoint:
        return "Anchor to In Point";
    case EffectPresetType::AnchorToOutPoint:
        return "Anchor to Out Point";
    }
    return "Scale";
}

EffectKind EffectPreset::kind() const {
    for (const EffectInstance& e : effects) {
        if (const auto k = stackKindOf(e)) {
            return *k;
        }
    }
    return EffectKind::Video;
}

bool samePresetName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

const EffectPreset* EffectsLibrary::findPreset(std::string_view name) const {
    const auto it = std::find_if(presets.begin(), presets.end(), [&](const EffectPreset& p) { return samePresetName(p.name, name); });
    return it == presets.end() ? nullptr : &*it;
}

const EffectsCustomBin* EffectsLibrary::findCustomBin(std::string_view name) const {
    const auto it =
        std::find_if(customBins.begin(), customBins.end(), [&](const EffectsCustomBin& b) { return samePresetName(b.name, name); });
    return it == customBins.end() ? nullptr : &*it;
}

bool EffectsLibrary::isFavorite(std::string_view item) const {
    return std::find(favorites.begin(), favorites.end(), item) != favorites.end();
}

void sortEffectsLibrary(EffectsLibrary& library) {
    std::stable_sort(library.presets.begin(), library.presets.end(),
                     [](const EffectPreset& a, const EffectPreset& b) { return lessName(a.name, b.name); });
    std::sort(library.presetBins.begin(), library.presetBins.end());
}

// ---- Checks --------------------------------------------------------------------------------------------------------------

std::optional<std::string> presetBinProblem(std::string_view bin) {
    if (bin.empty()) {
        return std::nullopt;
    }
    std::size_t depth = 0;
    std::size_t start = 0;
    while (start <= bin.size()) {
        const std::size_t slash = std::min(bin.find('/', start), bin.size());
        if (++depth > kMaxPresetBinDepth) {
            return "the presets bin is nested more than " + std::to_string(kMaxPresetBinDepth) + " deep";
        }
        if (const auto problem = nameProblem(bin.substr(start, slash - start), "a presets bin name")) {
            return *problem;
        }
        start = slash + 1;
    }
    return std::nullopt;
}

std::optional<std::string> effectPresetProblem(const EffectPreset& p) {
    if (const auto problem = nameProblem(p.name, "the preset's name")) {
        return problem;
    }
    if (p.description.size() > kMaxPresetDescriptionBytes || !edit::isValidUtf8(p.description)) {
        return "the description of preset '" + p.name + "' is not UTF-8 of at most " +
               std::to_string(kMaxPresetDescriptionBytes) + " bytes";
    }
    if (const auto problem = presetBinProblem(p.bin)) {
        return "preset '" + p.name + "': " + *problem;
    }
    if (p.duration.value <= 0 || p.duration.value > kMaxPresetTicks) {
        return "preset '" + p.name + "' has no valid duration";
    }
    if (p.effects.empty() || p.effects.size() > kMaxPresetEffects) {
        return "preset '" + p.name + "' needs 1 to " + std::to_string(kMaxPresetEffects) + " effects";
    }
    const EffectKind kind = p.kind();
    EffectStack stack;
    std::set<std::uint64_t> ids;
    for (const EffectInstance& e : p.effects) {
        const std::string what = "preset '" + p.name + "', effect '" + e.effect + "'";
        if (!e.id.isValid() || e.id.value > kMaxPresetEffects || !ids.insert(e.id.value).second) {
            return what + " has a missing or repeated id";
        }
        if (const auto k = stackKindOf(e); k && *k != kind) {
            return "preset '" + p.name + "' mixes video and audio effects";
        }
        const FixedEffect* f = fixedEffect(e.effect);
        if (!f) {
            stack.push_back(e);
            continue;
        }
        for (std::size_t i = 0; i < e.params.size(); ++i) {
            if (i > 0 && !(e.params[i - 1].key < e.params[i].key)) {
                return what + " has unsorted or duplicate parameter keys";
            }
            const auto id = fixedParam(*f, e.params[i].key);
            if (!id) {
                return what + " has no parameter " + e.params[i].key;
            }
            const ParameterInfo& info = parameterInfo(*id);
            if (!e.params[i].value.isValid(info.minValue, info.maxValue)) {
                return what + " has parameter " + e.params[i].key + " outside its range or not finite";
            }
        }
        if (!e.masks.empty() || !e.mask.empty()) {
            return what + " cannot have masks";
        }
        for (const EffectData& d : e.data) {
            const std::string* text = d.text();
            if (f->id != kOpacityEffect || d.key != kPresetBlendModeKey || !text || !blendModeFromString(*text)) {
                return what + " has unknown data '" + d.key + "'";
            }
        }
    }
    if (const auto problem = effectStackProblem(stack, kind, kMaxPresetEffects + 1)) {
        return "preset '" + p.name + "': " + *problem;
    }
    return std::nullopt;
}

bool isEffectItemReference(std::string_view item) {
    for (const std::string_view prefix : {kEffectItemPrefix, kPresetItemPrefix}) {
        if (item.starts_with(prefix)) {
            const std::string_view rest = item.substr(prefix.size());
            return !nameProblem(rest, "an item");
        }
    }
    return false;
}

std::optional<std::string> effectsLibraryProblem(const EffectsLibrary& library) {
    if (library.presets.size() > kMaxEffectPresets) {
        return "more than " + std::to_string(kMaxEffectPresets) + " presets";
    }
    const std::vector<EffectPreset> builtins = builtinEffectPresets();
    for (std::size_t i = 0; i < library.presets.size(); ++i) {
        const EffectPreset& p = library.presets[i];
        if (const auto problem = effectPresetProblem(p)) {
            return problem;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (samePresetName(library.presets[j].name, p.name)) {
                return "two presets are named '" + p.name + "'";
            }
        }
        for (const EffectPreset& b : builtins) {
            if (samePresetName(b.name, p.name)) {
                return "preset '" + p.name + "' has the name of a built-in preset";
            }
        }
    }
    if (library.presetBins.size() > kMaxPresetBins) {
        return "more than " + std::to_string(kMaxPresetBins) + " presets bins";
    }
    for (std::size_t i = 0; i < library.presetBins.size(); ++i) {
        if (library.presetBins[i].empty()) {
            return "a presets bin has no name";
        }
        if (const auto problem = presetBinProblem(library.presetBins[i])) {
            return problem;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (samePresetName(library.presetBins[j], library.presetBins[i])) {
                return "presets bin '" + library.presetBins[i] + "' is listed twice";
            }
        }
    }
    if (library.customBins.size() > kMaxCustomBins) {
        return "more than " + std::to_string(kMaxCustomBins) + " custom bins";
    }
    const auto itemsProblem = [](const std::vector<std::string>& items, std::size_t max, const std::string& where)
        -> std::optional<std::string> {
        if (items.size() > max) {
            return where + " has more than " + std::to_string(max) + " items";
        }
        std::set<std::string_view> seen;
        for (const std::string& item : items) {
            if (!isEffectItemReference(item)) {
                return where + " has an item that is not \"effect:<id>\" or \"preset:<name>\"";
            }
            if (!seen.insert(item).second) {
                return where + " lists '" + item + "' twice";
            }
        }
        return std::nullopt;
    };
    for (std::size_t i = 0; i < library.customBins.size(); ++i) {
        const EffectsCustomBin& b = library.customBins[i];
        if (const auto problem = nameProblem(b.name, "a custom bin's name")) {
            return problem;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (samePresetName(library.customBins[j].name, b.name)) {
                return "two custom bins are named '" + b.name + "'";
            }
        }
        if (const auto problem = itemsProblem(b.items, kMaxCustomBinItems, "custom bin '" + b.name + "'")) {
            return problem;
        }
    }
    return itemsProblem(library.favorites, kMaxFavorites, "the favourites");
}

// ---- Built-in presets ----------------------------------------------------------------------------------------------------

std::vector<EffectPreset> builtinEffectPresets(std::int32_t width, std::int32_t height) {
    const double w = std::max(1, width);
    const double h = std::max(1, height);
    using T = EffectPresetType;
    const Ticks end = kBuiltinDuration;
    std::vector<EffectPreset> out;

    // Bevel Edges (Premiere's Bevel Edges Thick / Thin): the frame's edges lit as a bevel.
    out.push_back(preset("Bevel Edges Thick", "Bevel Edges", T::Scale, "Thick lit, bevelled edges on the frame.",
                         {stackEffect("wabcut.perspective.bevelEdges", {{"thickness", 0.2}})}));
    out.push_back(preset("Bevel Edges Thin", "Bevel Edges", T::Scale, "Thin lit, bevelled edges on the frame.",
                         {stackEffect("wabcut.perspective.bevelEdges", {{"thickness", 0.05}})}));

    // Blurs: Premiere's Fast Blur In / Out (its Fast Blur is now Gaussian Blur).
    const double blur = std::round(h * 0.05);
    out.push_back(preset("Fast Blur In", "Blurs", T::AnchorToInPoint, "Starts blurred and sharpens over the first second.",
                         {stackEffect("wabcut.blur.gaussian", {{"radius", keyed({{Ticks{}, blur}, {kSecond, 0.0}})}})}));
    out.push_back(preset("Fast Blur Out", "Blurs", T::AnchorToOutPoint, "Blurs out over the last second.",
                         {stackEffect("wabcut.blur.gaussian", {{"radius", keyed({{end - kSecond, 0.0}, {end, blur}})}})}));

    // Mosaics: from big cells to (nearly) none, and back.
    out.push_back(preset("Mosaic In", "Mosaics", T::AnchorToInPoint, "Starts as big blocks that resolve over the first second.",
                         {stackEffect("wabcut.mosaic", {{"cellSize", keyed({{Ticks{}, 0.1}, {kSecond, 0.001}})}})}));
    out.push_back(preset("Mosaic Out", "Mosaics", T::AnchorToOutPoint, "Breaks into big blocks over the last second.",
                         {stackEffect("wabcut.mosaic", {{"cellSize", keyed({{end - kSecond, 0.001}, {end, 0.1}})}})}));

    // PiPs: the clip (frame-size) at 25 % or 40 % in a corner, 5 % of the frame from its edges.
    struct Corner {
        const char* name;
        double sx;
        double sy;
    };
    constexpr std::array<Corner, 4> kCorners{{{"UL", -1, -1}, {"UR", 1, -1}, {"LL", -1, 1}, {"LR", 1, 1}}};
    const auto cornerX = [&](double sx, double scale) { return sx * (w / 2 - 0.05 * w - scale * w / 2); };
    const auto cornerY = [&](double sy, double scale) { return sy * (h / 2 - 0.05 * h - scale * h / 2); };
    for (const double scale : {0.25, 0.40}) {
        const std::string pct = scale < 0.3 ? "25%" : "40%";
        const std::string bin = "PiPs/" + pct;
        for (const Corner& c : kCorners) {
            const double x = cornerX(c.sx, scale);
            const double y = cornerY(c.sy, scale);
            const std::string base = "PiP " + pct + " " + c.name;
            out.push_back(preset(base, bin, T::Scale, "The clip at " + pct + " in the " + c.name + " corner.",
                                 {motion(x, y, scale)}));
            out.push_back(preset(base + " Scale In", bin, T::AnchorToInPoint,
                                 "Grows from nothing into the " + std::string(c.name) + " corner over the first second.",
                                 {motion(x, y, keyed({{Ticks{}, 0.0}, {kSecond, scale}}))}));
            out.push_back(preset(base + " Scale Out", bin, T::AnchorToOutPoint,
                                 "Shrinks to nothing in the " + std::string(c.name) + " corner over the last second.",
                                 {motion(x, y, keyed({{end - kSecond, scale}, {end, 0.0}}))}));
        }
        // Spins: Premiere's PiP Spin In / Spin Out - a full turn while growing into (shrinking out of) the corner.
        for (const Corner& c : kCorners) {
            const double x = cornerX(c.sx, scale);
            const double y = cornerY(c.sy, scale);
            const std::string base = "PiP " + pct + " " + c.name;
            out.push_back(preset(base + " Spin In", bin, T::AnchorToInPoint,
                                 "Spins a full turn while growing into the " + std::string(c.name) + " corner over the first second.",
                                 {fixedOnly(kMotionEffect, {{"motion.position.x", x}, {"motion.position.y", y},
                                                            {"motion.scale", keyed({{Ticks{}, 0.0}, {kSecond, scale}})},
                                                            {"motion.rotation", keyed({{Ticks{}, -360.0}, {kSecond, 0.0}})}})}));
            out.push_back(preset(base + " Spin Out", bin, T::AnchorToOutPoint,
                                 "Spins a full turn while shrinking out of the " + std::string(c.name) + " corner over the last second.",
                                 {fixedOnly(kMotionEffect, {{"motion.position.x", x}, {"motion.position.y", y},
                                                            {"motion.scale", keyed({{end - kSecond, scale}, {end, 0.0}})},
                                                            {"motion.rotation", keyed({{end - kSecond, 0.0}, {end, 360.0}})}})}));
        }
        // Moves between opposite corners over the clip.
        const std::array<std::pair<const Corner*, const Corner*>, 4> kMoves{
            {{&kCorners[0], &kCorners[3]}, {&kCorners[3], &kCorners[0]}, {&kCorners[1], &kCorners[2]}, {&kCorners[2], &kCorners[1]}}};
        for (const auto& [from, to] : kMoves) {
            out.push_back(preset("PiP " + pct + " Motion " + from->name + " to " + to->name, bin, T::Scale,
                                 "Moves from the " + std::string(from->name) + " to the " + to->name + " corner over the clip.",
                                 {motion(keyed({{Ticks{}, cornerX(from->sx, scale)}, {end, cornerX(to->sx, scale)}}),
                                         keyed({{Ticks{}, cornerY(from->sy, scale)}, {end, cornerY(to->sy, scale)}}), scale)}));
        }
    }

    // Solarizes: inverts every tone at the start, none at the end (Threshold 0 -> 1), and back.
    out.push_back(preset("Solarize In", "Solarizes", T::AnchorToInPoint, "Starts solarized and clears over the first second.",
                         {stackEffect("wabcut.stylize.solarize", {{"threshold", keyed({{Ticks{}, 0.0}, {kSecond, 1.0}})}})}));
    out.push_back(preset("Solarize Out", "Solarizes", T::AnchorToOutPoint, "Solarizes over the last second.",
                         {stackEffect("wabcut.stylize.solarize", {{"threshold", keyed({{end - kSecond, 1.0}, {end, 0.0}})}})}));

    // Twirls: a full turn unwinding over the first second, and winding up over the last.
    out.push_back(preset("Twirl In", "Twirls", T::AnchorToInPoint, "Untwirls over the first second.",
                         {stackEffect("wabcut.distort.twirl", {{"angle", keyed({{Ticks{}, 360.0}, {kSecond, 0.0}})}, {"radius", 50.0}})}));
    out.push_back(preset("Twirl Out", "Twirls", T::AnchorToOutPoint, "Twirls up over the last second.",
                         {stackEffect("wabcut.distort.twirl", {{"angle", keyed({{end - kSecond, 0.0}, {end, 360.0}})}, {"radius", 50.0}})}));

    // Image Pans: the clip at 120 % travelling across the frame over its length (Ken Burns pans).
    const double panX = 0.1 * w;
    const double panY = 0.1 * h;
    out.push_back(preset("Horizontal Image Pan Left to Right", "Image Pans", T::Scale,
                         "At 120 %, pans from the left of the picture to its right over the clip.",
                         {motion(keyed({{Ticks{}, panX}, {end, -panX}}), 0.0, 1.2)}));
    out.push_back(preset("Horizontal Image Pan Right to Left", "Image Pans", T::Scale,
                         "At 120 %, pans from the right of the picture to its left over the clip.",
                         {motion(keyed({{Ticks{}, -panX}, {end, panX}}), 0.0, 1.2)}));
    out.push_back(preset("Vertical Image Pan Top to Bottom", "Image Pans", T::Scale,
                         "At 120 %, pans from the top of the picture to its bottom over the clip.",
                         {motion(0.0, keyed({{Ticks{}, panY}, {end, -panY}}), 1.2)}));
    out.push_back(preset("Vertical Image Pan Bottom to Top", "Image Pans", T::Scale,
                         "At 120 %, pans from the bottom of the picture to its top over the clip.",
                         {motion(0.0, keyed({{Ticks{}, -panY}, {end, panY}}), 1.2)}));
    // 0.4 Ken Burns moves for stills and archive: slow eased push-ins and pull-outs across the whole clip (type Scale,
    // so they stretch to any length). Only the Motion parameters a move changes are set (Rotation and Anchor stay the
    // clip's), and position keys share the scale keys' times and easing, so the frame's edge never shows: at every
    // moment |position| <= (scale - 1) / 2 of the frame side.
    {
        using I = Interpolation;
        const auto ease = [&](double from, double to) { return eased({{Ticks{}, from, I::EaseInOut}, {end, to, I::Linear}}); };
        out.push_back(preset("Slow Push In", "Image Pans", T::Scale,
                             "Slowly pushes in from 100 % to 112 % over the clip, eased (Ken Burns).",
                             {fixedOnly(kMotionEffect, {{"motion.scale", ease(1.0, 1.12)}})}));
        out.push_back(preset("Slow Pull Out", "Image Pans", T::Scale,
                             "Slowly pulls out from 112 % to 100 % over the clip, eased (Ken Burns).",
                             {fixedOnly(kMotionEffect, {{"motion.scale", ease(1.12, 1.0)}})}));
        out.push_back(preset("Push In to Top", "Image Pans", T::Scale,
                             "Pushes in from 100 % to 112 % while drifting to the top of the picture, eased.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", ease(1.0, 1.12)},
                                                        {"motion.position.y", ease(0.0, 0.05 * h)}})}));
        out.push_back(preset("Push In to Bottom", "Image Pans", T::Scale,
                             "Pushes in from 100 % to 112 % while drifting to the bottom of the picture, eased.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", ease(1.0, 1.12)},
                                                        {"motion.position.y", ease(0.0, -0.05 * h)}})}));
        out.push_back(preset("Push In Left to Right", "Image Pans", T::Scale,
                             "Pushes in from 108 % to 112 % while travelling from the left of the picture to its right, eased.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", ease(1.08, 1.12)},
                                                        {"motion.position.x", ease(0.03 * w, -0.03 * w)}})}));
        out.push_back(preset("Push In Right to Left", "Image Pans", T::Scale,
                             "Pushes in from 108 % to 112 % while travelling from the right of the picture to its left, eased.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", ease(1.08, 1.12)},
                                                        {"motion.position.x", ease(-0.03 * w, 0.03 * w)}})}));
    }

    // Animations (Premiere 25.5's motion animations, e.g. Pop, Spin, Travel, Pull, Spring): a clip animates in at its
    // In point or out at its Out point with editable Motion and Opacity keys, half a second long (Spring a little
    // longer); only the parameters an animation moves are set, and an In and an Out animation on one clip both stay
    // (applyEffectPreset keeps keys outside an anchored preset's time). Out animations are the In ones reversed.
    {
        using I = Interpolation;
        const Ticks half{kTicksPerSecond / 2};
        const auto at = [](double seconds) { return Ticks{static_cast<std::int64_t>(seconds * kTicksPerSecond)}; };
        const auto outAt = [&](double seconds) { return end - at(seconds); }; // seconds before the Out point
        const std::string bin = "Animations";
        out.push_back(preset("Fade In", bin, T::AnchorToInPoint, "Fades in from transparent over the first half second.",
                             {fixedOnly(kOpacityEffect, {{"opacity.opacity", eased({{Ticks{}, 0.0, I::EaseOut}, {half, 1.0, I::Linear}})}})}));
        out.push_back(preset("Fade Out", bin, T::AnchorToOutPoint, "Fades out to transparent over the last half second.",
                             {fixedOnly(kOpacityEffect, {{"opacity.opacity", eased({{end - half, 1.0, I::EaseIn}, {end, 0.0, I::Linear}})}})}));
        // Pop: grows from nothing past full size and settles, fading in quickly.
        out.push_back(preset("Pop In", bin, T::AnchorToInPoint, "Pops up from nothing, overshoots a little and settles.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", eased({{Ticks{}, 0.0, I::EaseOut}, {at(0.32), 1.1, I::EaseInOut},
                                                                                 {half, 1.0, I::Linear}})}}),
                              fixedOnly(kOpacityEffect, {{"opacity.opacity", eased({{Ticks{}, 0.0, I::Linear}, {at(0.12), 1.0, I::Linear}})}})}));
        out.push_back(preset("Pop Out", bin, T::AnchorToOutPoint, "Swells a little, then pops down to nothing.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", eased({{outAt(0.5), 1.0, I::EaseInOut}, {outAt(0.32), 1.1, I::EaseIn},
                                                                                 {end, 0.0, I::Linear}})}}),
                              fixedOnly(kOpacityEffect, {{"opacity.opacity", eased({{outAt(0.12), 1.0, I::Linear}, {end, 0.0, I::Linear}})}})}));
        // Spin: half a turn while growing in (shrinking out).
        out.push_back(preset("Spin In", bin, T::AnchorToInPoint, "Spins in half a turn while growing to full size.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", eased({{Ticks{}, 0.0, I::EaseOut}, {half, 1.0, I::Linear}})},
                                                        {"motion.rotation", eased({{Ticks{}, -180.0, I::EaseOut}, {half, 0.0, I::Linear}})}})}));
        out.push_back(preset("Spin Out", bin, T::AnchorToOutPoint, "Spins out half a turn while shrinking to nothing.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", eased({{end - half, 1.0, I::EaseIn}, {end, 0.0, I::Linear}})},
                                                        {"motion.rotation", eased({{end - half, 0.0, I::EaseIn}, {end, 180.0, I::Linear}})}})}));
        // Pull: from close up (160 %) back to full size while fading in; out: pushes in and fades.
        out.push_back(preset("Pull In", bin, T::AnchorToInPoint, "Pulls back from a close-up to full size while fading in.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", eased({{Ticks{}, 1.6, I::EaseOut}, {half, 1.0, I::Linear}})}}),
                              fixedOnly(kOpacityEffect, {{"opacity.opacity", eased({{Ticks{}, 0.0, I::Linear}, {at(0.3), 1.0, I::Linear}})}})}));
        out.push_back(preset("Pull Out", bin, T::AnchorToOutPoint, "Pushes into a close-up while fading out.",
                             {fixedOnly(kMotionEffect, {{"motion.scale", eased({{end - half, 1.0, I::EaseIn}, {end, 1.6, I::Linear}})}}),
                              fixedOnly(kOpacityEffect, {{"opacity.opacity", eased({{outAt(0.3), 1.0, I::Linear}, {end, 0.0, I::Linear}})}})}));
        // Travel: in from off the frame on one side; out to off the frame on one side.
        struct Side {
            const char* name;
            double dx;
            double dy;
        };
        constexpr std::array<Side, 4> kSides{{{"Left", -1, 0}, {"Right", 1, 0}, {"Top", 0, -1}, {"Bottom", 0, 1}}};
        for (const Side& side : kSides) {
            const double ox = side.dx * w;
            const double oy = side.dy * h;
            const std::string lower =
                std::string("the ") + (side.dx < 0 ? "left" : side.dx > 0 ? "right" : side.dy < 0 ? "top" : "bottom");
            EffectInstance in = fixedOnly(kMotionEffect, {});
            if (side.dx != 0) {
                in.set("motion.position.x", eased({{Ticks{}, ox, I::EaseOut}, {half, 0.0, I::Linear}}));
            } else {
                in.set("motion.position.y", eased({{Ticks{}, oy, I::EaseOut}, {half, 0.0, I::Linear}}));
            }
            out.push_back(preset(std::string("Travel In from ") + side.name, bin, T::AnchorToInPoint,
                                 "Slides in from " + lower + " of the frame.", {in}));
            EffectInstance gone = fixedOnly(kMotionEffect, {});
            if (side.dx != 0) {
                gone.set("motion.position.x", eased({{end - half, 0.0, I::EaseIn}, {end, ox, I::Linear}}));
            } else {
                gone.set("motion.position.y", eased({{end - half, 0.0, I::EaseIn}, {end, oy, I::Linear}}));
            }
            out.push_back(preset(std::string("Travel Out to ") + side.name, bin, T::AnchorToOutPoint,
                                 "Slides out to " + lower + " of the frame.", {gone}));
        }
        // Spring: up from below the frame, past its place and back, settling (a damped bounce); out: hops and drops.
        out.push_back(preset("Spring In", bin, T::AnchorToInPoint, "Springs up from below the frame, bounces and settles.",
                             {fixedOnly(kMotionEffect, {{"motion.position.y", eased({{Ticks{}, h, I::EaseOut}, {at(0.36), -0.06 * h, I::EaseInOut},
                                                                                     {at(0.52), 0.025 * h, I::EaseInOut}, {at(0.66), 0.0, I::Linear}})}})}));
        out.push_back(preset("Spring Out", bin, T::AnchorToOutPoint, "Hops up, then drops out below the frame.",
                             {fixedOnly(kMotionEffect, {{"motion.position.y", eased({{outAt(0.66), 0.0, I::EaseInOut}, {outAt(0.46), -0.06 * h, I::EaseIn},
                                                                                     {end, h, I::Linear}})}})}));
    }

    // Convolution Kernel presets (the names of Premiere's Convolution Kernel bin; the kernels are the classic ones, not
    // read from Premiere): 3 x 3 weights M11..M33 row by row, Offset (0..255 levels) and Scale.
    {
        struct Kernel {
            const char* name;
            std::array<double, 9> m;
            double offset;
            double scale;
            const char* description;
        };
        static constexpr std::array<Kernel, 11> kKernels{{
            {"Blur", {1, 1, 1, 1, 2, 1, 1, 1, 1}, 0, 10, "A light blur: each pixel averaged with its neighbours."},
            {"Blur More", {1, 1, 1, 1, 1, 1, 1, 1, 1}, 0, 9, "A stronger blur: the plain average of nine pixels."},
            {"Darken", {0, 0, 0, 0, 8, 0, 0, 0, 0}, 0, 10, "Darkens every channel to 80 %."},
            {"Emboss", {-1, -1, 0, -1, 0, 1, 0, 1, 1}, 128, 1, "A grey relief lit from the top left."},
            {"Find Edges", {-1, -1, -1, -1, 8, -1, -1, -1, -1}, 0, 1, "Bright edges on black (a Laplacian)."},
            {"Gaussian Blur", {1, 2, 1, 2, 4, 2, 1, 2, 1}, 0, 16, "A smooth 3 x 3 Gaussian blur."},
            {"Gaussian Sharpen", {-1, -2, -1, -2, 28, -2, -1, -2, -1}, 0, 16, "Sharpens against a Gaussian blur."},
            {"Lighten", {0, 0, 0, 0, 12, 0, 0, 0, 0}, 0, 10, "Brightens every channel to 120 %."},
            {"Sharpen", {0, -1, 0, -1, 5, -1, 0, -1, 0}, 0, 1, "Sharpens against the four nearest neighbours."},
            {"Sharpen Edges", {0, -1, 0, -1, 6, -1, 0, -1, 0}, 0, 2, "A gentle sharpen that stresses the edges."},
            {"Sharpen More", {-1, -1, -1, -1, 9, -1, -1, -1, -1}, 0, 1, "A strong sharpen against all eight neighbours."},
        }};
        static constexpr std::array<std::string_view, 9> kKeys{"m11", "m12", "m13", "m21", "m22", "m23", "m31", "m32", "m33"};
        for (const Kernel& k : kKernels) {
            EffectInstance e;
            e.effect = "wabcut.adjust.convolutionKernel";
            for (std::size_t i = 0; i < kKeys.size(); ++i) {
                e.set(std::string(kKeys[i]), k.m[i]);
            }
            e.set("offset", k.offset);
            e.set("scale", k.scale);
            out.push_back(preset(k.name, "Convolution Kernel", T::Scale, k.description, {std::move(e)}));
        }
    }

    // Lumetri Presets: Color Grade with a built-in look (render/color Looks.h) or a technical input LUT.
    struct Look {
        const char* name;
        const char* bin;
        const char* key;
        const char* lut;
        const char* description;
    };
    constexpr std::array<Look, 7> kLooks{{
        {"Teal & Orange", "Cinematic", "creative.look", "builtin:teal-orange", "Cool shadows, warm skin and highlights."},
        {"Cool Night", "Cinematic", "creative.look", "builtin:cool-night", "Darker, blue and less saturated: day for night."},
        {"Warm Film", "Filmstock", "creative.look", "builtin:warm-film", "Lifted blacks, warm highlights, softer colour."},
        {"Bleach Bypass", "Filmstock", "creative.look", "builtin:bleach-bypass", "High contrast, low saturation."},
        {"Mono Silver", "Monochrome", "creative.look", "builtin:mono-silver", "Black and white with a cool silver tone."},
        {"Sony S-Log3 (S-Gamut3.Cine) to Rec.709", "Technical", "basic.inputLut", "builtin:slog3-sgamut3cine-to-rec709",
         "Input LUT: Sony S-Log3 footage to Rec.709."},
        {"Panasonic V-Log (V-Gamut) to Rec.709", "Technical", "basic.inputLut", "builtin:vlog-vgamut-to-rec709",
         "Input LUT: Panasonic V-Log footage to Rec.709."},
    }};
    for (const Look& look : kLooks) {
        EffectInstance grade;
        grade.effect = "wabcut.color.grade";
        grade.setData(EffectData{look.key, std::string(look.lut)});
        out.push_back(preset(look.name, std::string("Lumetri Presets/") + look.bin, T::Scale, look.description, {std::move(grade)}));
    }
    return out;
}

std::optional<EffectPreset> findBuiltinEffectPreset(std::string_view name, std::int32_t width, std::int32_t height) {
    for (EffectPreset& p : builtinEffectPresets(width, height)) {
        if (samePresetName(p.name, name)) {
            return std::move(p);
        }
    }
    return std::nullopt;
}

bool isLumetriPreset(const EffectPreset& preset) {
    return preset.bin == "Lumetri Presets" || preset.bin.starts_with("Lumetri Presets/");
}

// ---- Save Preset and applying a preset ----------------------------------------------------------------------------------

namespace edit {

std::expected<EffectPreset, Error> makeEffectPreset(const ProjectState& state, const Clip& clip,
                                                    const PresetSelection& selection, std::string name,
                                                    EffectPresetType type, std::string description, std::string bin,
                                                    std::vector<std::string>* notes) {
    using detail::invalidArgument;
    if (selection.effects.empty() && selection.fixed.empty()) {
        return std::unexpected(invalidArgument("choose at least one effect to save"));
    }
    if (clip.duration().value <= 0) {
        return std::unexpected(invalidArgument("the clip has no length"));
    }
    const bool video = clip.stream.kind == StreamKind::Video;
    const Ticks start = clip.curveTimeAt(clip.start());
    const auto [num, den] = curveRate(clip);
    // Curve time on the clip -> timeline ticks from its In point.
    const auto toOffset = [start, num, den](Ticks t) {
        return clampTicks(static_cast<long double>((t - start).value) * static_cast<long double>(den)
                          / static_cast<long double>(num));
    };
    EffectPreset p;
    p.name = std::move(name);
    p.description = std::move(description);
    p.type = type;
    p.bin = std::move(bin);
    p.duration = clip.duration();
    std::uint64_t nextId = 1;
    // Fixed effects first, in the panel's order (Motion, Opacity / Volume, Panner).
    for (const FixedEffect& f : kFixedEffects) {
        if (std::find(selection.fixed.begin(), selection.fixed.end(), f.id) == selection.fixed.end()) {
            continue;
        }
        if ((f.kind == EffectKind::Video) != video) {
            return std::unexpected(invalidArgument(std::string(f.name) + (video ? " is not a video effect" : " is not an audio effect")));
        }
        EffectInstance e;
        e.id = EffectInstanceId{nextId++};
        e.effect = std::string(f.id);
        for (std::size_t i = 0; i < f.count; ++i) {
            e.set(std::string(parameterInfo(f.params[i]).key), mapCurve(clip.properties()[f.params[i]], toOffset));
        }
        if (f.id == kOpacityEffect) {
            e.setData(EffectData{std::string(kPresetBlendModeKey), std::string(toString(clip.blendMode()))});
        }
        p.effects.push_back(std::move(e));
    }
    for (const std::string& id : selection.fixed) {
        if (!fixedEffect(id)) {
            return std::unexpected(invalidArgument("'" + id + "' is not one of the clip's fixed effects"));
        }
    }
    const EffectStack& stack = clip.effects();
    std::vector<std::size_t> indices = selection.effects;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    for (const std::size_t i : indices) {
        if (i >= stack.size()) {
            return std::unexpected(invalidArgument("the clip has no effect #" + std::to_string(i + 1)));
        }
        EffectInstance e = stack[i];
        e.id = EffectInstanceId{nextId++};
        for (EffectParam& param : e.params) {
            param.value = mapCurve(param.value, toOffset);
        }
        const std::size_t before = e.masks.size();
        std::erase_if(e.masks, [](const Mask& m) { return m.shape == MaskShape::Matte; });
        if (e.masks.size() != before && notes) {
            notes->push_back(effectNameOf(e) + ": tracked matte masks are not saved in presets");
        }
        mapMasks(e.masks, toOffset);
        std::vector<EffectData> data;
        for (EffectData d : e.data) {
            if (const std::string* text = d.text()) {
                if (const AssetId asset = assetOfReference(*text); asset.isValid()) {
                    const ProjectAsset* a = state.findAsset(asset);
                    if (!a || a->locator.path.empty()) {
                        if (notes) {
                            notes->push_back(effectNameOf(e) + ": " + d.key + " refers to a missing file; left out");
                        }
                        continue;
                    }
                    d.value = std::string(kPresetFilePrefix) + a->locator.path;
                    if (const auto problem = effectDataProblem(d)) {
                        if (notes) {
                            notes->push_back(effectNameOf(e) + ": " + d.key + " has a path too long to save; left out");
                        }
                        continue;
                    }
                }
            }
            data.push_back(std::move(d));
        }
        e.data = std::move(data);
        p.effects.push_back(std::move(e));
    }
    if (const auto problem = effectPresetProblem(p)) {
        return std::unexpected(invalidArgument(*problem));
    }
    return p;
}

std::expected<PresetResult, Error> applyEffectPreset(const ProjectState& state, SequenceId sequenceId,
                                                     std::span<const ClipId> clips, const EffectPreset& preset) {
    using detail::invalidArgument;
    if (const auto problem = effectPresetProblem(preset)) {
        return std::unexpected(invalidArgument(*problem));
    }
    if (clips.empty()) {
        return std::unexpected(invalidArgument("no clips to apply the preset to"));
    }
    // Portable file references -> assets of this project (an existing one with that path, else a new one).
    ProjectState current = state;
    std::map<std::string, AssetId> assets;
    for (const EffectInstance& e : preset.effects) {
        for (const EffectData& d : e.data) {
            const std::string* text = d.text();
            if (!text || !text->starts_with(kPresetFilePrefix) || assets.contains(*text)) {
                continue;
            }
            const std::string path = text->substr(kPresetFilePrefix.size());
            const auto existing = std::find_if(current.assets.begin(), current.assets.end(), [&](const ProjectAsset& a) {
                return a.kind == kAssetKindLut && a.locator.path == path;
            });
            if (existing != current.assets.end()) {
                assets.emplace(*text, existing->id);
                continue;
            }
            ProjectAsset asset;
            asset.kind = std::string(kAssetKindLut);
            asset.name = pathToUtf8(pathFromUtf8(path).filename());
            if (asset.name.empty()) {
                asset.name = path;
            }
            asset.locator.path = path;
            auto added = addAsset(current, std::move(asset));
            if (!added) {
                return std::unexpected(added.error());
            }
            current = std::move(added->state);
            assets.emplace(*text, added->id);
        }
    }
    auto seq = detail::findSequence(current, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    auto refs = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    const EffectKind kind = preset.kind();
    detail::SequenceEditor ed(current, **seq);
    std::vector<PresetClipReport> reports;
    bool any = false;
    for (const detail::ClipRef& ref : *refs) {
        Clip& target = ed.mutableClip(ref.track, ref.index);
        PresetClipReport report{target.id, {}, {}};
        const bool video = target.stream.kind == StreamKind::Video;
        if ((kind == EffectKind::Video) != video) {
            for (const EffectInstance& e : preset.effects) {
                report.skipped.push_back(effectNameOf(e) + (video ? ": not a video clip" : ": not an audio clip"));
            }
            reports.push_back(std::move(report));
            continue;
        }
        const Ticks start = target.curveTimeAt(target.start());
        const auto [num, den] = curveRate(target);
        const long double scale = static_cast<long double>(target.duration().value) / static_cast<long double>(preset.duration.value);
        const Ticks shift = target.duration() - preset.duration;
        const EffectPresetType type = preset.type;
        // Timeline ticks from the preset clip's In point -> curve time on the target.
        const auto toCurve = [=](Ticks offset) {
            long double u = static_cast<long double>(offset.value);
            if (type == EffectPresetType::Scale) {
                u *= scale;
            } else if (type == EffectPresetType::AnchorToOutPoint) {
                u += static_cast<long double>(shift.value);
            }
            return start + clampTicks(u * static_cast<long double>(num) / static_cast<long double>(den));
        };
        ClipContent content = target.content();
        for (const EffectInstance& e : preset.effects) {
            if (const FixedEffect* f = fixedEffect(e.effect)) {
                for (const EffectParam& param : e.params) {
                    if (const auto id = fixedParam(*f, param.key)) {
                        ParamValue mapped = mapCurve(param.value, toCurve);
                        // 0.2: an In or Out animation (an anchored preset with keys) replaces only the target's keys
                        // inside its own time, so an In and an Out animation on one clip both stay.
                        const Curve* incoming = mapped.curve();
                        const Curve* existing = content.properties[*id].curve();
                        if (type != EffectPresetType::Scale && incoming && existing && !incoming->keys.empty()) {
                            Curve merged = *incoming;
                            const Ticks from = incoming->keys.front().time;
                            const Ticks to = incoming->keys.back().time;
                            for (const Keyframe& k : existing->keys) {
                                if (k.time < from || k.time > to) {
                                    merged.keys.push_back(k);
                                }
                            }
                            std::stable_sort(merged.keys.begin(), merged.keys.end(),
                                             [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
                            mapped = ParamValue(std::move(merged));
                        }
                        content.properties[*id] = std::move(mapped);
                    }
                }
                if (const EffectData* blend = e.findData(kPresetBlendModeKey)) {
                    if (const auto mode = blendModeFromString(*blend->text())) {
                        content.blendMode = *mode;
                    }
                }
                report.applied.push_back(std::string(f->name));
                continue;
            }
            EffectInstance copy = e;
            copy.id = ed.newEffectId();
            for (EffectParam& param : copy.params) {
                param.value = mapCurve(param.value, toCurve);
            }
            mapMasks(copy.masks, toCurve);
            for (EffectData& d : copy.data) {
                if (const std::string* text = d.text(); text && text->starts_with(kPresetFilePrefix)) {
                    d.value = assetReference(assets.at(*text));
                }
            }
            content.effects.push_back(std::move(copy));
            report.applied.push_back(effectNameOf(e));
        }
        target.setContent(std::move(content));
        any = true;
        reports.push_back(std::move(report));
    }
    if (!any) {
        std::string why = "the preset fits none of these clips";
        if (!reports.empty() && !reports.front().skipped.empty()) {
            why += " (" + reports.front().skipped.front() + ")";
        }
        return std::unexpected(invalidArgument(why));
    }
    EditResult edited = ed.finish();
    PresetResult result;
    result.edit.changes = diff(state, edited.state);
    result.edit.state = std::move(edited.state);
    result.edit.createdEffects = std::move(edited.createdEffects);
    result.clips = std::move(reports);
    return result;
}

} // namespace edit

// ---- JSON ------------------------------------------------------------------------------------------------------------------

namespace io {

namespace {

using detail::json;
using detail::Reader;

std::expected<json, Error> parseBounded(std::string_view text, const char* what) {
    if (text.size() > kMaxEffectsLibraryBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + " is larger than 16 MiB"});
    }
    if (!jsonNestingWithin(text)) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + " nests too deep"});
    }
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + " is not valid JSON (UTF-8)"});
    }
    return doc;
}

std::string dumped(const json& j) { return j.dump(2, ' ', false, json::error_handler_t::replace); }

json presetJson(const EffectPreset& p) {
    json out{{"name", p.name}, {"type", std::string(toString(p.type))}, {"duration", p.duration.value},
             {"effects", detail::effectStackToJsonValue(p.effects)}};
    if (!p.description.empty()) {
        out["description"] = p.description;
    }
    if (!p.bin.empty()) {
        out["bin"] = p.bin;
    }
    return out;
}

EffectPreset readPreset(const Reader& r) {
    if (!r.value().is_object()) {
        r.fail("a preset is an object");
    }
    EffectPreset p;
    p.name = r.child("name").asString();
    p.description = r.string("description");
    const std::string type = r.string("type", "scale");
    const auto t = effectPresetTypeFromString(type);
    if (!t) {
        r.child("type").fail("unknown preset type \"" + type + "\"");
    }
    p.type = *t;
    p.bin = r.string("bin");
    p.duration = r.ticks("duration");
    const Reader effects = r.child("effects");
    if (effects.arraySize() > kMaxPresetEffects) {
        effects.fail("more than " + std::to_string(kMaxPresetEffects) + " effects");
    }
    p.effects = detail::effectStackFromJsonValue(effects);
    if (const auto problem = effectPresetProblem(p)) {
        r.fail(*problem);
    }
    return p;
}

std::vector<EffectPreset> readPresets(const Reader& r) {
    if (r.arraySize() > kMaxEffectPresets) {
        r.fail("more than " + std::to_string(kMaxEffectPresets) + " presets");
    }
    std::vector<EffectPreset> out;
    for (std::size_t i = 0; i < r.arraySize(); ++i) {
        out.push_back(readPreset(r.element(i)));
    }
    return out;
}

std::vector<std::string> readStrings(const Reader& r, std::size_t max) {
    if (r.arraySize() > max) {
        r.fail("more than " + std::to_string(max) + " entries");
    }
    std::vector<std::string> out;
    for (std::size_t i = 0; i < r.arraySize(); ++i) {
        out.push_back(r.element(i).asString());
    }
    return out;
}

/// Checks the {"format", "version"} header of a file.
std::optional<Error> headerProblem(const json& file, std::string_view format, const char* what) {
    if (!file.is_object() || !file.contains("format") || !file.at("format").is_string() ||
        file.at("format").get<std::string>() != format) {
        return Error{ErrorCode::ParseError, std::string("not a Wabcut ") + what};
    }
    if (!file.contains("version") || !file.at("version").is_number_integer() || file.at("version").get<std::int64_t>() < 1) {
        return Error{ErrorCode::ParseError, std::string(what) + ": a bad version"};
    }
    if (file.at("version").get<std::int64_t>() > kEffectsLibraryVersion) {
        return Error{ErrorCode::VersionMismatch, std::string("the ") + what + " was saved by a newer Wabcut"};
    }
    return std::nullopt;
}

template <class T, class F>
std::expected<T, Error> guarded(const char* what, F&& read) {
    try {
        return read();
    } catch (const detail::FormatError& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + ": " + e.message});
    } catch (const json::exception& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + ": " + e.what()});
    }
}

} // namespace

std::string effectPresetToJson(const EffectPreset& preset) { return dumped(presetJson(preset)); }

std::expected<EffectPreset, Error> effectPresetFromJson(std::string_view text) {
    auto doc = parseBounded(text, "preset");
    if (!doc) {
        return std::unexpected(doc.error());
    }
    return guarded<EffectPreset>("preset", [&] { return readPreset(Reader(*doc, "")); });
}

std::string effectsLibraryToJson(const EffectsLibrary& library) {
    json presets = json::array();
    for (const EffectPreset& p : library.presets) {
        presets.push_back(presetJson(p));
    }
    json bins = json::array();
    for (const EffectsCustomBin& b : library.customBins) {
        bins.push_back(json{{"name", b.name}, {"items", b.items}});
    }
    return dumped(json{{"format", std::string(kEffectsLibraryFormat)},
                       {"version", kEffectsLibraryVersion},
                       {"presets", std::move(presets)},
                       {"presetBins", library.presetBins},
                       {"customBins", std::move(bins)},
                       {"favorites", library.favorites}});
}

std::expected<EffectsLibrary, Error> effectsLibraryFromJson(std::string_view text) {
    auto doc = parseBounded(text, "effects library");
    if (!doc) {
        return std::unexpected(doc.error());
    }
    if (auto problem = headerProblem(*doc, kEffectsLibraryFormat, "effects library")) {
        return std::unexpected(std::move(*problem));
    }
    return guarded<EffectsLibrary>("effects library", [&] {
        const Reader r(*doc, "");
        EffectsLibrary library;
        if (r.has("presets")) {
            library.presets = readPresets(r.child("presets"));
        }
        if (r.has("presetBins")) {
            library.presetBins = readStrings(r.child("presetBins"), kMaxPresetBins);
        }
        if (r.has("customBins")) {
            const Reader bins = r.child("customBins");
            if (bins.arraySize() > kMaxCustomBins) {
                bins.fail("more than " + std::to_string(kMaxCustomBins) + " custom bins");
            }
            for (std::size_t i = 0; i < bins.arraySize(); ++i) {
                const Reader b = bins.element(i);
                EffectsCustomBin bin;
                bin.name = b.child("name").asString();
                if (b.has("items")) {
                    bin.items = readStrings(b.child("items"), kMaxCustomBinItems);
                }
                library.customBins.push_back(std::move(bin));
            }
        }
        if (r.has("favorites")) {
            library.favorites = readStrings(r.child("favorites"), kMaxFavorites);
        }
        sortEffectsLibrary(library);
        if (const auto problem = effectsLibraryProblem(library)) {
            r.fail(*problem);
        }
        return library;
    });
}

std::string effectPresetsFileToJson(std::span<const EffectPreset> presets) {
    json list = json::array();
    for (const EffectPreset& p : presets) {
        list.push_back(presetJson(p));
    }
    return dumped(json{{"format", std::string(kEffectPresetsFileFormat)}, {"version", kEffectsLibraryVersion}, {"presets", std::move(list)}});
}

std::expected<std::vector<EffectPreset>, Error> effectPresetsFileFromJson(std::string_view text) {
    auto doc = parseBounded(text, "presets file");
    if (!doc) {
        return std::unexpected(doc.error());
    }
    if (auto problem = headerProblem(*doc, kEffectPresetsFileFormat, "presets file")) {
        return std::unexpected(std::move(*problem));
    }
    return guarded<std::vector<EffectPreset>>("presets file", [&] {
        const Reader r(*doc, "");
        std::vector<EffectPreset> presets = readPresets(r.child("presets"));
        for (std::size_t i = 0; i < presets.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (samePresetName(presets[i].name, presets[j].name)) {
                    r.child("presets").fail("two presets are named '" + presets[i].name + "'");
                }
            }
        }
        return presets;
    });
}

} // namespace io

} // namespace wabcut
