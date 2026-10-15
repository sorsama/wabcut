// .cube LUTs: the Adobe 1.0 format and Resolve's shaper form, malformed and fuzzed files (no crash, bounded memory,
// clear errors), and the interpolation: tetrahedral against an independent reference, trilinear exact on linear maps.

#include <wabcut/color/Looks.h>
#include <wabcut/color/Lut.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace wabcut;
using namespace wabcut::color;
using Catch::Approx;

namespace {

/// A 3D LUT of `size` points whose entries are f(r, g, b) of the lattice coordinates in [0, 1].
template <class F>
std::string cube3d(int size, F f, std::string header = {}) {
    std::string out = header + "LUT_3D_SIZE " + std::to_string(size) + "\n";
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                const Rgb v = f(static_cast<double>(r) / (size - 1), static_cast<double>(g) / (size - 1),
                                static_cast<double>(b) / (size - 1));
                char line[96];
                std::snprintf(line, sizeof line, "%.9g %.9g %.9g\n", v[0], v[1], v[2]);
                out += line;
            }
        }
    }
    return out;
}

/// Independent tetrahedral reference: the barycentric weights of the sorted fractions (Kasson et al. 1995), from the
/// lattice corners visited along the path c000 -> axis of the largest fraction -> ... -> c111.
Rgb tetraReference(const Lut& lut, const Rgb& in) {
    const int n = lut.size3d;
    std::array<double, 3> pos{};
    std::array<int, 3> base{};
    std::array<double, 3> frac{};
    for (int c = 0; c < 3; ++c) {
        pos[static_cast<std::size_t>(c)] = std::clamp(in[static_cast<std::size_t>(c)], 0.0, 1.0) * (n - 1);
        base[static_cast<std::size_t>(c)] = std::min(static_cast<int>(pos[static_cast<std::size_t>(c)]), n - 2);
        frac[static_cast<std::size_t>(c)] = pos[static_cast<std::size_t>(c)] - base[static_cast<std::size_t>(c)];
    }
    std::array<int, 3> order{0, 1, 2};
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return frac[static_cast<std::size_t>(a)] > frac[static_cast<std::size_t>(b)]; });
    std::array<int, 3> corner = base;
    Rgb acc = lut.entry3d(corner[0], corner[1], corner[2]) * (1.0 - frac[static_cast<std::size_t>(order[0])]);
    for (int k = 0; k < 3; ++k) {
        ++corner[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])];
        const double next = k < 2 ? frac[static_cast<std::size_t>(order[static_cast<std::size_t>(k + 1)])] : 0.0;
        const double w = frac[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])] - next;
        const Rgb e = lut.entry3d(corner[0], corner[1], corner[2]);
        acc = acc + e * w;
    }
    return acc;
}

} // namespace

TEST_CASE("Cube: 3D with TITLE, comments, CRLF, BOM and a domain", "[color][lut]") {
    std::string text = "\xEF\xBB\xBF# made by a test\r\nTITLE \"Test look\"\r\nDOMAIN_MIN 0 0 0\r\nDOMAIN_MAX 2 2 2\r\n\r\n" +
                       cube3d(3, [](double r, double g, double b) { return Rgb{r * 2, g, b * 0.5}; });
    // CRLF everywhere.
    std::string crlf;
    for (const char c : text) {
        if (c == '\n' && (crlf.empty() || crlf.back() != '\r')) {
            crlf += '\r';
        }
        crlf += c;
    }
    const auto lut = parseCube(crlf);
    INFO((lut ? "" : lut.error().message));
    REQUIRE(lut);
    CHECK(lut->title == "Test look");
    CHECK(lut->size3d == 3);
    CHECK_FALSE(lut->has1d());
    CHECK(lut->domain3dMax == Rgb{2, 2, 2});
    // Input 1.0 is the middle of a [0, 2] domain: entry 1 of 2.
    const Rgb out = applyLut(*lut, {1.0, 1.0, 1.0});
    CHECK(out[0] == Approx(1.0));
    CHECK(out[1] == Approx(0.5));
    CHECK(out[2] == Approx(0.25));
    // Outside the domain: clamped to the edge entries.
    const Rgb clamped = applyLut(*lut, {5.0, -3.0, 2.0});
    CHECK(clamped[0] == Approx(2.0));
    CHECK(clamped[1] == Approx(0.0));
    CHECK(clamped[2] == Approx(0.5));
}

