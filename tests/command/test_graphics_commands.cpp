// The motion graphics commands (C.13) through the engine: graphic.*, template.*, brand.* and lottie.import - dry runs,
// one undo step each, the JSON they take and give, and their refusals.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/GraphicJson.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPicture = R"(
    V2 tgt |..........|
    V1 tgt |Aaaaaaaaaa|
    A1     |Aaaaaaaaaa|
)";

Json lowerThird() {
    return Json{{"name", "Lower third"},
                {"layers",
                 Json::array({Json{{"id", 1}, {"kind", "shape"}, {"name", "Box"},
                                   {"shape", Json{{"type", "rectangle"}, {"width", 600}, {"height", 120},
                                                  {"fill", Json{{"color", Json{{"brand", "Primary"}, {"rgba", Json::array({0.2, 0.2, 0.2, 1})}}}}}}},
                                   {"transform", Json{{"x", 500}, {"y", 900}}}},
                              Json{{"id", 2}, {"kind", "text"}, {"name", "Name"},
                                   {"text", Json{{"runs", Json::array({Json{{"text", "Name"}, {"size", 64}, {"brandFont", "heading"}}})}}},
                                   {"transform", Json{{"x", 220}, {"y", 860}}}}})}};
}

std::uint64_t idOf(const command::CommandResult& r, const char* key) { return r.data[key].get<std::uint64_t>(); }

const Graphic& graphicIn(const CommandHarness& h, std::uint64_t clip) {
    const Graphic* g = h.sequence(h.state()).findClip(ClipId{clip})->clip().graphic();
    REQUIRE(g);
    return *g;
}

