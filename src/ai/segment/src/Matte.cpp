#include <wabcut/ai/segment/Matte.h>

#include <wabcut/ai/AnalysisCache.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

namespace wabcut::ai::segment {

using Json = nlohmann::json;

namespace {

std::unexpected<Error> bad(std::string message) { return std::unexpected(Error(ErrorCode::ParseError, "matte: " + std::move(message))); }

/// 1-D squared Euclidean distance transform (Felzenszwalb & Huttenlocher) of `f` (n samples spaced `s` apart):
/// d[q] = min_p ((q - p) s)^2 + f[p].
constexpr double kFar = 1.0e20; ///< "no sample here" (finite: the envelope arithmetic stays exact)

void edt1d(const double* f, int n, double s, double* d, int* v, double* z) {
    const double inf = std::numeric_limits<double>::infinity();
    const double s2 = s * s;
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; ++q) {
        double sx = ((f[q] + s2 * q * q) - (f[v[k]] + s2 * v[k] * v[k])) / (2.0 * s2 * (q - v[k]));
        while (sx <= z[k]) {
            --k;
            sx = ((f[q] + s2 * q * q) - (f[v[k]] + s2 * v[k] * v[k])) / (2.0 * s2 * (q - v[k]));
        }
        ++k;
        v[k] = q;
        z[k] = sx;
        z[k + 1] = inf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) {
            ++k;
        }
        const double dq = (q - v[k]) * s;
        d[q] = std::min(kFar, dq * dq + f[v[k]]);
    }
}

/// Squared distance (frame heights^2) from every texel to the nearest texel where `set` holds.
std::vector<double> squaredDistanceTo(const std::vector<bool>& set, int w, int h, double sx, double sy) {
    std::vector<double> grid(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < grid.size(); ++i) {
        grid[i] = set[i] ? 0.0 : kFar;
    }
    const int n = std::max(w, h);
    std::vector<double> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
    std::vector<int> v(static_cast<std::size_t>(n));
    for (int x = 0; x < w; ++x) { // columns
        for (int y = 0; y < h; ++y) {
            f[static_cast<std::size_t>(y)] = grid[static_cast<std::size_t>(y) * w + x];
        }
        edt1d(f.data(), h, sy, d.data(), v.data(), z.data());
        for (int y = 0; y < h; ++y) {
            grid[static_cast<std::size_t>(y) * w + x] = d[static_cast<std::size_t>(y)];
        }
    }
    for (int y = 0; y < h; ++y) { // rows
        edt1d(&grid[static_cast<std::size_t>(y) * w], w, sx, d.data(), v.data(), z.data());
        std::copy(d.begin(), d.begin() + w, grid.begin() + static_cast<std::ptrdiff_t>(y) * w);
    }
    return grid;
}

std::string hex16(std::uint64_t v) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

Json boxJson(const MatteBox& b) { return Json::array({b.x, b.y, b.width, b.height}); }

std::optional<MatteBox> boxFromJson(const Json& j) {
    if (!j.is_array() || j.size() != 4) {
        return std::nullopt;
    }
    for (const Json& v : j) {
        if (!v.is_number() || !std::isfinite(v.get<double>())) {
            return std::nullopt;
        }
    }
    return MatteBox{j[0].get<double>(), j[1].get<double>(), j[2].get<double>(), j[3].get<double>()};
}

Json promptJson(const Prompt& p) {
    Json points = Json::array();
    for (const PromptPoint& q : p.points) {
        points.push_back(Json::array({q.x, q.y, q.positive ? 1 : 0}));
    }
    Json j{{"t", p.time.value}, {"points", std::move(points)}};
    if (p.box) {
        j["box"] = boxJson(*p.box);
    }
    return j;
}

} // namespace

// ---- Codec --------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> encodeRle(const MatteImage& image) {
    std::vector<std::uint8_t> out;
    const std::size_t n = image.alpha.size();
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t value = image.alpha[i];
        std::size_t run = 1;
        while (i + run < n && image.alpha[i + run] == value) {
            ++run;
        }
        out.push_back(value);
        std::size_t r = run;
        do {
            std::uint8_t byte = static_cast<std::uint8_t>(r & 0x7f);
            r >>= 7;
            if (r != 0) {
                byte |= 0x80;
            }
            out.push_back(byte);
        } while (r != 0);
        i += run;
    }
    return out;
}

