// P.2 through the engine: interchange.export's AAF and OMF (with and without the host's audio engine) and the caption
// file commands, captions.import (a new caption track, one undo step) and captions.export (every format, dry runs,
// the host's redaction).

#include "CommandHarness.h"

#include <wabcut/command/AudioPostServices.h>
#include <wabcut/command/CaptionFileServices.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaBbbbCccc........|
    A1 tgt |AaaaBbbbCccc........|
)";

fs::path freshDir(const std::string& tag) {
    const fs::path p = fs::temp_directory_path() / ("wabcut-p2-commands-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

std::string readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeAll(const fs::path& file, std::string_view text) {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

/// Silence for every clip (the AAF / OMF writers only need the right number of frames).
class SilentRenderer final : public interchange::AudioPostRenderer {
public:
    int calls = 0;
    std::expected<void, Error> render(const ProjectState&, const interchange::AudioPostRenderRequest& request,
                                      const std::function<bool(std::span<const float>)>& sink) override {
        ++calls;
        const std::int64_t frames = ticksToSamples(request.range.duration, Rate{request.sampleRate, 1});
        std::vector<float> block(static_cast<std::size_t>(std::max<std::int64_t>(frames, 0) * request.channels), 0.0f);
        if (!block.empty() && !sink(block)) {
            return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
        }
        return {};
    }
};

command::EnginePolicy withAudioPost(std::shared_ptr<SilentRenderer> renderer) {
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::AudioPostServices>(std::move(renderer));
    return policy;
}

/// Gives the harness's sequence a caption track: "Hello" [0,10), "World" [12,20) frames at 25 fps.
void addCaptions(CommandHarness& h) {
    h.ok("captions.addTrack", Json{{"name", "English"}, {"language", "en"}});
    h.ok("captions.addCue", Json{{"start", tc(0)}, {"end", tc(10)}, {"text", "Hello"}});
    h.ok("captions.addCue", Json{{"start", tc(12)}, {"end", tc(20)}, {"text", "World"}});
}

} // namespace

TEST_CASE("interchange.export writes AAF linked to the media without the audio engine (P.2)", "[command][interchange][aaf]") {
    CommandHarness h(kPicture);
    h.tl.linkMatching();
    h.session.reset(h.tl.state);
    const fs::path dir = freshDir("aaf-linked");
    const std::string file = pathToUtf8(dir / "mix.aaf");
    const auto preview = h.run(Json{{"command", "interchange.export"}, {"params", {{"format", "aaf"}, {"path", file}}}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.data["written"] == false);
    CHECK(preview.data["tracks"] == 1);
    CHECK_FALSE(fs::exists(pathFromUtf8(file)));
    const auto written = h.ok("interchange.export", Json{{"format", "aaf"}, {"path", file}});
    CHECK(written.data["written"] == true);
    CHECK(written.data["clips"] == 3);
    CHECK(written.data["audioFiles"].empty());
    bool linkedNote = false;
    for (const Json& e : written.data["report"]["entries"]) {
        linkedNote = linkedNote || e["reason"].get<std::string>().find("Breakout to Mono needs") != std::string::npos;
    }
    CHECK(linkedNote);
    CHECK(readAll(pathFromUtf8(file)).substr(0, 8) == std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)); // MS-CFB
    // OMF always renders its audio: refused without the engine. Settings out of range are refused by the schema.
    CHECK(h.errorOf("interchange.export", Json{{"format", "omf"}, {"path", pathToUtf8(dir / "mix.omf")}}) == "InvalidRequest");
    CHECK(h.errorOf("interchange.export",
                    Json{{"format", "aaf"}, {"path", file}, {"audioPost", Json{{"sampleRate", 44100}}}}) == "InvalidParams");
    CHECK(h.errorOf("interchange.export", Json{{"format", "aaf"}, {"path", file}, {"audioPost", Json{{"handleFrames", 2000}}}}) ==
          "InvalidParams");
    CHECK(h.session.undoCount() == 0);
}

TEST_CASE("interchange.export renders AAF and OMF audio through the host's engine (P.2)", "[command][interchange][aaf][omf]") {
    auto renderer = std::make_shared<SilentRenderer>();
    CommandHarness h(kPicture, withAudioPost(renderer));
    h.tl.linkMatching();
    h.session.reset(h.tl.state);
    const fs::path dir = freshDir("audio-post");
    const auto aaf = h.ok("interchange.export",
                          Json{{"format", "aaf"},
                               {"path", pathToUtf8(dir / "mix.aaf")},
                               {"audioPost", Json{{"sampleRate", 96000}, {"bitsPerSample", 16}, {"files", "separate"},
                                                  {"render", "trim"}, {"handleFrames", 0}}}});
    CHECK(renderer->calls == 3);
    CHECK(aaf.data["audioFiles"].size() == 6); // three stereo clips, broken out to mono
    for (const Json& f : aaf.data["audioFiles"]) {
        const fs::path p = pathFromUtf8(f.get<std::string>());
        CHECK(p.parent_path().filename() == "mix Audio");
        CHECK(fs::exists(p));
    }
    const auto omf = h.ok("interchange.export", Json{{"format", "omf"},
                                                     {"path", pathToUtf8(dir / "mix.omf")},
                                                     {"audioPost", Json{{"omfTitle", "Reel 1"}, {"audioFormat", "aiff"}}}});
    CHECK(omf.data["tracks"] == 2);
    CHECK(omf.data["audioFiles"].empty()); // encapsulated
    const std::string bytes = readAll(dir / "mix.omf");
    REQUIRE(bytes.size() > 24);
    CHECK(bytes.substr(bytes.size() - 24, 8) == std::string("\xA4\x43\x4D\xA5\x48\x64\x72\xD7", 8)); // Bento label
}

TEST_CASE("captions.import reads a caption file into a new caption track, one undo step (P.2)", "[command][captions][p2]") {
    CommandHarness h(kPicture);
    const fs::path dir = freshDir("captions-import");
    const fs::path srt = dir / "Episode 3.srt";
    writeAll(srt, "1\n00:00:00,000 --> 00:00:00,400\n<i>Hello</i>\n\n2\n00:00:00,480 --> 00:00:00,800\nWorld\n\n");
    const auto r = h.ok("captions.import", Json{{"path", pathToUtf8(srt)}});
    CHECK(r.data["format"] == "srt");
    CHECK(r.data["cues"] == 2);
    CHECK(r.data["report"]["approximated"].get<int>() >= 1); // the italics
    const auto& tracks = h.sequence(h.state()).captionTracks;
    REQUIRE(tracks.size() == 1);
    CHECK(tracks.front().name == "Episode 3");
    CHECK(tracks.front().id.value == r.data["captionTrack"].get<std::uint64_t>());
    REQUIRE(tracks.front().cues.size() == 2);
    CHECK(tracks.front().cues[0].text == "Hello");
    CHECK(tracks.front().cues[1].start == framesToTicks(12, kRate));
    CHECK(h.session.undoCount() == 1);
    h.session.undo();
    CHECK(h.sequence(h.state()).captionTracks.empty());

    // A named format and track name; a dry run changes nothing.
    const fs::path vtt = dir / "subs.txt";
    writeAll(vtt, "WEBVTT\n\n00:00.000 --> 00:01.000 line:0\nTop\n");
    const auto preview = h.run(Json{{"command", "captions.import"},
                                    {"params", {{"path", pathToUtf8(vtt)}, {"format", "vtt"}, {"name", "Signs"}}},
                                    {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(h.sequence(h.state()).captionTracks.empty());
    h.ok("captions.import", Json{{"path", pathToUtf8(vtt)}, {"format", "vtt"}, {"name", "Signs"}, {"language", "en"}});
    const CaptionTrack& signs = h.sequence(h.state()).captionTracks.front();
    CHECK(signs.name == "Signs");
    CHECK(signs.language == "en");
    CHECK(signs.style.placement == CaptionPlacement::Top);

    CHECK(h.errorOf("captions.import", Json{{"path", pathToUtf8(dir / "missing.srt")}}) == "NotFound");
    writeAll(dir / "notes.txt", "just some notes\n");
    CHECK(h.errorOf("captions.import", Json{{"path", pathToUtf8(dir / "notes.txt")}}) == "InvalidParams");
    CHECK(h.errorOf("captions.import", Json{{"path", pathToUtf8(srt)}, {"frameRate", "12.5"}}) == "InvalidParams");
}

TEST_CASE("captions.export writes each caption file format and imports back (P.2)", "[command][captions][p2]") {
    CommandHarness h(kPicture);
    addCaptions(h);
    const fs::path dir = freshDir("captions-export");
    for (const std::string format : {"srt", "vtt", "scc", "mcc", "stl", "ttml", "dfxp", "smpte-tt", "itt"}) {
        INFO(format);
        const fs::path file = dir / ("out-" + format + ".cap");
        const auto preview =
            h.run(Json{{"command", "captions.export"}, {"params", {{"path", pathToUtf8(file)}, {"format", format}}}, {"dryRun", true}});
        REQUIRE(preview.ok());
        CHECK(preview.data["written"] == false);
        CHECK_FALSE(fs::exists(file));
        const auto r = h.ok("captions.export", Json{{"path", pathToUtf8(file)}, {"format", format}});
        CHECK(r.data["cues"] == 2);
        REQUIRE(fs::exists(file));
        // Read back into a second track.
        const auto back = h.ok("captions.import", Json{{"path", pathToUtf8(file)}, {"format", format}});
        CHECK(back.data["cues"] == 2);
    }
    // The format from the extension; an unknown one is refused.
    const auto scc = h.ok("captions.export", Json{{"path", pathToUtf8(dir / "show.scc")}, {"display", "roll-up-2"}});
    CHECK(scc.data["format"] == "scc");
    CHECK(readAll(dir / "show.scc").starts_with("Scenarist_SCC V1.0"));
    CHECK(h.errorOf("captions.export", Json{{"path", pathToUtf8(dir / "show.cap")}}) == "InvalidParams");
    CHECK(h.errorOf("captions.export", Json{{"path", pathToUtf8(dir / "x.srt")}, {"range", "inOut"}}) == "InvalidParams");
    CHECK(h.errorOf("captions.export", Json{{"path", pathToUtf8(dir / "x.srt")}, {"captionTrack", 999}}) == "NotFound");
}

TEST_CASE("captions.export replaces words through the host's redaction (P.2)", "[command][captions][p2]") {
    auto services = std::make_shared<command::CaptionFileServices>();
    services->redact = [](const ProjectState&, const Sequence&, const CaptionTrack& track) {
        CaptionTrack out = track;
        for (CaptionCue& c : out.cues) {
            if (c.text == "World") {
                c.text = "[bleep]";
            }
        }
        return out;
    };
    command::EnginePolicy policy;
    policy.services = services;
    CommandHarness h(kPicture, policy);
    addCaptions(h);
    const fs::path dir = freshDir("captions-redact");
    h.ok("captions.export", Json{{"path", pathToUtf8(dir / "out.srt")}});
    const std::string text = readAll(dir / "out.srt");
    CHECK(text.find("[bleep]") != std::string::npos);
    CHECK(text.find("World") == std::string::npos);
}
