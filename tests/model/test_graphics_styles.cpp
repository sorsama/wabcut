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

// ---- Style part of a text, reorder the styles (0.7, card 81) -------------------------------------------------------------

namespace {

std::string joined(const std::vector<TextRun>& runs) {
    std::string out;
    for (const TextRun& r : runs)
        out += r.text;
    return out;
}

TextStyle sized(double size) {
    TextStyle s;
    s.fontSize = size;
    return s;
}

} // namespace

TEST_CASE("Retexting runs keeps the styles of what did not change (card 81)", "[model][graphics][textStyles][runs]") {
    const std::vector<TextRun> runs{TextRun{"Hello ", sized(40)}, TextRun{"big", sized(90)}, TextRun{" world", sized(40)}};
    // Typing inside the styled word continues its style.
    auto r = edit::retextRuns(runs, "Hello bigger world");
    REQUIRE(r.size() == 3);
    CHECK(r[1].text == "bigger");
    CHECK(r[1].style.fontSize == 90.0);
    CHECK(r[0].style.fontSize == 40.0);
    CHECK(r[2].style.fontSize == 40.0);
    // Deleting the styled word removes its run; the neighbours (one style) join.
    r = edit::retextRuns(runs, "Hello  world");
    REQUIRE(r.size() == 1);
    CHECK(r[0].text == "Hello  world");
    CHECK(r[0].style.fontSize == 40.0);
    // Typing before everything continues the first run; after everything the last.
    r = edit::retextRuns(runs, "Oh Hello big world");
    CHECK(joined(r) == "Oh Hello big world");
    REQUIRE(r.size() == 3);
    CHECK(r.front().style.fontSize == 40.0);
    CHECK(r[1].text == "big");
    CHECK(r[1].style.fontSize == 90.0);
    r = edit::retextRuns(runs, "Hello big world!");
    REQUIRE(r.size() == 3);
    CHECK(r[2].text == " world!");
    CHECK(r.back().style.fontSize == 40.0);
    // An unchanged text is unchanged; a replaced middle takes the style of the run before it.
    CHECK(edit::retextRuns(runs, "Hello big world") == runs);
    r = edit::retextRuns(runs, "Hello large world");
    CHECK(joined(r) == "Hello large world");
    bool largeIsBig = false;
    for (const TextRun& run : r)
        largeIsBig = largeIsBig || (run.text.find("large") != std::string::npos && run.style.fontSize == 40.0);
    CHECK(largeIsBig); // the change starts where the 40 run ends: it continues that run
    // Everything replaced; empty text; one run (the plain case); no runs (a fallback style).
    r = edit::retextRuns(runs, "Completely new");
    CHECK(joined(r) == "Completely new");
    CHECK(joined(edit::retextRuns(runs, "")) == "");
    CHECK(edit::retextRuns(runs, "").size() == 1);
    r = edit::retextRuns({TextRun{"Plain", sized(50)}}, "Plain text");
    REQUIRE(r.size() == 1);
    CHECK(r[0].style.fontSize == 50.0);
    r = edit::retextRuns({}, "From nothing", sized(33));
    REQUIRE(r.size() == 1);
    CHECK(r[0].style.fontSize == 33.0);
    // Multi-byte text is cut on character boundaries.
    const std::vector<TextRun> thai{TextRun{"\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA", sized(40)}, TextRun{"A", sized(80)}};
    r = edit::retextRuns(thai, "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA" "AB");
    CHECK(joined(r) == "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA" "AB");
}

