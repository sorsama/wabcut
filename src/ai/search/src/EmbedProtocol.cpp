#include <wabcut/ai/search/EmbedProtocol.h>

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace wabcut::ai::search::embed {

namespace {

using Json = nlohmann::json;

std::unexpected<Error> bad(std::string message) {
    return std::unexpected(Error(ErrorCode::ParseError, "wabcut-embed protocol: " + std::move(message)));
}

std::string dump(const Json& j) {
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

bool imageBounds(int width, int height, int count) {
    return width > 0 && height > 0 && width <= kMaxImageSide && height <= kMaxImageSide && count > 0 &&
           count <= kMaxImagesPerRequest;
}

bool audioBounds(int channels, int frames, int rate) {
    return channels > 0 && channels <= kMaxAudioChannels && frames > 0 && frames <= kMaxAudioFrames && rate >= 8000 &&
           rate <= 192000;
}

/// A picture of the AI video effects: both sides within bounds and the area too.
bool pictureBounds(int width, int height) {
    return width > 0 && height > 0 && width <= kMaxPictureSide && height <= kMaxPictureSide &&
           static_cast<std::int64_t>(width) * height <= kMaxPicturePixels;
}

Json audioJson(const AudioBlock& a) {
    return Json{{"path", pathToUtf8(a.path)}, {"channels", a.channels}, {"frames", a.frames}, {"rate", a.rate}};
}

} // namespace

// --- Encoding -----------------------------------------------------------------------------------------------------

std::string encode(const Request& request) {
    return std::visit(
        [](const auto& r) -> std::string {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, Hello>) {
                return dump(Json{{"type", "hello"}, {"protocol", r.protocol}});
            } else if constexpr (std::is_same_v<T, Load>) {
                Json j{{"type", "load"}, {"id", r.id}, {"device", r.device}, {"threads", r.threads}};
                if (!r.vision.empty()) {
                    j["vision"] = pathToUtf8(r.vision);
                }
                if (!r.text.empty()) {
                    j["text"] = pathToUtf8(r.text);
                }
                if (!r.tokenizer.empty()) {
                    j["tokenizer"] = pathToUtf8(r.tokenizer);
                }
                if (!r.faces.empty()) {
                    j["faces"] = pathToUtf8(r.faces);
                }
                if (!r.objects.empty()) {
                    j["objects"] = pathToUtf8(r.objects);
                }
                if (!r.speech.empty()) {
                    j["speech"] = pathToUtf8(r.speech);
                }
                if (!r.stems.empty()) {
                    j["stems"] = pathToUtf8(r.stems);
                }
                if (!r.translation.empty()) {
                    j["translation"] = pathToUtf8(r.translation);
                }
                if (!r.segmentation.empty()) {
                    j["segmentation"] = pathToUtf8(r.segmentation);
                }
                if (!r.pictures.empty()) {
                    Json models = Json::array();
                    for (const PictureModelFile& m : r.pictures) {
                        models.push_back(Json{{"key", m.key}, {"path", pathToUtf8(m.path)}});
                    }
                    j["pictures"] = std::move(models);
                }
                return dump(j);
            } else if constexpr (std::is_same_v<T, EmbedImages>) {
                return dump(Json{{"type", "embed_images"},
                                 {"id", r.id},
                                 {"images", Json{{"path", pathToUtf8(r.images.path)},
                                                 {"width", r.images.width},
                                                 {"height", r.images.height},
                                                 {"count", r.images.count}}}});
            } else if constexpr (std::is_same_v<T, EmbedText>) {
                return dump(Json{{"type", "embed_text"}, {"id", r.id}, {"texts", r.texts}});
            } else if constexpr (std::is_same_v<T, Detect>) {
                return dump(Json{{"type", "detect"},
                                 {"id", r.id},
                                 {"images", Json{{"path", pathToUtf8(r.images.path)},
                                                 {"width", r.images.width},
                                                 {"height", r.images.height},
                                                 {"count", r.images.count}}},
                                 {"faces", r.faces},
                                 {"objects", r.objects},
                                 {"faceThreshold", r.faceThreshold},
                                 {"objectThreshold", r.objectThreshold},
                                 {"allClasses", r.allClasses}});
            } else if constexpr (std::is_same_v<T, EnhanceAudio>) {
                return dump(Json{{"type", "enhance_audio"}, {"id", r.id}, {"audio", audioJson(r.audio)}, {"output", pathToUtf8(r.output)}});
            } else if constexpr (std::is_same_v<T, SeparateAudio>) {
                return dump(Json{{"type", "separate_audio"}, {"id", r.id}, {"audio", audioJson(r.audio)}, {"output", pathToUtf8(r.output)}});
            } else if constexpr (std::is_same_v<T, TranslateText>) {
                return dump(Json{{"type", "translate_text"}, {"id", r.id}, {"texts", r.texts}, {"target", r.target}, {"maxTokens", r.maxTokens}});
            } else if constexpr (std::is_same_v<T, SegmentFrame>) {
                Json points = Json::array();
                for (const SegmentPoint& p : r.points) {
                    points.push_back(Json::array({p.x, p.y, p.label}));
                }
                return dump(Json{{"type", "segment_frame"},
                                 {"id", r.id},
                                 {"mode", r.mode},
                                 {"images", Json{{"path", pathToUtf8(r.images.path)},
                                                 {"width", r.images.width},
                                                 {"height", r.images.height},
                                                 {"count", r.images.count}}},
                                 {"frame", r.frame},
                                 {"frames", r.frames},
                                 {"points", std::move(points)},
                                 {"output", pathToUtf8(r.output)},
                                 {"width", r.width},
                                 {"height", r.height}});
            } else if constexpr (std::is_same_v<T, ProcessPicture>) {
                Json message{{"type", "process_picture"},
                             {"id", r.id},
                             {"model", r.model},
                             {"input", Json{{"path", pathToUtf8(r.input.path)}, {"width", r.input.width}, {"height", r.input.height}}},
                             {"output", pathToUtf8(r.output)},
                             {"scale", r.scale},
                             {"tile", r.tile},
                             {"pad", r.pad},
                             {"sigma", r.sigma}};
                if (!r.input2.path.empty()) { // 0.7: frame interpolation
                    message["input2"] = Json{{"path", pathToUtf8(r.input2.path)}, {"width", r.input2.width}, {"height", r.input2.height}};
                    message["timestep"] = r.timestep;
                }
                return dump(message);
            } else {
                return dump(Json{{"type", "shutdown"}});
            }
        },
        request);
}

std::string encode(const Response& response) {
    return std::visit(
        [](const auto& r) -> std::string {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, HelloReply>) {
                return dump(Json{{"type", "hello"},
                                 {"protocol", r.protocol},
                                 {"engine", r.engine},
                                 {"engineVersion", r.engineVersion},
                                 {"providers", r.providers}});
            } else if constexpr (std::is_same_v<T, Loaded>) {
                return dump(Json{{"type", "loaded"}, {"id", r.id}, {"device", r.device}, {"dimension", r.dimension}});
            } else if constexpr (std::is_same_v<T, Embeddings>) {
                Json vectors = Json::array();
                for (const Vector& v : r.vectors) {
                    vectors.push_back(encodeVector(v));
                }
                return dump(Json{{"type", "embeddings"}, {"id", r.id}, {"vectors", std::move(vectors)}, {"ms", r.milliseconds}});
            } else if constexpr (std::is_same_v<T, Detections>) {
                Json frames = Json::array();
                for (const DetectedFrame& f : r.frames) {
                    frames.push_back(Json{{"faces", f.faces}, {"objects", f.objects}});
                }
                return dump(Json{{"type", "detections"}, {"id", r.id}, {"frames", std::move(frames)}, {"ms", r.milliseconds}});
            } else if constexpr (std::is_same_v<T, AudioDone>) {
                return dump(Json{{"type", "audio"},
                                 {"id", r.id},
                                 {"frames", r.frames},
                                 {"channels", r.channels},
                                 {"outputs", r.outputs},
                                 {"ms", r.milliseconds},
                                 {"device", r.device}});
            } else if constexpr (std::is_same_v<T, Translations>) {
                return dump(Json{{"type", "translations"}, {"id", r.id}, {"texts", r.texts}, {"tokens", r.tokens}, {"ms", r.milliseconds},
                                 {"device", r.device}});
            } else if constexpr (std::is_same_v<T, Segmented>) {
                return dump(Json{{"type", "segmented"},
                                 {"id", r.id},
                                 {"iou", r.iou},
                                 {"objectScore", r.objectScore},
                                 {"present", r.present},
                                 {"width", r.width},
                                 {"height", r.height},
                                 {"ms", r.milliseconds},
                                 {"device", r.device}});
            } else if constexpr (std::is_same_v<T, PictureDone>) {
                return dump(Json{{"type", "picture"},
                                 {"id", r.id},
                                 {"width", r.width},
                                 {"height", r.height},
                                 {"tiles", r.tiles},
                                 {"ms", r.milliseconds},
                                 {"device", r.device}});
            } else {
                return dump(Json{{"type", "error"}, {"id", r.id}, {"code", r.code}, {"message", r.message}});
            }
        },
        response);
}

