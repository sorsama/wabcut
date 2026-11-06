// Built-in caption workshop commands (C.7, model: CaptionOps.h): caption tracks and cues, styles and presets, and the
// speaker records of the project's transcripts. Generating captions from a transcript and correcting transcripts need
// the transcripts themselves: those commands are in wabcut_ai_edit (TranscriptCommands.h).

#include "Catalog.h"

#include <wabcut/command/SpellingServices.h>
#include <wabcut/interchange/Docx.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Plural.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Spelling.h>
#include <wabcut/model/TextFind.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <tuple>

namespace wabcut::command {

const LineBreaker& lineBreakerOf(ExecutionContext& context) {
    if (const auto* services = findService<CaptionServices>(context.services())) {
        return services->lineBreaker();
    }
    return simpleLineBreaker();
}

} // namespace wabcut::command

namespace wabcut::command::detail {

namespace {

// --- References and values ---------------------------------------------------------------------------------

Json captionTrackParam() {
    return schema::integer("Caption track id (sequence.summary lists them). Default: the sequence's first.", 1);
}

Json cueParam(std::string description) {
    return schema::integer(std::move(description) + " Cue index in start order (captions.get lists them).", 0,
                           static_cast<std::int64_t>(kMaxCaptionCues));
}

Json textParam(std::string description) {
    Json s = schema::string(std::move(description) + " UTF-8; a newline starts a new line.");
    s["maxLength"] = static_cast<std::int64_t>(kMaxCaptionTextBytes);
    return s;
}

Json nameParam(std::string description, std::size_t maxBytes) {
    Json s = schema::string(std::move(description));
    s["minLength"] = 1;
    s["maxLength"] = static_cast<std::int64_t>(maxBytes);
    return s;
}

Json colorParam(std::string description) {
    return Json{{"type", Json::array({"string", "array"})},
                {"description", std::move(description) + " \"#RRGGBB\", or [red, green, blue] from 0 to 1."},
                {"items", schema::number("A component, 0..1.", 0.0, 1.0)},
                {"minItems", 3},
                {"maxItems", 3}};
}

/// The caption track named by "captionTrack", or the sequence's first.
const CaptionTrack& captionTrackOf(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    if (seq.captionTracks.empty()) {
        fail(ErrorCode::NotFound, "the sequence has no caption tracks (captions.addTrack or captions.generate makes one)");
    }
    if (!c.has("captionTrack")) {
        return seq.captionTracks.front();
    }
    const auto id = static_cast<std::uint64_t>(c.integer("captionTrack"));
    const auto it = std::find_if(seq.captionTracks.begin(), seq.captionTracks.end(),
                                 [&](const CaptionTrack& t) { return t.id.value == id; });
    if (it == seq.captionTracks.end()) {
        fail(ErrorCode::NotFound, "no caption track " + std::to_string(id) + " in this sequence",
             Json{{"path", "/captionTrack"}});
    }
    return *it;
}

std::size_t cueIndexOf(ExecutionContext& c, const CaptionTrack& track, const char* key = "cue") {
    const std::size_t index = c.indexValue(c.params()[key], std::string("/") + key);
    if (index >= track.cues.size()) {
        fail(ErrorCode::NotFound,
             "caption track " + std::to_string(track.id.value) + " has no cue " + std::to_string(index) + " (it has " +
                 std::to_string(track.cues.size()) + ")",
             Json{{"path", std::string("/") + key}});
    }
    return index;
}

SpeakerId speakerValue(ExecutionContext& c, const Json& value, const std::string& path) {
    if (value.is_null()) {
        return {};
    }
    const SpeakerId id{static_cast<std::uint64_t>(c.indexValue(value, path))};
    if (!c.state().findSpeaker(id)) {
        fail(ErrorCode::NotFound, "speaker " + std::to_string(id.value) + " does not exist", Json{{"path", path}});
    }
    return id;
}

CaptionRgb colorValue(const Json& value, const std::string& path) {
    if (value.is_array()) {
        return {value[0].get<double>(), value[1].get<double>(), value[2].get<double>()};
    }
    const std::string text = value.get<std::string>();
    unsigned r = 0;
    unsigned g = 0;
    unsigned b = 0;
    if (text.size() != 7 || text[0] != '#' || std::sscanf(text.c_str() + 1, "%2x%2x%2x", &r, &g, &b) != 3) {
        fail(ErrorCode::InvalidParams, "a colour is \"#RRGGBB\" or [red, green, blue]", Json{{"path", path}});
    }
    return {r / 255.0, g / 255.0, b / 255.0};
}

std::uint32_t speakerColorValue(const Json& value, const std::string& path) {
    const CaptionRgb rgb = colorValue(value, path);
    const auto byte = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return (byte(rgb[0]) << 16) | (byte(rgb[1]) << 8) | byte(rgb[2]);
}

std::string hexColor(const CaptionRgb& c) {
    const auto byte = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return std::format("#{:02X}{:02X}{:02X}", byte(c[0]), byte(c[1]), byte(c[2]));
}

// --- Styles ------------------------------------------------------------------------------------------------

Json styleSchema() {
    const auto num = [](const char* text, double lo, double hi) { return schema::number(text, lo, hi); };
    Json properties{
        {"fontFamily", nameParam("Font family; empty: the platform's sans-serif.", kMaxCaptionFontBytes)},
        {"fontWeight", schema::integer("100..900 (400 regular, 700 bold).", 100, 900)},
        {"fontSize", num("Font size as a fraction of the frame height (0.0444 = 48 px on 1080p).", 0.005, 0.5)},
        {"fill", colorParam("Text colour.")},
        {"outlineWidth", num("Outline width, fraction of the font size (0 = none).", 0.0, 0.5)},
        {"outline", colorParam("Outline colour.")},
        {"shadowOpacity", num("Shadow opacity (0 = none).", 0.0, 1.0)},
        {"shadowColor", colorParam("Shadow colour.")},
        {"shadowDistance", num("Shadow distance, fraction of the font size.", 0.0, 2.0)},
        {"shadowAngle", num("Shadow direction in degrees, clockwise from the x axis.", -360.0, 360.0)},
        {"shadowSoftness", num("Shadow blur, fraction of the font size.", 0.0, 2.0)},
        {"box", schema::boolean("A background box behind the text.")},
        {"boxColor", colorParam("Box colour.")},
        {"boxOpacity", num("Box opacity.", 0.0, 1.0)},
        {"boxPadding", num("Box padding, fraction of the font size.", 0.0, 2.0)},
        {"boxRadius", num("Box corner radius, fraction of the font size.", 0.0, 2.0)},
        {"features", schema::string("OpenType features for the shaper, as CSS font-feature-settings: calt, -liga, ss01, salt=2 (at most "
                                    "16; \"\" = the font's defaults). Persian and Arabic joining forms and contextual alternates are "
                                    "on already.")},
        {"placement", schema::enumeration({"bottom", "top", "custom"},
                                          "Bottom or top of the safe area, or at positionX/positionY.")},
        {"safeArea", num("Safe-area inset from each frame edge, fraction of the frame.", 0.0, 0.4)},
        {"positionX", num("Custom placement: centre of the text, fraction of the frame width.", 0.0, 1.0)},
        {"positionY", num("Custom placement: centre of the text, fraction of the frame height.", 0.0, 1.0)},
        {"alignment", schema::enumeration({"left", "center", "right"}, "Line alignment.")},
        {"maxWidth", num("Widest text block, fraction of the frame width.", 0.1, 1.0)},
        {"lineSpacing", num("Line spacing (1 = the font's).", 0.5, 3.0)},
        {"highlight", colorParam("Karaoke: colour of the words already spoken.")},
        {"animation", schema::enumeration({"none", "pop", "fade", "wordByWord", "wordPop", "bounce", "highlightBar",
                                           "typewriter", "zoom"},
                                          "Kinetic captions: pop / fade (the whole caption), wordByWord (words appear as "
                                          "spoken), wordPop (each word pops in), bounce (the spoken word hops), "
                                          "highlightBar (a bar slides to the spoken word), typewriter (letters appear "
                                          "one by one), zoom (the spoken word grows). The word looks follow the "
                                          "transcript's word times (else the cue's time shared by word length).")},
        {"animationSpeed", num("Seconds one word's transition takes (default 0.15); fade takes a third longer.", 0.05, 1.0)},
        {"animationEase", schema::enumeration({"smooth", "overshoot", "linear"},
                                              "The curve of a word's transition (default overshoot: a little past, then settles).")},
        {"emphasis", schema::enumeration({"none", "keywords", "auto"},
                                         "Emphasised words are drawn in emphasisColor and emphasisScale larger: keywords "
                                         "(the words of emphasisWords) or auto (those plus numbers, words in capitals "
                                         "and words followed by \"!\").")},
        {"emphasisWords", schema::array(schema::string("A keyword, compared without case."),
                                        "The keywords (at most 64 words of up to 64 bytes).", 0)},
        {"emphasisColor", colorParam("Colour of emphasised words.")},
        {"emphasisScale", num("How much larger an emphasised word is drawn (1 = not larger).", 1.0, 1.5)},
        {"textCase", schema::enumeration({"asWritten", "lower", "upper"},
                                         "The case the caption text is drawn and exported in (cue text is not changed).")},
        {"stripPunctuation", schema::boolean("Draw and export the text without commas, full stops, quotes and the like "
                                             "(apostrophes and hyphens inside words, and the separators of numbers, stay).")},
        {"stripAccents", schema::boolean("Draw and export the text without accents: marks on Latin, Greek and Cyrillic letters go, and a few letters fold to plain ones (Polish l-stroke, o-slash, eth, thorn); Thai vowel and tone marks stay.")},
        {"showSpeakers", schema::boolean("Start each cue with its speaker's name in the speaker's colour.")},
        {"allowOverlap", schema::boolean("Cues of the track may overlap (stacked).")},
        {"brand",
         schema::object({{"fill", schema::string("A brand kit colour for the text (\"\" = none).")},
                         {"outline", schema::string("A brand kit colour for the outline.")},
                         {"shadow", schema::string("A brand kit colour for the shadow.")},
                         {"box", schema::string("A brand kit colour for the box.")},
                         {"highlight", schema::string("A brand kit colour for the karaoke highlight.")},
                         {"font", schema::string("A brand kit font role (\"body\") for the family and weight.")}})}};
    properties["brand"]["description"] =
        "C.13: brand kit colours and font role that override the colours and font above while the project's kit has them "
        "(brand.set re-themes the captions); fields not given keep their values.";
    properties["fontFamily"].erase("minLength");
    return Json{{"type", "object"},
                {"description", "Style fields to change; the others keep their values."},
                {"properties", std::move(properties)},
                {"additionalProperties", false}};
}

/// `style` with the fields of the "style" param applied.
CaptionStyle patchedStyle(ExecutionContext& c, CaptionStyle s, const char* key = "style") {
    if (!c.has(key)) {
        return s;
    }
    const Json& p = c.params()[key];
    const std::string base = std::string("/") + key + "/";
    const auto number = [&](const char* field, double& out) {
        if (p.contains(field)) {
            out = p[field].get<double>();
        }
    };
    const auto color = [&](const char* field, CaptionRgb& out) {
        if (p.contains(field)) {
            out = colorValue(p[field], base + field);
        }
    };
    if (p.contains("fontFamily")) {
        s.fontFamily = p["fontFamily"].get<std::string>();
    }
    if (p.contains("fontWeight")) {
        s.fontWeight = p["fontWeight"].get<std::int32_t>();
    }
    if (p.contains("features")) { // 0.7 (card #138)
        const auto parsed = parseFontFeatures(p["features"].get<std::string>());
        if (!parsed) {
            fail(ErrorCode::InvalidParams, "OpenType features: " + parsed.error(), Json{{"path", base + "features"}});
        }
        s.features = toString(*parsed);
    }
    number("fontSize", s.fontSize);
    color("fill", s.fill);
    number("outlineWidth", s.outlineWidth);
    color("outline", s.outline);
    number("shadowOpacity", s.shadowOpacity);
    color("shadowColor", s.shadowColor);
    number("shadowDistance", s.shadowDistance);
    number("shadowAngle", s.shadowAngle);
    number("shadowSoftness", s.shadowSoftness);
    if (p.contains("box")) {
        s.box = p["box"].get<bool>();
    }
    color("boxColor", s.boxColor);
    number("boxOpacity", s.boxOpacity);
    number("boxPadding", s.boxPadding);
    number("boxRadius", s.boxRadius);
    if (p.contains("placement")) {
        s.placement = *captionPlacementFromString(p["placement"].get<std::string>());
    }
    number("safeArea", s.safeArea);
    if (p.contains("positionX")) {
        s.positionX = ParamValue(p["positionX"].get<double>());
    }
    if (p.contains("positionY")) {
        s.positionY = ParamValue(p["positionY"].get<double>());
    }
    if (p.contains("alignment")) {
        s.alignment = *captionAlignFromString(p["alignment"].get<std::string>());
    }
    number("maxWidth", s.maxWidth);
    number("lineSpacing", s.lineSpacing);
    color("highlight", s.highlight);
    if (p.contains("animation")) {
        s.animation = *captionAnimationFromString(p["animation"].get<std::string>());
    }
    number("animationSpeed", s.animationSpeed);
    if (p.contains("animationEase")) {
        s.animationEase = *captionEaseFromString(p["animationEase"].get<std::string>());
    }
    if (p.contains("emphasis")) {
        s.emphasis = *captionEmphasisFromString(p["emphasis"].get<std::string>());
    }
    if (p.contains("emphasisWords")) {
        s.emphasisWords = p["emphasisWords"].get<std::vector<std::string>>();
    }
    color("emphasisColor", s.emphasisColor);
    number("emphasisScale", s.emphasisScale);
    if (p.contains("textCase")) {
        s.textCase = *captionTextCaseFromString(p["textCase"].get<std::string>());
    }
    if (p.contains("stripPunctuation")) {
        s.stripPunctuation = p["stripPunctuation"].get<bool>();
    }
    if (p.contains("stripAccents")) {
        s.stripAccents = p["stripAccents"].get<bool>();
    }
    if (p.contains("showSpeakers")) {
        s.showSpeakers = p["showSpeakers"].get<bool>();
    }
    if (p.contains("allowOverlap")) {
        s.allowOverlap = p["allowOverlap"].get<bool>();
    }
    if (p.contains("brand")) {
        const Json& b = p["brand"];
        for (const auto& [field, target] : {std::pair{"fill", &s.brand.fill}, std::pair{"outline", &s.brand.outline},
                                            std::pair{"shadow", &s.brand.shadow}, std::pair{"box", &s.brand.box},
                                            std::pair{"highlight", &s.brand.highlight}, std::pair{"font", &s.brand.font}}) {
            if (b.contains(field)) {
                *target = b[field].get<std::string>();
            }
        }
    }
    if (const auto problem = captionStyleProblem(s)) {
        fail(ErrorCode::InvalidParams, "the style " + *problem, Json{{"path", std::string("/") + key}});
    }
    return s;
}

Json positionJson(const ParamValue& v) {
    if (v.isConstant()) {
        return v.constant();
    }
    Json keys = Json::array();
    for (const Keyframe& k : v.curve()->keys) {
        keys.push_back(Json::array({k.time.value, k.value}));
    }
    return Json{{"keys", std::move(keys)}};
}

}  // namespace

Json captionStyleJson(const CaptionStyle& s) {
    Json out{{"fontFamily", s.fontFamily},
                {"fontWeight", s.fontWeight},
                {"fontSize", s.fontSize},
                {"fill", hexColor(s.fill)},
                {"outlineWidth", s.outlineWidth},
                {"outline", hexColor(s.outline)},
                {"shadowOpacity", s.shadowOpacity},
                {"shadowColor", hexColor(s.shadowColor)},
                {"shadowDistance", s.shadowDistance},
                {"shadowAngle", s.shadowAngle},
                {"shadowSoftness", s.shadowSoftness},
                {"box", s.box},
                {"boxColor", hexColor(s.boxColor)},
                {"boxOpacity", s.boxOpacity},
                {"boxPadding", s.boxPadding},
                {"boxRadius", s.boxRadius},
                {"features", s.features},
                {"placement", std::string(toString(s.placement))},
                {"safeArea", s.safeArea},
                {"positionX", positionJson(s.positionX)},
                {"positionY", positionJson(s.positionY)},
                {"alignment", std::string(toString(s.alignment))},
                {"maxWidth", s.maxWidth},
                {"lineSpacing", s.lineSpacing},
                {"highlight", hexColor(s.highlight)},
                {"animation", std::string(toString(s.animation))},
                {"animationSpeed", s.animationSpeed},
                {"animationEase", std::string(toString(s.animationEase))},
                {"emphasis", std::string(toString(s.emphasis))},
                {"emphasisWords", s.emphasisWords},
                {"emphasisColor", hexColor(s.emphasisColor)},
                {"emphasisScale", s.emphasisScale},
                {"textCase", std::string(toString(s.textCase))},
                {"stripPunctuation", s.stripPunctuation},
                {"stripAccents", s.stripAccents},
                {"showSpeakers", s.showSpeakers},
                {"allowOverlap", s.allowOverlap}};
    if (!s.brand.empty()) {
        out["brand"] = Json{{"fill", s.brand.fill},       {"outline", s.brand.outline},     {"shadow", s.brand.shadow},
                            {"box", s.brand.box},         {"highlight", s.brand.highlight}, {"font", s.brand.font}};
    }
    return out;
}

namespace {

CommandOutput projectEdited(ProjectState state, Json data = nullptr) {
    CommandOutput out;
    out.state = std::move(state);
    out.data = std::move(data);
    return out;
}

/// 0.7: two caption preset names are the same ignoring case (ASCII), as the project treats them.
bool sameCaptionPresetName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// --- Speakers ----------------------------------------------------------------------------------------------

/// The source ranges of media streams that the audio clips play over the timeline range [from, to) (on `only` when
/// given), in clip order.
struct SourceSpan {
    MediaId media;
    std::int32_t stream = 0;
    TimeRange source;
};

std::vector<SourceSpan> sourceSpans(ExecutionContext& c, Ticks from, Ticks to, std::optional<TrackId> only) {
    std::vector<SourceSpan> out;
    for (const TrackPtr& t : c.sequence().audioTracks) {
        if (only && t->id != *only) {
            continue;
        }
        for (const Clip& clip : t->clips) {
            const Ticks a = std::max(from, clip.start());
            const Ticks b = std::min(to, clip.end());
            if (a >= b) {
                continue;
            }
            // The media the stretch shows (through holds, ramps and reverse segments of a remap too).
            out.push_back({clip.media, clip.stream.index, clip.shownSourceRange(a, b)});
        }
    }
    return out;
}

// --- 0.2: Find and Replace, import of caption files ------------------------------------------------------------

CommandOutput readOnly(ExecutionContext& c, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    return out;
}

/// A match of captions.find: the caption track, the cue and the byte range of the cue's text.
struct CaptionMatch {
    CaptionTrackId track;
    std::size_t cue = 0;
    std::size_t start = 0;
    std::size_t end = 0;
};

/// Every match in the sequence's caption tracks (`only` when given): track order, cue order, position.
std::vector<CaptionMatch> captionMatches(const Sequence& seq, std::optional<CaptionTrackId> only, std::string_view find,
                                         TextFindOptions options) {
    std::vector<CaptionMatch> out;
    for (const CaptionTrack& t : seq.captionTracks) {
        if (only && t.id != *only) {
            continue;
        }
        for (std::size_t i = 0; i < t.cues.size(); ++i) {
            for (const auto& [b, e] : findText(t.cues[i].text, find, options)) {
                out.push_back(CaptionMatch{t.id, i, b, e});
            }
        }
    }
    return out;
}

Json captionMatchJson(ExecutionContext& c, const Sequence& seq, const CaptionMatch& m) {
    Json out{{"captionTrack", m.track.value}, {"cue", m.cue}, {"start", m.start}, {"end", m.end}};
    for (const CaptionTrack& t : seq.captionTracks) {
        if (t.id == m.track && m.cue < t.cues.size()) {
            out["time"] = c.timecode(t.cues[m.cue].start);
            out["text"] = t.cues[m.cue].text;
        }
    }
    return out;
}

Json spellIssueJson(const SpellIssue& issue) {
    return Json{{"start", issue.start},
                {"end", issue.end},
                {"word", issue.word},
                {"action", std::string(toString(issue.action))},
                {"suggestions", issue.suggestions}};
}

TextFindOptions findOptions(ExecutionContext& c) {
    return TextFindOptions{c.boolean("matchCase", false), c.boolean("wholeWord", false)};
}

Json findParam() {
    Json s = schema::string("The text to find (plain text, not a pattern).");
    s["minLength"] = 1;
    s["maxLength"] = 1024;
    return s;
}

Json replaceParam() {
    Json s = schema::string("What each match becomes (may be empty).");
    s["maxLength"] = 1024;
    return s;
}

/// A caption file's cue before it is placed: times from the file, text, the speaker's name (timed text only).
struct FileCue {
    Ticks start;
    Ticks end;
    std::string text;
    std::string speaker;
};

constexpr std::uintmax_t kMaxCaptionFileBytes = 16u * 1024u * 1024u;

/// The file's bytes (at most kMaxCaptionFileBytes).
std::string readCaptionFileBytes(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        fail(ErrorCode::NotFound, "cannot read \"" + pathToUtf8(path) + "\"", Json{{"path", "/path"}});
    }
    if (size > kMaxCaptionFileBytes) {
        fail(ErrorCode::InvalidParams, "the caption file is larger than 16 MiB", Json{{"path", "/path"}});
    }
    std::ifstream in(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    return bytes;
}

/// The text of a caption or transcript file: UTF-8 without a byte order mark or carriage returns; a .docx (0.2
/// graphics: a transcript exported as a Word document) is read through interchange::readDocxText.
std::string readCaptionFile(const std::filesystem::path& path) {
    std::string text = readCaptionFileBytes(path);
    std::string ext = pathToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext == ".docx") {
        auto document = interchange::readDocxText(text);
        if (!document) {
            fail(ErrorCode::InvalidParams, document.error().message, Json{{"path", "/path"}});
        }
        text = std::move(*document);
    }
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    std::erase(text, '\r');
    return text;
}

std::vector<std::string> linesOf(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = std::min(text.find('\n', start), text.size());
        out.emplace_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::string trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

/// "hh:mm:ss,mmm" / "hh:mm:ss.mmm" / "mm:ss.mmm" (SRT, WebVTT); nullopt when it is not one.
std::optional<Ticks> clockTime(std::string_view s) {
    int h = 0;
    int m = 0;
    int sec = 0;
    int ms = 0;
    char sep = 0;
    int used = 0;
    const std::string text(s);
    if (std::sscanf(text.c_str(), "%d:%d:%d%c%3d%n", &h, &m, &sec, &sep, &ms, &used) == 5 && (sep == ',' || sep == '.') &&
        used == static_cast<int>(text.size())) {
    } else if (std::sscanf(text.c_str(), "%d:%d%c%3d%n", &m, &sec, &sep, &ms, &used) == 4 && (sep == ',' || sep == '.') &&
               used == static_cast<int>(text.size())) {
        h = 0;
    } else {
        return std::nullopt;
    }
    if (h < 0 || h > 99 || m < 0 || m > 59 || sec < 0 || sec > 59 || ms < 0) {
        return std::nullopt;
    }
    const std::int64_t millis = ((static_cast<std::int64_t>(h) * 60 + m) * 60 + sec) * 1000 + ms;
    return Ticks{millis * kTicksPerSecond / 1000};
}

/// Text without <i>, <b>, <font ...>, <v Speaker> tags (SRT and WebVTT styling).
std::string withoutTags(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            const std::size_t close = s.find('>', i);
            if (close != std::string_view::npos && close - i < 64) {
                i = close;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

/// SRT and WebVTT: blocks with a "start --> end" line, the lines after it the text.
std::vector<FileCue> parseTimedBlocks(const std::vector<std::string>& lines) {
    std::vector<FileCue> out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::size_t arrow = lines[i].find("-->");
        if (arrow == std::string::npos) {
            continue;
        }
        const std::string a = trimmed(std::string_view(lines[i]).substr(0, arrow));
        std::string b = trimmed(std::string_view(lines[i]).substr(arrow + 3));
        if (const std::size_t space = b.find_first_of(" \t"); space != std::string::npos) {
            b.resize(space); // WebVTT cue settings
        }
        const auto start = clockTime(a);
        const auto end = clockTime(b);
        if (!start || !end) {
            continue;
        }
        FileCue cue{*start, *end, {}, {}};
        std::size_t j = i + 1;
        for (; j < lines.size() && !trimmed(lines[j]).empty(); ++j) {
            cue.text += (cue.text.empty() ? "" : "\n") + withoutTags(trimmed(lines[j]));
        }
        out.push_back(std::move(cue));
        i = j;
    }
    return out;
}

/// Premiere's transcript text export (and transcript.export): blocks of "start - end" (timecodes), the speaker when
/// the block has three lines or more, then the text.
std::vector<FileCue> parseTimedText(ExecutionContext& c, const std::vector<std::string>& lines) {
    std::vector<FileCue> out;
    std::vector<std::string> block;
    const auto flush = [&] {
        if (block.size() >= 2) {
            const std::size_t dash = block[0].find(" - ");
            if (dash != std::string::npos) {
                try {
                    const Ticks start = c.timeValue(Json(trimmed(std::string_view(block[0]).substr(0, dash))), "/path");
                    const Ticks end = c.timeValue(Json(trimmed(std::string_view(block[0]).substr(dash + 3))), "/path");
                    FileCue cue{start, end, {}, block.size() >= 3 ? block[1] : std::string()};
                    for (std::size_t k = block.size() >= 3 ? 2 : 1; k < block.size(); ++k) {
                        cue.text += (cue.text.empty() ? "" : "\n") + block[k];
                    }
                    out.push_back(std::move(cue));
                } catch (const CommandFailure&) {
                    // not a timed block: skipped
                }
            }
        }
        block.clear();
    };
    for (const std::string& line : lines) {
        const std::string t = trimmed(line);
        if (t.empty()) {
            flush();
        } else {
            block.push_back(t);
        }
    }
    flush();
    return out;
}

/// The first `max` bytes of UTF-8 text, cut at a character boundary.
std::string clippedUtf8(std::string s, std::size_t max) {
    if (s.size() <= max) {
        return s;
    }
    std::size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
        --n;
    }
    s.resize(n);
    return s;
}


// --- 0.7 (card 075): captions to graphics -------------------------------------------------------------------------

/// A cue's text as the graphic shows it: lower case applied (upper case is the text's All Caps, which keeps the letters).
std::string graphicCueText(const CaptionCue& cue, const CaptionStyle& style) {
    std::string text = cue.text;
    if (style.textCase == CaptionTextCase::Lower) {
        for (char& ch : text) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
    }
    return text;
}

GraphicColor graphicColor(const CaptionRgb& rgb, double alpha = 1.0) {
    GraphicColor color;
    color.rgba = {rgb[0], rgb[1], rgb[2], alpha};
    return color;
}

/// The graphic of one cue, in the look of the track's caption style (CaptionStyle.h) on a `width` x `height` canvas. The text
/// is paragraph text in a box as wide as the caption block (the style's maxWidth) and as tall as its lines (boxHeight 0):
/// the box grows with the text, and a caption at the bottom (or top) of the frame is pinned to the frame's edge, so a longer
/// text grows upwards (downwards) from the safe margin instead of running off the picture.
Graphic graphicOfCue(const CaptionCue& cue, const CaptionStyle& style, std::int32_t width, std::int32_t height, const std::string& name) {
    const double size = std::clamp(style.fontSize * height, 1.0, 2000.0);
    Graphic graphic;
    graphic.name = name;
    graphic.width = width;
    graphic.height = height;

    GraphicLayer layer;
    layer.id = 1;
    layer.name = name;
    layer.kind = LayerKind::Text;

    TextRun run;
    run.text = graphicCueText(cue, style);
    run.style.fontFamily = style.fontFamily;
    run.style.fontWeight = style.fontWeight;
    run.style.fontSize = size;
    run.style.fill = graphicColor(style.fill);
    run.style.caps = style.textCase == CaptionTextCase::Upper ? TextCaps::AllCaps : TextCaps::Normal;
    run.style.leading = style.lineSpacing == 1.0 ? 0.0 : 1.2 * size * style.lineSpacing;
    layer.text.runs = {run};
    layer.text.align = style.alignment == CaptionAlign::Left    ? TextAlign::Left
                       : style.alignment == CaptionAlign::Right ? TextAlign::Right
                                                                : TextAlign::Center;
    const double boxWidth = std::clamp(style.maxWidth * width, 1.0, static_cast<double>(width));
    layer.text.boxWidth = boxWidth;
    layer.text.boxHeight = 0.0; // as tall as the text: the box grows with it
    if (style.box) {
        layer.text.background.enabled = true;
        layer.text.background.color = graphicColor(style.boxColor, style.boxOpacity);
        layer.text.background.padding = style.boxPadding * size;
        layer.text.background.radius = style.boxRadius * size;
    }
    if (style.outlineWidth > 0.0) {
        TextStroke stroke;
        stroke.color = graphicColor(style.outline);
        stroke.width = ParamValue(style.outlineWidth * size);
        stroke.position = StrokePosition::Outside;
        layer.text.strokes = {stroke};
    }
    if (style.shadowOpacity > 0.0) {
        layer.shadow.enabled = true;
        layer.shadow.color = graphicColor(style.shadowColor);
        layer.shadow.opacity = style.shadowOpacity;
        layer.shadow.angle = style.shadowAngle;
        layer.shadow.distance = style.shadowDistance * size;
        layer.shadow.softness = style.shadowSoftness * size;
    }

    // Where the block sits: its centre column at the frame's middle (or the custom position), the text laid out from the
    // block's left, centre or right edge.
    const double centreX = style.placement == CaptionPlacement::Custom ? style.positionX.constant() * width : width / 2.0;
    layer.transform.x = ParamValue(style.alignment == CaptionAlign::Left    ? centreX - boxWidth / 2.0
                                   : style.alignment == CaptionAlign::Right ? centreX + boxWidth / 2.0
                                                                            : centreX);
    // An estimate of the lines, for where the top edge starts before the frame's edge pins it.
    std::size_t lines = 1;
    for (const char ch : run.text) {
        lines += ch == '\n' ? 1 : 0;
    }
    const double lineHeight = run.style.leading > 0.0 ? run.style.leading : 1.2 * size;
    const double estimated = static_cast<double>(lines) * lineHeight;
    const double margin = style.safeArea * height;
    if (style.placement == CaptionPlacement::Top) {
        layer.transform.y = ParamValue(margin);
        LayerPin pin;
        pin.target = LayerPin::kVideoFrame;
        pin.edges = LayerPin::kTop;
        pin.offsets = {0.0, margin, 0.0, 0.0};
        layer.pin = pin;
    } else if (style.placement == CaptionPlacement::Bottom) {
        layer.transform.y = ParamValue(height - margin - estimated);
        LayerPin pin;
        pin.target = LayerPin::kVideoFrame;
        pin.edges = LayerPin::kBottom;
        pin.offsets = {0.0, 0.0, 0.0, -margin};
        layer.pin = pin;
    } else {
        layer.transform.y = ParamValue(style.positionY.constant() * height - estimated / 2.0);
    }
    graphic.layers = {std::move(layer)};
    return graphic;
}

CommandOutput captionsToGraphics(ExecutionContext& c) {
    const CaptionTrack& source = captionTrackOf(c);
    const Sequence& seq = c.sequence();
    const std::optional<Ticks> from = c.optionalTime("from");
    const std::optional<Ticks> to = c.optionalTime("to");
    if (from && to && *to <= *from) {
        fail(ErrorCode::InvalidParams, "\"to\" must come after \"from\"", Json{{"path", "/to"}});
    }
    const std::string captions = c.string("captions", "hide");
    if (captions != "hide" && captions != "keep" && captions != "remove") {
        fail(ErrorCode::InvalidParams, "\"captions\" is \"hide\", \"keep\" or \"remove\"", Json{{"path", "/captions"}});
    }
    // The cues that start inside the range, in order.
    std::vector<const CaptionCue*> cues;
    for (const CaptionCue& cue : source.cues) {
        if ((!from || cue.start >= *from) && (!to || cue.start < *to) && !cue.text.empty()) {
            cues.push_back(&cue);
        }
    }
    if (cues.empty()) {
        fail(ErrorCode::EditRejected, "caption track " + std::to_string(source.id.value) + " has no cues there to convert");
    }
    const CaptionStyle style = resolvedCaptionStyle(source.style, c.state().brandKit ? &*c.state().brandKit : nullptr);

    ProjectState state = c.state();
    // Where the graphics go: the named video track when it is free under every cue, else new video tracks on top - one,
    // or more when the track's cues overlap (a style that allows it).
    std::vector<TrackId> lanes;
    std::vector<Ticks> laneEnd;
    const bool named = c.has("track");
    if (named) {
        const TrackId id = c.track("track");
        const Track* t = seq.findTrack(id);
        if (t == nullptr || t->kind != TrackKind::Video) {
            fail(ErrorCode::InvalidParams, "\"track\" must be a video track", Json{{"path", "/track"}});
        }
        for (const CaptionCue* cue : cues) {
            for (const Clip& clip : t->clips) {
                if (clip.start() < cue->end() && cue->start < clip.end()) {
                    fail(ErrorCode::EditRejected,
                         "track " + c.trackLabel(id) + " is not free under the cue at " + c.timecode(cue->start) +
                             ": name a free track, or leave \"track\" out for new ones");
                }
            }
        }
        lanes.push_back(id);
        laneEnd.push_back(Ticks{});
    }
    CommandOutput out;
    Json placed = Json::array();
    const std::string baseName = source.name.empty() ? std::string("Caption") : source.name;
    for (const CaptionCue* cue : cues) {
        std::size_t lane = lanes.size();
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            if (laneEnd[i] <= cue->start) {
                lane = i;
                break;
            }
        }
        if (lane == lanes.size()) {
            if (named || lanes.size() >= 8) {
                fail(ErrorCode::EditRejected, "the cues at " + c.timecode(cue->start) + " overlap more than a track can take");
            }
            auto created = c.take(edit::addTrack(state, c.sequenceId(), TrackKind::Video, baseName));
            state = std::move(created.state);
            lanes.push_back(created.id);
            laneEnd.push_back(Ticks{});
        }
        laneEnd[lane] = cue->end();
        edit::EditResult r = c.take(edit::addGraphic(state, c.sequenceId(), lanes[lane], cue->start, cue->duration,
                                                     graphicOfCue(*cue, style, seq.width, seq.height, baseName)));
        state = std::move(r.state);
        out.createdIds.push_back(r.placed.front().value);
        placed.push_back(r.placed.front().value);
        c.checkpoint("converting captions");
    }
    // The captions themselves: hidden (the graphics show the same text), kept as they are, or removed with the track.
    if (captions == "hide") {
        edit::CaptionTrackChange change;
        change.enabled = false;
        state = c.take(edit::setCaptionTrackInfo(state, c.sequenceId(), source.id, change)).state;
    } else if (captions == "remove") {
        if (from || to) {
            fail(ErrorCode::InvalidParams, "\"remove\" takes the whole caption track: leave \"from\" and \"to\" out",
                 Json{{"path", "/captions"}});
        }
        state = c.take(edit::removeCaptionTrack(state, c.sequenceId(), source.id)).state;
    }
    out.state = std::move(state);
    Json labels = Json::array();
    for (const TrackId id : lanes) {
        labels.push_back(c.trackLabel(id));
    }
    out.data = Json{{"clips", std::move(placed)}, {"tracks", std::move(labels)}, {"captions", captions},
                    {"converted", cues.size()}};
    return out;
}

} // namespace

