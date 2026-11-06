// Sources and citations for video essays (0.7, card 177): sources.add / update / remove / list / export and
// sources.lowerThirds. A source is a sequence marker of type Source (Sequence.h MarkerCitation): the marker's name is the
// claim, its url the source's address, and the reference (title, author, publisher, date, archive link, screenshot) rides
// on the marker, so a source moves, ripples, undoes, merges and exports with the markers it is. A claim marked "needed" is
// an uncited claim waiting for its source; the Sources panel lists those first and the exports name them.
//
// sources.export is text only (read-only clients can use it); the app writes the file and makes the PDF from the same text.

#include "Catalog.h"

#include <wabcut/command/SourceList.h>
#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cmath>

namespace wabcut::command::detail {

namespace {

constexpr std::int32_t kCitedColor = 6;  // blue
constexpr std::int32_t kNeededColor = 1; // red

Json citationFieldSchema(const char* what) { return schema::string(what); }

Json sourceFields() {
    Json props = Json::object();
    props["claim"] = schema::string("The claim the source backs, as the video says it. Becomes the marker's name.");
    props["note"] = schema::string("A note for yourself (the marker's comment).");
    props["url"] = schema::string("The source's address (http or https).");
    props["title"] = citationFieldSchema("The work: an article, a video, a paper, a book.");
    props["author"] = citationFieldSchema("Who wrote or made it.");
    props["publisher"] = citationFieldSchema("The outlet or site.");
    props["date"] = citationFieldSchema("When it was published (2024-02-01 is best; APA export reads it).");
    props["archive"] = citationFieldSchema("An archive link (http or https) in case the page goes away.");
    props["screenshot"] = citationFieldSchema("A screenshot of the source: a file path or address, as written.");
    props["needed"] = schema::boolean("The claim still wants a source (an uncited claim). Default: true when there is no title or address.");
    props["colorIndex"] = schema::integer("Marker colour (default blue when cited, red when a source is needed).", 0, 15);
    return props;
}

/// Reads the source fields that are given into `marker`.
void applySourceFields(ExecutionContext& c, Marker& marker) {
    MarkerCitation citation = marker.citation.value_or(MarkerCitation{});
    if (c.has("claim")) {
        marker.name = c.string("claim");
    }
    if (c.has("note")) {
        marker.comment = c.string("note");
    }
    if (c.has("url")) {
        marker.url = c.string("url");
    }
    if (c.has("title")) {
        citation.title = c.string("title");
    }
    if (c.has("author")) {
        citation.author = c.string("author");
    }
    if (c.has("publisher")) {
        citation.publisher = c.string("publisher");
    }
    if (c.has("date")) {
        citation.date = c.string("date");
    }
    if (c.has("archive")) {
        citation.archive = c.string("archive");
    }
    if (c.has("screenshot")) {
        citation.screenshot = c.string("screenshot");
    }
    if (c.has("needed")) {
        citation.needed = c.boolean("needed", false);
    }
    marker.type = MarkerType::Source;
    marker.citation = std::move(citation);
}

Json entryJson(ExecutionContext& c, const SourceEntry& e) {
    return Json{{"index", e.marker},
                {"number", e.number},
                {"start", Json{{"ticks", e.start.value}, {"timecode", c.timecode(e.start)}}},
                {"seconds", std::round(e.start.toSeconds() * 100.0) / 100.0},
                {"duration", e.duration.value},
                {"claim", e.claim},
                {"note", e.note},
                {"url", e.url},
                {"title", e.citation.title},
                {"author", e.citation.author},
                {"publisher", e.citation.publisher},
                {"date", e.citation.date},
                {"archive", e.citation.archive},
                {"screenshot", e.citation.screenshot},
                {"cited", e.cited()},
                {"needed", e.needed()}};
}

const Marker& sourceMarker(ExecutionContext& c, std::size_t& index) {
    const Sequence& seq = c.sequence();
    index = c.indexValue(c.params()["index"], "/index");
    if (index >= seq.markers.size() || seq.markers[index].type != MarkerType::Source) {
        fail(ErrorCode::NotFound, "marker " + std::to_string(index) + " is not a source (sources.list shows them)",
             Json{{"path", "/index"}});
    }
    return seq.markers[index];
}

CommandOutput addSource(ExecutionContext& c) {
    Marker marker;
    marker.start = markerFrameTime(c, c.time("time"));
    marker.duration = c.optionalTime("duration").value_or(Ticks{});
    applySourceFields(c, marker);
    if (marker.name.empty()) {
        fail(ErrorCode::InvalidParams, "name the claim the source backs (\"claim\")", Json{{"path", "/claim"}});
    }
    MarkerCitation& citation = *marker.citation;
    if (!c.has("needed")) {
        citation.needed = citation.title.empty() && marker.url.empty();
    }
    marker.colorIndex = c.has("colorIndex") ? static_cast<std::int32_t>(c.integer("colorIndex"))
                                            : (citation.needed ? kNeededColor : kCitedColor);
    requireValidMarker(marker);
    edit::EditResult r = c.take(edit::addMarker(c.state(), c.sequenceId(), marker));
    // The new marker's index: sorted by start, equal starts after the ones there.
    const Sequence* after = r.state.findSequence(c.sequenceId());
    std::size_t index = 0;
    if (after != nullptr) {
        for (std::size_t i = 0; i < after->markers.size(); ++i) {
            if (after->markers[i] == marker) {
                index = i;
            }
        }
    }
    return edited(std::move(r), Json{{"index", index}, {"needed", citation.needed}});
}

CommandOutput updateSource(ExecutionContext& c) {
    std::size_t index = 0;
    Marker marker = sourceMarker(c, index);
    if (c.has("time")) {
        marker.start = markerFrameTime(c, c.time("time"));
    }
    if (c.has("duration")) {
        marker.duration = c.time("duration");
    }
    const bool hadSource = !marker.citation || (marker.citation->title.empty() && marker.url.empty());
    applySourceFields(c, marker);
    MarkerCitation& citation = *marker.citation;
    // Giving a claim its first title or address settles it, unless the person says otherwise.
    if (!c.has("needed") && hadSource && (!citation.title.empty() || !marker.url.empty())) {
        citation.needed = false;
    }
    if (c.has("colorIndex")) {
        marker.colorIndex = static_cast<std::int32_t>(c.integer("colorIndex"));
    }
    if (marker.name.empty()) {
        fail(ErrorCode::InvalidParams, "a source needs its claim (\"claim\")", Json{{"path", "/claim"}});
    }
    requireValidMarker(marker);
    return edited(c.take(edit::updateMarker(c.state(), c.sequenceId(), index, marker)));
}

CommandOutput removeSource(ExecutionContext& c) {
    std::size_t index = 0;
    sourceMarker(c, index);
    return edited(c.take(edit::removeMarker(c.state(), c.sequenceId(), index)));
}

CommandOutput listSources(ExecutionContext& c) {
    const std::vector<SourceEntry> entries = sourceEntries(c.sequence());
    const bool onlyNeeded = c.boolean("onlyNeeded", false);
    Json list = Json::array();
    int needed = 0;
    for (const SourceEntry& e : entries) {
        needed += e.needed() ? 1 : 0;
        if (!onlyNeeded || e.needed()) {
            list.push_back(entryJson(c, e));
        }
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"sources", std::move(list)}, {"count", entries.size()}, {"needed", needed}, {"cited", static_cast<int>(entries.size()) - needed}};
    return out;
}

CommandOutput exportSources(ExecutionContext& c) {
    const std::string name = c.string("format", "youtube");
    const auto format = sourceFormatFromName(name);
    if (!format) {
        fail(ErrorCode::InvalidParams, "\"format\" is youtube, plain, apa or markdown", Json{{"path", "/format"}});
    }
    SourceListOptions options;
    options.includeUncited = c.boolean("includeUncited", false);
    const SourceListText text = formatSources(sourceEntries(c.sequence()), *format, options);
    CommandOutput out;
    out.state = c.state();
    out.warnings = text.warnings;
    out.data = Json{{"format", std::string(sourceFormatName(*format))}, {"text", text.text}, {"cited", text.cited}, {"uncited", text.uncited}};
    return out;
}

CommandOutput lowerThirds(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const std::vector<SourceEntry> entries = sourceEntries(seq);
    const Ticks fallback{static_cast<std::int64_t>(std::llround((c.has("seconds") ? c.number("seconds") : 4.0) * static_cast<double>(kTicksPerSecond)))};
    CaptionStyle style;
    style.fontSize = 0.036;
    style.alignment = CaptionAlign::Left;
    style.placement = CaptionPlacement::Bottom;
    style.maxWidth = 0.6;
    style.safeArea = 0.14; // above the burned-in captions' band
    style.box = true;
    ProjectState state = c.state();
    std::vector<TrackId> lanes;
    std::vector<Ticks> laneEnd;
    Json placed = Json::array();
    CommandOutput out;
    int skipped = 0;
    for (const SourceEntry& e : entries) {
        if (!e.cited()) {
            ++skipped; // nothing to show for a claim without a source
            continue;
        }
        const Ticks length = e.duration.value > 0 ? e.duration : fallback;
        std::size_t lane = lanes.size();
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            if (laneEnd[i] <= e.start) {
                lane = i;
                break;
            }
        }
        if (lane == lanes.size()) {
            if (lanes.size() >= 4) {
                fail(ErrorCode::EditRejected, "more than four sources overlap at " + c.timecode(e.start));
            }
            auto created = c.take(edit::addTrack(state, c.sequenceId(), TrackKind::Video, "Sources"));
            state = std::move(created.state);
            lanes.push_back(created.id);
            laneEnd.push_back(Ticks{});
        }
        laneEnd[lane] = e.start + length;
        edit::EditResult r = c.take(edit::addGraphic(state, c.sequenceId(), lanes[lane], e.start, length,
                                                     captionTextGraphic(citationCaption(e), style, seq.width, seq.height, "Source")));
        state = std::move(r.state);
        out.createdIds.push_back(r.placed.front().value);
        placed.push_back(r.placed.front().value);
    }
    if (placed.empty()) {
        fail(ErrorCode::EditRejected, "no source with a title or an address to show: add sources first (sources.add)");
    }
    out.state = std::move(state);
    out.data = Json{{"clips", std::move(placed)}, {"skippedUncited", skipped}};
    return out;
}

} // namespace

