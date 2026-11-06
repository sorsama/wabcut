// Face-aware Morph Cut (0.7, FaceAlign.h): the similarity from landmarks, face matching and the face-aligned flow on a
// synthetic talking head with exact truth.

#include "SyntheticFace.h"

#include <wabcut/motion/FaceAlign.h>
#include <wabcut/motion/Flow.h>
#include <wabcut/motion/Interpolate.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>

using namespace wabcut::motion;
using wabcut::motion::test::HeadPose;
using wabcut::motion::test::midPose;
using wabcut::motion::test::rgbaOf;
using wabcut::motion::test::SyntheticFace;

namespace {

constexpr int kW = 480;
constexpr int kH = 270;

/// The mean absolute difference over a box (in picture pixels) between two pictures.
double meanDifference(const RgbaImage& a, const RgbaImage& b, double x0, double y0, double x1, double y1) {
    double sum = 0.0;
    int n = 0;
    for (int y = std::max(0, static_cast<int>(y0)); y < std::min(a.height, static_cast<int>(y1)); ++y) {
        for (int x = std::max(0, static_cast<int>(x0)); x < std::min(a.width, static_cast<int>(x1)); ++x) {
            sum += std::abs(a.px[4 * (static_cast<std::size_t>(y) * a.width + x)] - b.px[4 * (static_cast<std::size_t>(y) * a.width + x)]);
            ++n;
        }
    }
    return n > 0 ? sum / n : 0.0;
}

/// A jump cut of a talking head: it moved, grew, turned and its mouth opened.
struct Cut {
    HeadPose a;
    HeadPose b;
    SyntheticFace face;
    Plane pa;
    Plane pb;
    FlowPair plain;
    explicit Cut(const HeadPose& from, const HeadPose& to) : a(from), b(to), pa(face.picture(kW, kH, from)), pb(face.picture(kW, kH, to)) {
        plain = estimateFlowPair(pa, pb, analysisFlowOptions());
    }
    FaceFeatures featuresA() const { return SyntheticFace::features(a); }
    FaceFeatures featuresB() const { return SyntheticFace::features(b); }
    /// Truth, cross dissolve, plain flow and face-aligned flow side by side (a PPM for people to look at).
    void save(const std::string& name, const FlowPair& aligned) const {
        const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "motion";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const RgbaImage truth = rgbaOf(face.picture(kW, kH, midPose(a, b, 0.5)));
        const RgbaImage pics[4] = {truth, blend(rgbaOf(pa), rgbaOf(pb), 0.5), interpolate(rgbaOf(pa), rgbaOf(pb), plain, 0.5),
                                   interpolate(rgbaOf(pa), rgbaOf(pb), aligned, 0.5)};
        std::ofstream out(dir / (name + ".ppm"), std::ios::binary);
        out << "P6\n" << kW * 4 << " " << kH << "\n255\n";
        for (int y = 0; y < kH; ++y) {
            for (const RgbaImage& p : pics) {
                for (int x = 0; x < kW; ++x) {
                    const auto v = static_cast<unsigned char>(std::clamp(p.px[4 * (static_cast<std::size_t>(y) * kW + x)], 0.0f, 1.0f) * 255.0f + 0.5f);
                    out.put(static_cast<char>(v)).put(static_cast<char>(v)).put(static_cast<char>(v));
                }
            }
        }
    }
    /// The mid-point picture along `flow` and its error against the true mid-point head, over the face.
    double midError(const FlowPair& flow) const {
        const RgbaImage mid = interpolate(rgbaOf(pa), rgbaOf(pb), flow, 0.5);
        const RgbaImage truth = rgbaOf(face.picture(kW, kH, midPose(a, b, 0.5)));
        const HeadPose m = midPose(a, b, 0.5);
        return meanDifference(mid, truth, m.cx - m.halfWidth, m.cy - 1.25 * m.halfWidth, m.cx + m.halfWidth, m.cy + 1.25 * m.halfWidth);
    }
    double dissolveError() const {
        const RgbaImage mid = blend(rgbaOf(pa), rgbaOf(pb), 0.5);
        const RgbaImage truth = rgbaOf(face.picture(kW, kH, midPose(a, b, 0.5)));
        const HeadPose m = midPose(a, b, 0.5);
        return meanDifference(mid, truth, m.cx - m.halfWidth, m.cy - 1.25 * m.halfWidth, m.cx + m.halfWidth, m.cy + 1.25 * m.halfWidth);
    }
};

} // namespace

