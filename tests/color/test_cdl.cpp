// ASC CDL: .cc / .ccc / .cdl reading (namespaces, SATNode, descriptions), writing, round trips and errors.

#include <wabcut/color/Cdl.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace wabcut;
using namespace wabcut::color;

TEST_CASE("CDL: .cc, .ccc and .cdl read; namespaces and SATNode", "[color][cdl]") {
    const char* cc = R"(<?xml version="1.0" encoding="UTF-8"?>
<ColorCorrection id="shot_010">
  <SOPNode>
    <Description>warm it up</Description>
    <Slope>1.1 1.0 0.9</Slope>
    <Offset>0.01 0 -0.02</Offset>
    <Power>0.95 1 1.05</Power>
  </SOPNode>
  <SATNode><Saturation>0.85</Saturation></SATNode>
</ColorCorrection>)";
    const auto one = parseCdl(cc);
    REQUIRE(one);
    REQUIRE(one->size() == 1);
    const Cdl& c = one->front();
    CHECK(c.id == "shot_010");
    CHECK(c.description == "warm it up");
    CHECK(c.slope == Rgb{1.1, 1.0, 0.9});
    CHECK(c.offset == Rgb{0.01, 0, -0.02});
    CHECK(c.power == Rgb{0.95, 1, 1.05});
    CHECK(c.saturation == 0.85);

    const char* ccc = R"(<ColorCorrectionCollection xmlns="urn:ASC:CDL:v1.01">
  <ColorCorrection id="a"><SOPNode><Slope>2 2 2</Slope><Offset>0 0 0</Offset><Power>1 1 1</Power></SOPNode></ColorCorrection>
  <ColorCorrection id="b"><SatNode><Saturation>0</Saturation></SatNode></ColorCorrection>
</ColorCorrectionCollection>)";
    const auto many = parseCdl(ccc);
    REQUIRE(many);
    REQUIRE(many->size() == 2);
    CHECK((*many)[0].slope == Rgb{2, 2, 2});
    CHECK((*many)[1].saturation == 0.0);
    CHECK((*many)[1].slope == Rgb{1, 1, 1}); // missing SOP: identity

    const char* cdl = R"(<cdl:ColorDecisionList xmlns:cdl="urn:ASC:CDL:v1.2"><cdl:ColorDecision>
  <cdl:ColorCorrection id="x"><cdl:SOPNode><cdl:Slope>1 1 1</cdl:Slope><cdl:Offset>0.1 0.1 0.1</cdl:Offset>
  <cdl:Power>1 1 1</cdl:Power></cdl:SOPNode></cdl:ColorCorrection></cdl:ColorDecision></cdl:ColorDecisionList>)";
    const auto list = parseCdl(cdl);
    REQUIRE(list);
    CHECK(list->front().offset == Rgb{0.1, 0.1, 0.1});
}

TEST_CASE("CDL: errors are clear", "[color][cdl]") {
    CHECK_FALSE(parseCdl("not xml <"));
    CHECK_FALSE(parseCdl("<Something/>"));
    const auto bad = parseCdl("<ColorCorrection><SOPNode><Slope>1 1</Slope></SOPNode></ColorCorrection>");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().message.find("Slope needs 3 numbers") != std::string::npos);
    CHECK_FALSE(parseCdl("<ColorCorrection><SOPNode><Slope>1 x 1</Slope></SOPNode></ColorCorrection>"));
    CHECK_FALSE(parseCdl("<ColorCorrection><SOPNode><Slope>-1 1 1</Slope></SOPNode></ColorCorrection>"));
    CHECK_FALSE(parseCdl("<ColorCorrection><SOPNode><Power>0 1 1</Power></SOPNode></ColorCorrection>"));
    CHECK_FALSE(parseCdl("<ColorCorrection><SatNode><Saturation>nan</Saturation></SatNode></ColorCorrection>"));
}

TEST_CASE("CDL: write and read back, all three formats, through files", "[color][cdl]") {
    Cdl a;
    a.id = "clip <1> & co";
    a.description = "a \"quoted\" note";
    a.slope = {1.2, 0.9, 1.05};
    a.offset = {-0.015, 0.02, 0.0};
    a.power = {0.8, 1.0, 1.3};
    a.saturation = 1.25;
    Cdl b;
    b.id = "two";
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "color";
    std::filesystem::create_directories(dir);
    for (const char* name : {"one.cc", "many.ccc", "many.cdl"}) {
        const auto file = dir / name;
        const std::vector<Cdl> corrections = std::string(name) == "one.cc" ? std::vector<Cdl>{a} : std::vector<Cdl>{a, b};
        REQUIRE(saveCdl(corrections, file));
        const auto back = loadCdl(file);
        REQUIRE(back);
        REQUIRE(back->size() == corrections.size());
        for (std::size_t i = 0; i < corrections.size(); ++i) {
            CHECK((*back)[i] == corrections[i]); // 6 decimals are exact for these values
        }
    }
    CHECK(cdlFormatOf("x.CDL") == CdlFormat::Cdl);
    CHECK(cdlFormatOf("x.ccc") == CdlFormat::Ccc);
    CHECK(cdlFormatOf("x.cc") == CdlFormat::Cc);
    // Identity and the wheels mapping.
    CHECK(Cdl{}.isIdentity());
    GradeParams p;
    CHECK(cdlOf(p).isIdentity());
    p.lift = {0.1, 0.1, 0.1};
    p.gain = {1.0, 1.0, 1.0};
    const Cdl lifted = cdlOf(p);
    CHECK(lifted.slope == Rgb{0.9, 0.9, 0.9});
    CHECK(lifted.offset == Rgb{0.1, 0.1, 0.1});
}

TEST_CASE("CDL: a tiny power or a value six decimals cannot hold writes back exactly (C.10 fuzzing)", "[color][cdl]") {
    // The fuzzer's minimal reproducer: power 1e-7 was written as 0.000000, which parseCdl refuses.
    const auto tiny = parseCdl("<ColorCorrection><SOPNode><Power>1e-7 1 1</Power></SOPNode></ColorCorrection>");
    REQUIRE(tiny);
    REQUIRE(tiny->size() == 1);
    const std::string written = writeCdl(*tiny, CdlFormat::Cc);
    CHECK(written.find("0.000000 1.000000") == std::string::npos);
    const auto back = parseCdl(written);
    REQUIRE(back);
    CHECK(back->front() == tiny->front());
    CHECK(writeCdl(*back, CdlFormat::Cc) == written);
    // Values six decimals hold are still written the usual way.
    CHECK(written.find("<Slope>1.000000 1.000000 1.000000</Slope>") != std::string::npos);
    Cdl precise;
    precise.slope = {1.0000001, 0.1234567, 2.0};
    precise.saturation = 0.9999999;
    const auto exact = parseCdl(writeCdl({precise}, CdlFormat::Cc));
    REQUIRE(exact);
    CHECK(exact->front() == precise);
}
