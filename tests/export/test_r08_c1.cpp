// 0.7, stream C, batch C1: the pixel aspect ratio of the sequence in exports (145), the export-path fixes - preset ids of
// names that share ASCII text (147), an output name with dots (148), the target-size audio estimate (149), the second
// pass of a target-size export (150), UTF-8 safe truncation of chapter titles and subtitle cues (151), the OP-Atom
// file set moved into place as one unit (152). Most are pure (resolve(), the preset store, helpers); the file checks
// export the synthetic pattern (no media files needed) and read the result back with ffprobe.

#include "ExportTestUtil.h"
#include "MetadataWriter.h"
#include "MxfDelivery.h"
#include "Utf8.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/Registry.h>
#include <wabcut/export/ExportCommandServices.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h> // WIN32_LEAN_AND_MEAN and NOMINMAX come from the build
#endif

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

namespace fs = std::filesystem;


ExportSettings preset(std::string_view id) {
    const auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

Sequence makeSequence(int width, int height, Rate rate, Ticks duration, Rate pixelAspect = Rate{1, 1}) {
    Sequence s;
    s.width = width;
    s.height = height;
    s.frameRate = rate;
    s.duration = duration;
    s.pixelAspect = pixelAspect;
    return s;
}

std::string readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeAll(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Well-formed UTF-8 (no stray continuation or truncated lead bytes).
bool validUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += n;
    }
    return true;
}

std::string repeated(const std::string& unit, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) {
        out += unit;
    }
    return out;
}

} // namespace

// ---- 145: the sequence's pixel aspect ratio in exports ---------------------------------------------------------------

TEST_CASE("resolve(): an output of the canvas shape carries the sequence pixel aspect ratio", "[export][settings][par]") {
    const Sequence hdv = makeSequence(1440, 1080, kRate25, Ticks::fromSeconds(2), Rate{4, 3});
    ExportSettings s = preset("match-source");
    s.video.width.reset();
    s.video.height.reset();

    // The default size: the canvas as it is, tagged 4:3 pixels.
    auto r = resolve(s, hdv);
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect == Rate{4, 3});
    CHECK(r->width == 1440);
    CHECK(r->height == 1080);
    CHECK(r->renderWidth == 1440);
    CHECK(r->renderHeight == 1080);

    // A pure scale of the canvas (a half-size proxy) keeps the ratio.
    s.video.width = 720;
    s.video.height = 540;
    r = resolve(s, hdv);
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect == Rate{4, 3});
    CHECK(r->renderWidth == 720);
    CHECK(r->renderHeight == 540);

    // Another shape (1920x1080): square pixels, and the canvas is resampled to the 16:9 it is seen as - Fit fills the frame.
    s.video.width = 1920;
    s.video.height = 1080;
    r = resolve(s, hdv);
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect == Rate{1, 1});
    CHECK(r->renderWidth == 1920);
    CHECK(r->renderHeight == 1080);

    // A 4:3 frame of square pixels: the 16:9 picture is letterboxed (1440 x 810), not squeezed.
    s.video.width = 1440;
    s.video.height = 1080;
    s.video.scaleMode = ScaleMode::Fit;
    r = resolve(s, makeSequence(1920, 1080, kRate25, Ticks::fromSeconds(2), Rate{4, 3})); // 2560x1080 as seen
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect == Rate{1, 1});
    CHECK(r->renderWidth == 1440);
    CHECK(r->renderHeight == 608); // 1440 * 1080 / 2560 = 607.5 -> 608 (even)
}

