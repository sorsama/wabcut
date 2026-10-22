// The stabilizer's analysis as JSON (C.15): the cache payload. Reading treats the text as untrusted: its size, nesting
// and element count are bounded before nlohmann parses it (so a hostile file cannot exhaust memory), every field is
// type-checked before it is read, and numbers must be finite and in range.

#include <wabcut/ai/stabilize/Stabilize.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace wabcut::ai::stabilize {

using nlohmann::json;

namespace {

constexpr std::int64_t kMaxPictureSide = 16384;
constexpr std::size_t kMaxJsonBytes = std::size_t{128} << 20; // a full-length analysis is ~55 MB compact
constexpr int kMaxDepth = 8;
constexpr std::size_t kMaxElements = (kMaxAnalysisFrames + 16) * 24; // brackets and separators: 18 per frame

std::unexpected<Error> parseError(std::string message) {
    return std::unexpected(Error{ErrorCode::ParseError, "stabilize analysis: " + std::move(message)});
}

std::unexpected<Error> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, "stabilize analysis: " + std::move(message)});
}

std::string methodName(Method method) {
    switch (method) {
    case Method::Position:
        return "position";
    case Method::Similarity:
        return "similarity";
    case Method::Perspective:
        return "perspective";
    }
    return "similarity";
}

std::optional<Method> methodFromName(const std::string& name) {
    for (const Method m : {Method::Position, Method::Similarity, Method::Perspective}) {
        if (methodName(m) == name) {
            return m;
        }
    }
    return std::nullopt;
}

/// Whether a JSON text's nesting depth and its count of brackets and separators are within the limits, without
/// building it (strings skipped).
bool withinLimits(std::string_view text) {
    int depth = 0;
    std::size_t separators = 0;
    bool inString = false;
    bool escaped = false;
    for (const char ch : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        switch (ch) {
        case '"':
            inString = true;
            break;
        case '[':
        case '{':
            if (++depth > kMaxDepth) {
                return false;
            }
            ++separators;
            break;
        case ']':
        case '}':
            --depth;
            break;
        case ',':
        case ':':
            ++separators;
            break;
        default:
            break;
        }
        if (separators > kMaxElements) {
            return false;
        }
    }
    return true;
}

const json* member(const json& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

std::optional<std::int64_t> integerOf(const json* j) {
    if (!j) {
        return std::nullopt;
    }
    if (j->is_number_unsigned()) {
        const auto u = j->get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(u);
    }
    if (j->is_number_integer()) {
        return j->get<std::int64_t>();
    }
    return std::nullopt;
}

std::optional<double> numberOf(const json* j) {
    if (!j || !j->is_number()) {
        return std::nullopt;
    }
    return j->get<double>();
}

} // namespace

std::string toJson(const MotionAnalysis& analysis) {
    json frames = json::array();
    for (const FrameMotion& f : analysis.frames) {
        json m = json::array();
        for (const double v : f.motion.m) {
            m.push_back(v);
        }
        frames.push_back(json{{"t", f.time.value}, {"m", std::move(m)}, {"c", f.confidence}, {"lost", f.lost}});
    }
    return json{{"schema", std::string(MotionAnalysis::kAnalyzer)},
                {"version", MotionAnalysis::kVersion},
                {"width", analysis.width},
                {"height", analysis.height},
                {"method", methodName(analysis.method)},
                {"frames", std::move(frames)}}
        .dump();
}

std::expected<MotionAnalysis, Error> motionAnalysisFromJson(std::string_view text) {
    if (text.size() > kMaxJsonBytes) {
        return invalid("larger than " + std::to_string(kMaxJsonBytes >> 20) + " MB");
    }
    if (!withinLimits(text)) {
        return invalid("nested too deeply or too many elements");
    }
    try {
        const json doc = json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) {
            return parseError("not a JSON object");
        }
        const json* schema = member(doc, "schema");
        if (!schema || !schema->is_string() || schema->get<std::string>() != MotionAnalysis::kAnalyzer) {
            return parseError("not a " + std::string(MotionAnalysis::kAnalyzer) + " document");
        }
        const auto version = integerOf(member(doc, "version"));
        if (!version) {
            return parseError("no version");
        }
        if (*version != MotionAnalysis::kVersion) {
            return parseError("unsupported version " + std::to_string(*version));
        }
        MotionAnalysis out;
        const auto width = integerOf(member(doc, "width"));
        const auto height = integerOf(member(doc, "height"));
        if (!width || !height) {
            return parseError("width and height must be integers");
        }
        if (*width < 1 || *width > kMaxPictureSide || *height < 1 || *height > kMaxPictureSide) {
            return invalid("picture size out of range");
        }
        out.width = static_cast<int>(*width);
        out.height = static_cast<int>(*height);
        const json* method = member(doc, "method");
        if (!method || !method->is_string()) {
            return parseError("method must be a string");
        }
        const auto parsedMethod = methodFromName(method->get<std::string>());
        if (!parsedMethod) {
            return parseError("unknown method");
        }
        out.method = *parsedMethod;
        const json* frames = member(doc, "frames");
        if (!frames || !frames->is_array()) {
            return parseError("frames must be an array");
        }
        if (frames->size() > kMaxAnalysisFrames) {
            return invalid("more than " + std::to_string(kMaxAnalysisFrames) + " frames");
        }
        out.frames.reserve(frames->size());
        for (std::size_t i = 0; i < frames->size(); ++i) {
            const json& f = (*frames)[i];
            const std::string where = "frame " + std::to_string(i) + ": ";
            if (!f.is_object()) {
                return parseError(where + "not an object");
            }
            FrameMotion frame;
            const auto t = integerOf(member(f, "t"));
            if (!t) {
                return parseError(where + "t must be an integer");
            }
            frame.time = Ticks{*t};
            if (!out.frames.empty() && frame.time <= out.frames.back().time) {
                return invalid(where + "times must increase");
            }
            const json* m = member(f, "m");
            if (!m || !m->is_array() || m->size() != 9) {
                return parseError(where + "m must be 9 numbers");
            }
            for (std::size_t k = 0; k < 9; ++k) {
                const auto v = numberOf(&(*m)[k]);
                if (!v) {
                    return parseError(where + "m must be 9 numbers");
                }
                if (!std::isfinite(*v)) {
                    return invalid(where + "m is not finite");
                }
                frame.motion.m[k] = *v;
            }
            if (frame.motion.m[8] != 1.0) {
                const double scale = frame.motion.m[8];
                if (!(std::abs(scale) > 1e-12)) {
                    return invalid(where + "m is not a homography");
                }
                for (double& v : frame.motion.m) {
                    v /= scale;
                }
                frame.motion.m[8] = 1.0;
                for (const double v : frame.motion.m) {
                    if (!std::isfinite(v)) {
                        return invalid(where + "m is not finite");
                    }
                }
            }
            const auto confidence = numberOf(member(f, "c"));
            if (!confidence) {
                return parseError(where + "c must be a number");
            }
            if (!(*confidence >= 0.0 && *confidence <= 1.0)) {
                return invalid(where + "c must be within 0..1");
            }
            frame.confidence = *confidence;
            const json* lost = member(f, "lost");
            if (!lost || !lost->is_boolean()) {
                return parseError(where + "lost must be true or false");
            }
            frame.lost = lost->get<bool>();
            out.frames.push_back(frame);
        }
        return out;
    } catch (const std::exception& e) {
        return parseError(e.what());
    }
}

} // namespace wabcut::ai::stabilize
