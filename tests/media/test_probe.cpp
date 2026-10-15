#include "TestMedia.h"

#include <wabcut/media/Probe.h>

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

/// The audio streams' titles (A.10), in stream order.
std::vector<std::string> titles(const MediaInfo& info) {
    std::vector<std::string> out;
    for (const AudioStreamInfo& a : info.audio) {
        out.push_back(a.title);
    }
    return out;
}

/// The generated game recordings (tools/gen_test_media.py): H.264 320x180 10 fps + AAC stereo 48 kHz streams.
void checkRecording(const MediaInfo& info, std::size_t audioStreams) {
    REQUIRE(info.video.size() == 1);
    CHECK(info.video.front().codec == "h264");
    CHECK(info.video.front().width == 320);
    REQUIRE(info.audio.size() == audioStreams);
    for (std::size_t k = 0; k < info.audio.size(); ++k) {
        CHECK(info.audio[k].index == static_cast<int>(k + 1)); // the video is stream 0
        CHECK(info.audio[k].codec == "aac");
        CHECK(info.audio[k].channels == 2);
        CHECK(info.audio[k].sampleRate == kRate48kHz);
    }
}

MediaInfo probeOk(std::string_view name) {
    if (!haveMedia(name)) {
        SKIP("test clip " << name << " was not generated");
    }
    auto info = probe(mediaPath(name));
    INFO(name << ": " << (info ? "" : info.error().message));
    REQUIRE(info.has_value());
    CHECK(info->path == mediaPath(name));
    CHECK(info->fileSize > 0);
    CHECK(info->modifiedTime > 1'600'000'000); // seconds since the epoch, i.e. after 2020
    CHECK(info->duration.value > 0);
    return *info;
}

constexpr ColorInfo kBt709{ColorMatrix::BT709, ColorRange::Limited, ColorPrimaries::BT709,
                           TransferFunction::BT709, ChromaLocation::Left};

} // namespace

TEST_CASE("probe: H.264 1080p60 + AAC", "[media][probe]") {
    const MediaInfo info = probeOk("h264_1080p60_aac.mp4");
    CHECK(info.container.find("mp4") != std::string::npos);
    CHECK(info.duration == Ticks::fromSeconds(10));
    CHECK_FALSE(info.isStillImage);
    REQUIRE(info.video.size() == 1);
    REQUIRE(info.audio.size() == 1);

    const VideoStreamInfo& v = info.video.front();
    CHECK(v.index == 0);
    CHECK(v.codec == "h264");
    CHECK(v.profile == "High");
    CHECK(v.width == 1920);
    CHECK(v.height == 1080);
    CHECK(v.frameRate == kRate60);
    CHECK_FALSE(v.variableFrameRate);
    CHECK(v.sampleAspect == Rate{1, 1});
    CHECK(v.pixelFormat == "yuv420p");
    CHECK(v.bitDepth == 8);
    CHECK(v.chromaSubsampling == "4:2:0");
    CHECK(v.color == kBt709);
    CHECK(v.rotationDegrees == 0);
    CHECK(v.duration == Ticks::fromSeconds(10));
    CHECK(v.frameCount == 600);

    const AudioStreamInfo& a = info.audio.front();
    CHECK(a.index == 1);
    CHECK(a.codec == "aac");
    CHECK(a.sampleRate == kRate48kHz);
    CHECK(a.channels == 2);
    CHECK(a.channelLayout == "stereo");
    CHECK(a.duration == Ticks::fromSeconds(10));
    CHECK(a.title.empty()); // FFmpeg's default handler name, "SoundHandler", says nothing: untitled
}

TEST_CASE("probe: HEVC Main 10 tagged BT.2020 PQ", "[media][probe]") {
    const MediaInfo info = probeOk("hevc_main10_420.mp4");
    REQUIRE(info.video.size() == 1);
    CHECK(info.audio.empty());
    const VideoStreamInfo& v = info.video.front();
    CHECK(v.codec == "hevc");
    CHECK(v.profile == "Main 10");
    CHECK(v.bitDepth == 10);
    CHECK(v.pixelFormat == "yuv420p10le");
    CHECK(v.chromaSubsampling == "4:2:0");
    CHECK(v.frameRate == kRate30);
    CHECK(v.color == ColorInfo{ColorMatrix::BT2020NonConstant, ColorRange::Limited, ColorPrimaries::BT2020,
                               TransferFunction::PQ, ChromaLocation::Left});
    CHECK(v.frameCount == 60);
}

TEST_CASE("probe: H.264 High 4:2:2 10-bit", "[media][probe]") {
    const MediaInfo info = probeOk("h264_422_10bit.mp4");
    REQUIRE(info.video.size() == 1);
    const VideoStreamInfo& v = info.video.front();
    CHECK(v.codec == "h264");
    CHECK(v.profile == "High 4:2:2");
    CHECK(v.bitDepth == 10);
    CHECK(v.chromaSubsampling == "4:2:2");
    CHECK(v.pixelFormat == "yuv422p10le");
    CHECK(v.frameRate == kRate30);
    CHECK(v.color == kBt709);
}

