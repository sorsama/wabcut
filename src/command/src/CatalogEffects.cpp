// Built-in effects library commands (0.2, docs/effects.md "Presets, custom bins and favourites"): Premiere's effect
// presets (Save Preset, the Presets bin, Preset Properties, Import / Export Presets), the Effects panel's custom bins
// and favourites. Model: EffectPresets.h. The library is the host's EffectLibraryServices (ExecutionContext.h): the
// user's, not the project's, so saving a preset is not an undo step; changes reach it only once the request has
// succeeded (CommandOutput::afterCommit), and later commands of the same batch see them (the stage).

#include "Catalog.h"

#include <wabcut/model/EffectPresets.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

constexpr const char* kStageKey = "effectsLibrary";

EffectLibraryServices& libraryServices(ExecutionContext& c) {
    auto* services = findService<EffectLibraryServices>(c.services());
    if (!services) {
        fail(ErrorCode::InvalidRequest, "this host has no effects library");
    }
    return *services;
}

/// The library as this request sees it: what an earlier step staged, else the host's.
EffectsLibrary heldLibrary(ExecutionContext& c) {
    if (const ExecutionContext::Stage* stage = c.stage()) {
        if (const auto it = stage->find(kStageKey); it != stage->end()) {
            return *std::static_pointer_cast<const EffectsLibrary>(it->second);
        }
    }
    return libraryServices(c).library();
}

/// A changed library: checked now, handed to the host once the request has committed (never for a dry run).
CommandOutput libraryChanged(ExecutionContext& c, EffectsLibrary library, Json data) {
    sortEffectsLibrary(library);
    if (const auto problem = effectsLibraryProblem(library)) {
        fail(ErrorCode::InvalidParams, *problem);
    }
    EffectLibraryServices* services = &libraryServices(c);
    auto held = std::make_shared<const EffectsLibrary>(std::move(library));
    if (ExecutionContext::Stage* stage = c.stage()) {
        (*stage)[kStageKey] = held;
    }
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    out.afterCommit.push_back([services, held] { services->set(*held); });
    return out;
}

CommandOutput readOnly(ExecutionContext& c, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    return out;
}

/// The frame built-in presets are made for: the named sequence's, the project's only one, else 1920 x 1080.
std::pair<std::int32_t, std::int32_t> frameOf(ExecutionContext& c) {
    if (c.has("sequence") || c.state().sequences.size() == 1) {
        const Sequence& s = c.sequence();
        return {s.width, s.height};
    }
    return {1920, 1080};
}

struct FoundPreset {
    EffectPreset preset;
    bool builtIn = false;
};

std::optional<FoundPreset> lookupPreset(ExecutionContext& c, const EffectsLibrary& library, std::string_view name) {
    if (const EffectPreset* p = library.findPreset(name)) {
        return FoundPreset{*p, false};
    }
    const auto [w, h] = frameOf(c);
    if (auto b = findBuiltinEffectPreset(name, w, h)) {
        return FoundPreset{std::move(*b), true};
    }
    return std::nullopt;
}

FoundPreset presetNamed(ExecutionContext& c, const EffectsLibrary& library, const std::string& name, const char* param) {
    auto found = lookupPreset(c, library, name);
    if (!found) {
        fail(ErrorCode::NotFound, "no preset named '" + name + "'", Json{{"path", std::string("/") + param}});
    }
    return std::move(*found);
}

EffectPreset& userPreset(ExecutionContext& c, EffectsLibrary& library, const std::string& name, const char* param) {
    const auto it = std::find_if(library.presets.begin(), library.presets.end(),
                                 [&](const EffectPreset& p) { return samePresetName(p.name, name); });
    if (it == library.presets.end()) {
        const auto [w, h] = frameOf(c);
        fail(findBuiltinEffectPreset(name, w, h) ? ErrorCode::EditRejected : ErrorCode::NotFound,
             findBuiltinEffectPreset(name, w, h) ? "'" + name + "' is a built-in preset and cannot be changed"
                                                 : "no preset of yours is named '" + name + "'",
             Json{{"path", std::string("/") + param}});
    }
    return *it;
}

std::string effectDisplayName(const EffectInstance& e) {
    if (e.effect == kMotionEffect) {
        return "Motion";
    }
    if (const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect)) {
        return std::string(d->displayName);
    }
    return e.effect;
}

