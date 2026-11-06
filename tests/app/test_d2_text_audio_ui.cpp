// The font picker, spoken language, caption gallery and auto-mix UI (offscreen; no window is shown): the shared font picker and its ranking (card 053), the spoken
// language and translate options of the Transcript tab with Transcribe again and Delete transcript (cards 073 and 080), the
// Caption Style panel's animated gallery with timing and emphasis and text rules (cards 074 and 114), and the Audio Tools'
// auto-mix of several voices (card 137). Screenshots land in <build>/test-output/ui/ (dark and light themes).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "FontCatalog.h"
#include "ProjectController.h"
#include "SourceTranscriptBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/SpokenLanguages.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QQmlComponent>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* visibleNamed(Shell& shell, const QString& name) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == name && i->isVisible(); });
}

QQuickItem* namedIn(QQuickItem* root, const QString& name) {
    return findItem(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

void saveShot(const QImage& image, const QQuickItem* item, const QString& name, int margin = 0) {
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).adjusted(-margin, -margin, margin, margin);
    area = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

/// The frame a popup (a Popover, a menu) draws in, for a screenshot.
QQuickItem* popupFrame(QQuickItem* popupContent) {
    return popupContent && popupContent->parentItem() ? popupContent->parentItem() : popupContent;
}

// ---- Fonts ------------------------------------------------------------------------------------------------------------

using fonts::Entry;
using fonts::Kind;

constexpr int kLatin = QFontDatabase::Latin;
constexpr int kThai = QFontDatabase::Thai;
constexpr int kArabic = QFontDatabase::Arabic;
constexpr int kHebrew = QFontDatabase::Hebrew;
constexpr int kCyrillic = QFontDatabase::Cyrillic;
constexpr int kGreek = QFontDatabase::Greek;
constexpr int kVietnamese = QFontDatabase::Vietnamese;
constexpr int kJapanese = QFontDatabase::Japanese;
constexpr int kChinese = QFontDatabase::SimplifiedChinese;
constexpr int kKorean = QFontDatabase::Korean;

Entry entry(const QString& family, std::initializer_list<int> scripts, Kind kind = Kind::Sans, bool bold = true, bool italic = false,
            int weights = 2) {
    Entry e;
    e.family = family;
    e.folded = fonts::fold(family);
    e.words = fonts::foldedWords(family);
    for (const int s : scripts)
        e.scripts |= std::uint64_t{1} << s;
    e.kind = kind;
    e.bold = bold;
    e.italic = italic;
    e.weights = weights;
    e.fixedPitch = kind == Kind::Mono;
    return e;
}

/// A small system: Thai families made for Thai (and Latin), Windows' wide ones, CJK, serif, mono, script and display faces.
std::vector<Entry> system() {
    return {
        entry(QStringLiteral("Arial"), {kLatin, kGreek, kCyrillic, kHebrew, kArabic, kVietnamese}, Kind::Sans, true, true, 4),
        entry(QStringLiteral("Tahoma"), {kLatin, kThai, kArabic, kHebrew, kCyrillic, kGreek, kVietnamese}, Kind::Sans, true, false, 2),
        entry(QStringLiteral("Leelawadee UI"), {kLatin, kThai, kVietnamese}, Kind::Sans, true, false, 3),
        entry(QStringLiteral("Leelawadee UI Thai"), {kThai, kLatin}, Kind::Sans, true, false, 3),
        entry(QStringLiteral("Noto Sans Thai"), {kThai, kLatin}, Kind::Sans, true, false, 9),
        entry(QStringLiteral("TH Sarabun New"), {kThai, kLatin}, Kind::Sans, true, true, 2),
        entry(QStringLiteral("Bai Jamjuree"), {kThai, kLatin, kVietnamese}, Kind::Sans, true, true, 12),
        entry(QStringLiteral("Segoe UI"), {kLatin, kGreek, kCyrillic, kVietnamese}, Kind::Sans, true, true, 6),
        entry(QStringLiteral("Times New Roman"), {kLatin, kGreek, kCyrillic, kHebrew, kArabic, kVietnamese}, Kind::Serif, true, true, 2),
        entry(QStringLiteral("Georgia"), {kLatin, kGreek, kCyrillic}, Kind::Serif, true, true, 2),
        entry(QStringLiteral("Courier New"), {kLatin, kGreek, kCyrillic, kHebrew, kArabic}, Kind::Mono, true, true, 2),
        entry(QStringLiteral("Cascadia Mono"), {kLatin, kGreek, kCyrillic, kVietnamese}, Kind::Mono, true, true, 8),
        entry(QStringLiteral("Comic Sans MS"), {kLatin, kGreek, kCyrillic}, Kind::Script, true, true, 2),
        entry(QStringLiteral("Impact"), {kLatin, kCyrillic}, Kind::Display, false, false, 1),
        entry(QStringLiteral("MS Gothic"), {kLatin, kJapanese}, Kind::Sans, false, false, 1),
        entry(QStringLiteral("Meiryo"), {kLatin, kJapanese, kGreek, kCyrillic}, Kind::Sans, true, true, 2),
        entry(QStringLiteral("SimSun"), {kLatin, kChinese}, Kind::Serif, false, false, 1),
        entry(QStringLiteral("Malgun Gothic"), {kLatin, kKorean}, Kind::Sans, true, false, 2),
    };
}

QStringList families(const std::vector<fonts::Row>& rows, const char* group = nullptr) {
    QStringList out;
    for (const fonts::Row& r : rows) {
        if (!group || std::string_view(r.group) == group)
            out << r.entry->family;
    }
    return out;
}

QStringList groups(const std::vector<fonts::Row>& rows) {
    QStringList out;
    for (const fonts::Row& r : rows) {
        const QString g = QString::fromLatin1(r.group);
        if (out.isEmpty() || out.last() != g)
            out << g;
    }
    return out;
}

} // namespace

TEST_CASE("Font search: typing Thai lists Thai-named fonts first then fonts that draw Thai then loose matches",
          "[app][fonts][search]") {
    const std::vector<Entry> catalog = system();
    fonts::Query q;
    q.text = QStringLiteral("Thai");
    const std::vector<fonts::Row> rows = fonts::rank(catalog, q);
    REQUIRE_FALSE(rows.empty());
    // 1. The name matches: the shorter name first.
    CHECK(families(rows, "name") == QStringList{"Noto Sans Thai", "Leelawadee UI Thai"});
    // 2. The font draws Thai: the ones made for fewer scripts first (a Thai face before the wide ones), Tahoma last.
    const QStringList language = families(rows, "language");
    CHECK(language == QStringList{"TH Sarabun New", "Bai Jamjuree", "Leelawadee UI", "Tahoma"});
    // The groups come in that order, each family once, and a font that draws no Thai is not a language match.
    const QStringList order = groups(rows);
    CHECK(order.indexOf(QStringLiteral("name")) == 0);
    CHECK(order.indexOf(QStringLiteral("language")) == 1);
    CHECK_FALSE(language.contains(QStringLiteral("Arial")));
    const QStringList listed = families(rows);
    CHECK(std::set<QString>(listed.begin(), listed.end()).size() == static_cast<std::size_t>(listed.size()));
    // The language rows carry the script the header and the sample name.
    for (const fonts::Row& r : rows) {
        if (std::string_view(r.group) == "language") {
            REQUIRE_FALSE(r.languageScripts.empty());
            CHECK(r.languageScripts.front() == kThai);
        }
    }
}

TEST_CASE("Font search: language and script names work in English and in their own script", "[app][fonts][search]") {
    const std::vector<Entry> catalog = system();
    const auto ask = [&](const QString& text) {
        fonts::Query q;
        q.text = text;
        return fonts::rank(catalog, q);
    };
    // Thai in its own script: no name matches, every Thai font is a language match.
    const auto native = ask(QString::fromUtf8("ไทย"));
    CHECK(families(native, "name").isEmpty());
    CHECK(families(native, "language").size() == 6);
    CHECK(families(native, "language").contains(QStringLiteral("Tahoma")));
    // Japanese, 日本語, an alias (kana) and a prefix (japan).
    for (const QString& text : {QStringLiteral("Japanese"), QString::fromUtf8("日本語"), QStringLiteral("kana"), QStringLiteral("japan")}) {
        INFO(text.toStdString());
        const auto rows = ask(text);
        CHECK(families(rows, "language") == QStringList{"MS Gothic", "Meiryo"});
    }
    // Arabic and Hebrew in their own scripts.
    CHECK(families(ask(QString::fromUtf8("عربي")), "language").contains(QStringLiteral("Tahoma")));
    CHECK(families(ask(QString::fromUtf8("עברית")), "language").contains(QStringLiteral("Arial")));
    CHECK_FALSE(families(ask(QString::fromUtf8("עברית")), "language").contains(QStringLiteral("Georgia")));
    CHECK(families(ask(QStringLiteral("Cyrillic")), "language").contains(QStringLiteral("Impact")));
    CHECK(families(ask(QStringLiteral("Korean")), "language") == QStringList{"Malgun Gothic"});
    // "Chinese" names the simplified and the traditional script: a font that draws either is a match.
    CHECK(families(ask(QStringLiteral("Chinese")), "language") == QStringList{"SimSun"});
    // The script and a name word: "thai sans" is the Thai script plus "sans" in the name.
    const auto both = ask(QStringLiteral("thai sans"));
    CHECK(families(both, "name") == QStringList{"Noto Sans Thai"});
    CHECK(families(both, "language").isEmpty()); // no other Thai font has "sans" in its name
}

TEST_CASE("Font search: loose matches - letters in order and spaces and dashes and case and small typos", "[app][fonts][search]") {
    const std::vector<Entry> catalog = system();
    const auto ask = [&](const QString& text) {
        fonts::Query q;
        q.text = text;
        return fonts::rank(catalog, q);
    };
    // Spaces, dashes and case do not matter: all of these are the family's name.
    for (const QString& text : {QStringLiteral("noto-sans thai"), QStringLiteral("NOTOSANSTHAI"), QStringLiteral("Noto  Sans   Thai"),
                                QStringLiteral("notosans")}) {
        INFO(text.toStdString());
        CHECK(families(ask(text), "name").contains(QStringLiteral("Noto Sans Thai")));
    }
    // T-H-A-I spelled letter by letter is the word.
    CHECK(families(ask(QStringLiteral("T-H-A-I")), "name") == families(ask(QStringLiteral("Thai")), "name"));
    // A typo: transposed, a missing letter, one letter wrong - "Similar".
    CHECK(families(ask(QStringLiteral("arail")), "similar") == QStringList{"Arial"});
    CHECK(families(ask(QStringLiteral("tahma")), "similar").contains(QStringLiteral("Tahoma")));
    CHECK(families(ask(QStringLiteral("georgea")), "similar").contains(QStringLiteral("Georgia")));
    CHECK(families(ask(QStringLiteral("comik sans")), "similar").contains(QStringLiteral("Comic Sans MS")));
    // Letters in order, not together: "tmr" finds Times New Roman.
    CHECK(families(ask(QStringLiteral("tmsnrmn")), "similar").contains(QStringLiteral("Times New Roman")));
    // Nothing alike: nothing.
    CHECK(ask(QStringLiteral("zzzzqq")).empty());
    // Too short to guess: only names that contain it.
    CHECK(families(ask(QStringLiteral("xy")), "similar").isEmpty());
}

TEST_CASE("Font filters combine: script and style and type and source and sort and what a layer's text needs", "[app][fonts][filters]") {
    const std::vector<Entry> catalog = system();
    const auto ask = [&](const std::function<void(fonts::Query&)>& set) {
        fonts::Query q;
        set(q);
        return families(fonts::rank(catalog, q));
    };
    // The language filter alone: every Thai font; two scripts: fonts that draw both.
    CHECK(ask([](fonts::Query& q) { q.scripts = {kThai}; }).size() == 6);
    CHECK(ask([](fonts::Query& q) { q.scripts = {kThai, kArabic}; }) == QStringList{"Tahoma"});
    CHECK(ask([](fonts::Query& q) { q.scripts = {kThai, kChinese}; }).isEmpty());
    // Style: bold, italic, weights.
    CHECK_FALSE(ask([](fonts::Query& q) { q.italic = true; }).contains(QStringLiteral("Tahoma")));
    CHECK(ask([](fonts::Query& q) { q.italic = true; q.scripts = {kThai}; }) == QStringList{"Bai Jamjuree", "TH Sarabun New"}); // A to Z
    CHECK(ask([](fonts::Query& q) { q.minWeights = 8; }) == QStringList{"Bai Jamjuree", "Cascadia Mono", "Noto Sans Thai"});
    CHECK_FALSE(ask([](fonts::Query& q) { q.bold = true; }).contains(QStringLiteral("Impact")));
    // Type.
    CHECK(ask([](fonts::Query& q) { q.kinds = {Kind::Serif}; }) == QStringList{"Georgia", "SimSun", "Times New Roman"});
    CHECK(ask([](fonts::Query& q) { q.kinds = {Kind::Mono, Kind::Display}; }) == QStringList{"Cascadia Mono", "Courier New", "Impact"});
    CHECK(ask([](fonts::Query& q) { q.kinds = {Kind::Script}; }) == QStringList{"Comic Sans MS"});
    // Filters narrow a typed search too.
    CHECK(ask([](fonts::Query& q) { q.text = QStringLiteral("thai"); q.kinds = {Kind::Mono}; }).isEmpty());
    CHECK(ask([](fonts::Query& q) { q.text = QStringLiteral("thai"); q.italic = true; }) ==
          QStringList{"TH Sarabun New", "Bai Jamjuree"}); // typed: the font made for fewer scripts first
    // Source: favourites, recently used (with their order), brand kit and project families.
    CHECK(ask([](fonts::Query& q) { q.source = QStringLiteral("favourites"); q.favourites = {"Impact", "Georgia"}; }) ==
          QStringList{"Georgia", "Impact"});
    CHECK(ask([](fonts::Query& q) { q.source = QStringLiteral("recent"); q.recent = {"Impact", "Arial"}; }) ==
          QStringList{"Arial", "Impact"});
    CHECK(ask([](fonts::Query& q) { q.source = QStringLiteral("brand"); q.sourceFamilies = {"georgia", "Not Installed"}; }) ==
          QStringList{"Georgia"});
    CHECK(ask([](fonts::Query& q) { q.source = QStringLiteral("project"); q.sourceFamilies = {"Meiryo"}; }) == QStringList{"Meiryo"});
    // The layer's text needs Thai and Latin: fonts that cannot draw it are left out ("hide").
    const QStringList needs = ask([](fonts::Query& q) { q.mustDraw = fonts::scriptsOfText(QString::fromUtf8("สวัสดี Hello")); });
    CHECK(needs.size() == 6);
    CHECK_FALSE(needs.contains(QStringLiteral("Arial")));
    // Sort: A to Z, and recently used first.
    const QStringList az = ask([](fonts::Query& q) { q.sort = QStringLiteral("az"); q.recent = {"Impact"}; });
    CHECK(az.first() == QStringLiteral("Arial"));
    CHECK(az.last() == QStringLiteral("Times New Roman"));
    const QStringList recent = ask([](fonts::Query& q) { q.sort = QStringLiteral("recent"); q.recent = {"Impact", "Georgia"}; });
    CHECK(recent.first() == QStringLiteral("Impact"));
    CHECK(recent[1] == QStringLiteral("Georgia"));
    // Nothing typed with relevance: favourites first, then the recent few, then every other font.
    fonts::Query plain;
    plain.favourites = {"Meiryo"};
    plain.recent = {"Impact", "Meiryo"};
    const auto rows = fonts::rank(catalog, plain);
    CHECK(std::string_view(rows[0].group) == "favourite");
    CHECK(rows[0].entry->family == QStringLiteral("Meiryo"));
    CHECK(std::string_view(rows[1].group) == "recent");
    CHECK(rows[1].entry->family == QStringLiteral("Impact"));
    CHECK(std::string_view(rows[2].group) == "all");
    CHECK(rows.size() == catalog.size());
    // The limit.
    fonts::Query limited;
    limited.limit = 5;
    CHECK(fonts::rank(catalog, limited).size() == 5);
}

TEST_CASE("Font search stays instant on a thousand and more installed fonts", "[app][fonts][search][perf]") {
    // 2,400 families with plausible names: the search of every keystroke - names, the language group and the typo group.
    std::vector<Entry> big = system();
    const char* first[] = {"Noto", "Open", "Source", "Fira", "Roboto", "IBM Plex", "Libre", "Playfair", "Merriweather", "Lato", "Work", "DM"};
    const char* second[] = {"Sans", "Serif", "Mono", "Display", "Slab", "Condensed", "Rounded", "Script"};
    const char* third[] = {"Thai", "Arabic", "Hebrew", "JP", "KR", "SC", "TC", "Devanagari", "Light", "Black", "Text", "UI"};
    for (const char* a : first) {
        for (const char* b : second) {
            for (const char* c : third)
                big.push_back(entry(QStringLiteral("%1 %2 %3").arg(a, b, c), {kLatin, kThai}, Kind::Sans, true, true, 6));
        }
    }
    big.reserve(big.size() + 1);
    REQUIRE(big.size() > 1000);
    for (const QString& text : {QStringLiteral("Thai"), QStringLiteral("thai sans"), QStringLiteral("arail"), QStringLiteral("T-H-A-I"),
                                QString::fromUtf8("日本語"), QStringLiteral("noto serif")}) {
        QElapsedTimer timer;
        timer.start();
        fonts::Query q;
        q.text = text;
        const auto rows = fonts::rank(big, q);
        const qint64 ms = timer.elapsed();
        INFO(text.toStdString() << ": " << rows.size() << " rows in " << ms << " ms");
        CHECK(ms < 400); // far from instant would be seconds; a keystroke may take a few frames on a busy machine
        if (ms > 60)
            WARN("slow font search for " << text.toStdString() << ": " << ms << " ms");
        CHECK(rows.size() <= q.limit);
    }
}

TEST_CASE("Font helpers: the scripts of a text and of a language and the edit distance", "[app][fonts][search]") {
    using fonts::scriptsOfText;
    CHECK(scriptsOfText(QString::fromUtf8("สวัสดี Hello")) == std::vector<int>{kThai, kLatin});
    CHECK(scriptsOfText(QString::fromUtf8("Привет, мир")) == std::vector<int>{kCyrillic});
    CHECK(scriptsOfText(QString::fromUtf8("你好")) == std::vector<int>{kChinese});
    CHECK(scriptsOfText(QString::fromUtf8("こんにちは 日本")) == std::vector<int>{kJapanese});
    CHECK(scriptsOfText(QString::fromUtf8("안녕하세요")) == std::vector<int>{kKorean});
    CHECK(scriptsOfText(QString::fromUtf8("שלום")) == std::vector<int>{kHebrew});
    CHECK(scriptsOfText(QString::fromUtf8("مرحبا")) == std::vector<int>{kArabic});
    CHECK(scriptsOfText(QString::fromUtf8("Tiếng Việt")) == std::vector<int>{kLatin, kVietnamese});
    CHECK(scriptsOfText(QStringLiteral("123 - 456 !?")).empty()); // digits and marks need no script
    CHECK(fonts::scriptOfLanguage(QStringLiteral("th")) == kThai);
    CHECK(fonts::scriptOfLanguage(QStringLiteral("zh-Hant")) == QFontDatabase::TraditionalChinese);
    CHECK(fonts::scriptOfLanguage(QStringLiteral("zh-CN")) == kChinese);
    CHECK(fonts::scriptOfLanguage(QStringLiteral("he")) == kHebrew);
    CHECK(fonts::scriptOfLanguage(QStringLiteral("uk")) == kCyrillic);
    CHECK(fonts::scriptOfLanguage(QStringLiteral("en")) == kLatin);
    CHECK(fonts::scriptOfLanguage(QStringLiteral("pl")) == kLatin);
    CHECK(fonts::scriptOfLanguage(QString()) == -1);
    CHECK(fonts::scriptOfLanguage(QStringLiteral("auto")) == -1);
    CHECK(fonts::editDistance(QStringLiteral("arail"), QStringLiteral("arial"), 1) == 1);  // two letters swapped
    CHECK(fonts::editDistance(QStringLiteral("abc"), QStringLiteral("abd"), 1) == 1);
    CHECK(fonts::editDistance(QStringLiteral("abc"), QStringLiteral("xyz"), 2) == 3);       // over the limit: limit + 1
    CHECK(fonts::editDistance(QStringLiteral("same"), QStringLiteral("same"), 2) == 0);
    CHECK(fonts::fold(QStringLiteral("Noto Sans-Thai")) == QStringLiteral("notosansthai"));
    CHECK(fonts::fold(QString::fromUtf8("Café Sans")) == QStringLiteral("cafesans"));
    CHECK(fonts::classify(QStringLiteral("Cascadia Code"), false, false) == Kind::Mono);
    CHECK(fonts::classify(QStringLiteral("Noto Serif Thai"), false, false) == Kind::Serif);
    CHECK(fonts::classify(QStringLiteral("Noto Sans Thai"), false, false) == Kind::Sans);
    CHECK(fonts::classify(QStringLiteral("Anything"), true, false) == Kind::Mono);
    CHECK(fonts::classify(QStringLiteral("Wingdings"), false, true) == Kind::Symbol);
}

TEST_CASE("The font catalogue of this computer ranks its own fonts and remembers favourites and recent fonts", "[app][fonts][catalog]") {
    resetSettings();
    {
        FontCatalogBridge catalog;
        catalog.rebuildNow();
        REQUIRE(catalog.ready());
        REQUIRE(catalog.count() > 5);
        // Searching never crashes and groups come in order whatever is installed.
        for (const QString& text : {QStringLiteral("Thai"), QStringLiteral("Japanese"), QStringLiteral("Arabic"), QStringLiteral("sans")}) {
            INFO(text.toStdString());
            const QVariantMap result = catalog.search(text);
            const QVariantList rows = result.value(QStringLiteral("rows")).toList();
            CHECK(result.value(QStringLiteral("total")).toInt() == rows.size());
            int last = -1;
            for (const QVariant& v : rows) {
                const QString group = v.toMap().value(QStringLiteral("group")).toString();
                const int rank = group == QLatin1String("name") ? 0 : group == QLatin1String("language") ? 1 : 2;
                CHECK(rank >= last);
                last = std::max(last, rank);
            }
        }
#ifdef _WIN32
        // Windows ships Tahoma and Leelawadee UI: both draw Thai, and the real glyph check agrees.
        const QVariantList thai = catalog.search(QStringLiteral("Thai")).value(QStringLiteral("rows")).toList();
        QStringList found;
        for (const QVariant& v : thai)
            found << v.toMap().value(QStringLiteral("family")).toString();
        INFO(found.join(", ").toStdString());
        CHECK(found.contains(QStringLiteral("Tahoma")));
        CHECK(catalog.missingGlyphs(QStringLiteral("Tahoma"), QString::fromUtf8("สวัสดี")).isEmpty());
        CHECK_FALSE(catalog.missingGlyphs(QStringLiteral("Arial"), QString::fromUtf8("สวัสดี")).isEmpty()); // Arial has no Thai
        CHECK(catalog.missingGlyphs(QStringLiteral("Arial"), QStringLiteral("Hello 123")).isEmpty());
        CHECK(catalog.isInstalled(QStringLiteral("arial")));
        CHECK_FALSE(catalog.isInstalled(QStringLiteral("No Such Family 12345")));
#else
        WARN("the Thai font checks run on Windows (Tahoma, Leelawadee UI); other systems are checked by the ranking tests");
#endif
        // Favourites and recently used fonts are kept between sessions.
        const QStringList first = catalog.search(QString()).value(QStringLiteral("rows")).toList().mid(0, 2).isEmpty()
                                      ? QStringList()
                                      : QStringList{catalog.search(QString()).value(QStringLiteral("rows")).toList().at(0).toMap().value(QStringLiteral("family")).toString()};
        REQUIRE_FALSE(first.isEmpty());
        catalog.setFavourite(first.front(), true);
        catalog.noteUsed(first.front());
        catalog.noteUsed(QStringLiteral("Zzz Recent"));
        CHECK(catalog.isFavourite(first.front()));
        CHECK(catalog.recent().first() == QStringLiteral("Zzz Recent"));
        CHECK(catalog.recent().at(1) == first.front());
        // Using a font again moves it to the front once; the list is capped.
        catalog.noteUsed(first.front());
        CHECK(catalog.recent() == QStringList{first.front(), QStringLiteral("Zzz Recent")});
        for (int i = 0; i < 40; ++i)
            catalog.noteUsed(QStringLiteral("Family %1").arg(i));
        CHECK(catalog.recent().size() == 24);
        CHECK(catalog.recent().first() == QStringLiteral("Family 39"));
    }
    FontCatalogBridge again;
    CHECK(again.recent().first() == QStringLiteral("Family 39"));
    CHECK(again.favourites().size() == 1);
    again.setFavourite(again.favourites().first(), false);
    CHECK(again.favourites().isEmpty());
    // The rows tell favourites and sample text apart.
    again.setEntries(system());
    again.setFavourite(QStringLiteral("Georgia"), true);
    const QVariantList rows = again.search(QStringLiteral("Japanese")).value(QStringLiteral("rows")).toList();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].toMap().value(QStringLiteral("sample")).toString() == QString::fromUtf8("こんにちは")); // the greeting in the script
    CHECK(rows[0].toMap().value(QStringLiteral("groupLabel")).toString() == QStringLiteral("Supports Japanese"));
    const QVariantList names = again.search(QStringLiteral("Georgia")).value(QStringLiteral("rows")).toList();
    REQUIRE_FALSE(names.isEmpty());
    CHECK(names[0].toMap().value(QStringLiteral("sample")).toString() == QStringLiteral("Georgia")); // its own name
    CHECK(names[0].toMap().value(QStringLiteral("favourite")).toBool());
    CHECK(names[0].toMap().value(QStringLiteral("groupLabel")).toString() == QStringLiteral("Name"));
    CHECK(again.scriptOfLanguage(QStringLiteral("th")) == QStringLiteral("thai"));
    CHECK(again.scriptsOfText(QString::fromUtf8("สวัสดี")) == QStringList{"thai"});
    CHECK(again.scripts().size() >= 12);
    CHECK(again.scripts().first().toMap().value(QStringLiteral("key")).toString() == QStringLiteral("latin"));
    resetSettings();
}

