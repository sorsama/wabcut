#include <wabcut/ai/analysis/AnalysisJson.h>

#include <nlohmann/json.hpp>

namespace wabcut::ai::analysis {

using nlohmann::json;

namespace {

Error parseError(std::string message) { return Error{ErrorCode::ParseError, std::move(message)}; }

std::expected<json, Error> parseSchema(std::string_view text, std::string_view schema) {
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return std::unexpected(parseError("analysis result is not a JSON object"));
    }
    if (doc.value("schema", std::string()) != schema || doc.value("version", 0) != 1) {
        return std::unexpected(parseError("not a " + std::string(schema) + " v1 document"));
    }
    return doc;
}

} // namespace

std::string toJson(const ShotAnalysis& shots, int indent) {
    json cuts = json::array();
    for (const ShotCut& c : shots.cuts) {
        cuts.push_back(json{{"t", c.time.value},
                            {"frame", c.frame},
                            {"kind", std::string(toString(c.kind))},
                            {"confidence", c.confidence}});
    }
    return json{{"schema", "wabcut.shots"},
                {"version", 1},
                {"detector", shots.detector},
                {"frames", shots.frames},
                {"start", shots.start.value},
                {"end", shots.end.value},
                {"cuts", std::move(cuts)}}
        .dump(indent);
}

std::string toJson(const BeatAnalysis& beats, int indent) {
    json list = json::array();
    for (const Ticks t : beats.beats) {
        list.push_back(t.value);
    }
    json downbeats = json::array();
    for (const Ticks t : beats.downbeats) {
        downbeats.push_back(t.value);
    }
    return json{{"schema", "wabcut.beats"},
                {"version", 1},
                {"tracker", beats.tracker},
                {"bpm", beats.bpm},
                {"confidence", beats.confidence},
                {"beatsPerBar", beats.beatsPerBar},
                {"duration", beats.duration.value},
                {"beats", std::move(list)},
                {"downbeats", std::move(downbeats)}}
        .dump(indent);
}

std::expected<ShotAnalysis, Error> shotsFromJson(std::string_view text) {
    auto doc = parseSchema(text, "wabcut.shots");
    if (!doc) {
        return std::unexpected(doc.error());
    }
    try {
        ShotAnalysis out;
        out.detector = doc->at("detector").get<std::string>();
        out.frames = doc->at("frames").get<std::int64_t>();
        out.start = Ticks{doc->at("start").get<std::int64_t>()};
        out.end = Ticks{doc->at("end").get<std::int64_t>()};
        for (const json& c : doc->at("cuts")) {
            const auto kind = cutKindFromString(c.at("kind").get<std::string>());
            if (!kind) {
                return std::unexpected(parseError("unknown cut kind"));
            }
            out.cuts.push_back(ShotCut{Ticks{c.at("t").get<std::int64_t>()}, c.at("frame").get<std::int64_t>(), *kind,
                                       c.at("confidence").get<float>()});
        }
        return out;
    } catch (const json::exception& e) {
        return std::unexpected(parseError(std::string("shots: ") + e.what()));
    }
}

std::expected<BeatAnalysis, Error> beatsFromJson(std::string_view text) {
    auto doc = parseSchema(text, "wabcut.beats");
    if (!doc) {
        return std::unexpected(doc.error());
    }
    try {
        BeatAnalysis out;
        out.tracker = doc->at("tracker").get<std::string>();
        out.bpm = doc->at("bpm").get<double>();
        out.confidence = doc->at("confidence").get<float>();
        out.beatsPerBar = doc->at("beatsPerBar").get<int>();
        out.duration = Ticks{doc->at("duration").get<std::int64_t>()};
        for (const json& t : doc->at("beats")) {
            out.beats.push_back(Ticks{t.get<std::int64_t>()});
        }
        for (const json& t : doc->at("downbeats")) {
            out.downbeats.push_back(Ticks{t.get<std::int64_t>()});
        }
        return out;
    } catch (const json::exception& e) {
        return std::unexpected(parseError(std::string("beats: ") + e.what()));
    }
}

} // namespace wabcut::ai::analysis