Json presetJson(const EffectPreset& p, bool builtIn) {
    Json effects = Json::array();
    for (const EffectInstance& e : p.effects) {
        effects.push_back(Json{{"effect", e.effect}, {"name", effectDisplayName(e)}});
    }
    const std::string top = builtIn && isLumetriPreset(p) ? "Lumetri Presets" : "Presets";
    std::string bin = p.bin;
    if (bin == "Lumetri Presets" || bin.starts_with("Lumetri Presets/")) {
        bin = bin.size() > 16 ? bin.substr(16) : std::string();
    }
    return Json{{"name", p.name},
                {"type", std::string(toString(p.type))},
                {"typeName", std::string(displayName(p.type))},
                {"description", p.description},
                {"group", top},
                {"bin", bin},
                {"kind", std::string(toString(p.kind()))},
                {"builtIn", builtIn},
                {"duration", p.duration.value},
                {"effects", std::move(effects)},
                {"item", std::string(kPresetItemPrefix) + p.name}};
}

/// What an item reference stands for: its display name and kind, or "missing" (a preset that was deleted, an effect
/// this computer does not have).
Json itemJson(ExecutionContext& c, const EffectsLibrary& library, const std::string& item) {
    Json out{{"item", item}};
    if (item.starts_with(kEffectItemPrefix)) {
        const std::string id = item.substr(kEffectItemPrefix.size());
        out["type"] = "effect";
        out["id"] = id;
        if (const EffectDescriptor* d = EffectRegistry::builtin().find(id)) {
            out["name"] = std::string(d->displayName);
            out["kind"] = std::string(toString(d->kind));
            out["category"] = std::string(d->category);
        } else {
            out["name"] = id;
            out["missing"] = true;
        }
    } else {
        const std::string name = item.substr(kPresetItemPrefix.size());
        out["type"] = "preset";
        out["name"] = name;
        if (const auto found = lookupPreset(c, library, name)) {
            out["kind"] = std::string(toString(found->preset.kind()));
            out["builtIn"] = found->builtIn;
        } else {
            out["missing"] = true;
        }
    }
    return out;
}

/// An item reference from a param: "effect:<id>", "preset:<name>", or a bare effect id / preset name (an effect id
/// first). Fails with NotFound when it names nothing.
std::string itemReference(ExecutionContext& c, const EffectsLibrary& library, const std::string& text, const std::string& path) {
    if (text.starts_with(kEffectItemPrefix)) {
        if (!EffectRegistry::builtin().find(text.substr(kEffectItemPrefix.size()))) {
            fail(ErrorCode::NotFound, "no effect '" + text.substr(kEffectItemPrefix.size()) + "'", Json{{"path", path}});
        }
        return text;
    }
    if (text.starts_with(kPresetItemPrefix)) {
        const auto found = lookupPreset(c, library, text.substr(kPresetItemPrefix.size()));
        if (!found) {
            fail(ErrorCode::NotFound, "no preset named '" + text.substr(kPresetItemPrefix.size()) + "'", Json{{"path", path}});
        }
        return std::string(kPresetItemPrefix) + found->preset.name;
    }
    if (const EffectDescriptor* d = EffectRegistry::builtin().find(text); d && !d->intrinsic) {
        return std::string(kEffectItemPrefix) + text;
    }
    if (const auto found = lookupPreset(c, library, text)) {
        return std::string(kPresetItemPrefix) + found->preset.name;
    }
    fail(ErrorCode::NotFound, "'" + text + "' is neither an effect id nor a preset", Json{{"path", path}});
}

/// Renames item references to a preset everywhere (custom bins, favourites); an empty `to` removes them.
void retargetPresetItems(EffectsLibrary& library, const std::string& from, const std::string& to) {
    const std::string old = std::string(kPresetItemPrefix) + from;
    const std::string now = to.empty() ? std::string() : std::string(kPresetItemPrefix) + to;
    const auto fix = [&](std::vector<std::string>& items) {
        for (std::string& item : items) {
            if (samePresetName(item, old)) {
                item = now;
            }
        }
        std::erase(items, std::string());
    };
    for (EffectsCustomBin& b : library.customBins) {
        fix(b.items);
    }
    fix(library.favorites);
}

/// Whether a presets bin path is `bin` or inside it.
bool inBin(std::string_view path, std::string_view bin) {
    return samePresetName(path, bin) || (path.size() > bin.size() && path[bin.size()] == '/' && samePresetName(path.substr(0, bin.size()), bin));
}

std::string checkedBin(const std::string& bin, const char* param) {
    if (const auto problem = presetBinProblem(bin)) {
        fail(ErrorCode::InvalidParams, *problem, Json{{"path", std::string("/") + param}});
    }
    if (bin == "Lumetri Presets" || bin.starts_with("Lumetri Presets/")) {
        fail(ErrorCode::InvalidParams, "the Lumetri Presets bin holds the built-in looks only", Json{{"path", std::string("/") + param}});
    }
    return bin;
}

