// 0.2: the bounded ZIP reader (Zip.h) and Motion Graphics templates made in Premiere Pro (Mogrt.h): Essential
// Properties to template fields, After Effects templates refused, hostile archives refused.

#include <wabcut/interchange/Mogrt.h>
#include <wabcut/interchange/Zip.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::interchange;
using nlohmann::json;

namespace {

std::string zipOf(std::vector<std::pair<std::string, std::string>> files) { return writeStoredZip(files); }

json premiereDefinition() {
    return json{
        {"apiVersion", "1.0"},
        {"authorApp", "ppro"},
        {"capsuleID", "8BB3F1C2-LOWER-THIRD"},
        {"capsuleNameLocalized", {{"strDB", json::array({{{"localeString", "en_US"}, {"str", "Clean Lower Third"}}})}}},
        {"sourceInfoLocalized", {{"en_US", {{"width", 1280}, {"height", 720}, {"duration", 7}}}}},
        {"clientControls",
         json::array({
             {{"type", 6}, {"uiName", {{"strDB", json::array({{{"localeString", "en_US"}, {"str", "Name"}}})}}},
              {"value", {{"strDB", json::array({{{"localeString", "en_US"}, {"str", "Somchai Jaidee"}}})}}}},
             {{"type", 6}, {"uiName", "Title"}, {"value", R"({"textEditValue": "Director"})"}},
             {{"type", 2}, {"uiName", "Bar Colour"}, {"value", json::array({0.9, 0.2, 0.4, 1.0})}},
             {{"type", 4}, {"uiName", "Show Bar"}, {"value", false}},
             {{"type", 0}, {"uiName", "Size"}, {"value", 42}},
         })},
    };
}

} // namespace

TEST_CASE("The ZIP reader reads stored and deflated entries and refuses hostile archives", "[interchange][zip]") {
    const std::string zip = zipOf({{"definition.json", "{}"}, {"media/logo.png", "PNG"}});
    auto archive = ZipArchive::open(zip);
    REQUIRE(archive);
    REQUIRE(archive->entries().size() == 2);
    REQUIRE(archive->find("DEFINITION.JSON"));
    CHECK(*archive->read(*archive->find("media/logo.png")) == "PNG");

    // A deflated entry, written by hand (raw deflate as ZIP stores it).
    const std::string payload(5000, 'x');
    z_stream s{};
    REQUIRE(deflateInit2(&s, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string packed(1024, '\0');
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(payload.data()));
    s.avail_in = static_cast<uInt>(payload.size());
    s.next_out = reinterpret_cast<Bytef*>(packed.data());
    s.avail_out = static_cast<uInt>(packed.size());
    REQUIRE(deflate(&s, Z_FINISH) == Z_STREAM_END);
    packed.resize(s.total_out);
    deflateEnd(&s);
    std::string deflated = zipOf({{"big.txt", packed}});
    // Patch the method (8) and the sizes and CRC of the stored entry to describe a deflated one.
    const auto patch = [&](std::string& z, std::size_t at, std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            z[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
        }
    };
    const std::uint32_t crc = static_cast<std::uint32_t>(crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(payload.data()),
                                                               static_cast<uInt>(payload.size())));
    const std::size_t central = deflated.find("PK\x01\x02");
    REQUIRE(central != std::string::npos);
    for (const std::size_t header : {std::size_t{0}, central}) {
        const std::size_t base = header == 0 ? 0 : header + 2; // the central header has "version made by" first
        patch(deflated, base + 8, 8, 2);                          // method
        patch(deflated, base + 14, crc, 4);
        patch(deflated, base + 18, static_cast<std::uint32_t>(packed.size()), 4);
        patch(deflated, base + 22, static_cast<std::uint32_t>(payload.size()), 4);
    }
    auto inflated = ZipArchive::open(deflated);
    INFO((inflated ? std::string() : inflated.error().message));
    REQUIRE(inflated);
    CHECK(*inflated->read(inflated->entries().front()) == payload);
    // Declaring less than it inflates to (a bomb or a lie) is refused, not grown.
    std::string lying = deflated;
    for (const std::size_t header : {std::size_t{0}, central}) {
        patch(lying, (header == 0 ? 0 : header + 2) + 22, 100, 4);
    }
    auto liar = ZipArchive::open(lying);
    REQUIRE(liar);
    CHECK_FALSE(liar->read(liar->entries().front()));
    // A limit on what one archive may inflate.
    ZipLimits small;
    small.maxEntryBytes = 1000;
    CHECK_FALSE(ZipArchive::open(deflated, small));

    // Unsafe names, damaged CRCs, not a ZIP at all.
    for (const char* bad : {"../evil.txt", "/abs.txt", "C:/win.txt", "a/../../b", "a\\b.txt", "a//b"}) {
        INFO(bad);
        CHECK_FALSE(isSafeZipPath(bad));
        CHECK_FALSE(ZipArchive::open(zipOf({{bad, "x"}})));
    }
    CHECK(isSafeZipPath("folder/"));
    CHECK(isSafeZipPath("a/b/c.json"));
    std::string corrupt = zipOf({{"a.txt", "hello"}});
    corrupt[30 + 5] = 'X'; // the data, after the local header and the name
    auto damaged = ZipArchive::open(corrupt);
    REQUIRE(damaged);
    CHECK_FALSE(damaged->read(damaged->entries().front()));
    CHECK_FALSE(ZipArchive::open("not a zip at all, just text"));
    CHECK_FALSE(ZipArchive::open(zipOf({{"same.txt", "1"}, {"SAME.txt", "2"}})));
    // Truncated anywhere: refused, never read past the end.
    const std::string good = zipOf({{"definition.json", premiereDefinition().dump()}});
    for (std::size_t n = 0; n < good.size(); n += 7) {
        auto cut = ZipArchive::open(good.substr(0, n));
        if (cut) {
            for (const ZipEntry& e : cut->entries()) {
                (void)cut->read(e);
            }
        }
    }
}

