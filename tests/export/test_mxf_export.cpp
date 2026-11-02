// 0.6 (#73): broadcast delivery. MXF OP1a (XDCAM HD422, DNxHD / DNxHR, AVC-Intra 100) and OP-Atom (Avid MediaFiles:
// a file per essence) checked with ffprobe and libavformat - operational pattern, codec, timecode, reel, UMID, PCM
// tracks - and embedded closed captions read back from the files: CEA-608 through FFmpeg's own 608 decoder (cc_dec, a
// reference independent of Wabcut's writer) and CEA-708 through readCea708, from an SMPTE 436M track (whose CDPs are
// checked too), from H.264 caption SEI and from a QuickTime c608 track; at 23.976, 25, 29.97, 50 and 59.94 fps. Formats
// that cannot carry what is asked are refused with the reason.

#include "ExportTestUtil.h"

#include <wabcut/export/ClosedCaptions.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/interchange/CaptionFiles.h>
#include <wabcut/model/Timecode.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/smpte_436m.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

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

/// A sequence of `length` at `rate` and `width` x `height`, start timecode 01:00:00:00 (drop-frame at 29.97 / 59.94),
/// a caption track "English" with `cues` (default: "Hello" 0.2-0.8 s, "World" 1.0-1.6 s).
TestProject captioned(int width, int height, Rate rate, Ticks length, std::vector<CaptionCue> cues = {}) {
    TestProject project(width, height, rate, length);
    if (cues.empty()) {
        cues = {{ms(200), ms(600), "Hello"}, {ms(1000), ms(600), "World"}};
    }
    CaptionTrack track;
    track.id = project.state.ids.allocate<CaptionTrackId>();
    track.name = "English";
    track.language = "en";
    track.enabled = true;
    track.cues = std::move(cues);
    // Drop-frame timecode where it exists (29.97, 59.94); 01:00:00:00 counted in the sequence's own frames.
    const bool dropFrame = rate == kRate29_97 || rate == kRate59_94;
    const auto hour = parseTimecode("01:00:00:00", rate, dropFrame);
    REQUIRE(hour.has_value());
    project.editSequence([&](Sequence& s) {
        s.captionTracks = {track};
        s.startTimecode = *hour;
        s.dropFrame = dropFrame;
    });
    return project;
}

std::string tag(const std::map<std::string, std::string>& m, const std::string& key) {
    const auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

int countType(const ProbeResult& probe, std::string_view type) {
    return static_cast<int>(std::count_if(probe.streams.begin(), probe.streams.end(),
                                          [&](const auto& s) { return tag(s, "codec_type") == type; }));
}

struct AvInput {
    AVFormatContext* ctx = nullptr;
    ~AvInput() { avformat_close_input(&ctx); }
};

/// Every packet of the first stream of `id` in `file`, in order (MXF: `extract608` turns the 436M track into cc_data).
std::vector<std::vector<std::uint8_t>> packetsOf(const fs::path& file, AVCodecID id, bool extract608 = false) {
    std::vector<std::vector<std::uint8_t>> out;
    AvInput in;
    AVDictionary* opts = nullptr;
    if (extract608) {
        av_dict_set(&opts, "eia608_extract", "1", 0);
    }
    const int opened = avformat_open_input(&in.ctx, file.string().c_str(), nullptr, &opts);
    av_dict_free(&opts);
    REQUIRE(opened == 0);
    REQUIRE(avformat_find_stream_info(in.ctx, nullptr) >= 0);
    int stream = -1;
    for (unsigned i = 0; i < in.ctx->nb_streams; ++i) {
        if (in.ctx->streams[i]->codecpar->codec_id == id) {
            stream = static_cast<int>(i);
            break;
        }
    }
    REQUIRE(stream >= 0);
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(in.ctx, packet) >= 0) {
        if (packet->stream_index == stream) {
            out.emplace_back(packet->data, packet->data + packet->size);
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return out;
}

/// The A/53 caption data of each decoded video frame of `file` (display order).
std::vector<std::vector<std::uint8_t>> pictureCaptionData(const fs::path& file) {
    std::vector<std::vector<std::uint8_t>> out;
    AvInput in;
    REQUIRE(avformat_open_input(&in.ctx, file.string().c_str(), nullptr, nullptr) == 0);
    REQUIRE(avformat_find_stream_info(in.ctx, nullptr) >= 0);
    const int stream = av_find_best_stream(in.ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(stream >= 0);
    const AVCodec* codec = avcodec_find_decoder(in.ctx->streams[stream]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, in.ctx->streams[stream]->codecpar);
    REQUIRE(avcodec_open2(ctx, codec, nullptr) == 0);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC);
            out.push_back(sd ? std::vector<std::uint8_t>(sd->data, sd->data + sd->size) : std::vector<std::uint8_t>{});
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(in.ctx, packet) >= 0) {
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
    return out;
}

/// What FFmpeg's CEA-608 decoder (cc_dec) shows from per-frame cc_data, field 1 or 2: the subtitle texts in order.
std::vector<std::string> decode608(const std::vector<std::vector<std::uint8_t>>& frames, Rate rate, int field) {
    std::vector<std::string> texts;
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_EIA_608);
    REQUIRE(dec != nullptr);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    ctx->pkt_timebase = AVRational{rate.den, rate.num};
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "data_field", field == 1 ? "first" : "second", 0);
    REQUIRE(avcodec_open2(ctx, dec, &opts) == 0);
    av_dict_free(&opts);
    const auto take = [&](AVPacket* packet) {
        AVSubtitle sub{};
        int got = 0;
        if (avcodec_decode_subtitle2(ctx, &sub, &got, packet) >= 0 && got) {
            for (unsigned i = 0; i < sub.num_rects; ++i) {
                if (sub.rects[i]->ass) {
                    texts.emplace_back(sub.rects[i]->ass);
                }
            }
            avsubtitle_free(&sub);
        }
    };
    AVPacket* packet = av_packet_alloc();
    for (std::size_t k = 0; k < frames.size(); ++k) {
        if (frames[k].empty()) {
            continue;
        }
        REQUIRE(av_new_packet(packet, static_cast<int>(frames[k].size())) == 0);
        std::copy(frames[k].begin(), frames[k].end(), packet->data);
        packet->pts = static_cast<std::int64_t>(k);
        take(packet);
        av_packet_unref(packet);
    }
    av_packet_unref(packet); // flush: a pop-on caption still on screen
    take(packet);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    return texts;
}