void requireFreeName(ExecutionContext& c, const EffectsLibrary& library, const std::string& name, const char* param,
                     std::string_view except = {}) {
    if (!except.empty() && samePresetName(name, except)) {
        return;
    }
    const auto [w, h] = frameOf(c);
    if (findBuiltinEffectPreset(name, w, h)) {
        fail(ErrorCode::EditRejected, "'" + name + "' is the name of a built-in preset", Json{{"path", std::string("/") + param}});
    }
    if (library.findPreset(name)) {
        fail(ErrorCode::EditRejected, "a preset named '" + name + "' exists", Json{{"path", std::string("/") + param}});
    }
}

EffectPresetType typeParam(ExecutionContext& c, EffectPresetType fallback) {
    if (!c.has("type")) {
        return fallback;
    }
    const auto t = effectPresetTypeFromString(c.string("type"));
    if (!t) {
        fail(ErrorCode::InvalidParams, "unknown preset type", Json{{"path", "/type"}});
    }
    return *t;
}

Json typeSchema() {
    return schema::enumeration({"scale", "anchorToInPoint", "anchorToOutPoint"},
                               "Premiere's preset type: how the keyframes land on a clip of another length. \"scale\" "
                               "(default): stretched with the clip; \"anchorToInPoint\": the same distance from its In "
                               "point; \"anchorToOutPoint\": the same distance from its Out point.");
}

Json nameSchema(std::string description) {
    Json s = schema::string(std::move(description));
    s["minLength"] = 1;
    s["maxLength"] = static_cast<std::int64_t>(kMaxPresetNameBytes);
    return s;
}

std::filesystem::path presetsFilePath(ExecutionContext& c) {
    const std::string text = c.string("path");
    if (text.empty()) {
        fail(ErrorCode::InvalidParams, "a file path is needed", Json{{"path", "/path"}});
    }
    return pathFromUtf8(text);
}

CommandOutput libraryList(ExecutionContext& c) {
    EffectLibraryServices& services = libraryServices(c);
    const EffectsLibrary library = heldLibrary(c);
    const auto [w, h] = frameOf(c);
    Json presets = Json::array();
    for (const EffectPreset& p : builtinEffectPresets(w, h)) {
        presets.push_back(presetJson(p, true));
    }
    for (const EffectPreset& p : library.presets) {
        presets.push_back(presetJson(p, false));
    }
    Json bins = Json::array();
    for (const EffectsCustomBin& b : library.customBins) {
        Json items = Json::array();
        for (const std::string& item : b.items) {
            items.push_back(itemJson(c, library, item));
        }
        bins.push_back(Json{{"name", b.name}, {"items", std::move(items)}});
    }
    Json favorites = Json::array();
    for (const std::string& item : library.favorites) {
        favorites.push_back(itemJson(c, library, item));
    }
    const std::string error = services.error();
    return readOnly(c, Json{{"presets", std::move(presets)},
                            {"presetBins", library.presetBins},
                            {"customBins", std::move(bins)},
                            {"favorites", std::move(favorites)},
                            {"file", services.file().empty() ? Json(nullptr) : Json(pathToUtf8(services.file()))},
                            {"error", error.empty() ? Json(nullptr) : Json(error)}});
}

CommandOutput savePreset(ExecutionContext& c) {
    const ClipId clipId = c.clip("clip");
    const auto loc = c.sequence().findClip(clipId);
    if (!loc) {
        fail(ErrorCode::NotFound, "no clip " + std::to_string(clipId.value), Json{{"path", "/clip"}});
    }
    edit::PresetSelection selection;
    if (c.has("effects")) {
        for (std::size_t i = 0; i < c.params()["effects"].size(); ++i) {
            selection.effects.push_back(c.indexValue(c.params()["effects"][i], "/effects/" + std::to_string(i)));
        }
    }
    if (c.has("fixed")) {
        for (const Json& f : c.params()["fixed"]) {
            selection.fixed.push_back("wabcut." + f.get<std::string>());
        }
    }
    EffectsLibrary library = heldLibrary(c);
    const std::string name = c.string("name");
    const bool replace = c.boolean("replace", false);
    if (!replace || !library.findPreset(name)) {
        requireFreeName(c, library, name, "name");
    }
    std::vector<std::string> notes;
    auto preset = edit::makeEffectPreset(c.state(), loc->clip(), selection, name, typeParam(c, EffectPresetType::Scale),
                                         c.string("description"), checkedBin(c.string("bin"), "bin"), &notes);
    if (!preset) {
        fail(ErrorCode::InvalidParams, preset.error().message);
    }
    std::erase_if(library.presets, [&](const EffectPreset& p) { return samePresetName(p.name, name); });
    if (library.presets.size() >= kMaxEffectPresets) {
        fail(ErrorCode::EditRejected, "the library holds the most presets it can (" + std::to_string(kMaxEffectPresets) + ")");
    }
    Json effects = Json::array();
    for (const EffectInstance& e : preset->effects) {
        effects.push_back(effectDisplayName(e));
    }
    Json data{{"preset", preset->name}, {"type", std::string(toString(preset->type))}, {"effects", std::move(effects)},
              {"notes", notes}};
    library.presets.push_back(std::move(*preset));
    return libraryChanged(c, std::move(library), std::move(data));
}

