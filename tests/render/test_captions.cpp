// Burned-in captions (render::CaptionRenderer, I.4): the cue drawn inside the title-safe area at the bottom or top,
// wrapped to the caption width, over a background box or with an outline; Thai shaped with nothing missing; word
// spans for karaoke and the highlight split; and the caption composited by the GPU against a CPU reference of the
// same pixels (golden PSNR, English and Thai cues).

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/render/Caption.h>

#include <QDir>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <vector>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kW = 640;
constexpr int kH = 360;

CaptionFrame frameOf(std::string text, CaptionStyle style = {}) {
    CaptionFrame f;
    f.text = std::move(text);
    f.frameWidth = kW;
    f.frameHeight = kH;
    f.style = style;
    return f;
}

std::array<std::uint8_t, 4> pixel(const TitleRaster& r, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(r.width()) + static_cast<std::size_t>(x)) * 4;
    return {r.rgba()[i], r.rgba()[i + 1], r.rgba()[i + 2], r.rgba()[i + 3]};
}

/// The caption raster (premultiplied, sRGB-encoded) as the reference's straight-alpha input.
LinearImage linearOf(const TitleRaster& r) {
    return linearFromRgba8(r.width(), r.height(), [&](int x, int y) {
        const auto p = pixel(r, x, y);
        if (p[3] == 0) {
            return std::array<std::uint8_t, 4>{0, 0, 0, 0};
        }
        const auto un = [&](std::uint8_t c) {
            return static_cast<std::uint8_t>(std::min(255, static_cast<int>(std::lround(c * 255.0 / p[3]))));
        };
        return std::array<std::uint8_t, 4>{un(p[0]), un(p[1]), un(p[2]), p[3]};
    });
}

} // namespace

TEST_CASE("Captions: inside the title-safe area, wrapped, at the bottom or the top", "[render][captions]") {
    const auto bottom = CaptionRenderer::renderUncached(frameOf("Hello world"));
    const auto& b = bottom->info().bounds; // the box
    CHECK(b[1] + b[3] <= kH * 0.9 + 0.5);  // bottom edge on the title-safe line
    CHECK(b[1] + b[3] >= kH * 0.9 - 0.5);
    CHECK(std::abs((b[0] + b[2] / 2.0) - kW / 2.0) < 1.0); // centred
    CHECK(bottom->info().lines == 1);
    CHECK(bottom->info().missingGlyphs == 0);
    // The box is the configured colour at 78 %, behind the text.
    const auto inBox = pixel(*bottom, static_cast<int>(b[0] + 2), static_cast<int>(b[1] + b[3] / 2));
    CHECK(std::abs(inBox[3] - 199) <= 1); // 0.78 * 255
    CHECK(pixel(*bottom, 5, 5)[3] == 0);

    CaptionStyle top;
    top.position = CaptionPosition::Top;
    const auto atTop = CaptionRenderer::renderUncached(frameOf("Hello world", top));
    CHECK(atTop->info().bounds[1] == kH * 0.1);

    // A long cue wraps inside 80 % of the width; an explicit line break starts a new line.
    const auto wrapped = CaptionRenderer::renderUncached(
        frameOf("This caption is far too long to fit on one line of a small frame, so it wraps"));
    CHECK(wrapped->info().lines >= 2);
    CHECK(wrapped->info().bounds[2] <= kW * 0.8 + 0.5);
    CHECK(CaptionRenderer::renderUncached(frameOf("one\ntwo"))->info().lines == 2);

    // Without a box: an outline instead, no box pixels.
    CaptionStyle outlined;
    outlined.box = false;
    outlined.strokeWidth = 3.0;
    const auto open = CaptionRenderer::renderUncached(frameOf("Hello world", outlined));
    const auto& o = open->info().bounds;
    CHECK(pixel(*open, static_cast<int>(o[0]) - 6, static_cast<int>(o[1] + o[3] / 2))[3] == 0);

    // Deterministic and cached.
    auto& shared = CaptionRenderer::shared();
    CHECK(shared.render(frameOf("cache me")) == shared.render(frameOf("cache me")));
    const auto first = CaptionRenderer::renderUncached(frameOf("same"));
    const auto second = CaptionRenderer::renderUncached(frameOf("same"));
    int differing = 0;
    int firstX = -1;
    int firstY = -1;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (pixel(*first, x, y) != pixel(*second, x, y)) {
                if (differing++ == 0) {
                    firstX = x;
                    firstY = y;
                }
            }
        }
    }
    INFO("differing pixels " << differing << " first at " << firstX << "," << firstY << " box " << first->info().bounds[0]
                             << "," << first->info().bounds[1] << " " << first->info().bounds[2] << "x"
                             << first->info().bounds[3]);
    CHECK(differing == 0);
}