TEST_CASE("Cube: 1D, Resolve's shaper + 3D form, and the round trip", "[color][lut]") {
    std::string oneD = "LUT_1D_SIZE 5\n";
    for (int i = 0; i < 5; ++i) {
        const double x = i / 4.0;
        oneD += std::to_string(x * x) + " " + std::to_string(x) + " " + std::to_string(std::sqrt(x)) + "\n";
    }
    const auto lut1 = parseCube(oneD);
    REQUIRE(lut1);
    CHECK(lut1->size1d == 5);
    CHECK_FALSE(lut1->has3d());
    const Rgb half = applyLut(*lut1, {0.5, 0.5, 0.5});
    CHECK(half[0] == Approx(0.25));
    CHECK(half[1] == Approx(0.5));
    CHECK(half[2] == Approx(std::sqrt(0.5)));
    const Rgb between = applyLut(*lut1, {0.375, 0.375, 0.375}); // halfway between entries 1 and 2
    CHECK(between[0] == Approx((0.0625 + 0.25) / 2));

    // Resolve: LUT_1D_INPUT_RANGE + LUT_3D_INPUT_RANGE, the 1D table first, then the 3D.
    std::string both = "LUT_1D_INPUT_RANGE 0 4\nLUT_3D_INPUT_RANGE 0 1\nLUT_1D_SIZE 2\nLUT_3D_SIZE 2\n0 0 0\n1 1 1\n";
    both += cube3d(2, [](double r, double g, double b) { return Rgb{1 - r, 1 - g, 1 - b}; }).substr(std::string("LUT_3D_SIZE 2\n").size());
    const auto lut2 = parseCube(both);
    INFO((lut2 ? "" : lut2.error().message));
    REQUIRE(lut2);
    CHECK(lut2->has1d());
    CHECK(lut2->has3d());
    CHECK(lut2->domain1dMax[0] == 4.0);
    const Rgb shaped = applyLut(*lut2, {2.0, 2.0, 2.0}); // 1D: 2/4 -> 0.5; 3D: inverted -> 0.5
    CHECK(shaped[0] == Approx(0.5));

    // writeCube -> parseCube is exact.
    for (const auto* lut : {&*lut1, &*lut2}) {
        const auto again = parseCube(writeCube(*lut));
        REQUIRE(again);
        CHECK(again->table1d == lut->table1d);
        CHECK(again->table3d == lut->table3d);
        CHECK(again->domain1dMin == lut->domain1dMin);
        CHECK(again->domain3dMax == lut->domain3dMax);
    }
}

