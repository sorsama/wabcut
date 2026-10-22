// 0.2 (Premiere parity, export): Premiere's formats (alpha masters, DNxHD, audio-only files, image sequences), user
// presets, the Effects section (overlays, Lumetri Look/LUT, Video Limiter, Loudness Normalization), Metadata (chapters,
// timecode track, XMP, Broadcast WAV), soft subtitle tracks, the Media Encoder-style queue (hold, pause, resume), watch
// folders, smart rendering (source passthrough and Use Previews), Use Maximum Render Quality, the CPU limit and the
// export.* commands. Exports use the synthetic source unless a test says otherwise; outputs are probed with ffprobe and
// decoded with FFmpeg.

#include "ExportTestUtil.h"

#include <wabcut/audio/dsp/Loudness.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/Registry.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportCommandServices.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/WatchFolders.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/PreviewCache.h>

#include <QColor>
#include <QImage>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

ExportSettings preset(std::string_view id) {
    const auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

/// A sequence with no clips (the synthetic source draws the picture), `seconds` long.
TestProject emptyProject(int width, int height, Rate rate, double seconds) {
    return TestProject(width, height, rate, Ticks{static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond))});
}

std::expected<ExportResult, Error> runExport(const TestProject& project, ExportSettings settings, const std::filesystem::path& out) {
    ExportJob job(makeRequest(project, std::move(settings), out));
    return job.run();
}

/// The first frame of a file (video, or a still) as RGBA8.
struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
    std::array<int, 4> at(int x, int y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    }
};

std::optional<RgbaImage> firstFrameRgba(const std::filesystem::path& file, int frameIndex = 0) {
    AVFormatContext* fmt = nullptr;
    const std::string path = pathToUtf8(file);
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return std::nullopt;
    }
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> fmtOwner(fmt, [](AVFormatContext* f) { avformat_close_input(&f); });
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        return std::nullopt;
    }
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) {
        return std::nullopt;
    }
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> ctxOwner(ctx, [](AVCodecContext* c) { avcodec_free_context(&c); });
    avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        return std::nullopt;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::optional<RgbaImage> out;
    int seen = 0;
    bool flushing = false;
    while (!out) {
        if (!flushing) {
            if (av_read_frame(fmt, packet) < 0) {
                flushing = true;
                avcodec_send_packet(ctx, nullptr);
            } else if (packet->stream_index == stream) {
                avcodec_send_packet(ctx, packet);
                av_packet_unref(packet);
            } else {
                av_packet_unref(packet);
                continue;
            }
        }
        while (!out) {
            const int r = avcodec_receive_frame(ctx, frame);
            if (r < 0) {
                break;
            }
            if (seen++ < frameIndex) {
                av_frame_unref(frame);
                continue;
            }
            AVFrame* rgba = av_frame_alloc();
            rgba->format = AV_PIX_FMT_RGBA;
            rgba->width = frame->width;
            rgba->height = frame->height;
            SwsContext* sws = sws_alloc_context();
            if (sws_scale_frame(sws, rgba, frame) >= 0) {
                RgbaImage img;
                img.width = rgba->width;
                img.height = rgba->height;
                img.pixels.resize(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4);
                for (int y = 0; y < img.height; ++y) {
                    std::copy_n(rgba->data[0] + static_cast<std::ptrdiff_t>(y) * rgba->linesize[0], img.width * 4,
                                img.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) * 4);
                }
                out = std::move(img);
            }
            sws_free_context(&sws);
            av_frame_free(&rgba);
            av_frame_unref(frame);
        }
        if (flushing && !out) {
            break;
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    return out;
}

