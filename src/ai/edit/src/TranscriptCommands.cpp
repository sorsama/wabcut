#include <wabcut/ai/generation/GenerationCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/autozoom/AutoZoomCommands.h>
#include <wabcut/ai/dub/DubCommands.h>
#include <wabcut/ai/highlights/HighlightCommands.h>
#include <wabcut/ai/multicam/MulticamAiCommands.h>
#include <wabcut/ai/segment/FaceRedaction.h>
#include <wabcut/ai/segment/ObjectMask.h>
#include <wabcut/ai/workflow/WorkflowCommands.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/ai/roughcut/RoughCutCommands.h>
#include <wabcut/ai/translate/TranslateCommands.h>
#include <wabcut/ai/search/SearchCommands.h>
#include <wabcut/ai/shorts/ShortsCommands.h>
#include <wabcut/ai/stabilize/StabilizeCommands.h>
#include <wabcut/audio/edit/AudioCommands.h>
#include <wabcut/color/ColorCommands.h>
#include <wabcut/masks/MaskCommands.h>

#include <wabcut/ai/TranscriptRedaction.h>
#include <wabcut/ai/edit/CaptionBuilder.h>
#include <wabcut/ai/edit/MediaTranscript.h>
#include <wabcut/ai/edit/Profanity.h>
#include <wabcut/ai/edit/TextEdits.h>
#include <wabcut/ai/edit/TranscriptEditing.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/command/Schema.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/interchange/Docx.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/TextFind.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/RedactionMedia.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <tuple>
#include <unordered_map>

namespace wabcut::ai::edit {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

TranscriptServices& servicesOf(ExecutionContext& c) {
    auto* services = command::findService<TranscriptServices>(c.services());
    if (!services || !services->transcripts) {
        fail(ErrorCode::InvalidRequest,
             "no transcripts here: the host provides none (wabcut-cli reads them from the analysis cache or "
             "--transcript)");
    }
    return *services;
}

SequenceTranscript transcriptOf(ExecutionContext& c) {
    return SequenceTranscript::build(c.state(), c.sequenceId(), *servicesOf(c).transcripts);
}

/// A read's output; says so when the sequence has no words.
CommandOutput readOutput(ExecutionContext& c, const SequenceTranscript& st, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    if (st.empty()) {
        out.warnings.push_back("the sequence's audio has no transcript yet (transcribe its media first)");
    }
    return out;
}

Json withSequence(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required = {}) {
    Json s = schema::object(properties, std::move(required));
    s["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    return s;
}

Json timeParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Timecode in the sequence's rate (\"00:01:02:03\") or ticks."}};
}

Json secondsParam(std::string description, double minimum, double maximum) {
    return schema::number(std::move(description), minimum, maximum);
}

/// Word indices are bounded in the schema (no transcript has a billion words) and read with
/// ExecutionContext::indexValue, so a huge value is refused before any index arithmetic.
Json wordIndex(std::string description) { return schema::integer(std::move(description), 0, 1'000'000'000); }

Json wordRange(std::string description) {
    Json s = schema::array(wordIndex("Word index."), std::move(description), 2);
    s["maxItems"] = 2;
    return s;
}

Json wordRanges(std::string description) {
    return schema::array(wordRange("[first, last] word indices, inclusive."), std::move(description), 1);
}

Ticks secondsOr(ExecutionContext& c, const char* key, double fallback) {
    const double s = c.has(key) ? c.number(key) : fallback;
    return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))};
}

CutOptions cutOptions(ExecutionContext& c) {
    CutOptions o;
    o.padBefore = o.padAfter = secondsOr(c, "padding", 0.04);
    o.roomTone = secondsOr(c, "roomTone", 0.15);
    return o;
}

std::vector<WordRange> rangesOf(ExecutionContext& c, const char* key) {
    std::vector<WordRange> out;
    if (!c.has(key)) {
        return out;
    }
    const Json& list = c.params()[key];
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string path = "/" + std::string(key) + "/" + std::to_string(i);
        out.push_back(WordRange{c.indexValue(list[i][0], path + "/0"), c.indexValue(list[i][1], path + "/1")});
    }
    return out;
}

double rounded(double v) { return std::round(v * 100.0) / 100.0; }

Json timeJson(ExecutionContext& c, Ticks t) { return Json{{"ticks", t.value}, {"timecode", c.timecode(t)}}; }

/// Applies a delete plan as the catalog's own extracts, inside this command (one undo step).
CommandOutput applyPlan(ExecutionContext& c, const EditPlan& plan, Json data) {
    CommandOutput out;
    out.state = plan.cuts.empty()
                    ? c.state()
                    : runInline(command::builtinCommands(), c.state(), toCommandBatch(plan), c.control(), c.services());
    Json cuts = Json::array();
    for (const PlannedCut& cut : plan.cuts) {
        cuts.push_back(Json::array({c.timecode(cut.range.start), c.timecode(cut.range.end())}));
    }
    data["cuts"] = std::move(cuts);
    data["words"] = plan.words;
    data["removed"] = timeJson(c, plan.removed());
    out.data = std::move(data);
    if (plan.cuts.empty()) {
        out.warnings.push_back("nothing to remove");
    }
    return out;
}

ReviewOptions reviewOptions(ExecutionContext& c) {
    ReviewOptions o;
    o.fillers = c.boolean("fillers", true);
    const double silences = c.has("silences") ? c.number("silences") : 0.8;
    if (silences > 0.0) {
        o.silenceThreshold = Ticks{static_cast<std::int64_t>(std::llround(silences * static_cast<double>(kTicksPerSecond)))};
    } else {
        o.silenceThreshold.reset();
    }
    o.silenceDb = c.has("silenceDb") ? c.number("silenceDb") : -40.0;
    return o;
}

Json reviewParams(std::initializer_list<std::pair<const char*, Json>> extra = {}) {
    Json s = withSequence({{"fillers", schema::boolean("Look for filler words (um, uh; Thai อืม, เอ่อ ...). Default true.", true)},
                           {"silences", secondsParam("Pauses at least this many seconds long; 0 = none. Default 0.8.", 0.0, 3600.0)},
                           {"silenceDb", schema::number("A pause louder than this (dBFS) has sound in it. Default -40.", -200.0, 0.0)}});
    for (const auto& [key, value] : extra) {
        s["properties"][key] = value;
    }
    return s;
}

// --- Reads ---------------------------------------------------------------------------------------------------

CommandOutput transcriptText(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    const Ticks maxPause = secondsOr(c, "maxPause", 1.0);
    const std::optional<Ticks> from = c.optionalTime("from");
    const std::optional<Ticks> to = c.optionalTime("to");
    Json paragraphs = Json::array();
    std::size_t first = 0;
    for (std::size_t i = 0; i < st.size(); ++i) {
        const bool last = i + 1 == st.size();
        const bool split = last || (st[i + 1].speaker != st[i].speaker) ||
                           st[i + 1].timeline.start - st[i].timeline.end() > maxPause || i + 1 - first >= 80;
        if (!split) {
            continue;
        }
        const Ticks start = st[first].timeline.start;
        const Ticks end = st[i].timeline.end();
        if ((!from || end > *from) && (!to || start < *to)) {
            paragraphs.push_back(Json::array({first, i, c.timecode(start), c.timecode(end),
                                              st[first].speaker ? Json(*st[first].speaker) : Json(nullptr),
                                              st.textOf(first, i)}));
        }
        first = i + 1;
    }
    return readOutput(c, st,
                      Json{{"words", st.size()},
                           {"language", st.empty() ? std::string() : std::string(st.language(0))},
                           {"paragraphFields", Json::array({"first", "last", "start", "end", "speaker", "text"})},
                           {"paragraphs", std::move(paragraphs)}});
}

CommandOutput transcriptWords(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    // Words [lo, hi): the index window intersected with the time window.
    std::size_t lo = c.has("first") ? c.indexValue(c.params()["first"], "/first") : 0;
    std::size_t hi = st.size();
    if (c.has("last")) {
        const std::size_t last = c.indexValue(c.params()["last"], "/last");
        hi = last < st.size() ? last + 1 : st.size(); // no overflow for huge values
    }
    if (c.has("from") || c.has("to")) {
        const auto [b, e] = st.wordsIn(TimeRange::fromStartEnd(c.optionalTime("from").value_or(Ticks{Ticks::min().value / 2}),
                                                               c.optionalTime("to").value_or(Ticks{Ticks::max().value / 2})));
        lo = std::max(lo, b);
        hi = std::min(hi, e);
    }
    hi = std::min(hi, st.size());
    const auto limit = static_cast<std::size_t>(c.integer("limit", 500));
    Json words = Json::array();
    bool truncated = false;
    for (std::size_t i = lo; i < hi; ++i) {
        if (words.size() == limit) {
            truncated = true;
            break;
        }
        const TimelineWord& w = st[i];
        words.push_back(Json::array({i, st.text(i), c.timecode(w.timeline.start), c.timecode(w.timeline.end()),
                                     w.clip.value, c.trackLabel(w.track), w.speaker ? Json(*w.speaker) : Json(nullptr),
                                     rounded(w.confidence)}));
    }
    Json data{{"total", st.size()},
              {"wordFields", Json::array({"index", "text", "start", "end", "clip", "track", "speaker", "confidence"})},
              {"words", std::move(words)}};
    if (truncated) {
        data["truncated"] = true;
    }
    return readOutput(c, st, std::move(data));
}