TEST_CASE("Cube: malformed files fail with a clear message", "[color][lut]") {
    struct Bad {
        std::string text;
        std::string expect;
    };
    const std::string header3 = "LUT_3D_SIZE 2\n";
    const std::string eight = "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    const std::vector<Bad> corpus{
        {"", "not a .cube"},
        {"TITLE \"x\"\n", "not a .cube"},
        {"0 0 0\n", "before LUT_1D_SIZE"},
        {"LUT_3D_SIZE 66\n", "too large"},
        {"LUT_3D_SIZE 1\n0 0 0\n", "at least 2"},
        {"LUT_1D_SIZE 70000\n", "too large"},
        {"LUT_3D_SIZE two\n", "whole number"},
        {"LUT_3D_SIZE 2 2\n", "needs one number"},
        {header3 + "0 0 0\n", "data lines"},
        {header3 + eight + "1 1 1\n", "more data lines"},
        {header3 + "0 0\n", "exactly three"},
        {header3 + "0 0 0 0\n", "exactly three"},
        {header3 + "0 nan 0\n", "not a finite number"},
        {header3 + "0 inf 0\n", "not a finite number"},
        {header3 + "0 1e999 0\n", "not a finite number"},
        {header3 + "0 0x10 0\n", "not a finite number"},
        {header3 + "0 0 0\nTITLE \"late\"\n", "keyword after"},
        {"TITLE \"a\"\nTITLE \"b\"\n" + header3 + eight, "appears twice"},
        {"TITLE \"unclosed\n" + header3 + eight, "closing quote"},
        {header3 + header3 + eight, "appears twice"},
        {"DOMAIN_MIN 1 0 0\nDOMAIN_MAX 1 1 1\n" + header3 + eight, "minimum must be below"},
        {"DOMAIN_MIN 0 0\n" + header3 + eight, "three numbers"},
        {"LUT_3D_INPUT_RANGE 0\n" + header3 + eight, "two numbers"},
    };
    for (const Bad& bad : corpus) {
        INFO(bad.text);
        const auto lut = parseCube(bad.text);
        REQUIRE_FALSE(lut);
        CHECK(lut.error().code == ErrorCode::ParseError);
        CHECK(lut.error().message.find(bad.expect) != std::string::npos);
    }
    // Error messages carry the line.
    const auto late = parseCube(header3 + "0 0 0\nTITLE \"late\"\n");
    REQUIRE_FALSE(late);
    CHECK(late.error().message.starts_with("line 3:"));
    // Unknown keywords before the data are skipped.
    CHECK(parseCube("LUT_IN_VIDEO_RANGE\nVENDOR_THING 1 2\n" + header3 + eight));
}

TEST_CASE("Cube: a value beyond float's range is refused, so every LUT read writes back (C.10 fuzzing)", "[color][lut]") {
    // The fuzzer's minimal reproducer: 1e39 is a finite double but infinity as the table's float, which writeCube wrote
    // as "inf" and parseCube then refused (and which would have reached the GPU).
    for (const char* value : {"1e39", "-1e39", "3.5e38", "888888888888888888888888888888888888888881"}) {
        INFO(value);
        const auto lut = parseCube(std::string("LUT_1D_SIZE 2\n0 0 0\n1 1 ") + value + "\n");
        REQUIRE_FALSE(lut);
        CHECK(lut.error().message.find("not a finite number") != std::string::npos);
    }
    // The largest float still reads, and writes back unchanged.
    const auto big = parseCube("LUT_1D_SIZE 2\n0 0 0\n1 1 3.4e38\n");
    REQUIRE(big);
    const std::string written = writeCube(*big);
    const auto again = parseCube(written);
    REQUIRE(again);
    CHECK(writeCube(*again) == written);
    CHECK(std::isfinite(again->table1d.back()));
}

TEST_CASE("Cube: fuzzed files never crash and stay bounded", "[color][lut][fuzz]") {
    const std::string valid = "TITLE \"Fuzz\"\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n" +
                              cube3d(5, [](double r, double g, double b) { return Rgb{r * r, g, std::sqrt(b)}; });
    std::mt19937 rng(20261011);
    const std::string alphabet = "0123456789.-+eE \t\r\n#\"ABCDEFGHIJKLMNOPQRSTUVWXYZ_xnaif\xEF\xBB\xBF";
    int parsed = 0;
    for (int iteration = 0; iteration < 3000; ++iteration) {
        std::string text = valid;
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits; ++e) {
            const std::size_t at = rng() % (text.size() + 1);
            switch (rng() % 4) {
            case 0: // replace a byte
                if (at < text.size()) {
                    text[at] = alphabet[rng() % alphabet.size()];
                }
                break;
            case 1: // insert bytes
                text.insert(at, std::string(1 + rng() % 4, alphabet[rng() % alphabet.size()]));
                break;
            case 2: // delete a span
                text.erase(at, std::min<std::size_t>(rng() % 16, text.size() - std::min(at, text.size())));
                break;
            default: // duplicate or truncate
                if (rng() % 2) {
                    text += text.substr(at, 64);
                } else {
                    text.resize(at);
                }
                break;
            }
        }
        const auto lut = parseCube(text);
        if (lut) {
            ++parsed;
            // Whatever parsed is consistent and bounded.
            CHECK(lut->table3d.size() == static_cast<std::size_t>(lut->size3d) * lut->size3d * lut->size3d * 3);
            CHECK(lut->table1d.size() == static_cast<std::size_t>(lut->size1d) * 3);
            CHECK(lut->size3d <= kMaxLut3dSize);
            const Rgb out = applyLut(*lut, {0.3, 0.6, 0.9});
            CHECK(std::isfinite(out[0]));
        }
    }
    // Pure random bytes, and huge size claims with no data.
    for (int iteration = 0; iteration < 500; ++iteration) {
        std::string text(rng() % 2048, '\0');
        for (char& c : text) {
            c = static_cast<char>(rng() % 256);
        }
        (void)parseCube(text);
    }
    CHECK_FALSE(parseCube("LUT_3D_SIZE 65\n"));
    CHECK_FALSE(parseCube("LUT_1D_SIZE 65536\n0 0 0\n"));
    CHECK_FALSE(parseCube(std::string(kMaxCubeBytes + 1, '#')));
    INFO("parsed " << parsed << " of 3000 mutated files");
    CHECK(parsed > 0);
}

