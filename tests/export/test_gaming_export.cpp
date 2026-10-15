// Gaming exports (I.9, the gaming re-scope): the 60 fps YouTube, Shorts and Twitch presets; target sizes that land
// under Discord's upload limits - the whole file, video, audio and container, at most the size, close to it; 120 / 144 /
// 240 fps captures exported frame for frame (no frame dropped, doubled or out of order: the fixtures carry their frame
// number as a barcode); and variable-frame-rate captures (OBS dropping frames under load, a phone's alternating frame
// lengths) conformed to the sequence's rate, each sequence frame showing the capture frame on screen at its time.

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/Layouts.h>
#include <wabcut/model/Synthetic.h>

#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

/// `length` of the 1080p60 test clip (a moving pattern) scaled into a 1280x720 60 fps sequence, with its own audio.
TestProject gameplay(Ticks length) {
    TestProject p(1280, 720, Rate{60, 1}, length);
    const MediaId capture = p.addMedia("h264_1080p60_aac.mp4");
    Clip video = p.clip(capture, StreamKind::Video, Ticks{}, length, Ticks{});
    ClipProperties props;
    props.set(ParamId::Scale, 1280.0 / 1920.0);
    video.setProperties(props);
    p.addTrack(TrackKind::Video, {video});
    p.addAudioTrack(capture, Ticks{}, length, Ticks{});
    return p;
}

ExportSettings preset(std::string_view id) {
    auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

} // namespace

TEST_CASE("Gaming presets: 60 fps YouTube, Shorts and Twitch, and Discord's sizes", "[export][settings][gaming]") {
    struct Expect {
        const char* id;
        VideoCodec codec;
        int width;
        int height;
    };
    for (const Expect& e : {Expect{"youtube-1080p60", VideoCodec::H264, 1920, 1080}, Expect{"youtube-1440p60", VideoCodec::H264, 2560, 1440},
                            Expect{"youtube-4k60", VideoCodec::Hevc, 3840, 2160}, Expect{"youtube-1440p60-av1", VideoCodec::Av1, 2560, 1440},
                            Expect{"youtube-4k60-av1", VideoCodec::Av1, 3840, 2160}, Expect{"shorts-1080x1920-60", VideoCodec::H264, 1080, 1920},
                            Expect{"twitch-1080p60", VideoCodec::H264, 1920, 1080}}) {
        INFO(e.id);
        const ExportSettings s = preset(e.id);
        CHECK(s.presetId == e.id);
        CHECK(validate(s).has_value());
        CHECK(s.container == Container::Mp4);
        CHECK(s.video.codec == e.codec);
        CHECK(s.video.width == e.width);
        CHECK(s.video.height == e.height);
        CHECK(s.video.frameRate == Rate{60, 1});
        CHECK(s.video.rateControl == RateControl::Bitrate);
    }
    CHECK(preset("shorts-1080x1920-60").video.scaleMode == ScaleMode::Fill);
    CHECK(preset("twitch-1080p60").video.maxBitrate == 6'000'000); // Twitch's ceiling
    for (const auto& [id, megabytes] : {std::pair{"discord-10mb", 10}, std::pair{"discord-25mb", 25}, std::pair{"discord-50mb", 50}}) {
        INFO(id);
        const ExportSettings s = preset(id);
        CHECK(validate(s).has_value());
        CHECK(s.video.rateControl == RateControl::TargetSize);
        CHECK(s.video.targetBytes == std::int64_t{megabytes} * 1'000'000);
    }
}

