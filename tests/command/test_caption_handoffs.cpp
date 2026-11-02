// 0.6 (#57) through the engine: interchange.export reports every caption track with cues as dropped (timeline files and
// AAF / OMF carry none) and, with "captionSidecars", writes each one next to the timeline file - named after the file
// and the track, de-duplicated, with the host's redaction - turning its entry into a carried one; a dry run writes
// nothing. captions.export goes through the same writer.

#include "CommandHarness.h"

#include <wabcut/command/AudioPostServices.h>
#include <wabcut/command/CaptionFileServices.h>
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
    const fs::path p = fs::temp_directory_path() / ("wabcut-p06-captions-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

std::string readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Silence for every clip (the AAF writer only needs the right number of frames).
class SilentRenderer final : public interchange::AudioPostRenderer {
public:
    std::expected<void, Error> render(const ProjectState&, const interchange::AudioPostRenderRequest& request,
                                      const std::function<bool(std::span<const float>)>& sink) override {
        const std::int64_t frames = ticksToSamples(request.range.duration, Rate{request.sampleRate, 1});
        std::vector<float> block(static_cast<std::size_t>(std::max<std::int64_t>(frames, 0) * request.channels), 0.0f);
        if (!block.empty() && !sink(block)) {
            return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
        }
        return {};
    }
};

/// The host's services: "World" is bleeped in caption files; AAF audio is silence.
command::EnginePolicy hostPolicy() {
    auto captions = std::make_shared<command::CaptionFileServices>();
    captions->redact = [](const ProjectState&, const Sequence&, const CaptionTrack& track) {
        CaptionTrack out = track;
        for (CaptionCue& c : out.cues) {
            if (c.text == "World") {
                c.text = "[bleep]";
            }
        }
        return out;
    };
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<command::AudioPostServices>(std::make_shared<SilentRenderer>()), std::move(captions)});
    return policy;
}

/// Four caption tracks: "English" (en, 2 cues), a second "English" (en, 1 cue), "Thai/Sub" (th, 1 cue) and an empty
/// "Spare" track.
void addTracks(CommandHarness& h) {
    const auto first = h.ok("captions.addTrack", Json{{"name", "English"}, {"language", "en"}}).data["captionTrack"];
    h.ok("captions.addCue", Json{{"captionTrack", first}, {"start", tc(0)}, {"end", tc(10)}, {"text", "Hello"}});
    h.ok("captions.addCue", Json{{"captionTrack", first}, {"start", tc(12)}, {"end", tc(20)}, {"text", "World"}});
    const auto second = h.ok("captions.addTrack", Json{{"name", "English"}, {"language", "en"}}).data["captionTrack"];
    h.ok("captions.addCue", Json{{"captionTrack", second}, {"start", tc(0)}, {"end", tc(10)}, {"text", "Hi"}});
    const auto thai = h.ok("captions.addTrack", Json{{"name", "Thai/Sub"}, {"language", "th"}}).data["captionTrack"];
    h.ok("captions.addCue", Json{{"captionTrack", thai}, {"start", tc(0)}, {"end", tc(10)}, {"text", "Sawasdee"}});
    h.ok("captions.addTrack", Json{{"name", "Spare"}});
}

/// The report entries of category "caption".
std::vector<Json> captionEntries(const Json& data) {
    std::vector<Json> out;
    for (const Json& e : data["report"]["entries"]) {
        if (e["category"] == "caption") {
            out.push_back(e);
        }
    }
    return out;
}

} // namespace