CommandOutput applyPreset(ExecutionContext& c) {
    const EffectsLibrary library = heldLibrary(c);
    const FoundPreset found = presetNamed(c, library, c.string("preset"), "preset");
    edit::PresetResult result = c.take(edit::applyEffectPreset(c.state(), c.sequenceId(), c.clips("clips"), found.preset));
    Json clips = Json::array();
    for (const edit::PresetClipReport& r : result.clips) {
        clips.push_back(Json{{"clip", r.clip.value}, {"applied", r.applied}, {"skipped", r.skipped}});
    }
    return edited(std::move(result.edit), Json{{"preset", found.preset.name}, {"builtIn", found.builtIn}, {"clips", std::move(clips)}});
}

CommandOutput setPresetProperties(ExecutionContext& c) {
    EffectsLibrary library = heldLibrary(c);
    const std::string current = c.string("preset");
    EffectPreset& p = userPreset(c, library, current, "preset");
    const std::string oldName = p.name;
    if (c.has("name")) {
        const std::string name = c.string("name");
        requireFreeName(c, library, name, "name", oldName);
        p.name = name;
    }
    p.type = typeParam(c, p.type);
    if (c.has("description")) {
        p.description = c.string("description");
    }
    if (c.has("bin")) {
        p.bin = checkedBin(c.string("bin"), "bin");
    }
    if (const auto problem = effectPresetProblem(p)) {
        fail(ErrorCode::InvalidParams, *problem);
    }
    const std::string newName = p.name;
    Json data{{"preset", newName}, {"type", std::string(toString(p.type))}, {"bin", p.bin}};
    if (newName != oldName) {
        retargetPresetItems(library, oldName, newName);
    }
    return libraryChanged(c, std::move(library), std::move(data));
}

CommandOutput newPresetsBin(ExecutionContext& c) {
    EffectsLibrary library = heldLibrary(c);
    const std::string bin = checkedBin(c.string("bin"), "bin");
    if (bin.empty()) {
        fail(ErrorCode::InvalidParams, "name the bin", Json{{"path", "/bin"}});
    }
    // A bin inside a bin makes its parents too.
    for (std::size_t slash = bin.find('/'); ; slash = bin.find('/', slash + 1)) {
        const std::string path = bin.substr(0, slash);
        if (std::none_of(library.presetBins.begin(), library.presetBins.end(), [&](const std::string& b) { return samePresetName(b, path); })) {
            if (library.presetBins.size() >= kMaxPresetBins) {
                fail(ErrorCode::EditRejected, "the library holds the most presets bins it can");
            }
            library.presetBins.push_back(path);
        }
        if (slash == std::string::npos) {
            break;
        }
    }
    return libraryChanged(c, std::move(library), Json{{"bin", bin}});
}

/// "Custom Bin 01", "Custom Bin 02", ... as Premiere names new custom bins.
std::string nextCustomBinName(const EffectsLibrary& library) {
    for (int n = 1;; ++n) {
        std::string name = "Custom Bin " + std::string(n < 10 ? "0" : "") + std::to_string(n);
        if (!library.findCustomBin(name)) {
            return name;
        }
    }
}

CommandOutput newCustomBin(ExecutionContext& c) {
    EffectsLibrary library = heldLibrary(c);
    const std::string name = c.has("name") ? c.string("name") : nextCustomBinName(library);
    if (library.findCustomBin(name)) {
        fail(ErrorCode::EditRejected, "a custom bin named '" + name + "' exists", Json{{"path", "/name"}});
    }
    if (library.customBins.size() >= kMaxCustomBins) {
        fail(ErrorCode::EditRejected, "the library holds the most custom bins it can (" + std::to_string(kMaxCustomBins) + ")");
    }
    EffectsCustomBin bin{name, {}};
    if (c.has("items")) {
        for (std::size_t i = 0; i < c.params()["items"].size(); ++i) {
            const std::string item = itemReference(c, library, c.params()["items"][i].get<std::string>(), "/items/" + std::to_string(i));
            if (std::find(bin.items.begin(), bin.items.end(), item) == bin.items.end()) {
                bin.items.push_back(item);
            }
        }
    }
    library.customBins.push_back(std::move(bin));
    return libraryChanged(c, std::move(library), Json{{"customBin", name}});
}

