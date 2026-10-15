#include <wabcut/ai/InferProtocol.h>

#include <nlohmann/json.hpp>

namespace wabcut::ai::infer {

using nlohmann::json;

namespace {

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

json pcmToJson(const PcmRange& pcm) {
    return {{"path", pathToUtf8(pcm.path)}, {"offset", pcm.offset}, {"count", pcm.count}};
}

PcmRange pcmFromJson(const json& j) {
    PcmRange pcm;
    pcm.path = pathFromUtf8(j.at("path").get<std::string>());
    pcm.offset = j.at("offset").get<std::int64_t>();
    pcm.count = j.at("count").get<std::int64_t>();
    if (pcm.offset < 0 || pcm.count < 0) {
        throw std::invalid_argument("negative PCM range");
    }
    return pcm;
}

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

template <class T, class Decode>
std::expected<T, Error> parseWith(std::string_view line, Decode&& decode) {
    const json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::unexpected(Error{ErrorCode::ParseError, "wabcut-infer protocol: not a JSON object"});
    }
    try {
        return decode(j, j.at("type").get<std::string>());
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string("wabcut-infer protocol: ") + e.what()});
    }
}

} // namespace

std::string toHex(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out += kDigits[b >> 4];
        out += kDigits[b & 0x0F];
    }
    return out;
}

std::optional<std::string> fromHex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = digit(hex[i]);
        const int lo = digit(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

std::string encode(const Request& request) {
    return dump(std::visit(
        [](const auto& r) -> json {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, Hello>) {
                return {{"type", "hello"}, {"protocol", r.protocol}};
            } else if constexpr (std::is_same_v<T, LoadModel>) {
                return {{"type", "load_model"}, {"id", r.id}, {"path", pathToUtf8(r.path)}, {"useGpu", r.useGpu},
                        {"gpuDevice", r.gpuDevice}};
            } else if constexpr (std::is_same_v<T, LoadVad>) {
                return {{"type", "load_vad"}, {"id", r.id}, {"path", pathToUtf8(r.path)}};
            } else if constexpr (std::is_same_v<T, VadRequest>) {
                return {{"type", "vad"},           {"id", r.id},
                        {"pcm", pcmToJson(r.pcm)}, {"threshold", r.threshold},
                        {"minSpeechMs", r.minSpeechMs}, {"minSilenceMs", r.minSilenceMs},
                        {"padMs", r.padMs}};
            } else if constexpr (std::is_same_v<T, TranscribeRequest>) {
                return {{"type", "transcribe"}, {"id", r.id},           {"pcm", pcmToJson(r.pcm)},
                        {"language", r.language}, {"threads", r.threads}, {"beamSize", r.beamSize},
                        {"vad", r.vad},           {"initialPrompt", r.initialPrompt}};
            } else if constexpr (std::is_same_v<T, Cancel>) {
                return {{"type", "cancel"}, {"id", r.id}};
            } else {
                return {{"type", "shutdown"}};
            }
        },
        request));
}

std::string encode(const Response& response) {
    return dump(std::visit(
        [](const auto& r) -> json {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, HelloReply>) {
                json devices = json::array();
                for (const DeviceInfo& d : r.devices) {
                    devices.push_back({{"name", d.name}, {"description", d.description}, {"kind", d.kind}});
                }
                return {{"type", "hello"}, {"protocol", r.protocol}, {"engine", r.engine},
                        {"engineVersion", r.engineVersion}, {"devices", std::move(devices)}};
            } else if constexpr (std::is_same_v<T, Loaded>) {
                return {{"type", "loaded"}, {"id", r.id}, {"device", r.device}, {"info", r.info}};
            } else if constexpr (std::is_same_v<T, Progress>) {
                return {{"type", "progress"}, {"id", r.id}, {"percent", r.percent}};
            } else if constexpr (std::is_same_v<T, Segment>) {
                json tokens = json::array();
                for (const RecognizedToken& t : r.segment.tokens) {
                    tokens.push_back({{"b", toHex(t.bytes)}, {"t0", t.t0}, {"t1", t.t1}, {"p", t.probability}});
                }
                return {{"type", "segment"},
                        {"id", r.id},
                        {"language", r.language},
                        {"t0", r.segment.t0},
                        {"t1", r.segment.t1},
                        {"speakerTurnNext", r.segment.speakerTurnNext},
                        {"tokens", std::move(tokens)}};
            } else if constexpr (std::is_same_v<T, VadResult>) {
                json segments = json::array();
                for (const auto& [t0, t1] : r.segments) {
                    segments.push_back(json::array({t0, t1}));
                }
                return {{"type", "vad_result"}, {"id", r.id}, {"segments", std::move(segments)}};
            } else if constexpr (std::is_same_v<T, Result>) {
                json stats = json::object();
                for (const auto& [k, v] : r.stats) {
                    stats[k] = v;
                }
                return {{"type", "result"}, {"id", r.id}, {"language", r.language}, {"segments", r.segments},
                        {"stats", std::move(stats)}};
            } else {
                return {{"type", "error"}, {"id", r.id}, {"code", r.code}, {"message", r.message}};
            }
        },
        response));
}