// --- Decoding -----------------------------------------------------------------------------------------------------

namespace {

std::expected<Json, Error> parseLine(std::string_view line) {
    Json j = Json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return bad("not a JSON object");
    }
    if (!jsonString(j, "type")) {
        return bad("no \"type\"");
    }
    return j;
}

std::optional<std::string> requiredId(const Json& j) {
    auto id = jsonString(j, "id");
    if (!id || id->empty() || id->size() > 64) {
        return std::nullopt;
    }
    return id;
}

std::filesystem::path optionalPath(const Json& j, std::string_view key) {
    const auto s = jsonString(j, key);
    return s && !s->empty() ? pathFromUtf8(*s) : std::filesystem::path{};
}

std::optional<AudioBlock> audioBlock(const Json& j) {
    const Json* audio = jsonObject(j, "audio");
    if (!audio) {
        return std::nullopt;
    }
    AudioBlock a;
    a.path = optionalPath(*audio, "path");
    a.channels = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(*audio, "channels", 0), 0, 1 << 20));
    a.frames = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(*audio, "frames", 0), 0, 1 << 30));
    a.rate = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(*audio, "rate", 0), 0, 1 << 20));
    if (a.path.empty() || !audioBounds(a.channels, a.frames, a.rate)) {
        return std::nullopt;
    }
    return a;
}

} // namespace

