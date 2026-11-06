// Dynamic HDR metadata of an export (card "Dolby Vision 8.4 and HDR10+ dynamic metadata", DynamicMetadata.h): the HDR10+
// plan built from measured frames (shots, the brightness-jump rule, the T.35 payload FFmpeg parses back), the SEI NAL unit
// and its emulation prevention, the insertion into an Annex B access unit, the whole way through FFmpeg's MP4 muxer and
// decoder (an 8-bit libx265 stream: the SEI does not care), the settings rules (HEVC, 10-bit, MP4 / MOV / MKV, a PQ master),
// the Dolby Vision licence gate, preset and override JSON, the pre-export warning, and (on a computer with a 10-bit HEVC
// encoder) a real HDR10+ export read back by media::scanDynamicHdr.

#include "ExportTestUtil.h"

#include <wabcut/export/DynamicMetadata.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/export/PreExportCheck.h>
#include <wabcut/media/DynamicHdr.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/ColorManagementOps.h>
#include <wabcut/model/PreviewCache.h>

#include "HdrDynamic.h"
#include "SmartRender.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/opt.h>
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

constexpr Rate kRate{30, 1};

/// A frame's statistics: a flat picture whose brightest component is `peak` nits and whose mean is `mean`.
FrameLightStats stats(float peak, float mean) {
    FrameLightStats s;
    s.maxR = peak;
    s.maxG = peak * 0.9f;
    s.maxB = peak * 0.8f;
    s.meanMaxRgb = mean;
    for (int i = 0; i < kHdr10PlusPercentiles; ++i) {
        s.percentile[i] = mean * (0.2f + 0.2f * static_cast<float>(i));
    }
    return s;
}

/// FFmpeg's parse of a message of the plan (after the six T.35 header bytes).
AVDynamicHDRPlus parseMessage(const std::vector<std::uint8_t>& message) {
    REQUIRE(message.size() > 6);
    CHECK(message[0] == 0xB5);
    CHECK(message[1] == 0x00);
    CHECK(message[2] == 0x3C);
    CHECK(message[3] == 0x00);
    CHECK(message[4] == 0x01);
    CHECK(message[5] == 0x04);
    AVDynamicHDRPlus parsed{};
    REQUIRE(av_dynamic_hdr_plus_from_t35(&parsed, message.data() + 6, message.size() - 6) >= 0);
    return parsed;
}

double nitsOf(AVRational value) {
    return av_q2d(value) * 10000.0;
}

/// Restores "no Dolby Vision writer" after a test.
struct RegistryGuard {
    ~RegistryGuard() { DolbyVisionRegistry::set({}); }
};

/// A writer that supports what it is told to and writes one made-up NAL unit (a reserved type, which decoders skip; a licensed
/// writer's RPUs are type 62) behind the slices of every frame.
class FakeDolbyWriter final : public DolbyVisionWriter {
public:
    std::string name() const override { return "test writer"; }
    std::expected<void, Error> supports(DynamicMetadata kind) const override {
        if (kind == DynamicMetadata::DolbyVision84 && !profile84) {
            return std::unexpected(Error{ErrorCode::Unsupported, "this writer does not do 8.4"});
        }
        return {};
    }
    std::expected<std::vector<std::uint8_t>, Error> begin(DynamicMetadata, const std::vector<FrameLightStats>& stats,
                                                          const std::vector<std::int64_t>&) override {
        statsSeen = stats.size();
        // AVDOVIDecoderConfigurationRecord: version 1.0, profile 8, level 6, rpu, no el, bl, compatibility 1, md compression 0.
        return std::vector<std::uint8_t>{1, 0, 8, 6, 1, 0, 1, 1, 0};
    }
    std::expected<Units, Error> unitsFor(std::int64_t) override {
        Units units;
        units.after.push_back(exporter::detail::hevcNalUnit(41, std::vector<std::uint8_t>{0x19, 0x08, 0x80}));
        return units;
    }
    bool profile84 = false;
    std::size_t statsSeen = 0;
};

/// An Annex B access unit: VPS, SPS, PPS and one IDR slice.
std::vector<std::uint8_t> accessUnit() {
    std::vector<std::uint8_t> au;
    const auto nal = [&](std::vector<std::uint8_t> bytes, bool longStart) {
        if (longStart) {
            au.push_back(0);
        }
        au.insert(au.end(), {0, 0, 1});
        au.insert(au.end(), bytes.begin(), bytes.end());
    };
    nal({0x40, 0x01, 0x0C, 0x01}, true);        // VPS (32)
    nal({0x42, 0x01, 0x01, 0x01}, true);        // SPS (33)
    nal({0x44, 0x01, 0xC1, 0x72}, true);        // PPS (34)
    nal({0x26, 0x01, 0xAF, 0x11, 0x22}, false); // IDR_W_RADL (19), a three-byte start code
    return au;
}

std::vector<int> nalTypes(const std::uint8_t* data, std::size_t size) {
    std::vector<int> types;
    for (std::size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            types.push_back((data[i + 3] >> 1) & 0x3F);
            i += 2;
        }
    }
    return types;
}

} // namespace