void registerSourceCitationCommands(CommandRegistry& registry) {
    Json addProps = sourceFields();
    addProps["time"] = timeParam("Where the claim is made (the start of the range).");
    addProps["duration"] = timeParam("How long the claim is on screen or in the voice-over. Default 0 (a point).");
    addProps["sequence"] = sequenceParam();
    addBuiltin(registry,
               {"sources.add", 1, "Add Source",
                "Cites a source for a claim at a time of the sequence: a Source marker whose name is the claim, with the "
                "address, title, author, publisher, date, an archive link and a screenshot. Leave the title and address out "
                "(or pass \"needed\": true) to mark an uncited claim to come back to. sources.list shows them, sources.export "
                "writes the YouTube description block or the bibliography.",
                Capability::EditSequence,
                Json{{"type", "object"}, {"properties", std::move(addProps)}, {"additionalProperties", false},
                     {"required", Json::array({"time", "claim"})}},
                Json{{"time", "00:00:42:00"}, {"claim", "Unemployment fell 4 percent in 2023"}, {"url", "https://example.org/report"},
                     {"title", "Labour Market Report"}, {"author", "A. Author"}, {"date", "2024-02-01"}},
                {}, addSource});

    Json updateProps = sourceFields();
    updateProps["index"] = schema::integer("The source's marker index (sources.list shows it).", 0);
    updateProps["time"] = timeParam("Move the claim to this time.");
    updateProps["duration"] = timeParam("Its length.");
    updateProps["sequence"] = sequenceParam();
    addBuiltin(registry,
               {"sources.update", 1, "Edit Source",
                "Changes the fields of a source that are given (the claim, address, title, author, publisher, date, archive link, "
                "screenshot, note, its time). Giving a claim its first title or address clears \"needed\" unless you pass it.",
                Capability::EditSequence,
                Json{{"type", "object"}, {"properties", std::move(updateProps)}, {"additionalProperties", false},
                     {"required", Json::array({"index"})}},
                Json{{"index", 0}, {"title", "Labour Market Report"}, {"url", "https://example.org/report"}}, {}, updateSource});

    addBuiltin(registry,
               {"sources.remove", 1, "Remove Source", "Removes a source (its marker) by index; sources.list shows the indexes.",
                Capability::EditSequence, withSequence({{"index", schema::integer("The source's marker index.", 0)}}, {"index"}),
                Json{{"index", 0}}, {}, removeSource});

    addBuiltin(registry,
               {"sources.list", 1, "List Sources",
                "The sequence's sources by time: each claim with its address, reference and whether it still needs a source; "
                "counts of cited and uncited claims. Changes nothing.",
                Capability::Read,
                withSequence({{"onlyNeeded", schema::boolean("Only the claims that still need a source (default false).", false)}}),
                Json::object(), {}, listSources});

    addBuiltin(registry,
               {"sources.export", 1, "Export Sources",
                "The sources as text to publish: \"youtube\" (a description block with timestamps), \"plain\" (a numbered list), "
                "\"markdown\" or \"apa\" (a bibliography, one entry per distinct source). Claims that still need a source are "
                "named in the warnings and left out unless \"includeUncited\". Text only: the Sources panel writes the file or a PDF.",
                Capability::Read,
                withSequence({{"format", schema::enumeration({"youtube", "plain", "apa", "markdown"}, "The form of the list (default youtube).")},
                              {"includeUncited", schema::boolean("List the claims that need a source too, marked (default false).", false)}}),
                Json{{"format", "apa"}}, {}, exportSources});

    addBuiltin(registry,
               {"sources.lowerThirds", 1, "Show Citations as Lower Thirds",
                "Puts each cited source on screen as a lower-third graphic (\"Source: title - author\") over its claim, on new "
                "\"Sources\" video tracks: for its length, or \"seconds\" (default 4) for a point. Claims without a source are "
                "skipped. One undo step.",
                Capability::EditSequence,
                withSequence({{"seconds", schema::number("How long a point source stays on screen, seconds (default 4).", 0.5, 30.0)}}),
                Json{{"seconds", 5.0}}, {}, lowerThirds});
}

} // namespace wabcut::command::detail
