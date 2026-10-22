// 0.2 Essential Graphics parity through the command protocol: graphic.setRoll (Roll / Crawl) and graphic.align (Align
// and Distribute).

#include "CommandHarness.h"

#include <wabcut/interchange/Zip.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr std::string_view kPicture = R"(
    V2 tgt |..................................................|
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

Json box(int id, double x, double y) {
    return Json{{"id", id}, {"kind", "shape"}, {"name", "Box " + std::to_string(id)},
                {"shape", Json{{"type", "rectangle"}, {"width", 200}, {"height", 100}}},
                {"transform", Json{{"x", x}, {"y", y}}}};
}

const Graphic& graphicIn(const CommandHarness& h, std::uint64_t clip) {
    const Graphic* g = h.sequence(h.state()).findClip(ClipId{clip})->clip().graphic();
    REQUIRE(g);
    return *g;
}

} // namespace

TEST_CASE("graphic.setRoll makes credits roll and graphic.align lines layers up", "[command][graphics][roll][align]") {
    CommandHarness h(kPicture);
    const auto added = h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(50)}, {"track", "V2"},
                                                {"graphic", Json{{"layers", Json::array({box(1, 300, 200), box(2, 900, 700), box(3, 1600, 400)})}}}});
    const std::uint64_t clip = added.data["clip"].get<std::uint64_t>();

    h.ok("graphic.setRoll", Json{{"clip", clip}, {"roll", Json{{"kind", "crawlLeft"}, {"postroll", tc(5)}, {"easeIn", tc(10)}}}});
    REQUIRE(graphicIn(h, clip).roll);
    CHECK(graphicIn(h, clip).roll->kind == RollKind::CrawlLeft);
    CHECK(graphicIn(h, clip).roll->postroll == f(5));
    CHECK(graphicIn(h, clip).roll->easeIn == f(10));
    CHECK(h.session.undoLabel() == "Roll/Crawl Options");
    // At the clip's start the content is just past the frame's right edge.
    const Clip& c = h.sequence(h.state()).findClip(ClipId{clip})->clip();
    const Box start = contentBox(resolveGraphicClip(*h.state(), c, c.start()));
    CHECK_THAT(start.x, WithinAbs(1920.0, 1e-6));
    CHECK(h.errorOf("graphic.setRoll", Json{{"clip", clip}, {"roll", Json{{"kind", "sideways"}}}}) == "InvalidParams");
    h.ok("graphic.setRoll", Json{{"clip", clip}, {"roll", nullptr}});
    CHECK_FALSE(graphicIn(h, clip).roll);

    // Align to the video frame, then to the selection; distribute three.
    h.ok("graphic.align", Json{{"clip", clip}, {"layers", Json::array({1, 2})}, {"align", "top"}});
    CHECK_THAT(graphicIn(h, clip).findLayer(1)->transform.y.constant(), WithinAbs(50.0, 1e-9));
    CHECK_THAT(graphicIn(h, clip).findLayer(2)->transform.y.constant(), WithinAbs(50.0, 1e-9));
    h.ok("graphic.align", Json{{"clip", clip}, {"layers", Json::array({1, 2, 3})}, {"align", "bottom"}, {"to", "selection"}});
    CHECK_THAT(graphicIn(h, clip).findLayer(1)->transform.y.constant(), WithinAbs(400.0, 1e-9));
    h.ok("graphic.align", Json{{"clip", clip}, {"layers", Json::array({1, 2, 3})}, {"align", "distributeHorizontal"}});
    CHECK_THAT(graphicIn(h, clip).findLayer(2)->transform.x.constant(), WithinAbs(950.0, 1e-9));
    CHECK(h.errorOf("graphic.align", Json{{"clip", clip}, {"layers", Json::array({1, 2})}, {"align", "distributeVertical"}}) ==
          "EditRejected");
    CHECK(h.errorOf("graphic.align", Json{{"clip", clip}, {"layers", Json::array({1})}, {"align", "left"}, {"time", tc(60)}}) ==
          "InvalidParams");
}

