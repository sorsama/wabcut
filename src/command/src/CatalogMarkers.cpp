// Markers 2.0 (M5i): editing sequence markers (marker.update, marker.clearAll), clip markers on media items in media
// time (source.addMarker / updateMarker / removeMarker), and exporting a sequence's markers as CSV or as YouTube
// chapters (sequence.exportMarkers; text only, so read-only clients can use it: the app writes the file). 0.7: reading
// a marker list from a CSV, text or JSON file (marker.import, source.importMarkers; MarkerImport.cpp parses it).

#include "Catalog.h"
#include "MarkerImport.h"

#include <wabcut/command/CsvText.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/MarkerGroupOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::command::detail {

// --- Shared marker params (also used by marker.add) --------------------------------------------------------

Json markerTypeParam() {
    return schema::enumeration({"comment", "chapter", "segmentation", "webLink", "source"},
                               "Premiere's marker type (0.7: \"source\" marks a citation, see sources.add). Default comment.");
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

Ticks markerFrameTime(ExecutionContext& c, Ticks t) { return snapToFrame(t, c.sequence().frameRate); }

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
    case MarkerType::Source:
        return "Source";
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
        const Ticks moved = markerFrameTime(c, c.time("time"));
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

// --- Several markers at once (0.7, card 119) ---------------------------------------------------------------------

Json markerIndexesParam(std::string description) {
    Json indexes = schema::array(schema::integer("A marker index.", 0), std::move(description), 1);
    indexes["maxItems"] = static_cast<std::int64_t>(kMaxMarkers);
    return indexes;
}

/// The "indexes" param: marker indices into the sequence's list, each existing.
std::vector<std::size_t> markerIndexList(ExecutionContext& c, std::size_t count) {
    const Json& list = c.params()["indexes"];
    if (!list.is_array() || list.empty()) {
        fail(ErrorCode::InvalidParams, "name at least one marker index", Json{{"path", "/indexes"}});
    }
    std::vector<std::size_t> out;
    out.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::size_t index = c.indexValue(list[i], "/indexes/" + std::to_string(i));
        if (index >= count) {
            fail(ErrorCode::NotFound, "there is no marker " + std::to_string(index), Json{{"path", "/indexes/" + std::to_string(i)}});
        }
        out.push_back(index);
    }
    return out;
}

Json indexListJson(const std::vector<std::size_t>& touched) {
    Json out = Json::array();
    for (const std::size_t i : touched) {
        out.push_back(i);
    }
    return out;
}

