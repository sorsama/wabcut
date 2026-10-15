// The production video source (RenderedVideoSource): a real 3-layer timeline with motion, opacity, a display-
// rotated clip, a disabled clip and a gap, exported through the playback evaluator, media decoders and
// render::OffscreenRenderer, then decoded and compared frame by frame with the preview's render of the same
// snapshot at the same times (Principle 5: preview == export). Audio is checked against the playback mix.

#include "ExportTestUtil.h"

#include <wabcut/export/CaptionSidecar.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/Proxy.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/export/ReviewFrames.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>
#include <wabcut/playback/audio/AudioRedactions.h>

#include <QByteArray>
#include <QColor>
#include <QImage>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <format>
#include <fstream>
#include <stop_token>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

constexpr Rate kRate{30, 1};
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

ExportSettings presetSettings(std::string_view id) {
    auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

ExportRequest renderedRequest(const TestProject& project, ExportSettings settings,
                              const std::filesystem::path& output) {
    ExportRequest r = makeRequest(project, std::move(settings), output);
    r.videoSource = {}; // the default: the rendered source
    return r;
}

struct Comparison {
    int frames = 0;
    int timeErrors = 0;
    double minPsnr = 1e9;
    double sumPsnr = 0.0;
    double meanPsnr() const { return frames > 0 ? sumPsnr / frames : 0.0; }
};

/// Decodes `file` and compares output frame k with the preview of sequence time start + k frame periods.
Comparison compareWithPreview(const TestProject& project, const std::filesystem::path& file, Ticks start = {},
                              int width = 0, int height = 0,
                              std::optional<playback::CaptionSettings> captions = std::nullopt) {
    exporttest::PreviewRenderer preview(project);
    preview.setCaptions(std::move(captions));
    Comparison c;
    const Ticks period = framesToTicks(1, kRate);
    std::string error;
    const bool ok = decodeVideo(
        file,
        [&](const YuvFrame& f) {
            const Ticks time = start + framesToTicks(c.frames, kRate);
            if (std::abs(f.seconds - static_cast<double>(c.frames) / kRate.toDouble()) > 0.001) {
                ++c.timeErrors;
            }
            const auto rgba = preview.render(time, period, width, height);
            const double p = psnr(f, yuvFromRgba(rgba, f.width, f.height, f));
            c.minPsnr = std::min(c.minPsnr, p);
            c.sumPsnr += std::isinf(p) ? 100.0 : p;
            ++c.frames;
        },
        &error);
    INFO(error);
    REQUIRE(ok);
    return c;
}

double snrDb(const std::vector<float>& reference, const std::vector<float>& actual, std::ptrdiff_t lag = 0) {
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const auto j = static_cast<std::ptrdiff_t>(i) + lag;
        if (j < 0 || j >= static_cast<std::ptrdiff_t>(actual.size())) {
            continue;
        }
        signal += static_cast<double>(reference[i]) * reference[i];
        const double d = static_cast<double>(reference[i]) - actual[static_cast<std::size_t>(j)];
        noise += d * d;
    }
    return noise == 0.0 ? 200.0 : 10.0 * std::log10(signal / noise);
}

/// The lag (in interleaved stereo samples / 2) that best aligns `actual` with `reference` (|lag| <= 2048).
std::ptrdiff_t bestLag(const std::vector<float>& reference, const std::vector<float>& actual) {
    std::ptrdiff_t best = 0;
    double bestSnr = -1e9;
    for (std::ptrdiff_t lag = -2048; lag <= 2048; ++lag) {
        const double s = snrDb(reference, actual, 2 * lag);
        if (s > bestSnr) {
            bestSnr = s;
            best = lag;
        }
    }
    return best;
}

std::int64_t pcmMismatches(const std::vector<float>& reference, const std::vector<std::int32_t>& pcm,
                           int bits) {
    std::int64_t mismatches = 0;
    for (std::size_t i = 0; i < std::min(reference.size(), pcm.size()); ++i) {
        if (quantizePcmSample(reference[i], bits) != pcm[i]) {
            ++mismatches;
        }
    }
    return mismatches;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

} // namespace

