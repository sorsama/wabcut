// Stream C, batch C4 (0.7), export module:
//   * #49  H.264 / HEVC passthrough: smart rendering copies whole closed GOPs of untouched source clips (the packets are
//          the source's bit for bit), encodes only what a cut, an effect or a retime touches, and says why;
//   * #176 the pre-export check: one list of what will go wrong, from the project and the export settings;
//   * #103 the newer codecs: what the linked FFmpeg decodes and encodes, and HEVC 4:2:2 through the export and back.

#include "ExportTestUtil.h"
#include "LongGop.h"
#include "SmartRender.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/Registry.h>
#include <wabcut/export/ExportCommandServices.h>
#include <wabcut/export/ExportEffects.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/export/PreExportCheck.h>
#include <wabcut/media/CodecSupport.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MotionOps.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

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

std::string utf8Of(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::expected<ExportResult, Error> runExport(const TestProject& project, ExportSettings settings, const fs::path& out) {
    ExportJob job(makeRequest(project, std::move(settings), out));
    return job.run();
}

/// A packet of a file's first video stream, in decode order.
struct Pkt {
    std::int64_t pts = 0;
    std::int64_t dts = 0;
    bool key = false;
    std::vector<std::uint8_t> data;
};

std::vector<Pkt> readPackets(const fs::path& file) {
    std::vector<Pkt> out;
    AVFormatContext* fmt = nullptr;
    const std::string path = utf8Of(file);
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return out;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVPacket* packet = av_packet_alloc();
    while (stream >= 0 && av_read_frame(fmt, packet) >= 0) {
        if (packet->stream_index == stream) {
            out.push_back({packet->pts, packet->dts, (packet->flags & AV_PKT_FLAG_KEY) != 0,
                           std::vector<std::uint8_t>(packet->data, packet->data + packet->size)});
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&fmt);
    return out;
}

/// The packets in display order (by pts).
std::vector<Pkt> displayOrder(std::vector<Pkt> packets) {
    std::sort(packets.begin(), packets.end(), [](const Pkt& a, const Pkt& b) { return a.pts < b.pts; });
    return packets;
}

/// The GOPs of a file as (first display index, frames), from its key packets.
std::vector<std::pair<int, int>> gopsOf(const std::vector<Pkt>& decodeOrder) {
    std::vector<std::int64_t> sorted;
    for (const Pkt& p : decodeOrder) {
        sorted.push_back(p.pts);
    }
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::pair<int, int>> out;
    std::int64_t minPts = 0;
    int frames = 0;
    for (const Pkt& p : decodeOrder) {
        if (p.key && frames > 0) {
            out.push_back({static_cast<int>(std::lower_bound(sorted.begin(), sorted.end(), minPts) - sorted.begin()), frames});
            frames = 0;
        }
        minPts = frames == 0 ? p.pts : std::min(minPts, p.pts);
        ++frames;
    }
    if (frames > 0) {
        out.push_back({static_cast<int>(std::lower_bound(sorted.begin(), sorted.end(), minPts) - sorted.begin()), frames});
    }
    return out;
}

/// Frames in whole GOPs inside [from, to) of the source: what a perfect planner copies.
int framesInWholeGops(const std::vector<std::pair<int, int>>& gops, int from, int to) {
    int n = 0;
    for (const auto& [first, frames] : gops) {
        if (first >= from && first + frames <= to) {
            n += frames;
        }
    }
    return n;
}

/// Software H.264 / HEVC settings with a GOP of 12 frames, sized to the test sequence, no sound.
ExportSettings gopSettings(VideoCodec codec = VideoCodec::H264, int keyframes = 12) {
    ExportSettings s = preset("youtube-1080p");
    s.container = Container::Mp4;
    s.video.codec = codec;
    s.video.encoder = codec == VideoCodec::Hevc ? "libx265" : "libx264";
    s.video.width.reset();
    s.video.height.reset();
    s.video.frameRate.reset();
    s.video.rateControl = RateControl::Quality;
    s.video.quality = 26;
    s.video.keyframeInterval = framesToTicks(keyframes, kRate30);
    s.audio.enabled = false;
    s.render.smartRender = true;
    s.render.usePreviews = false;
    return s;
}

constexpr int kWidth = 640; // the barcode of a frame number is 16 bits of 32 pixels
constexpr int kHeight = 360;

/// Exports `frames` of the synthetic pattern as the source file the smart-render cases cut.
fs::path makeSource(const fs::path& dir, const char* name, int frames, ExportSettings settings) {
    TestProject project(kWidth, kHeight, kRate30, framesToTicks(frames, kRate30));
    const fs::path file = dir / name;
    const auto r = runExport(project, std::move(settings), file);
    REQUIRE(r.has_value());
    REQUIRE(r->frames == frames);
    return file;
}

MediaId addFile(TestProject& project, const fs::path& file) {
    const std::string path = utf8Of(file);
    auto info = media::probe(path);
    REQUIRE(info.has_value());
    MediaItem item;
    item.id = project.state.ids.allocate<MediaId>();
    item.name = utf8Of(file.filename());
    item.locator.path = path;
    item.info = *info;
    const MediaId id = item.id;
    project.state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    return id;
}

/// The barcode of every decoded frame, in display order (-1: unreadable).
std::vector<int> barcodes(const fs::path& file) {
    std::vector<int> out;
    REQUIRE(decodeVideo(file, [&](const YuvFrame& f) { out.push_back(barcodeOf(f)); }));
    return out;
}

/// dts counts up, pts never trails dts, pts are unique: what a muxer and every player needs.
void checkTimestamps(const std::vector<Pkt>& decodeOrder) {
    std::set<std::int64_t> pts;
    for (std::size_t i = 0; i < decodeOrder.size(); ++i) {
        INFO("packet " << i);
        CHECK(decodeOrder[i].pts >= decodeOrder[i].dts);
        if (i > 0) {
            CHECK(decodeOrder[i].dts > decodeOrder[i - 1].dts);
        }
        CHECK(pts.insert(decodeOrder[i].pts).second);
    }
}

/// A copy of the H.264 file `source` whose avcC says another level (the record's and the SPS' level_idc): the packets are
/// untouched, so it plays as before, but its parameter sets are no longer the export's.
fs::path withLevel(const fs::path& source, const fs::path& out, int level) {
    AVFormatContext* in = nullptr;
    const std::string inPath = utf8Of(source);
    REQUIRE(avformat_open_input(&in, inPath.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(in, nullptr) >= 0);
    const int stream = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(stream >= 0);
    AVFormatContext* dst = nullptr;
    const std::string outPath = utf8Of(out);
    REQUIRE(avformat_alloc_output_context2(&dst, nullptr, nullptr, outPath.c_str()) >= 0);
    AVStream* s = avformat_new_stream(dst, nullptr);
    REQUIRE(s != nullptr);
    REQUIRE(avcodec_parameters_copy(s->codecpar, in->streams[stream]->codecpar) >= 0);
    s->codecpar->codec_tag = 0;
    s->time_base = in->streams[stream]->time_base;
    // avcC: version, profile, compatibility, level, length size, SPS count, SPS length (2), then the SPS: NAL header,
    // profile_idc, constraint flags, level_idc.
    REQUIRE(s->codecpar->extradata_size > 12);
    REQUIRE(s->codecpar->extradata[0] == 1);
    s->codecpar->extradata[3] = static_cast<std::uint8_t>(level);
    s->codecpar->extradata[11] = static_cast<std::uint8_t>(level);
    REQUIRE(avio_open(&dst->pb, outPath.c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(dst, nullptr) >= 0);
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(in, packet) >= 0) {
        if (packet->stream_index == stream) {
            av_packet_rescale_ts(packet, in->streams[stream]->time_base, s->time_base);
            packet->stream_index = s->index;
            REQUIRE(av_interleaved_write_frame(dst, packet) >= 0);
        } else {
            av_packet_unref(packet);
        }
    }
    av_packet_free(&packet);
    REQUIRE(av_write_trailer(dst) >= 0);
    avio_closep(&dst->pb);
    avformat_free_context(dst);
    avformat_close_input(&in);
    return out;
}

/// Output display frames from `outStart` that equal the source's frames from `srcStart`, as many as `count`.
int identicalFrames(const std::vector<Pkt>& out, const std::vector<Pkt>& source) {
    const std::vector<Pkt> o = displayOrder(out);
    std::vector<std::vector<std::uint8_t>> sourceData;
    for (const Pkt& p : displayOrder(source)) {
        sourceData.push_back(p.data);
    }
    int n = 0;
    for (const Pkt& p : o) {
        if (std::find(sourceData.begin(), sourceData.end(), p.data) != sourceData.end()) {
            ++n;
        }
    }
    return n;
}

} // namespace

// ---- #49: the bitstream helpers ------------------------------------------------------------------------------------------

namespace {

namespace gop = wabcut::exporter::detail;
using gop::NalUnit;

NalUnit nal(std::initializer_list<int> bytes) {
    NalUnit out;
    for (const int b : bytes) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

void append(std::vector<std::uint8_t>& to, const NalUnit& n) {
    to.insert(to.end(), n.begin(), n.end());
}

} // namespace

TEST_CASE("LongGop reads the parameter sets of avcC and Annex B headers and the NAL units of a packet (049)", "[export][smart][gop]") {
    const NalUnit sps = nal({0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50});
    const NalUnit pps = nal({0x68, 0xee, 0x3c, 0x80});
    // avcC: version, profile, compatibility, level, 0xFC | (length size - 1), 0xE0 | SPS count, SPS, PPS count, PPS.
    std::vector<std::uint8_t> avcc{1, 0x64, 0x00, 0x1f, 0xFF, 0xE1, 0x00, static_cast<std::uint8_t>(sps.size())};
    append(avcc, sps);
    avcc.push_back(1);
    avcc.push_back(0x00);
    avcc.push_back(static_cast<std::uint8_t>(pps.size()));
    append(avcc, pps);
    // The same headers as Annex B (an encoder's global header: start codes, with a SEI that is not a parameter set).
    std::vector<std::uint8_t> annexB{0, 0, 0, 1};
    append(annexB, sps);
    annexB.insert(annexB.end(), {0, 0, 0, 1});
    append(annexB, pps);
    annexB.insert(annexB.end(), {0, 0, 1, 0x06, 0x05, 0x01, 0x80});

    const auto fromAvcc = gop::parameterSets(AV_CODEC_ID_H264, avcc);
    const auto fromAnnexB = gop::parameterSets(AV_CODEC_ID_H264, annexB);
    REQUIRE(fromAvcc.size() == 2);
    CHECK(fromAvcc == fromAnnexB); // sorted sets: the file's avcC equals the encoder's global header
    CHECK(gop::nalLengthSize(AV_CODEC_ID_H264, avcc) == 4);
    CHECK(gop::nalLengthSize(AV_CODEC_ID_H264, annexB) == 0); // a raw stream is not copied
    CHECK(gop::parameterSets(AV_CODEC_ID_H264, std::vector<std::uint8_t>{1, 2}).empty());
    CHECK(gop::parameterSets(AV_CODEC_ID_PRORES, avcc).empty());

    // A packet of 4-byte-length-prefixed NAL units: SPS + IDR slice; a P slice alone.
    std::vector<std::uint8_t> idr{0, 0, 0, static_cast<std::uint8_t>(sps.size())};
    append(idr, sps);
    idr.insert(idr.end(), {0, 0, 0, 3, 0x65, 0x88, 0x84});
    const gop::PacketNals idrNals = gop::inspectPacket(AV_CODEC_ID_H264, 4, idr);
    CHECK(idrNals.idr);
    CHECK(idrNals.parameterSets);
    CHECK_FALSE(idrNals.bad);
    REQUIRE(idrNals.sets.size() == 1);
    CHECK(idrNals.sets.front() == sps);
    const std::vector<std::uint8_t> p{0, 0, 0, 3, 0x41, 0x9a, 0x24};
    const gop::PacketNals pNals = gop::inspectPacket(AV_CODEC_ID_H264, 4, p);
    CHECK_FALSE(pNals.idr);
    CHECK_FALSE(pNals.parameterSets);
    // A length that runs past the packet is damage.
    const std::vector<std::uint8_t> bad{0, 0, 0, 9, 0x65, 0x88};
    CHECK(gop::inspectPacket(AV_CODEC_ID_H264, 4, bad).bad);

    // A copied packet reaches the muxer as Annex B: every NAL unit behind a four-byte start code, otherwise untouched.
    const std::vector<std::uint8_t> annexPacket = gop::toAnnexB(4, idr);
    std::vector<std::uint8_t> wantedAnnexB{0, 0, 0, 1};
    append(wantedAnnexB, sps);
    wantedAnnexB.insert(wantedAnnexB.end(), {0, 0, 0, 1, 0x65, 0x88, 0x84});
    CHECK(annexPacket == wantedAnnexB);
    CHECK(gop::toAnnexB(4, bad).empty());                                       // a length past the packet is damage
    CHECK(gop::toAnnexB(0, idr).empty());                                       // no such prefix
    const std::vector<std::uint8_t> twoByte{0, 3, 0x41, 0x9a, 0x24};            // a 2-byte length prefix
    CHECK(gop::toAnnexB(2, twoByte) == std::vector<std::uint8_t>({0, 0, 0, 1, 0x41, 0x9a, 0x24}));

    // HEVC: IDR_W_RADL (19) and IDR_N_LP (20) are IDR pictures, CRA (21) is not; the NAL type sits in bits 1..6.
    const std::vector<std::uint8_t> radl{0, 0, 0, 3, 19 << 1, 0x01, 0xaf};
    const std::vector<std::uint8_t> cra{0, 0, 0, 3, 21 << 1, 0x01, 0xaf};
    CHECK(gop::inspectPacket(AV_CODEC_ID_HEVC, 4, radl).idr);
    CHECK_FALSE(gop::inspectPacket(AV_CODEC_ID_HEVC, 4, cra).idr);
}

TEST_CASE("LongGop says which header differs between a source and the export (049)", "[export][smart][gop]") {
    const auto sps = [](int profile, int level, int extra) { return nal({0x67, profile, 0x00, level, extra, 0x80}); };
    const std::vector<NalUnit> high40{sps(100, 40, 0xac)};
    const std::vector<NalUnit> main40{sps(77, 40, 0xac)};
    const std::vector<NalUnit> high51{sps(100, 51, 0xac)};
    const std::vector<NalUnit> high40b{sps(100, 40, 0xad)};
    CHECK(gop::describeHeaderDifference(AV_CODEC_ID_H264, main40, high40).find("profile") != std::string::npos);
    const std::string level = gop::describeHeaderDifference(AV_CODEC_ID_H264, high51, high40);
    CHECK(level.find("level") != std::string::npos);
    CHECK(level.find("5.1") != std::string::npos);
    CHECK(level.find("4.0") != std::string::npos);
    CHECK(gop::describeHeaderDifference(AV_CODEC_ID_H264, high40b, high40).find("encoder settings") != std::string::npos);
    CHECK(gop::describeHeaderDifference(AV_CODEC_ID_H264, {}, high40).find("encoder settings") != std::string::npos);
}

// ---- #49: smart rendering of H.264 and HEVC ---------------------------------------------------------------------------

TEST_CASE("H.264 and HEVC passthrough is offered for MP4 and MOV exports, not for MKV (049)", "[export][smart][gop]") {
    // Matroska counts time in milliseconds: the frame times of 24, 30 and 60 fps are not whole ticks of it, and a copied
    // GOP keeps its source's stamps exactly, so an MKV export is never offered the copy (it is encoded as usual).
    for (const VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        ExportSettings s = gopSettings(codec);
        for (const Container container : {Container::Mp4, Container::Mov}) {
            s.container = container;
            CHECK(exporter::detail::smartRenderLongGop(s));
            CHECK(exporter::detail::smartRenderEligible(s));
        }
        s.container = Container::Mkv;
        CHECK_FALSE(exporter::detail::smartRenderLongGop(s));
        CHECK_FALSE(exporter::detail::smartRenderEligible(s));
    }
}

TEST_CASE("H.264 smart rendering copies every GOP of an untouched source bit for bit and the file plays (049)",
          "[export][smart][gop][media]") {
    const fs::path dir = freshOutputDir("v07_c4_gop_whole");
    const fs::path source = makeSource(dir, "source.mp4", 72, gopSettings());
    const std::vector<Pkt> sourcePackets = readPackets(source);
    REQUIRE(sourcePackets.size() == 72);
    const auto gops = gopsOf(sourcePackets);
    REQUIRE(gops.size() >= 3); // keyframes every 12 frames (scene cuts may add more)

    TestProject project(kWidth, kHeight, kRate30, framesToTicks(72, kRate30));
    const MediaId media = addFile(project, source);
    project.addVideoTrack(media, Ticks{}, framesToTicks(72, kRate30), Ticks{});
    const auto r = runExport(project, gopSettings(), dir / "copy.mp4");
    REQUIRE(r.has_value());
    CHECK(r->frames == 72);
    CHECK(r->copiedFrames == 72);
    CHECK(r->smartRenderCodec == "H.264");
    CHECK(r->smartRenderNotes.empty());

    const std::vector<Pkt> out = readPackets(dir / "copy.mp4");
    REQUIRE(out.size() == 72);
    checkTimestamps(out);
    CHECK(identicalFrames(out, sourcePackets) == 72); // never re-encoded
    // The file decodes across every join, frame after frame.
    const std::vector<int> codes = barcodes(dir / "copy.mp4");
    REQUIRE(codes.size() == 72);
    for (int i = 0; i < 72; ++i) {
        INFO("frame " << i);
        CHECK(codes[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("H.264 smart rendering encodes again only the GOPs a cut or a changed clip touches (049)",
          "[export][smart][gop][media]") {
    const fs::path dir = freshOutputDir("v07_c4_gop_cuts");
    const fs::path source = makeSource(dir, "source.mp4", 96, gopSettings());
    const std::vector<Pkt> sourcePackets = readPackets(source);
    REQUIRE(sourcePackets.size() == 96);
    const auto gops = gopsOf(sourcePackets);

    // (1) A cut inside a GOP at both ends: the whole GOPs in between are copied, the pictures up to the next IDR are not.
    {
        TestProject project(kWidth, kHeight, kRate30, framesToTicks(48, kRate30));
        const MediaId media = addFile(project, source);
        project.addVideoTrack(media, Ticks{}, framesToTicks(48, kRate30), framesToTicks(20, kRate30)); // source 20..67
        ExportSettings s = gopSettings();
        s.audio.enabled = true; // sound goes beside the copied and encoded pictures
        const auto r = runExport(project, s, dir / "cut.mp4");
        REQUIRE(r.has_value());
        const int expected = framesInWholeGops(gops, 20, 68);
        INFO("whole GOPs inside source 20..67: " << expected);
        CHECK(r->copiedFrames == expected);
        CHECK(expected > 0);
        CHECK(expected < 48);
        REQUIRE_FALSE(r->smartRenderNotes.empty());
        std::int64_t noted = 0;
        for (const SmartRenderNote& note : r->smartRenderNotes) {
            CHECK_FALSE(note.reason.empty());
            CHECK_FALSE(note.firstTimecode.empty());
            noted += note.frames;
        }
        CHECK(noted == 48 - expected); // every frame that was encoded again has a reason
        const std::vector<Pkt> out = readPackets(dir / "cut.mp4");
        REQUIRE(out.size() == 48);
        checkTimestamps(out);
        CHECK(identicalFrames(out, sourcePackets) == expected);
        const std::vector<int> codes = barcodes(dir / "cut.mp4");
        REQUIRE(codes.size() == 48);
        // Copied pictures show the source's frames 20.., the encoded ones the pattern of the export's frame number.
        int copiedShown = 0;
        for (int k = 0; k < 48; ++k) {
            if (codes[static_cast<std::size_t>(k)] == k + 20) {
                ++copiedShown;
            } else {
                CHECK(codes[static_cast<std::size_t>(k)] == k);
            }
        }
        CHECK(copiedShown >= expected);
        const auto probe = ffprobe(dir / "cut.mp4");
        REQUIRE(probe.has_value());
        REQUIRE(probe->stream("audio") != nullptr);
        REQUIRE(probe->stream("video") != nullptr);
        // Sound and picture start and end together (copied GOPs and encoded stretches alike).
        const auto seconds = [](const std::map<std::string, std::string>* section, const char* key) {
            const auto it = section->find(key);
            return it == section->end() ? -1000.0 : std::atof(it->second.c_str());
        };
        INFO("video start " << seconds(probe->stream("video"), "start_time") << " audio start "
                            << seconds(probe->stream("audio"), "start_time"));
        CHECK(std::abs(seconds(probe->stream("video"), "start_time") - seconds(probe->stream("audio"), "start_time")) < 0.15);
        CHECK(std::abs(seconds(probe->stream("video"), "duration") - seconds(probe->stream("audio"), "duration")) < 0.15);
        CHECK(std::abs(seconds(probe->stream("video"), "duration") - 1.6) < 0.1); // 48 frames at 30 fps
    }

    // (2) A clip in the middle that is not untouched (opacity): only its frames, and the pictures of the GOPs it cuts, render.
    {
        TestProject project(kWidth, kHeight, kRate30, framesToTicks(96, kRate30));
        const MediaId media = addFile(project, source);
        std::vector<Clip> clips;
        clips.push_back(project.clip(media, StreamKind::Video, Ticks{}, framesToTicks(36, kRate30), Ticks{}));
        Clip middle = project.clip(media, StreamKind::Video, framesToTicks(36, kRate30), framesToTicks(24, kRate30), framesToTicks(36, kRate30));
        ClipProperties props;
        props.set(ParamId::Opacity, 0.5);
        middle.setProperties(props);
        clips.push_back(std::move(middle));
        clips.push_back(project.clip(media, StreamKind::Video, framesToTicks(60, kRate30), framesToTicks(36, kRate30), framesToTicks(60, kRate30)));
        project.addTrack(TrackKind::Video, std::move(clips));
        const auto r = runExport(project, gopSettings(), dir / "middle.mp4");
        REQUIRE(r.has_value());
        const int expected = framesInWholeGops(gops, 0, 36) + framesInWholeGops(gops, 60, 96);
        CHECK(r->copiedFrames == expected);
        CHECK(expected > 0);
        CHECK(expected < 96 - 24);
        bool named = false;
        for (const SmartRenderNote& note : r->smartRenderNotes) {
            named = named || note.reason.find("opacity") != std::string::npos;
        }
        CHECK(named);
        const std::vector<Pkt> out = readPackets(dir / "middle.mp4");
        REQUIRE(out.size() == 96);
        checkTimestamps(out);
        CHECK(identicalFrames(out, sourcePackets) == expected);
        const std::vector<int> codes = barcodes(dir / "middle.mp4");
        REQUIRE(codes.size() == 96);
        for (std::size_t k = 0; k < codes.size(); ++k) {
            CHECK(codes[k] >= 0);
        }
    }
}

TEST_CASE("HEVC smart rendering copies an untouched source and the file plays (049)", "[export][smart][gop][media]") {
    const fs::path dir = freshOutputDir("v07_c4_hevc");
    const fs::path source = makeSource(dir, "source.mp4", 48, gopSettings(VideoCodec::Hevc));
    const std::vector<Pkt> sourcePackets = readPackets(source);
    REQUIRE(sourcePackets.size() == 48);
    TestProject project(kWidth, kHeight, kRate30, framesToTicks(48, kRate30));
    const MediaId media = addFile(project, source);
    project.addVideoTrack(media, Ticks{}, framesToTicks(48, kRate30), Ticks{});
    const auto r = runExport(project, gopSettings(VideoCodec::Hevc), dir / "copy.mp4");
    REQUIRE(r.has_value());
    CHECK(r->smartRenderCodec == "HEVC");
    INFO("notes: " << (r->smartRenderNotes.empty() ? "none" : r->smartRenderNotes.front().reason));
    CHECK(r->copiedFrames == 48);
    const std::vector<Pkt> out = readPackets(dir / "copy.mp4");
    REQUIRE(out.size() == 48);
    checkTimestamps(out);
    CHECK(identicalFrames(out, sourcePackets) == 48);
    const std::vector<int> codes = barcodes(dir / "copy.mp4");
    REQUIRE(codes.size() == 48);
    for (int i = 0; i < 48; ++i) {
        CHECK(codes[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("H.264 smart rendering copies nothing when the picture, the headers or the settings differ and says why (049)",
          "[export][smart][gop][media]") {
    const fs::path dir = freshOutputDir("v07_c4_gop_refuse");
    const fs::path source = makeSource(dir, "source.mp4", 48, gopSettings());
    const auto build = [&](const fs::path& file, Speed speed = Speed{1, 1}, std::int64_t frames = 48) {
        TestProject project(kWidth, kHeight, kRate30, framesToTicks(frames, kRate30));
        const MediaId media = addFile(project, file);
        Clip clip = project.clip(media, StreamKind::Video, Ticks{}, framesToTicks(frames, kRate30), Ticks{});
        clip.speed = speed;
        project.addTrack(TrackKind::Video, {clip});
        return project;
    };
    const auto reasons = [](const ExportResult& r) {
        std::string all;
        for (const SmartRenderNote& note : r.smartRenderNotes) {
            all += note.reason + "\n";
        }
        return all;
    };

    // A source whose headers are not the export's (here: another level in its avcC and SPS, the packets untouched): every
    // picture is encoded again, and the reason names what differs.
    {
        const fs::path other = withLevel(source, dir / "other.mp4", 51);
        const auto r = runExport(build(other), gopSettings(), dir / "other_out.mp4");
        REQUIRE(r.has_value());
        CHECK(r->copiedFrames == 0);
        CHECK(r->frames == 48);
        CHECK(r->smartRenderCodec == "H.264");
        CHECK(reasons(*r).find("other settings") != std::string::npos);
        CHECK(reasons(*r).find("level") != std::string::npos);
        const std::vector<int> codes = barcodes(dir / "other_out.mp4");
        REQUIRE(codes.size() == 48);
        for (int k = 0; k < 48; ++k) {
            CHECK(codes[static_cast<std::size_t>(k)] == k); // all of it is the export's own pictures
        }
    }
    // A source with a longer GOP but the export's encoder settings still carries the export's headers: copied, as one GOP.
    {
        const fs::path longer = makeSource(dir, "longer.mp4", 48, gopSettings(VideoCodec::H264, 90));
        const auto r = runExport(build(longer), gopSettings(), dir / "longer_out.mp4");
        REQUIRE(r.has_value());
        INFO("notes: " << (r->smartRenderNotes.empty() ? "none" : r->smartRenderNotes.front().reason));
        CHECK(r->copiedFrames == 48);
    }
    // A retimed clip: its pictures are not one after the other.
    {
        const auto r = runExport(build(source, Speed{2, 1}, 24), gopSettings(), dir / "fast.mp4");
        REQUIRE(r.has_value());
        CHECK(r->copiedFrames == 0);
        CHECK(reasons(*r).find("Retimed") != std::string::npos);
    }
    // Burned-in captions put pixels over every picture: not smart rendered at all.
    {
        ExportSettings s = gopSettings();
        s.captions.mode = CaptionExport::BurnIn;
        const auto r = runExport(build(source), s, dir / "burn.mp4");
        REQUIRE(r.has_value());
        CHECK(r->copiedFrames == 0);
        CHECK(r->smartRenderCodec.empty());
    }
    // Smart Render off: everything encodes.
    {
        ExportSettings s = gopSettings();
        s.render.smartRender = false;
        const auto r = runExport(build(source), s, dir / "off.mp4");
        REQUIRE(r.has_value());
        CHECK(r->copiedFrames == 0);
        CHECK(r->smartRenderCodec.empty());
    }
    // A size limit is met by encoding the programme at the rate that fits: no copying.
    {
        ExportSettings s = gopSettings();
        s.video.rateControl = RateControl::TargetSize;
        s.video.targetBytes = 50'000'000;
        const auto r = runExport(build(source), s, dir / "limit.mp4");
        REQUIRE(r.has_value());
        CHECK(r->copiedFrames == 0);
        CHECK(r->smartRenderCodec.empty());
    }
    // The same source with the same settings still copies (nothing above spoiled the source file).
    const auto again = runExport(build(source), gopSettings(), dir / "again.mp4");
    REQUIRE(again.has_value());
    CHECK(again->copiedFrames == 48);
}

TEST_CASE("H.264 smart rendering joins clips of two sources with black between them (049)", "[export][smart][gop][media]") {
    const fs::path dir = freshOutputDir("v07_c4_gop_mixed");
    // Two files made the same way (so both carry the export's headers), 36 frames each.
    const fs::path first = makeSource(dir, "first.mp4", 36, gopSettings());
    const fs::path second = makeSource(dir, "second.mp4", 36, gopSettings());
    TestProject project(kWidth, kHeight, kRate30, framesToTicks(84, kRate30));
    const MediaId a = addFile(project, first);
    const MediaId b = addFile(project, second);
    std::vector<Clip> clips;
    clips.push_back(project.clip(a, StreamKind::Video, Ticks{}, framesToTicks(36, kRate30), Ticks{}));
    clips.push_back(project.clip(b, StreamKind::Video, framesToTicks(48, kRate30), framesToTicks(36, kRate30), Ticks{}));
    project.addTrack(TrackKind::Video, std::move(clips)); // 12 frames of nothing between them
    const auto r = runExport(project, gopSettings(), dir / "mixed.mp4");
    REQUIRE(r.has_value());
    CHECK(r->frames == 84);
    CHECK(r->copiedFrames == 72); // both clips whole, the black encoded again
    bool black = false;
    for (const SmartRenderNote& note : r->smartRenderNotes) {
        if (note.reason.find("black") != std::string::npos) {
            black = true;
            CHECK(note.frames == 12);
            CHECK_FALSE(note.firstTimecode.empty());
        }
    }
    CHECK(black);
    const std::vector<Pkt> out = readPackets(dir / "mixed.mp4");
    REQUIRE(out.size() == 84);
    checkTimestamps(out);
    const std::vector<Pkt> sources = [&] {
        std::vector<Pkt> all = readPackets(first);
        const std::vector<Pkt> more = readPackets(second);
        all.insert(all.end(), more.begin(), more.end());
        return all;
    }();
    CHECK(identicalFrames(out, sources) == 72);
    const std::vector<int> codes = barcodes(dir / "mixed.mp4");
    REQUIRE(codes.size() == 84);
    for (int k = 0; k < 36; ++k) {
        CHECK(codes[static_cast<std::size_t>(k)] == k);
        CHECK(codes[static_cast<std::size_t>(48 + k)] == k);
    }
}

TEST_CASE("H.264 smart rendering copies 10-bit 4:2:2 and refuses a source of another sampling (049)", "[export][smart][gop][media]") {
    const fs::path dir = freshOutputDir("v07_c4_gop_422_10");
    ExportSettings wide = gopSettings();
    wide.video.pixelFormat = OutputPixelFormat::Yuv422p10;
    const fs::path source = makeSource(dir, "wide.mp4", 36, wide);
    const std::vector<Pkt> sourcePackets = readPackets(source);
    REQUIRE(sourcePackets.size() == 36);
    TestProject project(kWidth, kHeight, kRate30, framesToTicks(36, kRate30));
    const MediaId media = addFile(project, source);
    project.addVideoTrack(media, Ticks{}, framesToTicks(36, kRate30), Ticks{});
    const auto r = runExport(project, wide, dir / "wide_copy.mp4");
    REQUIRE(r.has_value());
    CHECK(r->copiedFrames == 36);
    CHECK(identicalFrames(readPackets(dir / "wide_copy.mp4"), sourcePackets) == 36);
    // The same 10-bit 4:2:2 source into an 8-bit 4:2:0 export: another sampling, so every picture is encoded again.
    const auto narrow = runExport(project, gopSettings(), dir / "narrow.mp4");
    REQUIRE(narrow.has_value());
    CHECK(narrow->copiedFrames == 0);
    REQUIRE_FALSE(narrow->smartRenderNotes.empty());
    CHECK(narrow->smartRenderNotes.front().reason.find("bit depth or chroma") != std::string::npos);
    CHECK(barcodes(dir / "narrow.mp4").size() == 36);
}

TEST_CASE("Smart rendering still copies ProRes frames (049 regression of the intra path)", "[export][smart][media]") {
    if (!haveMedia("prores_422.mov")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const Rate rate{24000, 1001};
    TestProject project(1280, 720, rate, framesToTicks(24, rate));
    const MediaId media = project.addMedia("prores_422.mov");
    project.addVideoTrack(media, framesToTicks(12, rate), framesToTicks(12, rate), Ticks{});
    const fs::path dir = freshOutputDir("v07_c4_prores_intra");
    ExportSettings s = preset("prores-422-hq");
    s.video.proresProfile = ProResProfile::Standard;
    s.audio.enabled = false;
    const auto r = runExport(project, s, dir / "smart.mov");
    REQUIRE(r.has_value());
    CHECK(r->copiedFrames == 12);
    CHECK(r->smartRenderCodec.empty()); // the report is the long-GOP kind's
}

// ---- #103: the newer codecs ------------------------------------------------------------------------------------------------

TEST_CASE("The codec support table is asked of the FFmpeg Wabcut links (103)", "[export][codecs]") {
    const std::vector<media::CodecSupport> table = media::newerCodecSupport();
    REQUIRE(table.size() == 4);
    const auto entry = [&](const std::string& id) -> const media::CodecSupport& {
        for (const media::CodecSupport& e : table) {
            if (e.id == id) {
                return e;
            }
        }
        FAIL("no entry " << id);
        return table.front();
    };
    // APV: FFmpeg's native decoder plays it; no encoder is in this build.
    CHECK(entry("apv").decodes());
    CHECK(entry("apv").decoder == "apv");
    CHECK_FALSE(entry("apv").exports());
    CHECK(avcodec_find_decoder_by_name("apv") != nullptr);
    CHECK(avcodec_find_encoder_by_name("apv") == nullptr);
    // HEVC 4:2:2 plays and exports (8-bit through libx265).
    CHECK(entry("hevc-422").decodes());
    CHECK(entry("hevc-422").exports());
    // AV1 4:2:2 plays (dav1d); the AV1 encoders are 4:2:0.
    CHECK(entry("av1-422").decodes());
    CHECK_FALSE(entry("av1-422").exports());
    // MV-HEVC: the HEVC decoder has the view selection option; Wabcut shows the base view.
    CHECK(entry("mv-hevc").decodes());
    CHECK(entry("mv-hevc").baseViewOnly);
    CHECK_FALSE(entry("mv-hevc").exports());

    // What a probed stream maps to.
    CHECK(media::codecSupportFor("apv", "4:2:2")->id == "apv");
    CHECK(media::codecSupportFor("hevc", "4:2:2")->id == "hevc-422");
    CHECK(media::codecSupportFor("av1", "4:2:2")->id == "av1-422");
    CHECK_FALSE(media::codecSupportFor("hevc", "4:2:0").has_value());
    CHECK_FALSE(media::codecSupportFor("h264", "4:2:2").has_value());
}

TEST_CASE("HEVC 4:2:2 8-bit exports, is read back as 4:2:2 and plays (103)", "[export][codecs][media]") {
    const fs::path dir = freshOutputDir("v07_c4_hevc_422");
    ExportSettings s = gopSettings(VideoCodec::Hevc);
    s.video.pixelFormat = OutputPixelFormat::Yuv422p;
    TestProject project(kWidth, kHeight, kRate30, framesToTicks(12, kRate30));
    const auto r = runExport(project, s, dir / "hevc422.mp4");
    REQUIRE(r.has_value());
    const auto info = media::probe(utf8Of(dir / "hevc422.mp4"));
    REQUIRE(info.has_value());
    REQUIRE(info->hasVideo());
    CHECK(info->video.front().codec == "hevc");
    CHECK(info->video.front().chromaSubsampling == "4:2:2");
    const auto support = media::codecSupportFor(info->video.front().codec, info->video.front().chromaSubsampling);
    REQUIRE(support.has_value());
    CHECK(support->decodes());
    CHECK(support->exports());
    const std::vector<int> codes = barcodes(dir / "hevc422.mp4");
    REQUIRE(codes.size() == 12);
    for (int i = 0; i < 12; ++i) {
        CHECK(codes[static_cast<std::size_t>(i)] == i);
    }
    // Wabcut's own decoder (software, then hardware where the machine's decoder takes 4:2:2) hands the frames out as 4:2:2.
    {
        media::DecoderOptions options;
        options.allowHardware = false;
        auto decoder = media::VideoDecoder::open(utf8Of(dir / "hevc422.mp4"), -1, options);
        REQUIRE(decoder.has_value());
        const auto frame = (*decoder)->frameAt(framesToTicks(5, kRate30));
        REQUIRE(frame.has_value());
        CHECK(frame->format == media::PixelFormat::YUV422P);
        CHECK(frame->width == kWidth);
        CHECK(frame->height == kHeight);
    }
    // 10-bit 4:2:2 HEVC and 4:2:2 AV1 are not exported: validate says so before anything runs.
    s.video.pixelFormat = OutputPixelFormat::Yuv422p10;
    CHECK_FALSE(validate(s).has_value());
    ExportSettings av1 = gopSettings();
    av1.video.codec = VideoCodec::Av1;
    av1.video.encoder = "auto";
    av1.video.pixelFormat = OutputPixelFormat::Yuv422p;
    CHECK_FALSE(validate(av1).has_value());
}

// ---- #176: the pre-export check ----------------------------------------------------------------------------------------------

namespace {

const CheckIssue* findIssue(const std::vector<CheckIssue>& rows, CheckKind kind, std::size_t nth = 0) {
    std::size_t seen = 0;
    for (const CheckIssue& row : rows) {
        if (row.kind == kind && seen++ == nth) {
            return &row;
        }
    }
    return nullptr;
}

std::size_t countOf(const std::vector<CheckIssue>& rows, CheckKind kind) {
    return static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [&](const CheckIssue& r) { return r.kind == kind; }));
}

ExportSettings checkSettings() {
    ExportSettings s = preset("youtube-1080p");
    s.video.width.reset();
    s.video.height.reset();
    return s;
}

} // namespace

TEST_CASE("Pre-export check: offline media is a problem with a Relink fix, at its first clip (176)", "[export][check]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1920, 1080, kRate30, Ticks::fromSeconds(4));
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4");
    project.addVideoTrack(media, ms(500), Ticks::fromSeconds(2), Ticks{});
    project.editMedia(media, [](MediaItem& item) { item.locator.offline = true; });
    const auto rows = preExportCheck(project.state, project.seq(), checkSettings());
    const CheckIssue* offline = findIssue(rows, CheckKind::OfflineMedia);
    REQUIRE(offline);
    CHECK(offline->severity == CheckSeverity::Problem);
    CHECK(offline->fix == CheckFix::Relink);
    CHECK(offline->subject == "h264_1080p60_aac.mp4");
    REQUIRE(offline->at.has_value());
    CHECK(*offline->at == ms(500));
    CHECK(rows.front().severity == CheckSeverity::Problem); // most serious first
    CHECK(checkKindName(CheckKind::OfflineMedia) == "offlineMedia");
    CHECK(checkFixName(CheckFix::Relink) == "relink");
    CHECK(checkSeverityName(CheckSeverity::Problem) == "problem");
}

TEST_CASE("Pre-export check: gaps in the picture and flash frames, each at its place (176)", "[export][check]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1920, 1080, kRate30, Ticks::fromSeconds(5));
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4");
    const Rate rate = kRate30;
    std::vector<Clip> clips;
    clips.push_back(project.clip(media, StreamKind::Video, ms(500), ms(1000), Ticks{}));                    // 0.5 - 1.5
    clips.push_back(project.clip(media, StreamKind::Video, ms(2000), ms(1000), Ticks{}));                   // 2.0 - 3.0
    clips.push_back(project.clip(media, StreamKind::Video, ms(3000), framesToTicks(2, rate), Ticks{}));     // 3.0 - 3.07: a flash frame
    clips.push_back(project.clip(media, StreamKind::Video, ms(3500), ms(1000), Ticks{}));                   // 3.5 - 4.5
    project.addTrack(TrackKind::Video, std::move(clips));
    const auto rows = preExportCheck(project.state, project.seq(), checkSettings());
    // Gaps: before the first clip, 1.5 - 2.0, and 3.07 - 3.5.
    REQUIRE(countOf(rows, CheckKind::Gap) == 3);
    const CheckIssue* leading = findIssue(rows, CheckKind::Gap, 0);
    REQUIRE(leading);
    CHECK(*leading->at == Ticks{});
    CHECK(leading->length == ms(500));
    CHECK(leading->fix == CheckFix::CloseGap);
    CHECK(leading->clip.isValid()); // the clip after it closes it
    const CheckIssue* middle = findIssue(rows, CheckKind::Gap, 1);
    REQUIRE(middle);
    CHECK(*middle->at == ms(1500));
    CHECK(middle->length == ms(500));
    CHECK(middle->severity == CheckSeverity::Warning);
    // The flash frame: two frames long, named by its media.
    const CheckIssue* flash = findIssue(rows, CheckKind::FlashFrame);
    REQUIRE(flash);
    CHECK(*flash->at == ms(3000));
    CHECK(flash->length == framesToTicks(2, rate));
    CHECK(flash->subject == "h264_1080p60_aac.mp4");
    // A range inside the covered time has no gap rows.
    ExportSettings inside = checkSettings();
    inside.range = RangeMode::Custom;
    inside.customRange = TimeRange{ms(2000), ms(900)};
    CHECK(countOf(preExportCheck(project.state, project.seq(), inside), CheckKind::Gap) == 0);
    // An audio-only export has no picture to leave empty.
    ExportSettings audioOnly = preset("audio-wav");
    CHECK(countOf(preExportCheck(project.state, project.seq(), audioOnly), CheckKind::Gap) == 0);
}

TEST_CASE("Pre-export check: many rows of one kind are listed and counted (176)", "[export][check]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1920, 1080, kRate30, Ticks::fromSeconds(60));
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4");
    std::vector<Clip> clips;
    for (int i = 0; i < 20; ++i) { // 20 clips of 0.5 s with 0.5 s of nothing between them
        clips.push_back(project.clip(media, StreamKind::Video, Ticks::fromSeconds(i * 2) + ms(500), ms(500), Ticks{}));
    }
    project.addTrack(TrackKind::Video, std::move(clips));
    const auto rows = preExportCheck(project.state, project.seq(), checkSettings());
    const std::size_t gapRows = countOf(rows, CheckKind::Gap);
    CHECK(gapRows == kMaxCheckRowsPerKind + 1);
    const auto more = std::find_if(rows.begin(), rows.end(), [](const CheckIssue& r) { return r.kind == CheckKind::Gap && r.more; });
    REQUIRE(more != rows.end());
    CHECK(more->count == 20 - static_cast<std::int64_t>(kMaxCheckRowsPerKind) + 0); // 20 gaps (leading + 19), 12 listed
    CHECK_FALSE(more->at.has_value());
    CHECK(more->fix == CheckFix::None);
}

TEST_CASE("Pre-export check: frame rates, stabilizers, slow effects and the export's shape (176)", "[export][check]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1920, 1080, kRate30, Ticks::fromSeconds(4));
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4"); // 60 fps in a 30 fps sequence
    Clip a = project.clip(media, StreamKind::Video, Ticks{}, ms(1000), Ticks{});
    Clip b = project.clip(media, StreamKind::Video, ms(1000), ms(1000), Ticks{});
    {
        ClipContent content = b.content();
        EffectInstance warp;
        warp.id = EffectInstanceId{1};
        warp.effect = std::string(kWarpStabilizerEffect);
        content.effects.push_back(warp);
        EffectInstance median;
        median.id = EffectInstanceId{2};
        median.effect = "wabcut.noise.median";
        content.effects.push_back(median);
        b.setContent(std::move(content));
    }
    Clip c = project.clip(media, StreamKind::Video, ms(2000), ms(1000), Ticks{});
    {
        ClipContent content = c.content();
        EffectInstance warp;
        warp.id = EffectInstanceId{3};
        warp.effect = std::string(kWarpStabilizerEffect);
        warp.setData(EffectData{"status", std::string("analyzed")}); // analysed: no row
        content.effects.push_back(warp);
        c.setContent(std::move(content));
    }
    project.addTrack(TrackKind::Video, {a, b, c});

    ExportSettings s = checkSettings();
    auto rows = preExportCheck(project.state, project.seq(), s);
    const CheckIssue* rate = findIssue(rows, CheckKind::MixedFrameRate);
    REQUIRE(rate);
    CHECK(rate->severity == CheckSeverity::Note);
    CHECK(rate->subject == "60 fps");
    CHECK(rate->count == 3);
    CHECK(*rate->at == Ticks{});
    REQUIRE(countOf(rows, CheckKind::UnanalysedStabilizer) == 1);
    const CheckIssue* stab = findIssue(rows, CheckKind::UnanalysedStabilizer);
    CHECK(*stab->at == ms(1000));
    CHECK(stab->severity == CheckSeverity::Warning);
    const CheckIssue* slow = findIssue(rows, CheckKind::SlowEffects);
    REQUIRE(slow);
    CHECK(slow->count == 2); // the clip with the unanalysed warp and the median, and the analysed one
    CHECK(slow->subject.find("Median") != std::string::npos);
    CHECK(slow->subject.find("Warp Stabilizer") != std::string::npos);
    // A variable frame rate file is a warning of its own.
    project.editMedia(media, [](MediaItem& item) { item.info.video.front().variableFrameRate = true; });
    rows = preExportCheck(project.state, project.seq(), s);
    CHECK(findIssue(rows, CheckKind::MixedFrameRate)->subject == "variable");
    CHECK(findIssue(rows, CheckKind::MixedFrameRate)->severity == CheckSeverity::Warning);

    // The shape of the export: a vertical export of a landscape sequence, by scale mode; the same shape; a larger one.
    s.video.width = 1080;
    s.video.height = 1920;
    rows = preExportCheck(project.state, project.seq(), s);
    const CheckIssue* bars = findIssue(rows, CheckKind::SizeMismatch);
    REQUIRE(bars);
    CHECK(bars->subject == "bars");
    CHECK(bars->fix == CheckFix::MatchSequenceSize);
    CHECK(bars->severity == CheckSeverity::Warning);
    s.video.scaleMode = ScaleMode::Fill;
    CHECK(findIssue(preExportCheck(project.state, project.seq(), s), CheckKind::SizeMismatch)->subject == "crop");
    s.video.scaleMode = ScaleMode::Stretch;
    CHECK(findIssue(preExportCheck(project.state, project.seq(), s), CheckKind::SizeMismatch)->subject == "stretch");
    s.video.scaleMode = ScaleMode::Fit;
    s.video.width = 1280;
    s.video.height = 720; // smaller, same shape: nothing to say
    CHECK(findIssue(preExportCheck(project.state, project.seq(), s), CheckKind::SizeMismatch) == nullptr);
    s.video.width = 3840;
    s.video.height = 2160;
    const auto upRows = preExportCheck(project.state, project.seq(), s);
    const CheckIssue* up = findIssue(upRows, CheckKind::SizeMismatch);
    REQUIRE(up);
    CHECK(up->subject == "upscale");
    CHECK(up->severity == CheckSeverity::Note);
}

TEST_CASE("Pre-export check: captions the file will not show, and captions asked for that do not exist (176)", "[export][check]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1920, 1080, kRate30, Ticks::fromSeconds(4));
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4");
    project.addVideoTrack(media, Ticks{}, Ticks::fromSeconds(4), Ticks{});
    ExportSettings s = checkSettings();
    s.captions.mode = CaptionExport::None;
    CHECK(findIssue(preExportCheck(project.state, project.seq(), s), CheckKind::CaptionsNotInFile) == nullptr);
    // Asked for, none made.
    s.captions.mode = CaptionExport::BurnIn;
    const auto missingRows = preExportCheck(project.state, project.seq(), s);
    const CheckIssue* missing = findIssue(missingRows, CheckKind::CaptionsMissing);
    REQUIRE(missing);
    CHECK(missing->severity == CheckSeverity::Warning);
    // Made, not asked for.
    project.editSequence([](Sequence& seq) {
        CaptionTrack track;
        track.id = CaptionTrackId{1};
        track.cues.push_back(CaptionCue{ms(1000), ms(1000), "Hello", {}});
        track.cues.push_back(CaptionCue{ms(2500), ms(500), "World", {}});
        seq.captionTracks.push_back(std::move(track));
    });
    s.captions.mode = CaptionExport::None;
    const auto notInRows = preExportCheck(project.state, project.seq(), s);
    const CheckIssue* notIn = findIssue(notInRows, CheckKind::CaptionsNotInFile);
    REQUIRE(notIn);
    CHECK(notIn->count == 2);
    CHECK(notIn->fix == CheckFix::TurnOnCaptions);
    CHECK(*notIn->at == ms(1000));
    s.captions.mode = CaptionExport::BurnIn;
    const auto rows = preExportCheck(project.state, project.seq(), s);
    CHECK(findIssue(rows, CheckKind::CaptionsNotInFile) == nullptr);
    CHECK(findIssue(rows, CheckKind::CaptionsMissing) == nullptr);
}

TEST_CASE("Pre-export check: loudness and clipped sound come from a measurement and its platform (176)", "[export][check]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1920, 1080, kRate30, Ticks::fromSeconds(2));
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4");
    project.addVideoTrack(media, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    ExportSettings s = checkSettings();
    // Not measured: no loudness rows.
    CHECK(findIssue(preExportCheck(project.state, project.seq(), s), CheckKind::LoudnessOffTarget) == nullptr);
    CheckContext context;
    context.platformLufs = -14.0;
    context.platformName = "YouTube";
    context.loudness = LoudnessReading{-22.5, -3.0};
    auto rows = preExportCheck(project.state, project.seq(), s, context);
    const CheckIssue* quiet = findIssue(rows, CheckKind::LoudnessOffTarget);
    REQUIRE(quiet);
    CHECK(quiet->fix == CheckFix::TurnOnLoudness);
    CHECK(quiet->subject == "YouTube");
    CHECK(quiet->value == -22.5);
    CHECK(quiet->other == -14.0);
    CHECK(findIssue(rows, CheckKind::AudioClips) == nullptr); // -3 dBTP is under the ceiling
    // Within 2 LU of the target: fine. Clipping peaks: its own row.
    context.loudness = LoudnessReading{-14.8, 0.6};
    rows = preExportCheck(project.state, project.seq(), s, context);
    CHECK(findIssue(rows, CheckKind::LoudnessOffTarget) == nullptr);
    const CheckIssue* clips = findIssue(rows, CheckKind::AudioClips);
    REQUIRE(clips);
    CHECK(clips->value == 0.6);
    CHECK(clips->fix == CheckFix::TurnOnLoudness);
    // Loudness Normalization on: the export fixes both itself.
    s.effects.loudness.applied = true;
    s.effects.loudness.standard = LoudnessStandard::Streaming;
    rows = preExportCheck(project.state, project.seq(), s, context);
    CHECK(findIssue(rows, CheckKind::AudioClips) == nullptr);
    CHECK(findIssue(rows, CheckKind::LoudnessOffTarget) == nullptr);
    // No audio in the export: nothing to say about it.
    s.effects.loudness.applied = false;
    s.audio.enabled = false;
    CHECK(findIssue(preExportCheck(project.state, project.seq(), s, context), CheckKind::AudioClips) == nullptr);
}

TEST_CASE("Pre-export check: settings that do not resolve are the one row, and a clean export has none (176)", "[export][check]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1920, 1080, kRate30, Ticks::fromSeconds(2));
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4");
    project.addVideoTrack(media, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    ExportSettings s = checkSettings();
    s.range = RangeMode::InOut; // the sequence has no In and Out marks
    const auto bad = preExportCheck(project.state, project.seq(), s);
    REQUIRE(bad.size() == 1);
    CHECK(bad.front().kind == CheckKind::InvalidSettings);
    CHECK(bad.front().severity == CheckSeverity::Problem);
    CHECK_FALSE(bad.front().subject.empty());
    // One 60 fps clip in a 60 fps sequence over the whole range, a normal export: nothing found.
    TestProject clean(1920, 1080, Rate{60, 1}, Ticks::fromSeconds(1));
    const MediaId same = clean.addMedia("h264_1080p60_aac.mp4");
    clean.addVideoTrack(same, Ticks{}, Ticks::fromSeconds(1), Ticks{});
    CHECK(preExportCheck(clean.state, clean.seq(), checkSettings()).empty());
}

TEST_CASE("Measuring the loudness of an export's mix works off a snapshot and can be cancelled (176)", "[export][check][media]") {
    if (!haveMedia("sine_48k.wav")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(640, 360, kRate30, Ticks::fromSeconds(1));
    const MediaId tone = project.addMedia("sine_48k.wav");
    project.addAudioTrack(tone, Ticks{}, Ticks::fromSeconds(1), Ticks{});
    ExportSettings s = preset("audio-wav");
    const auto reading = measureExportLoudness(project.state, project.seq(), s, {}, std::stop_token{});
    REQUIRE(reading.has_value());
    CHECK(std::isfinite(reading->integratedLufs));
    CHECK(reading->integratedLufs < 0.0);
    CHECK(reading->integratedLufs > -40.0);
    CHECK(reading->truePeakDbtp <= 0.5);
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto stopped = measureExportLoudness(project.state, project.seq(), s, {}, cancelled.get_token());
    REQUIRE_FALSE(stopped.has_value());
    CHECK(stopped.error().code == ErrorCode::Cancelled);
}

TEST_CASE("export.check lists the pre-export check as data and changes nothing (176)", "[export][check][commands]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project(1920, 1080, kRate30, Ticks::fromSeconds(4));
    const MediaId media = project.addMedia("h264_1080p60_aac.mp4");
    project.addVideoTrack(media, ms(500), Ticks::fromSeconds(2), Ticks{});
    project.editMedia(media, [](MediaItem& item) { item.locator.offline = true; });
    const fs::path dir = freshOutputDir("v07_c4_export_check");
    auto services = ExportCommandServices::standalone(dir / "project.wabcut", dir / "presets", syntheticVideoSourceFactory());
    EditSession session(project.state);
    command::EnginePolicy policy;
    policy.services = services;
    command::CommandEngine engine(session, command::builtinCommands(), policy);
    command::CommandRequest request;
    request.command = "export.check";
    request.params = {{"preset", "youtube-1080p"}};
    const auto result = engine.execute(request);
    REQUIRE(result.ok());
    const command::Json& rows = result.data["rows"];
    REQUIRE(rows.is_array());
    REQUIRE_FALSE(rows.empty());
    // The offline media comes first (a problem), with its place and the fix the app offers.
    CHECK(rows[0]["kind"] == "offlineMedia");
    CHECK(rows[0]["severity"] == "problem");
    CHECK(rows[0]["fix"] == "relink");
    CHECK(rows[0]["at"] == ms(500).value);
    CHECK(rows[0]["message"].get<std::string>().find("h264_1080p60_aac.mp4") != std::string::npos);
    CHECK(result.data["problems"] == 1);
    CHECK(result.data["warnings"].get<int>() >= 1); // the black before the clip
    // An unknown preset is an invalid request, not an empty list.
    request.params = {{"preset", "no-such-preset"}};
    const auto bad = engine.execute(request);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error->code == command::ErrorCode::InvalidParams);
    // The command is read-only: the project is as it was.
    CHECK(session.revision() == 0);
}