Graphic captionTextGraphic(const std::string& text, const CaptionStyle& style, std::int32_t width, std::int32_t height,
                           const std::string& name) {
    CaptionCue cue;
    cue.text = text;
    return graphicOfCue(cue, style, width, height, name);
}

void registerCaptionCommands(CommandRegistry& registry) {
    // --- Tracks --------------------------------------------------------------------------------------------
    std::vector<std::string> presetNames;
    for (const CaptionPreset& p : builtinCaptionPresets()) {
        presetNames.push_back(p.name);
    }
    std::string presetList;
    for (const std::string& n : presetNames) {
        presetList += (presetList.empty() ? "" : ", ") + n;
    }
    addBuiltin(registry,
               {"captions.addTrack", 1, "New Caption Track",
                "Adds an empty caption track, styled by a preset (built in: " + presetList +
                    "; or one of the project's) and/or style fields. The default look is white text in a dark box at "
                    "the bottom.",
                Capability::EditSequence,
                withSequence({{"name", nameParam("Track name. Default \"Captions\".", kMaxCaptionNameBytes)},
                              {"language", nameParam("ISO 639-1 language, e.g. \"en\", \"th\".", 16)},
                              {"preset", nameParam("A caption preset (captions.presets lists them).", kMaxCaptionNameBytes)},
                              {"style", styleSchema()}}),
                Json{{"name", "English"}, {"language", "en"}, {"preset", "Bold Box"}}, {},
                [](ExecutionContext& c) {
                    CaptionStyle style;
                    if (c.has("preset")) {
                        const auto preset = edit::captionPresetStyle(c.state(), c.string("preset"));
                        if (!preset) {
                            fail(ErrorCode::NotFound, "there is no caption preset \"" + c.string("preset") + "\"",
                                 Json{{"path", "/preset"}});
                        }
                        style = *preset;
                    }
                    style = patchedStyle(c, style);
                    edit::EditResult r = c.take(edit::addCaptionTrack(c.state(), c.sequenceId(), c.string("name"),
                                                                      c.string("language"), style));
                    const CaptionTrackId id = r.createdCaptionTrack;
                    CommandOutput out = edited(std::move(r), Json{{"captionTrack", id.value}});
                    out.createdIds.push_back(id.value);
                    return out;
                }});
    addBuiltin(registry,
               {"captions.setTrack", 1, "Caption Track Settings",
                "Renames a caption track, sets its language, shows/hides it, or draws it together with the main caption "
                "track (bilingual captions).",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"name", nameParam("New name.", kMaxCaptionNameBytes)},
                              {"language", nameParam("ISO 639-1 language.", 16)},
                              {"enabled", schema::boolean("Shown (burned in and exported).")},
                              {"showTogether",
                               schema::boolean("Drawn on screen together with the main caption track (the one chosen for the "
                                               "monitor or the export, else the first shown): the monitor and exports show "
                                               "both, stacked in track order (the first topmost), each in its own style. "
                                               "Needs \"enabled\".")}}),
                Json{{"captionTrack", 57}, {"showTogether", true}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    edit::CaptionTrackChange change;
                    if (c.has("name")) {
                        change.name = c.string("name");
                    }
                    if (c.has("language")) {
                        change.language = c.string("language");
                    }
                    if (c.has("enabled")) {
                        change.enabled = c.boolean("enabled", true);
                    }
                    if (c.has("showTogether")) {
                        change.showTogether = c.boolean("showTogether", false);
                    }
                    return edited(c.take(edit::setCaptionTrackInfo(c.state(), c.sequenceId(), track.id, change)));
                }});

    // --- Cues ----------------------------------------------------------------------------------------------
    addBuiltin(registry,
               {"captions.addCue", 1, "Add Caption",
                "Adds a cue from \"start\" to \"end\" (or for \"duration\"). Refused where it would overlap another cue "
                "(unless the track's style allows overlaps). Returns its index.",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"start", timeParam("Where it starts.")},
                              {"end", timeParam("Where it ends.")},
                              {"duration", timeParam("Or how long it lasts.")},
                              {"text", textParam("The caption.")},
                              {"speaker", schema::integer("Speaker id (transcript.speakers lists them).", 1)}},
                             {"start", "text"}),
                Json{{"start", "00:00:01:00"}, {"end", "00:00:03:12"}, {"text", "Hello there."}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    const Ticks start = c.time("start");
                    Ticks end;
                    if (c.has("end") == c.has("duration")) {
                        fail(ErrorCode::InvalidParams, "give \"end\" or \"duration\"");
                    }
                    end = c.has("end") ? c.time("end") : start + c.time("duration");
                    if (end <= start) {
                        fail(ErrorCode::InvalidParams, "a cue must end after it starts", Json{{"path", "/end"}});
                    }
                    CaptionCue cue{start, end - start, c.string("text"),
                                   c.has("speaker") ? speakerValue(c, c.params()["speaker"], "/speaker") : SpeakerId{}};
                    edit::EditResult r = c.take(edit::addCaptionCue(c.state(), c.sequenceId(), track.id, cue));
                    const Sequence& after = *r.state.findSequence(c.sequenceId());
                    std::size_t index = 0;
                    for (const CaptionTrack& t : after.captionTracks) {
                        if (t.id == track.id) {
                            const auto it = std::find(t.cues.begin(), t.cues.end(), cue);
                            index = static_cast<std::size_t>(it - t.cues.begin());
                        }
                    }
                    return edited(std::move(r), Json{{"cue", index}});
                }});
    addBuiltin(registry,
               {"captions.updateCue", 1, "Edit Caption",
                "Changes a cue's text, start, end or speaker (null clears the speaker).", Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"cue", cueParam("The cue.")},
                              {"text", textParam("New text.")},
                              {"start", timeParam("New start.")},
                              {"end", timeParam("New end.")},
                              {"speaker", Json{{"type", Json::array({"integer", "null"})},
                                               {"description", "Speaker id, or null for none."},
                                               {"minimum", 1}}}},
                             {"cue"}),
                Json{{"cue", 3}, {"text", "Hello, world."}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    const std::size_t index = cueIndexOf(c, track);
                    edit::CaptionCueChange change;
                    if (c.has("text")) {
                        change.text = c.string("text");
                    }
                    change.start = c.optionalTime("start");
                    change.end = c.optionalTime("end");
                    if (c.params().contains("speaker")) {
                        change.speaker = speakerValue(c, c.params()["speaker"], "/speaker");
                    }
                    return edited(c.take(edit::updateCaptionCue(c.state(), c.sequenceId(), track.id, index, change)));
                }});
    addBuiltin(registry,
               {"captions.removeCue", 1, "Delete Caption", "Deletes a cue of a caption track.", Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()}, {"cue", cueParam("The cue.")}}, {"cue"}),
                Json{{"cue", 3}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    return edited(c.take(edit::removeCaptionCue(c.state(), c.sequenceId(), track.id, cueIndexOf(c, track))));
                }});
    addBuiltin(registry,
               {"captions.splitCue", 1, "Split Caption",
                "Cuts a cue in two at a time inside it. The text splits at \"textOffset\" (a byte offset) or else at the "
                "line-break opportunity nearest the same share of the text (Thai and other unspaced scripts break "
                "between words where the host provides line-break rules).",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"cue", cueParam("The cue.")},
                              {"at", timeParam("Where the second cue starts (inside the cue).")},
                              {"textOffset", schema::integer("Byte offset of the second cue's text.", 1,
                                                             static_cast<std::int64_t>(kMaxCaptionTextBytes))}},
                             {"cue", "at"}),
                Json{{"cue", 3}, {"at", "00:00:12:10"}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    std::optional<std::size_t> offset;
                    if (c.has("textOffset")) {
                        offset = c.indexValue(c.params()["textOffset"], "/textOffset");
                    }
                    return edited(c.take(edit::splitCaptionCue(c.state(), c.sequenceId(), track.id, cueIndexOf(c, track),
                                                               c.time("at"), offset, lineBreakerOf(c))));
                }});
    addBuiltin(registry,
               {"captions.mergeCues", 1, "Merge Captions",
                "Joins \"count\" consecutive cues (default 2) into one, its text re-wrapped to maxLines lines of "
                "maxCharsPerLine characters.",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"cue", cueParam("The first cue.")},
                              {"count", schema::integer("How many cues (default 2).", 2, 1000)},
                              {"maxCharsPerLine", schema::integer("Default 42.", 8, 200)},
                              {"maxLines", schema::integer("Default 2.", 1, 6)}},
                             {"cue"}),
                Json{{"cue", 3}, {"count", 2}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    const std::size_t index = cueIndexOf(c, track);
                    return edited(c.take(edit::mergeCaptionCues(
                        c.state(), c.sequenceId(), track.id, index, static_cast<std::size_t>(c.integer("count", 2)),
                        lineBreakerOf(c), static_cast<int>(c.integer("maxCharsPerLine", 42)),
                        static_cast<int>(c.integer("maxLines", 2)))));
                }});
    addBuiltin(registry,
               {"captions.shiftCues", 1, "Shift Captions",
                "Moves the cues that start in [from, to) (default: all of them) by a signed delta, e.g. to resync "
                "captions. Refused when a cue would start before 0 or overlap a cue that stays.",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"from", timeParam("First cue start that moves. Default: 0.")},
                              {"to", timeParam("Cues starting at or after this stay. Default: the end.")},
                              {"delta", deltaParam("How far; positive = later.")}},
                             {"delta"}),
                Json{{"from", "00:01:00:00"}, {"delta", "-00:00:00:12"}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    const Ticks from = c.optionalTime("from").value_or(Ticks{});
                    const Ticks to = c.optionalTime("to").value_or(Ticks::max());
                    if (to <= from) {
                        fail(ErrorCode::InvalidParams, "\"to\" must come after \"from\"", Json{{"path", "/to"}});
                    }
                    return edited(c.take(edit::shiftCaptionCues(c.state(), c.sequenceId(), track.id,
                                                                TimeRange::fromStartEnd(from, to), c.time("delta"))));
                }});

    // --- Styles and presets --------------------------------------------------------------------------------
    addBuiltin(registry,
               {"captions.setStyle", 1, "Caption Style",
                "Changes how a caption track looks: font, size (fraction of the frame height), fill, outline, shadow, "
                "background box, placement (bottom, top or a custom position), alignment, width, karaoke highlight, "
                "speaker names. Fields not given keep their values. Preview and burn-in export both use it.",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()}, {"style", styleSchema()}}, {"style"}),
                Json{{"style", Json{{"fontSize", 0.05}, {"box", false}, {"outlineWidth", 0.08}}}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    return edited(c.take(edit::setCaptionStyle(c.state(), c.sequenceId(), track.id,
                                                               patchedStyle(c, track.style))),
                                  Json{{"style", captionStyleJson(patchedStyle(c, track.style))}});
                }});
    // 0.7 (card 075): Premiere's Upgrade Caption to Graphic, with a text box that grows with its text.
    addBuiltin(registry,
               {"captions.toGraphics", 1, "Convert Captions to Graphics",
                "Turns the cues of a caption track into graphic clips, one per cue at the cue's time, so the text can be "
                "animated, restyled and moved like a title (Essential Graphics, graphic.applyPreset, Effect Controls). Each is "
                "a text layer in the track's caption style - font, size, colours, outline, shadow, background box, "
                "alignment, placement, line spacing - as paragraph text in a box as wide as the caption block and as tall as "
                "its lines: the box grows with the text, and a caption at the bottom (top) is pinned to the frame's edge, so "
                "editing it to a longer text grows it upwards (downwards) from the safe margin. Lower and upper case are "
                "applied; the kinetic animation looks, speaker names, punctuation and accent rules are not (they belong to "
                "caption rendering). The graphics go on new video tracks on top (more than one when cues overlap), or on "
                "\"track\" when it is free under every cue. \"captions\" says what happens to the cues: \"hide\" switches "
                "the caption track off so the text is not drawn twice (default), \"keep\" leaves it showing, \"remove\" "
                "deletes the track. One undo step.",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"from", timeParam("Only the cues that start at or after this time.")},
                              {"to", timeParam("Only the cues that start before this time.")},
                              {"track", trackParam("A free video track for the graphics (default: new tracks on top).")},
                              {"captions", schema::enumeration({"hide", "keep", "remove"},
                                                               "What happens to the caption track. Default hide.")}}),
                Json{{"captionTrack", 57}, {"captions", "hide"}}, {}, captionsToGraphics});
    addBuiltin(registry,
               {"captions.applyPreset", 1, "Apply Caption Preset",
                "Gives a caption track the style of a preset: built in (" + presetList + ") or one of the project's.",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"preset", nameParam("Preset name (case does not matter).", kMaxCaptionNameBytes)}},
                             {"preset"}),
                Json{{"preset", "Thai-friendly"}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    const auto style = edit::captionPresetStyle(c.state(), c.string("preset"));
                    if (!style) {
                        fail(ErrorCode::NotFound, "there is no caption preset \"" + c.string("preset") + "\"",
                             Json{{"path", "/preset"}});
                    }
                    return edited(c.take(edit::setCaptionStyle(c.state(), c.sequenceId(), track.id, *style)));
                }});
    addBuiltin(registry,
               {"captions.savePreset", 1, "Save Caption Preset",
                "Saves a named caption style in the project: a caption track's style, with \"style\" fields applied on "
                "top (or the default look). Replaces the project preset of that name (every caption track that follows "
                "it as its Track Style takes the new look); built-in names are refused.",
                Capability::EditProject,
                withSequence({{"name", nameParam("Preset name.", kMaxCaptionNameBytes)},
                              {"captionTrack", schema::integer("Caption track whose style to save.", 1)},
                              {"style", styleSchema()}},
                             {"name"}),
                Json{{"name", "Channel look"}, {"captionTrack", 57}}, {},
                [](ExecutionContext& c) {
                    CaptionStyle base;
                    if (c.has("captionTrack")) {
                        base = captionTrackOf(c).style;
                    }
                    return projectEdited(c.take(edit::saveCaptionPreset(c.state(), {c.string("name"), patchedStyle(c, base)})));
                }});
    addBuiltin(registry,
               {"captions.removePreset", 1, "Delete Caption Preset", "Deletes one of the project's caption presets.",
                Capability::EditProject, withSequence({{"name", nameParam("Preset name.", kMaxCaptionNameBytes)}}, {"name"}),
                Json{{"name", "Channel look"}}, {},
                [](ExecutionContext& c) {
                    return projectEdited(c.take(edit::removeCaptionPreset(c.state(), c.string("name"))));
                }});
    // 0.7 (card 115): caption presets as files, to use one look in every project.
    addBuiltin(
        registry,
        command::touchesFiles(
            {"captions.exportPresets", 1, "Export Caption Presets",
             "Writes the project's caption presets (or the ones named) to a file (.wabcut-caption-presets.json) that "
             "captions.importPresets reads into any other project: how one look is reused across projects. \"merge\" keeps "
             "the presets already in the file (those of the same name are replaced) - how the app's own library of looks "
             "grows. Refuses to replace a file unless \"overwrite\" or \"merge\". Built-in presets are in every project and "
             "are not written. The project does not change; a dry run writes nothing.",
             Capability::Read,
             withSequence({{"path", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096},
                                         {"description", "The file to write (UTF-8 path)."}}},
                           {"names", schema::array(nameParam("A preset name.", kMaxCaptionNameBytes),
                                                   "Only these presets (default: all the project's).", 1)},
                           {"merge", schema::boolean("Keep the presets already in the file (default false).")},
                           {"overwrite", schema::boolean("Replace an existing file (default false).")}},
                          {"path"}),
             Json{{"path", "C:/Looks/channel.wabcut-caption-presets.json"}, {"names", Json::array({"Channel look"})}}, {},
             [](ExecutionContext& c) {
                 const std::filesystem::path file = pathFromUtf8(c.string("path"));
                 std::error_code ec;
                 const bool exists = std::filesystem::exists(file, ec);
                 const bool merge = c.boolean("merge", false);
                 if (exists && !merge && !c.boolean("overwrite", false)) {
                     fail(ErrorCode::EditRejected, "the file exists; pass \"overwrite\" to replace it or \"merge\" to add to it",
                          Json{{"path", "/path"}});
                 }
                 std::vector<CaptionPreset> out;
                 if (merge && exists) {
                     std::ifstream in(file, std::ios::binary);
                     const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                     out = c.take(io::captionPresetsFromJson(text));
                 }
                 std::vector<std::string> written;
                 const auto put = [&](const CaptionPreset& p) {
                     std::erase_if(out, [&](const CaptionPreset& q) { return sameCaptionPresetName(q.name, p.name); });
                     out.push_back(p);
                     written.push_back(p.name);
                 };
                 if (c.has("names")) {
                     for (const Json& n : c.params()["names"]) {
                         const CaptionPreset* p = c.state().findCaptionPreset(n.get<std::string>());
                         if (p == nullptr) {
                             fail(ErrorCode::NotFound, "the project has no caption preset \"" + n.get<std::string>() + "\"",
                                  Json{{"path", "/names"}});
                         }
                         put(*p);
                     }
                 } else {
                     for (const CaptionPreset& p : c.state().captionPresets) {
                         put(p);
                     }
                 }
                 if (written.empty()) {
                     fail(ErrorCode::EditRejected, "the project has no caption presets of its own to export");
                 }
                 if (out.size() > kMaxCaptionPresets) {
                     fail(ErrorCode::EditRejected, "a file holds at most " + std::to_string(kMaxCaptionPresets) + " caption presets");
                 }
                 if (!c.dryRun()) {
                     const std::string text = c.take(io::captionPresetsToJson(out));
                     c.take(io::writeFileAtomically(file, text));
                 }
                 return projectEdited(c.state(), Json{{"path", c.string("path")}, {"presets", written}, {"fileHolds", out.size()}});
             }},
            {"path"}));
    addBuiltin(
        registry,
        command::touchesFiles(
            {"captions.importPresets", 1, "Import Caption Presets",
             "Reads a caption presets file (captions.exportPresets, the app's Export Presets) into the project: every preset "
             "in it, or the ones named, becomes one of the project's own. A name the project already has is skipped unless "
             "\"replace\" (then every caption track that follows it takes the new look); built-in names are never replaced. "
             "One undo step.",
             Capability::EditProject,
             withSequence({{"path", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096},
                                         {"description", "The caption presets file (UTF-8 path)."}}},
                           {"names", schema::array(nameParam("A preset name.", kMaxCaptionNameBytes),
                                                   "Only these presets (default: all in the file).", 1)},
                           {"replace", schema::boolean("Replace the project's presets of the same names (default false: skipped).")}},
                          {"path"}),
             Json{{"path", "C:/Looks/channel.wabcut-caption-presets.json"}}, {},
             [](ExecutionContext& c) {
                 const std::filesystem::path file = pathFromUtf8(c.string("path"));
                 std::error_code ec;
                 if (!std::filesystem::is_regular_file(file, ec)) {
                     fail(ErrorCode::NotFound, "the presets file does not exist", Json{{"path", "/path"}});
                 }
                 if (std::filesystem::file_size(file, ec) > io::kMaxCaptionPresetsFileBytes) {
                     fail(ErrorCode::InvalidParams, "the presets file is larger than 4 MiB", Json{{"path", "/path"}});
                 }
                 std::ifstream in(file, std::ios::binary);
                 const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                 const std::vector<CaptionPreset> presets = c.take(io::captionPresetsFromJson(text));
                 std::vector<std::string> wanted;
                 if (c.has("names")) {
                     for (const Json& n : c.params()["names"]) {
                         wanted.push_back(n.get<std::string>());
                     }
                 }
                 const bool replace = c.boolean("replace", false);
                 ProjectState state = c.state();
                 Json imported = Json::array();
                 Json skipped = Json::array();
                 for (const CaptionPreset& p : presets) {
                     if (!wanted.empty() && std::none_of(wanted.begin(), wanted.end(), [&](const std::string& n) {
                             return sameCaptionPresetName(n, p.name);
                         })) {
                         continue;
                     }
                     if (isOriginalBuiltinCaptionPreset(p.name)) {
                         skipped.push_back(Json{{"name", p.name}, {"reason", "a built-in preset has this name"}});
                     } else if (!replace && state.findCaptionPreset(p.name) != nullptr) {
                         skipped.push_back(Json{{"name", p.name}, {"reason", "the project has a preset of this name"}});
                     } else {
                         state = c.take(edit::saveCaptionPreset(state, p));
                         imported.push_back(p.name);
                     }
                 }
                 if (imported.empty() && skipped.empty()) {
                     fail(ErrorCode::NotFound, wanted.empty() ? "the file holds no caption presets"
                                                              : "the file has none of the named presets", Json{{"path", "/names"}});
                 }
                 return projectEdited(std::move(state), Json{{"imported", std::move(imported)}, {"skipped", std::move(skipped)}});
             }},
            {"path"}));
    addBuiltin(registry,
               {"captions.presets", 1, "Caption Presets",
                "The caption presets: built in, then the project's, each with its style.", Capability::Read,
                withSequence({}), Json::object(), {},
                [](ExecutionContext& c) {
                    Json presets = Json::array();
                    for (const CaptionPreset& p : builtinCaptionPresets()) {
                        presets.push_back(Json{{"name", p.name}, {"builtIn", true}, {"style", captionStyleJson(p.style)}});
                    }
                    for (const CaptionPreset& p : c.state().captionPresets) {
                        presets.push_back(Json{{"name", p.name}, {"builtIn", false}, {"style", captionStyleJson(p.style)}});
                    }
                    CommandOutput out;
                    out.state = c.state();
                    out.data = Json{{"presets", std::move(presets)}};
                    return out;
                }});
    // 0.2: Premiere's Track Style (the Captions panel's Track Style menu and Push to Track Style).
    addBuiltin(registry,
               {"captions.setTrackStyle", 1, "Track Style",
                "Makes a caption track follow a caption preset (Premiere's Track Style menu): it takes the preset's style, and "
                "saving that preset again (captions.savePreset, captions.pushTrackStyle) restyles every track that follows "
                "it. \"style\": \"\" makes the track follow nothing (it keeps its look).",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"style", schema::string("Preset name (built in or the project's), or \"\" for none.")}},
                             {"style"}),
                Json{{"style", "Channel look"}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    return edited(c.take(edit::setCaptionTrackStyle(c.state(), c.sequenceId(), track.id, c.string("style"))));
                }});
    addBuiltin(registry,
               {"captions.pushTrackStyle", 1, "Push to Track Style",
                "Premiere's Push to Track Style: the caption track's current look becomes its track style (a project "
                "preset), and every caption track that follows that style, in every sequence, takes it. Refused for a "
                "built-in style (save it under a new name with captions.savePreset).",
                Capability::EditProject, withSequence({{"captionTrack", captionTrackParam()}}), Json::object(), {},
                [](ExecutionContext& c) {
                    const CaptionTrack& track = captionTrackOf(c);
                    return projectEdited(c.take(edit::pushCaptionTrackStyle(c.state(), c.sequenceId(), track.id)),
                                         Json{{"style", track.trackStyle}});
                }});

    // --- Custom words (0.7, card 061) ---------------------------------------------------------------------------
    addBuiltin(
        registry,
        {"transcript.setCustomWords", 1, "Custom Words",
         "Sets the project's custom words for transcription: names, game titles, jargon and invented terms the "
         "recogniser should spell right. They are given to the local transcriber as its vocabulary hint and to a cloud "
         "transcriber as key terms the next time audio is transcribed (transcript.transcribe; audio that already has a "
         "transcript needs \"force\" to be heard again). Give \"words\" (replaces the list; an empty list clears it) or "
         "\"text\" (one word or phrase per line, or separated by commas); \"add\" keeps the words already there and adds "
         "these. Words are trimmed, duplicates (any case) dropped; at most 200 of 80 bytes. One undo step.",
         Capability::EditProject,
         withSequence({{"words", schema::array(schema::string("A word or short phrase, e.g. \"Elden Ring\"."), "The words.")},
                       {"text", schema::string("Or the words as text: one per line, or separated by commas or semicolons.")},
                       {"add", schema::boolean("Add to the words already there instead of replacing them (default false).", false)}}),
         Json{{"words", Json::array({"Malenia", "Elden Ring"})}}, {},
         [](ExecutionContext& c) {
             std::vector<std::string> words;
             if (c.has("words")) {
                 for (const Json& w : c.params()["words"]) {
                     words.push_back(w.get<std::string>());
                 }
             }
             if (c.has("text")) {
                 for (std::string& w : edit::splitCustomWords(c.string("text"))) {
                     words.push_back(std::move(w));
                 }
             }
             if (!c.has("words") && !c.has("text")) {
                 fail(ErrorCode::InvalidParams, "give \"words\" or \"text\" (an empty list clears the words)");
             }
             if (c.boolean("add", false)) {
                 words.insert(words.begin(), c.state().customWords.begin(), c.state().customWords.end());
             }
             ProjectState next = c.take(edit::setCustomWords(c.state(), std::move(words)));
             Json list = next.customWords;
             return projectEdited(std::move(next), Json{{"words", std::move(list)}});
         }});
    addBuiltin(registry,
               {"transcript.customWords", 1, "List Custom Words",
                "The project's custom words for transcription (transcript.setCustomWords).", Capability::Read,
                withSequence({}), Json::object(), {},
                [](ExecutionContext& c) {
                    CommandOutput out;
                    out.state = c.state();
                    out.data = Json{{"words", c.state().customWords}};
                    return out;
                }});

    // --- Speakers ------------------------------------------------------------------------------------------
    addBuiltin(registry,
               {"transcript.addSpeaker", 1, "New Speaker", "Adds a speaker to the project. Returns its id.",
                Capability::EditProject,
                withSequence({{"name", nameParam("Display name.", kMaxSpeakerNameBytes)},
                              {"color", colorParam("Colour (default: the next of the palette).")}},
                             {"name"}),
                Json{{"name", "Host"}, {"color", "#4F8EF7"}}, {},
                [](ExecutionContext& c) {
                    std::optional<std::uint32_t> color;
                    if (c.has("color")) {
                        color = speakerColorValue(c.params()["color"], "/color");
                    }
                    auto created = c.take(edit::addSpeaker(c.state(), c.string("name"), color));
                    CommandOutput out = projectEdited(std::move(created.state), Json{{"speaker", created.id.value}});
                    out.createdIds.push_back(created.id.value);
                    return out;
                }});
    addBuiltin(
        registry,
        {"transcript.renameSpeaker", 1, "Rename Speaker",
         "Renames a speaker and/or changes its colour. Name it by \"speaker\" id, or by the diarization \"label\" of a "
         "media item's transcript (transcript.speakers lists both); a label without a speaker gets one.",
         Capability::EditProject,
         withSequence({{"speaker", schema::integer("Speaker id.", 1)},
                         {"media", mediaParam("Or the media item whose transcript has the label.")},
                         {"audioStream", schema::integer("Its audio stream (default 0).", 0, 64)},
                         {"label", schema::integer("The diarization label.", 0, 100000)},
                         {"name", nameParam("New name.", kMaxSpeakerNameBytes)},
                         {"color", colorParam("New colour.")}}),
         Json{{"speaker", 12}, {"name", "Dr. Somchai"}}, {},
         [](ExecutionContext& c) {
             ProjectState state = c.state();
             SpeakerId id;
             if (c.has("speaker")) {
                 id = speakerValue(c, c.params()["speaker"], "/speaker");
             } else if (c.has("media") && c.has("label")) {
                 const MediaId media = c.media("media");
                 const auto stream = static_cast<std::int32_t>(c.integer("audioStream", 0));
                 const auto label = static_cast<std::int32_t>(c.integer("label"));
                 const std::int32_t labels[] = {label};
                 state = c.take(edit::registerSpeakerLabels(state, media, stream, labels));
                 id = state.findTranscriptEdits(media, stream)->speakerOfLabel(label);
             } else {
                 fail(ErrorCode::InvalidParams, "name the speaker by \"speaker\", or by \"media\" and \"label\"");
             }
             std::optional<std::string> name;
             std::optional<std::uint32_t> color;
             if (c.has("name")) {
                 name = c.string("name");
             }
             if (c.has("color")) {
                 color = speakerColorValue(c.params()["color"], "/color");
             }
             return projectEdited(c.take(edit::renameSpeaker(state, id, name, color)), Json{{"speaker", id.value}});
         }});
    addBuiltin(registry,
               {"transcript.mergeSpeakers", 1, "Merge Speakers",
                "Makes speakers one: every word, assignment and caption of \"from\" (one id or several) becomes "
                "\"into\", and the merged speakers go.",
                Capability::EditProject,
                withSequence({{"from", Json{{"type", Json::array({"integer", "array"})},
                                             {"description", "One speaker id, or an array of them, to merge away."},
                                             {"items", schema::integer("Speaker id.", 1)},
                                             {"minimum", 1}}},
                                {"into", schema::integer("The speaker that stays.", 1)}},
                               {"from", "into"}),
                Json{{"from", Json::array({13, 14})}, {"into", 12}}, {},
                [](ExecutionContext& c) {
                    const SpeakerId into = speakerValue(c, c.params()["into"], "/into");
                    std::vector<SpeakerId> from;
                    const Json& f = c.params()["from"];
                    if (f.is_array()) {
                        for (std::size_t i = 0; i < f.size(); ++i) {
                            from.push_back(speakerValue(c, f[i], "/from/" + std::to_string(i)));
                        }
                    } else {
                        from.push_back(speakerValue(c, f, "/from"));
                    }
                    ProjectState state = c.state();
                    for (const SpeakerId id : from) {
                        state = c.take(edit::mergeSpeakers(state, id, into));
                    }
                    return projectEdited(std::move(state));
                }});
    addBuiltin(registry,
               {"transcript.assignSpeaker", 1, "Assign Speaker",
                "Says who speaks over a timeline range: the words the audio clips there play (on \"track\" only when "
                "given) belong to \"speaker\", or to a new speaker called \"name\". It wins over diarization and stays "
                "with the media when the clips are edited.",
                Capability::EditProject,
                withSequence({{"start", timeParam("Start of the range.")},
                              {"end", timeParam("End of the range.")},
                              {"speaker", schema::integer("Speaker id.", 1)},
                              {"name", nameParam("Or the name of a new speaker.", kMaxSpeakerNameBytes)},
                              {"track", trackParam("Only this audio track.")}},
                             {"start", "end"}),
                Json{{"start", "00:00:10:00"}, {"end", "00:00:14:00"}, {"speaker", 12}}, {},
                [](ExecutionContext& c) {
                    const Ticks start = c.time("start");
                    const Ticks end = c.time("end");
                    if (end <= start) {
                        fail(ErrorCode::InvalidParams, "\"end\" must come after \"start\"", Json{{"path", "/end"}});
                    }
                    ProjectState state = c.state();
                    SpeakerId speaker;
                    Json data = Json::object();
                    if (c.has("speaker") == c.has("name")) {
                        fail(ErrorCode::InvalidParams, "give \"speaker\" or \"name\"");
                    }
                    if (c.has("speaker")) {
                        speaker = speakerValue(c, c.params()["speaker"], "/speaker");
                    } else {
                        auto created = c.take(edit::addSpeaker(state, c.string("name")));
                        state = std::move(created.state);
                        speaker = created.id;
                    }
                    std::optional<TrackId> only;
                    if (c.has("track")) {
                        only = c.track("track");
                    }
                    const auto spans = sourceSpans(c, start, end, only);
                    if (spans.empty()) {
                        fail(ErrorCode::InvalidParams, "no audio clip plays in that range");
                    }
                    for (const SourceSpan& s : spans) {
                        state = c.take(edit::assignSpeaker(state, s.media, s.stream, s.source, speaker));
                    }
                    data["speaker"] = speaker.value;
                    data["clips"] = spans.size();
                    return projectEdited(std::move(state), std::move(data));
                }});
    // --- 0.2: Find and Replace (Find Next, Replace, Replace All), spell check -----------------------------------------
    addBuiltin(registry,
               {"captions.find", 1, "Find in Captions",
                "Premiere's Find (the arrows of Find and Replace): every match of \"find\" in the cues of a caption track "
                "(\"captionTrack\") or of every caption track of the sequence (default), in track order, then cue order, "
                "then position: the caption track, cue index, byte range in the cue's text, the cue's start, and the cue "
                "text. Plain text; \"matchCase\" and \"wholeWord\" as captions.findReplace. At most 1000 matches.",
                Capability::Read,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"find", findParam()},
                              {"matchCase", schema::boolean("Match Case. Default false.", false)},
                              {"wholeWord", schema::boolean("Whole Word. Default false.", false)}},
                             {"find"}),
                Json{{"find", "colour"}, {"wholeWord", true}}, {},
                [](ExecutionContext& c) {
                    const Sequence& seq = c.sequence();
                    if (seq.captionTracks.empty()) {
                        fail(ErrorCode::NotFound, "the sequence has no caption tracks");
                    }
                    const std::optional<CaptionTrackId> only =
                        c.has("captionTrack") ? std::optional(captionTrackOf(c).id) : std::nullopt;
                    const std::vector<CaptionMatch> matches = captionMatches(seq, only, c.string("find"), findOptions(c));
                    Json list = Json::array();
                    for (const CaptionMatch& m : matches) {
                        if (list.size() >= 1000) {
                            break;
                        }
                        list.push_back(captionMatchJson(c, seq, m));
                    }
                    return readOnly(c, Json{{"count", matches.size()}, {"matches", std::move(list)}});
                }});
    addBuiltin(registry,
               {"captions.findReplace", 1, "Find and Replace in Captions",
                "Premiere's Find and Replace for captions: replaces every match of \"find\" in the cues of a caption track "
                "(\"captionTrack\") or of every caption track of the sequence (default), in one undo step - or, with "
                "\"at\" (a match captions.find listed: its caption track, cue and start byte), only that match "
                "(Premiere's Replace); the result then names the next match after it (\"next\", or null). Plain text; "
                "\"matchCase\" and \"wholeWord\" as Premiere's options (without Match Case, Unicode simple case "
                "folding; whole word treats letters, digits and marks of every script as word characters). A dry run "
                "lists the matches without changing anything. A replacement that would leave a cue empty is refused.",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"find", findParam()},
                              {"replace", replaceParam()},
                              {"matchCase", schema::boolean("Match Case. Default false.", false)},
                              {"wholeWord", schema::boolean("Whole Word. Default false.", false)},
                              {"at", schema::object({{"captionTrack", schema::integer("The match's caption track id.", 1)},
                                                     {"cue", cueParam("The match's cue.")},
                                                     {"start", schema::integer("The match's first byte in the cue's text.", 0,
                                                                               static_cast<std::int64_t>(kMaxCaptionTextBytes))}},
                                                    {"captionTrack", "cue", "start"},
                                                    "Replace only this match (captions.find lists them).")}},
                             {"find", "replace"}),
                Json{{"find", "colour"}, {"replace", "color"}, {"wholeWord", true}}, {},
                [](ExecutionContext& c) {
                    const Sequence& seq = c.sequence();
                    if (seq.captionTracks.empty()) {
                        fail(ErrorCode::NotFound, "the sequence has no caption tracks");
                    }
                    const std::optional<CaptionTrackId> only =
                        c.has("captionTrack") ? std::optional(captionTrackOf(c).id) : std::nullopt;
                    const std::string find = c.string("find");
                    const std::string replacement = c.string("replace");
                    const TextFindOptions options = findOptions(c);
                    const auto checkText = [&](const std::string& text, std::size_t cue, const CaptionTrack& track) {
                        if (trimmed(text).empty()) {
                            fail(ErrorCode::EditRejected,
                                 "the replacement would leave cue " + std::to_string(cue) + " of caption track " +
                                     std::to_string(track.id.value) + " empty");
                        }
                        if (text.size() > kMaxCaptionTextBytes) {
                            fail(ErrorCode::EditRejected, "cue " + std::to_string(cue) + " would be longer than 4096 bytes");
                        }
                    };
                    if (c.has("at")) {
                        // Premiere's Replace: one match, then the next one after it.
                        const Json& at = c.params()["at"];
                        const CaptionTrackId trackId{at["captionTrack"].get<std::uint64_t>()};
                        const auto it = std::find_if(seq.captionTracks.begin(), seq.captionTracks.end(),
                                                     [&](const CaptionTrack& t) { return t.id == trackId; });
                        if (it == seq.captionTracks.end() || (only && *only != trackId)) {
                            fail(ErrorCode::NotFound, "no caption track " + std::to_string(trackId.value) + " to replace in",
                                 Json{{"path", "/at/captionTrack"}});
                        }
                        const auto cue = at["cue"].get<std::size_t>();
                        const auto start = at["start"].get<std::size_t>();
                        if (cue >= it->cues.size()) {
                            fail(ErrorCode::NotFound, "caption track " + std::to_string(trackId.value) + " has no cue " +
                                                          std::to_string(cue), Json{{"path", "/at/cue"}});
                        }
                        CaptionTrack track = *it;
                        const std::string before = track.cues[cue].text;
                        auto replaced = replaceTextAt(before, find, replacement, start, options);
                        if (!replaced) {
                            fail(ErrorCode::NotFound, "\"" + find + "\" is not at byte " + std::to_string(start) + " of cue " +
                                                          std::to_string(cue) + " (the text changed?)",
                                 Json{{"path", "/at/start"}});
                        }
                        checkText(*replaced, cue, track);
                        track.cues[cue].text = *replaced;
                        ProjectState state = c.take(edit::setCaptionTrack(c.state(), c.sequenceId(), track)).state;
                        // The next match: after the replacement in this cue, then on in scope order (wrapping round).
                        const Sequence* after = state.findSequence(c.sequenceId());
                        Json next = nullptr;
                        if (after) {
                            const std::vector<CaptionMatch> matches = captionMatches(*after, only, find, options);
                            const std::size_t resume = start + replacement.size();
                            const auto order = [&](const CaptionMatch& m) {
                                const auto t = std::find_if(after->captionTracks.begin(), after->captionTracks.end(),
                                                            [&](const CaptionTrack& x) { return x.id == m.track; });
                                return std::tuple(t - after->captionTracks.begin(), m.cue, m.start);
                            };
                            const auto here = std::tuple(it - seq.captionTracks.begin(), cue, resume);
                            const auto found = std::find_if(matches.begin(), matches.end(),
                                                            [&](const CaptionMatch& m) { return order(m) >= here; });
                            if (found != matches.end()) {
                                next = captionMatchJson(c, *after, *found);
                            } else if (!matches.empty()) {
                                next = captionMatchJson(c, *after, matches.front());
                            }
                        }
                        Json change{{"captionTrack", trackId.value}, {"cue", cue}, {"matches", 1}, {"before", before},
                                    {"after", *replaced}};
                        return projectEdited(std::move(state), Json{{"replaced", 1},
                                                                    {"cues", 1},
                                                                    {"changes", Json::array({std::move(change)})},
                                                                    {"next", std::move(next)}});
                    }
                    ProjectState state = c.state();
                    std::size_t total = 0;
                    std::size_t cuesChanged = 0;
                    Json matches = Json::array();
                    for (const CaptionTrack& original : seq.captionTracks) {
                        if (only && original.id != *only) {
                            continue;
                        }
                        CaptionTrack track = original;
                        bool changed = false;
                        for (std::size_t i = 0; i < track.cues.size(); ++i) {
                            std::size_t n = 0;
                            std::string text = replaceText(track.cues[i].text, find, replacement, options, &n);
                            if (n == 0) {
                                continue;
                            }
                            checkText(text, i, track);
                            total += n;
                            ++cuesChanged;
                            if (matches.size() < 200) {
                                matches.push_back(Json{{"captionTrack", track.id.value}, {"cue", i}, {"matches", n},
                                                       {"before", track.cues[i].text}, {"after", text}});
                            }
                            track.cues[i].text = std::move(text);
                            changed = true;
                        }
                        if (changed) {
                            state = c.take(edit::setCaptionTrack(state, c.sequenceId(), std::move(track))).state;
                        }
                    }
                    CommandOutput out = projectEdited(std::move(state), Json{{"replaced", total},
                                                                             {"cues", cuesChanged},
                                                                             {"changes", std::move(matches)}});
                    if (total == 0) {
                        out.warnings.push_back("\"" + find + "\" was not found");
                    }
                    return out;
                }});
    addBuiltin(registry,
               {"captions.checkSpelling", 1, "Check Spelling in Captions",
                "Premiere's spell check for captions: the misspelled words of a caption track's cues (\"captionTrack\", "
                "default the first) in its language (or \"language\"), from the host's spell checker and the user's "
                "dictionary: the cue, the word's byte range in the cue's text, the word, what the checker advises "
                "(\"suggest\", \"replace\" with the first suggestion, or \"delete\" a repeated word) and up to 8 "
                "suggestions. \"available\" is false when the host cannot check that language (nothing is listed). "
                "At most 2000 words. Corrections are captions.updateCue or captions.findReplace edits.",
                Capability::Read,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"language", schema::string("BCP 47 or ISO 639-1. Default: the track's, else the user's.")},
                              {"suggestions", schema::boolean("List suggestions. Default true.", true)}}),
                Json{{"captionTrack", 57}}, {},
                [](ExecutionContext& c) {
                    const auto* services = findService<SpellingServices>(c.services());
                    if (!services) {
                        fail(ErrorCode::InvalidRequest, "this host has no spell checker");
                    }
                    const CaptionTrack& track = captionTrackOf(c);
                    const std::string language = c.has("language") ? c.string("language") : track.language;
                    const SpellChecker& checker = services->checker();
                    const bool available = checker.supports(language);
                    Json issues = Json::array();
                    if (available) {
                        for (const TextSpellIssue& i :
                             spellingOf(track, checker, language, c.boolean("suggestions", true), 2000)) {
                            Json item = spellIssueJson(i.issue);
                            item["cue"] = i.cue;
                            issues.push_back(std::move(item));
                        }
                    }
                    return readOnly(c, Json{{"captionTrack", track.id.value},
                                              {"language", language},
                                              {"available", available},
                                              {"cues", track.cues.size()},
                                              {"issues", std::move(issues)}});
                }});
    {
        Json textSchema = schema::string("The text to check (UTF-8).");
        textSchema["maxLength"] = 64 * 1024;
        addBuiltin(registry,
                   {"spelling.check", 1, "Check Spelling",
                    "The misspelled words of a text in a language (default the user's), from the host's spell checker "
                    "and the user's dictionary, as captions.checkSpelling lists them (byte ranges of the text). "
                    "\"available\" is false when the host cannot check that language.",
                    Capability::Read,
                    withSequence({{"text", textSchema},
                                  {"language", schema::string("BCP 47 or ISO 639-1. Default: the user's.")},
                                  {"suggestions", schema::boolean("List suggestions. Default true.", true)}},
                                 {"text"}),
                    Json{{"text", "Teh quick brown fox"}, {"language", "en-US"}}, {},
                    [](ExecutionContext& c) {
                        const auto* services = findService<SpellingServices>(c.services());
                        if (!services) {
                            fail(ErrorCode::InvalidRequest, "this host has no spell checker");
                        }
                        const std::string language = c.has("language") ? c.string("language") : std::string();
                        const std::string text = c.string("text");
                        const SpellChecker& checker = services->checker();
                        const bool available = checker.supports(language);
                        Json issues = Json::array();
                        if (available) {
                            const bool suggestions = c.boolean("suggestions", true);
                            for (SpellIssue& issue : checker.check(text, language)) {
                                if (suggestions && issue.suggestions.empty() && issue.action != SpellAction::Delete) {
                                    issue.suggestions = checker.suggest(issue.word, language);
                                }
                                issues.push_back(spellIssueJson(issue));
                            }
                        }
                        return readOnly(c, Json{{"language", language}, {"available", available}, {"issues", std::move(issues)}});
                    }});
    }
}