TEST_CASE("Tetrahedral interpolation matches an independent reference; trilinear is exact on linear maps", "[color][lut]") {
    // A non-linear LUT (a strong look), 17 points.
    const auto lut = parseCube(cube3d(17, [](double r, double g, double b) {
        return Rgb{std::pow(r, 0.7) * 0.9 + 0.1 * g, g * g * 0.8 + 0.2 * b, std::sin(b * 1.4) * 0.7 + 0.3 * r * g};
    }));
    REQUIRE(lut);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    double worst = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const Rgb in{u(rng), u(rng), u(rng)};
        const Rgb a = apply3d(*lut, in, LutInterpolation::Tetrahedral);
        const Rgb b = tetraReference(*lut, in);
        for (int c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs(a[static_cast<std::size_t>(c)] - b[static_cast<std::size_t>(c)]));
        }
    }
    CHECK(worst < 1e-12);
    // Lattice points are exact; the grey axis is exact for an identity LUT; ties (equal fractions) are continuous.
    const Lut identity = identityLut3d(33);
    for (int i = 0; i < 1000; ++i) {
        const Rgb in{u(rng), u(rng), u(rng)};
        for (const auto interpolation : {LutInterpolation::Tetrahedral, LutInterpolation::Trilinear}) {
            const Rgb out = apply3d(identity, in, interpolation);
            CHECK(out[0] == Approx(in[0]).margin(1e-6));
            CHECK(out[1] == Approx(in[1]).margin(1e-6));
            CHECK(out[2] == Approx(in[2]).margin(1e-6));
        }
    }
    // Trilinear reproduces any affine map exactly; tetrahedral too.
    const auto affine = parseCube(cube3d(9, [](double r, double g, double b) { return Rgb{0.5 * r + 0.2 * g + 0.1, 0.3 * b + 0.6 * g, r - 0.5 * b + 0.25}; }));
    REQUIRE(affine);
    for (int i = 0; i < 1000; ++i) {
        const Rgb in{u(rng), u(rng), u(rng)};
        const Rgb want{0.5 * in[0] + 0.2 * in[1] + 0.1, 0.3 * in[2] + 0.6 * in[1], in[0] - 0.5 * in[2] + 0.25};
        for (const auto interpolation : {LutInterpolation::Tetrahedral, LutInterpolation::Trilinear}) {
            const Rgb out = apply3d(*affine, in, interpolation);
            for (int c = 0; c < 3; ++c) {
                CHECK(out[static_cast<std::size_t>(c)] == Approx(want[static_cast<std::size_t>(c)]).margin(1e-6));
            }
        }
    }
}

