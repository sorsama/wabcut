// effects.* (0.2): effect presets, presets bins, custom bins and favourites through the command protocol, the host's
// library file, and preset files.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EffectPresets.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaa....Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaa....Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
)";

fs::path scratch(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "effects-library" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/// A harness whose effects library is `services` (a file, or in memory).
struct LibraryHarness {
    explicit LibraryHarness(std::shared_ptr<command::EffectLibraryServices> s = std::make_shared<command::EffectLibraryServices>())
        : services(std::move(s)), h(kPicture, policyWith(services)) {}

    static command::EnginePolicy policyWith(std::shared_ptr<command::EffectLibraryServices> s) {
        command::EnginePolicy policy;
        policy.services = std::move(s);
        return policy;
    }

    /// Gaussian Blur keyed over the first clip (frames 2 and 10).
    void blurFirstClip() {
        const ClipId clip = h.clip("V1@0");
        h.ok("clip.addEffect", Json{{"clips", Json::array({clip.value})}, {"effect", "wabcut.blur.gaussian"}});
        const auto& e = h.sequence(h.state()).findClip(clip)->clip().effects().back();
        h.ok("clip.addKeyframe", Json{{"clip", clip.value}, {"effect", e.id.value}, {"param", "radius"}, {"time", tc(2)}, {"value", 30}});
        h.ok("clip.addKeyframe", Json{{"clip", clip.value}, {"effect", e.id.value}, {"param", "radius"}, {"time", tc(10)}, {"value", 0}});
    }

    std::shared_ptr<command::EffectLibraryServices> services;
    CommandHarness h;
};

} // namespace

TEST_CASE("Save Preset goes to the user's library and applies to other clips", "[command][effects][presets]") {
    LibraryHarness lh;
    lh.blurFirstClip();
    CommandHarness& h = lh.h;
    const ClipId first = h.clip("V1@0");
    const ClipId second = h.clip("V1@24");

    // A dry run saves nothing; the real request does, and is no undo step.
    const Json save{{"clip", first.value}, {"effects", Json::array({0})}, {"name", "Soft start"}, {"type", "anchorToInPoint"},
                    {"description", "Blur that clears"}, {"bin", "Mine"}};
    REQUIRE(h.run(Json{{"command", "effects.savePreset"}, {"params", save}, {"dryRun", true}}).ok());
    CHECK(lh.services->library().presets.empty());
    const auto revision = h.session.revision();
    h.ok("effects.savePreset", save);
    CHECK(h.session.revision() == revision);
    REQUIRE(lh.services->library().presets.size() == 1);
    CHECK(lh.services->library().presets[0].bin == "Mine");
    // Taken names: the user's (unless replaced) and the built-ins'.
    CHECK(h.errorOf("effects.savePreset", save) == "EditRejected");
    Json builtinName = save;
    builtinName["name"] = "twirl in";
    CHECK(h.errorOf("effects.savePreset", builtinName) == "EditRejected");
    Json replace = save;
    replace["replace"] = true;
    replace["type"] = "scale";
    h.ok("effects.savePreset", replace);
    CHECK(lh.services->library().presets[0].type == EffectPresetType::Scale);

    const auto list = h.ok("effects.library").data;
    bool mine = false;
    bool twirl = false;
    for (const Json& p : list["presets"]) {
        mine = mine || (p["name"] == "Soft start" && p["builtIn"] == false && p["bin"] == "Mine");
        twirl = twirl || (p["name"] == "Twirl In" && p["builtIn"] == true && p["group"] == "Presets");
    }
    CHECK(mine);
    CHECK(twirl);

    // Applying is one undo step; Scale stretches the keys over the 40-frame clip (4 and 20).
    const auto applied = h.ok("effects.applyPreset", Json{{"preset", "soft START"}, {"clips", Json::array({second.value})}});
    CHECK(applied.data["clips"][0]["applied"] == Json::array({"Gaussian Blur"}));
    const Clip& c = h.sequence(h.state()).findClip(second)->clip();
    REQUIRE(c.effects().size() == 1);
    const Curve* curve = c.effects()[0].find("radius")->curve();
    REQUIRE(curve);
    CHECK(curve->keys[0].time == c.sourceIn + f(4));
    CHECK(curve->keys[1].time == c.sourceIn + f(20));
    CHECK(h.session.canUndo());
    h.ok("effects.applyPreset", Json{{"preset", "Mosaic In"}, {"clips", Json::array({second.value})}});
    CHECK(h.errorOf("effects.applyPreset", Json{{"preset", "No such"}, {"clips", Json::array({second.value})}}) == "NotFound");
    // A video preset on an audio clip changes nothing.
    CHECK(h.errorOf("effects.applyPreset", Json{{"preset", "Twirl In"}, {"clips", Json::array({h.clip("A1@0").value})}}) ==
          "EditRejected");
}

