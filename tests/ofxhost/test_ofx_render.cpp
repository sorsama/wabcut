// OpenFX host (0.7): pictures through the helper - parameters of every kind, the pixel depths and components plug-ins ask
// for, the orientation of the picture, identity, a picture larger than the block it was created for, and a plug-in that fails.

#include "OfxFixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

std::shared_ptr<ofxhost::OfxRemoteInstance> instance(TestOfx& ofx, const char* effectId, int w, int h) {
    auto made = ofx.host->instantiate(ofx.plugin(effectId), w, h, 30.0);
    REQUIRE(made);
    return *made;
}

/// Renders `input` through a new instance of `effectId` with `params`.
std::vector<float> run(TestOfx& ofx, const char* effectId, int w, int h, const std::vector<float>& input, nlohmann::json params,
                       bool hasAlpha = true) {
    auto plugin = instance(ofx, effectId, w, h);
    std::vector<float> output(input.size(), -1.0f);
    const auto rendered = plugin->render(renderRequest(w, h, std::move(params), hasAlpha), input.data(), output.data(), false);
    const std::string why = rendered ? std::string() : rendered.error().message;
    INFO(why);
    REQUIRE(rendered);
    return output;
}

} // namespace

TEST_CASE("OpenFX render: a double parameter and a tint scale the picture", "[ofxhost][render]") {
    TestOfx ofx;
    constexpr int w = 16;
    constexpr int h = 8;
    const auto input = gradientPicture(w, h);
    auto params = gainParams(0.5);
    params["tint"] = {1.0, 0.5, 0.0};
    const auto out = run(ofx, kGain, w, h, input, params);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float* in = pixelAt(input, w, x, y);
            const float* o = pixelAt(out, w, x, y);
            REQUIRE(o[0] == Approx(in[0] * 0.5f).margin(1e-6));
            REQUIRE(o[1] == Approx(in[1] * 0.25f).margin(1e-6));
            REQUIRE(o[2] == Approx(0.0f).margin(1e-6));
            REQUIRE(o[3] == Approx(1.0f));
        }
    }
}

TEST_CASE("OpenFX render: choice, integer and boolean parameters", "[ofxhost][render]") {
    TestOfx ofx;
    constexpr int w = 8;
    constexpr int h = 4;
    const auto input = gradientPicture(w, h);
    SECTION("Add: the integer offset, in 8-bit steps") {
        auto params = gainParams();
        params["mode"] = 1;
        params["offset"] = 51; // 51 / 255 = 0.2
        const auto out = run(ofx, kGain, w, h, input, params);
        CHECK(pixelAt(out, w, 3, 2)[0] == Approx(pixelAt(input, w, 3, 2)[0] + 0.2f).margin(1e-6));
        CHECK(pixelAt(out, w, 3, 2)[2] == Approx(0.3f + 0.2f).margin(1e-6));
    }
    SECTION("Invert") {
        auto params = gainParams();
        params["mode"] = 2;
        const auto out = run(ofx, kGain, w, h, input, params);
        CHECK(pixelAt(out, w, 1, 1)[0] == Approx(1.0f - pixelAt(input, w, 1, 1)[0]).margin(1e-6));
        CHECK(pixelAt(out, w, 1, 1)[3] == Approx(1.0f));
    }
    SECTION("Skip: the plug-in says it changes nothing, and the picture passes through") {
        auto params = gainParams(0.25);
        params["skip"] = true;
        const auto out = run(ofx, kGain, w, h, input, params);
        CHECK(out == input);
    }
}