std::expected<Request, Error> decodeRequest(std::string_view line) {
    auto parsed = parseLine(line);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const Json& j = *parsed;
    const std::string type = *jsonString(j, "type");
    if (type == "hello") {
        const auto protocol = jsonInt(j, "protocol");
        if (!protocol) {
            return bad("hello without a protocol");
        }
        return Hello{static_cast<int>(*protocol)};
    }
    if (type == "shutdown") {
        return Shutdown{};
    }
    const auto id = requiredId(j);
    if (!id) {
        return bad(type + " without a valid id");
    }
    if (type == "load") {
        Load r;
        r.id = *id;
        r.vision = optionalPath(j, "vision");
        r.text = optionalPath(j, "text");
        r.tokenizer = optionalPath(j, "tokenizer");
        r.faces = optionalPath(j, "faces");
        r.objects = optionalPath(j, "objects");
        r.speech = optionalPath(j, "speech");
        r.stems = optionalPath(j, "stems");
        r.translation = optionalPath(j, "translation");
        r.segmentation = optionalPath(j, "segmentation");
        r.device = jsonStringOr(j, "device", "auto");
        r.threads = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "threads", 0), 0, 256));
        if (r.device != "auto" && r.device != "gpu" && r.device != "cpu") {
            return bad("unknown device \"" + r.device + "\"");
        }
        if (const Json* pictures = jsonArray(j, "pictures")) {
            if (pictures->size() > static_cast<std::size_t>(kMaxPictureModels)) {
                return bad("load names too many picture models");
            }
            for (const Json& m : *pictures) {
                PictureModelFile file;
                file.key = m.is_object() ? jsonStringOr(m, "key") : std::string{};
                file.path = m.is_object() ? optionalPath(m, "path") : std::filesystem::path{};
                if (file.key.empty() || file.key.size() > 64 || file.path.empty()) {
                    return bad("a picture model needs a key and a path");
                }
                r.pictures.push_back(std::move(file));
            }
        }
        if (r.vision.empty() && r.text.empty() && r.faces.empty() && r.objects.empty() && r.speech.empty() && r.stems.empty() &&
            r.translation.empty() && r.segmentation.empty() && r.pictures.empty()) {
            return bad("load names no model");
        }
        if (!r.text.empty() && r.tokenizer.empty()) {
            return bad("a text model needs its tokenizer");
        }
        return r;
    }
    if (type == "embed_images") {
        const Json* images = jsonObject(j, "images");
        if (!images) {
            return bad("embed_images without images");
        }
        EmbedImages r;
        r.id = *id;
        r.images.path = optionalPath(*images, "path");
        r.images.width = static_cast<int>(jsonIntOr(*images, "width", 0));
        r.images.height = static_cast<int>(jsonIntOr(*images, "height", 0));
        r.images.count = static_cast<int>(jsonIntOr(*images, "count", 0));
        if (r.images.path.empty() || !imageBounds(r.images.width, r.images.height, r.images.count)) {
            return bad("embed_images out of bounds");
        }
        return r;
    }
    if (type == "detect") {
        const Json* images = jsonObject(j, "images");
        if (!images) {
            return bad("detect without images");
        }
        Detect r;
        r.id = *id;
        r.images.path = optionalPath(*images, "path");
        r.images.width = static_cast<int>(jsonIntOr(*images, "width", 0));
        r.images.height = static_cast<int>(jsonIntOr(*images, "height", 0));
        r.images.count = static_cast<int>(jsonIntOr(*images, "count", 0));
        r.faces = jsonBoolOr(j, "faces", true);
        r.objects = jsonBoolOr(j, "objects", true);
        r.faceThreshold = static_cast<float>(std::clamp(jsonNumberOr(j, "faceThreshold", 0.6), 0.0, 1.0));
        r.objectThreshold = static_cast<float>(std::clamp(jsonNumberOr(j, "objectThreshold", 0.35), 0.0, 1.0));
        r.allClasses = jsonBoolOr(j, "allClasses", false);
        if (r.images.path.empty() || !imageBounds(r.images.width, r.images.height, r.images.count)) {
            return bad("detect out of bounds");
        }
        return r;
    }
    if (type == "segment_frame") {
        const Json* images = jsonObject(j, "images");
        if (!images) {
            return bad("segment_frame without images");
        }
        SegmentFrame r;
        r.id = *id;
        r.mode = jsonStringOr(j, "mode", "prompt");
        if (r.mode != "prompt" && r.mode != "track") {
            return bad("unknown segment_frame mode \"" + r.mode + "\"");
        }
        r.images.path = optionalPath(*images, "path");
        r.images.width = static_cast<int>(jsonIntOr(*images, "width", 0));
        r.images.height = static_cast<int>(jsonIntOr(*images, "height", 0));
        r.images.count = static_cast<int>(jsonIntOr(*images, "count", 0));
        r.frame = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "frame", 0), 0, 1 << 24));
        r.frames = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "frames", 1), 1, 1 << 24));
        r.output = optionalPath(j, "output");
        r.width = static_cast<int>(jsonIntOr(j, "width", 0));
        r.height = static_cast<int>(jsonIntOr(j, "height", 0));
        if (r.images.path.empty() || r.images.count != 1 || !imageBounds(r.images.width, r.images.height, 1) || r.output.empty() ||
            r.width <= 0 || r.height <= 0 || r.width > kMaxMatteSide || r.height > kMaxMatteSide || r.frame >= r.frames) {
            return bad("segment_frame out of bounds");
        }
        const Json* points = jsonArray(j, "points");
        if (points && points->size() > static_cast<std::size_t>(kMaxSegmentPoints)) {
            return bad("segment_frame has too many points");
        }
        if (points) {
            for (const Json& p : *points) {
                if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number_integer()) {
                    return bad("a segment point is [x, y, label]");
                }
                const SegmentPoint point{p[0].get<float>(), p[1].get<float>(), p[2].get<int>()};
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.label < 0 || point.label > 3) {
                    return bad("a segment point is out of bounds");
                }
                r.points.push_back(point);
            }
        }
        if (r.mode == "prompt" && r.points.empty()) {
            return bad("a segment prompt needs points or a box");
        }
        return r;
    }
    if (type == "process_picture") {
        const Json* input = jsonObject(j, "input");
        if (!input) {
            return bad("process_picture without an input");
        }
        ProcessPicture r;
        r.id = *id;
        r.model = jsonStringOr(j, "model");
        r.input.path = optionalPath(*input, "path");
        r.input.width = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(*input, "width", 0), 0, 1 << 30));
        r.input.height = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(*input, "height", 0), 0, 1 << 30));
        r.output = optionalPath(j, "output");
        r.scale = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "scale", 1), 0, 64));
        r.tile = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "tile", 256), 0, 1 << 20));
        r.pad = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "pad", 16), -1, 1 << 20));
        r.sigma = std::clamp(jsonNumberOr(j, "sigma", 0.0), -1.0, 2.0);
        if (const Json* second = jsonObject(j, "input2")) { // 0.7: the later frame of a frame interpolation
            r.input2.path = optionalPath(*second, "path");
            r.input2.width = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(*second, "width", 0), 0, 1 << 30));
            r.input2.height = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(*second, "height", 0), 0, 1 << 30));
            r.timestep = jsonNumberOr(j, "timestep", 0.5);
            if (r.input2.path.empty() || r.input2.width != r.input.width || r.input2.height != r.input.height ||
                !(r.timestep >= 0.0 && r.timestep <= 1.0)) {
                return bad("process_picture out of bounds");
            }
        }
        if (r.model.empty() || r.model.size() > 64 || r.input.path.empty() || r.output.empty() || !pictureBounds(r.input.width, r.input.height) ||
            (r.scale != 1 && r.scale != 2 && r.scale != 4) || r.tile < kMinTile || r.tile > kMaxTile || r.tile % 8 != 0 || r.pad < 0 ||
            r.pad > kMaxTilePad || r.pad % 8 != 0 || r.sigma < 0.0 ||
            !pictureBounds(r.input.width * r.scale, r.input.height * r.scale)) {
            return bad("process_picture out of bounds");
        }
        return r;
    }
    if (type == "enhance_audio" || type == "separate_audio") {
        auto audio = audioBlock(j);
        const auto output = optionalPath(j, "output");
        if (!audio || output.empty()) {
            return bad(type + " out of bounds");
        }
        if (type == "enhance_audio") {
            return EnhanceAudio{*id, std::move(*audio), output};
        }
        return SeparateAudio{*id, std::move(*audio), output};
    }
    if (type == "embed_text" || type == "translate_text") {
        const Json* texts = jsonArray(j, "texts");
        if (!texts || texts->empty() || texts->size() > static_cast<std::size_t>(kMaxTextsPerRequest)) {
            return bad(type + " needs 1 to " + std::to_string(kMaxTextsPerRequest) + " texts");
        }
        std::vector<std::string> list;
        for (const Json& t : *texts) {
            auto s = asString(&t);
            if (!s || s->size() > kMaxTextBytes) {
                return bad("a text is not a string of at most " + std::to_string(kMaxTextBytes) + " bytes");
            }
            list.push_back(std::move(*s));
        }
        if (type == "translate_text") {
            TranslateText r;
            r.id = *id;
            r.texts = std::move(list);
            r.target = jsonStringOr(j, "target");
            r.maxTokens = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "maxTokens", 256), 1, kMaxTranslationTokens));
            if (r.target.size() > 32) {
                return bad("translate_text target token too long");
            }
            return r;
        }
        EmbedText r;
        r.id = *id;
        r.texts = std::move(list);
        return r;
    }
    return bad("unknown request type \"" + type + "\"");
}

