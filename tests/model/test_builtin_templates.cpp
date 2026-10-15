// Built-in graphic templates (C.16, BuiltinTemplates.h): the 12 gaming templates the app ships. The files under
// src/model/templates/builtin are canonical and are what the authoring code (gaming_templates_author.cpp) makes; field
// defaults are what the graphics show; responsive time keeps intros and outros; every template resolves render-ready,
// takes Thai text in every text field, and follows the artwork rules. WABCUT_UPDATE_TEMPLATES=1 rewrites the files.

#include "TimelineFixture.h"
#include "gaming_templates_author.h"

#include <wabcut/model/BuiltinTemplates.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

const std::vector<std::string> kIds{
    "wabcut.gaming.arrowCallout", "wabcut.gaming.chatBubble", "wabcut.gaming.circleCallout", "wabcut.gaming.countdown",
    "wabcut.gaming.emotePop",     "wabcut.gaming.endCard",    "wabcut.gaming.intro",         "wabcut.gaming.killStreak",
    "wabcut.gaming.lowerThird",   "wabcut.gaming.replay",     "wabcut.gaming.subscribe",     "wabcut.gaming.webcamFrame",
};

/// "สวัสดีครับ เพื่อนๆ": 13 grapheme clusters; the fourth is "ดี" (do dek with sara ii).
const std::string kThai = "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5\xE0\xB8\x84\xE0\xB8\xA3"
                          "\xE0\xB8\xB1\xE0\xB8\x9A \xE0\xB9\x80\xE0\xB8\x9E\xE0\xB8\xB7\xE0\xB9\x88\xE0\xB8\xAD\xE0\xB8\x99"
                          "\xE0\xB9\x86";
const std::string kThaiDi = "\xE0\xB8\x94\xE0\xB8\xB5";

constexpr std::string_view kPicture = R"(
    V2     |..........|
    V1 tgt |Aaaaaaaaaa|
)";

fs::path builtinFolder() { return fs::path(WABCUT_TEST_DATA_DIR) / ".." / ".." / "src" / "model" / "templates" / "builtin"; }