CommandOutput addToCustomBin(ExecutionContext& c) {
    EffectsLibrary library = heldLibrary(c);
    const std::string name = c.string("customBin");
    const auto it = std::find_if(library.customBins.begin(), library.customBins.end(),
                                 [&](const EffectsCustomBin& b) { return samePresetName(b.name, name); });
    if (it == library.customBins.end()) {
        fail(ErrorCode::NotFound, "no custom bin named '" + name + "'", Json{{"path", "/customBin"}});
    }
    Json added = Json::array();
    for (std::size_t i = 0; i < c.params()["items"].size(); ++i) {
        const std::string item = itemReference(c, library, c.params()["items"][i].get<std::string>(), "/items/" + std::to_string(i));
        if (std::find(it->items.begin(), it->items.end(), item) == it->items.end()) {
            it->items.push_back(item);
            added.push_back(item);
        }
    }
    const std::string binName = it->name;
    return libraryChanged(c, std::move(library), Json{{"customBin", binName}, {"added", std::move(added)}});
}

CommandOutput renameCustomItem(ExecutionContext& c) {
    EffectsLibrary library = heldLibrary(c);
    const std::string name = c.string("name");
    if (c.has("customBin")) {
        const std::string from = c.string("customBin");
        const auto it = std::find_if(library.customBins.begin(), library.customBins.end(),
                                     [&](const EffectsCustomBin& b) { return samePresetName(b.name, from); });
        if (it == library.customBins.end()) {
            fail(ErrorCode::NotFound, "no custom bin named '" + from + "'", Json{{"path", "/customBin"}});
        }
        if (!samePresetName(from, name) && library.findCustomBin(name)) {
            fail(ErrorCode::EditRejected, "a custom bin named '" + name + "' exists", Json{{"path", "/name"}});
        }
        it->name = name;
        return libraryChanged(c, std::move(library), Json{{"customBin", name}});
    }
    if (c.has("presetsBin")) {
        const std::string from = checkedBin(c.string("presetsBin"), "presetsBin");
        if (name.find('/') != std::string::npos) {
            fail(ErrorCode::InvalidParams, "a bin's name has no '/'", Json{{"path", "/name"}});
        }
        const std::size_t cut = from.rfind('/');
        const std::string to = (cut == std::string::npos ? std::string() : from.substr(0, cut + 1)) + name;
        checkedBin(to, "name");
        bool found = false;
        const auto move = [&](std::string& path) {
            if (inBin(path, from)) {
                path = to + path.substr(from.size());
                found = true;
            }
        };
        for (std::string& b : library.presetBins) {
            move(b);
        }
        for (EffectPreset& p : library.presets) {
            move(p.bin);
        }
        if (!found) {
            fail(ErrorCode::NotFound, "no presets bin '" + from + "'", Json{{"path", "/presetsBin"}});
        }
        return libraryChanged(c, std::move(library), Json{{"presetsBin", to}});
    }
    if (c.has("preset")) {
        EffectPreset& p = userPreset(c, library, c.string("preset"), "preset");
        const std::string old = p.name;
        requireFreeName(c, library, name, "name", old);
        p.name = name;
        retargetPresetItems(library, old, name);
        return libraryChanged(c, std::move(library), Json{{"preset", name}});
    }
    fail(ErrorCode::InvalidParams, "name the item: \"customBin\", \"presetsBin\" or \"preset\"");
}

CommandOutput deleteCustomItem(ExecutionContext& c) {
    EffectsLibrary library = heldLibrary(c);
    Json removed = Json::array();
    if (c.has("preset")) {
        const std::string asked = c.string("preset");
        const std::string name = userPreset(c, library, asked, "preset").name;
        std::erase_if(library.presets, [&](const EffectPreset& q) { return samePresetName(q.name, name); });
        retargetPresetItems(library, name, {});
        removed.push_back(std::string(kPresetItemPrefix) + name);
    } else if (c.has("presetsBin")) {
        const std::string bin = checkedBin(c.string("presetsBin"), "presetsBin");
        if (bin.empty()) {
            fail(ErrorCode::InvalidParams, "the Presets bin itself cannot be deleted", Json{{"path", "/presetsBin"}});
        }
        const bool listed = std::any_of(library.presetBins.begin(), library.presetBins.end(), [&](const std::string& b) { return inBin(b, bin); });
        const bool used = std::any_of(library.presets.begin(), library.presets.end(), [&](const EffectPreset& p) { return inBin(p.bin, bin); });
        if (!listed && !used) {
            fail(ErrorCode::NotFound, "no presets bin '" + bin + "'", Json{{"path", "/presetsBin"}});
        }
        // The bin goes with what is in it (Premiere asks first; the app does).
        std::vector<std::string> gone;
        for (const EffectPreset& p : library.presets) {
            if (inBin(p.bin, bin)) {
                gone.push_back(p.name);
            }
        }
        for (const std::string& name : gone) {
            std::erase_if(library.presets, [&](const EffectPreset& q) { return q.name == name; });
            retargetPresetItems(library, name, {});
            removed.push_back(std::string(kPresetItemPrefix) + name);
        }
        std::erase_if(library.presetBins, [&](const std::string& b) { return inBin(b, bin); });
    } else if (c.has("customBin")) {
        const std::string name = c.string("customBin");
        const auto it = std::find_if(library.customBins.begin(), library.customBins.end(),
                                     [&](const EffectsCustomBin& b) { return samePresetName(b.name, name); });
        if (it == library.customBins.end()) {
            fail(ErrorCode::NotFound, "no custom bin named '" + name + "'", Json{{"path", "/customBin"}});
        }
        if (c.has("item")) {
            const std::string item = c.string("item");
            const auto at = std::find_if(it->items.begin(), it->items.end(), [&](const std::string& i) {
                return i == item || i == std::string(kEffectItemPrefix) + item || samePresetName(i, std::string(kPresetItemPrefix) + item);
            });
            if (at == it->items.end()) {
                fail(ErrorCode::NotFound, "custom bin '" + it->name + "' has no item '" + item + "'", Json{{"path", "/item"}});
            }
            removed.push_back(*at);
            it->items.erase(at);
        } else {
            removed.push_back(it->name);
            library.customBins.erase(it);
        }
    } else {
        fail(ErrorCode::InvalidParams, "name what to delete: \"preset\", \"presetsBin\" or \"customBin\" (with \"item\")");
    }
    return libraryChanged(c, std::move(library), Json{{"removed", std::move(removed)}});
}