TEST_CASE("The shared font picker in the shell: search and filters and grouped rows; screenshots", "[app][fonts][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* catalog = shell.engine.singletonInstance<FontCatalogBridge*>("Wabcut", "Fonts");
            REQUIRE(catalog);
            catalog->setEntries(system());
            QQmlComponent component(&shell.engine);
            component.setData("import QtQuick\nimport Wabcut\nItem {\n    property alias pill: choice\n"
                              "    FontChoicePill {\n        id: choice\n        x: 40\n        y: 40\n        family: \"Tahoma\"\n"
                              "        sampleText: \"\\u0e2a\\u0e27\\u0e31\\u0e2a\\u0e14\\u0e35 Hello\"\n"
                              "        extras: [{ id: \"\", name: \"Default (sans-serif)\" }]\n    }\n}\n",
                              QUrl(QStringLiteral("qrc:/d2/FontHost.qml")));
            std::unique_ptr<QObject> object(component.create());
            INFO(component.errorString().toStdString());
            REQUIRE(object);
            auto* host = qobject_cast<QQuickItem*>(object.get());
            REQUIRE(host);
            host->setParentItem(shell.window->contentItem());
            host->setSize(shell.window->contentItem()->size());
            auto* pill = host->property("pill").value<QQuickItem*>();
            REQUIRE(pill);
            CHECK(pill->property("text").toString() == QStringLiteral("Tahoma"));
            auto* picker = pill->property("picker").value<QObject*>();
            REQUIRE(picker);
            REQUIRE(QMetaObject::invokeMethod(picker, "open"));
            REQUIRE(waitFor([&] { return picker->property("opened").toBool(); }, 3000));
            // Opened over Thai text: the language filter starts on Thai, and the rows are the Thai fonts.
            CHECK(picker->property("scriptKeys").toStringList() == QStringList{"thai"});
            auto* content = picker->property("contentItem").value<QQuickItem*>();
            REQUIRE(content);
            QQuickItem* filter = namedIn(content, QStringLiteral("fontPickerFilter"));
            QQuickItem* list = namedIn(content, QStringLiteral("fontPickerList"));
            QQuickItem* status = namedIn(content, QStringLiteral("fontPickerStatus"));
            REQUIRE(filter);
            REQUIRE(list);
            REQUIRE(status);
            spin(100);
            CHECK(list->property("count").toInt() == 6); // the six fonts that draw Thai (a filter hides the default-font row)
            // Typing Thai: names first.
            filter->setProperty("text", QStringLiteral("Thai"));
            spin(100);
            const QVariantList rows = picker->property("rows").toList();
            REQUIRE(rows.size() >= 3);
            CHECK(rows[0].toMap().value(QStringLiteral("group")).toString() == QStringLiteral("name"));
            CHECK(rows[0].toMap().value(QStringLiteral("family")).toString() == QStringLiteral("Noto Sans Thai"));
            // The filters panel opens and a chip narrows the list.
            picker->setProperty("filtersOpen", true);
            spin(100);
            QQuickItem* italic = namedIn(content, QStringLiteral("fontItalic"));
            QQuickItem* hide = namedIn(content, QStringLiteral("fontHideUnsupported"));
            REQUIRE(italic);
            REQUIRE(hide);
            CHECK(hide->isVisible()); // there is text to draw
            REQUIRE(QMetaObject::invokeMethod(italic, "clicked"));
            spin(100);
            CHECK(picker->property("italic").toBool());
            CHECK(picker->property("filterCount").toInt() == 2); // Thai and italic
            const QVariantList narrowed = picker->property("rows").toList();
            for (const QVariant& v : narrowed) {
                const QString family = v.toMap().value(QStringLiteral("family")).toString();
                CHECK((family == QStringLiteral("Bai Jamjuree") || family == QStringLiteral("TH Sarabun New")));
            }
            spin(150);
            saveShot(shell.grab(), popupFrame(content), dark ? QStringLiteral("font-picker-thai.png") : QStringLiteral("font-picker-thai-light.png"), 8);
            // Clear filters: back to everything that matches the text.
            QQuickItem* clear = namedIn(content, QStringLiteral("fontClearFilters"));
            REQUIRE(clear);
            REQUIRE(QMetaObject::invokeMethod(clear, "clicked"));
            CHECK(picker->property("filterCount").toInt() == 0);
            // Choosing a row reports its family and closes.
            REQUIRE(QMetaObject::invokeMethod(picker, "chooseRow", Q_ARG(QVariant, rows[0])));
            CHECK(catalog->recent().first() == QStringLiteral("Noto Sans Thai")); // the pill's chosen() noted it as used
            CHECK_FALSE(picker->property("opened").toBool());
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}