TEST_CASE("resolve(): Fit and Fill use the seen shape of an anamorphic canvas", "[export][settings][par]") {
    // DV widescreen: 720x480 with 40:33 pixels is seen as 1.818:1.
    const Sequence dv = makeSequence(720, 480, kRate30, Ticks::fromSeconds(2), Rate{40, 33});
    ExportSettings s = preset("match-source");
    s.video.width.reset();
    s.video.height.reset();
    auto r = resolve(s, dv);
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect == Rate{40, 33});
    CHECK(r->width == 720);
    CHECK(r->height == 480);

    s.video.width = 1280;
    s.video.height = 720;
    r = resolve(s, dv); // square pixels: 1.818:1 into 16:9 is a little narrower than the frame height allows
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect == Rate{1, 1});
    CHECK(r->renderWidth == 1280);
    CHECK(r->renderHeight == 704);

    // Fill crops the seen shape: 1440x1080 with 4:3 pixels (16:9) into a 1080x1920 vertical frame.
    const Sequence hdv = makeSequence(1440, 1080, kRate25, Ticks::fromSeconds(2), Rate{4, 3});
    s.video.width = 1080;
    s.video.height = 1920;
    s.video.scaleMode = ScaleMode::Fill;
    r = resolve(s, hdv);
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect == Rate{1, 1});
    CHECK(r->renderHeight == 1920);
    CHECK(r->renderWidth == 3414); // 1920 * 16/9 = 3413.3

    // The same crop of a square 1920x1080 canvas gives the same numbers: only the seen shape counts.
    const auto square = resolve(s, makeSequence(1920, 1080, kRate25, Ticks::fromSeconds(2)));
    REQUIRE(square.has_value());
    CHECK(square->sampleAspect == Rate{1, 1});
    CHECK(square->renderWidth == r->renderWidth);
    CHECK(square->renderHeight == r->renderHeight);
}

TEST_CASE("resolve(): square sequences and damaged pixel aspect ratios write square pixels", "[export][settings][par]") {
    ExportSettings s = preset("match-source");
    s.video.width.reset();
    s.video.height.reset();
    auto r = resolve(s, makeSequence(1920, 1080, kRate30, Ticks::fromSeconds(1)));
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect == Rate{1, 1});
    // A project file can hold any ratio; outside what Sequence Settings accepts (0.1 to 10) it is not a shape.
    for (const Rate bad : {Rate{1, 100}, Rate{100, 1}, Rate{0, 1}, Rate{-4, 3}}) {
        r = resolve(s, makeSequence(1920, 1080, kRate30, Ticks::fromSeconds(1), bad));
        REQUIRE(r.has_value());
        CHECK(r->sampleAspect == Rate{1, 1});
        CHECK(r->renderWidth == 1920);
    }
    // An unreduced ratio comes out reduced.
    r = resolve(s, makeSequence(1440, 1080, kRate30, Ticks::fromSeconds(1), Rate{8, 6}));
    REQUIRE(r.has_value());
    CHECK(r->sampleAspect.num == 4);
    CHECK(r->sampleAspect.den == 3);
}

TEST_CASE("An export carries the sequence pixel aspect ratio in MP4 MOV and MKV", "[export][par][integration]") {
    TestProject hdv(480, 360, kRate30, Ticks::fromSeconds(1)); // seen as 640x360 (16:9) with 4:3 pixels
    hdv.editSequence([](Sequence& s) { s.pixelAspect = Rate{4, 3}; });
    const auto dir = freshOutputDir("par");
    struct Case {
        Container container;
        const char* file;
    };
    for (const Case& c : {Case{Container::Mp4, "hdv.mp4"}, Case{Container::Mov, "hdv.mov"}, Case{Container::Mkv, "hdv.mkv"}}) {
        INFO(c.file);
        ExportSettings s = preset("match-source");
        s.container = c.container;
        s.video.quality = 30;
        s.audio.enabled = false;
        s.video.width.reset();
        s.video.height.reset();
        const auto result = ExportJob(makeRequest(hdv, s, dir / c.file)).run();
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
        std::string error;
        const auto text = ffprobeText(dir / c.file, "-select_streams v:0 -show_streams", &error);
        INFO(error);
        REQUIRE(text.has_value());
        CHECK(text->find("width=480") != std::string::npos);
        CHECK(text->find("height=360") != std::string::npos);
        CHECK(text->find("sample_aspect_ratio=4:3") != std::string::npos);
        CHECK(text->find("display_aspect_ratio=16:9") != std::string::npos);
    }

    // A square sequence still says 1:1 (or nothing), and an output of another shape is square pixels.
    TestProject square(480, 360, kRate30, Ticks::fromSeconds(1));
    ExportSettings s = preset("match-source");
    s.video.quality = 30;
    s.audio.enabled = false;
    s.video.width.reset();
    s.video.height.reset();
    REQUIRE(ExportJob(makeRequest(square, s, dir / "square.mp4")).run().has_value());
    auto text = ffprobeText(dir / "square.mp4", "-select_streams v:0 -show_streams");
    REQUIRE(text.has_value());
    CHECK(text->find("sample_aspect_ratio=4:3") == std::string::npos);
    CHECK(text->find("display_aspect_ratio=4:3") != std::string::npos);

    s.video.width = 640; // the HDV-like canvas into 640x360: square pixels, the 16:9 picture fills the frame
    s.video.height = 360;
    REQUIRE(ExportJob(makeRequest(hdv, s, dir / "reshaped.mp4")).run().has_value());
    text = ffprobeText(dir / "reshaped.mp4", "-select_streams v:0 -show_streams");
    REQUIRE(text.has_value());
    CHECK(text->find("width=640") != std::string::npos);
    CHECK(text->find("sample_aspect_ratio=4:3") == std::string::npos);
    CHECK(text->find("display_aspect_ratio=16:9") != std::string::npos);
}