// P0 round: the source clip transcript (MediaTranscript.h) for the CLI and MCP: what one media item's audio stream
// says, in source time (the Transcript panel's Source mode reads the same words).
CommandOutput transcriptMediaWords(ExecutionContext& c) {
    const MediaItem& media = *c.state().findMedia(c.media("media"));
    if (media.isGenerator() || media.info.audio.empty()) {
        fail(ErrorCode::InvalidParams, "'" + media.name + "' has no sound to read words from", Json{{"path", "/media"}});
    }
    TranscriptServices& services = servicesOf(c);
    const int streams = static_cast<int>(media.info.audio.size());
    int stream = 0;
    if (c.has("audioStream")) {
        const std::int64_t given = c.integer("audioStream");
        if (given < 0 || given >= streams) {
            fail(ErrorCode::InvalidParams,
                 "'" + media.name + "' has " + std::to_string(streams) + (streams == 1 ? " audio stream" : " audio streams"),
                 Json{{"path", "/audioStream"}});
        }
        stream = static_cast<int>(given);
    } else {
        // The first stream that has words (OBS recordings: often not the first, which is the mix).
        for (int s = 0; s < streams; ++s) {
            if (services.transcripts->transcript(media, s)) {
                stream = s;
                break;
            }
        }
    }
    const MediaTranscript mt = MediaTranscript::build(c.state(), media.id, stream, *services.transcripts);
    const Rate rate = command::sourceTimecodeRate(c.state(), media);
    const auto sourceTime = [&](const char* key) -> std::optional<Ticks> {
        if (!c.has(key)) {
            return std::nullopt;
        }
        const Json& value = c.params()[key];
        if (value.is_number_integer()) {
            return Ticks{value.get<std::int64_t>()};
        }
        if (value.is_string()) {
            if (auto t = parseTimecode(value.get<std::string>(), rate, false)) {
                return *t;
            }
        }
        fail(ErrorCode::InvalidParams, std::string("expected ticks or a timecode in the media's rate"),
             Json{{"path", std::string("/") + key}});
    };
    // Words [lo, hi): the index window intersected with the time window.
    std::size_t lo = c.has("first") ? c.indexValue(c.params()["first"], "/first") : 0;
    std::size_t hi = mt.size();
    if (c.has("last")) {
        const std::size_t last = c.indexValue(c.params()["last"], "/last");
        hi = last < mt.size() ? last + 1 : mt.size();
    }
    const std::optional<Ticks> from = sourceTime("from");
    const std::optional<Ticks> to = sourceTime("to");
    const TimeRange window = TimeRange::fromStartEnd(from.value_or(Ticks{Ticks::min().value / 2}),
                                                     to.value_or(Ticks{Ticks::max().value / 2}));
    if (from || to) {
        const auto [b, e] = mt.wordsIn(window);
        lo = std::max(lo, b);
        hi = std::min(hi, e);
    }
    // Fix round 1: inside that span only the words that overlap the window (words may overlap each other); a
    // zero-length word by its instant.
    const auto inWindow = [&](const MediaWord& w) {
        return w.source.isEmpty() ? w.source.start >= window.start && w.source.start < window.end()
                                  : w.source.start < window.end() && w.source.end() > window.start;
    };
    const auto limit = static_cast<std::size_t>(c.integer("limit", 500));
    Json words = Json::array();
    bool truncated = false;
    for (std::size_t i = lo; i < hi; ++i) {
        const MediaWord& w = mt[i];
        if ((from || to) && !inWindow(w)) {
            continue;
        }
        if (words.size() == limit) {
            truncated = true;
            break;
        }
        words.push_back(Json::array({i, mt.text(i), ticksToTimecode(w.source.start, rate, false),
                                     ticksToTimecode(w.source.end(), rate, false), w.source.start.value, w.source.end().value,
                                     w.speaker ? Json(*w.speaker) : Json(nullptr), rounded(w.confidence)}));
    }
    Json data{{"media", media.id.value},
              {"name", media.name},
              {"audioStream", stream},
              {"audioStreams", streams},
              {"language", std::string(mt.language())},
              {"frameRate", Json{{"num", rate.num}, {"den", rate.den}}},
              {"total", mt.size()},
              {"wordFields", Json::array({"index", "text", "start", "end", "startTicks", "endTicks", "speaker", "confidence"})},
              {"words", std::move(words)}};
    if (truncated) {
        data["truncated"] = true;
    }
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    if (mt.empty()) {
        out.warnings.push_back("this audio stream has no transcript yet (transcript.transcribe with \"media\" first)");
    }
    return out;
}

Json reviewJson(ExecutionContext& c, const std::vector<ReviewItem>& items) {
    Json rows = Json::array();
    for (const ReviewItem& item : items) {
        rows.push_back(Json::array({toString(item.kind), item.firstWord, item.lastWord, c.timecode(item.range.start),
                                    c.timecode(item.range.end()), rounded(item.confidence), item.label, item.context}));
    }
    return rows;
}

CommandOutput transcriptReview(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    TranscriptServices& services = servicesOf(c);
    const auto items = review(st, c.state(), reviewOptions(c), services.energy.get());
    return readOutput(c, st,
                      Json{{"itemFields", Json::array({"kind", "first", "last", "start", "end", "confidence", "label", "context"})},
                           {"items", reviewJson(c, items)}});
}

// --- Transcription -------------------------------------------------------------------------------------------

MediaTranscriber& transcriberOf(ExecutionContext& c) {
    auto* services = command::findService<TranscribeServices>(c.services());
    if (!services || !services->transcriber) {
        fail(ErrorCode::InvalidRequest,
             "transcription is not available here: the host provides none (wabcut-cli and the app transcribe media)");
    }
    return *services->transcriber;
}

Json mediaRef(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})}, {"description", description + " A media id or exact name."}, {"minimum", 1}};
}

Json clipRef(std::string description) {
    return Json{{"type", Json::array({"integer", "object"})},
                {"description", description + " A clip id, or {\"track\", \"time\"} for the clip there."},
                {"properties", Json{{"track", Json{{"type", Json::array({"string", "integer"})}, {"minimum", 1}}},
                                    {"time", timeParam("A time inside the clip.")}}},
                {"required", Json::array({"track", "time"})},
                {"additionalProperties", false},
                {"minimum", 1}};
}

struct TranscribeItem {
    const MediaItem* media = nullptr;
    int audioStream = 0;
};

/// The audio streams to transcribe: the named media items and clips, else every audio clip's of the sequence.
std::vector<TranscribeItem> transcribeItems(ExecutionContext& c) {
    std::vector<TranscribeItem> items;
    const auto add = [&](const MediaItem& media, int stream) {
        const bool seen = std::ranges::any_of(items, [&](const TranscribeItem& i) {
            return i.media->id == media.id && i.audioStream == stream;
        });
        if (!seen) {
            items.push_back({&media, stream});
        }
    };
    const auto withAudio = [&](MediaId id) -> const MediaItem& {
        const MediaItem* media = c.state().findMedia(id);
        if (!media) {
            fail(ErrorCode::NotFound, "media item " + std::to_string(id.value) + " does not exist");
        }
        if (!media->hasStream(StreamRef{StreamKind::Audio, 0})) {
            fail(ErrorCode::EditRejected, "'" + media->name + "' has no audio");
        }
        return *media;
    };
    for (const MediaId id : c.mediaList("media")) {
        add(withAudio(id), 0);
    }
    for (const ClipId id : c.clips("clips")) {
        const Clip& clip = c.sequence().findClip(id)->clip();
        const MediaItem& media = withAudio(clip.media);
        // A video clip's media is transcribed from its first audio stream (what its linked audio plays).
        add(media, clip.stream.kind == StreamKind::Audio ? clip.stream.index : 0);
    }
    if (!c.has("media") && !c.has("clips")) {
        for (const auto& track : c.sequence().audioTracks) {
            for (const Clip& clip : track->clips) {
                if (clip.isFreeze() || clip.stream.kind != StreamKind::Audio) {
                    continue;
                }
                if (const MediaItem* media = c.state().findMedia(clip.media);
                    media && media->hasStream(StreamRef{StreamKind::Audio, clip.stream.index})) {
                    add(*media, clip.stream.index);
                }
            }
        }
    }
    return items;
}

Json itemJson(const TranscribeItem& item) {
    return Json{{"media", item.media->id.value}, {"name", item.media->name}, {"audioStream", item.audioStream}};
}

ErrorCode commandCode(const Error& e) {
    switch (e.code) {
    case ::wabcut::ErrorCode::NotFound:
        return ErrorCode::NotFound;
    case ::wabcut::ErrorCode::Cancelled:
        return ErrorCode::Cancelled;
    case ::wabcut::ErrorCode::InvalidArgument:
        return ErrorCode::InvalidParams;
    default:
        return ErrorCode::EditRejected;
    }
}