TEST_CASE("A Premiere Pro Motion Graphics template imports as text and colour fields", "[interchange][mogrt]") {
    const std::string mogrt = zipOf({{"definition.json", premiereDefinition().dump()}, {"thumb.png", "PNG"},
                                     {"project.prgraphic", "binary"}, {"media/logo.png", "PNG"}});
    auto imported = importMogrt(mogrt, "Clean Lower Third.mogrt");
    INFO((imported ? std::string() : imported.error().message));
    REQUIRE(imported);
    const GraphicTemplate& t = imported->tmpl;
    CHECK(t.name == "Clean Lower Third");
    CHECK(t.id == "mogrt.8bb3f1c2-lower-third");
    CHECK(t.graphic.width == 1280);
    CHECK(t.graphic.height == 720);
    CHECK(t.duration == Ticks{7 * kTicksPerSecond});
    CHECK_FALSE(templateProblem(t));
    REQUIRE(t.fields.size() == 4);
    CHECK(t.fields[0].id == "name");
    CHECK(t.fields[0].kind == FieldKind::Text);
    CHECK(t.fields[0].value.text == "Somchai Jaidee");
    CHECK(t.fields[1].value.text == "Director"); // Premiere's text document JSON
    CHECK(t.fields[2].kind == FieldKind::Color);
    CHECK(t.fields[2].value.color.rgba[0] == 0.9);
    CHECK(t.fields[3].kind == FieldKind::Checkbox);
    CHECK_FALSE(t.fields[3].value.flag);
    CHECK(imported->report.count(ReportOutcome::Dropped) == 2); // the slider and the media
    CHECK(imported->authorApp == "ppro");
}