// ---- 147: preset ids of names that share ASCII text ------------------------------------------------------------------

TEST_CASE("Presets with names that share ASCII text keep files of their own", "[export][presets]") {
    const auto dir = freshOutputDir("preset-ids");
    PresetStore store(dir);
    const ExportSettings settings = preset("match-source");

    const std::string first = "\xE0\xB8\xA2\xE0\xB8\xB9\xE0\xB8\x97\xE0\xB8\xB9\xE0\xB8\x9A 4K";  // Thai "YouTube" + " 4K"
    const std::string second = "\xE0\xB9\x84\xE0\xB8\xA5\xE0\xB8\x9F\xE0\xB9\x8C 4K";              // Thai "live" + " 4K"
    const auto a = store.save(first, "", settings, false);
    REQUIRE(a.has_value());
    CHECK(a->id == "user-4k"); // the id earlier versions gave it: nothing to migrate
    // The second name is not "exists": it gets an id of its own and the first preset is untouched.
    const auto b = store.save(second, "", settings, false);
    INFO((b ? std::string() : b.error().message));
    REQUIRE(b.has_value());
    CHECK(b->id != a->id);
    CHECK(b->id.starts_with("user-4k-"));
    CHECK(store.list().size() == 2);
    REQUIRE(store.find(a->id).has_value());
    CHECK(store.find(a->id)->name == first);
    REQUIRE(store.find(b->id).has_value());
    CHECK(store.find(b->id)->name == second);

    // Saving a name again is the same preset: refused without overwrite, replaced with it - and only that one.
    const auto again = store.save(second, "", settings, false);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().message.find(second) != std::string::npos);
    const auto replaced = store.save(second, "changed", settings, true);
    REQUIRE(replaced.has_value());
    CHECK(replaced->id == b->id);
    CHECK(store.list().size() == 2);
    CHECK(store.find(a->id)->description.empty());
    CHECK(store.find(b->id)->description == "changed");

    // ASCII names that differ in punctuation or case, and a name that comes after the hashed one.
    const auto y1 = store.save("YouTube 4K", "", settings, false);
    const auto y2 = store.save("youtube-4k", "", settings, false);
    const auto p1 = store.save("Preset 1", "", settings, false);
    const auto p2 = store.save("Preset-1", "", settings, false);
    REQUIRE(y1.has_value());
    REQUIRE(y2.has_value());
    REQUIRE(p1.has_value());
    REQUIRE(p2.has_value());
    CHECK(y1->id == "user-youtube-4k");
    CHECK(y2->id != y1->id);
    CHECK(p1->id == "user-preset-1");
    CHECK(p2->id != p1->id);
    CHECK(store.list().size() == 6);
    // Every id is valid for the store: deletable, found again, and a long name does not overflow the id limit.
    const std::string longName = repeated("abcdefghij", 10) + " one";
    const auto l1 = store.save(longName, "", settings, false);
    const auto l2 = store.save(repeated("abcdefghij", 10) + " two", "", settings, false);
    REQUIRE(l1.has_value());
    REQUIRE(l2.has_value());
    CHECK(l1->id != l2->id);
    CHECK(l2->id.size() <= 69);
    CHECK(store.remove(l2->id).has_value());
    CHECK(store.find(l1->id).has_value());
    // idFor() is what a save would use (the dry run of export.savePreset shows it).
    CHECK(store.idFor("Brand new name") == "user-brand-new-name");
    CHECK(store.idFor("youtube-4k") == y2->id);

    // Import of a shared file collides the same way, and the second preset survives.
    const fs::path shared = freshOutputDir("preset-ids-shared") / "shared.json";
    REQUIRE(store.exportFile(a->id, shared).has_value());
    PresetStore other(freshOutputDir("preset-ids-other"));
    REQUIRE(other.save(second, "", settings, false).has_value()); // takes user-4k here
    const auto imported = other.importFile(shared, false);
    INFO((imported ? std::string() : imported.error().message));
    REQUIRE(imported.has_value());
    CHECK(other.list().size() == 2);
}

