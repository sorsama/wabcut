// 0.7 (card #138): OpenType features and optical kerning on a text style (Graphics.h): the feature list's syntax and its
// bounds, saved with the graphic only when set (older graphics read and write unchanged), refused when unreadable, and handed
// to the draw list the renderer lays out.

#include "TimelineFixture.h"

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;

namespace {

std::string tagOf(const FontFeature& f) { return std::string(f.tag.data(), 4); }

GraphicLayer persianLayer() {
    GraphicLayer l;
    l.id = 1;
    l.name = "Persian";
    l.kind = LayerKind::Text;
    TextStyle s;
    s.fontSize = 80.0;
    s.features = "calt, -liga, ss01";
    l.text.runs = {TextRun{"\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85", s}}; // "salam"
    return l;
}

} // namespace

TEST_CASE("OpenType feature lists read the CSS-like spellings and normalise (card 138)", "[model][graphics][typography]") {
    const auto read = [](std::string_view text) {
        const auto list = parseFontFeatures(text);
        INFO(text << " -> " << (list ? toString(*list) : list.error()));
        REQUIRE(list);
        return *list;
    };
    CHECK(read("").empty());
    CHECK(read("   ").empty());
    CHECK(toString(read("calt")) == "calt");
    CHECK(toString(read("+calt")) == "calt");
    CHECK(toString(read("calt=1")) == "calt");
    CHECK(toString(read("-liga")) == "-liga");
    CHECK(toString(read("liga=0")) == "-liga");
    CHECK(toString(read("liga off")) == "-liga");
    CHECK(toString(read("salt=2")) == "salt=2");
    CHECK(toString(read("\"smcp\" 1")) == "smcp");
    CHECK(toString(read("'dlig' on, \"ss01\" 0")) == "dlig, -ss01");
    CHECK(toString(read("calt liga ss01")) == "calt, liga, ss01"); // spaces between tags work too
    CHECK(toString(read("calt, -liga, salt=2, ss01")) == "calt, -liga, salt=2, ss01");
    // A repeated tag keeps its first place and its last value.
    CHECK(toString(read("liga, ss01, -liga")) == "-liga, ss01");
    const auto list = read("calt, ss01=3");
    REQUIRE(list.size() == 2);
    CHECK(tagOf(list[0]) == "calt");
    CHECK(list[0].value == 1);
    CHECK(tagOf(list[1]) == "ss01");
    CHECK(list[1].value == 3);
    // The normal form reads back to the same list.
    CHECK(read(toString(list)) == list);

    for (const char* bad : {"cal", "caltt", "-", "calt=", "calt=100", "calt=x", "ca lt!", "\"calt'", "liga 1 2", "=1", "calt,,%%%%"}) {
        INFO(bad);
        CHECK_FALSE(parseFontFeatures(bad));
    }
    // Bounded: sixteen features, 512 characters.
    std::string many;
    for (int i = 0; i < 17; ++i) {
        many += (i ? ", ss" : "ss") + std::string(i < 9 ? "0" : "") + std::to_string(i + 1);
    }
    CHECK_FALSE(parseFontFeatures(many));
    CHECK_FALSE(parseFontFeatures(std::string(513, ' ')));
}

TEST_CASE("Features are turned on and off one at a time, keeping only what differs from the shaper's defaults (card 138)",
          "[model][graphics][typography]") {
    CHECK(fontFeatureDefaultOn("liga"));
    CHECK(fontFeatureDefaultOn("calt"));
    CHECK(fontFeatureDefaultOn("kern"));
    CHECK_FALSE(fontFeatureDefaultOn("dlig"));
    CHECK_FALSE(fontFeatureDefaultOn("ss01"));

    CHECK(fontFeatureOn("", "liga"));
    CHECK_FALSE(fontFeatureOn("", "dlig"));
    CHECK_FALSE(fontFeatureOn("-liga", "liga"));
    CHECK(fontFeatureOn("dlig", "dlig"));
    CHECK(fontFeatureOn("ss01, -liga", "calt"));

    auto next = withFontFeature("", "dlig", true);
    REQUIRE(next);
    CHECK(*next == "dlig");
    next = withFontFeature(*next, "liga", false);
    REQUIRE(next);
    CHECK(*next == "dlig, -liga");
    // Back to the default drops the entry; a list of defaults is empty.
    next = withFontFeature(*next, "liga", true);
    REQUIRE(next);
    CHECK(*next == "dlig");
    next = withFontFeature(*next, "dlig", false);
    REQUIRE(next);
    CHECK(next->empty());
    // A value that is not on or off (salt=2) turns off by being removed; switching it on makes it 1.
    next = withFontFeature("salt=2", "salt", true);
    REQUIRE(next);
    CHECK(*next == "salt");
    CHECK_FALSE(withFontFeature("", "no", true));
    CHECK_FALSE(withFontFeature("not a list!", "dlig", true));
}

