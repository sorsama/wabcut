// Built-in caption workshop commands (C.7, model: CaptionOps.h): caption tracks and cues, styles and presets, and the
// speaker records of the project's transcripts. Generating captions from a transcript and correcting transcripts need
// the transcripts themselves: those commands are in wabcut_ai_edit (TranscriptCommands.h).

#include "Catalog.h"

#include <wabcut/model/CaptionOps.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

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
        {"placement", schema::enumeration({"bottom", "top", "custom"},
                                          "Bottom or top of the safe area, or at positionX/positionY.")},
        {"safeArea", num("Safe-area inset from each frame edge, fraction of the frame.", 0.0, 0.4)},
        {"positionX", num("Custom placement: centre of the text, fraction of the frame width.", 0.0, 1.0)},
        {"positionY", num("Custom placement: centre of the text, fraction of the frame height.", 0.0, 1.0)},
        {"alignment", schema::enumeration({"left", "center", "right"}, "Line alignment.")},
        {"maxWidth", num("Widest text block, fraction of the frame width.", 0.1, 1.0)},
        {"lineSpacing", num("Line spacing (1 = the font's).", 0.5, 3.0)},
        {"highlight", colorParam("Karaoke: colour of the words already spoken.")},
        {"animation", schema::enumeration({"none", "pop", "fade", "wordByWord"},
                                          "Kinetic captions (reserved: drawn as none today).")},
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
                {"placement", std::string(toString(s.placement))},
                {"safeArea", s.safeArea},
                {"positionX", positionJson(s.positionX)},
                {"positionY", positionJson(s.positionY)},
                {"alignment", std::string(toString(s.alignment))},
                {"maxWidth", s.maxWidth},
                {"lineSpacing", s.lineSpacing},
                {"highlight", hexColor(s.highlight)},
                {"animation", std::string(toString(s.animation))},
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

} // namespace

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
               {"captions.setTrack", 1, "Caption Track Settings", "Renames a caption track, sets its language or shows/hides it.",
                Capability::EditSequence,
                withSequence({{"captionTrack", captionTrackParam()},
                              {"name", nameParam("New name.", kMaxCaptionNameBytes)},
                              {"language", nameParam("ISO 639-1 language.", 16)},
                              {"enabled", schema::boolean("Shown (burned in and exported).")}}),
                Json{{"captionTrack", 57}, {"name", "Thai"}, {"language", "th"}}, {},
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
                "top (or the default look). Replaces the project preset of that name; built-in names are refused.",
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
                                             {"description", "Speaker id(s) to merge away."},
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
}

} // namespace wabcut::command::detail