TEST_CASE("Captions: Thai is shaped with nothing missing; word spans; the karaoke split", "[render][captions]") {
    const auto thai = CaptionRenderer::renderUncached(frameOf("สวัสดีครับ ยินดีต้อนรับ"));
    CHECK(thai->info().missingGlyphs == 0);
    CHECK(thai->info().glyphs > 10);

    const auto spans = captionWordSpans("Hello, big world!");
    REQUIRE(spans.size() == 3);
    CHECK(spans[0] == std::pair{0, 5});
    CHECK(spans[1] == std::pair{7, 10});
    CHECK(spans[2] == std::pair{11, 16});
    CHECK(captionWordSpans("one\ntwo").size() == 2);
    const auto thaiSpans = captionWordSpans("สวัสดีครับ ยินดีต้อนรับ");
    CHECK(thaiSpans.size() >= 2); // at least the space-separated groups (Qt splits Thai words where it can)
    UNSCOPED_INFO("Thai word spans: " << thaiSpans.size());

    // Karaoke: the spoken words take the highlight colour, the rest the fill.
    CaptionStyle style;
    style.box = false;
    style.fill = {1.0, 1.0, 1.0};
    style.highlight = {1.0, 0.0, 0.0};
    CaptionFrame f = frameOf("AAAA BBBB", style);
    f.highlightEnd = 4; // "AAAA" spoken
    const auto raster = CaptionRenderer::renderUncached(f);
    const auto& bounds = raster->info().bounds;
    int red = 0;
    int white = 0;
    int redRight = 0;
    const int mid = static_cast<int>(bounds[0] + bounds[2] / 2);
    for (int y = static_cast<int>(bounds[1]); y < static_cast<int>(bounds[1] + bounds[3]); ++y) {
        for (int x = static_cast<int>(bounds[0]); x < static_cast<int>(bounds[0] + bounds[2]); ++x) {
            const auto p = pixel(*raster, x, y);
            if (p[3] < 250) {
                continue;
            }
            if (p[0] > 240 && p[1] < 20) {
                ++red;
                redRight += x > mid ? 1 : 0;
            } else if (p[0] > 240 && p[1] > 240) {
                ++white;
            }
        }
    }
    CHECK(red > 50);
    CHECK(white > 50);
    CHECK(redRight == 0); // only the first word
}