// ---- 148: an output name with dots and no extension ------------------------------------------------------------------

TEST_CASE("The format extension is swapped only for a known extension and appended to a name with dots", "[export][paths]") {
    CHECK(withOutputExtension("D:/Videos/Stream 2026.11.03 Highlights", ".mp4") == fs::path("D:/Videos/Stream 2026.11.03 Highlights.mp4"));
    CHECK(withOutputExtension("cut v1.2", ".mp4") == fs::path("cut v1.2.mp4"));
    CHECK(withOutputExtension("plain", ".mkv") == fs::path("plain.mkv"));
    CHECK(withOutputExtension("clip.mov", ".mp4") == fs::path("clip.mp4"));
    CHECK(withOutputExtension("Clip.MOV", ".mp4") == fs::path("Clip.mp4"));
    CHECK(withOutputExtension("a.b.c.mkv", ".wav") == fs::path("a.b.c.wav"));
    CHECK(withOutputExtension("still.jpeg", ".png") == fs::path("still.png"));
    CHECK(withOutputExtension("same.mp4", ".mp4") == fs::path("same.mp4"));
    CHECK(withOutputExtension("", ".mp4").empty());
    CHECK(isMediaFileExtension(".MP4"));
    CHECK(isMediaFileExtension(".aif"));
    CHECK_FALSE(isMediaFileExtension(".03 Highlights"));
    CHECK_FALSE(isMediaFileExtension(".2"));
    CHECK_FALSE(isMediaFileExtension(""));
    CHECK(isPlausibleExtension(".xyz"));
    CHECK(isPlausibleExtension(".m4a"));
    CHECK_FALSE(isPlausibleExtension(".03 Highlights"));
    CHECK_FALSE(isPlausibleExtension(".2"));
    CHECK_FALSE(isPlausibleExtension("."));
    CHECK_FALSE(isPlausibleExtension("mp4"));
}