fs::path scratch(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "graphics-commands" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("graphic.add places a graphic clip in one undo step and graphic.get reads it", "[command][graphics]") {
    CommandHarness h(kPicture);
    const ProjectSnapshot original = h.state();

    const auto dry = h.run(Json{{"command", "graphic.add"},
                                {"params", Json{{"at", tc(2)}, {"duration", tc(5)}, {"track", "V2"}, {"graphic", lowerThird()}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK_FALSE(dry.committed);
    CHECK(dry.changes.addedMedia.size() == 1);
    CHECK(*h.state() == *original);

    const auto added = h.ok("graphic.add", Json{{"at", tc(2)}, {"duration", tc(5)}, {"track", "V2"}, {"graphic", lowerThird()}});
    const std::uint64_t clip = idOf(added, "clip");
    REQUIRE(added.createdIds.size() == 2); // the clip and the canvas media item
    CHECK(added.createdIds[0] == clip);
    CHECK(added.createdIds[1] == idOf(added, "media"));
    CHECK(added.data["start"] == tc(2));
    CHECK(added.data["end"] == tc(7));
    CHECK(added.data["layers"].size() == 2);
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "New Graphic");
    // The design canvas defaults to the sequence's frame.
    CHECK(graphicIn(h, clip).width == h.sequence(h.state()).width);

    const auto got = h.ok("graphic.get", Json{{"clip", clip}});
    CHECK(got.data["graphic"]["name"] == "Lower third");
    CHECK(got.data["layers"][1] == Json{{"id", 2}, {"kind", "text"}, {"name", "Name"}, {"text", "Name"}});

    // A second graphic reuses the canvas.
    const auto second = h.ok("graphic.add", Json{{"at", tc(8)}, {"track", "V2"}, {"duration", tc(2)}, {"name", "Bug"}});
    CHECK(second.createdIds.size() == 1);
    CHECK_FALSE(second.data.contains("media"));

    h.session.undo();
    h.session.undo();
    CHECK(*h.state() == *original);

    // Refusals: a bad graphic (with the path of the problem), not a graphic clip.
    Json bad = lowerThird();
    bad["layers"][0]["transform"]["opacity"] = 300;
    CHECK(h.errorOf("graphic.add", Json{{"at", tc(0)}, {"track", "V2"}, {"graphic", bad}}) == "InvalidParams");
    CHECK(h.errorOf("graphic.get", Json{{"clip", h.clip("V1@0").value}}) == "InvalidParams");
}

TEST_CASE("graphic layer commands edit one layer per undo step", "[command][graphics]") {
    CommandHarness h(kPicture);
    const std::uint64_t clip = idOf(h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(10)}, {"track", "V2"}, {"graphic", lowerThird()}}), "clip");
    const ProjectSnapshot before = h.state();

    // addLayer: a fresh id, on top or at an index.
    const auto added = h.ok("graphic.addLayer", Json{{"clip", clip},
                                                      {"layer", Json{{"kind", "shape"}, {"shape", Json{{"type", "ellipse"}, {"width", 40}}}}},
                                                      {"index", 0}});
    CHECK(added.data["layer"] == 3);
    CHECK(graphicIn(h, clip).layers.front().id == 3);
    CHECK(h.session.undoLabel() == "Add Graphic Layer");

    // updateLayer: a merge patch; keys not named stay.
    const auto dry = h.run(Json{{"command", "graphic.updateLayer"},
                                {"params", Json{{"clip", clip}, {"layer", 2}, {"set", Json{{"text", Json{{"runs", Json::array({Json{{"text", "Somchai"}}})}}}}}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    REQUIRE(dry.changes.sequences.size() == 1);
    CHECK(dry.changes.sequences[0].modifiedClips == std::vector<ClipId>{ClipId{clip}});
    h.ok("graphic.updateLayer", Json{{"clip", clip}, {"layer", 2}, {"set", Json{{"transform", Json{{"opacity", 50}, {"x", nullptr}}}}}});
    const GraphicLayer& name = *graphicIn(h, clip).findLayer(2);
    CHECK(name.transform.opacity == ParamValue(50.0));
    CHECK(name.transform.x == ParamValue(0.0)); // null: back to the default
    CHECK(name.transform.y == ParamValue(860.0));
    CHECK(name.text.text() == "Name");
    // A keyframed property through the same patch.
    h.ok("graphic.updateLayer",
         Json{{"clip", clip}, {"layer", 1}, {"set", Json{{"transform", Json{{"x", Json::array({Json{{"t", 0}, {"v", 0}}, Json{{"t", 254016000000}, {"v", 500}}})}}}}}});
    CHECK(graphicIn(h, clip).findLayer(1)->transform.x.curve() != nullptr);
    CHECK(h.errorOf("graphic.updateLayer", Json{{"clip", clip}, {"layer", 1}, {"set", Json{{"transform", Json{{"opacity", "full"}}}}}}) ==
          "InvalidParams");
    CHECK(h.errorOf("graphic.updateLayer", Json{{"clip", clip}, {"layer", 99}, {"set", Json::object()}}) == "NotFound");

    // applyPreset: keys written into the layer.
    h.ok("graphic.applyPreset", Json{{"clip", clip}, {"layer", 2}, {"preset", "typewriter"}, {"duration", tc(5)}, {"easing", "linear"}});
    const GraphicLayer& typed = *graphicIn(h, clip).findLayer(2);
    REQUIRE(typed.text.animators.size() == 1);
    CHECK(typed.text.animators[0].start.valueAt(f(5)) == 100.0);
    CHECK(h.session.undoLabel() == "Apply Animation Preset");
    CHECK(h.errorOf("graphic.applyPreset", Json{{"clip", clip}, {"layer", 1}, {"preset", "typewriter"}}) == "EditRejected");

    // graphic.resolve: the draw list at a time.
    const auto drawn = h.ok("graphic.resolve", Json{{"clip", clip}, {"time", tc(0)}});
    CHECK(drawn.data["width"] == 1920);
    const Json& items = drawn.data["items"];
    REQUIRE(items.size() == 3);
    CHECK(items[0]["layer"] == 3);
    CHECK(items[0]["kind"] == "shape");
    CHECK(items[2]["kind"] == "text");
    CHECK(items[2]["text"]["clusters"].size() == 4);
    CHECK(items[2]["text"]["clusters"][0]["opacity"] == 0.0); // the typewriter has not started
    CHECK(items[1]["fill"]["color"] == Json::array({0.2, 0.2, 0.2, 1.0}));

    // graphic.update: a patch over the whole graphic.
    h.ok("graphic.update", Json{{"clip", clip}, {"set", Json{{"name", "Renamed"}, {"responsive", Json{{"duration", f(10).value}, {"intro", f(2).value}, {"outro", f(2).value}}}}}});
    CHECK(graphicIn(h, clip).name == "Renamed");
    CHECK(graphicIn(h, clip).responsive.has_value());

    // removeLayer: the box's layer goes.
    h.ok("graphic.removeLayer", Json{{"clip", clip}, {"layer", 1}});
    CHECK_FALSE(graphicIn(h, clip).findLayer(1));
    CHECK(h.errorOf("graphic.removeLayer", Json{{"clip", clip}, {"layer", 1}}) == "NotFound");

    while (h.session.undoCount() > 1) {
        h.session.undo();
    }
    CHECK(*h.state() == *before);
}

TEST_CASE("templates are saved instantiated with field values and edited through their fields", "[command][graphics][templates]") {
    CommandHarness h(kPicture);
    const std::uint64_t clip = idOf(h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(10)}, {"track", "V2"}, {"graphic", lowerThird()}}), "clip");
    const Json fields = Json::array({Json{{"id", "name"}, {"label", "Name"}, {"kind", "text"}, {"layer", 2}, {"property", "text"}},
                                     Json{{"id", "box"}, {"label", "Box"}, {"kind", "color"}, {"layer", 1}, {"property", "fill"}},
                                     Json{{"id", "show"}, {"label", "Show box"}, {"kind", "checkbox"}, {"layer", 1}, {"property", "visible"}},
                                     Json{{"id", "length"}, {"label", "Length"}, {"kind", "duration"}, {"property", "clip"}}});
    const auto saved = h.ok("template.save", Json{{"clip", clip}, {"id", "lower-third"}, {"name", "Lower third"}, {"fields", fields},
                                                  {"duration", tc(6)}});
    CHECK(saved.data == Json{{"template", "lower-third"}, {"version", 1}, {"library", "project"}});
    CHECK(h.session.undoLabel() == "Save as Template");
    CHECK(h.ok("template.save", Json{{"clip", clip}, {"id", "lower-third"}, {"name", "Lower third"}, {"fields", fields}}).data["version"] == 2);

    const auto listed = h.ok("template.list");
    REQUIRE(listed.data["project"].size() == 1);
    const Json& summary = listed.data["project"][0];
    CHECK(summary["id"] == "lower-third");
    CHECK(summary["fields"].size() == 4);
    CHECK(summary["fields"][0]["default"] == "Name");
    CHECK(summary["fields"][2]["default"] == true);
    CHECK(listed.data["userLibrary"].is_null());
    CHECK(h.ok("template.list", Json{{"id", "lower-third"}}).data["project"][0].contains("graphic"));

    // Instantiate: values by kind, a timecode for the clip's length.
    const ProjectSnapshot beforeInstance = h.state();
    const auto made = h.ok("template.instantiate", Json{{"template", "lower-third"},
                                                         {"at", tc(0)},
                                                         {"track", "V2"},
                                                         {"placement", "newTrack"},
                                                         {"values", Json{{"name", "Somchai"}, {"box", "#ff0000"}, {"length", tc(4)}}}});
    const std::uint64_t instance = idOf(made, "clip");
    CHECK(made.data["template"] == "lower-third");
    CHECK(made.data["library"] == "project");
    CHECK(made.data["end"] == tc(4));
    const Graphic& g = graphicIn(h, instance);
    CHECK(g.findLayer(2)->text.text() == "Somchai");
    CHECK(g.findLayer(1)->shape.fill.color.rgba == std::array<double, 4>{1.0, 0.0, 0.0, 1.0});
    CHECK(h.session.undoLabel() == "Add Template");
    CHECK(h.errorOf("template.instantiate", Json{{"template", "nope"}, {"at", tc(0)}, {"track", "V2"}}) == "NotFound");
    CHECK(h.errorOf("template.instantiate", Json{{"template", "lower-third"}, {"at", tc(0)}, {"track", "V2"}, {"values", Json{{"nope", 1}}}}) ==
          "InvalidParams");
    CHECK(h.errorOf("template.instantiate", Json{{"template", "lower-third"}, {"at", tc(0)}, {"track", "V2"}, {"values", Json{{"box", "red"}}}}) ==
          "InvalidParams");

    // setField: one field, or several in one undo step.
    const std::size_t undos = h.session.undoCount();
    h.ok("template.setField", Json{{"clip", instance}, {"field", "name"}, {"value", "Malee"}});
    h.ok("template.setField", Json{{"clip", instance}, {"values", Json{{"show", false}, {"box", Json{{"brand", "Primary"}, {"rgba", Json::array({0, 0, 1, 1})}}}, {"length", tc(6)}}}});
    CHECK(h.session.undoCount() == undos + 2);
    CHECK(h.session.undoLabel() == "Edit Template Fields");
    const Graphic& edited = graphicIn(h, instance);
    CHECK(edited.findLayer(2)->text.text() == "Malee");
    CHECK_FALSE(edited.findLayer(1)->visible);
    CHECK(edited.findLayer(1)->shape.fill.color.brand == "Primary");
    CHECK(h.sequence(h.state()).findClip(ClipId{instance})->clip().duration() == f(6));
    CHECK(h.errorOf("template.setField", Json{{"clip", instance}, {"field", "nope"}, {"value", 1}}) == "InvalidParams");
    CHECK(h.errorOf("template.setField", Json{{"clip", instance}}) == "InvalidParams");
    CHECK(h.errorOf("template.setField", Json{{"clip", clip}, {"field", "name"}, {"value", "x"}}) == "InvalidParams"); // not an instance

    while (h.session.undoCount() > undos - 1) {
        h.session.undo();
    }
    CHECK(*h.state() == *beforeInstance);
}

TEST_CASE("the user template library is the host's folder and dry runs write nothing", "[command][graphics][templates]") {
    const fs::path library = scratch("library");
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::GraphicsServices>(library);
    CommandHarness h(kPicture, policy);
    const std::uint64_t clip = idOf(h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(10)}, {"track", "V2"}, {"graphic", lowerThird()}}), "clip");
    const Json params{{"clip", clip}, {"id", "user.lower-third"}, {"name", "Mine"}, {"library", "user"},
                      {"fields", Json::array({Json{{"id", "name"}, {"kind", "text"}, {"layer", 2}, {"property", "text"}}})}};

    const auto dry = h.run(Json{{"command", "template.save"}, {"params", params}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(dry.data["file"].get<std::string>().ends_with("user.lower-third.wabcut-template.json"));
    CHECK(fs::is_empty(library));

    const ProjectSnapshot before = h.state();
    const auto saved = h.ok("template.save", params);
    CHECK(saved.data["library"] == "user");
    CHECK(fs::exists(library / "user.lower-third.wabcut-template.json"));
    CHECK(*h.state() == *before); // the project did not change
    CHECK(h.ok("template.save", params).data["version"] == 2);
    {
        std::ofstream broken(library / "broken.wabcut-template.json");
        broken << "{";
    }

    const auto listed = h.ok("template.list", Json{{"library", "user"}});
    CHECK(listed.data["project"].empty());
    REQUIRE(listed.data["user"].size() == 1);
    CHECK(listed.data["user"][0]["version"] == 2);
    CHECK(listed.data["errors"].size() == 1);
    const auto made = h.ok("template.instantiate", Json{{"template", "user.lower-third"}, {"at", tc(0)}, {"track", "V2"},
                                                         {"values", Json{{"name", "From the library"}}}});
    CHECK(made.data["library"] == "user");
    CHECK(graphicIn(h, idOf(made, "clip")).findLayer(2)->text.text() == "From the library");

    // Without the service there is no user library.
    CommandHarness plain(kPicture);
    const std::uint64_t other = idOf(plain.ok("graphic.add", Json{{"at", tc(0)}, {"track", "V2"}, {"graphic", lowerThird()}}), "clip");
    Json noLibrary = params;
    noLibrary["clip"] = other;
    CHECK(plain.errorOf("template.save", noLibrary) == "InvalidRequest");
}

TEST_CASE("brand.set re-themes graphics and brand.get reads the kit", "[command][graphics][brand]") {
    CommandHarness h(kPicture);
    const std::uint64_t clip = idOf(h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(10)}, {"track", "V2"}, {"graphic", lowerThird()}}), "clip");
    const auto boxColor = [&] {
        for (const Json& item : h.ok("graphic.resolve", Json{{"clip", clip}}).data["items"]) {
            if (item["layer"] == 1) {
                return item["fill"]["color"];
            }
        }
        return Json();
    };
    CHECK(boxColor() == Json::array({0.2, 0.2, 0.2, 1.0}));
    CHECK(h.ok("brand.get").data["kit"].is_null());

    const Json kit{{"name", "Studio"},
                   {"colors", Json::array({Json{{"name", "Primary"}, {"rgba", Json::array({1, 0, 0, 1})}}})},
                   {"fonts", Json::array({Json{{"role", "heading"}, {"family", "Kanit"}, {"weight", 700}}})},
                   {"logos", Json::array({Json{{"name", "Main"}, {"media", "A"}}})}};
    const auto dry = h.run(Json{{"command", "brand.set"}, {"params", Json{{"kit", kit}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(dry.changes.brandKitChanged);
    h.ok("brand.set", Json{{"kit", kit}});
    CHECK(h.session.undoLabel() == "Set Brand Kit");
    CHECK(boxColor() == Json::array({1.0, 0.0, 0.0, 1.0}));
    const Json got = h.ok("brand.get").data["kit"];
    CHECK(got["logos"][0]["media"] == h.tl.media('A').value);
    CHECK(got["fonts"][0]["family"] == "Kanit");

    h.ok("brand.set", Json{{"kit", nullptr}});
    CHECK(boxColor() == Json::array({0.2, 0.2, 0.2, 1.0}));
    h.session.undo();
    CHECK(boxColor() == Json::array({1.0, 0.0, 0.0, 1.0}));

    Json duplicate = kit;
    duplicate["colors"].push_back(duplicate["colors"][0]);
    CHECK(h.errorOf("brand.set", Json{{"kit", duplicate}}) == "InvalidParams");
    Json missing = kit;
    missing["logos"][0]["media"] = "No such media";
    CHECK(h.errorOf("brand.set", Json{{"kit", missing}}) == "NotFound");

    // Caption styles name brand colours and a font role.
    h.ok("captions.addTrack", Json{{"name", "English"}, {"style", Json{{"brand", Json{{"fill", "Primary"}}}}}});
    const auto styled = h.ok("captions.setStyle", Json{{"style", Json{{"brand", Json{{"font", "heading"}}}}}});
    CHECK(styled.data["style"]["brand"]["fill"] == "Primary");
    CHECK(styled.data["style"]["brand"]["font"] == "heading");
    const CaptionStyle& style = h.sequence(h.state()).captionTracks.at(0).style;
    CHECK(resolvedCaptionStyle(style, &*h.state()->brandKit).fill == CaptionRgb{1.0, 0.0, 0.0});
    CHECK(resolvedCaptionStyle(style, &*h.state()->brandKit).fontFamily == "Kanit");
    CHECK(h.errorOf("captions.setStyle", Json{{"style", Json{{"brand", Json{{"fill", "two\nlines"}}}}}}) == "InvalidParams");
}

TEST_CASE("lottie.import places an animation and saves it as a template", "[command][graphics][lottie]") {
    CommandHarness h(kPicture);
    const fs::path fixture = fs::path(WABCUT_COMMAND_TEST_DIR) / ".." / "interchange" / "fixtures" / "lottie" / "lower_third.json";
    REQUIRE(fs::exists(fixture));

    const ProjectSnapshot before = h.state();
    const auto dry = h.run(Json{{"command", "lottie.import"},
                                {"params", Json{{"path", pathToUtf8(fixture)}, {"at", tc(0)}, {"track", "V2"}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*h.state() == *before);

    const auto imported = h.ok("lottie.import", Json{{"path", pathToUtf8(fixture)},
                                                      {"at", tc(1)},
                                                      {"track", "V2"},
                                                      {"saveAs", Json{{"id", "lottie-lower-third"}, {"name", "Lottie lower third"}}}});
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "Import Lottie");
    CHECK(imported.data["template"] == "lottie-lower-third");
    CHECK(imported.data["frameRate"] == "30/1");
    CHECK(imported.data["report"]["carried"].get<int>() >= 3);
    CHECK(imported.data["report"]["approximated"].get<int>() >= 1);
    // 3 s of animation at 25 fps: 75 frames.
    CHECK(imported.data["end"] == tc(76));
    const Graphic& g = graphicIn(h, idOf(imported, "clip"));
    CHECK(g.name == "Lower Third");
    CHECK(g.layers.size() == 4);
    CHECK(h.state()->findTemplate("lottie-lower-third") != nullptr);

    // Inline data; a template only.
    const std::string data = R"({"fr": 25, "ip": 0, "op": 25, "w": 100, "h": 100, "nm": "Dot", "layers": [
        {"ty": 4, "ip": 0, "op": 25, "ks": {}, "shapes": [{"ty": "el", "s": {"a": 0, "k": [10, 10]}}, {"ty": "fl", "c": {"a": 0, "k": [1, 0, 0, 1]}}]}]})";
    const auto onlyTemplate = h.ok("lottie.import", Json{{"data", data}, {"saveAs", Json{{"id", "dot"}, {"name", "Dot"}}}});
    CHECK_FALSE(onlyTemplate.data.contains("clip"));
    CHECK(h.state()->findTemplate("dot")->duration == Ticks::fromSeconds(1));

    // Refusals.
    CHECK(h.errorOf("lottie.import", Json{{"data", data}}) == "InvalidParams"); // neither placed nor saved
    CHECK(h.errorOf("lottie.import", Json{{"data", data}, {"path", "x.json"}, {"at", tc(0)}}) == "InvalidParams");
    CHECK(h.errorOf("lottie.import", Json{{"data", "{not json"}, {"at", tc(0)}, {"track", "V2"}}) == "InvalidParams");
    CHECK(h.errorOf("lottie.import", Json{{"path", pathToUtf8(scratch("missing") / "none.json")}, {"at", tc(0)}}) == "NotFound");
    // A host that holds back file commands refuses it.
    command::EnginePolicy noFiles;
    noFiles.allowFileCommands = false;
    CommandHarness locked(kPicture, noFiles);
    CHECK(locked.errorOf("lottie.import", Json{{"data", data}, {"saveAs", Json{{"id", "dot"}, {"name", "Dot"}}}}) != "");
}

// ---- 0.7: keys on template fields (card 71), styling part of a text and reordering text styles (card 81), template
// folders (card 87) ---------------------------------------------------------------------------------------------------------

TEST_CASE("template fields are keyed over the clip and read back (card 71)", "[command][graphics][templates][keys]") {
    CommandHarness h(kPicture);
    const std::uint64_t clip = idOf(h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(10)}, {"track", "V2"}, {"graphic", lowerThird()}}), "clip");
    const Json fields = Json::array({Json{{"id", "name"}, {"label", "Name"}, {"kind", "text"}, {"layer", 2}, {"property", "text"}},
                                     Json{{"id", "box"}, {"label", "Box"}, {"kind", "color"}, {"layer", 1}, {"property", "fill"}},
                                     Json{{"id", "length"}, {"label", "Length"}, {"kind", "duration"}, {"property", "clip"}}});
    h.ok("template.save", Json{{"clip", clip}, {"id", "lower-third"}, {"name", "Lower third"}, {"fields", fields}, {"duration", tc(6)}});
    const std::uint64_t instance = idOf(h.ok("template.instantiate", Json{{"template", "lower-third"}, {"at", tc(0)}, {"track", "V2"}, {"placement", "newTrack"}}), "clip");

    const std::size_t undos = h.session.undoCount();
    h.ok("template.setFieldKey", Json{{"clip", instance}, {"field", "box"}, {"time", tc(0)}, {"value", "#ff0000"}});
    h.ok("template.setFieldKey", Json{{"clip", instance}, {"field", "box"}, {"time", tc(4)}, {"value", "#0000ff"}});
    h.ok("template.setFieldKey", Json{{"clip", instance}, {"field", "name"}, {"time", tc(2)}, {"value", "Second"}, {"interpolation", "hold"}});
    CHECK(h.session.undoCount() == undos + 3); // one undo step each
    CHECK(h.session.undoLabel() == "Key a Template Field");
    const Graphic& g = graphicIn(h, instance);
    REQUIRE(g.instance->fields[1].keys.size() == 2);
    CHECK(g.instance->fields[1].keys[1].time == f(4));
    CHECK(fieldValueAt(g.instance->fields[1], f(2)).color.rgba[0] == Approx(0.5));
    // graphic.get lists the keys with the field.
    const Json shown = h.ok("graphic.get", Json{{"clip", instance}}).data["graphic"];
    CHECK(shown.dump().find("\"keys\"") != std::string::npos);

    // A static edit of an animated field says how to go on; other fields and unkeyed ones are as before.
    CHECK(h.errorOf("template.setField", Json{{"clip", instance}, {"field", "box"}, {"value", "#00ff00"}}) == "EditRejected");
    h.ok("template.setField", Json{{"clip", instance}, {"field", "length"}, {"value", tc(8)}});
    // Refusals: a length, an unknown field, a time outside the clip, not an instance, a bad colour.
    CHECK(h.errorOf("template.setFieldKey", Json{{"clip", instance}, {"field", "length"}, {"time", tc(1)}, {"value", tc(3)}}) == "EditRejected");
    CHECK(h.errorOf("template.setFieldKey", Json{{"clip", instance}, {"field", "nope"}, {"time", tc(1)}, {"value", "x"}}) == "InvalidParams");
    CHECK(h.errorOf("template.setFieldKey", Json{{"clip", instance}, {"field", "box"}, {"time", tc(30)}, {"value", "#ffffff"}}) == "InvalidParams");
    CHECK(h.errorOf("template.setFieldKey", Json{{"clip", clip}, {"field", "box"}, {"time", tc(1)}, {"value", "#ffffff"}}) == "InvalidParams");
    CHECK(h.errorOf("template.setFieldKey", Json{{"clip", instance}, {"field", "box"}, {"time", tc(1)}, {"value", "red"}}) == "InvalidParams");

    // Remove a key; stop animating keeps the value at "time".
    h.ok("template.removeFieldKey", Json{{"clip", instance}, {"field", "box"}, {"index", 1}});
    CHECK(graphicIn(h, instance).instance->fields[1].keys.size() == 1);
    CHECK(h.errorOf("template.removeFieldKey", Json{{"clip", instance}, {"field", "box"}, {"index", 5}}) == "NotFound");
    h.ok("template.setFieldKey", Json{{"clip", instance}, {"field", "box"}, {"time", tc(5)}, {"value", "#0000ff"}});
    h.ok("template.clearFieldKeys", Json{{"clip", instance}, {"field", "box"}, {"time", tc(0)}});
    const Graphic& cleared = graphicIn(h, instance);
    CHECK(cleared.instance->fields[1].keys.empty());
    CHECK(cleared.findLayer(1)->shape.fill.color.rgba[0] == 1.0); // red, as it was at time 0
    h.session.undo();
    CHECK(graphicIn(h, instance).instance->fields[1].keys.size() == 2);
}