CommandOutput transcribeMedia(ExecutionContext& c) {
    MediaTranscriber& transcriber = transcriberOf(c);
    MediaTranscriber::Options options;
    options.provider = c.string("provider", "auto");
    options.language = c.string("language", "auto");
    options.force = c.boolean("force", false);
    const std::vector<TranscribeItem> items = transcribeItems(c);
    CommandOutput out;
    out.state = c.state(); // transcripts go to the analysis cache, not the project
    Json rows = Json::array();

    if (c.boolean("check", false)) {
        Json providers = Json::array();
        for (const MediaTranscriber::Provider& p : transcriber.providers()) {
            Json row{{"id", p.id}, {"label", p.label}, {"available", p.available}};
            if (!p.reason.empty()) {
                row["reason"] = p.reason;
            }
            providers.push_back(std::move(row));
        }
        for (const TranscribeItem& item : items) {
            Json row = itemJson(item);
            row["hasTranscript"] = transcriber.hasTranscript(*item.media, item.audioStream);
            rows.push_back(std::move(row));
        }
        const auto resolved = transcriber.resolve(options.provider);
        out.data = Json{{"provider", resolved ? Json(*resolved) : Json(nullptr)},
                        {"providers", std::move(providers)},
                        {"items", std::move(rows)}};
        if (!resolved) {
            out.data["providerError"] = resolved.error().message;
        }
        return out;
    }

    if (items.empty()) {
        out.data = Json{{"transcribed", 0}, {"cached", 0}, {"failed", 0}, {"items", std::move(rows)}};
        out.warnings.push_back("nothing to transcribe: the sequence has no audio clips");
        return out;
    }
    // For the report: audio with a transcript needs no provider, the rest fails with why there is none.
    const auto provider = transcriber.resolve(options.provider);
    std::size_t transcribed = 0;
    std::size_t cached = 0;
    std::optional<Error> firstError;
    for (const TranscribeItem& item : items) {
        const std::string what = "transcribing '" + item.media->name + "'";
        c.checkpoint(what);
        std::exception_ptr stopped;
        int lastPercent = -1;
        MediaTranscriber::Options itemOptions = options;
        itemOptions.redacted = ai::audioRedactionRanges(c.state(), item.media->id); // review P1-3
        auto result = transcriber.transcribe(*item.media, item.audioStream, itemOptions, [&](double fraction) {
            const int percent = static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
            if (percent == lastPercent) {
                return true;
            }
            lastPercent = percent;
            try {
                c.checkpoint(what + " (" + std::to_string(percent) + "%)");
                return true;
            } catch (...) {
                stopped = std::current_exception(); // rethrown once the transcriber has stopped
                return false;
            }
        });
        if (stopped) {
            std::rethrow_exception(stopped);
        }
        Json row = itemJson(item);
        if (result) {
            (result->fromCache ? cached : transcribed) += 1;
            row["status"] = result->fromCache ? "cached" : "transcribed";
            row["provider"] = result->provider;
            row["words"] = result->words;
            row["language"] = result->language;
        } else {
            row["status"] = "failed";
            row["error"] = result.error().message;
            out.warnings.push_back("'" + item.media->name + "': " + result.error().message);
            if (!firstError) {
                firstError = result.error();
            }
        }
        rows.push_back(std::move(row));
    }
    if (firstError && transcribed + cached == 0) {
        fail(commandCode(*firstError), "no transcript made: " + firstError->message, Json{{"items", rows}});
    }
    out.data = Json{{"provider", provider ? Json(*provider) : Json(nullptr)},
                    {"transcribed", transcribed},
                    {"cached", cached},
                    {"failed", items.size() - transcribed - cached},
                    {"items", std::move(rows)}};
    return out;
}

// --- Edits ---------------------------------------------------------------------------------------------------

CommandOutput deleteRanges(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    DeleteRequest request;
    request.words = rangesOf(c, "words");
    if (c.has("gaps")) {
        const Json& gaps = c.params()["gaps"];
        for (std::size_t i = 0; i < gaps.size(); ++i) {
            request.gapsAfter.push_back(c.indexValue(gaps[i], "/gaps/" + std::to_string(i)));
        }
    }
    request.options = cutOptions(c);
    if (request.words.empty() && request.gapsAfter.empty()) {
        fail(ErrorCode::InvalidParams, "pass \"words\" and/or \"gaps\"");
    }
    return applyPlan(c, c.take(planDelete(st, c.state(), request)), Json::object());
}

CommandOutput keepRanges(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    const std::vector<WordRange> keep = rangesOf(c, "words");
    return applyPlan(c, c.take(planKeep(st, c.state(), keep, cutOptions(c))), Json::object());
}

CommandOutput moveRange(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    const Json& range = c.params()["words"];
    const WordRange words{c.indexValue(range[0], "/words/0"), c.indexValue(range[1], "/words/1")};
    const std::size_t before = c.indexValue(c.params()["before"], "/before");
    const MovePlan plan = c.take(planMove(st, c.state(), words, before, cutOptions(c)));
    CommandOutput out;
    out.state = runInline(command::builtinCommands(), c.state(), toCommandBatch(plan, c.state()), c.control(), c.services());
    out.data = Json{{"source", Json::array({c.timecode(plan.source.start), c.timecode(plan.source.end())})},
                    {"destination", c.timecode(plan.destination)}};
    return out;
}

CommandOutput cleanup(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    TranscriptServices& services = servicesOf(c);
    const double minConfidence = c.has("minConfidence") ? c.number("minConfidence") : 0.5;
    std::vector<ReviewItem> chosen;
    std::size_t fillers = 0;
    std::size_t silences = 0;
    for (ReviewItem& item : review(st, c.state(), reviewOptions(c), services.energy.get())) {
        if (item.confidence >= minConfidence) {
            (item.kind == ReviewKind::Filler ? fillers : silences) += 1;
            chosen.push_back(std::move(item));
        }
    }
    const EditPlan plan = c.take(planDelete(st, c.state(), deleteRequestFor(chosen, cutOptions(c))));
    return applyPlan(c, plan, Json{{"fillers", fillers}, {"silences", silences}});
}

/// p02 audio: a list of strings param (at most ProfanityFilter::kMaxWords).
std::vector<std::string> stringList(ExecutionContext& c, const char* key) {
    std::vector<std::string> out;
    if (!c.has(key)) {
        return out;
    }
    const Json& list = c.params()[key];
    for (std::size_t i = 0; i < list.size() && i < ProfanityFilter::kMaxWords; ++i) {
        if (!list[i].is_string()) {
            fail(ErrorCode::InvalidParams, "a word list holds strings", Json{{"path", std::string("/") + key + "/" + std::to_string(i)}});
        }
        out.push_back(list[i].get<std::string>());
    }
    return out;
}

/// p02 audio: the profanity filter a request asks for ("hideProfanity", or always with `always`), with its
/// "profanityWords" added and "allowedWords" taken out of the built-in English and Thai lists.
std::optional<ProfanityFilter> profanityOf(ExecutionContext& c, bool always = false) {
    if (!always && !c.boolean("hideProfanity", false)) {
        return std::nullopt;
    }
    return ProfanityFilter(stringList(c, "profanityWords"), stringList(c, "allowedWords"));
}

CommandOutput generateCaptions(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    if (st.empty()) {
        fail(ErrorCode::EditRejected, "the sequence has no transcript to caption");
    }
    CaptionOptions options;
    options.maxCharsPerLine = static_cast<int>(c.integer("maxCharsPerLine", 42));
    options.maxLines = static_cast<int>(c.integer("maxLines", 2));
    options.maxDuration = secondsOr(c, "maxDuration", 7.0);
    options.minDuration = secondsOr(c, "minDuration", 0.7);
    const std::optional<ProfanityFilter> profanity = profanityOf(c); // p02 audio: Hide profanity
    options.profanity = profanity ? &*profanity : nullptr;
    CaptionTrack track = buildCaptions(st, options, c.string("name", "Captions"));
    if (c.has("replace")) {
        track.id = CaptionTrackId{static_cast<std::uint64_t>(c.integer("replace"))};
    }
    const std::size_t cues = track.cues.size();
    const std::string language = track.language;
    ::wabcut::edit::EditResult r = c.take(::wabcut::edit::setCaptionTrack(c.state(), c.sequenceId(), std::move(track)));
    CommandOutput out;
    const CaptionTrackId id = r.createdCaptionTrack.isValid() ? r.createdCaptionTrack
                                                              : CaptionTrackId{static_cast<std::uint64_t>(c.integer("replace"))};
    if (r.createdCaptionTrack.isValid()) {
        out.createdIds.push_back(id.value);
    }
    out.state = std::move(r.state);
    out.data = Json{{"captionTrack", id.value}, {"cues", cues}, {"language", language}};
    return out;
}

// --- C.7: captions from the transcript, corrections, speakers -------------------------------------------------

CaptionOptions captionOptions(ExecutionContext& c) {
    CaptionOptions o;
    o.maxCharsPerLine = static_cast<int>(c.integer("maxCharsPerLine", 42));
    o.maxLines = static_cast<int>(c.integer("maxLines", 2));
    o.maxDuration = secondsOr(c, "maxDuration", 7.0);
    o.minDuration = secondsOr(c, "minDuration", 0.7);
    o.maxPause = secondsOr(c, "maxPause", 1.0);
    o.minGap = secondsOr(c, "gap", 0.0);
    o.splitOnPunctuation = c.boolean("splitOnPunctuation", true);
    o.breakOnSpeakerChange = c.boolean("breakOnSpeakerChange", true);
    if (const auto* services = command::findService<command::CaptionServices>(c.services())) {
        o.breaker = &services->lineBreaker(); // the host's line-break rules (Thai, CJK, ...)
    }
    return o;
}

/// The media stream a timeline word comes from.
std::pair<MediaId, std::int32_t> streamOf(const Sequence& seq, const TimelineWord& w) {
    const auto loc = seq.findClip(w.clip);
    return loc ? std::pair{loc->clip().media, loc->clip().stream.index} : std::pair{MediaId{}, 0};
}

/// `state` with a project speaker for every diarization label the sequence's words use (the words' speakers can
/// then be named and shown on captions).
ProjectState withSpeakers(ExecutionContext& c, ProjectState state, const SequenceTranscript& st) {
    std::map<std::pair<std::uint64_t, std::int32_t>, std::vector<std::int32_t>> labels;
    for (const TimelineWord& w : st.words()) {
        if (w.speaker) {
            const auto [media, stream] = streamOf(c.sequence(), w);
            if (media.isValid()) {
                labels[{media.value, stream}].push_back(*w.speaker);
            }
        }
    }
    for (const auto& [key, list] : labels) {
        state = c.take(::wabcut::edit::registerSpeakerLabels(state, MediaId{key.first}, key.second, list));
    }
    return state;
}

/// The caption track named by "captionTrack" (default: the first) of the context's sequence.
const CaptionTrack& captionTrackParamOf(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    if (seq.captionTracks.empty()) {
        fail(ErrorCode::NotFound, "the sequence has no caption tracks");
    }
    if (!c.has("captionTrack")) {
        return seq.captionTracks.front();
    }
    const auto id = static_cast<std::uint64_t>(c.integer("captionTrack"));
    for (const CaptionTrack& t : seq.captionTracks) {
        if (t.id.value == id) {
            return t;
        }
    }
    fail(ErrorCode::NotFound, "no caption track " + std::to_string(id) + " in this sequence", Json{{"path", "/captionTrack"}});
}

