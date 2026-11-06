// Card 178 (0.7): Lottie templates with fields (text and constant colors), dotLottie archives and OGraf packages - the
// manifest read bounded (hostile input included) and a package that ships a Lottie version imported as a template.

#include "InterchangeTestSupport.h"

#include <wabcut/interchange/Lottie.h>
#include <wabcut/interchange/OGraf.h>
#include <wabcut/interchange/Zip.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::interchange;

namespace {

/// A text layer called `name` saying `text` in red.
std::string textLayer(const std::string& name, const std::string& text, int index) {
    return R"({"ty": 5, "nm": ")" + name + R"(", "ind": )" + std::to_string(index) +
           R"(, "ip": 0, "op": 60, "st": 0, "ks": {}, "t": {"d": {"k": [{"s": {"t": ")" + text +
           R"(", "s": 60, "f": "Arial", "fc": [1, 0, 0], "j": 0}, "t": 0}]}, "a": [], "p": {}}})";
}

/// A shape layer called `name` with one rectangle filled with `fill` (a Lottie property: {"a": 0, "k": [r, g, b, a]}).
std::string barLayer(const std::string& name, const std::string& fill, int index) {
    return R"({"ty": 4, "nm": ")" + name + R"(", "ind": )" + std::to_string(index) +
           R"(, "ip": 0, "op": 60, "st": 0, "ks": {}, "shapes": [{"ty": "gr", "nm": "Group", "it": [)"
           R"({"ty": "rc", "nm": "Rect", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [200, 50]}, "r": {"a": 0, "k": 0}}, )"
           R"({"ty": "fl", "c": )" + fill + R"(, "o": {"a": 0, "k": 100}}, )"
           R"({"ty": "tr", "p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100}}]}]})";
}

const std::string kStaticBlue = R"({"a": 0, "k": [0, 0.5, 1, 1]})";
const std::string kFadingRed =
    R"({"a": 1, "k": [{"t": 0, "s": [1, 0, 0, 1], "e": [0, 1, 0, 1]}, {"t": 30, "s": [0, 1, 0, 1]}]})";

std::string animation(const std::string& layers) {
    return R"({"v": "5.7.0", "fr": 30, "ip": 0, "op": 60, "w": 1000, "h": 500, "nm": "Lower Third", "layers": [)" + layers + "]}";
}

std::string titleCard() {
    return animation(textLayer("Title", "Hello", 1) + ", " + textLayer("Subtitle", "World", 2) + ", " + barLayer("Bar", kStaticBlue, 3) +
                     ", " + barLayer("Glow", kFadingRed, 4));
}

std::string zipOf(const std::vector<std::pair<std::string, std::string>>& files) { return writeStoredZip(files); }

const TemplateField* fieldOf(const std::vector<TemplateField>& fields, const std::string& id) {
    for (const TemplateField& f : fields) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

std::string manifestJson(const std::string& extra = {}, const std::string& main = "graphic.mjs") {
    return R"({"$schema": "https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json", "id": "com.example.lower-third",
 "version": "2", "name": "Lower Third", "description": "A name and a title", "author": {"name": "Jane"}, "main": ")" +
           main + R"(", "supportsRealTime": true, "supportsNonRealTime": false, "stepCount": 1,
 "schema": {"type": "object", "properties": {
   "title": {"type": "string", "title": "Headline", "default": "Hello", "order": 2},
   "subtitle": {"type": "string", "title": "Second line", "order": 3},
   "barColor": {"type": "string", "gddType": "color-rrggbb", "title": "Bar colour", "default": "#0080ff", "order": 1},
   "showGlow": {"type": "boolean", "title": "Glow", "default": true},
   "speed": {"type": "number", "default": 1}
 }}, "customActions": [{"id": "wave", "name": "Wave"}])" +
           extra + "}";
}

std::string listed(const ImportReport& report) {
    std::string out;
    for (const ReportEntry& e : report.entries()) {
        out += std::string(toString(e.outcome)) + " " + e.reason + " at " + e.location.element + "\n";
    }
    return out;
}

} // namespace

