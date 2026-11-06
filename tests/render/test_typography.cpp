// 0.7 (card #138): OpenType features and Optical kerning reach the text a graphic draws (render/TextTypography.h,
// GraphicRaster.cpp). Features are checked on Arabic, whose joining forms are OpenType features the shaper applies by
// default - turning them off must change the text, and Persian shapes the same way; optical kerning on Latin pairs that
// kerning tables close ("AV", "To") and letters that need nothing ("nn"). Fonts come from the machine: the tests say so
// when none has the script.

#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/render/GraphicRaster.h>
#include <wabcut/render/TextTypography.h>

#include <catch2/catch_test_macros.hpp>

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QString>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::render;

namespace {

void ensureGui() {
    if (!QGuiApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_render_tests";
        static char* argv[] = {name, nullptr};
        static QGuiApplication app(argc, argv);
    }
}

/// The first installed family of `candidates` that has the writing system (any family when Any), or "".
std::string familyFor(QFontDatabase::WritingSystem system, std::initializer_list<const char*> candidates) {
    const QStringList installed = QFontDatabase::families();
    for (const char* name : candidates) {
        const QString family = QString::fromUtf8(name);
        if (installed.contains(family, Qt::CaseInsensitive) && QFontDatabase::writingSystems(family).contains(system)) {
            return name;
        }
    }
    return {};
}

TextContent textOf(const std::string& text, const std::string& family, const TextStyle& base) {
    TextContent content;
    TextStyle style = base;
    style.fontFamily = family;
    style.fontSize = 100.0;
    content.runs = {TextRun{text, style}};
    return content;
}

double widthOf(const TextContent& content) {
    return graphicTextMeasurer().measure(content, {content.runs.front().style.fontFamily})[0];
}

// "سلام" (salam), "پارسی" (parsi, with the Persian letters pe and pe): joined letters in three forms.
const std::string kSalam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";
const std::string kParsi = "\xD9\xBE\xD8\xA7\xD8\xB1\xD8\xB3\xDB\x8C";

const std::initializer_list<const char*> kLatinFonts = {"Segoe UI", "Arial", "Helvetica", "DejaVu Sans", "Liberation Sans", "Noto Sans"};
const std::initializer_list<const char*> kArabicFonts = {"Tahoma", "Segoe UI", "Arial", "Noto Sans Arabic", "DejaVu Sans", "Noto Naskh Arabic"};

} // namespace

TEST_CASE("OpenType features set on a style reach the shaper: joining forms turn off and on (card 138)", "[render][typography]") {
    ensureGui();
    const std::string family = familyFor(QFontDatabase::Arabic, kArabicFonts);
    if (family.empty()) {
        SKIP("no installed font has Arabic letters");
    }
    for (const std::string& text : {kSalam, kParsi}) {
        INFO(text << " in " << family);
        const TextStyle plain;
        const double joined = widthOf(textOf(text, family, plain));
        REQUIRE(joined > 10.0);

        // Without the joining features every letter is drawn in its isolated form: another width.
        TextStyle isolated;
        isolated.features = "-init, -medi, -fina";
        const double apart = widthOf(textOf(text, family, isolated));
        CHECK(std::abs(apart - joined) > 1.0);

        // Contextual alternates and ligatures are on already: asking for them again changes nothing.
        TextStyle asked;
        asked.features = "calt, liga, clig";
        CHECK(widthOf(textOf(text, family, asked)) == joined);
        // A feature the font lacks is ignored, not an error.
        TextStyle unknown;
        unknown.features = "zzzz=3, ss19";
        CHECK(widthOf(textOf(text, family, unknown)) == joined);
        // An unreadable list sets nothing (the model refuses it where it is saved).
        TextStyle broken;
        broken.features = "calt, ???";
        CHECK(widthOf(textOf(text, family, broken)) == joined);
    }
}

TEST_CASE("A graphic's Persian text is drawn differently once its joining features are off (card 138)", "[render][typography]") {
    ensureGui();
    const std::string family = familyFor(QFontDatabase::Arabic, kArabicFonts);
    if (family.empty()) {
        SKIP("no installed font has Arabic letters");
    }
    const auto draw = [&](const std::string& features) {
        Graphic g;
        g.width = 600;
        g.height = 200;
        GraphicLayer l;
        l.id = 1;
        l.kind = LayerKind::Text;
        l.transform.x = ParamValue(300.0);
        l.transform.y = ParamValue(60.0);
        TextStyle style;
        style.fontFamily = family;
        style.fontSize = 100.0;
        style.features = features;
        l.text.runs = {TextRun{kParsi, style}};
        g.layers = {l};
        REQUIRE_FALSE(graphicProblem(g));
        return rasterizeGraphic(resolveGraphic(g, Ticks{}), 600, 200);
    };
    const auto joined = draw("");
    const auto apart = draw("-init, -medi, -fina");
    REQUIRE(joined != nullptr);
    REQUIRE(apart != nullptr);
    CHECK(joined->info().texts == 1);
    CHECK(joined->rgba() != apart->rgba());
    // The same list twice is the same picture.
    CHECK(draw("-init, -medi, -fina")->rgba() == apart->rgba());
}