bool updating() {
    const char* value = std::getenv("WABCUT_UPDATE_TEMPLATES");
    return value && std::string_view(value) == "1";
}

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// The template files in the folder, sorted by name.
std::vector<fs::path> templateFiles() {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(builtinFolder(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().ends_with(io::kTemplateFileSuffix)) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

Ticks ms(std::int64_t m) { return Ticks{m * (kTicksPerSecond / 1000)}; }

const ResponsiveTime& responsiveOf(const GraphicTemplate& t) {
    REQUIRE(t.graphic.responsive);
    return *t.graphic.responsive;
}

// ---- Layers and curves ------------------------------------------------------------------------------------------------

void collectLayers(const std::vector<GraphicLayer>& layers, std::vector<const GraphicLayer*>& out) {
    for (const GraphicLayer& l : layers) {
        out.push_back(&l);
        collectLayers(l.children, out);
    }
}

std::vector<const GraphicLayer*> allLayers(const Graphic& g) {
    std::vector<const GraphicLayer*> out;
    collectLayers(g.layers, out);
    return out;
}

/// Every keyframed value of a layer (not its children).
std::vector<const Curve*> curvesOf(const GraphicLayer& l) {
    std::vector<const Curve*> out;
    const auto add = [&](const ParamValue& p) {
        if (const Curve* c = p.curve()) {
            out.push_back(c);
        }
    };
    const LayerTransform& t = l.transform;
    for (const ParamValue* p : {&t.x, &t.y, &t.anchorX, &t.anchorY, &t.scaleX, &t.scaleY, &t.rotation, &t.opacity}) {
        add(*p);
    }
    const ShapeContent& s = l.shape;
    for (const ParamValue* p : {&s.width, &s.height, &s.cornerRadius, &s.outerRadius, &s.innerRadius, &s.trimStart, &s.trimEnd,
                                &s.trimOffset, &s.stroke.width}) {
        add(*p);
    }
    for (const TextAnimator& a : l.text.animators) {
        add(a.start);
        add(a.end);
        add(a.offset);
    }
    for (const Mask& m : l.masks) {
        for (const MaskParam& p : m.params) {
            add(p.value);
        }
    }
    return out;
}

std::vector<const Curve*> allCurves(const Graphic& g) {
    std::vector<const Curve*> out;
    for (const GraphicLayer* l : allLayers(g)) {
        for (const Curve* c : curvesOf(*l)) {
            out.push_back(c);
        }
    }
    return out;
}

/// A curve with at least two keys inside [from, to]: an animation baked there.
bool animatesWithin(const std::vector<const Curve*>& curves, Ticks from, Ticks to) {
    return std::any_of(curves.begin(), curves.end(), [&](const Curve* c) {
        return std::count_if(c->keys.begin(), c->keys.end(),
                             [&](const Keyframe& k) { return k.time.value >= from.value && k.time.value <= to.value; }) >= 2;
    });
}

/// The ancestors of layer `id` and the layer, outermost first (empty when it is not there).
bool pathTo(const std::vector<GraphicLayer>& layers, std::uint32_t id, std::vector<const GraphicLayer*>& path) {
    for (const GraphicLayer& l : layers) {
        path.push_back(&l);
        if (l.id == id || pathTo(l.children, id, path)) {
            return true;
        }
        path.pop_back();
    }
    return false;
}

/// When layer `id` shows (its in and out points and its groups'): [from, to).
std::pair<Ticks, Ticks> shownWindow(const GraphicTemplate& t, std::uint32_t id) {
    std::vector<const GraphicLayer*> path;
    REQUIRE(pathTo(t.graphic.layers, id, path));
    Ticks from{};
    Ticks to = responsiveOf(t).duration;
    for (const GraphicLayer* l : path) {
        from = std::max(from, l->in);
        if (l->out) {
            to = std::min(to, *l->out);
        }
    }
    return {from, to};
}

/// A design time at which layer `id` shows: the poster time when it does then, else the middle of its window.
Ticks timeShowing(const GraphicTemplate& t, std::uint32_t id) {
    const Ticks poster = templatePosterTime(t);
    const auto [from, to] = shownWindow(t, id);
    if (from <= poster && poster < to) {
        return poster;
    }
    return Ticks{(from.value + to.value) / 2};
}

// ---- Draw lists -------------------------------------------------------------------------------------------------------

bool finite(const Affine2D& m) {
    return std::isfinite(m.a) && std::isfinite(m.b) && std::isfinite(m.c) && std::isfinite(m.d) && std::isfinite(m.e) &&
           std::isfinite(m.f);
}

/// The item's content box on the canvas.
Box canvasBox(const DrawItem& i) {
    const Box& b = i.bounds;
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    bool first = true;
    for (const auto& [px, py] : {std::pair{b.x, b.y}, std::pair{b.x + b.width, b.y}, std::pair{b.x, b.y + b.height},
                                 std::pair{b.x + b.width, b.y + b.height}}) {
        const auto p = i.transform.map(px, py);
        x0 = first ? p[0] : std::min(x0, p[0]);
        y0 = first ? p[1] : std::min(y0, p[1]);
        x1 = first ? p[0] : std::max(x1, p[0]);
        y1 = first ? p[1] : std::max(y1, p[1]);
        first = false;
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

bool onCanvas(const DrawItem& i, const DrawList& list) {
    const Box b = canvasBox(i);
    return b.x < list.width && b.x + b.width > 0.0 && b.y < list.height && b.y + b.height > 0.0;
}

std::string textOf(const DrawItem& i) {
    std::string out;
    for (const DrawTextRun& r : i.text.runs) {
        out += r.text;
    }
    return out;
}

/// Whether the item puts anything on the canvas: not transparent, not scaled to nothing, not masked to nothing (a wipe's
/// mask before it starts or after it ends), not trimmed away, with text, a picture, or paint, and on the canvas.
bool drawsSomething(const DrawItem& i, const DrawList& list) {
    if (!(i.opacity > 1e-6)) {
        return false;
    }
    if (std::abs(i.transform.a * i.transform.d - i.transform.b * i.transform.c) < 1e-9) {
        return false;
    }
    for (const DrawMask& m : i.masks) {
        if (m.mode == MaskMode::Add && !m.inverted && m.outline.empty()) {
            return false;
        }
    }
    switch (i.kind) {
    case DrawKind::Shape:
        if (i.trimEnd <= i.trimStart + 1e-9 || (i.fill.kind == PaintKind::None && !i.stroke.enabled)) {
            return false;
        }
        break;
    case DrawKind::Text: {
        const std::string text = textOf(i);
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
            return false;
        }
        // With a text animator, some cluster that is not a space must show (word animators leave spaces alone).
        const auto shows = [&](const DrawCluster& c) {
            const std::string_view cluster = std::string_view(text).substr(c.byteStart, c.byteEnd - c.byteStart);
            return c.opacity > 1e-6 && cluster.find_first_not_of(" \t") != std::string_view::npos;
        };
        if (!i.text.clusters.empty() && std::none_of(i.text.clusters.begin(), i.text.clusters.end(), shows)) {
            return false;
        }
        break;
    }
    case DrawKind::Image:
        if (!i.image.media.isValid()) {
            return false; // an empty slot (a media that is not in the project draws nothing)
        }
        break;
    }
    return onCanvas(i, list);
}

bool nothingDrawn(const DrawList& list) {
    return std::none_of(list.items.begin(), list.items.end(), [&](const DrawItem& i) { return drawsSomething(i, list); });
}

const DrawItem* itemOf(const DrawList& list, std::uint32_t layer) {
    const auto it = std::find_if(list.items.begin(), list.items.end(), [&](const DrawItem& i) { return i.layer == layer; });
    return it == list.items.end() ? nullptr : &*it;
}

// ---- Instances --------------------------------------------------------------------------------------------------------

struct Instance {
    ProjectState state;
    SequenceId seq;
    ClipId clip;

    const Clip& clipNow() const { return state.findSequence(seq)->findClip(clip)->clip(); }
    const Graphic& graphic() const {
        const Graphic* g = clipNow().graphic();
        REQUIRE(g);
        return *g;
    }
};

/// `tmpl` on V2 of a 1920 x 1080 timeline at 2 s, `length` long.
Instance place(const GraphicTemplate& tmpl, Ticks length) {
    Timeline tl(kPicture);
    const auto placed = ok(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), Ticks::fromSeconds(2), length, tmpl));
    REQUIRE(placed.placed.size() == 1);
    return {placed.state, tl.seq, placed.placed[0]};
}

/// A still picture of the project (image fields need media with a video stream).
std::pair<ProjectState, MediaId> withStill(const ProjectState& state, std::string name) {
    MediaItem still;
    still.name = std::move(name);
    still.info.isStillImage = true;
    VideoStreamInfo video;
    video.index = 0;
    video.codec = "png";
    video.width = 512;
    video.height = 512;
    still.info.video.push_back(video);
    auto added = edit::addMedia(state, still);
    REQUIRE(added);
    return {added->state, added->id};
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return s;
}

} // namespace