TEST_CASE("Features and Optical kerning are saved only when set and read back; an unreadable list is refused (card 138)",
          "[model][graphics][typography]") {
    Graphic g;
    g.name = "Persian";
    g.layers = {persianLayer()};
    g.layers[0].text.runs[0].style.opticalKerning = true;
    REQUIRE_FALSE(graphicProblem(g));
    const std::string json = io::graphicToJson(g);
    CHECK(json.find("\"features\"") != std::string::npos);
    CHECK(json.find("\"opticalKerning\"") != std::string::npos);
    const auto back = io::graphicFromJson(json);
    REQUIRE(back);
    CHECK(*back == g);
    CHECK(back->layers[0].text.runs[0].style.features == "calt, -liga, ss01");
    CHECK(back->layers[0].text.runs[0].style.opticalKerning);

    // A graphic that sets neither writes neither key (older builds and old projects keep their bytes).
    Graphic plain;
    plain.layers = {persianLayer()};
    plain.layers[0].text.runs[0].style.features.clear();
    const std::string plainJson = io::graphicToJson(plain);
    CHECK(plainJson.find("features") == std::string::npos);
    CHECK(plainJson.find("opticalKerning") == std::string::npos);

    // An unreadable list, or a tag that is not four characters, is refused where it is checked and where it is read.
    Graphic bad = g;
    bad.layers[0].text.runs[0].style.features = "calt, nonsense!";
    CHECK(graphicProblem(bad));
    CHECK_FALSE(io::graphicFromJson(io::graphicToJson(bad)));
    bad.layers[0].text.runs[0].style.features = "liga=100";
    CHECK(graphicProblem(bad));
}

TEST_CASE("The draw list carries the features, and Optical kerning only while kerning is on (card 138)", "[model][graphics][typography]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    Graphic g;
    g.layers = {persianLayer()};
    g.layers[0].text.runs[0].style.opticalKerning = true;
    const DrawList list = resolveGraphic(g, Ticks{});
    REQUIRE(list.items.size() == 1);
    REQUIRE(list.items[0].text.runs.size() == 1);
    const DrawTextRun& run = list.items[0].text.runs[0];
    CHECK(run.features == "calt, -liga, ss01");
    CHECK(run.opticalKerning);
    // Metrics off is "no kerning at all": the optical choice does not apply.
    g.layers[0].text.runs[0].style.kerning = false;
    const DrawList off = resolveGraphic(g, Ticks{});
    CHECK_FALSE(off.items[0].text.runs[0].opticalKerning);
    CHECK_FALSE(off.items[0].text.runs[0].kerning);
    // A change is a different draw list (so the raster is drawn again).
    CHECK_FALSE(list == off);
    Graphic more = g;
    more.layers[0].text.runs[0].style.kerning = true;
    more.layers[0].text.runs[0].style.opticalKerning = true;
    more.layers[0].text.runs[0].style.features = "calt";
    CHECK_FALSE(resolveGraphic(more, Ticks{}) == list);
}

TEST_CASE("A text style keeps its features when it is saved, applied and pushed (card 138)", "[model][graphics][typography]") {
    Timeline tl(R"(
        V1 tgt |..........|
    )");
    Graphic g;
    g.layers = {persianLayer()};
    auto a = ok(edit::addGraphic(tl.state, tl.seq, tl.track("V1"), Ticks{}, f(5), g));
    const ClipId clip = a.placed.front();
    ProjectState s = ok(edit::createTextStyle(a.state, tl.seq, clip, 1, "Persian title")).state;
    REQUIRE(s.textStyles.size() == 1);
    CHECK(s.textStyles[0].style.features == "calt, -liga, ss01");
    const auto text = io::toJson(s);
    REQUIRE(text);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(loaded->textStyles[0].style.features == "calt, -liga, ss01");
}

TEST_CASE("A caption track's OpenType features are saved only when set and refused when unreadable (card 138)",
          "[model][captions][typography]") {
    Timeline tl(R"(
        V1 tgt |..........|
    )");
    auto added = edit::addCaptionTrack(tl.state, tl.seq, "Persian", "fa");
    REQUIRE(added);
    ProjectState s = added->state;
    const CaptionTrackId track = s.findSequence(tl.seq)->captionTracks.back().id;

    const auto plain = io::toJson(s);
    REQUIRE(plain);
    CHECK(plain->find("\"features\"") == std::string::npos); // nothing set: older builds read the project unchanged

    CaptionStyle style;
    style.features = "calt, -liga, ss01";
    s = ok(edit::setCaptionStyle(s, tl.seq, track, style)).state;
    const auto text = io::toJson(s);
    REQUIRE(text);
    CHECK(text->find("\"features\"") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(loaded->findSequence(tl.seq)->captionTracks.back().style.features == "calt, -liga, ss01");
    CHECK(*loaded == s);

    // An unreadable list is refused by the edit.
    style.features = "calt, nonsense!";
    CHECK_FALSE(edit::setCaptionStyle(s, tl.seq, track, style));
}
