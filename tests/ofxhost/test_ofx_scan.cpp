// OpenFX host (0.7): scanning the plug-in folders, describing the plug-ins, their parameters as effect descriptors, the
// blocklist, the cache and the folders.

#include "OfxFixture.h"

#include <wabcut/ofxhost/OfxCatalog.h>
#include <wabcut/ofxhost/OfxSystem.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

const ofxhost::OfxParamInfo& param(const ofxhost::OfxPluginInfo& p, std::string_view name) {
    const ofxhost::OfxParamInfo* found = p.param(name);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("OpenFX scan: the test bundles are found and their plug-ins described", "[ofxhost][scan]") {
    TestOfx ofx;
    const auto plugins = ofx.catalog->plugins();
    // main: gain, invert8, invert16, fail, gpu, general, pair; crash: crash; hang: hang.
    CHECK(plugins.size() == 9);

    const auto gain = ofx.plugin(kGain);
    CHECK(gain.id == "org.wabcut.test.gain");
    CHECK(gain.name == "Wabcut Test Gain");
    CHECK(gain.group == "Wabcut Tests");
    CHECK(gain.version == "1.3");
    CHECK(gain.context == "filter");
    CHECK(gain.depth == "float");
    CHECK(gain.components == "rgba");
    CHECK(gain.problem.empty());
    CHECK(gain.path.filename() == "WabcutTestOfx.ofx");
    CHECK(gain.path.parent_path().filename() == "Win64"); // found inside its bundle

    // The page's order, not the declaration order; the hidden parameter and the button are not offered.
    std::vector<std::string> names;
    for (const auto& p : gain.params) {
        names.push_back(p.name);
    }
    CHECK(names == std::vector<std::string>{"label", "center", "tint", "mode", "gain", "offset", "skip", "bottomMark"});

    const auto& g = param(gain, "gain");
    CHECK(g.kind == ofxhost::OfxParamKind::Double);
    CHECK(g.label == "Gain");
    CHECK(g.hint == "Multiplies the colour");
    CHECK(g.animates);
    REQUIRE(g.defaults.size() == 1);
    CHECK(g.defaults[0] == Approx(1.0));
    CHECK(g.mins[0] == Approx(0.0));
    CHECK(g.maxs[0] == Approx(4.0));
    CHECK(g.displayMaxs[0] == Approx(2.0));

    const auto& mode = param(gain, "mode");
    CHECK(mode.kind == ofxhost::OfxParamKind::Choice);
    CHECK(mode.options == std::vector<std::string>{"Multiply", "Add", "Invert"});
    CHECK(param(gain, "offset").kind == ofxhost::OfxParamKind::Int);
    CHECK(param(gain, "skip").kind == ofxhost::OfxParamKind::Bool);
    const auto& tint = param(gain, "tint");
    CHECK(tint.kind == ofxhost::OfxParamKind::Rgb);
    CHECK(tint.defaults == std::vector<double>{1.0, 1.0, 1.0});
    const auto& center = param(gain, "center");
    CHECK(center.kind == ofxhost::OfxParamKind::Double2D);
    CHECK(center.spatial); // normalised coordinates: a fraction of the picture
    CHECK(center.defaults == std::vector<double>{0.5, 0.5});
    const auto& label = param(gain, "label");
    CHECK(label.kind == ofxhost::OfxParamKind::String);
    CHECK(label.textDefault == "hello");
    CHECK(gain.param("hidden") == nullptr);
    CHECK(gain.param("button") == nullptr);

    // The other pixel formats.
    CHECK(ofx.plugin(kInvert8).depth == "byte");
    CHECK(ofx.plugin(kInvert8).components == "rgba");
    CHECK(ofx.plugin(kInvert16).depth == "short");
    CHECK(ofx.plugin(kInvert16).components == "rgb");
    const auto general = ofx.plugin(kGeneral);
    CHECK(general.context == "general"); // a general-context plug-in whose second clip is optional is usable
    CHECK(general.problem.empty());
}

TEST_CASE("OpenFX scan: plug-ins that cannot run here are listed with the reason, never offered", "[ofxhost][scan]") {
    TestOfx ofx;
    const auto gpu = ofx.catalog->find("plugin.ofx.org.wabcut.test.gpu");
    REQUIRE(gpu);
    CHECK(gpu->problem.find("GPU") != std::string::npos);
    const auto pair = ofx.catalog->find("plugin.ofx.org.wabcut.test.pair");
    REQUIRE(pair);
    CHECK_FALSE(pair->problem.empty()); // it needs a second input clip
    // Not effects the model knows: a project that names them keeps them as unknown effects.
    CHECK(EffectRegistry::builtin().find("plugin.ofx.org.wabcut.test.gpu") == nullptr);
    CHECK(EffectRegistry::builtin().find("plugin.ofx.org.wabcut.test.pair") == nullptr);
    CHECK(EffectRegistry::builtin().find("plugin.ofx.not.installed") == nullptr);
    CHECK_FALSE(ofx.effects->implements("plugin.ofx.org.wabcut.test.gpu"));
    CHECK(ofx.effects->implements(kGain));
}

TEST_CASE("OpenFX descriptors: parameters become effect parameters with their ranges, menus and text settings", "[ofxhost][scan][model]") {
    TestOfx ofx;
    const EffectDescriptor* d = EffectRegistry::builtin().find(kGain);
    REQUIRE(d != nullptr);
    CHECK(d->displayName == "Wabcut Test Gain");
    CHECK(d->kind == EffectKind::Video);
    CHECK(d->category == "OpenFX Plug-ins/Wabcut Tests");
    CHECK_FALSE(d->intrinsic);

    const auto find = [&](std::string_view key) {
        const EffectParamInfo* p = d->param(key);
        REQUIRE(p != nullptr);
        return p;
    };
    const EffectParamInfo* gain = find("gain");
    CHECK(gain->unit == ParamUnit::Amount);
    CHECK(gain->minValue == Approx(0.0));
    CHECK(gain->maxValue == Approx(4.0));
    CHECK(gain->defaultValue == Approx(1.0));
    CHECK(gain->animatable);

    const EffectParamInfo* mode = find("mode");
    CHECK(mode->isChoice());
    CHECK(mode->optionLabels().size() == 3);
    CHECK(mode->optionLabel(1.0) == "Add");
    CHECK_FALSE(mode->animatable);
    CHECK(find("skip")->maxValue == 1.0);
    CHECK_FALSE(find("skip")->animatable); // a switch
    CHECK(find("offset")->maxValue == 100.0);
    // Multi-value parameters: one effect parameter per component.
    CHECK(find("tint.r")->unit == ParamUnit::Fraction);
    CHECK(find("tint.b")->defaultValue == Approx(1.0));
    CHECK(find("center.x")->unit == ParamUnit::Fraction); // a position as a fraction of the picture
    CHECK(find("center.y")->defaultValue == Approx(0.5));
    CHECK(d->param("tint") == nullptr);
    CHECK(d->param("label") == nullptr); // a string is a text setting
    REQUIRE(d->texts.size() == 1);
    CHECK(d->texts[0].key == "label");
    CHECK(d->texts[0].displayName == "Label");
    CHECK(d->texts[0].defaultValue == "hello");

    // The model validates an instance of it like any effect: parameters in range, an unknown key kept.
    EffectInstance e = ofxhost::makeOfxEffect(ofx.plugin(kGain), EffectInstanceId{1});
    e.set("gain", ParamValue(2.0));
    CHECK_FALSE(effectStackProblem({e}, EffectKind::Video, 100).has_value());
    CHECK(isEvaluable(e, EffectKind::Video));
    e.set("gain", ParamValue(9.0)); // beyond the plug-in's maximum
    CHECK(effectStackProblem({e}, EffectKind::Video, 100).has_value());
    // The name it was saved with, for the missing-plug-in badge.
    const EffectData* saved = e.findData("plugin.name");
    REQUIRE(saved != nullptr);
    CHECK(*saved->text() == "Wabcut Test Gain (Wabcut Tests)");
}

TEST_CASE("OpenFX scan: a binary that crashes while it is described goes on the blocklist", "[ofxhost][scan][crash]") {
    ofxhost::OfxCatalogOptions options;
    options.helper = ofxHostPath();
    options.folders = {ofxTestDir() / "blocked"};
    options.cacheFile = ofxOutput("catalog-blocked.json");
    std::filesystem::remove(options.cacheFile);
    ofxhost::OfxCatalog catalog(options);

    const auto first = catalog.scan();
    CHECK(first.files == 1);
    CHECK(first.plugins == 0);
    REQUIRE(first.blocked.size() == 1);
    CHECK(first.blocked[0].reason.find("crashed while being scanned") != std::string::npos);
    CHECK(first.blocked[0].reason.find("0xC0000005") != std::string::npos);
    CHECK(catalog.blocked().size() == 1);

    const auto second = catalog.scan(); // unchanged: skipped, not scanned (and not crashed) again
    CHECK(second.skipped == 1);
    CHECK(second.scanned == 0);

    catalog.unblock(first.blocked[0].path);
    CHECK(catalog.blocked().empty());
    const auto third = catalog.scan(); // scanned again, and blocked again
    CHECK(third.scanned == 1);
    CHECK(third.blocked.size() == 1);

    // The blocklist survives a restart.
    ofxhost::OfxCatalog restarted(options);
    REQUIRE(restarted.load());
    CHECK(restarted.blocked().size() == 1);
}

TEST_CASE("OpenFX scan: unchanged binaries come from the cache; the cache survives a restart", "[ofxhost][scan]") {
    ofxhost::OfxCatalogOptions options;
    options.helper = ofxHostPath();
    options.folders = {ofxTestDir() / "main"};
    options.cacheFile = ofxOutput("catalog-cache.json");
    std::filesystem::remove(options.cacheFile);
    {
        ofxhost::OfxCatalog catalog(options);
        const auto first = catalog.scan();
        CHECK(first.scanned == 1);
        CHECK(first.cached == 0);
        CHECK(first.plugins == 7);
        CHECK(first.added == 7);
        const auto second = catalog.scan();
        CHECK(second.scanned == 0);
        CHECK(second.cached == 1);
        CHECK(second.added == 0);
        CHECK(catalog.scan({}, {}, true).scanned == 1); // a rescan visits it again
    }
    ofxhost::OfxCatalog restarted(options);
    REQUIRE(restarted.load());
    CHECK(restarted.plugins().size() == 7); // known before any scan
    const auto gain = restarted.find(kGain);
    REQUIRE(gain);
    CHECK(gain->params.size() == 8); // parameters, ranges and menus come back from the cache
    CHECK(gain->params[3].options.size() == 3);
    CHECK(restarted.scan().cached == 1);
}

TEST_CASE("OpenFX scan: the standard folders and the user's", "[ofxhost][scan]") {
    // OFX_PLUGIN_PATH names more folders (only those that exist count).
    const std::wstring list = ofxTestDir().wstring() + L"\\main;" + ofxTestDir().wstring() + L"\\nowhere";
    _wputenv_s(L"OFX_PLUGIN_PATH", list.c_str());
    const auto folders = ofxhost::OfxCatalog::standardFolders();
    _wputenv_s(L"OFX_PLUGIN_PATH", L"");
    CHECK(std::find(folders.begin(), folders.end(), (ofxTestDir() / "main").lexically_normal()) != folders.end());
    CHECK(std::find(folders.begin(), folders.end(), (ofxTestDir() / "nowhere").lexically_normal()) == folders.end());

    ofxhost::OfxCatalogOptions options;
    options.helper = ofxHostPath();
    options.folders = {ofxTestDir() / "none"}; // nothing there
    options.cacheFile = ofxOutput("catalog-user.json");
    std::filesystem::remove(options.cacheFile);
    ofxhost::OfxCatalog catalog(options);
    CHECK(catalog.scan().files == 0);
    catalog.addUserFolder(ofxTestDir() / "main");
    CHECK(catalog.folders().size() == 2);
    const auto scanned = catalog.scan();
    CHECK(scanned.files == 1);
    CHECK(scanned.plugins == 7);
    catalog.removeUserFolder(ofxTestDir() / "main");
    CHECK(catalog.scan().plugins == 0); // plug-ins that went away are forgotten
}

TEST_CASE("OpenFX scan: the catalog as the plugin library of plugins.list and plugins.scan", "[ofxhost][scan][commands]") {
    ofxhost::OfxCatalogOptions options;
    options.helper = ofxHostPath();
    options.folders = {ofxTestDir() / "main"};
    options.cacheFile = ofxOutput("catalog-library.json");
    std::filesystem::remove(options.cacheFile);
    auto catalog = std::make_shared<ofxhost::OfxCatalog>(options);
    const auto library = ofxhost::makeOfxPluginLibrary(catalog);

    // Scanning through the command's library: its numbers, the progress, the folders.
    double last = -1.0;
    const auto summary = library->scan({}, {}, [&](double fraction) { last = fraction; });
    REQUIRE(summary);
    CHECK(summary->files == 1);
    CHECK(summary->scanned == 1);
    CHECK(summary->plugins == 7);
    CHECK(summary->added == 7);
    CHECK(last == Approx(1.0));
    const auto folders = library->folders();
    REQUIRE(folders.size() == 1);
    CHECK(folders[0].find("main") != std::string::npos);

    // The listing: effect ids, the format, the group as the vendor, parameters by effect key.
    const auto listing = library->plugins();
    REQUIRE(listing.size() == 7);
    const auto gain = std::find_if(listing.begin(), listing.end(), [](const auto& p) { return p.effect == kGain; });
    REQUIRE(gain != listing.end());
    CHECK(gain->format == "openfx");
    CHECK(gain->name == "Wabcut Test Gain");
    CHECK(gain->vendor == "Wabcut Tests");
    CHECK(gain->category == "OpenFX Plug-ins/Wabcut Tests");
    CHECK(gain->version == "1.3");
    CHECK(gain->problem.empty());
    CHECK_FALSE(gain->editor);
    std::vector<std::string> keys;
    for (const auto& p : gain->params) {
        keys.push_back(p.key);
    }
    CHECK(keys == std::vector<std::string>{"center.x", "center.y", "tint.r", "tint.g", "tint.b", "mode", "gain", "offset", "skip", "bottomMark"});
    const auto mode = std::find_if(gain->params.begin(), gain->params.end(), [](const auto& p) { return p.key == "mode"; });
    CHECK(mode->maxValue == Approx(2.0));
    CHECK_FALSE(mode->automatable);
    const auto gpu = std::find_if(listing.begin(), listing.end(), [](const auto& p) { return p.effect == "plugin.ofx.org.wabcut.test.gpu"; });
    REQUIRE(gpu != listing.end());
    CHECK(gpu->problem.find("GPU") != std::string::npos); // listed, with the reason

    // Folders added through the command are scanned too.
    audio::edit::PluginScanOptions more;
    more.addFolders = {(ofxTestDir() / "crash").string()};
    const auto again = library->scan(more, {}, {});
    REQUIRE(again);
    CHECK(again->files == 2);
    CHECK(again->plugins == 8);
    CHECK(library->folders().size() == 2);
}