TEST_CASE("probe: ProRes 422 at 23.976 with PCM audio", "[media][probe]") {
    const MediaInfo info = probeOk("prores_422.mov");
    CHECK(info.container.find("mov") != std::string::npos);
    REQUIRE(info.video.size() == 1);
    REQUIRE(info.audio.size() == 1);
    const VideoStreamInfo& v = info.video.front();
    CHECK(v.codec == "prores");
    CHECK(v.frameRate == kRate23_976); // exact rational, not 23.976
    CHECK(v.frameRate.isExact());
    CHECK(v.bitDepth == 10);
    CHECK(v.chromaSubsampling == "4:2:2");
    CHECK(v.frameCount == 48);
    CHECK(v.duration == framesToTicks(48, kRate23_976));
    // ProRes does not signal chroma siting: probe reports the raw tags, decoders resolve it.
    CHECK(v.color == ColorInfo{ColorMatrix::BT709, ColorRange::Limited, ColorPrimaries::BT709,
                               TransferFunction::BT709, ChromaLocation::Unknown});

    const AudioStreamInfo& a = info.audio.front();
    CHECK(a.codec == "pcm_s24le");
    CHECK(a.sampleRate == kRate48kHz);
    CHECK(a.channels == 2);
    CHECK(a.duration == framesToTicks(48, kRate23_976));
}

TEST_CASE("probe: variable frame rate is detected", "[media][probe]") {
    const MediaInfo info = probeOk("vfr.mp4");
    REQUIRE(info.video.size() == 1);
    const VideoStreamInfo& v = info.video.front();
    CHECK(v.variableFrameRate);
    CHECK(v.frameRate.isValid());
    CHECK(v.frameCount == 60);
    // The constant-rate clips must not be flagged.
    CHECK_FALSE(probeOk("rotated_90.mp4").video.front().variableFrameRate);
}

TEST_CASE("probe: display matrix rotation", "[media][probe]") {
    const MediaInfo info = probeOk("rotated_90.mp4");
    REQUIRE(info.video.size() == 1);
    CHECK(info.video.front().rotationDegrees == 90);
    CHECK(info.video.front().width == 640); // stored size, not the rotated display size
    CHECK(info.video.front().height == 360);
}

TEST_CASE("probe: WAV files", "[media][probe]") {
    for (const char* name : {"sine_48k.wav", "ramp_48k.wav"}) {
        const MediaInfo info = probeOk(name);
        CHECK(info.container == "wav");
        CHECK(info.video.empty());
        REQUIRE(info.audio.size() == 1);
        CHECK(info.audio.front().codec == "pcm_s16le");
        CHECK(info.audio.front().sampleRate == kRate48kHz);
        CHECK(info.audio.front().channels == 2);
        CHECK(info.audio.front().title.empty());
        CHECK(info.duration == Ticks::fromSeconds(5));
    }
}

// A.10 multi-track ingest: the name a recording gives each audio stream.

TEST_CASE("probe: OBS MKV track names become audio stream titles", "[media][probe][multitrack]") {
    const MediaInfo info = probeOk("obs_multitrack_titled.mkv");
    CHECK(info.container.find("matroska") != std::string::npos);
    checkRecording(info, 4);
    CHECK(titles(info) == std::vector<std::string>{"Game", "Mic", "Discord", "Music"});
}

TEST_CASE("probe: OBS default track names Track1 to Track3 are kept as written", "[media][probe][multitrack]") {
    const MediaInfo info = probeOk("obs_multitrack_tracknames.mkv");
    checkRecording(info, 3);
    CHECK(titles(info) == std::vector<std::string>{"Track1", "Track2", "Track3"});
}

TEST_CASE("probe: OBS hybrid MP4 handler names become audio stream titles", "[media][probe][multitrack]") {
    const MediaInfo info = probeOk("obs_hybrid_titled.mp4");
    CHECK(info.container.find("mp4") != std::string::npos);
    checkRecording(info, 3);
    CHECK(titles(info) == std::vector<std::string>{"Desktop Audio", "Mic/Aux", "Discord"});
}

TEST_CASE("probe: ShadowPlay two-track MP4 with the default handler names is untitled", "[media][probe][multitrack]") {
    const MediaInfo info = probeOk("shadowplay_two_track.mp4");
    checkRecording(info, 2);
    CHECK(titles(info) == std::vector<std::string>{"", ""}); // "SoundHandler" is generic
}

TEST_CASE("probe: untitled three-track MKV", "[media][probe][multitrack]") {
    const MediaInfo info = probeOk("untitled_three_track.mkv");
    checkRecording(info, 3);
    CHECK(titles(info) == std::vector<std::string>{"", "", ""});
}

TEST_CASE("probe: errors", "[media][probe]") {
    auto missing = probe(mediaPath("does_not_exist.mp4"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);

    const auto junk = scratchDir("probe-errors") / "junk.mp4";
    {
        std::ofstream out(junk, std::ios::binary);
        out << "this is not a media file, just some text that no demuxer should accept";
    }
    const std::u8string junkPath = junk.generic_u8string();
    auto unsupported = probe(std::string(reinterpret_cast<const char*>(junkPath.data()), junkPath.size()));
    REQUIRE_FALSE(unsupported.has_value());
    CHECK(unsupported.error().code == ErrorCode::Unsupported);
}