TEST_CASE("Presets bins, custom bins and favourites", "[command][effects][presets]") {
    LibraryHarness lh;
    lh.blurFirstClip();
    CommandHarness& h = lh.h;
    const ClipId first = h.clip("V1@0");
    h.ok("effects.newPresetsBin", Json{{"bin", "Looks/Warm"}});
    CHECK(lh.services->library().presetBins == std::vector<std::string>{"Looks", "Looks/Warm"});
    h.ok("effects.savePreset", Json{{"clip", first.value}, {"effects", Json::array({0})}, {"name", "Glow"}, {"bin", "Looks/Warm"}});
    CHECK(h.errorOf("effects.savePreset", Json{{"clip", first.value}, {"effects", Json::array({0})}, {"name", "x"},
                                               {"bin", "Lumetri Presets"}}) == "InvalidParams");

    // Premiere names new custom bins "Custom Bin 01", "Custom Bin 02".
    CHECK(h.ok("effects.newCustomBin").data["customBin"] == "Custom Bin 01");
    CHECK(h.ok("effects.newCustomBin").data["customBin"] == "Custom Bin 02");
    h.ok("effects.renameCustomItem", Json{{"customBin", "Custom Bin 01"}, {"name", "Gaming"}});
    h.ok("effects.addToCustomBin", Json{{"customBin", "gaming"}, {"items", Json::array({"wabcut.blur.gaussian", "Glow", "crossDissolve", "Glow"})}});
    CHECK(lh.services->library().customBins[0].items ==
          std::vector<std::string>{"effect:wabcut.blur.gaussian", "preset:Glow", "effect:crossDissolve"});
    CHECK(h.errorOf("effects.addToCustomBin", Json{{"customBin", "Gaming"}, {"items", Json::array({"wabcut.nothing"})}}) == "NotFound");
    h.ok("effects.setFavorite", Json{{"item", "preset:Glow"}});
    h.ok("effects.setFavorite", Json{{"item", "Twirl In"}});

    // Renaming a preset follows it into custom bins and favourites; renaming a presets bin moves its presets.
    h.ok("effects.renameCustomItem", Json{{"preset", "Glow"}, {"name", "Warm glow"}});
    CHECK(lh.services->library().customBins[0].items[1] == "preset:Warm glow");
    CHECK(lh.services->library().favorites[0] == "preset:Warm glow");
    h.ok("effects.renameCustomItem", Json{{"presetsBin", "Looks"}, {"name", "Grades"}});
    CHECK(lh.services->library().presets[0].bin == "Grades/Warm");
    CHECK(lh.services->library().presetBins == std::vector<std::string>{"Grades", "Grades/Warm"});
    h.ok("effects.setPresetProperties", Json{{"preset", "Warm glow"}, {"type", "anchorToOutPoint"}, {"description", "d"}, {"bin", ""}});
    CHECK(lh.services->library().presets[0].type == EffectPresetType::AnchorToOutPoint);
    CHECK(lh.services->library().presets[0].bin.empty());
    CHECK(h.errorOf("effects.setPresetProperties", Json{{"preset", "Twirl In"}, {"type", "scale"}}) == "EditRejected");

    const auto list = h.ok("effects.library").data;
    CHECK(list["customBins"][0]["items"][1]["kind"] == "video");
    CHECK(list["favorites"].size() == 2);

    // Delete Custom Item: an item of a custom bin, a custom bin, a preset (it leaves the bins and favourites), a bin.
    h.ok("effects.deleteCustomItem", Json{{"customBin", "Gaming"}, {"item", "crossDissolve"}});
    CHECK(lh.services->library().customBins[0].items.size() == 2);
    h.ok("effects.deleteCustomItem", Json{{"customBin", "Custom Bin 02"}});
    CHECK(lh.services->library().customBins.size() == 1);
    h.ok("effects.deleteCustomItem", Json{{"preset", "Warm glow"}});
    CHECK(lh.services->library().presets.empty());
    CHECK(lh.services->library().customBins[0].items == std::vector<std::string>{"effect:wabcut.blur.gaussian"});
    CHECK(lh.services->library().favorites == std::vector<std::string>{"preset:Twirl In"});
    h.ok("effects.deleteCustomItem", Json{{"presetsBin", "Grades"}});
    CHECK(lh.services->library().presetBins.empty());
    CHECK(h.errorOf("effects.deleteCustomItem", Json{{"preset", "Twirl In"}}) == "EditRejected");
    h.ok("effects.setFavorite", Json{{"item", "Twirl In"}, {"favorite", false}});
    CHECK(lh.services->library().favorites.empty());
}

