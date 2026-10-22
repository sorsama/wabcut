// 0.2 media formats: layered Photoshop files (media/Psd.h, model/LayeredFile.h), spanned clips (model/SpannedClip.h),
// camera RAW (media/RawDevelop.h: CinemaDNG through FFmpeg's TIFF decoder) and interlaced footage's field order.

#include "PsdWriter.h"
#include "TestMedia.h"

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/Psd.h>
#include <wabcut/media/RawDevelop.h>
#include <wabcut/media/RawHelper.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/RawSourceSettings.h>
#include <wabcut/model/SpannedClip.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;
using Catch::Approx;

namespace {

using Rgba = std::array<std::uint16_t, 4>;

TestPsdLayer solid(std::string name, int left, int top, int w, int h, Rgba colour) {
    TestPsdLayer l;
    l.name = std::move(name);
    l.left = left;
    l.top = top;
    l.width = w;
    l.height = h;
    l.rgba = [colour](int, int) { return colour; };
    return l;
}

constexpr Rgba kRed{65535, 0, 0, 65535};
constexpr Rgba kBlue{0, 0, 65535, 65535};
constexpr Rgba kGrey{32768, 32768, 32768, 65535};

Rgba pixel(const PsdImage& image, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return {image.rgba[i], image.rgba[i + 1], image.rgba[i + 2], image.rgba[i + 3]};
}

bool closeTo(const Rgba& a, const Rgba& b, int tolerance = 300) { // not "near": a windows.h macro
    for (int c = 0; c < 4; ++c) {
        if (std::abs(static_cast<int>(a[static_cast<std::size_t>(c)]) - static_cast<int>(b[static_cast<std::size_t>(c)])) >
            tolerance) {
            return false;
        }
    }
    return true;
}

std::string describe(const Rgba& p) {
    return "(" + std::to_string(p[0]) + ", " + std::to_string(p[1]) + ", " + std::to_string(p[2]) + ", " +
           std::to_string(p[3]) + ")";
}

#define CHECK_PIXEL(image, x, y, expected)                                                                          \
    do {                                                                                                            \
        const Rgba got_ = pixel((image), (x), (y));                                                                 \
        INFO("pixel (" << (x) << ", " << (y) << ") " << describe(got_) << " expected " << describe(expected));     \
        CHECK(closeTo(got_, (expected)));                                                                              \
    } while (false)

/// A document of four layers: a grey background, a red square, a hidden blue square, a "Title" group with a blue
/// strip set to Multiply inside it.
std::vector<std::uint8_t> sampleDocument(int depth, TestPsdCompression compression, bool psb = false) {
    std::vector<TestPsdLayer> layers;
    layers.push_back(solid("Background", 0, 0, 64, 32, kGrey));
    layers.push_back(solid("Red square", 8, 8, 16, 16, kRed));
    TestPsdLayer hidden = solid("Hidden", 40, 0, 8, 8, kBlue);
    hidden.hidden = true;
    layers.push_back(hidden);
    TestPsdLayer end;
    end.name = "</Layer group>";
    end.section = 3;
    layers.push_back(end);
    TestPsdLayer strip = solid("Strip \xE0\xB8\x81", 0, 28, 64, 4, kBlue); // a Thai letter: the Unicode name
    strip.blend = "mul ";
    layers.push_back(strip);
    TestPsdLayer group;
    group.name = "Title";
    group.section = 1;
    layers.push_back(group);
    return writePsd(64, 32, depth, layers, compression, psb);
}

std::filesystem::path writeTemp(std::string_view dir, std::string_view name, const std::vector<std::uint8_t>& bytes) {
    const auto folder = scratchDir(dir);
    const auto file = folder / name;
    writeFile(file, bytes);
    return file;
}

} // namespace

TEST_CASE("PSD: layer records - names, bounds, visibility, blend modes and groups", "[media][psd]") {
    const auto bytes = sampleDocument(8, TestPsdCompression::Rle);
    auto doc = readPsdDocument(std::span<const std::uint8_t>(bytes));
    REQUIRE(doc.has_value());
    CHECK(doc->width == 64);
    CHECK(doc->height == 32);
    CHECK(doc->depth == 8);
    REQUIRE(doc->layers.size() == 6);
    CHECK(doc->layers[0].name == "Background");
    CHECK(doc->layers[1].left == 8);
    CHECK(doc->layers[1].right == 24);
    CHECK(doc->layers[1].hasPixels());
    CHECK_FALSE(doc->layers[2].visible);
    CHECK(doc->layers[3].group);                       // the group's end marker
    CHECK(doc->layers[4].name == "Strip \xE0\xB8\x81"); // the Unicode name wins over the Pascal one
    CHECK(doc->layers[4].blendMode == "mul ");
    REQUIRE(doc->layers[4].groups.size() == 1);
    CHECK(doc->layers[4].groups.front() == "Title");
    CHECK(doc->layers[5].group);
    CHECK(doc->layers[5].name == "Title");
}