std::expected<MatteImage, Error> decodeRle(std::span<const std::uint8_t> rle, int width, int height) {
    if (width <= 0 || height <= 0 || width > kMaxMatteSide || height > kMaxMatteSide) {
        return bad("a matte size out of bounds");
    }
    MatteImage out;
    out.width = width;
    out.height = height;
    const std::size_t total = static_cast<std::size_t>(width) * height;
    out.alpha.reserve(total);
    std::size_t i = 0;
    while (i < rle.size()) {
        const std::uint8_t value = rle[i++];
        std::size_t run = 0;
        int shift = 0;
        while (true) {
            if (i >= rle.size() || shift > 28) {
                return bad("a truncated run");
            }
            const std::uint8_t byte = rle[i++];
            run |= static_cast<std::size_t>(byte & 0x7f) << shift;
            shift += 7;
            if (!(byte & 0x80)) {
                break;
            }
        }
        if (run == 0 || out.alpha.size() + run > total) {
            return bad("runs longer than the matte");
        }
        out.alpha.insert(out.alpha.end(), run, value);
    }
    if (out.alpha.size() != total) {
        return bad("runs shorter than the matte");
    }
    return out;
}

std::string toBase64(std::span<const std::uint8_t> bytes) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(kTable[(triple >> 18) & 63]);
        out.push_back(kTable[(triple >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kTable[(triple >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kTable[triple & 63] : '=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> fromBase64(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=' && i + 4 == text.size() && k >= 2) {
                v[k] = 0;
                ++pad;
            } else {
                v[k] = value(c);
                if (v[k] < 0 || pad > 0) {
                    return std::nullopt;
                }
            }
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(v[0]) << 18) | (static_cast<std::uint32_t>(v[1]) << 12) |
                                     (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<std::uint8_t>(triple >> 16));
        if (pad < 2) {
            out.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xff));
        }
        if (pad < 1) {
            out.push_back(static_cast<std::uint8_t>(triple & 0xff));
        }
    }
    return out;
}

std::optional<MatteBox> boxOf(const MatteImage& image, std::uint8_t threshold) {
    if (!image.valid()) {
        return std::nullopt;
    }
    int x0 = image.width, y0 = image.height, x1 = -1, y1 = -1;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (image.alpha[static_cast<std::size_t>(y) * image.width + x] >= threshold) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < 0) {
        return std::nullopt;
    }
    return MatteBox{static_cast<double>(x0) / image.width, static_cast<double>(y0) / image.height,
                    static_cast<double>(x1 + 1 - x0) / image.width, static_cast<double>(y1 + 1 - y0) / image.height};
}

double iouOf(const MatteImage& a, const MatteImage& b) {
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) {
        return 0.0;
    }
    std::size_t both = 0, either = 0;
    for (std::size_t i = 0; i < a.alpha.size(); ++i) {
        const bool x = a.alpha[i] >= 128, y = b.alpha[i] >= 128;
        both += x && y ? 1 : 0;
        either += x || y ? 1 : 0;
    }
    return either == 0 ? 1.0 : static_cast<double>(both) / static_cast<double>(either);
}

MatteImage resized(const MatteImage& image, int width, int height) {
    MatteImage out;
    if (!image.valid() || width <= 0 || height <= 0) {
        return out;
    }
    out.width = width;
    out.height = height;
    out.alpha.resize(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const double sy = std::clamp((y + 0.5) * image.height / height - 0.5, 0.0, image.height - 1.0);
        const int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, image.height - 1);
        const double ty = sy - y0;
        for (int x = 0; x < width; ++x) {
            const double sx = std::clamp((x + 0.5) * image.width / width - 0.5, 0.0, image.width - 1.0);
            const int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, image.width - 1);
            const double tx = sx - x0;
            const auto at = [&](int xx, int yy) { return static_cast<double>(image.alpha[static_cast<std::size_t>(yy) * image.width + xx]); };
            const double v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
            out.alpha[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
        }
    }
    return out;
}

std::pair<int, int> matteSize(double aspect, int longSide) {
    aspect = aspect > 0.0 && std::isfinite(aspect) ? aspect : 16.0 / 9.0;
    longSide = std::clamp(longSide, 16, kMaxMatteSide);
    if (aspect >= 1.0) {
        return {longSide, std::max(1, static_cast<int>(std::lround(longSide / aspect)))};
    }
    return {std::max(1, static_cast<int>(std::lround(longSide * aspect))), longSide};
}

std::vector<float> signedDistance(const MatteImage& image, double aspect) {
    std::vector<float> out;
    if (!image.valid()) {
        return out;
    }
    aspect = aspect > 0.0 && std::isfinite(aspect) ? aspect : 16.0 / 9.0;
    const int w = image.width, h = image.height;
    const double sx = aspect / w, sy = 1.0 / h; // a texel in frame heights
    std::vector<bool> inside(image.alpha.size());
    bool any = false, all = true;
    for (std::size_t i = 0; i < inside.size(); ++i) {
        inside[i] = image.alpha[i] >= 128;
        any = any || inside[i];
        all = all && inside[i];
    }
    out.assign(inside.size(), any ? 1.0e4f : -1.0e4f);
    if (!any || all) {
        return out;
    }
    std::vector<bool> outside(inside.size());
    for (std::size_t i = 0; i < inside.size(); ++i) {
        outside[i] = !inside[i];
    }
    const std::vector<double> toInside = squaredDistanceTo(inside, w, h, sx, sy);
    const std::vector<double> toOutside = squaredDistanceTo(outside, w, h, sx, sy);
    const double half = 0.5 * std::min(sx, sy); // the edge lies between texel centres
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(inside[i] ? std::sqrt(toOutside[i]) - half : -(std::sqrt(toInside[i]) - half));
    }
    return out;
}