namespace {

/// Leftmost and rightmost columns with text (alpha above half) in rows [y0, y1).
std::pair<int, int> inkColumns(const TitleRaster& r, int y0, int y1) {
    int left = r.width();
    int right = -1;
    for (int y = std::max(0, y0); y < std::min(r.height(), y1); ++y) {
        for (int x = 0; x < r.width(); ++x) {
            if (pixel(r, x, y)[3] > 128) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    return {left, right};
}

int countPixels(const TitleRaster& r, const std::function<bool(const std::array<std::uint8_t, 4>&)>& match) {
    int n = 0;
    for (int y = 0; y < r.height(); ++y) {
        for (int x = 0; x < r.width(); ++x) {
            n += match(pixel(r, x, y)) ? 1 : 0;
        }
    }
    return n;
}

} // namespace

TEST_CASE("Captions: the model's style - alignment, custom placement, padding, radius, shadow, speaker colours",
          "[render][captions]") {
    // Alignment of the lines inside the block (no box: the ink alone).
    CaptionStyle plain;
    plain.box = false;
    const auto lines = [&](CaptionAlign align) {
        CaptionStyle s = plain;
        s.align = align;
        const auto r = CaptionRenderer::renderUncached(frameOf("Short\nA much longer second line", s));
        const auto& b = r->info().bounds;
        const int mid = static_cast<int>(b[1] + b[3] / 2);
        return std::pair{inkColumns(*r, static_cast<int>(b[1]), mid), inkColumns(*r, mid, static_cast<int>(b[1] + b[3]))};
    };
    const auto [leftTop, leftBottom] = lines(CaptionAlign::Left);
    CHECK(std::abs(leftTop.first - leftBottom.first) <= 3);
    const auto [rightTop, rightBottom] = lines(CaptionAlign::Right);
    CHECK(std::abs(rightTop.second - rightBottom.second) <= 3);
    const auto [centreTop, centreBottom] = lines(CaptionAlign::Center);
    CHECK(std::abs((centreTop.first + centreTop.second) - (centreBottom.first + centreBottom.second)) <= 6);
    CHECK(centreTop.first > centreBottom.first + 20);

    // Custom placement: the block centred at (25 %, 30 %); kept inside the safe area at the frame's edge.
    CaptionStyle custom;
    custom.position = CaptionPosition::Custom;
    custom.centerX = 0.25;
    custom.centerY = 0.3;
    const auto placed = CaptionRenderer::renderUncached(frameOf("Placed", custom));
    const auto& p = placed->info().bounds;
    CHECK(std::abs(p[0] + p[2] / 2 - kW * 0.25) < 1.0);
    CHECK(std::abs(p[1] + p[3] / 2 - kH * 0.3) < 1.0);
    custom.centerX = 0.0;
    custom.centerY = 1.0;
    const auto edge = CaptionRenderer::renderUncached(frameOf("Placed", custom));
    CHECK(std::abs(edge->info().bounds[0] - kW * 0.1) < 1e-6);
    CHECK(std::abs(edge->info().bounds[1] + edge->info().bounds[3] - kH * 0.9) < 1e-6);

    // Box padding and radius from the style (em).
    CaptionStyle roomy;
    roomy.boxPadding = 0.8;
    const auto big = CaptionRenderer::renderUncached(frameOf("Hello world", roomy));
    const auto normal = CaptionRenderer::renderUncached(frameOf("Hello world"));
    // 16 px text (48 px of a 1080-line frame at 360 lines): (0.8 - 0.35) em more padding on each side.
    CHECK(std::abs(big->info().bounds[3] - normal->info().bounds[3] - 0.45 * 16.0) < 0.01);
    CHECK(std::abs(big->info().bounds[2] - normal->info().bounds[2] - 2.0 * 0.45 * 16.0) < 0.01);
    CaptionStyle large;
    large.sizePx = 144.0; // 48 px at 360 lines: a radius of 8.6 px
    const auto rounded = CaptionRenderer::renderUncached(frameOf("Hello", large));
    CaptionStyle square = large;
    square.boxRadius = 0.0;
    const auto corner = CaptionRenderer::renderUncached(frameOf("Hello", square));
    const auto& q = corner->info().bounds;
    CHECK(pixel(*corner, static_cast<int>(q[0]) + 1, static_cast<int>(q[1]) + 1)[3] >= 190); // a square corner is filled
    CHECK(pixel(*rounded, static_cast<int>(q[0]) + 1, static_cast<int>(q[1]) + 1)[3] < 60);  // a round one is not

    // A drop shadow down and right of white text: dark pixels where the text is not.
    CaptionStyle shadowed = plain;
    shadowed.sizePx = 96.0;
    shadowed.shadowOpacity = 1.0;
    shadowed.shadowDistance = 0.15;
    shadowed.shadowAngle = 45.0;
    shadowed.shadowSoftness = 0.0;
    const auto hard = CaptionRenderer::renderUncached(frameOf("Shadow", shadowed));
    const auto dark = [](const std::array<std::uint8_t, 4>& px) { return px[3] > 200 && px[0] < 60 && px[1] < 60 && px[2] < 60; };
    CHECK(countPixels(*hard, dark) > 100);
    CaptionStyle unshadowed = plain;
    unshadowed.sizePx = 96.0;
    CHECK(countPixels(*CaptionRenderer::renderUncached(frameOf("Shadow", unshadowed)), dark) == 0);
    shadowed.shadowSoftness = 0.3;
    const auto soft = CaptionRenderer::renderUncached(frameOf("Shadow", shadowed));
    const auto faint = [](const std::array<std::uint8_t, 4>& px) { return px[3] > 5 && px[3] < 120; };
    CHECK(countPixels(*soft, faint) > countPixels(*hard, faint) + 200); // the blur spreads it

    // A speaker's name in their colour, the rest in the fill.
    CaptionFrame named = frameOf("Alice: hello there", plain);
    named.colorSpans = {{0, 5, {0.2, 0.4, 1.0}}};
    const auto speaker = CaptionRenderer::renderUncached(named);
    const auto blue = [](const std::array<std::uint8_t, 4>& px) { return px[3] > 240 && px[2] > 230 && px[0] < 80; };
    const auto white = [](const std::array<std::uint8_t, 4>& px) { return px[3] > 240 && px[0] > 240 && px[2] > 240; };
    CHECK(countPixels(*speaker, blue) > 50);
    CHECK(countPixels(*speaker, white) > 50);
}

TEST_CASE("Captions: animations - opacity, scale around the block, hidden words with a stable layout",
          "[render][captions]") {
    const auto base = CaptionRenderer::renderUncached(frameOf("AAAA BBBB"));
    const auto& b = base->info().bounds;
    const auto alphaAt = [&](const TitleRaster& r) { return pixel(r, static_cast<int>(b[0] + 3), static_cast<int>(b[1] + b[3] / 2))[3]; };
    CHECK(std::abs(alphaAt(*base) - 199) <= 1);
    CaptionFrame faded = frameOf("AAAA BBBB");
    faded.opacity = 0.5;
    CHECK(std::abs(alphaAt(*CaptionRenderer::renderUncached(faded)) - 100) <= 2);
    faded.opacity = 0.0;
    CHECK(countPixels(*CaptionRenderer::renderUncached(faded), [](const auto& px) { return px[3] != 0; }) == 0);

    CaptionFrame popped = frameOf("AAAA BBBB");
    popped.scale = 0.5;
    const auto small = CaptionRenderer::renderUncached(popped);
    const auto [left, right] = inkColumns(*small, 0, kH);
    CHECK(std::abs((right - left) - b[2] / 2) < 4.0);              // half the width
    CHECK(std::abs((left + right) / 2.0 - (b[0] + b[2] / 2)) < 2.0); // around the same centre

    // Word by word: the second word not drawn yet; the box (the layout) unchanged.
    CaptionStyle open;
    open.box = false;
    CaptionFrame byWord = frameOf("AAAA BBBB", open);
    const auto all = CaptionRenderer::renderUncached(byWord);
    byWord.hidden = {{5, 9}};
    const auto half = CaptionRenderer::renderUncached(byWord);
    const auto [allLeft, allRight] = inkColumns(*all, 0, kH);
    const auto [halfLeft, halfRight] = inkColumns(*half, 0, kH);
    CHECK(halfLeft == allLeft); // the first word did not move
    CHECK(halfRight < allLeft + (allRight - allLeft) / 2 + 2);
}

TEST_CASE("Captions: composited by the GPU match the CPU reference (en, th)", "[render][gpu][captions]") {
    auto renderer = makeRenderer(OutputRole::Export);
    QDir().mkpath(outputDir() + QStringLiteral("/captions"));
    const auto background = [](int x, int y) {
        return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(40 + x * 150 / kW), static_cast<std::uint8_t>(70 + y * 100 / kH),
                                           140, 255};
    };
    struct Case {
        const char* name;
        std::string text;
        bool box;
        CaptionPosition position;
        int highlightEnd;
    };
    const Case cases[] = {
        {"caption_en_box", "Every frame of the cue, burned in", true, CaptionPosition::Bottom, -1},
        {"caption_en_outline_top", "Outlined at the top", false, CaptionPosition::Top, -1},
        {"caption_th_box", "สวัสดีครับ ยินดีต้อนรับ", true, CaptionPosition::Bottom, -1},
        {"caption_en_karaoke", "Words light up as they are spoken", true, CaptionPosition::Bottom, 11},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        CaptionStyle style;
        style.box = c.box;
        style.strokeWidth = c.box ? 0.0 : 3.0;
        style.position = c.position;
        CaptionFrame f = frameOf(c.text, style);
        f.highlightEnd = c.highlightEnd;
        const auto raster = CaptionRenderer::shared().render(f);
        const LinearImage expected = referenceOver(linearOf(*raster), linearFromRgba8(kW, kH, background));
        auto composite = compositeOf({kW, kH}, {layerOf(makeRgbaFrame(kW, kH, background)), layerOf(raster->view(raster))});
        auto image = renderer->render(composite);
        REQUIRE(image);
        const OutputTransform transform = outputTransformFor(composite->color, OutputRole::Export);
        const double psnr = psnrRgb(*image, encodeOverBlack(expected, transform));
        savePng(*image, QStringLiteral("captions/%1.png").arg(QString::fromLatin1(c.name)));
        UNSCOPED_INFO(std::format("{}: PSNR {:.2f} dB", c.name, psnr));
        CHECK(psnr >= 42.0);
    }
}

TEST_CASE("Captions: styled (the model's styles) composited by the GPU match the CPU reference (en, th)",
          "[render][gpu][captions]") {
    auto renderer = makeRenderer(OutputRole::Export);
    QDir().mkpath(outputDir() + QStringLiteral("/captions"));
    const auto background = [](int x, int y) {
        return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(60 + x * 120 / kW), static_cast<std::uint8_t>(90 + y * 90 / kH),
                                           170, 255};
    };
    struct Case {
        const char* name;
        CaptionFrame frame;
    };
    std::vector<Case> cases;
    {
        // "Shorts": big, bold, outlined, a soft shadow, centred high up, no box.
        CaptionStyle s;
        s.sizePx = 96.0;
        s.weight = 800;
        s.box = false;
        s.strokeWidth = 9.6;
        s.shadowOpacity = 0.7;
        s.shadowDistance = 0.1;
        s.shadowAngle = 90.0;
        s.shadowSoftness = 0.25;
        s.position = CaptionPosition::Custom;
        s.centerX = 0.5;
        s.centerY = 0.35;
        cases.push_back({"caption_style_shorts_en", frameOf("Big words, loud", s)});
    }
    {
        // Left-aligned, two lines, a wide padded square box, a speaker in their colour.
        CaptionStyle s;
        s.align = CaptionAlign::Left;
        s.boxPadding = 0.6;
        s.boxRadius = 0.0;
        CaptionFrame f = frameOf("Alice: the left edge\nlines up here", s);
        f.colorSpans = {{0, 5, {0.98, 0.55, 0.2}}};
        cases.push_back({"caption_style_left_speaker_en", f});
    }
    {
        // Thai, outlined with a shadow (the "Thai-friendly" look), at the top.
        CaptionStyle s;
        s.sizePx = 56.0;
        s.box = false;
        s.strokeWidth = 4.0;
        s.shadowOpacity = 0.8;
        s.shadowSoftness = 0.15;
        s.position = CaptionPosition::Top;
        cases.push_back({"caption_style_th_top", frameOf("สวัสดีครับ ยินดีต้อนรับ", s)});
    }
    {
        // Mid-animation: popping in (scaled, half faded) and word by word (the last word not spoken yet).
        CaptionFrame pop = frameOf("Popping in");
        pop.scale = 0.9;
        pop.opacity = 0.6;
        cases.push_back({"caption_style_pop", pop});
        CaptionFrame byWord = frameOf("One word at a time");
        byWord.hidden = {{14, 18}};
        cases.push_back({"caption_style_word_by_word", byWord});
    }
    for (const Case& c : cases) {
        INFO(c.name);
        const auto raster = CaptionRenderer::shared().render(c.frame);
        CHECK(raster->info().missingGlyphs == 0);
        const LinearImage expected = referenceOver(linearOf(*raster), linearFromRgba8(kW, kH, background));
        auto composite = compositeOf({kW, kH}, {layerOf(makeRgbaFrame(kW, kH, background)), layerOf(raster->view(raster))});
        auto image = renderer->render(composite);
        REQUIRE(image);
        const OutputTransform transform = outputTransformFor(composite->color, OutputRole::Export);
        const double psnr = psnrRgb(*image, encodeOverBlack(expected, transform));
        savePng(*image, QStringLiteral("captions/%1.png").arg(QString::fromLatin1(c.name)));
        UNSCOPED_INFO(std::format("{}: PSNR {:.2f} dB", c.name, psnr));
        CHECK(psnr >= 42.0);
    }
}