TEST_CASE("template.import reads Premiere's .mogrt and Wabcut template files; template.export writes them", "[command][graphics][templates][mogrt]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "graphics-parity" / "templates";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto write = [](const fs::path& file, const std::string& bytes) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << bytes;
    };
    const Json definition{{"authorApp", "ppro"},
                          {"capsuleNameLocalized", {{"strDB", Json::array({{{"localeString", "en_US"}, {"str", "Speaker"}}})}}},
                          {"clientControls", Json::array({{{"type", 6}, {"uiName", "Name"}, {"value", "Somchai"}},
                                                          {{"type", 2}, {"uiName", "Accent"}, {"value", Json::array({1, 0, 0, 1})}}})}};
    const std::vector<std::pair<std::string, std::string>> files{{"definition.json", definition.dump()}, {"project.prgraphic", "x"}};
    write(dir / "speaker.mogrt", interchange::writeStoredZip(files));
    const std::vector<std::pair<std::string, std::string>> aeFiles{{"definition.json", Json{{"authorApp", "aefx"}}.dump()},
                                                                   {"project.aegraphic", "x"}};
    write(dir / "ae.mogrt", interchange::writeStoredZip(aeFiles));

    CommandHarness h(kPicture);
    const auto imported = h.ok("template.import", Json{{"path", pathToUtf8(dir / "speaker.mogrt")}});
    const std::string id = imported.data["template"];
    CHECK(imported.data["library"] == "project");
    CHECK(imported.data["fields"].size() == 2);
    CHECK(imported.data["report"]["approximated"].get<int>() >= 1);
    REQUIRE(h.state()->findTemplate(id));
    CHECK(h.errorOf("template.import", Json{{"path", pathToUtf8(dir / "ae.mogrt")}}) == "EditRejected");
    CHECK(h.errorOf("template.import", Json{{"path", pathToUtf8(dir / "missing.mogrt")}}) == "NotFound");

    // Export it, import it again under another id: the same template.
    const fs::path out = dir / "speaker.wabcut-template.json";
    h.ok("template.export", Json{{"template", id}, {"path", pathToUtf8(out)}});
    CHECK(h.errorOf("template.export", Json{{"template", id}, {"path", pathToUtf8(out)}}) == "EditRejected");
    h.ok("template.import", Json{{"path", pathToUtf8(out)}, {"id", "speaker.copy"}});
    const GraphicTemplate* copy = h.state()->findTemplate("speaker.copy");
    REQUIRE(copy);
    CHECK(copy->graphic == h.state()->findTemplate(id)->graphic);
    CHECK(copy->fields == h.state()->findTemplate(id)->fields);
    // A built-in exports too.
    h.ok("template.export", Json{{"template", "wabcut.gaming.lowerThird"}, {"path", pathToUtf8(dir / "gaming.wabcut-template.json")}});
    CHECK(fs::exists(dir / "gaming.wabcut-template.json"));
}