TEST_CASE("text styles style part of a text and are reordered through commands (card 81)", "[command][graphics][textStyles]") {
    CommandHarness h(kPicture);
    const std::uint64_t clip = idOf(h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(10)}, {"track", "V2"}, {"graphic", lowerThird()}}), "clip");
    // "Name" is layer 2 (4 characters).
    h.ok("textStyle.update", Json{{"name", "Big"}, {"style", Json{{"text", Json{{"runs", Json::array({Json{{"size", 120}, {"weight", 800}}})}}}}}});
    h.ok("textStyle.update", Json{{"name", "Small"}, {"style", Json{{"text", Json{{"runs", Json::array({Json{{"size", 20}}})}}}}}});
    const std::size_t undos = h.session.undoCount();
    h.ok("textStyle.applyRange", Json{{"clip", clip}, {"layer", 2}, {"from", 1}, {"to", 3}, {"name", "big"}});
    CHECK(h.session.undoCount() == undos + 1);
    CHECK(h.session.undoLabel() == "Apply Text Style to Part of a Text");
    const GraphicLayer* layer = graphicIn(h, clip).findLayer(2);
    REQUIRE(layer->text.runs.size() == 3);
    CHECK(layer->text.runs[0].text == "N");
    CHECK(layer->text.runs[1].text == "am");
    CHECK(layer->text.runs[1].style.fontSize == 120.0);
    CHECK(layer->text.runs[2].text == "e");
    CHECK(layer->textStyle.empty());
    CHECK(h.errorOf("textStyle.applyRange", Json{{"clip", clip}, {"layer", 2}, {"from", 1}, {"to", 3}, {"name", "none"}}) == "NotFound");
    CHECK(h.errorOf("textStyle.applyRange", Json{{"clip", clip}, {"layer", 2}, {"from", 3}, {"to", 3}, {"name", "Big"}}) == "EditRejected");
    CHECK(h.errorOf("textStyle.applyRange", Json{{"clip", clip}, {"layer", 2}, {"from", 0}, {"to", 99}, {"name", "Big"}}) == "EditRejected");
    CHECK(h.errorOf("textStyle.applyRange", Json{{"clip", clip}, {"layer", 1}, {"from", 0}, {"to", 1}, {"name", "Big"}}) == "EditRejected"); // a shape
    CHECK(h.errorOf("textStyle.applyRange", Json{{"clip", clip}, {"layer", 2}, {"from", 0}, {"to", 1}, {"name", "Big"}, {"time", tc(40)}}) == "InvalidParams");
    h.session.undo();
    CHECK(graphicIn(h, clip).findLayer(2)->text.runs.size() == 1);

    // The order: Big, Small -> Small, Big.
    const auto order = [&] {
        std::string out;
        for (const ProjectTextStyle& s : h.state()->textStyles)
            out += s.name + ",";
        return out;
    };
    CHECK(order() == "Big,Small,");
    h.ok("textStyle.move", Json{{"name", "Small"}, {"index", 0}});
    CHECK(order() == "Small,Big,");
    CHECK(h.session.undoLabel() == "Reorder Text Styles");
    CHECK(h.errorOf("textStyle.move", Json{{"name", "Missing"}, {"index", 0}}) == "NotFound");
    h.session.undo();
    CHECK(order() == "Big,Small,");
}