// ---- The library ------------------------------------------------------------------------------------------------------

TEST_CASE("The authored gaming templates are the shipped built-in files", "[model][templates][gaming]") {
    std::vector<GraphicTemplate> authored;
    REQUIRE_NOTHROW(authored = authorGamingTemplates());
    REQUIRE(authored.size() == kIds.size());
    if (updating()) {
        // Regenerate the files: write every template, remove files of templates no longer authored.
        fs::create_directories(builtinFolder());
        std::set<std::string> names;
        for (const GraphicTemplate& t : authored) {
            const std::string name = t.id + std::string(io::kTemplateFileSuffix);
            names.insert(name);
            std::ofstream out(builtinFolder() / name, std::ios::binary | std::ios::trunc);
            out << io::templateFileToJson(t);
            REQUIRE(out.good());
        }
        for (const fs::path& file : templateFiles()) {
            if (!names.contains(file.filename().string())) {
                fs::remove(file);
            }
        }
        for (std::size_t i = 0; i < authored.size(); ++i) {
            const auto reread = io::templateFileFromJson(readFile(builtinFolder() / (authored[i].id + std::string(io::kTemplateFileSuffix))));
            REQUIRE(reread);
            CHECK(*reread == authored[i]);
        }
        WARN("WABCUT_UPDATE_TEMPLATES=1: rewrote " << authored.size() << " files in " << builtinFolder().string()
                                                    << "; rebuild to embed them");
        return;
    }
    const std::vector<GraphicTemplate>& shipped = builtinTemplates();
    INFO("The built-in files differ from tests/model/gaming_templates_author.cpp: run the model tests with "
         "WABCUT_UPDATE_TEMPLATES=1, then rebuild");
    REQUIRE(shipped.size() == authored.size());
    for (std::size_t i = 0; i < authored.size(); ++i) {
        INFO(authored[i].id);
        CHECK(shipped[i] == authored[i]);
    }
}