TEST_CASE("interchange.export reports caption tracks and writes them as sidecar files when asked (0.6)",
          "[command][interchange][captions][p06]") {
    CommandHarness h(kPicture, hostPolicy());
    h.tl.linkMatching();
    h.session.reset(h.tl.state);
    addTracks(h);
    const std::size_t undo = h.session.undoCount();
    const fs::path dir = freshDir("sidecars");
    const std::string file = pathToUtf8(dir / "cut.xml");

    // Without sidecars: each track with cues is dropped, by name, language and cue count; the empty one is not named.
    const auto plain = h.ok("interchange.export", Json{{"format", "fcp7-xml"}, {"path", file}});
    std::vector<Json> entries = captionEntries(plain.data);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0]["outcome"] == "dropped");
    CHECK(entries[0]["reason"] == "caption track 'English' (en, 2 cues) is not carried by timeline files");
    CHECK(entries[2]["reason"] == "caption track 'Thai/Sub' (th, 1 cue) is not carried by timeline files");
    CHECK(plain.data["captionFiles"].empty());
    const int droppedBefore = plain.data["report"]["dropped"];
    CHECK(droppedBefore >= 3);

    // A dry run with sidecars names the files and writes none.
    const auto preview = h.run(Json{{"command", "interchange.export"},
                                    {"params", {{"format", "fcp7-xml"}, {"path", pathToUtf8(dir / "dry.xml")}, {"captionSidecars", "srt"}}},
                                    {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.data["captionFiles"].size() == 3);
    CHECK_FALSE(fs::exists(dir / "dry - English.en.srt"));

    // With SRT sidecars: three files next to the timeline, named after it and the track; the second English is "(2)".
    const auto r = h.ok("interchange.export", Json{{"format", "fcp7-xml"}, {"path", file}, {"captionSidecars", "srt"}});
    const Json& files = r.data["captionFiles"];
    REQUIRE(files.size() == 3);
    CHECK(files[0] == pathToUtf8(dir / "cut - English.en.srt"));
    CHECK(files[1] == pathToUtf8(dir / "cut - English.en (2).srt"));
    CHECK(files[2] == pathToUtf8(dir / "cut - Thai_Sub.th.srt"));
    for (const Json& f : files) {
        CHECK(fs::is_regular_file(pathFromUtf8(f.get<std::string>())));
    }
    CHECK_FALSE(fs::exists(dir / "cut - Spare.srt"));
    // The words the soundtrack bleeps are replaced, as in captions.export.
    const std::string english = readAll(dir / "cut - English.en.srt");
    CHECK(english.find("Hello") != std::string::npos);
    CHECK(english.find("[bleep]") != std::string::npos);
    CHECK(english.find("World") == std::string::npos);
    // Their entries are carried now, naming the file; the counts move with them.
    entries = captionEntries(r.data);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0]["outcome"] == "carried");
    CHECK(entries[0]["reason"] == "caption track 'English' (en, 2 cues) written to 'cut - English.en.srt'");
    CHECK(entries[1]["reason"].get<std::string>().find("'cut - English.en (2).srt'") != std::string::npos);
    CHECK(r.data["report"]["dropped"] == droppedBefore - 3);

    // Another format (TTML: .xml next to the .xml timeline), and AAF reports and writes them too.
    const auto ttml = h.ok("interchange.export", Json{{"format", "fcpxml"}, {"path", pathToUtf8(dir / "cut.fcpxml")},
                                                      {"captionSidecars", "ttml"}});
    CHECK(ttml.data["captionFiles"][0] == pathToUtf8(dir / "cut - English.en.xml"));
    const auto aaf = h.ok("interchange.export", Json{{"format", "aaf"}, {"path", pathToUtf8(dir / "mix.aaf")}});
    entries = captionEntries(aaf.data);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0]["reason"] == "caption track 'English' (en, 2 cues) is not carried by AAF or OMF files");
    const auto aafWith = h.ok("interchange.export", Json{{"format", "aaf"}, {"path", pathToUtf8(dir / "mix.aaf")},
                                                         {"captionSidecars", "vtt"}});
    CHECK(aafWith.data["captionFiles"].size() == 3);
    CHECK(fs::is_regular_file(dir / "mix - Thai_Sub.th.vtt"));
    CHECK(captionEntries(aafWith.data)[2]["outcome"] == "carried");

    // An unknown format is refused; the project never changed.
    CHECK(h.errorOf("interchange.export", Json{{"format", "edl"}, {"path", file}, {"captionSidecars", "doc"}}) ==
          "InvalidParams");
    CHECK(h.session.undoCount() == undo);
}