// ---- Tracks -------------------------------------------------------------------------------------------------------

std::string MatteTrack::identity() const { return hex16(fingerprint) + "/" + std::to_string(stream) + "/" + model + "/" + params; }

const MatteFrame* MatteTrack::frameAt(Ticks time, Ticks tolerance) const {
    const auto it = std::lower_bound(frames.begin(), frames.end(), time, [](const MatteFrame& f, Ticks t) { return f.time < t; });
    const MatteFrame* best = nullptr;
    Ticks bestGap{std::numeric_limits<std::int64_t>::max()};
    for (auto c = it == frames.begin() ? it : it - 1; c != frames.end() && c <= it; ++c) {
        const Ticks gap{std::abs((c->time - time).value)};
        if (gap <= tolerance && gap < bestGap) {
            best = &*c;
            bestGap = gap;
        }
    }
    return best;
}

std::expected<MatteImage, Error> MatteTrack::imageOf(const MatteFrame& frame) const { return decodeRle(frame.rle, width, height); }

TimeRange MatteTrack::span() const {
    if (frames.empty()) {
        return {};
    }
    return TimeRange::fromStartEnd(frames.front().time, frames.back().time + frameDuration);
}

void MatteTrack::merge(const MatteTrack& other) {
    std::vector<MatteFrame> mergedFrames;
    std::size_t a = 0, b = 0;
    while (a < frames.size() || b < other.frames.size()) {
        if (b >= other.frames.size() || (a < frames.size() && frames[a].time < other.frames[b].time)) {
            mergedFrames.push_back(std::move(frames[a++]));
        } else {
            if (a < frames.size() && frames[a].time == other.frames[b].time) {
                ++a; // replaced
            }
            mergedFrames.push_back(other.frames[b++]);
        }
    }
    frames = std::move(mergedFrames);
    for (const Prompt& p : other.prompts) {
        if (std::ranges::find(prompts, p) == prompts.end()) {
            prompts.push_back(p);
        }
    }
    for (const Ticks c : other.cuts) {
        if (std::ranges::find(cuts, c) == cuts.end()) {
            cuts.push_back(c);
        }
    }
    std::sort(cuts.begin(), cuts.end());
}

std::expected<MatteTrackId, Error> parseTrackId(std::string_view identity) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = identity.find('/', start);
        parts.push_back(identity.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    if (parts.size() != 4 || parts[0].size() != 16 || parts[2].empty() || parts[3].empty() || parts[1].empty() || parts[1].size() > 2) {
        return bad("not a matte track identity");
    }
    MatteTrackId id;
    try {
        id.fingerprint = std::stoull(std::string(parts[0]), nullptr, 16);
        id.stream = std::stoi(std::string(parts[1]));
    } catch (const std::exception&) {
        return bad("not a matte track identity");
    }
    id.model = std::string(parts[2]);
    id.params = std::string(parts[3]);
    for (const char c : id.model + id.params) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')) {
            return bad("not a matte track identity");
        }
    }
    return id;
}

std::string promptsHash(std::span<const Prompt> prompts, TimeRange span, int width, int height, std::string_view run) {
    Json list = Json::array();
    for (const Prompt& p : prompts) {
        list.push_back(promptJson(p));
    }
    return hashParams({{"prompts", list.dump()},
                       {"span", std::to_string(span.start.value) + "+" + std::to_string(span.duration.value)},
                       {"size", std::to_string(width) + "x" + std::to_string(height)},
                       {"run", std::string(run)},
                       {"format", "1"}});
}

Json trackToJson(const MatteTrack& t) {
    Json prompts = Json::array();
    for (const Prompt& p : t.prompts) {
        prompts.push_back(promptJson(p));
    }
    Json frames = Json::array();
    for (const MatteFrame& f : t.frames) {
        Json j{{"t", f.time.value}, {"c", f.confidence}, {"p", f.present}, {"m", toBase64(f.rle)}};
        if (f.box) {
            j["b"] = boxJson(*f.box);
        }
        frames.push_back(std::move(j));
    }
    Json cuts = Json::array();
    for (const Ticks c : t.cuts) {
        cuts.push_back(c.value);
    }
    return Json{{"format", "wabcut.matte-track"}, {"version", 1},      {"fingerprint", hex16(t.fingerprint)},
                {"stream", t.stream},             {"model", t.model},  {"params", t.params},
                {"width", t.width},               {"height", t.height}, {"aspect", t.aspect},
                {"frameDuration", t.frameDuration.value}, {"prompts", std::move(prompts)}, {"frames", std::move(frames)},
                {"cuts", std::move(cuts)}};
}