bool anyContains(const std::vector<std::string>& texts, std::string_view word) {
    return std::any_of(texts.begin(), texts.end(), [&](const std::string& t) { return t.find(word) != std::string::npos; });
}

std::string joined(const std::vector<std::string>& texts) {
    std::string out;
    for (const std::string& t : texts) {
        out += "[" + t + "] ";
    }
    return out;
}

std::string describe(const Cea708Reading& r) {
    std::string out = std::format("{} packets, {} errors:", r.packets, r.errors);
    for (const Cea708Caption& c : r.captions) {
        out += std::format(" [{}-{} \"{}\"]", c.startFrame, c.endFrame, c.text);
    }
    return out;
}

ExportResult run(ExportRequest request) {
    auto result = ExportJob(std::move(request)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    return *result;
}

/// The largest absolute sample of a pcm_s24le MXF track (its raw packets).
std::int32_t peak24(const std::vector<std::vector<std::uint8_t>>& packets) {
    std::int32_t peak = 0;
    for (const auto& p : packets) {
        for (std::size_t i = 0; i + 2 < p.size(); i += 3) {
            std::int32_t v = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16);
            if (v & 0x800000) {
                v -= 0x1000000;
            }
            peak = std::max(peak, std::abs(v));
        }
    }
    return peak;
}

} // namespace

// ---- Closed captions on their own ---------------------------------------------------------------------------------

TEST_CASE("Closed captions: cc_count follows the frame rate and 608 moves to CC2 CC3 and CC4", "[export][captions][mxf]") {
    CHECK(ccCountForRate(kRate23_976) == 25);
    CHECK(ccCountForRate(kRate24) == 25);
    CHECK(ccCountForRate(kRate25) == 24);
    CHECK(ccCountForRate(kRate29_97) == 20);
    CHECK(ccCountForRate(kRate30) == 20);
    CHECK(ccCountForRate(kRate50) == 12);
    CHECK(ccCountForRate(kRate59_94) == 10);
    CHECK(ccCountForRate(kRate60) == 10);
    // Resume Caption Loading (14h 20h, odd parity 94h 20h): CC2 1Ch (parity: 1Ch has three ones), CC3 15h -> 15h,
    // CC4 1Dh -> 9Dh. A preamble address code (14h 70h) keeps 14h on field 2; characters never change.
    const std::array<std::uint8_t, 2> rcl{0x94, 0x20};
    CHECK(cea608PairForChannel(rcl, 1) == rcl);
    CHECK(cea608PairForChannel(rcl, 2) == std::array<std::uint8_t, 2>{0x1C, 0x20});
    CHECK(cea608PairForChannel(rcl, 3) == std::array<std::uint8_t, 2>{0x15, 0x20});
    CHECK(cea608PairForChannel(rcl, 4) == std::array<std::uint8_t, 2>{0x9D, 0x20});
    const std::array<std::uint8_t, 2> pac{0x94, 0x70};
    CHECK(cea608PairForChannel(pac, 3) == pac);
    CHECK(cea608PairForChannel(pac, 4) == std::array<std::uint8_t, 2>{0x1C, 0x70});
    const std::array<std::uint8_t, 2> text{0xC8, 0xE5}; // "He"
    CHECK(cea608PairForChannel(text, 4) == text);
}

TEST_CASE("Closed captions: a 708 service with two pop-on windows round trips with gaps and the 608 fallback",
          "[export][captions][mxf]") {
    // Back to back, then a gap: windows 0, 1, 0 take turns; the third caption is hidden at its end.
    const std::vector<CaptionCue> cues{{ms(200), ms(400), "First caption"},
                                       {ms(600), ms(400), "Second one\non two lines"},
                                       {ms(1000), ms(400), "Third \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x99\xAA"},
                                       {ms(2000), ms(500), "After a gap"}};
    const TimeRange range{Ticks{}, Ticks::fromSeconds(3)};
    const Rate rate = kRate29_97;
    const std::int64_t frames = 90;
    ClosedCaptionOptions o;
    o.service708 = 1;
    const ClosedCaptionData cc = buildClosedCaptions(cues, range, rate, frames, o);
    REQUIRE(cc.frames == frames);
    REQUIRE(cc.ccCount == 20);
    CHECK(cc.cues == 4);
    CHECK(cc.late708 == 0);
    std::vector<std::vector<std::uint8_t>> perFrame;
    for (std::int64_t k = 0; k < frames; ++k) {
        const auto f = cc.frame(k);
        REQUIRE(f.size() == 60);
        perFrame.emplace_back(f.begin(), f.end());
    }
    const Cea708Reading r = readCea708(perFrame, 1);
    INFO(describe(r));
    CHECK(r.errors == 0);
    CHECK(r.services == std::vector<int>{1});
    CHECK(r.windowsDefined == std::vector<int>{0, 1});
    REQUIRE(r.captions.size() == 4);
    const auto frameOf = [&](Ticks t) { return *ticksToFrames(t, rate, Rounding::Nearest); };
    CHECK(r.captions[0].text == "First caption");
    CHECK(r.captions[0].startFrame == frameOf(ms(200)));
    CHECK(r.captions[0].endFrame == frameOf(ms(600)));
    CHECK(r.captions[0].windows == std::vector<int>{0});
    CHECK(r.captions[1].text == "Second one\non two lines");
    CHECK(r.captions[1].startFrame == frameOf(ms(600)));
    CHECK(r.captions[1].windows == std::vector<int>{1});
    CHECK(r.captions[2].text == "Third \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x99\xAA"); // G2 quotes and the G0 music note
    CHECK(r.captions[2].windows == std::vector<int>{0});
    CHECK(r.captions[2].endFrame == frameOf(ms(1400))); // hidden at its end: a gap follows
    CHECK(r.captions[3].text == "After a gap");
    CHECK(r.captions[3].startFrame == frameOf(ms(2000)));
    CHECK(r.captions[3].endFrame == frameOf(ms(2500)));

    // The 608 fallback: exactly the SCC writer's CC1 pairs, as field 1 triplets on the same frames.
    interchange::CaptionWriteOptions w;
    const auto expected = interchange::cea608ForVideo(cues, range, rate, w);
    std::vector<std::pair<std::int64_t, std::array<std::uint8_t, 2>>> want;
    std::vector<std::pair<std::int64_t, std::array<std::uint8_t, 2>>> got;
    for (const auto& [frame, list] : expected) {
        for (const auto& p : list) {
            if (!(p[0] == 0x80 && p[1] == 0x80)) {
                want.push_back({frame, p});
            }
        }
    }
    for (std::int64_t k = 0; k < frames; ++k) {
        for (std::size_t i = 0; i < perFrame[static_cast<std::size_t>(k)].size(); i += 3) {
            const auto* t = &perFrame[static_cast<std::size_t>(k)][i];
            if (t[0] == 0xFC && !(t[1] == 0x80 && t[2] == 0x80)) {
                got.push_back({k, {t[1], t[2]}});
            }
        }
    }
    REQUIRE_FALSE(want.empty());
    CHECK(got == want);
    // FFmpeg's 608 decoder reads every caption back.
    const auto texts = decode608(perFrame, rate, 1);
    INFO(joined(texts));
    CHECK(anyContains(texts, "First caption"));
    CHECK(anyContains(texts, "After a gap"));
}