/// Encodes `frames` small pictures with libx265 (8-bit: a stream's SEI and side data do not depend on the bit depth) into an MP4
/// at `file`, putting the NAL units of `injector` into every access unit where Muxer::write does it, and declaring
/// `dovi` (an AVDOVIDecoderConfigurationRecord) as the stream's Dolby Vision configuration when it is not empty. Returns how
/// many access units got NAL units; -1 when libx265 is not in this FFmpeg build.
static int writeHevcMp4(const fs::path& file, int kFrames, exporter::detail::NalInjector* injector, const std::vector<std::uint8_t>& dovi = {}) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
    if (!codec) {
        return -1;
    }
    int injected = 0;
    AVFormatContext* out = nullptr;
    REQUIRE(avformat_alloc_output_context2(&out, nullptr, "mp4", file.string().c_str()) >= 0);
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    REQUIRE(enc);
    enc->width = 128;
    enc->height = 72;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = AVRational{1, 30};
    enc->framerate = AVRational{30, 1};
    enc->gop_size = 8;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(enc->priv_data, "x265-params", "log-level=none:bframes=2", 0);
    REQUIRE(avcodec_open2(enc, codec, nullptr) >= 0);
    AVStream* stream = avformat_new_stream(out, nullptr);
    REQUIRE(stream);
    REQUIRE(avcodec_parameters_from_context(stream->codecpar, enc) >= 0);
    if (!dovi.empty()) {
        auto* record = static_cast<std::uint8_t*>(av_malloc(dovi.size()));
        std::copy(dovi.begin(), dovi.end(), record);
        REQUIRE(av_packet_side_data_add(&stream->codecpar->coded_side_data, &stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF, record, dovi.size(), 0) != nullptr);
    }
    stream->time_base = enc->time_base;
    if (!dovi.empty()) {
        out->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL; // movenc writes 'dvcC' / 'dvvC' only with this
    }
    REQUIRE(avio_open(&out->pb, file.string().c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(out, nullptr) >= 0);
    const auto drain = [&] {
        AVPacket* packet = av_packet_alloc();
        while (avcodec_receive_packet(enc, packet) >= 0) {
            const std::int64_t frame = packet->pts; // the encoder's time base is the frame period
            DolbyVisionWriter::Units none;
            auto units = injector ? injector->unitsFor(frame) : std::expected<DolbyVisionWriter::Units, Error>(none);
            REQUIRE(units);
            if (exporter::detail::insertNalUnits(packet, units->before, units->after)) {
                ++injected;
            }
            av_packet_rescale_ts(packet, enc->time_base, stream->time_base);
            packet->stream_index = stream->index;
            REQUIRE(av_interleaved_write_frame(out, packet) >= 0);
        }
        av_packet_free(&packet);
    };
    AVFrame* frame = av_frame_alloc();
    frame->format = enc->pix_fmt;
    frame->width = enc->width;
    frame->height = enc->height;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(av_frame_make_writable(frame) >= 0);
        for (int y = 0; y < enc->height; ++y) {
            std::fill(frame->data[0] + y * frame->linesize[0], frame->data[0] + y * frame->linesize[0] + enc->width,
                      static_cast<std::uint8_t>(40 + 12 * (i % 8) + y / 2));
        }
        for (int p = 1; p < 3; ++p) {
            for (int y = 0; y < enc->height / 2; ++y) {
                std::fill(frame->data[p] + y * frame->linesize[p], frame->data[p] + y * frame->linesize[p] + enc->width / 2, std::uint8_t{128});
            }
        }
        frame->pts = i;
        REQUIRE(avcodec_send_frame(enc, frame) >= 0);
        drain();
    }
    REQUIRE(avcodec_send_frame(enc, nullptr) >= 0);
    drain();
    REQUIRE(av_write_trailer(out) >= 0);
    avio_closep(&out->pb);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    avformat_free_context(out);

    return injected;
}

TEST_CASE("HDR10+ plan: shots from the cuts where the brightness jumps, maxscl per shot, a message per frame",
          "[export][hdr][dynamic]") {
    // An SDR clip (white at 203 nits) and then an HDR clip with highlights at 1200 nits, cut at frame 4.
    std::vector<FrameLightStats> frames;
    for (int i = 0; i < 4; ++i) {
        frames.push_back(stats(203.0f, 40.0f));
    }
    for (int i = 0; i < 4; ++i) {
        frames.push_back(stats(1200.0f - static_cast<float>(i) * 10.0f, 300.0f + static_cast<float>(i)));
    }
    const auto plan = exporter::detail::planHdr10Plus(frames, {4});
    REQUIRE(plan);
    CHECK(plan->report.kind == DynamicMetadata::Hdr10Plus);
    CHECK(plan->report.frames == 8);
    CHECK(plan->report.targetedDisplayNits == Approx(400.0));
    REQUIRE(plan->report.shots.size() == 2);
    CHECK(plan->report.shots[0].firstFrame == 0);
    CHECK(plan->report.shots[0].frameCount == 4);
    CHECK(plan->report.shots[0].peakNits == Approx(203.0));
    CHECK(plan->report.shots[1].firstFrame == 4);
    CHECK(plan->report.shots[1].peakNits == Approx(1200.0));
    CHECK(plan->report.peakNits == Approx(1200.0));
    REQUIRE(plan->messages.size() == 8);

    // Every message parses back with FFmpeg's reader: the shot's maxscl (the same in every frame of the shot), the frame's
    // own mean and distribution, no tone mapping curve.
    for (std::size_t i = 0; i < plan->messages.size(); ++i) {
        INFO("frame " << i);
        const AVDynamicHDRPlus hdr = parseMessage(plan->messages[i]);
        CHECK(hdr.num_windows == 1);
        CHECK(hdr.application_version == 1);
        CHECK(av_q2d(hdr.targeted_system_display_maximum_luminance) == Approx(400.0)); // nits, not a share of 10 000
        const AVHDRPlusColorTransformParams& p = hdr.params[0];
        const double shotPeak = i < 4 ? 203.0 : 1200.0;
        CHECK(nitsOf(p.maxscl[0]) == Approx(shotPeak).margin(0.2));
        CHECK(nitsOf(p.maxscl[1]) == Approx(shotPeak * 0.9).margin(0.2));
        CHECK(nitsOf(p.maxscl[2]) == Approx(shotPeak * 0.8).margin(0.2));
        CHECK(nitsOf(p.average_maxrgb) == Approx(frames[i].meanMaxRgb).margin(0.2));
        CHECK(p.num_distribution_maxrgb_percentiles == kHdr10PlusPercentiles);
        double previous = 0.0;
        for (int j = 0; j < kHdr10PlusPercentiles; ++j) {
            CHECK(p.distribution_maxrgb[j].percentage == static_cast<int>(kHdr10PlusPercentages[j]));
            const double value = nitsOf(p.distribution_maxrgb[j].percentile);
            CHECK(value >= previous);
            CHECK(value == Approx(frames[i].percentile[j]).margin(0.2));
            previous = value;
        }
        CHECK(p.tone_mapping_flag == 0);
        CHECK(p.color_saturation_mapping_flag == 0);
    }
}

