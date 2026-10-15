// AI-14 matte masks in the compositor (render/Matte.h): a matte mask's distance field, from the installed MatteSource,
// is the shape's distance texture. GPU coverage against the CPU reference (render::maskCoverageCpu) with feather, a
// choke, invert, opacity, the add / subtract / intersect combination and a transform; and the fallbacks: without a matte
// the bounding box (effects), and for redactions the bounding box grown by a quarter (fail closed: never nothing).

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/render/Compositor.h>
#include <wabcut/render/Mask.h>
#include <wabcut/render/Matte.h>

#include <QDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <string>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kW = 480;
constexpr int kH = 270;
constexpr double kAspect = static_cast<double>(kW) / kH;

/// Mattes by track: a fixed field per track (null = none).
class FixedMattes final : public MatteSource {
public:
    std::shared_ptr<const MatteField> fieldAt(const MaskMatte& matte, Ticks, double, MatteUse use) override {
        std::lock_guard lock(mutex);
        ++calls;
        lastUse = use;
        const auto it = fields.find(matte.track);
        return it == fields.end() ? nullptr : it->second;
    }
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const MatteField>> fields;
    int calls = 0;
    MatteUse lastUse = MatteUse::Effect;
};

/// Installs a source for the test's duration.
struct Installed {
    explicit Installed(std::shared_ptr<MatteSource> s) { setMatteSource(std::move(s)); }
    ~Installed() { setMatteSource(nullptr); }
};

/// A person-like matte: a head over shoulders (the union of a circle and a rounded box), as an exact distance field in
/// frame heights at `w` x `h` texels.
std::shared_ptr<MatteField> personField(int w, int h, std::uint64_t identity) {
    auto f = std::make_shared<MatteField>();
    f->width = w;
    f->height = h;
    f->identity = identity;
    f->distance.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double px = (x + 0.5) / w * kAspect;
            const double py = (y + 0.5) / h;
            const double head = 0.14 - std::hypot(px - 0.5 * kAspect, py - 0.32);
            // A box 0.5 wide (frame heights) and 0.4 high below the head, corners rounded by 0.08.
            const double qx = std::abs(px - 0.5 * kAspect) - (0.25 - 0.08);
            const double qy = std::abs(py - 0.72) - (0.2 - 0.08);
            const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - 0.08;
            f->distance[static_cast<std::size_t>(y) * w + x] = static_cast<float>(std::max(head, -outside));
        }
    }
    return f;
}

Mask matteMask(const std::string& track, std::initializer_list<std::pair<const char*, double>> values = {}, MaskMode mode = MaskMode::Add,
               bool inverted = false) {
    Mask m;
    m.shape = MaskShape::Matte;
    m.matte = MaskMatte{track, MediaId{1}, 0, TimeRange{Ticks{}, Ticks::fromSeconds(10)}, "test-matte"};
    // The bounding box: the person spans x 0.5 +- 0.25 / aspect, y 0.18 .. 0.92.
    m.set("center.x", 0.5);
    m.set("center.y", 0.55);
    m.set("size.x", 0.5 / kAspect);
    m.set("size.y", 0.74);
    for (const auto& [key, value] : values) {
        m.set(key, value);
    }
    m.mode = mode;
    m.inverted = inverted;
    return m;
}

Mask ellipse(double cx, double cy, double w, double h) {
    Mask m;
    m.shape = MaskShape::Ellipse;
    m.set("center.x", cx);
    m.set("center.y", cy);
    m.set("size.x", w);
    m.set("size.y", h);
    return m;
}

LinearImage coverageImage(const MaskSpec& spec) {
    const std::vector<float> c = maskCoverageCpu(spec, kW, kH);
    LinearImage out(kW, kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const double v = c[static_cast<std::size_t>(y) * kW + x];
            out.at(x, y) = {v, v, v, v};
        }
    }
    return out;
}

double coverageAt(const MaskSpec& spec, double x, double y) {
    const std::vector<float> c = maskCoverageCpu(spec, kW, kH);
    const int px = std::clamp(static_cast<int>(x * kW), 0, kW - 1);
    const int py = std::clamp(static_cast<int>(y * kH), 0, kH - 1);
    return c[static_cast<std::size_t>(py) * kW + px];
}

} // namespace