TEST_CASE("Closed captions: 708 roll-up and paint-on CC3 on field 2 an extended service and late captions",
          "[export][captions][mxf]") {
    const std::vector<CaptionCue> cues{{ms(100), ms(900), "Rolling up"}, {ms(1000), ms(900), "Second line"}};
    const TimeRange range{Ticks{}, Ticks::fromSeconds(2)};
    const auto read = [&](const ClosedCaptionData& cc, int service) {
        std::vector<std::vector<std::uint8_t>> perFrame;
        for (std::int64_t k = 0; k < cc.frames; ++k) {
            perFrame.emplace_back(cc.frame(k).begin(), cc.frame(k).end());
        }
        return std::pair{readCea708(perFrame, service), perFrame};
    };
    SECTION("roll-up 3 rows, service 9 (extended header), 608 on CC3") {
        ClosedCaptionOptions o;
        o.display = 2; // roll-up 3
        o.service708 = 9;
        o.channel608 = 3;
        const ClosedCaptionData cc = buildClosedCaptions(cues, range, kRate25, 50, o);
        const auto [r, perFrame] = read(cc, 9);
        INFO(describe(r));
        CHECK(r.errors == 0);
        CHECK(r.services == std::vector<int>{9});
        // Roll-up: the line arrives at the cue's start, the next one scrolls it up and stays under it.
        CHECK(std::any_of(r.captions.begin(), r.captions.end(), [](const Cea708Caption& c) { return c.text == "Rolling up"; }));
        CHECK(std::any_of(r.captions.begin(), r.captions.end(),
                          [](const Cea708Caption& c) { return c.text == "Rolling up\nSecond line"; }));
        REQUIRE_FALSE(r.captions.empty());
        CHECK(r.captions.back().endFrame == *ticksToFrames(ms(1900), kRate25, Rounding::Nearest)); // cleared at the end
        // CC3 is field 2: FFmpeg's decoder finds it there, not on field 1.
        const auto field2 = decode608(perFrame, kRate25, 2);
        INFO(joined(field2));
        CHECK(anyContains(field2, "Rolling"));
        CHECK_FALSE(anyContains(decode608(perFrame, kRate25, 1), "Rolling"));
    }
    SECTION("paint-on at 59.94") {
        ClosedCaptionOptions o;
        o.display = 4;
        const ClosedCaptionData cc = buildClosedCaptions(cues, range, kRate59_94, 120, o);
        CHECK(cc.ccCount == 10);
        const auto [r, perFrame] = read(cc, 1);
        INFO(describe(r));
        CHECK(r.errors == 0);
        CHECK(std::any_of(r.captions.begin(), r.captions.end(), [](const Cea708Caption& c) { return c.text == "Rolling up"; }));
        CHECK(std::any_of(r.captions.begin(), r.captions.end(), [](const Cea708Caption& c) { return c.text == "Second line"; }));
    }
    SECTION("a long caption at the first frame is shown late, and said") {
        const std::vector<CaptionCue> longCue{
            {Ticks{}, Ticks::fromSeconds(2),
             "A caption long enough to need four full rows of\nthirty-two columns, so that loading it takes\nmore "
             "than one frame of the bandwidth that\n708 has at fifty-nine point nine four"}};
        const ClosedCaptionData cc = buildClosedCaptions(longCue, range, kRate59_94, 120, ClosedCaptionOptions{});
        CHECK(cc.late708 == 1);
        CHECK(cc.maxLateFrames > 0);
        CHECK(std::any_of(cc.notes.begin(), cc.notes.end(), [](const std::string& n) { return n.find("late") != std::string::npos; }));
        const auto [r, perFrame] = read(cc, 1);
        INFO(describe(r));
        REQUIRE(r.captions.size() == 1);
        CHECK(r.captions[0].text.find("thirty-two columns") != std::string::npos);
    }
}