TEST_CASE("Rendered export == preview: H.264 MP4 (+AAC) and ProRes MOV (+PCM)", "[export][render][media][gpu]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = motionTimeline(kWidth, kHeight, kRate);
    const auto dir = freshOutputDir("rendered");
    const std::vector<float> mix = referenceMix(project, Ticks{}, 96'000);

    SECTION("H.264 MP4 + AAC") {
        ExportSettings settings = presetSettings("match-source");
        settings.video.quality = 12;
        const auto out = dir / "timeline.mp4";
        const auto result = ExportJob(renderedRequest(project, settings, out)).run();
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
        CHECK(result->videoSource.starts_with("OffscreenRenderer"));
        CHECK(result->frames == 60);
        const auto probe = ffprobe(out);
        REQUIRE(probe.has_value());
        REQUIRE(probe->stream("video") != nullptr);
        CHECK(probe->stream("video")->at("nb_frames") == "60");
        CHECK(probe->stream("video")->at("r_frame_rate") == "30/1");
        CHECK(probe->stream("video")->at("width") == "1280");

        const Comparison c = compareWithPreview(project, out);
        metric(
            std::format("rendered h264 ({}): preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames; "
                        "source {}",
                        result->videoEncoder, c.minPsnr, c.meanPsnr(), c.frames, result->videoSource));
        CHECK(c.frames == 60);
        CHECK(c.timeErrors == 0);
        CHECK(c.minPsnr >= 45.0);

        // AAC: aligned with the playback mix once the encoder delay is compensated (the edit list does it).
        const auto audio = decodeAudio(out);
        REQUIRE(audio.has_value());
        CHECK(audio->samples.size() == 96'000u * 2);
        const std::ptrdiff_t lag = bestLag(mix, audio->samples);
        const double snr = snrDb(mix, audio->samples, 2 * lag);
        metric(std::format("rendered h264: AAC lag {} samples, SNR {:.2f} dB vs the playback mix", lag, snr));
        CHECK(lag == 0);
        CHECK(snr >= 20.0);
    }

    SECTION("ProRes 422 HQ MOV + PCM 24") {
        const auto out = dir / "timeline.mov";
        const auto result = ExportJob(renderedRequest(project, presetSettings("prores-422-hq"), out)).run();
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
        const auto probe = ffprobe(out);
        REQUIRE(probe.has_value());
        CHECK(probe->stream("video")->at("nb_frames") == "60");
        CHECK(probe->stream("video")->at("pix_fmt") == "yuv422p10le");

        const Comparison c = compareWithPreview(project, out);
        metric(std::format("rendered prores: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames",
                           c.minPsnr, c.meanPsnr(), c.frames));
        CHECK(c.frames == 60);
        CHECK(c.timeErrors == 0);
        CHECK(c.minPsnr >= 50.0);

        const auto audio = decodeAudio(out);
        REQUIRE(audio.has_value());
        REQUIRE(audio->pcm.size() == 96'000u * 2);
        const auto mismatches = pcmMismatches(mix, audio->pcm, 24);
        metric(std::format("rendered prores: PCM24 mismatches vs the playback mix: {}", mismatches));
        CHECK(mismatches == 0);
    }

    SECTION("In/Out range at another output size") {
        TestProject marked = project;
        marked.editSequence([](Sequence& s) {
            s.markIn = ms(500);
            s.markOut = ms(1000);
        });
        ExportSettings settings = presetSettings("prores-422-hq");
        settings.range = RangeMode::InOut;
        settings.video.width = 640; // same aspect: the GPU renders at 640x360
        settings.video.height = 360;
        const auto out = dir / "range.mov";
        const auto result = ExportJob(renderedRequest(marked, settings, out)).run();
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
        CHECK(result->frames == 15);
        const Comparison c = compareWithPreview(marked, out, ms(500), 640, 360);
        metric(std::format("rendered prores 640x360 in/out: PSNR min {:.2f} dB", c.minPsnr));
        CHECK(c.frames == 15);
        CHECK(c.minPsnr >= 50.0);
    }
}

TEST_CASE("Rendered export of PNG with alpha and JPEG stills over video past their first second (RC.1)",
          "[export][render][media][gpu]") {
    // RC.1: a PNG did not decode at all (FFmpeg without zlib: "No decoder for png"), and a still's clip failed in
    // export after its first second (the decoder was asked for later source times of a one-frame file).
    if (!haveMedia("still_alpha.png") || !haveMedia("still.jpg") || !haveMedia("h264_1080p60_aac.mp4")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("rendered_stills");
    TestProject project(kWidth, kHeight, kRate, ms(2000));
    const MediaId video = project.addMedia("h264_1080p60_aac.mp4");
    const MediaId jpeg = project.addMedia("still.jpg");
    const MediaId png = project.addMedia("still_alpha.png");
    REQUIRE(project.state.findMedia(png)->info.isStillImage);
    REQUIRE(project.state.findMedia(png)->info.video.front().alpha == AlphaMode::Straight);
    project.addVideoTrack(video, Ticks{}, ms(2000), Ticks{});
    project.addVideoTrack(jpeg, Ticks{}, ms(2000), Ticks{}); // 640x360, centred: x 320..960, y 180..540
    project.addVideoTrack(png, Ticks{}, ms(2000), Ticks{});  // over it: its left half red, its right half clear
    ExportSettings settings = presetSettings("match-source");
    settings.audio.enabled = false;
    const auto out = dir / "stills.mp4";
    const auto result = ExportJob(renderedRequest(project, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 60);
    const Comparison c = compareWithPreview(project, out);
    CHECK(c.frames == 60);
    CHECK(c.timeErrors == 0);
    CHECK(c.minPsnr >= 40.0);
    // In the picture at 1.5 s: the PNG's opaque half is red, its clear half shows the JPEG, and the video around them.
    exporttest::PreviewRenderer preview(project);
    const auto rgba = preview.render(ms(1500), framesToTicks(1, kRate));
    REQUIRE(rgba.size() == static_cast<std::size_t>(kWidth) * kHeight * 4);
    const auto at = [&](int x, int y) { return &rgba[(static_cast<std::size_t>(y) * kWidth + x) * 4]; };
    const std::uint8_t* red = at(400, 360);
    CHECK(red[0] > 230);
    CHECK(red[1] < 30);
    CHECK(red[2] < 30);
    const std::uint8_t* clear = at(880, 360);
    CHECK_FALSE((clear[0] > 230 && clear[1] < 30 && clear[2] < 30));
    metric(std::format("stills: export == preview min PSNR {:.2f} dB over {} frames", c.minPsnr, c.frames));
}

TEST_CASE("Rendered export uses originals only: never the proxy; an offline original fails",
          "[export][render][media][gpu]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("rendered_originals");
    TestProject project = motionTimeline(kWidth, kHeight, kRate);
    const MediaId prores = project.state.media[1]->id; // V2
    const std::string proxyPath = (mediaDir() / "h264_422_10bit.mp4").generic_string();
    project.editMedia(prores, [&](MediaItem& m) { m.locator.proxy = ProxyMedia{proxyPath, {}}; });
    project.editSequence([](Sequence& s) {
        s.markIn = ms(500);
        s.markOut = ms(1000);
    });

    ExportSettings settings = presetSettings("prores-422-hq");
    settings.range = RangeMode::InOut;
    settings.audio.enabled = false;
    const auto out = dir / "originals.mov";
    const auto result = ExportJob(renderedRequest(project, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    // Matches the render of the originals...
    const Comparison originals = compareWithPreview(project, out, ms(500));
    CHECK(originals.minPsnr >= 50.0);
    // ...and not the render of the proxy.
    TestProject proxied = project;
    proxied.editMedia(prores, [&](MediaItem& m) { m.locator.path = proxyPath; });
    const Comparison proxy = compareWithPreview(proxied, out, ms(500));
    metric(std::format("originals only: PSNR vs originals {:.2f} dB, vs the proxy {:.2f} dB",
                       originals.minPsnr, proxy.minPsnr));
    // The proxy only changes V2 (half size, 75 % opacity), so the whole-frame PSNR against its render stays
    // moderate; what matters is that the export is far closer to the originals.
    CHECK(proxy.minPsnr < 40.0);
    CHECK(originals.minPsnr - proxy.minPsnr >= 10.0);

    // A real proxy of the same clip (I.3: ProRes Proxy at 1/2 with its provenance) looks nearly the same, and
    // is still never used: the export matches the originals' render, not the proxy's.
    const auto made = transcodeProxy((mediaDir() / "prores_422.mov").generic_string(), dir / "prores_422.proxy.mov", {});
    REQUIRE(made);
    ProxyMedia real{(dir / "prores_422.proxy.mov").generic_string(),
                    {std::string(kProxyGenerator), describeProxySettings({}), {}, 1}};
    TestProject withReal = project;
    withReal.editMedia(prores, [&](MediaItem& m) { m.locator.proxy = real; });
    const auto realOut = dir / "originals_real_proxy.mov";
    const auto realResult = ExportJob(renderedRequest(withReal, settings, realOut)).run();
    INFO((realResult ? std::string() : realResult.error().message));
    REQUIRE(realResult.has_value());
    const Comparison realOriginals = compareWithPreview(withReal, realOut, ms(500));
    TestProject realProxied = withReal;
    realProxied.editMedia(prores, [&](MediaItem& m) { m.locator.path = real.path; });
    const Comparison realProxy = compareWithPreview(realProxied, realOut, ms(500));
    metric(std::format("originals only, real ProRes Proxy 1/2 attached: PSNR vs originals {:.2f} dB, vs the proxy {:.2f} dB",
                       realOriginals.minPsnr, realProxy.minPsnr));
    CHECK(realOriginals.minPsnr >= 50.0);
    CHECK(realOriginals.minPsnr - realProxy.minPsnr >= 5.0);

    // The original offline: an error naming it, even though the proxy exists.
    TestProject offline = project;
    offline.editMedia(prores, [](MediaItem& m) { m.locator.offline = true; });
    const auto failed = ExportJob(renderedRequest(offline, settings, dir / "offline.mov")).run();
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ErrorCode::NotFound);
    CHECK(failed.error().message.find("prores_422.mov") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(dir / "offline.mov"));
    CHECK(partialFiles(dir).empty());
}

TEST_CASE("Cancelling a rendered export mid-way leaves no partial file", "[export][render][media][gpu]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = motionTimeline(kWidth, kHeight, kRate);
    const auto dir = freshOutputDir("rendered_cancel");
    const auto out = dir / "cancelled.mov";
    {
        std::ofstream old(out, std::ios::binary);
        old << "previous export";
    }
    std::stop_source cancel;
    ExportRequest request = renderedRequest(project, presetSettings("prores-422-hq"), out);
    request.videoSource = cancellingAfter(renderedVideoSourceFactory(), 10, cancel); // cancel at frame 10
    ExportJob job(std::move(request));
    const auto result = job.run(cancel.get_token());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(job.progress().framesDone < 60);
    CHECK(partialFiles(dir).empty());
    CHECK(readFile(out) == "previous export");
}

TEST_CASE("Rendered export == preview with effects, a transition and a title", "[export][render][media][gpu][effects]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p(kWidth, kHeight, kRate, Ticks::fromSeconds(2));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    const MediaId prores = p.addMedia("prores_422.mov");
    const auto instance = [&](const char* id, std::vector<std::pair<std::string, double>> params) {
        EffectInstance e;
        e.id = p.state.ids.allocate<EffectInstanceId>();
        e.effect = id;
        for (const auto& [key, value] : params) {
            e.set(key, ParamValue(value));
        }
        return e;
    };
    // V1: A (blur + correction) cross-dissolves into B (crop + vignette) over 0.95..1.45 s, using both handles.
    Clip a = p.clip(h264, StreamKind::Video, Ticks{}, ms(1200), Ticks::fromSeconds(1));
    Clip b = p.clip(prores, StreamKind::Video, ms(1200), ms(800), ms(500));
    ClipProperties fit;
    fit.set(ParamId::Scale, static_cast<double>(kWidth) / 1920.0);
    for (Clip* c : {&a, &b}) {
        ClipContent content = c->content();
        content.properties = fit;
        c->setContent(std::move(content));
    }
    {
        ClipContent content = a.content();
        content.effects = {instance("wabcut.blur.gaussian", {{"radius", 9.0}}),
                           instance("wabcut.color.basic", {{"exposure", 0.5}, {"saturation", 150.0}})};
        a.setContent(std::move(content));
        content = b.content();
        content.effects = {instance("wabcut.crop", {{"left", 0.1}, {"bottom", 0.15}, {"feather", 16.0}}),
                           instance("wabcut.vignette", {{"amount", -2.0}})};
        b.setContent(std::move(content));
    }
    p.addTrack(TrackKind::Video, {a, b});
    p.editSequence([&](Sequence& s) {
        Track track = *s.videoTracks.front();
        Transition dissolve;
        dissolve.id = p.state.ids.allocate<TransitionId>();
        dissolve.outgoing = a.id;
        dissolve.incoming = b.id;
        dissolve.before = ms(250);
        dissolve.after = ms(250);
        track.transitions = {dissolve};
        s.videoTracks.front() = std::make_shared<const Track>(std::move(track));
    });
    // V2: a title (C.4) with a stroke and its style's shadow, Thai included.
    p.addTrack(TrackKind::Video, {});
    const TrackId v2 = p.seq().videoTracks[1]->id;
    auto titled = edit::addTitle(p.state, p.sequence, v2, Ticks{}, Ticks::fromSeconds(2),
                                 "WABCUT \xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5",
                                 {{"size", ParamValue(96.0)},
                                  {"stroke.width", ParamValue(4.0)},
                                  {"shadow.opacity", ParamValue(0.7)},
                                  {"shadow.distance", ParamValue(8.0)}});
    INFO((titled ? std::string() : titled.error().message));
    REQUIRE(titled);
    p.state = std::move(titled->state);

    const auto dir = freshOutputDir("rendered-finishing");
    ExportSettings settings = presetSettings("match-source");
    settings.video.quality = 12;
    const auto out = dir / "finishing.mp4";
    const auto result = ExportJob(renderedRequest(p, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 60);

    const Comparison c = compareWithPreview(p, out);
    metric(std::format("rendered effects+transition+title: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames",
                       c.minPsnr, c.meanPsnr(), c.frames));
    CHECK(c.frames == 60);
    CHECK(c.timeErrors == 0);
    CHECK(c.minPsnr >= 45.0);
}

// 0.1.1: the Ultra Key on the GPU (C.15 keyer). V2's colour bars keyed at their green over V1, with matte cleanup and
// spill suppression: the export draws the key exactly as the preview does.
TEST_CASE("Rendered export == preview with an Ultra Key over a background (0.1.1)", "[export][render][media][gpu][effects]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    const MediaId prores = p.addMedia("prores_422.mov");
    ClipProperties fit;
    fit.set(ParamId::Scale, static_cast<double>(kWidth) / 1920.0);
    Clip background = p.clip(prores, StreamKind::Video, Ticks{}, Ticks::fromSeconds(1), Ticks{});
    Clip keyed = p.clip(h264, StreamKind::Video, Ticks{}, Ticks::fromSeconds(1), Ticks{});
    for (Clip* c : {&background, &keyed}) {
        ClipContent content = c->content();
        content.properties = fit;
        c->setContent(std::move(content));
    }
    EffectInstance key;
    key.id = p.state.ids.allocate<EffectInstanceId>();
    key.effect = "wabcut.key.ultra";
    for (const auto& [name, value] : std::vector<std::pair<std::string, double>>{
             {"keyColor.r", 0.0}, {"keyColor.g", 0.75}, {"keyColor.b", 0.0}, {"choke", 20.0}, {"soften", 30.0},
             {"spill", 70.0}, {"hue", 15.0}}) {
        key.set(name, ParamValue(value));
    }
    {
        ClipContent content = keyed.content();
        content.effects = {key};
        keyed.setContent(std::move(content));
    }
    p.addTrack(TrackKind::Video, {background});
    p.addTrack(TrackKind::Video, {keyed});

    const auto dir = freshOutputDir("rendered-ultra-key");
    ExportSettings settings = presetSettings("match-source");
    settings.video.quality = 12;
    const auto out = dir / "ultra_key.mp4";
    const auto result = ExportJob(renderedRequest(p, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 30);

    const Comparison c = compareWithPreview(p, out);
    metric(std::format("rendered Ultra Key over a background: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames",
                       c.minPsnr, c.meanPsnr(), c.frames));
    CHECK(c.frames == 30);
    CHECK(c.timeErrors == 0);
    CHECK(c.minPsnr >= 45.0);
}

// 0.1.2: the shader blend modes (the canvas pass split at each one). V2 in Overlay, V3 in Difference and V4 in Hue over
// V1: the export draws them exactly as the preview does. (Dissolve's per-pixel noise would measure H.264 loss here; the
// render tests compare it uncompressed.)
TEST_CASE("Rendered export == preview with shader blend modes over a background (0.1.2)", "[export][render][media][gpu][effects]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    const MediaId prores = p.addMedia("prores_422.mov");
    ClipProperties fit;
    fit.set(ParamId::Scale, static_cast<double>(kWidth) / 1920.0);
    const auto clipOf = [&](MediaId media, BlendMode mode, double opacity) {
        Clip c = p.clip(media, StreamKind::Video, Ticks{}, Ticks::fromSeconds(1), Ticks{});
        ClipContent content = c.content();
        content.properties = fit;
        content.properties.set(ParamId::Opacity, opacity);
        content.blendMode = mode;
        c.setContent(std::move(content));
        return c;
    };
    p.addTrack(TrackKind::Video, {clipOf(prores, BlendMode::Normal, 1.0)});
    p.addTrack(TrackKind::Video, {clipOf(h264, BlendMode::Overlay, 0.8)});
    p.addTrack(TrackKind::Video, {clipOf(prores, BlendMode::Difference, 0.5)});
    p.addTrack(TrackKind::Video, {clipOf(h264, BlendMode::Hue, 0.6)});

    const auto dir = freshOutputDir("rendered-blend-modes");
    ExportSettings settings = presetSettings("match-source");
    settings.video.quality = 12;
    const auto out = dir / "blend_modes.mp4";
    const auto result = ExportJob(renderedRequest(p, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 30);

    const Comparison c = compareWithPreview(p, out);
    metric(std::format("rendered shader blend modes: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames",
                       c.minPsnr, c.meanPsnr(), c.frames));
    CHECK(c.frames == 30);
    CHECK(c.timeErrors == 0);
    CHECK(c.minPsnr >= 45.0);
}

// 0.1.2: the Track Matte Key on the GPU. V3 shows through V2's luma (V2's output is off: it is only the matte) over V1.
TEST_CASE("Rendered export == preview with a Track Matte Key (0.1.2)", "[export][render][media][gpu][effects]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    const MediaId prores = p.addMedia("prores_422.mov");
    ClipProperties fit;
    fit.set(ParamId::Scale, static_cast<double>(kWidth) / 1920.0);
    const auto clipOf = [&](MediaId media, std::vector<EffectInstance> effects) {
        Clip c = p.clip(media, StreamKind::Video, Ticks{}, Ticks::fromSeconds(1), Ticks{});
        ClipContent content = c.content();
        content.properties = fit;
        content.effects = std::move(effects);
        c.setContent(std::move(content));
        return c;
    };
    EffectInstance key;
    key.id = p.state.ids.allocate<EffectInstanceId>();
    key.effect = "wabcut.key.trackMatte";
    key.set("matteTrack", ParamValue(2.0));
    key.set("compositeUsing", ParamValue(1.0));
    p.addTrack(TrackKind::Video, {clipOf(prores, {})});
    p.addTrack(TrackKind::Video, {clipOf(h264, {})});
    p.addTrack(TrackKind::Video, {clipOf(prores, {key})});
    p.editSequence([&](Sequence& s) {
        Track matte = *s.videoTracks[1];
        matte.enabled = false;
        s.videoTracks[1] = std::make_shared<const Track>(std::move(matte));
    });

    const auto dir = freshOutputDir("rendered-track-matte");
    ExportSettings settings = presetSettings("match-source");
    settings.video.quality = 12;
    const auto out = dir / "track_matte.mp4";
    const auto result = ExportJob(renderedRequest(p, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 30);

    const Comparison c = compareWithPreview(p, out);
    metric(std::format("rendered Track Matte Key: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames", c.minPsnr,
                       c.meanPsnr(), c.frames));
    CHECK(c.frames == 30);
    CHECK(c.timeErrors == 0);
    CHECK(c.minPsnr >= 45.0);
}

// Codex wave-4 P1-3: a bleeped word is never burned into the picture. A1 (ramp_48k.wav from 1 s) is bleeped at media
// 1.6..1.9 s, which plays at 0.6..0.9 s; the cue there has no word timings, so it is burned in as "[bleep]" whole. The
// export matches the preview (which masks it the same way), the preview of a project with "[bleep]" typed in, and not
// the picture without the redaction (the words). Failed before the fix (the words were burned in).
TEST_CASE("Rendered export burns in a bleeped cue as [bleep]", "[export][render][media][gpu][captions][redaction]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("captions-bleeped");
    TestProject project = motionTimeline(kWidth, kHeight, kRate);
    CaptionTrack track;
    track.id = project.state.ids.allocate<CaptionTrackId>();
    track.name = "English";
    track.enabled = true;
    track.cues = {{ms(500), ms(500), "call Alice now"}};
    project.editSequence([&](Sequence& s) {
        s.captionTracks = {track};
        s.markIn = ms(500);
        s.markOut = ms(1000);
    });
    Redaction r;
    r.id = project.state.ids.allocate<RedactionId>();
    r.media = project.seq().audioTracks.front()->clips.begin()->media;
    r.source = TimeRange::fromStartEnd(Ticks::fromSeconds(1), Ticks::fromSeconds(2));
    r.audio = RedactionAudio::Bleep;
    r.audioRanges = {TimeRange::fromStartEnd(ms(1600), ms(1900))};
    project.state.redactions = {r};

    ExportSettings settings = presetSettings("prores-422-hq");
    settings.range = RangeMode::InOut;
    settings.audio.enabled = false;
    settings.captions.mode = CaptionExport::BurnIn;
    settings.captions.style = render::CaptionStyle{}; // an explicit style (the bottom), as the app passes it
    const auto out = dir / "bleeped.mov";
    const auto result = ExportJob(renderedRequest(project, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());

    playback::CaptionSettings preview;
    preview.style = settings.captions.style;
    TestProject typed = project;
    typed.state.redactions.clear();
    typed.editSequence([](Sequence& s) { s.captionTracks.front().cues.front().text = "[bleep]"; });
    TestProject unredacted = project;
    unredacted.state.redactions.clear();
    const Comparison asPreview = compareWithPreview(project, out, ms(500), 0, 0, preview);
    const Comparison asTyped = compareWithPreview(typed, out, ms(500), 0, 0, preview);
    const Comparison asWords = compareWithPreview(unredacted, out, ms(500), 0, 0, preview);
    metric(std::format("bleeped cue burned in: PSNR vs the preview min {:.2f} dB; vs \"[bleep]\" typed in min {:.2f} dB; "
                       "vs the words min {:.2f} dB",
                       asPreview.minPsnr, asTyped.minPsnr, asWords.minPsnr));
    CHECK(asPreview.frames == 15);
    CHECK(asPreview.minPsnr >= 50.0); // preview == export
    CHECK(asTyped.minPsnr >= 50.0);
    CHECK(asWords.minPsnr < 40.0); // the words are not in the picture
}

TEST_CASE("Rendered export burns in captions exactly as the preview draws them; SRT and VTT sidecars",
          "[export][render][media][gpu][captions]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("captions");
    TestProject project = motionTimeline(kWidth, kHeight, kRate);
    CaptionTrack track;
    track.id = project.state.ids.allocate<CaptionTrackId>();
    track.name = "English";
    track.language = "en";
    track.enabled = true;
    track.cues = {{ms(100), ms(500), "Burned in, frame for frame"}, {ms(700), ms(600), "สวัสดีครับ ยินดีต้อนรับ"}};
    project.editSequence([&](Sequence& s) {
        s.captionTracks = {track};
        s.markIn = ms(500);
        s.markOut = ms(1000);
    });

    ExportSettings settings = presetSettings("prores-422-hq");
    settings.range = RangeMode::InOut;
    settings.audio.enabled = false;
    settings.captions.mode = CaptionExport::BurnIn;
    settings.captions.style = render::CaptionStyle{}; // an explicit style (the bottom), as the app passes it
    settings.captions.karaoke = true;
    const auto out = dir / "burned.mov";
    const auto result = ExportJob(renderedRequest(project, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->captions.empty()); // burned in: no sidecar
    playback::CaptionSettings preview;
    preview.style = settings.captions.style;
    preview.karaoke = true;
    const Comparison withCaptions = compareWithPreview(project, out, ms(500), 0, 0, preview);
    const Comparison without = compareWithPreview(project, out, ms(500));
    metric(std::format("captions burned in: PSNR vs the preview with captions min {:.2f} / mean {:.2f} dB; vs the preview "
                       "without captions min {:.2f} dB",
                       withCaptions.minPsnr, withCaptions.meanPsnr(), without.minPsnr));
    CHECK(withCaptions.frames == 15); // 0.5 s at 30 fps
    CHECK(withCaptions.minPsnr >= 50.0);
    CHECK(without.minPsnr < 40.0); // the captions are really in the picture

    // A sidecar instead: next to the file, cue times relative to the In point, cut to the range.
    settings.captions.mode = CaptionExport::Srt;
    const auto srtOut = dir / "sidecar.mov";
    const auto srt = ExportJob(renderedRequest(project, settings, srtOut)).run();
    INFO((srt ? std::string() : srt.error().message));
    REQUIRE(srt.has_value());
    CHECK(srt->captions == std::filesystem::path(dir / "sidecar.srt"));
    std::ifstream in(dir / "sidecar.srt", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == "1\n00:00:00,000 --> 00:00:00,100\nBurned in, frame for frame\n\n"
                  "2\n00:00:00,200 --> 00:00:00,500\nสวัสดีครับ ยินดีต้อนรับ\n\n");
    const auto check = ExportJob(renderedRequest(project, settings, dir / "sidecar2.mov")).run();
    REQUIRE(check.has_value());
    CHECK(partialFiles(dir).empty());

    CaptionTrack multi = track;
    multi.cues = {{ms(0), ms(1500), "Line one\nLine two"}};
    CHECK(captionSidecarText(multi, {ms(500), ms(500)}, CaptionExport::Vtt) ==
          "WEBVTT\n\n00:00:00.000 --> 00:00:00.500\nLine one\nLine two\n\n");
    CHECK(captionSidecarText(multi, {ms(2000), ms(500)}, CaptionExport::Srt).empty());
    // The track: the chosen one when enabled, else the first enabled one.
    Sequence seq = project.seq();
    CaptionTrack off = track;
    off.id = CaptionTrackId{track.id.value + 1000};
    off.enabled = false;
    seq.captionTracks = {off, track};
    CaptionExportSettings choose;
    choose.track = off.id.value;
    CHECK(captionTrackToExport(seq, choose)->id == track.id);
}

TEST_CASE("Rendered export == preview with a full colour grade (I.5): every stage, LUTs from the project",
          "[export][render][media][gpu][color]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    const MediaId prores = p.addMedia("prores_422.mov");
    // A look LUT as a project asset (a file), plus a built-in input LUT.
    const auto dir = freshOutputDir("rendered-grade");
    {
        std::ofstream lut(dir / "Look.cube", std::ios::binary | std::ios::trunc);
        lut << "TITLE \"export test look\"\nLUT_3D_SIZE 9\n";
        for (int b = 0; b < 9; ++b) {
            for (int g = 0; g < 9; ++g) {
                for (int r = 0; r < 9; ++r) {
                    const double R = r / 8.0, G = g / 8.0, B = b / 8.0;
                    lut << std::format("{:.6f} {:.6f} {:.6f}\n", std::pow(R, 0.8), 0.1 + 0.8 * G * G + 0.1 * B, 0.9 * B + 0.1 * R * G);
                }
            }
        }
    }
    ProjectAsset asset;
    asset.kind = std::string(kAssetKindLut);
    asset.name = "Look.cube";
    asset.locator.path = pathToUtf8(std::filesystem::absolute(dir / "Look.cube"));
    auto added = edit::addAsset(p.state, asset);
    REQUIRE(added);
    p.state = std::move(added->state);
    const AssetId look = added->id;

    const auto grade = [&](std::vector<std::pair<std::string, double>> params, std::vector<EffectData> data) {
        EffectInstance e;
        e.id = p.state.ids.allocate<EffectInstanceId>();
        e.effect = "wabcut.color.grade";
        for (const auto& [key, value] : params) {
            e.set(key, ParamValue(value));
        }
        for (EffectData& d : data) {
            e.setData(std::move(d));
        }
        return e;
    };
    Clip a = p.clip(h264, StreamKind::Video, Ticks{}, ms(500), Ticks::fromSeconds(1));
    Clip b = p.clip(prores, StreamKind::Video, ms(500), ms(500), ms(200));
    ClipProperties fit;
    fit.set(ParamId::Scale, static_cast<double>(kWidth) / 1920.0);
    {
        ClipContent content = a.content();
        content.properties = fit;
        content.effects = {grade({{"basic.temperature", 30.0}, {"basic.exposure", 0.4}, {"basic.contrast", 20.0},
                                  {"basic.highlights", -40.0}, {"basic.shadows", 30.0}, {"basic.saturation", 120.0},
                                  {"basic.vibrance", 30.0}, {"creative.lookIntensity", 80.0}, {"creative.fadedFilm", 15.0},
                                  {"creative.shadowTint.hue", 200.0}, {"creative.shadowTint.amount", 40.0},
                                  {"wheels.lift.blue", 0.02}, {"wheels.gain.red", 1.05}, {"wheels.saturation", 1.1},
                                  {"hsl.hue.center", 30.0}, {"hsl.hue.width", 60.0}, {"hsl.hue.softness", 20.0},
                                  {"hsl.denoise", 15.0}, {"hsl.blur", 4.0}, {"hsl.saturation", 140.0},
                                  {"vignette.amount", -1.0}},
                                 {EffectData{"creative.look", assetReference(look)},
                                  EffectData{"curves.master", std::vector<EffectPoint>{{0, 0.02}, {0.5, 0.55}, {1, 0.98}}},
                                  EffectData{"curves.hueSat", std::vector<EffectPoint>{{0.0, 0.5}, {0.33, 0.8}, {0.66, 0.4}}}})};
        a.setContent(std::move(content));
        content = b.content();
        content.properties = fit;
        content.effects = {grade({{"basic.tint", -25.0}, {"basic.whites", 30.0}, {"creative.saturation", 70.0}},
                                 {EffectData{"basic.inputLut", std::string("builtin:vlog-vgamut-to-rec709")}})};
        b.setContent(std::move(content));
    }
    p.addTrack(TrackKind::Video, {a, b});

    ExportSettings settings = presetSettings("match-source");
    settings.video.quality = 12;
    const auto out = dir / "graded.mp4";
    const auto result = ExportJob(renderedRequest(p, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 30);

    const Comparison c = compareWithPreview(p, out);
    metric(std::format("rendered full colour grade: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames", c.minPsnr,
                       c.meanPsnr(), c.frames));
    CHECK(c.frames == 30);
    CHECK(c.timeErrors == 0);
    CHECK(c.minPsnr >= 45.0);
}

TEST_CASE("Rendered export redacts every use of the marked source: picture under burned-in captions, and the bleep (I.6)",
          "[export][render][media][gpu][redaction]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p(kWidth, kHeight, kRate, Ticks::fromSeconds(2));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    // V1: a trim (media 1.0 .. 2.0 s), the same media at 2x (0.8 .. 1.8 s), and a use the redaction excepts.
    Clip a = p.clip(h264, StreamKind::Video, Ticks{}, ms(1000), ms(1000));
    Clip b = p.clip(h264, StreamKind::Video, ms(1000), ms(500), ms(800));
    b.speed = {2, 1};
    Clip c = p.clip(h264, StreamKind::Video, ms(1500), ms(500), ms(1200));
    ClipProperties fit;
    fit.set(ParamId::Scale, static_cast<double>(kWidth) / 1920.0);
    for (Clip* clip : {&a, &b, &c}) {
        ClipContent content = clip->content();
        content.properties = fit;
        clip->setContent(std::move(content));
    }
    bool remapped = false;
    SECTION("constant speed reuse") {}
    SECTION("remapped hold and reverse reuse") {
        remapped = true;
        b.speed = {1, 1};
        ClipContent content = b.content();
        Curve curve;
        curve.keys = {{ms(800), double(ms(1400).value)}, {ms(1000), double(ms(1400).value)},
                      {ms(1300), double(ms(1000).value)}};
        content.timeRemap = curve;
        b.setContent(std::move(content));
    }
    p.addTrack(TrackKind::Video, {a, b, c});
    p.addTrack(TrackKind::Audio, {p.clip(h264, StreamKind::Audio, Ticks{}, ms(1000), ms(1000))});
    // Marked once, in media time 1.2 .. 1.6 s: a magenta disc in the middle, bleeped.
    Redaction r;
    r.media = h264;
    r.source = TimeRange{ms(1200), ms(400)};
    r.style = RedactionStyle::Solid;
    r.colorR = 1.0;
    r.colorB = 1.0;
    r.region.shape = MaskShape::Ellipse;
    r.region.set("size.x", 0.3);
    r.region.set("size.y", 0.3);
    r.audio = RedactionAudio::Bleep;
    r.exceptions = {c.id};
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);
    CaptionTrack track;
    track.id = p.state.ids.allocate<CaptionTrackId>();
    track.name = "English";
    track.language = "en";
    track.enabled = true;
    track.cues = {{ms(0), ms(2000), "Burned in over a redacted picture"}};
    p.editSequence([&](Sequence& s) { s.captionTracks = {track}; });

    const auto dir = freshOutputDir(remapped ? "redaction-remap" : "redaction");
    ExportSettings settings = presetSettings("prores-422-hq");
    settings.captions.mode = CaptionExport::BurnIn;
    settings.captions.style = render::CaptionStyle{}; // an explicit style (the bottom), as the app passes it
    const auto out = dir / "redacted.mov";
    const auto result = ExportJob(renderedRequest(p, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 60);

    // Frames 6..17 (A shows media 1.2 .. 1.6 s) and 36..41 (B at 2x) are redacted; C, though marked, is excepted.
    const auto magentaAt = [](const YuvFrame& f, int x, int y) {
        const int shift = f.bits - 8;
        const int cx = x * f.chromaWidth / f.width;
        const int cy = y * f.chromaHeight / f.height;
        const int Y = f.y(x, y) >> shift;
        const int U = f.planes[1][static_cast<std::size_t>(cy) * f.chromaWidth + cx] >> shift;
        const int V = f.planes[2][static_cast<std::size_t>(cy) * f.chromaWidth + cx] >> shift;
        return std::abs(Y - 78) <= 8 && std::abs(U - 214) <= 10 && std::abs(V - 229) <= 10;
    };
    int frame = 0;
    int wrong = 0;
    std::string error;
    REQUIRE(decodeVideo(
        out,
        [&](const YuvFrame& f) {
            const bool expected = (frame >= 6 && frame < 18) ||
                                  (remapped ? (frame >= 30 && frame < 41) : (frame >= 36 && frame < 42));
            if (magentaAt(f, f.width / 2, f.height / 2) != expected) {
                ++wrong;
                UNSCOPED_INFO("frame " << frame << ": expected " << (expected ? "redacted" : "clear"));
            }
            ++frame;
        },
        &error));
    CHECK(frame == 60);
    CHECK(wrong == 0);
    playback::CaptionSettings preview;
    preview.style = settings.captions.style;
    const Comparison withCaptions = compareWithPreview(p, out, Ticks{}, 0, 0, preview);
    metric(std::format("redaction + burned-in captions: preview==export PSNR min {:.2f} / mean {:.2f} dB, {} frames",
                       withCaptions.minPsnr, withCaptions.meanPsnr(), withCaptions.frames));
    CHECK(withCaptions.minPsnr >= 45.0);

    // The sound of media 1.2 .. 1.6 s (timeline 0.2 .. 0.6 s) is the bleep, sample for sample (24-bit PCM).
    const auto audio = decodeAudio(out, &error);
    INFO(error);
    REQUIRE(audio);
    REQUIRE(audio->sampleRate == 48000);
    const auto ch = static_cast<std::size_t>(audio->channels);
    double worst = 0.0;
    for (std::int64_t k = 9600; k < 28800; ++k) {
        const float bleep = playback::AudioRedactions::bleepAt(ms(1000) + Ticks{k * (kTicksPerSecond / 48000)});
        worst = std::max(worst, static_cast<double>(std::fabs(audio->samples[static_cast<std::size_t>(k) * ch] - bleep)));
    }
    metric(std::format("redaction bleep in the export: largest difference {:.2e} over 0.4 s", worst));
    CHECK(worst < 1e-5);
}

// ---- C.8 nested sequences and multicam (I.8) ----------------------------------------------------------------------

namespace {

/// The same media's first video stream on a new video track of a hand-made sequence (for multicam sources).
Clip mediaClip(ProjectState& state, const MediaItem& item, Ticks start, Ticks duration, Ticks sourceIn) {
    Clip c;
    c.id = state.ids.allocate<ClipId>();
    c.media = item.id;
    c.stream = {StreamKind::Video, 0};
    c.timelineRange = {start, duration};
    c.sourceIn = sourceIn;
    return c;
}

/// A multicam source of `angles` (media, sync offset) at `canvas`: angle i is media i on V(i+1) from its offset.
SequenceId addMulticam(ProjectState& state, const std::vector<std::pair<MediaId, Ticks>>& angles, int width, int height,
                       Rate rate) {
    auto seq = std::make_shared<Sequence>();
    seq->id = state.ids.allocate<SequenceId>();
    seq->name = "Multicam";
    seq->frameRate = rate;
    seq->width = width;
    seq->height = height;
    MulticamInfo info;
    info.sync = MulticamSync::Manual;
    for (std::size_t i = 0; i < angles.size(); ++i) {
        const MediaItem& item = *state.findMedia(angles[i].first);
        Track track;
        track.id = state.ids.allocate<TrackId>();
        const Ticks length = item.info.video.front().duration;
        track.clips.push_back(mediaClip(state, item, angles[i].second, length, Ticks{}));
        seq->duration = std::max(seq->duration, angles[i].second + length);
        info.angles.push_back({"Angle " + std::to_string(i + 1), track.id, {}, angles[i].second});
        seq->videoTracks.push_back(std::make_shared<const Track>(std::move(track)));
    }
    seq->multicam = std::move(info);
    const SequenceId id = seq->id;
    state.sequences.push_back(std::move(seq));
    return id;
}

/// Contiguous multicam clips of `source` playing it from `sourceIn`, cut at `cuts` (start, angle).
std::vector<Clip> multicamCuts(ProjectState& state, SequenceId source, Ticks start, Ticks end, Ticks sourceIn,
                               const std::vector<std::pair<Ticks, std::size_t>>& cuts) {
    std::vector<Clip> out;
    for (std::size_t k = 0; k < cuts.size(); ++k) {
        const Ticks from = start + cuts[k].first;
        const Ticks to = k + 1 < cuts.size() ? start + cuts[k + 1].first : end;
        Clip c;
        c.id = state.ids.allocate<ClipId>();
        c.sequence = source;
        c.angles = AngleSet::of(cuts[k].second);
        c.stream = {StreamKind::Video, 0};
        c.timelineRange = TimeRange::fromStartEnd(from, to);
        c.sourceIn = sourceIn + cuts[k].first;
        out.push_back(c);
    }
    return out;
}

ClipId firstVideoClip(const ProjectState& state, SequenceId seq) {
    for (const TrackPtr& t : state.findSequence(seq)->videoTracks) {
        if (!t->clips.empty()) {
            return t->clips.front().id;
        }
    }
    FAIL("no video clip");
    return {};
}

} // namespace

TEST_CASE("Export: an offline file in a multicam angle never cut to does not stop it; in an angle used it does (review "
          "wave 5 E5-P2-10)",
          "[export][multicam][offline]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    const MediaId camera = p.seq().videoTracks.front()->clips.begin()->media;
    // The second camera's card was never relinked: its file is gone.
    MediaItem lost = *p.state.findMedia(camera);
    lost.name = "Camera 2";
    lost.locator.path = "C:/nowhere/camera2_card/A001.mov";
    lost.locator.offline = true;
    auto added = edit::addMedia(p.state, lost);
    REQUIRE(added);
    p.state = added->state;
    const SequenceId multicam = addMulticam(p.state, {{camera, Ticks{}}, {added->id, Ticks{}}}, 640, 360, Rate{30, 1});
    const auto exportWith = [&](std::size_t angle, const char* name) {
        TestProject q = p;
        q.editSequence([](Sequence& s) {
            s.videoTracks.clear();
            s.audioTracks.clear();
        });
        q.addTrack(TrackKind::Video, multicamCuts(q.state, multicam, Ticks{}, Ticks::fromSeconds(1), Ticks{}, {{Ticks{}, angle}}));
        ExportSettings settings = presetSettings("youtube-1080p");
        settings.audio.enabled = false;
        return ExportJob(makeRequest(q, settings, freshOutputDir("multicam-offline") / name)).run();
    };
    const auto used = exportWith(0, "angle1.mp4");
    INFO((used ? std::string() : used.error().message));
    CHECK(used.has_value()); // it refused before: "Camera 2 is offline"
    const auto refused = exportWith(1, "angle2.mp4");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("Camera 2") != std::string::npos);
}

TEST_CASE("Rendered export == preview with a nested clip (three layers inside) and a four-angle multicam clip (C.8)",
          "[export][render][media][gpu][nested]") {
    if (!haveMotionTimelineMedia() || !haveMedia("hevc_main10_420.mp4")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project = motionTimeline(kWidth, kHeight, kRate);
    // Nest the three video layers (and the disabled clip): one nested clip on V1, then moved, turned and faded.
    std::vector<ClipId> video;
    for (const TrackPtr& t : project.seq().videoTracks) {
        for (const Clip& c : t->clips) {
            video.push_back(c.id);
        }
    }
    auto nested = edit::nestClips(project.state, project.sequence, video, "Three layers");
    INFO((nested ? std::string() : nested.error().message));
    REQUIRE(nested);
    project.state = nested->edit.state;
    const ClipId nestClip = nested->clips.front();
    for (const auto& [param, value] : {std::pair{ParamId::Scale, 0.8}, std::pair{ParamId::Rotation, 6.0},
                                       std::pair{ParamId::PositionX, -60.0}, std::pair{ParamId::Opacity, 0.9}}) {
        auto set = edit::setParameter(project.state, project.sequence, nestClip, edit::ParamRef::of(param), ParamValue(value));
        REQUIRE(set);
        project.state = set->state;
    }
    // A four-angle multicam source (four different files, synced by hand), cut every quarter second, as a
    // picture-in-picture on top from 1 s.
    const MediaId a = project.addMedia("h264_1080p60_aac.mp4");
    const MediaId b = project.addMedia("prores_422.mov");
    const MediaId c = project.addMedia("hevc_main10_420.mp4");
    const MediaId d = project.addMedia("h264_422_10bit.mp4");
    const SequenceId multicam = addMulticam(project.state, {{a, Ticks{}}, {b, ms(100)}, {c, ms(200)}, {d, ms(300)}}, 1280, 720,
                                            Rate{30, 1});
    std::vector<Clip> cuts = multicamCuts(project.state, multicam, ms(1000), ms(2000), ms(400),
                                          {{Ticks{}, 0}, {ms(250), 1}, {ms(500), 2}, {ms(750), 3}});
    ClipProperties pip;
    pip.set(ParamId::Scale, 0.4);
    pip.set(ParamId::PositionX, 330.0);
    pip.set(ParamId::PositionY, 170.0);
    for (Clip& clip : cuts) {
        clip.setProperties(pip);
    }
    project.addTrack(TrackKind::Video, cuts);

    const auto dir = freshOutputDir("rendered-nested");
    const auto out = dir / "nested.mov";
    const auto result = ExportJob(renderedRequest(project, presetSettings("prores-422-hq"), out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 60);
    const Comparison cmp = compareWithPreview(project, out);
    metric(std::format("nested + 4-angle multicam: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames", cmp.minPsnr,
                       cmp.meanPsnr(), cmp.frames));
    CHECK(cmp.frames == 60);
    CHECK(cmp.timeErrors == 0);
    CHECK(cmp.minPsnr >= 50.0);

    // The nest shows what the clips showed before nesting (placement aside): the same picture as the original
    // timeline when the nested clip is left at its defaults.
    TestProject plain = motionTimeline(kWidth, kHeight, kRate);
    TestProject nestedPlain = plain;
    std::vector<ClipId> plainVideo;
    for (const TrackPtr& t : plain.seq().videoTracks) {
        for (const Clip& clip : t->clips) {
            plainVideo.push_back(clip.id);
        }
    }
    auto again = edit::nestClips(nestedPlain.state, nestedPlain.sequence, plainVideo, "Same");
    REQUIRE(again);
    nestedPlain.state = again->edit.state;
    exporttest::PreviewRenderer before(plain);
    exporttest::PreviewRenderer after(nestedPlain);
    const Ticks period = framesToTicks(1, kRate);
    double worst = 1e9;
    for (int k = 0; k < 60; k += 7) {
        const auto x = before.render(framesToTicks(k, kRate), period);
        const auto y = after.render(framesToTicks(k, kRate), period);
        double sum = 0.0;
        for (std::size_t i = 0; i < x.size(); i += 4) {
            for (std::size_t ch = 0; ch < 3; ++ch) {
                const double diff = static_cast<double>(x[i + ch]) - y[i + ch];
                sum += diff * diff;
            }
        }
        const double mse = sum / (static_cast<double>(x.size()) / 4 * 3);
        worst = std::min(worst, mse == 0.0 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / mse));
    }
    metric(std::format("nested at defaults vs the same layers unnested: PSNR min {:.2f} dB", worst));
    CHECK(worst >= 45.0);
}

TEST_CASE("Rendered export == preview with track effects, a master stack and a nested sequence's own caption (I.9)",
          "[export][render][media][gpu][nested][effects]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project = motionTimeline(kWidth, kHeight, kRate);
    std::vector<ClipId> video;
    for (const TrackPtr& t : project.seq().videoTracks) {
        for (const Clip& c : t->clips) {
            video.push_back(c.id);
        }
    }
    auto nested = edit::nestClips(project.state, project.sequence, video, "With a caption");
    REQUIRE(nested);
    project.state = nested->edit.state;
    const ClipId nestClip = nested->clips.front();
    auto smaller = edit::setParameter(project.state, project.sequence, nestClip, edit::ParamRef::of(ParamId::Scale), ParamValue(0.7));
    REQUIRE(smaller);
    project.state = smaller->state;
    const auto effect = [&](const char* id, std::initializer_list<std::pair<const char*, double>> params) {
        EffectInstance e;
        e.id = project.state.ids.allocate<EffectInstanceId>();
        e.effect = id;
        for (const auto& [key, value] : params) {
            e.set(key, ParamValue(value));
        }
        return e;
    };
    // The nested sequence's own caption, in its track's style (top, yellow, outlined).
    {
        auto inner = std::make_shared<Sequence>(*project.state.findSequence(nested->nested));
        CaptionTrack track;
        track.id = project.state.ids.allocate<CaptionTrackId>();
        track.enabled = true;
        track.style.placement = CaptionPlacement::Top;
        track.style.fill = {1.0, 0.85, 0.1};
        track.cues = {CaptionCue{Ticks{}, Ticks::fromSeconds(2), "Inside the nest"}};
        inner->captionTracks = {track};
        project.state = project.state.withSequence(inner);
    }
    // V1's track effect (a correction) and the master stack (a vignette), at the top level.
    project.editSequence([&](Sequence& s) {
        Track v1 = *s.videoTracks.front();
        v1.effects = {effect("wabcut.color.basic", {{"exposure", 0.6}, {"contrast", 20.0}, {"saturation", 70.0}})};
        s.videoTracks.front() = std::make_shared<const Track>(std::move(v1));
        s.effects = {effect("wabcut.vignette", {{"amount", -40.0}, {"midpoint", 40.0}, {"roundness", 0.0}, {"feather", 60.0}})};
    });

    ExportSettings settings = presetSettings("prores-422-hq");
    settings.captions.mode = CaptionExport::BurnIn;
    const auto dir = freshOutputDir("rendered-track-effects");
    const auto out = dir / "track_effects.mov";
    const auto result = ExportJob(renderedRequest(project, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    const Comparison cmp = compareWithPreview(project, out, {}, 0, 0, playback::CaptionSettings{});
    metric(std::format("track + master effects + nested caption: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames",
                       cmp.minPsnr, cmp.meanPsnr(), cmp.frames));
    CHECK(cmp.timeErrors == 0);
    CHECK(cmp.minPsnr >= 50.0);

    // The effects and the nested caption show: different from the same timeline without them.
    exporttest::PreviewRenderer withAll(project);
    withAll.setCaptions(playback::CaptionSettings{});
    const auto shown = withAll.render(ms(500), framesToTicks(1, kRate));
    QImage(shown.data(), withAll.width(), withAll.height(), withAll.width() * 4, QImage::Format_RGBA8888)
        .save(QString::fromStdWString((dir / "nested_caption_and_effects.png").wstring()));
    TestProject bare = project;
    bare.editSequence([](Sequence& s) {
        Track v1 = *s.videoTracks.front();
        v1.effects.clear();
        s.videoTracks.front() = std::make_shared<const Track>(std::move(v1));
        s.effects.clear();
    });
    exporttest::PreviewRenderer without(bare);
    const auto plain = without.render(ms(500), framesToTicks(1, kRate));
    double diff = 0.0;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        diff += std::abs(static_cast<int>(plain[i]) - static_cast<int>(shown[i]));
    }
    CHECK(diff / static_cast<double>(plain.size()) > 3.0);
}

TEST_CASE("Rendered export == preview with a motion graphic over the picture (C.13 graphic clip, I.9)",
          "[export][render][media][gpu][graphics]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project = motionTimeline(kWidth, kHeight, kRate);
    project.addTrack(TrackKind::Video, {});
    // A lower third: a gradient bar with a dashed outline, a name with a background box, a star that spins, fading in.
    Graphic g;
    g.width = 1920;
    g.height = 1080;
    GraphicLayer bar;
    bar.id = 1;
    bar.kind = LayerKind::Shape;
    bar.shape.kind = ShapeKind::Rectangle;
    bar.shape.width = ParamValue(900.0);
    bar.shape.height = ParamValue(140.0);
    bar.shape.cornerRadius = ParamValue(24.0);
    bar.shape.fill.kind = PaintKind::Linear;
    bar.shape.fill.start = {-450.0, 0.0};
    bar.shape.fill.end = {450.0, 0.0};
    bar.shape.fill.stops = {GradientStop{0.0, GraphicColor{{0.1, 0.1, 0.4, 0.95}, {}}}, GradientStop{1.0, GraphicColor{{0.6, 0.2, 0.8, 0.6}, {}}}};
    bar.shape.stroke.enabled = true;
    bar.shape.stroke.width = ParamValue(4.0);
    bar.shape.stroke.dashes = {18.0, 10.0};
    bar.transform.x = ParamValue(700.0);
    bar.transform.y = ParamValue(880.0);
    Curve fade;
    fade.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{Ticks::fromSeconds(1), 100.0}};
    bar.transform.opacity = ParamValue(fade);
    GraphicLayer name;
    name.id = 2;
    name.kind = LayerKind::Text;
    name.transform.x = ParamValue(300.0);
    name.transform.y = ParamValue(840.0);
    TextRun run;
    run.text = "Avery Quinn";
    run.style.fontSize = 64.0;
    name.text.runs = {run};
    GraphicLayer star;
    star.id = 3;
    star.kind = LayerKind::Shape;
    star.shape.kind = ShapeKind::Star;
    star.shape.outerRadius = ParamValue(60.0);
    star.shape.innerRadius = ParamValue(26.0);
    star.shape.fill.color.rgba = {1.0, 0.8, 0.1, 1.0};
    star.transform.x = ParamValue(1100.0);
    star.transform.y = ParamValue(880.0);
    Curve spin;
    spin.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{Ticks::fromSeconds(2), 360.0}};
    star.transform.rotation = ParamValue(spin);
    g.layers = {bar, name, star};
    auto added = edit::addGraphic(project.state, project.sequence, project.seq().videoTracks.back()->id, Ticks{},
                                  Ticks::fromSeconds(2), g);
    REQUIRE(added);
    project.state = added->state;

    ExportSettings settings = presetSettings("prores-422-hq");
    const auto dir = freshOutputDir("rendered-graphic");
    const auto out = dir / "graphic.mov";
    const auto result = ExportJob(renderedRequest(project, settings, out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    const Comparison cmp = compareWithPreview(project, out);
    metric(std::format("motion graphic over the timeline: preview==export PSNR min {:.2f} dB, mean {:.2f} dB, {} frames",
                       cmp.minPsnr, cmp.meanPsnr(), cmp.frames));
    CHECK(cmp.timeErrors == 0);
    CHECK(cmp.minPsnr >= 50.0);
    // A frame of the export as a picture (the report's screenshot).
    exporttest::PreviewRenderer preview(project);
    const auto rgba = preview.render(Ticks::fromSeconds(1.5), framesToTicks(1, kRate));
    const QImage image(rgba.data(), kWidth, kHeight, kWidth * 4, QImage::Format_RGBA8888);
    CHECK(image.save(QString::fromStdString((dir / "graphic_lower_third.png").string())));
}

TEST_CASE("Rendered multicam export cuts between angles on the exact frame, each angle at its sync",
          "[export][render][media][gpu][multicam]") {
    if (!haveMedia("h264_1080p60_aac.mp4")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p(1920, 1080, kRate, Ticks::fromSeconds(2));
    const MediaId media = p.addMedia("h264_1080p60_aac.mp4");
    // Four angles of the same recording, synced 0, 7, 19 and 31 frames (60 fps) apart: each shows its own frame number.
    const std::array<std::int64_t, 4> offsets{0, 7, 19, 31};
    std::vector<std::pair<MediaId, Ticks>> angles;
    for (const std::int64_t o : offsets) {
        angles.emplace_back(media, framesToTicks(o, kRate60));
    }
    const SequenceId multicam = addMulticam(p.state, angles, 1920, 1080, kRate60);
    // Cuts at output frames 10, 20 and 33 (30 fps), to angles 2, 4 and 3; the source played from 1 s.
    p.addTrack(TrackKind::Video, multicamCuts(p.state, multicam, Ticks{}, Ticks::fromSeconds(2), Ticks::fromSeconds(1),
                                              {{Ticks{}, 0},
                                               {framesToTicks(10, kRate), 1},
                                               {framesToTicks(20, kRate), 3},
                                               {framesToTicks(33, kRate), 2}}));
    const auto dir = freshOutputDir("rendered-multicam");
    const auto out = dir / "multicam.mov";
    const auto result = ExportJob(renderedRequest(p, presetSettings("prores-422-hq"), out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    const auto angleAt = [](int frame) { return frame < 10 ? 0 : frame < 20 ? 1 : frame < 33 ? 3 : 2; };
    int frame = 0;
    int wrong = 0;
    std::string error;
    REQUIRE(decodeVideo(
        out,
        [&](const YuvFrame& f) {
            // Output frame n shows multicam time 1 s + n/30 s = media frame 60 + 2n - offset (60 fps) of its angle.
            const std::int64_t expected = 60 + 2 * frame - offsets[static_cast<std::size_t>(angleAt(frame))];
            const int shown = barcodeOf(f);
            if (shown != expected) {
                ++wrong;
                UNSCOPED_INFO("output frame " << frame << ": barcode " << shown << ", expected " << expected);
            }
            ++frame;
        },
        &error));
    INFO(error);
    CHECK(frame == 60);
    CHECK(wrong == 0);
}

TEST_CASE("Rendered export redacts media inside a nested clip; excepting the nested clip excepts it; review thumbnails too",
          "[export][render][media][gpu][nested][redaction]") {
    if (!haveMedia("h264_1080p60_aac.mp4")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    Clip a = p.clip(h264, StreamKind::Video, Ticks{}, ms(1000), ms(1000));
    ClipProperties fit;
    fit.set(ParamId::Scale, static_cast<double>(kWidth) / 1920.0);
    a.setProperties(fit);
    p.addTrack(TrackKind::Video, {a});
    auto nest = edit::nestClips(p.state, p.sequence, std::vector<ClipId>{a.id}, "Nest");
    REQUIRE(nest);
    p.state = nest->edit.state;
    const ClipId nestClip = nest->clips.front();
    Redaction r;
    r.media = h264;
    r.source = TimeRange{ms(1200), ms(400)};
    r.style = RedactionStyle::Solid;
    r.colorR = 1.0;
    r.colorB = 1.0;
    r.region.shape = MaskShape::Ellipse;
    r.region.set("size.x", 0.3);
    r.region.set("size.y", 0.3);
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);
    const RedactionId redaction = added->id;

    const auto magentaAt = [](const YuvFrame& f, int x, int y) {
        const int shift = f.bits - 8;
        const int cx = x * f.chromaWidth / f.width;
        const int cy = y * f.chromaHeight / f.height;
        const int Y = f.y(x, y) >> shift;
        const int U = f.planes[1][static_cast<std::size_t>(cy) * f.chromaWidth + cx] >> shift;
        const int V = f.planes[2][static_cast<std::size_t>(cy) * f.chromaWidth + cx] >> shift;
        return std::abs(Y - 78) <= 8 && std::abs(U - 214) <= 10 && std::abs(V - 229) <= 10;
    };
    const auto redactedFrames = [&](const TestProject& project, const char* name) {
        const auto out = freshOutputDir(name) / "nested.mov";
        const auto result = ExportJob(renderedRequest(project, presetSettings("prores-422-hq"), out)).run();
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
        std::vector<int> redacted;
        int frame = 0;
        REQUIRE(decodeVideo(out, [&](const YuvFrame& f) {
            if (magentaAt(f, f.width / 2, f.height / 2)) {
                redacted.push_back(frame);
            }
            ++frame;
        }));
        CHECK(compareWithPreview(project, out).minPsnr >= 50.0); // the preview redacts the same frames
        return redacted;
    };
    // Media 1.2 .. 1.6 s = output frames 6..17.
    const std::vector<int> inside = redactedFrames(p, "rendered-nested-redaction");
    REQUIRE(inside.size() == 12);
    CHECK(inside.front() == 6);
    CHECK(inside.back() == 17);
    TestProject excepted = p;
    auto off = edit::setRedactionOverride(excepted.state, redaction, nestClip, false);
    REQUIRE(off);
    excepted.state = *off;
    CHECK(redactedFrames(excepted, "rendered-nested-excepted").empty());

    // Review package thumbnails (C.9) come from the same path: redacted inside the nest, JPEG, at most 640 wide.
    exporter::RenderedReviewFrames frames;
    const auto thumb = frames.frame(p.state, p.sequence, framesToTicks(9, kRate), 640);
    INFO((thumb ? std::string() : thumb.error().message));
    REQUIRE(thumb);
    CHECK(thumb->mime == "image/jpeg");
    const QImage image = QImage::fromData(QByteArray(thumb->bytes.data(), static_cast<qsizetype>(thumb->bytes.size())));
    REQUIRE_FALSE(image.isNull());
    CHECK(image.width() == 640);
    CHECK(image.height() == 360);
    const QColor centre = image.pixelColor(320, 180);
    CHECK(centre.red() > 220);
    CHECK(centre.blue() > 220);
    CHECK(centre.green() < 40);
    const auto clear = frames.frame(p.state, p.sequence, framesToTicks(2, kRate), 320);
    REQUIRE(clear);
    const QImage small = QImage::fromData(QByteArray(clear->bytes.data(), static_cast<qsizetype>(clear->bytes.size())));
    CHECK(small.width() == 320);
    const QColor open = small.pixelColor(160, 90);
    CHECK_FALSE((open.red() > 200 && open.blue() > 200 && open.green() < 60)); // not redacted there
    // Through the package writer: one thumbnail per note, no warnings.
    ReviewNote note;
    note.id = p.state.ids.allocate<ReviewNoteId>();
    note.sequence = p.sequence;
    note.start = framesToTicks(9, kRate);
    note.text = "Is the plate hidden?";
    note.author = "Reviewer";
    note.createdAt = "2026-10-12T10:00:00Z";
    const auto pkgDir = freshOutputDir("review-package");
    const auto written = command::writeReviewPackage(p.state, pkgDir / "review.wabreview", std::vector<ReviewNote>{note}, &frames,
                                                     "Review", "2026-10-12T10:00:00Z");
    REQUIRE(written);
    CHECK(written->frames == 1);
    CHECK(written->warnings.empty());
    // An offline layer is an error (no thumbnail), never a picture without it.
    TestProject offline = p;
    offline.editMedia(h264, [](MediaItem& m) { m.locator.offline = true; });
    const auto none = frames.frame(offline.state, offline.sequence, framesToTicks(9, kRate), 640);
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::NotFound);
}
