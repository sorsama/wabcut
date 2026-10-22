// 0.2: Premiere's Text Styles (project text styles that text layers follow) and caption Track Styles (caption presets
// that caption tracks follow): create, apply, push / update, rename, delete, saved with the project.

#include "TimelineFixture.h"

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

namespace {

GraphicLayer textLayer(std::uint32_t id, std::string text, double size) {
    GraphicLayer l;
    l.id = id;
    l.name = "Text " + std::to_string(id);
    l.kind = LayerKind::Text;
    TextStyle s;
    s.fontSize = size;
    l.text.runs = {TextRun{std::move(text), s}};
    return l;
}

const Graphic& graphicOf(const ProjectState& s, SequenceId seq, ClipId clip) {
    return *s.findSequence(seq)->findClip(clip)->clip().graphic();
}

} // namespace

TEST_CASE("Text styles: create from a layer, apply, push to every follower, rename and delete", "[model][graphics][textStyles]") {
    Timeline tl(R"(
        V2 tgt |..........|
        V1 tgt |..........|
    )");
    Graphic g;
    g.layers = {textLayer(1, "Name", 64.0), textLayer(2, "Title", 40.0)};
    g.layers[0].text.runs[0].style.fill.rgba = {1.0, 0.8, 0.0, 1.0};
    g.layers[0].text.align = TextAlign::Center;
    g.layers[0].shadow.enabled = true;
    auto a = ok(edit::addGraphic(tl.state, tl.seq, tl.track("V1"), Ticks{}, f(5), g));
    const ClipId first = a.placed.front();
    a = ok(edit::addGraphic(a.state, tl.seq, tl.track("V2"), Ticks{}, f(5), g));
    const ClipId second = a.placed.front();
    ProjectState s = a.state;

    // Create Text Style from the first layer: saved, and the layer follows it.
    s = ok(edit::createTextStyle(s, tl.seq, first, 1, "Lower third")).state;
    REQUIRE(s.textStyles.size() == 1);
    CHECK(s.textStyles[0].style.fontSize == 64.0);
    CHECK(s.textStyles[0].align == TextAlign::Center);
    CHECK(s.textStyles[0].shadow.enabled);
    CHECK(graphicOf(s, tl.seq, first).findLayer(1)->textStyle == "Lower third");
    CHECK_FALSE(edit::createTextStyle(s, tl.seq, first, 2, "lower THIRD")); // the name is taken
    // Apply it to the other clip's title layer: restyled and following it.
    s = ok(edit::applyTextStyle(s, tl.seq, second, std::vector<std::uint32_t>{2}, "lower third")).state;
    const GraphicLayer* applied = graphicOf(s, tl.seq, second).findLayer(2);
    CHECK(applied->textStyle == "Lower third");
    CHECK(applied->text.runs[0].style.fontSize == 64.0);
    CHECK(applied->text.runs[0].text == "Title"); // the text stays
    CHECK(applied->text.align == TextAlign::Center);

    // Push to Text Style: both followers change, the others not.
    ProjectTextStyle bigger = s.textStyles[0];
    bigger.style.fontSize = 90.0;
    const auto pushed = ok(edit::saveTextStyle(s, bigger));
    CHECK(pushed.changes.textStylesChanged);
    s = pushed.state;
    CHECK(graphicOf(s, tl.seq, first).findLayer(1)->text.runs[0].style.fontSize == 90.0);
    CHECK(graphicOf(s, tl.seq, second).findLayer(2)->text.runs[0].style.fontSize == 90.0);
    CHECK(graphicOf(s, tl.seq, second).findLayer(1)->text.runs[0].style.fontSize == 64.0);
    requireValid(s);

    // Saved with the project.
    const auto text = io::toJson(s);
    REQUIRE(text);
    auto back = io::fromJson(*text);
    REQUIRE(back);
    CHECK(back->textStyles == s.textStyles);
    CHECK(graphicOf(*back, tl.seq, second).findLayer(2)->textStyle == "Lower third");

    // Rename: followers follow; delete: they keep the look and follow nothing.
    s = ok(edit::renameTextStyle(s, "Lower third", "Name")).state;
    CHECK(graphicOf(s, tl.seq, first).findLayer(1)->textStyle == "Name");
    s = ok(edit::removeTextStyle(s, "name")).state;
    CHECK(s.textStyles.empty());
    CHECK(graphicOf(s, tl.seq, first).findLayer(1)->textStyle.empty());
    CHECK(graphicOf(s, tl.seq, first).findLayer(1)->text.runs[0].style.fontSize == 90.0);
    CHECK_FALSE(edit::applyTextStyle(s, tl.seq, second, std::vector<std::uint32_t>{2}, "Name"));

    // Bad input: names and values are checked; only text layers follow styles.
    ProjectTextStyle bad;
    bad.name = "";
    CHECK(textStyleProblem(bad));
    bad.name = "Huge";
    bad.style.fontSize = 1e9;
    CHECK(textStyleProblem(bad));
    CHECK_FALSE(io::textStyleFromJson(R"({"name": "x", "text": {"runs": [{}, {}]}})"));
    Graphic shapeStyled;
    GraphicLayer box;
    box.id = 1;
    box.kind = LayerKind::Shape;
    box.textStyle = "Name";
    shapeStyled.layers = {box};
    CHECK(graphicProblem(shapeStyled));
}