TEST_CASE("Target size: the video gets what the audio and the margin leave; a range too long for it is refused",
          "[export][settings][gaming]") {
    const TestProject p = gameplay(Ticks::fromSeconds(600));
    ExportSettings s = preset("discord-10mb");
    s.range = RangeMode::Custom;
    s.customRange = TimeRange{Ticks{}, Ticks::fromSeconds(30)};
    const auto r = resolve(s, p.seq());
    REQUIRE(r.has_value());
    // 10 MB less 4 %, less 30 s of 96 kb/s audio, over 30 s.
    const double expected = (10e6 * 8.0 * (1.0 - kTargetSizeMargin) - 96'000.0 * 30.0) / 30.0;
    CHECK(std::abs(static_cast<double>(r->targetVideoBitrate) - expected) < 2'000.0);
    // Ten minutes do not fit in 10 MB at any useful quality: the message says how long fits.
    s.range = RangeMode::WholeSequence;
    const auto tooLong = resolve(s, p.seq());
    REQUIRE_FALSE(tooLong.has_value());
    CHECK(tooLong.error().code == ErrorCode::InvalidArgument);
    INFO(tooLong.error().message);
    CHECK(tooLong.error().message.find("fits at most") != std::string::npos);
    // Only codecs with rate control take a size.
    ExportSettings prores = preset("prores-422-hq");
    prores.video.rateControl = RateControl::TargetSize;
    prores.video.targetBytes = 10'000'000;
    CHECK_FALSE(validate(prores).has_value());
    s.video.targetBytes = 1000; // below 1 MB
    CHECK_FALSE(validate(s).has_value());
}

TEST_CASE("Target size: the exported file lands under the cap and close to it (hardware first, and libx264)",
          "[export][media][gpu][gaming]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const Ticks length = Ticks::fromSeconds(8);
    const TestProject project = gameplay(length);
    for (const char* encoder : {"auto", "libx264"}) {
        DYNAMIC_SECTION(encoder) {
            ExportSettings s = preset("discord-10mb");
            s.video.encoder = encoder;
            s.video.targetBytes = 1'500'000; // tight enough for 8 s of moving 720p60 to need every bit
            s.range = RangeMode::Custom;
            s.customRange = TimeRange{Ticks{}, length};
            const auto dir = freshOutputDir(std::string("target-size-") + encoder);
            const auto out = dir / "discord.mp4";
            ExportRequest request = makeRequest(project, s, out);
            request.videoSource = {}; // the rendered source: the real picture
            const auto result = ExportJob(std::move(request)).run();
            INFO((result ? std::string() : result.error().message));
            REQUIRE(result.has_value());
            const auto bytes = static_cast<std::int64_t>(std::filesystem::file_size(out));
            CHECK(result->bytes == bytes);
            metric(std::format("target size {} B via {}: {} B ({:.1f} %), {} attempt(s), video {} kb/s", s.video.targetBytes,
                               result->videoEncoder, bytes, 100.0 * static_cast<double>(bytes) / static_cast<double>(s.video.targetBytes),
                               result->attempts, result->videoBitrate / 1000));
            CHECK(bytes <= s.video.targetBytes);
            CHECK(bytes >= s.video.targetBytes * 7 / 10); // the size was used, not thrown away
            const auto probe = ffprobe(out);
            REQUIRE(probe);
            const auto* v = probe->stream("video");
            REQUIRE(v != nullptr);
            CHECK(v->at("width") == "1280");
            CHECK(v->at("height") == "720");
            CHECK(v->at("nb_frames") == "480"); // 8 s at 60 fps, every frame
        }
    }
}

namespace {

/// The barcode (frame number of the capture) of every frame of an exported file, in order.
std::vector<int> barcodesOf(const std::filesystem::path& file) {
    std::vector<int> codes;
    std::string error;
    const bool decoded = decodeVideo(file, [&](const YuvFrame& f) { codes.push_back(barcodeOf(f)); }, &error);
    INFO(error);
    REQUIRE(decoded);
    return codes;
}

/// A 640x360 sequence of `rate` holding `length` of a capture from its start, exported at the sequence's size and rate
/// (Match Source, high quality so every barcode survives, no audio) through the rendered source.
std::vector<int> exportedBarcodes(const std::string& capture, Rate rate, Ticks length, const std::string& dirName,
                                  std::int64_t* frames) {
    TestProject project(640, 360, rate, length);
    const MediaId m = project.addMedia(capture);
    project.addVideoTrack(m, Ticks{}, length, Ticks{});
    ExportSettings s = preset("match-source");
    s.audio.enabled = false;
    s.video.quality = 12;
    const auto out = freshOutputDir(dirName) / "capture.mp4";
    ExportRequest request = makeRequest(project, s, out);
    request.videoSource = {}; // the rendered source: evaluator, decoders and compositor, as the monitor plays it
    const auto result = ExportJob(std::move(request)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    *frames = result->frames;
    const auto probe = ffprobe(out);
    REQUIRE(probe);
    const auto* v = probe->stream("video");
    REQUIRE(v != nullptr);
    CHECK(v->at("r_frame_rate") == std::format("{}/{}", rate.num, rate.den));
    return barcodesOf(out);
}

} // namespace

TEST_CASE("High frame rates: 120, 144 and 240 fps captures export every frame once, in order",
          "[export][media][gpu][gaming][hfr]") {
    for (const auto& [fps, seconds] : {std::pair{120, 2}, std::pair{144, 2}, std::pair{240, 1}}) {
        const std::string capture = std::format("h264_360p{}.mp4", fps);
        DYNAMIC_SECTION(capture) {
            if (!haveMedia(capture)) {
                SKIP("test clips were not generated (tools/gen_test_media.py)");
            }
            std::int64_t frames = 0;
            const std::vector<int> codes =
                exportedBarcodes(capture, Rate{fps, 1}, Ticks::fromSeconds(seconds), std::format("hfr-{}", fps), &frames);
            CHECK(frames == fps * seconds);
            REQUIRE(static_cast<std::int64_t>(codes.size()) == fps * seconds);
            int wrong = 0;
            std::string first;
            for (std::size_t k = 0; k < codes.size(); ++k) {
                if (codes[k] != static_cast<int>(k)) {
                    if (wrong++ == 0) {
                        first = std::format("frame {} shows capture frame {}", k, codes[k]);
                    }
                }
            }
            INFO(first);
            CHECK(wrong == 0); // no frame dropped, doubled or out of order
            metric(std::format("{} fps capture: {} frames exported, {} out of place", fps, codes.size(), wrong));
        }
    }
}

TEST_CASE("Variable frame rates conform: each sequence frame shows the capture frame on screen at its time (OBS, phone)",
          "[export][media][gpu][gaming][vfr]") {
    struct Case {
        const char* capture;
        Rate rate;      ///< the sequence's
        double seconds; ///< exported
        int sourceFrames;
        double (*start)(int); ///< when capture frame n starts (the fixture's timing)
    };
    const Case cases[] = {
        // OBS under load: a 60 fps grid with every seventh frame dropped, in a 60 fps sequence (the frame before a drop
        // shows twice).
        {"obs_vfr.mp4", Rate{60, 1}, 2.3, 120, [](int n) { return (n + n / 6) / 60.0; }},
        // A phone: frames alternately 1/30 s and 1/24 s long, in a 30 fps sequence.
        {"phone_vfr.mp4", Rate{30, 1}, 2.2, 60,
         [](int n) { return (n / 2) * (1.0 / 30.0 + 1.0 / 24.0) + (n % 2) / 30.0; }},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.capture) {
            if (!haveMedia(c.capture)) {
                SKIP("test clips were not generated (tools/gen_test_media.py)");
            }
            std::int64_t frames = 0;
            const Ticks length{static_cast<std::int64_t>(c.seconds * static_cast<double>(kTicksPerSecond))};
            const std::vector<int> codes = exportedBarcodes(c.capture, c.rate, length, std::string("vfr-") + c.capture, &frames);
            REQUIRE(static_cast<std::int64_t>(codes.size()) == frames);
            int wrong = 0;
            std::string first;
            for (std::size_t k = 0; k < codes.size(); ++k) {
                const double t = static_cast<double>(k) * c.rate.den / c.rate.num;
                int expected = 0;
                for (int n = 0; n < c.sourceFrames && c.start(n) <= t + 1e-9; ++n) {
                    expected = n; // the last frame that started by t is on screen
                }
                if (codes[k] != expected && wrong++ == 0) {
                    first = std::format("sequence frame {} ({:.4f} s) shows capture frame {}, expected {}", k, t, codes[k], expected);
                }
            }
            INFO(first);
            CHECK(wrong == 0);
            metric(std::format("{} in a {} fps sequence: {} frames, {} not the frame on screen", c.capture, c.rate.num,
                               codes.size(), wrong));
        }
    }
}

TEST_CASE("GIF: the preset, and what a GIF can hold", "[export][settings][gaming][gif]") {
    const ExportSettings gif = preset("gif-480p15");
    CHECK(validate(gif).has_value());
    CHECK(gif.container == Container::Gif);
    CHECK(gif.video.codec == VideoCodec::Gif);
    CHECK(gif.video.pixelFormat == OutputPixelFormat::Pal8);
    CHECK_FALSE(gif.audio.enabled);
    CHECK(fileExtension(Container::Gif) == ".gif");
    ExportSettings s = gif;
    s.audio.enabled = true; // a GIF has no sound
    CHECK_FALSE(validate(s).has_value());
    s = gif;
    s.video.frameRate = Rate{60, 1}; // GIF frame times are hundredths of a second
    CHECK_FALSE(validate(s).has_value());
    s = gif;
    s.container = Container::Mp4; // GIF video only in a .gif
    CHECK_FALSE(validate(s).has_value());
    s = preset("youtube-1080p60");
    s.video.pixelFormat = OutputPixelFormat::Pal8; // a palette only for GIF
    CHECK_FALSE(validate(s).has_value());
}

TEST_CASE("GIF: an animated GIF of the timeline - 480x270 at 15 fps, every frame, a palette per frame",
          "[export][media][gpu][gaming][gif]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = gameplay(Ticks::fromSeconds(2));
    ExportSettings s = preset("gif-480p15");
    s.range = RangeMode::Custom;
    s.customRange = TimeRange{Ticks{}, Ticks::fromSeconds(2)};
    const auto dir = freshOutputDir("gif");
    const auto out = dir / "meme.gif";
    ExportRequest request = makeRequest(project, s, out);
    request.videoSource = {};
    const auto result = ExportJob(std::move(request)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->videoEncoder == "gif");
    CHECK(result->frames == 30);
    const auto probe = ffprobe(out);
    REQUIRE(probe);
    CHECK(probe->format.at("format_name") == "gif");
    const auto* v = probe->stream("video");
    REQUIRE(v != nullptr);
    CHECK(v->at("codec_name") == "gif");
    CHECK(v->at("width") == "480");
    CHECK(v->at("height") == "270");
    CHECK(probe->stream("audio") == nullptr);
    CHECK(countVideoPackets(out) == 30); // one GIF image per frame
    const auto bytes = std::filesystem::file_size(out);
    metric(std::format("GIF 480x270 15 fps, 2 s of moving 720p60: {} KB", bytes / 1024));
    CHECK(bytes > 50'000);     // pictures, not empty frames
    CHECK(bytes < 20'000'000); // and not absurd
}

TEST_CASE("WebM: the preset, and what WebM holds (VP9 or AV1, Opus)", "[export][settings][gaming][webm]") {
    const ExportSettings webm = preset("webm-vp9-720p");
    CHECK(validate(webm).has_value());
    CHECK(webm.container == Container::WebM);
    CHECK(webm.video.codec == VideoCodec::Vp9);
    CHECK(webm.audio.codec == AudioCodec::Opus);
    CHECK(fileExtension(Container::WebM) == ".webm");
    ExportSettings s = webm;
    s.video.codec = VideoCodec::H264; // not in WebM
    CHECK_FALSE(validate(s).has_value());
    s = webm;
    s.audio.codec = AudioCodec::Aac; // WebM's audio is Opus
    CHECK_FALSE(validate(s).has_value());
    s = webm;
    s.video.codec = VideoCodec::Av1; // AV1 in WebM is fine
    CHECK(validate(s).has_value());
    s = preset("youtube-1080p60");
    s.video.codec = VideoCodec::Vp9; // VP9 in MP4: no
    CHECK_FALSE(validate(s).has_value());
    s = webm;
    s.audio.sampleRate = 44100; // Opus runs at 48 kHz
    CHECK_FALSE(validate(s).has_value());
}

TEST_CASE("WebM: a VP9 + Opus export of the timeline, every frame, close to the picture", "[export][media][gpu][gaming][webm]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = gameplay(Ticks::fromSeconds(2));
    ExportSettings s = preset("webm-vp9-720p");
    s.range = RangeMode::Custom;
    s.customRange = TimeRange{Ticks{}, Ticks::fromSeconds(2)};
    const auto out = freshOutputDir("webm") / "clip.webm";
    ExportRequest request = makeRequest(project, s, out);
    request.videoSource = {};
    const auto result = ExportJob(std::move(request)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 120);
    const auto probe = ffprobe(out);
    REQUIRE(probe);
    CHECK(probe->format.at("format_name").find("webm") != std::string::npos);
    const auto* v = probe->stream("video");
    const auto* a = probe->stream("audio");
    REQUIRE(v != nullptr);
    REQUIRE(a != nullptr);
    CHECK(v->at("codec_name") == "vp9");
    CHECK(v->at("width") == "1280");
    CHECK(v->at("height") == "720");
    CHECK(a->at("codec_name") == "opus");
    // The pictures: the export against the monitor's, frame by frame.
    exporttest::PreviewRenderer preview(project);
    int frames = 0;
    double minPsnr = 1e9;
    std::string error;
    REQUIRE(decodeVideo(
        out,
        [&](const YuvFrame& f) {
            const auto rgba = preview.render(framesToTicks(frames, Rate{60, 1}), framesToTicks(1, Rate{60, 1}), f.width, f.height);
            minPsnr = std::min(minPsnr, psnr(f, yuvFromRgba(rgba, f.width, f.height, f)));
            ++frames;
        },
        &error));
    INFO(error);
    CHECK(frames == 120);
    metric(std::format("WebM VP9 720p60 CRF 32 + Opus: {} KB for 2 s, preview==export PSNR min {:.1f} dB",
                       std::filesystem::file_size(out) / 1024, minPsnr));
    CHECK(minPsnr >= 30.0); // lossy web delivery; the same picture, not a different one
}

namespace {

/// RGB (0..255) of a pixel of an RGBA8 picture.
std::array<int, 3> rgbAt(const std::vector<std::uint8_t>& rgba, int width, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2]};
}

} // namespace

TEST_CASE("Layouts render: a rounded facecam picture-in-picture with a border and a shadow (C.15)",
          "[export][media][gpu][gaming][layouts]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto renderWithBorder = [](double width) {
        TestProject project = gameplay(Ticks::fromSeconds(2));
        LayoutRequest request;
        request.preset = LayoutPreset::PictureInPicture;
        request.gameplay = project.seq().videoTracks.front()->clips.front().id;
        request.options.facecamRegion = NormalizedRect{0.0, 0.0, 0.3, 0.3}; // the webcam corner of the capture
        request.options.borderWidth = width;
        request.options.borderColor = {1.0, 0.0, 1.0};
        const auto laid = edit::applyLayout(project.state, project.sequence, request);
        INFO((laid ? std::string() : laid.error().message));
        REQUIRE(laid.has_value());
        project.state = laid->edit.state;
        exporttest::PreviewRenderer preview(project);
        return preview.render(Ticks::fromSeconds(1), framesToTicks(1, Rate{60, 1}));
    };
    const auto bordered = renderWithBorder(8.0);
    const auto plain = renderWithBorder(0.0);
    const auto dir = freshOutputDir("layouts-pip");
    QImage(bordered.data(), 1280, 720, 1280 * 4, QImage::Format_RGBA8888)
        .save(QString::fromStdString((dir / "pip_rounded_border.png").string()));
    QImage(plain.data(), 1280, 720, 1280 * 4, QImage::Format_RGBA8888)
        .save(QString::fromStdString((dir / "pip_rounded_no_border.png").string()));
    // The test picture is colour bars (a magenta bar among them): the border is what the 8 px magenta border changes -
    // magenta pixels around the facecam in the bottom-right corner, nothing anywhere else.
    int inCorner = 0;
    int magentaInCorner = 0;
    int elsewhere = 0;
    for (int y = 0; y < 720; ++y) {
        for (int x = 0; x < 1280; ++x) {
            const auto a = rgbAt(bordered, 1280, x, y);
            const auto b = rgbAt(plain, 1280, x, y);
            if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 30) {
                continue;
            }
            if (x > 640 && y > 360) {
                ++inCorner;
                magentaInCorner += a[0] > 200 && a[1] < 60 && a[2] > 200 ? 1 : 0;
            } else {
                ++elsewhere;
            }
        }
    }
    INFO("changed pixels: " << inCorner << " in the facecam corner (" << magentaInCorner << " magenta), " << elsewhere
                            << " elsewhere");
    CHECK(magentaInCorner > 2000); // a stroke 8 px wide around a ~360 x 200 box (its rounded corners trimmed)
    CHECK(magentaInCorner > inCorner / 2); // the rest: anti-aliased edges and the shadow moving out with the border
    CHECK(elsewhere < 50);
}