TEST_CASE("A similarity from five landmarks recovers the head's shift, turn and scale", "[motion][face]") {
    const HeadPose a{200.0, 120.0, 50.0, 0.0, 0.0};
    const HeadPose b{231.0, 107.0, 56.0, 7.5, 0.0};
    const FaceFeatures fa = SyntheticFace::features(a);
    const FaceFeatures fb = SyntheticFace::features(b);
    const Similarity s = similarityOf(fa.points, fb.points);
    CHECK(s.scale() == Catch::Approx(56.0 / 50.0).epsilon(1e-4));
    CHECK(s.turnDegrees() == Catch::Approx(7.5).margin(1e-3));
    for (std::size_t i = 0; i < fa.points.size(); ++i) {
        const FacePoint m = s.map(fa.points[i]);
        CHECK(m.x == Catch::Approx(fb.points[i].x).margin(1e-2));
        CHECK(m.y == Catch::Approx(fb.points[i].y).margin(1e-2));
    }
    const Similarity back = s.inverted();
    for (std::size_t i = 0; i < fa.points.size(); ++i) {
        const FacePoint m = back.map(fb.points[i]);
        CHECK(m.x == Catch::Approx(fa.points[i].x).margin(1e-2));
        CHECK(m.y == Catch::Approx(fa.points[i].y).margin(1e-2));
    }
    // Degenerate input is the identity.
    const Similarity none = similarityOf(std::span<const FacePoint>{}, std::span<const FacePoint>{});
    CHECK(none.a == 1.0);
    CHECK(none.b == 0.0);
}

TEST_CASE("Faces are matched one to one by position and size", "[motion][face]") {
    const FaceFeatures left = SyntheticFace::features({100.0, 130.0, 40.0, 0.0, 0.0});
    const FaceFeatures right = SyntheticFace::features({380.0, 130.0, 40.0, 0.0, 0.0});
    const FaceFeatures leftMoved = SyntheticFace::features({112.0, 126.0, 42.0, 2.0, 0.0});
    const FaceFeatures rightMoved = SyntheticFace::features({370.0, 134.0, 39.0, 0.0, 0.0});
    const FaceFeatures stranger = SyntheticFace::features({240.0, 60.0, 15.0, 0.0, 0.0}); // far too small a head to be either
    const std::vector<FaceFeatures> first{left, right};
    const std::vector<FaceFeatures> second{rightMoved, leftMoved, stranger};
    const std::vector<FaceMatch> m = matchFaces(first, second);
    REQUIRE(m.size() == 2);
    CHECK(m[0].a == 0);
    CHECK(m[0].b == 1);
    CHECK(m[1].a == 1);
    CHECK(m[1].b == 0);
    // A head that moved by more than itself is somebody else; a head twice the size is too.
    const FaceFeatures far = SyntheticFace::features({100.0 + 140.0, 130.0, 40.0, 0.0, 0.0});
    const FaceFeatures huge = SyntheticFace::features({100.0, 130.0, 95.0, 0.0, 0.0});
    CHECK(matchFaces(std::vector<FaceFeatures>{left}, std::vector<FaceFeatures>{far}).empty());
    CHECK(matchFaces(std::vector<FaceFeatures>{left}, std::vector<FaceFeatures>{huge}).empty());
    CHECK(matchFaces(std::vector<FaceFeatures>{}, second).empty());
}

TEST_CASE("Without a face on both sides the plain flow is returned untouched", "[motion][face]") {
    const Cut cut({200.0, 130.0, 48.0, 0.0, 0.0}, {232.0, 124.0, 52.0, 5.0, 0.6});
    const std::vector<FaceFeatures> one{cut.featuresA()};
    const std::vector<FaceFeatures> none;
    FaceAlignStats stats;
    const FlowPair aOnly = alignFaces(cut.pa, cut.pb, cut.plain, one, none, {}, &stats);
    CHECK(aOnly.data == cut.plain.data);
    CHECK(stats.faces == 0);
    const FlowPair neither = alignFaces(cut.pa, cut.pb, cut.plain, none, none);
    CHECK(neither.data == cut.plain.data);
    // Landmarks that claim the head doubled in size are not believed.
    FaceFeatures liar = cut.featuresB();
    for (FacePoint& p : liar.points) {
        p.x = liar.centreX() + (p.x - liar.centreX()) * 2.2f;
        p.y = liar.centreY() + (p.y - liar.centreY()) * 2.2f;
    }
    const std::vector<FaceFeatures> lying{liar};
    const FlowPair refused = alignFaces(cut.pa, cut.pb, cut.plain, one, lying);
    CHECK(refused.data == cut.plain.data);
    // An empty or mismatched flow is returned as it is.
    const FlowPair empty;
    CHECK(alignFaces(cut.pa, cut.pb, empty, one, std::vector<FaceFeatures>{cut.featuresB()}).empty());
}