TEST_CASE("The built-ins are the 12 gaming templates sorted by id", "[model][templates][gaming]") {
    const std::vector<GraphicTemplate>& all = builtinTemplates();
    REQUIRE(all.size() == kIds.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        const GraphicTemplate& t = all[i];
        INFO(t.id);
        CHECK(t.id == kIds[i]);
        CHECK_FALSE(templateProblem(t));
        CHECK(templateCategory(t) == "gaming");
        CHECK(findBuiltinTemplate(t.id) == &t);
        CHECK(t.graphic.width == 1920);
        CHECK(t.graphic.height == 1080);
        CHECK(t.version == 1);
        // The template browser shows the description on one line.
        CHECK_FALSE(t.description.empty());
        CHECK(t.description.size() <= 100);
        CHECK(t.description.find('\n') == std::string::npos);
    }
    CHECK(findBuiltinTemplate(kReplayTemplateId) != nullptr);
    CHECK(findBuiltinTemplate("wabcut.gaming.nope") == nullptr);
    CHECK(findBuiltinTemplate("") == nullptr);
    // Built-ins are the same objects on every call.
    CHECK(&builtinTemplates() == &all);
}

TEST_CASE("Built-in template files are canonical template files", "[model][templates][gaming]") {
    const std::vector<fs::path> files = templateFiles();
    REQUIRE(files.size() == kIds.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        INFO(files[i].string());
        CHECK(files[i].filename().string() == kIds[i] + std::string(io::kTemplateFileSuffix));
        std::string text = readFile(files[i]);
        std::erase(text, '\r'); // a checkout may have CRLF line ends
        const auto parsed = io::templateFileFromJson(text);
        REQUIRE(parsed);
        CHECK(parsed->id == kIds[i]);
        // Exactly what the writer writes (so diffs of regenerated files show real changes only).
        CHECK(io::templateFileToJson(*parsed) == text);
        if (!updating()) {
            REQUIRE(builtinTemplates().size() == files.size());
            CHECK(*parsed == builtinTemplates()[i]); // what the build embedded
        }
    }
}

TEST_CASE("Built-in field ids are a stable contract", "[model][templates][gaming]") {
    // Instances, commands (clip.instantReplay sets the replay's "label") and the UI refer to fields by id, and an instance
    // keeps working after a template is tuned only while its ids stay. Renaming one must be a deliberate change here.
    const std::vector<std::pair<std::string, std::vector<std::string>>> expected{
        {"wabcut.gaming.arrowCallout", {"label", "accent", "length"}},
        {"wabcut.gaming.chatBubble", {"username", "message", "accent", "avatarColor", "showAvatar", "length"}},
        {"wabcut.gaming.circleCallout", {"label", "accent", "length"}},
        {"wabcut.gaming.countdown", {"count3", "count2", "count1", "go", "accent"}},
        {"wabcut.gaming.emotePop", {"emote", "caption", "accent", "showCaption"}},
        {"wabcut.gaming.endCard", {"title", "handle", "accent", "video1Thumb", "video2Thumb", "showChannel", "length"}},
        {"wabcut.gaming.intro", {"title", "subtitle", "accent", "length"}},
        {"wabcut.gaming.killStreak", {"title", "subtitle", "accent"}},
        {"wabcut.gaming.lowerThird", {"name", "handle", "accent", "length"}},
        {"wabcut.gaming.replay", {"label", "accent", "stingerColor"}},
        {"wabcut.gaming.subscribe", {"label", "accent", "showLike", "showBell", "showCursor"}},
        {"wabcut.gaming.webcamFrame", {"name", "accent", "tagColor", "length"}},
    };
    const std::vector<GraphicTemplate>& all = builtinTemplates();
    REQUIRE(all.size() == expected.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        INFO(all[i].id);
        CHECK(all[i].id == expected[i].first);
        std::vector<std::string> ids;
        for (const TemplateField& f : all[i].fields) {
            ids.push_back(f.id);
        }
        CHECK(ids == expected[i].second);
    }
    // The kinds the commands rely on.
    const GraphicTemplate* replay = findBuiltinTemplate(kReplayTemplateId);
    REQUIRE(replay);
    for (const TemplateField& f : replay->fields) {
        if (f.id == "label") {
            CHECK(f.kind == FieldKind::Text);
            CHECK(f.value.text == "REPLAY");
        }
    }
    for (const GraphicTemplate& t : all) {
        for (const TemplateField& f : t.fields) {
            INFO(t.id << " " << f.id);
            if (f.id == "accent" || f.id.ends_with("Color")) {
                CHECK(f.kind == FieldKind::Color);
            } else if (f.id.starts_with("show")) {
                CHECK(f.kind == FieldKind::Checkbox);
            } else if (f.id == "length") {
                CHECK((f.kind == FieldKind::Duration && f.property == "clip"));
            } else if (f.id == "emote" || f.id.ends_with("Thumb")) {
                CHECK(f.kind == FieldKind::Image);
            } else {
                CHECK(f.kind == FieldKind::Text);
            }
        }
    }
}