TEST_CASE("After Effects templates and empty ones are refused with a message", "[interchange][mogrt]") {
    json ae = premiereDefinition();
    ae["authorApp"] = "aefx";
    auto refused = importMogrt(zipOf({{"definition.json", ae.dump()}, {"project.aegraphic", "x"}}), "ae.mogrt");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::Unsupported);
    CHECK(refused.error().message.find("After Effects") != std::string::npos);
    // An After Effects project inside is enough, whatever the definition says.
    json unknown = premiereDefinition();
    unknown.erase("authorApp");
    CHECK_FALSE(importMogrt(zipOf({{"definition.json", unknown.dump()}, {"project.aegraphic", "x"}}), "x.mogrt"));
    // No definition, bad JSON, no text property.
    CHECK(importMogrt(zipOf({{"thumb.png", "x"}}), "x.mogrt").error().code == ErrorCode::ParseError);
    CHECK(importMogrt(zipOf({{"definition.json", "{nope"}}), "x.mogrt").error().code == ErrorCode::ParseError);
    json slidersOnly = premiereDefinition();
    slidersOnly["clientControls"] = json::array({{{"type", 0}, {"uiName", "Size"}, {"value", 1}}});
    CHECK(importMogrt(zipOf({{"definition.json", slidersOnly.dump()}}), "x.mogrt").error().code == ErrorCode::Unsupported);
    // Hostile values are bounded: a huge canvas falls back, long labels and texts are cut.
    json hostile = premiereDefinition();
    hostile["sourceInfoLocalized"]["en_US"]["width"] = 1e12;
    hostile["clientControls"][0]["uiName"] = std::string(5000, 'n');
    hostile["clientControls"][0]["value"] = std::string(100000, 't');
    auto bounded = importMogrt(zipOf({{"definition.json", hostile.dump()}}), "x.mogrt");
    REQUIRE(bounded);
    CHECK(bounded->tmpl.graphic.width == 1920);
    CHECK_FALSE(templateProblem(bounded->tmpl));
}

namespace {

std::string base64Of(std::string_view bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const auto v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                       static_cast<unsigned char>(bytes[i + 2]);
        out += {kAlphabet[(v >> 18) & 63], kAlphabet[(v >> 12) & 63], kAlphabet[(v >> 6) & 63], kAlphabet[v & 63]};
    }
    if (i + 1 == bytes.size()) {
        const auto v = static_cast<unsigned char>(bytes[i]) << 16;
        out += {kAlphabet[(v >> 18) & 63], kAlphabet[(v >> 12) & 63], '=', '='};
    } else if (i + 2 == bytes.size()) {
        const auto v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += {kAlphabet[(v >> 18) & 63], kAlphabet[(v >> 12) & 63], kAlphabet[(v >> 6) & 63], '='};
    }
    return out;
}

/// ASCII JSON as Premiere stores a Source Text: an 8-byte header, then UTF-16LE.
std::string sourceTextBlob(const std::string& jsonText) {
    std::string out("\x0C\x0A\0\0\0\0\0\0", 8);
    for (const char c : jsonText) {
        out.push_back(c);
        out.push_back('\0');
    }
    return out;
}

/// gzip of `text` (zlib, gzip header).
std::string gzipOf(const std::string& text) {
    z_stream s{};
    REQUIRE(deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string out(text.size() + 1024, '\0');
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
    s.avail_in = static_cast<uInt>(text.size());
    s.next_out = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&s, Z_FINISH) == Z_STREAM_END);
    out.resize(s.total_out);
    deflateEnd(&s);
    return out;
}