TEST_CASE("Lottie: texts and constant colors are the template's fields (card 178)", "[interchange][lottie][ograf]") {
    const auto r = importLottie(titleCard(), "card.json");
    REQUIRE(r);
    INFO(listed(r->report));
    REQUIRE_FALSE(graphicProblem(r->graphic));
    // Two texts, their two fills, the bar's blue; the fading Glow stays animation.
    REQUIRE(r->fields.size() == 5);
    CHECK(r->fields[0].kind == FieldKind::Text);
    CHECK(r->fields[0].label == "Title");
    CHECK(r->fields[0].value.text == "Hello");
    CHECK(r->fields[1].kind == FieldKind::Text);
    CHECK(r->fields[1].value.text == "World");
    int colors = 0;
    for (const TemplateField& f : r->fields) {
        if (f.kind == FieldKind::Color) {
            ++colors;
            CHECK(f.property == "fill");
        }
    }
    CHECK(colors == 3);
    const TemplateField* blue = nullptr;
    for (const TemplateField& f : r->fields) {
        if (f.kind == FieldKind::Color && f.label.find("Bar") != std::string::npos) {
            blue = &f;
        }
        CHECK(f.label.find("Glow") == std::string::npos); // keyed color: not a field
    }
    REQUIRE(blue);
    CHECK(blue->value.color.rgba[2] == Catch::Approx(1.0));
    // The fields make a valid template that instances can edit.
    GraphicTemplate tmpl;
    tmpl.id = "lottie.card";
    tmpl.name = "Card";
    tmpl.graphic = r->graphic;
    tmpl.fields = r->fields;
    CHECK_FALSE(templateProblem(tmpl));
    for (const TemplateField& f : r->fields) {
        CHECK(isValidFieldId(f.id));
        CHECK(r->graphic.findLayer(f.layer) != nullptr);
    }
}

TEST_CASE("Lottie: a field value edits the layer it names (card 178)", "[interchange][lottie][ograf]") {
    const auto r = importLottie(titleCard(), "card.json");
    REQUIRE(r);
    GraphicTemplate tmpl;
    tmpl.id = "lottie.card";
    tmpl.name = "Card";
    tmpl.graphic = r->graphic;
    tmpl.fields = r->fields;
    // withFieldValues needs the instance a clip remembers.
    Graphic g = r->graphic;
    g.instance = TemplateInstance{tmpl.id, 1, tmpl.fields};
    FieldValue text;
    text.text = "Breaking news";
    FieldValue color;
    color.color.rgba = {0.0, 1.0, 0.0, 1.0};
    const TemplateField* barColor = nullptr;
    for (const TemplateField& f : tmpl.fields) {
        if (f.kind == FieldKind::Color && f.label.find("Bar") != std::string::npos) {
            barColor = &f;
        }
    }
    REQUIRE(barColor);
    ProjectState state;
    const auto edited = edit::withFieldValues(state, g, {{tmpl.fields[0].id, text}, {barColor->id, color}});
    REQUIRE(edited);
    const GraphicLayer* title = edited->findLayer(tmpl.fields[0].layer);
    REQUIRE(title);
    CHECK(title->text.runs.front().text == "Breaking news");
    const GraphicLayer* bar = edited->findLayer(barColor->layer);
    REQUIRE(bar);
    CHECK(bar->shape.fill.color.rgba[1] == Catch::Approx(1.0));
}

TEST_CASE("dotLottie: the manifest's initial animation imports and the rest are reported (card 178)", "[interchange][lottie][ograf]") {
    const std::string a = animation(textLayer("First", "A", 1));
    const std::string b = animation(textLayer("Second", "B", 1));
    const std::string manifest = R"({"version": "2", "generator": "test", "animations": [{"id": "one"}, {"id": "two"}],
 "initial": {"animation": "two"}, "themes": [{"id": "dark"}]})";
    const std::string archive = zipOf({{"manifest.json", manifest}, {"animations/one.json", a}, {"animations/two.json", b}, {"images/x.png", "png"}});
    CHECK(looksLikeZip(archive));
    CHECK_FALSE(looksLikeZip(a));
    const auto ids = dotLottieAnimationIds(archive);
    REQUIRE(ids);
    CHECK(*ids == std::vector<std::string>{"one", "two"});

    const auto initial = importDotLottie(archive, "pack.lottie");
    REQUIRE(initial);
    REQUIRE(initial->fields.size() >= 1);
    CHECK(initial->fields.front().value.text == "B");
    const std::string report = listed(initial->report);
    CHECK(report.find("2 animations") != std::string::npos);
    CHECK(report.find("themes are not applied") != std::string::npos);

    const auto chosen = importDotLottie(archive, "pack.lottie", "one");
    REQUIRE(chosen);
    CHECK(chosen->fields.front().value.text == "A");

    const auto missing = importDotLottie(archive, "pack.lottie", "three");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().message.find("it holds: one, two") != std::string::npos);
}