// ---- The Transcript tab: the language spoken, Transcribe again, Delete transcript ----------------------------------------------

namespace {

struct W {
    std::string text;
    double from = 0; ///< source frames (25 fps)
    double to = 0;
    std::optional<int> speaker = std::nullopt;
};

std::shared_ptr<const ai::Transcript> transcript(std::string language, std::vector<W> words) {
    auto t = std::make_shared<ai::Transcript>();
    t->language = std::move(language);
    const double frame = static_cast<double>(framesToTicks(1, kRate).value);
    for (const W& w : words) {
        ai::TranscriptWord tw;
        tw.text = w.text;
        tw.t0 = Ticks{static_cast<std::int64_t>(w.from * frame)};
        tw.t1 = Ticks{static_cast<std::int64_t>(w.to * frame)};
        tw.confidence = 0.95f;
        tw.speakerId = w.speaker;
        t->words.push_back(std::move(tw));
    }
    return t;
}

/// `state` with media `media` read from a file that exists (a few bytes: a job only needs the path).
ProjectState withMediaFile(ProjectState state, MediaId media, const QString& file) {
    {
        std::ofstream(file.toStdString()) << "RIFF";
    }
    for (MediaPtr& m : state.media) {
        if (m->id == media) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->locator.path = file.toStdString();
            m = copy;
        }
    }
    return state;
}