CommandOutput generateFromTranscript(ExecutionContext& c) {
    SequenceTranscript st = transcriptOf(c);
    if (st.empty()) {
        fail(ErrorCode::EditRejected, "the sequence has no transcript to caption");
    }
    ProjectState state = c.state();
    if (c.boolean("speakers", true)) {
        state = withSpeakers(c, std::move(state), st);
        st = SequenceTranscript::build(state, c.sequenceId(), *servicesOf(c).transcripts);
    }
    CaptionOptions options = captionOptions(c);
    const std::optional<ProfanityFilter> profanity = profanityOf(c); // p02 audio: Hide profanity
    options.profanity = profanity ? &*profanity : nullptr;
    CaptionTrack track = buildCaptions(st, options, c.string("name", "Captions"));
    if (c.has("language")) {
        track.language = c.string("language");
    }
    if (c.has("replace")) {
        track.id = CaptionTrackId{static_cast<std::uint64_t>(c.integer("replace"))};
        for (const CaptionTrack& existing : c.sequence().captionTracks) {
            if (existing.id == track.id) {
                track.style = existing.style; // a new take keeps the track's look
            }
        }
    }
    if (c.has("preset")) {
        const auto style = ::wabcut::edit::captionPresetStyle(state, c.string("preset"));
        if (!style) {
            fail(ErrorCode::NotFound, "there is no caption preset \"" + c.string("preset") + "\"", Json{{"path", "/preset"}});
        }
        track.style = *style;
    }
    const std::size_t cues = track.cues.size();
    const std::string language = track.language;
    const std::size_t speakers = static_cast<std::size_t>(
        std::count_if(track.cues.begin(), track.cues.end(), [](const CaptionCue& cue) { return cue.speaker.isValid(); }));
    ::wabcut::edit::EditResult r = c.take(::wabcut::edit::setCaptionTrack(state, c.sequenceId(), std::move(track)));
    CommandOutput out;
    const CaptionTrackId id = r.createdCaptionTrack.isValid() ? r.createdCaptionTrack
                                                              : CaptionTrackId{static_cast<std::uint64_t>(c.integer("replace"))};
    if (r.createdCaptionTrack.isValid()) {
        out.createdIds.push_back(id.value);
    }
    out.state = std::move(r.state);
    out.data = Json{{"captionTrack", id.value}, {"cues", cues}, {"language", language}, {"cuesWithSpeaker", speakers}};
    return out;
}

CommandOutput updateFromTranscript(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    CaptionTrack track = captionTrackParamOf(c);
    const CaptionOptions options = captionOptions(c);
    const std::optional<ProfanityFilter> profanity = profanityOf(c); // p02 audio: Hide profanity
    std::size_t updated = 0;
    for (CaptionCue& cue : track.cues) {
        // The words whose middle lies in the cue, in order.
        std::vector<std::string> words;
        std::vector<SpeakerId> speakers;
        const auto [b, e] = st.wordsIn(TimeRange{cue.start, cue.duration});
        for (std::size_t i = b; i < e; ++i) {
            const TimeRange w = st[i].timeline;
            const Ticks mid = w.start + Ticks{w.duration.value / 2};
            if (mid >= cue.start && mid < cue.end()) {
                words.emplace_back(profanity ? profanity->mask(st.text(i)) : std::string(st.text(i)));
                speakers.push_back(st[i].speakerId);
            }
        }
        if (words.empty()) {
            continue;
        }
        std::string text = captionText(words, track.language, options);
        if (text != cue.text) {
            cue.text = std::move(text);
            ++updated;
        }
    }
    CommandOutput out;
    out.state = updated == 0 ? c.state()
                             : c.take(::wabcut::edit::setCaptionTrack(c.state(), c.sequenceId(), std::move(track))).state;
    out.data = Json{{"updated", updated}};
    return out;
}

// --- p02 audio: profanity (Premiere's Speech to Text "Hide profanity"; Profanity.h) ---------------------------------

CommandOutput hideProfanity(ExecutionContext& c) {
    CaptionTrack track = captionTrackParamOf(c);
    const ProfanityFilter filter = *profanityOf(c, true);
    std::size_t masked = 0;
    for (CaptionCue& cue : track.cues) {
        std::string text = filter.maskText(cue.text);
        if (text != cue.text) {
            cue.text = std::move(text);
            ++masked;
        }
    }
    CommandOutput out;
    out.state = masked == 0 ? c.state()
                            : c.take(::wabcut::edit::setCaptionTrack(c.state(), c.sequenceId(), std::move(track))).state;
    out.data = Json{{"cues", masked}};
    return out;
}

CommandOutput bleepProfanity(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    const ProfanityFilter filter = *profanityOf(c, true);
    const std::string modeName = c.string("mode", "bleep");
    const auto mode = redactionAudioFromString(modeName);
    if (!mode || *mode == RedactionAudio::None) {
        fail(ErrorCode::InvalidParams, "mode is bleep or mute", Json{{"path", "/mode"}});
    }
    const Ticks padding = secondsOr(c, "padding", 0.02);
    const Sequence& seq = c.sequence();
    // The profane words the sequence plays, by the media item they are heard from (words heard through nested or
    // multicam clips are reported, not bleeped: bleep them in their own sequence).
    std::map<std::uint64_t, std::vector<TimeRange>> byMedia;
    Json words = Json::array();
    std::size_t nested = 0;
    for (std::size_t i = 0; i < st.size(); ++i) {
        const TimelineWord& w = st[i];
        if (w.redacted || !filter.isProfane(st.text(i))) {
            continue;
        }
        const auto [media, stream] = streamOf(seq, w);
        const auto loc = seq.findClip(w.clip);
        if (!media.isValid() || !loc || loc->clip().isNested()) {
            ++nested;
            continue;
        }
        const Ticks start = std::max(Ticks{}, w.source.start - padding);
        byMedia[media.value].push_back(TimeRange::fromStartEnd(start, w.source.end() + padding));
        words.push_back(Json{{"word", i}, {"text", filter.mask(st.text(i))}, {"at", c.timecode(w.timeline.start)}});
    }
    ProjectState state = c.state();
    CommandOutput out;
    for (auto& [media, ranges] : byMedia) {
        std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
        std::vector<TimeRange> merged;
        for (const TimeRange& r : ranges) {
            if (!merged.empty() && r.start <= merged.back().end()) {
                merged.back() = TimeRange::fromStartEnd(merged.back().start, std::max(merged.back().end(), r.end()));
            } else if (merged.size() < kMaxRedactionRanges) {
                merged.push_back(r);
            }
        }
        Redaction r;
        r.media = MediaId{media};
        r.name = "Profanity";
        r.audio = *mode;
        r.audioRanges = merged;
        r.source = TimeRange::fromStartEnd(merged.front().start, merged.back().end());
        // The sound only: a region of no size covers no picture (Masks.h: an empty shape covers nothing).
        r.region.shape = MaskShape::Ellipse;
        r.region.set("size.x", 0.0);
        r.region.set("size.y", 0.0);
        auto added = c.take(::wabcut::edit::addRedaction(state, std::move(r)));
        out.createdIds.push_back(added.id.value);
        state = std::move(added.state);
    }
    // Captions over bleeped words follow (refreshRedactedCues, like the redaction commands).
    if (auto* services = command::findService<TranscriptServices>(c.services()); services && services->transcripts && !byMedia.empty()) {
        state = refreshRedactedCues(c.state(), std::move(state), *services->transcripts);
    }
    out.state = std::move(state);
    out.data = Json{{"mode", modeName}, {"words", std::move(words)}, {"redactions", byMedia.size()}, {"skippedNested", nested}};
    return out;
}

CommandOutput correctWords(ExecutionContext& c, std::size_t first, std::size_t last) {
    const SequenceTranscript st = transcriptOf(c);
    if (first > last || last >= st.size()) {
        fail(ErrorCode::InvalidParams, "the sequence has " + std::to_string(st.size()) + " words; [" + std::to_string(first) +
                                           ", " + std::to_string(last) + "] is not a range of them");
    }
    const auto stream = streamOf(c.sequence(), st[first]);
    Ticks start = st[first].source.start;
    Ticks end = st[first].source.end();
    for (std::size_t i = first; i <= last; ++i) {
        if (streamOf(c.sequence(), st[i]) != stream) {
            fail(ErrorCode::InvalidParams, "the words come from different media; correct each part on its own");
        }
        start = std::min(start, st[i].source.start);
        end = std::max(end, st[i].source.end());
    }
    if (end <= start) {
        end = start + Ticks{1};
    }
    WordCorrection correction{TimeRange::fromStartEnd(start, end), st.textOf(first, last), c.string("text")};
    const std::string original = correction.original;
    ProjectState state = c.take(::wabcut::edit::correctTranscript(c.state(), stream.first, stream.second, std::move(correction)));
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"media", stream.first.value}, {"audioStream", stream.second}, {"original", original},
                    {"text", c.string("text")}};
    return out;
}

// --- 0.2: Export transcript, Find and Replace ---------------------------------------------------------------------

/// The speaker line of a paragraph: the project speaker's name, else "Speaker N" from the diarization label (as
/// Premiere names unnamed speakers), else none.
std::string speakerName(ExecutionContext& c, const TimelineWord& w) {
    if (w.speakerId.isValid()) {
        if (const Speaker* s = c.state().findSpeaker(w.speakerId)) {
            return s->name;
        }
    }
    return w.speaker ? "Speaker " + std::to_string(*w.speaker + 1) : std::string();
}

/// A CSV field (RFC 4180): quoted when it holds a comma, a quote or a line break.
std::string csvField(std::string_view s) {
    if (s.find_first_of(",\"\n\r") == std::string_view::npos) {
        return std::string(s);
    }
    std::string out = "\"";
    for (const char ch : s) {
        out += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    }
    return out + "\"";
}