TEST_CASE("PSD: merging layers - normal, opacity, Multiply, hidden ones left out, Layer Size crops",
          "[media][psd]") {
    const auto compression = GENERATE(TestPsdCompression::Raw, TestPsdCompression::Rle, TestPsdCompression::ZipPrediction);
    const int depth = GENERATE(8, 16);
    INFO("compression " << static_cast<int>(compression) << ", " << depth << " bits");
    const auto bytes = sampleDocument(depth, compression);
    // Merge All Layers: grey, the red square over it, the blue strip multiplied (grey x blue = dark blue), no hidden blue.
    auto all = compositePsd(std::span<const std::uint8_t>(bytes), {}, true);
    REQUIRE(all.has_value());
    REQUIRE(all->width == 64);
    REQUIRE(all->height == 32);
    CHECK_PIXEL(*all, 2, 2, kGrey);
    CHECK_PIXEL(*all, 12, 12, kRed);
    CHECK_PIXEL(*all, 44, 4, kGrey); // the hidden layer is left out
    CHECK_PIXEL(*all, 30, 30, (Rgba{0, 0, 32768, 65535}));
    // Merged Layers: only the red square, on the document's canvas (transparent around it) or cropped to it.
    const std::vector<int> red{1};
    auto canvas = compositePsd(std::span<const std::uint8_t>(bytes), red, true);
    REQUIRE(canvas.has_value());
    CHECK_PIXEL(*canvas, 12, 12, kRed);
    CHECK(pixel(*canvas, 2, 2)[3] == 0);
    auto cropped = compositePsd(std::span<const std::uint8_t>(bytes), red, false);
    REQUIRE(cropped.has_value());
    CHECK(cropped->width == 16);
    CHECK(cropped->height == 16);
    CHECK(cropped->left == 8);
    CHECK(cropped->top == 8);
    CHECK_PIXEL(*cropped, 0, 0, kRed);
    // An index the file does not have.
    const std::vector<int> missing{9};
    CHECK_FALSE(compositePsd(std::span<const std::uint8_t>(bytes), missing, true).has_value());
}

TEST_CASE("PSD: opacity, clipping masks and layer masks", "[media][psd]") {
    std::vector<TestPsdLayer> layers;
    TestPsdLayer half = solid("Half", 0, 0, 16, 16, kRed);
    half.opacity = 128;
    layers.push_back(half);
    TestPsdLayer base = solid("Base", 20, 0, 4, 4, kGrey);
    layers.push_back(base);
    TestPsdLayer clipped = solid("Clipped", 16, 0, 16, 16, kBlue); // shows only over Base
    clipped.clipped = true;
    layers.push_back(clipped);
    TestPsdLayer masked = solid("Masked", 0, 20, 16, 8, kBlue);
    masked.mask = std::array<int, 4>{0, 20, 8, 28}; // the left half of the layer shows
    layers.push_back(masked);
    const auto bytes = writePsd(32, 32, 8, layers);
    auto image = compositePsd(std::span<const std::uint8_t>(bytes), {}, true);
    REQUIRE(image.has_value());
    const auto alphaAt = [&](int x, int y) { return pixel(*image, x, y)[3]; };
    CHECK(std::abs(alphaAt(4, 4) - 128 * 257) < 400); // 50 % opacity
    CHECK_PIXEL(*image, 21, 1, kBlue);                // clipped to Base
    CHECK(alphaAt(28, 8) == 0);                       // outside Base: nothing
    CHECK_PIXEL(*image, 2, 24, kBlue);                // inside the mask
    CHECK(alphaAt(12, 24) == 0);                      // outside the mask
}

TEST_CASE("PSD: the non-separable blend modes - Hue, Saturation, Color and Luminosity - and the Light modes",
          "[media][psd]") {
    // A layer of (0.2, 0.4, 0.9) over a backdrop of (0.8, 0.2, 0.2), 16 bits: the W3C formulas Photoshop's modes follow.
    const Rgba backdrop{52428, 13107, 13107, 65535};
    const Rgba source{13107, 26214, 58982, 65535};
    const auto blended = [&](std::string mode, Rgba under = {52428, 13107, 13107, 65535}) {
        std::vector<TestPsdLayer> layers;
        layers.push_back(solid("Backdrop", 0, 0, 4, 4, under));
        TestPsdLayer top = solid("Top", 0, 0, 4, 4, source);
        top.blend = std::move(mode);
        layers.push_back(top);
        auto image = compositePsd(std::span<const std::uint8_t>(writePsd(4, 4, 16, layers)), {}, true);
        REQUIRE(image.has_value());
        return pixel(*image, 1, 1);
    };
    CHECK(closeTo(blended("hue "), {13950, 25184, 53271, 65535}, 40));  // the layer's hue, the backdrop's lum + sat
    CHECK(closeTo(blended("sat "), {57016, 11141, 11141, 65535}, 40));  // the layer's saturation
    CHECK(closeTo(blended("colr"), {12124, 25231, 57999, 65535}, 40));  // the layer's hue + saturation
    CHECK(closeTo(blended("lum "), {53411, 14090, 14090, 65535}, 40));  // the layer's luminosity
    // Darker / Lighter Color take one colour whole; the Light modes are channel by channel.
    CHECK(closeTo(blended("dkCl"), backdrop, 40));                      // 1.2 total < 1.5
    CHECK(closeTo(blended("lgCl"), source, 40));
    CHECK(closeTo(blended("lLit"), {13107, 0, 65535, 65535}, 40));      // b + 2s - 1, clamped
    CHECK(closeTo(blended("pLit"), {26214, 13107, 52428, 65535}, 40));  // min(b, 2s) below the middle, else max(b, 2s - 1)
    CHECK(closeTo(blended("hMix", {39321, 13107, 13107, 65535}), {0, 0, 65535, 65535}, 40)); // b + s >= 1
    // A grey layer in Color mode over a colour takes the colour out (the backdrop's luminosity stays).
    std::vector<TestPsdLayer> layers;
    layers.push_back(solid("Colour", 0, 0, 2, 2, kRed));
    TestPsdLayer grey = solid("Grey", 0, 0, 2, 2, kGrey);
    grey.blend = "colr";
    layers.push_back(grey);
    auto image = compositePsd(std::span<const std::uint8_t>(writePsd(2, 2, 16, layers)), {}, true);
    REQUIRE(image.has_value());
    const Rgba out = pixel(*image, 0, 0);
    CHECK(out[0] == out[1]);
    CHECK(out[1] == out[2]);
    CHECK(std::abs(static_cast<int>(out[0]) - 19661) < 40); // red's luminosity, 0.3
}

