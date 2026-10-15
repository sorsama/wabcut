#include <wabcut/ai/search/SearchCommands.h>

#include <wabcut/command/Schema.h>

#include <cmath>
#include <cstdio>

namespace wabcut::ai::search {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

FootageSearch& searchOf(ExecutionContext& c) {
    auto* services = command::findService<SearchServices>(c.services());
    if (!services) {
        fail(ErrorCode::InvalidRequest, "smart search is not available here: the host provides no search index");
    }
    auto search = services->search();
    if (!search || !*search) {
        fail(ErrorCode::InvalidRequest, "smart search is not available here: " +
                                            (search ? std::string("the host provides no search index") : search.error().message));
    }
    return **search;
}

Json mediaRefs(std::string description) {
    return schema::array(Json{{"type", Json::array({"integer", "string"})}, {"description", "A media id or exact name."}, {"minimum", 1}},
                         std::move(description), 1);
}

/// "YYYY-MM-DD" or InvalidParams.
std::string dateParam(ExecutionContext& c, const char* key) {
    if (!c.has(key)) {
        return {};
    }
    const std::string date = c.string(key);
    const bool shaped = date.size() == 10 && date[4] == '-' && date[7] == '-' &&
                        std::ranges::all_of(date, [](char ch) { return ch == '-' || (ch >= '0' && ch <= '9'); });
    if (!shaped) {
        fail(ErrorCode::InvalidParams, std::string("\"") + key + "\" must be a date like 2026-10-11");
    }
    return date;
}

std::optional<Ticks> secondsParam(ExecutionContext& c, const char* key) {
    if (!c.has(key)) {
        return std::nullopt;
    }
    return Ticks{static_cast<std::int64_t>(std::llround(c.number(key) * static_cast<double>(kTicksPerSecond)))};
}

double rounded(double value, double scale) { return std::round(value * scale) / scale; }

CommandOutput searchFootage(ExecutionContext& c) {
    FootageSearch& search = searchOf(c);
    SearchFilters filters;
    if (c.has("media")) {
        filters.media = c.mediaList("media");
    }
    filters.name = c.string("name");
    filters.from = dateParam(c, "from");
    filters.to = dateParam(c, "to");
    filters.minDuration = secondsParam(c, "minDuration");
    filters.maxDuration = secondsParam(c, "maxDuration");
    filters.visual = c.boolean("visual", true);
    filters.transcript = c.boolean("transcript", true);
    const auto limit = static_cast<std::size_t>(c.integer("limit", 20));

    auto results = search.search(c.state(), c.string("query"), filters, limit);
    if (!results) {
        fail(results.error().code == wabcut::ErrorCode::InvalidArgument ? ErrorCode::InvalidParams
             : results.error().code == wabcut::ErrorCode::Cancelled     ? ErrorCode::Cancelled
                                                                        : ErrorCode::Internal,
             results.error().message);
    }
    Json hits = Json::array();
    for (const SearchHit& hit : results->hits) {
        hits.push_back(hitToJson(hit));
    }
    Json notIndexed = Json::array();
    for (const MediaId id : results->notIndexed) {
        notIndexed.push_back(id.value);
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"query", results->query},
                    {"hits", std::move(hits)},
                    {"mediaSearched", results->mediaSearched},
                    {"notIndexed", std::move(notIndexed)},
                    {"visual", results->visual},
                    {"milliseconds", rounded(results->milliseconds, 10.0)}};
    out.warnings = results->warnings;
    return out;
}

