// 0.7 (card #86): vector artwork (SVG, Illustrator, EPS) drawn at the size it is shown (render/VectorImage.h). SVG through Qt
// Svg; the PDF-compatible part of an Illustrator file through Windows.Data.Pdf (Windows) or Ghostscript; EPS through
// Ghostscript - here a stand-in executable (wabcut_fake_gs) that records its arguments, so no Ghostscript is needed to run
// the tests, and the "not installed" refusal is checked by looking in an empty folder.

#include <wabcut/model/MediaLocator.h>
#include <wabcut/render/VectorImage.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QGuiApplication>
#include <QImage>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::render;
namespace fs = std::filesystem;

namespace {

void ensureGui() {
    if (!QGuiApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_render_tests";
        static char* argv[] = {name, nullptr};
        static QGuiApplication app(argc, argv);
    }
}

/// A scratch folder that goes when the test does, and puts Ghostscript's search path back.
struct Scratch {
    fs::path dir;
    Scratch() {
        ensureGui();
        dir = fs::temp_directory_path() / ("wabcut-vector-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        VectorPictures::shared().clear();
    }
    ~Scratch() {
        setGhostscriptSearchPath(std::nullopt);
        VectorPictures::shared().clear();
        qunsetenv("WABCUT_FAKE_GS_LOG");
        qunsetenv("WABCUT_FAKE_GS_PNG");
        qunsetenv("WABCUT_FAKE_GS_MODE");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path write(const std::string& name, const std::string& text) const {
        const fs::path file = dir / pathFromUtf8(name);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        return file;
    }
    /// A folder with the stand-in Ghostscript in it under the name the lookup wants.
    fs::path fakeGhostscript() const {
        const fs::path folder = dir / "gs-bin";
        fs::create_directories(folder);
#ifdef _WIN32
        const char* name = "gswin64c.exe";
#else
        const char* name = "gs";
#endif
        fs::copy_file(fs::path(WABCUT_FAKE_GS), folder / name, fs::copy_options::overwrite_existing);
        return folder;
    }
};

/// A 32 x 16 SVG with a red 16 x 8 rectangle in the middle: x 8..24, y 4..12.
const std::string kRectSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16" viewBox="0 0 32 16"><rect x="8" y="4" width="16" height="8" fill="#ff0000"/></svg>)";

int alphaAt(const QImage& image, int x, int y) { return qAlpha(image.pixel(x, y)); }

bool isRed(const QImage& image, int x, int y) {
    const QRgb p = image.pixel(x, y);
    return qAlpha(p) > 250 && qRed(p) > 250 && qGreen(p) < 5 && qBlue(p) < 5;
}

/// A minimal one-page PDF whose page is 200 x 100 pt with a red 100 x 50 rectangle in the middle, with a correct xref.
[[maybe_unused]] std::string redRectanglePdf() { // its test is Windows only
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets;
    const auto object = [&](const std::string& body) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    const std::string content = "1 0 0 rg 50 25 100 50 re f";
    object("<< /Type /Catalog /Pages 2 0 R >>");
    object("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R /Resources << >> >>");
    object("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream");
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 5\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
        pdf += line;
    }
    pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

const std::string kEps = "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 200 100\n%%EndComments\n1 0 0 setrgbcolor 50 25 100 50 rectfill\nshowpage\n";

} // namespace

TEST_CASE("Vector files are recognised by extension and sized for the box they fill (card 86)", "[render][vector]") {
    CHECK(vectorFormatOf("logo.svg") == VectorFormat::Svg);
    CHECK(vectorFormatOf("LOGO.SVGZ") == VectorFormat::Svg);
    CHECK(vectorFormatOf("art.AI") == VectorFormat::Illustrator);
    CHECK(vectorFormatOf("art.eps") == VectorFormat::Eps);
    CHECK_FALSE(vectorFormatOf("photo.png").has_value());
    CHECK_FALSE(vectorFormatOf("doc.pdf").has_value());
    CHECK_FALSE(vectorFormatOf("noextension").has_value());

    // A 200 x 100 artwork: a box it just fits is drawn at its own size; a box twice as large, twice as large.
    const VectorInfo info{VectorFormat::Svg, 200.0, 100.0};
    CHECK(vectorRenderSize(info, 200.0, 100.0) == QSize(200, 100));
    CHECK(vectorRenderSize(info, 400.0, 200.0) == QSize(400, 200));
    // The box is covered on both axes (the larger scale), with the artwork's shape.
    CHECK(vectorRenderSize(info, 200.0, 200.0) == QSize(400, 200));
    // A slowly growing box does not draw it afresh every frame: sizes come in quarter-octave steps.
    const QSize a = vectorRenderSize(info, 410.0, 205.0);
    const QSize b = vectorRenderSize(info, 420.0, 210.0);
    CHECK(a == b);
    CHECK(a.width() >= 410);
    CHECK(a.width() < 410 * 1.2);
    // Never more than 8192 on a side, whatever the box says; never empty.
    const QSize huge = vectorRenderSize(info, 1e9, 1e9);
    CHECK(huge.width() == kMaxVectorRenderSide);
    CHECK(huge.height() == kMaxVectorRenderSide / 2);
    CHECK(vectorRenderSize(info, 0.0, 0.0).width() >= 1);
    CHECK(vectorRenderSize(info, -5.0, std::nan("")).height() >= 1);
}

TEST_CASE("An SVG is probed for its size and drawn crisp at any size (card 86)", "[render][vector][svg]") {
    Scratch scratch;
    const fs::path file = scratch.write("logo.svg", kRectSvg);
    const auto info = probeVector(file);
    REQUIRE(info);
    CHECK(info->format == VectorFormat::Svg);
    CHECK(info->width == 32.0);
    CHECK(info->height == 16.0);

    SECTION("at its own size") {
        const auto image = renderVector(file, *info, QSize(32, 16));
        REQUIRE(image);
        CHECK(image->size() == QSize(32, 16));
        CHECK(isRed(*image, 16, 8));
        CHECK(alphaAt(*image, 2, 8) == 0); // nothing drawn: transparent
    }
    SECTION("sixteen times larger, the edge is still a hard edge - not a stretched bitmap") {
        const auto image = renderVector(file, *info, QSize(512, 256));
        REQUIRE(image);
        CHECK(image->size() == QSize(512, 256));
        // The rectangle's left edge lies at x = 8 * 16 = 128: every pixel a few away from it is fully one or the other.
        CHECK(alphaAt(*image, 124, 128) == 0);
        CHECK(isRed(*image, 132, 128));
        CHECK(isRed(*image, 380, 128));
        CHECK(alphaAt(*image, 388, 128) == 0);
        CHECK(alphaAt(*image, 256, 60) == 0); // above the rectangle (y < 64)
        CHECK(isRed(*image, 256, 70));
    }
    SECTION("a size out of range is refused") {
        CHECK_FALSE(renderVector(file, *info, QSize(0, 10)));
        CHECK_FALSE(renderVector(file, *info, QSize(kMaxVectorRenderSide + 1, 10)));
    }
}

TEST_CASE("An SVG with only a viewBox, a huge size or no size is sized sensibly or refused (card 86)", "[render][vector][svg]") {
    Scratch scratch;
    SECTION("a viewBox alone is the size") {
        const auto info = probeVector(scratch.write(
            "a.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 24"><circle cx="12" cy="12" r="10"/></svg>)"));
        REQUIRE(info);
        CHECK(info->width == 48.0);
        CHECK(info->height == 24.0);
    }
    SECTION("a hostile size is scaled down to the limit with its shape") {
        const auto info = probeVector(scratch.write(
            "b.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="1000000" height="500000"><rect width="5" height="5"/></svg>)"));
        REQUIRE(info);
        CHECK(info->width == kMaxVectorSide);
        CHECK(info->height == kMaxVectorSide / 2);
    }
    SECTION("not an SVG, empty, or missing") {
        CHECK_FALSE(probeVector(scratch.write("c.svg", "this is not xml")));
        CHECK_FALSE(probeVector(scratch.write("d.svg", "")));
        CHECK(probeVector(scratch.dir / "missing.svg").error().code == ErrorCode::NotFound);
        CHECK(probeVector(scratch.write("e.png", "x")).error().code == ErrorCode::Unsupported);
    }
}

TEST_CASE("Drawings are kept per size and drawn again only for a new size or a changed file (card 86)", "[render][vector][svg]") {
    Scratch scratch;
    const fs::path file = scratch.write("logo.svg", kRectSvg);
    VectorPictures& pictures = VectorPictures::shared();
    const std::uint64_t before = pictures.renders();

    const QImage small = pictures.picture(file, 32.0, 16.0);
    REQUIRE_FALSE(small.isNull());
    CHECK(small.size() == QSize(32, 16));
    CHECK(pictures.picture(file, 32.0, 16.0).cacheKey() == small.cacheKey()); // the same drawing
    CHECK(pictures.renders() == before + 1);

    const QImage large = pictures.picture(file, 500.0, 250.0);
    REQUIRE_FALSE(large.isNull());
    CHECK(large.width() >= 500);
    CHECK(pictures.renders() == before + 2);
    const std::uint64_t afterLarge = pictures.renders();
    CHECK(pictures.picture(file, 505.0, 252.0).cacheKey() == large.cacheKey()); // the same step
    CHECK(pictures.renders() == afterLarge);

    // A file that changed on disk is drawn again (the size of the file differs: the stamp does not need the clock).
    scratch.write("logo.svg", kRectSvg + "\n<!-- edited -->");
    const QImage edited = pictures.picture(file, 500.0, 250.0);
    CHECK_FALSE(edited.isNull());
    CHECK(pictures.renders() == afterLarge + 1);
    // A file that cannot be drawn gives a null picture, not a crash.
    scratch.write("broken.svg", "<svg");
    CHECK(pictures.picture(scratch.dir / "broken.svg", 10.0, 10.0).isNull());
    CHECK(pictures.picture(scratch.dir / "gone.svg", 10.0, 10.0).isNull());
}

TEST_CASE("EPS and PostScript Illustrator files are refused with what to install when Ghostscript is missing (card 86)",
          "[render][vector][ghostscript]") {
    Scratch scratch;
    const fs::path empty = scratch.dir / "nothing-here";
    fs::create_directories(empty);
    setGhostscriptSearchPath(pathToUtf8(empty));
    CHECK_FALSE(findGhostscript().has_value());

    for (const char* name : {"art.eps", "old.ai"}) {
        INFO(name);
        const auto info = probeVector(scratch.write(name, kEps));
        REQUIRE_FALSE(info);
        CHECK(info.error().code == ErrorCode::Unsupported);
        // It says what to install, where, and the way round it.
        CHECK(info.error().message.find("Ghostscript") != std::string::npos);
        CHECK(info.error().message.find("https://ghostscript.com") != std::string::npos);
        CHECK(info.error().message.find("SVG") != std::string::npos);
    }
    // An SVG needs nothing.
    CHECK(probeVector(scratch.write("ok.svg", kRectSvg)));
}

TEST_CASE("An Illustrator file with neither PDF nor PostScript content says how to save it (card 86)", "[render][vector]") {
    Scratch scratch;
    const auto info = probeVector(scratch.write("plain.ai", "this was saved without PDF compatibility, or is not an Illustrator file at all"));
    REQUIRE_FALSE(info);
    CHECK(info.error().code == ErrorCode::Unsupported);
    CHECK(info.error().message.find("PDF Compatible") != std::string::npos);
    CHECK(info.error().message.find("SVG") != std::string::npos);
}

TEST_CASE("EPS is drawn by Ghostscript run safely at the resolution that makes the picture (card 86)", "[render][vector][ghostscript]") {
    Scratch scratch;
    setGhostscriptSearchPath(pathToUtf8(scratch.fakeGhostscript()));
    REQUIRE(findGhostscript().has_value());

    QImage red(200, 100, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(255, 0, 0));
    const fs::path png = scratch.dir / "picture.png";
    REQUIRE(red.save(QString::fromStdString(pathToUtf8(png))));
    const fs::path log = scratch.dir / "gs.log";
    qputenv("WABCUT_FAKE_GS_PNG", pathToUtf8(png).c_str());
    qputenv("WABCUT_FAKE_GS_LOG", pathToUtf8(log).c_str());

    const fs::path file = scratch.write("art.eps", kEps);
    const auto info = probeVector(file);
    REQUIRE(info);
    CHECK(info->format == VectorFormat::Eps);
    CHECK(info->width == 200.0); // from the %%BoundingBox: no process needed to size it
    CHECK(info->height == 100.0);

    const auto image = renderVector(file, *info, QSize(400, 200));
    REQUIRE(image);
    CHECK(image->size() == QSize(400, 200)); // the 200 x 100 answer scaled to the size asked for
    CHECK(isRed(*image, 200, 100));

    std::ifstream in(log);
    const std::string arguments((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    INFO(arguments);
    CHECK(arguments.find("-dSAFER\n") != std::string::npos); // untrusted PostScript: no file or process access
    CHECK(arguments.find("-dNOSAFER") == std::string::npos);
    CHECK(arguments.find("-dEPSCrop\n") != std::string::npos);
    CHECK(arguments.find("-sDEVICE=pngalpha\n") != std::string::npos);
    CHECK(arguments.find("-r144.000\n") != std::string::npos); // 72 dpi x 400 / 200
    CHECK(arguments.find("-sOutputFile=-\n") != std::string::npos);
    CHECK(arguments.find(pathToUtf8(file)) != std::string::npos);

    SECTION("a PostScript error is reported with Ghostscript's words") {
        qputenv("WABCUT_FAKE_GS_MODE", "fail");
        const auto failed = renderVector(file, *info, QSize(40, 20));
        REQUIRE_FALSE(failed);
        CHECK(failed.error().message.find("Ghostscript could not draw the file") != std::string::npos);
        CHECK(failed.error().message.find("/undefined in foo") != std::string::npos);
        // A drawing that failed is a null picture where the vector is shown, never a crash.
        VectorPictures::shared().clear();
        CHECK(VectorPictures::shared().picture(file, 40.0, 20.0).isNull());
    }
}

TEST_CASE("An EPS whose box is far bigger than the size limit is still drawn at the size asked for (card 86)",
          "[render][vector][ghostscript]") {
    Scratch scratch;
    setGhostscriptSearchPath(pathToUtf8(scratch.fakeGhostscript()));
    QImage red(200, 100, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(255, 0, 0));
    const fs::path png = scratch.dir / "picture.png";
    REQUIRE(red.save(QString::fromStdString(pathToUtf8(png))));
    const fs::path log = scratch.dir / "gs.log";
    qputenv("WABCUT_FAKE_GS_PNG", pathToUtf8(png).c_str());
    qputenv("WABCUT_FAKE_GS_LOG", pathToUtf8(log).c_str());
    const fs::path file = scratch.write(
        "huge.eps", "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 200000 100000\n%%EndComments\nshowpage\n");
    const auto info = probeVector(file);
    REQUIRE(info);
    CHECK(info->width <= kMaxVectorSide); // the nominal size is cut down ...
    const auto image = renderVector(file, *info, QSize(400, 200));
    REQUIRE(image);
    std::ifstream in(log);
    const std::string arguments((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    INFO(arguments);
    // ... but the resolution follows the box Ghostscript will really draw (-dEPSCrop): 72 dpi x 400 / 200000, not x 400 / 16384.
    CHECK(arguments.find("-r0.144\n") != std::string::npos);
}

TEST_CASE("An SVG that links to another file or address is refused; links inside it and data: pictures are fine (card 86)",
          "[render][vector][svg][security]") {
    Scratch scratch;
    const std::string head = R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="32" height="16">)";
    const char* const hostile[] = {
        R"(<image xlink:href="C:/Users/me/Pictures/secret.png" width="10" height="10"/>)",
        R"(<image href="//attacker/share/x.png" width="10" height="10"/>)",
        R"(<image xlink:href='\\attacker\share\x.png' width="10" height="10"/>)",
        R"(<image HREF = "file:///etc/passwd" width="10" height="10"/>)",
        R"(<image href="https://attacker.example.com/x.png" width="10" height="10"/>)",
        R"(<image href="x.png" width="10" height="10"/>)",
    };
    int n = 0;
    for (const char* body : hostile) {
        INFO(body);
        const fs::path file = scratch.write("evil" + std::to_string(n++) + ".svg", head + body + "</svg>");
        const auto probed = probeVector(file);
        REQUIRE_FALSE(probed);
        CHECK(probed.error().message.find("links to another file") != std::string::npos);
        VectorInfo info{VectorFormat::Svg, 32.0, 16.0};
        CHECK_FALSE(renderVector(file, info, QSize(32, 16)));
    }
    // Its own parts and an embedded picture are not links elsewhere.
    const fs::path fine = scratch.write(
        "fine.svg", head + R"(<defs><linearGradient id="g"/><rect id="r" width="4" height="4"/></defs><use href="#r"/><use xlink:href="#r"/>)" +
                        R"(<image href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==" width="1" height="1"/></svg>)");
    CHECK(probeVector(fine));
}

TEST_CASE("A PostScript-based Illustrator file is drawn by Ghostscript too (card 86)", "[render][vector][ghostscript]") {
    Scratch scratch;
    setGhostscriptSearchPath(pathToUtf8(scratch.fakeGhostscript()));
    QImage red(200, 100, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(255, 0, 0));
    const fs::path png = scratch.dir / "picture.png";
    REQUIRE(red.save(QString::fromStdString(pathToUtf8(png))));
    qputenv("WABCUT_FAKE_GS_PNG", pathToUtf8(png).c_str());
    const fs::path file = scratch.write("old.ai", kEps);
    const auto info = probeVector(file);
    REQUIRE(info);
    CHECK(info->format == VectorFormat::Illustrator);
    CHECK(info->width == 200.0);
    const auto image = renderVector(file, *info, QSize(100, 50));
    REQUIRE(image);
    CHECK(image->size() == QSize(100, 50));
    CHECK(isRed(*image, 50, 25));
}

#ifdef _WIN32
TEST_CASE("A PDF-compatible Illustrator file is drawn by Windows at any size (card 86)", "[render][vector][pdf]") {
    Scratch scratch;
    const fs::path file = scratch.write("logo.ai", redRectanglePdf());
    const auto info = probeVector(file);
    INFO((info ? std::string() : info.error().message));
    REQUIRE(info);
    CHECK(info->format == VectorFormat::Illustrator);
    CHECK(info->width == Catch::Approx(200.0).margin(0.5));
    CHECK(info->height == Catch::Approx(100.0).margin(0.5));

    for (const int scale : {1, 4}) {
        INFO("scale " << scale);
        const QSize size(200 * scale, 100 * scale);
        const auto image = renderVector(file, *info, size);
        INFO((image ? std::string() : image.error().message));
        REQUIRE(image);
        CHECK(image->size() == size);
        CHECK(isRed(*image, size.width() / 2, size.height() / 2));
        CHECK(alphaAt(*image, 3, 3) < 10); // outside the artwork: transparent, not white paper
        // Crisp edge: the rectangle starts at a quarter of the width; a few pixels either side are fully one or the other.
        const int edge = size.width() / 4;
        const int margin = 2 * scale + 1;
        CHECK(alphaAt(*image, edge - margin, size.height() / 2) < 10);
        CHECK(isRed(*image, edge + margin, size.height() / 2));
    }
    // A PDF that is not valid is an error with Windows' reason, not a crash.
    const auto broken = probeVector(scratch.write("broken.ai", "%PDF-1.4\nthis is not a pdf"));
    CHECK_FALSE(broken);
}
#endif