CommandOutput exportTranscript(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    if (st.empty()) {
        fail(ErrorCode::EditRejected, "the sequence has no transcript to export (transcribe its media first)");
    }
    std::string format = c.string("format");
    std::filesystem::path file;
    if (c.has("path")) {
        file = pathFromUtf8(c.string("path"));
        std::string ext = pathToUtf8(file.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (ext != ".txt" && ext != ".csv" && ext != ".docx") {
            fail(ErrorCode::InvalidParams, "a transcript is exported to a .txt, .csv or .docx file", Json{{"path", "/path"}});
        }
        if (format.empty()) {
            format = ext.substr(1);
        }
        std::error_code ec;
        if (std::filesystem::exists(file, ec) && !c.boolean("overwrite", false)) {
            fail(ErrorCode::EditRejected, "the file exists; pass \"overwrite\": true to replace it", Json{{"path", "/path"}});
        }
    }
    if (format.empty()) {
        format = "txt";
    }
    if (format == "docx" && file.empty()) {
        fail(ErrorCode::InvalidParams, "a .docx transcript is written to a file: give \"path\"", Json{{"path", "/format"}});
    }
    const bool speakers = c.boolean("speakers", true);
    std::vector<interchange::DocxParagraph> document;
    const Ticks maxPause = secondsOr(c, "maxPause", 1.0);
    std::string text = format == "csv" ? std::string("Start,End,Speaker,Text\n") : std::string();
    std::size_t paragraphs = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < st.size(); ++i) {
        const bool last = i + 1 == st.size();
        const bool split = last || (st[i + 1].speaker != st[i].speaker) ||
                           st[i + 1].timeline.start - st[i].timeline.end() > maxPause || i + 1 - first >= 80;
        if (!split) {
            continue;
        }
        const std::string start = c.timecode(st[first].timeline.start);
        const std::string end = c.timecode(st[i].timeline.end());
        const std::string who = speakers ? speakerName(c, st[first]) : std::string();
        const std::string words = st.textOf(first, i);
        if (format == "csv") {
            text += csvField(start) + "," + csvField(end) + "," + csvField(who) + "," + csvField(words) + "\n";
        } else if (format == "docx") {
            // The .txt layout as a Word document (Premiere's): the timecodes in grey, the speaker in bold, the text,
            // an empty paragraph between blocks (captions.import reads it back the same way).
            if (paragraphs > 0) {
                document.push_back({});
            }
            document.push_back({{{start + " - " + end, false, true}}});
            if (!who.empty()) {
                document.push_back({{{who, true, false}}});
            }
            document.push_back({{{words, false, false}}});
        } else {
            text += (paragraphs == 0 ? "" : "\n") + start + " - " + end + "\n" + (who.empty() ? "" : who + "\n") + words + "\n";
        }
        ++paragraphs;
        first = i + 1;
    }
    if (format == "docx") {
        text = interchange::writeDocx(document, pathToUtf8(file.stem()));
    }
    if (!file.empty() && !c.dryRun()) {
        if (auto written = ::wabcut::io::writeFileAtomically(file, text); !written) {
            fail(ErrorCode::EditRejected, "cannot write the transcript: " + written.error().message, Json{{"path", "/path"}});
        }
    }
    Json data{{"format", format}, {"paragraphs", paragraphs}, {"words", st.size()}};
    if (file.empty()) {
        data["text"] = text;
    } else {
        data["path"] = pathToUtf8(file);
        data["bytes"] = text.size();
    }
    return readOutput(c, st, std::move(data));
}

/// The matches of "find" in the transcript: each as the words it spans ([first, last], one more word per space in the
/// find text, since words never hold spaces) and how many times it is in them.
struct TranscriptMatch {
    std::size_t first = 0;
    std::size_t last = 0;
};

std::vector<TranscriptMatch> transcriptMatches(const SequenceTranscript& st, std::string_view find, TextFindOptions options) {
    std::vector<TranscriptMatch> out;
    const std::size_t span = static_cast<std::size_t>(std::count(find.begin(), find.end(), ' ')) + 1;
    for (std::size_t i = 0; i + span <= st.size();) {
        const std::size_t last = i + span - 1;
        if (findText(st.textOf(i, last), find, options).empty()) {
            ++i;
            continue;
        }
        out.push_back({i, last});
        i = last + 1;
    }
    return out;
}

CommandOutput findTranscript(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    const std::string find = c.string("find");
    const TextFindOptions options{c.boolean("matchCase", false), c.boolean("wholeWord", false)};
    const std::vector<TranscriptMatch> matches = transcriptMatches(st, find, options);
    Json list = Json::array();
    for (const TranscriptMatch& m : matches) {
        if (list.size() >= 1000) {
            break;
        }
        list.push_back(Json{{"words", Json::array({m.first, m.last})},
                            {"time", c.timecode(st[m.first].timeline.start)},
                            {"text", st.textOf(m.first, m.last)}});
    }
    return readOutput(c, st, Json{{"count", matches.size()}, {"matches", std::move(list)}});
}

CommandOutput findReplaceTranscript(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    const std::string find = c.string("find");
    const std::string replacement = c.string("replace");
    const TextFindOptions options{c.boolean("matchCase", false), c.boolean("wholeWord", false)};
    // The find text spans as many words as it has spaces + 1 (words never hold spaces).
    const std::size_t span = static_cast<std::size_t>(std::count(find.begin(), find.end(), ' ')) + 1;
    // Premiere's Replace (one match): "words" names the match transcript.find listed.
    std::optional<std::size_t> only;
    if (c.has("words")) {
        const Json& range = c.params()["words"];
        const std::size_t first = c.indexValue(range[0], "/words/0");
        const std::size_t last = c.indexValue(range[1], "/words/1");
        if (last + 1 != first + span || last >= st.size() || findText(st.textOf(first, last), find, options).empty()) {
            fail(ErrorCode::NotFound, "\"" + find + "\" is not at words [" + std::to_string(first) + ", " + std::to_string(last) +
                                          "] (the transcript changed?)",
                 Json{{"path", "/words"}});
        }
        only = first;
    }
    ProjectState state = c.state();
    std::size_t replaced = 0;
    std::size_t skipped = 0;
    Json changes = Json::array();
    for (std::size_t i = only.value_or(0); i + span <= st.size();) {
        if (only && i != *only) {
            break;
        }
        const std::size_t last = i + span - 1;
        const std::string before = st.textOf(i, last);
        std::size_t n = 0;
        const std::string after = replaceText(before, find, replacement, options, &n);
        if (n == 0) {
            ++i;
            continue;
        }
        const auto stream = streamOf(c.sequence(), st[i]);
        Ticks start = st[i].source.start;
        Ticks end = st[i].source.end();
        bool oneMedia = true;
        for (std::size_t k = i; k <= last; ++k) {
            oneMedia = oneMedia && streamOf(c.sequence(), st[k]) == stream;
            start = std::min(start, st[k].source.start);
            end = std::max(end, st[k].source.end());
        }
        if (!oneMedia) {
            ++skipped; // a match across two media items: corrections are per media
            i = last + 1;
            continue;
        }
        if (end <= start) {
            end = start + Ticks{1};
        }
        state = c.take(::wabcut::edit::correctTranscript(state, stream.first, stream.second,
                                                         WordCorrection{TimeRange::fromStartEnd(start, end), before, after}));
        replaced += n;
        if (changes.size() < 200) {
            changes.push_back(Json{{"words", Json::array({i, last})}, {"time", c.timecode(st[i].timeline.start)},
                                   {"before", before}, {"after", after}});
        }
        i = last + 1;
    }
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"replaced", replaced}, {"skipped", skipped}, {"changes", std::move(changes)}};
    if (replaced == 0) {
        out.warnings.push_back("\"" + find + "\" was not found in the transcript");
    }
    if (skipped > 0) {
        out.warnings.push_back(std::to_string(skipped) + " match(es) span two media items and were left alone");
    }
    return out;
}

CommandOutput correctWord(ExecutionContext& c) {
    const std::size_t word = c.indexValue(c.params()["word"], "/word");
    return correctWords(c, word, word);
}

CommandOutput correctRange(ExecutionContext& c) {
    const Json& range = c.params()["words"];
    return correctWords(c, c.indexValue(range[0], "/words/0"), c.indexValue(range[1], "/words/1"));
}

std::string hexOf(std::uint32_t color) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%06X", color & 0xFFFFFFu);
    return buf;
}

CommandOutput transcriptSpeakers(ExecutionContext& c) {
    const SequenceTranscript st = transcriptOf(c);
    Json speakers = Json::array();
    std::map<std::uint64_t, std::size_t> wordsBySpeaker;
    std::map<std::tuple<std::uint64_t, std::int32_t, std::int32_t>, std::size_t> labelWords;
    for (const TimelineWord& w : st.words()) {
        if (w.speakerId.isValid()) {
            ++wordsBySpeaker[w.speakerId.value];
        }
        if (w.speaker) {
            const auto [media, stream] = streamOf(c.sequence(), w);
            ++labelWords[{media.value, stream, *w.speaker}];
        }
    }
    for (const Speaker& s : c.state().speakers) {
        speakers.push_back(Json::array({s.id.value, s.name, hexOf(s.color), wordsBySpeaker[s.id.value]}));
    }
    Json labels = Json::array();
    for (const auto& [key, count] : labelWords) {
        const auto& [media, stream, label] = key;
        const TranscriptEdits* edits = c.state().findTranscriptEdits(MediaId{media}, stream);
        const SpeakerId speaker = edits ? edits->speakerOfLabel(label) : SpeakerId{};
        labels.push_back(Json::array({media, stream, label, speaker.isValid() ? Json(speaker.value) : Json(nullptr), count}));
    }
    return readOutput(c, st,
                      Json{{"speakerFields", Json::array({"id", "name", "color", "words"})},
                           {"speakers", std::move(speakers)},
                           {"labelFields", Json::array({"media", "audioStream", "label", "speaker", "words"})},
                           {"labels", std::move(labels)}});
}

