#include <wabcut/ai/TranscriptJson.h>

#include "JsonUtil.h"

#include <nlohmann/json.hpp>

namespace wabcut::ai {

using nlohmann::json;

namespace {

json wordToJson(const TranscriptWord& w) {
    json j{{"text", w.text}, {"t0", w.t0.value}, {"t1", w.t1.value}, {"confidence", w.confidence},
           {"edited", w.edited}, {"approved", w.approved}};
    if (w.speakerId) {
        j["speaker"] = *w.speakerId;
    }
    return j;
}

json segmentToJson(const TranscriptSegment& s) {
    json j{{"t0", s.t0.value}, {"t1", s.t1.value}, {"firstWord", s.firstWord}, {"wordCount", s.wordCount},
           {"text", s.text}};
    if (s.speakerId) {
        j["speaker"] = *s.speakerId;
    }
    return j;
}

} // namespace

std::string toJson(const Transcript& t, int indent) {
    json source{{"fingerprint", detail::hex64(t.source.fingerprint)}, {"stream", t.source.streamIndex}};
    if (!t.source.path.empty()) {
        source["path"] = t.source.path;
    }
    json params = json::object();
    for (const auto& [key, value] : t.provenance.params) {
        params[key] = value;
    }
    json words = json::array();
    for (const TranscriptWord& w : t.words) {
        words.push_back(wordToJson(w));
    }
    json segments = json::array();
    for (const TranscriptSegment& s : t.segments) {
        segments.push_back(segmentToJson(s));
    }
    const json doc{{"schema", kTranscriptSchema},
                   {"version", Transcript::kSchemaVersion},
                   {"ticksPerSecond", kTicksPerSecond},
                   {"language", t.language},
                   {"source", std::move(source)},
                   {"provenance",
                    {{"engine", t.provenance.engine},
                     {"engineVersion", t.provenance.engineVersion},
                     {"modelId", t.provenance.modelId},
                     {"modelVersion", t.provenance.modelVersion},
                     {"params", std::move(params)},
                     {"createdAt", t.provenance.createdAt}}},
                   {"segments", std::move(segments)},
                   {"words", std::move(words)}};
    return doc.dump(indent, ' ', false, json::error_handler_t::replace);
}

std::expected<Transcript, Error> transcriptFromJson(std::string_view text) {
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return std::unexpected(Error{ErrorCode::ParseError, "transcript: not a JSON object"});
    }
    try {
        if (doc.value("schema", std::string{}) != kTranscriptSchema) {
            return std::unexpected(Error{ErrorCode::VersionMismatch, "transcript: not a wabcut.transcript document"});
        }
        const int version = doc.at("version").get<int>();
        if (version != Transcript::kSchemaVersion) {
            return std::unexpected(Error{ErrorCode::VersionMismatch,
                                         "transcript: schema version " + std::to_string(version) +
                                             " is not supported (this build reads version " +
                                             std::to_string(Transcript::kSchemaVersion) + ")"});
        }
        if (doc.at("ticksPerSecond").get<std::int64_t>() != kTicksPerSecond) {
            return std::unexpected(Error{ErrorCode::VersionMismatch, "transcript: unexpected tick rate"});
        }
        Transcript t;
        t.language = doc.at("language").get<std::string>();
        const json& source = doc.at("source");
        const auto fingerprint = detail::parseHex64(source.at("fingerprint").get<std::string>());
        if (!fingerprint) {
            return std::unexpected(Error{ErrorCode::ParseError, "transcript: bad source fingerprint"});
        }
        t.source.fingerprint = *fingerprint;
        t.source.streamIndex = source.at("stream").get<int>();
        t.source.path = source.value("path", std::string{});

        const json& p = doc.at("provenance");
        t.provenance.engine = p.at("engine").get<std::string>();
        t.provenance.engineVersion = p.at("engineVersion").get<std::string>();
        t.provenance.modelId = p.at("modelId").get<std::string>();
        t.provenance.modelVersion = p.at("modelVersion").get<std::string>();
        t.provenance.createdAt = p.at("createdAt").get<std::string>();
        for (const auto& [key, value] : p.at("params").items()) {
            t.provenance.params[key] = value.get<std::string>();
        }

        for (const json& w : doc.at("words")) {
            TranscriptWord word;
            word.text = w.at("text").get<std::string>();
            word.t0 = Ticks{w.at("t0").get<std::int64_t>()};
            word.t1 = Ticks{w.at("t1").get<std::int64_t>()};
            word.confidence = w.at("confidence").get<float>();
            if (w.contains("speaker")) {
                word.speakerId = w.at("speaker").get<int>();
            }
            word.edited = w.value("edited", false);
            word.approved = w.value("approved", false);
            t.words.push_back(std::move(word));
        }
        for (const json& s : doc.at("segments")) {
            TranscriptSegment segment;
            segment.t0 = Ticks{s.at("t0").get<std::int64_t>()};
            segment.t1 = Ticks{s.at("t1").get<std::int64_t>()};
            segment.firstWord = s.at("firstWord").get<std::size_t>();
            segment.wordCount = s.at("wordCount").get<std::size_t>();
            segment.text = s.at("text").get<std::string>();
            if (s.contains("speaker")) {
                segment.speakerId = s.at("speaker").get<int>();
            }
            t.segments.push_back(std::move(segment));
        }
        if (auto valid = t.validate(); !valid) {
            return std::unexpected(valid.error());
        }
        return t;
    } catch (const json::exception& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string("transcript: ") + e.what()});
    }
}

} // namespace wabcut::ai