std::expected<Response, Error> decodeResponse(std::string_view line) {
    auto parsed = parseLine(line);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const Json& j = *parsed;
    const std::string type = *jsonString(j, "type");
    if (type == "hello") {
        HelloReply r;
        const auto protocol = jsonInt(j, "protocol");
        if (!protocol) {
            return bad("hello without a protocol");
        }
        r.protocol = static_cast<int>(*protocol);
        r.engine = jsonStringOr(j, "engine");
        r.engineVersion = jsonStringOr(j, "engineVersion");
        if (const Json* providers = jsonArray(j, "providers")) {
            for (const Json& p : *providers) {
                if (auto s = asString(&p)) {
                    r.providers.push_back(std::move(*s));
                }
            }
        }
        return r;
    }
    if (type == "error") {
        return Failure{jsonStringOr(j, "id"), jsonStringOr(j, "code", "protocol"), jsonStringOr(j, "message")};
    }
    const auto id = requiredId(j);
    if (!id) {
        return bad(type + " without a valid id");
    }
    if (type == "loaded") {
        const auto dimension = jsonInt(j, "dimension");
        if (!dimension || *dimension < 0 || *dimension > 65536) {
            return bad("loaded without a valid dimension");
        }
        return Loaded{*id, jsonStringOr(j, "device"), static_cast<int>(*dimension)};
    }
    if (type == "translations") {
        const Json* texts = jsonArray(j, "texts");
        if (!texts || texts->size() > static_cast<std::size_t>(kMaxTextsPerRequest)) {
            return bad("translations without texts");
        }
        Translations r;
        r.id = *id;
        for (const Json& t : *texts) {
            auto s = asString(&t);
            if (!s || s->size() > kMaxTextBytes * 4) {
                return bad("a translation is not a string");
            }
            r.texts.push_back(std::move(*s));
        }
        r.tokens = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "tokens", 0), 0, 1 << 24));
        r.milliseconds = jsonNumberOr(j, "ms", 0.0);
        r.device = jsonStringOr(j, "device");
        return r;
    }
    if (type == "segmented") {
        Segmented r;
        r.id = *id;
        r.iou = static_cast<float>(std::clamp(jsonNumberOr(j, "iou", 0.0), -1e6, 1e6));
        r.objectScore = static_cast<float>(std::clamp(jsonNumberOr(j, "objectScore", 0.0), -1e6, 1e6));
        r.present = jsonBoolOr(j, "present", false);
        r.width = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "width", 0), 0, kMaxMatteSide));
        r.height = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "height", 0), 0, kMaxMatteSide));
        r.milliseconds = jsonNumberOr(j, "ms", 0.0);
        r.device = jsonStringOr(j, "device");
        if (r.width <= 0 || r.height <= 0) {
            return bad("segmented without a matte size");
        }
        return r;
    }
    if (type == "picture") {
        PictureDone r;
        r.id = *id;
        r.width = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "width", 0), 0, kMaxPictureSide));
        r.height = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "height", 0), 0, kMaxPictureSide));
        r.tiles = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "tiles", 0), 0, 1 << 24));
        r.milliseconds = jsonNumberOr(j, "ms", 0.0);
        r.device = jsonStringOr(j, "device");
        if (!pictureBounds(r.width, r.height)) {
            return bad("picture without a size");
        }
        return r;
    }
    if (type == "audio") {
        AudioDone r;
        r.id = *id;
        r.frames = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "frames", 0), 0, 1 << 30));
        r.channels = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "channels", 0), 0, 1 << 20));
        r.outputs = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "outputs", 0), 0, 1 << 20));
        r.milliseconds = jsonNumberOr(j, "ms", 0.0);
        r.device = jsonStringOr(j, "device");
        if (r.frames <= 0 || r.frames > kMaxAudioFrames || r.channels <= 0 || r.channels > kMaxAudioChannels || r.outputs <= 0 ||
            r.outputs > kMaxAudioOutputs) {
            return bad("audio out of bounds");
        }
        return r;
    }
    if (type == "detections") {
        const Json* frames = jsonArray(j, "frames");
        if (!frames || frames->size() > static_cast<std::size_t>(kMaxImagesPerRequest)) {
            return bad("detections without frames");
        }
        Detections r;
        r.id = *id;
        r.milliseconds = jsonNumberOr(j, "ms", 0.0);
        const auto rows = [](const Json* list, std::size_t width, std::vector<std::vector<float>>& out) {
            if (!list) {
                return true;
            }
            if (!list->is_array() || list->size() > kMaxDetectionsPerFrame) {
                return false;
            }
            for (const Json& row : *list) {
                if (!row.is_array() || row.size() != width) {
                    return false;
                }
                std::vector<float> values;
                for (const Json& v : row) {
                    const auto x = asNumber(&v);
                    if (!x) {
                        return false;
                    }
                    values.push_back(static_cast<float>(*x));
                }
                out.push_back(std::move(values));
            }
            return true;
        };
        for (const Json& f : *frames) {
            DetectedFrame frame;
            if (!f.is_object() || !rows(jsonArray(f, "faces"), kFaceRow, frame.faces) ||
                !rows(jsonArray(f, "objects"), kObjectRow, frame.objects)) {
                return bad("a detection row is malformed");
            }
            r.frames.push_back(std::move(frame));
        }
        return r;
    }
    if (type == "embeddings") {
        const Json* vectors = jsonArray(j, "vectors");
        if (!vectors || vectors->size() > static_cast<std::size_t>(kMaxImagesPerRequest)) {
            return bad("embeddings without vectors");
        }
        Embeddings r;
        r.id = *id;
        r.milliseconds = jsonNumberOr(j, "ms", 0.0);
        for (const Json& v : *vectors) {
            const auto text = asString(&v);
            auto vector = text ? decodeVector(*text) : std::nullopt;
            if (!vector || (!r.vectors.empty() && vector->size() != r.vectors.front().size())) {
                return bad("an embedding is not a base64 float32 vector of the batch's dimension");
            }
            r.vectors.push_back(std::move(*vector));
        }
        return r;
    }
    return bad("unknown response type \"" + type + "\"");
}

