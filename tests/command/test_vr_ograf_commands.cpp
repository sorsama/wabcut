// Card 38 and card 178 (0.7) through the command engine: the VR format of a sequence and of footage (sequence.setSettings,
// media.interpretFootage, vr.info), vr.reorient, and importing Lottie templates (fields, dotLottie) and OGraf packages
// (ograf.inspect, ograf.import) - dry runs, one undo step each, and the refusals.

#include "CommandHarness.h"

#include <wabcut/interchange/Zip.h>
#include <wabcut/model/Vr.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPicture = R"(
    V2 tgt |..........|
    V1 tgt |Aaaaaaaaaa|
    A1     |Aaaaaaaaaa|
)";

const Json k360{{"projection", "equirect360"}, {"stereo", "topBottom"}};

fs::path scratch(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "vr-ograf-commands" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

std::string lottie() {
    return R"({"v": "5.7.0", "fr": 30, "ip": 0, "op": 60, "w": 1000, "h": 500, "nm": "Lower Third", "layers": [
 {"ty": 5, "nm": "Title", "ind": 1, "ip": 0, "op": 60, "st": 0, "ks": {}, "t": {"d": {"k": [{"s": {"t": "Hello", "s": 60, "f": "Arial", "fc": [1, 0, 0], "j": 0}, "t": 0}]}, "a": [], "p": {}}},
 {"ty": 4, "nm": "Bar", "ind": 2, "ip": 0, "op": 60, "st": 0, "ks": {}, "shapes": [{"ty": "gr", "nm": "G", "it": [
   {"ty": "rc", "nm": "R", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [200, 50]}, "r": {"a": 0, "k": 0}},
   {"ty": "fl", "c": {"a": 0, "k": [0, 0.5, 1, 1]}, "o": {"a": 0, "k": 100}},
   {"ty": "tr", "p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100}}]}]}]})";
}

std::string manifest(const std::string& extra = {}) {
    return R"({"$schema": "https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json", "id": "com.example.lt", "name": "LT",
 "main": "g.mjs", "supportsRealTime": true, "supportsNonRealTime": false,
 "schema": {"type": "object", "properties": {"title": {"type": "string", "title": "Headline", "default": "Hello"}}}})" +
           extra;
}

std::string manifestWith(const std::string& extra) {
    std::string text = manifest();
    text.pop_back(); // the closing brace
    return text + extra + "}";
}

} // namespace

TEST_CASE("VR: sequence.setSettings and vr.info set and read a sequence's format in one undo step (card 38)", "[command][vr]") {
    CommandHarness h(kPicture);
    const ProjectSnapshot before = h.state();
    CHECK_FALSE(h.ok("vr.info").data["sequence"]["isVr"].get<bool>());

    const auto dry = h.run(Json{{"command", "sequence.setSettings"}, {"params", Json{{"vr", k360}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*h.state() == *before);

    const auto set = h.ok("sequence.setSettings", Json{{"vr", k360}});
    CHECK(h.session.undoCount() == 1);
    CHECK(set.data["vr"]["projection"] == "equirect360");
    CHECK(set.data["vr"]["stereo"] == "topBottom");
    CHECK(h.sequence(h.state()).vr == VrFormat{VrProjection::Equirectangular360, StereoLayout::TopBottom});
    const auto info = h.ok("vr.info");
    CHECK(info.data["sequence"]["isVr"].get<bool>());
    CHECK(info.data["sequence"]["stereo"] == "topBottom");

    // null (or projection none) makes it flat again.
    h.ok("sequence.setSettings", Json{{"vr", nullptr}});
    CHECK_FALSE(h.sequence(h.state()).vr.isVr());
    h.ok("sequence.setSettings", Json{{"vr", Json{{"projection", "equirect180"}}}});
    CHECK(h.sequence(h.state()).vr == VrFormat{VrProjection::Equirectangular180, StereoLayout::Mono});

    // Refusals: an unknown projection or layout, a layout on a flat picture, a wrong type.
    CHECK(h.errorOf("sequence.setSettings", Json{{"vr", Json{{"projection", "cubemap"}}}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.setSettings", Json{{"vr", Json{{"projection", "equirect360"}, {"stereo", "anaglyph"}}}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.setSettings", Json{{"vr", Json{{"projection", "none"}, {"stereo", "topBottom"}}}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.setSettings", Json{{"vr", 7}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.setSettings", Json{{"vr", Json::object()}}) == "InvalidParams");
}

TEST_CASE("VR: media.interpretFootage says what a file's picture is and over its metadata (card 38)", "[command][vr]") {
    CommandHarness h(kPicture);
    const MediaId media = h.tl.media('A');
    const Json id = Json::array({media.value});
    CHECK_FALSE(h.ok("vr.info", Json{{"media", media.value}}).data["media"]["effective"]["isVr"].get<bool>());

    h.ok("media.interpretFootage", Json{{"media", id}, {"vr", Json{{"projection", "equirect360"}, {"stereo", "sideBySide"}}}});
    CHECK(h.session.undoCount() == 1);
    const auto info = h.ok("vr.info", Json{{"media", media.value}});
    CHECK(info.data["media"]["effective"]["projection"] == "equirect360");
    CHECK(info.data["media"]["effective"]["stereo"] == "sideBySide");
    CHECK(info.data["media"]["interpretation"]["projection"] == "equirect360");
    CHECK_FALSE(info.data["media"]["file"]["isVr"].get<bool>());
    CHECK(mediaVr(*h.state()->findMedia(media)) == VrFormat{VrProjection::Equirectangular360, StereoLayout::SideBySide});

    // null goes back to the file's; the other interpretation fields are untouched by a VR change.
    h.ok("media.interpretFootage", Json{{"media", id}, {"vr", nullptr}});
    CHECK_FALSE(mediaVr(*h.state()->findMedia(media)).isVr());
    CHECK_FALSE(h.state()->findMedia(media)->interpretation.has_value());
    CHECK(h.errorOf("media.interpretFootage", Json{{"media", id}, {"vr", Json{{"projection", "mercator"}}}}) == "InvalidParams");
}

TEST_CASE("VR: vr.reorient adds VR Reorient with the clip's format (card 38)", "[command][vr]") {
    CommandHarness h(kPicture);
    const ClipId clip = h.clip("V1@0");
    // A flat clip in a flat sequence needs to be told what it is.
    CHECK(h.errorOf("vr.reorient", Json{{"clips", Json::array({clip.value})}, {"yaw", 30}}) == "InvalidParams");
    h.ok("sequence.setSettings", Json{{"vr", k360}});
    const std::size_t before = h.session.undoCount();
    const auto added = h.ok("vr.reorient", Json{{"clips", Json::array({clip.value})}, {"yaw", 30}, {"pitch", -10}});
    CHECK(h.session.undoCount() == before + 1);
    const Clip& c = h.sequence(h.state()).findClip(clip)->clip();
    REQUIRE(c.content().effects.size() == 1);
    const EffectInstance& e = c.content().effects.front();
    CHECK(e.effect == "wabcut.vr.reorient");
    CHECK(effectParameterAt(e, "yaw", Ticks{}) == 30.0);
    CHECK(effectParameterAt(e, "pitch", Ticks{}) == -10.0);
    CHECK(effectParameterAt(e, "projection", Ticks{}) == 0.0); // 360
    CHECK(effectParameterAt(e, "stereo", Ticks{}) == 1.0);     // top - bottom, from the sequence
    // An explicit projection wins; hostile values are refused.
    h.ok("vr.reorient", Json{{"clips", Json::array({clip.value})}, {"projection", "equirect180"}, {"stereo", "mono"}});
    CHECK(h.errorOf("vr.reorient", Json{{"clips", Json::array({clip.value})}, {"pitch", 120}}) == "InvalidParams");
    CHECK(h.errorOf("vr.reorient", Json{{"clips", Json::array({clip.value})}, {"projection", "none"}}) == "InvalidParams");
    CHECK(h.errorOf("vr.reorient", Json{{"clips", Json::array()}}) == "InvalidParams");
    (void)added;
}

TEST_CASE("Lottie: lottie.import makes the template fields and places an instance of the template (card 178)", "[command][graphics][lottie]") {
    CommandHarness h(kPicture);
    const auto placed = h.ok("lottie.import", Json{{"data", lottie()}, {"at", tc(1)}, {"track", "V2"}});
    CHECK(h.session.undoCount() == 1);
    CHECK(placed.data["source"] == "lottie");
    // The title's text, its color, and the bar's color.
    REQUIRE(placed.data["fields"].size() == 3);
    CHECK(placed.data["fields"][0]["kind"] == "text");
    CHECK(placed.data["fields"][0]["label"] == "Title");
    const ClipId clip{placed.data["clip"].get<std::uint64_t>()};
    const Graphic* g = h.sequence(h.state()).findClip(clip)->clip().graphic();
    REQUIRE(g);
    REQUIRE(g->instance.has_value());
    CHECK(g->instance->fields.size() == 3);
    // The fields edit the layers and take keys like any template's.
    const std::string textField = placed.data["fields"][0]["id"];
    h.ok("template.setField", Json{{"clip", clip.value}, {"values", Json{{textField, "Breaking news"}}}});
    const Graphic* edited = h.sequence(h.state()).findClip(clip)->clip().graphic();
    CHECK(edited->findLayer(edited->instance->fields[0].layer)->text.runs.front().text == "Breaking news");
    h.ok("template.setFieldKey", Json{{"clip", clip.value}, {"field", textField}, {"time", tc(10)}, {"value", "Later"}});
    CHECK_FALSE(h.sequence(h.state()).findClip(clip)->clip().graphic()->instance->fields[0].keys.empty());

    // Without fields a plain graphic clip is placed (an animation of shapes only).
    const std::string shapesOnly = R"({"fr": 25, "ip": 0, "op": 25, "w": 100, "h": 100, "layers": [
        {"ty": 4, "ip": 0, "op": 25, "ks": {}, "shapes": [{"ty": "el", "s": {"a": 0, "k": [10, 10]}}, {"ty": "fl", "c": {"a": 1, "k": [{"t": 0, "s": [1, 0, 0, 1], "e": [0, 1, 0, 1]}, {"t": 20, "s": [0, 1, 0, 1]}]}}]}]})";
    const auto plain = h.ok("lottie.import", Json{{"data", shapesOnly}, {"at", tc(5)}, {"track", "V2"}});
    CHECK(plain.data["fields"].empty());
    CHECK_FALSE(h.sequence(h.state()).findClip(ClipId{plain.data["clip"].get<std::uint64_t>()})->clip().graphic()->instance.has_value());
}

TEST_CASE("Lottie: a dotLottie archive imports and choosing its animation by id (card 178)", "[command][graphics][lottie]") {
    CommandHarness h(kPicture);
    const fs::path dir = scratch("dotlottie");
    const std::string second = R"({"fr": 30, "ip": 0, "op": 30, "w": 100, "h": 100, "layers": [{"ty": 5, "nm": "Other", "ip": 0, "op": 30, "ks": {}, "t": {"d": {"k": [{"s": {"t": "Two", "s": 20, "f": "Arial", "fc": [0, 0, 0], "j": 0}, "t": 0}]}, "a": [], "p": {}}}]})";
    const std::string manifestText = R"({"animations": [{"id": "one"}, {"id": "two"}]})";
    const std::string archive = interchange::writeStoredZip(std::vector<std::pair<std::string, std::string>>{
        {"manifest.json", manifestText}, {"animations/one.json", lottie()}, {"animations/two.json", second}});
    write(dir / "pack.lottie", archive);

    const auto first = h.ok("lottie.import", Json{{"path", pathToUtf8(dir / "pack.lottie")}, {"at", tc(0)}, {"track", "V2"}});
    CHECK(first.data["fields"][0]["label"] == "Title");
    bool mentionsOthers = false;
    for (const Json& e : first.data["report"]["entries"]) {
        mentionsOthers = mentionsOthers || e["reason"].get<std::string>().find("2 animations") != std::string::npos;
    }
    CHECK(mentionsOthers);
    const auto other = h.ok("lottie.import", Json{{"path", pathToUtf8(dir / "pack.lottie")}, {"animation", "two"}, {"at", tc(3)}, {"track", "V2"}});
    CHECK(other.data["fields"][0]["label"] == "Other");
    CHECK(h.errorOf("lottie.import", Json{{"path", pathToUtf8(dir / "pack.lottie")}, {"animation", "three"}, {"at", tc(3)}}) == "InvalidParams");
    // A damaged archive says what it is.
    write(dir / "bad.lottie", std::string("PK\x03\x04", 4) + std::string(100, 'x'));
    CHECK(h.errorOf("lottie.import", Json{{"path", pathToUtf8(dir / "bad.lottie")}, {"at", tc(0)}}) == "InvalidParams");
}

TEST_CASE("OGraf: ograf.inspect reads a package and ograf.import draws one that ships a Lottie version (card 178)", "[command][graphics][ograf]") {
    CommandHarness h(kPicture);
    const fs::path dir = scratch("ograf");
    write(dir / "lt.ograf.json", manifest());
    write(dir / "g.mjs", "export default class {}");

    // Without a Lottie version: inspected, but not drawn - and the import says why.
    const auto inspected = h.ok("ograf.inspect", Json{{"path", pathToUtf8(dir / "lt.ograf.json")}});
    CHECK(inspected.data["name"] == "LT");
    CHECK(inspected.data["main"] == "g.mjs");
    CHECK_FALSE(inspected.data["renderable"].get<bool>());
    CHECK(inspected.data["why"].get<std::string>().find("web engine") != std::string::npos);
    REQUIRE(inspected.data["fields"].size() == 1);
    CHECK(inspected.data["fields"][0]["key"] == "title");
    CHECK(inspected.data["fields"][0]["kind"] == "text");
    CHECK(inspected.data["fields"][0]["default"] == "Hello");
    const ProjectSnapshot before = h.state();
    CHECK(h.errorOf("ograf.import", Json{{"path", pathToUtf8(dir / "lt.ograf.json")}, {"at", tc(0)}, {"track", "V2"}}) == "InvalidParams");
    CHECK(*h.state() == *before);

    // With one, by the manifest's name for it.
    write(dir / "motion.json", lottie());
    write(dir / "lt.ograf.json", manifestWith(R"(, "v_wabcut_lottie": "motion.json")"));
    const auto drawn = h.ok("ograf.inspect", Json{{"path", pathToUtf8(dir)}});
    CHECK(drawn.data["renderable"].get<bool>());
    CHECK(drawn.data["lottie"] == "motion.json");
    const auto imported = h.ok("ograf.import", Json{{"path", pathToUtf8(dir / "lt.ograf.json")}, {"at", tc(2)}, {"track", "V2"},
                                                    {"saveAs", Json{{"id", "ograf-lt"}, {"name", "Lower third"}}}});
    CHECK(h.session.undoCount() == 1);
    CHECK(imported.data["source"] == "ograf");
    CHECK(imported.data["template"] == "ograf-lt");
    const GraphicTemplate* tmpl = h.state()->findTemplate("ograf-lt");
    REQUIRE(tmpl != nullptr);
    CHECK(tmpl->name == "Lower third");
    bool headline = false;
    for (const TemplateField& f : tmpl->fields) {
        headline = headline || (f.id == "title" && f.label == "Headline");
    }
    CHECK(headline); // the manifest's field names the Lottie's "Title" text
    const ClipId clip{imported.data["clip"].get<std::uint64_t>()};
    REQUIRE(h.sequence(h.state()).findClip(clip)->clip().graphic()->instance.has_value());

    // A zip package works as well; the paths of a package never leave it.
    const std::string zipped = interchange::writeStoredZip(std::vector<std::pair<std::string, std::string>>{
        {"lt.ograf.json", manifestWith(R"(, "v_wabcut_lottie": "motion.json")")}, {"g.mjs", ""}, {"motion.json", lottie()}});
    write(dir / "pack.zip", zipped);
    CHECK(h.ok("ograf.inspect", Json{{"path", pathToUtf8(dir / "pack.zip")}}).data["renderable"].get<bool>());
    write(dir / "evil.ograf.json", manifestWith(R"(, "v_wabcut_lottie": "../../secret.json")"));
    CHECK(h.errorOf("ograf.inspect", Json{{"path", pathToUtf8(dir / "evil.ograf.json")}}) == "InvalidParams");
    CHECK(h.errorOf("ograf.import", Json{{"path", pathToUtf8(dir / "nowhere.ograf.json")}, {"at", tc(0)}}) != "");
    CHECK(h.errorOf("ograf.import", Json{{"path", pathToUtf8(dir / "pack.zip")}}) == "InvalidParams"); // neither placed nor saved
}