std::expected<MatteTrack, Error> trackFromJson(const Json& j) {
    if (!j.is_object() || j.value("format", std::string()) != "wabcut.matte-track" || j.value("version", 0) != 1) {
        return bad("not a matte track");
    }
    MatteTrack t;
    try {
        t.fingerprint = std::stoull(j.at("fingerprint").get<std::string>(), nullptr, 16);
        t.stream = j.at("stream").get<int>();
        t.model = j.at("model").get<std::string>();
        t.params = j.at("params").get<std::string>();
        t.width = j.at("width").get<int>();
        t.height = j.at("height").get<int>();
        t.aspect = j.at("aspect").get<double>();
        t.frameDuration = Ticks{j.at("frameDuration").get<std::int64_t>()};
        const Json& prompts = j.at("prompts");
        if (!prompts.is_array() || prompts.size() > kMaxPrompts) {
            return bad("too many prompts");
        }
        for (const Json& p : prompts) {
            Prompt prompt;
            prompt.time = Ticks{p.at("t").get<std::int64_t>()};
            const Json& points = p.at("points");
            if (!points.is_array() || points.size() > kMaxPromptPoints) {
                return bad("too many prompt points");
            }
            for (const Json& q : points) {
                if (!q.is_array() || q.size() != 3) {
                    return bad("a prompt point is [x, y, positive]");
                }
                prompt.points.push_back(PromptPoint{q[0].get<double>(), q[1].get<double>(), q[2].get<int>() != 0});
            }
            if (p.contains("box")) {
                prompt.box = boxFromJson(p["box"]);
                if (!prompt.box) {
                    return bad("a prompt box is [x, y, width, height]");
                }
            }
            t.prompts.push_back(std::move(prompt));
        }
        const Json& frames = j.at("frames");
        if (!frames.is_array() || frames.size() > kMaxMatteFrames) {
            return bad("too many frames");
        }
        for (const Json& f : frames) {
            MatteFrame frame;
            frame.time = Ticks{f.at("t").get<std::int64_t>()};
            frame.confidence = std::clamp(f.at("c").get<float>(), 0.0f, 1.0f);
            frame.present = f.at("p").get<bool>();
            auto rle = fromBase64(f.at("m").get<std::string>());
            if (!rle) {
                return bad("a frame's matte is not base64");
            }
            frame.rle = std::move(*rle);
            if (f.contains("b")) {
                frame.box = boxFromJson(f["b"]);
            }
            if (!t.frames.empty() && !(t.frames.back().time < frame.time)) {
                return bad("frames out of order");
            }
            t.frames.push_back(std::move(frame));
        }
        for (const Json& c : j.value("cuts", Json::array())) {
            t.cuts.push_back(Ticks{c.get<std::int64_t>()});
        }
    } catch (const Json::exception& e) {
        return bad(std::string("malformed: ") + e.what());
    } catch (const std::exception& e) {
        return bad(std::string("malformed: ") + e.what());
    }
    if (t.width <= 0 || t.height <= 0 || t.width > kMaxMatteSide || t.height > kMaxMatteSide || !(t.aspect > 0.0) || !std::isfinite(t.aspect) ||
        t.frameDuration.value <= 0 || t.stream < 0 || t.model.empty() || t.params.empty()) {
        return bad("out of bounds");
    }
    return t;
}

// ---- The store ----------------------------------------------------------------------------------------------------

MatteStore::MatteStore(std::filesystem::path cacheRoot) : m_root(std::move(cacheRoot)) {}

std::expected<void, Error> MatteStore::put(const MatteTrack& track) const {
    AnalysisCache cache(m_root);
    return cache.put(AnalysisKey{track.fingerprint, track.stream, std::string(kMatteAnalyzer), track.model, track.params},
                     trackToJson(track).dump());
}

std::expected<std::optional<MatteTrack>, Error> MatteStore::get(std::string_view identity) const {
    auto id = parseTrackId(identity);
    if (!id) {
        return std::unexpected(id.error());
    }
    AnalysisCache cache(m_root);
    auto payload = cache.get(AnalysisKey{id->fingerprint, id->stream, std::string(kMatteAnalyzer), id->model, id->params});
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!*payload) {
        return std::optional<MatteTrack>{};
    }
    const Json j = Json::parse(**payload, nullptr, false);
    auto track = trackFromJson(j);
    if (!track) {
        return std::unexpected(track.error());
    }
    return std::optional<MatteTrack>(std::move(*track));
}

} // namespace wabcut::ai::segment
