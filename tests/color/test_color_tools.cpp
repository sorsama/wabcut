// Auto and Color Match on synthetic pictures: Auto recovers neutral grey under colour casts even when one colour
// dominates the frame; Color Match moves a graded copy back to the reference's statistics; skin priority and weights.

#include <wabcut/color/ColorTools.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

using namespace wabcut;
using namespace wabcut::color;
using Catch::Approx;

namespace {

struct Patch {
    Rgb linear;
    double share;
    bool grey;
};

/// A scene: grey patches, dominant grass, sky, skin and a red object (linear light), with a little noise.
std::vector<Patch> scene() {
    return {{{0.03, 0.03, 0.03}, 0.04, true},  {{0.09, 0.09, 0.09}, 0.05, true}, {{0.18, 0.18, 0.18}, 0.06, true},
            {{0.35, 0.35, 0.35}, 0.05, true},  {{0.62, 0.62, 0.62}, 0.04, true}, {{0.08, 0.25, 0.05}, 0.45, false},
            {{0.25, 0.35, 0.6}, 0.15, false},  {{0.45, 0.3, 0.22}, 0.10, false}, {{0.5, 0.08, 0.06}, 0.06, false}};
}

/// Samples of the scene under `cast` (per-channel gains in linear light), display-referred.
std::vector<ColorSample> samplesOf(const Rgb& cast, int count = 3000, const GradeSpec* grade = nullptr) {
    std::mt19937 rng(5);
    std::normal_distribution<double> noise(0.0, 0.004);
    std::vector<ColorSample> out;
    for (const Patch& p : scene()) {
        const int n = static_cast<int>(p.share * count);
        for (int i = 0; i < n; ++i) {
            Rgb c = p.linear * (1.0 + noise(rng));
            c = c * cast;
            if (grade) {
                c = gradePixel(*grade, c);
            }
            out.push_back({encode709(c), 1.0});
        }
    }
    return out;
}

double chromaOf(const Rgb& linear) {
    const Rgb lab = linearToOklab(linear);
    return std::hypot(lab[1], lab[2]);
}

/// The worst Oklab chroma of the grey patches under `cast`, graded by `spec`.
double greyError(const GradeSpec& spec, const Rgb& cast) {
    double worst = 0.0;
    for (const Patch& p : scene()) {
        if (p.grey && p.linear[0] > 0.05) {
            worst = std::max(worst, chromaOf(gradePixel(spec, p.linear * cast)));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("Auto: neutral grey back from warm, cool, green and magenta casts with grass dominating", "[color][tools][auto]") {
    for (const Rgb cast : {Rgb{1.25, 1.0, 0.72}, Rgb{0.82, 1.0, 1.3}, Rgb{0.95, 1.18, 0.95}, Rgb{1.1, 0.85, 1.1}}) {
        INFO("cast " << cast[0] << " " << cast[1] << " " << cast[2]);
        const GradeSpec base;
        const double before = greyError(base, cast);
        const AutoResult result = autoCorrect(samplesOf(cast), base);
        INFO(result.summary);
        GradeSpec graded;
        graded.params = result.params;
        const double after = greyError(graded, cast);
        CHECK(before > 0.03);
        CHECK(after < 0.012);
        CHECK(after < before / 3.0);
        // Levels: highlights near 92 %, blacks not lifted, the controls not pinned at their limits.
        const auto percentiles = [&](const GradeSpec& spec) {
            std::vector<double> lumas;
            for (const ColorSample& s : samplesOf(cast)) {
                lumas.push_back(luma(encode709(gradePixel(spec, decode709(s.display)))));
            }
            std::sort(lumas.begin(), lumas.end());
            return std::pair{lumas[lumas.size() * 5 / 1000], lumas[lumas.size() * 995 / 1000]};
        };
        const auto [lo0, hi0] = percentiles(GradeSpec{});
        const auto [lo1, hi1] = percentiles(graded);
        CHECK(hi1 == Approx(0.92).margin(0.04));
        CHECK(lo1 < std::max(lo0, 0.12) + 0.03); // brighter overall, but the darkest stays dark
        CHECK(std::abs(result.params.whites) < 100);
        CHECK(std::abs(result.params.blacks) < 100);
    }
    // A neutral, well exposed scene barely moves.
    const AutoResult neutral = autoCorrect(samplesOf({1, 1, 1}), GradeSpec{});
    CHECK(std::abs(neutral.params.temperature) < 6.0);
    CHECK(std::abs(neutral.params.tint) < 6.0);
}

TEST_CASE("Color Match: a graded copy goes back to the reference's look", "[color][tools][match]") {
    GradeSpec damage;
    damage.params.exposure = -0.8;
    damage.params.temperature = 35;
    damage.params.saturation = 70;
    damage.params.contrast = -20;
    const auto reference = samplesOf({1, 1, 1});
    const auto current = samplesOf({1, 1, 1}, 3000, &damage);
    const MatchResult result = colorMatch(current, reference, GradeSpec{});
    INFO(result.summary);
    for (std::size_t c = 0; c < 3; ++c) {
        INFO("channel " << c);
        CHECK(result.achieved.mean[c] == Approx(result.reference.mean[c]).margin(c == 0 ? 0.01 : 0.004));
        CHECK(result.achieved.deviation[c] == Approx(result.reference.deviation[c]).epsilon(0.1).margin(0.003));
    }
    // It found roughly the inverse of the damage.
    CHECK(result.params.exposure > 0.4);
    CHECK(result.params.temperature < -10);
    CHECK(result.params.saturation > 110);
    // Strength 0 leaves the clip as it is; strength 0.5 lands in between.
    MatchOptions none;
    none.strength = 0.0;
    const MatchResult unchanged = colorMatch(current, reference, GradeSpec{}, none);
    CHECK(unchanged.achieved.mean[0] == Approx(unchanged.current.mean[0]).margin(0.003));
    MatchOptions half;
    half.strength = 0.5;
    const MatchResult between = colorMatch(current, reference, GradeSpec{}, half);
    CHECK(between.achieved.mean[0] > result.current.mean[0]);
    CHECK(between.achieved.mean[0] < result.achieved.mean[0]);
}

TEST_CASE("Color Match: skin priority and region weights change what counts", "[color][tools][match]") {
    CHECK(skinLikelihood(encode709({0.45, 0.3, 0.22})) > 0.5);
    CHECK(skinLikelihood(encode709({0.08, 0.25, 0.05})) == 0.0);
    CHECK(skinLikelihood(encode709({0.25, 0.35, 0.6})) == 0.0);
    // Weights: only the first half of the current samples counts.
    GradeSpec damage;
    damage.params.temperature = 40;
    const auto reference = samplesOf({1, 1, 1});
    const auto current = samplesOf({1, 1, 1}, 3000, &damage);
    MatchOptions options;
    options.skinPriority = true;
    options.currentWeights.assign(current.size(), 1.0);
    const MatchResult weighted = colorMatch(current, reference, GradeSpec{}, options);
    CHECK(weighted.params.temperature < -10);
    CHECK(std::isfinite(weighted.achieved.mean[0]));
    // Empty inputs are reported, not crashed on.
    CHECK(colorMatch({}, reference, GradeSpec{}).summary == "no pixels to match");
    CHECK(autoCorrect({}, GradeSpec{}).summary == "no pixels to analyse");
}

TEST_CASE("minimize finds a bounded quadratic's minimum", "[color][tools]") {
    const auto best = minimize([](const std::vector<double>& v) { return std::pow(v[0] - 2.0, 2) + 3.0 * std::pow(v[1] + 1.0, 2); },
                               {0.0, 0.0}, {1.0, 1.0}, {-5.0, -5.0}, {5.0, 5.0}, 400);
    CHECK(best[0] == Approx(2.0).margin(1e-3));
    CHECK(best[1] == Approx(-1.0).margin(1e-3));
    const auto clamped = minimize([](const std::vector<double>& v) { return std::pow(v[0] - 9.0, 2); }, {0.0}, {1.0}, {-5.0}, {5.0}, 200);
    CHECK(clamped[0] == Approx(5.0));
}

TEST_CASE("Color Match face priority: the faces' skin counts most, hair and background in the box less (0.2)", "[color][tools][match]") {
    // A 100 x 50 picture: blue background, a skin-toned face at the centre with a dark (hair) band on its top.
    ColorImage image;
    image.width = 100;
    image.height = 50;
    image.rgba.resize(100 * 50 * 4);
    const Rgb skin = encode709({0.45, 0.3, 0.22});
    const Rgb hair = encode709({0.03, 0.02, 0.02});
    const Rgb blue = encode709({0.05, 0.1, 0.5});
    for (int y = 0; y < 50; ++y) {
        for (int x = 0; x < 100; ++x) {
            const bool inFace = x >= 40 && x < 60 && y >= 10 && y < 40;
            const Rgb c = inFace ? (y < 15 ? hair : skin) : blue;
            float* p = image.rgba.data() + (static_cast<std::size_t>(y) * 100 + x) * 4;
            p[0] = static_cast<float>(c[0]), p[1] = static_cast<float>(c[1]), p[2] = static_cast<float>(c[2]), p[3] = 1.0f;
        }
    }
    CHECK(faceWeights(image, {}).empty());
    const std::vector<FaceBox> faces{FaceBox{0.4, 0.2, 0.2, 0.6}};
    const std::vector<float> w = faceWeights(image, faces, 12.0);
    REQUIRE(w.size() == 100 * 50);
    const auto at = [&](int x, int y) { return w[static_cast<std::size_t>(y) * 100 + x]; };
    CHECK(at(5, 5) == Approx(1.0));     // background
    CHECK(at(50, 25) == Approx(12.0));  // skin at the centre: the full weight
    CHECK(at(50, 12) < 5.0);            // hair inside the box counts less
    CHECK(at(50, 12) > 1.0);
    CHECK(at(41, 11) == Approx(1.0));   // the box's corner lies outside the ellipse
    // Through the samples, the face now outweighs the background that covers far more of the frame.
    const auto samples = samplePixels(image, 5000, &w);
    double faceWeight = 0.0, rest = 0.0;
    for (const ColorSample& s : samples) {
        (skinLikelihood(s.display) > 0.5 ? faceWeight : rest) += s.weight;
    }
    CHECK(faceWeight > rest);
}