TEST_CASE("Face alignment carries a large head move that the plain flow loses (Morph Cut)", "[motion][face]") {
    // The head moves 44 px (a tenth of the picture), turns 6 degrees, grows 8 % and opens its mouth.
    const Cut cut({196.0, 128.0, 48.0, 0.0, 0.0}, {240.0, 120.0, 52.0, 6.0, 0.8});
    const std::vector<FaceFeatures> fa{cut.featuresA()};
    const std::vector<FaceFeatures> fb{cut.featuresB()};
    FaceAlignStats stats;
    const FlowPair aligned = alignFaces(cut.pa, cut.pb, cut.plain, fa, fb, {}, &stats);
    REQUIRE(stats.faces == 1);
    // Inside the window the pictures agree better along the new flow (the error cannot get worse: each pixel keeps the better one).
    CHECK(stats.errorAfter <= stats.errorBefore + 1e-4);
    const double dissolve = cut.dissolveError();
    const double plainError = cut.midError(cut.plain);
    const double alignedError = cut.midError(aligned);
    INFO("dissolve " << dissolve << " plain flow " << plainError << " face aligned " << alignedError);
    cut.save("face_align_large", aligned);
    {
        std::ofstream figures(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "motion" / "face_align_large.txt");
        figures << "mean error over the face at the mid point (0..1): cross dissolve " << dissolve << ", plain flow " << plainError
                << ", face aligned " << alignedError << "; window error before " << stats.errorBefore << " after " << stats.errorAfter << "\n";
    }
    CHECK(alignedError < dissolve * 0.6);       // far better than the cross dissolve that ghosts the face
    CHECK(alignedError <= plainError * 1.02);   // never worse than the plain flow
    // Outside the window (the far corner) nothing changed.
    const std::size_t corner = 4 * (static_cast<std::size_t>(cut.plain.height - 6) * cut.plain.width + 6);
    CHECK(aligned.data[corner] == cut.plain.data[corner]);
    CHECK(aligned.data[corner + 1] == cut.plain.data[corner + 1]);
    CHECK(aligned.data[corner + 2] == cut.plain.data[corner + 2]);
    CHECK(aligned.data[corner + 3] == cut.plain.data[corner + 3]);
}

TEST_CASE("Face alignment improves a small head move with a mouth that opens (Morph Cut)", "[motion][face]") {
    const Cut cut({210.0, 132.0, 48.0, 0.0, 0.1}, {222.0, 128.0, 49.0, 2.0, 0.9});
    const std::vector<FaceFeatures> fa{cut.featuresA()};
    const std::vector<FaceFeatures> fb{cut.featuresB()};
    const FlowPair aligned = alignFaces(cut.pa, cut.pb, cut.plain, fa, fb);
    const double plainError = cut.midError(cut.plain);
    const double alignedError = cut.midError(aligned);
    INFO("plain flow " << plainError << " face aligned " << alignedError << " dissolve " << cut.dissolveError());
    cut.save("face_align_small", aligned);
    CHECK(alignedError <= plainError * 1.02);
    CHECK(alignedError < cut.dissolveError());
}

TEST_CASE("Face alignment gives the same flow on any number of threads", "[motion][face]") {
    const Cut cut({200.0, 130.0, 48.0, 0.0, 0.0}, {236.0, 122.0, 51.0, 4.0, 0.5});
    const std::vector<FaceFeatures> fa{cut.featuresA()};
    const std::vector<FaceFeatures> fb{cut.featuresB()};
    FaceAlignOptions one;
    one.threads = 1;
    FaceAlignOptions many;
    many.threads = 4;
    const FlowPair x = alignFaces(cut.pa, cut.pb, cut.plain, fa, fb, one);
    const FlowPair y = alignFaces(cut.pa, cut.pb, cut.plain, fa, fb, many);
    CHECK(x.data == y.data);
}

TEST_CASE("Face alignment handles faces at the picture's edge and planes of another size than the flow", "[motion][face]") {
    // The head half outside the left edge, and planes twice the flow's size (the face is in the planes' pixels).
    const HeadPose from{30.0, 130.0, 44.0, 0.0, 0.0};
    const HeadPose to{40.0, 126.0, 46.0, 3.0, 0.4};
    const SyntheticFace face;
    const Plane pa = face.picture(kW, kH, from);
    const Plane pb = face.picture(kW, kH, to);
    const FlowPair plain = estimateFlowPair(pa, pb, analysisFlowOptions());
    const std::vector<FaceFeatures> fa{SyntheticFace::features(from)};
    const std::vector<FaceFeatures> fb{SyntheticFace::features(to)};
    const FlowPair edge = alignFaces(pa, pb, plain, fa, fb);
    REQUIRE(edge.data.size() == plain.data.size());
    for (const float v : edge.data) {
        REQUIRE(std::isfinite(v));
    }
    // Planes larger than the pair's size: the same faces scaled.
    const HeadPose bigFrom{from.cx * 2.0, from.cy * 2.0, from.halfWidth * 2.0, 0.0, 0.0};
    const HeadPose bigTo{to.cx * 2.0, to.cy * 2.0, to.halfWidth * 2.0, to.turnDegrees, to.mouthOpen};
    const Plane qa = face.picture(kW * 2, kH * 2, bigFrom);
    const Plane qb = face.picture(kW * 2, kH * 2, bigTo);
    const FlowPair plain2 = estimateFlowPair(qa, qb, analysisFlowOptions());
    const std::vector<FaceFeatures> ga{SyntheticFace::features(bigFrom)};
    const std::vector<FaceFeatures> gb{SyntheticFace::features(bigTo)};
    FaceAlignStats stats;
    const FlowPair scaled = alignFaces(qa, qb, plain2, ga, gb, {}, &stats);
    CHECK(scaled.width == plain2.width);
    CHECK(stats.faces == 1);
}