// ---- captions.import (one command, CatalogInterchange.cpp): 0.2 graphics' reader of .srt / .vtt / timed .txt and
// the rules every imported cue follows -------------------------------------------------------------------------------

namespace {

/// Places `cues` (sorted here) as a new caption track: "offset", the overlap trimming, 4096-byte cues, at most
/// kMaxCaptionCues, "name" / "language" / "preset", speakers by name. `language` and `style` are the file's when the
/// parameters do not say; `extra` joins the result's data.
CommandOutput placeCaptionFileCues(ExecutionContext& c, const std::filesystem::path& file, std::vector<FileCue> cues,
                                   std::string language, std::optional<CaptionStyle> fileStyle, Json extra) {
    const Ticks offset = c.has("offset") ? c.time("offset") : Ticks{};
    std::stable_sort(cues.begin(), cues.end(), [](const FileCue& a, const FileCue& b) { return a.start < b.start; });
                    CaptionTrack track;
                    track.name = c.has("name") ? c.string("name") : clippedUtf8(pathToUtf8(file.stem()), kMaxCaptionNameBytes);
                    if (track.name.empty()) {
                        track.name = "Captions";
                    }
                    track.language = c.has("language") ? c.string("language") : language;
                    if (fileStyle) {
                        track.style = *fileStyle;
                    }
                    if (c.has("preset")) {
                        const auto style = edit::captionPresetStyle(c.state(), c.string("preset"));
                        if (!style) {
                            fail(ErrorCode::NotFound, "there is no caption preset \"" + c.string("preset") + "\"",
                                 Json{{"path", "/preset"}});
                        }
                        track.style = *style;
                    }
                    std::size_t dropped = 0;
                    std::size_t trimmedCues = 0;
                    std::size_t withSpeaker = 0;
                    for (std::size_t i = 0; i < cues.size(); ++i) {
                        FileCue& f = cues[i];
                        Ticks end = f.end;
                        if (i + 1 < cues.size() && cues[i + 1].start < end) {
                            end = cues[i + 1].start;
                            ++trimmedCues;
                        }
                        const std::string text = clippedUtf8(f.text, kMaxCaptionTextBytes);
                        if (end <= f.start || f.start + offset < Ticks{} || trimmed(text).empty() ||
                            track.cues.size() >= kMaxCaptionCues) {
                            ++dropped;
                            continue;
                        }
                        SpeakerId speaker;
                        if (!f.speaker.empty()) {
                            for (const Speaker& s : c.state().speakers) {
                                if (s.name == f.speaker) {
                                    speaker = s.id;
                                }
                            }
                            withSpeaker += speaker.isValid() ? 1 : 0;
                        }
                        track.cues.push_back(CaptionCue{f.start + offset, end - f.start, text, speaker});
                    }
                    if (track.cues.empty()) {
                        fail(ErrorCode::InvalidParams, "the file has no captions Wabcut can read", Json{{"path", "/path"}});
                    }
                    const std::size_t count = track.cues.size();
                    edit::EditResult r = c.take(edit::setCaptionTrack(c.state(), c.sequenceId(), std::move(track)));
                    const CaptionTrackId id = r.createdCaptionTrack;
                    CommandOutput out = edited(std::move(r), Json{{"captionTrack", id.value},
                                                                  {"cues", count},
                                                                  {"dropped", dropped},
                                                                  {"trimmed", trimmedCues},
                                                                  {"cuesWithSpeaker", withSpeaker}});
                    out.createdIds.push_back(id.value);
                    if (dropped > 0) {
                        out.warnings.push_back(counted(static_cast<std::int64_t>(dropped), "cue") +
                                               " not used (no text, no length or before the sequence start)");
                    }
                    for (auto& [key, value] : extra.items()) {
                        out.data[key] = value; // the format's own facts (interchange: format, report)
                    }
                    return out;
}

} // namespace

