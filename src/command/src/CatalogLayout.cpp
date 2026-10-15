// Gaming layouts and sequence presets (C.15): layout.apply (facecam picture-in-picture, vertical shorts layouts),
// layout.list, sequence.create (from a preset: 1080p/1440p/4K at 30-240 fps, Shorts 1080x1920) and sequence.presets.

#include "Catalog.h"

#include <wabcut/model/Layouts.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequencePresets.h>

#include <algorithm>
#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

std::vector<std::string> presetIds() {
    std::vector<std::string> out;
    for (const SequencePreset& p : sequencePresets()) {
        out.emplace_back(p.id);
    }
    return out;
}

Json rateJson(Rate r) {
    return Json{{"num", r.num}, {"den", r.den}, {"fps", static_cast<double>(r.num) / static_cast<double>(r.den)}};
}

/// A number param inside [lo, hi] (the schema says the range; this guards hand-made requests too).
double numberIn(ExecutionContext& c, std::string_view key, double fallback) {
    return c.has(key) ? c.number(key) : fallback;
}

LayoutOptions layoutOptions(ExecutionContext& c) {
    LayoutOptions o;
    if (c.has("corner")) {
        const auto corner = layoutCornerFromString(c.string("corner"));
        if (!corner) {
            fail(ErrorCode::InvalidParams, "unknown corner");
        }
        o.corner = *corner;
    }
    o.size = numberIn(c, "size", o.size);
    o.margin = numberIn(c, "margin", o.margin);
    o.circle = c.boolean("circle", o.circle);
    o.cornerRadius = numberIn(c, "cornerRadius", o.cornerRadius);
    o.borderWidth = numberIn(c, "borderWidth", o.borderWidth);
    if (c.has("borderColor")) {
        const Json& color = c.params().at("borderColor");
        if (!color.is_array() || color.size() != 3 || !std::all_of(color.begin(), color.end(), [](const Json& v) { return v.is_number(); })) {
            fail(ErrorCode::InvalidParams, "borderColor must be [red, green, blue], each 0..1");
        }
        o.borderColor = {color[0].get<double>(), color[1].get<double>(), color[2].get<double>()};
    }
    o.shadow = c.boolean("shadow", o.shadow);
    o.split = numberIn(c, "split", o.split);
    o.facecamOnTop = c.boolean("facecamOnTop", o.facecamOnTop);
    o.blur = numberIn(c, "blur", o.blur);
    o.zoom = numberIn(c, "zoom", o.zoom);
    if (c.has("facecamRegion")) {
        const Json& r = c.params().at("facecamRegion");
        for (const char* key : {"x", "y", "width", "height"}) {
            if (!r.contains(key) || !r[key].is_number()) {
                fail(ErrorCode::InvalidParams, std::string("facecamRegion.") + key + " must be a number");
            }
        }
        o.facecamRegion = NormalizedRect{r["x"].get<double>(), r["y"].get<double>(), r["width"].get<double>(),
                                         r["height"].get<double>()};
    }
    if (const auto problem = layoutOptionsProblem(o)) {
        fail(ErrorCode::InvalidParams, *problem);
    }
    return o;
}

Json rectJson() {
    Json s = schema::object({{"x", schema::number("Left edge, a fraction of the gameplay frame's width.", 0.0, 1.0)},
                             {"y", schema::number("Top edge, a fraction of its height.", 0.0, 1.0)},
                             {"width", schema::number("Width, a fraction of the frame's width.", 0.01, 1.0)},
                             {"height", schema::number("Height, a fraction of the frame's height.", 0.01, 1.0)}},
                            {"x", "y", "width", "height"});
    s["description"] = "Where the capture already shows the webcam, as a part of the gameplay frame (fractions). The facecam "
                       "is then a copy of the gameplay cropped to it; no facecam clip is needed.";
    return s;
}

} // namespace

void registerLayoutCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"layout.apply", 1, "Apply Layout",
         "Lays out a gameplay clip and a facecam in one step, with plain editable clip settings: \"pip\" (gameplay fills "
         "the frame, the facecam in a corner with rounded corners or a circle, a border and a drop shadow), "
         "\"verticalSplit\" (9:16: gameplay on top, facecam below, or the other way round) or \"verticalBlurFill\" (9:16: "
         "the gameplay fitted to the width over a blurred copy of itself filling the frame, an optional facecam on top). "
         "The facecam is a clip on a track above the gameplay's, or \"facecamRegion\": the part of the capture where the "
         "webcam already shows (a copy of the gameplay cropped to it). Copies of the gameplay go on free tracks above it "
         "(new tracks when none is free) and are linked with it. Applying again replaces the layout's own effects. One "
         "undo step. Make a Shorts sequence first with sequence.create preset \"shorts\".",
         Capability::EditSequence,
         withSequence({{"preset", schema::enumeration({"pip", "verticalSplit", "verticalBlurFill"}, "The layout.")},
                       {"gameplay", clipParam("The gameplay clip (video).")},
                       {"facecam", clipParam("The facecam clip (video, on a track above the gameplay's).")},
                       {"facecamRegion", rectJson()},
                       {"corner", schema::enumeration({"topLeft", "topRight", "bottomLeft", "bottomRight"},
                                                      "Where the facecam box goes. Default bottomRight.")},
                       {"size", schema::number("The facecam's width, a fraction of the frame width. Default 0.28.", 0.05, 1.0)},
                       {"margin", schema::number("Gap to the frame edges, a fraction of the frame height. Default 0.03.", 0.0, 0.4)},
                       {"circle", schema::boolean("A round facecam.", false)},
                       {"cornerRadius", schema::number("Rounded corners: a fraction of the box's shorter side. Default 0.12.", 0.0, 0.5)},
                       {"borderWidth", schema::number("Border width in sequence pixels (0 = none). Default 4.", 0.0, 100.0)},
                       {"borderColor", schema::array(schema::number("A channel, 0..1.", 0.0, 1.0),
                                                     "Border colour [red, green, blue]. Default white.", 3)},
                       {"shadow", schema::boolean("A drop shadow under the facecam.", true)},
                       {"split", schema::number("verticalSplit: the gameplay's share of the height. Default 0.6.", 0.2, 0.8)},
                       {"facecamOnTop", schema::boolean("verticalSplit: the facecam on top.", false)},
                       {"blur", schema::number("verticalBlurFill: the background's blur radius in pixels. Default 40.", 0.0, 500.0)},
                       {"zoom", schema::number("verticalBlurFill: the gameplay's scale over fit-to-width (above 1 crops its "
                                               "sides). Default 1.",
                                               1.0, 4.0)}},
                      {"preset", "gameplay"}),
         Json{{"preset", "pip"}, {"gameplay", 42}, {"facecam", 43}, {"corner", "bottomLeft"}, {"size", 0.3}}, {},
         [](ExecutionContext& c) {
             const auto preset = layoutPresetFromString(c.string("preset"));
             if (!preset) {
                 fail(ErrorCode::InvalidParams, "unknown layout preset");
             }
             LayoutRequest request;
             request.preset = *preset;
             request.gameplay = c.clip("gameplay");
             if (c.has("facecam")) {
                 request.facecam = c.clip("facecam");
             }
             request.options = layoutOptions(c);
             auto laid = edit::applyLayout(c.state(), c.sequenceId(), request);
             if (!laid) {
                 fail(laid.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::EditRejected,
                      laid.error().message);
             }
             Json data{{"preset", std::string(toString(*preset))}, {"gameplay", laid->gameplay.value}};
             if (laid->foreground.isValid()) {
                 data["foreground"] = laid->foreground.value;
             }
             if (laid->facecam.isValid()) {
                 data["facecam"] = laid->facecam.value;
             }
             return edited(std::move(laid->edit), std::move(data));
         }});
    addBuiltin(
        registry,
        {"layout.list", 1, "List Layouts",
         "The layout presets layout.apply takes and their options with defaults: facecam picture-in-picture and the "
         "vertical 9:16 shorts layouts.",
         Capability::Read, withSequence({}), Json::object(), {},
         [](ExecutionContext& c) {
             const LayoutOptions d;
             Json layouts = Json::array();
             layouts.push_back(Json{{"preset", "pip"},
                                    {"needsFacecam", true},
                                    {"description", "Gameplay fills the frame; facecam in a corner (rounded or a circle, "
                                                    "border, shadow)."}});
             layouts.push_back(Json{{"preset", "verticalSplit"},
                                    {"needsFacecam", true},
                                    {"description", "9:16: gameplay on top, facecam below (split, facecamOnTop)."}});
             layouts.push_back(Json{{"preset", "verticalBlurFill"},
                                    {"needsFacecam", false},
                                    {"description", "9:16: gameplay fitted to the width over a blurred copy filling the frame; "
                                                    "optional facecam box."}});
             CommandOutput out;
             out.state = c.state();
             out.data = Json{{"layouts", std::move(layouts)},
                             {"defaults", Json{{"corner", std::string(toString(d.corner))},
                                               {"size", d.size},
                                               {"margin", d.margin},
                                               {"circle", d.circle},
                                               {"cornerRadius", d.cornerRadius},
                                               {"borderWidth", d.borderWidth},
                                               {"borderColor", Json::array({d.borderColor[0], d.borderColor[1], d.borderColor[2]})},
                                               {"shadow", d.shadow},
                                               {"split", d.split},
                                               {"facecamOnTop", d.facecamOnTop},
                                               {"blur", d.blur},
                                               {"zoom", d.zoom}}}};
             return out;
         }});

    // --- Sequence presets (SequencePresets.h) ---------------------------------------------------------------------
    addBuiltin(
        registry,
        {"sequence.presets", 1, "Sequence Presets",
         "The sequence presets sequence.create takes: landscape 1080p, 1440p and 4K at 30, 60, 120, 144 and 240 fps (and "
         "59.94), and vertical 1080x1920 for Shorts, TikTok and Reels at 60 (\"shorts\") and 30 fps. Every rate is exact "
         "in ticks.",
         Capability::Read, withSequence({}), Json::object(), {},
         [](ExecutionContext& c) {
             Json list = Json::array();
             for (const SequencePreset& p : sequencePresets()) {
                 list.push_back(Json{{"id", std::string(p.id)},
                                     {"name", std::string(p.name)},
                                     {"group", std::string(p.group)},
                                     {"width", p.width},
                                     {"height", p.height},
                                     {"frameRate", rateJson(p.frameRate)}});
             }
             CommandOutput out;
             out.state = c.state();
             out.data = Json{{"presets", std::move(list)}};
             return out;
         }});
    addBuiltin(
        registry,
        {"sequence.create", 1, "New Sequence",
         "Makes a new, empty sequence (3 video and 3 audio tracks, 48 kHz) from a preset (sequence.presets: \"1080p60\", "
         "\"1440p144\", \"2160p60\", \"shorts\" for 1080x1920 60 fps, ...). createdIds: the sequence.",
         Capability::EditProject,
         withSequence({{"preset", schema::enumeration(presetIds(), "The preset (sequence.presets).")},
                       {"name", schema::string("The sequence's name. Default: the preset's name.")},
                       {"bin", schema::integer("The bin to put it in (bin id). Default: the root.", 1)}},
                      {"preset"}),
         Json{{"preset", "shorts"}, {"name", "Clutch 1v4 (Short)"}}, {},
         [](ExecutionContext& c) {
             const SequencePreset* preset = findSequencePreset(c.string("preset"));
             if (!preset) {
                 fail(ErrorCode::InvalidParams, "unknown sequence preset");
             }
             edit::SequenceSettings settings = sequenceSettingsFor(*preset, c.string("name"));
             settings.bin = BinId{static_cast<std::uint64_t>(c.integer("bin", 0))};
             if (settings.bin.isValid() && !c.state().findBin(settings.bin)) {
                 fail(ErrorCode::NotFound, "bin " + std::to_string(settings.bin.value) + " does not exist");
             }
             auto created = edit::addSequence(c.state(), settings);
             if (!created) {
                 fail(ErrorCode::EditRejected, created.error().message);
             }
             CommandOutput out;
             out.state = std::move(created->state);
             out.createdIds.push_back(created->id.value);
             out.data = Json{{"sequence", created->id.value},
                             {"name", settings.name},
                             {"width", settings.width},
                             {"height", settings.height},
                             {"frameRate", rateJson(settings.frameRate)}};
             return out;
         }});
}

} // namespace wabcut::command::detail
