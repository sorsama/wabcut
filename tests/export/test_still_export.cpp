// P.2 (E6): Export Frame's stills (StillExport.h): every format is written and reads back as the picture (FFmpeg's
// decoders for DPX, JPEG, TIFF, Targa, BMP and GIF, Qt for PNG, a reader that follows the OpenEXR spec for EXR), and
// sequence.exportFrame / source.exportFrame render through the export path and import the still into the project.

#include "ExportTestUtil.h"

#include <wabcut/command/Engine.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/export/StillExport.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QImage>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace wabcut;
using namespace exporttest;

namespace {

/// A test picture: four flat quadrants and a horizontal grey ramp across the bottom third.
QImage testPicture(int w = 96, int h = 72) {
    QImage image(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            QColor c;
            if (y >= 2 * h / 3) {
                const int v = x * 255 / (w - 1);
                c = QColor(v, v, v);
            } else if (x < w / 2) {
                c = y < h / 3 ? QColor(220, 30, 40) : QColor(20, 200, 60);
            } else {
                c = y < h / 3 ? QColor(30, 60, 230) : QColor(250, 240, 20);
            }
            image.setPixelColor(x, y, c);
        }
    }
    return image;
}

/// The first picture of `file`, decoded by FFmpeg and converted to RGBA (null when it cannot be read).
QImage decodeWithFfmpeg(const std::filesystem::path& file) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, pathToUtf8(file).c_str(), nullptr, nullptr) < 0) {
        return {};
    }
    QImage out;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        const int index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        const AVCodec* codec = index >= 0 ? avcodec_find_decoder(fmt->streams[index]->codecpar->codec_id) : nullptr;
        AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (ctx && avcodec_parameters_to_context(ctx, fmt->streams[index]->codecpar) >= 0 &&
            avcodec_open2(ctx, codec, nullptr) >= 0) {
            AVPacket* packet = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();
            bool got = false;
            while (!got && av_read_frame(fmt, packet) >= 0) {
                if (packet->stream_index == index && avcodec_send_packet(ctx, packet) >= 0) {
                    got = avcodec_receive_frame(ctx, frame) == 0;
                }
                av_packet_unref(packet);
            }
            if (!got && avcodec_send_packet(ctx, nullptr) >= 0) {
                got = avcodec_receive_frame(ctx, frame) == 0;
            }
            if (got) {
                SwsContext* sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                                 frame->width, frame->height, AV_PIX_FMT_RGBA,
                                                 SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr);
                if (sws) {
                    out = QImage(frame->width, frame->height, QImage::Format_RGBA8888);
                    std::uint8_t* dst[1] = {out.bits()};
                    const int stride[1] = {static_cast<int>(out.bytesPerLine())};
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, stride);
                    sws_freeContext(sws);
                }
            }
            av_frame_free(&frame);
            av_packet_free(&packet);
        }
        avcodec_free_context(&ctx);
    }
    avformat_close_input(&fmt);
    return out;
}

/// The largest difference of a colour channel between two pictures of one size, and the mean difference.
struct Difference {
    int max = 0;
    double mean = 0;
};
Difference differenceOf(const QImage& a, const QImage& b) {
    Difference d;
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const std::uint8_t* p = a.constScanLine(y);
        const std::uint8_t* q = b.constScanLine(y);
        for (int x = 0; x < a.width() * 4; ++x) {
            if (x % 4 == 3) {
                continue; // alpha: opaque everywhere
            }
            const int e = std::abs(static_cast<int>(p[x]) - static_cast<int>(q[x]));
            d.max = std::max(d.max, e);
            sum += e;
        }
    }
    d.mean = sum / (a.width() * a.height() * 3.0);
    return d;
}