// ---- card 114: the text rules ---------------------------------------------------------------------------------------

TEST_CASE("Caption text rules: punctuation and accents and case in the scripts of the card languages", "[render][captions][text]") {
    const auto text = [](const std::string& in, CaptionCase c, bool punct, bool accents, const std::string& lang = {}) {
        return captionDisplayText(in, c, punct, accents, lang);
    };
    // Nothing asked: the same text, byte for byte.
    CHECK(text("Hello, World!", CaptionCase::AsWritten, false, false) == "Hello, World!");
    // Punctuation: sentence marks go; apostrophes and hyphens inside words, the separators of numbers and symbols stay.
    CHECK(text("Hello, World! It's 3.5% \xE2\x80\x94 well-known; ok?", CaptionCase::AsWritten, true, false) ==
          "Hello World It's 3.5% well-known ok");
    CHECK(text("\xE2\x80\x9CHi\xE2\x80\x9D (there)... 1,000 times.", CaptionCase::AsWritten, true, false) == "Hi there 1,000 times");
    // A mark that was the only thing between two words leaves a space; line breaks stay.
    CHECK(text("yes,no", CaptionCase::AsWritten, true, false) == "yes no");
    CHECK(text("One,\ntwo.", CaptionCase::AsWritten, true, false) == "One\ntwo");
    // Spanish, Arabic and Chinese marks.
    CHECK(text("\xC2\xBF" "Qu\xC3\xA9 tal?", CaptionCase::AsWritten, true, true) == "Que tal");
    CHECK(text("مرحبا، كيف؟", CaptionCase::AsWritten, true, false) ==
          "مرحبا كيف");
    CHECK(text("你好，世界。", CaptionCase::AsWritten, true, false) == "你好 世界");
    // Accents: Polish (l-stroke has no mark), Icelandic (eth, thorn, ae), French; Hebrew points; Thai stays.
    CHECK(text("Zażółć gęślą jaźń", CaptionCase::AsWritten, false, true) == "Zazolc gesla jazn");
    CHECK(text("Þórður Ægir", CaptionCase::AsWritten, false, true) == "Thordur AEgir");
    CHECK(text("Crème brûlée", CaptionCase::AsWritten, false, true) == "Creme brulee");
    CHECK(text("שָׁלוֹם", CaptionCase::AsWritten, false, true) == "שלום");
    CHECK(text("สวัสดี", CaptionCase::AsWritten, true, true) == "สวัสดี");
    CHECK(text("がんばる", CaptionCase::AsWritten, true, true) == "がんばる"); // kana keep their marks
    // Case: Latin, Cyrillic, Greek; Turkish and Azeri turn I into dotless i.
    CHECK(text("Hello World", CaptionCase::Lower, false, false) == "hello world");
    CHECK(text("Hello World", CaptionCase::Upper, false, false) == "HELLO WORLD");
    CHECK(text("Привет", CaptionCase::Upper, false, false) == "ПРИВЕТ");
    CHECK(text("ISTANBUL", CaptionCase::Lower, false, false, "tr") == "ıstanbul");
    CHECK(text("istanbul", CaptionCase::Upper, false, false, "tr-TR") == "İSTANBUL");
    CHECK(text("ISTANBUL", CaptionCase::Lower, false, false, "en") == "istanbul");
    // Everything at once, and again: applying it twice changes nothing.
    const std::string once = text("¡Qué día, Señor!", CaptionCase::Upper, true, true, "es");
    CHECK(once == "QUE DIA SENOR");
    CHECK(text(once, CaptionCase::Upper, true, true, "es") == once);
}