TEST_CASE("Adjustment layers render: a blur over V1 for the layer's duration only, mixed by its opacity (C.15)",
          "[export][media][gpu][gaming][adjustment]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto sharpestStep = [](const std::vector<std::uint8_t>& rgba, int y) {
        const auto luma = [&](int x) {
            const auto c = rgbAt(rgba, 1280, x, y);
            return (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 255.0;
        };
        double step = 0.0;
        for (int x = 1; x < 1280; ++x) {
            step = std::max(step, std::abs(luma(x) - luma(x - 1)));
        }
        return step;
    };
    const auto renderAt = [&](double opacity, Ticks at) {
        TestProject project = gameplay(Ticks::fromSeconds(2));
        project.addTrack(TrackKind::Video, {});
        const TrackId v2 = project.seq().videoTracks[1]->id;
        auto added = edit::addAdjustmentLayer(project.state, project.sequence, v2, Ticks{}, Ticks::fromSeconds(1));
        INFO((added ? std::string() : added.error().message));
        REQUIRE(added.has_value());
        project.state = added->state;
        project.editSequence([&](Sequence& s) {
            Track track = *s.videoTracks[1];
            std::vector<Clip> clips;
            for (std::size_t i = 0; i < track.clips.size(); ++i) {
                clips.push_back(track.clips[i]);
            }
            REQUIRE(clips.size() == 1);
            EffectInstance blur;
            blur.id = project.state.ids.allocate<EffectInstanceId>();
            blur.effect = "wabcut.blur.gaussian";
            blur.set("radius", ParamValue(30.0));
            blur.set("repeatEdges", ParamValue(1.0));
            clips[0].setEffects({blur});
            ClipProperties props = clips[0].properties();
            props.set(ParamId::Opacity, opacity);
            clips[0].setProperties(props);
            track.clips = ClipList(std::move(clips));
            s.videoTracks[1] = std::make_shared<const Track>(std::move(track));
        });
        exporttest::PreviewRenderer preview(project);
        return preview.render(at, framesToTicks(1, Rate{60, 1}));
    };
    const auto blurred = renderAt(1.0, framesToTicks(30, Rate{60, 1}));
    const auto after = renderAt(1.0, framesToTicks(90, Rate{60, 1}));
    const auto half = renderAt(0.5, framesToTicks(30, Rate{60, 1}));
    const auto dir = freshOutputDir("layouts-adjustment");
    QImage(blurred.data(), 1280, 720, 1280 * 4, QImage::Format_RGBA8888).save(QString::fromStdString((dir / "blurred.png").string()));
    QImage(after.data(), 1280, 720, 1280 * 4, QImage::Format_RGBA8888).save(QString::fromStdString((dir / "after.png").string()));
    QImage(half.data(), 1280, 720, 1280 * 4, QImage::Format_RGBA8888).save(QString::fromStdString((dir / "half.png").string()));
    // The colour bars' edges: soft under the adjustment layer, hard after it, half of each at 50 % opacity.
    const double soft = sharpestStep(blurred, 600);
    const double hard = sharpestStep(after, 600);
    const double mixed = sharpestStep(half, 600);
    INFO("sharpest bar edge: " << soft << " blurred, " << hard << " after, " << mixed << " at half opacity");
    CHECK(soft < 0.06);
    CHECK(hard > 0.2);
    CHECK(mixed > 1.5 * soft);
    CHECK(mixed < 0.75 * hard);
}