TEST_CASE("PSD: CMYK documents convert to RGB", "[media][psd]") {
    const auto compression = GENERATE(TestPsdCompression::Rle, TestPsdCompression::ZipPrediction);
    const auto depth = GENERATE(8, 16);
    std::vector<TestPsdLayer> layers;
    layers.push_back(solid("Paper", 0, 0, 16, 8, kGrey)); // 50 % black
    layers.push_back(solid("Cyan", 0, 0, 8, 8, Rgba{0, 65535, 65535, 65535})); // 100 % cyan ink
    TestPsdLayer red = solid("Red", 8, 0, 8, 8, kRed); // 100 % magenta and yellow
    red.opacity = 128;
    layers.push_back(red);
    const auto bytes = writePsd(16, 8, depth, layers, compression, false, kGrey, TestPsdMode::Cmyk);
    auto doc = readPsdDocument(std::span<const std::uint8_t>(bytes));
    REQUIRE(doc.has_value());
    CHECK(doc->cmyk);
    CHECK_FALSE(doc->grayscale);
    CHECK(doc->layers.size() == 3);
    auto image = compositePsd(std::span<const std::uint8_t>(bytes), {}, true);
    REQUIRE(image.has_value());
    CHECK_PIXEL(*image, 2, 2, (Rgba{0, 65535, 65535, 65535}));
    CHECK_PIXEL(*image, 12, 2, (Rgba{49151, 16384, 16384, 65535})); // half red over grey
    // A flattened CMYK file's merged picture.
    auto flat = compositePsd(std::span<const std::uint8_t>(writePsd(4, 4, depth, {}, TestPsdCompression::Raw, false,
                                                                    kRed, TestPsdMode::Cmyk)),
                             {}, true);
    REQUIRE(flat.has_value());
    CHECK_PIXEL(*flat, 1, 1, kRed);
}

TEST_CASE("PSD: 32-bit float documents are merged in linear light and written with the sRGB curve", "[media][psd]") {
    const auto compression = GENERATE(TestPsdCompression::Raw, TestPsdCompression::Rle, TestPsdCompression::ZipPrediction);
    std::vector<TestPsdLayer> layers;
    TestPsdLayer ramp = solid("Ramp", 0, 0, 8, 4, kRed);
    // Linear 0.5 grey on the left half, over-bright (4.0) red on the right: clamped to white's red.
    ramp.rgbaFloat = [](int x, int) {
        return x < 4 ? std::array<float, 4>{0.5f, 0.5f, 0.5f, 1.0f} : std::array<float, 4>{4.0f, 0.0f, 0.0f, 1.0f};
    };
    layers.push_back(ramp);
    TestPsdLayer half = solid("Half", 0, 2, 8, 2, kBlue);
    half.rgbaFloat = [](int, int) { return std::array<float, 4>{0.0f, 0.0f, 1.0f, 0.5f}; };
    layers.push_back(half);
    const auto bytes = writePsd(8, 4, 32, layers, compression);
    auto doc = readPsdDocument(std::span<const std::uint8_t>(bytes));
    REQUIRE(doc.has_value());
    CHECK(doc->depth == 32);
    REQUIRE(doc->layers.size() == 2);
    CHECK(doc->layers[1].name == "Half");
    auto image = compositePsd(std::span<const std::uint8_t>(bytes), {}, true);
    REQUIRE(image.has_value());
    CHECK(image->depth == 32);
    CHECK_PIXEL(*image, 1, 0, (Rgba{48192, 48192, 48192, 65535})); // linear 0.5 -> sRGB 0.735
    CHECK_PIXEL(*image, 5, 0, kRed);
    // Half blue over linear grey mixes in linear light: (0.25, 0.25, 0.75), then the curve.
    CHECK_PIXEL(*image, 1, 3, (Rgba{35199, 35199, 57725, 65535}));
    // The PAM keeps 16 bits.
    CHECK(toPam(*image).find("MAXVAL 65535") != std::string::npos);
    // A flattened 32-bit file.
    auto flat = compositePsd(std::span<const std::uint8_t>(writePsd(2, 2, 32, {}, TestPsdCompression::Raw)), {}, true);
    REQUIRE(flat.has_value());
    CHECK_PIXEL(*flat, 0, 0, (Rgba{48192, 48192, 48192, 65535})); // the writer's default merged grey, linear 0.5
}