QStringList wordTexts(const TranscriptBridge& bridge) {
    QStringList out;
    const auto& st = bridge.sequenceTranscript();
    for (std::size_t i = 0; i < st.size(); ++i)
        out << QString::fromStdString(std::string(st.text(i)));
    return out;
}

/// One linked A/V clip of media A (60 frames), optionally with a transcript.
struct TranscriptFixture {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    QTemporaryDir dir; // the media needs a file for a job: A points at one
    ProjectController controller;
    TranscriptBridge bridge;

    explicit TranscriptFixture(std::shared_ptr<const ai::Transcript> words = nullptr) {
        tl.linkMatching();
        REQUIRE(dir.isValid());
        controller.resetProject(withMediaFile(tl.state, tl.media('A'), dir.filePath(QStringLiteral("a.wav"))), tl.seq);
        bridge.setUseAnalysisCache(false);
        bridge.setController(&controller);
        if (words)
            bridge.addTranscript(tl.media('A'), 0, words);
    }
    /// A transcriber under "fake" that says `words` (its language `language`).
    void fake(std::string language, std::vector<W> words, bool* ran = nullptr) {
        auto result = transcript(std::move(language), std::move(words));
        bridge.setTranscriber(QStringLiteral("fake"), QStringLiteral("This computer (test)"),
                              [result, ran](const std::string&, int, const std::function<void(float)>&, std::stop_token) {
                                  if (ran)
                                      *ran = true;
                                  return std::expected<ai::Transcript, Error>(*result);
                              });
        bridge.setProvider(QStringLiteral("fake"));
    }
    bool jobDone() { return waitFor([&] { return bridge.jobStatus() != QLatin1String("running"); }, 5000); }
};

} // namespace

TEST_CASE("Transcript language: the choice is remembered and reaches the job and an unknown one is refused", "[app][transcript][language]") {
    resetSettings();
    QSettings().remove(QStringLiteral("transcript"));
    TranscriptFixture fx;
    TranscriptBridge& t = fx.bridge;
    // The list: every language of the recogniser by English name, the card's languages among them, with their own names.
    const QVariantList all = t.spokenLanguages();
    REQUIRE(all.size() >= 99);
    QStringList names;
    for (const QVariant& v : all)
        names << v.toMap().value(QStringLiteral("name")).toString();
    CHECK(std::is_sorted(names.begin(), names.end()));
    const auto row = [&](const char* code) {
        for (const QVariant& v : all) {
            if (v.toMap().value(QStringLiteral("code")).toString() == QLatin1String(code))
                return v.toMap();
        }
        return QVariantMap{};
    };
    for (const char* code : {"id", "he", "uk", "ar", "pl", "tr", "is", "fi", "cs", "sk", "ro", "bs", "hr", "sr"})
        CHECK_FALSE(row(code).isEmpty());
    CHECK(row("he").value(QStringLiteral("label")).toString() == QString::fromUtf8("Hebrew · עברית"));
    CHECK(row("he").value(QStringLiteral("rtl")).toBool());
    CHECK_FALSE(row("is").value(QStringLiteral("rtl")).toBool());
    CHECK(row("is").value(QStringLiteral("tier")).toString() == QStringLiteral("limited"));
    CHECK(row("pl").value(QStringLiteral("tier")).toString() == QStringLiteral("strong"));

    // Auto-detect first, remembered between sessions.
    CHECK(t.spokenLanguage() == QStringLiteral("auto"));
    CHECK(t.spokenLanguageLabel() == QStringLiteral("Auto-detect"));
    CHECK(t.spokenLanguageNote().isEmpty());
    t.setSpokenLanguage(QStringLiteral("he-IL")); // a regional tag is Hebrew
    CHECK(t.spokenLanguage() == QStringLiteral("he"));
    CHECK(t.spokenLanguageRtl());
    CHECK(t.spokenLanguageLabel() == QString::fromUtf8("Hebrew · עברית"));
    CHECK(QSettings().value(QStringLiteral("transcript/spokenLanguage")).toString() == QStringLiteral("he"));
    t.setSpokenLanguage(QStringLiteral("klingon")); // refused: nothing changes
    CHECK(t.spokenLanguage() == QStringLiteral("he"));
    {
        TranscriptBridge later;
        CHECK(later.spokenLanguage() == QStringLiteral("he")); // the next session starts there
    }
    // A language Whisper knows little says so; the translate note follows the toggle.
    t.setSpokenLanguage(QStringLiteral("is"));
    CHECK(t.spokenLanguageNote().contains(QStringLiteral("Icelandic")));
    t.setProvider(QStringLiteral("local"));
    t.setTranslateToEnglish(true);
    CHECK(QSettings().value(QStringLiteral("transcript/translateToEnglish")).toBool());
    t.setProvider(QStringLiteral("cloud:some-model"));
    CHECK(t.spokenLanguageNote().contains(QStringLiteral("runs on this computer")));
    t.setTranslateToEnglish(false);
    t.setSpokenLanguage(QStringLiteral("auto"));
    CHECK(t.spokenLanguage() == QStringLiteral("auto"));
    resetSettings();
}