TEST_CASE("Text styles and caption track styles through commands", "[command][graphics][textStyles][captions]") {
    CommandHarness h(kPicture);
    const Json text{{"id", 1}, {"kind", "text"}, {"name", "Name"},
                    {"text", Json{{"runs", Json::array({Json{{"text", "Somchai"}, {"size", 80}}})}}}};
    const auto a = h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(10)}, {"track", "V2"}, {"graphic", Json{{"layers", Json::array({text})}}}});
    const std::uint64_t first = a.data["clip"].get<std::uint64_t>();
    h.ok("textStyle.create", Json{{"clip", first}, {"layer", 1}, {"name", "Big"}});
    CHECK(h.ok("textStyle.list").data["textStyles"][0]["layers"] == 1);
    // Push a new look from JSON: the layer follows.
    Json look = h.ok("textStyle.list").data["textStyles"][0];
    look.erase("name");
    look.erase("layers");
    look["text"]["runs"][0]["size"] = 120;
    h.ok("textStyle.update", Json{{"name", "Big"}, {"style", look}});
    CHECK(graphicIn(h, first).findLayer(1)->text.runs[0].style.fontSize == 120.0);
    h.ok("textStyle.rename", Json{{"name", "Big"}, {"to", "Huge"}});
    CHECK(graphicIn(h, first).findLayer(1)->textStyle == "Huge");
    CHECK(h.errorOf("textStyle.apply", Json{{"clip", first}, {"layers", Json::array({1})}, {"name", "Nope"}}) == "NotFound");
    h.ok("textStyle.remove", Json{{"name", "Huge"}});
    CHECK(graphicIn(h, first).findLayer(1)->textStyle.empty());

    // Captions: a track follows a preset; saving the preset again restyles it.
    h.ok("captions.addTrack", Json{{"name", "English"}, {"language", "en"}});
    const std::uint64_t track = h.sequence(h.state()).captionTracks.back().id.value;
    h.ok("captions.savePreset", Json{{"name", "Channel"}, {"style", Json{{"fontSize", 0.05}}}});
    h.ok("captions.setTrackStyle", Json{{"captionTrack", track}, {"style", "channel"}});
    CHECK(h.sequence(h.state()).captionTracks.back().trackStyle == "Channel");
    h.ok("captions.savePreset", Json{{"name", "Channel"}, {"style", Json{{"fontSize", 0.08}}}});
    CHECK(h.sequence(h.state()).captionTracks.back().style.fontSize == 0.08);
    h.ok("captions.setStyle", Json{{"captionTrack", track}, {"style", Json{{"fontSize", 0.06}}}});
    h.ok("captions.pushTrackStyle", Json{{"captionTrack", track}});
    CHECK(h.state()->findCaptionPreset("Channel")->style.fontSize == 0.06);
    CHECK(h.ok("captions.get", Json{{"captionTrack", track}}).data["captionTrack"]["trackStyle"] == "Channel");
}
TEST_CASE("captions.findReplace replaces in every caption track in one undo step", "[command][captions][findReplace]") {
    CommandHarness h(kPicture);
    const auto a = h.ok("captions.addTrack", Json{{"name", "English"}});
    const std::uint64_t first = a.data["captionTrack"];
    h.ok("captions.addCue", Json{{"captionTrack", first}, {"start", "00:00:00:00"}, {"end", "00:00:01:00"}, {"text", "The colour of Colour"}});
    h.ok("captions.addCue", Json{{"captionTrack", first}, {"start", "00:00:01:00"}, {"end", "00:00:02:00"}, {"text", "colourful"}});
    const auto b = h.ok("captions.addTrack", Json{{"name", "Other"}});
    const std::uint64_t second = b.data["captionTrack"];
    h.ok("captions.addCue", Json{{"captionTrack", second}, {"start", "00:00:00:00"}, {"end", "00:00:01:00"}, {"text", "colour"}});
    const auto cueText = [&](std::uint64_t track, std::size_t cue) {
        for (const CaptionTrack& t : h.sequence(h.state()).captionTracks) {
            if (t.id.value == track) {
                return t.cues.at(cue).text;
            }
        }
        return std::string();
    };

    // A dry run changes nothing.
    REQUIRE(h.run(Json{{"command", "captions.findReplace"}, {"params", Json{{"find", "colour"}, {"replace", "color"}}}, {"dryRun", true}}).ok());
    CHECK(cueText(first, 0) == "The colour of Colour");

    const std::size_t before = h.session.undoCount();
    // Whole word and match case.
    const auto r = h.ok("captions.findReplace", Json{{"find", "colour"}, {"replace", "color"}, {"wholeWord", true}, {"matchCase", true}});
    CHECK(r.data["replaced"] == 2);
    CHECK(h.session.undoCount() == before + 1);
    CHECK(cueText(first, 0) == "The color of Colour");
    CHECK(cueText(first, 1) == "colourful");
    CHECK(cueText(second, 0) == "color");
    // Neither: every spelling, inside words too; one track only.
    const auto all = h.ok("captions.findReplace", Json{{"captionTrack", first}, {"find", "COLOUR"}, {"replace", "hue"}});
    CHECK(all.data["replaced"] == 2);
    CHECK(cueText(first, 0) == "The color of hue");
    CHECK(cueText(first, 1) == "hueful");
    // Not found: a warning, nothing changes; emptying a cue is refused.
    CHECK(h.ok("captions.findReplace", Json{{"find", "zebra"}, {"replace", "x"}}).warnings.size() == 1);
    CHECK(h.errorOf("captions.findReplace", Json{{"captionTrack", second}, {"find", "color"}, {"replace", ""}}) == "EditRejected");
    CHECK(cueText(second, 0) == "color");
}