TEST_CASE("A batch sees its own library changes and a failed batch leaves the library", "[command][effects][presets]") {
    LibraryHarness lh;
    lh.blurFirstClip();
    CommandHarness& h = lh.h;
    const ClipId first = h.clip("V1@0");
    const ClipId second = h.clip("V1@24");
    const Json save{{"command", "effects.savePreset"},
                    {"params", Json{{"clip", first.value}, {"effects", Json::array({0})}, {"name", "Batch preset"}}}};
    const Json apply{{"command", "effects.applyPreset"}, {"params", Json{{"preset", "Batch preset"}, {"clips", Json::array({second.value})}}}};
    const auto ok = h.run(Json{{"commands", Json::array({save, apply})}});
    INFO(command::toJson(ok).dump());
    REQUIRE(ok.ok());
    CHECK(lh.services->library().findPreset("Batch preset"));

    LibraryHarness other;
    other.blurFirstClip();
    const Json bad{{"command", "effects.applyPreset"}, {"params", Json{{"preset", "Nothing"}, {"clips", Json::array({second.value})}}}};
    CHECK_FALSE(other.h.run(Json{{"commands", Json::array({save, bad})}}).ok());
    CHECK(other.services->library().presets.empty());
}

TEST_CASE("The library file survives the session and a bad file is left alone", "[command][effects][presets][io]") {
    const fs::path dir = scratch("file");
    const fs::path file = dir / "effects-library.json";
    {
        LibraryHarness lh(std::make_shared<command::EffectLibraryServices>(file));
        lh.blurFirstClip();
        lh.h.ok("effects.savePreset", Json{{"clip", lh.h.clip("V1@0").value}, {"effects", Json::array({0})}, {"name", "Kept"}});
        lh.h.ok("effects.newCustomBin", Json{{"name", "Mine"}, {"items", Json::array({"preset:Kept"})}});
    }
    REQUIRE(fs::exists(file));
    command::EffectLibraryServices again(file);
    CHECK(again.library().findPreset("Kept"));
    CHECK(again.library().findCustomBin("Mine"));
    CHECK(again.error().empty());

    const fs::path broken = dir / "broken.json";
    {
        std::ofstream out(broken, std::ios::binary);
        out << R"({"format": "wabcut-effects-library", "version": 1, "presets": [{"name": 7}]})";
    }
    command::EffectLibraryServices bad(broken);
    CHECK(bad.library().presets.empty());
    CHECK_FALSE(bad.error().empty());
    CHECK(fs::file_size(broken) > 0); // not overwritten by reading
}

TEST_CASE("Export Preset and Import Presets round trip through a file", "[command][effects][presets][io]") {
    const fs::path dir = scratch("export");
    LibraryHarness lh;
    lh.blurFirstClip();
    CommandHarness& h = lh.h;
    h.ok("effects.savePreset", Json{{"clip", h.clip("V1@0").value}, {"effects", Json::array({0})}, {"name", "Shared"}, {"bin", "Team"}});
    const fs::path file = dir / ("team" + std::string(io::kEffectPresetsFileSuffix));
    const Json exportParams{{"path", pathToUtf8(file)}, {"presets", Json::array({"Twirl In"})}, {"presetsBin", "Team"}};
    h.ok("effects.exportPresets", exportParams);
    REQUIRE(fs::exists(file));
    CHECK(h.errorOf("effects.exportPresets", exportParams) == "EditRejected"); // exists
    Json overwrite = exportParams;
    overwrite["overwrite"] = true;
    h.ok("effects.exportPresets", overwrite);

    // Imported names in use get a number: "Shared 2", "Twirl In 2".
    const auto imported = h.ok("effects.importPresets", Json{{"path", pathToUtf8(file)}, {"bin", "Imported"}}).data["imported"];
    REQUIRE(imported.size() == 2);
    CHECK(lh.services->library().findPreset("Shared 2"));
    CHECK(lh.services->library().findPreset("Twirl In 2"));
    CHECK(lh.services->library().findPreset("Shared 2")->bin == "Imported");

    const fs::path junk = dir / "junk.wabcut-presets.json";
    {
        std::ofstream out(junk, std::ios::binary);
        out << R"({"format": "wabcut-effect-presets", "version": 1, "presets": [{"name": "x", "duration": 5,
                  "effects": [{"id": 1, "effect": "wabcut.blur.gaussian", "params": {"radius": {"value": 1e9}}}]}]})";
    }
    CHECK(h.errorOf("effects.importPresets", Json{{"path", pathToUtf8(junk)}}) == "InvalidParams");
    CHECK(h.errorOf("effects.importPresets", Json{{"path", pathToUtf8(dir / "missing.json")}}) == "NotFound");
}