TEST_CASE("export.quick keeps the tail of a typed name that has dots and no extension", "[export][paths]") {
    TestProject project(640, 360, kRate30, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("dotted-name");
    auto services = ExportCommandServices::standalone(dir / "project.wabcut", dir / "presets", syntheticVideoSourceFactory());
    EditSession session(project.state);
    command::EnginePolicy policy;
    policy.services = services;
    command::CommandEngine engine(session, command::builtinCommands(), policy);
    const auto dry = [&](const fs::path& output) {
        command::CommandRequest request;
        request.command = "export.quick";
        request.params = {{"preset", "match-source"}, {"output", pathToUtf8(output)}};
        request.dryRun = true;
        return engine.execute(request);
    };
    auto r = dry(dir / "Stream 2026.11.03 Highlights");
    INFO(r.data.dump(2));
    REQUIRE(r.ok());
    REQUIRE(r.data["exports"].size() == 1);
    CHECK(r.data["exports"][0]["output"] == pathToUtf8(dir / "Stream 2026.11.03 Highlights.mp4"));
    r = dry(dir / "cut v1.2");
    REQUIRE(r.ok());
    CHECK(r.data["exports"][0]["output"] == pathToUtf8(dir / "cut v1.2.mp4"));
    r = dry(dir / "final.mov"); // a known extension is still swapped for the format's
    REQUIRE(r.ok());
    CHECK(r.data["exports"][0]["output"] == pathToUtf8(dir / "final.mp4"));
}

// ---- 149: the target-size audio estimate -----------------------------------------------------------------------------

TEST_CASE("Target size budgets the audio channels the file writes when channels follow the master", "[export][settings][channels]") {
    ExportSettings s = preset("match-source");
    s.container = Container::Mov;
    s.audio.enabled = true;
    s.audio.codec = AudioCodec::Pcm24;
    s.audio.sampleRate = 48000;
    s.audio.channels = 0; // as the master
    s.video.width.reset();
    s.video.height.reset();
    s.video.rateControl = RateControl::TargetSize;
    s.video.targetBytes = 200'000'000;
    Sequence seq = makeSequence(1920, 1080, kRate30, Ticks::fromSeconds(60));
    seq.masterFormat = ChannelFormat::Surround51;

    const auto budget = [&](int channels) {
        const double audioBits = 24.0 * 48000.0 * channels * 60.0;
        return static_cast<std::int64_t>((200e6 * 8.0 * (1.0 - kTargetSizeMargin) - audioBits) / 60.0);
    };
    const auto surround = resolve(s, seq);
    REQUIRE(surround.has_value());
    CHECK(std::abs(surround->targetVideoBitrate - budget(6)) <= 2); // 6 channels, not 1
    seq.masterFormat = ChannelFormat::Stereo;
    const auto stereo = resolve(s, seq);
    REQUIRE(stereo.has_value());
    CHECK(std::abs(stereo->targetVideoBitrate - budget(2)) <= 2);
    CHECK(stereo->targetVideoBitrate > surround->targetVideoBitrate);
    // An explicit count is used as it is.
    s.audio.channels = 6;
    const auto explicitSix = resolve(s, seq);
    REQUIRE(explicitSix.has_value());
    CHECK(explicitSix->targetVideoBitrate == surround->targetVideoBitrate);

    // The "too long for N MB" check counts them too: a minute of 5.1 PCM (52 MB) does not fit 40 MB, stereo (17 MB) does.
    s.audio.channels = 0;
    s.video.targetBytes = 40'000'000;
    seq.masterFormat = ChannelFormat::Surround51;
    const auto tooLong = resolve(s, seq);
    REQUIRE_FALSE(tooLong.has_value());
    CHECK(tooLong.error().message.find("too long") != std::string::npos);
    seq.masterFormat = ChannelFormat::Stereo;
    CHECK(resolve(s, seq).has_value());
}

// ---- 150: the second pass of a target-size export --------------------------------------------------------------------

namespace {

/// A target-size H.264 export of the synthetic pattern whose FIRST pass is made over the size (the finalize hook pads it).
ExportRequest oversizeFirstPass(const TestProject& project, const fs::path& output) {
    ExportSettings s = preset("match-source");
    s.video.width.reset();
    s.video.height.reset();
    s.video.rateControl = RateControl::TargetSize;
    s.video.targetBytes = 1'000'000;
    s.audio.enabled = false;
    s.metadata.xmp = XmpMode::Sidecar;
    ExportRequest request = makeRequest(project, s, output);
    auto passes = std::make_shared<int>(0);
    request.finalize = [passes](const fs::path& file, std::stop_token) -> std::string {
        if ((*passes)++ == 0) {
            std::ofstream(file, std::ios::binary | std::ios::app) << std::string(2'000'000, 'x');
        }
        return {};
    };
    return request;
}

/// The synthetic source for the first pass, then `second` for the retry.
VideoSourceFactory firstThen(VideoSourceFactory second) {
    auto calls = std::make_shared<int>(0);
    return [calls, second = std::move(second), first = syntheticVideoSourceFactory()](
               const ProjectState& project, const Sequence& sequence,
               const VideoSourceFormat& format) -> std::expected<std::unique_ptr<IVideoFrameSource>, Error> {
        return (*calls)++ == 0 ? first(project, sequence, format) : second(project, sequence, format);
    };
}

} // namespace

TEST_CASE("A target-size export that ends over its size is encoded again and the smaller file is the result", "[export][target]") {
    TestProject project(480, 270, kRate30, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("target-retry");
    const fs::path out = dir / "target.mp4";
    const auto result = ExportJob(oversizeFirstPass(project, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->attempts == 2);
    CHECK(result->bytes <= 1'000'000);
    CHECK(static_cast<std::int64_t>(fs::file_size(out)) == result->bytes);
    CHECK(fs::exists(exporter::detail::xmpSidecarPath(out)));
    CHECK(partialFiles(dir).empty());
}

TEST_CASE("Cancelling the second pass of a target-size export leaves no new file and keeps the old one", "[export][target]") {
    TestProject project(480, 270, kRate30, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("target-cancel");
    const fs::path out = dir / "target.mp4";
    writeAll(out, "previous export");
    std::stop_source cancel;
    ExportRequest request = oversizeFirstPass(project, out);
    request.videoSource = firstThen(cancellingAfter(syntheticVideoSourceFactory(), 3, cancel)); // cancelled in pass two
    ExportJob job(std::move(request));
    const auto result = job.run(cancel.get_token());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(job.progress().phase == ExportPhase::Cancelled);
    CHECK(readAll(out) == "previous export"); // the over-size first pass was never put in place
    CHECK_FALSE(fs::exists(exporter::detail::xmpSidecarPath(out)));
    CHECK(partialFiles(dir).empty());
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator()) == 1);

    // With no earlier file, a cancelled second pass leaves the folder empty.
    const fs::path fresh = dir / "fresh.mp4";
    std::stop_source cancelAgain;
    ExportRequest second = oversizeFirstPass(project, fresh);
    second.videoSource = firstThen(cancellingAfter(syntheticVideoSourceFactory(), 3, cancelAgain));
    const auto result2 = ExportJob(std::move(second)).run(cancelAgain.get_token());
    REQUIRE_FALSE(result2.has_value());
    CHECK_FALSE(fs::exists(fresh));
    CHECK_FALSE(fs::exists(exporter::detail::xmpSidecarPath(fresh)));
    CHECK(partialFiles(dir).empty());
}

TEST_CASE("A second pass that fails keeps the first file as a result that says it is over the size", "[export][target]") {
    TestProject project(480, 270, kRate30, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("target-fail");
    const fs::path out = dir / "target.mp4";
    ExportRequest request = oversizeFirstPass(project, out);
    request.videoSource = firstThen([](const ProjectState&, const Sequence&, const VideoSourceFormat&)
                                        -> std::expected<std::unique_ptr<IVideoFrameSource>, Error> {
        return std::unexpected(Error{ErrorCode::IoError, "disk full"});
    });
    const auto result = ExportJob(std::move(request)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->attempts == 2);
    CHECK(result->bytes > 1'000'000);
    CHECK(fs::exists(out));
    CHECK(fs::exists(exporter::detail::xmpSidecarPath(out)));
    REQUIRE_FALSE(result->warnings.empty());
    CHECK(std::ranges::any_of(result->warnings, [](const std::string& w) {
        return w.find("disk full") != std::string::npos && w.find("over") != std::string::npos;
    }));
    CHECK(partialFiles(dir).empty());
}

// ---- 151: UTF-8 safe truncation --------------------------------------------------------------------------------------

TEST_CASE("truncateUtf8 never cuts a character in two", "[export][utf8]") {
    using exporter::detail::truncateUtf8;
    std::string a = "short";
    truncateUtf8(a, 10);
    CHECK(a == "short");
    std::string thai = repeated("\xE0\xB8\x81", 10); // 30 bytes
    for (std::size_t limit : {std::size_t{30}, std::size_t{29}, std::size_t{28}, std::size_t{27}, std::size_t{7}, std::size_t{1}, std::size_t{0}}) {
        std::string t = thai;
        truncateUtf8(t, limit);
        INFO(limit);
        CHECK(t.size() <= limit);
        CHECK(t.size() % 3 == 0);
        CHECK(validUtf8(t));
        CHECK(t.size() + 3 > std::min<std::size_t>(limit, 30)); // as much as fits
    }
    std::string mixed = "a" + repeated("\xC3\xA9", 10); // 1 + 20 bytes: the limit falls inside a character
    truncateUtf8(mixed, 6);
    CHECK(mixed == "a" + repeated("\xC3\xA9", 2));
    std::string emoji = repeated("\xF0\x9F\x8E\xAC", 4); // 4-byte characters
    truncateUtf8(emoji, 10);
    CHECK(emoji.size() == 8);
    CHECK(validUtf8(emoji));
}

TEST_CASE("A long Thai chapter comment becomes a chapter title that is still valid UTF-8", "[export][utf8]") {
    Sequence seq = makeSequence(1920, 1080, kRate25, Ticks::fromSeconds(60));
    Marker m;
    m.type = MarkerType::Chapter;
    m.start = Ticks{};
    m.comment = repeated("\xE0\xB8\x81", 400); // 1200 bytes, no name: the comment becomes the title
    seq.markers.push_back(m);
    Marker m2 = m;
    m2.start = Ticks::fromSeconds(10);
    m2.comment = "a" + repeated("\xC3\xA9", 600); // the limit of 1024 bytes falls in the middle of a two-byte character
    seq.markers.push_back(m2);
    const auto chapters = exporter::detail::chapterMarks(seq, TimeRange{Ticks{}, Ticks::fromSeconds(60)});
    REQUIRE(chapters.size() == 2);
    for (const auto& c : chapters) {
        CHECK(c.title.size() <= 1024);
        CHECK(validUtf8(c.title));
    }
    CHECK(chapters[0].title.size() == 1023); // 341 characters
    CHECK(chapters[1].title.size() == 1023);
}

// ---- 152: a set of files moved into place as one unit ----------------------------------------------------------------

TEST_CASE("commitFileSet replaces every file or none", "[export][mxf]") {
    using exporter::detail::commitFileSet;
    using exporter::detail::FileMove;
    const auto dir = freshOutputDir("commit-set");
    // Three files, the last two replace earlier ones.
    writeAll(dir / "audio1.new", "A1 new");
    writeAll(dir / "audio2.new", "A2 new");
    writeAll(dir / "picture.new", "V new");
    writeAll(dir / "audio2.mxf", "A2 old");
    writeAll(dir / "picture.mxf", "V old");
    auto done = commitFileSet({{dir / "audio1.new", dir / "audio1.mxf"}, {dir / "audio2.new", dir / "audio2.mxf"},
                               {dir / "picture.new", dir / "picture.mxf"}});
    REQUIRE(done.has_value());
    CHECK(readAll(dir / "audio1.mxf") == "A1 new");
    CHECK(readAll(dir / "audio2.mxf") == "A2 new");
    CHECK(readAll(dir / "picture.mxf") == "V new");
    CHECK(partialFiles(dir).empty()); // the backups of the replaced files are gone

    // The picture's temporary file is missing: the move fails after the audio files were put in place - all of them go
    // back to what they were.
    writeAll(dir / "audio1.new", "A1 newer");
    writeAll(dir / "audio2.new", "A2 newer");
    done = commitFileSet({{dir / "audio1.new", dir / "audio1.mxf"}, {dir / "audio2.new", dir / "audio2.mxf"},
                          {dir / "picture.missing", dir / "picture.mxf"}});
    REQUIRE_FALSE(done.has_value());
    CHECK(done.error().code == ErrorCode::IoError);
    CHECK(readAll(dir / "audio1.mxf") == "A1 new");
    CHECK(readAll(dir / "audio2.mxf") == "A2 new");
    CHECK(readAll(dir / "picture.mxf") == "V new");
    CHECK(fs::exists(dir / "audio1.new")); // the caller's temporaries are still its own
    CHECK(partialFiles(dir).empty());

    // A first export (nothing to replace) that fails leaves no new file behind either.
    writeAll(dir / "b1.new", "B1");
    done = commitFileSet({{dir / "b1.new", dir / "b1.mxf"}, {dir / "b2.missing", dir / "b2.mxf"}});
    REQUIRE_FALSE(done.has_value());
    CHECK_FALSE(fs::exists(dir / "b1.mxf"));
    CHECK_FALSE(fs::exists(dir / "b2.mxf"));
}

TEST_CASE("removeStaleAudioFiles removes the earlier export's extra tracks and spares source media", "[export][mxf]") {
    const auto dir = freshOutputDir("stale-audio");
    const fs::path video = dir / "clip.mxf";
    // What Wabcut writes into an MXF's Identification set: its name as UTF-16BE (configureMxfMuxer).
    const std::string wrote("\x00W\x00""a\x00""b\x00""c\x00u\x00t", 12);
    for (int n = 1; n <= 6; ++n) {
        writeAll(exporter::detail::opAtomAudioPath(video, n), "header " + wrote + " audio " + std::to_string(n));
    }
    const fs::path source = exporter::detail::opAtomAudioPath(video, 5); // a source clip that happens to have such a name
    // Review fix: a file another program (or a camera) made, with the same kind of name, is not an earlier export's.
    const fs::path foreign = exporter::detail::opAtomAudioPath(video, 7);
    writeAll(foreign, "A camera original, not written by this program");
    std::vector<fs::path> left;
    const auto removed = exporter::detail::removeStaleAudioFiles(video, 3, {source}, &left);
    REQUIRE(removed.size() == 2);
    CHECK_FALSE(fs::exists(exporter::detail::opAtomAudioPath(video, 4)));
    CHECK_FALSE(fs::exists(exporter::detail::opAtomAudioPath(video, 6)));
    CHECK(fs::exists(exporter::detail::opAtomAudioPath(video, 5)));
    CHECK(fs::exists(foreign));
    CHECK(left.size() == 2); // the source clip and the foreign file
    for (int n = 1; n <= 3; ++n) {
        CHECK(fs::exists(exporter::detail::opAtomAudioPath(video, n)));
    }
    CHECK(exporter::detail::isWabcutMxf(exporter::detail::opAtomAudioPath(video, 1)));
    CHECK_FALSE(exporter::detail::isWabcutMxf(foreign));
    CHECK_FALSE(exporter::detail::isWabcutMxf(dir / "missing.mxf"));
}

TEST_CASE("Re-exporting an OP-Atom MXF with fewer tracks removes the earlier export's extra audio files", "[export][mxf]") {
    TestProject project(1280, 720, kRate25, Ticks::fromSeconds(1));
    ExportSettings s = preset("mxf-opatom-dnxhd-hq");
    s.video.dnxhrProfile = DnxhrProfile::Lb;
    s.video.width.reset();
    s.video.height.reset();
    const auto dir = freshOutputDir("mxf-opatom-fewer");
    const fs::path out = dir / "clip.mxf";
    s.audio.channels = 4;
    const auto first = ExportJob(makeRequest(project, s, out)).run();
    INFO((first ? std::string() : first.error().message));
    REQUIRE(first.has_value());
    REQUIRE(first->extraFiles.size() == 4);
    s.audio.channels = 2;
    const auto second = ExportJob(makeRequest(project, s, out)).run();
    INFO((second ? std::string() : second.error().message));
    REQUIRE(second.has_value());
    CHECK(second->extraFiles.size() == 2);
    CHECK(fs::exists(exporter::detail::opAtomAudioPath(out, 1)));
    CHECK(fs::exists(exporter::detail::opAtomAudioPath(out, 2)));
    CHECK_FALSE(fs::exists(exporter::detail::opAtomAudioPath(out, 3)));
    CHECK_FALSE(fs::exists(exporter::detail::opAtomAudioPath(out, 4)));
    CHECK(std::ranges::any_of(second->warnings, [](const std::string& w) { return w.find("clip_A3.mxf") != std::string::npos; }));
    CHECK(partialFiles(dir).empty());
}

#ifdef _WIN32
TEST_CASE("A locked audio file of the earlier OP-Atom export fails the re-export and leaves the media set as it was", "[export][mxf]") {
    TestProject project(1280, 720, kRate25, Ticks::fromSeconds(1));
    ExportSettings s = preset("mxf-opatom-dnxhd-hq");
    s.video.dnxhrProfile = DnxhrProfile::Lb;
    s.video.width.reset();
    s.video.height.reset();
    s.audio.channels = 3;
    s.mxf.reelName = "TAPE1";
    const auto dir = freshOutputDir("mxf-opatom-locked");
    const fs::path out = dir / "clip.mxf";
    REQUIRE(ExportJob(makeRequest(project, s, out)).run().has_value());
    std::vector<std::pair<fs::path, std::string>> before;
    before.emplace_back(out, readAll(out));
    for (int n = 1; n <= 3; ++n) {
        before.emplace_back(exporter::detail::opAtomAudioPath(out, n), readAll(exporter::detail::opAtomAudioPath(out, n)));
    }

    // Another program (Media Composer) has the second audio file open, without sharing it for deletion or renaming.
    const std::wstring locked = exporter::detail::opAtomAudioPath(out, 2).wstring();
    HANDLE handle = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    s.mxf.reelName = "TAPE2"; // the new export would differ from the old one
    const auto again = ExportJob(makeRequest(project, s, out)).run();
    CloseHandle(handle);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == ErrorCode::IoError);
    for (const auto& [file, content] : before) {
        INFO(file.string());
        CHECK(readAll(file) == content); // the picture and every audio file are still the earlier export's
    }
    CHECK(partialFiles(dir).empty());
    // Released, the same export goes through.
    CHECK(ExportJob(makeRequest(project, s, out)).run().has_value());
}
#endif