TEST_CASE("Caption emphasis: keywords without case and the automatic rules", "[render][captions][text]") {
    const std::string line = "GG this is 3 clutch! win";
    const auto spans = captionWordSpans(line);
    REQUIRE(spans.size() == 6);
    const auto flags = [&](bool automatic, std::vector<std::string> words) { return captionEmphasisFlags(line, spans, automatic, words); };
    // Keywords only: compared without case ("win" matches "WIN").
    CHECK(flags(false, {"gg", "WIN"}) == std::vector<char>{1, 0, 0, 0, 0, 1});
    // Automatic: capitals (GG), numbers (3), the word before "!" (clutch), plus the keywords.
    CHECK(flags(true, {}) == std::vector<char>{1, 0, 0, 1, 1, 0});
    CHECK(flags(true, {"win"}) == std::vector<char>{1, 0, 0, 1, 1, 1});
    // A single capital letter is not shouting ("I", "A"); a word with accents and capitals is.
    const std::string small = "I am ÉLAN";
    CHECK(captionEmphasisFlags(small, captionWordSpans(small), true, {}) == std::vector<char>{0, 0, 1});
}

// ---- card 074: per-word effects and the highlight bar ------------------------------------------------------------------

namespace {

/// Rows with text (alpha above half) between columns [x0, x1].
std::pair<int, int> inkRows(const TitleRaster& r, int x0, int x1) {
    int top = r.height();
    int bottom = -1;
    for (int y = 0; y < r.height(); ++y) {
        for (int x = std::max(0, x0); x <= std::min(r.width() - 1, x1); ++x) {
            if (pixel(r, x, y)[3] > 128) {
                top = std::min(top, y);
                bottom = std::max(bottom, y);
                break;
            }
        }
    }
    return {top, bottom};
}

int maxAlpha(const TitleRaster& r, int x0, int x1) {
    int best = 0;
    for (int y = 0; y < r.height(); ++y) {
        for (int x = std::max(0, x0); x <= std::min(r.width() - 1, x1); ++x) {
            best = std::max<int>(best, pixel(r, x, y)[3]);
        }
    }
    return best;
}

CaptionStyle openStyle() {
    CaptionStyle s;
    s.box = false;
    s.sizePx = 144.0; // 48 px at 360 lines
    return s;
}

} // namespace