CommandOutput groupEdited(ExecutionContext& c, std::expected<edit::MarkerGroupEdit, Error> group, Json extra = Json::object()) {
    if (!group) {
        fail(group.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::InvalidParams, group.error().message);
    }
    edit::EditResult r = c.take(edit::setSequenceMarkers(c.state(), c.sequenceId(), std::move(group->markers)));
    extra["indexes"] = indexListJson(group->touched);
    return edited(std::move(r), std::move(extra));
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

/// A marker's name or comment as a CSV cell. Marker text comes from clips, files and projects other people made: a cell
/// that starts with = + - @ (or a tab or carriage return) would run as a formula in a spreadsheet (CSV injection), so
/// it gets a leading quote (CsvText.h; csvTextUnguarded() takes it off again).
std::string csvField(const std::string& text) { return csvText(text); }

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

/// YouTube chapters: the chapter markers, or (unless `chaptersOnly`) every marker when there are none. P1 (#24): with a
/// `range` (the In/Out marks) only the markers that start inside it count, timed from its start, the last one running
/// to its end. YouTube needs the first chapter at 0:00, at least three chapters and each at least 10 s long; problems
/// are warnings, never silently fixed. Chapters only without any chapter marker: no text, the warning "no chapter
/// markers".
std::string youtubeChapters(const Sequence& seq, const std::optional<TimeRange>& range, bool chaptersOnly,
                            std::vector<std::string>& warnings, std::size_t& count) {
    const auto inside = [&](const Marker& m) { return !range || (m.start >= range->start && m.start < range->end()); };
    std::vector<const Marker*> chapters;
    for (const Marker& m : seq.markers) {
        if (m.type == MarkerType::Chapter && inside(m)) {
            chapters.push_back(&m);
        }
    }
    if (chapters.empty() && chaptersOnly) {
        count = 0;
        warnings.push_back("no chapter markers");
        return {};
    }
    if (chapters.empty()) {
        for (const Marker& m : seq.markers) {
            if (inside(m)) {
                chapters.push_back(&m);
            }
        }
    }
    const Ticks origin = range ? range->start : Ticks{};
    const Ticks end = range ? range->end() : seq.duration;
    const auto at = [&](const Marker& m) { return Ticks{m.start.value - origin.value}; };
    count = chapters.size();
    std::string out;
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        const Marker& m = *chapters[i];
        std::string title = !m.name.empty() ? firstLine(m.name) : firstLine(m.comment);
        if (title.empty()) {
            title = "Chapter " + std::to_string(i + 1);
        }
        out += youtubeTime(at(m)) + ' ' + title + '\n';
        const Ticks next = i + 1 < chapters.size() ? chapters[i + 1]->start : end;
        if (next.value - m.start.value < 10 * kTicksPerSecond) {
            warnings.push_back("chapter " + std::to_string(i + 1) + " (" + youtubeTime(at(m)) +
                               ") is shorter than 10 seconds");
        }
    }
    if (chapters.empty() || at(*chapters.front()).value >= kTicksPerSecond) {
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
    // P1 (#24): the In/Out range (times from its start) and chapter markers only.
    std::optional<TimeRange> range;
    if (c.string("range", "sequence") == "inOut") {
        const auto marked = seq.markedRange();
        if (!marked) {
            fail(ErrorCode::InvalidParams, "the sequence has no In and Out marks", Json{{"path", "/range"}});
        }
        range = *marked;
    }
    const bool chaptersOnly = c.boolean("chaptersOnly", false);
    std::vector<std::string> warnings;
    std::size_t count = 0;
    std::string text;
    if (format == "youtube") {
        text = youtubeChapters(seq, range, chaptersOnly, warnings, count);
    } else {
        std::vector<Marker> listed;
        for (const Marker& m : seq.markers) {
            if ((!range || (m.start >= range->start && m.start < range->end())) &&
                (!chaptersOnly || m.type == MarkerType::Chapter)) {
                listed.push_back(m);
            }
        }
        count = listed.size();
        text = markersCsv(c, listed);
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"format", format}, {"count", count}, {"text", text}, {"warnings", warnings}};
    if (range) {
        out.data["rangeStart"] = Json{{"ticks", range->start.value}, {"timecode", c.timecode(range->start)}};
    }
    return out;
}

// --- Import -------------------------------------------------------------------------------------------------------

Json markerPathParam(std::string description) {
    Json path = schema::string(std::move(description));
    path["maxLength"] = 4096;
    path["minLength"] = 1;
    return path;
}

/// The marker file's bytes (at most kMaxMarkerImportBytes).
std::string readMarkerFileBytes(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        fail(ErrorCode::NotFound, "cannot read \"" + pathToUtf8(path) + "\"", Json{{"path", "/path"}});
    }
    if (size > kMaxMarkerImportBytes) {
        fail(ErrorCode::InvalidParams, "the marker file is larger than 16 MiB", Json{{"path", "/path"}});
    }
    std::ifstream in(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    return bytes;
}

/// What an import found, ready to join the markers already there.
struct MarkerImport {
    std::vector<Marker> markers;
    std::string format;
    std::size_t skipped = 0;
    std::vector<std::string> warnings;
};