std::expected<Request, Error> decodeRequest(std::string_view line) {
    return parseWith<Request>(line, [](const json& j, const std::string& type) -> std::expected<Request, Error> {
        if (type == "hello") {
            return Hello{j.value("protocol", 0)};
        }
        if (type == "load_model") {
            return LoadModel{j.at("id").get<std::string>(), pathFromUtf8(j.at("path").get<std::string>()),
                             j.value("useGpu", true), j.value("gpuDevice", 0)};
        }
        if (type == "load_vad") {
            return LoadVad{j.at("id").get<std::string>(), pathFromUtf8(j.at("path").get<std::string>())};
        }
        if (type == "vad") {
            VadRequest r;
            r.id = j.at("id").get<std::string>();
            r.pcm = pcmFromJson(j.at("pcm"));
            r.threshold = j.value("threshold", r.threshold);
            r.minSpeechMs = j.value("minSpeechMs", r.minSpeechMs);
            r.minSilenceMs = j.value("minSilenceMs", r.minSilenceMs);
            r.padMs = j.value("padMs", r.padMs);
            return r;
        }
        if (type == "transcribe") {
            TranscribeRequest r;
            r.id = j.at("id").get<std::string>();
            r.pcm = pcmFromJson(j.at("pcm"));
            r.language = j.value("language", r.language);
            r.threads = j.value("threads", r.threads);
            r.beamSize = j.value("beamSize", r.beamSize);
            r.vad = j.value("vad", r.vad);
            r.initialPrompt = j.value("initialPrompt", r.initialPrompt);
            return r;
        }
        if (type == "cancel") {
            return Cancel{j.value("id", std::string{})};
        }
        if (type == "shutdown") {
            return Shutdown{};
        }
        return std::unexpected(Error{ErrorCode::ParseError, "wabcut-infer protocol: unknown request '" + type + "'"});
    });
}

std::expected<Response, Error> decodeResponse(std::string_view line) {
    return parseWith<Response>(line, [](const json& j, const std::string& type) -> std::expected<Response, Error> {
        if (type == "hello") {
            HelloReply r;
            r.protocol = j.at("protocol").get<int>();
            r.engine = j.value("engine", std::string{});
            r.engineVersion = j.value("engineVersion", std::string{});
            for (const json& d : j.value("devices", json::array())) {
                r.devices.push_back({d.value("name", std::string{}), d.value("description", std::string{}),
                                     d.value("kind", std::string{})});
            }
            return r;
        }
        if (type == "loaded") {
            return Loaded{j.at("id").get<std::string>(), j.value("device", std::string{}), j.value("info", std::string{})};
        }
        if (type == "progress") {
            return Progress{j.at("id").get<std::string>(), j.at("percent").get<int>()};
        }
        if (type == "segment") {
            Segment s;
            s.id = j.at("id").get<std::string>();
            s.language = j.value("language", std::string{});
            s.segment.t0 = j.at("t0").get<std::int64_t>();
            s.segment.t1 = j.at("t1").get<std::int64_t>();
            s.segment.speakerTurnNext = j.value("speakerTurnNext", false);
            for (const json& t : j.at("tokens")) {
                const auto bytes = fromHex(t.at("b").get<std::string>());
                if (!bytes) {
                    return std::unexpected(Error{ErrorCode::ParseError, "wabcut-infer protocol: bad token bytes"});
                }
                s.segment.tokens.push_back({*bytes, t.at("t0").get<std::int64_t>(), t.at("t1").get<std::int64_t>(),
                                            t.value("p", 0.0f)});
            }
            return s;
        }
        if (type == "vad_result") {
            VadResult r;
            r.id = j.at("id").get<std::string>();
            for (const json& s : j.at("segments")) {
                r.segments.emplace_back(s.at(0).get<std::int64_t>(), s.at(1).get<std::int64_t>());
            }
            return r;
        }
        if (type == "result") {
            Result r;
            r.id = j.at("id").get<std::string>();
            r.language = j.value("language", std::string{});
            r.segments = j.value("segments", 0);
            // A named copy: items() refers to the object, and a temporary in the loop header would be gone (GCC 14).
            const json stats = j.value("stats", json::object());
            for (const auto& [k, v] : stats.items()) {
                if (v.is_number()) {
                    r.stats[k] = v.get<double>();
                }
            }
            return r;
        }
        if (type == "error") {
            return Failure{j.value("id", std::string{}), j.value("code", std::string{"error"}),
                           j.value("message", std::string{})};
        }
        return std::unexpected(Error{ErrorCode::ParseError, "wabcut-infer protocol: unknown response '" + type + "'"});
    });
}

const std::string& responseId(const Response& response) {
    static const std::string kNone;
    return std::visit(
        [](const auto& r) -> const std::string& {
            if constexpr (std::is_same_v<std::decay_t<decltype(r)>, HelloReply>) {
                return kNone;
            } else {
                return r.id;
            }
        },
        response);
}

} // namespace wabcut::ai::infer
