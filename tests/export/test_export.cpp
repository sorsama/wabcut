// End-to-end exports of a real-media timeline (3 video layers, 2 audio tracks) with the synthetic video
// source: ffprobe checks the files, decoding them back checks every frame (barcode = frame number, PSNR
// against the reference conversion) and every audio sample (exact for PCM against the playback mix path).

#include "ExportTestUtil.h"

#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <thread>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr Rate kRate{30, 1};

ExportSettings presetSettings(std::string_view id) {
    auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

struct VideoCheck {
    int frames = 0;
    int barcodeErrors = 0;
    int timeErrors = 0;
    double minPsnr = 1e9;
    double sumPsnr = 0.0;
    std::string pixelFormat;
    int width = 0;
    int height = 0;
};

/// Decodes `file` and compares frame k with sequence frame firstFrame + k of the synthetic pattern.
VideoCheck checkVideo(const std::filesystem::path& file, std::int64_t firstFrame, int renderWidth,
                      int renderHeight, bool barcode = true) {
    VideoCheck c;
    std::string error;
    const bool ok = decodeVideo(
        file,
        [&](const YuvFrame& f) {
            const std::int64_t number = firstFrame + c.frames;
            if (barcode && barcodeOf(f) != static_cast<int>(number)) {
                ++c.barcodeErrors;
            }
            if (std::abs(f.seconds - static_cast<double>(c.frames) / kRate.toDouble()) > 0.001) {
                ++c.timeErrors;
            }
            const double p = psnr(f, referenceFrame(renderWidth, renderHeight, number, f));
            c.minPsnr = std::min(c.minPsnr, p);
            c.sumPsnr += std::isinf(p) ? 100.0 : p;
            c.pixelFormat = f.pixelFormat;
            c.width = f.width;
            c.height = f.height;
            ++c.frames;
        },
        &error);
    INFO(error);
    REQUIRE(ok);
    return c;
}

/// Signal-to-noise ratio (dB) of `actual` against `reference` over the common length.
double snrDb(const std::vector<float>& reference, const std::vector<float>& actual) {
    const std::size_t n = std::min(reference.size(), actual.size());
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        signal += static_cast<double>(reference[i]) * reference[i];
        const double d = static_cast<double>(reference[i]) - actual[i];
        noise += d * d;
    }
    return noise == 0.0 ? 200.0 : 10.0 * std::log10(signal / noise);
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

TEST_CASE("H.264 MP4 export of a 3-layer / 2-track timeline: ffprobe, frame-exact video, PSNR, AAC in sync",
          "[export][media][integration]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(2));
    const auto dir = freshOutputDir("h264_mp4");
    const auto out = dir / "timeline.mp4";

    ExportSettings settings = presetSettings("match-source");
    settings.video.quality = 12;
    std::vector<ExportPhase> phases;
    ExportRequest request = makeRequest(project, settings, out);
    request.onProgress = [&](const ExportProgress& p) {
        if (phases.empty() || phases.back() != p.phase) {
            phases.push_back(p.phase);
        }
    };
    ExportJob job(std::move(request));
    const auto result = job.run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 60);
    CHECK(result->audioSamples == 96'000);
    CHECK(result->duration == Ticks::fromSeconds(2));
    CHECK(result->audioEncoder == "aac");
    CHECK(result->videoSource == "synthetic 3-layer test pattern");
    CHECK(result->realtimeFactor > 0.0);
    CHECK(std::filesystem::is_regular_file(out));
    CHECK(partialFiles(dir).empty());
    CHECK(phases == std::vector<ExportPhase>{ExportPhase::Preparing, ExportPhase::Rendering,
                                             ExportPhase::Finalizing, ExportPhase::Done});
    CHECK(job.progress().phase == ExportPhase::Done);
    CHECK(job.progress().framesDone == 60);
    metric(std::format("h264_mp4: video encoder {}", result->videoEncoder));

    std::string error;
    const auto probe = ffprobe(out, &error);
    INFO(error);
    REQUIRE(probe.has_value());
    const auto* v = probe->stream("video");
    const auto* a = probe->stream("audio");
    REQUIRE(v != nullptr);
    REQUIRE(a != nullptr);
    CHECK(v->at("codec_name") == "h264");
    CHECK(v->at("width") == "640");
    CHECK(v->at("height") == "360");
    CHECK(v->at("pix_fmt") == "yuv420p");
    CHECK(v->at("r_frame_rate") == "30/1");
    CHECK(v->at("nb_frames") == "60");
    CHECK(v->at("color_space") == "bt709");
    CHECK(v->at("color_primaries") == "bt709");
    CHECK(v->at("color_transfer") == "bt709");
    CHECK(v->at("color_range") == "tv");
    CHECK(a->at("codec_name") == "aac");
    CHECK(a->at("sample_rate") == "48000");
    CHECK(a->at("channels") == "2");
    CHECK(probe->format.at("format_name").find("mp4") != std::string::npos);

    const VideoCheck video = checkVideo(out, 0, kWidth, kHeight);
    CHECK(video.frames == 60);
    CHECK(video.barcodeErrors == 0);
    CHECK(video.timeErrors == 0);
    metric(std::format("h264_mp4: PSNR min {:.2f} dB, mean {:.2f} dB over {} frames", video.minPsnr,
                       video.sumPsnr / video.frames, video.frames));
    CHECK(video.minPsnr >= 45.0);

    // AAC: the priming is carried as an edit list, so decoding gives the mix from sample 0 (not 1024 late).
    const auto audio = decodeAudio(out, &error);
    INFO(error);
    REQUIRE(audio.has_value());
    CHECK(audio->sampleRate == 48000);
    CHECK(audio->channels == 2);
    const std::int64_t decodedFrames = static_cast<std::int64_t>(audio->samples.size() / 2);
    metric(std::format("h264_mp4: AAC decoded {} sample frames (expected 96000)", decodedFrames));
    CHECK(decodedFrames == 96'000);
    const std::vector<float> reference = referenceMix(project, Ticks{}, 96'000);
    const double snr = snrDb(reference, audio->samples);
    metric(std::format("h264_mp4: AAC SNR {:.2f} dB vs the reference mix", snr));
    CHECK(snr >= 20.0);
}

TEST_CASE("ProRes 422 HQ MOV export: ffprobe, frame-exact 10-bit video, sample-exact 24-bit PCM",
          "[export][media][integration]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(2));
    const auto dir = freshOutputDir("prores_mov");
    const auto out = dir / "master.mov";

    const auto result = ExportJob(makeRequest(project, presetSettings("prores-422-hq"), out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->videoEncoder == "prores_ks");
    CHECK(result->encoderFallbacks.empty()); // no ProRes hardware: the software encoder is the only candidate
    CHECK(result->audioEncoder == "pcm_s24le");

    std::string error;
    const auto probe = ffprobe(out, &error);
    INFO(error);
    REQUIRE(probe.has_value());
    const auto* v = probe->stream("video");
    const auto* a = probe->stream("audio");
    REQUIRE(v != nullptr);
    REQUIRE(a != nullptr);
    CHECK(v->at("codec_name") == "prores");
    CHECK(v->at("profile") == "HQ");
    CHECK(v->at("pix_fmt") == "yuv422p10le");
    CHECK(v->at("nb_frames") == "60");
    CHECK(v->at("color_space") == "bt709");
    CHECK(a->at("codec_name") == "pcm_s24le");
    CHECK(a->at("sample_rate") == "48000");
    CHECK(probe->format.at("format_name").find("mov") != std::string::npos);

    const VideoCheck video = checkVideo(out, 0, kWidth, kHeight);
    CHECK(video.frames == 60);
    CHECK(video.barcodeErrors == 0);
    CHECK(video.pixelFormat == "yuv422p10le");
    metric(std::format("prores_mov: PSNR min {:.2f} dB, mean {:.2f} dB over {} frames", video.minPsnr,
                       video.sumPsnr / video.frames, video.frames));
    CHECK(video.minPsnr >= 45.0);

    const auto audio = decodeAudio(out, &error);
    INFO(error);
    REQUIRE(audio.has_value());
    REQUIRE(audio->pcm.size() == 96'000u * 2);
    const std::vector<float> reference = referenceMix(project, Ticks{}, 96'000);
    CHECK(pcmMismatches(reference, audio->pcm, 24) == 0);
}

TEST_CASE("In/Out export: the marked range only, video and audio from the In point",
          "[export][media][integration]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(2));
    project.editSequence([](Sequence& s) {
        s.markIn = ms(500);
        s.markOut = ms(1500);
    });
    const auto dir = freshOutputDir("in_out");

    // Video + PCM in MKV.
    ExportSettings mkv = presetSettings("prores-422-hq");
    mkv.container = Container::Mkv;
    mkv.audio.codec = AudioCodec::Pcm16;
    mkv.range = RangeMode::InOut;
    const auto result = ExportJob(makeRequest(project, mkv, dir / "range.mkv")).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 30);
    CHECK(result->audioSamples == 48'000);
    const VideoCheck video = checkVideo(dir / "range.mkv", 15, kWidth, kHeight); // 0.5 s = sequence frame 15
    CHECK(video.frames == 30);
    CHECK(video.barcodeErrors == 0);
    std::string error;
    const auto audio = decodeAudio(dir / "range.mkv", &error);
    INFO(error);
    REQUIRE(audio.has_value());
    REQUIRE(audio->pcm.size() == 48'000u * 2);
    CHECK(pcmMismatches(referenceMix(project, ms(500), 48'000), audio->pcm, 16) == 0);

    // Audio only (WAV).
    ExportSettings wav = presetSettings("audio-wav");
    wav.range = RangeMode::InOut;
    const auto wavResult = ExportJob(makeRequest(project, wav, dir / "range.wav")).run();
    INFO((wavResult ? std::string() : wavResult.error().message));
    REQUIRE(wavResult.has_value());
    CHECK(wavResult->frames == 0);
    CHECK(wavResult->videoEncoder.empty());
    const auto wavAudio = decodeAudio(dir / "range.wav", &error);
    REQUIRE(wavAudio.has_value());
    CHECK(wavAudio->codec == "pcm_s24le");
    REQUIRE(wavAudio->pcm.size() == 48'000u * 2);
    CHECK(pcmMismatches(referenceMix(project, ms(500), 48'000), wavAudio->pcm, 24) == 0);
}

TEST_CASE("ScaleMode: Fill centre-crops to 9:16, Fit letterboxes", "[export][media][integration]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("scale_mode");

    ExportSettings vertical = presetSettings("vertical-1080x1920");
    vertical.video.width = 360; // 9:16 at a test size
    vertical.video.height = 640;
    vertical.video.rateControl = RateControl::Quality;
    vertical.video.quality = 12;
    vertical.audio.enabled = false;
    const auto fill = ExportJob(makeRequest(project, vertical, dir / "fill.mp4")).run();
    INFO((fill ? std::string() : fill.error().message));
    REQUIRE(fill.has_value());
    const auto resolved = resolve(vertical, project.seq());
    REQUIRE(resolved.has_value());
    CHECK(resolved->renderWidth == 1138); // 640 * 16/9 = 1137.8
    CHECK(resolved->renderHeight == 640);
    const VideoCheck filled =
        checkVideo(dir / "fill.mp4", 0, resolved->renderWidth, resolved->renderHeight, false);
    CHECK(filled.frames == 30);
    CHECK(filled.width == 360);
    CHECK(filled.height == 640);
    metric(
        std::format("scale_mode: Fill 360x640 PSNR min {:.2f} dB ({})", filled.minPsnr, fill->videoEncoder));
    CHECK(filled.minPsnr >= 45.0);

    ExportSettings square = vertical;
    square.video.width = 640;
    square.video.height = 640;
    square.video.scaleMode = ScaleMode::Fit;
    const auto fit = ExportJob(makeRequest(project, square, dir / "fit.mp4")).run();
    INFO((fit ? std::string() : fit.error().message));
    REQUIRE(fit.has_value());
    const VideoCheck fitted = checkVideo(dir / "fit.mp4", 0, 640, 360, false);
    CHECK(fitted.frames == 30);
    CHECK(fitted.height == 640);
    metric(std::format("scale_mode: Fit 640x640 PSNR min {:.2f} dB ({})", fitted.minPsnr, fit->videoEncoder));
    CHECK(fitted.minPsnr >= 45.0); // includes the black bars
}

TEST_CASE("Cancelling an export leaves no partial file and keeps an existing output", "[export][media]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(2));
    const auto dir = freshOutputDir("cancel");
    const auto out = dir / "cancelled.mp4";
    {
        std::ofstream old(out, std::ios::binary);
        old << "previous export";
    }
    std::stop_source cancel;
    ExportRequest request = makeRequest(project, presetSettings("match-source"), out);
    request.videoSource = cancellingAfter(syntheticVideoSourceFactory(), 10, cancel); // cancel at frame 10
    ExportJob job(std::move(request));
    const auto result = job.run(cancel.get_token());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(job.progress().phase == ExportPhase::Cancelled);
    CHECK(job.progress().framesDone < 60);
    CHECK(partialFiles(dir).empty());
    CHECK(readFile(out) == "previous export");

    // Cancelled before it starts rendering: nothing at all is written.
    std::stop_source early;
    early.request_stop();
    const auto out2 = dir / "never.mp4";
    const auto r2 =
        ExportJob(makeRequest(project, presetSettings("match-source"), out2)).run(early.get_token());
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(std::filesystem::exists(out2));
    CHECK(partialFiles(dir).empty());
}

TEST_CASE("A successful export replaces an existing file atomically", "[export][media]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("overwrite");
    const auto out = dir / "a.wav";
    {
        std::ofstream old(out, std::ios::binary);
        old << "previous export";
    }
    const auto result = ExportJob(makeRequest(project, presetSettings("audio-wav"), out)).run();
    REQUIRE(result.has_value());
    CHECK(std::filesystem::file_size(out) > 100'000);
    CHECK(partialFiles(dir).empty());
    const auto audio = decodeAudio(out);
    REQUIRE(audio.has_value());
    CHECK(audio->pcm.size() == 48'000u * 2);
}

TEST_CASE("Offline or missing original media fails clearly; proxies are never used", "[export][media]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("offline");
    TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const MediaId prores = project.state.media[1]->id;
    const MediaId sine = project.state.media[4]->id;

    // Offline flag (e.g. the fingerprint no longer matches): an error naming the item, no file.
    TestProject offline = project;
    offline.editMedia(prores, [](MediaItem& m) { m.locator.offline = true; });
    auto r = ExportJob(makeRequest(offline, presetSettings("match-source"), dir / "x.mp4")).run();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::NotFound);
    CHECK(r.error().message.find("prores_422.mov") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(dir / "x.mp4"));

    // Missing file, with a proxy that does exist: still an error (exports use originals only).
    TestProject missing = project;
    missing.editMedia(sine, [](MediaItem& m) {
        m.locator.path = (mediaDir() / "no_such_file.wav").string();
        m.locator.proxy = ProxyMedia{(mediaDir() / "sine_48k.wav").string(), {}};
    });
    r = ExportJob(makeRequest(missing, presetSettings("audio-wav"), dir / "y.wav")).run();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::NotFound);
    CHECK(r.error().message.find("sine_48k.wav") != std::string::npos);
    CHECK(partialFiles(dir).empty());

    // Media outside the exported range does not matter.
    missing.editSequence([](Sequence& s) {
        s.markIn = Ticks{};
        s.markOut = ms(100); // A2 (sine) starts at 0.125 s
    });
    ExportSettings head = presetSettings("audio-wav");
    head.range = RangeMode::InOut;
    r = ExportJob(makeRequest(missing, head, dir / "z.wav")).run();
    INFO((r ? std::string() : r.error().message));
    CHECK(r.has_value());

    // Media of a muted track is not needed either.
    missing.editSequence([](Sequence& s) {
        Track a2 = *s.audioTracks[1];
        a2.muted = true;
        s.audioTracks[1] = std::make_shared<const Track>(std::move(a2));
    });
    r = ExportJob(makeRequest(missing, presetSettings("audio-wav"), dir / "w.wav")).run();
    INFO((r ? std::string() : r.error().message));
    CHECK(r.has_value());
}

TEST_CASE("Muted and soloed tracks mix like playback", "[export][media]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("solo");
    TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    project.editSequence([](Sequence& s) {
        Track a2 = *s.audioTracks[1];
        a2.solo = true; // only A2 (the sine) is heard
        s.audioTracks[1] = std::make_shared<const Track>(std::move(a2));
    });
    const auto r = ExportJob(makeRequest(project, presetSettings("audio-wav"), dir / "solo.wav")).run();
    REQUIRE(r.has_value());
    const auto audio = decodeAudio(dir / "solo.wav");
    REQUIRE(audio.has_value());
    const auto reference = referenceMix(project, Ticks{}, 48'000);
    CHECK(pcmMismatches(reference, audio->pcm, 24) == 0);
    // Before the sine starts (0.125 s) the soloed mix is silent: A1's ramp is not in it.
    CHECK(std::all_of(audio->pcm.begin(), audio->pcm.begin() + 2 * 5'900,
                      [](std::int32_t s) { return s == 0; }));
}

TEST_CASE("No working hardware encoder: falls back to libx264 and reports why", "[export][media][encoders]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    EncoderOrderGuard guard;
    const auto vendors = media::gpuVendors();
    const bool amd = std::find(vendors.begin(), vendors.end(), "AMD") != vendors.end();
    // A name this FFmpeg does not have, and (on a machine without an AMD GPU) AMF, which fails to open.
    std::vector<std::string> order{"h264_nosuchhw"};
    if (!amd) {
        order.push_back("h264_amf");
    }
    setHardwareEncoderOrder(order);
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("fallback");
    ExportSettings settings = presetSettings("match-source");
    settings.audio.enabled = false;
    const auto result = ExportJob(makeRequest(project, settings, dir / "fallback.mp4")).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->videoEncoder == "libx264");
    REQUIRE(result->encoderFallbacks.size() == order.size());
    CHECK(result->encoderFallbacks[0].starts_with("h264_nosuchhw: "));
    if (!amd) {
        CHECK(result->encoderFallbacks[1].starts_with("h264_amf: "));
    }
    const VideoCheck video = checkVideo(dir / "fallback.mp4", 0, kWidth, kHeight);
    CHECK(video.frames == 30);
    CHECK(video.barcodeErrors == 0);
    metric(std::format("fallback: libx264 CRF 16 PSNR min {:.2f} dB, mean {:.2f} dB", video.minPsnr,
                       video.sumPsnr / video.frames));
    CHECK(video.minPsnr >= 45.0);

    // An explicit encoder is used as is: no fallback.
    settings.video.encoder = "h264_nosuchhw";
    const auto explicitResult = ExportJob(makeRequest(project, settings, dir / "explicit.mp4")).run();
    REQUIRE_FALSE(explicitResult.has_value());
    CHECK(explicitResult.error().code == ErrorCode::Unsupported);
    CHECK(explicitResult.error().message.find("h264_nosuchhw") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(dir / "explicit.mp4"));
}

TEST_CASE("Automatic encoder selection uses NVENC on an NVIDIA machine", "[export][media][encoders][gpu]") {
    const auto vendors = media::gpuVendors();
    if (std::find(vendors.begin(), vendors.end(), "NVIDIA") == vendors.end()) {
        SKIP("no NVIDIA GPU");
    }
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    EncoderOrderGuard guard;
    setHardwareEncoderOrder({}); // the policy, not an environment override
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("nvenc");
    ExportSettings settings = presetSettings("match-source");
    settings.audio.enabled = false;
    const auto result = ExportJob(makeRequest(project, settings, dir / "nvenc.mp4")).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    std::string fallbacks;
    for (const auto& f : result->encoderFallbacks) {
        fallbacks += f + "; ";
    }
    INFO("fallbacks: " << fallbacks);
    CHECK(result->videoEncoder == "h264_nvenc");

    // HEVC too, when the GPU has it (every NVENC generation since Maxwell 2).
    settings.video.codec = VideoCodec::Hevc;
    const auto hevc = ExportJob(makeRequest(project, settings, dir / "nvenc_hevc.mp4")).run();
    INFO((hevc ? std::string() : hevc.error().message));
    REQUIRE(hevc.has_value());
    CHECK(hevc->videoEncoder == "hevc_nvenc");
    const auto probe = ffprobe(dir / "nvenc_hevc.mp4");
    REQUIRE(probe.has_value());
    REQUIRE(probe->stream("video") != nullptr);
    CHECK(probe->stream("video")->at("codec_name") == "hevc");
    CHECK(probe->stream("video")->at("codec_tag_string") == "hvc1");
}

TEST_CASE("Export refuses bad requests before writing anything", "[export][media]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("refuse");

    // Unknown sequence.
    ExportRequest unknown = makeRequest(project, presetSettings("audio-wav"), dir / "b.wav");
    unknown.sequence = SequenceId{999'999};
    auto r = ExportJob(std::move(unknown)).run();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::NotFound);

    // Missing output folder.
    r = ExportJob(makeRequest(project, presetSettings("audio-wav"), dir / "missing" / "c.wav")).run();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::IoError);

    // Overwriting a source file (a private copy, so a failure cannot damage the shared test clips).
    const auto copy = dir / "sine_copy.wav";
    std::filesystem::copy_file(mediaDir() / "sine_48k.wav", copy);
    const auto copySize = std::filesystem::file_size(copy);
    TestProject ownSource = project;
    ownSource.editMedia(project.state.media[4]->id, [&](MediaItem& m) { m.locator.path = copy.string(); });
    r = ExportJob(makeRequest(ownSource, presetSettings("audio-wav"), copy)).run();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(std::filesystem::file_size(copy) == copySize);

    // Invalid settings.
    ExportSettings bad = presetSettings("match-source");
    bad.audio.codec = AudioCodec::Pcm16;
    r = ExportJob(makeRequest(project, bad, dir / "d.mp4")).run();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(partialFiles(dir).empty());
}

TEST_CASE("Every codec path encodes, probes and decodes back frame-exact", "[export][media][codecs]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, ms(500));
    const auto dir = freshOutputDir("codecs");
    struct Case {
        std::string name;
        Container container;
        VideoCodec codec;
        std::string encoder;
        OutputPixelFormat pixelFormat;
        std::string probeCodec;
        std::string probePixelFormat;
        std::string probeProfile; // empty: not checked
        ProResProfile prores = ProResProfile::Hq;
        DnxhrProfile dnxhr = DnxhrProfile::Hq;
    };
    const std::vector<Case> cases{
        {"dnxhr_hq.mov", Container::Mov, VideoCodec::DnxHR, "auto", OutputPixelFormat::Yuv422p, "dnxhd",
         "yuv422p", "DNXHR HQ"},
        {"dnxhr_hqx.mkv", Container::Mkv, VideoCodec::DnxHR, "auto", OutputPixelFormat::Yuv422p10, "dnxhd",
         "yuv422p10le", "DNXHR HQX", ProResProfile::Hq, DnxhrProfile::Hqx},
        {"prores_proxy.mov", Container::Mov, VideoCodec::ProRes, "auto", OutputPixelFormat::Yuv422p10,
         "prores", "yuv422p10le", "Proxy", ProResProfile::Proxy},
        {"prores_lt.mov", Container::Mov, VideoCodec::ProRes, "auto", OutputPixelFormat::Yuv422p10, "prores",
         "yuv422p10le", "LT", ProResProfile::Lt},
        {"x265.mkv", Container::Mkv, VideoCodec::Hevc, "libx265", OutputPixelFormat::Yuv420p, "hevc",
         "yuv420p", "Main"},
        {"x264_10bit.mp4", Container::Mp4, VideoCodec::H264, "libx264", OutputPixelFormat::Yuv420p10, "h264",
         "yuv420p10le", "High 10"},
        {"x264_422.mkv", Container::Mkv, VideoCodec::H264, "libx264", OutputPixelFormat::Yuv422p, "h264",
         "yuv422p", "High 4:2:2"},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            ExportSettings s = presetSettings("match-source");
            s.container = c.container;
            s.video.codec = c.codec;
            s.video.encoder = c.encoder;
            s.video.pixelFormat = c.pixelFormat;
            s.video.proresProfile = c.prores;
            s.video.dnxhrProfile = c.dnxhr;
            s.audio.codec = c.container == Container::Mp4 ? AudioCodec::Aac : AudioCodec::Pcm16;
            const auto out = dir / c.name;
            const auto result = ExportJob(makeRequest(project, s, out)).run();
            INFO((result ? std::string() : result.error().message));
            REQUIRE(result.has_value());
            const auto probe = ffprobe(out);
            REQUIRE(probe.has_value());
            const auto* v = probe->stream("video");
            REQUIRE(v != nullptr);
            CHECK(v->at("codec_name") == c.probeCodec);
            CHECK(v->at("pix_fmt") == c.probePixelFormat);
            if (!c.probeProfile.empty()) {
                CHECK(v->at("profile") == c.probeProfile);
            }
            const VideoCheck video = checkVideo(out, 0, kWidth, kHeight);
            CHECK(video.frames == 15);
            CHECK(video.barcodeErrors == 0);
            metric(std::format("codecs: {} via {} PSNR min {:.2f} dB", c.name, result->videoEncoder,
                               video.minPsnr));
            CHECK(video.minPsnr >= 40.0);
        }
    }
}

TEST_CASE("44.1 kHz and 16-bit audio keep exact sample counts", "[export][media]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, Rate{30000, 1001}, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("audio_rates");
    ExportSettings wav = presetSettings("audio-wav");
    wav.audio.sampleRate = 44'100;
    wav.audio.codec = AudioCodec::Pcm16;
    const auto r = ExportJob(makeRequest(project, wav, dir / "a.wav")).run();
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r.has_value());
    CHECK(r->audioSamples == 44'100);
    const auto audio = decodeAudio(dir / "a.wav");
    REQUIRE(audio.has_value());
    CHECK(audio->sampleRate == 44'100);
    REQUIRE(audio->pcm.size() == 44'100u * 2);
    CHECK(pcmMismatches(referenceMix(project, Ticks{}, 44'100, 2, 44'100), audio->pcm, 16) == 0);

    // 29.97 fps video + AAC: ceil(1 s * 30000/1001) = 30 frames, audio = those 30 frame periods exactly.
    ExportSettings mp4 = presetSettings("match-source");
    const auto v = ExportJob(makeRequest(project, mp4, dir / "ntsc.mp4")).run();
    INFO((v ? std::string() : v.error().message));
    REQUIRE(v.has_value());
    CHECK(v->frames == 30);
    CHECK(v->audioSamples == 48'048);
    const auto aac = decodeAudio(dir / "ntsc.mp4");
    REQUIRE(aac.has_value());
    CHECK(aac->samples.size() == 48'048u * 2);
    const auto probe = ffprobe(dir / "ntsc.mp4");
    REQUIRE(probe.has_value());
    CHECK(probe->stream("video")->at("r_frame_rate") == "30000/1001");
}

namespace {

/// A source whose next() stalls at frame `stallAt` until stop() (a hung renderer or decoder).
class StallingSource final : public IVideoFrameSource {
public:
    StallingSource(std::unique_ptr<IVideoFrameSource> inner, std::int64_t stallAt)
        : m_inner(std::move(inner)), m_stallAt(stallAt) {}
    ~StallingSource() override { stop(); }
    std::string description() const override { return "stalling"; }
    int maxInFlight() const override { return m_inner->maxInFlight(); }
    std::expected<void, Error> request(std::int64_t frameIndex) override {
        return m_inner->request(frameIndex);
    }
    std::expected<media::FrameHandle, Error> next() override {
        if (m_taken++ >= m_stallAt) {
            std::unique_lock lock(m_mutex);
            m_changed.wait(lock, [&] { return m_stopped; });
            return std::unexpected(Error{ErrorCode::Cancelled, "stalled source stopped"});
        }
        return m_inner->next();
    }
    void stop() override {
        {
            std::lock_guard lock(m_mutex);
            m_stopped = true;
        }
        m_changed.notify_all();
        m_inner->stop();
    }

private:
    std::unique_ptr<IVideoFrameSource> m_inner;
    std::int64_t m_stallAt;
    std::int64_t m_taken = 0;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_stopped = false;
};

} // namespace

TEST_CASE("Cancelling unblocks a video source that stalls", "[export][media]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(kWidth, kHeight, kRate, Ticks::fromSeconds(2));
    const auto dir = freshOutputDir("stall");
    ExportRequest request = makeRequest(project, presetSettings("match-source"), dir / "stalled.mp4");
    request.videoSource =
        [](const ProjectState&, const Sequence&,
           const VideoSourceFormat& format) -> std::expected<std::unique_ptr<IVideoFrameSource>, Error> {
        return std::make_unique<StallingSource>(makeSyntheticVideoSource(format), 5);
    };
    std::stop_source cancel;
    std::atomic<bool> finished{false};
    std::expected<ExportResult, Error> result;
    std::thread worker([&] {
        result = ExportJob(std::move(request)).run(cancel.get_token());
        finished = true;
    });
    // Let the job reach the stalled frame, then cancel: the job must end promptly, not wait for the source.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(finished.load());
    const auto cancelledAt = std::chrono::steady_clock::now();
    cancel.request_stop();
    worker.join();
    CHECK(std::chrono::steady_clock::now() - cancelledAt < std::chrono::seconds(5));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(partialFiles(dir).empty());
    CHECK_FALSE(std::filesystem::exists(dir / "stalled.mp4"));
}

TEST_CASE("A 5.1 master exports 5.1 AAC and AC-3 (ffprobe), and a stereo export of it is the downmix (A.9)",
          "[export][media][channels]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project = threeLayerTimeline(kWidth, kHeight, Rate{25, 1}, Ticks::fromSeconds(1));
    project.editSequence([](Sequence& s) { s.masterFormat = ChannelFormat::Surround51; });
    const auto dir = freshOutputDir("surround");
    struct Case {
        const char* file;
        AudioCodec codec;
        const char* codecName;
    };
    for (const Case& c : {Case{"aac.mp4", AudioCodec::Aac, "aac"}, Case{"ac3.mp4", AudioCodec::Ac3, "ac3"}, Case{"ac3.mkv", AudioCodec::Ac3, "ac3"}}) {
        INFO(c.file);
        ExportSettings settings = presetSettings("match-source");
        if (std::string_view(c.file).ends_with(".mkv")) {
            settings.container = Container::Mkv;
        }
        settings.audio.codec = c.codec;
        settings.audio.channels = 0; // as the master
        const auto r = ExportJob(makeRequest(project, settings, dir / c.file)).run();
        INFO((r ? std::string() : r.error().message));
        REQUIRE(r.has_value());
        const auto probe = ffprobe(dir / c.file);
        REQUIRE(probe.has_value());
        const auto* audio = probe->stream("audio");
        REQUIRE(audio != nullptr);
        CHECK(audio->at("codec_name") == c.codecName);
        CHECK(audio->at("channels") == "6");
        CHECK(audio->at("channel_layout").starts_with("5.1"));
    }
    // channels 2 on the 5.1 master: a stereo file with the ITU-R BS.775 downmix of the 5.1 mix. The timeline's tracks are
    // stereo at the panner's default, so the downmix is their stereo mix (to the 24-bit LSB: a mono file reaches 5.1
    // through Wabcut's -3 dB, the stereo mix through FFmpeg's).
    ExportSettings stereo = presetSettings("audio-wav");
    stereo.audio.channels = 2;
    const auto r = ExportJob(makeRequest(project, stereo, dir / "downmix.wav")).run();
    REQUIRE(r.has_value());
    const auto decoded = decodeAudio(dir / "downmix.wav");
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels == 2);
    const std::vector<float> reference = referenceMix(project, Ticks{}, 48'000, 2, 48'000);
    REQUIRE(decoded->samples.size() == reference.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(decoded->samples[i]) - reference[i]));
    }
    CHECK(worst <= 2.0 / 8388608.0);
    // A stereo master with channels 0 stays stereo.
    project.editSequence([](Sequence& s) { s.masterFormat = ChannelFormat::Stereo; });
    ExportSettings follow = presetSettings("match-source");
    follow.audio.channels = 0;
    const auto plain = ExportJob(makeRequest(project, follow, dir / "stereo.mp4")).run();
    REQUIRE(plain.has_value());
    CHECK(ffprobe(dir / "stereo.mp4")->stream("audio")->at("channels") == "2");
}