TEST_CASE("PSD: PSB, flattened files and malformed input", "[media][psd]") {
    const auto psb = sampleDocument(8, TestPsdCompression::Rle, true);
    auto doc = readPsdDocument(std::span<const std::uint8_t>(psb));
    REQUIRE(doc.has_value());
    CHECK(doc->psb);
    CHECK(doc->layers.size() == 6);
    auto merged = compositePsd(std::span<const std::uint8_t>(psb), {}, true);
    REQUIRE(merged.has_value());
    CHECK_PIXEL(*merged, 12, 12, kRed);
    // No layers: the merged picture is the image.
    const auto flat = writePsd(8, 8, 8, {}, TestPsdCompression::Raw, false, kRed);
    auto flattened = compositePsd(std::span<const std::uint8_t>(flat), {}, true);
    REQUIRE(flattened.has_value());
    CHECK_PIXEL(*flattened, 4, 4, kRed);
    // Every truncation of a valid file is an error or a picture, never a crash.
    const auto bytes = sampleDocument(16, TestPsdCompression::ZipPrediction);
    for (std::size_t n = 0; n < bytes.size(); n += 7) {
        const std::span<const std::uint8_t> cut(bytes.data(), n);
        (void)readPsdDocument(cut);
        (void)compositePsd(cut, {}, true);
    }
    const std::uint8_t garbage[] = {'8', 'B', 'P', 'S', 0, 1, 0, 0, 0, 0, 0, 0, 0, 4, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_FALSE(readPsdDocument(std::span<const std::uint8_t>(garbage)).has_value());
    CHECK(readPsdDocument(std::span<const std::uint8_t>(garbage, 4)).error().code == ErrorCode::Unsupported);
}

TEST_CASE("PSD: a layered file's item probes and decodes as a still of its layers", "[media][psd]") {
    const auto file = writeTemp("psd-item", "Lower third.psd", sampleDocument(8, TestPsdCompression::Rle));
    LayeredFileSpec spec{file, {1}, false};
    const std::string path = layeredFilePath(spec);
    REQUIRE_FALSE(path.empty());
    CHECK(parseLayeredFilePath(path) == spec);
    CHECK(mediaFileToCheck(pathFromUtf8(path)) == file);
    auto info = probe(path);
    REQUIRE(info.has_value());
    CHECK(info->isStillImage);
    REQUIRE(info->video.size() == 1);
    CHECK(info->video.front().width == 16);
    CHECK(info->video.front().height == 16);
    auto decoder = VideoDecoder::open(path);
    REQUIRE(decoder.has_value());
    auto frame = (*decoder)->frameAt(Ticks{});
    REQUIRE(frame.has_value());
    CHECK(frame->width == 16);
    REQUIRE(frame->format == PixelFormat::RGBA8);
    const Plane& p = frame->cpu()->planes[0];
    CHECK(p.data[0] == 255); // red
    CHECK(p.data[1] == 0);
    CHECK(p.data[3] == 255);
    // Two layers of one file are two pictures (the frame cache keys them apart).
    LayeredFileSpec other{file, {0}, true};
    auto otherDecoder = VideoDecoder::open(layeredFilePath(other));
    REQUIRE(otherDecoder.has_value());
    CHECK((*otherDecoder)->source() != (*decoder)->source());
}

TEST_CASE("Spanned clip paths: built, parsed and bounded", "[media][spanned]") {
    SpannedClipSpec spec;
    spec.directory = pathFromUtf8("D:/DCIM/100GOPRO");
    spec.parts = {{"GX010042.MP4", 1'000'000}, {"GX020042.MP4", 500'000}};
    const std::string path = spannedClipPath(spec);
    CHECK(path == "D:/DCIM/100GOPRO/GX010042.MP4?span=GX010042.MP4:1000000|GX020042.MP4:500000");
    const auto parsed = parseSpannedClipPath(path);
    REQUIRE(parsed.has_value());
    CHECK(parsed->parts == spec.parts);
    CHECK(parsed->totalMicroseconds() == 1'500'000);
    for (const std::string_view bad : {"D:/a.mp4?span=a.mp4:1", "D:/a.mp4?span=b.mp4:1|a.mp4:1",
                                        "D:/a.mp4?span=a.mp4:1|..\\x.mp4:1", "D:/a.mp4?span=a.mp4:0|b.mp4:1",
                                        "D:/a.mp4?span=a.mp4:1|b.mp4:1|", "D:/a.mp4?span=a.mp4|b.mp4:1"}) {
        INFO(bad);
        CHECK_FALSE(parseSpannedClipPath(bad).has_value());
    }
}

TEST_CASE("Spanned clips: GoPro chapters play as one clip, frame-exact across the join", "[media][spanned]") {
    if (!haveMedia("GX010042.MP4") || !haveMedia("GX020042.MP4")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    SpannedClipSpec spec;
    spec.directory = pathFromUtf8(mediaPath("GX010042.MP4")).parent_path();
    spec.parts = {{"GX010042.MP4", 1'000'000}, {"GX020042.MP4", 1'000'000}};
    const std::string path = spannedClipPath(spec);
    auto info = probe(path);
    REQUIRE(info.has_value());
    CHECK(info->duration.toSeconds() == Approx(2.0).margin(0.05));
    REQUIRE(info->video.size() == 1);
    auto decoder = VideoDecoder::open(path);
    REQUIRE(decoder.has_value());
    for (const int frame : {0, 28, 29, 30, 31, 59, 45, 3}) { // forwards across the join, then seeks back
        auto f = (*decoder)->frameAt(framesToTicks(frame, Rate{30, 1}));
        REQUIRE(f.has_value());
        INFO("frame " << frame);
        CHECK(frameIndexOf(*f) == frame);
    }
    // The sound continues too: 2 s of it.
    auto audio = AudioDecoder::open(path, -1, kRate48kHz, ChannelLayout::stereo());
    REQUIRE(audio.has_value());
    std::vector<float> block(48000 * 2);
    auto read = (*audio)->read(Ticks::fromSeconds(1) - Ticks{kTicksPerSecond / 2}, block);
    REQUIRE(read.has_value());
    float peak = 0.0f;
    for (const float s : block) {
        peak = std::max(peak, std::abs(s));
    }
    CHECK(peak > 0.3f); // sound on both sides of the join
    // A missing part: not found.
    spec.parts[1].name = "GX990042.MP4";
    CHECK(probe(spannedClipPath(spec)).error().code == ErrorCode::NotFound);
}

TEST_CASE("Interlaced footage: the file's field order is probed", "[media][fields]") {
    if (!haveMedia("mpeg2_interlaced_tff.mpg")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    auto info = probe(mediaPath("mpeg2_interlaced_tff.mpg"));
    REQUIRE(info.has_value());
    REQUIRE(info->video.size() == 1);
    CHECK(info->video.front().fieldOrder == FieldOrder::UpperFirst);
    auto progressive = probe(mediaPath("GX010042.MP4"));
    if (progressive) {
        CHECK(progressive->video.front().fieldOrder == FieldOrder::Progressive);
    }
}

namespace {

/// A synthetic CinemaDNG frame: 16-bit RGGB samples, the left half one colour, the right half another (linear camera
/// RGB in [0, 1] above black 256 and below white 4095), uncompressed, identity colour matrix, white balance neutral.
std::vector<std::uint8_t> dngFrame(int width, int height, std::array<double, 3> left, std::array<double, 3> right) {
    std::vector<std::uint8_t> out{'I', 'I', 42, 0, 8, 0, 0, 0};
    const auto u16 = [&](std::uint16_t v) { out.push_back(static_cast<std::uint8_t>(v & 0xFF)); out.push_back(static_cast<std::uint8_t>(v >> 8)); };
    const auto u32 = [&](std::uint32_t v) { u16(static_cast<std::uint16_t>(v & 0xFFFF)); u16(static_cast<std::uint16_t>(v >> 16)); };
    struct Entry { std::uint16_t tag, type; std::uint32_t count; std::vector<std::uint8_t> value; };
    std::vector<Entry> entries;
    const auto add = [&](std::uint16_t tag, std::uint16_t type, std::vector<std::uint32_t> values) {
        Entry e{tag, type, static_cast<std::uint32_t>(values.size()), {}};
        for (const std::uint32_t v : values) {
            if (type == 3) { e.value.push_back(static_cast<std::uint8_t>(v & 0xFF)); e.value.push_back(static_cast<std::uint8_t>(v >> 8)); }
            else if (type == 1) { e.value.push_back(static_cast<std::uint8_t>(v)); }
            else { for (int k = 0; k < 4; ++k) e.value.push_back(static_cast<std::uint8_t>(v >> (8 * k))); }
        }
        if (type == 5 || type == 10) {
            e.count /= 2; // rationals: pairs
        }
        entries.push_back(std::move(e));
    };
    const std::uint32_t black = 256, white = 4095;
    std::vector<std::uint16_t> pixels;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto& c = x < width / 2 ? left : right;
            const int site = (y % 2) * 2 + (x % 2);
            const double v = site == 0 ? c[0] : site == 3 ? c[2] : c[1];
            pixels.push_back(static_cast<std::uint16_t>(black + v * (white - black)));
        }
    }
    add(254, 4, {0});
    add(256, 4, {static_cast<std::uint32_t>(width)});
    add(257, 4, {static_cast<std::uint32_t>(height)});
    add(258, 3, {16});
    add(259, 3, {1});
    add(262, 3, {32803});
    add(273, 4, {0}); // patched below
    add(277, 3, {1});
    add(278, 4, {static_cast<std::uint32_t>(height)});
    add(279, 4, {static_cast<std::uint32_t>(pixels.size() * 2)});
    add(284, 3, {1});
    add(33421, 3, {2, 2});
    add(33422, 1, {0, 1, 1, 2});
    add(50706, 1, {1, 4, 0, 0});
    add(50714, 4, {black});
    add(50717, 4, {white});
    add(50721, 10, {10000, 10000, 0, 10000, 0, 10000, 0, 10000, 10000, 10000, 0, 10000, 0, 10000, 0, 10000, 10000, 10000});
    add(50728, 5, {1, 1, 1, 1, 1, 1});
    add(50778, 3, {21});
    const std::size_t ifdSize = 2 + entries.size() * 12 + 4;
    std::size_t extraAt = 8 + ifdSize;
    std::vector<std::uint8_t> extra;
    std::vector<std::array<std::uint8_t, 4>> inline4;
    for (Entry& e : entries) {
        std::array<std::uint8_t, 4> field{};
        if (e.value.size() <= 4) {
            std::copy(e.value.begin(), e.value.end(), field.begin());
        } else {
            if ((extraAt + extra.size()) % 2 != 0) {
                extra.push_back(0);
            }
            const auto at = static_cast<std::uint32_t>(extraAt + extra.size());
            for (int k = 0; k < 4; ++k) field[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(at >> (8 * k));
            extra.insert(extra.end(), e.value.begin(), e.value.end());
        }
        inline4.push_back(field);
    }
    const auto dataAt = static_cast<std::uint32_t>(extraAt + extra.size());
    u16(static_cast<std::uint16_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        u16(entries[i].tag);
        u16(entries[i].type);
        u32(entries[i].count);
        if (entries[i].tag == 273) {
            u32(dataAt);
        } else {
            out.insert(out.end(), inline4[i].begin(), inline4[i].end());
        }
    }
    u32(0);
    out.insert(out.end(), extra.begin(), extra.end());
    for (const std::uint16_t v : pixels) {
        u16(v);
    }
    return out;
}

/// R'G'B' (BT.709 limited 10-bit decode) of pixel (x, y) of a YUV444P10 frame.
std::array<double, 3> rgbOf(const FrameHandle& f, int x, int y) {
    const auto sample = [&](int plane) {
        const Plane& p = f.cpu()->planes[static_cast<std::size_t>(plane)];
        std::uint16_t v = 0;
        std::memcpy(&v, p.data + static_cast<std::ptrdiff_t>(y) * p.stride + 2 * x, 2);
        return static_cast<double>(v);
    };
    const double yy = (sample(0) - 64.0) / 876.0;
    const double cb = (sample(1) - 512.0) / 896.0;
    const double cr = (sample(2) - 512.0) / 896.0;
    const double r = yy + 1.5748 * cr;
    const double b = yy + 1.8556 * cb;
    const double g = (yy - 0.2126 * r - 0.0722 * b) / 0.7152;
    return {r, g, b};
}

double bt709Decode(double v) {
    return v < 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1.0 / 0.45);
}

} // namespace

TEST_CASE("Camera RAW: a Bayer mosaic develops into Rec.709 with its levels, white balance and matrix",
          "[media][raw]") {
    // A uniform scene, linear (0.4, 0.2, 0.1) in camera RGB, sampled RGGB at 12 bits above a black of 256.
    constexpr int w = 8, h = 8;
    const std::array<double, 3> scene{0.4, 0.2, 0.1};
    std::vector<std::uint16_t> cfa(w * h);
    const double black = 256.0 / 65535.0, white = 4095.0 / 65535.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int site = (y % 2) * 2 + (x % 2);
            const double v = site == 0 ? scene[0] / 2.0 : site == 3 ? scene[2] / 0.5 : scene[1]; // as shot: wb 2 and 0.5
            cfa[y * w + x] = static_cast<std::uint16_t>(std::lround((black + v * (white - black)) * 65535.0));
        }
    }
    RawDevelopParams params;
    params.black = black;
    params.white = white;
    params.wbRed = 2.0;
    params.wbBlue = 0.5;
    std::vector<std::uint16_t> y(w * h), cb(w * h), cr(w * h);
    developRaw(cfa.data(), w, w, h, 65535, params, y.data(), cb.data(), cr.data(), w);
    // Decode the centre pixel back to linear light: the scene.
    const std::size_t i = 4 * w + 4;
    const double yy = (y[i] - 64.0) / 876.0, pb = (cb[i] - 512.0) / 896.0, pr = (cr[i] - 512.0) / 896.0;
    const double r = yy + 1.5748 * pr, b = yy + 1.8556 * pb, g = (yy - 0.2126 * r - 0.0722 * b) / 0.7152;
    CHECK(bt709Decode(r) == Approx(scene[0]).margin(0.01));
    CHECK(bt709Decode(g) == Approx(scene[1]).margin(0.01));
    CHECK(bt709Decode(b) == Approx(scene[2]).margin(0.01));
    // With ProRes RAW's camera matrix to XYZ (here: Rec.709 primaries' own RGB -> XYZ) and +1 stop of gain.
    params.hasMatrix = true;
    params.cameraToXyz = {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041};
    params.gain = 2.0;
    developRaw(cfa.data(), w, w, h, 65535, params, y.data(), cb.data(), cr.data(), w);
    const double y2 = (y[i] - 64.0) / 876.0, pb2 = (cb[i] - 512.0) / 896.0, pr2 = (cr[i] - 512.0) / 896.0;
    const double r2 = y2 + 1.5748 * pr2;
    const double b2 = y2 + 1.8556 * pb2;
    CHECK(bt709Decode(r2) == Approx(scene[0] * 2.0).margin(0.015));
    CHECK(bt709Decode(b2) == Approx(scene[2] * 2.0).margin(0.015));
    // Two halves keep their colours: the demosaic reads each pixel's own neighbourhood.
    params = RawDevelopParams{};
    for (int row = 0; row < h; ++row) {
        for (int x = 0; x < w; ++x) {
            const int site = (row % 2) * 2 + (x % 2);
            const bool redHalf = x < w / 2;
            const double v = site == 0 ? (redHalf ? 0.8 : 0.1) : site == 3 ? 0.1 : (redHalf ? 0.1 : 0.8);
            cfa[row * w + x] = static_cast<std::uint16_t>(std::lround(v * 65535.0));
        }
    }
    developRaw(cfa.data(), w, w, h, 65535, params, y.data(), cb.data(), cr.data(), w);
    const auto redAt = [&](std::size_t k) { return (y[k] - 64.0) / 876.0 + 1.5748 * (cr[k] - 512.0) / 896.0; };
    CHECK(redAt(2 * w + 1) > 0.7);
    CHECK(redAt(2 * w + 6) < 0.5);
}

TEST_CASE("Camera RAW: CinemaDNG frames import as an image sequence and decode developed", "[media][raw]") {
    const auto folder = scratchDir("cinemadng");
    for (int n = 1; n <= 3; ++n) {
        writeFile(folder / ("A001_C001_" + std::to_string(1000 + n).substr(1) + ".dng"),
                  dngFrame(64, 32, {0.8, 0.2, 0.1}, {0.1, 0.7, 0.2}));
    }
    auto sequence = findImageSequence(folder / "A001_C001_001.dng", Rate{24, 1});
    REQUIRE(sequence.has_value());
    CHECK(sequence->count == 3);
    const std::string path = imageSequencePath(*sequence);
    auto info = probe(path);
    REQUIRE(info.has_value());
    REQUIRE(info->video.size() == 1);
    CHECK(info->video.front().codec == "tiff");
    CHECK(info->video.front().color.transfer == TransferFunction::BT709); // what the development delivers
    CHECK(isRawMedia(*info)); // RAW Source Settings apply
    auto decoder = VideoDecoder::open(path);
    REQUIRE(decoder.has_value());
    auto frame = (*decoder)->frameAt(framesToTicks(2, Rate{24, 1}));
    REQUIRE(frame.has_value());
    REQUIRE(frame->format == PixelFormat::YUV444P10);
    CHECK(frame->color.transfer == TransferFunction::BT709);
    // FFmpeg normalises the levels and applies its white balance; the reds stay red, the greens green.
    const auto left = rgbOf(*frame, 10, 10);
    const auto right = rgbOf(*frame, 50, 10);
    INFO("left " << left[0] << " " << left[1] << " " << left[2] << ", right " << right[0] << " " << right[1] << " "
                 << right[2]);
    CHECK(left[0] > left[1]);
    CHECK(left[1] > left[2]);
    CHECK(right[1] > right[0]);
    CHECK(right[1] > right[2]);
    CHECK(bt709Decode(left[0]) > 0.6); // linear, not shown as dark gamma-coded values
}

TEST_CASE("Camera RAW: Source Settings' exposure and white balance in the development", "[media][raw]") {
    // The white balance: the identity for the as-shot light; a lower temperature turns a grey bluer, a higher one
    // warmer; + tint toward magenta.
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-3; };
    const auto same = whiteBalanceAdaptation(5600, 5600, 0);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            CHECK(close(same[static_cast<std::size_t>(r * 3 + c)], r == c ? 1.0 : 0.0));
        }
    }
    const auto d65 = lightChromaticity(6504, 0); // on the locus, beside D65 (0.3127, 0.3290)
    CHECK(d65[0] == Approx(0.3135).margin(0.002));
    CHECK(d65[1] == Approx(0.3237).margin(0.002));
    const auto tungsten = lightChromaticity(3200, 0);
    CHECK(tungsten[0] == Approx(0.4234).margin(0.003));
    CHECK(lightChromaticity(6504, 30)[1] > d65[1]); // + tint: greener light, above the locus

    constexpr int w = 8, h = 8;
    std::vector<std::uint16_t> cfa(w * h, static_cast<std::uint16_t>(0.25 * 65535)); // linear grey 0.25
    std::vector<std::uint16_t> y(w * h), cb(w * h), cr(w * h);
    const auto developed = [&](const RawDevelopParams& params) {
        developRaw(cfa.data(), w, w, h, 65535, params, y.data(), cb.data(), cr.data(), w);
        const std::size_t i = 4 * w + 4;
        const double yy = (y[i] - 64.0) / 876.0, pb = (cb[i] - 512.0) / 896.0, pr = (cr[i] - 512.0) / 896.0;
        const double r = yy + 1.5748 * pr, b = yy + 1.8556 * pb, g = (yy - 0.2126 * r - 0.0722 * b) / 0.7152;
        return std::array<double, 3>{bt709Decode(r), bt709Decode(g), bt709Decode(b)};
    };
    RawDevelopParams params;
    const auto shot = developed(params);
    CHECK(shot[0] == Approx(0.25).margin(0.01));
    params.exposureStops = 1.0; // +1 stop doubles the light
    CHECK(developed(params)[1] == Approx(0.5).margin(0.015));
    params.exposureStops = 0.0;
    params.whiteAdaptation = whiteBalanceAdaptation(6500, 3200, 0);
    const auto blue = developed(params);
    CHECK(blue[2] > blue[1]);
    CHECK(blue[1] > blue[0]);
    params.whiteAdaptation = whiteBalanceAdaptation(6500, 9000, 0);
    const auto warm = developed(params);
    CHECK(warm[0] > warm[1]);
    CHECK(warm[1] > warm[2]);
    params.whiteAdaptation = whiteBalanceAdaptation(6500, 6500, 50);
    const auto magenta = developed(params);
    CHECK(magenta[0] > magenta[1]);
    CHECK(magenta[2] > magenta[1]);

    // Through the media path: a CinemaDNG sequence developed with the item's settings, a picture of its own.
    const auto folder = scratchDir("cinemadng-settings");
    for (int n = 1; n <= 2; ++n) {
        writeFile(folder / ("A002_C001_" + std::to_string(1000 + n).substr(1) + ".dng"),
                  dngFrame(64, 32, {0.4, 0.4, 0.4}, {0.4, 0.4, 0.4}));
    }
    auto sequence = findImageSequence(folder / "A002_C001_001.dng", Rate{24, 1});
    REQUIRE(sequence.has_value());
    const std::string asShot = imageSequencePath(*sequence);
    const std::string darker = withRawSourceSettings(asShot, RawSourceSettings{-1.0, std::nullopt});
    const std::string tungstenLight = withRawSourceSettings(asShot, RawSourceSettings{0.0, RawWhiteBalance{3200, 0}});
    REQUIRE(probe(darker).has_value());
    CHECK(probe(darker)->video.front().width == 64);
    const auto pixelOf = [](const std::string& path) {
        auto decoder = VideoDecoder::open(path);
        REQUIRE(decoder.has_value());
        auto frame = (*decoder)->frameAt(Ticks{});
        REQUIRE(frame.has_value());
        const auto rgb = rgbOf(*frame, 20, 10);
        return std::pair{std::array<double, 3>{bt709Decode(rgb[0]), bt709Decode(rgb[1]), bt709Decode(rgb[2])},
                         (*decoder)->source()};
    };
    const auto [plain, plainSource] = pixelOf(asShot);
    const auto [dark, darkSource] = pixelOf(darker);
    const auto [cool, coolSource] = pixelOf(tungstenLight);
    INFO("as shot " << plain[0] << " " << plain[1] << " " << plain[2] << ", -1 stop " << dark[1] << ", 3200 K " << cool[0]
                    << " " << cool[1] << " " << cool[2]);
    CHECK(dark[1] == Approx(plain[1] / 2.0).margin(0.02));
    CHECK(cool[2] > plain[2]);
    CHECK(cool[0] < plain[0]);
    CHECK(darkSource != plainSource); // the frame caches keep the developments apart
    CHECK(coolSource != darkSource);
}

