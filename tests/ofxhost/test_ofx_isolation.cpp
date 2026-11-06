// OpenFX host (0.7): a plug-in that crashes or hangs disables only itself. The editor (this process) keeps running, the
// plug-in's instances report why, other plug-ins' helpers carry on, and a new instance gets a new helper.

#include "OfxFixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

TEST_CASE("OpenFX isolation: a plug-in that crashes while it renders is disabled; the editor and other plug-ins carry on", "[ofxhost][isolation][crash]") {
    TestOfx ofx;
    auto good = ofx.host->instantiate(ofx.plugin(kGain), 8, 8, 30.0);
    auto bad = ofx.host->instantiate(ofx.plugin(kCrash), 8, 8, 30.0);
    REQUIRE(good);
    REQUIRE(bad);
    CHECK(ofx.host->processCount() == 2); // one helper per binary
    const auto input = flatPicture(8, 8, 0.8f, 0.8f, 0.8f);
    std::vector<float> out(input.size());

    const auto crashed = (*bad)->render(renderRequest(8, 8, nlohmann::json::object()), input.data(), out.data(), false);
    REQUIRE_FALSE(crashed);
    CHECK(crashed.error().code == ErrorCode::IoError);
    CHECK(crashed.error().message.find("crashed") != std::string::npos);
    CHECK(crashed.error().message.find("0xC0000005") != std::string::npos); // the access violation
    CHECK((*bad)->health() == ofxhost::OfxHealth::Crashed);
    CHECK((*bad)->problem().find("0xC0000005") != std::string::npos);
    CHECK(ofx.host->processCount() == 1); // its helper is gone

    // Asked again, it answers at once with the same reason (nothing waits for a dead helper).
    const auto start = std::chrono::steady_clock::now();
    const auto again = (*bad)->render(renderRequest(8, 8, nlohmann::json::object()), input.data(), out.data(), false);
    CHECK_FALSE(again);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));

    // The other plug-in's helper is untouched.
    REQUIRE((*good)->render(renderRequest(8, 8, gainParams(0.5)), input.data(), out.data(), false));
    CHECK(pixelAt(out, 8, 2, 2)[0] == Approx(0.4f).margin(1e-6));
    CHECK((*good)->health() == ofxhost::OfxHealth::Running);

    // A new instance of the crashing plug-in starts a new helper (and crashes the same way: the editor is still here).
    auto next = ofx.host->instantiate(ofx.plugin(kCrash), 8, 8, 30.0);
    REQUIRE(next);
    CHECK(ofx.host->processCount() == 2);
    CHECK_FALSE((*next)->render(renderRequest(8, 8, nlohmann::json::object()), input.data(), out.data(), false));
    CHECK((*next)->health() == ofxhost::OfxHealth::Crashed);
}

TEST_CASE("OpenFX isolation: a plug-in that never returns a picture is ended after the timeout and counts as hung", "[ofxhost][isolation][hang]") {
    ofxhost::OfxHostOptions options = testOfxHostOptions();
    options.previewRenderTimeout = std::chrono::milliseconds(700);
    TestOfx ofx(options);
    auto hung = ofx.host->instantiate(ofx.plugin(kHang), 8, 8, 30.0);
    auto good = ofx.host->instantiate(ofx.plugin(kGain), 8, 8, 30.0);
    REQUIRE(hung);
    REQUIRE(good);
    const auto input = flatPicture(8, 8, 0.8f, 0.8f, 0.8f);
    std::vector<float> out(input.size());

    const auto start = std::chrono::steady_clock::now();
    const auto r = (*hung)->render(renderRequest(8, 8, nlohmann::json::object()), input.data(), out.data(), false);
    const auto took = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("did not return a picture within 700 ms") != std::string::npos);
    CHECK(took < std::chrono::seconds(10)); // ended after its timeout, not waited for
    CHECK((*hung)->health() == ofxhost::OfxHealth::Hung);
    CHECK(ofx.host->processCount() == 1); // the hung helper was ended

    REQUIRE((*good)->render(renderRequest(8, 8, gainParams(0.5)), input.data(), out.data(), false));
    CHECK(pixelAt(out, 8, 0, 0)[0] == Approx(0.4f).margin(1e-6));
}

TEST_CASE("OpenFX isolation: an offline render waits longer than the preview does", "[ofxhost][isolation][hang]") {
    ofxhost::OfxHostOptions options = testOfxHostOptions();
    options.previewRenderTimeout = std::chrono::milliseconds(300);
    options.offlineRenderTimeout = std::chrono::milliseconds(2000);
    TestOfx ofx(options);
    auto hung = ofx.host->instantiate(ofx.plugin(kHang), 8, 8, 30.0);
    REQUIRE(hung);
    const auto input = flatPicture(8, 8, 0.8f, 0.8f, 0.8f);
    std::vector<float> out(input.size());
    const auto start = std::chrono::steady_clock::now();
    const auto r = (*hung)->render(renderRequest(8, 8, nlohmann::json::object()), input.data(), out.data(), true);
    const auto took = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(r);
    CHECK(took >= std::chrono::milliseconds(1800)); // it waited for the offline timeout, not the preview's
    CHECK(r.error().message.find("within 2 s") != std::string::npos);
    CHECK((*hung)->health() == ofxhost::OfxHealth::Hung);
}