CommandOutput transcriptCorrections(ExecutionContext& c) {
    TranscriptServices& services = servicesOf(c);
    Json rows = Json::array();
    for (const TranscriptEdits& e : c.state().transcriptEdits) {
        if (e.corrections.empty()) {
            continue;
        }
        const MediaItem* media = c.state().findMedia(e.media);
        const auto raw = media ? services.transcripts->transcript(*media, e.audioStream) : nullptr;
        std::vector<CorrectionReport> report;
        if (raw) {
            applyCorrections(*raw, e, &report);
        }
        for (std::size_t k = 0; k < e.corrections.size(); ++k) {
            const WordCorrection& w = e.corrections[k];
            const auto it = std::find_if(report.begin(), report.end(), [&](const CorrectionReport& r) { return r.correction == k; });
            // Redacted speech stays private here too (any redaction of the media: no clip to except it).
            const bool redacted = isAudioRedacted(c.state(), e.media, w.source);
            const auto shown = [&](const std::string& text) { return redacted ? std::string(kRedactedWordText) : text; };
            rows.push_back(Json::array({e.media.value, e.audioStream, w.source.start.value, w.source.end().value,
                                        shown(w.original), shown(w.text),
                                        it == report.end() ? Json("noTranscript") : Json(std::string(toString(it->outcome))),
                                        it == report.end() ? Json(nullptr) : Json(shown(it->found))}));
        }
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"correctionFields", Json::array({"media", "audioStream", "start", "end", "original", "text",
                                                       "status", "found"})},
                    {"corrections", std::move(rows)}};
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