TEST_CASE("dotLottie: version 1 folders and a damaged manifest and hostile archives (card 178)", "[interchange][lottie][ograf]") {
    const std::string a = animation(textLayer("First", "A", 1));
    // Version 1 files keep animations in a/, and a damaged manifest does not stop the import.
    const auto v1 = importDotLottie(zipOf({{"manifest.json", "{ not json"}, {"a/solo.json", a}}), "old.lottie");
    REQUIRE(v1);
    CHECK(listed(v1->report).find("manifest.json is damaged") != std::string::npos);
    CHECK(v1->graphic.name == "Lower Third");

    // No animation at all; not a ZIP; a manifest naming an id that could point outside is never used as a path.
    CHECK_FALSE(importDotLottie(zipOf({{"manifest.json", "{}"}}), "empty.lottie"));
    CHECK_FALSE(importDotLottie("PK nonsense", "bad.lottie"));
    CHECK_FALSE(importDotLottie(a, "json.lottie"));
    const std::string sneaky = R"({"animations": [{"id": "../../secret"}, {"id": "a/b"}]})";
    const auto none = importDotLottie(zipOf({{"manifest.json", sneaky}, {"animations/real.json", a}}), "x.lottie");
    REQUIRE(none); // the unsafe ids are ignored; the file's own animation is found by name
    CHECK(none->fields.front().value.text == "A");
    // An unsafe entry name is refused by the ZIP reader.
    CHECK_FALSE(importDotLottie(zipOf({{"../evil.json", a}}), "evil.lottie"));
    // Too big.
    CHECK_FALSE(importDotLottie(std::string(kMaxDotLottieBytes + 1, 'x'), "big.lottie"));
}

TEST_CASE("OGraf: the manifest is read with its fields in order (card 178)", "[interchange][ograf]") {
    const auto m = parseOGrafManifest(manifestJson(R"(, "v_wabcut_lottie": "motion/lower-third.json", "thumbnails": [{"file": "thumb.png"}])"));
    REQUIRE(m);
    CHECK(m->id == "com.example.lower-third");
    CHECK(m->name == "Lower Third");
    CHECK(m->author == "Jane");
    CHECK(m->main == "graphic.mjs");
    CHECK(m->lottie == "motion/lower-third.json");
    CHECK(m->supportsRealTime);
    CHECK_FALSE(m->supportsNonRealTime);
    CHECK(m->customActions == std::vector<std::string>{"wave"});
    CHECK(m->thumbnails == std::vector<std::string>{"thumb.png"});
    REQUIRE(m->properties.size() == 5);
    // "order" sorts barColor (1), title (2), subtitle (3); the rest follow in the file's key order.
    CHECK(m->properties[0].key == "barColor");
    CHECK(m->properties[0].kind == OGrafValueKind::Color);
    CHECK(m->properties[0].defaultText == "#0080ff");
    CHECK(m->properties[1].key == "title");
    CHECK(m->properties[1].kind == OGrafValueKind::Text);
    CHECK(m->properties[1].title == "Headline");
    CHECK(m->properties[2].key == "subtitle");
    bool sawBoolean = false;
    bool sawNumber = false;
    for (const OGrafProperty& p : m->properties) {
        sawBoolean = sawBoolean || (p.key == "showGlow" && p.kind == OGrafValueKind::Boolean && p.defaultBool);
        sawNumber = sawNumber || (p.key == "speed" && p.kind == OGrafValueKind::Number && p.defaultNumber == 1.0);
    }
    CHECK(sawBoolean);
    CHECK(sawNumber);
}

