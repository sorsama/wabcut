// P.2: captions in an export as Premiere writes them: a sidecar in each caption file format (CaptionFiles.h) next to
// the output, timecode-based ones from the sequence's start timecode; and CEA-608 embedded in the H.264 stream as A/53
// caption SEI (Embed In Output File), decoded back from the file's frames.

#include "ExportTestUtil.h"

#include <wabcut/export/CaptionSidecar.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/interchange/CaptionFiles.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

ExportSettings presetSettings(std::string_view id) {
    const auto preset = findPreset(id);
    REQUIRE(preset.has_value());
    return preset->settings;
}

constexpr int kW = 320;
constexpr int kH = 180;

/// 2 s at 25 fps, start timecode 01:00:00:00, a caption track "Hello" 0.2-0.8 s and "World" 1.0-1.6 s.
TestProject captionedProject(Rate rate = kRate25) {
    TestProject project(kW, kH, rate, Ticks::fromSeconds(2));
    CaptionTrack track;
    track.id = project.state.ids.allocate<CaptionTrackId>();
    track.name = "English";
    track.language = "en";
    track.enabled = true;
    track.cues = {{ms(200), ms(600), "Hello"}, {ms(1000), ms(600), "World"}};
    project.editSequence([&](Sequence& s) {
        s.captionTracks = {track};
        s.startTimecode = Ticks::fromSeconds(3600);
    });
    return project;
}

ExportSettings h264(CaptionExport captions) {
    ExportSettings s = presetSettings("match-source");
    s.container = Container::Mp4;
    s.video.codec = VideoCodec::H264;
    s.video.encoder = "libx264";
    s.audio.enabled = false;
    s.captions.mode = captions;
    return s;
}

std::string readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The non-null CC1 field-1 byte pairs carried by the video frames' A/53 caption side data, in frame order, with the
/// frame each came in.
std::vector<std::pair<int, std::array<std::uint8_t, 2>>> embeddedPairs(const std::filesystem::path& file) {
    std::vector<std::pair<int, std::array<std::uint8_t, 2>>> out;
    AVFormatContext* fmt = nullptr;
    REQUIRE(avformat_open_input(&fmt, file.string().c_str(), nullptr, nullptr) == 0);
    REQUIRE(avformat_find_stream_info(fmt, nullptr) >= 0);
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(stream >= 0);
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar);
    REQUIRE(avcodec_open2(ctx, codec, nullptr) == 0);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int index = 0;
    auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC)) {
                for (std::size_t i = 0; i + 2 < sd->size; i += 3) {
                    const bool valid = (sd->data[i] & 0x04) != 0;
                    const int type = sd->data[i] & 0x03;
                    if (valid && type == 0 && !(sd->data[i + 1] == 0x80 && sd->data[i + 2] == 0x80)) {
                        out.push_back({index, {sd->data[i + 1], sd->data[i + 2]}});
                    }
                }
            }
            ++index;
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, packet) >= 0) {
        if (packet->stream_index == stream) {
            avcodec_send_packet(ctx, packet);
            drain();
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(ctx, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return out;
}

} // namespace

TEST_CASE("Export writes Premiere's caption sidecar formats next to the output (P.2)", "[export][captions][p2]") {
    const TestProject project = captionedProject();
    const auto dir = freshOutputDir("caption-sidecars");
    const std::pair<CaptionExport, const char*> modes[] = {
        {CaptionExport::Scc, ".scc"},  {CaptionExport::Mcc, ".mcc"},  {CaptionExport::EbuStl, ".stl"},
        {CaptionExport::Ttml, ".xml"}, {CaptionExport::Dfxp, ".dfxp"}, {CaptionExport::SmpteTt, ".xml"},
        {CaptionExport::Itt, ".itt"}};
    for (const auto& [mode, extension] : modes) {
        INFO(captionExportName(mode));
        const auto out = dir / (std::string(captionExportName(mode)) + ".mp4");
        const auto result = ExportJob(makeRequest(project, h264(mode), out)).run();
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
        std::filesystem::path expected = out;
        expected.replace_extension(extension);
        CHECK(result->captions == expected);
        // Read back as a timeline at the sequence's start timecode: the same two cues.
        interchange::CaptionReadOptions read;
        read.startTimecode = project.seq().startTimecode;
        const auto back = interchange::readCaptionFile(result->captions, read,
                                                       interchange::captionFormatFromName(captionExportName(mode)));
        INFO((back ? std::string() : back.error().message));
        REQUIRE(back.has_value());
        REQUIRE(back->cues.size() == 2);
        CHECK(back->cues[0].text == "Hello");
        CHECK(back->cues[1].text == "World");
        CHECK(std::llabs(back->cues[0].start.value - ms(200).value) <= ms(80).value);
    }
    // SCC timecodes count from the sequence's start timecode (01:00:00:00).
    CHECK(readAll(dir / "scc.scc").find("\n\n01:00:00") != std::string::npos);
    CHECK(partialFiles(dir).empty());
    CHECK(captionExportFromName("smpte-tt") == CaptionExport::SmpteTt);
    CHECK_FALSE(captionExportFromName("docx").has_value());
    CHECK(isCaptionSidecar(CaptionExport::Itt));
    CHECK_FALSE(isCaptionSidecar(CaptionExport::Embed));
}

TEST_CASE("Export embeds CEA-608 captions in H.264 as A/53 caption SEI (P.2)", "[export][captions][p2]") {
    const TestProject project = captionedProject(kRate30);
    const auto dir = freshOutputDir("caption-embed");
    const auto out = dir / "embedded.mp4";
    const auto result = ExportJob(makeRequest(project, h264(CaptionExport::Embed), out)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->captions.empty()); // no sidecar
    const auto pairs = embeddedPairs(out);
    // The frames carry exactly the 608 data an SCC of the same cues would, on the same frames.
    const Sequence& seq = project.seq();
    interchange::CaptionWriteOptions o;
    o.language = "en";
    const auto expected = interchange::cea608ForVideo(seq.captionTracks.front().cues, TimeRange{Ticks{}, Ticks::fromSeconds(2)},
                                                      kRate30, o);
    std::vector<std::pair<int, std::array<std::uint8_t, 2>>> want;
    for (const auto& [frame, list] : expected) {
        for (const auto& p : list) {
            if (!(p[0] == 0x80 && p[1] == 0x80)) {
                want.push_back({static_cast<int>(frame), p});
            }
        }
    }
    REQUIRE_FALSE(want.empty());
    CHECK(pairs == want);
    // The text is in there: "Hello" with odd parity (H = 0xC8, e = 0xE5).
    bool hello = false;
    for (const auto& [frame, p] : pairs) {
        hello = hello || (p[0] == 0xC8 && p[1] == 0xE5);
    }
    CHECK(hello);

    // Only H.264 / HEVC carry them, and only through encoders that write caption SEI.
    ExportSettings prores = presetSettings("prores-422-hq");
    prores.audio.enabled = false;
    prores.captions.mode = CaptionExport::Embed;
    const auto refused = ExportJob(makeRequest(project, prores, dir / "prores.mov")).run();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    ExportSettings mf = h264(CaptionExport::Embed);
    mf.video.encoder = "h264_amf";
    const auto noSei = ExportJob(makeRequest(project, mf, dir / "amf.mp4")).run();
    REQUIRE_FALSE(noSei.has_value());
    CHECK(noSei.error().message.find("closed captions") != std::string::npos);
    CHECK(partialFiles(dir).empty());
}