/// The largest difference of a colour channel's mean over `block` x `block` tiles: what dithering keeps.
double blockDifferenceOf(const QImage& a, const QImage& b, int block) {
    double worst = 0;
    for (int by = 0; by + block <= a.height(); by += block) {
        for (int bx = 0; bx + block <= a.width(); bx += block) {
            for (int c = 0; c < 3; ++c) {
                double sa = 0;
                double sb = 0;
                for (int y = by; y < by + block; ++y) {
                    for (int x = bx; x < bx + block; ++x) {
                        sa += a.constScanLine(y)[x * 4 + c];
                        sb += b.constScanLine(y)[x * 4 + c];
                    }
                }
                worst = std::max(worst, std::abs(sa - sb) / (block * block));
            }
        }
    }
    return worst;
}

float fromHalf(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const int exponent = (h >> 10) & 0x1F;
    const std::uint32_t mantissa = h & 0x3FFu;
    if (exponent == 0) {
        const float v = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -v : v;
    }
    const std::uint32_t bits = sign | (static_cast<std::uint32_t>(exponent - 15 + 127) << 23) | (mantissa << 13);
    return std::bit_cast<float>(bits);
}

/// Reads an OpenEXR file the way the spec lays it out (single-part scanline, HALF channels, no compression).
struct ExrImage {
    int width = 0, height = 0;
    std::vector<std::string> channels;
    int compression = -1;
    std::map<std::string, std::vector<float>> planes;
};
std::optional<ExrImage> readExr(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t at = 0;
    const auto i32 = [&]() {
        std::int32_t v = 0;
        std::memcpy(&v, bytes.data() + at, 4);
        at += 4;
        return v;
    };
    const auto text = [&]() {
        std::string s(bytes.data() + at);
        at += s.size() + 1;
        return s;
    };
    if (bytes.size() < 8 || i32() != 20000630 || i32() != 2) {
        return std::nullopt;
    }
    ExrImage image;
    std::array<std::int32_t, 4> data{};
    while (bytes[at] != 0) {
        const std::string name = text();
        const std::string type = text();
        const std::int32_t size = i32();
        const std::size_t end = at + static_cast<std::size_t>(size);
        if (name == "channels" && type == "chlist") {
            while (bytes[at] != 0) {
                image.channels.push_back(text());
                if (i32() != 1) { // HALF
                    return std::nullopt;
                }
                at += 4;      // pLinear + reserved
                at += 8;      // sampling 1 x 1
            }
        } else if (name == "compression") {
            image.compression = bytes[at];
        } else if (name == "dataWindow") {
            for (std::int32_t& v : data) {
                v = i32();
            }
        }
        at = end;
    }
    ++at;
    image.width = data[2] - data[0] + 1;
    image.height = data[3] - data[1] + 1;
    std::vector<std::uint64_t> offsets(static_cast<std::size_t>(image.height));
    for (std::uint64_t& o : offsets) {
        std::memcpy(&o, bytes.data() + at, 8);
        at += 8;
    }
    for (const std::string& c : image.channels) {
        image.planes[c].resize(static_cast<std::size_t>(image.width * image.height));
    }
    for (int y = 0; y < image.height; ++y) {
        at = offsets[static_cast<std::size_t>(y)];
        if (i32() != y) {
            return std::nullopt;
        }
        (void)i32(); // the line's size
        for (const std::string& c : image.channels) {
            for (int x = 0; x < image.width; ++x) {
                std::uint16_t h = 0;
                std::memcpy(&h, bytes.data() + at, 2);
                at += 2;
                image.planes[c][static_cast<std::size_t>(y * image.width + x)] = fromHalf(h);
            }
        }
    }
    return image;
}

} // namespace