TEST_CASE("OGraf: a hostile manifest is refused or bounded (card 178)", "[interchange][ograf]") {
    // Paths that leave the package.
    for (const char* main : {"../evil.mjs", "/etc/passwd", "C:/Windows/x.mjs", "a/../../b.mjs", "dir/", "a\\\\b.mjs", ""}) {
        INFO(main);
        CHECK_FALSE(parseOGrafManifest(manifestJson({}, main)));
    }
    CHECK_FALSE(parseOGrafManifest(manifestJson(R"(, "v_wabcut_lottie": "../../secret.json")")));
    // Thumbnails that leave the package are dropped, not followed.
    const auto thumbs = parseOGrafManifest(manifestJson(R"(, "thumbnails": [{"file": "../x.png"}, {"file": "ok.png"}, {"file": 5}])"));
    REQUIRE(thumbs);
    CHECK(thumbs->thumbnails == std::vector<std::string>{"ok.png"});
    CHECK(listed(thumbs->report).find("thumbnail path that leaves the package") != std::string::npos);
    // Required fields and their types.
    for (const char* broken :
         {R"({})", R"([])", R"("text")", R"({"$schema": 1})", "not json", R"({"id": "a", "name": "b", "main": "c.mjs"})",
          R"({"$schema": "x", "id": "a", "name": "b", "main": "c.mjs", "supportsRealTime": "yes", "supportsNonRealTime": false})",
          R"({"$schema": "x", "id": 5, "name": "b", "main": "c.mjs", "supportsRealTime": true, "supportsNonRealTime": false})"}) {
        INFO(broken);
        CHECK_FALSE(parseOGrafManifest(broken));
    }
    // Too big, too deep.
    CHECK_FALSE(parseOGrafManifest(std::string(kMaxOGrafManifestBytes + 1, ' ')));
    std::string deep;
    for (int i = 0; i < 5000; ++i) {
        deep += "[";
    }
    CHECK_FALSE(parseOGrafManifest(deep));
    // Many fields: bounded and reported; odd shapes of fields are skipped, long text is cut.
    std::string many = R"({"$schema": "https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json", "id": "x", "name": ")" +
                       std::string(5000, 'n') + R"(", "main": "m.mjs", "supportsRealTime": true, "supportsNonRealTime": true, "schema": {"properties": {)";
    for (std::size_t i = 0; i < kMaxOGrafProperties + 40; ++i) {
        many += (i ? "," : "") + std::string("\"f") + std::to_string(i) + "\": {\"type\": \"string\"}";
    }
    many += R"(, "weird": 5, "": {"type": "string"}}}, "unknown_key": 1, "v_vendor": {"anything": [1, 2]}})";
    const auto bounded = parseOGrafManifest(many);
    REQUIRE(bounded);
    CHECK(bounded->properties.size() == kMaxOGrafProperties);
    CHECK(bounded->name.size() <= 256);
    const std::string report = listed(bounded->report);
    CHECK(report.find("more than 256 fields") != std::string::npos);
    CHECK(report.find("unknown_key") != std::string::npos);
    CHECK(report.find("v_vendor") == std::string::npos); // vendor fields are allowed
    // Numbers that are not numbers.
    const auto steps = parseOGrafManifest(manifestJson(R"(, "stepCount": 1e300)"));
    REQUIRE(steps);
    CHECK(steps->stepCount == 10000);
}

TEST_CASE("OGraf: a package with a Lottie version imports as a template with the manifest's field names (card 178)",
          "[interchange][ograf]") {
    const std::string lottie = titleCard();
    const std::string archive =
        zipOf({{"lower-third.ograf.json", manifestJson()}, {"graphic.mjs", "export default class X extends HTMLElement {}"},
               {"motion/lower-third.json", lottie}, {"thumb.png", "png"}});
    const auto package = openOGrafZip(archive);
    REQUIRE(package);
    CHECK(ografManifests(*package) == std::vector<std::string>{"lower-third.ograf.json"});
    const auto r = importOGraf(*package);
    REQUIRE(r);
    INFO(listed(r->report));
    CHECK(r->lottieFile == "motion/lower-third.json");
    CHECK(r->tmpl.id == "ograf.com.example.lower-third");
    CHECK(r->tmpl.name == "Lower Third");
    CHECK(r->tmpl.description == "A name and a title");
    CHECK_FALSE(templateProblem(r->tmpl));
    CHECK(r->duration == Ticks::fromSeconds(2));
    // The text layer "Title" is the manifest's "title", "Subtitle" its "subtitle"; the bar's color is "barColor".
    const TemplateField* title = fieldOf(r->tmpl.fields, "title");
    REQUIRE(title);
    CHECK(title->kind == FieldKind::Text);
    CHECK(title->label == "Headline");
    CHECK(title->value.text == "Hello");
    const TemplateField* subtitle = fieldOf(r->tmpl.fields, "subtitle");
    REQUIRE(subtitle);
    CHECK(subtitle->label == "Second line");
    const TemplateField* bar = fieldOf(r->tmpl.fields, "barColor");
    REQUIRE(bar);
    CHECK(bar->kind == FieldKind::Color);
    CHECK(bar->label == "Bar colour");
    // "showGlow" finds the layer named Glow and becomes a show / hide checkbox; "speed" has nothing to connect to.
    const TemplateField* glow = fieldOf(r->tmpl.fields, "showGlow");
    REQUIRE(glow);
    CHECK(glow->kind == FieldKind::Checkbox);
    CHECK(glow->property == "visible");
    const std::string report = listed(r->report);
    CHECK(report.find("is not run") != std::string::npos);
    CHECK(report.find("\"speed\"") != std::string::npos);
}