CommandOutput setFavorite(ExecutionContext& c) {
    EffectsLibrary library = heldLibrary(c);
    const std::string item = itemReference(c, library, c.string("item"), "/item");
    const bool favorite = c.boolean("favorite", true);
    std::erase(library.favorites, item);
    if (favorite) {
        if (library.favorites.size() >= kMaxFavorites) {
            fail(ErrorCode::EditRejected, "the library holds the most favourites it can");
        }
        library.favorites.push_back(item);
    }
    return libraryChanged(c, std::move(library), Json{{"item", item}, {"favorite", favorite}});
}

CommandOutput exportPresets(ExecutionContext& c) {
    const EffectsLibrary library = heldLibrary(c);
    std::vector<EffectPreset> chosen;
    Json names = Json::array();
    if (c.has("presets")) {
        for (std::size_t i = 0; i < c.params()["presets"].size(); ++i) {
            FoundPreset found = presetNamed(c, library, c.params()["presets"][i].get<std::string>(), "presets");
            names.push_back(found.preset.name);
            chosen.push_back(std::move(found.preset));
        }
    }
    if (c.has("presetsBin")) {
        const std::string bin = c.string("presetsBin");
        for (const EffectPreset& p : library.presets) {
            if (bin.empty() || inBin(p.bin, bin)) {
                names.push_back(p.name);
                chosen.push_back(p);
            }
        }
    }
    if (chosen.empty()) {
        fail(ErrorCode::InvalidParams, "choose the presets to export (\"presets\" or \"presetsBin\")");
    }
    const std::filesystem::path file = presetsFilePath(c);
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && !c.boolean("overwrite", false)) {
        fail(ErrorCode::EditRejected, "the file exists; pass \"overwrite\": true to replace it", Json{{"path", "/path"}});
    }
    if (!c.dryRun()) {
        if (auto written = io::writeFileAtomically(file, io::effectPresetsFileToJson(chosen)); !written) {
            fail(ErrorCode::EditRejected, "cannot write the presets: " + written.error().message, Json{{"path", "/path"}});
        }
    }
    return readOnly(c, Json{{"path", pathToUtf8(file)}, {"presets", std::move(names)}});
}

CommandOutput importPresets(ExecutionContext& c) {
    const std::filesystem::path file = presetsFilePath(c);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        fail(ErrorCode::NotFound, "the file cannot be read", Json{{"path", "/path"}});
    }
    if (size > kMaxEffectsLibraryBytes) {
        fail(ErrorCode::InvalidParams, "the file is larger than 16 MiB", Json{{"path", "/path"}});
    }
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    auto presets = io::effectPresetsFileFromJson(text);
    if (!presets) {
        fail(presets.error().code == wabcut::ErrorCode::VersionMismatch ? ErrorCode::EditRejected : ErrorCode::InvalidParams,
             "the presets file cannot be read: " + presets.error().message, Json{{"path", "/path"}});
    }
    EffectsLibrary library = heldLibrary(c);
    const std::string into = c.has("bin") ? checkedBin(c.string("bin"), "bin") : std::string();
    const auto [w, h] = frameOf(c);
    Json imported = Json::array();
    for (EffectPreset& p : *presets) {
        // A name in use gets a number, as Premiere's bins do with duplicates ("Glow 2").
        const std::string base = p.name;
        for (int n = 2; library.findPreset(p.name) || findBuiltinEffectPreset(p.name, w, h); ++n) {
            p.name = base + " " + std::to_string(n);
        }
        if (c.has("bin")) {
            p.bin = into;
        }
        if (library.presets.size() >= kMaxEffectPresets) {
            fail(ErrorCode::EditRejected, "the library holds the most presets it can (" + std::to_string(kMaxEffectPresets) + ")");
        }
        imported.push_back(Json{{"name", p.name}, {"from", base}});
        library.presets.push_back(std::move(p));
    }
    return libraryChanged(c, std::move(library), Json{{"path", pathToUtf8(file)}, {"imported", std::move(imported)}});
}

} // namespace

void registerEffectsLibraryCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"effects.library", 1, "Effects Library",
                "The Effects panel's presets and the user's items: every preset (the built-in Presets and Lumetri Presets "
                "bins, and the user's presets with their presets bins; name, type, description, bin, kind, effects), the "
                "user's presets bins, custom bins (their items: effects, transitions and presets) and favourites. "
                "Built-in presets are made for the sequence's frame size (PiP positions). The library is the user's, kept "
                "by the host (\"file\"; \"error\" when it could not be read or written).",
                Capability::Read, withSequence({}), Json::object(), {}, libraryList});
    addBuiltin(
        registry,
        {"effects.savePreset", 1, "Save Effect Preset",
         "Saves effects of a clip as a preset in the user's library (Premiere's Effect Controls > Save Preset): stack "
         "effects by index and the clip's fixed effects (motion, opacity with its blend mode, volume, panner), with their "
         "keyframes and the preset type that times them on other clips. LUT files are referred to by path, tracked "
         "matte masks are left out (\"notes\"). Not an undo step: the library is the user's.",
         Capability::EditProject,
         withSequence({{"clip", clipParam("The clip whose effects to save.")},
                       {"effects", schema::array(schema::integer("An index in the clip's effect stack (0 = first).", 0),
                                                 "Stack effects to save.")},
                       {"fixed", schema::array(schema::enumeration({"motion", "opacity", "volume", "panner"}, "A fixed effect."),
                                               "Fixed effects to save (Motion and Opacity of a video clip, Volume and "
                                               "Panner of an audio clip).")},
                       {"name", nameSchema("The preset's name: unique among the user's presets, not a built-in's.")},
                       {"type", typeSchema()},
                       {"description", schema::string("A description (Premiere's Description field).")},
                       {"bin", schema::string("The presets bin it goes in, e.g. \"Lower thirds/Blue\" (default: the Presets bin).")},
                       {"replace", schema::boolean("Replace a preset of yours with that name (default false: refused).")}},
                      {"clip", "name"}),
         Json{{"clip", 42}, {"effects", Json::array({0})}, {"fixed", Json::array({"motion"})}, {"name", "Slow push"},
              {"type", "anchorToInPoint"}},
         {}, savePreset});
    addBuiltin(
        registry,
        {"effects.applyPreset", 1, "Apply Preset",
         "Applies a preset to clips (double-click or drag from the Effects panel's Presets bin): its stack effects are added "
         "after each clip's own, its fixed effects replace the clip's Motion / Opacity / Volume / Panner values (keyframes "
         "included), and keyframes are timed by its type. Clips of the other kind (a video preset on audio) are skipped "
         "and reported. Built-in presets (effects.library) and the user's.",
         Capability::EditSequence,
         withSequence({{"preset", nameSchema("The preset's name (case-insensitive).")}, {"clips", clipsParam("The clips.")}},
                      {"preset", "clips"}),
         Json{{"preset", "Twirl In"}, {"clips", Json::array({42})}}, {}, applyPreset});
    addBuiltin(registry,
               {"effects.setPresetProperties", 1, "Preset Properties",
                "Changes a user preset (Premiere's Preset Properties): its name (custom bins and favourites follow), type, "
                "description, or the presets bin it is in.",
                Capability::EditProject,
                withSequence({{"preset", nameSchema("The preset.")},
                              {"name", nameSchema("A new name.")},
                              {"type", typeSchema()},
                              {"description", schema::string("A new description.")},
                              {"bin", schema::string("The presets bin to move it to (\"\" = the Presets bin).")}},
                             {"preset"}),
                Json{{"preset", "Slow push"}, {"type", "scale"}}, {}, setPresetProperties});
    addBuiltin(registry,
               {"effects.newPresetsBin", 1, "New Presets Bin",
                "Makes a bin in the Presets bin for the user's presets (\"A/B\": B inside A; the parents are made too).",
                Capability::EditProject, withSequence({{"bin", schema::string("The bin's path, e.g. \"Lower thirds\".")}}, {"bin"}),
                Json{{"bin", "Lower thirds"}}, {}, newPresetsBin});
    addBuiltin(registry,
               {"effects.newCustomBin", 1, "New Custom Bin",
                "Makes a custom bin in the Effects panel (Premiere's New Custom Bin): a bin of shortcuts to effects, "
                "transitions and presets. Default name: \"Custom Bin 01\", \"Custom Bin 02\", ...",
                Capability::EditProject,
                withSequence({{"name", nameSchema("Its name.")},
                              {"items", schema::array(schema::string("\"effect:<id>\", \"preset:<name>\", or an effect id or preset name."),
                                                      "Items to put in it.")}}),
                Json{{"name", "Gaming"}, {"items", Json::array({"wabcut.blur.gaussian", "preset:Twirl In"})}}, {}, newCustomBin});
    addBuiltin(registry,
               {"effects.addToCustomBin", 1, "Add to Custom Bin",
                "Puts effects, transitions or presets in a custom bin (dragging them there in Premiere). Items already in "
                "it stay once.",
                Capability::EditProject,
                withSequence({{"customBin", nameSchema("The custom bin.")},
                              {"items", schema::array(schema::string("\"effect:<id>\", \"preset:<name>\", or an effect id or preset name."),
                                                      "The items.", 1)}},
                             {"customBin", "items"}),
                Json{{"customBin", "Gaming"}, {"items", Json::array({"crossDissolve"})}}, {}, addToCustomBin});
    addBuiltin(registry,
               {"effects.renameCustomItem", 1, "Rename Custom Item",
                "Renames a custom bin, a presets bin or a user preset (custom bins and favourites follow a renamed preset).",
                Capability::EditProject,
                withSequence({{"customBin", nameSchema("A custom bin to rename.")},
                              {"presetsBin", schema::string("A presets bin to rename (its path).")},
                              {"preset", nameSchema("A user preset to rename.")},
                              {"name", nameSchema("The new name.")}},
                             {"name"}),
                Json{{"customBin", "Custom Bin 01"}, {"name", "Gaming"}}, {}, renameCustomItem});
    addBuiltin(registry,
               {"effects.deleteCustomItem", 1, "Delete Custom Item",
                "Deletes a user item of the Effects panel (Premiere's Delete Custom Item): a preset (it leaves custom bins "
                "and favourites too), a presets bin with the presets in it, a custom bin, or one item of a custom bin "
                "(\"customBin\" with \"item\"). Built-in presets and bins cannot be deleted.",
                Capability::EditProject,
                withSequence({{"preset", nameSchema("A user preset.")},
                              {"presetsBin", schema::string("A presets bin (its path).")},
                              {"customBin", nameSchema("A custom bin.")},
                              {"item", schema::string("With \"customBin\": the item to take out of it (its reference, effect id or preset name).")}}),
                Json{{"customBin", "Gaming"}, {"item", "crossDissolve"}}, {}, deleteCustomItem});
    addBuiltin(registry,
               {"effects.setFavorite", 1, "Add to Favorites",
                "Marks an effect, transition or preset as a favourite (the Effects panel's Favorites bin), or with "
                "\"favorite\": false takes it off the list.",
                Capability::EditProject,
                withSequence({{"item", schema::string("\"effect:<id>\", \"preset:<name>\", or an effect id or preset name.")},
                              {"favorite", schema::boolean("true (default) adds it, false removes it.")}},
                             {"item"}),
                Json{{"item", "wabcut.blur.gaussian"}}, {}, setFavorite});
    addBuiltin(registry,
               touchesFiles({"effects.exportPresets", 1, "Export Effect Presets",
                             "Writes presets to a file (Premiere's Export Preset) another Wabcut can import: named presets "
                             "(built-in or the user's) and/or the user's presets in a presets bin (\"\" = all of them).",
                             Capability::Read,
                             withSequence({{"path", schema::string("The file to write (\".wabcut-presets.json\").")},
                                           {"presets", schema::array(nameSchema("A preset."), "Presets by name.")},
                                           {"presetsBin", schema::string("Every user preset in this bin and the bins inside it.")},
                                           {"overwrite", schema::boolean("Replace an existing file (default false).")}},
                                          {"path"}),
                             Json{{"path", "C:/Presets/mine.wabcut-presets.json"}, {"presetsBin", ""}}, {}, exportPresets},
                            {"path"}));
    addBuiltin(registry,
               touchesFiles({"effects.importPresets", 1, "Import Presets",
                             "Adds the presets of a presets file (effects.exportPresets) to the user's library (Premiere's "
                             "Import Presets). The file is untrusted: read bounded and checked. A name in use gets a number "
                             "(\"Glow 2\").",
                             Capability::EditProject,
                             withSequence({{"path", schema::string("The presets file.")},
                                           {"bin", schema::string("Put them in this presets bin (default: the bins they were exported from).")}},
                                          {"path"}),
                             Json{{"path", "C:/Presets/mine.wabcut-presets.json"}}, {}, importPresets},
                            {"path"}));
}

} // namespace wabcut::command::detail