const std::string& responseId(const Response& response) {
    static const std::string none;
    return std::visit(
        [](const auto& r) -> const std::string& {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, HelloReply>) {
                return none;
            } else {
                return r.id;
            }
        },
        response);
}

// --- Picture batches ----------------------------------------------------------------------------------------------

std::expected<ImageBatch, Error> writeImageBatch(const std::filesystem::path& path, std::span<const Image> images) {
    if (images.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no pictures"));
    }
    const int width = images.front().width;
    const int height = images.front().height;
    if (!imageBounds(width, height, static_cast<int>(images.size()))) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "picture batch out of bounds"));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(path)));
    }
    for (const Image& image : images) {
        if (!image.valid() || image.width != width || image.height != height) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the pictures of a batch must be valid and of one size"));
        }
        out.write(reinterpret_cast<const char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
    }
    out.close();
    if (!out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(path)));
    }
    return ImageBatch{path, width, height, static_cast<int>(images.size())};
}

std::expected<std::vector<Image>, Error> readImageBatch(const ImageBatch& batch) {
    if (!imageBounds(batch.width, batch.height, batch.count)) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "picture batch out of bounds"));
    }
    std::ifstream in(batch.path, std::ios::binary);
    if (!in) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot read " + pathToUtf8(batch.path)));
    }
    std::vector<Image> images(static_cast<std::size_t>(batch.count));
    const std::size_t bytes = static_cast<std::size_t>(batch.width) * static_cast<std::size_t>(batch.height) * 3;
    for (Image& image : images) {
        image.width = batch.width;
        image.height = batch.height;
        image.rgb.resize(bytes);
        in.read(reinterpret_cast<char*>(image.rgb.data()), static_cast<std::streamsize>(bytes));
        if (static_cast<std::size_t>(in.gcount()) != bytes) {
            return std::unexpected(Error(ErrorCode::IoError, "the picture batch is shorter than announced"));
        }
    }
    return images;
}

