// Built-in motion graphics commands (C.13, over GraphicOps.h): graphic clips and their layers (graphic.*), animation
// presets, templates with fields (template.*), the project's brand kit (brand.*) and Lottie import (lottie.import).
// Graphics, layers and brand kits travel as the JSON of GraphicJson.h (docs/graphics.md), checked like a project file.

#include "Catalog.h"

#include <wabcut/command/SpellingServices.h>
#include <wabcut/interchange/Lottie.h>
#include <wabcut/interchange/Mogrt.h>
#include <wabcut/model/BuiltinTemplates.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Spelling.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

// ---- Parameters -------------------------------------------------------------------------------------------------------

Json openObject(std::string description) { return Json{{"type", "object"}, {"description", std::move(description)}}; }

Json layerIdParam(std::string description) { return schema::integer(std::move(description) + " A layer id.", 1); }

Json graphicClipParams(std::initializer_list<std::pair<const char*, Json>> more, std::vector<std::string> required) {
    Json s = withSequence(more, std::move(required));
    s["properties"]["clip"] = clipParam("The graphic clip.");
    s["required"].push_back("clip");
    return s;
}

/// Model JSON from a param, or InvalidParams naming the param.
template <class T>
T parsed(std::expected<T, Error> value, const std::string& path) {
    if (!value) {
        fail(ErrorCode::InvalidParams, value.error().message, Json{{"path", path}});
    }
    return std::move(*value);
}

Json asJson(const std::string& text) { return Json::parse(text); }

const Clip& clipOf(ExecutionContext& c, ClipId id) { return c.sequence().findClip(id)->clip(); }

const Graphic& graphicOf(ExecutionContext& c, ClipId id) {
    const Graphic* g = clipOf(c, id).graphic();
    if (!g) {
        fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " is not a graphic", Json{{"path", "/clip"}});
    }
    return *g;
}

void layerSummary(const std::vector<GraphicLayer>& layers, std::uint32_t parent, Json& out) {
    for (const GraphicLayer& l : layers) {
        Json entry{{"id", l.id}, {"kind", std::string(toString(l.kind))}, {"name", l.name}};
        if (parent != 0) {
            entry["parent"] = parent;
        }
        if (l.kind == LayerKind::Text) {
            entry["text"] = l.text.text();
        }
        if (!l.visible) {
            entry["visible"] = false;
        }
        out.push_back(std::move(entry));
        layerSummary(l.children, l.id, out);
    }
}

/// The layers of a graphic as a flat list, bottom to top (groups before their children).
Json layersJson(const Graphic& g) {
    Json out = Json::array();
    layerSummary(g.layers, 0, out);
    return out;
}

Json graphicResult(ExecutionContext& c, const ProjectState& state, ClipId clip) {
    const Clip& placed = state.findSequence(c.sequenceId())->findClip(clip)->clip();
    Json data{{"clip", clip.value}, {"start", c.timecode(placed.start())}, {"end", c.timecode(placed.end())}};
    if (const Graphic* g = placed.graphic()) {
        data["layers"] = layersJson(*g);
        if (g->instance) {
            data["template"] = g->instance->templateId;
        }
    }
    return data;
}

CommandOutput graphicEdited(ExecutionContext& c, edit::EditResult r, ClipId clip, Json extra = Json::object()) {
    Json data = graphicResult(c, r.state, clip);
    data.update(extra);
    const MediaId canvas = r.createdMedia;
    if (canvas.isValid()) {
        data["media"] = canvas.value;
    }
    CommandOutput out = edited(std::move(r), std::move(data));
    if (canvas.isValid()) {
        out.createdIds.push_back(canvas.value);
    }
    return out;
}

CommandOutput readData(ExecutionContext& c, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    return out;
}

Json placementParam() {
    return schema::enumeration({"targeted", "freeTrackAtOrAbove", "newTrack"},
                               "\"targeted\": on \"track\" (or the targeted video track), overwriting what is there; the "
                               "default when \"track\" is given. \"freeTrackAtOrAbove\": the first unlocked video track from "
                               "\"track\" (or the targeted one) upwards with nothing in the graphic's time range, a new top track "
                               "when none is free; the default without \"track\", so nothing under the playhead is overwritten. "
                               "\"newTrack\": a new video track on top.");
}

// ---- Draw lists -------------------------------------------------------------------------------------------------------

Json rgbaJson(const Rgba& c) { return Json::array({c[0], c[1], c[2], c[3]}); }

Json pathJson(const std::vector<MaskVertex>& path) {
    Json out = Json::array();
    for (const MaskVertex& v : path) {
        out.push_back(Json::array({v.x, v.y, v.inX, v.inY, v.outX, v.outY}));
    }
    return out;
}

Json drawItemJson(const DrawItem& item) {
    const Affine2D& m = item.transform;
    Json out{{"layer", item.layer},
             {"kind", item.kind == DrawKind::Shape ? "shape" : item.kind == DrawKind::Text ? "text" : "image"},
             {"transform", Json::array({m.a, m.b, m.c, m.d, m.e, m.f})},
             {"opacity", item.opacity},
             {"blend", std::string(toString(item.blend))},
             {"bounds", Json::array({item.bounds.x, item.bounds.y, item.bounds.width, item.bounds.height})}};
    if (item.shadow.enabled) {
        out["shadow"] = Json{{"color", rgbaJson(item.shadow.color)}, {"opacity", item.shadow.opacity}, {"angle", item.shadow.angle},
                             {"distance", item.shadow.distance}, {"softness", item.shadow.softness}};
    }
    if (!item.masks.empty()) {
        Json masks = Json::array();
        for (const DrawMask& mask : item.masks) {
            masks.push_back(Json{{"owner", mask.owner}, {"mode", std::string(toString(mask.mode))}, {"inverted", mask.inverted},
                                 {"feather", mask.feather}, {"expansion", mask.expansion}, {"opacity", mask.opacity},
                                 {"outline", pathJson(mask.outline)}});
        }
        out["masks"] = std::move(masks);
    }
    switch (item.kind) {
    case DrawKind::Shape: {
        out["path"] = pathJson(item.path);
        out["closed"] = item.closed;
        Json fill{{"kind", std::string(toString(item.fill.kind))}};
        if (item.fill.kind == PaintKind::Solid) {
            fill["color"] = rgbaJson(item.fill.color);
        } else if (item.fill.kind != PaintKind::None) {
            Json stops = Json::array();
            for (const auto& [offset, color] : item.fill.stops) {
                stops.push_back(Json{{"offset", offset}, {"color", rgbaJson(color)}});
            }
            fill["stops"] = std::move(stops);
            fill["start"] = Json::array({item.fill.start[0], item.fill.start[1]});
            fill["end"] = Json::array({item.fill.end[0], item.fill.end[1]});
        }
        out["fill"] = std::move(fill);
        if (item.stroke.enabled) {
            out["stroke"] = Json{{"color", rgbaJson(item.stroke.color)}, {"width", item.stroke.width}, {"dashes", item.stroke.dashes},
                                 {"join", std::string(toString(item.stroke.join))}, {"cap", std::string(toString(item.stroke.cap))}};
        }
        if (item.trimStart != 0.0 || item.trimEnd != 1.0 || item.trimOffset != 0.0) {
            out["trim"] = Json{{"start", item.trimStart}, {"end", item.trimEnd}, {"offset", item.trimOffset}};
        }
        break;
    }
    case DrawKind::Text: {
        const DrawText& t = item.text;
        Json runs = Json::array();
        for (const DrawTextRun& r : t.runs) {
            runs.push_back(Json{{"byteStart", r.byteStart}, {"text", r.text}, {"fontFamily", r.fontFamily}, {"fontStyle", r.fontStyle},
                                {"fontWeight", r.fontWeight}, {"fontSize", r.fontSize}, {"fill", rgbaJson(r.fill)},
                                {"italic", r.italic}, {"underline", r.underline}, {"tracking", r.tracking},
                                {"leading", r.leading}, {"baselineShift", r.baselineShift}});
        }
        Json text{{"runs", std::move(runs)}, {"align", std::string(toString(t.align))}, {"boxWidth", t.boxWidth},
                  {"boxHeight", t.boxHeight}, {"language", t.language}};
        if (t.background) {
            text["background"] = Json{{"color", rgbaJson(t.backgroundColor)}, {"padding", t.backgroundPadding},
                                      {"radius", t.backgroundRadius}};
        }
        if (!t.clusters.empty()) {
            Json clusters = Json::array();
            for (const DrawCluster& cl : t.clusters) {
                clusters.push_back(Json{{"byteStart", cl.byteStart}, {"byteEnd", cl.byteEnd}, {"opacity", cl.opacity},
                                        {"offsetX", cl.offsetX}, {"offsetY", cl.offsetY}, {"scale", cl.scale},
                                        {"rotation", cl.rotation}});
            }
            text["clusters"] = std::move(clusters);
        }
        out["text"] = std::move(text);
        break;
    }
    case DrawKind::Image:
        out["image"] = Json{{"media", item.image.media.value}, {"width", item.image.width}, {"height", item.image.height},
                            {"fit", std::string(toString(item.image.fit))}};
        break;
    }
    return out;
}