TEST_CASE("HDR10+ plan: an edit point without a jump is not a cut, a dissolve is one shot, empty input is refused",
          "[export][hdr][dynamic]") {
    // An overlay that starts over a running shot (frame 5): the picture's brightness does not change there.
    std::vector<FrameLightStats> steady(10, stats(800.0f, 120.0f));
    const auto overlay = exporter::detail::planHdr10Plus(steady, {5});
    REQUIRE(overlay);
    CHECK(overlay->report.shots.size() == 1);

    // A dissolve from a dark shot to a bright one: the cut candidate sits in the middle of the fade, where neighbouring
    // frames differ by a few percent - one shot, not two.
    std::vector<FrameLightStats> fade;
    for (int i = 0; i < 12; ++i) {
        const float t = static_cast<float>(i) / 11.0f;
        fade.push_back(stats(100.0f + 1000.0f * t, 20.0f + 280.0f * t));
    }
    const auto dissolve = exporter::detail::planHdr10Plus(fade, {6});
    REQUIRE(dissolve);
    CHECK(dissolve->report.shots.size() == 1);
    CHECK(dissolve->report.shots[0].peakNits == Approx(1100.0));

    // The same programme as a hard cut at frame 6: two shots.
    std::vector<FrameLightStats> hard;
    for (int i = 0; i < 12; ++i) {
        hard.push_back(i < 6 ? stats(100.0f, 20.0f) : stats(1100.0f, 300.0f));
    }
    const auto cut = exporter::detail::planHdr10Plus(hard, {6, 6, 99, -1});
    REQUIRE(cut);
    CHECK(cut->report.shots.size() == 2);

    const auto none = exporter::detail::planHdr10Plus({}, {});
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Light statistics of an RGB10 picture: the brightest components, the mean and the distribution in nits",
          "[export][hdr][dynamic]") {
    const auto code = [](double nits) { return media::nitsToPq(nits, 10); };
    const auto nits = [](unsigned c) { return media::pqToNits(c, 10); };
    const auto word = [](unsigned r, unsigned g, unsigned b) { return static_cast<std::uint32_t>(r | (g << 10) | (b << 20)); };
    // 1000 pixels: 900 at (100, 80, 60) nits, 90 at (1000, 500, 200) and 10 highlights at (4000, 900, 300).
    std::vector<std::uint32_t> pixels;
    for (int i = 0; i < 900; ++i) {
        pixels.push_back(word(code(100), code(80), code(60)));
    }
    for (int i = 0; i < 90; ++i) {
        pixels.push_back(word(code(1000), code(500), code(200)));
    }
    for (int i = 0; i < 10; ++i) {
        pixels.push_back(word(code(4000), code(900), code(300)));
    }
    const FrameLightStats s = exporter::detail::measureLightRgb10(pixels.data(), pixels.size());
    CHECK(s.maxR == Approx(nits(code(4000))).epsilon(1e-4));
    CHECK(s.maxG == Approx(nits(code(900))).epsilon(1e-4));
    CHECK(s.maxB == Approx(nits(code(300))).epsilon(1e-4));
    CHECK(s.maxR == Approx(4000.0).epsilon(0.02)); // 10-bit codes: within a couple of percent of what was asked
    // The mean of the per-pixel maximum (R here in every pixel).
    CHECK(s.meanMaxRgb == Approx((900.0 * nits(code(100)) + 90.0 * nits(code(1000)) + 10.0 * nits(code(4000))) / 1000.0).epsilon(1e-4));
    // 90 % of the pixels are at 100 nits: the percentiles up to 90 are 100 nits; 95, 98 and 99 reach the 1000-nit group.
    for (int i = 0; i < kHdr10PlusPercentiles; ++i) {
        INFO("percentile " << kHdr10PlusPercentages[i]);
        const double expected = kHdr10PlusPercentages[i] <= 90.0f ? nits(code(100)) : nits(code(1000));
        CHECK(s.percentile[i] == Approx(expected).epsilon(1e-4));
    }
    // Black, and nothing.
    const std::vector<std::uint32_t> black(64, 0u);
    const FrameLightStats dark = exporter::detail::measureLightRgb10(black.data(), black.size());
    CHECK(dark.maxR == Approx(0.0).margin(1e-6));
    CHECK(dark.meanMaxRgb == Approx(0.0).margin(1e-6));
    CHECK(dark.percentile[kHdr10PlusPercentiles - 1] == Approx(0.0).margin(1e-6));
    const FrameLightStats none = exporter::detail::measureLightRgb10(nullptr, 0);
    CHECK(none.maxR == 0.0f);
    CHECK(none.meanMaxRgb == 0.0f);
}