TEST_CASE("OGraf: a package without a Lottie version says it needs a web engine (card 178)", "[interchange][ograf]") {
    const auto package = openOGrafZip(zipOf({{"lower-third.ograf.json", manifestJson()}, {"graphic.mjs", "export default class {}"}}));
    REQUIRE(package);
    const auto r = importOGraf(*package);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Unsupported);
    CHECK(r.error().message.find("web engine") != std::string::npos);
    CHECK(r.error().message.find("Headline") != std::string::npos); // its fields were read and are listed
    CHECK(r.error().message.find("Lottie") != std::string::npos);
}

TEST_CASE("OGraf: choosing among manifests and Lottie files and and paths that must not be followed (card 178)",
          "[interchange][ograf]") {
    const std::string lottie = titleCard();
    // Two graphics in a package: one must be named. Two Lottie files: the manifest must name one.
    const auto twoGraphics = openOGrafZip(zipOf({{"a.ograf.json", manifestJson()}, {"b.ograf.json", manifestJson()}, {"graphic.mjs", ""}, {"one.json", lottie}}));
    REQUIRE(twoGraphics);
    const auto unnamed = importOGraf(*twoGraphics);
    REQUIRE_FALSE(unnamed);
    CHECK(unnamed.error().message.find("name one") != std::string::npos);
    CHECK(importOGraf(*twoGraphics, "b.ograf.json"));
    CHECK_FALSE(importOGraf(*twoGraphics, "c.ograf.json"));
    const auto twoLotties = openOGrafZip(zipOf({{"a.ograf.json", manifestJson()}, {"graphic.mjs", ""}, {"one.json", lottie}, {"two.lottie", zipOf({{"animations/x.json", lottie}})}}));
    REQUIRE(twoLotties);
    const auto ambiguous = importOGraf(*twoLotties);
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error().message.find("v_wabcut_lottie") != std::string::npos);
    // The manifest names the right one (a dotLottie archive works too).
    const auto named = openOGrafZip(zipOf({{"a.ograf.json", manifestJson(R"(, "v_wabcut_lottie": "two.lottie")")}, {"graphic.mjs", ""}, {"one.json", lottie},
                                           {"two.lottie", zipOf({{"animations/x.json", lottie}})}}));
    REQUIRE(named);
    const auto r = importOGraf(*named);
    REQUIRE(r);
    CHECK(r->lottieFile == "two.lottie");
    // A named file that is not in the package; JSON that is not a Lottie animation is not a candidate.
    const auto absent = openOGrafZip(zipOf({{"a.ograf.json", manifestJson(R"(, "v_wabcut_lottie": "gone.json")")}, {"graphic.mjs", ""}}));
    REQUIRE(absent);
    CHECK_FALSE(importOGraf(*absent));
    const auto notLottie = openOGrafZip(zipOf({{"a.ograf.json", manifestJson()}, {"graphic.mjs", ""}, {"data.json", R"({"hello": 1})"}}));
    REQUIRE(notLottie);
    const auto refused = importOGraf(*notLottie);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::Unsupported);
    // Nothing without a manifest; a ZIP with unsafe names or the wrong bytes never opens.
    const auto empty = openOGrafZip(zipOf({{"readme.txt", "x"}}));
    REQUIRE(empty);
    CHECK_FALSE(importOGraf(*empty));
    CHECK_FALSE(openOGrafZip("not a zip"));
    CHECK_FALSE(openOGrafZip(zipOf({{"../evil.ograf.json", manifestJson()}})));
}