TEST_CASE("Transcribing names the language and translate and Transcribe again replaces the transcript", "[app][transcript][language]") {
    resetSettings();
    QSettings().remove(QStringLiteral("transcript"));
    TranscriptFixture fx;
    TranscriptBridge& t = fx.bridge;
    QStringList told;
    t.setJobObserver([&](const QString& language, bool translate, bool redo) {
        told << QStringLiteral("%1/%2/%3").arg(language).arg(translate ? "translate" : "same").arg(redo ? "redo" : "new");
    });
    bool ran = false;
    fx.fake("he", {{"shalom", 0, 8}, {"olam", 10, 20}}, &ran);

    // Hebrew named, nothing transcribed yet: the job is told.
    t.setSpokenLanguage(QStringLiteral("he"));
    REQUIRE(t.transcribe(QStringLiteral("sequence")).isEmpty());
    REQUIRE(fx.jobDone());
    CHECK(ran);
    CHECK(told == QStringList{"he/same/new"});
    CHECK(wordTexts(t) == QStringList{"shalom", "olam"});
    CHECK(t.language() == QStringLiteral("he"));
    CHECK(t.rtl()); // Hebrew reads right to left

    // A transcript that is there: Transcribe says so; Transcribe again (redo) runs and replaces it.
    CHECK_FALSE(t.transcribe(QStringLiteral("sequence")).isEmpty());
    CHECK(told.size() == 1);
    CHECK(t.transcriptCount(QStringLiteral("sequence")) == 1);
    fx.fake("en", {{"hello", 0, 8}, {"again", 10, 20}});
    t.setSpokenLanguage(QStringLiteral("en"));
    REQUIRE(t.transcribe(QStringLiteral("sequence"), false, true).isEmpty());
    REQUIRE(fx.jobDone());
    CHECK(told.last() == QStringLiteral("en/same/redo"));
    CHECK(wordTexts(t) == QStringList{"hello", "again"});
    CHECK_FALSE(t.rtl());

    // A redo that fails leaves the old transcript where it was.
    t.setTranscriber(QStringLiteral("fake"), QStringLiteral("This computer (test)"),
                     [](const std::string&, int, const std::function<void(float)>&, std::stop_token) {
                         return std::expected<ai::Transcript, Error>(std::unexpected(Error{ErrorCode::IoError, "the helper stopped"}));
                     });
    REQUIRE(t.transcribe(QStringLiteral("sequence"), false, true).isEmpty());
    REQUIRE(fx.jobDone());
    CHECK(t.jobStatus() == QStringLiteral("error"));
    CHECK(wordTexts(t) == QStringList{"hello", "again"});

    // Translate runs on this computer: a cloud transcriber is refused with why; the fake one (a local stand-in) is allowed.
    t.setTranslateToEnglish(true);
    t.setProvider(QStringLiteral("cloud:some-model"));
    const QString why = t.transcribe(QStringLiteral("sequence"), false, true);
    CHECK(why.contains(QStringLiteral("runs on this computer")));
    fx.fake("en", {{"hello", 0, 8}});
    t.setSpokenLanguage(QStringLiteral("is"));
    REQUIRE(t.transcribe(QStringLiteral("sequence"), false, true).isEmpty());
    REQUIRE(fx.jobDone());
    CHECK(told.last() == QStringLiteral("is/translate/redo"));
    resetSettings();
}

TEST_CASE("Delete transcript removes the words and Transcribe starts again from nothing" , "[app][transcript][delete]") {
    resetSettings();
    TranscriptFixture fx(transcript("en", {{"Hello", 0, 4}, {"world", 5, 9}}));
    TranscriptBridge& t = fx.bridge;
    REQUIRE(t.wordCount() == 2);
    CHECK(t.transcriptCount(QStringLiteral("sequence")) == 1);
    CHECK(t.transcriptCount(QStringLiteral("selection")) == 0); // nothing selected
    const std::size_t undo = fx.controller.session().undoCount();
    REQUIRE(t.deleteTranscript(QStringLiteral("sequence")).isEmpty());
    CHECK(t.wordCount() == 0);
    CHECK(t.message().contains(QStringLiteral("Deleted")));
    CHECK(fx.controller.session().undoCount() == undo); // the transcript is not the project's: no undo step
    CHECK(t.transcriptCount(QStringLiteral("sequence")) == 0);
    // Again: nothing to delete, said so.
    CHECK_FALSE(t.deleteTranscript(QStringLiteral("sequence")).isEmpty());
    CHECK(t.message().contains(QStringLiteral("Nothing to delete")));
    // Transcribe starts from nothing.
    bool ran = false;
    fx.fake("en", {{"fresh", 0, 4}}, &ran);
    REQUIRE(t.transcribe(QStringLiteral("sequence")).isEmpty());
    REQUIRE(fx.jobDone());
    CHECK(ran);
    CHECK(wordTexts(t) == QStringList{"fresh"});
    // Selected clips: only theirs.
    fx.controller.setSelection({fx.controller.currentSequence()->audioTracks[0]->clips[0].id});
    CHECK(t.transcriptCount(QStringLiteral("selection")) == 1);
    REQUIRE(t.deleteTranscript(QStringLiteral("selection")).isEmpty());
    CHECK(t.wordCount() == 0);
    // Not while a transcription runs.
    t.setTranscriber(QStringLiteral("fake"), QStringLiteral("This computer (test)"),
                     [](const std::string&, int, const std::function<void(float)>&, std::stop_token stop) {
                         for (int i = 0; i < 200 && !stop.stop_requested(); ++i)
                             std::this_thread::sleep_for(std::chrono::milliseconds(10));
                         return std::expected<ai::Transcript, Error>(*transcript("en", {{"slow", 0, 4}}));
                     });
    REQUIRE(t.transcribe(QStringLiteral("sequence")).isEmpty());
    CHECK_FALSE(t.deleteTranscript(QStringLiteral("sequence")).isEmpty());
    t.cancelTranscription();
    REQUIRE(waitFor([&] { return t.jobStatus() != QLatin1String("running"); }, 5000));
    resetSettings();
}