TEST_CASE("Optical kerning tightens pairs that need it and leaves round letters (card 138)", "[render][typography]") {
    ensureGui();
    const std::string family = familyFor(QFontDatabase::Latin, kLatinFonts);
    if (family.empty()) {
        SKIP("no installed font has Latin letters");
    }
    QFont font(QString::fromStdString(family));
    font.setPixelSize(100);

    // The adjustments themselves: a diagonal beside a diagonal closes up, "nn" stays within a few percent of an em.
    const std::vector<double> av = opticalSpacings(font, QStringLiteral("AV"));
    REQUIRE(av.size() == 2);
    CHECK(av[0] < -3.0);   // pixels at 100 px: well over 3 % of an em tighter
    CHECK(av[0] >= -18.001); // never more than the 0.18 em limit
    CHECK(av[1] == 0.0);   // nothing after the last letter
    const std::vector<double> nn = opticalSpacings(font, QStringLiteral("nn"));
    REQUIRE(nn.size() == 2);
    CHECK(std::abs(nn[0]) < 4.0);
    const std::vector<double> to = opticalSpacings(font, QStringLiteral("To"));
    REQUIRE(to.size() == 2);
    CHECK(to[0] < -2.0);
    // Spaces, single letters and joined scripts are left to the shaper.
    CHECK(opticalSpacings(font, QStringLiteral("A")).empty());
    CHECK(opticalSpacings(font, QString::fromUtf8(kSalam.c_str())).empty());
    CHECK(opticalSpacings(font, QStringLiteral("A V"))[0] == 0.0);

    // In a text block: optical kerning of "AVAVAV" is tighter than no kerning at all, and "nnnnnn" barely moves.
    TextStyle off;
    off.kerning = false;
    TextStyle optical;
    optical.opticalKerning = true;
    TextStyle metrics; // the font's own pairs
    const double none = widthOf(textOf("AVAVAV", family, off));
    const double opt = widthOf(textOf("AVAVAV", family, optical));
    CHECK(opt < none - 8.0);
    const double roundNone = widthOf(textOf("nnnnnn", family, off));
    const double roundOpt = widthOf(textOf("nnnnnn", family, optical));
    CHECK(std::abs(roundOpt - roundNone) < roundNone * 0.04);
    // Kerning off wins over optical: the optical choice only applies with kerning on.
    TextStyle both;
    both.kerning = false;
    both.opticalKerning = true;
    const DrawList list = [&] {
        Graphic g;
        GraphicLayer l;
        l.id = 1;
        l.kind = LayerKind::Text;
        l.text.runs = {TextRun{"AV", both}};
        g.layers = {l};
        return resolveGraphic(g, Ticks{});
    }();
    CHECK_FALSE(list.items.at(0).text.runs.at(0).opticalKerning);
    // Metrics is a third result (the font's table), not the optical one.
    CHECK(widthOf(textOf("AVAVAV", family, metrics)) != opt);
    // Tracking still adds up: +100/1000 em on six letters.
    TextStyle tracked = optical;
    tracked.tracking = 100.0;
    CHECK(widthOf(textOf("AVAVAV", family, tracked)) > opt + 40.0);
}

TEST_CASE("Features go on a font only as the shaper's tags (card 138)", "[render][typography]") {
    ensureGui();
    QFont font;
    applyFontFeatures(font, "dlig, -liga, ss01=1, salt=2");
    CHECK(font.featureTags().size() == 4);
    CHECK(font.isFeatureSet(QFont::Tag("dlig")));
    CHECK(font.featureValue(QFont::Tag("liga")) == 0);
    CHECK(font.featureValue(QFont::Tag("salt")) == 2);
    QFont untouched;
    applyFontFeatures(untouched, "");
    CHECK(untouched.featureTags().isEmpty());
    applyFontFeatures(untouched, "no good!");
    CHECK(untouched.featureTags().isEmpty());
}