TEST_CASE("Export Frame writes every still format and each reads back as the picture", "[export][stills]") {
    const std::filesystem::path dir = freshOutputDir("stills");
    const QImage picture = testPicture();
    using exporter::StillFormat;
    struct Case {
        StillFormat format;
        int maxError;
        double meanError;
    };
    // Lossless: Targa, BMP, TIFF (LZW), PNG; DPX at 10 bits rounds back exactly; JPEG and GIF approximate. GIF's 252
    // colours are dithered: a pixel may be a palette step off (51 levels), the tile's average is kept (below).
    for (const Case c : {Case{StillFormat::Dpx, 1, 0.5}, Case{StillFormat::Jpeg, 40, 3.0}, Case{StillFormat::Tiff, 0, 0.0},
                         Case{StillFormat::Targa, 0, 0.0}, Case{StillFormat::Bmp, 0, 0.0}, Case{StillFormat::Gif, 96, 30.0},
                         Case{StillFormat::Png, 0, 0.0}}) {
        const std::string name(exporter::stillFormatName(c.format));
        INFO(name);
        const std::filesystem::path file = dir / ("frame" + std::string(exporter::stillExtension(c.format)));
        const auto written = exporter::writeStill(picture, file, c.format);
        INFO((written ? std::string() : written.error().message));
        REQUIRE(written);
        REQUIRE(std::filesystem::file_size(file) > 0);
        QImage back = c.format == StillFormat::Png ? QImage(QString::fromStdString(pathToUtf8(file))).convertToFormat(QImage::Format_RGBA8888)
                                                   : decodeWithFfmpeg(file);
        REQUIRE_FALSE(back.isNull());
        REQUIRE(back.width() == picture.width());
        REQUIRE(back.height() == picture.height());
        const Difference d = differenceOf(back, picture);
        CHECK(d.max <= c.maxError);
        CHECK(d.mean <= c.meanError);
        if (c.format == StillFormat::Gif) {
            // The test picture's regions are whole 6 x 6 tiles.
            const double tiles = blockDifferenceOf(back, picture, 6);
            INFO("tile mean difference " << tiles << ", pixel mean difference " << d.mean);
            CHECK(tiles <= 8.0);
        }
    }
    // Names and extensions follow Premiere's list.
    CHECK(exporter::stillFormats().size() == 8);
    CHECK(exporter::stillFormatFromName("openexr") == StillFormat::OpenExr);
    CHECK_FALSE(exporter::stillFormatFromName("webp"));
    CHECK_FALSE(exporter::writeStill(QImage(), dir / "none.png", StillFormat::Png));
}

TEST_CASE("Export Frame's OpenEXR is a spec-conformant half-float file in linear light", "[export][stills]") {
    const std::filesystem::path file = freshOutputDir("stills-exr") / "frame.exr";
    const QImage picture = testPicture(40, 30);
    REQUIRE(exporter::writeStill(picture, file, exporter::StillFormat::OpenExr));
    const auto exr = readExr(file);
    REQUIRE(exr);
    CHECK(exr->width == 40);
    CHECK(exr->height == 30);
    CHECK(exr->compression == 0);
    CHECK(exr->channels == std::vector<std::string>{"A", "B", "G", "R"});
    const auto linear = [](int v) {
        const double e = v / 255.0;
        return e < 0.081 ? e / 4.5 : std::pow((e + 0.099) / 1.099, 1.0 / 0.45);
    };
    double worst = 0;
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 40; ++x) {
            const QColor c = picture.pixelColor(x, y);
            const std::size_t i = static_cast<std::size_t>(y * 40 + x);
            worst = std::max(worst, std::abs(exr->planes.at("R")[i] - linear(c.red())));
            worst = std::max(worst, std::abs(exr->planes.at("G")[i] - linear(c.green())));
            worst = std::max(worst, std::abs(exr->planes.at("B")[i] - linear(c.blue())));
            CHECK(exr->planes.at("A")[i] == 1.0f);
        }
    }
    CHECK(worst < 1e-3); // half precision
}