TEST_CASE("HDR10+ SEI: the NAL unit, its emulation prevention and the insertion into an access unit",
          "[export][hdr][dynamic]") {
    // 00 00 0x (x <= 3) gets an emulation prevention byte; other bytes and longer zero runs are escaped per pair.
    CHECK(exporter::detail::addEmulationPrevention(std::vector<std::uint8_t>{0, 0, 1}) == std::vector<std::uint8_t>{0, 0, 3, 1});
    CHECK(exporter::detail::addEmulationPrevention(std::vector<std::uint8_t>{0, 0, 4}) == std::vector<std::uint8_t>{0, 0, 4});
    CHECK(exporter::detail::addEmulationPrevention(std::vector<std::uint8_t>{0, 0, 0, 0, 2}) == std::vector<std::uint8_t>{0, 0, 3, 0, 0, 3, 2});
    CHECK(exporter::detail::addEmulationPrevention(std::vector<std::uint8_t>{1, 0, 2, 0, 0}) == std::vector<std::uint8_t>{1, 0, 2, 0, 0});

    // The SEI NAL unit of a payload with zero runs: start code, type 39 header, payload type 4, the size, the payload with
    // its escapes, the trailing bits.
    std::vector<std::uint8_t> t35{0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04, 0x01, 0x00, 0x00, 0x01, 0xFF};
    const std::vector<std::uint8_t> nal = exporter::detail::hevcPrefixSeiNal(t35);
    REQUIRE(nal.size() > 6 + 2 + t35.size());
    CHECK(nal[0] == 0);
    CHECK(nal[1] == 0);
    CHECK(nal[2] == 0);
    CHECK(nal[3] == 1);
    CHECK(((nal[4] >> 1) & 0x3F) == 39);
    CHECK(nal[5] == 0x01);
    CHECK(nal[6] == 4);                                // user_data_registered_itu_t_t35
    CHECK(nal[7] == t35.size());                       // the size before escaping
    CHECK(nal.back() == 0x80);
    // Taking the escapes out gives the payload back.
    std::vector<std::uint8_t> raw;
    int zeros = 0;
    for (std::size_t i = 8; i + 1 < nal.size(); ++i) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        raw.push_back(nal[i]);
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    CHECK(raw == t35);
    // A payload of 255 bytes or more has its size written in 0xFF steps.
    const std::vector<std::uint8_t> big(300, 0x55);
    const std::vector<std::uint8_t> bigNal = exporter::detail::hevcPrefixSeiNal(big);
    CHECK(bigNal[6] == 4);
    CHECK(bigNal[7] == 255);
    CHECK(bigNal[8] == 45);

    // Insertion: in front of the first slice and after the parameter sets; `after` goes to the end.
    std::vector<std::uint8_t> au = accessUnit();
    AVPacket* packet = av_packet_alloc();
    REQUIRE(packet);
    REQUIRE(av_new_packet(packet, static_cast<int>(au.size())) == 0);
    std::copy(au.begin(), au.end(), packet->data);
    CHECK(nalTypes(packet->data, static_cast<std::size_t>(packet->size)) == std::vector<int>{32, 33, 34, 19});
    const std::vector<std::vector<std::uint8_t>> before{nal};
    const std::vector<std::vector<std::uint8_t>> after{exporter::detail::hevcNalUnit(62, std::vector<std::uint8_t>{0x19, 0x08, 0x80})}; // an RPU
    REQUIRE(exporter::detail::insertNalUnits(packet, before, after));
    CHECK(nalTypes(packet->data, static_cast<std::size_t>(packet->size)) == std::vector<int>{32, 33, 34, 39, 19, 62});
    CHECK(packet->size == static_cast<int>(au.size() + nal.size() + after[0].size()));
    // The slice and everything before it are untouched.
    CHECK(std::equal(au.begin(), au.begin() + 12, packet->data));
    av_packet_free(&packet);

    // Not an Annex B access unit (a length-prefixed one, empty data): refused, nothing changes.
    AVPacket* other = av_packet_alloc();
    REQUIRE(av_new_packet(other, 16) == 0);
    std::fill(other->data, other->data + 16, std::uint8_t{0x7F});
    CHECK_FALSE(exporter::detail::insertNalUnits(other, before, {}));
    CHECK(other->size == 16);
    AVPacket* paramsOnly = av_packet_alloc();
    const std::vector<std::uint8_t> sets{0, 0, 0, 1, 0x40, 0x01, 0x0C, 0x01, 0, 0, 0, 1, 0x42, 0x01, 0x01, 0x01};
    REQUIRE(av_new_packet(paramsOnly, static_cast<int>(sets.size())) == 0);
    std::copy(sets.begin(), sets.end(), paramsOnly->data);
    CHECK_FALSE(exporter::detail::insertNalUnits(paramsOnly, before, {})); // no slice to put it before
    av_packet_free(&other);
    av_packet_free(&paramsOnly);
}