TEST_CASE("templates are filed in folders and a folder of template files installs in one step (card 87)", "[command][graphics][templates][folders]") {
    const fs::path library = scratch("folder-library");
    const fs::path source = scratch("folder-source");
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::GraphicsServices>(library);
    CommandHarness h(kPicture, policy);
    const std::uint64_t clip = idOf(h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(10)}, {"track", "V2"}, {"graphic", lowerThird()}}), "clip");
    const Json fields = Json::array({Json{{"id", "name"}, {"kind", "text"}, {"layer", 2}, {"property", "text"}}});
    h.ok("template.save", Json{{"clip", clip}, {"id", "one"}, {"name", "One"}, {"fields", fields}});
    h.ok("template.save", Json{{"clip", clip}, {"id", "two"}, {"name", "Two"}, {"fields", fields}, {"library", "user"}});

    // Filing: a project template (one undo step, the version stays) and a user one (its file is rewritten).
    const std::size_t undos = h.session.undoCount();
    h.ok("template.setFolder", Json{{"template", "one"}, {"folder", "Titles/Lower thirds"}});
    CHECK(h.session.undoCount() == undos + 1);
    CHECK(h.state()->findTemplate("one")->folder == "Titles/Lower thirds");
    CHECK(h.state()->findTemplate("one")->version == 1);
    CHECK(h.ok("template.list", Json{{"library", "project"}}).data["project"][0]["folder"] == "Titles/Lower thirds");
    h.ok("template.setFolder", Json{{"template", "two"}, {"library", "user"}, {"folder", "Mine"}});
    CHECK(h.ok("template.list", Json{{"library", "user"}}).data["user"][0]["folder"] == "Mine");
    h.ok("template.setFolder", Json{{"template", "two"}, {"library", "user"}, {"folder", ""}});
    CHECK_FALSE(h.ok("template.list", Json{{"library", "user"}}).data["user"][0].contains("folder"));
    for (const char* bad : {"/start", "end/", "a//b", "a/b/c/d", "back\\slash", "a/../b", "."})
        CHECK(h.errorOf("template.setFolder", Json{{"template", "one"}, {"folder", bad}}) == "InvalidParams");
    CHECK(h.errorOf("template.setFolder", Json{{"template", "nope"}, {"folder", "x"}}) == "NotFound");
    CHECK(h.errorOf("template.setFolder", Json{{"template", "one"}, {"library", "user"}, {"folder", "x"}}) == "NotFound");
    // The folder is saved with the template (project file and library file) and read back.
    CHECK(io::templateFileToJson(*h.state()->findTemplate("one")).find("\"folder\"") != std::string::npos);
    const auto reread = io::templateFileFromJson(io::templateFileToJson(*h.state()->findTemplate("one")));
    REQUIRE(reread);
    CHECK(reread->folder == "Titles/Lower thirds");

    // A folder of template files: the files of a tree install, each filed under the folder it was found in; a file that
    // does not read is skipped with the reason and the others still go in.
    const GraphicTemplate one = *h.state()->findTemplate("one");
    const auto write = [&](const fs::path& where, const std::string& id, const std::string& name) {
        GraphicTemplate t = one;
        t.id = id;
        t.name = name;
        t.folder.clear();
        fs::create_directories(where.parent_path());
        std::ofstream out(where, std::ios::binary);
        out << io::templateFileToJson(t);
    };
    write(source / "Pack" / "top.wabcut-template.json", "top", "Top");
    write(source / "Pack" / "Dark" / "dark.wabcut-template.json", "dark", "Dark");
    write(source / "Pack" / "Dark" / "Deeper" / "Deepest" / "deep.wabcut-template.json", "deep", "Deep");
    {
        std::ofstream broken(source / "Pack" / "broken.wabcut-template.json");
        broken << "{";
        std::ofstream notes(source / "Pack" / "notes.txt");
        notes << "not a template";
    }
    const std::string root = (source / "Pack").generic_string();
    const ProjectSnapshot before = h.state();
    const std::size_t steps = h.session.undoCount();
    const auto dry = h.run(Json{{"command", "template.importFolder"}, {"params", Json{{"path", root}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*h.state() == *before);
    const auto imported = h.ok("template.importFolder", Json{{"path", root}});
    CHECK(imported.data["imported"] == 3);
    CHECK(imported.data["skipped"].size() == 1);
    CHECK(imported.data["skipped"][0]["file"].get<std::string>().find("broken") != std::string::npos);
    CHECK_FALSE(imported.warnings.empty());
    CHECK(h.session.undoCount() == steps + 1); // one undo step for the library
    CHECK(h.state()->findTemplate("top")->folder == "Pack");
    CHECK(h.state()->findTemplate("dark")->folder == "Pack/Dark");
    CHECK(h.state()->findTemplate("deep")->folder == "Pack/Dark/Deeper Deepest"); // deeper levels share the third part
    h.session.undo();
    CHECK(h.state()->findTemplate("top") == nullptr);
    // Without subfolders only the top level; into the user's library the files are written.
    CHECK(h.ok("template.importFolder", Json{{"path", root}, {"recursive", false}}).data["imported"] == 1);
    CHECK(h.ok("template.importFolder", Json{{"path", root}, {"library", "user"}}).data["imported"] == 3);
    CHECK(fs::exists(library / "dark.wabcut-template.json"));
    CHECK(h.ok("template.list", Json{{"library", "user"}, {"id", "dark"}}).data["user"][0]["folder"] == "Pack/Dark");
    // Refusals: not a folder, no user library.
    CHECK(h.errorOf("template.importFolder", Json{{"path", (source / "missing").generic_string()}}) == "NotFound");
    CommandHarness plain(kPicture);
    CHECK(plain.errorOf("template.importFolder", Json{{"path", root}, {"library", "user"}}) == "InvalidRequest");
}