TEST_CASE("sequence.exportFrame and source.exportFrame render as an export and import the still",
          "[export][stills][gpu]") {
    if (!haveTimelineMedia()) {
        SKIP("test media missing");
    }
    TestProject project = threeLayerTimeline(640, 360, kRate30, ms(1000));
    EditSession session(project.state);
    command::EnginePolicy policy;
    policy.services = exporter::stillExportServices(
        [](const std::filesystem::path& file) { return editor::describeMediaFile(file); });
    command::CommandEngine engine(session, command::builtinCommands(), policy);
    const std::filesystem::path dir = freshOutputDir("stills-commands");

    // The program frame at 0.5 s as a JPEG, imported into the project: one undo step, one more media item.
    const std::size_t media = session.snapshot()->media.size();
    command::CommandRequest program;
    program.command = "sequence.exportFrame";
    program.params = command::Json{{"time", ms(500).value}, {"format", "jpeg"}, {"path", pathToUtf8(dir / "program")},
                                   {"importIntoProject", true}, {"sequence", project.sequence.value}};
    const command::CommandResult r = engine.execute(program);
    INFO(command::toJson(r).dump());
    REQUIRE(r.ok());
    CHECK(r.committed);
    const std::filesystem::path written = pathFromUtf8(r.data.at("path").get<std::string>());
    CHECK(written.filename() == "program.jpg");
    CHECK(r.data.at("width") == 640);
    CHECK(r.data.at("height") == 360);
    REQUIRE(std::filesystem::exists(written));
    CHECK(session.snapshot()->media.size() == media + 1);
    CHECK(session.snapshot()->findMedia(MediaId{r.data.at("media").get<std::uint64_t>()})->info.isStillImage);
    // It is the frame an export shows there.
    const auto rendered = exporter::renderStill(*project.snapshot(), project.sequence, ms(500));
    REQUIRE(rendered);
    const QImage back = decodeWithFfmpeg(written);
    REQUIRE(back.size() == rendered->size());
    CHECK(differenceOf(back, *rendered).mean < 3.0);

    // A dry run writes nothing.
    program.dryRun = true;
    program.params["path"] = pathToUtf8(dir / "dry.png");
    program.params["format"] = "png";
    REQUIRE(engine.execute(program).ok());
    CHECK_FALSE(std::filesystem::exists(dir / "dry.png"));

    // The Source Monitor's frame of the V2 clip's media at its own size, as a TIFF.
    const MediaItem& prores = *std::find_if(session.snapshot()->media.begin(), session.snapshot()->media.end(),
                                            [](const MediaPtr& m) { return m->name.find("prores") != std::string::npos; })
                                   ->get();
    command::CommandRequest source;
    source.command = "source.exportFrame";
    source.params = command::Json{{"media", prores.id.value}, {"time", ms(200).value}, {"format", "tiff"},
                                  {"path", pathToUtf8(dir / "source.tif")}};
    const command::CommandResult s = engine.execute(source);
    INFO(command::toJson(s).dump());
    REQUIRE(s.ok());
    CHECK(s.data.at("width") == prores.info.video.front().width);
    CHECK(s.data.at("height") == prores.info.video.front().height);
    const QImage sourceFrame = decodeWithFfmpeg(dir / "source.tif");
    REQUIRE_FALSE(sourceFrame.isNull());
    bool varied = false;
    const QRgb first = sourceFrame.pixel(0, 0);
    for (int y = 0; y < sourceFrame.height() && !varied; y += 9) {
        for (int x = 0; x < sourceFrame.width() && !varied; x += 9) {
            varied = sourceFrame.pixel(x, y) != first;
        }
    }
    CHECK(varied); // a picture, not a blank

    // Refusals: no host renderer, a format the host cannot write, a bad bin.
    command::CommandEngine bare(session, command::builtinCommands());
    program.dryRun = false;
    CHECK(bare.execute(program).error->code == command::ErrorCode::InvalidRequest);
    program.params["format"] = "webp";
    CHECK(engine.execute(program).error->code == command::ErrorCode::InvalidParams);
    program.params["format"] = "png";
    program.params["bin"] = 999999;
    CHECK(engine.execute(program).error->code == command::ErrorCode::NotFound);
}
