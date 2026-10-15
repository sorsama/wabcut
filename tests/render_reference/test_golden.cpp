// The golden-file format (Image.h): round trips, the parser's checks, comparison, and the golden check's mismatch path
// (the new picture written to the output folder, the update switch rewriting the golden).

#include "ReferenceTestSupport.h"

#include <wabcut/render/reference/Image.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

using namespace wabcut;
using namespace wabcut::render::reference;
using Catch::Approx;

namespace {

Image sample() {
    Image out(3, 2);
    out.set(0, 0, {0.0f, 0.5f, 1.0f, 1.0f});
    out.set(1, 0, {0.123456789f, 1e-7f, -0.0f, 0.75f});
    out.set(2, 0, {2.5f, 1234.567f, 0.333333343f, 0.5f});
    out.set(0, 1, {-0.25f, 0.0f, 0.0f, 0.0f});
    out.set(1, 1, {0.2140411f, 0.2597194f, 0.9999999f, 1.0f});
    out.set(2, 1, {1.0f, 1.0f, 1.0f, 1.0f});
    return out;
}

std::filesystem::path scratch(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "render_reference" / "golden_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

} // namespace

TEST_CASE("Golden files: text round trip", "[render_reference][golden]") {
    const Image image = sample();
    const std::string text = goldenText(image);
    CHECK(text.rfind("WRI1 3 2\n0 0.5 1 1\n0.123457 1e-07 0 0.75\n", 0) == 0); // %.6g, -0 as 0
    const auto back = parseGolden(text);
    REQUIRE(back.has_value());
    CHECK(back->width == 3);
    CHECK(back->height == 2);
    const Difference d = compare(*back, image);
    CHECK(d.maxAbs <= 0.01); // 1234.567 prints as 1234.57
    for (std::size_t i = 0; i < image.rgba.size(); ++i) {
        const double v = static_cast<double>(image.rgba[i]);
        CHECK(static_cast<double>(back->rgba[i]) == Approx(v).epsilon(5e-6).margin(1e-12));
    }
    CHECK(goldenText(*back) == text); // the text is a fixed point
    // Through a file, with CRLF line ends too.
    const std::filesystem::path file = scratch("round_trip.wri");
    REQUIRE(writeGolden(file, image));
    const auto read = readGolden(file);
    REQUIRE(read.has_value());
    CHECK(*read == *back);
    std::string crlf;
    for (const char c : text) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    const auto fromCrlf = parseGolden(crlf);
    REQUIRE(fromCrlf.has_value());
    CHECK(*fromCrlf == *back);
}

TEST_CASE("Golden files: the parser rejects malformed text", "[render_reference][golden]") {
    CHECK_FALSE(parseGolden("").has_value());
    CHECK_FALSE(parseGolden("PFM 1 1\n0 0 0 0\n").has_value());
    CHECK_FALSE(parseGolden("WRI1 0 1\n").has_value());
    CHECK_FALSE(parseGolden("WRI1 1 1\n0 0 0\n").has_value());          // too few values
    CHECK_FALSE(parseGolden("WRI1 1 1\n0 0 0 0 0\n").has_value());      // too many
    CHECK_FALSE(parseGolden("WRI1 1 1\n0 0 nan 0\n").has_value());      // not finite
    CHECK_FALSE(parseGolden("WRI1 1 1\n0 0 0x1 0\n").has_value());      // not a whole number token
    CHECK_FALSE(parseGolden("WRI1 99999 1\n").has_value());             // too large
    CHECK_FALSE(parseGolden("WRI1 4096 4096\n0 0 0 0\n").has_value());  // a large header without its data
    CHECK(parseGolden("WRI1 1 1 0.5 0.25 0 1").has_value());            // any whitespace
    const auto missing = readGolden(scratch("does_not_exist.wri"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK_FALSE(writeGolden(scratch("empty.wri"), Image{}));
    Image bad(1, 1);
    bad.rgba[0] = std::numeric_limits<float>::infinity();
    CHECK_FALSE(writeGolden(scratch("bad.wri"), bad));
}

TEST_CASE("Golden files: compare gives max abs and PSNR", "[render_reference][golden]") {
    const Image a = Image::filled(2, 2, {0.5f, 0.5f, 0.5f, 1.0f});
    const Difference same = compare(a, a);
    CHECK(same.maxAbs == 0.0);
    CHECK(std::isinf(same.psnr));
    Image b = a;
    b.rgba[0] = 0.6f; // one channel of 16 off by 0.1: MSE 0.01 / 16
    const Difference d = compare(a, b);
    CHECK(d.maxAbs == Approx(0.1).margin(1e-6));
    CHECK(d.psnr == Approx(10.0 * std::log10(16.0 / 0.01)).epsilon(1e-4));
    CHECK(std::isinf(compare(a, Image(2, 3)).maxAbs));
}

TEST_CASE("Golden files: a mismatch writes the new picture and fails", "[render_reference][golden]") {
    const std::filesystem::path goldens = scratch("goldens");
    const std::filesystem::path output = scratch("output");
    std::filesystem::remove_all(goldens);
    std::filesystem::remove_all(output);
    Image image = sample();
    image.rgba[9] = 0.5f; // values of a picture (1234.567 would round by more than the tolerance in six digits)
    // No golden yet: a failure, and the picture is written for review.
    test::GoldenCheck missing = test::compareWithGolden("case", image, goldens, output, false);
    CHECK_FALSE(missing.matches);
    CHECK(missing.message.find("not found") != std::string::npos);
    CHECK(std::filesystem::exists(output / "case.wri"));
    // The update switch writes the golden; then it matches and nothing is written.
    std::filesystem::remove_all(output);
    const test::GoldenCheck updated = test::compareWithGolden("case", image, goldens, output, true);
    CHECK(updated.matches);
    CHECK(updated.written.empty());
    CHECK_FALSE(std::filesystem::exists(output / "case.wri"));
    CHECK(test::compareWithGolden("case", image, goldens, output, false).matches);
    // A different picture: a failure naming the difference, the new picture written next to the old golden.
    Image changed = image;
    changed.rgba[5] += 0.01f;
    const test::GoldenCheck differs = test::compareWithGolden("case", changed, goldens, output, false);
    CHECK_FALSE(differs.matches);
    CHECK(differs.difference.maxAbs == Approx(0.01).margin(1e-5));
    CHECK(differs.message.find("differs") != std::string::npos);
    const auto written = readGolden(differs.written);
    REQUIRE(written.has_value());
    CHECK(compare(*written, changed).maxAbs <= test::kGoldenTolerance);
    const auto kept = readGolden(goldens / "case.wri");
    REQUIRE(kept.has_value());
    CHECK(compare(*kept, image).maxAbs <= test::kGoldenTolerance); // the golden is kept
}