TEST_CASE("Captions: a word with an effect is scaled and lifted and faded around its own centre", "[render][captions][animation]") {
    const CaptionFrame base = frameOf("AAAA BBBB", openStyle());
    const auto plain = CaptionRenderer::renderUncached(base);
    const auto [plainLeft, plainRight] = inkColumns(*plain, 0, kH);
    const auto [plainTop, plainBottom] = inkRows(*plain, 0, kW);

    // Scale: the second word twice as large grows the block's ink to the right (and its height).
    CaptionFrame big = base;
    big.wordFx = {{5, 9, 2.0, 0.0, 1.0}};
    const auto grown = CaptionRenderer::renderUncached(big);
    const auto [bigLeft, bigRight] = inkColumns(*grown, 0, kW);
    CHECK(bigLeft == plainLeft);       // the first word did not move
    CHECK(bigRight > plainRight + 8);  // the second one is wider
    CHECK(inkRows(*grown, 0, kW).first < plainTop - 3);

    // Lift: the first word 0.5 em higher; the second stays on the line.
    CaptionFrame lifted = base;
    lifted.wordFx = {{0, 4, 1.0, 0.5, 1.0}};
    const auto hop = CaptionRenderer::renderUncached(lifted);
    const int firstWordRight = plainLeft + (plainRight - plainLeft) / 2 - 2;
    CHECK(inkRows(*hop, plainLeft, firstWordRight).first < inkRows(*plain, plainLeft, firstWordRight).first - 4);
    CHECK(inkRows(*hop, firstWordRight + 6, plainRight).first == inkRows(*plain, firstWordRight + 6, plainRight).first);

    // Opacity: the second word half transparent, the first fully drawn.
    CaptionFrame faded = base;
    faded.wordFx = {{5, 9, 1.0, 0.0, 0.5}};
    const auto dim = CaptionRenderer::renderUncached(faded);
    const int middle = (plainLeft + plainRight) / 2;
    CHECK(maxAlpha(*dim, plainLeft, middle - 4) > 240);
    const int dimmest = maxAlpha(*dim, middle + 4, plainRight);
    CHECK(dimmest > 100);
    CHECK(dimmest < 150);

    // An effect on a hidden word draws nothing; the layout of the others is unchanged.
    CaptionFrame hidden = base;
    hidden.hidden = {{5, 9}};
    hidden.wordFx = {{5, 9, 2.0, 0.0, 1.0}};
    const auto shown = CaptionRenderer::renderUncached(hidden);
    CHECK(inkColumns(*shown, 0, kH).second < middle + 2);
}