void registerTranscriptCommands(command::CommandRegistry& registry) {
    using command::Capability;
    const Json padding = secondsParam("Seconds kept around the cut words (never into the next word). Default 0.04.", 0.0, 2.0);
    add(registry,
        command::runsModel({"transcript.transcribe", 1, "Transcribe Media",
         "Transcribes the audio of media items or clips (default: every audio clip of the sequence) so the other "
         "transcript commands have words. provider: \"auto\" (on this computer when the local engine is built, else "
         "the user's cloud transcription model), \"local\", or \"cloud:<model id>\"; cloud transcription goes through "
         "the user's consent, spend cap and offline / no-cloud locks. Audio that already has a transcript is not "
         "transcribed again unless \"force\". \"check\": true only lists the providers and what has a transcript. "
         "Transcripts are stored per source file (the analysis cache), not in the project.",
         Capability::MediaAnalysis,
         withSequence({{"media", schema::array(mediaRef("A media item."), "Media items to transcribe.", 1)},
                       {"clips", schema::array(clipRef("A clip."), "Clips whose media to transcribe.", 1)},
                       {"provider", schema::string("\"auto\" (default), \"local\" or \"cloud:<model id>\".")},
                       {"language", schema::string("ISO 639-1 language (\"en\", \"th\", ...) or \"auto\" (default).")},
                       {"force", schema::boolean("Transcribe even audio that has a transcript. Default false.", false)},
                       {"check", schema::boolean("Only list providers and which audio has a transcript. Default false.", false)}}),
         Json{{"media", Json::array({"Interview A"})}, {"language", "th"}}, {}, transcribeMedia}));
    add(registry,
        {"transcript.text", 1, "Transcript Text",
         "What the sequence says, in timeline order, as paragraphs (split at speaker changes and pauses) with the "
         "word indices the other transcript commands take. Words inside an audio redaction read \"[bleep]\". Start "
         "here.",
         Capability::Read,
         withSequence({{"from", timeParam("Only paragraphs that end after this time.")},
                       {"to", timeParam("Only paragraphs that start before this time.")},
                       {"maxPause", secondsParam("A longer pause starts a new paragraph. Default 1.", 0.0, 60.0)}}),
         Json::object(), {}, transcriptText});
    add(registry,
        {"transcript.words", 1, "Transcript Words",
         "The sequence's words in timeline order as arrays in wordFields order: index, text, timeline start and "
         "end, the audio clip and track it is heard on, speaker and recognition confidence.",
         Capability::Read,
         withSequence({{"first", wordIndex("First word index.")},
                       {"last", wordIndex("Last word index.")},
                       {"from", timeParam("Only words that end after this time.")},
                       {"to", timeParam("Only words that start before this time.")},
                       {"limit", schema::integer("Most words to return (default 500).", 1, 100000)}}),
         Json{{"first", 0}, {"last", 49}}, {}, transcriptWords});
    // P0 round: the source clip transcript (the Transcript panel's Source mode) for the CLI and MCP.
    add(registry,
        {"transcript.mediaWords", 1, "Media Transcript Words",
         "One media item's words in source time (what the clip says before any edit: the Transcript panel's Source "
         "mode), as arrays in wordFields order: index, text, start and end as source timecodes in the media's rate and "
         "as ticks, speaker and recognition confidence. Corrections of the project apply; words inside an audio "
         "redaction read \"[bleep]\". Pick a soundbite, then source.setMarks with its first word's start and its last "
         "word's end and edit.insertFromSource / overwriteFromSource. audioStream: an index into the media's audio "
         "streams (default: the first one with a transcript).",
         Capability::Read,
         schema::object({{"media", mediaRef("The media item.")},
                         {"audioStream", schema::integer("Audio stream index (0 = the first). Default: the first with a transcript.", 0, 64)},
                         {"first", wordIndex("First word index.")},
                         {"last", wordIndex("Last word index.")},
                         {"from", Json{{"type", Json::array({"integer", "string"})},
                                       {"description", "Only words that end after this source time: ticks or a timecode in the media's rate."}}},
                         {"to", Json{{"type", Json::array({"integer", "string"})},
                                     {"description", "Only words that start before this source time: ticks or a timecode in the media's rate."}}},
                         {"limit", schema::integer("Most words to return (default 500).", 1, 100000)}},
                        {"media"}),
         Json{{"media", 3}, {"from", "00:01:10:00"}, {"to", "00:01:30:00"}}, {}, transcriptMediaWords});
    add(registry,
        {"transcript.review", 1, "Review Fillers and Silences",
         "Filler words (language-aware: um, uh, like in disfluent context; Thai อืม, เอ่อ, แบบว่า ...) and pauses longer "
         "than a threshold, as a review list: kind, word indices, time range, confidence, label and context. Remove "
         "chosen items with transcript.deleteRanges (fillers as words, silences as gaps) or all with transcript.cleanup.",
         Capability::Read, reviewParams(), Json{{"silences", 1.0}}, {}, transcriptReview});
    add(registry,
        {"transcript.deleteRanges", 1, "Delete Words",
         "Deletes words (and pauses) like deleting text: ripple deletes on the tracks the words are heard on and "
         "their linked partners, cut on frame boundaries just outside the words, neighbouring deletions merged, "
         "sync-locked tracks following, locked tracks untouched. One undo step.",
         Capability::EditSequence,
         withSequence({{"words", wordRanges("Word ranges to delete.")},
                       {"gaps", schema::array(wordIndex("Word index."),
                                              "Pauses to remove: the pause after each of these words.")},
                       {"padding", padding},
                       {"roomTone", secondsParam("Seconds of each removed pause kept on both sides. Default 0.15.", 0.0, 5.0)}}),
         Json{{"words", Json::array({Json::array({12, 14}), Json::array({40, 40})})}}, {}, deleteRanges});
    add(registry,
        {"transcript.keepRanges", 1, "Keep Only Words",
         "Rough cut: keeps the given word ranges and ripple-deletes everything else on the transcript's tracks, from "
         "the first to the last clip that has words. One undo step.",
         Capability::EditSequence,
         withSequence({{"words", wordRanges("Word ranges to keep.")}, {"padding", padding}}, {"words"}),
         Json{{"words", Json::array({Json::array({0, 30}), Json::array({95, 140})})}}, {}, keepRanges});
    add(registry,
        {"transcript.moveRange", 1, "Move Words",
         "Moves words (a sentence, a paragraph) to the word boundary before another word: cut out with a ripple and "
         "inserted there, with their linked video. One undo step.",
         Capability::EditSequence,
         withSequence({{"words", wordRange("[first, last] word indices to move.")},
                       {"before", wordIndex("Insert before this word; the word count = after the last word.")},
                       {"padding", padding}},
                      {"words", "before"}),
         Json{{"words", Json::array({60, 85})}, {"before", 10}}, {}, moveRange});
    add(registry,
        {"transcript.cleanup", 1, "Remove Fillers and Silences",
         "Removes every filler word and pause the review finds with at least minConfidence, in one undo step.",
         Capability::EditSequence,
         reviewParams({{"minConfidence", secondsParam("Lowest confidence removed (0..1). Default 0.5.", 0.0, 1.0)},
                       {"padding", padding},
                       {"roomTone", secondsParam("Seconds of each removed pause kept on both sides. Default 0.15.", 0.0, 5.0)}}),
         Json{{"fillers", true}, {"silences", 0.8}}, {}, cleanup});
    // p02 audio: Premiere's "Hide profanity" and its word lists (Profanity.h).
    const Json profanitySwitch =
        schema::boolean("Hide profanity: profane words read as their first letter and asterisks (\"f***\"). Default false.");
    const Json profanityWords = schema::array(schema::string("A word; a trailing * matches words that start with it."),
                                              "Words to treat as profane besides the built-in English and Thai lists.");
    const Json allowedWords = schema::array(schema::string("A word of the built-in lists."), "Words of the built-in lists to allow.");
    add(registry,
        {"captions.generate", 1, "Generate Captions",
         "Makes a caption track from the transcript: cues of at most maxLines lines of maxCharsPerLine characters and "
         "maxDuration seconds, broken at pauses, speaker changes and sentence ends; Thai lines break between words, "
         "never at spaces that are not there. Replaces a caption track when \"replace\" names one.",
         Capability::EditSequence,
         withSequence({{"name", schema::string("Track name. Default \"Captions\".")},
                       {"maxCharsPerLine", schema::integer("Default 42.", 8, 200)},
                       {"maxLines", schema::integer("Default 2.", 1, 4)},
                       {"maxDuration", secondsParam("Longest cue in seconds. Default 7.", 1.0, 60.0)},
                       {"minDuration", secondsParam("Shortest cue in seconds. Default 0.7.", 0.0, 10.0)},
                       {"replace", schema::integer("Caption track id to replace.", 1)},
                       {"hideProfanity", profanitySwitch},
                       {"profanityWords", profanityWords},
                       {"allowedWords", allowedWords}}),
         Json{{"maxCharsPerLine", 37}}, {}, generateCaptions});
    const auto captionSchema = [&](std::initializer_list<std::pair<const char*, Json>> extra) {
        Json s = withSequence({{"maxCharsPerLine", schema::integer("Default 42.", 8, 200)},
                               {"maxLines", schema::integer("Default 2.", 1, 4)},
                               {"maxDuration", secondsParam("Longest cue in seconds. Default 7.", 1.0, 60.0)},
                               {"minDuration", secondsParam("Shortest cue in seconds. Default 0.7.", 0.0, 10.0)},
                               {"maxPause", secondsParam("A longer pause between words starts a new cue. Default 1.", 0.05, 30.0)},
                               {"gap", secondsParam("Least time between two cues in seconds. Default 0.", 0.0, 2.0)},
                               {"splitOnPunctuation",
                                schema::boolean("A sentence end closes a cue once it is half full. Default true.")},
                               {"breakOnSpeakerChange", schema::boolean("A new speaker starts a new cue. Default true.")},
                               {"hideProfanity", profanitySwitch},
                               {"profanityWords", profanityWords},
                               {"allowedWords", allowedWords}});
        for (const auto& [key, value] : extra) {
            s["properties"][key] = value;
        }
        return s;
    };
    add(registry,
        {"captions.generateFromTranscript", 1, "Captions from Transcript",
         "Makes a caption track from the sequence's transcript (with the project's corrections and speakers): cues of "
         "at most maxLines lines of maxCharsPerLine characters and maxDuration, broken at pauses, speaker changes and "
         "(optionally) sentence ends, at least \"gap\" apart. Thai and other unspaced scripts break between words, and "
         "where the host provides line-break rules also inside them. Each cue gets the speaker of most of its words "
         "(diarization labels without a speaker get one). \"replace\" regenerates a caption track and keeps its style.",
         Capability::EditSequence,
         captionSchema({{"name", schema::string("Track name. Default \"Captions\".")},
                        {"language", schema::string("Track language (default: the transcript's).")},
                        {"preset", schema::string("A caption preset for its style (captions.presets).")},
                        {"replace", schema::integer("Caption track id to replace.", 1)},
                        {"speakers", schema::boolean("Name speakers on the cues. Default true.")}}),
         Json{{"maxCharsPerLine", 32}, {"gap", 0.08}, {"preset", "Podcast"}}, {}, generateFromTranscript});
    add(registry,
        {"captions.updateFromTranscript", 1, "Update Captions from Transcript",
         "Rewrites the text of a caption track's cues from the transcript words under them (after transcript "
         "corrections), keeping their timing and style; cues with no words under them stay as they are.",
         Capability::EditSequence,
         captionSchema({{"captionTrack", schema::integer("Caption track id. Default: the first.", 1)}}),
         Json{{"captionTrack", 57}}, {}, updateFromTranscript});
    add(registry,
        {"captions.hideProfanity", 1, "Hide Profanity in Captions",
         "Premiere's Hide profanity on a caption track that exists: every profane word of its cues reads as its first "
         "letter and asterisks (\"f***\"), with the built-in English and Thai lists, \"profanityWords\" added and "
         "\"allowedWords\" taken out. One undo step.",
         Capability::EditSequence,
         withSequence({{"captionTrack", schema::integer("Caption track id. Default: the first.", 1)},
                       {"profanityWords", profanityWords},
                       {"allowedWords", allowedWords}}),
         Json{{"captionTrack", 57}}, {}, hideProfanity});
    add(registry,
        {"audio.bleepProfanity", 1, "Bleep Profanity",
         "Bleeps (a 1 kHz tone) or mutes the profane words the sequence's transcript finds, in the sound: one audio "
         "redaction per media item over those words (with \"padding\" each side), so they stay bleeped in every use of "
         "the media, in preview and export, and captions over them read [bleep]. The picture is untouched. Built-in "
         "English and Thai lists, \"profanityWords\" added, \"allowedWords\" taken out. dryRun lists the words first. "
         "One undo step.",
         Capability::EditProject,
         withSequence({{"mode", schema::enumeration({"bleep", "mute"}, "Bleep (default) or mute.")},
                       {"padding", secondsParam("Seconds added before and after each word. Default 0.02.", 0.0, 1.0)},
                       {"profanityWords", profanityWords},
                       {"allowedWords", allowedWords}}),
         Json{{"mode", "bleep"}}, {}, bleepProfanity});
    Json correctionText = schema::string("The corrected text (UTF-8; words separated by spaces). Empty removes the words.");
    correctionText["maxLength"] = 4096;
    add(registry,
        {"transcript.correctWord", 1, "Correct Word",
         "Fixes a transcription mistake: the word reads \"text\" from now on, in the transcript the project sees and in "
         "captions generated or updated afterwards. The correction is saved with the project on top of the cached "
         "transcript, so transcribing again keeps it.",
         Capability::EditProject,
         withSequence({{"word", wordIndex("Word index (transcript.words).")}, {"text", correctionText}}, {"word", "text"}),
         Json{{"word", 12}, {"text", "Wabcut"}}, {}, correctWord});
    add(registry,
        {"transcript.correctRange", 1, "Correct Words",
         "Fixes several consecutive words at once (they read \"text\", split over them by spaces). Like "
         "transcript.correctWord; the words must come from one media item.",
         Capability::EditProject,
         withSequence({{"words", wordRange("[first, last] word indices, inclusive.")}, {"text", correctionText}},
                      {"words", "text"}),
         Json{{"words", Json::array({12, 14})}, {"text", "open source editor"}}, {}, correctRange});
    add(registry,
        {"transcript.speakers", 1, "Speakers",
         "The project's speakers (id, name, colour, words in this sequence) and the diarization labels of the "
         "sequence's transcripts with the speaker each one is (null: none yet; transcript.renameSpeaker by media and "
         "label names one).",
         Capability::Read, withSequence({}), Json::object(), {}, transcriptSpeakers});
    add(registry,
        {"transcript.corrections", 1, "Transcript Corrections",
         "The project's transcript corrections and how they apply to the current transcripts: applied, conflict (the "
         "words changed since, e.g. after re-transcription; the correction still wins), restored (no words there any "
         "more; the correction brings them back) or noTranscript.",
         Capability::Read, withSequence({}), Json::object(), {}, transcriptCorrections});
    add(registry,
        command::touchesFiles({"transcript.export", 1, "Export Transcript",
         "Premiere's Export transcript as a text file: the sequence's transcript as paragraphs (split at speaker changes "
         "and pauses longer than maxPause) with their start and end timecodes and speaker. \"txt\": blocks of \"start - "
         "end\", the speaker, the text, a blank line between (transcript files captions.import reads back); \"csv\": "
         "Start,End,Speaker,Text rows; \"docx\": the .txt layout as a Word document (timecodes grey, speakers bold), "
         "which captions.import reads back too. Without \"path\" the text is returned (not for docx). Refuses to "
         "replace a file unless \"overwrite\"; a dry run writes nothing.",
         Capability::Read,
         withSequence({{"path", schema::string("A .txt, .csv or .docx file. Default: return the text.")},
                       {"format", schema::enumeration({"txt", "csv", "docx"}, "Default: the file's extension, else txt.")},
                       {"speakers", schema::boolean("Name the speakers. Default true.", true)},
                       {"maxPause", secondsParam("A longer pause starts a new paragraph. Default 1.", 0.0, 60.0)},
                       {"overwrite", schema::boolean("Replace an existing file. Default false.", false)}}),
         Json{{"path", "C:/exports/interview transcript.csv"}}, {}, exportTranscript}, {"path"}));
    {
        Json findSchema = schema::string("The text to find (plain text, not a pattern; spaces separate words).");
        findSchema["minLength"] = 1;
        findSchema["maxLength"] = 1024;
        Json replaceSchema = schema::string("What each match becomes (empty removes the words).");
        replaceSchema["maxLength"] = 4096;
        add(registry,
            {"transcript.find", 1, "Find in Transcript",
             "Premiere's Find for the transcript (the arrows of Find and Replace): every match of \"find\" (within a "
             "word, or across consecutive words when it has spaces) as the words it spans, the time of the first and "
             "their text, in transcript order. \"matchCase\" and \"wholeWord\" as transcript.findReplace. At most "
             "1000 matches.",
             Capability::Read,
             withSequence({{"find", findSchema},
                           {"matchCase", schema::boolean("Match Case. Default false.", false)},
                           {"wholeWord", schema::boolean("Whole Word. Default false.", false)}},
                          {"find"}),
             Json{{"find", "wab cut"}}, {}, findTranscript});
        add(registry,
            {"transcript.findReplace", 1, "Find and Replace in Transcript",
             "Premiere's Find and Replace for the transcript: every match of \"find\" (within a word, or across "
             "consecutive words when it has spaces) is corrected as transcript.correctRange would - saved with the "
             "project on top of the cached transcripts, so captions generated or updated later read it - in one undo "
             "step; with \"words\" (a match transcript.find listed) only that match (Premiere's Replace). "
             "\"matchCase\" and \"wholeWord\" as Premiere's options (without Match Case, Unicode simple case "
             "folding). A dry run lists the changes without making them.",
             Capability::EditProject,
             withSequence({{"find", findSchema},
                           {"replace", replaceSchema},
                           {"matchCase", schema::boolean("Match Case. Default false.", false)},
                           {"wholeWord", schema::boolean("Whole Word. Default false.", false)},
                           {"words", wordRange("Replace only the match on these words: [first, last], inclusive.")}},
                          {"find", "replace"}),
             Json{{"find", "wab cut"}, {"replace", "Wabcut"}}, {}, findReplaceTranscript});
    }
}