TEST_CASE("Built-in fields show the graphic's values and the time is responsive", "[model][templates][gaming]") {
    for (const GraphicTemplate& t : builtinTemplates()) {
        INFO(t.id);
        const ResponsiveTime& r = responsiveOf(t);
        CHECK(t.duration == r.duration);
        CHECK(r.intro.value > 0);
        CHECK(r.outro.value > 0);
        CHECK(r.intro.value + r.outro.value <= r.duration.value);
        CHECK(templatePosterTime(t) == r.intro);

        REQUIRE_FALSE(t.fields.empty());
        std::size_t textFields = 0;
        for (const TemplateField& f : t.fields) {
            INFO(f.id);
            CHECK(f.value == edit::currentFieldValue(t.graphic, f, t.duration));
            CHECK_FALSE(f.label.empty());
            textFields += f.kind == FieldKind::Text ? 1 : 0;
        }
        CHECK(textFields >= 1);
        CHECK(std::count_if(t.fields.begin(), t.fields.end(), [](const TemplateField& f) {
                  return f.kind == FieldKind::Color && f.id == "accent";
              }) == 1);

        // Baked animations in the intro and in the outro.
        const std::vector<const Curve*> curves = allCurves(t.graphic);
        const Ticks middleEnd{r.duration.value - r.outro.value};
        CHECK(animatesWithin(curves, Ticks{}, r.intro));
        CHECK(animatesWithin(curves, middleEnd, r.duration));
        // The stretchable middle only holds or loops: a curve keyed there comes back to where it was.
        for (const Curve* c : curves) {
            const bool keyedInMiddle = std::any_of(c->keys.begin(), c->keys.end(), [&](const Keyframe& k) {
                return k.time.value > r.intro.value && k.time.value < middleEnd.value;
            });
            if (keyedInMiddle) {
                CHECK(c->valueAt(r.intro) == Approx(c->valueAt(middleEnd)));
            }
        }
        // Layers come and go only in the intro or the outro.
        for (const GraphicLayer* l : allLayers(t.graphic)) {
            INFO(l->name);
            for (const std::optional<Ticks>& point : {std::optional<Ticks>(l->in), l->out}) {
                if (point) {
                    CHECK((point->value <= r.intro.value || point->value >= middleEnd.value));
                }
            }
        }
    }
}

TEST_CASE("Built-ins resolve render-ready at the start the poster the middle and the end", "[model][templates][gaming]") {
    for (const GraphicTemplate& t : builtinTemplates()) {
        INFO(t.id);
        const ResponsiveTime& r = responsiveOf(t);
        const Ticks poster = templatePosterTime(t);
        for (const Ticks time : {Ticks{}, poster, Ticks{r.duration.value / 2}, Ticks{r.duration.value - ms(1).value}, r.duration}) {
            INFO("design time " << time.value);
            DrawList list;
            REQUIRE_NOTHROW(list = resolveGraphic(t.graphic, time, ResolveOptions{}));
            CHECK(list.width == 1920);
            CHECK(list.height == 1080);
            for (const DrawItem& i : list.items) {
                INFO("layer " << i.layer);
                CHECK(i.opacity >= 0.0);
                CHECK(i.opacity <= 1.0);
                CHECK(finite(i.transform));
            }
        }
        // The thumbnail: everything is in and at rest, on the canvas.
        const DrawList list = resolveGraphic(t.graphic, poster, ResolveOptions{});
        REQUIRE_FALSE(list.items.empty());
        for (const DrawItem& i : list.items) {
            INFO("layer " << i.layer);
            if (i.kind != DrawKind::Image) {
                CHECK(onCanvas(i, list));
                CHECK(drawsSomething(i, list));
            }
        }
        // It starts from nothing and ends in nothing.
        CHECK(nothingDrawn(resolveGraphic(t.graphic, Ticks{})));
        CHECK(nothingDrawn(resolveGraphic(t.graphic, r.duration)));
    }
}