TEST_CASE("HDR10+ messages put into libx265's packets survive the MP4 muxer and are read back by FFmpeg's decoder",
          "[export][hdr][dynamic]") {
    constexpr int kFrames = 16;
    // Two shots: frames 0-7 dim, 8-15 bright (a hard cut at 8).
    std::vector<FrameLightStats> measured;
    for (int i = 0; i < kFrames; ++i) {
        measured.push_back(i < 8 ? stats(300.0f, 50.0f + static_cast<float>(i)) : stats(1500.0f, 400.0f + static_cast<float>(i)));
    }
    const auto plan = exporter::detail::planHdr10Plus(measured, {8});
    REQUIRE(plan);
    REQUIRE(plan->report.shots.size() == 2);
    auto injector = exporter::detail::makeHdr10PlusInjector(std::make_shared<const exporter::detail::Hdr10PlusPlan>(*plan));

    const fs::path dir = freshOutputDir("hdr-dynamic");
    const fs::path file = dir / "hdr10plus-roundtrip.mp4";

    const int injected = writeHevcMp4(file, kFrames, injector.get());
    if (injected < 0) {
        SKIP("libx265 is not in this FFmpeg build");
    }
    CHECK(injected == kFrames); // every access unit was an Annex B one with a slice

    // Decode it: every frame carries the dynamic metadata of its place in the plan.
    AVFormatContext* in = nullptr;
    REQUIRE(avformat_open_input(&in, file.string().c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(in, nullptr) >= 0);
    const int videoIndex = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(videoIndex >= 0);
    const AVCodec* decoder = avcodec_find_decoder(in->streams[videoIndex]->codecpar->codec_id);
    REQUIRE(decoder);
    AVCodecContext* dec = avcodec_alloc_context3(decoder);
    REQUIRE(avcodec_parameters_to_context(dec, in->streams[videoIndex]->codecpar) >= 0);
    REQUIRE(avcodec_open2(dec, decoder, nullptr) >= 0);
    std::map<std::int64_t, std::pair<double, double>> byFrame; // frame index -> (maxscl, average) as decoded
    const AVStream* videoStream = in->streams[videoIndex];
    AVPacket* packet = av_packet_alloc();
    AVFrame* decoded = av_frame_alloc();
    const auto collect = [&] {
        while (avcodec_receive_frame(dec, decoded) >= 0) {
            const AVFrameSideData* sd = av_frame_get_side_data(decoded, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
            REQUIRE(sd != nullptr);
            const auto* hdr = reinterpret_cast<const AVDynamicHDRPlus*>(sd->data);
            const std::int64_t index = av_rescale_q(decoded->pts - (videoStream->start_time == AV_NOPTS_VALUE ? 0 : videoStream->start_time),
                                                    videoStream->time_base, AVRational{1, 30});
            byFrame[index] = {nitsOf(hdr->params[0].maxscl[0]), nitsOf(hdr->params[0].average_maxrgb)};
            av_frame_unref(decoded);
        }
    };
    while (av_read_frame(in, packet) >= 0) {
        if (packet->stream_index == videoIndex) {
            REQUIRE(avcodec_send_packet(dec, packet) >= 0);
            collect();
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(dec, nullptr);
    collect();
    av_packet_free(&packet);
    av_frame_free(&decoded);
    avcodec_free_context(&dec);
    avformat_close_input(&in);
    // Every frame the decoder delivered carries the metadata of its place in the plan (the MP4 edit list of a stream with
    // B-frames may hide the first frame from a player: at most one is missing).
    REQUIRE(byFrame.size() >= static_cast<std::size_t>(kFrames - 1));
    for (const auto& [index, value] : byFrame) {
        INFO("frame " << index);
        REQUIRE(index >= 0);
        REQUIRE(index < kFrames);
        CHECK(value.first == Approx(index < 8 ? 300.0 : 1500.0).margin(0.2)); // the shot's maxscl
        CHECK(value.second == Approx(measured[static_cast<std::size_t>(index)].meanMaxRgb).margin(0.2));
    }

    // And media::scanDynamicHdr reads the same from the file (and says it is HDR10+ with application version 1).
    const auto report = media::scanDynamicHdr(file.string(), -1, 24);
    REQUIRE(report);
    CHECK(report->framesScanned >= kFrames - 1);
    CHECK(report->hdr10PlusFrames == report->framesScanned);
    CHECK(report->hdr10PlusApplicationVersion == 1);
    CHECK(report->hasHdr10Plus());
    CHECK(report->hdr10PlusPeakNits == Approx(1500.0).margin(0.5));
    CHECK(report->targetedDisplayNits == Approx(400.0));
    CHECK_FALSE(report->hdr10PlusToneCurve);
    CHECK_FALSE(report->hasDolbyVision());
}

TEST_CASE("A Dolby Vision configuration and RPU NAL units from a writer go through the MP4 muxer and are read back as the file's Dolby Vision",
          "[export][hdr][dynamic]") {
    // The writer's NAL units (here a made-up one of a reserved type after the slices) and its configuration record, 8.4: HLG base.
    struct WriterInjector final : exporter::detail::NalInjector {
        std::expected<DolbyVisionWriter::Units, Error> unitsFor(std::int64_t frame) override { return writer.unitsFor(frame); }
        FakeDolbyWriter writer;
    } injector;
    const std::vector<std::uint8_t> record{1, 0, 8, 6, 1, 0, 1, 4, 0}; // version 1.0, profile 8, level 6, RPU, BL, compatibility 4
    const fs::path dir = freshOutputDir("hdr-dynamic");
    const fs::path file = dir / "dolby-roundtrip.mp4";
    const int injected = writeHevcMp4(file, 8, &injector, record);
    if (injected < 0) {
        SKIP("libx265 is not in this FFmpeg build");
    }
    CHECK(injected == 8);
    const auto config = media::readDolbyVisionConfig(file.string());
    REQUIRE(config);
    REQUIRE(config->has_value());
    CHECK((*config)->profile == 8);
    CHECK((*config)->level == 6);
    CHECK((*config)->blCompatibilityId == 4);
    CHECK((*config)->rpuPresent);
    CHECK_FALSE((*config)->elPresent);
    CHECK((*config)->blPresent);
    CHECK((*config)->name() == "8.4");
    // The probe stores it on the stream; the file still decodes (the RPU NAL units are skipped by decoders that do not use them).
    const auto info = media::probe(file.string());
    REQUIRE(info);
    REQUIRE_FALSE(info->video.empty());
    REQUIRE(info->video.front().dolbyVision.has_value());
    CHECK(*info->video.front().dolbyVision == **config);
    CHECK_FALSE(info->video.front().hdr10Plus);
    const auto scan = media::scanDynamicHdr(file.string(), -1, 8);
    REQUIRE(scan);
    CHECK(scan->framesScanned >= 7);
    CHECK(scan->hasDolbyVision());
    CHECK(scan->dolbyVision == **config);
    CHECK_FALSE(scan->hasHdr10Plus());
    // A file with none: nothing declared.
    const fs::path plain = dir / "plain.mp4";
    REQUIRE(writeHevcMp4(plain, 4, nullptr) == 0);
    const auto none = media::readDolbyVisionConfig(plain.string());
    REQUIRE(none);
    CHECK_FALSE(none->has_value());
    CHECK_FALSE(media::probe(plain.string())->video.front().dolbyVision.has_value());
    CHECK_FALSE(media::readDolbyVisionConfig((dir / "missing.mp4").string()).has_value());
}

TEST_CASE("Dynamic metadata settings: HEVC, 10-bit, MP4 / MOV / MKV and a PQ master; Dolby Vision needs a licensed writer",
          "[export][hdr][dynamic]") {
    RegistryGuard guard;
    ExportSettings hdr = [] {
        auto preset = findPreset("youtube-hdr-pq");
        REQUIRE(preset.has_value());
        return preset->settings;
    }();
    REQUIRE(hdr.video.codec == VideoCodec::Hevc);
    REQUIRE(hdr.video.pixelFormat == OutputPixelFormat::Yuv420p10);
    Sequence sequence;
    sequence.width = 1920;
    sequence.height = 1080;
    sequence.frameRate = kRate;
    sequence.duration = Ticks::fromSeconds(10);
    ColorManagement pq;
    pq.mode = ColorManagementMode::HdrPq;

    // HDR10+ on a PQ HEVC master in MP4: resolves, and the resolved export names it.
    hdr.video.dynamicMetadata = DynamicMetadata::Hdr10Plus;
    const auto ok = resolve(hdr, sequence, pq);
    REQUIRE(ok);
    CHECK(ok->dynamicMetadata == DynamicMetadata::Hdr10Plus);
    CHECK(ok->dynamicRange == DynamicRange::Pq);
    // Smart rendering keeps its hands off such an export (copied packets carry their own metadata or none).
    hdr.render.smartRender = true;
    CHECK_FALSE(exporter::detail::smartRenderEligible(hdr));
    hdr.render.smartRender = false;
    // MOV and MKV are fine.
    for (const Container container : {Container::Mov, Container::Mkv}) {
        ExportSettings other = hdr;
        other.container = container;
        CHECK(validate(other).has_value());
    }

    // Each rule says what it needs.
    {
        ExportSettings s = hdr;
        s.video.codec = VideoCodec::Av1;
        const auto r = validate(s);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("HEVC video only") != std::string::npos);
    }
    {
        ExportSettings s = hdr;
        s.video.pixelFormat = OutputPixelFormat::Yuv420p;
        s.video.dynamicRange = DynamicRange::Auto;
        const auto r = validate(s);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("10-bit 4:2:0") != std::string::npos);
    }
    {
        ExportSettings s = hdr;
        s.container = Container::Mxf;
        const auto r = validate(s);
        REQUIRE_FALSE(r);
        CHECK_FALSE(r.error().message.empty());
    }
    {
        ExportSettings s = hdr;
        s.video.dynamicRange = DynamicRange::Sdr;
        const auto r = resolve(s, sequence, pq);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("needs a PQ master") != std::string::npos);
    }
    {
        ExportSettings s = hdr; // an SDR sequence cannot make a PQ master either
        const auto r = resolve(s, sequence, ColorManagement{});
        CHECK_FALSE(r.has_value());
    }

    // Dolby Vision: refused with the reason when no licensed writer is registered ...
    ExportSettings dolby = hdr;
    dolby.video.dynamicMetadata = DynamicMetadata::DolbyVision81;
    const auto refused = validate(dolby);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("Dolby licence") != std::string::npos);
    CHECK(refused.error().message.find("HDR10, HDR10+ and HLG exports are not affected") != std::string::npos);
    CHECK_FALSE(DolbyVisionRegistry::unavailableReason(DynamicMetadata::DolbyVision84).empty());
    CHECK(DolbyVisionRegistry::unavailableReason(DynamicMetadata::Hdr10Plus).empty()); // not Dolby's
    CHECK(DolbyVisionRegistry::unavailableReason(DynamicMetadata::None).empty());
    // ... and accepted per profile where one is: 8.1 yes (PQ), 8.4 (HLG) the writer says no to.
    auto writer = std::make_shared<FakeDolbyWriter>();
    DolbyVisionRegistry::set(writer);
    CHECK(DolbyVisionRegistry::unavailableReason(DynamicMetadata::DolbyVision81).empty());
    CHECK(validate(dolby).has_value());
    const auto resolved81 = resolve(dolby, sequence, pq);
    REQUIRE(resolved81);
    CHECK(resolved81->dynamicMetadata == DynamicMetadata::DolbyVision81);
    ExportSettings dolby84 = hdr;
    dolby84.video.dynamicMetadata = DynamicMetadata::DolbyVision84;
    const auto no84 = validate(dolby84);
    REQUIRE_FALSE(no84);
    CHECK(no84.error().message == "this writer does not do 8.4");
    writer->profile84 = true;
    // 8.4 is an HLG master: a PQ one is refused, an HLG one resolves.
    const auto wrongRange = resolve(dolby84, sequence, pq);
    REQUIRE_FALSE(wrongRange);
    CHECK(wrongRange.error().message.find("needs an HLG master") != std::string::npos);
    ExportSettings hlg = dolby84;
    hlg.video.dynamicRange = DynamicRange::Hlg;
    ColorManagement hlgSetup;
    hlgSetup.mode = ColorManagementMode::HdrHlg;
    const auto resolved84 = resolve(hlg, sequence, hlgSetup);
    REQUIRE(resolved84);
    CHECK(resolved84->dynamicRange == DynamicRange::Hlg);
    // The names the presets and overrides use.
    for (const DynamicMetadata kind :
         {DynamicMetadata::None, DynamicMetadata::Hdr10Plus, DynamicMetadata::DolbyVision81, DynamicMetadata::DolbyVision84}) {
        CHECK(dynamicMetadataFromName(dynamicMetadataName(kind)) == kind);
    }
    CHECK_FALSE(dynamicMetadataFromName("hdr11").has_value());
}