TEST_CASE("Captions: the highlight bar sits behind its word in the highlight colour and slides between words",
          "[render][captions][animation]") {
    CaptionStyle style = openStyle();
    style.highlight = {1.0, 0.0, 1.0}; // magenta: nothing else is magenta
    const auto magenta = [](const std::array<std::uint8_t, 4>& px) { return px[3] > 200 && px[0] > 200 && px[1] < 60 && px[2] > 200; };
    const auto columnsOf = [&](const CaptionFrame& f) {
        const auto r = CaptionRenderer::renderUncached(f);
        int left = r->width();
        int right = -1;
        int n = 0;
        for (int y = 0; y < r->height(); ++y) {
            for (int x = 0; x < r->width(); ++x) {
                if (magenta(pixel(*r, x, y))) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    ++n;
                }
            }
        }
        return std::array<int, 3>{left, right, n};
    };
    CaptionFrame f = frameOf("AAAA BBBB", style);
    CHECK(columnsOf(f)[2] == 0); // no bar: no magenta

    f.bar.toBegin = 0;
    f.bar.toEnd = 4;
    const auto first = columnsOf(f);
    f.bar.toBegin = 5;
    f.bar.toEnd = 9;
    const auto second = columnsOf(f);
    CHECK(first[2] > 300);
    CHECK(second[2] > 300);
    CHECK(second[0] > first[0] + 20);             // the second word's bar is to the right of the first's
    CHECK(std::abs((first[1] - first[0]) - (second[1] - second[0])) < 24); // words of about one width: bars of about one width

    // Sliding: half way from the first word to the second, the bar is between them.
    f.bar.fromBegin = 0;
    f.bar.fromEnd = 4;
    f.bar.t = 0.5;
    const auto sliding = columnsOf(f);
    CHECK(sliding[0] > first[0] + 10);
    CHECK(sliding[0] < second[0] - 10);
    // At t = 0 it is where it came from; at 1, where it goes.
    f.bar.t = 0.0;
    CHECK(std::abs(columnsOf(f)[0] - first[0]) <= 1);
    f.bar.t = 1.0;
    CHECK(std::abs(columnsOf(f)[0] - second[0]) <= 1);
    // The bar's opacity: fading in.
    f.bar.opacity = 0.0;
    CHECK(columnsOf(f)[2] == 0);
}