/// The packets of the first video stream (their bytes), in file order.
std::vector<std::vector<std::uint8_t>> videoPackets(const std::filesystem::path& file) {
    std::vector<std::vector<std::uint8_t>> out;
    AVFormatContext* fmt = nullptr;
    const std::string path = pathToUtf8(file);
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return out;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVPacket* packet = av_packet_alloc();
    while (stream >= 0 && av_read_frame(fmt, packet) >= 0) {
        if (packet->stream_index == stream) {
            out.emplace_back(packet->data, packet->data + packet->size);
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&fmt);
    return out;
}

std::string fileText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string probeValue(const std::map<std::string, std::string>* section, const std::string& key) {
    if (!section) {
        return {};
    }
    const auto it = section->find(key);
    return it == section->end() ? std::string() : it->second;
}

const std::map<std::string, std::string>* streamOfCodec(const ProbeResult& probe, std::string_view codec) {
    for (const auto& s : probe.streams) {
        if (const auto it = s.find("codec_name"); it != s.end() && it->second == codec) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

// ---- Settings, presets ---------------------------------------------------------------------------------------------

TEST_CASE("Export presets: the 0.2 formats validate and round-trip as JSON", "[export][settings][presets]") {
    for (const Preset& p : builtInPresets()) {
        INFO(p.id);
        CHECK(validate(p.settings).has_value());
        const Json json = settingsToJson(p.settings);
        const auto back = settingsFromJson(json);
        REQUIRE(back.has_value());
        CHECK(settingsToJson(*back) == json);
        const auto file = presetFromJson(presetToJson(p));
        REQUIRE(file.has_value());
        CHECK(file->id == p.id);
        CHECK(file->name == p.name);
    }
    CHECK(preset("prores-4444-alpha").video.pixelFormat == OutputPixelFormat::Yuva444p10);
    CHECK(preset("prores-4444-alpha").video.proresProfile == ProResProfile::P4444);
    CHECK(hasAlpha(preset("animation-alpha").video.pixelFormat));
    CHECK(preset("dnxhd-1080p-hq").video.dnxhrProfile == DnxhrProfile::DnxhdHq);
    CHECK(preset("audio-mp3").container == Container::Mp3);
    CHECK(preset("png-sequence").container == Container::Image);
    CHECK(outputExtension(preset("png-sequence")) == ".png");
    CHECK(outputExtension(preset("dpx-sequence")) == ".dpx");
    CHECK(outputExtension(preset("exr-sequence")) == ".exr");
    CHECK_FALSE(preset("png-still").video.exportAsSequence);

    // DNxHD: Avid's bitrate for the frame rate, 1920x1080 only; no HQX at 29.97.
    CHECK(dnxhdBitrate(DnxhrProfile::DnxhdHq, 1920, 1080, Rate{30000, 1001}) == 220'000'000);
    CHECK(dnxhdBitrate(DnxhrProfile::DnxhdHq, 1920, 1080, Rate{25, 1}) == 185'000'000);
    CHECK(dnxhdBitrate(DnxhrProfile::DnxhdLb, 1920, 1080, Rate{24000, 1001}) == 36'000'000);
    CHECK_FALSE(dnxhdBitrate(DnxhrProfile::DnxhdHqx, 1920, 1080, Rate{30, 1}).has_value());
    CHECK_FALSE(dnxhdBitrate(DnxhrProfile::DnxhdHq, 1280, 720, Rate{30, 1}).has_value());
    Sequence seq;
    seq.width = 1920;
    seq.height = 1080;
    seq.frameRate = Rate{30, 1};
    seq.duration = Ticks::fromSeconds(1);
    ExportSettings hqx = preset("dnxhd-1080p-hq");
    hqx.video.dnxhrProfile = DnxhrProfile::DnxhdHqx;
    hqx.video.pixelFormat = OutputPixelFormat::Yuv422p10;
    CHECK(validate(hqx).has_value());
    CHECK_FALSE(resolve(hqx, seq).has_value()); // 220x is not defined here

    // A still: one frame.
    const auto still = resolve(preset("png-still"), seq);
    REQUIRE(still.has_value());
    CHECK(still->frameCount == 1);
    CHECK(resolve(preset("png-sequence"), seq)->frameCount == 30);
}

TEST_CASE("Export settings JSON is checked like untrusted input", "[export][settings][presets]") {
    Json good = settingsToJson(preset("youtube-1080p"));
    good["somethingNew"] = 42; // a newer build's key: ignored
    CHECK(settingsFromJson(good).has_value());

    const auto rejects = [](Json j) {
        const auto r = settingsFromJson(j);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    };
    Json bad = good;
    bad["container"] = "avi";
    rejects(bad);
    bad = good;
    bad["video"]["quality"] = 999;
    rejects(bad);
    bad = good;
    bad["video"]["width"] = "wide";
    rejects(bad);
    bad = good;
    bad["effects"] = Json{{"imageOverlay", Json{{"applied", true}, {"image", std::string(5000, 'x')}}}};
    rejects(bad);
    bad = good;
    bad["effects"] = Json{{"limiter", Json{{"applied", true}, {"clipLevel", 300}}}};
    rejects(bad);
    bad = good;
    bad["container"] = "wav"; // video in WAV: validate() refuses
    rejects(bad);
    CHECK_FALSE(settingsFromJson(Json::array()).has_value());

    CHECK_FALSE(presetFromJson("not json").has_value());
    CHECK_FALSE(presetFromJson(R"({"format":"something-else","settings":{}})").has_value());
    CHECK_FALSE(presetFromJson(R"({"format":"wabcut-export-preset","name":"x"})").has_value()); // no settings
    CHECK_FALSE(presetFromJson(std::string(kMaxPresetFileBytes + 1, ' ')).has_value());
}

TEST_CASE("Overrides: Include Alpha Channel and the format defaults", "[export][settings]") {
    ExportSettings s = preset("prores-422-hq");
    REQUIRE(applyOverrides(s, Json{{"alpha", true}}).has_value());
    CHECK(s.video.proresProfile == ProResProfile::P4444);
    CHECK(s.video.pixelFormat == OutputPixelFormat::Yuva444p10);
    CHECK(validate(s).has_value());
    REQUIRE(applyOverrides(s, Json{{"alpha", false}}).has_value());
    CHECK(s.video.pixelFormat == OutputPixelFormat::Yuv444p10);

    s = preset("match-source");
    CHECK_FALSE(applyOverrides(s, Json{{"alpha", true}}).has_value()); // H.264 has no alpha
    REQUIRE(applyOverrides(s, Json{{"container", "mp3"}}).has_value());
    CHECK(s.audio.codec == AudioCodec::Mp3);
    CHECK_FALSE(s.video.enabled);
    CHECK(validate(s).has_value());

    s = preset("match-source");
    REQUIRE(applyOverrides(s, Json{{"container", "image"}, {"videoCodec", "tiff"}, {"alpha", true}}).has_value());
    CHECK(s.video.codec == VideoCodec::Tiff);
    CHECK(s.video.pixelFormat == OutputPixelFormat::Rgba);
    CHECK_FALSE(s.audio.enabled);
    CHECK(validate(s).has_value());

    s = preset("match-source");
    REQUIRE(applyOverrides(s, Json{{"customRange", Json{{"start", 100}, {"end", 400}}}}).has_value());
    CHECK(s.range == RangeMode::Custom);
    CHECK(s.customRange == TimeRange{Ticks{100}, Ticks{300}});
    CHECK_FALSE(applyOverrides(s, Json{{"customRange", Json{{"start", 400}, {"end", 100}}}}).has_value());
    CHECK_FALSE(applyOverrides(s, Json{{"range", "everything"}}).has_value());

    // QVariant-like values (the app's overrides go through JSON).
    s = preset("match-source");
    REQUIRE(applyOverrides(s, Json{{"quality", "20"}, {"video", 1}, {"frameRate", "59.94"}}).has_value());
    CHECK(s.video.quality == 20);
    CHECK(s.video.frameRate == Rate{60000, 1001});
}

TEST_CASE("PresetStore: Save Preset, Import Preset, Export Preset and Delete", "[export][presets]") {
    const auto dir = freshOutputDir("presets");
    PresetStore store(dir / "store");
    CHECK(store.list().empty());
    ExportSettings s = preset("youtube-1080p");
    s.video.bitrate = 20'000'000;
    const auto saved = store.save("My YouTube", "20 Mb/s", s, false);
    REQUIRE(saved.has_value());
    CHECK(saved->id == "user-my-youtube");
    CHECK(std::filesystem::is_regular_file(dir / "store" / "user-my-youtube.json"));
    CHECK_FALSE(store.save("My YouTube", "", s, false).has_value()); // taken
    CHECK(store.save("My YouTube", "again", s, true).has_value());   // replaced
    const auto found = store.findAny("user-my-youtube");
    REQUIRE(found.has_value());
    CHECK(found->settings.video.bitrate == 20'000'000);
    CHECK(store.findAny("youtube-4k").has_value()); // built-in presets too

    REQUIRE(store.exportFile("user-my-youtube", dir / "shared.json").has_value());
    REQUIRE(store.remove("user-my-youtube").has_value());
    CHECK_FALSE(store.find("user-my-youtube").has_value());
    CHECK_FALSE(store.remove("youtube-4k").has_value()); // built-in
    const auto imported = store.importFile(dir / "shared.json", false);
    REQUIRE(imported.has_value());
    CHECK(imported->name == "My YouTube");
    CHECK(store.list().size() == 1);

    // A broken file in the folder is left out and reported, never fatal.
    {
        std::ofstream broken(dir / "store" / "user-broken.json", std::ios::binary);
        broken << R"({"format":"wabcut-export-preset","version":1,"name":"Broken","settings":{"video":{"quality":"x"}}})";
    }
    CHECK(store.list().size() == 1);
    CHECK(store.problems().size() == 1);
    CHECK(userPresetId("  Client: Delivery #2 ") == "user-client-delivery-2");
}

// ---- Formats -------------------------------------------------------------------------------------------------------

TEST_CASE("ProRes 4444 QuickTime Animation and CineForm keep the alpha channel", "[export][media][alpha]") {
    const TestProject project = emptyProject(320, 180, Rate{10, 1}, 0.5);
    const auto dir = freshOutputDir("alpha");
    struct Case {
        const char* preset;
        const char* file;
        const char* codec;
    };
    for (const Case c : {Case{"prores-4444-alpha", "prores.mov", "prores"}, Case{"animation-alpha", "anim.mov", "qtrle"},
                         Case{"cineform-alpha", "cfhd.mov", "cfhd"}}) {
        INFO(c.preset);
        ExportSettings s = preset(c.preset);
        s.audio.enabled = false;
        const auto r = runExport(project, s, dir / c.file);
        REQUIRE(r.has_value());
        const auto probe = ffprobe(dir / c.file);
        REQUIRE(probe.has_value());
        const auto* v = probe->stream("video");
        REQUIRE(v);
        CHECK(probeValue(v, "codec_name") == c.codec);
        const std::string pixfmt = probeValue(v, "pix_fmt");
        CHECK((pixfmt.find("yuva") == 0 || pixfmt.find("argb") == 0 || pixfmt.find("gbrap") == 0 || pixfmt.find("rgba") == 0));
        const auto frame = firstFrameRgba(dir / c.file);
        REQUIRE(frame.has_value());
        // The synthetic pattern of an alpha export: the bottom-right sixteenth transparent, the rest opaque.
        CHECK(frame->at(frame->width - 8, frame->height - 8)[3] <= 2);
        CHECK(frame->at(frame->width / 4, frame->height / 2)[3] >= 253);
    }
    CHECK(preset("prores-4444-alpha").video.pixelFormat == OutputPixelFormat::Yuva444p10);
}

TEST_CASE("Image sequences: PNG with alpha TIFF DPX JPEG Targa OpenEXR and a single still", "[export][media][images]") {
    const TestProject project = emptyProject(320, 180, Rate{10, 1}, 0.5); // 5 frames
    const auto dir = freshOutputDir("images");
    struct Case {
        const char* preset;
        const char* name;
        const char* ext;
        const char* codec;
    };
    for (const Case c : {Case{"png-sequence", "png", ".png", "png"}, Case{"tiff-sequence", "tif", ".tif", "tiff"},
                         Case{"dpx-sequence", "dpx", ".dpx", "dpx"}, Case{"jpeg-sequence", "jpg", ".jpg", "mjpeg"},
                         Case{"targa-sequence", "tga", ".tga", "targa"}, Case{"exr-sequence", "exr", ".exr", "exr"}}) {
        INFO(c.preset);
        const auto folder = dir / c.name;
        std::filesystem::create_directories(folder);
        const auto r = runExport(project, preset(c.preset), folder / (std::string("Shot") + c.ext));
        REQUIRE(r.has_value());
        CHECK(r->imageFiles == 5);
        CHECK(r->output == folder / (std::string("Shot000") + c.ext));
        int files = 0;
        for (const auto& e : std::filesystem::directory_iterator(folder)) {
            files += e.path().extension() == c.ext ? 1 : 0;
            CHECK(pathToUtf8(e.path().filename()).find(".wabcut-partial") == std::string::npos);
        }
        CHECK(files == 5);
        CHECK(std::filesystem::is_regular_file(folder / (std::string("Shot004") + c.ext)));
        const auto probe = ffprobe(folder / (std::string("Shot002") + c.ext));
        REQUIRE(probe.has_value());
        CHECK(probeValue(probe->stream("video"), "codec_name") == c.codec);
        CHECK(probeValue(probe->stream("video"), "width") == "320");
    }
    // PNG with alpha: the transparent corner.
    const auto png = firstFrameRgba(dir / "png" / "Shot000.png");
    REQUIRE(png.has_value());
    CHECK(png->at(png->width - 4, png->height - 4)[3] == 0);
    CHECK(png->at(10, png->height / 2)[3] == 255);

    // Export As Sequence off: one still, named as asked.
    const auto stillDir = dir / "still";
    std::filesystem::create_directories(stillDir);
    const auto still = runExport(project, preset("png-still"), stillDir / "Frame.png");
    REQUIRE(still.has_value());
    CHECK(still->imageFiles == 1);
    CHECK(std::filesystem::is_regular_file(stillDir / "Frame.png"));
    CHECK(std::distance(std::filesystem::directory_iterator(stillDir), std::filesystem::directory_iterator()) == 1);
}

TEST_CASE("Audio-only exports: MP3 AAC in M4A AIFF and Broadcast WAV", "[export][media][audio]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    project.editSequence([](Sequence& s) { s.startTimecode = Ticks::fromSeconds(3600); });
    const auto dir = freshOutputDir("audio_only");

    const auto m4a = runExport(project, preset("audio-aac"), dir / "a.m4a");
    REQUIRE(m4a.has_value());
    auto probe = ffprobe(dir / "a.m4a");
    REQUIRE(probe.has_value());
    CHECK(probeValue(probe->stream("audio"), "codec_name") == "aac");
    CHECK(probe->stream("video") == nullptr);

    const auto aiff = runExport(project, preset("audio-aiff"), dir / "a.aif");
    REQUIRE(aiff.has_value());
    probe = ffprobe(dir / "a.aif");
    REQUIRE(probe.has_value());
    CHECK(probeValue(probe->stream("audio"), "codec_name") == "pcm_s24be");
    CHECK(aiff->audioSamples == 48'000);

    // Media Foundation refuses this (STA) thread: the job runs the export on a thread of its own.
    const auto mp3 = runExport(project, preset("audio-mp3"), dir / "a.mp3");
    if (!mp3 && mp3.error().code == ErrorCode::Unsupported) {
        WARN("no MP3 encoder in this FFmpeg build (mp3_mf is Windows' Media Foundation encoder)");
    } else {
        INFO((mp3 ? std::string() : mp3.error().message));
        REQUIRE(mp3.has_value()); // on this (Qt GUI, STA) thread too: the job encodes on a thread of its own
        probe = ffprobe(dir / "a.mp3");
        REQUIRE(probe.has_value());
        CHECK(probeValue(probe->stream("audio"), "codec_name") == "mp3");
        CHECK(probeValue(probe->stream("audio"), "channels") == "2");
        const double seconds = std::stod(probeValue(&probe->format, "duration"));
        CHECK(std::abs(seconds - 1.0) < 0.1);
    }

    // Broadcast WAV: the sequence's start timecode (01:00:00:00) as samples since midnight.
    const auto wav = runExport(project, preset("audio-wav"), dir / "a.wav");
    REQUIRE(wav.has_value());
    CHECK(wav->startTimecode == "01:00:00:00");
    probe = ffprobe(dir / "a.wav");
    REQUIRE(probe.has_value());
    CHECK(probeValue(&probe->format, "TAG:time_reference") == "172800000");
    CHECK(probeValue(&probe->format, "TAG:encoded_by") == "Wabcut"); // FFmpeg reads bext's originator as encoded_by
    CHECK(probeValue(&probe->format, "TAG:comment") == "Export test"); // and its description as the comment
}

TEST_CASE("DNxHD 1080p HQ uses Avid's bitrate for the frame rate", "[export][media]") {
    const TestProject project = emptyProject(1920, 1080, Rate{30, 1}, 0.2);
    const auto dir = freshOutputDir("dnxhd");
    ExportSettings s = preset("dnxhd-1080p-hq");
    s.audio.enabled = false;
    const auto r = runExport(project, s, dir / "hq.mov");
    REQUIRE(r.has_value());
    const auto probe = ffprobe(dir / "hq.mov");
    REQUIRE(probe.has_value());
    const auto* v = probe->stream("video");
    CHECK(probeValue(v, "codec_name") == "dnxhd");
    CHECK(probeValue(v, "profile") == "DNXHD");
    const double bitrate = std::stod(probeValue(v, "bit_rate"));
    CHECK(bitrate > 200e6);
    CHECK(bitrate < 240e6);
}

// ---- Metadata, subtitles -------------------------------------------------------------------------------------------

TEST_CASE("Metadata: chapters from chapter markers the timecode track and XMP", "[export][media][metadata]") {
    TestProject project = emptyProject(320, 180, Rate{25, 1}, 1.0);
    project.editSequence([](Sequence& s) {
        s.startTimecode = Ticks::fromSeconds(36000); // 10:00:00:00
        Marker intro;
        intro.start = Ticks{};
        intro.name = "Intro";
        intro.type = MarkerType::Chapter;
        Marker part;
        part.start = Ticks{kTicksPerSecond / 2};
        part.name = "Part 2";
        part.type = MarkerType::Chapter;
        Marker note;
        note.start = Ticks{kTicksPerSecond / 4};
        note.name = "Fix the colour";
        note.comment = "warmer";
        s.markers = {intro, note, part};
    });
    const auto dir = freshOutputDir("metadata");
    ExportSettings s = preset("match-source");
    s.video.encoder = "libx264";
    s.metadata.xmp = XmpMode::Embed;
    s.metadata.title = "Episode 12";
    s.metadata.description = "The final cut";
    const auto mp4 = runExport(project, s, dir / "chapters.mp4");
    REQUIRE(mp4.has_value());
    CHECK(mp4->chapters == 2);
    CHECK(mp4->startTimecode == "10:00:00:00");
    CHECK(mp4->xmpEmbedded);
    const auto chapters = ffprobeText(dir / "chapters.mp4", "-show_chapters");
    REQUIRE(chapters.has_value());
    CHECK(chapters->find("title=Intro") != std::string::npos);
    CHECK(chapters->find("title=Part 2") != std::string::npos);
    const auto probe = ffprobe(dir / "chapters.mp4");
    REQUIRE(probe.has_value());
    bool timecode = false;
    for (const auto& st : probe->streams) {
        timecode = timecode || (st.contains("TAG:timecode") && st.at("TAG:timecode") == "10:00:00:00");
    }
    CHECK(timecode);
    CHECK(probeValue(&probe->format, "TAG:title") == "Episode 12");
    const std::string bytes = fileText(dir / "chapters.mp4");
    const std::string uuid = "uuid\xBE\x7A\xCF\xCB\x97\xA9\x42\xE8\x9C\x71\x99\x94\x91\xE3\xAF\xAC";
    CHECK(bytes.find(uuid) != std::string::npos);
    CHECK(bytes.find("<rdf:li xml:lang=\"x-default\">Episode 12</rdf:li>") != std::string::npos);
    CHECK(bytes.find("<xmpDM:timeValue>10:00:00:00</xmpDM:timeValue>") != std::string::npos);
    CHECK(bytes.find("<xmpDM:name>Fix the colour</xmpDM:name>") != std::string::npos);
    // The file still plays: every frame is there.
    CHECK(countVideoPackets(dir / "chapters.mp4") == 25);

    // MKV: native chapters, the XMP as a sidecar.
    s.container = Container::Mkv;
    s.metadata.xmp = XmpMode::Sidecar;
    const auto mkv = runExport(project, s, dir / "chapters.mkv");
    REQUIRE(mkv.has_value());
    CHECK(mkv->chapters == 2);
    CHECK(mkv->xmp == dir / "chapters.xmp");
    CHECK(fileText(dir / "chapters.xmp").find("<xmpDM:type>Chapter</xmpDM:type>") != std::string::npos);
    const auto mkvChapters = ffprobeText(dir / "chapters.mkv", "-show_chapters");
    REQUIRE(mkvChapters.has_value());
    CHECK(mkvChapters->find("title=Part 2") != std::string::npos);

    // ProRes in QuickTime with Set Start Timecode.
    ExportSettings mov = preset("prores-422-hq");
    mov.audio.enabled = false;
    mov.metadata.startTimecode = Ticks::fromSeconds(3600) + Ticks{kTicksPerSecond / 5}; // 01:00:00:05 at 25 fps
    const auto prores = runExport(project, mov, dir / "tc.mov");
    REQUIRE(prores.has_value());
    CHECK(prores->startTimecode == "01:00:00:05");
    const auto movProbe = ffprobe(dir / "tc.mov");
    REQUIRE(movProbe.has_value());
    CHECK(streamOfCodec(*movProbe, "prores") != nullptr);
    bool tmcd = false;
    for (const auto& st : movProbe->streams) {
        tmcd = tmcd || probeValue(&st, "codec_tag_string") == "tmcd";
    }
    CHECK(tmcd);
}

TEST_CASE("Soft subtitle tracks: 3GPP Timed Text in MP4 and SubRip in MKV", "[export][media][captions]") {
    TestProject project = emptyProject(320, 180, Rate{25, 1}, 1.0);
    project.editSequence([&](Sequence& s) {
        CaptionTrack track;
        track.id = project.state.ids.allocate<CaptionTrackId>();
        track.name = "English";
        track.language = "en";
        track.cues.push_back(CaptionCue{Ticks{kTicksPerSecond / 10}, Ticks{kTicksPerSecond / 4}, "Hello", {}});
        track.cues.push_back(CaptionCue{Ticks{kTicksPerSecond / 2}, Ticks{kTicksPerSecond / 4}, "World\nline two", {}});
        s.captionTracks.push_back(track);
    });
    const auto dir = freshOutputDir("subtitles");
    ExportSettings s = preset("match-source");
    s.video.encoder = "libx264";
    s.subtitles.enabled = true;
    const auto mp4 = runExport(project, s, dir / "subs.mp4");
    REQUIRE(mp4.has_value());
    CHECK(mp4->subtitleCues == 2);
    auto probe = ffprobe(dir / "subs.mp4");
    REQUIRE(probe.has_value());
    const auto* sub = streamOfCodec(*probe, "mov_text");
    REQUIRE(sub);
    CHECK(probeValue(sub, "TAG:language") == "eng");
    const auto packets = ffprobeText(dir / "subs.mp4", "-select_streams s -show_packets");
    REQUIRE(packets.has_value());
    CHECK(packets->find("pts_time=0.100000") != std::string::npos);
    CHECK(packets->find("pts_time=0.500000") != std::string::npos);
    CHECK(countVideoPackets(dir / "subs.mp4") == 25);

    s.container = Container::Mkv;
    const auto mkv = runExport(project, s, dir / "subs.mkv");
    REQUIRE(mkv.has_value());
    probe = ffprobe(dir / "subs.mkv");
    REQUIRE(probe.has_value());
    CHECK(streamOfCodec(*probe, "subrip") != nullptr);

    // Without captions there is no track, and a warning says so.
    const TestProject bare = emptyProject(320, 180, Rate{25, 1}, 0.4);
    s.container = Container::Mp4;
    const auto none = runExport(bare, s, dir / "none.mp4");
    REQUIRE(none.has_value());
    CHECK(none->subtitleCues == 0);
    CHECK_FALSE(none->warnings.empty());
    ExportSettings wav = preset("audio-wav");
    wav.subtitles.enabled = true;
    CHECK_FALSE(validate(wav).has_value());
}

// ---- Effects -------------------------------------------------------------------------------------------------------

TEST_CASE("Export effects: image name and timecode overlays are burned into the output frame", "[export][media][effects]") {
    const TestProject project = emptyProject(640, 360, Rate{10, 1}, 0.5);
    const auto dir = freshOutputDir("overlays");
    QImage logo(40, 40, QImage::Format_ARGB32);
    logo.fill(QColor(0, 255, 0));
    REQUIRE(logo.save(QString::fromStdU16String((dir / "logo.png").u16string())));

    ExportSettings plain = preset("png-sequence");
    plain.video.pixelFormat = OutputPixelFormat::Rgb24;
    std::filesystem::create_directories(dir / "plain");
    REQUIRE(runExport(project, plain, dir / "plain" / "f.png").has_value());

    ExportSettings fx = plain;
    fx.effects.image.applied = true;
    fx.effects.image.image = pathToUtf8(dir / "logo.png");
    fx.effects.image.placement = OverlayPlacement{OverlayPosition::TopLeft, 200, 100, 100.0, 100.0};
    fx.effects.name.applied = true;
    fx.effects.name.prefix = "REVIEW ";
    fx.effects.timecode.applied = true;
    std::filesystem::create_directories(dir / "fx");
    const auto r = runExport(project, fx, dir / "fx" / "f.png");
    REQUIRE(r.has_value());

    const auto a = firstFrameRgba(dir / "plain" / "f000.png");
    const auto b = firstFrameRgba(dir / "fx" / "f000.png");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    // The logo: solid green at (200..240, 100..140).
    const auto green = b->at(220, 120);
    CHECK(green[0] <= 2);
    CHECK(green[1] >= 253);
    CHECK(green[2] <= 2);
    CHECK(a->at(220, 120) != green);
    // Name (bottom centre) and timecode (top centre): a dark box with white text where the plain export has the pattern.
    const auto region = [](const RgbaImage& img, int x0, int y0, int x1, int y1) {
        double sum = 0.0;
        int white = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const auto p = img.at(x, y);
                sum += (p[0] + p[1] + p[2]) / 3.0;
                white += p[0] > 235 && p[1] > 235 && p[2] > 235 ? 1 : 0;
            }
        }
        return std::pair<double, int>{sum / ((x1 - x0) * (y1 - y0)), white};
    };
    const auto [plainTop, plainTopWhite] = region(*a, 300, 34, 340, 50);
    const auto [fxTop, fxTopWhite] = region(*b, 300, 34, 340, 50);
    CHECK(fxTop < plainTop);
    CHECK(fxTopWhite > plainTopWhite);
    const auto [plainBottom, plainBottomWhite] = region(*a, 300, 360 - 66, 340, 360 - 52);
    const auto [fxBottom, fxBottomWhite] = region(*b, 300, 360 - 66, 340, 360 - 52);
    CHECK(fxBottom < plainBottom);
    CHECK(fxBottomWhite > plainBottomWhite);
    // Away from the overlays nothing changes.
    CHECK(a->at(600, 200) == b->at(600, 200));

    // Missing image: the export refuses before it starts.
    fx.effects.image.image = pathToUtf8(dir / "missing.png");
    const auto missing = runExport(project, fx, dir / "fx" / "g.png");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("Export effects: Lumetri Look on export and Video Limiter", "[export][media][effects]") {
    const TestProject project = emptyProject(320, 180, Rate{10, 1}, 0.3);
    const auto dir = freshOutputDir("look_limiter");
    ExportSettings s = preset("png-sequence");
    s.video.pixelFormat = OutputPixelFormat::Rgb24;
    s.effects.look.applied = true;
    s.effects.look.lut = "builtin:mono-silver";
    std::filesystem::create_directories(dir / "look");
    REQUIRE(runExport(project, s, dir / "look" / "f.png").has_value());
    const auto look = firstFrameRgba(dir / "look" / "f000.png");
    REQUIRE(look.has_value());
    int colourful = 0;
    for (int y = 40; y < look->height; y += 7) {
        for (int x = 0; x < look->width; x += 7) {
            const auto p = look->at(x, y);
            colourful += std::max({std::abs(p[0] - p[1]), std::abs(p[1] - p[2]), std::abs(p[0] - p[2])}) > 12 ? 1 : 0;
        }
    }
    CHECK(colourful == 0);

    s.effects.look.applied = false;
    s.effects.limiter.applied = true;
    s.effects.limiter.clipLevel = 90.0;
    s.effects.limiter.compression = 0.0;
    std::filesystem::create_directories(dir / "limit");
    REQUIRE(runExport(project, s, dir / "limit" / "f.png").has_value());
    const auto limited = firstFrameRgba(dir / "limit" / "f000.png");
    REQUIRE(limited.has_value());
    int over = 0;
    for (std::size_t i = 0; i < limited->pixels.size(); i += 4) {
        over += std::max({limited->pixels[i], limited->pixels[i + 1], limited->pixels[i + 2]}) > 231 ? 1 : 0;
    }
    CHECK(over == 0);

    // HDR exports refuse them (the look is display-referred).
    ExportSettings hdr = preset("youtube-hdr-pq");
    hdr.effects.limiter.applied = true;
    CHECK_FALSE(validate(hdr).has_value());
}

TEST_CASE("Loudness Normalization meets the target under the true-peak ceiling", "[export][media][audio]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(2));
    const auto dir = freshOutputDir("loudness");
    ExportSettings s = preset("audio-wav");
    s.effects.loudness.applied = true;
    s.effects.loudness.standard = LoudnessStandard::Custom;
    s.effects.loudness.targetLufs = -16.0;
    s.effects.loudness.maxTruePeakDbtp = -1.5;
    s.effects.loudness.toleranceLu = 0.5;
    s.effects.loudness.writeLog = true;
    const auto r = runExport(project, s, dir / "loud.wav");
    REQUIRE(r.has_value());
    REQUIRE(r->loudnessLufs.has_value());
    CHECK(r->loudnessGainDb != 0.0);
    CHECK(r->loudnessLog == dir / "loud.wav.loudness.txt");
    CHECK(fileText(r->loudnessLog).find("Measured loudness") != std::string::npos);
    const auto decoded = decodeAudio(dir / "loud.wav");
    REQUIRE(decoded.has_value());
    audio::dsp::LoudnessMeter meter(decoded->channels, decoded->sampleRate);
    meter.add(decoded->samples.data(), decoded->samples.size() / static_cast<std::size_t>(decoded->channels));
    const auto measured = meter.result();
    REQUIRE(measured.hasIntegrated());
    metric(std::format("loudness normalization: {:.2f} LUFS (target -16), true peak {:.2f} dBTP (ceiling -1.5)",
                       measured.integratedLufs, measured.truePeakDbtp));
    CHECK(std::abs(measured.integratedLufs - (-16.0)) < 1.0);
    CHECK(measured.truePeakDbtp < -1.5 + 0.3);
    CHECK(decoded->samples.size() == 96'000u * static_cast<std::size_t>(decoded->channels)); // sample-exact length
}

// ---- Queue, watch folders ------------------------------------------------------------------------------------------

TEST_CASE("ExportQueue: held jobs wait for start and running jobs pause and resume", "[export][media][queue]") {
    const TestProject project = emptyProject(640, 360, Rate{30, 1}, 3.0);
    const auto dir = freshOutputDir("queue_pause");
    ExportQueue queue;
    ExportSettings s = preset("match-source");
    s.video.encoder = "libx264";
    s.audio.enabled = false;

    const ExportJobId held = queue.enqueue(makeRequest(project, preset("audio-wav"), dir / "held.wav"), true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto status = queue.status(held);
    REQUIRE(status.has_value());
    CHECK(status->held);
    CHECK(status->progress.phase == ExportPhase::Queued);
    queue.start(held);
    status = queue.wait(held);
    REQUIRE(status.has_value());
    CHECK(status->progress.phase == ExportPhase::Done);
    CHECK_FALSE(status->held);

    std::atomic<std::int64_t> frames{0};
    ExportRequest slow = makeRequest(project, s, dir / "slow.mp4");
    slow.onProgress = [&](const ExportProgress& p) { frames = p.framesDone; };
    const ExportJobId running = queue.enqueue(std::move(slow));
    while (frames.load() == 0 && queue.status(running)->progress.phase != ExportPhase::Done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    queue.pause(running);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (queue.status(running)->progress.phase != ExportPhase::Paused &&
           queue.status(running)->progress.phase != ExportPhase::Done && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (queue.status(running)->progress.phase == ExportPhase::Paused) {
        const std::int64_t at = queue.status(running)->progress.framesDone;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(queue.status(running)->progress.framesDone == at); // nothing rendered while paused
        CHECK(queue.status(running)->progress.phase == ExportPhase::Paused);
        queue.resume(running);
    } else {
        WARN("the export finished before the pause reached it");
    }
    status = queue.wait(running);
    REQUIRE(status.has_value());
    CHECK(status->progress.phase == ExportPhase::Done);
    CHECK(countVideoPackets(dir / "slow.mp4") == 90);
    queue.remove(running);
    CHECK_FALSE(queue.status(running).has_value());
}

TEST_CASE("Watch folder: a dropped file is exported with the preset and moved into Source", "[export][media][watch]") {
    if (!haveMedia("sine_48k.wav")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("watch");
    ExportQueue queue;
    WatchFolderService::Options options;
    options.interval = std::chrono::milliseconds(0);
    options.settleScans = 1;
    options.videoSource = syntheticVideoSourceFactory();
    WatchFolderService watch(queue, options);
    WatchFolder folder;
    folder.folder = dir / "Drop";
    folder.presetId = "audio-aac";
    folder.settings = preset("audio-aac");
    const auto id = watch.add(folder);
    REQUIRE(id.has_value());
    CHECK(std::filesystem::is_directory(dir / "Drop" / "Output"));
    CHECK_FALSE(watch.add(folder).has_value()); // watched already

    std::filesystem::copy_file(mediaDir() / "sine_48k.wav", dir / "Drop" / "tone.wav");
    std::ofstream(dir / "Drop" / "notes.txt") << "not media";
    CHECK(watch.scanNow().empty()); // seen once: maybe still being copied
    const auto jobs = watch.scanNow();
    REQUIRE(jobs.size() == 1);
    const auto status = queue.wait(jobs.front());
    REQUIRE(status.has_value());
    CHECK(status->progress.phase == ExportPhase::Done);
    CHECK(std::filesystem::is_regular_file(dir / "Drop" / "Output" / "tone.m4a"));
    // The source moves once encoded (the job's completion runs on the queue's thread).
    for (int i = 0; i < 100 && !std::filesystem::exists(dir / "Drop" / "Source" / "tone.wav"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(std::filesystem::is_regular_file(dir / "Drop" / "Source" / "tone.wav"));
    CHECK_FALSE(std::filesystem::exists(dir / "Drop" / "tone.wav"));
    CHECK(watch.scanNow().empty()); // nothing new; the text file is not media
    const auto folders = watch.folders();
    REQUIRE(folders.size() == 1);
    CHECK(folders[0].done == 1);
    CHECK(folders[0].failed == 0);
    CHECK(watch.remove(*id));
    CHECK(watch.folders().empty());

    // The project a watch folder exports: the file whole at its own size and rate.
    const auto project = projectForMedia(mediaDir() / "prores_422.mov");
    REQUIRE(project.has_value());
    const Sequence& seq = *project->sequences.front();
    CHECK(seq.width == 1280);
    CHECK(seq.height == 720);
    CHECK(seq.frameRate == Rate{24000, 1001});
    CHECK(seq.videoTracks.size() == 1);
    CHECK_FALSE(projectForMedia(dir / "Drop" / "notes.txt").has_value());
}

// ---- Smart rendering -----------------------------------------------------------------------------------------------

TEST_CASE("Smart rendering copies untouched ProRes source frames and renders the rest", "[export][media][smart]") {
    if (!haveMedia("prores_422.mov")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const Rate rate{24000, 1001};
    TestProject project(1280, 720, rate, framesToTicks(24, rate));
    const MediaId media = project.addMedia("prores_422.mov");
    const Ticks half = framesToTicks(12, rate);
    project.addVideoTrack(media, half, half, Ticks{}); // frames 0..11: a gap that renders; 12..23: the source
    const auto dir = freshOutputDir("smart_render");
    ExportSettings s = preset("prores-422-hq");
    s.video.proresProfile = ProResProfile::Standard; // the source is ProRes 422 ('apcn')
    s.audio.enabled = false;
    const auto r = runExport(project, s, dir / "smart.mov");
    REQUIRE(r.has_value());
    CHECK(r->copiedFrames == 12);
    CHECK(r->frames == 24);
    const auto out = videoPackets(dir / "smart.mov");
    const auto source = videoPackets(mediaDir() / "prores_422.mov");
    REQUIRE(out.size() == 24);
    REQUIRE(source.size() >= 12);
    for (std::size_t i = 0; i < 12; ++i) {
        INFO(i);
        CHECK(out[12 + i] == source[i]); // bit for bit: never re-encoded
    }
    CHECK(out[0] != source[0]);
    int decoded = 0;
    REQUIRE(decodeVideo(dir / "smart.mov", [&](const YuvFrame&) { ++decoded; }));
    CHECK(decoded == 24);

    // Turned off, or a format that differs (ProRes HQ), everything renders.
    s.render.smartRender = false;
    const auto off = runExport(project, s, dir / "off.mov");
    REQUIRE(off.has_value());
    CHECK(off->copiedFrames == 0);
    s.render.smartRender = true;
    s.video.proresProfile = ProResProfile::Hq;
    const auto hq = runExport(project, s, dir / "hq.mov");
    REQUIRE(hq.has_value());
    CHECK(hq->copiedFrames == 0);
}

TEST_CASE("Use Previews copies the frames of a valid preview segment", "[export][media][smart]") {
    const Rate rate{25, 1};
    const TestProject project = emptyProject(640, 360, rate, 1.0);
    const auto dir = freshOutputDir("use_previews");
    auto cache = std::make_shared<PreviewCache>(dir / "Preview Files");
    std::filesystem::create_directories(cache->folder());
    // A rendered preview segment over the first half second, as render.previewInOut makes it (ProRes LT, the
    // sequence's size and rate).
    const TimeRange segment{Ticks{}, framesToTicks(12, rate)}; // 12 frames
    const std::string hash = renderContentHash(project.state, project.sequence, segment);
    const std::string name = PreviewCache::fileNameFor(project.sequence, segment, hash);
    ExportSettings previewSettings = preset("prores-422-hq");
    previewSettings.video.proresProfile = ProResProfile::Lt;
    previewSettings.audio.enabled = false;
    previewSettings.range = RangeMode::Custom;
    previewSettings.customRange = segment;
    REQUIRE(runExport(project, previewSettings, cache->folder() / name).has_value());
    cache->record(PreviewEntry{project.sequence, segment, hash, name,
                               static_cast<std::int64_t>(std::filesystem::file_size(cache->folder() / name)), 0});

    ExportSettings s = preset("prores-422-hq");
    s.video.proresProfile = ProResProfile::Lt;
    s.audio.enabled = false;
    s.render.usePreviews = true;
    s.render.smartRender = false;
    ExportRequest request = makeRequest(project, s, dir / "export.mov");
    request.previews = cache;
    ExportJob job(std::move(request));
    const auto r = job.run();
    REQUIRE(r.has_value());
    CHECK(r->copiedFrames == 12); // the frames inside the segment
    const auto out = videoPackets(dir / "export.mov");
    const auto preview = videoPackets(cache->folder() / name);
    REQUIRE(out.size() == 25);
    REQUIRE(preview.size() >= 12);
    for (std::size_t i = 0; i < 12; ++i) {
        CHECK(out[i] == preview[i]);
    }

    // Without Use Previews nothing is copied.
    s.render.usePreviews = false;
    ExportRequest plain = makeRequest(project, s, dir / "plain.mov");
    plain.previews = cache;
    const auto p = ExportJob(std::move(plain)).run();
    REQUIRE(p.has_value());
    CHECK(p->copiedFrames == 0);
}

// ---- Render toggles, CPU limit -------------------------------------------------------------------------------------

TEST_CASE("Use Maximum Render Quality renders at the sequence size and scales down", "[export][media]") {
    const TestProject project = emptyProject(1280, 720, Rate{10, 1}, 0.3);
    const auto dir = freshOutputDir("max_quality");
    std::vector<int> widths;
    const auto capturing = [&](const ProjectState& state, const Sequence& seq, const VideoSourceFormat& format) {
        widths.push_back(format.width);
        return syntheticVideoSourceFactory()(state, seq, format);
    };
    ExportSettings s = preset("match-source");
    s.video.encoder = "libx264";
    s.video.width = 640;
    s.video.height = 360;
    s.audio.enabled = false;
    for (const bool maximum : {false, true}) {
        s.render.maximumRenderQuality = maximum;
        ExportRequest request = makeRequest(project, s, dir / (maximum ? "max.mp4" : "normal.mp4"));
        request.videoSource = capturing;
        REQUIRE(ExportJob(std::move(request)).run().has_value());
        const auto probe = ffprobe(dir / (maximum ? "max.mp4" : "normal.mp4"));
        REQUIRE(probe.has_value());
        CHECK(probeValue(probe->stream("video"), "width") == "640");
    }
    REQUIRE(widths.size() == 2);
    CHECK(widths[0] == 640);
    CHECK(widths[1] == 1280);
}

TEST_CASE("The CPU limit preference caps export threads", "[export][settings]") {
    struct Restore {
        ~Restore() { setExportThreadLimit(0); }
    } restore;
    CHECK(threadLimitForPercent(100) == 0);
    const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    CHECK(threadLimitForPercent(50) == std::max(1, cores / 2));
    CHECK(threadLimitForPercent(1) >= 1);
    setExportThreadLimit(1);
    CHECK(exportThreadLimit() == 1);
    const TestProject project = emptyProject(320, 180, Rate{10, 1}, 0.5);
    const auto dir = freshOutputDir("cpu_limit");
    ExportSettings s = preset("match-source");
    s.video.encoder = "libx264";
    s.audio.enabled = false;
    const auto r = runExport(project, s, dir / "one-thread.mp4");
    REQUIRE(r.has_value());
    CHECK(countVideoPackets(dir / "one-thread.mp4") == 5);
}

// ---- Commands ------------------------------------------------------------------------------------------------------

TEST_CASE("Export commands: Quick Export the queue presets and watch folders", "[export][commands]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    const auto dir = freshOutputDir("export_commands");
    auto services = ExportCommandServices::standalone(dir / "project.wabcut", dir / "presets", syntheticVideoSourceFactory());
    EditSession session(project.state);
    command::EnginePolicy policy;
    policy.services = services;
    command::CommandEngine engine(session, command::builtinCommands(), policy);
    const auto run = [&](const std::string& name, command::Json params, bool dryRun = false) {
        command::CommandRequest request;
        request.command = name;
        request.params = std::move(params);
        request.dryRun = dryRun;
        return engine.execute(request);
    };

    auto r = run("export.presets", command::Json::object());
    REQUIRE(r.ok());
    CHECK(r.data["presets"].size() == builtInPresets().size());

    r = run("export.describe", {{"preset", "png-sequence"}, {"output", pathToUtf8(dir / "x.mov")}});
    REQUIRE(r.ok());
    CHECK(r.data["extension"] == ".png");
    CHECK(r.data["output"] == pathToUtf8(dir / "x.png"));
    CHECK(r.data["frames"] == 30);
    r = run("export.describe", {{"preset", "match-source"}, {"in", 0}, {"out", kTicksPerSecond / 2}});
    REQUIRE(r.ok());
    CHECK(r.data["frames"] == 15);
    CHECK(run("export.describe", {{"preset", "no-such"}}).error->code == command::ErrorCode::InvalidParams);

    // Save Preset, then Quick Export with it.
    r = run("export.preset.save", {{"name", "Test 44k"}, {"preset", "audio-wav"}, {"overrides", {{"sampleRate", 44100}}}});
    REQUIRE(r.ok());
    CHECK(r.data["id"] == "user-test-44k");
    CHECK_FALSE(run("export.preset.save", {{"name", "Test 44k"}, {"preset", "audio-wav"}}).ok());
    r = run("export.presets", command::Json::object());
    CHECK(r.data["presets"].size() == builtInPresets().size() + 1);
    r = run("export.quick", {{"preset", "user-test-44k"}, {"output", pathToUtf8(dir / "quick.wav")}}, true);
    REQUIRE(r.ok());
    CHECK(r.data["queued"] == false); // a dry run queues nothing
    CHECK(run("export.queue.list", command::Json::object()).data["jobs"].empty());
    r = run("export.quick", {{"preset", "user-test-44k"}, {"output", pathToUtf8(dir / "quick.wav")}});
    REQUIRE(r.ok());
    CHECK(r.data["queued"] == true);
    const std::string job = r.data["exports"][0]["job"];
    auto finished = services->waitForJobs();
    REQUIRE(finished.size() == 1);
    CHECK(finished[0]["job"] == job);
    CHECK(finished[0]["state"] == "done");
    const auto probe = ffprobe(dir / "quick.wav");
    REQUIRE(probe.has_value());
    CHECK(probeValue(probe->stream("audio"), "sample_rate") == "44100");

    // Send to Media Encoder: two destinations, held until Start Queue.
    r = run("export.queue", {{"presets", {"audio-wav", "audio-aiff"}},
                             {"outputs", {pathToUtf8(dir / "one.wav"), pathToUtf8(dir / "two.aif")}},
                             {"hold", true}});
    REQUIRE(r.ok());
    REQUIRE(r.data["exports"].size() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto jobs = run("export.queue.list", command::Json::object()).data["jobs"];
    REQUIRE(jobs.size() == 3);
    CHECK(jobs[1]["state"] == "held");
    CHECK(jobs[2]["state"] == "held");
    CHECK_FALSE(std::filesystem::exists(dir / "one.wav"));
    REQUIRE(run("export.queue.start", {{"all", true}}).ok());
    finished = services->waitForJobs();
    CHECK(finished[1]["state"] == "done");
    CHECK(finished[2]["state"] == "done");
    CHECK(std::filesystem::is_regular_file(dir / "one.wav"));
    CHECK(std::filesystem::is_regular_file(dir / "two.aif"));
    CHECK(run("export.queue.cancel", {{"jobs", {"j99"}}}).error->code == command::ErrorCode::NotFound);
    CHECK(run("export.queue", {{"presets", {"audio-wav"}}, {"outputs", {"a.wav", "b.wav"}}}).error->code ==
          command::ErrorCode::InvalidParams);

    // Export Preset, Delete, Import Preset.
    REQUIRE(run("export.preset.export", {{"id", "user-test-44k"}, {"file", pathToUtf8(dir / "shared.json")}}).ok());
    REQUIRE(run("export.preset.delete", {{"id", "user-test-44k"}}).ok());
    CHECK_FALSE(run("export.preset.delete", {{"id", "match-source"}}).ok());
    r = run("export.preset.import", {{"file", pathToUtf8(dir / "shared.json")}});
    REQUIRE(r.ok());
    CHECK(r.data["id"] == "user-test-44k");

    // Watch folders.
    r = run("export.watchFolder.add", {{"folder", pathToUtf8(dir / "Watch")}, {"preset", "audio-wav"}});
    REQUIRE(r.ok());
    const std::string watchId = r.data["id"];
    CHECK(run("export.watchFolder.list", command::Json::object()).data["folders"].size() == 1);
    REQUIRE(run("export.watchFolder.remove", {{"id", watchId}}).ok());
    CHECK(run("export.watchFolder.list", command::Json::object()).data["folders"].empty());

    // The project never changed.
    CHECK(session.undoCount() == 0);
}