TEST_CASE("captions.import reads SRT and WebVTT files and timed transcript text", "[command][captions][import]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "graphics-parity" / "captions";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto write = [](const fs::path& file, const std::string& bytes) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << bytes;
    };
    write(dir / "talk.srt", "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i> there\r\nsecond line\r\n\r\n"
                            "2\r\n00:00:02,000 --> 00:00:04,000\r\nOverlaps the first\r\n\r\n3\r\nbroken --> line\r\nskipped\r\n");
    write(dir / "talk.vtt", "WEBVTT\n\nNOTE a comment\n\nintro\n00:01.000 --> 00:02.000 align:start\n<v Somchai>Sawasdee</v>\n");
    write(dir / "talk.txt", "00:00:00:00 - 00:00:01:00\nHost\nWelcome back.\n\n00:00:01:00 - 00:00:02:00\nNo speaker line.\n");
    write(dir / "empty.srt", "nothing here\n");

    CommandHarness h(kPicture);
    const auto srt = h.ok("captions.import", Json{{"path", pathToUtf8(dir / "talk.srt")}, {"language", "en"}});
    CHECK(srt.data["cues"] == 2);
    // One captions.import (p02 integration): the SubRip reader may already end the first cue at the next one.
    CHECK(srt.data["trimmed"].get<int>() <= 1);
    const std::uint64_t srtTrack = srt.data["captionTrack"];
    const CaptionTrack* track = nullptr;
    for (const CaptionTrack& t : h.sequence(h.state()).captionTracks) {
        if (t.id.value == srtTrack) {
            track = &t;
        }
    }
    REQUIRE(track);
    CHECK(track->name == "talk");
    CHECK(track->language == "en");
    REQUIRE(track->cues.size() == 2);
    CHECK(track->cues[0].text == "Hello there\nsecond line");
    CHECK(track->cues[0].start == Ticks{kTicksPerSecond});
    CHECK(track->cues[0].end() == Ticks{2 * kTicksPerSecond}); // trimmed to the next cue
    CHECK(track->cues[1].end() == Ticks{4 * kTicksPerSecond});

    const auto vtt = h.ok("captions.import", Json{{"path", pathToUtf8(dir / "talk.vtt")}, {"offset", "00:00:10:00"}});
    CHECK(vtt.data["cues"] == 1);

    h.ok("transcript.addSpeaker", Json{{"name", "Host"}});
    const auto txt = h.ok("captions.import", Json{{"path", pathToUtf8(dir / "talk.txt")}, {"name", "Transcript"}});
    CHECK(txt.data["cues"] == 2);
    CHECK(txt.data["cuesWithSpeaker"] == 1);

    CHECK(h.errorOf("captions.import", Json{{"path", pathToUtf8(dir / "empty.srt")}}) == "InvalidParams");
    CHECK(h.errorOf("captions.import", Json{{"path", pathToUtf8(dir / "talk.doc")}}) == "InvalidParams");
    CHECK(h.errorOf("captions.import", Json{{"path", pathToUtf8(dir / "missing.srt")}}) == "NotFound");
}
