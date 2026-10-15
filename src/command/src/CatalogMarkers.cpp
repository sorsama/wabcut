// Markers 2.0 (M5i): editing sequence markers (marker.update, marker.clearAll), clip markers on media items in media
// time (source.addMarker / updateMarker / removeMarker), and exporting a sequence's markers as CSV or as YouTube
// chapters (sequence.exportMarkers; text only, so read-only clients can use it: the app writes the file).

#include "Catalog.h"

#include <wabcut/command/SourceServices.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Timecode.h>

#include <string>
#include <vector>

namespace wabcut::command::detail {

// --- Shared marker params (also used by marker.add) --------------------------------------------------------

Json markerTypeParam() {
    return schema::enumeration({"comment", "chapter", "segmentation", "webLink"},
                               "Premiere's marker type. Default comment.");
}

Json markerUrlParam() { return schema::string("Web link URL (http or https)."); }

Json markerTargetParam() { return schema::string("Web link frame target."); }

Json markerHighlightParam() {
    Json s = schema::object(
        {{"score", schema::number("How strong the moment is, 0..1.", 0.0, 1.0)},
         {"confidence", schema::enumeration({"low", "medium", "high"}, "How sure the finder is. Default medium.")},
         {"reasons", schema::array(schema::string("Why (1..200 bytes): \"kill streak\", \"mic peak\"."),
                                   "Up to 8 reasons.")},
         {"peak", timeParam("The best moment, inside the marker. Default: the marker's start.")},
         {"source", schema::string("What found it (1..64 bytes). Default \"highlights\".")}},
        {"score"},
        "C.15: a highlight finder's verdict on the marked moment (the Highlights panel reads it). null removes it.");
    s["type"] = Json::array({"object", "null"});
    return s;
}

Json highlightJson(ExecutionContext& c, const MarkerHighlight& h) {
    return Json{{"score", h.score},
                {"confidence", std::string(toString(h.confidence))},
                {"reasons", h.reasons},
                {"peak", c.timecode(h.peak)},
                {"peakTicks", h.peak.value},
                {"source", h.source}};
}

void applyMarkerLinkParams(ExecutionContext& c, Marker& marker) {
    if (c.has("type")) {
        marker.type = markerTypeFromName(c.string("type")).value_or(MarkerType::Comment);
    }
    if (c.has("url")) {
        marker.url = c.string("url");
    }
    if (c.has("target")) {
        marker.target = c.string("target");
    }
    if (c.params().is_object() && c.params().contains("highlight")) { // C.15
        const Json& h = c.params()["highlight"];
        if (h.is_null()) {
            marker.highlight.reset();
            return;
        }
        MarkerHighlight out = marker.highlight.value_or(MarkerHighlight{});
        if (!marker.highlight) {
            out.peak = marker.start;
        }
        if (h.contains("score") && h["score"].is_number()) {
            out.score = h["score"].get<double>();
        }
        if (h.contains("confidence") && h["confidence"].is_string()) {
            const auto confidence = highlightConfidenceFromString(h["confidence"].get<std::string>());
            if (!confidence) {
                fail(ErrorCode::InvalidParams, "highlight.confidence must be low, medium or high");
            }
            out.confidence = *confidence;
        }
        if (h.contains("reasons") && h["reasons"].is_array()) {
            out.reasons.clear();
            for (const Json& r : h["reasons"]) {
                if (!r.is_string()) {
                    fail(ErrorCode::InvalidParams, "highlight.reasons must be strings");
                }
                out.reasons.push_back(r.get<std::string>());
            }
        }
        if (h.contains("peak")) {
            out.peak = c.timeValue(h["peak"], "/highlight/peak");
        }
        if (h.contains("source") && h["source"].is_string()) {
            out.source = h["source"].get<std::string>();
        }
        marker.highlight = std::move(out);
    }
}

void requireValidMarker(const Marker& marker) {
    if (const auto problem = markerProblem(marker)) {
        fail(ErrorCode::InvalidParams, *problem);
    }
}

namespace {

/// Premiere's eight marker colours, colorIndex 0-7.
constexpr const char* kMarkerColorNames[] = {"Green", "Red", "Purple", "Orange", "Yellow", "White", "Blue", "Cyan"};

std::string colorName(std::int32_t index) {
    return index >= 0 && index < 8 ? kMarkerColorNames[index] : "Label " + std::to_string(index);
}

std::string typeLabel(MarkerType type) {
    switch (type) {
    case MarkerType::Chapter:
        return "Chapter";
    case MarkerType::Segmentation:
        return "Segmentation";
    case MarkerType::WebLink:
        return "Web Link";
    case MarkerType::Comment:
        break;
    }
    return "Comment";
}

/// The marker fields every add/update command takes (times are read by the caller).
Json withMarkerFields(std::initializer_list<std::pair<const char*, Json>> leading, Json timeSchema,
                      Json durationSchema, std::vector<std::string> required) {
    Json schema = withSequence(leading, std::move(required));
    Json& props = schema["properties"];
    props["time"] = std::move(timeSchema);
    props["duration"] = std::move(durationSchema);
    props["name"] = schema::string("Marker name.");
    props["comment"] = schema::string("Marker comment.");
    props["colorIndex"] = schema::integer("Marker colour index (Premiere's colours are 0-7).", 0, 15);
    props["type"] = markerTypeParam();
    props["url"] = markerUrlParam();
    props["target"] = markerTargetParam();
    props["highlight"] = markerHighlightParam();
    return schema;
}

/// Applies the given name, comment, colour, type, url and target params to `marker`.
void applyFields(ExecutionContext& c, Marker& marker) {
    if (c.has("name")) {
        marker.name = c.string("name");
    }
    if (c.has("comment")) {
        marker.comment = c.string("comment");
    }
    if (c.has("colorIndex")) {
        marker.colorIndex = static_cast<std::int32_t>(c.integer("colorIndex"));
    }
    applyMarkerLinkParams(c, marker);
}

Json markerJson(ExecutionContext& c, const Marker& m, std::size_t index) {
    return Json{{"index", index},
                {"start", timeJson(c, m.start)},
                {"duration", timeJson(c, m.duration)},
                {"name", m.name},
                {"comment", m.comment},
                {"colorIndex", m.colorIndex},
                {"type", std::string(markerTypeName(m.type))},
                {"url", m.url},
                {"target", m.target},
                {"highlight", m.highlight ? highlightJson(c, *m.highlight) : Json(nullptr)}};
}

std::size_t markerIndex(ExecutionContext& c, std::size_t count) {
    const std::size_t index = c.indexValue(c.params()["index"], "/index");
    if (index >= count) {
        fail(ErrorCode::NotFound, "there is no marker " + std::to_string(index), Json{{"path", "/index"}});
    }
    return index;
}

CommandOutput updateMarker(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const std::size_t index = markerIndex(c, seq.markers.size());
    Marker marker = seq.markers[index];
    if (c.has("time")) {
        const Ticks moved = c.time("time");
        if (marker.highlight) {
            marker.highlight->peak = marker.highlight->peak + (moved - marker.start); // the peak moves with its marker
        }
        marker.start = moved;
    }
    if (c.has("duration")) {
        marker.duration = c.time("duration");
    }
    applyFields(c, marker);
    requireValidMarker(marker);
    edit::EditResult r = c.take(edit::updateMarker(c.state(), seq.id, index, marker));
    return edited(std::move(r), Json{{"marker", markerJson(c, marker, index)}});
}

// --- Clip markers (media time) -----------------------------------------------------------------------------

const MediaItem& markerMedia(ExecutionContext& c) { return *c.state().findMedia(c.media("media")); }

Ticks sourceTime(ExecutionContext& c, const char* key, Rate rate, Ticks fallback) {
    if (!c.has(key)) {
        return fallback;
    }
    return sourceTimeValue(c.params()[key], std::string("/") + key, rate).value_or(fallback);
}

CommandOutput setClipMarker(ExecutionContext& c, bool add) {
    const MediaItem& media = markerMedia(c);
    const Rate rate = sourceTimecodeRate(c.state(), media);
    std::vector<Marker> markers = media.markers;
    Marker marker;
    std::size_t index = markers.size();
    if (add) {
        if (!c.has("time")) {
            fail(ErrorCode::InvalidParams, "pass \"time\"", Json{{"path", "/time"}});
        }
        if (markers.size() >= kMaxMarkers) {
            fail(ErrorCode::InvalidParams, "'" + media.name + "' already has " + std::to_string(kMaxMarkers) + " markers");
        }
    } else {
        index = markerIndex(c, markers.size());
        marker = markers[index];
        markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(index));
    }
    marker.start = sourceTime(c, "time", rate, marker.start);
    marker.duration = sourceTime(c, "duration", rate, marker.duration);
    applyFields(c, marker);
    requireValidMarker(marker);
    markers.push_back(marker);
    edit::EditResult r = c.take(edit::setMediaMarkers(c.state(), media.id, std::move(markers)));
    return edited(std::move(r), Json{{"media", media.id.value},
                                     {"start", Json{{"ticks", marker.start.value},
                                                    {"timecode", ticksToTimecode(marker.start, rate, false)}}}});
}

CommandOutput removeClipMarker(ExecutionContext& c) {
    const MediaItem& media = markerMedia(c);
    std::vector<Marker> markers = media.markers;
    const std::size_t index = markerIndex(c, markers.size());
    markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(index));
    return edited(c.take(edit::setMediaMarkers(c.state(), media.id, std::move(markers))),
                  Json{{"media", media.id.value}});
}