// ---- card 073: right-to-left scripts ------------------------------------------------------------------------------------

TEST_CASE("Captions: Hebrew and Arabic are shaped with nothing missing and read right to left", "[render][captions][rtl]") {
    CaptionStyle style = openStyle();
    // Hebrew "shalom." - in a right-to-left paragraph the full stop ends the line on its left.
    const auto hebrew = CaptionRenderer::renderUncached(frameOf("שלום.", style));
    INFO("fonts used: " << (hebrew->info().fonts.empty() ? std::string("none") : hebrew->info().fonts.front()));
    CHECK(hebrew->info().missingGlyphs == 0);
    const auto [left, right] = inkColumns(*hebrew, 0, kH);
    REQUIRE(right > left);
    const int sliver = left + 3; // the first three columns of ink
    const auto [overallTop, overallBottom] = inkRows(*hebrew, left, right);
    const auto [sliverTop, sliverBottom] = inkRows(*hebrew, left, sliver);
    REQUIRE(sliverBottom >= sliverTop);
    // The leftmost ink is the small full stop at the baseline, not a tall letter.
    CHECK((sliverBottom - sliverTop) * 10 < (overallBottom - overallTop) * 4);

    // The same letters without the full stop have no small mark anywhere near the left edge.
    const auto bare = CaptionRenderer::renderUncached(frameOf("שלום", style));
    const auto [bareLeft, bareRight] = inkColumns(*bare, 0, kH);
    const auto [bareTop, bareBottom] = inkRows(*bare, bareLeft, bareRight);
    const auto [bareSliverTop, bareSliverBottom] = inkRows(*bare, bareLeft, bareLeft + 3);
    CHECK((bareSliverBottom - bareSliverTop) * 10 >= (bareBottom - bareTop) * 4);

    // Arabic, with its comma: shaped, nothing missing, centred like any caption.
    const auto arabic = CaptionRenderer::renderUncached(frameOf("مرحبا، كيف حالك", style));
    CHECK(arabic->info().missingGlyphs == 0);
    const auto [arLeft, arRight] = inkColumns(*arabic, 0, kH);
    REQUIRE(arRight > arLeft);
    CHECK(std::abs((arLeft + arRight) / 2.0 - kW / 2.0) < 12.0);
}