TEST_CASE("The Transcript tab in the shell: the language row and its picker and the delete question; screenshots",
          "[app][transcript][language][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            QSettings().remove(QStringLiteral("transcript"));
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
            REQUIRE(t);
            t->setUseAnalysisCache(false);
            Timeline tl(R"(
                V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
                A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
            )");
            tl.linkMatching();
            QTemporaryDir mediaDir;
            REQUIRE(mediaDir.isValid());
            controller->resetProject(withMediaFile(tl.state, tl.media('A'), mediaDir.filePath(QStringLiteral("a.wav"))), tl.seq);
            t->setTranscriber(QStringLiteral("fake"), QStringLiteral("This computer (test)"),
                              [](const std::string&, int, const std::function<void(float)>&, std::stop_token) {
                                  return std::expected<ai::Transcript, Error>(*transcript("he", {{"shalom", 0, 8}}));
                              });
            t->setProvider(QStringLiteral("fake"));
            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
            spin(300);
            REQUIRE_FALSE(shell.grab().isNull()); // lays the hidden window out

            // The set-up: the language button says Auto-detect; Translate to English is off.
            QQuickItem* button = visibleNamed(shell, QStringLiteral("spokenLanguageButton"));
            QQuickItem* translate = visibleNamed(shell, QStringLiteral("translateToEnglish"));
            REQUIRE(button);
            REQUIRE(translate);
            CHECK(button->property("text").toString() == QStringLiteral("Auto-detect"));
            CHECK_FALSE(translate->property("checked").toBool());
            // Scroll the set-up so the row is in view, open the picker and look for Hebrew.
            QQuickItem* setup = visibleNamed(shell, QStringLiteral("transcribeSetup"));
            REQUIRE(setup);
            QQuickItem* setupContent = setup->property("contentItem").value<QQuickItem*>();
            REQUIRE(setupContent);
            const double most = std::max(0.0, setup->property("contentHeight").toDouble() - setup->height());
            setup->setProperty("contentY", std::clamp(button->mapToItem(setupContent, QPointF(0, 0)).y() - 120.0, 0.0, most));
            spin(100);
            REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
            QQuickItem* pickerFilter = nullptr;
            REQUIRE(waitFor([&] { return (pickerFilter = visibleNamed(shell, QStringLiteral("spokenLanguageFilter"))) != nullptr; }, 3000));
            QQuickItem* languageList = visibleNamed(shell, QStringLiteral("spokenLanguageList"));
            REQUIRE(languageList);
            CHECK(languageList->property("count").toInt() >= 100); // Auto-detect and every language
            pickerFilter->setProperty("text", QStringLiteral("heb"));
            spin(100);
            CHECK(languageList->property("count").toInt() == 1);
            pickerFilter->setProperty("text", QString::fromUtf8("עברית")); // by its own name
            spin(100);
            CHECK(languageList->property("count").toInt() == 1);
            pickerFilter->setProperty("text", QStringLiteral("pol"));
            spin(100);
            CHECK(languageList->property("count").toInt() == 1);
            pickerFilter->setProperty("text", QString());
            spin(150);
            saveShot(shell.grab(), popupFrame(languageList->parentItem()),
                     dark ? QStringLiteral("transcript-language-picker.png") : QStringLiteral("transcript-language-picker-light.png"), 8);
            // Choose Hebrew (Enter on the filtered list): the button says so.
            pickerFilter->setProperty("text", QStringLiteral("heb"));
            spin(50);
            REQUIRE(QMetaObject::invokeMethod(pickerFilter, "accepted"));
            CHECK(t->spokenLanguage() == QStringLiteral("he"));
            spin(150);
            CHECK(button->property("text").toString() == QString::fromUtf8("Hebrew · עברית"));
            REQUIRE(QMetaObject::invokeMethod(translate, "toggle"));
            REQUIRE(QMetaObject::invokeMethod(translate, "toggled"));
            CHECK(t->translateToEnglish());
            spin(100);
            saveShot(shell.grab(), setup, dark ? QStringLiteral("transcript-language-setup.png") : QStringLiteral("transcript-language-setup-light.png"));

            // Transcribe (the fake one): Hebrew words arrive and read from the right; Delete transcript asks first.
            t->setTranslateToEnglish(false);
            REQUIRE(t->transcribe(QStringLiteral("sequence")).isEmpty());
            REQUIRE(waitFor([&] { return t->wordCount() == 1; }, 5000));
            spin(300);
            CHECK(t->rtl());
            QQuickItem* more = visibleNamed(shell, QStringLiteral("transcriptMore"));
            REQUIRE(more);
            CHECK(more->isEnabled());
            QObject* panel = shell.item("transcriptPanel");
            REQUIRE(panel);
            // The page's question: opened by the menu's choice (the menu itself needs a pointer: call the page's function).
            QQuickItem* page = visibleNamed(shell, QStringLiteral("transcriptPage"));
            REQUIRE(page);
            REQUIRE(QMetaObject::invokeMethod(page, "openTask", Q_ARG(QString, QStringLiteral("delete")), Q_ARG(QString, QStringLiteral("sequence"))));
            spin(150);
            QQuickItem* taskPanel = visibleNamed(shell, QStringLiteral("transcriptTaskPanel"));
            REQUIRE(taskPanel);
            QQuickItem* taskText = visibleNamed(shell, QStringLiteral("transcriptTaskText"));
            REQUIRE(taskText);
            CHECK(taskText->property("text").toString().contains(QStringLiteral("Delete the transcript of 1 audio file")));
            saveShot(shell.grab(), static_cast<QQuickItem*>(panel), dark ? QStringLiteral("transcript-delete-question.png")
                                                                         : QStringLiteral("transcript-delete-question-light.png"));
            // Cancel changes nothing; Delete removes the words.
            QQuickItem* cancel = visibleNamed(shell, QStringLiteral("transcriptTaskCancel"));
            REQUIRE(cancel);
            REQUIRE(QMetaObject::invokeMethod(cancel, "clicked"));
            spin(50);
            CHECK(visibleNamed(shell, QStringLiteral("transcriptTaskPanel")) == nullptr);
            CHECK(t->wordCount() == 1);
            REQUIRE(QMetaObject::invokeMethod(page, "openTask", Q_ARG(QString, QStringLiteral("delete")), Q_ARG(QString, QStringLiteral("sequence"))));
            spin(50);
            QQuickItem* confirm = visibleNamed(shell, QStringLiteral("transcriptTaskConfirm"));
            REQUIRE(confirm);
            REQUIRE(QMetaObject::invokeMethod(confirm, "clicked"));
            spin(100);
            CHECK(t->wordCount() == 0);
            CHECK(visibleNamed(shell, QStringLiteral("transcribeSetup")) != nullptr); // back to the set-up
            QStringList messages = takeMessages();
            // The Library card reads the fixture's four-byte "a.wav" as its picture: not the panel under test.
            messages.erase(std::remove_if(messages.begin(), messages.end(),
                                          [](const QString& m) { return m.contains(QLatin1String("LibraryCard.qml")) && m.contains(QLatin1String("a.wav")); }),
                           messages.end());
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}