// --- Export -------------------------------------------------------------------------------------------------

std::string csvField(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (const char ch : text) {
        out += ch;
        if (ch == '"') {
            out += '"';
        }
    }
    return out + "\"";
}

/// Premiere's marker CSV columns, plus the colour.
std::string markersCsv(ExecutionContext& c, const std::vector<Marker>& markers) {
    std::string out = "Marker Name,Description,In,Out,Duration,Marker Type,Color\n";
    for (const Marker& m : markers) {
        out += csvField(m.name) + ',' + csvField(m.comment) + ',' + c.timecode(m.start) + ',' + c.timecode(m.end()) +
               ',' + c.timecode(m.duration) + ',' + typeLabel(m.type) + ',' + colorName(m.colorIndex) + '\n';
    }
    return out;
}

/// YouTube's chapter timestamps: M:SS, or H:MM:SS from an hour.
std::string youtubeTime(Ticks t) {
    const std::int64_t total = t.value / kTicksPerSecond;
    const std::int64_t h = total / 3600;
    const std::int64_t m = (total / 60) % 60;
    const std::int64_t s = total % 60;
    const auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    return h > 0 ? std::to_string(h) + ":" + two(m) + ":" + two(s) : std::to_string(m) + ":" + two(s);
}

std::string firstLine(const std::string& text) { return text.substr(0, text.find('\n')); }