TEST_CASE("Built-ins keep their intro and outro on short and long clips", "[model][templates][gaming]") {
    for (const GraphicTemplate& t : builtinTemplates()) {
        INFO(t.id);
        const ResponsiveTime& r = responsiveOf(t);
        const Ticks middleEnd{r.duration.value - r.outro.value};
        for (const Ticks length : {ms(500), Ticks::fromSeconds(60)}) {
            INFO("clip length " << length.value);
            const Instance inst = place(t, length);
            const Clip& clip = inst.clipNow();
            const Graphic& g = inst.graphic();
            REQUIRE(clip.duration() == length);
            const Ticks start = clip.start();
            const Ticks end{start.value + length.value};
            // The start shows the intro's start: nothing yet.
            const DrawList first = resolveGraphicClip(inst.state, clip, start);
            CHECK(first == resolveGraphic(g, Ticks{}));
            CHECK(nothingDrawn(first));
            // The last frame is in the outro, and the end is the outro's end: nothing left.
            const Ticks last{end.value - f(1).value};
            const Ticks design = graphicTimeAt(clip, g, last);
            CHECK(design.value >= middleEnd.value);
            CHECK(design.value <= r.duration.value);
            CHECK(resolveGraphicClip(inst.state, clip, last) == resolveGraphic(g, design));
            CHECK(graphicTimeAt(clip, g, end) == r.duration);
            CHECK(nothingDrawn(resolveGraphicClip(inst.state, clip, end)));
            if (length > r.duration) {
                // A long clip plays the intro as designed and stretches the middle.
                CHECK(graphicTimeAt(clip, g, Ticks{start.value + r.intro.value}) == r.intro);
                const Ticks middle = graphicTimeAt(clip, g, Ticks{start.value + length.value / 2});
                CHECK(middle.value >= r.intro.value);
                CHECK(middle.value <= middleEnd.value);
                CHECK_FALSE(resolveGraphicClip(inst.state, clip, Ticks{start.value + length.value / 2}).items.empty());
            }
        }
    }
}

TEST_CASE("Every built-in text field takes Thai text", "[model][templates][gaming][text]") {
    // The marks of "ดี" stay with their consonant.
    const auto clusters = graphemeClusters(kThai);
    REQUIRE(clusters.size() == 13);
    CHECK(kThai.substr(clusters[3].first, clusters[3].second - clusters[3].first) == kThaiDi);

    for (const GraphicTemplate& t : builtinTemplates()) {
        INFO(t.id);
        const Instance inst = place(t, Ticks::fromSeconds(60));
        std::vector<std::pair<std::string, FieldValue>> values;
        for (const TemplateField& f : t.fields) {
            if (f.kind == FieldKind::Text) {
                FieldValue v;
                v.text = kThai;
                values.emplace_back(f.id, v);
            }
        }
        REQUIRE_FALSE(values.empty());
        const auto set = ok(edit::setTemplateFields(inst.state, inst.seq, inst.clip, values));
        const Clip& clip = set.state.findSequence(inst.seq)->findClip(inst.clip)->clip();
        REQUIRE(clip.graphic());
        CHECK_FALSE(graphicProblem(*clip.graphic()));
        for (const TemplateField& f : t.fields) {
            if (f.kind != FieldKind::Text) {
                continue;
            }
            INFO(f.id);
            // Within the intro (or at its end) clip time and design time agree on a long clip.
            const Ticks time = timeShowing(t, f.layer);
            REQUIRE(time.value <= responsiveOf(t).intro.value);
            const DrawList list = resolveGraphicClip(set.state, clip, Ticks{clip.start().value + time.value});
            const DrawItem* item = itemOf(list, f.layer);
            REQUIRE(item);
            REQUIRE(item->kind == DrawKind::Text);
            CHECK(textOf(*item) == kThai);
            CHECK(drawsSomething(*item, list));
            for (const DrawTextRun& run : item->text.runs) {
                CHECK((run.fontFamily == "Bai Jamjuree" || run.fontFamily == "IBM Plex Sans Thai"));
            }
            if (!item->text.clusters.empty()) {
                // A text animator steps over grapheme clusters: Thai marks stay on their consonant.
                REQUIRE(item->text.clusters.size() == 13);
                CHECK(item->text.clusters[3].byteStart == clusters[3].first);
                CHECK(item->text.clusters[3].byteEnd == clusters[3].second);
            }
        }
    }
}