// --- Pictures of the AI video effects -----------------------------------------------------------------------------

std::expected<PictureFile, Error> writePictureFile(const std::filesystem::path& path, int width, int height,
                                                   std::span<const std::uint16_t> rgb) {
    if (!pictureBounds(width, height) || rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "picture out of bounds"));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(path)));
    }
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size_bytes()));
    out.close();
    if (!out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(path)));
    }
    return PictureFile{path, width, height};
}

std::expected<std::vector<std::uint16_t>, Error> readPictureFile(const PictureFile& file) {
    if (!pictureBounds(file.width, file.height)) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "picture out of bounds"));
    }
    std::ifstream in(file.path, std::ios::binary);
    if (!in) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot read " + pathToUtf8(file.path)));
    }
    std::vector<std::uint16_t> rgb(static_cast<std::size_t>(file.width) * static_cast<std::size_t>(file.height) * 3);
    in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size() * sizeof(std::uint16_t)));
    if (static_cast<std::size_t>(in.gcount()) != rgb.size() * sizeof(std::uint16_t)) {
        return std::unexpected(Error(ErrorCode::IoError, "the picture file is shorter than announced"));
    }
    return rgb;
}

// --- Audio blocks -------------------------------------------------------------------------------------------------

std::expected<AudioBlock, Error> writeAudioBlock(const std::filesystem::path& path, std::span<const float> samples,
                                                 int channels, int rate) {
    if (channels <= 0 || samples.size() % static_cast<std::size_t>(channels) != 0 ||
        samples.size() / static_cast<std::size_t>(channels) > static_cast<std::size_t>(kMaxAudioFrames) ||
        !audioBounds(channels, static_cast<int>(samples.size() / static_cast<std::size_t>(channels)), rate)) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "audio block out of bounds"));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(path)));
    }
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size_bytes()));
    out.close();
    if (!out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(path)));
    }
    return AudioBlock{path, channels, static_cast<int>(samples.size() / static_cast<std::size_t>(channels)), rate};
}

std::expected<std::vector<float>, Error> readAudioBlock(const AudioBlock& block, int blocks) {
    if (!audioBounds(block.channels, block.frames, block.rate) || blocks <= 0 || blocks > kMaxAudioOutputs) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "audio block out of bounds"));
    }
    std::ifstream in(block.path, std::ios::binary);
    if (!in) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot read " + pathToUtf8(block.path)));
    }
    std::vector<float> samples(static_cast<std::size_t>(block.frames) * static_cast<std::size_t>(block.channels) *
                               static_cast<std::size_t>(blocks));
    in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(float)));
    if (static_cast<std::size_t>(in.gcount()) != samples.size() * sizeof(float)) {
        return std::unexpected(Error(ErrorCode::IoError, "the audio block is shorter than announced"));
    }
    return samples;
}

} // namespace wabcut::ai::search::embed