TEST_CASE("Hebrew words read from the right in the Transcript tab; screenshot", "[app][transcript][rtl][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(t);
    t->setUseAnalysisCache(false);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    t->addTranscript(tl.media('A'), 0,
                     transcript("he", {{"\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", 0, 8},      // shalom
                                       {"\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D", 10, 20},    // olam
                                       {"\xD7\x9E\xD7\x94", 22, 28},                    // ma
                                       {"\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9E\xD7\x9A", 30, 40}})); // shlomcha
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull());
    CHECK(t->language() == QStringLiteral("he"));
    CHECK(t->rtl());
    // The first word sits to the right of the second.
    QQuickItem* first = findItem(shell.window->contentItem(), [](QQuickItem* i) {
        return i->isVisible() && i->property("wordIndex").isValid() && i->property("wordIndex").toInt() == 0;
    });
    QQuickItem* second = findItem(shell.window->contentItem(), [](QQuickItem* i) {
        return i->isVisible() && i->property("wordIndex").isValid() && i->property("wordIndex").toInt() == 1;
    });
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->mapToScene(QPointF(0, 0)).x() > second->mapToScene(QPointF(0, 0)).x());
    QQuickItem* list = visibleNamed(shell, QStringLiteral("transcriptList"));
    REQUIRE(list);
    saveShot(shell.grab(), list, QStringLiteral("transcript-hebrew-rtl.png"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- The Caption Style panel ---------------------------------------------------------------------------------------------------

namespace {

/// An English caption track over a 60-frame clip, in the Captions workspace.
struct CaptionFixture {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
};

} // namespace

TEST_CASE("Caption Style: the gallery and animations and emphasis and text rules reach the style through the panel's bridge",
          "[app][captions][style][animation]") {
    resetSettings();
    TranscriptFixture fx;
    CaptionsBridge captions;
    captions.setController(&fx.controller);
    captions.setTranscript(&fx.bridge);
    REQUIRE(captions.addTrack(QStringLiteral("English"), {}).isEmpty());
    const auto track = [&]() -> const CaptionTrack& { return fx.controller.currentSequence()->captionTracks.front(); };

    // The gallery: the eleven built-in presets with what a tile needs to show the look moving.
    const QVariantList presets = captions.presets();
    REQUIRE(presets.size() == 11);
    QStringList names;
    for (const QVariant& v : presets)
        names << v.toMap().value(QStringLiteral("name")).toString();
    CHECK(names == QStringList{"Clean", "Bold Box", "Podcast", "Shorts", "Thai-friendly", "Word Pop", "Highlighter", "Bounce",
                               "Typewriter", "Zoom", "Hype"});
    const auto preset = [&](const char* name) {
        for (const QVariant& v : presets) {
            if (v.toMap().value(QStringLiteral("name")).toString() == QLatin1String(name))
                return v.toMap();
        }
        return QVariantMap{};
    };
    CHECK(preset("Highlighter").value(QStringLiteral("animation")).toString() == QStringLiteral("highlightBar"));
    CHECK(preset("Word Pop").value(QStringLiteral("animationSpeed")).toDouble() == Approx(0.18));
    CHECK(preset("Hype").value(QStringLiteral("textCase")).toString() == QStringLiteral("upper"));
    CHECK(preset("Hype").value(QStringLiteral("stripPunctuation")).toBool());
    CHECK(preset("Hype").value(QStringLiteral("emphasis")).toString() == QStringLiteral("auto"));

    // Applying a gallery preset is one undo step and the style map reads it back.
    const std::size_t undo = fx.controller.session().undoCount();
    REQUIRE(captions.applyPreset(QStringLiteral("Bounce")).isEmpty());
    CHECK(fx.controller.session().undoCount() == undo + 1);
    CHECK(captions.style().value(QStringLiteral("animation")).toString() == QStringLiteral("bounce"));
    CHECK(track().style.animation == CaptionAnimation::Bounce);

    // Every new control is one Captions.setStyle(key, value).
    for (const auto& [key, value] : std::vector<std::pair<QString, QVariant>>{
             {"animation", "highlightBar"}, {"animationSpeed", 0.3}, {"animationEase", "linear"}, {"emphasis", "keywords"},
             {"emphasisColor", "#FF0000"}, {"emphasisScale", 1.3}, {"textCase", "lower"}, {"stripPunctuation", true},
             {"stripAccents", true}}) {
        INFO(key.toStdString());
        CHECK(captions.setStyle(key, value).isEmpty());
    }
    const CaptionStyle& s = track().style;
    CHECK(s.animation == CaptionAnimation::HighlightBar);
    CHECK(s.animationSpeed == Approx(0.3));
    CHECK(s.animationEase == CaptionEase::Linear);
    CHECK(s.emphasis == CaptionEmphasis::Keywords);
    CHECK(s.emphasisColor == CaptionRgb{1.0, 0.0, 0.0});
    CHECK(s.emphasisScale == Approx(1.3));
    CHECK(s.textCase == CaptionTextCase::Lower);
    CHECK(s.stripPunctuation);
    CHECK(s.stripAccents);
    // The key words: commas, semicolons and line breaks separate them; each once without regard to case.
    CHECK(captions.setStyle(QStringLiteral("emphasisWords"), QStringLiteral("GG, win;  gg\nclutch ,")).isEmpty());
    CHECK(track().style.emphasisWords == std::vector<std::string>{"GG", "win", "clutch"});
    CHECK(captions.style().value(QStringLiteral("emphasisWordsText")).toString() == QStringLiteral("GG, win, clutch"));
    CHECK_FALSE(captions.setStyle(QStringLiteral("emphasisWords"), QString(100, QLatin1Char('x'))).isEmpty()); // too long
    // Out of range values are clamped (sliders) or refused (names).
    CHECK(captions.setStyle(QStringLiteral("animationSpeed"), 9.0).isEmpty());
    CHECK(track().style.animationSpeed == Approx(1.0));
    CHECK_FALSE(captions.setStyle(QStringLiteral("animation"), QStringLiteral("spin")).isEmpty());
    CHECK_FALSE(captions.setStyle(QStringLiteral("textCase"), QStringLiteral("title")).isEmpty());
    // A project's own preset named like a gallery one is shown instead of it (projects made before the gallery).
    CHECK(captions.savePreset(QStringLiteral("My look")).isEmpty());
    CHECK(captions.presets().size() == 12);
    resetSettings();
}

TEST_CASE("The Caption Style panel in the Captions workspace: the animated gallery and animation and emphasis and text rules; screenshots",
          "[app][captions][style][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
            REQUIRE(controller);
            REQUIRE(captions);
            REQUIRE(captions->addTrack(QStringLiteral("English"), {}).isEmpty());
            REQUIRE(captions->applyPreset(QStringLiteral("Hype")).isEmpty());
            shell.window->setProperty("workspace", 4); // Captions
            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
            REQUIRE(waitFor([&] { return shell.item("transcriptPanel") != nullptr; }, 5000));
            spin(300);
            QQuickItem* tabs = visibleNamed(shell, QStringLiteral("transcriptTabs"));
            REQUIRE(tabs);
            tabs->setProperty("currentIndex", 2); // Captions
            spin(300);
            REQUIRE_FALSE(shell.grab().isNull());
            QQuickItem* style = visibleNamed(shell, QStringLiteral("captionStylePanel"));
            REQUIRE(style);
            QQuickItem* presets = namedIn(style, QStringLiteral("captionPresets"));
            REQUIRE(presets);
            // Every gallery preset has a tile with the moving sample.
            for (const char* name : {"Word Pop", "Highlighter", "Bounce", "Typewriter", "Zoom", "Hype"}) {
                INFO(name);
                QQuickItem* tile = namedIn(style, QStringLiteral("captionPresetSample_") + QLatin1String(name));
                REQUIRE(tile);
                CHECK(tile->property("anim").toString() != QStringLiteral("none"));
            }
            CHECK(namedIn(style, QStringLiteral("captionPresetSample_Hype"))->property("words").toStringList().first() == QStringLiteral("SAY"));
            CHECK(namedIn(style, QStringLiteral("captionPresetSample_Clean"))->property("anim").toString() == QStringLiteral("none"));
            // The panel is a flickable: scroll to the gallery, then to the animation, the emphasis and the text rules.
            QQuickItem* flick = presets->parentItem();
            while (flick != nullptr && !flick->inherits("QQuickFlickable"))
                flick = flick->parentItem();
            REQUIRE(flick);
            QQuickItem* content = flick->property("contentItem").value<QQuickItem*>();
            REQUIRE(content);
            const auto scrollTo = [&](QQuickItem* target, double above = 8.0) {
                const double room = std::max(0.0, flick->property("contentHeight").toDouble() - flick->height());
                flick->setProperty("contentY", std::clamp(target->mapToItem(content, QPointF(0, 0)).y() - above, 0.0, room));
                spin(150);
            };
            scrollTo(presets, 24.0);
            saveShot(shell.grab(), style, dark ? QStringLiteral("caption-style-gallery.png") : QStringLiteral("caption-style-gallery-light.png"));

            // The animation controls: speed and curve show with an animation; emphasis fields with an emphasis.
            REQUIRE(captions->setStyle(QStringLiteral("animation"), QStringLiteral("highlightBar")).isEmpty());
            spin(150);
            QQuickItem* animation = namedIn(style, QStringLiteral("captionAnimation"));
            REQUIRE(animation);
            CHECK(animation->property("text").toString() == QStringLiteral("Highlight bar"));
            QQuickItem* speed = namedIn(style, QStringLiteral("captionAnimationSpeed"));
            QQuickItem* ease = namedIn(style, QStringLiteral("captionAnimationEase"));
            REQUIRE(speed);
            REQUIRE(ease);
            CHECK(speed->isVisible());
            CHECK(ease->isVisible());
            REQUIRE(QMetaObject::invokeMethod(speed, "valueModified", Q_ARG(double, 0.3)));
            CHECK(controller->currentSequence()->captionTracks.front().style.animationSpeed == Approx(0.3));
            QQuickItem* emphasisWords = namedIn(style, QStringLiteral("captionEmphasisWords"));
            REQUIRE(emphasisWords);
            CHECK(emphasisWords->isVisible()); // Hype emphasises automatically
            emphasisWords->setProperty("text", QStringLiteral("GG, win"));
            REQUIRE(QMetaObject::invokeMethod(emphasisWords, "editingFinished"));
            CHECK(controller->currentSequence()->captionTracks.front().style.emphasisWords == std::vector<std::string>{"GG", "win"});
            scrollTo(animation, 80.0);
            saveShot(shell.grab(), style, dark ? QStringLiteral("caption-style-animation.png") : QStringLiteral("caption-style-animation-light.png"));

            // Text rules: the toggles and the case menu are the style.
            QQuickItem* punctuation = namedIn(style, QStringLiteral("captionStripPunctuation"));
            QQuickItem* accents = namedIn(style, QStringLiteral("captionStripAccents"));
            QQuickItem* textCase = namedIn(style, QStringLiteral("captionTextCase"));
            REQUIRE(punctuation);
            REQUIRE(accents);
            REQUIRE(textCase);
            CHECK(punctuation->property("checked").toBool()); // Hype
            CHECK_FALSE(accents->property("checked").toBool());
            CHECK(textCase->property("text").toString() == QStringLiteral("Uppercase"));
            REQUIRE(QMetaObject::invokeMethod(accents, "toggle"));
            REQUIRE(QMetaObject::invokeMethod(accents, "toggled"));
            CHECK(controller->currentSequence()->captionTracks.front().style.stripAccents);
            REQUIRE(QMetaObject::invokeMethod(textCase, "chosen", Q_ARG(QString, QStringLiteral("lower"))));
            CHECK(controller->currentSequence()->captionTracks.front().style.textCase == CaptionTextCase::Lower);
            scrollTo(textCase, 80.0);
            saveShot(shell.grab(), style, dark ? QStringLiteral("caption-style-text-rules.png") : QStringLiteral("caption-style-text-rules-light.png"));
            shell.window->setProperty("workspace", 0);
            spin(100);
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}

// ---- Audio Tools: auto-mix of several voices ----------------------------------------------------------------------------------

namespace {

/// Speech by track: the ranges a test gave each track.
class SpeechByTrack final : public audio::edit::SpeechDetector {
public:
    void set(TrackId track, std::vector<TimeRange> speech) { m_speech[track.value] = std::move(speech); }
    std::expected<std::vector<TimeRange>, Error> speech(const ProjectState&, SequenceId, std::span<const TrackId> tracks,
                                                        audio::edit::SpeechSource, std::optional<TimeRange>,
                                                        command::ExecutionControl*) override {
        std::vector<TimeRange> out;
        for (const TrackId t : tracks) {
            if (const auto it = m_speech.find(t.value); it != m_speech.end())
                out.insert(out.end(), it->second.begin(), it->second.end());
        }
        return out;
    }

private:
    std::map<std::uint64_t, std::vector<TimeRange>> m_speech;
};

TimeRange seconds(double from, double to) {
    return TimeRange::fromStartEnd(Ticks{static_cast<std::int64_t>(from * static_cast<double>(kTicksPerSecond))},
                                   Ticks{static_cast<std::int64_t>(to * static_cast<double>(kTicksPerSecond))});
}

/// A host on A1 and a guest on A2 (both Dialogue), music on A3: 8 s at 25 fps.
struct MixFixture {
    Timeline tl{R"(
        V1 tgt          |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A2 dialogue     |Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
        A3 music        |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )"};
    ProjectController controller;
    std::shared_ptr<SpeechByTrack> speech = std::make_shared<SpeechByTrack>();

    MixFixture() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        const SequencePtr seq = controller.currentSequence();
        speech->set(seq->audioTracks[0]->id, {seconds(1.0, 3.0)});
        speech->set(seq->audioTracks[1]->id, {seconds(4.0, 6.0)});
    }
    std::shared_ptr<audio::edit::AudioServices> services() const {
        return std::make_shared<audio::edit::AudioServices>(nullptr, speech);
    }
    double lowest(int track) {
        const ParamValue v = controller.currentSequence()->audioTracks[static_cast<std::size_t>(track)]->clips[0].properties()[ParamId::VolumeDb];
        double out = v.isCurve() ? 0.0 : v.valueAt(Ticks{});
        if (v.isCurve()) {
            for (const Keyframe& k : v.curve()->keys)
                out = std::min(out, k.value);
        }
        return out;
    }
};

} // namespace

TEST_CASE("Audio tools: Several voices lowers the quiet microphones against each other with one undo step", "[app][audio][duck][automix]") {
    resetSettings();
    MixFixture fx;
    AudioMixerBridge mixer;
    mixer.setController(&fx.controller);
    mixer.setServices(fx.services());
    mixer.setSynchronous(true);

    // The mode: its tracks are the dialogue-role ones; its timing is its own.
    CHECK(mixer.duckMode() == QStringLiteral("default"));
    CHECK(mixer.gameTracks().value(QStringLiteral("mix")).toStringList().size() == 2);
    mixer.setDuckMode(QStringLiteral("autoMix"));
    CHECK(mixer.duckMode() == QStringLiteral("autoMix"));
    CHECK(mixer.duckAmount() == Approx(-12.0));
    CHECK(mixer.duckAttack() == Approx(0.1));
    CHECK(mixer.duckRelease() == Approx(0.5));
    CHECK(mixer.duckHold() == Approx(0.4));
    CHECK_FALSE(mixer.duckGate());
    mixer.setDuckMode(QStringLiteral("default"));
    CHECK(mixer.duckAmount() == Approx(-12.0));
    CHECK(mixer.duckAttack() == Approx(0.25)); // the music mode's own
    mixer.setDuckMode(QStringLiteral("autoMix"));

    // Preview: a dry run (ghost keys on the timeline), then Apply: one undo step "Auto-Mix Voices".
    const std::size_t undo = fx.controller.session().undoCount();
    REQUIRE(mixer.previewAutoDuck().isEmpty());
    const QVariantMap pending = mixer.pending();
    REQUIRE_FALSE(pending.isEmpty());
    CHECK(pending.value(QStringLiteral("kind")).toString() == QStringLiteral("autoDuck"));
    CHECK(pending.value(QStringLiteral("title")).toString().contains(QStringLiteral("quiet microphones")));
    CHECK(pending.value(QStringLiteral("info")).toString().contains(QStringLiteral("2 lowered stretch")));
    CHECK(fx.controller.session().undoCount() == undo); // nothing applied yet
    REQUIRE(mixer.applyPending().isEmpty());
    CHECK(fx.controller.session().undoCount() == undo + 1);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Auto-Mix Voices"));
    CHECK(fx.lowest(0) == Approx(-12.0)); // the host dips while the guest speaks
    CHECK(fx.lowest(1) == Approx(-12.0)); // and the guest while the host does
    CHECK(fx.lowest(2) == Approx(0.0));   // the music is not a voice
    fx.controller.undo();
    CHECK(fx.lowest(0) == Approx(0.0));
    CHECK(fx.lowest(1) == Approx(0.0));

    // Fewer than two voices: nothing to mix, and the panel says so (the track line); a changed amount reaches the command.
    mixer.setDuckAmount(-20.0);
    REQUIRE(mixer.previewAutoDuck().isEmpty());
    REQUIRE(mixer.applyPending().isEmpty());
    CHECK(fx.lowest(0) == Approx(-20.0));
    CHECK(fx.controller.undo());
    REQUIRE(mixer.setRole(trackKey(fx.controller.currentSequence()->audioTracks[1]->id), QStringLiteral("none")).isEmpty());
    CHECK(mixer.gameTracks().value(QStringLiteral("mix")).toStringList().size() == 1);
    mixer.previewAutoDuck(); // one voice is not a mix: nothing is planned
    CHECK(mixer.pending().isEmpty());
    CHECK_FALSE(mixer.message().isEmpty());
    resetSettings();
}

TEST_CASE("The Auto-duck card's Several voices mode in the Audio workspace; screenshots", "[app][audio][duck][automix][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
            REQUIRE(controller);
            REQUIRE(mixer);
            MixFixture fx;
            controller->resetProject(fx.tl.state, fx.tl.seq);
            const SequencePtr seq = controller->currentSequence();
            fx.speech->set(seq->audioTracks[0]->id, {seconds(1.0, 3.0)});
            fx.speech->set(seq->audioTracks[1]->id, {seconds(4.0, 6.0)});
            mixer->setServices(fx.services());
            mixer->setSynchronous(true);
            shell.window->setProperty("workspace", 1);
            spin(500);
            QQuickItem* tools = shell.item("audioToolsPanel");
            REQUIRE(tools);
            REQUIRE(tools->isVisible());
            QQuickItem* tabs = namedIn(tools, QStringLiteral("duckModeTabs"));
            REQUIRE(tabs);
            REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 2)));
            CHECK(mixer->duckMode() == QStringLiteral("autoMix"));
            spin(150);
            QQuickItem* line = namedIn(tools, QStringLiteral("mixTracksLine"));
            QQuickItem* gate = namedIn(tools, QStringLiteral("duckGate"));
            QQuickItem* preview = namedIn(tools, QStringLiteral("previewDuckButton"));
            QQuickItem* includeMusic = namedIn(tools, QStringLiteral("duckIncludeMusic"));
            REQUIRE(line);
            REQUIRE(gate);
            REQUIRE(preview);
            REQUIRE(includeMusic);
            CHECK(line->isVisible());
            CHECK(line->property("text").toString().contains(QStringLiteral("each dips while the others speak")));
            CHECK(gate->isVisible());
            CHECK_FALSE(includeMusic->isVisible()); // that is the game mode's
            CHECK(tabs->property("currentIndex").toInt() == 2);
            CHECK(preview->isEnabled());
            REQUIRE(QMetaObject::invokeMethod(gate, "toggle"));
            REQUIRE(QMetaObject::invokeMethod(gate, "toggled"));
            CHECK(mixer->duckGate());
            REQUIRE(QMetaObject::invokeMethod(gate, "toggle"));
            REQUIRE(QMetaObject::invokeMethod(gate, "toggled"));
            CHECK_FALSE(mixer->duckGate());
            // Preview shows the plan with Apply; scroll the card into view for the shot.
            REQUIRE(QMetaObject::invokeMethod(preview, "clicked"));
            spin(200);
            CHECK_FALSE(mixer->pending().isEmpty());
            QQuickItem* card = namedIn(tools, QStringLiteral("duckTool"));
            REQUIRE(card);
            QQuickItem* flick = card->parentItem();
            while (flick != nullptr && !flick->inherits("QQuickFlickable"))
                flick = flick->parentItem();
            REQUIRE(flick);
            const double cardTop = card->mapToItem(flick->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y();
            const double room = flick->property("contentHeight").toDouble() - flick->height();
            flick->setProperty("contentY", std::max(0.0, std::min(cardTop - 8.0, room)));
            spin(200);
            saveShot(shell.grab(), tools, dark ? QStringLiteral("audio-duck-several-voices.png") : QStringLiteral("audio-duck-several-voices-light.png"));
            shell.window->setProperty("workspace", 0);
            spin(300);
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}