/// YouTube chapters: the chapter markers, or every marker when there are none. YouTube needs the first chapter at
/// 0:00, at least three chapters and each at least 10 s long; problems are warnings, never silently fixed.
std::string youtubeChapters(const Sequence& seq, std::vector<std::string>& warnings, std::size_t& count) {
    std::vector<const Marker*> chapters;
    for (const Marker& m : seq.markers) {
        if (m.type == MarkerType::Chapter) {
            chapters.push_back(&m);
        }
    }
    if (chapters.empty()) {
        for (const Marker& m : seq.markers) {
            chapters.push_back(&m);
        }
    }
    count = chapters.size();
    std::string out;
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        const Marker& m = *chapters[i];
        std::string title = !m.name.empty() ? firstLine(m.name) : firstLine(m.comment);
        if (title.empty()) {
            title = "Chapter " + std::to_string(i + 1);
        }
        out += youtubeTime(m.start) + ' ' + title + '\n';
        const Ticks next = i + 1 < chapters.size() ? chapters[i + 1]->start : seq.duration;
        if (next.value - m.start.value < 10 * kTicksPerSecond) {
            warnings.push_back("chapter " + std::to_string(i + 1) + " (" + youtubeTime(m.start) +
                               ") is shorter than 10 seconds");
        }
    }
    if (chapters.empty() || chapters.front()->start.value >= kTicksPerSecond) {
        warnings.push_back("the first chapter must start at 0:00");
    }
    if (chapters.size() < 3) {
        warnings.push_back("YouTube needs at least 3 chapters");
    }
    return out;
}

CommandOutput exportMarkers(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const std::string format = c.string("format", "csv");
    std::vector<std::string> warnings;
    std::size_t count = seq.markers.size();
    const std::string text = format == "youtube" ? youtubeChapters(seq, warnings, count) : markersCsv(c, seq.markers);
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"format", format}, {"count", count}, {"text", text}, {"warnings", warnings}};
    return out;
}

} // namespace

void registerMarkerCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"marker.update", 1, "Edit Marker",
                "Changes the sequence marker at an index (as listed by sequence.summary): any of its time, duration, "
                "name, comment, colour, type, URL and frame target. Keys left out keep their values; the markers stay "
                "sorted by start.",
                Capability::EditSequence,
                withMarkerFields({{"index", schema::integer("Marker index.", 0)}},
                                 timeParam("New start."), timeParam("New length (0 = a point marker)."), {"index"}),
                Json{{"index", 0}, {"name", "Interview"}, {"colorIndex", 1}}, {"markers.update"},
                [](ExecutionContext& c) { return updateMarker(c); }});
    addBuiltin(registry,
               {"marker.clearAll", 1, "Clear All Markers", "Removes every marker of the sequence (Premiere's Clear All "
                "Markers).",
                Capability::EditSequence, withSequence({}), Json::object(), {"markers.clearAll"},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::setSequenceMarkers(c.state(), c.sequenceId(), {})));
                }});
    addBuiltin(registry,
               {"marker.list", 1, "List Markers",
                "The sequence's markers in time order with all their fields: index (what marker.update and marker.remove "
                "take), start, duration, name, comment, colour, type, web link, and the highlight a highlight finder "
                "wrote (score, confidence, reasons, peak, source; null when none).",
                Capability::Read, withSequence({}), Json::object(), {},
                [](ExecutionContext& c) {
                    const Sequence& seq = c.sequence();
                    Json list = Json::array();
                    for (std::size_t i = 0; i < seq.markers.size(); ++i) {
                        list.push_back(markerJson(c, seq.markers[i], i));
                    }
                    CommandOutput out;
                    out.state = c.state();
                    out.data = Json{{"markers", std::move(list)}};
                    return out;
                }});
    addBuiltin(registry,
               {"source.addMarker", 1, "Add Clip Marker",
                "Adds a clip marker to a media item, in media time. Every clip of the media shows it where it uses that "
                "part of the source (Premiere's master clip markers).",
                Capability::EditProject,
                withMarkerFields({{"media", mediaParam("The media.")}},
                                 sourceTimeParam("Where the marker starts, in media time."),
                                 sourceTimeParam("Length for a range marker. Default 0."), {"media", "time"}),
                Json{{"media", 3}, {"time", "00:00:02:00"}, {"name", "Good take"}}, {},
                [](ExecutionContext& c) { return setClipMarker(c, true); }});
    addBuiltin(registry,
               {"source.updateMarker", 1, "Edit Clip Marker",
                "Changes a media item's clip marker at an index (keys left out keep their values).",
                Capability::EditProject,
                withMarkerFields({{"media", mediaParam("The media.")}, {"index", schema::integer("Marker index.", 0)}},
                                 sourceTimeParam("New start, in media time."), sourceTimeParam("New length."),
                                 {"media", "index"}),
                Json{{"media", 3}, {"index", 0}, {"colorIndex", 2}}, {},
                [](ExecutionContext& c) { return setClipMarker(c, false); }});
    addBuiltin(registry,
               {"source.removeMarker", 1, "Remove Clip Marker", "Removes a media item's clip marker at an index.",
                Capability::EditProject,
                withSequence({{"media", mediaParam("The media.")}, {"index", schema::integer("Marker index.", 0)}},
                             {"media", "index"}),
                Json{{"media", 3}, {"index", 0}}, {}, [](ExecutionContext& c) { return removeClipMarker(c); }});
    addBuiltin(registry,
               {"sequence.exportMarkers", 1, "Export Markers",
                "Returns the sequence's markers as text: \"csv\" (Premiere's columns: Marker Name, Description, In, Out, "
                "Duration, Marker Type, plus Color) or \"youtube\" (chapter lines for a video description, from the "
                "chapter markers, or every marker when there are none). YouTube's rules (the first chapter at 0:00, at "
                "least 3 chapters, each at least 10 s) are reported as warnings, never fixed silently. Nothing is "
                "written to disk.",
                Capability::Read,
                withSequence({{"format", schema::enumeration({"csv", "youtube"}, "The text format. Default csv.")}}),
                Json{{"format", "youtube"}}, {},
                [](ExecutionContext& c) { return exportMarkers(c); }});
}

} // namespace wabcut::command::detail