TEST_CASE("Dynamic metadata in presets and overrides: saved only when set, an override picks the master it describes",
          "[export][hdr][dynamic]") {
    ExportSettings plain;
    CHECK(settingsToJson(plain).dump().find("dynamicMetadata") == std::string::npos);

    ExportSettings s;
    s.video.codec = VideoCodec::Hevc;
    s.video.pixelFormat = OutputPixelFormat::Yuv420p10;
    s.video.dynamicRange = DynamicRange::Pq;
    s.video.dynamicMetadata = DynamicMetadata::Hdr10Plus;
    const Json json = settingsToJson(s);
    CHECK(json.dump().find("\"dynamicMetadata\":\"hdr10plus\"") != std::string::npos);
    const auto back = settingsFromJson(json);
    REQUIRE(back);
    CHECK(back->video.dynamicMetadata == DynamicMetadata::Hdr10Plus);
    // A damaged value is refused, not guessed.
    Json bad = json;
    bad["video"]["dynamicMetadata"] = "turbo";
    CHECK_FALSE(settingsFromJson(bad).has_value());

    // The override: HDR10+ on an Auto, 8-bit HEVC export sets the PQ master and the 10-bit format it needs.
    ExportSettings overridden;
    overridden.video.codec = VideoCodec::Hevc;
    REQUIRE(overridden.video.pixelFormat == OutputPixelFormat::Yuv420p);
    REQUIRE(applyOverrides(overridden, Json{{"dynamicMetadata", "hdr10plus"}}));
    CHECK(overridden.video.dynamicMetadata == DynamicMetadata::Hdr10Plus);
    CHECK(overridden.video.dynamicRange == DynamicRange::Pq);
    CHECK(overridden.video.pixelFormat == OutputPixelFormat::Yuv420p10);
    REQUIRE(applyOverrides(overridden, Json{{"dynamicMetadata", "none"}}));
    CHECK(overridden.video.dynamicMetadata == DynamicMetadata::None);
    const auto unknown = applyOverrides(overridden, Json{{"dynamicMetadata", "hdr11"}});
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().message.find("dynamicMetadata") != std::string::npos);
}