TEST_CASE("Text styles can style part of a text layer (card 81)", "[model][graphics][textStyles][runs]") {
    Timeline tl(R"(
        V1 tgt |..........|
    )");
    Graphic g;
    g.layers = {textLayer(1, "The quick brown fox", 40.0)};
    g.layers[0].text.align = TextAlign::Center;
    auto a = ok(edit::addGraphic(tl.state, tl.seq, tl.track("V1"), Ticks{}, f(5), g));
    const ClipId clip = a.placed.front();
    ProjectState s = a.state;
    ProjectTextStyle highlight;
    highlight.name = "Highlight";
    highlight.style.fontSize = 90.0;
    highlight.style.fill.rgba = {1.0, 0.8, 0.0, 1.0};
    highlight.style.italic = true;
    highlight.align = TextAlign::Right; // layer-level parts of a style are not applied to a range
    s = ok(edit::saveTextStyle(s, highlight)).state;

    // "quick" is characters 4..9.
    const auto styled = ok(edit::applyTextStyleToRange(s, tl.seq, clip, 1, 4, 9, "highlight"));
    const GraphicLayer* layer = graphicOf(styled.state, tl.seq, clip).findLayer(1);
    REQUIRE(layer->text.runs.size() == 3);
    CHECK(layer->text.runs[0].text == "The ");
    CHECK(layer->text.runs[1].text == "quick");
    CHECK(layer->text.runs[1].style.fontSize == 90.0);
    CHECK(layer->text.runs[1].style.italic);
    CHECK(layer->text.runs[1].style.fill.rgba[1] == 0.8);
    CHECK(layer->text.runs[2].text == " brown fox");
    CHECK(layer->text.runs[2].style.fontSize == 40.0);
    CHECK(layer->text.align == TextAlign::Center); // the layer's own alignment stays
    CHECK(layer->textStyle.empty());               // and it follows nothing
    requireValid(styled.state);
    CHECK(layer->text.text() == "The quick brown fox");

    // A second range next to the first joins its run; styling a range already in the style is refused.
    const auto more = ok(edit::applyTextStyleToRange(styled.state, tl.seq, clip, 1, 9, 15, "Highlight"));
    const GraphicLayer* joinedLayer = graphicOf(more.state, tl.seq, clip).findLayer(1);
    REQUIRE(joinedLayer->text.runs.size() == 3);
    CHECK(joinedLayer->text.runs[1].text == "quick brown");
    CHECK_FALSE(edit::applyTextStyleToRange(more.state, tl.seq, clip, 1, 4, 9, "Highlight"));
    // Refusals: no such style, layer or range; a range past the text; not a text layer.
    CHECK_FALSE(edit::applyTextStyleToRange(s, tl.seq, clip, 1, 4, 9, "Missing"));
    CHECK_FALSE(edit::applyTextStyleToRange(s, tl.seq, clip, 7, 4, 9, "Highlight"));
    CHECK_FALSE(edit::applyTextStyleToRange(s, tl.seq, clip, 1, 9, 9, "Highlight"));
    CHECK_FALSE(edit::applyTextStyleToRange(s, tl.seq, clip, 1, 9, 4, "Highlight"));
    CHECK_FALSE(edit::applyTextStyleToRange(s, tl.seq, clip, 1, 4, 200, "Highlight"));

    // Editing the text afterwards keeps the styled word styled (what the on-monitor editor now does on commit).
    GraphicLayer edited = *layer;
    edited.text.runs = edit::retextRuns(edited.text.runs, "The quick brown fox jumps");
    CHECK(edited.text.runs[1].text == "quick");
    CHECK(edited.text.runs[1].style.fontSize == 90.0);
    CHECK(edited.text.text() == "The quick brown fox jumps");

    // Saved with the project and read back.
    const auto json = io::toJson(styled.state);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(graphicOf(*loaded, tl.seq, clip).findLayer(1)->text.runs.size() == 3);
}

TEST_CASE("Text styles can be reordered (card 81)", "[model][graphics][textStyles]") {
    Timeline tl(R"(
        V1 tgt |..........|
    )");
    ProjectState s = tl.state;
    for (const char* name : {"First", "Second", "Third", "Fourth"}) {
        ProjectTextStyle style;
        style.name = name;
        s = ok(edit::saveTextStyle(s, style)).state;
    }
    const auto order = [](const ProjectState& state) {
        std::string out;
        for (const ProjectTextStyle& st : state.textStyles)
            out += st.name + ",";
        return out;
    };
    CHECK(order(s) == "First,Second,Third,Fourth,");
    const auto moved = ok(edit::moveTextStyle(s, "Fourth", 1));
    CHECK(order(moved.state) == "First,Fourth,Second,Third,");
    CHECK(moved.changes.textStylesChanged);
    CHECK(order(ok(edit::moveTextStyle(s, "first", 99)).state) == "Second,Third,Fourth,First,"); // past the end = last, names are case-blind
    CHECK(order(ok(edit::moveTextStyle(s, "Third", 2)).state) == order(s)); // already there
    CHECK_FALSE(edit::moveTextStyle(s, "Missing", 0));
    requireValid(moved.state);
    // The order is saved with the project.
    const auto json = io::toJson(moved.state);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(order(*loaded) == "First,Fourth,Second,Third,");
}