TEST_CASE("Caption track styles follow their preset", "[model][captions][trackStyles]") {
    Timeline tl(R"(
        V1 tgt |..........|
    )");
    ProjectState s = tl.state;
    const auto addTrack = [&](const char* name) {
        auto r = edit::addCaptionTrack(s, tl.seq, name, "en");
        REQUIRE(r);
        s = r->state;
        return s.findSequence(tl.seq)->captionTracks.back().id;
    };
    const CaptionTrackId english = addTrack("English");
    const CaptionTrackId thai = addTrack("Thai");
    CaptionStyle look;
    look.fontSize = 0.07;
    s = *edit::saveCaptionPreset(s, CaptionPreset{"Channel look", look});
    s = ok(edit::setCaptionTrackStyle(s, tl.seq, english, "channel LOOK")).state;
    s = ok(edit::setCaptionTrackStyle(s, tl.seq, thai, "Channel look")).state;
    const auto track = [&](CaptionTrackId id) -> const CaptionTrack& {
        for (const CaptionTrack& t : s.findSequence(tl.seq)->captionTracks) {
            if (t.id == id) {
                return t;
            }
        }
        FAIL("no track");
        return s.findSequence(tl.seq)->captionTracks.front();
    };
    CHECK(track(english).trackStyle == "Channel look");
    CHECK(track(english).style.fontSize == 0.07);

    // Push to Track Style from the English track: the Thai track follows.
    CaptionStyle bigger = track(english).style;
    bigger.fontSize = 0.09;
    s = ok(edit::setCaptionStyle(s, tl.seq, english, bigger)).state;
    CHECK(track(thai).style.fontSize == 0.07);
    s = *edit::pushCaptionTrackStyle(s, tl.seq, english);
    CHECK(track(thai).style.fontSize == 0.09);
    CHECK(s.findCaptionPreset("Channel look")->style.fontSize == 0.09);
    requireValid(s);

    // Saved with the project.
    auto back = io::fromJson(*io::toJson(s));
    REQUIRE(back);
    CHECK(back->findSequence(tl.seq)->captionTracks[1].trackStyle == "Channel look");

    // A built-in track style cannot be pushed; removing the preset unlinks the tracks.
    s = ok(edit::setCaptionTrackStyle(s, tl.seq, thai, "Bold Box")).state;
    CHECK_FALSE(edit::pushCaptionTrackStyle(s, tl.seq, thai));
    s = *edit::removeCaptionPreset(s, "Channel look");
    CHECK(track(english).trackStyle.empty());
    CHECK(track(thai).trackStyle == "Bold Box");
    CHECK_FALSE(edit::setCaptionTrackStyle(s, tl.seq, thai, "Nothing like it"));
    s = ok(edit::setCaptionTrackStyle(s, tl.seq, thai, "")).state;
    CHECK(track(thai).trackStyle.empty());
}