TEST_CASE("OpenFX render: the picture keeps its orientation", "[ofxhost][render]") {
    TestOfx ofx;
    constexpr int w = 16;
    constexpr int h = 8;
    const auto input = gradientPicture(w, h);
    auto params = gainParams();
    params["bottomMark"] = true; // the plug-in paints the BOTTOM row green (OpenFX rows start at the bottom)
    const auto out = run(ofx, kGain, w, h, input, params);
    for (int x = 0; x < w; ++x) {
        const float* bottom = pixelAt(out, w, x, h - 1);
        REQUIRE(bottom[0] == Approx(0.0f));
        REQUIRE(bottom[1] == Approx(1.0f));
        REQUIRE(bottom[2] == Approx(0.0f));
        const float* top = pixelAt(out, w, x, 0);
        REQUIRE(top[0] == Approx(pixelAt(input, w, x, 0)[0]).margin(1e-6)); // the top row is untouched
        REQUIRE(top[1] == Approx(pixelAt(input, w, x, 0)[1]).margin(1e-6));
    }
}

TEST_CASE("OpenFX render: a position in normalised coordinates reaches the plug-in in pixels; a string parameter reaches it", "[ofxhost][render]") {
    TestOfx ofx;
    constexpr int w = 16;
    constexpr int h = 8;
    const auto input = gradientPicture(w, h);
    auto params = gainParams();
    params["center"] = {0.5, 0.5}; // half of 16 pixels: the plug-in draws a red column at x = 8
    params["label"] = "abcdef";    // six characters: written into the blue of the bottom-left pixel
    const auto out = run(ofx, kGain, w, h, input, params);
    for (int y = 0; y < h; ++y) {
        REQUIRE(pixelAt(out, w, 8, y)[0] == Approx(1.0f));
        REQUIRE(pixelAt(out, w, 8, y)[1] == Approx(0.0f));
        REQUIRE(pixelAt(out, w, 7, y)[0] == Approx(pixelAt(input, w, 7, y)[0]).margin(1e-6)); // its neighbours are not
    }
    CHECK(pixelAt(out, w, 0, h - 1)[2] == Approx(6.0f / 255.0f).margin(1e-6));
    CHECK(pixelAt(out, w, 1, h - 1)[2] == Approx(0.3f).margin(1e-6));
}

TEST_CASE("OpenFX render: Byte RGBA and Short RGB plug-ins get the pictures they ask for", "[ofxhost][render][depth]") {
    TestOfx ofx;
    constexpr int w = 8;
    constexpr int h = 8;
    SECTION("8-bit RGBA") {
        const auto out = run(ofx, kInvert8, w, h, flatPicture(w, h, 0.2f, 0.4f, 0.6f, 1.0f), nlohmann::json::object());
        const float* p = pixelAt(out, w, 3, 3);
        CHECK(p[0] == Approx(0.8f).margin(1.0f / 255.0f)); // 0.2 -> 51 -> 204
        CHECK(p[1] == Approx(0.6f).margin(1.0f / 255.0f));
        CHECK(p[2] == Approx(0.4f).margin(1.0f / 255.0f));
        CHECK(p[3] == Approx(1.0f));
    }
    SECTION("16-bit RGB: a plug-in without alpha gives an opaque picture") {
        const auto out = run(ofx, kInvert16, w, h, flatPicture(w, h, 0.25f, 0.5f, 0.75f, 0.5f), nlohmann::json::object());
        const float* p = pixelAt(out, w, 2, 5);
        CHECK(p[0] == Approx(0.75f).margin(1e-4));
        CHECK(p[1] == Approx(0.5f).margin(1e-4));
        CHECK(p[2] == Approx(0.25f).margin(1e-4));
        CHECK(p[3] == Approx(1.0f));
    }
}

TEST_CASE("OpenFX render: the General context, with an optional input that has no picture", "[ofxhost][render]") {
    TestOfx ofx;
    const auto out = run(ofx, kGeneral, 8, 4, flatPicture(8, 4, 0.8f, 0.4f, 0.2f), nlohmann::json::object());
    CHECK(pixelAt(out, 8, 0, 0)[0] == Approx(0.4f).margin(1e-6));
    CHECK(pixelAt(out, 8, 7, 3)[2] == Approx(0.1f).margin(1e-6));
}