/// Reads the file named by "path": its markers shifted by "offset" and put on the frame grid of `rate`. A marker that
/// would start before 0 or breaks a marker limit is skipped and reported.
MarkerImport readMarkerImport(ExecutionContext& c, Rate rate, bool dropFrame, Ticks offset) {
    const std::filesystem::path file = pathFromUtf8(c.string("path"));
    auto parsed = parseMarkerList(markerFileText(readMarkerFileBytes(file)), c.string("format", "auto"), rate, dropFrame);
    if (!parsed) {
        fail(ErrorCode::InvalidParams, parsed.error(), Json{{"path", "/path"}});
    }
    MarkerImport out;
    out.format = parsed->format;
    out.skipped = parsed->skipped;
    out.warnings = std::move(parsed->warnings);
    const auto note = [&](std::string message) {
        if (out.warnings.size() < kMaxMarkerImportWarnings) {
            out.warnings.push_back(std::move(message));
        }
    };
    for (ParsedMarker& p : parsed->markers) {
        Marker m = std::move(p.marker);
        const std::int64_t shifted = m.start.value + offset.value;
        if (shifted < 0) {
            ++out.skipped;
            note("entry " + std::to_string(p.where) + ": starts before 0 after the offset");
            continue;
        }
        m.start = snapToFrame(Ticks{shifted}, rate);
        if (const auto problem = markerProblem(m)) {
            ++out.skipped;
            note("entry " + std::to_string(p.where) + ": " + *problem);
            continue;
        }
        out.markers.push_back(std::move(m));
    }
    return out;
}

/// `imported` joined to `markers` (replace: instead of them). An imported marker equal to one already there (the same
/// start, length, name and comment) is left out, so reading one file twice adds nothing. Returns how many were added;
/// `duplicates` counts those left out.
std::size_t joinMarkers(std::vector<Marker>& markers, bool replace, std::vector<Marker> imported, std::size_t& duplicates) {
    if (replace) {
        markers.clear();
    }
    const std::size_t existing = markers.size();
    std::size_t added = 0;
    for (Marker& m : imported) {
        const bool same = std::any_of(markers.begin(), markers.begin() + static_cast<std::ptrdiff_t>(existing), [&](const Marker& e) {
            return e.start == m.start && e.duration == m.duration && e.name == m.name && e.comment == m.comment;
        });
        if (same) {
            ++duplicates;
            continue;
        }
        markers.push_back(std::move(m));
        ++added;
    }
    if (markers.size() > kMaxMarkers) {
        fail(ErrorCode::InvalidParams,
             "the markers would be more than " + std::to_string(kMaxMarkers) + " (the limit of a sequence or a clip)");
    }
    return added;
}

Json importReport(const MarkerImport& in, std::size_t added, std::size_t duplicates, bool replaced) {
    return Json{{"format", in.format},
                {"imported", added},
                {"skipped", in.skipped},
                {"duplicates", duplicates},
                {"replaced", replaced},
                {"warnings", in.warnings}};
}

CommandOutput importSequenceMarkers(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const MarkerImport in = readMarkerImport(c, seq.frameRate, seq.dropFrame, c.has("offset") ? c.time("offset") : Ticks{});
    const bool replace = c.boolean("replace", false);
    std::vector<Marker> markers = seq.markers;
    std::size_t duplicates = 0;
    const std::size_t added = joinMarkers(markers, replace, in.markers, duplicates);
    return edited(c.take(edit::setSequenceMarkers(c.state(), seq.id, std::move(markers))),
                  importReport(in, added, duplicates, replace));
}