std::optional<CommandOutput> importCaptionFileCues(ExecutionContext& c) {
                    const std::filesystem::path file = pathFromUtf8(c.string("path"));
                    std::string ext = pathToUtf8(file.extension());
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    if (ext != ".srt" && ext != ".vtt" && ext != ".txt" && ext != ".docx") {
                        return std::nullopt;
                    }
                    const std::vector<std::string> lines = linesOf(readCaptionFile(file));
                    std::vector<FileCue> cues =
                        ext == ".txt" || ext == ".docx" ? parseTimedText(c, lines) : parseTimedBlocks(lines);
    if (cues.empty()) {
        return std::nullopt;
    }
    return placeCaptionFileCues(c, file, std::move(cues), {}, std::nullopt, Json::object());
}

CommandOutput placeImportedCaptions(ExecutionContext& c, const std::filesystem::path& file, const std::vector<CaptionCue>& cues,
                                    std::string language, std::optional<CaptionStyle> style, Json extra) {
    std::vector<FileCue> fileCues;
    fileCues.reserve(cues.size());
    for (const CaptionCue& cue : cues) {
        fileCues.push_back(FileCue{cue.start, cue.end(), cue.text, {}});
    }
    return placeCaptionFileCues(c, file, std::move(fileCues), std::move(language), std::move(style), std::move(extra));
}

} // namespace wabcut::command::detail