TEST_CASE("The emote and end card image slots are empty until a picture is set", "[model][templates][gaming]") {
    const auto imageFields = [](const GraphicTemplate& t) {
        std::vector<std::string> ids;
        for (const TemplateField& f : t.fields) {
            if (f.kind == FieldKind::Image) {
                ids.push_back(f.id);
            }
        }
        return ids;
    };
    for (const GraphicTemplate& t : builtinTemplates()) {
        INFO(t.id);
        if (t.id == "wabcut.gaming.emotePop") {
            CHECK(imageFields(t) == std::vector<std::string>{"emote"});
        } else if (t.id == "wabcut.gaming.endCard") {
            CHECK(imageFields(t) == std::vector<std::string>{"video1Thumb", "video2Thumb"});
        } else {
            CHECK(imageFields(t).empty());
        }
    }

    for (const std::string_view id : {"wabcut.gaming.emotePop", "wabcut.gaming.endCard"}) {
        INFO(id);
        const GraphicTemplate* t = findBuiltinTemplate(id);
        REQUIRE(t);
        const Instance inst = place(*t, Ticks::fromSeconds(30));
        const Ticks poster{inst.clipNow().start().value + templatePosterTime(*t).value};
        // Empty slots draw no picture.
        const DrawList empty = resolveGraphicClip(inst.state, inst.clipNow(), poster);
        CHECK(std::none_of(empty.items.begin(), empty.items.end(),
                           [](const DrawItem& i) { return i.kind == DrawKind::Image && i.image.media.isValid(); }));
        // With pictures of the project, each slot shows its own.
        ProjectState state = inst.state;
        std::vector<std::pair<std::string, FieldValue>> values;
        std::vector<MediaId> pictures;
        for (const TemplateField& f : t->fields) {
            if (f.kind == FieldKind::Image) {
                auto [withPicture, media] = withStill(state, f.id + ".png");
                state = std::move(withPicture);
                FieldValue v;
                v.media = media;
                values.emplace_back(f.id, v);
                pictures.push_back(media);
            }
        }
        const auto set = edit::setTemplateFields(state, inst.seq, inst.clip, values);
        REQUIRE(set);
        const Clip& clip = set->state.findSequence(inst.seq)->findClip(inst.clip)->clip();
        const DrawList filled = resolveGraphicClip(set->state, clip, poster);
        for (std::size_t i = 0; i < pictures.size(); ++i) {
            const TemplateField* field = nullptr;
            for (const TemplateField& f : t->fields) {
                if (f.id == values[i].first) {
                    field = &f;
                }
            }
            REQUIRE(field);
            const DrawItem* item = itemOf(filled, field->layer);
            REQUIRE(item);
            CHECK(item->kind == DrawKind::Image);
            CHECK(item->image.media == pictures[i]);
            CHECK(drawsSomething(*item, filled));
        }
    }
}