namespace {

/// Whitespace-insensitive text equality ("a\nb" == "a b").
bool sameWords(std::string_view a, std::string_view b) {
    const auto squeeze = [](std::string_view s) {
        std::string out;
        bool space = false;
        for (char c : s) {
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                space = !out.empty();
                continue;
            }
            if (space) {
                out.push_back(' ');
                space = false;
            }
            out.push_back(c);
        }
        return out;
    };
    return squeeze(a) == squeeze(b);
}

/// `spelling` in `text` as a whole word (not inside a longer one), from `from`; npos when it is not there.
std::size_t findWord(std::string_view text, std::string_view spelling, std::size_t from) {
    if (spelling.empty()) {
        return std::string_view::npos;
    }
    const auto isWordByte = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || (static_cast<unsigned char>(c) & 0x80); };
    for (std::size_t at = text.find(spelling, from); at != std::string_view::npos; at = text.find(spelling, at + 1)) {
        const bool startOk = at == 0 || !isWordByte(text[at - 1]);
        const std::size_t end = at + spelling.size();
        const bool endOk = end >= text.size() || !isWordByte(text[end]);
        if (startOk && endOk) {
            return at;
        }
    }
    return std::string_view::npos;
}

} // namespace

/// The caption cues over speech that a redaction change touches. A cue made from the transcript (its text is the
/// transcript's words before or after the change) gets its text from the transcript again, so it reads "[bleep]" where
/// the sound bleeps - and the words again once a redaction goes or excepts their clip (wave-4 P1-3 follow-up; burn-in
/// and sidecars mask at render time too). Any other cue - translated, edited, imported - never gets source-language
/// words (review wave 5, A5-P1-5): a newly redacted word becomes "[bleep]" where its recognised spelling is, and when
/// it cannot be found the whole cue does. Cues over untouched speech keep their text.
ProjectState refreshRedactedCues(const ProjectState& before, ProjectState after, const TranscriptProvider& provider) {
    // The media ranges of every redaction that changed, as it was and as it is, at their redaction roots (a subclip or
    // stem hears its source's: model/RedactionMedia.h).
    std::vector<std::pair<MediaId, TimeRange>> changed;
    const auto collect = [&](const ProjectState& state, const Redaction& r) {
        const MediaId root = redactionRoot(state, r.media);
        if (r.audioRanges.empty()) {
            changed.emplace_back(root, r.source);
        }
        for (const TimeRange& w : r.audioRanges) {
            changed.emplace_back(root, w);
        }
    };
    for (const Redaction& r : before.redactions) {
        if (const Redaction* now = after.findRedaction(r.id); !now || !(*now == r)) {
            collect(before, r);
        }
    }
    for (const Redaction& r : after.redactions) {
        if (const Redaction* was = before.findRedaction(r.id); !was || !(*was == r)) {
            collect(after, r);
        }
    }
    if (changed.empty()) {
        return after;
    }
    const CaptionOptions options;
    for (const SequencePtr& seq : std::vector<SequencePtr>(after.sequences)) {
        if (seq->captionTracks.empty()) {
            continue;
        }
        std::unordered_map<std::uint64_t, MediaId> mediaOf; // clip -> the redaction root of its media
        for (const TrackPtr& t : seq->audioTracks) {
            for (const Clip& c : t->clips) {
                mediaOf.emplace(c.id.value, redactionRoot(after, c.media));
            }
        }
        const SequenceTranscript st = SequenceTranscript::build(after, seq->id, provider);
        const SequenceTranscript prior = before.findSequence(seq->id) ? SequenceTranscript::build(before, seq->id, provider) : st;
        // Where the touched words are on the timeline.
        std::vector<TimeRange> touched;
        for (std::size_t i = 0; i < st.size(); ++i) {
            const auto media = mediaOf.find(st[i].clip.value);
            const TimeRange w = st[i].source;
            if (media != mediaOf.end() &&
                std::any_of(changed.begin(), changed.end(), [&](const auto& c) {
                    return c.first == media->second && w.start < c.second.end() && c.second.start < std::max(w.end(), w.start + Ticks{1});
                })) {
                touched.push_back(st[i].timeline);
            }
        }
        if (touched.empty()) {
            continue;
        }
        for (const CaptionTrack& original : seq->captionTracks) {
            CaptionTrack track = original;
            bool updated = false;
            for (CaptionCue& cue : track.cues) {
                const TimeRange span{cue.start, cue.duration};
                if (std::none_of(touched.begin(), touched.end(), [&](const TimeRange& t) {
                        return t.start < span.end() && span.start < std::max(t.end(), t.start + Ticks{1});
                    })) {
                    continue;
                }
                const auto wordsOf = [&cue, &span](const SequenceTranscript& t) {
                    std::vector<std::size_t> out;
                    const auto [b, e] = t.wordsIn(span);
                    for (std::size_t i = b; i < e; ++i) {
                        const TimeRange w = t[i].timeline;
                        const Ticks mid = w.start + Ticks{w.duration.value / 2};
                        if (mid >= cue.start && mid < cue.end()) {
                            out.push_back(i);
                        }
                    }
                    return out;
                };
                const std::vector<std::size_t> now = wordsOf(st);
                if (now.empty()) {
                    continue;
                }
                const std::vector<std::size_t> was = wordsOf(prior);
                std::vector<std::string> nowWords;
                std::vector<std::string> wasWords;
                for (const std::size_t i : now) {
                    nowWords.emplace_back(st.text(i));
                }
                for (const std::size_t i : was) {
                    wasWords.emplace_back(prior.text(i));
                }
                std::string generated = captionText(nowWords, track.language, options);
                if (sameWords(cue.text, captionText(wasWords, track.language, options)) || sameWords(cue.text, generated)) {
                    if (generated != cue.text) { // a cue made from the transcript: its words again, as they are now
                        cue.text = std::move(generated);
                        updated = true;
                    }
                    continue;
                }
                // Not made from the transcript: only the words that became redacted change, by their recognised
                // spelling; a word that cannot be found masks the whole cue (it may be translated or misspelt).
                std::string text = cue.text;
                bool lost = false;
                if (was.size() != now.size()) {
                    lost = std::any_of(now.begin(), now.end(), [&](std::size_t i) { return st[i].redacted; });
                } else {
                    std::size_t cursor = 0;
                    for (std::size_t k = 0; k < now.size(); ++k) {
                        if (!st[now[k]].redacted || prior[was[k]].redacted) {
                            continue;
                        }
                        const std::string spelling(prior.text(was[k]));
                        const std::size_t at = findWord(text, spelling, cursor);
                        if (at == std::string::npos) {
                            lost = true;
                            break;
                        }
                        text.replace(at, spelling.size(), kRedactedWordText);
                        cursor = at + kRedactedWordText.size();
                    }
                }
                if (lost) {
                    text = std::string(kRedactedWordText);
                }
                if (text != cue.text) {
                    cue.text = std::move(text);
                    updated = true;
                }
            }
            if (updated) {
                if (auto r = ::wabcut::edit::setCaptionTrack(after, seq->id, std::move(track))) {
                    after = std::move(r->state);
                }
            }
        }
    }
    return after;
}

namespace {

/// The I.6 mask, tracking and redaction commands, the redaction edits followed by refreshRedactedCues.
void registerMaskCommandsWithCaptions(command::CommandRegistry& registry) {
    command::CommandRegistry masks;
    masks::registerMaskCommands(masks);
    for (const command::CommandSpec& spec : masks.all()) {
        command::CommandSpec copy = spec;
        if (copy.name.starts_with("redaction.") && copy.mutates()) {
            copy.execute = [inner = spec.execute](ExecutionContext& c) {
                command::CommandOutput out = inner(c);
                if (auto* services = command::findService<TranscriptServices>(c.services());
                    services && services->transcripts) {
                    out.state = refreshRedactedCues(c.state(), std::move(out.state), *services->transcripts);
                }
                return out;
            };
        }
        [[maybe_unused]] const auto added = registry.add(std::move(copy));
    }
}

} // namespace

const command::CommandRegistry& editingCommands() {
    static const command::CommandRegistry registry = [] {
        command::CommandRegistry r = command::builtinCommands();
        registerTranscriptCommands(r);
        generation::registerGenerationCommands(r);
        analysis::registerAnalysisCommands(r);
        audio::edit::registerAudioCommands(r); // AI-4: roles, track/master effects, loudness, auto-ducking
        search::registerSearchCommands(r);     // AI-8: search.footage, search.indexMedia
        color::registerColorCommands(r);       // I.5: grade, Auto, Color Match, LUTs, CDL, scopes
        registerMaskCommandsWithCaptions(r);   // I.6: masks, tracking, source-anchored redactions (captions follow)
        reframe::registerReframeCommands(r);   // AI-9: analysis.subjects, reframe.createSequence, reframe.rerun
        shorts::registerShortsCommands(r);     // AI-9: shorts.find, shorts.create
        roughcut::registerRoughCutCommands(r); // AI-11: roughcut.fromScript, roughcut.swapTake, takes.find, takes.use
        autozoom::registerAutoZoomCommands(r); // AI-11: autozoom.apply
        translate::registerTranslateCommands(r); // AI-12: captions.translate
        dub::registerDubCommands(r);             // AI-12: dub.create, narration.create
        multicam::registerMulticamAiCommands(r); // AI-13: analysis.speakers, multicam.autoSwitch
        workflow::registerWorkflowCommands(r);   // AI-13: workflow.podcast
        segment::registerObjectMaskCommands(r);  // AI-14: mask.objectSelect, mask.refine, mask.propagate
        segment::registerFaceRedactionCommands(r); // 0.2: redaction.detectFaces
        highlights::registerHighlightCommands(r); // G.2: highlights.find, highlights.toReel, highlights.toShort
        stabilize::registerStabilizeCommands(r);  // P.2: effect.analyzeStabilization, effect.analyzeRollingShutter
        return r;
    }();
    return registry;
}

} // namespace wabcut::ai::edit