// Fix round 1 (ARGON-05): an empty or blank cue right after a caption used to keep that caption's window busy to the end
// of the export, and every later caption was dropped; pop-on loads went out as early as the window was free (minutes
// before, for a decoder that starts or seeks later); after a gap a roll-up line did not go back to the bottom row.
TEST_CASE("Closed captions: 708 drops nothing after an empty or blank cue and loads each caption near its start",
          "[export][captions][mxf]") {
    const std::vector<CaptionCue> cues{{ms(200), ms(400), "One"},
                                       {ms(600), ms(400), ""}, // right after "One": shows nothing
                                       {ms(1000), ms(400), "Two"},
                                       {ms(1400), ms(400), "  \n "}, // blank, right after "Two"
                                       {ms(1800), ms(400), "Three"},
                                       {Ticks::fromSeconds(8), ms(500), "Much later"}};
    const TimeRange range{Ticks{}, Ticks::fromSeconds(10)};
    const Rate rate = kRate29_97;
    const std::int64_t frames = 300;
    ClosedCaptionOptions o;
    o.service708 = 1;
    const ClosedCaptionData cc = buildClosedCaptions(cues, range, rate, frames, o);
    CHECK(cc.late708 == 0);
    CHECK(std::none_of(cc.notes.begin(), cc.notes.end(),
                       [](const std::string& n) { return n.find("did not fit") != std::string::npos; }));
    std::vector<std::vector<std::uint8_t>> perFrame;
    for (std::int64_t k = 0; k < frames; ++k) {
        perFrame.emplace_back(cc.frame(k).begin(), cc.frame(k).end());
    }
    const Cea708Reading r = readCea708(perFrame, 1);
    INFO(describe(r));
    CHECK(r.errors == 0);
    REQUIRE(r.captions.size() == 4);
    const auto frameOf = [&](Ticks t) { return *ticksToFrames(t, rate, Rounding::Nearest); };
    CHECK(r.captions[0].text == "One");
    CHECK(r.captions[0].endFrame == frameOf(ms(600))); // hidden at its end: the empty cue shows nothing
    CHECK(r.captions[1].text == "Two");
    CHECK(r.captions[1].startFrame == frameOf(ms(1000)));
    CHECK(r.captions[1].endFrame == frameOf(ms(1400)));
    CHECK(r.captions[2].text == "Three");
    CHECK(r.captions[2].startFrame == frameOf(ms(1800)));
    CHECK(r.captions[3].text == "Much later");
    CHECK(r.captions[3].startFrame == frameOf(Ticks::fromSeconds(8)));

    // "Much later" is loaded at most 2 seconds before it shows: after "Three" is hidden no 708 byte goes out until then.
    const auto has708 = [&](std::int64_t k) {
        const auto& f = perFrame[static_cast<std::size_t>(k)];
        for (std::size_t i = 0; i + 2 < f.size(); i += 3) {
            if (f[i] == 0xFF || f[i] == 0xFE) {
                return true;
            }
        }
        return false;
    };
    const std::int64_t earliest = frameOf(Ticks::fromSeconds(8)) - static_cast<std::int64_t>(std::ceil(rate.toDouble() * 2.0));
    for (std::int64_t k = frameOf(ms(2200)) + 1; k < earliest; ++k) {
        INFO("frame " << k);
        CHECK_FALSE(has708(k));
    }
    bool loaded = false;
    for (std::int64_t k = earliest; k < frameOf(Ticks::fromSeconds(8)); ++k) {
        loaded = loaded || has708(k);
    }
    CHECK(loaded);
}

TEST_CASE("Closed captions: 708 roll-up after a gap writes on the bottom row again", "[export][captions][mxf]") {
    const std::vector<CaptionCue> cues{{ms(100), ms(400), "Line one"}, {ms(1000), ms(400), "Line two"}};
    const TimeRange range{Ticks{}, Ticks::fromSeconds(2)};
    ClosedCaptionOptions o;
    o.display = 2; // roll-up 3: the bottom row is row 2
    const ClosedCaptionData cc = buildClosedCaptions(cues, range, kRate25, 50, o);
    // The 708 bytes from the gap's ClearWindows on (DTVCC packet bytes of each frame in order).
    const std::int64_t gap = *ticksToFrames(ms(500), kRate25, Rounding::Nearest);
    std::vector<std::uint8_t> after;
    for (std::int64_t k = gap; k < cc.frames; ++k) {
        const auto f = cc.frame(k);
        for (std::size_t i = 0; i + 2 < f.size(); i += 3) {
            if (f[i] == 0xFF || f[i] == 0xFE) {
                after.push_back(f[i + 1]);
                after.push_back(f[i + 2]);
            }
        }
    }
    const std::vector<std::uint8_t> bottomRow{0x92, 0x02, 0x00}; // SetPenLocation row 2, column 0
    CHECK(std::search(after.begin(), after.end(), bottomRow.begin(), bottomRow.end()) != after.end());
    std::vector<std::vector<std::uint8_t>> perFrame;
    for (std::int64_t k = 0; k < cc.frames; ++k) {
        perFrame.emplace_back(cc.frame(k).begin(), cc.frame(k).end());
    }
    const Cea708Reading r = readCea708(perFrame, 1);
    INFO(describe(r));
    CHECK(r.errors == 0);
    CHECK(std::any_of(r.captions.begin(), r.captions.end(), [](const Cea708Caption& c) { return c.text == "Line two"; }));
}

// ---- MXF -----------------------------------------------------------------------------------------------------------