TEST_CASE("Pre-export check: clips with HDR10+ or Dolby Vision metadata that the HDR export does not keep",
          "[export][hdr][dynamic]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project = threeLayerTimeline(1280, 720, kRate, Ticks::fromSeconds(2));
    ColorManagement pq;
    pq.mode = ColorManagementMode::HdrPq;
    auto managed = edit::setColorManagement(project.state, pq);
    REQUIRE(managed);
    project.state = *managed;
    ExportSettings settings = findPreset("youtube-hdr-pq")->settings;
    settings.range = RangeMode::WholeSequence;
    const auto rowsOf = [&](const ExportSettings& s, const char* subject) {
        std::int64_t clips = 0;
        for (const CheckIssue& row : preExportCheck(project.state, project.seq(), s, CheckContext{})) {
            if (row.kind == CheckKind::DynamicMetadataDropped && row.subject == subject) {
                clips += row.count;
            }
        }
        return clips;
    };
    CHECK(rowsOf(settings, "hdr10plus") == 0); // nothing carries it yet
    CHECK(rowsOf(settings, "dolbyVision") == 0);

    for (MediaPtr& m : project.state.media) {
        auto item = std::make_shared<MediaItem>(*m);
        if (!item->info.video.empty()) {
            item->info.video.front().hdr10Plus = true;
            item->info.video.front().dolbyVision = DolbyVisionInfo{8, 6, 1, true, false, true};
        }
        m = item;
    }
    const std::int64_t carriers = rowsOf(settings, "hdr10plus");
    CHECK(carriers >= 1);
    CHECK(rowsOf(settings, "dolbyVision") == carriers);
    // The message names the cure.
    for (const CheckIssue& row : preExportCheck(project.state, project.seq(), settings, CheckContext{})) {
        if (row.kind == CheckKind::DynamicMetadataDropped && row.subject == "hdr10plus") {
            CHECK(row.severity == CheckSeverity::Warning);
            CHECK(checkIssueMessage(row, project.seq()).find("set Dynamic metadata to HDR10+") != std::string::npos);
        }
        if (row.kind == CheckKind::DynamicMetadataDropped && row.subject == "dolbyVision") {
            CHECK(checkIssueMessage(row, project.seq()).find("HDR10 or HLG metadata only") != std::string::npos);
        }
    }
    // Asking for HDR10+ measures it again: that row goes; an SDR export has no dynamic metadata to lose.
    ExportSettings measured = settings;
    measured.video.dynamicMetadata = DynamicMetadata::Hdr10Plus;
    CHECK(rowsOf(measured, "hdr10plus") == 0);
    CHECK(rowsOf(measured, "dolbyVision") == carriers);
    ExportSettings sdr = settings;
    sdr.video.dynamicRange = DynamicRange::Sdr;
    sdr.video.pixelFormat = OutputPixelFormat::Yuv420p;
    CHECK(rowsOf(sdr, "hdr10plus") == 0);
    CHECK(rowsOf(sdr, "dolbyVision") == 0);
}

TEST_CASE("Shots: the sequence's video edit points inside the exported range, as output frames",
          "[export][hdr][dynamic]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(1280, 720, kRate, Ticks::fromSeconds(2));
    const TimeRange range{Ticks{}, Ticks::fromSeconds(2)};
    const std::vector<std::int64_t> starts = exporter::detail::shotStartsOf(project.state, project.sequence, range, kRate, 60);
    REQUIRE_FALSE(starts.empty());
    CHECK(starts.front() == 0);
    CHECK(std::is_sorted(starts.begin(), starts.end()));
    CHECK(std::adjacent_find(starts.begin(), starts.end()) == starts.end());
    CHECK(starts.back() < 60);
    // V2 starts at L/4 = 15 frames and V3 at L/2 = 30: both are edit points of the timeline.
    CHECK(std::find(starts.begin(), starts.end(), 15) != starts.end());
    CHECK(std::find(starts.begin(), starts.end(), 30) != starts.end());
    // A range that starts later numbers its frames from its own start.
    const std::vector<std::int64_t> later =
        exporter::detail::shotStartsOf(project.state, project.sequence, TimeRange{Ticks::fromSeconds(1), Ticks::fromSeconds(1)}, kRate, 30);
    CHECK(later.front() == 0);
    CHECK(std::all_of(later.begin(), later.end(), [](std::int64_t k) { return k >= 0 && k < 30; }));
}