TEST_CASE("OpenFX render: parameters change between pictures; a larger picture than the block was made for", "[ofxhost][render]") {
    TestOfx ofx;
    auto plugin = instance(ofx, kGain, 8, 8);
    const auto small = flatPicture(8, 8, 0.8f, 0.8f, 0.8f);
    std::vector<float> out(small.size());
    REQUIRE(plugin->render(renderRequest(8, 8, gainParams(0.5)), small.data(), out.data(), false));
    CHECK(pixelAt(out, 8, 4, 4)[0] == Approx(0.4f).margin(1e-6));
    REQUIRE(plugin->render(renderRequest(8, 8, gainParams(0.25)), small.data(), out.data(), false));
    CHECK(pixelAt(out, 8, 4, 4)[0] == Approx(0.2f).margin(1e-6));
    // 64 x 32 does not fit the block of an 8 x 8 instance: the host makes a bigger one and tells the helper.
    const auto big = flatPicture(64, 32, 0.6f, 0.6f, 0.6f);
    std::vector<float> bigOut(big.size());
    REQUIRE(plugin->render(renderRequest(64, 32, gainParams(0.5)), big.data(), bigOut.data(), false));
    CHECK(pixelAt(bigOut, 64, 63, 31)[0] == Approx(0.3f).margin(1e-6));
    CHECK(plugin->renders() == 3);
    CHECK(plugin->health() == ofxhost::OfxHealth::Running);
    // A picture no plug-in is offered.
    std::vector<float> none(16);
    CHECK_FALSE(plugin->render(renderRequest(5000, 5000, gainParams()), none.data(), none.data(), false));
}

TEST_CASE("OpenFX render: a plug-in that fails every picture is failed after three, with the plug-in's message", "[ofxhost][render]") {
    TestOfx ofx;
    auto plugin = instance(ofx, kFail, 8, 8);
    const auto input = flatPicture(8, 8, 0.5f, 0.5f, 0.5f);
    std::vector<float> out(input.size());
    for (int i = 0; i < 3; ++i) {
        const auto r = plugin->render(renderRequest(8, 8, nlohmann::json::object()), input.data(), out.data(), false);
        REQUIRE_FALSE(r);
        CHECK(r.error().message == "this plug-in always fails");
    }
    CHECK(plugin->health() == ofxhost::OfxHealth::Failed);
    const auto again = plugin->render(renderRequest(8, 8, nlohmann::json::object()), input.data(), out.data(), false);
    REQUIRE_FALSE(again);
    CHECK(again.error().message == "this plug-in always fails"); // bypassed: the reason stays
}

TEST_CASE("OpenFX render: instances of one plug-in share a helper, and each keeps its own parameters", "[ofxhost][render]") {
    TestOfx ofx;
    auto a = instance(ofx, kGain, 8, 8);
    auto b = instance(ofx, kGain, 8, 8);
    auto c = instance(ofx, kInvert8, 8, 8); // the same binary
    CHECK(ofx.host->processCount() == 1);
    const auto input = flatPicture(8, 8, 0.8f, 0.8f, 0.8f);
    std::vector<float> oa(input.size());
    std::vector<float> ob(input.size());
    REQUIRE(a->render(renderRequest(8, 8, gainParams(0.5)), input.data(), oa.data(), false));
    REQUIRE(b->render(renderRequest(8, 8, gainParams(0.25)), input.data(), ob.data(), false));
    CHECK(pixelAt(oa, 8, 1, 1)[0] == Approx(0.4f).margin(1e-6));
    CHECK(pixelAt(ob, 8, 1, 1)[0] == Approx(0.2f).margin(1e-6));
    REQUIRE(a->render(renderRequest(8, 8, gainParams(0.5)), input.data(), oa.data(), false)); // a again: not b's value
    CHECK(pixelAt(oa, 8, 1, 1)[0] == Approx(0.4f).margin(1e-6));
}