// ---- Templates --------------------------------------------------------------------------------------------------------

const GraphicsServices* graphicsServices(ExecutionContext& c) { return findService<GraphicsServices>(c.services()); }

std::filesystem::path userLibrary(ExecutionContext& c, bool required) {
    const GraphicsServices* s = graphicsServices(c);
    if ((!s || s->userLibrary().empty()) && required) {
        fail(ErrorCode::InvalidRequest, "this host has no user template library (save to the project's with \"library\": \"project\")");
    }
    return s ? s->userLibrary() : std::filesystem::path();
}

} // namespace

FoundTemplate findTemplate(ExecutionContext& c, const std::string& id, const std::string& library) {
    if (library.empty() || library == "project") {
        if (const GraphicTemplate* t = c.state().findTemplate(id)) {
            return {*t, "project"};
        }
    }
    if (library.empty() || library == "user") {
        const std::filesystem::path folder = userLibrary(c, false);
        if (!folder.empty()) {
            for (const io::LibraryTemplate& entry : io::readTemplateLibrary(folder)) {
                if (entry.tmpl && entry.tmpl->id == id) {
                    return {*entry.tmpl, "user"};
                }
            }
        }
    }
    if (library.empty() || library == "builtin") { // C.16
        if (const GraphicTemplate* t = findBuiltinTemplate(id)) {
            return {*t, "builtin"};
        }
    }
    fail(ErrorCode::NotFound, "no template \"" + id + "\"" + (library.empty() ? std::string() : " in the " + library + " library"),
         Json{{"path", "/template"}});
}

namespace {

Json templateSummary(const GraphicTemplate& t) {
    Json fields = Json::array();
    for (const TemplateField& f : t.fields) {
        Json field{{"id", f.id}, {"label", f.label}, {"kind", std::string(toString(f.kind))}, {"property", f.property},
                   {"default", asJson(io::fieldValueToJson(f.kind, f.value))}};
        if (f.layer != 0) {
            field["layer"] = f.layer;
        }
        fields.push_back(std::move(field));
    }
    return Json{{"id", t.id},
                {"name", t.name},
                {"description", t.description},
                {"version", t.version},
                {"duration", t.duration.value},
                {"width", t.graphic.width},
                {"height", t.graphic.height},
                {"layers", t.graphic.layerCount()},
                {"fields", std::move(fields)}};
}

const TemplateField* fieldNamed(const std::vector<TemplateField>& fields, const std::string& id) {
    const auto it = std::find_if(fields.begin(), fields.end(), [&](const TemplateField& f) { return f.id == id; });
    return it == fields.end() ? nullptr : &*it;
}

/// A field value from a param: as the field JSON takes it, except images (a media id or name) and durations (a time).
FieldValue fieldValue(ExecutionContext& c, const TemplateField& field, const Json& value, const std::string& path) {
    Json v = value;
    if (field.kind == FieldKind::Image) {
        v = c.mediaValue(value, path).value;
    } else if (field.kind == FieldKind::Duration) {
        v = c.timeValue(value, path).value;
    }
    return parsed(io::fieldValueFromJson(field.kind, v.dump()), path);
}

} // namespace

std::vector<std::pair<std::string, FieldValue>> fieldValues(ExecutionContext& c, const std::vector<TemplateField>& fields,
                                                            const Json& values, const std::string& path) {
    std::vector<std::pair<std::string, FieldValue>> out;
    if (!values.is_object()) {
        return out;
    }
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::string& id = it.key();
        const TemplateField* field = fieldNamed(fields, id);
        if (!field) {
            fail(ErrorCode::InvalidParams, "the template has no field \"" + id + "\"", Json{{"path", path + "/" + id}});
        }
        out.emplace_back(id, fieldValue(c, *field, it.value(), path + "/" + id));
    }
    return out;
}

namespace {

std::vector<TemplateField> fieldsParam(ExecutionContext& c) {
    std::vector<TemplateField> fields;
    if (!c.has("fields")) {
        return fields;
    }
    const Json& list = c.params()["fields"];
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& f = list[i];
        const std::string path = "/fields/" + std::to_string(i);
        TemplateField field;
        field.id = f.value("id", std::string());
        field.label = f.value("label", field.id);
        const auto kind = fieldKindFromString(f.value("kind", std::string()));
        if (!kind) {
            fail(ErrorCode::InvalidParams, "unknown field kind", Json{{"path", path + "/kind"}});
        }
        field.kind = *kind;
        field.layer = f.contains("layer") ? f["layer"].get<std::uint32_t>() : 0;
        field.property = f.value("property", std::string());
        fields.push_back(std::move(field));
    }
    return fields;
}

Json fieldListSchema() {
    return schema::array(
        schema::object({{"id", schema::string("Stable key: [A-Za-z][A-Za-z0-9_.-]*.")},
                        {"label", schema::string("What the Essential Graphics panel shows.")},
                        {"kind", schema::enumeration({"text", "color", "font", "image", "duration", "checkbox"}, "What it edits.")},
                        {"layer", schema::integer("The layer it edits (not for a \"clip\" duration).", 0, 4294967295)},
                        {"property", schema::string("text: \"text\"; color: \"fill\", \"stroke\", \"background\" or \"shadow\"; "
                                                    "font: \"font\"; image: \"media\"; duration: \"out\" (the layer's out "
                                                    "point) or \"clip\" (the clip's length); checkbox: \"visible\".")}},
                       {"id", "kind", "property"}),
        "The editable fields; their defaults are what the graphic shows now.");
}

// ---- Lottie -----------------------------------------------------------------------------------------------------------

std::string readLottieFile(const std::string& utf8Path) {
    const std::filesystem::path path = pathFromUtf8(utf8Path);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        fail(ErrorCode::NotFound, "cannot read \"" + utf8Path + "\"", Json{{"path", "/path"}});
    }
    if (size > interchange::kMaxLottieBytes) {
        fail(ErrorCode::InvalidParams, "the Lottie file is larger than 16 MiB", Json{{"path", "/path"}});
    }
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in && !in.eof()) {
        fail(ErrorCode::NotFound, "cannot read \"" + utf8Path + "\"", Json{{"path", "/path"}});
    }
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

Json reportJson(const interchange::ImportReport& report) {
    using interchange::ReportOutcome;
    Json entries = Json::array();
    for (const interchange::ReportEntry& e : report.entries()) {
        if (e.outcome == ReportOutcome::Carried || entries.size() >= 200) {
            continue;
        }
        entries.push_back(Json{{"outcome", std::string(toString(e.outcome))}, {"category", std::string(toString(e.category))},
                               {"reason", e.reason}, {"element", e.location.element}});
    }
    return Json{{"carried", report.count(ReportOutcome::Carried)},
                {"approximated", report.count(ReportOutcome::Approximated)},
                {"dropped", report.count(ReportOutcome::Dropped)},
                {"entries", std::move(entries)}};
}

} // namespace