TEST_CASE("Matte masks: GPU coverage matches the CPU reference (feather, choke, invert, combination, transform)",
          "[render][gpu][masks][matte]") {
    auto mattes = std::make_shared<FixedMattes>();
    mattes->fields["person"] = personField(240, 135, 11); // half the coverage resolution: bilinear on both sides
    Installed installed(mattes);
    QDir().mkpath(outputDir() + QStringLiteral("/masks"));
    auto renderer = makeRenderer(OutputRole::Export);
    struct Case {
        std::string name;
        std::vector<Mask> masks;
    };
    const std::vector<Case> cases = {
        {"matte", {matteMask("person")}},
        {"matte_feather", {matteMask("person", {{"feather", 0.05}})}},
        {"matte_choke", {matteMask("person", {{"expansion", -0.03}, {"feather", 0.02}})}},
        {"matte_grow", {matteMask("person", {{"expansion", 0.04}})}},
        {"matte_inverted_opacity", {matteMask("person", {{"opacity", 0.6}, {"feather", 0.02}}, MaskMode::Add, true)}},
        {"matte_minus_ellipse", {matteMask("person"), [] {
                                     Mask e = ellipse(0.5, 0.32, 0.12, 0.2);
                                     e.mode = MaskMode::Subtract;
                                     return e;
                                 }()}},
        {"ellipse_intersect_matte", {ellipse(0.5, 0.6, 0.6, 0.5), matteMask("person", {}, MaskMode::Intersect)}},
        {"matte_moved", {matteMask("person", {{"transform.x", 0.12}, {"transform.y", -0.04}, {"transform.scaleX", 0.8},
                                              {"transform.scaleY", 0.8}, {"feather", 0.02}})}},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const auto spec = maskSpecAt(c.masks, Ticks{}, kAspect);
            REQUIRE(spec);
            bool hasMatte = false;
            for (const MaskShapeSpec& s : spec->shapes) {
                hasMatte = hasMatte || s.matte != nullptr;
            }
            REQUIRE(hasMatte);
            CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, [](int, int) { return std::array<std::uint8_t, 4>{255, 255, 255, 255}; }));
            LayerEffect e;
            e.type = EffectType::OpacityMask;
            e.mask = spec;
            layer.effects = {e};
            const auto composite = compositeOf({kW, kH}, {layer});
            auto image = renderer->render(composite);
            REQUIRE(image);
            const LinearImage expected = coverageImage(*spec);
            const std::vector<std::uint8_t> reference = encodeOverBlack(expected, outputTransformFor(composite->color, OutputRole::Export));
            savePng(*image, QStringLiteral("masks/%1.png").arg(QString::fromStdString(c.name)));
            const double psnr = psnrRgb(*image, reference);
            WARN(c.name << ": PSNR " << psnr << " dB");
            CHECK(psnr >= 38.0);
        }
    }
}

TEST_CASE("Matte masks: the matte where there is one; the bounding box where there is none; redactions fail closed",
          "[render][masks][matte]") {
    auto mattes = std::make_shared<FixedMattes>();
    mattes->fields["person"] = personField(240, 135, 21);
    Installed installed(mattes);

    // With a matte: its shape, not its box (the box's corner beside the head is outside the person).
    const auto withMatte = maskSpecAt(std::vector<Mask>{matteMask("person")}, Ticks{}, kAspect);
    REQUIRE(withMatte);
    REQUIRE(withMatte->shapes.front().matte);
    CHECK(mattes->lastUse == MatteUse::Effect);
    CHECK(coverageAt(*withMatte, 0.5, 0.32) > 0.99);  // the head
    CHECK(coverageAt(*withMatte, 0.5, 0.75) > 0.99);  // the shoulders
    CHECK(coverageAt(*withMatte, 0.38, 0.22) < 0.01); // inside the box, beside the head
    // Its identity is part of the cache keys: another matte (the next frame) is another texture.
    mattes->fields["person"] = personField(240, 135, 22);
    const auto nextFrame = maskSpecAt(std::vector<Mask>{matteMask("person")}, Ticks{}, kAspect);
    CHECK(nextFrame->key != withMatte->key);
    CHECK(nextFrame->shapes.front().key != withMatte->shapes.front().key);

    // No matte at the frame (not propagated there, another computer): an effect mask shows the bounding box.
    const auto box = maskSpecAt(std::vector<Mask>{matteMask("missing")}, Ticks{}, kAspect);
    REQUIRE(box);
    CHECK_FALSE(box->shapes.front().matte);
    CHECK(coverageAt(*box, 0.38, 0.22) > 0.99); // the box's corner
    CHECK(coverageAt(*box, 0.5, 0.1) < 0.01);   // above the box

    // A redaction: the box grown by a quarter of its larger side (12.5 % each way): above the box is covered too.
    const auto redaction = maskSpecAt(std::vector<Mask>{matteMask("missing")}, Ticks{}, kAspect, MatteUse::Redaction);
    REQUIRE(redaction);
    CHECK(mattes->lastUse == MatteUse::Redaction);
    CHECK(redaction->shapes.front().expansion > 0.09);
    CHECK(coverageAt(*redaction, 0.5, 0.12) > 0.99);
    CHECK(coverageAt(*redaction, 0.38, 0.22) > 0.99);
    // No source installed at all: the same fail-closed box.
    setMatteSource(nullptr);
    const auto noSource = maskSpecAt(std::vector<Mask>{matteMask("person")}, Ticks{}, kAspect, MatteUse::Redaction);
    REQUIRE(noSource);
    CHECK(coverageAt(*noSource, 0.5, 0.12) > 0.99);
}