CommandOutput importClipMarkers(ExecutionContext& c) {
    const MediaItem& media = markerMedia(c);
    const Rate rate = sourceTimecodeRate(c.state(), media);
    const MarkerImport in = readMarkerImport(c, rate, false, sourceTime(c, "offset", rate, Ticks{}));
    const bool replace = c.boolean("replace", false);
    std::vector<Marker> markers = media.markers;
    std::size_t duplicates = 0;
    const std::size_t added = joinMarkers(markers, replace, in.markers, duplicates);
    return edited(c.take(edit::setMediaMarkers(c.state(), media.id, std::move(markers))),
                  importReport(in, added, duplicates, replace));
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
               {"marker.removeMany", 1, "Remove Markers",
                "Removes several sequence markers in one step (0.7): the markers at the given indices (as marker.list "
                "numbers them) go; the others stay. One undo step.",
                Capability::EditSequence,
                withSequence({{"indexes", markerIndexesParam("Marker indices to remove.")}}, {"indexes"}),
                Json{{"indexes", Json::array({0, 2, 3})}}, {"markers.removeMany"},
                [](ExecutionContext& c) {
                    const std::vector<std::size_t> indexes = markerIndexList(c, c.sequence().markers.size());
                    edit::EditResult r = c.take(edit::setSequenceMarkers(
                        c.state(), c.sequenceId(), [&] {
                            auto group = edit::removeMarkersAt(c.sequence().markers, indexes);
                            if (!group) {
                                fail(ErrorCode::InvalidParams, group.error().message);
                            }
                            return std::move(group->markers);
                        }()));
                    return edited(std::move(r), Json{{"removed", indexes.size()}});
                }});
    addBuiltin(registry,
               {"marker.moveMany", 1, "Move Markers",
                "Moves several sequence markers by the same amount in one step (0.7): `by` is added to the start of each "
                "marker at the given indices (negative = earlier; their lengths stay; a highlight's peak moves with its "
                "marker). The move is cut short so the earliest of them does not start before 0; the result says how far "
                "they went. The list stays sorted by start, so the result lists the markers' new indices. One undo step.",
                Capability::EditSequence,
                withSequence({{"indexes", markerIndexesParam("Marker indices to move.")},
                              {"by", deltaParam("How far; positive = later. It lands on the frame grid.")}},
                             {"indexes", "by"}),
                Json{{"indexes", Json::array({1, 2})}, {"by", "00:00:01:00"}}, {"markers.moveMany"},
                [](ExecutionContext& c) {
                    const std::vector<std::size_t> indexes = markerIndexList(c, c.sequence().markers.size());
                    const Ticks by = snapToFrame(c.time("by"), c.sequence().frameRate);
                    auto group = edit::moveMarkersBy(c.sequence().markers, indexes, by);
                    const Ticks applied = group ? group->appliedDelta : Ticks{};
                    return groupEdited(c, std::move(group), Json{{"moved", indexes.size()}, {"applied", timeJson(c, applied)}});
                }});
    addBuiltin(registry,
               {"marker.duplicate", 1, "Duplicate Markers",
                "Copies several sequence markers in one step (0.7): each marker at the given indices gets a copy `offset` "
                "later (or earlier when negative) with the same name, comment, colour, type, link and length. The copies' "
                "new indices are in the result. One undo step.",
                Capability::EditSequence,
                withSequence({{"indexes", markerIndexesParam("Marker indices to copy.")},
                              {"offset", deltaParam("How far from the originals (not 0). It lands on the frame grid.")}},
                             {"indexes", "offset"}),
                Json{{"indexes", Json::array({0, 1})}, {"offset", "00:00:10:00"}}, {"markers.duplicate"},
                [](ExecutionContext& c) {
                    const std::vector<std::size_t> indexes = markerIndexList(c, c.sequence().markers.size());
                    const Ticks offset = snapToFrame(c.time("offset"), c.sequence().frameRate);
                    return groupEdited(c, edit::duplicateMarkersBy(c.sequence().markers, indexes, offset),
                                       Json{{"copied", indexes.size()}});
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
               touchesFiles(
                   {"marker.import", 1, "Import Markers",
                    "Reads a marker list into the sequence's markers: a CSV or tab-separated table with a header row "
                    "(Premiere's marker export, a spreadsheet: Marker Name / Name, Description / Comment, In / Start / "
                    "Time, Out, Duration, Marker Type, Color), a text file with one marker a line (YouTube chapters "
                    "\"0:00 Intro\", \"00:01:23:10 Name\", Audacity labels) or JSON (an array, or an object with a "
                    "\"markers\" array; marker.list's output reads back). Times: HH:MM:SS:FF at the sequence's frame "
                    "rate, H:MM:SS.fff or M:SS clocks, seconds (\"12.5\", \"12.5s\"), \"1500ms\"; they land on the frame "
                    "grid. \"offset\" is added to every time (a file that counts from 0 to start at the playhead). The "
                    "markers join the ones already there, leaving out exact copies (reading a file twice adds nothing); "
                    "\"replace\" clears the existing markers first. A line that cannot be read is skipped and listed in "
                    "the result's warnings. Untrusted input: at most 16 MiB and the 10,000 markers a sequence holds.",
                    Capability::EditSequence,
                    withSequence({{"path", markerPathParam("The marker file (UTF-8 path).")},
                                  {"format", schema::enumeration({"auto", "csv", "txt", "json"},
                                                                 "The file's shape. Default auto: detected from the content.")},
                                  {"offset", timeParam("Added to every marker's time. Default 0.")},
                                  {"replace", schema::boolean("Remove the sequence's markers first. Default false.")}},
                                 {"path"}),
                    Json{{"path", "C:/Delivery/chapters.csv"}}, {"markers.import"},
                    [](ExecutionContext& c) { return importSequenceMarkers(c); }},
                   {"path"}));
    addBuiltin(registry,
               touchesFiles(
                   {"source.importMarkers", 1, "Import Clip Markers",
                    "Reads a marker list (see marker.import: CSV, text lines or JSON) into a media item's clip markers, in "
                    "media time (\"HH:MM:SS:FF\" at the media's own frame rate). Every clip of the media shows them. "
                    "\"offset\" is added to every time; \"replace\" clears the media's markers first; exact copies of "
                    "markers already there are left out.",
                    Capability::EditProject,
                    withSequence({{"media", mediaParam("The media.")},
                                  {"path", markerPathParam("The marker file (UTF-8 path).")},
                                  {"format", schema::enumeration({"auto", "csv", "txt", "json"},
                                                                 "The file's shape. Default auto: detected from the content.")},
                                  {"offset", sourceTimeParam("Added to every marker's time, in media time. Default 0.")},
                                  {"replace", schema::boolean("Remove the media's markers first. Default false.")}},
                                 {"media", "path"}),
                    Json{{"media", 3}, {"path", "C:/Delivery/takes.csv"}}, {},
                    [](ExecutionContext& c) { return importClipMarkers(c); }},
                   {"path"}));
    addBuiltin(registry,
               {"sequence.exportMarkers", 1, "Export Markers",
                "Returns the sequence's markers as text: \"csv\" (Premiere's columns: Marker Name, Description, In, Out, "
                "Duration, Marker Type, plus Color) or \"youtube\" (chapter lines for a video description, from the "
                "chapter markers, or every marker when there are none). YouTube's rules (the first chapter at 0:00, at "
                "least 3 chapters, each at least 10 s) are reported as warnings, never fixed silently. \"range\": "
                "\"inOut\" keeps the markers that start between the sequence's In and Out marks, YouTube times counted "
                "from the In mark. \"chaptersOnly\": chapter markers only (no text and the warning \"no chapter "
                "markers\" when there are none). Nothing is written to disk.",
                Capability::Read,
                withSequence({{"format", schema::enumeration({"csv", "youtube"}, "The text format. Default csv.")},
                              {"range", schema::enumeration({"sequence", "inOut"},
                                                            "The whole sequence (default) or its In/Out marks, "
                                                            "YouTube times counted from the In mark.")},
                              {"chaptersOnly", schema::boolean("Chapter markers only, never every marker. Default "
                                                               "false.")}}),
                Json{{"format", "youtube"}}, {},
                [](ExecutionContext& c) { return exportMarkers(c); }});
}

} // namespace wabcut::command::detail