TEST_CASE("OGraf: a folder is read like a package without leaving it (card 178)", "[interchange][ograf]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "ograf-folder";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "motion", ec);
    const auto write = [&](const fs::path& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary);
        out << bytes;
    };
    write(dir / "lt.ograf.json", manifestJson());
    write(dir / "graphic.mjs", "export default class {}");
    write(dir / "motion" / "lt.json", titleCard());
    const auto package = openOGrafFolder(pathToUtf8(dir));
    REQUIRE(package);
    CHECK(package->files.size() == 3);
    const auto r = importOGraf(*package);
    REQUIRE(r);
    CHECK(r->lottieFile == "motion/lt.json");
    // A path that is not in the listing cannot be read, however it is spelled.
    for (const char* sneaky : {"../x", "motion/../../x", "/etc/passwd", "lt.ograf.json/../../x"}) {
        CHECK_FALSE(package->read(sneaky));
    }
    CHECK_FALSE(openOGrafFolder(pathToUtf8(dir / "nowhere")));
    fs::remove_all(dir, ec);
}

TEST_CASE("Lottie: keyed template fields change what the graphic draws over the clip (card 178)", "[interchange][lottie][ograf]") {
    const auto r = importLottie(titleCard(), "card.json");
    REQUIRE(r);
    Graphic g = r->graphic;
    g.instance = TemplateInstance{"lottie.card", 1, r->fields};
    const auto fieldIndex = [&](FieldKind kind, const std::string& labelPart) {
        for (std::size_t i = 0; i < g.instance->fields.size(); ++i) {
            const TemplateField& f = g.instance->fields[i];
            if (f.kind == kind && f.label.find(labelPart) != std::string::npos) {
                return i;
            }
        }
        FAIL("no such field " << labelPart);
        return std::size_t{0};
    };
    // The bar's color fades from red to blue over two seconds; the title's text switches at one second.
    TemplateField& bar = g.instance->fields[fieldIndex(FieldKind::Color, "Bar")];
    FieldValue red;
    red.color.rgba = {1.0, 0.0, 0.0, 1.0};
    FieldValue blue;
    blue.color.rgba = {0.0, 0.0, 1.0, 1.0};
    bar.keys = {FieldKey{Ticks{}, red, Interpolation::Linear}, FieldKey{Ticks::fromSeconds(2), blue, Interpolation::Linear}};
    TemplateField& title = g.instance->fields[fieldIndex(FieldKind::Text, "Title")];
    FieldValue first;
    first.text = "Hello";
    FieldValue second;
    second.text = "Goodbye";
    title.keys = {FieldKey{Ticks{}, first, Interpolation::Hold}, FieldKey{Ticks::fromSeconds(1), second, Interpolation::Hold}};
    REQUIRE_FALSE(graphicProblem(g));
    CHECK(hasFieldKeys(g));

    const auto barAt = [&](double seconds) {
        const Graphic snapshot = withFieldKeysAt(g, Ticks{static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond))});
        return snapshot.findLayer(bar.layer)->shape.fill.color.rgba;
    };
    CHECK(barAt(0.0)[0] == Catch::Approx(1.0));
    CHECK(barAt(1.0)[0] == Catch::Approx(0.5));
    CHECK(barAt(1.0)[2] == Catch::Approx(0.5));
    CHECK(barAt(2.0)[2] == Catch::Approx(1.0));
    const auto titleAt = [&](double seconds) {
        return withFieldKeysAt(g, Ticks{static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond))}).findLayer(title.layer)->text.runs.front().text;
    };
    CHECK(titleAt(0.5) == "Hello");
    CHECK(titleAt(1.5) == "Goodbye");
    // The same graphic is stored and read back with its keys (a template instance of a clip).
    const std::string json = io::graphicToJson(g);
    const auto loaded = io::graphicFromJson(json);
    REQUIRE(loaded);
    CHECK(*loaded == g);
}