TEST_CASE("The replay stinger keeps its blinking corner tag over a long replay", "[model][templates][gaming]") {
    const GraphicTemplate* t = findBuiltinTemplate(kReplayTemplateId);
    REQUIRE(t);
    const ResponsiveTime& r = responsiveOf(*t);
    std::uint32_t labelLayer = 0;
    for (const TemplateField& f : t->fields) {
        if (f.id == "label") {
            labelLayer = f.layer;
            CHECK(f.value.text == "REPLAY");
        }
    }
    REQUIRE(labelLayer != 0);
    bool accent = false;
    for (const TemplateField& f : t->fields) {
        accent = accent || (f.id == "accent" && f.kind == FieldKind::Color);
    }
    CHECK(accent);

    // A 20 s slow-motion replay: 10 s in, the tag is there.
    const Instance inst = place(*t, Ticks::fromSeconds(20));
    const Clip& clip = inst.clipNow();
    const DrawList middle = resolveGraphicClip(inst.state, clip, Ticks{clip.start().value + Ticks::fromSeconds(10).value});
    const DrawItem* tag = itemOf(middle, labelLayer);
    REQUIRE(tag);
    CHECK(textOf(*tag) == "REPLAY");
    CHECK(drawsSomething(*tag, middle));
    const auto named = [&](std::string_view name) -> const GraphicLayer* {
        for (const GraphicLayer* l : allLayers(t->graphic)) {
            if (l->name == name) {
                return l;
            }
        }
        return nullptr;
    };
    // The stinger (its flash and band) is long gone by then; the tag is at rest in the top-left corner.
    const GraphicLayer* flash = named("Flash");
    const GraphicLayer* band = named("Band");
    REQUIRE(flash);
    REQUIRE(band);
    CHECK_FALSE(itemOf(middle, flash->id));
    CHECK_FALSE(itemOf(middle, band->id));
    const Box corner = canvasBox(*tag);
    CHECK(corner.x < 400.0);
    CHECK(corner.y < 200.0);
    // During the stinger the same text is big in the middle of the frame.
    const DrawList stinger = resolveGraphicClip(inst.state, clip, Ticks{clip.start().value + ms(300).value});
    const DrawItem* big = itemOf(stinger, labelLayer);
    REQUIRE(big);
    CHECK(textOf(*big) == "REPLAY");
    CHECK(canvasBox(*big).width > 2.0 * corner.width);
    CHECK(itemOf(stinger, band->id));

    // The dot blinks: a keyed fade loop over the stretchable middle.
    const GraphicLayer* dot = named("Dot");
    REQUIRE(dot);
    const Curve* blink = dot->transform.opacity.curve();
    REQUIRE(blink);
    CHECK(blink->keys.size() >= 6);
    std::set<double> values;
    for (const Keyframe& k : blink->keys) {
        CHECK(k.time.value >= r.intro.value);
        CHECK(k.time.value <= r.duration.value - r.outro.value);
        values.insert(k.value);
    }
    CHECK(values.size() >= 2);
    // And it blinks in the middle of the 20 s replay too.
    std::set<double> seen;
    for (int s = 2; s <= 18; ++s) {
        const DrawList at = resolveGraphicClip(inst.state, clip, Ticks{clip.start().value + ms(s * 1000 - 250).value});
        if (const DrawItem* d = itemOf(at, dot->id)) {
            seen.insert(std::round(d->opacity * 20.0));
        }
    }
    CHECK(seen.size() >= 2);
}

TEST_CASE("Built-in artwork is original shapes and text", "[model][templates][gaming]") {
    for (const GraphicTemplate& t : builtinTemplates()) {
        INFO(t.id);
        for (const GraphicLayer* l : allLayers(t.graphic)) {
            INFO(l->name);
            CHECK(l->image.brandLogo.empty());
            CHECK_FALSE(l->image.media.isValid());
            for (const TextRun& run : l->text.runs) {
                CHECK(run.style.brandFont.empty());
                CHECK((run.style.fontFamily == "Bai Jamjuree" || run.style.fontFamily == "IBM Plex Sans Thai"));
                CHECK(run.style.fontWeight >= (run.style.fontFamily == "Bai Jamjuree" ? 500 : 400));
                CHECK(run.style.fontWeight <= 600);
            }
            CHECK(l->text.language.empty()); // the default texts are English
        }
        std::vector<std::string> texts{t.name, t.description};
        for (const TemplateField& f : t.fields) {
            CHECK_FALSE(f.value.media.isValid());
            texts.push_back(f.value.text);
            texts.push_back(f.label);
        }
        for (const GraphicLayer* l : allLayers(t.graphic)) {
            texts.push_back(l->text.text());
            texts.push_back(l->name);
        }
        for (const std::string& text : texts) {
            INFO(text);
            const std::string low = lowered(text);
            for (const std::string_view platform : {"youtube", "twitch", "tiktok"}) {
                CHECK(low.find(platform) == std::string::npos);
            }
        }
    }
}

TEST_CASE("Poster times and categories of templates", "[model][templates]") {
    GraphicTemplate t;
    t.id = "user.title";
    t.name = "Title";
    CHECK(templateCategory(t).empty());
    CHECK(templatePosterTime(t) == Ticks{Ticks::fromSeconds(5).value / 2}); // half the default 5 s
    t.duration = Ticks::fromSeconds(4);
    CHECK(templatePosterTime(t) == Ticks::fromSeconds(2));
    t.graphic.responsive = ResponsiveTime{Ticks::fromSeconds(6), Ticks::fromSeconds(1), Ticks::fromSeconds(1)};
    CHECK(templatePosterTime(t) == Ticks::fromSeconds(1)); // the end of the intro
    t.graphic.responsive->intro = Ticks{};
    CHECK(templatePosterTime(t) == Ticks::fromSeconds(2)); // no intro: half its length
    t.duration = Ticks{};
    CHECK(templatePosterTime(t) == Ticks::fromSeconds(3));
    t.id = "wabcut.gaming.custom";
    CHECK(templateCategory(t) == "gaming");
    t.id = "wabcut.gaming.";
    CHECK(templateCategory(t).empty());
}