void registerGraphicsCommands(CommandRegistry& registry) {
    // ---- Graphic clips and layers -------------------------------------------------------------------------------------
    addBuiltin(
        registry,
        {"graphic.add", 1, "New Graphic",
         "Adds a motion graphic as a clip on a video track (overwriting what is there): \"graphic\" is the graphic's JSON "
         "(docs/graphics.md: name, width/height of its design canvas - default the sequence's frame - layers of text, "
         "shapes, images and groups, responsive time). Layer ids 0 get fresh ones. Add layers later with graphic.addLayer, "
         "animate them with graphic.applyPreset or keyframes in the layer JSON. createdIds: the clip, and the canvas media "
         "item when the project had none of this size.",
         Capability::EditSequence,
         withSequence({{"at", timeParam("Where the graphic starts.")},
                       {"duration", timeParam("How long it lasts. Default 5 seconds.")},
                       {"track", trackParam("A video track. Default: the first targeted unlocked video track.")},
                       {"placement", placementParam()},
                       {"name", schema::string("The graphic's name (also the clip's).")},
                       {"graphic", openObject("The graphic (docs/graphics.md). Default: an empty one.")}},
                      {"at"}),
         Json{{"at", "00:00:05:00"},
              {"track", "V2"},
              {"name", "Lower third"},
              {"graphic", Json{{"layers", Json::array({Json{{"id", 1}, {"kind", "text"}, {"transform", Json{{"x", 160}, {"y", 880}}},
                                                            {"text", Json{{"runs", Json::array({Json{{"text", "Somchai"}, {"size", 64}}})}}}}})}}}},
         {},
         [](ExecutionContext& c) {
             const Sequence& seq = c.sequence();
             Json graphic = c.has("graphic") ? c.params()["graphic"] : Json::object();
             if (!graphic.contains("width") && !graphic.contains("height")) {
                 graphic["width"] = seq.width;
                 graphic["height"] = seq.height;
             }
             if (c.has("name")) {
                 graphic["name"] = c.string("name");
             }
             Graphic g = parsed(io::graphicFromJson(graphic.dump()), "/graphic");
             const Ticks at = c.time("at");
             const Ticks duration = c.optionalTime("duration").value_or(edit::kDefaultStillDuration);
             ProjectState state = c.state();
             const TrackId track = videoPlacementTrack(c, state, at, duration);
             edit::EditResult r = c.take(edit::addGraphic(state, c.sequenceId(), track, at, duration, std::move(g)));
             const ClipId clip = r.placed.front();
             return graphicEdited(c, std::move(r), clip);
         }});
    addBuiltin(registry,
               {"graphic.get", 1, "Graphic",
                "A graphic clip's graphic as JSON (docs/graphics.md), with its layers listed flat (bottom to top, groups "
                "before their children) and, for a template instance, its template and field values.",
                Capability::Read, graphicClipParams({}, {}), Json{{"clip", 42}}, {},
                [](ExecutionContext& c) {
                    const ClipId clip = c.clip("clip");
                    const Graphic& g = graphicOf(c, clip);
                    Json data = graphicResult(c, c.state(), clip);
                    data["graphic"] = asJson(io::graphicToJson(g));
                    return readData(c, std::move(data));
                }});
    addBuiltin(registry,
               {"graphic.checkSpelling", 1, "Check Spelling in Graphic",
                "Spell check for a graphic clip's text: the misspelled words of every text layer (groups included, bottom "
                "to top), each in its layer's language (or \"language\"), from the host's spell checker and the user's "
                "dictionary - the layer, the word's byte range in the layer's text, the word, the checker's advice and up "
                "to 8 suggestions, as captions.checkSpelling lists them. Corrections are graphic.setText edits.",
                Capability::Read,
                graphicClipParams({{"language", schema::string("BCP 47 or ISO 639-1. Default: each layer's, else the user's.")},
                                   {"suggestions", schema::boolean("List suggestions. Default true.", true)}},
                                  {}),
                Json{{"clip", 42}}, {},
                [](ExecutionContext& c) {
                    const auto* services = findService<SpellingServices>(c.services());
                    if (!services) {
                        fail(ErrorCode::InvalidRequest, "this host has no spell checker");
                    }
                    const ClipId clip = c.clip("clip");
                    const Graphic& g = graphicOf(c, clip);
                    const std::string language = c.has("language") ? c.string("language") : std::string();
                    Json issues = Json::array();
                    for (const TextSpellIssue& i :
                         spellingOf(g, services->checker(), language, c.boolean("suggestions", true), 2000)) {
                        issues.push_back(Json{{"layer", i.layer},
                                              {"start", i.issue.start},
                                              {"end", i.issue.end},
                                              {"word", i.issue.word},
                                              {"action", std::string(toString(i.issue.action))},
                                              {"suggestions", i.issue.suggestions}});
                    }
                    return readData(c, Json{{"clip", clip.value}, {"issues", std::move(issues)}});
                }});
    addBuiltin(
        registry,
        {"graphic.resolve", 1, "Graphic Draw List",
         "The draw list of a graphic clip at a timeline time (default its start): every visible layer bottom to top with "
         "its transform to canvas pixels [a, b, c, d, e, f], opacity, blend mode, shadow, masks, and its path and paints, "
         "text runs (and per-character animator values) or image - with the brand kit applied. What the renderer draws "
         "(docs/graphics.md).",
         Capability::Read,
         graphicClipParams({{"time", timeParam("A timeline time (default the clip's start).")}}, {}),
         Json{{"clip", 42}, {"time", "00:00:06:00"}}, {},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             graphicOf(c, id);
             const Clip& clip = clipOf(c, id);
             const Ticks t = c.optionalTime("time").value_or(clip.start());
             ResolveOptions options;
             if (const GraphicsServices* s = graphicsServices(c)) {
                 options.measurer = s->measurer();
             }
             if (const auto* captions = findService<CaptionServices>(c.services())) {
                 options.lineBreaker = &captions->lineBreaker();
             }
             const DrawList list = resolveGraphicClip(c.state(), clip, t, options);
             Json items = Json::array();
             for (const DrawItem& item : list.items) {
                 items.push_back(drawItemJson(item));
             }
             return readData(c, Json{{"clip", id.value},
                                     {"graphicTime", graphicTimeAt(clip, *clip.graphic(), t).value},
                                     {"width", list.width},
                                     {"height", list.height},
                                     {"items", std::move(items)}});
         }});
    addBuiltin(registry,
               {"graphic.update", 1, "Edit Graphic",
                "Changes a graphic clip's graphic with a JSON merge patch (RFC 7386) over its JSON (graphic.get): objects "
                "merge, null resets a key to its default, arrays (\"layers\") are replaced whole. For one layer use "
                "graphic.updateLayer.",
                Capability::EditSequence,
                graphicClipParams({{"set", openObject("The merge patch, e.g. {\"name\": \"Title\", \"responsive\": {...}}.")}},
                                  {"set"}),
                Json{{"clip", 42}, {"set", Json{{"responsive", Json{{"duration", 1270080000000}, {"intro", 254016000000}, {"outro", 254016000000}}}}}},
                {},
                [](ExecutionContext& c) {
                    const ClipId clip = c.clip("clip");
                    Json current = asJson(io::graphicToJson(graphicOf(c, clip)));
                    current.merge_patch(c.params()["set"]);
                    Graphic g = parsed(io::graphicFromJson(current.dump()), "/set");
                    return graphicEdited(c, c.take(edit::setGraphic(c.state(), c.sequenceId(), clip, std::move(g))), clip);
                }});
    addBuiltin(
        registry,
        {"graphic.addLayer", 1, "Add Graphic Layer",
         "Adds a layer (JSON, docs/graphics.md) to a graphic clip: on top of \"parent\" (a group layer; default the top "
         "level), or at \"index\" (0 = bottom). An id of 0 or one in use gets a fresh one; returns it as \"layer\".",
         Capability::EditSequence,
         graphicClipParams({{"layer", openObject("The layer: {\"kind\": \"text\" | \"shape\" | \"image\" | \"group\", ...}.")},
                            {"parent", schema::integer("A group layer to add it to (default 0: the top level).", 0, 4294967295)},
                            {"index", schema::integer("Where in the parent's list, 0 = bottom (default: on top).", 0)}},
                           {"layer"}),
         Json{{"clip", 42},
              {"layer", Json{{"kind", "shape"}, {"shape", Json{{"type", "rectangle"}, {"width", 640}, {"height", 120}, {"cornerRadius", 16}}},
                             {"transform", Json{{"x", 480}, {"y", 900}}}}}},
         {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             const Graphic& g = graphicOf(c, clip);
             Json layerJson = c.params()["layer"];
             if (!layerJson.contains("id")) {
                 layerJson["id"] = 0;
             }
             GraphicLayer layer = parsed(io::layerFromJson(layerJson.dump(), g.width, g.height), "/layer");
             const auto parent = static_cast<std::uint32_t>(c.integer("parent", 0));
             const std::optional<std::size_t> index =
                 c.has("index") ? std::optional<std::size_t>(static_cast<std::size_t>(c.integer("index"))) : std::nullopt;
             edit::EditResult r = c.take(edit::addGraphicLayer(c.state(), c.sequenceId(), clip, std::move(layer), parent, index));
             const std::uint32_t created = r.createdLayer;
             return graphicEdited(c, std::move(r), clip, Json{{"layer", created}});
         }});
    addBuiltin(registry,
               {"graphic.updateLayer", 1, "Edit Graphic Layer",
                "Changes a layer of a graphic clip with a JSON merge patch (RFC 7386) over the layer's JSON: "
                "{\"text\": {\"runs\": [{\"text\": \"New\"}]}} replaces the runs, {\"transform\": {\"x\": 200}} moves it, a "
                "curve ({\"keys\": [{\"t\": ticks, \"v\": value}, ...]} or just the array of keys) animates it. Its id stays.",
                Capability::EditSequence,
                graphicClipParams({{"layer", layerIdParam("The layer.")}, {"set", openObject("The merge patch.")}},
                                  {"layer", "set"}),
                Json{{"clip", 42}, {"layer", 2}, {"set", Json{{"transform", Json{{"opacity", 80}}}}}}, {},
                [](ExecutionContext& c) {
                    const ClipId clip = c.clip("clip");
                    const Graphic& g = graphicOf(c, clip);
                    const auto id = static_cast<std::uint32_t>(c.integer("layer"));
                    const GraphicLayer* layer = g.findLayer(id);
                    if (!layer) {
                        fail(ErrorCode::NotFound, "the graphic has no layer " + std::to_string(id), Json{{"path", "/layer"}});
                    }
                    Json current = asJson(io::layerToJson(*layer));
                    current.merge_patch(c.params()["set"]);
                    current["id"] = id;
                    GraphicLayer changed = parsed(io::layerFromJson(current.dump(), g.width, g.height), "/set");
                    return graphicEdited(
                        c, c.take(edit::updateGraphicLayer(c.state(), c.sequenceId(), clip, id, std::move(changed))), clip);
                }});
    {
        Json vertex = schema::array(schema::number("A coordinate, pixels.", -1e6, 1e6),
                                    "[x, y] or [x, y, inX, inY, outX, outY] (tangents relative to the vertex), pixels.", 2);
        vertex["maxItems"] = 6;
        Json vertices = schema::array(vertex, "custom: the path's vertices in the layer's pixels.", 2);
        vertices["maxItems"] = static_cast<std::int64_t>(kMaxPathVertices);
        addBuiltin(
            registry,
            {"graphic.setTextPath", 1, "Type on a Path",
             "Puts a text layer on a path (the Legacy Titler's Path Type tool, After Effects' Path Options): \"shape\" "
             "line, arc (the top of an ellipse: text over a rainbow), circle (clockwise from its left: text round the "
             "outside) or wave, \"width\" and \"height\" in pixels, centred on the layer's origin - or custom "
             "\"vertices\" (\"closed\" for a closed path); \"none\" puts it back on straight lines. Options: "
             "\"reverse\" (Reverse Path), \"perpendicular\" (characters turn with the path, default true), "
             "\"forceAlignment\" (spread from the first margin to the last), \"firstMargin\" / \"lastMargin\" "
             "(pixels; keyframe the first margin with graphic.updateLayer to send the text along the path). Without "
             "\"shape\" only the options change. The text's alignment places it between the margins. One undo step.",
             Capability::EditSequence,
             graphicClipParams({{"layer", layerIdParam("The text layer.")},
                                {"shape", schema::enumeration({"none", "line", "arc", "circle", "wave", "custom"},
                                                              "The path. Default: keep the layer's (arc when it has none).")},
                                {"width", schema::number("The shape's width, pixels. Default 800 (a circle: its diameter).", 1.0, 1e6)},
                                {"height", schema::number("The arc's or wave's height, pixels. Default 300 (wave 120).", 1.0, 1e6)},
                                {"vertices", vertices},
                                {"closed", schema::boolean("custom: a closed path. Default false.")},
                                {"reverse", schema::boolean("Reverse Path.")},
                                {"perpendicular", schema::boolean("Perpendicular To Path.")},
                                {"forceAlignment", schema::boolean("Force Alignment.")},
                                {"firstMargin", schema::number("First Margin, pixels.", -1e6, 1e6)},
                                {"lastMargin", schema::number("Last Margin, pixels.", -1e6, 1e6)}},
                               {"layer"}),
             Json{{"clip", 42}, {"layer", 2}, {"shape", "arc"}, {"width", 900}, {"height", 320}}, {},
             [](ExecutionContext& c) {
                 const ClipId clip = c.clip("clip");
                 const Graphic& g = graphicOf(c, clip);
                 const auto id = static_cast<std::uint32_t>(c.integer("layer"));
                 const GraphicLayer* layer = g.findLayer(id);
                 if (!layer) {
                     fail(ErrorCode::NotFound, "the graphic has no layer " + std::to_string(id), Json{{"path", "/layer"}});
                 }
                 if (layer->kind != LayerKind::Text) {
                     fail(ErrorCode::InvalidParams, "layer " + std::to_string(id) + " is not a text layer", Json{{"path", "/layer"}});
                 }
                 const std::string shape = c.has("shape") ? c.string("shape") : std::string();
                 if (shape == "none") {
                     return graphicEdited(c, c.take(edit::setTextPath(c.state(), c.sequenceId(), clip, id, std::nullopt)), clip);
                 }
                 TextPath path = layer->text.path.value_or(TextPath{});
                 if (!shape.empty() || !layer->text.path) {
                     MaskPathKey key;
                     if (shape == "custom") {
                         if (!c.has("vertices")) {
                             fail(ErrorCode::InvalidParams, "a custom path needs \"vertices\"", Json{{"path", "/vertices"}});
                         }
                         for (const Json& v : c.params()["vertices"]) {
                             MaskVertex point{v[0].get<double>(), v[1].get<double>(), 0.0, 0.0, 0.0, 0.0};
                             if (v.size() == 6) {
                                 point.inX = v[2].get<double>();
                                 point.inY = v[3].get<double>();
                                 point.outX = v[4].get<double>();
                                 point.outY = v[5].get<double>();
                             } else if (v.size() != 2) {
                                 fail(ErrorCode::InvalidParams, "a vertex is [x, y] or [x, y, inX, inY, outX, outY]",
                                      Json{{"path", "/vertices"}});
                             }
                             key.vertices.push_back(point);
                         }
                         path.closed = c.boolean("closed", false);
                     } else {
                         const edit::TextPathShape kind =
                             edit::textPathShapeFromString(shape.empty() ? "arc" : shape).value_or(edit::TextPathShape::Arc);
                         const double width = c.has("width") ? c.number("width") : 800.0;
                         const double height = c.has("height") ? c.number("height") : (kind == edit::TextPathShape::Wave ? 120.0 : 300.0);
                         key.vertices = edit::textPathVertices(kind, width, height, &path.closed);
                     }
                     path.path = {std::move(key)};
                 }
                 if (c.has("reverse")) {
                     path.reverse = c.boolean("reverse", path.reverse);
                 }
                 if (c.has("perpendicular")) {
                     path.perpendicular = c.boolean("perpendicular", path.perpendicular);
                 }
                 if (c.has("forceAlignment")) {
                     path.forceAlignment = c.boolean("forceAlignment", path.forceAlignment);
                 }
                 if (c.has("firstMargin")) {
                     path.firstMargin = ParamValue(c.number("firstMargin"));
                 }
                 if (c.has("lastMargin")) {
                     path.lastMargin = ParamValue(c.number("lastMargin"));
                 }
                 return graphicEdited(c, c.take(edit::setTextPath(c.state(), c.sequenceId(), clip, id, std::move(path))), clip);
             }});
    }
    addBuiltin(registry,
               {"graphic.removeLayer", 1, "Delete Graphic Layer",
                "Removes a layer from a graphic clip (a group with its children); pins to it and template fields bound to it "
                "go too.",
                Capability::EditSequence, graphicClipParams({{"layer", layerIdParam("The layer.")}}, {"layer"}),
                Json{{"clip", 42}, {"layer", 3}}, {},
                [](ExecutionContext& c) {
                    const ClipId clip = c.clip("clip");
                    graphicOf(c, clip);
                    return graphicEdited(c,
                                         c.take(edit::removeGraphicLayer(c.state(), c.sequenceId(), clip,
                                                                         static_cast<std::uint32_t>(c.integer("layer")))),
                                         clip);
                }});
    addBuiltin(
        registry,
        {"graphic.applyPreset", 1, "Apply Animation Preset",
         "Animates a layer with editable keyframes: fade, slide, scalePop, bounce (the transform), typewriter and "
         "wordByWord (a text-range animator: characters or words appear in turn), wipe (a mask). \"in\" animates the "
         "graphic's start, \"out\" its end, \"loop\" repeats over the whole clip. Keys inside the preset's time replace "
         "the ones there; others stay.",
         Capability::EditSequence,
         graphicClipParams(
             {{"layer", layerIdParam("The layer.")},
              {"preset", schema::enumeration({"fade", "slide", "scalePop", "typewriter", "wordByWord", "wipe", "bounce"}, "The preset.")},
              {"phase", schema::enumeration({"in", "out", "loop"}, "In (default), out or loop.")},
              {"duration", timeParam("How long it takes (one cycle of a loop). Default half a second.")},
              {"easing", schema::enumeration({"linear", "easeIn", "easeOut", "easeInOut", "overshoot"}, "Default easeOut.")},
              {"direction", schema::enumeration({"left", "right", "up", "down"},
                                                "Slide and wipe: where from (in), to (out) or along (loop). Default left.")},
              {"distance", schema::number("Slide and bounce: how far, pixels (default a quarter of the canvas).", 0.0, 1e6)},
              {"overshoot", schema::number("Scale pop, bounce and overshoot easing: how far past, % (default 10).", 0.0, 100.0)}},
             {"layer", "preset"}),
         Json{{"clip", 42}, {"layer", 2}, {"preset", "typewriter"}, {"duration", "00:00:01:00"}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             graphicOf(c, clip);
             edit::PresetOptions o;
             o.kind = *edit::presetKindFromString(c.string("preset"));
             o.phase = edit::presetPhaseFromString(c.string("phase", "in")).value_or(edit::PresetPhase::In);
             o.easing = edit::presetEasingFromString(c.string("easing", "easeOut")).value_or(edit::PresetEasing::EaseOut);
             o.direction = edit::presetDirectionFromString(c.string("direction", "left")).value_or(edit::PresetDirection::Left);
             if (const auto d = c.optionalTime("duration")) {
                 o.duration = *d;
             }
             if (c.has("distance")) {
                 o.distance = c.number("distance");
             }
             if (c.has("overshoot")) {
                 o.overshoot = c.number("overshoot");
             }
             return graphicEdited(
                 c, c.take(edit::applyGraphicPreset(c.state(), c.sequenceId(), clip, static_cast<std::uint32_t>(c.integer("layer")), o)),
                 clip);
         }});

    // ---- Roll / Crawl and Align / Distribute (0.2) ---------------------------------------------------------------------
    addBuiltin(
        registry,
        {"graphic.setRoll", 1, "Roll/Crawl Options",
         "Makes a graphic clip roll up (credits: Premiere's Essential Graphics Roll) or crawl left or right (a ticker: the "
         "Legacy Titler's Crawl Left / Crawl Right) over the clip, so the clip's length sets the speed; \"roll\": null stops "
         "it. Start Off Screen / End Off Screen (default on), Preroll and Postroll (held at the start / end), Ease-In and "
         "Ease-Out (speeding up / slowing down) as Premiere has them. The graphic is designed in place: layers below the "
         "frame are the rest of the roll.",
         Capability::EditSequence,
         graphicClipParams({{"roll", Json{{"type", Json::array({"object", "null"})},
                                          {"description", "The options, or null for none."},
                                          {"properties", Json{{"kind", schema::enumeration({"roll", "crawlLeft", "crawlRight"}, "Default roll.")},
                                                              {"startOffScreen", schema::boolean("Start Off Screen (default true).")},
                                                              {"endOffScreen", schema::boolean("End Off Screen (default true).")},
                                                              {"preroll", timeParam("Preroll: held at the start.")},
                                                              {"postroll", timeParam("Postroll: held at the end.")},
                                                              {"easeIn", timeParam("Ease-In.")},
                                                              {"easeOut", timeParam("Ease-Out.")}}},
                                          {"additionalProperties", false}}}},
                           {"roll"}),
         Json{{"clip", 42}, {"roll", Json{{"kind", "roll"}, {"postroll", "00:00:01:00"}, {"easeOut", "00:00:00:12"}}}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             Graphic g = graphicOf(c, clip);
             const Json& param = c.params()["roll"];
             if (param.is_null()) {
                 g.roll.reset();
             } else {
                 GraphicRoll roll;
                 if (param.contains("kind")) {
                     roll.kind = *rollKindFromString(param["kind"].get<std::string>());
                 }
                 roll.startOffScreen = param.value("startOffScreen", true);
                 roll.endOffScreen = param.value("endOffScreen", true);
                 const auto span = [&](const char* key) {
                     if (!param.contains(key)) {
                         return Ticks{};
                     }
                     const Ticks t = c.timeValue(param[key], std::string("/roll/") + key);
                     if (t.value < 0) {
                         fail(ErrorCode::InvalidParams, std::string(key) + " must not be negative", Json{{"path", std::string("/roll/") + key}});
                     }
                     return t;
                 };
                 roll.preroll = span("preroll");
                 roll.postroll = span("postroll");
                 roll.easeIn = span("easeIn");
                 roll.easeOut = span("easeOut");
                 g.roll = roll;
             }
             return graphicEdited(c, c.take(edit::setGraphic(c.state(), c.sequenceId(), clip, std::move(g))), clip);
         }});
    addBuiltin(
        registry,
        {"graphic.align", 1, "Align Layers",
         "Premiere's Essential Graphics Align and Distribute: lines layers up with the video frame (\"to\": \"frame\", the "
         "default) or with each other (\"selection\") - left, horizontal centre, right, top, vertical centre, bottom - or "
         "spaces three or more evenly (distributeHorizontal / distributeVertical: centres evenly spaced between the outermost "
         "two). Layers move by their Position (an animated one with all its keys), measured as drawn at \"time\" (default "
         "the clip's start).",
         Capability::EditSequence,
         graphicClipParams({{"layers", schema::array(layerIdParam("A layer."), "The layers.", 1)},
                            {"align", schema::enumeration({"left", "horizontalCenter", "right", "top", "verticalCenter", "bottom",
                                                           "distributeHorizontal", "distributeVertical"},
                                                          "What to do.")},
                            {"to", schema::enumeration({"frame", "selection"}, "Align to the video frame (default) or the selection.")},
                            {"time", timeParam("When to measure the layers (inside the clip). Default its start.")}},
                           {"layers", "align"}),
         Json{{"clip", 42}, {"layers", Json::array({2, 3})}, {"align", "horizontalCenter"}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             const Clip& placed = clipOf(c, clip);
             const Graphic& g = graphicOf(c, clip);
             std::vector<std::uint32_t> layers;
             for (const Json& id : c.params()["layers"]) {
                 layers.push_back(static_cast<std::uint32_t>(id.get<std::int64_t>()));
             }
             const Ticks at = c.optionalTime("time").value_or(placed.start());
             if (at < placed.start() || at >= placed.end()) {
                 fail(ErrorCode::InvalidParams, "the time lies outside the clip", Json{{"path", "/time"}});
             }
             ResolveOptions options;
             if (const GraphicsServices* s = graphicsServices(c)) {
                 options.measurer = s->measurer();
             }
             if (const auto* captions = findService<CaptionServices>(c.services())) {
                 options.lineBreaker = &captions->lineBreaker();
             }
             const edit::LayerAlign align = *edit::layerAlignFromString(c.string("align"));
             const edit::AlignTo to = c.string("to", "frame") == "selection" ? edit::AlignTo::Selection : edit::AlignTo::VideoFrame;
             return graphicEdited(c, c.take(edit::alignLayers(c.state(), c.sequenceId(), clip, layers, align, to,
                                                             graphicTimeAt(placed, g, at), options)),
                                  clip);
         }});

    // ---- Text styles (0.2): Premiere's Text Styles, saved with the project ------------------------------------------
    const auto styleJson = [](const ProjectTextStyle& s) { return asJson(io::textStyleToJson(s)); };
    addBuiltin(registry,
               {"textStyle.list", 1, "Text Styles",
                "The project's text styles (Premiere's Text Style items): each with its look (the text JSON of a layer with one "
                "run: font, size, fill, tracking...; alignment, background; shadow) and how many text layers follow it.",
                Capability::Read, withSequence({}), Json::object(), {},
                [styleJson](ExecutionContext& c) {
                    std::map<std::string, int> uses;
                    const std::function<void(const std::vector<GraphicLayer>&)> count = [&](const std::vector<GraphicLayer>& layers) {
                        for (const GraphicLayer& l : layers) {
                            if (!l.textStyle.empty()) {
                                ++uses[l.textStyle];
                            }
                            count(l.children);
                        }
                    };
                    for (const SequencePtr& seq : c.state().sequences) {
                        seq->forEachTrack([&](const Track& t) {
                            for (const Clip& cl : t.clips) {
                                if (const Graphic* g = cl.graphic()) {
                                    count(g->layers);
                                }
                            }
                        });
                    }
                    Json styles = Json::array();
                    for (const ProjectTextStyle& s : c.state().textStyles) {
                        Json e = styleJson(s);
                        e["layers"] = uses[s.name];
                        styles.push_back(std::move(e));
                    }
                    return readData(c, Json{{"textStyles", std::move(styles)}});
                }});
    addBuiltin(registry,
               {"textStyle.create", 1, "Create Text Style",
                "Premiere's Create Text Style: the look of a text layer (its first run's font, size, fill, tracking...; its "
                "alignment, background and shadow) saved in the project under a name; the layer follows it.",
                Capability::EditSequence,
                graphicClipParams({{"layer", layerIdParam("The text layer.")}, {"name", schema::string("The style's name (unique).")}},
                                  {"layer", "name"}),
                Json{{"clip", 42}, {"layer", 2}, {"name", "Lower third name"}}, {},
                [](ExecutionContext& c) {
                    const ClipId clip = c.clip("clip");
                    return graphicEdited(c, c.take(edit::createTextStyle(c.state(), c.sequenceId(), clip,
                                                                        static_cast<std::uint32_t>(c.integer("layer")), c.string("name"))),
                                         clip, Json{{"textStyle", c.string("name")}});
                }});
    addBuiltin(registry,
               {"textStyle.apply", 1, "Apply Text Style",
                "Gives text layers of a graphic clip a project text style; they follow it (updating the style restyles them).",
                Capability::EditSequence,
                graphicClipParams({{"layers", schema::array(layerIdParam("A text layer."), "The text layers.", 1)},
                                   {"name", schema::string("The text style.")}},
                                  {"layers", "name"}),
                Json{{"clip", 42}, {"layers", Json::array({2, 3})}, {"name", "Lower third name"}}, {},
                [](ExecutionContext& c) {
                    const ClipId clip = c.clip("clip");
                    std::vector<std::uint32_t> layers;
                    for (const Json& id : c.params()["layers"]) {
                        layers.push_back(static_cast<std::uint32_t>(id.get<std::int64_t>()));
                    }
                    return graphicEdited(c, c.take(edit::applyTextStyle(c.state(), c.sequenceId(), clip, layers, c.string("name"))), clip);
                }});
    addBuiltin(
        registry,
        {"textStyle.update", 1, "Push to Text Style",
         "Updates a project text style - Premiere's Push to Text Style - from a text layer's current look (\"clip\" and "
         "\"layer\") or from its JSON (\"style\", as textStyle.list gives it), and restyles every text layer of every graphic "
         "clip that follows it. Makes the style when there is none of that name.",
         Capability::EditProject,
         withSequence({{"name", schema::string("The text style.")},
                       {"clip", clipParam("A graphic clip whose text layer has the look.")},
                       {"layer", layerIdParam("That text layer.")},
                       {"style", openObject("Or the look as JSON (textStyle.list's form, without the name).")}},
                      {"name"}),
         Json{{"name", "Lower third name"}, {"clip", 42}, {"layer", 2}}, {},
         [](ExecutionContext& c) {
             ProjectTextStyle style;
             if (c.has("clip")) {
                 const Graphic& g = graphicOf(c, c.clip("clip"));
                 const GraphicLayer* layer = g.findLayer(static_cast<std::uint32_t>(c.integer("layer")));
                 if (!layer || layer->kind != LayerKind::Text) {
                     fail(ErrorCode::InvalidParams, "\"layer\" must be a text layer of the clip", Json{{"path", "/layer"}});
                 }
                 style = edit::textStyleOf(*layer, c.string("name"));
             } else if (c.has("style")) {
                 Json j = c.params()["style"];
                 j["name"] = c.string("name");
                 style = parsed(io::textStyleFromJson(j.dump()), "/style");
             } else {
                 fail(ErrorCode::InvalidParams, "give the look: \"clip\" and \"layer\", or \"style\"");
             }
             edit::EditResult r = c.take(edit::saveTextStyle(c.state(), style));
             return edited(std::move(r), Json{{"textStyle", style.name}});
         }});
    addBuiltin(registry,
               {"textStyle.rename", 1, "Rename Text Style",
                "Renames a project text style; the layers that follow it follow the new name.", Capability::EditProject,
                withSequence({{"name", schema::string("The text style.")}, {"to", schema::string("The new name.")}}, {"name", "to"}),
                Json{{"name", "Lower third name"}, {"to", "Name"}}, {},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::renameTextStyle(c.state(), c.string("name"), c.string("to"))),
                                  Json{{"textStyle", c.string("to")}});
                }});
    addBuiltin(registry,
               {"textStyle.remove", 1, "Delete Text Style",
                "Deletes a project text style: the layers that followed it keep their look and follow nothing.",
                Capability::EditProject, withSequence({{"name", schema::string("The text style.")}}, {"name"}),
                Json{{"name", "Name"}}, {},
                [](ExecutionContext& c) { return edited(c.take(edit::removeTextStyle(c.state(), c.string("name")))); }});
    // ---- Templates ----------------------------------------------------------------------------------------------------
    addBuiltin(
        registry,
        {"template.save", 1, "Save as Template",
         "Saves a graphic clip's graphic as a template with editable fields (Premiere's Motion Graphics Templates): into "
         "the project's library (default), or the user's template library folder (\"library\": \"user\"; the host's, "
         "not a path). Saving under an id in use replaces that template and raises its version.",
         Capability::EditProject,
         graphicClipParams({{"id", schema::string("The template's id: [A-Za-z0-9][A-Za-z0-9_.-]*, at most 128 bytes.")},
                            {"name", schema::string("Its name.")},
                            {"description", schema::string("A description.")},
                            {"fields", fieldListSchema()},
                            {"duration", timeParam("Default clip length (default: the responsive duration, else 5 s).")},
                            {"library", schema::enumeration({"project", "user"}, "Where it goes. Default \"project\".")}},
                           {"id", "name"}),
         Json{{"clip", 42},
              {"id", "lower-third"},
              {"name", "Lower third"},
              {"fields", Json::array({Json{{"id", "name"}, {"label", "Name"}, {"kind", "text"}, {"layer", 2}, {"property", "text"}},
                                      Json{{"id", "boxColor"}, {"label", "Box"}, {"kind", "color"}, {"layer", 1}, {"property", "fill"}}})}},
         {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             const Graphic& g = graphicOf(c, clip);
             const Ticks duration = c.optionalTime("duration").value_or(Ticks{});
             GraphicTemplate tmpl =
                 parsed(edit::makeTemplate(g, c.string("id"), c.string("name"), fieldsParam(c), duration, c.string("description")), "/fields");
             if (c.string("library", "project") == "user") {
                 const std::filesystem::path folder = userLibrary(c, true);
                 for (const io::LibraryTemplate& existing : io::readTemplateLibrary(folder)) {
                     if (existing.tmpl && existing.tmpl->id == tmpl.id) {
                         tmpl.version = std::max(tmpl.version, existing.tmpl->version + 1);
                     }
                 }
                 Json data{{"template", tmpl.id}, {"version", tmpl.version}, {"library", "user"}};
                 const std::filesystem::path file = folder / pathFromUtf8(tmpl.id + std::string(io::kTemplateFileSuffix));
                 data["file"] = pathToUtf8(file);
                 if (!c.dryRun()) {
                     c.take(io::writeTemplateToLibrary(folder, tmpl));
                 }
                 return readData(c, std::move(data));
             }
             edit::EditResult r = c.take(edit::saveProjectTemplate(c.state(), tmpl));
             const int version = r.state.findTemplate(tmpl.id)->version;
             return edited(std::move(r), Json{{"template", tmpl.id}, {"version", version}, {"library", "project"}});
         }});
    addBuiltin(registry,
               {"template.list", 1, "Templates",
                "The graphic templates of the project's library, the user's and the built-in ones that ship with Wabcut (C.16: "
                "gaming lower thirds, banners, callouts, stingers, end cards...; 0.4: essay and documentary name and title, "
                "location, quote, source and chapter cards that follow the brand kit): id, name, version, default length, canvas, "
                "and the fields (id, label, kind, default value) template.instantiate and template.setField take. Built-ins "
                "also give their category and \"posterTime\" (the design time a thumbnail shows: template.resolve draws it). "
                "With \"id\", that template only, with its graphic.",
                Capability::Read,
                withSequence({{"library", schema::enumeration({"project", "user", "builtin", "all"}, "Which library. Default \"all\".")},
                              {"id", schema::string("Only this template, with its graphic.")}}),
                Json{{"library", "all"}}, {},
                [](ExecutionContext& c) {
                    const std::string library = c.string("library", "all");
                    const std::string only = c.string("id");
                    const auto entry = [&](const GraphicTemplate& t) {
                        Json e = templateSummary(t);
                        if (!only.empty()) {
                            e["graphic"] = asJson(io::graphicToJson(t.graphic));
                        }
                        return e;
                    };
                    Json data{{"project", Json::array()}, {"user", Json::array()}, {"builtin", Json::array()}, {"errors", Json::array()}};
                    if (library == "all" || library == "builtin") { // C.16
                        for (const GraphicTemplate& t : builtinTemplates()) {
                            if (only.empty() || t.id == only) {
                                Json e = entry(t);
                                e["category"] = std::string(templateCategory(t));
                                e["posterTime"] = templatePosterTime(t).value;
                                data["builtin"].push_back(std::move(e));
                            }
                        }
                    }
                    if (library == "all" || library == "project") {
                        for (const GraphicTemplate& t : c.state().templates) {
                            if (only.empty() || t.id == only) {
                                data["project"].push_back(entry(t));
                            }
                        }
                    }
                    const std::filesystem::path folder = userLibrary(c, false);
                    data["userLibrary"] = folder.empty() ? Json(nullptr) : Json(pathToUtf8(folder));
                    if ((library == "all" || library == "user") && !folder.empty()) {
                        for (const io::LibraryTemplate& lt : io::readTemplateLibrary(folder)) {
                            if (!lt.tmpl) {
                                data["errors"].push_back(Json{{"file", pathToUtf8(lt.file)}, {"message", lt.tmpl.error().message}});
                            } else if (only.empty() || lt.tmpl->id == only) {
                                Json e = entry(*lt.tmpl);
                                e["file"] = pathToUtf8(lt.file);
                                data["user"].push_back(std::move(e));
                            }
                        }
                    }
                    return readData(c, std::move(data));
                }});
    addBuiltin(
        registry,
        {"template.resolve", 1, "Template Draw List",
         "The draw list of a template at a design time, without adding it to a timeline: what template browsers draw for "
         "thumbnails and hover previews (C.16). Default time: the template's poster time (everything has animated in). "
         "\"values\" previews field values. Same items as graphic.resolve.",
         Capability::Read,
         withSequence({{"template", schema::string("The template's id.")},
                       {"library", schema::enumeration({"project", "user", "builtin"}, "Only this library.")},
                       {"time", schema::integer("Design time in ticks from the template's start. Default its poster time.", 0)},
                       {"values", openObject("Field values by field id, as template.instantiate takes them.")}},
                      {"template"}),
         Json{{"template", "wabcut.gaming.lowerThird"}, {"values", Json{{"name", "Somchai"}}}}, {},
         [](ExecutionContext& c) {
             const FoundTemplate found = findTemplate(c, c.string("template"), c.string("library"));
             const GraphicTemplate& t = found.tmpl;
             Graphic graphic = t.graphic;
             graphic.instance = TemplateInstance{t.id, t.version, t.fields};
             const auto values = fieldValues(c, t.fields, c.has("values") ? c.params()["values"] : Json::object(), "/values");
             if (!values.empty()) {
                 auto with = edit::withFieldValues(c.state(), graphic, values);
                 if (!with) {
                     fail(ErrorCode::InvalidParams, with.error().message, Json{{"path", "/values"}});
                 }
                 graphic = std::move(*with);
             }
             const Ticks time = c.has("time") ? Ticks{c.params()["time"].get<std::int64_t>()} : templatePosterTime(t);
             ResolveOptions options;
             if (const GraphicsServices* s = graphicsServices(c)) {
                 options.measurer = s->measurer();
             }
             if (const auto* captions = findService<CaptionServices>(c.services())) {
                 options.lineBreaker = &captions->lineBreaker();
             }
             options.brandKit = c.state().brandKit ? &*c.state().brandKit : nullptr;
             const DrawList list = resolveGraphic(graphic, time, options);
             Json items = Json::array();
             for (const DrawItem& item : list.items) {
                 items.push_back(drawItemJson(item));
             }
             return readData(c, Json{{"template", t.id},
                                     {"library", found.library},
                                     {"time", time.value},
                                     {"width", list.width},
                                     {"height", list.height},
                                     {"items", std::move(items)}});
         }});
    addBuiltin(
        registry,
        {"template.instantiate", 1, "Add Template",
         "Adds a graphic made from a template (template.list) as a clip, with field values (by field id: text a string, "
         "colour \"#RRGGBB\" or [r, g, b, a] 0..1 or {\"brand\": name}, font a family or {\"family\", \"style\", "
         "\"weight\"}, image a media id or name, duration a time, checkbox true/false). Its length: a \"clip\" duration "
         "field, else \"duration\", else the template's. The project's library is searched first, then the user's, then "
         "the built-ins (template.list).",
         Capability::EditSequence,
         withSequence({{"template", schema::string("The template's id.")},
                       {"library", schema::enumeration({"project", "user", "builtin"}, "Only this library.")},
                       {"at", timeParam("Where it starts.")},
                       {"duration", timeParam("How long it lasts.")},
                       {"track", trackParam("A video track. Default: the first targeted unlocked video track.")},
                       {"placement", placementParam()},
                       {"values", openObject("Field values by field id.")}},
                      {"template", "at"}),
         Json{{"template", "lower-third"}, {"at", "00:00:12:00"}, {"track", "V2"}, {"values", Json{{"name", "Somchai Jaidee"}}}}, {},
         [](ExecutionContext& c) {
             const FoundTemplate found = findTemplate(c, c.string("template"), c.string("library"));
             const GraphicTemplate& t = found.tmpl;
             const auto values = fieldValues(c, t.fields, c.has("values") ? c.params()["values"] : Json::object(), "/values");
             const std::optional<Ticks> duration = c.optionalTime("duration");
             Ticks length = duration.value_or(t.duration.value > 0 ? t.duration
                                              : t.graphic.responsive ? t.graphic.responsive->duration
                                                                     : edit::kDefaultStillDuration);
             for (const auto& [id, value] : values) {
                 const TemplateField* f = fieldNamed(t.fields, id);
                 if (f->kind == FieldKind::Duration && f->property == "clip" && value.duration.value > 0) {
                     length = value.duration;
                 }
             }
             const Ticks at = c.time("at");
             ProjectState state = c.state();
             const TrackId track = videoPlacementTrack(c, state, at, length);
             edit::EditResult r = c.take(edit::instantiateTemplate(state, c.sequenceId(), track, at, duration, t, values));
             const ClipId clip = r.placed.front();
             return graphicEdited(c, std::move(r), clip, Json{{"library", found.library}});
         }});
    addBuiltin(registry,
               {"template.setField", 1, "Edit Template Fields",
                "Sets field values of a template instance clip (template.list shows the fields): \"field\" and \"value\", or "
                "several at once in \"values\" - one undo step. A \"clip\" duration field trims the clip's end.",
                Capability::EditSequence,
                graphicClipParams({{"field", schema::string("A field id.")},
                                   {"value", Json{{"description", "The field's value (as template.instantiate takes it)."}}},
                                   {"values", openObject("Field values by field id.")}},
                                  {}),
                Json{{"clip", 42}, {"field", "name"}, {"value", "Malee"}}, {},
                [](ExecutionContext& c) {
                    const ClipId clip = c.clip("clip");
                    const Graphic& g = graphicOf(c, clip);
                    if (!g.instance) {
                        fail(ErrorCode::InvalidParams, "the graphic is not made from a template", Json{{"path", "/clip"}});
                    }
                    std::vector<std::pair<std::string, FieldValue>> values;
                    if (c.has("field")) {
                        const std::string id = c.string("field");
                        const TemplateField* f = fieldNamed(g.instance->fields, id);
                        if (!f) {
                            fail(ErrorCode::InvalidParams, "the template has no field \"" + id + "\"", Json{{"path", "/field"}});
                        }
                        if (!c.params().contains("value")) {
                            fail(ErrorCode::InvalidParams, "\"field\" needs a \"value\"", Json{{"path", "/value"}});
                        }
                        values.emplace_back(id, fieldValue(c, *f, c.params()["value"], "/value"));
                    }
                    if (c.has("values")) {
                        for (auto& v : fieldValues(c, g.instance->fields, c.params()["values"], "/values")) {
                            values.push_back(std::move(v));
                        }
                    }
                    if (values.empty()) {
                        fail(ErrorCode::InvalidParams, "give \"field\" and \"value\", or \"values\"");
                    }
                    return graphicEdited(c, c.take(edit::setTemplateFields(c.state(), c.sequenceId(), clip, values)), clip);
                }});

    // ---- Template files (0.2): import (.mogrt made in Premiere Pro, Wabcut template files) and export -----------------
    addBuiltin(
        registry,
        command::touchesFiles(
            {"template.import", 1, "Import Motion Graphics Template",
             "Adds a template file to the project's library (default) or the user's: a Wabcut template file "
             "(\".wabcut-template.json\", template.export) as it is, or a Motion Graphics template (\".mogrt\") made in "
             "Premiere Pro, approximately - its text Essential Properties become text fields of a lower third Wabcut lays "
             "out, the first colour the box's, checkboxes show it; the report lists what was approximated or dropped. "
             "Templates made in After Effects are refused (Wabcut cannot run After Effects compositions: export them as "
             "Lottie). Files are untrusted: the ZIP is read bounded, with no path leaving it.",
             Capability::EditProject,
             withSequence({{"path", schema::string("The .mogrt or .wabcut-template.json file.")},
                           {"library", schema::enumeration({"project", "user"}, "Where it goes. Default \"project\".")},
                           {"id", schema::string("Give it this id (default: the file's).")}},
                          {"path"}),
             Json{{"path", "D:/Templates/Lower Third.mogrt"}}, {},
             [](ExecutionContext& c) {
                 const std::filesystem::path file = pathFromUtf8(c.string("path"));
                 std::error_code ec;
                 const std::uintmax_t size = std::filesystem::file_size(file, ec);
                 if (ec) {
                     fail(ErrorCode::NotFound, "cannot read \"" + c.string("path") + "\"", Json{{"path", "/path"}});
                 }
                 if (size > interchange::kMaxMogrtBytes) {
                     fail(ErrorCode::InvalidParams, "the file is larger than 64 MiB", Json{{"path", "/path"}});
                 }
                 std::ifstream in(file, std::ios::binary);
                 std::string bytes(static_cast<std::size_t>(size), '\0');
                 in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                 bytes.resize(static_cast<std::size_t>(in.gcount()));
                 std::string ext = pathToUtf8(file.extension());
                 std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                 GraphicTemplate tmpl;
                 Json data = Json::object();
                 if (ext == ".mogrt") {
                     auto imported = interchange::importMogrt(bytes, pathToUtf8(file.filename()));
                     if (!imported) {
                         fail(imported.error().code == wabcut::ErrorCode::Unsupported ? ErrorCode::EditRejected : ErrorCode::InvalidParams,
                              imported.error().message, Json{{"path", "/path"}});
                     }
                     tmpl = std::move(imported->tmpl);
                     data["authorApp"] = imported->authorApp;
                     data["report"] = reportJson(imported->report);
                 } else {
                     tmpl = parsed(io::templateFileFromJson(bytes), "/path");
                 }
                 if (c.has("id")) {
                     tmpl.id = c.string("id");
                 }
                 if (const auto problem = templateProblem(tmpl)) {
                     fail(ErrorCode::InvalidParams, *problem, Json{{"path", "/id"}});
                 }
                 data["template"] = tmpl.id;
                 data["name"] = tmpl.name;
                 Json fields = Json::array();
                 for (const TemplateField& f : tmpl.fields) {
                     fields.push_back(Json{{"id", f.id}, {"label", f.label}, {"kind", std::string(toString(f.kind))}});
                 }
                 data["fields"] = std::move(fields);
                 if (c.string("library", "project") == "user") {
                     const std::filesystem::path folder = userLibrary(c, true);
                     data["library"] = "user";
                     if (!c.dryRun()) {
                         c.take(io::writeTemplateToLibrary(folder, tmpl));
                     }
                     return readData(c, std::move(data));
                 }
                 edit::EditResult r = c.take(edit::saveProjectTemplate(c.state(), tmpl));
                 data["library"] = "project";
                 data["version"] = r.state.findTemplate(tmpl.id)->version;
                 return edited(std::move(r), std::move(data));
             }},
            {"path"}));
    addBuiltin(registry,
               command::touchesFiles(
                   {"template.export", 1, "Export Template",
                    "Writes a template (template.list: the project's, the user's or a built-in) to a Wabcut template file "
                    "(\".wabcut-template.json\") another Wabcut imports with template.import. Refuses to replace a file "
                    "unless \"overwrite\"; a dry run writes nothing.",
                    Capability::Read,
                    withSequence({{"template", schema::string("The template's id.")},
                                  {"library", schema::enumeration({"project", "user", "builtin"}, "Only this library.")},
                                  {"path", schema::string("The file to write.")},
                                  {"overwrite", schema::boolean("Replace an existing file. Default false.")}},
                                 {"template", "path"}),
                    Json{{"template", "lower-third"}, {"path", "D:/Templates/lower-third.wabcut-template.json"}}, {},
                    [](ExecutionContext& c) {
                        const FoundTemplate found = findTemplate(c, c.string("template"), c.string("library"));
                        const std::filesystem::path file = pathFromUtf8(c.string("path"));
                        std::error_code ec;
                        if (std::filesystem::exists(file, ec) && !c.boolean("overwrite", false)) {
                            fail(ErrorCode::EditRejected, "the file exists; pass \"overwrite\": true to replace it", Json{{"path", "/path"}});
                        }
                        if (!c.dryRun()) {
                            if (auto written = io::writeFileAtomically(file, io::templateFileToJson(found.tmpl)); !written) {
                                fail(ErrorCode::EditRejected, "cannot write the template: " + written.error().message, Json{{"path", "/path"}});
                            }
                        }
                        return readData(c, Json{{"template", found.tmpl.id}, {"library", found.library}, {"path", pathToUtf8(file)}});
                    }},
                   {"path"}));
    // ---- Brand kit ----------------------------------------------------------------------------------------------------
    addBuiltin(registry,
               {"brand.set", 1, "Set Brand Kit",
                "Sets the project's brand kit - named colours, font roles and logos (media of the project) that graphics, "
                "templates and caption styles refer to by name - or removes it (null). Everything that refers to it "
                "re-themes.",
                Capability::EditProject,
                withSequence({{"kit", Json{{"type", Json::array({"object", "null"})},
                                             {"description", "{\"name\", \"colors\": [{\"name\", \"rgba\": [r, g, b, a]}], "
                                                             "\"fonts\": [{\"role\", \"family\", \"style\", \"weight\"}], "
                                                             "\"logos\": [{\"name\", \"media\": id or name}]}, or null."}}}},
                               {"kit"}),
                Json{{"kit", Json{{"name", "Studio"},
                                  {"colors", Json::array({Json{{"name", "Primary"}, {"rgba", Json::array({0.9, 0.1, 0.4, 1.0})}}})},
                                  {"fonts", Json::array({Json{{"role", "heading"}, {"family", "Kanit"}, {"weight", 700}}})}}}},
                {},
                [](ExecutionContext& c) {
                    const Json& kitJson = c.params()["kit"];
                    if (kitJson.is_null()) {
                        return edited(c.take(edit::setBrandKit(c.state(), std::nullopt)));
                    }
                    Json kit = kitJson;
                    if (kit.contains("logos") && kit["logos"].is_array()) {
                        for (std::size_t i = 0; i < kit["logos"].size(); ++i) {
                            Json& logo = kit["logos"][i];
                            if (logo.is_object() && logo.contains("media") && logo["media"].is_string()) {
                                logo["media"] = c.mediaValue(logo["media"], "/kit/logos/" + std::to_string(i) + "/media").value;
                            }
                        }
                    }
                    BrandKit parsedKit = parsed(io::brandKitFromJson(kit.dump()), "/kit");
                    return edited(c.take(edit::setBrandKit(c.state(), std::move(parsedKit))));
                }});
    addBuiltin(registry,
               {"brand.get", 1, "Brand Kit", "The project's brand kit (null when it has none).", Capability::Read,
                withSequence({}), Json::object(), {},
                [](ExecutionContext& c) {
                    const auto& kit = c.state().brandKit;
                    return readData(c, Json{{"kit", kit ? asJson(io::brandKitToJson(*kit)) : Json(nullptr)}});
                }});

    // ---- Lottie -------------------------------------------------------------------------------------------------------
    addBuiltin(
        registry,
        command::touchesFiles(
            {"lottie.import", 1, "Import Lottie",
             "Imports a Lottie animation (Bodymovin JSON from After Effects, LottieFiles) as a motion graphic: from a file "
             "(\"path\") or the JSON itself (\"data\"). With \"at\" it becomes a clip there; with \"saveAs\" a template in "
             "the project's library (both are allowed). Shapes, fills, strokes, gradients, trims, transforms with their "
             "easing, text, masks and precomps come in; the report lists what was approximated or dropped (images, "
             "effects, mattes, expressions...).",
             Capability::EditProject,
             withSequence({{"path", schema::string("The .json file (UTF-8 path).")},
                           {"data", schema::string("The Lottie JSON itself.")},
                           {"at", timeParam("Place it as a clip starting here.")},
                           {"duration", timeParam("The clip's length (default the animation's).")},
                           {"track", trackParam("A video track. Default: the first targeted unlocked video track.")},
                           {"placement", placementParam()},
                           {"saveAs", schema::object({{"id", schema::string("Template id.")},
                                                      {"name", schema::string("Template name.")},
                                                      {"description", schema::string("Description.")}},
                                                     {"id", "name"})}}),
             Json{{"path", "D:/Motion/confetti.json"}, {"at", "00:00:03:00"}, {"track", "V3"}}, {},
             [](ExecutionContext& c) {
                 if (c.has("path") == c.has("data")) {
                     fail(ErrorCode::InvalidParams, "give \"path\" or \"data\"");
                 }
                 if (!c.has("at") && !c.has("saveAs")) {
                     fail(ErrorCode::InvalidParams, "give \"at\" to place the animation or \"saveAs\" to keep it as a template");
                 }
                 const std::string text = c.has("path") ? readLottieFile(c.string("path")) : c.string("data");
                 const std::string name = c.has("path") ? pathToUtf8(pathFromUtf8(c.string("path")).filename()) : "data";
                 interchange::LottieImport imported =
                     parsed(interchange::importLottie(text, name), c.has("path") ? "/path" : "/data");
                 if (imported.graphic.name.empty()) {
                     imported.graphic.name = c.has("path") ? pathToUtf8(pathFromUtf8(c.string("path")).stem()) : "Lottie";
                 }
                 Json data{{"report", reportJson(imported.report)},
                           {"duration", imported.duration.value},
                           {"frameRate", std::to_string(imported.frameRate.num) + "/" + std::to_string(imported.frameRate.den)},
                           {"width", imported.graphic.width},
                           {"height", imported.graphic.height}};
                 ProjectState state = c.state();
                 std::vector<std::uint64_t> created;
                 if (c.has("saveAs")) {
                     const Json& as = c.params()["saveAs"];
                     GraphicTemplate tmpl = parsed(edit::makeTemplate(imported.graphic, as.value("id", std::string()),
                                                                      as.value("name", std::string()), {}, imported.duration,
                                                                      as.value("description", std::string())),
                                                   "/saveAs");
                     state = c.take(edit::saveProjectTemplate(state, tmpl)).state;
                     data["template"] = tmpl.id;
                 }
                 if (c.has("at")) {
                     const Sequence& seq = c.sequence();
                     const Ticks at = c.time("at");
                     // A whole number of the sequence's frames, at least one.
                     const std::int64_t frames =
                         std::max<std::int64_t>(1, ticksToFrames(imported.duration, seq.frameRate, Rounding::Ceil).value_or(1));
                     const Ticks length = c.optionalTime("duration").value_or(
                         framesToTicks(frames, seq.frameRate, Rounding::Nearest).value_or(imported.duration));
                     const TrackId track = videoPlacementTrack(c, state, at, length);
                     edit::EditResult r = c.take(edit::addGraphic(state, c.sequenceId(), track, at, length, imported.graphic));
                     const ClipId clip = r.placed.front();
                     data.update(graphicResult(c, r.state, clip));
                     state = std::move(r.state);
                     created.push_back(clip.value);
                     if (r.createdMedia.isValid()) {
                         created.push_back(r.createdMedia.value);
                         data["media"] = r.createdMedia.value;
                     }
                 }
                 CommandOutput out;
                 out.state = std::move(state);
                 out.data = std::move(data);
                 out.createdIds = std::move(created);
                 if (imported.report.count(interchange::ReportOutcome::Dropped) > 0) {
                     out.warnings.push_back(std::to_string(imported.report.count(interchange::ReportOutcome::Dropped)) +
                                            " Lottie feature(s) were not imported (see report)");
                 }
                 return out;
             }}, {"path"}));
}

} // namespace wabcut::command::detail
