// Proxy generation (exporter::transcodeProxy): the media suite's clips transcoded to ProRes Proxy / DNxHR LB /
// all-intra H.264 at half and quarter resolution. Every source frame must map onto the proxy frame that shows
// the same picture (the generated clips carry a frame-number barcode) at the same source time, VFR included;
// audio is copied, rotation / colour / pixel aspect / start timecode carry over, and a cancelled or refused
// transcode leaves no file behind.

#include "ExportTestUtil.h"

#include <wabcut/export/Proxy.h>
#include <wabcut/export/TrimmedCopy.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstring>
#include <format>

using namespace wabcut;
using namespace exporttest;

namespace {

/// Luma of (x, y) in 8-bit terms for the layouts the clips and proxies use.
int luma8(const media::FrameHandle& f, int x, int y) {
    const media::CpuPlanes* cpu = f.cpu();
    if (!cpu || x < 0 || y < 0 || x >= f.width || y >= f.height) {
        return -1;
    }
    const media::Plane& p = cpu->planes[0];
    const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
    std::uint16_t v = 0;
    switch (f.format) {
    case media::PixelFormat::NV12:
    case media::PixelFormat::YUV420P:
    case media::PixelFormat::YUV422P:
    case media::PixelFormat::YUV444P:
        return row[x];
    case media::PixelFormat::P010:
        std::memcpy(&v, row + 2 * x, 2);
        return v >> 8;
    case media::PixelFormat::YUV420P10:
    case media::PixelFormat::YUV422P10:
    case media::PixelFormat::YUV444P10:
        std::memcpy(&v, row + 2 * x, 2);
        return v >> 2;
    default:
        return -1;
    }
}

/// The generated clips' frame number (16 cells of 32 source pixels, top left), read from a frame of any size.
std::int64_t barcode(const media::FrameHandle& f, int sourceWidth) {
    const double s = static_cast<double>(f.width) / sourceWidth;
    std::int64_t index = 0;
    for (int bit = 0; bit < 16 && (bit * 32 + 16) < sourceWidth; ++bit) {
        if (luma8(f, static_cast<int>((bit * 32 + 16) * s), static_cast<int>(16 * s)) > 128) {
            index |= std::int64_t{1} << bit;
        }
    }
    return index;
}

std::string utf8(const std::filesystem::path& p) {
    return pathToUtf8(p);
}

struct Walk {
    std::int64_t frames = 0;
    Ticks end{}; ///< end of the last source frame
};

/// Every source frame against the proxy frame at its start: same start, same length, same barcode; and nothing
/// in the proxy after the last source frame.
Walk walkFrameExact(const std::string& source, const std::filesystem::path& proxy) {
    auto src = media::VideoDecoder::open(source, -1, media::DecoderOptions{.allowHardware = false});
    auto prx = media::VideoDecoder::open(utf8(proxy), -1, media::DecoderOptions{.allowHardware = false});
    REQUIRE(src);
    REQUIRE(prx);
    const int sourceWidth = (*src)->streamInfo().width;
    CHECK((*prx)->firstFrameTime() == (*src)->firstFrameTime());
    std::int64_t n = 0;
    std::int64_t mismatches = 0;
    Ticks t = (*src)->firstFrameTime();
    for (;;) {
        auto a = (*src)->frameAt(t);
        if (!a) {
            REQUIRE(a.error().code == ErrorCode::NotFound);
            break;
        }
        auto b = (*prx)->frameAt(a->pts);
        REQUIRE(b);
        const bool same = b->pts == a->pts && b->duration == a->duration &&
                          barcode(*b, sourceWidth) == barcode(*a, sourceWidth);
        if (!same && mismatches++ < 3) {
            UNSCOPED_INFO(std::format("frame {}: source {} s #{} ({} s long), proxy {} s #{} ({} s long)", n,
                                      a->pts.toSeconds(), barcode(*a, sourceWidth), a->duration.toSeconds(),
                                      b->pts.toSeconds(), barcode(*b, sourceWidth), b->duration.toSeconds()));
        }
        ++n;
        t = a->pts + std::max(a->duration, Ticks{1});
    }
    // The proxy ends where the source ends: nothing after the last source frame.
    auto past = (*prx)->frameAt(t);
    CHECK((!past && past.error().code == ErrorCode::NotFound));
    CHECK(mismatches == 0);
    return {n, t};
}

std::int64_t expectFrameExact(const std::string& source, const std::filesystem::path& proxy) {
    return walkFrameExact(source, proxy).frames;
}

/// Copy of `source` with a start timecode (stream-copied MOV; the clips themselves carry none).
bool remuxWithTimecode(const std::string& source, const std::filesystem::path& out, const char* timecode) {
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, source.c_str(), nullptr, nullptr) < 0) {
        return false;
    }
    avformat_find_stream_info(in, nullptr);
    AVFormatContext* o = nullptr;
    const std::string outPath = utf8(out);
    bool ok = avformat_alloc_output_context2(&o, nullptr, "mov", outPath.c_str()) >= 0;
    for (unsigned i = 0; ok && i < in->nb_streams; ++i) {
        AVStream* st = avformat_new_stream(o, nullptr);
        ok = st && avcodec_parameters_copy(st->codecpar, in->streams[i]->codecpar) >= 0;
        if (ok) {
            st->codecpar->codec_tag = 0;
            st->time_base = in->streams[i]->time_base;
            st->avg_frame_rate = in->streams[i]->avg_frame_rate; // MOV needs the rate for a tmcd track
            if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                av_dict_set(&st->metadata, "timecode", timecode, 0);
            }
        }
    }
    ok = ok && avio_open(&o->pb, outPath.c_str(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(o, nullptr) >= 0;
    AVPacket* pkt = av_packet_alloc();
    while (ok && av_read_frame(in, pkt) >= 0) {
        av_packet_rescale_ts(pkt, in->streams[pkt->stream_index]->time_base, o->streams[pkt->stream_index]->time_base);
        ok = av_interleaved_write_frame(o, pkt) >= 0;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    ok = ok && av_write_trailer(o) >= 0;
    if (o) {
        avio_closep(&o->pb);
        avformat_free_context(o);
    }
    avformat_close_input(&in);
    return ok;
}

std::string streamTag(const ProbeResult& probe, const std::string& key) {
    for (const auto& s : probe.streams) {
        if (const auto it = s.find(key); it != s.end()) {
            return it->second;
        }
    }
    if (const auto it = probe.format.find(key); it != probe.format.end()) {
        return it->second;
    }
    return {};
}

} // namespace

TEST_CASE("Proxy: settings text and frame size", "[proxy]") {
    const exporter::ProxySettings s{exporter::ProxyCodec::ProResProxy, 2, true};
    CHECK(exporter::describeProxySettings(s) == "prores_ks proxy 1/2 audio=copy");
    for (const auto codec : {exporter::ProxyCodec::ProResProxy, exporter::ProxyCodec::DnxhrLb,
                             exporter::ProxyCodec::H264Intra}) {
        for (const int divisor : {2, 4}) {
            const exporter::ProxySettings in{codec, divisor, divisor == 2};
            exporter::ProxySettings out{};
            REQUIRE(exporter::parseProxySettings(exporter::describeProxySettings(in), out));
            CHECK(out.codec == in.codec);
            CHECK(out.divisor == in.divisor);
            CHECK(out.copyAudio == in.copyAudio);
        }
    }
    CHECK(exporter::proxyFrameSize(1920, 1080, 2) == std::pair{960, 540});
    CHECK(exporter::proxyFrameSize(1920, 1080, 4) == std::pair{480, 270});
    CHECK(exporter::proxyFrameSize(1280, 720, 4) == std::pair{320, 180});
    CHECK(exporter::proxyFrameSize(1366, 768, 4) == std::pair{342, 192});
    CHECK(exporter::proxyFrameSize(3, 3, 4) == std::pair{2, 2});
}

TEST_CASE("Proxy: ProRes Proxy at half resolution maps frame for frame onto the source", "[proxy]") {
    const std::string name = GENERATE(as<std::string>{}, "h264_422_10bit.mp4", "h264_1080p60_aac.mp4",
                                      "prores_422.mov", "vfr.mp4", "rotated_90.mp4", "hevc_main10_420.mp4");
    if (!haveMedia(name)) {
        SKIP(name + " was not generated");
    }
    INFO(name);
    const std::string source = utf8(mediaDir() / name);
    const std::filesystem::path dir = freshOutputDir("proxy-" + name);
    const std::filesystem::path out = dir / "proxy.mov";
    std::int64_t lastDone = 0;
    std::int64_t progressCalls = 0;
    exporter::ProxyControl control;
    control.onProgress = [&](std::int64_t done, std::int64_t) {
        CHECK(done == lastDone + 1);
        lastDone = done;
        ++progressCalls;
    };
    auto result = exporter::transcodeProxy(source, out, {}, control);
    REQUIRE(result);
    CHECK(std::filesystem::is_regular_file(out));
    CHECK(!std::filesystem::exists(std::filesystem::path(out).concat(".partial")));
    CHECK(result->encoder == "prores_ks");
    CHECK(progressCalls == result->frames);

    auto sourceInfo = media::probe(source);
    auto proxyInfo = media::probe(utf8(out));
    REQUIRE(sourceInfo);
    REQUIRE(proxyInfo);
    REQUIRE(proxyInfo->video.size() == 1);
    const VideoStreamInfo& sv = sourceInfo->video.front();
    const VideoStreamInfo& pv = proxyInfo->video.front();
    const auto [w, h] = exporter::proxyFrameSize(sv.width, sv.height, 2);
    CHECK(pv.width == w);
    CHECK(pv.height == h);
    CHECK(pv.codec == "prores");
    CHECK(pv.rotationDegrees == sv.rotationDegrees);
    // Same displayed shape: width * pixel aspect / height.
    CHECK(std::abs(pv.width * pv.sampleAspect.toDouble() / pv.height - sv.width * sv.sampleAspect.toDouble() / sv.height) <
          1e-3);
    if (sv.frameCount > 0) {
        CHECK(pv.frameCount == sv.frameCount);
    }
    REQUIRE(proxyInfo->audio.size() == sourceInfo->audio.size());
    for (std::size_t i = 0; i < proxyInfo->audio.size(); ++i) {
        CHECK(proxyInfo->audio[i].codec == sourceInfo->audio[i].codec); // copied, not re-encoded
        CHECK(proxyInfo->audio[i].channels == sourceInfo->audio[i].channels);
        CHECK(std::abs((proxyInfo->audio[i].duration - sourceInfo->audio[i].duration).toSeconds()) < 0.05);
    }
    std::string error;
    const auto probe = ffprobe(out, &error);
    REQUIRE(probe);
    const auto* video = probe->stream("video");
    REQUIRE(video);
    CHECK(video->at("profile") == "Proxy");
    CHECK(video->at("pix_fmt") == "yuv422p10le");

    // The effective colour the decoders resolve is the same (untagged HD stays BT.709 at 540p).
    auto a = media::VideoDecoder::open(source, -1, media::DecoderOptions{.allowHardware = false});
    auto b = media::VideoDecoder::open(utf8(out), -1, media::DecoderOptions{.allowHardware = false});
    REQUIRE(a);
    REQUIRE(b);
    auto fa = (*a)->frameAt((*a)->firstFrameTime());
    auto fb = (*b)->frameAt((*b)->firstFrameTime());
    REQUIRE(fa);
    REQUIRE(fb);
    CHECK(fb->color == fa->color);
    a->reset();
    b->reset();

    const Walk walk = walkFrameExact(source, out);
    CHECK(walk.frames == result->frames);
    // The proxy lasts until the end of the last source frame (MP4 sources may state a shorter container
    // duration when B-frames shift their sample table: vfr.mp4 says 2.9 s, its last frame ends at 3.0 s).
    CHECK(std::abs((pv.duration - (walk.end - result->firstFrame)).toSeconds()) < 1e-3);
}

TEST_CASE("Proxy: DNxHR LB and all-intra H.264 at quarter resolution", "[proxy]") {
    const std::string name = "prores_422.mov";
    if (!haveMedia(name) || !haveMedia("h264_422_10bit.mp4")) {
        SKIP("clips were not generated");
    }
    const std::filesystem::path dir = freshOutputDir("proxy-codecs");
    SECTION("DNxHR LB") {
        const std::string source = utf8(mediaDir() / name);
        auto result = exporter::transcodeProxy(source, dir / "dnxhr.mov", {exporter::ProxyCodec::DnxhrLb, 4, true});
        REQUIRE(result);
        CHECK(result->encoder == "dnxhd");
        CHECK(result->width == 320);
        CHECK(result->height == 180);
        const auto probe = ffprobe(dir / "dnxhr.mov");
        REQUIRE(probe);
        CHECK(probe->stream("video")->at("profile") == "DNXHR LB");
        CHECK(probe->stream("audio")->at("codec_name") == "pcm_s24le");
        CHECK(expectFrameExact(source, dir / "dnxhr.mov") == 48);
    }
    SECTION("H.264 intra (hardware encoder when it works)") {
        const std::string source = utf8(mediaDir() / "h264_422_10bit.mp4");
        auto result = exporter::transcodeProxy(source, dir / "h264.mov", {exporter::ProxyCodec::H264Intra, 4, true});
        REQUIRE(result);
        UNSCOPED_INFO("encoder " << result->encoder);
        CHECK((result->encoder == "libx264" || result->encoder.ends_with("_nvenc") ||
               result->encoder.ends_with("_amf") || result->encoder.ends_with("_qsv")));
        const auto probe = ffprobe(dir / "h264.mov");
        REQUIRE(probe);
        CHECK(probe->stream("video")->at("codec_name") == "h264");
        CHECK(probe->stream("video")->at("pix_fmt") == "yuv420p");
        CHECK(probe->stream("video")->at("has_b_frames") == "0");
        CHECK(expectFrameExact(source, dir / "h264.mov") == 60);
        // Intra only: every frame decodes on its own, so a backwards jump lands without a preroll.
        auto d = media::VideoDecoder::open(utf8(dir / "h264.mov"), -1, media::DecoderOptions{.allowHardware = false});
        REQUIRE(d);
        auto f = (*d)->frameAt(Ticks::fromSeconds(1));
        REQUIRE(f);
        CHECK(barcode(*f, 1280) == 30);
    }
}

TEST_CASE("Proxy: the start timecode carries over", "[proxy]") {
    if (!haveMedia("prores_422.mov")) {
        SKIP("prores_422.mov was not generated");
    }
    const std::filesystem::path dir = freshOutputDir("proxy-timecode");
    REQUIRE(remuxWithTimecode(utf8(mediaDir() / "prores_422.mov"), dir / "tc.mov", "01:00:10:05"));
    const auto sourceProbe = ffprobe(dir / "tc.mov");
    REQUIRE(sourceProbe);
    REQUIRE(streamTag(*sourceProbe, "TAG:timecode") == "01:00:10:05");
    auto result = exporter::transcodeProxy(utf8(dir / "tc.mov"), dir / "proxy.mov", {});
    REQUIRE(result);
    CHECK(result->timecode);
    const auto probe = ffprobe(dir / "proxy.mov");
    REQUIRE(probe);
    CHECK(streamTag(*probe, "TAG:timecode") == "01:00:10:05");
    CHECK(expectFrameExact(utf8(dir / "tc.mov"), dir / "proxy.mov") == 48);
}

TEST_CASE("Proxy: cancelling or refusing leaves nothing behind", "[proxy]") {
    if (!haveMedia("h264_422_10bit.mp4") || !haveMedia("prores_4444_alpha.mov")) {
        SKIP("clips were not generated");
    }
    const std::filesystem::path dir = freshOutputDir("proxy-cancel");
    SECTION("cancelled through the yield hook after five frames") {
        int frames = 0;
        exporter::ProxyControl control;
        control.yield = [&] { return frames++ < 5; };
        auto result = exporter::transcodeProxy(utf8(mediaDir() / "h264_422_10bit.mp4"), dir / "p.mov", {}, control);
        REQUIRE(!result);
        CHECK(result.error().code == ErrorCode::Cancelled);
    }
    SECTION("cancelled through the stop token") {
        std::stop_source stop;
        exporter::ProxyControl control;
        control.stop = stop.get_token();
        control.onProgress = [&](std::int64_t done, std::int64_t) {
            if (done == 3) {
                stop.request_stop();
            }
        };
        auto result = exporter::transcodeProxy(utf8(mediaDir() / "h264_422_10bit.mp4"), dir / "p.mov", {}, control);
        REQUIRE(!result);
        CHECK(result.error().code == ErrorCode::Cancelled);
    }
    SECTION("media with alpha") {
        auto result = exporter::transcodeProxy(utf8(mediaDir() / "prores_4444_alpha.mov"), dir / "p.mov", {});
        REQUIRE(!result);
        CHECK(result.error().code == ErrorCode::Unsupported);
    }
    SECTION("a missing source") {
        auto result = exporter::transcodeProxy(utf8(dir / "missing.mp4"), dir / "p.mov", {});
        REQUIRE(!result);
    }
    CHECK(std::filesystem::is_empty(dir));
}

TEST_CASE("Trimmed copy: whole GOPs around the range, every stream, frame for frame after the measured shift",
          "[proxy][capsule]") {
    const std::string name = GENERATE(as<std::string>{}, "h264_1080p60_aac.mp4", "prores_422.mov");
    if (!haveMedia(name)) {
        SKIP(name + " was not generated");
    }
    INFO(name);
    const std::string source = utf8(mediaDir() / name);
    const std::filesystem::path dir = freshOutputDir("trim-" + name);
    const std::filesystem::path out = dir / ("trimmed" + std::filesystem::path(name).extension().string());
    double lastProgress = -1.0;
    auto copy = exporter::trimmedCopy(source, out, TimeRange::fromStartEnd(Ticks{kTicksPerSecond / 2}, Ticks{kTicksPerSecond * 9 / 10}),
                                      {}, [&](double p) { lastProgress = p; });
    REQUIRE(copy);
    CHECK(std::filesystem::exists(out));
    CHECK(!std::filesystem::exists(std::filesystem::path(out).concat(".partial")));
    CHECK(std::filesystem::file_size(out) < std::filesystem::file_size(mediaDir() / name));
    CHECK(copy->shift.value >= 0);
    CHECK(copy->shift <= Ticks{kTicksPerSecond / 2});
    CHECK(lastProgress >= 0.0);
    auto sourceInfo = media::probe(source);
    auto copyInfo = media::probe(utf8(out));
    REQUIRE(sourceInfo);
    REQUIRE(copyInfo);
    CHECK(copyInfo->video.size() == sourceInfo->video.size());
    CHECK(copyInfo->audio.size() == sourceInfo->audio.size());

    auto a = media::VideoDecoder::open(source, -1, media::DecoderOptions{.allowHardware = false});
    auto b = media::VideoDecoder::open(utf8(out), -1, media::DecoderOptions{.allowHardware = false});
    REQUIRE(a);
    REQUIRE(b);
    const int width = (*a)->streamInfo().width;
    int frames = 0;
    for (Ticks t{kTicksPerSecond / 2}; t < Ticks{kTicksPerSecond * 9 / 10};) {
        auto fa = (*a)->frameAt(t);
        REQUIRE(fa);
        auto fb = (*b)->frameAt(t - copy->shift);
        REQUIRE(fb);
        CHECK(fb->pts == fa->pts - copy->shift);
        CHECK(barcode(*fb, width) == barcode(*fa, width));
        t = fa->pts + std::max(fa->duration, Ticks{1});
        ++frames;
    }
    CHECK(frames > 10);
}

TEST_CASE("Trimmed copy: cancelled or refused, nothing is left behind", "[proxy][capsule]") {
    if (!haveMedia("h264_1080p60_aac.mp4") || !haveMedia("sine_48k.wav")) {
        SKIP("clips were not generated");
    }
    const std::filesystem::path dir = freshOutputDir("trim-cancel");
    std::stop_source stop;
    stop.request_stop();
    auto r = exporter::trimmedCopy(utf8(mediaDir() / "h264_1080p60_aac.mp4"), dir / "t.mp4",
                                   TimeRange::fromStartEnd(Ticks::fromSeconds(1), Ticks::fromSeconds(2)), stop.get_token());
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(exporter::canTrimCopy(mediaDir() / "sine_48k.wav"));
    auto wav = exporter::trimmedCopy(utf8(mediaDir() / "sine_48k.wav"), dir / "t.wav",
                                     TimeRange::fromStartEnd(Ticks::fromSeconds(1), Ticks::fromSeconds(2)));
    REQUIRE_FALSE(wav);
    CHECK(wav.error().code == ErrorCode::Unsupported);
    CHECK(std::filesystem::is_empty(dir));
}