TEST_CASE("An HDR10+ export on a 10-bit HEVC encoder carries the metadata in every frame and reads back as HDR10+",
          "[export][render][media][gpu][hdr][dynamic]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    ExportSettings settings = findPreset("youtube-hdr-pq")->settings;
    settings.range = RangeMode::Custom;
    settings.customRange = TimeRange{Ticks{}, Ticks::fromSeconds(1)};
    settings.video.width.reset();
    settings.video.height.reset();
    settings.video.dynamicMetadata = DynamicMetadata::Hdr10Plus;
    const auto probe = probeVideoEncoder(settings.video, 1280, 720, kRate);
    if (!probe) {
        SKIP(std::format("no 10-bit HEVC encoder here: {}", probe.error().message));
    }
    TestProject project = motionTimeline(1280, 720, kRate);
    ColorManagement management;
    management.mode = ColorManagementMode::HdrPq;
    auto set = edit::setColorManagement(project.state, management);
    REQUIRE(set);
    project.state = *set;
    const ClipId v1 = project.seq().videoTracks.front()->clips.front().id;
    const ClipId one[] = {v1};
    auto pqClip = edit::setClipInputColorSpace(project.state, project.sequence, one, InputColorSpace::Rec2100Pq);
    REQUIRE(pqClip);
    project.state = pqClip->state;

    const fs::path dir = freshOutputDir("hdr10plus-export");
    const fs::path output = dir / "hdr10plus.mp4";
    ExportRequest request = makeRequest(project, settings, output);
    request.videoSource = {}; // the rendered source
    ExportProgress last;
    bool sawAnalysis = false;
    request.onProgress = [&](const ExportProgress& p) {
        sawAnalysis = sawAnalysis || p.analyzing;
        last = p;
    };
    ExportJob job(std::move(request));
    const auto result = job.run();
    REQUIRE(result);
    CHECK(sawAnalysis); // the first pass reported itself
    CHECK_FALSE(last.analyzing);
    CHECK(result->dynamicMetadata.kind == DynamicMetadata::Hdr10Plus);
    CHECK(result->dynamicMetadata.written);
    CHECK(result->dynamicMetadata.frames == result->frames);
    CHECK_FALSE(result->dynamicMetadata.shots.empty());
    CHECK(result->dynamicMetadata.peakNits > 0.0);
    CHECK(result->dynamicMetadata.averageNits > 0.0);
    CHECK(result->dynamicMetadata.peakNits >= result->dynamicMetadata.averageNits);
    CHECK(result->warnings.empty());
    std::int64_t shotFrames = 0;
    for (const ShotReport& shot : result->dynamicMetadata.shots) {
        shotFrames += shot.frameCount;
    }
    CHECK(shotFrames == result->frames);

    // The file: HDR10+ in the first frames, the stream still HEVC PQ, and the probe flags it.
    const auto scan = media::scanDynamicHdr(output.string(), -1, 24);
    REQUIRE(scan);
    CHECK(scan->hdr10PlusFrames == scan->framesScanned);
    CHECK(scan->hdr10PlusFrames > 0);
    CHECK(scan->hdr10PlusApplicationVersion == 1);
    CHECK(scan->hdr10PlusPeakNits > 0.0);
    const auto info = media::probe(output.string());
    REQUIRE(info);
    REQUIRE_FALSE(info->video.empty());
    CHECK(info->video.front().hdr10Plus);
    CHECK(info->video.front().color.transfer == TransferFunction::PQ);
    CHECK_FALSE(info->video.front().dolbyVision.has_value());
}

TEST_CASE("A Dolby Vision export with a registered writer carries its configuration and RPU NAL units",
          "[export][render][media][gpu][hdr][dynamic]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    RegistryGuard guard;
    auto writer = std::make_shared<FakeDolbyWriter>();
    DolbyVisionRegistry::set(writer);
    ExportSettings settings = findPreset("youtube-hdr-pq")->settings;
    settings.range = RangeMode::Custom;
    settings.customRange = TimeRange{Ticks{}, Ticks::fromSeconds(1)};
    settings.video.width.reset();
    settings.video.height.reset();
    settings.video.dynamicMetadata = DynamicMetadata::DolbyVision81;
    const auto probe = probeVideoEncoder(settings.video, 1280, 720, kRate);
    if (!probe) {
        SKIP(std::format("no 10-bit HEVC encoder here: {}", probe.error().message));
    }
    TestProject project = motionTimeline(1280, 720, kRate);
    ColorManagement management;
    management.mode = ColorManagementMode::HdrPq;
    auto set = edit::setColorManagement(project.state, management);
    REQUIRE(set);
    project.state = *set;
    const fs::path dir = freshOutputDir("dolby-export");
    const fs::path output = dir / "dolby.mp4";
    ExportRequest request = makeRequest(project, settings, output);
    request.videoSource = {};
    ExportJob job(std::move(request));
    const auto result = job.run();
    REQUIRE(result);
    CHECK(writer->statsSeen == static_cast<std::size_t>(result->frames)); // it got the measurements of every frame
    CHECK(result->dynamicMetadata.kind == DynamicMetadata::DolbyVision81);
    CHECK(result->dynamicMetadata.written);
    // The container declares profile 8.1 (level 6) and the probe reads it back.
    const auto config = media::readDolbyVisionConfig(output.string());
    REQUIRE(config);
    REQUIRE(config->has_value());
    CHECK((*config)->profile == 8);
    CHECK((*config)->level == 6);
    CHECK((*config)->blCompatibilityId == 1);
    CHECK((*config)->rpuPresent);
    CHECK((*config)->name() == "8.1");
}