TEST_CASE("Built-in LUTs: looks are generated, input LUTs follow the published curves", "[color][lut][looks]") {
    for (const BuiltinLut& b : builtinLuts()) {
        INFO(b.name);
        const auto lut = builtinLut(std::string("builtin:") + std::string(b.name));
        REQUIRE(lut);
        CHECK(lut->size3d == (b.input ? 65 : 33));
        CHECK_FALSE(b.source.empty());
        // Monotone along the grey axis (no look inverts brightness).
        double previous = -1.0;
        for (int i = 0; i <= 20; ++i) {
            const double v = i / 20.0;
            const Rgb out = applyLut(*lut, {v, v, v});
            const double y = 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2];
            CHECK(y >= previous - 1e-6);
            previous = y;
        }
    }
    CHECK_FALSE(builtinLut("builtin:nope"));
    // S-Log3: 18 % grey is code 420 of 1023 and back; V-Log: 18 % grey at 0.423.
    CHECK(linearToSlog3(0.18) * 1023.0 == Approx(420.0).margin(1e-9));
    CHECK(slog3ToLinear(linearToSlog3(0.18)) == Approx(0.18).margin(1e-12));
    CHECK(slog3ToLinear(95.0 / 1023.0) == Approx(0.0).margin(1e-12));
    CHECK(linearToVlog(0.18) == Approx(0.4233).margin(1e-3));
    for (const double x : {0.0, 0.005, 0.01, 0.05, 0.18, 0.9, 4.0, 20.0}) {
        CHECK(slog3ToLinear(linearToSlog3(x)) == Approx(x).margin(1e-9));
        CHECK(vlogToLinear(linearToVlog(x)) == Approx(x).margin(1e-9));
    }
    // A grey S-Log3 card at 18 % lands at the Rec.709 code value of 18 % grey (the roll-off leaves grey alone).
    const auto slog = builtinLut("builtin:slog3-sgamut3cine-to-rec709");
    const double code = linearToSlog3(0.18);
    const double v = (code * 1023.0 - 64.0) / 876.0; // legal-range decoded value
    const Rgb out = applyLut(*slog, {v, v, v});
    CHECK(out[1] == Approx(encode709(0.18)).margin(0.01));
    CHECK(out[0] == Approx(out[1]).margin(0.01)); // neutral stays neutral (D65 to D65)
    CHECK(out[2] == Approx(out[1]).margin(0.01));
    // Gamut matrices keep white.
    const Mat3 m = gamutToRec709(0.766, 0.275, 0.225, 0.800, 0.089, -0.087);
    const Rgb white = mul(m, {1, 1, 1});
    CHECK(white[0] == Approx(1.0).margin(1e-9));
    CHECK(white[2] == Approx(1.0).margin(1e-9));
    CHECK(highlightRolloff(0.18) == 0.18);
    CHECK(highlightRolloff(100.0) <= 1.0);
    CHECK(highlightRolloff(4.0) < 1.0);
    CHECK(highlightRolloff(4.0) > 0.99);
}

TEST_CASE("LutLibrary loads asset files once and resolves references", "[color][lut]") {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "color";
    std::filesystem::create_directories(dir);
    const auto file = dir / "library.cube";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << cube3d(2, [](double r, double g, double b) { return Rgb{g, b, r}; });
    }
    LutLibrary library;
    std::string error;
    const auto a = library.load(file, &error);
    REQUIRE(a);
    CHECK(error.empty());
    CHECK(library.load(file) == a); // cached
    ProjectState state;
    state.ids.next = 10;
    ProjectAsset asset{AssetId{3}, "lut", "library.cube", {}};
    asset.locator.path = file.generic_string();
    state.assets.push_back(asset);
    const LutResolver resolve = library.resolver(state);
    CHECK(resolve("asset:3") == a);
    CHECK_FALSE(resolve("asset:4"));
    CHECK(resolve("builtin:teal-orange"));
    state.assets.front().locator.offline = true;
    CHECK_FALSE(library.resolver(state)("asset:3"));
    CHECK_FALSE(library.load(dir / "missing.cube", &error));
    CHECK_FALSE(error.empty());
}