// AI-14 review, finding 1 (privacy): a matte field that is broken (the wrong size, a non-finite distance) used to reach
// the CPU coverage, which then covered NOTHING (library thumbnails and filmstrips go through it: the face showed),
// while the GPU fell back to the bounding box. Both now treat it as no matte: a redaction covers its bounding box grown
// by a quarter; an effect its bounding box. Finding 5: a matte shape without a matte reference is grown the same way.
TEST_CASE("Matte redactions fail closed on the CPU path too: a broken field or a missing reference is the grown box",
          "[render][masks][matte]") {
    auto mattes = std::make_shared<FixedMattes>();
    auto wrongSize = std::make_shared<MatteField>();
    wrongSize->width = 64;
    wrongSize->height = 36;
    wrongSize->identity = 3;
    wrongSize->distance.assign(10, 1.0f);
    mattes->fields["wrong-size"] = wrongSize;
    auto notFinite = personField(240, 135, 5);
    notFinite->distance[100] = std::numeric_limits<float>::quiet_NaN();
    mattes->fields["not-finite"] = notFinite;
    Installed installed(mattes);
    for (const char* track : {"wrong-size", "not-finite"}) {
        INFO(track);
        const auto redaction = maskSpecAt(std::vector<Mask>{matteMask(track)}, Ticks{}, kAspect, MatteUse::Redaction);
        REQUIRE(redaction);
        CHECK(coverageAt(*redaction, 0.5, 0.55) > 0.99);  // the box's centre
        CHECK(coverageAt(*redaction, 0.38, 0.22) > 0.99); // the box's corner
        CHECK(coverageAt(*redaction, 0.5, 0.12) > 0.99);  // above the box: grown
        const auto effect = maskSpecAt(std::vector<Mask>{matteMask(track)}, Ticks{}, kAspect);
        REQUIRE(effect);
        CHECK(coverageAt(*effect, 0.38, 0.22) > 0.99); // an effect: the box itself
        CHECK(coverageAt(*effect, 0.5, 0.1) < 0.01);
    }
    CHECK_FALSE(usableField(*wrongSize));
    CHECK_FALSE(usableField(*notFinite));
    CHECK(usableField(*personField(24, 14, 6)));
    // The half floats a source can hand over (MatteField::half): IEEE binary16, round to nearest even.
    CHECK(toHalf(0.0f) == 0x0000);
    CHECK(toHalf(1.0f) == 0x3C00);
    CHECK(toHalf(-2.0f) == 0xC000);
    CHECK(toHalf(0.1f) == 0x2E66);
    CHECK(toHalf(65504.0f) == 0x7BFF);
    CHECK(toHalf(1.0e6f) == 0x7C00);
    CHECK(toHalf(std::numeric_limits<float>::infinity()) == 0x7C00);
    // A matte shape that lost its matte reference (a hand-edited file, an older version): a redaction still grows.
    Mask lost = matteMask("person");
    lost.matte.reset();
    const auto noReference = maskSpecAt(std::vector<Mask>{lost}, Ticks{}, kAspect, MatteUse::Redaction);
    REQUIRE(noReference);
    CHECK(coverageAt(*noReference, 0.5, 0.12) > 0.99);
}