/// A Premiere graphic as a .mogrt holds it (project.prgraphic: a ZIP with the graphic's gzipped Premiere project),
/// with one text layer per (text, PostScript font, size, 0xRRGGBB fill, alignment code).
std::string prgraphicOf(const std::vector<std::tuple<std::string, std::string, int, int, int>>& layers) {
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<PremiereData Version=\"3\">";
    int id = 100;
    for (const auto& [text, font, size, fill, align] : layers) {
        const std::string json = "{\"mTextParam\":{\"mAlignment\":" + std::to_string(align) +
                                 ",\"mShadowVisible\":true,\"mShadowColor\":0,\"mShadowOpacity\":50,\"mShadowOffset\":4,"
                                 "\"mStyleSheet\":{\"mFontName\":{\"mParamValues\":[[0,\"" + font + "\"]]},"
                                 "\"mFontSize\":{\"mParamValues\":[[0," + std::to_string(size) + "]]},"
                                 "\"mFillColor\":{\"mParamValues\":[[0," + std::to_string(fill) + "]]},"
                                 "\"mTracking\":{\"mParamValues\":[[0,20]]},\"mText\":\"" + text + "\"}},\"mVersion\":1}";
        const int param = id++;
        xml += "<VideoFilterComponent ObjectID=\"" + std::to_string(id++) + "\"><Component><Params><Param Index=\"0\" ObjectRef=\"" +
               std::to_string(param) + "\"/></Params><DisplayName>Text</DisplayName><InstanceName>" + text +
               "</InstanceName></Component><MatchName>AE.ADBE Text</MatchName></VideoFilterComponent>";
        xml += "<ArbVideoComponentParam ObjectID=\"" + std::to_string(param) + "\"><Name>Source Text</Name>"
               "<StartKeyframeValue Encoding=\"base64\">" + base64Of(sourceTextBlob(json)) + "</StartKeyframeValue>"
               "</ArbVideoComponentParam>";
    }
    xml += "</PremiereData>";
    return zipOf({{"Template.prproj", gzipOf(xml)}});
}

} // namespace

TEST_CASE("A Premiere template's text layers lend their font, size, colour and alignment to its fields", "[interchange][mogrt]") {
    // The Name layer matches by its text; the Title layer by order.
    const std::string graphic = prgraphicOf({{"Somchai Jaidee", "BebasNeue-Regular", 75, 0x00BEE1, 1},
                                             {"Job title", "Montserrat-SemiBoldItalic", 40, 0xFFFFFF, 2}});
    json def = premiereDefinition();
    def["clientControls"] = json::array({def["clientControls"][0], def["clientControls"][1]});
    const std::string mogrt = zipOf({{"definition.json", def.dump()}, {"project.prgraphic", graphic}});
    auto imported = importMogrt(mogrt, "Clean Lower Third.mogrt");
    INFO((imported ? std::string() : imported.error().message));
    REQUIRE(imported);
    const GraphicTemplate& t = imported->tmpl;
    CHECK_FALSE(templateProblem(t));
    const GraphicLayer* name = t.graphic.findLayer(t.fields[0].layer);
    const GraphicLayer* title = t.graphic.findLayer(t.fields[1].layer);
    REQUIRE(name);
    REQUIRE(title);
    const TextStyle& a = name->text.runs.front().style;
    CHECK(a.fontFamily == "Bebas Neue");
    CHECK(a.fontWeight == 400);
    CHECK(a.fontSize == 75.0);
    CHECK(a.fill.rgba[0] == 0.0);
    CHECK(a.fill.rgba[1] == 190.0 / 255.0);
    CHECK(a.fill.rgba[2] == 225.0 / 255.0);
    CHECK(a.tracking == 20.0);
    CHECK(name->text.align == TextAlign::Right);
    CHECK(name->shadow.enabled);
    CHECK(name->shadow.opacity == 0.5);
    const TextStyle& b = title->text.runs.front().style;
    CHECK(b.fontFamily == "Montserrat");
    CHECK(b.fontWeight == 600);
    CHECK(b.italic);
    CHECK(title->text.align == TextAlign::Center);
    CHECK(imported->report.count(ReportOutcome::Carried) >= 4);

    // A graphic that does not read is no reason to refuse the template.
    const std::string broken = zipOf({{"definition.json", def.dump()}, {"project.prgraphic", zipOf({{"x.prproj", "not gzip <"}})}});
    auto plain = importMogrt(broken, "Broken.mogrt");
    REQUIRE(plain);
    CHECK(plain->tmpl.graphic.findLayer(plain->tmpl.fields[0].layer)->text.runs.front().style.fontFamily.empty());
}