TEST_CASE("Layouts render: a vertical blur-fill Short - blurred to the frame edges, the sharp copy across the width (C.15)",
          "[export][media][gpu][gaming][layouts]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1080, 1920, Rate{60, 1}, Ticks::fromSeconds(2));
    const MediaId capture = project.addMedia("h264_1080p60_aac.mp4");
    project.addVideoTrack(capture, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    LayoutRequest request;
    request.preset = LayoutPreset::VerticalBlurFill;
    request.gameplay = project.seq().videoTracks.front()->clips.front().id;
    const auto laid = edit::applyLayout(project.state, project.sequence, request);
    INFO((laid ? std::string() : laid.error().message));
    REQUIRE(laid.has_value());
    project.state = laid->edit.state;
    exporttest::PreviewRenderer preview(project);
    const auto rgba = preview.render(Ticks::fromSeconds(1), framesToTicks(1, Rate{60, 1}));
    QImage(rgba.data(), 1080, 1920, 1080 * 4, QImage::Format_RGBA8888)
        .save(QString::fromStdString((freshOutputDir("layouts-shorts") / "vertical_blur_fill.png").string()));
    const auto luma = [&](int x, int y) {
        const auto c = rgbAt(rgba, 1080, x, y);
        return (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 255.0;
    };
    // The blurred background reaches the corners (repeated edges: no dark rim) ...
    for (const auto& [x, y] : {std::pair{2, 2}, std::pair{1077, 2}, std::pair{2, 1917}, std::pair{1077, 1917}}) {
        INFO("corner " << x << ", " << y);
        CHECK(luma(x, y) > 0.06);
        CHECK(std::abs(luma(x, y) - luma(x + (x < 540 ? 30 : -30), y + (y < 960 ? 30 : -30))) < 0.08); // no edge darkening
    }
    // ... and the sharp copy fills the width in the middle: its bar edges are hard steps, the background's are soft.
    const auto sharpestStep = [&](int y) {
        double step = 0.0;
        for (int x = 1; x < 1080; ++x) {
            step = std::max(step, std::abs(luma(x, y) - luma(x - 1, y)));
        }
        return step;
    };
    INFO("sharpest step along the middle row " << sharpestStep(960) << ", along background rows " << sharpestStep(100)
                                                << " / " << sharpestStep(1800));
    CHECK(sharpestStep(960) > 0.2);
    CHECK(sharpestStep(100) < 0.06);
    CHECK(sharpestStep(1800) < 0.06);
}