TEST_CASE("Camera RAW helpers: a vendor-SDK helper's frames probe and decode like any clip", "[media][raw][helper]") {
    // The helper protocol (media/RawHelper.h) with the fake helper the build puts in WABCUT_TEST_RAW_HELPERS.
#if defined(_WIN32)
    _putenv_s("WABCUT_RAW_HELPERS", WABCUT_TEST_RAW_HELPERS);
#else
    setenv("WABCUT_RAW_HELPERS", WABCUT_TEST_RAW_HELPERS, 1);
#endif
    const auto folder = scratchDir("raw-helper");
    {
        std::ofstream(folder / "A001.fakeraw") << "64 32 24 1 12\n";
        std::ofstream(folder / "broken.fakeraw") << "error\n";
    }
    REQUIRE(rawHelperFor(folder / "A001.fakeraw").has_value());
    CHECK_FALSE(rawHelperFor(folder / "clip.mp4").has_value()); // FFmpeg's own formats never go to a helper
    const std::string path = pathToUtf8(folder / "A001.fakeraw");
    auto info = probe(path);
    REQUIRE(info.has_value());
    REQUIRE(info->video.size() == 1);
    CHECK(info->video.front().width == 64);
    CHECK(info->video.front().frameRate == (Rate{24, 1}));
    CHECK(info->duration.toSeconds() == Approx(0.5).margin(0.01));
    CHECK(info->audio.empty());
    auto decoder = VideoDecoder::open(path);
    REQUIRE(decoder.has_value());
    for (const int k : {0, 5, 11, 2}) { // forwards, then back: the helper is asked for any frame
        auto f = (*decoder)->frameAt(framesToTicks(k, Rate{24, 1}));
        REQUIRE(f.has_value());
        REQUIRE(f->format == PixelFormat::RGBA8);
        INFO("frame " << k);
        CHECK(std::abs(static_cast<int>(f->cpu()->planes[0].data[0]) - 20 * k) <= 1);
    }
    // A helper that refuses the file says why.
    auto refused = probe(pathToUtf8(folder / "broken.fakeraw"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("not supported by the fake helper") != std::string::npos);
}