CommandOutput indexMedia(ExecutionContext& c) {
    FootageSearch& search = searchOf(c);
    std::vector<MediaId> media;
    if (c.has("media")) {
        media = c.mediaList("media");
    }
    auto report = search.index(c.state(), media, [&c](const IndexProgress& p) {
        char text[96];
        std::snprintf(text, sizeof text, " (%zu of %zu, %d%%)", p.index + 1, p.count, static_cast<int>(std::lround(p.fraction * 100.0)));
        c.checkpoint("indexing '" + p.media + "'" + text);
        return true;
    });
    if (!report) {
        fail(report.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
             report.error().message);
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"indexed", report->indexed},     {"upToDate", report->upToDate}, {"failed", report->failed},
                    {"keyframes", report->keyframes}, {"phrases", report->phrases},   {"visual", report->visual},
                    {"errors", report->errors}};
    out.warnings = report->errors;
    if (!report->visual) {
        out.warnings.push_back("visual indexing is off (no embedding model here): only transcripts were indexed");
    }
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

std::expected<std::shared_ptr<FootageSearch>, Error> SearchServices::search() {
    std::lock_guard lock(m_mutex);
    if (!m_search && m_open) {
        auto opened = m_open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        m_search = std::move(*opened);
    }
    return m_search;
}

Json hitToJson(const SearchHit& hit) {
    Json j{{"media", hit.media.value},
           {"mediaName", hit.mediaName},
           {"sourceIn", hit.range.start.value},
           {"sourceOut", hit.range.end().value},
           {"start", rounded(hit.range.start.toSeconds(), 1000.0)},
           {"end", rounded(hit.range.end().toSeconds(), 1000.0)},
           {"thumbnail", hit.thumbnail.value},
           {"score", rounded(hit.score, 1e6)},
           {"transcript", hit.relevance.has_value()},
           {"snippet", hit.snippet},
           {"matchedWords", hit.matchedWords}};
    if (hit.similarity) {
        j["similarity"] = rounded(*hit.similarity, 1e4);
    }
    return j;
}

void registerSearchCommands(command::CommandRegistry& registry) {
    using command::Capability;
    add(registry,
        command::runsModel({"search.footage", 1, "Search Footage",
         "Smart search: finds the moments of the project's media that show or say what the query describes (\"a dog "
         "on the beach\", \"the part where she mentions the budget\", Thai too), by what the frames show (an image-text "
         "model) and what is said (the transcripts). Returns hits best first: the media, its source range (sourceIn / "
         "sourceOut in ticks, start / end in seconds) ready for clip.insert, the matched transcript words and a score. "
         "Media must be indexed first (search.indexMedia); a read, it changes nothing.",
         Capability::Read,
         schema::object({{"query", Json{{"type", "string"}, {"description", "What to find."}, {"minLength", 1}, {"maxLength", 500}}},
                         {"limit", schema::integer("Most hits to return (default 20).", 1, 100)},
                         {"media", mediaRefs("Only these media items.")},
                         {"name", schema::string("Only media whose name contains this (case-insensitive).")},
                         {"from", schema::string("Only media dated on or after this day (YYYY-MM-DD, the file's date).")},
                         {"to", schema::string("Only media dated on or before this day (YYYY-MM-DD).")},
                         {"minDuration", schema::number("Only media at least this long (seconds).", 0.0, 7.0 * 86400.0)},
                         {"maxDuration", schema::number("Only media at most this long (seconds).", 0.0, 7.0 * 86400.0)},
                         {"visual", schema::boolean("Search what the frames show (default true).", true)},
                         {"transcript", schema::boolean("Search what is said (default true).", true)}},
                        {"query"}),
         Json{{"query", "a dog running on the beach"}, {"limit", 10}}, {}, searchFootage}));
    add(registry,
        command::runsModel({"search.indexMedia", 1, "Index Media for Search",
         "Brings the smart-search index up to date for some or all media: keyframes of every shot (one per shot, more "
         "in long shots) through the image-text model, and the transcript's phrases. Unchanged files are skipped, so "
         "running it again is cheap. Runs locally; nothing leaves the computer.",
         Capability::MediaAnalysis, schema::object({{"media", mediaRefs("Media items to index (default: all).")}}),
         Json::object(), {}, indexMedia}));
}

} // namespace wabcut::ai::search
