// OpenFX host (0.7), opt-in: plug-ins built from the OpenFX SDK's own examples (github.com/AcademySoftwareFoundation/openfx,
// Examples/Invert, Basic, ChoiceParams, Rectangle), the kind of code real plug-ins are written like. Hidden from the default
// run: build the four examples as bundles (`<Name>.ofx.bundle/Contents/Win64/<Name>.ofx`) into one folder and run
//   WABCUT_OFX_SDK_DIR=<folder> wabcut_ofxhost_tests "[.sdk]"
// (the SDK is not part of this repository; only its headers are vendored).

#include "OfxFixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

std::filesystem::path sdkDir() {
    const char* dir = std::getenv("WABCUT_OFX_SDK_DIR");
    return dir != nullptr ? std::filesystem::path(dir) : std::filesystem::path();
}

} // namespace

TEST_CASE("OpenFX SDK examples: Invert, Basic Gain, Choice Params and Rectangle describe and render through the helper", "[ofxhost][.sdk]") {
    const std::filesystem::path dir = sdkDir();
    REQUIRE_FALSE(dir.empty()); // set WABCUT_OFX_SDK_DIR to the folder of the built examples
    ofxhost::OfxCatalogOptions options;
    options.helper = ofxHostPath();
    options.folders = {dir};
    options.cacheFile = ofxOutput("catalog-sdk.json");
    std::filesystem::remove(options.cacheFile);
    auto catalog = std::make_shared<ofxhost::OfxCatalog>(options);
    const auto report = catalog->scan();
    CHECK(report.blocked.empty());
    CHECK(report.plugins >= 4);
    for (const auto& p : catalog->plugins()) {
        INFO(p.id << ": " << p.problem);
        CHECK(p.problem.empty());
    }
    auto host = ofxhost::OfxPluginHost::create(testOfxHostOptions());

    const auto find = [&](const std::string& id) {
        for (const auto& p : catalog->plugins()) {
            if (p.id == id) {
                return p;
            }
        }
        FAIL("no plug-in " << id);
        return ofxhost::OfxPluginInfo{};
    };
    constexpr int w = 32;
    constexpr int h = 16;
    const auto input = flatPicture(w, h, 0.2f, 0.4f, 0.6f);
    std::vector<float> output(input.size());

    SECTION("Invert (8-bit)") {
        auto instance = host->instantiate(find("uk.co.thefoundry.OfxInvertExample"), w, h, 24.0);
        REQUIRE(instance);
        REQUIRE((*instance)->render(renderRequest(w, h, nlohmann::json::object()), input.data(), output.data(), false));
        CHECK(pixelAt(output, w, 5, 5)[0] == Approx(0.8f).margin(1.0f / 255.0f));
        CHECK(pixelAt(output, w, 5, 5)[1] == Approx(0.6f).margin(1.0f / 255.0f));
        CHECK(pixelAt(output, w, 5, 5)[2] == Approx(0.4f).margin(1.0f / 255.0f));
    }
    SECTION("Basic Gain") {
        const auto gain = find("uk.co.thefoundry.BasicGainPlugin");
        auto instance = host->instantiate(gain, w, h, 24.0);
        REQUIRE(instance);
        REQUIRE((*instance)->render(renderRequest(w, h, nlohmann::json{{"scale", 0.5}, {"scaleComponents", true}, {"scaleR", 1.0}, {"scaleG", 1.0}, {"scaleB", 1.0}, {"scaleA", 1.0}}),
                                    input.data(), output.data(), false));
        CHECK(pixelAt(output, w, 3, 3)[0] == Approx(0.1f).margin(1e-4));
        CHECK(pixelAt(output, w, 3, 3)[2] == Approx(0.3f).margin(1e-4));
    }
    SECTION("Choice Params and Rectangle render") {
        auto choice = host->instantiate(find("org.openfx.ChoiceParamPlugin"), w, h, 24.0);
        REQUIRE(choice);
        CHECK((*choice)->render(renderRequest(w, h, nlohmann::json{{"red_choice", 1}, {"green_choice", 2}, {"blue_choice", 0}}), input.data(), output.data(), false));
        auto rectangle = host->instantiate(find("uk.co.thefoundry.GeneratorExample"), w, h, 24.0);
        REQUIRE(rectangle);
        CHECK((*rectangle)->render(renderRequest(w, h, nlohmann::json{{"colour", {1.0, 0.0, 0.0, 1.0}}}), input.data(), output.data(), false));
    }
}