TEST_CASE("MXF OP1a XDCAM HD422 with 8 mono PCM tracks timecode reel UMID and an SMPTE 436M caption track",
          "[export][mxf]") {
    TestProject project = captioned(1280, 720, kRate50, Ticks::fromSeconds(2));
    const bool withAudio = haveMedia("sine_48k.wav");
    if (withAudio) {
        const MediaId sine = project.addMedia("sine_48k.wav");
        project.addAudioTrack(sine, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    }
    ExportSettings s = preset("mxf-op1a-xdcam-hd422");
    s.video.width = 1280;
    s.video.height = 720;
    s.audio.channels = 8;
    s.mxf.reelName = "A001";
    s.captions.mode = CaptionExport::Embed;
    const auto dir = freshOutputDir("mxf-op1a-xdcam");
    const fs::path out = dir / "delivery.mxf";
    const ExportResult result = run(makeRequest(project, s, out));
    CHECK(result.operationalPattern == "OP1a");
    CHECK(result.startTimecode == "01:00:00:00");
    CHECK(result.audioLayout.find("8 mono tracks") != std::string::npos);
    CHECK(result.audioLayout.find("3-8 silent") != std::string::npos);
    CHECK(result.embeddedCaptions == "CEA-608 CC1 + CEA-708 service 1 in an SMPTE 436M track");
    CHECK(result.embeddedCaptionCues == 2);
    CHECK(result.videoEncoder == "mpeg2video");
    CHECK(partialFiles(dir).empty());

    std::string error;
    const auto probe = ffprobe(out, &error);
    INFO(error);
    REQUIRE(probe.has_value());
    CHECK(tag(probe->format, "format_name") == "mxf");
    // OP1a: SMPTE 378M's operational pattern UL (item complexity 01, package complexity 01).
    CHECK(tag(probe->format, "TAG:operational_pattern_ul").starts_with("060e2b34.04010101.0d010201.0101"));
    CHECK(tag(probe->format, "TAG:timecode") == "01:00:00:00");
    CHECK(tag(probe->format, "TAG:material_package_name") == project.seq().name);
    CHECK(tag(probe->format, "TAG:company_name") == "Wabcut");
    const std::string umid = tag(probe->format, "TAG:material_package_umid");
    CHECK(umid.starts_with("0x060A2B34"));
    CHECK(result.materialUmid == umid);
    const auto* video = probe->stream("video");
    REQUIRE(video != nullptr);
    CHECK(tag(*video, "codec_name") == "mpeg2video");
    CHECK(tag(*video, "profile") == "4:2:2");
    CHECK(tag(*video, "width") == "1280");
    CHECK(tag(*video, "r_frame_rate") == "50/1");
    CHECK(tag(*video, "TAG:reel_name") == "A001");
    CHECK(countType(*probe, "audio") == 8);
    for (const auto& st : probe->streams) {
        if (tag(st, "codec_type") == "audio") {
            CHECK(tag(st, "codec_name") == "pcm_s24le");
            CHECK(tag(st, "channels") == "1");
            CHECK(tag(st, "sample_rate") == "48000");
        }
    }
    CHECK(countType(*probe, "data") == 1);
    CHECK(tag(probe->streams.back(), "codec_name") == "smpte_436m_anc");

    // Track 1 carries the mix, track 3 is silent.
    if (withAudio) {
        AvInput in;
        REQUIRE(avformat_open_input(&in.ctx, out.string().c_str(), nullptr, nullptr) == 0);
        REQUIRE(avformat_find_stream_info(in.ctx, nullptr) >= 0);
        std::vector<std::vector<std::vector<std::uint8_t>>> tracks(in.ctx->nb_streams);
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(in.ctx, packet) >= 0) {
            tracks[static_cast<std::size_t>(packet->stream_index)].emplace_back(packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        CHECK(peak24(tracks[1]) > 100'000);
        CHECK(peak24(tracks[3]) == 0);
        CHECK(peak24(tracks[8]) == 0);
    }

    // The 436M track: one CDP (ANC DID 61h / SDID 01h, CDP frame rate 50 = 6Fh) per frame, cc_count 12.
    const auto anc = packetsOf(out, AV_CODEC_ID_SMPTE_436M_ANC);
    CHECK(static_cast<std::int64_t>(anc.size()) == countVideoPackets(out));
    REQUIRE_FALSE(anc.empty());
    AVSmpte436mAncIterator iter;
    REQUIRE(av_smpte_436m_anc_iter_init(&iter, anc.front().data(), static_cast<int>(anc.front().size())) == 0);
    AVSmpte436mCodedAnc coded;
    REQUIRE(av_smpte_436m_anc_iter_next(&iter, &coded) == 0);
    AVSmpte291mAnc8bit packet291;
    REQUIRE(av_smpte_291m_anc_8bit_decode(&packet291, coded.payload_sample_coding, coded.payload_sample_count, coded.payload,
                                          nullptr) == 0);
    CHECK(packet291.did == 0x61);
    CHECK(packet291.sdid_or_dbn == 0x01);
    CHECK(packet291.payload[0] == 0x96); // cdp_identifier
    CHECK(packet291.payload[1] == 0x69);
    CHECK(packet291.payload[3] == 0x6F); // cdp_frame_rate: 50 fps

    // The captions come back: 608 through FFmpeg's decoder, 708 through readCea708, on the frames of the cues.
    const auto cc = packetsOf(out, AV_CODEC_ID_EIA_608, true);
    REQUIRE_FALSE(cc.empty());
    CHECK(cc.front().size() == 36); // cc_count 12
    const auto texts = decode608(cc, kRate50, 1);
    INFO(joined(texts));
    CHECK(anyContains(texts, "Hello"));
    CHECK(anyContains(texts, "World"));
    const Cea708Reading r = readCea708(cc, 1);
    INFO(describe(r));
    CHECK(r.errors == 0);
    REQUIRE(r.captions.size() == 2);
    CHECK(r.captions[0].text == "Hello");
    CHECK(r.captions[0].startFrame == 10);
    CHECK(r.captions[0].endFrame == 40);
    CHECK(r.captions[1].text == "World");
    CHECK(r.captions[1].startFrame == 50);
    CHECK(r.captions[1].endFrame == 80);
}

TEST_CASE("MXF OP1a DNxHD at 23.976 25 29.97 50 and 59.94 fps with timecode and 436M captions at each rate",
          "[export][mxf]") {
    struct Case {
        Rate rate;
        const char* fraction;
        std::uint8_t cdpRate;
        const char* timecode;
    };
    const Case cases[] = {{kRate23_976, "24000/1001", 0x1F, "01:00:00:00"},
                          {kRate25, "25/1", 0x3F, "01:00:00:00"},
                          {kRate29_97, "30000/1001", 0x4F, "01:00:00;00"},
                          {kRate50, "50/1", 0x6F, "01:00:00:00"},
                          {kRate59_94, "60000/1001", 0x7F, "01:00:00;00"}};
    const auto dir = freshOutputDir("mxf-rates");
    for (const Case& c : cases) {
        INFO(c.fraction);
        const char* timecode = c.timecode;
        TestProject project = captioned(1920, 1080, c.rate, ms(800), {{ms(100), ms(500), "Rate check"}});
        ExportSettings s = preset("mxf-op1a-dnxhd-hq");
        s.audio.channels = 2;
        s.captions.mode = CaptionExport::Embed;
        std::string name = c.fraction;
        std::replace(name.begin(), name.end(), '/', '-');
        const fs::path out = dir / ("dnxhd-" + name + ".mxf");
        const ExportResult result = run(makeRequest(project, s, out));
        CHECK(result.startTimecode == timecode);
        const auto probe = ffprobe(out);
        REQUIRE(probe.has_value());
        const auto* video = probe->stream("video");
        REQUIRE(video != nullptr);
        CHECK(tag(*video, "codec_name") == "dnxhd");
        CHECK(tag(*video, "r_frame_rate") == c.fraction);
        CHECK(tag(probe->format, "TAG:timecode") == timecode);
        CHECK(countType(*probe, "audio") == 2);
        CHECK(countType(*probe, "data") == 1);
        const auto anc = packetsOf(out, AV_CODEC_ID_SMPTE_436M_ANC);
        REQUIRE_FALSE(anc.empty());
        AVSmpte436mAncIterator iter;
        REQUIRE(av_smpte_436m_anc_iter_init(&iter, anc.front().data(), static_cast<int>(anc.front().size())) == 0);
        AVSmpte436mCodedAnc coded;
        REQUIRE(av_smpte_436m_anc_iter_next(&iter, &coded) == 0);
        AVSmpte291mAnc8bit packet291;
        REQUIRE(av_smpte_291m_anc_8bit_decode(&packet291, coded.payload_sample_coding, coded.payload_sample_count,
                                              coded.payload, nullptr) == 0);
        CHECK(packet291.payload[3] == c.cdpRate);
        const auto cc = packetsOf(out, AV_CODEC_ID_EIA_608, true);
        REQUIRE_FALSE(cc.empty());
        CHECK(static_cast<int>(cc.front().size()) == 3 * ccCountForRate(c.rate));
        const Cea708Reading r = readCea708(cc, 1);
        INFO(describe(r));
        REQUIRE(r.captions.size() == 1);
        CHECK(r.captions[0].text == "Rate check");
        CHECK(r.captions[0].startFrame == *ticksToFrames(ms(100), c.rate, Rounding::Nearest));
        const auto texts = decode608(cc, c.rate, 1);
        INFO(joined(texts));
        CHECK(anyContains(texts, "Rate check"));
    }
}

TEST_CASE("MXF OP-Atom writes a DNxHR video file and one mono PCM file per channel", "[export][mxf]") {
    TestProject project(1280, 720, kRate25, Ticks::fromSeconds(1));
    ExportSettings s = preset("mxf-opatom-dnxhd-hq");
    s.video.dnxhrProfile = DnxhrProfile::Lb; // DNxHR at 1280x720 (DNxHD is 1080 only)
    s.video.width.reset();
    s.video.height.reset();
    s.audio.channels = 4;
    s.mxf.reelName = "TAPE7";
    const auto dir = freshOutputDir("mxf-opatom");
    const fs::path out = dir / "Avid clip.mxf";
    const ExportResult result = run(makeRequest(project, s, out));
    CHECK(result.operationalPattern == "OP-Atom");
    CHECK(result.output == fs::absolute(out));
    REQUIRE(result.extraFiles.size() == 4);
    CHECK(result.extraFiles[0].filename() == "Avid clip_A1.mxf");
    CHECK(result.extraFiles[3].filename() == "Avid clip_A4.mxf");
    CHECK(result.audioLayout.find("4 mono audio files") != std::string::npos);
    CHECK(partialFiles(dir).empty());
    const auto video = ffprobe(out);
    REQUIRE(video.has_value());
    REQUIRE(video->streams.size() == 1);
    CHECK(tag(video->streams[0], "codec_name") == "dnxhd");
    // OP-Atom: SMPTE 390M's operational pattern UL.
    CHECK(tag(video->format, "TAG:operational_pattern_ul").starts_with("060e2b34.04010102.0d010201.1003"));
    CHECK(tag(video->format, "TAG:timecode") == "00:00:00:00"); // the sequence starts at 0
    CHECK(tag(video->streams[0], "TAG:reel_name") == "TAPE7");
    CHECK(result.materialUmid == tag(video->format, "TAG:material_package_umid"));
    for (const fs::path& file : result.extraFiles) {
        INFO(file.string());
        const auto audio = ffprobe(file);
        REQUIRE(audio.has_value());
        REQUIRE(audio->streams.size() == 1);
        CHECK(tag(audio->streams[0], "codec_name") == "pcm_s24le");
        CHECK(tag(audio->streams[0], "channels") == "1");
        CHECK(tag(audio->format, "TAG:operational_pattern_ul").starts_with("060e2b34.04010102.0d010201.1003"));
        CHECK(tag(audio->format, "TAG:timecode") == "00:00:00:00");
        CHECK(tag(audio->streams[0], "TAG:reel_name") == "TAPE7");
        // Fix round 1 (ARGON-05): FFmpeg's mxf_opatom gives every file a material package of its own (a random UMID,
        // no option to share one): separate clips in Media Composer, as docs/export.md says. Should a newer FFmpeg
        // share it, this check fails and the docs, the preset text and the job details can say "one clip".
        const std::string umid = tag(audio->format, "TAG:material_package_umid");
        CHECK(umid.starts_with("0x060A2B34"));
        CHECK(umid != result.materialUmid);
    }
}

TEST_CASE("MXF OP1a AVC-Intra 100 at 720p50 with one multichannel PCM track", "[export][mxf]") {
    TestProject project(1280, 720, kRate50, ms(600));
    ExportSettings s = preset("mxf-op1a-avc-intra-100");
    s.video.width = 1280;
    s.video.height = 720;
    s.audio.channels = 4;
    s.audio.codec = AudioCodec::Pcm16;
    s.mxf.audioLayout = MxfAudioLayout::OneTrack;
    const auto dir = freshOutputDir("mxf-avc-intra");
    const fs::path out = dir / "avci.mxf";
    const ExportResult result = run(makeRequest(project, s, out));
    CHECK(result.audioLayout.find("one 4-channel track") != std::string::npos);
    const auto probe = ffprobe(out);
    REQUIRE(probe.has_value());
    const auto* video = probe->stream("video");
    REQUIRE(video != nullptr);
    CHECK(tag(*video, "codec_name") == "h264");
    CHECK(tag(*video, "profile") == "High 4:2:2 Intra");
    CHECK(tag(*video, "pix_fmt") == "yuv422p10le");
    REQUIRE(countType(*probe, "audio") == 1);
    const auto* audio = probe->stream("audio");
    CHECK(tag(*audio, "codec_name") == "pcm_s16le");
    CHECK(tag(*audio, "channels") == "4");
}

TEST_CASE("Embedded 708 rides in H.264 caption SEI and 608 in a QuickTime c608 track", "[export][captions][mxf]") {
    // 29.97: a c608 sample a frame; 59.94: a 608 pair every other frame, each sample two frames long.
    const Rate rate = GENERATE(kRate29_97, kRate59_94);
    INFO(rate.toDouble());
    const std::int64_t frames = rate == kRate29_97 ? 60 : 120;
    const TestProject project = captioned(320, 180, rate, Ticks::fromSeconds(2));
    ExportSettings s = preset("match-source");
    s.container = Container::Mov;
    s.video.encoder = "libx264";
    s.audio.enabled = false;
    s.captions.mode = CaptionExport::Embed;
    s.captions.carriage = CaptionCarriage::Both;
    s.captions.service708 = 2;
    const auto dir = freshOutputDir(rate == kRate29_97 ? "mov-captions-2997" : "mov-captions-5994");
    const fs::path out = dir / "captions.mov";
    const ExportResult result = run(makeRequest(project, s, out));
    CHECK(result.embeddedCaptions == "CEA-608 CC1 + CEA-708 service 2 in the H.264 stream and a QuickTime c608 track");
    const auto probe = ffprobe(out);
    REQUIRE(probe.has_value());
    bool c608 = false;
    for (const auto& st : probe->streams) {
        c608 = c608 || (tag(st, "codec_name") == "eia_608" && tag(st, "codec_tag_string") == "c608");
    }
    CHECK(c608);
    // 708 service 2 from the H.264 frames' caption SEI.
    const auto sei = pictureCaptionData(out);
    REQUIRE(static_cast<std::int64_t>(sei.size()) == frames);
    CHECK(static_cast<int>(sei.front().size()) == 3 * ccCountForRate(rate)); // 20 at 29.97, 10 at 59.94
    const Cea708Reading r = readCea708(sei, 2);
    INFO(describe(r));
    REQUIRE(r.captions.size() == 2);
    CHECK(r.captions[0].text == "Hello");
    CHECK(r.captions[1].text == "World");
    CHECK(r.services == std::vector<int>{2});
    // 608 from the c608 track, through FFmpeg's decoder.
    const auto track = packetsOf(out, AV_CODEC_ID_EIA_608);
    REQUIRE_FALSE(track.empty());
    CHECK(static_cast<std::int64_t>(track.size()) == (rate == kRate29_97 ? frames : frames / 2));
    const auto texts = decode608(track, rate, 1);
    INFO(joined(texts));
    CHECK(anyContains(texts, "Hello"));
    CHECK(anyContains(texts, "World"));
}

TEST_CASE("Embedded captions: formats that cannot carry them are refused with the reason", "[export][captions][mxf]") {
    const auto refused = [](ExportSettings s, std::string_view words) {
        s.captions.mode = s.captions.mode == CaptionExport::None ? CaptionExport::Embed : s.captions.mode;
        const auto ok = validate(s);
        INFO(std::string(words));
        REQUIRE_FALSE(ok.has_value());
        CHECK(ok.error().code == ErrorCode::InvalidArgument);
        INFO(ok.error().message);
        CHECK(ok.error().message.find(words) != std::string::npos);
    };
    ExportSettings prores = preset("prores-422-hq"); // c608 holds 608 only
    refused(prores, "cannot carry CEA-708");
    prores.captions.standard = CaptionStandard::Cea608;
    prores.captions.mode = CaptionExport::Embed;
    CHECK(validate(prores).has_value());
    prores.captions.channel608 = 3;
    refused(prores, "CC1 or CC2");
    ExportSettings av1 = preset("youtube-1440p60-av1");
    refused(av1, "cannot carry captions in its stream");
    refused(preset("mxf-opatom-dnxhd-hq"), "OP-Atom");
    ExportSettings mp4Track = preset("match-source");
    mp4Track.captions.carriage = CaptionCarriage::CaptionTrack;
    refused(mp4Track, "has no caption track");
    ExportSettings avci = preset("mxf-op1a-avc-intra-100");
    avci.captions.carriage = CaptionCarriage::VideoStream;
    refused(avci, "AVC-Intra 100 video cannot carry captions in its stream");
    avci.captions.carriage = CaptionCarriage::Auto; // the 436M track is fine
    avci.captions.mode = CaptionExport::Embed;
    CHECK(validate(avci).has_value());

    // MXF's own rules.
    const auto invalid = [](const ExportSettings& s, std::string_view words) {
        const auto ok = validate(s);
        INFO(std::string(words));
        REQUIRE_FALSE(ok.has_value());
        INFO(ok.error().message);
        CHECK(ok.error().message.find(words) != std::string::npos);
    };
    ExportSettings h264Mxf = preset("mxf-op1a-dnxhr-hq");
    h264Mxf.video.codec = VideoCodec::H264;
    h264Mxf.video.pixelFormat = OutputPixelFormat::Yuv420p;
    invalid(h264Mxf, "MXF OP1a holds");
    ExportSettings aac = preset("mxf-op1a-dnxhr-hq");
    aac.audio.codec = AudioCodec::Aac;
    invalid(aac, "16 or 24-bit PCM at 48 kHz");
    ExportSettings xdcamMp4 = preset("mxf-op1a-xdcam-hd422");
    xdcamMp4.container = Container::Mkv;
    invalid(xdcamMp4, "go into MXF or QuickTime");
    // Rates and rasters, once the sequence is known.
    Sequence seq;
    seq.width = 1920;
    seq.height = 1080;
    seq.frameRate = Rate{15, 1};
    seq.duration = Ticks::fromSeconds(1);
    const auto resolved = [&](const ExportSettings& s, std::string_view words) {
        const auto r = resolve(s, seq);
        INFO(std::string(words));
        REQUIRE_FALSE(r.has_value());
        INFO(r.error().message);
        CHECK(r.error().message.find(words) != std::string::npos);
    };
    resolved(preset("mxf-op1a-dnxhr-hq"), "MXF is written at");
    seq.frameRate = kRate50;
    resolved(preset("mxf-op1a-xdcam-hd422"), "XDCAM HD422 is 1920");
    seq.frameRate = kRate23_976;
    resolved(preset("mxf-op1a-avc-intra-100"), "AVC-Intra 100 is 1920");
    seq.frameRate = kRate25;
    CHECK(resolve(preset("mxf-op1a-xdcam-hd422"), seq).has_value());
    CHECK(resolve(preset("mxf-op1a-avc-intra-100"), seq).has_value());
}

TEST_CASE("Embedded captions: a missing or empty caption track is a warning not a silent success", "[export][captions][mxf]") {
    const auto dir = freshOutputDir("mxf-no-captions");
    ExportSettings s = preset("mxf-op1a-dnxhr-hq");
    s.audio.enabled = false;
    s.captions.mode = CaptionExport::Embed;
    {
        TestProject project(640, 360, kRate25, ms(400));
        const ExportResult result = run(makeRequest(project, s, dir / "none.mxf"));
        CHECK(std::any_of(result.warnings.begin(), result.warnings.end(),
                          [](const std::string& w) { return w.find("no caption track") != std::string::npos; }));
        CHECK(result.embeddedCaptions.empty());
        const auto probe = ffprobe(dir / "none.mxf");
        REQUIRE(probe.has_value());
        CHECK(countType(*probe, "data") == 0);
    }
    {
        // Captions only after the range ends.
        TestProject project = captioned(640, 360, kRate25, Ticks::fromSeconds(3), {{Ticks::fromSeconds(2), ms(500), "Later"}});
        s.range = RangeMode::Custom;
        s.customRange = TimeRange{Ticks{}, Ticks::fromSeconds(1)};
        const ExportResult result = run(makeRequest(project, s, dir / "empty.mxf"));
        CHECK(std::any_of(result.warnings.begin(), result.warnings.end(),
                          [](const std::string& w) { return w.find("no captions in the export range") != std::string::npos; }));
        CHECK(result.embeddedCaptionCues == 0);
    }
}

TEST_CASE("MXF and embedded caption settings round trip as JSON and through the override keys", "[export][settings][mxf]") {
    ExportSettings s = preset("mxf-op1a-xdcam-hd422");
    s.mxf.reelName = "A001";
    s.mxf.audioLayout = MxfAudioLayout::OneTrack;
    s.captions.mode = CaptionExport::Embed;
    s.captions.standard = CaptionStandard::Cea708;
    s.captions.service708 = 12;
    s.captions.carriage = CaptionCarriage::Both;
    s.captions.display608 = 2;
    const Json json = settingsToJson(s);
    CHECK(json["mxf"]["reelName"] == "A001");
    CHECK(json["captions"]["standard"] == "708");
    const auto back = settingsFromJson(json);
    REQUIRE(back.has_value());
    CHECK(back->mxf == s.mxf);
    CHECK(back->captions.standard == CaptionStandard::Cea708);
    CHECK(back->captions.service708 == 12);
    CHECK(back->captions.carriage == CaptionCarriage::Both);
    CHECK(back->captions.display608 == 2);
    CHECK(settingsToJson(*back) == json);
    // A preset without these keys stays as it was (no "mxf" section outside MXF).
    CHECK_FALSE(settingsToJson(preset("match-source")).contains("mxf"));

    // Override keys: a container change brings MXF's codec and PCM; the caption choices.
    ExportSettings o = preset("match-source");
    REQUIRE(applyOverrides(o, Json{{"container", "mxf"}, {"captions", "embed"}, {"captionStandard", "608"},
                                   {"cc608Channel", 3}, {"cc708Service", 7}, {"captionCarriage", "track"},
                                   {"captionDisplay", "roll-up-2"}, {"reelName", "R2"}, {"mxfAudioLayout", "multichannel"},
                                   {"audioChannels", 8}})
                .has_value());
    CHECK(o.container == Container::Mxf);
    CHECK(o.video.codec == VideoCodec::DnxHR);
    CHECK(o.audio.codec == AudioCodec::Pcm24);
    CHECK(o.captions.standard == CaptionStandard::Cea608);
    CHECK(o.captions.channel608 == 3);
    CHECK(o.captions.service708 == 7);
    CHECK(o.captions.carriage == CaptionCarriage::CaptionTrack);
    CHECK(o.captions.display608 == 1);
    CHECK(o.mxf.reelName == "R2");
    CHECK(o.mxf.audioLayout == MxfAudioLayout::OneTrack);
    CHECK(o.audio.channels == 8);
    CHECK(validate(o).has_value());
    REQUIRE(applyOverrides(o, Json{{"videoCodec", "xdcam-hd422"}}).has_value());
    CHECK(o.video.pixelFormat == OutputPixelFormat::Yuv422p);
    CHECK_FALSE(applyOverrides(o, Json{{"cc708Service", 64}}).has_value());
    CHECK_FALSE(applyOverrides(o, Json{{"captionStandard", "609"}}).has_value());
}
